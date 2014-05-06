/*
 * Text-mode CGA/VGA display output device driver.
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

#ifndef PIOS_KERN_VIDEO_H_
#define PIOS_KERN_VIDEO_H_
#ifndef PIOS_KERNEL
# error "This is a kernel header; user programs should not #include it"
#endif

#include <inc/types.h>
#include <inc/x86.h>


#define MONO_BASE	0x3B4
#define MONO_BUF	0xB0000
#define CGA_BASE	0x3D4
#define CGA_BUF		0xB8000

#define CRT_ROWS	25
#define CRT_COLS	80
#define CRT_SIZE	(CRT_ROWS * CRT_COLS)


void video_init(void);
void video_putc(int c);

int delete_chars(int n);
int video_move_cursor(int n, int del);

void blk_left(void);
void blk_right(void);
void clear_line(void);
void to_begin(void);
void to_end(void);
void set_blk(uint16_t pos);

#endif /* PIOS_KERN_VIDEO_H_ */
