/* linker.ld -- TomatOS kernel link script
 *
 * 32-bit x86, Multiboot 1, ELF32 output.
 *
 * The kernel is loaded at the classic 1 MiB mark. The Multiboot header must
 * appear within the first 8 KiB of the image, so anything that could carry it
 * (an explicit .multiboot section, otherwise start.o's own .text) is placed
 * at the very front of the first output section.
 *
 * Replaces the old flat-binary script in bin/link.ld: GRUB / QEMU load the
 * ELF via its program headers, no OUTPUT_FORMAT("binary") tricks needed.
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(start)

KERNEL_LOAD_ADDR = 0x00100000;   /* 1 MiB */

SECTIONS
{
    . = KERNEL_LOAD_ADDR;

    kernel_start = .;
    _kernel_start = .;

    /* Code. The Multiboot header lives at the top of this section. */
    .text ALIGN(4K) :
    {
        /* If start.asm ever grows a dedicated section for the header,
         * it is honoured here and comes first. KEEP() protects it from
         * --gc-sections. */
        KEEP(*(.multiboot))
        KEEP(*(.multiboot_header))
        KEEP(*(.mboot))

        /* Otherwise the header sits inside start.o's plain .text, so that
         * object has to be the first one in the section. */
        *start.o(.text .text.*)

        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    }

    /* Read-only data. */
    .rodata ALIGN(4K) :
    {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    }

    /* Initialised data. */
    .data ALIGN(4K) :
    {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
    }

    /* Zero-initialised data (incl. the kernel stack from start.asm). */
    .bss ALIGN(4K) :
    {
        bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        bss_end = .;
    }

    . = ALIGN(4K);

    /* End of the kernel image -- the memory manager will start allocating
     * from here. Both spellings are provided for convenience. */
    kernel_end = .;
    _kernel_end = .;
    end = .;

    /* Toolchain leftovers we do not want in a freestanding kernel. */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
