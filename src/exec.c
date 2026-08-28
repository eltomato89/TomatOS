/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Turns a bootloader module into a running ring 3 task.
*
*  Two halves, and they meet only through the module table in the middle:
*
*    - exec_init() records what GRUB loaded next to the kernel. Everything in
*      the multiboot module list is PHYSICAL and comes from outside the
*      kernel, so every address is range checked against DIRECT_MAP_LIMIT
*      before a pointer is built from it with P2V(). The same discipline the
*      pmm applies to the memory map applies here: a bootloader that hands us
*      nonsense must produce "no modules", never a fault.
*
*    - exec_spawn() loads one of them. It parses the ELF32 headers, allocates
*      a frame for every page the PT_LOAD segments cover, copies the file
*      contents in, leaves the rest zero (that is .bss) and maps the pages
*      into the address space of a task that was created suspended for exactly
*      this purpose. Only when the whole image is in place does the task learn
*      its entry point; the caller then starts it.
*
*  Two details that decide whether a program runs or dies mysteriously:
*
*    - segments are not page aligned. The tail of one and the head of the next
*      routinely share a page, so a frame is allocated only for a page that is
*      not mapped yet, and the copy always lands at the byte offset the
*      segment actually asks for. Allocating per segment instead of per page
*      would either lose the tail or hand out two frames for one page.
*
*    - a fresh frame is zeroed before anything is copied into it. The pmm
*      hands out dirty memory, and everything mapped here is readable from
*      ring 3 - an unzeroed frame would show the previous owner's data to a
*      user program. Zeroing also makes .bss correct for free: the bytes
*      between p_filesz and p_memsz are simply the ones nothing is copied over.
*
*  What is deliberately NOT supported: anything dynamic. A PT_INTERP segment
*  means the program wants a dynamic linker, and there is none, so it is
*  rejected with a readable reason instead of being loaded into a program
*  counter that faults at the first PLT entry.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <mm.h>
#include <vmm.h>
#include <multiboot.h>
#include <elf.h>
#include <exec.h>

/* Indices into e_ident that the loader checks. The remaining bytes (version,
*  OS ABI, padding) say nothing a static i386 executable would depend on. */
#define EI_CLASS  4
#define EI_DATA   5

/* Address part of a page aligned address; the low 12 bits are the offset. */
#define PAGE_ADDR_MASK  0xFFFFF000UL

/* Longest module name kept, including the terminator. Names come from the
*  bootloader command line and are only ever compared and printed. */
#define EXEC_NAME_MAX   32

/* Same size as task_settings.error, so an error can be handed to
*  taskmgr_task_abort() without being truncated there. */
#define EXEC_ERROR_MAX  128

/* How far into a module command line the loader is willing to read before it
*  gives up on finding a terminator. Protects against a string that the
*  bootloader left unterminated. */
#define EXEC_CMDLINE_MAX 256

/* error_no a task aborted by the loader carries. The text says the rest. */
#define EXEC_ABORT_ERRNO 1

/* A module as the kernel keeps it: the bootloader's physical range, boiled
*  down to start and size, plus the name taken from its command line. */
typedef struct
{
	uint32_t start;			/* PHYSICAL address of the first byte */
	uint32_t size;
	char     name[EXEC_NAME_MAX];
} exec_module;

static exec_module modules[EXEC_MAX_MODULES];
static int         module_count = 0;

static char last_error[EXEC_ERROR_MAX] = "";

/* --- Small helpers -------------------------------------------------------- */

/* Bounded copy, always terminated. There is no strncpy() in the kernel, and
*  every string here comes either from the bootloader or from a literal. */
static void copy_bounded(char *dest, const char *src, int max)
{
	int i;

	if (max <= 0) return;

	for (i = 0; i < max - 1 && src[i] != '\0'; i++)
		dest[i] = src[i];

	dest[i] = '\0';
}

static void exec_set_error(const char *msg)
{
	copy_bounded(last_error, msg, EXEC_ERROR_MAX);
}

/* Non-zero if [phys, phys + len) lies inside the window P2V() describes.
*  Written without an addition that could wrap: the length is compared
*  against the room left above phys, not against phys + len. */
static int phys_range_ok(uint32_t phys, uint32_t len)
{
	if (phys > DIRECT_MAP_LIMIT) return 0;
	if (len == 0) return 1;
	return len <= DIRECT_MAP_LIMIT - phys + 1;
}

/* --- The module list ------------------------------------------------------ */

/* First word of a module command line, which is what the kernel uses as the
*  module's name. The command line address is physical and may be absent,
*  outside the direct mapping or not terminated at all - all three end up as
*  "?" rather than as a fault. */
static void module_name_from_cmdline(uint32_t phys, char *dest)
{
	const char *cmd;
	uint32_t    room;
	int         limit;
	int         start;
	int         i;

	copy_bounded(dest, "?", EXEC_NAME_MAX);

	if (phys == 0 || phys > DIRECT_MAP_LIMIT) return;

	/* Never read past the end of the direct mapping, however long the
	*  string claims to be. */
	room  = DIRECT_MAP_LIMIT - phys + 1;
	limit = EXEC_CMDLINE_MAX;
	if (room < (uint32_t)limit) limit = (int)room;

	cmd = (const char *)P2V(phys);

	start = 0;
	while (start < limit && (cmd[start] == ' ' || cmd[start] == '\t'))
		start++;

	if (start >= limit || cmd[start] == '\0') return;	/* only blanks */

	for (i = 0; start + i < limit && i < EXEC_NAME_MAX - 1; i++)
	{
		if (cmd[start + i] == '\0' || cmd[start + i] == ' ' ||
		    cmd[start + i] == '\t')
			break;

		dest[i] = cmd[start + i];
	}

	dest[i] = '\0';
}

void exec_init(multiboot_info *mbi)
{
	multiboot_module *mods;
	uint32_t          count;
	uint32_t          bytes;
	uint32_t          start;
	uint32_t          end;
	uint32_t          i;

	module_count = 0;
	exec_set_error("");

	if (mbi == 0) return;
	if ((mbi->flags & MULTIBOOT_INFO_MODS) == 0) return;
	if (mbi->mods_count == 0 || mbi->mods_addr == 0) return;

	count = mbi->mods_count;
	if (count > EXEC_MAX_MODULES)
	{
		printf("EXEC: %d modules loaded, only the first %d are used\n",
		       (int)count, (int)EXEC_MAX_MODULES);
		count = EXEC_MAX_MODULES;
	}

	/* The array itself has to be readable before a single field of it is
	*  looked at. count is already clamped, so the multiplication cannot
	*  overflow. */
	bytes = count * (uint32_t)sizeof(multiboot_module);
	if (!phys_range_ok(mbi->mods_addr, bytes))
	{
		printf("EXEC: module list at 0x%X is outside the direct mapping\n",
		       (int)mbi->mods_addr);
		return;
	}

	mods = (multiboot_module *)P2V(mbi->mods_addr);

	for (i = 0; i < count; i++)
	{
		start = mods[i].mod_start;
		end   = mods[i].mod_end;

		/* mod_end is the address behind the last byte. Anything else -
		*  an empty or a reversed range - is a broken entry. */
		if (end <= start)
		{
			printf("EXEC: module %d has an empty range, ignored\n", (int)i);
			continue;
		}

		if (!phys_range_ok(start, end - start))
		{
			printf("EXEC: module %d at 0x%X is outside the direct mapping\n",
			       (int)i, (int)start);
			continue;
		}

		modules[module_count].start = start;
		modules[module_count].size  = end - start;
		module_name_from_cmdline(mods[i].cmdline,
		                         modules[module_count].name);
		module_count++;
	}
}

int exec_module_count(void)
{
	return module_count;
}

const char *exec_module_name(int index)
{
	if (index < 0 || index >= module_count) return "?";
	return modules[index].name;
}

uint32_t exec_module_size(int index)
{
	if (index < 0 || index >= module_count) return 0;
	return modules[index].size;
}

int exec_module_find(const char *name)
{
	int i;

	if (name == 0) return -1;

	for (i = 0; i < module_count; i++)
	{
		if (strcmp(modules[i].name, name) == 0) return i;
	}

	return -1;
}

const char *exec_last_error(void)
{
	return last_error;
}

/* --- ELF32 loading -------------------------------------------------------- */

/* Program header number i. The caller has already established that the whole
*  table lies inside the module. */
static elf32_phdr *phdr_at(uint32_t image, elf32_ehdr *eh, int i)
{
	return (elf32_phdr *)(image + eh->e_phoff +
	                      (uint32_t)i * (uint32_t)eh->e_phentsize);
}

/* Everything about the file header that has to hold before any field of it is
*  believed. Returns 0 when the module is a static 32-bit i386 executable
*  whose program header table lies completely inside the module. */
static int check_header(uint32_t image, uint32_t size)
{
	elf32_ehdr *eh;
	uint32_t    table;

	if (size < (uint32_t)sizeof(elf32_ehdr))
	{
		exec_set_error("module is too small to hold an ELF header");
		return -1;
	}

	eh = (elf32_ehdr *)image;

	if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
	    eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3)
	{
		exec_set_error("not an ELF file (bad magic)");
		return -1;
	}

	if (eh->e_ident[EI_CLASS] != ELFCLASS32)
	{
		exec_set_error("not a 32 bit ELF file");
		return -1;
	}

	if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
	{
		exec_set_error("not a little endian ELF file");
		return -1;
	}

	if (eh->e_type != ET_EXEC)
	{
		exec_set_error("not a static executable, ET_EXEC expected");
		return -1;
	}

	if (eh->e_machine != EM_386)
	{
		exec_set_error("not built for i386");
		return -1;
	}

	/* The table is walked with a stride of e_phentsize, so a size that does
	*  not match the structure would read every entry at the wrong offset. */
	if (eh->e_phentsize != (uint16_t)sizeof(elf32_phdr))
	{
		exec_set_error("unexpected program header size");
		return -1;
	}

	if (eh->e_phnum == 0)
	{
		exec_set_error("no program headers, nothing to load");
		return -1;
	}

	/* e_phnum is 16 bit and e_phentsize is fixed by the test above, so the
	*  product is far below 4 GiB and the comparison needs no wrap check. */
	table = (uint32_t)eh->e_phnum * (uint32_t)eh->e_phentsize;
	if (eh->e_phoff >= size || table > size - eh->e_phoff)
	{
		exec_set_error("program header table lies outside the module");
		return -1;
	}

	return 0;
}

/* Non-zero if the page belongs to the memory image of a PT_LOAD segment that
*  was loaded before segment number "upto".
*
*  This is what tells the two reasons for an already mapped page apart. A page
*  shared with an earlier segment of the same program is expected and is
*  filled in, whereas a page that is mapped although no earlier segment claims
*  it belongs to something else in this address space - the task's user stack,
*  for instance - and the program does not get to overwrite it. */
static int page_of_earlier_segment(uint32_t image, elf32_ehdr *eh, int upto,
                                   uint32_t page)
{
	elf32_phdr *ph;
	uint32_t    first;
	uint32_t    last;
	int         i;

	for (i = 0; i < upto; i++)
	{
		ph = phdr_at(image, eh, i);

		if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

		/* Earlier segments are already validated, so p_vaddr + p_memsz
		*  is known not to wrap here. */
		first = ph->p_vaddr & PAGE_ADDR_MASK;
		last  = (ph->p_vaddr + ph->p_memsz - 1) & PAGE_ADDR_MASK;

		if (page >= first && page <= last) return 1;
	}

	return 0;
}

/* Loads one PT_LOAD segment into the given address space, one page at a time.
*  Returns 0 on success, -1 with last_error set otherwise. */
static int load_segment(addrspace_t space, uint32_t image, uint32_t size,
                        elf32_ehdr *eh, int index)
{
	elf32_phdr *ph;
	uint32_t    flags;
	uint32_t    file_end;	/* first byte of the segment that is .bss  */
	uint32_t    page;
	uint32_t    last;
	uint32_t    phys;
	uint32_t    from;	/* first byte of this page taken from file */
	uint32_t    to;		/* one past the last such byte             */
	void       *frame;

	ph = phdr_at(image, eh, index);

	if (ph->p_memsz == 0) return 0;		/* nothing to place */

	if (ph->p_filesz > ph->p_memsz)
	{
		exec_set_error("segment claims more file bytes than memory bytes");
		return -1;
	}

	/* What the segment reads out of the module has to be inside it. */
	if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
	{
		exec_set_error("segment contents lie outside the module");
		return -1;
	}

	/* Page zero stays unmapped in every address space, so that a null
	*  pointer faults instead of reading something. A program does not get
	*  to undo that by asking to be loaded there. */
	if (ph->p_vaddr < PAGE_SIZE)
	{
		exec_set_error("segment would map the null page");
		return -1;
	}

	/* And the far end: no program may ask to be loaded over the kernel.
	*  Written as a subtraction so that a p_memsz which would wrap the
	*  address space is caught by the same test. */
	if (ph->p_vaddr >= KERNEL_VIRTUAL_BASE ||
	    ph->p_memsz > KERNEL_VIRTUAL_BASE - ph->p_vaddr)
	{
		exec_set_error("segment reaches into the kernel half");
		return -1;
	}

	flags = PAGE_PRESENT | PAGE_USER;
	if (ph->p_flags & PF_W) flags |= PAGE_WRITE;

	file_end = ph->p_vaddr + ph->p_filesz;

	page = ph->p_vaddr & PAGE_ADDR_MASK;
	last = (ph->p_vaddr + ph->p_memsz - 1) & PAGE_ADDR_MASK;

	for (;;)
	{
		phys = vmm_get_phys_in(space, page);

		if (phys == 0)
		{
			frame = pmm_alloc_frame();
			if (frame == 0)
			{
				exec_set_error("out of physical memory while loading a segment");
				return -1;
			}

			/* The frame is filled in through the direct mapping,
			*  because the space being built is not the active one
			*  and the user address does not resolve here. */
			if ((uint32_t)frame > DIRECT_MAP_LIMIT)
			{
				pmm_free_frame(frame);
				exec_set_error("frame outside the direct mapping");
				return -1;
			}

			/* Before the mapping, not after: from the moment the
			*  page is in the table it is ring 3 readable, and what
			*  the previous owner left in the frame is none of the
			*  program's business. This also is what makes .bss
			*  zero - nothing is copied over those bytes below. */
			memset(P2V(frame), (char)0, (size_t)PAGE_SIZE);

			if (vmm_map_in(space, page, (uint32_t)frame, flags) != 0)
			{
				pmm_free_frame(frame);
				exec_set_error("no memory for a page table");
				return -1;
			}

			phys = (uint32_t)frame;
		}
		else
		{
			phys &= PAGE_ADDR_MASK;

			if (!page_of_earlier_segment(image, eh, index, page))
			{
				exec_set_error("segment overlaps something already mapped");
				return -1;
			}

			/* A page shared with an earlier segment carries that
			*  segment's rights. If this one needs to write, the
			*  page has to allow it, so the entry is rewritten with
			*  the same frame and the wider flags. The alternative
			*  would be a data segment that faults on its first
			*  store because its first bytes live in the last page
			*  of .text. */
			if ((flags & PAGE_WRITE) &&
			    vmm_map_in(space, page, phys, flags) != 0)
			{
				exec_set_error("could not widen the rights of a shared page");
				return -1;
			}
		}

		/* The part of this page the file has something to say about.
		*  Both ends are clamped, so the first and the last page of a
		*  segment are handled by the same two lines - and a page that
		*  is entirely .bss simply ends up with to <= from. */
		from = page;
		if (from < ph->p_vaddr) from = ph->p_vaddr;

		to = page + PAGE_SIZE;
		if (to > file_end) to = file_end;

		if (to > from)
		{
			memcpy((char *)P2V(phys) + (from - page),
			       (const char *)(image + ph->p_offset +
			                      (from - ph->p_vaddr)),
			       (size_t)(to - from));
		}

		if (page == last) break;
		page += PAGE_SIZE;
	}

	return 0;
}

/* Parses the module and places every PT_LOAD segment into the space. On
*  success *entry_out holds the address the task is to start at. */
static int load_image(addrspace_t space, uint32_t image, uint32_t size,
                      uint32_t *entry_out)
{
	elf32_ehdr *eh;
	elf32_phdr *ph;
	uint32_t    entry;
	int         loaded;
	int         found;
	int         i;

	if (check_header(image, size) != 0) return -1;

	eh = (elf32_ehdr *)image;

	/* A dynamic executable is rejected before a single page is mapped.
	*  There is no dynamic linker to hand the program to, and loading it
	*  anyway would only move the failure to the first call through a
	*  relocation, where nothing points back to this cause. */
	for (i = 0; i < (int)eh->e_phnum; i++)
	{
		ph = phdr_at(image, eh, i);

		if (ph->p_type == PT_INTERP)
		{
			exec_set_error("needs a dynamic linker, link it statically");
			return -1;
		}
	}

	loaded = 0;

	for (i = 0; i < (int)eh->e_phnum; i++)
	{
		ph = phdr_at(image, eh, i);

		if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

		if (load_segment(space, image, size, eh, i) != 0) return -1;

		loaded++;
	}

	if (loaded == 0)
	{
		exec_set_error("no loadable segment in the module");
		return -1;
	}

	/* The entry point has to be a place the program actually has: inside
	*  one of the segments just mapped. Anywhere else is either a corrupt
	*  header or an attempt to start execution somewhere unmapped, and both
	*  are better reported here than as a page fault in ring 3. */
	entry = eh->e_entry;
	found = 0;

	for (i = 0; i < (int)eh->e_phnum && found == 0; i++)
	{
		ph = phdr_at(image, eh, i);

		if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

		if (entry >= ph->p_vaddr && entry - ph->p_vaddr < ph->p_memsz)
			found = 1;
	}

	if (found == 0)
	{
		exec_set_error("entry point lies outside every loaded segment");
		return -1;
	}

	*entry_out = entry;
	return 0;
}

/* --- Spawning ------------------------------------------------------------- */

int exec_spawn(int index, int prio)
{
	addrspace_t space;
	uint32_t    image;
	uint32_t    entry;
	int         pid;

	exec_set_error("");

	if (index < 0 || index >= module_count)
	{
		exec_set_error("no module with that index");
		return -1;
	}

	/* exec_init() has already established that the whole module lies inside
	*  the direct mapping, so this pointer is good for size bytes. */
	image = (uint32_t)P2V(modules[index].start);

	/* The task comes first and comes suspended: it owns the address space
	*  the segments are mapped into, and it must not be scheduled while its
	*  image is still half in place. The entry point is still unknown here,
	*  which is exactly why it is passed as 0 and set further down. */
	pid = taskmgr_add_user_task(0, modules[index].name, prio);
	if (pid < 0)
	{
		exec_set_error("no free task slot or no memory for the address space");
		return -1;
	}

	space = taskmgr_task_space(pid);
	if (space == 0)
	{
		exec_set_error("the new task has no address space of its own");
		taskmgr_task_abort(pid, EXEC_ABORT_ERRNO, last_error);
		return -1;
	}

	entry = 0;

	/* Every failure from here on leaves a half built task behind, so it is
	*  aborted with the reason attached. The frames and page tables already
	*  mapped belong to its address space and go back to the pmm with it -
	*  there is deliberately no second list of them here that could free
	*  them a second time. */
	if (load_image(space, image, modules[index].size, &entry) != 0)
	{
		taskmgr_task_abort(pid, EXEC_ABORT_ERRNO, last_error);
		return -1;
	}

	/* Last step, and the one that turns the loaded image into something that
	*  can run. It refuses a task that is not suspended any more, which would
	*  mean somebody started this one behind our back. */
	if (taskmgr_task_set_entry(pid, entry) != 0)
	{
		exec_set_error("the entry point was not accepted by the task manager");
		taskmgr_task_abort(pid, EXEC_ABORT_ERRNO, last_error);
		return -1;
	}

	/* Suspended on purpose. The caller starts it with taskmgr_task_start()
	*  once it is done looking at it. */
	return pid;
}
