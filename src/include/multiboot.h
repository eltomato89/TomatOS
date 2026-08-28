/* TomatOS - Multiboot 1 structures
*  Desc: What the bootloader (GRUB or QEMU -kernel) hands us at startup.
*
*  The 64-bit fields of the memory map are deliberately modelled as pairs of
*  two 32-bit values: the kernel is 32-bit, and this way we avoid 64-bit
*  arithmetic and the libgcc helper routines that come with it.
*/
#ifndef __MULTIBOOT_H
#define __MULTIBOOT_H

#include "typedefs.h"

/* Value the bootloader leaves in eax according to the specification */
#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002

/* Bits in the flags field of the info structure */
#define MULTIBOOT_INFO_MEMORY       0x00000001  /* mem_lower / mem_upper valid */
#define MULTIBOOT_INFO_BOOTDEV      0x00000002
#define MULTIBOOT_INFO_CMDLINE      0x00000004
#define MULTIBOOT_INFO_MODS         0x00000008
#define MULTIBOOT_INFO_MEM_MAP      0x00000040  /* mmap_addr / mmap_length valid */
#define MULTIBOOT_INFO_BOOT_LOADER  0x00000200

/* Types of a memory map entry */
#define MULTIBOOT_MEMORY_AVAILABLE  1
#define MULTIBOOT_MEMORY_RESERVED   2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS        4
#define MULTIBOOT_MEMORY_BADRAM     5

/* A single entry of the memory map.
 * Careful: size counts the bytes AFTER the size field itself. Advancing to
 * the next entry therefore goes via (address_of_entry + size + 4). */
typedef struct
{
    uint32_t size;
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t len_low;
    uint32_t len_high;
    uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry;

/* A module the bootloader loaded alongside the kernel. GRUB puts one of
*  these per "module" line in grub.cfg. Addresses are PHYSICAL. */
typedef struct
{
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;      /* physical address of a NUL terminated string */
    uint32_t reserved;
} __attribute__((packed)) multiboot_module;

typedef struct
{
    uint32_t flags;

    /* valid with MULTIBOOT_INFO_MEMORY - in KiB */
    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;
    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t syms[4];

    /* valid with MULTIBOOT_INFO_MEM_MAP */
    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
} __attribute__((packed)) multiboot_info;

#endif
