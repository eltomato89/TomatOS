/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Virtual memory manager. Two-level x86 paging with 4 KiB pages.
*
*  The whole of RAM is mapped one to one: virtual address equals physical
*  address. That is the cheapest possible arrangement, and it is what makes
*  the switch into paged mode safe - every pointer that was valid a moment
*  before the "mov cr0" is still valid a moment after it, including the
*  instruction pointer, the stack pointer and the page tables themselves.
*
*  Two things are deliberately not one to one:
*    - page zero is never mapped, so a null pointer dereference raises a
*      page fault instead of quietly reading the real mode interrupt vector
*      table that still sits at address 0
*    - the kernel's own code is mapped present but not writable
*
*  Because the kernel runs in ring 0, a read-only mapping only bites when
*  CR0.WP is set as well - without that bit the processor lets supervisor
*  code write to read-only pages. vmm_init() therefore sets WP together
*  with PG.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <mm.h>
#include <vmm.h>

/* Symbols from the linker script. Declared as arrays so that the name
*  itself already is the address - the same trick pmm.c uses. */
extern char text_start[];
extern char text_end[];

/* Address part of a directory or table entry. The low 12 bits are flags. */
#define PAGE_ADDR_MASK  0xFFFFF000UL
#define PAGE_FLAG_MASK  0x00000FFFUL

/* Bits in CR0 */
#define CR0_WP  0x00010000UL    /* write protect: honour read-only in ring 0 */
#define CR0_PG  0x80000000UL    /* paging on                                 */

/* --- State ---------------------------------------------------------------- */

static uint32_t *page_directory = 0;    /* 1024 entries, one 4 KiB frame      */
static uint32_t  directory_phys = 0;    /* same value, as it goes into CR3    */
static int       paging_active  = 0;    /* set once CR0.PG is on              */
static uint32_t  tables_in_use  = 0;    /* page tables allocated so far       */
static uint32_t  pages_mapped   = 0;    /* present entries across all tables  */

/* --- Small helpers -------------------------------------------------------- */

/* Drops a single address out of the TLB. Cheaper than reloading CR3, and it
*  does not throw away the rest of the translations. */
static void tlb_flush_page(uint32_t addr)
{
	__asm__ __volatile__("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Page table behind a directory entry. Identity mapping means the physical
*  address in the entry is directly usable as a pointer. */
static uint32_t *table_of(uint32_t pde)
{
	return (uint32_t *)(pde & PAGE_ADDR_MASK);
}

/* --- Mapping -------------------------------------------------------------- */

int vmm_map(uint32_t virt, uint32_t phys, uint32_t flags)
{
	uint32_t  di;
	uint32_t  ti;
	uint32_t *table;
	void     *frame;

	if (page_directory == 0) return -1;

	virt &= PAGE_ADDR_MASK;
	phys &= PAGE_ADDR_MASK;

	di = virt >> 22;			/* directory index: bits 31..22 */
	ti = (virt >> 12) & 0x3FF;		/* table index:     bits 21..12 */

	if (!(page_directory[di] & PAGE_PRESENT))
	{
		frame = pmm_alloc_frame();
		if (frame == 0) return -1;

		table = (uint32_t *)frame;

		/* Zero the fresh table *before* it is hung into the directory.
		*  The pmm hands out frames without clearing them, so until this
		*  memset has run every one of the 1024 entries is whatever the
		*  previous owner left behind - and a stale entry with bit 0 set
		*  would be a live mapping to a random frame the moment the
		*  processor could reach it. Since the directory entry is still
		*  absent at this point, the MMU cannot walk into the table yet,
		*  and no TLB entry can exist for it either. The write itself is
		*  safe both before paging is on (physical access) and after it
		*  (the frame lies inside the identity mapped range, see
		*  vmm_init). */
		memset(table, (char)0, (size_t)PAGE_SIZE);

		/* The directory entry stays permissive: the effective rights of
		*  a page are the AND of directory and table entry, so a
		*  read-only page needs its table entry read-only, not this one. */
		page_directory[di] = ((uint32_t)table & PAGE_ADDR_MASK) |
		                     PAGE_PRESENT | PAGE_WRITE |
		                     (flags & PAGE_USER);
		tables_in_use++;
	}
	else
	{
		table = table_of(page_directory[di]);

		/* A user page below an entry that is not user accessible would
		*  stay unreachable, so widen the directory entry if needed. */
		if (flags & PAGE_USER)
			page_directory[di] |= PAGE_USER;
	}

	if (!(table[ti] & PAGE_PRESENT))
		pages_mapped++;

	table[ti] = phys | (flags & PAGE_FLAG_MASK) | PAGE_PRESENT;

	tlb_flush_page(virt);
	return 0;
}

int vmm_unmap(uint32_t virt)
{
	uint32_t  di;
	uint32_t  ti;
	uint32_t *table;

	if (page_directory == 0) return -1;

	virt &= PAGE_ADDR_MASK;
	di = virt >> 22;
	ti = (virt >> 12) & 0x3FF;

	if (!(page_directory[di] & PAGE_PRESENT)) return -1;

	table = table_of(page_directory[di]);
	if (!(table[ti] & PAGE_PRESENT)) return -1;

	table[ti] = 0;
	if (pages_mapped > 0) pages_mapped--;

	/* A table that has just lost its last entry is kept and stays hooked
	*  into the directory. Giving the frame back would mean counting the
	*  remaining entries on every unmap, and a table that is empty now is
	*  quite likely to be needed again in a moment. Refinement for later. */

	tlb_flush_page(virt);
	return 0;
}

uint32_t vmm_get_phys(uint32_t virt)
{
	uint32_t  di;
	uint32_t  ti;
	uint32_t *table;

	if (page_directory == 0) return 0;

	di = virt >> 22;
	ti = (virt >> 12) & 0x3FF;

	if (!(page_directory[di] & PAGE_PRESENT)) return 0;

	table = table_of(page_directory[di]);
	if (!(table[ti] & PAGE_PRESENT)) return 0;

	/* Frame address plus the offset inside the page - the caller asked
	*  about an address, not about a frame. */
	return (table[ti] & PAGE_ADDR_MASK) | (virt & PAGE_FLAG_MASK);
}

int vmm_is_mapped(uint32_t virt)
{
	uint32_t  di;
	uint32_t  ti;
	uint32_t *table;

	if (page_directory == 0) return 0;

	di = virt >> 22;
	ti = (virt >> 12) & 0x3FF;

	if (!(page_directory[di] & PAGE_PRESENT)) return 0;

	table = table_of(page_directory[di]);
	return (table[ti] & PAGE_PRESENT) != 0;
}

/* --- Statistics and registers --------------------------------------------- */

int vmm_enabled(void)
{
	return paging_active;
}

uint32_t vmm_table_count(void)
{
	return tables_in_use;
}

uint32_t vmm_mapped_pages(void)
{
	return pages_mapped;
}

uint32_t vmm_directory_phys(void)
{
	return directory_phys;
}

/* CR2 holds the linear address that caused the last page fault. Only
*  meaningful inside the fault handler, before the next fault overwrites it. */
uint32_t vmm_read_cr2(void)
{
	uint32_t value;

	__asm__ __volatile__("movl %%cr2, %0" : "=r"(value));
	return value;
}

/* --- Bringing paging up --------------------------------------------------- */

/* Loads CR3 and flips PG (and WP) in CR0. Interrupts are off around the
*  switch: not because an interrupt would be fatal - the IDT, the handlers
*  and the stack are all identity mapped - but because there is no reason to
*  take one between the two register writes. */
static void paging_switch_on(uint32_t dir_phys)
{
	uint32_t eflags;
	uint32_t cr0;

	__asm__ __volatile__("pushfl ; popl %0" : "=r"(eflags) : : "memory");
	__asm__ __volatile__("cli");

	__asm__ __volatile__("movl %0, %%cr3" : : "r"(dir_phys) : "memory");
	__asm__ __volatile__("movl %%cr0, %0" : "=r"(cr0));
	cr0 |= CR0_WP | CR0_PG;
	__asm__ __volatile__("movl %0, %%cr0" : : "r"(cr0) : "memory");

	__asm__ __volatile__("pushl %0 ; popfl" : : "r"(eflags) : "memory", "cc");
}

void vmm_init(void)
{
	uint32_t frames;
	uint32_t ro_first;
	uint32_t ro_last;
	uint32_t addr;
	uint32_t flags;
	uint32_t i;
	void    *dir;

	if (paging_active) return;

	/* A frame from the pmm is 4 KiB and naturally page aligned, which is
	*  exactly what CR3 wants. */
	dir = pmm_alloc_frame();
	if (dir == 0)
	{
		panic("VMM: no frame for the page directory");
		return;
	}

	page_directory = (uint32_t *)dir;
	directory_phys = (uint32_t)dir;
	memset(page_directory, (char)0, (size_t)PAGE_SIZE);

	/* The read-only window: the kernel's code section, rounded *inward* to
	*  whole pages. Rounding outward could catch a page that also holds
	*  data of a neighbouring section, and write protecting that would kill
	*  the kernel the first time it touched a variable. The linker script
	*  aligns .text and the following section to 4 KiB, so in practice
	*  nothing is lost here. */
	ro_first = ((uint32_t)text_start + (PAGE_SIZE - 1)) & PAGE_ADDR_MASK;
	ro_last  = (uint32_t)text_end & PAGE_ADDR_MASK;
	if (ro_last <= ro_first)
	{
		/* Degenerate range - protect nothing rather than guess. */
		ro_first = 0;
		ro_last  = 0;
	}

	/* Map every frame the pmm knows about, starting at frame 1. Note that
	*  this walks the whole address range up to the top of RAM and not only
	*  the frames marked free: the kernel image, the frame bitmap, the
	*  multiboot structures and the text mode buffer at 0xB8000 all have to
	*  stay reachable. Frame 0 is skipped on purpose - see the file header.
	*  The tables needed along the way come from pmm_alloc_frame() inside
	*  vmm_map(), and every frame the pmm can return has an index below
	*  pmm_total_frames(), so each of them is covered by this very loop. */
	frames = pmm_total_frames();

	for (i = 1; i < frames; i++)
	{
		addr  = i << 12;
		flags = PAGE_PRESENT | PAGE_WRITE;

		if (ro_last != 0 && addr >= ro_first && addr < ro_last)
			flags = PAGE_PRESENT;		/* kernel code: no write */

		if (vmm_map(addr, addr, flags) != 0)
		{
			panic("VMM: out of memory while building the page tables");
			return;
		}
	}

	/* Everything the processor will touch across the switch is mapped now:
	*  the code (.text), the stack and all variables (.data/.bss, both
	*  inside the kernel image), the page directory and every page table
	*  (frames from the pmm, all below the top of RAM), the IDT and the
	*  screen buffer. So the instruction right behind the "mov cr0" is
	*  fetched from the same linear address as before, and it resolves to
	*  the same physical byte. */
	paging_switch_on(directory_phys);
	paging_active = 1;

	printf("VMM: paging on, %d pages in %d tables, page 0 left unmapped\n",
	       (int)pages_mapped, (int)tables_in_use);
}
