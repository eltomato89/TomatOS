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
#define MULTIBOOT_INFO_VBE           0x00000800  /* vbe_* fields valid       */
#define MULTIBOOT_INFO_FRAMEBUFFER   0x00001000  /* framebuffer_* fields valid */

/* framebuffer_type */
#define MULTIBOOT_FRAMEBUFFER_INDEXED  0   /* palette, one byte per pixel */
#define MULTIBOOT_FRAMEBUFFER_RGB      1   /* direct colour               */
#define MULTIBOOT_FRAMEBUFFER_EGA_TEXT 2   /* the usual 0xB8000 text mode */

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

    /* gueltig bei MULTIBOOT_INFO_VBE -- untouched by this kernel, but the
    *  fields have to be here so the ones after them sit at the right
    *  offset. */
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    /* gueltig bei MULTIBOOT_INFO_FRAMEBUFFER.
    *
    *  This is how a graphics mode reaches the kernel. It cannot be set from
    *  protected mode -- a VBE mode needs int 0x10, which is real mode only --
    *  so whoever boots us establishes the mode and describes it here. GRUB
    *  does it when the video fields are set in the multiboot header, and our
    *  own stage 2 does the same before it leaves real mode.
    *
    *  pitch is the byte distance between two rows and is NOT width * bpp/8:
    *  hardware commonly pads rows. Computing the row offset from the width
    *  is the classic way to get a picture that slants across the screen. */
    /* Split into two halves for the same reason the memory map entries are:
    *  this kernel has no uint64_t, and 64-bit arithmetic would drag in
    *  libgcc helpers. A framebuffer above 4 GiB is unreachable for us
    *  anyway, so framebuffer_addr_high being non-zero means "cannot use
    *  it". */
    uint32_t framebuffer_addr_low;
    uint32_t framebuffer_addr_high;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;

    /* For type RGB: position and size of each channel within a pixel. */
    uint8_t  framebuffer_red_field_position;
    uint8_t  framebuffer_red_mask_size;
    uint8_t  framebuffer_green_field_position;
    uint8_t  framebuffer_green_mask_size;
    uint8_t  framebuffer_blue_field_position;
    uint8_t  framebuffer_blue_mask_size;
} __attribute__((packed)) multiboot_info;

#endif
