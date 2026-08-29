/* TomatOS - cp: copy a file
*  Desc: sys_read() on one side, sys_fwrite() on the other, and a buffer
*        between them that is nowhere near as large as the files it moves.
*
*  ------------------------------------------------------------------------
*  Why the file is never held
*  ------------------------------------------------------------------------
*  There is no malloc here and the user stack is ONE 4 KiB page (user/user.ld
*  and the stack region in src/tasks.c), so "read the file, then write it" is
*  not a shape this program can have -- it would cap the size of a copyable
*  file at something well under a page and fail on anything larger with a
*  fault rather than a message.
*
*  What makes streaming free is the shape of the two calls. Neither
*  sys_read() nor sys_fwrite() has a file position: each one names the file
*  and says where in it to start (see user/syscall.h). So there is nothing to
*  seek, nothing to keep open, and a single offset serves both sides -- the
*  read takes CP_CHUNK bytes from it, the write puts them back at the same
*  place in the other file, and the copy advances. A one megabyte file costs
*  exactly the same four kilobytes of .bss as an empty one.
*
*  That single offset is "copied" below, and it is deliberately ONE variable
*  rather than a read offset and a write offset that happen to agree: it is
*  the number of bytes that have actually reached the destination, so it is
*  also where the next read must start and what the report at the end must
*  say. Two variables could drift apart; one cannot.
*
*  A short sys_read() is NORMAL -- the kernel caps a single read at 4096
*  bytes, the driver may stop at a cluster boundary, and the last chunk of a
*  file is short almost by definition. A short sys_fwrite() is normal too, but
*  it means something quite different: the volume is filling up. Only a return
*  of ZERO from the write says "it will not take any more".
*
*  ------------------------------------------------------------------------
*  The destination, which is where all the judgement is
*  ------------------------------------------------------------------------
*  AN EXISTING DESTINATION IS REFUSED. sys_fcreate() refuses one on purpose
*  and this program does not argue with it. "cp A B" is a request to have a
*  copy of A; it is not a request to lose B, and a mistyped destination is the
*  ordinary way that happens. The reader who did mean it says so with "rm B",
*  which is a command with its own name and its own report.
*
*  THAT IS ALSO WHAT CATCHES A COPY ONTO ITSELF. same_file() below compares
*  the two names the way FAT compares them -- case insensitively, with the
*  separators normalised -- and gives a clear message when they match, but it
*  is a courtesy and not the guard: it cannot see that "/DOCS/../A.TXT" and
*  "/A.TXT" are one file. The guard is that if the destination IS the source,
*  then the destination exists, and an existing destination is refused. There
*  is no arrangement of names that gets past that, which matters more than the
*  message does -- "cp A A" that got through would open the file for writing
*  at offset 0 while reading it at offset 0, and the outcome would depend on
*  the order the driver happened to do things in.
*
*  A PARTIAL COPY IS REMOVED WHEN THE FAILURE IS ON THE DESTINATION SIDE, and
*  KEPT WHEN IT IS ON THE SOURCE SIDE. The two halves of that rule come from
*  the same question -- is the fragment worth anything? -- and get opposite
*  answers:
*
*    - The volume filled up, or a write failed. The source is sitting there
*      intact, so the fragment holds not one byte that cannot be had again,
*      and it is occupying room on a volume that has just proved it has none.
*      It goes, and the message says so.
*    - The source could not be read to the end -- a bad sector, or a file
*      whose directory entry claims more bytes than its cluster chain holds.
*      Now the fragment is the only readable part of something that may never
*      be readable again, and deleting it would be destroying a rescue. It
*      stays, and the message says exactly how much of it is real.
*
*  Note that fetch -o keeps ITS partial file for the second reason, not the
*  first: bytes that came off a network cannot be read again without another
*  transfer. The rule is the same rule; the sources differ.
*
*  ------------------------------------------------------------------------
*  What is not copied
*  ------------------------------------------------------------------------
*  The bytes, and nothing else. FAT carries a timestamp and an attribute byte
*  per entry, and the write interface exposes neither -- sys_fcreate() makes
*  an ordinary file and that is the whole of the choice available. So the copy
*  is a new file with the current time on it, which is the truth about when it
*  was made, and "cp" here is not "cp -p" pretending to be it.
*/
#include "syscall.h"
#include "lib.h"

/* How much moves at a time. SYS_READ_MAX is the kernel's own cap on a single
*  read, so asking for more only means being answered with this much anyway,
*  and asking for less would cost real time: every sys_read() and every
*  sys_fwrite() resolves the path from the root again and walks the cluster
*  chain to the offset (src/fat.c), so the number of CALLS, not the number of
*  bytes, is what a large file pays for. */
#define CP_CHUNK   4096

/* Exit statuses, as in ls, cat, fetch and rm: 0 means the destination holds a
*  complete copy, 1 means it does not, 2 means the command line was wrong and
*  nothing was attempted. */
#define CP_OK      0
#define CP_FAILED  1
#define CP_USAGE   2

/* At file scope, i.e. in .bss, and that is not a style choice: a 4 KiB local
*  array would run straight off the bottom of the one page of user stack into
*  the guard page, and the kernel would kill the task before main() printed
*  anything. .bss costs nothing in the file on disk either -- the loader zeroes
*  the difference between p_filesz and p_memsz. */
static char cp_buf[CP_CHUNK];

/* Bytes that have actually reached the destination. Also the offset both
*  sides of the next chunk use -- see the note at the top on why it is one
*  variable and not two. */
static unsigned long copied;


/* --- Saying what went wrong ---------------------------------------------- */

static void usage(void)
{
	printf("Syntax: cp SOURCE DEST\n");
	printf("          SOURCE   the file to copy\n");
	printf("          DEST     the name to copy it to; it must not exist\n");
	printf("        An existing DEST is refused rather than overwritten --\n");
	printf("        \"rm DEST\" first if that is what was meant.\n");
}

/* The same explanation ls, cat and rm give, and for the same reason: booting
*  without a disk is a normal state on this machine, and "no such file" would
*  be the wrong answer to a perfectly healthy system with nothing mounted.
*
*  Returns 1 when a filesystem is mounted and the caller may go ahead. */
static int require_mount(void)
{
	sys_fsinfo info;

	if(sys_statfs(&info) == 0) return 1;

	printf("cp: no filesystem is mounted.\n");
	printf("    Either no disk was found or nothing on it could be read --\n");
	printf("    booting without a disk is a normal case here, not a fault.\n");
	printf("    \"df\" shows what the drivers did find.\n");
	return 0;
}

/* Is this path a directory? A FAT subdirectory always carries its own "."
*  and ".." entries, so entry zero of a real directory always exists -- which
*  makes sys_readdir() the one call that answers the question directly. */
static int is_dir(const char *path)
{
	sys_dirent ent;

	return sys_readdir(path, 0, &ent) == 0;
}

/* One statement about a directory, true whichever side of the copy ran into
*  it. Worth a function because the two sides reach it through DIFFERENT error
*  codes and would otherwise say different things about the same fact. */
static void say_directory(const char *path)
{
	printf("cp: %s is a directory, not a file.\n", path);
	printf("    cp copies one file to one file -- it will not read a\n");
	printf("    directory and it will not write over one. Copying what is\n");
	printf("    inside a directory is a recursion this program does not\n");
	printf("    have.\n");
}

/* Turns a negative system call return into a sentence. "path" names the file
*  the call was about, so that a message reads the same whichever side of the
*  copy produced it.
*
*  THE TWO SIDES OF THE INTERFACE DISAGREE ABOUT DIRECTORIES, and this
*  function is where that shows. The read side collapses one into SYS_ENOENT,
*  because sys_stat() and sys_read() refuse a directory exactly as they refuse
*  a name that is not there. The write side reports it precisely: sys_fcreate()
*  answers SYS_EEXIST -- the name IS taken, by something -- and sys_fwrite()
*  and sys_unlink() answer SYS_EINVAL. So "cp /DOCS /X" and "cp /X /DOCS" fail
*  through different codes for the same reason, and all three branches ask
*  is_dir() rather than trusting the code to mean what it usually means. The
*  one that matters most is EEXIST: without the probe, a directory destination
*  is told to run "rm /DOCS", which rm refuses. */
static void explain(const char *path, int rc)
{
	switch(rc)
	{
		case SYS_ENOENT:
			if(is_dir(path)) say_directory(path);
			else             printf("cp: %s: no such file.\n", path);
			break;

		case SYS_EEXIST:
			if(is_dir(path))
			{
				say_directory(path);
				break;
			}
			printf("cp: %s: that file is already there.\n", path);
			printf("    cp will not overwrite it -- \"rm %s\" first, or\n",
			       path);
			printf("    copy to another name.\n");
			break;

		case SYS_ENOSPC:
			printf("cp: %s: the volume is full.\n", path);
			printf("    \"df\" shows how much room is left.\n");
			break;

		case SYS_EROFS:
			printf("cp: %s: this volume cannot be written to.\n", path);
			break;

		case SYS_EINVAL:
			/* What sys_fwrite() and sys_unlink() answer for a directory, and
			*  also what the kernel answers for a name it will not act on. */
			if(is_dir(path))
			{
				say_directory(path);
				break;
			}
			printf("cp: %s: the kernel rejected that name.\n", path);
			printf("    A file here is an 8.3 name on a FAT volume; \"ls\"\n");
			printf("    shows the names as the disk spells them.\n");
			break;

		case SYS_EFAULT:
			/* The kernel copies the path onto its own stack and refuses
			*  anything that does not fit in SYS_PATH_MAX (64) bytes. From out
			*  here a rejected pointer and an over-long string look the same,
			*  and the second is the one a user can actually cause. */
			printf("cp: %s: the kernel would not read that path -- it is\n",
			       path);
			printf("    longer than the 63 characters a path may have.\n");
			break;

		case SYS_EIO:
			printf("cp: %s: the disk refused.\n", path);
			break;

		case SYS_ENOSYS:
			/* user/syscall.h warns about exactly this at the top: this
			*  program is a file on a disk and the kernel booting it may be
			*  older than the call numbers compiled into it. */
			printf("cp: this kernel has no write system calls.\n");
			printf("    The program on disk is newer than the kernel that is\n");
			printf("    running it -- rebuild and copy both.\n");
			break;

		default:
			printf("cp: %s: failed with error %d.\n", path, rc);
			break;
	}
}


/* --- Do the two names mean one file? ------------------------------------- */

static char lower(char c)
{
	if(c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
	return c;
}

/* Compares two paths the way the filesystem underneath compares them: 8.3
*  names on FAT are case insensitive, every path is resolved from the root
*  (see fat_resolve() in src/fat.c -- there is no working directory), and runs
*  of '/' as well as a trailing one mean nothing. So "/A.TXT", "a.txt" and
*  "//A.txt/" are one file, and this says so.
*
*  It compares component by component rather than character by character,
*  which is what keeps "AB" and "A/B" apart -- collapsing separators without
*  respecting component boundaries would call those equal.
*
*  What it CANNOT see is a path that reaches the same file by another route,
*  "." and ".." in particular. That is why this is a courtesy and not the
*  guard; the guard is the destination having to not exist. */
static int same_file(const char *a, const char *b)
{
	for(;;)
	{
		while(*a == '/') a++;
		while(*b == '/') b++;

		/* Both exhausted at the same time means every component matched. */
		if(*a == '\0' || *b == '\0') return (*a == *b);

		while(*a != '\0' && *a != '/' && *b != '\0' && *b != '/')
		{
			if(lower(*a) != lower(*b)) return 0;
			a++;
			b++;
		}

		/* One component ran on past the other: not the same name. */
		if(*a != '\0' && *a != '/') return 0;
		if(*b != '\0' && *b != '/') return 0;
	}
}


/* --- Moving the bytes ---------------------------------------------------- */

/* Writes the first "len" bytes of cp_buf to dst at offset "copied", advancing
*  "copied" by every byte the filesystem actually took.
*
*  sys_fwrite() returns how many bytes it TOOK, and a count short of what was
*  offered is not an error: it is the volume running out underneath the write.
*  So this asks again for the remainder, and only a return of ZERO means "it
*  will not take any more". Getting that distinction the wrong way round is
*  the whole reason this function exists -- treating a short count as failure
*  would abandon a copy that was going perfectly well, and treating it as
*  success would report bytes as copied that are not on the disk.
*
*  Returns 0 when everything was written, -1 when it was not; in the second
*  case the reason has already been printed. */
static int write_all(const char *dst, int len)
{
	int done;
	int n;

	done = 0;

	while(done < len)
	{
		n = sys_fwrite(dst, copied, (unsigned long)(len - done),
		               cp_buf + done);

		if(n < 0)
		{
			printf("cp: writing %s failed after %lu bytes:\n", dst, copied);
			explain(dst, n);
			return -1;
		}

		if(n == 0)
		{
			/* No progress on a non-empty request: the volume is full. Said
			*  with the number, because "the disk is full" and "the disk is
			*  full and the copy stops at byte 8192" are different pieces of
			*  news and only the second one can be acted on. */
			printf("cp: the volume is full after %lu bytes.\n", copied);
			return -1;
		}

		/* A write claiming to have taken more than it was offered is the one
		*  number here that crossed the privilege boundary. It cannot happen,
		*  and it is checked anyway: believing it would push "copied" past the
		*  bytes that are really there and leave a zero-filled hole behind. */
		if(n > len - done)
		{
			printf("cp: the kernel wrote %d bytes of a %d byte request.\n",
			       n, len - done);
			return -1;
		}

		copied += (unsigned long)n;
		done += n;
	}

	return 0;
}

/* Removes a destination that was created and could not be filled. Only called
*  for a failure on the destination side -- see the header for why a source
*  side failure keeps its fragment instead. */
static void discard(const char *src, const char *dst)
{
	int rc;

	rc = sys_unlink(dst);
	if(rc == 0)
	{
		printf("    %s has been removed again, so the copy left nothing\n",
		       dst);
		printf("    behind and %s is untouched.\n", src);
		return;
	}

	printf("    %s could NOT be removed again and holds an incomplete\n", dst);
	printf("    copy of %lu bytes:\n", copied);
	explain(dst, rc);
}


int main(int argc, char **argv)
{
	const char *src;
	const char *dst;
	unsigned long size;
	unsigned long want;
	unsigned long check;
	int short_source;
	int failed;
	int got;
	int rc;
	int i;

	/* Asked BEFORE anything is counted, and the order is the point: "cp -r A
	*  B" is somebody reaching for an option, and "one source and one
	*  destination, 3 were given" would be counting a flag as a file name and
	*  answering a question nobody asked. A name beginning with a dash cannot
	*  exist on a FAT volume, so nothing real is refused by this. */
	for(i = 1; i < argc; i++)
	{
		if(argv[i][0] == '-')
		{
			printf("cp: \"%s\" looks like an option; there are none.\n",
			       argv[i]);
			usage();
			return CP_USAGE;
		}
	}

	if(argc < 3)
	{
		if(argc < 2) printf("cp: no files given.\n");
		else         printf("cp: no destination given.\n");
		usage();
		return CP_USAGE;
	}

	if(argc > 3)
	{
		/* "cp A B C" on other systems means "copy A and B into directory C",
		*  which needs a directory to copy into and a rule for what happens
		*  when the second of them fails. Neither exists here, and reading it
		*  as anything else would be a guess about which argument was the
		*  mistake. */
		printf("cp: one source and one destination, %d were given.\n",
		       argc - 1);
		usage();
		return CP_USAGE;
	}

	src = argv[1];
	dst = argv[2];

	if(same_file(src, dst))
	{
		/* Caught here for the message. The guard that cannot be talked
		*  around is the existence check further down -- see the header. */
		printf("cp: %s and %s are the same file.\n", src, dst);
		printf("    Names on a FAT volume are case insensitive and every\n");
		printf("    path is read from the root, so these two reach one file.\n");
		printf("    Nothing was copied and nothing was touched.\n");
		return CP_USAGE;
	}

	if(!require_mount()) return CP_FAILED;

	/* The source is asked for first, and not only to size the loop: it is the
	*  one call that establishes the source exists and is a file, so a mistyped
	*  name is reported before a destination has been created for it. */
	size = 0;
	rc = sys_stat(src, &size);
	if(rc != 0)
	{
		explain(src, rc);
		return CP_FAILED;
	}

	/* The destination must not exist. sys_fcreate() enforces this itself and
	*  is the guard that counts; this check is here for the better message and
	*  for the size, which is the number that tells a reader whether the file
	*  they are about to be refused is one they had forgotten about. */
	check = 0;
	if(sys_stat(dst, &check) == 0)
	{
		printf("cp: %s is already there (%lu bytes).\n", dst, check);
		printf("    cp will not overwrite it: \"cp %s %s\" is a request for\n",
		       src, dst);
		printf("    a copy of %s, not a request to lose %s. Remove it with\n",
		       src, dst);
		printf("    \"rm %s\" if that is what was meant.\n", dst);
		return CP_FAILED;
	}

	rc = sys_fcreate(dst);
	if(rc != 0)
	{
		explain(dst, rc);
		return CP_FAILED;
	}

	copied = 0;
	short_source = 0;
	failed = 0;

	while(copied < size)
	{
		want = size - copied;
		if(want > (unsigned long)CP_CHUNK) want = (unsigned long)CP_CHUNK;

		got = sys_read(src, copied, want, cp_buf);

		if(got < 0)
		{
			printf("cp: reading %s failed at offset %lu:\n", src, copied);
			explain(src, got);
			/* Source side: the fragment stays. See the header. */
			failed = 1;
			break;
		}

		if(got == 0)
		{
			/* The file ended earlier than its directory entry said it would.
			*  On a damaged volume the cluster chain and the recorded size
			*  need not agree, and the chain is the one holding the bytes --
			*  so this is the end of the source, and what has been copied is
			*  everything there was to copy. */
			short_source = 1;
			break;
		}

		/* A read that answered with more than was asked for would have
		*  written past the end of cp_buf already. It cannot happen, and it is
		*  checked anyway: this number came from the other side of the
		*  privilege boundary. */
		if((unsigned long)got > want)
		{
			printf("cp: the kernel returned %d bytes for a %lu byte request.\n",
			       got, want);
			failed = 1;
			break;
		}

		if(write_all(dst, got) != 0)
		{
			/* Destination side: the fragment is worth nothing -- the source
			*  is intact -- and it is sitting on a volume that has just run
			*  out of room. It goes. */
			discard(src, dst);
			return CP_FAILED;
		}
	}

	if(failed)
	{
		printf("    %s holds the first %lu bytes of %s and is INCOMPLETE.\n",
		       dst, copied, src);
		printf("    It is kept: the source is what failed, so this may be the\n");
		printf("    only readable part of it left.\n");
		return CP_FAILED;
	}

	/* "A program that says written is not a program that wrote." The copy
	*  loop reports what the calls returned; this is what the filesystem says
	*  is there afterwards, and it costs one directory walk. A destination
	*  whose size disagrees with the bytes that were written means the volume
	*  is not doing what it said, which is worth an alarm rather than a
	*  cheerful summary. */
	check = 0;
	rc = sys_stat(dst, &check);
	if(rc != 0)
	{
		printf("cp: %s cannot be found after copying %lu bytes into it:\n",
		       dst, copied);
		explain(dst, rc);
		return CP_FAILED;
	}

	if(check != copied)
	{
		printf("cp: %s is %lu bytes after %lu were written to it. Something\n",
		       dst, check, copied);
		printf("    is wrong with this volume -- do not trust the copy.\n");
		return CP_FAILED;
	}

	if(short_source)
	{
		printf("cp: %s says it is %lu bytes but only %lu could be read.\n",
		       src, size, copied);
		printf("    %s holds every byte that was there, which is less than\n",
		       dst);
		printf("    the source claims to hold. The volume is damaged.\n");
		return CP_FAILED;
	}

	printf("cp: copied %lu bytes from %s to %s\n", copied, src, dst);

	if(copied == 0)
		printf("    (%s is empty; %s is an empty file too.)\n", src, dst);

	return CP_OK;
}
