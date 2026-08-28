/* linker.ld -- TomatOS kernel link script
 *
 * 32-bit x86, Multiboot 1, ELF32 output.
 *
 * Higher half layout. The kernel is LINKED for virtual 0xC0100000 but still
 * LOADED at physical 0x00100000, the classic 1 MiB mark. The two views are
 * separated by exactly KERNEL_VIRTUAL_BASE, which is the same constant the C
 * side uses in src/include/vmm.h (V2P / P2V).
 *
 * The split is expressed with the AT() operator: every output section gets a
 * virtual address (VMA) at 0xC01xxxxx and a load address (LMA) at 0x001xxxxx.
 * ld writes the LMA into p_paddr of the program header, which is the field the
 * Multiboot ELF loader in GRUB uses to place the image in physical memory.
 *
 * The Multiboot header must appear within the first 8 KiB of the image FILE,
 * so anything that could carry it (an explicit .multiboot section, otherwise
 * start.o's own .text) is placed at the very front of the first output
 * section, and that section is the first thing in the file.
 *
 * Replaces the old flat-binary script in bin/link.ld: GRUB / QEMU load the
 * ELF via its program headers, no OUTPUT_FORMAT("binary") tricks needed.
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(start)

KERNEL_VIRTUAL_BASE = 0xC0000000;   /* must match vmm.h */
KERNEL_LOAD_ADDR    = 0x00100000;   /* 1 MiB, physical */
KERNEL_VIRTUAL_ADDR = KERNEL_VIRTUAL_BASE + KERNEL_LOAD_ADDR;   /* 0xC0100000 */

SECTIONS
{
    /* The location counter tracks VIRTUAL addresses from here on. */
    . = KERNEL_VIRTUAL_ADDR;

    kernel_start = .;
    _kernel_start = .;

    /* Code. The Multiboot header lives at the top of this section.
     *
     * The vmm maps everything between text_start and text_end read-only, so
     * both boundaries have to be page boundaries. ALIGN(4K) on the output
     * section only guarantees the start; the trailing ". = ALIGN(4K)" pads
     * the section out to a full page so that text_end is aligned too.
     * Without that padding the last page of .text would also hold the first
     * bytes of the following section, and marking it read-only would fault
     * on the first write to a variable living there. */
    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE)
    {
        text_start = .;

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

        . = ALIGN(4K);
        text_end = .;
    }

    /* Ring 3 code.
     *
     * Everything that executes with CPL 3 is put here by
     * __attribute__((section(".usertext"))) in the C source, so that the
     * whole set of such routines forms ONE contiguous, whole-page range that
     * can be handed to ring 3 on its own. The kernel copies this range into
     * frames of its own at boot-up of the demo and maps only the copy with
     * PAGE_USER -- no page of kernel .text is ever opened to ring 3, and no
     * unrelated code shares a page with the ring 3 routines.
     *
     * Two properties matter and both are produced here:
     *
     *   - the range is page aligned at BOTH ends, so a whole number of pages
     *     covers it exactly. ALIGN(4K) on the output section only fixes the
     *     start; the explicit ". = ALIGN(4K)" before usertext_end pads the
     *     tail, the same trap that .text above documents.
     *
     *   - the routines end up next to each other, so a call between two of
     *     them is a relative one and survives being copied elsewhere. What
     *     must not appear in here is an absolute reference to anything
     *     outside the section; "objdump -r" over the object file is the
     *     check, and it has to come back empty for .usertext. */
    .usertext ALIGN(4K) : AT(ADDR(.usertext) - KERNEL_VIRTUAL_BASE)
    {
        usertext_start = .;
        *(.usertext .usertext.*)
        . = ALIGN(4K);
        usertext_end = .;
    }

    /* Read-only data. Same padding rule as .text: the range is a candidate
     * for a read-only mapping of its own. */
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE)
    {
        rodata_start = .;
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(4K);
        rodata_end = .;
    }

    /* Initialised data. Writable, so the padding here is only for symmetry
     * and to keep .bss starting on its own page. */
    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE)
    {
        data_start = .;
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(4K);
        data_end = .;
    }

    /* Zero-initialised data (incl. the kernel stack and the boot page
     * directory from start.asm). start.asm touches parts of this section
     * BEFORE paging is on, through their physical aliases. */
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRTUAL_BASE)
    {
        bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        bss_end = .;
    }

    . = ALIGN(4K);

    /* End of the kernel image -- the memory manager will start allocating
     * from here. Both spellings are provided for convenience.
     * These are VIRTUAL addresses now. */
    kernel_end = .;
    _kernel_end = .;
    end = .;

    /* Physical counterparts of the boundary symbols. The boot code in
     * start.asm and any early code that runs with paging off (or that has to
     * hand a physical address to the MMU / to the pmm) needs the load view.
     * Derived here rather than open coded as "sym - 0xC0000000" all over the
     * place, so the offset lives in exactly one file.
     *
     * The C side has V2P() / P2V() from vmm.h for the same job; these exist
     * for assembly and for anything that wants the value as a link time
     * constant. */
    kernel_start_phys = kernel_start - KERNEL_VIRTUAL_BASE;
    kernel_end_phys   = kernel_end   - KERNEL_VIRTUAL_BASE;
    text_start_phys   = text_start   - KERNEL_VIRTUAL_BASE;
    text_end_phys     = text_end     - KERNEL_VIRTUAL_BASE;
    usertext_start_phys = usertext_start - KERNEL_VIRTUAL_BASE;
    usertext_end_phys   = usertext_end   - KERNEL_VIRTUAL_BASE;
    rodata_start_phys = rodata_start - KERNEL_VIRTUAL_BASE;
    rodata_end_phys   = rodata_end   - KERNEL_VIRTUAL_BASE;
    data_start_phys   = data_start   - KERNEL_VIRTUAL_BASE;
    data_end_phys     = data_end     - KERNEL_VIRTUAL_BASE;
    bss_start_phys    = bss_start    - KERNEL_VIRTUAL_BASE;
    bss_end_phys      = bss_end      - KERNEL_VIRTUAL_BASE;

    /* Toolchain leftovers we do not want in a freestanding kernel. */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
