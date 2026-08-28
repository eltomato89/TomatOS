/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Interrupt Service Routines installer and exceptions
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (friesenb@gmail.com)
*/

#include <system.h>
#include <string.h>
#include <stdio.h>

/* settextcolor() lives in scrn.c, but does not (yet) have a prototype in any
*  header. Declare it locally so that -Wall does not complain here. */
extern void settextcolor(unsigned char forecolor, unsigned char backcolor);

/* How often schedule() is called at most in order to leave a crashed task.
*  schedule() only switches tasks once their time slice (cpu_time) is used
*  up, and subtracts one slice per call. The upper bound makes sure that we
*  cannot possibly loop forever in interrupt context (the largest priority is
*  TASK_PRIORITY_REALTIME = 20). */
#define FAULT_SCHEDULE_TRIES 64

extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();

void isrs_install()
{
    idt_set_gate(0, (unsigned)isr0, 0x08, 0x8E);
    idt_set_gate(1, (unsigned)isr1, 0x08, 0x8E);
    idt_set_gate(2, (unsigned)isr2, 0x08, 0x8E);
    idt_set_gate(3, (unsigned)isr3, 0x08, 0x8E);
    idt_set_gate(4, (unsigned)isr4, 0x08, 0x8E);
    idt_set_gate(5, (unsigned)isr5, 0x08, 0x8E);
    idt_set_gate(6, (unsigned)isr6, 0x08, 0x8E);
    idt_set_gate(7, (unsigned)isr7, 0x08, 0x8E);

    idt_set_gate(8, (unsigned)isr8, 0x08, 0x8E);
    idt_set_gate(9, (unsigned)isr9, 0x08, 0x8E);
    idt_set_gate(10, (unsigned)isr10, 0x08, 0x8E);
    idt_set_gate(11, (unsigned)isr11, 0x08, 0x8E);
    idt_set_gate(12, (unsigned)isr12, 0x08, 0x8E);
    idt_set_gate(13, (unsigned)isr13, 0x08, 0x8E);
    idt_set_gate(14, (unsigned)isr14, 0x08, 0x8E);
    idt_set_gate(15, (unsigned)isr15, 0x08, 0x8E);

    idt_set_gate(16, (unsigned)isr16, 0x08, 0x8E);
    idt_set_gate(17, (unsigned)isr17, 0x08, 0x8E);
    idt_set_gate(18, (unsigned)isr18, 0x08, 0x8E);
    idt_set_gate(19, (unsigned)isr19, 0x08, 0x8E);
    idt_set_gate(20, (unsigned)isr20, 0x08, 0x8E);
    idt_set_gate(21, (unsigned)isr21, 0x08, 0x8E);
    idt_set_gate(22, (unsigned)isr22, 0x08, 0x8E);
    idt_set_gate(23, (unsigned)isr23, 0x08, 0x8E);

    idt_set_gate(24, (unsigned)isr24, 0x08, 0x8E);
    idt_set_gate(25, (unsigned)isr25, 0x08, 0x8E);
    idt_set_gate(26, (unsigned)isr26, 0x08, 0x8E);
    idt_set_gate(27, (unsigned)isr27, 0x08, 0x8E);
    idt_set_gate(28, (unsigned)isr28, 0x08, 0x8E);
    idt_set_gate(29, (unsigned)isr29, 0x08, 0x8E);
    idt_set_gate(30, (unsigned)isr30, 0x08, 0x8E);
    idt_set_gate(31, (unsigned)isr31, 0x08, 0x8E);
}

/* char* instead of unsigned char*: the texts go to printf("%s") and to
*  taskmgr_task_abort(const char*), where unsigned char* only produced
*  -Wpointer-sign warnings. */
char *exception_messages[] =
{
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",

    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",

    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",

    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

void fault_handler(struct regs *r)
{
	int pid;
	int i;
	struct regs *next;

	pid = taskmgr_get_currpid();

	/* If a running task trips over an exception, it is aborted and the
	*  context is redirected to the next runnable task. If we simply returned
	*  here, the iret at the end of isr_common_stub would jump straight back
	*  to the faulting instruction and re-trigger the exception forever. */
	if(pid >= 0 && r->int_no < 32)
	{
		taskmgr_task_abort(pid, r->err_code, exception_messages[r->int_no]);

		/* Short, visible feedback - a crash should not happen silently. */
		settextcolor(4,0);
		printf("Task %i aborted: %s (error code %i, eip %i)\n",
		       pid, exception_messages[r->int_no], r->err_code, r->eip);
		settextcolor(15,0);

		/* schedule() only switches tasks once their time slice is used up,
		*  and until then returns the context it was handed. So call it until
		*  a different context comes out - but only a bounded number of
		*  times, so that no endless loop can arise here inside the
		*  interrupt. The aborted task is no longer RUNNING and is skipped by
		*  the search. */
		next = r;
		for(i=0; i < FAULT_SCHEDULE_TRIES; i++)
		{
			next = schedule(r);
			if(next != r) break;
		}

		if(next != r)
		{
			/* Replace the context in place: isr_common_stub restores all
			*  registers from exactly this memory area and jumps into it via
			*  iret. This is the equivalent of the IRQ path's "mov esp, eax",
			*  only without touching the assembler. */
			*r = *next;
			return;
		}

		/* schedule() returns the unchanged context when no task is runnable
		*  any more. In that case there is nowhere the iret could jump to
		*  without triggering the exception again -> halt. */
		settextcolor(4,0);
		printf("No runnable task left - CPU HALT\n");
		settextcolor(15,0);
		dump(r);
		for(;;);
	}

	/* No task active: the kernel itself stumbled -> kernel panic. */
	if (r->int_no < 32)
	{

	  //cls();
	  settextcolor(4,0);
	  printf("                                   ^~~~^\n");
	  printf("                                  -     -\n");
	  printf("                                ..       ..\n");
	  printf("                               ~           ~\n");
	  printf("                              .             .\n");
	  printf("                             (;:   BOOM!   :;)\n");
	  printf("----------                    (:           :)\n");
	  printf("| PANIC! |                      ,         ,\n");
	  printf("|_  _____|                       :._   _.:\n");
	  printf("  |/                                | |\n");
	  printf(" 0                                (=====)\n");
	  printf("- -                                 | |\n");
	  printf(" |                                  | |\n");
	  printf("/ \\                              ((/   \\))\n");
	  settextcolor(2,0);
	  printf("----------------------------------------------------------\n");
	  settextcolor(15,0);
	  printf("The kernel made a boo-boo and couldn't fix it.\n");
	  printf("Error code %i\nError: %s\n\n", r->err_code, exception_messages[r->int_no]);
	  printf("PID: %i\n", pid);
	  printf("CPU HALT\n");

	} else {

		printf("fault_handler() r=%i\n", r->int_no);
	}

	dump(r);
	for(;;);
}

void dump(struct regs *r)
{

	printf("TomatOS CPU Dump\n");
	printf("gs: %i, fs: %i, es: %i, ds: %i\n", r->gs, r->fs, r->es, r->ds);
	printf("edi: %i, esi: %i, ebp: %i, esp: %i\n", r->edi, r->esi, r->ebp, r->esp);
	printf("eax: %i, ebx: %i, ecx: %i, edx: %i\n", r->eax, r->ebx, r->ecx, r->edx);
	printf("int_no: %i, err_code: %i\n", r->int_no, r->err_code);
	printf("eip: %i, cs: %i, ss: %i\n", r->eip, r->cs, r->ss);
	printf("eflags: %i, useresp: %i\n\n", r->eflags, r->useresp);

}
