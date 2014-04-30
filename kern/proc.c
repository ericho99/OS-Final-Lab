/*
 * PIOS process management.
 *
 * Copyright (C) 2010 Yale University.
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Primary author: Bryan Ford
 */

#include <inc/string.h>
#include <inc/syscall.h>

#include <kern/cpu.h>
#include <kern/mem.h>
#include <kern/trap.h>
#include <kern/proc.h>
#include <kern/init.h>
#include <kern/file.h>
#include <kern/net.h>



proc proc_null;		// null process - just leave it initialized to 0

proc *proc_root;	// root process, once it's created in init()

// LAB 2: insert your scheduling data structure declarations here.

proc *proc_head;
proc *proc_tail;
struct spinlock proc_lock;

void
proc_init(void)
{
	if (!cpu_onboot())
		return;

	// your module initialization code here
	spinlock_init(&proc_lock);
	proc_head = proc_tail = NULL;
}

// Allocate and initialize a new proc as child 'cn' of parent 'p'.
// Returns NULL if no physical memory available.
proc *
proc_alloc(proc *p, uint32_t cn)
{
	pageinfo *pi = mem_alloc();
	if (!pi)
		return NULL;
	mem_incref(pi);
	
	proc *cp = (proc*)mem_pi2ptr(pi);
	memset(cp, 0, sizeof(proc));
	spinlock_init(&cp->lock);
	cp->parent = p;
	cp->state = PROC_STOP;


	cp->home = RRCONS(net_node, mem_phys(cp), 0);
	
	//Page directories - we might need this stuff, idk, merge conflict
	// cp->pdir = pmap_newpdir();
	// cp->rpdir = pmap_newpdir();

	// Integer register state
	cp->sv.tf.ds = CPU_GDT_UDATA | 3;
	cp->sv.tf.es = CPU_GDT_UDATA | 3;
	cp->sv.tf.cs = CPU_GDT_UCODE | 3;
	cp->sv.tf.ss = CPU_GDT_UDATA | 3;

	cp->pdir = pmap_newpdir();
	if (!cp->pdir)
		return NULL;

	cp->rpdir = pmap_newpdir();
	if (!cp->rpdir)
	{
		pmap_freepdir(mem_ptr2pi(cp->pdir));
		return NULL;
	}

	if (p)
		p->child[cn] = cp;
	return cp;
}

// Put process p in the ready state and add it to the ready queue.
void
proc_ready(proc *p)
{
	//if (p->parent) cprintf("Added child to sched queue\n");
	//	else cprintf("added root to sched queue\n");
	spinlock_acquire(&p->lock);
	
	p->state = PROC_READY;
	p->readynext = NULL;
	spinlock_release(&p->lock);

	spinlock_acquire(&proc_lock);
	
	// I changed this, I think it was buggy -Ivan
	if (proc_head && proc_tail){
		proc_tail->readynext = p;
		proc_tail = p;
	} else
		proc_tail = proc_head = p;

	spinlock_release(&proc_lock);
}

// Save the current process's state before switching to another process.
// Copies trapframe 'tf' into the proc struct,
// and saves any other relevant state such as FPU state.
// The 'entry' parameter is one of:
//	-1	if we entered the kernel via a trap before executing an insn
//	0	if we entered via a syscall and must abort/rollback the syscall
//	1	if we entered via a syscall and are completing the syscall
void
proc_save(proc *p, trapframe *tf, int entry)
{
	//memcpy(&p->sv.tf, tf, sizeof(trapframe));
	p->sv.tf = *tf;
	if (!entry) {
			p->sv.tf.eip -= 2;
	}
	//we need to implement this correctly for blocking and stuff like that
}

// Go to sleep waiting for a given child process to finish running.
// Parent process 'p' must be running and locked on entry.
// The supplied trapframe represents p's register state on syscall entry.
void gcc_noreturn
proc_wait(proc *p, proc *cp, trapframe *tf)
{
	//assert(proc_cur() == p);
	assert(spinlock_holding(&p->lock));
	proc_save(p, tf, 0); //saves the trapframe in the parent, roll back the INT instruction (entry of 0)
	p->state = PROC_WAIT;
	p->runcpu = NULL;
	p->waitchild = cp;
	spinlock_release(&p->lock); //release the lock on the parent
	proc_sched(); //runs scheduler again
}

void gcc_noreturn
proc_sched(void)
{
	spinlock_acquire(&proc_lock);
	while (proc_head == NULL) {
		spinlock_release(&proc_lock);
		sti(); //Enable kbd interrupts
		pause();
		cli(); //Disable kbd interrupts
		spinlock_acquire(&proc_lock);
	}

	proc *p = proc_head;
	proc_head = p->readynext;
	if (proc_head == NULL)
		proc_tail = NULL;

	spinlock_acquire(&p->lock);
	spinlock_release(&proc_lock);
	proc_run(p);
}

// Switch to and run a specified process, which must already be locked.
void gcc_noreturn
proc_run(proc *p)
{
	if (!spinlock_holding(&p->lock)) panic("proc_run without lock");

	//if (p->parent) cprintf("running child\n");
	//	else cprintf("running root\n");
	
	p->state = PROC_RUN;
	p->runcpu = cpu_cur();
	cpu_cur()->proc = p;
	
	// Enable interrupts (for preemption)
	p->sv.tf.eflags |= (1 << 9);
	p->sv.tf.ds = CPU_GDT_UDATA | 3;
	p->sv.tf.es = CPU_GDT_UDATA | 3;
	p->sv.tf.cs = CPU_GDT_UCODE | 3;
	p->sv.tf.ss = CPU_GDT_UDATA | 3;
	lcr3 (mem_phys(p->pdir));
	
	spinlock_release(&p->lock);
	
	trap_return(&p->sv.tf);
}

// Yield the current CPU to another ready process.
// Called while handling a timer interrupt.
void gcc_noreturn
proc_yield(trapframe *tf)
{
	//proc * p = cpu_cur()->proc;
	proc *p = proc_cur();
	proc_save(p, tf, -1); // -1 because timer interrupt
	proc_ready(p);
	proc_sched();
}

// Put the current process to sleep by "returning" to its parent process.
// Used both when a process calls the SYS_RET system call explicitly,
// and when a process causes an unhandled trap in user mode.
// The 'entry' parameter is as in proc_save().
void gcc_noreturn
proc_ret(trapframe *tf, int entry)
{
	proc *cp = proc_cur();
	proc *p = cp->parent;
	
	if (p == NULL){
		if (tf->trapno != T_SYSCALL){
			trap_print(tf);
			panic("no parent to reflect trap");
		}
		file_io(tf);
		cprintf("fileio done\n");
	}

	spinlock_acquire(&cp->lock);
	cp->state = PROC_STOP;
	cp->runcpu = NULL;
	proc_save(cp, tf, entry);
	spinlock_release(&cp->lock);
	
	spinlock_acquire(&p->lock);
	if (p->state == PROC_WAIT && p->waitchild == cp) {
		p->waitchild = NULL;
		proc_run(p);
	}
	spinlock_release(&p->lock);
	
	proc_sched();
}

// Helper functions for proc_check()
static void child(int n);
static void grandchild(int n);

static struct procstate child_state;
static char gcc_aligned(16) child_stack[4][PAGESIZE];

static volatile uint32_t pingpong = 0;
static void *recovargs;

void
proc_check(void)
{
	// Spawn 2 child processes, executing on statically allocated stacks.

	int i;
	for (i = 0; i < 4; i++) {
		// Setup register state for child
		uint32_t *esp = (uint32_t*) &child_stack[i][PAGESIZE];
		*--esp = i;	// push argument to child() function
		*--esp = 0;	// fake return address
		child_state.tf.eip = (uint32_t) child;
		child_state.tf.esp = (uint32_t) esp;

		// Use PUT syscall to create each child,
		// but only start the first 2 children for now.
		cprintf("spawning child %d\n", i);
		sys_put(SYS_REGS | (i < 2 ? SYS_START : 0), i, &child_state,
			NULL, NULL, 0);
	}

	// Wait for both children to complete.
	// This should complete without preemptive scheduling
	// when we're running on a 2-processor machine.
	for (i = 0; i < 2; i++) {
		cprintf("waiting for child %d\n", i);
		sys_get(SYS_REGS, i, &child_state, NULL, NULL, 0);
	}
	cprintf("proc_check() 2-child test succeeded\n");
	
	// (Re)start all four children, and wait for them.
	// This will require preemptive scheduling to complete
	// if we have less than 4 CPUs.
	cprintf("proc_check: spawning 4 children\n");
	for (i = 0; i < 4; i++) {
		cprintf("spawning child %d\n", i);
		sys_put(SYS_START, i, NULL, NULL, NULL, 0);
	}
	
	// Wait for all 4 children to complete.
	for (i = 0; i < 4; i++)
		sys_get(0, i, NULL, NULL, NULL, 0);
	cprintf("proc_check() 4-child test succeeded\n");
	
	// Now do a trap handling test using all 4 children -
	// but they'll _think_ they're all child 0!
	// (We'll lose the register state of the other children.)
	i = 0;
	sys_get(SYS_REGS, i, &child_state, NULL, NULL, 0);
		// get child 0's state
	assert(recovargs == NULL);
	
	do {
		sys_put(SYS_REGS | SYS_START, i, &child_state, NULL, NULL, 0);
		sys_get(SYS_REGS, i, &child_state, NULL, NULL, 0);
		if (recovargs) {	// trap recovery needed
			trap_check_args *args = recovargs;
			cprintf("recover from trap %d\n",
				child_state.tf.trapno);
			child_state.tf.eip = (uint32_t) args->reip;
			args->trapno = child_state.tf.trapno;
		} else
			assert(child_state.tf.trapno == T_SYSCALL);
		i = (i+1) % 4;	// rotate to next child proc
	} while (child_state.tf.trapno != T_SYSCALL);
	assert(recovargs == NULL);

	cprintf("proc_check() trap reflection test succeeded\n");

	cprintf("proc_check() succeeded!\n");
}

static void child(int n)
{
	// Only first 2 children participate in first pingpong test
	if (n < 2) {
		int i;
		for (i = 0; i < 10; i++) {
			cprintf("in child %d count %d\n", n, i);
			while (pingpong != n)
				pause();
			xchg(&pingpong, !pingpong);
		}
		sys_ret();
	}

	// Second test, round-robin pingpong between all 4 children
	int i;
	for (i = 0; i < 10; i++) {
		cprintf("in child %d count %d\n", n, i);
		while (pingpong != n)
			pause();
		xchg(&pingpong, (pingpong + 1) % 4);
	}
	sys_ret();

	// Only "child 0" (or the proc that thinks it's child 0), trap check...
	if (n == 0) {
		assert(recovargs == NULL);
		trap_check(&recovargs);
		assert(recovargs == NULL);
		cprintf("Finished trap check\n");
		sys_ret();
	}

	panic("child(): shouldn't have gotten here");
}

static void grandchild(int n)
{
	panic("grandchild(): shouldn't have gotten here");
}

