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
#include <syscall.h>
#include <fat.h>
#include <ata.h>

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

	/* The task is marked aborted and loses its remaining time slice, so the
	*  search below skips it. */
	taskmgr_task_abort(pid, status, "exit()");

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
*  What the loop actually does while it waits is hlt, and hlt is the point.
*  The CPU stops until the next interrupt instead of spinning, and the next
*  interrupt is either IRQ1 with the key or IRQ0 with a tick. IRQ0 is the
*  interesting one: irq_handler() calls schedule() for it and returns a
*  different register frame, so the timer that fires while this task sits in
*  hlt is the same timer that hands the CPU to another task. The waiting task
*  is descheduled by preemption, exactly as SYS_SLEEP is -- timer_wait() waits
*  in the same shape, and this call earns its keep from the same invariant.
*
*  What this is not is a proper blocked state. The task stays runnable, so the
*  scheduler keeps re-electing it, and it keeps going straight back into hlt
*  until a key arrives. That costs one wakeup per time slice, not a spin, and
*  it does not delay anybody else. A wait queue that took the task off the run
*  list belongs in tasks.c, and would be the right next step.
*
*  The IF test mirrors timer_wait()'s. Ring 3 always arrives with IF set --
*  tasks are created with eflags 0x202 and cli is privileged -- so the hlt path
*  is the only one a user program can reach. The fallback exists for a ring 0
*  caller that got here with interrupts off, where neither branch can make
*  progress but at least the busy loop does not park the CPU in a state only an
*  interrupt could leave. */
static int sys_getch(struct regs *r)
{
	unsigned char key;
	unsigned long flags;
	int irqs_enabled;

	(void)r;

	__asm__ __volatile__ ("pushfl; popl %0" : "=r" (flags) : : "memory");
	irqs_enabled = (flags & 0x200) != 0;

	for(;;)
	{
		key = getchn();
		if(key != 0) return (int)key;

		if(irqs_enabled) __asm__ __volatile__ ("hlt");
	}
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

	/* exec_spawn_path() hands the task back SUSPENDED on purpose, so that the
	*  shell can look at it -- or take it back down -- before it has executed
	*  anything. A system call has nothing to look at and nobody to ask, and
	*  the call is named spawn(): a caller that got a pid but no running task
	*  would have no way to start one, because there is no SYS_START. So this
	*  is the last step of the call rather than the caller's job. */
	taskmgr_task_start(pid);

	return pid;
}


/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* Indexed by call number. A table instead of a switch, so that an unknown
*  number is a bounds check and a null test rather than a forgotten case --
*  the gaps up to SYSCALL_MAX are zero and answer with SYS_ENOSYS. */
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
	0              /* 15 -- unassigned, answers SYS_ENOSYS */
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
