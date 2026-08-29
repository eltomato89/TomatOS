/* TomatOS - Network stack: ethernet and ARP
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: The bottom two layers of the stack, plus the byte order and
*        checksum helpers the layers above share.
*
*  What lives here: the conversion helpers, the one's complement checksum,
*  the ethernet framing in both directions and the whole of ARP. IP and ICMP
*  live in ip.c and are reached through ip_receive() below.
*
*  Two conventions run through this file and every caller has to know them:
*
*    - Byte order. Everything inside a packed header is in NETWORK order and
*      is only ever read through ntohs()/ntohl() or written through
*      htons()/htonl(). Everything OUTSIDE a packet -- the addresses in
*      net_configure(), net_ip(), ip_send(), the ARP cache, every uint32_t
*      address in an interface here -- is in HOST order. The conversion
*      happens at exactly one place per field, where the value is copied
*      into or out of a header. A field converted twice, or not at all, is
*      the classic first-stack bug: the packet leaves the machine looking
*      plausible and nothing ever answers it.
*
*    - Two contexts, one boundary. net_receive() is called by the card
*      driver from inside its interrupt handler and does nothing but check
*      the addressing and copy the frame into the receive queue. Everything
*      above it -- ARP handling, the reply it sends, ip_receive() and the
*      whole of what hangs off it -- runs in net_queue_drain(), in a task,
*      with interrupts on. The queue between the two is the only place in
*      this file where the two contexts meet, and the rules that make that
*      meeting safe are written out above it.
*
*      What did not change is that net_send() and the ARP cache are still
*      reached from both sides -- a task sends, and net_receive() reads
*      net_hwaddr while doing so -- which is what irq_save() below is for.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <mm.h>
#include <net.h>
#include <rtl8139.h>

/* --- The boundary to ip.c -------------------------------------------------
*
*  Handing a received IP packet upward. net.h does not declare this because
*  it is internal to the stack -- nothing outside net.c and ip.c may call it.
*  The pointer is to the first byte after the ethernet header, len is what is
*  left of the frame from there on, and ip.c must not hold on to the buffer:
*  it belongs to the drain's staging buffer and is overwritten by the next
*  frame we hand up.
*
*  Called from TASK context now, out of net_queue_drain(), not from the
*  card's interrupt any more. The rule about not keeping the buffer is the
*  one thing that did not change with that.
*/
extern void ip_receive(const uint8_t *packet, uint32_t len);

/* --- Constants ----------------------------------------------------------- */

/* Shortest frame the wire allows, without the trailing checksum the card
*  appends itself. Anything smaller is padded with zeros -- with zeros, not
*  with whatever the send buffer held last time, which would leak the
*  previous packet's tail onto the network. */
#define ETH_FRAME_MIN     60

/* ARP, as it appears on ethernet carrying IPv4. */
#define ARP_HW_ETHERNET    1
#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2
#define ARP_PKT_LEN       28

/* How long a learned address stays usable, in milliseconds.
*
*  Two minutes. The tension is that a cache entry is a claim about a machine
*  we cannot see: too long and we keep sending to a MAC that moved to another
*  port or another machine entirely (a swapped NIC, a failed-over router, a
*  DHCP lease that changed hands), too short and every burst of traffic pays
*  for a fresh round trip. Two minutes is the middle that common stacks
*  settle on -- long enough that a ping series or a file transfer resolves
*  once, short enough that a machine we have wrong is forgotten while someone
*  is still typing the command that failed. */
#define ARP_ENTRY_TTL_MS   120000UL

/* How long an unanswered request may sit in the cache before we give up on
*  it and let the slot be reused. */
#define ARP_PENDING_TTL_MS   3000UL

/* Minimum spacing between two requests for the same address. arp_lookup()
*  sends a request on every miss and the caller is expected to retry, so
*  without this a caller polling in a loop would put thousands of requests a
*  second on the wire. */
#define ARP_RETRY_MS         1000UL

/* Cache entry states */
#define ARP_STATE_FREE     0
#define ARP_STATE_PENDING  1   /* request sent, no reply yet          */
#define ARP_STATE_VALID    2   /* mac is usable until it expires      */

typedef struct
{
    uint32_t ip;               /* HOST order                          */
    uint8_t  mac[ETH_ALEN];
    int      state;
    uint32_t stamp;            /* when the mac was learned, ms        */
    uint32_t asked;            /* when we last sent a request, ms     */
} arp_entry;

/* The ARP packet as it sits on the wire. Packed and read through the
*  conversion helpers -- note that sender_ip and target_ip are not naturally
*  aligned inside the frame, which is exactly what packed is here for. */
typedef struct
{
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;
} __attribute__((packed)) arp_packet;

/* --- State --------------------------------------------------------------- */

static uint8_t  net_hwaddr[ETH_ALEN];
static uint32_t net_addr    = 0;        /* our IP, host order         */
static uint32_t net_mask    = 0;
static uint32_t net_gw      = 0;
static int      net_is_up   = 0;

static arp_entry arp_cache[ARP_CACHE_SIZE];

/* Counters, and which context owns each one.
*
*  Every one of these is a read-modify-write, so a counter touched from both
*  the interrupt and a task would lose increments whenever the interrupt
*  lands between the load and the store. Nothing terrible follows from a
*  lost count, but the fix is free: the drop counter is split in two, one
*  per context, and net_rx_dropped() adds them up. stat_tx is incremented
*  inside net_send()'s critical section and needs nothing further.
*
*  stat_rx counts every frame that passed the addressing check, exactly as
*  it did when net_receive() did the protocol work itself -- including the
*  ones the queue then had no room for, which stat_overrun counts as well.
*  So rx - overrun is what actually reached the protocol layers. */
static uint32_t stat_rx       = 0;   /* irq  */
static uint32_t stat_tx       = 0;   /* both, under irq_save()             */
static uint32_t stat_drop_irq = 0;   /* irq  */
static uint32_t stat_drop_tsk = 0;   /* task */
static uint32_t stat_overrun  = 0;   /* irq  */

/* The frame we assemble outgoing packets in. One buffer, shared between
*  task context and the ARP replies that go out from the interrupt handler,
*  which is why net_send() builds and hands it over with interrupts off. */
static uint8_t  tx_frame[ETH_FRAME_MAX];

static const uint8_t mac_broadcast[ETH_ALEN] =
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t mac_zero[ETH_ALEN] =
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* --- Byte order ----------------------------------------------------------
*
*  x86 is little endian, the wire is big endian, so all four of these are
*  real swaps. They are written out rather than done with __builtin_bswap so
*  that -fno-builtin cannot quietly change what they mean.
*/

uint16_t htons(uint16_t v)
{
    return (uint16_t)(((v & 0x00FFU) << 8) | ((v >> 8) & 0x00FFU));
}

uint16_t ntohs(uint16_t v)
{
    return htons(v);
}

uint32_t htonl(uint32_t v)
{
    return ((v & 0x000000FFUL) << 24) |
           ((v & 0x0000FF00UL) <<  8) |
           ((v >>  8) & 0x0000FF00UL) |
           ((v >> 24) & 0x000000FFUL);
}

uint32_t ntohl(uint32_t v)
{
    return htonl(v);
}

/* --- Checksum ------------------------------------------------------------
*
*  The one's complement sum of RFC 1071, which IP and ICMP both use.
*
*  Two details decide whether it is right:
*
*    - Carries out of the 16 bit sum are folded back in, and folding can
*      itself produce a carry, so the fold repeats until nothing is left
*      above bit 15. Folding exactly once is right for almost every buffer
*      and wrong for the ones that matter.
*
*    - A buffer of odd length is padded with a ZERO byte. The tempting
*      shortcut -- reading one 16 bit word past the end and masking later --
*      pulls in whatever happens to follow in memory, which gives a checksum
*      that depends on unrelated data and fails only sometimes.
*
*  The return value is in NETWORK order and can be assigned straight into a
*  header's checksum field. Verifying an incoming header keeps working
*  either way: run the sum over the header with its checksum field left in
*  place and the result is zero when it is intact.
*/
uint16_t net_checksum(const void *data, uint32_t len)
{
    const uint8_t *p;
    uint32_t sum;

    sum = 0;
    p = (const uint8_t *)data;

    while(len > 1)
    {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p += 2;
        len -= 2;

        /* Keep the accumulator from overflowing on a very long buffer. A
        *  32 bit sum only runs out after 128 KiB of 0xFFFF words, well
        *  past an ethernet frame, but folding early costs one test and
        *  removes the limit rather than documenting it. */
        if(sum & 0x80000000UL)
            sum = (sum & 0xFFFFUL) + (sum >> 16);
    }

    /* The odd trailing byte is the high half of a word whose low half is
    *  zero -- the pad, not the next byte in memory. */
    if(len == 1)
        sum += (uint32_t)p[0] << 8;

    while(sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    return htons((uint16_t)(~sum & 0xFFFFUL));
}

/* --- Small helpers -------------------------------------------------------- */

/* Uptime in milliseconds. timer_get_ticks() returns an int and wraps after
*  roughly 25 days; every age below is computed as a difference in uint32_t,
*  which stays correct across that wrap. */
static uint32_t net_now(void)
{
    return (uint32_t)timer_get_ticks();
}

static int mac_equal(const uint8_t *a, const uint8_t *b)
{
    int i;

    for(i = 0; i < ETH_ALEN; i++)
        if(a[i] != b[i])
            return 0;
    return 1;
}

/* A MAC in the usual notation. The kernel's printf() knows %X but has
*  neither %x nor a field width, so "%02x" would print the literal text and
*  swallow no argument at all -- the rest of the line would then take its
*  values from the wrong places. Formatting it here sidesteps that. */
static const char *mac_string(const uint8_t *m)
{
    static const char hex[] = "0123456789abcdef";
    static char buf[18];
    int i;
    int p;

    p = 0;
    for(i = 0; i < ETH_ALEN; i++)
    {
        if(i > 0)
            buf[p++] = ':';
        buf[p++] = hex[(m[i] >> 4) & 0x0F];
        buf[p++] = hex[m[i] & 0x0F];
    }
    buf[p] = '\0';
    return buf;
}

/* Bit 0 of the first byte is the group bit: set means multicast, and the
*  broadcast address is the all-ones case of it. No machine may ever claim
*  such an address as its own, so a packet that says otherwise is either
*  broken or hostile. */
static int mac_is_group(const uint8_t *m)
{
    return (m[0] & 0x01) != 0;
}

/* --- Interrupt guard -----------------------------------------------------
*
*  net_send() and the ARP cache are reached from both task context and the
*  card's interrupt handler. Saving the flags and clearing IF is the only
*  form that works in both: a plain enable() at the end would switch
*  interrupts on inside an interrupt handler that was entered with them off.
*/

static uint32_t irq_save(void)
{
    uint32_t flags;

    __asm__ __volatile__ ("pushfl; popl %0; cli"
                          : "=r" (flags) : : "memory");
    return flags;
}

static void irq_restore(uint32_t flags)
{
    __asm__ __volatile__ ("pushl %0; popfl"
                          : : "r" (flags) : "memory", "cc");
}

/* --- The receive queue ---------------------------------------------------
*
*  One producer in interrupt context, one consumer in task context, and no
*  lock between them. What follows is why that is safe, because it is not
*  obvious and it is the only thing standing between this file and frames
*  that arrive half written.
*
*  SHAPE. A byte ring with length prefixed records, not an array of frame
*  sized slots. A slot array sized for the worst case would be 1518 bytes
*  per frame while the frames that actually turn up -- an ACK, an ARP reply,
*  a DNS answer -- are 60 to 100, so nine tenths of it would be padding, and
*  a queue that holds twenty frames whatever their size is the wrong queue
*  for a stack whose bursts are made of small ones. A byte ring holds what
*  it is given: twenty one full sized frames, or five hundred acknowledge-
*  ments, out of the same memory.
*
*  Each record is a two byte length in host order followed by that many
*  bytes of frame. Two bytes because the longest frame is 1518.
*
*  INDICES. Both are free running byte counters that are never reduced
*  modulo anything; the position of a byte is index & NET_RXQ_MASK, which is
*  why the size has to be a power of two. Occupancy is head - tail in
*  unsigned arithmetic and stays right when the counters wrap, so there is
*  no "full or empty?" ambiguity to resolve and no byte to sacrifice to it.
*  This is what tcp.c does with its two rings, for the same reason -- see
*  the note on sequence indexed buffers at the top of that file -- and doing
*  it differently here would mean two idioms for one problem.
*
*  WHO WRITES WHAT. Exactly one context writes each variable:
*
*      rxq_head   the interrupt only. Where the next frame will be written.
*      rxq_tail   the task only.      Where the next frame will be read.
*      rxq_in     the interrupt only. Frames enqueued, ever.
*      rxq_out    the task only.      Frames dequeued, ever.
*      rxq_peak   the interrupt only. High water mark, in bytes.
*
*  Neither side ever writes the other's, and each reads the other's exactly
*  once per operation into a local. That is what removes the need to mask
*  interrupts on the common path: there is no read-modify-write for an
*  interrupt to land in the middle of. A 32 bit aligned load or store cannot
*  be torn on this machine, so a stale read is the worst that happens, and a
*  stale read is always conservative -- the producer believes there is less
*  room than there is, the consumer believes there is less data than there
*  is, and the next call sees the truth. The counters wrapping past 2^32
*  changes none of that, since only differences are ever computed.
*
*  Note what this rules out: the queue must not be drained by two tasks at
*  once, because rxq_tail would then be advanced by two readers. That is a
*  task against task race, not an interrupt one, and net_queue_drain()
*  refuses the second caller rather than pretending it cannot happen.
*
*  PUBLICATION ORDER. The frame is written into the ring at [head, head+n)
*  -- memory the consumer does not look at, because it only reads below
*  head -- and only then does head move. So a partially written frame is
*  never visible: the length, the bytes and the index that publishes them
*  are stored in that order, with a compiler barrier before the last one.
*  The barrier is against the compiler alone. Producer and consumer are the
*  same CPU with an interrupt in between, and an interrupt is a serialising
*  event, so there is no store buffer to flush and no fence to issue -- the
*  volatile qualifiers plus that barrier are the whole of the ordering.
*
*  THE WRAP. A record that would run past the end of the ring is SPLIT: the
*  part that fits goes at the end, the rest at the beginning, and the two
*  byte length can itself be split down the middle. The alternative is to
*  leave the tail unused and start the record at zero, which needs a marker
*  record so the reader knows to skip -- and a marker is a length that is
*  not a length, so every reader has to know a reserved value. Splitting
*  needs neither: it costs one extra memcpy in the rare case, wastes not one
*  byte, and it is again what tcp_ring_put()/tcp_ring_get() already do.
*
*  OVERFLOW. When the room is not there the newest frame is dropped and
*  net_rx_overrun() counts it. Dropping the oldest instead would mean the
*  interrupt moving rxq_tail, which is the consumer's variable and possibly
*  in use by a consumer that was preempted halfway through reading the very
*  record we would be reclaiming -- it would buy a slightly nicer loss
*  policy at the price of the entire argument above. Keeping the oldest is
*  also the better answer for what runs on top: the frames already in the
*  queue are the earlier ones, so what a burst loses is its tail, which is
*  what a lost frame on the wire looks like anyway and what TCP recovers
*  from most cheaply. A drop is not counted in net_rx_dropped(): that one
*  means a frame the protocols rejected, and this one means the machine did
*  not keep up, which is a different problem with a different answer.
*/

/* 32 KiB, the same size as the card's own receive ring.
*
*  The upper bound on what can legitimately be in flight towards us is a
*  TCP receive window -- 8 KiB -- plus its headers, about 8.6 KiB on the
*  wire, so one connection cannot fill this queue even if the drain does not
*  run for the whole time the window is open. Matching the card's ring is
*  the other half of the argument: whichever of the two fills first, the
*  answer is the same one, and there is no point in a second stage so much
*  smaller than the first that it becomes the bottleneck.
*
*  It comes from the heap, and that is deliberate: the .bss of this kernel
*  was cut from 341 KB to 67 KB on purpose, and 32 KB of it would go
*  straight back for a buffer that a machine with no card never touches.
*  From the heap it is paid for only once the stack is actually up. */
#define NET_RXQ_SIZE      32768UL
#define NET_RXQ_MASK      (NET_RXQ_SIZE - 1UL)

/* The length prefix in front of every record. */
#define NET_RXQ_HDR       2UL

/* One allocation, two uses: the ring, and behind it the buffer the drain
*  reassembles a split frame into. Separate mallocs would work as well; one
*  is simply one thing to fail and one thing to free. The driver's ring has
*  the same slack behind it for the same reason. */
#define NET_RXQ_ALLOC     (NET_RXQ_SIZE + (uint32_t)ETH_FRAME_MAX)

/* The masking above is only a modulo when the size is a power of two. This
*  fails the build rather than the network if that ever stops being true. */
typedef char net_rxq_size_is_power_of_two
             [((NET_RXQ_SIZE & (NET_RXQ_SIZE - 1UL)) == 0) ? 1 : -1];

/* How many frames one net_queue_drain() call will handle before returning.
*
*  Not a limit on throughput: the task calls it again immediately while it
*  returns a full burst. It exists so that the function always terminates,
*  even against a sender that can fill the queue faster than we empty it --
*  an unbounded loop would be at the mercy of the wire for when it ends. */
#define NET_DRAIN_BURST   64

/* How long the drain task sleeps once the queue is empty, in milliseconds.
*
*  There are no wait queues in this kernel, so the task is not woken when a
*  frame arrives: it polls. One tick is the shortest poll the 1000 Hz timer
*  can express and it is what this uses, because the interval is pure added
*  latency on every acknowledgement, every echo reply and every DNS answer,
*  and one millisecond is already more than the round trip to a gateway on
*  the same segment.
*
*  What it costs is a wakeup per tick, and that is genuinely small: the
*  timer interrupt happens at that rate regardless, the task's whole turn
*  when the queue is empty is one comparison, and sleep() spends the rest of
*  it in hlt. What the interval does NOT cost is throughput, because the
*  task drains until the queue is empty before it sleeps at all -- only the
*  first frame after an idle period waits, never the second and the
*  hundredth of a burst.
*
*  The honest caveat, and it is the bigger half: this interval is not what
*  decides the latency. The scheduler is round robin with a fixed slice per
*  task and no notion of one task being more urgent than another, and a task
*  waiting in sleep() still holds its slice to the end -- so the distance
*  between two drains is the sum of every other runnable task's slice. With
*  the console at 20 ticks and the status bar at 10, that is about 31 ms,
*  and it shows: the same ping that answers in 0 ms with the protocol work
*  in the interrupt answers in 33 ms with it in a task. That is the price of
*  this change, it is paid to the scheduler rather than to the queue, and no
*  sleep interval can undo it. What the interval has to do is not be the
*  term that decides -- one tick against thirty is not. */
#define NET_DRAIN_SLEEP_MS 1

static uint8_t *rxq_buf   = 0;      /* the ring, NET_RXQ_SIZE bytes         */
static uint8_t *rxq_frame = 0;      /* staging for one frame, behind it     */

static volatile uint32_t rxq_head = 0;   /* irq writes,  task reads         */
static volatile uint32_t rxq_tail = 0;   /* task writes, irq reads          */
static volatile uint32_t rxq_in   = 0;   /* irq writes,  task reads         */
static volatile uint32_t rxq_out  = 0;   /* task writes, irq never reads    */
static volatile uint32_t rxq_peak = 0;   /* irq writes                      */

/* Held while a task is inside net_queue_drain(). See the note there. */
static int rxq_draining = 0;

/* The pid of the drain task, or -1. Kept so a second net_init() does not
*  create a second drainer -- two of them would be exactly the case the
*  single writer rule above forbids -- and so that it can be made runnable
*  later than it is created; see net_drain_start() for why it has to be. */
static int rxq_task    = -1;
static int rxq_running = 0;

/* Makes the drain task runnable, once and not before it is safe to.
*
*  Creating the task in net_init() is free -- taskmgr_add_task() leaves the
*  slot suspended and the scheduler cannot elect what it cannot see -- but
*  STARTING it there is not, and the reason is worth writing down because
*  the symptom is a machine that boots to a blank screen with a perfectly
*  working network on it.
*
*  net_init() runs on the boot path, from kernel.c's network_init(), which
*  is called before the console task is created. That path is not a task: it
*  runs on the boot stack. schedule() saves the context it was interrupted
*  from only when there is a current task to save it into, and until the
*  first switch there is none -- so the first tick after ANY task becomes
*  runnable elects that task and throws the boot away. Everything kernel.c
*  does after network_init(), the console task included, would never happen.
*
*  So the task is made runnable at the first moment the scheduler is
*  demonstrably already running: taskmgr_get_currpid() returns -1 until it
*  has elected somebody, and the somebody it elects first is the console
*  task, because that is the first task the kernel creates. Testing it costs
*  a load and a branch, and only until the answer is yes.
*
*  The two callers are net_send() and net_receive(), which is every use of
*  the stack in either direction. Whichever comes first starts the drain,
*  and until one of them does there is nothing for it to drain. */
static void net_drain_start(void)
{
    if(rxq_running || rxq_task < 0)
        return;
    if(taskmgr_get_currpid() < 0)
        return;

    rxq_running = 1;
    taskmgr_task_start(rxq_task);
}

/* Keeps the compiler from moving the stores that fill a record past the
*  store that publishes it. Nothing is emitted; the clobber is the point. */
static void net_barrier(void)
{
    __asm__ __volatile__ ("" : : : "memory");
}

/* len bytes into the ring at the free running position at, split across the
*  end when it lands there. The one thing that matters about both of these
*  is that neither may touch a byte outside the ring, whatever position and
*  length they are handed -- a length of zero and a position on the last
*  byte included, neither of which the receive path produces but both of
*  which the split arithmetic has to survive. */
static void rxq_put(uint32_t at, const uint8_t *src, uint32_t len)
{
    uint32_t off;
    uint32_t first;

    off = at & NET_RXQ_MASK;
    first = NET_RXQ_SIZE - off;
    if(first > len)
        first = len;

    memcpy((void *)(rxq_buf + off), (const void *)src, (size_t)first);
    if(len > first)
        memcpy((void *)rxq_buf, (const void *)(src + first),
               (size_t)(len - first));
}

static void rxq_get(uint32_t at, uint8_t *dst, uint32_t len)
{
    uint32_t off;
    uint32_t first;

    off = at & NET_RXQ_MASK;
    first = NET_RXQ_SIZE - off;
    if(first > len)
        first = len;

    memcpy((void *)dst, (const void *)(rxq_buf + off), (size_t)first);
    if(len > first)
        memcpy((void *)(dst + first), (const void *)rxq_buf,
               (size_t)(len - first));
}

/* Allocates the queue. Returns 0 when the heap has nothing, which is not a
*  crash and not a panic: net_init() reports it and leaves the stack down. */
static int net_queue_init(void)
{
    rxq_head = 0;
    rxq_tail = 0;
    rxq_in = 0;
    rxq_out = 0;
    rxq_peak = 0;
    rxq_draining = 0;

    if(rxq_buf != 0)
        return 1;                /* a second net_init(); keep the one queue */

    rxq_buf = (uint8_t *)malloc((size_t)NET_RXQ_ALLOC);
    if(rxq_buf == 0)
    {
        rxq_frame = 0;
        return 0;
    }

    rxq_frame = rxq_buf + NET_RXQ_SIZE;
    return 1;
}

static void net_queue_free(void)
{
    uint8_t *p;

    p = rxq_buf;
    rxq_buf = 0;
    rxq_frame = 0;
    free((void *)p);
}

uint32_t net_queue_capacity(void)
{
    return (rxq_buf == 0) ? 0UL : NET_RXQ_SIZE;
}

uint32_t net_queue_used(void)
{
    return rxq_head - rxq_tail;
}

uint32_t net_queue_peak(void)
{
    return rxq_peak;
}

int net_queue_frames(void)
{
    return (int)(rxq_in - rxq_out);
}

uint32_t net_rx_overrun(void)
{
    return stat_overrun;
}

/* The task. Drain until there is nothing left, then wait a tick.
*
*  Draining to empty rather than once per sleep is what keeps a burst from
*  being paced at one frame per millisecond, which would be slower than the
*  interrupt handler it replaces and would show up as a stalled download
*  rather than as a statistic. Under a sustained flood the inner loop simply
*  does not end and the task never sleeps -- correct, and not a hang: the
*  timer still preempts it, and every other task keeps its share. */
static void net_rx_task(void)
{
    for(;;)
    {
        while(net_queue_drain() > 0)
            ;

        sleep(NET_DRAIN_SLEEP_MS);
    }
}

/* --- Configuration -------------------------------------------------------- */

void net_init(void)
{
    const uint8_t *mac;
    int i;

    for(i = 0; i < ARP_CACHE_SIZE; i++)
    {
        arp_cache[i].state = ARP_STATE_FREE;
        arp_cache[i].ip = 0;
        arp_cache[i].stamp = 0;
        arp_cache[i].asked = 0;
        memset((char *)arp_cache[i].mac, 0, ETH_ALEN);
    }

    stat_rx = 0;
    stat_tx = 0;
    stat_drop_irq = 0;
    stat_drop_tsk = 0;
    stat_overrun = 0;
    net_is_up = 0;
    memset((char *)net_hwaddr, 0, ETH_ALEN);

    /* The driver normally probes during boot; if nobody has, give it the
    *  one chance here. Everything after this point tolerates a machine
    *  with no card -- net_up() simply stays false and net_send() refuses. */
    if(!rtl8139_present())
        rtl8139_init();

    if(!rtl8139_present())
    {
        printf("net: no network card, stack stays down\n");
        return;
    }

    mac = rtl8139_mac();
    if(mac == 0)
    {
        printf("net: card present but has no MAC address\n");
        return;
    }

    memcpy((void *)net_hwaddr, (const void *)mac, ETH_ALEN);

    /* A card that reports all zeros or a group address has not been read
    *  out properly; sending from such an address gets us nowhere. */
    if(mac_is_group(net_hwaddr) || mac_equal(net_hwaddr, mac_zero))
    {
        printf("net: card reported an unusable MAC address\n");
        memset((char *)net_hwaddr, 0, ETH_ALEN);
        return;
    }

    /* The queue and the task that drains it. Neither existed when this file
    *  was written and the stack cannot run without them any more: with no
    *  queue there is nowhere to put an arriving frame, and with no drain
    *  nobody would ever look at one. Both failures are reported the same
    *  way as a missing card -- the machine boots, ifconfig says the stack
    *  is down, and nothing else in the kernel is any the wiser.
    *
    *  Order matters in one direction only: the buffer has to exist before
    *  the task can run, since the task calls net_queue_drain() immediately.
    *  net_is_up stays false until both are in place, which is what keeps
    *  net_receive() from filling a queue nobody drains. */
    if(!net_queue_init())
    {
        printf("net: no memory for the receive queue, stack stays down\n");
        return;
    }

    /* TASK_PRIORITY_LOW, and that is not modesty. Priority in this kernel
    *  is the length of a task's slice, not a right to run before anybody
    *  else: the scheduler is round robin over every runnable slot, so what
    *  decides how soon a frame is looked at is how long the OTHER tasks
    *  hold the CPU, and the only thing a bigger slice here would do is
    *  lengthen that cycle for everyone. One tick is the smallest
    *  contribution to it and is still far more than a drain needs -- the
    *  whole queue is 21 frames at its worst, and the task sleeps as soon as
    *  it is empty. */
    if(rxq_task < 0)
        rxq_task = taskmgr_add_task((void *)net_rx_task, "NET RX",
                                    TASK_PRIORITY_LOW);
    if(rxq_task < 0)
    {
        printf("net: no task for the receive queue, stack stays down\n");
        net_queue_free();
        return;
    }

    /* Not started here. See net_drain_start(). */
    net_drain_start();

    net_is_up = 1;

    printf("net: up on %s\n", mac_string(net_hwaddr));
}

int net_up(void)
{
    return net_is_up;
}

/* Addresses arrive and leave in HOST order -- 192.168.0.2 is 0xC0A80002,
*  whatever the machine's endianness. */
void net_configure(uint32_t ip, uint32_t netmask, uint32_t gateway)
{
    net_addr = ip;
    net_mask = netmask;
    net_gw = gateway;
}

uint32_t net_ip(void)
{
    return net_addr;
}

uint32_t net_netmask(void)
{
    return net_mask;
}

uint32_t net_gateway(void)
{
    return net_gw;
}

const uint8_t *net_mac(void)
{
    return net_hwaddr;
}

uint32_t net_rx_packets(void)
{
    return stat_rx;
}

uint32_t net_tx_packets(void)
{
    return stat_tx;
}

uint32_t net_rx_dropped(void)
{
    return stat_drop_irq + stat_drop_tsk;
}

/* --- Sending -------------------------------------------------------------
*
*  Builds the ethernet header in front of the payload and hands the frame to
*  the card. Returns 0 on success.
*
*  Safe to call from interrupt context: no printing, no waiting, and the
*  shared build buffer is held only for the few microseconds it takes to
*  copy the payload in.
*/
int net_send(const uint8_t dst_mac[ETH_ALEN], uint16_t type,
             const void *payload, uint32_t len)
{
    eth_header *eth;
    uint32_t flags;
    uint32_t frame_len;
    int rc;

    if(!net_is_up)
        return -1;
    if(dst_mac == 0)
        return -1;
    if(len > ETH_MTU)
        return -1;
    if(len > 0 && payload == 0)
        return -1;

    /* Task context, and the earliest point at which anything uses the
    *  stack: the right moment to let the drain run. */
    net_drain_start();

    flags = irq_save();

    eth = (eth_header *)tx_frame;
    memcpy((void *)eth->dst, (const void *)dst_mac, ETH_ALEN);
    memcpy((void *)eth->src, (const void *)net_hwaddr, ETH_ALEN);
    eth->type = htons(type);

    if(len > 0)
        memcpy((void *)(tx_frame + ETH_HDR_LEN), payload, (size_t)len);

    frame_len = ETH_HDR_LEN + len;

    /* Pad a short frame with zeros. The card would pad it too, but with
    *  what is left in its buffer -- and that is the tail of the previous
    *  packet going out on the wire. */
    if(frame_len < ETH_FRAME_MIN)
    {
        memset((char *)(tx_frame + frame_len), 0,
               (size_t)(ETH_FRAME_MIN - frame_len));
        frame_len = ETH_FRAME_MIN;
    }

    rc = rtl8139_send((const void *)tx_frame, frame_len);
    if(rc == 0)
        stat_tx++;

    irq_restore(flags);
    return rc;
}

/* --- ARP cache ------------------------------------------------------------ */

/* Drops what has timed out. Bounded by the cache size, so it is cheap
*  enough to run at the head of every cache operation and saves every other
*  place from having to think about age. */
static void arp_expire(uint32_t now)
{
    int i;

    for(i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if(arp_cache[i].state == ARP_STATE_VALID)
        {
            if((now - arp_cache[i].stamp) >= ARP_ENTRY_TTL_MS)
                arp_cache[i].state = ARP_STATE_FREE;
        }
        else if(arp_cache[i].state == ARP_STATE_PENDING)
        {
            if((now - arp_cache[i].asked) >= ARP_PENDING_TTL_MS)
                arp_cache[i].state = ARP_STATE_FREE;
        }
    }
}

static int arp_find(uint32_t ip)
{
    int i;

    for(i = 0; i < ARP_CACHE_SIZE; i++)
        if(arp_cache[i].state != ARP_STATE_FREE && arp_cache[i].ip == ip)
            return i;
    return -1;
}

/* Picks a slot for a new entry. A free one first, then the oldest pending
*  one -- a request nobody answered is worth less than an address we know.
*  Only when evict is set do we throw out a valid entry, and then the least
*  recently learned one. */
static int arp_alloc(int evict, uint32_t now)
{
    int i;
    int best;
    uint32_t best_age;
    uint32_t age;

    for(i = 0; i < ARP_CACHE_SIZE; i++)
        if(arp_cache[i].state == ARP_STATE_FREE)
            return i;

    best = -1;
    best_age = 0;
    for(i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if(arp_cache[i].state != ARP_STATE_PENDING)
            continue;
        age = now - arp_cache[i].asked;
        if(best < 0 || age > best_age)
        {
            best = i;
            best_age = age;
        }
    }
    if(best >= 0)
        return best;

    if(!evict)
        return -1;

    best = -1;
    best_age = 0;
    for(i = 0; i < ARP_CACHE_SIZE; i++)
    {
        age = now - arp_cache[i].stamp;
        if(best < 0 || age > best_age)
        {
            best = i;
            best_age = age;
        }
    }
    return best;
}

/* Records an address pair.
*
*  Called for every ARP packet that reaches us, request or reply, addressed
*  to us or not -- the mapping in a request we merely overhear is as good as
*  the one in a reply, and taking it costs nothing but keeps us from asking
*  for an address the neighbour just announced.
*
*  What it will not do is let that overheard traffic push out entries we
*  actually use: a packet not aimed at us may refresh an existing entry or
*  take a spare slot, but never evict a valid one. Otherwise a busy segment
*  would keep flushing the cache with addresses we have no interest in.
*
*  Guards, in order: an address that is a group address or all zeros cannot
*  belong to a real sender; a packet claiming OUR IP is an address conflict
*  and must not be cached, or the next packet we send to ourselves goes to
*  whoever made the claim; a packet carrying our own MAC is our own frame
*  coming back at us.
*/
static void arp_learn(uint32_t ip, const uint8_t *mac, int for_us, uint32_t now)
{
    int i;

    if(ip == 0)
        return;
    if(mac_is_group(mac) || mac_equal(mac, mac_zero))
        return;
    if(net_addr != 0 && ip == net_addr)
        return;
    if(mac_equal(mac, net_hwaddr))
        return;

    i = arp_find(ip);
    if(i < 0)
    {
        i = arp_alloc(for_us, now);
        if(i < 0)
            return;
        arp_cache[i].ip = ip;
        arp_cache[i].asked = now;
    }

    memcpy((void *)arp_cache[i].mac, (const void *)mac, ETH_ALEN);
    arp_cache[i].state = ARP_STATE_VALID;
    arp_cache[i].stamp = now;
}

/* Broadcasts "who has ip, tell us". */
void arp_request(uint32_t ip)
{
    arp_packet pkt;

    if(!net_is_up || net_addr == 0)
        return;

    pkt.hw_type    = htons(ARP_HW_ETHERNET);
    pkt.proto_type = htons(ETH_TYPE_IP);
    pkt.hw_len     = ETH_ALEN;
    pkt.proto_len  = 4;
    pkt.opcode     = htons(ARP_OP_REQUEST);
    memcpy((void *)pkt.sender_mac, (const void *)net_hwaddr, ETH_ALEN);
    pkt.sender_ip  = htonl(net_addr);
    /* The target MAC is what we are asking for, so it goes out as zeros --
    *  not as the broadcast address, which belongs in the ethernet header. */
    memset((char *)pkt.target_mac, 0, ETH_ALEN);
    pkt.target_ip  = htonl(ip);

    net_send(mac_broadcast, ETH_TYPE_ARP, (const void *)&pkt, ARP_PKT_LEN);
}

/* Returns the MAC for ip, or 0.
*
*  A miss is NOT an error and resolution is NOT instant: the request goes out
*  here and the answer arrives some milliseconds later in another interrupt.
*  A caller that needs the address has to come back for it, roughly:
*
*      mac = arp_lookup(ip);
*      if(mac == 0) { give the reply time to arrive, then try again }
*
*  Repeated calls are therefore expected and cheap -- the request itself is
*  rate limited to one per ARP_RETRY_MS per address, so a caller polling in a
*  tight loop cannot turn a miss into a broadcast storm.
*/
const uint8_t *arp_lookup(uint32_t ip)
{
    uint32_t now;
    uint32_t flags;
    const uint8_t *result;
    int i;
    int ask;

    if(!net_is_up || ip == 0)
        return 0;

    /* Our own address never needs asking for. */
    if(ip == net_addr)
        return net_hwaddr;

    /* The broadcast address of our subnet, and the all-ones one, go to the
    *  broadcast MAC directly -- nobody answers an ARP request for them. */
    if(ip == 0xFFFFFFFFUL ||
       (net_mask != 0 && ip == (net_addr | ~net_mask)))
        return mac_broadcast;

    result = 0;
    ask = 0;
    now = net_now();

    flags = irq_save();

    arp_expire(now);
    i = arp_find(ip);

    if(i >= 0 && arp_cache[i].state == ARP_STATE_VALID)
    {
        result = arp_cache[i].mac;
    }
    else
    {
        if(i < 0)
        {
            i = arp_alloc(1, now);
            if(i >= 0)
            {
                arp_cache[i].ip = ip;
                arp_cache[i].state = ARP_STATE_PENDING;
                memset((char *)arp_cache[i].mac, 0, ETH_ALEN);
                arp_cache[i].stamp = now;
                /* Backdate so the first request goes out immediately. */
                arp_cache[i].asked = now - ARP_RETRY_MS;
            }
        }

        if(i >= 0 && (now - arp_cache[i].asked) >= ARP_RETRY_MS)
        {
            arp_cache[i].asked = now;
            ask = 1;
        }
        else if(i < 0)
        {
            /* Cache full of fresh entries: ask anyway, we just cannot
            *  remember that we did. */
            ask = 1;
        }
    }

    irq_restore(flags);

    if(ask)
        arp_request(ip);

    return result;
}

int arp_cache_entries(void)
{
    uint32_t flags;
    int i;
    int n;

    n = 0;
    flags = irq_save();
    arp_expire(net_now());
    for(i = 0; i < ARP_CACHE_SIZE; i++)
        if(arp_cache[i].state == ARP_STATE_VALID)
            n++;
    irq_restore(flags);
    return n;
}

/* The index-th resolved entry, counted the same way arp_cache_entries()
*  counts. Returns 0 on success, negative when there is no such entry. */
int arp_cache_get(int index, uint32_t *ip, uint8_t mac[ETH_ALEN])
{
    uint32_t flags;
    int i;
    int n;
    int rc;

    if(index < 0)
        return -1;

    rc = -1;
    n = 0;
    flags = irq_save();
    arp_expire(net_now());
    for(i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if(arp_cache[i].state != ARP_STATE_VALID)
            continue;
        if(n == index)
        {
            if(ip != 0)
                *ip = arp_cache[i].ip;
            if(mac != 0)
                memcpy((void *)mac, (const void *)arp_cache[i].mac, ETH_ALEN);
            rc = 0;
            break;
        }
        n++;
    }
    irq_restore(flags);
    return rc;
}

/* Answers a request aimed at us. Without this nothing on the segment can
*  start a conversation with this machine: it would ask who has our address,
*  hear nothing, and never send the packet it wanted to.
*
*  This is a send from the drain, so it is a send from TASK context now,
*  where it used to be one from the card's interrupt. It reaches the wire
*  through net_send() rather than ip_send(), and net_send() builds its frame
*  with interrupts off -- which on a uniprocessor also means the scheduler
*  cannot take the CPU away in the middle of it. The shared build buffer is
*  therefore as safe against the timer preempting this task as it was
*  against the card preempting a task, and for the same reason. */
static void arp_reply(const arp_packet *req, uint32_t sender_ip)
{
    arp_packet pkt;

    pkt.hw_type    = htons(ARP_HW_ETHERNET);
    pkt.proto_type = htons(ETH_TYPE_IP);
    pkt.hw_len     = ETH_ALEN;
    pkt.proto_len  = 4;
    pkt.opcode     = htons(ARP_OP_REPLY);
    memcpy((void *)pkt.sender_mac, (const void *)net_hwaddr, ETH_ALEN);
    pkt.sender_ip  = htonl(net_addr);
    memcpy((void *)pkt.target_mac, (const void *)req->sender_mac, ETH_ALEN);
    pkt.target_ip  = htonl(sender_ip);

    /* Unicast back to the asker rather than to the broadcast address: the
    *  answer only concerns the one machine that asked. */
    net_send(req->sender_mac, ETH_TYPE_ARP, (const void *)&pkt, ARP_PKT_LEN);
}

/* Everything that arrives with ethertype 0x0806. Task context, out of the
*  drain -- the ARP cache it touches is shared with arp_lookup(), which is
*  why every access to it goes through irq_save(). */
static void arp_receive(const uint8_t *data, uint32_t len)
{
    const arp_packet *pkt;
    uint32_t sender_ip;
    uint32_t target_ip;
    uint16_t opcode;
    int for_us;

    if(len < ARP_PKT_LEN)
    {
        stat_drop_tsk++;
        return;
    }

    pkt = (const arp_packet *)data;

    /* Only ethernet/IPv4 ARP, and only with the lengths that implies -- a
    *  packet that says something else is not one we can read the addresses
    *  out of. */
    if(ntohs(pkt->hw_type) != ARP_HW_ETHERNET ||
       ntohs(pkt->proto_type) != ETH_TYPE_IP ||
       pkt->hw_len != ETH_ALEN ||
       pkt->proto_len != 4)
    {
        stat_drop_tsk++;
        return;
    }

    opcode = ntohs(pkt->opcode);
    sender_ip = ntohl(pkt->sender_ip);
    target_ip = ntohl(pkt->target_ip);
    for_us = (net_addr != 0 && target_ip == net_addr);

    /* Learn from requests and replies alike, whoever they were aimed at. */
    arp_learn(sender_ip, pkt->sender_mac, for_us, net_now());

    if(opcode == ARP_OP_REQUEST && for_us && net_is_up)
        arp_reply(pkt, sender_ip);

    /* A reply needs nothing further -- arp_learn() above is the whole of it,
    *  and an unsolicited one is a gratuitous ARP, which is legitimate and
    *  already handled by the same call. */
}

/* --- Receiving: the producer ---------------------------------------------
*
*  The driver calls this from its interrupt handler, once per frame, and it
*  does two things and no more: it decides whether the frame is worth
*  keeping, and it copies it into the queue.
*
*  Everything it decides is a fixed number of comparisons over the fourteen
*  bytes of the ethernet header, and the copy is one or two memcpys. There
*  is no cache walk, no reply built, no checksum and nothing that follows a
*  pointer out of the frame -- all of that moved to net_queue_drain(). What
*  was a bug in interrupt context, where a spin hangs the machine, is now a
*  bug in a task, where it hangs a task.
*
*  Why the ethertype is filtered HERE rather than in the drain, when the
*  addressing is the only check net.h promises: a segment carrying IPv6 or
*  VLAN tagged traffic would otherwise fill the queue with frames whose only
*  destiny is to be counted and thrown away, and pushing out frames that
*  were for us to make room for frames that were not is the wrong trade for
*  the sake of a tidier split. It costs one comparison against a value that
*  has already been loaded.
*
*  The frame belongs to the driver's receive ring and is reused the moment
*  this returns, which is the whole reason it is copied rather than
*  remembered.
*/
void net_receive(const uint8_t *frame, uint32_t len)
{
    const eth_header *eth;
    uint8_t  hdr[NET_RXQ_HDR];
    uint32_t head;
    uint32_t used;
    uint32_t need;
    uint16_t type;

    if(frame == 0 || !net_is_up || rxq_buf == 0)
    {
        stat_drop_irq++;
        return;
    }

    /* Long enough to contain a header before anything reads one. */
    if(len < ETH_HDR_LEN || len > ETH_FRAME_MAX)
    {
        stat_drop_irq++;
        return;
    }

    eth = (const eth_header *)frame;

    /* Addressed to us, or broadcast. The card can be told to hand up
    *  everything it hears, so this check is what keeps the rest of the
    *  stack from answering another machine's traffic. */
    if(!mac_equal(eth->dst, net_hwaddr) && !mac_equal(eth->dst, mac_broadcast))
    {
        stat_drop_irq++;
        return;
    }

    /* A frame that claims to come FROM us is either our own transmission
    *  echoed back or somebody spoofing; either way there is nothing to do
    *  with it. */
    if(mac_equal(eth->src, net_hwaddr))
    {
        stat_drop_irq++;
        return;
    }

    stat_rx++;

    /* A machine that only listens still needs its drain. Nothing here is
    *  reached before the scheduler is running -- net_drain_start() checks
    *  that itself -- and all it does then is set a state byte. */
    net_drain_start();

    type = ntohs(eth->type);
    if(type != ETH_TYPE_ARP && type != ETH_TYPE_IP)
    {
        /* IPv6, VLAN tags, whatever else the segment carries. Counted, not
        *  printed -- this is the common case on a real network and a printf
        *  here would bury the machine in interrupt context. */
        stat_drop_irq++;
        return;
    }

    /* Enqueue. rxq_head is ours; rxq_tail is read exactly once, into a local
    *  through the difference below, and a value the task has just moved on
    *  only makes us believe the queue is fuller than it is. */
    need = NET_RXQ_HDR + len;
    head = rxq_head;
    used = head - rxq_tail;

    if(NET_RXQ_SIZE - used < need)
    {
        /* No room: the newest frame is the one that goes. See the overflow
        *  note above -- this is not net_rx_dropped(), it is the machine
        *  failing to keep up with the wire. */
        stat_overrun++;
        return;
    }

    hdr[0] = (uint8_t)(len & 0xFFUL);
    hdr[1] = (uint8_t)((len >> 8) & 0xFFUL);

    rxq_put(head, hdr, NET_RXQ_HDR);
    rxq_put(head + NET_RXQ_HDR, frame, len);

    /* Length and bytes first, the index that publishes them last. Until this
    *  store the consumer does not look at any of it. */
    net_barrier();
    rxq_head = head + need;
    rxq_in = rxq_in + 1;

    if(used + need > rxq_peak)
        rxq_peak = used + need;
}

/* --- Receiving: the consumer ---------------------------------------------
*
*  Takes the oldest frame out of the queue into the staging buffer and
*  advances rxq_tail past it. Returns its length, or 0 when the queue is
*  empty -- a stored frame is never shorter than ETH_HDR_LEN, so zero is
*  free to mean "nothing".
*
*  The frame is copied out BEFORE rxq_tail moves, and that order is not
*  cosmetic: the moment the tail passes a record, the interrupt is entitled
*  to write a new frame over it. Dispatching straight out of the ring would
*  also mean handing ip.c a pointer to something that may be split across
*  the wrap.
*/
static uint32_t rxq_take(void)
{
    uint8_t  hdr[NET_RXQ_HDR];
    uint32_t tail;
    uint32_t head;
    uint32_t in;
    uint32_t avail;
    uint32_t len;

    tail = rxq_tail;

    /* One read of each of the producer's indices, in this order. Frames it
    *  publishes after these reads are simply seen by the next call -- and
    *  the order matters for the recovery below, where in must not be newer
    *  than head. */
    in = rxq_in;
    head = rxq_head;

    avail = head - tail;
    if(avail < NET_RXQ_HDR)
        return 0;

    rxq_get(tail, hdr, NET_RXQ_HDR);
    len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8);

    /* Cannot happen: the producer publishes a record only once all of it is
    *  written, and it never writes a length it did not check. If it does
    *  happen the ring is no longer describable and the honest answer is to
    *  drop what is in it rather than to walk it further -- the same answer
    *  the driver gives a corrupt receive header.
    *
    *  Both of the consumer's variables are put back on a basis that cannot
    *  be ahead of the producer: in was read before head, so it counts no
    *  more frames than head's bytes contain, and net_queue_frames() can
    *  therefore be left reporting one frame too many but never a negative
    *  number. Nothing else here is allowed to leave the queue in a state
    *  the display code has to defend itself against. */
    if(len < ETH_HDR_LEN || len > (uint32_t)ETH_FRAME_MAX ||
       avail < NET_RXQ_HDR + len)
    {
        stat_drop_tsk++;
        rxq_out = in;
        rxq_tail = head;
        return 0;
    }

    rxq_get(tail + NET_RXQ_HDR, rxq_frame, len);

    /* The bytes are out of the ring before the space is given back. */
    net_barrier();
    rxq_tail = tail + NET_RXQ_HDR + len;
    rxq_out = rxq_out + 1;

    return len;
}

/* Runs the protocol work for the frames the queue holds, in task context,
*  with interrupts on. Returns how many frames it handled.
*
*  One caller at a time. rxq_tail is advanced with a read, a change and a
*  store, which is safe against the interrupt -- the interrupt never writes
*  it -- but not against a second TASK doing the same thing, and net.h makes
*  this function public. A second caller is therefore turned away rather
*  than allowed to lose an update; taking the flag is the one place in the
*  receive path that masks interrupts, once per call rather than once per
*  frame, and it is there to exclude a task rather than the card.
*
*  Nothing here prints. It could now -- that is half the point of being in a
*  task -- but a busy segment would bury the console, so the reporting is
*  left to the counters.
*
*  One thing this changed for the layer above, and it is worth being exact
*  about because ip.c cannot see it from where it stands. An echo reply and
*  an ARP reply are now sent from a TASK, where they used to be sent from
*  the card's interrupt. ip.c's ip_tx_busy guard is still needed and still
*  does its job -- a second writer of the shared packet buffer is refused
*  rather than allowed to corrupt the first -- but the kind of preemption it
*  faces has changed: an interrupt ran to completion between two of a task's
*  instructions, whereas the timer can now stop this task anywhere. That is
*  fine for the buffer, which is only written after the flag is taken, and
*  it leaves ONE window that did not exist before: two tasks can both pass
*  the test before either does the store, because the test and the store are
*  not one instruction. The fix is a cli around those two lines in ip.c,
*  which is not this file's to make; it is written down here so that the
*  next person to open ip.c knows it is now reachable. Everything that made
*  a refused send harmless still holds -- data is covered by the
*  retransmission timer and a pure ACK is re-sent from tcp_poll(). */
int net_queue_drain(void)
{
    const eth_header *eth;
    uint32_t flags;
    uint32_t len;
    uint16_t type;
    int handled;

    if(rxq_buf == 0)
        return 0;

    flags = irq_save();
    if(rxq_draining)
    {
        irq_restore(flags);
        return 0;
    }
    rxq_draining = 1;
    irq_restore(flags);

    handled = 0;
    while(handled < NET_DRAIN_BURST)
    {
        len = rxq_take();
        if(len == 0)
            break;

        handled++;

        eth = (const eth_header *)rxq_frame;
        type = ntohs(eth->type);

        /* The producer let nothing else through, so the default is a frame
        *  that cannot be here at all. */
        if(type == ETH_TYPE_ARP)
            arp_receive(rxq_frame + ETH_HDR_LEN, len - ETH_HDR_LEN);
        else if(type == ETH_TYPE_IP)
            ip_receive(rxq_frame + ETH_HDR_LEN, len - ETH_HDR_LEN);
        else
            stat_drop_tsk++;
    }

    rxq_draining = 0;
    return handled;
}
