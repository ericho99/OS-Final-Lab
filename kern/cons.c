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

//struct cons_line *lines;
//struct cons_line *curr_line;

struct cons_hist *hist;
struct cons_hist *curr;
int line_start = 0;
int line_starts[256];
int line_no = 0;
int curr_line = 0;
int line_wpos;
int line_chars = 0;

#define LINE_MAX 1024
char line_buff[LINE_MAX];
char last_buff[LINE_MAX];
int char_pos = 0;
int line_len = 0;
int last_len = 0;

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

// called by device interrupt routines to feed input characters
// into the circular console input buffer.
void
cons_intr(int (*proc)(void))
{
	int c;

	spinlock_acquire(&cons_lock);
	fileinode * consin = &files->fi[FILEINO_CONSIN];
	while ((c = (*proc)()) != -1) {//cprintf("\nc = %d (%c)\n", c, c);
		//if (c == 228)
		//	cprintf("\n YOU PRESSED LEFT\n");
		//else if (c == 10)
		//	cprintf("\n YOU ENTERED U NUBCAKE\n");

		if (c == 8) {    // backspace
			// delete current char and shift everything to the left
			if (char_pos <= 0)
				break;

			line_len--;
			int i;
			for (i = char_pos - 1; i < line_len; i++) {
				line_buff[i] = line_buff[i + 1];
			}
			line_buff[line_len] = '\0';
			char_pos--;
			video_putc('\b');
			//cprintf("\n|%s|\n", line_buff);
			break;


			//cprintf("\npressed BACKSPACE\n");
			//((char*) FILEDATA(FILEINO_CONSIN))[--consin->size] = '\0';
			//iodone = 1;
			//last--;
			//consin->size--;
			//video_move_cursor(-1);

			//break;
		}

		if (c == 10 || c == '\n') {    // pressed enter
			//hist->start_pos = line_start;
			//hist->end_pos = consin->size;

			////cprintf("\nPOS = %d, TEXT = |%s|\n", hist->start_pos, ((char*) FILEDATA(FILEINO_CONSIN) + hist->start_pos));

			//struct cons_hist *next_hist;
			//next_hist->prev = hist;
			//hist->next = next_hist;

			////cprintf("\nhist->start_pos = %d\n", hist->next->prev->start_pos);

			//hist = next_hist;
			//curr = hist;

			////cprintf("\nhist->start_pos = %d\n", hist->prev->start_pos);

			//clear_line();
			//cons_clear_line();

			line_starts[line_no++] = line_start;
			curr_line = line_no;

			line_start += line_len + 1;
			line_starts[line_no] = line_start;

			line_wpos = cons.wpos + 1;
			line_chars = 0;
			//cprintf("\nsize = %d\n", strlen(cons.buf));
			//cprintf("\nBUF = |%s|\n", cons.buf);


			// write from temp buff to console buffer
			int color_input = false;
			int which_mask;
			int i;
			for (i = 0; i < NUM_COLORS; i++) {
				if (buf_strstr(cols[i], strlen(cols[i]))) {
					color_input = true;
					//which_mask = masks[i];
					color_mask = masks[i];
					break;
				}
			}

			if (!color_input) {
				cons_clear_line();
				for (i = 0; i < line_len; i++) {
					cons.buf[cons.wpos++] = line_buff[i];
				}
			}
			cons.buf[cons.wpos++] = '\n';    // send newline to terminate line

			//if (color_input) {
			//	for (i = 0; i < line_len; i++) {
			//		cons_putc(line_buff[i]);
			//	}
			//	color_mask = which_mask;
			//} else {
			//	for (i = 0; i < line_len; i++) {
			//		cons.buf[cons.wpos++] = line_buff[i];
			//	}
			//}
			//cons.buf[cons.wpos++] = '\n';    // send newline to terminate line

			//int i;
			//if (buf_strstr("blue",4)){
			//	color_mask = 0x0900;
			//}
			//else if (buf_strstr("white",5)){
			//	color_mask = 0x0700;
			//}
			//else if (buf_strstr("green",5)){
			//	color_mask = 0x0200;
			//}
			//else if (buf_strstr("cyan",4)){
			//	color_mask = 0x0300;
			//}
			//else if (buf_strstr("red",3)){
			//	color_mask = 0x0400;
			//}
			//else if (buf_strstr("magenta",7)){
			//	color_mask = 0x0500;
			//}
			//else if (buf_strstr("orange",6)){
			//	color_mask = 0x0600;
			//}
			//else if (buf_strstr("grey",4) || buf_strstr("gray",4)){
			//	color_mask = 0x0800;
			//}
			//else{
			////cprintf("leinlen = %d\n", line_len);
			//	for (i = 0; i < line_len; i++) {
			//		cons.buf[cons.wpos++] = line_buff[i];
			//		//panic("SHIT");
			//	}
			//}
			//cons.buf[cons.wpos++] = '\n';    // send newline to terminate line

			// reset temporary buffer
			line_len = 0;
			char_pos = 0;

			// break not necessary once code finished
			break;
		} else if (c == 226) {    // pressed up
			if (curr_line <= 0)
				break;

			if (curr_line == line_no) {
				//cprintf("\nCOPYING\n");
				//memmove(last_buff, line_buff, line_len);
				int i;
				for (i = 0; i < line_len; i++) {
					last_buff[i] = line_buff[i];
				}
				last_len = line_len;
			}

			//int j;
			//for (j = 0; j <= line_no; j++) {
			//	cprintf("line %d start = %d\n", j, line_starts[j]);
			//}

			curr_line--;

			cons_clear_line();

			//cprintf("CONSIN = |%s|\n", ((char*) FILEDATA(FILEINO_CONSIN)) + line_starts[curr_line]);

			int sz = line_starts[curr_line + 1] - line_starts[curr_line] - 1;
			//cprintf("sz = %d\n", sz);

			int index = line_starts[curr_line];
			int len = sz;
			int i;

			line_len = sz;
			//cons.wpos = line_wpos;
			//cons.wpos -= line_chars;
			//cons.wpos = 0;
			//cprintf("linelen = %d\n", line_len);
			for (i = 0; i < line_len; i++) {
				//cons.buf[cons.wpos++] = ((char*) FILEDATA(FILEINO_CONSIN))[index + i];
				//if (cons.wpos == CONSBUFSIZE)
				//	cons.wpos = 0;
				line_buff[i] = ((char*) FILEDATA(FILEINO_CONSIN))[index + i];
				cons_putc(line_buff[i]);
			}

			char_pos = line_len;

			//memmove((void *)line_start, ((char *)FILEDATA(FILEINO_CONSIN)) + line_starts[curr_line], sz);
			//consin->size = consin->size - line_start + sz;

			//curr = curr->prev;
			//cprintf("\nPOS = %d, TEXT = |%s|\n", curr->start_pos, ((char*) FILEDATA(FILEINO_CONSIN) + curr->start_pos));

			break;
		} else if (c == 227) {
			//cprintf("\n");
			//cprintf("line = %d\n", cons.wpos);
			//cprintf("wpos = %d\n", cons.wpos);
			//((char*) FILEDATA(FILEINO_CONSIN))[--consin->size] = '\0';
			//cons.buf[--cons.wpos] = '\0';
			//((char*) FILEDATA(FILEINO_CONSIN))[consin->size++] = 'Y';
			//cons.buf[cons.wpos++] = 'X';
			//cprintf("len1 = %d, len2 = %d\n", strlen(((char*) FILEDATA(FILEINO_CONSIN))), strlen(cons.buf));
			video_move_cursor(-3, true);
			break;
		}

		if (c == 228) {    // left
			// video_move_cursor(-1, false);
			blk_left();
			char_pos--;
			//pos_shift(-1);
			break;
		} else if (c == 229) {    // right
			// video_move_cursor(1, false);
			blk_right();
			char_pos++;
			//pos_shift(1);
			break;
		}

		if (c == 0)
			continue;
		
		//cons.buf[cons.wpos++] = c;
		//if (cons.wpos == CONSBUFSIZE)
		//	cons.wpos = 0;

		cons_putc(c);

		// update the temporary buffer, shifting everything to the right of the
		// current position one position, to simulate insertion
		line_len++;
		int i;
		for (i = line_len; i > char_pos; i--) {
			line_buff[i] = line_buff[i - 1];
		}
		line_buff[char_pos++] = c;

		line_chars++;

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
		//cprintf("CONSIN = |%s|\n", ((char*) FILEDATA(FILEINO_CONSIN)));
		iodone = 1;
	}
	//cprintf("DONE\n");
	
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

