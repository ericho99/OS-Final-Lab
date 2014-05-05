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
#include <inc/string.h>
#include <kern/mem.h>
#include <dev/video.h>
static unsigned addr_6845;
static uint16_t *crt_buf;
static uint16_t crt_pos;
static uint16_t blk_pos;
static uint16_t line_pos;
void
video_init(void)
{
	volatile uint16_t *cp;
	uint16_t was;
	unsigned pos;
	/* Get a pointer to the memory-mapped text display buffer. */
	cp = (uint16_t*) mem_ptr(CGA_BUF);
	was = *cp;
	*cp = (uint16_t) 0xA55A;
	if (*cp != 0xA55A) {
		cp = (uint16_t*) mem_ptr(MONO_BUF);
		addr_6845 = MONO_BASE;
	} else {
		*cp = was;
		addr_6845 = CGA_BASE;
	}
	
	/* Extract cursor location */
	outb(addr_6845, 14);
	pos = inb(addr_6845 + 1) << 8;
	outb(addr_6845, 15);
	pos |= inb(addr_6845 + 1);
	crt_buf = (uint16_t*) cp;
	crt_pos = pos;
	blk_pos = pos;
	line_pos = 1442;
}
void
video_putc(int c)
{
	int i,temp,temp2;
	// if no attribute given, then use black on white
	if (!(c & ~0xFF))
		c |= 0x0700;
	switch (c & 0xff) {
	case '\b':
		if (blk_pos > line_pos){
			crt_buf[blk_pos-1] = (c & ~0xff) | ' ';
			for (i=blk_pos-1;i<crt_pos;++i){
				crt_buf[i] = crt_buf[i+1];
			}
			blk_pos--;
			crt_pos--;
		}
		break;
	case '\n':
		crt_pos += CRT_COLS;
		blk_pos = crt_pos;
		line_pos = crt_pos + 2;
		/* fallthru */
	case '\r':
		// blk_right();
		crt_pos -= (crt_pos % CRT_COLS);
		blk_pos = crt_pos;
		line_pos = crt_pos + 2;
		break;
	case '\t':
		video_putc(' ');
		video_putc(' ');
		video_putc(' ');
		video_putc(' ');
		video_putc(' ');
		blk_pos = crt_pos;
		break;
	default:
		// crt_buf[crt_pos++] = c;		 write the character 
		// blk_pos++;
		// break;
		// basically just shifts all the characters in the buffer that are to the right of the blinker position to the right
		temp = crt_buf[blk_pos];
		crt_buf[blk_pos] = c;
		for (i=blk_pos+1;i<crt_pos+1;i++){
			temp2 = crt_buf[i];
			crt_buf[i] = temp;
			temp = temp2;
		}
		crt_pos++;
		blk_pos++;
		break;
	}
	// the following just resizes the window
	if (crt_pos >= CRT_SIZE) {
		int i;
		memmove(crt_buf, crt_buf + CRT_COLS,
			(CRT_SIZE - CRT_COLS) * sizeof(uint16_t));
		for (i = CRT_SIZE - CRT_COLS; i < CRT_SIZE; i++)
			crt_buf[i] = 0x0700 | ' ';
		crt_pos -= CRT_COLS;
		blk_pos = crt_pos;
		line_pos = crt_pos + 2;
	}
	/* move that little blinky thing */
	// outb(addr_6845, 14);
	// outb(addr_6845 + 1, crt_pos >> 8);
	// outb(addr_6845, 15);
	// outb(addr_6845 + 1, crt_pos);
	outb(addr_6845, 14);
	outb(addr_6845 + 1, blk_pos >> 8);
	outb(addr_6845, 15);
	outb(addr_6845 + 1, blk_pos);
}

void blk_left(){
	if (blk_pos > line_pos){
		blk_pos--;
		outb(addr_6845, 14);
		outb(addr_6845 + 1, blk_pos >> 8);
		outb(addr_6845, 15);
		outb(addr_6845 + 1, blk_pos);
	}
}

void blk_right(){
	if (blk_pos < crt_pos){
		blk_pos++;
		outb(addr_6845, 14);
		outb(addr_6845 + 1, blk_pos >> 8);
		outb(addr_6845, 15);
		outb(addr_6845 + 1, blk_pos);
	}
}

void clear_line(){
	while(crt_pos > line_pos){
		crt_buf[--crt_pos] = ('\b' & ~0xff) | ' ';
		//crt_pos--;
	}
	blk_pos = crt_pos;
}

int
delete_chars(int n) {
	int i;
	for (i = 0; i < n; i++) {
		crt_buf[--crt_pos] = ('\b' & ~0xff) | ' ';
	}
	blk_pos = crt_pos;

	return n;
}

void
set_cursor_pos(uint16_t pos) {
	outb(addr_6845, 14);
	outb(addr_6845 + 1, pos >> 8);
	outb(addr_6845, 15);
	outb(addr_6845 + 1, pos);
}

int
video_move_cursor(int n, int del) {
	int new_pos = blk_pos + n;
	//cprintf("here b4\n");

	//cprintf("blkpos = %d, linepos = %d, crtpos = %d\n", blk_pos, line_pos, crt_pos);

	//if (blk_pos < line_pos || blk_pos > crt_pos)
	//	return 0;

	// check to make sure that the new position is valid
	//if (!(new_pos >= line_pos && new_pos <= crt_pos))
	//	return 0;
	
	//cprintf("here\n");

	if (del) {
		int i;
		if (n < 0) {
			for (i = 0; i < -n; i++) {
				uint16_t del_pos = blk_pos + i;
				uint16_t rep_pos = new_pos + i;
				int rep_char = (del_pos <= crt_pos) ? crt_buf[del_pos] : 'X';
				rep_char = ':';
				crt_buf[rep_pos] = rep_char;
				//video_putc(rep_char);
			}
			blk_pos = new_pos;
		}
		//uint16_t st_pos = (n < 0) ? new_pos : blk_pos;
		//int pos_n = (n < 0) ? -n : n;
		//for (i = 0; i < pos_n; i++) {
		//	uint16_t del_pos = blk_pos + i;
		//	uint16_t rep_pos = st_pos + i;
		//	//crt_buf[rep_pos] =  (rep_pos <= crt_pos) ? crt_buf[rep_pos] : 'X';
		//	crt_buf[rep_pos] = 'A';
		//	//cprintf("YO");
		//}
		//blk_pos = new_pos;
	} else {
		blk_pos = new_pos;
	}

	set_cursor_pos(blk_pos);

	return 1;
}
