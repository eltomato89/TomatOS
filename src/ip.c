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
*  returns (ip_send(), icmp_send_echo(), icmp_last_reply(), net_ip(),
*  net_netmask(), net_gateway(), arp_lookup()) -- is in HOST order. The
*  conversion happens exactly where a value enters or leaves a header.
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
*  ip_send() is reached from two directions -- the shell sending a ping, and
*  the receive path answering an echo request from interrupt context. If the
*  card interrupts the shell halfway through building a packet, both would
*  write into this buffer. The busy flag catches that: the inner call gives
*  up instead of corrupting the outer packet. Losing one echo reply costs
*  the peer a retry, which it does anyway. */
static uint8_t  ip_tx_packet[IP_HDR_LEN + IP_MAX_PAYLOAD];
static volatile int ip_tx_busy = 0;

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
*
*  Routing: a destination inside our subnet is reached directly, anything
*  else goes to the gateway. The comparison is on the network part only --
*  getting it the wrong way round still works on the local wire and fails
*  for every address beyond it, which is a miserable thing to debug.
*
*  ARP: the next hop has to be resolved before a frame can be addressed.
*  arp_lookup() answers from its cache or returns 0 and sends a request, so
*  a first send to an unknown host cannot succeed. IP_ERR_PENDING says so.
*  A caller is expected to retry: send, wait a few milliseconds, send again,
*  a handful of times before giving up. A ping that stops at the first
*  refusal would never get a packet out of a cold cache. */
int ip_send(uint32_t dst, uint8_t protocol, const void *payload, uint32_t len)
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

    /* Routing decision. Same network part as ours -> direct. */
    if (((dst ^ net_ip()) & net_netmask()) == 0)
        next_hop = dst;
    else
        next_hop = net_gateway();

    /* Off our subnet with no gateway configured: nowhere to send it. */
    if (next_hop == 0)
        return IP_ERR_FAILED;

    mac = arp_lookup(next_hop);
    if (mac == 0)
        return IP_ERR_PENDING;   /* request is on its way -- retry */

    if (ip_tx_busy)
        return IP_ERR_FAILED;    /* interrupted a send in progress */
    ip_tx_busy = 1;

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
    ip->src            = htonl(net_ip());
    ip->dst            = htonl(dst);
    ip->checksum       = net_checksum(ip, IP_HDR_LEN);  /* header only */

    if (len > 0)
        memcpy(ip_tx_packet + IP_HDR_LEN, payload, (size_t)len);

    result = net_send(mac, ETH_TYPE_IP, ip_tx_packet, total);

    ip_tx_busy = 0;

    return (result == 0) ? 0 : IP_ERR_FAILED;
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

/* Runs in interrupt context: no printing on the normal path, and every
*  field is checked before it is believed. Anything that does not add up is
*  dropped without a word -- a hostile or simply broken packet is not a
*  reason to write to the screen from an interrupt. */
void ip_receive(const uint8_t *packet, uint32_t len)
{
    const ip_header *ip;
    uint32_t header_len;
    uint32_t total;
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

    /* Not ours, not our business. No forwarding -- this is a host. */
    if (ntohl(ip->dst) != net_ip())
        return;

    if (ip->protocol == IP_PROTO_ICMP)
        icmp_receive(packet + header_len, total - header_len, ntohl(ip->src));

    /* TCP and UDP have nobody to go to yet. */
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
