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

/* Gibt die nutzbaren Bereiche der Multiboot-Memory-Map aus. Bewusst kompakt:
*  der Textmodus hat nur 25 Zeilen, und die Bootmeldungen sollen hineinpassen.
*  Reservierte Bereiche interessieren hier nicht, die wertet pmm_init() aus.
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
		printf("Memory-Map: keine vom Bootloader erhalten\n");
		return;
	}

	summe_kib = 0;
	gezeigt = 0;
	offset = 0;

	printf("Memory-Map (nutzbar):\n");
	while(offset < mbi->mmap_length)
	{
		entry = (multiboot_mmap_entry *)(mbi->mmap_addr + offset);
		/* size zaehlt erst ab dem Feld dahinter, daher die zusaetzlichen 4 */
		offset += entry->size + 4;

		if(entry->type != MULTIBOOT_MEMORY_AVAILABLE) continue;
		/* Oberhalb von 4 GiB kommt ein 32-Bit-Kernel ohnehin nicht hin */
		if(entry->addr_high != 0) continue;

		if(entry->len_high != 0)
			laenge_kib = (0xFFFFFFFFu - entry->addr_low) / 1024u;
		else
			laenge_kib = entry->len_low / 1024u;

		summe_kib += laenge_kib;

		if(gezeigt < 6)
			printf("  0x%X  %i KiB  Typ %i\n", entry->addr_low, laenge_kib, entry->type);
		else if(gezeigt == 6)
			printf("  ...\n");
		gezeigt++;
	}

	printf("  Summe: %i KiB (%i MiB) in %i Bereichen\n",
		summe_kib, (summe_kib / 1024u), gezeigt);
}

int kernel(uint32_t magic, multiboot_info *mbi)
{
	extern void main();
	int task_console;

    init_video();
    printf("\n\nTomatOS/x86 boot v0.2\n");

	/* Ohne die Magic wissen wir nicht, ob ebx ueberhaupt auf eine
	*  Multiboot-Info-Struktur zeigt. Alles Weitere waere geraten. */
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC)
	{
		printf("Multiboot-Magic: %X erwartet, %X erhalten\n",
			MULTIBOOT_BOOTLOADER_MAGIC, magic);
		panic("Kein Multiboot-konformer Bootloader.\nTomatOS braucht die Speicherinformationen des Bootloaders.");
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
	/* Speicherverwaltung aufsetzen. Die Groesse kommt jetzt aus der
	*  Memory-Map des Bootloaders - kein destruktives Testschreiben quer durch
	*  den Adressraum mehr.
	*  Reihenfolge: pmm_init() braucht nur die Memory-Map, heap_init() setzt
	*  auf einem fertigen pmm auf, und beide muessen vor mt_install() stehen,
	*  damit auch Tasks Speicher anfordern koennen. */
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

	task_console = taskmgr_add_task( main, "KONSOLE", TASK_PRIORITY_REALTIME );
	taskmgr_task_start(task_console);

	/* Ab hier uebernimmt der Timer-IRQ: der Scheduler springt in den
	 * Konsolen-Task. Wir kehren nach start.asm zurueck, das dort in einer
	 * Endlosschleife auf den ersten Taskwechsel wartet. */
	return 0;
}
