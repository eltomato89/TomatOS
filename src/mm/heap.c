/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Kernel heap. malloc()/free() on top of the physical frame
*        allocator from pmm.c.
*
*  Design: a single, address-sorted doubly linked list over *all* blocks
*  (used as well as free), each with a header in front of it.
*  The address ordering is the trick: merging two free neighbours is then
*  just a look at prev/next, and the question "do those two really touch?"
*  is answered by an address comparison -- necessary, because the heap
*  consists of several, not necessarily contiguous frame blocks from the
*  pmm.
*
*  Address domain: the pmm hands out PHYSICAL addresses, the kernel runs in
*  the higher half. Everything in this file -- headers, the address ordering
*  of the list, the adjacency test, the pointers handed to the caller -- is
*  therefore expressed in VIRTUAL addresses. The single point of conversion
*  is heap_grow(), which puts a fresh pmm block through P2V() before anyone
*  else ever sees it. Mixing the two views anywhere else would break the
*  adjacency test and merge regions that do not touch.
*
*  A buddy or slab allocator would be overkill for a kernel of this size;
*  the free list is entirely sufficient.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <mm.h>
#include <vmm.h>

/* Alignment of every returned pointer. 8 bytes, so that later structures
*  with 64-bit fields (uint64, double) sit properly and the processor does
*  not have to split accesses across word boundaries. */
#define HEAP_ALIGN          8

/* Signature in the block header. It lets free() recognise pointers that do
*  not come from the heap at all, instead of silently tearing down the
*  bookkeeping -- in ring 0 without memory protection the only protection
*  we have. */
#define HEAP_MAGIC          0xA110CA7EUL

/* Initial size and minimum top-up, each in frames (4 KiB).
*  16 frames = 64 KiB. Better to grow in larger steps than to pester the
*  pmm on every malloc(). */
#define HEAP_INITIAL_FRAMES 16
#define HEAP_GROW_FRAMES    16

/* Upper limit for a single request. Prevents the rounding up to whole
*  frames further below from overflowing. */
#define HEAP_MAX_ALLOC      0x3FFFFFFFUL

struct heap_block
{
    uint32_t magic;             /* HEAP_MAGIC, otherwise not a valid block */
    uint32_t size;              /* payload in bytes, always a multiple of 8 */
    uint32_t used;              /* 0 = free, 1 = in use */
    uint32_t fill;              /* keeps the header at 24 bytes (see below) */
    struct heap_block *prev;    /* predecessor in address order */
    struct heap_block *next;    /* successor in address order */
};

/* 24 bytes, a multiple of HEAP_ALIGN. Together with the fact that every
*  payload size is rounded up to 8 and that the pmm frames lie on 4 KiB
*  boundaries, every block start -- and hence every payload -- is
*  8-aligned. */
#define HEAP_HEADER_SIZE    ((uint32_t)sizeof(struct heap_block))

/* Smallest payload a block is still allowed to carry. */
#define HEAP_MIN_PAYLOAD    HEAP_ALIGN

/* Compile-time assertion: were the header not a multiple of HEAP_ALIGN,
*  every second payload would come out skewed. Aborts the compilation with
*  "negative array size" should that ever be violated. */
typedef char heap_header_alignment_check
    [(sizeof(struct heap_block) % HEAP_ALIGN) == 0 ? 1 : -1];

/* Round up to the next alignment boundary. */
#define HEAP_ALIGN_UP(x)    (((x) + (uint32_t)(HEAP_ALIGN - 1)) & ~(uint32_t)(HEAP_ALIGN - 1))

static struct heap_block *heap_first = 0;   /* head of the address list */
static uint32_t heap_bytes = 0;             /* total taken from the pmm */
static int heap_ready = 0;

/* --- Internal helpers ---------------------------------------------------- */

/* Does b lie immediately behind the payload of a? Only then may the two be
*  merged -- being neighbours in the list alone is not enough, because two
*  separate pmm requests can leave a gap between them.
*
*  Both addresses are virtual, and that is exactly what makes the test still
*  work after the move into the higher half: P2V() adds one and the same
*  constant to every physical address, so two blocks that touch physically
*  (p2 == p1 + n) also touch virtually (p2 + K == p1 + K + n) and two blocks
*  with a gap keep the very same gap. The comparison is therefore just as
*  valid in the virtual view -- as long as no physical address ever sneaks
*  into the list. */
static int heap_adjacent(struct heap_block *a, struct heap_block *b)
{
    if(a == 0 || b == 0)
        return 0;
    return ((unsigned char *)a + HEAP_HEADER_SIZE + a->size) == (unsigned char *)b;
}

/* Swallows the successor. The caller guarantees: both free and adjacent.
*  The successor's header vanishes into the payload and is therefore
*  invalidated -- a pointer to it still lying around then stands out. */
static void heap_merge_next(struct heap_block *blk)
{
    struct heap_block *n;

    n = blk->next;
    blk->size += HEAP_HEADER_SIZE + n->size;
    blk->next = n->next;
    if(n->next != 0)
        n->next->prev = blk;
    n->magic = 0;
}

/* Merges a freshly released block with both neighbours. Forward first, then
*  backward: that way at most a single free block remains, no matter in
*  which order things were released. */
static void heap_coalesce(struct heap_block *blk)
{
    if(blk->next != 0 && blk->next->used == 0 && heap_adjacent(blk, blk->next))
        heap_merge_next(blk);

    if(blk->prev != 0 && blk->prev->used == 0 && heap_adjacent(blk->prev, blk))
        heap_merge_next(blk->prev);
}

/* Splits blk so that exactly need bytes of payload sit at the front. The
*  remainder becomes a free block of its own -- but only if it can still
*  carry a header *and* a usable payload. Otherwise the excess stays with
*  the block, which is cheaper than an unusable splinter. */
static void heap_split(struct heap_block *blk, uint32_t need)
{
    struct heap_block *rest;

    if(blk->size < need + HEAP_HEADER_SIZE + HEAP_MIN_PAYLOAD)
        return;

    rest = (struct heap_block *)((unsigned char *)blk + HEAP_HEADER_SIZE + need);
    rest->magic = HEAP_MAGIC;
    rest->size  = blk->size - need - HEAP_HEADER_SIZE;
    rest->used  = 0;
    rest->fill  = 0;
    rest->prev  = blk;
    rest->next  = blk->next;

    if(rest->next != 0)
        rest->next->prev = rest;
    blk->next = rest;
    blk->size = need;
}

/* Links a region freshly obtained from the pmm into the address list as a
*  free block and immediately tries to merge it with its neighbours.
*  base is already a VIRTUAL address (see heap_grow()).
*  The pmm may give us frames below as well as above the existing heap,
*  which is why it is inserted at the right place and not merely appended. */
static void heap_add_region(void *base, uint32_t bytes)
{
    struct heap_block *blk;
    struct heap_block *p;

    if(base == 0 || bytes < HEAP_HEADER_SIZE + HEAP_MIN_PAYLOAD)
        return;

    blk = (struct heap_block *)base;
    blk->magic = HEAP_MAGIC;
    blk->size  = bytes - HEAP_HEADER_SIZE;
    blk->used  = 0;
    blk->fill  = 0;
    blk->prev  = 0;
    blk->next  = 0;

    heap_bytes += bytes;

    if(heap_first == 0)
    {
        heap_first = blk;
        return;
    }

    if((uint32_t)blk < (uint32_t)heap_first)
    {
        blk->next = heap_first;
        heap_first->prev = blk;
        heap_first = blk;
    }
    else
    {
        p = heap_first;
        while(p->next != 0 && (uint32_t)p->next < (uint32_t)blk)
            p = p->next;

        blk->prev = p;
        blk->next = p->next;
        if(p->next != 0)
            p->next->prev = blk;
        p->next = blk;
    }

    heap_coalesce(blk);
}

/* Asks the pmm for as many frames as it takes for need bytes of payload to
*  fit in -- but at least HEAP_GROW_FRAMES. Returns 1 on success.
*
*  This is the only place in the file where the physical view appears: the
*  block from the pmm is translated with P2V() once and enters the list as a
*  virtual address. */
static int heap_grow(uint32_t need)
{
    uint32_t bytes;
    uint32_t frames;
    uint32_t minimum;
    uint32_t phys;
    uint32_t span;

    if(need > HEAP_MAX_ALLOC)
        return 0;

    bytes = need + HEAP_HEADER_SIZE;
    minimum = (bytes + (uint32_t)(PMM_FRAME_SIZE - 1)) / (uint32_t)PMM_FRAME_SIZE;

    frames = minimum;
    if(frames < (uint32_t)HEAP_GROW_FRAMES)
        frames = (uint32_t)HEAP_GROW_FRAMES;

    phys = (uint32_t)pmm_alloc_frames(frames);
    if(phys == 0 && frames != minimum)
    {
        /* There was no contiguous block left for the generous wish --
        *  second attempt with what is really needed. */
        frames = minimum;
        phys = (uint32_t)pmm_alloc_frames(frames);
    }
    if(phys == 0)
        return 0;

    span = frames * (uint32_t)PMM_FRAME_SIZE;

    /* P2V() is only defined inside the directly mapped window. Frames above
    *  it have no fixed virtual address and are of no use to the heap, so
    *  they go straight back -- and pmm_free_frames() wants the physical
    *  address again, which is precisely the one we still hold here.
    *  Phrased so that the subtraction cannot wrap: span is at most
    *  DIRECT_MAP_LIMIT + 1 bytes, because need is capped at HEAP_MAX_ALLOC. */
    if(phys > (uint32_t)DIRECT_MAP_LIMIT - (span - 1))
    {
        pmm_free_frames((void *)phys, frames);
        return 0;
    }

    heap_add_region(P2V(phys), span);
    return 1;
}

/* First matching free block (first fit). With the few dozen blocks of a
*  hobby kernel, best fit is not worth the effort. */
static struct heap_block *heap_find(uint32_t need)
{
    struct heap_block *blk;

    blk = heap_first;
    while(blk != 0)
    {
        if(blk->used == 0 && blk->size >= need)
            return blk;
        blk = blk->next;
    }
    return 0;
}

/* From the payload pointer back to the block header, with a sanity check.
*  Returns 0 and reports the error if the pointer is not from the heap. */
static struct heap_block *heap_block_of(void *ptr)
{
    struct heap_block *blk;

    /* Cheap pre-check before dereferencing: every heap payload is 8-aligned
    *  and sits behind a header, so it is never right down at the bottom. */
    if((((uint32_t)ptr) & (uint32_t)(HEAP_ALIGN - 1)) != 0 ||
       ((uint32_t)ptr) < HEAP_HEADER_SIZE)
    {
        printf("heap: pointer %X does not come from the heap (alignment)\n",
               (uint32_t)ptr);
        return 0;
    }

    blk = (struct heap_block *)((unsigned char *)ptr - HEAP_HEADER_SIZE);
    if(blk->magic != HEAP_MAGIC)
    {
        printf("heap: invalid pointer %X (magic %X)\n",
               (uint32_t)ptr, blk->magic);
        return 0;
    }

    return blk;
}

/* Rounds a user request up to the internal block size.
*  malloc(0) thereby ends up at HEAP_MIN_PAYLOAD (see malloc()). */
static uint32_t heap_round(size_t size)
{
    uint32_t need;

    need = HEAP_ALIGN_UP((uint32_t)size);
    if(need == 0)
        need = HEAP_MIN_PAYLOAD;
    return need;
}

/* --- Public interface ---------------------------------------------------- */

void heap_init(void)
{
    if(heap_ready != 0)
        return;

    heap_ready = 1;
    heap_first = 0;
    heap_bytes = 0;

    if(heap_grow((uint32_t)HEAP_INITIAL_FRAMES * (uint32_t)PMM_FRAME_SIZE
                 - HEAP_HEADER_SIZE) == 0)
        printf("heap: no memory from the pmm for the initial heap\n");
}

/* malloc(0) returns a valid, unique pointer to a block with
*  HEAP_MIN_PAYLOAD bytes of payload -- that is, *not* a null pointer. This
*  way no caller has to treat 0 as a special case, and the return value may
*  go to free() or realloc() perfectly normally. Dereferencing it is of
*  course not allowed. (C permits both readings; this is ours.) */
void *malloc(size_t size)
{
    struct heap_block *blk;
    uint32_t need;

    /* In this project size_t is "int", that is, signed. A negative size is
    *  a programming error and is rejected, rather than being read as just
    *  under 4 GiB. */
    if(size < 0)
    {
        printf("heap: malloc with negative size (%d)\n", size);
        return 0;
    }

    /* In case somebody uses the heap before heap_init(): quietly catch up. */
    if(heap_ready == 0)
        heap_init();

    need = heap_round(size);
    if(need > HEAP_MAX_ALLOC)
        return 0;

    blk = heap_find(need);
    if(blk == 0)
    {
        /* Nothing suitable there -- ask the pmm for more and retry once.
        *  After the merging an already existing block may be the right one
        *  as well, hence the full search. */
        if(heap_grow(need) == 0)
            return 0;
        blk = heap_find(need);
        if(blk == 0)
            return 0;
    }

    heap_split(blk, need);
    blk->used = 1;

    return (void *)((unsigned char *)blk + HEAP_HEADER_SIZE);
}

void free(void *ptr)
{
    struct heap_block *blk;

    /* free(0) is explicitly without effect. */
    if(ptr == 0)
        return;

    blk = heap_block_of(ptr);
    if(blk == 0)
        return;                 /* error has already been reported */

    if(blk->used == 0)
    {
        printf("heap: double free() on %X\n", (uint32_t)ptr);
        return;
    }

    blk->used = 0;
    heap_coalesce(blk);
}

void *calloc(size_t num, size_t size)
{
    uint32_t bytes;
    void *ptr;

    if(num < 0 || size < 0)
    {
        printf("heap: calloc with negative size (%d * %d)\n", num, size);
        return 0;
    }

    if(num == 0 || size == 0)
        return malloc(0);

    /* Overflow protection by division instead of 64-bit multiplication: if
    *  num no longer fits into the permitted range divided by size, then the
    *  product would wrap around. */
    if((uint32_t)num > HEAP_MAX_ALLOC / (uint32_t)size)
    {
        printf("heap: calloc overflow (%d * %d)\n", num, size);
        return 0;
    }

    bytes = (uint32_t)num * (uint32_t)size;

    ptr = malloc((size_t)bytes);
    if(ptr != 0)
        memset(ptr, 0, (size_t)bytes);

    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    struct heap_block *blk;
    void *newptr;
    uint32_t need;
    uint32_t copy;

    if(size < 0)
    {
        printf("heap: realloc with negative size (%d)\n", size);
        return 0;
    }

    /* realloc(0, n) is malloc(n) ... */
    if(ptr == 0)
        return malloc(size);

    /* ... and realloc(p, 0) is free(p). */
    if(size == 0)
    {
        free(ptr);
        return 0;
    }

    blk = heap_block_of(ptr);
    if(blk == 0)
        return 0;

    if(blk->used == 0)
    {
        printf("heap: realloc on already freed block %X\n",
               (uint32_t)ptr);
        return 0;
    }

    need = heap_round(size);
    if(need > HEAP_MAX_ALLOC)
        return 0;

    /* Already fits: at most cut off the excess, the pointer stays. */
    if(blk->size >= need)
    {
        heap_split(blk, need);
        if(blk->next != 0 && blk->next->used == 0)
            heap_coalesce(blk->next);
        return ptr;
    }

    /* If the directly following free neighbour grows into it, we save
    *  ourselves the copying and moving entirely. */
    if(blk->next != 0 && blk->next->used == 0 && heap_adjacent(blk, blk->next) &&
       blk->size + HEAP_HEADER_SIZE + blk->next->size >= need)
    {
        heap_merge_next(blk);
        heap_split(blk, need);
        return ptr;
    }

    newptr = malloc(size);
    if(newptr == 0)
        return 0;               /* the old block stays untouched and valid */

    /* Copy only as much as the old block really had -- here it is always
    *  smaller than the new one, but the guard does no harm. */
    copy = blk->size;
    if(copy > need)
        copy = need;

    memcpy(newptr, ptr, (size_t)copy);
    free(ptr);

    return newptr;
}

/* In use: the payload of the used blocks plus their management headers.
*  Determined by walking the list instead of being counted along -- with the
*  block numbers here that is negligible, and in return it cannot drift. */
uint32_t heap_used(void)
{
    struct heap_block *blk;
    uint32_t sum;

    sum = 0;
    blk = heap_first;
    while(blk != 0)
    {
        if(blk->used != 0)
            sum += HEAP_HEADER_SIZE + blk->size;
        blk = blk->next;
    }
    return sum;
}

/* Memory requested from the pmm in total, in bytes. */
uint32_t heap_total(void)
{
    return heap_bytes;
}
