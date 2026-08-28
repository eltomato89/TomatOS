/* TomatOS - Multiboot 1 Strukturen
*  Desc: Was der Bootloader (GRUB oder QEMU -kernel) uns beim Start uebergibt.
*
*  Die 64-Bit-Felder der Memory-Map sind bewusst als Paare aus zwei 32-Bit-
*  Werten abgebildet: der Kernel ist 32-bittig, und so vermeiden wir
*  64-Bit-Arithmetik und die zugehoerigen libgcc-Hilfsroutinen.
*/
#ifndef __MULTIBOOT_H
#define __MULTIBOOT_H

#include "typedefs.h"

/* Wert, den der Bootloader laut Spezifikation in eax hinterlaesst */
#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002

/* Bits im flags-Feld der Info-Struktur */
#define MULTIBOOT_INFO_MEMORY       0x00000001  /* mem_lower / mem_upper gueltig */
#define MULTIBOOT_INFO_BOOTDEV      0x00000002
#define MULTIBOOT_INFO_CMDLINE      0x00000004
#define MULTIBOOT_INFO_MODS         0x00000008
#define MULTIBOOT_INFO_MEM_MAP      0x00000040  /* mmap_addr / mmap_length gueltig */
#define MULTIBOOT_INFO_BOOT_LOADER  0x00000200

/* Typen eines Memory-Map-Eintrags */
#define MULTIBOOT_MEMORY_AVAILABLE  1
#define MULTIBOOT_MEMORY_RESERVED   2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS        4
#define MULTIBOOT_MEMORY_BADRAM     5

/* Ein Eintrag der Memory-Map.
 * Achtung: size zaehlt die Bytes NACH dem size-Feld selbst. Zum naechsten
 * Eintrag geht es daher ueber (addr_des_eintrags + size + 4). */
typedef struct
{
    uint32_t size;
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t len_low;
    uint32_t len_high;
    uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry;

typedef struct
{
    uint32_t flags;

    /* gueltig bei MULTIBOOT_INFO_MEMORY - in KiB */
    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;
    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t syms[4];

    /* gueltig bei MULTIBOOT_INFO_MEM_MAP */
    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
} __attribute__((packed)) multiboot_info;

#endif
