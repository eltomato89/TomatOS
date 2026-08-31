/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Global Descriptor Table Management
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (friesenb@gmail.com)
*/

#include <system.h>

/* Compile time assertion. A false condition declares an array of negative
*  size, which is an error the compiler reports at this line - so a mistake in
*  one of the packed layouts below breaks the build instead of the boot. */
#define BUILD_ASSERT(name, cond) typedef char name[(cond) ? 1 : -1]

/* Defines a GDT entry */
struct gdt_entry
{
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char base_middle;
    unsigned char access;
    unsigned char granularity;
    unsigned char base_high;
} __attribute__((packed));

struct gdt_ptr
{
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

/* The 32-bit Task State Segment, exactly as the CPU expects it. TomatOS never
*  switches hardware tasks, so almost every field here is dead weight that only
*  exists to make the structure the right size.
*
*  The one field that matters is the ss0/esp0 pair. Whenever the CPU takes an
*  interrupt, an exception or a syscall while the current privilege level is 3,
*  it cannot keep using the ring 3 stack: it reads ss0 and esp0 out of the TSS
*  and switches to that stack before pushing the interrupt frame. Get those two
*  wrong and the very first interrupt in user mode is a triple fault.
*
*  ss1/esp1 and ss2/esp2 would do the same for rings 1 and 2, which we do not
*  use. cr3, eip, eflags and the register images are only read by a hardware
*  task switch, which we never perform. They all stay zero. */
struct tss_entry
{
    unsigned int prev_tss;   /* link to the previous task, unused */
    unsigned int esp0;       /* stack pointer the CPU loads on a ring 3 -> 0 switch */
    unsigned int ss0;        /* stack segment for the same switch */
    unsigned int esp1;
    unsigned int ss1;
    unsigned int esp2;
    unsigned int ss2;
    unsigned int cr3;
    unsigned int eip;
    unsigned int eflags;
    unsigned int eax;
    unsigned int ecx;
    unsigned int edx;
    unsigned int ebx;
    unsigned int esp;
    unsigned int ebp;
    unsigned int esi;
    unsigned int edi;
    unsigned int es;
    unsigned int cs;
    unsigned int ss;
    unsigned int ds;
    unsigned int fs;
    unsigned int gs;
    unsigned int ldt;
    unsigned short trap;        /* bit 0 raises a debug exception on task switch */
    unsigned short iomap_base;  /* offset of the I/O permission bitmap */
} __attribute__((packed));

/* If either of these fires, a field above lost its packing or its width.
*  8 bytes per descriptor and 104 bytes for the TSS are what the hardware
*  reads, not what we would like it to read. */
BUILD_ASSERT(assert_gdt_entry_is_8_bytes, sizeof(struct gdt_entry) == 8);
BUILD_ASSERT(assert_tss_entry_is_104_bytes, sizeof(struct tss_entry) == 104);

/* Six descriptors now: null, kernel code, kernel data, user code, user data
*  and the TSS. gdt_flush() in start.asm still works unchanged - it reloads the
*  data segments with 0x10 and far-jumps to 0x08, and both selectors keep their
*  slot and their meaning. */
struct gdt_entry gdt[GDT_ENTRIES];
struct gdt_ptr gp;

/* The one and only TSS. Static storage, so it starts out zeroed. */
static struct tss_entry tss;

/* Fallback kernel stack for esp0, used until the scheduler knows better.
*  It cannot be the boot stack from start.asm: that one is in use by whoever
*  gets interrupted, and letting the CPU push an interrupt frame onto a stack
*  that is already live would overwrite the interrupted kernel's own frames.
*  This buffer belongs to nobody, so an early trap out of ring 3 lands
*  somewhere valid instead of at address zero. */
static unsigned char tss_boot_stack[4096];

/* inside start.asm. Reload the new segment registers */
extern void gdt_flush();

/* Setup a descriptor in the Global Descriptor Table */
void gdt_set_gate(int num, unsigned long base, unsigned long limit, unsigned char access, unsigned char gran)
{
    /* Setup the descriptor base address */
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    /* Setup the descriptor limits */
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F);

    /* Set up granularity and access flags */
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access = access;
}

/* Points the TSS descriptor at our structure and loads it into the task
*  register. Must run after gdt_flush(), because ltr looks the selector up in
*  the GDT that is current at that moment.
*
*  About the base address: descriptor bases are LINEAR addresses, i.e. what
*  comes out of segmentation and goes into paging - not physical ones. Every
*  segment here is flat with base 0, so linear equals virtual, and the value
*  the CPU wants is the plain higher-half pointer &tss (0xC01.....). No V2P()
*  in sight. The same reasoning already applies to gp.base below, which has
*  been the virtual address of the gdt array all along and works. Handing the
*  CPU V2P(&tss) instead would make it read the TSS from a low address that
*  paging does not map to our structure at all.
*
*  Limit is sizeof(tss) - 1 with byte granularity (gran 0x00), so the segment
*  covers exactly the 104 bytes of the structure and not one byte more.
*  Access 0xE9 = present, DPL 0, system descriptor, type 9 (32-bit TSS,
*  available). DPL 0 is right even though ring 3 code causes the TSS to be
*  read: the CPU reads it on its own behalf, no user code ever loads the
*  selector. */
static void tss_install(void)
{
    /* Kernel stack the CPU switches to when it leaves ring 3. ss0 is the
    *  kernel data selector; esp0 starts at the top of our fallback stack and
    *  is overwritten by tss_set_kernel_stack() on every task switch. */
    tss.ss0 = GDT_KERNEL_DATA;
    tss.esp0 = (unsigned int)tss_boot_stack + sizeof(tss_boot_stack);

    /* No I/O permission bitmap. Setting iomap_base to the size of the
    *  structure puts it past the segment limit, which the CPU reads as "the
    *  bitmap is absent" - every I/O port from ring 3 then simply traps.
    *  Leaving this at 0 would instead point the bitmap at the start of the
    *  TSS, and the CPU would happily interpret our own fields as permission
    *  bits. */
    tss.iomap_base = sizeof(struct tss_entry);

    gdt_set_gate(GDT_TSS / 8, (unsigned long)&tss,
                 sizeof(struct tss_entry) - 1, 0xE9, 0x00);

    /* Load the task register. The selector's RPL must be 0. */
    __asm__ __volatile__ ("ltr %0" : : "r" ((unsigned short)GDT_TSS));
}

/* Tells the CPU which kernel stack to switch to the next time it leaves
*  ring 3. The scheduler calls this on every context switch with the top of
*  the stack that belongs to the task it is about to resume - otherwise two
*  tasks would take their interrupts on the same stack and corrupt each
*  other's saved state. */
void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = (unsigned int)esp0;
}

void gdt_install()
{
    /* Setup the GDT pointer and limit */
    gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gp.base = (unsigned int)&gdt;

    /* NULL descriptor */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* Code Segment
       Base address = 0
       Limit 4 GB (uses 4 Kbyte granularity / 32-bit opcodes)
    */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* Data Segment. EXACTLY the same as code segment, but the descriptor type in
       this entry's access byte says it's a Data Segment */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* User code and user data. Same flat 4 GB window and the same granularity
       byte as the kernel pair - the separation between kernel and user memory
       is paging's job (PAGE_USER), not segmentation's. The only difference is
       the DPL in bits 5-6 of the access byte: 0x9A/0x92 carry DPL 0, and
       0xFA/0xF2 are the same descriptors with DPL 3.

       These two are the ones people get wrong when loading them, because a
       segment register holds the selector AND the requested privilege level in
       its low two bits. Ring 3 therefore never runs with the bare table
       offsets 0x18 and 0x20, it runs with RPL 3 or'ed in:

           CS = 0x18 | 3 = 0x1B
           SS = DS = ES = FS = GS = 0x20 | 3 = 0x23

       A plain 0x18 in CS would be a ring 0 selector and general-protect at
       once (the CPU refuses a CPL/RPL mismatch), and a plain 0x20 in SS gives
       the same fault. Kernel selectors are used with RPL 0, so 0x08 and 0x10
       stay exactly as written. */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* Flush out the old GDT and install the new changes! */
    gdt_flush();

    /* Slot 5 and the task register. Only valid once the GDT above is live. */
    tss_install();
}
