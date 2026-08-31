# Booting TomatOS from a USB stick

This is the procedure for putting TomatOS on a stick and starting a real
machine from it, written for a **ThinkPad T430** because that is the machine
it was developed against. Nothing in the boot chain is Lenovo specific — any
BIOS with a working CSM should behave the same — but the BIOS settings below
are named the way Lenovo names them, and the port advice is about that
chassis.

Read the section *What a successful boot looks like* before you start, because
the single most surprising thing about this is that **a correct boot ends with
no filesystem mounted and `df` reporting nothing**. That is the expected
outcome on a T430, not a failure, and knowing it in advance saves an hour of
debugging something that is working.

There is no GRUB here and no EFI stub. `build/tomatos_boot.img` carries the
whole chain itself: stage 1 in the boot sector, stage 2 and a flat kernel and
the user programs in the FAT volume's reserved sectors, and an MBR partition
table so that a BIOS is willing to call the stick a hard disk. See *Booting
without GRUB* in the README for what each of those pieces does.


## Writing the stick

Two commands:

```sh
make bootdisk
make usb-boot DEV=/dev/sdX
```

`make bootdisk` produces `build/tomatos_boot.img`, a 32 MB image, and prints
the layout as it writes it. A good build ends like this:

```
  IMG     build/tomatos_boot.img  (32 MB, FAT16, 1040 reserved sectors)
  BOOT    LBA 0        stage 1  (jump + code, BPB preserved)
  BOOT    off 446  partition 1  active, type 0x0e, LBA 0 + 65520, CHS 0/0/1 - 64/15/63
  BOOT    LBA 1        stage 2  (4608 bytes)
  BOOT    LBA 17       kernel   (229376 bytes, flat)
  BOOT    LBA 528 ..    7 modules, 207 / 512 sectors
  BOOT    7 modules verified against build/*.elf, byte for byte
  Boot image ready: build/tomatos_boot.img   (BPB intact, 0x55AA present)
```

That last-but-one line is worth more than it looks. The build extracts every
module back out of the finished image by its recorded LBA and length and
compares it with the ELF it came from, so "the image is not what the module
table says it is" is a build failure rather than a mystery at the prompt.

### Finding the right device

```sh
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
```

You want the row where `TRAN` is `usb` and the size matches the stick you just
plugged in, and you want the **whole disk** — `sdc`, not `sdc1`. Pass it as
`DEV=/dev/sdc`.

Get this wrong and `tools/usb-boot.sh` will tell you so. It refuses six
different mistakes, all of them **before** it asks for confirmation, because
the prompt is the weakest check in the list — somebody who has decided to type
`yes` will type `yes`. What each refusal looks like:

| If you pass | It says | Why it is refused |
|---|---|---|
| nothing | *no target device given* | there is deliberately no default device |
| a path that is not a block device | *dd would create it as an ordinary file and report success* | the write would "succeed" and the stick would still be empty |
| `/dev/sdc1` | *is a partition, not a whole disk* | the boot sector would land at the start of the partition, and the BIOS reads LBA 0 of the **disk** |
| an internal disk | *is not a removable device* | `/sys/block/<name>/removable` says 0; this one is not overridable |
| a card reader or a hot-swap bay | *is not on the USB bus* | removable and USB are not the same thing, and a hot-swap bay can hold the machine's own disk |
| a stick with something mounted | *has mounted partitions* | writing under a live mount corrupts it in both directions; it prints the `udisksctl unmount -b …` line and stops rather than unmounting somebody else's open files |

Only then does it show `lsblk` for the device, the image size, and ask. The
answer has to be exactly `yes` — not `y`, not `Y`, not a bare Return. Those
three extra keystrokes are the point.

The write itself is `sudo dd … bs=4M oflag=sync conv=fsync` followed by a
`sync`, so when it says *It is safe to remove*, it is: `oflag=sync` is also
what makes `status=progress` tell the truth instead of racing to 100 % and
then sitting in `close()`.

### What the host tools will say about the finished stick afterwards

Do not be alarmed by any of this — it is all expected:

```
$ blkid -p build/tomatos_boot.img
… LABEL="TOMATOS" VERSION="FAT16" TYPE="vfat" USAGE="filesystem"

$ partx --show build/tomatos_boot.img
partx: … cannot read partition table

$ fdisk -l build/tomatos_boot.img
Device                   Boot Start   End Sectors Size Id Type
build/tomatos_boot.img1  *        0 65519   65520  32M  e W95 FAT16 (LBA)
```

The FAT boot sector **is** the MBR here, so the one partition starts at LBA 0
and contains its own table. `blkid` and `partx` refuse to see a partition
table on anything whose first sector is a filesystem; `fdisk` reads the entry
and prints it. The BIOS is the one consumer whose opinion matters, and it
looks at bytes 446..509 without asking Linux what it thinks.


## What the ThinkPad's BIOS has to be set to

Power on and press **F1** at the ThinkPad splash for setup. **F12** at the
same moment gives a one-shot boot menu instead, which is what you want once
the settings are right — it avoids changing the boot order permanently.

The settings below and their menu paths are taken from `t430-risks.md`, which
another agent wrote against this machine. **I could not verify any Lenovo menu
name or its behaviour** — there is no T430 attached to the machine this was
tested on, and QEMU's SeaBIOS has none of these options. Where a claim is
mine and checkable, it says so.

### `Startup` → `UEFI/Legacy Boot` = **Legacy Only**

This is the setting to change first, and the one that makes the difference
between "it boots" and "nothing at all happens".

TomatOS has no EFI stub of any kind. Stage 1 is a 512 byte MBR-style boot
sector that expects to be loaded at `0x7C00` in real mode with the boot drive
in `DL` (`boot/stage1.asm`). Under **UEFI Only** the firmware does not look at
that sector at all — it looks for an EFI system partition and an
`\EFI\BOOT\BOOTX64.EFI`, finds neither, and moves on.

The interesting choice is **Legacy Only** versus **Both**, and the argument
for Legacy Only is about *diagnosis*, not capability. With `Both` a second
setting appears, `UEFI/Legacy Boot Priority`, and the firmware is then free to
prefer an EFI path it finds somewhere — including on the internal disk. The
failure that produces is: the machine boots Windows or Linux from the internal
drive, silently, with no error and nothing that looks like TomatOS having been
tried. That is indistinguishable from a dozen other faults. With **Legacy
Only** there is no UEFI path to be preferred, so a stick that is not offered
is genuinely not being seen, and you are debugging the right problem.

Some BIOS levels also expose `Startup` → `CSM Support`; if it is there, it
must be **Yes**. The CSM is what provides int 13h, and int 13h is the entire
mechanism by which stage 1, stage 2, the kernel and all seven modules reach
memory.

### `Security` → `Secure Boot` = **Disabled**

Secure Boot only applies to UEFI booting, and on this machine it is only
offered while UEFI is enabled — so setting `Legacy Only` normally settles it
by itself. Check it anyway. A machine left half-configured can end up with
Secure Boot on and legacy boot quietly refused, and that failure looks exactly
like the `UEFI Only` one above.

### `Config` → `USB` → `USB UEFI BIOS Support` = **Enabled**

The name varies by BIOS level; on older ones it is `USB Legacy Support`. On
many machines this would be the first thing to check for a dead keyboard, and
**on a T430 it is not**, which is worth being precise about: the built-in
keyboard and TrackPoint are not USB. They hang off the embedded controller and
appear as genuine 8042 PS/2 devices at ports `0x60`/`0x64`, which is what the
kernel's own keyboard driver talks to. The console works whether this setting
is on or off.

Leave it **Enabled** anyway, for a different reason: on many BIOSes the same
firmware feature that emulates a PS/2 keyboard for a USB one is also the one
that publishes USB mass storage as an int 13h drive. Turn it off and the stick
can vanish from the boot menu entirely. There is no upside to disabling it and
one plausible way for it to break the boot.

One genuine hazard it creates, and the only reason to touch it: while SMM is
emulating a PS/2 keyboard for a USB one, an SMI fires on accesses to port
`0x60`. If an **external USB keyboard** is plugged in, the kernel's 8042
driver and the firmware's SMM handler are both driving the same controller.
Nobody has tested what that does here. If the keyboard behaves erratically,
unplug everything except the boot stick before suspecting the kernel.


## Which port to use

The T430 has three USB-A ports: **two on the left, which are USB 3.0**
(one of them the yellow Always-On port), and **one on the right**, next to the
VGA and eSATA connectors, **which is USB 2.0**. Internally the QM77 chipset
has one xHCI controller and two EHCI controllers, and the firmware decides
which one owns which port.

**Use the right-hand USB 2.0 port first.** For int 13h this should not matter
— the CSM initialises both controllers and implements int 13h against whichever
one the stick turned up on — but "should not matter" and "does not matter" are
different claims about a 2012 Lenovo CSM, and a stick that boots from one port
and not another is a known class of bug of that generation, usually on the
xHCI side. The USB 2.0 port is the better-tested path and it is also the one
whose controller a future EHCI driver would target.

If it boots from the right and not from the left, the setting that tells you
so is `Config` → `USB` → `USB 3.0 Mode`, which offers **Auto**, **Enabled**
and **Disabled**. Setting it to **Disabled** makes the firmware route the
left-hand ports through EHCI instead of xHCI. If that turns "boots on the
right only" into "boots from any port", the problem was the CSM's xHCI path,
which is a useful thing to know even if `Disabled` is not the setting you want
to keep. (Cited from `t430-risks.md`; not verified here.)

Once the kernel is running, the port makes no difference at all, because the
kernel drives neither controller. See below.


## What a successful boot looks like

There are four things on the screen, in this order.

### 1. The firmware, then a moment of nothing

The CSM loads LBA 0 and jumps to it. **Stage 1 prints nothing when it
succeeds** — it has exactly one message, `S1 disk error`, and 384 bytes to
work in between the end of the BPB at offset 62 and the partition table at
446, so silence is the success case. Expect a blink and then stage 2.

### 2. Stage 2, six short lines

```
TomatOS stage2 (hold shift for text mode)
a20 ok
e820 ok
load ok
mods ok
vbe
```

Each word is printed *before* the step it names and the `ok` after it, so if
the machine stops here the last word on the screen is the step that failed —
that is the whole design of the progress output. `vbe` has no `ok`, because a
successful mode set replaces the text screen with a framebuffer and the
teletype output disappears with it.

The other things that line can say:

| Line | Means |
|---|---|
| `vbe text` | there is no usable VBE at all; the kernel comes up on 80x25 |
| `vbe safe` | there *is* a usable VBE and you told stage 2 not to use it — you held shift |
| `mods none` | stage 2 found no module table in its own image. Fine on an image built without programs, a diagnosis on one that was supposed to have them |

**Read the banner once and remember it**, because it is the only place the
escape hatch is ever advertised, and the day you need it the screen will be
black. Holding **either shift key down while the machine powers on** makes
stage 2 skip the VBE mode set entirely and hand the kernel an ordinary 80x25
text screen. It is sampled from the BIOS keyboard shift-state byte at
`0040:0017` at the very top of stage 2, before the kernel and the modules are
read, so it catches the state you set up before pressing the power button —
nobody holds a key for the whole of a USB load. It also **beeps**: the
confirmation message opens with a BEL, which the BIOS teletype call sounds on
the PC speaker, and on a machine whose panel shows nothing that beep is the
entire user interface.

*(These lines are quoted from the message strings in `boot/stage2.asm`,
confirmed present in the built `build/stage2.bin` with `strings`. They are on
screen for a fraction of a second before the mode switch wipes them, and I did
not manage to photograph them in QEMU — several attempts at stepping the guest
and reading `0xB8000` never caught the window. The shift-held path in
particular could not be exercised at all: QEMU has no way to hold a key down
through POST.)*

### 3. The kernel

Everything from here on **is** quoted from a real run: the image built by
`make clean && make all bootdisk` on 2026-09-01, booted as a `usb-storage`
device behind a `usb-ehci` controller — one of the two shapes a T430 has —
with the `t430.py` harness. The prompt arrived **1.40 s** after QEMU started.

```
TomatOS/x86 boot v0.2
Higher half: kernel 0xC0100000 virt = 0x100000 phys
Intel Specific Features:
...
Memory map (usable):
  0x0  639 KiB  type 1
  0x100000  64376 KiB  type 1
  Total: 65015 KiB (63 MiB) in 2 regions
PMM: 65015 KiB usable, 16350 frames of 4 KiB, 15982 free
VMM: paging on, 16350 pages in 20 tables, page 0 left unmapped
Framebuffer: RGB 1280x800 32 bpp, pitch 5120, at 0xFD000000 (outside the direct mapping, console active)
63 MB Memory (65015 KB) in 16350 Frames
Modules: 7 (hello, ls, cat, fetch, rm, cp, gui)
Loading TomatOS/x86
Loading Driver Components.
...
Protected Mode Kernel Running.
PCI: 7 device(s) on 1 bus(es)
USB: 1x EHCI found, this kernel drives UHCI only
net: no network card, stack stays down
eltomato's TomatOS 0.31 [Version 0.31 Build 2011/27/09]
(c) Copyright 2006-2011 Jens K.hler
@TomatOS>
```

The resolution in the `Framebuffer:` line will be whatever the panel asked
for, not a fixed number: stage 2 now reads the display's EDID and refuses
modes larger than the preferred timing it reports, which is why the run above
came out at 1280x800 rather than the 1920x1080 QEMU's mode list also offers.
On the T430 expect the panel's own 1366x768 or 1600x900. What matters is that
the line is there at all.

Three lines are the ones to read.

**`USB: 1x EHCI found, this kernel drives UHCI only`.** On a T430 expect this
or its xHCI twin, `USB: 1x xHCI found, this kernel drives UHCI only`. This is
the kernel correctly reporting that there is USB hardware present and it is
not hardware this driver speaks. It is a limit of the driver, not a fault of
the machine, and `lsusb` will say the same thing at greater length along with
the PCI address and ID of the controller it found. **If you instead see
`USB: UHCI, 2 root port(s)`, you are not on a T430** — that is the QEMU
control configuration.

**`Modules: 7 (hello, ls, cat, fetch, rm, cp, gui)`.** This is the line that
decides whether the machine is usable. Those seven programs were read off the
stick by *stage 2*, through int 13h, while the BIOS was still available, and
handed to the kernel as Multiboot modules — exactly the way GRUB would have.
They are the only commands the machine will have. A count lower than seven, or
`Modules: 0`, means stage 2's module table and the sectors disagree, and the
place to look is the `make bootdisk` output quoted at the top of this
document.

**`df` says nothing is mounted**, and *this is the correct outcome*:

```
@TomatOS> df
Filesystem:
  Nothing is mounted -- no path can be read.
Drives the ATA driver found:
  None. Either no controller answered or nothing is attached
  to it -- booting without a disk is a normal case here.
```

Two independent reasons, both of them permanent facts about this machine:

- The kernel's `ata.c` speaks **legacy IDE ports** over programmed I/O. The
  T430's SATA controller runs in AHCI and does not decode those ports at all,
  so reads return `0xFF` and the driver gives up. There is nothing wrong and
  nothing to fix by fiddling with the BIOS.
- The kernel's USB mass storage driver reaches the bus through **UHCI**, and
  the QM77 chipset has EHCI and xHCI and no UHCI whatsoever. Panther Point
  dropped it. So the stick the machine just booted from is, to the running
  kernel, unreachable.

**The kernel cannot read the medium it booted from.** That is the entire
reason the modules exist. A version of this that expected to find `/BIN` on
the stick would come up on a T430 with a shell and no commands at all — which
is exactly what happened during development, and why `boot/layout.inc` spends
a page explaining it.

So on the real machine, `ls` and `cat` will *run* and then correctly tell you
there is no filesystem:

```
@TomatOS> ls
ls: no filesystem is mounted.
    Either no disk was found or nothing on it could be read --
    booting without a disk is a normal case here, not a fault.
    "df" shows what the drivers did find.
Task 3 exited with status 1
```

That message is printed by the ring 3 program `ls`, which was loaded from a
module, given its own address space and run. Seeing it is *proof the module
mechanism works*, not evidence that something is broken.

### 4. Something that actually does something

`hello` is the one to type, because it needs nothing from the outside world:

```
@TomatOS> hello
Hello from user space! This is a real ELF with its own address space.
  pid       : 2
  argc      : 1
  argv[0]   : hello
  uptime    : 16599 ms since boot
  ticking   : ..........
  slept     : 2002 ms (asked for 2000 ms)
Goodbye - exiting with status 42.
Task 2 exited with status 42
```

Note `argv[0] : hello`, lower case and without an extension. That is the
module's name from stage 2's table. When the same program is loaded from a FAT
volume instead, `argv[0]` reads `HELLO.ELF` — the 8.3 name off the disk. So
that one field tells you, at a glance, which of the two paths the program came
down, and on a T430 it must read `hello`.

`help` lists what is available, `ps` shows the modules, and `mem`, `page`,
`taskmgr` and `gfx -i` all work — they are in the kernel and need no
filesystem.


## When it does not work

Organised by what you see, because that is what you have.

### Nothing happens — no TomatOS text, the machine boots as usual

The stick was never started. Almost always the BIOS boot mode.

1. F1 → `Startup` → `UEFI/Legacy Boot`. It must read **Legacy Only**. If it
   reads `Both`, the firmware may have preferred an EFI path on the internal
   disk and never looked at the stick.
2. `Security` → `Secure Boot` = Disabled.
3. If the BIOS level has it, `Startup` → `CSM Support` = Yes.
4. Then F12 and pick the stick explicitly rather than relying on the boot
   order.

### The stick is not offered as a boot device at all

Now it is about how the CSM classifies the device, not about boot mode.

1. In F12, look at **which heading** the stick is listed under. `USB HDD` is
   what the partition table is there to produce. `USB FDD` means the CSM
   decided it is a floppy; that should still boot, because stage 1 stashes
   whatever `DL` the BIOS handed it (`mov [drive], dl`, `boot/stage1.asm:73`)
   and reloads it before every int 13h call rather than assuming `0x80` — but
   this specific case has never been exercised on real hardware, because QEMU
   always presents a `usb-storage` device as a hard disk.
2. Move the port. Right-hand USB 2.0 first, then a left-hand one. Then set
   `Config` → `USB` → `USB 3.0 Mode` = `Disabled` and try the left ports
   again.
3. Check `Config` → `USB` → `USB UEFI BIOS Support` is **Enabled**. If
   somebody turned it off, the firmware may have stopped publishing USB mass
   storage as an int 13h drive, and the stick disappears from the menu.
4. `Startup` → `Boot` — make sure `USB HDD` is in the list and enabled, and
   move it above the internal drive.
5. **Try a different stick.** Some Lenovo CSMs of this era refuse USB 3.0
   flash controllers in legacy mode outright. An old, small, slow USB 2.0
   stick is the best thing to test with.
6. Re-verify the write. Read the first megabyte back off the stick and
   compare it with the image:
   `sudo cmp -n 1048576 build/tomatos_boot.img /dev/sdX`. Silence means they
   match. A difference at byte 1 means the write went to the wrong device or
   to a partition rather than to the disk.

### It starts, then the screen goes black

**This is the most likely way a fully working boot looks like a hang.** The
first thing to do is not to reboot but to get the picture back, and there is
now a one-key way to do that.

**Power the machine off, hold either shift key down, and switch it back on.**
Keep it held until you hear a beep or see text. Stage 2 samples the BIOS
shift-state byte before it does anything else and, if a shift key is down,
skips the VBE mode set entirely and hands the kernel a plain 80x25 text
screen. If the machine comes up in text mode, **the kernel was fine all along
and the problem is the video mode**. That is the diagnosis and the workaround
in the same gesture.

Why this happens at all: `boot/vbe.inc` does not request a particular mode. It
enumerates whatever the video BIOS lists, scores each one, and takes the
winner. It now also reads the display's EDID with `int 10h AX=4F15h` and
refuses any mode larger than the preferred timing the panel reports — which is
the right fix and is why the QEMU run above came out at 1280x800 instead of
1920x1080. But an Intel HD 4000 legacy VBIOS lists 1920x1080 because the
machine has VGA and DisplayPort outputs that can drive it, the internal LVDS
panel is 1366x768 or 1600x900 and cannot, and the EDID is exactly the thing a
machine in this state is most likely to be lying about or not providing at
all. When that happens `4F02h` succeeds, the kernel boots perfectly, and the
panel shows black or an "input not supported" message. The shift hatch exists
because the EDID filter cannot be trusted to save every machine.

Two more things worth trying in the same session:

- **Plug an external monitor into the VGA or DisplayPort socket before you
  power on.** If the picture is there, the kernel is fine and only the panel
  is out of range — the same conclusion the shift key gives you, reached
  differently, and useful as a cross-check.
- A black screen with **no** stage 2 text before it is a different fault
  entirely — see *nothing happens* above.

If shift-held text mode works, say so: that is the fix for this machine, and
the long-term answer is then to look at why the EDID path did not catch the
mode rather than to keep holding shift.

If the screen went black **before** `vbe`, so you saw `a20`/`e820`/`load` and
then nothing, that is not the video path. Read the last word printed: it names
the step that did not finish. `load` stopping means `disk error` from stage 2
— the BIOS stopped answering int 13h partway through the kernel, which points
at the port or the stick, not at the kernel.

### It boots to a prompt, but there are no commands

Look at the `Modules:` line in the scrollback.

- **`Modules: 0`, or fewer than seven.** Stage 2 did not load them. The build
  cross-checks the module table against the image, so a mismatch here is
  almost certainly a stick that was written from a stale image or written
  incompletely — rebuild with `make bootdisk` and rewrite. If the count is
  right in the build output and zero on the machine, stage 2 read the module
  sectors and got something else back, which is an int 13h problem: try the
  other port and a different stick.
- **`Modules: 7` and typing `hello` still says command not found.** Then the
  modules loaded but were destroyed afterwards. This has happened once, and it
  is documented in `boot/layout.inc`: the modules used to be loaded at
  `0x140000`, which is inside the kernel's `.bss`, and the kernel wiped five
  of the seven while zeroing it. They now go to `MODULES_PHYS = 0x200000` and
  the build refuses an image whose modules would land below the kernel's real
  end computed from its ELF program headers. If you see this, that check has
  been defeated somehow — report the `Modules:` line and the kernel's
  `PMM: bitmap at …` line together.
- **`ls` says "no filesystem is mounted".** That is not this fault. That is a
  program that ran correctly. See *What a successful boot looks like*.

### It boots, but the keyboard does nothing

First establish whether the machine is alive at all: **look at the clock in
the status bar at the top right of the screen.**

- **Clock ticking, keyboard dead** — the kernel is running and scheduling; the
  input path is what is broken. The T430's built-in keyboard is a real 8042
  PS/2 device, so this is `src/drivers/input/` talking to the controller and
  not any USB setting. Unplug every USB device except the boot stick: if an
  external USB keyboard is attached, the firmware's SMM PS/2 emulation is
  driving the same 8042 the kernel is, and neither of them knows about the
  other.
- **Clock frozen** — the kernel is not running. The timer interrupt or the
  scheduler died, and this is a kernel fault rather than an input one. It is
  worth noting the last line printed before it stopped.
- **No status bar at all** — you are probably in text mode (`vbe text` or
  `vbe safe` in stage 2's output) or the console never came up. Different problem.

The layout is German (`src/drivers/input/`, the kernel's own scancode table),
which is decided by the kernel and not by the BIOS or by QEMU's `-k`, so it is
the same table on the real machine as in every test here.


## What is still untested, and can only be tested on the laptop

Everything in this document about the boot chain itself was verified against
the built image and against QEMU. These are the things that were not, and
cannot be from here:

1. **Every Lenovo BIOS setting named above.** The menu paths, the exact option
   names and the behaviour of `Both` versus `Legacy Only` come from
   `t430-risks.md` and from the machine's documentation, not from a test.
2. **Whether the CSM offers the stick as `USB HDD` or as `USB FDD`.** The MBR
   partition table is there to make it the former. QEMU's SeaBIOS does classify
   the finished image as a hard disk — it prints `Booting from Hard Disk...`
   — but SeaBIOS is not a 2012 Lenovo CSM, and the `USB FDD` path (where the
   BIOS hands stage 1 `DL = 0x00`) has never been executed anywhere.
3. **Which physical port works.** Untestable without the chassis.
4. **The video mode, and both of its safety nets.** Whether the HD 4000's
   VBIOS returns a usable EDID for the internal panel, and whether it lists
   modes the panel cannot display, is the single largest remaining risk — it
   is the one that produces a black screen on a machine that is working
   perfectly. QEMU's std VGA does answer `AX=4F15h` with a real EDID, and the
   filter demonstrably works there (1280x800 chosen over an available
   1920x1080), but that says nothing about a 2012 Intel VBIOS.
   **The shift-held text mode escape hatch is entirely untested**, and cannot
   be tested here: QEMU offers no way to hold a key down through POST, so the
   byte at `0040:0017` is never non-zero when stage 2 samples it. The code
   path is one compare and a branch and it builds, and that is all that is
   known about it. The first person to hold shift at power-on will be finding
   out whether it works.
5. **The E820 memory map.** SeaBIOS with 64 MB returns two usable regions; a
   T430 returns ten to fifteen, broken up by the Intel graphics stolen memory,
   the ME region and ACPI ranges. `MB_MMAP_MAX` is 32, which is comfortably
   above that, and stage 2 truncates cleanly rather than overrunning. What was
   *not* tested is a region **above 4 GiB**, which a 8 or 16 GB T430 will
   report: `qemu-system-i386` clamps low memory at about 3071 MiB and never
   presented such an entry in any run here.
6. **An external USB keyboard against the firmware's SMM PS/2 emulation.**
7. **The FAT16 volume on the stick.** It is known good — the UHCI control run
   mounts it and reports `32040 KiB of 32160 KiB free` — but on a T430 nothing
   will ever read it, so it is 32 MB of correct, unreachable filesystem
   waiting for an EHCI or xHCI driver.
