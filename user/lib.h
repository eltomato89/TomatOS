/* TomatOS - User space runtime library
*  Desc: The small pile of C a TomatOS program is allowed to stand on.
*
*  user/syscall.h is the kernel's ABI and nothing more: it can print a string
*  and read a key, and every program that wants to print a number has to
*  write the division loop itself (see user/hello.c, which does exactly
*  that). This header is the layer above it -- a printf(), the handful of
*  string and memory routines the utilities actually use, and the startup
*  code that turns "an ELF the kernel jumps into" into "a C program with a
*  main()".
*
*  Two rules shape everything in here:
*
*    1. It is linked into EVERY program. A convenience that only renames a
*       system call costs space in every binary on the disk and buys nothing,
*       so it is not here. sys_write(), sys_getch() and friends are perfectly
*       good names already; call them directly.
*
*    2. It depends on user/syscall.h and on nothing else. No src/include/,
*       no host headers, not even <stdarg.h> -- see the va_list section
*       below. A program on disk may be older than the kernel that runs it,
*       and the syscall numbers are the only contract that spans that gap.
*
*  Semantics are the STANDARD C ones wherever a function carries a standard
*  C name, and this is worth saying out loud rather than assuming: the
*  kernel's own strcmp() returned 1 for "equal" for years and the bugs that
*  came out of it were real (src/str.c still carries the note). So:
*  strcmp() returns 0 when the strings are equal, memcmp() likewise, and a
*  negative or positive result orders the operands the way the standard says.
*/
#ifndef __USER_LIB_H
#define __USER_LIB_H

#include "syscall.h"


/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* __SIZE_TYPE__ is what the compiler itself believes size_t to be, which on
*  this 32-bit target is "unsigned int". Spelling it this way rather than
*  hard coding "unsigned int" matters for one specific reason: GCC knows the
*  prototypes of strlen(), memcpy() and the rest even under -fno-builtin, and
*  warns when a declaration disagrees with them. Taking the type from the
*  compiler means the declarations below cannot disagree. */
typedef __SIZE_TYPE__ size_t;

/* Varargs. <stdarg.h> is off limits here: the kernel's copy
*  (src/include/stdarg.h) is a DJGPP-era header that pulls in
*  <sys/djtypes.h>, and the whole point of this library is that a user
*  program includes nothing from src/. What is left is what a freestanding
*  GCC guarantees on its own -- the __builtin_va_* family, which is what
*  <stdarg.h> expands to on any modern compiler anyway. The macros below are
*  therefore not a reimplementation of stdarg, they are the same thing under
*  a name we are allowed to use.
*
*  The DJGPP header's pointer-walking va_arg() would in fact have worked on
*  i386 cdecl, but it is not portable to the compiler's own idea of the
*  calling convention and there is no reason to prefer it. */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_end(ap)          __builtin_va_end(ap)


/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

/* Every program provides this; lib.c provides the _start that calls it.
*  argv[0] is the program name, argv[argc] is 0, and the return value
*  becomes the exit status the kernel records for the task. */
int main(int argc, char **argv);


/* ------------------------------------------------------------------ */
/* Formatted output                                                    */
/* ------------------------------------------------------------------ */

/* The supported format is
*
*      %[flags][width][.precision][length]conversion
*
*      flags        -   left justify inside the field
*                   0   pad numbers with leading zeros instead of spaces
*      width        a decimal count, or * to take an int argument
*      .precision   a decimal count, or .* to take an int argument
*                     - for %s: the maximum number of characters printed
*                     - for the integer conversions: the minimum number of
*                       digits, zero filled on the left
*      length       h, hh and l are accepted and do nothing, because on this
*                   ILP32 target short and char promote to int and long is
*                   exactly as wide as int. ll is REJECTED -- it would mean
*                   a 64-bit argument, which this printf cannot read.
*      conversion   d i   signed decimal
*                   u     unsigned decimal
*                   x X   unsigned hexadecimal, lower or upper case
*                   p     pointer, as 0x followed by eight hex digits
*                   c     one character
*                   s     NUL terminated string, or "(null)" for a 0 pointer
*                   %     a literal percent sign
*
*  Field widths are the reason this exists at all. The kernel's printf()
*  (src/scrn.c) has none, and the code around it pays for that: the shell
*  hand-rolls its column padding and src/kernel.c carries a print_capped()
*  helper purely because there is no way to say "%.20s". The utilities that
*  are moving out of the kernel -- ls, df -- are column printers almost
*  entirely, so %-12s and %8u are the whole job.
*
*  What is deliberately NOT here: %o (nothing on this system speaks octal),
*  %e %f %g (the programs are built -mgeneral-regs-only, so there is no
*  floating point to print), %n (a security hole with no upside), the ' ',
*  '+' and '#' flags, and any 64-bit conversion (a long long divide would
*  drag in libgcc's __udivdi3, and we link -nostdlib).
*
*  AN UNSUPPORTED CONVERSION IS NOT IGNORED. The kernel's printf() has a
*  default case that skips the character and consumes no argument, so one
*  %f in a format string silently shifts every argument after it and the
*  rest of the line is garbage -- the exact bug this note exists to avoid
*  repeating. Here, hitting a conversion this printf does not implement
*  prints the marker "%!" followed by the offending character and then stops
*  processing the format string entirely, because once an argument of
*  unknown width has been skipped the position of every later argument is
*  unknown too. Truncated, visibly wrong output beats plausible, silently
*  wrong output.
*
*  The __format__ attribute makes GCC check the arguments against the format
*  string at every call site, which catches the mismatches before they can
*  ever run. It is spelled with the underscored names on purpose so that the
*  host test harness can rename these functions with -Dprintf=... without
*  mangling the attribute.
*/
int printf(const char *fmt, ...)
	__attribute__((__format__(__printf__, 1, 2)));

int vprintf(const char *fmt, va_list ap)
	__attribute__((__format__(__printf__, 1, 0)));

/* Formats into a buffer instead of onto the console. Standard C semantics:
*  at most size-1 characters are written plus a terminating NUL, the result
*  is always terminated when size > 0, and the return value is the length
*  the complete string WOULD have had -- so a return of size or more means
*  the output was truncated. Earns its place because building a path out of
*  a directory and a dirent name, or a right aligned column out of a number
*  and a unit, is otherwise three string calls and an off-by-one. */
int snprintf(char *buf, size_t size, const char *fmt, ...)
	__attribute__((__format__(__printf__, 3, 4)));

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
	__attribute__((__format__(__printf__, 3, 0)));


/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

/* Standard C, all of them. */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);

/* The bounded copy, and the only copy offered: plain strcpy() cannot be
*  used safely against a name that came out of a directory entry or off a
*  command line, and having it available means someone will.
*
*  Copies at most size-1 characters and ALWAYS terminates when size > 0.
*  Returns strlen(src), i.e. the length it wanted to write -- a return of
*  size or more means the copy was truncated. This is BSD's strlcpy() and
*  not strncpy(): strncpy() does not terminate on truncation and pads the
*  whole remaining buffer with NULs, both of which are surprises nobody
*  here wants. */
size_t strlcpy(char *dst, const char *src, size_t size);


/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/* Standard C, all of them. memcpy() and memset() are not optional even if
*  no program ever calls them by name: GCC synthesises calls to both out of
*  ordinary code -- a struct assignment becomes memcpy, an array initialiser
*  becomes memset -- and with -nostdlib there is nothing else to resolve
*  them against. memmove() is here for the same reason plus one of its own:
*  it is the only one of the four that is defined when the regions overlap,
*  which is what a line editor deleting a character in the middle needs. */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);


/* ------------------------------------------------------------------ */
/* Numbers                                                             */
/* ------------------------------------------------------------------ */

/* Parses a whole decimal string, with an optional leading + or -.
*
*  Returns 1 and stores the value on success, 0 on failure, and touches
*  *out only on success. That return value is the entire reason this is not
*  called atoi(): atoi("banana") and atoi("0") both answer 0, so a program
*  taking a numeric argument cannot tell a valid zero from a typo and every
*  caller ends up re-validating the string by hand.
*
*  Strict on purpose. Leading and trailing whitespace, an empty string, a
*  lone sign, any non-digit anywhere, and a value outside the range of a
*  32-bit signed int are all failures. There is no "parse as far as you can"
*  mode because no caller here wants one -- a command line argument is
*  either a number or a mistake. */
int parse_int(const char *s, int *out);


/* ------------------------------------------------------------------ */
/* Convenience over the raw calls                                      */
/* ------------------------------------------------------------------ */

/* Reads a file from its start into buf, up to size bytes.
*
*  What this adds over sys_read() is the loop. sys_read() is allowed to
*  return a short count -- it does so at the end of the file, and it may do
*  so at a cluster or sector boundary for reasons that belong to the driver
*  and not to the caller -- so a single call is never enough to be sure the
*  buffer is full. Every program that reads a file would otherwise write
*  this same loop, and the version that forgets it works fine until the file
*  crosses a cluster.
*
*  Returns the number of bytes read, which is short at the end of the file,
*  or a negative SYS_* error. An error is reported even when some bytes had
*  already been read; the buffer contents are undefined in that case.
*
*  The usual shape is sys_stat() for the size, then this to fill a buffer of
*  that size. Nothing is appended, in particular no NUL -- the caller knows
*  whether it is holding text. */
long read_file(const char *path, void *buf, size_t size);

/* Reads one line from the keyboard into buf, echoing as it goes.
*
*  Returns the length of the line, not counting the terminator, which is
*  always written when size > 0. Enter ends the line; backspace erases the
*  last character on screen as well as in the buffer; other control keys are
*  ignored; a full buffer stops accepting characters but keeps accepting
*  backspace and Enter, so the line can still be corrected and submitted.
*
*  This is the one wrapper that is unarguably worth its space. sys_getch()
*  gives one key. Turning a stream of keys into a line means echoing each
*  one, and erasing on backspace means printing "\b \b" rather than "\b",
*  because the console's backspace moves the cursor without clearing the
*  cell under it (src/scrn.c putch()). Every interactive program needs this
*  and every one of them would get it slightly wrong. */
int readline(char *buf, size_t size);

#endif
