/* TomatOS - User space system call interface
*  Desc: The whole "libc" a TomatOS program gets: inline "int 0x80" wrappers.
*
*  This header is deliberately self contained. A user program is built
*  separately from the kernel and must not depend on kernel internals -- it
*  is a different address space, a different privilege level and, once there
*  is a filesystem, a different binary on disk that may well be older than
*  the kernel booting it. So nothing from src/include/ is included here.
*
*  The call numbers below are COPIES of the ones in src/include/syscall.h.
*  They are part of the kernel's ABI and the kernel promises to keep them
*  stable ("Keep them stable, user code encodes them literally"), but if that
*  list ever changes, this one has to be changed in step -- there is no
*  compiler on either side that would catch a mismatch.
*
*  The convention, also from src/include/syscall.h:
*
*      eax  call number         eax  return value
*      ebx  first argument
*      ecx  second argument
*      edx  third argument
*
*  Vector 0x80 is the only IDT gate with DPL 3, so it is the only way in.
*/
#ifndef __USER_SYSCALL_H
#define __USER_SYSCALL_H

/* --- Call numbers -- mirror of src/include/syscall.h -------------------- */
#define SYS_EXIT     0       /* exit(status)   -- does not return          */
#define SYS_WRITE    1       /* write(text)    -- prints a NUL terminated
                              *                   string, returns its length */
#define SYS_GETPID   2       /* getpid()                                    */
#define SYS_SLEEP    3       /* sleep(ms)                                   */
#define SYS_PUTCH    4       /* putch(c)       -- one character             */
#define SYS_UPTIME   5       /* uptime()       -- milliseconds since boot   */

/* --- Error returns -- mirror of src/include/syscall.h ------------------- */
#define SYS_ENOSYS   (-1)    /* no such call number                         */
#define SYS_EFAULT   (-2)    /* argument pointer outside the caller's reach */


/* ------------------------------------------------------------------ */
/* The raw gate                                                        */
/* ------------------------------------------------------------------ */

/* One wrapper per argument count. They are __inline__ rather than plain
*  inline because the programs are built as -std=gnu89, where "inline" alone
*  still carries the old semantics.
*
*  The "memory" clobber is not paranoia: SYS_WRITE reads a buffer the
*  compiler has just filled in, so the stores must not be sunk past the trap.
*
*  ebx can be used as a plain input operand here because the programs are
*  built -fno-pic: nothing needs it as a GOT pointer. The kernel restores the
*  full register set from the stack frame its stub pushed, so ebx, ecx and
*  edx come back unchanged; only eax carries the result.
*/

static __inline__ int syscall0(int num)
{
	int ret;
	__asm__ __volatile__("int $0x80"
	                     : "=a"(ret)
	                     : "a"(num)
	                     : "memory");
	return ret;
}

static __inline__ int syscall1(int num, int arg1)
{
	int ret;
	__asm__ __volatile__("int $0x80"
	                     : "=a"(ret)
	                     : "a"(num), "b"(arg1)
	                     : "memory");
	return ret;
}

static __inline__ int syscall2(int num, int arg1, int arg2)
{
	int ret;
	__asm__ __volatile__("int $0x80"
	                     : "=a"(ret)
	                     : "a"(num), "b"(arg1), "c"(arg2)
	                     : "memory");
	return ret;
}

static __inline__ int syscall3(int num, int arg1, int arg2, int arg3)
{
	int ret;
	__asm__ __volatile__("int $0x80"
	                     : "=a"(ret)
	                     : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
	                     : "memory");
	return ret;
}


/* ------------------------------------------------------------------ */
/* The calls                                                           */
/* ------------------------------------------------------------------ */

/* Terminates the caller. The kernel replaces the saved context with the next
*  runnable task's, so the "int 0x80" never returns -- the loop afterwards
*  only exists so the compiler knows that too and does not warn about falling
*  off the end of a noreturn function. */
static __inline__ void sys_exit(int status)
{
	syscall1(SYS_EXIT, status);
	for(;;) { }
}

/* Prints a NUL terminated string. Returns the number of characters written,
*  or SYS_EFAULT if the string does not lie in readable user memory. The
*  kernel rejects anything longer than 1023 characters outright. */
static __inline__ int sys_write(const char *text)
{
	return syscall1(SYS_WRITE, (int)text);
}

/* Task id of the caller. */
static __inline__ int sys_getpid(void)
{
	return syscall0(SYS_GETPID);
}

/* Blocks for roughly ms milliseconds. Values <= 0 return immediately, large
*  values are capped by the kernel's timer. */
static __inline__ int sys_sleep(int ms)
{
	return syscall1(SYS_SLEEP, ms);
}

/* Prints a single character. Returns 1. */
static __inline__ int sys_putch(int c)
{
	return syscall1(SYS_PUTCH, c);
}

/* Milliseconds since boot. */
static __inline__ int sys_uptime(void)
{
	return syscall0(SYS_UPTIME);
}

#endif
