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

/* The kernel lives in the top quarter of the address space. It is linked for
*  0xC0100000 but loaded at physical 0x00100000, and all usable RAM is mapped
*  as a contiguous block starting at KERNEL_VIRTUAL_BASE. That fixed offset is
*  what makes the two macros below valid.
*
*  V2P / P2V convert between the two views and are the only sanctioned way to
*  do so. Use them wherever a physical address meets a pointer:
*    - page directory and table entries hold PHYSICAL addresses (the MMU reads
*      them), but the kernel dereferences the tables through VIRTUAL ones
*    - pmm_alloc_frame() returns a PHYSICAL address; touching that memory
*      requires P2V()
*    - the multiboot info from the bootloader holds PHYSICAL addresses
*    - memory mapped hardware such as the VGA text buffer at 0xB8000
*
*  The macros only hold for addresses inside the directly mapped window, i.e.
*  physical memory below (4 GiB - KERNEL_VIRTUAL_BASE) = 1 GiB. Anything the
*  vmm maps elsewhere on request has no such relation and must be tracked. */
#define KERNEL_VIRTUAL_BASE  0xC0000000u
#define KERNEL_PHYSICAL_BASE 0x00100000u
#define KERNEL_VIRTUAL_START (KERNEL_VIRTUAL_BASE + KERNEL_PHYSICAL_BASE)

#define V2P(a)  ((uint32_t)(a) - KERNEL_VIRTUAL_BASE)
#define P2V(a)  ((void *)((uint32_t)(a) + KERNEL_VIRTUAL_BASE))

/* Highest physical address the direct mapping can reach. */
#define DIRECT_MAP_LIMIT     (0xFFFFFFFFu - KERNEL_VIRTUAL_BASE)

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

/* --- Address spaces ------------------------------------------------------
*
*  Every task can own a page directory. The upper quarter of it -- entries
*  768..1023, i.e. everything from KERNEL_VIRTUAL_BASE up -- is SHARED with
*  the kernel directory: the same page tables, not copies. That is what lets
*  an interrupt or a system call run in any task's address space, since the
*  kernel code, its data and the task's kernel stack stay mapped throughout.
*
*  The lower three quarters are private. Two tasks can hold the same virtual
*  address with different contents, and neither can reach the other's.
*
*  Caveat worth knowing: sharing happens at directory-entry granularity. A
*  kernel mapping inside an already existing table is visible everywhere at
*  once, but a kernel mapping that needs a BRAND NEW page table would only
*  land in the directory that was active at the time. All RAM is mapped up
*  front by vmm_init(), so this does not arise today.
*
*  An address space is identified by the physical address of its directory,
*  which is exactly the value CR3 takes.
*/
typedef uint32_t addrspace_t;

/* The kernel's own address space, built by vmm_init(). */
extern addrspace_t vmm_kernel_space(void);

/* Fresh address space: kernel half shared, user half empty. 0 on failure. */
extern addrspace_t vmm_create_space(void);

/* Releases a space and every user-half frame and page table it owns. The
*  kernel half is shared and must not be touched. Refuses the kernel space
*  and the currently active one. */
extern void vmm_destroy_space(addrspace_t space);

/* Loads CR3. Safe from inside an interrupt handler because the kernel half
*  is identical in every space -- code, stack and tables stay mapped across
*  the switch. */
extern void vmm_switch_space(addrspace_t space);
extern addrspace_t vmm_current_space(void);

/* Same as vmm_map/vmm_unmap/vmm_get_phys but for a space that is not the
*  active one. Reaches the foreign tables through the direct mapping. */
extern int vmm_map_in(addrspace_t space, uint32_t virt, uint32_t phys, uint32_t flags);
extern int vmm_unmap_in(addrspace_t space, uint32_t virt);
extern uint32_t vmm_get_phys_in(addrspace_t space, uint32_t virt);

/* Non-zero if virt is mapped AND reachable from ring 3 in the active space,
*  i.e. the PAGE_USER bit is set in both the directory and the table entry.
*  This is the check a system call needs for a pointer it was handed: being
*  mapped is not enough, the caller must actually be allowed to see it. */
extern int vmm_is_user_mapped(uint32_t virt);

#endif
