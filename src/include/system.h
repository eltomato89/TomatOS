/* bkerndev - Bran's Kernel Development Tutorial
*  By:   Brandon F. (friesenb@gmail.com)
*  Desc: Global function declarations and type definitions
*
*  Notes: No warranty expressed or implied. Use at own risk. */
#ifndef __SYSTEM_H
#define __SYSTEM_H

#include "typedefs.h"

#define TASK_PRIORITY_REALTIME 20
#define TASK_PRIORITY_HIGH 10
#define TASK_PRIORITY_NORMAL 5
#define TASK_PRIORITY_LOW 1

#define TASK_STATE_RUNNING 0
#define TASK_STATE_SUSPENDED 1
#define TASK_STATE_ABORTED 2
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
extern void puts(unsigned char *text);
//extern void putch(unsigned char c);
extern void cls(void);
extern void display_update_statusbar();

/* GDT.C */
extern void gdt_set_gate(int num, unsigned long base, unsigned long limit, unsigned char access, unsigned char gran);
extern void gdt_install();

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
extern void sleep(int ticks);
extern void timer_setInterval(int hz);
extern datetime cmos_readtime();

extern int timer_install_handler(void (*handler)(struct regs *r));
extern int timer_uninstall_handler(void (*handler)(struct regs *r));

/* KEYBOARD.C */
extern void keyboard_install();
extern unsigned char getch();
extern void kb_flush();

/* MULTITASKING.C */
extern int mt_install();
extern int taskmgr_list_tasks();
extern int taskmgr_add_task( void* tfunct, const char *bezeichnung, int cpu_time);
extern int taskmgr_get_currpid();
extern void taskmgr_task_abort(int pid, int error_number, const char *error_descr);
extern void taskmgr_task_start(int pid);
extern void taskmgr_task_suspend(int pid);
extern int taskmgr_killall();

extern int prio_queue_pop();
extern void prio_queue_push(int pid);
extern int prio_queue_size();
extern void prio_queue_rebuild();

extern void reboot();

#endif
