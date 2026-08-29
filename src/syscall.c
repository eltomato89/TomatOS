/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: System call implementation -- the kernel side of "int 0x80"
*
*  The assembly stub in start.asm saves the register set and hands it here.
*  eax holds the call number, ebx/ecx/edx/esi the arguments, and the result is
*  written back into r->eax so that the iret delivers it to the caller. esi
*  joined the set for SYS_READ, whose (path, offset, len, buffer) genuinely
*  does not fit in three registers.
*
*  Everything in this file runs on behalf of ring 3, and -- just as important
*  since tasks own separate page directories -- it runs *in the caller's
*  address space*. Entering through a gate changes cs, ss and esp, never CR3.
*  That makes this the one place where the kernel touches values it did not
*  produce itself, so every argument is treated as hostile until it has been
*  checked -- see user_byte_ok() and user_string_len() below.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <vmm.h>
#include <mm.h>
#include <syscall.h>
#include <fat.h>
#include <ata.h>
#include <net.h>
#include <tcp.h>
#include <dns.h>
#include <dhcp.h>
#include <fbcon.h>
#include <mouse.h>

/* The stub in start.asm. It pushes a dummy error code and the interrupt
*  number, saves the registers and calls syscall_handler(). */
extern void syscall_stub();

/* settextcolor() lives in scrn.c but has no prototype in any header yet.
*  Declared locally, exactly as isrs.c does it. */
extern void settextcolor(unsigned char forecolor, unsigned char backcolor);

/* getchn() lives in kb.c next to getch(), but only getch() made it into
*  system.h. It takes whatever key is pending and hands back 0 when there is
*  none -- the non-blocking read SYS_PEEKCH needs, and the reason that call
*  does not have to invent one. Declared locally for the same reason as
*  settextcolor() above; kb.c is not this file's to edit. */
extern unsigned char getchn(void);

/* The loader side of SYS_SPAWN. Declared here rather than included from
*  <exec.h> because that header is being extended in parallel; the prototype
*  is the whole contract this file needs -- load the program at path as a ring
*  3 task of its own, give it args as its command line, run it at prio, and
*  return the new pid or a negative value. */
extern int exec_spawn_path(const char *path, const char *args, int prio);

/* Longest string SYS_WRITE accepts, terminator included. A user pointer may
*  point at memory that simply never contains a zero byte, so the scan needs
*  an upper bound -- without one, ring 3 could park the kernel in a loop
*  inside an interrupt handler forever. */
#define SYS_WRITE_MAX 1024

/* Longest path the filesystem calls accept, and longest command line
*  SYS_SPAWN passes on, terminators included. Both bounds exist for the same
*  reason as SYS_WRITE_MAX -- a user "string" is only a string once someone
*  has proved it ends -- but they are much tighter, because these two are also
*  the size of a buffer.
*
*  The buffer lives on the kernel stack, which is KERNEL_STACK_SIZE (4 KiB)
*  per task in tasks.c and also has to carry every interrupt frame that lands
*  while the call runs. A static buffer would cost nothing there and would be
*  wrong: vector 0x80 is a trap gate, the timer preempts a call in progress,
*  and a second task can be inside this very handler before the first one has
*  finished -- one shared buffer between them is a data race with a filename
*  in it. 64 bytes is roomy for the 8.3 names fat.c deals in, nested a few
*  directories deep, and nothing more. */
#define SYS_PATH_MAX 64
#define SYS_ARGS_MAX 128

/* Most bytes a single SYS_READ may move.
*
*  Same argument as SYS_WRITE_MAX, one layer down: the length comes from ring
*  3, this runs inside an interrupt handler, and fat_read() reaches the disk
*  through PIO, one 16 bit word per inb. An uncapped length would let a user
*  program decide how long the kernel spends in a single trap, which is a
*  denial of service with no exploit needed -- just a big number.
*
*  A page is the natural bound: it is one mapping, one validation walk, and it
*  keeps the call cheap enough that the timer tick that preempts it is never
*  far away. Larger requests are answered short rather than refused, which is
*  the ordinary read() contract and lets a caller loop without knowing the
*  limit. */
#define SYS_READ_MAX 4096

/* Most bytes a single SYS_FWRITE may move.
*
*  The same argument as SYS_READ_MAX and the same number, one direction over:
*  the length comes from ring 3, this runs inside an interrupt handler, and
*  fat_write() reaches the disk through PIO. An uncapped length would let a
*  user program decide how long the kernel spends inside a single trap, which
*  is a denial of service that needs no exploit -- just a big number.
*
*  The write side has one extra reason for the cap and one fewer objection to
*  it. The extra reason: a write is not only a transfer. It allocates clusters,
*  rewrites FAT entries in both copies and updates the directory entry, so a
*  page of it is already several sector writes rather than one long read, and
*  it is the operation during which the volume is briefly inconsistent. The
*  absent objection: answering short needs no new contract here, because
*  fat_write() can already return short on its own when the volume fills up.
*  A caller that loops until everything has been written -- the only correct
*  way to use this call -- therefore works without knowing the cap is there. */
#define SYS_FWRITE_MAX 4096

/* Longest host name SYS_RESOLVE accepts, terminator included.
*
*  DNS_NAME_MAX is the protocol's own limit on a whole name, so anything
*  longer is not a name the resolver could encode -- dns_resolve() would refuse
*  it after this file had already copied it. Bounding the copy at the same
*  number means a name that is too long is turned down here, for the reason
*  user_string_len() gives: at that point the argument is not a host name, and
*  guessing where it ends is not the kernel's job.
*
*  256 bytes is four times what SYS_PATH_MAX costs and it lands on the same 4
*  KiB kernel stack, which is why it is worth saying that it fits: the deepest
*  this call goes below itself is dns_resolve(), which works out of static
*  buffers of its own and does not recurse. */
#define SYS_HOST_MAX (DNS_NAME_MAX + 1)

/* Most bytes a single SYS_SEND or SYS_RECV may move.
*
*  Same argument as SYS_READ_MAX, and the same number for the same reason: one
*  page is one mapping, one validation walk, and a bound on how long ring 3 can
*  keep the kernel inside a single trap by passing a big length. It also sits at
*  or below both connection buffers -- TCP_SND_BUF is 4096 and TCP_RCV_BUF is
*  8192 -- so it never promises a transfer the stack below could not perform in
*  one call anyway.
*
*  Larger requests are answered short rather than refused. That is what both
*  calls already have to do (tcp_send() takes what fits, tcp_recv() returns what
*  has arrived), so a caller that loops until it has what it wants works without
*  knowing the cap exists. */
#define SYS_NET_XFER_MAX 4096

/* How long a network call waits before it gives up, in milliseconds, and how
*  long it sleeps between two looks.
*
*  Every one of these five calls blocks, so every one of them needs a number
*  here: a task waiting on a machine that answers nothing must end in
*  SYS_ETIMEDOUT rather than never returning at all. The bounds are backstops
*  wherever the layer below has a schedule of its own -- the useful failure is
*  the one the resolver or the TCP retransmission timer reports, because it
*  knows why -- so each is set above that schedule rather than in competition
*  with it.
*
*  SYS_NET_POLL is the LONGEST one wait may last, not the interval between two
*  looks -- a frame that arrives sooner wakes the wait and the look happens at
*  once. It is still the shell's 50 ms from main.c, and it is still a real
*  number rather than a formality: it is what drives dns_poll() and tcp_poll(),
*  and those two retransmit for connections nobody is waiting on. Nothing wakes
*  a retransmission timer, so this bound is the timer's tick and may not grow
*  into one. See sysnet_pump() for the whole of that argument.
*
*  SYS_NET_RESOLVE_MS covers our own lookup. dns.c sends at most
*  DNS_MAX_ATTEMPTS = 3 queries with a timeout that starts at 1000 ms and
*  doubles, so it concludes on its own after 1 + 2 + 4 = 7 seconds; ten leaves
*  that schedule room to finish and report DNS_STATE_FAILED, which is the answer
*  worth having.
*
*  SYS_NET_RESOLVE_BUSY covers the wait for the resolver to be free at all,
*  which is a separate thing to wait for because dns.c serves one lookup at a
*  time for the whole machine -- see sysnet_dns_take(). Eight seconds tolerates
*  exactly one other lookup running its schedule out in front of us. The two
*  phases add up to eighteen seconds in the worst case, and that case needs a
*  second task resolving at the same moment; a lookup that has the resolver to
*  itself is bounded by the ten.
*
*  SYS_NET_CONNECT_MS covers the handshake. A SYN that is dropped rather than
*  refused is retransmitted on the classic 1/2/4 second schedule, so ten seconds
*  is three attempts plus room for the ARP exchange that has to resolve the
*  gateway before the very first segment of the first connection after boot can
*  leave. Short enough that a wrong address reads as a failure rather than as a
*  hung shell, which a minute would not.
*
*  SYS_NET_SEND_MS is the wait for room in the send buffer. That buffer only
*  stays full while the peer stops acknowledging, so this is not really a
*  bandwidth limit but a liveness one: ten seconds without a single
*  acknowledgement, on a link that normally answers in under a millisecond, is a
*  peer that is gone.
*
*  SYS_NET_RECV_MS is longer than the rest on purpose, because it is the only
*  one waiting on somebody else's thinking. A server that has taken the request
*  and produced nothing at all for fifteen seconds is not about to; fifteen is
*  chosen to sit above a slow origin and below the point where a user decides
*  the machine has crashed. It bounds silence, not the transfer -- each call
*  brings its own fresh budget, so a download that keeps trickling never trips
*  it.
*
*  SYS_NET_CLOSE_MS is short, and that is the interesting one. It waits for our
*  FIN to be acknowledged, which takes a round trip, and NOT for the connection
*  to reach TCP_CLOSED -- TIME_WAIT is twice the maximum segment lifetime and
*  can be a minute, which is exactly the wait that would make close() look like
*  a hang. Two seconds is many round trips on any link this kernel will see. */
#define SYS_NET_POLL             50
#define SYS_NET_RESOLVE_MS    10000
#define SYS_NET_RESOLVE_BUSY   8000
#define SYS_NET_CONNECT_MS    10000
#define SYS_NET_SEND_MS       10000
#define SYS_NET_RECV_MS       15000
#define SYS_NET_CLOSE_MS       2000

/* Largest port number, and the largest handle ring 3 can be given. A handle is
*  its slot in sysnet_conns[] plus one, so that 0 is never a valid handle --
*  see sysnet_slot_of(). */
#define SYS_PORT_MAX          65535
#define SYS_NET_HANDLE_MAX    TCP_MAX_CONNS

/* Where the framebuffer is put in the CALLING TASK's address space.
*
*  This is the first mapping this kernel places in a user half because ring 3
*  asked for one, so the address is a decision rather than a detail -- and the
*  question it has to answer is not "is anything there today" but "can anything
*  ever be there". Three things live in a task's user half and all three are
*  placed by somebody other than this file:
*
*    - THE PROGRAM IMAGE. user/user.ld links every program at 0x00400000 and
*      says at length why; exec.c maps the PT_LOAD segments it finds and
*      nothing else, so an image occupies a contiguous run growing UPWARDS
*      from 4 MiB. A program would have to be 2.7 gigabytes long to reach
*      0xB0000000, on a machine this kernel runs with 32 MiB of RAM.
*    - THE USER STACK. tasks.c parks the stacks directly under the kernel
*      half: USER_STACK_REGION_TOP is 0xBFFFF000 and MAX_TASKS = 64 slots of
*      8 KiB reach down to 0xBFF80000. That is the LOWEST address any stack
*      page can have, with the task table full, and it is 255 MiB above the
*      end of the largest mapping this call can make.
*    - THE ARGUMENT BLOCK. exec.c writes argc, argv and the strings into the
*      top of the stack page it already found -- inside the stack, not next
*      to it -- so it adds no third region to keep clear of.
*
*  0xB0000000 therefore sits in the wide empty middle that user.ld's own
*  comment describes as "almost 3 GiB of untouched address space", 704 MiB
*  above the top of any image and 247 MiB below the lowest stack page even
*  after the 8 MiB bound below. Both distances are checked against the numbers
*  those two files state, not against what a program happens to do today.
*
*  It is aligned to 4 MiB, i.e. to a page directory entry, for the reason
*  user.ld gives for the load address: the mapping starts a page table of its
*  own and shares it with nothing, so the table vmm_destroy_space() eventually
*  frees for it holds nothing else the task owns.
*
*  The one collision arithmetic cannot rule out is a second SYS_MAPFB into the
*  same space, and it does not have to: one owner at a time is enforced below,
*  and the owner's second call is refused before anything is mapped. */
#define SYS_FB_VIRT       0xB0000000u

/* Most bytes SYS_MAPFB will map, and so how far past SYS_FB_VIRT the mapping
*  can reach.
*
*  The largest mode anything can establish on this machine is 1024x768 at
*  32 bpp -- which is a fact about the bootloader rather than a guess, see
*  FBCON_MAX_COLS in fbcon.h -- and that is 3 MiB exactly. Eight leaves room
*  for a card that pads its rows, and it is the number the collision argument
*  above is made with, so a mode this kernel cannot currently produce does not
*  quietly invalidate it.
*
*  A geometry that needs more is refused rather than clipped. Half a screen
*  mapped is a program drawing into memory the card is not scanning out, i.e.
*  a picture that is silently wrong in its lower half, and there is no way for
*  the program to find that out. */
#define SYS_FB_MAX_BYTES  0x00800000u

/* How often the reclaim task looks for a screen whose owner has died, in
*  milliseconds. See sysfb_reclaim_task() for why the task exists at all. 250
*  is a quarter of a second of a black screen in the worst case -- fast enough
*  that a killed program looks like it took the console with it and gave it
*  straight back, and slow enough that four state lookups a second is not a
*  cost worth discussing. */
#define SYS_FB_REAP_MS    250

/* Longest ONE wait inside SYS_INPUT may last, in milliseconds.
*
*  It is not the call's timeout -- the caller gives that -- it is how long the
*  task stays blocked before it looks at the keyboard again, and it exists
*  because this call waits on two devices at once and task_wait() waits on one
*  channel. The mouse has a published channel and wakes it for every packet;
*  the keyboard's is a file static in kb.c with no accessor, and sys_getch()
*  below says why this file does not invent one. So the wait is on the mouse
*  and the keyboard is sampled once per turn, which makes this the keyboard's
*  sampling interval and nothing else.
*
*  20 ms is chosen from both ends. A key held down repeats at about 30 Hz and
*  a typist at speed leaves 100 ms between characters, while kb.c holds ONE
*  key -- so the sample has to be well below the gap between two keystrokes or
*  the second one overwrites the first before anybody takes it. And 20 ms is
*  below a 60 Hz frame, so a keystroke never costs a drawn frame of latency.
*  The price is 50 wakeups a second in a task that is otherwise asleep, which
*  is the same order as SYS_NET_POLL's 20 and is nothing next to the thousand
*  ticks the timer produces anyway. */
#define SYS_INPUT_POLL    20

/* Upper bound for the schedule() calls in sys_exit(), same reasoning as
*  FAULT_SCHEDULE_TRIES in isrs.c: schedule() only switches once the running
*  task's time slice is used up, and the loop must not be able to spin. */
#define SYSCALL_SCHEDULE_TRIES 64

/* Number of calls served since boot, reported by syscall_count(). */
static uint32_t syscall_calls = 0;

/* A handler reads its arguments out of the saved register set and returns
*  the value for eax. */
typedef int (*syscall_fn)(struct regs *r);


/* ------------------------------------------------------------------ */
/* Argument validation                                                 */
/* ------------------------------------------------------------------ */

/* Non-zero if a single byte at addr may be read on behalf of ring 3.
*
*  "Is this address readable" only has an answer relative to an address
*  space, and the space that counts is the caller's. It is also the active
*  one: the CPU did not reload CR3 on the way in, so the directory the checks
*  below walk is the very directory the calling task was running under. That
*  is what makes the question well posed at all now that tasks no longer
*  share one directory.
*
*  Three rules. The first and the last are the ones that decide the answer
*  today; the middle one is the one that no longer has to, and says below why
*  it is kept regardless:
*    - address zero and the rest of the first page are rejected outright.
*      Page zero is deliberately never mapped (see vmm.h), so a null pointer
*      would fault anyway -- but faulting inside the kernel with a user
*      pointer is precisely what we are trying not to do.
*    - anything at or above KERNEL_VIRTUAL_BASE belongs to the kernel and is
*      turned down on sight. This used to be load bearing in the plainest
*      possible way: the kernel half is shared by every address space, and it
*      was not free of PAGE_USER, because main.c opened the kernel text pages
*      around the ring 3 demo so the demo could be executed where it was
*      linked. Those pages passed a PAGE_USER test, and without this line a
*      pointer into one of them would have let ring 3 have SYS_WRITE read
*      kernel memory out loud. That is over: the ring 3 code lives in its own
*      section now and is copied into a user page below the line before it is
*      entered, so no page above KERNEL_VIRTUAL_BASE carries PAGE_USER any
*      more and the permission test below would already reject a kernel
*      pointer at the directory entry.
*
*      The line stays anyway, and not out of sentiment. vmm_is_user_mapped()
*      answers what the page tables happen to say; this line states what the
*      kernel means -- an argument from ring 3 lives in the user half, full
*      stop. That is a property this file can hold on its own, whereas the
*      other one is a promise made in main.c and in every future line that
*      calls vmm_map(). The history above is exactly the case of that promise
*      being broken, and the cost of not trusting it is one compare on a path
*      that is about to walk two levels of page tables anyway. It also keeps
*      the pointer arithmetic honest: with addr below 0xC0000000, addr plus a
*      length of at most SYS_WRITE_MAX cannot wrap around the end of the
*      address space, so user_string_len() may walk a string upwards without
*      a separate overflow check.
*    - the page must be present *and* carry PAGE_USER in both the directory
*      entry and the table entry, which is exactly what vmm_is_user_mapped()
*      reports. Mere presence is the weaker question and no longer the
*      interesting one: with a private lower half per task, a page that
*      exists but is not user accessible is a page the caller was never meant
*      to see, and following such a pointer would hand it kernel-side data
*      through a call it is allowed to make. The test answers from the page
*      tables without touching the memory, which is the whole point -- a
*      dereference to find out would already be the page fault we are
*      avoiding.
*
*  What this deliberately does not establish is writability. PAGE_WRITE is
*  never asked about here, so this remains the test for a pointer the kernel
*  only ever reads through. A call that hands a result back through a user
*  pointer needs the stronger question, and asks user_byte_writable() below. */
static int user_byte_ok(uint32_t addr)
{
	if(addr < PAGE_SIZE) return 0;
	if(addr >= KERNEL_VIRTUAL_BASE) return 0;
	if(!vmm_is_user_mapped(addr)) return 0;

	return 1;
}

/* Frame address bits of a page directory or page table entry. vmm.c has the
*  same constant, but keeps it private, and vmm.h is not this file's to extend
*  this round -- see the walk in user_byte_writable() below for why the entries
*  have to be read here at all. */
#define SYS_PTE_ADDR_MASK 0xFFFFF000UL

/* Non-zero if a single byte at addr may be *written* on behalf of ring 3.
*
*  This is the question the calls added this round raise for the first time.
*  Everything before them returned its answer in eax and touched nothing the
*  caller owns; SYS_FSINFO, SYS_STAT, SYS_READ and SYS_READDIR all store
*  through a pointer ring 3 chose, and a readable page is not a writable one.
*
*  Read-only user pages are not hypothetical here. exec.c maps a PT_LOAD that
*  carries no PF_W as PAGE_USER without PAGE_WRITE, and main.c's ring 3 demo
*  maps its copy of .usertext present and user readable but not writable. Any
*  of those addresses is something a program can name and cannot write to
*  itself.
*
*  What an attacker gets without this test is a fault, not silent corruption,
*  and the fault is the point. vmm_init() sets CR0.WP, so a supervisor store
*  does obey the R/W bit of a user page and the write raises #PF instead of
*  going through. That page fault is taken in ring 0, inside an interrupt
*  handler, at an address ring 3 chose -- and isrs.c answers a fault with a
*  task running by aborting that task and switching away. The task dies in the
*  middle of a system call, abandoning whatever the call was holding: a heap
*  block in exec.c, the sector cache in fat.c, a half written structure. One
*  line of ring 3 code, no exploit required. The same thing happens, without
*  needing a read-only page at all, for a pointer into a hole.
*
*  And the silent version is one bit away. CR0.WP is set in vmm.c, not here; a
*  check that is only correct because another file happens to set a flag is a
*  check that stops being correct the day it does not. Then SYS_READ would
*  overwrite code the page tables were told to protect, and the system call
*  would have become exactly the write primitive they exist to deny.
*
*  vmm.h offers vmm_is_user_mapped() and no writable sibling, so the two level
*  walk is repeated here with PAGE_WRITE added to the mask. Both levels have to
*  grant all three bits, for the reason vmm.c spells out at its own walk: the
*  processor takes the effective rights of a page as the AND of the directory
*  entry and the table entry, so a writable table entry underneath a read-only
*  directory entry is not writable. The right home for this is a
*  vmm_is_user_writable() next to the function it mirrors; until that exists,
*  duplicating twenty lines is the smaller evil, because the alternative on
*  offer is not checking at all.
*
*  The directory walked is the ACTIVE one -- vmm_current_space() reads CR3, and
*  CR3 still belongs to the caller because entering through a gate never
*  reloads it. That is the same invariant user_byte_ok() leans on, and the same
*  reason the question is well posed now that tasks own separate spaces.
*
*  Both frames the walk dereferences are reached through P2V(), so both have to
*  lie inside the direct mapping. vmm_create_space() refuses a directory frame
*  that does not, and page tables come from the same allocator -- but the bound
*  is re-tested rather than assumed, because the cost is one compare and the
*  failure mode is a wild kernel read. */
static int user_byte_writable(uint32_t addr)
{
	const uint32_t need = PAGE_PRESENT | PAGE_USER | PAGE_WRITE;
	uint32_t space;
	uint32_t *dir;
	uint32_t *table;
	uint32_t pde;
	uint32_t pte;

	/* The same two range rules as user_byte_ok(), and stated here rather than
	*  borrowed, so that this function is safe to call on its own. */
	if(addr < PAGE_SIZE) return 0;
	if(addr >= KERNEL_VIRTUAL_BASE) return 0;

	space = vmm_current_space();
	if(space == 0 || space > DIRECT_MAP_LIMIT) return 0;

	dir = (uint32_t *)P2V(space & SYS_PTE_ADDR_MASK);
	pde = dir[addr >> 22];
	if((pde & need) != need) return 0;

	if((pde & SYS_PTE_ADDR_MASK) > DIRECT_MAP_LIMIT) return 0;

	table = (uint32_t *)P2V(pde & SYS_PTE_ADDR_MASK);
	pte = table[(addr >> 12) & 0x3FF];
	if((pte & need) != need) return 0;

	return 1;
}

/* Non-zero if every byte of [addr, addr+len) may be touched on behalf of ring
*  3 -- read only, or read and written when writable is set.
*
*  A buffer is a range, not a byte, and that distinction is the whole of this
*  function. A mapping covers one page, and SYS_READ is handed a length the
*  caller picked. A check that looked only at the first byte would let a
*  program map a single page, ask for a megabyte and have the kernel walk
*  straight out the far end -- into whatever the next page happens to hold, or
*  into nothing at all and the ring 0 page fault described above. So every page
*  the range touches is validated, from the one holding the first byte to the
*  one holding the last, and at page granularity because that is the
*  granularity the answer has.
*
*  The two lines that carry the most weight are the last two of the four. The
*  end of the range is worked out without ever forming addr + len as a value
*  that could wrap: len is compared against the distance from addr up to
*  KERNEL_VIRTUAL_BASE, a subtraction that cannot overflow because addr is
*  already known to be below it. That single comparison does two jobs. It
*  refuses addr = 0xBFFFF000 with len = 0xFFFFFFFF on arithmetic, rather than
*  entering a loop that has already wrapped to zero and is cheerfully
*  validating page zero upwards. And it is what stops a buffer that starts in
*  the user half from ENDING in the kernel half -- a page just below the line,
*  a length that carries past it, and the kernel writing file contents over its
*  own data, which the per-page walk alone would not prevent if the loop were
*  allowed to get that far. Once the test is passed, addr + len - 1 is an
*  ordinary address below KERNEL_VIRTUAL_BASE and the loop cannot wrap either.
*
*  A zero length range is accepted and touches nothing. The two callers that
*  can produce one say at their call site what they do about it. */
static int user_range_ok(uint32_t addr, uint32_t len, int writable)
{
	uint32_t page;
	uint32_t last;

	if(len == 0) return 1;
	if(addr < PAGE_SIZE) return 0;
	if(addr >= KERNEL_VIRTUAL_BASE) return 0;
	if(len > KERNEL_VIRTUAL_BASE - addr) return 0;

	last = (addr + len - 1) & ~(uint32_t)(PAGE_SIZE - 1);

	for(page = addr & ~(uint32_t)(PAGE_SIZE - 1); page <= last; page += PAGE_SIZE)
	{
		if(writable)
		{
			/* Subsumes the mapped-and-user test: the mask user_byte_writable()
			*  applies already contains PAGE_PRESENT and PAGE_USER, so asking
			*  vmm_is_user_mapped() as well would only walk the same two levels
			*  a second time for an answer already given. */
			if(!user_byte_writable(page)) return 0;
		}
		else
		{
			if(!user_byte_ok(page)) return 0;
		}
	}

	return 1;
}

/* Copies len bytes into user memory at dst, which the caller must already have
*  put through user_range_ok() with writable set.
*
*  Byte at a time on purpose. A user pointer carries no alignment promise, and
*  a struct written field by field through an unaligned pointer would be a
*  string of separate stores this file would have to reason about one by one.
*  Copying a fully built local out in one pass means the check and the write
*  are about the same thing: a range that was validated as a whole.
*
*  That the local is built first also closes an information leak. Both ABI
*  structs are laid out to have no padding, but they are still stack memory,
*  and stack memory in this kernel has held whatever the last call left there.
*  The handlers clear their local before filling it, so what crosses the gate
*  is only what was meant to. */
static void copy_to_user(uint32_t dst, const void *src, uint32_t len)
{
	const unsigned char *from;
	uint32_t i;

	from = (const unsigned char *)src;

	for(i = 0; i < len; i++)
	{
		*(volatile unsigned char *)(dst + i) = from[i];
	}
}

/* Length of the NUL terminated string at addr, or -1 if the string is not
*  safe to read. max bounds the scan, terminator included.
*
*  The check has to walk the string, because a mapping only ever covers one
*  page: a string may start in a page the caller may read and run straight
*  into one it may not, or into none at all. Rather than validating every
*  single byte (vmm_is_user_mapped() walks directory and table, that would be
*  two lookups per character), the page is validated once at the start and
*  then again at every 4 KiB boundary the string crosses.
*
*  The length is deliberately established before the first character is
*  printed. Validating and printing in one pass would mean a bad pointer
*  produces half a line of output and an error code at the same time; this
*  way SYS_WRITE either prints the whole string or prints nothing at all.
*
*  A string with no terminator within max bytes is rejected instead of
*  truncated: at that point the argument is not a string, and guessing where it
*  ends is not the kernel's job.
*
*  The bound is a parameter rather than SYS_WRITE_MAX outright, because the two
*  kinds of string that cross this gate deserve different ones. A string to be
*  printed may reasonably be a whole line of text; a path is copied into a
*  buffer on a 4 KiB kernel stack and has no business being longer than
*  SYS_PATH_MAX. Passing the bound in keeps that decision at the call site,
*  where the size of the destination is actually known. */
static int user_string_len(uint32_t addr, int max)
{
	int len;
	uint32_t at;

	if(!user_byte_ok(addr)) return -1;

	for(len = 0; len < max; len++)
	{
		at = addr + (uint32_t)len;

		/* A new page starts here, so the old permission no longer says
		*  anything about this byte. Re-check before reading it. */
		if(len > 0 && (at & (PAGE_SIZE - 1)) == 0)
		{
			if(!user_byte_ok(at)) return -1;
		}

		if(*(const volatile unsigned char *)at == '\0') return len;
	}

	return -1;
}

/* Copies the user string at addr into dst, which holds size bytes including
*  the terminator. Returns its length, or -1 if the pointer does not survive
*  user_string_len().
*
*  Everything below that reaches the filesystem goes through here rather than
*  handing fat.c or exec.c the ring 3 pointer directly, for two reasons that
*  both come down to not letting a user pointer travel further than it has to.
*
*  A path is validated once and then used for the whole length of a disk read,
*  which is a long time by the standards of this kernel -- a PIO transfer with
*  the timer free to preempt it. Copying first means the bytes the filesystem
*  parses are the bytes that were checked, and no later line has to re-derive
*  why the pointer is still good.
*
*  It also keeps the ring 3 pointer out of code that knows nothing about it.
*  exec_spawn_path() builds a second address space; a user pointer is a
*  statement about the caller's, and the difference between the two is exactly
*  the sort of thing that is obvious here and invisible three files away. */
static int copy_string_from_user(uint32_t addr, char *dst, int size)
{
	int len;
	int i;

	len = user_string_len(addr, size);
	if(len < 0) return -1;

	for(i = 0; i < len; i++)
	{
		dst[i] = (char)*(const volatile unsigned char *)(addr + (uint32_t)i);
	}
	dst[len] = '\0';

	return len;
}

/* Copies a kernel string into a fixed width field of an ABI struct,
*  truncating if it does not fit and zero filling the rest of the field. The
*  fill is not decoration: the field is part of a structure that crosses into
*  user memory, and trailing bytes left as they were would be kernel stack
*  contents travelling out with it. */
static void field_copy(char *dst, int size, const char *src)
{
	int i;

	for(i = 0; i < size - 1 && src[i] != '\0'; i++)
	{
		dst[i] = src[i];
	}
	for(; i < size; i++)
	{
		dst[i] = '\0';
	}
}


/* ------------------------------------------------------------------ */
/* Network connections owned by ring 3                                 */
/* ------------------------------------------------------------------ */

/* What one open connection is, from this gate's point of view.
*
*  The table exists because a TCP handle from tcp.c is a number and nothing
*  else. Handing that number straight to ring 3 would make every one of the
*  four connections on the machine reachable by every program, since a handle
*  another task obtained is a small integer and guessing it takes four tries.
*  So the number ring 3 is given indexes THIS table, the table records who
*  opened the entry, and the kernel handle never crosses the gate in either
*  direction.
*
*  owner is the pid that called SYS_CONNECT. sysnet_slot_of() refuses an entry
*  whose owner is not the caller, which is what makes the answer to "can a
*  program reach a connection another task opened" a no rather than a hope.
*
*  peer, peer_port and local_port are not bookkeeping for the shell -- they are
*  an identity. See sysnet_identity_ok() for what they defend against. */
typedef struct
{
	int      used;
	int      khandle;      /* the handle tcp.c gave us                     */
	int      owner;        /* pid that opened it                           */
	uint32_t peer;         /* host order, as passed to tcp_connect()       */
	uint16_t peer_port;
	uint16_t local_port;   /* 0 if tcp_conn_get() would not say            */
} sysnet_conn;

/* One entry per connection tcp.c can hold, so the table is never the tighter
*  limit of the two and a refusal always means the same thing. */
static sysnet_conn sysnet_conns[SYS_NET_HANDLE_MAX];

/* Whoever is inside SYS_RESOLVE, and a lock to get there.
*
*  dns.c serves ONE lookup at a time for the whole machine: dns_resolve()
*  refuses while a query is in flight, and the answer lands in a single
*  dns_result(). With one shell that is a limitation; with several tasks it is a
*  correctness problem, and not the obvious one. The obvious one is that the
*  second task's dns_resolve() fails, which is merely inconvenient. The real one
*  is the race after it succeeds: task A's answer arrives and dns.c moves to
*  DNS_STATE_DONE, task B wakes first, sees a resolver that is no longer busy
*  and starts its own lookup, which clears dns_result_ip -- and A then reads
*  B's address for A's name. Nothing in the sequence is invalid, and the wrong
*  address is returned without any error anywhere.
*
*  A lock held across the whole of "start a lookup, collect its result" is what
*  closes that, and it has to be taken atomically: vector 0x80 is a trap gate,
*  so the timer can preempt this file between a test and a set. xchg is the
*  smallest thing that cannot be preempted in the middle, needs no cli, and is
*  correct on the single processor this kernel runs on.
*
*  The owner is recorded next to the lock so that a task aborted while holding
*  it does not take the resolver down with it -- see sysnet_reap(). */
static volatile int sysnet_dns_lock = 0;
static int sysnet_dns_owner = -1;

/* Milliseconds elapsed since a timer_get_ticks() snapshot.
*
*  Unsigned subtraction, like timer_wait()'s own loop, and for the same reason:
*  timer_get_ticks() counts milliseconds in an int and eventually wraps, but
*  the difference between two snapshots stays right across the wrap. A plain
*  "now > start + limit" would be false forever on the wrong side of it, which
*  is a timeout that never fires -- precisely the failure these bounds exist to
*  prevent. */
static uint32_t sysnet_ms_since(uint32_t start)
{
	return (uint32_t)timer_get_ticks() - start;
}

/* Interrupts off, and back to what the caller had.
*
*  Every task_wait() in this file is wrapped in these two, because the idiom
*  system.h spells out only closes the lost wakeup race if the condition is
*  tested with interrupts already off. net.c and kb.c have a pair of these
*  each and both are static to their file; there is no global one, so this
*  file carries its own rather than inventing a cross-file interface for six
*  lines of inline assembly.
*
*  pushfl before cli, so a caller that already held interrupts off gets them
*  back off and one that did not gets them back on. */
static unsigned long sysnet_irq_save(void)
{
	unsigned long flags;

	__asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
	return flags;
}

static void sysnet_irq_restore(unsigned long flags)
{
	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

/* One turn of every waiting loop below: drive the state machines, then block
*  until a frame has been processed or the poll interval is up.
*
*  Both polls are called from every wait, not just from the wait that cares
*  about them. Neither does anything when it has nothing to do -- dns_poll()
*  returns at once unless a query is in flight, tcp_poll() walks four slots --
*  and calling both means a task blocked in SYS_RECV keeps another task's
*  handshake and retransmission timers running rather than freezing them for
*  fifteen seconds. Nothing else moves them: the receive path only reacts to
*  segments that do arrive, and the interesting failure is the one where none
*  does.
*
*  WAKING AND POLLING ARE NOT THE SAME WAIT, and reconciling them is the whole
*  design of this function. A wake says "a frame was processed, look again"; it
*  says nothing about the timers above, and nothing ever will -- a
*  retransmission is due precisely when NOTHING arrived. So the wait is bounded
*  by SYS_NET_POLL and the bound is not a formality: it is the interval at
*  which the two polls run, unchanged from the sleep it replaces. A wait with
*  no timeout would stop retransmitting on a silent link, which is the one case
*  the timers exist for.
*
*  What the wake buys is the other side of the same interval. Before, a reply
*  that landed one millisecond after a look was not noticed for another
*  forty-nine; now it wakes the task and is noticed at once. The bound is
*  therefore an upper limit that only quiet links reach, and the polls run at
*  least as often as they used to -- never less, which is what would break a
*  timer -- and more often when the network is busy, which costs two cheap
*  calls per frame.
*
*  THE DRAIN COMES FIRST, and when it did something this returns without
*  blocking at all. The caller's loop tested its condition BEFORE calling here,
*  so a frame this call parses has not been looked at by anybody yet -- and
*  blocking on the strength of a wake that already happened is exactly the lost
*  wakeup the idiom exists to prevent, arrived at from inside. Handing the
*  caller back to its own test is both the correct answer and the fast one.
*
*  THE WINDOW AFTER THE DRAIN is closed with a counter rather than left open.
*  A frame that arrives once this call has drained is parsed by the drain task,
*  which wakes the channel while this task is still RUNNING -- a wake that
*  reaches nobody, and a wait that then sits out its whole interval although
*  its answer has already been parsed. net_rx_packets() is incremented by the
*  card's interrupt before anything is parsed, so reading it after the drain
*  and comparing it with interrupts off is exactly the "test the condition
*  again, with interrupts off" that system.h asks for, standing in for a
*  condition this function cannot see. It cannot spin: a turn that skips the
*  wait has a frame to show for it and the caller re-tests at once.
*
*  What stays open is the window before this call -- between the caller's own
*  test and the drain below. A frame parsed in there was parsed by somebody,
*  so the caller's next test sees it; the cost is one turn of the loop and not
*  a wait.
*
*  NOT EVERY CALLER CAN BLOCK. task_wait() answers a caller with no current
*  task with "timed out" immediately, which would turn this into a spin rather
*  than a wait, so that case keeps the sleep it always had. It should not be
*  reachable -- everything here runs on behalf of a ring 3 task -- and it costs
*  one comparison to make sure. */
static void sysnet_pump(void)
{
	unsigned long flags;
	uint32_t seen;
	int handled;

	/* Drain the receive queue ourselves rather than waiting for the task that
	*  normally does it.
	*
	*  Since the protocol work moved out of the card's interrupt, a frame is
	*  copied into a queue and handled later -- and "later" is one full trip
	*  round the scheduler, because every task holds its slice even while it
	*  sleeps. Measured, that turned a ping's round trip from 0 ms into 34 ms.
	*  No sleep interval fixes it; the cost is in tasks.c.
	*
	*  But a task that is sitting here is a task waiting for exactly the packet
	*  in that queue, so it may as well take it out itself. net_queue_drain()
	*  is written for task context and refuses re-entry, so two tasks calling
	*  it is not a problem -- the second gets 0 and carries on. The dedicated
	*  drain task stays: it is what handles everything nobody is waiting for,
	*  an ARP request to answer or a peer's retransmission arriving while this
	*  machine has nothing open. */
	handled = net_queue_drain();

	/* Read after the drain and before anything else: see the window above. */
	seen = net_rx_packets();

	dns_poll();
	tcp_poll();

	/* Something was parsed that the caller has not seen. Its condition may be
	*  true already, so the only right thing to do is let it look. */
	if(handled > 0) return;

	if(taskmgr_get_currpid() < 0)
	{
		sleep(SYS_NET_POLL);
		return;
	}

	flags = sysnet_irq_save();

	if(net_rx_packets() == seen)
		task_wait(net_wait_channel(), SYS_NET_POLL);

	sysnet_irq_restore(flags);
}

/* Takes the resolver, or reports that somebody else has it. */
static int sysnet_dns_take(void)
{
	int taken;

	taken = 1;

	/* xchg writes 1 and hands back what was there, in one instruction that no
	*  interrupt can land inside. A zero came back means the lock was free and
	*  is now ours; a one means it was already held and we changed nothing. */
	__asm__ __volatile__ ("xchgl %0, %1"
		: "+r" (taken), "+m" (sysnet_dns_lock)
		: : "memory");

	if(taken != 0) return 0;

	sysnet_dns_owner = taskmgr_get_currpid();
	return 1;
}

/* Gives it back. The owner is cleared first and the lock last, so that a task
*  which takes it the instant after cannot find a stale owner recorded against
*  a lock it now holds. */
static void sysnet_dns_release(void)
{
	sysnet_dns_owner = -1;
	__asm__ __volatile__ ("" : : : "memory");
	sysnet_dns_lock = 0;
}

/* Drops everything a task owns: its connections, and the resolver if it was
*  holding it.
*
*  Called from sys_exit() so that a program which returns without closing does
*  not leak a connection -- there are four on the machine, and a shell that has
*  run "fetch" four times would otherwise have none left. tcp_abort() rather
*  than tcp_close() is deliberate and is what tcp.h describes it for: this is a
*  caller that has given up, the peer should learn the connection failed rather
*  than ended, and the slot has to come back now rather than after TIME_WAIT.
*
*  Also called from sys_spawn() on the pid it was just handed. Task slots are
*  reused -- find_free_slot() in tasks.c takes an aborted one -- so a pid is not
*  unique over time, and an entry left behind by a task that died without
*  reaching sys_exit() would otherwise be inherited by whoever next gets that
*  number. Clearing at the moment a pid is issued is the one point where that
*  can be done without a death notification tasks.c does not offer. */
static void sysnet_release_task(int pid)
{
	int i;

	if(pid < 0) return;

	for(i = 0; i < SYS_NET_HANDLE_MAX; i++)
	{
		if(!sysnet_conns[i].used) continue;
		if(sysnet_conns[i].owner != pid) continue;

		tcp_abort(sysnet_conns[i].khandle);
		sysnet_conns[i].used = 0;
	}

	if(sysnet_dns_lock != 0 && sysnet_dns_owner == pid) sysnet_dns_release();
}

/* Collects after tasks that died without saying so.
*
*  sys_exit() is the polite path and it is not the only one: a task aborted by
*  a page fault, or by taskmgr_killall(), never runs another instruction, so
*  whatever it held is held forever unless somebody notices. Every network call
*  starts with this sweep, which costs four state lookups.
*
*  ABORTED and NULL only. A SUSPENDED task is alive -- the shell suspends
*  tasks, and exec.c creates them suspended -- and taking its connections away
*  would be a bug of this file's own making. */
static void sysnet_reap(void)
{
	int i;
	int state;

	for(i = 0; i < SYS_NET_HANDLE_MAX; i++)
	{
		if(!sysnet_conns[i].used) continue;

		/* Both endings release: a task that exited normally holds a
		*  connection no less firmly than one that was aborted, and it is the
		*  ordinary case -- a program that returns without closing. */
		state = taskmgr_task_state(sysnet_conns[i].owner);
		if(state != TASK_STATE_ABORTED && state != TASK_STATE_EXITED
		   && state != TASK_STATE_NULL) continue;

		tcp_abort(sysnet_conns[i].khandle);
		sysnet_conns[i].used = 0;
	}

	if(sysnet_dns_lock != 0)
	{
		state = taskmgr_task_state(sysnet_dns_owner);
		if(state == TASK_STATE_ABORTED || state == TASK_STATE_EXITED
		   || state == TASK_STATE_NULL)
		{
			/* dns.c is still waiting for an answer nobody will collect. Cancel
			*  it as well, or the next lookup spends its whole budget waiting
			*  for a query that belongs to a task that no longer exists. */
			if(dns_state() == DNS_STATE_QUERY) dns_cancel();
			sysnet_dns_release();
		}
	}
}

/* The table slot a handle from ring 3 names, or -1.
*
*  This is the hostile-input check for the one argument of three of these five
*  calls, and it asks three things in order.
*
*  Is it in range. The handle is the slot plus one, so 0 -- the value an
*  uninitialised variable in a user program holds, and the value a failed
*  connect() left in it -- names nothing and is refused rather than quietly
*  meaning the first connection. Everything outside 1..TCP_MAX_CONNS is refused
*  on arithmetic, before the table is touched.
*
*  Is it open. A handle that was closed, or was never opened, indexes an entry
*  with used = 0.
*
*  Is it the caller's. This is the one that matters. Without it the check above
*  would be satisfied by any of the four small integers, and a program could
*  read from -- or write into -- a connection a different task opened simply by
*  counting to four. With it, a handle is only ever usable by the pid that
*  obtained it, and a program that guesses learns nothing but SYS_EINVAL: the
*  same answer it gets for a handle that does not exist, so guessing does not
*  even reveal that somebody else's connection is there.
*
*  A pid of -1 (no task running, i.e. the kernel called this itself) matches no
*  entry, because every entry is owned by a task that called SYS_CONNECT. */
static int sysnet_slot_of(int handle)
{
	int slot;

	if(handle < 1 || handle > SYS_NET_HANDLE_MAX) return -1;

	slot = handle - 1;
	if(!sysnet_conns[slot].used) return -1;
	if(sysnet_conns[slot].owner != taskmgr_get_currpid()) return -1;

	return slot;
}

/* Non-zero if the kernel handle in this slot is still the connection we opened.
*
*  The ownership test above settles who may name a slot. It does not settle
*  what the slot still points at, and those are different questions, because
*  the kernel handle inside it can be recycled underneath us. A connection that
*  the peer resets reaches TCP_CLOSED and its slot in tcp.c becomes free; if
*  another task connects before this one wakes from its fifty millisecond sleep,
*  the same small integer now names that task's connection -- and a tcp_recv()
*  on it would hand this program the other program's bytes. Nothing in the
*  ownership check sees that, because our table entry never changed.
*
*  So identity is checked rather than assumed, out of what tcp.h already
*  publishes: tcp_conn_get() reports the peer address, the peer port and the
*  local port of every open connection. The local port is the strong part -- it
*  is ephemeral and a new connection picks a different one -- and the peer pair
*  catches the rest. A tuple that does not match the one recorded at connect
*  time means the handle is not ours any more, whatever it is.
*
*  The enumeration walks TCP_MAX_CONNS indices and trusts tcp_conn_get()'s
*  return value rather than tcp_conn_count(), so it is right whether that count
*  is the number of live connections or the size of the table.
*
*  Not being listed at all is deliberately NOT treated as proof. It is the
*  ordinary answer for a connection that has been freed, and it is also what a
*  stack that does not fill the enumeration in would say about every connection
*  there is -- and this test failing wrongly would break every transfer. So the
*  fallback is the weaker question tcp_state() answers, which is the check this
*  file would have had without any of the above: gone if the handle is closed or
*  refused, still ours otherwise. The strong test costs nothing when it works
*  and costs nothing when it does not. */
static int sysnet_identity_ok(int slot)
{
	int i;
	int found;
	int state;
	uint32_t peer;
	uint16_t peer_port;
	uint16_t local_port;

	for(i = 0; i < TCP_MAX_CONNS; i++)
	{
		found     = -1;
		peer      = 0;
		peer_port = 0;
		local_port = 0;

		if(tcp_conn_get(i, &found, &peer, &peer_port, &local_port, 0, 0, 0) != 0)
			continue;
		if(found != sysnet_conns[slot].khandle) continue;

		if(peer != sysnet_conns[slot].peer) return 0;
		if(peer_port != sysnet_conns[slot].peer_port) return 0;
		if(sysnet_conns[slot].local_port != 0 &&
		   local_port != sysnet_conns[slot].local_port) return 0;

		return 1;
	}

	state = tcp_state(sysnet_conns[slot].khandle);
	if(state < 0 || state == TCP_CLOSED) return 0;

	return 1;
}

/* Records what the connection just opened is, so that sysnet_identity_ok() has
*  something to compare against. The local port is asked of tcp.c rather than
*  guessed; a stack that will not say leaves it 0, which the comparison then
*  skips rather than failing on. */
static void sysnet_remember(int slot, int khandle, uint32_t ip, uint16_t port)
{
	int i;
	int found;
	uint16_t local_port;

	sysnet_conns[slot].khandle    = khandle;
	sysnet_conns[slot].peer       = ip;
	sysnet_conns[slot].peer_port  = port;
	sysnet_conns[slot].local_port = 0;

	for(i = 0; i < TCP_MAX_CONNS; i++)
	{
		found      = -1;
		local_port = 0;

		if(tcp_conn_get(i, &found, 0, 0, &local_port, 0, 0, 0) != 0) continue;
		if(found != khandle) continue;

		sysnet_conns[slot].local_port = local_port;
		return;
	}
}


/* ------------------------------------------------------------------ */
/* The screen owned by ring 3                                          */
/* ------------------------------------------------------------------ */

/* Who has the screen, and everything needed to take it back from them.
*
*  One owner at a time, because the kernel's console is on that screen too and
*  two owners of one screen is not a thing that can be made to work -- fbcon.h
*  says so at fbcon_suspend() and refuses nesting for the same reason. The
*  record here is the kernel side of that: fbcon.c knows the console is
*  suspended, and this knows WHO for, which is the part needed to give the
*  screen back when that task stops existing.
*
*  THE SPACE IS RECORDED RATHER THAN LOOKED UP LATER, and that is the field
*  that matters most. A dead task's slot is recycled by find_free_slot() in
*  tasks.c, its address space handed back by task_space_release(), and the pid
*  reissued to somebody else -- so taskmgr_task_space(owner) at cleanup time
*  can name a DIFFERENT space, belonging to a live task, whose pages have
*  nothing to do with the screen. Unmapping SYS_FB_VIRT in that one would take
*  a page away from a program that legitimately owns it. Keeping the space we
*  mapped into and comparing the two is what makes the cleanup safe: they
*  agree only while the mapping is still where it was put.
*
*  bytes is the size that was mapped, so the unmap covers exactly the pages the
*  map covered, and not a size re-derived from a mode that may have changed. */
static int         sysfb_owner = -1;
static addrspace_t sysfb_space = 0;
static uint32_t    sysfb_bytes = 0;

/* Non-zero once the reclaim task exists. It is never taken down again -- see
*  sysfb_reclaim_task(). */
static int         sysfb_janitor = 0;

/* Removes the mapping SYS_MAPFB made, page by page.
*
*  The whole range is walked even when only part of it was mapped, which is
*  what the failure path in sys_mapfb() relies on: vmm_unmap_in() answers -1
*  for a page that was never mapped and changes nothing, so one function
*  cleans up after both a complete mapping and a half finished one.
*
*  NOTHING IS FREED HERE and nothing may be. The frames behind these pages are
*  the card's, not the pmm's -- vmm_map_mmio() spells out what handing one to
*  pmm_free_frame() would do, namely clear a bit belonging to a completely
*  unrelated frame and hand that one out twice. Only the mapping goes. */
static void sysfb_unmap_pages(addrspace_t space, uint32_t bytes)
{
	uint32_t off;

	for(off = 0; off < bytes; off += PAGE_SIZE)
	{
		vmm_unmap_in(space, SYS_FB_VIRT + off);
	}
}

/* Takes the screen back and gives it to the console. Safe to call when nobody
*  holds it, and safe to call twice.
*
*  The record is read and cleared with interrupts off, in one step, and the
*  work is done afterwards out of the locals. Both halves of that matter. The
*  clear has to be atomic against everything else that can call this -- the
*  owner's own SYS_UNMAPFB, sys_exit(), and the reclaim task, any two of which
*  can be interleaved because vector 0x80 is a trap gate and the timer
*  preempts -- or two callers both see an owner, both unmap and both repaint,
*  and the second repaint lands on a screen a third program may already have
*  taken. And the work has to happen with interrupts back ON, because
*  fbcon_resume() repaints the entire console: three megabytes of writes to
*  uncached device memory is not a thing to do with the timer masked.
*
*  THE UNMAP IS CONDITIONAL, THE REPAINT IS NOT. If the space is not the one
*  that was mapped into, the mapping is already gone with the space it lived in
*  and touching that directory would be a write into whatever the pmm has since
*  handed the frames to. The console does not care either way: the screen is
*  hardware and it has to be repainted whatever became of the task that was
*  drawing on it. Getting that order wrong is the difference between a machine
*  that comes back and one that sits at a black screen. */
static void sysfb_drop(void)
{
	unsigned long flags;
	addrspace_t space;
	uint32_t bytes;
	int owner;

	flags = sysnet_irq_save();

	owner = sysfb_owner;
	space = sysfb_space;
	bytes = sysfb_bytes;

	sysfb_owner = -1;
	sysfb_space = 0;
	sysfb_bytes = 0;

	sysnet_irq_restore(flags);

	if(owner < 0) return;

	if(space != 0 && taskmgr_task_space(owner) == space)
		sysfb_unmap_pages(space, bytes);

	fbcon_resume();
}

/* Gives the screen back if the task holding it has ended. The screen's version
*  of sysnet_reap(), and the same three states mean the same thing here:
*  ABORTED for a task that faulted or was killed, EXITED for one that returned,
*  NULL for a slot that names nobody. A SUSPENDED task is alive and keeps its
*  screen -- taking it away would repaint the console over a picture whose
*  owner is coming back. */
static void sysfb_reap(void)
{
	int state;

	if(sysfb_owner < 0) return;

	state = taskmgr_task_state(sysfb_owner);
	if(state != TASK_STATE_ABORTED && state != TASK_STATE_EXITED
	   && state != TASK_STATE_NULL) return;

	sysfb_drop();
}

/* Drops what one particular pid holds. Called from sys_exit() next to
*  sysnet_release_task(), and from sys_spawn() on the pid it was just handed,
*  for exactly the two reasons given there: a program that returns without
*  calling SYS_UNMAPFB is the ordinary case rather than the exceptional one,
*  and a pid is not unique over time, so a record left behind by a task that
*  died without reaching sys_exit() must not be inherited by whoever next gets
*  that number. */
static void sysfb_release_task(int pid)
{
	if(pid < 0) return;
	if(sysfb_owner != pid) return;

	sysfb_drop();
}

/* Looks, four times a second, for a screen whose owner is gone.
*
*  This task exists because of a gap this file cannot close any other way.
*  sys_exit() covers the program that ends properly, and the reap at the head
*  of SYS_MAPFB covers the next program that wants the screen -- but a task can
*  also die by a page fault, by "taskmgr -k" and by taskmgr_killall(), and in
*  all three it simply never executes another instruction. There is no
*  notification: taskmgr_task_abort() changes a state and no more, and no
*  address stands for "this pid finished" (run_wait() in main.c makes the same
*  observation about waiting for a program and settles for polling too). So
*  nothing runs on behalf of the dead task, and the screen it was drawing on
*  would stay exactly as it left it -- with the console printing into a shadow
*  buffer nobody paints -- until some other program happened to ask for it.
*  On a machine whose shell is a kernel task and issues no system calls at all,
*  that can be forever. A black screen with a live prompt behind it is the one
*  outcome a user cannot recover from without a reboot.
*
*  A task is what it takes, because the recovery is fbcon_resume() and that
*  repaints the whole console: it needs task context, not the timer interrupt.
*  It is created on the first successful SYS_MAPFB rather than at boot -- a
*  machine that never runs a graphical program never pays for it -- and it is
*  never taken down, because the cost of an existing one is a state lookup four
*  times a second and the cost of tearing it down and recreating it is a task
*  slot changing hands for no gain.
*
*  It is a poll and it is honest about being one. The mechanism that would
*  replace it is one line in tasks.c: taskmgr_task_exit() and
*  taskmgr_task_abort() waking a channel that stands for the slot, and system.h
*  naming that address. Then this becomes a task_wait() with SYS_FB_REAP_MS as
*  the backstop instead of the interval, and a killed program's console comes
*  back in the same millisecond. tasks.c is not this file's to edit. */
static void sysfb_reclaim_task(void)
{
	for(;;)
	{
		sleep(SYS_FB_REAP_MS);
		sysfb_reap();
	}
}

/* Starts it, once. A failure to create it is not a failure of the call that
*  triggered it: the screen still works, only the recovery after a kill is
*  missing, and refusing SYS_MAPFB over that would be trading a working
*  program for a cleanup path. */
static void sysfb_start_janitor(void)
{
	int pid;

	if(sysfb_janitor) return;

	pid = taskmgr_add_task(sysfb_reclaim_task, "Screen Reclaim Task",
	                       TASK_PRIORITY_LOW);
	if(pid < 0) return;

	taskmgr_task_start(pid);
	sysfb_janitor = 1;
}

/* Position and width of one colour channel, read off the bit mask that channel
*  alone produces. Both 0 for a mask of 0.
*
*  The mask is assumed to be contiguous, which every pixel format on hardware
*  is: a channel is a run of adjacent bits. A hypothetical scattered one would
*  come out as the run that spans it, which is wrong in the same way and to the
*  same degree as any other answer this file could invent for it. */
static void sysfb_channel(uint32_t mask, uint8_t *pos, uint8_t *size)
{
	int first;
	int last;
	int i;

	*pos  = 0;
	*size = 0;

	if(mask == 0) return;

	first = -1;
	last  = -1;

	for(i = 0; i < 32; i++)
	{
		if((mask & ((uint32_t)1 << i)) == 0) continue;

		if(first < 0) first = i;
		last = i;
	}

	*pos  = (uint8_t)first;
	*size = (uint8_t)(last - first + 1);
}

/* Fills in where the colour channels sit, which sys_fbinfo carries because 32
*  bpp is usually but not always 0x00RRGGBB and 16 bpp is usually but not
*  always 5-6-5.
*
*  fbcon.h publishes no accessor for the channel layout -- it has fbcon_rgb(),
*  which PACKS a colour, and that is asked instead of guessed. A pure red gives
*  back exactly the bits red occupies, and the same for the other two, so three
*  calls to the function that owns the format produce the format. Deriving it
*  from the depth here would be a second implementation of format_from_depth()
*  in fbcon.c, kept in step by hand, and would be wrong for precisely the modes
*  the fields exist for.
*
*  Two answers are refused rather than reported. A depth below 15 is an INDEXED
*  mode, where a pixel is a palette index and fbcon_rgb() returns the nearest
*  console colour -- a small integer that is not a bit field at all and would
*  read as a lie about a channel. And masks that overlap, or any that is zero,
*  mean the probe did not measure what it thinks it did. Both leave the six
*  fields at zero, which the struct's memset() already made them: no claim,
*  rather than a wrong one. A program that finds red_size at 0 knows it has to
*  work the format out for itself or refuse the mode. */
static void sysfb_format(sys_fbinfo *info)
{
	uint32_t red;
	uint32_t green;
	uint32_t blue;

	if(fbcon_bpp() < 15) return;

	red   = fbcon_rgb(255, 0, 0);
	green = fbcon_rgb(0, 255, 0);
	blue  = fbcon_rgb(0, 0, 255);

	if(red == 0 || green == 0 || blue == 0) return;
	if((red & green) != 0 || (red & blue) != 0 || (green & blue) != 0) return;

	sysfb_channel(red,   &info->red_pos,   &info->red_size);
	sysfb_channel(green, &info->green_pos, &info->green_size);
	sysfb_channel(blue,  &info->blue_pos,  &info->blue_size);
}


/* ------------------------------------------------------------------ */
/* The calls                                                           */
/* ------------------------------------------------------------------ */

/* exit(status) -- terminates the calling task. This is the one call that
*  must not return to its caller.
*
*  Returning normally would be pointless: the stub ends in an iret, and that
*  iret would put the CPU back on the instruction after the "int 0x80" of a
*  task that is supposed to be dead. So the same trick fault_handler() uses
*  is applied here -- the saved context is overwritten in place with the
*  context of the next runnable task, and the iret lands in that task
*  instead. The exiting task's own registers are never restored again. */
static int sys_exit(struct regs *r)
{
	int pid;
	int i;
	struct regs *next;
	int status;

	status = (int)r->ebx;
	pid = taskmgr_get_currpid();

	/* No task is running, so there is nothing to terminate. That means the
	*  kernel itself issued the call -- report it and let the caller carry
	*  on, rather than switching a context that does not exist. */
	if(pid < 0)
	{
		printf("exit() called without a running task\n");
		r->eax = (unsigned int)SYS_ENOSYS;
		return 0;
	}

	/* Anything the task still holds on the network side goes back now, while
	*  there is still a pid to look it up by. A program that returns without
	*  closing is the ordinary case, not the exceptional one, and four
	*  connections is few enough that leaking one matters. */
	sysnet_release_task(pid);

	/* And the screen, for the same reason and with one addition: this is the
	*  last moment at which the mapping can be removed cheaply and correctly.
	*  The task's address space is the ACTIVE one right now -- entering through
	*  a gate never reloads CR3 -- so the unmap below walks the very directory
	*  the pages are in and gets its invlpg for free, and the space is
	*  certainly still alive because the task is standing in it. A moment later
	*  it may not be: taskmgr_task_exit() makes the slot recyclable, and
	*  whoever recycles it hands the space back.
	*
	*  It is also not optional in the way a leaked connection is. A connection
	*  nobody closes costs one of four slots; a screen nobody gives back is a
	*  console printing into a shadow buffer that never reaches the monitor,
	*  i.e. a machine that looks dead to the person sitting in front of it. */
	sysfb_release_task(pid);

	/* The task is marked as ended and loses its remaining time slice, so the
	*  search below skips it. Ended, not aborted: a program that returns 0 has
	*  not failed, and "ps" saying it was aborted is read as a fault by exactly
	*  the person who went looking for one. */
	taskmgr_task_exit(pid, status);

	settextcolor(2,0);
	printf("Task %i exited with status %i\n", pid, status);
	settextcolor(15,0);

	/* schedule() hands back the context it was given until the current time
	*  slice is used up, so it is called until a different one comes out --
	*  bounded, because this runs inside an interrupt.
	*
	*  schedule() is also the only place that elects a task, and therefore the
	*  only place that loads CR3. When it returns a foreign context, the
	*  address space belonging to that context is already active. Nothing here
	*  switches anything, and nothing here should: this file is never told
	*  which space a task owns, and a second switch would only be an
	*  opportunity to disagree with the scheduler. */
	next = r;
	for(i = 0; i < SYSCALL_SCHEDULE_TRIES; i++)
	{
		next = schedule(r);
		if(next != r) break;
	}

	if(next != r)
	{
		/* The stub restores every register from exactly this memory and
		*  then irets. Replacing the contents therefore switches tasks
		*  without a single line of assembler.
		*
		*  The copy is indifferent to which address space is loaded while it
		*  runs, and that is not luck: r points into the exiting task's kernel
		*  stack and next into the incoming task's, and both stacks live above
		*  KERNEL_VIRTUAL_BASE, in the quarter of the directory every space
		*  shares. Same memory, same contents, in either space. The iret at
		*  the end of the stub is the first instruction that actually needs
		*  the new space -- it is the one that reaches user code again -- and
		*  by then schedule() has long since loaded it. */
		*r = *next;
		return 0;
	}

	/* Nothing left to switch to: there is no context the iret could return
	*  into that would not immediately be the dead task again. */
	settextcolor(4,0);
	printf("No runnable task left - CPU HALT\n");
	settextcolor(15,0);
	for(;;);

	return 0;
}

/* write(text) -- prints a NUL terminated string and returns the number of
*  characters written, or SYS_EFAULT if the pointer does not survive
*  user_string_len(). */
static int sys_write(struct regs *r)
{
	uint32_t addr;
	int len;
	int i;

	addr = (uint32_t)r->ebx;

	len = user_string_len(addr, SYS_WRITE_MAX);
	if(len < 0) return SYS_EFAULT;

	/* Validated above, so the bytes can be read without further checks. */
	for(i = 0; i < len; i++)
	{
		putch(*(const volatile unsigned char *)(addr + (uint32_t)i));
	}

	return len;
}

/* getpid() */
static int sys_getpid(struct regs *r)
{
	(void)r;
	return taskmgr_get_currpid();
}

/* sleep(ms) -- milliseconds. timer_wait() caps the value itself and ignores
*  anything <= 0, so no separate range check is needed here. */
static int sys_sleep(struct regs *r)
{
	sleep((int)r->ebx);
	return 0;
}

/* putch(c) -- a single character. No pointer involved, so nothing to
*  validate: every one of the 256 possible byte values is printable as far as
*  the VGA text buffer is concerned. */
static int sys_putch(struct regs *r)
{
	putch((unsigned char)(r->ebx & 0xFF));
	return 1;
}

/* uptime() -- milliseconds since boot. */
static int sys_uptime(struct regs *r)
{
	(void)r;
	return timer_get_ticks();
}

/* getch() -- waits for a key and returns it. The one call here that blocks on
*  something other than the clock.
*
*  Blocking inside a system call only works because vector 0x80 is a trap gate
*  and not an interrupt gate; syscall_install() at the bottom of this file says
*  why at length. An interrupt gate clears IF on entry, and a task waiting for
*  a keystroke with interrupts masked would be waiting for an IRQ that can
*  never be delivered -- by its own keyboard, and equally by the timer that
*  would otherwise let anybody else run. The gate type is what makes this call
*  possible at all, not a detail of it.
*
*  This used to be a hlt loop over getchn(), the non-blocking read, and it said
*  in this comment that a wait queue in tasks.c would be the right next step.
*  It is there now -- and the keyboard's end of it is not reachable from this
*  file. kb.c blocks on the address of last_key, which is a file static and is
*  in no header; system.h exports the driver's waiting read and nothing below
*  it. So the wait is not rebuilt here out of a channel this file would have to
*  invent, which is the one way to get a channel wrong that costs a task that
*  never runs again: getch() IS the blocking read, it is declared in system.h,
*  and calling it is how this call blocks.
*
*  What that changes is the cost of a shell sitting at its prompt. The old loop
*  left the task TASK_STATE_RUNNING, so the scheduler kept electing it for a
*  turn whose entire content was a read and a hlt, once per time slice for as
*  long as nobody typed. getch() puts the task in TASK_STATE_BLOCKED and the
*  scheduler passes it over until the keyboard handler wakes it.
*
*  Every case the old loop handled getch() handles too, and in the same shape:
*  a caller with no task running, or one that arrived with interrupts off,
*  falls back to exactly this hlt loop inside kb.c. It also keeps the property
*  this call needs -- it returns only a key that is really there, never 0 --
*  because its loop is over "last_key is empty" and it takes the key with
*  interrupts still off.
*
*  Two tasks can be inside it at once, the console and a ring 3 program, and
*  one key wakes both; the one that loses re-tests, finds nothing and blocks
*  again. That is the same single key slot the two shared before, with the same
*  consequence and no new one. */
static int sys_getch(struct regs *r)
{
	(void)r;

	return (int)getch();
}

/* peekch() -- the key that is waiting, or 0 when none is.
*
*  kb.c has no non-blocking read in system.h, but it does have one in the file:
*  getchn() is getch() without the wait, taking last_key and clearing it. So
*  nothing had to be built here beyond the local declaration at the top -- the
*  smallest thing that gives a non-blocking read, and it changes nothing in a
*  file this round does not own.
*
*  The 0 that means "nothing" is the driver's own EOS, which is also why the
*  keyboard tables map every key that has no character to 0: a modifier or a
*  function key is indistinguishable from no key at all through this call. That
*  is the existing behaviour of getch(), which loops past exactly those, and
*  not something introduced here. */
static int sys_peekch(struct regs *r)
{
	(void)r;
	return (int)getchn();
}

/* cls() -- clears the screen. No arguments, nothing to validate; scrn.c takes
*  its own console lock and picks the text buffer or the framebuffer itself. */
static int sys_cls(struct regs *r)
{
	(void)r;
	cls();
	return 0;
}

/* setcolor(foreground, background) -- the attribute everything printed from
*  here on is drawn with.
*
*  Both arguments are range checked rather than masked. settextcolor() ands the
*  foreground with 0x0F but shifts the background left by four unmasked, so an
*  out of range background does not stay in its nibble; more to the point, a
*  program that asks for colour 200 has made a mistake, and telling it so is
*  more useful than quietly drawing something else. The cast to int is what
*  makes the test bite: ebx holds an unsigned register value, and 0x80000000
*  read as unsigned would pass a "> 15" test only by wrapping expectations, but
*  read as int it is plainly negative. */
static int sys_setcolor(struct regs *r)
{
	int foreground;
	int background;

	foreground = (int)r->ebx;
	background = (int)r->ecx;

	if(foreground < 0 || foreground > 15) return SYS_EINVAL;
	if(background < 0 || background > 15) return SYS_EINVAL;

	settextcolor((unsigned char)foreground, (unsigned char)background);
	return 0;
}

/* Which ATA drive the mounted filesystem sits on.
*
*  fat.h exposes the type, the label and the sizes, but not the drive number it
*  mounted -- fat_drive is private to fat.c. kernel.c's disk_init() walks
*  drives 0..ATA_MAX_DRIVES-1 and stops at the first one that mounts, so the
*  first present drive is the answer in every configuration this kernel is
*  actually handed, which is one disk. Two drives where the first is present
*  but carries nothing readable would make this name the wrong one. It is
*  reported rather than left at -1 because -1 already means something else here
*  ("no filesystem"), and the honest fix is a fat_drive() accessor in fat.h --
*  which is not this file's to add. */
static int fs_drive(void)
{
	int drive;

	for(drive = 0; drive < ATA_MAX_DRIVES; drive++)
	{
		if(ata_present(drive)) return drive;
	}

	return -1;
}

/* fsinfo(out) -- what is mounted, if anything.
*
*  Nothing mounted is a normal state on this kernel and not a failure: "make
*  run" boots with no disk at all. So the answer is SYS_ENOENT *and* a filled
*  in structure -- zeroed, with empty strings and drive -1 -- rather than an
*  error code alone. A caller that checks the return value learns there is no
*  filesystem; a caller that does not still reads empty fields instead of
*  whatever its own stack happened to hold, which is the difference between a
*  program that prints nothing and a program that prints garbage. */
static int sys_statfs(struct regs *r)
{
	sys_fsinfo info;
	uint32_t out;

	out = (uint32_t)r->ebx;

	if(!user_range_ok(out, (uint32_t)sizeof(sys_fsinfo), 1)) return SYS_EFAULT;

	memset(&info, 0, sizeof(info));
	info.drive = -1;

	if(!fat_mounted())
	{
		copy_to_user(out, &info, (uint32_t)sizeof(info));
		return SYS_ENOENT;
	}

	field_copy(info.type,  (int)sizeof(info.type),  fat_type());
	field_copy(info.label, (int)sizeof(info.label), fat_label());
	info.total_bytes   = fat_total_bytes();
	info.free_bytes    = fat_free_bytes();
	info.cluster_bytes = fat_cluster_bytes();
	info.drive         = (int32_t)fs_drive();

	copy_to_user(out, &info, (uint32_t)sizeof(info));
	return 0;
}

/* stat(path, size) -- the size of a file in bytes.
*
*  Both arguments are checked before anything reaches the disk. That ordering
*  is deliberate: a bad pointer should cost the caller an error code, not a
*  directory walk, and a call that faults on its output pointer after having
*  read a sector has done work on behalf of an argument that was never valid.
*
*  fat.c collapses every failure into -1 and describes it only in
*  fat_last_error(), a string. So the distinction this call can honestly make
*  is the one it can ask about directly -- whether anything is mounted -- and
*  everything else is SYS_ENOENT, which covers the missing file, the path that
*  is a directory, and a disk that would not read. Splitting those apart needs
*  error codes fat.h does not have yet. */
static int sys_stat(struct regs *r)
{
	char path[SYS_PATH_MAX];
	uint32_t out;
	uint32_t size;

	out = (uint32_t)r->ecx;

	if(copy_string_from_user((uint32_t)r->ebx, path, SYS_PATH_MAX) < 0)
		return SYS_EFAULT;
	if(!user_range_ok(out, (uint32_t)sizeof(uint32_t), 1)) return SYS_EFAULT;

	if(!fat_mounted()) return SYS_ENOENT;

	size = 0;
	if(fat_size(path, &size) != 0) return SYS_ENOENT;

	copy_to_user(out, &size, (uint32_t)sizeof(size));
	return 0;
}

/* read(path, offset, len, buf) -- up to len bytes from offset, into buf.
*  Returns how many bytes were actually read.
*
*  This is the call the range checking above was written for. buf is a user
*  pointer the kernel WRITES through, len is a number ring 3 chose, and the two
*  together are the only place in this file where a single argument decides how
*  much memory the kernel touches.
*
*  The order is: clamp, then validate, then work. Clamping first matters --
*  user_range_ok() is asked about the length that will actually be written, so
*  a caller asking for four gigabytes is not refused for a buffer it never
*  needed, it is answered with SYS_READ_MAX bytes into a buffer that only has
*  to be that big. A short return is the ordinary read() contract, so a loop
*  that keeps calling until it gets 0 works without knowing the cap exists.
*
*  offset + len is never formed. That sum is where an obvious implementation
*  would overflow -- offset near 4 GiB plus a large len wraps to a small number
*  that looks like it is inside the file -- and it is avoided by never needing
*  it: fat_read() compares offset against the file size first and returns 0
*  past the end, then derives the length from the *difference*, which cannot
*  wrap because offset is already known to be smaller. The buffer check on this
*  side uses buf and len alone and does its own no-wrap test. There is no third
*  place where the two are added. */
static int sys_read(struct regs *r)
{
	char path[SYS_PATH_MAX];
	uint32_t offset;
	uint32_t len;
	uint32_t buf;
	uint32_t unused;
	int got;

	offset = (uint32_t)r->ecx;
	len    = (uint32_t)r->edx;
	buf    = (uint32_t)r->esi;

	if(copy_string_from_user((uint32_t)r->ebx, path, SYS_PATH_MAX) < 0)
		return SYS_EFAULT;

	if(len > SYS_READ_MAX) len = SYS_READ_MAX;

	if(!user_range_ok(buf, len, 1)) return SYS_EFAULT;

	/* A zero length read touches nothing, so it is answered without reaching
	*  the disk at all -- including for a path that does not exist. That is the
	*  same trade read() makes everywhere: asking for no bytes is not a way to
	*  ask whether a file is there, SYS_STAT is. */
	if(len == 0) return 0;

	if(!fat_mounted()) return SYS_ENOENT;

	/* fat_read() writes at most len bytes and never more: it clamps len
	*  against the remaining file size before it starts and copies sector
	*  fragments bounded by len - done. The destination is the caller's buffer
	*  directly rather than a bounce buffer, because every page of it has just
	*  been validated as writable from ring 3, and the only code that could
	*  invalidate that mapping is the caller -- which is parked in this call. */
	got = fat_read(path, offset, len, (void *)buf);
	if(got < 0)
	{
		/* fat.c reports a missing file and a disk that would not read with the
		*  same -1. Asking for the size afterwards separates them: if the file
		*  cannot be found either, this was SYS_ENOENT; if it can, the entry
		*  exists and the layer below refused to hand over its contents, which
		*  is what SYS_EIO is for. The extra directory walk is on the error
		*  path only. */
		if(fat_size(path, &unused) != 0) return SYS_ENOENT;
		return SYS_EIO;
	}

	return got;
}

/* readdir(path, index, out) -- entry number index of a directory.
*
*  Index based rather than an open/read/close trio, which is what makes the
*  call stateless: there is no handle to allocate, nothing to leak when a task
*  is aborted mid-listing, and no per-task table for the kernel to keep.
*
*  Running past the last entry is SYS_ENOENT, and that is how a caller knows to
*  stop -- fat_readdir() distinguishes "no more entries" (1) from an error
*  (negative), but both mean the same thing to a loop, and the header offers
*  one code for "no such file, or nothing is mounted" rather than two. */
static int sys_readdir(struct regs *r)
{
	char path[SYS_PATH_MAX];
	fat_dirent entry;
	sys_dirent result;
	uint32_t out;
	int index;

	index = (int)r->ecx;
	out   = (uint32_t)r->edx;

	if(copy_string_from_user((uint32_t)r->ebx, path, SYS_PATH_MAX) < 0)
		return SYS_EFAULT;
	if(!user_range_ok(out, (uint32_t)sizeof(sys_dirent), 1)) return SYS_EFAULT;

	if(index < 0) return SYS_EINVAL;
	if(!fat_mounted()) return SYS_ENOENT;

	memset(&entry, 0, sizeof(entry));
	if(fat_readdir(path, index, &entry) != 0) return SYS_ENOENT;

	/* Built here and copied out in one go, rather than written field by field
	*  through the user pointer. The two are not the same: fat_dirent is 13
	*  bytes of name and sys_dirent is 16, the flags word does not exist on the
	*  filesystem side at all, and a translation done in place would be a
	*  sequence of stores into memory that was checked once at the start. */
	memset(&result, 0, sizeof(result));
	field_copy(result.name, (int)sizeof(result.name), entry.name);
	result.size  = entry.is_dir ? 0u : entry.size;
	result.flags = entry.is_dir ? (uint32_t)SYS_DIRENT_DIR : 0u;

	copy_to_user(out, &result, (uint32_t)sizeof(result));
	return 0;
}

/* spawn(path, args, prio) -- loads a program from disk and starts it as a task
*  of its own. Returns the new pid; it does not wait for the program.
*
*  Both strings are copied onto this stack before exec.c sees them, for the
*  reason copy_string_from_user() gives: exec_spawn_path() builds a second
*  address space, and a ring 3 pointer is a statement about the caller's.
*
*  The priority is bounded at TASK_PRIORITY_HIGH rather than at
*  TASK_PRIORITY_REALTIME. The scheduler hands out time slices in proportion to
*  priority, so a realtime task obtained from ring 3 is a program that can
*  starve the shell it was started from, and the ability to ask for one is not
*  something a user program should get merely by passing a bigger number. It is
*  refused rather than silently clamped: a clamp would report success for a
*  request that was not granted. */
static int sys_spawn(struct regs *r)
{
	char path[SYS_PATH_MAX];
	char args[SYS_ARGS_MAX];
	const char *argp;
	uint32_t unused;
	int prio;
	int pid;

	prio = (int)r->edx;

	if(copy_string_from_user((uint32_t)r->ebx, path, SYS_PATH_MAX) < 0)
		return SYS_EFAULT;

	/* A null args pointer means "no command line" and is passed straight
	*  through as one, so exec.c can tell it apart from an empty string. */
	argp = 0;
	if(r->ecx != 0)
	{
		if(copy_string_from_user((uint32_t)r->ecx, args, SYS_ARGS_MAX) < 0)
			return SYS_EFAULT;
		argp = args;
	}

	if(prio < TASK_PRIORITY_LOW || prio > TASK_PRIORITY_HIGH) return SYS_EINVAL;

	if(!fat_mounted()) return SYS_ENOENT;

	pid = exec_spawn_path(path, argp, prio);
	if(pid < 0)
	{
		/* Same split as SYS_READ, and for the same reason: exec.c reports one
		*  negative value and puts the detail in exec_last_error(). A path that
		*  cannot be found at all is SYS_ENOENT; a path that can is a program
		*  that failed to load, and out of memory or out of task slots is what
		*  the header's SYS_ENOMEM was written for. A malformed ELF ends up in
		*  the same bucket, which is as fine a distinction as this file can
		*  make without error codes in exec.h. */
		if(fat_size(path, &unused) != 0) return SYS_ENOENT;
		return SYS_ENOMEM;
	}

	/* The pid may be a reused slot, and a slot is reused after its previous
	*  occupant was aborted -- which is the one way a task leaves without
	*  passing through sys_exit(). Anything still recorded against this number
	*  belongs to that dead task and must not be inherited by this one. */
	sysnet_release_task(pid);
	sysfb_release_task(pid);

	/* exec_spawn_path() hands the task back SUSPENDED on purpose, so that the
	*  shell can look at it -- or take it back down -- before it has executed
	*  anything. A system call has nothing to look at and nobody to ask, and
	*  the call is named spawn(): a caller that got a pid but no running task
	*  would have no way to start one, because there is no SYS_START. So this
	*  is the last step of the call rather than the caller's job. */
	taskmgr_task_start(pid);

	return pid;
}


/* Why a network call cannot even be attempted, or 0 if it can.
*
*  Three separate things have to be true before any of this works and none of
*  them is a program's fault: a card was found, an address was configured, and
*  -- for a name -- a resolver was learned. They collapse to one error code
*  because syscall.h offers one, but they are asked in order so that the
*  earliest missing piece is the one that decides, and they are asked BEFORE
*  anything is attempted so that "there is no network" comes back immediately
*  instead of after a ten second timeout.
*
*  That immediacy is the point of the whole function. SYS_ENETDOWN and
*  SYS_ETIMEDOUT are the two answers a program most needs to tell apart: the
*  first means asking again later is pointless until something is configured,
*  the second means this particular host did not answer and another one might.
*  A machine with no card that timed out would look exactly like a network with
*  a dead server. */
static int sysnet_down(int need_dns)
{
	if(!net_up()) return SYS_ENETDOWN;
	if(net_ip() == IP_ADDR_ANY) return SYS_ENETDOWN;
	if(need_dns && dhcp_dns() == IP_ADDR_ANY) return SYS_ENETDOWN;

	return 0;
}

/* resolve(name, uint32_t *ip) -- a host name to an address, in host order.
*
*  The address is written through a user pointer, so both arguments are checked
*  before anything reaches the wire, and for the reason sys_stat() gives: a bad
*  pointer should cost the caller an error code, not a lookup.
*
*  The cache is asked first even though dns_resolve() would consult it too.
*  That is not a duplicated optimisation -- it is what keeps a cached name from
*  having to queue behind another task's lookup for the resolver dns.c only has
*  one of. A hit answers without blocking at all, which is the common case for
*  the second and third connection a program makes.
*
*  The two waits are separate and bounded separately: getting the resolver, and
*  then getting an answer out of it. See the SYS_NET_RESOLVE_* comments for the
*  numbers.
*
*  DNS_STATE_FAILED is answered with SYS_ENOENT rather than SYS_ETIMEDOUT, and
*  the distinction is worth keeping even though dns.c cannot make it fully.
*  SYS_ETIMEDOUT is reserved for OUR bound running out, so a caller can tell
*  "the resolver reached a conclusion and it was no" from "nobody concluded
*  anything in the time allowed". What dns.c collapses into that one state is a
*  name that does not exist and a server that never answered; separating those
*  needs an error code dns.h does not have, and only dns_last_error() has the
*  words. */
static int sys_resolve(struct regs *r)
{
	char name[SYS_HOST_MAX];
	uint32_t out;
	uint32_t ip;
	uint32_t start;
	int down;
	int state;
	int held;
	int started;

	out = (uint32_t)r->ecx;

	if(copy_string_from_user((uint32_t)r->ebx, name, SYS_HOST_MAX) < 0)
		return SYS_EFAULT;
	if(!user_range_ok(out, (uint32_t)sizeof(uint32_t), 1)) return SYS_EFAULT;

	if(name[0] == '\0') return SYS_EINVAL;

	sysnet_reap();

	down = sysnet_down(1);
	if(down != 0) return down;

	ip = dns_lookup_cached(name);
	if(ip != IP_ADDR_ANY)
	{
		copy_to_user(out, &ip, (uint32_t)sizeof(ip));
		return 0;
	}

	/* Phase one: the resolver itself. The lock keeps two tasks from
	*  interleaving lookups; dns_resolve() failing while dns_state() is
	*  DNS_STATE_QUERY keeps us behind a lookup the shell started without
	*  going through this gate at all, which the lock cannot see. */
	held    = 0;
	started = 0;
	start   = (uint32_t)timer_get_ticks();

	while(sysnet_ms_since(start) < SYS_NET_RESOLVE_BUSY)
	{
		if(!held) held = sysnet_dns_take();

		if(held)
		{
			if(dns_resolve(name) == 0)
			{
				started = 1;
				break;
			}

			if(dns_state() != DNS_STATE_QUERY)
			{
				/* Not contention: dns.c refused this name or has nowhere to
				*  ask. The configuration is re-tested because it can have
				*  changed while we waited -- a lease can expire. */
				sysnet_dns_release();

				down = sysnet_down(1);
				if(down != 0) return down;

				return SYS_EINVAL;
			}
		}

		sysnet_pump();
		sysnet_reap();
	}

	/* Holding the lock is not the same as having asked. The budget can run out
	*  with the lock in hand and the resolver still busy with a lookup the shell
	*  started without passing through this gate, and going on to phase two then
	*  would mean collecting somebody else's answer under our own name -- the
	*  exact confusion the lock exists to prevent, arrived at from the other
	*  side. Only a dns_resolve() that returned 0 means the query in flight is
	*  ours. */
	if(!started)
	{
		if(held) sysnet_dns_release();
		return SYS_ETIMEDOUT;
	}

	/* Phase two: the answer. dns_poll() is the only thing that retransmits and
	*  the only thing that ever gives up, so it is called on every turn and its
	*  own conclusion is preferred to ours. */
	ip    = IP_ADDR_ANY;
	start = (uint32_t)timer_get_ticks();

	for(;;)
	{
		state = dns_poll();

		if(state == DNS_STATE_DONE)
		{
			ip = dns_result();
			break;
		}

		if(state == DNS_STATE_FAILED)
		{
			sysnet_dns_release();
			return SYS_ENOENT;
		}

		if(sysnet_ms_since(start) >= SYS_NET_RESOLVE_MS)
		{
			/* Abandon the query rather than leaving it in flight. A lookup
			*  nobody is waiting for would otherwise hold the resolver against
			*  the next caller for the rest of its own schedule. */
			dns_cancel();
			sysnet_dns_release();
			return SYS_ETIMEDOUT;
		}

		sysnet_pump();
	}

	sysnet_dns_release();

	if(ip == IP_ADDR_ANY) return SYS_ENOENT;

	/* Checked once on the way in and once here. The invariant that makes one
	*  check enough elsewhere in this file -- only the caller edits the caller's
	*  half, and the caller is parked inside this call -- still holds, but it is
	*  being leaned on for up to eighteen seconds rather than for the length of
	*  a disk read, and the re-test is two compares and a page table walk. */
	if(!user_range_ok(out, (uint32_t)sizeof(ip), 1)) return SYS_EFAULT;

	copy_to_user(out, &ip, (uint32_t)sizeof(ip));
	return 0;
}

/* connect(ip, port) -- opens a connection and waits for the handshake.
*
*  tcp_connect() does not wait: it puts a SYN on the wire and the handshake
*  finishes from the card's interrupt. So the kernel side of this call is the
*  loop tcp.h describes -- watch tcp_state(), call tcp_poll(), sleep -- with a
*  bound on it, because the interesting failure is a port that is filtered
*  rather than refused and therefore answers nothing at all.
*
*  The table entry is reserved BEFORE tcp_connect() rather than after. A
*  connection that is opened and then has nowhere to be recorded would have to
*  be torn down again, and doing that in the window where the peer may already
*  have answered is more moving parts than reserving a slot that costs nothing
*  to give back.
*
*  Address zero and the broadcast address are refused. Neither is a host: one
*  is "no address" and the other is delivered to every station on the wire,
*  where a TCP handshake means nothing. Everything else is passed through,
*  including addresses that will never answer -- that is what the timeout is
*  for, and it is not this file's business to have opinions about which parts
*  of the address space are worth talking to. */
static int sys_connect(struct regs *r)
{
	uint32_t ip;
	uint32_t start;
	int port;
	int slot;
	int khandle;
	int state;
	int down;
	int i;

	ip   = (uint32_t)r->ebx;
	port = (int)r->ecx;

	if(ip == IP_ADDR_ANY || ip == IP_ADDR_BROADCAST) return SYS_EINVAL;
	if(port <= 0 || port > SYS_PORT_MAX) return SYS_EINVAL;

	sysnet_reap();

	down = sysnet_down(0);
	if(down != 0) return down;

	slot = -1;
	for(i = 0; i < SYS_NET_HANDLE_MAX; i++)
	{
		if(!sysnet_conns[i].used)
		{
			slot = i;
			break;
		}
	}
	if(slot < 0) return SYS_ENOMEM;

	memset(&sysnet_conns[slot], 0, sizeof(sysnet_conns[slot]));
	sysnet_conns[slot].used  = 1;
	sysnet_conns[slot].owner = taskmgr_get_currpid();

	khandle = tcp_connect(ip, (uint16_t)port);
	if(khandle < 0)
	{
		sysnet_conns[slot].used = 0;

		/* TCP_ENOCONN is "all four are in use", which is the same shape of
		*  failure as a full task table and is what SYS_ENOMEM says here.
		*  Anything else out of tcp_connect() means the stack below is not in a
		*  state to open anything, since the arguments were checked above. */
		if(khandle == TCP_ENOCONN) return SYS_ENOMEM;
		return SYS_ENETDOWN;
	}

	sysnet_remember(slot, khandle, ip, (uint16_t)port);

	start = (uint32_t)timer_get_ticks();

	for(;;)
	{
		state = tcp_state(khandle);

		if(state == TCP_ESTABLISHED) break;

		/* Reaching TCP_CLOSED during a handshake is a refusal: a RST came
		*  back, or tcp.c gave up on its own. The slot is dropped without
		*  calling tcp_abort(), and that is deliberate rather than an
		*  omission. tcp.h says a handle stays valid until TCP_CLOSED, so at
		*  TCP_CLOSED it is already tcp.c's to reuse -- and aborting a number
		*  that has since been handed to another task's connect() would tear
		*  down a stranger's connection. Not reclaiming something that is
		*  already free is the safe side of that trade. */
		if(state < 0 || state == TCP_CLOSED)
		{
			sysnet_conns[slot].used = 0;
			return SYS_ECONNRESET;
		}

		if(sysnet_ms_since(start) >= SYS_NET_CONNECT_MS)
		{
			/* Half open on this side and unknown on the other. tcp_abort()
			*  frees the slot at once and tells a peer that did hear the SYN
			*  that the connection failed, rather than leaving it to time out
			*  a connection we have already given up on. */
			tcp_abort(khandle);
			sysnet_conns[slot].used = 0;
			return SYS_ETIMEDOUT;
		}

		sysnet_pump();
	}

	/* The local port is only final once the connection exists, so identity is
	*  recorded again now that it does. */
	sysnet_remember(slot, khandle, ip, (uint16_t)port);

	return slot + 1;
}

/* send(handle, buf, len) -- queues data, returns how many bytes were taken.
*
*  A short return is the contract, not a failure: tcp_send() takes what fits in
*  a send buffer that only empties when the peer acknowledges, so a caller
*  loops. What this call adds is that it does not return ZERO -- it waits until
*  at least one byte could be taken, or until the bound runs out. A send that
*  could return 0 would leave a user program with nothing to do but call again
*  immediately, which is a busy loop in ring 3, and ring 3 has no way to sleep
*  between attempts that is any better than the one this loop already uses.
*
*  The user buffer is handed to tcp_send() directly rather than bounced through
*  the kernel stack, for the reason sys_read() gives about fat_read(): every
*  page of it has just been validated as readable from ring 3, tcp_send() copies
*  it into the connection's own buffer synchronously, and the only code that
*  could invalidate the mapping is the caller -- which is parked in this call. */
static int sys_send(struct regs *r)
{
	uint32_t buf;
	uint32_t len;
	uint32_t start;
	int slot;
	int khandle;
	int state;
	int took;

	buf = (uint32_t)r->ecx;
	len = (uint32_t)r->edx;

	slot = sysnet_slot_of((int)r->ebx);
	if(slot < 0) return SYS_EINVAL;

	if(len > SYS_NET_XFER_MAX) len = SYS_NET_XFER_MAX;
	if(!user_range_ok(buf, len, 0)) return SYS_EFAULT;

	/* Nothing to take, and no reason to wait for room to put it in. Zero is
	*  unambiguous here in a way it is not for SYS_RECV: this call's zero means
	*  "no bytes moved", which is exactly what happened. */
	if(len == 0) return 0;

	khandle = sysnet_conns[slot].khandle;
	start   = (uint32_t)timer_get_ticks();

	for(;;)
	{
		if(!sysnet_identity_ok(slot)) return SYS_ECONNRESET;

		/* TCP_CLOSE_WAIT is not an error to send in: the peer has closed its
		*  direction and ours is still open, which is a half open connection
		*  and a legal one. Anything else means the stream is finished. */
		state = tcp_state(khandle);
		if(state != TCP_ESTABLISHED && state != TCP_CLOSE_WAIT)
			return SYS_ECONNRESET;

		took = tcp_send(khandle, (const void *)buf, len);
		if(took > 0) return took;
		if(took < 0) return SYS_ECONNRESET;

		if(sysnet_ms_since(start) >= SYS_NET_SEND_MS) return SYS_ETIMEDOUT;

		sysnet_pump();
	}
}

/* recv(handle, buf, len) -- bytes read, or 0 once the stream has ended.
*
*  This call exists to map two different kernel answers onto one user-visible
*  convention, and getting that mapping backwards is the difference between a
*  program that hangs forever and one that truncates every page it fetches.
*
*  tcp_recv() says three things with numbers that look alike:
*    - a positive count: this much arrived and was copied.
*    - 0: nothing RIGHT NOW. The connection is open, the peer simply has not
*      sent anything yet. This is not an answer to give back to ring 3 -- a
*      program that read 0 as "the stream ended" would stop at the first pause
*      in the transfer and truncate whatever it was fetching.
*    - TCP_ECLOSED: the peer has closed AND everything it sent has been read.
*      This is the end, and it is deliberately the LAST thing tcp_recv() says
*      rather than the first: data that arrived before the FIN is still handed
*      over first, so the tail of the stream is not lost to the close.
*
*  So the loop below returns the count for a count, waits when told 0, and
*  returns 0 only for TCP_ECLOSED. The zero that crosses the gate means the
*  stream ended -- the one thing a caller can loop on until it stops -- and the
*  zero that comes up from tcp.c never does.
*
*  A zero LENGTH request is refused with SYS_EINVAL rather than answered with
*  0, which is the one place this call departs from read(). Here 0 is not "no
*  bytes moved", it is "there will never be any more", and a call that returned
*  it for an empty buffer would be telling a caller the connection is finished
*  when it is not. read() has the same ambiguity and lives with it because its
*  0 is rarer; this one is the whole signal.
*
*  What cannot be distinguished is an orderly close from a reset that arrived
*  after everything readable had been read: tcp.h reports both as TCP_ECLOSED,
*  so both end the stream here. Telling them apart needs a signal tcp.h does not
*  offer, and the shape of the fix is a separate query, not a fourth meaning for
*  this return value. */
static int sys_recv(struct regs *r)
{
	uint32_t buf;
	uint32_t len;
	uint32_t start;
	int slot;
	int khandle;
	int got;

	buf = (uint32_t)r->ecx;
	len = (uint32_t)r->edx;

	slot = sysnet_slot_of((int)r->ebx);
	if(slot < 0) return SYS_EINVAL;

	if(len == 0) return SYS_EINVAL;
	if(len > SYS_NET_XFER_MAX) len = SYS_NET_XFER_MAX;
	if(!user_range_ok(buf, len, 1)) return SYS_EFAULT;

	khandle = sysnet_conns[slot].khandle;
	start   = (uint32_t)timer_get_ticks();

	for(;;)
	{
		/* Asked before the read, not after. A handle that has been recycled
		*  under us would otherwise have already copied somebody else's bytes
		*  into this program's buffer by the time we noticed. A connection the
		*  peer has merely closed is still listed and still ours, so this does
		*  not swallow the end of the stream -- tcp.c only frees the slot once
		*  we close as well. */
		if(!sysnet_identity_ok(slot)) return SYS_ECONNRESET;

		got = tcp_recv(khandle, (void *)buf, len);

		if(got > 0) return got;
		if(got == TCP_ECLOSED) return 0;
		if(got < 0) return SYS_ECONNRESET;

		if(sysnet_ms_since(start) >= SYS_NET_RECV_MS) return SYS_ETIMEDOUT;

		sysnet_pump();
	}
}

/* close(handle) -- closes our direction and gives the handle back.
*
*  Two things end here and they end at different times, which is what the wait
*  is about. The HANDLE ends immediately and unconditionally: whatever happens
*  below, the slot is released and the number ring 3 was holding stops naming
*  anything. The CONNECTION ends when TCP says so, and TCP says so at its own
*  pace -- our FIN has to be acknowledged, and after that TIME_WAIT runs for
*  twice the maximum segment lifetime so that a lost final acknowledgement can
*  still be resent.
*
*  Waiting for the connection would therefore mean waiting up to a minute for a
*  call that has nothing left to tell the caller, which is precisely the wait
*  that makes a shell look hung. So the bound covers the round trip and nothing
*  more: leave FIN_WAIT_1, CLOSING or LAST_ACK and we are done, because from
*  TIME_WAIT onwards tcp.c finishes on its own.
*
*  If the bound runs out with the FIN still unacknowledged -- or with the peer
*  keeping its own direction open in FIN_WAIT_2, where nothing will ever read
*  what it sends again, because the handle that could is gone -- the connection
*  is aborted. That is not impatience: there are four connections on the
*  machine, nothing outside a network system call drives tcp_poll(), and a slot
*  held by a connection no program can reach is a slot the next connect() will
*  be refused for. A RST also tells a peer that is still sending to stop.
*
*  The return is 0 in every one of those cases. The caller closed; the close
*  happened. Reporting how gracefully it happened would be information no
*  program can act on -- there is no handle left to retry with. */
static int sys_close(struct regs *r)
{
	uint32_t start;
	int slot;
	int khandle;
	int state;

	slot = sysnet_slot_of((int)r->ebx);
	if(slot < 0) return SYS_EINVAL;

	khandle = sysnet_conns[slot].khandle;

	if(sysnet_identity_ok(slot))
	{
		tcp_close(khandle);

		start = (uint32_t)timer_get_ticks();

		for(;;)
		{
			state = tcp_state(khandle);

			if(state < 0 || state == TCP_CLOSED || state == TCP_TIME_WAIT)
				break;

			if(sysnet_ms_since(start) >= SYS_NET_CLOSE_MS)
			{
				tcp_abort(khandle);
				break;
			}

			sysnet_pump();
		}
	}

	sysnet_conns[slot].used = 0;
	return 0;
}


/* ------------------------------------------------------------------ */
/* Writing to the filesystem                                           */
/* ------------------------------------------------------------------ */

/* These four are the first calls in this file that can DESTROY something.
*
*  Everything above this line is additive, or is the caller's own. SYS_WRITE
*  prints, SYS_SPAWN creates a task, a network call touches nobody else's data,
*  and the worst a mistaken SYS_READ does is fill the caller's own buffer with
*  the wrong bytes. A path handed to SYS_UNLINK names a file that is gone
*  afterwards; a path handed to SYS_FWRITE names bytes that are overwritten.
*  fat.c writes sectors, and there is no undo below this line.
*
*  So these calls have to answer a question the read side never raised: WHAT
*  MAY RING 3 NAME?
*
*  The honest answer on this kernel is "anything the filesystem accepts", and
*  it is worth being blunt about why, because that answer looks negligent
*  until the alternatives have been looked at.
*
*  There are no permissions here. No owner, no mode bits, no read-only
*  attribute honoured, no notion of a file the system needs -- fat.c has none
*  of that and neither does tasks.c, so there is nothing for this file to
*  consult. A program is at this gate because somebody typed its name at the
*  shell, so it acts with exactly the authority that person has, which is all
*  of it. That is the bargain MS-DOS made, and it is the bargain any system
*  without accounts is making whether it admits it or not.
*
*  Concretely, and the case worth stating rather than leaving to be discovered:
*  a program CAN unlink /BIN/LS.ELF, and afterwards the shell will not find
*  "ls" any more. The obvious response is to special case that directory here.
*  It was considered and turned down, for two reasons.
*
*  It would be the shape of a boundary and not a boundary. The only rule
*  available at this gate is a comparison against a path, and a path has many
*  spellings: /bin/ls.elf, //BIN/LS.ELF, /BIN/../BIN/LS.ELF. fat.c's resolver
*  folds those together and a comparison here would not, so the check would
*  turn down the honest caller and let through the one that is trying. A check
*  that is defeated by typing the argument differently is worse than no check
*  at all, because everybody who comes afterwards reads it as protection.
*
*  And it would protect the wrong verb. Denying SYS_UNLINK on /BIN still leaves
*  SYS_TRUNCATE and SYS_FWRITE, either of which turns the same file into
*  something the loader refuses -- the command is just as gone. Covering all
*  four means teaching this file what fat.c means by a path, which is a second
*  implementation of the filesystem's own name rules, kept in step by hand.
*
*  What IS enforced here is the boundary this gate actually owns, and it is
*  unchanged from the read side: a path is a bounded, terminated string in the
*  CALLER's address space, and a buffer is a range of the caller's own pages.
*  Ring 3 cannot make the kernel read kernel memory, cannot make it walk off
*  the end of a mapping, and cannot make it spend an unbounded time inside one
*  trap. Memory isolation is a promise this file can keep on its own. Data
*  protection is not, and twenty lines of path matching pretending otherwise
*  would only be discovered to be absent at the worst moment.
*
*  The real next step is not a check here. It is an attribute in fat.c and an
*  owner in tasks.c -- a permission system -- and until there is one the kernel
*  should say plainly that there is not. */

/* Copies the path argument of one of the four calls below into dst, which
*  holds SYS_PATH_MAX bytes. Returns 0, or the error the caller should return.
*
*  The copy is the read side's -- see copy_string_from_user() for why the bytes
*  are taken across rather than the pointer -- plus the one rule about names
*  that belongs at this gate rather than in fat.c: an empty path, and the root
*  directory spelled as "/", are refused. Both are how fat.c names the root,
*  and the root is not a file to create, write into, empty or remove. They are
*  turned down here rather than left to fail below because failing below costs
*  a directory walk and comes back as a -1 whose meaning this file would then
*  have to guess.
*
*  Every other rule about a name -- eight characters, an optional dot and three
*  more, the character set FAT allows -- stays in fat.c, which refuses a name
*  it cannot represent instead of mangling it (see fat.h). A second copy of
*  those rules here would buy nothing and would be one edit away from
*  disagreeing with the first. */
static int sysfs_path(uint32_t addr, char *dst)
{
	if(copy_string_from_user(addr, dst, SYS_PATH_MAX) < 0) return SYS_EFAULT;

	if(dst[0] == '\0') return SYS_EINVAL;
	if(dst[0] == '/' && dst[1] == '\0') return SYS_EINVAL;

	return 0;
}

/* Whether there is anything to write to at all, asked of the one function that
*  answers exactly that.
*
*  Nothing mounted is the ORDINARY case on this kernel, not an exceptional one:
*  "make run" boots with no disk and every one of these four calls has to have
*  a sensible answer there. SYS_EROFS is that answer, and it is a different
*  thing from the SYS_ENOENT the read side gives -- "there is no file" invites
*  a program to try another name, "there is nowhere to write" tells it to stop
*  asking.
*
*  fat_writable() rather than fat_mounted(), and the difference is not
*  theoretical: fat.h defines it as mounted AND the drive answered an identify
*  AND the geometry left room to write back. A volume that is mounted and
*  cannot be written to would pass the weaker test and then fail after the
*  caller had already been told to go ahead, which fat.h calls out as the worse
*  answer of the two. */
static int sysfs_rofs(void)
{
	if(!fat_writable()) return SYS_EROFS;

	return 0;
}

/* Non-zero if the path names a directory.
*
*  All four calls below need this on their error path, and they need it because
*  of a property of fat_size(): it answers about FILES. A path that names a
*  directory is refused by it exactly as a path that names nothing is, so the
*  existence probe the read side uses -- ask for the size, and take a failure to
*  mean the file is not there -- reports SYS_ENOENT for a directory that is
*  plainly there. That is not a limitation worth passing on to ring 3: telling a
*  program that /BIN does not exist when what happened is that /BIN cannot be
*  unlinked sends it looking for the wrong problem.
*
*  fat_readdir() is the one function in fat.h that answers about a directory as
*  such. It takes a directory path and refuses anything else, so index 0 is a
*  type test: 0 or 1 (filled in, or an empty directory) means a directory, and
*  negative means it is not one -- including the path that names nothing, which
*  is why the probe is asked FIRST and the size probe only afterwards. The walk
*  costs the same as the one fat_size() would have done, and it happens on the
*  error path only.
*
*  fat_dirent is filled in and thrown away. It is 24 bytes on a 4 KiB kernel
*  stack, in a function that calls nothing deeper than fat.c's own directory
*  walk -- the same cost sys_readdir() already carries on its ordinary path.
*
*  Worth stating because it is a difference between the two halves of this
*  file: the READ side does not do this. SYS_STAT and SYS_READ answer
*  SYS_ENOENT for a directory, and say in their own comments that they cannot
*  do better with the error codes fat.h offers -- which was true when they were
*  written, and is why cat.c re-asks with sys_readdir() out in ring 3. The
*  write side can do better because it already has to ask a second question on
*  its error path, so the probe is nearly free here, and because getting it
*  wrong is worse: telling a program that a path it can plainly see does not
*  exist, on the one call that would have destroyed it, is the kind of answer
*  somebody acts on. The two sides therefore disagree about a directory until
*  the read side is brought up to this, and a caller must not assume they
*  agree. */
static int sysfs_is_dir(const char *path)
{
	fat_dirent entry;

	return fat_readdir(path, 0, &entry) >= 0;
}

/* fcreate(path) -- creates an empty file. Returns 0 or a negative code.
*
*  Fails when the file is already there, which is fat_create()'s contract kept
*  rather than smoothed over. A "create" that quietly meant "create or empty"
*  is how a program destroys a file it only meant to name, and the caller is
*  the one with the context to decide: SYS_EEXIST is a code it acts on, by
*  asking, by picking another name, or by saying truncate(path, 0) and meaning
*  it.
*
*  fat.c reports every failure as -1 and puts the words in fat_last_error(), so
*  the split here is made by asking a second question: does the path name
*  something now? A file of that name, or a directory of that name, both mean
*  the create failed because the name is taken, and both are SYS_EEXIST -- a
*  caller acts on either of them the same way, by choosing another name.
*
*  What is left is SYS_EINVAL: a name that does not survive the 8.3 conversion,
*  a directory on the way to it that does not exist, or a root directory with no
*  free slot left. The first is much the most likely and is genuinely an
*  argument that makes no sense; splitting the other two out needs error codes
*  fat.h does not have, the same limit sys_stat() records. */
static int sys_fcreate(struct regs *r)
{
	char path[SYS_PATH_MAX];
	uint32_t unused;
	int err;

	err = sysfs_path((uint32_t)r->ebx, path);
	if(err != 0) return err;

	err = sysfs_rofs();
	if(err != 0) return err;

	if(fat_create(path) != 0)
	{
		if(fat_size(path, &unused) == 0) return SYS_EEXIST;
		if(sysfs_is_dir(path)) return SYS_EEXIST;

		return SYS_EINVAL;
	}

	return 0;
}

/* fwrite(path, offset, len, buf) -- writes len bytes at offset. Returns how
*  many bytes were written.
*
*  SYS_READ's mirror image, and the three places where a mirror image is not a
*  copy are what this comment is about.
*
*  FIRST, the direction of the pointer. buf is READ through, never written
*  through, so the check is user_range_ok(buf, len, 0) -- the readable one,
*  user_byte_ok() per page, PAGE_PRESENT and PAGE_USER and no PAGE_WRITE.
*  SYS_READ's writable check is not the stricter version of that, it is the
*  answer to a different question, and using it here would be wrong twice over.
*  It would ask something this call does not raise: the kernel never stores
*  into this buffer, so whether the CALLER may store into it is none of the
*  kernel's business. And it would refuse the ordinary case -- exec.c maps a
*  PT_LOAD without PF_W as PAGE_USER and not PAGE_WRITE, so a program writing a
*  string literal out of its own read-only data would be handed SYS_EFAULT for
*  a pointer that is perfectly good to read from. The direction of the copy
*  picks the check; nothing else does.
*
*  SECOND, the length. Capped at SYS_FWRITE_MAX, for the reason set out there,
*  and capped BEFORE the range check for the reason sys_read() gives: the
*  question put to user_range_ok() is about the bytes that will actually be
*  touched, so a caller asking for four gigabytes is not refused over a buffer
*  it never needed -- it is answered with a page out of a buffer that only has
*  to be a page long.
*
*  THIRD, and the one that decides whether a program can use this call at all:
*  a SHORT WRITE IS NOT AN ERROR. fat_write() returns how many bytes landed,
*  which is short exactly when the volume filled up in the middle, and that
*  number is the only thing telling a caller where to carry on from. It is
*  passed through untouched. Folding it into an error code would tell a program
*  nothing was written when most of it was -- and would tell it that after the
*  bytes had already reached the disk, so the file and the return value would
*  disagree.
*
*  The one count that does not cross unchanged is zero out of a request for
*  bytes, which becomes SYS_ENOSPC. Zero is not partial progress a caller can
*  build on, it is no progress at all, and as a return value it would be
*  indistinguishable from the zero length request handled below -- so the
*  obvious loop, "write what is left and advance by the result", would call
*  again with the same arguments forever. fat.c reports its failures
*  negatively, so a zero out of a non-zero request leaves one cause, and it is
*  the one SYS_ENOSPC names. The pair of rules is what matters: never turn a
*  short write into an error, and never turn a stall into a zero.
*
*  offset + len is never formed, for the reason sys_read() spells out. The
*  buffer check uses buf and len alone and does its own no-wrap test, and the
*  offset is compared against the size of the volume rather than added to
*  anything. */
static int sys_fwrite(struct regs *r)
{
	char path[SYS_PATH_MAX];
	uint32_t offset;
	uint32_t len;
	uint32_t buf;
	uint32_t total;
	uint32_t unused;
	int err;
	int wrote;

	offset = (uint32_t)r->ecx;
	len    = (uint32_t)r->edx;
	buf    = (uint32_t)r->esi;

	err = sysfs_path((uint32_t)r->ebx, path);
	if(err != 0) return err;

	if(len > SYS_FWRITE_MAX) len = SYS_FWRITE_MAX;

	if(!user_range_ok(buf, len, 0)) return SYS_EFAULT;

	err = sysfs_rofs();
	if(err != 0) return err;

	/* Nothing to write, so nothing is written and the file is neither created
	*  nor extended on the way past.
	*
	*  Unlike SYS_READ this is answered AFTER the mount test rather than before
	*  it, and the difference is deliberate. A zero length read asks for no
	*  bytes and gets none, which is true whatever the disk is doing. A zero
	*  length write answered 0 on a machine with no disk would be reporting
	*  success from a call whose entire purpose is to put bytes somewhere, and
	*  the program that writes an empty buffer first to see whether it may write
	*  at all deserves to be told SYS_EROFS. */
	if(len == 0) return 0;

	/* An offset past the end of the volume can never be inside a file on it,
	*  so the write must fail -- but fat_write() would find that out by
	*  zero-filling the gap, which means spending every free cluster on the
	*  disk on zeroes before returning 0. A typo in an offset would consume the
	*  whole volume. Refusing here costs one comparison and is the same answer.
	*
	*  Guarded on total being non-zero so that a filesystem which does not
	*  report a size cannot turn every write into SYS_ENOSPC. What is left
	*  unbounded is the legitimate case -- an offset just inside the volume
	*  still zero-fills up to the end of it -- and that bound belongs in fat.c,
	*  which is the only code that knows how far it has got. */
	total = fat_total_bytes();
	if(total != 0 && offset >= total) return SYS_ENOSPC;

	/* The caller's buffer is handed to fat_write() directly rather than bounced
	*  through the kernel stack, for the reason sys_read() gives: every page of
	*  it has just been validated as readable from ring 3, and the only code
	*  that could invalidate that mapping is the caller -- which is parked
	*  inside this call. */
	wrote = fat_write(path, offset, len, (const void *)buf);
	if(wrote < 0)
	{
		/* The same split as SYS_READ, on the same evidence, with the directory
		*  asked about first for the reason sysfs_is_dir() gives: a directory is
		*  not a file to write bytes into and saying so is more use than
		*  SYS_ENOENT about a path that is plainly there. Then, if the path
		*  cannot be sized either, there was no file to write to; and if it can,
		*  the entry exists and the layer below refused to store the bytes,
		*  which is what SYS_EIO is for. All of it is on the error path only. */
		if(sysfs_is_dir(path)) return SYS_EINVAL;
		if(fat_size(path, &unused) != 0) return SYS_ENOENT;

		return SYS_EIO;
	}

	if(wrote == 0) return SYS_ENOSPC;

	return wrote;
}

/* unlink(path) -- removes a file. Returns 0 or a negative code.
*
*  The call the section header above is really about: the one that destroys
*  rather than changes. Everything this kernel can honestly say about which
*  files ring 3 may do that to is said there.
*
*  fat_delete() refuses a directory, and this call does not try to work around
*  that. There is no fat_rmdir() to pair with, and emptying a directory from
*  inside a system call is a recursive walk over a filesystem being modified
*  underneath it -- with no handle, no depth bound and a 4 KiB kernel stack.
*  A directory is an argument that makes no sense for a call that removes
*  files, and is answered as one.
*
*  The split asks afterwards what the path actually names. A directory is
*  SYS_EINVAL, which is the case this call is most likely to be handed by
*  mistake. Nothing at all is SYS_ENOENT, and that is a distinction a caller
*  acts on -- removing a name that was already gone is a different situation
*  from a removal that could not be performed. A file that is still there was
*  not removed by a drive that refused the write, which is SYS_EIO. */
static int sys_unlink(struct regs *r)
{
	char path[SYS_PATH_MAX];
	uint32_t unused;
	int err;

	err = sysfs_path((uint32_t)r->ebx, path);
	if(err != 0) return err;

	err = sysfs_rofs();
	if(err != 0) return err;

	if(fat_delete(path) != 0)
	{
		if(sysfs_is_dir(path)) return SYS_EINVAL;
		if(fat_size(path, &unused) != 0) return SYS_ENOENT;

		return SYS_EIO;
	}

	return 0;
}

/* truncate(path, size) -- sets the length of a file. Returns 0 or negative.
*
*  Two operations wearing one name, and the shrinking one is the ordinary case:
*  truncate(path, 0) is how a program overwrites a file it did not create, and
*  it is what SYS_FCREATE's SYS_EEXIST points a caller at.
*
*  The growing one is where the argument needs the same suspicion SYS_FWRITE's
*  length gets. Growing zero-fills, and a zero fill is written to the disk, so
*  truncate(path, 0xFFFFFFFF) is a request to write four gigabytes of zeroes by
*  PIO from inside a single trap. Unlike a length, a size cannot simply be
*  capped: a truncate that quietly produced a different size than it was asked
*  for would be lying to its caller about the one thing it does.
*
*  What can be done is to refuse the sizes that could never be reached. A file
*  cannot be larger than the volume holding it, so a size above
*  fat_total_bytes() is answered SYS_ENOSPC before anything is written, instead
*  of after every free cluster on the disk has been spent on zeroes to learn
*  the same thing. That is not a complete bound -- a size that does fit still
*  zero-fills up to the size of the volume -- and the rest of it belongs in
*  fat.c, which is the only code that knows how far it has got. It is a bound
*  on what a typo can cost, which is what an argument check is for.
*
*  The failure split uses the size the file already had, which the existence
*  probe hands over for free. A grow that failed ran out of room, and
*  SYS_ENOSPC is a code a program can act on by deleting something; a shrink
*  that failed did not run out of room, so it was the drive refusing, which is
*  SYS_EIO. That is inference over fat.c's single -1 rather than something
*  fat.h reports, and it is drawn this way because the inference is right in
*  the case that actually happens. The directory and the missing path are asked
*  about first, in the order sysfs_is_dir() explains. */
static int sys_truncate(struct regs *r)
{
	char path[SYS_PATH_MAX];
	uint32_t size;
	uint32_t total;
	uint32_t have;
	int err;

	size = (uint32_t)r->ecx;

	err = sysfs_path((uint32_t)r->ebx, path);
	if(err != 0) return err;

	err = sysfs_rofs();
	if(err != 0) return err;

	total = fat_total_bytes();
	if(total != 0 && size > total) return SYS_ENOSPC;

	if(fat_truncate(path, size) != 0)
	{
		if(sysfs_is_dir(path)) return SYS_EINVAL;

		have = 0;
		if(fat_size(path, &have) != 0) return SYS_ENOENT;
		if(size > have) return SYS_ENOSPC;

		return SYS_EIO;
	}

	return 0;
}


/* ------------------------------------------------------------------ */
/* The screen, and what the user does to it                            */
/* ------------------------------------------------------------------ */

/* mapfb(out) -- puts the framebuffer into the caller's address space and hands
*  it the screen. Returns 0 and fills in *out, or a negative code.
*
*  THIS IS THE FIRST CALL THAT MAPS HARDWARE INTO A RING 3 ADDRESS SPACE, and
*  every other call in this file only ever handed ring 3 bytes it had copied.
*  What crosses here is a window onto the card: writes to it change what is on
*  the monitor, immediately, anywhere on it. That is the point of the call --
*  a graphical program cannot exist without it -- and the whole of the work
*  below is making sure that is ALL it is. Four things guard that:
*
*    - WHERE. SYS_FB_VIRT, and its comment is the argument that a 3 MiB
*      mapping there cannot land on the program image, on any task's stack or
*      on the argument block. Nothing is mapped over, because nothing of the
*      caller's is ever there to map over.
*    - WHAT. The physical range must lie above every frame the pmm has ever
*      heard of. A framebuffer is decoded outside RAM -- QEMU's stdvga puts it
*      at 0xFD000000 -- so this costs nothing in the ordinary case, and in the
*      case it is written for it is everything: an address that OVERLAPS RAM,
*      whether the bootloader lied, the mode line is wrong or a chipset carved
*      the buffer out of main memory, would map kernel memory into a user half
*      with PAGE_USER set. The check is a single compare and it converts the
*      worst outcome in this file into SYS_ENODEV.
*    - HOW MUCH. pitch * height, bounded by SYS_FB_MAX_BYTES, computed without
*      ever forming a product that could wrap.
*    - WHO ELSE. The console is on that screen. fbcon_suspend() takes it and
*      is refused if anybody already has it, which makes fbcon.c the second
*      opinion on the ownership this file records.
*
*  THE CACHING FLAGS ARE THE ONES vmm_map_mmio() ARGUES FOR, and the argument
*  is not this file's to re-run: PAGE_NOCACHE with PAGE_WRITETHROUGH, i.e. the
*  strongest uncached type available without PAT or MTRR setup. A framebuffer
*  left write-back is the classic bug where the picture updates late or only
*  when something else evicts the line -- the writes sit in the cache and the
*  card has no way to be told. PCD forbids caching; PWT is set alongside it so
*  the entry names fully uncached UC rather than the weaker UC- of the default
*  PAT layout. Exactly what the kernel's own mapping of the same frames uses,
*  which is the point: two mappings of one device with different cacheability
*  is an architecturally undefined thing to do, and it would be undefined in a
*  direction nobody could debug -- the console and the program disagreeing
*  about pixels neither of them wrote.
*
*  PAGE_USER is what the kernel's mapping does not have, and it is the whole
*  difference. It is added here only after the address, the range and the size
*  have all been settled.
*
*  The bounds are handed to the mouse as well. mouse.h asks for the screen the
*  pointer is drawn on and the driver's default is 640x480, set before anything
*  knew the mode -- so on a 1024x768 screen the pointer would stop a third of
*  the way from the right edge and the user could not get it back. Nothing is
*  restored on release, and that is correct rather than lazy: main.c derives
*  the console's own field from fbcon_width()/fbcon_height() too, so this is
*  the same rectangle either way. The screen does not change when its owner
*  does. */
static int sys_mapfb(struct regs *r)
{
	sys_fbinfo info;
	unsigned long flags;
	addrspace_t space;
	uint32_t out;
	uint32_t phys;
	uint32_t pitch;
	uint32_t height;
	uint32_t bytes;
	uint32_t off;
	uint32_t mapflags;
	int pid;

	out = (uint32_t)r->ebx;

	if(!user_range_ok(out, (uint32_t)sizeof(sys_fbinfo), 1)) return SYS_EFAULT;

	/* A screen held by a task that is already gone is not a busy screen. */
	sysfb_reap();

	/* No task means no address space of the caller's to map into, and no pid
	*  to record the ownership against. That is the kernel calling itself,
	*  which has fbcon_pixels() and does not need this. */
	pid = taskmgr_get_currpid();
	if(pid < 0) return SYS_EINVAL;

	phys   = fbcon_phys();
	pitch  = fbcon_pitch();
	height = fbcon_height();

	/* No framebuffer at all is the ordinary "make run" case rather than a
	*  failure: QEMU's -kernel loader does not implement the Multiboot video
	*  request, so the machine comes up in text mode and there is nothing here
	*  to hand over. Each of the five is asked because each is separately 0 in
	*  that case and a caller that got a structure full of zeroes would be
	*  drawing into an address of 0 with a pitch of 0. */
	if(phys == 0 || pitch == 0 || height == 0) return SYS_ENODEV;
	if(fbcon_width() == 0 || fbcon_bpp() == 0) return SYS_ENODEV;

	/* Size, and the product is only formed once it is known not to wrap: the
	*  division is against a constant that is never 0 and pitch is known
	*  non-zero above, so the test is exact rather than approximate. */
	if(height > SYS_FB_MAX_BYTES / pitch) return SYS_ENODEV;

	bytes = pitch * height;
	bytes = (bytes + (uint32_t)PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
	if(bytes == 0 || bytes > SYS_FB_MAX_BYTES) return SYS_ENODEV;

	/* Not RAM, and does not run off the end of the address space. The first
	*  is the isolation check the call comment describes; the second keeps the
	*  loop below from wrapping phys + off around zero and mapping the low
	*  frames -- which on this kernel are the ones holding it. */
	if(phys < pmm_total_bytes()) return SYS_ENODEV;
	if(phys > 0xFFFFFFFFu - (bytes - 1)) return SYS_ENODEV;

	space = taskmgr_task_space(pid);
	if(space == 0) return SYS_EINVAL;

	/* The claim is one indivisible step, for the reason sysfb_drop() gives:
	*  the timer preempts a system call, so a plain test followed by a store
	*  is two tasks both finding the screen free. */
	flags = sysnet_irq_save();

	if(sysfb_owner >= 0)
	{
		sysnet_irq_restore(flags);
		return SYS_EBUSY;
	}

	sysfb_owner = pid;
	sysfb_space = space;
	sysfb_bytes = bytes;

	sysnet_irq_restore(flags);

	/* fbcon.c gets to refuse as well. It does so when the console is already
	*  suspended by somebody who did not come through this gate -- "gfx" hands
	*  the screen over the same way -- and that is a busy screen even though
	*  no task owns it here. Reported as SYS_EBUSY rather than as an error of
	*  its own because it is the same thing to the caller: come back later. */
	if(fbcon_suspend() != 0)
	{
		flags = sysnet_irq_save();
		sysfb_owner = -1;
		sysfb_space = 0;
		sysfb_bytes = 0;
		sysnet_irq_restore(flags);

		return SYS_EBUSY;
	}

	mapflags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER
	           | PAGE_NOCACHE | PAGE_WRITETHROUGH;

	for(off = 0; off < bytes; off += PAGE_SIZE)
	{
		if(vmm_map_in(space, SYS_FB_VIRT + off, phys + off, mapflags) == 0)
			continue;

		/* The only way this fails is the pmm having no frame left for the one
		*  page table the range needs. Everything done so far comes back --
		*  sysfb_drop() walks the whole range and does not mind the pages that
		*  never got mapped -- and the console is repainted, so a failed
		*  SYS_MAPFB leaves the machine exactly as it found it. */
		sysfb_drop();
		return SYS_ENOMEM;
	}

	mouse_set_bounds((int)fbcon_width(), (int)fbcon_height());

	/* Asked a second time, after the mapping. The invariant that makes one
	*  check enough elsewhere -- only the caller edits the caller's half, and
	*  the caller is parked inside this call -- does not hold here, because
	*  THIS CALL edits it. A pointer that was valid on the way in cannot in
	*  fact have been covered (a page inside SYS_FB_VIRT's range was unmapped
	*  before, so the first check would have refused it), but the write below
	*  is the one place where being wrong about that is a store through a
	*  pointer the page tables no longer agree with, and the re-test is two
	*  compares and a walk. */
	if(!user_range_ok(out, (uint32_t)sizeof(sys_fbinfo), 1))
	{
		sysfb_drop();
		return SYS_EFAULT;
	}

	/* Built here and copied out in one go, for the reason copy_to_user()
	*  gives: the check was about a range as a whole, so the write should be
	*  too, and a struct that is zeroed first cannot carry kernel stack
	*  contents across the gate in its padding. */
	memset(&info, 0, sizeof(info));
	info.addr   = SYS_FB_VIRT;
	info.size   = bytes;
	info.width  = fbcon_width();
	info.height = height;
	info.pitch  = pitch;
	info.bpp    = fbcon_bpp();
	sysfb_format(&info);

	copy_to_user(out, &info, (uint32_t)sizeof(info));

	/* Last, and only on the path that actually gave the screen away: there is
	*  nothing to reclaim until somebody holds something. */
	sysfb_start_janitor();

	return 0;
}

/* unmapfb() -- gives the screen back and repaints the console.
*
*  Only the owner may. A task that never had it gets SYS_EINVAL, and so does a
*  task calling it while a DIFFERENT task holds the screen: the alternative is
*  a call that repaints the console over a picture somebody else is drawing,
*  which is precisely what one-owner-at-a-time exists to prevent, reachable by
*  any program that calls the wrong function.
*
*  Everything it does, sys_exit() does too. That is deliberate -- a program
*  that returns without calling this is the ordinary case, not a leak the
*  kernel has to punish -- and this call exists for the program that wants its
*  output printed into a console that is back. */
static int sys_unmapfb(struct regs *r)
{
	int pid;

	(void)r;

	sysfb_reap();

	pid = taskmgr_get_currpid();
	if(pid < 0) return SYS_EINVAL;
	if(sysfb_owner != pid) return SYS_EINVAL;

	sysfb_drop();
	return 0;
}

/* Takes the next mouse event into an ABI event. 1 if there was one.
*
*  The two structures are close enough to look interchangeable and are not:
*  mouse_event stores coordinates in int16_t and the button masks in uint8_t,
*  sys_input_event uses int32_t and uint32_t. The widening is the conversion,
*  and doing it through a local is what keeps a partially built structure out
*  of user memory. The button masks cross unchanged -- MOUSE_BUTTON_LEFT is 1,
*  RIGHT is 2, MIDDLE is 4, and user/syscall.h publishes the same three
*  numbers, so there is nothing to translate. */
static int sysinput_take_mouse(sys_input_event *ev)
{
	mouse_event m;

	if(mouse_poll(&m) == 0) return 0;

	memset(ev, 0, sizeof(*ev));
	ev->type    = SYS_INPUT_MOUSE;
	ev->time_ms = m.time_ms;
	ev->x       = (int32_t)m.x;
	ev->y       = (int32_t)m.y;
	ev->dx      = (int32_t)m.dx;
	ev->dy      = (int32_t)m.dy;
	ev->buttons = (uint32_t)m.buttons;
	ev->changed = (uint32_t)m.changed;

	return 1;
}

/* The same for the keyboard. 1 if a key was waiting.
*
*  getchn() is the non-blocking read SYS_PEEKCH already uses, and 0 is the
*  driver's own "nothing", which is also what every key that has no character
*  maps to -- a modifier or a function key is indistinguishable from no key at
*  all through this interface, exactly as it is through SYS_GETCH.
*
*  Only type, time and key are filled in. The other fields belong to the mouse
*  by the header's own comments, and a key event that carried a pointer
*  position would be inviting a program to read x and y out of an event that
*  is not about the pointer. The time is taken here rather than in the driver
*  because kb.c does not record one: it is when the key was COLLECTED, which
*  on a 20 ms sample is within a sample of when it was pressed. */
static int sysinput_take_key(sys_input_event *ev)
{
	unsigned char key;

	key = getchn();
	if(key == 0) return 0;

	memset(ev, 0, sizeof(*ev));
	ev->type    = SYS_INPUT_KEY;
	ev->time_ms = (uint32_t)timer_get_ticks();
	ev->key     = (uint32_t)key;

	return 1;
}

/* Which of the two was served last, so the next look starts with the other
*  one. See sysinput_take(). */
static int sysinput_mouse_first = 1;

/* One event out of whichever source has one, alternating which is asked
*  first. 1 if *ev was filled in.
*
*  BOTH SOURCES CAN HAVE SOMETHING AT ONCE and that is the case worth
*  designing for rather than discovering: a user holding a key while moving the
*  mouse produces a repeat every 33 ms and a packet every few, and only one
*  event is returned per call. A fixed order starves the other source for as
*  long as the preferred one keeps delivering -- keyboard first means the
*  pointer stops moving while a key is held, mouse first means a held key is
*  swallowed for as long as the mouse is moving. Neither is acceptable and
*  neither is more acceptable than the other, which is why the choice is not
*  made once but alternated: whichever source was served last is asked second
*  next time, so the worst case for either is one event of delay behind the
*  other. Cheap, and it needs no knowledge of what either device is doing.
*
*  The variable is shared by every task in this call rather than kept per task.
*  Two graphical programs cannot both be reading input usefully anyway -- there
*  is one keyboard slot and one mouse queue between them -- and a global that
*  is only ever a hint about ORDER cannot make either of them miss an event.
*
*  Both takes are destructive, which is what lets this be the condition of the
*  wait below: there is no way to ASK either driver whether something is
*  waiting without consuming it (mouse.h offers mouse_poll() and nothing
*  weaker, kb.c's getchn() takes the key as it reads it), so the test and the
*  take are one action, and the caller runs it with interrupts off. */
static int sysinput_take(sys_input_event *ev)
{
	if(sysinput_mouse_first)
	{
		if(sysinput_take_mouse(ev))
		{
			sysinput_mouse_first = 0;
			return 1;
		}

		return sysinput_take_key(ev);
	}

	if(sysinput_take_key(ev))
	{
		sysinput_mouse_first = 1;
		return 1;
	}

	return sysinput_take_mouse(ev);
}

/* input(out, timeout_ms) -- the next thing the user did. 1 when *out was
*  filled in, 0 when the time ran out, negative on a bad argument.
*
*  ONE QUEUE FOR TWO DEVICES, because a program with a pointer and a keyboard
*  has to wait for whichever comes first and this kernel cannot wait on two
*  things at once: task_wait() takes one channel. So one of the two has to be
*  the one that wakes the task and the other has to be sampled, and the choice
*  is made by what the drivers publish. mouse.h has mouse_wait_channel(),
*  woken from the interrupt for every packet. kb.c blocks on the address of
*  last_key, which is in no header -- and sys_getch() above already says what
*  this file does about that: it does not rebuild the wait out of a channel it
*  would have to invent, because a channel that is wrong by one address is a
*  task that never runs again. So the wait is on the mouse and the keyboard is
*  read once per turn, at SYS_INPUT_POLL, which is what that constant is.
*
*  The consequence is worth stating plainly: a mouse packet wakes this call at
*  once, a keystroke is noticed within 20 ms. If the keyboard ever exports its
*  channel the two become symmetrical and this loop does not otherwise change.
*
*  THE IDIOM IS system.h's, and the condition it tests is sysinput_take()
*  itself. Interrupts are off across the test AND the block, so a packet or a
*  key that arrives between the two cannot be lost; the condition is re-tested
*  after every wake rather than trusted, so a wake for the other waiter -- the
*  console is in getch() on the same keyboard, and a second task can be in here
*  -- costs one turn of the loop instead of an event. The take being
*  destructive is what makes the test meaningful: what it reports is not "there
*  is something" but "I have it", which cannot then be stolen between the test
*  and the return.
*
*  It is deliberately not tied to SYS_MAPFB. A program that has not taken the
*  screen may still want the pointer, and one that has may want neither.
*
*  What it shares with SYS_GETCH is the one key slot in kb.c: the console's own
*  getch() takes from it too, so whoever looks first wins. In practice the
*  shell is in run_wait() while a program it started runs -- main.c says why it
*  deliberately does not poll the keyboard there -- so a program has the
*  keyboard to itself until the shell gives up waiting for it.
*
*  timeout_ms of 0 waits forever, which is task_wait()'s own convention and
*  what user/syscall.h documents; a negative one is not a duration and is
*  refused. Even a forever wait is chunked, so a task blocked here is blocked
*  in the scheduler's sense -- "ps" shows Blocked, not a busy loop -- and comes
*  back once every SYS_INPUT_POLL to look at the keyboard. */
static int sys_input(struct regs *r)
{
	sys_input_event ev;
	unsigned long flags;
	uint32_t out;
	uint32_t start;
	uint32_t waited;
	int timeout;
	int chunk;
	int got;

	out     = (uint32_t)r->ebx;
	timeout = (int)r->ecx;

	if(!user_range_ok(out, (uint32_t)sizeof(sys_input_event), 1))
		return SYS_EFAULT;
	if(timeout < 0) return SYS_EINVAL;

	start = (uint32_t)timer_get_ticks();

	for(;;)
	{
		/* How much of the caller's budget is left, and therefore how long
		*  this turn may block. Worked out before the take so that a call with
		*  a timeout that has already expired still gets one look at both
		*  devices -- input(out, 0-length) is a poll, and a poll that returns
		*  without looking would be useless. */
		chunk = SYS_INPUT_POLL;

		if(timeout != 0)
		{
			waited = sysnet_ms_since(start);
			if(waited >= (uint32_t)timeout) chunk = 0;
			else if((uint32_t)timeout - waited < (uint32_t)SYS_INPUT_POLL)
				chunk = (int)((uint32_t)timeout - waited);
		}

		flags = sysnet_irq_save();

		got = sysinput_take(&ev);

		/* Only when there is something to wait for and somebody to wait. A
		*  caller with no current task is answered "timed out" by task_wait()
		*  at once, which would turn this into a spin; it is not reachable from
		*  ring 3 and costs one comparison to keep out. chunk of 0 means the
		*  budget is gone, and task_wait() would read that as "wait forever". */
		if(!got && chunk > 0 && taskmgr_get_currpid() >= 0)
			task_wait(mouse_wait_channel(), chunk);

		sysnet_irq_restore(flags);

		if(got)
		{
			copy_to_user(out, &ev, (uint32_t)sizeof(ev));
			return 1;
		}

		if(chunk == 0) return 0;

		/* No task: the wait above did not happen, so this is the fallback
		*  sysnet_pump() uses for the same case -- sleep rather than spin. */
		if(taskmgr_get_currpid() < 0) sleep(chunk);
	}
}


/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* Indexed by call number. A table instead of a switch, so that an unknown
*  number is a bounds check and a null test rather than a forgotten case --
*  any gap up to SYSCALL_MAX is zero and answers with SYS_ENOSYS. There is no
*  gap at the moment: the table is full to SYSCALL_MAX, so a call number ring 3
*  invents is rejected by the bounds check alone, and the null test is what
*  keeps the next number added to syscall.h safe before it has a handler. */
static syscall_fn syscall_table[SYSCALL_MAX] =
{
	sys_exit,      /* SYS_EXIT      0 */
	sys_write,     /* SYS_WRITE     1 */
	sys_getpid,    /* SYS_GETPID    2 */
	sys_sleep,     /* SYS_SLEEP     3 */
	sys_putch,     /* SYS_PUTCH     4 */
	sys_uptime,    /* SYS_UPTIME    5 */
	sys_getch,     /* SYS_GETCH     6 */
	sys_peekch,    /* SYS_PEEKCH    7 */
	sys_cls,       /* SYS_CLS       8 */
	sys_setcolor,  /* SYS_SETCOLOR  9 */
	sys_statfs,    /* SYS_FSINFO   10 */
	sys_stat,      /* SYS_STAT     11 */
	sys_read,      /* SYS_READ     12 */
	sys_readdir,   /* SYS_READDIR  13 */
	sys_spawn,     /* SYS_SPAWN    14 */
	sys_resolve,   /* SYS_RESOLVE  15 */
	sys_connect,   /* SYS_CONNECT  16 */
	sys_send,      /* SYS_SEND     17 */
	sys_recv,      /* SYS_RECV     18 */
	sys_close,     /* SYS_CLOSE    19 */
	sys_fcreate,   /* SYS_FCREATE  20 */
	sys_fwrite,    /* SYS_FWRITE   21 */
	sys_unlink,    /* SYS_UNLINK   22 */
	sys_truncate,  /* SYS_TRUNCATE 23 */
	sys_mapfb,     /* SYS_MAPFB    24 */
	sys_unmapfb,   /* SYS_UNMAPFB  25 */
	sys_input      /* SYS_INPUT    26 */
};

void syscall_handler(struct regs *r)
{
	unsigned int num;
	int result;

	num = r->eax;
	syscall_calls++;

	if(num >= (unsigned int)SYSCALL_MAX || syscall_table[num] == 0)
	{
		r->eax = (unsigned int)SYS_ENOSYS;
		return;
	}

	result = syscall_table[num](r);

	/* SYS_EXIT has replaced *r with another task's context by now (or has
	*  already written its own error into eax). Writing the result here would
	*  clobber that task's eax, so the one call that does not return to its
	*  caller is also the one whose result is not delivered. */
	if(num == SYS_EXIT) return;

	r->eax = (unsigned int)result;
}

uint32_t syscall_count(void)
{
	return syscall_calls;
}

/* Installs the gate for vector 0x80. The flags byte is 0xEF where every other
*  vector uses 0x8E, and the two differences are worth reading separately.
*
*  Bits 6-5 are the DPL, and 0xEF sets them to 3. Without that, "int 0x80"
*  from ring 3 raises a general protection fault instead of entering the
*  kernel, because the CPU compares the caller's CPL against the gate's DPL.
*
*  The low nibble makes it a *trap* gate (type 0xF), not an interrupt gate
*  (type 0xE) as every other vector installs. An interrupt gate
*  clears IF on entry, which would break SYS_SLEEP outright: timer_wait()
*  waits for timer_ticks to advance, but the timer IRQ that advances it can
*  never be delivered while interrupts are masked, and ring 3 cannot sti
*  itself. A trap gate leaves IF as the caller had it, so a system call is
*  preemptible.
*
*  The price is a time-of-check/time-of-use window: the pointer validation
*  and the reads that follow it are separate passes, and the timer IRQ can
*  land between them. Two different things could change in that gap, and they
*  are worth keeping apart.
*
*  The mapping itself does not. Everything user_byte_ok() lets through lies
*  below KERNEL_VIRTUAL_BASE -- its range check says so outright, without
*  having to know what anyone mapped -- i.e. in the private three quarters of
*  the caller's own directory, and the only ring 3 code that edits that half
*  is the caller, which is parked inside this system call and not running. A
*  task that preempts us edits its own space. While all tasks shared one
*  directory this was a genuine race; per-task spaces closed it, and that,
*  not the pointer checks by themselves, is where the isolation comes from.
*
*  The address space the *re-checks* are answered in could. user_string_len()
*  asks again at every 4 KiB boundary, and such a question must not be put to
*  a stranger's directory. It is not: schedule() elects a task and loads its
*  CR3 in one step, so the active space always belongs to the current task,
*  and a call resumed after a preemption walks the same directory it walked
*  before. This file depends on that invariant and does not maintain it --
*  tasks.c does.
*
*  Two gaps remain, and neither is closed by paging:
*    - the read is a ring 0 read, and supervisor accesses ignore the U/S bit.
*      PAGE_USER is therefore checked by software, not enforced by the CPU at
*      the moment of the read. Only the kernel could clear that bit mid-call,
*      so today nothing exploits it, but a future vmm_unmap() from another
*      task's context would not be stopped by hardware here.
*    - a task aborted from outside while parked mid-call is simply never
*      re-elected, so its half-finished frame is abandoned rather than
*      resumed against a directory that may since have been torn down. That
*      is a property of the scheduler's policy, not a guarantee this file
*      obtains, and it would have to be re-examined if aborted tasks ever
*      became resumable.
*
*  This is the only descriptor ring 3 is allowed to reach. Every exception
*  and IRQ vector keeps DPL 0, so user code cannot software-trigger a page
*  fault or a timer interrupt and feed the kernel a made-up error code. */
void syscall_install(void)
{
	idt_set_gate(SYSCALL_VECTOR, (unsigned long)syscall_stub, GDT_KERNEL_CODE, 0xEF);
}
