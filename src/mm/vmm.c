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
*    - the top 16 MiB, 0xFF000000 upwards, are not part of the direct mapping
*      at all. They are the window vmm_map_mmio() hands out for memory mapped
*      hardware that lies outside RAM, such as a card's linear framebuffer
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
*  On top of that single kernel directory the file provides per-task address
*  spaces: a private lower three quarters, a kernel half shared by entry copy
*  with every other space. The section "Address spaces" at the end of this file
*  explains the split and the rules that come with it.
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

/* Where the shared kernel half of a page directory begins. Every address from
*  KERNEL_VIRTUAL_BASE up is described by the entries at this index and above,
*  and those entries are identical in every address space - see the address
*  space section at the bottom of this file. */
#define KERNEL_DIR_FIRST   (KERNEL_VIRTUAL_BASE >> 22)              /* 768 */
#define KERNEL_DIR_ENTRIES (PAGE_ENTRIES - KERNEL_DIR_FIRST)        /* 256 */

/* --- The MMIO window ------------------------------------------------------
*
*  Memory mapped hardware is not RAM. A card's linear framebuffer sits wherever
*  the chipset decoded it - QEMU's stdvga puts it at 0xFD000000 - which is far
*  above the top of RAM and usually above the whole direct mapping. P2V() says
*  nothing about it and vmm_init() never maps it, because the pmm has never
*  heard of those frames. Such a range therefore needs a virtual address that
*  the vmm hands out itself.
*
*  Where to put it. Three requirements, and together they leave exactly one
*  sensible answer:
*
*    - above KERNEL_VIRTUAL_BASE. The framebuffer console prints from any
*      context, including from an interrupt taken while a ring 3 task is
*      current, so its window has to exist in EVERY address space. Only the
*      kernel half is shared, so only the kernel half will do.
*    - clear of the direct mapping, which claims KERNEL_VIRTUAL_BASE + p for
*      every physical frame p the pmm knows. Nominally that reaches all the way
*      to 0xFFFFFFFF, so the window can only be carved out of the top by
*      *shortening* the direct mapping - see DIRECT_MAP_TOP below.
*    - at a fixed, compile time known base, so that the page tables covering it
*      can be created once, up front. That is what makes the mapping visible in
*      every space; the reasoning is at mmio_reserve_tables().
*
*  So: the top 16 MiB of the address space, 0xFF000000..0xFFFFFFFF, four whole
*  directory entries (1020..1023). Ranges are handed out sequentially from the
*  base with a bump pointer and are never reclaimed - there is no
*  vmm_unmap_mmio(). Hardware windows are claimed once during bring-up and kept
*  for the lifetime of the system, so a free list would be bookkeeping for a
*  case that does not occur. 16 MiB is room for five framebuffers of the
*  1024x768x32 size (768 pages, 3 MiB) the console asks for. */
#define MMIO_WINDOW_BASE   0xFF000000u
#define MMIO_WINDOW_PAGES  4096u                                    /* 16 MiB */
#define MMIO_DIR_FIRST     (MMIO_WINDOW_BASE >> 22)                 /* 1020 */

/* Where the direct mapping now stops. Physical memory from here up has no
*  virtual alias any more: its slot at KERNEL_VIRTUAL_BASE + p belongs to the
*  MMIO window. That costs the last 16 MiB of a machine with more than 1008 MiB
*  of RAM, which is the cheapest currency available - the alternative is a
*  framebuffer no task but the current one can see. Everything that walks the
*  direct mapping (page tables, page directories, P2V() on a pmm frame) has to
*  respect this limit, which is why reachable_limit() reports it. */
#define DIRECT_MAP_TOP     (MMIO_WINDOW_BASE - KERNEL_VIRTUAL_BASE) /* 0x3F000000 */

/* --- State ---------------------------------------------------------------- */

static uint32_t *page_directory = 0;    /* VIRTUAL pointer to the directory   */
static uint32_t  directory_phys = 0;    /* PHYSICAL address, as CR3 wants it  */
static int       paging_active  = 0;    /* set once our own tables are live   */
static uint32_t  tables_in_use  = 0;    /* page tables allocated so far       */
static uint32_t  pages_mapped   = 0;    /* present entries across all tables  */
static uint32_t  mmio_next_page = 0;    /* bump pointer into the MMIO window  */

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

/* Physical address of the directory the processor is translating through right
*  now. That is what an address space *is* here, so this doubles as
*  vmm_current_space(). Paging is already on when this file starts running -
*  start.asm turned it on - so CR3 always holds something meaningful. */
static uint32_t read_cr3(void)
{
	uint32_t value;

	__asm__ __volatile__("movl %%cr3, %0" : "=r"(value));
	return value & PAGE_ADDR_MASK;
}

/* Kernel view of a directory given by its physical address. Works for any
*  space, the active one included, because every directory frame lies inside
*  the direct mapping - vmm_create_space() refuses a frame that does not. */
static uint32_t *dir_of(uint32_t space)
{
	return (uint32_t *)P2V(space & PAGE_ADDR_MASK);
}

/* The space vmm_map() and friends work on: whatever CR3 points at.
*
*  Before vmm_init() has switched CR3 that is still start.asm's boot directory,
*  and the directory we are busy filling in is our own - so answer with ours
*  during the bring-up. Afterwards the two coincide until the first task
*  switch, so nothing about the existing behaviour changes. */
static uint32_t active_space(void)
{
	if (!paging_active) return directory_phys;
	return read_cr3();
}

/* Highest physical address the kernel can currently reach through P2V().
*  Before vmm_init() switches CR3 that is whatever start.asm's boot directory
*  covers; afterwards it is the full direct mapping window. A page table has
*  to be written by the kernel, so a frame beyond this limit is unusable as
*  one, no matter how much free memory the pmm still reports. */
static uint32_t reachable_limit(void)
{
	if (paging_active) return DIRECT_MAP_TOP - 1;
	return BOOT_MAP_LIMIT - 1;
}

/* --- Mapping -------------------------------------------------------------- */

/* Does a TLB shootdown for one page make sense in this space?
*
*  invlpg drops the translation for a linear address out of the TLB of the
*  address space that is loaded RIGHT NOW. The x86 TLB carries no space
*  identifier (no PCID at this level), so entries cached for a foreign
*  directory simply do not exist while that directory is not in CR3: touching
*  another space's tables cannot leave a stale entry behind, and invalidating
*  for it would only throw away a translation of the *active* space that
*  happens to share the linear address. Pointless at best, harmful at worst.
*
*  What guarantees correctness for the foreign case is the CR3 load in
*  vmm_switch_space(): writing CR3 flushes every non-global entry, and this
*  kernel never sets PAGE_GLOBAL, so a space picks up all edits made while it
*  was parked the moment it becomes active again.
*
*  For the active space the flush is mandatory - the entry may well be cached. */
static int space_is_active(uint32_t space)
{
	return space == read_cr3();
}

/* Core of vmm_map()/vmm_map_in(). Works on the directory given by its physical
*  address, which may or may not be the one in CR3. */
static int map_page(uint32_t space, uint32_t virt, uint32_t phys, uint32_t flags)
{
	uint32_t *dir;
	uint32_t  di;
	uint32_t  ti;
	uint32_t *table;
	void     *frame;

	/* A directory the kernel cannot reach through P2V() cannot be edited,
	*  and a space of 0 is the "no such space" value of addrspace_t. */
	if (space == 0 || space > DIRECT_MAP_LIMIT) return -1;

	dir = dir_of(space);

	virt &= PAGE_ADDR_MASK;
	phys &= PAGE_ADDR_MASK;

	di = virt >> 22;			/* directory index: bits 31..22 */
	ti = (virt >> 12) & 0x3FF;		/* table index:     bits 21..12 */

	if (!(dir[di] & PAGE_PRESENT))
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
		dir[di] = (V2P(table) & PAGE_ADDR_MASK) |
		          PAGE_PRESENT | PAGE_WRITE |
		          (flags & PAGE_USER);
		tables_in_use++;
	}
	else
	{
		table = table_of(dir[di]);

		/* A user page below an entry that is not user accessible would
		*  stay unreachable, so widen the directory entry if needed. */
		if (flags & PAGE_USER)
			dir[di] |= PAGE_USER;
	}

	if (!(table[ti] & PAGE_PRESENT))
		pages_mapped++;

	/* phys is already physical - it is what the caller wants the MMU to
	*  resolve this page to, so no conversion here. */
	table[ti] = phys | (flags & PAGE_FLAG_MASK) | PAGE_PRESENT;

	if (space_is_active(space)) tlb_flush_page(virt);
	return 0;
}

/* Core of vmm_unmap()/vmm_unmap_in(). */
static int unmap_page(uint32_t space, uint32_t virt)
{
	uint32_t *dir;
	uint32_t  di;
	uint32_t  ti;
	uint32_t *table;

	if (space == 0 || space > DIRECT_MAP_LIMIT) return -1;

	dir = dir_of(space);

	virt &= PAGE_ADDR_MASK;
	di = virt >> 22;
	ti = (virt >> 12) & 0x3FF;

	if (!(dir[di] & PAGE_PRESENT)) return -1;

	table = table_of(dir[di]);
	if (!(table[ti] & PAGE_PRESENT)) return -1;

	table[ti] = 0;
	if (pages_mapped > 0) pages_mapped--;

	/* A table that has just lost its last entry is kept and stays hooked
	*  into the directory. Giving the frame back would mean counting the
	*  remaining entries on every unmap, and a table that is empty now is
	*  quite likely to be needed again in a moment. Refinement for later.
	*  vmm_destroy_space() collects them all in one go. */

	if (space_is_active(space)) tlb_flush_page(virt);
	return 0;
}

/* Walks the two levels for virt and hands back the table entry, or 0 if the
*  page is not present. A present entry always has bit 0 set and is therefore
*  never 0, so the sentinel is unambiguous.
*
*  pde_out, if given, receives the directory entry. The caller needs it because
*  rights live on both levels: the effective permission of a page is the AND of
*  the two entries, and the directory entry alone can revoke user access to a
*  whole 4 MiB region. */
static uint32_t walk_page(uint32_t space, uint32_t virt, uint32_t *pde_out)
{
	uint32_t *dir;
	uint32_t  di;
	uint32_t  ti;
	uint32_t *table;

	if (pde_out != 0) *pde_out = 0;
	if (space == 0 || space > DIRECT_MAP_LIMIT) return 0;

	dir = dir_of(space);

	di = virt >> 22;
	ti = (virt >> 12) & 0x3FF;

	if (!(dir[di] & PAGE_PRESENT)) return 0;
	if (pde_out != 0) *pde_out = dir[di];

	table = table_of(dir[di]);
	if (!(table[ti] & PAGE_PRESENT)) return 0;

	return table[ti];
}

/* Core of vmm_get_phys()/vmm_get_phys_in(). */
static uint32_t phys_of(uint32_t space, uint32_t virt)
{
	uint32_t pte;

	pte = walk_page(space, virt, 0);
	if (pte == 0) return 0;

	/* Frame address plus the offset inside the page - the caller asked
	*  about an address, not about a frame. The result is physical by
	*  definition, so it is the one place here that must not use P2V(). */
	return (pte & PAGE_ADDR_MASK) | (virt & PAGE_FLAG_MASK);
}

int vmm_map(uint32_t virt, uint32_t phys, uint32_t flags)
{
	if (page_directory == 0) return -1;
	return map_page(active_space(), virt, phys, flags);
}

int vmm_unmap(uint32_t virt)
{
	if (page_directory == 0) return -1;
	return unmap_page(active_space(), virt);
}

uint32_t vmm_get_phys(uint32_t virt)
{
	if (page_directory == 0) return 0;
	return phys_of(active_space(), virt);
}

int vmm_is_mapped(uint32_t virt)
{
	if (page_directory == 0) return 0;

	/* Asking walk_page() rather than vmm_get_phys(), because a page may
	*  legitimately resolve to physical frame 0 - it holds the real mode
	*  interrupt vector table and is mapped at P2V(0) like any other frame. */
	return walk_page(active_space(), virt, 0) != 0;
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

/* Creates the four page tables that cover the MMIO window and hangs them into
*  the kernel directory, empty.
*
*  This is the whole answer to the trap vmm.h documents. Sharing between address
*  spaces happens at DIRECTORY ENTRY granularity: vmm_create_space() copies
*  entries 768..1023 out of the kernel directory, so a kernel page mapped inside
*  a table that already exists appears in every space at once, while a kernel
*  mapping that has to allocate a NEW table writes that table's address into one
*  directory only - the active one. Every other space would keep an absent entry
*  there and fault on the framebuffer, which for a console printing from a ring 3
*  task's interrupt is a fault inside the fault handler.
*
*  So the tables are created here, once, at a fixed base, before the first
*  address space exists. From then on every vmm_create_space() copies the four
*  entries along with the rest of the kernel half, and vmm_map_mmio() only ever
*  writes PAGE TABLE entries into tables that all spaces already point at. That
*  removes the timing constraint entirely: an MMIO mapping may be established at
*  any moment, from any address space, and is immediately visible in all of them.
*  Costs four frames, 16 KiB, whether or not any hardware is ever mapped.
*
*  The tables must be created here in vmm_init() and nowhere else - that is the
*  one ordering rule. Doing it lazily on the first vmm_map_mmio() would be
*  correct only as long as no address space had been created yet, which is
*  precisely the sort of invisible precondition this arrangement is meant to
*  avoid. Returns 0 on success, -1 if the pmm cannot supply a usable frame. */
static int mmio_reserve_tables(void)
{
	uint32_t  di;
	uint32_t *table;
	void     *frame;

	for (di = MMIO_DIR_FIRST; di < PAGE_ENTRIES; di++)
	{
		if (page_directory[di] & PAGE_PRESENT) continue;

		frame = pmm_alloc_frame();
		if (frame == 0) return -1;

		/* Same requirement as any other page table: the kernel writes it,
		*  so it has to have an alias in the direct mapping. */
		if ((uint32_t)frame > reachable_limit())
		{
			pmm_free_frame(frame);
			return -1;
		}

		table = (uint32_t *)P2V(frame);
		memset(table, (char)0, (size_t)PAGE_SIZE);

		/* Empty and present. An empty table maps nothing - every entry has
		*  bit 0 clear - so the window stays unmapped until somebody asks
		*  for a range, and a stray access into it still faults.
		*
		*  No PAGE_USER: hardware windows are kernel only, and the entry is
		*  copied verbatim into every task's directory. */
		page_directory[di] = (V2P(table) & PAGE_ADDR_MASK) |
		                     PAGE_PRESENT | PAGE_WRITE;
		tables_in_use++;
	}

	return 0;
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
	*  stays unmapped rather than wrapping around into the low half.
	*
	*  It stops a little earlier still, at DIRECT_MAP_TOP, because the top
	*  16 MiB of linear space are reserved for the MMIO window - see there.
	*  On a machine with 1008 MiB of RAM or less this changes nothing at
	*  all; above that, the last frames lose their alias instead of the
	*  framebuffer losing its address. */
	frame_limit = DIRECT_MAP_TOP >> 12;
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

	/* After the switch, so the four tables may come from anywhere in the
	*  direct mapping rather than from the low 4 MiB the boot directory
	*  covers, and still before any task address space can exist. */
	if (mmio_reserve_tables() != 0)
	{
		panic("VMM: no frame for the MMIO window page tables");
		return;
	}

	printf("VMM: paging on, %d pages in %d tables, page 0 left unmapped\n",
	       (int)pages_mapped, (int)tables_in_use);
}

/* --- Memory mapped I/O ----------------------------------------------------- */

/* Maps a physical hardware range into the kernel's MMIO window and returns a
*  pointer to it, or 0 on failure.
*
*  Declared in vmm.h as:
*      extern void *vmm_map_mmio(uint32_t phys, uint32_t size);
*
*  phys need not be page aligned. The mapping is rounded down to the page that
*  contains it and extended to cover phys + size - 1, and the offset inside the
*  first page is added back to the result, so the caller gets a pointer to the
*  very byte it asked about and never has to think about the rounding.
*
*  The frames are deliberately NOT marked used in the pmm. They are not RAM; the
*  pmm's bitmap has no bit for them, has never handed them out and must never be
*  told about them - a pmm_free_frame() on a framebuffer frame would clear a bit
*  belonging to some completely unrelated frame and hand that one out twice.
*  Nothing here allocates physical memory at all; the only frames involved are
*  the page tables, and those were taken during vmm_init().
*
*  Cache attributes: PAGE_NOCACHE together with PAGE_WRITETHROUGH, i.e. the
*  strongest uncached type the processor offers without PAT or MTRR setup. A
*  framebuffer left write-back is the classic "the picture updates late, or
*  only when something else happens to evict the line" bug: the writes sit in
*  the cache, the card has not seen them, and there is no coherency mechanism
*  that would tell it. PCD is the bit that actually forbids caching; PWT is set
*  as well so that the entry names the fully uncached type rather than the
*  weaker UC- in the default PAT layout, which is the deterministic choice while
*  nothing in this kernel programs MTRRs. Should write combining ever be worth
*  the trouble - and for a framebuffer it is, byte by byte writes to uncached
*  memory are slow - the change is to drop PWT here and let a WC MTRR or a PAT
*  entry take effect over UC-.
*
*  PAGE_WRITE, no PAGE_USER: the window is kernel only, and its four directory
*  entries are copied into every task's directory. */
void *vmm_map_mmio(uint32_t phys, uint32_t size)
{
	uint32_t offset;
	uint32_t base;
	uint32_t pages;
	uint32_t first;
	uint32_t flags;
	uint32_t i;
	uint32_t virt;

	if (page_directory == 0 || size == 0) return 0;

	/* The range must stay inside the 4 GiB physical address space; a
	*  request that wraps around is a caller bug, not a mapping. */
	if (phys > 0xFFFFFFFFu - (size - 1)) return 0;

	/* Nothing larger than the window can ever be satisfied, and rejecting
	*  it here also keeps the page count below from overflowing. */
	if (size > (MMIO_WINDOW_PAGES << 12)) return 0;

	offset = phys & PAGE_FLAG_MASK;
	base   = phys & PAGE_ADDR_MASK;

	/* Pages spanned by [phys, phys+size), rounded up. offset is below 4096
	*  and size is at most 16 MiB, so the sum stays well inside 32 bits. */
	pages = (offset + size + (PAGE_SIZE - 1)) >> 12;

	/* Room left in the window. Written as a subtraction rather than as
	*  mmio_next_page + pages, which could wrap. */
	if (pages > MMIO_WINDOW_PAGES - mmio_next_page) return 0;

	first = mmio_next_page;
	flags = PAGE_PRESENT | PAGE_WRITE | PAGE_NOCACHE | PAGE_WRITETHROUGH;

	for (i = 0; i < pages; i++)
	{
		virt = MMIO_WINDOW_BASE + ((first + i) << 12);

		/* The active space is as good as any other here: the table
		*  behind this address is the same physical table in every
		*  directory, so the entry lands in all of them at once, and
		*  going through the active one gets the invlpg for free. The
		*  call cannot need a new table - mmio_reserve_tables() made
		*  them all - so the pmm is not touched. */
		if (map_page(active_space(), virt, base + (i << 12), flags) != 0)
		{
			/* Should not happen. Undo the partial mapping and leave
			*  the bump pointer where it was, so the window does not
			*  leak on a failed request. */
			while (i > 0)
			{
				i--;
				unmap_page(active_space(),
				           MMIO_WINDOW_BASE + ((first + i) << 12));
			}
			return 0;
		}
	}

	mmio_next_page += pages;

	return (void *)(MMIO_WINDOW_BASE + (first << 12) + offset);
}

/* --- Address spaces -------------------------------------------------------
*
*  A page directory has 1024 entries, each describing 4 MiB. The split this
*  kernel uses is:
*
*      entries    0 .. 767    private  ->  0x00000000 .. 0xBFFFFFFF, user
*      entries  768 .. 1023   SHARED   ->  0xC0000000 .. 0xFFFFFFFF, kernel
*
*  "Shared" means the entries are copied, not the tables. Every space points
*  its upper 256 entries at the very same page tables the kernel directory
*  uses, so kernel code, kernel data, the direct mapping of all RAM and each
*  task's kernel stack keep their translation no matter which directory sits in
*  CR3. That is precisely what makes it safe to take an interrupt, or to enter
*  a system call, from inside any address space: the CPU switches privilege but
*  not the mapping it needs to survive.
*
*  The flip side is the rule that governs vmm_destroy_space(): those 256
*  entries do not belong to the space that holds them. Handing one of their
*  tables back to the pmm would pull the kernel's own mapping out from under
*  every other space at once.
*
*  The second consequence is that a space only ever learns about kernel
*  directory entries that existed when it was created. A kernel mapping placed
*  into an existing table is seen everywhere immediately; one that needs a new
*  table is seen only by the directory that was in CR3 at the time. All of RAM
*  is mapped by vmm_init(), and so are the four empty tables of the MMIO window
*  - see mmio_reserve_tables() - so every kernel table this system will ever use
*  exists before the first vmm_create_space(). Any future kernel range that
*  needs its own table has to be reserved the same way, in vmm_init(); adding
*  one later is the bug this note exists to prevent.
*/

addrspace_t vmm_kernel_space(void)
{
	return directory_phys;
}

addrspace_t vmm_current_space(void)
{
	return read_cr3();
}

addrspace_t vmm_create_space(void)
{
	void     *frame;
	uint32_t *dir;

	if (page_directory == 0) return 0;

	frame = pmm_alloc_frame();
	if (frame == 0) return 0;

	/* The directory has to be written by the kernel, so it must live inside
	*  the direct mapping - the same requirement a page table has. */
	if ((uint32_t)frame > reachable_limit())
	{
		pmm_free_frame(frame);
		return 0;
	}

	dir = (uint32_t *)P2V(frame);

	/* The private half starts out empty. The pmm hands out frames without
	*  clearing them, and a leftover bit 0 in the user half would be a live
	*  mapping to a random frame the instant this directory reached CR3 -
	*  reachable from ring 3 if the stale entry also had PAGE_USER set. So
	*  the lower 768 entries are zeroed before anything else touches them. */
	memset(dir, (char)0, (size_t)(KERNEL_DIR_FIRST * sizeof(uint32_t)));

	/* The kernel half is taken over verbatim from the kernel directory.
	*  Copying the ENTRIES is the whole trick: each one still carries the
	*  physical address of a kernel page table, so both directories walk
	*  into the same table and see the same pages. No copy is made and none
	*  is wanted - a later kernel mapping inside an existing table becomes
	*  visible in every space at once.
	*
	*  Between the memset and this memcpy every one of the 1024 entries has
	*  been written, so no byte of the frame keeps its previous content. */
	memcpy(&dir[KERNEL_DIR_FIRST], &page_directory[KERNEL_DIR_FIRST],
	       (size_t)(KERNEL_DIR_ENTRIES * sizeof(uint32_t)));

	/* The space is named by the physical address of its directory, which is
	*  exactly the value CR3 takes. pmm_alloc_frame() already returns one. */
	return (addrspace_t)frame;
}

void vmm_destroy_space(addrspace_t space)
{
	uint32_t *dir;
	uint32_t  di;
	uint32_t  ti;
	uint32_t *table;

	if (space == 0) return;

	if (space > DIRECT_MAP_LIMIT)
	{
		printf("VMM: cannot destroy space 0x%X, outside the direct mapping\n",
		       (uint32_t)space);
		return;
	}

	/* The kernel directory owns the shared half. Freeing it would free the
	*  tables every other space borrows. */
	if (space == directory_phys)
	{
		printf("VMM: refusing to destroy the kernel address space\n");
		return;
	}

	/* And the space we are standing in: the moment its tables go back to
	*  the pmm they can be handed out and overwritten, while CR3 still walks
	*  them. Switch to another space first, then destroy this one. */
	if (space == read_cr3())
	{
		printf("VMM: refusing to destroy the active address space 0x%X\n",
		       (uint32_t)space);
		return;
	}

	dir = dir_of(space);

	/* Strictly di < KERNEL_DIR_FIRST: the loop stops one entry short of the
	*  shared half and never looks at 768..1023. Those tables belong to the
	*  kernel and are still in use by every other space. */
	for (di = 0; di < KERNEL_DIR_FIRST; di++)
	{
		if (!(dir[di] & PAGE_PRESENT)) continue;

		table = table_of(dir[di]);

		/* Every frame this table maps is a frame the task owns - the
		*  user half is never used for shared or memory mapped pages in
		*  this kernel, so giving them all back is right. */
		for (ti = 0; ti < PAGE_ENTRIES; ti++)
		{
			if (!(table[ti] & PAGE_PRESENT)) continue;

			pmm_free_frame((void *)(table[ti] & PAGE_ADDR_MASK));
			table[ti] = 0;
			if (pages_mapped > 0) pages_mapped--;
		}

		/* Then the table itself. Clearing the directory entry first
		*  keeps the directory consistent for the rest of the loop. */
		pmm_free_frame((void *)(dir[di] & PAGE_ADDR_MASK));
		dir[di] = 0;
		if (tables_in_use > 0) tables_in_use--;
	}

	/* Finally the directory frame. No TLB work is needed: this space is not
	*  in CR3, so the processor holds no translation of it, and the next
	*  CR3 load flushes what it does hold anyway. */
	pmm_free_frame((void *)space);
}

void vmm_switch_space(addrspace_t space)
{
	if (space == 0 || space > DIRECT_MAP_LIMIT) return;

	space &= PAGE_ADDR_MASK;

	/* Reloading the same directory would flush the whole TLB for nothing. */
	if (space == read_cr3()) return;

	/* Safe from inside an interrupt handler, and that is not luck: the
	*  handler's code, its stack and the IDT all live above
	*  KERNEL_VIRTUAL_BASE, i.e. in the half every directory shares, so the
	*  instruction after this one is fetched from the same physical byte and
	*  ESP still points at the same memory. Only the private half changes.
	*
	*  Writing CR3 also flushes all non-global entries, which is what makes
	*  edits done to a parked space through vmm_map_in() take effect here. */
	__asm__ __volatile__("movl %0, %%cr3" : : "r"(space) : "memory");
}

int vmm_map_in(addrspace_t space, uint32_t virt, uint32_t phys, uint32_t flags)
{
	if (page_directory == 0) return -1;
	return map_page(space, virt, phys, flags);
}

int vmm_unmap_in(addrspace_t space, uint32_t virt)
{
	if (page_directory == 0) return -1;
	return unmap_page(space, virt);
}

uint32_t vmm_get_phys_in(addrspace_t space, uint32_t virt)
{
	if (page_directory == 0) return 0;
	return phys_of(space, virt);
}

int vmm_is_user_mapped(uint32_t virt)
{
	uint32_t pde;
	uint32_t pte;

	if (page_directory == 0) return 0;

	pde = 0;
	pte = walk_page(active_space(), virt, &pde);
	if (pte == 0) return 0;

	/* Both levels have to grant user access. The processor computes the
	*  effective rights of a page as the AND of the directory entry and the
	*  table entry, so a page whose table entry says PAGE_USER but whose
	*  directory entry does not is unreachable from ring 3 - the fault comes
	*  before the table is ever consulted. Checking only the table entry
	*  would let a system call happily dereference a pointer the caller
	*  cannot touch itself, which is the exact hole this function exists to
	*  close: the question is not "is something mapped here" but "may the
	*  caller see it".
	*
	*  This also makes an explicit KERNEL_VIRTUAL_BASE test unnecessary. The
	*  kernel half is mapped without PAGE_USER, so a user pointer into it
	*  fails right here. */
	return (pde & PAGE_USER) != 0 && (pte & PAGE_USER) != 0;
}
