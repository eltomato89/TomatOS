/* TomatOS - Virtual memory management
*  Desc: Two-level x86 paging with 4 KiB pages.
*
*  The kernel is identity mapped: virtual address equals physical address.
*  That keeps every pointer in the system valid across the moment paging is
*  switched on, at the price of not yet separating kernel and user space.
*
*  What this buys us right now:
*    - page zero stays unmapped, so a null pointer dereference faults
*      instead of quietly reading the interrupt vector table
*    - the kernel's own code is mapped read-only
*    - a fault reports the offending address (CR2) instead of a bare halt
*/
#ifndef __VMM_H
#define __VMM_H

#include "typedefs.h"

#define PAGE_SIZE          4096
#define PAGE_ENTRIES       1024        /* entries per directory and per table */
#define PAGE_TABLE_SPAN    (PAGE_ENTRIES * PAGE_SIZE)   /* 4 MiB per table */

/* Flags for a page directory or page table entry */
#define PAGE_PRESENT       0x001
#define PAGE_WRITE         0x002
#define PAGE_USER          0x004
#define PAGE_WRITETHROUGH  0x008
#define PAGE_NOCACHE       0x010
#define PAGE_ACCESSED      0x020
#define PAGE_DIRTY         0x040
#define PAGE_GLOBAL        0x100

/* Bits in the error code the CPU pushes for a page fault (interrupt 14) */
#define PF_PROTECTION      0x01   /* 0 = page not present, 1 = protection violation */
#define PF_WRITE           0x02   /* 0 = read, 1 = write */
#define PF_USER            0x04   /* 0 = supervisor, 1 = user mode */
#define PF_RESERVED        0x08   /* a reserved bit was set in an entry */
#define PF_FETCH           0x10   /* fault during instruction fetch */

/* Builds the page tables for all usable RAM and turns paging on.
 * Requires an initialised pmm, because the tables come from pmm_alloc_frame().
 * Call before heap_init(), so the heap grows into an already mapped range. */
extern void vmm_init(void);

/* Maps one page. virt and phys are rounded down to a page boundary.
 * Allocates a page table on demand. Returns 0 on success, -1 on failure
 * (out of physical memory for a new table). */
extern int vmm_map(uint32_t virt, uint32_t phys, uint32_t flags);

/* Removes a mapping and invalidates the TLB entry. Returns 0 on success,
 * -1 if nothing was mapped there. */
extern int vmm_unmap(uint32_t virt);

/* Physical address behind a virtual one, or 0 if unmapped. Keeps the offset
 * within the page. Note that 0 doubles as "not mapped" -- page zero is
 * deliberately never mapped, so the value is unambiguous here. */
extern uint32_t vmm_get_phys(uint32_t virt);

/* Non-zero if the page holding virt is present. */
extern int vmm_is_mapped(uint32_t virt);

/* Non-zero once paging is active. */
extern int vmm_enabled(void);

/* Statistics for the shell: page tables in use, and pages currently mapped. */
extern uint32_t vmm_table_count(void);
extern uint32_t vmm_mapped_pages(void);

/* Physical address of the active page directory (the value in CR3). */
extern uint32_t vmm_directory_phys(void);

/* Faulting address of the last page fault, taken from CR2 by the handler. */
extern uint32_t vmm_read_cr2(void);

#endif
