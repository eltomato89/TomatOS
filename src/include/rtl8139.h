/* TomatOS - Realtek RTL8139 driver
*  Desc: The network card, found on the PCI bus.
*
*  Chosen for being simple enough to understand end to end: one receive ring
*  the card writes into continuously, four transmit slots used round robin,
*  and a handful of registers. No descriptor rings, no scatter-gather.
*
*  Both the receive ring and the transmit buffers are handed to the card as
*  PHYSICAL addresses -- it does not go through the MMU. They also have to
*  stay below 4 GiB and be contiguous, which is why they come from the frame
*  allocator rather than the heap.
*/
#ifndef __RTL8139_H
#define __RTL8139_H

#include "typedefs.h"
#include "net.h"

/* Probes the PCI bus for the card and brings it up. Returns 0 on success. */
extern int rtl8139_init(void);

extern int rtl8139_present(void);

/* The card's MAC address, read out of its registers. */
extern const uint8_t *rtl8139_mac(void);

/* Queues one frame for transmission. Returns 0 on success, negative when
*  all four transmit slots are still busy. */
extern int rtl8139_send(const void *frame, uint32_t len);

/* Description for the shell: I/O base, IRQ, MAC. */
extern const char *rtl8139_info(void);

#endif
