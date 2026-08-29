/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: DNS resolver -- turns a name into an address, over UDP.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*
*  What the protocol is and what is deliberately left out is described in
*  dns.h. What that description leaves out are the properties of this file
*  that are easy to get wrong, and all of them come from the same fact: every
*  byte parsed here was chosen by somebody else, and it is parsed inside an
*  interrupt handler where a mistake does not kill a process -- there are no
*  processes to kill -- but stops the machine.
*
*  1. The name decoder is the whole security story of this file.
*
*  A name on the wire may contain a pointer to an offset earlier in the same
*  message, so decoding means following offsets that came off the wire. A
*  pointer to itself, or a pair of pointers aimed at each other, is an
*  endless loop in interrupt context: no scheduler runs, no key is read, the
*  machine is simply gone. dns_name_decode() therefore treats every byte as
*  hostile and is written so that termination is a property of the arithmetic
*  rather than of the data -- see the long comment above it.
*
*  It is also needed for names nobody wants to read. A resource record starts
*  with a name of variable length, so a record cannot be skipped without
*  decoding the name in front of it, and a decoder that gets that length
*  wrong desynchronises every record after it. There is no separate, sloppier
*  "skip" path here for that reason: the same function does both, and the
*  caller simply passes no output buffer when it only wants the length.
*
*  2. Believing a reply is a decision, not a formality.
*
*  UDP has no connections. Anything on the wire, or anything that can reach
*  this machine, may send a datagram to our port claiming to be the answer,
*  and the first one to arrive wins because we have nothing to compare a
*  second one against. An attacker who can SEE our query has already won and
*  no amount of checking helps; the whole game is against one who cannot see
*  it and has to guess. dns_receive() therefore refuses everything that does
*  not match on all of: the source address, the source port, the port it
*  arrived at, the id, the QR and opcode fields, and the question section
*  echoed back byte for byte in meaning. Each of those the attacker must get
*  right at the same time. See the comment above dns_receive().
*
*  3. Only retransmission makes this work at all.
*
*  A query that is lost provokes nothing: no packet arrives, no handler runs,
*  and a caller watching dns_state() would wait forever. dns_poll() owns that
*  timer, which is why dns.h says nothing else moves a lookup forward.
*
*  Where the work happens is split exactly as in dhcp.c, and for the same
*  reasons. dns_receive() runs in interrupt context: it decides whether the
*  datagram is ours, walks it, and leaves four numbers in a one-slot mailbox.
*  It sends nothing, prints nothing, allocates nothing and keeps no pointer
*  into the driver's receive ring. Everything else -- building a query,
*  writing the cache, changing state -- happens in dns_poll() in task
*  context, which leaves the transmit buffer with exactly one writer.
*
*  On the string routines: this file uses none of them. <string.h> here has
*  strcmp (case sensitive, and DNS names are not), strcpy (unbounded, which
*  is the one thing a parser of hostile input must never be) and nothing that
*  compares or copies with a limit. The three small helpers below do what is
*  needed and do it inside a stated bound.
*/

#include <system.h>
#include <net.h>
#include <dhcp.h>
#include <dns.h>

/* ------------------------------------------------------------------ */
/* Protocol constants                                                  */
/* ------------------------------------------------------------------ */

/* The header is six 16 bit fields and is the only fixed size part of a DNS
*  message. Everything after it is variable length and has to be walked. */
#define DNS_HDR_LEN        12

/* Flags word, by bit. The layout is worth writing down because the field is
*  usually printed as one hex number in captures:
*
*      15   QR      0 = query, 1 = response
*   14-11   OPCODE  0 = standard query
*      10   AA      answer is authoritative
*       9   TC      truncated -- see dns.h
*       8   RD      recursion desired (we set it; we are a stub resolver)
*       7   RA      recursion available (the server's answer to RD)
*     6-4   Z       reserved, and no longer all zero: bit 5 is AD and bit 4
*                   is CD in DNSSEC, so this field must NOT be tested for
*                   zero -- a resolver that does rejects every reply from a
*                   validating server.
*     3-0   RCODE   0 = no error, see dns_rcode_text()
*/
#define DNS_FLAG_QR        0x8000
#define DNS_FLAG_OPCODE    0x7800
#define DNS_FLAG_TC        0x0200
#define DNS_FLAG_RD        0x0100
#define DNS_FLAG_RCODE     0x000F

#define DNS_OPCODE_QUERY   0x0000

/* Response codes we can name. 0-5 are RFC 1035; 6-10 arrive from update and
*  zone transfer machinery a stub resolver never asks for, but a server is
*  free to send one and "unknown" is a worse answer than the name. */
#define DNS_RCODE_NOERROR   0
#define DNS_RCODE_FORMERR   1
#define DNS_RCODE_SERVFAIL  2
#define DNS_RCODE_NXDOMAIN  3
#define DNS_RCODE_NOTIMP    4
#define DNS_RCODE_REFUSED   5

/* Record types and classes. Only A and CNAME are understood; everything else
*  is skipped by its length, which is why the length must be checked before
*  it is used rather than after. */
#define DNS_TYPE_A          1
#define DNS_TYPE_CNAME      5
#define DNS_CLASS_IN        1

/* The two high bits of a length byte, and what they mean. Exactly two of the
*  four combinations are defined:
*
*      00  an ordinary label, the low six bits are its length (0..63)
*      11  a compression pointer, the low six bits are the high six bits of
*          a 14 bit offset and the next byte is the low eight
*      01  reserved and never assigned
*      10  reserved; was proposed for the "extended label types" of RFC 2673,
*          which was moved to historic
*
*  01 and 10 are neither a label nor a pointer, and the only correct response
*  is to stop: guessing "it is probably a label" hands an attacker a length
*  byte of up to 191 for a 63 byte field, and guessing "it is probably a
*  pointer" hands them an offset with two bits of freedom nobody checks. */
#define DNS_LABEL_MASK     0xC0
#define DNS_LABEL_PLAIN    0x00
#define DNS_LABEL_POINTER  0xC0

/* The largest DNS message that travels over UDP without EDNS0. We never
*  announce a larger buffer -- there is no OPT record in our query -- so no
*  correct server sends us more than this, and a datagram that is larger is
*  not something to try to make sense of. */
#define DNS_MSG_MAX       512

/* ------------------------------------------------------------------ */
/* Bounds on the parser                                                */
/* ------------------------------------------------------------------ */

/* How many compression pointers one name may be assembled from.
*
*  This is the second of two independent guards, and the weaker one. The
*  first is in dns_name_decode() and is an invariant rather than a counter:
*  every pointer must aim strictly before the previous pointer's target, so
*  the targets form a strictly decreasing sequence of offsets inside a
*  message that is at most DNS_MSG_MAX bytes long. A strictly decreasing
*  sequence of unsigned values below 512 has at most 512 members, so the loop
*  terminates whatever the message says -- no counter needed, and no reliance
*  on a limit somebody might raise later.
*
*  The counter is here to bound the WORK rather than to guarantee the end of
*  it. 512 jumps per name, times the records a walk may touch, is more time
*  in an interrupt handler than a lookup is worth. 128 is above anything a
*  real message needs: a name is at most 255 bytes and a label costs at least
*  two of them, so no name has more than 127 labels, and every jump a real
*  compressor emits lands on a fragment that contributes at least one label
*  -- a compressor that already knows where a suffix lives points at the
*  suffix, not at another pointer. Replies in practice use one jump per name
*  and occasionally two. */
#define DNS_MAX_JUMPS     128

/* How many records of the answer section are examined. Bounded for the same
*  reason: the count in the header is a number an attacker writes, and while
*  the walk stops when it runs out of bytes, "runs out of bytes" is up to 42
*  minimal records in a 512 byte message and every one of them costs a name
*  decode. 32 A records is far more than any name resolves to. */
#define DNS_MAX_RECORDS    32

/* How many CNAMEs may be followed inside one reply. Aliases of aliases are
*  ordinary -- www.microsoft.com is two deep before it reaches an address --
*  but a chain longer than this in a single message is either a server with a
*  configuration loop or somebody making us walk in circles. Four is well
*  past what the web does. */
#define DNS_MAX_CNAMES      4

/* Retransmission. Three attempts, doubling: the query, then a resend one
*  second later, then another two seconds after that -- seven seconds of
*  patience in total.
*
*  Shorter than dhcp.c's fifteen on purpose. A DHCP client is waiting for a
*  broadcast to find a server that may be behind a relay on another subnet; a
*  stub resolver is talking to one address the lease already named, usually
*  the router in the same room. If that has not answered in seven seconds it
*  is not a slow answer, it is no answer, and a shell that appears hung for
*  fifteen seconds per typo is worse than one that says so quickly.
*
*  The doubling is what keeps the retries from becoming a burst at a server
*  that has already shown it is not listening. */
#define DNS_MAX_ATTEMPTS    3
#define DNS_FIRST_TIMEOUT   1000UL     /* milliseconds; doubles per attempt */

/* What happens when the query does not go out at all.
*
*  udp_send() fails while ARP is still resolving the next hop, and on the
*  first lookup after boot it always does: the server named in the lease is
*  on our own subnet, so it is ARPed for directly, and its cache entry can
*  only be filled by the request that the failed lookup just sent. Nothing
*  was transmitted, so nothing can answer, and treating that as an attempt
*  that went unanswered means waiting a full second for a reply to a datagram
*  that never left -- which is exactly what the first lookup after boot cost
*  before this existed.
*
*  So an unsent query is retried quickly and does not spend an attempt: an
*  ARP reply on a local segment comes back in a millisecond or two, and the
*  retransmission schedule is there to survive a lost datagram, not a
*  datagram that was never sent. The count is bounded all the same -- twenty
*  tries is a second of a next hop that will not resolve -- and once it is
*  used up the ordinary schedule takes over, so a permanently failing
*  transmit still ends in "no answer" rather than spinning. */
#define DNS_UNSENT_TIMEOUT    50UL     /* milliseconds between quick retries */
#define DNS_UNSENT_MAX        20

/* The furthest ahead a cache entry may be dated, for the same reason as the
*  lease clamp in dhcp.c: the millisecond clock wraps after about 25 days, so
*  two instants more than half its range apart cannot be ordered by an
*  unsigned difference. A TTL beyond this is kept at this. */
#define DNS_MAX_TTL_MS      0x7FFFFFFFUL
#define DNS_MAX_TTL_SECS    (DNS_MAX_TTL_MS / 1000UL)

/* The ephemeral port range, as IANA defines it: 49152..65535, 16384 ports.
*  Below it are the registered ports, where a listener on this machine might
*  reasonably want to sit, and picking out of it would eventually collide
*  with one. */
#define DNS_PORT_FIRST      49152U
#define DNS_PORT_COUNT      16384U

/* ------------------------------------------------------------------ */
/* Results of the answer walk                                          */
/* ------------------------------------------------------------------ */

/* Why a reply did or did not produce an address. Kept apart from the state
*  so that dns_receive() can record what it found without deciding what to
*  say about it -- the words belong in task context, next to the caller. */
#define DNS_ANS_OK          0   /* an A record for the name we asked about  */
#define DNS_ANS_TRUNCATED   1   /* TC set: the reply is a fragment          */
#define DNS_ANS_RCODE       2   /* the server said no, and why -- see rcode */
#define DNS_ANS_NODATA      3   /* no error, and no address either          */
#define DNS_ANS_CNAME       4   /* an alias whose target is not in the reply*/
#define DNS_ANS_MALFORMED   5   /* the message does not parse               */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* The query under construction.
*
*  Static, and deliberately not on the stack. The kernel stack is 4 KiB and
*  dns_poll() is reached from the shell, which is already several frames deep
*  and may take an interrupt on top of that; half a kilobyte of message
*  buffer per frame is how a stack overflow turns into memory corruption
*  somewhere else entirely. In .bss it is half a kilobyte of image that is
*  never in doubt.
*
*  It needs no busy flag. Only dns_resolve() and dns_poll() write it, both in
*  task context, and the receive handler -- the only thing that can preempt
*  them -- never builds a message. That is what the mailbox below is for. */
static uint8_t  dns_tx[DNS_MSG_MAX];

/* The question, encoded once when the lookup starts and copied into every
*  retransmission. A resend must ask the identical question: the reply to the
*  first attempt may be in flight right now, and it will be checked against
*  what we think we asked. */
static uint8_t  dns_qwire[DNS_NAME_MAX + 1];
static uint32_t dns_qwire_len = 0;

/* The name in text form, canonical: it is produced by decoding dns_qwire
*  rather than by copying the caller's string, so a trailing dot is gone and
*  anything the encoder would not accept never gets this far. It is what the
*  echoed question is compared against and what the cache is keyed on.
*
*  Written by dns_resolve() before the state becomes DNS_STATE_QUERY and read
*  by dns_receive() only while it is that -- and dns_resolve() refuses to
*  start while a lookup is in flight, so the two never overlap. */
static char     dns_query_name[DNS_NAME_MAX + 1];

static int      dns_current_state = DNS_STATE_IDLE;
static const char *dns_error = "";

/* Identity of the query in flight. All three are what a reply is matched
*  against; see dns_receive(). */
static uint32_t dns_server_addr = 0;
static uint16_t dns_id = 0;
static uint16_t dns_bound_port = 0;    /* 0 = nothing bound */

/* Retransmission bookkeeping, as in dhcp.c. dns_attempts counts queries that
*  actually reached the wire, which is why it starts at zero rather than at
*  one; dns_unsent_tries counts the ones that did not -- see
*  DNS_UNSENT_TIMEOUT. */
static uint32_t dns_sent_ms = 0;
static uint32_t dns_timeout_ms = 0;
static int      dns_attempts = 0;
static int      dns_unsent_tries = 0;

/* The answer, as dns.h promises it. */
static uint32_t dns_result_ip = 0;
static uint32_t dns_result_secs = 0;

/* Counters. */
static uint32_t dns_stat_sent    = 0;
static uint32_t dns_stat_used    = 0;
static uint32_t dns_stat_dropped = 0;

/* The mailbox between the interrupt handler and dns_poll(). One slot, and
*  the same handshake as dhcp.c uses: the handler fills it only while the
*  flag is 0 and sets the flag last, dns_poll() copies the struct out while
*  the flag is still 1 -- so the handler cannot be writing it -- and clears
*  the flag afterwards. Only the flag has to be volatile.
*
*  A second reply arriving before the first is collected is dropped. There is
*  nothing useful to do with it: we asked one question, and the first answer
*  that survived every check is the answer. */
typedef struct
{
    uint8_t  status;          /* DNS_ANS_*                                */
    uint8_t  rcode;           /* meaningful when status is DNS_ANS_RCODE  */
    uint32_t ip;              /* host order                               */
    uint32_t ttl;             /* seconds, as the server stated it         */
} dns_answer;

static dns_answer   dns_inbox;
static volatile int dns_inbox_full = 0;

/* Scratch for the receive path, and only for it.
*
*  Three names of 256 bytes each is three quarters of a kilobyte, which has
*  no business on an interrupt stack -- the same argument as for dns_tx, only
*  sharper, because the interrupt lands on top of whatever the shell was
*  already using. In .bss they cost nothing that can go wrong.
*
*  They need no lock. Interrupt gates clear IF, so an IRQ handler cannot be
*  preempted by another interrupt on this kernel, and dns_receive() is
*  reachable from nowhere else -- udp_receive() is its only caller and that
*  runs in the card's IRQ. One writer at a time is a property of the machine
*  here, not a hope. */
static char dns_rx_owner[DNS_NAME_MAX + 1];    /* owner name of a record   */
static char dns_rx_target[DNS_NAME_MAX + 1];   /* name we are looking for  */
static char dns_rx_cname[DNS_NAME_MAX + 1];    /* where an alias points    */

/* Scratch for task context, for the same reason: dns_resolve() and
*  dns_lookup_cached() both need to canonicalise a name and neither should
*  put 256 bytes on the shell's stack to do it. Task context is single
*  threaded in this kernel, so one buffer serves both. */
static char dns_scratch[DNS_NAME_MAX + 1];

/* ------------------------------------------------------------------ */
/* The cache                                                           */
/* ------------------------------------------------------------------ */

/* One remembered answer. added_ms is when it arrived, in the same
*  milliseconds-since-boot units as timer_get_ticks(), and ttl is what the
*  server said in seconds -- both are kept rather than a precomputed expiry,
*  because dns_cache_get() has to report how much of the TTL is left and that
*  is the difference of the two. */
typedef struct
{
    char     name[DNS_NAME_MAX + 1];
    uint32_t ip;
    uint32_t added_ms;
    uint32_t ttl;
    uint8_t  used;
} dns_cache_entry;

static dns_cache_entry dns_cache[DNS_CACHE_SIZE];

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Milliseconds since a snapshot of timer_get_ticks().
*
*  timer_get_ticks() returns int and wraps after roughly 25 days. Around the
*  wrap a signed "now - then" is nonsense -- negative, or a comparison that
*  flips -- and the resolver would either retransmit continuously or never
*  again. The subtraction on the unsigned bit patterns gives the correct
*  elapsed time straight through the wrap, which is what every caller here
*  means by "how long ago". */
static uint32_t dns_ms_since(uint32_t then)
{
    return (uint32_t)timer_get_ticks() - then;
}

/* Big endian reads out of an unaligned byte run. A DNS message is a byte
*  stream: a record's type field starts wherever the name in front of it
*  ended, so casting to a uint16_t* would be an alignment fault waiting for a
*  machine less forgiving than x86, as well as an endian bug. */
static uint16_t dns_get16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t dns_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* ASCII lower case, for name comparison only.
*
*  DNS names are case insensitive over ASCII letters and only over those:
*  RFC 4343 is explicit that a label is a string of octets and that the
*  insensitivity does not extend past A-Z. Folding by adding 0x20 to
*  "anything with the 0x40 bit" would fold punctuation and the high half of
*  the byte range as well, which would make two different names compare
*  equal. The range test is the whole of it. */
static char dns_lower(char c)
{
    if(c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');

    return c;
}

/* Compares two names for equality, ignoring case and one trailing dot on
*  either side -- "WWW.Example.COM." and "www.example.com" are the same name,
*  and a caller who types the dot should not miss the cache because of it.
*
*  Bounded by the longest legal name rather than by the terminators: both
*  arguments are either produced by the decoder below, which terminates what
*  it writes, or handed in by a caller, and a caller who passes an
*  unterminated buffer gets a mismatch instead of a walk through memory. The
*  bound is one short of DNS_NAME_MAX so that the look ahead at [i + 1] --
*  which is what recognises a trailing dot -- stays inside a buffer of
*  DNS_NAME_MAX + 1 bytes even when nothing in it terminates. */
static int dns_name_equal(const char *a, const char *b)
{
    uint32_t i;
    char ca;
    char cb;

    if(a == 0 || b == 0)
        return 0;

    for(i = 0; i < DNS_NAME_MAX; i++)
    {
        ca = dns_lower(a[i]);
        cb = dns_lower(b[i]);

        /* A trailing dot is the root label written out and means nothing
        *  else; one name may have it and the other not. */
        if(ca == '.' && a[i + 1] == '\0')
            ca = '\0';
        if(cb == '.' && b[i + 1] == '\0')
            cb = '\0';

        if(ca != cb)
            return 0;

        if(ca == '\0')
            return 1;
    }

    return 0;   /* neither ended inside the longest legal name */
}

/* Copies a name into a buffer of a stated size, always terminating and
*  truncating rather than overflowing. This is what <string.h> does not have
*  and the reason nothing here calls strcpy(). */
static void dns_name_copy(char *dst, uint32_t size, const char *src)
{
    uint32_t i;

    if(dst == 0 || size == 0)
        return;

    for(i = 0; i + 1 < size && src[i] != '\0'; i++)
        dst[i] = src[i];

    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Names: encoding                                                     */
/* ------------------------------------------------------------------ */

/* Turns "www.example.com" into 3 w w w 7 e x a m p l e 3 c o m 0.
*
*  The easy direction, but not a memcpy with dots replaced by counts, because
*  the limits are real and a name that violates one is a name no server will
*  answer -- better refused here, where dns_resolve() can say so, than sent
*  and timed out on:
*
*    - a label is 1..63 bytes. 0 is the root and cannot appear in the middle;
*      "a..b" has an empty label and is not a name with an empty piece in it,
*      it is not a name.
*    - the encoded form is at most 255 bytes INCLUDING the length byte of
*      every label and the zero that ends it, which is why the check below
*      counts 1 + len per label and reserves the final byte.
*    - a trailing dot is legal and means the same name: it is how the root is
*      written, so "example.com." ends after "com" exactly as "example.com"
*      does. A leading dot is not the same thing -- it is an empty first
*      label -- and is refused.
*
*  Bytes outside printable ASCII are refused. The protocol permits any octet
*  in a label, but this resolver is reached from a shell where such a name
*  cannot be typed, so a control byte in the input is a bug in the caller
*  rather than a hostname, and refusing it keeps anything unprintable out of
*  the cache listing.
*
*  Returns 0 and the encoded length, or -1. Nothing is written past out_size
*  on any path, including the failing ones.
*
*  How far this reads. "name" is an ordinary C string and the caller owes it
*  a terminator, as everywhere in C -- but "owes" is not "provides", so the
*  scan below fixes the reach of this function before anything else happens:
*  it looks at index 0 through DNS_NAME_MAX and no further, and refuses the
*  name if no terminator is in there. Every loop afterwards can then trust
*  that one exists. A buffer of 256 bytes with nothing terminating it is
*  therefore refused rather than walked into whatever follows it in memory,
*  which is the case the shell could actually produce with a long enough
*  line, and it is why no later loop needs a bound of its own. */
static int dns_name_encode(const char *name, uint8_t *out, uint32_t out_size,
                           uint32_t *written)
{
    uint32_t i;
    uint32_t j;
    uint32_t at;
    uint32_t start;
    uint32_t len;

    if(name == 0 || out == 0 || out_size == 0)
        return -1;

    for(i = 0; i <= DNS_NAME_MAX; i++)
        if(name[i] == '\0')
            break;

    if(i > DNS_NAME_MAX)
        return -1;                       /* longer than any legal name    */

    /* The empty name and the bare root are both "no name to look up". */
    if(name[0] == '\0')
        return -1;
    if(name[0] == '.' && name[1] == '\0')
        return -1;

    i = 0;
    at = 0;

    while(name[i] != '\0')
    {
        start = i;

        while(name[i] != '\0' && name[i] != '.')
        {
            if(name[i] < '!' || name[i] > '~')
                return -1;

            i++;
        }

        len = i - start;

        if(len == 0)
            return -1;                   /* "a..b", or a leading dot      */
        if(len > DNS_LABEL_MAX)
            return -1;

        /* 1 length byte, len bytes of label, and the root byte that will
        *  end the name: all three have to fit in both limits. */
        if(at + 1 + len + 1 > out_size)
            return -1;
        if(at + 1 + len + 1 > DNS_NAME_MAX)
            return -1;

        out[at++] = (uint8_t)len;
        for(j = 0; j < len; j++)
            out[at + j] = (uint8_t)name[start + j];
        at += len;

        if(name[i] == '.')
        {
            i++;
            if(name[i] == '\0')
                break;                   /* trailing dot: the name ends   */
        }
    }

    out[at++] = 0;                       /* the root label                */

    if(written != 0)
        *written = at;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Names: decoding                                                     */
/* ------------------------------------------------------------------ */

/* Reads the name at "at" out of the message [msg, msg+len), writing its text
*  form into out (or nowhere, if out is 0) and reporting in *next the offset
*  of the first byte AFTER the name as it appears at "at".
*
*  Two return values, and the difference matters: 0 means the name is sound
*  and *next may be used to continue the walk; -1 means the message is
*  malformed and NOTHING about it may be believed afterwards, including *next
*  -- a caller that carries on from a guessed offset is parsing rubbish.
*
*  This is the function the whole file is built around, so its guarantees are
*  worth stating one at a time.
*
*  TERMINATION. A pointer sends the cursor backwards to an offset the sender
*  chose, so nothing about the message itself makes this loop end. Two rules
*  make it end regardless:
*
*    (a) A pointer must aim strictly before the pointer itself. That alone
*        kills the self-pointer -- the two byte name that says "continue at
*        me" and hangs a naive decoder on its first message.
*    (b) A pointer must aim strictly before the PREVIOUS pointer's target.
*        The targets therefore form a strictly decreasing sequence of
*        unsigned offsets, all below len, so there can be at most len of
*        them: the loop ends after at most 512 jumps whatever the message
*        says. This is what (a) alone does not give. Two pointers can each
*        aim backwards and still form a cycle -- a pointer at 300 aiming at
*        100, whose labels run forward to another pointer at 400 aiming at
*        200, whose labels run forward to 300 again -- and every jump in that
*        cycle goes backwards. Only the decreasing rule breaks it.
*
*        And it refuses no legal message. Compression exists to point at a
*        name that was already written earlier in the message; if the name at
*        that earlier offset is itself compressed, the fragment IT points at
*        was written earlier still, because a compressor cannot point at
*        something it has not emitted yet. Strictly decreasing targets is
*        exactly the shape a correct compressor produces.
*
*    The jump counter is a third guard on top of both, bounding the work
*    rather than proving the end of it -- see DNS_MAX_JUMPS.
*
*  BOUNDS. Every byte is proved to be inside the message before it is read:
*  the length byte by "here >= len", the second half of a pointer by
*  "here + 1 >= len", and a label's body by "here + 1 + b > len". A pointer's
*  target is below the pointer's own offset and therefore inside the message
*  by construction. None of those sums can overflow -- len is at most 512, an
*  offset is below it and a label length is a single byte.
*
*  A pointer that lands in the middle of a length byte, or in the middle of a
*  record's rdata, needs no special case and gets none: whatever byte is
*  there is read as a length byte or a pointer, and it either forms a sound
*  name from that offset or fails one of the tests here. There is no way to
*  ask the message "is this offset the start of a name", and inventing an
*  answer would be a guess.
*
*  LENGTH. The 255 byte limit is on the ASSEMBLED name, not on the pieces, so
*  it cannot be checked by looking at any single label: a hundred legal 60
*  byte labels reached through a hundred legal pointers is a legal message
*  and an illegal name. wire counts what the name would occupy uncompressed
*  -- one byte per label length plus the label, plus the root byte -- which
*  is the form the limit is written in.
*
*  *next is set from the first pointer if there is one, and only from it: the
*  name occupies two bytes at "at" in that case, and everything the cursor
*  visits afterwards belongs to some other name. Where there is no pointer it
*  is the byte after the terminating zero. Getting this wrong does not
*  corrupt anything here, it desynchronises the record walk -- and a record
*  walk reading a type field out of the middle of a name will find records
*  that were never sent.
*/
static int dns_name_decode(const uint8_t *msg, uint32_t len, uint32_t at,
                           char *out, uint32_t out_size, uint32_t *next)
{
    uint32_t here;
    uint32_t limit;
    uint32_t target;
    uint32_t wire;
    uint32_t written;
    uint32_t i;
    int      jumps;
    int      followed;
    uint8_t  b;

    if(msg == 0 || at >= len)
        return -1;
    if(out != 0 && out_size == 0)
        return -1;

    here     = at;
    limit    = len;      /* no pointer has been followed yet */
    wire     = 1;        /* the root byte is part of every name */
    written  = 0;
    jumps    = 0;
    followed = 0;

    if(next != 0)
        *next = at;

    for(;;)
    {
        if(here >= len)
            return -1;

        b = msg[here];

        if((b & DNS_LABEL_MASK) == DNS_LABEL_PLAIN)
        {
            if(b == 0)
            {
                /* The root label ends the name. */
                if(!followed && next != 0)
                    *next = here + 1;

                if(out != 0)
                    out[written] = '\0';

                return 0;
            }

            /* The label's body must be inside the message. */
            if(here + 1 + (uint32_t)b > len)
                return -1;

            wire += 1 + (uint32_t)b;
            if(wire > DNS_NAME_MAX)
                return -1;

            if(out != 0)
            {
                if(written > 0)
                {
                    if(written + 1 >= out_size)
                        return -1;
                    out[written++] = '.';
                }

                if(written + (uint32_t)b >= out_size)
                    return -1;

                for(i = 0; i < (uint32_t)b; i++)
                    out[written + i] = (char)msg[here + 1 + i];

                written += (uint32_t)b;
            }

            here += 1 + (uint32_t)b;
            continue;
        }

        if((b & DNS_LABEL_MASK) == DNS_LABEL_POINTER)
        {
            if(here + 1 >= len)
                return -1;               /* half a pointer at the end     */

            target = (((uint32_t)b & 0x3F) << 8) | (uint32_t)msg[here + 1];

            /* The name at "at" is two bytes long in this case, and this is
            *  the last moment at which that is known. */
            if(!followed && next != 0)
                *next = here + 2;
            followed = 1;

            if(target >= here)
                return -1;               /* rule (a): strictly backwards  */
            if(target >= limit)
                return -1;               /* rule (b): strictly decreasing */

            jumps++;
            if(jumps > DNS_MAX_JUMPS)
                return -1;

            limit = target;
            here  = target;
            continue;
        }

        /* 01 and 10: not a label, not a pointer, nothing to do but stop. */
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* The transaction id and the source port                              */
/* ------------------------------------------------------------------ */

/* Where unpredictable numbers come from in a kernel that has none.
*
*  dhcp.c faced the same problem and reached a different conclusion, and the
*  difference is the point. A DHCP transaction id only has to be UNIQUE: it
*  labels a conversation on a wire where every answer is broadcast, and an
*  attacker who wants to forge an OFFER can simply read the id off the
*  DISCOVER they just saw. Unpredictability would buy nothing there.
*
*  Here it buys the only defence there is. A DNS reply is accepted on the
*  strength of matching an id, a port, an address and a question, and the
*  attacker this defends against is the one who CANNOT see the query -- the
*  off-path forger who has to guess all four and get an answer in before the
*  real server does. Every bit of the id and the port they cannot predict
*  doubles the number of packets they must send. So the same mixer is used
*  but the reasoning about what it must produce is stronger, and it is worth
*  being plain that this falls short of it: the material below is a MAC
*  address an attacker on the segment already knows, a wall clock they can
*  guess to the second, a tick counter that is the uptime of the machine, and
*  a counter. Against an off-path attacker the tick counter is the part that
*  actually hides -- the millisecond at which a user typed a command -- and
*  the rest mostly separates one lookup from the next.
*
*  That is 16 bits of id and 14 bits of port, so about 30 bits of matching
*  material, none of it from a real entropy source. It raises the bar a long
*  way above a fixed id and a fixed port, which is what this replaces, and it
*  is not cryptography. When this kernel grows a random source, this function
*  is the one place to change.
*
*  The salt is what makes the id and the port independent rather than two
*  views of the same number: they are drawn from the same inputs, and without
*  it, guessing one would hand the attacker the other.
*
*  The pieces are multiplied into different parts of the word before being
*  xored, not xored raw: a MAC and a small clock reading both carry most of
*  their variation in the low bits, and xoring them as they are would pile it
*  into the same place and cancel. The constant is Knuth's odd multiplier for
*  32 bit hashing, which spreads a change in any input bit across the word. */
static uint32_t dns_mix(uint32_t salt)
{
    static uint32_t sequence = 0;
    const uint8_t *mac;
    datetime now;
    uint32_t x;
    uint32_t clock;

    mac = net_mac();
    now = cmos_readtime();

    x = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
        ((uint32_t)mac[4] <<  8) |  (uint32_t)mac[5];

    clock = ((uint32_t)now.years  * 32UL + (uint32_t)now.months) * 32UL;
    clock = (clock + (uint32_t)now.days) * 24UL + (uint32_t)now.hours;
    clock = (clock * 60UL + (uint32_t)now.minutes) * 60UL + (uint32_t)now.seconds;

    x ^= clock * 2654435761UL;
    x ^= (uint32_t)timer_get_ticks() * 40503UL;
    x ^= (++sequence) * 2654435761UL;
    x ^= salt * 2246822519UL;

    /* One more round of mixing, so that the low bits -- which is all the
    *  port selection below keeps -- depend on the whole word rather than on
    *  the low bits of the inputs. */
    x ^= x >> 15;
    x *= 2246822519UL;
    x ^= x >> 13;

    return x;
}

/* An id may be any of the 65536 values. Unlike DHCP's xid, zero is not worth
*  avoiding: an id is never believed on its own here -- it is believed
*  together with the port, the server's address and the echoed question -- so
*  a zero id says nothing suspicious, and refusing it would only shrink the
*  space an attacker has to search. */
static uint16_t dns_make_id(void)
{
    return (uint16_t)(dns_mix(0x1D) & 0xFFFFUL);
}

static uint16_t dns_make_port(void)
{
    return (uint16_t)(DNS_PORT_FIRST + (dns_mix(0x50) % DNS_PORT_COUNT));
}

/* ------------------------------------------------------------------ */
/* The cache                                                           */
/* ------------------------------------------------------------------ */

/* How much of an entry's TTL is left, in seconds, and 0 when it has run out.
*
*  The age is an unsigned difference of two millisecond readings, for the
*  reason given at dns_ms_since(): computed any other way, every entry in the
*  cache becomes either immortal or instantly stale once the clock wraps. */
static uint32_t dns_cache_left(const dns_cache_entry *e)
{
    uint32_t age;

    age = dns_ms_since(e->added_ms) / 1000UL;

    if(age >= e->ttl)
        return 0;

    return e->ttl - age;
}

/* Finds a live entry for a name, or -1. An expired entry is not a hit and is
*  left where it is: it will be reused by the next store, and clearing it
*  here would mean writing the cache from whatever context asked a question
*  of it. */
static int dns_cache_find(const char *name)
{
    int i;

    for(i = 0; i < DNS_CACHE_SIZE; i++)
    {
        if(!dns_cache[i].used)
            continue;
        if(dns_cache_left(&dns_cache[i]) == 0)
            continue;
        if(dns_name_equal(dns_cache[i].name, name))
            return i;
    }

    return -1;
}

/* Remembers an answer.
*
*  A TTL of zero is an instruction and not a rounding error: it means "use
*  this once and do not remember it", and it is how a load balancer that
*  hands out a different address per query says so. Storing it anyway --
*  even for a second -- is the difference between following the server and
*  overriding it.
*
*  Which slot: the one already holding this name, so a fresh answer replaces
*  a stale one rather than sitting beside it; then a free slot; then an
*  expired one; and failing all of those, the entry with the least time left,
*  which is the one whose loss costs the least. */
static void dns_cache_store(const char *name, uint32_t ip, uint32_t ttl)
{
    int i;
    int slot;
    uint32_t left;
    uint32_t worst;

    if(ttl == 0 || ip == 0)
        return;

    if(ttl > DNS_MAX_TTL_SECS)
        ttl = DNS_MAX_TTL_SECS;

    slot = -1;

    for(i = 0; i < DNS_CACHE_SIZE; i++)
        if(dns_cache[i].used && dns_name_equal(dns_cache[i].name, name))
        {
            slot = i;
            break;
        }

    if(slot < 0)
        for(i = 0; i < DNS_CACHE_SIZE; i++)
            if(!dns_cache[i].used || dns_cache_left(&dns_cache[i]) == 0)
            {
                slot = i;
                break;
            }

    if(slot < 0)
    {
        worst = 0xFFFFFFFFUL;
        slot = 0;

        for(i = 0; i < DNS_CACHE_SIZE; i++)
        {
            left = dns_cache_left(&dns_cache[i]);
            if(left < worst)
            {
                worst = left;
                slot = i;
            }
        }
    }

    dns_name_copy(dns_cache[slot].name, sizeof(dns_cache[slot].name), name);
    dns_cache[slot].ip = ip;
    dns_cache[slot].ttl = ttl;
    dns_cache[slot].added_ms = (uint32_t)timer_get_ticks();
    dns_cache[slot].used = 1;
}

/* ------------------------------------------------------------------ */
/* Building and sending a query                                        */
/* ------------------------------------------------------------------ */

/* One question, recursion desired, nothing else.
*
*  RD is what makes this a stub resolver: it asks the server named in the
*  lease to do the walk from the root on our behalf and hand back the answer.
*  Without it a server that is willing to recurse answers with a referral to
*  the next name server down, and following referrals is a resolver rather
*  than a client -- a great deal more code, and a different program.
*
*  No OPT record, so no EDNS0, so no reply larger than 512 bytes: that is the
*  deliberate consequence described at DNS_MSG_MAX. Announcing a larger
*  buffer we do not have would invite exactly the datagram we cannot hold. */
static int dns_send_query(void)
{
    uint32_t at;

    if(dns_qwire_len == 0 || dns_server_addr == 0)
        return -1;

    memset((char *)dns_tx, 0, DNS_MSG_MAX);

    dns_tx[0] = (uint8_t)(dns_id >> 8);
    dns_tx[1] = (uint8_t)(dns_id & 0xFF);
    dns_tx[2] = (uint8_t)(DNS_FLAG_RD >> 8);
    dns_tx[3] = (uint8_t)(DNS_FLAG_RD & 0xFF);
    dns_tx[4] = 0;                     /* qdcount high */
    dns_tx[5] = 1;                     /* one question */
    /* ancount, nscount and arcount stay zero -- memset did that. */

    at = DNS_HDR_LEN;

    /* The buffer is 512 bytes and the name is at most 255, so this cannot
    *  overflow; the test is here so that a later edit which changes either
    *  number fails loudly rather than quietly. */
    if(at + dns_qwire_len + 4 > DNS_MSG_MAX)
        return -1;

    memcpy(dns_tx + at, dns_qwire, (size_t)dns_qwire_len);
    at += dns_qwire_len;

    dns_tx[at++] = 0;                  /* qtype  = A  */
    dns_tx[at++] = DNS_TYPE_A;
    dns_tx[at++] = 0;                  /* qclass = IN */
    dns_tx[at++] = DNS_CLASS_IN;

    return udp_send(dns_server_addr, dns_bound_port, DNS_PORT_SERVER,
                    dns_tx, at);
}

/* ------------------------------------------------------------------ */
/* Reading a reply -- interrupt context                                */
/* ------------------------------------------------------------------ */

/* Walks the answer section looking for an address for dns_rx_target,
*  following aliases as it goes. Returns a DNS_ANS_* code.
*
*  A CNAME chain is the ordinary case and not an exception. A server asked
*  for a name that is an alias answers with the CNAME and, almost always, the
*  A record of what it points at, in the same message and in the same
*  section: www.microsoft.com arrives as two CNAMEs and one A. So the walk
*  cannot simply take the first A record it sees -- the records are a set,
*  not a sequence, and the A in a chained reply belongs to the END of the
*  chain. Taking any A record in the message would mean accepting an address
*  for a name nobody asked about, which is precisely the shape of a cache
*  poisoning attack: attach an A record for a name of your choosing to an
*  answer somebody is expecting.
*
*  So each pass looks for an A record whose OWNER is the name currently being
*  chased, and only if there is none does it look for a CNAME with that owner
*  and chase what it points at. An A record wins over a CNAME within a pass,
*  because the records may arrive in any order and a server that sends both
*  has answered the question directly.
*
*  When the chain ends at a name with no A record in the message, that is
*  DNS_ANS_CNAME and the lookup fails with the reason said out loud. The
*  alternative would be to send a second query for the target, which is
*  correct and is what a full resolver does -- but it turns one lookup into a
*  chain of them, each with its own timeout and its own retransmissions, and
*  a recursive server that omits the target's A record is a server that has
*  not finished the job it was asked to do. Reporting it is more useful than
*  quietly working around it.
*
*  The TTL reported is the SMALLEST along the chain, not the A record's. An
*  answer is only valid while every link that leads to it is: caching the
*  address for an hour because the A record said so, when the CNAME that
*  points at it expires in thirty seconds, keeps an answer that has stopped
*  being true.
*
*  Bounds: ancount is a number from the wire and is capped at
*  DNS_MAX_RECORDS; every name is decoded through dns_name_decode(), which
*  fails rather than running away; the ten fixed bytes of a record are proved
*  to be inside the message before they are read; and rdlength is proved to
*  fit before it is used to skip or to read an address. A record that fails
*  any of those stops the walk -- once a length is wrong there is no way to
*  know where the next record starts, so continuing would be parsing
*  whatever happens to be there. */
static int dns_walk_answers(const uint8_t *msg, uint32_t len, uint32_t at,
                            uint32_t ancount, uint32_t *ip, uint32_t *ttl)
{
    uint32_t cursor;
    uint32_t rdata;
    uint32_t i;
    uint32_t round;
    uint32_t type;
    uint32_t class;
    uint32_t rdlen;
    uint32_t rr_ttl;
    uint32_t best_ttl;
    int      have_cname;

    best_ttl = 0xFFFFFFFFUL;

    if(ancount > DNS_MAX_RECORDS)
        ancount = DNS_MAX_RECORDS;

    for(round = 0; round <= DNS_MAX_CNAMES; round++)
    {
        cursor = at;
        have_cname = 0;

        for(i = 0; i < ancount; i++)
        {
            if(dns_name_decode(msg, len, cursor, dns_rx_owner,
                               sizeof(dns_rx_owner), &cursor) < 0)
                return DNS_ANS_MALFORMED;

            /* type, class, ttl, rdlength: ten bytes, and the record is not
            *  a record without them. */
            if(cursor + 10 > len)
                return DNS_ANS_MALFORMED;

            type   = (uint32_t)dns_get16(msg + cursor);
            class  = (uint32_t)dns_get16(msg + cursor + 2);
            rr_ttl = dns_get32(msg + cursor + 4);
            rdlen  = (uint32_t)dns_get16(msg + cursor + 8);
            cursor += 10;

            if(cursor + rdlen > len)
                return DNS_ANS_MALFORMED;

            rdata = cursor;
            cursor += rdlen;             /* every unknown type is skipped */

            /* The top bit of a TTL is reserved and a value with it set is to
            *  be treated as zero -- "do not cache" -- rather than as a
            *  lifetime of 68 years. */
            if(rr_ttl > DNS_MAX_TTL_SECS)
                rr_ttl = 0;

            if(class != DNS_CLASS_IN)
                continue;                /* CHAOS and HESIOD are not ours */

            if(type == DNS_TYPE_A && rdlen == 4 &&
               dns_name_equal(dns_rx_owner, dns_rx_target))
            {
                *ip = dns_get32(msg + rdata);

                if(rr_ttl < best_ttl)
                    best_ttl = rr_ttl;
                *ttl = best_ttl;

                /* 0.0.0.0 is a legal 32 bit value and not an address a name
                *  resolves to; a server offering it is answering with the
                *  one number that means "none". */
                if(*ip == IP_ADDR_ANY)
                    return DNS_ANS_NODATA;

                return DNS_ANS_OK;
            }

            if(type == DNS_TYPE_CNAME && !have_cname &&
               dns_name_equal(dns_rx_owner, dns_rx_target))
            {
                /* The rdata of a CNAME is a name like any other and may be
                *  compressed, so it is decoded from its own offset rather
                *  than copied. It is not followed yet: an A record for the
                *  current name may still be further down this section, and
                *  it would answer the question directly. */
                if(dns_name_decode(msg, len, rdata, dns_rx_cname,
                                   sizeof(dns_rx_cname), 0) < 0)
                    return DNS_ANS_MALFORMED;

                if(rr_ttl < best_ttl)
                    best_ttl = rr_ttl;

                have_cname = 1;
            }
        }

        if(!have_cname)
        {
            /* Nothing in this section is about the name we are chasing.
            *  Either the reply carried no answers at all -- a name that
            *  exists with no address of this type, which the header calls
            *  NODATA -- or it carried answers about something else. */
            if(round == 0)
                return DNS_ANS_NODATA;

            return DNS_ANS_CNAME;
        }

        /* Follow the alias and go round again. */
        dns_name_copy(dns_rx_target, sizeof(dns_rx_target), dns_rx_cname);
    }

    /* More links than DNS_MAX_CNAMES: a loop, or a chain nobody sane built. */
    return DNS_ANS_CNAME;
}

/* Called by the UDP layer for every datagram that reaches our port, from the
*  card's interrupt handler.
*
*  Everything before the walk is the decision to believe the datagram at all,
*  and it is worth listing what has to be true at once, because each of them
*  is a number an off-path attacker has to guess:
*
*    - We are waiting for an answer. Outside DNS_STATE_QUERY there is no
*      question outstanding and nothing that could be an answer to it.
*    - It arrived at the port we are listening on. That is not tested here
*      because it cannot be false: udp_receive() dispatches on the
*      destination port and this handler is bound to exactly one, which
*      dns_resolve() draws afresh for every lookup. A datagram to any other
*      port never reaches this function.
*    - It came from the server we asked, at port 53. Not "some DNS server":
*      the address out of the lease, the one the query went to.
*    - The id matches the one we sent.
*    - It is a response (QR set) to a standard query (opcode 0). A query
*      arriving at our ephemeral port is somebody else's confusion.
*    - There is exactly one question and it is OUR question: the name decodes
*      to the name we asked about, ignoring case as DNS does, and the type
*      and class are the A and IN we sent. This is the check that costs an
*      attacker the most, because they have to know which name is being
*      looked up, and it is the one a resolver that trusts the id alone
*      leaves out.
*
*  Anything that fails one of those is dropped without a word and counted.
*  Nothing is printed, on any path: a handler that printed could be made to
*  scroll the console by anyone able to send a datagram to this machine, and
*  it would land in the middle of whatever the shell was saying. Every
*  outcome is visible through dns_state() and dns_last_error() instead.
*
*  Nothing that points into "data" outlives this call. The buffer belongs to
*  the driver's receive ring and is reused the moment we return; what is kept
*  is four numbers in the mailbox. */
static void dns_receive(uint32_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint32_t len)
{
    dns_answer a;
    uint32_t qend;
    uint32_t ancount;
    uint16_t flags;
    uint16_t qtype;
    uint16_t qclass;

    if(dns_current_state != DNS_STATE_QUERY)
    {
        dns_stat_dropped++;
        return;
    }

    if(src_port != DNS_PORT_SERVER || src_ip != dns_server_addr)
    {
        dns_stat_dropped++;
        return;
    }

    /* Too short to hold a header, or larger than anything we could have
    *  provoked -- we announced no EDNS0 buffer, so 512 is the ceiling. */
    if(len < DNS_HDR_LEN || len > DNS_MSG_MAX)
    {
        dns_stat_dropped++;
        return;
    }

    if(dns_get16(data) != dns_id)
    {
        dns_stat_dropped++;
        return;
    }

    flags = dns_get16(data + 2);

    if((flags & DNS_FLAG_QR) == 0 ||
       (flags & DNS_FLAG_OPCODE) != DNS_OPCODE_QUERY)
    {
        dns_stat_dropped++;
        return;
    }

    /* We sent one question and a reply echoes what it was asked. Walking the
    *  question section is how the answer section is found, so this is not
    *  only a check -- it is the only way to know where the records start,
    *  and the length must not be assumed to be the length we sent even
    *  though it should be. */
    if(dns_get16(data + 4) != 1)
    {
        dns_stat_dropped++;
        return;
    }

    if(dns_name_decode(data, len, DNS_HDR_LEN, dns_rx_owner,
                       sizeof(dns_rx_owner), &qend) < 0)
    {
        dns_stat_dropped++;
        return;
    }

    if(qend + 4 > len)
    {
        dns_stat_dropped++;
        return;
    }

    qtype  = dns_get16(data + qend);
    qclass = dns_get16(data + qend + 2);
    qend += 4;

    if(qtype != DNS_TYPE_A || qclass != DNS_CLASS_IN ||
       !dns_name_equal(dns_rx_owner, dns_query_name))
    {
        dns_stat_dropped++;
        return;
    }

    /* The previous reply has not been collected yet. Drop this one rather
    *  than write over a record dns_poll() may be reading; we asked once, so
    *  a second answer has nothing to add. */
    if(dns_inbox_full)
    {
        dns_stat_dropped++;
        return;
    }

    a.status = DNS_ANS_MALFORMED;
    a.rcode  = (uint8_t)(flags & DNS_FLAG_RCODE);
    a.ip     = 0;
    a.ttl    = 0;

    if((flags & DNS_FLAG_TC) != 0)
    {
        /* The reply did not fit and the protocol's answer is to ask again
        *  over TCP. There is no TCP here, and what arrived is the BEGINNING
        *  of an answer -- the records that did fit, in the order the server
        *  happened to write them. Using them would mean reporting one
        *  address out of a set whose other members were cut off, or
        *  reporting "no such name" for a name whose answer was simply too
        *  long. Truncated is a different outcome from either, and it is
        *  reported as itself. */
        a.status = DNS_ANS_TRUNCATED;
    }
    else if(a.rcode != DNS_RCODE_NOERROR)
    {
        a.status = DNS_ANS_RCODE;
    }
    else
    {
        ancount = (uint32_t)dns_get16(data + 6);

        dns_name_copy(dns_rx_target, sizeof(dns_rx_target), dns_query_name);
        a.status = (uint8_t)dns_walk_answers(data, len, qend, ancount,
                                             &a.ip, &a.ttl);
    }

    dns_stat_used++;

    dns_inbox = a;
    dns_inbox_full = 1;        /* written last: see the comment on the flag */
}

/* ------------------------------------------------------------------ */
/* State machine -- task context                                       */
/* ------------------------------------------------------------------ */

/* What a response code means, in the words the shell shows. A table rather
*  than a formatted number, because there is nothing in this file that may
*  print and no formatter that does not use a shared static buffer. */
static const char *dns_rcode_text(uint8_t rcode)
{
    switch(rcode)
    {
    case DNS_RCODE_FORMERR:
        return "the server could not parse the query (FORMERR)";
    case DNS_RCODE_SERVFAIL:
        return "the DNS server failed (SERVFAIL)";
    case DNS_RCODE_NXDOMAIN:
        return "no such name (NXDOMAIN)";
    case DNS_RCODE_NOTIMP:
        return "the server does not implement this query (NOTIMP)";
    case DNS_RCODE_REFUSED:
        return "the server refused the query (REFUSED)";
    case 6:  return "the name exists and should not (YXDOMAIN)";
    case 7:  return "the record set exists and should not (YXRRSET)";
    case 8:  return "the record set does not exist (NXRRSET)";
    case 9:  return "the server is not authoritative for the zone (NOTAUTH)";
    case 10: return "the name is outside the zone (NOTZONE)";
    default: return "the server answered with an unknown response code";
    }
}

/* Arms the retransmit timer for what just happened. "sent" says whether the
*  datagram actually went to the card; see DNS_UNSENT_TIMEOUT for why the two
*  cases are not the same thing. */
static void dns_arm_timer(int sent)
{
    dns_sent_ms = (uint32_t)timer_get_ticks();

    if(!sent && dns_unsent_tries < DNS_UNSENT_MAX)
    {
        dns_unsent_tries++;
        dns_timeout_ms = DNS_UNSENT_TIMEOUT;
        return;                          /* no attempt was spent */
    }

    if(dns_attempts == 0)
    {
        dns_attempts = 1;
        dns_timeout_ms = DNS_FIRST_TIMEOUT;
    }
    else
    {
        dns_attempts++;
        dns_timeout_ms *= 2;
    }
}

/* Puts the query on the wire and arms the timer to match. The one place that
*  sends, so the counter and the timer cannot disagree about whether anything
*  left the machine. */
static void dns_transmit(void)
{
    int sent;

    sent = (dns_send_query() == 0);

    if(sent)
    {
        dns_stat_sent++;
        dns_unsent_tries = 0;
    }

    dns_arm_timer(sent);
}

static void dns_fail(const char *why)
{
    dns_current_state = DNS_STATE_FAILED;
    dns_error = why;
    dns_result_ip = 0;
    dns_result_secs = 0;
}

/* Acts on a reply that dns_receive() left in the mailbox. Task context, so
*  this is where the cache is written and where the words are chosen. */
static void dns_handle_answer(const dns_answer *a)
{
    switch(a->status)
    {
    case DNS_ANS_OK:
        dns_result_ip = a->ip;
        dns_result_secs = a->ttl;
        dns_current_state = DNS_STATE_DONE;
        dns_error = "";

        /* A TTL of zero means the server does not want this remembered, and
        *  dns_cache_store() honours it. */
        dns_cache_store(dns_query_name, a->ip, a->ttl);
        break;

    case DNS_ANS_TRUNCATED:
        dns_fail("the reply was truncated and there is no TCP to retry on");
        break;

    case DNS_ANS_RCODE:
        dns_fail(dns_rcode_text(a->rcode));
        break;

    case DNS_ANS_NODATA:
        dns_fail("the name has no address record");
        break;

    case DNS_ANS_CNAME:
        dns_fail("the name is an alias and the reply carried no address for it");
        break;

    default:
        dns_fail("the reply could not be parsed");
        break;
    }
}

int dns_poll(void)
{
    dns_answer a;

    /* Anything in the mailbox is acted on first: an answer that has already
    *  arrived makes the timer below irrelevant. */
    if(dns_inbox_full)
    {
        a = dns_inbox;          /* safe: the handler does not write while
                                *  the flag is set */
        dns_inbox_full = 0;

        if(dns_current_state == DNS_STATE_QUERY)
            dns_handle_answer(&a);

        return dns_current_state;
    }

    if(dns_current_state != DNS_STATE_QUERY)
        return dns_current_state;

    if(dns_ms_since(dns_sent_ms) < dns_timeout_ms)
        return dns_current_state;

    if(dns_attempts >= DNS_MAX_ATTEMPTS)
    {
        dns_fail("no answer from the DNS server");
        return dns_current_state;
    }

    /* Resend the identical question with the identical id and from the
    *  identical port. This is a retransmission of one query, not a new one:
    *  the answer to the first attempt may be on the wire at this moment, and
    *  a resolver that renumbered on every retry would be unable to recognise
    *  it -- getting slower exactly as the network got worse. */
    dns_transmit();

    return dns_current_state;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

int dns_init(void)
{
    if(!net_up())
    {
        dns_error = "no network card";
        return -1;
    }

    if(dns_bound_port != 0)
        return 0;                        /* already listening */

    dns_bound_port = dns_make_port();

    if(udp_bind(dns_bound_port, dns_receive) < 0)
    {
        dns_bound_port = 0;
        dns_error = "no free UDP binding for the resolver";
        return -1;
    }

    return 0;
}

/* Moves the resolver to a freshly drawn source port.
*
*  A fixed client port would mean an off-path attacker had only the id to
*  guess -- 16 bits, and a few thousand forged datagrams. Drawing a new port
*  for every lookup adds the 14 bits of the ephemeral range to what they must
*  get right at the same time, which is the cheapest defence a resolver
*  without cryptography has and the reason dns.h names the source port
*  alongside the id.
*
*  The new port is bound BEFORE the old one is released, so there is never a
*  moment with no binding: if the binding table is full the old port is still
*  live and the lookup goes out on it rather than failing. Between the two
*  calls both ports reach the same handler, which is harmless -- a late reply
*  to the previous query fails the id and question checks anyway. */
static void dns_rebind(void)
{
    uint16_t port;

    port = dns_make_port();

    if(port == dns_bound_port)
        return;

    if(udp_bind(port, dns_receive) < 0)
        return;                          /* keep the port we have */

    if(dns_bound_port != 0)
        udp_unbind(dns_bound_port);

    dns_bound_port = port;
}

int dns_resolve(const char *name)
{
    int hit;

    if(dns_current_state == DNS_STATE_QUERY)
    {
        dns_error = "a lookup is already in flight";
        return -1;
    }

    if(!net_up())
    {
        dns_error = "no network card";
        return -1;
    }

    if(name == 0)
    {
        dns_error = "no name to look up";
        return -1;
    }

    /* Encode first, then decode what was encoded. The round trip is not
    *  wasted work: it validates the name against every wire limit exactly
    *  once, in the code that has to enforce them anyway, and it hands back
    *  the canonical text -- trailing dot removed -- which is what the echoed
    *  question is compared against and what the cache is keyed on. */
    if(dns_name_encode(name, dns_qwire, sizeof(dns_qwire),
                       &dns_qwire_len) < 0)
    {
        dns_qwire_len = 0;
        dns_error = "not a usable host name";
        return -1;
    }

    if(dns_name_decode(dns_qwire, dns_qwire_len, 0, dns_scratch,
                       sizeof(dns_scratch), 0) < 0)
    {
        dns_qwire_len = 0;
        dns_error = "not a usable host name";
        return -1;
    }

    dns_name_copy(dns_query_name, sizeof(dns_query_name), dns_scratch);

    /* Nothing from a previous lookup may be left where this one's answer
    *  goes -- including an answer that arrived after we stopped waiting. */
    dns_inbox_full = 0;
    dns_result_ip = 0;
    dns_result_secs = 0;
    dns_error = "";
    dns_attempts = 0;
    dns_unsent_tries = 0;

    /* A name that is still in the cache is answered from it, and still goes
    *  through the states dns.h describes: the answer is posted into the same
    *  mailbox a real reply would land in and the state becomes
    *  DNS_STATE_QUERY, so the first dns_poll() completes the lookup exactly
    *  as it would have completed a lookup that went to the wire. The caller
    *  has one path either way, which is the whole point -- and no datagram
    *  is sent, which is the point of having a cache. */
    hit = dns_cache_find(dns_query_name);
    if(hit >= 0)
    {
        dns_inbox.status = DNS_ANS_OK;
        dns_inbox.rcode = DNS_RCODE_NOERROR;
        dns_inbox.ip = dns_cache[hit].ip;
        dns_inbox.ttl = dns_cache_left(&dns_cache[hit]);

        dns_current_state = DNS_STATE_QUERY;
        dns_arm_timer(1);                /* nothing to wait for, but the
                                          *  timer is armed like any other */
        dns_inbox_full = 1;

        return 0;
    }

    /* The server comes from the lease. There is no configured fallback and
    *  no public resolver hard coded here: a machine that has not been told
    *  where to ask cannot guess, and pointing at somebody else's server by
    *  default would send every name this machine looks up to a third party
    *  the user never chose. */
    dns_server_addr = dhcp_dns();
    if(dns_server_addr == 0 || dns_server_addr == IP_ADDR_BROADCAST)
    {
        dns_error = "no DNS server known -- run dhcp first";
        return -1;
    }

    if(dns_bound_port == 0 && dns_init() < 0)
        return -1;

    dns_rebind();
    dns_id = dns_make_id();

    /* The state is set before the query goes out, not after: the reply can
    *  arrive from the card's interrupt while udp_send() is still returning,
    *  and dns_receive() drops everything unless the state says we are
    *  waiting for it. */
    dns_current_state = DNS_STATE_QUERY;

    /* A transmit that fails is not fatal and is not even unusual -- ARP is
    *  very likely still resolving the server on the first lookup after boot.
    *  dns_transmit() arms the timer for whichever of the two happened. */
    dns_transmit();

    return 0;
}

void dns_cancel(void)
{
    dns_inbox_full = 0;

    if(dns_current_state == DNS_STATE_QUERY)
    {
        dns_current_state = DNS_STATE_IDLE;
        dns_error = "";
    }

    /* The port stays bound. Nothing is listening for now, but a late reply
    *  to the abandoned query is dropped by the state test in dns_receive()
    *  either way, and keeping the binding means the next lookup does not
    *  depend on a free slot in the UDP table being available again. */
}

int dns_state(void)
{
    return dns_current_state;
}

uint32_t dns_result(void)
{
    return dns_result_ip;
}

uint32_t dns_result_ttl(void)
{
    return dns_result_secs;
}

const char *dns_last_error(void)
{
    return dns_error;
}

uint32_t dns_lookup_cached(const char *name)
{
    int hit;

    if(name == 0)
        return 0;

    hit = dns_cache_find(name);
    if(hit < 0)
        return 0;

    return dns_cache[hit].ip;
}

int dns_cache_entries(void)
{
    int i;
    int n;

    n = 0;
    for(i = 0; i < DNS_CACHE_SIZE; i++)
        if(dns_cache[i].used && dns_cache_left(&dns_cache[i]) > 0)
            n++;

    return n;
}

/* Walks the live entries in slot order. The index counts only entries that
*  are still within their TTL, which is what dns_cache_entries() returns, so
*  a caller looping from 0 to that count sees exactly those -- and an entry
*  that expires halfway through the loop shortens it rather than making the
*  caller read a stale one. */
int dns_cache_get(int index, char *name, uint32_t size,
                  uint32_t *ip, uint32_t *ttl_left)
{
    int i;
    int n;

    if(index < 0)
        return -1;

    n = 0;
    for(i = 0; i < DNS_CACHE_SIZE; i++)
    {
        if(!dns_cache[i].used)
            continue;
        if(dns_cache_left(&dns_cache[i]) == 0)
            continue;

        if(n == index)
        {
            if(name != 0)
                dns_name_copy(name, size, dns_cache[i].name);
            if(ip != 0)
                *ip = dns_cache[i].ip;
            if(ttl_left != 0)
                *ttl_left = dns_cache_left(&dns_cache[i]);

            return 0;
        }

        n++;
    }

    return -1;
}

void dns_cache_flush(void)
{
    int i;

    for(i = 0; i < DNS_CACHE_SIZE; i++)
        dns_cache[i].used = 0;
}

uint32_t dns_queries_sent(void)
{
    return dns_stat_sent;
}

/* Replies that survived every check and were acted on. */
uint32_t dns_replies_used(void)
{
    return dns_stat_used;
}

/* Datagrams that reached our port and were not believed: the wrong source,
*  the wrong id, a question that is not ours, or one arriving when nothing
*  was asked. On a quiet network this stays at zero; anything else is either
*  a late duplicate or somebody guessing. */
uint32_t dns_replies_dropped(void)
{
    return dns_stat_dropped;
}
