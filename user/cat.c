/* TomatOS - cat: print a file
*  Desc: The shell's "cat", moved out of the kernel and into a ring 3 program.
*
*  This was printfile()/fs_cat() in src/main.c. The rules about what a byte
*  from a file is allowed to do to the screen have not changed -- they are
*  restated below, because they are the whole of this program's judgement --
*  but everything underneath them has: fat_size() and fat_read() are now
*  sys_stat() and sys_read(), and there is a page of stack instead of the
*  kernel's.
*
*  ------------------------------------------------------------------------
*  Why the file is streamed, and why read_file() is not used
*  ------------------------------------------------------------------------
*  A file can be any size and this program has none to spare, so the file is
*  never held. sys_read() names the file and the offset on every call and
*  keeps no position of its own, which is exactly what makes streaming free:
*  the loop below moves CAT_CHUNK bytes at a time and a one megabyte file
*  costs the same four kilobytes as an empty one.
*
*  lib.h offers read_file(), which loops for the caller, and it is the right
*  tool for the job it describes -- "fill this buffer from the start of the
*  file" -- but that is not this job. It always begins at offset 0, so it can
*  only ever produce the first buffer's worth of a file; using it here would
*  mean a buffer as large as the largest file worth printing, which is not a
*  size that exists. The loop it saves is four lines, and this program needs
*  its own anyway, one chunk further in each time.
*
*  A short read is NORMAL and is not an error. The kernel clamps a single
*  read to SYS_READ_MAX (4096) bytes, the driver below it may stop at a
*  sector or cluster boundary, and the last chunk of a file is short almost
*  by definition. The loop therefore advances by what it actually got and
*  asks again; only a zero and a negative return mean anything.
*
*  ------------------------------------------------------------------------
*  A binary file, which is the case that decides this program's design
*  ------------------------------------------------------------------------
*  There is no terminal driver here, no escape sequence parser, and nothing
*  that could put the console back the way it was. sys_putch() -- and
*  sys_write() through it -- acts on the control characters it is handed:
*  0x08 walks the cursor backwards over text that is already there, 0x0D
*  throws it to the left margin so the next line overwrites the last one,
*  0x09 jumps it to a tab stop. An ELF is full of such bytes. Printed
*  verbatim, "cat /BIN/LS.ELF" would not print a file, it would scribble over
*  the shell's screen and leave the cursor at an address decided by whatever
*  the linker happened to emit -- and there is no "reset" to type afterwards.
*
*  So three kinds of byte get through untouched and nothing else:
*
*    - printable ASCII, 0x20 to 0x7E, the only range where a byte means the
*      same thing in every VGA font and every code page;
*    - '\n', because that is what makes a text file readable at all;
*    - '\t', whose effect on the cursor is forwards and bounded.
*
*  '\r' is dropped rather than replaced: DOS text files end every line with
*  CR LF, and turning the CR into a visible mark would decorate the end of
*  every single line of an ordinary file. The LF that follows does the work.
*  Everything else becomes CAT_REPLACEMENT.
*
*  Substituting is friendlier than printing raw, and it is also a statement
*  about the file that is not true: the bytes on the disk are not dots. That
*  lie is repaired by the trailer, which is the reason the trailer is not
*  decoration. It says how many bytes were shown and how many of them were
*  not printable, so "42 not printable" out of 42 bytes is a file that is not
*  text, and 0 out of 42 is a file that is exactly what it appeared to be.
*  Nothing is hidden -- what is not shown is counted. A program that has to
*  see the actual bytes wants a hex dump, which is a different program with a
*  different output format, not a flag on this one.
*/
#include "syscall.h"
#include "lib.h"

/* How much is read at a time. SYS_READ_MAX is the kernel's own cap on a
*  single read, so asking for more only means being answered with this much
*  anyway -- and asking for less would cost real time. Every sys_read()
*  resolves the path from the root again and walks the cluster chain from the
*  file's first cluster to the offset (src/fat.c), so the number of calls,
*  not the number of bytes, is what a large file pays for. Four kilobytes is
*  the largest chunk the interface will answer in one go. */
#define CAT_CHUNK        4096

/* How much filtered output is collected before it is handed to the kernel.
*  One sys_write() per 512 characters rather than one sys_putch() per
*  character: the trap, the argument checks and the console lock are per
*  call, not per byte. The bound is well under the 1023 characters
*  sys_write() accepts, so a full buffer is never refused. */
#define CAT_OUT          512

/* The range that is let through, and what everything else is shown as. */
#define CAT_FIRST_PRINT  0x20
#define CAT_LAST_PRINT   0x7E
#define CAT_REPLACEMENT  '.'

/* Exit statuses, as in ls: 0 means the whole file was printed, 1 means it
*  was not, 2 means the command line was wrong and nothing was read. */
#define CAT_OK           0
#define CAT_FAILED       1
#define CAT_USAGE        2

/* Both buffers are at file scope, i.e. in .bss, and that is not a style
*  choice. The user stack is ONE 4 KiB page (see user/user.ld and the task
*  manager's stack region); a 4 KiB local array would run straight off the
*  bottom of it into an unmapped guard page and the kernel would kill the
*  task before main() printed anything. .bss costs nothing in the file on
*  disk either -- the loader zeroes the difference between p_filesz and
*  p_memsz -- and only this task ever reaches them. */
static char in_buf[CAT_CHUNK];
static char out_buf[CAT_OUT + 1];   /* +1 for the NUL sys_write() wants */
static int  out_held = 0;


/* --- The output side ----------------------------------------------------- */

static void out_flush(void)
{
	if(out_held == 0) return;

	out_buf[out_held] = '\0';
	sys_write(out_buf);
	out_held = 0;
}

/* One filtered character. Never called with a NUL: sys_write() takes a NUL
*  terminated string, so a zero byte in the middle of the buffer would end
*  the write early and swallow the rest of the chunk -- which is precisely
*  why 0x00 is one of the bytes the filter replaces. */
static void out_putc(char c)
{
	out_buf[out_held++] = c;
	if(out_held == CAT_OUT) out_flush();
}


/* --- Saying what went wrong ---------------------------------------------- */

static void usage(void)
{
	printf("Syntax: cat PATH\n");
	printf("          PATH   print that file as text\n");
	printf("                 \"ls\" lists what there is\n");
}

/* The same explanation ls gives, and for the same reason: a machine with no
*  disk is a normal machine here, and "no such file" would be the wrong
*  answer to it. Ring 3 can see that nothing is mounted; whether that is for
*  want of a drive or want of a filesystem on it is not something the system
*  call interface reports, so both are named.
*
*  Returns 1 when a filesystem is mounted and the caller may go ahead. */
static int require_mount(void)
{
	sys_fsinfo info;

	if(sys_statfs(&info) == 0) return 1;

	printf("cat: no filesystem is mounted.\n");
	printf("     Either no disk was found or nothing on it could be read --\n");
	printf("     booting without a disk is a normal case here, not a fault.\n");
	printf("     \"df\" shows what the drivers did find.\n");
	return 0;
}

/* Turns a negative system call return into a sentence.
*
*  SYS_ENOENT gets the most work because it is the one code that covers more
*  than one situation. A mount has already been established by the time this
*  is reached, so "nothing is mounted" is ruled out -- but the kernel also
*  answers ENOENT for a path that is a directory, because fat_size() refuses
*  one. That is worth telling apart, and it can be: a directory is the thing
*  sys_readdir() will answer for. FAT subdirectories always carry their own
*  "." and ".." entries, so entry zero of a real directory always exists. */
static void explain(const char *path, int rc)
{
	sys_dirent ent;

	switch(rc)
	{
		case SYS_ENOENT:
			if(sys_readdir(path, 0, &ent) == 0)
			{
				printf("cat: %s is a directory, not a file.\n", path);
				printf("     \"ls %s\" lists it.\n", path);
			} else {
				printf("cat: %s: no such file.\n", path);
			}
			break;

		case SYS_EFAULT:
			/* The kernel copies the path onto its own stack and rejects
			*  anything that does not fit in SYS_PATH_MAX (64) bytes. From
			*  out here that is indistinguishable from a bad pointer, and it
			*  is the only one of the two a user can actually cause. */
			printf("cat: %s: the kernel would not read that path -- it is\n",
			       path);
			printf("     longer than the 63 characters a path may have.\n");
			break;

		case SYS_EIO:
			printf("cat: %s: the disk refused to read.\n", path);
			break;

		case SYS_EINVAL:
			printf("cat: %s: the kernel rejected the request as nonsense.\n",
			       path);
			break;

		default:
			printf("cat: %s: failed with error %d.\n", path, rc);
			break;
	}
}


int main(int argc, char **argv)
{
	const char *path;
	unsigned long size;
	unsigned long offset;
	unsigned long want;
	unsigned long hidden;
	unsigned char c;
	int at_margin;
	int failed;
	int got;
	int rc;
	int i;

	if(argc < 2)
	{
		printf("cat: no file given.\n");
		usage();
		return CAT_USAGE;
	}

	if(argc > 2)
	{
		/* One file at a time. Concatenating several is what the name
		*  promises elsewhere, but it also needs a rule for what happens
		*  when the third of five cannot be read, and there is no shell here
		*  that could redirect the result anywhere -- so the honest small
		*  version is the one that ships. */
		printf("cat: one file at a time, %d were given.\n", argc - 1);
		usage();
		return CAT_USAGE;
	}

	/* There are no options, so anything shaped like one is a mistake. */
	if(argv[1][0] == '-')
	{
		printf("cat: \"%s\" looks like an option; there are none.\n", argv[1]);
		usage();
		return CAT_USAGE;
	}

	path = argv[1];

	if(!require_mount()) return CAT_FAILED;

	/* The size is asked for first, and not only to size the loop: it is the
	*  one call that establishes the file exists before anything is printed,
	*  so an error message never has to be untangled from half a file. */
	size = 0;
	rc = sys_stat(path, &size);
	if(rc != 0)
	{
		explain(path, rc);
		return CAT_FAILED;
	}

	if(size == 0)
	{
		/* Not an error -- an empty file is a perfectly good file. Printing
		*  nothing at all would leave the question of whether cat ran, so it
		*  says so and exits successfully. */
		printf("cat: %s is empty (0 bytes).\n", path);
		return CAT_OK;
	}

	offset = 0;
	hidden = 0;
	failed = 0;

	/* Whether the cursor sits at the left margin, so the trailer can start
	*  on a line of its own without inserting a blank line after a file that
	*  already ended in a newline. */
	at_margin = 1;

	while(offset < size)
	{
		want = size - offset;
		if(want > (unsigned long)CAT_CHUNK) want = (unsigned long)CAT_CHUNK;

		got = sys_read(path, offset, want, in_buf);

		if(got < 0)
		{
			out_flush();
			if(!at_margin) printf("\n");
			printf("cat: reading %s failed at offset %lu:\n", path, offset);
			explain(path, got);
			failed = 1;
			break;
		}

		/* Zero means the file ended earlier than the directory entry said
		*  it would. On a damaged volume the cluster chain and the recorded
		*  size need not agree, and the chain is the one holding the bytes,
		*  so this is the end of the file -- the trailer's "x of y bytes"
		*  reports the disagreement rather than hiding it. */
		if(got == 0) break;

		/* A read that answered with more than was asked for would have
		*  written past the end of in_buf already. It cannot happen, and it
		*  is checked anyway: this is the one number in this program that
		*  comes from the other side of the privilege boundary. */
		if((unsigned long)got > want)
		{
			out_flush();
			if(!at_margin) printf("\n");
			printf("cat: the kernel returned %d bytes for a %lu byte request.\n",
			       got, want);
			failed = 1;
			break;
		}

		for(i = 0; i < got; i++)
		{
			c = (unsigned char)in_buf[i];

			if(c == '\n')
			{
				out_putc((char)c);
				at_margin = 1;
			}
			else if(c == '\t')
			{
				out_putc((char)c);
				at_margin = 0;
			}
			else if(c == '\r')
			{
				/* CR LF line ends: swallowed, the LF does the work. It is
				*  still counted -- it is a byte that was not shown. */
				hidden++;
			}
			else if(c >= CAT_FIRST_PRINT && c <= CAT_LAST_PRINT)
			{
				out_putc((char)c);
				at_margin = 0;
			} else {
				out_putc(CAT_REPLACEMENT);
				at_margin = 0;
				hidden++;
			}
		}

		offset += (unsigned long)got;
	}

	/* Whatever is left in the staging buffer -- the last chunk is short
	*  almost always, so this is the normal way the tail of a file reaches
	*  the screen, not an exceptional path. */
	out_flush();

	if(!at_margin) printf("\n");

	printf("[%s: %lu of %lu bytes shown", path, offset, size);

	if(hidden != 0)
	{
		printf(", %lu not printable", hidden);
	}

	printf("]\n");

	return failed ? CAT_FAILED : CAT_OK;
}
