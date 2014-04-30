/*
 * Trap handler entry and exit code, written in assembly language.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from the MIT Exokernel and JOS.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#include <inc/mmu.h>
#include <inc/trap.h>

#include <kern/cpu.h>



/* The TRAPHANDLER macro defines a globally-visible function for handling
 * a trap.  It pushes a trap number onto the stack, then jumps to _alltraps.
 * Use TRAPHANDLER for traps where the CPU automatically pushes an error code.
 */ 
#define TRAPHANDLER(name, num)						\
	.globl name;		/* define global symbol for 'name' */	\
	.type name, @function;	/* symbol type is function */		\
	.align 2;		/* align function definition */		\
	name:			/* function starts here */		\
	pushl $(num);							\
	jmp _alltraps

/* Use TRAPHANDLER_NOEC for traps where the CPU doesn't push an error code.
 * It pushes a 0 in place of the error code, so the trap frame has the same
 * format in either case.
 */
#define TRAPHANDLER_NOEC(name, num)					\
	.globl name;							\
	.type name, @function;						\
	.align 2;							\
	name:								\
	pushl $0;							\
	pushl $(num);							\
	jmp _alltraps

.text

/*
 * Lab 1: Your code here for generating entry points for the different traps.
 */
 
TRAPHANDLER_NOEC(h_divide, T_DIVIDE);
TRAPHANDLER_NOEC(h_debug, T_DEBUG);
TRAPHANDLER_NOEC(h_nmi, T_NMI);
TRAPHANDLER_NOEC(h_brkpt, T_BRKPT);
TRAPHANDLER_NOEC(h_oflow, T_OFLOW);
TRAPHANDLER_NOEC(h_bound, T_BOUND);
TRAPHANDLER_NOEC(h_illop, T_ILLOP);
TRAPHANDLER_NOEC(h_device, T_DEVICE);
TRAPHANDLER(h_dblflt, T_DBLFLT);
/* #9 skipped */
TRAPHANDLER(h_tss, T_TSS);
TRAPHANDLER(h_segnp, T_SEGNP);
TRAPHANDLER(h_stack, T_STACK);
TRAPHANDLER(h_gpflt, T_GPFLT);
TRAPHANDLER(h_pgflt, T_PGFLT);
/* #15 skipped */
TRAPHANDLER_NOEC(h_fperr, T_FPERR);
TRAPHANDLER(h_align, T_ALIGN);
TRAPHANDLER_NOEC(h_mchk, T_MCHK);
TRAPHANDLER_NOEC(h_simd, T_SIMD);
TRAPHANDLER_NOEC(h_secev, T_SECEV);
TRAPHANDLER_NOEC(h_syscall, T_SYSCALL);
TRAPHANDLER_NOEC(h_ltimer, T_LTIMER);
TRAPHANDLER_NOEC(h_spurious, T_IRQ0+IRQ_SPURIOUS);
TRAPHANDLER_NOEC(h_kbd, T_IRQ0+IRQ_KBD);
TRAPHANDLER_NOEC(h_serial, T_IRQ0+IRQ_SERIAL);

TRAPHANDLER_NOEC(h_0,   T_IRQ0+0);
TRAPHANDLER_NOEC(h_2,   T_IRQ0+2);
TRAPHANDLER_NOEC(h_3,   T_IRQ0+3);
TRAPHANDLER_NOEC(h_5,   T_IRQ0+5);
TRAPHANDLER_NOEC(h_6,   T_IRQ0+6);
TRAPHANDLER_NOEC(h_8,   T_IRQ0+8);
TRAPHANDLER_NOEC(h_9,   T_IRQ0+9);
TRAPHANDLER_NOEC(h_10,  T_IRQ0+10);
TRAPHANDLER_NOEC(h_11,  T_IRQ0+11);
TRAPHANDLER_NOEC(h_12,  T_IRQ0+12);
TRAPHANDLER_NOEC(h_13,  T_IRQ0+13);
TRAPHANDLER_NOEC(h_14,  T_IRQ0+14);
TRAPHANDLER_NOEC(h_15,  T_IRQ0+15);
 

/*
 * Lab 1: Your code here for _alltraps
 */
 
.type _alltraps, @function
_alltraps:
    cli		/* Disable interrupts */
    pushl	%ds			/* Here we're creating a struct trapframe in reverse */
    pushl 	%es			/* Earlier parts are already filled in by the macro and CPU */
    pushl 	%fs
    pushl	%gs
    pushal				/*pushes all (many?) register values*/
	movw	$CPU_GDT_KDATA, %ax
    movw	%ax, %ds
    movw	%ax, %es
    pushl	%esp	/*push a pointer to this trapframe as argument to trap*/
    call	trap


//
// Trap return code.
// C code in the kernel will call this function to return from a trap,
// providing the 
// Restore the CPU state from a given trapframe struct
// and return from the trap using the processor's 'iret' instruction.
// This function does not return to the caller,
// since the new CPU state this function loads
// replaces the caller's stack pointer and other registers.
//
.globl	trap_return
.type	trap_return,@function
.p2align 4, 0x90		/* 16-byte alignment, nop filled */
trap_return:
/*
 * Lab 1: Your code here for trap_return
 */
    popl	%eax		# 
    popl	%esp		#make esp point to the trapframe
    popal		#restore the states of registers
    popl		%gs	
    popl		%fs
    popl		%es
    popl		%ds
    addl	$8, %esp 	#get rid of trapno and errcode
    iret
