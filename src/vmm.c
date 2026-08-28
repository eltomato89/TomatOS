/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Virtual memory manager. Two-level x86 paging with 4 KiB pages.
*
*  The kernel runs in the higher half. It is linked for KERNEL_VIRTUAL_START
*  (0xC0100000) but loaded at KERNEL_PHYSICAL_BASE (0x00100000), and this file
*  builds the tables that make that true: every physical frame p of usable RAM
*  is mapped to the virtual address p + KERNEL_VIRTUAL_BASE. One fixed offset
*  for the whole of RAM, which is exactly what V2P() and P2V() in vmm.h encode.
*
*  The one rule that governs this file: a page directory or table entry holds
*  a PHYSICAL address, because the MMU walks the tables itself and knows
*  nothing about our mapping. The kernel, on the other hand, may only touch
*  those tables through a VIRTUAL pointer. Every place where the two views
*  meet goes through V2P() or P2V(), never through a bare cast - a bare cast
*  is the bug that hides while both views still coincide and surfaces much
*  later as a fault at an address nobody recognises.
*
*  Two ranges are deliberately special:
*    - the whole low 3 GiB stay unmapped, so virtual page zero is unmapped
*      too and a null pointer dereference raises a page fault. Note that
*      physical frame 0 is a different thing entirely: it holds the real mode
*      interrupt vector table, is mapped like any other frame, and lives at
*      P2V(0) = 0xC0000000
*    - the kernel's own code (text_start .. text_end, virtual symbols now) is
*      mapped present but not writable
*
*  Because the kernel runs in ring 0, a read-only mapping only bites when
*  CR0.WP is set as well - without that bit the processor lets supervisor
*  code write to read-only pages. vmm_init() therefore sets WP when it
*  installs the real page directory.
*
*  start.asm has already enabled paging with a small boot directory that maps
*  the low 4 MiB twice: once identity and once at KERNEL_VIRTUAL_BASE. That is
*  just enough to get a higher-half kernel running. vmm_init() replaces it and
*  does NOT reproduce the identity half - see the comment there.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <mm.h>
#include <vmm.h>

/* Symbols from the linker script. Declared as arrays so that the name
*  itself already is the address - the same trick pmm.c uses. With the
*  higher-half link these are virtual addresses, i.e. 0xC01xxxxx. */
extern char text_start[];
extern char text_end[];

/* Address part of a directory or table entry. The low 12 bits are flags. */
#define PAGE_ADDR_MASK  0xFFFFF000UL
#define PAGE_FLAG_MASK  0x00000FFFUL

/* Bits in CR0 */
#define CR0_WP  0x00010000UL    /* write protect: honour read-only in ring 0 */
#define CR0_PG  0x80000000UL    /* paging on                                 */

/* How far the boot directory from start.asm reaches. It maps the first 4 MiB
*  of physical memory, identity and at KERNEL_VIRTUAL_BASE. Until vmm_init()
*  has loaded CR3 with our own directory, P2V() of anything at or above this
*  limit points into nothing. */
#define BOOT_MAP_LIMIT  0x00400000UL

/* --- State ---------------------------------------------------------------- */

static uint32_t *page_directory = 0;    /* VIRTUAL pointer to the directory   */
static uint32_t  directory_phys = 0;    /* PHYSICAL address, as CR3 wants it  */
static int       paging_active  = 0;    /* set once our own tables are live   */
static uint32_t  tables_in_use  = 0;    /* page tables allocated so far       */
static uint32_t  pages_mapped   = 0;    /* present entries across all tables  */

/* --- Small helpers -------------------------------------------------------- */

/* Drops a single address out of the TLB. Cheaper than reloading CR3, and it
*  does not throw away the rest of the translations. */
static void tlb_flush_page(uint32_t addr)
{
	__asm__ __volatile__("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Page table behind a directory entry. The entry carries the table's physical
*  address for the MMU; the kernel has to go through the direct mapping to
*  reach the same bytes, hence P2V(). */
static uint32_t *table_of(uint32_t pde)
{
	return (uint32_t *)P2V(pde & PAGE_ADDR_MASK);
}

/* Highest physical address the kernel can currently reach through P2V().
*  Before vmm_init() switches CR3 that is whatever start.asm's boot directory
*  covers; afterwards it is the full direct mapping window. A page table has
*  to be written by the kernel, so a frame beyond this limit is unusable as
*  one, no matter how much free memory the pmm still reports. */
static uint32_t reachable_limit(void)
{
	if (paging_active) return DIRECT_MAP_LIMIT;
	return BOOT_MAP_LIMIT - 1;
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

		/* pmm_alloc_frame() returns a PHYSICAL address. The kernel is
		*  about to write 4 KiB into that frame, so it needs the frame's
		*  virtual alias - and that alias only exists if the frame lies
		*  inside the window the current tables cover. Hand the frame
		*  back rather than fault on it. */
		if ((uint32_t)frame > reachable_limit())
		{
			pmm_free_frame(frame);
			return -1;
		}

		table = (uint32_t *)P2V(frame);

		/* Zero the fresh table *before* it is hung into the directory.
		*  The pmm hands out frames without clearing them, so until this
		*  memset has run every one of the 1024 entries is whatever the
		*  previous owner left behind - and a stale entry with bit 0 set
		*  would be a live mapping to a random frame the moment the
		*  processor could reach it. Since the directory entry is still
		*  absent at this point, the MMU cannot walk into the table yet,
		*  and no TLB entry can exist for it either. */
		memset(table, (char)0, (size_t)PAGE_SIZE);

		/* And back the other way: the directory entry is read by the
		*  MMU, so it gets the PHYSICAL address. V2P(table) is the frame
		*  we started from; writing it out this way rather than reusing
		*  "frame" keeps the direction of the conversion visible.
		*
		*  The entry stays permissive: the effective rights of a page
		*  are the AND of directory and table entry, so a read-only page
		*  needs its table entry read-only, not this one. */
		page_directory[di] = (V2P(table) & PAGE_ADDR_MASK) |
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

	/* phys is already physical - it is what the caller wants the MMU to
	*  resolve this page to, so no conversion here. */
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
	*  about an address, not about a frame. The result is physical by
	*  definition, so it is the one place here that must not use P2V(). */
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

/* Installs our own page directory and makes sure WP is on.
*
*  dir_phys is a PHYSICAL address, because that is the only thing CR3 accepts.
*
*  Paging is already running when this is called - start.asm turned it on with
*  its boot directory. So the "mov cr3" is not a switch from unpaged to paged
*  but an exchange of one translation for another, and it takes effect on the
*  very next instruction fetch. Three things have to survive that instant:
*
*    - EIP. We are executing kernel code somewhere in .text, which the linker
*      placed at 0xC01xxxxx and which the new tables map to physical
*      0x001xxxxx, the same bytes the boot directory's higher-half half was
*      resolving to. The next instruction is therefore fetched from the same
*      linear address and lands on the same physical byte.
*    - ESP. The stack lives in .bss, inside the kernel image, so the same
*      argument applies to it, to the return address on it and to the saved
*      registers.
*    - the tables themselves. The new directory and every table below it are
*      pmm frames inside RAM, and vmm_init() mapped all of RAM before getting
*      here, so page_directory and every table_of() pointer stay valid.
*
*  What does NOT survive is the boot directory's identity half. That is
*  deliberate; anything still holding a low physical pointer faults here.
*  Loading CR3 also flushes the TLB of all non-global entries, and we never
*  set PAGE_GLOBAL, so no stale identity translation lingers.
*
*  Interrupts are off across the two register writes. Not because an interrupt
*  would be fatal - the IDT, the handlers and the stack are all mapped in both
*  directories - but there is no reason to take one in the middle. */
static void paging_switch_on(uint32_t dir_phys)
{
	uint32_t eflags;
	uint32_t cr0;

	__asm__ __volatile__("pushfl ; popl %0" : "=r"(eflags) : : "memory");
	__asm__ __volatile__("cli");

	__asm__ __volatile__("movl %0, %%cr3" : : "r"(dir_phys) : "memory");

	/* PG is already set by start.asm; setting it again costs nothing and
	*  keeps this correct even if the bootstrap ever changes. WP is the bit
	*  that actually matters here: without it the read-only mapping of the
	*  kernel's code would be ignored in ring 0. */
	__asm__ __volatile__("movl %%cr0, %0" : "=r"(cr0));
	cr0 |= CR0_WP | CR0_PG;
	__asm__ __volatile__("movl %0, %%cr0" : : "r"(cr0) : "memory");

	__asm__ __volatile__("pushl %0 ; popfl" : : "r"(eflags) : "memory", "cc");
}

void vmm_init(void)
{
	uint32_t frames;
	uint32_t frame_limit;
	uint32_t ro_first;
	uint32_t ro_last;
	uint32_t phys;
	uint32_t virt;
	uint32_t flags;
	uint32_t i;
	void    *dir;

	if (paging_active) return;

	/* A frame from the pmm is 4 KiB and naturally page aligned, which is
	*  exactly what CR3 wants. It is a physical address, so it is the CR3
	*  value directly - and the kernel's own view of it needs P2V(). */
	dir = pmm_alloc_frame();
	if (dir == 0)
	{
		panic("VMM: no frame for the page directory");
		return;
	}
	if ((uint32_t)dir >= BOOT_MAP_LIMIT)
	{
		/* Same reasoning as in vmm_map(): until CR3 points at this very
		*  directory, the only way to write it is through the boot
		*  mapping from start.asm. */
		panic("VMM: page directory outside the boot mapping");
		return;
	}

	directory_phys = (uint32_t)dir;
	page_directory = (uint32_t *)P2V(dir);
	memset(page_directory, (char)0, (size_t)PAGE_SIZE);

	/* The read-only window: the kernel's code section, rounded *inward* to
	*  whole pages. Rounding outward could catch a page that also holds
	*  data of a neighbouring section, and write protecting that would kill
	*  the kernel the first time it touched a variable. The linker script
	*  aligns .text and the following section to 4 KiB, so in practice
	*  nothing is lost here.
	*
	*  text_start and text_end are virtual now, and the loop below compares
	*  them against the virtual address it is about to map - no conversion
	*  needed, and none wanted: the two would only agree by accident. */
	ro_first = ((uint32_t)text_start + (PAGE_SIZE - 1)) & PAGE_ADDR_MASK;
	ro_last  = (uint32_t)text_end & PAGE_ADDR_MASK;
	if (ro_last <= ro_first)
	{
		/* Degenerate range - protect nothing rather than guess. */
		ro_first = 0;
		ro_last  = 0;
	}

	/* Map every frame the pmm knows about to KERNEL_VIRTUAL_BASE + frame.
	*  This walks the whole address range up to the top of RAM and not only
	*  the frames marked free: the kernel image, the frame bitmap, the
	*  multiboot structures and the sub-1 MiB MMIO including the text mode
	*  buffer at 0xB8000 are all marked used in the bitmap and all have to
	*  stay reachable through P2V().
	*
	*  Physical frame 0 is mapped like every other one; it is virtual page
	*  zero that stays out, and that comes for free because nothing below
	*  KERNEL_VIRTUAL_BASE is mapped at all.
	*
	*  The tables needed along the way come from pmm_alloc_frame() inside
	*  vmm_map(), and every frame the pmm can return has an index below
	*  pmm_total_frames(), so each of them is covered by this very loop. */
	frames = pmm_total_frames();

	/* The direct mapping ends where the address space does: physical
	*  memory above DIRECT_MAP_LIMIT has no room left below 4 GiB. Such a
	*  machine needs a real high-memory scheme; until then the surplus
	*  stays unmapped rather than wrapping around into the low half. */
	frame_limit = (DIRECT_MAP_LIMIT + 1) >> 12;
	if (frames > frame_limit) frames = frame_limit;

	for (i = 0; i < frames; i++)
	{
		phys  = i << 12;
		virt  = KERNEL_VIRTUAL_BASE + phys;
		flags = PAGE_PRESENT | PAGE_WRITE;

		if (ro_last != 0 && virt >= ro_first && virt < ro_last)
			flags = PAGE_PRESENT;		/* kernel code: no write */

		if (vmm_map(virt, phys, flags) != 0)
		{
			panic("VMM: no usable frame for a page table");
			return;
		}
	}

	/* The boot directory's identity mapping of the low 4 MiB is not
	*  reproduced here, on purpose. It exists only to bridge the moment
	*  between enabling paging and jumping into the higher half; keeping it
	*  alive afterwards would let every leftover physical pointer keep
	*  working by accident, which is precisely the class of bug the V2P /
	*  P2V discipline is meant to expose. Dropping it turns each of those
	*  into a page fault at the earliest possible moment, with the offending
	*  address in CR2.
	*
	*  The price is that anything still relying on it dies right here. The
	*  known case is the multiboot info structure, which the bootloader left
	*  in low physical memory; kernel.c reaches it through P2V(), which the
	*  loop above has just mapped. */
	paging_switch_on(directory_phys);
	paging_active = 1;

	printf("VMM: paging on, %d pages in %d tables, page 0 left unmapped\n",
	       (int)pages_mapped, (int)tables_in_use);
}
