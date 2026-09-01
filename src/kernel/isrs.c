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
#include <vmm.h>

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

/* Number of the page fault exception. */
#define EXCEPTION_PAGE_FAULT 14

/* Formats value as "0xXXXXXXXX" into buf and returns buf. buf needs 11 bytes.
*  Our printf() knows neither field widths nor %08X, and hextoa() would drop
*  the leading zeros ("0x0" instead of "0x00000000"). The padding is therefore
*  done by hand: addresses of the same length line up under each other, and a
*  bare "0" does not read like an address at all - which is exactly the case
*  that matters most, the null pointer. */
static char *hex32(uint32_t value, char *buf)
{
	static const char digits[] = "0123456789ABCDEF";
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for(i = 0; i < 8; i++)
	{
		buf[2 + i] = digits[(value >> (28 - 4 * i)) & 0x0F];
	}
	buf[10] = '\0';

	return buf;
}

/* Prints what the CPU tells us about a page fault: where it happened, in
*  which address space, which instruction caused it and what exactly was not
*  allowed. cr2 is handed in instead of read here, so that the value is the
*  one from the moment the exception was taken.
*
*  The address alone stopped being an answer once tasks got their own page
*  directories: below KERNEL_VIRTUAL_BASE the very same virtual address means
*  something different in every space, so the report names the active space -
*  the CR3 value, which is what identifies an address space (see vmm.h).
*
*  PF_USER decides how the whole thing reads. A fault from ring 3 is a task
*  reaching outside what its own space maps, i.e. the isolation working as
*  intended; a fault in the kernel is the kernel itself being wrong. Those
*  are two different findings and must not look alike in the log.
*
*  space is handed in for the same reason as cr2: by the time this runs the
*  task has already been aborted, and whoever cleans up after it may well
*  have loaded a different CR3 in the meantime. */
static void page_fault_report(struct regs *r, uint32_t cr2, addrspace_t space)
{
	char addr[11];
	char eip[11];
	char cr3[11];
	unsigned int err;
	int user;

	err = r->err_code;
	user = (err & PF_USER) != 0;

	printf("%s page fault at %s", user ? "Ring 3" : "Kernel",
	       hex32(cr2, addr));
	printf(" (eip %s", hex32(r->eip, eip));
	printf(", space %s)\n", hex32((uint32_t)space, cr3));

	/* Bit 0 separates the two fundamentally different cases: nothing is
	*  mapped there at all, or something is mapped but the access was not
	*  permitted (e.g. a write to a read-only page). */
	printf("  %s, ", (err & PF_PROTECTION) ? "protection" : "not present");
	printf("%s, ", (err & PF_WRITE) ? "write" : "read");
	printf("%s", user ? "user" : "supervisor");

	/* Both are rare and each has exactly one cause, so they are only
	*  mentioned when they actually occurred. */
	if(err & PF_RESERVED) printf(", reserved bit");
	if(err & PF_FETCH) printf(", fetch");
	printf("\n");

	/* Page zero is deliberately never mapped (see vmm.h). Anything faulting
	*  inside the first page is therefore a null pointer plus an offset - the
	*  offset being the member or array index the code was reaching for.
	*  That reading holds in every space and therefore comes first. */
	if(cr2 < PAGE_SIZE)
	{
		printf("  -> NULL pointer dereference, offset %u\n", (int)cr2);
	}
	else if(user)
	{
		printf("  -> address not available to ring 3 here, no kernel bug\n");
	}
}

/* Returns THE FRAME TO RETURN INTO, which is a different one when the faulting
*  task was aborted and another was elected. isr_common_stub does "mov esp,
*  eax" with it, exactly as irq_common_stub has always done with schedule()'s
*  answer.
*
*  It used to be void, and switched by copying the incoming context over the
*  outgoing one. See the block in start.asm for why that was wrong for a ring 0
*  task; in short, the copy restores every register except esp. */
struct regs *fault_handler(struct regs *r)
{
	int pid;
	int i;
	struct regs *next;
	uint32_t cr2;
	addrspace_t space;
	char eipbuf[11];

	pid = taskmgr_get_currpid();

	/* CR2 holds the faulting address, but only until the next page fault
	*  overwrites it. Read it right away, before any printf() runs. It is
	*  meaningless for every other exception and simply stays unused there. */
	cr2 = vmm_read_cr2();

	/* Same idea for the address space the fault happened in: the exception
	*  entered the kernel through an interrupt gate, so CR3 is still the one
	*  of the faulting task - but only until the task is aborted and the next
	*  one is switched in. Below KERNEL_VIRTUAL_BASE the address in cr2 is
	*  worth nothing without it. */
	space = vmm_current_space();

	/* If a running task trips over an exception, it is aborted and the
	*  context is redirected to the next runnable task. If we simply returned
	*  here, the iret at the end of isr_common_stub would jump straight back
	*  to the faulting instruction and re-trigger the exception forever. */
	if(pid >= 0 && r->int_no < 32)
	{
		taskmgr_task_abort(pid, r->err_code, exception_messages[r->int_no]);

		/* Short, visible feedback - a crash should not happen silently.
		*  eip is printed in hex: since the kernel moved into the higher
		*  half its addresses have the top bit set, and %i would render
		*  them as a negative number. */
		settextcolor(4,0);
		hex32(r->eip, eipbuf);
		printf("Task %i aborted: %s (error code %i, eip %s)\n",
		       pid, exception_messages[r->int_no], r->err_code, eipbuf);

		/* "Page Fault" alone says nothing about what went wrong, so the
		*  decoded details follow directly below it - including which
		*  address space the task was running in. */
		if(r->int_no == EXCEPTION_PAGE_FAULT) page_fault_report(r, cr2, space);

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
			/* Hand the new frame back and let the stub switch to it. The
			*  comment that used to sit here called copying it "the
			*  equivalent of the IRQ path's mov esp, eax, only without
			*  touching the assembler", and that was the bug: a copy moves
			*  every register except the stack pointer. */
			return next;
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
	  printf("Error code %i\nError: %s\n", r->err_code, exception_messages[r->int_no]);

	  /* Same here: for a page fault the generic name is replaced by the
	  *  address, the active space, the instruction and the decoded error
	  *  code. */
	  if(r->int_no == EXCEPTION_PAGE_FAULT) page_fault_report(r, cr2, space);

	  printf("\nPID: %i\n", pid);
	  printf("CPU HALT\n");

	} else {

		printf("fault_handler() r=%i\n", r->int_no);
	}

	dump(r);
	for(;;);
}

void dump(struct regs *r)
{
	char addr[11];

	printf("TomatOS CPU Dump\n");
	printf("gs: %i, fs: %i, es: %i, ds: %i\n", r->gs, r->fs, r->es, r->ds);
	printf("edi: %i, esi: %i, ebp: %i, esp: %i\n", r->edi, r->esi, r->ebp, r->esp);
	printf("eax: %i, ebx: %i, ecx: %i, edx: %i\n", r->eax, r->ebx, r->ecx, r->edx);
	printf("int_no: %i, err_code: %i\n", r->int_no, r->err_code);
	printf("eip: %i, cs: %i, ss: %i\n", r->eip, r->cs, r->ss);
	printf("eflags: %i, useresp: %i\n", r->eflags, r->useresp);

	/* CR2 is not part of struct regs, so without this line the one address
	*  that explains the crash would be missing from the dump. */
	if(r->int_no == EXCEPTION_PAGE_FAULT)
	{
		printf("cr2: %s (faulting address)\n", hex32(vmm_read_cr2(), addr));
	}

	printf("\n");

}
