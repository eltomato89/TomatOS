/* TomatOS - fetch: retrieve a page over HTTP
*  Desc: An HTTP client in ring 3, standing on the kernel's five TCP calls.
*
*  This program is the reason TCP is not a shell command. The kernel gained
*  sys_resolve(), sys_connect(), sys_send(), sys_recv() and sys_close(); what
*  sits on top of them is a text protocol and a formatter for a human reader,
*  and neither of those has any business running with the whole address space
*  in reach. "ping" and "nslookup" stayed in the kernel because they ARE the
*  protocol they speak; an HTTP client is a program that happens to use one.
*
*  ------------------------------------------------------------------------
*  The command line: three arguments, not a URL
*  ------------------------------------------------------------------------
*      fetch [-i] HOST [PATH] [PORT]
*
*  A URL parser would be friendlier to type at and it was seriously
*  considered. What settled it is what each form costs at the point where the
*  parts are actually used:
*
*    - sys_resolve() wants the host as a NUL terminated string of its own.
*      With separate arguments argv[1] IS that string -- the kernel wrote the
*      terminator when it built the argument vector. Splitting
*      "example.com/index.html" means either writing a NUL into argv, which
*      is memory this program did not create and has no promise about, or
*      copying the host into a fixed buffer, which then needs its own length
*      limit and its own error message for exceeding it.
*    - The three parts are three things. A URL packs them into one string
*      with punctuation, and the whole of the parser's output is the three
*      strings that were typed in the first place, one character apart.
*
*  So the parts are typed apart. The one concession is that a URL is
*  RECOGNISED and answered with the right form (see main()), because the
*  alternative -- handing "example.com/index.html" to the resolver and
*  printing "no such name" -- is a confusing way to report a punctuation
*  mistake.
*
*  A host that is already a dotted quad is parsed here rather than sent to
*  sys_resolve(). That is not an optimisation, it is what makes
*  "fetch 10.0.2.2 / 8080" work on a machine that has an address but no DNS
*  server yet -- the resolver would answer SYS_ENETDOWN for a name that never
*  needed resolving.
*
*  ------------------------------------------------------------------------
*  Why HTTP/1.0
*  ------------------------------------------------------------------------
*  HTTP/1.1 is the version everything speaks, and it is the wrong one to ask
*  for here. 1.1 obliges the CLIENT to handle two things this program would
*  otherwise not need to know about:
*
*    - chunked transfer encoding, where the body arrives as a sequence of
*      hex length prefixes and their payloads and a zero length chunk ends
*      it. That is a second parser, with its own boundary cases, for the sole
*      purpose of finding the end of something.
*    - persistent connections. A 1.1 server keeps the connection open after
*      the response, so "the peer closed" no longer means "the body ended",
*      and the client must know the length in advance -- from Content-Length
*      or from the chunk framing -- or it hangs.
*
*  HTTP/1.0 has neither. The body runs from the blank line after the headers
*  to the moment the server closes the connection, and "the server closed the
*  connection" is exactly what sys_recv() returning 0 already reports. The
*  end of the body costs no code at all, which is the whole argument.
*
*  The request carries a Host: header even so, which 1.0 does not require.
*  Name based virtual hosting means one address serves many sites and the
*  server picks between them by that header alone; without it, most of the
*  interesting servers on the internet answer with somebody else's page, or
*  with 400. It also carries Connection: close, which is redundant under 1.0
*  -- closing is the default when the client did not ask otherwise -- and
*  which is sent anyway because a server that decides to hold the connection
*  open is the one failure mode that would leave this program blocked in
*  sys_recv() with nothing to report.
*
*  ------------------------------------------------------------------------
*  Where the headers end, and the bug that is worth the state machine
*  ------------------------------------------------------------------------
*  The response is a status line, then headers, then a blank line, then the
*  body. The blank line is "\r\n\r\n" -- four bytes -- and TCP has no
*  obligation whatsoever to deliver them in one piece. A server that sends
*  the header block and the body in separate segments will routinely split
*  them, and a client that searches each received chunk for those four bytes
*  works perfectly against one server and prints its own headers as page text
*  against the next one. That is the classic defect in a first HTTP client
*  and it is not reliably reproducible, which is what makes it expensive.
*
*  The cure is to never look at a chunk. feed() below consumes ONE BYTE AT A
*  TIME and keeps its state -- which line it is on, how much of that line it
*  has -- in file scope variables that survive between calls. A split at any
*  offset is then not a case at all: the second call resumes exactly where the
*  first stopped. There is no chunk boundary in the parser because the parser
*  never sees a chunk.
*
*  The blank line is found by counting, not by matching: a header line is
*  ended by '\n', '\r' is discarded wherever it appears in the header block,
*  and a line that ends having collected no characters IS the blank line.
*  That accepts "\r\n\r\n" and also bare "\n\n", which is what a server
*  hand-written against nc tends to emit, and it holds for a header line
*  longer than the line buffer as well: the buffer stops growing but the line
*  is still not empty, so the split cannot be triggered by a truncation.
*
*  ------------------------------------------------------------------------
*  A page can be a JPEG
*  ------------------------------------------------------------------------
*  cat faced this exact question about files and answered it (see the long
*  note in user/cat.c); this program answers it the same way rather than
*  inventing a second convention for the same screen. There is no terminal
*  driver here: sys_putch() acts on the control characters it is handed, so
*  0x08 walks the cursor backwards over text that is already there and 0x0D
*  throws it to the left margin for the next line to overwrite. An image
*  printed verbatim does not display as garbage, it scribbles over the shell.
*
*  So: printable ASCII 0x20..0x7E, '\n' and '\t' pass through; '\r' is
*  dropped because CR LF line ends would otherwise decorate every line;
*  everything else becomes '.'. And, as in cat, what is not shown is COUNTED
*  -- the trailer's "not printable" figure is what turns a screen of dots
*  from a lie about the bytes into a statement about them.
*
*  ------------------------------------------------------------------------
*  What the status code does, and what it does not do
*  ------------------------------------------------------------------------
*  The status line is always printed. A 404 or a 500 is the server answering
*  the question that was asked; the transaction succeeded and the exit status
*  says so. Exit 1 is kept for the cases where no answer arrived or the
*  answer was cut off -- the name did not resolve, the connection failed, the
*  stream ended inside the headers, a key press stopped the transfer. A
*  program that treated a 404 as its own failure would be lying about which
*  side of the wire the problem is on.
*
*  A redirect is NOT followed, and that is a decision rather than an omission:
*
*    - following one means a second round of resolve/connect/send/receive, a
*      hop counter to stop a redirect loop, and a rule for the relative form
*      ("Location: /elsewhere") that is a URL parser again;
*    - and the overwhelming majority of redirects on today's internet are
*      http -> https, which this program cannot follow at any hop count,
*      because there is no TLS on this machine and no prospect of one. An
*      automatic follower would therefore mostly turn one clear message into
*      the same message one connection later.
*
*  So the target is printed, in full, as the thing to type next -- and when
*  it is an https:// target, the reason it cannot be typed next is printed
*  with it.
*/
#include "syscall.h"
#include "lib.h"

/* Exit statuses, as in ls and cat: 0 means the exchange completed, 1 means it
*  did not, 2 means the command line was wrong and nothing was attempted. */
#define FETCH_OK           0
#define FETCH_FAILED       1
#define FETCH_USAGE        2

/* The port assumed when none is given, and the range one may be given in. */
#define FETCH_PORT         80
#define FETCH_PORT_MAX     65535

/* How much is asked for in one sys_recv(). A short return is normal and the
*  loop simply asks again, so this is a throughput knob and nothing else: it
*  is comfortably above one TCP segment, so an ordinary page costs a handful
*  of traps rather than one per segment. */
#define FETCH_RECV         2048

/* How much filtered output is staged before it is handed to the kernel. One
*  sys_write() per 512 characters instead of one sys_putch() per character --
*  the trap, the argument check and the console lock are per call, not per
*  byte. Well under the 1023 characters sys_write() accepts, so a full buffer
*  is never refused. Copied from cat, for the same reason cat has it. */
#define FETCH_OUT          512

/* One line of the header block. Long enough for any status line and for the
*  headers that matter here; a Set-Cookie can exceed it, and losing the tail
*  of one is harmless because nothing in this program acts on it. What must
*  NOT happen is that an over-long line breaks the header/body split, and it
*  cannot: see the note on counting rather than matching, above. */
#define FETCH_LINE         1024

/* The request, and the redirect target kept out of a Location: header. */
#define FETCH_REQUEST      768
#define FETCH_LOCATION     512

/* "host:port/path" as it is echoed in the trailer. */
#define FETCH_TARGET       256

/* cat's filter, byte for byte. */
#define FETCH_FIRST_PRINT  0x20
#define FETCH_LAST_PRINT   0x7E
#define FETCH_REPLACEMENT  '.'

/* sys_send() is documented to take fewer bytes than offered, which the loop
*  in send_all() handles by asking again. It is not documented to take NONE,
*  and if it ever does, a loop that assumes progress becomes a ring 3 spin
*  with a wedged command and no way to type anything at it. So a stall is
*  bounded: at most this many refusals, this many milliseconds apart. */
#define FETCH_SEND_STALLS  200
#define FETCH_SEND_PAUSE   10

/* Where feed() is in the response. */
#define ST_STATUS          0     /* still on the first line              */
#define ST_HEADER          1     /* in the header block                  */
#define ST_BODY            2     /* past the blank line; this is content */


/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* Every buffer here is at file scope, i.e. in .bss, and that is not a style
*  preference. The user stack is ONE 4 KiB page (user/user.ld and the stack
*  region in src/tasks.c); a 2 KiB local array would already be half of it
*  and the pair below would run off the bottom into the guard page, which
*  the kernel answers by killing the task before main() prints anything.
*  .bss costs nothing in the file on disk either -- the loader zeroes the
*  difference between p_filesz and p_memsz. */
static char recv_buf[FETCH_RECV];
static char out_buf[FETCH_OUT + 1];      /* +1 for the NUL sys_write() wants */
static char line_buf[FETCH_LINE];
static char request[FETCH_REQUEST];
static char location[FETCH_LOCATION];
static char target[FETCH_TARGET];

static int out_held;                     /* characters staged in out_buf     */
static int line_held;                    /* characters of the current line   */
static int line_cut;                     /* that line was longer than fits   */
static int state;                        /* ST_*                             */

static int show_headers;                 /* -i was given                     */
static int status_code = -1;             /* from the status line, -1 if none */
static int not_http;                     /* the reply did not begin "HTTP/"  */

static unsigned long received;           /* bytes off the wire, all of them  */
static unsigned long body_bytes;         /* bytes after the blank line       */
static unsigned long hidden;             /* body bytes not shown as themselves */
static unsigned long dropped;            /* header bytes past FETCH_LINE     */
static int at_margin = 1;                /* is the cursor at the left edge?  */


/* ------------------------------------------------------------------ */
/* The output side                                                     */
/* ------------------------------------------------------------------ */

static void out_flush(void)
{
	if(out_held == 0) return;

	out_buf[out_held] = '\0';
	sys_write(out_buf);
	out_held = 0;
}

/* One character into the staging buffer. Never called with a NUL:
*  sys_write() takes a NUL terminated string, so a zero byte in the middle of
*  the buffer would end the write early and swallow everything after it --
*  which is exactly why 0x00 is one of the bytes the filter replaces. */
static void out_putc(char c)
{
	out_buf[out_held++] = c;
	if(out_held == FETCH_OUT) out_flush();
}

/* A string this program wrote itself, staged rather than printed, so that it
*  keeps its place in the stream of filtered bytes around it. */
static void out_text(const char *s)
{
	while(*s != '\0') out_putc(*s++);
}

/* One byte that came off the network. cat's filter exactly, and "count"
*  selects whether a substitution is charged to the trailer's tally: it is
*  for the body, which is content, and is not for a header line shown under
*  -i, where a control character is a curiosity about the protocol rather
*  than a byte of the page that could not be shown. */
static void net_putc(unsigned char c, int count)
{
	if(c == '\n')
	{
		out_putc('\n');
		at_margin = 1;
		return;
	}

	if(c == '\t')
	{
		out_putc('\t');
		at_margin = 0;
		return;
	}

	if(c == '\r')
	{
		/* CR LF line ends: swallowed, the LF does the work. Still counted
		*  -- it is a byte that was not shown. */
		if(count) hidden++;
		return;
	}

	if(c >= FETCH_FIRST_PRINT && c <= FETCH_LAST_PRINT)
	{
		out_putc((char)c);
		at_margin = 0;
		return;
	}

	out_putc(FETCH_REPLACEMENT);
	at_margin = 0;
	if(count) hidden++;
}

/* Prints the line currently in line_buf -- the status line, or a header
*  under -i. Filtered like everything else that arrived from the network:
*  the reason phrase and the header values are strings a server chose, and a
*  server that puts a backspace in one must not be able to walk the cursor
*  over the shell's screen. */
static void show_line(void)
{
	int i;

	for(i = 0; i < line_held; i++) net_putc((unsigned char)line_buf[i], 0);

	/* Said rather than hidden: a truncated header shown as if it were whole
	*  would be a quiet lie about what the server sent. */
	if(line_cut) out_text(" [...]");

	out_putc('\n');
	at_margin = 1;
}


/* ------------------------------------------------------------------ */
/* Reading the response                                                */
/* ------------------------------------------------------------------ */

/* The three digits after the first space of "HTTP/1.1 200 OK". Returns -1
*  for anything that is not shaped like that, which is then reported as an
*  unreadable status line rather than guessed at. */
static int status_from(const char *line)
{
	const char *p;
	int code;
	int i;

	p = strchr(line, ' ');
	if(p == 0) return -1;
	p++;

	code = 0;
	for(i = 0; i < 3; i++)
	{
		if(p[i] < '0' || p[i] > '9') return -1;
		code = code * 10 + (p[i] - '0');
	}

	return code;
}

static char lower(char c)
{
	if(c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
	return c;
}

/* If the line is the named header, returns a pointer to its value with the
*  leading whitespace stepped over; 0 otherwise. The comparison is case
*  insensitive because header names are: "Location", "location" and
*  "LOCATION" are the same header, and servers do not agree on which to
*  send. */
static const char *header_value(const char *line, const char *name)
{
	size_t n;
	size_t i;
	const char *v;

	n = strlen(name);

	for(i = 0; i < n; i++)
	{
		if(line[i] == '\0') return 0;
		if(lower(line[i]) != lower(name[i])) return 0;
	}

	if(line[n] != ':') return 0;

	v = line + n + 1;
	while(*v == ' ' || *v == '\t') v++;
	return v;
}

/* The line just completed is the first one. Either it names the protocol, or
*  this is not an HTTP server and there are no headers to strip -- in which
*  case everything that arrived is content and the line itself is the start
*  of it. Stripping "up to the first blank line" from a reply that has no
*  header block would eat real bytes, and this program's one promise about
*  bytes it does not show is that it says so. */
static void status_line_done(void)
{
	int i;

	if(strncmp(line_buf, "HTTP/", 5) != 0)
	{
		not_http = 1;
		state = ST_BODY;

		out_flush();
		printf("fetch: the reply does not begin with \"HTTP/\", so it is not\n");
		printf("       an HTTP response and nothing is stripped from it --\n");
		printf("       what follows is every byte that arrived.\n");

		for(i = 0; i < line_held; i++) net_putc((unsigned char)line_buf[i], 1);
		net_putc((unsigned char)'\n', 1);
		body_bytes += (unsigned long)line_held + 1;
		return;
	}

	status_code = status_from(line_buf);
	show_line();

	/* Flushed here and not left to fill the buffer: on a slow server the
	*  status line is the only sign of life for as long as the body takes,
	*  and a status line sitting in a staging buffer is no sign at all. */
	out_flush();

	state = ST_HEADER;
}

/* One header line. */
static void header_line(void)
{
	const char *v;

	if(show_headers) show_line();

	v = header_value(line_buf, "location");
	if(v != 0) strlcpy(location, v, sizeof(location));
}

/* The blank line. */
static void headers_done(void)
{
	state = ST_BODY;

	/* One empty line between what the protocol said and what the page says,
	*  so the two are never read as one block of text. */
	out_putc('\n');
	at_margin = 1;
	out_flush();
}

/* Called once for every line of the header block, however many reads it took
*  to assemble. line_held == 0 is the blank line and the end of the block --
*  and it is reached by counting characters, so a line that overflowed
*  line_buf still ends with line_held at the buffer's cap and can never be
*  mistaken for the empty one. */
static void line_done(void)
{
	line_buf[line_held] = '\0';

	if(state == ST_STATUS)      status_line_done();
	else if(line_held == 0)     headers_done();
	else                        header_line();

	line_held = 0;
	line_cut = 0;
}

/* One byte of the header block into the current line. Bytes past the end of
*  the buffer are counted, not silently forgotten. */
static void line_push(char c)
{
	if(line_held < FETCH_LINE - 1)
	{
		line_buf[line_held++] = c;
		return;
	}

	line_cut = 1;
	dropped++;
}

/* THE function this program is built around: everything that arrives goes
*  through here, one byte at a time, and all of the parser's state lives
*  outside it. Where the chunk boundaries fall is therefore not a case that
*  has to be handled, because from in here there are no chunks.
*
*  Note what is NOT done to the body: nothing. Once state is ST_BODY every
*  byte goes to the filter unexamined. '\r' is dropped in the header block
*  because it is protocol there -- the CR of a CR LF -- and dropped in the
*  body because cat drops it, but only the body's is counted. */
static void feed(const unsigned char *data, int n)
{
	int i;
	unsigned char c;

	for(i = 0; i < n; i++)
	{
		c = data[i];

		if(state == ST_BODY)
		{
			body_bytes++;
			net_putc(c, 1);
			continue;
		}

		if(c == '\r') continue;

		if(c == '\n')
		{
			line_done();
			continue;
		}

		line_push((char)c);
	}
}


/* ------------------------------------------------------------------ */
/* Talking to the other end                                            */
/* ------------------------------------------------------------------ */

/* Sends the whole buffer. sys_send() returns how many bytes it TOOK, which
*  may be fewer than were offered -- the send window is finite and the kernel
*  is under no obligation to wait for it. A single call is therefore never
*  enough, and getting this wrong is not a visible bug: the server receives
*  the front of a request, finds no blank line at the end of it, and waits for
*  the rest, so the symptom is a fetch that hangs until a timeout rather than
*  anything that says "truncated".
*
*  Returns 0, or the negative error the kernel gave. */
static int send_all(int handle, const char *buf, unsigned long len)
{
	unsigned long done;
	int stalls;
	int n;

	done = 0;
	stalls = 0;

	while(done < len)
	{
		n = sys_send(handle, buf + done, len - done);

		if(n < 0) return n;

		if(n == 0)
		{
			/* No progress. See FETCH_SEND_STALLS. */
			stalls++;
			if(stalls > FETCH_SEND_STALLS) return SYS_ETIMEDOUT;
			sys_sleep(FETCH_SEND_PAUSE);
			continue;
		}

		/* A send that claims to have taken more than it was offered is the
		*  one number here that came from the other side of the privilege
		*  boundary. It cannot happen, and it is checked anyway. */
		if((unsigned long)n > len - done) return SYS_EINVAL;

		stalls = 0;
		done += (unsigned long)n;
	}

	return 0;
}

/* Milliseconds since a mark. sys_uptime() is an int of milliseconds since
*  boot and wraps after about 24.8 days; a negative difference means the wrap
*  happened during the fetch, and reporting 0 ms for it is a better answer
*  than reporting three weeks. */
static int since(int mark)
{
	int now;

	now = sys_uptime();
	if(now < mark) return 0;
	return now - mark;
}


/* ------------------------------------------------------------------ */
/* Addresses                                                           */
/* ------------------------------------------------------------------ */

/* "10.0.2.2" -> 0x0A000202. Returns 1 on success, 0 if the string is not a
*  dotted quad -- in which case it is a name and the resolver gets it.
*
*  Strict: four parts, one to three digits each, no part above 255, nothing
*  before or after. "10.0.2" and "10.0.2.2." are names as far as this is
*  concerned, and the DNS lookup that follows will say so. */
static int parse_ipv4(const char *s, unsigned long *out)
{
	unsigned long ip;
	int part;
	int digits;
	int value;

	ip = 0;

	for(part = 0; part < 4; part++)
	{
		if(part != 0)
		{
			if(*s != '.') return 0;
			s++;
		}

		value = 0;
		digits = 0;

		while(*s >= '0' && *s <= '9')
		{
			value = value * 10 + (*s - '0');
			digits++;
			if(value > 255 || digits > 3) return 0;
			s++;
		}

		if(digits == 0) return 0;

		ip = (ip << 8) | (unsigned long)value;
	}

	if(*s != '\0') return 0;

	*out = ip;
	return 1;
}

/* Host order in, dotted quad out -- the byte order the kernel uses
*  everywhere outside a packed header (see the README's network chapter). */
static void print_ip(unsigned long ip)
{
	printf("%lu.%lu.%lu.%lu",
	       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
	       (ip >> 8) & 0xFF, ip & 0xFF);
}


/* ------------------------------------------------------------------ */
/* Saying what went wrong                                              */
/* ------------------------------------------------------------------ */

static void usage(void)
{
	printf("Syntax: fetch [-i] HOST [PATH] [PORT]\n");
	printf("          HOST   a name or a dotted quad\n");
	printf("          PATH   what to ask for, \"/\" if not given\n");
	printf("          PORT   the TCP port, %d if not given\n", FETCH_PORT);
	printf("          -i     print the response headers too\n");
	printf("        The parts are separate arguments, not a URL:\n");
	printf("          fetch example.com /index.html\n");
	printf("          fetch 10.0.2.2 / 8080\n");
}

/* Turns a negative system call return into a sentence. "what" names the step
*  that failed, so that every message reads the same however far the exchange
*  had got.
*
*  SYS_ENETDOWN gets the most words because it is the one code that is nearly
*  always the same mistake: the machine has no lease yet. Nothing on this
*  system obtains one at boot -- "dhcp" is a command somebody types -- so a
*  fresh boot answering ENETDOWN to a name lookup is the normal first
*  experience of this program, and "network is down" would send the reader
*  looking at the card. */
static void explain(const char *what, int rc)
{
	switch(rc)
	{
		case SYS_ENETDOWN:
			printf("fetch: %s: the network stack has nothing to work with.\n",
			       what);
			printf("       Either no card was found, or no address has been\n");
			printf("       configured yet. Type \"dhcp\" and try again --\n");
			printf("       \"ifconfig\" shows what the machine currently has.\n");
			break;

		case SYS_ENOENT:
			printf("fetch: %s: there is no such name.\n", what);
			break;

		case SYS_ETIMEDOUT:
			printf("fetch: %s: nothing answered in the time allowed.\n", what);
			break;

		case SYS_ECONNRESET:
			printf("fetch: %s: the peer refused or reset the connection.\n",
			       what);
			break;

		case SYS_EINVAL:
			printf("fetch: %s: the kernel rejected the request as nonsense.\n",
			       what);
			break;

		case SYS_EFAULT:
			printf("fetch: %s: the kernel would not read that argument --\n",
			       what);
			printf("       it is longer than the interface accepts.\n");
			break;

		case SYS_ENOMEM:
			printf("fetch: %s: the kernel has no room for another connection.\n",
			       what);
			break;

		case SYS_EIO:
			printf("fetch: %s: the card refused.\n", what);
			break;

		case SYS_ENOSYS:
			/* The case user/syscall.h warns about at the top: this program
			*  is a file on a disk and the kernel booting it may be older
			*  than the call numbers compiled into it. Worth naming, because
			*  everything else about the machine looks perfectly healthy. */
			printf("fetch: %s: this kernel has no such system call.\n", what);
			printf("       The program on disk is newer than the kernel that\n");
			printf("       is running it -- rebuild and copy both.\n");
			break;

		default:
			printf("fetch: %s: failed with error %d.\n", what, rc);
			break;
	}
}


/* ------------------------------------------------------------------ */
/* What the answer meant                                               */
/* ------------------------------------------------------------------ */

/* Printed after the page, where the reader's eye ends up, and after the
*  trailer so that the last line on the screen is the one worth acting on.
*
*  Nothing here changes the exit status. The server answered; whatever it
*  answered, this program did its job. */
static void report_status(void)
{
	if(status_code < 0)
	{
		if(!not_http)
		{
			printf("fetch: the status line could not be read. The reply began\n");
			printf("       with \"HTTP/\" but what followed was not a code.\n");
		}
		return;
	}

	if(status_code >= 300 && status_code < 400)
	{
		if(location[0] == '\0')
		{
			printf("fetch: %d is a redirect, but the reply named no Location:\n",
			       status_code);
			printf("       header, so there is nothing to redirect to.\n");
			return;
		}

		printf("fetch: %d is a redirect. The page is at\n", status_code);
		printf("         %s\n", location);

		if(strncmp(location, "https:", 6) == 0)
		{
			printf("       which cannot be fetched from here: https means TLS\n");
			printf("       and this machine has none. Nothing above is broken;\n");
			printf("       the page is simply not reachable over plain HTTP.\n");
		} else {
			printf("       fetch does not follow redirects -- ask for that\n");
			printf("       address directly.\n");
		}
		return;
	}

	if(status_code >= 400)
	{
		printf("fetch: %d is the server's answer to the request, not a fault\n",
		       status_code);
		printf("       here. What is printed above is the page it sent\n");
		printf("       instead of the one that was asked for.\n");
	}
}

/* The trailer, in cat's shape and for cat's reason: the filter substitutes
*  bytes, which is a statement about the page that is not true, and the only
*  thing that repairs it is counting what was not shown. "0 not printable"
*  out of 1256 bytes is a page that is exactly what it appeared to be. */
static void report_transfer(int ms)
{
	printf("[%s: ", target);

	if(status_code >= 0) printf("%d, ", status_code);
	else                 printf("no status, ");

	printf("%lu bytes received", received);

	if(!not_http) printf(", %lu body", body_bytes);
	if(hidden != 0) printf(", %lu not printable", hidden);
	if(dropped != 0) printf(", %lu dropped from an over-long line", dropped);

	printf(", %d ms]\n", ms);
}


/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	const char *host;
	const char *path;
	const char *slash;
	unsigned long ip;
	int port;
	int handle;
	int rest;
	int arg;
	int len;
	int rc;
	int got;
	int started;
	int elapsed;
	int failed;

	host = 0;
	path = "/";
	port = FETCH_PORT;
	failed = 0;

	/* The one option, and it may only come first. Anything more elaborate
	*  would mean an option parser, and there is one option. */
	arg = 1;
	if(argc > 1 && argv[1][0] == '-')
	{
		if(strcmp(argv[1], "-i") == 0)
		{
			show_headers = 1;
			arg = 2;
		} else {
			printf("fetch: \"%s\" is not an option here.\n", argv[1]);
			usage();
			return FETCH_USAGE;
		}
	}

	rest = argc - arg;

	if(rest < 1)
	{
		printf("fetch: no host given.\n");
		usage();
		return FETCH_USAGE;
	}

	if(rest > 3)
	{
		printf("fetch: %d arguments after the host; at most two are used.\n",
		       rest - 1);
		usage();
		return FETCH_USAGE;
	}

	host = argv[arg];
	if(rest >= 2) path = argv[arg + 1];

	if(rest == 3)
	{
		if(!parse_int(argv[arg + 2], &port) ||
		   port < 1 || port > FETCH_PORT_MAX)
		{
			printf("fetch: \"%s\" is not a port number between 1 and %d.\n",
			       argv[arg + 2], FETCH_PORT_MAX);
			return FETCH_USAGE;
		}
	}

	if(host[0] == '\0')
	{
		printf("fetch: the host is empty.\n");
		usage();
		return FETCH_USAGE;
	}

	/* A URL is not accepted, and is recognised so that the reader is told
	*  the shape rather than watching the resolver fail on a name that
	*  contains a slash. */
	if(strncmp(host, "http://", 7) == 0 ||
	   strncmp(host, "https://", 8) == 0 ||
	   strchr(host, '/') != 0)
	{
		printf("fetch: \"%s\" is a URL; the parts are separate here.\n", host);
		printf("       \"fetch example.com /index.html\", not\n");
		printf("       \"fetch http://example.com/index.html\".\n");
		return FETCH_USAGE;
	}

	/* A path must be absolute in a request line. Prepending the slash is not
	*  a guess about what was meant -- there is no other thing "index.html"
	*  could be asking for -- and every browser does the same. */
	slash = (path[0] == '/') ? "" : "/";

	/* Kept for the trailer, so the summary names what was actually asked for
	*  rather than what was typed. The port is shown only when it is not the
	*  default, since "example.com:80/" is noise. */
	if(port == FETCH_PORT)
		snprintf(target, sizeof(target), "%s%s%s", host, slash, path);
	else
		snprintf(target, sizeof(target), "%s:%d%s%s", host, port, slash, path);

	started = sys_uptime();

	/* An address that is already an address does not go to DNS. Without
	*  this, "fetch 10.0.2.2 / 8080" on a machine with an address but no
	*  nameserver would fail at the one step it did not need. */
	if(!parse_ipv4(host, &ip))
	{
		rc = sys_resolve(host, &ip);
		if(rc != 0)
		{
			explain(host, rc);
			return FETCH_FAILED;
		}
	}

	printf("fetch: %s is ", host);
	print_ip(ip);
	printf(", connecting to port %d\n", port);

	handle = sys_connect(ip, port);
	if(handle < 0)
	{
		explain("connect", handle);
		return FETCH_FAILED;
	}

	/* HTTP/1.0 and a Host: header -- see the note at the top of this file
	*  for why both, and why Connection: close is spelled out although 1.0
	*  already implies it. The User-Agent is not decoration: a fair number of
	*  servers answer 403 to a request that carries none. */
	if(port == FETCH_PORT)
		len = snprintf(request, sizeof(request),
		               "GET %s%s HTTP/1.0\r\n"
		               "Host: %s\r\n"
		               "User-Agent: TomatOS-fetch/1.0\r\n"
		               "Connection: close\r\n"
		               "\r\n",
		               slash, path, host);
	else
		len = snprintf(request, sizeof(request),
		               "GET %s%s HTTP/1.0\r\n"
		               "Host: %s:%d\r\n"
		               "User-Agent: TomatOS-fetch/1.0\r\n"
		               "Connection: close\r\n"
		               "\r\n",
		               slash, path, host, port);

	/* snprintf() returns the length the request WOULD have had, so this is
	*  the truncation test. A truncated request would be a GET for half a
	*  path with no blank line after it, which the server would wait on
	*  rather than reject. */
	if(len < 0 || (unsigned)len >= sizeof(request))
	{
		printf("fetch: the request does not fit in %u bytes -- the host and\n",
		       (unsigned)sizeof(request));
		printf("       path together are too long.\n");
		sys_close(handle);
		return FETCH_FAILED;
	}

	rc = send_all(handle, request, (unsigned long)len);
	if(rc != 0)
	{
		explain("sending the request", rc);
		sys_close(handle);
		return FETCH_FAILED;
	}

	printf("fetch: connected and %d bytes sent in %d ms\n",
	       len, since(started));

	/* The receive loop. sys_recv() returning 0 is the END OF THE STREAM --
	*  the peer closed and everything it sent has been read -- and under
	*  HTTP/1.0 that is precisely the end of the body. It is not "nothing
	*  arrived yet": sys_recv() blocks, so there is no such answer. Reading
	*  it the other way round would give a client that never terminates on a
	*  page that ended perfectly normally. */
	for(;;)
	{
		/* A page has no length limit and this console has no pager, so the
		*  way out of a stream that is longer than anybody wanted is a key.
		*  Checked once per chunk rather than per byte: sys_peekch() is a
		*  trap, and a trap per byte would cost more than the transfer. */
		if(sys_peekch() != 0)
		{
			out_flush();
			if(!at_margin) printf("\n");
			printf("fetch: stopped by a key press -- the page is incomplete.\n");
			failed = 1;
			break;
		}

		got = sys_recv(handle, recv_buf, FETCH_RECV);

		if(got == 0) break;

		if(got < 0)
		{
			out_flush();
			if(!at_margin) printf("\n");
			explain("receiving", got);
			failed = 1;
			break;
		}

		/* As in send_all(): the count crossed the privilege boundary, and
		*  believing one larger than the buffer would mean having already
		*  read past the end of it. */
		if(got > FETCH_RECV)
		{
			out_flush();
			if(!at_margin) printf("\n");
			printf("fetch: the kernel returned %d bytes for a %d byte buffer.\n",
			       got, FETCH_RECV);
			failed = 1;
			break;
		}

		received += (unsigned long)got;
		feed((const unsigned char *)recv_buf, got);
	}

	/* The clock is read HERE, before the close and before anything is
	*  reported, because the number is meant to answer "how long did the page
	*  take" and sys_close() is not part of that. It waits for our FIN to be
	*  acknowledged, which is a round trip that happens after the last byte
	*  has already been printed -- measured on QEMU's user network that is
	*  two seconds against a fetch of three hundred milliseconds, so counting
	*  it would report a number that is almost entirely the goodbye. */
	elapsed = since(started);

	sys_close(handle);

	/* Whatever the last chunk left staged. The tail of a page reaches the
	*  screen this way almost always, so this is the normal path and not an
	*  exceptional one. */
	out_flush();
	if(!at_margin) printf("\n");

	/* A stream that ended before the blank line did not deliver a response.
	*  Worth its own message: the trailer would otherwise report a perfectly
	*  healthy looking byte count for a page that was never shown. */
	if(state != ST_BODY && !failed)
	{
		if(received == 0)
		{
			/* The handshake completed and then the peer closed without
			*  saying anything. Worth telling apart from a truncated reply:
			*  it usually means something is listening on that port that is
			*  not a web server. */
			printf("fetch: the connection was accepted and then closed without\n");
			printf("       a single byte being sent. Something is listening on\n");
			printf("       port %d, but it did not answer an HTTP request.\n",
			       port);
		} else {
			printf("fetch: the reply ended inside its headers -- there was no\n");
			printf("       blank line, so no body was ever reached.\n");
		}
		failed = 1;
	}

	report_transfer(elapsed);
	report_status();

	return failed ? FETCH_FAILED : FETCH_OK;
}
