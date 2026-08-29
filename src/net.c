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
*    - Interrupt context. net_receive() is called by the card driver from
*      inside its interrupt handler. Everything it reaches -- ARP handling,
*      the reply it sends, ip_receive() -- therefore runs with the rest of
*      the kernel stopped mid-instruction. That is why nothing down that
*      path prints, allocates, waits for the timer or spins on the card:
*      the work is bounded by a fixed 16 entry cache walk and one frame
*      copy. See the note above net_receive() for what a queue would buy
*      us instead.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <net.h>
#include <rtl8139.h>

/* --- The boundary to ip.c -------------------------------------------------
*
*  Handing a received IP packet upward. net.h does not declare this because
*  it is internal to the stack -- nothing outside net.c and ip.c may call it.
*  The pointer is to the first byte after the ethernet header, len is what is
*  left of the frame from there on, and ip.c must not hold on to the buffer:
*  it belongs to the driver's receive ring and is reused as soon as we
*  return.
*
*  Called from interrupt context, with the same restrictions that apply here.
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

static uint32_t stat_rx     = 0;
static uint32_t stat_tx     = 0;
static uint32_t stat_drop   = 0;

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
    stat_drop = 0;
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
    return stat_drop;
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
*  hear nothing, and never send the packet it wanted to. */
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

/* Everything that arrives with ethertype 0x0806. Interrupt context. */
static void arp_receive(const uint8_t *data, uint32_t len)
{
    const arp_packet *pkt;
    uint32_t sender_ip;
    uint32_t target_ip;
    uint16_t opcode;
    int for_us;

    if(len < ARP_PKT_LEN)
    {
        stat_drop++;
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
        stat_drop++;
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

/* --- Receiving -----------------------------------------------------------
*
*  The driver calls this from its interrupt handler, once per frame.
*
*  Doing the protocol work here, rather than queueing the frame for a
*  softirq or a kernel thread, is a deliberate simplification and it holds
*  only because every path from here is short and finite: a length check, a
*  16 entry cache walk, at most one frame copied into the card's transmit
*  slot. Nothing prints, nothing allocates, nothing waits on the timer, and
*  the frame is never handed to code that could sleep.
*
*  What we pay for it is that a burst of frames is processed with interrupts
*  effectively serialised behind the card, and that a bug anywhere above --
*  in ARP or in ip.c -- is a bug in interrupt context, where a spin is a
*  hung machine rather than a hung process. A receive queue would be the
*  right next step: net_receive() would then do the addressing check, copy
*  the frame into a ring and return, and a kernel task would drain it with
*  interrupts on, which is also what makes it safe to eventually print,
*  allocate or block up here. It is not built because a queue needs a task
*  to drain it and a policy for what to do when it overflows, and neither
*  belongs in the same change as the first packet that works.
*/
void net_receive(const uint8_t *frame, uint32_t len)
{
    const eth_header *eth;
    uint16_t type;

    if(frame == 0 || !net_is_up)
    {
        stat_drop++;
        return;
    }

    /* Long enough to contain a header before anything reads one. */
    if(len < ETH_HDR_LEN || len > ETH_FRAME_MAX)
    {
        stat_drop++;
        return;
    }

    eth = (const eth_header *)frame;

    /* Addressed to us, or broadcast. The card can be told to hand up
    *  everything it hears, so this check is what keeps the rest of the
    *  stack from answering another machine's traffic. */
    if(!mac_equal(eth->dst, net_hwaddr) && !mac_equal(eth->dst, mac_broadcast))
    {
        stat_drop++;
        return;
    }

    /* A frame that claims to come FROM us is either our own transmission
    *  echoed back or somebody spoofing; either way there is nothing to do
    *  with it. */
    if(mac_equal(eth->src, net_hwaddr))
    {
        stat_drop++;
        return;
    }

    stat_rx++;
    type = ntohs(eth->type);

    switch(type)
    {
        case ETH_TYPE_ARP:
            arp_receive(frame + ETH_HDR_LEN, len - ETH_HDR_LEN);
            break;

        case ETH_TYPE_IP:
            ip_receive(frame + ETH_HDR_LEN, len - ETH_HDR_LEN);
            break;

        default:
            /* IPv6, VLAN tags, whatever else the segment carries. Counted,
            *  not printed -- this is the common case on a real network and
            *  a printf here would bury the machine in interrupt context. */
            stat_drop++;
            break;
    }
}
