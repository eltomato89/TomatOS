/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: UDP -- the thinnest thing that lets two protocols share a wire, and
*        the pseudo header checksum that TCP would use as well.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*
*  There are no sockets here and no receive queues. A datagram that arrives
*  is handed to whoever bound its port, in the interrupt handler that took it
*  off the card, and that is the whole delivery model. It is enough for a
*  DHCP client, which is what it was written for, and it is honest about what
*  it is: a queue would need memory the receive path may not allocate and a
*  wakeup the scheduler does not have yet.
*
*  Three conventions run through this file:
*
*  Byte order. Every field inside the packed headers is in NETWORK order and
*  is only ever touched through htons/ntohs. Every address and port in an
*  interface -- the arguments of udp_send(), the ones handed to a handler --
*  is in HOST order, exactly as in net.c and ip.c. The pseudo header below is
*  the one place where an address that came in host order has to be put back
*  into network order before it is summed, and getting that wrong produces a
*  checksum that is wrong on every packet and looks fine in a hex dump.
*
*  Interrupt context. udp_receive() is reached from the card's interrupt
*  handler through net_receive() and ip_receive(), so everything it calls --
*  including a bound handler -- runs with the rest of the kernel stopped
*  mid-instruction. Nothing on that path prints, allocates or waits, and the
*  work is bounded by one walk of an eight entry table.
*
*  Checksum span. Unlike IP, which sums its header alone, and unlike ICMP,
*  which sums its message, UDP sums a pseudo header that is never
*  transmitted -- and the length of the datagram appears twice, once in that
*  invisible header and once in the real one. See net_checksum_pseudo().
*/

#include <system.h>
#include <stdio.h>
#include <net.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/* An IP header without options. Only needed to work out how much payload
*  still fits in one frame; ip.c owns the real definition. */
#define IP_HDR_LEN         20

/* What fits in a datagram that must not be fragmented -- and this stack
*  never fragments, in either direction. */
#define UDP_MAX_PAYLOAD    (ETH_MTU - IP_HDR_LEN - UDP_HDR_LEN)

/* Return codes. -1 is a permanent failure for this datagram; anything else
*  negative comes straight out of ip_send_from() and keeps its meaning
*  there, in particular -2 for "the next hop is still being resolved, try
*  again in a moment". See the comment above udp_send_from(). */
#define UDP_ERR_FAILED     -1

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* The pseudo header, laid out exactly as RFC 768 prescribes it, so that it
*  can be summed as a block of bytes like any real header. It never leaves
*  the machine: it exists only to make the checksum depend on the addresses,
*  which the UDP header itself does not carry. That is what stops a datagram
*  from being accepted after it was delivered to the wrong host.
*
*  The zero byte is not padding for alignment -- it is the high half of the
*  protocol field, which is 8 bits in IP and 16 bits here. */
typedef struct
{
    uint32_t src;             /* network order                              */
    uint32_t dst;             /* network order                              */
    uint8_t  zero;            /* always 0                                   */
    uint8_t  protocol;        /* IP_PROTO_UDP, or TCP's number one day      */
    uint16_t length;          /* UDP header AND payload -- the same number  */
                              /* as the real header's length field          */
} __attribute__((packed)) udp_pseudo_header;

/* One bound port. port == 0 marks a free slot: port 0 is reserved in UDP
*  and no sane listener asks for it, so it costs nothing to spend it as the
*  "empty" marker and saves a separate flag that would have to be written
*  and read in the right order against the interrupt handler.
*
*  Both fields are volatile because the interrupt handler reads this table
*  while task context writes it, and the compiler must not cache either
*  across the sequence points that make the writes safe. */
typedef struct
{
    volatile uint16_t    port;    /* host order, 0 = free */
    volatile udp_handler fn;
} udp_binding;

static udp_binding udp_bindings[UDP_MAX_BINDINGS];

/* One datagram under construction. Static rather than on the stack, for the
*  same reason as in ip.c: an interrupt stack has no business carrying 1480
*  bytes.
*
*  And it has the same problem, so it gets the same guard. udp_send() is
*  reachable from a task -- a DHCP client asking for a lease -- and from
*  interrupt context, because a bound handler runs in the interrupt handler
*  and is allowed to answer what it just received. If the card interrupts a
*  task halfway through building a datagram, both writers would be in this
*  buffer at once and the packet that goes out is neither.
*
*  The flag is not a lock and does not pretend to be one. On a uniprocessor
*  kernel the only concurrency is an interrupt preempting a task, never the
*  other way round, so the inner call simply gives up: the outer datagram
*  stays intact and the inner sender gets a permanent failure it can retry
*  from its own timer. Losing one datagram is what UDP is for. */
static uint8_t      udp_tx_packet[UDP_HDR_LEN + UDP_MAX_PAYLOAD];
static volatile int udp_tx_busy = 0;

/* Counters. Every datagram that reaches udp_receive() ends up in exactly
*  one of the two receive counters, so the pair adds up to what IP handed
*  us: stat_rx counts the ones delivered to a handler, stat_drop everything
*  discarded -- malformed, checksum failed, or simply nobody listening. */
static uint32_t stat_rx   = 0;
static uint32_t stat_tx   = 0;
static uint32_t stat_drop = 0;

/* ------------------------------------------------------------------ */
/* Checksum                                                            */
/* ------------------------------------------------------------------ */

/* One block of bytes added into a running one's complement sum, big endian
*  pairs, exactly as net_checksum() does it -- this is that function taken
*  apart so that the pseudo header and the real datagram can be summed as if
*  they were one buffer, which is precisely what the standard asks for.
*
*  The odd trailing byte is padded with a ZERO, not with whatever follows in
*  memory. Note that only the LAST block may be odd: the pseudo header is 12
*  bytes, so padding it in the middle would silently insert a byte that the
*  peer does not sum. That is why this helper is called with the payload
*  last and why no caller may split the payload across two calls. */
static uint32_t checksum_add(uint32_t sum, const void *data, uint32_t len)
{
    const uint8_t *p;

    p = (const uint8_t *)data;

    while(len > 1)
    {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p += 2;
        len -= 2;

        /* Fold early so the 32 bit accumulator cannot overflow on a long
        *  buffer. Same reasoning as in net_checksum(). */
        if(sum & 0x80000000UL)
            sum = (sum & 0xFFFFUL) + (sum >> 16);
    }

    if(len == 1)
        sum += (uint32_t)p[0] << 8;

    return sum;
}

/* The checksum UDP and TCP share.
*
*  "data" points at the real UDP header, with its own checksum field zeroed
*  when computing and left in place when verifying, and "len" is the length
*  of the datagram -- header plus payload. That same length goes into the
*  pseudo header, which is the detail that catches people out: the number
*  appears twice, and it is the same number both times. It is not the IP
*  total length and not the payload length alone.
*
*  The addresses arrive in HOST order, like every address in this stack that
*  is not inside a packed header, and are converted here. They are summed in
*  network order because that is how the peer sums them, and a sum is not
*  symmetric in the byte order of its inputs: a swapped address gives a
*  wrong checksum for every single packet, one that no capture makes
*  obvious because the pseudo header is not on the wire to compare against.
*
*  The result is returned in NETWORK order, ready to be assigned straight
*  into the header, with one substitution: a computed zero goes out as
*  0xFFFF. In one's complement arithmetic those are the same value -- +0 and
*  -0 -- and UDP spends the bit pattern 0x0000 on a different meaning, "the
*  sender did not compute a checksum at all". Sending a real zero would tell
*  the peer not to check anything.
*
*  Verifying is the same call with the checksum field left in place. A sound
*  datagram then sums to 0xFFFF and its complement is zero -- which this
*  function turns back into 0xFFFF by the rule above, so the test a receiver
*  makes is "== 0xFFFF", not "== 0". 0xFFFF is its own byte swap, so that
*  test needs no conversion. */
uint16_t net_checksum_pseudo(uint32_t src, uint32_t dst,
                             uint8_t protocol,
                             const void *data, uint32_t len)
{
    udp_pseudo_header pseudo;
    uint32_t sum;
    uint16_t result;

    pseudo.src      = htonl(src);
    pseudo.dst      = htonl(dst);
    pseudo.zero     = 0;
    pseudo.protocol = protocol;
    pseudo.length   = htons((uint16_t)len);

    /* Pseudo header first, then the datagram, in one continuous sum. The
    *  pseudo header is 12 bytes, so the boundary never falls inside a word
    *  and the two blocks add up as if they were contiguous. */
    sum = checksum_add(0, (const void *)&pseudo, (uint32_t)sizeof(pseudo));
    sum = checksum_add(sum, data, len);

    while(sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    result = htons((uint16_t)(~sum & 0xFFFFUL));

    /* +0 becomes -0. Both are zero, only one of them is a checksum. */
    if(result == 0)
        result = 0xFFFF;

    return result;
}

/* ------------------------------------------------------------------ */
/* Binding                                                             */
/* ------------------------------------------------------------------ */

/* Claims a port. Returns 0 on success, -1 when the port or the handler is
*  unusable or the table is full.
*
*  Binding a port that is already bound REPLACES its handler and succeeds.
*  There is no ownership token in this interface -- a caller cannot prove a
*  binding is its own -- so refusing would mostly punish the honest case: a
*  DHCP client that restarts after a failed lease would find its own port
*  taken and could never get it back without unbinding a binding it cannot
*  identify. Replacing makes bind idempotent, which is the behaviour a
*  restarting protocol actually wants. It does mean two protocols that pick
*  the same port silently share it, with the later one winning; with eight
*  slots and a hand written stack that is a bug to find in review, not one
*  worth spending a table entry and an error path on.
*
*  Ordering against the interrupt handler. This runs in task context and can
*  be interrupted by udp_receive() between any two instructions, so a slot
*  must never be visible with a port set and no handler behind it. The
*  handler is therefore written FIRST and the port -- the field that makes
*  the slot live -- LAST. Both are single aligned stores, which x86 does not
*  tear, and both are volatile so the compiler may not reorder them. No
*  interrupts are disabled: there is nothing to disable them for.
*
*  A handler may call this on itself. Nothing here walks a list that the
*  dispatcher below is also walking, and slots are never moved or compacted,
*  so an index the dispatcher is holding stays the slot it was. */
int udp_bind(uint16_t port, udp_handler fn)
{
    int i;

    /* Port 0 is the free marker of this table and is reserved in UDP
    *  besides; a handler of 0 would be a call through a null pointer in
    *  interrupt context, which is a triple fault, not a bug report. */
    if(port == 0 || fn == 0)
        return -1;

    for(i = 0; i < UDP_MAX_BINDINGS; i++)
    {
        if(udp_bindings[i].port == port)
        {
            /* Already live: only the handler changes. One store, so the
            *  interrupt handler sees either the old function or the new
            *  one and never something in between. */
            udp_bindings[i].fn = fn;
            return 0;
        }
    }

    for(i = 0; i < UDP_MAX_BINDINGS; i++)
    {
        if(udp_bindings[i].port == 0)
        {
            udp_bindings[i].fn = fn;      /* first ...            */
            udp_bindings[i].port = port;  /* ... and this publishes it */
            return 0;
        }
    }

    return -1;   /* table full */
}

/* Releases a port. Returns 0 when there was a binding, -1 when there was
*  none -- which is not a failure to be afraid of, only the answer to "was
*  anything actually listening": the port is free either way.
*
*  The port is cleared FIRST, so the slot stops being live before the
*  handler is dropped; the reverse order would leave a window in which an
*  arriving datagram is dispatched to a null pointer.
*
*  A handler may unbind itself from inside the interrupt handler. The
*  dispatcher below has already copied the function pointer to a local by
*  the time the handler runs and does not touch the table afterwards. */
int udp_unbind(uint16_t port)
{
    int i;

    if(port == 0)
        return -1;

    for(i = 0; i < UDP_MAX_BINDINGS; i++)
    {
        if(udp_bindings[i].port == port)
        {
            udp_bindings[i].port = 0;    /* retire the slot ... */
            udp_bindings[i].fn = 0;      /* ... then forget the handler */
            return 0;
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* Receiving                                                           */
/* ------------------------------------------------------------------ */

/* One datagram, already known to be addressed to this machine -- ip.c
*  checked that and stripped the IP header. "packet" points at the UDP
*  header and "len" is how many bytes are readable from there.
*
*  Runs in interrupt context, so nothing here prints on the normal path and
*  every field is checked before it is believed. Both addresses are passed
*  in because the checksum covers them and the UDP header does not carry
*  them.
*
*  What is accepted and what is dropped:
*
*    - Shorter than a UDP header: dropped. There is nothing to read.
*    - A length field below UDP_HDR_LEN or larger than the bytes that
*      actually arrived: dropped. That second test is the important one.
*      Trusting a length from the wire and copying that many bytes is how a
*      remote sender gets the kernel to read past the end of the driver's
*      receive ring, and the sender does not have to be hostile for it to
*      happen -- a truncated frame is enough.
*    - A length field SMALLER than what arrived is fine and normal: ethernet
*      pads every frame out to 60 bytes, so a short datagram routinely
*      arrives with trailing bytes that are not part of it. This is exactly
*      why the payload length is taken from the header and not from len.
*    - Checksum field zero: ACCEPTED without checking. In IPv4 that is the
*      documented way of saying "the sender did not compute one", not a
*      corrupt datagram, and a receiver that rejects it drops perfectly
*      good traffic from senders that leave it off.
*    - Checksum field set but wrong: dropped.
*    - Nobody bound the port: dropped and counted, silently. This is not an
*      error, it is the ordinary condition of a network -- broadcasts and
*      strays arrive on ports nothing is listening to all day. Printing here
*      would put a line on the screen from an interrupt handler at the
*      convenience of anyone on the wire.
*/
void udp_receive(uint32_t src_ip, uint32_t dst_ip,
                 const uint8_t *packet, uint32_t len)
{
    const udp_header *udp;
    udp_handler handler;
    uint32_t datagram_len;
    uint16_t dst_port;
    int i;

    if(packet == 0 || len < UDP_HDR_LEN)
    {
        stat_drop++;
        return;
    }

    udp = (const udp_header *)packet;

    /* Header plus payload, as the sender counted it. */
    datagram_len = (uint32_t)ntohs(udp->length);
    if(datagram_len < UDP_HDR_LEN || datagram_len > len)
    {
        stat_drop++;
        return;
    }

    /* The checksum is optional in IPv4 and zero means it was not computed.
    *  When it is there it covers the pseudo header, this header with the
    *  field still in place, and the payload -- and a sound datagram then
    *  comes back as 0xFFFF. The span is the length the header claims, not
    *  the padded frame: ethernet's padding is not part of the datagram and
    *  the sender never summed it. */
    if(udp->checksum != 0)
    {
        if(net_checksum_pseudo(src_ip, dst_ip, IP_PROTO_UDP,
                               packet, datagram_len) != 0xFFFF)
        {
            stat_drop++;
            return;
        }
    }

    dst_port = ntohs(udp->dst_port);

    /* Find the listener and let go of the table before calling it, so the
    *  handler is free to bind or unbind -- including itself -- without
    *  anything here still depending on what the slot said. */
    handler = 0;
    for(i = 0; i < UDP_MAX_BINDINGS; i++)
    {
        if(udp_bindings[i].port == dst_port)
        {
            handler = udp_bindings[i].fn;
            break;
        }
    }

    if(handler == 0)
    {
        stat_drop++;   /* nobody listening -- normal, and not worth a word */
        return;
    }

    stat_rx++;

    /* The payload alone, its length taken from the checked header field.
    *  The buffer belongs to the driver's receive ring and is reused as soon
    *  as this returns; a handler that wants to keep anything copies it. */
    handler(src_ip, ntohs(udp->src_port),
            packet + UDP_HDR_LEN, datagram_len - UDP_HDR_LEN);
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/* Builds a datagram and hands it to IP, with the source address spelled out
*  instead of taken from net_ip().
*
*  This exists for the same single situation as ip_send_from(): a DHCP
*  client has no address yet and must send from IP_ADDR_ANY. The source
*  address is not decoration here -- it is summed into the pseudo header, so
*  a datagram sent from 0.0.0.0 has to be checksummed as coming from
*  0.0.0.0 or the server discards it.
*
*  The return value of ip_send_from() is passed through UNCHANGED, and that
*  matters for one of them: -2 does not mean the send failed, it means ARP
*  has not resolved the next hop yet and the call is worth repeating. Turn
*  it into a generic error here and a first datagram to a cold ARP cache can
*  never be sent, because the cache is only ever filled by the request that
*  the failed lookup just sent. icmp_send_echo() passes it through for the
*  same reason and callers of both are expected to retry a handful of times,
*  a few milliseconds apart. */
int udp_send_from(uint32_t src, uint32_t dst,
                  uint16_t src_port, uint16_t dst_port,
                  const void *payload, uint32_t len)
{
    udp_header *udp;
    uint32_t total;
    int result;

    /* One datagram, one frame: this stack does not fragment. */
    if(len > UDP_MAX_PAYLOAD)
        return UDP_ERR_FAILED;
    if(len > 0 && payload == 0)
        return UDP_ERR_FAILED;

    /* See the comment at udp_tx_packet: an interrupt that lands inside a
    *  send gives up rather than writing into a half built datagram. */
    if(udp_tx_busy)
        return UDP_ERR_FAILED;
    udp_tx_busy = 1;

    total = UDP_HDR_LEN + len;

    udp = (udp_header *)udp_tx_packet;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((uint16_t)total);  /* header AND payload */
    udp->checksum = 0;                       /* zeroed before summing */

    if(len > 0)
        memcpy(udp_tx_packet + UDP_HDR_LEN, payload, (size_t)len);

    /* Sum over the finished datagram, payload included, with the pseudo
    *  header in front. The same "total" the header carries goes into the
    *  pseudo header, and net_checksum_pseudo() turns a computed zero into
    *  0xFFFF so the field never accidentally reads as "not computed". */
    udp->checksum = net_checksum_pseudo(src, dst, IP_PROTO_UDP,
                                        udp_tx_packet, total);

    result = ip_send_from(src, dst, IP_PROTO_UDP, udp_tx_packet, total);

    udp_tx_busy = 0;

    if(result == 0)
        stat_tx++;

    return result;
}

/* The ordinary case: our own address as the source. Everything the comment
*  above says about the return value holds here too. */
int udp_send(uint32_t dst, uint16_t src_port, uint16_t dst_port,
             const void *payload, uint32_t len)
{
    return udp_send_from(net_ip(), dst, src_port, dst_port, payload, len);
}

/* ------------------------------------------------------------------ */
/* Counters                                                            */
/* ------------------------------------------------------------------ */

/* Datagrams handed to a bound handler. */
uint32_t udp_rx_packets(void)
{
    return stat_rx;
}

/* Datagrams put on the wire, counted when IP accepted them -- a send that
*  is still waiting on ARP has not gone anywhere and is not counted. */
uint32_t udp_tx_packets(void)
{
    return stat_tx;
}

/* Everything discarded on the way in: too short, an impossible length, a
*  checksum that did not verify, or no listener on the port. The last of
*  those is the common one on a live wire and says nothing is wrong. */
uint32_t udp_rx_dropped(void)
{
    return stat_drop;
}
