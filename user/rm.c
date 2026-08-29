/* TomatOS - rm: remove a file
*  Desc: The first program on this machine that destroys something.
*
*  Everything else in /BIN reads: ls walks a directory, cat prints a file,
*  fetch pulls a page down a wire. A mistake in any of them costs a screen of
*  output and the time it took. A mistake here costs the file, and there is no
*  undelete on this system -- fat_delete() marks the directory entry free and
*  releases the cluster chain, and the moment anything else is written those
*  clusters are somebody else's. So the interesting part of this program is
*  not the one system call it makes; it is what it does around it.
*
*  ------------------------------------------------------------------------
*  Why it does not ask "are you sure?"
*  ------------------------------------------------------------------------
*  A prompt was written, tried and taken out again. Three reasons, in the
*  order they mattered:
*
*    1. A prompt that appears EVERY time is a prompt nobody reads. It trains
*       the hand to answer before the eye has finished the line, and the one
*       time it matters the answer has already been given. That is not a
*       theory about users, it is what "rm -f" exists for on every system that
*       shipped the prompt: people turn it off, and then the guard is gone for
*       the dangerous cases too.
*    2. There is nothing here for a prompt to protect against. There are no
*       wildcards -- the shell does no globbing, so "rm *" removes a file
*       literally named "*" or nothing at all -- and no recursion, and one
*       name per run. Every removal is a name a human typed, in full, in the
*       command they are looking at. A prompt would ask them to confirm what
*       they had just finished typing.
*    3. The protection a prompt pretends to offer is better spent on being
*       exact BEFORE the call and honest AFTER it. Which is what this does:
*       the file is stat'ed first, so the name is known to be a file and its
*       size is known; and it is stat'ed again afterwards, so "removed" is a
*       fact that was checked rather than an assumption that the call worked.
*
*  What replaces the prompt is the report. "rm: removed /DOCS/NOTE.TXT (412
*  bytes)" names what went, and the size is what makes it useful -- it is the
*  one number that says whether the file that vanished was the file that was
*  meant. Nothing else on the screen could tell the difference between the
*  right NOTE.TXT and the wrong one.
*
*  ------------------------------------------------------------------------
*  The cases, and what each of them is
*  ------------------------------------------------------------------------
*    no argument       Not a removal of nothing -- a command that was not
*                      finished. Usage, exit 2, nothing touched.
*    several names     Refused, exit 2. Not because a loop is hard, but
*                      because of what a loop would have to promise: if the
*                      third of five names cannot be removed, does it stop or
*                      go on? Either answer leaves the reader unsure which of
*                      the five are still there, and with no globbing the five
*                      had to be typed by hand anyway. One name, one outcome.
*    an option         There are none, and the two that would be reached for
*                      -- -f and -r -- are named in the message, because both
*                      have an answer here: nothing prompts, so there is
*                      nothing for -f to suppress, and directories are refused
*                      outright, so there is nothing for -r to recurse into.
*    no such file      An error, exit 1. Not "already gone, nothing to do":
*                      the overwhelmingly likely cause of a name that is not
*                      there is a typo, and the second most likely is being in
*                      the wrong idea of where things are. Both want saying.
*    a directory       sys_unlink() refuses one, and this says which rule it
*                      ran into rather than passing the kernel's code through.
*                      Removing a directory means removing what is inside it,
*                      which is the recursion this program does not have.
*    it is there       Removed, reported with its size, exit 0.
*/
#include "syscall.h"
#include "lib.h"

/* Exit statuses, as in ls, cat and fetch: 0 means the file is gone, 1 means it
*  is not, 2 means the command line was wrong and nothing was attempted. The
*  distinction matters more here than anywhere else -- 2 is the status that
*  promises the disk was not touched. */
#define RM_OK      0
#define RM_FAILED  1
#define RM_USAGE   2


static void usage(void)
{
	printf("Syntax: rm FILE\n");
	printf("          FILE   remove that file. There is no undelete.\n");
	printf("        One file per run, and no options:\n");
	printf("          -f  would suppress a prompt, and nothing prompts\n");
	printf("          -r  would recurse, and directories are refused\n");
}

/* The same explanation ls and cat give, and for the same reason: booting
*  without a disk is a normal state on this machine, and "no such file" would
*  be the wrong answer to a perfectly healthy system with nothing mounted.
*
*  Returns 1 when a filesystem is mounted and the caller may go ahead. */
static int require_mount(void)
{
	sys_fsinfo info;

	if(sys_statfs(&info) == 0) return 1;

	printf("rm: no filesystem is mounted.\n");
	printf("    Either no disk was found or nothing on it could be read --\n");
	printf("    booting without a disk is a normal case here, not a fault.\n");
	printf("    \"df\" shows what the drivers did find.\n");
	return 0;
}

/* Is this path a directory? A FAT subdirectory always carries its own "." and
*  ".." entries, so entry zero of a real directory always exists -- which
*  makes sys_readdir() the one call that answers the question directly. */
static int is_dir(const char *path)
{
	sys_dirent ent;

	return sys_readdir(path, 0, &ent) == 0;
}

static void say_directory(const char *path)
{
	printf("rm: %s is a directory, not a file.\n", path);
	printf("    rm removes files. Removing a directory means removing\n");
	printf("    everything inside it, which is a recursion this program\n");
	printf("    does not have.\n");
}

/* Turns a negative system call return into a sentence.
*
*  TWO codes can mean "that is a directory", and they come from opposite
*  sides of the interface. sys_stat() collapses a directory into SYS_ENOENT,
*  because it refuses one exactly as it refuses a name that is not there; the
*  write side is precise and sys_unlink() answers SYS_EINVAL for one. In the
*  ordinary run of this program only the first is ever seen -- the stat comes
*  first and stops there -- but the second is answered too, because "the
*  kernel rejected that name" would be a poor thing to say about /BIN, and the
*  cost of being right about it is one probe.
*
*  A mount is already established by the time this is reached, so SYS_ENOENT
*  cannot mean "nothing is mounted" here; require_mount() answered that. */
static void explain(const char *path, int rc)
{
	switch(rc)
	{
		case SYS_ENOENT:
			if(is_dir(path))
			{
				say_directory(path);
			} else {
				printf("rm: %s: no such file.\n", path);
				printf("    Nothing was removed. \"ls\" lists what there is.\n");
			}
			break;

		case SYS_EINVAL:
			/* A directory, or a name the kernel will not act on at all -- a
			*  path that is not a valid 8.3 name, or "/" itself. */
			if(is_dir(path))
			{
				say_directory(path);
				break;
			}
			printf("rm: %s: the kernel rejected that name.\n", path);
			printf("    A file here is an 8.3 name on a FAT volume; \"ls\"\n");
			printf("    shows the names as the disk spells them.\n");
			break;

		case SYS_EFAULT:
			/* The kernel copies the path onto its own stack and refuses
			*  anything that does not fit in SYS_PATH_MAX (64) bytes. From out
			*  here a rejected pointer and an over-long string look the same,
			*  and the second is the one a user can actually cause. */
			printf("rm: %s: the kernel would not read that path -- it is\n",
			       path);
			printf("    longer than the 63 characters a path may have.\n");
			break;

		case SYS_EROFS:
			printf("rm: %s: this volume cannot be written to.\n", path);
			break;

		case SYS_EIO:
			printf("rm: %s: the disk refused. The file may or may not still\n",
			       path);
			printf("    be there -- \"ls\" is the one that knows.\n");
			break;

		case SYS_ENOSYS:
			/* user/syscall.h warns about exactly this at the top: the program
			*  is a file on a disk and the kernel booting it may be older than
			*  the call numbers compiled into it. Worth naming, because
			*  everything else about the machine looks perfectly healthy. */
			printf("rm: this kernel has no unlink system call.\n");
			printf("    The program on disk is newer than the kernel that is\n");
			printf("    running it -- rebuild and copy both.\n");
			break;

		default:
			printf("rm: %s: failed with error %d.\n", path, rc);
			break;
	}
}

int main(int argc, char **argv)
{
	const char *path;
	unsigned long size;
	unsigned long after;
	int rc;
	int i;

	if(argc < 2)
	{
		printf("rm: no file given.\n");
		usage();
		return RM_USAGE;
	}

	/* Asked BEFORE the count, and the order is the point: "rm -f A" is
	*  somebody reaching for an option, and answering it with "one file at a
	*  time, 2 were given" would be counting a flag as a file name and
	*  answering a question nobody asked. A name beginning with a dash cannot
	*  exist on a FAT volume, so nothing real is refused by this. */
	for(i = 1; i < argc; i++)
	{
		if(argv[i][0] == '-')
		{
			printf("rm: \"%s\" looks like an option; there are none.\n",
			       argv[i]);
			usage();
			return RM_USAGE;
		}
	}

	if(argc > 2)
	{
		/* See the header: the objection is not to the loop, it is to what a
		*  loop would have to promise about a failure halfway down the list. */
		printf("rm: one file at a time, %d were given.\n", argc - 1);
		printf("    Removing several means deciding what happens when the\n");
		printf("    third of them cannot be removed, and either answer leaves\n");
		printf("    you unsure which ones are still there.\n");
		usage();
		return RM_USAGE;
	}

	path = argv[1];

	if(!require_mount()) return RM_FAILED;

	/* Asked first, and it does two jobs. It establishes that the name is a
	*  FILE -- sys_stat() succeeds for nothing else -- so a directory or a
	*  typo is reported before anything is destroyed rather than as a code
	*  coming back out of the removal. And it takes the size, which is the
	*  only detail the report afterwards can offer that would let a reader
	*  notice they removed the wrong NOTE.TXT. Neither is possible after the
	*  fact: once the entry is gone there is nothing left to ask. */
	size = 0;
	rc = sys_stat(path, &size);
	if(rc != 0)
	{
		explain(path, rc);
		return RM_FAILED;
	}

	rc = sys_unlink(path);
	if(rc != 0)
	{
		/* The file was there a moment ago and is still there now. Whatever
		*  the code says, the state of the disk is the news. */
		explain(path, rc);
		printf("    %s was NOT removed.\n", path);
		return RM_FAILED;
	}

	/* "A program that says removed is not a program that removed." The call
	*  returned 0, which is the kernel's account of what it did; this is the
	*  filesystem's. It costs one directory walk and it is the difference
	*  between reporting a return value and reporting a fact.
	*
	*  A file that survives its own successful removal means the directory
	*  entry and the disk disagree, which is worth an alarm rather than a
	*  shrug: everything written to this volume afterwards is at risk. */
	if(sys_stat(path, &after) == 0)
	{
		printf("rm: %s IS STILL THERE (%lu bytes) after the kernel reported\n",
		       path, after);
		printf("    it removed. Something is wrong with this volume -- do not\n");
		printf("    write to it until you know what.\n");
		return RM_FAILED;
	}

	printf("rm: removed %s (%lu bytes)\n", path, size);
	return RM_OK;
}
