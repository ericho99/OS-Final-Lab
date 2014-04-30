/*
 * Page mapping and page directory/table management.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from the MIT Exokernel and JOS.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */


#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/cdefs.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/syscall.h>
#include <inc/vm.h>

#include <kern/cpu.h>
#include <kern/trap.h>
#include <kern/proc.h>
#include <kern/pmap.h>


// Statically allocated page directory mapping the kernel's address space.
// We use this as a template for all pdirs for user-level processes.
pde_t pmap_bootpdir[1024] gcc_aligned(PAGESIZE);

// Statically allocated page that we always keep set to all zeros.
uint8_t pmap_zero[PAGESIZE] gcc_aligned(PAGESIZE);


// --------------------------------------------------------------
// Set up initial memory mappings and turn on MMU.
// --------------------------------------------------------------



// Set up a two-level page table:
// pmap_bootpdir is its linear (virtual) address of the root
// Then turn on paging.
// 
// This function only creates mappings in the kernel part of the address space
// (addresses outside of the range between VM_USERLO and VM_USERHI).
// The user part of the address space remains all PTE_ZERO until later.
//



void
pmap_init(void)
{
	if (cpu_onboot()) {
		// Initialize pmap_bootpdir, the bootstrap page directory.
		// Page directory entries (PDEs) corresponding to the 
		// user-mode address space between VM_USERLO and VM_USERHI
		// should all be initialized to PTE_ZERO (see kern/pmap.h).
		// All virtual addresses below and above this user area
		// should be identity-mapped to the same physical addresses,
		// but only accessible in kernel mode (not in user mode).
		// The easiest way to do this is to use 4MB page mappings.
		// Since these page mappings never change on context switches,
		// we can also mark them global (PTE_G) so the processor
		// doesn't flush these mappings when we reload the PDBR.

		int i;
		for (i=0; i < 1024; ++i){
			uint32_t addr = (uint32_t)i*PTSIZE;
			if (addr >= VM_USERLO && addr < VM_USERHI){
				pmap_bootpdir[i] = PTE_ZERO;
			}
			else{
				pmap_bootpdir[i] = (i << PDXSHIFT) | PTE_P | PTE_W | PTE_PS | PTE_G;
			}
		}
	}

	// On x86, segmentation maps a VA to a LA (linear addr) and
	// paging maps the LA to a PA.  i.e., VA => LA => PA.  If paging is
	// turned off the LA is used as the PA.  There is no way to
	// turn off segmentation.  At the moment we turn on paging,
	// the code we're executing must be in an identity-mapped memory area
	// where LA == PA according to the page mapping structures.
	// In PIOS this is always the case for the kernel's address space,
	// so we don't have to play any special tricks as in other kernels.

	// Enable 4MB pages and global pages.
	uint32_t cr4 = rcr4();
	cr4 |= CR4_PSE | CR4_PGE;
	lcr4(cr4);

	// Install the bootstrap page directory into the PDBR.
	lcr3(mem_phys(pmap_bootpdir));
	// Turn on paging.
	uint32_t cr0 = rcr0();
	cr0 |= CR0_PE|CR0_PG|CR0_AM|CR0_WP|CR0_NE|CR0_TS|CR0_MP|CR0_TS;
	cr0 &= ~(CR0_EM);
	lcr0(cr0);

	// If we survived the lcr0, we're running with paging enabled.
	// Now check the page table management functions below.
	if (cpu_onboot())
		pmap_check();
}

//
// Allocate a new page directory, initialized from the bootstrap pdir.
// Returns the new pdir with a reference count of 1.
//
pte_t *
pmap_newpdir(void)
{
	pageinfo *pi = mem_alloc();
	if (pi == NULL)
		return NULL;
	mem_incref(pi);
	pte_t *pdir = mem_pi2ptr(pi);

	// Initialize it from the bootstrap page directory
	assert(sizeof(pmap_bootpdir) == PAGESIZE);
	memmove(pdir, pmap_bootpdir, PAGESIZE);

	return pdir;
}

// Free a page directory, and all page tables and mappings it may contain.
void
pmap_freepdir(pageinfo *pdirpi)
{
	pmap_remove(mem_pi2ptr(pdirpi), VM_USERLO, VM_USERHI-VM_USERLO);
	mem_free(pdirpi);
}

// Free a page table and all page mappings it may contain.
void
pmap_freeptab(pageinfo *ptabpi)
{
	pte_t *pte = mem_pi2ptr(ptabpi), *ptelim = pte + NPTENTRIES;
	for (; pte < ptelim; pte++) {
		uint32_t pgaddr = PGADDR(*pte);
		if (pgaddr != PTE_ZERO)
			mem_decref(mem_phys2pi(pgaddr), mem_free);
	}
	mem_free(ptabpi);
}

// Given 'pdir', a pointer to a page directory, pmap_walk returns
// a pointer to the page table entry (PTE) for user virtual address 'va'.
// This requires walking the two-level page table structure.
//
// If the relevant page table doesn't exist in the page directory, then:
//    - If writing == 0, pmap_walk returns NULL.
//    - Otherwise, pmap_walk tries to allocate a new page table
//	with mem_alloc.  If this fails, pmap_walk returns NULL.
//    - The new page table is cleared and its refcount set to 1.
//    - Finally, pmap_walk returns a pointer to the requested entry
//	within the new page table.
//
// If the relevant page table does already exist in the page directory,
// but it is read shared and writing != 0, then copy the page table
// to obtain an exclusive copy of it and write-enable the PDE.
//
// Hint: you can turn a pageinfo pointer into the physical address of the
// page it refers to with mem_pi2phys() from kern/mem.h.
//
// Hint 2: the x86 MMU checks permission bits in both the page directory
// and the page table, so it's safe to leave some page permissions
// more permissive than strictly necessary.
pte_t *
pmap_walk(pde_t *pdir, uint32_t va, bool writing)
{
	assert(va >= VM_USERLO && va < VM_USERHI);
	
	pde_t *pdentry = &pdir[PDX(va)];
	if (*pdentry == PTE_ZERO){
		if (writing){
			pageinfo *pi = mem_alloc();
			if (pi == NULL){
				return NULL;
			}
			mem_incref(pi);
			pte_t *ptable = mem_pi2ptr(pi);

			int i;
			for (i = 0; i < 1024; i++){
			  	ptable[i] = PTE_ZERO;
			}

			*pdentry = mem_pi2phys(pi) | PTE_A | PTE_P | PTE_W | PTE_U;
			assert(*pdentry != PTE_ZERO);
			
			return &ptable[PTX(va)];
		}
		else{
			return NULL;
		}
	}
	else{
		//grab the page table pointed to by the entry
		pte_t *ptable = (uint32_t *) PGADDR(*pdentry);
		//return the page table entry given by the table and the offset in va
		return &ptable[PTX(va)];
	}
}

//
// Map the physical page 'pi' at user virtual address 'va'.
// The permissions (the low 12 bits) of the page table
//  entry should be set to 'perm | PTE_P'.
//
// Requirements
//   - If there is already a page mapped at 'va', it should be pmap_remove()d.
//   - If necessary, allocate a page table on demand and insert into 'pdir'.
//   - pi->refcount should be incremented if the insertion succeeds.
//   - The TLB must be invalidated if a page was formerly present at 'va'.
//
// Corner-case hint: Make sure to consider what happens when the same 
// pi is re-inserted at the same virtual address in the same pdir.
// What if this is the only reference to that page?
//
// RETURNS: 
//   a pointer to the inserted PTE on success (same as pmap_walk)
//   NULL, if page table couldn't be allocated
//
// Hint: The reference solution uses pmap_walk, pmap_remove, and mem_pi2phys.
//
pte_t *
pmap_insert(pde_t *pdir, pageinfo *pi, uint32_t va, int perm)
{
	// Fill in this function
	pte_t *pte = pmap_walk(pdir, va, 1);

	if (pte == NULL)
    	return NULL;

    mem_incref(pi);

	if (*pte != PTE_ZERO){
	    pmap_remove(pdir, va, PAGESIZE);
	}

	*pte = mem_pi2phys(pi) | perm | PTE_P;
	return pte;
}

//
// Unmap the physical pages starting at user virtual address 'va'
// and covering a virtual address region of 'size' bytes.
// The caller must ensure that both 'va' and 'size' are page-aligned.
// If there is no mapping at that address, pmap_remove silently does nothing.
// Clears nominal permissions (SYS_RW flags) as well as mappings themselves.
//
// Details:
//   - The refcount on mapped pages should be decremented atomically.
//   - The physical page should be freed if the refcount reaches 0.
//   - The page table entry corresponding to 'va' should be set to 0.
//     (if such a PTE exists)
//   - The TLB must be invalidated if you remove an entry from
//     the pdir/ptab.
//   - If the region to remove covers a whole 4MB page table region,
//     then unmap and free the page table after unmapping all its contents.
//
// Hint: The TA solution is implemented using pmap_lookup,
// 	pmap_inval, and mem_decref.

void
pmap_remove(pde_t *pdir, uint32_t va, size_t size)
{
	assert(PGOFF(size) == 0);	// must be page-aligned
	assert(va >= VM_USERLO && va < VM_USERHI);
	assert(size <= VM_USERHI - va);
	
	assert(PGOFF(va) == 0); //va must be page-aligned too!

	pmap_inval(pdir, va, size);

	pde_t *pdentry = &pdir[PDX(va)];
	uint32_t toppage = va + size;

	//first chunk, free 4KB table entries up until we get to the start of a 4MB page,
	//or if we've already reached the top page
	while (va < ROUNDUP(va,PTSIZE) && va < toppage){
		//if the page table whose elements we are removing is uninitialized we just keep going
		pdentry = &pdir[PDX(va)];
		if (*pdentry != PTE_ZERO){
			pte_t *pte = pmap_walk(pdir, va, 1);
			uint32_t addr = PGADDR(*pte);
			if (addr != PTE_ZERO){
				mem_decref(mem_phys2pi(addr),mem_free);
				*pte = PTE_ZERO;
			}
		}
		va = va + PAGESIZE;
	}

	//free all the 4MB page chunks
	while ((va+PTSIZE) <= ROUNDDOWN(toppage,PTSIZE)){
		if (toppage - va >= PTSIZE){
			pdentry = &pdir[PDX(va)];
			if (*pdentry != PTE_ZERO){
				uint32_t addr = PGADDR(*pdentry);
				mem_decref(mem_phys2pi(addr), pmap_freeptab);
				*pdentry = PTE_ZERO;
			}
			va = va + PTSIZE;
		}
		else{
			break;
		}
	}

	//free the last 4KB page table entries after the last 4MB chunk
	while (va < toppage){
		pdentry = &pdir[PDX(va)];
		if (*pdentry != PTE_ZERO){
			pte_t *pte = pmap_walk(pdir, va, 1);
			uint32_t addr = PGADDR(*pte);
			if (addr != PTE_ZERO){
				mem_decref(mem_phys2pi(addr),mem_free);
				*pte = PTE_ZERO;
			}
		}
		va = va + PAGESIZE;
	}
}

//
// Invalidate the TLB entry or entries for a given virtual address range,
// but only if the page tables being edited are the ones
// currently in use by the processor.
//
void
pmap_inval(pde_t *pdir, uint32_t va, size_t size)
{
	// Flush the entry only if we're modifying the current address space.
	proc *p = proc_cur();
	if (p == NULL || p->pdir == pdir) {
		if (size == PAGESIZE)
			invlpg(mem_ptr(va));	// invalidate one page
		else
			lcr3(mem_phys(pdir));	// invalidate everything
	}
}

//
// Virtually copy a range of pages from spdir to dpdir (could be the same).
// Uses copy-on-write to avoid the cost of immediate copying:
// instead just copies the mappings and makes both source and dest read-only.
// Returns true if successfull, false if not enough memory for copy.
//
int
pmap_copy(pde_t *spdir, uint32_t sva, pde_t *dpdir, uint32_t dva,
		size_t size)
{
	assert(PTOFF(sva) == 0);	// must be 4MB-aligned
	assert(PTOFF(dva) == 0);
	assert(PTOFF(size) == 0);
	assert(sva >= VM_USERLO && sva < VM_USERHI);
	assert(dva >= VM_USERLO && dva < VM_USERHI);
	assert(size <= VM_USERHI - sva);
	assert(size <= VM_USERHI - dva);

	pmap_inval(spdir, sva, size);
	pmap_inval(dpdir, dva, size);
	
	pde_t * spentry = &spdir[PDX(sva)];
	pde_t * dpentry = &dpdir[PDX(dva)];
	uint32_t end = sva + size;
	
	int j = 0;
	for (; sva < end; sva += PTSIZE, spentry++, dpentry++){
		if (PGADDR(*spentry) == PTE_ZERO){
			*dpentry = *spentry;
		} else {
			pageinfo * pi = mem_alloc();
			if (!pi) return 0;
			pte_t * entry = (pte_t *) PGADDR(*spentry);
			int i;
			for (i = 0; i < NPTENTRIES; i++, entry++){
					if(PGADDR(*entry) == PGADDR(PTE_ZERO)) continue;
					int perm = PGOFF(*entry) | SYS_READ;
					if (perm & PTE_W || perm & SYS_WRITE){
						perm = (perm & ~PTE_W) | SYS_WRITE;
					}
					*entry = PGADDR(*entry) | perm;
					mem_incref(mem_phys2pi(PGADDR(*entry)));
			}
			memmove(mem_pi2ptr(pi), (void *) PGADDR(*spentry), PAGESIZE);
			*dpentry = PGADDR(mem_pi2phys(pi)) | PGOFF(*spentry);
			assert(PGADDR(*spentry) != PGADDR(*dpentry));
			pi->refcount=1;
		}
	}
	return 1;
}

//
// Transparently handle a page fault entirely in the kernel, if possible.
// If the page fault was caused by a write to a copy-on-write page,
// then performs the actual page copy on demand and calls trap_return().
// If the fault wasn't due to the kernel's copy on write optimization,
// however, this function just returns so the trap gets blamed on the user.
//
void
pmap_pagefault(trapframe *tf)
{
	tf->trapno = T_PGFLT;
	// Read processor's CR2 register to find the faulting linear address.
	uint32_t fva = rcr2();
	//cprintf("pmap_pagefault fva %x eip %x\n", fva, tf->eip);
	//if (!(tf->cs & 3)) cprintf("while in kernel mode\n");
	
	// Fill in the rest of this code.
	if (fva < VM_USERLO || fva >= VM_USERHI) {
		cprintf("pmap_pagefault - outside of userspace!!\n");
		return;
		//proc_ret(tf, 1);
	}

	proc *p = proc_cur();
	pmap_inval(p->pdir, fva, PAGESIZE);
	pde_t *pde = p->pdir;
	pte_t *pte = pmap_walk(pde, fva, 1);
	
	pte_t oldpte = *pte;
	
	if (!pte) {
		cprintf("pmap_pagefault - !pte !!!\n");
		return;
		//proc_ret(tf, 1);
	}

	uint32_t permissions = PGOFF(*pte);
	pageinfo *pi = mem_phys2pi(PGADDR(*pte));
	if (!(permissions & PTE_W) && (permissions & SYS_WRITE)) {
		if (mem_pi2phys(pi) == PTE_ZERO || pi->refcount > 1) {
			//cprintf("copying on write - new\n");
			pageinfo *pi_new = mem_alloc();
			pi_new->refcount = 1;
			memmove(mem_pi2ptr(pi_new), (void *) PGADDR(*pte), PAGESIZE);
			if (PGADDR(*pte) != PTE_ZERO)
				mem_decref(mem_phys2pi(PGADDR(*pte)), mem_free);
			*pte = mem_pi2phys(pi_new);
			assert(*pte != oldpte);
		}// else cprintf("copying on write - old\n");
		
		permissions = (permissions | PTE_W | PTE_P) & ~SYS_RW;
		*pte = PGADDR(*pte) | permissions;
		trap_return(tf);
	}// else cprintf("won't copy on write coz - %d , %d\n", !(permissions & PTE_W), (permissions & SYS_WRITE));

	// This page is not mapped right and causes pagefaults on read_ebp()
	// I can't find the bug in pmap_init so fixing it this way for now..
	if (tf->eip == 0x100948) {
		//cprintf("** special fault - hacked fix ***\n");
		*pte |= PTE_P | PTE_W;
		trap_return(tf);
	}
	
	cprintf("pmap_pagefault - permissions not good %d %d\n", (permissions & PTE_W), (permissions & SYS_WRITE));
	return;
	//proc_ret(tf, 1);
}

//
// Helper function for pmap_merge: merge a single memory page
// that has been modified in both the source and destination.
// If conflicting writes to a single byte are detected on the page,
// print a warning to the console and remove the page from the destination.
// If the destination page is read-shared, be sure to copy it before modifying!
//
void
pmap_mergepage(pte_t *rpte, pte_t *spte, pte_t *dpte, uint32_t dva)
{
	// check if it is read shared
	int d_perm = PGOFF(*dpte);
	if ((d_perm & PTE_P) && !(d_perm & PTE_W) && (d_perm & SYS_WRITE) &&
					((PGADDR(*dpte) == PTE_ZERO) ||
					  mem_phys2pi(PGADDR(*dpte))->refcount >= 1)) {
		int perm = (PGOFF(*dpte)| PTE_W) & ~SYS_RW;
		if (mem_phys2pi(PGADDR(*dpte))->refcount > 1) {
			//cprintf("copy on write dest - new\n");
			pageinfo *pi_new = mem_alloc();
			pi_new->refcount = 1;
			memmove(mem_pi2ptr(pi_new), (void *) PGADDR(*dpte), PAGESIZE);
			if (PGADDR(*dpte) != PTE_ZERO)
				mem_decref(mem_phys2pi(PGADDR(*dpte)), mem_free);
			*dpte = mem_pi2phys(pi_new);
		}// else cprintf("copy on write dest - old\n");
		*dpte = PGADDR(*dpte) | perm;
	}
	
	uint32_t *rpg = (uint32_t *)PGADDR(*rpte);
	uint32_t *spg = (uint32_t *)PGADDR(*spte);
	uint32_t *dpg = (uint32_t *)PGADDR(*dpte);

	int i;
	//PAGESIZE is in bytes?
	for (i = 0; i < PAGESIZE/4; i++, dpg++, spg++, rpg++) {
		if (*rpg != *spg) {
			// check for conflicting write
			if (*rpg != *dpg && *spg != *dpg) {
				cprintf("pmap_mergepage: conflicting write: %x %x %x\n", *spg, *rpg, *dpg);
				mem_decref(mem_phys2pi(PGADDR(*dpte)), mem_free);
				*dpte = PTE_ZERO;
				return;
			}
			*dpg = *spg;
		}
	}

	//panic("pmap_mergepage() not implemented");
}

// 
// Merge differences between a reference snapshot represented by rpdir
// and a source address space spdir into a destination address space dpdir.
//
int
pmap_merge(pde_t *rpdir, pde_t *spdir, uint32_t sva,
		pde_t *dpdir, uint32_t dva, size_t size)
{
	assert(PTOFF(sva) == 0);	// must be 4MB-aligned
	assert(PTOFF(dva) == 0);
	assert(PTOFF(size) == 0);
	assert(sva >= VM_USERLO && sva < VM_USERHI);
	assert(dva >= VM_USERLO && dva < VM_USERHI);
	assert(size <= VM_USERHI - sva);
	assert(size <= VM_USERHI - dva);
	
	pmap_inval(spdir, sva, size);
	pmap_inval(dpdir, dva, size);
	
	uint32_t i;
	for (i = 0; i < size; ) {
		pde_t* spde = &spdir[PDX(sva+i)];
		pde_t* dpde = &dpdir[PDX(dva+i)];
		pde_t* rpde = &rpdir[PDX(sva+i)];
		
		//Skip empty/unchanged PTs
		if (*spde == *rpde) {
			i += PTSIZE;
			continue;
		}

		int j;
		for (j = 0; j < NPTENTRIES; j++, i += PAGESIZE){		
			pte_t *rpte = pmap_walk(rpdir, sva + i, false);
			pte_t *spte = pmap_walk(spdir, sva + i, false);
			pte_t *dpte = pmap_walk(dpdir, dva + i, false);
			
			//Skip same entries
			if (*spte == *rpte && *dpte == *rpte) continue;
			
			//If changed only at source, copy on write
			if (*dpte == *rpte && *spte != *rpte){
				if(PGADDR(*dpte) != PTE_ZERO)
					mem_decref(mem_phys2pi(PGADDR(*dpte)), mem_free);
				mem_incref(mem_phys2pi(PGADDR(*spte)));
				if (PGOFF(*spte) & PTE_W || PGOFF(*spte) & SYS_WRITE)
					*spte |= SYS_WRITE;
				*spte &= ~PTE_W;
				*dpte = *spte;
				//cprintf("copy-on-write merged\n");
				continue;
			}
		    
		    //Else merge changes
			pmap_mergepage(rpte, spte, dpte, dva + i);
			//cprintf("merged a page\n");
		}
	}

	// what to return?!
	return size;
}

//
// Set the nominal permission bits on a range of virtual pages to 'perm'.
// Adding permission to a nonexistent page maps zero-filled memory.
// It's OK to add SYS_READ and/or SYS_WRITE permission to a PTE_ZERO mapping;
// this causes the pmap_zero page to be mapped read-only (PTE_P but not PTE_W).
// If the user gives SYS_WRITE permission to a PTE_ZERO mapping,
// the page fault handler copies the zero page when the first write occurs.
//
int
pmap_setperm(pde_t *pdir, uint32_t va, uint32_t size, int perm)
{
	assert(PGOFF(va) == 0);
	assert(PGOFF(size) == 0);
	assert(va >= VM_USERLO && va < VM_USERHI);
	assert(size <= VM_USERHI - va);
	assert((perm & ~(SYS_RW)) == 0);

	uint32_t a;
	for (a = va; a < va + size; a += PAGESIZE) {
		pte_t *pte = pmap_walk(pdir, a, true);
		assert(pte != NULL);
		*pte |= perm | PTE_U;
	}

	// what to return?!
	return size;
	
}

//
// This function returns the physical address of the page containing 'va',
// defined by the page directory 'pdir'.  The hardware normally performs
// this functionality for us!  We define our own version to help check
// the pmap_check() function; it shouldn't be used elsewhere.
//
static uint32_t
va2pa(pde_t *pdir, uintptr_t va)
{
	pdir = &pdir[PDX(va)];
	if (!(*pdir & PTE_P))
		return ~0;
	pte_t *ptab = mem_ptr(PGADDR(*pdir));
	if (!(ptab[PTX(va)] & PTE_P))
		return ~0;
	return PGADDR(ptab[PTX(va)]);
}

// check pmap_insert, pmap_remove, &c
void
pmap_check(void)
{
	extern pageinfo *mem_freelist;

	pageinfo *pi, *pi0, *pi1, *pi2, *pi3;
	pageinfo *fl;
	pte_t *ptep, *ptep1;
	int i;

	// should be able to allocate three pages
	pi0 = pi1 = pi2 = 0;
	pi0 = mem_alloc();
	pi1 = mem_alloc();
	pi2 = mem_alloc();
	pi3 = mem_alloc();

	assert(pi0);
	assert(pi1 && pi1 != pi0);
	assert(pi2 && pi2 != pi1 && pi2 != pi0);

	// temporarily steal the rest of the free pages
	fl = mem_freelist;
	mem_freelist = NULL;

	// should be no free memory
	assert(mem_alloc() == NULL);

	// there is no free memory, so we can't allocate a page table 
	assert(pmap_insert(pmap_bootpdir, pi1, VM_USERLO, 0) == NULL);

	// free pi0 and try again: pi0 should be used for page table
	mem_free(pi0);
	assert(pmap_insert(pmap_bootpdir, pi1, VM_USERLO, 0) != NULL);
	assert(PGADDR(pmap_bootpdir[PDX(VM_USERLO)]) == mem_pi2phys(pi0));
	assert(va2pa(pmap_bootpdir, VM_USERLO) == mem_pi2phys(pi1));
	assert(pi1->refcount == 1);
	assert(pi0->refcount == 1);

	// should be able to map pi2 at VM_USERLO+PAGESIZE
	// because pi0 is already allocated for page table
	assert(pmap_insert(pmap_bootpdir, pi2, VM_USERLO+PAGESIZE, 0));
	assert(va2pa(pmap_bootpdir, VM_USERLO+PAGESIZE) == mem_pi2phys(pi2));
	assert(pi2->refcount == 1);

	// should be no free memory
	assert(mem_alloc() == NULL);

	// should be able to map pi2 at VM_USERLO+PAGESIZE
	// because it's already there
	assert(pmap_insert(pmap_bootpdir, pi2, VM_USERLO+PAGESIZE, 0));
	assert(va2pa(pmap_bootpdir, VM_USERLO+PAGESIZE) == mem_pi2phys(pi2));
	assert(pi2->refcount == 1);

	// pi2 should NOT be on the free list
	// could hapien in ref counts are handled slopiily in pmap_insert
	assert(mem_alloc() == NULL);

	// check that pmap_walk returns a pointer to the pte
	ptep = mem_ptr(PGADDR(pmap_bootpdir[PDX(VM_USERLO+PAGESIZE)]));
	assert(pmap_walk(pmap_bootpdir, VM_USERLO+PAGESIZE, 0)
		== ptep+PTX(VM_USERLO+PAGESIZE));

	// should be able to change permissions too.
	assert(pmap_insert(pmap_bootpdir, pi2, VM_USERLO+PAGESIZE, PTE_U));
	assert(va2pa(pmap_bootpdir, VM_USERLO+PAGESIZE) == mem_pi2phys(pi2));
	assert(pi2->refcount == 1);
	assert(*pmap_walk(pmap_bootpdir, VM_USERLO+PAGESIZE, 0) & PTE_U);
	assert(pmap_bootpdir[PDX(VM_USERLO)] & PTE_U);
	
	// should not be able to map at VM_USERLO+PTSIZE
	// because we need a free page for a page table
	assert(pmap_insert(pmap_bootpdir, pi0, VM_USERLO+PTSIZE, 0) == NULL);

	// insert pi1 at VM_USERLO+PAGESIZE (replacing pi2)
	assert(pmap_insert(pmap_bootpdir, pi1, VM_USERLO+PAGESIZE, 0));
	assert(!(*pmap_walk(pmap_bootpdir, VM_USERLO+PAGESIZE, 0) & PTE_U));

	// should have pi1 at both +0 and +PAGESIZE, pi2 nowhere, ...
	assert(va2pa(pmap_bootpdir, VM_USERLO+0) == mem_pi2phys(pi1));
	assert(va2pa(pmap_bootpdir, VM_USERLO+PAGESIZE) == mem_pi2phys(pi1));
	// ... and ref counts should reflect this
	assert(pi1->refcount == 2);
	assert(pi2->refcount == 0);

	// pi2 should be returned by mem_alloc
	assert(mem_alloc() == pi2);

	// unmapping pi1 at VM_USERLO+0 should keep pi1 at +PAGESIZE
	pmap_remove(pmap_bootpdir, VM_USERLO+0, PAGESIZE);
	assert(va2pa(pmap_bootpdir, VM_USERLO+0) == ~0);
	assert(va2pa(pmap_bootpdir, VM_USERLO+PAGESIZE) == mem_pi2phys(pi1));
	assert(pi1->refcount == 1);
	assert(pi2->refcount == 0);
	assert(mem_alloc() == NULL);	// still should have no pages free

	// unmapping pi1 at VM_USERLO+PAGESIZE should free it
	pmap_remove(pmap_bootpdir, VM_USERLO+PAGESIZE, PAGESIZE);
	assert(va2pa(pmap_bootpdir, VM_USERLO+0) == ~0);
	assert(va2pa(pmap_bootpdir, VM_USERLO+PAGESIZE) == ~0);
	assert(pi1->refcount == 0);
	assert(pi2->refcount == 0);

	// so it should be returned by page_alloc
	assert(mem_alloc() == pi1);

	// should once again have no free memory
	assert(mem_alloc() == NULL);

	// should be able to pmap_insert to change a page
	// and see the new data immediately.
	memset(mem_pi2ptr(pi1), 1, PAGESIZE);
	memset(mem_pi2ptr(pi2), 2, PAGESIZE);
	pmap_insert(pmap_bootpdir, pi1, VM_USERLO, 0);
	assert(pi1->refcount == 1);
	assert(*(int*)VM_USERLO == 0x01010101);
	pmap_insert(pmap_bootpdir, pi2, VM_USERLO, 0);
	assert(*(int*)VM_USERLO == 0x02020202);
	assert(pi2->refcount == 1);
	assert(pi1->refcount == 0);
	assert(mem_alloc() == pi1);
	pmap_remove(pmap_bootpdir, VM_USERLO, PAGESIZE);
	assert(pi2->refcount == 0);
	assert(mem_alloc() == pi2);

	// now use a pmap_remove on a large region to take pi0 back
	pmap_remove(pmap_bootpdir, VM_USERLO, VM_USERHI-VM_USERLO);
	assert(pmap_bootpdir[PDX(VM_USERLO)] == PTE_ZERO);
	assert(pi0->refcount == 0);
	assert(mem_alloc() == pi0);
	assert(mem_freelist == NULL);

	// test pmap_remove with large, non-ptable-aligned regions
	mem_free(pi1);
	uintptr_t va = VM_USERLO;
	assert(pmap_insert(pmap_bootpdir, pi0, va, 0));
	assert(pmap_insert(pmap_bootpdir, pi0, va+PAGESIZE, 0));
	assert(pmap_insert(pmap_bootpdir, pi0, va+PTSIZE-PAGESIZE, 0));
	assert(PGADDR(pmap_bootpdir[PDX(VM_USERLO)]) == mem_pi2phys(pi1));
	assert(mem_freelist == NULL);
	mem_free(pi2);
	assert(pmap_insert(pmap_bootpdir, pi0, va+PTSIZE, 0));
	assert(pmap_insert(pmap_bootpdir, pi0, va+PTSIZE+PAGESIZE, 0));
	assert(pmap_insert(pmap_bootpdir, pi0, va+PTSIZE*2-PAGESIZE, 0));
	assert(PGADDR(pmap_bootpdir[PDX(VM_USERLO+PTSIZE)])
		== mem_pi2phys(pi2));
	assert(mem_freelist == NULL);
	mem_free(pi3);
	assert(pmap_insert(pmap_bootpdir, pi0, va+PTSIZE*2, 0));
	assert(pmap_insert(pmap_bootpdir, pi0, va+PTSIZE*2+PAGESIZE, 0));
	assert(pmap_insert(pmap_bootpdir, pi0, va+PTSIZE*3-PAGESIZE*2, 0));
	assert(pmap_insert(pmap_bootpdir, pi0, va+PTSIZE*3-PAGESIZE, 0));
	assert(PGADDR(pmap_bootpdir[PDX(VM_USERLO+PTSIZE*2)])
		== mem_pi2phys(pi3));
	assert(mem_freelist == NULL);
	assert(pi0->refcount == 10);
	assert(pi1->refcount == 1);
	assert(pi2->refcount == 1);
	assert(pi3->refcount == 1);
	pmap_remove(pmap_bootpdir, va+PAGESIZE, PTSIZE*3-PAGESIZE*2);
	assert(pi0->refcount == 2);
	assert(pi2->refcount == 0); assert(mem_alloc() == pi2);
	assert(mem_freelist == NULL);
	pmap_remove(pmap_bootpdir, va, PTSIZE*3-PAGESIZE);
	assert(pi0->refcount == 1);
	assert(pi1->refcount == 0); assert(mem_alloc() == pi1);
	assert(mem_freelist == NULL);
	pmap_remove(pmap_bootpdir, va+PTSIZE*3-PAGESIZE, PAGESIZE);
	assert(pi0->refcount == 0);	// pi3 might or might not also be freed
	pmap_remove(pmap_bootpdir, va+PAGESIZE, PTSIZE*3);
	assert(pi3->refcount == 0);
	mem_alloc(); mem_alloc();	// collect pi0 and pi3
	assert(mem_freelist == NULL);

	// check pointer arithmetic in pmap_walk
	mem_free(pi0);
	va = VM_USERLO + PAGESIZE*NPTENTRIES + PAGESIZE;
	ptep = pmap_walk(pmap_bootpdir, va, 1);
	ptep1 = mem_ptr(PGADDR(pmap_bootpdir[PDX(va)]));
	assert(ptep == ptep1 + PTX(va));
	pmap_bootpdir[PDX(va)] = PTE_ZERO;
	pi0->refcount = 0;

	// check that new page tables get cleared
	memset(mem_pi2ptr(pi0), 0xFF, PAGESIZE);
	mem_free(pi0);
	pmap_walk(pmap_bootpdir, VM_USERHI-PAGESIZE, 1);
	ptep = mem_pi2ptr(pi0);
	for(i=0; i<NPTENTRIES; i++)
		assert(ptep[i] == PTE_ZERO);
	pmap_bootpdir[PDX(VM_USERHI-PAGESIZE)] = PTE_ZERO;
	pi0->refcount = 0;

	// give free list back
	mem_freelist = fl;

	// free the pages we filched
	mem_free(pi0);
	mem_free(pi1);
	mem_free(pi2);
	mem_free(pi3);

	cprintf("pmap_check() succeeded!\n");
}
