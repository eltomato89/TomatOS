/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: TCP, client side -- a stream built out of packets that may be lost,
*        duplicated or reordered.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*
*  tcp.h says what this is and what it deliberately is not. This file is the
*  how, and five conventions run through all of it. Every one of them is a
*  bug that hides for hours and then eats a stream, so they are spelled out
*  here rather than left to be inferred from the code.
*
*  1. SEQUENCE NUMBERS ARE COMPARED WITH HELPERS, NEVER WITH < OR >.
*
*  A sequence number is a 32 bit counter on a circle. It has no smallest and
*  no largest value, so "a < b" is not a question the type can answer; the
*  question that does have an answer is "is b ahead of a", and that is
*  (int32_t)(a - b) < 0. It is correct whenever the two are less than 2 GB
*  apart, which they always are here: everything compared lives inside a
*  window of at most 64 KB or inside one retransmission's worth of it.
*
*  The five helpers seq_lt/seq_le/seq_gt/seq_ge/seq_diff below are the ONLY
*  place in this file where a relational operator touches a value from the
*  sequence space. Equality is a different matter and is used directly: two
*  wrapping counters are equal exactly when they name the same byte, which
*  needs no interpretation. Everything else that is compared with < or > in
*  this file is a length, a buffer index, a byte count, a retry counter or a
*  millisecond duration -- none of which wrap in any way that matters, and
*  all of which are named so that this is obvious at the point of use.
*
*  2. THE BUFFERS ARE INDEXED BY SEQUENCE NUMBER.
*
*  Both rings are a power of two long, and the byte with sequence number s
*  lives at s & (size - 1). There is no head pointer and no length field to
*  keep in step with anything, which matters more than the elegance: the
*  interrupt handler and task context each advance exactly one of the two
*  ends of each ring and only ever read the other's, so there is no
*  read-modify-write that an interrupt landing in the middle could undo.
*
*    send ring   valid bytes are [snd_una, snd_end)
*                snd_una advances in the interrupt (an ACK arrived)
*                snd_end advances in task context (tcp_send took bytes)
*
*    receive ring  unread bytes are [rcv_rd, rcv_end)
*                rcv_end advances in the interrupt (data arrived)
*                rcv_rd  advances in task context (tcp_recv handed it out)
*
*  rcv_end is the end of the DATA, and rcv_nxt is the end of the sequence
*  space -- they differ by one once the peer's FIN has been taken, because a
*  FIN occupies a sequence number without being a byte. Everything that talks
*  to the peer uses rcv_nxt; everything that talks to the caller uses rcv_end.
*  One variable for both is the bug that hands the reader a byte that was
*  never sent.
*
*  A stale read of the other side's index is always safe in the conservative
*  direction: it makes the reader believe there is less room, or less data,
*  than there really is, and the next call sees the truth.
*
*  2a. ONE CALLER PER HANDLE. The split above is between task context and the
*  interrupt, which on a uniprocessor is the only concurrency there is: an
*  interrupt runs to completion between two of a task's instructions, never
*  alongside them. Two TASKS sharing one handle is a different matter and is
*  not supported -- rcv_rd and snd_end are each advanced with a read, a
*  change and a store, and a second task in the middle of that would lose
*  one of the two updates. Every other module here makes the same assumption;
*  a handle belongs to whoever opened it.
*
*  3. THE INTERRUPT NEVER ALLOCATES, FREES OR PRINTS.
*
*  tcp_receive() runs inside the card's IRQ. malloc() and free() walk a list
*  that a task may be halfway through rewriting, so the receive path only
*  ever sets state; the actual free() happens in tcp_poll(), in task context.
*  Nothing here prints, on any path -- dhcp.c had one leftover printf in its
*  handler and it interleaved into the middle of the shell's output.
*
*  4. SENDING FROM THE INTERRUPT MAY FAIL, AND THAT IS NOT AN ERROR.
*
*  ip.c builds every packet in one shared buffer and refuses a second writer
*  outright (see ip_tx_busy there), so an acknowledgement sent from the
*  interrupt while a task is mid-ip_send() is simply not sent. Nothing here
*  treats that as a failure: data is covered by the retransmission timer
*  anyway, and a pure acknowledgement sets ack_pending so that the next
*  tcp_poll() sends it from task context. The two directions also use
*  separate transmit buffers -- see tcp_tx_task and tcp_tx_irq -- so the two
*  contexts can never be in the same one.
*
*  5. BYTE ORDER. Every field inside the packed header is in NETWORK order
*  and is only touched through htons/htonl/ntohs/ntohl. Every address, port
*  and sequence number this file holds in a variable is in HOST order. The
*  conversion happens where a value enters or leaves a header, nowhere else.
*/

#include <system.h>
#include <net.h>
#include <tcp.h>
#include <mm.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define TCP_HDR_LEN         20      /* header without options              */
#define TCP_OPT_MSS_LEN      4      /* the one option we send              */

/* Option kinds. Only three are recognised; everything else is skipped by
*  its own length byte, which is what the encoding is for. */
#define TCP_OPT_END          0
#define TCP_OPT_NOP          1
#define TCP_OPT_MSS          2

/* An IP header without options. Only needed to know how much of a segment
*  still fits in one frame; ip.c owns the real definition. */
#define IP_HDR_LEN          20

/* What ip_send() returns when the next hop is still being resolved. It is
*  ip.c's IP_ERR_PENDING, which no header exports; udp.c repeats the value
*  for the same reason. -2 means "nothing left the machine, try again in a
*  moment", which is a very different thing from -1. */
#define IP_SEND_PENDING     (-2)

/* Retransmission timeout, in milliseconds.
*
*  The initial value is the one RFC 6298 requires before any round trip has
*  been measured: a second, because a stack that has never heard from a peer
*  has no basis for guessing anything shorter.
*
*  The lower bound is where this departs from the specification, which asks
*  for a whole second. That number exists to keep a stack from flooding the
*  internet with retransmissions it only thinks are needed; here the peer is
*  one hop away across a virtual switch or a few tens of milliseconds away
*  through it, and a second of silence after a single dropped segment is a
*  fifth of the time the whole fetch is allowed to take. 200 ms is still an
*  order of magnitude above any round trip this machine will see, so it does
*  not cause spurious retransmissions, and it is the same floor Linux uses.
*
*  The upper bound is far below the 60 s the specification permits, for the
*  same reason as TCP_RTX_RETRIES below: the total time this stack is willing
*  to spend on a connection that has gone quiet is half a minute, and an
*  8 s backoff step is as large as fits inside that budget. */
#define TCP_RTO_INIT      1000UL
#define TCP_RTO_MIN        200UL
#define TCP_RTO_MAX       8000UL

/* How many times a segment is retransmitted before the connection is given
*  up on. The handshake gets fewer: 1+2+4+8 seconds is fifteen seconds of a
*  host that is not answering at all, which is long enough to survive a slow
*  DHCP-configured gateway and short enough that a shell command does not
*  look hung. An established connection gets 1+2+4+8+8+8, about half a
*  minute, because by then bytes have moved and the peer has earned more
*  patience than an address that has never answered. */
#define TCP_SYN_RETRIES      4
#define TCP_RTX_RETRIES      6

/* How long a transmit that never left the machine is retried before it
*  counts as a lost segment. ip_send() refuses while ARP has not resolved
*  the next hop, and on the first connection after boot it always does -- the
*  ARP request that will fill the cache is the one that failed send just
*  issued. Treating that as a segment that went unanswered would spend a full
*  RTO waiting for a reply to a packet that was never on the wire, which is
*  exactly the mistake dns.c documents at DNS_UNSENT_TIMEOUT. An ARP reply on
*  a local segment comes back in a millisecond or two, so the quick retry is
*  50 ms; twenty of them is a second of a next hop that will not resolve,
*  after which the ordinary schedule takes over and the connection fails in
*  the ordinary way rather than spinning. */
#define TCP_UNSENT_MS       50UL
#define TCP_UNSENT_MAX      20

/* How long the side that closed first waits before the control block may be
*  reused.
*
*  The specification says 2 x MSL, four minutes, and it is defending two
*  different things. The first is the final acknowledgement: if it is lost,
*  the peer retransmits its FIN and something has to be there to answer, or
*  the peer's connection ends in a reset instead of a close. The second is
*  reincarnation: an old duplicate segment from this connection must not be
*  accepted by a NEW connection that happens to reuse the same four tuple.
*
*  On this machine the first need is satisfied by a few seconds. The peer's
*  FIN retransmission schedule is its own RTO doubling -- one second, two,
*  four -- so ten seconds covers three attempts on any link this kernel will
*  see, and the fourth would arrive after the peer had already given up.
*
*  The second need is answered somewhere else entirely: every connection
*  draws a fresh ephemeral port out of 16384 and a fresh unpredictable ISN,
*  so the four tuple of the next connection is almost certainly different,
*  and if it is not, the sequence space is. Waiting four minutes to defend
*  against that would cost one of four connection slots for four minutes,
*  and a shell that fetches five pages in a row would stall on the fifth.
*
*  The slot is not even really spent: TIME_WAIT keeps the control block, but
*  its buffers are released the moment it is entered -- 12 KB of heap per
*  connection is what this wait would otherwise hold hostage -- and
*  tcp_connect() will reclaim the oldest waiting slot rather than fail. So
*  ten seconds is what the acknowledgement needs, and nothing has to pay for
*  it if the machine is busy. */
#define TCP_TIME_WAIT_MS  10000UL

/* How long we stay in FIN_WAIT_2, i.e. how long a peer may keep sending
*  after we have closed our direction. Half open is normal and is exactly
*  what an HTTP client does, so this has to be generous; it is bounded all
*  the same, because a peer that never closes would otherwise hold a slot
*  forever and there are four of them. Thirty seconds is longer than any
*  page this machine will fetch takes to arrive. */
#define TCP_FIN_WAIT2_MS  30000UL

/* How long a finished connection keeps its slot so that a caller polling
*  tcp_state() can still see TCP_CLOSED and read the last of its data. After
*  this the slot is released outright. tcp_connect() reclaims one sooner if
*  it needs to, so this is a tidiness bound and not a resource limit. */
#define TCP_LINGER_MS     30000UL

/* The peer's MSS, clamped. A peer that states no MSS option gets 536, which
*  is what RFC 1122 requires of a receiver that was told nothing. The floor
*  exists because a peer that states something absurd -- one byte -- would
*  otherwise turn every payload into a segment of its own; 64 is small enough
*  to respect any real link and large enough that a header is not most of the
*  packet. */
#define TCP_MSS_DEFAULT    536
#define TCP_MSS_MIN         64

/* The ephemeral port range, as IANA defines it and as dns.c uses it:
*  49152..65535. Below it are the registered ports, where a listener on this
*  machine might reasonably sit. */
#define TCP_PORT_FIRST   49152UL
#define TCP_PORT_COUNT   16384UL

/* The rings are indexed by sequence number modulo their size, which is only
*  a mask when the size is a power of two. These two lines are the check:
*  a size that is not a power of two makes the array size negative and the
*  file does not compile, instead of producing a stream that is subtly
*  scrambled at every wrap of the buffer. */
typedef char tcp_snd_buf_is_power_of_two
             [((TCP_SND_BUF & (TCP_SND_BUF - 1)) == 0) ? 1 : -1];
typedef char tcp_rcv_buf_is_power_of_two
             [((TCP_RCV_BUF & (TCP_RCV_BUF - 1)) == 0) ? 1 : -1];

#define TCP_SND_MASK   ((uint32_t)(TCP_SND_BUF - 1))
#define TCP_RCV_MASK   ((uint32_t)(TCP_RCV_BUF - 1))

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* One connection.
*
*  Which context writes which field is the thing to keep straight, so it is
*  written next to each of them. "irq" means tcp_receive() and nothing else;
*  "task" means the API and tcp_poll(). A field written by both would need a
*  lock this kernel does not have, and there is none. */
typedef struct
{
    volatile uint8_t  used;        /* irq reads, task writes: slot in use   */
    volatile uint8_t  state;       /* both -- a single byte, never torn     */
    uint16_t generation;           /* task: makes a reused slot a new handle*/

    uint16_t local_port;           /* task, at connect                      */
    uint16_t peer_port;            /* task, at connect                      */
    uint32_t peer_ip;              /* task, at connect                      */

    /* Send sequence space. All host order, all wrapping. */
    uint32_t iss;                  /* task: our initial sequence number     */
    volatile uint32_t snd_una;     /* irq:  oldest byte not acknowledged    */
    uint32_t snd_nxt;              /* task: next byte to put on the wire    */
    uint32_t snd_end;              /* task: one past the last queued byte   */
    uint32_t snd_high;             /* task: highest snd_nxt ever reached    */
    volatile uint32_t snd_wnd;     /* irq:  what the peer will accept       */
    uint32_t snd_wl1;              /* irq:  seq of the last window update   */
    uint32_t snd_wl2;              /* irq:  ack of the last window update   */
    uint32_t snd_fin_seq;          /* task: the sequence our FIN occupies   */
    uint8_t  fin_queued;           /* task: tcp_close() was called          */

    /* Receive sequence space. */
    uint32_t irs;                  /* irq:  the peer's initial sequence     */
    volatile uint32_t rcv_nxt;     /* irq:  next sequence number expected    */
    volatile uint32_t rcv_end;     /* irq:  one past the last DATA byte      */
    uint32_t rcv_rd;               /* task: next byte to hand to the caller  */
    volatile uint8_t  rcv_closed;  /* irq:  nothing more will ever arrive   */
    volatile uint8_t  reset;       /* irq:  the peer reset the connection   */

    /* Buffers. Heap, allocated in tcp_connect(), released as soon as each
    *  can no longer be needed -- see tcp_release(). Both may be 0 while the
    *  control block is still alive, so every use checks. */
    uint8_t *snd_buf;              /* task allocates and frees, irq reads   */
    uint8_t *rcv_buf;              /* task allocates and frees, irq writes  */

    /* Timers. Milliseconds, in the units of timer_get_ticks(). */
    uint32_t rtx_ms;               /* task: when the timer was started      */
    uint32_t rtx_timeout;          /* task: what it is waiting for          */
    uint8_t  rtx_armed;            /* task: is it running at all            */
    uint8_t  retries;              /* task: retransmissions of this segment */
    uint8_t  unsent_tries;         /* task: sends that never left the card  */
    uint32_t state_ms;             /* task: TIME_WAIT / FIN_WAIT_2 / linger */

    /* Round trip estimate, RFC 6298. Milliseconds. srtt == 0 means no
    *  sample has been taken yet. */
    uint32_t rto;                  /* task: the timeout before backoff      */
    uint32_t srtt;                 /* task: smoothed round trip time        */
    uint32_t rttvar;               /* task: its mean deviation              */
    uint32_t rtt_start_ms;         /* task: when the timed segment went out */
    uint32_t rtt_seq;              /* task: the ack that ends the sample    */
    uint8_t  rtt_active;           /* task: a sample is in progress         */

    uint16_t mss;                  /* task: the most we may put in a segment*/
    volatile uint8_t ack_pending;  /* irq sets, task clears: owed an ACK    */

    uint32_t bytes_sent;           /* irq:  acknowledged by the peer        */
    volatile uint32_t bytes_received; /* irq: delivered into the ring       */
} tcp_conn;

static tcp_conn tcp_conns[TCP_MAX_CONNS];

/* Bumped on every allocation so that a handle for a slot that has since been
*  reused does not resolve to the connection now living there. It may wrap;
*  what matters is only that consecutive uses of one slot differ. */
static uint16_t tcp_next_generation = 1;

/* Segments under construction.
*
*  Two buffers, and the split is what makes them safe without a lock. The
*  first is written only in task context -- tcp_send(), tcp_close(),
*  tcp_poll() -- exactly as dns.c's query buffer is, and for the same reason
*  needs no busy flag: there is one class of writer and an interrupt is not
*  one of them. The second is written only by tcp_receive(), which sends
*  nothing but bare acknowledgements and resets and therefore needs no room
*  for a payload.
*
*  Static rather than on the stack: the kernel stack is 4 KiB and an
*  interrupt may land on top of a shell that is already several frames deep,
*  which is how a segment-sized local turns into corruption somewhere else
*  entirely. A kilogram and a half of .bss is the whole cost, and unlike the
*  per-connection buffers it does not multiply by four. */
static uint8_t tcp_tx_task[TCP_HDR_LEN + TCP_OPT_MSS_LEN + TCP_MSS];
static uint8_t tcp_tx_irq[TCP_HDR_LEN + TCP_OPT_MSS_LEN];

/* Counters. Every segment that reaches tcp_receive() ends in exactly one of
*  stat_rx and stat_drop, so the two add up to what IP handed us. */
static uint32_t stat_tx    = 0;
static uint32_t stat_rx    = 0;
static uint32_t stat_drop  = 0;
static uint32_t stat_rtx   = 0;

static const char *tcp_error = "";

/* local helper functions -- not declared in any header */
static tcp_conn *tcp_lookup(int handle);
static void tcp_release(tcp_conn *c);
static void tcp_output(tcp_conn *c);
static int  tcp_ctl(tcp_conn *c, uint8_t flags, int from_irq);

/* ------------------------------------------------------------------ */
/* Sequence arithmetic                                                 */
/* ------------------------------------------------------------------ */

/* The five functions that decide every ordering question in this file.
*
*  A sequence number is a point on a circle of 2^32 values. Subtracting two
*  of them gives the signed distance from the first to the second as long as
*  they are less than 2^31 apart, and everything compared here is inside a
*  window of at most 64 KB or one connection's worth of retransmission, so
*  they always are. The cast to int32_t is where the wrap is handled: the
*  bit pattern of the difference is read as a signed number, so a difference
*  that "went past the end" comes back as a small negative value instead of
*  a huge positive one.
*
*  The classic failure this replaces: with a plain a > b, a peer whose
*  sequence numbers cross 0xFFFFFFFF has every subsequent segment judged as
*  older than everything already seen. Nothing rejects it, nothing logs it;
*  the connection simply stops accepting data after four gigabytes, or --
*  far more likely on a machine that draws a random ISN -- within the first
*  few kilobytes of a connection that happened to start just below the wrap.
*
*  These are the only relational operators in the file that see a sequence
*  number. Equality is used directly where it appears: two wrapping counters
*  are equal exactly when they name the same byte, and there is nothing for a
*  helper to interpret. */
static int seq_lt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static int seq_le(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static int seq_gt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static int seq_ge(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

/* How far b is ahead of a, for a caller that already knows it is. Written
*  as a function so that a bare subtraction of two sequence numbers is as
*  visible in the code as a bare comparison would be. */
static uint32_t seq_diff(uint32_t a, uint32_t b)
{
    return b - a;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Milliseconds since a snapshot of timer_get_ticks(), the same helper dns.c
*  and dhcp.c carry. timer_get_ticks() returns int and wraps after about 25
*  days; the subtraction on the unsigned bit patterns gives the right elapsed
*  time straight through the wrap, which a signed "now - then" does not. */
static uint32_t tcp_ms_since(uint32_t then)
{
    return (uint32_t)timer_get_ticks() - then;
}

/* A mixed 32 bit value, for the initial sequence number and the ephemeral
*  port. This is dns_mix() in tcp.c's clothing and shares its honesty: the
*  inputs are the MAC address, the wall clock, the tick counter and a
*  counter, none of which is an entropy source. Against an off-path attacker
*  the tick counter is the part that actually hides -- the millisecond at
*  which a connection was opened -- and the rest separates one connection
*  from the next.
*
*  For an ISN that matters more than it does for a DNS id. RFC 6528 asks for
*  an ISN that an off-path attacker cannot guess, because guessing it is what
*  lets them inject data into a connection they cannot see. The salt is the
*  four tuple, so two connections opened in the same millisecond to different
*  peers do not get neighbouring numbers.
*
*  When this kernel grows a real random source, this function and dns_mix()
*  are the two places to change. */
static uint32_t tcp_mix(uint32_t salt)
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

    x ^= x >> 15;
    x *= 2246822519UL;
    x ^= x >> 13;

    return x;
}

/* The initial sequence number for one connection.
*
*  Two parts, and both are needed. The mixed value makes it unpredictable,
*  which is what stops an off-path attacker from injecting into the stream.
*  The clock term is what RFC 793 asked for and RFC 6528 keeps: a component
*  that advances with time, so that two connections between the same four
*  tuple, one after the other, cannot start at the same number and have an
*  old duplicate from the first be mistaken for data in the second. The
*  timer runs at a kilohertz and 250 per tick is one increment every four
*  microseconds, which is the rate the specification names. */
static uint32_t tcp_initial_sequence(uint32_t peer_ip, uint16_t peer_port,
                                     uint16_t local_port)
{
    uint32_t salt;

    salt = peer_ip ^ ((uint32_t)peer_port << 16) ^ (uint32_t)local_port;

    return tcp_mix(salt) + (uint32_t)timer_get_ticks() * 250UL;
}

/* Reads one byte run out of a ring that is indexed by sequence number, and
*  writes one into it. The byte with sequence s is always at s & mask, so
*  there is no head to keep and no length to keep in step -- the position of
*  a byte is a property of the byte, not of the state of the buffer.
*
*  A run may cross the end of the ring, which is the only reason these are
*  two memcpy calls and not one. */
static void tcp_ring_put(uint8_t *ring, uint32_t mask, uint32_t seq,
                         const uint8_t *src, uint32_t len)
{
    uint32_t at;
    uint32_t first;

    at = seq & mask;
    first = (mask + 1UL) - at;
    if (first > len)
        first = len;

    memcpy(ring + at, src, (size_t)first);

    if (len > first)
        memcpy(ring, src + first, (size_t)(len - first));
}

static void tcp_ring_get(const uint8_t *ring, uint32_t mask, uint32_t seq,
                         uint8_t *dst, uint32_t len)
{
    uint32_t at;
    uint32_t first;

    at = seq & mask;
    first = (mask + 1UL) - at;
    if (first > len)
        first = len;

    memcpy(dst, ring + at, (size_t)first);

    if (len > first)
        memcpy(dst + first, ring, (size_t)(len - first));
}

/* What we advertise as our receive window: whatever is left of the receive
*  ring once the bytes the caller has not read yet are subtracted.
*
*  The right edge of this window is rcv_nxt + window, which by the line below
*  is rcv_rd + TCP_RCV_BUF (one more once the peer's FIN has been counted,
*  which no longer matters -- nothing else will arrive). rcv_rd only ever
*  moves forward, and only
*  when the caller reads, so THE ADVERTISED WINDOW NEVER SHRINKS -- the edge
*  moves right or stands still. A window whose right edge retreats is legal
*  but hated, because it invalidates data the peer has already committed to
*  sending, and it is the usual way an advertised window turns into a
*  deadlock. Here it cannot happen by construction.
*
*  A released receive buffer advertises zero. That only happens once the peer
*  has closed, so there is nothing left to advertise for. */
static uint16_t tcp_window(const tcp_conn *c)
{
    uint32_t used;

    if (c->rcv_buf == 0)
        return 0;

    used = seq_diff(c->rcv_rd, c->rcv_end);
    if (used >= (uint32_t)TCP_RCV_BUF)
        return 0;

    return (uint16_t)((uint32_t)TCP_RCV_BUF - used);
}

/* Bytes queued by the caller that have not been put on the wire yet. The
*  test is a sequence comparison and not a subtraction, because once the FIN
*  has gone out snd_nxt is one PAST snd_end and a bare difference would come
*  back as four billion. */
static uint32_t tcp_unsent(const tcp_conn *c)
{
    if (seq_lt(c->snd_nxt, c->snd_end))
        return seq_diff(c->snd_nxt, c->snd_end);

    return 0;
}

/* Has our FIN been acknowledged? It occupies one sequence number of its own,
*  so the acknowledgement that covers it is the one past it. */
static int tcp_fin_acked(const tcp_conn *c)
{
    if (!c->fin_queued)
        return 0;

    return seq_ge(c->snd_una, c->snd_fin_seq + 1UL);
}

/* Is our FIN queued and still waiting to be sent, or resent? */
static int tcp_fin_pending(const tcp_conn *c)
{
    if (!c->fin_queued)
        return 0;

    return seq_le(c->snd_nxt, c->snd_fin_seq);
}

/* Is anything of ours unacknowledged -- data, our SYN or our FIN? This is
*  what decides whether the retransmission timer has a job. */
static int tcp_outstanding(const tcp_conn *c)
{
    return seq_lt(c->snd_una, c->snd_nxt);
}

/* ------------------------------------------------------------------ */
/* Buffers                                                             */
/* ------------------------------------------------------------------ */

/* Gives back what the connection can no longer need. TASK CONTEXT ONLY: the
*  heap's free list is not something to walk from an interrupt that may have
*  landed in the middle of a malloc(), so the receive path only ever sets the
*  state that makes a buffer releasable and this runs afterwards.
*
*  The two buffers become useless at different moments and are freed at those
*  moments rather than both at the end, because between them they are 12 KB
*  and there are four connections:
*
*    The send ring is dead once our FIN has been acknowledged. Nothing can
*    ever be queued after a close and nothing unacknowledged is left, so
*    there is nothing a retransmission could want. That is the moment a
*    connection enters TIME_WAIT, which is precisely why TIME_WAIT costs a
*    control block and not a buffer.
*
*    The receive ring is dead once the peer has closed AND the caller has
*    read everything. Not one moment sooner: data that arrived before the FIN
*    is still the caller's to collect, and a reader that stops at the FIN
*    would otherwise lose the end of the stream. This is why a connection in
*    TCP_CLOSED may still be holding a buffer, and why tcp_recv() keeps
*    working on it.
*
*  The pointer is cleared BEFORE the free, and every user in the interrupt
*  path checks it. On a uniprocessor an interrupt cannot land between those
*  two stores and then survive into the free -- it runs to completion first --
*  so the interrupt either sees a live buffer or no buffer, never a freed
*  one. */
static void tcp_release(tcp_conn *c)
{
    uint8_t *p;

    if (c->snd_buf != 0 && (tcp_fin_acked(c) || c->state == TCP_CLOSED))
    {
        p = c->snd_buf;
        c->snd_buf = 0;
        free(p);
    }

    if (c->rcv_buf != 0 && c->rcv_closed && c->rcv_rd == c->rcv_end)
    {
        p = c->rcv_buf;
        c->rcv_buf = 0;
        free(p);
    }
}

/* Everything, unconditionally -- for a slot that is being torn down or
*  handed to a new connection. Anything the caller had not read is gone, and
*  that is the point: the slot is no longer theirs. */
static void tcp_release_all(tcp_conn *c)
{
    uint8_t *p;

    if (c->snd_buf != 0)
    {
        p = c->snd_buf;
        c->snd_buf = 0;
        free(p);
    }

    if (c->rcv_buf != 0)
    {
        p = c->rcv_buf;
        c->rcv_buf = 0;
        free(p);
    }
}

/* ------------------------------------------------------------------ */
/* Slots and handles                                                   */
/* ------------------------------------------------------------------ */

/* A handle is an index and a generation packed into a positive int. The
*  generation is what makes a stale handle harmless: a caller that keeps one
*  past the end of its connection and uses it after the slot has been given
*  to somebody else gets TCP_ENOCONN rather than somebody else's stream.
*
*  Four bits of index, which is room for sixteen connections -- more than
*  TCP_MAX_CONNS will plausibly become, and the assertion is the mask in
*  tcp_lookup(). The generation starts at 1, so no handle is ever 0 and a
*  caller cannot confuse one with a null. */
#define TCP_HANDLE_INDEX_BITS   4
#define TCP_HANDLE_INDEX_MASK   0x0F

static int tcp_handle_of(int index)
{
    tcp_conn *c;

    c = &tcp_conns[index];

    return (int)(((uint32_t)c->generation << TCP_HANDLE_INDEX_BITS) |
                 (uint32_t)index);
}

static tcp_conn *tcp_lookup(int handle)
{
    int index;
    uint16_t generation;

    if (handle <= 0)
        return 0;

    index = handle & TCP_HANDLE_INDEX_MASK;
    if (index >= TCP_MAX_CONNS)
        return 0;

    generation = (uint16_t)((uint32_t)handle >> TCP_HANDLE_INDEX_BITS);

    if (!tcp_conns[index].used)
        return 0;
    if (tcp_conns[index].generation != generation)
        return 0;

    return &tcp_conns[index];
}

/* Retires a slot: buffers gone, handle no longer resolving, ready to be
*  handed out again. The generation moves on here rather than at allocation
*  so that a handle stops working the instant the connection it named is
*  gone, even if nothing takes the slot for a while. */
static void tcp_free_slot(tcp_conn *c)
{
    tcp_release_all(c);

    memset((void *)c, 0, (size_t)sizeof(tcp_conn));

    c->state = TCP_CLOSED;
    c->used = 0;
}

/* Finds a slot for a new connection.
*
*  A free one first. Failing that, a finished one -- CLOSED or TIME_WAIT --
*  is taken over, oldest first, because holding a slot for a connection that
*  has ended in order to refuse a connection that has not is the wrong way
*  round. This is what keeps TIME_WAIT from ever being the reason a fetch
*  fails, and it is why ten seconds of it costs nothing when the machine is
*  busy: the wait is only ever served out of slack. */
static tcp_conn *tcp_alloc_slot(void)
{
    tcp_conn *best;
    uint32_t best_age;
    uint32_t age;
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++)
    {
        if (!tcp_conns[i].used)
            return &tcp_conns[i];
    }

    best = 0;
    best_age = 0;

    for (i = 0; i < TCP_MAX_CONNS; i++)
    {
        if (tcp_conns[i].state != TCP_CLOSED &&
            tcp_conns[i].state != TCP_TIME_WAIT)
            continue;

        age = tcp_ms_since(tcp_conns[i].state_ms);

        if (best == 0 || age > best_age)
        {
            best = &tcp_conns[i];
            best_age = age;
        }
    }

    if (best != 0)
        tcp_free_slot(best);

    return best;
}

/* A source port nothing else here is using. Two connections to the same peer
*  and port must differ somewhere, and the source port is the only field a
*  client gets to choose. Drawing rather than counting also means a
*  connection that ends and is reopened almost never lands on the same four
*  tuple, which is most of why TIME_WAIT can be as short as it is. */
static uint16_t tcp_pick_port(uint32_t peer_ip, uint16_t peer_port)
{
    uint16_t port;
    int attempt;
    int i;
    int taken;

    for (attempt = 0; attempt < 32; attempt++)
    {
        port = (uint16_t)(TCP_PORT_FIRST + (tcp_mix(0x7C) % TCP_PORT_COUNT));

        taken = 0;
        for (i = 0; i < TCP_MAX_CONNS; i++)
        {
            if (!tcp_conns[i].used)
                continue;
            if (tcp_conns[i].local_port != port)
                continue;

            /* The same port is only a collision when the rest of the four
            *  tuple matches too -- two connections to different peers may
            *  share it and often will, with only 16384 to draw from. */
            if (tcp_conns[i].peer_ip == peer_ip &&
                tcp_conns[i].peer_port == peer_port)
            {
                taken = 1;
                break;
            }
        }

        if (!taken)
            return port;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/* Builds one segment and hands it to IP. The single place a segment is
*  constructed, so there is one description of the header and it cannot
*  drift between the six callers.
*
*  "buf" is the caller's transmit buffer, which is what keeps task context
*  and the interrupt out of each other's way; see tcp_tx_task above. "seq" is
*  the sequence number the segment starts at, and "data_len" bytes are taken
*  out of the send ring AT THAT SEQUENCE -- not at some offset a caller had
*  to compute, which is the whole reason the ring is indexed the way it is.
*
*  Returns what ip_send() returned: 0 sent, IP_SEND_PENDING if ARP is still
*  resolving the next hop, anything else a permanent failure for this
*  segment. No caller treats a failure as fatal -- the retransmission timer
*  is the recovery path for everything this function can fail at. */
static int tcp_transmit(tcp_conn *c, uint8_t *buf, uint32_t seq, uint8_t flags,
                        int with_mss, uint32_t data_len)
{
    tcp_header *th;
    uint32_t hdr_len;
    uint32_t total;
    int result;

    hdr_len = TCP_HDR_LEN;
    if (with_mss)
        hdr_len += TCP_OPT_MSS_LEN;

    if (data_len > 0 && c->snd_buf == 0)
        data_len = 0;

    th = (tcp_header *)buf;
    th->src_port = htons(c->local_port);
    th->dst_port = htons(c->peer_port);
    th->seq      = htonl(seq);

    /* The acknowledgement field carries rcv_nxt whether or not the ACK flag
    *  is set. It is only meaningful with the flag, and the one segment we
    *  send without it -- the very first SYN -- has rcv_nxt at zero, so
    *  nothing is leaked and nothing has to be special-cased. */
    th->ack      = htonl(c->rcv_nxt);
    th->offset   = (uint8_t)((hdr_len / 4) << 4);
    th->flags    = flags;
    th->window   = htons(tcp_window(c));
    th->checksum = 0;             /* zeroed before summing, never after */
    th->urgent   = 0;

    if (with_mss)
    {
        /* Kind, length, then the value big endian. Four bytes, so the header
        *  stays a whole number of 32 bit words and needs no padding. */
        buf[TCP_HDR_LEN + 0] = TCP_OPT_MSS;
        buf[TCP_HDR_LEN + 1] = TCP_OPT_MSS_LEN;
        buf[TCP_HDR_LEN + 2] = (uint8_t)(TCP_MSS >> 8);
        buf[TCP_HDR_LEN + 3] = (uint8_t)(TCP_MSS & 0xFF);
    }

    if (data_len > 0)
        tcp_ring_get(c->snd_buf, TCP_SND_MASK, seq, buf + hdr_len, data_len);

    total = hdr_len + data_len;

    /* The same pseudo header UDP uses, over the whole segment. The source
    *  address is net_ip() because that is what ip_send() will write into the
    *  IP header; summing anything else produces a segment that looks perfect
    *  in a capture and is discarded by every peer.
    *
    *  net_checksum_pseudo() turns a computed zero into 0xFFFF, which UDP
    *  needs because zero means "no checksum" there. TCP has no such rule, so
    *  a real zero would be legal -- but it is also harmless to send 0xFFFF
    *  instead, because in one's complement the two are the same value, +0
    *  and -0, and the peer's sum comes out identical either way. So the
    *  shared function is right for both protocols and no special case is
    *  needed here. */
    th->checksum = net_checksum_pseudo(net_ip(), c->peer_ip, IP_PROTO_TCP,
                                       buf, total);

    result = ip_send(c->peer_ip, IP_PROTO_TCP, buf, total);

    if (result == 0)
    {
        stat_tx++;

        /* Whatever the next hop was, it is resolved now. */
        c->unsent_tries = 0;
    }
    else if (result == IP_SEND_PENDING && c->unsent_tries == 0)
    {
        /* Nothing left the machine and ARP is the reason. The quick retry in
        *  tcp_poll() picks this up; see TCP_UNSENT_MS. */
        c->unsent_tries = 1;
    }

    return result;
}

/* A bare control segment -- an acknowledgement, or a reset. Sequence
*  snd_nxt, acknowledgement rcv_nxt, no payload and no options.
*
*  "from_irq" chooses the buffer and nothing else. The two contexts have
*  separate ones so that a task building a data segment and an interrupt
*  answering something that just arrived are never in the same memory. */
static int tcp_ctl(tcp_conn *c, uint8_t flags, int from_irq)
{
    return tcp_transmit(c, from_irq ? tcp_tx_irq : tcp_tx_task,
                        c->snd_nxt, flags, 0, 0);
}

/* Acknowledges, and remembers to try again if it could not.
*
*  ip.c refuses a send that arrives while another is half built, so an
*  acknowledgement raised from the interrupt is genuinely lost now and then.
*  It is never simply dropped: ack_pending makes the next tcp_poll() send it
*  from task context, where nothing is in the way. That is also the safety
*  net for a window update -- the segment that tells a peer with a full
*  window that it may start again is the one segment nothing else would
*  retransmit for us. */
static void tcp_ack(tcp_conn *c, int from_irq)
{
    if (tcp_ctl(c, TCP_ACK, from_irq) == 0)
        c->ack_pending = 0;
    else
        c->ack_pending = 1;
}

/* Our SYN, with our MSS in it. Only from task context: tcp_connect() and the
*  handshake retransmission in tcp_poll(). */
static int tcp_send_syn(tcp_conn *c)
{
    return tcp_transmit(c, tcp_tx_task, c->iss, TCP_SYN, 1, 0);
}

/* Answers a segment that belongs to no connection.
*
*  Without this, a peer that still believes in a connection this machine has
*  forgotten -- because it was aborted, or because the kernel rebooted --
*  keeps retransmitting into silence until its own timers give up, which can
*  be minutes. A reset ends it at once and tells the truth.
*
*  RFC 793's two forms, and the difference matters: a segment that carried an
*  acknowledgement is answered with a reset whose sequence number is that
*  acknowledgement, because that is the only number the peer will accept as
*  being inside its window. One that did not is answered with sequence zero
*  and an acknowledgement of everything the segment occupied.
*
*  A reset is never answered with a reset. That is the loop that would
*  otherwise run until one of the two machines was rebooted. */
static void tcp_reset_stranger(uint32_t src_ip, uint16_t src_port,
                               uint16_t dst_port, const tcp_header *th,
                               uint32_t seg_space)
{
    tcp_header *out;
    uint32_t total;

    if (th->flags & TCP_RST)
        return;

    out = (tcp_header *)tcp_tx_irq;
    out->src_port = htons(dst_port);
    out->dst_port = htons(src_port);
    out->offset   = (uint8_t)((TCP_HDR_LEN / 4) << 4);
    out->window   = 0;
    out->checksum = 0;
    out->urgent   = 0;

    if (th->flags & TCP_ACK)
    {
        out->seq   = th->ack;               /* already in network order */
        out->ack   = 0;
        out->flags = TCP_RST;
    }
    else
    {
        out->seq   = 0;
        out->ack   = htonl(ntohl(th->seq) + seg_space);
        out->flags = TCP_RST | TCP_ACK;
    }

    total = TCP_HDR_LEN;

    out->checksum = net_checksum_pseudo(net_ip(), src_ip, IP_PROTO_TCP,
                                        tcp_tx_irq, total);

    if (ip_send(src_ip, IP_PROTO_TCP, tcp_tx_irq, total) == 0)
        stat_tx++;
}

/* ------------------------------------------------------------------ */
/* The retransmission timer                                            */
/* ------------------------------------------------------------------ */

/* Starts the timer if it is not already running. RFC 6298 5.1: a segment
*  carrying data -- or a SYN or a FIN, which occupy sequence numbers and are
*  acknowledged the same way -- arms the timer, and an already running timer
*  is left alone so that the oldest unacknowledged segment is the one being
*  timed. */
static void tcp_timer_arm(tcp_conn *c)
{
    if (c->rtx_armed)
        return;

    c->rtx_ms      = (uint32_t)timer_get_ticks();
    c->rtx_timeout = c->rto;
    c->rtx_armed   = 1;
}

/* Restarts it from now, for a segment that has just been sent or an
*  acknowledgement that has just moved the window along. RFC 6298 5.3. */
static void tcp_timer_restart(tcp_conn *c)
{
    c->rtx_ms      = (uint32_t)timer_get_ticks();
    c->rtx_timeout = c->rto;
    c->rtx_armed   = 1;
}

static void tcp_timer_stop(tcp_conn *c)
{
    c->rtx_armed = 0;
    c->retries = 0;
}

/* Folds one round trip measurement into the estimate. RFC 6298's estimator,
*  in integers: the shifts are the 1/8 and 1/4 the algorithm calls for, and
*  every term stays well inside 32 bits because the values are milliseconds
*  and are clamped below.
*
*  A sample of zero would be indistinguishable from "no sample yet", which is
*  what srtt == 0 means, and a round trip across a virtual switch really is
*  under one tick. RFC 6298 has the same problem and the same answer: the
*  clock granularity is the floor of what can be measured, so a sample below
*  it is one tick. */
static void tcp_rtt_sample(tcp_conn *c, uint32_t measured)
{
    uint32_t delta;
    uint32_t rto;

    if (measured == 0)
        measured = 1;

    if (c->srtt == 0)
    {
        c->srtt   = measured;
        c->rttvar = measured / 2;
    }
    else
    {
        if (c->srtt > measured)
            delta = c->srtt - measured;
        else
            delta = measured - c->srtt;

        c->rttvar = c->rttvar - (c->rttvar >> 2) + (delta >> 2);
        c->srtt   = c->srtt   - (c->srtt   >> 3) + (measured >> 3);
    }

    /* RTO = SRTT + max(G, 4 * RTTVAR), G being the clock granularity of one
    *  millisecond. Without the floor a very steady link drives RTTVAR to
    *  zero and the timeout collapses onto the mean round trip, where half of
    *  all segments are declared lost. */
    rto = c->rttvar * 4UL;
    if (rto < 1UL)
        rto = 1UL;

    rto += c->srtt;

    if (rto < TCP_RTO_MIN)
        rto = TCP_RTO_MIN;
    if (rto > TCP_RTO_MAX)
        rto = TCP_RTO_MAX;

    c->rto = rto;
}

/* Doubles the timeout for the next attempt, saturating rather than wrapping.
*
*  Exponential backoff is the only congestion response this stack has. There
*  is no window that shrinks and no slow start; what there is, is a sender
*  that waits twice as long after every failure, which is enough that a
*  machine on a link that has gone bad stops making it worse.
*
*  Karn's algorithm has two halves and this is the second: the backed off
*  timeout stays in force until something is acknowledged. The first half is
*  in tcp_output(), where a segment that has been retransmitted is never used
*  to measure a round trip -- there is no way to tell which of the two copies
*  the acknowledgement was for, and guessing wrong poisons the estimate in
*  whichever direction hurts most. */
static void tcp_backoff(tcp_conn *c)
{
    uint32_t t;

    t = c->rtx_timeout * 2UL;

    if (t > TCP_RTO_MAX || t < c->rtx_timeout)
        t = TCP_RTO_MAX;

    c->rtx_ms      = (uint32_t)timer_get_ticks();
    c->rtx_timeout = t;
    c->rtx_armed   = 1;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

/* Puts as much as the peer will take on the wire, and the FIN behind it if
*  one is queued. Task context only.
*
*  How much the peer will take is snd_una + snd_wnd, the right edge of the
*  window it advertised, and how much we have is snd_end. The segment size is
*  the smaller of what is left of either, capped at the MSS the peer stated.
*  Nothing here is clever: there is no Nagle, so a byte queued is a byte sent,
*  and no congestion window, so the peer's advertised window is the only
*  limit. On a virtual link with a 4 KB send buffer, neither is missed.
*
*  Karn's first half lives here. snd_high is the furthest snd_nxt has ever
*  reached, so a segment starting at or beyond it has never been sent before
*  and its acknowledgement can only be for this copy. A segment starting
*  below it is a retransmission -- the timer rewound snd_nxt -- and is never
*  timed. */
static void tcp_output(tcp_conn *c)
{
    uint32_t unsent;
    uint32_t usable;
    uint32_t edge;
    uint32_t seq;
    uint32_t n;
    uint8_t  flags;
    int fin_now;
    int timed;

    if (c->snd_buf == 0 && !tcp_fin_pending(c))
        return;

    for (;;)
    {
        /* An acknowledgement may have overtaken us: it is processed in the
        *  interrupt and can land between any two instructions in this loop,
        *  including the rewind that tcp_retransmit() does just before
        *  calling here. Never send from behind what the peer has already
        *  admitted to holding. */
        if (seq_lt(c->snd_nxt, c->snd_una))
            c->snd_nxt = c->snd_una;

        unsent = tcp_unsent(c);

        /* The right edge of the peer's window, and how far past snd_nxt it
        *  still is. A window that has closed since the last segment left can
        *  put the edge BEHIND snd_nxt, which is why this is a sequence
        *  comparison and not a subtraction that would underflow. */
        edge = c->snd_una + c->snd_wnd;
        if (seq_lt(c->snd_nxt, edge))
            usable = seq_diff(c->snd_nxt, edge);
        else
            usable = 0;

        n = unsent;
        if (n > usable)
            n = usable;
        if (n > (uint32_t)c->mss)
            n = (uint32_t)c->mss;

        /* The FIN goes out behind the last byte, in the same segment when
        *  there is one -- that is what a real client's capture shows, and it
        *  costs the peer one fewer acknowledgement. It needs a sequence
        *  number of its own, hence usable > n rather than usable >= n; when
        *  the window is too tight for even that, the probe below carries it
        *  instead. */
        fin_now = (tcp_fin_pending(c) && n == unsent && usable > n);

        if (n == 0 && !fin_now)
            break;

        seq = c->snd_nxt;

        flags = TCP_ACK;
        if (fin_now)
            flags |= TCP_FIN;

        /* PSH on the segment that empties the buffer. It asks the peer to
        *  hand what it has to its application rather than wait for more,
        *  which is what makes a request that fits in one segment get
        *  answered immediately instead of after the peer's own timer. */
        if (n > 0 && n == unsent)
            flags |= TCP_PSH;

        /* THE CONTROL BLOCK IS BROUGHT UP TO DATE BEFORE THE SEGMENT LEAVES,
        *  and the order is not a matter of taste.
        *
        *  ip_send() reaches the card, the card raises its interrupt, and
        *  tcp_receive() runs -- all of it between two instructions of this
        *  function. On a virtual link the peer's acknowledgement comes back
        *  in a tenth of a millisecond, so this is not a rare interleaving;
        *  it is the ordinary one. If snd_nxt were still describing the state
        *  before this segment when that acknowledgement arrived, the receive
        *  path would find it acknowledging bytes we had not sent, answer it
        *  with a challenge and throw it away -- and it is the only
        *  acknowledgement the peer will ever send for those bytes. What
        *  follows is a connection that retransmits perfectly good data every
        *  RTO and a FIN that is never acknowledged, ending in a reset.
        *
        *  So: what we are about to have sent is recorded first, and the
        *  transmit is the last thing that happens. A segment that then fails
        *  to leave is not a problem -- snd_nxt is past snd_una, the timer is
        *  running, and the retransmission rewinds and sends it again. That
        *  is exactly how tcp_connect() already treats the SYN.
        *
        *  Karn: time this segment only if it is genuinely new -- one that
        *  starts at or beyond the furthest we have ever reached has never
        *  been sent before, so an acknowledgement can only be for this copy. */
        timed = 0;

        if (!c->rtt_active && seq_ge(seq, c->snd_high))
        {
            c->rtt_seq      = seq + n + (fin_now ? 1UL : 0UL);
            c->rtt_start_ms = (uint32_t)timer_get_ticks();
            c->rtt_active   = 1;
            timed = 1;
        }

        c->snd_nxt = seq + n + (fin_now ? 1UL : 0UL);

        if (seq_gt(c->snd_nxt, c->snd_high))
            c->snd_high = c->snd_nxt;

        tcp_timer_arm(c);

        if (tcp_transmit(c, tcp_tx_task, seq, flags, 0, n) != 0)
        {
            /* Nothing left the machine -- ARP, most likely. The timer is
            *  armed and will send it again; it is not an error and does not
            *  spend an attempt, see tcp_poll(). The one thing to undo is the
            *  round trip measurement: a segment that never reached the card
            *  would time the ARP exchange rather than the peer. Only the
            *  sample this iteration started, never one already running. */
            if (timed)
                c->rtt_active = 0;

            break;
        }

        if (fin_now || n == 0)
            break;
    }

    /* Queued but blocked by a closed window: the timer still has to run, or
    *  nothing would ever ask the peer whether it has room again. That probe
    *  is in tcp_retransmit(). */
    if (!c->rtx_armed && (tcp_unsent(c) > 0 || tcp_fin_pending(c)))
        tcp_timer_arm(c);
}

/* One segment, sent past a window that says no.
*
*  A peer whose receive buffer is full advertises zero, and the window update
*  that reopens it is a bare acknowledgement -- which is not retransmitted by
*  anybody. If that update is lost and this side is waiting for the window,
*  both ends wait forever. The way out is that the blocked sender keeps
*  poking: one byte past the edge, on the same backing off schedule as a
*  retransmission. The peer must answer it with an acknowledgement, and that
*  acknowledgement carries the current window.
*
*  With no data to probe with, the FIN does the same job. */
static void tcp_probe(tcp_conn *c)
{
    uint32_t n;
    uint32_t seq;

    n = tcp_unsent(c);
    if (n > 1UL)
        n = 1UL;

    if (n == 0 && !tcp_fin_pending(c))
        return;

    seq = c->snd_nxt;

    /* Recorded before it is sent, for the reason spelled out in tcp_output():
    *  the answer to this probe can arrive in the interrupt before the call
    *  that sent it has returned. The probe advances snd_nxt exactly as an
    *  ordinary segment would, because it is ordinary data -- sent at a
    *  moment the window said not to. A peer that has room takes it, and one
    *  that has not drops it and acknowledges what it does have, which is the
    *  answer we were after either way. */
    c->snd_nxt = seq + n + (n == 0 ? 1UL : 0UL);

    if (seq_gt(c->snd_nxt, c->snd_high))
        c->snd_high = c->snd_nxt;

    tcp_transmit(c, tcp_tx_task, seq,
                 (uint8_t)(TCP_ACK | (n == 0 ? TCP_FIN : 0)), 0, n);
}

/* The retransmission timer fired: everything from the oldest unacknowledged
*  byte goes again.
*
*  Go back N, which is the only choice a stack without selective
*  acknowledgement has -- and, since this one also does not reassemble out of
*  order data, the only one the peer could make use of. Rewinding snd_nxt is
*  the whole mechanism: tcp_output() then finds the data unsent and sends it,
*  the FIN included if it was in the rewound range. */
static void tcp_retransmit(tcp_conn *c)
{
    stat_rtx++;

    /* Karn: nothing sent from here on may be used to measure a round trip. */
    c->rtt_active = 0;

    if (c->state == TCP_SYN_SENT)
    {
        tcp_send_syn(c);
        return;
    }

    c->snd_nxt = c->snd_una;

    if (c->snd_wnd == 0)
        tcp_probe(c);
    else
        tcp_output(c);
}

/* ------------------------------------------------------------------ */
/* Receiving                                                           */
/* ------------------------------------------------------------------ */

/* The peer's MSS out of the options of its SYN, or 0 when it stated none.
*
*  Every length in here comes off the wire, so every one of them is checked
*  before it is used to step forward: a length byte of zero would loop
*  forever and one that points past the header would read into the driver's
*  receive ring. Both are what a malformed SYN looks like, and the answer to
*  both is to stop reading options rather than to drop the segment -- the
*  handshake is still perfectly usable without them. */
static uint16_t tcp_option_mss(const uint8_t *segment, uint32_t data_off)
{
    uint32_t at;
    uint32_t kind;
    uint32_t optlen;

    at = TCP_HDR_LEN;

    while (at < data_off)
    {
        kind = (uint32_t)segment[at];

        if (kind == TCP_OPT_END)
            break;

        if (kind == TCP_OPT_NOP)
        {
            at++;
            continue;
        }

        if (at + 1UL >= data_off)
            break;                       /* a length byte that is not there */

        optlen = (uint32_t)segment[at + 1UL];

        if (optlen < 2UL || at + optlen > data_off)
            break;                       /* malformed: stop, do not guess   */

        if (kind == TCP_OPT_MSS && optlen == 4UL)
            return (uint16_t)(((uint16_t)segment[at + 2UL] << 8) |
                               (uint16_t)segment[at + 3UL]);

        at += optlen;
    }

    return 0;
}

/* Settles on a segment size once the peer has stated its own. It may be
*  smaller than ours and often is -- a tunnel, or a peer being careful -- and
*  it is not a suggestion: a segment larger than what the peer said it can
*  reassemble is a segment it may drop. Ours is the other bound, because this
*  stack does not fragment and anything above it could not leave the card. */
static void tcp_apply_mss(tcp_conn *c, uint16_t peer_mss)
{
    uint32_t mss;

    mss = (peer_mss == 0) ? (uint32_t)TCP_MSS_DEFAULT : (uint32_t)peer_mss;

    if (mss > (uint32_t)TCP_MSS)
        mss = (uint32_t)TCP_MSS;
    if (mss < (uint32_t)TCP_MSS_MIN)
        mss = (uint32_t)TCP_MSS_MIN;

    c->mss = (uint16_t)mss;
}

/* Is this segment inside the window we advertised?
*
*  RFC 793's four cases, and they are four because a zero window and an empty
*  segment are each their own situation:
*
*    empty segment, window open: acceptable if it starts inside the window.
*    empty segment, window shut: acceptable only exactly at rcv_nxt, which is
*        what lets a peer's window probe through -- refusing it would leave
*        the peer poking at a connection that never answers.
*    segment with data, window shut: never. There is nowhere to put it.
*    segment with data, window open: acceptable if either end of it is inside
*        the window. Either end, not both -- a segment that starts before
*        rcv_nxt and runs into the window is a retransmission of something
*        partly seen, and it carries bytes we still need.
*
*  The length here is the DATA and does not count the FIN, and that is a
*  deliberate departure from a literal reading of RFC 793. A FIN occupies a
*  sequence number but needs no room in the receive ring, so a stream whose
*  last byte exactly fills the buffer would otherwise end like this: the
*  window is zero, the peer's FIN is judged a one octet segment against a
*  shut window, it is refused, and the close is delayed until the peer's own
*  timer resends it -- a whole retransmission timeout added to the end of
*  every response that happens to be the size of the buffer. Counting only
*  the data lets a FIN through at rcv_nxt whatever the window says, which is
*  what Linux does and what nothing can abuse: a FIN anywhere else is still
*  refused by the empty-segment rules above, and the FIN is only ACTED on
*  further down when every byte in front of it has been delivered.
*
*  Getting this wrong is the failure that only shows up on a busy link: a
*  quiet connection never produces an out of window segment, so an
*  implementation that simply accepts everything works perfectly until the
*  first retransmission or reordering, and then delivers duplicated or
*  reordered bytes into the stream. */
static int tcp_acceptable(const tcp_conn *c, uint32_t seq, uint32_t data_len,
                          uint32_t window)
{
    uint32_t last;
    uint32_t edge;

    edge = c->rcv_nxt + window;

    if (data_len == 0)
    {
        if (window == 0)
            return (seq == c->rcv_nxt);

        return (seq_ge(seq, c->rcv_nxt) && seq_lt(seq, edge));
    }

    if (window == 0)
        return 0;

    last = seq + data_len - 1UL;

    return (seq_ge(seq, c->rcv_nxt) && seq_lt(seq, edge)) ||
           (seq_ge(last, c->rcv_nxt) && seq_lt(last, edge));
}

/* Everything a connection does when it ends badly. The state goes to CLOSED
*  and the slot stays, so that the caller polling tcp_state() sees the end
*  and tcp_recv() can still hand over whatever arrived before it. Buffers are
*  not freed here: this runs in the interrupt as often as not, and the heap
*  is not the interrupt's to touch. tcp_poll() does it. */
static void tcp_kill(tcp_conn *c, int was_reset)
{
    c->state      = TCP_CLOSED;
    c->rcv_closed = 1;
    c->rtx_armed  = 0;
    c->state_ms   = (uint32_t)timer_get_ticks();

    if (was_reset)
        c->reset = 1;
}

/* Moves into TIME_WAIT and restarts its clock. Also reached from TIME_WAIT
*  itself, when the peer retransmits its FIN because our last acknowledgement
*  was lost -- which is the entire reason the state exists, so the wait
*  starts again from that moment rather than from the first arrival. */
static void tcp_time_wait(tcp_conn *c)
{
    c->state     = TCP_TIME_WAIT;
    c->state_ms  = (uint32_t)timer_get_ticks();
    c->rtx_armed = 0;
}

/* An acknowledgement that may be new. Returns 1 when it moved snd_una.
*
*  Everything here is a sequence comparison, including the one that decides
*  how much of what was acknowledged is data: our FIN sits at snd_fin_seq and
*  occupies a sequence number without being a byte, so an acknowledgement
*  past it covers one more number than there are bytes in the ring. Treating
*  that number as a byte would slide the send ring one position and every
*  retransmission after it would send the stream shifted by one. */
static int tcp_ack_update(tcp_conn *c, uint32_t ack)
{
    uint32_t acked;
    uint32_t measured;

    if (seq_le(ack, c->snd_una))
        return 0;                        /* nothing new; a duplicate ACK */

    acked = seq_diff(c->snd_una, ack);

    /* How much of that is STREAM, which is a different number. Our FIN
    *  occupies a sequence number without being a byte, and so does our SYN,
    *  so an acknowledgement routinely covers one more number than there are
    *  bytes in the send ring -- and the ring holds exactly the bytes from
    *  snd_una to snd_end. Capping against it is the whole correction, and
    *  without it the byte count runs one past the truth from the moment a
    *  FIN is acknowledged. */
    if (acked > seq_diff(c->snd_una, c->snd_end))
        acked = seq_diff(c->snd_una, c->snd_end);

    c->snd_una = ack;
    c->bytes_sent += acked;

    /* Karn, the measuring half: a sample is only taken when the segment it
    *  timed was never retransmitted, which tcp_output() guaranteed when it
    *  started the sample. */
    if (c->rtt_active && seq_ge(ack, c->rtt_seq))
    {
        measured = tcp_ms_since(c->rtt_start_ms);
        c->rtt_active = 0;
        tcp_rtt_sample(c, measured);
    }

    /* Progress: the backed off timeout is retired and the retry count starts
    *  again. A connection that survived one bad moment should not carry the
    *  penalty into the next hour. */
    c->retries = 0;

    if (tcp_outstanding(c))
        tcp_timer_restart(c);
    else
        tcp_timer_stop(c);

    return 1;
}

/* The window the peer advertised, taken only from a segment that is newer
*  than the one it was last taken from. RFC 793's SND.WL1/WL2 test.
*
*  Without it, a segment that was delayed on the network and arrives after a
*  newer one can put back an old, smaller window -- retracting room the peer
*  has since offered, and in the worst case closing a window the peer thinks
*  is open, which is a deadlock neither side can see a reason for. */
static void tcp_window_update(tcp_conn *c, uint32_t seq, uint32_t ack,
                              uint32_t window)
{
    if (seq_lt(c->snd_wl1, seq) ||
        (c->snd_wl1 == seq && seq_le(c->snd_wl2, ack)))
    {
        c->snd_wnd = window;
        c->snd_wl1 = seq;
        c->snd_wl2 = ack;
    }
}

/* The handshake, from the one state where almost none of the ordinary rules
*  apply: there is no window yet to judge a segment against, so what makes a
*  segment believable here is that its acknowledgement names our SYN. */
static void tcp_receive_syn_sent(tcp_conn *c, const tcp_header *th,
                                 const uint8_t *segment, uint32_t data_off,
                                 uint32_t seg_seq, uint32_t seg_ack,
                                 uint8_t flags)
{
    if (flags & TCP_ACK)
    {
        /* The only acceptable acknowledgement is of our SYN and nothing
        *  else: anything at or below the ISN, or beyond what we have sent,
        *  is from some other connection or from an attacker guessing. */
        if (seq_le(seg_ack, c->iss) || seq_gt(seg_ack, c->snd_nxt))
        {
            if (!(flags & TCP_RST))
            {
                /* Answered with a reset carrying their acknowledgement as
                *  its sequence, which is the one number they will accept. */
                tcp_reset_stranger(c->peer_ip, c->peer_port, c->local_port,
                                   th, 0);
            }

            stat_drop++;
            return;
        }
    }

    if (flags & TCP_RST)
    {
        /* A reset with a valid acknowledgement in this state means one thing
        *  and it is worth saying precisely: nothing is listening on that
        *  port. A refused connection is not a timeout, and a caller that
        *  reports it as one sends its user looking for a network problem
        *  that is not there.
        *
        *  A reset WITHOUT an acknowledgement is not believable here -- there
        *  is no window to place it in, so anyone could have sent it -- and
        *  is dropped. */
        if (flags & TCP_ACK)
        {
            tcp_error = "the peer refused the connection";
            tcp_kill(c, 1);
            stat_rx++;
        }
        else
        {
            stat_drop++;
        }

        return;
    }

    if (!(flags & TCP_SYN))
    {
        stat_drop++;
        return;
    }

    stat_rx++;

    c->irs     = seg_seq;
    c->rcv_nxt = seg_seq + 1UL;
    c->rcv_end = c->rcv_nxt;
    c->rcv_rd  = c->rcv_nxt;

    tcp_apply_mss(c, tcp_option_mss(segment, data_off));

    if (flags & TCP_ACK)
    {
        c->snd_una = seg_ack;

        /* Our SYN was timed like any other segment. */
        if (c->rtt_active && seq_ge(seg_ack, c->rtt_seq))
        {
            c->rtt_active = 0;
            tcp_rtt_sample(c, tcp_ms_since(c->rtt_start_ms));
        }

        c->snd_wnd = (uint32_t)ntohs(th->window);
        c->snd_wl1 = seg_seq;
        c->snd_wl2 = seg_ack;

        c->state = TCP_ESTABLISHED;
        c->retries = 0;
        tcp_timer_stop(c);

        tcp_ack(c, 1);
        return;
    }

    /* A SYN with no acknowledgement is a simultaneous open: the peer is
    *  trying to connect to us at the same moment. That needs SYN_RECEIVED,
    *  which tcp.h says this stack does not have -- it never listens, so the
    *  case can only arise from a peer that is confused or hostile. Reset it
    *  and report the failure rather than leave the caller waiting on a state
    *  machine that has nowhere to go. */
    tcp_ctl(c, (uint8_t)(TCP_RST), 1);
    tcp_error = "the peer answered with a SYN of its own";
    tcp_kill(c, 1);
}

/* One segment on an established -- or closing -- connection. This is the
*  order RFC 793 prescribes and the order matters: a segment is judged
*  acceptable before anything in it is believed, the reset is handled before
*  the acknowledgement because a reset ends the connection whatever it
*  acknowledges, and the acknowledgement is handled before the FIN so that
*  FIN_WAIT_1 has already become FIN_WAIT_2 by the time the peer's FIN is
*  seen. Reordering any of those produces a state machine that works on a
*  quiet link. */
static void tcp_receive_conn(tcp_conn *c, const tcp_header *th,
                             const uint8_t *segment, uint32_t data_off,
                             uint32_t len, uint32_t seg_seq, uint32_t seg_ack,
                             uint8_t flags)
{
    const uint8_t *data;
    uint32_t payload;
    uint32_t carried;
    uint32_t space;
    uint32_t window;
    uint32_t fin_seq;
    uint32_t skip;
    uint32_t taken;
    int all_taken;

    payload = len - data_off;
    carried = payload;               /* what it held before any trimming */
    data    = segment + data_off;
    window  = (uint32_t)tcp_window(c);

    /* The sequence number one past everything this segment occupies. Needed
    *  before any trimming, because it is what decides whether a FIN is in
    *  order: a FIN is only the end of the stream if every byte in front of
    *  it has been delivered. */
    fin_seq = seg_seq + payload;

    /* --- acceptable? ---------------------------------------------- */
    if (!tcp_acceptable(c, seg_seq, payload, window))
    {
        /* Outside the window. It is not believed, and the peer is told what
        *  we do have so that it can correct itself -- a duplicate
        *  acknowledgement is how a peer learns that its segment fell in a
        *  hole. A reset is the exception: answering one with an
        *  acknowledgement is how two machines argue forever. */
        if (!(flags & TCP_RST))
        {
            /* This is where a retransmitted FIN lands in TIME_WAIT, and it
            *  is the whole reason the state exists. The FIN sits one below
            *  rcv_nxt -- we already counted it -- so it can never be inside
            *  the window, and the peer is sending it again because our last
            *  acknowledgement never arrived. It gets another one, and the
            *  wait starts over so that a second loss is covered too. */
            if (c->state == TCP_TIME_WAIT && (flags & TCP_FIN))
                tcp_time_wait(c);

            tcp_ack(c, 1);
        }

        stat_drop++;
        return;
    }

    stat_rx++;

    /* --- reset ---------------------------------------------------- */
    if (flags & TCP_RST)
    {
        /* RFC 5961's refinement of RFC 793, and it is worth the four extra
        *  lines: a reset anywhere in the window used to be enough to tear a
        *  connection down, which means an off-path attacker only had to
        *  guess a sequence number within 64 KB. Only a reset exactly at
        *  rcv_nxt is acted on; one merely inside the window gets a
        *  "challenge" acknowledgement, and a peer that really has lost the
        *  connection will answer that with a reset at the right number. */
        if (seg_seq == c->rcv_nxt)
        {
            if (c->state == TCP_TIME_WAIT)
                tcp_kill(c, 0);
            else
            {
                tcp_error = "the connection was reset by the peer";
                tcp_kill(c, 1);
            }
        }
        else
        {
            tcp_ack(c, 1);
        }

        return;
    }

    /* --- a SYN inside the window ---------------------------------- */
    if (flags & TCP_SYN)
    {
        /* The peer restarted, or something is forging. Either way this
        *  connection cannot continue: the sequence space it is built on has
        *  been declared invalid by the other end. */
        tcp_ctl(c, TCP_RST, 1);
        tcp_error = "the peer sent a SYN on an open connection";
        tcp_kill(c, 1);
        return;
    }

    /* --- acknowledgement ------------------------------------------ */
    if (!(flags & TCP_ACK))
        return;                          /* every segment after the handshake
                                         *  carries one; one that does not is
                                         *  not worth acting on */

    if (seq_gt(seg_ack, c->snd_nxt))
    {
        /* An acknowledgement of something we have not sent. Not believed,
        *  and the peer is told where we really are. */
        tcp_ack(c, 1);
        return;
    }

    tcp_ack_update(c, seg_ack);
    tcp_window_update(c, seg_seq, seg_ack, (uint32_t)ntohs(th->window));

    /* State moves that the acknowledgement alone can cause. */
    switch (c->state)
    {
    case TCP_FIN_WAIT_1:
        if (tcp_fin_acked(c))
        {
            c->state    = TCP_FIN_WAIT_2;
            c->state_ms = (uint32_t)timer_get_ticks();
        }
        break;

    case TCP_CLOSING:
        if (tcp_fin_acked(c))
            tcp_time_wait(c);
        break;

    case TCP_LAST_ACK:
        if (tcp_fin_acked(c))
        {
            tcp_kill(c, 0);
            return;                      /* the connection is over */
        }
        break;

    case TCP_TIME_WAIT:
        /* Nothing new can be acknowledged here, but the peer retransmitting
        *  its FIN has to be answered and the wait restarted. That is done
        *  below, where the FIN itself is seen. */
        break;

    default:
        break;
    }

    /* --- data ----------------------------------------------------- */
    all_taken = 1;

    if (payload > 0 && (c->state == TCP_ESTABLISHED ||
                        c->state == TCP_FIN_WAIT_1 ||
                        c->state == TCP_FIN_WAIT_2))
    {
        /* A retransmission may repeat bytes already delivered, so the part
        *  in front of rcv_nxt is dropped rather than the whole segment: the
        *  rest of it is data we still need, and throwing it away would make
        *  the peer resend the same overlap forever. */
        if (seq_lt(seg_seq, c->rcv_nxt))
        {
            skip = seq_diff(seg_seq, c->rcv_nxt);

            if (skip >= payload)
                payload = 0;
            else
            {
                data    += skip;
                payload -= skip;
                seg_seq  = c->rcv_nxt;
            }
        }

        if (payload > 0)
        {
            if (seg_seq != c->rcv_nxt)
            {
                /* Ahead of what we expect: something in between was lost.
                *  tcp.h says there is no reassembly here, so this is dropped
                *  and the acknowledgement below tells the peer where the
                *  hole starts. It will resend from there, and the segment we
                *  just threw away with it. Legal, simple, and slow only when
                *  the link is losing packets. */
                all_taken = 0;
                payload = 0;
            }
            else if (c->rcv_buf == 0)
            {
                all_taken = 0;
                payload = 0;
            }
            else
            {
                space = (uint32_t)TCP_RCV_BUF -
                        seq_diff(c->rcv_rd, c->rcv_end);

                taken = payload;
                if (taken > space)
                {
                    /* More than we advertised. Trimming rather than dropping
                    *  keeps the stream moving, but the tail is gone -- and
                    *  so, therefore, is any FIN behind it. */
                    taken = space;
                    all_taken = 0;
                }

                if (taken > 0)
                {
                    tcp_ring_put(c->rcv_buf, TCP_RCV_MASK, c->rcv_nxt,
                                 data, taken);

                    /* rcv_end after the bytes: it is what publishes them to
                    *  the reader, which runs in task context and can be
                    *  interrupted between any two instructions. rcv_nxt after
                    *  that, because it is what the peer is told. */
                    c->rcv_end = c->rcv_end + taken;
                    c->rcv_nxt = c->rcv_end;
                    c->bytes_received += taken;
                }
            }
        }
    }
    else if (payload > 0)
    {
        /* Data after we have seen the peer's FIN, or in a state where the
        *  peer has no business sending any. Nothing to do with it. */
        all_taken = 0;
    }

    /* --- FIN ------------------------------------------------------ */
    if ((flags & TCP_FIN) && all_taken && fin_seq == c->rcv_nxt)
    {
        /* The FIN takes a sequence number and no more than that: rcv_end,
        *  which is what the reader measures against, stays where it is. */
        c->rcv_nxt    = c->rcv_end + 1UL;
        c->rcv_closed = 1;

        switch (c->state)
        {
        case TCP_ESTABLISHED:
            /* Half open, and this is the normal shape of it: the peer has
            *  said everything, we may still be sending. tcp_recv() keeps
            *  handing out what arrived before the FIN until it is drained,
            *  and only then reports the close. */
            c->state = TCP_CLOSE_WAIT;
            break;

        case TCP_FIN_WAIT_1:
            /* Both sides closed at once and ours is not acknowledged yet.
            *  The acknowledgement step above runs before this one, so if our
            *  FIN had been acknowledged the state would already be
            *  FIN_WAIT_2 and this case would not be reached -- which is
            *  exactly why the two are in that order. */
            c->state    = TCP_CLOSING;
            c->state_ms = (uint32_t)timer_get_ticks();
            break;

        case TCP_FIN_WAIT_2:
            tcp_time_wait(c);
            break;

        default:
            break;
        }
    }

    /* Acknowledge whatever the segment carried. No delayed acknowledgement --
    *  tcp.h rules it out -- so anything that occupied sequence space is
    *  answered at once, whether it was taken or dropped: a segment we could
    *  not use is precisely the one the peer most needs to hear about. A bare
    *  acknowledgement from the peer carries nothing and is not answered,
    *  which is what stops two machines acknowledging each other forever. */
    if (carried > 0 || (flags & TCP_FIN))
        tcp_ack(c, 1);
}

/* Called by ip.c for protocol 6, from the card's interrupt.
*
*  Nothing here prints, allocates or waits. Every field is checked before it
*  is believed: a segment arrives from anywhere on the wire, and the first
*  thing it can be is wrong. */
void tcp_receive(uint32_t src_ip, uint32_t dst_ip,
                 const uint8_t *segment, uint32_t len)
{
    const tcp_header *th;
    tcp_conn *c;
    uint32_t data_off;
    uint32_t seg_seq;
    uint32_t seg_ack;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  flags;
    int i;

    if (segment == 0 || len < TCP_HDR_LEN)
    {
        stat_drop++;
        return;
    }

    /* Addressed to this machine specifically. ip.c also delivers broadcasts,
    *  which is right for DHCP and wrong for a connection: a segment sent to
    *  the whole segment belongs to no four tuple of ours. */
    if (net_ip() == IP_ADDR_ANY || dst_ip != net_ip())
    {
        stat_drop++;
        return;
    }

    th = (const tcp_header *)segment;

    /* The data offset is in 32 bit words and may exceed 20 when options are
    *  present. It has to be at least a bare header and must still lie inside
    *  what arrived -- a value from the wire that says otherwise is how a
    *  remote sender gets the kernel to read past the driver's ring. */
    data_off = (uint32_t)(th->offset >> 4) * 4UL;
    if (data_off < TCP_HDR_LEN || data_off > len)
    {
        stat_drop++;
        return;
    }

    /* Unlike UDP, the checksum is not optional and a field of zero is not a
    *  special value -- it is simply a checksum that happens to be zero, and
    *  it still has to verify. A sound segment sums to 0xFFFF; see the note
    *  at net_checksum_pseudo(). The span is the whole segment, whose length
    *  IP worked out from its own header, so ethernet's padding is not in it. */
    if (net_checksum_pseudo(src_ip, dst_ip, IP_PROTO_TCP, segment, len)
        != 0xFFFF)
    {
        stat_drop++;
        return;
    }

    src_port = ntohs(th->src_port);
    dst_port = ntohs(th->dst_port);
    seg_seq  = ntohl(th->seq);
    seg_ack  = ntohl(th->ack);
    flags    = th->flags;

    /* Find the connection this belongs to. A slot in TCP_CLOSED is not one:
    *  it is a finished connection whose handle is still being held open for
    *  its owner, and a segment arriving for it is a stray from a peer that
    *  has not caught up. */
    c = 0;
    for (i = 0; i < TCP_MAX_CONNS; i++)
    {
        if (!tcp_conns[i].used)
            continue;
        if (tcp_conns[i].state == TCP_CLOSED)
            continue;
        if (tcp_conns[i].local_port != dst_port)
            continue;
        if (tcp_conns[i].peer_port != src_port)
            continue;
        if (tcp_conns[i].peer_ip != src_ip)
            continue;

        c = &tcp_conns[i];
        break;
    }

    if (c == 0)
    {
        /* Nobody's. Reset it so the peer stops retransmitting into a machine
        *  that has forgotten the connection ever existed. */
        tcp_reset_stranger(src_ip, src_port, dst_port, th,
                           (len - data_off) +
                           ((flags & TCP_SYN) ? 1UL : 0UL) +
                           ((flags & TCP_FIN) ? 1UL : 0UL));
        stat_drop++;
        return;
    }

    if (c->state == TCP_SYN_SENT)
        tcp_receive_syn_sent(c, th, segment, data_off, seg_seq, seg_ack, flags);
    else
        tcp_receive_conn(c, th, segment, data_off, len, seg_seq, seg_ack, flags);
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

/* Everything one connection's clocks can cause. Task context, so this is
*  where the heap is touched and where a connection that has run out of
*  patience is given up on. */
static void tcp_poll_conn(tcp_conn *c)
{
    /* Whatever the receive path made releasable, release now -- it could not
    *  do it itself. */
    tcp_release(c);

    /* An acknowledgement the interrupt owed and could not send, because ip.c
    *  was busy with somebody else's packet. */
    if (c->ack_pending && c->state != TCP_CLOSED)
        tcp_ack(c, 0);

    switch (c->state)
    {
    case TCP_CLOSED:
        /* Finished. The slot is kept for a while so that a caller polling
        *  tcp_state() sees TCP_CLOSED and can still read the last of what
        *  arrived; after that it is given back. tcp_connect() takes one
        *  sooner when it has to. */
        if (tcp_ms_since(c->state_ms) >= TCP_LINGER_MS)
            tcp_free_slot(c);
        return;

    case TCP_TIME_WAIT:
        if (tcp_ms_since(c->state_ms) >= TCP_TIME_WAIT_MS)
        {
            tcp_kill(c, 0);
            tcp_release(c);
        }
        return;

    case TCP_FIN_WAIT_2:
        /* Half open for too long. The peer has our FIN and has acknowledged
        *  it, and is simply never going to close its own direction. Legal,
        *  and not something four connection slots can wait on forever. */
        if (tcp_ms_since(c->state_ms) >= TCP_FIN_WAIT2_MS)
        {
            tcp_ctl(c, TCP_RST, 0);
            tcp_error = "the peer never closed its half of the connection";
            tcp_kill(c, 1);
            tcp_release(c);
            return;
        }
        break;

    default:
        break;
    }

    if (!c->rtx_armed)
    {
        /* Nothing outstanding. There may still be something to send: a
        *  window that reopened, or bytes queued while ARP was cold. */
        tcp_output(c);
        return;
    }

    if (tcp_ms_since(c->rtx_ms) < c->rtx_timeout)
    {
        /* Not a retransmission -- but the peer may have opened its window
        *  since the last poll, and the segment that said so was handled in
        *  the interrupt, which does not send data. This is where queued
        *  bytes get their chance. */
        tcp_output(c);
        return;
    }

    /* The timer fired. First, the case that is not a lost segment at all:
    *  the last attempt never reached the card, because ip_send() is still
    *  waiting for ARP to resolve the next hop. Retrying quickly costs
    *  nothing and does not spend an attempt -- the schedule exists to
    *  survive a segment that was lost, not one that was never sent. The
    *  count is bounded, so a next hop that never resolves still ends in an
    *  honest failure rather than a spin. */
    if (c->unsent_tries > 0 && c->unsent_tries < TCP_UNSENT_MAX)
    {
        c->unsent_tries++;
        c->rtx_ms      = (uint32_t)timer_get_ticks();
        c->rtx_timeout = TCP_UNSENT_MS;

        if (c->state == TCP_SYN_SENT)
            tcp_send_syn(c);
        else
        {
            c->snd_nxt = c->snd_una;
            tcp_output(c);
        }

        /* tcp_transmit() clears the counter the moment something actually
        *  reaches the card, which is the signal to go back to the ordinary
        *  schedule. */
        if (c->unsent_tries == 0)
        {
            c->rtx_ms      = (uint32_t)timer_get_ticks();
            c->rtx_timeout = c->rto;
        }

        return;
    }

    c->retries++;

    if (c->state == TCP_SYN_SENT)
    {
        if (c->retries > TCP_SYN_RETRIES)
        {
            tcp_error = "no answer from the peer";
            tcp_kill(c, 0);
            tcp_release(c);
            return;
        }
    }
    else if (c->retries > TCP_RTX_RETRIES)
    {
        tcp_ctl(c, TCP_RST, 0);
        tcp_error = "the peer stopped acknowledging";
        tcp_kill(c, 1);
        tcp_release(c);
        return;
    }

    tcp_backoff(c);
    tcp_retransmit(c);
}

void tcp_poll(void)
{
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++)
    {
        if (!tcp_conns[i].used)
            continue;

        tcp_poll_conn(&tcp_conns[i]);
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

void tcp_init(void)
{
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++)
    {
        /* Not tcp_free_slot(): at boot the pointers are whatever .bss holds,
        *  which is zero, and freeing them would be a free() of nothing. On a
        *  second call the slots are live and their buffers do have to go. */
        if (tcp_conns[i].used)
            tcp_free_slot(&tcp_conns[i]);
        else
            memset((void *)&tcp_conns[i], 0, (size_t)sizeof(tcp_conn));

        tcp_conns[i].state = TCP_CLOSED;
    }

    tcp_error = "";
}

int tcp_connect(uint32_t dst, uint16_t port)
{
    tcp_conn *c;
    uint16_t local_port;
    uint8_t *snd;
    uint8_t *rcv;
    int index;

    if (!net_up())
    {
        tcp_error = "no network card";
        return TCP_EFAILED;
    }

    if (dst == IP_ADDR_ANY || dst == IP_ADDR_BROADCAST || port == 0)
    {
        tcp_error = "not an address a connection can be made to";
        return TCP_EFAILED;
    }

    c = tcp_alloc_slot();
    if (c == 0)
    {
        tcp_error = "all connections are in use";
        return TCP_ENOCONN;
    }

    local_port = tcp_pick_port(dst, port);
    if (local_port == 0)
    {
        tcp_error = "no free local port";
        return TCP_ENOCONN;
    }

    /* Both buffers or neither. 12 KB per connection is the reason they are
    *  not in .bss: four of them would be 48 KB of image that is resident
    *  whether or not anything is connected, and this kernel was shrunk from
    *  341 KB of .bss to 67 KB precisely to stop paying for what it is not
    *  using. */
    snd = (uint8_t *)malloc((size_t)TCP_SND_BUF);
    rcv = (uint8_t *)malloc((size_t)TCP_RCV_BUF);

    if (snd == 0 || rcv == 0)
    {
        if (snd != 0)
            free(snd);
        if (rcv != 0)
            free(rcv);

        tcp_error = "not enough memory for the connection's buffers";
        return TCP_EFAILED;
    }

    index = (int)(c - tcp_conns);

    memset((void *)c, 0, (size_t)sizeof(tcp_conn));

    c->generation = tcp_next_generation++;
    if (c->generation == 0)
        c->generation = tcp_next_generation++;   /* 0 would make handle 0 */

    c->peer_ip    = dst;
    c->peer_port  = port;
    c->local_port = local_port;
    c->snd_buf    = snd;
    c->rcv_buf    = rcv;

    c->iss     = tcp_initial_sequence(dst, port, local_port);
    c->snd_una = c->iss;

    /* The SYN occupies the ISN itself, so the first byte the caller ever
    *  queues is one past it -- which is where the empty send ring starts,
    *  and why snd_end is not the ISN. Getting this off by one would shift
    *  every byte of the stream against the sequence numbers that name it. */
    c->snd_end = c->iss + 1UL;

    /* Until the peer's SYN says otherwise: the window we assume is one
    *  segment, which is enough to get the handshake's own acknowledgement
    *  out, and the segment size is the conservative default. Both are
    *  replaced the moment the SYN,ACK arrives. */
    c->snd_wnd = (uint32_t)TCP_MSS;
    c->mss     = TCP_MSS_DEFAULT;

    c->rto      = TCP_RTO_INIT;
    c->state    = TCP_SYN_SENT;
    c->state_ms = (uint32_t)timer_get_ticks();

    /* The SYN occupies one sequence number whether or not it reaches the
    *  card. Advancing snd_nxt here rather than after a successful send is
    *  what makes the retransmission path work without a special case: the
    *  SYN is simply the oldest unacknowledged thing, exactly like data. */
    c->snd_nxt  = c->iss + 1UL;
    c->snd_high = c->snd_nxt;

    c->rtt_active   = 1;
    c->rtt_start_ms = (uint32_t)timer_get_ticks();
    c->rtt_seq      = c->snd_nxt;

    /* Last, and only now: from this store on, an arriving segment can find
    *  this slot, and everything it will be judged against is in place. */
    c->used = 1;

    /* Armed before the SYN goes out, not after: the SYN,ACK can come back
    *  from the interrupt before tcp_send_syn() has returned, and it stops
    *  this timer. Arming afterwards would start a timer with nothing
    *  outstanding, which fires on an established connection, finds nothing
    *  to retransmit, backs off six times and kills it with a reset. */
    tcp_timer_arm(c);

    tcp_send_syn(c);

    if (c->unsent_tries > 0)
    {
        /* Almost always ARP: the first connection after boot cannot resolve
        *  the next hop in time, because the request that will resolve it is
        *  the one the failed send just triggered. tcp_poll() retries in
        *  50 ms and does not count it as an attempt. */
        c->rtx_timeout = TCP_UNSENT_MS;
    }

    tcp_error = "";

    return tcp_handle_of(index);
}

int tcp_send(int handle, const void *data, uint32_t len)
{
    tcp_conn *c;
    uint32_t room;
    uint32_t n;

    c = tcp_lookup(handle);
    if (c == 0)
    {
        tcp_error = "not an open connection";
        return TCP_ENOCONN;
    }

    if (data == 0)
    {
        tcp_error = "nothing to send";
        return TCP_EFAILED;
    }

    /* CLOSE_WAIT is included on purpose: the peer closing its direction says
    *  nothing about ours, and an HTTP client that has had a response is
    *  still entitled to send. What is not allowed is sending after our own
    *  close -- the FIN has already claimed a sequence number and the bytes
    *  would sit behind it forever. */
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
    {
        tcp_error = (c->state == TCP_SYN_SENT)
                    ? "the connection is not established yet"
                    : "the connection is closing or closed";
        return (c->state == TCP_SYN_SENT) ? 0 : TCP_ECLOSED;
    }

    if (c->fin_queued || c->snd_buf == 0)
    {
        tcp_error = "this end of the connection is already closed";
        return TCP_ECLOSED;
    }

    if (len == 0)
        return 0;

    /* snd_una only moves forward, and only in the interrupt, so a value read
    *  here can be stale in exactly one direction: it makes the buffer look
    *  fuller than it is. Taking fewer bytes than we could is not a bug, and
    *  the caller is already required to look at the count. */
    room = (uint32_t)TCP_SND_BUF - seq_diff(c->snd_una, c->snd_end);

    n = len;
    if (n > room)
        n = room;

    if (n == 0)
        return 0;                        /* full: the caller sends the rest
                                         *  later, as tcp.h says it must */

    tcp_ring_put(c->snd_buf, TCP_SND_MASK, c->snd_end,
                 (const uint8_t *)data, n);

    c->snd_end = c->snd_end + n;

    tcp_output(c);

    return (int)n;
}

int tcp_recv(int handle, void *buf, uint32_t len)
{
    tcp_conn *c;
    uint32_t have;
    uint32_t n;
    uint16_t before;

    c = tcp_lookup(handle);
    if (c == 0)
    {
        tcp_error = "not an open connection";
        return TCP_ENOCONN;
    }

    if (buf == 0)
    {
        tcp_error = "nowhere to put the data";
        return TCP_EFAILED;
    }

    /* rcv_end only moves forward, and only in the interrupt, so a stale read
    *  here means we hand over less than has arrived. The next call sees it.
    *  rcv_end and not rcv_nxt: the peer's FIN is in the second and is not a
    *  byte anybody may read. */
    have = seq_diff(c->rcv_rd, c->rcv_end);

    if (have > 0 && c->rcv_buf != 0 && len > 0)
    {
        before = tcp_window(c);

        n = have;
        if (n > len)
            n = len;

        tcp_ring_get(c->rcv_buf, TCP_RCV_MASK, c->rcv_rd,
                     (uint8_t *)buf, n);

        c->rcv_rd = c->rcv_rd + n;

        /* The window just grew. If it had closed, or nearly, the peer is
        *  waiting to be told -- and the segment that tells it is a bare
        *  acknowledgement, which nothing retransmits. Sending it here, from
        *  task context, is the one place it can be sent reliably.
        *
        *  Only when the window was actually tight: an acknowledgement per
        *  read on a connection that is keeping up would double the segment
        *  count for nothing. */
        if (before < c->mss && tcp_window(c) >= c->mss)
            tcp_ack(c, 0);

        tcp_release(c);

        return (int)n;
    }

    /* Nothing to hand over. Whether that is "not yet" or "never again" is
    *  the whole question, and the order tcp.h insists on is that data comes
    *  first: everything the peer sent before its FIN is delivered above,
    *  and only once it has all been read does the close become visible. */
    if (c->rcv_closed || c->reset ||
        c->state == TCP_CLOSED || c->state == TCP_TIME_WAIT ||
        c->state == TCP_CLOSE_WAIT || c->state == TCP_CLOSING ||
        c->state == TCP_LAST_ACK)
    {
        tcp_release(c);
        return TCP_ECLOSED;
    }

    return 0;
}

int tcp_close(int handle)
{
    tcp_conn *c;

    c = tcp_lookup(handle);
    if (c == 0)
    {
        tcp_error = "not an open connection";
        return TCP_ENOCONN;
    }

    switch (c->state)
    {
    case TCP_SYN_SENT:
        /* Nothing was ever established, so there is nothing to close down
        *  politely: the peer either never saw our SYN or is about to answer
        *  one for a connection that no longer exists, and it will get a
        *  reset from tcp_receive() when it does. */
        tcp_kill(c, 0);
        tcp_release(c);
        break;

    case TCP_ESTABLISHED:
        c->fin_queued  = 1;
        c->snd_fin_seq = c->snd_end;
        c->state       = TCP_FIN_WAIT_1;
        c->state_ms    = (uint32_t)timer_get_ticks();
        tcp_output(c);
        break;

    case TCP_CLOSE_WAIT:
        /* The peer closed first, so this is the second half and there is no
        *  TIME_WAIT to serve afterwards -- the side that closes last is the
        *  side whose final acknowledgement comes from the other end. */
        c->fin_queued  = 1;
        c->snd_fin_seq = c->snd_end;
        c->state       = TCP_LAST_ACK;
        c->state_ms    = (uint32_t)timer_get_ticks();
        tcp_output(c);
        break;

    default:
        /* Already closing, waiting, or finished. Closing twice is not an
        *  error; it is what a caller that lost track does, and the answer is
        *  the state it is already in. */
        break;
    }

    return 0;
}

void tcp_abort(int handle)
{
    tcp_conn *c;

    c = tcp_lookup(handle);
    if (c == 0)
        return;

    /* A reset, so the peer learns the connection failed rather than ended --
    *  which is a different thing to whoever is reading at the other end.
    *  Nothing is sent from TIME_WAIT or CLOSED: there is no peer left that
    *  believes in the connection. */
    if (c->state != TCP_CLOSED && c->state != TCP_TIME_WAIT)
        tcp_ctl(c, TCP_RST, 0);

    /* tcp.h says the handle is freed at once, so it is: anything the caller
    *  had not read is gone with it. That is what giving up means, and it is
    *  why tcp_close() exists for callers that have not. */
    tcp_free_slot(c);
}

int tcp_state(int handle)
{
    tcp_conn *c;

    c = tcp_lookup(handle);
    if (c == 0)
        return TCP_ENOCONN;

    return (int)c->state;
}

const char *tcp_state_name(int state)
{
    switch (state)
    {
    case TCP_CLOSED:      return "CLOSED";
    case TCP_SYN_SENT:    return "SYN_SENT";
    case TCP_ESTABLISHED: return "ESTABLISHED";
    case TCP_FIN_WAIT_1:  return "FIN_WAIT_1";
    case TCP_FIN_WAIT_2:  return "FIN_WAIT_2";
    case TCP_CLOSING:     return "CLOSING";
    case TCP_TIME_WAIT:   return "TIME_WAIT";
    case TCP_CLOSE_WAIT:  return "CLOSE_WAIT";
    case TCP_LAST_ACK:    return "LAST_ACK";
    default:              return "unknown";
    }
}

const char *tcp_last_error(void)
{
    return tcp_error;
}

int tcp_conn_count(void)
{
    int count;
    int i;

    count = 0;

    for (i = 0; i < TCP_MAX_CONNS; i++)
    {
        if (tcp_conns[i].used)
            count++;
    }

    return count;
}

int tcp_conn_get(int index, int *handle, uint32_t *peer, uint16_t *peer_port,
                 uint16_t *local_port, int *state,
                 uint32_t *sent, uint32_t *received)
{
    tcp_conn *c;
    int seen;
    int i;

    if (index < 0)
        return -1;

    seen = 0;

    for (i = 0; i < TCP_MAX_CONNS; i++)
    {
        if (!tcp_conns[i].used)
            continue;

        if (seen != index)
        {
            seen++;
            continue;
        }

        c = &tcp_conns[i];

        if (handle != 0)
            *handle = tcp_handle_of(i);
        if (peer != 0)
            *peer = c->peer_ip;
        if (peer_port != 0)
            *peer_port = c->peer_port;
        if (local_port != 0)
            *local_port = c->local_port;
        if (state != 0)
            *state = (int)c->state;

        /* "sent" is what the peer admitted to having, not what the caller
        *  handed over: bytes in the send ring have been queued and may still
        *  be lost, and a counter that claimed them would be reporting hope.
        *  "received" is what reached the receive ring, read or not. */
        if (sent != 0)
            *sent = c->bytes_sent;
        if (received != 0)
            *received = c->bytes_received;

        return 0;
    }

    return -1;
}

uint32_t tcp_segments_sent(void)
{
    return stat_tx;
}

uint32_t tcp_segments_received(void)
{
    return stat_rx;
}

uint32_t tcp_segments_dropped(void)
{
    return stat_drop;
}

uint32_t tcp_retransmits(void)
{
    return stat_rtx;
}
