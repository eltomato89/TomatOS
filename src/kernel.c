/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: C Code entry
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (fiesenb@gmail.com)
*/

#include <system.h>
#include <stdio.h>
#include <hardware.h>
#include <mm.h>
#include <syscall.h>
#include <vmm.h>
#define cpuid(in, a, b, c, d) __asm__("cpuid": "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (in));

void *memcpy(void *dest, const void *src, size_t count)
{
    const char *sp = (const char *)src;
    char *dp = (char *)dest;
    for(; count != 0; count--) *dp++ = *sp++;
    return dest;
}

void *memset(void *dest, char val, size_t count)
{
    char *temp = (char *)dest;
    for( ; count != 0; count--) *temp++ = val;
    return dest;
}

unsigned short *memsetw(unsigned short *dest, unsigned short val, size_t count)
{
    unsigned short *temp = (unsigned short *)dest;
    for( ; count != 0; count--) *temp++ = val;
    return dest;
}

unsigned char inportb (unsigned short _port)
{
    unsigned char rv;
    __asm__ __volatile__ ("inb %1, %0" : "=a" (rv) : "dN" (_port));
    return rv;
}

void outportw(unsigned short port, unsigned short value)
{
	__asm__ __volatile__ ("outw %%ax,%%dx": :"dN"(port), "a"(value));
} 

void outportb (unsigned short _port, unsigned char _data)
{
    __asm__ __volatile__ ("outb %1, %0" : : "dN" (_port), "a" (_data));
}

void reboot() 
{ 
	printf("Killing all Processes ..\n");
	taskmgr_killall();
	sleep(500);
	
	printf("Rebooting ..");
	sleep(3000);

    unsigned int temp;
    do { 
       temp = inportb( 0x64 ); 
       if( temp & 1 ) inportb( 0x60 ); 
    }
	while ( temp & 2 ); 
 
    outportb(0x64, 0xFE); 
}


int checkCPUID(void)
{
	unsigned long eax, ecx;
   __asm__ __volatile__("pushf; pop %0; mov %0, %1; xor $0x200000, %0; push %0; popf; pushf; pop %0" : "=a" (eax), "=c" (ecx) : : "cc");
   if(eax == ecx)
   {
      return 0; //Not Supported
   } else {
      return 1; //Supported
   }
}

/* Prints the usable regions of the multiboot memory map. Deliberately compact:
*  text mode only has 25 lines, and the boot messages have to fit into them.
*  Reserved regions are of no interest here, pmm_init() evaluates those.
*
*  mbi is already a virtual pointer (kernel() converted it). Its mmap_addr
*  field, however, is still a raw PHYSICAL address from the bootloader and
*  gets its own P2V() below - once, into mmap, not per entry.
*/
static void print_memory_map(multiboot_info *mbi)
{
	multiboot_mmap_entry *entry;
	uint8_t *mmap;
	uint32_t offset;
	uint32_t len_kib;
	uint32_t total_kib;
	int shown;

	if(!(mbi->flags & MULTIBOOT_INFO_MEM_MAP))
	{
		printf("Memory map: none received from the bootloader\n");
		return;
	}

	/* The map itself has to lie in the directly mapped window, otherwise
	*  P2V() would produce an address that is not backed by anything. */
	if(mbi->mmap_addr == 0 || mbi->mmap_addr > DIRECT_MAP_LIMIT)
	{
		printf("Memory map: address 0x%X out of the direct mapping\n",
			mbi->mmap_addr);
		return;
	}

	mmap = (uint8_t *)P2V(mbi->mmap_addr);
	total_kib = 0;
	shown = 0;
	offset = 0;

	printf("Memory map (usable):\n");
	while(offset < mbi->mmap_length)
	{
		entry = (multiboot_mmap_entry *)(mmap + offset);
		/* size only counts from the field after it, hence the extra 4 */
		offset += entry->size + 4;

		if(entry->type != MULTIBOOT_MEMORY_AVAILABLE) continue;
		/* A 32-bit kernel cannot reach anything above 4 GiB anyway */
		if(entry->addr_high != 0) continue;

		if(entry->len_high != 0)
			len_kib = (0xFFFFFFFFu - entry->addr_low) / 1024u;
		else
			len_kib = entry->len_low / 1024u;

		total_kib += len_kib;

		if(shown < 6)
			printf("  0x%X  %i KiB  type %i\n", entry->addr_low, len_kib, entry->type);
		else if(shown == 6)
			printf("  ...\n");
		shown++;
	}

	printf("  Total: %i KiB (%i MiB) in %i regions\n",
		total_kib, (total_kib / 1024u), shown);
}

/* Entry point from start.asm. mbi_phys is the pointer the bootloader left in
*  ebx: a PHYSICAL address. The kernel runs in the higher half, so that value
*  is not a usable pointer - it is converted once, right here, and everything
*  below (print_memory_map(), pmm_init()) sees only the virtual mbi. */
int kernel(uint32_t magic, multiboot_info *mbi_phys)
{
	extern void main();
	multiboot_info *mbi;
	int task_console;

    init_video();
    printf("\n\nTomatOS/x86 boot v0.2\n");

	/* One line on the split, because it is exactly what one wants to see
	*  first when a higher half mapping goes wrong. */
	printf("Higher half: kernel 0x%X virt = 0x%X phys\n",
		KERNEL_VIRTUAL_START, V2P(KERNEL_VIRTUAL_START));

	/* Without the magic we do not know whether ebx points at a multiboot
	*  info structure at all. Everything beyond this would be guesswork.
	*  Checked before the conversion: P2V() on garbage yields garbage. */
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC)
	{
		printf("Multiboot magic: expected %X, got %X\n",
			MULTIBOOT_BOOTLOADER_MAGIC, magic);
		panic("No multiboot compliant bootloader.\nTomatOS needs the memory information from the bootloader.");
		return 0;
	}

	/* The magic only vouches for the register, not for the pointer. P2V()
	*  is defined for physical addresses inside the direct mapping alone, so
	*  anything above DIRECT_MAP_LIMIT (or a null pointer) cannot be turned
	*  into something dereferenceable - better to say so than to fault in
	*  print_memory_map() with no handler installed yet. */
	if((uint32_t)mbi_phys == 0 || (uint32_t)mbi_phys > DIRECT_MAP_LIMIT)
	{
		printf("Multiboot info at 0x%X, outside the direct mapping\n",
			(uint32_t)mbi_phys);
		panic("Multiboot info pointer out of range.\nTomatOS cannot reach the memory information.");
		return 0;
	}

	mbi = (multiboot_info *)P2V(mbi_phys);

	detect_cpu();
	if(checkCPUID()==1)
	   {
		printf("CPU Supported\n");
	   } else {
		printf("CPU Not Supported\n");
		panic("An unsupported CPU ID has been detected\nTomatOS requires a i386 or above");
		return 0;
	   }
	/* Set up memory management. The size now comes from the bootloader's
	*  memory map - no more destructive test writes across the address
	*  space.
	*  Order: pmm_init() only needs the memory map, vmm_init() takes its page
	*  tables from the ready pmm, heap_init() then grows into an already
	*  mapped range, and all three must come before mt_install() so that
	*  tasks can request memory as well.
	*
	*  Address spaces make that last point stricter rather than adding a step.
	*  From mt_install() on, a task may own a page directory of its own, and
	*  it shares only the upper quarter - everything from KERNEL_VIRTUAL_BASE
	*  up - with the kernel space vmm_init() built. Sharing works at
	*  directory-entry granularity (see vmm.h), so a kernel mapping that needs
	*  a brand new page table would end up in the directory that happens to be
	*  active and nowhere else. Every kernel page table must therefore exist
	*  before the first task space is created:
	*    - vmm_init() maps all usable RAM up front, so the kernel half is
	*      complete the moment it returns. Nothing has to be inserted between
	*      it and mt_install().
	*    - heap_init(), and every later heap growth, only takes frames out of
	*      that already mapped window via P2V() and never calls vmm_map(). It
	*      adds no page table, which is why the heap stays visible in every
	*      task space even though it keeps growing long after mt_install().
	*  Anything added later that maps a fresh kernel range belongs here, in
	*  front of mt_install(), for exactly this reason. vmm_create_space()
	*  itself needs nothing but a ready pmm and the direct mapping, both of
	*  which are in place well before the first task exists.
	*
	*  The kernel is linked for 0xC0100000 but loaded at 0x00100000, and by
	*  the time this function runs it already executes from the higher half:
	*  start.asm puts up a provisional mapping and jumps up there before
	*  calling us. Two consequences run through the whole boot path:
	*    - Every address that comes from outside the kernel is PHYSICAL and
	*      has to pass P2V() before it is dereferenced. That is the multiboot
	*      pointer above, the mmap_addr inside it, and later everything
	*      pmm_alloc_frame() hands out. The other direction, V2P(), is for
	*      what the MMU reads: page directory and table entries.
	*    - The two views differ by the constant KERNEL_VIRTUAL_BASE, and only
	*      for physical memory below DIRECT_MAP_LIMIT. Outside that window
	*      the macros mean nothing, hence the range checks.
	*
	*  Paging is reloaded here, before idt_install()/isrs_install(), i.e.
	*  without a page fault handler. That is deliberate:
	*    - The IDT gates reference a GDT selector, so pulling idt_install()
	*      forward would mean pulling gdt_install() forward as well and
	*      rearranging the whole driver block for no real gain.
	*    - vmm_init() builds the real directory over the same higher half
	*      window start.asm already provides and maps all usable RAM into it,
	*      so every pointer keeps its value across the write to CR3. There is
	*      no access in this window that could legitimately fault.
	*    - Should it fault anyway, it happens on the very first instruction
	*      after the new directory is loaded: a reproducible triple fault
	*      right after this message, not a subtle bug. A handler would not
	*      help either, because a broken mapping would just as likely swallow
	*      the handler itself.
	*  The page fault handler is for the faults that come later - null
	*  pointer dereferences, writes into the read-only kernel text - and
	*  those all happen well after isrs_install(). */
	print_memory_map(mbi);
	pmm_init(mbi);
	vmm_init();
	/* No extra line for the address space: the directory this prints IS the
	*  kernel space, so naming it here costs nothing, while a second message
	*  would cost one of the 25 rows the boot output has to fit into. */
	printf("Paging on: %i page tables, kernel space 0x%X\n",
		vmm_table_count(), vmm_kernel_space());
	heap_init();
	printf("%i MB Memory (%i KB) in %i Frames\n",
		(pmm_total_bytes() / (1024u * 1024u)),
		(pmm_total_bytes() / 1024u),
		pmm_total_frames());

	printf("\n\nLoading TomatOS/x86\n");
	printf("Loading Driver Components.\n");
    gdt_install();
    idt_install();
    isrs_install();
    irq_install();
	/* After idt_install(), which clears the table: syscall_install() adds
	*  vector 0x80 as the one gate ring 3 may reach. gdt_install() has
	*  already loaded the TSS the CPU needs to find a kernel stack when a
	*  ring 3 task traps into the kernel. */
	syscall_install();
	mt_install();
    pic_install();
    keyboard_install();
	printf("Initializing Serial Port\n");
	init_serial();
	printf("Enabling A20 Gate\n");
	//EnableA20Gate();
	printf("Protected Mode Kernel Running.\n\n");

	getchn(); // flush last keyboard character set by EnableA20Gate();
	__asm__ __volatile__ ("sti");

	task_console = taskmgr_add_task( main, "CONSOLE", TASK_PRIORITY_REALTIME );
	taskmgr_task_start(task_console);

	/* From here on the timer IRQ takes over: the scheduler jumps into the
	 * console task. We return to start.asm, which waits there in an
	 * endless loop for the first task switch. */
	return 0;
}
