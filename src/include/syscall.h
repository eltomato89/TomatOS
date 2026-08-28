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
#define SYSCALL_MAX      8       /* table size, keep in step with the list */

/* Call numbers. Keep them stable, user code encodes them literally. */
#define SYS_EXIT         0       /* exit(status)            -- does not return */
#define SYS_WRITE        1       /* write(text)             -- prints a string */
#define SYS_GETPID       2       /* getpid()                                   */
#define SYS_SLEEP        3       /* sleep(ms)                                  */
#define SYS_PUTCH        4       /* putch(c)                                   */
#define SYS_UPTIME       5       /* uptime()   -- milliseconds since boot      */

/* Error returns. Negative so a valid result stays distinguishable. */
#define SYS_ENOSYS       (-1)    /* no such call number                        */
#define SYS_EFAULT       (-2)    /* argument pointer outside the caller's reach */

/* Installs the IDT gate for 0x80 with DPL 3. Call after idt_install(). */
extern void syscall_install(void);

/* Entry point called by the assembly stub in start.asm. Reads the call
*  number and arguments out of the saved register set and writes the result
*  back into r->eax, so the iret hands it to the caller. */
extern void syscall_handler(struct regs *r);

/* Number of system calls served since boot -- shown by the shell. */
extern uint32_t syscall_count(void);

#endif
