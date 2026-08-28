/* TomatOS - Minimal ELF32 definitions
*  Desc: Just enough of the format to load a static executable.
*
*  Only what the loader actually reads: the file header to find the program
*  headers and the entry point, and the program headers to know which parts
*  of the file go where in memory. Sections, symbols and relocations are of
*  no interest -- a statically linked executable needs none of them at load
*  time.
*/
#ifndef __ELF_H
#define __ELF_H

#include "typedefs.h"

#define EI_NIDENT   16

/* e_ident bytes we check */
#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS32  1       /* 32-bit objects        */
#define ELFDATA2LSB 1       /* little endian         */

/* e_type */
#define ET_EXEC     2       /* executable, fixed load addresses */

/* e_machine */
#define EM_386      3

/* p_type */
#define PT_NULL     0
#define PT_LOAD     1       /* the only one we act on */
#define PT_DYNAMIC  2
#define PT_INTERP   3       /* presence means it needs a dynamic linker */

/* p_flags */
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

typedef struct
{
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;       /* virtual address to start at */
    uint32_t e_phoff;       /* program header table, file offset */
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr;

typedef struct
{
    uint32_t p_type;
    uint32_t p_offset;      /* where it sits in the file    */
    uint32_t p_vaddr;       /* where it belongs in memory   */
    uint32_t p_paddr;
    uint32_t p_filesz;      /* bytes present in the file    */
    uint32_t p_memsz;       /* bytes needed in memory; the
                             * difference to p_filesz is .bss
                             * and must be zeroed            */
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) elf32_phdr;

#endif
