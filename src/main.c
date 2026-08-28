
#include <system.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <asm.h>
#include <math.h>
#include <mm.h>
#include <vmm.h>
//#include <wmessages.h>

#define NULL 0
#define BCD2BIN(val) (((val) & 0x0F) + ((val) >> 4) * 10)

/* Size of the blocks that "mem -t" uses to probe the heap. */
#define MEM_TEST_SIZE   64
#define MEM_TEST_SMALL  32
#define MEM_TEST_LARGE  128

/* Text mode video memory -- identity mapped like everything else, and the
   one address in the system whose physical location is common knowledge. */
#define PAGE_VGA_TEXT   0xB8000

/* Offset inside a page for the test that vmm_get_phys() does not just
   translate the frame but keeps the offset. Deliberately not aligned. */
#define PAGE_TEST_OFFSET 0x123

/* Where "page -t" starts looking for a free virtual address for its
   map/unmap test: far above any physical RAM this kernel will see, and far
   below the top 4 MiB, which a recursive directory mapping would occupy.
   The address is not used as is -- it is only the starting point of a probe
   with vmm_is_mapped(), see page_find_free_virt(). */
#define PAGE_TEST_VIRT   0xE0000000
#define PAGE_TEST_PROBES 1024

/* Pattern written through the freshly created mapping. */
#define PAGE_TEST_PATTERN 0xC0FFEE01

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

/* Self-test counters, maintained by mem_check(). */
static int mem_tests_run = 0;
static int mem_tests_ok = 0;

/* The same for page_check(). */
static int page_tests_run = 0;
static int page_tests_ok = 0;

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
   carries its own padding so the columns line up. */
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

	/* A few translations that make the identity mapping tangible. The heap
	   block is only borrowed for the duration of the printout. */
	block = (unsigned char *)malloc(MEM_TEST_SIZE);

	printf("  Translations:\n");
	page_print_translation("    Kernel code  ", (uint32_t)main);
	page_print_translation("    VGA text     ", (uint32_t)PAGE_VGA_TEXT);
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

/* Identity mapping for one address: the physical address must be the
   virtual one. */
static void page_check_identity(char *label, uint32_t virt)
{
	uint32_t phys;

	phys = vmm_get_phys(virt);
	page_check(virt != 0 && phys == virt);
	printf("%s", label);
	page_print_hex(virt, 8);
	printf(" -> ");
	page_print_hex(phys, 8);
	printf(" identity\n");
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
	uint32_t *frame;
	volatile uint32_t *window;
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

	/* 1.-3. The identity mapping must hold everywhere, not just where the
	         kernel happens to live -- code, heap and the VGA buffer sit in
	         three different regions. */
	block = (unsigned char *)malloc(MEM_TEST_SIZE);
	heap_addr = (uint32_t)block;

	page_check_identity("Kernel   ", (uint32_t)main);
	page_check_identity("VGA text ", (uint32_t)PAGE_VGA_TEXT);
	page_check_identity("Heap     ", heap_addr);

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
	      vmm_map() rather than just inspecting what vmm_init() built. */
	frame = (uint32_t *)pmm_alloc_frame();
	virt = page_find_free_virt();
	mapped = 0;
	ok = 0;

	if(frame != 0 && virt != 0 &&
	   vmm_map(virt, (uint32_t)frame, PAGE_PRESENT | PAGE_WRITE) == 0)
	{
		mapped = 1;
		window = (volatile uint32_t *)virt;
		window[0] = (uint32_t)PAGE_TEST_PATTERN;
		window[1] = ~(uint32_t)PAGE_TEST_PATTERN;
		window[PAGE_SIZE / 4 - 1] = (uint32_t)PAGE_TEST_PATTERN;

		ok = (window[0] == (uint32_t)PAGE_TEST_PATTERN &&
		      window[1] == ~(uint32_t)PAGE_TEST_PATTERN &&
		      window[PAGE_SIZE / 4 - 1] == (uint32_t)PAGE_TEST_PATTERN &&
		      vmm_get_phys(virt) == (uint32_t)frame &&
		      vmm_is_mapped(virt));
	}
	page_check(ok);
	printf("Mapped ");
	page_print_hex(virt, 8);
	printf(" -> ");
	page_print_hex((uint32_t)frame, 8);
	printf(", pattern read back\n");

	/* 8. The new mapping must point at the very same physical page, so the
	      frame's own identity address has to show the pattern too. */
	ok = 0;
	seen = 0;
	if(mapped && vmm_is_mapped((uint32_t)frame))
	{
		seen = frame[0];
		ok = (seen == (uint32_t)PAGE_TEST_PATTERN);
	}
	page_check(ok);
	printf("Frame ");
	page_print_hex((uint32_t)frame, 8);
	printf(" via identity reads ");
	page_print_hex(seen, 8);
	printf("\n");

	/* 9. After vmm_unmap() the address must be gone for good. */
	if(mapped) vmm_unmap(virt);
	page_check(mapped && !vmm_is_mapped(virt) && vmm_get_phys(virt) == 0);
	printf("Unmapped ");
	page_print_hex(virt, 8);
	printf(": is_mapped = %i\n", vmm_is_mapped(virt));

	pmm_free_frame(frame);

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

void help(void)
{
	printf("TomatOS Help\n");
	printf("Available commands:\n");
	printf("\thelp      Show this overview\n");
	printf("\ttaskmgr   List and control tasks\n");
	printf("\tstart     Start a test task\n");
	printf("\tmem       Show memory usage, mem -t tests the heap\n");
	printf("\tpage      Show paging state, page -t tests paging\n");
	printf("\treboot    Restart the computer\n");
	printf("\texit      Exit the shell\n");
}
