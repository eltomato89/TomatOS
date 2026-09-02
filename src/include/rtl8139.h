/* TomatOS - Realtek RTL8139 driver
*  Desc: The one thing anybody outside the driver still needs -- bring the
*        card up, or find out there is none.
*
*  Chosen for being simple enough to understand end to end: one receive ring
*  the card writes into continuously, four transmit slots used round robin,
*  and a handful of registers. No descriptor rings, no scatter-gather.
*
*  Both the receive ring and the transmit buffers are handed to the card as
*  PHYSICAL addresses -- it does not go through the MMU. They also have to
*  stay below 4 GiB and be contiguous, which is why they come from the frame
*  allocator rather than the heap.
*
*  WHAT THIS HEADER NO LONGER IS. It used to declare four more functions --
*  rtl8139_present(), rtl8139_mac(), rtl8139_send() and rtl8139_info() -- and
*  net.c and the shell called them by those names. That was honest while an
*  RTL8139 was the only card this kernel could drive and stopped being honest
*  the moment a second driver appeared; netdev.h has the argument in full.
*  Those four are still in rtl8139.c, unchanged in what they do, but they are
*  static now and are reached only as the four members of the netdev_ops this
*  driver registers. Anything above the driver asks netdev.h instead:
*  netdev_present(), netdev_mac(), netdev_send(), netdev_describe().
*
*  So what is left here is the one thing that genuinely cannot go through
*  netdev.h, because it is what makes a netdev exist in the first place.
*/
#ifndef __RTL8139_H
#define __RTL8139_H

#include "typedefs.h"

/* Probes the PCI bus for the card and brings it up, then offers itself to the
*  netdev layer. Returns 0 when this driver's card became THE card of the
*  machine, and negative in all three ways that can fail to happen: no RTL8139
*  on the bus, a card that would not come up, and a card that came up fine but
*  arrived after another driver had already registered. Only the middle one is
*  a fault, and the driver says which it was on the way out -- the caller does
*  not have to tell them apart and nothing in kernel.c does.
*
*  Called from network_init() in kernel.c, after pci_init() has enumerated the
*  bus and before net_init() asks whether a card registered. */
extern int rtl8139_init(void);

#endif
