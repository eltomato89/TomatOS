/* TomatOS - Network stack
*  Desc: Ethernet, ARP, IP and ICMP -- enough to answer and send a ping.
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

/* Byte order helpers. Named as everywhere else so the code reads familiar. */
extern uint16_t htons(uint16_t v);
extern uint16_t ntohs(uint16_t v);
extern uint32_t htonl(uint32_t v);
extern uint32_t ntohl(uint32_t v);

/* The one's complement checksum IP and ICMP both use. Fold carries, and
*  mind that an odd-length buffer pads with a zero byte -- not with the
*  next byte in memory. */
extern uint16_t net_checksum(const void *data, uint32_t len);

/* Brings the stack up on whatever card the driver found. Safe to call with
*  no card present; net_up() then stays false. */
extern void net_init(void);
extern int net_up(void);

/* Our addresses. Static for now -- DHCP is a separate undertaking. */
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

/* ICMP echo. The reply arrives asynchronously, so the caller polls
*  icmp_reply_seq() for the sequence number it sent. */
extern int icmp_send_echo(uint32_t dst, uint16_t id, uint16_t sequence);
extern int icmp_last_reply(uint32_t *from, uint16_t *id, uint16_t *sequence);

/* Counters, for the shell to show what actually moved. */
extern uint32_t net_rx_packets(void);
extern uint32_t net_tx_packets(void);
extern uint32_t net_rx_dropped(void);

#endif
