
#include <system.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <asm.h>
#include <mm.h>
#include <vmm.h>
#include <syscall.h>
#include <exec.h>
#include <ata.h>
#include <fat.h>
#include <vga.h>
#include <fbcon.h>
#include <fbdraw.h>
#include <multiboot.h>
#include <pci.h>
#include <rtl8139.h>
#include <net.h>
#include <dhcp.h>
#include <dns.h>
#include <tcp.h>
#include <mouse.h>
#include <usb.h>
#include <blockdev.h>
#include <uhci.h>
//#include <wmessages.h>

#define NULL 0
#define BCD2BIN(val) (((val) & 0x0F) + ((val) >> 4) * 10)

/* Size of the blocks that "mem -t" uses to probe the heap. */
#define MEM_TEST_SIZE   64
#define MEM_TEST_SMALL  32
#define MEM_TEST_LARGE  128

/* Text mode video memory: the one address in the system whose physical
   location is common knowledge -- and, since the kernel moved into the
   higher half, no longer the address one writes to. It is reached through
   the direct mapping like every other piece of physical memory. */
#define PAGE_VGA_PHYS   0xB8000
#define PAGE_VGA_TEXT   ((uint32_t)P2V(PAGE_VGA_PHYS))

/* Offset inside a page for the test that vmm_get_phys() does not just
   translate the frame but keeps the offset. Deliberately not aligned. */
#define PAGE_TEST_OFFSET 0x123

/* Where "page -t" starts looking for a free virtual address for its
   map/unmap test. The kernel now owns everything from KERNEL_VIRTUAL_BASE
   upwards -- the direct mapping of all usable RAM lives there, and so would
   a recursive directory mapping in the top 4 MiB -- so the probe has to stay
   below 0xC0000000. 2 GiB is far above any physical RAM this kernel will
   see (and above anything start.asm might still have identity mapped low
   down), and still a whole gigabyte clear of the kernel window.
   The address is not used as is -- it is only the starting point of a probe
   with vmm_is_mapped(), see page_find_free_virt(). */
#define PAGE_TEST_VIRT   0x80000000
#define PAGE_TEST_PROBES 1024

/* Pattern written through the freshly created mapping. */
#define PAGE_TEST_PATTERN 0xC0FFEE01

/* --- ring 3 ------------------------------------------------------------- */

/* The one page of memory ring 3 is allowed to read and write. It holds the
   strings the demo prints and nothing else -- see the comment on user_demo()
   for why the strings cannot simply be literals.

   The address is a fixed constant on purpose: a constant compiles into the
   instruction stream as an immediate, whereas a kernel variable holding the
   address would have to be read out of kernel memory, which is exactly what
   ring 3 must not do.

   1 GiB is comfortably below the kernel window at 0xC0000000, comfortably
   above anything a user image would ever be loaded at, and comfortably clear
   of the per task user stacks the task manager parks just under the kernel. */
#define USER_PAGE_VIRT   0x40000000u

/* Slots inside that page, one message each. */
#define USER_MSG_SIZE    0x40
#define USER_MSG_HELLO   (USER_PAGE_VIRT + 0x000)
#define USER_MSG_PID     (USER_PAGE_VIRT + 0x040)
#define USER_MSG_SLEPT   (USER_PAGE_VIRT + 0x080)
#define USER_MSG_BYE     (USER_PAGE_VIRT + 0x0C0)
#define USER_MSG_TEST    (USER_PAGE_VIRT + 0x100)

/* The pieces the isolation tasks print. Both tasks read them from the same
   frame -- that page is shared on purpose, unlike the one they write to. */
#define USER_MSG_ISO_PID   (USER_PAGE_VIRT + 0x140)
#define USER_MSG_ISO_WROTE (USER_PAGE_VIRT + 0x180)
#define USER_MSG_ISO_AND   (USER_PAGE_VIRT + 0x1C0)
#define USER_MSG_ISO_READ  (USER_PAGE_VIRT + 0x200)
#define USER_MSG_ISO_OWN   (USER_PAGE_VIRT + 0x240)
#define USER_MSG_ISO_LOST  (USER_PAGE_VIRT + 0x280)

/* The ring 3 code, as the linker laid it out.
*
*  Every routine that executes with CPL 3 carries USER_TEXT below and is
*  therefore linked into the .usertext section, which linker.ld gives an
*  output section of its own with these two symbols around it, page aligned at
*  both ends. The section is the unit of the whole exercise: user_setup()
*  copies exactly this range into frames of its own and maps only those for
*  ring 3, so no page of kernel .text is ever opened.
*
*  A linker symbol IS the address, hence the array declaration -- see the same
*  trick on kernel_end in pmm.c. */
extern char usertext_start[];
extern char usertext_end[];

/* Puts a function into the ring 3 code section. Applied to every routine that
*  runs at CPL 3, and to nothing else: whatever carries this becomes readable
*  and executable from ring 3, so the section has to stay exactly as large as
*  the set of routines that need it. */
#define USER_TEXT __attribute__((section(".usertext")))

/* Where the copy of that section is mapped.
*
*  The address is in the kernel quarter rather than down next to the string
*  page, and that is not a matter of taste. A ring 3 task runs in an address
*  space of its own from its very first instruction, and vmm_create_space()
*  zeroes the private 768 directory entries of a fresh space -- so a mapping
*  below KERNEL_VIRTUAL_BASE made here, in the shell's space, simply is not
*  there when the task starts, and the task would fault on its entry point
*  before anything could map it. Only the top quarter is copied verbatim into
*  every new space, so that is where code a task must be able to fetch on its
*  first instruction has to live. The kernel half of the directory is shared,
*  which is exactly why one vmm_map() here reaches every task.
*
*  What is opened by this is one page table entry per page of .usertext, and
*  those pages hold ring 3 code and nothing else. The kernel's own text keeps
*  its supervisor-only mapping.
*
*  Chosen 8 MiB below the top of the address space: far above the direct
*  mapping of RAM (which would have to reach a gigabyte to get here), far
*  below nothing, and clear of everything the kernel maps. user_text_map()
*  verifies the range really is free before it uses it. */
#define USER_TEXT_VIRT   0xFF800000u

/* Upper bound on the size of .usertext, in pages. Two small routines live
*  there today; the limit exists so a section that unexpectedly grows is
*  reported instead of silently overrunning the frame table below. */
#define USER_TEXT_MAX    4

/* A call number that is not in the table, for the SYS_ENOSYS check. Well
   above SYSCALL_MAX, so it stays unassigned when calls are added. */
#define SYS_NOSUCHCALL   99

#define USER_DEMO_SLEEP  120  /* ms the ring 3 demo sleeps */
#define USER_TEST_SLEEP  50   /* ms "user -t" sleeps       */
#define USER_DEMO_WAIT   800  /* ms the shell waits for the demo to finish */

/* --- address spaces ----------------------------------------------------- */

/* Every ring 3 task now runs in a page directory of its own, of which only
*  the quarter from KERNEL_VIRTUAL_BASE up is shared with the kernel (see the
*  address space block in vmm.h). Everything below that -- the string page,
*  the test page, the task's stack -- is private to one task.
*
*  That has a direct consequence for this file: a vmm_map() done here maps
*  into the SHELL's directory, and a ring 3 task will not see it. Pages a task
*  is meant to have are therefore put into the task's own space with
*  vmm_map_in(), which needs the space, which in turn only exists once the
*  task exists. So every ring 3 routine below begins with a sleep, long enough
*  for the shell to furnish its address space while it waits.
*
*  How the shell learns which space a task got: it watches. vmm_current_space()
*  reads CR3, so calling it inside a timer interrupt that hit a given task
*  yields exactly that task's directory -- the interrupt handler runs before
*  schedule() and therefore still in the interrupted task's address space. See
*  user_space_probe(). No task manager bookkeeping is needed for it, and the
*  identifier printed for a task is the very value vmm_current_space() returned
*  while that task was on the CPU. */

/* ms a fresh ring 3 task waits before touching any of its own pages. Has to
*  outlast the shell's side of the handover, i.e. the waiting below for as
*  many tasks as one command starts -- two of them, so 2 * USER_SPACE_WAIT,
*  plus room for the mapping itself. */
#define USER_MAP_DELAY   500

/* How long, and how finely, the shell waits for a task's space to show up.
*  A task that has been started is normally on the CPU within a tick or two;
*  the limit only exists so a task that never runs cannot hang the shell. */
#define USER_SPACE_WAIT  150
#define USER_SPACE_POLL  10

/* --- isolation test ----------------------------------------------------- */

/* The one virtual address BOTH isolation tasks use. One page above the string
*  page, so still in the private lower three quarters, and mapped in each
*  task's own directory to a frame of its own. Two tasks holding this address
*  with different contents is the property the whole test is about. */
#define USER_ISO_VIRT    0x40001000u

/* Word indices inside that page. The cell is what a task writes and reads
*  back. Role and value are parameters: the shell writes them into each frame
*  through the direct mapping before the task starts, so the two tasks find
*  different numbers at the same address before they have executed a single
*  instruction -- the first thing the test demonstrates. */
#define USER_ISO_CELL    0
#define USER_ISO_ROLE    1
#define USER_ISO_VALUE   2

/* One task writes 111111 and then 111112, the other 222222 and then 222223.
*  Far apart, so a clobbered read-back is obvious in the output. */
#define USER_ISO_VALUE_A 111111u
#define USER_ISO_VALUE_B 222222u

/* Length of one step of the interleaving schedule, see user_iso_task(). */
#define USER_ISO_STEP    100

/* ms the shell waits for both tasks to finish. The later task is done after
*  USER_MAP_DELAY + 5 steps = 1000 ms, counted from its start, and the shell
*  begins this wait at most 300 ms into that. */
#define USER_ISO_WAIT    1200

/* --- programs ----------------------------------------------------------- */

/* Column widths of the tables "ps" prints. printf() has no field widths, so
*  every column is padded by hand -- see mem_print_right() for the numbers and
*  ps_print_left() for the names. A name column of 20 leaves room for the
*  module names a "module" line in grub.cfg realistically carries and still
*  keeps the whole row inside 80 columns. */
#define PS_NAME_WIDTH    20
#define PS_SIZE_WIDTH    10
#define PS_PID_WIDTH     3

/* How many program starts the shell remembers. The list exists for one
*  question only -- which pid and which address space every instance got --
*  and that question is about the recent ones, so the oldest entry drops out
*  when the table is full rather than the table growing without end. */
#define PS_INSTANCES     8

/* --- filesystem --------------------------------------------------------- */

/* Width of the model column in "df". IDENTIFY hands out 40 characters, plus
*  one space to the next column. */
#define DF_MODEL_WIDTH   41

/* --- programs on the disk ------------------------------------------------
*
*  Where a command that is not built in is looked for, and what its file is
*  called. Neither half is a matter of taste here: the Makefile copies every
*  user program into /BIN as NAME.ELF in upper case, because mcopy would
*  otherwise write VFAT long name entries and fat.c skips those -- a lower
*  case name would simply not be there to find.
*
*  So the shell converts, in run_path() and nowhere else: what the user types
*  is lower case, what the directory holds is upper case 8.3. */
#define BIN_DIR          "/BIN/"
#define BIN_EXT          ".ELF"

/* The "8" of 8.3, and therefore the longest command name that can name a
*  file on this volume. */
#define BIN_NAME_MAX     8

/* "/BIN/" plus eight characters plus ".ELF" plus the terminator. */
#define BIN_PATH_MAX     18

/* Upper bound on the walk over /BIN that "help" does. fat_readdir() says when
*  it is done, so this is not how the loop normally ends -- it is the guard
*  that keeps a damaged or looping directory from hanging the shell. */
#define BIN_MAX_ENTRIES  4096

/* The listing "help" prints. A name is at most BIN_NAME_MAX characters once
*  the extension is off, so ten leaves two spaces between columns, and six of
*  those plus the leading tab stay inside eighty columns. */
#define BIN_NAME_WIDTH   10
#define BIN_PER_LINE     6

/* How the shell waits for a program it started.
*
*  RUN_WAIT_MS bounds the SHELL's patience, not the program's life: a program
*  that outlives it keeps running and keeps its output, the shell merely stops
*  waiting and says so. Killing it instead would be the shell deciding that
*  half a minute of work is too much, which is not its call to make; waiting
*  forever would let one bad program on the disk take the machine away, which
*  is worse than a prompt printed into somebody's output. */
#define RUN_POLL_MS      10
#define RUN_WAIT_MS      30000

/* --- graphics ----------------------------------------------------------- */

/* Palette layout of the demo picture.
*
*  Everything the picture uses lives at index 16 and above, and that is not a
*  matter of taste: entries 0..15 are the sixteen colours text mode paints the
*  console with, and the DAC is one single piece of hardware for both modes. A
*  vga_palette(1, ...) here would therefore come back as a shell whose blue is
*  no longer blue, long after the mode switch is over. Leaving the first
*  sixteen entries untouched is what makes the return invisible from the
*  palette side; the registers and the font are vga_set_mode()'s business.
*
*  The DAC takes six bits per channel, so every component below runs 0..63. */
#define GFX_BLACK        16
#define GFX_WHITE        17
#define GFX_TEXT         18   /* labels and captions      */
#define GFX_AMBER        19   /* title and separators     */
#define GFX_SHADOW       20   /* drop shadow of the title */
#define GFX_FRAME        21   /* panel outlines           */
#define GFX_PANEL        22   /* panel and bar background */
#define GFX_LEAF         23
#define GFX_LEAF_DARK    24
#define GFX_STEM         25

/* Vertical background gradient, one entry per band of the screen. */
#define GFX_SKY_FIRST    32
#define GFX_SKY_STEPS    64

/* Shading ramp of the tomato, dark rim to lit highlight. */
#define GFX_TOMATO_FIRST 96
#define GFX_TOMATO_STEPS 16

/* Hue sweep for the palette band. 144 entries at two pixels each are exactly
*  the 288 pixels the band is wide, so every single entry is visible. */
#define GFX_SPECTRUM_FIRST 112
#define GFX_SPECTRUM_STEPS 144

/* A background colour of 0xFF means "leave the pixels alone", see vga.h. */
#define GFX_TRANSPARENT  0xFF

/* Layout of the picture. Three panels of 96 pixels with 8 pixel gutters fill
*  the 320 pixel width exactly. */
#define GFX_HEAD_H       27
#define GFX_PANEL_Y      34
#define GFX_PANEL_W      96
#define GFX_PANEL_H      110
#define GFX_PANEL1_X     8
#define GFX_PANEL2_X     112
#define GFX_PANEL3_X     216
#define GFX_LABEL_DY     98   /* label offset inside a panel */
#define GFX_BAND_X       16
#define GFX_BAND_Y       152
#define GFX_BAND_W       288
#define GFX_BAND_H       12
#define GFX_CAPTION_Y    168
#define GFX_FOOT_Y       176
#define GFX_FOOT_TEXT_Y  184

/* The tomato: centre, radius and the height its stem reaches above it. */
#define GFX_TOMATO_CX    56
#define GFX_TOMATO_CY    80
#define GFX_TOMATO_R     34

/* Size of the title, in whole font pixels per glyph pixel. On a framebuffer
*  it is multiplied by the same unit every other weight in the picture is,
*  see gfx_unit. */
#define GFX_TITLE_SCALE  3

/* What the line under the title says the mode offers. Mode 13h has 256
*  colours out of its DAC; the framebuffer has no palette and prints its
*  depth instead, so this number is only ever used on the one path. */
#define GFX_VGA_COLOURS  256

/* The buffer gfx_mode_text() builds into. "1024x768x32" and its terminator
*  are twelve bytes; the rest is headroom, because the two numbers come from
*  the surface at runtime and a stack buffer sized to exactly the mode that
*  happens to be on the screen is a buffer sized to today's boot. */
#define GFX_MODE_TEXT    24

/* gfx_enter() returns this rather than a vga_set_mode() code when the console
*  is on a framebuffer that fbdraw reports no surface for. It is not an error
*  from the mode switch -- there is no mode switch on that path, and taking
*  one would be the mistake -- so it must not be mistaken for one: -1 is what
*  vga_set_mode() itself returns for a mode it does not know. */
#define GFX_NO_SURFACE   (-2)

/* --- the graphics self-test --------------------------------------------- */

/* Where "gfx -t" writes its single pixel, and with which value. The colour is
*  deliberately none of 0, 1 or 15, so a read-back that only ever returns a
*  stuck value cannot pass by accident. */
#define GFX_TEST_X       100
#define GFX_TEST_Y       50
#define GFX_TEST_COLOUR  37

/* The clipping check. A fill that starts 40 pixels to the left of the screen
*  must appear from column 0 up to GFX_CLIP_X + GFX_CLIP_W - 1 and nowhere
*  else -- so the guard pixel one column further right has to survive it.
*
*  The second guard is the interesting one: it sits at the far right end of
*  the PREVIOUS row, which is precisely where the negative start column lands
*  when a drawing routine computes fb[y * 320 + x] without clipping first.
*  A wrapped row is invisible in a picture and obvious here. */
#define GFX_GUARD_X      60
#define GFX_GUARD_Y      60
#define GFX_GUARD_COLOUR 200
#define GFX_CLIP_X       (-40)
#define GFX_CLIP_W       100
#define GFX_CLIP_COLOUR  100

/* The colours the framebuffer half of the self-test paints its three test
*  indices with, and a fourth it recolours one of them to afterwards.
*
*  Full eight bit channels, because fbdraw_palette() takes 0..255 where the
*  DAC takes 0..63 -- see gfx_palette(). They are far apart so that a
*  mistranslated entry cannot pass by accident, and none of them is a grey,
*  so the last check can tell a repainted console from a framebuffer that
*  still holds the test picture. */
#define GFX_FB_TEST_R    0x20
#define GFX_FB_TEST_G    0xA0
#define GFX_FB_TEST_B    0xF0
#define GFX_FB_GUARD_R   0xF0
#define GFX_FB_GUARD_G   0x10
#define GFX_FB_GUARD_B   0x30
#define GFX_FB_CLIP_R    0x10
#define GFX_FB_CLIP_G    0xE0
#define GFX_FB_CLIP_B    0x40
#define GFX_FB_OTHER_R   0xC0
#define GFX_FB_OTHER_G   0xC0
#define GFX_FB_OTHER_B   0x10

/* The text screen is 80 by 25 cells of one character plus one attribute. */
#define GFX_TEXT_CELLS   (80 * 25)

/* Width of the label column of "gfx -i", counted from the start of the line
*  including the two leading spaces. The labels below carry their padding
*  literally, the way every other table in this file does -- printf() has no
*  field widths, so a "%-15s" is not available and would only hide where the
*  column actually is. */
#define GFX_INFO_LABEL   17

/* --- mouse ---------------------------------------------------------------
*
*  Column widths of the row "mouse" repaints while it runs, and of the header
*  over it. printf() has no field widths, so every field is padded by hand out
*  of ps_print_left(), fs_print_right_text(), mem_print_right() and
*  mouse_print_signed(), exactly as the "ps", "df", "arp" and "netstat" tables
*  are.
*
*  THE ROW HAS TO COME OUT THE SAME WIDTH EVERY TIME. It is rewritten in place
*  with a carriage return rather than a newline -- that is what makes it a live
*  display instead of a page of scrolling numbers -- and a carriage return only
*  moves the cursor back to the margin, erasing nothing. A row that shrank from
*  four digits to three would leave the fourth standing there. So every field
*  below is padded to its width and none is truncated to it, and the widths are
*  the ones the values cannot outgrow: a coordinate is clamped into the bounds
*  and a movement carries a sign, and five columns hold both at any screen size
*  this kernel can drive.
*
*  The counters are 32 bit and would want ten columns each. They get nine, and
*  a machine that has produced a billion packets pushes the row along instead
*  of losing a digit -- a display that has stopped lining up rather than one
*  that has started lying. It takes a fortnight of moving the mouse to get
*  there, and the alternative costs the row its position column.
*
*  2 + 13 + 2 + 13 + 2 + 7 + 4 * 9 = 75 columns, which leaves the eighty column
*  line intact and the cursor short of the margin -- so the row never wraps
*  onto a second line, which would leave half of it on the screen for good. */
#define MOUSE_COORD_WIDTH    5   /* one coordinate, its sign included      */
#define MOUSE_PAIR_WIDTH    13   /* "(nnnnn,nnnnn)": two of those, the
                                    brackets and the comma                 */
#define MOUSE_BTN_WIDTH      7   /* "LMR" and the gap to the next column   */
#define MOUSE_COUNT_WIDTH    9
#define MOUSE_GAP            2
#define MOUSE_ROW_WIDTH   (MOUSE_GAP + MOUSE_PAIR_WIDTH \
                           + MOUSE_GAP + MOUSE_PAIR_WIDTH \
                           + MOUSE_GAP + MOUSE_BTN_WIDTH \
                           + 4 * MOUSE_COUNT_WIDTH)

/* Width of the label column of "mouse -i", counted from the start of the line
*  including the two leading spaces -- the same idea as GFX_INFO_LABEL. */
#define MOUSE_INFO_LABEL    14

/* The text screen, and the size of the cell the font draws into it. Only used
*  to convert between the two: the pointer's field is cells times cell size,
*  and "mouse -i" divides back to say which cell the pointer is over. See the
*  comment above mouse_console_field() for why a text screen is measured in
*  pixels at all. */
#define MOUSE_TEXT_COLS     80
#define MOUSE_TEXT_ROWS     25
#define MOUSE_CELL_W         8
#define MOUSE_CELL_H        16

/* Upper bound on one wait in the live loop, in milliseconds. Not a deadline:
*  the loop around it sees to that. It is how often the keyboard is looked at,
*  and getch() carries the same number for a related reason -- see
*  mouse_wait_turn(). */
#define MOUSE_WAIT_MS      200

/* The largest field "mouse -b" accepts. Nothing this kernel can drive is
*  anywhere near it; it is here so that a typing mistake becomes a message
*  rather than a pointer somewhere off in a field of two billion pixels. */
#define MOUSE_FIELD_MAX   8192

/* The field "mouse -t" works in, and the numbers it pushes the pointer to.
*  A field of its own rather than the console's, so that the checks read the
*  same on every machine, and one no screen has exactly -- a clamp that
*  happened to land on the console size would prove nothing. MOUSE_TEST_OVER
*  is how far past a corner the pointer is pushed: comfortably more than one,
*  so an off-by-one clamp cannot be mistaken for a clamp that works. */
#define MOUSE_TEST_W       800
#define MOUSE_TEST_H       600
#define MOUSE_TEST_X       100
#define MOUSE_TEST_Y        50
#define MOUSE_TEST_OVER    100
#define MOUSE_SMALL_W      320
#define MOUSE_SMALL_H      200

/* --- network -------------------------------------------------------------
*
*  Byte order, once, because everything below depends on it: the addresses
*  that cross this interface -- net_ip(), net_configure(), ip_send(),
*  icmp_send_echo(), arp_cache_get() -- are HOST order 32 bit values, with
*  a.b.c.d stored as (a << 24) | (b << 16) | (c << 8) | d. Network order
*  belongs to the packed headers in net.h and nowhere else; net.c converts at
*  the edge with htonl(). net_parse_ip() and net_ip_text() are the two
*  directions of that host order form, and they are the only two places in
*  this file that know what a dotted quad looks like.
*
*  Text buffer sizes. There is no sprintf() here, so a value that has to be
*  padded into a column is written into a buffer of its own first and then
*  handed to ps_print_left() or fs_print_right_text() like any other string. */
#define NET_IP_TEXT      16   /* "255.255.255.255" and the terminator     */
#define NET_MAC_TEXT     18   /* "00:11:22:33:44:55" and the terminator   */
#define NET_HEX_TEXT      9   /* eight hex digits and the terminator      */
#define NET_CLASS_TEXT    6   /* "06:00" and the terminator               */

/* Width of the label column of "ifconfig", counted from the start of the
*  line including the two leading spaces -- the same idea as GFX_INFO_LABEL,
*  and for the same reason: printf() has no "%-15s". */
#define NET_INFO_LABEL   17

/* When a peak occupancy of the receive queue is worth calling "most of it".
*  Three quarters rather than nine tenths, because the whole point of the
*  peak is that it is seen BEFORE frames are lost: a queue that has already
*  been three quarters full absorbed a burst it only just fitted, and the
*  next one of those is the one that does not fit. */
#define NET_QUEUE_HIGH   75

/* Which card the driver drives. rtl8139.c owns these two numbers; they are
*  repeated here for one purpose only, namely to point at the line in the
*  "lspci" table that is the network card. */
#define NET_RTL_VENDOR   0x10EC
#define NET_RTL_DEVICE   0x8139

/* PCI class codes "lspci" names in words. Only the ones a machine this
*  kernel boots on actually shows are listed; anything else prints its
*  number and no name. */
#define PCI_CLASS_STORAGE  0x01
#define PCI_CLASS_NETWORK  0x02
#define PCI_CLASS_DISPLAY  0x03
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_MEMORY   0x05
#define PCI_CLASS_BRIDGE   0x06
#define PCI_CLASS_SERIAL   0x0C

/* --- ping ----------------------------------------------------------------
*
*  How long it waits, and why each of these numbers is what it is.
*
*  The ARP figures are the important ones. The first packet to an address
*  nobody has talked to yet cannot go out at all: ip_send() needs the
*  destination's MAC, the cache is cold, so all it can do is put a query on
*  the wire and return an error. Retrying is not papering over a bug -- it IS
*  how resolution works, because the answer comes back in an interrupt while
*  this task sleeps. 50 attempts 20 ms apart give the other end a full second
*  to answer, and cost only 20 ms when it answers at once, which on a virtual
*  network it does. */
#define NET_ARP_TRIES    50
#define NET_ARP_WAIT     20   /* ms between two attempts -> 1000 ms total */

/* One request, and how long its reply may take.
*
*  icmp_last_reply() is a mailbox that the stack writes into, so it is looked
*  at rather than waited on -- but the wait between two looks is now a real
*  wait on the network, and NET_PING_POLL is the longest ONE of them may last
*  rather than the interval between two. A reply that is parsed wakes the task
*  and is read at once; the bound is what catches a wake that was missed, and
*  ten milliseconds keeps that miss invisible in a measurement. */
#define NET_PING_COUNT    4   /* echo requests per run                    */
#define NET_PING_WAIT  1000   /* ms to wait for one reply                 */
#define NET_PING_POLL    10   /* ms one wait for the mailbox may last     */
#define NET_PING_GAP    300   /* ms between two requests, as ping does    */

/* The identifier carried in every echo this shell sends, so a reply meant
*  for somebody else's ping is not counted as ours. 0x546F is 'T','o'. */
#define NET_PING_ID    0x546F

/* What net_ping_once() returns instead of a round trip time. Zero is a
*  perfectly good time -- a virtual network answers inside one tick -- so the
*  failures have to be negative rather than falsy. */
#define NET_PING_UNRESOLVED (-1)
#define NET_PING_LOST       (-2)

/* --- dhcp ----------------------------------------------------------------
*
*  The same shape as ping, for the same reason: dhcp_start() only puts the
*  first message on the wire, and every answer after that is parsed while this
*  task waits. So the command is a loop that drains the queue, calls
*  dhcp_poll() -- which is the only thing that resends a message nobody
*  answered -- watches dhcp_state() move, and then blocks on the network until
*  the next message has been processed or the interval below is up.
*
*  The upper limit is this shell's, not the client's. dhcp_poll() gives up on
*  its own once its attempts are used up, and that is the failure worth
*  reporting because it knows why; DHCP_WAIT exists only so that a client
*  which never reaches a conclusion cannot hang the shell forever. It is
*  therefore set well above any retransmission schedule the client could
*  plausibly run -- the current one gives up after fifteen seconds -- so that
*  it is a backstop and not a second, competing deadline that would take the
*  reason away from the answer.
*
*  DHCP_POLL is the longest ONE wait may last, not the interval between two
*  looks: a message that is parsed wakes the loop and the state is read at
*  once. The bound stays because nothing wakes a retransmission -- dhcp_poll()
*  resends a message nobody answered, and "nobody answered" is precisely the
*  case in which no wake will ever come. It also still has to be a length at
*  which two steps can be crossed inside one wait, because the states are
*  ordered and a look that finds two of them crossed prints both -- see
*  net_dhcp_run(). */
#define DHCP_WAIT     25000   /* ms before the shell stops waiting        */
#define DHCP_POLL        50   /* ms one wait for a message may last       */

/* A lease of 0xFFFFFFFF seconds is the protocol's way of writing "forever",
*  not a duration of 136 years, and printing it as one would be silly. */
#define DHCP_LEASE_FOREVER 0xFFFFFFFFUL

/* Seconds in the units a lease is worth reading in. Used for a DNS TTL as
*  well as for a lease -- see net_duration(). */
#define DHCP_SECS_PER_DAY  86400
#define DHCP_SECS_PER_HOUR  3600
#define DHCP_SECS_PER_MIN     60

/* --- dns -----------------------------------------------------------------
*
*  The third command in this file that watches a state machine somebody else
*  drives, and the shape is the one "dhcp" established: dns_resolve() puts a
*  query on the wire and returns, the answer is parsed while this task waits,
*  so the command is a loop that drains the queue, calls dns_poll() -- the
*  only thing that resends and the only thing that ever gives up -- watches
*  dns_state(), and blocks on the network in between.
*
*  Unlike DHCP there are no steps worth printing: one query, one answer, and
*  the states in between say nothing a user could act on. So the loop is
*  silent and only the conclusion is printed.
*
*  DNS_WAIT is this shell's backstop and nothing else, exactly as DHCP_WAIT
*  is. dns_poll() gives up on its own once its attempts are used up, and that
*  is the failure worth reporting because it knows why; this limit exists so
*  that a resolver which never concludes cannot hang the shell, and it is set
*  well above any retransmission schedule a resolver could plausibly run so
*  that it is a backstop rather than a second, competing deadline.
*
*  DNS_POLL, like DHCP_POLL, is the longest one wait may last rather than the
*  interval between two looks: the answer wakes the loop. The bound stays for
*  the same reason -- dns_poll() is what retransmits, and a query that needs
*  retransmitting is one nothing answered, so no wake is coming. */
#define DNS_WAIT      15000   /* ms before the shell stops waiting        */
#define DNS_POLL         50   /* ms one wait for the answer may last      */

/* As long a name as can be typed. prmv() hands back at most 100 bytes, so a
*  longer one cannot reach these commands however long DNS_NAME_MAX is. */
#define NET_DNS_NAME    100

/* The name column of the cache listing. Wide enough for the names anybody
*  types and narrow enough that name, address and TTL fit in eighty columns
*  together -- 2 + 33 + 16 + 10 = 61. */
#define NET_DNS_NAME_WIDTH 33

/* What one lookup produced. A small record rather than four out parameters,
*  because "nslookup" and "ping" want the same lookup and differ only in how
*  they print it -- see net_dns_resolve(). */
typedef struct
{
	uint32_t ip;      /* host order, the address that was found            */
	uint32_t ttl;     /* seconds it may be kept, or seconds left if cached */
	int      cached;  /* 1 when the answer was already here                */
	int      ms;      /* how long the exchange took, 0 for a cache hit     */
} net_dns_answer;

/* --- tcp -----------------------------------------------------------------
*
*  Column widths of the table "netstat" prints. printf() has no field widths,
*  so every column is padded by hand out of ps_print_left() and
*  fs_print_right_text(), exactly as the "ps", "df", "arp" and "nslookup"
*  tables are.
*
*  The row is two leading spaces plus 7 + 7 + 23 + 14 + 10 + 10 = 73 columns,
*  which leaves the eighty column line intact. The two that are not obvious:
*  a peer needs 21 characters at worst ("255.255.255.255:65535") and gets 23
*  so there are always two spaces after it, and a state needs 11 at worst
*  ("ESTABLISHED") and gets 14 for the same reason. Neither number may shrink
*  -- a truncated state name is precisely the field somebody is reading. */
#define NET_TCP_HANDLE_WIDTH  7
#define NET_TCP_PORT_WIDTH    7
#define NET_TCP_PEER_WIDTH   23
#define NET_TCP_STATE_WIDTH  14
#define NET_TCP_BYTES_WIDTH  10

/* Text buffer sizes, the same idea as NET_IP_TEXT: there is no sprintf()
*  here, so a value that has to be padded into a column is written into a
*  buffer of its own first. */
#define NET_TCP_PEER_TEXT    22   /* "255.255.255.255:65535" and the end   */
#define NET_TCP_NUM_TEXT     11   /* ten digits of a uint32_t and the end  */

/* --- usb -----------------------------------------------------------------
*
*  "lsusb" is the fourth command in this file that has to describe hardware
*  which may not be there, so it borrows the shape the network commands
*  established: a label column, then a table, then the paragraph that says
*  what an empty listing means. Same padding helpers, for the same reason --
*  printf() has no field widths.
*/

/* Width of the label column, two leading spaces included. The longest label
*  is "Last error:", eleven characters, so fifteen leaves two spaces after
*  every one of them. */
#define USB_INFO_LABEL   15

/* Text buffers, the same idea as NET_IP_TEXT: a value that has to be padded
*  into a column is written into a buffer of its own first. */
#define USB_ID_TEXT      10   /* "0627:0001" and the terminator          */
#define USB_CLASS_TEXT    9   /* "03:01:01" and the terminator           */

/* How long the frame number is watched for. Long enough that the reading is
*  not dominated by the millisecond the timer counts in -- two hundred frames
*  put the granularity at half a percent -- and short enough that nobody
*  notices the command pausing. See usb_show_frames() for why a rate is taken
*  at all rather than a single reading. */
#define USB_FRAME_SAMPLE_MS   200

/* What a running UHCI controller must read. A frame is one millisecond by
*  definition, so this is not a measurement of this machine but a property of
*  the bus, and the band around it is only there to absorb a sample the
*  scheduler interrupted. */
#define USB_FRAME_RATE_NOMINAL 1000
#define USB_FRAME_RATE_LOW      900
#define USB_FRAME_RATE_HIGH    1100

/* A USB host controller on the PCI bus is class 0x0C subclass 0x03 -- and
*  that pair says nothing about WHICH of the four incompatible interfaces it
*  is, which is the whole point of pci.h keeping the prog-if byte. These are
*  the four, and 0xFE, which is a device rather than a controller. */
#define USB_PCI_SUBCLASS  0x03
#define USB_PROGIF_UHCI   0x00
#define USB_PROGIF_OHCI   0x10
#define USB_PROGIF_EHCI   0x20
#define USB_PROGIF_XHCI   0x30
#define USB_PROGIF_DEV    0xFE

/* The class triples worth spelling out, and they are the two classes this
*  stack exists for. A HID interface with subclass 1 speaks the boot protocol,
*  which is the fixed report format a BIOS can read without parsing anything,
*  and protocol 1 and 2 are the only two there are: keyboard and mouse. Mass
*  storage subclass 6 is the SCSI command set and protocol 0x50 is bulk-only
*  transport, which together are what every USB disk made since 2000 is. */
#define USB_HID_SUB_BOOT       0x01
#define USB_HID_PROTO_KEYBOARD 0x01
#define USB_HID_PROTO_MOUSE    0x02
#define USB_MSC_SUB_SCSI       0x06
#define USB_MSC_PROTO_BULK     0x50

/* --- what the bootloader reported ----------------------------------------
*
*  kernel.c owns this record: it reads the framebuffer fields out of the
*  multiboot info once, at boot, and hands them out through these accessors.
*  There is no framebuffer.h yet -- the comment above framebuffer_init() in
*  kernel.c says why, and says that a user declares what it needs until there
*  is one. This is that declaration.
*
*  The values are what the BOOTLOADER said. Whether the framebuffer console
*  is actually driving the screen is a different question and is asked of
*  fbcon.h, which is why "gfx -i" prints both. */
extern uint32_t fb_base(void);           /* physical base, 0 = none reported */
extern uint32_t fb_pitch_bytes(void);    /* bytes per row, NOT width * bpp/8 */
extern uint32_t fb_pixel_width(void);
extern uint32_t fb_pixel_height(void);
extern uint32_t fb_bits_per_pixel(void);
extern uint32_t fb_kind(void);           /* MULTIBOOT_FRAMEBUFFER_*          */

void update_infobar() {
	while(1)
	{
		display_update_statusbar();
		sleep(10);
	}
}

void task() {
	while(1)
	{
		sleep(1000);
	}
}

void taskmanager(char *cmd);
void memory(char *cmd);
void paging(char *cmd);
void usermode(char *cmd);
void processes(char *cmd);
void execute(char *cmd);
void diskfree(char *cmd);
void graphics(char *cmd);
void listpci(char *cmd);
void listusb(char *cmd);
void netconfig(char *cmd);
void dhcpclient(char *cmd);
void arptable(char *cmd);
void pinghost(char *cmd);
void nslookup(char *cmd);
void netstat(char *cmd);
void mousepointer(char *cmd);
void help(void);

static void mem_print_right(uint32_t value, int width);
static void mem_show_status(void);
static void mem_selftest(void);
static void mem_check(int ok);

static void page_print_hex(uint32_t value, int digits);
static void page_show_status(void);
static void page_selftest(void);
static void page_fault_demo(void);
static void page_check(int ok);

USER_TEXT static void user_demo(void);
USER_TEXT static void user_iso_task(void);
static int  user_setup(void);
static void user_run(void);
static void user_selftest(void);
static void user_isolation(void);
static void user_check(int ok);

static void ps_print_left(const char *text, int width);
static void ps_modules(void);
static void ps_instances(void);
static void exec_remember(int pid, addrspace_t space, const char *name);

static void fs_print_right_text(const char *text, int width);
static int  fs_drives_found(void);
static void fs_explain_unmounted(void);
static void fs_df(void);

static int  run_name_char(char c);
static char run_upper(char c);
static int  run_path(const char *word, char *path);
static const char *run_arguments(char *cmd);
static void run_wait(int pid, const char *name);
static void run_program(const char *word, char *cmd);
static void help_programs(void);

static void gfx_statusbar_hold(void);
static void gfx_statusbar_release(void);
static void gfx_text_store(void);
static void gfx_text_recall(void);
static void gfx_pick_surface(void);
static int  gfx_enter(void);
static int  gfx_leave(void);
static int  gfx_sx(int value);
static int  gfx_sy(int value);
static int  gfx_fit_scale(int chars, int width, int most);
static void gfx_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
static void gfx_setup_palette(void);
static void gfx_char_scaled(int x, int y, char c, int scale, uint8_t colour);
static void gfx_string_scaled(int x, int y, const char *s, int scale,
                              uint8_t colour);
static void gfx_text(int x, int y, const char *s, int scale, uint8_t colour);
static void gfx_text_centred(int cx, int y, const char *s, int scale,
                             uint8_t colour);
static void gfx_text_clamped(int x, int y, const char *s, int scale,
                             int right, uint8_t colour);
static void gfx_frame(int x, int y, int w, int h, int thick, uint8_t colour);
static void gfx_thick_line(int x0, int y0, int x1, int y1, int thick,
                           uint8_t colour);
static void gfx_ring(int cx, int cy, int radius, int thick, uint8_t colour);
static void gfx_mode_text(char *out);
static void gfx_panel(int x, const char *label, int scale);
static void gfx_draw_picture(void);
static void gfx_show(void);
static uint32_t gfx_fb_read(int x, int y);
static void gfx_selftest_vga(void);
static void gfx_selftest_fb(void);
static void gfx_selftest(void);
static void gfx_check(int ok);
static const char *gfx_mode_kind(void);
static void gfx_info_label(const char *label);
static void gfx_show_mode(void);

static void mouse_console_field(int *w, int *h);
static void mouse_apply_field(void);
static const char *mouse_field_text(void);
static void mouse_print_signed(int value, int width, int plus);
static void mouse_print_pair(int a, int b, int plus);
static void mouse_button_text(int buttons, char *text);
static const char *mouse_button_name(int mask);
static void mouse_row_header(void);
static void mouse_row(int x, int y, int dx, int dy, int buttons);
static void mouse_clear_row(void);
static void mouse_info_label(const char *label);
static void mouse_explain_absent(void);
static void mouse_explain_counters(uint32_t resyncs, uint32_t overflows,
                                   uint32_t dropped);
static void mouse_show_counters(void);
static void mouse_show_status(void);
static void mouse_print_event(const mouse_event *ev, int *presses,
                              int *releases);
static void mouse_wait_turn(uint32_t seen);
static void mouse_live(void);
static void mouse_set_field(char *cmd);
static void mouse_check(int ok);
static void mouse_selftest(void);

static char *net_put_byte(char *p, uint32_t value);
static void net_ip_text(uint32_t ip, char *text);
static void net_mac_text(const uint8_t *mac, char *text);
static void net_hex_text(uint32_t value, int digits, char *text);
static void net_print_ip(uint32_t ip);
static int  net_parse_ip(const char *text, uint32_t *out);
static void net_info_label(const char *label);
static void net_explain_down(void);
static int  net_interface_ready(const char *what, int need_address);
static const char *net_class_name(uint8_t class_code, uint8_t subclass);
static uint16_t net_pci_io_base(const pci_device *dev);
static void net_show_pci(void);
static void net_show_interface(void);
static void net_show_arp(void);
static unsigned long net_irq_save(void);
static void net_irq_restore(unsigned long flags);
static void net_wait_turn(uint32_t seen, int timeout_ms);
static int  net_ping_once(uint32_t dst, uint16_t sequence, uint32_t *from);
static void net_ping(uint32_t dst, const char *name);

static void net_duration(uint32_t seconds);
static uint32_t net_dhcp_remaining(void);
static void net_dhcp_leased_ip(const char *label, uint32_t ip);
static void net_dhcp_step(int state);
static const char *net_dhcp_stalled(int reached);
static void net_dhcp_show_lease(void);
static void net_dhcp_run(void);

static void net_dns_explain_error(void);
static int  net_dns_resolve(const char *what, const char *name,
                            net_dns_answer *answer);
static void net_dns_show_cache(void);

static char *net_put_uint(char *p, uint32_t value);
static void net_tcp_endpoint_text(uint32_t ip, uint16_t port, char *text);
static void net_show_tcp(void);

static void usb_info_label(const char *label);
static const char *usb_speed_text(int speed);
static const char *usb_endpoint_type_text(uint8_t type);
static const char *usb_class_text(uint8_t iface_class, uint8_t subclass,
                                  uint8_t protocol);
static const char *usb_hc_kind_text(uint8_t prog_if);
static int  usb_uhci_on_bus(void);
static int  usb_show_hc_on_bus(void);
static void usb_explain_layering(void);
static void usb_explain_absent(void);
static void usb_show_frames(void);
static void usb_show_endpoints(const usb_device *dev);
static void usb_show_storage_line(const usb_device *dev);
static void usb_show_devices(void);
static void usb_show_counters(void);
static void usb_show_bus(void);

/* Self-test counters, maintained by mem_check(). */
static int mem_tests_run = 0;
static int mem_tests_ok = 0;

/* The same for page_check(). */
static int page_tests_run = 0;
static int page_tests_ok = 0;

/* And for user_check(). */
static int user_tests_run = 0;
static int user_tests_ok = 0;

/* And for gfx_check(). */
static int gfx_tests_run = 0;
static int gfx_tests_ok = 0;

/* Where the address the interface carries came from, so that "ifconfig" can
*  say. net_ip() cannot tell the two apart, and dhcp_state() only almost can:
*  a second exchange that fails leaves the state at FAILED while the address
*  from the first one is still configured and still working, and calling that
*  "set by hand" would be a lie about the one thing this line is for. So the
*  shell remembers it -- set where a lease is taken, cleared where "ifconfig"
*  overwrites it with typed numbers. */
static int net_address_from_lease = 0;

/* The pid of the status bar task, so the graphics commands can hold it while
*  the screen does not belong to the console -- see gfx_statusbar_hold().
*  Negative until main() has created the task. */
static int statusbar_pid = -1;

/* Copy of the visible text screen, taken before the mode switch and written
*  back after it. Mode 13h and text mode share the same video RAM, and the
*  framebuffer covers what the characters and attributes are stored in, so
*  drawing a picture wipes the console -- see gfx_text_store(). */
static unsigned short gfx_text_page[GFX_TEXT_CELLS];

void main()
{
	char cmd[256];
	char word[100];

    printf("eltomato's TomatOS 0.31 [Version 0.31 Build 2011/27/09]\n");
    printf("(c) Copyright 2006-2011 Jens Köhler\n\n");

	/* The pid is kept: "gfx" suspends this task for the duration of the
	   picture, because the bar writes straight into the text screen and the
	   screen is not a text screen while the picture is up. */
	statusbar_pid = taskmgr_add_task( update_infobar, "Statusbar Update Task", TASK_PRIORITY_HIGH );
	taskmgr_task_start(statusbar_pid);
	//taskmgr_task_start(taskmgr_add_task( task, "Test Task", TASK_PRIORITY_LOW ));

	do{
		printf("\n@TomatOS> ");

		scan(cmd);

		/* prmv() returns a pointer to a static buffer -- the first word is
		   therefore saved away before prmv() is called again anywhere else
		   (e.g. in taskmanager()). */
		strcpy(word, prmv(0, cmd));

		/* strcmp() now returns 0 on equality (the usual C semantics). */
		if(strcmp(word, "taskmgr") == 0) taskmanager(cmd);
		else if(strcmp(word, "mem") == 0) memory(cmd);
		else if(strcmp(word, "page") == 0) paging(cmd);
		else if(strcmp(word, "user") == 0) usermode(cmd);
		else if(strcmp(word, "ps") == 0) processes(cmd);
		else if(strcmp(word, "exec") == 0) execute(cmd);
		else if(strcmp(word, "df") == 0) diskfree(cmd);
		else if(strcmp(word, "gfx") == 0) graphics(cmd);
		else if(strcmp(word, "lspci") == 0) listpci(cmd);
		else if(strcmp(word, "lsusb") == 0) listusb(cmd);
		else if(strcmp(word, "ifconfig") == 0) netconfig(cmd);
		else if(strcmp(word, "dhcp") == 0) dhcpclient(cmd);
		else if(strcmp(word, "arp") == 0) arptable(cmd);
		else if(strcmp(word, "ping") == 0) pinghost(cmd);
		else if(strcmp(word, "nslookup") == 0) nslookup(cmd);
		else if(strcmp(word, "netstat") == 0) netstat(cmd);
		else if(strcmp(word, "mouse") == 0) mousepointer(cmd);
		else if(strcmp(word, "reboot") == 0) reboot();
		else if(strcmp(word, "help") == 0) help();
		else if(strcmp(word, "start") == 0) taskmgr_task_start(taskmgr_add_task( task, "Test Task", TASK_PRIORITY_LOW ));
		else if(strcmp(word, "exit") == 0) ; /* handled by the loop condition */
		/* Only an empty input silently brings up a new prompt. Everything
		   else is not necessarily a mistake: a word this list does not know
		   may be the name of a program on the disk, and run_program() is
		   what looks. "Unknown command" is what it says when there is no
		   such program either. */
		else if(word[0] != EOS) run_program(word, cmd);

	} while(strcmp(cmd, "exit") != 0);

	//taskmgr_killall();

	cls();
	printf("It is now safe to turn off your computer!");

	/* main() runs as a task and must not return. */
	for(;;);
}

void taskmanager(char *cmd)
{
	if(prmc(cmd)==0)
	{
		printf("Syntax: taskmgr [-l] [-k pid] [-s pid] [-r pid]\n");
		printf("\t-l        List tasks\n");
		printf("\t-k PID    Kill task\n");
		printf("\t-s PID    Suspend task\n");
		printf("\t-r PID    Resume task\n");
	}

	/* strcmp() returns 0 on equality -- the return value must therefore no
	   longer be used directly as a truth value. */
	if(strcmp(prmv(1, cmd), "-l") == 0) //List tasks
	{
		taskmgr_list_tasks();
	}

	if(strcmp(prmv(1, cmd), "-k") == 0) //Kill PID
	{
		if(prmc(cmd) < 2)
		{
			printf("No PID given!\n");
		} else {
			taskmgr_task_abort(atoi(prmv(2, cmd)), 0, "Canceled by user");
		}
	}

	if(strcmp(prmv(1, cmd), "-s") == 0) //Suspend PID
	{
		if(prmc(cmd) < 2)
		{
			printf("No PID given!\n");
		} else {
			taskmgr_task_suspend(atoi(prmv(2, cmd)));
		}
	}
	if(strcmp(prmv(1, cmd), "-r") == 0) //Resume PID
	{
		if(prmc(cmd) < 2)
		{
			printf("No PID given!\n");
		} else {
			taskmgr_task_start(atoi(prmv(2, cmd)));
		}
	}

}

/* --- mem ---------------------------------------------------------------- */

/* Prints value right-aligned in a field of the given width. printf() knows no
   field widths like %5i, so we count the digits ourselves and put the missing
   spaces in front. */
static void mem_print_right(uint32_t value, int width)
{
	uint32_t rest;
	int digits;

	digits = 1;
	rest = value;
	while(rest >= 10)
	{
		rest = rest / 10;
		digits++;
	}

	while(digits < width)
	{
		putch(' ');
		digits++;
	}

	printf("%u", (int)value);
}

/* One row of the table: label, MiB, KiB and frame count.
   Everything is computed from the frame count so that the three rows
   match up: 1 frame = 4 KiB, 256 frames = 1 MiB. */
static void mem_print_frames(char *label, uint32_t frames)
{
	printf("%s", label);
	mem_print_right(frames / 256, 6);
	mem_print_right(frames * (PMM_FRAME_SIZE / 1024), 11);
	mem_print_right(frames, 12);
	printf("\n");
}

static void mem_show_status(void)
{
	uint32_t used;
	uint32_t heapused;
	uint32_t heaptotal;

	used = pmm_used_frames();
	heapused = heap_used();
	heaptotal = heap_total();

	printf("Memory usage:\n");
	printf("                     MiB        KiB       Frames\n");
	mem_print_frames("  Physical total: ", pmm_total_frames());
	mem_print_frames("  Physical used:  ", used);
	mem_print_frames("  Physical free:  ", pmm_free_frames_count());
	printf("  Frame size %u bytes, memory map %u KiB usable\n",
	       (int)PMM_FRAME_SIZE, (int)(pmm_total_bytes() / 1024));

	printf("  Heap used:    ");
	mem_print_right(heapused, 8);
	printf(" bytes (");
	mem_print_right(heapused / 1024, 6);
	printf(" KiB)\n");

	printf("  Heap total:   ");
	mem_print_right(heaptotal, 8);
	printf(" bytes (");
	mem_print_right(heaptotal / 1024, 6);
	printf(" KiB)\n");
}

/* Records the result of a check and writes the marker to the start of the
   line. The rest of the line is printed by the caller. */
static void mem_check(int ok)
{
	mem_tests_run++;

	if(ok)
	{
		mem_tests_ok++;
		printf("  [  OK  ] ");
	} else {
		printf("  [FAILED] ");
	}
}

/* Pattern that is written into the test blocks. Deliberately depends on the
   position so that a moved or overwritten block stands out. */
static unsigned char mem_pattern(int i)
{
	return (unsigned char)((i * 7 + 3) & 0xFF);
}

static void mem_selftest(void)
{
	unsigned char *a;
	unsigned char *b;
	unsigned char *c;
	unsigned char *r;
	unsigned char *r2;
	uint32_t base;
	uint32_t used_before;
	uint32_t used_after;
	int i;
	int good;
	int good2;

	mem_tests_run = 0;
	mem_tests_ok = 0;
	base = heap_used();

	printf("Heap self-test:\n");
	printf("  Start: %u bytes used, %u bytes requested\n",
	       (int)base, (int)heap_total());

	/* 1. malloc() returns memory that can be written to and read back
	      again. */
	a = (unsigned char *)malloc(MEM_TEST_SIZE);
	good = 0;
	if(a != 0)
	{
		for(i = 0; i < MEM_TEST_SIZE; i++)
		{
			a[i] = mem_pattern(i);
		}
		for(i = 0; i < MEM_TEST_SIZE; i++)
		{
			if(a[i] == mem_pattern(i)) good++;
		}
	}
	mem_check(a != 0 && good == MEM_TEST_SIZE);
	printf("malloc(%i) = 0x%X, pattern %i/%i bytes\n",
	       MEM_TEST_SIZE, (int)a, good, MEM_TEST_SIZE);

	/* 2. A second allocation must neither overlap nor damage the first --
	      b is therefore filled completely and a is checked afterwards. */
	b = (unsigned char *)malloc(MEM_TEST_SIZE);
	good2 = 0;
	if(a != 0 && b != 0)
	{
		memset(b, (char)0xAA, MEM_TEST_SIZE);
		for(i = 0; i < MEM_TEST_SIZE; i++)
		{
			if(a[i] == mem_pattern(i)) good2++;
		}
	}
	mem_check(a != 0 && b != 0 && good2 == MEM_TEST_SIZE &&
	          (a + MEM_TEST_SIZE <= b || b + MEM_TEST_SIZE <= a));
	printf("0x%X and 0x%X separate, %i/%i bytes untouched\n",
	       (int)a, (int)b, good2, MEM_TEST_SIZE);

	/* 3. After free() the same size must come out of the freed space
	      again, so the heap does not grow without end. */
	used_before = heap_used();
	free(a);
	free(b);
	a = (unsigned char *)malloc(MEM_TEST_SIZE);
	b = (unsigned char *)malloc(MEM_TEST_SIZE);
	used_after = heap_used();
	mem_check(a != 0 && b != 0 && used_after == used_before);
	printf("free + malloc: used %u -> %u bytes\n",
	       (int)used_before, (int)used_after);
	free(a);
	free(b);

	/* 4. calloc() must return zeroed memory. */
	c = (unsigned char *)calloc(16, 4);
	good = 0;
	if(c != 0)
	{
		for(i = 0; i < MEM_TEST_SIZE; i++)
		{
			if(c[i] == 0) good++;
		}
	}
	mem_check(c != 0 && good == MEM_TEST_SIZE);
	printf("calloc(16,4) = 0x%X, %i/%i bytes zeroed\n",
	       (int)c, good, MEM_TEST_SIZE);
	free(c);

	/* 5. realloc() must carry the previous contents over. */
	r = (unsigned char *)malloc(MEM_TEST_SMALL);
	if(r != 0)
	{
		for(i = 0; i < MEM_TEST_SMALL; i++)
		{
			r[i] = mem_pattern(i);
		}
	}
	r2 = (unsigned char *)realloc(r, MEM_TEST_LARGE);
	good = 0;
	if(r2 != 0)
	{
		for(i = 0; i < MEM_TEST_SMALL; i++)
		{
			if(r2[i] == mem_pattern(i)) good++;
		}
	}
	mem_check(r != 0 && r2 != 0 && good == MEM_TEST_SMALL);
	printf("realloc(%i -> %i) = 0x%X, %i/%i bytes kept\n",
	       MEM_TEST_SMALL, MEM_TEST_LARGE, (int)r2, good, MEM_TEST_SMALL);
	if(r2 != 0)
	{
		free(r2);
	} else {
		free(r);
	}

	/* 6. free(0) is allowed and must not crash. If we get back out of it,
	      the check has passed. */
	free(NULL);
	mem_check(1);
	printf("free(0) survived\n");

	/* 7. Everything requested has been given back -- the heap must be back
	      at its starting value. */
	used_after = heap_used();
	mem_check(used_after == base);
	printf("Heap back at %u bytes (start %u bytes)\n",
	       (int)used_after, (int)base);

	printf("  Result: %i of %i checks passed\n",
	       mem_tests_ok, mem_tests_run);
}

void memory(char *cmd)
{
	char opt[100];

	if(prmc(cmd) == 0)
	{
		mem_show_status();
		return;
	}

	strcpy(opt, prmv(1, cmd));

	if(strcmp(opt, "-t") == 0)
	{
		mem_selftest();
	} else {
		printf("Syntax: mem [-t]\n");
		printf("\t          Show memory usage\n");
		printf("\t-t        Run the heap self-test\n");
	}
}

/* --- page --------------------------------------------------------------- */

/* Prints value as a zero padded hex number with a fixed number of digits.
   printf("%X") writes as many digits as the value needs, which would make
   the address columns dance around -- so the digits are put out by hand,
   the same idea as mem_print_right() for decimal numbers. */
static void page_print_hex(uint32_t value, int digits)
{
	char *hexdigit = "0123456789ABCDEF";
	int i;

	printf("0x");
	for(i = digits - 1; i >= 0; i--)
	{
		putch((unsigned char)hexdigit[(value >> (i * 4)) & 0xF]);
	}
}

/* One row of the translation table: label, virtual address, physical
   address behind it and whether the page is mapped at all. The label
   carries its own padding so the columns line up.
   In the higher half layout the two columns differ by KERNEL_VIRTUAL_BASE,
   which is the whole point of printing them side by side: kernel code shows
   up as 0xC01xxxxx -> 0x001xxxxx. */
static void page_print_translation(char *label, uint32_t virt)
{
	printf("%s", label);
	page_print_hex(virt, 8);
	printf(" -> ");

	if(vmm_is_mapped(virt))
	{
		page_print_hex(vmm_get_phys(virt), 8);
		printf("  mapped\n");
	} else {
		/* Same width as an address, so the last column stays put. */
		printf("----------  unmapped\n");
	}
}

static void page_show_status(void)
{
	uint32_t pages;
	unsigned char *block;

	printf("Paging state:\n");

	if(!vmm_enabled())
	{
		printf("  Paging is not enabled.\n");
		return;
	}

	pages = vmm_mapped_pages();

	printf("  Status:          enabled\n");
	printf("  Page directory:  ");
	page_print_hex(vmm_directory_phys(), 8);
	printf("  (CR3)\n");

	printf("  Page tables:  ");
	mem_print_right(vmm_table_count(), 5);
	printf("   Mapped pages: ");
	mem_print_right(pages, 7);
	printf(" (");
	mem_print_right(pages / (1024 * 1024 / PAGE_SIZE), 5);
	printf(" MiB)\n");

	/* A few translations that make the higher half split tangible: all of
	   them drop by KERNEL_VIRTUAL_BASE, page zero has no physical address at
	   all. The heap block is only borrowed for the duration of the
	   printout. */
	block = (unsigned char *)malloc(MEM_TEST_SIZE);

	printf("  Translations (virtual -> physical, offset ");
	page_print_hex((uint32_t)KERNEL_VIRTUAL_BASE, 8);
	printf("):\n");
	page_print_translation("    Kernel code  ", (uint32_t)main);
	page_print_translation("    VGA text     ", PAGE_VGA_TEXT);
	if(block != 0)
	{
		page_print_translation("    Heap block   ", (uint32_t)block);
	}
	page_print_translation("    Page zero    ", (uint32_t)0);

	free(block);
}

/* Records the result of a check and writes the marker to the start of the
   line. The rest of the line is printed by the caller. Both markers are
   eight characters wide, so the text behind them lines up. */
static void page_check(int ok)
{
	page_tests_run++;

	if(ok)
	{
		page_tests_ok++;
		printf("  [  OK  ] ");
	} else {
		printf("  [FAILED] ");
	}
}

/* Direct mapping for one address: the physical address must be exactly
   KERNEL_VIRTUAL_BASE below the virtual one. This replaces the old identity
   check -- with the kernel in the higher half the relation is no longer
   "equal" but "constant offset", and V2P() is what states it. Only valid for
   addresses inside the direct mapping window, i.e. at or above
   KERNEL_VIRTUAL_BASE. */
static void page_check_offset(char *label, uint32_t virt)
{
	uint32_t phys;
	uint32_t want;

	phys = vmm_get_phys(virt);
	want = (uint32_t)V2P(virt);
	page_check(virt >= KERNEL_VIRTUAL_BASE && phys == want);
	printf("%s", label);
	page_print_hex(virt, 8);
	printf(" -> ");
	page_print_hex(phys, 8);
	printf(" = V2P\n");
}

/* First virtual address from PAGE_TEST_VIRT upwards whose page is not
   mapped. Asking the vmm itself is the only way to be sure the address
   collides with nothing in use. Returns 0 if everything probed is taken. */
static uint32_t page_find_free_virt(void)
{
	uint32_t virt;
	int i;

	virt = (uint32_t)PAGE_TEST_VIRT;
	for(i = 0; i < PAGE_TEST_PROBES; i++)
	{
		if(!vmm_is_mapped(virt)) return virt;
		virt += PAGE_SIZE;
	}

	return 0;
}

static void page_selftest(void)
{
	unsigned char *block;
	uint32_t frame_phys;
	volatile uint32_t *window;
	volatile uint32_t *direct;
	uint32_t virt;
	uint32_t base;
	uint32_t base_phys;
	uint32_t off_phys;
	uint32_t heap_addr;
	uint32_t seen;
	int mapped;
	int ok;

	page_tests_run = 0;
	page_tests_ok = 0;

	printf("Paging self-test:\n");

	if(!vmm_enabled())
	{
		printf("  Paging is not enabled -- nothing to test.\n");
		return;
	}

	printf("  Directory ");
	page_print_hex(vmm_directory_phys(), 8);
	printf(", %u tables, %u pages mapped\n",
	       (int)vmm_table_count(), (int)vmm_mapped_pages());

	/* 1.-3. The direct mapping must hold everywhere, not just where the
	         kernel happens to live -- code, heap and the VGA buffer sit in
	         three different regions, and all three have to come out exactly
	         KERNEL_VIRTUAL_BASE lower. */
	block = (unsigned char *)malloc(MEM_TEST_SIZE);
	heap_addr = (uint32_t)block;

	page_check_offset("Kernel   ", (uint32_t)main);
	page_check_offset("VGA text ", PAGE_VGA_TEXT);
	page_check_offset("Heap     ", heap_addr);

	/* 4. A translation must keep the offset within the page. An address in
	      the middle of a page therefore has to come out that much behind
	      the physical address of the page start. */
	base = (uint32_t)main & ~((uint32_t)PAGE_SIZE - 1);
	base_phys = vmm_get_phys(base);
	off_phys = vmm_get_phys(base + PAGE_TEST_OFFSET);
	page_check(base_phys != 0 && off_phys == base_phys + PAGE_TEST_OFFSET);
	printf("Offset: page ");
	page_print_hex(base, 8);
	printf(" + 0x123 -> ");
	page_print_hex(off_phys, 8);
	printf("\n");

	/* 5. Page zero stays unmapped, so a null pointer faults instead of
	      quietly reading the interrupt vector table. Checked by asking the
	      vmm -- dereferencing null here would kill the shell task. */
	page_check(!vmm_is_mapped(0) && vmm_get_phys(0) == 0);
	printf("Page zero: is_mapped(0) = %i, get_phys(0) = %i (null trap)\n",
	       vmm_is_mapped(0), (int)vmm_get_phys(0));

	/* 6. Whatever the heap hands out has to lie in mapped memory --
	      otherwise malloc() would be handing out page faults. */
	page_check(block != 0 && vmm_is_mapped(heap_addr));
	printf("Heap block ");
	page_print_hex(heap_addr, 8);
	printf(" lies in mapped memory\n");
	free(block);

	/* 7. Map a fresh frame at a virtual address that is provably free,
	      write to it and read it back. This is the one check that exercises
	      vmm_map() rather than just inspecting what vmm_init() built.
	      pmm_alloc_frame() returns a PHYSICAL address -- it goes into
	      vmm_map() as is, and is never dereferenced without P2V(). The
	      virtual address comes from below the kernel window, see
	      PAGE_TEST_VIRT, so the new mapping is one the direct mapping does
	      not already provide. */
	frame_phys = (uint32_t)pmm_alloc_frame();
	virt = page_find_free_virt();
	mapped = 0;
	ok = 0;

	if(frame_phys != 0 && virt != 0 &&
	   vmm_map(virt, frame_phys, PAGE_PRESENT | PAGE_WRITE) == 0)
	{
		mapped = 1;
		window = (volatile uint32_t *)virt;
		window[0] = (uint32_t)PAGE_TEST_PATTERN;
		window[1] = ~(uint32_t)PAGE_TEST_PATTERN;
		window[PAGE_SIZE / 4 - 1] = (uint32_t)PAGE_TEST_PATTERN;

		ok = (window[0] == (uint32_t)PAGE_TEST_PATTERN &&
		      window[1] == ~(uint32_t)PAGE_TEST_PATTERN &&
		      window[PAGE_SIZE / 4 - 1] == (uint32_t)PAGE_TEST_PATTERN &&
		      vmm_get_phys(virt) == frame_phys &&
		      vmm_is_mapped(virt));
	}
	page_check(ok);
	printf("Mapped ");
	page_print_hex(virt, 8);
	printf(" -> ");
	page_print_hex(frame_phys, 8);
	printf(", pattern read back\n");

	/* 8. The new mapping must point at the very same physical page. The
	      second way to that page is the direct mapping, so P2V() of the
	      frame has to show the pattern too -- written through a virtual
	      address below the kernel, read back through one above it. */
	ok = 0;
	seen = 0;
	if(mapped && vmm_is_mapped((uint32_t)P2V(frame_phys)))
	{
		direct = (volatile uint32_t *)P2V(frame_phys);
		seen = direct[0];
		ok = (seen == (uint32_t)PAGE_TEST_PATTERN);
	}
	page_check(ok);
	printf("Frame ");
	page_print_hex(frame_phys, 8);
	printf(" via P2V reads ");
	page_print_hex(seen, 8);
	printf("\n");

	/* 9. After vmm_unmap() the address must be gone for good. */
	if(mapped) vmm_unmap(virt);
	page_check(mapped && !vmm_is_mapped(virt) && vmm_get_phys(virt) == 0);
	printf("Unmapped ");
	page_print_hex(virt, 8);
	printf(": is_mapped = %i\n", vmm_is_mapped(virt));

	pmm_free_frame((void *)frame_phys);

	printf("  Result: %i of %i checks passed\n",
	       page_tests_ok, page_tests_run);
}

/* Deliberate null pointer write, only reachable via "page -f". The pointer
   itself is volatile so the compiler has to load it and cannot fold the
   access into an undefined-behaviour trap of its own making. */
static uint32_t * volatile page_fault_target = 0;

static void page_fault_demo(void)
{
	printf("Writing to the null pointer on purpose.\n");
	printf("The page fault handler takes over from here -- this task dies.\n");

	*page_fault_target = (uint32_t)PAGE_TEST_PATTERN;

	/* Not reached while page zero stays unmapped. */
	printf("No fault happened -- page zero appears to be mapped!\n");
}

void paging(char *cmd)
{
	char opt[100];

	if(prmc(cmd) == 0)
	{
		page_show_status();
		return;
	}

	strcpy(opt, prmv(1, cmd));

	if(strcmp(opt, "-t") == 0)
	{
		page_selftest();
	}
	else if(strcmp(opt, "-f") == 0)
	{
		page_fault_demo();
	} else {
		printf("Syntax: page [-t] [-f]\n");
		printf("\t          Show the paging state\n");
		printf("\t-t        Run the paging self-test\n");
		printf("\t-f        Fault on the null pointer on purpose,\n");
		printf("\t          which kills the current task\n");
	}
}

/* --- user --------------------------------------------------------------- */

/* The ring 3 side of the system call interface, in the spirit of a minimal
   libc: one wrapper per call, and underneath them the two functions that
   actually issue the int.

   The convention is the one syscall.h lays down -- call number in eax, up to
   three arguments in ebx, ecx and edx, result in eax. That is what the "=a"
   output and the "a"/"b" inputs express; no call this kernel has needs more
   than one argument yet, so ecx and edx stay unused for now.

   Two constraints deserve a word:

     "b"      GCC will not hand out ebx as an operand when it compiles
              position independent code, because there ebx is reserved for the
              GOT pointer. This build passes -fno-pic -fno-pie (a kernel
              linked to a fixed address must not be PIC), so ebx is an
              ordinary register here and the constraint is safe. Should that
              flag ever disappear, these wrappers have to save and restore ebx
              around the int themselves.

     "memory" The kernel may read (SYS_WRITE) or write memory on our behalf
              during the call, so nothing may be kept in a register across it.

   always_inline is not decoration either. user_demo() below executes with
   CPL 3, and the only code it may execute is the copy of the .usertext
   section that user_setup() maps for it. A real call to an out-of-line
   sys_call1() would leave that copy -- and, worse, would be a call to an
   absolute address that no longer says anything after the copy. Inlining
   keeps every ring 3 routine self contained. */
#define USER_INLINE static __inline__ __attribute__((always_inline))

USER_INLINE int sys_call0(int nr)
{
	int ret;
	__asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(nr) : "memory");
	return ret;
}

USER_INLINE int sys_call1(int nr, int arg)
{
	int ret;
	__asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(nr), "b"(arg) : "memory");
	return ret;
}

USER_INLINE void u_exit(int status)  { sys_call1(SYS_EXIT, status); }
USER_INLINE int  u_write(char *text) { return sys_call1(SYS_WRITE, (int)text); }
USER_INLINE int  u_getpid(void)      { return sys_call0(SYS_GETPID); }
USER_INLINE int  u_sleep(int ms)     { return sys_call1(SYS_SLEEP, ms); }
USER_INLINE int  u_putch(int c)      { return sys_call1(SYS_PUTCH, c); }
USER_INLINE int  u_uptime(void)      { return sys_call0(SYS_UPTIME); }

/* Decimal output for ring 3. printf() is a kernel function and out of reach,
   so the digits are produced on the stack and pushed out one SYS_PUTCH at a
   time. The buffer is an automatic array that is never initialised from a
   literal, so it costs nothing in .rodata. */
USER_INLINE void u_putuint(unsigned int value)
{
	char digits[12];
	int i;

	i = 0;
	do
	{
		digits[i] = (char)('0' + (value % 10u));
		value = value / 10u;
		i++;
	} while(value != 0 && i < 11);

	while(i > 0)
	{
		i--;
		u_putch((int)digits[i]);
	}
}

/* The demo, and one of the two routines in this file that run with CPL 3.
*
*  It may touch exactly three things: its own stack, which the task manager
*  maps one page of for every ring 3 task, the page at USER_PAGE_VIRT, and the
*  copy of .usertext it is executing out of. Every other page in the system is
*  mapped without PAGE_USER and faults on the first access from here.
*
*  It executes out of that copy, not out of the kernel image, so it has to be
*  position independent: no absolute reference may leave the section. Calls
*  between routines in .usertext are relative and therefore fine -- the whole
*  section is copied as one block -- but a call to anything outside it, or a
*  pointer to a kernel object taken here, would be an address that means
*  nothing at the copy's location. See the note on USER_INLINE above; that is
*  what always_inline on the system call wrappers is for.
*
*  That rules out more than the obvious. printf() and putch() are kernel
*  functions and unreachable. So is every kernel global. And so -- this is the
*  part that is easy to miss -- is every string literal: a literal lives in
*  .rodata, which after vmm_init() sits somewhere around 0xC01xxxxx with no
*  PAGE_USER bit. Merely computing its address would be fine, but the moment
*  such a pointer went to SYS_WRITE the kernel would refuse it with SYS_EFAULT
*  (it rejects everything at or above KERNEL_VIRTUAL_BASE), and reading the
*  bytes here in ring 3 would fault outright.
*
*  So the strings this routine prints are not in this routine. user_setup()
*  copies them into the user page before ring 3 is ever entered, and the code
*  below names them by their fixed virtual address. USER_MSG_HELLO and its
*  siblings are compile time constants, so they end up as immediate operands
*  inside the instruction stream -- no load from kernel memory, no relocation
*  into it. The one thing ring 3 is allowed to know about the address space is
*  a number.
*
*  The other rule is that this function must not return: a freshly built user
*  stack has no return address on it. SYS_EXIT is the way out.
*
*  Since tasks got address spaces of their own, the string page is no longer
*  there when this task starts: it is mapped in the shell's directory, and
*  this task has its own. The shell puts it into this task's space while the
*  sleep below runs -- SYS_SLEEP needs no memory beyond the stack, so it is
*  the one call that is safe to make before that has happened. */
USER_TEXT static void user_demo(void)
{
	int pid;
	int before;
	int after;

	u_sleep(USER_MAP_DELAY);

	u_write((char *)USER_MSG_HELLO);

	pid = u_getpid();
	u_write((char *)USER_MSG_PID);
	u_putuint((unsigned int)pid);
	u_putch('\n');

	before = u_uptime();
	u_sleep(USER_DEMO_SLEEP);
	after = u_uptime();

	u_write((char *)USER_MSG_SLEPT);
	u_putuint((unsigned int)(after - before));
	u_write((char *)USER_MSG_BYE);

	u_exit(0);

	/* Not reached. If SYS_EXIT ever did come back, spinning here is still
	   better than returning into nothing. */
	for(;;);
}

/* The ring 3 half of the isolation test. Both tasks run THIS function -- what
*  makes them behave differently is not their code but what they find at
*  USER_ISO_VIRT, and that is the point: the same instructions, reading the
*  same address, get different numbers, because the two tasks look at
*  different frames.
*
*  The schedule is built so that the writes of the two tasks fall between each
*  other's write and read-back. With one step of USER_ISO_STEP ms, and the
*  second task starting one step late:
*
*      t = 0    task A writes A
*      t = 1    task B writes B
*      t = 2    task A reads back -- and writes A + 1
*      t = 3    task B reads back -- and writes B + 1
*      t = 4    task A reads back
*      t = 5    task B reads back
*
*  On one shared page every single one of those four read-backs would return
*  the value the *other* task wrote in the meantime: A would read B at t = 2,
*  B would read A + 1 at t = 3, and so on. There is no ordering of two tasks
*  sharing one page in which all four read-backs still return their own value,
*  so the test cannot pass by luck of scheduling -- it can only pass if the
*  two writes went to two different frames.
*
*  Sleeping is what buys that ordering: SYS_SLEEP waits for wall clock time,
*  not for a time slice, so "the other task has written by now" is true even
*  if the scheduler runs the two tasks in a lopsided way.
*
*  Everything else follows the rules user_demo() lays out: strings come out of
*  the string page, the routine never returns, and it must not touch a page
*  before the shell has mapped it -- hence the sleep at the top. */
USER_TEXT static void user_iso_task(void)
{
	volatile uint32_t *page;
	unsigned int role;
	unsigned int value;
	unsigned int first;
	unsigned int second;
	int ok;

	u_sleep(USER_MAP_DELAY);

	page = (volatile uint32_t *)USER_ISO_VIRT;
	role = page[USER_ISO_ROLE];
	value = page[USER_ISO_VALUE];

	/* The second task starts one step later than the first, so that the two
	   writes end up between each other's write and read-back. */
	if(role != 0) u_sleep(USER_ISO_STEP);

	page[USER_ISO_CELL] = value;
	u_sleep(2 * USER_ISO_STEP);
	first = page[USER_ISO_CELL];

	page[USER_ISO_CELL] = value + 1;
	u_sleep(2 * USER_ISO_STEP);
	second = page[USER_ISO_CELL];

	ok = (first == value && second == value + 1);

	u_write((char *)USER_MSG_ISO_PID);
	u_putuint((unsigned int)u_getpid());
	u_write((char *)USER_MSG_ISO_WROTE);
	u_putuint(value);
	u_write((char *)USER_MSG_ISO_AND);
	u_putuint(value + 1);
	u_write((char *)USER_MSG_ISO_READ);
	u_putuint(first);
	u_write((char *)USER_MSG_ISO_AND);
	u_putuint(second);

	if(ok)
	{
		u_write((char *)USER_MSG_ISO_OWN);
	} else {
		u_write((char *)USER_MSG_ISO_LOST);
	}

	/* The status is what the task manager records for the slot, so the
	   verdict survives the task -- "taskmgr -l" shows it. */
	u_exit(ok ? 0 : 1);

	for(;;);
}

/* Physical frame behind the user page, and whether the setup has run. */
static uint32_t user_page_frame = 0;
static int user_page_ready = 0;

/* Copies one message into its slot in the user page. Bounded, so a message
   that outgrows its slot is cut short instead of running into the next one. */
static void user_store(uint32_t at, char *text)
{
	char *dest;
	int i;

	dest = (char *)at;
	for(i = 0; i < USER_MSG_SIZE - 1 && text[i] != EOS; i++)
	{
		dest[i] = text[i];
	}
	dest[i] = EOS;
}

/* The frames the ring 3 code was copied into, and how many are in use. Kept
*  so that a setup which fails half way can hand back what it already took. */
static uint32_t user_text_frames[USER_TEXT_MAX];
static int user_text_pages = 0;

/* Undoes a partial user_text_map(): drops the mappings made so far and gives
*  the frames behind them back to the pmm. */
static void user_text_unmap(void)
{
	int i;

	for(i = 0; i < user_text_pages; i++)
	{
		vmm_unmap((uint32_t)USER_TEXT_VIRT + (uint32_t)i * PAGE_SIZE);
		pmm_free_frame((void *)user_text_frames[i]);
		user_text_frames[i] = 0;
	}

	user_text_pages = 0;
}

/* Puts the ring 3 code where ring 3 may fetch it, once.
*
*  What this does NOT do any more is re-map kernel text with PAGE_USER. That
*  used to be how ring 3 reached user_demo(), and it was the coarse move in
*  the whole exercise: a page is the smallest thing paging can talk about, so
*  whatever else the linker had put next to user_demo() -- kernel code, all of
*  it -- became readable from ring 3 along with it.
*
*  So the ring 3 routines are linked into .usertext instead, that range is
*  copied into frames of its own through the direct mapping, and only the copy
*  is mapped for ring 3. The pages that carry it then hold ring 3 code and
*  nothing else, and not one page of kernel .text is user accessible.
*
*  The flags are PAGE_PRESENT | PAGE_USER, deliberately without PAGE_WRITE:
*  read and execute, no write. Ring 3 cannot patch its own code, and neither
*  can the kernel through this address -- the writable view is the direct
*  mapping used for the copy, which no ring 3 task can reach.
*
*  Since the code is fetched from the copy, the routines run at an address
*  they were not linked for. That is what the position independence rule in
*  the comment on user_demo() is about, and what makes user_text_entry()
*  necessary below. */
static int user_text_map(void)
{
	uint32_t bytes;
	uint32_t virt;
	uint32_t frame;
	int pages;
	int i;

	/* Page aligned at both ends by the linker script, so the division is
	*  exact and the copy below never touches a neighbouring section. */
	bytes = (uint32_t)usertext_end - (uint32_t)usertext_start;
	pages = (int)(bytes / (uint32_t)PAGE_SIZE);

	if(pages <= 0 || pages > USER_TEXT_MAX)
	{
		printf("user: .usertext spans %i pages, which is not between 1 and %i.\n",
		       pages, USER_TEXT_MAX);
		return 0;
	}

	for(i = 0; i < pages; i++)
	{
		virt = (uint32_t)USER_TEXT_VIRT + (uint32_t)i * PAGE_SIZE;

		/* The address is a fixed constant, so it is checked rather than
		*  assumed: anything already living there would be overwritten. */
		if(vmm_is_mapped(virt))
		{
			printf("user: ");
			page_print_hex(virt, 8);
			printf(" is taken, the ring 3 code has nowhere to go.\n");
			user_text_unmap();
			return 0;
		}

		frame = (uint32_t)pmm_alloc_frame();
		if(frame == 0)
		{
			printf("user: no free frame for the ring 3 code.\n");
			user_text_unmap();
			return 0;
		}

		/* Through the direct mapping, i.e. the kernel's own writable view
		*  of the frame. The user mapping created right after is read only,
		*  so this is the only moment the page is ever written. */
		memcpy(P2V(frame), usertext_start + (uint32_t)i * PAGE_SIZE,
		       (size_t)PAGE_SIZE);

		if(vmm_map(virt, frame, PAGE_PRESENT | PAGE_USER) != 0)
		{
			pmm_free_frame((void *)frame);
			printf("user: the ring 3 code page at ");
			page_print_hex(virt, 8);
			printf(" could not be mapped.\n");
			user_text_unmap();
			return 0;
		}

		user_text_frames[i] = frame;
		user_text_pages = i + 1;
	}

	return 1;
}

/* Where a ring 3 routine ended up in the user mapping.
*
*  The routines are linked into .usertext but executed from the copy, so the
*  linked address of user_demo() is not the address to start a task at. What
*  survives the copy is the routine's OFFSET inside the section -- the copy is
*  the section, byte for byte -- so the entry point is that offset added to
*  where the copy is mapped. */
static void *user_text_entry(void *fn)
{
	return (void *)((uint32_t)USER_TEXT_VIRT +
	                ((uint32_t)fn - (uint32_t)usertext_start));
}

/* Prepares everything ring 3 needs, once. Returns 1 when the demo can be
*  entered, 0 otherwise.
*
*  Two mappings are involved, opened for quite different reasons:
*
*    - one fresh frame at USER_PAGE_VIRT, present, writable and PAGE_USER.
*      This is the demo's data, i.e. its strings and nothing else. It is
*      zeroed first: the pmm hands out frames with the previous owner's
*      contents still in them, and ring 3 is about to be able to read them.
*
*    - the copy of .usertext at USER_TEXT_VIRT, present and PAGE_USER but not
*      writable, so ring 3 can fetch its own instructions. See
*      user_text_map().
*
*  What is deliberately *not* opened is anything else: not .text, not .rodata.
*  The strings live in the user page precisely so that the code exception
*  stays an exception about code. */
static int user_setup(void)
{
	if(user_page_ready) return 1;

	if(!vmm_enabled())
	{
		printf("user: paging is off -- there is no ring 3 to enter.\n");
		return 0;
	}

	user_page_frame = (uint32_t)pmm_alloc_frame();
	if(user_page_frame == 0)
	{
		printf("user: no free frame for the user data page.\n");
		return 0;
	}

	if(vmm_map((uint32_t)USER_PAGE_VIRT, user_page_frame,
	           PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0)
	{
		pmm_free_frame((void *)user_page_frame);
		user_page_frame = 0;
		printf("user: the user data page could not be mapped.\n");
		return 0;
	}

	memset((void *)USER_PAGE_VIRT, (char)0, (size_t)PAGE_SIZE);

	user_store(USER_MSG_HELLO, "\n[ring 3] Hello from user mode!\n");
	user_store(USER_MSG_PID,   "[ring 3] getpid() says ");
	user_store(USER_MSG_SLEPT, "[ring 3] uptime moved on by ");
	user_store(USER_MSG_BYE,   " ms, now exit(0).\n");
	user_store(USER_MSG_TEST,  "SYS_WRITE through a user page\n");

	user_store(USER_MSG_ISO_PID,   "[ring 3] pid ");
	user_store(USER_MSG_ISO_WROTE, " wrote ");
	user_store(USER_MSG_ISO_AND,   " and ");
	user_store(USER_MSG_ISO_READ,  ", read back ");
	user_store(USER_MSG_ISO_OWN,   " -- own values throughout\n");
	user_store(USER_MSG_ISO_LOST,  " -- CLOBBERED, the page is shared!\n");

	if(!user_text_map()) return 0;

	user_page_ready = 1;
	return 1;
}

/* --- handing pages to a task -------------------------------------------- */

/* Which address space a task runs in, sampled from the timer interrupt.
*  Filled in by user_space_probe() below, read by user_space_wait(). */
#define USER_SPACE_SLOTS 2

static volatile int user_watch_pid[USER_SPACE_SLOTS] = { -1, -1 };
static volatile uint32_t user_watch_space[USER_SPACE_SLOTS] = { 0, 0 };
static int user_watch_active = 0;

/* Timer handler, so it runs on every tick, in the context of whatever task
*  the tick interrupted -- irq_handler() calls the installed handlers before
*  schedule(), so CR3 and taskmgr_get_currpid() both still describe that task
*  and not the one that will run next. vmm_current_space() called here
*  therefore returns the directory of the interrupted task, which is how the
*  shell learns the identifier of a task other than itself.
*
*  A slot is written once and then left alone, so the value cannot change
*  under the reader. */
static void user_space_probe(struct regs *r)
{
	int pid;
	int i;

	(void)r;

	pid = taskmgr_get_currpid();
	if(pid < 0) return;

	for(i = 0; i < USER_SPACE_SLOTS; i++)
	{
		if(user_watch_pid[i] == pid && user_watch_space[i] == 0)
		{
			user_watch_space[i] = (uint32_t)vmm_current_space();
		}
	}
}

/* Arms a slot for a pid. Must happen before the task is started, otherwise
*  the task could run past its opening sleep unobserved. The handler is
*  installed once for however many slots are armed, so it stays one call per
*  tick no matter how many tasks are being watched. */
static void user_watch_arm(int slot, int pid)
{
	user_watch_pid[slot] = pid;
	user_watch_space[slot] = 0;

	if(!user_watch_active)
	{
		if(timer_install_handler(user_space_probe) < 0)
		{
			printf("user: no free timer handler -- address spaces stay unknown.\n");
			return;
		}
		user_watch_active = 1;
	}
}

static void user_watch_stop(void)
{
	int i;

	if(user_watch_active) timer_uninstall_handler(user_space_probe);
	user_watch_active = 0;

	for(i = 0; i < USER_SPACE_SLOTS; i++)
	{
		user_watch_pid[i] = -1;
		user_watch_space[i] = 0;
	}
}

/* Waits until the task in that slot has been seen on the CPU, at most
*  USER_SPACE_WAIT ms. Sleeping is what lets it get there in the first place.
*  Returns 0 if the task never ran -- the caller then has nothing to map into
*  and says so instead of guessing. */
static addrspace_t user_space_wait(int slot)
{
	int waited;

	for(waited = 0; waited < USER_SPACE_WAIT; waited += USER_SPACE_POLL)
	{
		if(user_watch_space[slot] != 0) break;
		sleep(USER_SPACE_POLL);
	}

	return (addrspace_t)user_watch_space[slot];
}

/* Hands a page to a task, at the same address the shell knows it by. Returns
*  1 on success.
*
*  A space of 0 means the task was never seen on the CPU, so there is no
*  directory to map into and no amount of trying will produce one -- refused
*  here rather than passed to vmm_map_in(), which would take the 0 for a
*  physical address and walk a directory that is not there.
*
*  The shell's own space is deliberately NOT refused: if the task manager ever
*  handed out no separate directories at all, mapping both test pages there is
*  what makes the failure visible instead of turning it into a page fault. */
static int user_open_page(addrspace_t space, uint32_t virt, uint32_t phys,
                          uint32_t flags)
{
	if(space == 0) return 0;

	return vmm_map_in(space, virt, phys, flags) == 0;
}

/* vmm_get_phys_in() for a space that may not exist. A space of 0 is not an
*  empty directory but no directory at all, and handing that number on would
*  have the walk start at P2V(0), i.e. in the middle of the kernel window --
*  it would read something, which is worse than reading nothing. */
static uint32_t user_phys_in(addrspace_t space, uint32_t virt)
{
	if(space == 0) return 0;

	return vmm_get_phys_in(space, virt);
}

/* Takes a page back out of a task's space once the task is done with it.
*
*  Not politeness: vmm_destroy_space() frees every frame it finds in the user
*  half, and the string page belongs to the shell, not to the task. Leaving it
*  mapped in a dying task's directory would have the frame freed underneath
*  the shell. The check makes sure we only remove what we put there.
*
*  The shell's own space is refused outright: pages are only ever handed to a
*  foreign directory, so a match there would be the shell's own mapping and
*  removing it would be a bug, not a cleanup. */
static void user_close_page(addrspace_t space, uint32_t virt, uint32_t phys)
{
	if(space == 0 || space == vmm_current_space()) return;
	if(user_phys_in(space, virt) != phys) return;

	vmm_unmap_in(space, virt);
}

/* "user": start the demo and say what came of it. The demo is a task of its
   own -- it has to be, because it ends in SYS_EXIT and that would take the
   shell down with it otherwise -- so the shell starts it, waits, and then
   reports what the syscall counter saw in the meantime. */
static void user_run(void)
{
	addrspace_t space;
	uint32_t before;
	uint32_t after;
	int pid;

	if(!user_setup()) return;

	printf("Ring 3 demo:\n");
	printf("  Entry:   ");
	page_print_hex((uint32_t)user_text_entry((void *)user_demo), 8);
	printf("  a copy of .usertext, %i page(s), no kernel code in them\n",
	       user_text_pages);
	printf("  Strings: ");
	page_print_hex((uint32_t)USER_PAGE_VIRT, 8);
	printf("  user page, writable here, read only in the task\n");

	before = syscall_count();

	pid = taskmgr_add_user_task(user_text_entry((void *)user_demo),
	                            "Ring 3 Demo", TASK_PRIORITY_NORMAL);
	if(pid < 0) return;

	user_watch_arm(0, pid);
	taskmgr_task_start(pid);
	printf("  Task %i started in ring 3, waiting for it to exit.\n", pid);

	/* The demo opens with a sleep; this is the window in which its string
	   page is put into its own address space. Read only -- the demo prints
	   the strings, it does not write them. */
	space = user_space_wait(0);
	user_watch_stop();

	printf("  Space:   ");
	page_print_hex((uint32_t)space, 8);
	if(space == 0)
	{
		printf("  the task never reached the CPU\n");
	}
	else if(space == vmm_current_space())
	{
		printf("  the shell's own space -- no separation here\n");
	} else {
		printf("  private to task %i, string page mapped into it\n", pid);
		user_open_page(space, (uint32_t)USER_PAGE_VIRT, user_page_frame,
		               PAGE_PRESENT | PAGE_USER);
	}

	/* Its own sleeps plus a margin, so the shell is back at the prompt only
	   after the demo has run its course. */
	sleep(USER_DEMO_WAIT);

	/* The shell keeps that frame, so it must not stay in a directory that
	   will be torn down with the task -- see user_close_page(). */
	user_close_page(space, (uint32_t)USER_PAGE_VIRT, user_page_frame);

	after = syscall_count();
	printf("  Back in the shell, %u system calls served in between.\n",
	       (int)(after - before));
}

/* Records the result of a check and writes the marker to the start of the
   line, exactly as mem_check() and page_check() do. Both markers are eight
   characters wide, so the text behind them lines up. */
static void user_check(int ok)
{
	user_tests_run++;

	if(ok)
	{
		user_tests_ok++;
		printf("  [  OK  ] ");
	} else {
		printf("  [FAILED] ");
	}
}

/* "user -t".
*
*  Every call below is issued from ring 0, because "int 0x80" works from
*  either side of the privilege boundary and running the checks here means
*  they can be reported in one place instead of shouting results out of a task
*  that is about to exit. That has a consequence worth stating on screen: what
*  these checks prove is that the call path works and that the kernel guards
*  its arguments. They do not prove the privilege drop -- only "user" does.
*
*  SYS_EXIT is not among them for the obvious reason. */
static void user_selftest(void)
{
	char *kernel_text = "THIS TEXT LIVES IN KERNEL MEMORY";
	uint32_t before;
	uint32_t after;
	int pid;
	int sys_pid;
	int written;
	int put;
	int t0;
	int t1;
	int t2;
	int slept;
	int slept_ret;
	int bogus;
	int fault;

	user_tests_run = 0;
	user_tests_ok = 0;

	printf("System call self-test:\n");

	if(!user_setup()) return;

	before = syscall_count();
	printf("  Vector 0x%X, %u calls served since boot\n",
	       SYSCALL_VECTOR, (int)before);
	printf("  Issued from ring 0: these check the call path and the argument\n");
	printf("  guards, not the privilege drop -- \"user\" does that.\n");

	/* 1. The kernel has to answer with the pid of whoever is asking, which
	      right now is this very shell task. */
	pid = taskmgr_get_currpid();
	sys_pid = sys_call0(SYS_GETPID);
	user_check(pid >= 0 && sys_pid == pid);
	printf("SYS_GETPID = %i, taskmgr_get_currpid() = %i\n", sys_pid, pid);

	/* 2. SYS_WRITE with a pointer into the user page: accepted, and the
	      return value is the number of characters it put out. The text
	      appears one line above the marker, which is where it belongs. */
	written = sys_call1(SYS_WRITE, (int)USER_MSG_TEST);
	user_check(written == (int)strlen((char *)USER_MSG_TEST));
	printf("SYS_WRITE(user page) = %i, string is %i characters long\n",
	       written, (int)strlen((char *)USER_MSG_TEST));

	/* 3. SYS_PUTCH puts out exactly one character and says so. The second
	      call closes the line, so the marker below still starts in the
	      column all the other markers do. */
	put = sys_call1(SYS_PUTCH, (int)'*');
	put += sys_call1(SYS_PUTCH, (int)'\n');
	user_check(put == 2);
	printf("SYS_PUTCH twice = %i, the star above is the first of them\n",
	       put);

	/* 4. Uptime has to be a plausible number of milliseconds -- the kernel
	      has been up at least long enough to reach the shell. */
	t0 = sys_call0(SYS_UPTIME);
	user_check(t0 > 0);
	printf("SYS_UPTIME = %i ms since boot\n", t0);

	/* 5. And it has to move on across a SYS_SLEEP. The read is taken
	      immediately before the sleep so the screen output above does not
	      count towards the difference. Half the requested time is the floor,
	      because the tick resolution rounds and the scheduler may hand the
	      CPU on in between -- it may take longer, never noticeably less. */
	t1 = sys_call0(SYS_UPTIME);
	slept_ret = sys_call1(SYS_SLEEP, USER_TEST_SLEEP);
	t2 = sys_call0(SYS_UPTIME);
	slept = t2 - t1;
	user_check(slept_ret == 0 && slept >= USER_TEST_SLEEP / 2);
	printf("SYS_SLEEP(%i) = %i, uptime %i -> %i (+%i ms)\n",
	       USER_TEST_SLEEP, slept_ret, t1, t2, slept);

	/* 6. A call number the table does not know must come back as
	      SYS_ENOSYS rather than as a jump through a null entry. */
	bogus = sys_call0(SYS_NOSUCHCALL);
	user_check(bogus == SYS_ENOSYS);
	printf("Call number %i = %i (SYS_ENOSYS is %i)\n",
	       SYS_NOSUCHCALL, bogus, SYS_ENOSYS);

	/* 7. The check that is actually about security. kernel_text is a plain
	      string literal, so it lives in .rodata at or above
	      KERNEL_VIRTUAL_BASE. Handing that to SYS_WRITE must be refused with
	      SYS_EFAULT -- if the kernel followed the pointer instead, the
	      sentence would be sitting on the screen for everyone to see, which
	      is the whole reason the check reads that way. */
	fault = sys_call1(SYS_WRITE, (int)kernel_text);
	user_check((uint32_t)kernel_text >= KERNEL_VIRTUAL_BASE &&
	           fault == SYS_EFAULT);
	printf("SYS_WRITE(");
	page_print_hex((uint32_t)kernel_text, 8);
	printf(") = %i, kernel memory stayed unread\n", fault);

	/* 8. The rule the previous check enforces, asked of the page tables
	      directly: reachable from ring 3 means present AND PAGE_USER in both
	      the directory and the table entry, which is what
	      vmm_is_user_mapped() answers. The string page has that bit, kernel
	      text does not -- and that is the difference between a pointer a
	      system call may follow and one it must refuse. */
	user_check(vmm_is_user_mapped((uint32_t)USER_PAGE_VIRT) &&
	           !vmm_is_user_mapped((uint32_t)kernel_text));
	printf("vmm_is_user_mapped: user page %i, kernel memory %i\n",
	       vmm_is_user_mapped((uint32_t)USER_PAGE_VIRT),
	       vmm_is_user_mapped((uint32_t)kernel_text));

	/* 9. All of the above went through the one entry point, so the kernel's
	      own counter must have moved by at least the ten calls made here. */
	after = syscall_count();
	user_check(after >= before + 10);
	printf("syscall_count() %u -> %u\n", (int)before, (int)after);

	printf("  Result: %i of %i checks passed\n",
	       user_tests_ok, user_tests_run);
	printf("  Not covered: SYS_EXIT -- from ring 0 it would end the shell.\n");
	printf("  Not covered: address space separation -- \"user -i\" does that.\n");
}

/* "user -i".
*
*  A separate subcommand rather than another block inside "user -t", for the
*  reason "user -t" states about itself: those checks are issued from ring 0
*  and deliberately prove nothing about the privilege drop. This one is the
*  opposite in every respect -- it starts two real ring 3 tasks, takes a good
*  second and a half of wall clock time, and its verdict comes out of what
*  those tasks read back rather than out of a return value. Folding the two
*  together would blur exactly the distinction the self-test is careful to
*  make, and would make the quick check slow.
*
*  The shape of the test:
*
*    - two tasks, one function, one virtual address, two frames
*    - each writes its own value and reads it back while the other writes
*    - the shell states the property directly: vmm_get_phys_in() on the two
*      spaces must return different frames for that one address
*    - and confirms afterwards, from the kernel side, that each frame really
*      holds the value of the task that owns it
*
*  If separation were silently absent, this does not merely fail to prove
*  anything -- it fails loudly: both tasks would report CLOBBERED, the two
*  frame numbers would be equal, and the shell would find the second task's
*  value in the first task's page. */
static void user_isolation(void)
{
	addrspace_t space_a;
	addrspace_t space_b;
	addrspace_t shell;
	volatile uint32_t *page_a;
	volatile uint32_t *page_b;
	uint32_t frame_a;
	uint32_t frame_b;
	uint32_t phys_a;
	uint32_t phys_b;
	uint32_t shared_a;
	uint32_t shared_b;
	int pid_a;
	int pid_b;
	int mapped;

	user_tests_run = 0;
	user_tests_ok = 0;

	printf("Address space isolation test:\n");

	if(!user_setup()) return;

	frame_a = (uint32_t)pmm_alloc_frame();
	frame_b = (uint32_t)pmm_alloc_frame();
	if(frame_a == 0 || frame_b == 0)
	{
		printf("  No free frames for the two test pages.\n");
		pmm_free_frame((void *)frame_a);
		pmm_free_frame((void *)frame_b);
		return;
	}

	/* Both frames are prepared through the direct mapping, i.e. from the
	   kernel window at P2V(frame). No user mapping is needed for that, and
	   the same window is how the results are read back at the end -- the
	   shell never needs the test address in its own space, which is the
	   first thing the property means. */
	page_a = (volatile uint32_t *)P2V(frame_a);
	page_b = (volatile uint32_t *)P2V(frame_b);
	memset((void *)page_a, (char)0, (size_t)PAGE_SIZE);
	memset((void *)page_b, (char)0, (size_t)PAGE_SIZE);

	page_a[USER_ISO_ROLE] = 0;
	page_a[USER_ISO_VALUE] = (uint32_t)USER_ISO_VALUE_A;
	page_b[USER_ISO_ROLE] = 1;
	page_b[USER_ISO_VALUE] = (uint32_t)USER_ISO_VALUE_B;

	pid_a = taskmgr_add_user_task(user_text_entry((void *)user_iso_task),
	                              "Ring 3 Isolation A", TASK_PRIORITY_NORMAL);
	pid_b = taskmgr_add_user_task(user_text_entry((void *)user_iso_task),
	                              "Ring 3 Isolation B", TASK_PRIORITY_NORMAL);
	if(pid_a < 0 || pid_b < 0)
	{
		if(pid_a >= 0) taskmgr_task_abort(pid_a, 0, "isolation test not started");
		if(pid_b >= 0) taskmgr_task_abort(pid_b, 0, "isolation test not started");
		pmm_free_frame((void *)frame_a);
		pmm_free_frame((void *)frame_b);
		return;
	}

	printf("  Address:  ");
	page_print_hex((uint32_t)USER_ISO_VIRT, 8);
	printf("  used by both tasks\n");
	printf("  Task %i writes %u, task %i writes %u -- one page each\n",
	       pid_a, (int)USER_ISO_VALUE_A, pid_b, (int)USER_ISO_VALUE_B);
	printf("  Each writes, sleeps while the other writes, then reads back.\n");

	/* Both tasks open with a sleep, so they are on the CPU -- and their
	   directories therefore observable -- before they touch any of their
	   pages. */
	user_watch_arm(0, pid_a);
	user_watch_arm(1, pid_b);
	taskmgr_task_start(pid_a);
	taskmgr_task_start(pid_b);

	space_a = user_space_wait(0);
	space_b = user_space_wait(1);
	user_watch_stop();
	shell = vmm_current_space();

	/* 1. Two tasks, two directories. Every check below rests on this one, so
	      it is stated before anything is mapped. */
	user_check(space_a != 0 && space_b != 0 && space_a != space_b);
	printf("Task %i in space ", pid_a);
	page_print_hex((uint32_t)space_a, 8);
	printf(", task %i in ", pid_b);
	page_print_hex((uint32_t)space_b, 8);
	printf("\n");

	/* 2. And neither of them is the one the shell runs in, which is the
	      kernel's. */
	user_check(shell == vmm_kernel_space() && space_a != shell &&
	           space_b != shell);
	printf("Shell in ");
	page_print_hex((uint32_t)shell, 8);
	printf(" = vmm_kernel_space(), neither task is\n");

	/* The two pages every task needs: its own test page, writable, and the
	   string page it prints through, read only -- it reads those strings, it
	   has no business writing them. */
	mapped = 1;
	if(!user_open_page(space_a, (uint32_t)USER_ISO_VIRT, frame_a,
	                   PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) mapped = 0;
	if(!user_open_page(space_b, (uint32_t)USER_ISO_VIRT, frame_b,
	                   PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) mapped = 0;
	if(!user_open_page(space_a, (uint32_t)USER_PAGE_VIRT, user_page_frame,
	                   PAGE_PRESENT | PAGE_USER)) mapped = 0;
	if(!user_open_page(space_b, (uint32_t)USER_PAGE_VIRT, user_page_frame,
	                   PAGE_PRESENT | PAGE_USER)) mapped = 0;

	/* 3. The property itself, stated as plainly as it can be: one virtual
	      address, translated in two address spaces, must come out as two
	      different physical frames. */
	phys_a = user_phys_in(space_a, (uint32_t)USER_ISO_VIRT);
	phys_b = user_phys_in(space_b, (uint32_t)USER_ISO_VIRT);
	user_check(mapped && phys_a == frame_a && phys_b == frame_b &&
	           phys_a != phys_b);
	printf("One address, two frames: ");
	page_print_hex(phys_a, 8);
	printf(" and ");
	page_print_hex(phys_b, 8);
	printf("\n");

	/* 4. The control for it. The string page is mapped into both spaces on
	      purpose, at the same address and from the same frame -- so
	      vmm_get_phys_in() returning two different numbers above is a real
	      difference between the spaces and not the function answering
	      differently for each of them. Sharing is a decision here, not the
	      default. */
	shared_a = user_phys_in(space_a, (uint32_t)USER_PAGE_VIRT);
	shared_b = user_phys_in(space_b, (uint32_t)USER_PAGE_VIRT);
	user_check(mapped && shared_a == user_page_frame &&
	           shared_b == user_page_frame);
	printf("String page shared on purpose: both show ");
	page_print_hex(shared_a, 8);
	printf("\n");

	/* 5. And the shell, in its own space, has nothing at that address at
	      all -- the test page exists twice without existing here once. */
	user_check(!vmm_is_mapped((uint32_t)USER_ISO_VIRT) &&
	           vmm_get_phys((uint32_t)USER_ISO_VIRT) == 0);
	printf("Nothing mapped at ");
	page_print_hex((uint32_t)USER_ISO_VIRT, 8);
	printf(" in the shell's space\n");

	if(!mapped)
	{
		printf("  The test pages could not be mapped -- stopping both tasks.\n");
		taskmgr_task_abort(pid_a, 0, "isolation test setup failed");
		taskmgr_task_abort(pid_b, 0, "isolation test setup failed");
	} else {
		printf("  Both tasks are waiting for their page, letting them run:\n");

		/* The two ring 3 lines appear during this sleep, as does the task
		   manager's note about each exit. */
		sleep(USER_ISO_WAIT);

		/* 6. What the tasks said, checked from the kernel side rather than
		      taken on trust: each frame has to hold the last value ITS task
		      wrote. Read through the direct mapping, so this looks at the
		      frames themselves, not at either task's view of them. */
		user_check(page_a[USER_ISO_CELL] == (uint32_t)USER_ISO_VALUE_A + 1 &&
		           page_b[USER_ISO_CELL] == (uint32_t)USER_ISO_VALUE_B + 1);
		printf("Frames hold %u and %u, each its own task's last write\n",
		       (int)page_a[USER_ISO_CELL], (int)page_b[USER_ISO_CELL]);
	}

	/* Both tasks are done with their pages, so the pages come back out of
	   their directories before anything can tear those down with the frames
	   still in them. */
	user_close_page(space_a, (uint32_t)USER_ISO_VIRT, frame_a);
	user_close_page(space_b, (uint32_t)USER_ISO_VIRT, frame_b);
	user_close_page(space_a, (uint32_t)USER_PAGE_VIRT, user_page_frame);
	user_close_page(space_b, (uint32_t)USER_PAGE_VIRT, user_page_frame);

	/* Only reachable if there were no separate spaces to map into: then the
	   test page landed in the shell's own directory. */
	if(vmm_get_phys((uint32_t)USER_ISO_VIRT) == frame_a ||
	   vmm_get_phys((uint32_t)USER_ISO_VIRT) == frame_b)
	{
		vmm_unmap((uint32_t)USER_ISO_VIRT);
	}

	pmm_free_frame((void *)frame_a);
	pmm_free_frame((void *)frame_b);

	printf("  Result: %i of %i checks passed\n",
	       user_tests_ok, user_tests_run);

	if(user_tests_ok == user_tests_run)
	{
		printf("  Proven: two ring 3 tasks held one virtual address at the same\n");
		printf("  time, each read back only its own writes while the other was\n");
		printf("  writing to that address, and the frames behind it differ.\n");
	} else {
		printf("  Proven: nothing -- a check above failed, so whatever the two\n");
		printf("  tasks printed says nothing about separate address spaces.\n");
	}

	printf("  Not proven either way: that nothing else leaks between the two.\n");
	printf("  One page was tested, the quarter above ");
	page_print_hex((uint32_t)KERNEL_VIRTUAL_BASE, 8);
	printf(" is shared by\n");
	printf("  design, and both tasks run a copy of .usertext rather than a\n");
	printf("  program loaded from anywhere.\n");
}

void usermode(char *cmd)
{
	char opt[100];

	if(prmc(cmd) == 0)
	{
		user_run();
		return;
	}

	strcpy(opt, prmv(1, cmd));

	if(strcmp(opt, "-t") == 0)
	{
		user_selftest();
	}
	else if(strcmp(opt, "-i") == 0)
	{
		user_isolation();
	} else {
		printf("Syntax: user [-t] [-i]\n");
		printf("\t          Run the ring 3 demo\n");
		printf("\t-t        Run the system call self-test\n");
		printf("\t-i        Run the address space isolation test:\n");
		printf("\t          two ring 3 tasks, one address, two pages\n");
	}
}

/* --- ps and exec --------------------------------------------------------- */

/* Prints text left-aligned in a field of the given width, the counterpart to
*  mem_print_right() for the name columns. At most width - 1 characters are
*  put out, so a long name is cut short instead of pushing the column behind
*  it out of line, and there is always at least one space to the next
*  column. */
static void ps_print_left(const char *text, int width)
{
	int i;

	for(i = 0; i < width - 1 && text[i] != EOS; i++)
	{
		putch((unsigned char)text[i]);
	}

	while(i < width)
	{
		putch(' ');
		i++;
	}
}

/* What "exec" has started, in the order it started it. Only the shell writes
*  this, and only from the command it is running, so no locking is involved.
*
*  The address space is recorded rather than only looked up later, because it
*  is the answer to "did this instance get one of its own": two rows naming
*  the same program with two different directories say that outright. It is
*  also what tells a slot that has been reused from the task that is still in
*  it -- see ps_instances(). */
static int      exec_inst_pid[PS_INSTANCES];
static uint32_t exec_inst_space[PS_INSTANCES];
static char     exec_inst_name[PS_INSTANCES][PS_NAME_WIDTH];
static int      exec_inst_used = 0;

static void exec_remember(int pid, addrspace_t space, const char *name)
{
	int i;
	int j;

	if(exec_inst_used == PS_INSTANCES)
	{
		/* The oldest entry drops out and the rest keeps its order, so the
		   list still reads from top to bottom in the order things started. */
		for(i = 1; i < PS_INSTANCES; i++)
		{
			exec_inst_pid[i - 1] = exec_inst_pid[i];
			exec_inst_space[i - 1] = exec_inst_space[i];

			for(j = 0; j < PS_NAME_WIDTH; j++)
			{
				exec_inst_name[i - 1][j] = exec_inst_name[i][j];
			}
		}

		exec_inst_used--;
	}

	i = exec_inst_used;
	exec_inst_pid[i] = pid;
	exec_inst_space[i] = (uint32_t)space;

	/* Bounded copy -- there is no strncpy() here, and a module name is not
	   under this file's control. */
	for(j = 0; j < PS_NAME_WIDTH - 1 && name[j] != EOS; j++)
	{
		exec_inst_name[i][j] = name[j];
	}
	exec_inst_name[i][j] = EOS;

	exec_inst_used++;
}

/* The modules the bootloader handed over, i.e. what "exec" can be asked for.
*  No modules is a normal state, not an error: "make run" boots the kernel on
*  its own, and so does an ISO built before programs existed. exec_init() then
*  simply recorded nothing and the table is empty. */
static void ps_modules(void)
{
	int count;
	int i;

	count = exec_module_count();

	printf("Modules the bootloader passed:\n");

	if(count == 0)
	{
		printf("  None -- this kernel was booted without any, so there is\n");
		printf("  nothing for \"exec\" to load.\n");
		return;
	}

	/* The header is padded to the same widths as the rows below: two leading
	   spaces, the number, two spaces, the name field, and the size field --
	   the five spaces before "Bytes" are what right-aligns it over the
	   numbers. */
	printf("  Nr  ");
	ps_print_left("Name", PS_NAME_WIDTH);
	printf("     Bytes\n");

	for(i = 0; i < count; i++)
	{
		printf("  ");
		mem_print_right((uint32_t)i, 2);
		printf("  ");
		ps_print_left(exec_module_name(i), PS_NAME_WIDTH);
		mem_print_right(exec_module_size(i), PS_SIZE_WIDTH);
		printf("\n");
	}
}

/* The instances "exec" has started, with the address space each one was given.
*
*  Whether an instance is still there is asked of the task manager rather than
*  assumed: taskmgr_task_space() returns 0 once a task is gone and its
*  directory has been reaped. Comparing the answer with what was recorded also
*  covers the case that matters for a pid -- pids are slot numbers here, so a
*  later task can inherit the number, and it will not inherit the directory. */
static void ps_instances(void)
{
	addrspace_t space;
	int i;

	if(exec_inst_used == 0) return;

	printf("Started with \"exec\":\n");
	printf("  Pid  ");
	ps_print_left("Program", PS_NAME_WIDTH);
	printf("Address space\n");

	for(i = 0; i < exec_inst_used; i++)
	{
		printf("  ");
		mem_print_right((uint32_t)exec_inst_pid[i], PS_PID_WIDTH);
		printf("  ");
		ps_print_left(exec_inst_name[i], PS_NAME_WIDTH);
		page_print_hex(exec_inst_space[i], 8);

		space = taskmgr_task_space(exec_inst_pid[i]);
		if(space == exec_inst_space[i])
		{
			printf("  live\n");
		} else {
			printf("  ended\n");
		}
	}
}

/* "ps": what is loaded, what has been started from it, and what is running.
*
*  The last of those three is left to taskmgr_list_tasks() instead of being
*  printed here, and that is a deliberate choice rather than laziness. The task
*  table lives in tasks.c and nothing exports it -- there is no call that hands
*  out a pid, a name or a state for a slot, only the one that prints them. A
*  listing written here could therefore not show anything that listing does not
*  already show; it could only duplicate the walk over a table it cannot see,
*  and drift from it the day a state is added.
*
*  What this file can add is the part tasks.c knows nothing about: which
*  program a task came from, and which of the modules is which. Hence the two
*  tables above and the task manager's own below. */
void processes(char *cmd)
{
	if(prmc(cmd) != 0)
	{
		printf("Syntax: ps\n");
		printf("\t          List the loaded modules, the programs started\n");
		printf("\t          from them and the running tasks\n");
		return;
	}

	ps_modules();
	printf("\n");
	ps_instances();
	if(exec_inst_used != 0) printf("\n");

	taskmgr_list_tasks();
}

/* "exec NAME": load the module of that name into an address space of its own
*  and run it.
*
*  Nothing here is shared with a previous run of the same program. exec_spawn()
*  builds a fresh space, loads the segments into it and returns a task that is
*  suspended -- so the same name can be started as often as there are free task
*  slots and free frames, and every instance gets its own directory, its own
*  copy of the segments and its own .bss. "ps" shows that as two rows with the
*  same program name and two different address spaces.
*
*  The window between exec_spawn() and taskmgr_task_start() is what makes the
*  space printable at all: the task exists, it has its directory, and it has
*  not executed an instruction yet, so taskmgr_task_space() can simply be asked
*  instead of sampling CR3 from the timer the way the ring 3 demo has to. */
void execute(char *cmd)
{
	char name[100];
	addrspace_t space;
	int index;
	int pid;

	if(prmc(cmd) == 0)
	{
		printf("Syntax: exec NAME\n");
		printf("\t          Load the module NAME and run it as a ring 3 task\n");
		printf("\t          \"ps\" lists the names there are\n");
		return;
	}

	/* prmv() hands back a pointer into one static buffer, so the name is
	   copied out before anything else calls prmv() again. */
	strcpy(name, prmv(1, cmd));

	/* Programs can come from two places now: a bootloader module, or a file
	   on the mounted filesystem. exec_module_find() looks in both, so this
	   only gives up when neither source exists at all. */
	if(exec_module_count() == 0 && !fat_mounted())
	{
		printf("exec: no modules were passed and no filesystem is mounted --\n");
		printf("      there is nothing to run. \"ps\" and \"df\" say the same.\n");
		return;
	}

	index = exec_module_find(name);
	if(index < 0)
	{
		printf("exec: no module called \"%s\" -- \"ps\" lists what there is.\n",
		       name);
		return;
	}

	/* A program that cannot be loaded says why: exec_last_error() carries the
	   reason the loader refused it -- not an ELF, wrong machine, no free
	   frame -- which is a good deal more use than a bare failure. */
	pid = exec_spawn(index, TASK_PRIORITY_NORMAL);
	if(pid < 0)
	{
		printf("exec: %s could not be loaded: %s\n", name, exec_last_error());
		return;
	}

	space = taskmgr_task_space(pid);

	printf("Loaded %s:\n", name);
	printf("  Module:  %i, %u bytes\n", index, (int)exec_module_size(index));
	printf("  Task:    pid %i, priority %i, suspended so far\n",
	       pid, TASK_PRIORITY_NORMAL);
	printf("  Space:   ");
	page_print_hex((uint32_t)space, 8);

	/* A user task without a directory of its own is a contradiction -- the
	   loader wrote the segments into one, so there has to be one. If the task
	   manager says otherwise, the task is taken back down rather than started
	   into whatever it would be running in. */
	if(space == 0)
	{
		printf("  no address space of its own -- not started\n");
		taskmgr_task_abort(pid, 0, "no address space");
		return;
	}

	printf("  private to pid %i, the program is loaded in it\n", pid);

	exec_remember(pid, space, name);
	taskmgr_task_start(pid);

	printf("  Started. \"ps\" shows it for as long as it runs.\n");
}

/* --- df ------------------------------------------------------------------
*
*  What is left of the filesystem in the kernel. Listing a directory and
*  printing a file used to live here as "ls" and "cat"; they are ring 3
*  programs now (/BIN/LS.ELF and /BIN/CAT.ELF), because SYS_READDIR, SYS_STAT
*  and SYS_READ give a program everything those two ever did -- they were pure
*  formatting on top of the same three questions.
*
*  "df" stays, and not out of sentiment: it reports the ATA drive table as
*  well as the mount, and there is no system call for the drive half. Moving
*  it would mean either inventing one or quietly dropping half the command. */

/* Prints text right-aligned in a field of the given width -- the counterpart
*  to ps_print_left(), and the same idea as mem_print_right() for numbers.
*  It exists because the size column is not always a number: a directory has
*  no size of its own, and the header carries a word, and both have to line up
*  over the digits below them. Text longer than the field is cut short rather
*  than pushing the row out of line. */
static void fs_print_right_text(const char *text, int width)
{
	int len;
	int i;

	len = (int)strlen(text);
	if(len > width) len = width;

	for(i = len; i < width; i++)
	{
		putch(' ');
	}

	for(i = 0; i < len; i++)
	{
		putch((unsigned char)text[i]);
	}
}

/* How many drives the ATA driver identified. Asked in two places: to explain
*  an unmounted filesystem, and to print the drive table in "df". */
static int fs_drives_found(void)
{
	int drive;
	int found;

	found = 0;
	for(drive = 0; drive < ATA_MAX_DRIVES; drive++)
	{
		if(ata_present(drive)) found++;
	}

	return found;
}

/* Why there is no filesystem. The one place that knows, so that everything
*  which runs into an unmounted volume gives the same account of the same
*  machine -- the filesystem counterpart of net_explain_down().
*
*  Booting without a disk is a normal state here, not a failure -- "make run"
*  does exactly that -- so the answer has to distinguish the two cases that
*  lead to it. No drive at all is a different situation from a drive that
*  holds nothing this kernel recognises, and only the second one has an error
*  message worth printing.
*
*  The caller prints the headline, because the two callers are asking
*  different questions: "help" is listing what could be run, and the shell is
*  explaining a word it could not place. Only the reasons underneath are the
*  same, and only those are here. */
static void fs_explain_unmounted(void)
{
	const char *why;
	int drives;

	drives = fs_drives_found();

	if(drives == 0)
	{
		printf("      The ATA driver found no drive at all, so there is nothing\n");
		printf("      to mount. Booting without a disk is a normal case here.\n");
	} else {
		why = fat_last_error();
		printf("      %i drive(s) were found, but none of them carries a FAT12\n",
		       drives);
		printf("      or FAT16 filesystem this kernel can read.\n");
		if(why[0] != EOS) printf("      Last error: %s\n", why);
	}

	printf("      \"df\" shows what the drivers did find.\n");
}

/* "df": the state of the mount and of the drives underneath it.
*
*  This is the command one runs when something does not work, so it prints
*  both halves even when the upper one is missing: a filesystem that is not
*  mounted still leaves the question of whether a disk was found at all, and
*  that answer lives in the ATA driver. */
static void fs_df(void)
{
	const char *label;
	uint32_t total_kib;
	uint32_t avail_kib;
	uint32_t sectors;
	int drives;
	int drive;

	printf("Filesystem:\n");

	if(!fat_mounted())
	{
		printf("  Nothing is mounted -- no path can be read.\n");
	} else {
		/* Kibibytes from fat.h now, and the second column is MiB rather
		*  than KiB: the byte figure the first column used to carry cannot
		*  be formed at all once a volume passes 4 GiB, which is the case
		*  FAT32 exists for. The columns and their widths are unchanged, so
		*  the three rows still line up under each other. */
		total_kib = fat_total_kib();
		avail_kib = fat_free_kib();
		label = fat_label();

		printf("  Type:          %s\n", fat_type());
		printf("  Label:         %s\n", label[0] == EOS ? "(none)" : label);

		printf("  Total:   ");
		mem_print_right(total_kib, 12);
		printf(" KiB (");
		mem_print_right(total_kib / 1024, 8);
		printf(" MiB)\n");

		printf("  Used:    ");
		mem_print_right(total_kib - avail_kib, 12);
		printf(" KiB (");
		mem_print_right((total_kib - avail_kib) / 1024, 8);
		printf(" MiB)\n");

		printf("  Free:    ");
		mem_print_right(avail_kib, 12);
		printf(" KiB (");
		mem_print_right(avail_kib / 1024, 8);
		printf(" MiB)\n");

		printf("  Cluster size:  %u bytes\n", (int)fat_cluster_bytes());
	}

	printf("\n");
	printf("Drives the ATA driver found:\n");

	drives = fs_drives_found();

	if(drives == 0)
	{
		printf("  None. Either no controller answered or nothing is attached\n");
		printf("  to it -- booting without a disk is a normal case here.\n");

		/* The driver's own account of the last failure, if it had one. A
		   probe that simply found nothing leaves this empty. */
		if(ata_last_error()[0] != EOS)
		{
			printf("  Last error: %s\n", ata_last_error());
		}
		return;
	}

	printf("  Nr  ");
	ps_print_left("Model", DF_MODEL_WIDTH);
	fs_print_right_text("Sectors", 10);
	fs_print_right_text("MiB", 11);
	printf("\n");

	for(drive = 0; drive < ATA_MAX_DRIVES; drive++)
	{
		if(!ata_present(drive)) continue;

		sectors = ata_sectors(drive);

		printf("  ");
		mem_print_right((uint32_t)drive, 2);
		printf("  ");
		ps_print_left(ata_model(drive), DF_MODEL_WIDTH);
		mem_print_right(sectors, 10);

		/* Divided rather than multiplied: sectors * 512 overflows 32 bits
		   at 8 GiB, and a drive that large is entirely plausible. */
		mem_print_right(sectors / (1024 * 1024 / ATA_SECTOR_SIZE), 11);
		printf("\n");
	}
}

void diskfree(char *cmd)
{
	if(prmc(cmd) != 0)
	{
		printf("Syntax: df\n");
		printf("\t          Show the mounted filesystem and the drives found\n");
		return;
	}

	fs_df();
}

/* --- running a program off the disk ---------------------------------------
*
*  What happens to a word the dispatch above does not know. It used to be an
*  error; it is now a lookup, and that single change is what lets a command
*  leave the kernel at all -- "ls" works after the deletion above because
*  /BIN/LS.ELF is found here, and it is typed exactly as it was before.
*
*  Three things had to be got right for that to be usable rather than merely
*  present, and they are the three routines below: the name has to be turned
*  into something a FAT volume can actually hold (run_path), the rest of the
*  line has to reach the program intact (run_arguments), and the shell has to
*  wait rather than print its prompt into somebody's output (run_wait). */

/* Whether a character may stand in an 8.3 short name.
*
*  The set is FAT's, not this file's invention: letters, digits and the
*  handful of punctuation marks DOS left alone. Everything else is excluded
*  for a reason worth being precise about -- the space and the dot end a name
*  field, '/' is the separator this shell builds the path with, and
*  "*+,:;<=>?[\]| and the quote are reserved. A word containing any of them
*  can never name a file here, so building a path out of it would only produce
*  a nonsense string to fail on later. */
static int run_name_char(char c)
{
	static const char allowed[] = "$%'-_@~`!(){}^#&";
	int i;

	if(c >= 'A' && c <= 'Z') return 1;
	if(c >= 'a' && c <= 'z') return 1;
	if(c >= '0' && c <= '9') return 1;

	for(i = 0; allowed[i] != EOS; i++)
	{
		if(allowed[i] == c) return 1;
	}

	return 0;
}

/* ASCII case folding, which is all an 8.3 name needs -- everything outside
*  a..z is already what it has to be. */
static char run_upper(char c)
{
	if(c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');

	return c;
}

/* The one place that turns a typed word into the path of a program.
*
*  Returns 1 and fills path (BIN_PATH_MAX bytes) when the word can name a file
*  in BIN_DIR, and 0 when it cannot -- too long for the eight characters an
*  8.3 name has, empty, or carrying a character run_name_char() refuses. A
*  refusal is not an error to report: nothing on this volume can be called
*  that, so the answer to "is there a program of that name" is simply no.
*
*  Note what is NOT accepted: a name with an extension. "cat" becomes
*  /BIN/CAT.ELF, and "cat.elf" is rejected rather than becoming
*  /BIN/CAT.ELF.ELF -- the extension belongs to the mechanism, not to the
*  command. */
static int run_path(const char *word, char *path)
{
	int len;
	int at;
	int i;

	len = (int)strlen(word);
	if(len < 1 || len > BIN_NAME_MAX) return 0;

	for(i = 0; i < len; i++)
	{
		if(!run_name_char(word[i])) return 0;
	}

	strcpy(path, BIN_DIR);
	at = (int)strlen(path);

	for(i = 0; i < len; i++)
	{
		path[at + i] = run_upper(word[i]);
	}

	strcpy(path + at + len, BIN_EXT);

	return 1;
}

/* Everything after the command word, as one string, or 0 when there is
*  nothing.
*
*  This deliberately does not go through prmv(). prmv() hands back one word at
*  a time out of a single static buffer -- the trap this file works around
*  everywhere else by copying the result out immediately -- so assembling a
*  line from it would mean copying every word into a second buffer only to put
*  the blanks back in. The line is already sitting in the caller's own buffer
*  in exactly the form the program wants, so a pointer into it is both cheaper
*  and more faithful: exec_spawn_path() does the splitting, and it splits what
*  was actually typed rather than a reconstruction of it.
*
*  cmd is the shell's own array and outlives the spawn, so the pointer stays
*  valid for as long as the loader needs it. */
static const char *run_arguments(char *cmd)
{
	int i;

	i = 0;
	while(cmd[i] != EOS && cmd[i] != ' ') i++;   /* over the command word    */
	while(cmd[i] == ' ') i++;                    /* and the blanks after it  */

	if(cmd[i] == EOS) return 0;

	return cmd + i;
}

/* Waits for a program to finish, so the prompt comes back after its output
*  instead of into the middle of it.
*
*  The wait is a poll with sleep() rather than a spin: exec_spawn_path()
*  returns as soon as the task exists, the program runs as a task of its own,
*  and the only way it gets a processor is if this one gives it up. A busy
*  loop here would work -- the timer preempts -- but would burn every slice
*  the scheduler hands the shell on asking the same question again.
*
*  THIS ONE IS NOT CONVERTED to the wait in system.h, and the reason is that
*  there is nothing to wait ON. A channel is only worth blocking on if
*  something wakes it, and a task that ends wakes nothing: taskmgr_task_exit()
*  and taskmgr_task_abort() change a state and no more, and no address that
*  stands for "this pid finished" is exported. Blocking on an address nobody
*  ever wakes is sleep() with a longer name and a false promise in it -- every
*  wait would run to its timeout, the loop would poll exactly as it does now,
*  and the next reader would have to work out that the wake is imaginary.
*
*  That is what taskmgr_exit_channel() now is: taskmgr_task_exit() and
*  taskmgr_task_abort() both wake it, so this waits instead of polling and
*  RUN_POLL_MS is the backstop rather than the interval. One channel serves
*  every death, so a wake means "some task ended" and the state is re-tested --
*  which is the idiom anyway, and it is what makes a shared channel correct
*  rather than merely convenient.
*
*  A program that never ends is the case worth deciding rather than
*  discovering. The shell stops waiting after RUN_WAIT_MS and says so; the
*  program is left running and can be looked at with "ps" and stopped with
*  "taskmgr -k". Two things it deliberately does not do: it does not wait
*  forever, because then one bad file on the disk takes the machine away for
*  good, and it does not kill the program, because a shell that decides half
*  a minute of work is too much is worse than a prompt in the wrong place.
*  There is no key to interrupt with either, and that is the same kind of
*  choice: getch() and SYS_GETCH share one single key slot in kb.c, so a shell
*  polling the keyboard while a program runs would take every keystroke away
*  from the program it is waiting for. */
static void run_wait(int pid, const char *name)
{
	int start;
	int state;
	int elapsed;
	unsigned long flags;

	/* The deadline is read off the clock rather than added up from the sleeps
	   below, and the difference is not academic. sleep() rounds up to whole
	   timer ticks and, more importantly, returns only once this task is
	   scheduled again -- with a program and the status bar competing for the
	   processor, three thousand sleeps of ten milliseconds take a good deal
	   more than the thirty seconds they nominally add up to. Counting them
	   would make RUN_WAIT_MS mean whatever the load happens to be. */
	start = timer_get_ticks();

	for(;;)
	{
		/* Tested with interrupts off and tested again after every wake, per
		   the idiom above task_wait() in system.h. The state is what the wake
		   is about, so reading it inside the critical section is not caution
		   for its own sake: an exit landing between the read and the block is
		   exactly the wake that would otherwise be lost. */
		flags = net_irq_save();
		state = taskmgr_task_state(pid);

		/* EXITED is where a task that returned or called exit() ends up,
		   ABORTED where one that faulted or was killed does. The two are
		   worth telling apart in "ps"; to a caller that only has to stop
		   waiting they are the same thing, and both are checked here so that
		   a shell does not sit out its full timeout on a program that
		   finished. NULL is a slot that names no task at all. Everything
		   else means it is still there. */
		if(state == TASK_STATE_EXITED || state == TASK_STATE_ABORTED
		   || state == TASK_STATE_NULL)
		{
			net_irq_restore(flags);
			return;
		}

		elapsed = timer_get_ticks() - start;
		if(elapsed >= RUN_WAIT_MS || elapsed < 0)
		{
			net_irq_restore(flags);
			break;
		}

		task_wait(taskmgr_exit_channel(), RUN_POLL_MS);
		net_irq_restore(flags);
	}

	printf("\n[%s is still running as pid %i after %i seconds. The prompt is\n",
	       name, pid, RUN_WAIT_MS / 1000);
	printf(" back and its output will land in it -- \"taskmgr -k %i\" stops it.]\n",
	       pid);
}

/* A word the built-in list did not know: look for a program of that name in
*  BIN_DIR and run it, and only say "Unknown command" when there is none.
*
*  The file is looked up with fat_size() before exec_spawn_path() is asked to
*  load it, and that is not a redundant walk of the directory. It separates
*  the two answers the user needs to be able to tell apart: a name that is on
*  no disk is a typo and deserves "Unknown command", while a name that IS
*  there and will not load is a broken program and deserves the loader's own
*  reason for refusing it. Without the lookup both come back as one failed
*  spawn. */
static void run_program(const char *word, char *cmd)
{
	char path[BIN_PATH_MAX];
	const char *args;
	uint32_t size;
	int pid;

	if(!run_path(word, path))
	{
		/* No file on this volume can carry that name, so there was never
		   anywhere to look. */
		printf("Unknown command: %s\n", word);
		printf("      No file in %s can be called that. \"help\" lists them.\n",
		       BIN_DIR);
		return;
	}

	if(!fat_mounted())
	{
		printf("Unknown command: %s\n", word);
		printf("      Not built in, and nothing is mounted -- no %s to look\n",
		       BIN_DIR);
		printf("      a program up in.\n");
		fs_explain_unmounted();
		return;
	}

	if(fat_size(path, &size) != 0)
	{
		printf("Unknown command: %s\n", word);
		printf("      Not built in, and there is no %s. \"help\" lists both.\n",
		       path);
		return;
	}

	args = run_arguments(cmd);

	/* Everything the loader can refuse -- a text file with the right name, a
	   truncated ELF, a 64-bit binary, no free frame -- comes back here as one
	   negative return with the reason in exec_last_error(). */
	pid = exec_spawn_path(path, args, TASK_PRIORITY_NORMAL);
	if(pid < 0)
	{
		printf("%s: %s could not be loaded: %s\n", word, path,
		       exec_last_error());
		return;
	}

	/* The task comes back suspended, which is what let "exec" print the
	   address space before anything ran. Nothing needs printing here: the
	   program's own output is the answer to the command, and a shell that
	   announces every start would bury it. */
	taskmgr_task_start(pid);

	run_wait(pid, word);
}

/* --- gfx ----------------------------------------------------------------
*
*  Two surfaces, one picture.
*
*  A machine that booted into text mode has no graphics mode until this
*  command makes one: vga_set_mode() programs the registers, 320x200x256
*  appears, and it goes away again afterwards. A machine that booted with
*  "make run-bootdisk" is already in a graphics mode -- a 1024x768 linear
*  framebuffer our own stage 2 negotiated -- and that one must NOT be
*  switched to mode 13h. VBE is a real mode BIOS interface, so the switch
*  would be one way: nothing in protected mode can put 1024x768 back. See
*  the header comment of fbdraw.h, which is the contract for drawing into
*  the framebuffer that is already there.
*
*  So the command asks fbcon_active() which machine it is on and paints
*  through the matching surface. Everything below the surface -- the layout,
*  the palette, every shape of the picture -- exists once.
*
*  What leaving the console costs, and how this section pays for it.
*
*  1. Mode 13h destroys the console. The video RAM is one memory: the
*     framebuffer at 0xA0000 is chained across all four planes, so the first
*     eight thousand pixels of the picture land on exactly the bytes that
*     hold the characters and the attributes of the 80x25 screen.
*     vga_set_mode() saves and restores the font (see vga.h), which is a
*     different plane and a different problem; the visible text is this
*     file's to keep, so gfx_text_store() takes a copy of the 2000 cells
*     before the switch and gfx_text_recall() writes it back after.
*
*     The framebuffer path has the same problem and a better answer. The
*     picture covers the console because there is only one screen, but
*     fbcon keeps a shadow buffer of every character it ever printed, so
*     nothing has to be copied out first: fbcon_repaint() draws the console
*     back from that buffer. gfx_leave() calls it, and the console returns
*     with its scrollback and its cursor.
*
*  2. The status bar task keeps running, and on the framebuffer it would
*     draw straight into the picture -- its output goes through fbcon like
*     any other printing. In mode 13h it is harmless by accident: it writes
*     80 cells into 0xB8000, which is not part of the 64 KiB window mode 13h
*     maps, so the hardware discards them. Harmless by accident is not a
*     reason to leave it running, and on the other path it is not harmless
*     at all, so the task is suspended for the duration on BOTH paths and
*     started again afterwards. It repaints itself within ten ticks.
*
*  3. The same trap catches printf(), on both paths and for two different
*     reasons: in mode 13h the characters are thrown away by the hardware
*     while the console cursor still advances and scrolls the screen the
*     copy in gfx_text_page[] no longer matches, and on the framebuffer they
*     land in the middle of the picture and scroll fbcon's shadow buffer, so
*     that what comes back is not what was there. Neither half of this
*     section prints a single character while the picture is up: the
*     self-tests collect their results in local variables and report once the
*     console is back. */

/* The drawing surface, as a table of the primitives vga.h and fbdraw.h both
*  offer. The two headers were written to mirror each other down to the
*  argument order and to taking a palette INDEX rather than an RGB triple, so
*  these nine signatures are identical on both sides and the table can hold
*  either set unchanged.
*
*  A table rather than nine wrapper functions, because a wrapper would be
*  nine bodies that each say the same "if framebuffer, else" -- nine places
*  to forget one. Here the choice is made once, in gfx_pick_surface(), and
*  everything above it simply draws. Two things are deliberately NOT in the
*  table: the palette and the text, because they are the only two calls whose
*  arguments actually differ between the paths, and gfx_palette() and
*  gfx_text() adapt them in one place each. */
typedef struct
{
	void (*clear)(uint8_t colour);
	void (*pixel)(int x, int y, uint8_t colour);
	void (*hline)(int x, int y, int len, uint8_t colour);
	void (*vline)(int x, int y, int len, uint8_t colour);
	void (*line)(int x0, int y0, int x1, int y1, uint8_t colour);
	void (*rect)(int x, int y, int w, int h, uint8_t colour);
	void (*fill)(int x, int y, int w, int h, uint8_t colour);
	void (*circle)(int cx, int cy, int radius, uint8_t colour);
	void (*disc)(int cx, int cy, int radius, uint8_t colour);
} gfx_surface;

static const gfx_surface gfx_vga_ops =
{
	vga_clear, vga_pixel, vga_hline, vga_vline,
	vga_line, vga_rect, vga_fill, vga_circle, vga_disc
};

static const gfx_surface gfx_fb_ops =
{
	fbdraw_clear, fbdraw_pixel, fbdraw_hline, fbdraw_vline,
	fbdraw_line, fbdraw_rect, fbdraw_fill, fbdraw_circle, fbdraw_disc
};

/* The surface in use and its geometry, all set by gfx_pick_surface(). The
*  defaults are mode 13h, which is what a machine without a framebuffer has
*  and the only thing that is true before anything has been asked. */
static const gfx_surface *gfx_surf = &gfx_vga_ops;
static int gfx_on_fb = 0;
static int gfx_w = VGA_WIDTH;
static int gfx_h = VGA_HEIGHT;

/* Line weights and text sizes are multiples of this rather than fixed pixel
*  counts, so a feature keeps its visual weight instead of thinning to a hair
*  as the surface grows. One at 320x200, two at 640x480 and 800x600, three at
*  1024x768 -- see gfx_pick_surface(). */
static int gfx_unit = 1;

/* Suspends the status bar task, if there is one. taskmgr_task_suspend()
*  complains about pid 0 -- the system task -- so the check is for a pid that
*  is really the bar's. */
static void gfx_statusbar_hold(void)
{
	if(statusbar_pid > 0)
	{
		taskmgr_task_suspend(statusbar_pid);
	}
}

static void gfx_statusbar_release(void)
{
	if(statusbar_pid > 0)
	{
		taskmgr_task_start(statusbar_pid);
	}
}

/* Takes the copy of the visible text screen. Read through the direct mapping
*  like every other access to that buffer in the kernel. Only the mode 13h
*  path needs this; the framebuffer path has fbcon's shadow buffer. */
static void gfx_text_store(void)
{
	volatile unsigned short *screen;
	int i;

	screen = (volatile unsigned short *)PAGE_VGA_TEXT;

	for(i = 0; i < GFX_TEXT_CELLS; i++)
	{
		gfx_text_page[i] = screen[i];
	}
}

/* Writes the copy back. Only meaningful in text mode -- in mode 13h the
*  writes would go nowhere, see the section comment. */
static void gfx_text_recall(void)
{
	volatile unsigned short *screen;
	int i;

	screen = (volatile unsigned short *)PAGE_VGA_TEXT;

	for(i = 0; i < GFX_TEXT_CELLS; i++)
	{
		screen[i] = gfx_text_page[i];
	}
}

/* Decides which surface this machine has and records its geometry. Called
*  before anything is drawn and before "gfx -i" reports, so both answer from
*  the same place.
*
*  fbcon_active() is the question, not fb_base(): what matters is whether the
*  screen the user is looking at is a framebuffer, and that is the decision
*  fbcon_activate() made, not what the bootloader reported. A mode that was
*  reported and then not taken over leaves the VGA text console in charge,
*  and mode 13h is then both available and correct.
*
*  Nothing is assumed about the size. fbdraw_width() and fbdraw_height()
*  report the surface that boot actually negotiated, and 800x600 and 640x480
*  come out of that negotiation just as readily as 1024x768. */
static void gfx_pick_surface(void)
{
	gfx_on_fb = (fbcon_active() && fbdraw_available());

	if(gfx_on_fb)
	{
		gfx_surf = &gfx_fb_ops;
		gfx_w = fbdraw_width();
		gfx_h = fbdraw_height();
	} else {
		gfx_surf = &gfx_vga_ops;
		gfx_w = VGA_WIDTH;
		gfx_h = VGA_HEIGHT;
	}

	gfx_unit = gfx_w / VGA_WIDTH;
	if(gfx_unit < 1) gfx_unit = 1;
}

/* Everything that has to happen before the first pixel: the surface is
*  chosen, nothing else may write to the screen, and whatever the screen
*  holds has to be recoverable. Returns 0 on success, and on failure leaves
*  the system exactly as it was found.
*
*  The framebuffer path has no mode to enter -- the mode is already there,
*  which is the whole point -- so there is nothing here that can fail on it.
*  The one case it refuses is a framebuffer console whose fbdraw reports no
*  surface: falling back to mode 13h there would be the exact mistake
*  fbdraw.h warns about, a switch away from a mode nothing can switch back
*  to. Drawing nothing is the only safe answer. */
static int gfx_enter(void)
{
	int rc;

	if(fbcon_active() && !fbdraw_available())
	{
		return GFX_NO_SURFACE;
	}

	gfx_pick_surface();
	gfx_statusbar_hold();

	if(gfx_on_fb)
	{
		return 0;
	}

	gfx_text_store();

	rc = vga_set_mode(VGA_MODE_GRAPHICS);

	if(rc != 0)
	{
		gfx_statusbar_release();
	}

	return rc;
}

/* And the way back. On the framebuffer that is fbcon_repaint(), which draws
*  the console -- text, colours and cursor -- back out of the shadow buffer
*  that was being kept all along; no mode is touched, because none was. In
*  mode 13h it is the mode first and the saved cells after, because the
*  console buffer can only be written once it is a console buffer again. */
static int gfx_leave(void)
{
	int rc;

	if(gfx_on_fb)
	{
		fbcon_repaint();
		gfx_statusbar_release();
		return 0;
	}

	rc = vga_set_mode(VGA_MODE_TEXT);
	gfx_text_recall();
	gfx_statusbar_release();

	return rc;
}

/* --- the layout ----------------------------------------------------------
*
*  Every GFX_* position and size in the constants at the top of this file is
*  a number in a 320x200 reference layout, and these two turn one into a
*  position on the surface actually in use. At 320x200 they are the identity,
*  which is what keeps the mode 13h picture exactly the picture it was.
*
*  Why scale the LAYOUT and not the picture. Blowing a finished 320x200 image
*  up by three would put a 960x600 postage stamp on a 1024x768 screen with a
*  64 by 168 pixel dead margin, and every circle and every line in it would
*  be three pixels wide because it was one pixel wide before. Deriving each
*  position from the surface instead fills the screen, uses the mode's own
*  aspect ratio -- 4:3 here, where mode 13h is 16:10 -- and draws every shape
*  at the resolution the surface really has, which is the only way a picture
*  can show what the mode can do rather than what mode 13h could do. It is
*  also the only version that works at all on the other two sizes the boot
*  negotiation can produce: an integer factor of three is wrong for 800x600
*  and wrong again for 640x480, and there is no factor that is right for all
*  three.
*
*  Horizontal and vertical are scaled separately because the aspect ratios
*  differ. Anything radial -- a circle's radius, an offset from a centre --
*  goes through gfx_sx(), since the horizontal ratio is the smaller of the
*  two on every mode the bootloader can set, so a circle sized by it stays
*  inside the box that was drawn for it. */
static int gfx_sx(int value)
{
	return (value * gfx_w) / VGA_WIDTH;
}

static int gfx_sy(int value)
{
	return (value * gfx_h) / VGA_HEIGHT;
}

/* The largest text scale in 1..most at which a string of that many
*  characters still fits into width pixels. The reference layout was drawn
*  around strings of a known length at scale 1; on a wider surface the
*  strings are not only bigger but sometimes different -- "fbdraw_circle" is
*  three characters longer than "vga_circle" -- so the size that fits is
*  worked out rather than assumed. Never returns less than 1: a label that
*  overhangs its panel is bad, a label that is not drawn at all is worse. */
static int gfx_fit_scale(int chars, int width, int most)
{
	int scale;

	for(scale = most; scale > 1; scale--)
	{
		if(chars * FONT_WIDTH * scale <= width) break;
	}

	return scale;
}

/* One palette entry into whichever surface is in use.
*
*  The components are the DAC's six bits, 0..63, because that is what mode
*  13h can store and there is no point in carrying precision one of the two
*  paths has to throw away. fbdraw_palette() takes full 0..255 channels --
*  it has no DAC, it fills a translation table -- so the six bits are spread
*  over eight here, the usual way: the top two bits repeated into the bottom,
*  so 63 becomes 255 and 0 stays 0 rather than 63 becoming 252.
*
*  The other difference is one of timing and is why every caller must set the
*  palette BEFORE it draws. In mode 13h the screen holds indices, so changing
*  an entry changes pixels that are already on it; on the framebuffer the
*  translation happened when the pixel was written and changing the entry
*  afterwards does nothing. gfx -t checks exactly that.
*
*  Indices 0..15 are left alone on both paths, and for a reason that only
*  applies to one of them: the DAC is one piece of hardware shared with text
*  mode, so a vga_palette(1, ...) here would come back as a shell whose blue
*  is no longer blue, long after the mode switch is over. fbdraw's table is
*  private and could not do that -- but the picture is one picture and its
*  colours are one set of indices, so the rule is kept on both. */
static void gfx_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	if(gfx_on_fb)
	{
		fbdraw_palette(index,
		               (uint8_t)((r << 2) | (r >> 4)),
		               (uint8_t)((g << 2) | (g >> 4)),
		               (uint8_t)((b << 2) | (b >> 4)));
	} else {
		vga_palette(index, r, g, b);
	}
}

/* Loads the palette the picture is painted with. Three ramps and a handful of
*  named colours, all of them at index 16 and above. */
static void gfx_setup_palette(void)
{
	int i;
	int pos;
	int region;
	int f;
	int r;
	int g;
	int b;

	gfx_palette(GFX_BLACK,      0,  0,  0);
	gfx_palette(GFX_WHITE,     63, 63, 63);
	gfx_palette(GFX_TEXT,      44, 46, 54);
	gfx_palette(GFX_AMBER,     63, 46, 12);
	gfx_palette(GFX_SHADOW,     6,  5, 12);
	gfx_palette(GFX_FRAME,     26, 30, 46);
	gfx_palette(GFX_PANEL,      8,  9, 20);
	gfx_palette(GFX_LEAF,      26, 50, 22);
	gfx_palette(GFX_LEAF_DARK, 12, 32, 14);
	gfx_palette(GFX_STEM,      18, 40, 16);

	/* The background: night blue at the top, warming towards the bottom.
	   All integer -- there is no floating point in this kernel, and a ramp
	   never needs any: the step is start + i * span / steps. */
	for(i = 0; i < GFX_SKY_STEPS; i++)
	{
		gfx_palette((uint8_t)(GFX_SKY_FIRST + i),
		            (uint8_t)(3 + (i * 18) / GFX_SKY_STEPS),
		            (uint8_t)(4 + (i * 10) / GFX_SKY_STEPS),
		            (uint8_t)(12 + (i * 14) / GFX_SKY_STEPS));
	}

	/* The tomato, from a nearly black rim to a lit skin. */
	for(i = 0; i < GFX_TOMATO_STEPS; i++)
	{
		gfx_palette((uint8_t)(GFX_TOMATO_FIRST + i),
		            (uint8_t)(22 + (i * 41) / (GFX_TOMATO_STEPS - 1)),
		            (uint8_t)(2 + (i * 40) / (GFX_TOMATO_STEPS - 1)),
		            (uint8_t)(2 + (i * 32) / (GFX_TOMATO_STEPS - 1)));
	}

	/* A full turn of hue at full saturation, done the way a hue really is
	   defined: six sectors of 64 steps, in each of which one channel rises
	   or falls while the other two stand still. pos runs 0..383 over the
	   whole sweep, the sector is pos / 64 and the position inside it
	   pos % 64 -- no trigonometry and no fractions anywhere. */
	for(i = 0; i < GFX_SPECTRUM_STEPS; i++)
	{
		pos = (i * 384) / GFX_SPECTRUM_STEPS;
		region = pos / 64;
		f = pos % 64;

		switch(region)
		{
			case 0:  r = 63;     g = f;      b = 0;      break;
			case 1:  r = 63 - f; g = 63;     b = 0;      break;
			case 2:  r = 0;      g = 63;     b = f;      break;
			case 3:  r = 0;      g = 63 - f; b = 63;     break;
			case 4:  r = f;      g = 0;      b = 63;     break;
			default: r = 63;     g = 0;      b = 63 - f; break;
		}

		gfx_palette((uint8_t)(GFX_SPECTRUM_FIRST + i),
		            (uint8_t)r, (uint8_t)g, (uint8_t)b);
	}
}

/* One glyph of the built-in font, blown up by an integer factor. The font is
*  part of the contract in vga.h -- one byte per row, most significant bit
*  leftmost -- so a set bit simply becomes a scale by scale block. This is
*  what the mode 13h path draws text with; vga_string() has no scale of its
*  own, and at scale 1 this produces exactly what it would have. */
static void gfx_char_scaled(int x, int y, char c, int scale, uint8_t colour)
{
	unsigned char index;
	unsigned char bits;
	int row;
	int col;

	index = (unsigned char)c;
	if(index >= 128) return;

	for(row = 0; row < FONT_HEIGHT; row++)
	{
		bits = font8x8[index][row];

		for(col = 0; col < FONT_WIDTH; col++)
		{
			if(bits & (0x80 >> col))
			{
				gfx_surf->fill(x + col * scale, y + row * scale,
				               scale, scale, colour);
			}
		}
	}
}

static void gfx_string_scaled(int x, int y, const char *s, int scale,
                              uint8_t colour)
{
	while(*s != EOS)
	{
		gfx_char_scaled(x, y, *s, scale, colour);
		x += FONT_WIDTH * scale;
		s++;
	}
}

/* A string on whichever surface is in use. The framebuffer has a scaled text
*  routine of its own and draws its glyphs a pixel at a time rather than as a
*  fill per pixel, which on a screen this size is the difference between a
*  caption and a wait; mode 13h has no scale in its API, so it goes through
*  the loop above. Both advance FONT_WIDTH * scale per glyph, which is what
*  the width calculations below rely on. */
static void gfx_text(int x, int y, const char *s, int scale, uint8_t colour)
{
	if(gfx_on_fb)
	{
		fbdraw_string(x, y, s, scale, colour);
	}
	else if(scale == 1)
	{
		/* vga_string() has no scale of its own, so it can only serve this
		   one case -- but it is the primitive the picture is there to show
		   off, and at scale 1 it draws exactly what the loop below would. */
		vga_string(x, y, s, colour, GFX_TRANSPARENT);
	} else {
		gfx_string_scaled(x, y, s, scale, colour);
	}
}

/* Draws a string so that its middle sits on cx. The same hand counting the
*  tables in this file do, only in pixels instead of columns. */
static void gfx_text_centred(int cx, int y, const char *s, int scale,
                             uint8_t colour)
{
	int width;

	width = (int)strlen(s) * FONT_WIDTH * scale;

	gfx_text(cx - width / 2, y, s, scale, colour);
}

/* Draws a string at x unless that would run it past right, in which case it
*  ends there instead. The reference positions were measured against strings
*  of a fixed length in a 320 pixel wide layout; the framebuffer path prints
*  its own resolution, which is a different number of characters, and a
*  header that overhangs the screen edge is not worth a second constant. */
static void gfx_text_clamped(int x, int y, const char *s, int scale,
                             int right, uint8_t colour)
{
	int width;

	width = (int)strlen(s) * FONT_WIDTH * scale;

	if(x + width > right) x = right - width;
	if(x < 0) x = 0;

	gfx_text(x, y, s, scale, colour);
}

/* An outline of a given thickness, drawn as nested rectangles. A rect is one
*  pixel thin on every surface, and one pixel on 1024x768 is a third of what
*  it was on 320x200 -- the frames would fade out exactly where the picture
*  got bigger. At thickness 1 this is the single rect it always was. */
static void gfx_frame(int x, int y, int w, int h, int thick, uint8_t colour)
{
	int i;

	for(i = 0; i < thick; i++)
	{
		gfx_surf->rect(x + i, y + i, w - 2 * i, h - 2 * i, colour);
	}
}

/* A line of a given thickness, as parallel Bresenham lines offset across the
*  direction it runs in: a shallow line is thickened downwards, a steep one
*  sideways, so the weight stays the same whichever way it points. */
static void gfx_thick_line(int x0, int y0, int x1, int y1, int thick,
                           uint8_t colour)
{
	int dx;
	int dy;
	int i;

	dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
	dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);

	for(i = 0; i < thick; i++)
	{
		if(dx >= dy)
		{
			gfx_surf->line(x0, y0 + i, x1, y1 + i, colour);
		} else {
			gfx_surf->line(x0 + i, y0, x1 + i, y1, colour);
		}
	}
}

/* A ring of a given thickness, as concentric circles grown outwards from the
*  nominal radius. Same reason as gfx_frame(). */
static void gfx_ring(int cx, int cy, int radius, int thick, uint8_t colour)
{
	int i;

	for(i = 0; i < thick; i++)
	{
		gfx_surf->circle(cx, cy, radius + i, colour);
	}
}

/* The line under the title: "320x200x256" in mode 13h, and the surface's own
*  numbers with its depth on the framebuffer, e.g. "1024x768x32". Built here
*  because there is no sprintf() in this kernel and printf() cannot be used
*  while the picture is up. */
static void gfx_mode_text(char *out)
{
	char *p;

	p = out;
	p = net_put_uint(p, (uint32_t)gfx_w);
	*p++ = 'x';
	p = net_put_uint(p, (uint32_t)gfx_h);
	*p++ = 'x';

	/* The last field is what the mode offers: mode 13h has 256 colours out
	   of a six bit DAC, the framebuffer has no palette at all and its depth
	   is the honest number to print. */
	p = net_put_uint(p, gfx_on_fb ? fbcon_bpp() : (uint32_t)GFX_VGA_COLOURS);

	*p = EOS;
}

/* The background and the outline of one of the three panels, with its label
*  centred along the bottom edge. */
static void gfx_panel(int x, const char *label, int scale)
{
	int y;
	int w;
	int h;

	y = gfx_sy(GFX_PANEL_Y);
	w = gfx_sx(GFX_PANEL_W);
	h = gfx_sy(GFX_PANEL_H);

	gfx_surf->fill(x, y, w, h, GFX_PANEL);
	gfx_frame(x, y, w, h, gfx_unit, GFX_FRAME);

	gfx_text_centred(x + w / 2, y + gfx_sy(GFX_LABEL_DY), label, scale,
	                 GFX_TEXT);
}

static void gfx_draw_picture(void)
{
	char mode_line[GFX_MODE_TEXT];
	const char *label1;
	const char *label2;
	const char *label3;
	const char *caption;
	const char *kind;
	const char *foot;
	int i;
	int t;
	int cx;
	int cy;
	int x0;
	int x1;
	int bx;
	int by;
	int bw;
	int bh;
	int margin;
	int right;
	int text_scale;
	int title_scale;
	int label_scale;

	/* The panels are labelled with the routines they demonstrate, which is
	   not the same set of routines on the two paths -- that is the point of
	   the label. The longest of the three decides the size all three are
	   drawn at, so they stay one size rather than one of them shrinking. */
	if(gfx_on_fb)
	{
		label1 = "fbdraw_disc";
		label2 = "fbdraw_line";
		label3 = "fbdraw_circle";
		caption = "Direct colour, 144-step hue sweep";
		kind = "VBE LFB";
	} else {
		label1 = "vga_disc";
		label2 = "vga_line";
		label3 = "vga_circle";
		caption = "256-colour DAC, 144-step hue sweep";
		kind = "MODE 13H";
	}

	foot = "Press any key to return to the shell";

	gfx_mode_text(mode_line);

	margin = gfx_sx(GFX_PANEL1_X);
	right = gfx_w - margin;

	text_scale = gfx_unit;
	title_scale = GFX_TITLE_SCALE * gfx_unit;
	label_scale = gfx_fit_scale((int)strlen(label3), gfx_sx(GFX_PANEL_W),
	                            gfx_unit);

	/* Background: one horizontal line per row, one palette entry per band.
	   64 entries over the height means each colour covers a few rows, which
	   at any of these sizes reads as a smooth wash. */
	for(i = 0; i < gfx_h; i++)
	{
		gfx_surf->hline(0, i, gfx_w,
		                (uint8_t)(GFX_SKY_FIRST + (i * GFX_SKY_STEPS) / gfx_h));
	}

	/* Header bar and the title, with a drop shadow behind it. */
	gfx_surf->fill(0, 0, gfx_w, gfx_sy(GFX_HEAD_H), GFX_PANEL);
	gfx_surf->fill(0, gfx_sy(GFX_HEAD_H), gfx_w, gfx_unit, GFX_AMBER);
	gfx_surf->fill(0, gfx_sy(GFX_HEAD_H) + gfx_unit, gfx_w, gfx_unit,
	               GFX_SHADOW);

	gfx_text(gfx_sx(13), gfx_sy(4), "TomatOS", title_scale, GFX_SHADOW);
	gfx_text(gfx_sx(11), gfx_sy(2), "TomatOS", title_scale, GFX_AMBER);

	gfx_text_clamped(gfx_sx(222), gfx_sy(5), mode_line, text_scale, right,
	                 GFX_TEXT);
	gfx_text_clamped(gfx_sx(246), gfx_sy(15), kind, text_scale, right,
	                 GFX_AMBER);

	/* --- panel 1: a shaded ball out of nested discs ---------------------
	   Each disc is a little smaller than the last and its centre moves up
	   and to the left, so the ramp lays itself down as light falling from
	   that corner. Sixteen steps cover the radius. */
	gfx_panel(margin, label1, label_scale);

	cx = gfx_sx(GFX_TOMATO_CX);
	cy = gfx_sy(GFX_TOMATO_CY);

	for(i = 0; i < GFX_TOMATO_STEPS; i++)
	{
		gfx_surf->disc(cx - gfx_sx(i / 2), cy - gfx_sx(i / 2),
		               gfx_sx(GFX_TOMATO_R - 2 * i),
		               (uint8_t)(GFX_TOMATO_FIRST + i));
	}

	gfx_ring(cx, cy, gfx_sx(GFX_TOMATO_R), gfx_unit, GFX_TOMATO_FIRST);
	gfx_surf->disc(cx - gfx_sx(12), cy - gfx_sx(12), gfx_sx(3), GFX_WHITE);

	/* Stem and calyx on top of the finished ball. Every leaf is drawn as a
	   band of lines rather than one, because a single Bresenham line is one
	   pixel thin and disappears against the skin at this size. */
	gfx_surf->fill(cx, cy - gfx_sx(44), gfx_unit, gfx_sx(14), GFX_STEM);
	gfx_surf->fill(cx + gfx_unit, cy - gfx_sx(44), gfx_unit, gfx_sx(14),
	               GFX_LEAF_DARK);
	gfx_surf->disc(cx, cy - gfx_sx(30), gfx_sx(6), GFX_LEAF_DARK);

	for(i = 0; i < 2 * gfx_unit; i++)
	{
		gfx_surf->line(cx, cy - gfx_sx(30) + i,
		               cx - gfx_sx(15), cy - gfx_sx(36) + i, GFX_LEAF);
		gfx_surf->line(cx, cy - gfx_sx(30) + i,
		               cx + gfx_sx(15), cy - gfx_sx(36) + i, GFX_LEAF);
		gfx_surf->line(cx, cy - gfx_sx(30) + i,
		               cx - gfx_sx(11), cy - gfx_sx(21) + i, GFX_LEAF);
		gfx_surf->line(cx, cy - gfx_sx(30) + i,
		               cx + gfx_sx(11), cy - gfx_sx(21) + i, GFX_LEAF);
	}

	/* --- panel 2: a fan of lines and a pair of rectangles ---------------
	   The fan starts in one corner and ends on the opposite two edges, so
	   the slopes run from steeper than vertical round to shallower than
	   horizontal -- every case a line routine has to get right. */
	gfx_panel(gfx_sx(GFX_PANEL2_X), label2, label_scale);

	for(i = 0; i < 9; i++)
	{
		gfx_thick_line(gfx_sx(118), gfx_sy(100),
		               gfx_sx(118 + i * 11), gfx_sy(40), gfx_unit,
		               (uint8_t)(GFX_SPECTRUM_FIRST + i * 16));
	}

	for(i = 1; i < 6; i++)
	{
		gfx_thick_line(gfx_sx(118), gfx_sy(100),
		               gfx_sx(206), gfx_sy(40 + i * 11), gfx_unit,
		               (uint8_t)(GFX_SPECTRUM_FIRST + GFX_SPECTRUM_STEPS
		                         - i * 16));
	}

	/* Filled next to outlined, the same size, so the difference between a
	   fill and a rect is there to be seen. */
	gfx_surf->fill(gfx_sx(122), gfx_sy(106), gfx_sx(36), gfx_sy(16),
	               GFX_AMBER);
	gfx_frame(gfx_sx(166), gfx_sy(106), gfx_sx(36), gfx_sy(16), gfx_unit,
	          GFX_WHITE);

	/* --- panel 3: rings and discs --------------------------------------- */
	gfx_panel(gfx_sx(GFX_PANEL3_X), label3, label_scale);

	for(i = 5; i >= 1; i--)
	{
		gfx_ring(gfx_sx(264), gfx_sy(74), gfx_sx(i * 6), gfx_unit,
		         (uint8_t)(GFX_SPECTRUM_FIRST + (5 - i) * 28));
	}

	for(i = 0; i < 3; i++)
	{
		gfx_surf->disc(gfx_sx(240 + i * 24), gfx_sy(114), gfx_sx(8),
		               (uint8_t)(GFX_SPECTRUM_FIRST + 20 + i * 44));
	}

	/* --- the palette band ------------------------------------------------
	   144 entries, framed, tiled across the band so that the last one ends
	   exactly on its right edge whatever the band is wide -- at 320 that
	   comes out at the two pixels each it always was. This is the part that
	   shows at a glance that there really are more than sixteen colours. */
	bx = gfx_sx(GFX_BAND_X);
	by = gfx_sy(GFX_BAND_Y);
	bw = gfx_sx(GFX_BAND_W);
	bh = gfx_sy(GFX_BAND_H);

	gfx_frame(bx - gfx_unit, by - gfx_unit, bw + 2 * gfx_unit,
	          bh + 2 * gfx_unit, gfx_unit, GFX_FRAME);

	for(i = 0; i < GFX_SPECTRUM_STEPS; i++)
	{
		x0 = bx + (i * bw) / GFX_SPECTRUM_STEPS;
		x1 = bx + ((i + 1) * bw) / GFX_SPECTRUM_STEPS;

		gfx_surf->fill(x0, by, x1 - x0, bh,
		               (uint8_t)(GFX_SPECTRUM_FIRST + i));
	}

	t = gfx_fit_scale((int)strlen(caption), gfx_w - 2 * margin, gfx_unit);
	gfx_text_centred(gfx_w / 2, gfx_sy(GFX_CAPTION_Y), caption, t, GFX_TEXT);

	/* --- footer ---------------------------------------------------------- */
	gfx_surf->fill(0, gfx_sy(GFX_FOOT_Y), gfx_w, gfx_h - gfx_sy(GFX_FOOT_Y),
	               GFX_PANEL);
	gfx_surf->fill(0, gfx_sy(GFX_FOOT_Y), gfx_w, gfx_unit, GFX_AMBER);

	t = gfx_fit_scale((int)strlen(foot), gfx_w - 2 * margin, gfx_unit);
	gfx_text_centred(gfx_w / 2, gfx_sy(GFX_FOOT_TEXT_Y), foot, t, GFX_WHITE);

	/* Border last, so nothing drawn over it can eat the corners. */
	gfx_frame(0, 0, gfx_w, gfx_h, gfx_unit, GFX_FRAME);
}

static void gfx_show(void)
{
	int rc;

	/* A key that was already waiting would be taken by the getch() below
	   before the picture had been looked at. kb_flush() clears the slot for
	   the special keys; the ordinary one has a slot of its own, and
	   getchn() is the non-blocking read that empties it. */
	kb_flush();
	getchn();

	rc = gfx_enter();

	if(rc == GFX_NO_SURFACE)
	{
		printf("The console is on a framebuffer that fbdraw cannot draw on.\n");
		printf("Switching to mode 13h would lose it for good, so nothing "
		       "was drawn.\n");
		return;
	}

	if(rc != 0)
	{
		printf("Graphics mode could not be entered, vga_set_mode() = %i\n", rc);
		return;
	}

	gfx_setup_palette();
	gfx_draw_picture();

	/* The keyboard IRQ carries on running while the picture is up -- nothing
	   about either path touches the PIC or the controller -- so the ordinary
	   blocking read is all that is needed here. */
	getch();

	rc = gfx_leave();

	if(rc != 0)
	{
		printf("Text mode could not be restored, vga_set_mode() = %i\n", rc);
		return;
	}

	if(gfx_on_fb)
	{
		printf("Console repainted, %i by %i pixels drawn into the "
		       "framebuffer.\n", gfx_w, gfx_h);
	} else {
		printf("Back in text mode, %i by %i pixels drawn.\n", gfx_w, gfx_h);
	}
}

/* --- gfx -i: the mode the machine actually came up in ---------------------
*
*  Two different things are reported here and they must not be conflated.
*  What the BOOTLOADER set is a property of the machine: kernel.c read it out
*  of the multiboot info at boot and hands it out through fb_base() and its
*  neighbours. Whether the framebuffer CONSOLE took that mode over is a
*  decision fbcon_activate() made afterwards, and it can perfectly well have
*  gone the other way -- a mode was reported and the mapping failed, or the
*  mode is the EGA text buffer and there was never anything to take over.
*  Both halves are printed, and the last lines say how they came out and what
*  the picture would therefore be drawn into.
*
*  It reads on a text mode boot too, which is still the normal case for
*  "make run" and for the GRUB path: the framebuffer block is simply left
*  out when nothing was reported, and the console lines then say the VGA text
*  console has the screen. */

/* Name of a multiboot framebuffer type. The same four cases kernel.c prints
*  in its boot line; kept here rather than exported because the string is
*  presentation, not state. */
static const char *gfx_mode_kind(void)
{
	switch(fb_kind())
	{
		case MULTIBOOT_FRAMEBUFFER_INDEXED:  return "indexed";
		case MULTIBOOT_FRAMEBUFFER_RGB:      return "RGB";
		case MULTIBOOT_FRAMEBUFFER_EGA_TEXT: return "EGA text";
		default:                             return "unknown";
	}
}

/* The label column: two spaces, the label, and blanks up to GFX_INFO_LABEL.
*  printf() has no field widths, so the padding is counted out here -- the
*  same idea as mem_print_right(), only on the other side of the text. The
*  labels differ in length by seven characters, and a column maintained by
*  hand in nine format strings is a column that drifts. */
static void gfx_info_label(const char *label)
{
	int used;

	printf("  %s", label);

	used = 2 + (int)strlen(label);
	while(used < GFX_INFO_LABEL)
	{
		putch(' ');
		used++;
	}
}

static void gfx_show_mode(void)
{
	const char *info;
	uint32_t phys;
	uint32_t virt;
	int reported;
	int graphics_mode;

	phys = fb_base();
	info = fbcon_info();
	if(info == 0) info = "no description";

	/* A base address of zero is the "nothing reported" case: kernel.c leaves
	   its record untouched when the multiboot info carries no framebuffer
	   bit, which is what GRUB Legacy always does and what GRUB 2 does for a
	   boot that asked for no video mode. A REPORTED EGA text framebuffer is
	   something else again -- GRUB 2 describes the 80x25 buffer at 0xB8000
	   that way -- and it is a framebuffer with an address and a pitch that
	   simply must not be drawn into. Hence two flags, not one. */
	reported = (phys != 0);
	graphics_mode = (reported && fb_kind() != MULTIBOOT_FRAMEBUFFER_EGA_TEXT);

	printf("Framebuffer mode:\n");

	gfx_info_label("Reported:");

	if(!reported)
	{
		printf("nothing, the bootloader set no video mode\n");
	}
	else
	{
		printf("%s (multiboot type %i)\n", gfx_mode_kind(), (int)fb_kind());

		gfx_info_label("Resolution:");
		mem_print_right(fb_pixel_width(), 5);
		printf(" x ");
		mem_print_right(fb_pixel_height(), 5);
		printf(", ");
		mem_print_right(fb_bits_per_pixel(), 3);
		printf(" bpp\n");

		/* The pitch is the one number that cannot be derived: rows are
		   commonly padded, so it is not width * bpp / 8, and computing a row
		   offset from the width is the classic way to get a picture that
		   slants across the screen. The total behind it is pitch times
		   height, i.e. the memory the mode really occupies. */
		gfx_info_label("Pitch:");
		mem_print_right(fb_pitch_bytes(), 5);
		printf(" bytes per row, ");
		mem_print_right((fb_pitch_bytes() * fb_pixel_height()) / 1024u, 6);
		printf(" KiB total\n");

		gfx_info_label("Physical:");
		page_print_hex(phys, 8);
		printf("\n");

		/* Two different virtual addresses could answer here, and conflating
		   them would be the mistake. A framebuffer inside the direct mapping
		   -- the EGA text buffer at 0xB8000 is one -- has an alias P2V()
		   names, and that alias is the answer. QEMU's card at 0xFD000000 is
		   above DIRECT_MAP_LIMIT and has no alias at all; it is reachable
		   only through the window fbcon_activate() mapped for it. That
		   address belongs to fbcon, so the fbcon line below names it rather
		   than this one inventing a second source of truth for it. */
		virt = (phys <= DIRECT_MAP_LIMIT) ? (uint32_t)P2V(phys) : 0;

		gfx_info_label("Virtual:");
		if(virt != 0 && vmm_is_mapped(virt))
		{
			page_print_hex(virt, 8);
			printf(" (direct mapping)\n");
		} else {
			printf("no direct mapping, see the fbcon line\n");
		}
	}

	/* The console geometry is fbcon's own and is zero whenever there is no
	   framebuffer console -- which, read next to the line under it, says
	   exactly the right thing rather than looking like a missing number. */
	gfx_info_label("Console:");
	mem_print_right((uint32_t)fbcon_cols(), 5);
	printf(" x ");
	mem_print_right((uint32_t)fbcon_rows(), 5);
	printf(" characters\n");

	gfx_info_label("In charge:");
	if(fbcon_active())
	{
		printf("framebuffer console\n");
	} else {
		printf("VGA text console, 80 x 25 characters\n");
	}

	gfx_info_label("fbcon:");
	printf("%s\n", info);

	/* Which of the two surfaces "gfx" would use, and how big the picture
	   would be. This is the line the whole command exists to answer, and it
	   is deliberately asked of the same routine the drawing asks, so that
	   what is reported here cannot drift from what actually happens. */
	gfx_pick_surface();

	gfx_info_label("Picture into:");

	if(gfx_on_fb)
	{
		printf("the framebuffer already on the screen,\n");
		gfx_info_label("");
		mem_print_right((uint32_t)gfx_w, 5);
		printf(" x ");
		mem_print_right((uint32_t)gfx_h, 5);
		printf(" pixels through fbdraw\n");
	}
	else if(fbcon_active())
	{
		printf("nothing, fbdraw reports no surface\n");
	} else {
		printf("VGA mode 13h,\n");
		gfx_info_label("");
		mem_print_right((uint32_t)gfx_w, 5);
		printf(" x ");
		mem_print_right((uint32_t)gfx_h, 5);
		printf(" pixels, entered and left again\n");
	}

	/* The marker only appears where there is a pass and a fail to tell
	   apart. A text mode boot is the normal case for "make run" and for the
	   GRUB path, not a failure, so it gets no [FAILED]. A graphics mode that
	   was reported and then not taken over is the case worth flagging: the
	   screen would be showing nothing at all, and this line would be one of
	   the things nobody could read. */
	if(graphics_mode)
	{
		if(fbcon_active())
		{
			printf("  [  OK  ] The reported graphics mode is on the screen.\n");
		} else {
			printf("  [FAILED] The graphics mode was not taken over.\n");
		}
	}
}

/* Records the result of a check, like mem_check() does for the heap. */
static void gfx_check(int ok)
{
	gfx_tests_run++;

	if(ok)
	{
		gfx_tests_ok++;
		printf("  [  OK  ] ");
	} else {
		printf("  [FAILED] ");
	}
}

/* --- gfx -t --------------------------------------------------------------
*
*  Two self-tests, because the two paths do not share a single assertion
*  worth making. Mode 13h is a mode this command enters and leaves, and half
*  of what is worth checking there is that the switch happened and came back;
*  on the framebuffer there is no switch at all, and asserting one -- or
*  asserting that vga_framebuffer() became non-zero, or that vga_mode() moved
*  -- would be asserting something the path deliberately does not do. The
*  framebuffer test asserts the opposite of that instead, which is the real
*  invariant: the mode on the screen is still exactly the mode that was
*  there.
*
*  What both do check, and by the same means, is that a pixel written is a
*  pixel present and that a shape hanging off the left edge is clipped rather
*  than wrapped onto the row above. Mode 13h reads it back with
*  vga_pixel_at(). fbdraw.h offers no read-back at all -- it is a write-only
*  contract, and rightly so, since reading a framebuffer over the bus is slow
*  enough that no drawing routine should ever do it -- so the framebuffer
*  test reads the handful of pixels it needs out of fbcon_pixels() itself,
*  which is the memory fbdraw just wrote into. */

/* One pixel straight out of the mapped framebuffer, assembled little endian
*  from however many bytes the mode's depth takes. Only the self-test calls
*  this, and only a few times: see the note in fbcon.h about reads. */
static uint32_t gfx_fb_read(int x, int y)
{
	volatile uint8_t *p;
	uint32_t value;
	uint32_t bytes;
	uint32_t i;

	p = (volatile uint8_t *)fbcon_pixels();
	bytes = fbcon_bpp() / 8;

	if(p == 0 || bytes == 0 || bytes > 4) return 0;

	p = p + (uint32_t)y * fbcon_pitch() + (uint32_t)x * bytes;

	value = 0;
	for(i = 0; i < bytes; i++)
	{
		value = value | (((uint32_t)p[i]) << (i * 8));
	}

	return value;
}

static void gfx_selftest_vga(void)
{
	uint8_t *fb_text;
	uint8_t *fb_graphics;
	uint8_t *fb_back;
	int mode_text;
	int mode_graphics;
	int mode_back;
	int rc_in;
	int rc_out;
	int readback;
	int edge_left;
	int edge_right;
	int guard;
	int wrap;

	/* Everything is measured first and printed afterwards: see point 3 of
	   the section comment -- a printf() between the two mode switches is
	   thrown away by the hardware and scrolls the console out from under
	   the copy gfx_enter() took. */
	readback = -1;
	edge_left = -1;
	edge_right = -1;
	guard = -1;
	wrap = -1;
	rc_out = -1;
	mode_graphics = -1;

	mode_text = vga_mode();
	fb_text = vga_framebuffer();
	fb_graphics = 0;

	rc_in = gfx_enter();

	if(rc_in == 0)
	{
		mode_graphics = vga_mode();
		fb_graphics = vga_framebuffer();

		vga_clear(GFX_BLACK);

		/* One pixel, written and read straight back. */
		vga_pixel(GFX_TEST_X, GFX_TEST_Y, GFX_TEST_COLOUR);
		readback = (int)vga_pixel_at(GFX_TEST_X, GFX_TEST_Y);

		/* The two guards, then the shape that hangs off the left edge. */
		vga_pixel(GFX_GUARD_X, GFX_GUARD_Y, GFX_GUARD_COLOUR);
		vga_pixel(VGA_WIDTH - 1, GFX_GUARD_Y - 1, GFX_GUARD_COLOUR);

		vga_fill(GFX_CLIP_X, GFX_GUARD_Y, GFX_CLIP_W, 1, GFX_CLIP_COLOUR);

		edge_left = (int)vga_pixel_at(0, GFX_GUARD_Y);
		edge_right = (int)vga_pixel_at(GFX_GUARD_X - 1, GFX_GUARD_Y);
		guard = (int)vga_pixel_at(GFX_GUARD_X, GFX_GUARD_Y);
		wrap = (int)vga_pixel_at(VGA_WIDTH - 1, GFX_GUARD_Y - 1);

		rc_out = gfx_leave();
	}

	mode_back = vga_mode();
	fb_back = vga_framebuffer();

	printf("Graphics self-test, VGA mode 13h:\n");
	printf("  Text mode is %i, graphics mode is %i\n",
	       VGA_MODE_TEXT, VGA_MODE_GRAPHICS);

	/* 1. The mode switch itself, and vga_mode() agreeing with it. */
	gfx_check(rc_in == 0 && mode_graphics == VGA_MODE_GRAPHICS);
	printf("vga_set_mode(%i) = %i, vga_mode() %i -> %i\n",
	       VGA_MODE_GRAPHICS, rc_in, mode_text, mode_graphics);

	/* 2. The framebuffer exists exactly while the mode does. */
	gfx_check(fb_text == 0 && fb_graphics != 0);
	printf("Framebuffer 0x%X in text mode, 0x%X in graphics mode\n",
	       (int)fb_text, (int)fb_graphics);

	/* 3. A pixel that is written has to be there afterwards. */
	gfx_check(readback == GFX_TEST_COLOUR);
	printf("Pixel (%i,%i): wrote %i, vga_pixel_at() read %i\n",
	       GFX_TEST_X, GFX_TEST_Y, GFX_TEST_COLOUR, readback);

	/* 4. The clipped shape has to draw the part that is on the screen ... */
	gfx_check(edge_left == GFX_CLIP_COLOUR && edge_right == GFX_CLIP_COLOUR);
	printf("Fill x %i..%i drew (0,%i) = %i and (%i,%i) = %i\n",
	       GFX_CLIP_X, GFX_CLIP_X + GFX_CLIP_W - 1, GFX_GUARD_Y, edge_left,
	       GFX_GUARD_X - 1, GFX_GUARD_Y, edge_right);

	/* 5. ... and nothing beyond it: neither the next pixel to the right nor
	      the end of the row above, where an unclipped negative start column
	      would have wrapped to. */
	gfx_check(guard == GFX_GUARD_COLOUR && wrap == GFX_GUARD_COLOUR);
	printf("Guard (%i,%i) = %i, wrap guard (%i,%i) = %i, both set to %i\n",
	       GFX_GUARD_X, GFX_GUARD_Y, guard, VGA_WIDTH - 1, GFX_GUARD_Y - 1,
	       wrap, GFX_GUARD_COLOUR);

	/* 6. And the way back, which is the whole point of the exercise. */
	gfx_check(rc_out == 0 && mode_back == VGA_MODE_TEXT && fb_back == 0);
	printf("vga_set_mode(%i) = %i, vga_mode() = %i, framebuffer 0x%X\n",
	       VGA_MODE_TEXT, rc_out, mode_back, (int)fb_back);
}

static void gfx_selftest_fb(void)
{
	uint32_t want;
	uint32_t written;
	uint32_t recoloured;
	uint32_t edge_left;
	uint32_t edge_right;
	uint32_t guard;
	uint32_t wrap;
	uint32_t guard_want;
	uint32_t clip_want;
	uint32_t restored;
	uint32_t pitch;
	uint32_t least;
	int width;
	int height;
	int rc_in;
	int rc_out;
	int mode_after;
	int active_after;
	uint8_t *vga_fb_after;

	written = 1;
	recoloured = 0;
	edge_left = 1;
	edge_right = 1;
	guard = 0;
	wrap = 0;
	restored = 0;
	rc_out = -1;

	width = fbdraw_width();
	height = fbdraw_height();
	pitch = fbcon_pitch();
	least = (uint32_t)width * (fbcon_bpp() / 8);

	/* What the three test colours have to look like in memory once fbdraw
	   has translated them. fbcon_rgb() is the mode's own packing, so the
	   comparison holds at 32, 24 and 16 bits alike -- at 16 both sides lose
	   the same low bits. */
	want = fbcon_rgb(GFX_FB_TEST_R, GFX_FB_TEST_G, GFX_FB_TEST_B);
	guard_want = fbcon_rgb(GFX_FB_GUARD_R, GFX_FB_GUARD_G, GFX_FB_GUARD_B);
	clip_want = fbcon_rgb(GFX_FB_CLIP_R, GFX_FB_CLIP_G, GFX_FB_CLIP_B);

	rc_in = gfx_enter();

	if(rc_in == 0)
	{
		fbdraw_palette(GFX_TEST_COLOUR, GFX_FB_TEST_R, GFX_FB_TEST_G,
		               GFX_FB_TEST_B);
		fbdraw_palette(GFX_GUARD_COLOUR, GFX_FB_GUARD_R, GFX_FB_GUARD_G,
		               GFX_FB_GUARD_B);
		fbdraw_palette(GFX_CLIP_COLOUR, GFX_FB_CLIP_R, GFX_FB_CLIP_G,
		               GFX_FB_CLIP_B);

		fbdraw_clear(GFX_BLACK);

		/* One pixel, written and read straight back out of the mapped
		   framebuffer. The test point is neither on row 0 nor in column 0,
		   so a row offset computed from the width instead of the pitch
		   lands somewhere else and this fails. */
		fbdraw_pixel(GFX_TEST_X, GFX_TEST_Y, GFX_TEST_COLOUR);
		written = gfx_fb_read(GFX_TEST_X, GFX_TEST_Y);

		/* The documented difference from mode 13h, and the reason the
		   palette must be programmed before anything is drawn: the screen
		   holds colours here, not indices, so changing the entry now must
		   leave the pixel exactly as it is. In mode 13h the same two lines
		   would repaint it. */
		fbdraw_palette(GFX_TEST_COLOUR, GFX_FB_OTHER_R, GFX_FB_OTHER_G,
		               GFX_FB_OTHER_B);
		recoloured = gfx_fb_read(GFX_TEST_X, GFX_TEST_Y);

		/* The two guards, then the shape that hangs off the left edge. */
		fbdraw_pixel(GFX_GUARD_X, GFX_GUARD_Y, GFX_GUARD_COLOUR);
		fbdraw_pixel(width - 1, GFX_GUARD_Y - 1, GFX_GUARD_COLOUR);

		fbdraw_fill(GFX_CLIP_X, GFX_GUARD_Y, GFX_CLIP_W, 1,
		            GFX_CLIP_COLOUR);

		edge_left = gfx_fb_read(0, GFX_GUARD_Y);
		edge_right = gfx_fb_read(GFX_GUARD_X - 1, GFX_GUARD_Y);
		guard = gfx_fb_read(GFX_GUARD_X, GFX_GUARD_Y);
		wrap = gfx_fb_read(width - 1, GFX_GUARD_Y - 1);

		rc_out = gfx_leave();

		/* After the repaint the test pixel has to be console again. The
		   colour it was painted is nothing the console draws with, so a
		   framebuffer that still holds it is a framebuffer nobody put
		   back. */
		restored = gfx_fb_read(GFX_TEST_X, GFX_TEST_Y);
	}

	mode_after = vga_mode();
	vga_fb_after = vga_framebuffer();
	active_after = fbcon_active();

	printf("Graphics self-test, framebuffer:\n");
	printf("  Drawing through fbdraw, no mode is switched\n");

	/* 1. The surface fbdraw draws on is the surface the console is on.
	      Two different answers here would mean the picture goes somewhere
	      other than the screen, and the pitch is checked with them because
	      it is the one number that cannot be derived from the other two. */
	gfx_check(rc_in == 0 && fbdraw_available() && width > 0 && height > 0
	          && (uint32_t)width == fbcon_width()
	          && (uint32_t)height == fbcon_height()
	          && pitch >= least);
	printf("fbdraw %i x %i, fbcon %i x %i, pitch %i >= %i bytes\n",
	       width, height, (int)fbcon_width(), (int)fbcon_height(),
	       (int)pitch, (int)least);

	/* 2. A pixel that is written has to be there afterwards, carrying the
	      colour the palette entry was given. */
	gfx_check(written == want);
	printf("Pixel (%i,%i): index %i wanted 0x%X, framebuffer holds 0x%X\n",
	       GFX_TEST_X, GFX_TEST_Y, GFX_TEST_COLOUR, (int)want, (int)written);

	/* 3. The palette is a translation table, not a hardware DAC. */
	gfx_check(recoloured == written);
	printf("Entry %i changed after drawing: pixel 0x%X -> 0x%X, unchanged\n",
	       GFX_TEST_COLOUR, (int)written, (int)recoloured);

	/* 4. The clipped shape has to draw the part that is on the screen ... */
	gfx_check(edge_left == clip_want && edge_right == clip_want);
	printf("Fill x %i..%i drew (0,%i) = 0x%X and (%i,%i) = 0x%X\n",
	       GFX_CLIP_X, GFX_CLIP_X + GFX_CLIP_W - 1, GFX_GUARD_Y,
	       (int)edge_left, GFX_GUARD_X - 1, GFX_GUARD_Y, (int)edge_right);

	/* 5. ... and nothing beyond it: neither the next pixel to the right nor
	      the end of the row above, where an unclipped negative start column
	      would have wrapped to. */
	gfx_check(guard == guard_want && wrap == guard_want);
	printf("Guard (%i,%i) = 0x%X, wrap guard (%i,%i) = 0x%X, both 0x%X\n",
	       GFX_GUARD_X, GFX_GUARD_Y, (int)guard, width - 1, GFX_GUARD_Y - 1,
	       (int)wrap, (int)guard_want);

	/* 6. The way back, which on this path is two claims at once: the console
	      was repainted over the picture, and the mode on the screen was
	      never touched -- no VGA mode switch, no mode 13h framebuffer, and
	      fbcon still in charge. That last part is the one thing this path
	      must never get wrong, because it could not be undone. */
	gfx_check(rc_out == 0 && restored != want && active_after
	          && mode_after == VGA_MODE_TEXT && vga_fb_after == 0);
	printf("Repainted (%i,%i) = 0x%X, vga_mode() = %i, VGA framebuffer 0x%X\n",
	       GFX_TEST_X, GFX_TEST_Y, (int)restored, mode_after,
	       (int)vga_fb_after);
}

static void gfx_selftest(void)
{
	gfx_tests_run = 0;
	gfx_tests_ok = 0;

	gfx_pick_surface();

	if(fbcon_active() && !fbdraw_available())
	{
		printf("Graphics self-test:\n");
		printf("  The console is on a framebuffer that fbdraw cannot draw\n");
		printf("  on, and mode 13h would be a one way trip off it. There\n");
		printf("  is nothing here that can be tested without a surface.\n");
		return;
	}

	if(gfx_on_fb)
	{
		gfx_selftest_fb();
	} else {
		gfx_selftest_vga();
	}

	printf("  Result: %i of %i checks passed\n",
	       gfx_tests_ok, gfx_tests_run);
}

void graphics(char *cmd)
{
	char opt[100];

	if(prmc(cmd) == 0)
	{
		gfx_show();
		return;
	}

	/* prmv() hands back a pointer into one static buffer, so the option is
	   copied out before anything else can call prmv() again. */
	strcpy(opt, prmv(1, cmd));

	if(strcmp(opt, "-t") == 0)
	{
		gfx_selftest();
	}
	else if(strcmp(opt, "-i") == 0)
	{
		gfx_show_mode();
	} else {
		printf("Syntax: gfx [-t] [-i]\n");
		printf("\t          Draw the demo picture, any key returns\n");
		printf("\t-t        Run the self-test for the surface in use\n");
		printf("\t-i        Show the mode the machine booted into\n");
	}
}

/* --- mouse ---------------------------------------------------------------
*
*  Why this command exists at all. Nothing draws a cursor yet -- there is no
*  GUI to draw one in -- so the only way to tell a driver that works from one
*  that does not is to print what the interface says and watch it while the
*  mouse is moved. Every field below is one that separates a fault from a
*  device merely being used.
*
*  NOTHING HERE KNOWS WHAT KIND OF POINTER IT IS. mouse.h is deliberately not
*  about any one bus, because a second driver is meant to fill the same queue
*  later, and a shell command that printed a packet layout or an interrupt
*  number would be the first thing to break the day it does. So this asks
*  mouse.h its questions and prints the answers, and the one place a device
*  may name itself is mouse_describe(), whose sentence is printed as it comes.
*
*  Three things are shown that the position by itself does not say.
*
*  THE EVENTS, not only the state. mouse_buttons() answers "what is down
*  now", and a click that is pressed and released between two looks at it is
*  invisible -- which, on a queue read a few times a second, is most clicks.
*  So the loop drains mouse_poll() and prints a line for every press and every
*  release, while the row underneath keeps the state. Draining is also what
*  keeps the queue empty: a reader that only looked at the position would let
*  the queue fill and the dropped counter climb, and would then be displaying
*  a fault it had caused itself.
*
*  THE COUNTERS, and resyncs before the others. A pointer that jumps looks
*  exactly like a driver bug and very often is not one: if the stream keeps
*  losing its framing this counter says so, and the fault is below the driver.
*  Overflows and drops then separate a device moving faster than one event can
*  describe from a queue nobody emptied in time. Three different faults that
*  look identical on the screen, told apart by three numbers.
*
*  AND THAT IT WAITS RATHER THAN SPINS. The loop blocks on
*  mouse_wait_channel() with the idiom system.h spells out, so a mouse lying
*  still costs this machine five wakeups a second and nothing else. "ps" from
*  a second shell shows the task Blocked, and that is the whole difference
*  from the polling loop this would otherwise have been.
*/

/* What "the surface" is when there is no surface.
*
*  mouse_set_bounds() wants the screen the pointer is drawn on, in pixels, and
*  a text shell draws nothing: there is no pixel surface here and no cursor
*  whose clamping could be seen. Bounds are still needed -- a driver with none
*  has no idea how far right is -- so the honest choice is the screen the user
*  is actually looking at, expressed in the pixels it would have if it were a
*  surface.
*
*  On a framebuffer console that is a real number and is asked for. On the VGA
*  text console there are no pixels to ask about, so a character cell is taken
*  at the size of the font that draws it, 8 by 16, which makes the 80 by 25
*  screen 640 by 400. Both give the one property that matters: moving the
*  mouse to the right edge of the screen puts the pointer at the right edge of
*  its field, so the clamping happens where the user expects to see it. A
*  field of some invented size would clamp somewhere in the middle of the
*  screen and look like a bug.
*
*  THE SHELL REMEMBERS WHAT IT SET, because mouse.h offers no way to ask. That
*  is right on both sides: the driver is told the surface by whoever owns it,
*  and a display that prints a position has to be able to say what field the
*  number is in. "mouse -b" writes the same three variables, so a field set by
*  hand survives the next "mouse" instead of being quietly reset -- which is
*  what makes it possible to walk the pointer into a corner of a small field
*  and watch it stop. */
#define MOUSE_FIELD_NONE     0   /* nothing has set the bounds yet         */
#define MOUSE_FIELD_CONSOLE  1   /* the screen the console is on           */
#define MOUSE_FIELD_TYPED    2   /* whatever "mouse -b" was given          */

static int mouse_field_w = 0;
static int mouse_field_h = 0;
static int mouse_field_source = MOUSE_FIELD_NONE;

/* Self-test counters, the same pair every other self-test in this file
*  keeps. */
static int mouse_tests_run = 0;
static int mouse_tests_ok = 0;

/* The size of a text cell in pixels, and the text screen in cells. Only used
*  to turn the one into the other, in both directions: the field is cells
*  times cell size, and "mouse -i" divides back to say which cell the pointer
*  is over. */
static void mouse_console_field(int *w, int *h)
{
	if(fbcon_active() && fbcon_width() != 0 && fbcon_height() != 0)
	{
		*w = (int)fbcon_width();
		*h = (int)fbcon_height();
		return;
	}

	*w = MOUSE_TEXT_COLS * MOUSE_CELL_W;
	*h = MOUSE_TEXT_ROWS * MOUSE_CELL_H;
}

/* Gives the driver a field to work in, once. Every entry point calls this,
*  and a field somebody typed is left alone -- see the comment above. */
static void mouse_apply_field(void)
{
	int w;
	int h;

	if(mouse_field_source != MOUSE_FIELD_NONE) return;

	mouse_console_field(&w, &h);
	mouse_set_bounds(w, h);

	mouse_field_w = w;
	mouse_field_h = h;
	mouse_field_source = MOUSE_FIELD_CONSOLE;
}

/* Where the field came from, in the words the display uses. */
static const char *mouse_field_text(void)
{
	if(mouse_field_source == MOUSE_FIELD_TYPED) return "set with mouse -b";
	if(fbcon_active()) return "the framebuffer console";
	return "the text console, 8 x 16 pixels a cell";
}

/* A signed value right-aligned in a field of the given width, with the sign
*  immediately in front of the digits rather than out at the left edge of the
*  column. mem_print_right() takes a uint32_t and would print a movement of
*  -1 as 4294967295; this is its counterpart for the two columns that carry a
*  direction. plus asks for an explicit '+', which is worth two characters on
*  a movement -- a column of bare numbers that sometimes carry a minus reads
*  as a column of magnitudes with mistakes in it. */
static void mouse_print_signed(int value, int width, int plus)
{
	unsigned int magnitude;
	unsigned int rest;
	int digits;
	int i;

	magnitude = (value < 0) ? (unsigned int)(-value) : (unsigned int)value;

	digits = 1;
	rest = magnitude;
	while(rest >= 10)
	{
		rest = rest / 10;
		digits++;
	}

	if(value < 0 || plus) digits++;

	for(i = digits; i < width; i++)
	{
		putch(' ');
	}

	if(value < 0)
	{
		putch('-');
	}
	else if(plus)
	{
		putch('+');
	}

	printf("%u", (int)magnitude);
}

/* "(nnnnn,nnnnn)", exactly MOUSE_PAIR_WIDTH columns wide whatever the two
*  values are. Both the position and the movement are printed with it, so the
*  two columns line up under each other. */
static void mouse_print_pair(int a, int b, int plus)
{
	putch('(');
	mouse_print_signed(a, MOUSE_COORD_WIDTH, plus);
	putch(',');
	mouse_print_signed(b, MOUSE_COORD_WIDTH, plus);
	putch(')');
}

/* Which buttons are down, as three fixed positions rather than a list: a
*  list would change width as buttons go down and up, and this is written
*  into a row that is rewritten in place. L, M and R in the order they sit
*  under the hand, a dash for one that is up. */
static void mouse_button_text(int buttons, char *text)
{
	text[0] = (buttons & MOUSE_BUTTON_LEFT)   ? 'L' : '-';
	text[1] = (buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : '-';
	text[2] = (buttons & MOUSE_BUTTON_RIGHT)  ? 'R' : '-';
	text[3] = EOS;
}

/* One button's name, for the event lines. Only the three mouse.h defines are
*  named; anything else keeps its number and says so, because a driver that
*  reports a fourth button is exactly the thing this command should not hide. */
static const char *mouse_button_name(int mask)
{
	switch(mask)
	{
		case MOUSE_BUTTON_LEFT:   return "left";
		case MOUSE_BUTTON_MIDDLE: return "middle";
		case MOUSE_BUTTON_RIGHT:  return "right";
		default:                  break;
	}

	return "other";
}

/* The header over the live row. The widths are the row's own, so the two
*  cannot drift apart: a column here is one field there. */
static void mouse_row_header(void)
{
	printf("  ");
	ps_print_left("Position", MOUSE_PAIR_WIDTH + MOUSE_GAP);
	ps_print_left("Movement", MOUSE_PAIR_WIDTH + MOUSE_GAP);
	ps_print_left("Btn", MOUSE_BTN_WIDTH);
	fs_print_right_text("Packets", MOUSE_COUNT_WIDTH);
	fs_print_right_text("Resyncs", MOUSE_COUNT_WIDTH);
	fs_print_right_text("Overflow", MOUSE_COUNT_WIDTH);
	fs_print_right_text("Dropped", MOUSE_COUNT_WIDTH);
	printf("\n");
}

/* The row itself, written from the margin of the line it is already on and
*  without a newline after it, so the next call overwrites it. That is what
*  makes it a live display instead of a page of scrolling numbers, and it is
*  why every field is padded rather than truncated: a carriage return moves
*  the cursor and erases nothing, so a row that shrank from four digits to
*  three would leave the fourth standing there. */
static void mouse_row(int x, int y, int dx, int dy, int buttons)
{
	char text[4];

	mouse_button_text(buttons, text);

	printf("\r  ");
	mouse_print_pair(x, y, 0);
	printf("  ");
	mouse_print_pair(dx, dy, 1);
	printf("  ");
	ps_print_left(text, MOUSE_BTN_WIDTH);
	mem_print_right(mouse_packets(), MOUSE_COUNT_WIDTH);
	mem_print_right(mouse_resyncs(), MOUSE_COUNT_WIDTH);
	mem_print_right(mouse_overflows(), MOUSE_COUNT_WIDTH);
	mem_print_right(mouse_dropped(), MOUSE_COUNT_WIDTH);
}

/* Blanks the row and comes back to the margin, for the moment before a line
*  is printed over it. Without this the new line would inherit whatever of the
*  row it did not cover. */
static void mouse_clear_row(void)
{
	int i;

	putch('\r');
	for(i = 0; i < MOUSE_ROW_WIDTH; i++)
	{
		putch(' ');
	}
	putch('\r');
}

/* The label column of "mouse -i", padded the way "gfx -i" and "ifconfig" pad
*  theirs, and for the same reason: printf() has no field widths. */
static void mouse_info_label(const char *label)
{
	int used;

	printf("  %s", label);

	used = 2 + (int)strlen(label);
	while(used < MOUSE_INFO_LABEL)
	{
		putch(' ');
		used++;
	}
}

/* Having no pointer is an ordinary state, so it is explained rather than
*  reported as a failure -- the same answer the network commands give for a
*  machine with no card, and in one place so that all four entry points give
*  it identically. */
static void mouse_explain_absent(void)
{
	printf("  No pointing device is present.\n");
	printf("  Nothing answered when the driver looked for one, which is an\n");
	printf("  ordinary outcome rather than a failure: a machine may simply\n");
	printf("  have none, and a virtual machine has none unless it is given\n");
	printf("  one. Everything above the interface then sees a pointer that\n");
	printf("  never moves, so nothing else stops working -- there is only\n");
	printf("  nothing here to show.\n");
}

/* The two counters that mean something is wrong, and the one that means the
*  reader was too slow. Said only when they are not zero, the way "ifconfig"
*  explains an overrun only while there has been one: prose next to a zero is
*  prose that teaches the reader to skip the paragraph.
*
*  The values are passed in rather than read here, because both callers have a
*  different pair of them -- "mouse -i" shows the totals since boot and the
*  live view shows what moved while it was running, and the second is the one
*  that says "this is happening now". */
static void mouse_explain_counters(uint32_t resyncs, uint32_t overflows,
                                   uint32_t dropped)
{
	if(resyncs != 0)
	{
		printf("  Resyncs are the stream losing its framing and the driver\n");
		printf("  finding it again. A byte was lost or invented below the\n");
		printf("  driver -- a missed interrupt, a controller dropping one --\n");
		printf("  and until the boundary is back the pointer flies off. A\n");
		printf("  pointer that jumps while this number stands still is a\n");
		printf("  different fault, and it is above this line, not below it.\n");
	}

	if(overflows != 0)
	{
		printf("  Overflows are movements the device could not fit into one\n");
		printf("  event. How far the hand went is not recoverable, so the\n");
		printf("  pointer falls behind it -- fast movement, not a fault.\n");
	}

	if(dropped != 0)
	{
		printf("  Dropped events arrived faster than they were taken out of\n");
		printf("  the queue and the oldest were overwritten. The queue bridges\n");
		printf("  two turns of the reader and is not a history, so this costs\n");
		printf("  detail and never the position -- but a click can be lost\n");
		printf("  with it.\n");
	}
}

/* The four counters, in two rows of two. */
static void mouse_show_counters(void)
{
	mouse_info_label("Packets:");
	mem_print_right(mouse_packets(), 8);
	printf("   Resyncs:");
	mem_print_right(mouse_resyncs(), 9);
	printf("\n");

	mouse_info_label("Overflows:");
	mem_print_right(mouse_overflows(), 8);
	printf("   Dropped:");
	mem_print_right(mouse_dropped(), 9);
	printf("\n");
}

/* "mouse -i": everything the live view shows, once, and left on the screen.
*  The live view has to be ended before it can be read, and the state it was
*  in is exactly what one wants to look at afterwards. */
static void mouse_show_status(void)
{
	char text[4];
	int buttons;
	int cols;
	int rows;

	printf("Pointer:\n");

	if(!mouse_present())
	{
		mouse_explain_absent();
		return;
	}

	mouse_apply_field();
	buttons = mouse_buttons();
	mouse_button_text(buttons, text);

	mouse_info_label("Device:");
	printf("%s\n", mouse_describe());

	mouse_info_label("Field:");
	printf("%i x %i pixels, %s\n", mouse_field_w, mouse_field_h,
	       mouse_field_text());

	mouse_info_label("Position:");
	printf("(%i, %i)", mouse_x(), mouse_y());

	/* Which character cell the pointer is over, and only while the field is
	   the console's: on a field somebody typed the division would produce a
	   cell that has nothing to do with the screen, and a number that means
	   nothing is worse than no number. */
	if(mouse_field_source == MOUSE_FIELD_CONSOLE
	   && mouse_field_w > 0 && mouse_field_h > 0)
	{
		cols = fbcon_active() ? fbcon_cols() : MOUSE_TEXT_COLS;
		rows = fbcon_active() ? fbcon_rows() : MOUSE_TEXT_ROWS;

		printf(", over cell (%i, %i) of %i x %i",
		       (mouse_x() * cols) / mouse_field_w,
		       (mouse_y() * rows) / mouse_field_h, cols, rows);
	}

	printf("\n");

	mouse_info_label("Buttons:");
	if(buttons == 0)
	{
		printf("none down\n");
	} else {
		printf("%s down\n", text);
	}

	mouse_show_counters();

	/* Totals since boot. A counter that has been standing at 3 since the
	   machine started says something quite different from one that moved
	   while somebody watched, which is what the live view reports instead. */
	mouse_explain_counters(mouse_resyncs(), mouse_overflows(),
	                       mouse_dropped());

	if(mouse_packets() == 0)
	{
		printf("  No packet has arrived yet. The device answered when it was\n");
		printf("  looked for, so this is a mouse nobody has touched -- move it\n");
		printf("  and run \"mouse\" to watch the numbers move.\n");
	}
}

/* One line for each button that changed with this event, and the two running
*  totals the summary prints. A press and the release after it are two lines,
*  because they are two events and the whole reason the queue exists is that
*  the state between them can be gone before anyone looks. */
static void mouse_print_event(const mouse_event *ev, int *presses,
                              int *releases)
{
	static const int masks[3] =
	{
		MOUSE_BUTTON_LEFT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_RIGHT
	};
	int changed;
	int down;
	int i;

	changed = (int)ev->changed;

	for(i = 0; i < 3; i++)
	{
		if((changed & masks[i]) == 0) continue;

		down = ((int)ev->buttons & masks[i]) != 0;
		if(down) (*presses)++; else (*releases)++;

		printf("  ");
		mem_print_right(ev->time_ms, 9);
		printf(" ms  ");
		ps_print_left(down ? "press" : "release", 9);
		ps_print_left(mouse_button_name(masks[i]), 8);
		mouse_print_pair((int)ev->x, (int)ev->y, 0);
		printf("\n");
	}

	/* A bit outside the three mouse.h names. Nothing here can say what it
	   is, so it is printed as the number it is rather than swallowed. */
	changed = changed & ~(MOUSE_BUTTON_LEFT | MOUSE_BUTTON_MIDDLE
	                      | MOUSE_BUTTON_RIGHT);
	if(changed != 0)
	{
		printf("  ");
		mem_print_right(ev->time_ms, 9);
		printf(" ms  changed  button mask 0x%X, which is not one of the\n",
		       changed);
		printf("               three mouse.h names\n");
	}
}

/* One turn of the live loop: block until the driver has something new, or
*  until the keyboard is due to be looked at again.
*
*  This is the mechanism the command was written to make visible, so it is
*  written exactly the way system.h asks and for the reasons it gives there.
*
*  THE CONDITION IS mouse_packets(), because the loop cannot see inside the
*  queue with interrupts off without emptying it. The caller reads the counter
*  BEFORE its drain and passes it in; if it has moved by the time interrupts
*  are off here, a packet arrived at some point during the turn and the loop
*  must go round again rather than wait for a wake that may already have been
*  spent. Reading it before the drain rather than after costs one wasted turn
*  per burst -- the packet it saw may well be one the drain already took --
*  and buys the guarantee that this never blocks with an event sitting in the
*  queue. The counter is written only from the interrupt and tested here with
*  interrupts off, so there is no window left between the test and the block,
*  which is what closes the lost wakeup race.
*
*  It cannot spin: a turn that skips the wait has a packet to show for it, and
*  a device nobody is touching produces none.
*
*  WHY THERE IS A TIMEOUT although the mouse wakes this task. The view ends on
*  a keypress, and a key does not wake the mouse's channel -- kb.c wakes its
*  own, which is not exported, and a task waits on one channel anyway. So the
*  keyboard is looked at once a turn and this bound is the longest a keystroke
*  can sit unnoticed. 200 ms is what getch() gives its own wait for a related
*  reason: five wakeups a second next to the thousand ticks the timer produces
*  in the same second is nothing, and it is fast enough that nobody can feel
*  the key.
*
*  NO TASK TO BLOCK means task_wait() would report a timeout at once and the
*  loop would spin, so that case keeps a sleep. It should not happen -- the
*  shell is a task -- and one comparison makes sure of it. */
static void mouse_wait_turn(uint32_t seen)
{
	unsigned long flags;

	if(taskmgr_get_currpid() < 0)
	{
		sleep(MOUSE_WAIT_MS);
		return;
	}

	/* net_irq_save() and its partner are this file's pair; the name records
	   where they were first needed and not who may use them. A second
	   identical pair of inline assembly in the same file would be worse than
	   the name. */
	flags = net_irq_save();

	if(mouse_packets() == seen)
	{
		task_wait(mouse_wait_channel(), MOUSE_WAIT_MS);
	}

	net_irq_restore(flags);
}

/* "mouse": the live view, until a key is pressed. */
static void mouse_live(void)
{
	mouse_event ev;
	uint32_t seen;
	uint32_t packets;
	uint32_t resyncs;
	uint32_t overflows;
	uint32_t dropped;
	int started;
	int elapsed;
	int events;
	int presses;
	int releases;
	int redraw;
	int taken;
	int x;
	int y;
	int dx;
	int dy;
	unsigned char key;

	if(!mouse_present())
	{
		printf("Pointer:\n");
		mouse_explain_absent();
		return;
	}

	mouse_apply_field();

	/* A key already in the slot would end the view before it had been looked
	   at -- the same guard "gfx" puts in front of its picture. kb_flush()
	   clears the special key, getchn() the ordinary one. */
	kb_flush();
	getchn();

	/* And events already in the queue are things that happened before this
	   command was typed. Printing them as if they had just happened would be
	   the one lie this display must not tell, so they are thrown away here --
	   which is also the state the drain below expects to start from.

	   Bounded, like every drain in this command: the queue holds
	   MOUSE_QUEUE_SIZE events and no more, so a read that never empties is a
	   driver fault rather than a busy mouse, and it must not take the shell
	   with it. Finding exactly that kind of fault is what this command is
	   for. */
	taken = 0;
	while(taken <= MOUSE_QUEUE_SIZE && mouse_poll(&ev))
	{
		taken++;
	}

	packets = mouse_packets();
	resyncs = mouse_resyncs();
	overflows = mouse_overflows();
	dropped = mouse_dropped();
	started = timer_get_ticks();

	events = 0;
	presses = 0;
	releases = 0;

	x = mouse_x();
	y = mouse_y();
	dx = 0;
	dy = 0;

	printf("Pointer: %s\n", mouse_describe());
	printf("  Field:   %i x %i pixels, %s\n", mouse_field_w, mouse_field_h,
	       mouse_field_text());
	printf("  Moving updates the row, a press or a release prints a line of\n");
	printf("  its own. Press any key to return.\n\n");

	mouse_row_header();
	mouse_row(x, y, dx, dy, mouse_buttons());

	key = getchn();
	while(key == EOS)
	{
		/* Before the drain, never after it -- see mouse_wait_turn(). */
		seen = mouse_packets();
		redraw = 0;

		taken = 0;
		while(taken <= MOUSE_QUEUE_SIZE && mouse_poll(&ev))
		{
			taken++;
			events++;
			redraw = 1;

			x = (int)ev.x;
			y = (int)ev.y;
			dx = (int)ev.dx;
			dy = (int)ev.dy;

			if(ev.changed != 0)
			{
				mouse_clear_row();
				mouse_print_event(&ev, &presses, &releases);
			}
		}

		/* The row carries the last event's movement rather than a sum: it is
		   redrawn immediately after the drain, so "what just happened" is the
		   event that just happened. The buttons come from mouse_buttons()
		   because that column is the state, and the state is what that call
		   is for. */
		if(redraw || mouse_packets() != seen)
		{
			mouse_row(x, y, dx, dy, mouse_buttons());
		}

		mouse_wait_turn(seen);
		key = getchn();
	}

	printf("\n\n");

	packets = mouse_packets() - packets;
	resyncs = mouse_resyncs() - resyncs;
	overflows = mouse_overflows() - overflows;
	dropped = mouse_dropped() - dropped;

	elapsed = (timer_get_ticks() - started) / 1000;

	printf("  While that ran: %u packets in %i s, ", (int)packets, elapsed);
	printf("%i events, %i presses,\n", events, presses);
	printf("  %i releases. Resyncs %u, overflows %u, dropped %u.\n",
	       releases, (int)resyncs, (int)overflows, (int)dropped);

	mouse_explain_counters(resyncs, overflows, dropped);
}

/* "mouse -b [WIDTH HEIGHT]": the field, by hand.
*
*  Worth a form of its own for one reason: the clamping is the half of the
*  interface that cannot be seen on a screen the pointer already fits on. A
*  small field walked into with the mouse stops at a number one can read,
*  which is the only demonstration available that the driver clamps at all --
*  and setting a large one back is how the pointer is freed again. */
static void mouse_set_field(char *cmd)
{
	int w;
	int h;

	/* "mouse -b" with nothing after it goes back to the console, which is the
	   way out of a field small enough to be inconvenient. */
	if(prmc(cmd) == 1)
	{
		mouse_field_source = MOUSE_FIELD_NONE;
		mouse_apply_field();

		printf("Field back to the console: %i x %i pixels.\n",
		       mouse_field_w, mouse_field_h);
		printf("  Pointer at (%i, %i).\n", mouse_x(), mouse_y());
		return;
	}

	if(prmc(cmd) != 3)
	{
		printf("Syntax: mouse -b [WIDTH HEIGHT]\n");
		printf("\t          Both numbers, or neither to go back to the "
		       "console\n");
		return;
	}

	/* prmv() hands back one static buffer, so the two are converted one at a
	   time rather than both being held at once. */
	w = atoi(prmv(2, cmd));
	h = atoi(prmv(3, cmd));

	/* atoi() answers -1 for anything that is not all digits. */
	if(w < 1 || h < 1 || w > MOUSE_FIELD_MAX || h > MOUSE_FIELD_MAX)
	{
		printf("Both have to be whole numbers from 1 to %i.\n",
		       MOUSE_FIELD_MAX);
		return;
	}

	mouse_set_bounds(w, h);

	mouse_field_w = w;
	mouse_field_h = h;
	mouse_field_source = MOUSE_FIELD_TYPED;

	printf("Field set to %i x %i pixels.\n", w, h);

	/* Where the pointer ended up, because setting bounds under it moves it:
	   mouse.h clamps the position into the new field, so a field smaller than
	   the old position is the one case where this command moves the pointer
	   without anybody touching the mouse. */
	printf("  Pointer at (%i, %i).\n", mouse_x(), mouse_y());
	printf("  \"mouse\" now stops at the edges of that field, \"mouse -b\"\n");
	printf("  alone puts the console's field back.\n");
}

/* Records the result of a check, like gfx_check() does for the surface. */
static void mouse_check(int ok)
{
	mouse_tests_run++;

	if(ok)
	{
		mouse_tests_ok++;
		printf("  [  OK  ] ");
	} else {
		printf("  [FAILED] ");
	}
}

/* --- mouse -t ------------------------------------------------------------
*
*  WHAT CAN BE ASSERTED ABOUT A DEVICE DRIVEN BY A HAND, and nothing else.
*  Not that a packet ever arrives, not that a movement to the right raises x,
*  not that a button reports down, not that the counters stay at zero: every
*  one of those needs somebody touching the mouse while the test runs, and a
*  check that fails because nobody did is a check that teaches its reader to
*  ignore it. The command itself is where those are looked at, by eye, which
*  is the only instrument there is for them.
*
*  What is left is the half of mouse.h that is bookkeeping, and it is worth
*  testing precisely because it is the half a driver gets wrong quietly. A
*  clamp that is off by one puts the pointer one pixel outside the surface,
*  where a GUI writes past the end of a row; bounds that do not pull the
*  position in leave it outside a field that just shrank; a queue whose read
*  never empties hangs whoever drains it. None of that shows on the screen
*  until something else is built on top.
*
*  IT RUNS WITH INTERRUPTS OFF, which is what makes it a test rather than a
*  race. Every check below is a call and its answer, and between the two the
*  device's interrupt could otherwise move the pointer and fail an assertion
*  that is perfectly true -- while a hand rests on the mouse, which is the
*  normal state of a machine somebody is typing at. Nothing here waits, so
*  the whole sequence is a few dozen instructions with the interrupt held
*  off, and the drain is bounded by the queue's own size.
*
*  IT PUTS BACK WHAT IT MOVED. The field and the position are read first and
*  written back last: a self-test that leaves the pointer in the corner of a
*  320 by 200 field has broken the thing it was asked to check. */
static void mouse_selftest(void)
{
	mouse_event ev;
	unsigned long flags;
	const char *name;
	int save_w;
	int save_h;
	int save_x;
	int save_y;
	int at_x;
	int at_y;
	int far_x;
	int far_y;
	int low_x;
	int low_y;
	int shrunk_x;
	int shrunk_y;
	int drained;
	int empty;

	mouse_tests_run = 0;
	mouse_tests_ok = 0;

	printf("Pointer self-test:\n");

	if(!mouse_present())
	{
		mouse_explain_absent();
		printf("  There is nothing here that can be tested without one: every\n");
		printf("  check below is about a position the driver keeps for a\n");
		printf("  device, and there is no device.\n");
		return;
	}

	mouse_apply_field();

	save_w = mouse_field_w;
	save_h = mouse_field_h;
	save_x = mouse_x();
	save_y = mouse_y();

	name = mouse_describe();

	flags = net_irq_save();

	mouse_set_bounds(MOUSE_TEST_W, MOUSE_TEST_H);

	/* A position inside the field, taken as given. */
	mouse_set_position(MOUSE_TEST_X, MOUSE_TEST_Y);
	at_x = mouse_x();
	at_y = mouse_y();

	/* Well past the far corner. */
	mouse_set_position(MOUSE_TEST_W + MOUSE_TEST_OVER,
	                   MOUSE_TEST_H + MOUSE_TEST_OVER);
	far_x = mouse_x();
	far_y = mouse_y();

	/* And past the near one, which is the direction a signed coordinate gets
	   wrong differently: an unsigned clamp lets a negative through as a very
	   large number and it lands at the far edge instead of the origin. */
	mouse_set_position(-MOUSE_TEST_OVER, -MOUSE_TEST_OVER);
	low_x = mouse_x();
	low_y = mouse_y();

	/* The field shrinking under a pointer that is already outside where it
	   will end. */
	mouse_set_position(MOUSE_TEST_W - 1, MOUSE_TEST_H - 1);
	mouse_set_bounds(MOUSE_SMALL_W, MOUSE_SMALL_H);
	shrunk_x = mouse_x();
	shrunk_y = mouse_y();

	/* The queue, emptied and then read once more. Bounded by its own size
	   plus one, so a read that never empties ends the loop instead of
	   hanging the machine with interrupts off. */
	drained = 0;
	while(drained <= MOUSE_QUEUE_SIZE && mouse_poll(&ev))
	{
		drained++;
	}
	empty = mouse_poll(&ev);

	mouse_set_bounds(save_w, save_h);
	mouse_set_position(save_x, save_y);

	net_irq_restore(flags);

	printf("  Field %i x %i for the duration, then %i x %i back.\n",
	       MOUSE_TEST_W, MOUSE_TEST_H, save_w, save_h);

	/* 1. The device says what it is. The string is the driver's own and is
	      printed unread everywhere else, so the one thing worth checking is
	      that there is one: a null here is a null pointer handed to puts(). */
	mouse_check(name != 0 && name[0] != EOS);
	printf("mouse_describe() = \"%s\"\n", (name != 0) ? name : "");

	/* 2. A position inside the field is the position. */
	mouse_check(at_x == MOUSE_TEST_X && at_y == MOUSE_TEST_Y);
	printf("Set (%i,%i) inside the field, read (%i,%i)\n",
	       MOUSE_TEST_X, MOUSE_TEST_Y, at_x, at_y);

	/* 3. Beyond the far corner the pointer stops at the last pixel that is
	      inside. Not merely somewhere inside: a pointer pushed right has to
	      stop at the right edge, and one that jumped back to the middle
	      would be a pointer the user cannot park anywhere. */
	mouse_check(far_x == MOUSE_TEST_W - 1 && far_y == MOUSE_TEST_H - 1);
	printf("Set (%i,%i) past the corner, read (%i,%i), edge is (%i,%i)\n",
	       MOUSE_TEST_W + MOUSE_TEST_OVER, MOUSE_TEST_H + MOUSE_TEST_OVER,
	       far_x, far_y, MOUSE_TEST_W - 1, MOUSE_TEST_H - 1);

	/* 4. And below the origin it stops at the origin. */
	mouse_check(low_x == 0 && low_y == 0);
	printf("Set (%i,%i) below the origin, read (%i,%i)\n",
	       -MOUSE_TEST_OVER, -MOUSE_TEST_OVER, low_x, low_y);

	/* 5. A field that shrinks takes the pointer with it. Whoever owns the
	      surface calls this when the surface changes, and a position left
	      outside is a cursor drawn off the end of a row. */
	mouse_check(shrunk_x >= 0 && shrunk_x < MOUSE_SMALL_W
	            && shrunk_y >= 0 && shrunk_y < MOUSE_SMALL_H);
	printf("At (%i,%i), field cut to %i x %i, pointer now (%i,%i)\n",
	       MOUSE_TEST_W - 1, MOUSE_TEST_H - 1, MOUSE_SMALL_W, MOUSE_SMALL_H,
	       shrunk_x, shrunk_y);

	/* 6. An emptied queue reads empty, and the read that found it empty came
	      back. Both halves ran with interrupts off, so a mouse_poll() that
	      waited for an event would have stopped this machine rather than
	      returned a wrong answer -- which is the strongest form the "never
	      blocks" half of the contract can be tested in. */
	mouse_check(empty == 0 && drained <= MOUSE_QUEUE_SIZE);
	printf("Drained %i event(s) of at most %i, next read returned %i\n",
	       drained, MOUSE_QUEUE_SIZE, empty);

	printf("  Result: %i of %i checks passed\n",
	       mouse_tests_ok, mouse_tests_run);

	printf("  Nothing above needed the mouse to move, and nothing above says\n");
	printf("  the device works: that a packet arrives, that x rises to the\n");
	printf("  right, that a button reports down are all facts about a hand on\n");
	printf("  the mouse. \"mouse\" is where they are looked at.\n");
}

/* "mouse": what the pointer is doing, live.
*
*  Four forms and one of them is the point; the other three are the questions
*  that come up while looking at it. "-i" is the same numbers left standing on
*  the screen after the view has been ended, "-b" is the field they are
*  measured in, and "-t" is the part of the contract a machine can check by
*  itself. */
void mousepointer(char *cmd)
{
	char opt[100];

	if(prmc(cmd) == 0)
	{
		mouse_live();
		return;
	}

	/* prmv() hands back a pointer into one static buffer, so the option is
	   copied out before anything else can call prmv() again. */
	strcpy(opt, prmv(1, cmd));

	if(strcmp(opt, "-i") == 0)
	{
		mouse_show_status();
	}
	else if(strcmp(opt, "-t") == 0)
	{
		mouse_selftest();
	}
	else if(strcmp(opt, "-b") == 0)
	{
		mouse_set_field(cmd);
	} else {
		printf("Syntax: mouse [-i] [-t] [-b WIDTH HEIGHT]\n");
		printf("\t          Watch the pointer live, any key returns\n");
		printf("\t-i        Show the device, the position and the counters\n");
		printf("\t-t        Run the checks that need nobody to touch it\n");
		printf("\t-b W H    Set the field the pointer moves in, -b alone\n");
		printf("\t          puts the console's field back\n");
	}
}

/* --- lspci, ifconfig, arp and ping ---------------------------------------
*
*  Four commands and one thing they have in common: on a machine that booted
*  without a network device -- which is what "make run" gives you -- every one
*  of them has to say why there is nothing rather than print a row of zeroes.
*  net_explain_down() is that answer, in one place, and the three states it
*  tells apart are no card on the bus, a card the stack was never brought up
*  on, and a stack that has no address yet.
*
*  These are the shell's commands, not the stack's: everything below asks
*  net.h, pci.h and rtl8139.h questions and formats the answers. No packet is
*  built here.
*
*  The formatting helpers exist because printf() has neither field widths nor
*  a way to print an address or a MAC. A value that has to line up in a column
*  is written into a small buffer first and then padded by ps_print_left() or
*  fs_print_right_text(), the same two routines the "ps" and "df" tables use.
*/

/* One decimal byte, 0..255, without leading zeroes. Returns the position
*  after the digits it wrote so the callers can chain. */
static char *net_put_byte(char *p, uint32_t value)
{
	if(value >= 100)
	{
		*p = (char)('0' + (value / 100));
		p++;
	}

	if(value >= 10)
	{
		*p = (char)('0' + ((value / 10) % 10));
		p++;
	}

	*p = (char)('0' + (value % 10));
	p++;
	return p;
}

/* Writes a host order address as a dotted quad. text holds NET_IP_TEXT
*  bytes. This and net_parse_ip() are the two directions of the same format
*  and are kept next to each other for that reason -- there is no inet_aton()
*  here to borrow, and this printf() cannot print an address either. */
static void net_ip_text(uint32_t ip, char *text)
{
	char *p;
	int i;

	p = text;
	for(i = 3; i >= 0; i--)
	{
		p = net_put_byte(p, (ip >> (i * 8)) & 0xFF);
		if(i > 0)
		{
			*p = '.';
			p++;
		}
	}

	*p = EOS;
}

/* Writes a MAC as six colon separated hex pairs. text holds NET_MAC_TEXT
*  bytes. A null pointer -- which is what net_mac() hands back when there is
*  no card -- is spelled out rather than dereferenced. */
static void net_mac_text(const uint8_t *mac, char *text)
{
	const char *hexdigit = "0123456789ABCDEF";
	char *p;
	int i;

	if(mac == 0)
	{
		strcpy(text, "(none)");
		return;
	}

	p = text;
	for(i = 0; i < ETH_ALEN; i++)
	{
		*p = hexdigit[(mac[i] >> 4) & 0xF];
		p++;
		*p = hexdigit[mac[i] & 0xF];
		p++;
		if(i < ETH_ALEN - 1)
		{
			*p = ':';
			p++;
		}
	}

	*p = EOS;
}

/* Writes value as exactly the given number of upper case hex digits, with
*  leading zeroes and no "0x" in front -- a vendor id is written 10EC and
*  8139, never 0x10EC and 0x8139, so that the two columns line up under each
*  other. page_print_hex() prints the other form, for addresses.
*  text holds NET_HEX_TEXT bytes and digits is at most 8. */
static void net_hex_text(uint32_t value, int digits, char *text)
{
	const char *hexdigit = "0123456789ABCDEF";
	int i;

	for(i = 0; i < digits; i++)
	{
		text[i] = hexdigit[(value >> ((digits - 1 - i) * 4)) & 0xF];
	}

	text[digits] = EOS;
}

static void net_print_ip(uint32_t ip)
{
	char text[NET_IP_TEXT];

	net_ip_text(ip, text);
	printf("%s", text);
}

/* Reads a dotted quad into a host order address. Returns 1 when the whole
*  string was one, 0 otherwise -- deliberately strict, because a typed
*  address that is silently taken to mean something else is worse than a
*  refusal: exactly four parts, one to three digits each, every part 0..255,
*  and nothing at all after the last one. */
static int net_parse_ip(const char *text, uint32_t *out)
{
	uint32_t address;
	int part;
	int value;
	int digits;
	int i;

	address = 0;
	i = 0;

	for(part = 0; part < 4; part++)
	{
		value = 0;
		digits = 0;

		while(text[i] >= '0' && text[i] <= '9')
		{
			value = value * 10 + (text[i] - '0');
			if(value > 255) return 0;
			digits++;
			i++;
		}

		if(digits == 0 || digits > 3) return 0;

		address = (address << 8) | (uint32_t)value;

		if(part < 3)
		{
			if(text[i] != '.') return 0;
			i++;
		}
	}

	if(text[i] != EOS) return 0;

	*out = address;
	return 1;
}

/* The label column of "ifconfig", padded the way gfx_info_label() pads the
*  one of "gfx -i". */
static void net_info_label(const char *label)
{
	int used;

	printf("  %s", label);

	used = 2 + (int)strlen(label);
	while(used < NET_INFO_LABEL)
	{
		putch(' ');
		used++;
	}
}

/* Why there is no network. The one place that knows, so that all four
*  commands give the same account of the same machine.
*
*  Having no network device is a normal state here, not a failure -- "make
*  run" boots without one -- so the reasons that lead to it are told apart.
*  A bus that answered with nothing at all is a different situation from a
*  bus full of devices none of which is a network card, and only the first
*  points at the kernel rather than at the machine. */
static void net_explain_down(void)
{
	if(!rtl8139_present())
	{
		if(pci_count() == 0)
		{
			printf("  The PCI bus was not enumerated at all, so no card was ever\n");
			printf("  looked for. Every PC answers with at least a host bridge,\n");
			printf("  so an empty bus means the enumeration has not run.\n");
		} else {
			printf("  %i PCI device(s) answered, none of them an RTL8139.\n",
			       pci_count());
			printf("  Booting without a network device is a normal case here --\n");
			printf("  \"make run\" does exactly that. \"lspci\" shows what the bus\n");
			printf("  did answer with.\n");
		}
		return;
	}

	if(!net_up())
	{
		printf("  The card was found, but the stack was not brought up on it,\n");
		printf("  so nothing can be sent or received.\n");
		return;
	}

	if(net_ip() == 0)
	{
		/* Two ways to get one, and the first is now the one to reach for:
		   "dhcp" asks the network, and the network is the only thing that
		   actually knows the answer. The typed form stays in the message
		   because it still works where nothing answers. */
		printf("  The interface is up but has no address yet. \"dhcp\" asks the\n");
		printf("  network for one, which is what QEMU's user network answers.\n");
		printf("  \"ifconfig 10.0.2.15 255.255.255.0 10.0.2.2\" sets the same\n");
		printf("  three by hand where nothing answers.\n");
	}
}

/* Guard in front of a command that needs to put a packet on the wire.
*  Returns 1 when it may go ahead. */
static int net_interface_ready(const char *what, int need_address)
{
	if(!rtl8139_present())
	{
		printf("%s: no network card is present.\n", what);
		net_explain_down();
		return 0;
	}

	if(!net_up())
	{
		printf("%s: the network stack is not up.\n", what);
		net_explain_down();
		return 0;
	}

	if(need_address && net_ip() == 0)
	{
		printf("%s: no address is configured.\n", what);
		net_explain_down();
		return 0;
	}

	return 1;
}

/* --- lspci --------------------------------------------------------------- */

/* What a class code means, in the words one would use out loud. The subclass
*  is only consulted where it changes the answer usefully -- a bridge is worth
*  distinguishing from a host bridge, an IDE controller from a disk controller
*  in general. Everything unrecognised keeps its number and loses its name. */
static const char *net_class_name(uint8_t class_code, uint8_t subclass)
{
	switch(class_code)
	{
		case PCI_CLASS_STORAGE:
			if(subclass == 0x01) return "IDE controller";
			if(subclass == 0x06) return "SATA controller";
			return "Mass storage";
		case PCI_CLASS_NETWORK:
			if(subclass == 0x00) return "Ethernet controller";
			return "Network controller";
		case PCI_CLASS_DISPLAY:
			if(subclass == 0x00) return "VGA controller";
			return "Display controller";
		case PCI_CLASS_MULTIMEDIA:
			return "Multimedia device";
		case PCI_CLASS_MEMORY:
			return "Memory controller";
		case PCI_CLASS_BRIDGE:
			if(subclass == 0x00) return "Host bridge";
			if(subclass == 0x01) return "ISA bridge";
			if(subclass == 0x04) return "PCI-to-PCI bridge";
			return "Bridge";
		case PCI_CLASS_SERIAL:
			if(subclass == 0x03) return "USB controller";
			return "Serial bus controller";
		default:
			break;
	}

	return "Device";
}

/* The first of the six base address registers that describes an I/O region,
*  or 0 when the device is memory mapped only. That port number is what a
*  driver needs and therefore what this table is really for. */
static uint16_t net_pci_io_base(const pci_device *dev)
{
	uint16_t base;
	int bar;

	for(bar = 0; bar < 6; bar++)
	{
		base = pci_io_base(dev, bar);
		if(base != 0) return base;
	}

	return 0;
}

/* "lspci": one row per device, and underneath it the answer to the question
*  this command is normally run to ask -- is the network card there.
*
*  The columns are the ones a driver needs: where the device sits, what it
*  says it is, which interrupt line it was given, and the I/O base it answers
*  on. Everything is built into a small text buffer first and padded into the
*  column afterwards, because printf() has no field widths. */
static void net_show_pci(void)
{
	const pci_device *dev;
	const pci_device *card;
	char text[NET_HEX_TEXT];
	char class_text[NET_CLASS_TEXT];
	char io_text[NET_HEX_TEXT + 2];
	uint16_t io;
	int count;
	int network;
	int i;

	count = pci_count();

	printf("PCI devices:\n");

	if(count == 0)
	{
		printf("  None. The bus was not enumerated, or nothing answered on it.\n");
		printf("  Every PC has at least a host bridge, so an empty list here\n");
		printf("  almost always means the enumeration has not run rather than\n");
		printf("  that the machine is empty.\n");
		return;
	}

	printf("  ");
	fs_print_right_text("Bus", 3);
	fs_print_right_text("Slot", 6);
	fs_print_right_text("Fn", 4);
	fs_print_right_text("Vendor", 8);
	fs_print_right_text("Device", 8);
	fs_print_right_text("Class", 7);
	fs_print_right_text("IRQ", 5);
	fs_print_right_text("I/O base", 10);
	printf("  Description\n");

	network = 0;

	for(i = 0; i < count; i++)
	{
		dev = pci_get(i);
		if(dev == 0) continue;

		if(dev->class_code == PCI_CLASS_NETWORK) network++;

		printf("  ");
		net_hex_text((uint32_t)dev->bus, 2, text);
		fs_print_right_text(text, 3);
		net_hex_text((uint32_t)dev->slot, 2, text);
		fs_print_right_text(text, 6);
		mem_print_right((uint32_t)dev->func, 4);
		net_hex_text((uint32_t)dev->vendor, 4, text);
		fs_print_right_text(text, 8);
		net_hex_text((uint32_t)dev->device, 4, text);
		fs_print_right_text(text, 8);

		/* Class and subclass as one cell, "06:00", the way the pair is
		   always written -- they are only meaningful together. */
		net_hex_text((uint32_t)dev->class_code, 2, class_text);
		class_text[2] = ':';
		net_hex_text((uint32_t)dev->subclass, 2, class_text + 3);
		fs_print_right_text(class_text, 7);

		mem_print_right((uint32_t)dev->irq, 5);

		io = net_pci_io_base(dev);
		if(io == 0)
		{
			/* Not a failure: a device can be memory mapped only. */
			fs_print_right_text("-", 10);
		} else {
			io_text[0] = '0';
			io_text[1] = 'x';
			net_hex_text((uint32_t)io, 4, io_text + 2);
			fs_print_right_text(io_text, 10);
		}

		printf("  %s\n", net_class_name(dev->class_code, dev->subclass));
	}

	printf("  %i device(s) found, %i of them a network controller.\n",
	       count, network);

	/* And the line this command is usually run for. */
	card = pci_find(NET_RTL_VENDOR, NET_RTL_DEVICE);
	if(card == 0)
	{
		printf("  No RTL8139 (10EC:8139) is on this bus, so the network stack\n");
		printf("  has no card to drive. That is the normal state of \"make run\"\n");
		printf("  without a network device.\n");
		return;
	}

	net_hex_text((uint32_t)card->bus, 2, text);
	printf("  RTL8139 at %s:", text);
	net_hex_text((uint32_t)card->slot, 2, text);
	printf("%s.%i, ", text, (int)card->func);
	io_text[0] = '0';
	io_text[1] = 'x';
	net_hex_text((uint32_t)net_pci_io_base(card), 4, io_text + 2);
	printf("I/O base %s, IRQ %i -- ", io_text, (int)card->irq);

	if(rtl8139_present())
	{
		printf("the driver is using it.\n");
	} else {
		printf("the driver did not take it.\n");
	}
}

/* --- ifconfig ------------------------------------------------------------ */

/* "ifconfig": the interface, its addresses and what has actually moved.
*
*  The counters are the honest part of this: net_rx_packets() counts what the
*  card's interrupt handed up, net_rx_dropped() what the stack threw away
*  again, and the two together say a great deal more about a network that is
*  not working than any status word does.
*
*  There are three of them on that row now. The overrun count is the one
*  number out of the receive queue that could not be left to "ifconfig -q" --
*  see net_show_queue() for why the rest of them are there and not here --
*  because it is the only counter on this screen that says the fault is in
*  this machine rather than out on the network. It costs no row: it is a
*  third column on a line that had two. */
static void net_show_interface(void)
{
	char text[NET_MAC_TEXT];
	int configured;

	configured = (net_ip() != 0);

	printf("Interface eth0:\n");

	net_info_label("Status:");
	if(!rtl8139_present())
	{
		printf("down -- no network card was found\n");
	}
	else if(!net_up())
	{
		printf("down -- card present, stack not up\n");
	}
	else if(!configured)
	{
		printf("up -- no address configured\n");
	} else {
		printf("up\n");
	}

	net_info_label("Card:");
	if(rtl8139_present())
	{
		printf("%s\n", rtl8139_info());
	} else {
		printf("none on the PCI bus\n");
	}

	/* Asked of the card rather than of net_mac(): with no card the stack
	   holds an all zero address, and six zero bytes printed as a MAC look
	   like an answer instead of the absence of one. */
	net_mac_text(rtl8139_present() ? net_mac() : 0, text);
	net_info_label("Hardware:");
	printf("%s\n", text);

	net_info_label("Address:");
	net_print_ip(net_ip());
	if(!configured) printf("  (not configured)");
	printf("\n");

	net_info_label("Netmask:");
	net_print_ip(net_netmask());
	printf("\n");

	net_info_label("Gateway:");
	net_print_ip(net_gateway());
	printf("\n");

	/* Where the three above came from. Worth a line of its own because the
	   two origins fail differently: numbers typed in stay wrong until they
	   are typed again, a lease stops being true on its own when it runs
	   out, and only the second sort has anything more to show. */
	net_info_label("Source:");
	if(!configured)
	{
		printf("none -- no address is set\n");
	}
	else if(net_address_from_lease)
	{
		printf("DHCP lease from ");
		net_print_ip(dhcp_server());
		if(dhcp_state() != DHCP_STATE_BOUND)
		{
			/* The lease is still configured and still works -- dhcp_stop()
			   does not undo it -- but a later exchange has moved the client
			   off it, so the two do not agree any more. */
			printf(", client now %s", dhcp_state_name());
		}
		printf("\n");

		net_dhcp_leased_ip("DNS server:", dhcp_dns());

		net_info_label("Lease:");
		net_duration(dhcp_lease_seconds());
		if(dhcp_lease_seconds() != 0
		   && dhcp_lease_seconds() != (uint32_t)DHCP_LEASE_FOREVER)
		{
			printf(", ");
			net_duration(net_dhcp_remaining());
			printf(" left");
		}
		printf("\n");
	} else {
		printf("set by hand with ifconfig\n");
	}

	printf("  RX packets: ");
	mem_print_right(net_rx_packets(), 8);
	printf("   dropped: ");
	mem_print_right(net_rx_dropped(), 8);
	printf("   overrun: ");
	mem_print_right(net_rx_overrun(), 8);
	printf("\n");

	printf("  TX packets: ");
	mem_print_right(net_tx_packets(), 8);
	printf("\n");

	/* The UDP counters belong here, one layer up though they are: this
	   command is where one looks when the network does not work, and since
	   DHCP the interesting failure is a datagram that the card counted and
	   UDP then dropped -- a wrong checksum, a port nobody bound. Two rows in
	   the same columns as the two above show that difference at a glance,
	   and they are cheap: four lines that are read together. */
	printf("  UDP     rx: ");
	mem_print_right(udp_rx_packets(), 8);
	printf("   dropped: ");
	mem_print_right(udp_rx_dropped(), 8);
	printf("\n");

	printf("  UDP     tx: ");
	mem_print_right(udp_tx_packets(), 8);
	printf("\n");

	/* And no TCP rows under those, deliberately -- see the note above
	   net_show_tcp(). UDP is here because it has no command of its own;
	   TCP has "netstat", the DNS counters set the precedent for leaving a
	   command's counters with its command, and this display is already at
	   the height of the screen. */

	/* What the third column of the first row means, said only when it is not
	   zero -- the way "netstat" explains TIME_WAIT only while a row is
	   sitting in it. A zero there needs no words, and two rows of prose for
	   a zero is exactly what this display has no room for. Anything else is
	   worth the two rows, because two counters standing beside each other
	   are read as one kind of number and these are not: a drop is ordinary,
	   an overrun is the machine falling behind. The rest of the queue is one
	   command away and is named here, so that somebody who has just been
	   told the queue was full can go and see how full it gets. */
	if(net_rx_overrun() != 0)
	{
		printf("  Overrun is frames the card's interrupt had nowhere to put,\n");
		printf("  the receive queue being full. \"ifconfig -q\" shows the queue.\n");
	}

	/* A row of zeroes explains nothing by itself, so when the interface
	   cannot carry a packet it says why underneath. */
	if(!rtl8139_present() || !net_up() || !configured)
	{
		printf("\n");
		net_explain_down();
	}
}

/* --- the receive queue ---------------------------------------------------
*
*  Where these numbers go, and why they are not in the display above.
*
*  The queue belongs to the interface. It is eth0's backlog and nothing
*  else's, and there is no second thing it could be filed under -- so it
*  keeps the interface's command word instead of getting one of its own, the
*  same way "gfx -i" is a second display under "gfx" rather than a command
*  called "gfxinfo". A new word in "help" buys nothing when the word it would
*  be next to is already the right one.
*
*  But it does not belong in what "ifconfig" prints by itself. That is what
*  one reads to find out whether the network works, and these five numbers do
*  not answer that question: they are detail about HOW the receiving is done,
*  and on a machine that is working most of them are zero. The display is
*  already fourteen rows on a screen with twenty-five -- the DNS counters
*  were kept out of it for that reason, and TCP's went to "netstat" for the
*  same one -- and five more rows would push the interface itself off the top
*  of the screen to show them.
*
*  So the option gets the queue and the ordinary display gains no row at all.
*  It gains one COLUMN: the overrun count, beside the drop count that has
*  always been there, because that single number must not wait behind an
*  option that nobody types when they have no reason to suspect a queue
*  exists. It is also the only place where an overrun can be read for what it
*  is, which is not what the number next to it is.
*
*  What the two numbers here are for, since neither is obvious:
*
*  The peak is the only evidence that the drain keeps up with the wire. Used
*  is a sample -- by the time it is printed the drain has almost certainly
*  emptied the queue, so it reads zero on a machine under load and on a
*  machine doing nothing alike. The peak remembers the worst moment since
*  boot, and the distance between it and the capacity is the margin the
*  machine has left. A queue that never fills is working. One whose peak is
*  approaching capacity is about to start losing frames, and the whole value
*  of the number is that it says so BEFORE the first one is lost.
*
*  An overrun is a different failure from a drop, and printing them as one
*  number would hide the only one that means anything is wrong.
*  net_rx_dropped() counts frames the protocol layers rejected -- not for us,
*  failed a check -- which is ordinary and happens on any shared wire.
*  net_rx_overrun() counts frames the interrupt threw away because there was
*  no room, which means this machine could not keep up with its own card. */

/* "ifconfig -q": the queue between the card's interrupt and the task that
*  does the protocol work. */
static void net_show_queue(void)
{
	uint32_t capacity;
	uint32_t used;
	uint32_t peak;
	uint32_t percent;
	uint32_t overrun;

	capacity = net_queue_capacity();
	used = net_queue_used();
	peak = net_queue_peak();
	overrun = net_rx_overrun();

	/* A capacity of zero would only come from a queue that is not there, and
	   dividing by it is not the way to find that out. */
	percent = 0;
	if(capacity != 0) percent = (peak * (uint32_t)100) / capacity;

	printf("Receive queue:\n");

	/* The line that earns its space. Without it these are five numbers about
	   a thing the reader has no reason to know exists, and no amount of
	   labelling the numbers would tell them what it is for. */
	printf("  The card's interrupt copies each frame in here and returns; the\n");
	printf("  protocol work happens afterwards in a task. What is in the queue\n");
	printf("  is how far behind the wire that task is.\n");

	net_info_label("Capacity:");
	mem_print_right(capacity, 8);
	printf(" bytes, %i frames at the %i byte maximum\n",
	       (int)(capacity / (uint32_t)ETH_FRAME_MAX), ETH_FRAME_MAX);

	/* Almost always zero, and it is here anyway: it is the number that says
	   what "peak" is the peak OF, and a reader who sees only a high water
	   mark has no way to tell a queue from a counter. */
	net_info_label("In use:");
	mem_print_right(used, 8);
	printf(" bytes, %i frame(s) waiting now\n", net_queue_frames());

	net_info_label("Peak:");
	mem_print_right(peak, 8);
	printf(" bytes, %u percent of capacity\n", (int)percent);

	/* Which of the two things the peak means, said outright. Reading a high
	   water mark is exactly the skill somebody looking at this for the first
	   time does not have, and the number is worthless without it. */
	if(peak == 0)
	{
		printf("  Nothing has ever waited here: every frame was drained before\n");
		printf("  the next one arrived, which is a drain well ahead of the wire.\n");
	}
	else if(percent >= NET_QUEUE_HIGH)
	{
		printf("  The peak is most of the queue. The drain is only just keeping\n");
		printf("  up, and the next burst is the one that starts being lost.\n");
	} else {
		printf("  The drain has been keeping up: the wire never filled more of\n");
		printf("  the queue than that, and the rest of it is the margin left.\n");
	}

	net_info_label("Overrun:");
	mem_print_right(overrun, 8);
	printf(" frames, arrived with no room here\n");

	net_info_label("Dropped:");
	mem_print_right(net_rx_dropped(), 8);
	printf(" frames, rejected by the protocol layers\n");

	/* The two above are next to each other and are not the same kind of
	   number, so the difference is spelled out whenever the one that matters
	   is not zero. */
	if(overrun != 0)
	{
		printf("  An overrun is not a drop. Dropped is ordinary: a frame that\n");
		printf("  was not for us, or that failed a check. Overrun is this\n");
		printf("  machine failing to keep up with its own card.\n");
	}

	/* Numbers about a queue nothing can put anything into explain nothing by
	   themselves, the same as the row of zeroes in the display above. */
	if(!rtl8139_present() || !net_up())
	{
		printf("\n");
		net_explain_down();
	}
}

/* --- arp ----------------------------------------------------------------- */

/* "arp": which addresses have been resolved to which cards.
*
*  Worth looking at exactly when a ping does not work, because it separates
*  "the other end never answered who-has" from "it answered and the packets
*  are getting lost afterwards". */
static void net_show_arp(void)
{
	uint8_t mac[ETH_ALEN];
	char ip_text[NET_IP_TEXT];
	char mac_text[NET_MAC_TEXT];
	uint32_t ip;
	int entries;
	int shown;
	int i;

	entries = arp_cache_entries();

	printf("ARP cache:\n");

	if(entries <= 0)
	{
		printf("  Empty -- no address has been resolved yet.\n");

		if(!rtl8139_present() || !net_up())
		{
			net_explain_down();
		} else {
			printf("  An entry appears as soon as something is sent somewhere:\n");
			printf("  \"ping\" resolves its destination before the first echo\n");
			printf("  request can go out.\n");
		}
		return;
	}

	printf("  ");
	ps_print_left("Address", NET_IP_TEXT);
	printf("Hardware\n");

	/* arp_cache_get() answers 0 for an index it filled in and a negative
	   value for one it has nothing for -- 0 is the success, not the failure,
	   which is the opposite of what the eye expects here. The indices run
	   over the resolved entries in order, so the first refusal is the end of
	   the list; the loop is still bounded by the cache size rather than by
	   the count, so a cache that grows between the two calls cannot run it
	   off the end. */
	shown = 0;
	for(i = 0; i < ARP_CACHE_SIZE; i++)
	{
		if(arp_cache_get(i, &ip, mac) != 0) break;

		net_ip_text(ip, ip_text);
		net_mac_text(mac, mac_text);

		printf("  ");
		ps_print_left(ip_text, NET_IP_TEXT);
		printf("%s\n", mac_text);
		shown++;
	}

	printf("  %i of %i cache entries in use.\n", shown, ARP_CACHE_SIZE);
}

/* --- waiting for the network --------------------------------------------- */

/* Interrupts off, and back to what the caller had.
*
*  The wait in system.h only closes the lost wakeup race if the condition is
*  tested with interrupts already off, so every task_wait() below is wrapped in
*  these two. net.c, kb.c and syscall.c each have a pair and every one of them
*  is static to its file; there is no global one, and six lines of inline
*  assembly are cheaper than a new cross-file interface for them.
*
*  pushfl before cli, so a caller that already held interrupts off gets them
*  back off and one that did not gets them back on. */
static unsigned long net_irq_save(void)
{
	unsigned long flags;

	__asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
	return flags;
}

static void net_irq_restore(unsigned long flags)
{
	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

/* The end of one turn of the three loops below: block until the stack has
*  processed something, or until the turn's own interval is up.
*
*  This is what the sleep at the end of each of those loops became. Every one
*  of them is written the same way and has to stay that way:
*
*      net_queue_drain();           -- take what arrived out of the queue
*      seen = net_rx_packets();     -- what the card had taken in by now
*      test the condition           -- against what the drain just parsed
*      net_wait_turn(seen, ms)      -- and block until there is more
*
*  THE DRAIN COMES FIRST and the test comes second, which is the one thing the
*  old comments here got exactly right and the wait must not undo: the answer
*  arrives while this task is blocked, so draining after the test would leave
*  it in the queue for another whole interval. Nothing about waking changes
*  that -- a wake means "a frame has been processed", so the very first thing
*  to do on the way round is look at what came of it.
*
*  WHY THERE IS STILL A TIMEOUT. A wake tells a task that something arrived and
*  nothing else. It cannot tell it that dhcp_poll() is due to resend a DISCOVER
*  nobody answered, or that dns_poll() has a query to retransmit -- those fall
*  due precisely when nothing arrives, so nothing will ever wake them. The
*  bound is therefore the interval at which the loop's own poll runs, unchanged
*  from the sleep it replaces, and it is what keeps a silent network from
*  turning these loops into a wait with no retries in it. What the wake buys is
*  the other end: a reply that lands a millisecond after a look is acted on at
*  once instead of at the end of the interval. Polls happen at least as often
*  as before and usually more often, which is the direction that cannot break a
*  timer.
*
*  SEEN IS WHAT CLOSES THE LOST WAKEUP, and it is why this takes a count
*  rather than only an interval. The wake is sent by whoever drains the queue,
*  and that can be the dedicated drain task -- so it can arrive in the window
*  between this task's test and its block, find this task RUNNING rather than
*  BLOCKED, and wake nobody. The condition has then already come true and the
*  wait sits out its whole interval regardless. That is not a hang, but it is
*  not rare either: it is what happens every time a network answers faster
*  than this task can get from its drain to its block, which on this one is
*  most of the time. It was measured at a whole interval on the first lookup
*  after an ARP entry expires, where the reply comes back in fifty
*  MICROseconds.
*
*  net_rx_packets() closes it because the card's interrupt increments that
*  counter before anything is parsed. The caller reads it immediately after
*  its drain; if it has moved by the time interrupts are off here, a frame
*  came in during the turn and this task must look again rather than wait for
*  a wake that has already been spent. The test is made with interrupts off
*  and the counter is only ever written from the interrupt, so there is no
*  window left between the test and the block -- which is exactly what
*  system.h asks of every caller, with net_rx_packets() standing in for a
*  condition this function cannot see.
*
*  It cannot spin: a turn that skips the wait has a frame to show for it, and
*  the caller drains and re-tests before it comes back. What it costs on a
*  busy segment is one extra turn per frame that was not ours, which is two
*  cheap calls, and the wake would have cost that anyway.
*
*  A missed wake would still cost only one interval, because the timeout ends
*  the wait and the loop tests again. That is the backstop, not the mechanism.
*
*  NO TASK TO BLOCK means task_wait() reports a timeout at once and the loop
*  would spin, so that case keeps the sleep. It should not happen -- the shell
*  is a task -- and one comparison makes sure of it. */
static void net_wait_turn(uint32_t seen, int timeout_ms)
{
	unsigned long flags;

	if(timeout_ms <= 0) return;

	if(taskmgr_get_currpid() < 0)
	{
		sleep(timeout_ms);
		return;
	}

	flags = net_irq_save();

	if(net_rx_packets() == seen)
		task_wait(net_wait_channel(), timeout_ms);

	net_irq_restore(flags);
}

/* --- ping ---------------------------------------------------------------- */

/* Sequence numbers are handed out from here and never handed out twice for
*  as long as the machine runs. That is what makes a reply identifiable.
*
*  icmp_last_reply() is a mailbox: it reports the last echo reply that came
*  in, and it keeps reporting it. A per run counter starting at 1 would mean
*  the reply to the first request of the previous ping is sitting there
*  looking exactly like the reply to the first request of this one -- and a
*  reply that arrives late, after this ping has already written its request
*  off, would be counted for the next request instead. A number that only
*  ever goes up makes both impossible. */
static uint16_t net_next_sequence = 1;

/* Sends one echo request and waits for its reply. Returns the round trip
*  time in milliseconds, or NET_PING_UNRESOLVED when ARP never answered and
*  the request could not even be sent, or NET_PING_LOST when it went out and
*  nothing came back. The address the reply came from is stored through from.
*
*  Two things this has to get right, and neither of them is visible in the
*  return value of icmp_send_echo():
*
*  1. The first request to an address cannot go out. ip_send() needs a MAC,
*     the ARP cache is cold, so it puts a who-has on the wire and fails. The
*     answer arrives in an interrupt some milliseconds later. Retrying is not
*     a workaround, it is the resolution: NET_ARP_TRIES attempts NET_ARP_WAIT
*     milliseconds apart, one second in total, and it usually succeeds on the
*     second attempt.
*
*  2. The reply is not returned by anything. It is parsed while this task is
*     blocked and left in icmp_last_reply(), which is therefore read after
*     every wake -- and matched on both the identifier and the sequence number
*     that were sent, so that neither somebody else's ping nor an old reply of
*     our own is counted as an answer to this request. */
static int net_ping_once(uint32_t dst, uint16_t sequence, uint32_t *from)
{
	uint32_t reply_from;
	uint32_t seen;
	uint16_t reply_id;
	uint16_t reply_sequence;
	int start;
	int elapsed;
	int remaining;
	int tries;
	int rc;

	/* Set before anything can fail, and set again from the reply itself when
	*  one arrives -- so a reply that came from somewhere other than the
	*  address that was pinged shows up as exactly that. The three reply
	*  fields are cleared because icmp_last_reply() leaves them alone when it
	*  has nothing to report. */
	*from = dst;
	reply_from = 0;
	reply_id = 0;
	reply_sequence = 0;

	rc = icmp_send_echo(dst, NET_PING_ID, sequence);

	/* The ARP retries keep their fixed sleep, and that is a decision rather
	*  than an omission. Every turn of this loop puts a packet on the wire, so
	*  the sleep is not only a wait, it is what spaces the attempts out -- and
	*  a wake means "some frame was processed", not "the ARP reply arrived".
	*  On a segment with other traffic on it, waking here would spend all
	*  NET_ARP_TRIES attempts on somebody else's frames within a few
	*  milliseconds and report a destination as unresolved that had simply not
	*  answered yet. The whole of what waking could save is the tail of one
	*  twenty millisecond sleep, once per run, against a retry schedule that
	*  would no longer be a schedule. The wait below is where the interval
	*  actually costs something. */
	for(tries = 0; rc != 0 && tries < NET_ARP_TRIES; tries++)
	{
		if(tries == 0)
		{
			printf("resolving ");
			net_print_ip(dst);
			printf(" ... ");
		}

		net_queue_drain();     /* the ARP reply is in there, see below */
		sleep(NET_ARP_WAIT);
		rc = icmp_send_echo(dst, NET_PING_ID, sequence);
	}

	if(rc != 0) return NET_PING_UNRESOLVED;

	start = timer_get_ticks();

	/* The bound is read off the clock rather than added up from the waits,
	*  and that is what the wait below made necessary. A turn that is cut
	*  short by a reply is no longer NET_PING_POLL long, so counting turns
	*  would end the wait after a thousand of them however little time they
	*  took -- NET_PING_WAIT would stop meaning a second and start meaning
	*  whatever the traffic made of it. run_wait() reads its deadline off the
	*  clock for the same reason. */
	for(;;)
	{
		/* Take the frame out of the receive queue ourselves, and do it
		*  BEFORE looking at the mailbox rather than after -- the reply
		*  arrives while this task is blocked at the end of the loop, so
		*  draining afterwards would leave it sitting there for one more
		*  round and add a whole interval to every measurement. Waking
		*  changes nothing about that order: a wake says a frame was
		*  processed, so looking at what came of it is the first thing to do.
		*
		*  The drain task would get to it a full trip round the scheduler
		*  later, and a ping that reports the scheduler instead of the
		*  network is worthless. net_queue_drain() refuses re-entry, so
		*  doing it here alongside that task is safe. */
		net_queue_drain();

		/* Read after the drain and before the mailbox: see net_wait_turn(). */
		seen = net_rx_packets();

		if(icmp_last_reply(&reply_from, &reply_id, &reply_sequence)
		   && reply_id == (uint16_t)NET_PING_ID
		   && reply_sequence == sequence)
		{
			*from = reply_from;

			/* The clock has a millisecond of resolution, so a reply that
			   beats it reads as zero. Zero is a time, not a failure -- which
			   is why the failures above are negative. */
			elapsed = timer_get_ticks() - start;
			if(elapsed < 0) elapsed = 0;
			return elapsed;
		}

		elapsed = timer_get_ticks() - start;
		if(elapsed < 0 || elapsed >= NET_PING_WAIT) break;

		/* Never past the deadline: the last wait of a run that is about to
		*  give up is however much of the second is left, not a whole
		*  interval more. */
		remaining = NET_PING_WAIT - elapsed;
		if(remaining > NET_PING_POLL) remaining = NET_PING_POLL;

		net_wait_turn(seen, remaining);
	}

	return NET_PING_LOST;
}

/* "ping HOST": NET_PING_COUNT echo requests, one line each, and the summary
*  a ping is expected to end with.
*
*  name is what was typed when that was a name and 0 when it was an address.
*  It is only ever printed where the address is not, because the two together
*  in the header would push the line past eighty columns for any name of a
*  realistic length -- and the address is on the line above (pinghost()
*  printed what the name resolved to) and on every reply line below, so
*  nothing is hidden by naming the destination the way the user named it. */
static void net_ping(uint32_t dst, const char *name)
{
	uint32_t from;
	uint16_t sequence;
	int received;
	int fastest;
	int slowest;
	int total;
	int start;
	int rtt;
	int i;

	printf("PING ");
	if(name != 0) printf("%s", name); else net_print_ip(dst);
	printf(": %i requests, %i ms for a reply, %i ms for ARP.\n",
	       NET_PING_COUNT, NET_PING_WAIT, NET_ARP_TRIES * NET_ARP_WAIT);

	received = 0;
	fastest = 0;
	slowest = 0;
	total = 0;
	start = timer_get_ticks();

	for(i = 0; i < NET_PING_COUNT; i++)
	{
		sequence = net_next_sequence;
		net_next_sequence++;

		/* The number printed is the one that goes on the wire, which is why
		   a second run of "ping" carries on where the first stopped instead
		   of counting from one again. */
		printf("  seq %i: ", (int)sequence);

		rtt = net_ping_once(dst, sequence, &from);

		if(rtt == NET_PING_UNRESOLVED)
		{
			/* The address is already on this line, in front of the
			   ellipsis, so it is not repeated -- the line has to stay
			   inside eighty columns. */
			printf("no ARP reply in %i ms -- unreachable\n",
			       NET_ARP_TRIES * NET_ARP_WAIT);
		}
		else if(rtt == NET_PING_LOST)
		{
			printf("no reply within %i ms\n", NET_PING_WAIT);
		} else {
			printf("reply from ");
			net_print_ip(from);
			printf(", %i ms\n", rtt);

			if(received == 0 || rtt < fastest) fastest = rtt;
			if(received == 0 || rtt > slowest) slowest = rtt;
			total += rtt;
			received++;
		}

		/* A real ping paces itself, and the pause is also what lets a late
		   reply to this request arrive and be discarded before the next one
		   goes out. Not after the last one -- nothing is waiting on it. */
		if(i < NET_PING_COUNT - 1) sleep(NET_PING_GAP);
	}

	printf("  --- ");
	if(name != 0) printf("%s", name); else net_print_ip(dst);
	printf(" ping statistics ---\n");

	printf("  %i sent, %i received, %i%% loss, %i ms total\n",
	       NET_PING_COUNT, received,
	       ((NET_PING_COUNT - received) * 100) / NET_PING_COUNT,
	       timer_get_ticks() - start);

	if(received > 0)
	{
		printf("  round trip min/avg/max = %i/%i/%i ms\n",
		       fastest, total / received, slowest);
	}
}

/* --- dhcp ---------------------------------------------------------------- */

/* A duration in the units it is worth reading in.
*
*  A lease is stated in seconds and 86400 is not a number anybody converts in
*  their head, so the seconds are printed and the same value follows in days,
*  hours and minutes. Units that are zero are left out -- "1 d 30 min" rather
*  than "1 d 0 h 30 min" -- and anything under a minute is already readable
*  as it stands and gets no second form at all.
*
*  Written for a lease and named for one until a DNS TTL turned out to be the
*  same problem: a number of seconds a user has to judge in their head. Hence
*  the name without "dhcp" in it. The one lease-specific thing it keeps is
*  0xFFFFFFFF, which DHCP writes for "forever" and which no sane TTL is. */
static void net_duration(uint32_t seconds)
{
	uint32_t days;
	uint32_t hours;
	uint32_t minutes;
	int written;

	if(seconds == (uint32_t)DHCP_LEASE_FOREVER)
	{
		printf("infinite");
		return;
	}

	printf("%u s", seconds);

	if(seconds < DHCP_SECS_PER_MIN) return;

	days = seconds / DHCP_SECS_PER_DAY;
	hours = (seconds / DHCP_SECS_PER_HOUR) % 24;
	minutes = (seconds / DHCP_SECS_PER_MIN) % 60;

	printf(" (");
	written = 0;

	if(days > 0)
	{
		printf("%u d", days);
		written = 1;
	}

	if(hours > 0)
	{
		if(written) putch(' ');
		printf("%u h", hours);
		written = 1;
	}

	if(minutes > 0)
	{
		if(written) putch(' ');
		printf("%u min", minutes);
	}

	printf(")");
}

/* How much of the lease is left, in seconds.
*
*  dhcp_lease_expires() is a moment in milliseconds since boot and
*  timer_get_ticks() is the same clock, so the difference is the answer. It
*  is computed rather than remembered because it is different every time it
*  is asked for, and it saturates at zero: nothing here acts on an expiry, so
*  a lease that has run out is reported as having no time left rather than as
*  a very large number of seconds obtained by subtracting the wrong way. */
static uint32_t net_dhcp_remaining(void)
{
	uint32_t expires;
	uint32_t now;

	expires = dhcp_lease_expires();
	now = (uint32_t)timer_get_ticks();

	if(expires <= now) return 0;

	return (expires - now) / 1000;
}

/* One labelled address out of the lease, where zero means the server did not
*  send that option rather than the address 0.0.0.0. A DHCP server is not
*  obliged to offer a DNS server, and printing 0.0.0.0 for one it withheld
*  would look like an answer. */
static void net_dhcp_leased_ip(const char *label, uint32_t ip)
{
	net_info_label(label);

	if(ip == 0)
	{
		printf("none offered\n");
		return;
	}

	net_print_ip(ip);
	printf("\n");
}

/* One line per step of the exchange, named after the message that caused it.
*
*  The states and the messages are not the same thing and the difference is
*  the whole reason this reads the way it does: reaching DHCP_STATE_REQUEST
*  means an OFFER came in AND the REQUEST went back out, which is two of the
*  four messages in one transition. Naming the line after the message that
*  arrived is what makes the four steps recognisable to somebody watching. */
static void net_dhcp_step(int state)
{
	switch(state)
	{
		case DHCP_STATE_DISCOVER:
			printf("  DISCOVER  broadcast -- is there a server?\n");
			break;
		case DHCP_STATE_REQUEST:
			printf("  OFFER     received -- REQUEST broadcast for it\n");
			break;
		case DHCP_STATE_BOUND:
			printf("  ACK       received -- the lease is confirmed\n");
			break;
		default:
			break;
	}
}

/* What the machine was waiting for when it gave up, from the last step it
*  did reach. This is the difference between "nothing out there answers DHCP
*  at all" and "something answered and then went quiet", and the two point at
*  entirely different things to go and look at. */
static const char *net_dhcp_stalled(int reached)
{
	switch(reached)
	{
		case DHCP_STATE_IDLE:
			return "before the DISCOVER went out";
		case DHCP_STATE_DISCOVER:
			return "waiting for an OFFER -- no server answered the broadcast";
		case DHCP_STATE_REQUEST:
			return "waiting for an ACK -- a server offered, then did not confirm";
		default:
			break;
	}

	return "in a state it should not have stopped in";
}

/* What the lease turned out to be, once it is one.
*
*  The first three are read back through net_ip() and friends rather than
*  from the client, deliberately: they are printed to show what the interface
*  now carries, and reading them from where the rest of the kernel reads them
*  is what makes that claim worth anything. The last three are what DHCP
*  knows and the IP layer has no field for. */
static void net_dhcp_show_lease(void)
{
	net_info_label("Address:");
	net_print_ip(net_ip());
	printf("\n");

	net_info_label("Netmask:");
	net_print_ip(net_netmask());
	printf("\n");

	net_dhcp_leased_ip("Gateway:", net_gateway());
	net_dhcp_leased_ip("DNS server:", dhcp_dns());
	net_dhcp_leased_ip("DHCP server:", dhcp_server());

	net_info_label("Lease:");
	net_duration(dhcp_lease_seconds());
	printf("\n");
}

/* "dhcp": the four message exchange, watched from the outside.
*
*  Built like net_ping_once() and for the same reason -- nothing here parses a
*  packet, because that happens in the drain while this task waits. So the
*  loop drains the queue, calls dhcp_poll() (the only thing that resends a
*  message nobody answered, and the only thing that ever gives up), watches
*  dhcp_state(), and blocks on the network in between.
*
*  The state IS the progress report: each of the four messages moves it, so
*  printing every change shows the exchange happening. A spinner would show
*  only that the machine is still alive, and the question a user with no
*  address actually has is which of the four steps it is stuck on. */
static void net_dhcp_run(void)
{
	uint32_t leased;
	uint32_t seen;
	const char *reason;
	int state;
	int shown;
	int elapsed;
	int remaining;
	int start;

	printf("DHCP on eth0: asking the network for an address.\n");

	if(dhcp_start() != 0)
	{
		printf("dhcp: the exchange could not be started.\n");
		reason = dhcp_last_error();
		if(reason != 0 && reason[0] != EOS) printf("  %s\n", reason);
		return;
	}

	start = timer_get_ticks();
	shown = DHCP_STATE_IDLE;
	state = DHCP_STATE_IDLE;

	/* Read off the clock rather than added up from the waits, because a wait
	*  that a reply cuts short is no longer DHCP_POLL long -- see the same
	*  argument in net_ping_once(). */
	for(;;)
	{
		/* The OFFER and the ACK are in the queue, and taking them out has to
		   happen BEFORE dhcp_poll() is asked where the exchange has got to,
		   not after. Draining afterwards -- which is what this loop did --
		   means the state the drain produced is not looked at until the next
		   turn, so every message of the four cost a whole interval that was
		   not the network's. */
		net_queue_drain();

		/* Read after the drain and before the state is asked for: see
		   net_wait_turn(). */
		seen = net_rx_packets();

		state = dhcp_poll();

		if(state == DHCP_STATE_FAILED) break;

		/* The states before BOUND are numbered in the order they happen in,
		   which is what makes this loop safe at 50 ms: on a virtual network
		   the OFFER and the ACK can both arrive inside one wait, and the
		   client would then be found two steps further on than it was left.
		   Printing only where it is now would silently drop a step that did
		   happen, so every step in between is printed as well. */
		if(shown < state)
		{
			while(shown < state)
			{
				shown++;
				net_dhcp_step(shown);
			}

			/* Printing three lines to a console that may have to scroll takes
			   milliseconds, and milliseconds are several timer ticks: the
			   drain task can process the next message inside them and wake
			   nobody, because this task is running rather than blocked. So
			   the exchange is looked at again before this turn commits to a
			   wait, instead of blocking on a state that has already moved.
			   Measured, this was worth a whole DHCP_POLL on a network that
			   answers a rebind in a tenth of a millisecond -- 54 ms to bind
			   against 5 -- and it costs one extra turn per step printed. */
			if(state != DHCP_STATE_BOUND) continue;
		}

		if(state == DHCP_STATE_BOUND) break;

		elapsed = timer_get_ticks() - start;
		if(elapsed < 0 || elapsed >= DHCP_WAIT) break;

		remaining = DHCP_WAIT - elapsed;
		if(remaining > DHCP_POLL) remaining = DHCP_POLL;

		net_wait_turn(seen, remaining);
	}

	if(state == DHCP_STATE_BOUND)
	{
		/* Remembered before anything is printed: this is the one place that
		   knows the address in the interface was leased rather than typed,
		   and "ifconfig" asks afterwards. */
		net_address_from_lease = 1;

		printf("  Bound in %i ms.\n", timer_get_ticks() - start);
		net_dhcp_show_lease();

		leased = dhcp_lease_seconds();
		if(leased != 0 && leased != (uint32_t)DHCP_LEASE_FOREVER)
		{
			printf("  Nothing renews it: when the lease runs out the address\n");
			printf("  stays configured and stops being ours. \"ifconfig\" shows\n");
			printf("  how much of it is left.\n");
		}
		return;
	}

	/* Everything from here down is a failure, and the point of the next few
	   lines is that the three kinds do not read alike. */
	printf("dhcp: no address was obtained after %i ms.\n",
	       timer_get_ticks() - start);
	printf("  Stalled %s.\n", net_dhcp_stalled(shown));

	if(state != DHCP_STATE_FAILED)
	{
		/* The loop ran out before the client did, which is not a case that
		   should occur -- dhcp_poll() gives up long before DHCP_WAIT. Said
		   plainly, because it means the client is not counting its own
		   attempts rather than that the network is quiet. */
		printf("  The client had not given up yet; the shell stopped waiting.\n");
		dhcp_stop();
	}

	reason = dhcp_last_error();
	if(reason != 0 && reason[0] != EOS) printf("  %s\n", reason);

	if(shown == DHCP_STATE_DISCOVER)
	{
		printf("  A network with no DHCP server is not broken, it is just not\n");
		printf("  answering: set the addresses by hand with\n");
		printf("  \"ifconfig 10.0.2.15 255.255.255.0 10.0.2.2\" instead.\n");
	}
}

/* --- dns ----------------------------------------------------------------- */

/* Why the last lookup did not work, in the resolver's own words.
*
*  Four situations end up here and they are not the same thing at all: the
*  name does not exist, the server tried and failed, no server is configured,
*  or nothing answered. A user acts differently on each -- fix the name, wait
*  and try again, get a server, look at the network -- so dns_last_error() is
*  printed as it stands rather than boiled down into one "lookup failed".
*
*  Only one of the four has a fix that is a single command away, and this is
*  where that command is named: with no lease there is no server to ask, and
*  "dhcp" is what produces one. It is asked of dhcp_dns() rather than of the
*  resolver because the shell is the one that knows where a server would have
*  come from, and it is added to whatever the resolver said rather than in
*  place of it -- a lookup can also fail for its own reasons on a machine
*  that never had a server, and both halves are worth having. */
static void net_dns_explain_error(void)
{
	const char *reason;

	reason = dns_last_error();
	if(reason != 0 && reason[0] != EOS) printf("  %s\n", reason);

	if(dhcp_dns() == 0)
	{
		/* Deliberately not a second "run dhcp": the resolver may well have
		   said that already. What it cannot know, and what the user in front
		   of a hand-configured interface needs, is WHY there is no server --
		   that one arrives with a lease and that "ifconfig" does not set
		   one, so no amount of typing addresses will produce it. */
		printf("  No lease has named one -- a DNS server arrives with the\n");
		printf("  address \"dhcp\" asks for, and \"ifconfig\" does not set one.\n");
	}
}

/* Runs one lookup to a conclusion. Returns 1 and fills in answer when a name
*  was resolved, or 0 having already said why, under the caller's own name.
*
*  Shared by "nslookup" and "ping" because the waiting is identical and only
*  the presentation of the answer is not -- and because the failures must
*  read the same wherever a name was typed. The caller prints its own header
*  line before calling, so that everything printed here starts on a fresh
*  line of its own.
*
*  There is ONE path through here whether the name is cached or not, which is
*  what dns.h asks for: a cached name goes through the same states and its
*  answer comes back through dns_result() and dns_result_ttl() like any
*  other, with the TTL already counted down to what is left of it. So the
*  cache is not read for the answer. It is asked one question before the
*  lookup starts and only that one -- was this already here -- because a
*  cache hit and a fresh query are different things to somebody watching a
*  name that keeps changing, and afterwards there is no way to tell them
*  apart. dns_lookup_cached() is the right question to ask: it matches names
*  the way the cache does, ignoring case and a trailing dot, which a strcmp()
*  in this file would not. */
static int net_dns_resolve(const char *what, const char *name,
                           net_dns_answer *answer)
{
	uint32_t seen;
	int state;
	int elapsed;
	int remaining;
	int start;

	answer->ip = 0;
	answer->ttl = 0;
	answer->cached = (dns_lookup_cached(name) != 0);
	answer->ms = 0;

	if(dns_resolve(name) != 0)
	{
		printf("%s: the lookup of %s could not be started.\n", what, name);
		net_dns_explain_error();
		return 0;
	}

	start = timer_get_ticks();
	state = DNS_STATE_QUERY;

	/* Read off the clock rather than added up from the waits: a wait the
	*  answer cuts short is not DNS_POLL long -- see net_ping_once(). */
	for(;;)
	{
		/* The answer is in the queue, and it is taken out BEFORE dns_poll()
		   is asked what became of the query rather than after. The other
		   order -- which is what this loop did -- leaves the state the drain
		   produced unread until the next turn, and every lookup on a network
		   that answers instantly then costs an interval it did not need. */
		net_queue_drain();

		/* Read after the drain and before the state is asked for: see
		   net_wait_turn(). */
		seen = net_rx_packets();

		state = dns_poll();

		if(state == DNS_STATE_DONE || state == DNS_STATE_FAILED) break;

		elapsed = timer_get_ticks() - start;
		if(elapsed < 0 || elapsed >= DNS_WAIT) break;

		remaining = DNS_WAIT - elapsed;
		if(remaining > DNS_POLL) remaining = DNS_POLL;

		net_wait_turn(seen, remaining);
	}

	if(state == DNS_STATE_DONE)
	{
		answer->ip = dns_result();
		answer->ttl = dns_result_ttl();

		answer->ms = timer_get_ticks() - start;
		if(answer->ms < 0) answer->ms = 0;

		/* Checked because the caller may be about to send packets to it. An
		   address of zero at DNS_STATE_DONE would be a resolver bug rather
		   than a lookup that failed, but ip_send() has no opinion about
		   0.0.0.0 and would happily go looking for its MAC, so it is
		   refused here where it can still be called what it is. */
		if(answer->ip == 0)
		{
			printf("%s: %s resolved to no address at all.\n", what, name);
			return 0;
		}

		/* Nothing is cancelled here on purpose. The lookup is finished, not
		   in flight, and dns_cancel() is for one that is; the next call to
		   dns_resolve() starts from a collected lookup perfectly well. */
		return 1;
	}

	if(state != DNS_STATE_FAILED)
	{
		/* The shell's backstop ran out before the resolver gave up, which
		   dns_poll() is supposed to do long before DNS_WAIT. Said plainly,
		   because it means the resolver is not counting its own attempts
		   rather than that the network is quiet -- the same distinction
		   net_dhcp_run() draws, and for the same reason. */
		printf("%s: %s was not resolved within %i ms.\n", what, name, DNS_WAIT);
		printf("  The resolver had not given up yet; the shell stopped waiting.\n");
		dns_cancel();
		net_dns_explain_error();
		return 0;
	}

	printf("%s: %s could not be resolved.\n", what, name);
	net_dns_explain_error();
	return 0;
}

/* "nslookup" with no name: what has been resolved and what has moved.
*
*  Part of "nslookup" rather than a command of its own, which is the opposite
*  of what "arp" does with the ARP cache, and deliberately. The ARP cache has
*  no command that fills it -- entries appear as a side effect of "ping" --
*  so showing it needs a word of its own. This cache has an owner: every
*  entry in it was put there by "nslookup" or by a "ping" that looked a name
*  up, and the command that fills a cache is the natural place to look at it.
*  A second command word would buy nothing and cost a line of "help".
*
*  The counters come with it, and that is where the argument about "ifconfig"
*  is settled. They would be at home next to the UDP ones -- DNS runs on UDP
*  and "ifconfig" is where one looks when the network does not work -- but
*  that display is already fourteen rows on a screen that has twenty-five,
*  and two more would push the top of it past the edge for the sake of
*  numbers that are one word away here. They also read better here: queries
*  sent against replies used is the story of this cache, and dropped is what
*  separates "the server never answered" from "something answered and the
*  resolver would not have it". */
static void net_dns_show_cache(void)
{
	char entry[NET_DNS_NAME];
	char ip_text[NET_IP_TEXT];
	uint32_t ip;
	uint32_t left;
	int entries;
	int shown;
	int i;

	entries = dns_cache_entries();

	printf("DNS cache:\n");

	if(entries <= 0)
	{
		printf("  Empty -- no name has been looked up yet.\n");

		if(dhcp_dns() == 0)
		{
			printf("  No DNS server is known either: \"dhcp\" asks the network\n");
			printf("  for one, and \"nslookup NAME\" then fills this in.\n");
		} else {
			printf("  \"nslookup NAME\" fills it: every answer is kept until the\n");
			printf("  server's TTL for it runs out.\n");
		}
	} else {
		printf("  ");
		ps_print_left("Name", NET_DNS_NAME_WIDTH);
		ps_print_left("Address", NET_IP_TEXT);
		fs_print_right_text("TTL left", 10);
		printf("\n");

		/* dns_cache_get() answers 0 for an index it filled in, the same way
		   round as arp_cache_get() and the same way round as the eye does
		   not expect. The loop is bounded by the cache size rather than by
		   the count, so a cache that changes between the two calls -- an
		   entry expiring is enough -- cannot run it off the end. */
		shown = 0;
		for(i = 0; i < DNS_CACHE_SIZE; i++)
		{
			if(dns_cache_get(i, entry, (uint32_t)sizeof(entry), &ip, &left) != 0)
			{
				break;
			}

			net_ip_text(ip, ip_text);

			printf("  ");
			ps_print_left(entry, NET_DNS_NAME_WIDTH);
			ps_print_left(ip_text, NET_IP_TEXT);
			mem_print_right(left, 8);
			printf(" s\n");
			shown++;
		}

		printf("  %i of %i cache entries in use.\n", shown, DNS_CACHE_SIZE);
	}

	printf("  Queries sent: ");
	mem_print_right(dns_queries_sent(), 8);
	printf("   used: ");
	mem_print_right(dns_replies_used(), 8);
	printf("   dropped: ");
	mem_print_right(dns_replies_dropped(), 8);
	printf("\n");
}

/* --- netstat -------------------------------------------------------------
*
*  What connections exist -- a question none of the other network commands
*  can answer, because none of the other protocols here has anything to ask
*  it about. An ARP request, an echo, a DHCP offer, a DNS answer: each is one
*  message, and by the time the shell prints anything the exchange is over.
*  A TCP connection is not a message. It persists between commands, it moves
*  from state to state on its own while nobody is looking, and it can stop
*  moving -- and that last case is the whole reason this command exists. It
*  is what one runs when a transfer hangs, so it is built around the field
*  that says where it hangs.
*
*  The state names are RFC 793's, unchanged. Somebody debugging TCP already
*  knows what FIN_WAIT_2 means, and a friendlier word invented here would
*  only have to be translated back before it was any use -- so the column is
*  wide enough for the longest of them and nothing is abbreviated.
*
*  Where the counters go, and why NOT in "ifconfig". The UDP ones are in
*  "ifconfig" because UDP has no command of its own; that was the only place
*  they could be. The DNS ones are not, and the reasons given there apply
*  again word for word: that display is already fourteen rows on a screen
*  with twenty-five, and counters read better beside the thing they count.
*  TCP now has a command of its own, so it follows the DNS precedent rather
*  than the UDP one and "ifconfig" is left exactly as it was. Retransmits
*  would be the one number worth crossing the screen for, and they say very
*  little without the connection they belong to sitting above them.
*
*  One call here is not a read, and it has to be. Nothing outside a network
*  system call drives tcp_poll() on this machine -- src/kernel/syscall.c says so in
*  as many words -- so a connection whose TIME_WAIT ran out ten seconds ago
*  is still sitting in the table with nothing to notice. Printing that row
*  would be answering "is this stuck?" with one that only looks stuck because
*  nobody had asked the stack to look. So the table is polled once before it
*  is read: it walks four slots, it sends nothing the stack did not already
*  owe, and it is what makes the answer true at the moment it is printed.
*/

/* One decimal number, no leading zeroes. net_put_byte() does this for the
*  four parts of an address and stops at 255, which is exactly right for a
*  dotted quad and not enough for a port. Returns the position after the
*  digits it wrote, so callers chain it the same way. */
static char *net_put_uint(char *p, uint32_t value)
{
	char digits[NET_TCP_NUM_TEXT];
	int n;

	/* Written out backwards and reversed, because the number of digits is
	   not known before the last division. */
	n = 0;
	do
	{
		digits[n] = (char)('0' + (int)(value % 10));
		n++;
		value = value / 10;
	} while(value != 0 && n < NET_TCP_NUM_TEXT - 1);

	while(n > 0)
	{
		n--;
		*p = digits[n];
		p++;
	}

	return p;
}

/* An endpoint as "address:port", which is the form a peer is worth reading
*  in -- the two halves identify a connection together and separating them
*  into two columns would only make the eye put them back. text holds
*  NET_TCP_PEER_TEXT bytes. The address half is net_ip_text()'s work: there
*  is one dotted quad printer in this file and this is not a second one. */
static void net_tcp_endpoint_text(uint32_t ip, uint16_t port, char *text)
{
	char *p;

	net_ip_text(ip, text);

	p = text + strlen(text);
	*p = ':';
	p++;
	p = net_put_uint(p, (uint32_t)port);
	*p = EOS;
}

/* "netstat": the connections, and what the stack has actually moved. */
static void net_show_tcp(void)
{
	char handle_text[NET_TCP_NUM_TEXT];
	char port_text[NET_TCP_NUM_TEXT];
	char peer_text[NET_TCP_PEER_TEXT];
	char *p;
	uint32_t peer;
	uint32_t sent;
	uint32_t received;
	uint16_t peer_port;
	uint16_t local_port;
	int handle;
	int state;
	int connections;
	int time_wait;
	int closed;
	int shown;
	int i;

	/* Before anything is read, so that what is read is current -- see the
	   note above. A retransmission that was due goes out here rather than
	   the next time a program happens to make a network call, and a
	   TIME_WAIT that has run its ten seconds is gone before the table is
	   printed instead of after somebody has worried about it. */
	tcp_poll();

	connections = tcp_conn_count();
	time_wait = 0;
	closed = 0;

	printf("TCP connections:\n");

	/* Nothing open is the ordinary state on this machine, not an error and
	   not an empty table: a header with no rows under it reads like a
	   failure, so it is said in words instead -- the same choice "arp" and
	   "nslookup" make about their caches. */
	if(connections <= 0)
	{
		printf("  None are open.\n");

		if(!rtl8139_present() || !net_up() || net_ip() == 0)
		{
			net_explain_down();
		} else {
			printf("  Nothing in the kernel holds a connection between commands,\n");
			printf("  so this is what it looks like when no program is using one.\n");
			printf("  A row appears here while a ring 3 program that opened one\n");
			printf("  with connect() is running, and for a short while after.\n");
		}
	} else {
		printf("  ");
		ps_print_left("Handle", NET_TCP_HANDLE_WIDTH);
		ps_print_left("Local", NET_TCP_PORT_WIDTH);
		ps_print_left("Peer", NET_TCP_PEER_WIDTH);
		ps_print_left("State", NET_TCP_STATE_WIDTH);
		fs_print_right_text("Sent", NET_TCP_BYTES_WIDTH);
		fs_print_right_text("Received", NET_TCP_BYTES_WIDTH);
		printf("\n");

		/* tcp_conn_get() answers 0 for an index it filled in, the same way
		   round as arp_cache_get() and dns_cache_get() and the same way
		   round as the eye does not expect. The loop is bounded by the
		   table size rather than by the count, so a connection that closes
		   between the two calls -- a TIME_WAIT running out is enough --
		   cannot run it off the end. */
		shown = 0;
		for(i = 0; i < TCP_MAX_CONNS; i++)
		{
			if(tcp_conn_get(i, &handle, &peer, &peer_port, &local_port,
			                &state, &sent, &received) != 0)
			{
				break;
			}

			p = net_put_uint(handle_text, (uint32_t)handle);
			*p = EOS;
			p = net_put_uint(port_text, (uint32_t)local_port);
			*p = EOS;
			net_tcp_endpoint_text(peer, peer_port, peer_text);

			printf("  ");
			ps_print_left(handle_text, NET_TCP_HANDLE_WIDTH);
			ps_print_left(port_text, NET_TCP_PORT_WIDTH);
			ps_print_left(peer_text, NET_TCP_PEER_WIDTH);
			ps_print_left(tcp_state_name(state), NET_TCP_STATE_WIDTH);
			mem_print_right(sent, NET_TCP_BYTES_WIDTH);
			mem_print_right(received, NET_TCP_BYTES_WIDTH);
			printf("\n");

			if(state == TCP_TIME_WAIT) time_wait++;
			if(state == TCP_CLOSED) closed++;
			shown++;
		}

		printf("  %i of %i connection(s) in use.\n", shown, TCP_MAX_CONNS);

		/* The two states that look like a fault and are not. Both are said
		   only when one is actually on the screen: an explanation of
		   something nobody is looking at is just another row, but when
		   there IS one, this is the line that stops somebody hunting for a
		   leak that does not exist. */
		if(time_wait != 0)
		{
			printf("  TIME_WAIT is finished, not stuck: the side that closed\n");
			printf("  first waits there in case its last acknowledgement was\n");
			printf("  lost and the peer resends its FIN. It ends on its own.\n");
		}

		if(closed != 0)
		{
			printf("  CLOSED is finished too. The slot is held a little longer\n");
			printf("  so a program still reading can see the end of the stream,\n");
			printf("  and is then given back.\n");
		}
	}

	/* Always, whether anything is open or not: they are what the stack has
	   done since it came up, and after a transfer has ended they are all
	   that is left of it. */
	printf("  Segments tx: ");
	mem_print_right(tcp_segments_sent(), 8);
	printf("   rx: ");
	mem_print_right(tcp_segments_received(), 8);
	printf("   dropped: ");
	mem_print_right(tcp_segments_dropped(), 8);
	printf("\n");

	printf("  Retransmits: ");
	mem_print_right(tcp_retransmits(), 8);
	printf("\n");

	/* Retransmitting is how a lost segment is recovered, so the count is not
	   a fault by itself -- but it is the number that separates a slow link
	   from a broken stack, and that only helps somebody who knows which it
	   is. Said once, when there is something to say it about. */
	if(tcp_retransmits() != 0)
	{
		printf("  A retransmit is a segment nobody acknowledged, sent again.\n");
		printf("  A few are a lossy link; a count climbing through one\n");
		printf("  transfer is the link, not the stack.\n");
	}
}

/* --- lsusb ---------------------------------------------------------------
*
*  What is on the USB bus, written for the machine where a device is NOT
*  working -- which is the only reason anybody types this. So the states that
*  had to read well are the empty ones, not the one where a keyboard is found,
*  and they are told apart rather than run together into one "no USB":
*
*    - no USB host controller on the PCI bus at all, which is what
*      "make run USB=0" gives;
*    - one that is there and is not UHCI, which is every machine built in the
*      last decade and is the case usb.h's layering was written for;
*    - a UHCI that was found and did not come up, where uhci_info() is the
*      only thing that knows why;
*    - a controller that is up, with nothing plugged in or with devices the
*      kernel enumerated and has no driver for.
*
*  Everything here asks usb.h, uhci.h and pci.h questions and formats the
*  answers. No transfer is started from this file, and no register is touched.
*
*  The formatting is the house style of the "ps", "df", "lspci" and "netstat"
*  tables: printf() has no field widths, so a value that has to line up in a
*  column is written into a small buffer first and padded into place by
*  ps_print_left(), fs_print_right_text() or mem_print_right(). */

static void usb_info_label(const char *label)
{
	int used;

	printf("  %s", label);

	used = 2 + (int)strlen(label);
	while(used < USB_INFO_LABEL)
	{
		putch(' ');
		used++;
	}
}

/* The two speeds this stack knows, in the words the specification uses.
*  Anything else cannot reach here -- the core forces an unrecognised value to
*  full speed before it stores it -- but the default is kept so that a future
*  high speed device does not silently print as a full speed one. */
static const char *usb_speed_text(int speed)
{
	switch(speed)
	{
		case USB_SPEED_LOW:  return "low";
		case USB_SPEED_FULL: return "full";
		default:             break;
	}

	return "?";
}

static const char *usb_endpoint_type_text(uint8_t type)
{
	switch(type)
	{
		case USB_XFER_CONTROL:   return "control";
		case USB_XFER_ISOC:      return "isochronous";
		case USB_XFER_BULK:      return "bulk";
		case USB_XFER_INTERRUPT: return "interrupt";
		default:                 break;
	}

	return "unknown";
}

/* What the class triple MEANS, next to the raw numbers rather than instead of
*  them.
*
*  The numbers stay because they are what somebody compares against "lsusb" on
*  a real machine, and a name this file invented would not compare against
*  anything. But a bare 03:01:01 helps nobody either: the whole difference
*  between a keyboard and a mouse is in the last byte, and that is precisely
*  what a person staring at a device that does not work needs to read.
*
*  How far the decoding goes is deliberately uneven. The full triple is taken
*  apart for the two classes this stack is about -- HID, where the subclass
*  says "boot protocol" and the protocol says which of the two boot devices it
*  is, and mass storage, where the pair says which command set travels over
*  which transport. Everything else gets its class named and its subclass and
*  protocol left as the numbers in the column, because naming them would be a
*  table of hundreds of entries in aid of a device this kernel would not touch
*  anyway. Class 0x09, the hub, is named for the opposite reason: it is not
*  supported on purpose and that is exactly what the reader has to be told. */
static const char *usb_class_text(uint8_t iface_class, uint8_t subclass,
                                  uint8_t protocol)
{
	switch(iface_class)
	{
		case 0x00:
			/* Invalid in an interface descriptor: 0 means "look at the
			   device descriptor instead", and a device that puts it here is
			   describing nothing. */
			return "no interface class";

		case 0x01: return "audio";
		case 0x02: return "communications";

		case USB_CLASS_HID:
			if(subclass == USB_HID_SUB_BOOT)
			{
				if(protocol == USB_HID_PROTO_KEYBOARD) return "HID boot keyboard";
				if(protocol == USB_HID_PROTO_MOUSE)    return "HID boot mouse";
				return "HID boot, no protocol";
			}
			if(subclass == 0x00) return "HID, no boot protocol";
			return "HID";

		case 0x05: return "physical interface";
		case 0x06: return "still imaging";
		case 0x07: return "printer";

		case USB_CLASS_MASS_STORAGE:
			if(subclass == USB_MSC_SUB_SCSI)
			{
				if(protocol == USB_MSC_PROTO_BULK) return "SCSI disk, bulk-only";
				return "SCSI disk";
			}
			if(subclass == 0x04) return "UFI floppy";
			if(subclass == 0x05) return "SFF-8070i storage";
			return "mass storage";

		case USB_CLASS_HUB:  return "hub (not supported)";

		case 0x0A: return "communications data";
		case 0x0B: return "smart card";
		case 0x0E: return "video";

		case 0xE0:
			if(subclass == 0x01 && protocol == 0x01) return "Bluetooth";
			return "wireless controller";

		case 0xFF: return "vendor specific";

		default:   break;
	}

	return "unknown class";
}

/* Which of the four incompatible host controller interfaces a PCI device is.
*  The prog-if byte is the whole answer and the reason pci.h keeps it: class
*  0C subclass 03 says "USB host controller" and says nothing about which
*  kind, and their register interfaces have nothing in common. */
static const char *usb_hc_kind_text(uint8_t prog_if)
{
	switch(prog_if)
	{
		case USB_PROGIF_UHCI: return "UHCI, USB 1.1 -- the one this kernel drives";
		case USB_PROGIF_OHCI: return "OHCI, USB 1.1";
		case USB_PROGIF_EHCI: return "EHCI, USB 2.0";
		case USB_PROGIF_XHCI: return "xHCI, USB 3";
		case USB_PROGIF_DEV:  return "a USB device, not a host controller";
		default:              break;
	}

	return "an unknown host controller interface";
}

/* Whether a UHCI controller is on the bus at all. Asked only to tell "there
*  is nothing here for the driver" apart from "the driver had something and
*  could not bring it up", which are two different problems with two different
*  answers. */
static int usb_uhci_on_bus(void)
{
	const pci_device *dev;
	int i;

	for(i = 0; i < pci_count(); i++)
	{
		dev = pci_get(i);
		if(dev == 0) continue;

		if(dev->class_code == PCI_CLASS_SERIAL &&
		   dev->subclass   == USB_PCI_SUBCLASS &&
		   dev->prog_if    == USB_PROGIF_UHCI)
		{
			return 1;
		}
	}

	return 0;
}

/* Every USB host controller the PCI enumeration found, of whatever kind, and
*  how many there were. "Is there USB hardware here" is a different question
*  from "is there USB hardware this kernel can drive", and only this list
*  answers the first one. */
static int usb_show_hc_on_bus(void)
{
	const pci_device *dev;
	char text[NET_HEX_TEXT];
	int found;
	int i;

	found = 0;

	for(i = 0; i < pci_count(); i++)
	{
		dev = pci_get(i);
		if(dev == 0) continue;

		if(dev->class_code != PCI_CLASS_SERIAL) continue;
		if(dev->subclass   != USB_PCI_SUBCLASS) continue;

		found++;

		printf("    ");
		net_hex_text((uint32_t)dev->bus, 2, text);
		printf("%s:", text);
		net_hex_text((uint32_t)dev->slot, 2, text);
		printf("%s.%i  ", text, (int)dev->func);
		net_hex_text((uint32_t)dev->vendor, 4, text);
		printf("%s:", text);
		net_hex_text((uint32_t)dev->device, 4, text);
		printf("%s  prog-if ", text);
		net_hex_text((uint32_t)dev->prog_if, 2, text);
		printf("%s, %s\n", text, usb_hc_kind_text(dev->prog_if));
	}

	return found;
}

/* Why an xHCI on the bus is not a fault, said once. The header of usb.h is
*  where this comes from and it is worth repeating on screen, because a user
*  who reads "no USB" on a modern laptop has every reason to think the machine
*  is broken. */
static void usb_explain_layering(void)
{
	printf("  usb.h draws its line for exactly this reason. Resetting a port,\n");
	printf("  addressing a device and reading its descriptors are the same\n");
	printf("  whatever the silicon underneath; only the bottom third, moving\n");
	printf("  bytes to an endpoint, is controller specific. A driver for\n");
	printf("  another controller fills in usb_hc_ops underneath, and\n");
	printf("  everything above it -- including this command -- keeps working\n");
	printf("  unchanged.\n");
}

/* No controller. The ordinary outcome, and the one that has to teach rather
*  than merely report -- so it does not stop at "none" but says what IS on the
*  bus, which is the difference between "this machine has no USB" and "this
*  machine's USB is not the kind this kernel speaks". */
static void usb_explain_absent(void)
{
	int controllers;

	usb_info_label("Controller:");
	printf("none this kernel can drive\n");

	printf("  USB host controllers on the PCI bus (class 0C:03):\n");
	controllers = usb_show_hc_on_bus();

	if(controllers == 0)
	{
		printf("    None.\n");
		printf("  There is no USB hardware here at all, which is what \"make run\n");
		printf("  USB=0\" gives: QEMU attaches a controller only when it is asked\n");
		printf("  to, with \"-device piix3-usb-uhci\".\n");
		usb_explain_layering();
		return;
	}

	if(usb_uhci_on_bus())
	{
		/* The one case here that IS a fault. Everything above knows only
		   that no controller registered; uhci.c knows which step failed,
		   and it kept the sentence. */
		printf("  A UHCI controller is on the bus and did not come up, so\n");
		printf("  this is a failure rather than a missing device. The driver\n");
		printf("  is the only thing that knows which step it was:\n");
		printf("    %s\n", uhci_info());
		return;
	}

	printf("  There is USB hardware here, but none of it is UHCI -- the only\n");
	printf("  host controller interface this kernel drives. This is a limit\n");
	printf("  of the driver, not a fault of the machine: every machine built\n");
	printf("  in the last decade has xHCI and nothing else, and its keyboard\n");
	printf("  works perfectly well under a system that has an xHCI driver.\n");
	usb_explain_layering();
}

/* THE FRAME NUMBER, AS A RATE.
*
*  uhci.h calls this the one number that separates a controller which is
*  running from one that was set up and never started -- and one sample cannot
*  do that, because every value it could show, zero included, is a legal
*  reading for both. Only the difference between two samples says anything, so
*  this takes two a fixed interval apart and reports what moved.
*
*  It is printed as a rate rather than as a raw difference because a rate has
*  an expected value and a difference does not. One frame is one millisecond
*  by definition, so a live UHCI controller reads about 1000 frames a second
*  and nothing else, whatever the interval was. That turns the line into a
*  check anybody can make without knowing this kernel: 1000 is right, 0 is a
*  controller that was set up and never started, and something in between is
*  a sample the scheduler disturbed rather than a bus running at some other
*  speed.
*
*  The elapsed time is measured rather than assumed. sleep() may return late
*  under a busy scheduler, and dividing by the interval that was asked for
*  instead of the one that actually passed would turn the shell's own
*  scheduling into a wrong statement about the hardware.
*
*  The running total is on the same line and is the smaller half of the
*  answer: it says how long the controller has been alive, and it is the one
*  number that survives being read once.
*
*  All of it is one line, with the paragraph underneath only where there is
*  something wrong to say. The whole report has to fit on a screen that holds
*  twenty-four lines, and a sentence explaining that everything is fine is the
*  first thing to lose when the alternative is scrolling the controller off
*  the top. */
static void usb_show_frames(void)
{
	uint32_t first;
	uint32_t second;
	uint32_t moved;
	uint32_t rate;
	unsigned int started;
	unsigned int elapsed;

	first   = uhci_frames();
	started = (unsigned int)timer_get_ticks();

	sleep(USB_FRAME_SAMPLE_MS);

	second = uhci_frames();

	/* Unsigned subtraction, because the millisecond counter wraps and only
	   the difference of two snapshots stays meaningful across the wrap --
	   the same reason sleep() computes its deadline this way. */
	elapsed = (unsigned int)timer_get_ticks() - started;
	if(elapsed == 0) elapsed = (unsigned int)USB_FRAME_SAMPLE_MS;

	moved = (second >= first) ? (second - first) : 0;
	rate  = (moved * 1000UL) / (uint32_t)elapsed;

	usb_info_label("Frames:");
	mem_print_right(rate, 1);
	printf("/s -- ");

	if(moved == 0)
	{
		printf("NOT running (");
	}
	else if(rate < (uint32_t)USB_FRAME_RATE_LOW ||
	        rate > (uint32_t)USB_FRAME_RATE_HIGH)
	{
		printf("running, oddly (");
	} else {
		printf("the schedule is running (");
	}

	mem_print_right(moved, 1);
	printf(" in %u ms, ", (int)elapsed);
	mem_print_right(second, 1);
	printf(" total)\n");

	if(moved == 0)
	{
		printf("  The frame number has not moved, so the schedule is not\n");
		printf("  running. A controller that was set up and never started\n");
		printf("  looks identical to a working one from every other angle --\n");
		printf("  its registers read back, its ports report what is plugged\n");
		printf("  in, and every transfer merely times out. This is the one\n");
		printf("  number that tells the two apart.\n");
		return;
	}

	if(rate < (uint32_t)USB_FRAME_RATE_LOW || rate > (uint32_t)USB_FRAME_RATE_HIGH)
	{
		printf("  A UHCI frame is one millisecond by definition, so a running\n");
		printf("  controller reads about %u/s and no other rate. A reading\n",
		       (int)USB_FRAME_RATE_NOMINAL);
		printf("  well off that is this shell being descheduled during the\n");
		printf("  sample, not the bus. Run it again.\n");
	}
}

/* The endpoints of one device, one to a line under its row.
*
*  Endpoint 0 is deliberately not among them and neither is any isochronous
*  endpoint -- see the note usb_show_devices() prints under the table, which
*  is where that belongs because it is a property of the listing rather than
*  of any one device. */
static void usb_show_endpoints(const usb_device *dev)
{
	const usb_endpoint *ep;
	int i;

	for(i = 0; i < dev->endpoints && i < USB_MAX_ENDPOINTS; i++)
	{
		ep = &dev->endpoint[i];

		printf("      EP");
		mem_print_right((uint32_t)ep->address, 3);
		putch(' ');
		ps_print_left((ep->direction & USB_DIR_IN) ? "IN" : "OUT", 4);
		ps_print_left(usb_endpoint_type_text(ep->type), 12);
		mem_print_right((uint32_t)ep->max_packet, 4);
		printf(" bytes");

		/* An interval is only meaningful for an interrupt endpoint. Bulk
		   and control transfers are scheduled whenever there is bandwidth
		   left, and a device may put anything at all in the field, so
		   printing it would be inventing a promise nobody made. */
		if(ep->type == USB_XFER_INTERRUPT)
		{
			if(ep->interval <= 1)
			{
				printf(", polled every frame");
			} else {
				printf(", polled every %u frames", (int)ep->interval);
			}
		}

		printf("\n");
	}
}

/* The table, and the two empty states that are not the table.
*
*  Raw numbers in the columns, because they are what somebody compares against
*  "lsusb" on a real machine; the reading of them in the last column, because
*  a class code nobody can decode is a wasted line. Hexadecimal for both ids
*  and the class triple: that is how "lsusb" writes an id, it is how "lspci"
*  writes a class here, and half the class codes -- 0x0A, 0x0E, 0xE0, 0xFF --
*  are only recognisable that way. */
/* One extra line under a mass storage device: which block device number it
*  became, and how big it is.
*
*  It is not part of the table because it is not a USB fact -- everything else
*  in that row came off the wire, and this comes from the layer above. But it
*  is the number "df" and a mount would name, so a listing that shows a disk
*  and leaves it out sends the reader to another command to finish a sentence
*  this one started.
*
*  A unit the driver refused -- a sector size that is not 512, a medium that
*  never became ready -- has no number, and saying so is the point: without it
*  a refused stick and a stick nobody looked at are the same blank space. */
static void usb_show_storage_line(const usb_device *dev)
{
	int unit;
	int blkdev;

	if(dev->iface_class != USB_CLASS_MASS_STORAGE)
		return;

	for(unit = 0; unit < usbmsc_count(); unit++)
	{
		blkdev = usbmsc_blkdev(unit);

		printf("      ");
		if(blkdev < 0)
		{
			printf("no block device: %s\n", usbmsc_describe(unit));
		}
		else
		{
			printf("block device %i, %i MiB, %s\n",
			       blkdev,
			       (int)(blk_sectors(blkdev) / 2048u),
			       blk_describe(blkdev));
		}
	}
}

static void usb_show_devices(void)
{
	const usb_device *dev;
	char id_text[USB_ID_TEXT];
	char class_text[USB_CLASS_TEXT];
	int count;
	int bound;
	int hubs;
	int i;

	count = usb_device_count();

	usb_info_label("Devices:");
	if(count == 0)
	{
		printf("none\n");

		/* Two ways to end up with an empty list, and they are opposite
		   problems. Nothing answered a port is the ordinary one; something
		   answered and did not survive being enumerated is a failure, and
		   the counters above are what tell them apart. */
		if(usb_errors() != 0)
		{
			printf("  Something did answer a root port and did not survive being\n");
			printf("  enumerated: the failure count above is not zero, and the last\n");
			printf("  error names the step that gave up. So this empty list is a\n");
			printf("  failure rather than an empty controller.\n");
			return;
		}

		printf("  Every root port was reset and none of them answered, so nothing\n");
		printf("  is plugged in -- and no transfer was needed to find that out,\n");
		printf("  which is why the counters above are zero. That is an empty\n");
		printf("  controller, not a broken one; the frame rate is what says the\n");
		printf("  controller itself is alive.\n");
		printf("  \"make run USB=hid\" plugs a keyboard and a mouse in. It is\n");
		printf("  not the default because QEMU then routes input to them, and\n");
		printf("  there is no HID driver yet -- the machine boots and cannot\n");
		printf("  be typed at. That driver is the next thing missing here.\n");
		return;
	}

	bound = 0;
	hubs  = 0;

	for(i = 0; i < count; i++)
	{
		dev = usb_device_get(i);
		if(dev == 0) continue;
		if(dev->driver != 0 && strcmp(dev->driver, "none") != 0) bound++;
		if(dev->iface_class == USB_CLASS_HUB) hubs++;
	}

	printf("%i enumerated, ", count);
	if(bound == 0)
	{
		printf("none with a driver\n");
	} else {
		printf("%i with a driver\n", bound);
	}

	printf("  ");
	fs_print_right_text("Port", 6);
	fs_print_right_text("Addr", 6);
	fs_print_right_text("Speed", 6);
	fs_print_right_text("ID", 11);
	fs_print_right_text("Class", 10);
	fs_print_right_text("EPs", 4);
	printf("  ");
	ps_print_left("Driver", 9);
	printf("What it says it is\n");

	for(i = 0; i < count; i++)
	{
		dev = usb_device_get(i);
		if(dev == 0) continue;

		/* "0627:0001", the way lsusb prints an id and the form a search
		   engine takes. */
		net_hex_text((uint32_t)dev->vendor, 4, id_text);
		id_text[4] = ':';
		net_hex_text((uint32_t)dev->product, 4, id_text + 5);

		/* "03:01:01" -- class, subclass and protocol, which are only
		   meaningful as a triple, in the same colon form "lspci" uses for
		   the pair it prints. */
		net_hex_text((uint32_t)dev->iface_class, 2, class_text);
		class_text[2] = ':';
		net_hex_text((uint32_t)dev->iface_subclass, 2, class_text + 3);
		class_text[5] = ':';
		net_hex_text((uint32_t)dev->iface_protocol, 2, class_text + 6);

		printf("  ");
		mem_print_right((uint32_t)dev->port, 6);
		mem_print_right((uint32_t)dev->address, 6);
		fs_print_right_text(usb_speed_text(dev->speed), 6);
		fs_print_right_text(id_text, 11);
		fs_print_right_text(class_text, 10);
		mem_print_right((uint32_t)dev->endpoints, 4);
		printf("  ");

		/* usb.h promises this is never null; the guard is here because a null
		   would print as nothing at all and leave an empty column that looks
		   like a device with no driver rather than like a bug. */
		ps_print_left((dev->driver != 0) ? dev->driver : "?", 9);

		printf("%s\n", usb_class_text(dev->iface_class, dev->iface_subclass,
		                              dev->iface_protocol));

		usb_show_endpoints(dev);

		/* For a disk, the block device number it took -- which is the one
		   thing on this screen a person then has to type, and the only piece
		   of the answer that is not in the USB descriptors. A unit that was
		   refused says so instead, because "no number" and "not listed" look
		   identical otherwise. */
		usb_show_storage_line(dev);
	}

	/* Why the EPs column may be smaller than the device's own descriptor
	   says. Printed once, under the table, because it is a property of the
	   listing and not of any one row. */
	printf("\n");
	printf("  Endpoint 0 is in no configuration, and isochronous endpoints\n");
	printf("  are dropped on purpose (see usb.h). Neither is listed.\n");

	if(bound == 0)
	{
		printf("  No device has a driver. Enumeration finished -- port reset,\n");
		printf("  address given, descriptors read, configuration set -- so every\n");
		printf("  number above came off the wire. What is missing is the layer\n");
		printf("  above: nothing here claims a class yet, so nobody polls those\n");
		printf("  endpoints. The bus works; there is no driver sitting on it.\n");
	}

	if(hubs != 0)
	{
		printf("  A hub is among them, and hubs are absent on purpose -- usb.h\n");
		printf("  lists them next to isochronous transfers and USB 3. So the hub\n");
		printf("  enumerated and nothing plugged into it did: those are ports\n");
		printf("  only a hub driver could reset. QEMU produces this by itself --\n");
		printf("  it inserts a hub once more than two devices are attached.\n");
	}
}

/* What the transfers add up to. Counted by the core across every device, so
*  they are the bus's total and not any one device's -- which is what makes
*  them useful when the question is whether anything has moved at all. */
static void usb_show_counters(void)
{
	const char *error;

	usb_info_label("Transfers:");
	mem_print_right(usb_transfers(), 1);
	printf(" attempted, ");
	mem_print_right(usb_errors(), 1);
	printf(" failed\n");

	error = usb_last_error();

	usb_info_label("Last error:");
	if(error == 0 || error[0] == EOS)
	{
		printf("none since boot\n");
	} else {
		printf("%s\n", error);
	}

}

static void usb_show_bus(void)
{
	printf("USB bus:\n");

	if(!usb_present())
	{
		usb_explain_absent();
		return;
	}

	usb_info_label("Controller:");
	printf("%s\n", usb_hc_name());

	/* THE TWO LINES BELOW ARE THE ONLY ONES THAT ASK uhci.h, and they are
	   behind uhci_present() because of the very layering this command
	   describes. Everything else here goes through usb.h and would print
	   the same for an OHCI or xHCI driver that filled in usb_hc_ops; the
	   info string and the frame counter belong to one particular
	   controller, and asking them about another one would answer 0 -- which
	   reads as "the schedule is stopped" and would be a lie about a bus
	   that is working perfectly. */
	if(uhci_present())
	{
		/* The controller's own account of itself, in its own words. The one
		   line here this file did not compose, and it carries what only
		   uhci.c knows: where the registers are, how many root ports there
		   are, what the BIOS had done to the legacy support register before
		   the driver took it away, and whether the driver polls or takes an
		   interrupt. It is longer than eighty columns and is allowed to
		   wrap -- breaking it up would mean parsing a string uhci.c owns. */
		usb_info_label("It reports:");
		printf("%s\n", uhci_info());

		usb_show_frames();
	} else {
		usb_info_label("Frames:");
		printf("not asked -- %s is not the UHCI driver\n", usb_hc_name());
	}

	usb_show_counters();
	usb_show_devices();
}

/* --- the commands themselves --------------------------------------------- */

void listpci(char *cmd)
{
	if(prmc(cmd) != 0)
	{
		printf("Syntax: lspci\n");
		printf("\t          List the devices the PCI enumeration found\n");
		return;
	}

	net_show_pci();
}

void listusb(char *cmd)
{
	/* No options. There are at most eight devices and two root ports, so
	   there is nothing here worth filtering out -- and the command is typed
	   when something does not work, which is exactly when the line somebody
	   would have filtered away is the one that explains it. */
	if(prmc(cmd) != 0)
	{
		printf("Syntax: lsusb\n");
		printf("\t          Show the USB controller, whether its schedule is\n");
		printf("\t          running, and the devices that were enumerated\n");
		return;
	}

	usb_show_bus();
}

void netconfig(char *cmd)
{
	char address_text[100];
	char netmask_text[100];
	char gateway_text[100];
	uint32_t address;
	uint32_t netmask;
	uint32_t gateway;

	if(prmc(cmd) == 0)
	{
		net_show_interface();
		return;
	}

	/* The queue is the interface's, so it is under the interface's word --
	   see net_show_queue() for why it is behind an option and not in the
	   display above it. Checked before the count, because one argument that
	   is not "-q" is a mistyped address list and belongs in the syntax
	   message with the rest of them. */
	if(prmc(cmd) == 1 && strcmp(prmv(1, cmd), "-q") == 0)
	{
		net_show_queue();
		return;
	}

	if(prmc(cmd) != 3)
	{
		printf("Syntax: ifconfig [-q] [IP NETMASK GATEWAY]\n");
		printf("\t          Show the interface, its addresses and counters\n");
		printf("\t-q        Show the receive queue the card's interrupt fills\n");
		printf("\tIP ...    Set all three, for example\n");
		printf("\t          ifconfig 10.0.2.15 255.255.255.0 10.0.2.2\n");
		printf("\t          which is what QEMU's user network expects\n");
		return;
	}

	/* prmv() hands back a pointer into one static buffer, so each argument
	   is copied out before the next call overwrites it. */
	strcpy(address_text, prmv(1, cmd));
	strcpy(netmask_text, prmv(2, cmd));
	strcpy(gateway_text, prmv(3, cmd));

	if(!net_parse_ip(address_text, &address))
	{
		printf("ifconfig: %s is not an address.\n", address_text);
		return;
	}

	if(!net_parse_ip(netmask_text, &netmask))
	{
		printf("ifconfig: %s is not a netmask.\n", netmask_text);
		return;
	}

	if(!net_parse_ip(gateway_text, &gateway))
	{
		printf("ifconfig: %s is not an address.\n", gateway_text);
		return;
	}

	/* A netmask is a run of ones followed by a run of zeroes, and nothing
	   else. The check is worth making because a mask like 255.0.255.0 does
	   not fail loudly later -- it just makes "is this address on my subnet"
	   answer nonsense, and the packet goes to the wrong next hop. */
	if((~netmask & (~netmask + (uint32_t)1)) != (uint32_t)0)
	{
		printf("ifconfig: %s is not a contiguous netmask.\n", netmask_text);
		return;
	}

	net_configure(address, netmask, gateway);

	/* Typed numbers, whatever the interface carried before them. A lease may
	   still be running underneath -- dhcp_stop() is not called, because the
	   address it obtained is exactly what is being overwritten here and the
	   conversation is over either way -- but it is no longer what the
	   interface is using, and "ifconfig" must not go on claiming it is. */
	net_address_from_lease = 0;

	net_show_interface();
}

/* "dhcp": ask the network instead of being told.
*
*  What happens when there already is an address: nothing, unless -f is
*  given. It is the conservative choice and it is the right one here, because
*  the failure is asymmetric. Starting an exchange means the interface is
*  committed to whatever comes back -- and if nothing comes back, the state
*  is FAILED and the machine has spent seconds finding that out, on a machine
*  that was reachable when the command was typed. A user who typed "dhcp"
*  twice by accident, or who typed it on a machine somebody had already
*  configured by hand, gets told what the address is and where it came from
*  instead. Asking again is one flag away, and then it is deliberate. */
void dhcpclient(char *cmd)
{
	int force;

	force = 0;

	if(prmc(cmd) == 1 && strcmp(prmv(1, cmd), "-f") == 0) force = 1;

	if(prmc(cmd) > 1 || (prmc(cmd) == 1 && !force))
	{
		printf("Syntax: dhcp [-f]\n");
		printf("\t          Ask the network for an address, netmask, gateway\n");
		printf("\t          and DNS server, and configure the interface with\n");
		printf("\t          what comes back\n");
		printf("\t-f        Ask again although an address is already set\n");
		return;
	}

	/* No address is wanted here -- obtaining one is the point -- but a card
	   and a stack are, and this says which of the two is missing. */
	if(!net_interface_ready("dhcp", 0)) return;

	if(net_ip() != 0 && !force)
	{
		printf("dhcp: the interface already has ");
		net_print_ip(net_ip());
		printf(".\n");

		if(net_address_from_lease)
		{
			printf("      It came from a lease -- \"ifconfig\" shows how much of\n");
			printf("      it is left. \"dhcp -f\" asks for a new one.\n");
		} else {
			printf("      It was set by hand with \"ifconfig\". \"dhcp -f\" replaces\n");
			printf("      it with whatever the network offers.\n");
		}

		printf("      Nothing was sent.\n");
		return;
	}

	net_dhcp_run();
}

void arptable(char *cmd)
{
	if(prmc(cmd) != 0)
	{
		printf("Syntax: arp\n");
		printf("\t          Show which addresses have been resolved to a MAC\n");
		return;
	}

	net_show_arp();
}

/* "ping HOST": an address, or a name to look up first.
*
*  Taking a name is what makes the resolver useful rather than a thing one
*  can demonstrate, and it costs one branch: what does not parse as a dotted
*  quad is tried as a name.
*
*  The order of the three steps is the whole of the care needed here. The
*  parse decides which kind of argument this is and nothing else -- it does
*  NOT write dst when it fails, which is exactly why a failed lookup has to
*  return rather than fall through: dst would still hold whatever the stack
*  left there, and ip_send() treats 0.0.0.0 as a destination like any other
*  and would go looking for its MAC. A name that does not resolve fails as a
*  name, here, before there is an address to misuse. */
void pinghost(char *cmd)
{
	char target[NET_DNS_NAME];
	net_dns_answer answer;
	uint32_t dst;
	int named;

	if(prmc(cmd) == 0)
	{
		printf("Syntax: ping HOST\n");
		printf("\t          Send %i echo requests and time the replies\n",
		       NET_PING_COUNT);
		printf("\tHOST      An address, for example ping 10.0.2.2, or a name\n");
		printf("\t          to look up first, for example ping example.com\n");
		return;
	}

	/* prmv() hands back a pointer into one static buffer, so the argument is
	   copied out before anything else can call prmv() again. */
	strcpy(target, prmv(1, cmd));

	named = !net_parse_ip(target, &dst);

	/* Asked before the lookup, not only before the echo requests: a query is
	   a packet too, and "no address is configured" is a better answer to
	   "ping example.com" than a resolver timing out because it had no source
	   address to send from. */
	if(!net_interface_ready("ping", 1)) return;

	if(named)
	{
		/* Printed before the wait rather than after it, because the wait is
		   the part with nothing to show: a whole line, so that whatever
		   net_dns_resolve() has to say begins on the next one. */
		printf("ping: looking up %s ...\n", target);

		if(!net_dns_resolve("ping", target, &answer)) return;

		dst = answer.ip;

		printf("ping: %s is ", target);
		net_print_ip(dst);
		if(answer.cached)
		{
			printf(" -- cached, ");
			net_duration(answer.ttl);
			printf(" left\n");
		} else {
			printf(" -- TTL ");
			net_duration(answer.ttl);
			printf("\n");
		}
	}

	net_ping(dst, named ? target : 0);
}

/* "nslookup": a name into an address, and the cache that keeps the answers.
*
*  Two jobs in one command word -- see net_dns_show_cache() for why the cache
*  is not a command of its own. With a name it asks; with nothing, "-c" or
*  "-f" it shows or empties what has been asked already. */
void nslookup(char *cmd)
{
	char name[NET_DNS_NAME];
	net_dns_answer answer;
	uint32_t address;
	int entries;

	if(prmc(cmd) == 0)
	{
		net_dns_show_cache();
		return;
	}

	if(prmc(cmd) != 1)
	{
		printf("Syntax: nslookup [NAME|-c|-f]\n");
		printf("\t          With no argument, show the cached answers and\n");
		printf("\t          what the resolver has sent and received\n");
		printf("\tNAME      Ask the DNS server for the address of NAME,\n");
		printf("\t          for example nslookup example.com\n");
		printf("\t-c        Show the cache, as with no argument at all\n");
		printf("\t-f        Discard every cached answer\n");
		return;
	}

	/* prmv() hands back a pointer into one static buffer, so the name is
	   copied out before anything else can call prmv() again. */
	strcpy(name, prmv(1, cmd));

	if(strcmp(name, "-c") == 0)
	{
		net_dns_show_cache();
		return;
	}

	if(strcmp(name, "-f") == 0)
	{
		entries = dns_cache_entries();
		dns_cache_flush();

		if(entries <= 0)
		{
			printf("nslookup: the cache was already empty.\n");
			return;
		}

		printf("nslookup: %i cached answer(s) discarded.\n", entries);
		printf("          The next lookup of any of them asks the server\n");
		printf("          again, which is the point of emptying it.\n");
		return;
	}

	/* An argument that is already an address. Refused rather than sent,
	   because sending it would fail in a way that reads like the network is
	   broken: "10.0.2.2" is a perfectly legal name to ask for and the server
	   would answer that there is no such thing. The reverse lookup that
	   would actually answer the question does not exist here -- this
	   resolver asks for A records only -- so the honest thing is to say that
	   and point at the command an address IS good for. */
	if(net_parse_ip(name, &address))
	{
		printf("nslookup: %s is already an address.\n", name);
		printf("  There is nothing to look up. This resolver asks for A\n");
		printf("  records -- name to address -- and has no reverse lookup, so\n");
		printf("  an address cannot be turned back into a name here. Asking\n");
		printf("  for it as a name would only be told there is no such name.\n");
		printf("  \"ping %s\" is what an address is for.\n", name);
		return;
	}

	if(!net_interface_ready("nslookup", 1)) return;

	printf("Looking up %s.\n", name);

	if(!net_dns_resolve("nslookup", name, &answer)) return;

	net_info_label("Address:");
	net_print_ip(answer.ip);
	printf("\n");

	/* The TTL is the reason this command reports more than an address: it is
	   what says whether the answer will still be true in a minute. A cached
	   one is what is LEFT of it, which is a different number from the one
	   the server stated, so the two do not read alike. */
	net_info_label("TTL:");
	net_duration(answer.ttl);
	if(answer.cached) printf(" left");
	printf("\n");

	/* Where it came from. A cache hit and a fresh query are different things
	   to somebody debugging a name that keeps moving, and only one of them
	   proves the server is still answering. */
	net_info_label("Answer:");
	if(answer.cached)
	{
		printf("from the cache -- nothing was sent\n");
	} else {
		printf("from ");
		net_print_ip(dhcp_dns());
		printf(" in %i ms\n", answer.ms);
	}
}

void netstat(char *cmd)
{
	/* No arguments and no options. There is nothing here to filter -- four
	   connections at most, and every one of them is worth seeing when the
	   question is why a transfer has stopped. */
	if(prmc(cmd) != 0)
	{
		printf("Syntax: netstat\n");
		printf("\t          Show the open TCP connections and the TCP counters\n");
		return;
	}

	net_show_tcp();
}

/* The other half of what can be typed: the programs in BIN_DIR.
*
*  Read off the disk rather than written out here, and that is worth the
*  twenty lines it costs. A hardcoded list would be a promise this file cannot
*  keep -- the programs are separate binaries now, added by dropping a name
*  into the Makefile, and the day somebody does that the kernel would have to
*  be edited too or "help" would start lying. Worse, it would go on listing
*  "ls" on a machine that booted without a disk, which is precisely the
*  situation where the difference between a built-in command and a program
*  becomes visible and the user needs to be told about it.
*
*  So the listing is the directory, and when there is no directory it says so
*  and why. The names lose their extension because that is how they are
*  typed -- run_path() puts it back. */
static void help_programs(void)
{
	fat_dirent ent;
	char name[FAT_NAME_MAX];
	int index;
	int shown;
	int len;
	int ext;
	int i;
	int rc;

	printf("Programs in %s, typed by name without the extension:\n", BIN_DIR);

	if(!fat_mounted())
	{
		printf("\tNone are reachable -- no filesystem is mounted.\n");
		fs_explain_unmounted();
		return;
	}

	shown = 0;
	rc = 0;
	ext = (int)strlen(BIN_EXT);

	for(index = 0; index < BIN_MAX_ENTRIES; index++)
	{
		rc = fat_readdir(BIN_DIR, index, &ent);

		/* 1 means the directory is exhausted, the normal way out. Anything
		   negative -- including "no such directory" on a volume that has no
		   /BIN at all -- ends the walk and is explained below. */
		if(rc != 0) break;

		if(ent.is_dir) continue;

		/* Only the files this mechanism can actually start. A README in
		   there is not a command and must not be listed as one. */
		len = (int)strlen(ent.name);
		if(len <= ext) continue;
		if(strcmp(ent.name + (len - ext), BIN_EXT) != 0) continue;

		/* The name as it is typed: extension off, lower case back on. */
		len = len - ext;
		if(len > (int)sizeof(name) - 1) len = (int)sizeof(name) - 1;
		for(i = 0; i < len; i++)
		{
			name[i] = (ent.name[i] >= 'A' && ent.name[i] <= 'Z')
			          ? (char)(ent.name[i] - 'A' + 'a') : ent.name[i];
		}
		name[len] = EOS;

		if((shown % BIN_PER_LINE) == 0) printf("\t");
		ps_print_left(name, BIN_NAME_WIDTH);
		shown++;
		if((shown % BIN_PER_LINE) == 0) printf("\n");
	}

	if((shown % BIN_PER_LINE) != 0) printf("\n");

	if(shown == 0)
	{
		printf("\tNone -- %s holds no %s file.\n", BIN_DIR, BIN_EXT);
		if(rc < 0) printf("\t%s\n", fat_last_error());
		return;
	}

	printf("  %i program(s), run with their arguments as typed.\n", shown);
}

void help(void)
{
	printf("TomatOS Help\n");

	/* Two lists, because there are now two kinds of command and the
	   difference matters exactly once: when the disk is missing. Typing them
	   feels identical, but only the first list is in the kernel and works
	   with no volume mounted -- so they are not run together into one
	   "available commands" that would be wrong half the time. */
	printf("Built into the kernel, always available:\n");
	printf("\thelp      Show this overview\n");
	printf("\ttaskmgr   List and control tasks\n");
	printf("\tstart     Start a test task\n");
	printf("\tmem       Show memory usage, mem -t tests the heap\n");
	printf("\tpage      Show paging state, page -t tests paging\n");
	printf("\tuser      Ring 3 demo, user -t tests the system calls,\n");
	printf("\t          user -i tests the address space isolation\n");
	printf("\tps        List the loaded modules and the running tasks\n");
	printf("\texec      exec NAME runs the module NAME as a ring 3 task\n");
	printf("\tdf        Show the mounted filesystem and the drives found\n");
	printf("\tgfx       Draw the graphics demo, gfx -t tests the surface\n");
	printf("\t          it draws on, gfx -i shows the mode booted into\n");
	printf("\tlspci     List the devices the PCI enumeration found\n");
	printf("\tlsusb     Show the USB controller, whether its schedule is\n");
	printf("\t          running, and the devices that were enumerated\n");
	printf("\tifconfig  Show the interface and its counters, ifconfig -q\n");
	printf("\t          shows the receive queue the card's interrupt fills,\n");
	printf("\t          ifconfig IP NETMASK GATEWAY sets the addresses\n");
	printf("\tdhcp      Ask the network for an address instead of typing\n");
	printf("\t          one, dhcp -f asks again when one is already set\n");
	printf("\tarp       Show the ARP cache, address to hardware address\n");
	printf("\tnslookup  nslookup NAME asks the DNS server for its address,\n");
	printf("\t          nslookup alone or -c shows the cache, -f empties it\n");
	printf("\tping      ping HOST sends echo requests and times the replies,\n");
	printf("\t          HOST is an address or a name to look up first\n");
	printf("\tnetstat   Show the open TCP connections, the state each one is\n");
	printf("\t          in, and what the TCP layer has sent and resent\n");
	printf("\tmouse     Watch the pointer move and the buttons go down until\n");
	printf("\t          a key is pressed, mouse -i shows the same numbers\n");
	printf("\t          once, mouse -t tests what needs no hand on it, and\n");
	printf("\t          mouse -b W H sets the field it may move in\n");
	printf("\treboot    Restart the computer\n");
	printf("\texit      Exit the shell\n");

	printf("\n");
	help_programs();
}
