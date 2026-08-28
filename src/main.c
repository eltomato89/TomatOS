
#include <system.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <asm.h>
#include <math.h>
#include <mm.h>
#include <vmm.h>
#include <syscall.h>
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

/* Pages of kernel text that user_setup() opens for ring 3, counted from the
   page a ring 3 routine starts in. Each of them is well under 4 KiB long, so
   it fits in one page and can at worst straddle one boundary -- two pages
   cover it either way. */
#define USER_TEXT_PAGES  2

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
void help(void);
//void network_test(char *cmd);

static void mem_print_right(uint32_t value, int width);
static void mem_show_status(void);
static void mem_selftest(void);
static void mem_check(int ok);

static void page_print_hex(uint32_t value, int digits);
static void page_show_status(void);
static void page_selftest(void);
static void page_fault_demo(void);
static void page_check(int ok);

static void user_demo(void);
static void user_iso_task(void);
static int  user_setup(void);
static void user_run(void);
static void user_selftest(void);
static void user_isolation(void);
static void user_check(int ok);

/* Self-test counters, maintained by mem_check(). */
static int mem_tests_run = 0;
static int mem_tests_ok = 0;

/* The same for page_check(). */
static int page_tests_run = 0;
static int page_tests_ok = 0;

/* And for user_check(). */
static int user_tests_run = 0;
static int user_tests_ok = 0;

void main()
{
	char cmd[256];
	char word[100];

    printf("eltomato's TomatOS 0.31 [Version 0.31 Build 2011/27/09]\n");
    printf("(c) Copyright 2006-2011 Jens Köhler\n\n");

	taskmgr_task_start(taskmgr_add_task( update_infobar, "Statusbar Update Task", TASK_PRIORITY_HIGH ));
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
		else if(strcmp(word, "reboot") == 0) reboot();
		else if(strcmp(word, "help") == 0) help();
		else if(strcmp(word, "start") == 0) taskmgr_task_start(taskmgr_add_task( task, "Test Task", TASK_PRIORITY_LOW ));
		else if(strcmp(word, "exit") == 0) ; /* handled by the loop condition */
		/* Only an empty input silently brings up a new prompt. */
		else if(word[0] != EOS) printf("Unknown command: %s\n", word);

		//if(strcmp(word, "test") == 0) network_test(cmd);

	} while(strcmp(cmd, "exit") != 0);

	//taskmgr_killall();

	cls();
	printf("It is now safe to turn off your computer!");

	/* main() runs as a task and must not return. */
	for(;;);
}
/*
void network_test(char *cmd)
{
	char mac_address[7];
	char i;
	for (i = 0; i < 6; i++)
	{
		mac_address[i] = inportb(ioaddr + i); // ioaddr is the base address obtainable from the PCI device configuration space.
	}

	printf("MAC ADDRESS: %s", mac_address);
}
*/
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
   CPL 3, and the only kernel code it may execute is the couple of pages
   user_setup() opens for it. A real call to an out-of-line sys_call1() would
   jump out of those pages and fault. Inlining keeps user_demo() one
   contiguous, call free routine. */
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

/* The demo, and the only routine in this file that runs with CPL 3.
*
*  It may touch exactly two things: its own stack, which the task manager maps
*  one page of for every ring 3 task, and the page at USER_PAGE_VIRT. Every
*  other page in the system is mapped without PAGE_USER and faults on the
*  first access from here.
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
static void user_demo(void)
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
static void user_iso_task(void)
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

/* Prepares everything ring 3 needs, once. Returns 1 when user_demo() can be
*  entered, 0 otherwise.
*
*  Two mappings are involved, opened for quite different reasons:
*
*    - one fresh frame at USER_PAGE_VIRT, present, writable and PAGE_USER.
*      This is the demo's data, i.e. its strings and nothing else. It is
*      zeroed first: the pmm hands out frames with the previous owner's
*      contents still in them, and ring 3 is about to be able to read them.
*
*    - the kernel text pages holding user_demo(), re-mapped with PAGE_USER
*      added so ring 3 can fetch those instructions. They stay without
*      PAGE_WRITE, so this grants read and execute, not write.
*
*  That second mapping is the coarse move in the whole exercise and is worth
*  naming rather than hiding: a page is the smallest thing paging can talk
*  about, so whatever else the linker put next to user_demo() becomes readable
*  from ring 3 as well. A grown-up kernel links user code into a section of its
*  own and maps only that. What is deliberately *not* opened is .rodata -- the
*  strings stay in the user page precisely so this stays a text-only exception. */
/* Opens the pages holding one ring 3 routine for CPL 3. Called once per such
*  routine, because the linker is free to place them anywhere in .text -- two
*  functions next to each other in this file need not end up in the same page,
*  and overlapping calls simply re-write the same entry with the same flags.
*
*  Kernel text sits above KERNEL_VIRTUAL_BASE, i.e. in the quarter every
*  address space shares, so one vmm_map() here is visible to every task. */
static int user_open_text(uint32_t addr)
{
	uint32_t page;
	uint32_t phys;
	int i;

	page = addr & ~((uint32_t)PAGE_SIZE - 1);

	for(i = 0; i < USER_TEXT_PAGES; i++)
	{
		phys = vmm_get_phys(page);
		if(phys == 0 || vmm_map(page, phys, PAGE_PRESENT | PAGE_USER) != 0)
		{
			printf("user: kernel text at ");
			page_print_hex(page, 8);
			printf(" could not be opened for ring 3.\n");
			return 0;
		}
		page += PAGE_SIZE;
	}

	return 1;
}

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

	if(!user_open_text((uint32_t)user_demo)) return 0;
	if(!user_open_text((uint32_t)user_iso_task)) return 0;

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
	page_print_hex((uint32_t)user_demo, 8);
	printf("  kernel text, opened for ring 3\n");
	printf("  Strings: ");
	page_print_hex((uint32_t)USER_PAGE_VIRT, 8);
	printf("  user page, writable here, read only in the task\n");

	before = syscall_count();

	pid = taskmgr_add_user_task(user_demo, "Ring 3 Demo", TASK_PRIORITY_NORMAL);
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

	pid_a = taskmgr_add_user_task(user_iso_task, "Ring 3 Isolation A",
	                              TASK_PRIORITY_NORMAL);
	pid_b = taskmgr_add_user_task(user_iso_task, "Ring 3 Isolation B",
	                              TASK_PRIORITY_NORMAL);
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
	printf("  design, and both tasks are kernel routines opened for ring 3\n");
	printf("  rather than programs loaded from anywhere.\n");
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

void help(void)
{
	printf("TomatOS Help\n");
	printf("Available commands:\n");
	printf("\thelp      Show this overview\n");
	printf("\ttaskmgr   List and control tasks\n");
	printf("\tstart     Start a test task\n");
	printf("\tmem       Show memory usage, mem -t tests the heap\n");
	printf("\tpage      Show paging state, page -t tests paging\n");
	printf("\tuser      Ring 3 demo, user -t tests the system calls,\n");
	printf("\t          user -i tests the address space isolation\n");
	printf("\treboot    Restart the computer\n");
	printf("\texit      Exit the shell\n");
}
