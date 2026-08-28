/* TomatOS - memory management
*  Desc: Physical frame allocator and kernel heap.
*
*  Two layers: pmm_* hands out physical 4 KiB frames based on the
*  bootloader's memory map, and the heap with malloc()/free() builds on top.
*/
#ifndef __MM_H
#define __MM_H

#include "typedefs.h"
#include "multiboot.h"

#define PMM_FRAME_SIZE  4096

/* --- Physical memory (pmm.c) -------------------------------------------- */

/* Builds the frame bitmap from the multiboot memory map. Frames occupied by
 * the kernel image itself or by the bitmap are marked as used. Must run
 * before any other pmm function. */
extern void pmm_init(multiboot_info *mbi);

/* Returns a free 4 KiB frame, or 0 if none is left.
 * The contents are not zeroed. */
extern void *pmm_alloc_frame(void);

/* Releases a frame previously obtained via pmm_alloc_frame().
 * A null pointer is allowed and does nothing. */
extern void pmm_free_frame(void *frame);

/* Contiguous block of count frames, or 0. For the heap. */
extern void *pmm_alloc_frames(uint32_t count);
extern void pmm_free_frames(void *frames, uint32_t count);

/* Statistics, in frames. Intended for the mem command and the status bar. */
extern uint32_t pmm_total_frames(void);
extern uint32_t pmm_used_frames(void);
extern uint32_t pmm_free_frames_count(void);

/* Total usable memory in bytes according to the memory map. */
extern uint32_t pmm_total_bytes(void);

/* --- Kernel heap (heap.c) ------------------------------------------------ */

/* Sets up the heap. Requires an initialised pmm. */
extern void heap_init(void);

extern void *malloc(size_t size);
extern void free(void *ptr);
extern void *calloc(size_t num, size_t size);
extern void *realloc(void *ptr, size_t size);

/* Statistics in bytes: used (payload including bookkeeping) and the total
 * memory the heap has requested from the pmm. */
extern uint32_t heap_used(void);
extern uint32_t heap_total(void);

#endif
