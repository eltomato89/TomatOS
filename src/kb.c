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

/* Die Zeichenwerte sind CP437-Codepunkte, denn der VGA-Textmodus stellt
*  CP437 dar und nicht Latin-1. */
unsigned char kbdde_s[128] =
{
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',	/* 9 */
  '9', '0', 0xE1 /* ß */, 0x27 /* ´ (CP437 kennt kein Akut) */, '\b',	/* Backspace */
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

    /* last_key ist volatile, die Schleife darf also nicht wegoptimiert
    *  werden. Statt busy zu pollen warten wir stromsparend mit 'hlt' auf
    *  den naechsten Interrupt - der Tastatur-IRQ (und notfalls der Timer)
    *  weckt uns wieder auf. */
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

/* gets() wurde entfernt: die Funktion gab einen Zeiger auf ein lokales
*  Array zurueck (Undefined Behaviour), hatte keinen einzigen Aufrufer und
*  scan() leistet dasselbe ueber einen Ausgabeparameter. */

/* Achtung: scan() kopiert das Ergebnis mit strcpy() nach 'var', ohne dessen
*  Groesse zu kennen. Der Zielpuffer muss darum mindestens SCAN_BUFSZ Bytes
*  fassen. Ohne Signaturaenderung (die Deklaration steht in stdio.h) laesst
*  sich das nicht sauberer loesen; wenigstens ist der interne Puffer jetzt
*  begrenzt und die maximale Kopierlaenge damit bekannt. */
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
        /* Ist der Puffer voll, werden weitere Zeichen ignoriert. */
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
  
  /* Gleiche Einschraenkung wie bei scan(): 'var' muss mindestens
  *  SCAN_BUFSZ Bytes fassen. */
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
        /* Ist der Puffer voll, werden weitere Zeichen ignoriert. */
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
