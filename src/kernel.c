/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: C Code entry
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (fiesenb@gmail.com)
*/

#include <system.h>
#include <stdio.h>
#include <string.h>
#include <hardware.h>
#include <mm.h>
#include <syscall.h>
#include <vmm.h>
#include <fbcon.h>
#include <exec.h>
#include <ata.h>
#include <fat.h>
#include <pci.h>
#include <rtl8139.h>
#include <net.h>
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

/* --- The framebuffer the bootloader handed over ---------------------------
*
*  A graphics mode cannot be established from protected mode: a VBE mode needs
*  int 0x10 and that is real mode only. Whoever boots us therefore sets the
*  mode and describes the result in the multiboot info, and all this kernel
*  can do is read what it got. GRUB reports the plain EGA text buffer here
*  when no video mode was requested; our own stage 2 reports whatever VBE mode
*  it selected before leaving real mode.
*
*  The description is kept in file statics with the accessors below rather
*  than in a struct in a header, because no header may be touched in this
*  step. What this really wants to be is one "struct framebuffer" and a
*  "const struct framebuffer *fb_info(void)" in a src/include/framebuffer.h -
*  seven accessors for what is one immutable record are six too many, and a
*  framebuffer console would have to call all of them to draw a single pixel.
*  Until that header exists, a user declares what it needs:
*
*      extern uint32_t fb_base(void);          physical base address
*      extern uint32_t fb_pitch_bytes(void);   bytes per row
*      extern uint32_t fb_pixel_width(void);
*      extern uint32_t fb_pixel_height(void);
*      extern uint32_t fb_bits_per_pixel(void);
*      extern uint32_t fb_kind(void);          MULTIBOOT_FRAMEBUFFER_*
*      extern int      fb_usable(void);        safe to write to right now
*/
static uint32_t fb_addr;	/* physical base, 0 = nothing was reported   */
static uint32_t fb_pitch;	/* bytes per row - NOT width * bpp / 8       */
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_bpp;
static uint32_t fb_type = MULTIBOOT_FRAMEBUFFER_EGA_TEXT;
static int fb_reachable;	/* P2V() can name it at all                  */
static int fb_mapped;		/* and the page is actually present          */

uint32_t fb_base(void)           { return fb_addr; }
uint32_t fb_pitch_bytes(void)    { return fb_pitch; }
uint32_t fb_pixel_width(void)    { return fb_width; }
uint32_t fb_pixel_height(void)   { return fb_height; }
uint32_t fb_bits_per_pixel(void) { return fb_bpp; }
uint32_t fb_kind(void)           { return fb_type; }

/* The only question a caller that wants to draw should ask: a framebuffer
*  that is a text buffer, that P2V() cannot name, or whose pages are not
*  present is not something to write into. */
int fb_usable(void)
{
	return fb_addr != 0
		&& fb_type != MULTIBOOT_FRAMEBUFFER_EGA_TEXT
		&& fb_reachable
		&& fb_mapped;
}

/* Hands the mode over to the framebuffer console, before a single character
*  has been printed. This is the first thing kernel() does with the multiboot
*  info, and the position is the point of it.
*
*  fbcon_describe() touches nothing but a shadow buffer in .bss. It needs no
*  mapping, no vmm and no heap, so the only thing keeping it from being the
*  very first statement of the kernel is that mbi has to be a validated and
*  converted pointer first - which is why the two multiboot checks moved
*  ahead of the banner rather than this call moving behind them.
*
*  What is at stake is everything printed before it. On a machine that came
*  up in a graphics mode there is no text buffer behind 0xB8000 that anybody
*  looks at: characters written there are simply gone, and they are not in
*  the shadow buffer either, so fbcon_activate() cannot replay them later.
*  Every line that would tell one why a boot went wrong is exactly the kind
*  of line one loses that way.
*
*  init_video() is behind this call for the same reason. Its cls() is console
*  output like any other - scrn.c asks fbcon_active() which screen to clear -
*  so it only clears the right one once the console knows what it drives. Run
*  the other way round, cls() would blank a text buffer nobody displays and
*  leave the shadow buffer full of the NUL bytes .bss starts out with.
*
*  A framebuffer above 4 GiB is passed on as address zero rather than as its
*  truncated low half: a 32-bit kernel cannot map it at all, and half an
*  address is worse than none. The console then stays in text mode, which is
*  what the no-framebuffer case below does as well - both are documented as
*  safe by fbcon.h. */
static void framebuffer_describe(multiboot_info *mbi)
{
	uint32_t addr;

	if(!(mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER))
	{
		fbcon_describe(0, 0, 0, 0, 0, MULTIBOOT_FRAMEBUFFER_EGA_TEXT);
		return;
	}

	addr = (mbi->framebuffer_addr_high == 0) ? mbi->framebuffer_addr_low : 0;

	fbcon_describe(addr, mbi->framebuffer_pitch,
		mbi->framebuffer_width, mbi->framebuffer_height,
		(uint8_t)mbi->framebuffer_bpp, (uint8_t)mbi->framebuffer_type);
}

/* Records what the bootloader reported and says so in one line.
*
*  Needs a live vmm: the second of the two checks below asks vmm_is_mapped(),
*  which has nothing to answer with before vmm_init() has built the tables.
*  Hence its place in kernel() and not next to print_memory_map(). */
static void framebuffer_init(multiboot_info *mbi)
{
	const char *kind;
	const char *status;

	if(!(mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER))
	{
		/* GRUB Legacy never sets the bit, and GRUB 2 leaves it out when
		*  the multiboot header asks for no video mode. That is not a
		*  failure - it means the machine is still in the VGA text mode
		*  scrn.c drives, which is what we want today. */
		printf("Framebuffer: none reported, VGA text mode\n");
		return;
	}

	fb_type   = mbi->framebuffer_type;
	fb_width  = mbi->framebuffer_width;
	fb_height = mbi->framebuffer_height;
	fb_bpp    = mbi->framebuffer_bpp;
	fb_pitch  = mbi->framebuffer_pitch;
	fb_addr   = mbi->framebuffer_addr_low;

	switch(fb_type)
	{
		case MULTIBOOT_FRAMEBUFFER_INDEXED:  kind = "indexed";  break;
		case MULTIBOOT_FRAMEBUFFER_RGB:      kind = "RGB";      break;
		case MULTIBOOT_FRAMEBUFFER_EGA_TEXT: kind = "EGA text"; break;
		default:                             kind = "unknown";  break;
	}

	/* Two questions, independent of each other, and neither may be assumed.
	*
	*  1. Can P2V() name it? The direct mapping covers physical memory below
	*     DIRECT_MAP_LIMIT (1 GiB) and nothing else, so a 64-bit address -
	*     framebuffer_addr_high non-zero - or a low half beyond that limit
	*     has no virtual alias at all. There is no address to hand out then,
	*     and none is invented: reaching such a framebuffer means giving it a
	*     mapping of its own with vmm_map(), which is a follow-up step. */
	fb_reachable = (mbi->framebuffer_addr_high == 0
			&& fb_addr != 0
			&& fb_addr <= DIRECT_MAP_LIMIT);

	/*  2. Is it mapped at all? vmm_init() maps RAM as the memory map reports
	*     it, but a framebuffer is memory mapped hardware and need not appear
	*     in that map - a card answering at 0xFD000000 typically does not, so
	*     its pages are simply absent. Writing there would page fault, which
	*     is why this is checked and reported rather than assumed. Only worth
	*     asking where P2V() applies in the first place. */
	fb_mapped = fb_reachable ? vmm_is_mapped((uint32_t)P2V(fb_addr)) : 0;

	if(mbi->framebuffer_addr_high != 0)
		status = "above 4 GiB, unreachable";
	else if(fb_addr == 0)
		status = "no address reported";
	else if(!fb_reachable)
		status = "outside the direct mapping";
	else if(!fb_mapped)
		status = "not mapped";
	else
		status = "mapped";

	/* One line, assembled in three calls because the 64-bit case cannot use
	*  the same conversion: %X prints an unpadded 32-bit value, so gluing two
	*  of them together would silently misrepresent the address. */
	printf("Framebuffer: %s %ix%i %i bpp, pitch %i, at ",
		kind, fb_width, fb_height, fb_bpp, fb_pitch);

	if(mbi->framebuffer_addr_high != 0)
		printf("0x%X:0x%X", mbi->framebuffer_addr_high, fb_addr);
	else
		printf("0x%X", fb_addr);

	/* The last field is the one thing here that is not a property of the
	*  hardware but a decision: fbcon_activate() ran a few lines further up
	*  and either took the screen over or did not. Worth saying in the same
	*  line, because the two halves explain each other - "outside the direct
	*  mapping, console active" is not a contradiction, it is the console
	*  having mapped what P2V() cannot name. Two words rather than the
	*  geometry, so the EGA text case stays inside 80 columns; the shell
	*  reports the full mode with "gfx -i". */
	printf(" (%s, %s)\n", status,
		fbcon_active() ? "console active" : "text mode");
}

/* Reports the modules the bootloader loaded alongside the kernel, in exactly
*  one line - the boot output has to fit into 25 rows, and the names are the
*  only thing one cannot look up again later with the shell.
*
*  Without modules nothing is printed at all: a kernel booted without a
*  single program is the normal case while there is no filesystem, and a
*  line saying "none" would cost a row for saying nothing. The shell reports
*  an empty list itself when someone actually asks for one.
*
*  The names are cut off once the line gets close to the 80 column width,
*  because a wrapped line costs a second row - exactly what we are avoiding. */
static void print_modules(void)
{
	int count;
	int i;
	int len;
	int used;
	const char *name;

	count = exec_module_count();
	if(count <= 0) return;

	printf("Modules: %i (", count);

	used = 0;
	for(i = 0; i < count; i++)
	{
		name = exec_module_name(i);
		if(name == 0) name = "?";
		len = (int)strlen(name);

		/* The first name is always printed, however long it is - a line
		*  that only says "(...)" would be worse than a long one. */
		if(i > 0 && used + 2 + len > 48)
		{
			printf(", ...");
			break;
		}

		if(i > 0)
		{
			printf(", ");
			used += 2;
		}

		printf("%s", name);
		used += len;
	}

	printf(")\n");
}

/* At most max characters of s. printf() knows no precision, and both strings
*  that end up on the disk lines below come from somewhere else - a model
*  string out of the drive's IDENTIFY data, an error text out of the
*  filesystem. A line that runs past 80 columns wraps and costs a second row,
*  which is exactly what the whole boot output is trying not to do. */
static void print_capped(const char *s, int max)
{
	int i;

	if(s == 0) return;

	for(i = 0; i < max && s[i] != '\0'; i++) putch((unsigned char)s[i]);
}

/* Brings the disk up and mounts what is on it, in at most two lines: one for
*  the drive, one for the filesystem.
*
*  Which drive: the first one that carries something mountable, tried from 0
*  upwards. Drive 0 is where an image handed to qemu as -hda ends up, so it is
*  the answer in practically every case here - but insisting on it would be
*  wrong on real hardware, where drive 0 can just as well be an empty slot or
*  the CD-ROM the machine booted from while the data sits on the slave. Trying
*  the others costs one IDENTIFY per drive, all of them already done by
*  ata_init(), and a boot sector read for each drive that is actually there.
*  The first mount that succeeds ends the search, so a machine with two
*  filesystems gets the lower drive - deterministic, and changeable from the
*  shell later.
*
*  No disk at all is a normal case, not a failure: "make run" without an image
*  boots exactly like this, and so does hardware with nothing attached. It
*  prints nothing at all then, for the same reason print_modules() says
*  nothing about an empty module list - a row spent on "no disk" is a row
*  taken away from something that has news. Whatever the outcome, this returns
*  and the shell comes up: ata_init() is documented as safe with no controller
*  present, and a drive that answers nothing simply is not present(). */
static void disk_init(void)
{
	uint32_t sectors;
	int drive;
	int first;		/* first drive that exists at all        */
	int mounted;		/* first one that holds a filesystem     */

	ata_init();

	first   = -1;
	mounted = -1;

	for(drive = 0; drive < ATA_MAX_DRIVES; drive++)
	{
		if(!ata_present(drive)) continue;

		if(first < 0) first = drive;

		if(fat_mount(drive) == 0)
		{
			mounted = drive;
			break;
		}
	}

	if(first < 0) return;

	drive   = (mounted >= 0) ? mounted : first;
	sectors = ata_sectors(drive);

	/* 512 byte sectors, so 2048 of them make a MiB. The size is printed
	*  rather than the sector count, which says nothing at a glance - and in
	*  KiB below a megabyte, because a 1.44 MiB floppy image is a perfectly
	*  normal FAT12 medium here and "0 MiB" would be a useless line. */
	printf("Disk: hd%i, ", drive);
	if(sectors >= 2048u)
		printf("%i MiB, ", (int)(sectors / 2048u));
	else
		printf("%i KiB, ", (int)(sectors / 2u));
	print_capped(ata_model(drive), 40);
	printf("\n");

	if(mounted < 0)
	{
		/* One line, and it names the reason: a disk that is there but
		*  holds nothing this kernel understands is the case one wants
		*  explained, unlike no disk at all. */
		printf("Filesystem: none (");
		print_capped(fat_last_error(), 46);
		printf(")\n");
		return;
	}

	printf("Filesystem: %s on hd%i, %i KiB of %i KiB free\n",
		fat_type(), drive,
		(int)(fat_free_bytes() / 1024u),
		(int)(fat_total_bytes() / 1024u));
}

/* ---------------------------------------------------------------------------
*  network_init() - PCI bus, network card, protocol stack
*
*  Three layers, and the order between them is not a matter of taste: pci_init()
*  enumerates the bus, rtl8139_init() looks up its card in what the enumeration
*  found, and net_init() has a MAC address to work with only once the card
*  answered. Nothing here probes hardware blindly; without the PCI scan the
*  kernel has no idea a network card exists at all.
*
*  Placed after disk_init() and after sti, and that placement is the point:
*  the card delivers received frames through an interrupt, so it is only from
*  here on that anything can arrive. Bringing it up earlier would work too -
*  irq_install_handler() just fills a table entry - but the frames would pile
*  up in the receive ring with nobody reading it, for no gain.
*
*  No card is the normal case: "make run NET=0", and every machine that has
*  none. rtl8139_init() then reports nothing found, net_init() marks the stack
*  down, and the shell comes up with ifconfig able to say why rather than
*  showing zeros. Nothing below may keep the boot from finishing.
*
*  The address is not configured here. QEMU's user network has no DHCP client
*  on our side, so it has to be set by hand from the shell:
*
*      ifconfig 10.0.2.15 255.255.255.0 10.0.2.2
*      ping 10.0.2.2
*
*  Hardcoding those would be wrong the moment the kernel meets a real network,
*  and a DHCP client is a protocol of its own - deliberately not part of this
*  step. */
static void network_init(void)
{
	pci_init();
	rtl8139_init();
	net_init();
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

	/* The two multiboot checks come first, ahead of the banner, and that is
	*  a consequence of the framebuffer console rather than a tidy-up.
	*
	*  Nothing may be printed before framebuffer_describe() has run, or it is
	*  lost on a graphics mode boot (see the comment there). The description
	*  can only be read out of mbi, and mbi is only a pointer worth
	*  dereferencing once these two checks have passed - so they have to
	*  happen before any output, not after the first three lines of it.
	*
	*  That leaves exactly one thing that still goes to text mode alone: the
	*  failure messages of these two checks. It is not a hole that can be
	*  closed. Without a usable multiboot info there is no geometry to
	*  describe, so there is no framebuffer console to say them on either,
	*  and on a machine that booted into a graphics mode the panic screen is
	*  invisible. The condition it reports - a bootloader that did not leave
	*  a multiboot info behind - is not one a graphics mode makes any more
	*  likely, and our own stage 2, which is what sets a VBE mode in the
	*  first place, is precisely the code that gets both of these right.
	*
	*  Each branch brings the text console up itself. init_video() cannot be
	*  hoisted above them for that: it clears the screen, and cls() asks
	*  fbcon_active() which screen it is clearing.
	*
	*  Without the magic we do not know whether ebx points at a multiboot
	*  info structure at all. Everything beyond this would be guesswork.
	*  Checked before the conversion: P2V() on garbage yields garbage. */
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC)
	{
		init_video();
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
		init_video();
		printf("Multiboot info at 0x%X, outside the direct mapping\n",
			(uint32_t)mbi_phys);
		panic("Multiboot info pointer out of range.\nTomatOS cannot reach the memory information.");
		return 0;
	}

	mbi = (multiboot_info *)P2V(mbi_phys);

	/* The earliest point the console can be told what it drives, and
	*  therefore the point it is told. Everything below this is recorded in
	*  the shadow buffer and survives to be painted; everything above it
	*  would not. */
	framebuffer_describe(mbi);

	/* Now the screen may be cleared - and on a graphics boot this is the
	*  clear that counts. There is no 80x25 buffer on such a machine, so
	*  cls() blanks the shadow buffer instead of 0xB8000, and the pixels
	*  follow when fbcon_activate() paints it. The leading blank lines of the
	*  banner are for the text case, where they separate our output from the
	*  bootloader's; a framebuffer console has 48 rows and does not miss the
	*  two. */
	init_video();
	printf("\n\nTomatOS/x86 boot v0.2\n");

	/* One line on the split, because it is exactly what one wants to see
	*  first when a higher half mapping goes wrong. */
	printf("Higher half: kernel 0x%X virt = 0x%X phys\n",
		KERNEL_VIRTUAL_START, V2P(KERNEL_VIRTUAL_START));

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
	*  The programs hang off the end of that same chain. exec_init() reads the
	*  module list out of the multiboot info, so it needs the converted mbi
	*  just like pmm_init() does, and it reads the module command lines
	*  through P2V() - which means the direct mapping vmm_init() built has to
	*  be live. It only writes down addresses, names and sizes; the ELF
	*  parsing, the frames and the address space all come later, in
	*  exec_spawn(), on demand from the shell. Hence its place: behind
	*  heap_init(), where memory management is complete and nothing moves
	*  around any more, and in front of mt_install(), because from the console
	*  task onwards someone may ask for the list at any time. It adds no
	*  kernel mapping of its own, so the rule above about page tables and
	*  task spaces does not concern it.
	*
	*  One thing exec_init() cannot do, and this is a real hole: protect the
	*  modules from the frame allocator. pmm_init() locks the multiboot info
	*  structure and the memory map, but neither the module list at mods_addr
	*  nor the module contents between mod_start and mod_end. Those frames sit
	*  inside a region the memory map reports as available, so pmm_init()
	*  releases them and pmm_alloc_frame() will hand them out - the first heap
	*  growth or the first task page directory can overwrite a program long
	*  before anybody tries to load it, and the failure then looks like a
	*  broken ELF header rather than what it is. The fix belongs in
	*  pmm_init(), next to the existing region_mark_used() calls, one per
	*  module plus the list itself; recording addresses here does not make
	*  the memory behind them any safer.
	*
	*  The disk hangs off the very end of the boot, behind the "sti", and it
	*  is the one step that is not placed by what it needs from memory
	*  management. Its lower bound is the same as everywhere here: ata_init()
	*  and fat_mount() may allocate - the filesystem takes a buffer for the
	*  FAT out of the heap - so heap_init() has to be done, and it is, long
	*  before. The upper bound is the one that decides it. The driver polls,
	*  so it needs no interrupt of its own and none of the IRQ machinery is a
	*  precondition. But a PIO driver that waits for a drive to become ready
	*  may well wait with sleep(), and sleep() is timer_wait(), which spins on
	*  timer_ticks until the timer IRQ moves it on. Before pic_install() the
	*  PIT is not programmed and no handler is installed; before the "sti" the
	*  interrupt cannot arrive at all, and timer_wait() then notices that
	*  interrupts are off and drops its hlt - but the loop itself has no way
	*  out. One sleep() in a driver placed further up would therefore not be
	*  slow, it would be the end of the boot, on precisely the machines whose
	*  drives are slow to answer. Behind the "sti" every mechanism a polling
	*  driver might reach for actually works, and the price is only that the
	*  two disk lines appear below "Protected Mode Kernel Running" instead of
	*  in the middle of the driver block.
	*
	*  Still in front of the console task, though, and that part is not
	*  cosmetic: the shell may ask fat_readdir() something with its first
	*  command, and by the time it can be typed the mount is either done or
	*  known to have failed. Nothing here needs a task or a scheduler - it
	*  runs on the boot stack like everything else in this function.
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
	*  those all happen well after isrs_install().
	*
	*  The framebuffer console is split across this chain in two places, and
	*  neither of them is negotiable.
	*
	*  framebuffer_describe() is already done, far above, before the first
	*  character - see the comment on it. What is left here is
	*  fbcon_activate(), which maps the framebuffer and paints the shadow
	*  buffer onto it, and it is bounded from both sides.
	*
	*  Its lower bound is vmm_init(). The address the bootloader reports is
	*  physical and belongs to memory mapped hardware, not to RAM: QEMU's
	*  card answers at 0xFD000000, which is above DIRECT_MAP_LIMIT, so P2V()
	*  has no name for it, and it appears in no memory map, so vmm_init()
	*  never mapped it. It has to be given a mapping of its own with
	*  vmm_map_mmio(), and there is nothing to map with before vmm_init() has
	*  built the tables. Hence the call one line behind it, which is the
	*  earliest instruction at which it can succeed at all.
	*
	*  Its upper bound is mt_install(), and that is the rule the block above
	*  states, hitting a real case for the first time. The kernel half is
	*  shared between address spaces at page DIRECTORY ENTRY granularity, so
	*  a kernel mapping that needs a brand new page table lands only in the
	*  directory that was active when it was made. The framebuffer window is
	*  exactly such a mapping - it is nowhere near the direct mapping and no
	*  existing kernel table covers it. Made after the first task space
	*  exists, it would be present in whichever directory happened to be
	*  loaded and absent from every other, and the console would page fault
	*  the moment the scheduler switched into a task with a space of its own.
	*  Doing it here means every directory vmm_create_space() makes later
	*  copies the entry along with the rest of the kernel half.
	*
	*  vmm.h now also reserves the page tables for that window inside
	*  vmm_init() for the same reason, which makes the upper bound belt and
	*  braces rather than the only thing standing between us and the fault.
	*  The call stays here regardless: relying on a reservation elsewhere to
	*  make a late mapping safe is how the rule gets forgotten the next time
	*  something wants a kernel range of its own.
	*
	*  Painting happens here as well, which is why the boot output is on the
	*  screen from this line onwards rather than from the shell. Everything
	*  above - the memory map, the higher half line, even the banner - was
	*  written into the shadow buffer and is drawn in one go.
	*
	*  framebuffer_init() then reports, and it reports afterwards on purpose:
	*  its last field says whether the console took the framebuffer over, and
	*  that is only knowable once fbcon_activate() has returned. Its own two
	*  checks are unchanged and still answer a different question - whether
	*  the direct mapping reaches the framebuffer, which is what fb_usable()
	*  is about - and vmm_is_mapped() still needs the tables vmm_init() built.
	*/
	print_memory_map(mbi);
	pmm_init(mbi);
	vmm_init();
	fbcon_activate();
	/* No extra line for the address space: the directory this prints IS the
	*  kernel space, so naming it here costs nothing, while a second message
	*  would cost one of the 25 rows the boot output has to fit into. The
	*  table count already includes whatever fbcon_activate() needed for the
	*  framebuffer window. */
	printf("Paging on: %i page tables, kernel space 0x%X\n",
		vmm_table_count(), vmm_kernel_space());
	framebuffer_init(mbi);
	heap_init();
	printf("%i MB Memory (%i KB) in %i Frames\n",
		(pmm_total_bytes() / (1024u * 1024u)),
		(pmm_total_bytes() / 1024u),
		pmm_total_frames());
	exec_init(mbi);
	print_modules();

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

	/* Disk and filesystem: last, because this is the first point at which a
	*  driver may wait for hardware with sleep(). See the ordering comment
	*  above. Says nothing when there is no drive. */
	disk_init();

	/* Network last: it is the only driver whose work starts by itself, from
	*  an interrupt, and there is nothing above it that waits on the result. */
	network_init();

	task_console = taskmgr_add_task( main, "CONSOLE", TASK_PRIORITY_REALTIME );
	taskmgr_task_start(task_console);

	/* From here on the timer IRQ takes over: the scheduler jumps into the
	 * console task. We return to start.asm, which waits there in an
	 * endless loop for the first task switch. */
	return 0;
}
