/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: PCI bus enumeration (configuration mechanism #1)
*
*  Every device driver needs to know where its card is before it can say a
*  single word to it: which I/O ports answer, which interrupt line it pulls,
*  whether the card is present at all. On a PC that information lives in the
*  configuration space of the PCI bus, and this file is the only place in the
*  kernel that reads it.
*
*  The mechanism is two ports wide. 0xCF8 takes an address, 0xCFC the data:
*
*      0xCF8 <- 0x80000000 | bus<<16 | slot<<11 | func<<8 | (offset & 0xFC)
*      0xCFC -> the 32 bit register at that offset
*
*  Three details in that formula decide whether this works:
*
*    - Bit 31 is the enable bit. Without it the chipset ignores the access
*      and the data port returns garbage instead of configuration space.
*
*    - The data port is DWORD addressed: the low two bits of the offset are
*      masked off in the address. A 16 or 8 bit register is therefore not
*      read by reading "at" its offset - the containing dword is read and
*      the wanted half or byte is shifted out of it. Getting that shift
*      wrong reads the neighbouring register, which is why vendor id
*      (offset 0x00) and device id (offset 0x02) are the classic pair to
*      test it with: they sit in one dword and must not come out swapped.
*
*    - Writes to a sub-dword register are read-modify-write for the same
*      reason. Writing the command register (16 bit at 0x04) by writing a
*      dword would clear the status register at 0x06 along with it.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <pci.h>

/* --- The two ports of configuration mechanism #1 -------------------------- */
#define PCI_CONFIG_ADDRESS  0x0CF8
#define PCI_CONFIG_DATA     0x0CFC

#define PCI_ENABLE_BIT      0x80000000UL

/* Registers this file needs but no driver does, so they stay local. */
#define PCI_HDR_SECONDARY   0x19  /* header type 1: bus behind the bridge    */

/* Bits and fields of the header type register (offset 0x0E). */
#define PCI_HDR_MULTIFUNC   0x80  /* device answers on more than function 0  */
#define PCI_HDR_LAYOUT      0x7F  /* 0 = normal device, 1 = PCI-to-PCI bridge */
#define PCI_HDR_TYPE_BRIDGE 0x01

/* Class codes we react to during enumeration. */
#define PCI_CLASS_BRIDGE    0x06
#define PCI_SUBCLASS_PCI2PCI 0x04
#define PCI_CLASS_NETWORK   0x02

/* An absent device reads back all ones on the vendor id - the bus is not
*  driven, so the pull-ups win. It is the only reliable "nothing here". */
#define PCI_VENDOR_NONE     0xFFFF

#define PCI_MAX_BUS         256
#define PCI_SLOTS_PER_BUS   32
#define PCI_FUNCS_PER_SLOT  8
#define PCI_BARS            6

/* --- Dword wide port I/O ---------------------------------------------------
*
*  Configuration space is reached with 32 bit accesses only. system.h offers
*  inportb/outportb and declares an inportw that nothing in the kernel ever
*  defines; there is no 32 bit pair at all. Like src/ata.c does for its word
*  wide transfers, the two helpers below are local - a header this file does
*  not own is no place to add to. */

static uint32_t pci_inl(unsigned short port)
{
    uint32_t rv;
    __asm__ __volatile__ ("inl %1, %0" : "=a" (rv) : "dN" (port));
    return rv;
}

static void pci_outl(unsigned short port, uint32_t value)
{
    __asm__ __volatile__ ("outl %1, %0" : : "dN" (port), "a" (value));
}

/* --- The device table ------------------------------------------------------
*
*  Enumeration happens once at boot and the result is a plain array. There is
*  no hotplug on the buses this kernel supports, so nothing invalidates it
*  afterwards. */

static pci_device pci_devices[PCI_MAX_DEVICES];
static int pci_device_count;    /* entries actually stored                   */
static int pci_found_count;     /* functions seen, including those dropped   */
static int pci_bus_count;       /* buses actually walked                     */

/* --- Bus worklist ----------------------------------------------------------
*
*  Scanning strategy: recurse from bus 0 through the PCI-to-PCI bridges
*  rather than brute forcing all 256 buses.
*
*  The brute force walk is 256 * 32 * 8 = 65536 configuration reads, two port
*  accesses each. On real hardware a port access costs on the order of a
*  microsecond, so that is a tenth of a second spent almost entirely on buses
*  that do not exist - and reading an unimplemented bus is not free of risk
*  either, some chipsets are unhappy about it. Following the bridges touches
*  only the buses that are really there: on the i440FX this kernel is tested
*  against that is bus 0 alone, 256 slot probes instead of 65536.
*
*  The walk is breadth first over an explicit queue instead of recursion.
*  Same coverage, but the depth of a bridge chain cannot turn into depth of
*  the kernel stack - a misprogrammed or hostile bridge that names itself as
*  its own secondary bus would otherwise recurse until the stack is gone.
*  The seen bitmap makes that impossible here: every bus is queued at most
*  once, so the queue can never hold more than 256 entries and the walk
*  always terminates. */

static uint8_t pci_bus_queue[PCI_MAX_BUS];
static int pci_bus_queue_len;
static uint32_t pci_bus_seen[PCI_MAX_BUS / 32];

static void pci_bus_queue_add(uint8_t bus)
{
    uint32_t mask;
    int word;

    word = bus >> 5;
    mask = 1UL << (bus & 31);

    if(pci_bus_seen[word] & mask)
        return;                 /* already walked or already queued */

    pci_bus_seen[word] |= mask;
    pci_bus_queue[pci_bus_queue_len] = bus;
    pci_bus_queue_len++;
}

/* --- Configuration space access ------------------------------------------- */

/* Builds the value for 0xCF8. The offset is masked to a dword boundary here
*  and nowhere else, so every caller below may pass a natural register
*  offset. */
static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return PCI_ENABLE_BIT
         | ((uint32_t)bus  << 16)
         | ((uint32_t)(slot & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | ((uint32_t)off & 0xFCUL);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    pci_outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, off));
    return pci_inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t value)
{
    pci_outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, off));
    pci_outl(PCI_CONFIG_DATA, value);
}

/* Reads the dword containing the register and shifts the wanted half down.
*  Offset 0x02 lives in the upper half of the dword at 0x00, so the shift is
*  16 bits for an offset with bit 1 set and 0 otherwise. */
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t dword;
    int shift;

    dword = pci_read32(bus, slot, func, off);
    shift = (off & 0x02) * 8;

    return (uint16_t)((dword >> shift) & 0xFFFFUL);
}

/* Read-modify-write, so that the other half of the dword survives. */
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t value)
{
    uint32_t dword;
    int shift;

    shift = (off & 0x02) * 8;
    dword = pci_read32(bus, slot, func, off);
    dword &= ~(0xFFFFUL << shift);
    dword |= ((uint32_t)value) << shift;

    pci_write32(bus, slot, func, off, dword);
}

/* Same idea one step further down: class code, subclass and header type are
*  single bytes. Local, because no driver has asked for byte access yet. */
static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t dword;
    int shift;

    dword = pci_read32(bus, slot, func, off);
    shift = (off & 0x03) * 8;

    return (uint8_t)((dword >> shift) & 0xFFUL);
}

/* --- Enumeration ---------------------------------------------------------- */

/* Stores one function. Everything a driver commonly needs is copied out of
*  configuration space here so that the drivers themselves do not have to
*  touch the ports at all.
*
*  Beyond PCI_MAX_DEVICES entries nothing is stored any more, but the walk
*  continues: a bridge found late must still be followed, otherwise a full
*  table would silently hide whole buses. pci_found_count keeps counting so
*  that the summary can say how many devices were dropped. */
static void pci_record(uint8_t bus, uint8_t slot, uint8_t func,
                       uint16_t vendor, uint8_t header)
{
    pci_device *d;
    uint16_t class_word;
    int i;
    int bars;

    pci_found_count++;

    if(pci_device_count >= PCI_MAX_DEVICES)
        return;

    d = &pci_devices[pci_device_count];

    d->bus    = bus;
    d->slot   = slot;
    d->func   = func;
    d->vendor = vendor;
    d->device = pci_read16(bus, slot, func, PCI_DEVICE_ID);

    /* Subclass (0x0A) and class (0x0B) are neighbours in one dword half,
    *  so one 16 bit read fetches both. */
    class_word    = pci_read16(bus, slot, func, PCI_SUBCLASS);
    d->subclass   = (uint8_t)(class_word & 0xFF);
    d->class_code = (uint8_t)((class_word >> 8) & 0xFF);

    d->irq = pci_read8(bus, slot, func, PCI_INTERRUPT_LINE);

    /* A normal device has six base address registers, a PCI-to-PCI bridge
    *  only two - the space behind 0x18 means bus numbers and windows there,
    *  not addresses. Reading it as a BAR would hand a driver a number that
    *  looks like an I/O port and is not one. */
    bars = ((header & PCI_HDR_LAYOUT) == PCI_HDR_TYPE_BRIDGE) ? 2 : PCI_BARS;

    for(i = 0; i < PCI_BARS; i++)
    {
        if(i < bars)
            d->bar[i] = pci_read32(bus, slot, func, (uint8_t)(PCI_BAR0 + i * 4));
        else
            d->bar[i] = 0;
    }

    pci_device_count++;
}

/* One function of one slot. Returns nothing; a bridge found here puts its
*  secondary bus on the worklist. */
static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t vendor;
    uint8_t header;
    uint8_t class_code;
    uint8_t subclass;

    vendor = pci_read16(bus, slot, func, PCI_VENDOR_ID);
    if(vendor == PCI_VENDOR_NONE)
        return;

    header = pci_read8(bus, slot, func, PCI_HEADER_TYPE);

    pci_record(bus, slot, func, vendor, header);

    /* Follow the bridge. Class 0x06 subclass 0x04 is PCI-to-PCI; its
    *  secondary bus number at 0x19 is the bus on the far side. */
    class_code = pci_read8(bus, slot, func, PCI_CLASS);
    subclass   = pci_read8(bus, slot, func, PCI_SUBCLASS);

    if(class_code == PCI_CLASS_BRIDGE && subclass == PCI_SUBCLASS_PCI2PCI)
        pci_bus_queue_add(pci_read8(bus, slot, func, PCI_HDR_SECONDARY));
}

/* One slot.
*
*  Function 0 must exist for the slot to be occupied at all, so it is probed
*  first and decides whether the rest is looked at. Bit 7 of its header type
*  says the device is multi-function; only then do functions 1..7 answer.
*  Without that check the second port of a dual port card, or the IDE and USB
*  functions of a PIIX3 south bridge, simply do not exist as far as this
*  kernel is concerned. Probing all eight unconditionally is the other
*  mistake: a single-function device is allowed to decode the function
*  number not at all and would then appear eight times over. */
static void pci_scan_slot(uint8_t bus, uint8_t slot)
{
    uint8_t header;
    uint8_t func;

    if(pci_read16(bus, slot, 0, PCI_VENDOR_ID) == PCI_VENDOR_NONE)
        return;

    pci_scan_function(bus, slot, 0);

    header = pci_read8(bus, slot, 0, PCI_HEADER_TYPE);
    if(!(header & PCI_HDR_MULTIFUNC))
        return;

    for(func = 1; func < PCI_FUNCS_PER_SLOT; func++)
        pci_scan_function(bus, slot, func);
}

static void pci_scan_bus(uint8_t bus)
{
    uint8_t slot;

    for(slot = 0; slot < PCI_SLOTS_PER_BUS; slot++)
        pci_scan_slot(bus, slot);
}

/* --- Summary --------------------------------------------------------------- */

/* Cards this kernel can put a name to. Everything else is reported by its
*  numbers, which is all a driver needs anyway. */
typedef struct
{
    uint16_t vendor;
    uint16_t device;
    const char *name;
} pci_known;

static const pci_known pci_known_net[] =
{
    { 0x10EC, 0x8139, "RTL8139"    },
    { 0x10EC, 0x8029, "RTL8029"    },
    { 0x8086, 0x100E, "e1000"      },
    { 0x8086, 0x1209, "8255x"      },
    { 0x1022, 0x2000, "PCnet"      },
    { 0x1AF4, 0x1000, "virtio-net" },
    { 0x0000, 0x0000, 0            }
};

static const char *pci_net_name(const pci_device *d)
{
    int i;

    for(i = 0; pci_known_net[i].name != 0; i++)
    {
        if(pci_known_net[i].vendor == d->vendor &&
           pci_known_net[i].device == d->device)
            return pci_known_net[i].name;
    }

    return 0;
}

/* The first BAR of a device that describes an I/O region, or 0. */
static uint16_t pci_first_io(const pci_device *d)
{
    uint16_t base;
    int i;

    for(i = 0; i < PCI_BARS; i++)
    {
        base = pci_io_base(d, i);
        if(base != 0)
            return base;
    }

    return 0;
}

/* One line for the totals, at most one more for the network cards. The text
*  screen has 25 rows and the boot messages have to share them. */
static void pci_report(void)
{
    const pci_device *d;
    const char *name;
    uint16_t io;
    int listed;
    int i;

    printf("PCI: %d device(s) on %d bus(es)", pci_device_count, pci_bus_count);
    if(pci_found_count > pci_device_count)
        printf(", %d not recorded", pci_found_count - pci_device_count);
    printf("\n");

    listed = 0;
    for(i = 0; i < pci_device_count; i++)
    {
        d = &pci_devices[i];
        if(d->class_code != PCI_CLASS_NETWORK)
            continue;

        /* Three of them fill the line; anything beyond that is in the table
        *  and a shell command can still show it. */
        if(listed >= 3)
        {
            printf(" ...");
            break;
        }

        printf("%s", (listed == 0) ? "PCI: net: " : ", ");
        listed++;

        name = pci_net_name(d);
        if(name != 0)
            printf("%s", name);
        else
            printf("%X:%X", d->vendor, d->device);

        io = pci_first_io(d);
        if(io != 0)
            printf(" io 0x%X", io);
        printf(" irq %d", d->irq);
    }

    if(listed > 0)
        printf("\n");
}

/* --- Public interface ------------------------------------------------------ */

void pci_init(void)
{
    uint8_t header;
    uint8_t func;
    int i;

    pci_device_count = 0;
    pci_found_count  = 0;
    pci_bus_count    = 0;
    pci_bus_queue_len = 0;

    for(i = 0; i < (int)(PCI_MAX_BUS / 32); i++)
        pci_bus_seen[i] = 0;

    /* Bus 0 always exists - the host bridge sits on it, and it is the only
    *  bus reachable without asking anybody first. */
    pci_bus_queue_add(0);

    /* A multi-function host bridge at 00:00 is the documented way a board
    *  announces additional host buses: function n is the bridge for bus n.
    *  Machines with one host bridge, the i440FX among them, leave bit 7
    *  clear and nothing extra is queued. */
    if(pci_read16(0, 0, 0, PCI_VENDOR_ID) != PCI_VENDOR_NONE)
    {
        header = pci_read8(0, 0, 0, PCI_HEADER_TYPE);
        if(header & PCI_HDR_MULTIFUNC)
        {
            for(func = 1; func < PCI_FUNCS_PER_SLOT; func++)
            {
                if(pci_read16(0, 0, func, PCI_VENDOR_ID) != PCI_VENDOR_NONE)
                    pci_bus_queue_add(func);
            }
        }
    }

    /* The queue grows while it is walked: every bridge appends the bus
    *  behind it. Each bus enters the queue at most once, so this ends. */
    for(i = 0; i < pci_bus_queue_len; i++)
    {
        pci_scan_bus(pci_bus_queue[i]);
        pci_bus_count++;
    }

    pci_report();
}

int pci_count(void)
{
    return pci_device_count;
}

const pci_device *pci_get(int index)
{
    if(index < 0 || index >= pci_device_count)
        return 0;

    return &pci_devices[index];
}

const pci_device *pci_find(uint16_t vendor, uint16_t device)
{
    int i;

    for(i = 0; i < pci_device_count; i++)
    {
        if(pci_devices[i].vendor == vendor && pci_devices[i].device == device)
            return &pci_devices[i];
    }

    return 0;
}

void pci_enable(const pci_device *dev, uint16_t bits)
{
    uint16_t command;

    if(dev == 0)
        return;

    /* Read-modify-write: the command register holds more than the caller
    *  cares about, and the status register shares its dword. */
    command = pci_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    command |= bits;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, command);
}

uint16_t pci_io_base(const pci_device *dev, int bar)
{
    uint32_t value;

    if(dev == 0 || bar < 0 || bar >= PCI_BARS)
        return 0;

    value = dev->bar[bar];

    /* Bit 0 tells the two kinds of BAR apart. In a memory BAR the same bits
    *  mean the type and prefetchability of a memory window, and the rest is
    *  a physical address - handing that back as a port number would have a
    *  driver write into arbitrary I/O ports. */
    if(!(value & 0x01UL))
        return 0;

    /* Bit 1 is reserved, bit 0 is the type bit: the port number starts at
    *  bit 2. x86 has 16 address lines for I/O, so anything above bit 15 in
    *  an I/O BAR is not addressable by an in/out instruction at all and is
    *  reported as "no I/O region" rather than truncated to something that
    *  would silently talk to the wrong port. */
    if(value & ~0xFFFFUL)
        return 0;

    return (uint16_t)(value & 0xFFFCUL);
}
