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

/* The channel a task blocks on while it waits for a key.
*
*  It is the address of last_key itself: the thing that is waited for, which is
*  what system.h asks for and what makes the pairing obvious -- the handler
*  writes last_key and wakes the same address, and nothing ever reads through
*  the pointer. special_key deliberately has no channel: getchs() is a
*  non-blocking read and nobody waits on it, so a wake there would cost a walk
*  over the task table for no waiter.
*
*  The cast is only about the qualifier. last_key is volatile, task_wake()
*  takes a plain const void *, and handing one to the other without saying so
*  is a warning about a discarded qualifier rather than a real conversion. */
#define KB_WAIT_CHANNEL ((const void *)&last_key)

/* Upper bound on one wait inside getch(), in milliseconds.
*
*  getch() blocks until a key arrives however long that takes -- the loop
*  around the wait sees to that -- so this is not a deadline, it is the safety
*  net system.h asks every waiter on hardware to carry. If a wake is ever
*  missed the read degrades to polling five times a second instead of to a
*  keyboard that is dead until the next keystroke that happens to land while
*  somebody else is waiting. Five wakeups a second on an idle machine is
*  nothing next to the thousand ticks the timer produces anyway. */
#define KB_WAIT_MS 200

/* EFLAGS.IF, tested to find out whether the caller arrived inside a critical
*  section of its own. */
#define KB_EFLAGS_IF 0x200

/* net.c has a pair of these but they are static to that file, and there is no
*  global one; a handful of lines is cheaper than a new cross-file interface.
*  Saving the flags and clearing IF is the only form that is safe in a caller
*  that may already have interrupts off -- a plain enable() at the end would
*  hand them back on to somebody who had deliberately switched them off. */
static unsigned long kb_irq_save(void)
{
    unsigned long flags;

    __asm__ __volatile__ ("pushfl; popl %0; cli"
                          : "=r" (flags) : : "memory");
    return flags;
}

static void kb_irq_restore(unsigned long flags)
{
    __asm__ __volatile__ ("pushl %0; popfl"
                          : : "r" (flags) : "memory", "cc");
}

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

      /* A key is in the slot: wake whoever is waiting for one. This is
      *  interrupt context, which is what task_wake() is built for -- it walks
      *  the task table, stores states and returns. Nothing else may creep in
      *  here: no printing, no drain, no switch.
      *
      *  Only when there really is a character. A scancode that maps to 0 -- a
      *  modifier, a function key -- leaves last_key at EOS, so every waiter's
      *  condition is still false and the wake would buy nothing but the walk
      *  over the table. */
      if (last_key != EOS)
      {
         task_wake(KB_WAIT_CHANNEL);
      }
    }
	

}

void keyboard_install()
{
    last_key = EOS;
    irq_install_handler(1, keyboard_handler);
}

/* Waits for a key and returns it.
*
*  This used to halt in a loop, and the loop was the problem rather than the
*  halt: the task stayed TASK_STATE_RUNNING throughout, so the scheduler kept
*  electing a task whose whole turn was one comparison and a hlt, once per time
*  slice for as long as nobody typed. A shell sitting at its prompt -- which is
*  what this machine does most of the time -- was costing a full share of the
*  CPU to wait.
*
*  Now it blocks on the channel the keyboard handler wakes, in the idiom
*  system.h spells out and for the reason it gives: interrupts are off across
*  the test AND the block, so the keystroke that arrives in between cannot be
*  lost, and the condition is re-tested after every wake rather than trusted.
*  Two tasks can be in here at once -- the console and a program that reads --
*  and both are woken by one key; the one that loses the race finds last_key
*  back at EOS and blocks again, which is exactly what the re-test is for.
*
*  The key is taken with interrupts still off, so the read and the clear cannot
*  have a second keystroke land between them.
*
*  TWO CALLERS CANNOT BLOCK, and both fall back to what this did before.
*
*    - No task is running: the boot path, before the console task exists.
*      task_wait() answers such a caller with "timed out" at once, so the loop
*      around it would turn into a busy spin; the hlt loop is what it was and
*      is still right there.
*    - Interrupts are off on entry. The caller built a critical section out of
*      cli, and task_wait() halts with them ON -- it has to, or nothing could
*      ever wake it -- so blocking would both break that section open and hand
*      the CPU to a task that may walk into the data it is holding. sleep() in
*      timer.c faced exactly this and answered it the same way: fall back to
*      the old loop, which gets nowhere with the keyboard IRQ masked but breaks
*      nobody's invariant. The hlt is dropped in that case for timer_wait()'s
*      reason -- halting with IF clear is a CPU that only a reset leaves --
*      which leaves a spin that is at least interruptible.
*
*  Ring 3 reaches the keyboard through SYS_GETCH and arrives with IF set (tasks
*  start with eflags 0x202 and cli is privileged), and vector 0x80 is a trap
*  gate, so a user program always gets the blocking path. */
unsigned char getch()
{
    unsigned char ky;
    unsigned long flags;

    flags = kb_irq_save();

    if(taskmgr_get_currpid() < 0 || (flags & KB_EFLAGS_IF) == 0)
    {
        kb_irq_restore(flags);

        /* last_key is volatile, so the loop cannot be optimised away. */
        while(last_key == EOS)
        {
            if(flags & KB_EFLAGS_IF)
                __asm__ __volatile__("hlt");
        }

        flags = kb_irq_save();
    }
    else
    {
        /* KB_WAIT_MS is a bound on one wait, not on the read: a timeout only
        *  sends the loop round again. */
        while(last_key == EOS)
        {
            task_wait(KB_WAIT_CHANNEL, KB_WAIT_MS);
        }
    }

    ky = last_key;
    last_key = EOS;

    kb_irq_restore(flags);

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
