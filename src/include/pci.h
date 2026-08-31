/* TomatOS - PCI bus enumeration
*  Desc: Finding devices, and what is needed to talk to them.
*
*  Configuration space is reached through the two ports 0xCF8 (address) and
*  0xCFC (data): write bus/device/function/register into the first, read or
*  write the second. That is all the mechanism there is on a PC.
*
*  This is infrastructure, not a network thing -- every future device driver
*  needs it to find its card, its I/O base and its interrupt line.
*/
#ifndef __PCI_H
#define __PCI_H

#include "typedefs.h"

#define PCI_MAX_DEVICES  32   /* how many we are willing to remember */

/* Config space registers we care about */
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_CLASS        0x0B
#define PCI_SUBCLASS     0x0A
#define PCI_PROG_IF      0x09
#define PCI_HEADER_TYPE  0x0E
#define PCI_BAR0         0x10
#define PCI_INTERRUPT_LINE 0x3C

/* Bits in PCI_COMMAND */
#define PCI_CMD_IO       0x0001   /* respond to I/O space accesses  */
#define PCI_CMD_MEMORY   0x0002   /* respond to memory accesses     */
#define PCI_CMD_MASTER   0x0004   /* allowed to act as a bus master */

typedef struct
{
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    /* The third level of the class triple, and it is not optional for every
    *  device: class 0x0C subclass 0x03 is "USB host controller" and says
    *  nothing about which KIND -- UHCI is 0x00, OHCI 0x10, EHCI 0x20 and xHCI
    *  0x30, and their register interfaces have nothing in common. A driver
    *  that matched on class and subclass alone would happily bind to a
    *  controller it cannot speak to. */
    uint8_t  prog_if;
    uint8_t  irq;
    uint32_t bar[6];        /* raw base address registers */
} pci_device;

/* Walks the buses and records what it finds. */
extern void pci_init(void);

extern int pci_count(void);
extern const pci_device *pci_get(int index);

/* First device matching vendor and device id, or 0. */
extern const pci_device *pci_find(uint16_t vendor, uint16_t device);

/* Config space access, for a driver that needs more than the record holds. */
extern uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
extern uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
extern void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t value);
extern void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t value);

/* Sets bits in the command register -- a card typically needs I/O space and
*  bus mastering enabled before it will do anything. */
extern void pci_enable(const pci_device *dev, uint16_t bits);

/* An I/O base address register has its low bit set and the port number in
*  the upper bits; a memory one is laid out differently. Returns 0 if that
*  BAR is not an I/O region. */
extern uint16_t pci_io_base(const pci_device *dev, int bar);

#endif
