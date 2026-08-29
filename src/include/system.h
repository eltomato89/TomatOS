/* bkerndev - Bran's Kernel Development Tutorial
*  By:   Brandon F. (friesenb@gmail.com)
*  Desc: Global function declarations and type definitions
*
*  Notes: No warranty expressed or implied. Use at own risk. */
#ifndef __SYSTEM_H
#define __SYSTEM_H

#include "typedefs.h"

/* For addrspace_t, which taskmgr_task_space() hands out below. vmm.h pulls in
*  nothing but typedefs.h and carries its own inclusion guard, and it does not
*  refer back to system.h - so this cannot loop, and a file that includes
*  system.h alone still compiles. Including the header is preferable to
*  repeating the typedef here: a second "typedef uint32_t addrspace_t" would
*  be a redefinition in every translation unit that has both headers, and it
*  would silently drift the day vmm.h changes the underlying type. */
#include "vmm.h"

#define TASK_PRIORITY_REALTIME 20
#define TASK_PRIORITY_HIGH 10
#define TASK_PRIORITY_NORMAL 5
#define TASK_PRIORITY_LOW 1

#define TASK_STATE_RUNNING 0
#define TASK_STATE_SUSPENDED 1
#define TASK_STATE_ABORTED 2
/* A task that ran to its end and said so. Distinct from ABORTED on purpose:
*  both mean "finished, the slot may be recycled", but only one of them means
*  something went wrong. Reporting a program that returned 0 as "Aborted" is
*  what this exists to stop -- "ps" is read precisely when somebody suspects a
*  failure, and it should not manufacture one.
*
*  Anything that treats ABORTED as "this task is over" has to treat EXITED the
*  same way; the two differ only in what they are called and in whether an
*  error is worth printing next to them. */
#define TASK_STATE_EXITED 3

/* Waiting for something that is not the clock: a key, a packet, a disk. The
*  scheduler passes a blocked task over, and task_wake() puts it back to
*  RUNNING. Distinct from SUSPENDED, which is a decision somebody made about
*  the task from outside; this one the task made about itself and will undo
*  itself.
*
*  Anything that treats RUNNING as "alive" has to count this too, and anything
*  that treats not-RUNNING as "finished" must not. */
#define TASK_STATE_BLOCKED 4
#define TASK_STATE_NULL -1

/* This defines what the stack looks like after an ISR was running */
struct regs
{
    unsigned int gs, fs, es, ds;
    unsigned int edi, esi, ebp, esp, ebx, edx, ecx, eax;
    unsigned int int_no, err_code;
    unsigned int eip, cs, eflags, useresp, ss;    
};

typedef struct
{
	int pid;
	char name[32];
	int state;
	int priority;
	char error[128];
	int error_no;
	int cpu_time;
	
} task_settings;

typedef struct {
	unsigned int hours;
	unsigned int minutes;
	unsigned int seconds;
	
	unsigned int days;
	unsigned int months;
	unsigned int years;
} datetime;

/* MAIN.C */
extern void *memcpy(void *dest, const void *src, size_t count);
extern void *memset(void *dest, char val, size_t count);
extern unsigned short *memsetw(unsigned short *dest, unsigned short val, size_t count);
extern unsigned char inportb (unsigned short _port);
extern void outportb (unsigned short _port, unsigned char _data);
//extern int outportw (int port, int data);
extern void outportw(unsigned short port, unsigned short value);
extern int inportw (int port);
extern void disable ();
extern void enable ();

extern long register_read(char* reg);
extern void register_write(char* reg, long value);
extern void asm_test();

extern void debug_test();
extern struct regs* handle_interrupt(struct regs* cpu);
extern void switch_task(uint32_t esp);
extern void intr_common_handler();
extern struct regs* schedule(struct regs* cpu);
extern int taskmgr_get_taskcount();


/* CONSOLE.C */
extern void puts(char *text);
extern void putch(unsigned char c);
extern void cls(void);
extern void init_video(void);
extern void settextcolor(unsigned char forecolor, unsigned char backcolor);
extern void panic(char *desc);
extern void display_update_statusbar();

/* GDT.C */

/* Segment selectors, i.e. the byte offset of the descriptor inside the GDT.
*  The kernel pair is used as-is (RPL 0); ring 3 always adds RPL 3 to the
*  user selectors, so user code runs with CS 0x1B and SS 0x23 - see the
*  comment in gdt.c. */
#define GDT_ENTRIES      6
#define GDT_NULL         0x00
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x18
#define GDT_USER_DATA    0x20
#define GDT_TSS          0x28

/* Requested privilege level to or into a selector before it reaches a
*  segment register in ring 3. */
#define GDT_RPL_USER     0x03

extern void gdt_set_gate(int num, unsigned long base, unsigned long limit, unsigned char access, unsigned char gran);
extern void gdt_install();

/* Point the TSS at the kernel stack the CPU should switch to on the next
*  entry from ring 3. The scheduler calls this on every task switch with the
*  top of the incoming task's kernel stack. */
extern void tss_set_kernel_stack(uint32_t esp0);

/* IDT.C */
extern void idt_set_gate(unsigned char num, unsigned long base, unsigned short sel, unsigned char flags);
extern void idt_install();

/* ISRS.C */
extern void isrs_install();
extern void dump(struct regs *r);

/* IRQ.C */
extern void irq_install_handler(int irq, void (*handler)(struct regs *r));
extern void irq_uninstall_handler(int irq);
extern void irq_install();

/* TIMER.C */
extern void timer_wait(int ticks);
extern void timer_install();
extern void pic_install();
extern void sleep(int ticks);
extern void timer_setInterval(int hz);
extern int timer_get_ticks();
extern datetime cmos_readtime();

extern int timer_install_handler(void (*handler)(struct regs *r));
extern int timer_uninstall_handler(void (*handler)(struct regs *r));

/* KEYBOARD.C */
extern void keyboard_install();
extern unsigned char getch();
extern void kb_flush();

/* MULTITASKING.C */
extern void mt_install();
extern void taskmgr_list_tasks();
extern int taskmgr_add_task( void* tfunct, const char *name, int cpu_time);
extern int taskmgr_add_user_task(void *tfunct, const char *name, int prio);

/* The address space a task runs in, or 0 for an invalid pid and for a task
*  that runs in the kernel space (every ring 0 task does).
*
*  This is what makes it possible to furnish a task before it runs: create it
*  with taskmgr_add_user_task(), ask for its space here, map the program into
*  that space, then hand over the entry point with taskmgr_task_set_entry().
*  The returned value is a page directory the task manager still owns - map
*  into it, but never destroy it. */
extern addrspace_t taskmgr_task_space(int pid);

/* Sets the entry point of a task that has not started yet, i.e. writes eip in
*  its saved context. Returns 0 on success, a negative value if the pid is
*  invalid, the task has no saved context, it is not suspended (so it may
*  already have run), or entry is 0. */
extern int taskmgr_task_set_entry(int pid, uint32_t entry);

/* Moves a suspended ring 3 task's initial user stack pointer. Needed when the
*  loader has to put something -- an argument vector -- into the stack page
*  before the program starts, so that esp begins below it. Same conditions as
*  taskmgr_task_set_entry(). Returns 0, or -1 if the slot is not in a state
*  where writing its saved frame is safe. */
extern int taskmgr_task_set_stack(int pid, uint32_t user_esp);

/* The state of a slot: one of the TASK_STATE_* values, and TASK_STATE_NULL for
*  a pid that names none. For a caller that has to wait for a task to finish
*  rather than merely start it -- the shell running a program off the disk. */
extern int taskmgr_task_state(int pid);

/* ---------------------------------------------------------------------------
*  Waiting and waking
*
*  Everything in this kernel that waits for something currently polls: it
*  sleeps a few milliseconds, looks again, and sleeps again. That is written
*  down as a shortcoming in five places by now -- the network system calls, the
*  receive queue's drain task, getch(), and the ping, DHCP and DNS loops in the
*  shell -- and it costs exactly what it looks like it costs. Moving the
*  protocol work out of the interrupt handler took a ping's round trip from
*  0 ms to 34 ms, and the fix at the time was to have the waiting task drain
*  the queue itself, which works but is a workaround for the missing mechanism
*  rather than the mechanism.
*
*  A CHANNEL is any address: what is waited on is identified by a pointer,
*  usually to the thing itself -- a queue, a buffer, a connection. Nothing is
*  ever read through it. Two waiters on the same address are woken together,
*  and an address nobody waits on is a wake that costs a loop over the task
*  table and does nothing.
*
*  THE RACE THIS MUST SURVIVE is the lost wakeup: a task tests its condition,
*  finds it false, and before it blocks the interrupt that would have woken it
*  arrives and wakes nobody. The task then waits for something that already
*  happened. The idiom that avoids it is not optional and is why task_wait()
*  takes the caller's interrupt state:
*
*      flags = irq_save();                        // interrupts off
*      while(!condition)
*          if(!task_wait(&channel, timeout_ms))
*              break;                             // timed out
*      irq_restore(flags);
*
*  Two things make that safe. The condition is tested with interrupts off, so
*  nothing can change it between the test and the block. And it is tested
*  AGAIN after every wake -- so a wake that arrives in the window where the
*  task is marked blocked but has not yet stopped running is not lost, it just
*  costs one more turn around the loop. A caller that tests once and assumes
*  the wake means what it hoped will eventually hang, and the hang will be
*  rare and reproducible only under load, which is the worst kind.
*
*  Returns 1 when woken, 0 when the timeout expired. A timeout of 0 waits
*  forever, which is only ever right when something is guaranteed to wake it;
*  every caller that waits on hardware should give a bound.
*/
extern int task_wait(const void *channel, int timeout_ms);

/* Wakes everything waiting on the channel. Safe from interrupt context, and
*  that is the normal case -- the keyboard, the card and the disk all wake
*  from their handlers. It only changes states; the scheduler does the rest at
*  the next tick, so nothing here can switch tasks underneath a handler. */
extern void task_wake(const void *channel);

/* Gives up the rest of the current time slice. For a task that has work left
*  but nothing to wait for. */
extern void task_yield(void);

/* How many tasks are blocked right now, for a shell that wants to show that
*  waiting is what they are doing rather than spinning. */
extern int taskmgr_blocked_count(void);

/* The channel woken whenever a task ends, whether it exited or was aborted.
*
*  One channel for every death rather than one per pid, for the same reason
*  net_wait_channel() is one for the whole stack: a wake here means no more
*  than "a task ended, look again", and the waiting idiom above task_wait()
*  already requires the condition to be re-tested afterwards. A waiter that was
*  not waiting for this particular task simply blocks again. Per-pid channels
*  would buy fewer spurious wakes at the price of every waiter having to name
*  the task it cares about -- and a task that dies without waking the right
*  channel is a waiter that hangs, whereas an extra wake costs a loop.
*
*  Woken from taskmgr_task_exit() and taskmgr_task_abort(), which means it can
*  come from interrupt context -- the exception handler aborts through the same
*  path. Waking only changes states, so that is safe.
*
*  Before this existed, anything waiting for a task to finish polled: the
*  shell's wait for a program it started, and the reclaim task that takes the
*  screen back from a program that died holding it. Both are written down as
*  such in their own comments. */
extern const void *taskmgr_exit_channel(void);

extern int taskmgr_get_currpid();
extern void taskmgr_task_abort(int pid, int error_number, const char *error_descr);

/* The ordinary end of a task: it returned, or called exit(). status is what it
*  handed back and is kept for "ps" to show. Same effect on the slot as
*  taskmgr_task_abort() -- the task stops being scheduled and the slot is
*  recyclable -- and the same rule about not freeing anything here, since this
*  runs on the stack of the task it is ending. */
extern void taskmgr_task_exit(int pid, int status);
extern void taskmgr_task_start(int pid);
extern void taskmgr_task_suspend(int pid);
extern void taskmgr_killall();

extern void reboot();

#endif
