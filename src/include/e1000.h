/* TomatOS - Intel e1000 gigabit ethernet driver
*  Desc: The one thing anybody outside the driver still needs -- bring the
*        card up, or find out there is none.
*
*  WHY THIS CARD IS HERE AT ALL. The RTL8139 was the only network chip this
*  kernel could drive, and that held right up to the day the kernel was booted
*  in UTM on a Mac: that machine offers an Intel e1000 and no Realtek, so the
*  boot printed "PCI: net: e1000 io 0xC000 irq 10" and then, one line further
*  down, "net: no network card, stack stays down". The card was found, named,
*  and then declared not to exist. This file is the other half of that
*  sentence. It is also what QEMU's "-device e1000" is, so the same driver
*  covers the emulator and the machine that prompted it.
*
*  WHAT IT IS. An 82540EM (vendor 0x8086, device 0x100E) and the 82545EM
*  (0x100F) that shares its register map. Not the whole family: the parts that
*  moved to a different descriptor format or a different interrupt scheme are
*  deliberately not claimed, because a driver that binds to a chip it cannot
*  actually drive takes the network down more thoroughly than one that stands
*  aside -- the card is bound, so nothing else will touch it, and it does not
*  work.
*
*  THREE THINGS ABOUT IT ARE UNLIKE THE RTL8139, and each of them is a way to
*  fault at boot rather than to merely not work:
*
*    - ITS REGISTERS ARE MEMORY MAPPED. 128 KiB in BAR0, put by the firmware
*      wherever it liked, which on QEMU is high above the top of RAM and
*      outside the kernel's direct mapping. There is no port to write; there
*      is an address that has to be MAPPED before it may be dereferenced, and
*      dereferencing it unmapped is a page fault on the boot path with the
*      console already handed over. vmm_map_mmio() exists for exactly this.
*
*    - IT USES DESCRIPTOR RINGS. Not a linear buffer the card fills and not
*      four slots, but two arrays of 16 byte descriptors in memory the card
*      reads and writes itself, with a head the card owns and a tail the
*      driver owns. Every address inside them is PHYSICAL, for the same reason
*      RBSTART was: the card's DMA engine does not go through the MMU.
*
*    - ITS MAC IS NOT SIMPLY THERE. The RTL8139 has six port-readable bytes.
*      This card has receive address registers that the hardware fills from an
*      EEPROM during reset, and an EEPROM that can be read by hand when it
*      did not. The driver reads one, checks it, and falls back to the other,
*      because netdev.h requires a driver that cannot read its own MAC not to
*      register at all.
*/
#ifndef __E1000_H
#define __E1000_H

#include "typedefs.h"

/* Looks for the card in what pci_init() enumerated, brings it up, and offers
*  itself to the netdev layer. Returns 0 when this driver's card became THE
*  card of the machine, and negative in every way that can fail to happen: no
*  e1000 on the bus, a BAR that could not be mapped, a card that would not
*  reset, no memory for the rings, no readable MAC -- and a card that came up
*  perfectly well but arrived after another driver had already registered.
*  Only the middle group is a fault, and the driver says which it was on the
*  way out; the caller does not have to tell them apart.
*
*  Called from network_init() in kernel.c, after pci_init() has enumerated the
*  bus and before net_init() asks whether a card registered. */
extern int e1000_init(void);

#endif
