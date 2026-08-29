/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Interrupt Request Management
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (friesenb@gmail.com)
*/

#include <system.h>

extern void irq0();
extern void irq1();
extern void irq2();
extern void irq3();
extern void irq4();
extern void irq5();
extern void irq6();
extern void irq7();
extern void irq8();
extern void irq9();
extern void irq10();
extern void irq11();
extern void irq12();
extern void irq13();
extern void irq14();
extern void irq15();

void *irq_routines[16] =
{
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};


/* This installs a custom IRQ handler for the given IRQ */
void irq_install_handler(int irq, void (*handler)(struct regs *r))
{
    irq_routines[irq] = handler;
}

/* This clears the handler for a given IRQ */
void irq_uninstall_handler(int irq)
{
    irq_routines[irq] = 0;
}

void irq_remap(void)
{
    outportb(0x20, 0x11);
    outportb(0xA0, 0x11);
    outportb(0x21, 0x20);
    outportb(0xA1, 0x28);
    outportb(0x21, 0x04);
    outportb(0xA1, 0x02);
    outportb(0x21, 0x01);
    outportb(0xA1, 0x01);
    outportb(0x21, 0x0);
    outportb(0xA1, 0x0);
}

void irq_install()
{
    irq_remap();

    idt_set_gate(32, (unsigned)irq0, 0x08, 0x8E);
    idt_set_gate(33, (unsigned)irq1, 0x08, 0x8E);
    idt_set_gate(34, (unsigned)irq2, 0x08, 0x8E);
    idt_set_gate(35, (unsigned)irq3, 0x08, 0x8E);
    idt_set_gate(36, (unsigned)irq4, 0x08, 0x8E);
    idt_set_gate(37, (unsigned)irq5, 0x08, 0x8E);
    idt_set_gate(38, (unsigned)irq6, 0x08, 0x8E);
    idt_set_gate(39, (unsigned)irq7, 0x08, 0x8E);

    idt_set_gate(40, (unsigned)irq8, 0x08, 0x8E);
    idt_set_gate(41, (unsigned)irq9, 0x08, 0x8E);
    idt_set_gate(42, (unsigned)irq10, 0x08, 0x8E);
    idt_set_gate(43, (unsigned)irq11, 0x08, 0x8E);
    idt_set_gate(44, (unsigned)irq12, 0x08, 0x8E);
    idt_set_gate(45, (unsigned)irq13, 0x08, 0x8E);
    idt_set_gate(46, (unsigned)irq14, 0x08, 0x8E);
    idt_set_gate(47, (unsigned)irq15, 0x08, 0x8E);
}

struct regs* irq_handler(struct regs *r)
{
	struct regs* new_cpu = r;
    /* This is a blank function pointer */
    void (*handler)(struct regs *r);

    /* Find out if we have a custom handler to run for this
    *  IRQ, and then finally, run it */
    handler = irq_routines[r->int_no - 32];
    if (handler)
    {
        handler(r);
    }

    /* If the IDT entry that was invoked was greater than 40
    *  (meaning IRQ8 - 15), then we need to send an EOI to
    *  the slave controller */
    if (r->int_no >= 40)
    {
        outportb(0xA0, 0x20);
    }
	if (r->int_no == 0x20) {
		/* No cli/sti around this, and the sti that used to be here was the
		*  cause of a general protection fault that took roughly eighty
		*  thousand timer ticks to show up.
		*
		*  The cli was redundant: every IRQ gate is an interrupt gate (0x8E,
		*  see irq_install above), so the CPU has already cleared IF, and the
		*  stub adds its own cli on top of that. Interrupts are off for the
		*  whole of this function whether we ask or not, and the iret at the
		*  end of irq_common_stub puts the interrupted code's IF back.
		*
		*  The sti was worse than redundant. schedule() commits the election
		*  before it returns -- current_task already names the INCOMING task
		*  while the CPU is still running on the OUTGOING task's kernel stack,
		*  and stays that way until the stub's "mov %eax,%esp" a few
		*  instructions later. Turning interrupts on inside that window, and
		*  then writing the EOI below so the PIC may deliver again, let a timer
		*  tick that had become pending during this handler arrive right there.
		*  It pushed its frame onto the outgoing task's stack and called
		*  schedule() a second time, which filed the OUTGOING task's registers
		*  as task_states[incoming]. The incoming task's real context was lost
		*  and replaced by a pointer into a stack whose owner kept using it, so
		*  ordinary calls overwrote it -- and the next time that context was
		*  dispatched, "pop %fs" got a data word.
		*
		*  It was survivable before only because a task held its time slice for
		*  up to twenty ticks, so most ticks re-elected the same task and the
		*  window did not matter. Blocking changed that: a task that blocks is
		*  descheduled on the very next tick, so nearly every tick now changes
		*  current_task, and the same old window started landing on the case
		*  that corrupts.
		*
		*  schedule() also guards against being entered nested, which keeps it
		*  correct if anything ever reopens a window like this one. Both are
		*  kept: this line stops the nesting, that guard makes nesting harmless. */
        new_cpu = schedule(r);
   }

    /* In either case, we need to send an EOI to the master
    *  interrupt controller too. Interrupts are still off here, so a tick that
    *  becomes deliverable at this instant is latched by the PIC and arrives
    *  after the iret -- by which time the CPU is on the incoming task's stack
    *  and the two agree with each other again. */
    outportb(0x20, 0x20);
	return new_cpu;
	
}

