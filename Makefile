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
.PHONY: all user run iso run-iso floppy run-floppy debug usb clean help

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
run: $(KERNEL) user
	$(call run_qemu,-kernel $(notdir $(KERNEL)) $(QEMU_INITRD))

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

run-iso: $(ISO)
	$(call run_qemu,-cdrom $(notdir $(ISO)))

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

# --- debugging -------------------------------------------------------------
debug: $(KERNEL) user
	@echo ""
	@echo "  QEMU is starting halted with a GDB stub on tcp:1234."
	@echo "  In a second terminal:"
	@echo ""
	@echo "      gdb $(KERNEL)"
	@echo "      (gdb) target remote :1234"
	@echo "      (gdb) break kernel"
	@echo "      (gdb) continue"
	@echo ""
	$(call run_qemu,-s -S -kernel $(notdir $(KERNEL)) $(QEMU_INITRD))

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
	@echo "  run-iso     Boot that ISO in QEMU (-cdrom)"
	@echo "  floppy      Copy the kernel into a copy of $(FLOPPY_TMPL)"
	@echo "  run-floppy  Boot that floppy image in QEMU (-fda)"
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

# Header dependencies generated by -MMD -MP (kept last on purpose).
-include $(DEPS)
