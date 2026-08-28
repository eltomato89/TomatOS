/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Keyboard driver
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (friesenb@gmail.com)
*/

#include <system.h>
#include <string.h>
#include <stdio.h>

/* German keyboard layout, unshifted. The array is indexed by scancode, so no
*  entry may ever be moved.
*  The character values are CP437 code points, because the VGA text mode
*  renders CP437 and not Latin-1 - with the former Latin-1 values, typed
*  umlauts showed up as completely different characters. */
unsigned char kbdde_s[128] =
{
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',	/* 9 */
  '9', '0', 0xE1 /* ß */, 0x27 /* ´ key (CP437 has no acute accent) */, '\b',	/* Backspace */
  '\t',			/* Tab */
  'q', 'w', 'e', 'r',	/* 19 */
  't', 'z', 'u', 'i', 'o', 'p', 0x81 /* ü */, '+', '\n',		/* Enter key */
    0,			/* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0x94 /* ö */,	/* 39 */
 0x84/* ä */, '^',   0,		/* Left shift */
 '#', 'y', 'x', 'c', 'v', 'b', 'n',			/* 49 */
  'm', ',', '.', '-',   0,					/* Right shift */
  '*',
    0,	/* Alt */
  ' ',	/* Space bar */
    0,	/* Caps lock */
    0,	/* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,	/* < ... F10 */
    0,	/* 69 - Num lock*/
    0,	/* Scroll Lock */
    0,	/* Home key */
    0,	/* Up Arrow */
    0,	/* Page Up */
  '-',
    0,	/* Left Arrow */
    0,
    0,	/* Right Arrow */
  '+',
    0,	/* 79 - End key*/
    0,	/* Down Arrow */
    0,	/* Page Down */
    0,	/* Insert Key */
    0,	/* Delete Key */
    0,   0,   0,
    0,	/* F11 Key */
    0,	/* F12 Key */
    0,	/* All other keys are undefined */
};
/* Same layout with shift held down; CP437 code points as above, so this table
*  yields the uppercase umlauts. */
unsigned char kbdde_b[128] =
{
    0,  27, '!', '\"', 0x15 /* § */, '$', '%', '&', '/', '(',	/* 9 */
  ')', '=', 0xE1 /* ß */, '`', '\b',	/* Backspace */
  '\t',			/* Tab */
  'Q', 'W', 'E', 'R',	/* 19 */
  'T', 'Z', 'U', 'I', 'O', 'P', 0x9A /* Ü */, '*', '\n',		/* Enter key */
    0,			/* 29   - Control */
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0x99 /* Ö */,	/* 39 */
 0x8E/* Ä */, 0xF8 /* ° */,   0,		/* Left shift */
 0x27, 'Y', 'X', 'C', 'V', 'B', 'N',			/* 49 */
  'M', ';', ':', '_',   0,					/* Right shift */
  '*',
    0,	/* Alt */
  ' ',	/* Space bar */
    0,	/* Caps lock */
    0,	/* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,	/* < ... F10 */
    0,	/* 69 - Num lock*/
    0,	/* Scroll Lock */
    0,	/* Home key */
    0,	/* Up Arrow */
    0,	/* Page Up */
  '-',
    0,	/* Left Arrow */
    0,
    0,	/* Right Arrow */
  '+',
    0,	/* 79 - End key*/
    0,	/* Down Arrow */
    0,	/* Page Down */
    0,	/* Insert Key */
    0,	/* Delete Key */
    0,   0,   0,
    0,	/* F11 Key */
    0,	/* F12 Key */
    0,	/* All other keys are undefined */
};


volatile unsigned char last_key;
volatile unsigned char special_key = EOS;
int shift = 0;
 
 void keyboard_handler(struct regs *r)
{
    unsigned char scancode;
	
    /* Read from the keyboard's data buffer */
    scancode = inportb(0x60);
	special_key = scancode;
    if (scancode == (42|0x80) || scancode == (54|0x80))
    {
      shift = 0;
    }
   else if (scancode == 42 || scancode == 54)
   {
      //then it was shift
      shift = 1;
   }
    else if (scancode <= 127)
    {
      if (shift==1)
      {
         last_key = kbdde_b[scancode];
      }
      else
      {
         last_key = kbdde_s[scancode];
      }
    }
	

}

void keyboard_install()
{
    last_key = EOS;
    irq_install_handler(1, keyboard_handler);
}

unsigned char getch()
{   
    char ky;

    /* last_key is volatile, so the loop must not be optimised away. Instead
    *  of busy polling we wait power-savingly with 'hlt' for the next
    *  interrupt - the keyboard IRQ (or, failing that, the timer) wakes us
    *  up again. */
    while(last_key == EOS)
    {
      __asm__ __volatile__("hlt");
    }

    ky = last_key;
    last_key = EOS;
    

    return ky;
}
unsigned char getchs()
{
	char x;
	x = special_key;
	special_key = EOS;
	return x;
}
unsigned char getchn()
{
	char x;
	x = last_key;
	last_key = EOS;
	return x;
}

/* gets() has been removed: the function returned a pointer to a local array
*  (undefined behaviour), had no caller at all, and scan() does the same job
*  through an output parameter. */

/* Careful: scan() copies the result to 'var' with strcpy() without knowing
*  its size. The destination buffer must therefore hold at least SCAN_BUFSZ
*  bytes. Without changing the signature (the declaration lives in stdio.h)
*  this cannot be solved more cleanly; at least the internal buffer is now
*  bounded and the maximum copy length is thus known. */
#define SCAN_BUFSZ 80

void scan(char* var)
  {
    unsigned char ky;
    static char ret[SCAN_BUFSZ];
    int i=0;

    do
    {
      ky = getch();

      if(ky >= 32 && ky != 127)
      {
        /* Once the buffer is full, further characters are ignored. */
        if(i < SCAN_BUFSZ - 1)
        {
          ret[i] = ky;
          putch(ky);
          i++;
        }
      }

      if(ky == 8 && i > 0)
      {
        
        printf("\b \b");
        
        i--;
        
      }

      
    } while(ky != 10);
    
    ret[i] = '\0';
    printf("\n");
    
	strcpy(var, ret);
  }
  
  /* Same restriction as for scan(): 'var' must hold at least SCAN_BUFSZ
  *  bytes. */
  void scan_h(char* var, int out)
  {
    unsigned char ky;
    static char ret[SCAN_BUFSZ];
    int i=0;

    do
    {
      ky = getch();

      if(ky >= 32 && ky != 127)
      {
        /* Once the buffer is full, further characters are ignored. */
        if(i < SCAN_BUFSZ - 1)
        {
          ret[i] = ky;
          printf("%c", out);
          i++;
        }
      }

      if(ky == 8 && i > 0)
      {
        
        printf("\b \b");
        
        i--;
        
      }

      
    } while(ky != 10);
    
    ret[i] = '\0';
    printf("\n");
    
	strcpy(var, ret);
  }
  
void kb_flush()
{
	special_key = EOS;
}
