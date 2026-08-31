/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: DHCP client -- obtains an address instead of being told one.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*
*  The exchange itself is described in dhcp.h. What that description leaves
*  out are the three properties of this file that are easy to get wrong, and
*  every one of them follows from the same fact: the conversation happens
*  before the machine has an address, over a protocol that loses packets, on
*  a wire where every answer is a broadcast that everybody hears.
*
*  1. Nothing identifies a reply as ours except the transaction id.
*
*  The server answers by broadcast, to a MAC and an address that are not yet
*  configured, and every other client on the segment sees the same frame. So
*  a reply is ours if and only if its xid is the one we sent, its chaddr is
*  our MAC and it is a BOOTREPLY. Anything else is somebody else's exchange
*  and is dropped without a trace -- acting on it would mean configuring this
*  machine with an address a server handed to another one.
*
*  2. The options are the message; the fixed header barely matters.
*
*  Everything worth having -- the netmask, the router, the DNS server, the
*  lease, and even the answer to "what kind of message is this" -- lives in a
*  walk of type/length/value triples appended after a magic cookie. The walk
*  is the only part of this file that reads attacker-controlled data, since
*  the packet comes off the wire and nothing before us has vouched for a byte
*  of it. dhcp_parse_options() is written to be boring about that: it never
*  trusts a length byte, it never reads a length byte it has not proved is
*  there, and it treats every option it does not know as a number of bytes to
*  skip rather than as anything to interpret.
*
*  3. Only retransmission makes the client work at all.
*
*  UDP has no acknowledgement and DISCOVER is a broadcast that any switch may
*  drop. When no answer comes, nothing in the receive path fires and nothing
*  moves -- which is exactly why dhcp_poll() exists and why the header says
*  nothing else advances the state machine.
*
*  A note on where the work happens. dhcp_receive() runs in interrupt context,
*  called from the card's IRQ, so it does the minimum that cannot be deferred:
*  it decides whether the datagram is ours and, if it is, copies the handful of
*  numbers it carries into a one-slot mailbox. It sends nothing. Everything
*  that follows -- building the next message, calling net_configure(), changing
*  state -- happens in dhcp_poll(), in task context. That split buys two things
*  for free: the transmit buffer below has exactly one writer and needs no
*  re-entrancy guard, and the driver is never asked to transmit from inside its
*  own interrupt handler.
*/

#include <system.h>
#include <net.h>
#include <dhcp.h>

/* ------------------------------------------------------------------ */
/* Protocol constants                                                  */
/* ------------------------------------------------------------------ */

/* op: a client always sends BOOTREQUEST, a server always BOOTREPLY. The
*  field survives from BOOTP, which DHCP is an extension of. */
#define BOOTREQUEST        1
#define BOOTREPLY          2

/* htype/hlen describe the hardware address in chaddr. 1 is "10 Mb ethernet"
*  in the ARP hardware type registry, which is what every ethernet card
*  reports regardless of its actual speed. */
#define DHCP_HTYPE_ETHER   1
#define DHCP_HLEN_ETHER    6

/* The BROADCAST flag, bit 15 of the flags field.
*
*  Without it a server is entitled to unicast its reply straight to the
*  address it is about to hand out -- which means an ethernet frame addressed
*  to our MAC but an IP packet addressed to something we have not configured
*  yet. Our own receive path would drop that as "not for us", and the client
*  would sit waiting for an answer that already arrived. Setting the flag
*  tells the server to broadcast instead, and a broadcast we accept. */
#define DHCP_FLAG_BROADCAST 0x8000

/* The four bytes that begin the options area, 99.130.83.99 in the original
*  notation. A BOOTP reply has no options at all and a BOOTP server that is
*  answering our broadcast will not have written this; without the check its
*  vendor area would be walked as if it were DHCP options and produce
*  whatever the padding happens to look like. */
#define DHCP_MAGIC_COOKIE  0x63825363UL

/* Option codes. Only the ones this client sends or reads. */
#define DHCP_OPT_PAD           0
#define DHCP_OPT_SUBNET_MASK   1
#define DHCP_OPT_ROUTER        3
#define DHCP_OPT_DNS_SERVER    6
#define DHCP_OPT_REQUESTED_IP  50
#define DHCP_OPT_LEASE_TIME    51
#define DHCP_OPT_MSG_TYPE      53
#define DHCP_OPT_SERVER_ID     54
#define DHCP_OPT_PARAM_LIST    55
#define DHCP_OPT_MAX_MSG_SIZE  57
#define DHCP_OPT_CLIENT_ID     61
#define DHCP_OPT_END           255

/* Values of option 53. */
#define DHCP_MSG_DISCOVER  1
#define DHCP_MSG_OFFER     2
#define DHCP_MSG_REQUEST   3
#define DHCP_MSG_DECLINE   4
#define DHCP_MSG_ACK       5
#define DHCP_MSG_NAK       6

/* ------------------------------------------------------------------ */
/* Message layout and sizes                                            */
/* ------------------------------------------------------------------ */

/* The fixed part of a DHCP message: 236 bytes inherited wholesale from
*  BOOTP, most of which this client never fills in. sname and file are the
*  bulk of it and exist to name a boot image on a TFTP server. */
typedef struct
{
    uint8_t  op;              /* BOOTREQUEST / BOOTREPLY                  */
    uint8_t  htype;           /* hardware type, 1 for ethernet            */
    uint8_t  hlen;            /* hardware address length, 6               */
    uint8_t  hops;            /* incremented by relay agents, 0 from us   */
    uint32_t xid;             /* transaction id -- see dhcp_make_xid()    */
    uint16_t secs;            /* seconds since the client started trying  */
    uint16_t flags;           /* only DHCP_FLAG_BROADCAST is defined      */
    uint32_t ciaddr;          /* our address, only when already bound     */
    uint32_t yiaddr;          /* "your" address: what the server offers   */
    uint32_t siaddr;          /* next server for the boot file, not us    */
    uint32_t giaddr;          /* relay agent that forwarded this          */
    uint8_t  chaddr[16];      /* client hardware address, MAC then zeros  */
    uint8_t  sname[64];       /* server host name, unused                 */
    uint8_t  file[128];       /* boot file name, unused                   */
} __attribute__((packed)) dhcp_header;

/* 236 is not a number to get wrong: every offset in the options area is
*  measured from it, and a padding byte slipped in by the compiler would
*  shift the magic cookie and make every reply look like BOOTP. The array
*  below has a negative size -- and so fails to compile -- if that happens. */
typedef char dhcp_header_size_is_236[(sizeof(dhcp_header) == 236) ? 1 : -1];

#define DHCP_FIXED_LEN   236

/* 548 is the largest DHCP message that fits the 576 byte IP datagram every
*  host is required to accept without fragmenting: 576 - 20 (IP) - 8 (UDP).
*  Staying inside it means no reply we provoke can arrive in fragments, and
*  this stack does not reassemble fragments. */
#define DHCP_MSG_MAX     548
#define DHCP_OPTS_MAX    (DHCP_MSG_MAX - DHCP_FIXED_LEN)

/* Shortest message we send. BOOTP relays and some servers still expect the
*  300 byte BOOTP message and drop anything shorter, so the options area is
*  zero padded out to that after the END option. Padding after END is
*  explicitly allowed and is ignored by anything that parses correctly. */
#define DHCP_MSG_MIN     300

/* The smallest thing that can be a DHCP reply at all: the fixed header plus
*  the cookie. Anything shorter cannot even be asked what kind of message it
*  is, so there is nothing to do with it but drop it. */
#define DHCP_MSG_MIN_RX  (DHCP_FIXED_LEN + 4)

/* ------------------------------------------------------------------ */
/* Retransmission schedule                                             */
/* ------------------------------------------------------------------ */

/* Four attempts, doubling: the first resend one second after the message,
*  then two, then four, then eight -- fifteen seconds of patience in total
*  before the client gives up.
*
*  Both ends of that were picked against a failure mode. A client that gives
*  up in a few hundred milliseconds is useless on anything but a loopback:
*  a server on a busy segment, a relay agent that has to forward the
*  broadcast to another subnet, or a switch running spanning tree on a port
*  that just came up will all take longer than that to produce an answer,
*  and the client would report "no server" on a network that has one. At the
*  other end, a client that waits a minute makes the shell look hung, with
*  no output and no way to tell a slow server from a broken one.
*
*  The doubling is what makes both affordable. A fixed one second interval
*  for fifteen seconds would put fifteen broadcasts on a segment that has
*  already demonstrated that nobody is listening; backing off halves the
*  rate each time, so the total wait grows while the traffic does not.
*
*  RFC 2131 asks for a random jitter of about a second on each interval so
*  that a room full of machines powering up together does not synchronise
*  into a burst. There is no random source in this kernel, and one machine
*  cannot collide with itself, so the jitter is left out knowingly rather
*  than forgotten. */
#define DHCP_MAX_ATTEMPTS   4
#define DHCP_FIRST_TIMEOUT  1000UL   /* milliseconds; doubles per attempt */

/* The furthest ahead a lease expiry can be expressed -- see the clamp in
*  dhcp_handle_ack(). Half the range of the millisecond clock, which is as
*  far ahead as an unsigned difference against it can still be read as
*  "later" rather than "earlier". */
#define DHCP_MAX_LEASE_MS   0x7FFFFFFFUL

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* The message under construction.
*
*  Static, and deliberately not on the stack. 548 bytes is a seventh of the
*  4 KiB kernel stack, and dhcp_poll() is called from the shell, which is
*  already several frames deep by the time it gets here and may take an
*  interrupt on top of that. Putting the buffer in .bss costs half a kilobyte
*  of image that is never in doubt, instead of a stack overflow that shows up
*  as memory corruption somewhere else entirely.
*
*  It needs no busy flag, unlike the shared buffer in ip.c. Only dhcp_start()
*  and dhcp_poll() ever write it, both run in task context, and the receive
*  handler -- the one caller that could preempt them -- never builds a
*  message. That is the point of the mailbox below. */
static uint8_t  dhcp_tx[DHCP_MSG_MAX];

static int      dhcp_current_state = DHCP_STATE_IDLE;
static const char *dhcp_error = "";

/* Identity of the exchange in progress. */
static uint32_t dhcp_xid = 0;
static uint32_t dhcp_start_ms = 0;      /* for the secs field             */

/* Retransmission bookkeeping: when the last message went out, how long we
*  are willing to wait for this attempt, and how many attempts are used. */
static uint32_t dhcp_sent_ms = 0;
static uint32_t dhcp_timeout_ms = 0;
static int      dhcp_attempts = 0;

/* What the accepted OFFER proposed, and what the ACK confirmed. */
static uint32_t dhcp_offered_ip = 0;
static uint32_t dhcp_offered_mask = 0;
static uint32_t dhcp_offered_router = 0;
static uint32_t dhcp_offered_dns = 0;
static uint32_t dhcp_offered_server = 0;
static uint32_t dhcp_offered_lease = 0;

/* Results, as dhcp.h promises them. */
static uint32_t dhcp_server_id = 0;
static uint32_t dhcp_dns_addr = 0;
static uint32_t dhcp_lease_secs = 0;
static uint32_t dhcp_lease_end = 0;

/* Everything a reply is worth keeping, and whether it was there at all. A
*  server is not obliged to send every option, and "absent" has to be
*  distinguishable from "zero": 0.0.0.0 is a legal value for a router option
*  and means something quite different from the option being missing. */
typedef struct
{
    uint8_t  msg_type;        /* option 53; 0 when the option was absent  */
    uint8_t  have_mask;
    uint8_t  have_router;
    uint8_t  have_dns;
    uint8_t  have_lease;
    uint8_t  have_server;
    uint32_t mask;
    uint32_t router;
    uint32_t dns;
    uint32_t lease;
    uint32_t server;
    uint32_t yiaddr;
} dhcp_reply;

/* The mailbox between the interrupt handler and dhcp_poll(). One slot: a
*  second reply arriving before the first has been acted on is dropped, and
*  the retransmit timer covers the loss. Queueing them would mean deciding
*  what to do with two offers we cannot both accept anyway.
*
*  The handshake is what makes this safe without disabling interrupts. The
*  handler writes dhcp_inbox only while dhcp_inbox_full is 0 and sets the
*  flag last; dhcp_poll() copies the struct out while the flag is still 1 --
*  so the handler cannot be writing -- and clears it afterwards. Only the
*  flag needs to be volatile; the struct is never read and written at the
*  same time. */
static dhcp_reply     dhcp_inbox;
static volatile int   dhcp_inbox_full = 0;

/* Whether udp_bind() currently holds port 68 for us, so that dhcp_stop()
*  and a second dhcp_start() cannot unbind or double-bind by accident. */
static int      dhcp_bound_port = 0;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Milliseconds elapsed since a snapshot of timer_get_ticks().
*
*  timer_get_ticks() returns int and wraps after roughly 25 days, and around
*  that point a plain "now - then" on signed values is nonsense: the result
*  is negative, or the comparison against a timeout flips, and the client
*  either fires every retransmit at once or never fires one again. Doing the
*  subtraction on the unsigned bit patterns gives the correct elapsed time
*  straight through the wrap, as long as less than 2^32 ms has actually
*  passed -- which is what every caller here means. */
static uint32_t dhcp_ms_since(uint32_t then)
{
    return (uint32_t)timer_get_ticks() - then;
}

/* Reads a 32 bit big endian value out of an unaligned byte run. The options
*  area is a byte stream: an option body can start on any offset, so casting
*  a pointer to uint32_t* would be an unaligned access as well as an endian
*  bug. */
static uint32_t dhcp_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* ------------------------------------------------------------------ */
/* The transaction id                                                  */
/* ------------------------------------------------------------------ */

/* Where a random number comes from in a kernel that has no random numbers.
*
*  The requirement is narrow and worth stating exactly, because it is much
*  weaker than randomness: the xid has to differ from the xid of every other
*  exchange that could be live on the same wire at the same time. It is a
*  label for matching replies, not a secret -- an attacker who can see our
*  DISCOVER can read the xid off the wire no matter how it was chosen, so
*  unpredictability would buy nothing here.
*
*  Three sources are mixed, each covering a way two exchanges could collide:
*
*  - The low four bytes of the MAC address. The card's address is unique by
*    construction -- the upper three bytes are the vendor, the lower three
*    are assigned by that vendor -- so this alone separates our exchange from
*    every other machine's on the segment, which is the collision that
*    actually matters.
*  - The wall clock from the CMOS. This is what separates two boots of the
*    same machine. The tick counter alone would not: dhcp_start() tends to
*    run at the same point in every boot, so the milliseconds since power on
*    are nearly the same every time, and two boots in a row could easily
*    produce the same value. The date and time cannot.
*  - The tick counter and an attempt counter, which separate repeated tries
*    within one boot, where the wall clock may not have moved a second.
*
*  They are combined by multiplying each into a different part of the word
*  before xoring, rather than by xoring them raw: the MAC and a small clock
*  reading both have most of their entropy in the low bits, and xoring them
*  as they are would pile it all into the same place and cancel. The constant
*  is Knuth's odd multiplier for 32 bit hashing, which spreads a change in
*  any bit across the whole word. */
static uint32_t dhcp_make_xid(void)
{
    static uint32_t attempt = 0;
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
    x ^= (++attempt) * 2654435761UL;

    /* Zero is a legal xid but a poor one: it is also what an uninitialised
    *  field looks like, so a reply carrying a zero xid is as likely to be a
    *  bug somewhere as an answer to us. Refusing to use it costs nothing. */
    if(x == 0)
        x = 1;

    return x;
}

/* ------------------------------------------------------------------ */
/* Building a message                                                  */
/* ------------------------------------------------------------------ */

/* Appends one option and returns the new cursor. Returns the cursor
*  unchanged when the option would not fit -- the buffer is sized for far
*  more than this client ever sends, so that is a guard against a later edit
*  rather than a condition that occurs, but silently running off the end of
*  the buffer is not an acceptable way to find out about such an edit. */
static uint32_t dhcp_put_option(uint8_t *opts, uint32_t at, uint8_t code,
                                const uint8_t *value, uint32_t len)
{
    uint32_t i;

    if(at + 2 + len > DHCP_OPTS_MAX)
        return at;

    opts[at++] = code;
    opts[at++] = (uint8_t)len;
    for(i = 0; i < len; i++)
        opts[at++] = value[i];

    return at;
}

/* The same for a single byte value, which most of them are. */
static uint32_t dhcp_put_option_u8(uint8_t *opts, uint32_t at, uint8_t code,
                                   uint8_t value)
{
    return dhcp_put_option(opts, at, code, &value, 1);
}

/* And for an IP address, which goes on the wire in network order. */
static uint32_t dhcp_put_option_ip(uint8_t *opts, uint32_t at, uint8_t code,
                                   uint32_t ip)
{
    uint8_t v[4];

    v[0] = (uint8_t)(ip >> 24);
    v[1] = (uint8_t)(ip >> 16);
    v[2] = (uint8_t)(ip >>  8);
    v[3] = (uint8_t)(ip);

    return dhcp_put_option(opts, at, code, v, 4);
}

/* Fills in the fixed header and the magic cookie, and returns the offset of
*  the first free byte in the options area. Everything both messages have in
*  common lives here; only the options differ between DISCOVER and REQUEST.
*
*  ciaddr stays zero on purpose in both. It is the client's *current*
*  address, and it is only filled in when renewing a lease already held --
*  which this client does not do. A REQUEST during the initial exchange that
*  puts the offered address in ciaddr instead of in option 50 is a different
*  message to a server and gets refused. */
static uint32_t dhcp_build_common(void)
{
    dhcp_header *h;
    const uint8_t *mac;
    uint32_t secs;
    uint32_t at;

    memset((char *)dhcp_tx, 0, DHCP_MSG_MAX);

    h = (dhcp_header *)dhcp_tx;
    mac = net_mac();

    h->op    = BOOTREQUEST;
    h->htype = DHCP_HTYPE_ETHER;
    h->hlen  = DHCP_HLEN_ETHER;
    h->hops  = 0;
    h->xid   = htonl(dhcp_xid);

    /* secs counts from the first attempt, not from this message, and relay
    *  agents and backup servers use it to decide whether a client has been
    *  unanswered long enough for them to step in. The field is 16 bit, so
    *  it saturates rather than wrapping back to "just started". */
    secs = dhcp_ms_since(dhcp_start_ms) / 1000UL;
    if(secs > 0xFFFFUL)
        secs = 0xFFFFUL;
    h->secs  = htons((uint16_t)secs);

    h->flags = htons(DHCP_FLAG_BROADCAST);

    memcpy(h->chaddr, mac, ETH_ALEN);

    at = DHCP_FIXED_LEN;
    dhcp_tx[at++] = (uint8_t)(DHCP_MAGIC_COOKIE >> 24);
    dhcp_tx[at++] = (uint8_t)(DHCP_MAGIC_COOKIE >> 16);
    dhcp_tx[at++] = (uint8_t)(DHCP_MAGIC_COOKIE >>  8);
    dhcp_tx[at++] = (uint8_t)(DHCP_MAGIC_COOKIE);

    return at - DHCP_FIXED_LEN;   /* cursor within the options area */
}

/* The parameter request list: what we want the server to tell us. A server
*  is free to send more and free to send less, but it will generally not
*  volunteer an option nobody asked for, so leaving this out is a good way to
*  get an address with no netmask. */
static const uint8_t dhcp_param_list[] =
{
    DHCP_OPT_SUBNET_MASK,
    DHCP_OPT_ROUTER,
    DHCP_OPT_DNS_SERVER,
    DHCP_OPT_LEASE_TIME,
    DHCP_OPT_SERVER_ID
};

/* The client identifier: hardware type 1 followed by the MAC. Servers key
*  their lease database on this when it is present, so sending the same one
*  every time is what makes a machine get the same address back across
*  reboots instead of consuming a fresh lease each run. */
static uint32_t dhcp_put_client_id(uint8_t *opts, uint32_t at)
{
    uint8_t id[1 + ETH_ALEN];

    id[0] = DHCP_HTYPE_ETHER;
    memcpy(&id[1], net_mac(), ETH_ALEN);

    return dhcp_put_option(opts, at, DHCP_OPT_CLIENT_ID, id, sizeof(id));
}

/* Finishes the options with END, pads to the BOOTP minimum and hands the
*  message to UDP.
*
*  Source IP_ADDR_ANY, destination IP_ADDR_BROADCAST: we have no address to
*  send from and no idea where the server is, and both facts are permanent
*  until the ACK arrives. udp_send_from() exists for precisely this. */
static int dhcp_transmit(uint32_t at)
{
    uint32_t len;

    if(at < DHCP_OPTS_MAX)
        dhcp_tx[DHCP_FIXED_LEN + at++] = DHCP_OPT_END;

    len = DHCP_FIXED_LEN + at;
    if(len < DHCP_MSG_MIN)
        len = DHCP_MSG_MIN;      /* the buffer is already zeroed */

    return udp_send_from(IP_ADDR_ANY, IP_ADDR_BROADCAST,
                         DHCP_PORT_CLIENT, DHCP_PORT_SERVER,
                         dhcp_tx, len);
}

static int dhcp_send_discover(void)
{
    uint8_t *opts;
    uint32_t at;

    at = dhcp_build_common();
    opts = dhcp_tx + DHCP_FIXED_LEN;

    at = dhcp_put_option_u8(opts, at, DHCP_OPT_MSG_TYPE, DHCP_MSG_DISCOVER);
    at = dhcp_put_client_id(opts, at);

    /* Tell the server how big a reply we can take, so it truncates its
    *  option list rather than sending something we would have to reassemble
    *  out of fragments. */
    {
        uint8_t sz[2];
        sz[0] = (uint8_t)(DHCP_MSG_MAX >> 8);
        sz[1] = (uint8_t)(DHCP_MSG_MAX & 0xFF);
        at = dhcp_put_option(opts, at, DHCP_OPT_MAX_MSG_SIZE, sz, 2);
    }

    at = dhcp_put_option(opts, at, DHCP_OPT_PARAM_LIST,
                         dhcp_param_list, sizeof(dhcp_param_list));

    return dhcp_transmit(at);
}

/* REQUEST goes out as a broadcast even though dhcp_offered_server names the
*  server we are answering, and dhcp.h explains why: the servers whose offers
*  we did not take only learn to release the addresses they reserved by
*  seeing this message name somebody else. */
static int dhcp_send_request(void)
{
    uint8_t *opts;
    uint32_t at;

    at = dhcp_build_common();
    opts = dhcp_tx + DHCP_FIXED_LEN;

    at = dhcp_put_option_u8(opts, at, DHCP_OPT_MSG_TYPE, DHCP_MSG_REQUEST);
    at = dhcp_put_client_id(opts, at);
    at = dhcp_put_option_ip(opts, at, DHCP_OPT_REQUESTED_IP, dhcp_offered_ip);

    /* Option 54 is what makes this a reply to one particular offer. Without
    *  it every server that offered would think it had been chosen. */
    if(dhcp_offered_server != 0)
        at = dhcp_put_option_ip(opts, at, DHCP_OPT_SERVER_ID,
                                dhcp_offered_server);

    at = dhcp_put_option(opts, at, DHCP_OPT_PARAM_LIST,
                         dhcp_param_list, sizeof(dhcp_param_list));

    return dhcp_transmit(at);
}

/* ------------------------------------------------------------------ */
/* The option walk                                                     */
/* ------------------------------------------------------------------ */

/* Walks the options area and fills in what it recognises.
*
*  This is the one function here that reads bytes an attacker chose, so it
*  is worth spelling out exactly what bounds it. The area is [opts, opts+len)
*  and the cursor never leaves it:
*
*  - The loop condition proves opts[i] is inside the buffer before the code
*    byte is read. Nothing is read before that test.
*  - PAD (0) is a bare byte with no length and no body -- the one option that
*    is not a triple. It advances the cursor by one and nothing else. A run
*    of a hundred PADs is a hundred iterations and no reads past the end.
*  - END (255) stops the walk. Whatever follows is padding by definition and
*    is not examined, which is what lets dhcp_transmit() pad with zeros.
*  - Every other code needs a length byte at i+1, and that byte is only read
*    after proving i+1 is inside the buffer. A message whose last byte is an
*    option code with the length cut off stops here instead of reading the
*    byte after the buffer.
*  - The body is only read after proving that all of it fits: i+2+olen must
*    be within len. A length byte that claims 255 bytes at the end of a short
*    buffer -- the obvious way to try to walk this off the end of the receive
*    ring -- fails that test and stops the walk. None of those sums can
*    overflow: len is at most a frame's worth, i is below it, and olen is a
*    single byte.
*  - Unknown codes are skipped by their own length and never interpreted.
*
*  The bound tests stop the walk rather than merely skipping the bad option,
*  because a length that does not fit means the remaining bytes cannot be
*  parsed as options at all: there is no way to know where the next one would
*  start. Whatever was collected before the damage is kept, and the caller
*  decides whether that is enough -- a truncated reply with no message type
*  is refused by dhcp_receive() on those grounds.
*
*  Option 52 (option overload), which lets a server continue its options into
*  the unused sname and file fields, is not implemented. It only appears when
*  a server has more to say than fits, which is not a situation the five
*  options we ask for can create. */
static void dhcp_parse_options(const uint8_t *opts, uint32_t len,
                               dhcp_reply *out)
{
    uint32_t i;
    uint8_t  code;
    uint32_t olen;
    const uint8_t *body;

    i = 0;
    while(i < len)
    {
        code = opts[i];

        if(code == DHCP_OPT_PAD)
        {
            i++;
            continue;
        }

        if(code == DHCP_OPT_END)
            break;

        if(i + 1 >= len)
            break;                       /* code byte with no length byte */

        olen = opts[i + 1];

        if(i + 2 + olen > len)
            break;                       /* body runs past the end        */

        body = &opts[i + 2];

        switch(code)
        {
        case DHCP_OPT_MSG_TYPE:
            /* One byte. A server sending a longer one is not something to
            *  guess at, so the option is only taken at its stated size. */
            if(olen == 1)
                out->msg_type = body[0];
            break;

        case DHCP_OPT_SUBNET_MASK:
            if(olen >= 4)
            {
                out->mask = dhcp_get32(body);
                out->have_mask = 1;
            }
            break;

        case DHCP_OPT_ROUTER:
            /* A list of routers in order of preference, so the length is a
            *  multiple of four and may well be more than one. There is one
            *  gateway in this stack, so the first is the one we take. */
            if(olen >= 4)
            {
                out->router = dhcp_get32(body);
                out->have_router = 1;
            }
            break;

        case DHCP_OPT_DNS_SERVER:
            /* Also a list, same reasoning. */
            if(olen >= 4)
            {
                out->dns = dhcp_get32(body);
                out->have_dns = 1;
            }
            break;

        case DHCP_OPT_LEASE_TIME:
            if(olen >= 4)
            {
                out->lease = dhcp_get32(body);
                out->have_lease = 1;
            }
            break;

        case DHCP_OPT_SERVER_ID:
            if(olen >= 4)
            {
                out->server = dhcp_get32(body);
                out->have_server = 1;
            }
            break;

        default:
            break;
        }

        i += 2 + olen;
    }
}

/* ------------------------------------------------------------------ */
/* Receive path -- interrupt context                                   */
/* ------------------------------------------------------------------ */

/* Called by the UDP layer for every datagram that reaches port 68, from the
*  card's interrupt handler.
*
*  Short by construction: a handful of comparisons, one walk over at most a
*  frame's worth of options, and a copy of eleven words into the mailbox. It
*  prints nothing -- not even on a message it rejects, which would let any
*  station on the segment scroll the console by sending rubbish to port 68 --
*  allocates nothing and waits for nothing.
*
*  Nothing is kept that points into data: the walk reads the driver's receive
*  ring in place and copies out only the numbers it found, so there is
*  nothing left referring to that buffer when this returns and the driver
*  reuses it. */
static void dhcp_receive(uint32_t src_ip, uint16_t src_port,
                         const uint8_t *data, uint32_t len)
{
    const dhcp_header *h;
    const uint8_t *mac;
    dhcp_reply r;
    uint32_t cookie;
    int i;

    /* Not waiting for anything: bound already, or never started. */
    if(dhcp_current_state != DHCP_STATE_DISCOVER &&
       dhcp_current_state != DHCP_STATE_REQUEST)
        return;

    /* A server answers from port 67 and nothing else does. */
    if(src_port != DHCP_PORT_SERVER)
        return;

    /* Too short to be a DHCP message at all. Every field read below is
    *  inside this length. */
    if(len < DHCP_MSG_MIN_RX)
        return;

    h = (const dhcp_header *)data;

    if(h->op != BOOTREPLY)
        return;

    /* The transaction id is the whole of "is this reply mine". Every server
    *  broadcasts its answers, so a segment with two clients booting at once
    *  delivers each of them both conversations; without this test the first
    *  offer to arrive would be taken whoever it was meant for. */
    if(ntohl(h->xid) != dhcp_xid)
        return;

    /* And the hardware address, which is the same question asked a second
    *  way. It costs six comparisons and it catches a server that replies to
    *  a stale xid as well as a client that guessed ours. */
    if(h->htype != DHCP_HTYPE_ETHER || h->hlen != DHCP_HLEN_ETHER)
        return;

    mac = net_mac();
    for(i = 0; i < ETH_ALEN; i++)
        if(h->chaddr[i] != mac[i])
            return;

    /* The cookie separates DHCP from plain BOOTP. A BOOTP server answering
    *  our broadcast reaches this point with a vendor area that is not
    *  options, and walking it would produce whatever its padding looks
    *  like. */
    cookie = dhcp_get32(data + DHCP_FIXED_LEN);
    if(cookie != DHCP_MAGIC_COOKIE)
        return;

    /* The mailbox still holds a reply dhcp_poll() has not looked at. Drop
    *  this one rather than overwrite a record that is being read; the
    *  retransmit timer is what covers the loss. */
    if(dhcp_inbox_full)
        return;

    memset((char *)&r, 0, sizeof(r));
    r.yiaddr = ntohl(h->yiaddr);

    dhcp_parse_options(data + DHCP_FIXED_LEN + 4,
                       len - DHCP_FIXED_LEN - 4, &r);

    /* Without option 53 there is no way to tell an OFFER from an ACK, and
    *  guessing from the state we happen to be in is how a NAK gets treated
    *  as a confirmation. A DHCP message always carries it. */
    if(r.msg_type == 0)
        return;

    /* siaddr is not the server; option 54 is. But when a server omits the
    *  option, the address the packet came from is the best answer available
    *  and is better than addressing the REQUEST to 0.0.0.0. */
    if(!r.have_server && src_ip != IP_ADDR_ANY &&
       src_ip != IP_ADDR_BROADCAST)
    {
        r.server = src_ip;
        r.have_server = 1;
    }

    dhcp_inbox = r;
    dhcp_inbox_full = 1;      /* written last: see the comment on the flag */
}

/* ------------------------------------------------------------------ */
/* State machine -- task context                                       */
/* ------------------------------------------------------------------ */

/* Arms the retransmit timer for a freshly sent message. */
static void dhcp_arm_timer(int first)
{
    dhcp_sent_ms = (uint32_t)timer_get_ticks();

    if(first)
    {
        dhcp_attempts = 1;
        dhcp_timeout_ms = DHCP_FIRST_TIMEOUT;
    }
    else
    {
        dhcp_attempts++;
        dhcp_timeout_ms *= 2;
    }
}

static void dhcp_fail(const char *why)
{
    dhcp_current_state = DHCP_STATE_FAILED;
    dhcp_error = why;

    if(dhcp_bound_port)
    {
        udp_unbind(DHCP_PORT_CLIENT);
        dhcp_bound_port = 0;
    }
}

/* An OFFER arrived while we were waiting for one. The first acceptable offer
*  wins: several servers may answer, and with one address to configure there
*  is nothing useful to do with the second. */
static void dhcp_handle_offer(const dhcp_reply *r)
{
    if(dhcp_current_state != DHCP_STATE_DISCOVER)
        return;

    /* An offer of 0.0.0.0 is not an offer. Nothing forbids a server from
    *  sending it, and accepting it would put the machine on the wire with
    *  the address that means "I have none". */
    if(r->yiaddr == IP_ADDR_ANY || r->yiaddr == IP_ADDR_BROADCAST)
        return;

    dhcp_offered_ip     = r->yiaddr;
    dhcp_offered_mask   = r->have_mask   ? r->mask   : 0;
    dhcp_offered_router = r->have_router ? r->router : 0;
    dhcp_offered_dns    = r->have_dns    ? r->dns    : 0;
    dhcp_offered_server = r->have_server ? r->server : 0;
    dhcp_offered_lease  = r->have_lease  ? r->lease  : 0;

    dhcp_current_state = DHCP_STATE_REQUEST;

    if(dhcp_send_request() < 0)
    {
        /* The transmit failed -- most likely the card's queue is full or
        *  ip.c was already building a packet when we got here. The state has
        *  moved on regardless, and the timer is armed as if the message had
        *  gone out, so the next dhcp_poll() past the timeout tries again.
        *  Treating a transmit failure as fatal would give up on a condition
        *  that clears itself in microseconds. */
    }

    dhcp_arm_timer(1);
}

/* The ACK is the only place the machine gets configured, and it is done here
*  in task context rather than in the handler that received it: net_configure()
*  writes three separate words, and a shell reading net_ip() in the middle of
*  that from an interrupt would see half a configuration. */
static void dhcp_handle_ack(const dhcp_reply *r)
{
    uint32_t mask;
    uint32_t lease_ms;

    if(dhcp_current_state != DHCP_STATE_REQUEST)
        return;

    if(r->yiaddr == IP_ADDR_ANY || r->yiaddr == IP_ADDR_BROADCAST)
    {
        dhcp_fail("server confirmed an invalid address");
        return;
    }

    /* A server is required to send the netmask and in practice always does,
    *  but "required" is not "guaranteed" and a zero mask would make every
    *  destination look off-subnet and send every packet to the gateway. Fall
    *  back to the classful mask the first byte implies, which is what the
    *  address would have meant before CIDR and is right often enough to
    *  leave the machine usable. */
    mask = r->have_mask ? r->mask : dhcp_offered_mask;
    if(mask == 0)
    {
        uint8_t first = (uint8_t)(r->yiaddr >> 24);

        if(first < 128)
            mask = 0xFF000000UL;
        else if(first < 192)
            mask = 0xFFFF0000UL;
        else
            mask = 0xFFFFFF00UL;
    }

    net_configure(r->yiaddr,
                  mask,
                  r->have_router ? r->router : dhcp_offered_router);

    dhcp_dns_addr  = r->have_dns    ? r->dns    : dhcp_offered_dns;
    dhcp_server_id = r->have_server ? r->server : dhcp_offered_server;
    dhcp_lease_secs = r->have_lease ? r->lease  : dhcp_offered_lease;

    /* When the lease expires, in the same milliseconds-since-boot units as
    *  timer_get_ticks().
    *
    *  Two limits meet here and the clamp respects the tighter one. A lease
    *  is stated in seconds and may be 0xFFFFFFFF, the documented way of
    *  saying "forever"; times a thousand that is far outside a 32 bit
    *  millisecond count and would wrap to an expiry in the past, which reads
    *  as "expired the moment it was granted" -- the exact opposite of what
    *  the server said. But clamping merely to 0xFFFFFFFF is not enough
    *  either: a millisecond clock that wraps every 25 days can only order
    *  two instants that are less than half its range apart, so an expiry
    *  more than about 24 days out is indistinguishable from one 24 days in
    *  the past. Saturating at half the range keeps every value this returns
    *  comparable against timer_get_ticks() in the only way that works.
    *
    *  dhcp_lease_seconds() still reports what the server actually said, so
    *  nothing is hidden -- only the derived timestamp is capped. */
    if(dhcp_lease_secs > (DHCP_MAX_LEASE_MS / 1000UL))
        lease_ms = DHCP_MAX_LEASE_MS;
    else
        lease_ms = dhcp_lease_secs * 1000UL;

    dhcp_lease_end = (uint32_t)timer_get_ticks() + lease_ms;

    dhcp_current_state = DHCP_STATE_BOUND;
    dhcp_error = "";

    /* The port stays bound. Nothing needs it once the lease is in hand and
    *  renewal is not implemented, but unbinding here would mean a late
    *  duplicate ACK -- servers do resend -- arriving at a port with no
    *  handler, which the UDP layer counts as a drop. dhcp_stop() releases
    *  it when the caller is done. */

    /* Nothing is printed here, on success or on failure. The client has no
    *  idea what its caller is in the middle of saying, and a line appearing
    *  from underneath the shell's own progress report lands in the middle of
    *  it. Every state change is visible through dhcp_state() and every
    *  failure through dhcp_last_error(), which is what the caller reads. */
}

int dhcp_poll(void)
{
    dhcp_reply r;

    /* Anything waiting in the mailbox is acted on first: a reply that has
    *  already arrived makes the timer below irrelevant. */
    if(dhcp_inbox_full)
    {
        r = dhcp_inbox;         /* safe: the handler will not write while
                                *  the flag is set */
        dhcp_inbox_full = 0;

        switch(r.msg_type)
        {
        case DHCP_MSG_OFFER:
            dhcp_handle_offer(&r);
            break;

        case DHCP_MSG_ACK:
            dhcp_handle_ack(&r);
            break;

        case DHCP_MSG_NAK:
            /* The offer has been withdrawn. It happens when the address was
            *  given to somebody else between the OFFER and our REQUEST, or
            *  when the server has been reconfigured and no longer considers
            *  the address it offered to be its to give.
            *
            *  We stop, and deliberately do not start a new DISCOVER by
            *  ourselves. RFC 2131 allows restarting, but a client that
            *  restarts automatically on every NAK will loop against a server
            *  that is misconfigured rather than merely busy, filling the wire
            *  and printing nothing useful; a machine still holding a working
            *  lease from an earlier run would also lose it to that loop.
            *  Failing with the reason recorded turns that into one line the
            *  user can read and one command they can repeat. The existing
            *  configuration is left exactly as it is -- a NAK refuses an
            *  offer, it does not revoke an address already in use. */
            dhcp_fail("server refused the address (NAK)");
            break;

        default:
            /* A DECLINE or a RELEASE reaching a client, or a message type
            *  that does not exist. Nothing to do and nothing to say. */
            break;
        }

        return dhcp_current_state;
    }

    /* Nothing arrived. Only the two waiting states have a timer running;
    *  IDLE, BOUND and FAILED are all resting places. */
    if(dhcp_current_state != DHCP_STATE_DISCOVER &&
       dhcp_current_state != DHCP_STATE_REQUEST)
        return dhcp_current_state;

    if(dhcp_ms_since(dhcp_sent_ms) < dhcp_timeout_ms)
        return dhcp_current_state;

    if(dhcp_attempts >= DHCP_MAX_ATTEMPTS)
    {
        if(dhcp_current_state == DHCP_STATE_DISCOVER)
            dhcp_fail("no DHCP server answered");
        else
            dhcp_fail("no answer to the DHCP request");

        return dhcp_current_state;
    }

    /* Resend the message for the state we are in, with the same xid: this is
    *  a retransmission of one conversation, not a new one. A fresh xid would
    *  make the earlier attempt's answer -- which may be in flight right now
    *  -- unrecognisable, so a client that renumbered on every retry would get
    *  slower the worse the network was. */
    if(dhcp_current_state == DHCP_STATE_DISCOVER)
        dhcp_send_discover();
    else
        dhcp_send_request();

    dhcp_arm_timer(0);

    return dhcp_current_state;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

int dhcp_start(void)
{
    if(!net_up())
    {
        dhcp_error = "no network card";
        dhcp_current_state = DHCP_STATE_FAILED;
        return -1;
    }

    /* A second start abandons whatever was in progress. The port is only
    *  bound once; udp_bind() would otherwise fill a binding slot per
    *  attempt and eventually run out. */
    if(!dhcp_bound_port)
    {
        if(udp_bind(DHCP_PORT_CLIENT, dhcp_receive) < 0)
        {
            dhcp_error = "port 68 is already in use";
            dhcp_current_state = DHCP_STATE_FAILED;
            return -1;
        }
        dhcp_bound_port = 1;
    }

    /* Clear the mailbox before the new xid is chosen, so a reply to the
    *  previous exchange that is sitting in it cannot be read as an answer
    *  to this one. */
    dhcp_inbox_full = 0;

    dhcp_error = "";
    dhcp_xid = dhcp_make_xid();
    dhcp_start_ms = (uint32_t)timer_get_ticks();

    dhcp_offered_ip = 0;
    dhcp_offered_mask = 0;
    dhcp_offered_router = 0;
    dhcp_offered_dns = 0;
    dhcp_offered_server = 0;
    dhcp_offered_lease = 0;

    dhcp_server_id = 0;
    dhcp_dns_addr = 0;
    dhcp_lease_secs = 0;
    dhcp_lease_end = 0;

    /* The state is set before the message goes out, not after: the reply can
    *  arrive from the card's interrupt while udp_send_from() is still
    *  returning, and dhcp_receive() ignores everything unless the state says
    *  we are waiting. */
    dhcp_current_state = DHCP_STATE_DISCOVER;
    dhcp_arm_timer(1);

    if(dhcp_send_discover() < 0)
    {
        /* Not fatal, and not worth a special case: the timer is armed, so
        *  the first dhcp_poll() past the timeout sends it again. */
    }

    return 0;
}

void dhcp_stop(void)
{
    if(dhcp_bound_port)
    {
        udp_unbind(DHCP_PORT_CLIENT);
        dhcp_bound_port = 0;
    }

    dhcp_inbox_full = 0;

    /* Only an exchange in progress is abandoned. A lease already obtained
    *  stays configured, as dhcp.h promises -- this gives up on a
    *  conversation, it does not undo one that succeeded. */
    if(dhcp_current_state == DHCP_STATE_DISCOVER ||
       dhcp_current_state == DHCP_STATE_REQUEST)
        dhcp_current_state = DHCP_STATE_IDLE;
}

int dhcp_state(void)
{
    return dhcp_current_state;
}

const char *dhcp_state_name(void)
{
    switch(dhcp_current_state)
    {
    case DHCP_STATE_IDLE:     return "idle";
    case DHCP_STATE_DISCOVER: return "discovering";
    case DHCP_STATE_REQUEST:  return "requesting";
    case DHCP_STATE_BOUND:    return "bound";
    case DHCP_STATE_FAILED:   return "failed";
    default:                  return "unknown";
    }
}

uint32_t dhcp_server(void)
{
    return dhcp_server_id;
}

uint32_t dhcp_dns(void)
{
    return dhcp_dns_addr;
}

uint32_t dhcp_lease_seconds(void)
{
    return dhcp_lease_secs;
}

uint32_t dhcp_lease_expires(void)
{
    return dhcp_lease_end;
}

const char *dhcp_last_error(void)
{
    return dhcp_error;
}
