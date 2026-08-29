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
RESERVED_SECTORS    equ 528

; Real mode load addresses. Everything below 0x7C00 up to 0x0500 is free,
; and 0x7E00 upward is free until the BIOS data at 0x9FC00.
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
MB_FLAG_MMAP        equ 0x040
MB_FLAG_FRAMEBUFFER equ 0x1000

; Offsets inside the multiboot info structure (see src/include/multiboot.h,
; where the same offsets are asserted against the specification).
MBI_FLAGS           equ 0
MBI_MEM_LOWER       equ 4
MBI_MEM_UPPER       equ 8
MBI_BOOT_DEVICE     equ 12
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
