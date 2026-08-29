# TomatOS

A 32-bit x86 hobby kernel (Multiboot 1) — preemptive scheduler, VGA text
console, PS/2 keyboard with German layout, PIT timer, CMOS clock, physical
frame allocator, kernel heap, paging, ring 3 with system calls, per-task
address spaces, loadable programs, an ATA driver with a FAT12/16 filesystem,
VGA graphics, a framebuffer console at 1024x768, and a small shell. Originally written between
2006 and 2011, ported to a current Linux toolchain in 2026.

## Requirements

No cross toolchain is needed — the system GCC produces the required 32-bit
code with `-m32`.

```sh
sudo pacman -S --needed nasm qemu-system-x86 grub libisoburn mtools
```

`nasm` and `qemu-system-x86` are enough to build and test. `grub` and
`libisoburn` are only needed for `make iso`, `mtools` only for `make floppy`.

## Building

```sh
make
```

The result is `build/kernel.elf`. All intermediate files go to `build/`, and
`make clean` removes them.

## Running under QEMU

```sh
make run
```

QEMU loads the Multiboot ELF directly via `-kernel`, with no bootloader
involved — the fastest way to try a change. Quit with `Ctrl-C` in the
terminal.

The shell offers `help`, `taskmgr`, `start`, `mem`, `page`, `user`, `ps`,
`exec`, `ls`, `cat`, `df`, `gfx`, `reboot` and `exit`. `taskmgr` without an argument prints its own syntax, `mem` and `page`
show the memory and paging state, and `mem -t` / `page -t` run self-tests.

### Memory layout

The kernel is a **higher-half kernel**: linked for virtual `0xC0100000`,
loaded at physical `0x00100000`. All usable RAM is mapped as one contiguous
block from `KERNEL_VIRTUAL_BASE` (`0xC0000000`) upward, so physical and
virtual addresses differ by a constant offset. `V2P()` and `P2V()` in
`src/include/vmm.h` convert between the two, and every place where a
physical address meets a pointer goes through them — page table entries,
frames from the allocator, the multiboot info from the bootloader, and
memory mapped hardware such as the VGA buffer.

The lower 3 GiB are unmapped, which is where user space will live once
there is any. Two consequences today: a null pointer dereference raises a
page fault naming the offending address, instead of quietly reading the
real-mode interrupt vector table; and the kernel's own code is mapped
read-only, enforced by `CR0.WP` — without that bit the CPU ignores
read-only page table entries in ring 0.

`page -f` triggers a null pointer write on purpose to demonstrate the fault
handler; it kills the calling task, and the system carries on.

### Ring 3 and system calls

`user` starts a task that runs in **ring 3** and can only reach the kernel
through `int 0x80` — call number in `eax`, arguments in `ebx`/`ecx`/`edx`,
result in `eax`. `user -t` exercises the call path and the argument guards.

Vector `0x80` is the only IDT gate with DPL 3; every exception and IRQ
vector stays DPL 0, so user code cannot fake a page fault or a timer
interrupt. It is a *trap* gate rather than an interrupt gate, because an
interrupt gate clears `IF` and `SYS_SLEEP` would then wait forever for a
timer tick that can never arrive.

Each task owns a kernel stack; the TSS points `esp0` at the current one, so
the CPU has somewhere to switch to when a ring 3 task traps into the kernel.
User stacks sit below the kernel window with an unmapped guard page beneath
each, and are zeroed on allocation — the frame allocator hands out dirty
memory and ring 3 must not see what the previous owner left there.

### Address spaces

Every ring 3 task owns a page directory. Its upper quarter — entries 768 and
up, everything from `KERNEL_VIRTUAL_BASE` — is **shared**: the same page
tables, not copies. That is what lets an interrupt or a system call be taken
in any task's address space, since kernel code, kernel data and the task's
kernel stack stay mapped across a `CR3` load. The lower three quarters are
private, so two tasks can hold the same virtual address with different
contents and neither can reach the other's.

`user -i` demonstrates it: two ring 3 tasks write different values to one
address, each reads back only its own, and the kernel confirms the two
spaces resolve that address to different physical frames.

Pointer arguments from ring 3 are resolved in the caller's own directory —
a system call does not change `CR3` — and must carry `PAGE_USER` in both the
directory and the table entry, since the effective rights are the AND of the
two. Being mapped is not the question; being permitted is.

Sharing happens at directory-entry granularity, which has one consequence
worth knowing: a kernel mapping inside an existing page table shows up in
every address space at once, but one that needs a **new** page table would
only land in whichever directory is active. All RAM is mapped up front by
`vmm_init()`, so this does not arise today — but anything added later that
maps a fresh kernel range has to happen before the first task space exists.

### Display

The QEMU package on Arch ships without graphical display backends
(`-display help` reports only `none`; `qemu-ui-gtk` and friends are separate
packages). The `run` targets therefore use QEMU's built-in VNC server and
launch a client automatically once the port is up. The server binds to
`127.0.0.1` only — without that, the running VM would be reachable from the
network without a password.

A client is required, for example `sudo pacman -S tigervnc`. The build looks
for `vncviewer`, `gvncviewer`, `vinagre`, `xtightvncviewer` and `krdc`.

```sh
make run VNC_CLIENT=gvncviewer   # different client
make run VNC_DISPLAY=3           # different display number (port 5903)
make run VNC=0                   # no VNC
```

Alternatively, `sudo pacman -S qemu-ui-gtk` adds a native window; then
`make run VNC=0 QEMUFLAGS="-m 32 -k de -display gtk"` is enough.

### Keyboard layout

The kernel carries a German keymap, so `make run` starts QEMU with `-k de`.
Without it, QEMU translates via the keysym under Wayland and assumes the US
layout — the minus key would then arrive as `ß`. For a different layout:

```sh
make run QEMU_KEYMAP=en-us
```

## All targets

| Command | Effect |
|---|---|
| `make` | Build the kernel to `build/kernel.elf` |
| `make run` | Boot directly in QEMU (fastest test cycle) |
| `make iso` | BIOS-bootable `build/tomatos.iso` via GRUB 2 |
| `make run-iso` | Boot that ISO in QEMU |
| `make floppy` | Place the kernel into a copy of the GRUB Legacy floppy image (see note) |
| `make run-floppy` | Boot that floppy image in QEMU (see note) |
| `make debug` | Start QEMU halted with a GDB stub on port 1234 |
| `make usb DEV=/dev/sdX` | Write the ISO to a USB stick (asks first) |
| `make clean` | Remove `build/` |
| `make help` | This list |

### The GRUB Legacy floppy

The floppy image in `bin/` carries GRUB Legacy 0.97 from 2011 and **no
longer boots the kernel** since the move to the higher half. It fails with
`Error 7: Loading below 1MB is not supported`, because that version derives
the load address from the ELF program header's *virtual* address, and
`0xC0100000` read as a signed value looks like it is below 1 MiB. GRUB 2 on
the ISO uses the physical address and loads the kernel correctly, as does
QEMU's `-kernel`.

### Programs

There is no filesystem yet, so programs arrive as **multiboot modules**: the
bootloader loads them next to the kernel, and the kernel records them at
boot. `ps` lists them, `exec <name>` loads one into a fresh address space
and runs it. Starting the same program twice gives two independent
instances, each with its own directory and its own copy of the data.

Programs are ordinary static ELF32 executables built separately from the
kernel — see `user/`. They link against nothing but `user/syscall.h`, which
is the entire libc a TomatOS program gets: inline `int 0x80` wrappers. Entry
point is `_start`, there is no C runtime and nothing to return to, so a
program ends by calling `SYS_EXIT`.

```sh
make user      # builds build/hello.elf
make run       # QEMU passes it via -initrd
make run-iso   # GRUB passes it via a module line
```

The loader walks the `PT_LOAD` program headers page by page rather than
segment by segment, because segments are not page aligned: the tail of one
and the head of the next can share a page. It maps a segment read-only
unless `PF_W` is set, zeroes the `.bss` part beyond `p_filesz`, and refuses
anything that would land at or above `KERNEL_VIRTUAL_BASE`.

### Disk and filesystem

`src/ata.c` talks to ATA/IDE drives with programmed I/O and LBA28 — no DMA,
no PCI enumeration, no interrupt handler; the driver polls and masks the
drive's interrupt. On top of it `src/fat.c` reads FAT12 and FAT16 volumes.

`make disk` builds a 32 MB FAT16 image with mtools and puts the user
programs plus a few text files on it; the run targets attach it as a hard
disk. `df` shows what was found, `ls` and `cat` read it, and `exec` can load
a program **from the filesystem** rather than from a boot module:

```
@TomatOS> exec /bin/hello.elf
```

Two details worth knowing about FAT. The type follows from the **data
cluster count** (below 4085 is FAT12, below 65525 FAT16), never from the
type string in the boot sector — that field is advisory and often wrong. And
the root directory of FAT12/16 is a fixed area behind the FATs, not part of
the cluster chain, unlike every subdirectory.

### Graphics

`gfx` switches to VGA mode 13h — 320x200 at 256 colours — draws a picture
with the built-in primitives, and returns to the shell on a keypress.
There is no BIOS to call from protected mode, so the mode is set by writing
the register tables directly.

Two things make the round trip work. The text mode font lives in plane 2 of
the very memory the graphics mode uses as its framebuffer, so it is saved
before the switch and written back afterwards — without that the console
returns to a screen of blanks. The visible text is a second casualty for the
same reason and is saved separately, which is why the scrollback survives.

Nothing prints while graphics mode is up: writes to `0xB8000` are discarded
by the hardware in that mode, but they would still move the cursor and
scroll a screen the saved copy no longer matches. The status bar task is
suspended for the duration for the same reason.

### Booting without GRUB

`make bootdisk` builds `build/tomatos_boot.img`, which carries its own boot
chain — no GRUB involved. `make run-bootdisk` boots it as a hard disk.

```
LBA 0        stage 1, sharing the boot sector with the FAT16 BPB
LBA 1 .. 16  stage 2
LBA 17 ..    the kernel, as a flat image
LBA 528 ..   the FAT16 filesystem the kernel then mounts
```

Stage 2 and the kernel live in the volume's **reserved sectors**, the area
a FAT filesystem sets aside before its first FAT. So the boot chain needs no
FAT reader in assembly, and the filesystem stays perfectly ordinary.

The handover is deliberately identical to GRUB's: `eax = 0x2BADB002` and
`ebx` pointing at a multiboot info structure that stage 2 fills in itself —
memory map from `int 15h/E820`, and the framebuffer fields from VBE. Both
boot paths therefore remain available, and the kernel cannot tell them apart.

**The reason for doing this at all:** stage 2 runs in real mode, where
`int 0x10` is available. That is the only place a VBE mode such as 1024x768
can be set — from protected mode it is unreachable without a v86 monitor.
The kernel cannot establish a high resolution mode by itself, so whoever
boots it has to.

### The framebuffer console

Booted from `make run-bootdisk`, stage 2 establishes a 1024x768 VBE mode and
the console renders text into it — **128x48 characters** against the 80x25 of
text mode. `gfx -i` reports the mode, the mapping and the console geometry.

Three things make it work:

The framebuffer is **not reachable when the first line is printed**. Paging is
on from `start.asm`, but its boot directory maps only the low 4 MiB, and a
card's framebuffer sits far outside the direct mapping — QEMU puts it at
`0xFD000000`. So the console writes into a **shadow buffer** in `.bss` from
the very first character and paints the whole thing once `vmm_map_mmio()` can
map the memory. Nothing said during early boot is lost, which is exactly when
losing it would hurt.

The page tables for that MMIO window are created **while `vmm_init()` runs**,
before any address space exists. The kernel half is shared between address
spaces at page *directory entry* granularity, so a mapping needing a fresh
page table would otherwise live only in whichever directory was active — and
the console would fault the moment the scheduler switched tasks.

**Scrolling re-rasterises from the shadow buffer** rather than moving pixels,
and only where cells actually differ. Measured: 0.27 ms against 1.16 ms for a
pixel move at 1024x768x32 — and the real margin is wider, because moving
pixels means *reading* an uncached framebuffer. The framebuffer is never read.

## On real hardware

`make iso` produces an image that boots via **legacy BIOS/CSM**. It goes onto
a stick with `make usb DEV=/dev/sdX` — the target device is shown first and
has to be confirmed with `yes`. Choose the device carefully; `lsblk` helps to
identify it.

On UEFI-only machines without CSM the screen stays black: the kernel writes
directly to the VGA text buffer at `0xB8000`, which no longer exists there.
That would require a framebuffer output path.

## Debugging

```sh
make debug           # terminal 1 — QEMU waits, halted
gdb build/kernel.elf # terminal 2
(gdb) target remote :1234
(gdb) break kernel
(gdb) continue
```

Since the kernel is linked as ELF, GDB has all symbols available.

## Layout

```
src/            kernel sources
src/include/    own headers (src/include/sys/ is an unused DJGPP leftover)
linker.ld       linker script, loads at 1 MiB
bin/            the original GRUB Legacy floppy image (template, never modified)
legacy/         the 2011 Windows toolchain (DJGPP, VFD) and its batch files
```

Sources are UTF-8 with LF. The build uses `-std=gnu89`; under newer standards
the implicit declarations of the original code become hard errors.
