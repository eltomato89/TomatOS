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
MCOPY       := mcopy
MFORMAT     := mformat
MMD         := mmd

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
USER_PROGS := hello

USER_ELFS  := $(patsubst %,$(BUILD_DIR)/%.elf,$(USER_PROGS))
USER_OBJS  := $(patsubst %,$(USER_OBJ_DIR)/%.o,$(USER_PROGS))
USER_DEPS  := $(USER_OBJS:.o=.d)

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
.PHONY: all user run iso run-iso floppy run-floppy disk debug usb clean help

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

$(USER_OBJS): $(USER_OBJ_DIR)/%.o: $(USER_DIR)/%.c | $(USER_OBJ_DIR)
	@echo "  CC/U    $<"
	$(CC) $(USER_CFLAGS) $(DEPFLAGS) -c $< -o $@

# One object per program, and nothing else on the link line: no kernel
# objects, no libgcc, no crt files. What comes out is a static ET_EXEC with a
# single PT_LOAD segment and not one relocation -- verify with
# "readelf -l" and "readelf -r".
$(USER_ELFS): $(BUILD_DIR)/%.elf: $(USER_OBJ_DIR)/%.o $(USER_LINKER_SCRIPT) | $(BUILD_DIR)
	@echo "  LD/U    $@"
	$(LD) $(USER_LDFLAGS) -o $@ $<

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

# Makefile is a prerequisite because the text files below live in this file:
# editing their content has to rebuild the image, and so does changing the
# geometry.
$(DISK): $(USER_ELFS) Makefile | $(BUILD_DIR)
	$(call need,$(MFORMAT),mtools)
	@echo "  IMG     $@  ($(DISK_SIZE_MB) MB, C/H/S $(DISK_CYLS)/$(DISK_HEADS)/$(DISK_SECS), FAT16)"
	@rm -f $@
	$(MTOOLS_ENV) $(MFORMAT) -C -i $@ \
		-t $(DISK_CYLS) -h $(DISK_HEADS) -s $(DISK_SECS) \
		-c $(DISK_CLUSTER) -H 0 -m $(DISK_MEDIA) -v $(DISK_LABEL) ::
	@$(MTOOLS_ENV) $(MMD) -i $@ ::/BIN ::/DOCS
	@for p in $(USER_PROGS); do \
		u=`echo $$p | tr 'a-z' 'A-Z'`; \
		echo "  DISK    /BIN/$$u.ELF"; \
		$(MTOOLS_ENV) $(MCOPY) -o -i $@ $(BUILD_DIR)/$$p.elf ::/BIN/$$u.ELF; \
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
	} | $(MTOOLS_ENV) $(MCOPY) -o -i $@ - ::/README.TXT
	@{ \
		echo 'Welcome to TomatOS.'; \
		echo 'Ripe since 2011.'; \
	} | $(MTOOLS_ENV) $(MCOPY) -o -i $@ - ::/MOTD.TXT
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
	} | $(MTOOLS_ENV) $(MCOPY) -o -i $@ - ::/DOCS/DISK.TXT
	@echo "  Disk ready: $@"

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

# Header dependencies generated by -MMD -MP (kept last on purpose).
-include $(DEPS)
