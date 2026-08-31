#include <system.h>
#include <stdio.h>
#include <mm.h>
#include <vmm.h>
#include "string.h"

#define MAX_TASKS 64
#define NUM_OF_PRIORITIES 4

#define KERNEL_STACK_SIZE 4096

/* Where the user stacks live.
*
*  After vmm_init() nothing below KERNEL_VIRTUAL_BASE is mapped any more (the
*  boot directory's identity window of the low 4 MiB is deliberately not
*  reproduced), so the whole lower 3 GiB is free real estate. The stacks are
*  parked at the very top of it, directly under the kernel: that keeps the low
*  addresses - where a user program image would eventually be loaded - clear,
*  and it is the classic layout of code low / stack high.
*
*  Every ring 3 task now runs in an address space of its own, so two tasks
*  could just as well put their stack at the very same virtual address without
*  ever seeing each other. The per slot layout below is kept anyway: it costs
*  nothing, it keeps a stack address unique across the system - which makes a
*  page fault report readable, since the faulting address alone names the slot
*  - and it survives a task deciding to share memory with another one later.
*
*  Every task owns an 8 KiB slot of which only the upper 4 KiB are mapped. The
*  unmapped lower half is a guard page: a task that runs its stack over the
*  edge hits a page fault instead of silently scribbling into the stack of the
*  task below it. The topmost page (0xBFFFF000) is left out for the same reason
*  at the other end.
*
*      slot i:  [ TOP - (i+1)*SLOT , TOP - i*SLOT )
*                 \___ guard ____/ \____ stack ___/
*
*  With MAX_TASKS = 64 the whole region spans 512 KiB, from 0xBFF80000 up to
*  0xBFFFF000 - far away from anything else the kernel maps. */
#define USER_STACK_REGION_TOP  0xBFFFF000u
#define USER_STACK_SLOT_SIZE   0x2000u          /* 4 KiB stack + 4 KiB guard */

static int current_task = -1;

/* THE BOOT PATH IS NOT A TASK, and this is what keeps that from mattering.
*
*  kernel() runs on the boot stack, not on any slot's. schedule() files the
*  registers it interrupted into task_states[current_task], and current_task
*  is -1 until it has elected somebody -- so before the first switch there is
*  nowhere to put them and they are not saved at all. The consequence is
*  sharp: the first timer tick after ANY task becomes runnable elects that
*  task, and the boot path is simply gone. Everything kernel() had left to do,
*  the console task included, never happens.
*
*  This cost a boot that stopped dead one line after the USB devices were
*  listed. Three modules met it independently, and each answered it locally --
*  net.c deferred its drain to the first packet, usbhid.c installed a timer
*  handler that polled taskmgr_get_currpid() until it stopped saying -1. Both
*  worked. Neither was discoverable: the rule lived in two comments, and the
*  fourth module to start a task from kernel() would have found it the same
*  way the first three did.
*
*  So the rule is enforced in the one place that can enforce it. Until
*  taskmgr_boot_complete() is called, taskmgr_task_start() records the intent
*  instead of acting on it, and nothing the boot path creates can be elected
*  out from under it. The scheduler is untouched: it cannot elect what is not
*  RUNNING, and now nothing is RUNNING until the boot path says so.
*
*  What this does NOT do is let a task run DURING boot. A boot path that
*  started a task and then waited for it to do something would now hang where
*  it used to lose the boot. Nothing does that, both are bugs, and a hang at
*  least stops somewhere a debugger can look. The real cure for that case is
*  to give the boot strand a slot of its own so it becomes an ordinary task --
*  a much larger change to the most dangerous code here, for a case nothing
*  has needed yet. */
static int boot_handed_over = 0;

/* Slots whose taskmgr_task_start() arrived before the handover. Not a queue:
*  a slot can only be pending once, and the order they are released in is the
*  scheduler's business rather than this file's. */
static uint8_t task_start_pending[MAX_TASKS];

task_settings tasks[MAX_TASKS];

/* The kernel stack of every task, or 0 for a slot that has none. A ring 0
*  task simply runs on it. A ring 3 task never touches it directly: it is the
*  stack the CPU switches to, using esp0 out of the TSS, whenever an interrupt
*  or a system call drags the task into the kernel. Either way this is where
*  the task's saved struct regs lives, which is why every task needs one of
*  its own - two tasks sharing a kernel stack would overwrite each other's
*  saved context.
*
*  This used to be a static uint8_t[MAX_TASKS][KERNEL_STACK_SIZE], i.e. 256
*  KiB of bss that existed from the first instruction of the kernel onwards,
*  on a machine that realistically runs a handful of tasks. Now a stack is
*  allocated when a task is created and given back when its slot is recycled,
*  and an unused slot costs the four bytes of this pointer.
*
*  The pointer names the stack page itself, i.e. its LOWEST address; the stack
*  grows down from kernel_stack_top(). Zero initialised statics give the right
*  answer before the first task exists: no slot has a stack, and none is ever
*  looked at, because a slot only becomes schedulable in
*  taskmgr_task_start(), long after its stack is in place. */
static uint8_t *task_kstacks[MAX_TASKS];

/* A kernel stack whose task is gone but that could not be handed back at the
*  moment its slot was recycled, see kernel_stack_release(). Freed by
*  reap_pending_kstack() on the next task creation, exactly like
*  pending_space next to it. */
static uint8_t *pending_kstack;

/* The address space a slot runs in, or 0 for "the kernel space".
*
*  A ring 3 task gets a private space from vmm_create_space(): kernel half
*  shared, user half its own. Its user stack frame and the page table holding
*  it belong to that space and are released with it - there is deliberately no
*  second record of the frame here, so it cannot be freed twice.
*
*  A ring 0 task has no user half worth isolating and keeps running in the
*  kernel space, which is what the 0 stands for. 0 is never a valid space
*  either: a directory is a physical frame and the pmm never hands out frame
*  zero. Zero initialised statics therefore give the right default before the
*  first task exists.
*
*  Non-zero doubles as the "this slot belongs to a ring 3 task" marker. */
static addrspace_t task_space[MAX_TASKS];

/* A space whose task is gone but that was still loaded in CR3 when its slot
*  was recycled, see task_space_release(). It is destroyed as soon as somebody
*  asks for a new task while running somewhere else. */
static addrspace_t pending_space;

static struct regs* task_states[MAX_TASKS];

//Indexed by TASK_STATE_*. TASK_STATE_NULL is -1 and deliberately has no entry:
//every unused slot is filtered out first, because an unused slot is not a task
//with a state but the absence of one.
//
//The array is indexed by a value that comes out of tasks[].state, so it has to
//grow with every state that is added - a missing entry is not a missing word,
//it is a read past the end of the array and a pointer made of whatever follows
//it. TASK_STATE_BLOCKED was the fifth.
char *readable_task_state[] =
{
    "Running",
    "Suspended",
	"Aborted",
	"Exited",
	"Blocked"
};

/* --- Waiting and waking --------------------------------------------------
*
*  The three arrays below are the whole of the wait state, one entry per slot,
*  and they are only ever meaningful for a slot in TASK_STATE_BLOCKED. There is
*  deliberately no queue: with 64 slots a wake is a walk over the table, which
*  is a few dozen comparisons inside an interrupt handler and needs no list to
*  be kept consistent from two contexts at once. A linked queue would be faster
*  at a thousand tasks and is a source of dangling pointers at sixty-four.
*
*    wait_channel   what the task is waiting for. Any address; never read
*                   through, only compared. 0 means "nothing" - a wait that
*                   only its deadline can end, which is what sleep() does.
*    wait_deadline  when the wait gives up, in milliseconds of uptime, or 0
*                   for "never". Absolute rather than a countdown so that
*                   nothing has to be decremented per tick per slot.
*    wait_result    what task_wait() will return: 1 woken, 0 timed out. Set to
*                   1 when the wait is armed, so that every way out of
*                   TASK_STATE_BLOCKED that is not a timeout - a wake, but also
*                   an outside taskmgr_task_start() - reports "woken" and makes
*                   the caller re-test its condition. Guessing "timed out"
*                   would make a caller give up on something that is about to
*                   happen; guessing "woken" only ever costs one more turn
*                   around the caller's loop. */
static const void  *wait_channel[MAX_TASKS];
static unsigned int wait_deadline[MAX_TASKS];
static int          wait_result[MAX_TASKS];

/* The earliest armed deadline, or 0 when none is armed. This is what keeps the
*  timeout check off the per-tick bill: schedule() tests one integer, and only
*  when that test says a deadline is actually due does anything walk the table.
*
*  It is a hint and is only ever wrong in the safe direction - it may name a
*  moment EARLIER than the true earliest deadline, never a later one. A waiter
*  that is woken normally, or killed, leaves its value behind here; the cost is
*  one sweep of the table at that moment, and the sweep recomputes this exactly.
*  A value that could be too late would be the other thing entirely: a timeout
*  that never fires and a task that waits forever. */
static unsigned int wait_deadline_next;

/* Uptime in milliseconds wraps - timer_get_ticks() returns a signed int fed by
*  a tick counter that overflows after about 25 days at 1000 Hz - and a plain
*  "now >= then" flips its answer at the wrap, which would either fire every
*  armed timeout at once or let none of them fire again. Comparing the
*  DIFFERENCE against half the range instead is stable across the wrap, and is
*  correct as long as no deadline is more than ~12 days out. sleep() caps a
*  wait at ten minutes and every other caller is waiting on hardware, so that
*  bound is not one anybody can reach.
*
*  Same reasoning as the unsigned subtraction in timer_wait(), which was
*  written after the signed version of that loop hung on overflow. */
static int time_reached(unsigned int now, unsigned int then)
{
	return (unsigned int)(now - then) < 0x80000000u;
}

/* Takes a slot out of TASK_STATE_BLOCKED. The caller sets wait_result[] first.
*
*  This is everything a wake does, and it is deliberately three stores: no
*  allocation, no output, no scheduling decision, nothing that could take an
*  unbounded amount of time. task_wake() runs in interrupt context - the
*  keyboard, the card, the disk all wake from their handlers - and an interrupt
*  handler that switched tasks would return onto a stack belonging to somebody
*  else.
*
*  The slice is deliberately NOT touched here. A task that blocked still holds
*  the remainder of the slice it was running on, and leaving it alone is what
*  makes the fast path free: a wake that arrives before the next tick finds the
*  task RUNNING again with time left, so schedule() hands it straight back and
*  the wait costs nothing at all. A task that was blocked long enough to be
*  descheduled had its slice refilled by schedule() on the way out. */
static void wait_end(int slot)
{
	wait_channel[slot]  = 0;
	wait_deadline[slot] = 0;
	tasks[slot].state   = TASK_STATE_RUNNING;
}

/* Clears whatever the previous occupant of a slot was waiting for. Nothing
*  reads these entries for a slot that is not BLOCKED, so this is hygiene
*  rather than correctness - but a stale channel in a slot that is about to be
*  handed to a new task is exactly the kind of leftover that turns into a wake
*  arriving at the wrong program. */
static void wait_forget(int slot)
{
	wait_channel[slot]  = 0;
	wait_deadline[slot] = 0;
	wait_result[slot]   = 0;
}

/* Wakes every blocked task whose deadline has passed, and recomputes the hint.
*  Returns non-zero if it woke a task other than the current one, i.e. if the
*  election below has a new reason to happen now rather than at the end of the
*  running task's slice.
*
*  Called from schedule() and nowhere else. Putting it there rather than in
*  timer_handler() is a deliberate choice and the cheaper of the two: both run
*  on every tick, but schedule() is already the place that decides who runs and
*  is already allowed to change task states, whereas a check in the timer
*  handler would have to reach into tasks.c through an interface system.h does
*  not declare - and it would still be a tick early or a tick late relative to
*  the election, because timer_notify_handlers() runs before irq_handler() gets
*  to schedule(). Here an expired task is runnable in the very tick its
*  deadline passes.
*
*  What it costs: one comparison per tick while nothing has a timeout armed,
*  which is the ordinary state of the machine. While something does, one call
*  to timer_get_ticks() - three divisions - and one comparison per tick, and a
*  walk over all 64 slots only on the tick a deadline actually comes due. The
*  alternative that was rejected is the obvious one, sweeping 64 slots on every
*  tick: correct, but 64000 comparisons a second to notice an event that
*  happens a handful of times a second. */
static int wait_check_timeouts(void)
{
	unsigned int now;
	unsigned int earliest;
	int          preempt;
	int          i;

	now = (unsigned int) timer_get_ticks();

	if(!time_reached(now, wait_deadline_next)) return 0;

	earliest = 0;
	preempt  = 0;

	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state != TASK_STATE_BLOCKED) continue;
		if(wait_deadline[i] == 0) continue;

		if(time_reached(now, wait_deadline[i]))
		{
			wait_result[i] = 0;
			wait_end(i);

			if(i != current_task) preempt = 1;
			continue;
		}

		//Still in the future, so it decides the next check. !time_reached(a,b)
		//is "a is before b", wrap-safe in the same way.
		if(earliest == 0 || !time_reached(wait_deadline[i], earliest))
		{
			earliest = wait_deadline[i];
		}
	}

	wait_deadline_next = earliest;

	return preempt;
}

//Safe copy into a fixed-size buffer: copies at most size-1 characters and always
//terminates with '\0'. Replaces strcpy() in those places where the source comes
//from a caller buffer of arbitrary length.
static void copy_bounded(char *dest, const char *src, int size)
{
	int i;

	if(dest == 0 || size <= 0) return;

	if(src == 0)
	{
		dest[0] = '\0';
		return;
	}

	for(i=0; i < size-1 && src[i] != '\0'; i++)
	{
		dest[i] = src[i];
	}
	dest[i] = '\0';
}


/* Top of a task's kernel stack, i.e. the address the stack grows down from.
*  This is what goes into the TSS: the CPU writes the ring 3 ss/esp/eflags/cs/
*  eip *below* esp0, so esp0 has to be the first address past the end of the
*  stack, not its base. Handing over the base instead would make the very
*  first interrupt push five words into whatever happens to lie in front of
*  the stack. */
static uint32_t kernel_stack_top(int slot)
{
	return (uint32_t) task_kstacks[slot] + KERNEL_STACK_SIZE;
}

/* --- Kernel stacks -------------------------------------------------------
*
*  Where the memory comes from, and why it is the frame allocator rather than
*  malloc().
*
*  A kernel stack has two hard requirements that a plain heap block does not
*  meet on its own:
*
*    - It has to be page aligned. The saved struct regs is carved out of the
*      top of the stack and read back as 32 bit words, and the page below it
*      is meant to be the guard page described further down. malloc() promises
*      neither; asking it for a page aligned page means asking for two and
*      throwing half away, plus the chunk header of the neighbouring block
*      sits directly under the stack - the one thing an overflow must not be
*      able to reach quietly.
*
*    - It has to be mapped in EVERY address space, not just in the one that
*      happened to be active when it was allocated. An interrupt can arrive
*      while any task runs, and the very first thing the CPU does with it is
*      push onto the kernel stack of that task - through whatever page
*      directory is in CR3 at that moment. A stack that exists in only one
*      directory would work perfectly until the first context switch and then
*      triple fault the machine, which is precisely the kind of failure that
*      never shows up in the code that caused it.
*
*  One frame out of pmm_alloc_frames(), addressed through P2V(), satisfies
*  both. Alignment comes for free - a frame is 4 KiB and frame aligned, and
*  KERNEL_STACK_SIZE is exactly one frame. Visibility comes from the direct
*  mapping: vmm_init() maps all usable RAM at KERNEL_VIRTUAL_BASE + phys
*  before any task space can exist, and vmm_create_space() copies the kernel
*  half of the directory - the ENTRIES, so every space walks into the same
*  page tables (see vmm.h). A P2V() address is therefore mapped in every space
*  that exists and in every space that will ever be created, and no allocation
*  here can ever need a new kernel page table, which is the one thing that
*  would NOT propagate. The heap would have been acceptable on that second
*  point for the same reason - heap_init() and every later growth take frames
*  out of that same window and never call vmm_map() - but it fails the first.
*
*  The pmm can hand out frames the direct mapping does not reach (physical
*  memory above DIRECT_MAP_TOP on a large machine); P2V() is meaningless for
*  those, so both frames are checked with vmm_is_mapped() before use rather
*  than assumed.
*
*  The guard page. Each stack is the UPPER of two contiguous frames, and the
*  lower one is unmapped from the direct mapping for as long as the stack
*  lives. A kernel stack overflow then hits a not-present page instead of
*  quietly writing into whatever the frame allocator handed out next - a page
*  table, a page directory, a heap block, another task's saved registers.
*  This was impossible while all the stacks sat in one bss array: the page
*  below a stack was the next task's stack, and unmapping it would have taken
*  that task's memory away.
*
*  It is worth being honest about what the fault looks like. On x86-32 a fault
*  taken in ring 0 does not switch stacks, so the CPU tries to push the
*  exception frame at the esp that just ran off the end - inside the guard
*  page - which faults again: double fault, and the double fault handler is
*  pushed onto the same dead stack, so in practice the machine triple faults
*  and resets. That is still the better outcome by a wide margin: it happens
*  at the exact instruction that overflowed, it is perfectly reproducible, and
*  it destroys nothing. Silent corruption of a random kernel frame is the
*  alternative, and it surfaces minutes later somewhere unrelated. Turning the
*  reset into a readable message needs a double fault task gate with a stack
*  of its own, which belongs in the descriptor tables and not here.
*
*  The guard frame is deliberately kept ALLOCATED in the pmm although it is
*  unmapped. It is not free memory - handing it back while its direct mapping
*  is missing would give the next owner a frame it cannot address through
*  P2V(), and the first heap growth or page table would fault. That is also
*  why kernel_stack_destroy() restores the mapping before, and only before,
*  the pair goes back. */

/* One page of stack plus one page of guard, or 0 if that could not be had.
*  Returns the stack page, i.e. the address the stack grows down from the end
*  of; the guard page is the one directly below it. */
static uint8_t *kernel_stack_create(void)
{
	void    *frames;
	uint32_t guard;
	uint32_t stack;

	/* Contiguous, because the guard has to be the page immediately below
	*  the stack - in the direct mapping neighbouring virtual pages are
	*  neighbouring physical frames, so nothing else will do. */
	frames = pmm_alloc_frames(2);
	if(frames == 0) return 0;

	guard = (uint32_t) P2V(frames);
	stack = guard + PAGE_SIZE;

	/* Both halves have to be reachable through the direct mapping before
	*  the pair is of any use: the stack because the kernel runs on it, the
	*  guard because the unmap below has to have something to remove. A pair
	*  outside the window goes straight back. */
	if(!vmm_is_mapped(guard) || !vmm_is_mapped(stack))
	{
		pmm_free_frames(frames, 2);
		return 0;
	}

	/* Punching the hole edits a page table of the kernel half, which every
	*  address space shares, so the guard is gone in all of them at once and
	*  no space can be created later that still has it. The TLB of the
	*  active space is invalidated by vmm_unmap() itself; a parked space
	*  picks the change up from the CR3 load that reactivates it, because
	*  nothing here is mapped PAGE_GLOBAL. */
	if(vmm_unmap(guard) != 0)
	{
		pmm_free_frames(frames, 2);
		return 0;
	}

	return (uint8_t *) stack;
}

/* Gives a stack and its guard page back. The direct mapping is repaired
*  first: a frame whose P2V() alias is missing must never reach the free list,
*  so if the repair fails - which it cannot today, the page table it writes
*  into has existed since vmm_init() - the two frames are leaked on purpose
*  rather than handed out again. */
static void kernel_stack_destroy(uint8_t *stack)
{
	uint32_t guard;

	if(stack == 0) return;

	guard = (uint32_t) stack - PAGE_SIZE;

	if(vmm_map(guard, V2P(guard), PAGE_PRESENT | PAGE_WRITE) != 0) return;

	pmm_free_frames((void *) V2P(guard), 2);
}

/* The esp of the caller, used only to answer "are we standing on this very
*  stack right now". Reading it is enough: the value is compared against a
*  page sized range, and no call this function returns through can move esp
*  out of the frame that asked. */
static uint32_t current_esp(void)
{
	uint32_t esp;

	__asm__ __volatile__ ("movl %%esp, %0" : "=r" (esp));

	return esp;
}

/* Is anything still relying on this stack? Two things can:
*
*    - the CPU, if it is executing on it at this very moment. That is the
*      whole trap: a task that frees its own kernel stack from inside a call
*      that is running on it hands live memory to the allocator, and the next
*      allocation overwrites the return addresses under esp.
*    - a saved context, if some slot's task_states[] entry points into it. A
*      slot whose stack is being replaced has its entry cleared first, so what
*      this finds is an entry that ended up pointing at a FOREIGN stack - the
*      case of a task that aborted itself, saw its own slot recycled, and was
*      only then preempted, so the scheduler wrote its context into a slot
*      that now belongs to somebody else. Dispatching from freed memory is the
*      failure that would follow, and refusing to free is the cheap way out.
*
*  Everything else is genuinely dead: an aborted task that is not the current
*  one will never be elected again - schedule() elects only RUNNING tasks, and
*  the BLOCKED ones it falls back to when nothing is runnable, neither of which
*  an aborted task is - so nothing will ever return onto its stack. A blocked
*  task is not dead at all and never reaches this function: it is released only
*  when its slot is recycled, and find_free_slot() does not recycle a slot that
*  is merely waiting. */
static int kernel_stack_in_use(uint8_t *stack)
{
	uint32_t base;
	uint32_t top;
	uint32_t esp;
	uint32_t saved;
	int      i;

	if(stack == 0) return 0;

	base = (uint32_t) stack;
	top  = base + KERNEL_STACK_SIZE;

	/* >= base, <= top: an esp of exactly top means the stack is empty,
	*  which cannot happen while a call is running on it, and counting it as
	*  "in use" errs towards keeping memory rather than towards freeing it. */
	esp = current_esp();
	if(esp >= base && esp <= top) return 1;

	for(i=0; i <= MAX_TASKS-1; i++)
	{
		saved = (uint32_t) task_states[i];
		if(saved >= base && saved < top) return 1;
	}

	return 0;
}

/* Hands a slot's kernel stack back. Called when a slot is recycled, and that
*  is the only moment a task's memory is ever given up here: taskmgr_task_
*  abort(), the exception handler, exit() and taskmgr_killall() all do nothing
*  but set a state, and everything a dead task owns deliberately stays around
*  until somebody actually wants the slot. Which means the caller is always a
*  taskmgr_add_*_task(), running in some task's context - never in an
*  interrupt handler, and never on the stack of the slot being recycled unless
*  that task is recycling its own slot, which is the case the parking below
*  exists for.
*
*  The context pointer goes first, because it is by definition the stack's own
*  saved frame and would otherwise make the in-use test below answer "yes" for
*  every single stack. The caller replaces it immediately anyway; in the one
*  path where it does not - an allocation that fails - a zero entry is exactly
*  right, since taskmgr_task_start() and taskmgr_task_set_entry() both refuse
*  a slot without a context.
*
*  A stack that is still live is parked instead of freed, the same answer
*  task_space_release() gives for an address space that is still in CR3 and
*  for the same reason. At most one can be parked: parking requires the caller
*  to be running on the stack it is releasing, and there is only one esp. */
static void kernel_stack_release(int slot)
{
	uint8_t *stack;

	stack = task_kstacks[slot];

	//Both together, and before the early exit, so that "no stack" and "no
	//saved context" can never disagree: a context always lives on the stack
	//of its own slot, so one without the other would be a dangling pointer.
	task_kstacks[slot] = 0;
	task_states[slot]  = 0;

	if(stack == 0) return;

	if(kernel_stack_in_use(stack))
	{
		pending_kstack = stack;
		return;
	}

	kernel_stack_destroy(stack);
}

/* Frees a parked stack once nothing depends on it any more. Called from task
*  context only, never from schedule(): this walks into the pmm and the vmm,
*  and the timer IRQ can hit in the middle of an allocation somewhere else -
*  the same rule reap_pending_space() states next door.
*
*  A stack can stay parked across several attempts, and in the pathological
*  case (a saved context that ended up on a foreign stack) forever. That costs
*  8 KiB and is the deliberate trade: leaking memory is recoverable, freeing a
*  stack somebody still returns onto is not. */
static void reap_pending_kstack(void)
{
	if(pending_kstack == 0) return;
	if(kernel_stack_in_use(pending_kstack)) return;

	kernel_stack_destroy(pending_kstack);
	pending_kstack = 0;
}

/* Highest address of the slot's user stack, i.e. the initial user esp. */
static uint32_t user_stack_top(int slot)
{
	return USER_STACK_REGION_TOP - (uint32_t) slot * USER_STACK_SLOT_SIZE;
}

/* Gives a slot a user stack inside the given address space: one physical
*  frame, mapped user readable and writable at the slot's fixed virtual
*  address. Returns the initial user esp, or 0 if no frame was left or the
*  mapping failed.
*
*  The space is brand new and not the active one, so the mapping goes in with
*  vmm_map_in() and the page cannot be written through its user address - it
*  is not in the directory the MMU is currently reading. The frame is reached
*  through the direct mapping instead, which is why the zeroing below works on
*  P2V(frame) and not on the user address. */
static uint32_t user_stack_create(int slot, addrspace_t space)
{
	void*    frame;
	uint32_t page;

	page = user_stack_top(slot) - PAGE_SIZE;

	frame = pmm_alloc_frame();
	if(frame == 0) return 0;

	if(vmm_map_in(space, page, (uint32_t) frame, PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0)
	{
		pmm_free_frame(frame);
		return 0;
	}

	/* The pmm hands out frames uncleared, so the new task would start out
	*  looking at whatever the previous owner of this frame left behind - and
	*  it could read it from ring 3. Wipe it before the task ever sees it.
	*  Every frame the pmm knows about lies inside the direct mapping, so
	*  P2V() is valid here. */
	memset(P2V((uint32_t) frame), (char) 0, (size_t) PAGE_SIZE);

	return user_stack_top(slot);
}

/* Hands a slot's address space back, with it the user stack frame and the
*  page tables of the user half. Called when a slot is recycled: a ring 0 task
*  moving in has no business with ring 3 memory, and a new ring 3 task gets a
*  space of its own rather than inheriting the dead task's.
*
*  The one case that needs care is a task that killed itself. Nothing frees
*  anything at abort time - the slot and everything in it deliberately stay
*  around until somebody recycles them - and by then the scheduler has long
*  since elected another task and moved CR3 with it, so the space is cold and
*  goes away here and now. But the corner case exists where the aborted task
*  is still the current one: it aborted itself and has not been preempted yet,
*  or it is simply the only task left, so schedule() kept returning its
*  context unchanged. CR3 then still points at exactly this space, and
*  vmm_destroy_space() rightly refuses to pull the directory out from under
*  the CPU that is reading it. Freeing the frames would be no better: the very
*  next ring 3 instruction would touch a stack that has been handed back.
*
*  So such a space is parked in pending_space and destroyed by
*  reap_pending_space() on the next task creation, which by definition runs
*  after the caller returned to whatever it was doing - and only if CR3 has
*  moved on by then. At most one space can ever be parked, because only the
*  single active space can reach this branch and its slot is cleared on the
*  way out. */
static void task_space_release(int slot)
{
	addrspace_t space;

	space = task_space[slot];
	if(space == 0) return;

	task_space[slot] = 0;

	if(space == vmm_current_space())
	{
		pending_space = space;
		return;
	}

	vmm_destroy_space(space);
}

/* Destroys a parked space once the CPU has left it. Called from task context
*  only, never from schedule(): vmm_destroy_space() walks back into the pmm,
*  and the timer IRQ can hit in the middle of a frame allocation somewhere
*  else - the scheduler must not touch the allocator the code it interrupted
*  may be halfway through. */
static void reap_pending_space(void)
{
	if(pending_space == 0) return;
	if(pending_space == vmm_current_space()) return;

	vmm_destroy_space(pending_space);
	pending_space = 0;
}


struct regs* init_task(uint8_t* stack, void* entry)
{
    struct regs new_state = {
        .eax = 0, .ebx = 0, .ecx = 0, .edx = 0,
        .esi = 0, .edi = 0, .ebp = 0,
        //.esp = unused (no ring switch)
        .eip = (uint32_t) entry,
 
        /* ring 0 segment register */
        .cs  = 0x08,

        /* enable IRQs (IF = 1) */
        .eflags = 0x202,
		
		.gs=16, .fs=16,	.es=16,	.ds=16,
		
		.esp=0,	.useresp=0,	.ss=0,
		.int_no=0, .err_code=0,	
    };
	
    struct regs* state = (void*) (stack + KERNEL_STACK_SIZE - sizeof(new_state));
    *state = new_state;

    return state;
}

/* The same thing for a task that is to start out in ring 3.
*
*  Two things make this frame different from the ring 0 one above. First the
*  selectors: cs and ss carry the user descriptors with RPL 3, and that RPL is
*  what actually performs the privilege change - the iret at the end of the
*  interrupt stub sees a target cs whose RPL is numerically greater than the
*  current CPL and therefore treats the return as one to a less privileged
*  level. Second, and following from it, esp and ss are no longer dead weight:
*  such an iret pops them, so useresp has to name the top of the *user* stack.
*  For a ring 0 task those two slots are simply left on the stack unread.
*
*  eflags keeps IF set. Ring 3 cannot execute sti, so a task started with
*  interrupts disabled could never be preempted and would own the CPU.
*
*  The frame itself still sits on the KERNEL stack, not on the user one: it is
*  consumed by an iret that executes in ring 0, and once that iret has popped
*  all 19 words the kernel stack is empty again, with esp back at exactly the
*  esp0 the TSS hands out.
*
*  entry may be 0, meaning "not known yet": the caller is about to load a
*  program into the task's address space and will only then know where it
*  begins. Everything else about the frame is complete, eip alone stays open
*  until taskmgr_task_set_entry() fills it in. Nothing can run in that state -
*  the task is suspended, and taskmgr_task_start() refuses it. */
static struct regs* init_user_task(uint8_t* kstack, void* entry, uint32_t user_esp)
{
    struct regs new_state = {
        .eax = 0, .ebx = 0, .ecx = 0, .edx = 0,
        .esi = 0, .edi = 0, .ebp = 0,
        .eip = (uint32_t) entry,

        /* ring 3 code segment, RPL 3 */
        .cs  = GDT_USER_CODE | GDT_RPL_USER,

        /* enable IRQs (IF = 1) */
        .eflags = 0x202,

        /* ring 3 data segments, RPL 3 */
        .gs = GDT_USER_DATA | GDT_RPL_USER, .fs = GDT_USER_DATA | GDT_RPL_USER,
        .es = GDT_USER_DATA | GDT_RPL_USER, .ds = GDT_USER_DATA | GDT_RPL_USER,

        /* popa discards the esp slot, the iret takes useresp - the value is
        *  duplicated only so a register dump of a fresh task is not
        *  misleading. */
        .esp = user_esp, .useresp = user_esp,
        .ss = GDT_USER_DATA | GDT_RPL_USER,

        .int_no = 0, .err_code = 0,
    };

    struct regs* state = (void*) (kstack + KERNEL_STACK_SIZE - sizeof(new_state));
    *state = new_state;

    return state;
}


/* --- Proving it, rather than not seeing it -------------------------------
*
*  The race above fires roughly once in eighty thousand ticks on an idle
*  machine, so "it did not happen while I watched" is not evidence of anything.
*  Setting SCHED_DEBUG to 1 turns the two invariants this file depends on into
*  something that reports itself the moment it is broken:
*
*    - every nested election that is turned away is traced, with the frame, the
*      stack it is on, the task that owns that stack, and what the previous
*      election decided. Seeing these appear and the machine carry on is the
*      positive result: the condition occurs and is handled.
*    - every frame is checked for being a frame at all just before the stub is
*      told to restore it. That is the check that would have named this bug in
*      one line instead of a general protection fault three tasks later.
*
*  Output goes to QEMU's debug console (port 0xE9), not through printf(): this
*  runs inside the timer interrupt, where the console belongs to whoever was
*  interrupted, and where anything that scrolls loses the record. One "out" per
*  character, no locking, straight into a file on the host.
*
*  To reproduce, both halves matter -- the race needs a tick to be already
*  pending when irq_handler() sends its EOI, so the interrupt path has to be
*  slow relative to the tick:
*
*      set SCHED_DEBUG to 1
*      set TIMER_DEFAULT_HZ in timer.c to 8000
*      qemu-system-i386 ... -d int -D int.log -debugcon file:dbg.log
*      then, in the shell:  hello
*
*  Twenty-one nested elections in one such run, every one of them interrupted
*  at the instruction after the "out" in outportb(). With the guard removed,
*  the same run walks a task's kernel stack downwards one interrupt frame at a
*  time; with it, dbg.log fills up and nothing else happens. */
#define SCHED_DEBUG 0

#if SCHED_DEBUG
#define SCHED_DEBUG_MAX 40

static int sched_debug_lines;

static void dbg_putc(char c)
{
	outportb(0xE9, (unsigned char) c);
}

static void dbg_puts(const char *t)
{
	while(*t) dbg_putc(*t++);
}

static void dbg_hex(uint32_t v)
{
	const char *digits = "0123456789abcdef";
	int i;

	dbg_puts("0x");
	for(i = 28; i >= 0; i -= 4) dbg_putc(digits[(v >> i) & 0xF]);
}

static void dbg_dec(int v)
{
	char buf[12];
	int  i = 0;

	if(v < 0) { dbg_putc('-'); v = -v; }
	if(v == 0) { dbg_putc('0'); return; }
	while(v > 0 && i < 11) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
	while(i > 0) dbg_putc(buf[--i]);
}

/* Does this still describe a task, or is it the memory that used to hold one?
*  Only the selectors are worth testing: they have a tiny legal set, and they
*  are what irq_common_stub pops straight into segment registers -- a wrong
*  value there is precisely the general protection fault this hunts. */
static int selector_ok(unsigned int sel, unsigned int r0, unsigned int r3)
{
	return sel == r0 || sel == r3;
}

static int frame_is_sane(const struct regs *f)
{
	if(f == 0) return 0;
	if(((uint32_t) f & 3) != 0) return 0;

	if(!selector_ok(f->ds, GDT_KERNEL_DATA, GDT_USER_DATA | GDT_RPL_USER)) return 0;
	if(!selector_ok(f->es, GDT_KERNEL_DATA, GDT_USER_DATA | GDT_RPL_USER)) return 0;
	if(!selector_ok(f->fs, GDT_KERNEL_DATA, GDT_USER_DATA | GDT_RPL_USER)) return 0;
	if(!selector_ok(f->gs, GDT_KERNEL_DATA, GDT_USER_DATA | GDT_RPL_USER)) return 0;
	if(!selector_ok(f->cs, GDT_KERNEL_CODE, GDT_USER_CODE | GDT_RPL_USER)) return 0;

	/* Bit 1 of eflags reads as 1 on every x86, bits 3 and 5 as 0. */
	if((f->eflags & 0x02) == 0) return 0;
	if((f->eflags & 0x28) != 0) return 0;

	return 1;
}
#endif

/* --- Re-entrant elections ------------------------------------------------
*
*  schedule() can be entered again before the election it just made has been
*  carried out, and the frame it is handed then belongs to a different task
*  than the one it believes is running. Getting that wrong writes one task's
*  saved context into another task's slot, and the machine dies later, in a
*  place that says nothing about the cause -- so this is worth setting out in
*  full.
*
*  HOW IT HAPPENS. irq_handler() ends a timer interrupt like this:
*
*      cli
*      new_cpu = schedule(r);      // elects the next task, sets current_task
*      sti                         // <- interrupts back on
*      outportb(0x20, 0x20);       // <- EOI: the PIC may now deliver again
*      return new_cpu;             // -> irq_common_stub: mov %eax,%esp; iret
*
*  Between that EOI and the stub's "mov %eax,%esp" the election has been made
*  -- current_task already names the incoming task -- but the CPU is still
*  running on the OUTGOING task's kernel stack. If the PIT ticked again while
*  the handler was running, the tick is pending at the instant of the EOI and
*  is delivered on the very next instruction. It pushes its frame onto the
*  outgoing task's stack and calls schedule() a second time.
*
*  This is not theory. Under "-d int" the interrupted address in that nested
*  frame is the instruction after the "out" in outportb() every single time --
*  the EOI itself -- and the frame sits one interrupt frame below the outer
*  one on the same stack.
*
*  WHAT GOES WRONG WITHOUT THIS. The nested call saves its frame as
*  task_states[current_task], i.e. it files the OUTGOING task's registers under
*  the INCOMING task's slot. The incoming task's real context is lost, and what
*  replaces it is a pointer into a stack that its owner goes on using, so the
*  frame is overwritten by ordinary calls. When the incoming task is next
*  dispatched, the stub pops segment registers out of that overwritten memory:
*  general protection fault at "pop %fs" in irq_common_stub, with a plausible
*  gs and a data word in fs. It also runs away -- each nested election hands
*  back the frame it just filed, so the next tick nests one interrupt frame
*  deeper down the same stack until it reaches the guard page.
*
*  WHY IT SURFACED WITH BLOCKING. The window is old; what changed is how often
*  it matters. It only bites when current_task actually changed in that window,
*  and before there was a wait, a task kept the CPU for its whole slice, so a
*  tick re-elected rarely. Now a task that blocks is descheduled on the very
*  next tick, so most ticks change current_task and nearly every occurrence of
*  the race lands on the case that corrupts.
*
*  THE ANSWER. An election that is already in flight must be allowed to finish.
*  A nested call does nothing at all and hands back the frame it was given: the
*  nested stub then returns straight to the interrupted handler, which carries
*  on and performs its own switch with a context that is still fresh, because
*  nothing has run in between. Saving would be wrong as well as unnecessary --
*  the outgoing task's resume point is the OUTER frame, which the outer call
*  already filed, not this one.
*
*  DETECTING IT. A frame lies on the kernel stack of the task it belongs to.
*  So: if the frame handed in is not on current_task's stack but is on the
*  stack of some other task that is still alive, an election is in flight and
*  we are inside it. Live means RUNNING or BLOCKED, and the restriction is what
*  keeps this away from the one case where a task legitimately runs on another
*  task's stack: sys_exit() and fault_handler() switch by copying a context
*  over their own frame, so a ring 0 task carries on for a few instructions on
*  the stack of the task that just died -- and that task is EXITED or ABORTED,
*  never live.
*
*  The cost is one range test per tick. The table walk below only runs when
*  that test fails, which is the anomaly itself.
*
*  This does not repair irq_handler(). The proper fix is one line in irq.c --
*  drop the cli/sti pair around the schedule() call, since the gate already
*  entered with IF clear and the iret at the end of the stub restores the
*  caller's IF anyway -- and with it the window does not exist. Until that
*  happens, this makes the scheduler correct in spite of it. */

/* Is this frame on the kernel stack of that slot? */
static int frame_on_stack(const struct regs *cpu, int slot)
{
	uint32_t addr;
	uint32_t base;

	if(slot < 0 || task_kstacks[slot] == 0) return 0;

	addr = (uint32_t) cpu;
	base = (uint32_t) task_kstacks[slot];

	return addr >= base && addr < base + KERNEL_STACK_SIZE;
}

/* The live task whose kernel stack this frame is on, or -1. */
static int live_frame_owner(const struct regs *cpu)
{
	int i;

	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state != TASK_STATE_RUNNING
		   && tasks[i].state != TASK_STATE_BLOCKED) continue;

		if(frame_on_stack(cpu, i)) return i;
	}

	return -1;
}

/* How many nested elections have been turned away. Nothing reads it -- it
*  exists so that the condition can be confirmed to occur, and confirmed to be
*  handled, with a debugger or a watch expression rather than by waiting for
*  the crash it used to cause. */
static uint32_t sched_nested_elections;

struct regs* schedule(struct regs* cpu)
{
	int i;
	int slot;
	int next;
	int start;
	int expired;
	addrspace_t space;

	//Is an election already in flight? See the block above schedule(): a frame
	//that is not on the current task's stack, but is on the stack of another
	//task that is still alive, means this call is nested inside an interrupt
	//handler that has already elected somebody and has not yet switched to it.
	//Do nothing and hand the frame straight back, so that handler resumes and
	//completes its own switch.
	//
	//The test is inside "current_task >= 0" because before the first task
	//exists there is no election to be nested inside, and no stack to compare
	//against either.
	if(current_task >= 0 && !frame_on_stack(cpu, current_task))
	{
		int owner = live_frame_owner(cpu);

		if(owner >= 0)
		{
			sched_nested_elections++;
#if SCHED_DEBUG
			if(sched_debug_lines < SCHED_DEBUG_MAX)
			{
				sched_debug_lines++;
				dbg_puts("[sched] nested election #");
				dbg_dec((int) sched_nested_elections);
				dbg_puts(": frame "); dbg_hex((uint32_t) cpu);
				dbg_puts(" is on the stack of task "); dbg_dec(owner);
				dbg_puts(" (state "); dbg_dec(tasks[owner].state);
				dbg_puts("), current_task="); dbg_dec(current_task);
				dbg_puts(", interrupted at "); dbg_hex(cpu->eip);
				dbg_puts(", int_no="); dbg_hex(cpu->int_no);
				dbg_puts("\n");
			}
#endif
			return cpu;
		}
	}

	//If a task is running, save its state.
    if (current_task >= 0) {
        task_states[current_task] = cpu;
    }

	//Deadlines first, so that a task whose timeout came due this tick is
	//runnable in time for the election a few lines down instead of a whole
	//round of slices later. One integer comparison when nothing is armed.
	expired = 0;
	if(wait_deadline_next != 0) expired = wait_check_timeouts();

	//A task that just became runnable again must not have to sit out the rest
	//of somebody else's slice. Dropping the running task's remainder makes the
	//election below happen now; it costs the interrupted task the tail of its
	//slice and nothing more, because schedule() refills the slice of whoever it
	//deschedules. Same reasoning as in task_wake(), and the same trade: under a
	//storm of wakeups a compute bound task is cut back towards one tick per
	//turn, which is slower but never starvation.
	if(expired && current_task >= 0) tasks[current_task].cpu_time = 0;

	//On the very first call current_task is still -1, so look for a task right away
	//(no access to tasks[-1]).
	//
	//The state test is the second half of this condition and it is what makes
	//blocking cost a tick instead of a full slice. Marking a task
	//TASK_STATE_BLOCKED takes it out of the running for future elections, but
	//without this line it would keep the CPU it already has: the else branch
	//below hands the same context straight back until cpu_time runs out, so a
	//task that blocks in the first millisecond of a twenty millisecond slice
	//would sit in its halt loop for the other nineteen while other tasks are
	//ready to run. Re-electing whenever the current task is not RUNNING covers
	//BLOCKED, SUSPENDED, ABORTED and EXITED in one test - the three "cpu_time
	//= 0" lines further down in this file each patch one of those cases from
	//the outside, and this is the same rule stated once, in the one place that
	//actually makes the decision.
	if(current_task < 0
	   || tasks[current_task].state != TASK_STATE_RUNNING
	   || tasks[current_task].cpu_time <= 0)
	{
		if(current_task >= 0)
		{
			tasks[current_task].cpu_time = tasks[current_task].priority;
		}

		//look for the next task to run; the search wraps around at most once
		//instead of spinning forever inside the interrupt handler
		next = -1;
		start = current_task + 1;	//so for current_task == -1 this starts at slot 0
		for(i=0; i <= MAX_TASKS-1; i++)
		{
			slot = (start + i) % MAX_TASKS;
			if(tasks[slot].state == TASK_STATE_RUNNING && task_states[slot] != 0)
			{
				next = slot;
				break;
			}
		}

		//Nothing is runnable. Before giving up, look for a task that is merely
		//BLOCKED - and elect it.
		//
		//That sounds like the one thing a scheduler must not do, and it is safe
		//for a specific reason: a blocked task is not parked in some arbitrary
		//place, it is parked inside the halt loop in task_wait(), which tests
		//its own state before every hlt. Dispatching it puts the CPU back on
		//that loop, it finds itself still blocked, and it halts again with
		//interrupts on. So a blocked task is a perfectly good idle task, and it
		//is the only idle task this kernel has.
		//
		//Without this the machine dies in two ways that are easy to reach.
		//
		//  - Everything is blocked at once - every task waiting for a key, a
		//    packet or a deadline. The first pass finds nothing, and the plain
		//    "return cpu" below hands back the context that was interrupted.
		//    That is survivable while that context happens to be a blocked
		//    task's own halt loop, which is the usual case, but it is survival
		//    by luck rather than by construction, and it is not the case that
		//    matters.
		//  - The one that is not survivable: sys_exit() and the page fault
		//    handler both get a dying task off the CPU by calling schedule()
		//    until it returns a context different from the one they passed in,
		//    and both print "No runnable task left - CPU HALT" and stop the
		//    machine when it never does. A shell that waits for the program it
		//    spawned now waits by blocking, so at the moment that program
		//    exits the shell is very often the only other task and it is
		//    BLOCKED. Without this pass, every single program that runs to
		//    completion while the shell waits for it would halt the machine.
		//
		//The search starts at current_task rather than one past it, so a
		//blocked task that is already the current one is picked first and no
		//context is switched at all; that is the all-blocked idle, and it costs
		//one table walk per tick on a CPU that is halted anyway. For a dying
		//task the filter skips it - it is EXITED or ABORTED, not BLOCKED - so a
		//different context comes out and the callers above get what they need.
		if(next < 0)
		{
			start = current_task;
			if(start < 0) start = 0;

			for(i=0; i <= MAX_TASKS-1; i++)
			{
				slot = (start + i) % MAX_TASKS;
				if(tasks[slot].state == TASK_STATE_BLOCKED && task_states[slot] != 0)
				{
					next = slot;
					break;
				}
			}
		}

		//Not one task left that could hold a CPU: keep using the previous
		//context unchanged. There is nothing else to hand back.
		if(next < 0) return cpu;

		//Point the TSS at the incoming task's kernel stack. From here on any
		//entry from ring 3 - a timer IRQ, an exception, an int 0x80 - makes
		//the CPU switch to exactly this stack, which is also the one the saved
		//struct regs of this task lives on.
		//
		//Only a real change needs the write: nothing but this line touches
		//esp0, so when the scheduler re-elects the task that is already
		//running the TSS still holds the right value. Setting it for a ring 0
		//task is not strictly necessary either - no privilege change means no
		//stack switch, so the CPU never reads esp0 while such a task runs -
		//but it is a single store and it keeps one invariant instead of two:
		//esp0 always names the current task's kernel stack. That also covers
		//the case of a ring 0 task dropping into ring 3 on its own via
		//enter_user_mode().
		//
		//Order against the CR3 load further down does not matter. The TSS is
		//kernel data and esp0 is a kernel address, so both are identical in
		//every space and neither write depends on which directory is loaded.
		//Nor can anything observe an in-between state: this runs inside an
		//interrupt gate with IF cleared, and esp0 is read by the CPU only on
		//an entry from ring 3 - impossible while we are the ring 0 code
		//between these two lines.
		if(next != current_task)
		{
			tss_set_kernel_stack(kernel_stack_top(next));
		}

		current_task = next;

	} else {
		tasks[current_task].cpu_time--;
	}

	//Give the CPU the elected task's address space. Deliberately the last
	//thing that happens, and driven by the space rather than by the slot
	//number: the else branch above re-elects the running task without ever
	//looking at next, and a slot whose space was swapped underneath it would
	//be missed by a "did the task change" test. Comparing against CR3 covers
	//both and costs one register read per tick.
	//
	//This is safe here for one reason: entries 768..1023 of every directory
	//are the same page tables, so everything the kernel is standing on right
	//now keeps its mapping across the load.
	//  - the code: schedule() and its caller are linked above
	//    KERNEL_VIRTUAL_BASE, so the instruction after the write to CR3 is
	//    fetched from a still valid mapping,
	//  - the stack: esp points into the kernel stack of a task, which is a
	//    frame addressed through the direct mapping, so the return address
	//    and the locals survive,
	//  - the value returned: task_states[] points into such a stack as well,
	//    so the pointer irq_common_stub loads into esp and pops the new
	//    context from is mapped in the new space too.
	//The kernel stacks moved out of the bss into individually allocated
	//frames, which changes nothing about this argument: both live in the
	//kernel half, and the kernel half is the same page tables in every
	//directory. It does add a rule - a stack must never need a page table
	//that did not already exist when the first address space was created -
	//and that rule is met by construction, because a stack is a frame seen
	//through the direct mapping vmm_init() completed long before mt_install().
	//Nothing below this line reads user memory, which is the other half of the
	//argument - the old user half is gone the moment CR3 changes.
	space = task_space[current_task];
	if(space == 0) space = vmm_kernel_space();

	if(space != vmm_current_space())
	{
		vmm_switch_space(space);
	}

    cpu = task_states[current_task];

#if SCHED_DEBUG
    /* The last thing before the stub is told to restore this. Anything that
    *  reaches here and is not a frame is a lost context, and saying so now
    *  names the task; letting it through faults in irq_common_stub instead,
    *  with nothing left to say which slot it came from. */
    if(!frame_is_sane(cpu) && sched_debug_lines < SCHED_DEBUG_MAX)
    {
        sched_debug_lines++;
        dbg_puts("[sched] LOST CONTEXT: task "); dbg_dec(current_task);
        dbg_puts(" frame "); dbg_hex((uint32_t) cpu);
        dbg_puts(" cs="); dbg_hex(cpu ? cpu->cs : 0);
        dbg_puts(" ds="); dbg_hex(cpu ? cpu->ds : 0);
        dbg_puts(" fs="); dbg_hex(cpu ? cpu->fs : 0);
        dbg_puts(" eflags="); dbg_hex(cpu ? cpu->eflags : 0);
        dbg_puts("\n");
    }
#endif

    return cpu;
}

//No caller evaluates a result, hence void instead of int.
//
//Nothing is allocated here, and that is the point: not one of the 64 slots
//owns a kernel stack until a task moves into it. What has to hold instead is
//that the scheduler can never reach a slot whose stack is not there yet, and
//that follows from two things.
//
//  - Marking every slot TASK_STATE_NULL is the whole initialisation. A slot
//    only becomes a candidate in schedule() once it is TASK_STATE_RUNNING AND
//    has a saved context, and taskmgr_add_*_task() sets neither before its
//    kernel stack exists: occupy_slot() leaves the task suspended, and only
//    taskmgr_task_start() - a separate call, made by whoever created the task
//    - makes it runnable. So the stack is always older than the first tick
//    that could look at it.
//  - The timer IRQ is in fact still masked when this runs; kernel.c does its
//    "sti" further down and creates the console task after that. But the
//    scheduler is written not to depend on that ordering, and it must not
//    start to: taskmgr_add_task() runs with interrupts on for every task
//    after the first one, and a tick landing in the middle of it finds either
//    a slot that is not runnable yet or one that is complete.
void mt_install()
{
	int i;
	for(i=0; i <= MAX_TASKS-1; i++) //Mark all task slots as unused
	{
		tasks[i].state = TASK_STATE_NULL;
	}
}

//Picks the slot a new task goes into: a never used one first, and only when
//none is left the slot of a task that has been aborted. Returns -1 when even
//that fails. Split out because a ring 0 and a ring 3 task pick their slot by
//exactly the same rules.
static int find_free_slot()
{
	int i;
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_NULL) return i;
	}

	//Only overwrite a finished task when no free slot is left. Both endings
	//count: a task that exited normally is just as over as one that was
	//aborted, and refusing to recycle it would make a machine run out of slots
	//after 64 successful programs.
	//
	//TASK_STATE_BLOCKED is deliberately not in this list and must never be. A
	//blocked task is alive - it is sitting inside task_wait() waiting for a key
	//or a packet - and recycling its slot would hand its kernel stack, its
	//address space and its saved context to a new program while the old one is
	//still standing on all three. "Not RUNNING" is not "finished"; the two
	//endings are, and they are named here one by one for that reason.
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_ABORTED) return i;
		if(tasks[i].state == TASK_STATE_EXITED) return i;
	}

	return -1;
}

static void occupy_slot(int slot, const char *name, int prio)
{
	//The previous occupant may have been killed while it was blocked, in which
	//case it never got to clear its own wait entry on the way out of
	//task_wait(). Nothing reads those entries for a slot that is not BLOCKED,
	//so this is not what keeps a wake from going astray - but a channel left
	//behind in a slot that is being handed to a new program is exactly the kind
	//of leftover that becomes one later.
	wait_forget(slot);

	/* Same reasoning as wait_forget() above, for the other thing a slot can
	*  be carrying when it is handed on: a start that was asked for and never
	*  released belongs to the previous occupant, not to this one. */
	task_start_pending[slot] = 0;

	tasks[slot].pid = slot;
	copy_bounded(tasks[slot].name, name, sizeof(tasks[slot].name));
	tasks[slot].priority = prio;
	tasks[slot].state = TASK_STATE_SUSPENDED;
}

int taskmgr_add_task( void* tfunct, const char *name, int prio)
{
	uint8_t *kstack;
	int      i;

	//Before the slot search, so a space or a kernel stack that was still live
	//the last time a slot was recycled gives its frames back in time to be
	//used again here.
	reap_pending_space();
	reap_pending_kstack();

	i = find_free_slot();

	if(i < 0)
	{
		printf("ERR: Task could not be started, no available pid left!\n");
		return -1;
	}

	//A ring 0 task has no business with a user half. If a ring 3 task used
	//this slot before, its address space - and with it its user stack - goes
	//away here instead of staying allocated. The slot falls back to
	//task_space[i] == 0, the kernel space, which is where a ring 0 task runs.
	task_space_release(i);

	//Same for the previous occupant's kernel stack, and for the same reason
	//it happens before the allocation below rather than after: two frames
	//that come back here may well be the two the new task is about to get.
	kernel_stack_release(i);

	//The one allocation in this function, and the one thing that can fail
	//now that a kernel stack is no longer simply there. Nothing about the
	//slot has been changed yet at this point - the state is still whatever
	//find_free_slot() accepted, TASK_STATE_NULL or TASK_STATE_ABORTED, and
	//task_states[i] was cleared above - so returning here leaves the slot
	//exactly as free as it was found. There is no half built task to undo,
	//and the next attempt will pick the very same slot.
	kstack = kernel_stack_create();
	if(kstack == 0)
	{
		printf("ERR: Task '%s' could not be started, no memory for its kernel stack!\n", name);
		return -1;
	}

	occupy_slot(i, name, prio);

	//The stack is recorded before the context is built on it, so that
	//kernel_stack_top() is right the first time the scheduler asks - which
	//it can only do once taskmgr_task_start() makes the slot runnable, but
	//the order costs nothing and leaves no window to reason about.
	task_kstacks[i] = kstack;
	task_states[i] = init_task(kstack, tfunct);

	//printf("Task '%s' started with PID %i\n", name, i);
	return i;
}

//Starts a task that begins life in ring 3. Same slot handling and same
//priorities as taskmgr_add_task(), it only differs in what the task gets: an
//address space of its own, a user stack inside it, and an initial context
//that iret drops into ring 3.
//
//tfunct may be 0. That is the case of a program that is not in memory yet:
//there is no entry point to name before the image has been parsed, and the
//image cannot be placed before the address space it goes into exists. The
//task is therefore built complete except for eip, the caller asks for the
//space with taskmgr_task_space(), maps the program into it, and hands over
//the entry point with taskmgr_task_set_entry(). Since every task is created
//suspended, none of this is observable from outside.
int taskmgr_add_user_task( void* tfunct, const char *name, int prio)
{
	uint32_t    user_esp;
	addrspace_t space;
	uint8_t    *kstack;
	int         i;

	reap_pending_space();
	reap_pending_kstack();

	i = find_free_slot();
	if(i < 0)
	{
		printf("ERR: Task could not be started, no available pid left!\n");
		return -1;
	}

	//Whatever the previous occupant of the slot left behind goes first: the
	//new task gets a space of its own and must not inherit the dead one, and
	//the frames come back early enough to serve the allocations below.
	task_space_release(i);
	kernel_stack_release(i);

	space = vmm_create_space();
	if(space == 0)
	{
		printf("ERR: Task '%s' could not be started, no memory for its address space!\n", name);
		return -1;
	}

	//Still before anything about the slot is changed, so a failure here leaves
	//no half built task behind - only the released space of the previous
	//occupant, which was dead anyway.
	user_esp = user_stack_create(i, space);
	if(user_esp == 0)
	{
		vmm_destroy_space(space);
		printf("ERR: Task '%s' could not be started, no memory for its user stack!\n", name);
		return -1;
	}

	//The kernel stack comes last of the three allocations, which is what
	//keeps the unwinding to a single line: the user stack frame and the page
	//table holding it belong to the space, so destroying the space gives
	//everything allocated so far back at once. Ordered the other way round
	//each later failure would have to remember to release the kernel stack
	//as well.
	kstack = kernel_stack_create();
	if(kstack == 0)
	{
		vmm_destroy_space(space);
		printf("ERR: Task '%s' could not be started, no memory for its kernel stack!\n", name);
		return -1;
	}

	task_space[i] = space;
	task_kstacks[i] = kstack;

	occupy_slot(i, name, prio);

	task_states[i] = init_user_task(kstack, tfunct, user_esp);

	return i;
}

//Pure output function, no caller evaluates a result, hence void instead of int.
void taskmgr_list_tasks()
{
	int i;

	printf("Running Tasks:\n");
	
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state != TASK_STATE_NULL)
		{
			printf(" %s (%i), Priority: %i, State: %s", tasks[i].name, tasks[i].pid, tasks[i].priority, readable_task_state[tasks[i].state]);
			//An abort has a reason worth reading; an exit has a status, and
			//a status of 0 is the uninteresting case that needs no ceremony.
			if(tasks[i].state == TASK_STATE_ABORTED)
			{
				printf(" ( ERR %i, %s )", tasks[i].error_no, tasks[i].error);
			}
			else if(tasks[i].state == TASK_STATE_EXITED && tasks[i].error_no != 0)
			{
				printf(" ( status %i )", tasks[i].error_no);
			}
			printf("\n");
		}
	}
}

//How many tasks are alive, which is what the status bar shows.
//
//BLOCKED counts. A task waiting for a key or a packet has not gone anywhere -
//it will run again the moment its wake arrives - and a task counter that made
//the number in the status bar drop every time the shell waited for a keystroke
//would be reporting the opposite of what is happening. This is the rule
//system.h states next to TASK_STATE_BLOCKED: anything that treats RUNNING as
//"alive" has to count this too.
int taskmgr_get_taskcount()
{
	int i;
	int count = 0;
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_RUNNING
		   || tasks[i].state == TASK_STATE_BLOCKED)
		{
			count++;
		}
	}
	return count;
}

//How many of them are waiting rather than running, so that a shell can say so.
int taskmgr_blocked_count(void)
{
	int i;
	int count = 0;
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_BLOCKED) count++;
	}
	return count;
}

/* Blocks the calling task until somebody wakes the channel or the timeout
*  runs out. 1 for a wake, 0 for a timeout. See system.h for what a channel is
*  and for the loop every caller has to be written as; what follows is how the
*  three hard parts of that promise are actually kept.
*
*  HOW THE TASK STOPS RUNNING. It marks itself blocked and then halts, in a
*  loop, until its own state says otherwise. It does not switch tasks itself.
*  The timer IRQ arrives within one tick, schedule() sees a current task that is
*  no longer TASK_STATE_RUNNING and elects somebody else, and the halted task's
*  context is saved exactly where it stands - between the hlt and the cli. Later
*  a wake, or an expired deadline, puts the state back to RUNNING; the scheduler
*  elects the task in the normal way, the iret lands on the instruction after
*  the hlt, and the loop falls out.
*
*  The cost of that is at most one tick - one millisecond at 1000 Hz - of a CPU
*  that is halted rather than spinning, once per block, before another task gets
*  it. The alternative was a software interrupt vector that switches on the spot
*  instead of at the next tick, and it is the wrong trade here: it buys back a
*  millisecond of idle CPU in exchange for an IDT entry, an assembly stub and a
*  second path into the scheduler that has to get the saved frame exactly right.
*  It would also be no help at all in the case that matters most - when every
*  task is blocked there is nothing to switch TO, and the halt loop is then not
*  a fallback but the entire idle mechanism. The scheduler elects a blocked task
*  when it can find nothing else precisely because this loop is safe to be
*  dispatched into at any time: it re-tests before every hlt.
*
*  It is also worth being clear that "one tick" is the cost of BLOCKING, not the
*  latency of a WAKE. A wake that arrives before the next tick finds the task
*  still current with slice left, and the task simply carries on - the whole
*  wait costs nothing.
*
*  INTERRUPTS. The caller arrives with IF clear; that is what closes the lost
*  wakeup race, because it means nothing can change the condition between the
*  caller's test and the state change below. But a task that is not running must
*  have interrupts on or nothing will ever wake it, and "hlt with IF clear" is
*  a CPU that only a reset leaves. The loop body is therefore the one form that
*  makes that state unreachable: "sti; hlt" as a single instruction pair, which
*  the hardware guarantees is atomic - sti holds interrupt recognition off for
*  exactly one instruction, so no interrupt can be taken in the gap and the hlt
*  is always entered with IF already set. The cli that follows puts the state
*  back before the loop condition is read again, so every test of the state
*  happens with interrupts off, exactly as the first one did.
*
*  The caller's flags are saved and restored around the whole thing rather than
*  relying on that trailing cli, so a caller that did not arrive with IF clear -
*  which is a caller with a lost-wakeup bug, but still - gets back what it had.
*
*  NOT FROM INTERRUPT CONTEXT. There is no current task inside a handler, only
*  a task that was interrupted, and blocking it here would leave a half finished
*  interrupt on its kernel stack and re-enable interrupts inside a handler that
*  was entered with them off. There is no cheap way to detect that from here, so
*  it is a rule rather than a check: wake from handlers, wait from tasks. */
int task_wait(const void *channel, int timeout_ms)
{
	unsigned long flags;
	unsigned int  deadline;
	int           slot;
	int           result;

	slot = current_task;

	/* No task is running: the boot path, before the console task exists, or
	*  the kernel itself. There is nothing for the scheduler to take the CPU
	*  away from, so blocking would be halting the machine. Reporting a timeout
	*  is the honest answer - the caller's loop gives up and carries on, which
	*  is what every one of these call sites did before there was a wait at
	*  all. */
	if(slot < 0) return 0;

	/* No channel and no deadline is a wait nothing can end. It is always a
	*  caller bug, and the symptom would be a task that never runs again and
	*  possibly a machine with nothing left to run, so it is refused here where
	*  it is still one wrong argument rather than a hang. */
	if(channel == 0 && timeout_ms <= 0) return 0;

	__asm__ __volatile__ ("pushfl; popl %0" : "=r" (flags) : : "memory");

	wait_result[slot]   = 1;
	wait_channel[slot]  = channel;
	wait_deadline[slot] = 0;

	if(timeout_ms > 0)
	{
		/* Absolute, in milliseconds of uptime, which is the unit the timeout
		*  is given in and the unit timer_get_ticks() answers in - no
		*  conversion, and nothing to keep in step with the tick frequency.
		*  0 is the "no deadline" marker, so a deadline that lands exactly on
		*  the wrap is nudged by one millisecond rather than losing its
		*  timeout. */
		deadline = (unsigned int) timer_get_ticks() + (unsigned int) timeout_ms;
		if(deadline == 0) deadline = 1;

		wait_deadline[slot] = deadline;

		if(wait_deadline_next == 0 || !time_reached(deadline, wait_deadline_next))
		{
			wait_deadline_next = deadline;
		}
	}

	tasks[slot].state = TASK_STATE_BLOCKED;

	while(tasks[slot].state == TASK_STATE_BLOCKED)
	{
		/* The "memory" clobber is load bearing twice over: it stops the
		*  compiler from hoisting the state read out of the loop - tasks[] is
		*  ordinary memory written by an interrupt handler - and it keeps the
		*  three instructions in this order. */
		__asm__ __volatile__ ("sti; hlt; cli" : : : "memory");
	}

	result = wait_result[slot];

	/* The state can leave BLOCKED without going through wait_end(): an outside
	*  taskmgr_task_start(), or a suspend followed by a start. Clearing here as
	*  well means no slot is ever left naming a channel it is not waiting on,
	*  whichever way the wait ended. */
	wait_channel[slot]  = 0;
	wait_deadline[slot] = 0;

	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");

	return result;
}

/* Wakes everything waiting on a channel.
*
*  Two waiters on the same address are both woken, and that is the intended
*  behaviour rather than a simplification: the idiom in system.h has every
*  waiter re-test its own condition, so waking one too many costs a turn around
*  a loop while waking one too few costs a task that never runs again.
*
*  Safe from interrupt context, which is the normal case. All it does is walk
*  the table and store states - no allocation, no printing, no scheduling, and
*  above all no task switch, which in a handler would mean returning onto a
*  stack that belongs to a different task than the one that was interrupted.
*
*  The one thing it does beyond changing states is drop the RUNNING task's
*  remaining slice, and that is what decides how long a wake takes to arrive.
*  Without it a task woken by a keystroke waits for the task that happens to be
*  running to use up its slice first - up to twenty milliseconds with the
*  console's realtime priority - and a shell that is woken twenty milliseconds
*  after the key was pressed is exactly the latency this whole mechanism exists
*  to remove. With it the next tick re-elects, so a wake reaches the woken task
*  within a tick or two on a machine with a handful of tasks. It is still a
*  round robin: with many runnable tasks in between, the bound is the sum of
*  their slices, which is what a scheduler without priorities can promise.
*
*  It is one integer store, it cannot switch anything, and it is skipped when
*  the woken task is the one already running - that task needs its slice, not
*  a preemption. What it costs is fairness under a storm of wakeups: a compute
*  bound task loses the tail of its slice each time and is cut back towards one
*  tick per turn. It is never starved, because schedule() gives it a full fresh
*  slice every time it deschedules it. */
/* The address woken whenever a task ends. Its value is never read -- only its
*  address is, as a channel -- so one byte is enough and what it holds is
*  irrelevant. See the declaration in system.h for why there is one of these
*  rather than one per pid. */
static char task_exit_channel_marker;

const void *taskmgr_exit_channel(void)
{
	return (const void *)&task_exit_channel_marker;
}

void task_wake(const void *channel)
{
	int i;
	int preempt;

	/* 0 is not an address. It is also the value sleep() blocks on - a wait with
	*  no channel, only a deadline - so answering a null wake would wake every
	*  sleeping task on the machine at once. */
	if(channel == 0) return;

	preempt = 0;

	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state != TASK_STATE_BLOCKED) continue;
		if(wait_channel[i] != channel) continue;

		wait_result[i] = 1;
		wait_end(i);

		if(i != current_task) preempt = 1;
	}

	if(preempt && current_task >= 0) tasks[current_task].cpu_time = 0;
}

/* Gives up the rest of the current time slice.
*
*  For a task that has work left but nothing to wait for, so there is no state
*  to change and nothing to be woken by: it drops its slice and halts until the
*  tick that hands the CPU on. The loop is there because hlt returns on ANY
*  interrupt - a keystroke, a packet - and only the timer runs the scheduler, so
*  a yield that stopped at the first interrupt would often not have yielded at
*  all. Waiting for the millisecond counter to move is waiting for exactly the
*  interrupt that matters.
*
*  Costs at most one tick even when nothing else wants the CPU: with no other
*  runnable task the scheduler re-elects this one on that tick and the loop
*  falls out immediately.
*
*  The flags are saved and restored for the same reason as in task_wait() -
*  interrupts have to be on across the hlt, and a caller that had them off is
*  entitled to have them off again on return. */
void task_yield(void)
{
	unsigned long flags;
	int           start;
	int           slot;

	slot = current_task;
	if(slot < 0) return;

	__asm__ __volatile__ ("pushfl; popl %0" : "=r" (flags) : : "memory");

	tasks[slot].cpu_time = 0;

	start = timer_get_ticks();

	do
	{
		__asm__ __volatile__ ("sti; hlt" : : : "memory");
	}
	while(timer_get_ticks() == start);

	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

int taskmgr_get_currpid()
{
	return current_task;
}

//The address space a task runs in, or 0 when there is none to hand out: an
//invalid pid, or a task that runs in the kernel space. Both cases collapse
//into the same answer on purpose, because task_space[] already uses 0 for
//"the kernel space" and 0 is never a valid directory - the pmm does not hand
//out frame zero.
//
//This is the read side of the split that lets a program be placed into a task
//before the task runs: taskmgr_add_user_task() builds the space, the caller
//maps into it through vmm_map_in(), and taskmgr_task_set_entry() closes the
//context afterwards. The space stays owned by the task manager; it is
//released with the slot in task_space_release() and must not be destroyed by
//the caller.
addrspace_t taskmgr_task_space(int pid)
{
	if(pid < 0 || pid > MAX_TASKS-1) return 0;

	return task_space[pid];
}

//Fills in the entry point of a task that has not started yet, i.e. the eip of
//its saved context. Returns 0 on success and a negative value otherwise.
//
//Writing into a saved struct regs from the outside is only ever safe while the
//task is standing still, so the conditions are deliberately narrow:
//
//  - the pid names a slot and that slot has a context at all. task_states[] is
//    0 for a slot that was never used, and dereferencing it would write into
//    whatever lies at address 0 - which is not mapped, so it would fault, but
//    a refusal says more than a page fault does.
//  - the task is suspended. That is the state every task is created in and the
//    one it stays in until taskmgr_task_start(), so it is exactly the window
//    in which no CPU is looking at the frame. A running task would have its
//    context overwritten from the kernel stack on the next interrupt anyway,
//    and an aborted one is on its way out.
//  - entry is not 0. Zero is the "not known yet" marker itself, so accepting
//    it would only re-arm the very trap taskmgr_task_start() guards against.
//
//This does not check that entry is actually mapped in the task's space. It
//cannot, in general: the caller may well map the image only afterwards, and
//the mapping lives in a foreign directory. An unmapped entry ends in a page
//fault at that address in ring 3, which the fault handler reports with CR2.
int taskmgr_task_set_entry(int pid, uint32_t entry)
{
	if(pid < 0 || pid > MAX_TASKS-1) return -1;
	if(task_states[pid] == 0) return -1;
	if(tasks[pid].state != TASK_STATE_SUSPENDED) return -1;
	if(entry == 0) return -1;

	task_states[pid]->eip = entry;

	return 0;
}

//Moves a suspended ring 3 task's initial user stack pointer.
//
//The counterpart of taskmgr_task_set_entry(), and it exists for the same
//reason: a loader has to finish building a task after the task manager created
//it. add_user_task() hands out a stack page and points esp at its very top,
//which is right for a program that gets nothing - but a program that gets
//arguments needs them written into that page first, and esp then has to start
//BELOW the block instead of above it.
//
//Without this call exec.c had to reach the same end by writing twelve bytes of
//machine code into the top of the stack page ("mov eax, entry; mov esp, base;
//jmp eax") and entering there. That works - 32 bit x86 without PAE has no NX
//bit - but executing off the stack is precisely the pattern hardware spent two
//decades learning to forbid, and it made the loader's output depend on the
//encoding of three instructions. One field write says the same thing.
//
//The conditions match set_entry() exactly, and for the same reasons: a slot
//with a context, suspended so that no CPU is looking at the frame, and a value
//that is not the "not set" marker. Whether esp is mapped in the task's space
//is not checked and cannot be - the caller usually maps it afterwards, in a
//directory that is not the active one. A wrong value faults in ring 3 with
//CR2 naming it, which is a readable failure.
int taskmgr_task_set_stack(int pid, uint32_t user_esp)
{
	if(pid < 0 || pid > MAX_TASKS-1) return -1;
	if(task_states[pid] == 0) return -1;
	if(tasks[pid].state != TASK_STATE_SUSPENDED) return -1;
	if(user_esp == 0) return -1;

	task_states[pid]->useresp = user_esp;

	return 0;
}

//What state a slot is in, for a caller that has to wait for a task rather than
//just start one.
//
//The shell needs this: once a command can be a program on disk, "cat file"
//means spawn, then wait, or the prompt comes back and prints over the output.
//Without an accessor the only way to ask was taskmgr_list_tasks(), which
//prints rather than answers.
//
//TASK_STATE_NULL for a pid that names no slot, which is deliberately the same
//answer a slot that was never used gives: to a waiting caller "gone" and
//"never existed" are the same thing, and both mean stop waiting. A caller that
//needs to tell them apart is asking the wrong question.
int taskmgr_task_state(int pid)
{
	if(pid < 0 || pid > MAX_TASKS-1) return TASK_STATE_NULL;

	return tasks[pid].state;
}

void taskmgr_task_abort(int pid, int error_number, const char *error_descr)
{
	if(pid < 0 || pid > MAX_TASKS-1)
	{
		printf("ERR: Task %i could not be found!\n", pid);
	} else {
		tasks[pid].state = TASK_STATE_ABORTED;
		tasks[pid].error_no = error_number;
		copy_bounded(tasks[pid].error, error_descr, sizeof(tasks[pid].error));

		//The slot deliberately stays occupied, taskmgr_add_task() recycles it later.
		//That now covers the kernel stack as well, and freeing it here instead
		//would be the classic way to lose the machine: an abort arrives from
		//exactly three places - exit(), the exception handler, and the kill
		//command - and the first two run in the kernel ON the stack of the task
		//they are aborting. The task also keeps running until the next tick
		//elects somebody else, so its stack is live well past this line. Handing
		//it to the allocator here would give the next caller of malloc() the
		//return addresses this function is standing on.
		//But if the currently running task is aborted, its remaining CPU time must be
		//dropped: otherwise it would keep running in the else branch of schedule() until
		//cpu_time expires, even though it is no longer TASK_STATE_RUNNING.
		if(pid == current_task)
		{
			tasks[pid].cpu_time = 0;
		}

		//Same wake, same ordering rule, as taskmgr_task_exit(): the state is
		//set above and the wake comes after it. An abort reaches here from the
		//exception handler as well, i.e. from interrupt context -- which is
		//safe because waking only changes states and never switches tasks.
		task_wake(taskmgr_exit_channel());
	}
}

//The ordinary end of a task. Everything taskmgr_task_abort() says about not
//freeing anything here applies unchanged -- this runs on the stack of the task
//it is ending, and that task keeps running until the next tick elects somebody
//else -- so the two differ in exactly two things: the state, and the fact that
//there is no error string to keep. The status goes into error_no, which is the
//field "ps" already reads; naming it after the failure case is a leftover from
//when a task could only end by failing.
void taskmgr_task_exit(int pid, int status)
{
	if(pid < 0 || pid > MAX_TASKS-1)
	{
		printf("ERR: Task %i could not be found!\n", pid);
		return;
	}

	tasks[pid].state = TASK_STATE_EXITED;
	tasks[pid].error_no = status;
	tasks[pid].error[0] = '\0';

	//Same reason as in taskmgr_task_abort(): a task that is no longer runnable
	//must not keep the CPU until its slice runs out.
	if(pid == current_task)
	{
		tasks[pid].cpu_time = 0;
	}

	//Tell whoever was waiting for this. The state is set FIRST and the wake
	//comes after, which is the order that matters: a waiter re-tests its
	//condition when it wakes, and waking before the state changed would have
	//it look, see the task still alive, and go back to sleep - having used up
	//the one wake it was going to get.
	task_wake(taskmgr_exit_channel());
}

//Makes a task eligible for the scheduler. This is the one place where the
//"entry point not known yet" state of taskmgr_add_user_task() is caught: a
//task whose saved eip is still 0 would be dispatched into a jump to virtual
//address 0, an address that is deliberately never mapped in any space. The
//page fault handler would name it, but a refusal here is both earlier and
//clearer - it says which task was never given an entry point, instead of
//reporting a fault after the fact.
//
//The check belongs here rather than in schedule(): this runs in task context
//where printf() is allowed, it is the moment the decision is actually made,
//and the scheduler stays free of anything it does not strictly need. A task
//that is refused simply stays suspended, so nothing is left half started.
//
//task_states[pid] is checked on the way, because a slot that was never used
//has none and reading eip through a null pointer would fault. That also makes
//the existing shell path - "start <number>" with a number typed by the user -
//safe against a pid that names an empty slot.
void taskmgr_task_start(int pid)
{
	if(pid < 0 || pid > MAX_TASKS-1)
	{
		printf("ERR: Task %i could not be found!\n", pid);
	} else
	if(task_states[pid] == 0)
	{
		printf("ERR: Task %i has no saved context and can not be started!\n", pid);
	} else
	if(task_states[pid]->eip == 0)
	{
		printf("ERR: Task %i has no entry point and can not be started!\n", pid);
	} else
	if(!boot_handed_over)
	{
		/* Recorded, not refused. Every caller wants exactly this -- see the
		*  block above boot_handed_over -- and a caller that had to ask first
		*  would be back to remembering a rule instead of being held to one.
		*
		*  The validation above has already run, so a bad pid is still
		*  reported here, on the boot path, where the message is visible and
		*  the call is. Deferring the diagnosis to the release would report it
		*  from a context that says nothing about who asked. */
		task_start_pending[pid] = 1;
	} else {
		tasks[pid].state = TASK_STATE_RUNNING;
	}
}

/* The handover: the boot path has nothing left to do, so what it created may
*  run. Called once, from kernel(), after the console task is created -- and
*  the console is NOT a special case here, it is simply the last slot the boot
*  path asked to start.
*
*  INTERRUPTS ARE OFF ACROSS THE LOOP, and that is the whole subtlety of this
*  function. Setting one slot RUNNING with the timer live re-creates the exact
*  bug this exists to remove: the next tick elects that slot, and the rest of
*  the loop -- and the console with it -- never runs. The window is a few
*  instructions wide and would have been a boot that hangs once in a while.
*
*  After the restore a tick may switch away at any point, which is the
*  intended handover rather than a hazard: kernel() returns into start.asm's
*  halt loop, and there is nothing left to lose.
*
*  A slot that stopped being startable in the meantime is skipped rather than
*  resurrected. Nothing does that today; the test costs one comparison and
*  removes a way for a dead task to come back to life. */
void taskmgr_boot_complete(void)
{
	unsigned long flags;
	int i;

	if(boot_handed_over) return;

	__asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");

	boot_handed_over = 1;

	for(i = 0; i < MAX_TASKS; i++)
	{
		if(!task_start_pending[i]) continue;
		task_start_pending[i] = 0;

		if(tasks[i].state == TASK_STATE_SUSPENDED && task_states[i] != 0)
			tasks[i].state = TASK_STATE_RUNNING;
	}

	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

void taskmgr_task_suspend(int pid)
{
	if(pid == 0)
	{
		printf("ERR: System-Task can not be suspended! Use \"exit\" instead for shooting down.");
	} else
	if(pid < 0 || pid > MAX_TASKS-1)
	{
		printf("ERR: Task %i could not be found!\n", pid);
	} else {
		tasks[pid].state = TASK_STATE_SUSPENDED;

		/* A start that has not been released yet is cancelled by this, which
		*  is what "suspended" has to mean for it to mean anything: without
		*  the line, a suspend on the boot path would be quietly undone at the
		*  handover and the task would start after all. */
		task_start_pending[pid] = 0;

		//Same problem as on abort: after being suspended, the currently running task
		//must not keep using up its remaining CPU time.
		if(pid == current_task)
		{
			tasks[pid].cpu_time = 0;
		}
	}
}

//Terminates all tasks from index 1 on. Slot 0 is deliberately spared: it runs the
//console (the system task) through which killall is invoked in the first place -
//if it were aborted as well, no runnable task would be left.
//
//Nothing is released here either, for one reason more than in
//taskmgr_task_abort(): this walks 63 slots at once, and any number of them
//may have been preempted somewhere deep inside the kernel, with a half
//finished call parked on their kernel stacks. Their memory becomes free the
//moment their slot is wanted and not a tick earlier.
//No caller evaluates a result, hence void instead of int.
void taskmgr_killall()
{
	int i;
	for(i=1; i <= MAX_TASKS-1; i++)
	{
		tasks[i].state = TASK_STATE_ABORTED;
	}
}

