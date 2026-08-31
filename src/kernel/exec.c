/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Turns a bootloader module or a file on disk into a running ring 3 task.
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
*  Since there is a filesystem, a program no longer has to be baked into the
*  boot media: exec_spawn_path() loads one from a file on the mounted volume.
*  The loader itself is not doubled for that. Everything above - the header
*  checks, the per page frame allocation, the .bss gap, PAGE_USER and the
*  PF_W dependent PAGE_WRITE - is one code path that asks an "image" for
*  bytes instead of dereferencing a pointer. The two sources answer the same
*  two questions and differ in nothing else:
*
*    - a module is already in RAM, inside the direct mapping, so a read is a
*      memcpy() from base + offset and costs nothing extra;
*    - a file is read with fat_read() at that same offset, one page at a
*      time, straight into the frame that page will be mapped from.
*
*  The alternative - read the whole file into a heap buffer and run the
*  existing module loader over it - would have been shorter still, but it
*  doubles the memory a program needs at the worst possible moment (the heap
*  copy is alive while the frames for the image are being allocated) and it
*  puts a hard ceiling on the program size that has nothing to do with how
*  much memory the program actually needs. A 300 KiB binary would then fail
*  on a heap that has 256 KiB left, although its segments would fit in RAM
*  perfectly well. Reading page by page has no such ceiling: only the ELF
*  header plus the program header table are held in the heap, a few hundred
*  bytes, and a file too large for the machine fails exactly where a module
*  of the same size would - in pmm_alloc_frame(), with "out of physical
*  memory while loading a segment".
*
*  The price is one fat_read() per page instead of one memcpy() per segment.
*  For a loader that runs once per program start this is the cheaper of the
*  two prices.
*
*  A program also has to be told what it was started with, and that argument
*  vector is built on the USER STACK, in the page the task manager already
*  gave the task. Nowhere else: a per task argument buffer in the kernel
*  would be MAX_TASKS times its size in .bss, permanently, for something a
*  program reads once - in a kernel that is deliberately moving things out,
*  not in. The stack page exists anyway, it is private to the task, it is
*  handed back with the address space, and the program is free to overwrite
*  the strings the moment it has read them.
*
*  The layout at the entry point is the one a plain
*
*      void _start(int argc, char **argv)
*
*  compiled cdecl expects to find, i.e. exactly what a "call" would have left
*  behind:
*
*      higher addresses
*        "hello\0" "readme.txt\0" ...   the strings, argv[0] first
*        0                              argv[argc], the terminator
*        &"readme.txt"                  argv[1]
*        &"hello"                       argv[0]
*        argv                           a pointer to the argv[0] slot above
*        argc
*        0                              a fake return address, never used
*      esp -> lower addresses
*
*  The two things that make or break it:
*
*    - every pointer stored in that block is a USER address. The block is
*      written through the direct mapping, because the space being furnished
*      is not the active one, so the address the kernel writes at and the
*      address the program will read at are different views of the same
*      frame. Everything is therefore computed in user addresses and turned
*      into a kernel one only at the moment of the store.
*    - the block lives in the task's single 4 KiB stack page, so it competes
*      with the stack the program is about to use. It is refused unless a
*      fixed reserve is left below it - better a program that does not start
*      than one that starts and overruns its stack into the guard page in its
*      first function call.
*
*  What is left is the esp the task starts with. taskmgr_add_user_task() points
*  it at the very top of the stack page, which is right for a program that gets
*  nothing and wrong for one that gets arguments: the block has to sit ABOVE
*  esp for the offsets to line up. taskmgr_task_set_stack() moves it down to
*  the base of the block, and is set BEFORE the entry point - a task whose
*  entry point is known is complete as far as taskmgr_task_start() is
*  concerned, so the other order would leave a window in which the program
*  could start on top of its own arguments.
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
#include <fat.h>

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

/* Longest path to a program file that can be remembered in the table below,
*  including the terminator. exec_spawn_path() itself has no such limit - it
*  reads through the caller's string. */
#define EXEC_PATH_MAX   64

/* How many files exec_module_find() keeps resolved at a time, and where
*  their indices start. The gap to EXEC_MAX_MODULES is deliberate: an index
*  is either a module or a file, never both, and a stale one is rejected
*  rather than silently landing on the wrong program. */
#define EXEC_MAX_FILES  4
#define EXEC_FILE_INDEX_BASE 1000

/* Everything in front of the last program header, i.e. e_phoff plus the
*  table, is what a file image keeps in the heap. Real linkers put the table
*  right behind the file header, so this bound is never anywhere near - it
*  only stops a corrupt e_phoff from asking for a megabyte of heap. */
#define EXEC_HEADER_MAX 16384

/* Most arguments a program can be started with, argv[0] included. The array
*  of words below it lives on the KERNEL stack, which is 4 KiB per task and
*  shared with everything a system call does, so the bound is what keeps a
*  caller's argument string from deciding how much of it is used. Sixteen is
*  well past anything this shell can produce. */
#define EXEC_ARGV_MAX   16

/* Bytes of the stack page that stay free for the program itself. The
*  argument block is refused rather than squeezed in below this: a program
*  whose arguments leave it 200 bytes of stack would fault in its first
*  function call, and a page fault at some address inside the guard page says
*  a great deal less than "the arguments do not fit". */
#define EXEC_STACK_MIN_FREE  2048

/* How far down from the kernel half stack_page_of() looks for the task's
*  user stack. The task manager puts the stacks of all MAX_TASKS slots
*  directly below KERNEL_VIRTUAL_BASE, 8 KiB apart, which is 512 KiB for 64
*  slots; 4 MiB of search is that with room to spare and costs one page
*  directory lookup per empty page. */
#define EXEC_STACK_SEARCH_PAGES 1024

/* A module as the kernel keeps it: the bootloader's physical range, boiled
*  down to start and size, plus the name taken from its command line. */
typedef struct
{
	uint32_t start;			/* PHYSICAL address of the first byte */
	uint32_t size;
	char     name[EXEC_NAME_MAX];
} exec_module;

/* A file the name resolution below has found on the mounted volume. Only the
*  path is authoritative; size and name are what the shell prints. */
typedef struct
{
	char     path[EXEC_PATH_MAX];
	char     name[EXEC_NAME_MAX];
	uint32_t size;
	int      used;
} exec_file;

/* Where the bytes of a program image come from. The loader below never
*  touches a module or a file directly, it asks this:
*
*    base  is byte 0 of the image as a virtual address - the module itself
*          for a module, the heap copy of the ELF and program headers for a
*          file. Only the first "hdr" bytes of the image may be reached
*          through it, which for a module happens to be all of them.
*    path  is the file the rest is read from, or 0 when base covers
*          everything.
*    noun  is what an error message calls the thing, "module" or "file". */
typedef struct
{
	uint32_t    size;		/* total size of the image in bytes  */
	uint32_t    base;		/* virtual address of image byte 0   */
	uint32_t    hdr;		/* bytes base really covers          */
	const char *path;		/* file path, 0 for a module         */
	const char *noun;
} exec_image;

/* One argument, still where it was found: a slice of the caller's string
*  rather than a copy of it. The words are only needed between the split and
*  the single pass that writes them onto the user stack, and both happen
*  before this function returns, so there is nothing to own and nothing to
*  free. */
typedef struct
{
	const char *text;
	uint32_t    len;		/* without a terminator, there is none yet */
} exec_word;

static exec_module modules[EXEC_MAX_MODULES];
static int         module_count = 0;

static exec_file   files[EXEC_MAX_FILES];
static int         file_next = 0;

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

/* Two part message. Exists for the handful of errors that have to name the
*  source - "module is too small ...", "... outside the file" - so that one
*  loader can produce both wordings without a printf() into a buffer. */
static void exec_set_error_2(const char *first, const char *second)
{
	int n;
	int i;

	copy_bounded(last_error, first, EXEC_ERROR_MAX);

	n = (int)strlen(last_error);
	for (i = 0; n + i < EXEC_ERROR_MAX - 1 && second[i] != '\0'; i++)
		last_error[n + i] = second[i];

	last_error[n + i] = '\0';
}

/* Last component of a path, which is what a task started from a file is
*  called. A path that ends in a separator has no name of its own and keeps
*  the whole string - better a strange task name than an empty one. */
static void name_from_path(const char *path, char *dest, int max)
{
	int last;
	int i;

	last = -1;

	for (i = 0; path[i] != '\0'; i++)
	{
		if (path[i] == '/' || path[i] == '\\') last = i;
	}

	if (last < 0 || path[last + 1] == '\0')
		copy_bounded(dest, path, max);
	else
		copy_bounded(dest, path + last + 1, max);
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

/* --- Files on the mounted volume ------------------------------------------ */

/* The module list is what the bootloader handed over and never changes. A
*  file is not part of it - it is looked up on demand and remembered just
*  long enough for the caller to spawn it, in a table of its own with indices
*  of its own. That is what lets exec_spawn() take a file without a second
*  interface: an index either names a module or it names one of these. */

/* The file behind an index, or 0 if the index does not name a live one. */
static exec_file *file_at(int index)
{
	int i;

	if (index < EXEC_FILE_INDEX_BASE) return 0;

	i = index - EXEC_FILE_INDEX_BASE;
	if (i >= EXEC_MAX_FILES) return 0;
	if (!files[i].used) return 0;

	return &files[i];
}

/* Puts a path into the table and returns its index, or -1 if the path is too
*  long to be kept whole. Truncating one would be worse than refusing it: the
*  shortened path could well name a different file that exists.
*
*  A path already in the table keeps its slot, so asking for the same program
*  twice does not use two of them. Otherwise the oldest entry is overwritten;
*  an index the caller kept from four lookups ago then names a different
*  program, which is why nothing but the shell's "resolve, then immediately
*  spawn" is promised here. */
static int file_register(const char *path, uint32_t size)
{
	int i;

	if (path == 0 || path[0] == '\0') return -1;
	if ((int)strlen(path) >= EXEC_PATH_MAX) return -1;

	for (i = 0; i < EXEC_MAX_FILES; i++)
	{
		if (files[i].used && strcmp(files[i].path, path) == 0)
		{
			files[i].size = size;
			return EXEC_FILE_INDEX_BASE + i;
		}
	}

	i = file_next;
	file_next = (file_next + 1) % EXEC_MAX_FILES;

	copy_bounded(files[i].path, path, EXEC_PATH_MAX);
	name_from_path(path, files[i].name, EXEC_NAME_MAX);
	files[i].size = size;
	files[i].used = 1;

	return EXEC_FILE_INDEX_BASE + i;
}

/* Looks a name up on the mounted volume, first as given and then below the
*  root, so that both "hello.elf" and "/bin/hello.elf" work. Returns the
*  index of the registered file, or -1. */
static int file_lookup(const char *name)
{
	char     path[EXEC_PATH_MAX];
	uint32_t size;

	if (!fat_mounted()) return -1;
	if (name[0] == '\0') return -1;

	if (fat_size(name, &size) == 0) return file_register(name, size);

	if (name[0] == '/' || (int)strlen(name) + 1 >= EXEC_PATH_MAX) return -1;

	path[0] = '/';
	copy_bounded(path + 1, name, EXEC_PATH_MAX - 1);

	if (fat_size(path, &size) == 0) return file_register(path, size);

	return -1;
}

/* --- Looking programs up -------------------------------------------------- */

int exec_module_count(void)
{
	return module_count;
}

const char *exec_module_name(int index)
{
	exec_file *f;

	f = file_at(index);
	if (f != 0) return f->name;

	if (index < 0 || index >= module_count) return "?";
	return modules[index].name;
}

uint32_t exec_module_size(int index)
{
	exec_file *f;

	f = file_at(index);
	if (f != 0) return f->size;

	if (index < 0 || index >= module_count) return 0;
	return modules[index].size;
}

/* A module of that name, or - since there is a filesystem - a file of that
*  name on the mounted volume. Modules win, because they are the ones a
*  "module" line in grub.cfg deliberately put there.
*
*  Resolving a file here rather than in a second lookup function is what
*  keeps the caller free of the distinction: whatever comes back is an index
*  exec_spawn() accepts, and exec_module_name() and exec_module_size()
*  describe it either way. */
int exec_module_find(const char *name)
{
	int i;

	if (name == 0) return -1;

	for (i = 0; i < module_count; i++)
	{
		if (strcmp(modules[i].name, name) == 0) return i;
	}

	return file_lookup(name);
}

const char *exec_last_error(void)
{
	return last_error;
}

/* --- The image, the one thing both sources have to answer ----------------- */

/* len bytes at offset off of the image, into dest. Returns 0 on success.
*
*  The range is checked against the size of the image first, for both kinds
*  alike: a module read past its end would walk into the next one, and a file
*  read past its end would come back short and leave dest half filled with
*  whatever was in the frame. */
static int image_read(const exec_image *img, uint32_t off, uint32_t len,
                      void *dest)
{
	int got;

	if (len == 0) return 0;
	if (off > img->size || len > img->size - off) return -1;

	if (img->path == 0)
	{
		memcpy(dest, (const char *)(img->base + off), (size_t)len);
		return 0;
	}

	got = fat_read(img->path, off, len, dest);
	if (got < 0 || (uint32_t)got != len) return -1;

	return 0;
}

/* --- ELF32 loading -------------------------------------------------------- */

/* Program header number i. The caller has already established that the whole
*  table lies inside the image - and, for a file, that the heap copy reaches
*  that far. */
static elf32_phdr *phdr_at(const exec_image *img, elf32_ehdr *eh, int i)
{
	return (elf32_phdr *)(img->base + eh->e_phoff +
	                      (uint32_t)i * (uint32_t)eh->e_phentsize);
}

/* Everything about the file header that has to hold before any field of it is
*  believed. Returns 0 when the image is a static 32-bit i386 executable
*  whose program header table lies completely inside it.
*
*  Reads nothing but the ELF header itself, which is why a file can be run
*  through this the moment its first 52 bytes are in the heap - long before
*  it is known how far the program header table reaches. */
static int check_header(const exec_image *img)
{
	elf32_ehdr *eh;
	uint32_t    size;
	uint32_t    table;

	size = img->size;

	if (size < (uint32_t)sizeof(elf32_ehdr))
	{
		exec_set_error_2(img->noun, " is too small to hold an ELF header");
		return -1;
	}

	eh = (elf32_ehdr *)img->base;

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
		exec_set_error_2("program header table lies outside the ",
		                 img->noun);
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
static int page_of_earlier_segment(const exec_image *img, elf32_ehdr *eh,
                                   int upto, uint32_t page)
{
	elf32_phdr *ph;
	uint32_t    first;
	uint32_t    last;
	int         i;

	for (i = 0; i < upto; i++)
	{
		ph = phdr_at(img, eh, i);

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
static int load_segment(addrspace_t space, const exec_image *img,
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

	ph = phdr_at(img, eh, index);

	if (ph->p_memsz == 0) return 0;		/* nothing to place */

	if (ph->p_filesz > ph->p_memsz)
	{
		exec_set_error("segment claims more file bytes than memory bytes");
		return -1;
	}

	/* What the segment reads out of the image has to be inside it. */
	if (ph->p_offset > img->size || ph->p_filesz > img->size - ph->p_offset)
	{
		exec_set_error_2("segment contents lie outside the ", img->noun);
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

			if (!page_of_earlier_segment(img, eh, index, page))
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

		/* The one place the two sources part company, and it is a
		*  single call: a module is copied out of the direct mapping,
		*  a file is read from the volume - in both cases straight
		*  into the frame that is about to carry this page. */
		if (to > from)
		{
			if (image_read(img,
			               ph->p_offset + (from - ph->p_vaddr),
			               to - from,
			               (char *)P2V(phys) + (from - page)) != 0)
			{
				exec_set_error_2("could not read the segment out of the ",
				                 img->noun);
				return -1;
			}
		}

		if (page == last) break;
		page += PAGE_SIZE;
	}

	return 0;
}

/* Parses the image and places every PT_LOAD segment into the space. On
*  success *entry_out holds the address the task is to start at. */
static int load_image(addrspace_t space, const exec_image *img,
                      uint32_t *entry_out)
{
	elf32_ehdr *eh;
	elf32_phdr *ph;
	uint32_t    entry;
	int         loaded;
	int         found;
	int         i;

	if (check_header(img) != 0) return -1;

	eh = (elf32_ehdr *)img->base;

	/* A dynamic executable is rejected before a single page is mapped.
	*  There is no dynamic linker to hand the program to, and loading it
	*  anyway would only move the failure to the first call through a
	*  relocation, where nothing points back to this cause. */
	for (i = 0; i < (int)eh->e_phnum; i++)
	{
		ph = phdr_at(img, eh, i);

		if (ph->p_type == PT_INTERP)
		{
			exec_set_error("needs a dynamic linker, link it statically");
			return -1;
		}
	}

	loaded = 0;

	for (i = 0; i < (int)eh->e_phnum; i++)
	{
		ph = phdr_at(img, eh, i);

		if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

		if (load_segment(space, img, eh, i) != 0) return -1;

		loaded++;
	}

	if (loaded == 0)
	{
		exec_set_error_2("no loadable segment in the ", img->noun);
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
		ph = phdr_at(img, eh, i);

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

/* --- Opening a file as an image ------------------------------------------- */

/* Prepares img so that the loader can work on the file at path: the size
*  comes from the directory entry, and the ELF header together with the
*  program header table is copied into the heap, because those are the only
*  bytes the loader dereferences instead of reading.
*
*  Two steps rather than one, and the header check sits between them: how far
*  the table reaches is stated by the very header whose plausibility has not
*  been established yet, so nothing is allocated on the strength of e_phoff
*  before check_header() has passed over it.
*
*  On success the caller owns img->base and releases it with image_close();
*  on failure nothing is left allocated and last_error says why. */
static int image_open_file(exec_image *img, const char *path)
{
	elf32_ehdr *eh;
	uint32_t    hdr;
	char       *buf;

	img->size = 0;
	img->base = 0;
	img->hdr  = 0;
	img->path = path;
	img->noun = "file";

	if (!fat_mounted())
	{
		exec_set_error("no filesystem mounted");
		return -1;
	}

	if (fat_size(path, &img->size) != 0)
	{
		exec_set_error_2("no such file: ", fat_last_error());
		return -1;
	}

	buf = (char *)malloc(sizeof(elf32_ehdr));
	if (buf == 0)
	{
		exec_set_error("no heap memory for the ELF header");
		return -1;
	}

	img->base = (uint32_t)buf;
	img->hdr  = (uint32_t)sizeof(elf32_ehdr);

	/* A file shorter than a header would make this read fail with a
	*  message about the disk, which is not what is wrong with it. */
	if (img->size < (uint32_t)sizeof(elf32_ehdr))
	{
		free(buf);
		img->base = 0;
		exec_set_error("file is too small to hold an ELF header");
		return -1;
	}

	if (image_read(img, 0, (uint32_t)sizeof(elf32_ehdr), buf) != 0)
	{
		free(buf);
		img->base = 0;
		exec_set_error_2("could not read the ELF header: ",
		                 fat_last_error());
		return -1;
	}

	if (check_header(img) != 0)
	{
		free(buf);
		img->base = 0;
		return -1;
	}

	eh = (elf32_ehdr *)img->base;

	/* check_header() has established that this lies inside the file, so
	*  the sum cannot wrap and the only question left is the heap. */
	hdr = eh->e_phoff + (uint32_t)eh->e_phnum * (uint32_t)eh->e_phentsize;

	if (hdr > EXEC_HEADER_MAX)
	{
		free(buf);
		img->base = 0;
		exec_set_error("program header table lies too far into the file");
		return -1;
	}

	if (hdr > img->hdr)
	{
		free(buf);
		img->base = 0;

		buf = (char *)malloc(hdr);
		if (buf == 0)
		{
			exec_set_error("no heap memory for the program headers");
			return -1;
		}

		img->base = (uint32_t)buf;
		img->hdr  = hdr;

		if (image_read(img, 0, hdr, buf) != 0)
		{
			free(buf);
			img->base = 0;
			exec_set_error_2("could not read the program headers: ",
			                 fat_last_error());
			return -1;
		}
	}

	return 0;
}

static void image_close(exec_image *img)
{
	if (img->path != 0 && img->base != 0) free((void *)img->base);

	img->base = 0;
	img->hdr  = 0;
}

/* --- The initial user stack ----------------------------------------------- */

/* Four bytes, little endian, byte by byte. Every store below lands on an
*  aligned address, so this could be a uint32_t write. It is not, because the
*  destination is a char* into a foreign task's page and writing it a byte at
*  a time makes the layout independent of what the compiler assumes about
*  alignment - the cost is four stores per word, in a function that runs once
*  per program start. */
static void put32(char *at, uint32_t value)
{
	at[0] = (char)(value & 0xFF);
	at[1] = (char)((value >> 8) & 0xFF);
	at[2] = (char)((value >> 16) & 0xFF);
	at[3] = (char)((value >> 24) & 0xFF);
}

/* Splits an argument string into words and puts them behind argv[0], which is
*  always the program's own name. Returns argc, -1 if there are more words
*  than out can hold and -2 if they are longer than a stack page could ever
*  take. *bytes_out is what the strings will occupy once every one of them
*  has its terminator.
*
*  Separators are spaces and tabs, any run of them counts once, and leading
*  and trailing ones produce no empty argument. There is no quoting and no
*  escaping: neither the shell nor sys_spawn() offers a way to produce an
*  argument with a blank in it, so a rule for one would be a rule about
*  nothing. Adding it later changes this function and nothing else.
*
*  args may be 0 or empty, and then argc is 1 - a program always has a name,
*  even when it was given nothing else. */
static int args_split(const char *name, const char *args, exec_word *out,
                      int max, uint32_t *bytes_out)
{
	uint32_t bytes;
	int      argc;
	int      i;

	out[0].text = name;
	out[0].len  = (uint32_t)strlen(name);

	argc  = 1;
	bytes = out[0].len + 1;

	if (args != 0)
	{
		i = 0;

		for (;;)
		{
			while (args[i] == ' ' || args[i] == '\t') i++;

			if (args[i] == '\0') break;

			if (argc >= max) return -1;

			out[argc].text = args + i;

			while (args[i] != '\0' && args[i] != ' ' && args[i] != '\t')
				i++;

			out[argc].len = (uint32_t)(args + i - out[argc].text);

			/* Counted before the total can grow past anything the page
			*  could hold, so that a pathological string cannot make the
			*  sum wrap and come out small. */
			bytes += out[argc].len + 1;
			if (bytes > (uint32_t)PAGE_SIZE) return -2;

			argc++;
		}
	}

	*bytes_out = bytes;
	return argc;
}

/* The page the task's user stack lives in, or 0 if there is none.
*
*  Found rather than computed: where the stacks are is the task manager's
*  business, and repeating its arithmetic here would mean two files that have
*  to be changed together whenever the region moves. What is used instead is
*  the one property the loader can rely on - a brand new user task has
*  exactly one page mapped in its user half, and that page is its stack. So
*  the highest mapped page below the kernel half IS the stack, as long as the
*  question is asked before any segment has been placed.
*
*  Which is why this runs first in spawn_image() and not where its result is
*  needed. Afterwards a program loaded high in the user half could well own a
*  page above the stack and the answer would be a segment. */
static uint32_t stack_page_of(addrspace_t space)
{
	uint32_t page;
	int      i;

	page = KERNEL_VIRTUAL_BASE - PAGE_SIZE;

	for (i = 0; i < EXEC_STACK_SEARCH_PAGES; i++)
	{
		if (vmm_get_phys_in(space, page) != 0) return page;

		page -= PAGE_SIZE;
	}

	return 0;
}

/* Writes argc, argv and the strings into the task's stack page and hands back
*  the esp the program has to start with. Returns 0 on success.
*
*  Everything is laid out in USER addresses first - base, array, str - and
*  only the store itself is translated, by subtracting the page and adding it
*  to the kernel view of the frame. That is what makes the pointers in the
*  argv array right: they are the addresses the program will use, not the
*  ones this function is writing through.
*
*  The block is placed against the top of the page and the stack grows down
*  from it, so the program has everything between the page's first byte and
*  base to itself. */
static int stack_build(addrspace_t space, uint32_t page, const char *name,
                       const char *args, uint32_t *esp_out)
{
	exec_word words[EXEC_ARGV_MAX];
	uint32_t  phys;
	char     *kpage;		/* the same frame, seen from the kernel */
	uint32_t  top;			/* first address above the block        */
	uint32_t  strbytes;
	uint32_t  need;
	uint32_t  room;
	uint32_t  base;			/* the esp the program starts with      */
	uint32_t  array;		/* user address of the argv[0] slot     */
	uint32_t  str;			/* user address of the next string      */
	int       argc;
	int       i;

	phys = vmm_get_phys_in(space, page);
	if (phys == 0)
	{
		exec_set_error("the task has no user stack for its arguments");
		return -1;
	}

	phys &= PAGE_ADDR_MASK;

	/* The stack frame comes from the pmm and therefore lies inside the
	*  direct mapping - but the block is written through P2V(), so this is
	*  established rather than assumed. */
	if (phys > DIRECT_MAP_LIMIT)
	{
		exec_set_error("the user stack is outside the direct mapping");
		return -1;
	}

	kpage = (char *)P2V(phys);

	argc = args_split(name, args, words, EXEC_ARGV_MAX, &strbytes);
	if (argc == -1)
	{
		exec_set_error("too many arguments");
		return -1;
	}
	if (argc < 0)
	{
		exec_set_error("the arguments do not fit on the user stack");
		return -1;
	}

	/* The block ends at the top of the page: esp is moved to its base with
	*  taskmgr_task_set_stack(), so nothing has to be kept free above it. */
	top = page + PAGE_SIZE;

	/* Three words - fake return address, argc, argv - then the array with
	*  its null terminator, then the strings. */
	need = 12 + 4 * ((uint32_t)argc + 1) + strbytes;

	/* What is there to spend, checked before base is computed so that the
	*  subtraction below cannot run off the bottom of the page. The 16 is
	*  the alignment slack the rounding may still take. */
	room = top - (page + EXEC_STACK_MIN_FREE);
	if (need + 16 > room)
	{
		exec_set_error("the arguments do not fit on the user stack");
		return -1;
	}

	/* base is the esp the program sees, and the ABI wants esp + 4 - the
	*  first argument of a cdecl function - 16 byte aligned, which is the
	*  state a real "call" leaves behind. Rounded down, never up: upwards
	*  would run past the end of the page. */
	base  = top - need;
	base  = ((base - 12) & ~15u) + 12;

	array = base + 12;
	str   = array + 4 * ((uint32_t)argc + 1);

	put32(kpage + (base - page),     0);			/* return address */
	put32(kpage + (base - page) + 4, (uint32_t)argc);
	put32(kpage + (base - page) + 8, array);

	for (i = 0; i < argc; i++)
	{
		put32(kpage + (array - page) + 4 * (uint32_t)i, str);

		memcpy(kpage + (str - page), words[i].text,
		       (size_t)words[i].len);
		kpage[(str - page) + words[i].len] = '\0';

		str += words[i].len + 1;
	}

	/* argv[argc]. A program is free to walk the array until it hits this
	*  instead of counting down from argc, and both have to work. */
	put32(kpage + (array - page) + 4 * (uint32_t)argc, 0);

	*esp_out = base;
	return 0;
}

/* --- Spawning ------------------------------------------------------------- */

/* Everything both sources share once the image is ready: a suspended task
*  with an address space of its own, the segments in it, argc and argv on its
*  stack, the entry point set. Returns the pid, or -1 with last_error set.
*
*  args is the command line behind the program name and may be 0, which is
*  what a module started with "exec NAME" passes. The argument block is built
*  either way - a program compiled as void _start(int argc, char **argv)
*  reads [esp+4] and [esp+8] whether it was given anything or not, so a path
*  that skipped this would hand it two words of whatever the stack page ends
*  in. There is no "no arguments" case, only argc == 1. */
static int spawn_image(const exec_image *img, const char *name,
                       const char *args, int prio)
{
	addrspace_t space;
	uint32_t    stack;
	uint32_t    entry;
	uint32_t    esp;
	int         pid;

	/* The task comes first and comes suspended: it owns the address space
	*  the segments are mapped into, and it must not be scheduled while its
	*  image is still half in place. The entry point is still unknown here,
	*  which is exactly why it is passed as 0 and set further down. */
	pid = taskmgr_add_user_task(0, name, prio);
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

	/* Asked here and not where it is used: right now the stack is the only
	*  page mapped in the user half, which is what makes it findable at all.
	*  See stack_page_of(). */
	stack = stack_page_of(space);
	if (stack == 0)
	{
		exec_set_error("the new task has no user stack");
		taskmgr_task_abort(pid, EXEC_ABORT_ERRNO, last_error);
		return -1;
	}

	entry = 0;
	esp   = 0;

	/* Every failure from here on leaves a half built task behind, so it is
	*  aborted with the reason attached. The frames and page tables already
	*  mapped belong to its address space and go back to the pmm with it -
	*  there is deliberately no second list of them here that could free
	*  them a second time. */
	if (load_image(space, img, &entry) != 0)
	{
		taskmgr_task_abort(pid, EXEC_ABORT_ERRNO, last_error);
		return -1;
	}

	/* After the segments, because a segment that reached into the stack
	*  page would have been refused by load_segment() as "overlaps something
	*  already mapped" - and if it had not been, the arguments would now be
	*  sitting in the program's own data. */
	if (stack_build(space, stack, name, args, &esp) != 0)
	{
		taskmgr_task_abort(pid, EXEC_ABORT_ERRNO, last_error);
		return -1;
	}

	/* The last two steps, and the ones that turn the loaded image into
	*  something that can run. Both refuse a task that is not suspended any
	*  more, which would mean somebody started this one behind our back.
	*
	*  The stack pointer first: a task whose entry point is set is complete
	*  as far as taskmgr_task_start() is concerned, so setting it before esp
	*  would leave a window in which the program could be started with its
	*  esp still at the top of the page, on top of its own argument block. */
	if (taskmgr_task_set_stack(pid, esp) != 0)
	{
		exec_set_error("the stack pointer was not accepted by the task manager");
		taskmgr_task_abort(pid, EXEC_ABORT_ERRNO, last_error);
		return -1;
	}

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

/* Loads the ELF file at path as a new ring 3 task and returns its pid, or a
*  negative value with exec_last_error() set. Like exec_spawn(), the task
*  comes back suspended.
*
*  args is everything behind the program name on the command line, one
*  string, or 0 when there is nothing. It is split into words here, not by
*  the caller: what a word is belongs to the argument vector, and a caller
*  that had to split it first would need somewhere to put the pieces.
*
*  argv[0] is the last component of path, so a program called as
*  "/BIN/CAT.ELF" sees "CAT.ELF" and not the whole path. The path is where
*  the file was found, the name is what the program is called, and the two
*  are only the same by accident. */
int exec_spawn_path(const char *path, const char *args, int prio)
{
	exec_image img;
	char       name[EXEC_NAME_MAX];
	int        pid;

	exec_set_error("");

	if (path == 0 || path[0] == '\0')
	{
		exec_set_error("no file name given");
		return -1;
	}

	if (image_open_file(&img, path) != 0) return -1;

	name_from_path(path, name, EXEC_NAME_MAX);

	pid = spawn_image(&img, name, args, prio);

	/* The header copy has done its job either way: the segments are in the
	*  task's frames, and nothing below this line reads from the file again.
	*  Released before the pid is returned, so that a caller which starts
	*  the task immediately does not run it against a heap this loader is
	*  still holding on to. */
	image_close(&img);

	return pid;
}

int exec_spawn(int index, int prio)
{
	exec_image  img;
	exec_file  *f;

	exec_set_error("");

	/* An index from exec_module_find() may name a file just as well as a
	*  module. It is handed on rather than duplicated here - the loader is
	*  the same one, only the source of the bytes differs. */
	f = file_at(index);
	if (f != 0) return exec_spawn_path(f->path, 0, prio);

	if (index < 0 || index >= module_count)
	{
		exec_set_error("no module with that index");
		return -1;
	}

	/* exec_init() has already established that the whole module lies inside
	*  the direct mapping, so this pointer is good for size bytes - which is
	*  why a module needs no heap copy of its headers and no read function:
	*  base covers the whole image. */
	img.size = modules[index].size;
	img.base = (uint32_t)P2V(modules[index].start);
	img.hdr  = modules[index].size;
	img.path = 0;
	img.noun = "module";

	return spawn_image(&img, modules[index].name, 0, prio);
}
