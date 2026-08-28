/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Physical memory manager (frame allocator)
*
*  Manages physical memory in pages of 4 KiB. The allocation state lives in
*  a bitmap: one bit per frame, 1 = used, 0 = free. The bitmap is placed
*  directly behind the kernel image, because at this point there is no
*  malloc() yet.
*
*  Two views of memory meet in this file, and mixing them up is fatal:
*    - everything the bitmap describes is PHYSICAL. Frame indices, the
*      addresses out of the multiboot memory map, and the return values of
*      pmm_alloc_frame() are physical addresses and stay that way.
*    - every dereference goes through a VIRTUAL address. The kernel is
*      linked for the higher half, so the linker symbols and the bitmap
*      pointer are virtual and differ from the physical view by
*      KERNEL_VIRTUAL_BASE.
*  V2P() / P2V() from vmm.h are the only sanctioned way across that line.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <mm.h>
#include <vmm.h>

/* Symbols provided by the linker script. Declared as arrays so that the
*  name itself already is the address - an "extern uint32_t kernel_end;"
*  would read the memory contents at that location instead of the address.
*  Both are VIRTUAL addresses in the higher half. */
extern char kernel_start[];
extern char kernel_end[];

/* Return value of frame_find_free() when nothing is free. Frame 0xFFFFF is
*  the last possible index, so 0xFFFFFFFF can never be a valid one. */
#define PMM_NO_FRAME    0xFFFFFFFFUL

/* Lowest address from which we hand out frames at all. Everything below
*  belongs to the BIOS, the EBDA and the text mode buffer at 0xB8000. */
#define PMM_LOW_LIMIT   0x00100000UL

/* Source of the memory information - both passes must use the same one */
#define PMM_SRC_MMAP    0   /* multiboot memory map */
#define PMM_SRC_MEM     1   /* only mem_lower / mem_upper */
#define PMM_SRC_GUESS   2   /* nothing usable reported at all */

/* --- State ---------------------------------------------------------------- */

static uint32_t *pmm_bitmap = 0;        /* base of the bitmap                 */
static uint32_t  pmm_bitmap_bytes = 0;  /* its size in bytes                  */
static uint32_t  pmm_frames = 0;        /* number of managed frames           */
static uint32_t  pmm_used = 0;          /* of those, in use                   */
static uint32_t  pmm_avail_bytes = 0;   /* usable memory according to the map */
static uint32_t  pmm_top_incl = 0;      /* highest usable byte address        */
static int       pmm_have_top = 0;      /* was anything found at all?         */
static uint32_t  pmm_hint = 0;          /* word index to start searching at   */
static int       pmm_capped = 0;        /* RAM dropped above the direct map   */

/* --- Bit level ------------------------------------------------------------ */

/* 1 = used. Everything outside the bitmap counts as used, so that no caller
*  accidentally takes memory beyond what is being managed. */
static int frame_test(uint32_t idx)
{
	if (idx >= pmm_frames) return 1;
	return (pmm_bitmap[idx >> 5] & ((uint32_t)1 << (idx & 31))) != 0;
}

/* Both markers are deliberately idempotent: they only count when the bit
*  really changes. That way neither a double allocation nor a double free
*  can throw the counters out of step. */
static void frame_mark_used(uint32_t idx)
{
	if (idx >= pmm_frames) return;
	if (pmm_bitmap[idx >> 5] & ((uint32_t)1 << (idx & 31))) return;
	pmm_bitmap[idx >> 5] |= (uint32_t)1 << (idx & 31);
	pmm_used++;
}

static void frame_mark_free(uint32_t idx)
{
	if (idx >= pmm_frames) return;
	if (!(pmm_bitmap[idx >> 5] & ((uint32_t)1 << (idx & 31)))) return;
	pmm_bitmap[idx >> 5] &= ~((uint32_t)1 << (idx & 31));
	pmm_used--;
}

/* --- Region level --------------------------------------------------------- */

/* Converts (base, len) into the last contained byte address. We compute with
*  this inclusive end address throughout, because base+len would overflow for
*  a region that reaches up to the 4 GiB boundary.
*  Returns 0 when the region is empty. */
static int region_bounds(uint32_t base, uint32_t len, uint32_t *end_incl)
{
	uint32_t e;

	if (len == 0) return 0;
	e = base + len - 1;
	if (e < base) e = 0xFFFFFFFFUL;	/* overflow: clamp at 4 GiB */
	*end_incl = e;
	return 1;
}

/* Allocating: round outward. Every frame the region even touches counts as
*  used - better to lock one frame too many than one that still holds
*  somebody else's data. */
static void region_mark_used(uint32_t base, uint32_t len)
{
	uint32_t end_incl;
	uint32_t first;
	uint32_t last;
	uint32_t i;

	if (pmm_frames == 0) return;
	if (!region_bounds(base, len, &end_incl)) return;

	first = base >> 12;
	last  = end_incl >> 12;
	if (first >= pmm_frames) return;
	if (last >= pmm_frames) last = pmm_frames - 1;

	for (i = first; i <= last; i++)
		frame_mark_used(i);
}

/* Releasing: round inward. Only frames that lie entirely inside the region
*  become free - a frame clipped at either edge stays locked. */
static void region_mark_free(uint32_t base, uint32_t len)
{
	uint32_t end_incl;
	uint32_t first;
	uint32_t last;
	uint32_t i;

	if (pmm_frames == 0) return;
	if (!region_bounds(base, len, &end_incl)) return;

	/* Does a whole frame fit in there at all? */
	if (end_incl < PMM_FRAME_SIZE - 1) return;
	if (base > 0xFFFFFFFFUL - (PMM_FRAME_SIZE - 1)) return;

	first = (base + (PMM_FRAME_SIZE - 1)) >> 12;	/* round start up */
	last  = (end_incl - (PMM_FRAME_SIZE - 1)) >> 12;	/* round end down */

	if (first > last) return;
	if (first >= pmm_frames) return;
	if (last >= pmm_frames) last = pmm_frames - 1;

	for (i = first; i <= last; i++)
		frame_mark_free(i);
}

/* First pass: remember the highest address and the total amount.
*  Physical memory the direct mapping cannot reach is dropped right here.
*  P2V() is only valid below DIRECT_MAP_LIMIT, so a frame above it could
*  never be touched by the kernel - it must not enter the bitmap either,
*  which is why the cap happens on the measuring pass and not later. */
static void region_note(uint32_t base, uint32_t len)
{
	uint32_t end_incl;
	uint32_t clen;

	if (!region_bounds(base, len, &end_incl)) return;

	if (base > DIRECT_MAP_LIMIT)
	{
		pmm_capped = 1;
		return;
	}
	if (end_incl > DIRECT_MAP_LIMIT)
	{
		end_incl = DIRECT_MAP_LIMIT;
		pmm_capped = 1;
	}

	if (!pmm_have_top || end_incl > pmm_top_incl)
	{
		pmm_top_incl = end_incl;
		pmm_have_top = 1;
	}

	clen = end_incl - base + 1;
	if (pmm_avail_bytes > 0xFFFFFFFFUL - clen) pmm_avail_bytes = 0xFFFFFFFFUL;
	else pmm_avail_bytes += clen;
}

/* --- Multiboot memory map ------------------------------------------------- */

/* pass == 0: only measure, pass != 0: release the available regions.
*  mbi->mmap_addr is a PHYSICAL address from the bootloader; walking the
*  entries means reading them, so the cursor runs through P2V(). The
*  addresses inside the entries stay physical - they describe frames. */
static void pmm_walk_mmap(multiboot_info *mbi, int pass)
{
	multiboot_mmap_entry *ent;
	uint32_t phys;
	uint32_t addr;
	uint32_t end;
	uint32_t next;

	phys = mbi->mmap_addr;

	/* The map has to lie inside the directly mapped window, otherwise
	*  there is no pointer with which to read it. */
	if (phys > DIRECT_MAP_LIMIT) return;
	if (mbi->mmap_length > DIRECT_MAP_LIMIT - phys + 1) return;

	addr = (uint32_t)P2V(phys);
	end  = addr + mbi->mmap_length;
	if (end < addr) return;		/* nonsensical length given */

	while (addr < end)
	{
		/* Is the remainder even enough for a complete entry? */
		if (end - addr < (uint32_t)sizeof(multiboot_mmap_entry)) break;

		ent = (multiboot_mmap_entry *)addr;

		/* size counts the bytes AFTER the size field; no valid entry can
		*  be smaller than 20. Bail out instead of spinning forever. */
		if (ent->size < 20) break;

		/* Only real RAM, and only what a 32-bit kernel can address:
		*  anything with the upper halves set lies beyond 4 GiB. */
		if (ent->type == MULTIBOOT_MEMORY_AVAILABLE &&
		    ent->addr_high == 0 && ent->len_high == 0)
		{
			if (pass == 0) region_note(ent->addr_low, ent->len_low);
			else           region_mark_free(ent->addr_low, ent->len_low);
		}

		/* The classic pitfall: do NOT advance by sizeof(). */
		next = addr + ent->size + 4;
		if (next <= addr) break;	/* overflow or standstill */
		addr = next;
	}
}

/* Fallback path for sparse bootloaders: mem_lower / mem_upper in KiB. */
static void pmm_walk_meminfo(multiboot_info *mbi, int pass)
{
	uint32_t lower;
	uint32_t upper;

	lower = mbi->mem_lower;
	upper = mbi->mem_upper;

	/* Below 1 MiB there is at most 640 KiB of usable RAM, and above it we
	*  cap such that 0x100000 + upper*1024 does not overflow. */
	if (lower > 640) lower = 640;
	if (upper > 0x003FF000UL) upper = 0x003FF000UL;

	if (pass == 0)
	{
		region_note(0, lower << 10);
		region_note(PMM_LOW_LIMIT, upper << 10);
	}
	else
	{
		region_mark_free(0, lower << 10);
		region_mark_free(PMM_LOW_LIMIT, upper << 10);
	}
}

/* --- Setup ---------------------------------------------------------------- */

void pmm_init(multiboot_info *mbi)
{
	uint32_t kstart;	/* physical start of the kernel image */
	uint32_t kend;		/* physical end of the kernel image   */
	uint32_t bmp_virt;	/* where the bitmap is written        */
	uint32_t bmp_phys;	/* which frames it occupies           */
	int src;

	pmm_bitmap       = 0;
	pmm_bitmap_bytes = 0;
	pmm_frames       = 0;
	pmm_used         = 0;
	pmm_avail_bytes  = 0;
	pmm_top_incl     = 0;
	pmm_have_top     = 0;
	pmm_hint         = 0;
	pmm_capped       = 0;

	/* The linker symbols are virtual. Everything from here on is compared
	*  against and written into the bitmap, and the bitmap speaks physical. */
	kstart = V2P((uint32_t)kernel_start);
	kend   = V2P((uint32_t)kernel_end);

	/* 1st pass: how do we know how much memory is there? */
	src = PMM_SRC_GUESS;

	if (mbi != 0 && (mbi->flags & MULTIBOOT_INFO_MEM_MAP) != 0 &&
	    mbi->mmap_length >= (uint32_t)sizeof(multiboot_mmap_entry))
	{
		pmm_walk_mmap(mbi, 0);
		if (pmm_have_top) src = PMM_SRC_MMAP;
	}

	if (src == PMM_SRC_GUESS && mbi != 0 &&
	    (mbi->flags & MULTIBOOT_INFO_MEMORY) != 0)
	{
		pmm_walk_meminfo(mbi, 0);
		if (pmm_have_top) src = PMM_SRC_MEM;
	}

	/* Last resort. The fallback path can deliver nonsense too - if there
	*  supposedly is nothing behind the kernel image, the figure is worthless. */
	if (!pmm_have_top || pmm_top_incl < kend)
	{
		printf("PMM: bootloader reports no usable memory, assuming 16 MiB\n");
		pmm_top_incl    = 0;
		pmm_have_top    = 0;
		pmm_avail_bytes = 0;
		src = PMM_SRC_GUESS;
		region_note(PMM_LOW_LIMIT, 0x00F00000UL);	/* 1 MiB .. 16 MiB */
	}

	/* Frames: pmm_top_incl is the last usable byte address, so the frame
	*  with that index still exists - hence the +1. */
	pmm_frames       = (pmm_top_incl >> 12) + 1;
	pmm_bitmap_bytes = ((pmm_frames + 31) >> 5) * 4;

	/* The bitmap goes directly behind the kernel image (4 byte aligned,
	*  because we scan it word by word). The rounding happens on the
	*  virtual address, because that is the one we will dereference; the
	*  frames it covers are named by the physical one.
	*
	*  Both checks live in the world they belong to: the alignment must not
	*  have wrapped around the end of the virtual address space, and the
	*  bitmap must fit below the top of physical RAM. pmm_top_incl is a
	*  physical byte address, so the comparison needs bmp_phys - with the
	*  virtual value it would be off by KERNEL_VIRTUAL_BASE and always fail.
	*  Since pmm_top_incl is capped at DIRECT_MAP_LIMIT, that same test also
	*  proves the whole bitmap is reachable through the direct mapping. */
	bmp_virt = ((uint32_t)kernel_end + 3) & ~(uint32_t)3;
	bmp_phys = V2P(bmp_virt);

	if (bmp_virt < (uint32_t)kernel_end || pmm_bitmap_bytes == 0 ||
	    bmp_phys < kend ||
	    bmp_phys > 0xFFFFFFFFUL - pmm_bitmap_bytes ||
	    bmp_phys + pmm_bitmap_bytes - 1 > pmm_top_incl)
	{
		pmm_frames = 0;
		panic("PMM: no room for the frame bitmap");
		return;
	}

	pmm_bitmap = (uint32_t *)bmp_virt;

	/* At first everything counts as used. The padding bits in the last word
	*  above pmm_frames thereby stay 1 permanently and can never be handed
	*  out - releasing only ever happens through indices < pmm_frames. */
	memset(pmm_bitmap, (char)0xFF, (size_t)pmm_bitmap_bytes);
	pmm_used = pmm_frames;

	/* Then release the regions that are actually available. */
	if (src == PMM_SRC_MMAP)     pmm_walk_mmap(mbi, 1);
	else if (src == PMM_SRC_MEM) pmm_walk_meminfo(mbi, 1);
	else                         region_mark_free(PMM_LOW_LIMIT, 0x00F00000UL);

	/* And finally lock everything again that must not be handed out. */
	frame_mark_used(0);				/* address 0 stays invalid  */
	region_mark_used(0, PMM_LOW_LIMIT);		/* BIOS, EBDA, VGA at 0xB8000 */
	region_mark_used(kstart, kend - kstart);	/* the kernel image itself  */
	region_mark_used(bmp_phys, pmm_bitmap_bytes);	/* the bitmap itself        */

	/* The bootloader puts the multiboot structures somewhere in RAM -
	*  usually below 1 MiB, but that is not guaranteed. Lock them, so they
	*  are not later overwritten out from under the kernel.
	*  mbi itself already arrives as a virtual pointer, so it goes back
	*  through V2P(); mmap_addr never left the physical world. */
	if (mbi != 0)
	{
		region_mark_used(V2P((uint32_t)mbi), (uint32_t)sizeof(multiboot_info));
		if (src == PMM_SRC_MMAP && mbi->mmap_length <= 0x10000UL)
			region_mark_used(mbi->mmap_addr, mbi->mmap_length);
	}

	if (pmm_capped)
		printf("PMM: RAM above %d MiB ignored, outside the direct mapping\n",
		       (int)((DIRECT_MAP_LIMIT >> 20) + 1));

	printf("PMM: %d KiB usable, %d frames of 4 KiB, %d free\n",
	       (int)(pmm_avail_bytes >> 10), (int)pmm_frames,
	       (int)(pmm_frames - pmm_used));
	printf("PMM: bitmap at 0x%X (phys 0x%X), %d bytes\n",
	       (int)bmp_virt, (int)bmp_phys, (int)pmm_bitmap_bytes);
}

/* --- Search --------------------------------------------------------------- */

/* Lowest zero bit in word w, or PMM_NO_FRAME. */
static uint32_t word_first_free(uint32_t w)
{
	uint32_t bits;
	uint32_t b;
	uint32_t idx;

	bits = pmm_bitmap[w];
	for (b = 0; b < 32; b++)
	{
		if ((bits & ((uint32_t)1 << b)) == 0)
		{
			idx = (w << 5) + b;
			if (idx < pmm_frames) return idx;
			return PMM_NO_FRAME;
		}
	}
	return PMM_NO_FRAME;
}

/* First free frame. Starts at the hint and wraps around once. */
static uint32_t frame_find_free(void)
{
	uint32_t words;
	uint32_t w;
	uint32_t idx;

	words = (pmm_frames + 31) >> 5;
	if (pmm_hint >= words) pmm_hint = 0;

	for (w = pmm_hint; w < words; w++)
	{
		if (pmm_bitmap[w] == 0xFFFFFFFFUL) continue;
		idx = word_first_free(w);
		if (idx != PMM_NO_FRAME) { pmm_hint = w; return idx; }
	}
	for (w = 0; w < pmm_hint; w++)
	{
		if (pmm_bitmap[w] == 0xFFFFFFFFUL) continue;
		idx = word_first_free(w);
		if (idx != PMM_NO_FRAME) { pmm_hint = w; return idx; }
	}
	return PMM_NO_FRAME;
}

/* --- Public interface ----------------------------------------------------- */

void *pmm_alloc_frame(void)
{
	uint32_t idx;

	if (pmm_frames == 0) return 0;

	idx = frame_find_free();
	if (idx == PMM_NO_FRAME) return 0;

	frame_mark_used(idx);
	return (void *)(idx << 12);
}

void pmm_free_frame(void *frame)
{
	uint32_t addr;
	uint32_t idx;

	if (frame == 0) return;			/* null pointer is allowed */
	if (pmm_frames == 0) return;

	addr = (uint32_t)frame;
	if ((addr & (PMM_FRAME_SIZE - 1)) != 0) return;	/* not aligned */

	idx = addr >> 12;
	if (idx >= pmm_frames) return;

	/* frame_mark_free() only counts down when the bit really was set - so a
	*  double free() has no effect. */
	if ((idx >> 5) < pmm_hint) pmm_hint = idx >> 5;
	frame_mark_free(idx);
}

void *pmm_alloc_frames(uint32_t count)
{
	uint32_t i;
	uint32_t j;
	uint32_t run;
	uint32_t start;

	if (count == 0) return 0;
	if (count == 1) return pmm_alloc_frame();
	if (pmm_frames == 0 || count > pmm_frames) return 0;

	run   = 0;
	start = 0;

	/* i never runs past pmm_frames, so the bitmap is only read within its
	*  own bounds. A run only counts as found once count frames have
	*  actually been checked. */
	for (i = 0; i < pmm_frames; i++)
	{
		/* Skip full words: 32 used frames in a row. */
		if ((i & 31) == 0 && pmm_bitmap[i >> 5] == 0xFFFFFFFFUL)
		{
			run = 0;
			i += 31;	/* the loop head adds the 32nd */
			continue;
		}

		if (frame_test(i))
		{
			run = 0;
			continue;
		}

		if (run == 0) start = i;
		run++;

		if (run == count)
		{
			for (j = 0; j < count; j++)
				frame_mark_used(start + j);
			return (void *)(start << 12);
		}
	}

	return 0;
}

void pmm_free_frames(void *frames, uint32_t count)
{
	uint32_t addr;
	uint32_t idx;
	uint32_t i;

	if (frames == 0 || count == 0) return;
	if (pmm_frames == 0) return;

	addr = (uint32_t)frames;
	if ((addr & (PMM_FRAME_SIZE - 1)) != 0) return;

	idx = addr >> 12;
	/* Phrased so that idx + count cannot overflow. */
	if (idx >= pmm_frames || count > pmm_frames - idx) return;

	if ((idx >> 5) < pmm_hint) pmm_hint = idx >> 5;

	for (i = 0; i < count; i++)
		frame_mark_free(idx + i);
}

/* --- Statistics ----------------------------------------------------------- */

uint32_t pmm_total_frames(void)
{
	return pmm_frames;
}

uint32_t pmm_used_frames(void)
{
	return pmm_used;
}

uint32_t pmm_free_frames_count(void)
{
	return pmm_frames - pmm_used;
}

/* Usable memory according to the memory map, that is the sum of the regions
*  reported as AVAILABLE - not the same as pmm_total_frames() * 4096, which
*  also counts the holes between the regions. */
uint32_t pmm_total_bytes(void)
{
	return pmm_avail_bytes;
}
