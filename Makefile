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
INC_DIR     := $(SRC_DIR)/include
BUILD_DIR   := build
ISO_DIR     := $(BUILD_DIR)/iso

# User space lives entirely outside src/. Its programs are separate ELF
# executables that share nothing with the kernel but the "int 0x80" ABI, so
# they get their own sources, their own headers and their own object
# directory -- see the "User space" block further down.
USER_DIR    := user
USER_OBJ_DIR := $(BUILD_DIR)/user

KERNEL      := $(BUILD_DIR)/kernel.elf
ISO         := $(BUILD_DIR)/tomatos.iso

# Bootable GRUB Legacy floppy: the original in bin/ is a read-only template,
# we always work on a copy.
FLOPPY_TMPL := bin/dev_kernel_grub.img
FLOPPY      := $(BUILD_DIR)/tomatos_floppy.img

# FAT hard disk image -- the disk src/ata.c and src/fat.c talk to.
DISK        := $(BUILD_DIR)/tomatos_disk.img

LINKER_SCRIPT      := linker.ld
USER_LINKER_SCRIPT := $(USER_DIR)/user.ld

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
#  One entry per program; user/<name>.c becomes $(BUILD_DIR)/<name>.elf.
#  Programs are single translation units on purpose -- there is no libc to
#  link against, so anything shared between them belongs in a header.
# ---------------------------------------------------------------------------
# Programs that live on the disk rather than in the kernel. "make disk" copies
# each one into /BIN as NAME.ELF, and the shell runs an unknown command by
# looking for it there -- so adding a name here is all it takes to add a
# command. Keep the names to eight characters: they end up as 8.3 entries on a
# FAT16 volume, and a longer one would need a VFAT long name entry, which the
# kernel's directory reader skips.
USER_PROGS := hello ls cat fetch rm cp

# The C library every program links against: user/lib.c, holding _start (which
# calls main), printf, the string routines and the file helpers. It is a plain
# object rather than an archive -- with a handful of programs, "ar" would only
# add a step and a chance for a stale index, and the linker garbage collects
# nothing here either way.
USER_LIB_SRC := $(USER_DIR)/lib.c
USER_LIB_OBJ := $(USER_OBJ_DIR)/lib.o

USER_ELFS  := $(patsubst %,$(BUILD_DIR)/%.elf,$(USER_PROGS))
USER_OBJS  := $(patsubst %,$(USER_OBJ_DIR)/%.o,$(USER_PROGS))
USER_DEPS  := $(USER_OBJS:.o=.d) $(USER_OBJ_DIR)/lib.d

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
#   -nostdinc -I $(USER_DIR)
#                         No system headers, and NOT $(INC_DIR): a user
#                         program must not be able to reach kernel internals
#                         even by accident. user/syscall.h is the whole of
#                         its world.
USER_CFLAGS := \
	-m32 \
	-std=gnu89 \
	-mgeneral-regs-only \
	-fno-pic -fno-pie \
	-ffreestanding -fno-builtin -nostdinc \
	-I $(USER_DIR) \
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

# Keyboard layout. The kernel carries a German keymap (src/kb.c), so QEMU has
# to deliver German scancodes. Under Wayland QEMU does not pass raw scancodes
# through but translates via the keysym -- without -k the guest ends up with
# the US layout, and the minus key arrives as the German sharp s.
# Override with:  make run QEMU_KEYMAP=en-us
QEMU_KEYMAP ?= de

QEMUFLAGS := -m 32 -k $(QEMU_KEYMAP)

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
#  The card is an RTL8139 because that is the one src/rtl8139.c drives.
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
# master, the drive src/ata.c calls drive 0), with "format=raw" spelled out.
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
#      LBA 1 .. 16      stage 2                       (8 KiB)
#      LBA 17 .. 527    kernel, flat image            (255 KiB)
#      LBA 528 ..       the FAT16 filesystem proper
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
#  Wildcard based -- no hand-maintained file list.
#  src/test.S is a leftover empty file and is deliberately not built.
# ---------------------------------------------------------------------------
C_SRCS   := $(wildcard $(SRC_DIR)/*.c)
ASM_SRCS := $(wildcard $(SRC_DIR)/*.asm)

C_OBJS   := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
ASM_OBJS := $(patsubst $(SRC_DIR)/%.asm,$(BUILD_DIR)/%.o,$(ASM_SRCS))

# start.o first: the Multiboot header must end up at the front of the image.
OBJS := $(BUILD_DIR)/start.o $(filter-out $(BUILD_DIR)/start.o,$(ASM_OBJS) $(C_OBJS))

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
        debug usb clean help

all: $(KERNEL) user

# --- link ------------------------------------------------------------------
$(KERNEL): $(OBJS) $(LINKER_SCRIPT) | $(BUILD_DIR)
	@echo "  LD      $@"
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)
	@echo "  Kernel ready: $@"

# --- compile ---------------------------------------------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC      $<"
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.asm | $(BUILD_DIR)
	$(call need,$(AS),nasm)
	@echo "  AS      $<"
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(USER_OBJ_DIR):
	@mkdir -p $(USER_OBJ_DIR)

# --- user space programs ---------------------------------------------------
user: $(USER_ELFS) $(USER_MODULES)

$(USER_OBJS) $(USER_LIB_OBJ): $(USER_OBJ_DIR)/%.o: $(USER_DIR)/%.c | $(USER_OBJ_DIR)
	@echo "  CC/U    $<"
	$(CC) $(USER_CFLAGS) $(DEPFLAGS) -c $< -o $@

# The program plus the library, and nothing else on the link line: no kernel
# objects, no libgcc, no crt files. What comes out is a static ET_EXEC with a
# single PT_LOAD segment and not one relocation -- verify with
# "readelf -l" and "readelf -r".
#
# lib.o carries _start, so it has to be on the line even for a program that
# never calls anything else in it; ENTRY(_start) in user.ld would otherwise
# have no symbol to resolve.
$(USER_ELFS): $(BUILD_DIR)/%.elf: $(USER_OBJ_DIR)/%.o $(USER_LIB_OBJ) $(USER_LINKER_SCRIPT) | $(BUILD_DIR)
	@echo "  LD/U    $@"
	$(LD) $(USER_LDFLAGS) -o $@ $< $(USER_LIB_OBJ)

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
# The volume is NOT partitioned. There is no MBR partition table and no
# hidden-sector offset (-H 0): the FAT boot sector sits at LBA 0, which is
# where fat_mount() looks for it. A partition table would mean the kernel had
# to parse one before it could find the filesystem, and it does not.
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
# $(RESERVED_SECTORS) = 528, which is what keeps the filesystem out of the
# boot chain's sectors -- mformat then starts the first FAT at LBA 528 and
# the area below it is ours to write with dd.
#
# Raising the reserved count does not endanger the FAT16 classification the
# geometry block above argues for: it costs 527 sectors out of 65520, so
#   (65520 - 528 - 2*64 - 32) / 4 = 16208 clusters
# instead of 16339. Both sit in the middle of the 4085..65524 band. Confirm
# with "minfo -i <image> ::" -- it prints the reserved count and the type.
define fat_format
	$(call need,$(MFORMAT),mtools)
	@rm -f $(1)
	$(MTOOLS_ENV) $(MFORMAT) -C -i $(1) \
		-t $(DISK_CYLS) -h $(DISK_HEADS) -s $(DISK_SECS) \
		-c $(DISK_CLUSTER) -H 0 -m $(DISK_MEDIA) -R $(2) -v $(DISK_LABEL) ::
endef

# Fill a formatted volume with the tree the shell walks. Shared verbatim by
# $(DISK) and $(BOOTIMG) so the two images differ only in their boot chain,
# never in their contents.
#   $(1) = image file
define fat_populate
	@$(MTOOLS_ENV) $(MMD) -i $(1) ::/BIN ::/DOCS
	@for p in $(USER_PROGS); do \
		u=`echo $$p | tr 'a-z' 'A-Z'`; \
		echo "  DISK    /BIN/$$u.ELF"; \
		$(MTOOLS_ENV) $(MCOPY) -o -i $(1) $(BUILD_DIR)/$$p.elf ::/BIN/$$u.ELF; \
	done
	@echo "  DISK    /README.TXT /MOTD.TXT /DOCS/DISK.TXT"
	@{ \
		echo 'TomatOS on a disk'; \
		echo '================='; \
		echo ''; \
		echo 'This volume is read by src/ata.c (ATA PIO, LBA28) and'; \
		echo 'src/fat.c (FAT12/16, read only).'; \
		echo ''; \
		echo 'Layout:'; \
		echo '  /BIN    user programs, one ELF each'; \
		echo '  /DOCS   text files, one level down'; \
		echo ''; \
		echo 'Rebuild it with "make disk".'; \
	} | $(MTOOLS_ENV) $(MCOPY) -o -i $(1) - ::/README.TXT
	@{ \
		echo 'Welcome to TomatOS.'; \
		echo 'Ripe since 2011.'; \
	} | $(MTOOLS_ENV) $(MCOPY) -o -i $(1) - ::/MOTD.TXT
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
		echo 'range. Nothing here is a partition: the boot sector is'; \
		echo 'at LBA 0.'; \
		echo ''; \
		echo 'If you can read this, directory traversal works.'; \
	} | $(MTOOLS_ENV) $(MCOPY) -o -i $(1) - ::/DOCS/DISK.TXT
endef

# Makefile is a prerequisite because the text files above live in this file:
# editing their content has to rebuild the image, and so does changing the
# geometry.
$(DISK): $(USER_ELFS) Makefile | $(BUILD_DIR)
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
	 echo "  Stage 2: $$sz / $(STAGE2_MAX_BYTES) bytes used, header magic OK"

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
# 4 byte aligned. It is NOT at offset 0 -- src/start.asm puts the entry point
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
	   printf '       src/start.asm still carries it and that linker.ld keeps\n'; \
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

$(BOOTIMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(USER_ELFS) Makefile | $(BUILD_DIR)
	@echo "  IMG     $@  ($(DISK_SIZE_MB) MB, FAT16, $(RESERVED_SECTORS) reserved sectors)"
	$(call fat_format,$@,$(RESERVED_SECTORS))
	$(call fat_populate,$@)
	@echo "  BOOT    LBA 0        stage 1  (jump + code, BPB preserved)"
	@dd if=$(STAGE1_BIN) of=$@ bs=1 count=3 conv=notrunc status=none
	@dd if=$(STAGE1_BIN) of=$@ bs=1 skip=62 seek=62 count=450 conv=notrunc status=none
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
	@echo "                LBA $(RESERVED_SECTORS) ..    the filesystem proper"
	@echo "  run-bootdisk"
	@echo "              Boot that image in QEMU as a hard disk. No -kernel,"
	@echo "              no -initrd, no GRUB: the BIOS starts stage 1."
	@echo "  debug       Start QEMU halted with a GDB stub on :1234"
	@echo "  usb         dd the ISO onto a USB stick: make usb DEV=/dev/sdX"
	@echo "  clean       Remove $(BUILD_DIR)/"
	@echo "  help        This text"
	@echo ""
	@echo "Options for the run targets:"
	@echo "  VNC=0                 run without VNC"
	@echo "  VNC_CLIENT=<prog>     use a different VNC client"
	@echo "  VNC_DISPLAY=<n>       different display number (port 5900+n)"
	@echo "  QEMU_KEYMAP=<layout>  keyboard layout, default: de"
	@echo "  NET=0                 run without a network card"
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
	@echo "User programs (user/<name>.c -> $(BUILD_DIR)/<name>.elf):"
	@echo "  $(USER_PROGS)"
	@echo "  Add one by dropping the source in $(USER_DIR)/ and appending its"
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
