/* bkerndev - Bran's Kernel Development Tutorial
*  By:   Brandon F. (friesenb@gmail.com)
*  Desc: Screen output functions for Console I/O
*
*  Notes: No warranty expressed or implied. Use at own risk. */
#include <system.h>
#include <stdarg.h>
#include <string.h>
#include <mm.h>
#include <vmm.h>
#include <fbcon.h>
#include <hardware.h>

/* Physical address of the VGA text mode buffer. Memory mapped hardware, not
*  RAM -- the kernel reaches it through the direct mapping at P2V(). */
#define VGA_TEXT_PHYS   0xB8000

/* The geometry of the VGA text mode buffer. These are properties of the
*  hardware behind 0xB8000 and never change; the geometry of the CONSOLE is a
*  different question and is asked through screen_cols()/screen_rows(). */
#define TEXT_COLS       80
#define TEXT_ROWS       25

/* These define our textpointer, our background and foreground
*  colors (attributes), and x and y cursor coordinates */
volatile unsigned short *textmemptr;
int attrib = 0x0F;
int csr_x = 0, csr_y = 0;

/* --- Screen geometry ------------------------------------------------------
*
*  Text mode is 80x25 and always was, so the numbers used to be written into
*  the code. A framebuffer console is whatever the mode the bootloader picked
*  works out to -- 1024x768 with an 8x16 cell is 128x48 -- so nothing may
*  assume 80 or 25 any more. Everything that needs the size of the screen
*  asks these two, and they answer for whichever console is in charge.
*
*  Both are cheap enough to call per character: fbcon_active() reads one
*  variable, and when it is false neither fbcon_cols() nor fbcon_rows() is
*  reached at all. */
static int screen_cols(void)
{
	if(fbcon_active()) return fbcon_cols();
	return TEXT_COLS;
}

static int screen_rows(void)
{
	if(fbcon_active()) return fbcon_rows();
	return TEXT_ROWS;
}

/* --- The console lock -----------------------------------------------------
*
*  The screen is shared: the console task writes through putch(), the status
*  bar task writes into row 0 directly, and any ring 3 task can reach it
*  through SYS_WRITE. All of them touch the same csr_x/csr_y/attrib and the
*  same VGA buffer, and none of that is atomic.
*
*  The visible symptom was duplicated lines. scroll() moves 24 rows with a
*  memcpy; a timer tick landing inside it lets the status bar task write row
*  0, and when the memcpy resumes it copies over a screen that has changed
*  underneath it. The same race scrambles the cursor when two tasks print at
*  once, which became reachable the moment two ring 3 tasks could each call
*  SYS_WRITE.
*
*  On a single processor, masking interrupts is the lock: nothing else can
*  run while they are off. Saving EFLAGS rather than ending with sti makes
*  it safe to nest -- printf() takes it, calls puts(), which calls putch(),
*  and each inner release restores "still masked" instead of switching
*  interrupts back on halfway through a line.
*
*  Held only for the length of one output call, which is microseconds; long
*  enough to matter would mean a single printf() of many hundreds of
*  characters, and there is none.
*
*  It protects the static scratch buffers on the way, too: itoa() and
*  hextoa() in str.c hand back pointers into one static array each, so two
*  tasks formatting a number at once would otherwise read each other's
*  digits. */
static unsigned int console_lock(void)
{
	unsigned int flags;
	__asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
	return flags;
}

static void console_unlock(unsigned int flags)
{
	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

/* Scrolls the screen */
void scroll(void)
{
    unsigned blank, temp;
    int rows;

    /* A blank is defined as a space... we need to give it
    *  backcolor too */
    blank = 0x20 | (attrib << 8);

    /* The last row is the end, this means we need to scroll up. Which row
    *  that is depends on the console: 25 in text mode, whatever the
    *  framebuffer mode works out to otherwise. */
    rows = screen_rows();
    if(csr_y < rows) return;

    if(fbcon_active())
    {
        /* The framebuffer console scrolls its own shadow buffer and repaints
        *  as it sees fit; it moves one line per call, so a cursor that ran
        *  several lines past the bottom takes several. In practice it is
        *  never more than one. */
        while(csr_y >= rows)
        {
            fbcon_scroll(attrib);
            csr_y--;
        }
        return;
    }

    /* Move the current text chunk that makes up the screen
    *  back in the buffer by a line */
    temp = csr_y - rows + 1;
    memcpy ((void *)textmemptr, (const void *)(textmemptr + temp * TEXT_COLS), (rows - temp) * TEXT_COLS * 2);

    /* Finally, we set the chunk of memory that occupies
    *  the last line of text to our 'blank' character */
    memsetw ((unsigned short *)(textmemptr + (rows - temp) * TEXT_COLS), blank, TEXT_COLS);
    csr_y = rows - 1;
}

/* Updates the hardware cursor: the little blinking line
*  on the screen under the last character pressed! */
void move_csr(void)
{
    unsigned temp;

    /* There is no CRT controller in a graphics mode and no hardware cursor to
    *  program: the framebuffer console draws its own. */
    if(fbcon_active())
    {
        fbcon_cursor(csr_x, csr_y);
        return;
    }

    /* The equation for finding the index in a linear
    *  chunk of memory can be represented by:
    *  Index = [(y * width) + x] */
    temp = csr_y * TEXT_COLS + csr_x;

    /* This sends a command to indicies 14 and 15 in the
    *  CRT Control Register of the VGA controller. These
    *  are the high and low bytes of the index that show
    *  where the hardware cursor is to be 'blinking'. To
    *  learn more, you should look up some VGA specific
    *  programming documents. A great start to graphics:
    *  http://www.brackeen.com/home/vga */
    outportb(0x3D4, 14);
    outportb(0x3D5, temp >> 8);
    outportb(0x3D4, 15);
    outportb(0x3D5, temp);
	
}

/* Clears the screen */
void cls(void)
{
    unsigned blank;
    int i;
    unsigned int flags;
    int printf(char * string, ...);

    flags = console_lock();

    /* Again, we need the 'short' that will be used to
    *  represent a space with color */
    blank = 0x20 | (attrib << 8);

    /* Sets the entire screen to spaces in our current
    *  color */
    if(fbcon_active())
    {
        fbcon_clear(attrib);
    }
    else
    {
        for(i = 0; i < TEXT_ROWS; i++)
            memsetw ((unsigned short *)(textmemptr + i * TEXT_COLS), blank, TEXT_COLS);
    }

    /* Update out virtual cursor, and then move the
    *  hardware cursor */
    csr_x = 0;
    csr_y = 0;
    move_csr();

    console_unlock(flags);
}

/* --- The serial mirror ----------------------------------------------------
*
*  Everything the kernel prints also leaves through COM1, and this is the one
*  line that does it. It is here and nowhere else because putch() is the one
*  place every character passes through, exactly once:
*
*      printf() -> puts() -> putch()   every conversion, via puts()
*      printf() -> putch()             %c and %%, and the CP437 umlauts
*      puts()   -> putch()             a string, one character at a time
*      SYS_WRITE -> putch()            a ring 3 program's string, the same way
*      SYS_PUTCH -> putch()            a ring 3 program's single character
*      keyboard  -> putch()            the echo of what was typed
*      panic()  -> printf() -> ...     the last thing a dying kernel says
*
*  and the framebuffer console does NOT bypass it: fbcon_putc() is not a
*  console entry point but the back end putch() calls instead of writing into
*  the VGA text buffer when a graphics mode is in charge. Outside this file
*  nothing calls it at all. So one mirror here catches the text console and
*  the framebuffer console both, and catches each character once - putting it
*  in puts() as well would double every string, and putting it in fbcon.c
*  would lose every line the kernel prints before the framebuffer exists,
*  which is most of the boot.
*
*  The status bar is the deliberate exception, and it is an exception because
*  it does not come through here: display_update_statusbar() writes row 0
*  through statusbar_cell(). That is the right outcome - the bar repaints a
*  clock once a second forever, and mirroring it would bury the log under it.
*  cls() does not come through here either, so clearing the screen does not
*  clear the log. A log is a record of what was said, not a picture of the
*  screen as it stands.
*
*  It goes first, before the cursor arithmetic and before anything is drawn:
*  the character is then in the log even if what follows faults, which is the
*  situation in which one most wants to know what the last thing printed was.
*
*  The cost is a bounded wait for the UART, taken with interrupts masked
*  because putch() already holds the console lock. See SERIAL_TX_SPINS in
*  src/kernel/hardware.c for the bound and for what happens when it runs out.
*  On a machine with no serial port at all - a T430, say - the port is probed
*  once, found absent, and every character after that costs one compare.
*
*  There is no switch to turn this off, and that is a decision rather than an
*  omission. A run time flag could only be flipped by a shell command, and the
*  boot messages - the ones an automated test actually needs, and the ones a
*  machine that dies before the prompt will only ever produce - are all
*  printed before any shell exists to flip it. A flag would therefore be able
*  to disable the case that does not matter and not the case that does. The
*  one switch worth having is the one the hardware throws itself: no UART, no
*  output, no cost. */

/* Puts a single character on the screen */
void putch(unsigned char c)
{
    volatile unsigned short *where;
    unsigned att;
    unsigned int flags;
    int cols;

    flags = console_lock();

    serial_console_putc(c);

    att = attrib << 8;
    cols = screen_cols();

    /* Handle a backspace, by moving the cursor back one space */
    if(c == 0x08)
    {
        if(csr_x != 0) csr_x--;
    }
    /* Handles a tab by incrementing the cursor's x, but only
    *  to a point that will make it divisible by 8 */
    else if(c == 0x09)
    {
        csr_x = (csr_x + 8) & ~(8 - 1);
    }
    /* Handles a 'Carriage Return', which simply brings the
    *  cursor back to the margin */
    else if(c == '\r')
    {
        csr_x = 0;
    }
    /* We handle our newlines the way DOS and the BIOS do: we
    *  treat it as if a 'CR' was also there, so we bring the
    *  cursor to the margin and we increment the 'y' value */
    else if(c == '\n')
    {
        csr_x = 0;
        csr_y++;
    }
    /* Any character greater than and including a space, is a
    *  printable character. The equation for finding the index
    *  in a linear chunk of memory can be represented by:
    *  Index = [(y * width) + x] */
    else if(c >= ' ')
    {
        /* The framebuffer console wants a column and a row rather than an
        *  offset into a linear buffer -- it has to know the cell to be able
        *  to draw the glyph. */
        if(fbcon_active())
        {
            fbcon_putc(csr_x, csr_y, c, attrib);
        }
        else
        {
            where = textmemptr + (csr_y * TEXT_COLS + csr_x);
            *where = c | att;	/* Character AND attributes: color */
        }
        csr_x++;
    }

    /* If the cursor has reached the edge of the screen's width, we
    *  insert a new line in there */
    if(csr_x >= cols)
    {
        csr_x = 0;
        csr_y++;
    }

    /* Scroll the screen if needed, and finally move the cursor */
    scroll();
    move_csr();

    console_unlock(flags);
}

/* Uses the above routine to output a string... */
void puts(char *text)
{
    int i;
    int len;
    unsigned int flags;

    /* Taken around the whole string, not just each character: otherwise a
    *  second task can slip its own output between two letters of this one.
    *  putch() takes the lock again inside, which is why saving EFLAGS
    *  rather than ending with sti matters. */
    flags = console_lock();
    len = (int)strlen(text);
    for (i = 0; i < len; i++)
    {
        putch(text[i]);
    }
    console_unlock(flags);
}

/* Sets the forecolor and backcolor that we will use */
void settextcolor(unsigned char forecolor, unsigned char backcolor)
{
    /* Top 4 bytes are the background, bottom 4 bytes
    *  are the foreground color */
    attrib = (backcolor << 4) | (forecolor & 0x0F);
}

/* Sets our text-mode VGA pointer, then clears the screen for us.
*  The kernel is linked for the higher half, so the buffer is not addressed
*  by its physical address any more but through the direct mapping. Every
*  other function in this file works through textmemptr and follows along
*  by itself. */
void init_video(void)
{
    textmemptr = (volatile unsigned short *)P2V(VGA_TEXT_PHYS);
    cls();
}

int printf(char * string, ...)
{
	int i;
  int stringlen;
  int tmp;
  unsigned char uc;
  unsigned char uc2;
  unsigned int flags;

	va_list argptr;

	/* One printf() is one unit of output. Without this, a timer tick between
	*  two conversions lets another task print into the middle of the line -
	*  and the static buffers itoa() and hextoa() return would be shared
	*  while both are mid-format. */
	flags = console_lock();
	va_start(argptr, string);
		
	stringlen = (int)strlen(string);
	
	for(i=0; i <= stringlen; i++)
  {
    if(string[i] == '%')
      {
        /* Controlcharacter. Check indice */
        switch(string[i+1])
        {
          case 's':
            puts(va_arg(argptr, char *));
            i++;
            break;
          case 'u':
		  case 'd':
          case 'i':
            tmp = va_arg(argptr, int);
            if(tmp <0)
            {
              printf("-");
              tmp = tmp *(-1);
            }
            puts(itoa(tmp));
            i++;
            break;
          case 'c':
            /* int, not char: everything smaller than an int is promoted to
            *  one on its way into a "..." argument list, so there is no char
            *  on the stack to fetch. The old DJGPP stdarg.h rounded every
            *  size up to sizeof(int) and hid this; the compiler's own
            *  builtins do not, and are right to say so. */
            putch((char)va_arg(argptr, int));
            i++;
            break;
          case 'p':
            puts(itoa(va_arg(argptr, int)));
            i++;
            break;
          case 'X':
            hextoa(va_arg(argptr, int));
            i++;
            break;
          case '%':
            putch('%');
            i++;
            break;

          default:
            i++;
            break;
        }
      }
    else
      {
        /* Umlauts are UTF-8 encoded: they all start with the lead
        *  byte 0xC3, the following byte selects the character.
        *  We translate them to their CP437 codes. */
        uc = (unsigned char)string[i];
        if(uc == 0xC3 && i < stringlen)
          {
            uc2 = (unsigned char)string[i+1];
            switch(uc2)
            {
              case 0x84: /* AE */
                putch(0x8E);
                i++;
                break;
              case 0xA4: /* ae */
                putch(0x84);
                i++;
                break;
              case 0x96: /* OE */
                putch(0x99);
                i++;
                break;
              case 0xB6: /* oe */
                putch(0x94);
                i++;
                break;
              case 0x9C: /* UE */
                putch(0x9A);
                i++;
                break;
              case 0xBC: /* ue */
                putch(0x81);
                i++;
                break;
              case 0x9F: /* sz */
                putch(0xE1);
                i++;
                break;

              default:
                /* Unknown sequence: emit the lead byte as-is and let
                *  the next iteration handle the following byte. */
                putch(uc);
                break;
            }
          }
        else
          {
            putch(string[i]);
          }
      }
  }

	va_end(argptr);

	console_unlock(flags);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Status bar                                                          */
/* ------------------------------------------------------------------ */

/* The bar is the topmost line of the screen and therefore exactly as wide as
*  the screen is -- 80 columns in text mode, 128 in a 1024x768 framebuffer.
*  There is no STATUSBAR_WIDTH any more: the width is read once per update
*  from screen_cols() and carried through in a local, so that every column in
*  one line is measured against the same number.
*
*  The clock is right aligned: "hh:mm:ss" occupies eight columns ending two
*  columns short of the right edge -- those two stay empty (they used to hold
*  the terminating zero of the time string, which showed up as a space). */
#define STATUSBAR_CLOCK_LEN 8
#define STATUSBAR_CLOCK_COL(width) ((width) - 2 - STATUSBAR_CLOCK_LEN)

/* Frames per megabyte: 1 MiB / 4 KiB = 256. The display deliberately counts
*  in frames rather than in bytes -- a byte value would overflow at 4 GiB,
*  while the number of frames always fits comfortably into 32 bits. */
#define STATUSBAR_FRAMES_PER_MB (1024 * 1024 / PMM_FRAME_SIZE)

/* Writes one cell of the bar with the currently set attrib. The bar is row 0
*  in either mode, so this is the one place that has to know which console is
*  in charge -- everything above it works in columns and stays the same. */
static void statusbar_cell(int col, unsigned char c)
{
	volatile unsigned short *pos;

	if(fbcon_active())
	{
		fbcon_putc(col, 0, c, attrib);
		return;
	}

	pos = textmemptr + col;
	*pos = c | (attrib << 8);
}

/* Writes text with the currently set attrib into the bar starting at column
*  col and returns the first free column. Anything that would run into the
*  area of the clock is discarded: that way neither a long number nor a high
*  task count can blow up the line or overflow into the next one. The limit
*  is passed in rather than looked up, because on a framebuffer it depends on
*  a screen width the caller has already determined. */
static int statusbar_puts(int col, int limit, const char *text)
{
	while(*text != EOS && col < limit)
	{
		statusbar_cell(col, (unsigned char)*text);
		col++;
		text++;
	}

	return col;
}

/* Like statusbar_puts(), but for a single character -- needed for the
*  activity dot 0xFE, which does not appear in any ordinary string. */
static int statusbar_putc(int col, int limit, unsigned char c)
{
	if(col < limit)
	{
		statusbar_cell(col, c);
		col++;
	}

	return col;
}

void display_update_statusbar()
{
	unsigned int flags;
	int org_attrib;
	int i;
	int x;
	int back = 0x7;
	int width;
	int clock_col;
	int taskcount;
	uint32_t total_frames;
	uint32_t used_frames;
	datetime now;
	char mem[24];
	char time[9] = "00:00:00";

	/* Fetch the time before the critical section: cmos_readtime() may wait
	*  for an RTC update to finish and already guards the interrupts itself.
	*  Waiting inside our own cli would hold the lock unnecessarily long. */
	now = cmos_readtime();

	/* The very same lock putch() takes, for the very same reason: this runs
	*  as its own task, and everything below touches state the console task
	*  and any ring 3 task using SYS_WRITE touch as well -- the global attrib,
	*  the static buffer itoa() hands back, and row 0 of whichever console is
	*  active. On a framebuffer that last one now means the shadow buffer and
	*  the pixels behind it, which putch() writes through too, so the lock
	*  matters more here than it did in text mode, not less. It saves EFLAGS
	*  rather than ending with sti, so a caller that had deliberately masked
	*  the interrupts gets them back masked. */
	flags = console_lock();

	org_attrib = attrib;

	/* The width of the bar is the width of the screen. Read it once: every
	*  column below is measured against it, and they all have to agree. */
	width = screen_cols();
	clock_col = STATUSBAR_CLOCK_COL(width);
	if(clock_col < 0) clock_col = 0;

	/* The metrics and the number formatting belong inside the critical
	*  section: itoa() returns a pointer to a static buffer that all tasks
	*  share. */
	taskcount = taskmgr_get_taskcount();
	total_frames = pmm_total_frames();
	used_frames = pmm_used_frames();

	if(total_frames == 0)
	{
		/* Memory management not set up yet -- more honest than a made up
		*  number. */
		strcpy(mem, "n/a");
	}
	else
	{
		/* The used amount is rounded up so that a small but existing usage
		*  does not show up as "0"; the total size is rounded down so that
		*  the bar never promises more memory than is really there. */
		strcpy(mem, itoa((int)((used_frames + STATUSBAR_FRAMES_PER_MB - 1)
		                       / STATUSBAR_FRAMES_PER_MB)));
		strcat(mem, "/");
		/* Only now may itoa() run again: the static buffer holding the
		*  first value has been copied away above. */
		strcat(mem, itoa((int)(total_frames / STATUSBAR_FRAMES_PER_MB)));
		strcat(mem, "mb");
	}

	/* Background of the whole line: black text on light grey. */
	settextcolor(0x00, 0x7);
	for(i = 0; i < width; i++)
	{
		statusbar_cell(i, ' ');
	}

	i = 0;

	/* [cpu: .] -- brackets black, label white */
	i = statusbar_puts(i, clock_col, "[");
	settextcolor(15, back);
	i = statusbar_puts(i, clock_col, "cpu: ");
	i = statusbar_putc(i, clock_col, 0xFE);	/* activity dot, CPU */
	settextcolor(0x00, 0x7);
	i = statusbar_puts(i, clock_col, "] ");

	/* [mem: used/total mb] */
	i = statusbar_puts(i, clock_col, "[");
	settextcolor(15, back);
	i = statusbar_puts(i, clock_col, "mem: ");
	i = statusbar_puts(i, clock_col, mem);
	settextcolor(0x00, 0x7);
	i = statusbar_puts(i, clock_col, "] ");

	/* [task: ..] -- one dot per running task */
	i = statusbar_puts(i, clock_col, "[");
	settextcolor(15, back);
	i = statusbar_puts(i, clock_col, "task: ");
	for(x = 0; x < taskcount; x++)
	{
		i = statusbar_putc(i, clock_col, 0xFE);
	}
	settextcolor(0x00, 0x7);
	i = statusbar_puts(i, clock_col, "]");

	/* Clock, right aligned. attrib is already set to black/light grey. */
	if(now.hours < 10) {
		time[1] = now.hours + '0';
	} else {
		time[0] = (int)(now.hours) / 10 + '0';
		time[1] = now.hours % 10 + '0';
	}

	if(now.minutes < 10) {
		time[4] = now.minutes + '0';
	} else {
		time[3] = (int)(now.minutes) / 10 + '0';
		time[4] = now.minutes % 10 + '0';
	}

	if(now.seconds < 10) {
		time[7] = now.seconds + '0';
	} else {
		time[6] = (int)(now.seconds) / 10 + '0';
		time[7] = now.seconds % 10 + '0';
	}

	for(x = 0; x < STATUSBAR_CLOCK_LEN; x++)
	{
		statusbar_cell(clock_col + x, (unsigned char)time[x]);
	}

	attrib = org_attrib;

	/* EFLAGS back -- afterwards the interrupts are disabled or enabled
	*  exactly as they were on entry. */
	console_unlock(flags);
}

void panic(char *desc)
{
	  cls();
      settextcolor(4,0);
      printf("                                   ^~~~^\n");
      printf("                                  -     -\n");
      printf("                                ..       ..\n");
      printf("                               ~           ~\n");
      printf("                              .             .\n");
      printf("                             (;:   BOOM!   :;)\n");
      printf("----------                    (:           :)\n");
      printf("| PANIC! |                      ,         ,\n");
      printf("|_  _____|                       :._   _.:\n");
      printf("  |/                                | |\n");
      printf(" 0                                (=====)\n");
      printf("- -                                 | |\n");
      printf(" |                                  | |\n");
      printf("/ \\                              ((/   \\))\n");
      settextcolor(2,0);
      printf("----------------------------------------------------------\n");
      settextcolor(15,0);
      printf("The kernel made a boo-boo and couldn't fix it.\n");
	  printf("%s\n\n", desc);
	  printf("CPU HALT\n");
	  for(;;){}
}
