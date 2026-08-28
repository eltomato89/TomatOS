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

KERNEL      := $(BUILD_DIR)/kernel.elf
ISO         := $(BUILD_DIR)/tomatos.iso

# Bootable GRUB Legacy floppy: the original in bin/ is a read-only template,
# we always work on a copy.
FLOPPY_TMPL := bin/dev_kernel_grub.img
FLOPPY      := $(BUILD_DIR)/tomatos_floppy.img

LINKER_SCRIPT := linker.ld

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

# Keyboard layout. The kernel carries a German keymap (src/kb.c), so QEMU has
# to deliver German scancodes. Under Wayland QEMU does not pass raw scancodes
# through but translates via the keysym -- without -k the guest ends up with
# the US layout, and the minus key arrives as the German sharp s.
# Override with:  make run QEMU_KEYMAP=en-us
QEMU_KEYMAP ?= de

QEMUFLAGS := -m 32 -k $(QEMU_KEYMAP)

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
	 $(QEMU) $(1) -serial stdio $(QEMU_DISPLAY_FLAGS) $(QEMUFLAGS) \
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

DEPS := $(C_OBJS:.o=.d)

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
.PHONY: all run iso run-iso floppy run-floppy debug usb clean help

all: $(KERNEL)

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

# --- run directly (fastest test cycle) -------------------------------------
# QEMU understands Multiboot ELF kernels, so no bootloader is involved.
run: $(KERNEL)
	$(call run_qemu,-kernel $(KERNEL))

# --- bootable ISO ----------------------------------------------------------
iso: $(ISO)

$(ISO): $(KERNEL)
	$(call need,$(GRUB_MKRESCUE),grub)
	$(call need,xorriso,libisoburn)
	@echo "  ISO     $@"
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	@printf '%s\n' \
		'set default=0' \
		'set timeout=3' \
		'' \
		'menuentry "TomatOS" {' \
		'    multiboot /boot/kernel.elf' \
		'    boot' \
		'}' \
		> $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR) 2>/dev/null
	@echo "  ISO ready: $@"

run-iso: $(ISO)
	$(call run_qemu,-cdrom $(ISO))

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
	$(call run_qemu,-fda $(FLOPPY))

# --- debugging -------------------------------------------------------------
debug: $(KERNEL)
	@echo ""
	@echo "  QEMU is starting halted with a GDB stub on tcp:1234."
	@echo "  In a second terminal:"
	@echo ""
	@echo "      gdb $(KERNEL)"
	@echo "      (gdb) target remote :1234"
	@echo "      (gdb) break kernel"
	@echo "      (gdb) continue"
	@echo ""
	$(call run_qemu,-s -S -kernel $(KERNEL))

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
	@echo "  all         Build $(KERNEL)  (default)"
	@echo "  run         Boot the kernel directly in QEMU (-kernel, fastest)"
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

# Header dependencies generated by -MMD -MP (kept last on purpose).
-include $(DEPS)
