/* TomatOS - TCP
*  Desc: A stream between two machines, built out of packets that may be lost,
*        duplicated or reordered.
*
*  Everything in this stack so far has been a message: an ARP request, an echo,
*  a DHCP offer, a DNS answer. Each was one packet, and losing one meant asking
*  again. TCP is different in kind, and the difference is what makes it the
*  largest piece here by some margin: the two ends have to agree on what has
*  been received, keep agreeing while packets go missing, and take the
*  connection down in a way that survives the last message being lost.
*
*  SEQUENCE NUMBERS WRAP, and that shapes every comparison in the
*  implementation. A 32 bit counter passes 4 GB and starts again, so "is this
*  byte newer than that one" cannot be written as a > b. It is written as
*  (int32_t)(a - b) > 0, which is correct as long as the two are less than 2 GB
*  apart -- and they always are, because a window is at most 64 KB. Every
*  comparison in this file must use that form. A plain > is a bug that will not
*  show up for hours of transfer and then corrupts a stream.
*
*  WHAT IS SENT IS NOT WHAT IS ACKNOWLEDGED. Data has to be kept after it goes
*  out, because the only evidence it arrived is an acknowledgement that may
*  never come. A send buffer is therefore not a queue that empties when the
*  card takes the bytes; it empties when the peer admits to having them.
*
*  CLOSING IS NOT ONE EVENT. Each side closes its own direction, so a
*  connection can be half open -- we have said everything and are still
*  listening, which is exactly what an HTTP client does after sending its
*  request. And the side that closes first has to wait afterwards, in TIME_WAIT,
*  because its last acknowledgement may have been lost and the peer may resend
*  a FIN that would otherwise be answered by a machine that has forgotten the
*  connection ever existed.
*
*  Scope, deliberately: active open only -- this connects, it does not listen,
*  which halves the state machine and costs nothing we need. No out of order
*  reassembly: a segment that arrives early is dropped and the peer resends it,
*  which is legal, simple, and slow only on a lossy link. No Nagle, no delayed
*  acknowledgement, no window scaling, no selective acknowledgement, no urgent
*  data, no congestion control beyond a retransmission timer -- a hobby kernel
*  fetching a page over a virtual network is not where any of those earn their
*  complexity.
*/
#ifndef __TCP_H
#define __TCP_H

#include "typedefs.h"

/* Connections open at once. Each one costs its buffers, and those come from
*  the heap when it opens rather than from .bss, so this is a limit on
*  simultaneous use and not a permanent cost. */
#define TCP_MAX_CONNS      4

/* The largest segment we will send or advertise. 1460 = the 1500 byte ethernet
*  MTU less 20 bytes of IP and 20 of TCP header. There is no fragmentation in
*  this stack, so a segment larger than this could not be sent at all -- the
*  number is a consequence, not a preference. The peer states its own in the
*  SYN and we must respect it; it may be smaller and often is. */
#define TCP_MSS         1460

/* Per connection buffers. The send buffer holds what has gone out and is not
*  yet acknowledged, so it bounds how much can be in flight; the receive buffer
*  is what the window advertises. Both are heap allocated on connect. */
#define TCP_SND_BUF     4096
#define TCP_RCV_BUF     8192

/* The states, in the order a client passes through them. The names are the
*  ones in RFC 793 because every description of TCP in the world uses them, and
*  a private vocabulary here would only make this file harder to check against
*  the specification.
*
*  LISTEN and SYN_RECEIVED are absent: this end never accepts a connection. */
#define TCP_CLOSED         0   /* no connection                                */
#define TCP_SYN_SENT       1   /* our SYN is out, waiting for SYN,ACK          */
#define TCP_ESTABLISHED    2   /* both directions open                         */
#define TCP_FIN_WAIT_1     3   /* we closed, our FIN is not acknowledged yet   */
#define TCP_FIN_WAIT_2     4   /* our FIN is acknowledged, peer still sending  */
#define TCP_CLOSING        5   /* both closed at once, our FIN unacknowledged  */
#define TCP_TIME_WAIT      6   /* waiting in case our last ACK was lost        */
#define TCP_CLOSE_WAIT     7   /* peer closed, we may still send              */
#define TCP_LAST_ACK       8   /* we closed after the peer, waiting for its ACK*/

/* Header flags, in the order they sit in the byte. */
#define TCP_FIN         0x01
#define TCP_SYN         0x02
#define TCP_RST         0x04
#define TCP_PSH         0x08
#define TCP_ACK         0x10
#define TCP_URG         0x20

typedef struct
{
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  offset;          /* header length in 32 bit words, high 4 bits   */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_header;

/* Errors. Negative, so a byte count stays distinguishable. */
#define TCP_ENOCONN      (-1)   /* no free connection, or the handle is not one */
#define TCP_ECLOSED      (-2)   /* the connection is gone, or was reset         */
#define TCP_EWOULDBLOCK  (-3)   /* nothing to read yet, or no room to write     */
#define TCP_EFAILED      (-4)   /* the stack is down, or an argument is wrong   */

/* Called once when the stack comes up. Safe with no card present. */
extern void tcp_init(void);

/* Opens a connection. Returns a handle, or a negative error. It does NOT wait:
*  the handshake completes from the card's interrupt, so a caller watches
*  tcp_state() and calls tcp_poll() while it does -- the same shape as the DHCP
*  and DNS clients. */
extern int tcp_connect(uint32_t dst, uint16_t port);

/* Queues data. Returns how many bytes were taken, which can be fewer than
*  offered when the send buffer is full -- a caller has to look at the number
*  and send the rest later. Nothing is taken before the connection is
*  established. */
extern int tcp_send(int handle, const void *data, uint32_t len);

/* Takes delivered data out of the receive buffer. Returns how many bytes were
*  copied, 0 when there is nothing right now, and TCP_ECLOSED once the peer has
*  closed AND everything it sent has been read -- the ordering matters: data
*  that arrived before the FIN is still there to be read afterwards, and a
*  reader that stops at the FIN loses the end of the stream. */
extern int tcp_recv(int handle, void *buf, uint32_t len);

/* Closes our direction. The peer may still send, and tcp_recv() keeps working
*  until it closes too. The handle stays valid until the connection reaches
*  TCP_CLOSED, which can be a while: TIME_WAIT exists so that a lost final
*  acknowledgement can be resent. */
extern int tcp_close(int handle);

/* Tears the connection down with a RST and frees the handle at once. For a
*  caller that has given up; a peer that receives this knows the connection
*  failed rather than ended. */
extern void tcp_abort(int handle);

/* Drives every connection's timers: retransmission, the handshake timeout,
*  TIME_WAIT. Call it regularly while anything is open -- the receive path only
*  reacts to segments that do arrive, and the interesting failure is the one
*  where none does. */
extern void tcp_poll(void);

extern int tcp_state(int handle);
extern const char *tcp_state_name(int state);

/* Why the last call failed, in words, for a caller that wants to say so. */
extern const char *tcp_last_error(void);

/* For a shell that wants to show what is open: how many connections exist and
*  what each one is. Fills in nothing it is passed a null pointer for. */
extern int tcp_conn_count(void);
extern int tcp_conn_get(int index, int *handle, uint32_t *peer, uint16_t *peer_port,
                        uint16_t *local_port, int *state,
                        uint32_t *sent, uint32_t *received);

/* Counters, for the shell to show what actually moved. */
extern uint32_t tcp_segments_sent(void);
extern uint32_t tcp_segments_received(void);
extern uint32_t tcp_segments_dropped(void);
extern uint32_t tcp_retransmits(void);

/* Called by the IP layer for protocol 6. Both addresses, because the checksum
*  covers them -- the same pseudo header UDP uses. */
extern void tcp_receive(uint32_t src_ip, uint32_t dst_ip,
                        const uint8_t *segment, uint32_t len);

#endif
