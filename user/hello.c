/* TomatOS - hello: the first real user space program
*  Desc: A ring 3 program that is a genuinely separate binary.
*
*  Everything ring 3 has run so far were kernel routines copied into a user
*  page -- code that was compiled and linked as part of the kernel and just
*  happened to execute with CPL 3. This is the real thing: an ELF32
*  executable built on its own, with its own linker script and its own load
*  address, that the bootloader hands to the kernel as a module and that the
*  loader maps into an address space of its own.
*
*  Consequences, all of them visible in this file:
*
*    - There is no C runtime. Nothing calls main() and nothing catches its
*      return value, so the entry point is _start() and it must never fall
*      off its end. The last thing it does is sys_exit().
*    - There is no libc. Formatting a number means writing the loop for it.
*    - The only way to reach the outside world is "int 0x80".
*/
#include "syscall.h"

/* Exit status handed to the kernel. Deliberately not 0 -- a distinctive
*  value proves the status actually travels from ring 3 through sys_exit()
*  into the task's bookkeeping, which a 0 would not. */
#define HELLO_EXIT_STATUS  42

/* How the "still alive" line is drawn. */
#define TICK_COUNT         10
#define TICK_INTERVAL_MS   200

/* Scratch space for print_dec(). Deliberately a file scope buffer rather
*  than a local: it puts something in .bss, which means the program's single
*  PT_LOAD segment has p_memsz > p_filesz and the loader's zeroing path gets
*  exercised by the very first program that ever runs. 11 bytes is the
*  longest an unsigned 32-bit value plus terminator can get. */
static char dec_buf[11];


/* Prints an unsigned value in decimal.
*
*  Digits come out of the division loop least significant first, so they are
*  written into the buffer from the back and the string starts wherever that
*  loop stopped. No leading zeros, no padding -- there is nothing here that
*  would need them. */
static void print_dec(unsigned int value)
{
	int i;

	i = (int)sizeof(dec_buf) - 1;
	dec_buf[i] = '\0';

	if(value == 0)
	{
		dec_buf[--i] = '0';
	}
	else
	{
		while(value != 0 && i > 0)
		{
			dec_buf[--i] = (char)('0' + (value % 10));
			value /= 10;
		}
	}

	sys_write(&dec_buf[i]);
}

/* Prints a signed value, for the milliseconds the kernel returns as int. */
static void print_int(int value)
{
	if(value < 0)
	{
		sys_putch('-');
		print_dec((unsigned int)(-value));
	}
	else
	{
		print_dec((unsigned int)value);
	}
}


/* The entry point. ENTRY(_start) in user.ld puts its address in e_entry, and
*  that is the address the loader iret's to. There is no argc, no argv and no
*  return address on the stack -- the kernel builds the initial user context
*  from scratch, so the stack is empty and this function has nowhere to
*  return to. */
void _start(void)
{
	int pid;
	int start_ms;
	int end_ms;
	int i;

	sys_write("\nHello from user space! This is a real ELF, loaded as a module.\n");

	pid = sys_getpid();
	sys_write("  pid       : ");
	print_int(pid);
	sys_putch('\n');

	start_ms = sys_uptime();
	sys_write("  uptime    : ");
	print_int(start_ms);
	sys_write(" ms since boot\n");

	/* Something visibly alive: one dot every TICK_INTERVAL_MS. Each dot is
	*  a separate SYS_PUTCH and each pause a separate SYS_SLEEP, so the line
	*  grows in real time -- and while this task sleeps the scheduler is free
	*  to run everything else, which is the point of doing it this way rather
	*  than in a busy loop. */
	sys_write("  ticking   : ");
	for(i = 0; i < TICK_COUNT; i++)
	{
		sys_putch('.');
		sys_sleep(TICK_INTERVAL_MS);
	}
	sys_putch('\n');

	/* The measured time is what the kernel actually granted, not what was
	*  asked for: sleeps round up to whole timer ticks and other tasks run in
	*  between, so this is always a little more than the nominal total. */
	end_ms = sys_uptime();
	sys_write("  slept     : ");
	print_int(end_ms - start_ms);
	sys_write(" ms (asked for ");
	print_int(TICK_COUNT * TICK_INTERVAL_MS);
	sys_write(" ms)\n");

	sys_write("Goodbye - exiting with status ");
	print_int(HELLO_EXIT_STATUS);
	sys_write(".\n");

	/* No return. If sys_exit() ever did come back, the iret would resume a
	*  task that is supposed to be dead. */
	sys_exit(HELLO_EXIT_STATUS);
}
