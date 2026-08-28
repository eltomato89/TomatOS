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

/* The kernel stack of every task. A ring 0 task simply runs on it. A ring 3
*  task never touches it directly: it is the stack the CPU switches to, using
*  esp0 out of the TSS, whenever an interrupt or a system call drags the task
*  into the kernel. Either way this is where the task's saved struct regs
*  lives, which is why every task needs one of its own - two tasks sharing a
*  kernel stack would overwrite each other's saved context.
*
*  Aligned because the saved register frame is carved out of the top of it and
*  is accessed as struct regs, i.e. as 32 bit words. */
static uint8_t task_kernel_stacks[MAX_TASKS][KERNEL_STACK_SIZE] __attribute__((aligned(16)));

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

char *readable_task_state[] =
{
    "Running",
    "Suspended",
	"Aborted",
	"Unused"
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
*  the array. */
static uint32_t kernel_stack_top(int slot)
{
	return (uint32_t) task_kernel_stacks[slot] + KERNEL_STACK_SIZE;
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
*  esp0 the TSS hands out. */
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
	//  - the stack: esp points into task_kernel_stacks[] in the kernel's bss,
	//    so the return address and the locals survive,
	//  - the value returned: task_states[] points into that same array, so the
	//    pointer irq_common_stub loads into esp and pops the new context from
	//    is mapped in the new space as well.
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

	//Only overwrite aborted tasks when no free task slots are available any more
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_ABORTED) return i;
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
	int i;

	//Before the slot search, so a space that was still active the last time a
	//slot was recycled gives its frames back in time to be used again here.
	reap_pending_space();

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

	occupy_slot(i, name, prio);

	task_states[i] = init_task(task_kernel_stacks[i], tfunct);

	//printf("Task '%s' started with PID %i\n", name, i);
	return i;
}

//Starts a task that begins life in ring 3. Same slot handling and same
//priorities as taskmgr_add_task(), it only differs in what the task gets: an
//address space of its own, a user stack inside it, and an initial context
//that iret drops into ring 3.
int taskmgr_add_user_task( void* tfunct, const char *name, int prio)
{
	uint32_t    user_esp;
	addrspace_t space;
	int         i;

	reap_pending_space();

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

	task_space[i] = space;

	occupy_slot(i, name, prio);

	task_states[i] = init_user_task(task_kernel_stacks[i], tfunct, user_esp);

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
			if(tasks[i].state == TASK_STATE_ABORTED)
			{
				printf(" ( ERR %i, %s )", tasks[i].error_no, tasks[i].error);
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

void taskmgr_task_abort(int pid, int error_number, const char *error_descr)
{
	if(pid < 0 || pid > MAX_TASKS-1)
	{
		printf("ERR: Task %i could not be found!\n", pid);
	} else {
		tasks[pid].state = TASK_STATE_ABORTED;
		tasks[pid].error_no = error_number;
		copy_bounded(tasks[pid].error, error_descr, sizeof(tasks[pid].error));

		//The stack slot deliberately stays occupied, taskmgr_add_task() recycles it later.
		//But if the currently running task is aborted, its remaining CPU time must be
		//dropped: otherwise it would keep running in the else branch of schedule() until
		//cpu_time expires, even though it is no longer TASK_STATE_RUNNING.
		if(pid == current_task)
		{
			tasks[pid].cpu_time = 0;
		}
	}
}

void taskmgr_task_start(int pid)
{
	if(pid < 0 || pid > MAX_TASKS-1)
	{
		printf("ERR: Task %i could not be found!\n", pid);
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
//No caller evaluates a result, hence void instead of int.
void taskmgr_killall()
{
	int i;
	for(i=1; i <= MAX_TASKS-1; i++)
	{
		tasks[i].state = TASK_STATE_ABORTED;
	}
}

