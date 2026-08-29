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
//every caller filters it out first, because an unused slot is not a task with
//a state but the absence of one.
char *readable_task_state[] =
{
    "Running",
    "Suspended",
	"Aborted",
	"Exited"
};

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
*  one will never be elected again - schedule() only looks at
*  TASK_STATE_RUNNING - so nothing will ever return onto its stack. */
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


struct regs* schedule(struct regs* cpu)
{
	int i;
	int slot;
	int next;
	int start;
	addrspace_t space;

	//If a task is running, save its state.
    if (current_task >= 0) {
        task_states[current_task] = cpu;
    }

	//On the very first call current_task is still -1, so look for a task right away
	//(no access to tasks[-1]).
	if(current_task < 0 || tasks[current_task].cpu_time <= 0)
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

		//No runnable task found: keep using the previous context unchanged
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
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_ABORTED) return i;
		if(tasks[i].state == TASK_STATE_EXITED) return i;
	}

	return -1;
}

static void occupy_slot(int slot, const char *name, int prio)
{
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

int taskmgr_get_taskcount()
{
	int i;
	int count = 0;
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state != TASK_STATE_NULL)
		{
			if(tasks[i].state == TASK_STATE_RUNNING)
			{
				count++;
			}
		}
	}
	return count;
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
	} else {
		tasks[pid].state = TASK_STATE_RUNNING;
	}
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

