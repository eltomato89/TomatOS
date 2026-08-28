#include <system.h>
#include <stdio.h>
#include "string.h"

#define MAX_TASKS 64
#define NUM_OF_PRIORITIES 4

static int current_task = -1;

task_settings tasks[MAX_TASKS];
static uint8_t task_stacks[MAX_TASKS][4096];
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
	
    struct regs* state = (void*) (stack + 4096 - sizeof(new_state));
    *state = new_state;
 
    return state;
}


struct regs* schedule(struct regs* cpu)
{
	int i;
	int slot;
	int next;
	int start;

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

		current_task = next;

	} else {
		tasks[current_task].cpu_time--;
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

int taskmgr_add_task( void* tfunct, const char *name, int prio)
{
	int i;
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_NULL)
		{
			tasks[i].pid = i;
			copy_bounded(tasks[i].name, name, sizeof(tasks[i].name));
			tasks[i].priority = prio;
			tasks[i].state = TASK_STATE_SUSPENDED;
			
			task_states[i] = init_task(task_stacks[i], tfunct);
			
			//printf("Task '%s' started with PID %i\n", name, i);
			return i;
		}
	}
	
	//Only overwrite aborted tasks when no free task slots are available any more
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_ABORTED)
		{
			tasks[i].pid = i;
			copy_bounded(tasks[i].name, name, sizeof(tasks[i].name));
			tasks[i].priority = prio;
			tasks[i].state = TASK_STATE_SUSPENDED;
			
			task_states[i] = init_task(task_stacks[i], tfunct);
			
			//printf("Task '%s' started with PID %i\n", name, i);
			return i;
		}
	}
	
	printf("ERR: Task could not be started, no available pid left!\n");
	
	return -1;
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

