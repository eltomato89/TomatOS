/* TomatOS - DHCP client
*  Desc: Asks the network for an address instead of being told one.
*
*  Until now net_configure() had to be called by hand from the shell, with
*  numbers the user had to know. That is fine for QEMU's user network, whose
*  addresses are fixed and documented, and useless anywhere else.
*
*  The exchange is four messages and their names are worth keeping straight,
*  because the client is in a different state after each one:
*
*      DISCOVER  ->  broadcast: "is there a server?"
*      OFFER     <-  a server proposes an address
*      REQUEST   ->  broadcast: "I take that one, from that server"
*      ACK       <-  the server confirms and states the lease
*
*  REQUEST is broadcast even though the server is known by then, and that is
*  not an oversight: several servers may have offered, and the broadcast is
*  how the ones that were not chosen learn to release what they reserved.
*
*  The whole conversation happens before the machine has an address, which
*  shapes everything: packets go out from IP_ADDR_ANY to IP_ADDR_BROADCAST,
*  and the replies arrive addressed to a machine that is not yet us. The
*  receive path has to accept those, or the answers are dropped as "not for
*  us" and the client waits forever for something that already arrived.
*
*  Not implemented, deliberately: renewing a lease before it expires, RELEASE
*  on shutdown, and DECLINE when the offered address turns out to be in use.
*  All three matter on a real network and none is needed to obtain an address.
*/
#ifndef __DHCP_H
#define __DHCP_H

#include "typedefs.h"

/* The ports are fixed by the protocol and are not negotiable: a client
*  listens on 68 and talks to 67, and a server that answers to anything else
*  does not exist. */
#define DHCP_PORT_SERVER  67
#define DHCP_PORT_CLIENT  68

/* Where the client is in the exchange. The order is the order they happen in,
*  so a caller can print progress without a lookup table. */
#define DHCP_STATE_IDLE        0   /* never started, or given up             */
#define DHCP_STATE_DISCOVER    1   /* DISCOVER sent, waiting for an OFFER    */
#define DHCP_STATE_REQUEST     2   /* REQUEST sent, waiting for an ACK       */
#define DHCP_STATE_BOUND       3   /* address obtained and configured        */
#define DHCP_STATE_FAILED      4   /* no answer, or the server said NAK      */

/* Starts the exchange: binds the client port and sends a DISCOVER. Returns 0
*  if it is under way, or a negative value if the stack is down. It does NOT
*  block -- the answers arrive from the card's interrupt, so a caller watches
*  dhcp_state() and calls dhcp_poll() while it waits. */
extern int dhcp_start(void);

/* Gives up on an exchange in progress and unbinds the port. Safe at any time.
*  A lease already obtained stays configured -- this abandons the conversation,
*  it does not undo its result. */
extern void dhcp_stop(void);

/* Where the exchange stands, and the same as a word for printing. */
extern int dhcp_state(void);
extern const char *dhcp_state_name(void);

/* Drives the timers: resends a request that was not answered, and gives up
*  once the attempts are used up. Call it regularly while waiting -- nothing
*  else moves the state machine forward, because the receive path only reacts
*  to packets that do arrive and the interesting failure is the one where none
*  does. Returns the current state, so a waiting loop needs only this call. */
extern int dhcp_poll(void);

/* What the lease said. Zero until DHCP_STATE_BOUND.
*
*  The address, netmask and gateway are handed to net_configure() as soon as
*  the ACK arrives, so those are readable through net_ip() and friends like
*  any other configuration. These two are what DHCP knows and the IP layer has
*  no field for. */
extern uint32_t dhcp_server(void);
extern uint32_t dhcp_dns(void);

/* Lease duration in seconds as the server stated it, and when it runs out --
*  milliseconds since boot, comparable against timer_get_ticks(). Nothing acts
*  on the expiry yet; it is reported so that the shell can show an address
*  that is about to become invalid rather than pretending it is permanent. */
extern uint32_t dhcp_lease_seconds(void);
extern uint32_t dhcp_lease_expires(void);

/* Why the last attempt failed, in words, for the shell to show. Empty while
*  nothing has gone wrong. */
extern const char *dhcp_last_error(void);

#endif
