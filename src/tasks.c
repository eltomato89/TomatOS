#include <system.h>
#include <stdio.h>
#include "string.h"

#define MAX_TASKS 64
#define NUM_OF_PRIORITIES 4
void prio_queue_dump();
void queue_push(int pid);

static int current_task = -1;

static int q = 2;

task_settings tasks[MAX_TASKS];
static uint8_t task_stacks[MAX_TASKS][4096];
static struct regs* task_states[MAX_TASKS];

unsigned char *readable_task_state[] =
{
    "Running",
    "Suspended",
	"Aborted",
	"Unused"
};


struct regs* init_task(uint8_t* stack, void* entry)
{
    struct regs new_state = {
        .eax = 0, .ebx = 0, .ecx = 0, .edx = 0,
        .esi = 0, .edi = 0, .ebp = 0,
        //.esp = unbenutzt (kein Ring-Wechsel)
        .eip = (uint32_t) entry,
 
        /* Ring-0-Segmentregister */
        .cs  = 0x08,

        /* IRQs einschalten (IF = 1) */
        .eflags = 0x202,
		
		.gs=16, .fs=16,	.es=16,	.ds=16,
		
		.esp=0,	.useresp=0,	.ss=0,
		.int_no=0, .err_code=0,	
    };
	
    struct regs* state = (void*) (stack + 4096 - sizeof(new_state));
    *state = new_state;
 
    return state;
}


 
void init_multitasking(void)
{
		
}
 
struct regs* schedule(struct regs* cpu)
{
	int i;
	int slot;
	int next;
	int start;

	//Wenn Task laeuft, Zustand sichern.
    if (current_task >= 0) {
        task_states[current_task] = cpu;
    }

	//Beim allerersten Aufruf ist current_task noch -1, dann direkt einen Task suchen
	//(kein Zugriff auf tasks[-1]).
	if(current_task < 0 || tasks[current_task].cpu_time <= 0)
	{
		if(current_task >= 0)
		{
			tasks[current_task].cpu_time = tasks[current_task].priority;
		}

		//nächsten auszuführenden Task suchen, höchstens ein kompletter Umlauf
		next = -1;
		start = current_task + 1;	//bei current_task == -1 also ab Slot 0
		for(i=0; i <= MAX_TASKS-1; i++)
		{
			slot = (start + i) % MAX_TASKS;
			if(tasks[slot].state == TASK_STATE_RUNNING && task_states[slot] != 0)
			{
				next = slot;
				break;
			}
		}

		//Kein lauffähiger Task gefunden: bisherigen Kontext unverändert weiterbenutzen
		if(next < 0) return cpu;

		current_task = next;

	} else {
		tasks[current_task].cpu_time--;
	}

    cpu = task_states[current_task];

    return cpu;
}

int mt_install()
{
	int i;
	for(i=0; i <= MAX_TASKS-1; i++) //Alle Task-Slots als unbenutzt definieren
	{
		tasks[i].state = TASK_STATE_NULL;
	}
	
}

int taskmgr_add_task( void* tfunct, const char *bezeichnung, int prio)
{
	int i;
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_NULL)
		{
			tasks[i].pid = i;
			strcpy(tasks[i].name, bezeichnung);
			tasks[i].priority = prio;
			tasks[i].state = TASK_STATE_SUSPENDED;
			
			task_states[i] = init_task(task_stacks[i], tfunct);
			
			//printf("Task '%s' gestartet mit PID %i\n", bezeichnung, i);
			return i;
		}
	}
	
	//Abgebrochene Tasks nur überschreiben, wenn keine freien Tasks-Slots mehr verfügbar sind
	for(i=0; i <= MAX_TASKS-1; i++)
	{
		if(tasks[i].state == TASK_STATE_ABORTED)
		{
			tasks[i].pid = i;
			strcpy(tasks[i].name, bezeichnung);
			tasks[i].priority = prio;
			tasks[i].state = TASK_STATE_SUSPENDED;
			
			task_states[i] = init_task(task_stacks[i], tfunct);
			
			//printf("Task '%s' gestartet mit PID %i\n", bezeichnung, i);
			return i;
		}
	}
	
	printf("ERR: Task could not be started, no available pid left!\n");
	
	return -1;
}

int taskmgr_list_tasks()
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
		printf("ERR: Task %i could not be found!\n");
	} else {
		tasks[pid].state = TASK_STATE_ABORTED;
		tasks[pid].error_no = error_number;
		strcpy(tasks[pid].error, error_descr);
	}
}

void taskmgr_task_start(int pid)
{
	if(pid < 0 || pid > MAX_TASKS-1)
	{
		printf("ERR: Task %i could not be found!\n");
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
		printf("ERR: Task %i could not be found!\n");
	} else {
		tasks[pid].state = TASK_STATE_SUSPENDED;
	}
}

int taskmgr_killall()
{
	int i;
	for(i=1; i <= MAX_TASKS-1; i++)
	{
		tasks[i].state = TASK_STATE_ABORTED;
	}
}

