/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: IP and ICMP -- the layer between the ethernet code in net.c and
*        the shell's ping.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*
*  Two conventions run through this file and both are easy to get wrong:
*
*  Byte order. Every field inside the packed headers is in NETWORK order and
*  is only ever touched through htons/htonl/ntohs/ntohl. Everything else --
*  the addresses this file passes around and the ones the API takes and
*  returns (ip_send(), ip_send_from(), icmp_send_echo(), icmp_last_reply(),
*  udp_receive(), net_ip(), net_netmask(), net_gateway(), arp_lookup()) --
*  is in HOST order. The conversion happens exactly where a value enters or
*  leaves a header.
*
*  Checksum span. The IP checksum covers the IP header ALONE, never the
*  payload. The ICMP checksum covers the WHOLE ICMP message, header and
*  payload together. Both are computed with the checksum field zeroed first.
*  A packet with those two swapped looks perfectly fine in a hex dump and is
*  dropped by every peer on the wire.
*/

#include <system.h>
#include <stdio.h>
#include <net.h>
#include <tcp.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define IP_HDR_LEN        20              /* header without options    */
#define IP_MAX_PAYLOAD    (ETH_MTU - IP_HDR_LEN)
#define IP_VERSION         4
#define IP_DEFAULT_TTL    64

/* flags_fragment, once brought into host order */
#define IP_FLAG_MORE      0x2000          /* MF -- more fragments follow */
#define IP_FLAG_DONT      0x4000          /* DF -- do not fragment       */
#define IP_FRAG_OFFSET    0x1FFF          /* offset in 8 byte units      */

#define ICMP_HDR_LEN       8
#define ICMP_ECHO_PAYLOAD 32              /* what our own echo carries   */

/* Return codes of ip_send(). Negative, as the header promises. -1 is a
*  permanent failure for this packet, -2 only means "not yet" -- see the
*  comment at ip_send(). */
#define IP_ERR_FAILED     -1
#define IP_ERR_PENDING    -2

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* Identification field. Nothing depends on the value as long as packets do
*  not repeat one too quickly; a plain counter is enough while there is no
*  fragmentation. */
static uint16_t ip_next_id = 1;

/* One packet under construction. Static rather than on the stack: 1500
*  bytes is more than an interrupt stack should carry.
*
*  ip_send() is reached from more than one direction, and which directions has
*  changed. It used to be the shell against the card's interrupt answering an
*  echo request. Since the receive queue, the protocol work runs in the drain
*  TASK instead -- so the second writer is now another task, preempted by the
*  timer rather than by the card. Either way both would write into this buffer,
*  and the busy flag catches it: the second caller gives up rather than
*  corrupting the first one's packet. Losing an echo reply costs the peer a
*  retry, which it does anyway.
*
*  The change of direction matters for HOW the flag is taken, see
*  ip_tx_acquire() below. */
static uint8_t  ip_tx_packet[IP_HDR_LEN + IP_MAX_PAYLOAD];
static volatile int ip_tx_busy = 0;

/* Takes the transmit buffer if it is free. Returns 1 on success, 0 if
*  somebody else has it.
*
*  The test and the store have to be one indivisible step, and until the
*  receive queue landed they did not have to be. The old argument was written
*  down here and was sound at the time: the only other caller was an interrupt,
*  and an interrupt landing between the test and the store runs its own send to
*  completion and clears the flag again before the interrupted instruction
*  resumes -- serial, never overlapping.
*
*  That argument does not survive two TASKS. Task A tests and finds the flag
*  clear; the timer preempts it before the store; task B tests, also finds it
*  clear, takes it and starts building; the timer preempts B mid-packet; A
*  resumes, stores the flag that is already set, and builds into the same
*  buffer. Two half-packets, one buffer, and nothing refused anybody.
*
*  cli closes it. On a uniprocessor that blocks the scheduler as well as the
*  card, which is exactly the two things that could intervene, and it is held
*  for two instructions rather than for the whole packet -- the building itself
*  is still preemptible, which is the point of having a flag at all rather than
*  just masking interrupts around the send. The flags are saved and restored
*  rather than ending in sti, because this is also reachable with interrupts
*  already off. */
static int ip_tx_acquire(void)
{
    uint32_t flags;
    int      got;

    __asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");

    got = !ip_tx_busy;
    if (got) ip_tx_busy = 1;

    __asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");

    return got;
}

/* The echo reply we build in the receive path, and the one we send from
*  icmp_send_echo(). Separate buffers: the first is only ever used in
*  interrupt context, the second only from a task. */
static uint8_t icmp_reply_buffer[IP_MAX_PAYLOAD];
static uint8_t icmp_echo_buffer[ICMP_HDR_LEN + ICMP_ECHO_PAYLOAD];

/* Last echo reply that came back, filled in from interrupt context. The
*  pending flag is written last so a reader never sees half an entry, and
*  icmp_last_reply() clears it -- a reply is reported exactly once, so a
*  stale one cannot be mistaken for the answer to the next ping. */
static volatile uint32_t icmp_reply_from     = 0;
static volatile uint16_t icmp_reply_id       = 0;
static volatile uint16_t icmp_reply_sequence = 0;
static volatile int      icmp_reply_pending  = 0;

/* local helper functions -- not declared in any header */
static void icmp_receive(const uint8_t *msg, uint32_t len, uint32_t from);
static void icmp_echo_reply(const uint8_t *request, uint32_t len, uint32_t to);
static int  ip_dst_is_ours(uint32_t dst);
static int  ip_dst_is_local(uint32_t dst);

/* Entry point of the receive path. net.c calls this for every frame of type
*  ETH_TYPE_IP, with the ethernet header already stripped: "packet" points at
*  the IP header and "len" is how many bytes are readable from there
*  (frame length minus ETH_HDR_LEN, so it may still include ethernet padding
*  beyond total_length). Declared here rather than in net.h because net.h is
*  the shared contract and this is the one seam between the two files. */
void ip_receive(const uint8_t *packet, uint32_t len);

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/* Builds the IP header around a payload and hands the packet to net_send().
*  The source address is spelled out by the caller instead of being taken
*  from net_ip(), and this is the only implementation -- ip_send() below is a
*  wrapper. Two copies of the header construction would be two places to fix
*  the next time a field changes, and they would not stay identical.
*
*  Why a caller would pass anything other than net_ip(): a DHCP client has no
*  address until the exchange it is trying to run has finished, so its
*  DISCOVER and REQUEST go out from IP_ADDR_ANY. That is the one legitimate
*  case; everything else uses ip_send().
*
*  Note that "src" takes no part in the routing decision below. Which wire a
*  packet leaves by is a property of this machine's interface, not of the
*  number it chooses to write in the source field -- and an unconfigured
*  sender has no source address to route by in the first place.
*
*  Routing: the limited broadcast is never routed; a destination inside our
*  subnet is reached directly; anything else goes to the gateway. The subnet
*  comparison is on the network part only -- getting it the wrong way round
*  still works on the local wire and fails for every address beyond it, which
*  is a miserable thing to debug.
*
*  Unconfigured (address, netmask and gateway all zero) the subnet test is
*  vacuous: (dst ^ 0) & 0 is zero for every destination, so everything is
*  treated as direct, which is exactly right when the only reachable thing is
*  the local wire.
*
*  ARP: the next hop has to be resolved before a frame can be addressed.
*  arp_lookup() answers from its cache or returns 0 and sends a request, so
*  a first send to an unknown host cannot succeed. IP_ERR_PENDING says so.
*  A caller is expected to retry: send, wait a few milliseconds, send again,
*  a handful of times before giving up. A ping that stops at the first
*  refusal would never get a packet out of a cold cache. A broadcast is the
*  exception: arp_lookup() answers it with the broadcast MAC out of hand, so
*  a DHCP DISCOVER goes out on the first attempt from a cold cache. */
int ip_send_from(uint32_t src, uint32_t dst, uint8_t protocol,
                 const void *payload, uint32_t len)
{
    ip_header *ip;
    const uint8_t *mac;
    uint32_t next_hop;
    uint32_t total;
    int result;

    if (!net_up())
        return IP_ERR_FAILED;
    if (len > IP_MAX_PAYLOAD)
        return IP_ERR_FAILED;
    if (len > 0 && payload == 0)
        return IP_ERR_FAILED;

    /* Routing decision.
    *
    *  The limited broadcast goes on this wire and nowhere else, by
    *  definition. Without this case a configured machine broadcasting --
    *  a DHCP client rebinding, or one whose lease ran out starting over --
    *  would find 255.255.255.255 outside its subnet and hand the frame to
    *  the gateway's MAC: a broadcast delivered to exactly one station,
    *  which is not a broadcast at all. */
    if (dst == IP_ADDR_BROADCAST)
        next_hop = dst;
    else if (((dst ^ net_ip()) & net_netmask()) == 0)  /* our network -> direct */
        next_hop = dst;
    else
        next_hop = net_gateway();

    /* Off our subnet with no gateway configured: nowhere to send it. */
    if (next_hop == 0)
        return IP_ERR_FAILED;

    mac = arp_lookup(next_hop);
    if (mac == 0)
        return IP_ERR_PENDING;   /* request is on its way -- retry */

    /* Everything above this line only reads. From here on the shared
    *  transmit buffer is written, which is what the flag protects: a send
    *  reached from an interrupt while a task is mid-packet gives up rather
    *  than overwriting it. Taken indivisibly -- see ip_tx_acquire(), which
    *  explains why the plain test-then-store this used to be stopped being
    *  enough the moment the receive path moved out of the interrupt and into
    *  a task of its own. */
    if (!ip_tx_acquire())
        return IP_ERR_FAILED;    /* somebody else is mid-packet */

    total = IP_HDR_LEN + len;

    ip = (ip_header *)ip_tx_packet;
    ip->version_ihl    = (IP_VERSION << 4) | (IP_HDR_LEN / 4);
    ip->tos            = 0;
    ip->total_length   = htons((uint16_t)total);   /* header AND payload */
    ip->id             = htons(ip_next_id++);
    ip->flags_fragment = 0;      /* no flags, offset 0 -- we never fragment */
    ip->ttl            = IP_DEFAULT_TTL;
    ip->protocol       = protocol;
    ip->checksum       = 0;      /* zeroed before summing, never after */
    ip->src            = htonl(src);
    ip->dst            = htonl(dst);
    ip->checksum       = net_checksum(ip, IP_HDR_LEN);  /* header only */

    if (len > 0)
        memcpy(ip_tx_packet + IP_HDR_LEN, payload, (size_t)len);

    result = net_send(mac, ETH_TYPE_IP, ip_tx_packet, total);

    ip_tx_busy = 0;

    return (result == 0) ? 0 : IP_ERR_FAILED;
}

/* The ordinary case: send from whatever address this machine is configured
*  with. Every caller but the DHCP client wants this one. */
int ip_send(uint32_t dst, uint8_t protocol, const void *payload, uint32_t len)
{
    return ip_send_from(net_ip(), dst, protocol, payload, len);
}

/* ------------------------------------------------------------------ */
/* ICMP -- sending                                                     */
/* ------------------------------------------------------------------ */

/* Sends one echo request. The reply comes back asynchronously and is picked
*  up by icmp_last_reply(); the id and sequence let the caller tell its own
*  replies apart from anything else on the wire.
*
*  Returns what ip_send() returned, so IP_ERR_PENDING (-2) here means the
*  next hop is still being resolved and the call is worth repeating. */
int icmp_send_echo(uint32_t dst, uint16_t id, uint16_t sequence)
{
    icmp_header *icmp;
    uint32_t total;
    uint32_t i;

    total = ICMP_HDR_LEN + ICMP_ECHO_PAYLOAD;

    icmp = (icmp_header *)icmp_echo_buffer;
    icmp->type     = ICMP_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(id);
    icmp->sequence = htons(sequence);

    /* The usual printable filler. A peer echoes it back untouched, which
    *  makes a captured reply readable at a glance. */
    for (i = 0; i < ICMP_ECHO_PAYLOAD; i++)
        icmp_echo_buffer[ICMP_HDR_LEN + i] = (uint8_t)('a' + (i % 26));

    /* Whole message: header and payload together. */
    icmp->checksum = net_checksum(icmp_echo_buffer, total);

    return ip_send(dst, IP_PROTO_ICMP, icmp_echo_buffer, total);
}

/* Hands out the last echo reply that arrived and clears it, so one reply is
*  reported once. Returns 1 when there was one, 0 otherwise; the out
*  parameters stay untouched in the second case. Any of them may be 0. */
int icmp_last_reply(uint32_t *from, uint16_t *id, uint16_t *sequence)
{
    if (!icmp_reply_pending)
        return 0;

    if (from != 0)
        *from = icmp_reply_from;
    if (id != 0)
        *id = icmp_reply_id;
    if (sequence != 0)
        *sequence = icmp_reply_sequence;

    icmp_reply_pending = 0;

    return 1;
}

/* ------------------------------------------------------------------ */
/* Receiving                                                           */
/* ------------------------------------------------------------------ */

/* Is this destination address ours alone -- the single unicast address this
*  machine answers to? A broadcast is not, and an unconfigured machine has no
*  such address at all, so both answer 0.
*
*  Used for the traffic that may only be replied to when it was aimed at this
*  machine specifically, which is ICMP echo: see ip_receive() below. */
static int ip_dst_is_ours(uint32_t dst)
{
    uint32_t me;

    me = net_ip();

    return (me != IP_ADDR_ANY && dst == me);
}

/* Should a packet with this destination be delivered to this host at all?
*
*  Until DHCP there was one answer -- our own address -- and it was right for
*  every packet a configured machine has any business with. It also drops
*  every DHCP reply, which is why this exists. The accepted cases, and why
*  each is needed:
*
*    - our own unicast address. The ordinary case, unchanged.
*
*    - 255.255.255.255, the limited broadcast. A DHCP server answers a client
*      that asked to be broadcast to here, and a client whose lease expired
*      hears its own segment's traffic here. This one is not optional: it is
*      how a machine with no address can be spoken to at all.
*
*    - our subnet's broadcast, our address with the host bits set. A server
*      on our own wire may answer there once we are configured -- a renewal
*      or a rebind -- and it is what every other host on the segment accepts.
*      Skipped while the netmask is zero, because "our address with the host
*      bits set" is then 255.255.255.255 for any address at all, which is the
*      case above and not a second one.
*
*    - anything, but ONLY while this machine has no address of its own. This
*      is the case worth being uncomfortable about, so: a DHCP server that
*      gets a request with the broadcast flag clear may unicast its OFFER and
*      ACK to the address it is about to hand out -- addressed to a machine
*      that is not yet us, and unmatchable by definition, because knowing the
*      address is the entire point of the exchange. There is nothing in the
*      IP header to compare it against.
*
*      What keeps this from being "accept everything" is that it is not the
*      IP layer's only filter. To reach here at all a frame passed
*      net_receive(), which drops anything whose ethernet destination is
*      neither our MAC nor the broadcast MAC -- so a unicast accepted here
*      was addressed to this card by a server that had our MAC from our own
*      DISCOVER. And the moment net_configure() runs, this case stops
*      applying: it is live only in the window where it is needed.
*
*  Deliberately still dropped: another host's unicast address once we have
*  one of our own, every multicast group (we join none), and 0.0.0.0, which
*  is "no address" and never a destination -- accepting it would mean an
*  unconfigured machine treating an unaddressed packet as its own. */
static int ip_dst_is_local(uint32_t dst)
{
    uint32_t me;
    uint32_t mask;

    if (dst == IP_ADDR_ANY)
        return 0;

    if (dst == IP_ADDR_BROADCAST)
        return 1;

    me = net_ip();

    if (me == IP_ADDR_ANY)
        return 1;        /* no address yet -- see the note above */

    if (dst == me)
        return 1;

    mask = net_netmask();
    if (mask != 0 && dst == (me | ~mask))
        return 1;        /* our subnet's broadcast */

    return 0;
}

/* Runs in interrupt context: no printing on the normal path, and every
*  field is checked before it is believed. Anything that does not add up is
*  dropped without a word -- a hostile or simply broken packet is not a
*  reason to write to the screen from an interrupt. */
void ip_receive(const uint8_t *packet, uint32_t len)
{
    const ip_header *ip;
    uint32_t header_len;
    uint32_t total;
    uint32_t src;
    uint32_t dst;
    uint16_t fragment;

    if (packet == 0 || len < IP_HDR_LEN)
        return;

    ip = (const ip_header *)packet;

    if ((ip->version_ihl >> 4) != IP_VERSION)
        return;

    /* Header length is in 4 byte words and may be larger than 20 when
    *  options are present. It has to be at least a bare header and must
    *  still lie inside what actually arrived. */
    header_len = (uint32_t)(ip->version_ihl & 0x0F) * 4;
    if (header_len < IP_HDR_LEN || header_len > len)
        return;

    /* total_length counts header plus payload. A shorter frame means the
    *  packet is truncated; a longer one is just ethernet padding, which is
    *  why the payload length below comes from the header and not from len. */
    total = ntohs(ip->total_length);
    if (total < header_len || total > len)
        return;

    /* The checksum field is part of the sum, so a valid header sums to
    *  0xFFFF and its complement -- what net_checksum() returns -- is 0.
    *  Options, if any, are covered as well: the span is header_len. */
    if (net_checksum(packet, header_len) != 0)
        return;

    /* Fragments: MF set, or a non-zero offset. Reassembly means holding
    *  pieces and timing them out, which is its own undertaking, so they are
    *  dropped here rather than passed up half-complete. DF is a sender's
    *  instruction and is ignored on receive. Nothing that answers a ping
    *  arrives fragmented in practice. */
    fragment = ntohs(ip->flags_fragment);
    if ((fragment & IP_FLAG_MORE) || (fragment & IP_FRAG_OFFSET))
        return;

    /* Not for this host, not our business. No forwarding -- this is a host,
    *  and what "for this host" covers is the whole of ip_dst_is_local(). */
    src = ntohl(ip->src);
    dst = ntohl(ip->dst);

    if (!ip_dst_is_local(dst))
        return;

    if (ip->protocol == IP_PROTO_ICMP)
    {
        /* Echo requests are answered, and an answer names this machine as
        *  its source -- so only requests aimed at this machine alone get
        *  one. A broadcast ping is heard and ignored: replying to it turns
        *  every host on the segment into an amplifier for one forged packet,
        *  and an unconfigured machine has no source address to reply with
        *  anyway. This is the behaviour before broadcasts were accepted at
        *  all, kept deliberately -- the wider acceptance above exists for
        *  DHCP and nothing here needed it. */
        if (!ip_dst_is_ours(dst))
            return;

        icmp_receive(packet + header_len, total - header_len, src);
    }
    else if (ip->protocol == IP_PROTO_UDP)
    {
        /* Both addresses, because the UDP checksum covers a pseudo header
        *  built from them and the datagram alone does not carry them. The
        *  destination is passed as it arrived rather than as net_ip(): a
        *  DHCP reply is addressed to a broadcast or to an address we do not
        *  have yet, and summing over what we wish it said would fail every
        *  check. udp_receive() is given the UDP header onwards, so the
        *  length is the IP payload -- total_length, not the frame length,
        *  which may still carry ethernet padding. */
        udp_receive(src, dst, packet + header_len, total - header_len);
    }
    else if (ip->protocol == IP_PROTO_TCP)
    {
        /* Same reasoning as UDP above, and the same pseudo header: TCP's
        *  checksum covers the addresses too. Unlike UDP there is no
        *  unconfigured case to allow for -- a connection cannot exist before
        *  the machine has an address -- but the destination is still passed
        *  as it arrived rather than as net_ip(), so that the one place the
        *  checksum is computed does not have to know which of the two it is
        *  looking at. */
        tcp_receive(src, dst, packet + header_len, total - header_len);
    }

    /* Everything else is dropped: this stack speaks ICMP, UDP and TCP. */
}

/* One ICMP message, already known to be addressed to us. */
static void icmp_receive(const uint8_t *msg, uint32_t len, uint32_t from)
{
    const icmp_header *icmp;

    if (len < ICMP_HDR_LEN)
        return;

    /* Whole message, header and payload -- unlike the IP checksum above. */
    if (net_checksum(msg, len) != 0)
        return;

    icmp = (const icmp_header *)msg;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0)
    {
        icmp_echo_reply(msg, len, from);
    }
    else if (icmp->type == ICMP_ECHO_REPLY && icmp->code == 0)
    {
        /* Record it for whoever is waiting. The pending flag goes last so
        *  a reader that looks in between never sees a half written entry. */
        icmp_reply_from     = from;
        icmp_reply_id       = ntohs(icmp->id);
        icmp_reply_sequence = ntohs(icmp->sequence);
        icmp_reply_pending  = 1;
    }
}

/* Answers an echo request: the same id, the same sequence and the same
*  payload come back, only the type changes and the checksum is recomputed
*  over the result. This is what makes the machine answer a ping from
*  outside.
*
*  If ARP has not seen the sender yet, ip_send() returns IP_ERR_PENDING and
*  this reply is lost -- there is no retrying from interrupt context. The
*  peer repeats its request a second later and by then the ARP request we
*  sent has been answered, so at worst the first ping of a session goes
*  missing. */
static void icmp_echo_reply(const uint8_t *request, uint32_t len, uint32_t to)
{
    icmp_header *reply;

    if (len > IP_MAX_PAYLOAD)
        return;   /* longer than we can put back on the wire in one piece */

    memcpy(icmp_reply_buffer, request, (size_t)len);

    reply = (icmp_header *)icmp_reply_buffer;
    reply->type     = ICMP_ECHO_REPLY;
    reply->code     = 0;
    reply->checksum = 0;   /* zeroed first, then summed over everything */
    reply->checksum = net_checksum(icmp_reply_buffer, len);

    ip_send(to, IP_PROTO_ICMP, icmp_reply_buffer, len);
}
