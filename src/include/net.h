/* TomatOS - Network stack
*  Desc: Ethernet, ARP, IP, ICMP and UDP -- enough to ping, and enough for a
*        DHCP client to ask for the address the rest of it needs.
*
*  Layering, and where the boundary sits: the card driver and this stack are
*  kernel code because they need interrupts, port I/O and memory the card can
*  reach by physical address. The tools that use them -- ping, ifconfig --
*  are ordinary programs' work and only live in the shell until there are
*  system calls for them.
*
*  Byte order is the thing to keep straight throughout. Everything on the
*  wire is big endian; the machine is little endian. Values are converted at
*  the edges, and every field in the packed headers below is stored in
*  NETWORK order -- read it with ntohs/ntohl, never directly.
*/
#ifndef __NET_H
#define __NET_H

#include "typedefs.h"

#define ETH_ALEN          6      /* MAC address length            */
#define ETH_HDR_LEN      14
#define ETH_MTU        1500
#define ETH_FRAME_MAX  1518

#define ETH_TYPE_IP    0x0800
#define ETH_TYPE_ARP   0x0806

#define IP_PROTO_ICMP     1
#define IP_PROTO_TCP      6
#define IP_PROTO_UDP     17

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

#define ARP_CACHE_SIZE   16

#define UDP_HDR_LEN       8

/* The two addresses that are not a host.
*
*  IP_ADDR_ANY is "no address": what a machine puts in the source field before
*  it has one, which is exactly the situation a DHCP client starts in.
*  IP_ADDR_BROADCAST is the limited broadcast, delivered to every station on
*  the wire and routed nowhere. Both are needed before configuration, and
*  neither can be reached by asking ARP -- arp_lookup() answers a broadcast
*  address with the broadcast MAC directly, because nobody replies to a
*  request for it. */
#define IP_ADDR_ANY        0x00000000UL
#define IP_ADDR_BROADCAST  0xFFFFFFFFUL

/* How many ports can be listened on at once. Two is already enough for DHCP
*  plus one more; eight leaves room without costing anything worth counting. */
#define UDP_MAX_BINDINGS  8

typedef struct
{
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed)) eth_header;

typedef struct
{
    uint8_t  version_ihl;     /* 4 bits each: version, header length in words */
    uint8_t  tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed)) ip_header;

typedef struct
{
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed)) icmp_header;

typedef struct
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;          /* header AND payload, unlike IP's total_length */
    uint16_t checksum;        /* 0 means "not computed" -- see below          */
} __attribute__((packed)) udp_header;

/* Byte order helpers. Named as everywhere else so the code reads familiar. */
extern uint16_t htons(uint16_t v);
extern uint16_t ntohs(uint16_t v);
extern uint32_t htonl(uint32_t v);
extern uint32_t ntohl(uint32_t v);

/* The one's complement checksum IP and ICMP both use. Fold carries, and
*  mind that an odd-length buffer pads with a zero byte -- not with the
*  next byte in memory. */
extern uint16_t net_checksum(const void *data, uint32_t len);

/* The checksum UDP and TCP use. Same one's complement sum, but it covers a
*  PSEUDO HEADER that never appears on the wire -- source address, destination
*  address, a zero byte, the protocol and the payload length -- before the real
*  header and data. Summing only the visible bytes produces a packet that looks
*  correct in a hex dump and is discarded by every peer, which is a slow way to
*  find out. The addresses are passed in host order and converted here.
*
*  UDP has one further quirk this returns ready for: a checksum field of zero
*  means "sender did not compute one", so a computed result of zero has to go
*  on the wire as 0xFFFF -- the two are the same value in one's complement. */
extern uint16_t net_checksum_pseudo(uint32_t src, uint32_t dst,
                                    uint8_t protocol,
                                    const void *data, uint32_t len);

/* Brings the stack up on whatever card the driver found. Safe to call with
*  no card present; net_up() then stays false. */
extern void net_init(void);
extern int net_up(void);

/* Our addresses. Set by hand from the shell, or by the DHCP client. */
extern void net_configure(uint32_t ip, uint32_t netmask, uint32_t gateway);
extern uint32_t net_ip(void);
extern uint32_t net_netmask(void);
extern uint32_t net_gateway(void);
extern const uint8_t *net_mac(void);

/* Called by the driver for every frame that arrives, from interrupt
*  context. Keep the work here bounded. */
extern void net_receive(const uint8_t *frame, uint32_t len);

/* Sends one frame, filling in the ethernet header. Returns 0 on success. */
extern int net_send(const uint8_t dst_mac[ETH_ALEN], uint16_t type,
                    const void *payload, uint32_t len);

/* ARP. arp_lookup() returns a cached MAC or 0 and sends a request when it
*  misses -- resolution is not instant, so a caller has to retry. */
extern const uint8_t *arp_lookup(uint32_t ip);
extern void arp_request(uint32_t ip);
extern int arp_cache_entries(void);
extern int arp_cache_get(int index, uint32_t *ip, uint8_t mac[ETH_ALEN]);

/* Sends an IP packet, resolving the next hop through ARP -- the gateway
*  when the destination is outside our subnet. Returns 0 on success, and a
*  negative value when the address is not resolved yet. */
extern int ip_send(uint32_t dst, uint8_t protocol, const void *payload, uint32_t len);

/* The same, with the source address spelled out instead of taken from
*  net_ip(). Exists for exactly one situation and should not be used for
*  others: a DHCP client has no address yet and has to send from IP_ADDR_ANY,
*  which is the one case where the source is legitimately not ours. */
extern int ip_send_from(uint32_t src, uint32_t dst, uint8_t protocol,
                        const void *payload, uint32_t len);

/* ICMP echo. The reply arrives asynchronously, so the caller polls
*  icmp_reply_seq() for the sequence number it sent. */
extern int icmp_send_echo(uint32_t dst, uint16_t id, uint16_t sequence);
extern int icmp_last_reply(uint32_t *from, uint16_t *id, uint16_t *sequence);

/* UDP.
*
*  A handler is called for every datagram that arrives on the port it was
*  bound to -- from INTERRUPT CONTEXT, like everything else in the receive
*  path, so it must be short and must not print, allocate or wait. The buffer
*  it is given belongs to the driver's receive ring and is reused the moment
*  the handler returns; anything worth keeping has to be copied out.
*
*  There are no sockets and no receive queues. A bound port and a callback is
*  the smallest thing that lets two protocols share UDP, and it is what a
*  DHCP client needs. */
typedef void (*udp_handler)(uint32_t src_ip, uint16_t src_port,
                            const uint8_t *data, uint32_t len);

extern int udp_bind(uint16_t port, udp_handler fn);
extern int udp_unbind(uint16_t port);

extern int udp_send(uint32_t dst, uint16_t src_port, uint16_t dst_port,
                    const void *payload, uint32_t len);

/* With the source address spelled out, for a sender that has none yet. */
extern int udp_send_from(uint32_t src, uint32_t dst,
                         uint16_t src_port, uint16_t dst_port,
                         const void *payload, uint32_t len);

/* Called by the IP layer for protocol 17. Both addresses are needed because
*  the checksum covers them. */
extern void udp_receive(uint32_t src_ip, uint32_t dst_ip,
                        const uint8_t *packet, uint32_t len);

/* Counters, for the shell to show what actually moved. */
extern uint32_t net_rx_packets(void);
extern uint32_t net_tx_packets(void);
extern uint32_t net_rx_dropped(void);
extern uint32_t udp_rx_packets(void);
extern uint32_t udp_tx_packets(void);
extern uint32_t udp_rx_dropped(void);

#endif
