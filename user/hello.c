/* TomatOS - hello: the first real user space program
*  Desc: A ring 3 program that is a genuinely separate binary.
*
*  Everything ring 3 has run so far were kernel routines copied into a user
*  page -- code that was compiled and linked as part of the kernel and just
*  happened to execute with CPL 3. This is the real thing: an ELF32
*  executable built on its own, with its own linker script and its own load
*  address, that the kernel maps into an address space of its own -- either
*  from a Multiboot module or from a file on the mounted volume.
*
*  It used to hand-roll its own decimal conversion, because the only header a
*  program got was the raw "int 0x80" wrappers. There is a small C library now
*  (user/lib.h), so this reads like a C program: an entry point called main(),
*  arguments, printf. What it demonstrates has not changed:
*
*    - There is still no C runtime beyond what lib.c provides, and no libc
*      underneath it. Everything ultimately becomes "int 0x80".
*    - main() may return; _start() passes the value to sys_exit() for it.
*/
#include "syscall.h"
#include "lib.h"

/* Exit status handed to the kernel. Deliberately not 0 -- a distinctive
*  value proves the status actually travels from ring 3 through sys_exit()
*  into the task's bookkeeping, which a 0 would not. */
#define HELLO_EXIT_STATUS  42

/* How the "still alive" line is drawn. */
#define TICK_COUNT         10
#define TICK_INTERVAL_MS   200

int main(int argc, char **argv)
{
	int start_ms;
	int end_ms;
	int i;

	printf("\nHello from user space! This is a real ELF with its own address space.\n");
	printf("  pid       : %d\n", sys_getpid());

	/* argv[0] is the program name and is always there, so argc is never 0.
	*  Printing the arguments back is the cheapest proof that the loader put
	*  them where the calling convention says they are. */
	printf("  argc      : %d\n", argc);
	for(i = 0; i < argc; i++)
	{
		printf("  argv[%d]   : %s\n", i, argv[i]);
	}

	start_ms = sys_uptime();
	printf("  uptime    : %d ms since boot\n", start_ms);

	/* Something visibly alive: one dot every TICK_INTERVAL_MS. Each pause is
	*  a separate SYS_SLEEP, so the line grows in real time -- and while this
	*  task sleeps the scheduler is free to run everything else, which is the
	*  point of doing it this way rather than in a busy loop. */
	printf("  ticking   : ");
	for(i = 0; i < TICK_COUNT; i++)
	{
		printf(".");
		sys_sleep(TICK_INTERVAL_MS);
	}
	printf("\n");

	/* The measured time is what the kernel actually granted, not what was
	*  asked for: sleeps round up to whole timer ticks and other tasks run in
	*  between, so this is always a little more than the nominal total. */
	end_ms = sys_uptime();
	printf("  slept     : %d ms (asked for %d ms)\n",
	       end_ms - start_ms, TICK_COUNT * TICK_INTERVAL_MS);

	printf("Goodbye - exiting with status %d.\n", HELLO_EXIT_STATUS);

	/* Returning is enough: _start() in lib.c hands this to sys_exit(). */
	return HELLO_EXIT_STATUS;
}
