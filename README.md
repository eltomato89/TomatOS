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
