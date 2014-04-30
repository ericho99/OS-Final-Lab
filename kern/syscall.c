/*
 * System call handling.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from the xv6 instructional operating system from MIT.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#include <inc/x86.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/trap.h>
#include <inc/syscall.h>

#include <kern/cpu.h>
#include <kern/trap.h>
#include <kern/proc.h>
#include <kern/syscall.h>
#include <kern/net.h>

// This bit mask defines the eflags bits user code is allowed to set.
#define FL_USER		(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_DF|FL_OF)


// During a system call, generate a specific processor trap -
// as if the user code's INT 0x30 instruction had caused it -
// and reflect the trap to the parent process as with other traps.
static void gcc_noreturn
systrap(trapframe *utf, int trapno, int err)
{
	utf->trapno = trapno;
	utf->err = err;
	proc_ret(utf, 0);
}

// Recover from a trap that occurs during a copyin or copyout,
// by aborting the system call and reflecting the trap to the parent process,
// behaving as if the user program's INT instruction had caused the trap.
// This uses the 'recover' pointer in the current cpu struct,
// and invokes systrap() above to blame the trap on the user process.
//
// Notes:
// - Be sure the parent gets the correct trapno, err, and eip values.
// - Be sure to release any spinlocks you were holding during the copyin/out.
//
static void gcc_noreturn
sysrecover(trapframe *ktf, void *recoverdata)
{
	cpu *cp = cpu_cur();
	cp->recover = NULL;
	systrap((trapframe *) recoverdata, ktf->trapno, ktf->err);
}

// Check a user virtual address block for validity:
// i.e., make sure the complete area specified lies in
// the user address space between VM_USERLO and VM_USERHI.
// If not, abort the syscall by sending a T_PGFLT to the parent,
// again as if the user program's INT instruction was to blame.
//
// Note: Be careful that your arithmetic works correctly
// even if size is very large, e.g., if uva+size wraps around!
//
static void checkva(trapframe *utf, uint32_t uva, size_t size)
{
	if (uva < VM_USERLO || uva >= VM_USERHI || size > VM_USERHI - uva) {
		systrap(utf, T_PGFLT, 0);
	}
}

// Copy data to/from user space,
// using checkva() above to validate the address range
// and using sysrecover() to recover from any traps during the copy.
void usercopy(trapframe *utf, bool copyout,
			void *kva, uint32_t uva, size_t size)
{
	checkva(utf, uva, size);

	cpu *cp = cpu_cur();
	void *temp = cp->recover;
	cp->recover = sysrecover;
	cp->recoverdata = (void *)utf;

	// Now do the copy, but recover from page faults.
	if (copyout) {
		memmove((void *) uva, kva, size);
	} else {
		memmove(kva, (void *) uva, size);
	}

	cp->recover = temp;
	//cp->recover = NULL;
	
}

static void
do_cputs(trapframe *tf, uint32_t cmd)
{
	//cprintf("cputs\n");
	// Print the string supplied by the user: pointer in EBX
	char buf[CPUTS_MAX + 1];
	usercopy(tf, 0,  buf, tf->regs.ebx, CPUTS_MAX);
	buf[CPUTS_MAX] = 0;
	cprintf("%s", buf);
	//cprintf("%s", (char*)tf->regs.ebx);

	trap_return(tf);	// syscall completed
}

static void
do_put(trapframe * tf, uint32_t flags){

	uint8_t nnum = (tf->regs.edx & 0xFF00) >> 8;
	int cp_i = tf->regs.edx & 0xFF; //16 bit child index in edx
	//child procstate
	// procstate * cps = (procstate *) tf->regs.ebx;
	proc *p = proc_cur();
	proc *cp = p->child[cp_i];


	if (nnum != net_node){
		if (nnum != 0){
			net_migrate(tf, nnum, 0);
		}
		else if (RRNODE(p->home) != net_node){
			net_migrate(tf, RRNODE(p->home), 0);
		}
	}
	
	spinlock_acquire(&p->lock);
	
	if (!cp) {
		cp = proc_alloc(p, cp_i);
	}
	
	//We have to check this before doing anything
	while (cp->state != PROC_STOP) {
		proc_wait(p, cp, tf);
	}
	
	spinlock_release(&p->lock);
	
	if (flags & SYS_REGS){
		usercopy(tf, 0, &cp->sv, tf->regs.ebx, sizeof(procstate));
		// memcpy(&cp->sv, cps, sizeof(procstate));
		cp->sv.tf.eflags &= FL_USER;
	}

	// handle memory flags
	uintptr_t sva = tf->regs.esi;
	uintptr_t dva = tf->regs.edi;
	size_t size = tf->regs.ecx;
	uint32_t memop = flags & SYS_MEMOP;

	if (memop & SYS_ZERO) {
		checkva(tf, dva, size);
		pmap_remove(cp->pdir, dva, size);
	} else if (memop & SYS_COPY) {
		checkva(tf, sva, size);
		checkva(tf, dva, size);
		pmap_copy(p->pdir, sva, cp->pdir, dva, size);
	}

	// handle permission changes
	//uint32_t perms = flags & SYS_PERM;
	//flags cpdir childdest(dva) sz
	if (flags & SYS_PERM) {
		int nom_perm = flags & SYS_RW;
		if (nom_perm & SYS_READ)
			nom_perm |= PTE_P | PTE_U;

		uint32_t a;
		for (a = dva; a < dva + size; a += PAGESIZE) {
			pte_t *pte = pmap_walk(cp->pdir, a, true);
			assert(pte != NULL);
			if (nom_perm)
				*pte = *pte | nom_perm;
			else
				*pte = PGADDR(*pte);
		}
	}

	if (flags & SYS_SNAP) {
		pmap_copy(cp->pdir, VM_USERLO, cp->rpdir, VM_USERLO, VM_USERHI - VM_USERLO);
	}

	if (flags & SYS_START){
		proc_ready(cp);
	}

	trap_return(tf);
}

static void
do_get(trapframe * tf, uint32_t flags){
	//cprintf("get\n");
	int cp_i = tf->regs.edx & 0xFF; //16 bit child index in edx

	// procstate * ps = (procstate *) tf->regs.ebx;

	uint8_t nnum = (tf->regs.edx & 0xFF00) >> 8;
	proc *p = proc_cur();

	if (nnum != net_node){
		if (nnum != 0){
			net_migrate(tf, nnum, 0);
		}
		else if (RRNODE(p->home) != net_node){
			net_migrate(tf, RRNODE(p->home), 0);
		}
	}

	spinlock_acquire(&p->lock);
	proc *cp = p->child[cp_i];
	
	if (!cp) {
			cp = &proc_null;
	}
	
	assert(cp != NULL); //check to make sure the child actually exists...good to know methinks
	
	//wait for child to stop
	while (cp->state != PROC_STOP) {
		//cprintf("has to wait\n");
		proc_wait(p, cp, tf);
	}
	
	spinlock_release(&p->lock);

	if (flags & SYS_REGS){
		// memcpy(ps, &cp->sv, sizeof(procstate));
		usercopy(tf, 1, &cp->sv, tf->regs.ebx, sizeof(procstate));
	}

	// handle memory flags
	uintptr_t sva = tf->regs.esi;
	uintptr_t dva = tf->regs.edi;
	size_t size = tf->regs.ecx;
	uint32_t memop = flags & SYS_MEMOP;
	if ((flags & SYS_MERGE) == SYS_MERGE) {
		pmap_merge(cp->rpdir, cp->pdir, sva, p->pdir, dva, size);
	}
	else if (memop & SYS_ZERO) {
		checkva(tf, dva, size);
		pmap_remove(p->pdir, dva, size);
	}
	else if (memop & SYS_COPY) {
		checkva(tf, dva, size);
		checkva(tf, sva, size);
		pmap_copy(cp->pdir, sva, p->pdir, dva, size);
	}
	
	// handle permission changes
	//uint32_t perms = flags & SYS_PERM;
	//flags ppdir localdest(dva) sz
	if (flags & SYS_PERM) {
		int nom_perm = flags & SYS_RW;
		if (nom_perm & SYS_READ)
			nom_perm |= PTE_P | PTE_U;

		//pmap_setperm(p->pdir, dva, size, flags & SYS_RW);
		uint32_t a;
		for (a = dva; a < dva + size; a += PAGESIZE) {
			pte_t *pte = pmap_walk(p->pdir, a, true);
			assert(pte != NULL);
			if (nom_perm)
				*pte = *pte | nom_perm;
			else
				*pte = PGADDR(*pte);
		}
	}
	
	trap_return(tf);
}

static void
do_ret(trapframe * tf, uint32_t flags){
	proc *cur = proc_cur();
	if(RRNODE(cur->home) != net_node) {
    	net_migrate(tf, RRNODE(cur->home), 0);
    }
	proc_ret(tf, 1);
}

// Common function to handle all system calls -
// decode the system call type and call an appropriate handler function.
// Be sure to handle undefined system calls appropriately.
void
syscall(trapframe *tf)
{
	// EAX register holds system call command/flags
	uint32_t cmd = tf->regs.eax;
	proc *p = proc_cur();
	switch (cmd & SYS_TYPE) {
	case SYS_CPUTS:	return do_cputs(tf, cmd);
	// Your implementations of SYS_PUT, SYS_GET, SYS_RET here...

	case SYS_PUT: return do_put(tf, cmd);
	case SYS_GET: return do_get(tf, cmd);
	case SYS_RET: return do_ret(tf, cmd);
	default: return;		// handle as a regular trap (is this what we're supposed to do? undefinied system calls?)
	}
}

