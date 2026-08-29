
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
#include <multiboot.h>
#include <pci.h>
#include <rtl8139.h>
#include <net.h>
#include <dhcp.h>
#include <dns.h>
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

/* Size of the title, in whole font pixels per glyph pixel. */
#define GFX_TITLE_SCALE  3

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

/* The text screen is 80 by 25 cells of one character plus one attribute. */
#define GFX_TEXT_CELLS   (80 * 25)

/* Width of the label column of "gfx -i", counted from the start of the line
*  including the two leading spaces. The labels below carry their padding
*  literally, the way every other table in this file does -- printf() has no
*  field widths, so a "%-15s" is not available and would only hide where the
*  column actually is. */
#define GFX_INFO_LABEL   17

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

/* One request, and how long its reply may take. Polled in 10 ms steps
*  because icmp_last_reply() is a mailbox the interrupt writes into, not
*  something that can be waited on. */
#define NET_PING_COUNT    4   /* echo requests per run                    */
#define NET_PING_WAIT  1000   /* ms to wait for one reply                 */
#define NET_PING_POLL    10   /* ms between two looks at the mailbox      */
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
*  first message on the wire, and every answer after that arrives in an
*  interrupt while this task sleeps. So the command is a loop that sleeps,
*  calls dhcp_poll() -- which is the only thing that resends a message
*  nobody answered -- and watches dhcp_state() move.
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
*  50 ms between two looks is fine even though a virtual network answers far
*  faster than that: the states are ordered, so a poll that finds two of them
*  crossed at once can still print both -- see net_dhcp_run(). */
#define DHCP_WAIT     25000   /* ms before the shell stops waiting        */
#define DHCP_POLL        50   /* ms between two calls to dhcp_poll()      */

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
*  query on the wire and returns, the answer arrives in the card's interrupt
*  while this task sleeps, so the command is a loop that sleeps, calls
*  dns_poll() -- the only thing that resends and the only thing that ever
*  gives up -- and watches dns_state().
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
*  that it is a backstop rather than a second, competing deadline. */
#define DNS_WAIT      15000   /* ms before the shell stops waiting        */
#define DNS_POLL         50   /* ms between two calls to dns_poll()       */

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
void netconfig(char *cmd);
void dhcpclient(char *cmd);
void arptable(char *cmd);
void pinghost(char *cmd);
void nslookup(char *cmd);
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
static int  gfx_enter(void);
static int  gfx_leave(void);
static void gfx_setup_palette(void);
static void gfx_char_scaled(int x, int y, char c, int scale, uint8_t colour);
static void gfx_string_scaled(int x, int y, const char *s, int scale,
                              uint8_t colour);
static void gfx_string_centred(int cx, int y, const char *s, uint8_t colour);
static void gfx_panel(int x, const char *label);
static void gfx_draw_picture(void);
static void gfx_show(void);
static void gfx_selftest(void);
static void gfx_check(int ok);
static const char *gfx_mode_kind(void);
static void gfx_info_label(const char *label);
static void gfx_show_mode(void);

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
		else if(strcmp(word, "ifconfig") == 0) netconfig(cmd);
		else if(strcmp(word, "dhcp") == 0) dhcpclient(cmd);
		else if(strcmp(word, "arp") == 0) arptable(cmd);
		else if(strcmp(word, "ping") == 0) pinghost(cmd);
		else if(strcmp(word, "nslookup") == 0) nslookup(cmd);
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
	uint32_t total;
	uint32_t avail;
	uint32_t sectors;
	int drives;
	int drive;

	printf("Filesystem:\n");

	if(!fat_mounted())
	{
		printf("  Nothing is mounted -- no path can be read.\n");
	} else {
		total = fat_total_bytes();
		avail = fat_free_bytes();
		label = fat_label();

		printf("  Type:          %s\n", fat_type());
		printf("  Label:         %s\n", label[0] == EOS ? "(none)" : label);

		printf("  Total:   ");
		mem_print_right(total, 12);
		printf(" bytes (");
		mem_print_right(total / 1024, 8);
		printf(" KiB)\n");

		printf("  Used:    ");
		mem_print_right(total - avail, 12);
		printf(" bytes (");
		mem_print_right((total - avail) / 1024, 8);
		printf(" KiB)\n");

		printf("  Free:    ");
		mem_print_right(avail, 12);
		printf(" bytes (");
		mem_print_right(avail / 1024, 8);
		printf(" KiB)\n");

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
		state = taskmgr_task_state(pid);

		/* ABORTED is where a task that called exit() ends up -- sys_exit()
		   goes through taskmgr_task_abort(), so a clean return and a fault
		   look the same from here, and to a caller that only has to stop
		   waiting they are the same. NULL is a slot that names no task at
		   all. Everything else means it is still there. */
		if(state == TASK_STATE_ABORTED || state == TASK_STATE_NULL) return;

		elapsed = timer_get_ticks() - start;
		if(elapsed >= RUN_WAIT_MS || elapsed < 0) break;

		sleep(RUN_POLL_MS);
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
*  What leaving text mode really costs, and how the two halves of this
*  section pay for it.
*
*  1. The video RAM is one memory. In mode 13h the framebuffer at 0xA0000 is
*     chained across all four planes, so the first eight thousand pixels of
*     the picture land on exactly the bytes that hold the characters and the
*     attributes of the 80x25 screen. Drawing anything at all therefore
*     destroys the console. vga_set_mode() saves and restores the font (see
*     the header), which is a different plane and a different problem; the
*     visible text is this file's to keep, so gfx_text_store() takes a copy of
*     the 2000 cells before the switch and gfx_text_recall() writes it back
*     after the way home. That is what makes the shell come back with its
*     scrollback rather than with the rubble of the picture.
*
*  2. The status bar task keeps running. It writes 80 cells into 0xB8000
*     every ten ticks, and 0xB8000 is not part of the framebuffer: mode 13h
*     leaves the graphics controller's memory map at "0xA0000, 64 KiB", so
*     the hardware discards those writes outright. The picture is safe from
*     the bar even if nothing is done about it -- but the bar is not safe
*     from the picture: its output during that window is simply lost, and
*     the moment the memory map were ever set to the 128 KiB window instead,
*     the same writes would land in video RAM behind the visible page. The
*     task is therefore suspended for the duration and started again
*     afterwards; it repaints itself within ten ticks on its own.
*
*  3. The same trap catches printf(). Anything printed between the two mode
*     switches goes to 0xB8000, is thrown away by the hardware, and still
*     advances the console cursor and scrolls the screen the copy in
*     gfx_text_page[] no longer matches. So neither half of this section
*     prints a single character while the picture is up: the self-test
*     collects its results in local variables and reports them once the mode
*     is back. */

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
*  like every other access to that buffer in the kernel. */
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

/* Everything that has to happen before the first pixel: nothing else may
*  write to the screen, and the screen has to be saved. Returns what
*  vga_set_mode() returned, and on failure leaves the system exactly as it
*  was found. */
static int gfx_enter(void)
{
	int rc;

	gfx_statusbar_hold();
	gfx_text_store();

	rc = vga_set_mode(VGA_MODE_GRAPHICS);

	if(rc != 0)
	{
		gfx_statusbar_release();
	}

	return rc;
}

/* And the way back, in the opposite order: the mode first, because the
*  console buffer can only be written once it is a console buffer again. */
static int gfx_leave(void)
{
	int rc;

	rc = vga_set_mode(VGA_MODE_TEXT);
	gfx_text_recall();
	gfx_statusbar_release();

	return rc;
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

	vga_palette(GFX_BLACK,      0,  0,  0);
	vga_palette(GFX_WHITE,     63, 63, 63);
	vga_palette(GFX_TEXT,      44, 46, 54);
	vga_palette(GFX_AMBER,     63, 46, 12);
	vga_palette(GFX_SHADOW,     6,  5, 12);
	vga_palette(GFX_FRAME,     26, 30, 46);
	vga_palette(GFX_PANEL,      8,  9, 20);
	vga_palette(GFX_LEAF,      26, 50, 22);
	vga_palette(GFX_LEAF_DARK, 12, 32, 14);
	vga_palette(GFX_STEM,      18, 40, 16);

	/* The background: night blue at the top, warming towards the bottom.
	   All integer -- there is no floating point in this kernel, and a ramp
	   never needs any: the step is start + i * span / steps. */
	for(i = 0; i < GFX_SKY_STEPS; i++)
	{
		vga_palette((uint8_t)(GFX_SKY_FIRST + i),
		            (uint8_t)(3 + (i * 18) / GFX_SKY_STEPS),
		            (uint8_t)(4 + (i * 10) / GFX_SKY_STEPS),
		            (uint8_t)(12 + (i * 14) / GFX_SKY_STEPS));
	}

	/* The tomato, from a nearly black rim to a lit skin. */
	for(i = 0; i < GFX_TOMATO_STEPS; i++)
	{
		vga_palette((uint8_t)(GFX_TOMATO_FIRST + i),
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

		vga_palette((uint8_t)(GFX_SPECTRUM_FIRST + i),
		            (uint8_t)r, (uint8_t)g, (uint8_t)b);
	}
}

/* One glyph of the built-in font, blown up by an integer factor. The font is
*  part of the contract in vga.h -- one byte per row, most significant bit
*  leftmost -- so a set bit simply becomes a scale by scale block. */
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
				vga_fill(x + col * scale, y + row * scale,
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

/* Draws a string so that its middle sits on cx. The same hand counting the
*  tables in this file do, only in pixels instead of columns. */
static void gfx_string_centred(int cx, int y, const char *s, uint8_t colour)
{
	int width;

	width = (int)strlen(s) * FONT_WIDTH;

	vga_string(cx - width / 2, y, s, colour, GFX_TRANSPARENT);
}

/* The background and the outline of one of the three panels, with its label
*  centred along the bottom edge. */
static void gfx_panel(int x, const char *label)
{
	vga_fill(x, GFX_PANEL_Y, GFX_PANEL_W, GFX_PANEL_H, GFX_PANEL);
	vga_rect(x, GFX_PANEL_Y, GFX_PANEL_W, GFX_PANEL_H, GFX_FRAME);

	gfx_string_centred(x + GFX_PANEL_W / 2, GFX_PANEL_Y + GFX_LABEL_DY,
	                   label, GFX_TEXT);
}

static void gfx_draw_picture(void)
{
	int i;
	int cx;
	int cy;

	/* Background: one horizontal line per row, one palette entry per band.
	   64 entries over 200 rows means each colour covers three or four rows,
	   which at this size reads as a smooth wash. */
	for(i = 0; i < VGA_HEIGHT; i++)
	{
		vga_hline(0, i, VGA_WIDTH,
		          (uint8_t)(GFX_SKY_FIRST + (i * GFX_SKY_STEPS) / VGA_HEIGHT));
	}

	/* Header bar and the title, with a drop shadow behind it. */
	vga_fill(0, 0, VGA_WIDTH, GFX_HEAD_H, GFX_PANEL);
	vga_hline(0, GFX_HEAD_H, VGA_WIDTH, GFX_AMBER);
	vga_hline(0, GFX_HEAD_H + 1, VGA_WIDTH, GFX_SHADOW);

	gfx_string_scaled(13, 4, "TomatOS", GFX_TITLE_SCALE, GFX_SHADOW);
	gfx_string_scaled(11, 2, "TomatOS", GFX_TITLE_SCALE, GFX_AMBER);

	vga_string(222, 5, "320x200x256", GFX_TEXT, GFX_TRANSPARENT);
	vga_string(246, 15, "MODE 13H", GFX_AMBER, GFX_TRANSPARENT);

	/* --- panel 1: a shaded ball out of nested discs ---------------------
	   Each disc is a little smaller than the last and its centre moves up
	   and to the left, so the ramp lays itself down as light falling from
	   that corner. Sixteen steps of two pixels cover the radius. */
	gfx_panel(GFX_PANEL1_X, "vga_disc");

	cx = GFX_TOMATO_CX;
	cy = GFX_TOMATO_CY;

	for(i = 0; i < GFX_TOMATO_STEPS; i++)
	{
		vga_disc(cx - i / 2, cy - i / 2, GFX_TOMATO_R - 2 * i,
		         (uint8_t)(GFX_TOMATO_FIRST + i));
	}

	vga_circle(cx, cy, GFX_TOMATO_R, GFX_TOMATO_FIRST);
	vga_disc(cx - 12, cy - 12, 3, GFX_WHITE);

	/* Stem and calyx on top of the finished ball. Every leaf is drawn twice,
	   one pixel apart, because a single Bresenham line is one pixel thin and
	   disappears against the skin at this size. */
	vga_vline(cx, cy - 44, 14, GFX_STEM);
	vga_vline(cx + 1, cy - 44, 14, GFX_LEAF_DARK);
	vga_disc(cx, cy - 30, 6, GFX_LEAF_DARK);

	for(i = 0; i < 2; i++)
	{
		vga_line(cx, cy - 30 + i, cx - 15, cy - 36 + i, GFX_LEAF);
		vga_line(cx, cy - 30 + i, cx + 15, cy - 36 + i, GFX_LEAF);
		vga_line(cx, cy - 30 + i, cx - 11, cy - 21 + i, GFX_LEAF);
		vga_line(cx, cy - 30 + i, cx + 11, cy - 21 + i, GFX_LEAF);
	}

	/* --- panel 2: a fan of lines and a pair of rectangles ---------------
	   The fan starts in one corner and ends on the opposite two edges, so
	   the slopes run from steeper than vertical round to shallower than
	   horizontal -- every case a line routine has to get right. */
	gfx_panel(GFX_PANEL2_X, "vga_line");

	for(i = 0; i < 9; i++)
	{
		vga_line(118, 100, 118 + i * 11, 40,
		         (uint8_t)(GFX_SPECTRUM_FIRST + i * 16));
	}

	for(i = 1; i < 6; i++)
	{
		vga_line(118, 100, 206, 40 + i * 11,
		         (uint8_t)(GFX_SPECTRUM_FIRST + GFX_SPECTRUM_STEPS - i * 16));
	}

	/* Filled next to outlined, the same size, so the difference between
	   vga_fill() and vga_rect() is there to be seen. */
	vga_fill(122, 106, 36, 16, GFX_AMBER);
	vga_rect(166, 106, 36, 16, GFX_WHITE);

	/* --- panel 3: rings and discs --------------------------------------- */
	gfx_panel(GFX_PANEL3_X, "vga_circle");

	for(i = 5; i >= 1; i--)
	{
		vga_circle(264, 74, i * 6,
		           (uint8_t)(GFX_SPECTRUM_FIRST + (5 - i) * 28));
	}

	for(i = 0; i < 3; i++)
	{
		vga_disc(240 + i * 24, 114, 8,
		         (uint8_t)(GFX_SPECTRUM_FIRST + 20 + i * 44));
	}

	/* --- the palette band ------------------------------------------------
	   144 entries, two pixels each, framed. This is the part that shows at
	   a glance that there really are more than sixteen colours here. */
	vga_rect(GFX_BAND_X - 1, GFX_BAND_Y - 1, GFX_BAND_W + 2, GFX_BAND_H + 2,
	         GFX_FRAME);

	for(i = 0; i < GFX_SPECTRUM_STEPS; i++)
	{
		vga_fill(GFX_BAND_X + i * 2, GFX_BAND_Y, 2, GFX_BAND_H,
		         (uint8_t)(GFX_SPECTRUM_FIRST + i));
	}

	gfx_string_centred(VGA_WIDTH / 2, GFX_CAPTION_Y,
	                   "256-colour DAC, 144-step hue sweep", GFX_TEXT);

	/* --- footer ---------------------------------------------------------- */
	vga_fill(0, GFX_FOOT_Y, VGA_WIDTH, VGA_HEIGHT - GFX_FOOT_Y, GFX_PANEL);
	vga_hline(0, GFX_FOOT_Y, VGA_WIDTH, GFX_AMBER);

	gfx_string_centred(VGA_WIDTH / 2, GFX_FOOT_TEXT_Y,
	                   "Press any key to return to the shell", GFX_WHITE);

	/* Border last, so nothing drawn over it can eat the corners. */
	vga_rect(0, 0, VGA_WIDTH, VGA_HEIGHT, GFX_FRAME);
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

	if(rc != 0)
	{
		printf("Graphics mode could not be entered, vga_set_mode() = %i\n", rc);
		return;
	}

	gfx_setup_palette();
	gfx_draw_picture();

	/* The keyboard IRQ carries on running in graphics mode -- nothing about
	   the mode touches the PIC or the controller -- so the ordinary blocking
	   read is all that is needed here. */
	getch();

	rc = gfx_leave();

	if(rc != 0)
	{
		printf("Text mode could not be restored, vga_set_mode() = %i\n", rc);
		return;
	}

	printf("Back in text mode, %i by %i pixels drawn.\n",
	       VGA_WIDTH, VGA_HEIGHT);
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
*  Both halves are printed, and the last line says how they came out.
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

static void gfx_selftest(void)
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

	gfx_tests_run = 0;
	gfx_tests_ok = 0;

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

	printf("Graphics self-test:\n");
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
		printf("\t-t        Run the graphics mode self-test\n");
		printf("\t-i        Show the mode the machine booted into\n");
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
*  not working than any status word does. */
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
	printf("\n");

	printf("  TX packets: ");
	mem_print_right(net_tx_packets(), 8);
	printf("\n");

	/* The UDP counters belong here, one layer up though they are: this
	   command is where one looks when the network does not work, and since
	   DHCP the interesting failure is a datagram that the card counted and
	   UDP then dropped -- a wrong checksum, a port nobody bound. Two rows of
	   the same three columns as above show that difference at a glance, and
	   they are cheap: four lines that are read together. */
	printf("  UDP     rx: ");
	mem_print_right(udp_rx_packets(), 8);
	printf("   dropped: ");
	mem_print_right(udp_rx_dropped(), 8);
	printf("\n");

	printf("  UDP     tx: ");
	mem_print_right(udp_tx_packets(), 8);
	printf("\n");

	/* A row of zeroes explains nothing by itself, so when the interface
	   cannot carry a packet it says why underneath. */
	if(!rtl8139_present() || !net_up() || !configured)
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
*  2. The reply is not returned by anything. It arrives in interrupt context
*     while this task sleeps and is left in icmp_last_reply(), which is
*     therefore polled -- and matched on both the identifier and the sequence
*     number that were sent, so that neither somebody else's ping nor an old
*     reply of our own is counted as an answer to this request. */
static int net_ping_once(uint32_t dst, uint16_t sequence, uint32_t *from)
{
	uint32_t reply_from;
	uint16_t reply_id;
	uint16_t reply_sequence;
	int start;
	int waited;
	int elapsed;
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

	for(tries = 0; rc != 0 && tries < NET_ARP_TRIES; tries++)
	{
		if(tries == 0)
		{
			printf("resolving ");
			net_print_ip(dst);
			printf(" ... ");
		}

		sleep(NET_ARP_WAIT);
		rc = icmp_send_echo(dst, NET_PING_ID, sequence);
	}

	if(rc != 0) return NET_PING_UNRESOLVED;

	start = timer_get_ticks();

	for(waited = 0; waited < NET_PING_WAIT; waited += NET_PING_POLL)
	{
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

		sleep(NET_PING_POLL);
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
*  Built like net_ping_once() and for the same reason -- nothing here can
*  wait on a packet, because every answer arrives in interrupt context while
*  this task sleeps. So the loop sleeps, calls dhcp_poll() (the only thing
*  that resends a message nobody answered, and the only thing that ever gives
*  up) and watches dhcp_state().
*
*  The state IS the progress report: each of the four messages moves it, so
*  printing every change shows the exchange happening. A spinner would show
*  only that the machine is still alive, and the question a user with no
*  address actually has is which of the four steps it is stuck on. */
static void net_dhcp_run(void)
{
	uint32_t leased;
	const char *reason;
	int state;
	int shown;
	int waited;
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

	for(waited = 0; waited < DHCP_WAIT; waited += DHCP_POLL)
	{
		state = dhcp_poll();

		if(state == DHCP_STATE_FAILED) break;

		/* The states before BOUND are numbered in the order they happen in,
		   which is what makes this loop safe at 50 ms: on a virtual network
		   the OFFER and the ACK can both arrive inside one sleep, and the
		   client would then be found two steps further on than it was left.
		   Printing only where it is now would silently drop a step that did
		   happen, so every step in between is printed as well. */
		while(shown < state)
		{
			shown++;
			net_dhcp_step(shown);
		}

		if(state == DHCP_STATE_BOUND) break;

		sleep(DHCP_POLL);
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
	int state;
	int waited;
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

	for(waited = 0; waited < DNS_WAIT; waited += DNS_POLL)
	{
		state = dns_poll();

		if(state == DNS_STATE_DONE || state == DNS_STATE_FAILED) break;

		sleep(DNS_POLL);
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

	if(prmc(cmd) != 3)
	{
		printf("Syntax: ifconfig [IP NETMASK GATEWAY]\n");
		printf("\t          Show the interface, its addresses and counters\n");
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
	printf("\tgfx       Draw the graphics demo, gfx -t tests mode 13h,\n");
	printf("\t          gfx -i shows the mode the machine booted into\n");
	printf("\tlspci     List the devices the PCI enumeration found\n");
	printf("\tifconfig  Show the interface and its counters,\n");
	printf("\t          ifconfig IP NETMASK GATEWAY sets the addresses\n");
	printf("\tdhcp      Ask the network for an address instead of typing\n");
	printf("\t          one, dhcp -f asks again when one is already set\n");
	printf("\tarp       Show the ARP cache, address to hardware address\n");
	printf("\tnslookup  nslookup NAME asks the DNS server for its address,\n");
	printf("\t          nslookup alone or -c shows the cache, -f empties it\n");
	printf("\tping      ping HOST sends echo requests and times the replies,\n");
	printf("\t          HOST is an address or a name to look up first\n");
	printf("\treboot    Restart the computer\n");
	printf("\texit      Exit the shell\n");

	printf("\n");
	help_programs();
}
