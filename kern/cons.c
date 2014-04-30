/*
 * Main console driver for PIOS, which manages lower-level console devices
 * such as video (dev/video.*), keyboard (dev/kbd.*), and serial (dev/serial.*)
 *
 * Copyright (c) 2010 Yale University.
 * Copyright (c) 1993, 1994, 1995 Charles Hannum.
 * Copyright (c) 1990 The Regents of the University of California.
 * See section "BSD License" in the file LICENSES for licensing terms.
 *
 * This code is derived from the NetBSD pcons driver, and in turn derived
 * from software contributed to Berkeley by William Jolitz and Don Ahn.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#include <inc/stdio.h>
#include <inc/stdarg.h>
#include <inc/x86.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/syscall.h>

#include <kern/cpu.h>
#include <kern/cons.h>
#include <kern/mem.h>
#include <kern/spinlock.h>
#include <kern/file.h>

#include <dev/video.h>
#include <dev/kbd.h>
#include <dev/serial.h>

void cons_intr(int (*proc)(void));
static void cons_putc(int c);

int last = 0;

spinlock cons_lock;	// Spinlock to make console output atomic

/***** General device-independent console code *****/
// Here we manage the console input buffer,
// where we stash characters received from the keyboard or serial port
// whenever the corresponding interrupt occurs.

#define CONSBUFSIZE 512

static struct {
	uint8_t buf[CONSBUFSIZE];
	uint32_t rpos;
	uint32_t wpos;
} cons;


// called by device interrupt routines to feed input characters
// into the circular console input buffer.
void
cons_intr(int (*proc)(void))
{
	int c;

	spinlock_acquire(&cons_lock);
	while ((c = (*proc)()) != -1) {
		if (c == 0)
			continue;
		cons.buf[cons.wpos++] = c;
		if (cons.wpos == CONSBUFSIZE)
			cons.wpos = 0;
	}
	spinlock_release(&cons_lock);

	// Wake the root process
	file_wakeroot();
}

// return the next input character from the console, or 0 if none waiting
int
cons_getc(void)
{
	int c;

	// poll for any pending input characters,
	// so that this function works even when interrupts are disabled
	// (e.g., when called from the kernel monitor).
	serial_intr();
	kbd_intr();

	// grab the next character from the input buffer.
	if (cons.rpos != cons.wpos) {
		c = cons.buf[cons.rpos++];
		if (cons.rpos == CONSBUFSIZE)
			cons.rpos = 0;
		return c;
	}
	return 0;
}


static int esc_flag = 0; //Flag which designates whether the last char was ESC
static int color_mask = 0x0700; //Applied to change text color
			//(see en.wikipedia.org/wiki/VGA-compatible_text_mode)

// output a character to the console
// ESC character (ascii 27) is ignored; char after it is set as color mask
static void
cons_putc(int c)
{
	
	if (esc_flag > 0){
	    color_mask = c << 8;
	    esc_flag = 0;
	    return;
	}
	if (c == 27){
	  esc_flag = 1;
	  return;
	}
	c |= color_mask;
	
	
	serial_putc(c);
	video_putc(c);
}

// initialize the console devices
void
cons_init(void)
{
	if (!cpu_onboot())	// only do once, on the boot CPU
		return;

	spinlock_init(&cons_lock);
	video_init();
	kbd_init();
	serial_init();

	if (!serial_exists)
		warn("Serial port does not exist!\n");
}

// Enable console interrupts.
void
cons_intenable(void)
{
	if (!cpu_onboot())	// only do once, on the boot CPU
		return;

	kbd_intenable();
	serial_intenable();
}

// `High'-level console I/O.  Used by readline and cprintf.
void
cputs(const char *str)
{
	if (read_cs() & 3)
		return sys_cputs(str);	// use syscall from user mode

	// Hold the console spinlock while printing the entire string,
	// so that the output of different cputs calls won't get mixed.
	// Implement ad hoc recursive locking for debugging convenience.
	bool already = spinlock_holding(&cons_lock);
	if (!already)
		spinlock_acquire(&cons_lock);

	char ch;
	while (*str)
		cons_putc(*str++);

	if (!already)
		spinlock_release(&cons_lock);
}

// Synchronize the root process's console special files
// with the actual console I/O device.
bool
cons_io(void)
{
	// Lab 4: your console I/O code here.
	int c;
	int iodone = 0;
	
	// Input
	fileinode * consin = &files->fi[FILEINO_CONSIN];
	while((c = cons_getc())){
		((char*) FILEDATA(FILEINO_CONSIN))[consin->size++] = c;
		iodone = 1;
	}
	
	// Output
	fileinode * consout = &files->fi[FILEINO_CONSOUT];
	int i = last;
	spinlock_acquire(&cons_lock);
	for(i; i < consout->size; i++){
		c = ((char*) FILEDATA(FILEINO_CONSOUT))[i];
		cons_putc(c);
		iodone = 1;
	}
	spinlock_release(&cons_lock);
	last = i;

	return iodone;	// 0 indicates no I/O done
}

