# TomatOS

A 32-bit x86 hobby kernel (Multiboot 1) — preemptive scheduler, VGA text
console, PS/2 keyboard with German layout, PIT timer, CMOS clock, physical
frame allocator, kernel heap and a small shell. Originally written between
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

The shell offers `help`, `taskmgr`, `start`, `mem`, `reboot` and `exit`.
`taskmgr` without an argument prints its own syntax, `mem` shows the memory
state, and `mem -t` runs a heap self-test.

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
| `make floppy` | Place the kernel into a copy of the GRUB Legacy floppy image |
| `make run-floppy` | Boot that floppy image in QEMU |
| `make debug` | Start QEMU halted with a GDB stub on port 1234 |
| `make usb DEV=/dev/sdX` | Write the ISO to a USB stick (asks first) |
| `make clean` | Remove `build/` |
| `make help` | This list |

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
