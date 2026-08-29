/* TomatOS - ls: list a directory
*  Desc: The shell's "ls", moved out of the kernel and into a ring 3 program.
*
*  This used to be listdir()/fs_list() in src/main.c, where it ran in the
*  shell's own task with the whole kernel in reach: it called fat_readdir()
*  directly, asked fat_mounted() why there was nothing, and padded its columns
*  by hand because the kernel's printf() has no field widths. None of that is
*  available here. What is left is four system calls and a printf that can do
*  "%-12s", and the listing comes out better for it.
*
*  ------------------------------------------------------------------------
*  Why nothing is sorted, and what happens to a directory that is too big
*  ------------------------------------------------------------------------
*  There is no malloc in this program and there is no heap to get one from,
*  so anything held has to be held in a buffer whose size is decided at
*  compile time. That single fact decides the question of sorting, because a
*  sort has to see every name before it may print the first one:
*
*    - Collecting the entries into a fixed array of N and sorting that puts a
*      hard cap on how many entries this program can list correctly. The
*      cap is invisible from outside: "ls" on a directory of N+1 entries
*      would print a perfectly plausible, perfectly wrong listing, and the
*      one thing a listing has to be is complete. Printing the first N and
*      stopping is the same bug with better manners.
*    - Sorting without holding the entries is possible -- print the smallest
*      name, then the smallest name larger than that one, and so on -- but it
*      costs a full walk of the directory per line printed. sys_readdir() is
*      index based and the driver underneath re-reads the directory from its
*      first sector for every single index (see fat_dir_nth() in src/fat.c),
*      so an ordinary listing already costs O(n^2) sector reads. Multiplying
*      that by another n to get alphabetical order is not a trade worth
*      making on a machine reading a real disk one sector at a time.
*
*  So the entries are streamed: one sys_dirent is fetched, printed, and
*  forgotten before the next one is asked for. Memory is one dirent
*  regardless of how large the directory is, there is no cap that a bigger
*  directory could quietly exceed, and the order is the order the directory
*  itself is in -- which on FAT is creation order, stable and reproducible.
*  A listing that needs to be alphabetical can be sorted by whoever reads it;
*  a listing that is missing entries cannot be repaired by anyone.
*
*  LS_MAX_ENTRIES below is therefore NOT a capacity. It is a guard against a
*  directory whose cluster chain loops back on itself, which would otherwise
*  keep this loop printing forever, and running into it is reported as a
*  failure with a non-zero exit status rather than passed off as the end of
*  the listing.
*/
#include "syscall.h"
#include "lib.h"

/* Column widths. A FAT 8.3 name is at most twelve characters, so the name
*  column is exactly wide enough for the longest name there can be and a full
*  row never pushes the size column out of line. The size column holds ten
*  digits, which is every value an unsigned long can take. */
#define LS_NAME_WIDTH    12
#define LS_SIZE_WIDTH    10

/* Upper bound on the directory walk -- see the header comment. A directory
*  this long would already cost millions of sector reads to list, so a real
*  one that hits the bound is far more likely to be damaged than large. */
#define LS_MAX_ENTRIES   4096

/* Exit statuses. main()'s return value becomes the status the kernel records
*  for the task, so it is the only thing a caller that did not read the screen
*  can go on: 0 means the listing is complete, 1 means it is not, and 2 means
*  the command line was wrong and nothing was attempted. */
#define LS_OK            0
#define LS_FAILED        1
#define LS_USAGE         2

/* What is listed when no argument is given. The kernel's readdir accepts "/"
*  and the empty string alike for the root; "/" is the one that prints. */
#define LS_DEFAULT_PATH  "/"


/* Says how to call this. Printed for a wrong command line and for anything
*  that looks like an option, because there are none. */
static void usage(void)
{
	printf("Syntax: ls [PATH]\n");
	printf("          no argument   list the root directory\n");
	printf("          PATH          list that directory instead\n");
}

/* The one explanation of an unmounted filesystem, and the reason it is worth
*  a function of its own: booting without a disk is a NORMAL state on this
*  machine -- "make run" does exactly that -- so "no such file" would be a
*  misleading answer to a perfectly healthy system.
*
*  Ring 3 cannot tell the two causes apart the way the kernel's shell could.
*  sys_statfs() answers "nothing is mounted" and fills in a zeroed structure;
*  whether the ATA driver found no drive at all or found one carrying a
*  filesystem it cannot read is not a distinction the system call interface
*  offers, so this says both and points at the command that knows.
*
*  Returns 1 when a filesystem is mounted and the caller may go ahead. */
static int require_mount(void)
{
	sys_fsinfo info;

	if(sys_statfs(&info) == 0) return 1;

	printf("ls: no filesystem is mounted.\n");
	printf("    Either no disk was found or nothing on it could be read --\n");
	printf("    booting without a disk is a normal case here, not a fault.\n");
	printf("    \"df\" shows what the drivers did find.\n");
	return 0;
}

/* Turns a negative system call return into a sentence. Every error this
*  program can report goes through here, so that "ls: /NOPE: ..." reads the
*  same whichever call produced it.
*
*  SYS_ENOENT arrives here only when a mount has already been established, so
*  it really does mean the path: the "nothing is mounted" reading of that same
*  code is handled by require_mount() before any path is touched. The walk
*  below reads SYS_ENOENT as "the directory ends here" and never brings it to
*  this function at all; it is answered anyway, so that every code the
*  interface can return has an answer in one place. */
static void explain(const char *path, int rc)
{
	switch(rc)
	{
		case SYS_ENOENT:
			printf("ls: %s: no such directory.\n", path);
			break;

		case SYS_EFAULT:
			/* The kernel copies the path onto its own stack and refuses
			*  anything that does not fit in SYS_PATH_MAX (64) bytes. From
			*  out here a rejected pointer and an over-long string look the
			*  same, and the second is the one a user can actually cause. */
			printf("ls: %s: the kernel would not read that path -- it is\n",
			       path);
			printf("    longer than the 63 characters a path may have.\n");
			break;

		case SYS_EIO:
			printf("ls: %s: the disk refused to read.\n", path);
			break;

		case SYS_EINVAL:
			printf("ls: %s: the kernel rejected the request as nonsense.\n",
			       path);
			break;

		default:
			printf("ls: %s: failed with error %d.\n", path, rc);
			break;
	}
}

/* Why a directory produced no rows at all. sys_readdir() answers SYS_ENOENT
*  both for "this directory is empty" and for "there is no such directory",
*  which are very different pieces of news, so the two are separated here
*  instead of being reported as one.
*
*  What separates them is what else the path can be asked. sys_stat() succeeds
*  only for a file, so a path that stats is a file somebody tried to list. A
*  path that neither stats nor yields an entry is not there -- with one honest
*  exception: a FAT subdirectory always carries its own "." and ".." entries,
*  so it can never look empty, but the root directory has no such entries and
*  an empty root really is empty. That is why the root is answered first.
*
*  Returns the exit status the program should end with. */
static int no_entries(const char *path)
{
	unsigned long size;

	if(strcmp(path, "/") == 0 || path[0] == '\0')
	{
		printf("Directory of %s\n", path);
		printf("  (empty)\n");
		return LS_OK;
	}

	if(sys_stat(path, &size) == 0)
	{
		printf("ls: %s is a file of %lu bytes, not a directory.\n",
		       path, size);
		printf("    \"cat %s\" prints it.\n", path);
		return LS_FAILED;
	}

	printf("ls: %s: no such directory.\n", path);
	return LS_FAILED;
}

int main(int argc, char **argv)
{
	sys_dirent ent;
	const char *path;
	unsigned long bytes;
	int entries;
	int files;
	int dirs;
	int index;
	int incomplete;
	int rc;

	if(argc > 2)
	{
		printf("ls: one path at a time, %d were given.\n", argc - 1);
		usage();
		return LS_USAGE;
	}

	path = LS_DEFAULT_PATH;

	if(argc == 2)
	{
		/* There are no options, so anything that looks like one is a
		*  mistake rather than a path -- and a path really beginning with a
		*  dash cannot exist on a FAT volume anyway. */
		if(argv[1][0] == '-')
		{
			printf("ls: \"%s\" looks like an option; there are none.\n",
			       argv[1]);
			usage();
			return LS_USAGE;
		}

		path = argv[1];
	}

	if(!require_mount()) return LS_FAILED;

	bytes = 0;
	entries = 0;
	files = 0;
	dirs = 0;
	incomplete = 0;

	for(index = 0; index < LS_MAX_ENTRIES; index++)
	{
		rc = sys_readdir(path, index, &ent);

		/* SYS_ENOENT is how the walk ends. It is not an error and is not
		*  reported as one -- the directory simply has no entry with that
		*  index. Entry zero answering it means the listing is empty, which
		*  no_entries() below explains rather than reports. */
		if(rc == SYS_ENOENT) break;

		if(rc < 0)
		{
			explain(path, rc);
			printf("    The failure was at entry %d; what is above this\n",
			       index);
			printf("    line is only as much of the directory as was read.\n");
			incomplete = 1;
			break;
		}

		/* The name crossed a privilege boundary. The kernel terminates it
		*  and a FAT 8.3 name cannot fill the field anyway, but this is the
		*  one string in this program that was written by the other side of
		*  the gate, and %s would run off the end of a buffer that was not.
		*  One store is cheaper than trusting. */
		ent.name[SYS_NAME_MAX - 1] = '\0';

		/* The header waits for the first row on purpose: a path that turns
		*  out not to be a directory should not have "Directory of ..."
		*  printed over the top of the message saying so. */
		if(entries == 0)
		{
			printf("Directory of %s\n", path);
			printf("  %-*s  %*s  %s\n",
			       LS_NAME_WIDTH, "Name", LS_SIZE_WIDTH, "Size", "Type");
		}

		if(ent.flags & SYS_DIRENT_DIR)
		{
			/* A directory has no size of its own on FAT -- the kernel sends
			*  zero for one -- and a zero printed in the size column would
			*  read as an empty file. A dash says "this column does not
			*  apply here", which is the true statement. */
			printf("  %-*s  %*s  %s\n",
			       LS_NAME_WIDTH, ent.name, LS_SIZE_WIDTH, "-", "dir");
			dirs++;
		} else {
			printf("  %-*s  %*lu  %s\n",
			       LS_NAME_WIDTH, ent.name, LS_SIZE_WIDTH, ent.size, "file");
			bytes += ent.size;
			files++;
		}

		entries++;
	}

	if(entries == 0)
	{
		/* A listing that failed on its very first entry has already said so
		*  and has nothing to summarise; a listing that found nothing has an
		*  explanation to give. */
		if(incomplete) return LS_FAILED;
		return no_entries(path);
	}

	if(index == LS_MAX_ENTRIES)
	{
		/* The guard, not the end of the directory. Said in as many words,
		*  and with a non-zero exit status behind it, because a truncated
		*  listing that looks complete is the worst thing this program could
		*  hand anybody. */
		printf("  ls: stopped after %d entries -- THIS LISTING IS INCOMPLETE.\n",
		       LS_MAX_ENTRIES);
		printf("      A directory that long is more likely damaged than large.\n");
		incomplete = 1;
	}

	printf("  %d entries (%d files, %d directories), %lu bytes\n",
	       entries, files, dirs, bytes);

	return incomplete ? LS_FAILED : LS_OK;
}
