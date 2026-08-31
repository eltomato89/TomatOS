/* TomatOS - Link script for user space programs
*  Desc: Turns a freestanding object file into an ET_EXEC the loader can map.
*
*  ------------------------------------------------------------------------
*  Why 0x00400000
*  ------------------------------------------------------------------------
*  The address has to satisfy four things at once:
*
*    1. Below KERNEL_VIRTUAL_BASE (0xC0000000, src/include/vmm.h). Everything
*       from there up belongs to the kernel and is mapped without PAGE_USER,
*       so a program placed there would fault on its first instruction.
*
*    2. Clear of the user stacks. taskmgr parks them directly under the
*       kernel: USER_STACK_REGION_TOP is 0xBFFFF000 and with 64 tasks at
*       8 KiB per slot the region reaches down to 0xBFF80000 (src/kernel/tasks.c).
*       4 MiB is nowhere near it -- there are almost 3 GiB of untouched
*       address space between the two, room enough for a heap and for
*       whatever mmap-like thing comes later.
*
*    3. NOT at 0. The first page table entry stays unmapped on purpose, so a
*       null pointer dereference in ring 3 is a page fault instead of a
*       silent read of whatever happens to live at address 0. Linking at
*       0x00400000 leaves the entire first 4 MiB unmapped.
*
*    4. Aligned to 4 MiB, i.e. exactly on a page directory boundary. The
*       program therefore starts a page table of its own (PDE index 1) and
*       shares it with nothing -- one page table covers 4 MiB, which is more
*       than any program here will need for a long time. Had the program been
*       linked at, say, 0x08048000 (the traditional System V i386 address) it
*       would sit in the middle of a directory entry for no benefit.
*
*  ------------------------------------------------------------------------
*  Why exactly one PT_LOAD
*  ------------------------------------------------------------------------
*  The loader in src/kernel/exec.c walks the program headers and acts on PT_LOAD
*  only; it allocates a frame per page, copies p_filesz bytes in, zeroes the
*  rest up to p_memsz and maps the result with PAGE_USER. Every additional
*  segment is another allocate-copy-map round for no gain: 32-bit x86 without
*  PAE has no NX bit, so splitting text and data into a read-execute and a
*  read-write segment would not actually buy any protection here. The
*  explicit PHDRS block below therefore declares a single RWX segment, and
*  because the block is explicit, ld emits that one and nothing else -- no
*  PT_GNU_STACK, no PT_GNU_PROPERTY, no PT_PHDR to confuse anyone reading
*  "readelf -l".
*
*  .bss goes into the same segment and is the reason p_memsz comes out larger
*  than p_filesz, which is precisely the case the loader has to zero.
*
*  There is no PT_INTERP and no PT_DYNAMIC: the programs are linked -static,
*  and the loader rejects a PT_INTERP outright because there is no dynamic
*  linker to hand the file to.
*/

ENTRY(_start)

PHDRS
{
    /* FLAGS(7) = PF_R | PF_W | PF_X */
    load PT_LOAD FLAGS(7);
}

SECTIONS
{
    . = 0x00400000;

    .text : {
        *(.text .text.*)
    } :load

    .rodata : {
        *(.rodata .rodata.*)
    } :load

    .data : {
        *(.data .data.*)
    } :load

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :load

    /* Nothing here reads its own unwind tables or build notes, and a
    *  .note.* section would otherwise drag a segment along. */
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
    }
}
