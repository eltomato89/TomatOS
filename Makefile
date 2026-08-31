# ============================================================================
#  TomatOS -- Build system
#
#  32-bit x86 hobby kernel (Multiboot 1) from 2011, ported to a modern
#  Linux toolchain (GCC 16 / GNU ld / NASM 3).
#
#  "make help" lists all available targets.
# ============================================================================

# ---------------------------------------------------------------------------
#  Directories
# ---------------------------------------------------------------------------
SRC_DIR     := src
# Headers stay in ONE flat directory while the .c files below are grouped
# into subsystems, and that asymmetry is the point rather than an oversight.
# A header is an interface: this directory is the list of what the kernel
# offers, readable at a glance, and the grouping of the implementations says
# nothing about who may call them. Keeping it flat also keeps -I to a single
# entry and every #include in the tree to a bare name -- so moving a .c file
# between subsystems, as this layout invites, touches no source at all.
INC_DIR     := $(SRC_DIR)/include
BUILD_DIR   := build
ISO_DIR     := $(BUILD_DIR)/iso

# User space lives entirely outside src/. Its programs are separate ELF
# executables that share nothing with the kernel but the "int 0x80" ABI, so
# they get their own sources, their own headers and their own object
# directory -- see the "User space" block further down.
USER_DIR     := user
USER_OBJ_DIR := $(BUILD_DIR)/user

# Each program is a directory of its own, and every .c in it is part of that
# program -- so a command that outgrows one file needs no change here. The
# two directories that are not programs:
#
#   include/   the headers a program may see, and the whole of its world.
#   lib/       the C library every program links, plus the ones it can opt
#              into. Sources here are never picked up by a program's own
#              wildcard, which is what keeps "one directory, one program"
#              true rather than approximately true.
USER_INC_DIR := $(USER_DIR)/include
USER_LIB_DIR := $(USER_DIR)/lib

# What the disk contains, as files rather than as recipe lines.
#
# Everything under here is copied onto the image with its path and its name
# unchanged, so the tree in the source is the tree the shell walks. Adding a
# file to the disk is putting it in this directory; nothing in this Makefile
# has to learn about it.
#
# THE NAMES ARE UPPER CASE 8.3 ON PURPOSE, and that is the one thing about
# this directory that looks wrong and is not. mtools writes a VFAT long name
# entry for any name that is not a valid 8.3, and src/fs/fat.c skips long name
# entries -- so a file called "readme.txt" here would be copied onto the
# image, take up clusters, and be invisible to every program that lists a
# directory. The name in the source tree is the name on the disk, which also
# means one can see from here what "ls /" will print.
#
# Build outputs do not live here: the programs are compiled into $(BUILD_DIR)
# and joined with this tree in $(STAGING_DIR) below.
SYSROOT_DIR := sysroot

KERNEL      := $(BUILD_DIR)/kernel.elf
ISO         := $(BUILD_DIR)/tomatos.iso

# Bootable GRUB Legacy floppy: the original in bin/ is a read-only template,
# we always work on a copy.
FLOPPY_TMPL := bin/dev_kernel_grub.img
FLOPPY      := $(BUILD_DIR)/tomatos_floppy.img

# FAT hard disk image -- the disk src/drivers/block/ata.c and src/fs/fat.c talk to.
DISK        := $(BUILD_DIR)/tomatos_disk.img

LINKER_SCRIPT      := linker.ld
USER_LINKER_SCRIPT := $(USER_DIR)/user.ld

# Helper scripts that are too long to be recipes. The usb-boot one is almost
# entirely refusals, and a refusal is only worth having if the reason it
# exists is written next to it -- see the target near the bottom of this file.
TOOLS_DIR   := tools
USB_BOOT_SH := $(TOOLS_DIR)/usb-boot.sh

# ---------------------------------------------------------------------------
#  Tools
# ---------------------------------------------------------------------------
CC          := gcc
AS          := nasm
LD          := ld
QEMU        := qemu-system-i386
GRUB_MKRESCUE := grub-mkrescue
OBJCOPY     := objcopy
MCOPY       := mcopy
MFORMAT     := mformat
MMD         := mmd
MDIR        := mdir

# mtools refuses to touch a plain file that does not look like a device it
# knows unless told not to check. Every mtools call below is on a regular
# file in $(BUILD_DIR), so the check has nothing useful to say.
MTOOLS_ENV  := MTOOLS_SKIP_CHECK=1

# ---------------------------------------------------------------------------
#  Flags
#
#  The first three are NOT negotiable:
#
#  -mgeneral-regs-only
#      CRITICAL. Without it modern GCC happily emits SSE instructions for
#      plain integer/struct code. CR4.OSFXSR is never set by this kernel
#      (-> #UD on the first SSE op) and the interrupt stubs in start.asm do
#      not align the stack to 16 bytes (-> #GP/#AC on movaps). Restricting
#      the compiler to general purpose registers avoids both.
#
#  -fno-pic -fno-pie / -no-pie
#      Arch's GCC defaults to PIE. A freestanding kernel linked at a fixed
#      physical address must not be position independent -- otherwise we get
#      GOT relocations and R_386_* errors at link time.
#
#  -std=gnu89
#      The 2011 sources are verified warning-tolerant but error-free under
#      gnu89. Under gnu11 the implicit function declarations become hard
#      errors; under gnu23 the "typedef enum { false, true } bool" in
#      src/include/typedefs.h additionally collides with the built-in bool.
# ---------------------------------------------------------------------------
CFLAGS := \
	-m32 \
	-std=gnu89 \
	-mgeneral-regs-only \
	-fno-pic -fno-pie \
	-ffreestanding -fno-builtin -nostdinc \
	-I $(INC_DIR) \
	-fno-strict-aliasing \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-O1 \
	-fno-omit-frame-pointer \
	-Wall

# Automatic header dependency generation (.d files next to the .o files).
DEPFLAGS := -MMD -MP

ASFLAGS  := -f elf32

# libgcc supplies the compiler helper routines (64-bit division, modulo, ...).
# Query the path lazily so a missing multilib does not break "make help".
LIBGCC    = $(shell $(CC) -m32 -print-libgcc-file-name)

LDFLAGS  := -m elf_i386 -T $(LINKER_SCRIPT) -nostdlib -no-pie

# ---------------------------------------------------------------------------
#  User space
#
#  One entry per program; user/<name>/ becomes $(BUILD_DIR)/<name>.elf, with
#  every .c in that directory compiled into it.
# ---------------------------------------------------------------------------
# Programs that live on the disk rather than in the kernel. "make disk" copies
# each one into /BIN as NAME.ELF, and the shell runs an unknown command by
# looking for it there -- so adding a name here is all it takes to add a
# command. Keep the names to eight characters: they end up as 8.3 entries on a
# FAT16 volume, and a longer one would need a VFAT long name entry, which the
# kernel's directory reader skips.
USER_PROGS := hello ls cat fetch rm cp gui

# The sources and the objects of one program, by name. Wildcarded, so adding
# a second file to a program is dropping it in that program's directory.
#   $(1) = program name
user_srcs = $(sort $(wildcard $(USER_DIR)/$(1)/*.c))
user_objs = $(patsubst $(USER_DIR)/%.c,$(USER_OBJ_DIR)/%.o,$(call user_srcs,$(1)))

# The C library every program links against: user/lib/lib.c, holding _start
# (which calls main), printf, the string routines and the file helpers. It is
# a plain object rather than an archive -- with a handful of programs, "ar"
# would only add a step and a chance for a stale index, and the linker
# garbage collects nothing here either way.
USER_LIB_OBJ := $(USER_OBJ_DIR)/lib/lib.o

# Libraries a program opts into, named per program. gfxlib is NOT in
# USER_LIB_OBJ and must not be: its back buffer is 3 MiB of .bss, and linking
# it into every program would give "ls" a three megabyte memory image for a
# buffer it never touches. The loader allocates a frame per page of memsz, so
# that is not a number on disk -- it is real RAM at every start.
USER_GFX_OBJ   := $(USER_OBJ_DIR)/lib/gfxlib.o
USER_LIBS_gui  := $(USER_GFX_OBJ)

USER_ELFS  := $(patsubst %,$(BUILD_DIR)/%.elf,$(USER_PROGS))
USER_OBJS  := $(foreach p,$(USER_PROGS),$(call user_objs,$(p)))
USER_DEPS  := $(USER_OBJS:.o=.d) $(USER_LIB_OBJ:.o=.d) $(USER_GFX_OBJ:.o=.d)

# The objects deliberately land in a directory of their own. Nothing forces
# the separation otherwise: both sides are -m32 freestanding ELF objects, and
# a stray $(BUILD_DIR)/*.o in the user link line would happily pull kernel
# code into a ring 3 binary that would then fault on its first privileged
# instruction -- or worse, not fault at all and quietly work.
#
# The flags are the kernel's, minus what does not apply:
#
#   -mgeneral-regs-only   Same reason as for the kernel, and it matters even
#                         more here: CR4.OSFXSR is never set, so an SSE
#                         instruction in ring 3 is an immediate #UD, and the
#                         kernel would report a fault in a program that looks
#                         perfectly innocent in its source.
#   -fno-pic -fno-pie     The loader maps an ET_EXEC at fixed addresses and
#                         performs no relocations at all. Position
#                         independent code would need a GOT and someone to
#                         fill it in; there is nobody.
#   -nostdinc -I $(USER_INC_DIR)
#                         No system headers, and NOT $(INC_DIR): a user
#                         program must not be able to reach kernel internals
#                         even by accident. What is in user/include/ is the
#                         whole of its world -- and now that this is a
#                         directory rather than the whole of user/, a program
#                         cannot reach another program's headers either.
USER_CFLAGS := \
	-m32 \
	-std=gnu89 \
	-mgeneral-regs-only \
	-fno-pic -fno-pie \
	-ffreestanding -fno-builtin -nostdinc \
	-I $(USER_INC_DIR) \
	-fno-strict-aliasing \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-O1 \
	-fno-omit-frame-pointer \
	-Wall

# --no-warn-rwx-segments: user.ld puts text, rodata and bss into a single
# read-write-execute PT_LOAD, which newer ld warns about by default. The
# warning is sound advice on a system with an NX bit; 32-bit x86 without PAE
# has none, so the split would cost the loader a second segment and buy
# nothing. See the comment in user/user.ld.
USER_LDFLAGS := -m elf_i386 -T $(USER_LINKER_SCRIPT) -nostdlib -no-pie -static \
                --no-warn-rwx-segments

# Keyboard layout. The kernel carries a German keymap (src/drivers/input/kb.c), so QEMU has
# to deliver German scancodes. Under Wayland QEMU does not pass raw scancodes
# through but translates via the keysym -- without -k the guest ends up with
# the US layout, and the minus key arrives as the German sharp s.
# Override with:  make run QEMU_KEYMAP=en-us
QEMU_KEYMAP ?= de

# Guest memory. 64 rather than the 32 this ran on for years, because a
# graphics mode is expensive twice over: the framebuffer itself, and the back
# buffer a ring 3 program draws into before copying across. At 1920x1080x32
# those are 8.3 MB each, and the loader allocates a frame per page of a
# program's .bss -- so "gui" alone wants more than a quarter of 32 MB before
# the kernel, the heap and the task stacks are counted.
#
# It changes nothing about the kernel, which reads the memory map and adapts.
# Override with:  make run QEMU_MEM=32
QEMU_MEM ?= 64

# ---------------------------------------------------------------------------
#  USB
#
#  QEMU has no USB controller unless one is asked for, which is also the
#  ordinary state of a machine built in the last decade -- those have xHCI and
#  nothing else, and this kernel drives UHCI. So "no controller" is a case the
#  kernel has to handle either way, and USB=0 is how to see it.
#
#  Three settings, because attaching a USB input device has a consequence that
#  is easy to walk into:
#
#      USB=0     no controller at all -- also what a modern machine looks like
#                to this kernel, since those have xHCI and nothing else
#      USB=1     the controller, no devices                        (default)
#      USB=hid   the controller with a keyboard and a mouse on it
#
#  USB=hid works: the HID class driver reads both, and typing on the USB
#  keyboard reaches the shell exactly as the PS/2 one does -- kb_inject() and
#  mouse_inject() feed the same queues, so nothing above learns which bus a
#  keypress came from.
#
#  It is still not the default, and the reason is about failure rather than
#  function. QEMU routes input to the most recently attached device of that
#  kind, so with a USB keyboard present the PS/2 one gets nothing at all --
#  which means a regression in the HID driver produces a machine that boots,
#  shows a prompt, and cannot be typed at. That happened before the driver
#  existed, and it is not a state "make run" should be one bad commit away
#  from.
#
#  The "port=" is not decoration either. Left to itself QEMU attaches the first
#  device to a root port and inserts a HUB for the second, putting it behind
#  that -- and hubs are deliberately not supported, so the second device
#  becomes invisible and the kernel reports "class 9/0/0 hub" instead. Naming
#  the ports puts both on the controller directly. The controller has two, so a
#  third device would need the hub support that is not here yet.
USB ?= 1

ifeq ($(USB),hid)
USBFLAGS := -device piix3-usb-uhci \
            -device usb-kbd,port=1 \
            -device usb-mouse,port=2
else ifeq ($(USB),0)
USBFLAGS :=
else
USBFLAGS := -device piix3-usb-uhci
endif

QEMUFLAGS := -m $(QEMU_MEM) -k $(QEMU_KEYMAP) $(USBFLAGS)

# ---------------------------------------------------------------------------
#  Network
#
#  QEMU's user mode network needs neither root nor a TAP device: it emulates a
#  small network in userspace behind a NAT. The addresses in it are fixed and
#  the kernel has to be told them, since there is no DHCP client:
#
#      10.0.2.15   the guest
#      10.0.2.2    gateway, answers ARP and ICMP echo -- the ping target
#      10.0.2.3    DNS forwarder
#      255.255.255.0
#
#  The card is an RTL8139 because that is the one src/drivers/net/rtl8139.c drives.
#
#  Turn it off with:   make run NET=0
#  Record traffic with: make run NETDUMP=1   -> build/net.pcap, readable by
#  wireshark or "tcpdump -r". That capture is the arbiter when a packet
#  leaves the kernel but nothing answers: it shows whether the frame reached
#  the wire at all and whether its byte order is what we think it is.
NET     ?= 1
NETDUMP ?= 0


ifeq ($(NET),1)
NETDEV := -netdev user,id=n0
ifeq ($(NETDUMP),1)
NETDEV += -object filter-dump,id=dump0,netdev=n0,file=$(abspath $(BUILD_DIR))/net.pcap
endif
QEMUFLAGS += $(NETDEV) -device rtl8139,netdev=n0
endif

# ---------------------------------------------------------------------------
#  FAT hard disk image
#
#  Geometry is chosen, not defaulted, because the FAT type follows from it and
#  the kernel only implements FAT12 and FAT16 (src/include/fat.h).
#
#  Which of the two you get is decided solely by the number of DATA CLUSTERS:
#  below 4085 it is FAT12, below 65525 FAT16, at or above that FAT32. The
#  boundaries are the whole reason this block is explicit -- a geometry picked
#  by feel can land just over one of them and hand the kernel a filesystem it
#  cannot read, and neither mformat nor QEMU will complain.
#
#      65 cylinders * 16 heads * 63 sectors  = 65520 sectors = 32 MB
#      65520 - 1 boot - 128 FAT - 32 root    = 65359 data sectors
#      65359 / 4 sectors per cluster         = 16339 clusters
#
#  16339 sits in the middle of the FAT16 band, far from 4085 and far from
#  65525, so no amount of rounding in anyone's arithmetic can turn this into
#  a FAT12 or FAT32 volume. Verify with "minfo -i $(DISK) ::".
#
#  Two more consequences of staying at 65520 sectors, both deliberate:
#
#    - Under 65536 sectors the count still fits the 16-bit "total sectors"
#      field of the BPB, so the 32-bit one stays zero. A driver that reads
#      only the small field -- which a first FAT implementation usually does
#      -- gets the right answer.
#    - CHS is legal for a BIOS (heads <= 16, sectors <= 63), so nothing here
#      depends on LBA translation being available.
#
#  Media descriptor 0xf8 means "fixed disk"; it is what makes mformat stamp
#  the BPB physical drive id 0x80 instead of the floppy's 0x00.
#
# Note the comments below sit on their own lines: a trailing "# ..." after a
# value is a comment to make, but the blanks in front of it are part of the
# value, and a stray space would end up inside the mformat command line.
DISK_CYLS     := 65
DISK_HEADS    := 16
DISK_SECS     := 63
# Sectors per cluster -> 2 KB clusters.
DISK_CLUSTER  := 4
# Media descriptor: fixed disk.
DISK_MEDIA    := 0xf8
DISK_LABEL    := TOMATOS

DISK_TOTAL_SECS := $(shell expr $(DISK_CYLS) \* $(DISK_HEADS) \* $(DISK_SECS))
# Rounded, not truncated: 65520 sectors are 31.99 MB, and reporting that as
# "31 MB" would invite someone to fix a size that is not broken.
DISK_SIZE_MB    := $(shell expr \( $(DISK_TOTAL_SECS) + 1024 \) / 2048)

# How the image is handed to QEMU.
#
# This is exactly what "-hda" expands to (index=0, media=disk -> IDE bus 0
# master, the drive src/drivers/block/ata.c calls drive 0), with "format=raw" spelled out.
# Without it QEMU probes the file and prints a warning about having guessed --
# harmless, but it would be the only noise in an otherwise quiet build.
#
# -hda and -cdrom do not collide: -cdrom is index=2, i.e. IDE bus 1 master. A
# run-iso therefore boots from the CD and finds the hard disk beside it. The
# BIOS boot order does have to be forced there, see the run-iso target.
QEMU_DISK := -drive file=$(notdir $(DISK)),format=raw,index=0,media=disk

# ===========================================================================
#  The own boot chain -- stage 1, stage 2, flat kernel, all on one disk
#
#  This is the GRUB-free path: the BIOS loads LBA 0, stage 1 loads stage 2,
#  stage 2 loads the kernel and enters it with the Multiboot register
#  contract GRUB would have used. See boot/layout.inc, which is the single
#  source of truth for the sector layout -- the numbers below are READ OUT of
#  it rather than repeated here, so the two cannot drift apart.
#
#      LBA 0            stage 1, sharing the boot sector with the FAT16 BPB
#                       and with an MBR partition table at offset 446
#      LBA 1 .. 16      stage 2                       (8 KiB)
#      LBA 17 .. 527    kernel, flat image            (255 KiB)
#      LBA 528 .. 1039  user programs as Multiboot modules      (256 KiB)
#      LBA 1040 ..      the FAT16 filesystem proper
#
#  The ISO path deliberately keeps GRUB. The kernel is entered identically
#  either way, so having both is a check on the handover, not duplication.
# ===========================================================================
BOOT_DIR    := boot
LAYOUT_INC  := $(BOOT_DIR)/layout.inc

STAGE1_SRC  := $(BOOT_DIR)/stage1.asm
STAGE2_SRC  := $(BOOT_DIR)/stage2.asm

# Every .inc in boot/ is an include candidate for both stages (layout.inc,
# vbe.inc, ...). Wildcarded, so a new one is picked up without editing this
# file and a not-yet-written one does not break the parse.
BOOT_INCS   := $(wildcard $(BOOT_DIR)/*.inc)

STAGE1_BIN  := $(BUILD_DIR)/stage1.bin
STAGE2_BIN  := $(BUILD_DIR)/stage2.bin
KERNEL_BIN  := $(BUILD_DIR)/kernel.bin
BOOTIMG     := $(BUILD_DIR)/tomatos_boot.img

# Pull one "NAME equ <decimal>" out of layout.inc. The fallback after the
# comma is what the layout said when this block was written; it only ever
# applies if layout.inc has gone missing, in which case the size checks
# below would otherwise silently compare against nothing.
layout_value = $(or $(shell awk '$$1 == "$(1)" && $$2 == "equ" { print $$3; exit }' \
                            $(LAYOUT_INC) 2>/dev/null),$(2))

STAGE2_LBA         := $(call layout_value,STAGE2_LBA,1)
STAGE2_SECTORS     := $(call layout_value,STAGE2_SECTORS,16)
KERNEL_LBA         := $(call layout_value,KERNEL_LBA,17)
KERNEL_MAX_SECTORS := $(call layout_value,KERNEL_MAX_SECTORS,511)
RESERVED_SECTORS   := $(call layout_value,RESERVED_SECTORS,528)

KERNEL_MAX_BYTES   := $(shell expr $(KERNEL_MAX_SECTORS) \* 512)

# ---------------------------------------------------------------------------
#  >>> HOW THE KERNEL'S SIZE REACHES STAGE 2 <<<
#  >>> the contract between this file and boot/stage2.asm -- change one and <<<
#  >>> you must change the other <<<
#
#  Stage 2 must not read 511 sectors when the kernel is 168: everything past
#  the image is filesystem, reading it is slow, and a flat image has no
#  length of its own once objcopy has thrown the ELF headers away. So the
#  BUILD writes the length down and stage 2 reads it back out of itself.
#
#  stage2.asm opens with a small patchable header, deliberately at the very
#  front where nothing can move underneath it:
#
#      file offset  0 ..  3   jmp s2_start, padded to 4 bytes
#      file offset  4 .. 11   the ASCII magic "TOMATOS2"
#      file offset 12 .. 15   dd, kernel size in 512 byte sectors, LE
#
#  The Makefile patches offset 12 IN THE IMAGE, not in $(STAGE2_BIN): the
#  .bin stays exactly what nasm produced, and the copy at LBA $(STAGE2_LBA) of
#  $(BOOTIMG) carries the real number. Disk byte offset of the field:
#
#      $(STAGE2_LBA) * 512 + 12
#
#  The magic at offset 4 is checked before the patch is written. It is the
#  entire safety net -- without it a reordered stage2.asm would have four of
#  its instruction bytes silently replaced by a sector count, and the failure
#  would show up as a triple fault with no explanation.
#
#  Stage 2's built-in default is KERNEL_MAX_SECTORS, so an UNPATCHED
#  stage2.bin still boots; it just reads the whole reserved area. That is why
#  the patch has to be verified rather than trusted to be obviously missing.
#
#  Stage 2 also clamps the value to KERNEL_MAX_SECTORS at run time, so a
#  corrupt field cannot make it read past LBA $(RESERVED_SECTORS) into the
#  filesystem.
# ---------------------------------------------------------------------------
S2HDR_MAGIC      := TOMATOS2
S2HDR_MAGIC_OFF  := 4
S2HDR_SECTORS_OFF := 12
# Where those two live in the finished image.
S2HDR_MAGIC_DISK_OFF   := $(shell expr $(STAGE2_LBA) \* 512 + $(S2HDR_MAGIC_OFF))
S2HDR_SECTORS_DISK_OFF := $(shell expr $(STAGE2_LBA) \* 512 + $(S2HDR_SECTORS_OFF))

# ---------------------------------------------------------------------------
#  >>> HOW THE USER PROGRAMS REACH STAGE 2 <<<
#  >>> the second half of the same contract; boot/layout.inc states it, <<<
#  >>> this file writes it, boot/stage2.asm reads it <<<
#
#  Same mechanism as the kernel's length above, for the same reason: stage 2
#  cannot look a file up, so the build writes down where the bytes are.
#
#  Why the programs are on the disk TWICE -- once as /BIN/*.ELF in the FAT
#  volume and once as raw sectors from LBA $(MODULES_LBA) -- is argued at
#  length in boot/layout.inc, and it is not redundancy. On the machine this
#  boot chain exists for, a ThinkPad T430, the kernel cannot read the stick it
#  booted from: its ATA driver speaks port I/O to IDE and SATA, and its USB
#  mass storage driver reaches the bus through UHCI, which Panther Point
#  replaced with EHCI and xHCI. /BIN would be unreachable and the shell would
#  come up with no commands at all. The BIOS has no such problem -- int 13h is
#  how stage 1 and stage 2 got there -- so stage 2 reads the programs while
#  int 13h is still available and hands them over as Multiboot modules, the
#  path the kernel already implements for GRUB and for "-kernel". /BIN stays
#  for the machines where the ATA driver does work.
#
#      stage2.bin +$(S2HDR_MODCOUNT_OFF) .. +19   dd, how many modules follow
#      stage2.bin +$(S2HDR_MODTAB_OFF) ..        $(MB_MODS_MAX) entries of $(S2MOD_ENTRY_SIZE) bytes:
#                              +0   dd   first LBA of this module
#                              +4   dd   its length in bytes
#                              +8   16   its name, NUL padded
#
#  The name is deliberately the SAME string the grub.cfg in the iso rule
#  writes: the bare lower case program name. The kernel takes the first word
#  of a module's command line as the program's name, so "hello" has to be
#  called "hello" whichever of the three boot paths brought it in -- otherwise
#  the set of commands the shell offers would depend on how the machine was
#  started, which is the kind of difference nobody thinks to look for.
#
#  Unlike the kernel's sector count there is no useful non-zero default here:
#  an unpatched stage2.bin must come up with NO modules, not with sixteen
#  garbage LBAs. That is why the $(STAGE2_BIN) rule insists the whole area is
#  zero in what nasm produced -- see the check there.
S2HDR_MODCOUNT_OFF := $(call layout_value,S2HDR_MODCOUNT_OFF,16)
S2HDR_MODTAB_OFF   := $(call layout_value,S2HDR_MODTAB_OFF,20)
S2MOD_ENTRY_SIZE   := $(call layout_value,S2MOD_ENTRY_SIZE,24)
S2MOD_NAME_MAX     := $(call layout_value,S2MOD_NAME_MAX,16)
MB_MODS_MAX        := $(call layout_value,MB_MODS_MAX,16)

S2MOD_TABLE_BYTES  := $(shell expr $(MB_MODS_MAX) \* $(S2MOD_ENTRY_SIZE))

# The count and the table together, as one span starting at the count. That
# is the region the build writes and the region stage 2 has to reserve, so it
# is also the region the "is it really reserved" check measures.
S2HDR_MODAREA_BYTES := $(shell expr $(S2HDR_MODTAB_OFF) + $(S2MOD_TABLE_BYTES) \
                                    - $(S2HDR_MODCOUNT_OFF))

# Where those two live in the finished image.
S2HDR_MODCOUNT_DISK_OFF := $(shell expr $(STAGE2_LBA) \* 512 + $(S2HDR_MODCOUNT_OFF))
S2HDR_MODTAB_DISK_OFF   := $(shell expr $(STAGE2_LBA) \* 512 + $(S2HDR_MODTAB_OFF))

# ---------------------------------------------------------------------------
#  How big stage 2 is allowed to get
#
#  Two ceilings, and the lower one wins:
#
#    1. the sectors layout.inc reserves,  STAGE2_SECTORS * 512
#    2. the distance from STAGE2_ORG up to DISK_BUFFER
#
#  The second one exists because stage 2 is loaded at STAGE2_ORG and then
#  reads disk sectors into DISK_BUFFER. If the buffer sits inside the span
#  stage 1 loaded, everything stage 2 has above it is overwritten by its own
#  first read -- and only once stage 2 has grown that far, which is a
#  spectacularly unpleasant way to find out. layout.inc has DISK_BUFFER at
#  0xA000, immediately above the 8 KiB at 0x8000, so today the two ceilings
#  coincide at 8192 and nothing is wasted. Both are read out of layout.inc
#  rather than written down, so moving either one here needs no edit.
#
#  stage2.asm asserts the same thing itself with a %error at s2_end. The
#  check in the $(STAGE2_BIN) rule is the second line of defence, so a build
#  still breaks with a readable message if that assertion is ever removed.
# ---------------------------------------------------------------------------
# Same as layout_value, but for the hex constants.
layout_hex = $(shell awk '$$1 == "$(1)" && $$2 == "equ" { print strtonum($$3); exit }' \
                     $(LAYOUT_INC) 2>/dev/null)

STAGE2_ORG         := $(or $(call layout_hex,STAGE2_ORG),32768)
DISK_BUFFER        := $(or $(call layout_hex,DISK_BUFFER),40960)

STAGE2_AREA_BYTES  := $(shell expr $(STAGE2_SECTORS) \* 512)
STAGE2_HEADROOM    := $(shell expr $(DISK_BUFFER) - $(STAGE2_ORG))
STAGE2_MAX_BYTES   := $(shell if [ $(STAGE2_HEADROOM) -lt $(STAGE2_AREA_BYTES) ]; \
                              then echo $(STAGE2_HEADROOM); \
                              else echo $(STAGE2_AREA_BYTES); fi)

# ---------------------------------------------------------------------------
#  The sectors the modules live in, and the memory stage 2 reads them into
#
#  Read out of boot/layout.inc for the same reason the kernel's numbers are:
#  the sector map has exactly one owner and it is not this file.
#
#  KERNEL_PHYS and MODULES_PHYS are here because the recipe for $(BOOTIMG)
#  turns the sentence layout.inc writes about them -- "chosen so it cannot
#  collide with the kernel by construction rather than by checking" -- into an
#  actual check. Construction is only as good as the next person's arithmetic.
MODULES_LBA         := $(call layout_value,MODULES_LBA,528)
MODULES_MAX_SECTORS := $(call layout_value,MODULES_MAX_SECTORS,512)
MODULES_MAX_BYTES   := $(shell expr $(MODULES_MAX_SECTORS) \* 512)

KERNEL_PHYS         := $(or $(call layout_hex,KERNEL_PHYS),1048576)
MODULES_PHYS        := $(or $(call layout_hex,MODULES_PHYS),1310720)

# Where the kernel's load image ends in physical memory if it ever grows to
# the largest size the sector map allows. Not where it ends today: the point
# of the check is that a kernel which is allowed to reach KERNEL_MAX_SECTORS
# must not be able to reach MODULES_PHYS, because stage 2 loads the modules
# before anything would notice the overlap and the symptom is a triple fault
# with an empty screen.
KERNEL_PHYS_END     := $(shell expr $(KERNEL_PHYS) + $(KERNEL_MAX_BYTES))

# ---------------------------------------------------------------------------
#  The MBR partition table
#
#  Offsets from the IBM PC boot sector layout: four 16 byte entries at 446,
#  then the 0x55AA signature at 510. Only the first entry is written; see the
#  long block in the $(BOOTIMG) recipe for what goes in it and why a partition
#  that starts at LBA 0 is the right answer on this image.
MBR_PART_OFF        := 446
MBR_PART_SIZE       := 16

# 0x0E, "W95 FAT16 (LBA)". See the recipe for why this and not 0x06.
MBR_PART_TYPE       := 14
MBR_PART_ACTIVE     := 128

# How the boot image is handed to QEMU: the same IDE 0 master the kernel's
# ata.c talks to, only now the BIOS boots from it as well. No -kernel, no
# -initrd, no -cdrom, no GRUB anywhere -- the whole point of the target.
# -boot c is the default already; it is spelled out so nothing in the
# neighbouring -boot d of run-iso can be mistaken for load bearing here.
QEMU_BOOTDISK := -drive file=$(notdir $(BOOTIMG)),format=raw,index=0,media=disk -boot c

# ---------------------------------------------------------------------------
#  Multiboot modules under "-kernel"
#
#  QEMU's own Multiboot loader takes modules through -initrd: a comma
#  separated list, and within one entry the first space separates the file
#  name from the command line. So this is a two module invocation with a
#  command line on each:
#
#      -initrd "hello a b,shell"
#
#  There is one wrinkle, and it decides how the rest of this block looks.
#  GRUB and QEMU do NOT agree on what a module's command line is:
#
#      grub.cfg   module /boot/hello.elf hello      ->  cmdline "hello"
#      qemu       -initrd "build/hello.elf hello"   ->  cmdline
#                                                       "build/hello.elf hello"
#
#  GRUB drops the file name and passes only what follows it; QEMU passes the
#  entry verbatim, path and all. Since the kernel takes the first word of the
#  command line as the module's name, the same program would be called
#  "hello" after booting the ISO and "build/hello.elf" after "make run".
#
#  The fix is to give QEMU an entry that is nothing but the name. QEMU
#  resolves it relative to its working directory, so the run targets start
#  QEMU inside $(BUILD_DIR) -- where every file it opens lives anyway -- and
#  $(USER_MODULES) provides, next to each <name>.elf, a symlink called plainly
#  <name> for it to open. Both boot paths then report the same module name.
QEMU_RUNDIR  := $(BUILD_DIR)
USER_MODULES := $(patsubst %,$(BUILD_DIR)/%,$(USER_PROGS))

# "hello shell" -> "hello,shell"; empty if there are no programs at all.
comma := ,
space := $(subst ,, )
QEMU_INITRD := $(if $(USER_PROGS), \
                   -initrd $(subst $(space),$(comma),$(strip $(USER_PROGS))))

# ---------------------------------------------------------------------------
#  Display via VNC
#
#  This QEMU is built without graphical display backends -- "-display help"
#  reports only "none", because qemu-ui-gtk and friends are separate packages
#  on Arch. The built-in VNC server (from qemu-common) is therefore the way to
#  get a picture. The run targets start it and attach a client automatically.
#
#  Binding to 127.0.0.1 is deliberate: otherwise QEMU listens on all
#  interfaces, and without a password at that.
#
#  Disable (e.g. when qemu-ui-gtk is installed):  make run VNC=0
#  Different client:   make run VNC_CLIENT=gvncviewer
#  Different display:  make run VNC_DISPLAY=3
VNC         ?= 1
VNC_DISPLAY ?= 1
VNC_HOST    := 127.0.0.1
VNC_PORT    := $(shell expr 5900 + $(VNC_DISPLAY))

# Pick the first client available from the list, unless one was given.
VNC_CLIENT ?= $(firstword $(foreach c,vncviewer gvncviewer vinagre xtightvncviewer krdc, \
                  $(shell command -v $(c) 2>/dev/null)))

# Invocation differs per client: most want "host:display", remmina a URL.
VNC_TARGET = $(if $(findstring remmina,$(VNC_CLIENT)), \
                 vnc://$(VNC_HOST):$(VNC_PORT), \
                 $(VNC_HOST):$(VNC_DISPLAY))

ifeq ($(VNC),1)
  QEMU_DISPLAY_FLAGS := -vnc $(VNC_HOST):$(VNC_DISPLAY)
else
  QEMU_DISPLAY_FLAGS :=
endif

# Starts QEMU and attaches the VNC client alongside it.
#
# QEMU stays in the foreground and therefore keeps stdio for the serial
# output; Ctrl-C quits as usual. The client waits in a subshell until the
# port is open, and is taken down when QEMU exits.
#
# QEMU itself is started in $(QEMU_RUNDIR), i.e. $(BUILD_DIR): everything it
# opens is in there, and the module names in -initrd have to be bare (see the
# Multiboot module block above). All file arguments in $(1) are therefore
# plain file names, not paths.
#   $(1) = QEMU arguments for the respective boot medium
define run_qemu
	$(call need,$(QEMU),qemu-system-x86)
	@if [ "$(VNC)" = "1" ] && [ -z "$(VNC_CLIENT)" ]; then \
		printf '\nERROR: no VNC client found.\n'; \
		printf '       Looked for: vncviewer gvncviewer vinagre xtightvncviewer krdc\n'; \
		printf '       Install one, e.g.:  sudo pacman -S tigervnc\n'; \
		printf '       Or name your own:   make $@ VNC_CLIENT=/path/to/client\n'; \
		printf '       Or run without VNC: make $@ VNC=0\n\n'; \
		exit 1; \
	fi
	@if [ "$(VNC)" = "1" ]; then \
		echo "  VNC     $(VNC_HOST):$(VNC_DISPLAY) (Port $(VNC_PORT)) -> $(notdir $(VNC_CLIENT))"; \
	fi
	@set -e; \
	 CLIENT_PID=""; \
	 if [ "$(VNC)" = "1" ]; then \
	   ( for i in $$(seq 1 100); do \
	       if ss -ltn 2>/dev/null | grep -q '127\.0\.0\.1:$(VNC_PORT) '; then \
	         exec $(VNC_CLIENT) $(VNC_TARGET); \
	       fi; \
	       sleep 0.1; \
	     done; \
	     echo "  VNC port $(VNC_PORT) never came up - client not started." >&2 \
	   ) >/dev/null 2>&1 & \
	   CLIENT_PID=$$!; \
	   trap 'kill $$CLIENT_PID 2>/dev/null; pkill -P $$CLIENT_PID 2>/dev/null; true' EXIT INT TERM; \
	 fi; \
	 cd $(QEMU_RUNDIR) && $(QEMU) $(1) -serial stdio $(QEMU_DISPLAY_FLAGS) $(QEMUFLAGS) \
	   || { rc=$$?; \
	        [ $$rc -eq 130 ] || [ $$rc -eq 143 ] || exit $$rc; }
endef

# ---------------------------------------------------------------------------
#  Sources / objects
#
#  Found rather than listed, at any depth: src/ is grouped into subsystems
#  (kernel, mm, fs, net, video, drivers/<bus>), and a new one needs no entry
#  here. The object tree mirrors the source tree -- src/net/ip.c becomes
#  build/net/ip.o -- so two files of the same name in different subsystems
#  cannot collide, which a flat object directory would let them do silently.
#
#  Sorted, so the link order is a property of the tree and not of the order
#  the filesystem happens to hand the names back.
#
#  include/ is excluded because it holds no translation units; it is left
#  flat on purpose. A header is an interface, and the single directory IS the
#  list of what the kernel offers -- see the comment above INC_DIR.
# ---------------------------------------------------------------------------
C_SRCS   := $(sort $(shell find $(SRC_DIR) -name '*.c'   -not -path '$(INC_DIR)/*'))
ASM_SRCS := $(sort $(shell find $(SRC_DIR) -name '*.asm' -not -path '$(INC_DIR)/*'))

C_OBJS   := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
ASM_OBJS := $(patsubst $(SRC_DIR)/%.asm,$(BUILD_DIR)/%.o,$(ASM_SRCS))

# start.o first: the Multiboot header must end up at the front of the image.
START_OBJ := $(BUILD_DIR)/kernel/start.o
OBJS := $(START_OBJ) $(filter-out $(START_OBJ),$(ASM_OBJS) $(C_OBJS))

DEPS := $(C_OBJS:.o=.d) $(USER_DEPS)

# ---------------------------------------------------------------------------
#  Helper: check that an external tool exists.
#  Evaluated inside recipes only, so the Makefile still parses (and "help"
#  still works) while the user is installing packages in parallel.
#    $(1) = binary, $(2) = Arch package hint
# ---------------------------------------------------------------------------
define need
	@command -v $(1) >/dev/null 2>&1 || { \
		printf '\nERROR: "%s" not found in PATH.\n' '$(1)'; \
		printf '       Install it, e.g.:  sudo pacman -S %s\n\n' '$(2)'; \
		exit 1; \
	}
endef

# ===========================================================================
#  Targets
# ===========================================================================
.PHONY: all user run iso run-iso floppy run-floppy disk bootdisk run-bootdisk \
        debug usb usb-boot clean help

all: $(KERNEL) user

# --- link ------------------------------------------------------------------
$(KERNEL): $(OBJS) $(LINKER_SCRIPT) | $(BUILD_DIR)
	@echo "  LD      $@"
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)
	@echo "  Kernel ready: $@"

# --- compile ---------------------------------------------------------------
#
# The user rule comes first deliberately. build/user/ls/ls.o matches the
# kernel pattern as well, with a stem of "user/ls/ls"; make only rejects it
# because src/user/ls/ls.c cannot be made, which is true today and is a thin
# thing to rely on. Order makes it not matter.
#
# Each rule creates its own output directory. The object tree mirrors the
# source tree, so the directories are not known until the wildcards have run,
# and an order-only prerequisite on a fixed list would have to be kept in
# step with the source layout by hand -- exactly what this restructuring was
# meant to stop.
$(USER_OBJ_DIR)/%.o: $(USER_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC/U    $<"
	$(CC) $(USER_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.asm
	$(call need,$(AS),nasm)
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# --- user space programs ---------------------------------------------------
user: $(USER_ELFS) $(USER_MODULES)

# The program plus the library, and nothing else on the link line: no kernel
# objects, no libgcc, no crt files. What comes out is a static ET_EXEC with a
# single PT_LOAD segment and not one relocation -- verify with
# "readelf -l" and "readelf -r".
#
# lib.o carries _start, so it has to be on the line even for a program that
# never calls anything else in it; ENTRY(_start) in user.ld would otherwise
# have no symbol to resolve.
# Two things here are chosen by the target's own name, which is what the
# secondary expansion is for: every object of the program's own directory,
# and USER_LIBS_<name>, the libraries only that program links.
.SECONDEXPANSION:
$(USER_ELFS): $(BUILD_DIR)/%.elf: $$(call user_objs,$$*) $(USER_LIB_OBJ) \
                                  $$(USER_LIBS_$$*) $(USER_LINKER_SCRIPT) | $(BUILD_DIR)
	@echo "  LD/U    $@"
	$(LD) $(USER_LDFLAGS) -o $@ $(call user_objs,$*) $(USER_LIB_OBJ) $(USER_LIBS_$*)

# The bare-name alias QEMU loads (see the Multiboot module block at the top).
# Relative symlink, so moving $(BUILD_DIR) does not break it.
$(USER_MODULES): $(BUILD_DIR)/%: $(BUILD_DIR)/%.elf
	@ln -sfn $(notdir $<) $@

# --- run directly (fastest test cycle) -------------------------------------
# QEMU understands Multiboot ELF kernels, so no bootloader is involved. It
# loads no modules on its own though -- that is what -initrd is for.
run: $(KERNEL) user $(DISK)
	$(call run_qemu,-kernel $(notdir $(KERNEL)) $(QEMU_INITRD) $(QEMU_DISK))

# --- bootable ISO ----------------------------------------------------------
iso: $(ISO)

$(ISO): $(KERNEL) $(USER_ELFS)
	$(call need,$(GRUB_MKRESCUE),grub)
	$(call need,xorriso,libisoburn)
	@echo "  ISO     $@"
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	@for p in $(USER_PROGS); do \
		echo "  MOD     /boot/$$p.elf  (module name: $$p)"; \
		cp $(BUILD_DIR)/$$p.elf $(ISO_DIR)/boot/$$p.elf; \
	done
	@# One "module" line per program. GRUB passes everything AFTER the file
	@# name as the module command line, so "/boot/hello.elf hello" arrives in
	@# the kernel as the command line "hello" -- and the kernel takes the
	@# first word of that as the module's name.
	@{ \
		echo 'set default=0'; \
		echo 'set timeout=3'; \
		echo ''; \
		echo 'menuentry "TomatOS" {'; \
		echo '    multiboot /boot/kernel.elf'; \
		for p in $(USER_PROGS); do \
			echo "    module /boot/$$p.elf $$p"; \
		done; \
		echo '    boot'; \
		echo '}'; \
	} > $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR) 2>/dev/null
	@echo "  ISO ready: $@"

# -boot d is NOT optional here, and the reason is worth spelling out.
#
# -cdrom and the hard disk sit on different IDE buses and do not conflict, but
# they do compete for the BIOS boot order, and the default order puts the hard
# disk first. The disk IS bootable as far as the BIOS can tell -- mformat
# writes a boot sector with a valid 0x55AA signature at LBA 0 -- so SeaBIOS
# prints "Booting from Hard Disk...", jumps into mformat's stub, and the
# machine sits there forever without ever looking at the CD.
#
# Verified by screendump: without -boot d the screen stops at "Booting from
# Hard Disk..."; with it, GRUB comes up and the kernel reaches its prompt.
run-iso: $(ISO) $(DISK)
	$(call run_qemu,-cdrom $(notdir $(ISO)) $(QEMU_DISK) -boot d)

# --- GRUB Legacy floppy image ----------------------------------------------
# The image in bin/ already contains stage1/stage2/menu.lst; menu.lst loads
# /kernel.bin. GRUB Legacy cannot boot a flat binary here -- it expects a
# Multiboot image -- so kernel.elf is copied in under the name kernel.bin.
floppy: $(FLOPPY)

$(FLOPPY): $(KERNEL) $(FLOPPY_TMPL) | $(BUILD_DIR)
	$(call need,$(MCOPY),mtools)
	@echo "  IMG     $@  (copy of $(FLOPPY_TMPL), original untouched)"
	@cp -f $(FLOPPY_TMPL) $@
	MTOOLS_SKIP_CHECK=1 $(MCOPY) -o -i $@ $(KERNEL) ::/kernel.bin
	@echo "  Floppy ready: $@"

run-floppy: $(FLOPPY)
	$(call run_qemu,-fda $(notdir $(FLOPPY)))

# --- FAT hard disk image ---------------------------------------------------
# Built entirely with mtools, so no root, no loop device and no mkfs: mformat
# writes the boot sector and both FATs into a plain file, mmd and mcopy fill
# it in from outside. See the geometry block near the top for why the numbers
# are what they are.
#
# THE FILESYSTEM IS NOT INSIDE A PARTITION, on either image. There is no
# hidden-sector offset (-H 0): the FAT boot sector sits at LBA 0, which is
# where fat_mount() looks for it. A partition that started anywhere else would
# mean the kernel had to parse a table before it could find the filesystem,
# and it does not.
#
# $(BOOTIMG) does carry an MBR partition table -- a BIOS will not always offer
# a USB stick without one -- but its single entry spans the whole volume from
# LBA 0, so it describes the same sectors the BPB does rather than displacing
# them. Both statements are true at once, which is unusual enough to be worth
# saying plainly: the boot sector is the MBR, the volume boot record and the
# BPB, all three. See the partition block in the $(BOOTIMG) recipe.
#
# Contents are laid out to give the shell something to walk:
#
#     /README.TXT      /MOTD.TXT        two files in the root
#     /BIN/*.ELF       the user programs, to be loaded from disk later
#     /DOCS/DISK.TXT   a file one level down, so a directory has to be
#                      traversed to reach it rather than just the root read
#
# Names are written in upper case 8.3 on purpose. mcopy would otherwise add
# VFAT long name entries, and the kernel skips those -- a program called
# "something-long" would then be invisible on disk. Keep USER_PROGS names to
# eight characters and this stays true.
disk: $(DISK)

# mformat a FAT16 volume into a plain file.
#   $(1) = image file, $(2) = number of reserved sectors
#
# $(DISK) passes 1 (the boot sector and nothing else). $(BOOTIMG) passes
# $(RESERVED_SECTORS), which is what keeps the filesystem out of the boot
# chain's sectors -- mformat then starts the first FAT above them and the area
# below is ours to write with dd. That count grew from 528 to 1040 when the
# user programs moved onto the disk as Multiboot modules; the constant is in
# boot/layout.inc and is read out of it, so this paragraph does not name it.
#
# Raising the reserved count does not endanger the FAT16 classification the
# geometry block above argues for. With 1040 reserved sectors, two FATs of 63
# sectors and a 32 sector root directory:
#   (65520 - 1040 - 2*63 - 32) / 4 = 16080 clusters
# instead of 16339. Both sit in the middle of the 4085..65524 band, and there
# is room for the reserved area to grow a great deal before that stops being
# true. Confirm with "minfo -i <image> ::" -- it prints the reserved count and
# the type.
define fat_format
	$(call need,$(MFORMAT),mtools)
	@rm -f $(1)
	$(MTOOLS_ENV) $(MFORMAT) -C -i $(1) \
		-t $(DISK_CYLS) -h $(DISK_HEADS) -s $(DISK_SECS) \
		-c $(DISK_CLUSTER) -H 0 -m $(DISK_MEDIA) -R $(2) -v $(DISK_LABEL) ::
endef

# ---------------------------------------------------------------------------
#  The staging tree: what goes on the image, assembled as a directory
#
#  The image used to be filled by a recipe -- an mmd for the directories, a
#  loop for the programs, and three text files whose wording lived inside a
#  shell block in this Makefile. That worked, and it had three costs: the
#  contents of the disk could not be looked at without reading a Makefile,
#  adding a file meant editing a recipe, and the directory structure was
#  hard coded in an mmd line that had to be kept in step with the copies
#  below it.
#
#  So the image is now built from a directory instead. $(STAGING_DIR) is the
#  disk, laid out exactly as it will appear, and filling the volume is one
#  recursive copy. The structure comes from the tree: a new subdirectory in
#  $(SYSROOT_DIR) needs no mmd here, because mtools creates what it finds.
#
#  It is assembled rather than checked in, because two different kinds of
#  thing end up on the disk:
#
#    - $(SYSROOT_DIR), in the source tree, holds what a human wrote. It is
#      versioned, and a change to it shows up in a diff as a change to the
#      file rather than as a change to a shell quoting escape.
#    - The programs are build outputs. They are compiled into $(BUILD_DIR),
#      and copying them here under their 8.3 names is the only place the
#      lower case source name and the upper case disk name meet.
#
#  DOCS/DISK.TXT stays GENERATED, which is the one deliberate exception. It
#  quotes the geometry the image was actually built with, so it is checkable
#  against the volume it sits on -- a static copy of those numbers would be a
#  second place to change them and a first place for them to go stale.
STAGING_DIR   := $(BUILD_DIR)/staging
STAGING_STAMP := $(BUILD_DIR)/staging.stamp

# Every file, at any depth. Listed rather than globbed one level deep so that
# editing sysroot/DOCS/whatever rebuilds the image.
SYSROOT_FILES := $(shell find $(SYSROOT_DIR) -type f 2>/dev/null)

# THE DIRECTORIES ARE PREREQUISITES TOO, and that is not redundant with the
# files above. This list is taken once, when make parses this file, so a
# DELETED file is not in it -- there is nothing left to be newer than the
# stamp, make finds nothing to do, and the image keeps a file the source tree
# no longer has. That is the worst of the three cases: an added file that is
# missing gets noticed, a changed file that is stale gets noticed, and a
# deleted file that is still there looks exactly like a file that is supposed
# to be there.
#
# A directory's timestamp moves when an entry is added to it or removed from
# it, which is precisely the event the file list cannot see. Removing a whole
# subtree touches its parent, so that is covered as well.
SYSROOT_DIRS := $(shell find $(SYSROOT_DIR) -type d 2>/dev/null)

# Makefile is a prerequisite because DISK.TXT below is generated from it.
$(STAGING_STAMP): $(USER_ELFS) $(SYSROOT_FILES) $(SYSROOT_DIRS) Makefile | $(BUILD_DIR)
	@echo "  STAGE   $(STAGING_DIR)"
	@rm -rf $(STAGING_DIR)
	@mkdir -p $(STAGING_DIR)/BIN $(STAGING_DIR)/DOCS
	@cp -R $(SYSROOT_DIR)/. $(STAGING_DIR)/
	@for p in $(USER_PROGS); do \
		u=`echo $$p | tr 'a-z' 'A-Z'`; \
		cp $(BUILD_DIR)/$$p.elf $(STAGING_DIR)/BIN/$$u.ELF; \
	done
	@{ \
		echo 'Disk geometry'; \
		echo '-------------'; \
		echo '  cylinders          $(DISK_CYLS)'; \
		echo '  heads              $(DISK_HEADS)'; \
		echo '  sectors per track  $(DISK_SECS)'; \
		echo '  total sectors      $(DISK_TOTAL_SECS)'; \
		echo '  sectors/cluster    $(DISK_CLUSTER)'; \
		echo ''; \
		echo 'The cluster count puts this in the middle of the FAT16'; \
		echo 'range. The filesystem is not inside a partition: its'; \
		echo 'boot sector is at LBA 0, which is where the driver'; \
		echo 'looks. The bootable image carries an MBR table there'; \
		echo 'as well, spanning the whole volume, because a BIOS'; \
		echo 'will not always offer a stick without one.'; \
		echo ''; \
		echo 'If you can read this, directory traversal works.'; \
	} > $(STAGING_DIR)/DOCS/DISK.TXT
	@touch $@

# Fill a formatted volume with the staging tree. Shared verbatim by $(DISK)
# and $(BOOTIMG) so the two images differ only in their boot chain, never in
# their contents.
#
# -s makes mcopy descend, which is what turns the tree into the recipe. The
# shell expands the wildcard to the top level entries, files and directories
# alike; everything below them is mtools' business.
#   $(1) = image file
define fat_populate
	@echo "  DISK    $(STAGING_DIR) -> $(1)"
	@$(MTOOLS_ENV) $(MCOPY) -s -o -i $(1) $(STAGING_DIR)/* ::
endef

# The staging tree is the prerequisite rather than the programs: it already
# depends on them, on sysroot/ and on this file, so anything that changes what
# lands on the disk arrives through it. Makefile stays listed for the geometry
# in the format line below, which is not part of the tree.
$(DISK): $(STAGING_STAMP) Makefile | $(BUILD_DIR)
	@echo "  IMG     $@  ($(DISK_SIZE_MB) MB, C/H/S $(DISK_CYLS)/$(DISK_HEADS)/$(DISK_SECS), FAT16)"
	$(call fat_format,$@,1)
	$(call fat_populate,$@)
	@echo "  Disk ready: $@"

# ===========================================================================
#  The own boot chain
# ===========================================================================

# --- stage 1 ---------------------------------------------------------------
# 512 bytes exactly, ending in 0x55AA. Not "at most 512": stage 1 IS the boot
# sector, so a short binary would mean the signature is missing and a long
# one that nasm has silently produced something no BIOS will ever load.
$(STAGE1_BIN): $(STAGE1_SRC) $(BOOT_INCS) | $(BUILD_DIR)
	$(call need,$(AS),nasm)
	@echo "  AS/B    $< -> $@"
	$(AS) -f bin -I $(BOOT_DIR) $< -o $@
	@sz=`stat -c%s $@`; \
	 if [ "$$sz" != "512" ]; then \
		printf '\nERROR: %s is %s bytes, must be exactly 512.\n' '$@' "$$sz"; \
		printf '       Stage 1 is the boot sector. Pad it with\n'; \
		printf '           times 510-($$-$$$$) db 0\n'; \
		printf '           dw 0xAA55\n'; \
		printf '       and make sure nothing follows.\n\n'; \
		rm -f $@; exit 1; \
	 fi; \
	 sig=`od -An -tx1 -j 510 -N 2 $@ | tr -d ' \n'`; \
	 if [ "$$sig" != "55aa" ]; then \
		printf '\nERROR: %s does not end in the 0x55AA boot signature (found %s).\n\n' '$@' "$$sig"; \
		rm -f $@; exit 1; \
	 fi

# --- stage 2 ---------------------------------------------------------------
# Size capped (see the STAGE2_MAX_BYTES block at the top), and the patchable
# header checked: the "TOMATOS2" magic at offset 4 has to be there, because
# the boot image rule is about to overwrite offset 12..15 with the kernel's
# sector count and has no other way of knowing it is aiming at the right
# bytes.
$(STAGE2_BIN): $(STAGE2_SRC) $(BOOT_INCS) | $(BUILD_DIR)
	$(call need,$(AS),nasm)
	@echo "  AS/B    $< -> $@"
	$(AS) -f bin -I $(BOOT_DIR) $< -o $@
	@sz=`stat -c%s $@`; \
	 if [ "$$sz" -gt "$(STAGE2_MAX_BYTES)" ]; then \
		printf '\nERROR: stage 2 is %s bytes, the limit is %s.\n' "$$sz" '$(STAGE2_MAX_BYTES)'; \
		printf '       boot/layout.inc reserves %s sectors (%s bytes) at LBA %s,\n' \
		       '$(STAGE2_SECTORS)' '$(STAGE2_AREA_BYTES)' '$(STAGE2_LBA)'; \
		printf '       and leaves %s bytes between STAGE2_ORG and DISK_BUFFER.\n' \
		       '$(STAGE2_HEADROOM)'; \
		printf '       The smaller of the two is the limit. Refusing to build an\n'; \
		printf '       image that would be truncated on disk or overwritten in RAM.\n\n'; \
		rm -f $@; exit 1; \
	 fi; \
	 magic=`dd if=$@ bs=1 skip=$(S2HDR_MAGIC_OFF) count=8 status=none`; \
	 if [ "$$magic" != '$(S2HDR_MAGIC)' ]; then \
		printf '\nERROR: no "%s" magic at offset %s of %s (found "%s").\n' \
		       '$(S2HDR_MAGIC)' '$(S2HDR_MAGIC_OFF)' '$@' "$$magic"; \
		printf '       The kernel size is patched into offset %s of this file,\n' \
		       '$(S2HDR_SECTORS_OFF)'; \
		printf '       and the magic is what proves those bytes are the header\n'; \
		printf '       and not instructions. Restore the s2_magic / \n'; \
		printf '       s2_kernel_sectors block at the top of %s.\n\n' '$<'; \
		rm -f $@; exit 1; \
	 fi; \
	 if [ "$$sz" -lt "$(shell expr $(S2HDR_MODCOUNT_OFF) + $(S2HDR_MODAREA_BYTES))" ]; then \
		printf '\nERROR: %s is %s bytes, too short to hold the module table.\n' '$@' "$$sz"; \
		printf '       The table alone needs offsets %s..%s.\n\n' \
		       '$(S2HDR_MODCOUNT_OFF)' \
		       "`expr $(S2HDR_MODCOUNT_OFF) + $(S2HDR_MODAREA_BYTES) - 1`"; \
		rm -f $@; exit 1; \
	 fi; \
	 area=`od -An -tx1 -j $(S2HDR_MODCOUNT_OFF) -N $(S2HDR_MODAREA_BYTES) -v $@ \
	       | tr -d ' \n'`; \
	 if [ "$$area" != "`printf '%0*d' $$(( $(S2HDR_MODAREA_BYTES) * 2 )) 0`" ]; then \
		printf '\nERROR: offsets %s..%s of %s are not reserved.\n' \
		       '$(S2HDR_MODCOUNT_OFF)' \
		       "`expr $(S2HDR_MODCOUNT_OFF) + $(S2HDR_MODAREA_BYTES) - 1`" '$@'; \
		printf '       The boot image rule is about to write a dword module count\n'; \
		printf '       at %s and %s entries of %s bytes at %s over them. Those bytes\n' \
		       '$(S2HDR_MODCOUNT_OFF)' '$(MB_MODS_MAX)' '$(S2MOD_ENTRY_SIZE)' \
		       '$(S2HDR_MODTAB_OFF)'; \
		printf '       are non-zero, which means they are still code or data that\n'; \
		printf '       stage 2 needs -- patching them would replace instructions.\n'; \
		printf '       boot/layout.inc defines the table; %s has to\n' '$<'; \
		printf '       reserve it right behind s2_kernel_sectors, zero filled:\n'; \
		printf '           s2_mod_count: dd 0\n'; \
		printf '           s2_mod_table: times MB_MODS_MAX * S2MOD_ENTRY_SIZE db 0\n'; \
		printf '       Zero and not a default: unlike the kernel sector count, an\n'; \
		printf '       unpatched stage 2 must come up with NO modules rather than\n'; \
		printf '       with sixteen garbage LBAs.\n\n'; \
		rm -f $@; exit 1; \
	 fi; \
	 echo "  Stage 2: $$sz / $(STAGE2_MAX_BYTES) bytes used, header magic OK," \
	      "$(S2HDR_MODAREA_BYTES) bytes reserved for $(MB_MODS_MAX) modules"

# --- the kernel as a flat image --------------------------------------------
# objcopy -O binary walks the ALLOC sections and places each one at
#   (section LMA - lowest LMA)
# in the output. That is exactly what stage 2 needs: it can read the file
# into KERNEL_PHYS (0x00100000) sector by sector and be done, with no ELF
# parsing in real mode.
#
# The higher-half link makes that worth verifying rather than assuming. Every
# section has a VMA of 0xC01xxxxx and an LMA of 0x001xxxxx; if objcopy went
# by VMA the image would be identical anyway, because the two differ by the
# same constant everywhere. The check below is therefore not "did it use the
# LMA" -- it is the stronger and more useful "does the file that comes out
# match the program headers", i.e.
#
#   1. the flat size equals (highest LMA + FileSiz) - (lowest LMA), so no
#      section was dropped and no gap was mis-sized, and
#   2. the first LOAD segment's bytes appear at offset 0 of the flat image,
#      read straight out of the ELF at its own file offset. .rodata makes
#      this a real test: its ELF file offset is 0x10000 but its address puts
#      it at 0x0F000 in the flat image, so a plain "cat of the ELF" or an
#      off-by-a-section layout could not pass.
#
# And the Multiboot header: it must lie within the first 8 KiB of the image,
# 4 byte aligned. It is NOT at offset 0 -- src/kernel/start.asm puts the entry point
# first and the header just after it, currently at 0x5C -- and that is the
# right way round here, because stage 2 enters the image at its first byte.
# The rule prints where it found the magic so a reshuffle of start.asm cannot
# quietly push it past 8 KiB and break the GRUB path instead.
$(KERNEL_BIN): $(KERNEL) | $(BUILD_DIR)
	@echo "  OBJCOPY $< -> $@  (flat, no ELF headers)"
	$(OBJCOPY) -O binary $< $@
	@set -e; \
	 lo=; hi=0; foff=; fsz=; \
	 for e in `readelf -lW $< | awk '/^  LOAD/ { print $$2 ":" $$4 ":" $$5 }'`; do \
	   o=$$(( $${e%%:*} )); r=$${e#*:}; l=$$(( $${r%%:*} )); f=$$(( $${r##*:} )); \
	   if [ -z "$$lo" ] || [ $$l -lt $$lo ]; then lo=$$l; foff=$$o; fsz=$$f; fi; \
	   if [ $$((l + f)) -gt $$hi ]; then hi=$$((l + f)); fi; \
	 done; \
	 span=$$((hi - lo)); actual=`stat -c%s $@`; \
	 if [ "$$span" != "$$actual" ]; then \
	   printf '\nERROR: flat image is %s bytes, the load addresses span %s.\n' "$$actual" "$$span"; \
	   printf '       objcopy did not lay the image out the way the program\n'; \
	   printf '       headers describe it. Check linker.ld and readelf -l.\n\n'; \
	   rm -f $@; exit 1; \
	 fi; \
	 dd if=$@ bs=1 count=$$fsz status=none > $@.head; \
	 if ! dd if=$< bs=1 skip=$$foff count=$$fsz status=none | cmp -s - $@.head; then \
	   printf '\nERROR: the first LOAD segment is not at offset 0 of %s.\n\n' '$@'; \
	   rm -f $@ $@.head; exit 1; \
	 fi; \
	 rm -f $@.head; \
	 mb=`od -Ad -tx4 -N 8192 -v $@ | \
	     awk '{ for (i = 2; i <= NF; i++) \
	              if ($$i == "1badb002") { print $$1 + (i - 2) * 4; exit } }'`; \
	 if [ -z "$$mb" ]; then \
	   printf '\nERROR: no Multiboot header (0x1BADB002) in the first 8 KiB of %s.\n' '$@'; \
	   printf '       The GRUB / -kernel paths would stop booting. Check that\n'; \
	   printf '       src/kernel/start.asm still carries it and that linker.ld keeps\n'; \
	   printf '       start.o at the front of .text.\n\n'; \
	   rm -f $@; exit 1; \
	 fi; \
	 secs=$$(( (actual + 511) / 512 )); \
	 if [ "$$secs" -gt "$(KERNEL_MAX_SECTORS)" ]; then \
	   printf '\nERROR: kernel is %s sectors (%s bytes), the limit is %s.\n' \
	          "$$secs" "$$actual" '$(KERNEL_MAX_SECTORS)'; \
	   printf '       boot/layout.inc gives the kernel LBA %s..%s. Growing past\n' \
	          '$(KERNEL_LBA)' "`expr $(KERNEL_LBA) + $(KERNEL_MAX_SECTORS) - 1`"; \
	   printf '       that would write into the FAT16 volume at LBA %s.\n' '$(RESERVED_SECTORS)'; \
	   printf '       Raise KERNEL_MAX_SECTORS and RESERVED_SECTORS together,\n'; \
	   printf '       or shrink the kernel.\n\n'; \
	   rm -f $@; exit 1; \
	 fi; \
	 echo "  Kernel:  $$actual bytes = $$secs / $(KERNEL_MAX_SECTORS) sectors," \
	      "Multiboot header at offset $$mb"

# --- the bootable image ----------------------------------------------------
# Everything the disk image is, plus the boot chain in the reserved sectors.
#
# THE BOOT SECTOR IS WRITTEN IN TWO PIECES, and that is the whole trick:
# mformat has just put a BPB there and the kernel's own fat_mount() reads it
# back after boot, so overwriting all 512 bytes with stage 1 would produce a
# disk that boots and then cannot find its filesystem.
#
#   bytes 0x00..0x02   the jump over the BPB          -> from stage 1
#   bytes 0x03..0x3D   OEM name and BPB               -> mformat's, untouched
#   bytes 0x3E..0x1FF  code and the 0x55AA signature  -> from stage 1
#
# So: 3 bytes at offset 0, then 450 bytes at offset 62, both conv=notrunc.
# Stage 1 must reserve 0x03..0x3D itself (a "times 90-($$-$$$$) db 0" after
# its jump, or an assembled dummy BPB) -- whatever it puts there is simply
# not copied.
bootdisk: $(BOOTIMG)

$(BOOTIMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(STAGING_STAMP) Makefile | $(BUILD_DIR)
	@echo "  IMG     $@  ($(DISK_SIZE_MB) MB, FAT16, $(RESERVED_SECTORS) reserved sectors)"
	$(call fat_format,$@,$(RESERVED_SECTORS))
	$(call fat_populate,$@)
	@echo "  BOOT    LBA 0        stage 1  (jump + code, BPB preserved)"
	@dd if=$(STAGE1_BIN) of=$@ bs=1 count=3 conv=notrunc status=none
	@dd if=$(STAGE1_BIN) of=$@ bs=1 skip=62 seek=62 count=450 conv=notrunc status=none
	@# ---------------------------------------------------------------------
	@#  The MBR partition table
	@#
	@#  WHY IT EXISTS. Without it this image is a "superfloppy": a filesystem
	@#  at LBA 0 and bytes 446..509 all zero. SeaBIOS boots that from USB
	@#  quite happily, so no amount of testing in QEMU says anything about the
	@#  case that matters. Many real BIOSes will only offer a USB stick in the
	@#  boot menu at all when it carries a partition table with an active
	@#  partition -- that table is what tells them to treat the stick as a
	@#  hard disk rather than as a floppy or as nothing. The ThinkPad T430
	@#  this whole boot chain exists for is a likely case, and the failure it
	@#  produces is a stick that simply never appears in the menu, with no
	@#  message to search for.
	@#
	@#  WHY IT IS WRITTEN AFTER STAGE 1 AND NOT CARVED OUT OF IT. The dd above
	@#  covers bytes 62..511, so on its way past it stamps zeros over 446..509
	@#  -- stage 1's own image has nothing there. The alternative is to stop
	@#  that dd short at 446 (count=384) and leave the area to mformat, which
	@#  then needs a third dd for the 0x55AA at 510..511 that stage 1 carries.
	@#  Letting it run and writing the entry afterwards is one dd instead of
	@#  two, keeps the boot signature coming from stage 1 where the check at
	@#  the end of this recipe expects it, and has a useful side effect: the
	@#  three unused entries are provably zero rather than merely believed to
	@#  be, because they were overwritten a line ago.
	@#
	@#  Stage 1 neither moves nor shrinks for this. Its code ends at offset
	@#  314 and the table starts at 446, so 131 bytes sit unused in between;
	@#  the boot sector is not the tight thing on this image.
	@#
	@#  THE AWKWARD PART, said plainly: the FAT boot sector IS the MBR here,
	@#  so the partition has to start at LBA 0 and therefore contains the very
	@#  sector that describes it. That is not how anyone lays out a disk on
	@#  purpose -- a first partition normally starts at 2048 -- and fdisk will
	@#  point out the overlap.
	@#
	@#  It has one measured consequence worth knowing before someone reports
	@#  it as a bug. "fdisk -l" and "sfdisk --dump" read the table and print
	@#  it; "partx --show" and "blkid -p -s PTTYPE" report no partition table
	@#  at all. That is NOT the LBA 0 start -- moving the entry to LBA 1 does
	@#  not help, and defacing the BPB while leaving the entry at LBA 0 makes
	@#  partx list it immediately. libblkid simply refuses to look for a
	@#  partition table on anything whose first sector is a filesystem, which
	@#  this image is by design. Nothing in the boot path consults libblkid:
	@#  the BIOS reads these sixteen bytes itself, and the kernel reads the
	@#  BPB. The alternative layout above would fix the cosmetics and nothing
	@#  else.
	@#
	@#  It is still the right trade, because the alternative is a real MBR at
	@#  LBA 0 that chainloads a volume boot record at 2048, and that costs:
	@#
	@#    - stage 1 splits in two. An MBR half that walks the table for the
	@#      active entry and loads its first sector, and a VBR half that is
	@#      what stage 1 is today. Two boot sectors to keep in step.
	@#    - every LBA in boot/layout.inc becomes partition relative, and both
	@#      stage 1 and stage 2 have to add the partition base to every read.
	@#      The numbers stop being checkable with dd and xxd from outside.
	@#    - the kernel's fat_mount() reads the BPB at LBA 0. It would have to
	@#      parse a partition table first, which is a FAT driver learning
	@#      about partitioning for the benefit of nothing but the boot.
	@#    - mformat's -H 0 becomes -H 2048, and the BPB hidden-sectors field
	@#      starts to matter to code that today does not read it.
	@#
	@#  Four moving parts, three of them in assembly, to buy nothing the BIOS
	@#  looks at: it reads this table for the active flag and the geometry,
	@#  then loads LBA 0 and jumps, exactly as it does now.
	@#
	@#  TYPE 0x0E, "W95 FAT16 (LBA)", not 0x06. Stage 1 already requires the
	@#  int 13h extensions -- it calls AH=41h and refuses to go on without
	@#  them, then reads with AH=42h -- so a table advertising the CHS-only
	@#  0x06 would be describing a disk this boot chain cannot boot from
	@#  anyway. 0x0E is also what a current fdisk writes for a FAT16 stick,
	@#  which matters when the user checks our table against a known good one.
	@#
	@#  CHS FILLED IN HONESTLY, not with the 0xFE 0xFF 0xFF "look it up via
	@#  LBA" filler. The geometry block near the top of this file already
	@#  constrains the image to heads <= 16 and sectors <= 63 so that CHS
	@#  stays legal for a BIOS, so the true values exist and are exact. The
	@#  filler is for volumes CHS cannot express; using it here would throw
	@#  away information a CHS-only BIOS could still act on, in exchange for
	@#  nothing. The check below is what keeps that claim true.
	@set -e; \
	 last=`expr $(DISK_TOTAL_SECS) - 1`; \
	 track=`expr $(DISK_HEADS) \* $(DISK_SECS)`; \
	 cyl=`expr $$last / $$track`; rem=`expr $$last % $$track`; \
	 hd=`expr $$rem / $(DISK_SECS)`; sc=`expr $$rem % $(DISK_SECS) + 1`; \
	 if [ "$$cyl" -gt 1023 ]; then \
		printf '\nERROR: LBA %s is cylinder %s, and CHS stops at 1023.\n' \
		       "$$last" "$$cyl"; \
		printf '       The geometry C/H/S %s/%s/%s no longer fits a partition\n' \
		       '$(DISK_CYLS)' '$(DISK_HEADS)' '$(DISK_SECS)'; \
		printf '       entry. Either shrink the image or write the conventional\n'; \
		printf '       0xFE 0xFF 0xFF filler into both CHS fields instead, which\n'; \
		printf '       tells a BIOS to go by the LBA fields alone.\n\n'; \
		exit 1; \
	 fi; \
	 printf '%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x' \
	        $(MBR_PART_ACTIVE) 0 1 0 \
	        $(MBR_PART_TYPE) $$hd $$(( sc | ((cyl >> 8) << 6) )) $$(( cyl & 255 )) \
	        0 0 0 0 \
	        $$(( $(DISK_TOTAL_SECS) & 255 )) $$(( $(DISK_TOTAL_SECS) >> 8 & 255 )) \
	        $$(( $(DISK_TOTAL_SECS) >> 16 & 255 )) $$(( $(DISK_TOTAL_SECS) >> 24 & 255 )) \
	   | xxd -r -p > $@.mbr; \
	 dd if=$@.mbr of=$@ bs=1 seek=$(MBR_PART_OFF) conv=notrunc status=none; \
	 rm -f $@.mbr; \
	 got=`od -An -tu1 -j $(MBR_PART_OFF) -N 5 -v $@ | tr -s ' ' | sed 's/^ //'`; \
	 [ "$$got" = "$(MBR_PART_ACTIVE) 0 1 0 $(MBR_PART_TYPE)" ] || { \
		printf '\nERROR: partition entry reads back as "%s", expected "%s".\n\n' \
		       "$$got" '$(MBR_PART_ACTIVE) 0 1 0 $(MBR_PART_TYPE)'; \
		exit 1; \
	 }; \
	 echo "  BOOT    off $(MBR_PART_OFF)  partition 1  active, type 0x0e," \
	      "LBA 0 + $(DISK_TOTAL_SECS), CHS 0/0/1 - $$cyl/$$hd/$$sc"
	@echo "  BOOT    LBA $(STAGE2_LBA)        stage 2  (`stat -c%s $(STAGE2_BIN)` bytes)"
	@dd if=$(STAGE2_BIN) of=$@ bs=512 seek=$(STAGE2_LBA) conv=notrunc status=none
	@# Patch the kernel's length into stage 2's header ON THE DISK. See the
	@# ">>> HOW THE KERNEL'S SIZE REACHES STAGE 2 <<<" block at the top.
	@# Check the magic in the image first -- offset 12 is being overwritten
	@# and there must be no doubt about what is there.
	@magic=`dd if=$@ bs=1 skip=$(S2HDR_MAGIC_DISK_OFF) count=8 status=none`; \
	 [ "$$magic" = '$(S2HDR_MAGIC)' ] || { \
		printf '\nERROR: no "%s" magic at image offset %s.\n\n' \
		       '$(S2HDR_MAGIC)' '$(S2HDR_MAGIC_DISK_OFF)'; \
		exit 1; \
	 }; \
	 secs=$$(( (`stat -c%s $(KERNEL_BIN)` + 511) / 512 )); \
	 printf '%02x%02x%02x%02x' \
	        $$((secs & 255)) $$((secs >> 8 & 255)) \
	        $$((secs >> 16 & 255)) $$((secs >> 24 & 255)) \
	   | xxd -r -p > $@.hdr; \
	 dd if=$@.hdr of=$@ bs=1 seek=$(S2HDR_SECTORS_DISK_OFF) conv=notrunc status=none; \
	 rm -f $@.hdr; \
	 got=`od -An -tu4 -j $(S2HDR_SECTORS_DISK_OFF) -N 4 $@ | tr -d ' \n'`; \
	 [ "$$got" = "$$secs" ] || { \
		printf '\nERROR: read back %s sectors from image offset %s, wrote %s.\n\n' \
		       "$$got" '$(S2HDR_SECTORS_DISK_OFF)' "$$secs"; \
		exit 1; \
	 }; \
	 echo "  BOOT    off $(S2HDR_SECTORS_DISK_OFF)   s2_kernel_sectors = $$secs" \
	      "(stage2.bin itself keeps its $(KERNEL_MAX_SECTORS) default)"
	@echo "  BOOT    LBA $(KERNEL_LBA)       kernel   (`stat -c%s $(KERNEL_BIN)` bytes, flat)"
	@dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_LBA) conv=notrunc status=none
	@# ---------------------------------------------------------------------
	@#  Two things boot/layout.inc argues are true by construction, checked
	@#  here so that a future edit to the layout trips over them.
	@#
	@#  The first one is asked of the LINKED KERNEL rather than of the sector
	@#  budget, and that distinction is the whole value of the check. The
	@#  obvious form -- KERNEL_PHYS plus KERNEL_MAX_SECTORS times 512 -- asks
	@#  how long the IMAGE can be, and the image is not the kernel: .bss is
	@#  NOBITS, it occupies no bytes on disk and every one of them in RAM.
	@#  That version of this test passed while the modules sat inside .bss,
	@#  and the kernel wiped five of seven of them clearing it. So the number
	@#  comes out of the ELF's program headers, where memsz is the field that
	@#  knows the difference.
	@#
	@#  The failure mode this prevents is the worst kind on the machine this
	@#  is aimed at. In QEMU it hides: the shell finds the same programs on
	@#  the FAT volume and runs them, and everything looks right. On a laptop
	@#  that cannot read its own stick it is a shell with no commands.
	@kend=`readelf -lW $(KERNEL) | awk '$$1=="LOAD"{print $$4"+"$$6}' | \
	       { m=0; while read p; do e=$$(( $$p )); [ $$e -gt $$m ] && m=$$e; done; echo $$m; }`; \
	 if [ "$$kend" -le 0 ]; then \
		printf '\nERROR: could not read the load segments of %s.\n' '$(KERNEL)'; \
		printf '       This check is what keeps the modules out of the kernel\n'; \
		printf '       .bss, so it refuses rather than assuming they are clear.\n\n'; \
		exit 1; \
	 fi; \
	 if [ "$$kend" -gt $(MODULES_PHYS) ]; then \
		printf '\nERROR: the kernel and the modules overlap in memory.\n'; \
		printf '       %s reaches physical %s (0x%X) once .bss is counted,\n' \
		       '$(KERNEL)' "$$kend" "$$kend"; \
		printf '       but MODULES_PHYS is %s.\n' '$(MODULES_PHYS)'; \
		printf '       Raise MODULES_PHYS in boot/layout.inc above that, on a\n'; \
		printf '       page boundary. Note this is the LINKED size including\n'; \
		printf '       .bss, not the sector budget -- do not compare it against\n'; \
		printf '       KERNEL_MAX_SECTORS.\n\n'; \
		exit 1; \
	 fi
	@# The same statement about the disk rather than about memory: the module
	@# region has to end at or before the first sector of the FAT16 volume,
	@# because mformat has already written a filesystem there and dd would not
	@# notice it was overwriting one.
	@if [ `expr $(MODULES_LBA) + $(MODULES_MAX_SECTORS)` -gt $(RESERVED_SECTORS) ]; then \
		printf '\nERROR: the module region runs into the filesystem.\n'; \
		printf '       LBA %s plus %s sectors ends at %s, and the FAT16 volume\n' \
		       '$(MODULES_LBA)' '$(MODULES_MAX_SECTORS)' \
		       "`expr $(MODULES_LBA) + $(MODULES_MAX_SECTORS)`"; \
		printf '       starts at LBA %s.\n' '$(RESERVED_SECTORS)'; \
		printf '       Raise RESERVED_SECTORS in boot/layout.inc to match.\n\n'; \
		exit 1; \
	 fi
	@# ---------------------------------------------------------------------
	@#  The user programs, as raw sectors, plus the table in stage 2's header
	@#  that finds them. See ">>> HOW THE USER PROGRAMS REACH STAGE 2 <<<" at
	@#  the top of this file for why they are on the disk a second time.
	@#
	@#  Counted and measured before anything is written, because both of the
	@#  ways this can go wrong go wrong LATE. A program past MB_MODS_MAX would
	@#  simply not be in the table; a region past MODULES_MAX_SECTORS would be
	@#  written straight through the filesystem. Either way the first symptom
	@#  is exec_init() rejecting an ELF whose header turned out to be somebody
	@#  else's data, one boot and several layers away from the cause.
	@set -e; \
	 n=0; secs=0; \
	 for p in $(USER_PROGS); do \
	   sz=`stat -c%s $(BUILD_DIR)/$$p.elf`; \
	   n=$$((n + 1)); secs=$$((secs + (sz + 511) / 512)); \
	   if [ $${#p} -ge $(S2MOD_NAME_MAX) ]; then \
	     printf '\nERROR: module name "%s" is %s characters.\n' "$$p" "$${#p}"; \
	     printf '       The name field is %s bytes and the kernel reads it as a\n' \
	            '$(S2MOD_NAME_MAX)'; \
	     printf '       command line, so it must end in a NUL: %s is the limit.\n\n' \
	            "`expr $(S2MOD_NAME_MAX) - 1`"; \
	     exit 1; \
	   fi; \
	 done; \
	 if [ $$n -gt $(MB_MODS_MAX) ]; then \
	   printf '\nERROR: %s programs in USER_PROGS, but the table holds %s.\n' \
	          "$$n" '$(MB_MODS_MAX)'; \
	   printf '       Truncating the list here would put a module on the disk\n'; \
	   printf '       that nothing points at, and leave the shell missing a\n'; \
	   printf '       command for reasons visible nowhere. Raise MB_MODS_MAX in\n'; \
	   printf '       boot/layout.inc (MB_MODS at 0x7500 has room until 0x7600,\n'; \
	   printf '       i.e. %s entries of MB_MOD_SIZE) or drop a program.\n\n' '16'; \
	   exit 1; \
	 fi; \
	 if [ $$secs -gt $(MODULES_MAX_SECTORS) ]; then \
	   printf '\nERROR: the modules need %s sectors, the region holds %s.\n' \
	          "$$secs" '$(MODULES_MAX_SECTORS)'; \
	   printf '       LBA %s .. %s in boot/layout.inc, %s bytes in total.\n' \
	          '$(MODULES_LBA)' \
	          "`expr $(MODULES_LBA) + $(MODULES_MAX_SECTORS) - 1`" \
	          '$(MODULES_MAX_BYTES)'; \
	   printf '       Raise MODULES_MAX_SECTORS and RESERVED_SECTORS together,\n'; \
	   printf '       and check MODULES_PHYS still has that much room above it.\n\n'; \
	   exit 1; \
	 fi; \
	 echo "  BOOT    LBA $(MODULES_LBA) ..    $$n modules, $$secs / $(MODULES_MAX_SECTORS) sectors"; \
	 lba=$(MODULES_LBA); hex=''; \
	 for p in $(USER_PROGS); do \
	   sz=`stat -c%s $(BUILD_DIR)/$$p.elf`; \
	   dd if=$(BUILD_DIR)/$$p.elf of=$@ bs=512 seek=$$lba conv=notrunc status=none; \
	   dw=`printf '%02x%02x%02x%02x%02x%02x%02x%02x' \
	       $$((lba & 255)) $$((lba >> 8 & 255)) \
	       $$((lba >> 16 & 255)) $$((lba >> 24 & 255)) \
	       $$((sz & 255)) $$((sz >> 8 & 255)) \
	       $$((sz >> 16 & 255)) $$((sz >> 24 & 255))`; \
	   nm=`printf '%s' "$$p" | od -An -tx1 -v | tr -d ' \n'`; \
	   hex="$$hex$$dw$$nm"; \
	   while [ $$(( $${#hex} % ($(S2MOD_ENTRY_SIZE) * 2) )) -ne 0 ]; do \
	     hex="$${hex}00"; \
	   done; \
	   echo "  MOD     LBA $$lba   $$p  ($$sz bytes, name \"$$p\")"; \
	   lba=$$((lba + (sz + 511) / 512)); \
	 done; \
	 while [ $${#hex} -lt $$(( $(S2MOD_TABLE_BYTES) * 2 )) ]; do hex="$${hex}00"; done; \
	 magic=`dd if=$@ bs=1 skip=$(S2HDR_MAGIC_DISK_OFF) count=8 status=none`; \
	 [ "$$magic" = '$(S2HDR_MAGIC)' ] || { \
		printf '\nERROR: no "%s" magic at image offset %s.\n\n' \
		       '$(S2HDR_MAGIC)' '$(S2HDR_MAGIC_DISK_OFF)'; \
		exit 1; \
	 }; \
	 [ $$(( $(S2HDR_MODCOUNT_OFF) + 4 )) -eq $(S2HDR_MODTAB_OFF) ] || { \
		printf '\nERROR: the module count at %s and the table at %s are not\n' \
		       '$(S2HDR_MODCOUNT_OFF)' '$(S2HDR_MODTAB_OFF)'; \
		printf '       adjacent, and this recipe writes them as one block.\n\n'; \
		exit 1; \
	 }; \
	 printf '%02x%02x%02x%02x' \
	        $$((n & 255)) $$((n >> 8 & 255)) \
	        $$((n >> 16 & 255)) $$((n >> 24 & 255)) \
	   | xxd -r -p > $@.mod; \
	 printf '%s' "$$hex" | xxd -r -p >> $@.mod; \
	 dd if=$@.mod of=$@ bs=1 seek=$(S2HDR_MODCOUNT_DISK_OFF) conv=notrunc status=none; \
	 rm -f $@.mod; \
	 echo "  BOOT    off $(S2HDR_MODCOUNT_DISK_OFF)   s2_mod_count = $$n," \
	      "off $(S2HDR_MODTAB_DISK_OFF)  $(MB_MODS_MAX) x $(S2MOD_ENTRY_SIZE) byte table"
	@# Read the table back OUT of the finished image and follow it, rather
	@# than trusting the arithmetic that produced it. Cheap, and it is the
	@# only place where "the table says LBA n" and "the program is at LBA n"
	@# are ever compared -- stage 2 will believe the table without checking,
	@# and by then the evidence is gone.
	@set -e; \
	 got=`od -An -tu4 -j $(S2HDR_MODCOUNT_DISK_OFF) -N 4 $@ | tr -d ' \n'`; \
	 n=0; for p in $(USER_PROGS); do n=$$((n + 1)); done; \
	 [ "$$got" = "$$n" ] || { \
		printf '\nERROR: read back %s modules from image offset %s, wrote %s.\n\n' \
		       "$$got" '$(S2HDR_MODCOUNT_DISK_OFF)' "$$n"; \
		exit 1; \
	 }; \
	 i=0; \
	 for p in $(USER_PROGS); do \
	   e=$$(( $(S2HDR_MODTAB_DISK_OFF) + i * $(S2MOD_ENTRY_SIZE) )); \
	   rlba=`od -An -tu4 -j $$e -N 4 $@ | tr -d ' \n'`; \
	   rsz=`od -An -tu4 -j $$((e + 4)) -N 4 $@ | tr -d ' \n'`; \
	   rnm=`dd if=$@ bs=1 skip=$$((e + 8)) count=$(S2MOD_NAME_MAX) status=none \
	        | tr -d '\000'`; \
	   sz=`stat -c%s $(BUILD_DIR)/$$p.elf`; \
	   if [ "$$rsz" != "$$sz" ] || [ "$$rnm" != "$$p" ]; then \
	     printf '\nERROR: entry %s of the module table says %s bytes named "%s",\n' \
	            "$$i" "$$rsz" "$$rnm"; \
	     printf '       expected %s bytes named "%s".\n\n' "$$sz" "$$p"; \
	     exit 1; \
	   fi; \
	   dd if=$@ bs=512 skip=$$rlba count=$$(( (rsz + 511) / 512 )) status=none \
	     | head -c $$rsz > $@.mod; \
	   if ! cmp -s $@.mod $(BUILD_DIR)/$$p.elf; then \
	     printf '\nERROR: the %s bytes at LBA %s are not %s.\n' \
	            "$$rsz" "$$rlba" '$(BUILD_DIR)'/$$p.elf; \
	     printf '       The table points somewhere the program is not.\n\n'; \
	     rm -f $@.mod; exit 1; \
	   fi; \
	   rm -f $@.mod; \
	   i=$$((i + 1)); \
	 done; \
	 echo "  BOOT    $$n modules verified against $(BUILD_DIR)/*.elf, byte for byte"
	@# Prove the two-piece boot sector write did not break the BPB: if the
	@# BPB were damaged mdir cannot locate the root directory and fails.
	@$(MTOOLS_ENV) $(MDIR) -i $@ :: > /dev/null || { \
		printf '\nERROR: the filesystem in %s is no longer readable.\n' '$@'; \
		printf '       The BPB at 0x03..0x3D was damaged by the boot sector write.\n\n'; \
		exit 1; \
	 }
	@sig=`od -An -tx1 -j 510 -N 2 $@ | tr -d ' \n'`; \
	 [ "$$sig" = "55aa" ] || { \
		printf '\nERROR: boot signature at 0x1FE is %s, expected 55aa.\n\n' "$$sig"; \
		exit 1; \
	 }
	@echo "  Boot image ready: $@   (BPB intact, 0x55AA present)"

# Boot it the way a real machine would: BIOS, LBA 0, stage 1. No -kernel,
# no -initrd, no GRUB, no CD -- if this reaches the shell prompt then the
# whole chain in boot/ works on its own.
#
# The image is also the kernel's data disk: it is one FAT16 volume with the
# boot chain living in its reserved sectors, so /BIN/HELLO.ELF and the text
# files are right there for the shell to read.
run-bootdisk: $(BOOTIMG)
	$(call run_qemu,$(QEMU_BOOTDISK))

# --- debugging -------------------------------------------------------------
debug: $(KERNEL) user $(DISK)
	@echo ""
	@echo "  QEMU is starting halted with a GDB stub on tcp:1234."
	@echo "  In a second terminal:"
	@echo ""
	@echo "      gdb $(KERNEL)"
	@echo "      (gdb) target remote :1234"
	@echo "      (gdb) break kernel"
	@echo "      (gdb) continue"
	@echo ""
	$(call run_qemu,-s -S -kernel $(notdir $(KERNEL)) $(QEMU_INITRD) $(QEMU_DISK))

# --- write the ISO to a USB stick ------------------------------------------
# Usage: make usb DEV=/dev/sdX
# There is deliberately no default device and no way to skip the prompt.
usb: $(ISO)
	@if [ -z "$(DEV)" ]; then \
		printf '\nERROR: no target device given.\n'; \
		printf '       Usage:  make usb DEV=/dev/sdX\n'; \
		printf '       Find the right device with:  lsblk -o NAME,SIZE,MODEL,TRAN\n\n'; \
		exit 1; \
	fi
	@if [ ! -b "$(DEV)" ]; then \
		printf '\nERROR: "%s" is not a block device.\n\n' '$(DEV)'; \
		exit 1; \
	fi
	@echo ""
	@echo "  About to OVERWRITE this device with $(ISO):"
	@echo ""
	@lsblk -o NAME,SIZE,TYPE,MODEL,TRAN,MOUNTPOINTS $(DEV) || true
	@echo ""
	@echo "  ALL DATA ON $(DEV) WILL BE DESTROYED."
	@printf '  Type exactly "yes" to continue: '; \
	read answer; \
	if [ "$$answer" != "yes" ]; then echo "  Aborted."; exit 1; fi; \
	echo "  Writing..."; \
	sudo dd if=$(ISO) of=$(DEV) bs=4M status=progress oflag=sync conv=fsync
	@echo "  Done."

# --- write the GRUB-free boot image to a USB stick -------------------------
# Usage: make usb-boot DEV=/dev/sdX
#
# This is the one that matters on real hardware. $(BOOTIMG) carries our own
# stage 1 at LBA 0, an MBR partition table so that a BIOS offers the stick as
# a boot device at all, and the user programs as Multiboot modules in the
# reserved sectors -- no GRUB anywhere, and nothing the kernel has to read
# back off the stick once it is running.
#
# The refusals live in $(USB_BOOT_SH) rather than in this recipe, and there
# are six of them. Each one gets a paragraph in that file saying which way of
# losing a disk it prevents, and that reasoning does not survive being folded
# into a recipe where every line ends in a backslash and every dollar is
# doubled. The script is also runnable on its own, which is worth having when
# the thing being debugged is the script.
#
# DEV is passed QUOTED. An unset DEV then reaches the script as an empty
# second argument and is refused there, along with every other bad device,
# instead of vanishing from the command line and shifting the image into the
# device position -- which would put $(BOOTIMG) in front of dd's "of=".
usb-boot: $(BOOTIMG)
	@$(USB_BOOT_SH) $(BOOTIMG) '$(DEV)'

# --- housekeeping ----------------------------------------------------------
clean:
	@echo "  RM      $(BUILD_DIR)"
	@rm -rf $(BUILD_DIR)

help:
	@echo "TomatOS build targets:"
	@echo ""
	@echo "  all         Build $(KERNEL) and the user programs  (default)"
	@echo "  user        Build the ring 3 programs only: $(USER_ELFS)"
	@echo "  run         Boot the kernel directly in QEMU (-kernel, fastest),"
	@echo "              user programs handed over via -initrd"
	@echo "  iso         Build a BIOS-bootable $(ISO) via grub-mkrescue"
	@echo "  run-iso     Boot that ISO in QEMU (-cdrom, forced with -boot d so"
	@echo "              the BIOS does not try the hard disk first)"
	@echo "  floppy      Copy the kernel into a copy of $(FLOPPY_TMPL)"
	@echo "  run-floppy  Boot that floppy image in QEMU (-fda)"
	@echo "  disk        Build the FAT16 hard disk $(DISK) with mtools"
	@echo "              ($(DISK_SIZE_MB) MB, C/H/S $(DISK_CYLS)/$(DISK_HEADS)/$(DISK_SECS), 2 KB clusters,"
	@echo "              unpartitioned - boot sector at LBA 0). Attached as"
	@echo "              IDE 0 master by run, run-iso and debug."
	@echo "  bootdisk    Build $(BOOTIMG) -- the same FAT16 volume,"
	@echo "              plus TomatOS' OWN boot chain in its reserved sectors:"
	@echo "                LBA 0        stage 1, sharing the sector with the BPB"
	@echo "                LBA $(STAGE2_LBA) .. $(shell expr $(STAGE2_LBA) + $(STAGE2_SECTORS) - 1)   stage 2"
	@echo "                LBA $(KERNEL_LBA) .. $(shell expr $(KERNEL_LBA) + $(KERNEL_MAX_SECTORS) - 1)  kernel, flat (objcopy -O binary)"
	@echo "                LBA $(MODULES_LBA) .. $(shell expr $(MODULES_LBA) + $(MODULES_MAX_SECTORS) - 1) the user programs, as Multiboot modules"
	@echo "                LBA $(RESERVED_SECTORS) ..   the filesystem proper"
	@echo "              plus an MBR partition table at offset $(MBR_PART_OFF): one active"
	@echo "              entry, type 0x0e, spanning the volume. Many BIOSes only"
	@echo "              offer a USB stick as a boot device when it has one."
	@echo "  run-bootdisk"
	@echo "              Boot that image in QEMU as a hard disk. No -kernel,"
	@echo "              no -initrd, no GRUB: the BIOS starts stage 1."
	@echo "  debug       Start QEMU halted with a GDB stub on :1234"
	@echo "  usb         dd the ISO onto a USB stick: make usb DEV=/dev/sdX"
	@echo "  usb-boot    dd $(BOOTIMG) onto a USB stick:"
	@echo "                  make usb-boot DEV=/dev/sdX"
	@echo "              The GRUB-free one, and the one for real hardware."
	@echo "              $(USB_BOOT_SH) refuses a device that is not a"
	@echo "              whole disk, not removable, not on the USB bus or has"
	@echo "              something mounted, before it asks anything."
	@echo "  clean       Remove $(BUILD_DIR)/"
	@echo "  help        This text"
	@echo ""
	@echo "Options for the run targets:"
	@echo "  VNC=0                 run without VNC"
	@echo "  VNC_CLIENT=<prog>     use a different VNC client"
	@echo "  VNC_DISPLAY=<n>       different display number (port 5900+n)"
	@echo "  QEMU_KEYMAP=<layout>  keyboard layout, default: de"
	@echo "  NET=0                 run without a network card"
	@echo "  USB=0                 run without a USB controller"
	@echo "  USB=hid               attach a USB keyboard and mouse as well."
	@echo "                        They work -- QEMU routes input to them and"
	@echo "                        the HID driver reads it. Not the default"
	@echo "                        because it leaves the PS/2 pair unused, so"
	@echo "                        a driver regression would lock the keyboard."
	@echo ""
	@echo "Which targets give a graphics console (1024x768) and which do not:"
	@echo "  run          text mode. QEMU's -kernel loader does not implement"
	@echo "               the Multiboot video request -- it says so itself:"
	@echo "               \"multiboot knows VBE. we don't\"."
	@echo "  run-iso      graphics. GRUB honours the request in the header."
	@echo "  run-bootdisk graphics. Our own stage 2 sets the mode via VBE."
	@echo "  Add -append \"text\" to a Multiboot boot to ask for text mode;"
	@echo "  it is refused when the loader has already set a graphics mode,"
	@echo "  because nothing in the kernel can put that back."
	@echo "  NETDUMP=1             record traffic to $(BUILD_DIR)/net.pcap"
	@echo ""
	@echo "Network (QEMU user mode, no root needed) - set from the shell:"
	@echo "  ifconfig 10.0.2.15 255.255.255.0 10.0.2.2"
	@echo "  ping 10.0.2.2         the gateway answers ARP and ICMP echo"
	@echo ""
	@echo "User programs (user/<name>/ -> $(BUILD_DIR)/<name>.elf):"
	@echo "  $(USER_PROGS)"
	@echo "  Add one by making a directory under $(USER_DIR)/ and appending its"
	@echo "  name to USER_PROGS in this Makefile - iso and run pick it up."
	@echo ""
	@echo "Contents of the disk image (make disk):"
	@echo "  /README.TXT /MOTD.TXT     text, for the shell's cat"
	@echo "  /BIN/*.ELF                the user programs, to load from disk"
	@echo "  /DOCS/DISK.TXT            one level down, for directory traversal"
	@echo "  Inspect it without booting:"
	@echo "      minfo -i $(DISK) ::      geometry and FAT type"
	@echo "      mdir  -i $(DISK) -/ ::   the whole tree"
	@echo ""
	@echo "Boot chain contract (make bootdisk) <-> boot/stage2.asm:"
	@echo "  The kernel's length is patched into the image by the build, so"
	@echo "  stage 2 need not read all $(KERNEL_MAX_SECTORS) reserved sectors. stage2.asm"
	@echo "  opens with a header the Makefile writes into:"
	@echo "      stage2.bin +0 .. +3    jmp s2_start"
	@echo "      stage2.bin +$(S2HDR_MAGIC_OFF) .. +11   the magic \"$(S2HDR_MAGIC)\", checked, never written"
	@echo "      stage2.bin +$(S2HDR_SECTORS_OFF) .. +15  dd, kernel size in sectors, patched"
	@echo "      stage2.bin +$(S2HDR_MODCOUNT_OFF) .. +19  dd, how many modules follow, patched"
	@echo "      stage2.bin +$(S2HDR_MODTAB_OFF) ..      $(MB_MODS_MAX) x $(S2MOD_ENTRY_SIZE) bytes: dd LBA, dd length,"
	@echo "                             16 byte NUL padded name"
	@echo "  The programs are on the disk twice: as /BIN/*.ELF for machines"
	@echo "  whose disk the kernel can read, and as raw sectors from LBA"
	@echo "  $(MODULES_LBA) for the ones where it cannot -- a T430 has no UHCI, so"
	@echo "  the kernel cannot read the very stick it booted from, while the"
	@echo "  BIOS can. Decode the table with:"
	@echo "      xxd -s $(S2HDR_MODCOUNT_DISK_OFF) -l 64 $(BOOTIMG)"
	@echo "  Patched at image offset $(S2HDR_SECTORS_DISK_OFF) (LBA $(STAGE2_LBA) + $(S2HDR_SECTORS_OFF)); $(STAGE2_BIN)"
	@echo "  itself keeps its built-in default of $(KERNEL_MAX_SECTORS), so an unpatched"
	@echo "  stage 2 still boots and just reads more than it needs."
	@echo "  Size limits, both hard errors and never a silent truncation:"
	@echo "      stage 2  $(STAGE2_MAX_BYTES) bytes  (min of $(STAGE2_AREA_BYTES) reserved and"
	@echo "                            $(STAGE2_HEADROOM) between STAGE2_ORG and DISK_BUFFER)"
	@echo "      kernel   $(KERNEL_MAX_SECTORS) sectors = $(KERNEL_MAX_BYTES) bytes"
	@echo "  Inspect:  xxd -s $(S2HDR_MAGIC_DISK_OFF) -l 16 $(BOOTIMG)"
	@echo ""

# Header dependencies generated by -MMD -MP (kept last on purpose).
-include $(DEPS)
