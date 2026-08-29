/* TomatOS - DNS resolver
*  Desc: Turns a name into an address, over UDP.
*
*  The last piece that makes the address the DHCP client obtained useful for
*  something a human typed: the lease already names a DNS server, and this asks
*  it. Query out, answer back, one datagram each in the ordinary case.
*
*  Three things in the protocol are worth knowing before reading the code,
*  because each is a place where a straightforward implementation is wrong.
*
*  A NAME ON THE WIRE IS NOT A STRING. It is a sequence of length-prefixed
*  labels ending in a zero byte: "www.example.com" travels as 3 w w w 7 e x a
*  m p l e 3 c o m 0. Encoding is mechanical. Decoding is not, because of the
*  next point.
*
*  NAMES ARE COMPRESSED, and this is the dangerous part. A label whose top two
*  bits are set is not a label but a POINTER to an offset earlier in the same
*  message, so a name can be assembled from pieces scattered through it. That
*  is what keeps a reply with several answers small. It also means a decoder
*  follows offsets that came off the wire -- and a pointer that points at
*  itself, or at another pointer that points back, is an infinite loop inside
*  an interrupt handler. Every decoder must bound the number of jumps it will
*  make, and must refuse a pointer that does not go strictly backwards.
*
*  A REPLY THAT DID NOT FIT SETS THE TC BIT, and the protocol's answer to that
*  is to ask again over TCP. There is no TCP here, so a truncated reply is
*  reported as such rather than silently used -- what arrived is the beginning
*  of an answer, not a short answer.
*
*  Deliberately absent: TCP fallback, anything but A records (no AAAA, no MX,
*  no SRV -- there is no IPv6 and nothing that would use the others), reverse
*  lookups, and any attempt at resisting an attacker who can see the query.
*  Off-path spoofing is made harder by matching the id, the source port and
*  the question section, which is what a resolver can do without cryptography;
*  DNSSEC is a different undertaking entirely.
*/
#ifndef __DNS_H
#define __DNS_H

#include "typedefs.h"

#define DNS_PORT_SERVER   53

/* Wire limits, both from the protocol rather than chosen here: a label is at
*  most 63 bytes (the two high bits of the length byte are taken by the
*  compression marker, leaving six), and a whole name at most 255. */
#define DNS_LABEL_MAX     63
#define DNS_NAME_MAX     255

/* How many answers are remembered. Small on purpose: this is a convenience so
*  that "ping example.com" twice does not ask twice, not a serving cache. */
#define DNS_CACHE_SIZE     8

/* Where a lookup stands. Ordered, like the DHCP states, so a caller can print
*  progress without a lookup table. */
#define DNS_STATE_IDLE     0   /* nothing asked, or the last one is collected */
#define DNS_STATE_QUERY    1   /* query sent, waiting for an answer           */
#define DNS_STATE_DONE     2   /* an address was found -- dns_result() has it */
#define DNS_STATE_FAILED   3   /* no answer, or the server said there is none */

/* Binds the client port. Called once when the stack comes up; safe to call
*  again. Returns 0, or negative if there is no card. */
extern int dns_init(void);

/* Starts a lookup. Returns 0 if a query went out, a negative value if the name
*  is unusable, no server is known, or one is already in flight -- there is one
*  outstanding query at a time, which is all a shell needs and saves a table of
*  pending requests that would each need their own timeout.
*
*  It does NOT block: the answer arrives from the card's interrupt, so a caller
*  watches dns_state() and calls dns_poll() while it waits. A name already in
*  the cache still goes through the same states, so a caller has one path
*  rather than two -- check dns_lookup_cached() first if that matters. */
extern int dns_resolve(const char *name);

/* Drives the retransmission timer and gives up when the attempts are used up.
*  Call it regularly while waiting; nothing else moves a lookup forward, since
*  the interesting failure is the one where no answer arrives at all. Returns
*  the current state. */
extern int dns_poll(void);

/* Abandons a lookup in progress. The cache keeps whatever it already held. */
extern void dns_cancel(void);

extern int dns_state(void);

/* The address that was found, in host order, or 0. Valid at DNS_STATE_DONE. */
extern uint32_t dns_result(void);

/* How long the answer may be kept, as the server stated it. Reported rather
*  than merely obeyed, because a very short TTL is worth seeing when a name
*  keeps resolving differently. */
extern uint32_t dns_result_ttl(void);

/* Why the last lookup failed, in words, for the shell to show. Empty while
*  nothing has gone wrong. */
extern const char *dns_last_error(void);

/* The cache, without asking anyone: the address for a name that is still
*  within its TTL, or 0. */
extern uint32_t dns_lookup_cached(const char *name);

/* Walking the cache, for a shell that wants to show it. dns_cache_get() fills
*  in the name (truncated to size), the address and how many seconds of its TTL
*  are left, and returns 0 on success. */
extern int dns_cache_entries(void);
extern int dns_cache_get(int index, char *name, uint32_t size,
                         uint32_t *ip, uint32_t *ttl_left);
extern void dns_cache_flush(void);

/* Counters, for the shell to show what actually moved. */
extern uint32_t dns_queries_sent(void);
extern uint32_t dns_replies_used(void);
extern uint32_t dns_replies_dropped(void);

#endif
