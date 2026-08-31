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

**Typing a command that is not built into the shell runs a program off the
disk.** `ls` is `/BIN/LS.ELF`, an ordinary ring 3 ELF in an address space of
its own — the shell spawns it, waits for it, and prints the prompt afterwards.
That is the mechanism the kernel/userland split rests on: moving a command out
of the kernel means writing it as a program and adding its name to
`USER_PROGS`, nothing else. `help` lists both categories and reads the second
one off `/BIN` rather than from a hardcoded list, so it cannot drift.

Programs also arrive as **multiboot modules**, which is how they work with no
disk attached: the bootloader loads them next to the kernel, `ps` lists them
and `exec <name>` runs one.

```sh
make user      # builds build/hello.elf, build/ls.elf, build/cat.elf
make run       # QEMU passes them via -initrd, and attaches the disk image
make run-iso   # GRUB passes them via module lines
```

A program is a static ELF32 built separately from the kernel — see `user/`.
It links against `user/syscall.h` (the raw `int 0x80` wrappers) and
`user/lib.c`, a small C library holding `_start`, `printf`, the string
routines and the file helpers. `_start` calls `main(argc, argv)` and hands its
return value to `SYS_EXIT`, so a utility is just a `main()`.

Two things about that library are worth knowing. Its `printf` **has field
widths**; the kernel's does not, which is why the kernel's shell pads columns
by hand. And an unsupported conversion makes it stop rather than continue —
the kernel's `printf` skips one silently, and because the skipped argument is
never fetched, *every later* `va_arg` reads from the wrong place. That is why
`printf("%02x", v)` prints the literal text `02x` inside the kernel.

Arguments live **on the user stack**, not in kernel memory. The loader writes
the strings, the `argv` array, `argc` and a fake return address into the top of
the program's stack page and points `esp` below them, which is exactly the
frame a cdecl `_start(int argc, char **argv)` expects. A per-task argument
buffer in the kernel would have been 64 tasks' worth of `.bss` in a kernel
being actively shrunk.

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

### A window system in ring 3

`gui` is a program on the disk — `/BIN/GUI.ELF` — with windows, a pointer, a Z
order, dragging and raising. The kernel supplies three system calls and
nothing else:

```
sys_mapfb(&info)     take the screen; address, geometry and pixel format
sys_unmapfb()        give it back, the kernel repaints its console
sys_input(&ev, ms)   the next keyboard or mouse event, blocking
```

**`sys_mapfb` is the first call here that puts hardware into a ring 3 address
space.** What comes back is a window onto the graphics card: writes change the
monitor immediately, there is no clipping and no compositing, and a program
holding it can draw anywhere. That is the point. What it must not be able to
do is reach anything else, and the guard that matters is that the physical
range has to lie **above the RAM the frame allocator knows about** — a
"framebuffer" overlapping RAM would put kernel memory into a user half with
`PAGE_USER` set. The caching flags are deliberately identical to the kernel's
own mapping of the same frames, because two mappings of one device with
different cacheability is architecturally undefined.

It is an **ownership transfer**, not just a mapping. The kernel's console is on
that screen too, so `fbcon_suspend()` takes it and `fbcon_resume()` gives it
back with a full repaint; a second caller gets `SYS_EBUSY`. The console keeps
its shadow buffer up to date while suspended, so everything printed meanwhile
appears in order afterwards — verified by counting colours in a screenshot:
during a suspension the image contains exactly the three colours the other
program drew and zero console pixels, despite 120 lines being printed.

**Releasing on death is not optional.** A program that crashes must not leave a
screen nobody repaints, so the release happens on a normal exit, on a page
fault, on `taskmgr -k` and on pid reuse — all four confirmed on screen, with
the console and its full boot log back each time.

Keyboard and mouse share **one** event queue, because a program with a pointer
and a keyboard has to wait for whichever comes first, and this kernel cannot
wait on two things at once.

#### Redrawing is the whole problem

Moving a window exposes what was behind it, and that — not drawing — is what a
window system is about. `gui` keeps no saved pixels anywhere, not even under
the pointer: for each damage rectangle it sets a clip window and repaints the
*entire* scene into it, desktop first, then windows bottom to top, then the
cursor. The exposed area is repaired without anything having to know it was
exposed. The desktop's grid is there on purpose — a repaint that got the region
wrong shows as a broken line where a flat colour would hide it.

**Damage rectangles are not an optimisation here, they are the difference
between usable and not.** Measured on the machine: a full-screen blit is 3.0 to
3.8 ms, while the cheapest real frame — the pointer moved and nothing else — is
one 24×34 rectangle, 3264 bytes, about 960 times cheaper. A whole session came
to 71 MB copied in 97 ms where flushing every frame would have moved 550 MB.
Damage is coalesced until the event queue runs dry or a 16 ms budget expires,
so a drag burst becomes frames rather than one blit per mouse packet.

Hit testing walks the Z order **top down** and stops at the first window
containing the point, and raising happens *after* the hit is decided — raising
first would change the answer. Raising damages two rectangles, not one: the
window coming forward, and the one that *was* in front, which does not move but
is drawn differently once it is no longer active. A screenshot caught exactly
that bug, as half a title bar in the old colour.

The 3 MB back buffer is `.bss`, which turns out to be right rather than merely
available: `exec.c` allocates a frame per page and zeroes everything past
`p_filesz`, so it costs nothing on disk and arrives already cleared — a 3 MB
`memset` that never runs.

#### The mode is negotiated, not chosen

The same binary lands at 1920x1080 on one machine and 640x480 on another.
`vbe_res_table` in `boot/vbe.inc` ranks six sizes and stage 2 takes the highest
the card actually reports; the Multiboot header asks GRUB for the same. Nothing
matches on a VBE *mode number* — the enumeration compares the width and height
the card reports, so 1920x1080 not being a standard VESA mode costs nothing,
and a size the card does not offer is simply never scored.

Tested by starving the card: 16 MB gives 1920x1080x32, 4 MB gives
1920x1080x**16** (resolution beats depth, which is how the ranking is meant to
work), 2 MB gives 1024x768, 1 MB gives 800x600, and a Cirrus card — a different
vgabios entirely — lands on 1280x1024. None failed.

**1600x1200 is deliberately absent** although it is a standard VESA mode and
belongs there by pixel count: 1200 exceeds `FBCON_MAX_HEIGHT`, so the console
would size itself to 67 rows on a screen with room for 75 and leave the bottom
128 lines holding whatever the BIOS left. The assembler refuses the row, which
is how that was caught rather than remembered. 1600x900 takes its place.

The ranking is by pixel count, and the file says where that is the wrong
question: a 16:9 panel whose card stops short of 1920x1080 would drop to
1280x1024, a 5:4 mode a widescreen monitor stretches. 1600x900 exists for that.
What cannot be fixed by any ordering is a card offering only 5:4 modes — the
shape of the monitor is not in the VBE information at all.

`FBCON_MAX_WIDTH`/`HEIGHT` in `src/include/fbcon.h` is the single place the
ceiling is written down, and its comment names the four things that have to
agree: the bootloader's table, the Multiboot request, the console's shadow
buffer, and the window system's back buffer. If one falls behind, the failure
is quiet — the console uses the top-left corner of a larger screen and the rest
keeps whatever was on it.

#### Windows are measured in cells, not pixels

The window system sizes itself the same way. A window declares how many
character cells it needs; the cell comes from the mode. The floor was found by
counting the actual strings rather than guessing — the widest line in the
program is 29 characters and the deepest window 11 lines, so the smallest
honest window is 242x132. At 1280x720 and above the cell doubles to 16x16 and
everything doubles with it, which is the only magnification an 8x8 font
tolerates.

Placement is a centred cascade whose step is the window size minus an overlap,
so consecutive windows overlap *by construction* at any size — a quadrant grid
would stop overlapping at 1920x1080, and the Z order would stop being visible.
A tightening factor shrinks the steps until the group fits the margins, and
tightening only ever increases overlap, so a small screen loses spread rather
than losing a window off an edge.

The result at three sizes: 640x480 fills the width and stays legible, 1024x768
uses a third of the screen where the first version used most of it, and
1920x1080 centres a cluster over about 70 by 50 percent.

**A framebuffer needs a loader that supplies one.** `make run` does not, so
`gui` is `make run-iso` or `make run-bootdisk`. QEMU now gets 64 MB by default:
a graphics mode costs twice, once for the framebuffer and once for the back
buffer a program draws into, and at 1920x1080x32 those are 8.3 MB each.

### USB

`lsusb` enumerates what is on the bus. The stack is in two files on purpose,
and the split is the point rather than tidiness: roughly two thirds of USB is
the same whatever silicon is underneath — resetting a port, giving a device an
address, reading its descriptors, working out what it is — and only the bottom
third, moving bytes to an endpoint, is controller specific. There are four
incompatible ways to do that, and this kernel drives the simplest: **UHCI**,
whose registers are in I/O space and whose data structures are three small
things.

A machine built in the last decade has **xHCI and nothing else**, so `lsusb`
says so rather than reporting "no USB": it walks the PCI bus itself for class
0C:03 and, finding an xHCI, names it and says whose limit that is. That
distinction — no USB hardware versus USB this driver cannot speak — is what a
person actually needs.

Three things about UHCI decided the driver:

**The controller reads its own data structures by DMA**, so they live at
physical addresses, their alignment is enforced by hardware (the low bits of
every link pointer are flags, so an unaligned structure is a *different*
address), and the controller may be walking a list while it is being built. A
transfer is therefore assembled completely while nothing points at it and
published with **one naturally aligned 32-bit store** — indivisible against a
PCI master's read, so the controller sees either the old terminator or the
finished chain and never a third state.

**Three answers are not errors.** A short packet is how a device says "that is
all", and a control transfer's data stage depends on it. A NAK is how an idle
keyboard says it has nothing — 1136 empty polls returned 0 in testing, and a
driver treating that as failure reports a broken keyboard on every machine. A
STALL is how a device says it does not know a request, and enumeration
deliberately provokes one.

**It polls rather than using its interrupt**, and for a reason found by
measuring: QEMU puts the UHCI on IRQ 11 — the same line as the network card —
and `irq_install_handler()` keeps one handler per line, so installing there
would silently unhook the NIC. Fixing that properly needs a shared-interrupt
chain in `irq.c`.

Enumeration has one step that is almost always got subtly wrong, and the
reason is worth recording. Reading the device descriptor needs endpoint 0's
packet size, which is *in* the device descriptor. The answer is to ask for only
the first 8 bytes — 8 is the smallest legal packet size, so every device can
move it, and the field sits at offset 7. The lazy versions work perfectly on
every device whose packet size really is 8, which is every low-speed keyboard
and mouse: exactly the hardware one tests with.

`uhci_frames()` exists because a controller that was configured and never
started looks identical from every other angle. `lsusb` prints it as a *rate*,
because a rate has an expected value: a UHCI frame is one millisecond by
definition, so a live controller reads 1000/s and no other number.

**`make run` attaches the controller and no devices.** `USB=hid` plugs in a
keyboard and a mouse, and is not the default because QEMU then routes input to
them — and with no HID driver yet, the machine boots, shows a prompt, and
cannot be typed at. That was tried rather than assumed.

#### HID: the same keys, a different bus

`USB=hid` attaches a USB keyboard and mouse, and they work. The proof is
blunt: QEMU routes input to the most recently attached device of a kind, so
with a USB keyboard present the PS/2 one gets nothing — and before the class
driver existed, that configuration booted to a prompt that could not be typed
at. Now `lsusb` typed on the USB keyboard reaches the shell.

Nothing above the drivers learns which bus a keypress came from.
`kb_inject()` and `mouse_inject()` deliver into the same slot and the same
queue the PS/2 handlers fill, so `getch()`, the shell, `SYS_GETCH` and the
window system are all unchanged. That was the promise at the top of
`mouse.h` from the day it was written, and this is it being cashed in.

Three things about HID are not what the PS/2 equivalent taught:

**A keyboard report is a state, not an event.** Eight bytes: modifiers, a
reserved byte, and six slots naming the keys *currently held*. A press is a
usage that is in the new report and not the old one, so the previous report
has to be kept — and slot order is meaningless, so it is set membership rather
than position. All six slots holding `0x01` is not six keys but the device
saying too many are down to report, and storing that as the previous state
would make every still-held key look newly pressed afterwards: a burst of
characters on release.

**Auto-repeat does not exist.** A PS/2 keyboard repeats in hardware; a USB one
just keeps saying the key is held. Without repeat implemented in the driver, a
held backspace deletes one character and the two keyboards on one machine
behave visibly differently.

**Y is already the right way up.** A HID boot mouse reports screen
orientation, positive down; a PS/2 mouse counts up and is flipped in its
driver. `mouse_inject()` therefore takes screen orientation, and its comment
says so, because doing the flip in both places is a pointer that moves the
wrong way from code that looks identical.

Usage IDs are not scancodes — a different alphabet with the shift state in its
own byte — so the layout is a second table, and it must produce the *same*
characters as `kb.c` does for the same physical keys. That correspondence is
testable without hardware and was: 82 usages compared against the PS/2 tables
in both shift states, all equal, with the divergences (keypad `/`, the key
next to left shift) documented rather than smoothed over.

Deliberately absent: hubs (so one device per root port — QEMU inserts a hub of
its own once a second device is attached unless the ports are named), mass
storage, isochronous transfers, and anything about USB 3, whose bus is
electrically separate and reachable only through xHCI.

### The mouse

`mouse` shows the pointer live: position, movement, buttons, and counters for
packets, resyncs, overflows and dropped events. It blocks while nothing moves
rather than polling — `ps` from another command shows the task as `Blocked`,
waiting on the channel `mouse_wait_channel()` returns.

The interface in `src/include/mouse.h` deliberately says nothing about PS/2 —
no ports, no scancodes, no packet format. A USB HID driver is meant to fill the
same queue later, and everything above it should keep working without learning
that anything changed.

Three details in the device get implementations wrong, and one thing about the
controller cannot be worked out by reading anything:

**The byte stream can desynchronise.** There is no framing, so a lost or
spurious byte shifts every packet after it and the pointer flies off. Bit 3 of
the first byte is always set and is the only anchor — but it is not enough on
its own: a misaligned byte that happens to have it set (a dy of 8 to 15) slips
through, so a half-collected packet older than 50 ms is also treated as debris.
Tested by stealing one byte out of the live stream, which is exactly what a
lost interrupt looks like: two resyncs, then fifteen consecutive movements
decoded exactly.

**dx and dy are 9-bit signed**, with the sign bit in the flags byte rather than
the data byte. Treating them as signed chars works until somebody moves the
mouse quickly, and then the pointer jumps backwards.

**Y counts up and screens count down.** Inverting it in one place is the
difference between a pointer that works and one that works in half the code.

**And the one that had to be found by testing:** the 8042 answers "read
configuration" by setting the *keyboard* output flag, so it raises IRQ 1. The
keyboard handler then reads the byte and the mouse's bring-up never sees it —
the first run reported "no mouse" on a machine that has one. IRQ 1 is masked
across that step now, and every failure path restores the configuration byte,
because a driver that gives up and leaves the keyboard switched off has done
more damage than the mouse it did not find.

Overflow keeps the buttons and discards the movement: an overflowed data byte
is the movement modulo 512 with the true size known only to be at least 256, so
using it is not using a value but inventing one. The buttons in the same packet
did not overflow.

### Faster copies

`memcpy` and `memset` were byte loops from 2011. They are `rep movsl`/`rep
stosl` with an aligned destination now, which matters for a GUI blitting a
3 MB back buffer, and already mattered for console scrolling, the ELF loader
and the network stack:

| | before | after | |
|---|---|---|---|
| `memcpy`, 3 MB | 1.00 ms | 0.27 ms | 3.8× |
| `memset`, 3 MB | 0.81 ms | 0.09 ms | 8.7× |

The choice of inline assembly over a C `uint32_t` loop was made from the
generated code, not from taste: GCC 16 at `-O1` compiles the C version to a
six-instruction body with no unrolling — the byte loop with a quarter of the
iterations. There is also a hazard peculiar to a freestanding kernel, which
inline assembly removes: a C copy loop is exactly the shape
`-ftree-loop-distribute-patterns` rewrites into a call to `memcpy`, so `memcpy`
would compile into a call to itself.

Measurement settled the alignment question too. Aligning the destination or the
source costs the same; what costs 2.5× is the two sides being *mutually*
misaligned, which neither choice can fix. The destination is aligned anyway,
for the case RAM benchmarks do not contain — a copy *into* the framebuffer,
where a split store can break a write-combining burst apart and a split load
cannot.

### Graphics

`gfx` draws a demonstration picture, and **which surface it draws into depends
on how the machine booted** — because the two graphics paths on this machine
have almost nothing in common.

Mode 13h (320x200, 256 palette entries) is reached at *runtime* by programming
the VGA registers, which is all `src/vga.c` needs and why it works from the
text console. The VBE framebuffer (1024x768x32) is established by our own
stage 2 *before the kernel starts*, and cannot be entered later: VBE is a real
mode BIOS interface and is out of reach once the kernel is in protected mode.

That asymmetry decides the design. A machine booted with `make run-bootdisk` is
already in the better mode, and switching it to 13h would be a one-way trip —
there would be no way back to 1024x768. So on that path the picture is drawn
into the framebuffer that is already there (`src/fbdraw.c`) and the console is
restored afterwards from `fbcon`'s shadow buffer, which is the only record of
what was on the screen.

`fbdraw.h` mirrors `vga.h` down to the argument order, including taking a
**palette index** rather than an RGB triple — mode 13h has no choice about
that, its hardware palette *is* what colour means there, and RGB on the other
side would have meant two copies of every drawing routine above it. `gfx`
picks between them with a function-pointer table chosen once, so there is one
copy of the picture and one of the layout. Two calls need adapters and are
worth knowing about: `fbdraw_palette()` takes 0..255 per channel where
`vga_palette()` takes the DAC's 0..63, and a table programmed into both without
scaling comes out at a quarter brightness.

The layout is **proportional, not scaled**. Every constant maps from the
320x200 reference onto the real surface, which is the identity at 320x200 so
mode 13h is untouched. Scaling by three would put a 960x600 stamp on a
1024x768 screen with a dead margin, and no integer factor is right for 800x600
*and* 640x480 *and* 1024x768 — all three are possible outcomes of the boot
negotiation, so nothing is hardcoded to any of them.

`gfx -t` self-tests both paths with six checks each, and they are not the same
six: half of what mode 13h asserts has no meaning on a framebuffer. The
framebuffer's include the one difference that is visible to a user — changing
a palette entry after drawing leaves the pixels alone, because the screen holds
colours there rather than indices — and the invariant this path must never
break: that `vga_mode()` is still text and no mode was switched.

`gfx -i` reports which surface the machine has, answered by the same function
the drawing uses so the report cannot drift from the behaviour.


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
and only where cells actually differ. That last clause has a trap in it, and
it bit: the comparison is only valid while the screen shows what the shadow
buffer says, and the cursor is the one thing painted over a cell *without*
being recorded there. So its cell compared equal, was skipped as already
correct, and the underline stayed behind — one mark per scroll, accumulating
along the bottom row for as long as the machine ran. The cursor is taken off
the screen before the comparison now. Measured: 0.27 ms against 1.16 ms for a
pixel move at 1024x768x32 — and the real margin is wider, because moving
pixels means *reading* an uncached framebuffer. The framebuffer is never read.

### Writing to the disk

FAT is no longer read only. `fetch -o page.txt example.com` keeps what it
downloaded, `cp` copies and `rm` removes — all three are ring 3 programs in
`/BIN`, working through four system calls that mirror the read side exactly: a
path, an offset and a length, and no file descriptor anywhere.

**This is the first code in the kernel that can destroy data.** Everything
before it was additive: a wrong packet is dropped, a wrong pixel overwritten,
a wrong page faults. A wrong sector write takes a file with it. One sentence
decided nearly every design question in `src/fat.c`:

> A leak is one `fsck -r` away; a cross-link cannot be repaired without losing
> a file.

So the ordering always falls the same way. **Deleting** writes the `0xE5`
first and frees the chain afterwards — accepting that a crash in between
leaves clusters belonging to no file. The other order would leave a live
directory entry pointing at clusters the next `create` hands out, and then two
files describe the same sectors. **Shrinking** is the mirror: the directory
entry first, then the release, because an entry claiming a size the chain no
longer backs sends a reader off the end. **Growing** commits last: data, then
chain, then entry. And a cluster is marked used before anything is written
into it, and zeroed before it is linked.

**Both FAT copies** are written, backups first and the primary last, because
the primary is what every reader uses — a failure part way then leaves the
volume behaving as it did before rather than half committed. The count comes
from the BPB rather than being assumed to be two. The one window that cannot
be closed is between two sector writes; when it happens the driver latches the
volume read-only until remount rather than piling more changes onto
bookkeeping it knows is inconsistent.

**FAT12's split entries** are the most error-prone thing in the file: a 12-bit
entry can straddle a sector boundary, so writing one is a read-modify-write of
two sectors in which the neighbouring entry sharing those bytes has to come
through untouched. It is tested in both sharing directions, with a *different
file's* chain as the neighbour that must survive, and mutation-tested —
widening either nibble mask produces about thirty failures.

The arbiter is not our own tests. `fsck.vfat -n` reports **clean, exit 0** on
volumes this driver wrote, with the FAT copies byte identical, and it was
confirmed that it does detect a deliberately introduced mismatch, so "clean"
means something. Under 120 injected write failures there were zero cross-links
and zero out-of-range clusters; the only residues were unreferenced clusters,
which is the benign side of the trade above.

Timestamps are written only when the CMOS clock reads plausibly. A zero date
is legal and every tool renders it; a *wrong* one is worse than none, because
nothing flags it — and on a machine with a flat battery it would be every file
it ever writes.

### Waiting instead of polling

Everything that waited in this kernel used to poll: sleep a few milliseconds,
look again, sleep again. It was written down as a shortcoming in five places —
the network system calls, the receive queue's drain task, `getch()`, and the
ping, DHCP and DNS loops in the shell — and it cost what it looked like it
cost.

`task_wait(channel, timeout_ms)` and `task_wake(channel)` replace it. A channel
is any address, usually of the thing being waited on; nothing is read through
it. The gain is not subtle:

| | before | after |
|---|---|---|
| `dhcp` | 105 ms | **4 ms** |
| `nslookup`, warm ARP | 50–100 ms | **3 ms** |
| `fetch` from the gateway | 78 ms | **31 ms** |
| `ping example.com` | 20 ms | **14 ms** |

**The race this had to survive** is the lost wakeup: a task tests its
condition, finds it false, and before it blocks the interrupt that would have
woken it arrives and wakes nobody. Two things close it, and both live in the
calling idiom rather than in the implementation — the condition is tested with
interrupts off, and it is tested *again* after every wake. A wake landing in
the window where the task is marked blocked but still running is then not lost;
it costs one more turn around the loop. A caller that tests once and believes
the wake will hang, rarely, and only under load.

Three things in the scheduler were less obvious than the mechanism:

**Blocking has to actually give up the CPU.** `schedule()` re-elected only when
a slice ran out, so a task blocking in the first millisecond of the console's
twenty would have sat in its halt loop for the other nineteen. It now
re-elects whenever the current task is not `RUNNING`, which covers BLOCKED,
SUSPENDED, ABORTED and EXITED in one test.

**A blocked task is dispatched when nothing else is runnable**, which sounds
like the one thing a scheduler must not do. It is safe because a blocked task
is parked in `task_wait()`'s halt loop, which re-tests its own state before
every `hlt` — so dispatching one puts the CPU straight back to halted. It is
also necessary: `sys_exit()` and the fault handler both get a dying task off
the CPU by calling `schedule()` until a *different* context comes out, and a
shell that waits for the program it spawned is very often the only other task
and is blocked. Measured with the pass disabled, every program that ran to
completion printed `No runnable task left - CPU HALT`.

**`sti; hlt` is atomic as a pair** — `sti` holds interrupt recognition off for
exactly one instruction — so "halted with interrupts disabled", the one state
from which only a reset returns, is unreachable by construction rather than by
luck.

#### The bug this uncovered

A general protection fault in the interrupt stub, roughly one per eighty
thousand timer ticks, at `pop %fs` restoring a context that was not one.

`irq_handler()` had a `cli`/`sti` pair around its call to `schedule()`. The
`cli` was redundant — an interrupt gate has already cleared IF. The `sti` was
the bug: `schedule()` commits the election before it returns, so
`current_task` already names the *incoming* task while the CPU is still on the
*outgoing* task's kernel stack, and stays that way until the stub reloads
`esp` a few instructions later. Enabling interrupts inside that window, and
then writing the EOI so the PIC may deliver again, let a tick that had become
pending during the handler arrive exactly there. It pushed its frame onto the
outgoing task's stack and called `schedule()` again, which filed the *outgoing*
task's registers as the *incoming* task's context. That context was then a
pointer into a stack whose owner kept using it, so ordinary calls overwrote it,
and the next dispatch of it faulted.

The window is old. What changed is that a task used to hold its slice for up to
twenty ticks, so most ticks re-elected the same task and the window did not
matter; a blocked task is descheduled on the very next tick, so nearly every
tick now changes `current_task`. Same latent race, an order of magnitude more
likely to land on the case that corrupts.

Both halves are fixed: the `sti` is gone, and `schedule()` also detects being
entered nested and hands the frame back untouched, so a window like this one
cannot corrupt anything if it is ever reopened. `SCHED_DEBUG` in `tasks.c`
traces turned-away nested elections and validates every frame before it is
restored, writing to QEMU's debug port rather than the console — which belongs
to whoever was interrupted, and anything that scrolls loses the record.

### Networking

`ping` works, in both directions. The run targets attach an **RTL8139** to
QEMU's user mode network, which needs neither root nor a TAP device — QEMU
emulates a small network in userspace behind a NAT. The addresses in it are
fixed:

| Address | |
|---|---|
| `10.0.2.15` | the guest |
| `10.0.2.2` | gateway — answers ARP and ICMP echo |
| `10.0.2.3` | DNS forwarder |
| `255.255.255.0` | netmask |

There is a DHCP client, so the address does not have to be typed in:

```
dhcp
ping 10.0.2.2
```

`dhcp` prints the exchange as it happens — DISCOVER, OFFER, REQUEST, ACK — and
then what the lease says. `ifconfig 10.0.2.15 255.255.255.0 10.0.2.2` still
works and still wins; `ifconfig` afterwards shows which of the two the address
came from.

The lease also names a DNS server, so names work:

```
nslookup example.com
ping example.com
```

`ping` takes a name or an address. `nslookup` alone shows the cache; `-f`
empties it.

And with TCP there is something to fetch:

```
fetch example.com
fetch 10.0.2.2 /index.html 8080
```

`fetch` is **not a shell command** — it is `/BIN/FETCH.ELF`, a ring 3 program
that opens the connection itself through system calls. `netstat` shows what is
open, which is the kernel's business and stays in the kernel.

`lspci` lists what the bus enumeration found — the command to run when the
card is not detected. `arp` shows the cache, `ifconfig` without arguments the
interface and its counters.

`make run NET=0` runs without a card. That is a normal state, not a failure:
the stack reports why it is down instead of showing zeros. `make run
NETDUMP=1` writes every frame to `build/net.pcap`, readable with Wireshark or
`tcpdump -r` — that capture is the arbiter when a packet leaves the kernel and
nothing answers, since it shows whether the frame reached the wire at all and
whether its byte order is what you think it is.

Five layers, and a few decisions worth knowing about:

**PCI enumeration walks from bus 0 through the bridges** rather than sweeping
all 256 buses — 163 configuration reads instead of 65536 on a two-bus machine,
and it never asks a chipset about buses it does not implement. Configuration
space is dword addressed, so a 16-bit access reads the containing dword and
shifts; a 16-bit *write* is read-modify-write, because writing a whole dword
to the command register would clear the status register sharing it.

**Everything the card sees is a physical address.** The RTL8139 does not go
through the MMU. Its buffers come from the frame allocator, which hands out
physical addresses directly, and every kernel access to them goes through
`P2V()`. The receive ring is 32 KiB plus the 16 bytes the hardware demands and
1536 bytes of slack, with `RCR.WRAP` left clear — the card then runs a frame
past the end of the ring into that slack instead of splitting it, which is why
the read path has no wrap case at all. `CAPR` carries its documented 16-byte
offset; getting that wrong is the classic driver that receives exactly one
packet and then goes quiet.

**ARP entries expire after 120 s**, an unanswered request after 3 s, and
requests for one address are rate limited to one per second. The contract is
that the caller retries, so without that limit a polling loop turns into a
broadcast storm. Every ARP packet on the wire is learned from, but an
overheard one never evicts a valid entry.

**IP addresses are in host byte order everywhere outside a packed header.**
Conversion happens once per field, at the header boundary. Fragments are
dropped rather than silently mishandled — `DF` alone passes, it is a sender's
instruction and not a fragment marker. The ICMP checksum spans the whole
message, IP's only its own header.

The protocol work runs **in the card's interrupt handler**, which holds only
because every path from it is short and finite: no printing, no allocation, no
waiting. A receive queue drained by a task would be the better structure and
is noted as such in `net.c`; it needs a task and an overflow policy, and
neither belongs in the same change as the first packet that works.

#### The receive queue

Every protocol added after the first ping ran inside the card's interrupt
handler, and that held only because each path was short and finite. It stopped
being true: a DHCP option walk, a DNS name with compression pointers and a TCP
state machine are not short in the sense that argument needed — and a bug in
any of them is a hung *machine*, not a hung process, because the handler runs
with the whole system waiting.

So `net_receive()` now checks the frame is addressed to us, copies it into a
queue and returns. `net_queue_drain()` does the protocol work afterwards, in a
task, with interrupts on.

The queue is a **byte ring with length-prefixed frames**, 32 KB from the heap.
An array of 1518-byte slots would be nine tenths padding for what actually
arrives — acknowledgements, ARP replies and DNS answers are 60 to 100 bytes —
and the same memory holds 21 full frames *or* some five hundred small ones.
Both indices are free-running byte counters and occupancy is their unsigned
difference, the idiom `tcp.c` already uses for its rings, so there is no
full/empty ambiguity and no wasted byte. A record that runs past the end is
split rather than skipped: skipping needs a marker, and a marker is a length
that is not a length, which every reader would then have to know about.

**Neither side masks interrupts on the common path.** The interrupt writes the
head and the frame; the task writes the tail. Neither writes the other's, each
reads the other's exactly once into a local, and every stale read is
conservative. Publication order is length, bytes, then head. The drop counter
is two counters, one per context, summed by `net_rx_dropped()` — for exactly
the same reason.

**Overflow drops the newest** and counts it separately in `net_rx_overrun()`.
Dropping the oldest would mean the interrupt moving the *consumer's* index,
which would cost the whole lock-free argument; and losing the tail of a burst
is what wire loss looks like anyway, which is what TCP is built to handle.

Two things this cost, and what was done about them:

**The boot.** Starting the drain task inside `net_init()` strands the machine.
`network_init()` runs on the boot stack before the console task exists, and
`schedule()` saves no context while `current_task < 0` — so the first tick
after any task becomes runnable throws the boot away. The machine came up with
a working network and no shell. The task is created there but made runnable
later, at the first moment there is a current task to switch away from.

**Latency.** A ping's round trip went from 0 ms to 34 ms, and no sleep interval
fixes that: a task holds its slice even while sleeping, so the gap between
drains is the sum of the other tasks' slices. The answer is not to poll faster
but to notice that a task waiting for a packet may as well take it out of the
queue itself — `net_queue_drain()` is written for task context and refuses
re-entry, so the ping, DHCP, DNS and system-call wait loops all call it. Back
to 0 ms on the local network, and 20 ms to a host on the internet, which is
the network rather than the machine. The drain task stays: it handles what
nobody is waiting for.

One thing the queue does **not** remove is concurrency. It changes which kind:
the receive path can no longer preempt a send half way through, but the drain
task can still be preempted by the timer while another task is inside
`ip_send()`. That turned up a real defect — `ip_tx_busy` was tested and set in
two separate steps, which was safe against an interrupt (it runs to completion
and clears the flag before the interrupted instruction resumes) and is not safe
against another task, which can be preempted mid-packet. It is taken under
`cli` now, for two instructions rather than for the whole packet.

#### UDP and DHCP

The point of the DHCP client is that a machine on an unfamiliar network can
find its own address. Three things about it are worth knowing, because each is
a case that fails silently rather than loudly.

**The UDP checksum covers a pseudo header that never appears on the wire** —
source address, destination address, protocol and length — and the length is in
it twice, once there and once in the real header. Summing only the visible
bytes produces a datagram that looks correct in a hex dump and is discarded by
every peer. UDP also allows a checksum of zero, meaning "not computed", so a
computed zero has to go out as `0xFFFF` and an incoming zero must be *accepted*
rather than rejected.

**The whole exchange happens before the machine has an address.** Requests go
out from `0.0.0.0` to `255.255.255.255`, and the answers arrive addressed to a
machine that is not us yet. `ip_receive()` drops anything not addressed to us —
correctly, and that is exactly what would drop the replies. So while
`net_ip()` is still zero the receive path accepts any destination, and stops
the instant `net_configure()` runs. Two things bound that: the ethernet layer
above it already discards frames addressed to neither our MAC nor broadcast, so
a unicast reaching it was sent to *this card*; and ICMP was deliberately not
widened, so a broadcast ping is still heard and ignored rather than answered.

Sending had a defect the same work uncovered: a limited broadcast from a
machine that *does* have an address is off-subnet by definition, so it was
routed to the gateway and would have gone to the router's MAC — a broadcast
delivered to one station. It is short-circuited before the subnet test now,
which is what a lease renewal after expiry needs.

**The option field is a walk, not a struct**, over type/length/value triples
that came off the wire and that nothing has vouched for. Each read is bounded
before it happens, and a bad length *stops* the walk rather than skipping the
option — once a length is wrong there is no way to know where the next option
begins. It was fuzzed with 500,000 random option areas under ASan, each copied
into an exact-size allocation so the redzone sits immediately after the last
legal byte.

Retransmission is 1 s, 2 s, 4 s, 8 s and then give up. Sub-second is useless
anywhere but loopback — a relay agent, or a switch running spanning tree on a
port that just came up, takes longer — and a flat retry would put fifteen
broadcasts on a segment that has already shown nobody is listening.

#### DNS

The resolver is small except for one function, and that function is the reason
this is not just filling in a struct.

**A name on the wire is not a string.** It is length-prefixed labels ending in
a zero byte — and a label whose top two bits are set is not a label at all but
a **pointer** to an earlier offset in the same message. That is what keeps a
reply with several answers small, and it means the decoder follows offsets that
arrived from the network. A pointer to itself, or two pointing at each other,
is an infinite loop **inside an interrupt handler**, which on this kernel is a
hung machine rather than a hung process.

The bound is arithmetic rather than a counter, which is what makes it
trustworthy. A pointer target must be strictly *before* the pointer itself, and
also strictly before the *previous* target. The second rule is the load-bearing
one: going backwards is not enough, because a pointer at 300→100 whose labels
run forward into a pointer at 400→200 whose labels run forward to 300 again is
a cycle in which every jump goes backwards. With targets strictly decreasing
and all below the 512-byte message limit, there can be at most 512 of them —
termination follows from the arithmetic and not from the data being friendly.
It refuses no legal message either, since a compressor can only point at a name
it has already emitted. A jump cap bounds the *work* on top of that.

That claim was checked by mutation: remove either rule alone and the tests
still pass, because another catches it; remove all three and the test **hangs**
— so the suite genuinely exercises the case.

**A CNAME chain is the ordinary case**, not an exception. Each pass looks for
an A record whose owner is the name currently being chased, and only follows a
CNAME if there is none — taking any A record in the message would mean
accepting an address for a name nobody asked about. The reported TTL is the
minimum along the chain.

**A truncated reply is reported, not used.** The TC bit means the answer did
not fit and the protocol wants the query repeated over TCP; there is no TCP
here, and what arrived is the beginning of an answer rather than a short one.

Matching a reply uses the id, the ephemeral source port, the server address and
the echoed question. With no random source on this machine that is roughly 30
bits of material, and the code says so plainly rather than implying more.

#### TCP

Everything else in this stack is a message: an ARP request, an echo, a DHCP
offer, a DNS answer. Losing one means asking again. TCP is different in kind,
and that is why it is the largest piece here: the two ends have to agree on
what has arrived, keep agreeing while packets go missing, and take the
connection down in a way that survives the last message being lost.

**Sequence numbers wrap**, and that shapes every comparison. A 32-bit counter
passes 4 GB and starts again, so `a > b` is wrong; it is written
`(int32_t)(a - b) > 0`, which holds as long as the two are less than 2 GB
apart — and they always are, because a window is at most 64 KB. A plain `>`
is a bug that hides for hours of transfer and then corrupts a stream. That the
implementation contains none was established three ways: a mechanical audit of
all 76 lines containing a relational operator, a reference test whose cases are
built so the true ordering is known independently (and which asserts that 1024
of its 4800 cases are ones a plain `<` gets wrong, so it cannot pass for the
wrong reason), and mutation testing — 20 of 21 mutants caught, the survivor
provably equivalent.

**Closing is not one event.** Each side closes its own direction, so a
half-open connection is normal — it is exactly what an HTTP client does after
sending its request. The side that closes first then waits in `TIME_WAIT`,
because its final acknowledgement may have been lost and the peer may resend a
FIN that would otherwise reach a machine which has forgotten the connection.
`netstat` says so under the table, because a connection sitting there looks
stuck and is not.

Three bugs from this work are worth recording, because each was invisible to a
test that looked reasonable:

**One byte too many at the end of every stream.** `rcv_nxt` is the end of the
*sequence space* and is advanced by the peer's FIN, since a FIN occupies a
sequence number without being data. Computing the readable amount from it
handed out one stale byte from the ring — after the FIN, and only then. Split
into `rcv_end` (end of the data) and `rcv_nxt` (end of the sequence space):
what talks to the peer uses one, what talks to the caller uses the other.

**A FIN refused by our own closed window.** A stream whose last byte exactly
fills the receive buffer leaves the window at zero, and RFC 793's literal
acceptability test then rejects the bare FIN as a one-octet segment against a
shut window — adding a full retransmission timeout to the end of every such
response.

**Acknowledgements thrown away as being for bytes we had not sent.** The
control block was updated *after* `ip_send()` returned. On a virtual link the
peer answers in a tenth of a millisecond, so the card's interrupt runs
*inside* that call, and the acknowledgement was judged against a `snd_nxt`
that still described the state before the segment. It was the only
acknowledgement those bytes would ever get, so the data was retransmitted and
every connection ended in a RST instead of a clean close. The control block is
now brought up to date before the segment leaves.

The third one is the interesting one: **the host test harness could not
produce it**, because a scripted peer answers *between* calls into the module
while a real card answers *inside* one. The harness gained a hook that fires
the interrupt from within `ip_send()`, and that is where a third of its fuzz
traffic now goes.

Deliberately absent: passive open (this connects, it does not listen), out of
order reassembly, Nagle, delayed acknowledgement, window scaling, selective
acknowledgement, urgent data, congestion control beyond a retransmission timer,
renewing a DHCP lease, RELEASE, DECLINE, AAAA and everything but A records,
reverse lookups, and any form of fragment reassembly.

### What is in the kernel, and what is not

A standing question in this project: what actually gets compiled into the
kernel, and what could live outside it. Two things make the question
answerable rather than rhetorical — there is a filesystem to put programs on,
and there are system calls for them to work through.

The `.bss` went from **341 KB to 67 KB**, and almost all of that was one array:

**Kernel stacks are allocated per task.** `task_kernel_stacks[64][4096]` was
256 KB reserved from boot on a machine that runs five tasks. A stack now comes
from the frame allocator when a task is created. Two properties decide whether
that works. It has to be page aligned, because the saved `struct regs` is
carved out of the top and read as 32-bit words — which rules `malloc` out, and
would in any case put a neighbouring heap header directly under the stack, the
one thing an overflow must not reach quietly. And it has to be visible in
**every** address space, because an interrupt can arrive while any task is
running: that is a page-table question, not an address one. `vmm_init()` maps
all usable RAM before any space exists and `vmm_create_space()` copies
directory *entries*, so every space walks into the same kernel page tables and
a stack allocation can never need a new one.

Freeing is the part that bites. `taskmgr_task_abort()` runs **on the stack of
the task it is aborting**, and the task keeps running until the next tick
elects somebody else — so nothing is freed there. The only real release is
slot recycling, and a stack that is still live is parked rather than freed, the
same answer `task_space_release()` already gives for an address space still
loaded in CR3.

Allocating separately also made a **guard page** possible, which a static array
could not have: the page below each stack is unmapped, so a kernel stack
overflow faults instead of silently eating the next task's saved registers. On
x86-32 a ring 0 fault does not switch stacks, so the machine triple-faults and
resets — still far better than corrupting a page table and continuing.

**`ls` and `cat` are programs now**, `/BIN/LS.ELF` and `/BIN/CAT.ELF`. They
were pure filesystem formatting, and `SYS_READDIR`, `SYS_STAT` and `SYS_READ`
do everything they need. The immediate saving is small — what left is largely
balanced by the machinery that arrived — but that machinery is a one-off, and
the next command to leave subtracts cleanly.

**What deliberately stays.** Most of the shell cannot move and should not:
`page` walks page tables, `mem` drives the allocators, `user` is a ring 3
self-test wearing a command's clothes, and `gfx`, `lspci` and the network
commands reach hardware with no system call behind it. A command that needs
kernel internals belongs in the kernel; moving it would mean inventing a
system call that exports those internals, which is worse than the problem.
`df` stays for a smaller reason: it reports the ATA drive table as well as the
filesystem, and only the filesystem half has a call.

Smaller items in the same pass:

- The framebuffer console's shadow buffer was dimensioned for 1280×1024, a
  mode **this bootloader cannot produce** — `vbe_res_table` in `boot/vbe.inc`
  tops out at 1024×768. The constants derive from that now: 8 KB less, and 20
  percent off every scroll, which moved whole rows of the old width.
- `start.asm` had 4088 bytes of pure alignment padding between the boot stack
  and the page directory. Same footprint, stack half again as large.
- The DJGPP `stdarg.h` and the 30 unused headers under `src/include/sys/` are
  gone, replaced by seven lines over GCC's own builtins. That immediately
  exposed a bug the old macros hid: `va_arg(argptr, char)` in the kernel's
  `printf`. Nothing smaller than an `int` survives the trip through `...`, so
  there was no `char` on the stack to fetch — the old header rounded every size
  up to `sizeof(int)` and made it look correct.
- `src/math.c` was dead, and its `pow()` was wrong: `pow(2,3)` returned 4.

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
src/include/    own headers
user/           ring 3 programs and their C library, built separately
linker.ld       linker script, loads at 1 MiB
bin/            the original GRUB Legacy floppy image (template, never modified)
legacy/         the 2011 Windows toolchain (DJGPP, VFD) and its batch files
```

Sources are UTF-8 with LF. The build uses `-std=gnu89`; under newer standards
the implicit declarations of the original code become hard errors.
