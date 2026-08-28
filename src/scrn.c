/* bkerndev - Bran's Kernel Development Tutorial
*  By:   Brandon F. (friesenb@gmail.com)
*  Desc: Screen output functions for Console I/O
*
*  Notes: No warranty expressed or implied. Use at own risk. */
#include <system.h>
#include <stdarg.h>
#include <string.h>
#include <mm.h>
/* These define our textpointer, our background and foreground
*  colors (attributes), and x and y cursor coordinates */
volatile unsigned short *textmemptr;
int attrib = 0x0F;
int csr_x = 0, csr_y = 0;

/* Scrolls the screen */
void scroll(void)
{
    unsigned blank, temp;

    /* A blank is defined as a space... we need to give it
    *  backcolor too */
    blank = 0x20 | (attrib << 8);

    /* Row 25 is the end, this means we need to scroll up */
    if(csr_y >= 25)
    {
        /* Move the current text chunk that makes up the screen
        *  back in the buffer by a line */
        temp = csr_y - 25 + 1;
        memcpy ((void *)textmemptr, (const void *)(textmemptr + temp * 80), (25 - temp) * 80 * 2);

        /* Finally, we set the chunk of memory that occupies
        *  the last line of text to our 'blank' character */
        memsetw ((unsigned short *)(textmemptr + (25 - temp) * 80), blank, 80);
        csr_y = 25 - 1;
    }
	
	
}

/* Updates the hardware cursor: the little blinking line
*  on the screen under the last character pressed! */
void move_csr(void)
{
    unsigned temp;

    /* The equation for finding the index in a linear
    *  chunk of memory can be represented by:
    *  Index = [(y * width) + x] */
    temp = csr_y * 80 + csr_x;

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
    int printf(char * string, ...);

    /* Again, we need the 'short' that will be used to
    *  represent a space with color */
    blank = 0x20 | (attrib << 8);

    /* Sets the entire screen to spaces in our current
    *  color */
    for(i = 0; i < 25; i++)
        memsetw ((unsigned short *)(textmemptr + i * 80), blank, 80);

    /* Update out virtual cursor, and then move the
    *  hardware cursor */
    csr_x = 0;
    csr_y = 0;
    move_csr();
    
}

/* Puts a single character on the screen */
void putch(unsigned char c)
{
    volatile unsigned short *where;
    unsigned att = attrib << 8;

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
        where = textmemptr + (csr_y * 80 + csr_x);
        *where = c | att;	/* Character AND attributes: color */
        csr_x++;
    }

    /* If the cursor has reached the edge of the screen's width, we
    *  insert a new line in there */
    if(csr_x >= 80)
    {
        csr_x = 0;
        csr_y++;
    }

    /* Scroll the screen if needed, and finally move the cursor */
    scroll();
    move_csr();
	
}

/* Uses the above routine to output a string... */
void puts(char *text)
{
    int i;

    for (i = 0; i < strlen(text); i++)
    {
        putch(text[i]);
    }
}

/* Sets the forecolor and backcolor that we will use */
void settextcolor(unsigned char forecolor, unsigned char backcolor)
{
    /* Top 4 bytes are the background, bottom 4 bytes
    *  are the foreground color */
    attrib = (backcolor << 4) | (forecolor & 0x0F);
}

/* Sets our text-mode VGA pointer, then clears the screen for us */
void init_video(void)
{
    textmemptr = (volatile unsigned short *)0xB8000;
    cls();
}

int printf(char * string, ...)
{
	int i;
  int stringlen;
  int tmp;
  unsigned char uc;
  unsigned char uc2;

	va_list argzeiger;
	va_start(argzeiger, string);
		
	stringlen = (int)strlen(string);
	
	for(i=0; i <= stringlen; i++)
  {
    if(string[i] == '%')
      {
        /* Controlcharacter. Check indice */
        switch(string[i+1])
        {
          case 's':
            puts(va_arg(argzeiger, char *));
            i++;
            break;
          case 'u':
		  case 'd':
          case 'i':
            tmp = va_arg(argzeiger, int);
            if(tmp <0)
            {
              printf("-");
              tmp = tmp *(-1);
            }
            puts(itoa(tmp));
            i++;
            break;
          case 'c':
            putch(va_arg(argzeiger, char));
            i++;
            break;
          case 'p':
            puts(itoa(va_arg(argzeiger, int)));
            i++;
            break;
          case 'X':
            hextoa(va_arg(argzeiger, int));
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

	va_end(argzeiger);
	
	
	return 0;
}

/* ------------------------------------------------------------------ */
/* Statusleiste                                                        */
/* ------------------------------------------------------------------ */

/* Die Leiste ist die oberste Bildschirmzeile und damit genau 80 Spalten
*  breit. */
#define STATUSBAR_WIDTH     80

/* Die Uhr steht rechtsbündig: "hh:mm:ss" belegt acht Spalten ab Spalte 70,
*  die beiden letzten Spalten bleiben leer (dort stand früher die
*  Abschlussnull des Zeitstrings, die als Leerzeichen erschien). */
#define STATUSBAR_CLOCK_LEN 8
#define STATUSBAR_CLOCK_COL (STATUSBAR_WIDTH - 2 - STATUSBAR_CLOCK_LEN)

/* Frames pro Megabyte: 1 MiB / 4 KiB = 256. Die Anzeige rechnet bewusst in
*  Frames statt in Bytes -- ein Bytewert liefe bei 4 GiB über, die Zahl der
*  Frames passt immer bequem in 32 Bit. */
#define STATUSBAR_FRAMES_PER_MB (1024 * 1024 / PMM_FRAME_SIZE)

/* Schreibt text mit dem aktuell eingestellten attrib ab Spalte col in die
*  Leiste und liefert die erste freie Spalte zurück. Was in den Bereich der
*  Uhr laufen würde, wird verworfen: so kann weder eine lange Zahl noch eine
*  hohe Taskzahl die Zeile sprengen oder in die nächste überlaufen. */
static int statusbar_puts(int col, const char *text)
{
	volatile unsigned short *pos;

	while(*text != EOS && col < STATUSBAR_CLOCK_COL)
	{
		pos = textmemptr + col;
		*pos = (unsigned char)*text | (attrib << 8);
		col++;
		text++;
	}

	return col;
}

/* Wie statusbar_puts(), aber für ein einzelnes Zeichen -- gebraucht für den
*  Aktivitätspunkt 0xFE, der in keinem gewöhnlichen String steht. */
static int statusbar_putc(int col, unsigned char c)
{
	volatile unsigned short *pos;

	if(col < STATUSBAR_CLOCK_COL)
	{
		pos = textmemptr + col;
		*pos = c | (attrib << 8);
		col++;
	}

	return col;
}

void display_update_statusbar()
{
	volatile unsigned short *pos;
	unsigned long flags;
	int org_attrib;
	int i;
	int x;
	int back = 0x7;
	int taskcount;
	uint32_t total_frames;
	uint32_t used_frames;
	datetime now;
	char mem[24];
	char time[9] = "00:00:00";

	/* Die Uhrzeit vor der kritischen Sektion holen: cmos_readtime() wartet
	*  unter Umständen auf das Ende eines RTC-Updates und sichert die
	*  Interrupts bereits selbst ab. Innerhalb unseres cli zu warten würde
	*  die Sperre unnötig lange halten. */
	now = cmos_readtime();

	/* EFLAGS sichern und erst dann sperren. Ein bedingungsloses sti am Ende
	*  würde die Interrupts auch dann einschalten, wenn der Aufrufer sie
	*  absichtlich gesperrt hatte. Die Sperre selbst ist nötig, weil hier
	*  direkt in den VGA-Puffer geschrieben wird, während der Konsolen-Task
	*  dasselbe über putch() tut. */
	__asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");

	org_attrib = attrib;

	/* Kennzahlen und Zahlenformatierung gehören in die kritische Sektion:
	*  itoa() liefert einen Zeiger auf einen statischen Puffer, den sich
	*  alle Tasks teilen. */
	taskcount = taskmgr_get_taskcount();
	total_frames = pmm_total_frames();
	used_frames = pmm_used_frames();

	if(total_frames == 0)
	{
		/* Speicherverwaltung noch nicht aufgesetzt -- ehrlicher als eine
		*  erfundene Zahl. */
		strcpy(mem, "n/a");
	}
	else
	{
		/* Belegt wird aufgerundet, damit eine kleine, aber vorhandene
		*  Belegung nicht als "0" erscheint; die Gesamtgröße wird
		*  abgerundet, damit die Leiste nie mehr Speicher verspricht, als
		*  wirklich da ist. */
		strcpy(mem, itoa((int)((used_frames + STATUSBAR_FRAMES_PER_MB - 1)
		                       / STATUSBAR_FRAMES_PER_MB)));
		strcat(mem, "/");
		/* Erst jetzt darf itoa() erneut laufen: der statische Puffer mit
		*  dem ersten Wert ist oben weggesichert. */
		strcat(mem, itoa((int)(total_frames / STATUSBAR_FRAMES_PER_MB)));
		strcat(mem, "mb");
	}

	/* Hintergrund der ganzen Zeile: schwarze Schrift auf hellgrau. */
	settextcolor(0x00, 0x7);
	for(i = 0; i < STATUSBAR_WIDTH; i++)
	{
		pos = textmemptr + i;
		*pos = ' ' | (attrib << 8);
	}

	i = 0;

	/* [cpu: .] -- Klammern schwarz, Beschriftung weiß */
	i = statusbar_puts(i, "[");
	settextcolor(15, back);
	i = statusbar_puts(i, "cpu: ");
	i = statusbar_putc(i, 0xFE);	/* Aktivitätspunkt, CPU */
	settextcolor(0x00, 0x7);
	i = statusbar_puts(i, "] ");

	/* [mem: belegt/gesamt mb] */
	i = statusbar_puts(i, "[");
	settextcolor(15, back);
	i = statusbar_puts(i, "mem: ");
	i = statusbar_puts(i, mem);
	settextcolor(0x00, 0x7);
	i = statusbar_puts(i, "] ");

	/* [task: ..] -- ein Punkt je laufendem Task */
	i = statusbar_puts(i, "[");
	settextcolor(15, back);
	i = statusbar_puts(i, "task: ");
	for(x = 0; x < taskcount; x++)
	{
		i = statusbar_putc(i, 0xFE);
	}
	settextcolor(0x00, 0x7);
	i = statusbar_puts(i, "]");

	/* Uhr, rechtsbündig. attrib steht bereits auf schwarz/hellgrau. */
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
		pos = textmemptr + (STATUSBAR_CLOCK_COL + x);
		*pos = time[x] | (attrib << 8);
	}

	attrib = org_attrib;

	/* EFLAGS zurück -- die Interrupts sind danach genau so gesperrt oder
	*  freigegeben wie beim Eintritt. */
	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
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
