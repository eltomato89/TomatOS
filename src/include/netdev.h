/* TomatOS - network cards
*  Desc: One way to send and receive an Ethernet frame, whatever card is under
*        it.
*
*  WHY THIS EXISTS. net.c called rtl8139_send() and rtl8139_mac() by name,
*  which was honest while an RTL8139 was the only card this kernel could
*  drive. It stopped being honest the moment a second one appeared: a machine
*  emulated by UTM offers an Intel e1000 and no Realtek at all, and the boot
*  line "net: no network card, stack stays down" was printed one line under
*  "PCI: net: e1000 io 0xC000 irq 10" -- the kernel had found the card, named
*  it, and then declared there was none, because the only name it knew how to
*  say was the other one.
*
*  So this is the join, and it is deliberately the same shape as blockdev.h
*  one subsystem over, for the same reason stated there: the thing above
*  should survive the thing below being replaced. A driver registers what it
*  can do; the stack sends a frame and never learns which silicon carried it.
*
*  ONE CARD AT A TIME, and that is a decision rather than a limitation nobody
*  got round to lifting. The stack has one IP address, one ARP cache, one
*  default gateway and one DHCP lease; a second card would need a routing
*  decision for every outgoing frame and an answer to which of two leases
*  wins, and neither question has a good answer in an OS with no routing
*  table. The first driver to register wins and the rest report themselves and
*  stand down -- "lspci" still lists them, so a machine with two cards says so
*  rather than pretending the second is not there.
*
*  RECEIVING IS NOT IN THIS INTERFACE. A card pushes frames up by calling
*  net_receive() from its interrupt handler, exactly as the RTL8139 always
*  did. There is nothing for the stack to poll and therefore nothing to
*  abstract: the direction that needs a function pointer is the one where the
*  caller chooses when it happens, and that is only true of sending.
*/
#ifndef __NETDEV_H
#define __NETDEV_H

#include "typedefs.h"

#define NETDEV_MAC_LEN  6

/* What a driver provides.
*
*  None of these may be null, and none of them is called before the driver
*  has registered -- a driver registers when it is up, not when it is found,
*  so there is no "not ready yet" state for anything here to describe. */
typedef struct
{
    /* Short, for the boot line and for "ifconfig": "RTL8139", "Intel e1000".
    *  It names the CHIP rather than the vendor, because that is what a person
    *  looking for the driver in this tree will search for. */
    const char *name;

    /* Queues one frame for transmission. Returns 0 on success and negative
    *  when the card refused it -- a full transmit ring, a length the hardware
    *  cannot carry, or a card that has stopped answering.
    *
    *  The frame is a complete Ethernet frame starting at the destination MAC.
    *  The driver adds the CRC (every card this kernel is likely to meet does
    *  it in hardware) and pads to the 60 byte minimum; a caller that had to
    *  know which of those two the card does for itself would be a caller that
    *  knows which card it is talking to.
    *
    *  Callable from task context with interrupts on. NOT from an interrupt
    *  handler: a driver may spin waiting for a descriptor here. */
    int (*send)(const void *frame, uint32_t len);

    /* The card's own address, NETDEV_MAC_LEN bytes, read out of its hardware
    *  rather than invented. Never null and never all zeroes: a driver that
    *  could not read its own MAC has no business registering, because every
    *  frame it sent would be unanswerable. */
    const uint8_t *(*mac)(void);

    /* One line for the shell -- I/O or memory base, IRQ, MAC, whatever the
    *  driver thinks is worth knowing when the network does not work. Never
    *  null. */
    const char *(*describe)(void);
} netdev_ops;

/* Takes a driver into use. Returns 0 when it became THE card, and negative
*  when one was already registered -- which is not an error the caller should
*  report as a failure, since its card is fine and simply not the one in use.
*  See the note about one card at a time above. */
extern int netdev_register(const netdev_ops *ops);

/* Is there a card at all? Everything above this line is written so that the
*  answer being no is an ordinary state rather than a fault: a machine with no
*  network is a machine this kernel runs on perfectly well. */
extern int netdev_present(void);

/* The three things the stack and the shell actually ask for. Each is safe to
*  call with no card registered and then answers for a machine that has none:
*  send fails, mac is a pointer to six zero bytes, and the two strings say so
*  in words rather than being null. */
extern int             netdev_send(const void *frame, uint32_t len);
extern const uint8_t  *netdev_mac(void);
extern const char     *netdev_name(void);
extern const char     *netdev_describe(void);

/* How many drivers offered themselves, registered or not. The boot line uses
*  it to say "a second card was found and is not in use" rather than leaving
*  somebody to wonder why their other adapter is quiet. */
extern int netdev_offered(void);

#endif
