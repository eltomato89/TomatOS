/* TomatOS - System calls
*  Desc: The gate between ring 3 and the kernel.
*
*  User code triggers "int 0x80" with the call number in eax and up to three
*  arguments in ebx, ecx and edx. The return value comes back in eax. That is
*  the classic Linux i386 convention, chosen because it needs nothing beyond
*  what this kernel already has -- sysenter would require MSR setup and a
*  fixed kernel entry stack.
*
*  The IDT gate for 0x80 is the only one with DPL 3: every other vector stays
*  DPL 0, so ring 3 cannot fake a page fault or an IRQ.
*/
#ifndef __SYSCALL_H
#define __SYSCALL_H

#include "typedefs.h"
#include "system.h"

#define SYSCALL_VECTOR   0x80
#define SYSCALL_MAX      24      /* table size, keep in step with the list */

/* Call numbers. Keep them stable, user code encodes them literally.
*
*  Arguments live in ebx, ecx, edx, esi, edi in that order -- the first three
*  are the original convention, esi and edi were added for SYS_READ, which
*  genuinely needs four. */
#define SYS_EXIT         0       /* exit(status)            -- does not return */
#define SYS_WRITE        1       /* write(text)             -- prints a string */
#define SYS_GETPID       2       /* getpid()                                   */
#define SYS_SLEEP        3       /* sleep(ms)                                  */
#define SYS_PUTCH        4       /* putch(c)                                   */
#define SYS_UPTIME       5       /* uptime()   -- milliseconds since boot      */
#define SYS_GETCH        6       /* getch()    -- one key, blocks              */
#define SYS_PEEKCH       7       /* peekch()   -- one key or 0, never blocks   */
#define SYS_CLS          8       /* cls()                                      */
#define SYS_SETCOLOR     9       /* setcolor(foreground, background)           */
#define SYS_FSINFO      10       /* fsinfo(sys_fsinfo *out)                    */
#define SYS_STAT        11       /* stat(path, uint32_t *size)                 */
#define SYS_READ        12       /* read(path, offset, len, buf) -> bytes read */
#define SYS_READDIR     13       /* readdir(path, index, sys_dirent *out)      */
#define SYS_SPAWN       14       /* spawn(path, args, prio) -> pid             */

/* Networking. These are what let a program do something on its own behalf
*  rather than asking the shell to do it -- the reason "fetch" is a program on
*  the disk and not another command compiled into the kernel.
*
*  All five BLOCK, with a timeout of their own, and that is a deliberate
*  simplification: a non-blocking interface would need a way to wait for one of
*  several things at once, which is a scheduler feature this kernel does not
*  have. Blocking is honest here because vector 0x80 is a trap gate -- the task
*  is descheduled while it waits and everything else keeps running.
*/
#define SYS_RESOLVE     15       /* resolve(name, uint32_t *ip)                */
#define SYS_CONNECT     16       /* connect(ip, port) -> handle                */
#define SYS_SEND        17       /* send(handle, buf, len) -> bytes taken      */
#define SYS_RECV        18       /* recv(handle, buf, len) -> bytes, 0 = ended */
#define SYS_CLOSE       19       /* close(handle)                              */

/* Error returns. Negative so a valid result stays distinguishable. */
#define SYS_ENOSYS       (-1)    /* no such call number                        */
#define SYS_EFAULT       (-2)    /* argument pointer outside the caller's reach */
#define SYS_ENOENT       (-3)    /* no such file, or nothing is mounted        */
#define SYS_EIO          (-4)    /* the driver below refused                   */
#define SYS_EINVAL       (-5)    /* an argument makes no sense                 */
#define SYS_ENOMEM       (-6)    /* out of memory, or no task slot left        */
#define SYS_ENETDOWN     (-7)    /* no card, or the stack is not configured    */
#define SYS_ETIMEDOUT    (-8)    /* nothing answered in the time allowed       */
#define SYS_ECONNRESET   (-9)    /* the peer refused or reset the connection   */

/* ---------------------------------------------------------------------------
*  Structures crossing the gate
*
*  These are ABI, not internal types: a program on disk may have been compiled
*  against an older kernel, so the layout is fixed deliberately rather than
*  left to the compiler. Every field is a fixed width, char arrays are sized
*  so that no padding is inserted, and nothing is ever appended in the middle.
*  The same declarations exist in user/syscall.h and have to move in step --
*  see the note at the top of that file for why they are copies and not a
*  shared include. */

#define SYS_NAME_MAX     16      /* 8.3 plus dot plus terminator, rounded up  */

#define SYS_DIRENT_DIR   0x01    /* the entry is a directory                  */

typedef struct
{
	char     name[SYS_NAME_MAX];
	uint32_t size;               /* bytes; 0 for a directory                 */
	uint32_t flags;              /* SYS_DIRENT_DIR                           */
} sys_dirent;

typedef struct
{
	char     type[8];            /* "FAT12" / "FAT16", empty if not mounted  */
	char     label[12];          /* volume label, may be empty               */
	uint32_t total_bytes;
	uint32_t free_bytes;
	uint32_t cluster_bytes;
	int32_t  drive;              /* ATA drive it sits on, -1 if none         */
} sys_fsinfo;

/* Installs the IDT gate for 0x80 with DPL 3. Call after idt_install(). */
extern void syscall_install(void);

/* Entry point called by the assembly stub in start.asm. Reads the call
*  number and arguments out of the saved register set and writes the result
*  back into r->eax, so the iret hands it to the caller. */
extern void syscall_handler(struct regs *r);

/* Number of system calls served since boot -- shown by the shell. */
extern uint32_t syscall_count(void);

#endif
