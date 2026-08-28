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
*/
static void print_memory_map(multiboot_info *mbi)
{
	multiboot_mmap_entry *entry;
	uint32_t offset;
	uint32_t laenge_kib;
	uint32_t summe_kib;
	int gezeigt;

	if(!(mbi->flags & MULTIBOOT_INFO_MEM_MAP))
	{
		printf("Memory map: none received from the bootloader\n");
		return;
	}

	summe_kib = 0;
	gezeigt = 0;
	offset = 0;

	printf("Memory map (usable):\n");
	while(offset < mbi->mmap_length)
	{
		entry = (multiboot_mmap_entry *)(mbi->mmap_addr + offset);
		/* size only counts from the field after it, hence the extra 4 */
		offset += entry->size + 4;

		if(entry->type != MULTIBOOT_MEMORY_AVAILABLE) continue;
		/* A 32-bit kernel cannot reach anything above 4 GiB anyway */
		if(entry->addr_high != 0) continue;

		if(entry->len_high != 0)
			laenge_kib = (0xFFFFFFFFu - entry->addr_low) / 1024u;
		else
			laenge_kib = entry->len_low / 1024u;

		summe_kib += laenge_kib;

		if(gezeigt < 6)
			printf("  0x%X  %i KiB  type %i\n", entry->addr_low, laenge_kib, entry->type);
		else if(gezeigt == 6)
			printf("  ...\n");
		gezeigt++;
	}

	printf("  Total: %i KiB (%i MiB) in %i regions\n",
		summe_kib, (summe_kib / 1024u), gezeigt);
}

int kernel(uint32_t magic, multiboot_info *mbi)
{
	extern void main();
	int task_console;

    init_video();
    printf("\n\nTomatOS/x86 boot v0.2\n");

	/* Without the magic we do not know whether ebx points at a multiboot
	*  info structure at all. Everything beyond this would be guesswork. */
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC)
	{
		printf("Multiboot magic: expected %X, got %X\n",
			MULTIBOOT_BOOTLOADER_MAGIC, magic);
		panic("No multiboot compliant bootloader.\nTomatOS needs the memory information from the bootloader.");
		return 0;
	}

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
	*  Order: pmm_init() only needs the memory map, heap_init() builds on a
	*  ready pmm, and both must come before mt_install() so that tasks can
	*  request memory as well. */
	print_memory_map(mbi);
	pmm_init(mbi);
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
