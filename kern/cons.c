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

// keep track of where lines start in the input buffer
int line_start = 0;
int line_starts[256];
int line_no = 0;
int curr_line = 0;

// the temporary buffer for the latest line
#define LINE_MAX 1024
char line_buff[LINE_MAX];
char last_buff[LINE_MAX];
int char_pos = 0;
int line_len = 0;
int last_len = 0;

// information for changing colors
#define NUM_COLORS 9
char *cols[NUM_COLORS];    // array for colors & respective masks, initialize
int masks[NUM_COLORS];     // these values in cons_init()
int color_mask;    //Applied to change text color
			       //(see en.wikipedia.org/wiki/VGA-compatible_text_mode)

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


void
print_cons_line(cons_line *line)
{
	struct cons_char *curr_char = line->cons_text;
	while (curr_char != NULL) {
		cons_putc(curr_char->c);
		curr_char = curr_char->next;
	}
}

void
cons_writec(char c) {
	cons.buf[cons.wpos++] = c;
	if (cons.wpos == CONSBUFSIZE)
		cons.wpos = 0;
}

int
actual_len(char *arr, int len) {
	int i;
	int bs = 0;
	for (i = 0; i < len; i++) {
		bs += (arr[i] == '\b');
	}
	return (len - 2 * bs);
}

void
cons_clear_line(void) {
	int act_len = actual_len(line_buff, line_len);
	delete_chars(act_len);
}

void
pos_shift(int n) {
	if (n < 0) {
		while (n < 0) {
			if (line_buff[--char_pos] == '\b') {
				n--;
			} else {
				n++;
			}
		}
	} else {
		int i;
		int last_good;
		int last_complete;
		int cnt = 0;
		for (i = char_pos; i < line_len; i++) {
			if (line_buff[i] == '\b')
				cnt--;
			else
				cnt++;

			if (cnt == n && !last_good) {
				last_complete = i;
				last_good = true;
			} else if (cnt <= 0) {
				last_good = false;
			}
		}

		char_pos = last_good ? last_complete + 1 : line_len;
	}
}

int
buf_strstr(char *col, int len){
	int i;
	for (i=0;i<len;i++){
		if (i>line_len){
			return 0;
		}
		else if (line_buff[i] != col[i]){
			return 0;
		}
	}
	// if (line_buff[i] != col[i])
	return 1;
}

int
blank_line(int n) {
	int sz = line_starts[n + 1] - line_starts[n] - 1;
	int index = line_starts[n];
	int i;
	for (i = 0; i < sz; i++) {
		int c = ((char*) FILEDATA(FILEINO_CONSIN))[index + i];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\0') {
			return false;
		}
	}
	return true;
}

// called by device interrupt routines to feed input characters
// into the circular console input buffer.
void
cons_intr(int (*proc)(void))
{
	int c;

	spinlock_acquire(&cons_lock);
	fileinode * consin = &files->fi[FILEINO_CONSIN];
	while ((c = (*proc)()) != -1) {
		// cprintf("%d\n",c);
		if (c == 0) {    // null character, keep looking
			continue;
		} else if (c == '\b') {    // pressed backspace
			if (char_pos <= 0)
				break;

			line_len--;

			// delete current char and shift everything to the left
			int i;
			for (i = char_pos - 1; i < line_len; i++) {
				line_buff[i] = line_buff[i + 1];
			}
			line_buff[line_len] = '\0';
			char_pos--;

			// hackishly get the video to update by sending the backspace char
			video_putc('\b');

			break;
		} else if (c == '\n') {    // pressed enter

			// update previous line start
			line_starts[line_no++] = line_start;
			curr_line = line_no;

			// update current line start
			line_start += line_len + 1;
			line_starts[line_no] = line_start;

			// check for color change using cos & masks arrays
			int color_input = false;
			int which_mask;
			int i;
			for (i = 0; i < NUM_COLORS; i++) {
				if (buf_strstr(cols[i], strlen(cols[i]))) {
					color_input = true;
					color_mask = masks[i];
					break;
				}
			}

			// clear the line and write from temp buff to console buffer only if
			// the color was not changed, otherwise send the "blank" input
			if (color_input) {
				int curr_wpos = cons.wpos;
				for (i = 0; i < line_len; i++) {
					cons.buf[cons.wpos++] = ' ';
				}
			} else {
				cons_clear_line();
				for (i = 0; i < line_len; i++) {
					cons.buf[cons.wpos++] = line_buff[i];
				}
			}
			cons.buf[cons.wpos++] = '\n';    // send newline to terminate line

			// reset temporary buffer
			line_len = 0;
			char_pos = 0;

			break;
		} else if (c == 226) {    // pressed up
			int start_reached = false;
			do {
				if (curr_line <= 0) {
					start_reached = true;
					break;
				}

				// save the latest line
				if (curr_line == line_no) {
					int i;
					for (i = 0; i < line_len; i++) {
						last_buff[i] = line_buff[i];
					}
					last_len = line_len;
				}

				curr_line--;
			} while (blank_line(curr_line));

			if (start_reached)
				break;

			cons_clear_line();

			// load from history to temp buffer & write temp buffer
			line_len = line_starts[curr_line + 1] - line_starts[curr_line] - 1;
			int index = line_starts[curr_line];
			int i;
			for (i = 0; i < line_len; i++) {
				line_buff[i] = ((char*) FILEDATA(FILEINO_CONSIN))[index + i];
				cons_putc(line_buff[i]);
			}

			char_pos = line_len;

			break;
		} else if (c == 227) {    // pressed down
			if (curr_line >= line_no)
				break;

			int peek_line = curr_line + 1;
			while (peek_line < line_no && blank_line(peek_line)) {
				curr_line++;
				peek_line = curr_line + 1;
			}

			curr_line++;

			cons_clear_line();

			if (curr_line == line_no) {
				// load the saved latest line
				int i;
				for (i = 0; i < last_len; i++) {
					line_buff[i] = last_buff[i];
				}
				line_len = last_len;
			} else {
				// load from history
				line_len = line_starts[curr_line + 1] - line_starts[curr_line] - 1;
				int index = line_starts[curr_line];
				int i;
				for (i = 0; i < line_len; i++) {
					line_buff[i] = ((char*) FILEDATA(FILEINO_CONSIN))[index + i];
				}
			}

			// write the temporary buffer
			int i;
			for (i = 0; i < line_len; i++) {
				cons_putc(line_buff[i]);
			}

			char_pos = line_len;

			break;
		} else if (c == 228) {    // pressed left
			blk_left();
			char_pos--;
			break;
		} else if (c == 229) {    // pressed right
			blk_right();
			char_pos++;
			break;
		} else if (c == 1) {    // pressed crtl+A
			to_begin();
			char_pos = line_start;
			break;
		} else if (c == 5 || c == 225){		// pressed crtl+E
			to_end();
			char_pos = line_len;
			break;
		}
		
		cons_putc(c);

		// update the temporary buffer, shifting everything to the right of the
		// current position one position, to simulate insertion
		line_len++;
		int i;
		for (i = line_len; i > char_pos; i--) {
			line_buff[i] = line_buff[i - 1];
		}
		line_buff[char_pos++] = c;
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

	color_mask = 0x0700;

	// initialize the color & mask arrays
	cols[0] = "blue";
	masks[0] = 0x0900;
	cols[1] = "white";
	masks[1] = 0x0700;
	cols[2] = "green";
	masks[2] = 0x0200;
	cols[3] = "cyan";
	masks[3] = 0x0300;
	cols[4] = "red";
	masks[4] = 0x0400;
	cols[5] = "magenta";
	masks[5] = 0x0500;
	cols[6] = "orange";
	masks[6] = 0x0600;
	cols[7] = "gray";
	masks[7] = 0x0800;
	cols[8] = "grey";
	masks[8] = 0x0800;

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

