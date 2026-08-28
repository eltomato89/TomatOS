/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: System call implementation -- the kernel side of "int 0x80"
*
*  The assembly stub in start.asm saves the register set and hands it here.
*  eax holds the call number, ebx/ecx/edx the arguments, and the result is
*  written back into r->eax so that the iret delivers it to the caller.
*
*  Everything in this file runs on behalf of ring 3. That makes it the one
*  place where the kernel touches values it did not produce itself, so every
*  argument is treated as hostile until it has been checked -- see
*  user_string_len() below.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <vmm.h>
#include <syscall.h>

/* The stub in start.asm. It pushes a dummy error code and the interrupt
*  number, saves the registers and calls syscall_handler(). */
extern void syscall_stub();

/* settextcolor() lives in scrn.c but has no prototype in any header yet.
*  Declared locally, exactly as isrs.c does it. */
extern void settextcolor(unsigned char forecolor, unsigned char backcolor);

/* Longest string SYS_WRITE accepts, terminator included. A user pointer may
*  point at memory that simply never contains a zero byte, so the scan needs
*  an upper bound -- without one, ring 3 could park the kernel in a loop
*  inside an interrupt handler forever. */
#define SYS_WRITE_MAX 1024

/* Upper bound for the schedule() calls in sys_exit(), same reasoning as
*  FAULT_SCHEDULE_TRIES in isrs.c: schedule() only switches once the running
*  task's time slice is used up, and the loop must not be able to spin. */
#define SYSCALL_SCHEDULE_TRIES 64

/* Number of calls served since boot, reported by syscall_count(). */
static uint32_t syscall_calls = 0;

/* A handler reads its arguments out of the saved register set and returns
*  the value for eax. */
typedef int (*syscall_fn)(struct regs *r);


/* ------------------------------------------------------------------ */
/* Argument validation                                                 */
/* ------------------------------------------------------------------ */

/* Non-zero if a single byte at addr may be read on behalf of ring 3.
*
*  Three rules, and each of them exists because of a concrete attack:
*    - address zero and the rest of the first page are rejected outright.
*      Page zero is deliberately never mapped (see vmm.h), so a null pointer
*      would fault anyway -- but faulting inside the kernel with a user
*      pointer is precisely what we are trying not to do.
*    - anything at or above KERNEL_VIRTUAL_BASE belongs to the kernel. Ring 3
*      cannot read there itself; without this test it could ask the kernel to
*      read it out loud through SYS_WRITE and dump kernel memory to screen.
*    - the page must actually be present. vmm_is_mapped() answers that
*      question without touching the memory, which is the whole point: a
*      dereference to find out would already be the page fault we are
*      avoiding. */
static int user_byte_ok(uint32_t addr)
{
	if(addr < PAGE_SIZE) return 0;
	if(addr >= KERNEL_VIRTUAL_BASE) return 0;
	if(!vmm_is_mapped(addr)) return 0;

	return 1;
}

/* Length of the NUL terminated string at addr, or -1 if the string is not
*  safe to read.
*
*  The check has to walk the string, because a mapping only ever covers one
*  page: a string may start in a mapped page and run straight into an
*  unmapped one. Rather than validating every single byte (vmm_is_mapped()
*  walks the page directory, that would be one table lookup per character),
*  the page is validated once at the start and then again at every 4 KiB
*  boundary the string crosses.
*
*  The length is deliberately established before the first character is
*  printed. Validating and printing in one pass would mean a bad pointer
*  produces half a line of output and an error code at the same time; this
*  way SYS_WRITE either prints the whole string or prints nothing at all.
*
*  A string with no terminator within SYS_WRITE_MAX bytes is rejected instead
*  of truncated: at that point the argument is not a string, and guessing
*  where it ends is not the kernel's job. */
static int user_string_len(uint32_t addr)
{
	int len;
	uint32_t at;

	if(!user_byte_ok(addr)) return -1;

	for(len = 0; len < SYS_WRITE_MAX; len++)
	{
		at = addr + (uint32_t)len;

		/* A new page starts here, so the old permission no longer says
		*  anything about this byte. Re-check before reading it. */
		if(len > 0 && (at & (PAGE_SIZE - 1)) == 0)
		{
			if(!user_byte_ok(at)) return -1;
		}

		if(*(const volatile unsigned char *)at == '\0') return len;
	}

	return -1;
}


/* ------------------------------------------------------------------ */
/* The calls                                                           */
/* ------------------------------------------------------------------ */

/* exit(status) -- terminates the calling task. This is the one call that
*  must not return to its caller.
*
*  Returning normally would be pointless: the stub ends in an iret, and that
*  iret would put the CPU back on the instruction after the "int 0x80" of a
*  task that is supposed to be dead. So the same trick fault_handler() uses
*  is applied here -- the saved context is overwritten in place with the
*  context of the next runnable task, and the iret lands in that task
*  instead. The exiting task's own registers are never restored again. */
static int sys_exit(struct regs *r)
{
	int pid;
	int i;
	struct regs *next;
	int status;

	status = (int)r->ebx;
	pid = taskmgr_get_currpid();

	/* No task is running, so there is nothing to terminate. That means the
	*  kernel itself issued the call -- report it and let the caller carry
	*  on, rather than switching a context that does not exist. */
	if(pid < 0)
	{
		printf("exit() called without a running task\n");
		r->eax = (unsigned int)SYS_ENOSYS;
		return 0;
	}

	/* The task is marked aborted and loses its remaining time slice, so the
	*  search below skips it. */
	taskmgr_task_abort(pid, status, "exit()");

	settextcolor(2,0);
	printf("Task %i exited with status %i\n", pid, status);
	settextcolor(15,0);

	/* schedule() hands back the context it was given until the current time
	*  slice is used up, so it is called until a different one comes out --
	*  bounded, because this runs inside an interrupt. */
	next = r;
	for(i = 0; i < SYSCALL_SCHEDULE_TRIES; i++)
	{
		next = schedule(r);
		if(next != r) break;
	}

	if(next != r)
	{
		/* The stub restores every register from exactly this memory and
		*  then irets. Replacing the contents therefore switches tasks
		*  without a single line of assembler. */
		*r = *next;
		return 0;
	}

	/* Nothing left to switch to: there is no context the iret could return
	*  into that would not immediately be the dead task again. */
	settextcolor(4,0);
	printf("No runnable task left - CPU HALT\n");
	settextcolor(15,0);
	for(;;);

	return 0;
}

/* write(text) -- prints a NUL terminated string and returns the number of
*  characters written, or SYS_EFAULT if the pointer does not survive
*  user_string_len(). */
static int sys_write(struct regs *r)
{
	uint32_t addr;
	int len;
	int i;

	addr = (uint32_t)r->ebx;

	len = user_string_len(addr);
	if(len < 0) return SYS_EFAULT;

	/* Validated above, so the bytes can be read without further checks. */
	for(i = 0; i < len; i++)
	{
		putch(*(const volatile unsigned char *)(addr + (uint32_t)i));
	}

	return len;
}

/* getpid() */
static int sys_getpid(struct regs *r)
{
	(void)r;
	return taskmgr_get_currpid();
}

/* sleep(ms) -- milliseconds. timer_wait() caps the value itself and ignores
*  anything <= 0, so no separate range check is needed here. */
static int sys_sleep(struct regs *r)
{
	sleep((int)r->ebx);
	return 0;
}

/* putch(c) -- a single character. No pointer involved, so nothing to
*  validate: every one of the 256 possible byte values is printable as far as
*  the VGA text buffer is concerned. */
static int sys_putch(struct regs *r)
{
	putch((unsigned char)(r->ebx & 0xFF));
	return 1;
}

/* uptime() -- milliseconds since boot. */
static int sys_uptime(struct regs *r)
{
	(void)r;
	return timer_get_ticks();
}


/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* Indexed by call number. A table instead of a switch, so that an unknown
*  number is a bounds check and a null test rather than a forgotten case --
*  the gaps up to SYSCALL_MAX are zero and answer with SYS_ENOSYS. */
static syscall_fn syscall_table[SYSCALL_MAX] =
{
	sys_exit,      /* SYS_EXIT   0 */
	sys_write,     /* SYS_WRITE  1 */
	sys_getpid,    /* SYS_GETPID 2 */
	sys_sleep,     /* SYS_SLEEP  3 */
	sys_putch,     /* SYS_PUTCH  4 */
	sys_uptime,    /* SYS_UPTIME 5 */
	0,
	0
};

void syscall_handler(struct regs *r)
{
	unsigned int num;
	int result;

	num = r->eax;
	syscall_calls++;

	if(num >= (unsigned int)SYSCALL_MAX || syscall_table[num] == 0)
	{
		r->eax = (unsigned int)SYS_ENOSYS;
		return;
	}

	result = syscall_table[num](r);

	/* SYS_EXIT has replaced *r with another task's context by now (or has
	*  already written its own error into eax). Writing the result here would
	*  clobber that task's eax, so the one call that does not return to its
	*  caller is also the one whose result is not delivered. */
	if(num == SYS_EXIT) return;

	r->eax = (unsigned int)result;
}

uint32_t syscall_count(void)
{
	return syscall_calls;
}

/* Installs the gate for vector 0x80. The flags byte is 0xEE, not the 0x8E
*  every other vector uses: bits 6-5 are the DPL, and 0xEE sets them to 3.
*  Without that, "int 0x80" from ring 3 raises a general protection fault
*  instead of entering the kernel, because the CPU compares the caller's CPL
*  against the gate's DPL.
*
*  It is a *trap* gate (type 0xF), not an interrupt gate. An interrupt gate
*  clears IF on entry, which would break SYS_SLEEP outright: timer_wait()
*  waits for timer_ticks to advance, but the timer IRQ that advances it can
*  never be delivered while interrupts are masked, and ring 3 cannot sti
*  itself. A trap gate leaves IF as the caller had it, so a system call is
*  preemptible.
*
*  The price is a theoretical time-of-check/time-of-use window: the pointer
*  validation below and the subsequent read are two separate passes, and a
*  preemption in between could see a page unmapped. That is acceptable here
*  because all tasks share one page directory anyway -- these checks stop
*  kernel reads and unmapped pages, they are not process isolation. Once
*  tasks get separate address spaces this needs revisiting.
*
*  This is the only descriptor ring 3 is allowed to reach. Every exception
*  and IRQ vector keeps DPL 0, so user code cannot software-trigger a page
*  fault or a timer interrupt and feed the kernel a made-up error code. */
void syscall_install(void)
{
	idt_set_gate(SYSCALL_VECTOR, (unsigned long)syscall_stub, GDT_KERNEL_CODE, 0xEF);
}
