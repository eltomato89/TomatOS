; TomatOS - shared layout for the boot chain
;
; The kernel is handed over exactly the way GRUB would hand it over: eax
; holds 0x2BADB002 and ebx points at a multiboot info structure. That is a
; deliberate choice -- the kernel already understands it, and keeping the
; contract means GRUB stays a working alternative rather than being
; replaced by something only we can boot.
;
; Disk layout. Stage 2 and the kernel live in the FAT16 volume's RESERVED
; sectors, the area the filesystem sets aside before its first FAT. That
; way the boot chain needs no FAT reader in assembly, while the filesystem
; the kernel mounts stays a perfectly ordinary FAT16 volume.
;
;   LBA 0            stage 1, in the boot sector, sharing it with the BPB
;   LBA 1  .. 16     stage 2                       (8 KiB)
;   LBA 17 .. 527    kernel image, raw             (255 KiB)
;   LBA 528 ..       the FAT16 filesystem proper
;
; mformat is told to reserve 528 sectors, so nothing above ever overlaps
; the filesystem. The kernel is written as a flat image, not as ELF: stage 2
; would otherwise have to parse program headers in real mode, and the
; Makefile can do that with objcopy instead.

STAGE2_LBA          equ 1
STAGE2_SECTORS      equ 16
KERNEL_LBA          equ 17
KERNEL_MAX_SECTORS  equ 511
; --- Multiboot modules on the disk -----------------------------------------
;
; WHY THESE EXIST. On the machine this boot chain is aimed at -- a ThinkPad
; T430, and anything else built after about 2010 -- the kernel cannot read the
; stick it booted from. Its ATA driver speaks to IDE and SATA controllers
; through port I/O and a USB stick is neither, and its USB mass storage driver
; reaches the bus through UHCI, which Panther Point and everything after it
; dropped in favour of EHCI and xHCI. So a kernel that expected to find /BIN on
; the medium would come up with no commands at all.
;
; The BIOS has no such problem: it reads the stick through int 13h, which is
; how stage 1 and stage 2 got here in the first place. So stage 2 loads the
; programs the same way it loads the kernel and hands them over as Multiboot
; modules -- the mechanism the kernel already implements for the GRUB and the
; "-kernel" paths, and the one pmm.c already protects from the frame
; allocator.
;
; The region sits between the kernel and the filesystem, so the layout stays
; one run of reserved sectors in front of a FAT volume that starts where the
; BPB says it does.
MODULES_LBA         equ 528
MODULES_MAX_SECTORS equ 512         ; 256 KiB for all of them together

; Where they are loaded, physically.
;
; THIS WAS 0x140000 AND THAT WAS WRONG, in a way worth writing down because the
; reasoning looked airtight. The argument was: the kernel starts at KERNEL_PHYS
; and the image is at most KERNEL_MAX_SECTORS long, so it ends by 0x13FE00 and
; the next page above that is safe "by construction rather than by checking".
;
; The image is not the kernel. .bss is NOBITS: it occupies no bytes on disk and
; every one of them in RAM. It runs to physical 0x1516E4, so 0x140000 sat in
; the middle of it, and the kernel wiped five of the seven modules the moment
; it cleared its own .bss. QEMU hid it completely -- the shell found the same
; programs on the FAT volume and ran them -- and on a machine that cannot read
; its own medium, which is the entire reason this mechanism exists, it would
; have been a shell with no commands and no clue why.
;
; So: above the .bss, with room, and NOT trusted. The Makefile computes the
; kernel's real end from the ELF's program headers and refuses to build an
; image whose modules would land below it. A constant picked from today's
; measurement is the same mistake one size larger.
MODULES_PHYS        equ 0x00200000

; The multiboot_module array and the command line strings it points at. Both
; live in low memory next to the rest of the multiboot structures, because the
; kernel reads them through the direct mapping and they must survive until
; exec_init() looks. See the map above MB_INFO for what is where.
;
;   MB_INFO   0x7000 .. 0x7074   the structure itself
;   MB_MMAP   0x7200 .. 0x7500   32 entries of 24 bytes
;   MB_MODS   0x7500 .. 0x7600   16 entries of 16 bytes
;   MB_MODSTR 0x7600 .. 0x7700   the names, NUL terminated
;                     .. 0x7C00  stack, growing down from STAGE2_STACK
MB_MODS             equ 0x7500
MB_MODS_MAX         equ 16
MB_MOD_SIZE         equ 16          ; mod_start, mod_end, cmdline, reserved
MB_MODSTR           equ 0x7600
MB_MODSTR_MAX       equ 256

; The table in stage 2 that says where the modules are. Written into the image
; by the Makefile after the programs are built, exactly as s2_kernel_sectors
; is, and read by stage 2 at run time. Both sides code to these offsets, so
; they are stated once here rather than twice in two languages.
;
;   file offset 4  .. 11   'TOMATOS2'      the magic the Makefile asserts
;               12 .. 15   kernel sectors
;               16 .. 19   module count
;               20 ..      MB_MODS_MAX entries of S2MOD_ENTRY_SIZE:
;                            +0  dword  first LBA of this module
;                            +4  dword  its length in bytes
;                            +8  16     its name, NUL padded
;
; The name is what the kernel takes as the module's command line, and the
; first word of that is what "ps" and the shell call the program. Sixteen
; bytes is more than the eight a FAT 8.3 name allows, so nothing that fits on
; the disk fails to fit here.
S2HDR_MODCOUNT_OFF  equ 16
S2HDR_MODTAB_OFF    equ 20
S2MOD_ENTRY_SIZE    equ 24
S2MOD_LBA_OFF       equ 0
S2MOD_BYTES_OFF     equ 4
S2MOD_NAME_OFF      equ 8
S2MOD_NAME_MAX      equ 16

RESERVED_SECTORS    equ 1040

; Real mode load addresses. Everything below 0x7C00 up to 0x0500 is free,
; and 0x7E00 upward is free until the BIOS data at 0x9FC00.
; BIOS data area, the keyboard shift-state byte. Bit 0 is right shift and bit
; 1 is left shift, and they read as "held right now" rather than as something
; buffered -- which is exactly why the text mode escape hatch uses them: a
; person looking at a black screen can hold shift through POST and find out
; nothing about whether it worked until the machine comes up in 80x25.
BIOS_KBD_FLAGS      equ 0x0417

STAGE1_ORG          equ 0x7C00      ; where the BIOS puts us
STAGE2_ORG          equ 0x8000      ; stage 1 loads stage 2 here
STAGE2_STACK        equ 0x7C00      ; grows down, below stage 2

; The kernel is linked for 0xC0100000 but loaded at physical 0x00100000 --
; above the 1 MiB line, which real mode cannot address. Stage 2 therefore
; switches the data segment limits to 4 GiB ("unreal mode") so it can copy
; there with a 32-bit offset while BIOS calls still work.
KERNEL_PHYS         equ 0x00100000

; Buffer stage 2 reads disk sectors into before copying them up. Must sit
; below 1 MiB and not cross a 64 KiB boundary.
;
; Placed directly ABOVE stage 2's own 8 KiB, not inside it: stage 2 is
; loaded at 0x8000 and may occupy STAGE2_SECTORS * 512 = 8 KiB, i.e. up to
; 0xA000. An earlier value of 0x9000 sat in the middle of that and would
; have had stage 2 read sectors over its own code and buffers -- with the
; damage appearing only once it grew past 4 KiB.
;   0xA000 .. 0xC000 is free, and 8 KiB from 0xA000 crosses no 64 KiB
;   boundary, which the BIOS disk services require.
DISK_BUFFER         equ 0xA000
DISK_BUFFER_SECTORS equ 16

; Multiboot info structure, built by stage 2 and pointed at by ebx.
MB_INFO             equ 0x7000      ; the structure itself
MB_MMAP             equ 0x7200      ; E820 entries, multiboot format
MB_MMAP_MAX         equ 32          ; entries we are willing to record

MULTIBOOT_MAGIC_EAX equ 0x2BADB002

; Bits stage 2 sets in the info structure's flags field.
MB_FLAG_MEMORY      equ 0x001
MB_FLAG_BOOTDEV     equ 0x002
MB_FLAG_MODS        equ 0x008
MB_FLAG_MMAP        equ 0x040
MB_FLAG_FRAMEBUFFER equ 0x1000

; Offsets inside the multiboot info structure (see src/include/multiboot.h,
; where the same offsets are asserted against the specification).
MBI_FLAGS           equ 0
MBI_MEM_LOWER       equ 4
MBI_MEM_UPPER       equ 8
MBI_BOOT_DEVICE     equ 12
MBI_MODS_COUNT      equ 20
MBI_MODS_ADDR       equ 24
MBI_MMAP_LENGTH     equ 44
MBI_MMAP_ADDR       equ 48
MBI_FB_ADDR_LOW     equ 88
MBI_FB_ADDR_HIGH    equ 92
MBI_FB_PITCH        equ 96
MBI_FB_WIDTH        equ 100
MBI_FB_HEIGHT       equ 104
MBI_FB_BPP          equ 108
MBI_FB_TYPE         equ 109
MBI_FB_RED_POS      equ 110
MBI_FB_RED_SIZE     equ 111
MBI_FB_GREEN_POS    equ 112
MBI_FB_GREEN_SIZE   equ 113
MBI_FB_BLUE_POS     equ 114
MBI_FB_BLUE_SIZE    equ 115
MBI_SIZE            equ 116

MB_FB_TYPE_INDEXED  equ 0
MB_FB_TYPE_RGB      equ 1
MB_FB_TYPE_TEXT     equ 2
