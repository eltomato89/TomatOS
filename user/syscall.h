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
#define SYS_GETCH    6       /* getch()        -- one key, blocks           */
#define SYS_PEEKCH   7       /* peekch()       -- one key or 0, never blocks */
#define SYS_CLS      8       /* cls()                                       */
#define SYS_SETCOLOR 9       /* setcolor(foreground, background)            */
#define SYS_FSINFO  10       /* fsinfo(sys_fsinfo *out)                     */
#define SYS_STAT    11       /* stat(path, unsigned long *size)             */
#define SYS_READ    12       /* read(path, offset, len, buf) -> bytes read  */
#define SYS_READDIR 13       /* readdir(path, index, sys_dirent *out)       */
#define SYS_SPAWN   14       /* spawn(path, args, prio) -> pid              */
#define SYS_RESOLVE 15       /* resolve(name, unsigned long *ip)            */
#define SYS_CONNECT 16       /* connect(ip, port) -> handle                 */
#define SYS_SEND    17       /* send(handle, buf, len) -> bytes taken       */
#define SYS_RECV    18       /* recv(handle, buf, len) -> bytes, 0 = ended  */
#define SYS_CLOSE   19       /* close(handle)                               */
#define SYS_FCREATE 20       /* fcreate(path)                               */
#define SYS_FWRITE  21       /* fwrite(path, offset, len, buf) -> written    */
#define SYS_UNLINK  22       /* unlink(path)                                */
#define SYS_TRUNCATE 23      /* truncate(path, size)                        */

/* --- Error returns -- mirror of src/include/syscall.h ------------------- */
#define SYS_ENOSYS   (-1)    /* no such call number                         */
#define SYS_EFAULT   (-2)    /* argument pointer outside the caller's reach */
#define SYS_ENOENT   (-3)    /* no such file, or nothing is mounted         */
#define SYS_EIO      (-4)    /* the driver below refused                    */
#define SYS_EINVAL   (-5)    /* an argument makes no sense                  */
#define SYS_ENOMEM   (-6)    /* out of memory, or no task slot left         */
#define SYS_ENETDOWN (-7)    /* no card, or the stack is not configured     */
#define SYS_ETIMEDOUT (-8)   /* nothing answered in the time allowed        */
#define SYS_ECONNRESET (-9)  /* the peer refused or reset the connection    */
#define SYS_EEXIST  (-10)    /* the file is already there                   */
#define SYS_EROFS   (-11)    /* nothing mounted, or it cannot be written     */
#define SYS_ENOSPC  (-12)    /* the volume is full                          */

/* --- Structures crossing the gate -- mirror of src/include/syscall.h ----
*
*  Fixed layout on purpose: this program may be read from disk by a kernel
*  newer than the compiler that built it. Field widths are spelled out rather
*  than left to <stdint.h>, which does not exist here.
*/
#define SYS_NAME_MAX 16      /* 8.3 plus dot plus terminator, rounded up    */
#define SYS_DIRENT_DIR 0x01  /* the entry is a directory                    */

typedef struct
{
	char          name[SYS_NAME_MAX];
	unsigned long size;              /* bytes; 0 for a directory            */
	unsigned long flags;             /* SYS_DIRENT_DIR                      */
} sys_dirent;

typedef struct
{
	char          type[8];           /* "FAT12" / "FAT16", empty if unmounted */
	char          label[12];         /* volume label, may be empty          */
	unsigned long total_bytes;
	unsigned long free_bytes;
	unsigned long cluster_bytes;
	long          drive;             /* ATA drive it sits on, -1 if none    */
} sys_fsinfo;


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

/* Four arguments. esi joins the set for SYS_READ, whose (path, offset, len,
*  buffer) genuinely does not fit in three -- splitting it into an open/read
*  pair would have meant a file descriptor table in the kernel, which for a
*  read only filesystem buys nothing but state to get wrong. */
static __inline__ int syscall4(int num, int arg1, int arg2, int arg3, int arg4)
{
	int ret;
	__asm__ __volatile__("int $0x80"
	                     : "=a"(ret)
	                     : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4)
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

/* Waits for a key and returns it. Blocks: the task is descheduled while the
*  keyboard is idle, it does not spin. */
static __inline__ int sys_getch(void)
{
	return syscall0(SYS_GETCH);
}

/* The same, but returns 0 straight away when nothing was typed. For a program
*  that has to keep doing something while it watches the keyboard. */
static __inline__ int sys_peekch(void)
{
	return syscall0(SYS_PEEKCH);
}

/* Clears the screen and puts the cursor in the top left corner. */
static __inline__ int sys_cls(void)
{
	return syscall0(SYS_CLS);
}

/* Sets the colour of everything printed from here on. */
static __inline__ int sys_setcolor(int foreground, int background)
{
	return syscall2(SYS_SETCOLOR, foreground, background);
}

/* Fills in what is mounted. Returns 0, or SYS_ENOENT if nothing is. Named
*  after the call it makes rather than after the struct it fills, so that the
*  struct can keep the obvious name. */
static __inline__ int sys_statfs(sys_fsinfo *out)
{
	return syscall1(SYS_FSINFO, (int)out);
}

/* Size of a file in bytes. Returns 0 on success, SYS_ENOENT if there is no
*  such file. */
static __inline__ int sys_stat(const char *path, unsigned long *size)
{
	return syscall2(SYS_STAT, (int)path, (int)size);
}

/* Reads up to len bytes from offset. Returns how many were actually read,
*  which is short at the end of the file and 0 past it. There is no file
*  descriptor and no file position: every call names the file and says where
*  to start, so nothing has to be opened, closed or leaked. */
static __inline__ int sys_read(const char *path, unsigned long offset,
                               unsigned long len, void *buf)
{
	return syscall4(SYS_READ, (int)path, (int)offset, (int)len, (int)buf);
}

/* Entry number "index" of a directory, counting from 0. Returns 0 on
*  success and SYS_ENOENT once the directory is exhausted -- which is how a
*  caller knows to stop. */
static __inline__ int sys_readdir(const char *path, int index, sys_dirent *out)
{
	return syscall3(SYS_READDIR, (int)path, index, (int)out);
}

/* Loads a program from disk and starts it as a task of its own. args is the
*  command line it will find in argv, or 0 for none. Returns the new pid, or
*  a negative error. It does NOT wait for the program to finish. */
static __inline__ int sys_spawn(const char *path, const char *args, int prio)
{
	return syscall3(SYS_SPAWN, (int)path, (int)args, prio);
}

/* --- Networking --------------------------------------------------------
*
*  All five block, each with a timeout of its own. That is deliberate: a
*  non-blocking interface would need a way to wait for several things at once,
*  which this kernel's scheduler cannot express. Blocking costs nothing here --
*  the task is descheduled while it waits and everything else keeps running.
*/

/* Turns a name into an address. Returns 0 and fills in *ip, or a negative
*  error -- SYS_ENETDOWN when no DNS server is known, which means no lease has
*  been obtained yet. */
static __inline__ int sys_resolve(const char *name, unsigned long *ip)
{
	return syscall2(SYS_RESOLVE, (int)name, (int)ip);
}

/* Opens a TCP connection and waits for the handshake. Returns a handle, or a
*  negative error: SYS_ECONNRESET when the peer refused, SYS_ETIMEDOUT when
*  nothing answered at all. The address is in host order, as everywhere. */
static __inline__ int sys_connect(unsigned long ip, int port)
{
	return syscall2(SYS_CONNECT, (int)ip, port);
}

/* Sends. Returns how many bytes were taken, which can be fewer than offered --
*  the caller sends the rest afterwards. */
static __inline__ int sys_send(int handle, const void *buf, unsigned long len)
{
	return syscall3(SYS_SEND, handle, (int)buf, (int)len);
}

/* Receives. Blocks until something arrives. Returns the number of bytes, or
*  0 once the peer has closed and everything it sent has been read -- 0 is the
*  end of the stream, not an error, and a reader stops there. */
static __inline__ int sys_recv(int handle, void *buf, unsigned long len)
{
	return syscall3(SYS_RECV, handle, (int)buf, (int)len);
}

/* Closes our end. The connection may linger in the kernel afterwards, which is
*  the protocol working correctly and not a leak. */
static __inline__ int sys_close(int handle)
{
	return syscall1(SYS_CLOSE, handle);
}

/* --- Writing files -----------------------------------------------------
*
*  The same shape as sys_read(): a path, an offset and a length, and no handle
*  anywhere. Nothing has to be opened, so nothing can be left open.
*/

/* Creates an empty file. SYS_EEXIST if it is already there -- overwriting is
*  a decision this call will not make for you; sys_truncate(path, 0) is how
*  you say you meant it. The name must fit 8.3 or it is refused rather than
*  shortened into something you cannot name again. */
static __inline__ int sys_fcreate(const char *path)
{
	return syscall1(SYS_FCREATE, (int)path);
}

/* Writes at an offset, growing the file if it has to. Returns how many bytes
*  were written, which is short when the volume fills up -- look at the number.
*  Writing past the end zero-fills the gap. */
static __inline__ int sys_fwrite(const char *path, unsigned long offset,
                                 unsigned long len, const void *buf)
{
	return syscall4(SYS_FWRITE, (int)path, (int)offset, (int)len, (int)buf);
}

/* Removes a file. Refuses a directory. */
static __inline__ int sys_unlink(const char *path)
{
	return syscall1(SYS_UNLINK, (int)path);
}

/* Sets the length: shrinking frees what is past the end, growing zero-fills.
*  Truncating to 0 is the ordinary way to overwrite a file that exists. */
static __inline__ int sys_truncate(const char *path, unsigned long size)
{
	return syscall2(SYS_TRUNCATE, (int)path, (int)size);
}

#endif
