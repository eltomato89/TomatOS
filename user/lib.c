/* TomatOS - User space runtime library
*  Desc: Startup code, printf, strings, and the two wrappers worth having.
*
*  See user/lib.h for what each of these promises. This file is about how.
*
*  Built freestanding and linked -nostdlib, so there is no libgcc to fall
*  back on either. That rules out one thing in particular and it is worth
*  naming: no 64-bit arithmetic anywhere below. A "long long" divide would
*  become a call to __udivdi3, which nothing would resolve. 32-bit unsigned
*  division is a single x86 instruction, so everything here stays in it.
*/
#include "lib.h"


/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

/* The entry point. ENTRY(_start) in user.ld puts its address in e_entry and
*  the loader iret's straight to it.
*
*  The kernel does not "call" this, it resumes a context it built by hand,
*  and the stack it built looks exactly like the one a cdecl call would have
*  left behind:
*
*      higher addresses
*        ...the argument strings themselves, NUL terminated...
*        argv[argc] = 0
*        ...
*        argv[1]
*        argv[0]
*        argv                (a pointer to the argv[0] slot above)
*        argc
*        0                   <- a fake return address, never used
*      esp
*
*  A cdecl function finds its first argument at [esp+4] and its second at
*  [esp+8], which is precisely where argc and argv are. So no assembly stub
*  is needed and none is written: a plain C function with the right
*  signature reads the arguments correctly by construction. The fake return
*  address exists only to make that offset come out right; nothing ever
*  returns through it, because sys_exit() below does not return.
*
*  That is the whole trick. From here on a TomatOS utility is an ordinary C
*  program: it writes main(), it returns a status, and this hands that
*  status to the kernel.
*
*  On stack alignment: GCC assumes a 16-byte aligned stack at function entry
*  on i386 and the kernel's frame makes no such promise. It does not matter
*  here, because the programs are built -mgeneral-regs-only: there is no SSE
*  code to fault on a misaligned spill, and GCC realigns its own frame for
*  anything that would care. If that flag ever goes away, this function
*  needs __attribute__((force_align_arg_pointer)).
*/
void _start(int argc, char **argv)
{
	sys_exit(main(argc, argv));
}


/* ------------------------------------------------------------------ */
/* The output sink                                                     */
/* ------------------------------------------------------------------ */

/* printf() and snprintf() are the same formatter pointed at two different
*  destinations, so the formatter writes through this instead of choosing.
*
*  For the console the characters are staged and handed over in chunks
*  rather than one sys_putch() per character. Two reasons, and the second is
*  the important one:
*
*    - An "int 0x80" per character is a ring transition per character. One
*      call for a whole line is a hundred times cheaper.
*    - The kernel takes its console lock for the length of one sys_write().
*      Character by character, a timer tick between two of them lets another
*      task print into the middle of the line -- exactly the interleaving
*      the kernel's own printf() takes a lock to prevent (src/scrn.c). A
*      chunk at a time does not make a printf() atomic, but it makes the
*      window a hundred times narrower and costs nothing.
*
*  The chunk is deliberately smaller than the 1023 characters sys_write()
*  accepts: it lives on the caller's stack, and a user stack is 8 KiB.
*/
#define LIB_CON_CHUNK  128

/* The largest field width or precision honoured. A width is a decimal
*  number in the format string, so "%99999999d" would otherwise ask for a
*  hundred million spaces one system call at a time. The console is 80
*  columns wide; 255 is already far past anything meaningful. */
#define LIB_FIELD_MAX  255

typedef struct
{
	char   *buf;                    /* destination, or 0 for the console  */
	size_t  cap;                    /* size of buf, including the NUL     */
	size_t  len;                    /* produced so far, fitting or not    */
	size_t  held;                   /* staged in con[] but not yet written */
	char    con[LIB_CON_CHUNK + 1]; /* +1 for the terminator sys_write wants */
} lib_out;

static void out_flush(lib_out *o)
{
	if(o->held == 0) return;

	o->con[o->held] = '\0';
	sys_write(o->con);
	o->held = 0;
}

static void out_putc(lib_out *o, char c)
{
	/* A NUL byte cannot travel. sys_write() takes a NUL terminated string,
	*  so an embedded zero would end the line instead of printing anything,
	*  and the rest of the chunk would be lost. It can only arrive through
	*  %c with a zero argument, which is a caller mistake in any case. It is
	*  dropped, and it is not counted -- and the buffer sink drops it too,
	*  so that printf() and snprintf() of the same arguments never disagree
	*  about the length. */
	if(c == '\0') return;

	if(o->buf != 0)
	{
		/* Standard snprintf() accounting: keep counting past the end of the
		*  buffer, write only what fits, and leave room for the terminator
		*  that vsnprintf() appends afterwards. */
		if(o->len + 1 < o->cap) o->buf[o->len] = c;
	}
	else
	{
		o->con[o->held++] = c;
		if(o->held == LIB_CON_CHUNK) out_flush(o);
	}

	o->len++;
}

static void out_pad(lib_out *o, char c, int n)
{
	while(n-- > 0) out_putc(o, c);
}


/* ------------------------------------------------------------------ */
/* Conversions                                                         */
/* ------------------------------------------------------------------ */

/* Writes value in the given base into dst, most significant digit first,
*  and returns how many digits that took. dst needs 11 bytes: the longest
*  result is 4294967295 in base 10.
*
*  The digits come out of the division loop backwards, so they are collected
*  in a scratch array and reversed. Zero produces one digit, "0" -- the
*  do/while rather than a while is what guarantees that. */
static int fmt_utoa(char *dst, unsigned int value, unsigned int base,
                    int upper)
{
	const char *set;
	char tmp[11];
	int n;
	int i;

	set = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	n = 0;
	do
	{
		tmp[n++] = set[value % base];
		value /= base;
	}
	while(value != 0);

	for(i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
	return n;
}

/* Emits a run of characters inside a field of the given width. Used for %s
*  and %c; the 0 flag is not honoured here, since zero padding a string is
*  undefined in standard C and would only ever be a typo. */
static void fmt_str(lib_out *o, const char *s, int len, int width, int left)
{
	int pad;

	pad = width - len;
	if(pad < 0) pad = 0;

	if(!left) out_pad(o, ' ', pad);
	while(len-- > 0) out_putc(o, *s++);
	if(left) out_pad(o, ' ', pad);
}

/* Emits a converted number inside a field.
*
*  The assembled item is sign, then prefix ("0x" for %p), then any leading
*  zeros, then the digits. The order matters: "-007" is right and "00-7" is
*  not, which is why the zeros cannot simply be handed to the field padding
*  as a fill character.
*
*  Leading zeros come from two places that have to be reconciled. An
*  explicit precision ("%.4d") is a minimum digit count. The 0 flag ("%04d")
*  is a request to fill the whole field with zeros instead of spaces, which
*  amounts to the same thing computed from the width. Standard C says the 0
*  flag is ignored when a precision is given and ignored when the field is
*  left justified, and both of those fall out of the condition below. Once
*  the zero count is known, the remaining field padding is always spaces. */
static void fmt_number(lib_out *o, const char *digits, int ndigits,
                       int negative, const char *prefix, int prefixlen,
                       int width, int left, int zero, int prec)
{
	int head;
	int zeros;
	int total;
	int pad;
	int extra;

	head = (negative ? 1 : 0) + prefixlen;

	zeros = (prec > ndigits) ? prec - ndigits : 0;
	if(zero && !left && prec < 0)
	{
		extra = width - head - ndigits;
		if(extra > zeros) zeros = extra;
	}

	total = head + zeros + ndigits;
	pad = width - total;
	if(pad < 0) pad = 0;

	if(!left) out_pad(o, ' ', pad);
	if(negative) out_putc(o, '-');
	while(prefixlen-- > 0) out_putc(o, *prefix++);
	out_pad(o, '0', zeros);
	while(ndigits-- > 0) out_putc(o, *digits++);
	if(left) out_pad(o, ' ', pad);
}

/* The marker for a conversion this printf does not implement. See the long
*  note in lib.h: the point is that it is impossible to carry on. The
*  argument the unknown conversion would have consumed has an unknown width,
*  so every following va_arg() would read from the wrong place -- and that
*  is worse than stopping, because it produces output that looks fine. */
static void fmt_bad(lib_out *o, char c)
{
	out_putc(o, '%');
	out_putc(o, '!');
	out_putc(o, c);
}

/* The formatter proper. Returns nothing; the caller reads o->len for the
*  count. va_list is passed by value, which is correct on i386 where it is a
*  plain pointer and where the caller never touches it again afterwards. */
static void fmt_core(lib_out *o, const char *fmt, va_list ap)
{
	const char *s;
	char nbuf[11];
	char cbuf[1];
	int left;
	int zero;
	int width;
	int prec;
	int len;
	int ndigits;
	int negative;
	int sv;
	unsigned int uv;

	if(fmt == 0) fmt = "(null)";

	while(*fmt != '\0')
	{
		if(*fmt != '%')
		{
			out_putc(o, *fmt++);
			continue;
		}
		fmt++;

		/* --- flags --------------------------------------------------- */
		left = 0;
		zero = 0;
		for(;;)
		{
			if(*fmt == '-')      { left = 1; fmt++; }
			else if(*fmt == '0') { zero = 1; fmt++; }
			else break;
		}

		/* --- width --------------------------------------------------- */
		width = 0;
		if(*fmt == '*')
		{
			fmt++;
			width = va_arg(ap, int);
			/* A negative width is the same as the - flag, per standard C.
			*  Negating it is written this way rather than as -width so that
			*  the most negative int does not overflow on the way. */
			if(width < 0)
			{
				left = 1;
				width = (width < -LIB_FIELD_MAX) ? LIB_FIELD_MAX : -width;
			}
		}
		else
		{
			while(*fmt >= '0' && *fmt <= '9')
			{
				if(width < LIB_FIELD_MAX)
				{
					width = width * 10 + (*fmt - '0');
				}
				fmt++;
			}
		}
		if(width > LIB_FIELD_MAX) width = LIB_FIELD_MAX;

		/* --- precision ------------------------------------------------ */
		/* -1 means "none given", which is not the same as 0: "%.0d" of the
		*  value 0 prints nothing at all, which is what precision 0 means. */
		prec = -1;
		if(*fmt == '.')
		{
			fmt++;
			if(*fmt == '*')
			{
				fmt++;
				prec = va_arg(ap, int);
				/* A negative precision from * is "as if it were omitted". */
				if(prec < 0) prec = -1;
			}
			else
			{
				prec = 0;
				while(*fmt >= '0' && *fmt <= '9')
				{
					if(prec < LIB_FIELD_MAX)
					{
						prec = prec * 10 + (*fmt - '0');
					}
					fmt++;
				}
			}
			if(prec > LIB_FIELD_MAX) prec = LIB_FIELD_MAX;
		}

		/* --- length modifier ------------------------------------------ */
		/* h, hh and l are accepted and change nothing. That is not
		*  laziness: on this ILP32 target char and short promote to int
		*  before they are ever passed, and long is the same 32 bits as int,
		*  so "%lu" and "%u" genuinely do read the same argument. Accepting
		*  them matters because sys_dirent.size and the sys_fsinfo fields
		*  are declared "unsigned long" and every caller will reach for %lu.
		*
		*  "ll" is rejected. It promises a 64-bit argument, which would sit
		*  in two stack slots and cannot be read here at all -- consuming
		*  only the low half would corrupt everything after it, which is the
		*  precise failure mode this printf exists to avoid. */
		if(*fmt == 'h')
		{
			fmt++;
			if(*fmt == 'h') fmt++;
		}
		else if(*fmt == 'l')
		{
			fmt++;
			if(*fmt == 'l')
			{
				fmt_bad(o, 'l');
				return;
			}
		}

		/* --- conversion ------------------------------------------------ */
		switch(*fmt)
		{
		case 'd':
		case 'i':
			sv = va_arg(ap, int);
			negative = (sv < 0);
			/* The magnitude of the most negative int does not fit in an
			*  int, so it is built in unsigned arithmetic: -(v+1) is always
			*  representable, and one more than that is the magnitude. */
			uv = negative ? (unsigned int)(-(sv + 1)) + 1u : (unsigned int)sv;
			ndigits = fmt_utoa(nbuf, uv, 10, 0);
			if(prec == 0 && uv == 0) ndigits = 0;
			fmt_number(o, nbuf, ndigits, negative, 0, 0,
			           width, left, zero, prec);
			break;

		case 'u':
			uv = va_arg(ap, unsigned int);
			ndigits = fmt_utoa(nbuf, uv, 10, 0);
			if(prec == 0 && uv == 0) ndigits = 0;
			fmt_number(o, nbuf, ndigits, 0, 0, 0, width, left, zero, prec);
			break;

		case 'x':
		case 'X':
			uv = va_arg(ap, unsigned int);
			ndigits = fmt_utoa(nbuf, uv, 16, (*fmt == 'X'));
			if(prec == 0 && uv == 0) ndigits = 0;
			fmt_number(o, nbuf, ndigits, 0, 0, 0, width, left, zero, prec);
			break;

		case 'p':
			/* Fixed shape: 0x and eight digits, because a pointer with a
			*  variable number of digits is unreadable in a column and
			*  because every address here is 32 bits wide anyway. The double
			*  cast goes through unsigned long to say explicitly that this
			*  is an ILP32 assumption and not an accident. The kernel's %p
			*  prints a signed decimal, which is not useful; this does not
			*  copy that. */
			uv = (unsigned int)(unsigned long)va_arg(ap, void *);
			ndigits = fmt_utoa(nbuf, uv, 16, 0);
			fmt_number(o, nbuf, ndigits, 0, "0x", 2, width, left, 0, 8);
			break;

		case 'c':
			cbuf[0] = (char)va_arg(ap, int);
			/* A zero character produces nothing, so the field is all
			*  padding; see out_putc(). */
			fmt_str(o, cbuf, (cbuf[0] != '\0') ? 1 : 0, width, left);
			break;

		case 's':
			s = va_arg(ap, const char *);
			/* A null pointer is printed rather than dereferenced. The
			*  kernel's printf() walks it and faults. */
			if(s == 0) s = "(null)";
			len = (int)strlen(s);
			if(prec >= 0 && prec < len) len = prec;
			fmt_str(o, s, len, width, left);
			break;

		case '%':
			/* Not a conversion, so it takes no argument and no field. */
			out_putc(o, '%');
			break;

		default:
			/* Includes the end of the string: a format ending in a bare
			*  '%' is a truncated specifier and just as broken. */
			fmt_bad(o, *fmt);
			return;
		}

		fmt++;
	}
}


/* ------------------------------------------------------------------ */
/* Formatted output                                                    */
/* ------------------------------------------------------------------ */

int vprintf(const char *fmt, va_list ap)
{
	lib_out o;

	o.buf  = 0;
	o.cap  = 0;
	o.len  = 0;
	o.held = 0;

	fmt_core(&o, fmt, ap);
	out_flush(&o);

	return (int)o.len;
}

int printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vprintf(fmt, ap);
	va_end(ap);

	return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	lib_out o;

	/* A zero size is legal and means "count only": nothing is written, not
	*  even the terminator, and buf may be 0. */
	o.buf  = (size > 0) ? buf : 0;
	o.cap  = size;
	o.len  = 0;
	o.held = 0;

	/* With buf == 0 the sink would be the console, which is not what a
	*  size of 0 asks for. Count without a destination instead. */
	if(o.buf == 0 && size > 0) return 0;

	if(size == 0)
	{
		/* Counting run: point the sink at a one byte scratch that nothing
		*  ever fits into, so out_putc() takes the buffer path and writes
		*  nowhere. */
		o.buf = (char *)&o;
		o.cap = 1;
		fmt_core(&o, fmt, ap);
		return (int)o.len;
	}

	fmt_core(&o, fmt, ap);

	/* Always terminated. o.len may have run past the buffer, so the
	*  terminator goes at the end of what fitted, not at the end of what was
	*  produced. */
	buf[(o.len < size) ? o.len : size - 1] = '\0';

	return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);

	return n;
}


/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

size_t strlen(const char *s)
{
	const char *p;

	p = s;
	while(*p != '\0') p++;

	return (size_t)(p - s);
}

/* Standard C: 0 when equal, otherwise the difference of the first differing
*  pair compared as unsigned char. The unsigned comparison is not decoration
*  -- with plain char the CP437 high bytes the keyboard produces for umlauts
*  would sort before every ASCII character. */
int strcmp(const char *a, const char *b)
{
	while(*a != '\0' && *a == *b)
	{
		a++;
		b++;
	}

	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	while(n > 0 && *a != '\0' && *a == *b)
	{
		a++;
		b++;
		n--;
	}

	if(n == 0) return 0;

	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Standard C, terminator included: strchr(s, '\0') finds the end of the
*  string rather than failing. */
char *strchr(const char *s, int c)
{
	char want;

	want = (char)c;
	for(;;)
	{
		if(*s == want) return (char *)s;
		if(*s == '\0') return 0;
		s++;
	}
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t len;
	size_t n;

	len = strlen(src);

	if(size > 0)
	{
		n = (len < size - 1) ? len : size - 1;
		memcpy(dst, src, n);
		dst[n] = '\0';
	}

	/* The length of the SOURCE, not of what was copied. That is what makes
	*  truncation detectable: a return >= size means something was lost. */
	return len;
}


/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/* Byte at a time, all four of them. A word-at-a-time copy would be several
*  times faster and is not written here, because nothing in a user program
*  moves enough memory for it to matter and because the obvious version is
*  the one that is obviously correct.
*
*  One thing to be careful of: GCC turns byte loops like these back into
*  calls to memcpy() and memset(), which for these two functions would be
*  infinite recursion. It is -fno-builtin in the build flags that stops it
*  -- the loop-to-libcall rewrite is only allowed when the compiler is
*  permitted to assume the standard library. Removing that flag would break
*  this file in a way that only shows up at run time. */
void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d;
	const unsigned char *s;

	d = (unsigned char *)dst;
	s = (const unsigned char *)src;
	while(n-- > 0) *d++ = *s++;

	return dst;
}

/* The overlapping case. Copying backwards is correct exactly when the
*  destination lies above the source inside the same block; in every other
*  case forwards is fine and is what memcpy() would have done. */
void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d;
	const unsigned char *s;

	d = (unsigned char *)dst;
	s = (const unsigned char *)src;

	if(d > s && d < s + n)
	{
		d += n;
		s += n;
		while(n-- > 0) *--d = *--s;
	}
	else
	{
		while(n-- > 0) *d++ = *s++;
	}

	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d;
	unsigned char v;

	d = (unsigned char *)dst;
	v = (unsigned char)c;
	while(n-- > 0) *d++ = v;

	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p;
	const unsigned char *q;

	p = (const unsigned char *)a;
	q = (const unsigned char *)b;

	while(n-- > 0)
	{
		if(*p != *q) return (int)*p - (int)*q;
		p++;
		q++;
	}

	return 0;
}


/* ------------------------------------------------------------------ */
/* Numbers                                                             */
/* ------------------------------------------------------------------ */

int parse_int(const char *s, int *out)
{
	unsigned int mag;
	unsigned int limit;
	unsigned int d;
	int negative;
	int digits;

	if(s == 0 || out == 0) return 0;

	negative = 0;
	if(*s == '+' || *s == '-')
	{
		negative = (*s == '-');
		s++;
	}

	/* How far the magnitude may go: 2147483647 upwards, one more than that
	*  downwards, because the two's complement range is asymmetric. Both are
	*  held as unsigned, which is the only type here that can represent the
	*  negative limit at all. */
	limit = negative ? 2147483648u : 2147483647u;

	mag = 0;
	digits = 0;
	while(*s >= '0' && *s <= '9')
	{
		d = (unsigned int)(*s - '0');

		/* Checked BEFORE folding the digit in. Checking afterwards would
		*  mean inspecting a value that has already wrapped, which tells you
		*  nothing. mag*10 + d <= limit is the same statement as
		*  mag <= (limit - d)/10 in integer arithmetic, and that one cannot
		*  overflow. */
		if(mag > (limit - d) / 10u) return 0;

		mag = mag * 10u + d;
		digits++;
		s++;
	}

	if(digits == 0) return 0;   /* empty string, or nothing but a sign     */
	if(*s != '\0') return 0;    /* trailing rubbish: "12x", "3 ", "1.5"    */

	/* Built without ever negating a value that cannot be negated: for the
	*  most negative int, mag is 2147483648 and mag-1 fits comfortably. */
	*out = negative ? -(int)(mag - 1u) - 1 : (int)mag;

	return 1;
}


/* ------------------------------------------------------------------ */
/* Convenience over the raw calls                                      */
/* ------------------------------------------------------------------ */

long read_file(const char *path, void *buf, size_t size)
{
	unsigned char *p;
	unsigned long off;
	int got;

	if(path == 0 || buf == 0) return SYS_EINVAL;

	p = (unsigned char *)buf;
	off = 0;

	while(off < (unsigned long)size)
	{
		got = sys_read(path, off, (unsigned long)size - off, p + off);

		if(got < 0) return (long)got;   /* SYS_ENOENT, SYS_EIO, ...        */
		if(got == 0) break;             /* end of file                     */

		/* A read that answered with more than was asked for would run off
		*  the end of the buffer on the next round. Refuse to believe it
		*  rather than trust the other side of the gate. */
		if((unsigned long)got > (unsigned long)size - off) return SYS_EIO;

		off += (unsigned long)got;
	}

	return (long)off;
}

int readline(char *buf, size_t size)
{
	size_t len;
	int c;

	if(buf == 0 || size == 0) return 0;

	len = 0;
	for(;;)
	{
		c = sys_getch();

		/* sys_getch() blocks and yields the CPU while it waits, so this is
		*  not a spin loop. A non-positive result should not happen; treat
		*  it as nothing typed rather than as a character. */
		if(c <= 0) continue;

		if(c == '\n' || c == '\r')
		{
			sys_putch('\n');
			break;
		}

		if(c == '\b' || c == 0x7F)
		{
			if(len > 0)
			{
				len--;
				/* "\b \b" and not "\b". The console's backspace only moves
				*  the cursor left, it does not clear the cell (src/scrn.c
				*  putch()), so the character has to be overwritten with a
				*  space and the cursor moved back again. This is the whole
				*  reason this function exists. */
				sys_write("\b \b");
			}
			continue;
		}

		/* Everything else below space is a control key this has no meaning
		*  for -- tab, escape, and whatever the keyboard map produces for
		*  the function keys. Dropping them keeps the buffer printable and
		*  the echo honest. Bytes from 0x80 up are kept: those are the
		*  CP437 umlauts the German keymap emits, and they are ordinary
		*  characters as far as this is concerned. */
		if(c < 0x20) continue;

		/* Full. The line is not truncated silently and not accepted early:
		*  the extra characters are simply not taken, while backspace and
		*  Enter keep working, so the user can still fix and submit it.
		*  size-1 because of the terminator. */
		if(len + 1 >= size) continue;

		buf[len++] = (char)c;
		sys_putch(c);
	}

	buf[len] = '\0';

	return (int)len;
}
