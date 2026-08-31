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
#define SYSCALL_MAX      32      /* table size, keep in step with the list */

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

/* Writing to the filesystem. Deliberately the same shape as SYS_READ -- a
*  path, an offset and a length, no handle -- so that the read and write sides
*  of a file look alike and neither needs a descriptor table in the kernel.
*
*  The cost of that symmetry is that every call re-walks the directory and the
*  cluster chain, which for a program writing a page in 4 KB pieces is a real
*  cost. It is accepted for the same reason as on the read side: a descriptor
*  table is per-task state to allocate, validate, inherit and leak, and this is
*  a filesystem a shell writes files to, not a database. */
#define SYS_FCREATE     20       /* fcreate(path)                              */
#define SYS_FWRITE      21       /* fwrite(path, offset, len, buf) -> written   */
#define SYS_UNLINK      22       /* unlink(path)                               */
#define SYS_TRUNCATE    23       /* truncate(path, size)                       */

/* The screen, and what the user does to it.
*
*  These three are what let a graphical program be a program rather than
*  another thing compiled into the kernel. SYS_MAPFB is the first call in this
*  kernel that puts HARDWARE into a ring 3 address space -- the framebuffer is
*  a window onto the card, not memory, and a program holding it can draw
*  anything anywhere on the screen. That is the point of it, and it is also
*  why it is an ownership transfer rather than a mapping: the kernel's own
*  console lives on that screen too, and the two cannot both have it.
*
*  Ownership is released by SYS_UNMAPFB, and by the task ending -- the same
*  cleanup on exit the network calls already do, and for the same reason. A
*  program that crashes must not leave the machine with a screen nobody
*  repaints. */
#define SYS_MAPFB       24       /* mapfb(sys_fbinfo *out)                     */
#define SYS_UNMAPFB     25       /* unmapfb()                                  */
#define SYS_INPUT       26       /* input(sys_input_event *out, timeout_ms)    */

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
#define SYS_EEXIST      (-10)    /* the file is already there                  */
#define SYS_EROFS       (-11)    /* nothing mounted, or it cannot be written   */
#define SYS_ENOSPC      (-12)    /* the volume is full                         */
#define SYS_ENODEV      (-13)    /* there is no framebuffer to map             */
#define SYS_EBUSY       (-14)    /* another task holds the screen              */

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

/* What SYS_MAPFB hands back. Everything a program needs to put a pixel down
*  without guessing anything: where the memory is IN ITS OWN address space, how
*  far apart two rows are, and where the colour channels sit.
*
*  The pitch is not width * bytes. A card may pad a row and several do, so a
*  program computing the stride instead of reading it draws a picture that
*  shears diagonally. The channel positions are supplied for the same reason --
*  32 bpp is usually but not always 0x00RRGGBB. */
typedef struct
{
	uint32_t addr;               /* first byte, in the caller's address space */
	uint32_t size;               /* bytes mapped, pitch * height rounded up   */
	uint32_t width;              /* pixels                                    */
	uint32_t height;
	uint32_t pitch;              /* BYTES per row -- not width * bytes        */
	uint32_t bpp;                /* bits per pixel: 32, 24, 16 or 15          */
	uint8_t  red_pos;            /* channel positions and widths, in bits     */
	uint8_t  red_size;
	uint8_t  green_pos;
	uint8_t  green_size;
	uint8_t  blue_pos;
	uint8_t  blue_size;
	uint8_t  reserved[2];
} sys_fbinfo;

/* One thing the user did. Deliberately one type for the keyboard and the
*  mouse: a program with a pointer and a keyboard has to wait for whichever
*  comes first, and two queues would mean waiting on two things at once --
*  which this kernel's scheduler cannot express. */
#define SYS_INPUT_KEY    1
#define SYS_INPUT_MOUSE  2

typedef struct
{
	uint32_t type;               /* SYS_INPUT_KEY or SYS_INPUT_MOUSE          */
	uint32_t time_ms;            /* uptime when it happened                   */
	int32_t  x;                  /* mouse: where the pointer is now           */
	int32_t  y;
	int32_t  dx;                 /* mouse: how far it moved this time         */
	int32_t  dy;
	uint32_t buttons;            /* mouse: which are down now                 */
	uint32_t changed;            /* mouse: which changed, 0 for a plain move  */
	uint32_t key;                /* key: the character                        */
} sys_input_event;

typedef struct
{
	char     type[8];            /* "FAT12" / "FAT16", empty if not mounted  */
	char     label[12];          /* volume label, may be empty               */
	uint32_t total_kib;             /* KIBIBYTES, not bytes -- a byte count in
	                                 *  32 bits stops at 4 GB and cannot
	                                 *  describe the FAT32 media this now
	                                 *  mounts. Both round down. */
	uint32_t free_kib;
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
