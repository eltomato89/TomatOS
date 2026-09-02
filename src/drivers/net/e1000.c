/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Intel 82540EM / 82545EM gigabit ethernet driver ("e1000")
*
*  The card is found in the table pci_init() already built (vendor 0x8086,
*  device 0x100E or 0x100F) and driven entirely through the 128 KiB register
*  block in BAR0. Two rings of 16 byte descriptors carry the frames: the card
*  owns the head of each ring, this driver owns the tail, and every address
*  written into a descriptor or into a ring base register is PHYSICAL.
*
*  Five things decide whether this driver works at all, and four of them are
*  silent when they are wrong:
*
*    - THE REGISTER BLOCK HAS TO BE MAPPED. This is the loud one. BAR0 is a
*      MEMORY base address register, not an I/O one, and pci_io_base()
*      deliberately refuses it -- handing a physical address back as a port
*      number would have a driver write into arbitrary I/O ports, so it
*      returns 0 instead and this driver reads the raw BAR itself. What comes
*      out is a physical address the firmware chose, which on QEMU is at
*      0xFEBC0000, far above the top of RAM and therefore outside everything
*      the direct mapping covers. P2V() says nothing about it and nothing maps
*      it on its own. Dereferencing it as it stands is a page fault on the
*      boot path, after the console has been handed to the framebuffer, which
*      is about the least informative moment available. vmm_map_mmio() is what
*      turns it into a pointer, and it is called before the first register
*      access rather than after.
*
*    - BUS MASTERING, exactly as for the RTL8139 and for the same reason. The
*      card fetches its own descriptors and moves frame data by itself.
*      Without PCI_CMD_MASTER every register still answers, the link still
*      comes up, no error bit is set anywhere -- and not one descriptor is
*      ever fetched, so nothing is received and nothing is sent.
*
*    - PHYSICAL ADDRESSES, again. RDBAL, TDBAL and the address field of every
*      descriptor are read by the card's DMA engine, which does not go through
*      the MMU. All of it comes from pmm_alloc_frames(), which returns
*      physical addresses and hands out contiguous blocks (the heap promises
*      neither), and every kernel access to the same bytes goes through P2V().
*
*    - THE TAIL POINTER IS THE HANDOVER. A descriptor becomes the card's the
*      moment the tail register is written past it, and not before. A driver
*      that fills descriptors and forgets the tail write has a card that sits
*      there with an empty ring; a driver that writes the tail before the
*      descriptor is complete hands over a half-written descriptor. The tail
*      write is therefore always the last store of a sequence.
*
*    - HEAD EQUALS TAIL MEANS EMPTY, on both rings, which is why the transmit
*      side may never have more than one descriptor short of the whole ring in
*      flight: filling the last free slot would make the tail catch the head
*      up from behind, and the card would read that as "nothing to send" and
*      stop -- with a full ring of work in front of it that it will now never
*      look at, and no interrupt coming to unstick it.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <mm.h>
#include <vmm.h>
#include <pci.h>
#include <net.h>
#include <netdev.h>
#include <e1000.h>

/* --- Identity --------------------------------------------------------------
*
*  Two device ids, one driver, and the second one is free rather than hopeful.
*  The 82545EM is the same MAC core as the 82540EM with a different physical
*  layer bolted on: identical register offsets, identical legacy descriptor
*  layout, identical reset sequence, and QEMU emulates both with the same
*  device model. Nothing below this line asks which of the two it is talking
*  to, because there is nothing it would do differently -- and that was
*  checked rather than assumed: "-device e1000-82545em" boots, reads its MAC,
*  takes a DHCP lease and answers a ping through this file unchanged.
*
*  WHAT IS NOT CLAIMED, AND WHY THE LIST STOPS HERE. The 82574L (0x10D3) is
*  also emulated by QEMU, as "-device e1000e", and it was tried: with its id
*  added it comes up, binds an address and fetches a page over TCP, on the
*  first attempt and with nothing else changed. It is still not in the list,
*  and the reason is one concrete difference rather than caution in general.
*
*  On the 82574 the EEPROM read register is not laid out as it is here: the
*  word address goes at bit 2 and the done flag is bit 1, where these parts
*  put the address at bit 8 and done at bit 4. The constants at E1000_EERD
*  below would therefore read a DIFFERENT WORD on real 82574 silicon and
*  report success -- so the MAC fallback path would hand back six plausible
*  bytes that are not the card's address. QEMU's model does not expose that,
*  because the fallback never runs there: the receive address registers are
*  loaded and valid, so the EEPROM is never asked.
*
*  That is the shape of thing this driver should not guess at. One difference
*  is visible from a datasheet and matters; the ones that are not visible from
*  here -- the 82574 defaults to MSI-X, has a PBA to size and a CTRL_EXT this
*  file never writes -- cannot be checked without the card. A driver that
*  binds hardware it turns out not to drive is worse than one that stands
*  aside, because the card is then claimed and nothing else will come for it.
*  Adding the id is one line plus an EERD layout chosen by device id, on the
*  day somebody can boot a real 82574L and watch a ping come back. */
#define E1000_VENDOR_ID     0x8086
#define E1000_DEV_82540EM   0x100E
#define E1000_DEV_82545EM   0x100F

/* How much of the address space BAR0 decodes. Fixed at 128 KiB on these
*  parts; the registers this driver touches all live in the first 32 KiB, but
*  the whole window is mapped because that is what the card decodes and a
*  partial mapping would only turn a future register access into a fault
*  instead of a read. Thirty-two pages out of the 4096 the MMIO window has. */
#define E1000_BAR_SIZE      0x20000UL

/* --- Register offsets, in bytes from the start of the block ----------------
*
*  Every one of them is a 32 bit register and is accessed as a dword. Unlike
*  the RTL8139's I/O window there is no width to get wrong here -- a byte
*  access to a memory mapped register on this card is not defined behaviour,
*  so the accessors below only do dwords and there is no narrow variant to
*  reach for by accident. */
#define E1000_CTRL          0x0000   /* device control                        */
#define E1000_STATUS        0x0008   /* device status, read only              */
#define E1000_EERD          0x0014   /* EEPROM read, request and result       */
#define E1000_ICR           0x00C0   /* interrupt cause, READING CLEARS IT    */
#define E1000_ITR           0x00C4   /* interrupt throttling                  */
#define E1000_IMS           0x00D0   /* interrupt mask set: 1 bits enable     */
#define E1000_IMC           0x00D8   /* interrupt mask clear: 1 bits disable  */
#define E1000_RCTL          0x0100   /* receive control                       */
#define E1000_TCTL          0x0400   /* transmit control                      */
#define E1000_TIPG          0x0410   /* transmit inter packet gap             */
#define E1000_FCAL          0x0028   /* flow control address low              */
#define E1000_FCAH          0x002C   /* flow control address high             */
#define E1000_FCT           0x0030   /* flow control type                     */
#define E1000_FCTTV         0x0170   /* flow control transmit timer           */
#define E1000_RDBAL         0x2800   /* receive ring base, low 32 bits, PHYS  */
#define E1000_RDBAH         0x2804   /* receive ring base, high 32 bits       */
#define E1000_RDLEN         0x2808   /* receive ring length in BYTES          */
#define E1000_RDH           0x2810   /* receive head -- the card's pointer    */
#define E1000_RDT           0x2818   /* receive tail -- ours                  */
#define E1000_RDTR          0x2820   /* receive delay timer                   */
#define E1000_RADV          0x282C   /* receive absolute delay timer          */
#define E1000_TDBAL         0x3800   /* transmit ring base, low 32 bits, PHYS */
#define E1000_TDBAH         0x3804
#define E1000_TDLEN         0x3808
#define E1000_TDH           0x3810
#define E1000_TDT           0x3818   /* transmit tail -- writing it starts it */
#define E1000_MTA           0x5200   /* multicast table array, 128 dwords     */
#define E1000_RAL0          0x5400   /* receive address 0, low four bytes     */
#define E1000_RAH0          0x5404   /* receive address 0, high two + flags   */

#define E1000_MTA_ENTRIES   128

/* --- Device control (0x0000) ----------------------------------------------- */
#define E1000_CTRL_FD       0x00000001UL /* full duplex                       */
#define E1000_CTRL_ASDE     0x00000020UL /* auto speed detection enable       */
#define E1000_CTRL_SLU      0x00000040UL /* SET LINK UP -- see the bring-up   */
#define E1000_CTRL_ILOS     0x00000080UL /* invert loss of signal; must be 0  */
#define E1000_CTRL_RST      0x04000000UL /* software reset, self clearing     */
#define E1000_CTRL_VME      0x40000000UL /* VLAN tag stripping; must be 0     */
#define E1000_CTRL_PHY_RST  0x80000000UL /* holds the PHY in reset; must be 0 */

/* --- Device status (0x0008) ------------------------------------------------ */
#define E1000_STATUS_LU     0x00000002UL /* link up                           */

/* --- EEPROM read (0x0014) --------------------------------------------------
*
*  And this is the one register whose layout differs across the family, which
*  is the concrete reason the identity list above is short. On the 82540 and
*  82545 the word address goes at bit 8 and the "done" flag is bit 4; on the
*  82541 and 82547 the address goes at bit 2 and done is bit 1. Using the
*  wrong pair does not fail visibly -- it reads a different word and reports
*  success -- so the constants below are correct for the two ids claimed at
*  the top of this file and for nothing else. */
#define E1000_EERD_START    0x00000001UL /* write with the address to begin   */
#define E1000_EERD_DONE     0x00000010UL /* set by the card when data is valid*/
#define E1000_EERD_ADDR_SH  8            /* word address goes here            */
#define E1000_EERD_DATA_SH  16           /* the 16 bit word comes back here   */

#define E1000_EEPROM_MAC0   0x00         /* words 0..2 hold the MAC address   */

/* --- Receive address high (0x5404) ----------------------------------------- */
#define E1000_RAH_AV        0x80000000UL /* address valid                     */

/* --- Interrupt bits, identical in ICR, IMS and IMC -------------------------
*
*  Only the ones this driver acts on are named. Everything else is left masked
*  rather than enabled and ignored: an enabled cause that no branch handles is
*  a line that keeps being asserted, and on a level triggered PCI interrupt
*  that is an interrupt storm rather than a missed event. */
#define E1000_INT_TXDW      0x00000001UL /* a transmit descriptor is done     */
#define E1000_INT_LSC       0x00000004UL /* link status changed               */
#define E1000_INT_RXDMT0    0x00000010UL /* receive ring is running low       */
#define E1000_INT_RXO       0x00000040UL /* receive overrun -- ring was full  */
#define E1000_INT_RXT0      0x00000080UL /* a frame arrived (receive timer)   */

#define E1000_INT_MASK      (E1000_INT_RXT0 | E1000_INT_RXDMT0 | \
                             E1000_INT_RXO  | E1000_INT_LSC)

/* --- Receive control (0x0100) ---------------------------------------------- */
#define E1000_RCTL_EN       0x00000002UL /* receiver enable                   */
#define E1000_RCTL_SBP      0x00000004UL /* store bad packets                 */
#define E1000_RCTL_UPE      0x00000008UL /* unicast promiscuous               */
#define E1000_RCTL_MPE      0x00000010UL /* multicast promiscuous             */
#define E1000_RCTL_LPE      0x00000020UL /* long packet enable (jumbo)        */
#define E1000_RCTL_LBM_NONE 0x00000000UL /* no loopback                       */
#define E1000_RCTL_RDMTS_H2 0x00000000UL /* RXDMT0 at half the ring           */
#define E1000_RCTL_MO_0     0x00000000UL /* multicast filter offset 47:36     */
#define E1000_RCTL_BAM      0x00008000UL /* accept broadcast (ARP needs it)   */
#define E1000_RCTL_SZ_2048  0x00000000UL /* buffer size, with BSEX clear      */
#define E1000_RCTL_BSEX     0x02000000UL /* multiply the size by 16           */
#define E1000_RCTL_SECRC    0x04000000UL /* strip the ethernet CRC            */

/* --- Transmit control (0x0400) ---------------------------------------------
*
*  CT is the collision threshold and COLD the collision distance, both in the
*  units the standard gives them: 16 attempts, and 64 byte times of back off
*  for a full duplex link. They are only consulted on a half duplex segment,
*  which nothing this kernel is likely to meet still is -- but the reset value
*  of both fields is zero, and a zero collision threshold on a link that ever
*  did go half duplex means the transmitter gives up on the first collision. */
#define E1000_TCTL_EN       0x00000002UL /* transmitter enable                */
#define E1000_TCTL_PSP      0x00000008UL /* pad short packets to 60 bytes     */
#define E1000_TCTL_CT_16    0x00000100UL /* collision threshold: 16           */
#define E1000_TCTL_COLD_FD  0x00040000UL /* collision distance: 64 byte times */
#define E1000_TCTL_RTLC     0x01000000UL /* retransmit on late collision      */

/* Inter packet gap: IPGT 10, IPGR1 8 at bit 10, IPGR2 6 at bit 20. These are
*  the values the manual gives for a copper link and they are what the reset
*  default is NOT -- the register comes up as zero, and a zero gap is a
*  transmitter that does not respect the minimum spacing between frames. */
#define E1000_TIPG_DEFAULT  (10UL | (8UL << 10) | (6UL << 20))

/* --- Descriptors -----------------------------------------------------------
*
*  The legacy 16 byte layout, which is the only one the receive side of these
*  parts has and the simplest of the three the transmit side offers. Both are
*  read and written by the card while the kernel is looking at the same bytes,
*  so every pointer to one is volatile: without that the compiler is entitled
*  to hoist the status byte out of the drain loop and read it exactly once,
*  which is a driver that receives one frame and then waits forever.
*
*  The address field is 64 bits wide on a card that can address more memory
*  than this kernel can. It is written as two 32 bit halves rather than as one
*  64 bit field: the high half is always zero here, saying so explicitly is
*  clearer than relying on a long long, and it removes any question about how
*  the compiler lays out an eight byte member inside a packed struct. */
typedef struct
{
    uint32_t addr_low;      /* PHYSICAL address of the frame buffer          */
    uint32_t addr_high;     /* always 0 -- see above                         */
    uint16_t length;        /* bytes the card wrote, CRC already stripped    */
    uint16_t checksum;      /* offload result; unused, RXCSUM stays off      */
    uint8_t  status;        /* DD and EOP, written by the card               */
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc;

typedef struct
{
    uint32_t addr_low;      /* PHYSICAL address of the frame to send         */
    uint32_t addr_high;
    uint16_t length;        /* bytes to send from that buffer                */
    uint8_t  cso;           /* checksum offset; unused                       */
    uint8_t  cmd;           /* EOP, IFCS, RS -- see the send path            */
    uint8_t  status;        /* DD, written back by the card                  */
    uint8_t  css;           /* checksum start; unused                        */
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc;

/* Receive descriptor status and error bits */
#define E1000_RXD_STAT_DD   0x01     /* descriptor done -- the card is finished*/
#define E1000_RXD_STAT_EOP  0x02     /* end of packet: the frame ends here    */

/* Errors worth dropping a frame for. Deliberately not "errors != 0": the two
*  top bits of this byte are the IP and TCP checksum offload results, which
*  are meaningless with RXCSUM left at its reset value and would otherwise
*  make this driver discard perfectly good frames on some parts. */
#define E1000_RXD_ERR_CE    0x01     /* CRC error                             */
#define E1000_RXD_ERR_SE    0x02     /* symbol error                          */
#define E1000_RXD_ERR_SEQ   0x04     /* sequence error                        */
#define E1000_RXD_ERR_CXE   0x10     /* carrier extension error               */
#define E1000_RXD_ERR_RXE   0x80     /* RX data error                         */
#define E1000_RXD_ERR_BAD   (E1000_RXD_ERR_CE  | E1000_RXD_ERR_SE  | \
                             E1000_RXD_ERR_SEQ | E1000_RXD_ERR_CXE | \
                             E1000_RXD_ERR_RXE)

/* Transmit descriptor command and status bits */
#define E1000_TXD_CMD_EOP   0x01     /* last descriptor of this frame         */
#define E1000_TXD_CMD_IFCS  0x02     /* insert the ethernet CRC               */
#define E1000_TXD_CMD_RS    0x08     /* report status: write DD back when done*/
#define E1000_TXD_STAT_DD   0x01     /* descriptor done                       */

/* --- Ring geometry ---------------------------------------------------------
*
*  Both counts are chosen rather than inherited, and both are powers of two so
*  that the wrap is a mask instead of a division.
*
*  THIRTY-TWO RECEIVE DESCRIPTORS, because the number that matters is how long
*  a burst the card can absorb without the driver looking. Each descriptor
*  holds one whole frame, so this is 32 frames -- comparable to the twenty the
*  RTL8139's 32 KiB ring holds, and chosen for the same reason: a burst has to
*  survive the gap between two interrupts, and on this kernel that gap can be
*  a whole scheduler slice if the drain task is not the one running. It costs
*  64 KiB of buffers, which is sixteen frames of physical memory.
*
*  SIXTEEN TRANSMIT DESCRIPTORS, which is more than the RTL8139's four for one
*  reason: net_send() has no queue behind it and no way to wait, so a refused
*  frame is a DROPPED frame -- TCP finds out about it a retransmission timeout
*  later. Fifteen usable slots (see the head-equals-tail rule in the file
*  header) is enough for any burst this stack produces, since it is a single
*  drain task and a single shell task doing the sending.
*
*  RDLEN AND TDLEN ARE IN BYTES AND MUST BE A MULTIPLE OF 128. 32 * 16 = 512
*  and 16 * 16 = 256 both are; the counts could not be 20 and 12 even if that
*  were the better size. */
#define E1000_RX_DESCS      32
#define E1000_TX_DESCS      16
#define E1000_RX_MASK       (E1000_RX_DESCS - 1)
#define E1000_TX_MASK       (E1000_TX_DESCS - 1)

/* One buffer per descriptor, and 2048 bytes is the smallest of the four sizes
*  RCTL.BSIZE offers. It holds the 1518 byte maximum frame with room to spare,
*  and since LPE stays clear the card will not even accept a longer one -- so
*  a frame can never span two descriptors, and the receive path needs no
*  reassembly case. */
#define E1000_BUF_SIZE      2048UL

#define E1000_RX_BUF_BYTES  (E1000_RX_DESCS * E1000_BUF_SIZE)
#define E1000_TX_BUF_BYTES  (E1000_TX_DESCS * E1000_BUF_SIZE)
#define E1000_RX_BUF_FRAMES (E1000_RX_BUF_BYTES / PMM_FRAME_SIZE)
#define E1000_TX_BUF_FRAMES (E1000_TX_BUF_BYTES / PMM_FRAME_SIZE)

/* Both rings share one 4 KiB frame: the receive ring in the first half, the
*  transmit ring in the second. A page from pmm_alloc_frames() is 4096 byte
*  aligned, so both bases are far better aligned than the 16 bytes the card
*  requires, and the 2048 byte split keeps them from ever being confused for
*  one another in a memory dump. 768 bytes of the page are unused, which is
*  cheaper than a second allocation and its second failure path. */
#define E1000_TX_RING_OFF   2048UL

/* Frames handled in one interrupt. Same argument as the RTL8139's budget: the
*  handler runs with interrupts off and the whole machine waits for it, so the
*  drain has to be bounded even against a card that keeps refilling the ring.
*  Anything left over is picked up by the next interrupt -- and at a full ring
*  per interrupt that never happens, because the budget IS the ring. */
#define E1000_RX_BUDGET     E1000_RX_DESCS

/* Shortest and longest frame this driver will put on the wire. The minimum is
*  padded to by TCTL.PSP rather than by hand; the maximum is header plus MTU,
*  without the CRC the card appends itself. */
#define E1000_TX_MIN        60UL
#define E1000_TX_MAX        (ETH_HDR_LEN + ETH_MTU)

/* How long the reset and the EEPROM are given, in microseconds of the delay
*  helper below. The manual allows a device reset 10 ms; an EEPROM word comes
*  back in tens of microseconds. Both are generous by an order of magnitude
*  and both are BOUNDED, because a card that never answers must cost a
*  fraction of a second rather than the machine. */
#define E1000_RESET_TIMEOUT 100000UL
#define E1000_EEPROM_TIMEOUT 10000UL

/* --- State -----------------------------------------------------------------
*
*  Everything the card can reach exists twice: as the physical address the DMA
*  engine is given, and as the virtual pointer the kernel reads and writes.
*  Keeping both in named pairs is what stops the two views being mixed up,
*  which is the failure the RTL8139 file warns about and which here would be a
*  card DMAing frames into somebody else's page. */
static volatile uint32_t *e1000_regs;    /* BAR0, mapped                     */
static uint32_t  e1000_mmio_phys;        /* what the BAR said, for describe()*/
static uint16_t  e1000_device_id;
static uint8_t   e1000_irq;
static uint8_t   e1000_mac_addr[ETH_ALEN];
static int       e1000_ready;
static int       e1000_link;             /* last known STATUS.LU             */
static const char *e1000_mac_source;     /* which of the two it came from    */

static uint32_t  e1000_ring_phys;                 /* the shared ring page    */
static volatile e1000_rx_desc *e1000_rx_ring;
static volatile e1000_tx_desc *e1000_tx_ring;

static uint32_t  e1000_rx_buf_phys;
static uint8_t  *e1000_rx_buf;
static uint32_t  e1000_rx_next;          /* next descriptor we expect a frame in */

static uint32_t  e1000_tx_buf_phys;
static uint8_t  *e1000_tx_buf;
static uint32_t  e1000_tx_next;          /* next descriptor to fill          */
static uint32_t  e1000_tx_reap;          /* oldest one still in flight       */
static uint32_t  e1000_tx_inflight;

static char      e1000_info_buf[128];

/* --- Register access -------------------------------------------------------
*
*  Dwords only, and through a volatile pointer, because these are not memory:
*  a read has a side effect on the card (ICR clears itself when read) and a
*  write has one on the wire. The compiler must not merge two of them, drop
*  one whose result is unused, or move one across another. The index is the
*  byte offset divided by four, which is why every offset above is written as
*  the byte offset the manual gives -- a table of pre-divided indices would be
*  one transcription error away from writing to the wrong register. */

static uint32_t e1000_read(uint32_t reg)
{
    return e1000_regs[reg >> 2];
}

static void e1000_write(uint32_t reg, uint32_t value)
{
    e1000_regs[reg >> 2] = value;
}

/* Forces everything written so far to have reached the card before the next
*  step. PCI writes are posted -- they are accepted by the bridge and complete
*  later -- so a sequence that programs a ring and then enables the engine can
*  in principle have the enable overtake the ring. A read of any register on
*  the same device cannot complete until the writes ahead of it have, so one
*  read of STATUS is the whole barrier. STATUS is chosen because reading it
*  has no effect of its own; reading ICR here would silently eat interrupt
*  causes. */
static void e1000_flush(void)
{
    (void)e1000_read(E1000_STATUS);
}

/* Roughly n microseconds, counted in accesses to port 0x80.
*
*  There is no usable clock at the point this is needed. timer_wait() counts
*  ticks of the scheduler's interrupt and this runs on the boot path where
*  spending 10 ms of ticks is both slower than necessary and dependent on an
*  interrupt that may not be flowing yet. Port 0x80 is the POST diagnostic
*  port: writing and reading it does nothing on any machine, and it is decoded
*  on the ISA bus, so one access takes about a microsecond and cannot be
*  optimised away or made faster by a faster CPU. It is the delay every BIOS
*  and every early driver uses, for exactly that reason. */
static void e1000_delay_us(uint32_t us)
{
    while(us-- != 0)
    {
        (void)inportb(0x80);
    }
}

/* --- The description string ------------------------------------------------
*
*  Composed by hand into a fixed buffer, the same way rtl8139.c and ata.c do
*  it: there is no snprintf() in this kernel. */

static int e1000_put_str(int pos, const char *s)
{
    while(*s != '\0' && pos < (int)sizeof(e1000_info_buf) - 1)
    {
        e1000_info_buf[pos++] = *s++;
    }
    return pos;
}

static int e1000_put_hex(int pos, uint32_t value, int digits)
{
    static const char digit[] = "0123456789ABCDEF";
    int shift;

    shift = (digits - 1) * 4;
    while(shift >= 0 && pos < (int)sizeof(e1000_info_buf) - 1)
    {
        e1000_info_buf[pos++] = digit[(value >> shift) & 0x0FUL];
        shift -= 4;
    }
    return pos;
}

static int e1000_put_dec(int pos, uint32_t value)
{
    char tmp[12];
    int  n;

    n = 0;
    do
    {
        tmp[n++] = (char)('0' + (int)(value % 10UL));
        value /= 10UL;
    }
    while(value != 0 && n < (int)sizeof(tmp));

    while(n > 0 && pos < (int)sizeof(e1000_info_buf) - 1)
    {
        e1000_info_buf[pos++] = tmp[--n];
    }
    return pos;
}

/* The physical MMIO base rather than the virtual one, and that is the useful
*  half of the pair: the physical address is what "lspci" would print and what
*  the firmware assigned, so it is the number to compare against when the card
*  is not answering. The virtual address is an artefact of where this kernel
*  happened to put its MMIO window and tells nobody anything. */
static void e1000_build_info(void)
{
    int pos;
    int i;

    pos = e1000_put_str(0, "Intel e1000 (");
    pos = e1000_put_hex(pos, (uint32_t)e1000_device_id, 4);
    pos = e1000_put_str(pos, ") at MMIO 0x");
    pos = e1000_put_hex(pos, e1000_mmio_phys, 8);
    pos = e1000_put_str(pos, ", IRQ ");
    pos = e1000_put_dec(pos, (uint32_t)e1000_irq);
    pos = e1000_put_str(pos, ", MAC ");

    for(i = 0; i < ETH_ALEN; i++)
    {
        if(i != 0)
        {
            pos = e1000_put_str(pos, ":");
        }
        pos = e1000_put_hex(pos, (uint32_t)e1000_mac_addr[i], 2);
    }

    pos = e1000_put_str(pos, e1000_link ? ", link up" : ", link down");

    e1000_info_buf[pos] = '\0';
}

/* --- Interrupt controller --------------------------------------------------
*
*  The same two port accesses rtl8139.c makes, and for the same reason: they
*  are redundant while irq_remap() opens both masks completely, and they make
*  the driver independent of that. The part worth spelling out is the one that
*  is easy to get wrong -- a line above 7 hangs off the SLAVE PIC, whose mask
*  is at 0xA1, and the slave only reaches the CPU through the cascade on IRQ 2
*  in the master mask. QEMU routes this card to IRQ 11, so this is not a
*  theoretical branch here; it is the one that runs. */
static void e1000_unmask_irq(uint8_t irq)
{
    uint8_t mask;

    if(irq >= 8)
    {
        mask = inportb(0xA1);
        outportb(0xA1, (uint8_t)(mask & ~(1 << (irq - 8))));

        mask = inportb(0x21);
        outportb(0x21, (uint8_t)(mask & ~(1 << 2)));    /* cascade */
    }
    else
    {
        mask = inportb(0x21);
        outportb(0x21, (uint8_t)(mask & ~(1 << irq)));
    }
}

/* --- The MAC address -------------------------------------------------------
*
*  Two sources, and the order between them is a decision rather than a
*  fallback chain that happened.
*
*  RAL0/RAH0 IS ASKED FIRST, because it is not merely a copy of the address --
*  it IS the address, in the sense that matters: the receive filter compares
*  every incoming frame against those registers and nothing else. The hardware
*  loads them from the EEPROM during reset. If the two ever disagreed, taking
*  the EEPROM value would give a driver that announces one address in every
*  frame it sends and accepts replies to a different one, which is a card that
*  transmits perfectly and receives nothing -- about the hardest failure in
*  this file to diagnose from the outside. Taking the register value cannot
*  produce that, whatever the EEPROM says.
*
*  THE EEPROM IS THE FALLBACK, for the case where the automatic load did not
*  happen: a card whose EEPROM read at reset failed, or one arriving from a
*  firmware that had already reset it and not put the address back. Words 0, 1
*  and 2 hold the six bytes, low byte of each word first.
*
*  Both answers are checked before being believed. All zeroes is what an
*  unloaded register reads as; all ones is what a card that has stopped
*  answering its BAR reads as, and taking either would be a driver that
*  registers with an address no frame can ever come back to. netdev.h requires
*  that not to happen -- a driver that cannot read its own MAC does not
*  register at all. */

static int e1000_mac_is_usable(const uint8_t *mac)
{
    int all_zero;
    int all_ones;
    int i;

    all_zero = 1;
    all_ones = 1;

    for(i = 0; i < ETH_ALEN; i++)
    {
        if(mac[i] != 0x00)
        {
            all_zero = 0;
        }
        if(mac[i] != 0xFF)
        {
            all_ones = 0;
        }
    }

    if(all_zero || all_ones)
    {
        return 0;
    }

    /* The low bit of the first byte marks a GROUP address. A station's own
    *  address can never be one, so a card reporting it has reported garbage
    *  that happens not to be all zeroes. */
    if((mac[0] & 0x01) != 0)
    {
        return 0;
    }

    return 1;
}

/* One 16 bit word out of the serial EEPROM. Returns 0 on success, and -1 when
*  the card did not set DONE in time -- which is a perfectly ordinary answer
*  on a part whose EEPROM is absent, and the reason the caller treats a failed
*  read as "no address here" rather than as a fault. */
static int e1000_eeprom_read(uint32_t word, uint16_t *out)
{
    uint32_t value;
    uint32_t timeout;

    e1000_write(E1000_EERD,
                (word << E1000_EERD_ADDR_SH) | E1000_EERD_START);

    for(timeout = 0; timeout < E1000_EEPROM_TIMEOUT; timeout++)
    {
        value = e1000_read(E1000_EERD);
        if((value & E1000_EERD_DONE) != 0)
        {
            *out = (uint16_t)(value >> E1000_EERD_DATA_SH);
            return 0;
        }
        e1000_delay_us(1);
    }

    return -1;
}

static int e1000_read_mac(void)
{
    uint8_t  candidate[ETH_ALEN];
    uint32_t ral;
    uint32_t rah;
    uint16_t word;
    int i;

    ral = e1000_read(E1000_RAL0);
    rah = e1000_read(E1000_RAH0);

    /* AV says the hardware itself considers the entry valid, i.e. the load
    *  from EEPROM completed. The bytes are checked as well rather than
    *  instead: a card can set AV over an address that is still nonsense. */
    if((rah & E1000_RAH_AV) != 0)
    {
        candidate[0] = (uint8_t)(ral & 0xFFUL);
        candidate[1] = (uint8_t)((ral >> 8) & 0xFFUL);
        candidate[2] = (uint8_t)((ral >> 16) & 0xFFUL);
        candidate[3] = (uint8_t)((ral >> 24) & 0xFFUL);
        candidate[4] = (uint8_t)(rah & 0xFFUL);
        candidate[5] = (uint8_t)((rah >> 8) & 0xFFUL);

        if(e1000_mac_is_usable(candidate))
        {
            memcpy((void *)e1000_mac_addr, (const void *)candidate, ETH_ALEN);
            e1000_mac_source = "RAL/RAH";
            return 0;
        }
    }

    for(i = 0; i < 3; i++)
    {
        if(e1000_eeprom_read((uint32_t)(E1000_EEPROM_MAC0 + i), &word) != 0)
        {
            return -1;
        }
        candidate[i * 2]     = (uint8_t)(word & 0x00FFU);
        candidate[i * 2 + 1] = (uint8_t)((word >> 8) & 0x00FFU);
    }

    if(!e1000_mac_is_usable(candidate))
    {
        return -1;
    }

    memcpy((void *)e1000_mac_addr, (const void *)candidate, ETH_ALEN);
    e1000_mac_source = "EEPROM";
    return 0;
}

/* --- Receive ---------------------------------------------------------------
*
*  The card walks the ring from RDH forwards, writing one frame into the
*  buffer each descriptor points at and then setting DD in that descriptor's
*  status byte. It stops when it reaches RDT, which is ours: everything from
*  RDH up to RDT-1 belongs to the card, and the descriptor AT RDT does not.
*
*  So the whole receive path is: look at the descriptor we expect next, and if
*  DD is set, the frame is complete and is ours to read. Hand it up, clear the
*  status byte so the card does not see a stale DD, and move RDT onto that
*  descriptor -- which is the act of giving it back.
*
*  RDT IS WRITTEN AFTER net_receive() HAS RETURNED, not before, and this is
*  the same discipline the RTL8139's CAPR needed: the descriptor is the card's
*  again the instant the tail passes it, so writing the tail first is handing
*  back a buffer the stack is still reading out of. net_receive() copies the
*  frame into the receive queue and returns, so the window is short -- but
*  "short" is not "closed", and a frame arriving inside it would be written
*  over the one being copied. */

static void e1000_receive(void)
{
    volatile e1000_rx_desc *desc;
    uint32_t index;
    uint32_t length;
    uint8_t  status;
    uint8_t  errors;
    int budget;

    budget = E1000_RX_BUDGET;

    for(;;)
    {
        if(--budget < 0)
        {
            break;
        }

        index  = e1000_rx_next;
        desc   = &e1000_rx_ring[index];
        status = desc->status;

        if((status & E1000_RXD_STAT_DD) == 0)
        {
            break;              /* the card has not finished this one */
        }

        errors = desc->errors;
        length = (uint32_t)desc->length;

        /* EOP has to be set as well as DD. A frame split across descriptors
        *  cannot happen here -- LPE is off and the buffers are 2048 bytes,
        *  so the card will not accept anything that would not fit -- but a
        *  descriptor with DD and no EOP means it did happen, and passing the
        *  first fragment up as a whole frame would be worse than dropping
        *  it. The length is checked for the same reason: a frame shorter
        *  than an ethernet header is not one, whatever the card says. */
        if((status & E1000_RXD_STAT_EOP) != 0 &&
           (errors & E1000_RXD_ERR_BAD) == 0 &&
           length >= (uint32_t)ETH_HDR_LEN &&
           length <= E1000_BUF_SIZE)
        {
            /* No length adjustment for the CRC: RCTL.SECRC is set in the
            *  bring-up, so the card strips those four bytes and the length
            *  it reports is already the frame without them. The RTL8139 had
            *  to subtract 4 here; getting that difference the wrong way
            *  round gives every frame four bytes of tail garbage or four
            *  bytes missing, and the IP layer rejects both silently. */
            net_receive(e1000_rx_buf + index * E1000_BUF_SIZE, length);
        }

        /* Back to the card: status cleared first, so the descriptor is not
        *  still claiming DD when the tail passes it, then the tail. */
        desc->status = 0;
        desc->length = 0;

        e1000_rx_next = (index + 1UL) & (uint32_t)E1000_RX_MASK;
        e1000_write(E1000_RDT, index);
    }
}

/* --- Transmit --------------------------------------------------------------
*
*  Descriptors are handed to the card by writing TDT past them; the card sets
*  DD in each one it has finished with, because every descriptor this driver
*  posts has RS set. Reclaiming is therefore a walk forwards from the oldest
*  outstanding descriptor for as long as DD is set.
*
*  It is done at the head of the SEND path rather than from an interrupt, and
*  that is why TXDW is not in the enabled interrupt mask. The card sets DD
*  whether or not anybody asked to be told about it, so an interrupt would buy
*  nothing but a trip through the handler for work that the next send is about
*  to do anyway -- and the only caller that cares whether a slot is free is
*  the send path itself. */
static void e1000_transmit_reap(void)
{
    while(e1000_tx_inflight != 0)
    {
        if((e1000_tx_ring[e1000_tx_reap].status & E1000_TXD_STAT_DD) == 0)
        {
            break;
        }

        e1000_tx_ring[e1000_tx_reap].status = 0;
        e1000_tx_reap = (e1000_tx_reap + 1UL) & (uint32_t)E1000_TX_MASK;
        e1000_tx_inflight--;
    }
}

/* --- Interrupt handler -----------------------------------------------------
*
*  READING ICR IS THE ACKNOWLEDGEMENT. There is no write-one-to-clear register
*  to follow it with: the read returns the causes and clears them in the same
*  access, and it is that clearing which drops the PCI interrupt line. A
*  handler that did the work first and read ICR afterwards would race exactly
*  as the RTL8139's would -- a frame arriving mid-handler sets RXT0, and the
*  late read swallows it along with the causes it meant to acknowledge, giving
*  a receive that stops for no visible reason.
*
*  A zero read means the interrupt was not ours. PCI lines are shared and the
*  PIC hands every handler on the line its turn, so this is an ordinary
*  outcome and not an error.
*
*  Bounded to four rounds for the same reason the RTL8139's is: this runs with
*  interrupts disabled and the machine waiting, and a loop that kept pace with
*  a busy card would never give anything else a turn. */
static void e1000_interrupt(struct regs *r)
{
    uint32_t icr;
    int rounds;

    (void)r;    /* the register frame is of no interest here */

    for(rounds = 0; rounds < 4; rounds++)
    {
        icr = e1000_read(E1000_ICR);
        if(icr == 0)
        {
            break;
        }

        /* An overrun is drained exactly like a normal receive. RXO means the
        *  card ran out of descriptors and threw frames away; the cure is to
        *  consume what is in the ring and move the tail on, which is what
        *  the drain does. Nothing else needs saying about it -- the frames
        *  it lost are gone and the protocols above deal with loss. */
        if((icr & (E1000_INT_RXT0 | E1000_INT_RXDMT0 | E1000_INT_RXO)) != 0)
        {
            e1000_receive();
        }

        /* The link came or went. Only recorded, because there is nothing to
        *  do about it from here: the stack has no notion of an interface
        *  going down, and reprogramming the card would be work done in
        *  interrupt context for a state change nobody is waiting on. What it
        *  buys is that "ifconfig" says "link down" rather than leaving
        *  somebody to conclude the driver is broken when the cable is out. */
        if((icr & E1000_INT_LSC) != 0)
        {
            e1000_link = ((e1000_read(E1000_STATUS) & E1000_STATUS_LU) != 0);
        }
    }
}

/* --- What this driver offers the netdev layer ------------------------------
*
*  Declared here and defined below, because the transmit path belongs at the
*  bottom of the file next to the ring it fills while the struct has to exist
*  before e1000_init() can hand it over. The same shape rtl8139.c has, for the
*  same reason: nothing above this line ever learns which chip carried its
*  frame. */
static const uint8_t *e1000_mac(void);
static const char    *e1000_describe(void);
static int            e1000_send(const void *frame, uint32_t len);

static const netdev_ops e1000_netdev_ops =
{
    "Intel e1000",
    e1000_send,
    e1000_mac,
    e1000_describe
};

/* --- Memory for the rings --------------------------------------------------
*
*  Three allocations, all from the frame allocator and none from the heap, for
*  the reason rtl8139.c states and this card makes stricter: the card reads
*  descriptors and frame data by PHYSICAL address, so the memory has to be
*  physically contiguous -- which pmm_alloc_frames() promises and malloc()
*  does not -- and it has to have a physical address the driver can know,
*  which for a heap pointer means nothing at all.
*
*  THE CHECK AGAINST DIRECT_MAP_LIMIT is the other half. The card can reach
*  any physical address a 32 bit bus can carry, so it is not the card that
*  constrains this; it is the kernel. P2V() is the only way this driver can
*  read the bytes the card wrote, and P2V() is defined only inside the direct
*  mapping. A block above it would be perfectly good DMA memory that the
*  driver could never look at.
*
*  Frees on the failure paths rather than leaking: e1000_init() can be reached
*  on a machine whose memory is nearly gone, and a failed bring-up that keeps
*  a hundred kilobytes forever is a failure that makes the next one likelier.
*/

static void e1000_free_memory(void)
{
    if(e1000_rx_buf_phys != 0)
    {
        pmm_free_frames((void *)e1000_rx_buf_phys, (uint32_t)E1000_RX_BUF_FRAMES);
        e1000_rx_buf_phys = 0;
        e1000_rx_buf = 0;
    }
    if(e1000_tx_buf_phys != 0)
    {
        pmm_free_frames((void *)e1000_tx_buf_phys, (uint32_t)E1000_TX_BUF_FRAMES);
        e1000_tx_buf_phys = 0;
        e1000_tx_buf = 0;
    }
    if(e1000_ring_phys != 0)
    {
        pmm_free_frames((void *)e1000_ring_phys, 1UL);
        e1000_ring_phys = 0;
        e1000_rx_ring = 0;
        e1000_tx_ring = 0;
    }
}

/* Contiguous, physical, and reachable through the direct mapping. Returns the
*  physical address or 0, and reports which of the two ways it failed -- "out
*  of memory" and "the memory is where I cannot read it" have different cures
*  and look identical from the caller. */
static uint32_t e1000_alloc_block(uint32_t frames, uint32_t bytes,
                                  const char *what)
{
    uint32_t phys;

    phys = (uint32_t)pmm_alloc_frames(frames);
    if(phys == 0)
    {
        printf("e1000: no contiguous memory for the %s\n", what);
        return 0;
    }
    if(phys > (uint32_t)DIRECT_MAP_LIMIT - (bytes - 1UL))
    {
        pmm_free_frames((void *)phys, frames);
        printf("e1000: the %s landed outside the direct mapping\n", what);
        return 0;
    }

    memset(P2V(phys), 0, (size_t)bytes);
    return phys;
}

static int e1000_alloc_memory(void)
{
    e1000_ring_phys = e1000_alloc_block(1UL, (uint32_t)PMM_FRAME_SIZE,
                                        "descriptor rings");
    if(e1000_ring_phys == 0)
    {
        return -1;
    }

    e1000_rx_buf_phys = e1000_alloc_block((uint32_t)E1000_RX_BUF_FRAMES,
                                          (uint32_t)E1000_RX_BUF_BYTES,
                                          "receive buffers");
    if(e1000_rx_buf_phys == 0)
    {
        e1000_free_memory();
        return -1;
    }

    e1000_tx_buf_phys = e1000_alloc_block((uint32_t)E1000_TX_BUF_FRAMES,
                                          (uint32_t)E1000_TX_BUF_BYTES,
                                          "transmit buffers");
    if(e1000_tx_buf_phys == 0)
    {
        e1000_free_memory();
        return -1;
    }

    e1000_rx_ring = (volatile e1000_rx_desc *)P2V(e1000_ring_phys);
    e1000_tx_ring = (volatile e1000_tx_desc *)
                    P2V(e1000_ring_phys + E1000_TX_RING_OFF);
    e1000_rx_buf  = (uint8_t *)P2V(e1000_rx_buf_phys);
    e1000_tx_buf  = (uint8_t *)P2V(e1000_tx_buf_phys);

    return 0;
}

/* --- Bringing the two rings up ---------------------------------------------
*
*  Both follow the same five register sequence -- base low, base high, length,
*  head, tail -- and the order inside it is not free: the head and tail
*  pointers are indices into a ring whose base and length the card has to know
*  first, and writing the control register that enables the engine before all
*  five are in place starts a DMA engine on a ring description that is half
*  somebody else's leftovers. So the enable is not here; it happens once, at
*  the end of the bring-up, after both of these have run. */

static void e1000_setup_rx_ring(void)
{
    uint32_t i;

    for(i = 0; i < (uint32_t)E1000_RX_DESCS; i++)
    {
        e1000_rx_ring[i].addr_low  = e1000_rx_buf_phys + i * E1000_BUF_SIZE;
        e1000_rx_ring[i].addr_high = 0;
        e1000_rx_ring[i].length    = 0;
        e1000_rx_ring[i].checksum  = 0;
        e1000_rx_ring[i].status    = 0;
        e1000_rx_ring[i].errors    = 0;
        e1000_rx_ring[i].special   = 0;
    }

    e1000_write(E1000_RDBAL, e1000_ring_phys);
    e1000_write(E1000_RDBAH, 0);
    e1000_write(E1000_RDLEN, (uint32_t)(E1000_RX_DESCS * 16UL));

    /* HEAD AT 0, TAIL AT THE LAST DESCRIPTOR. The card owns everything from
    *  the head up to but not including the tail, so this gives it 31 of the
    *  32 and keeps the last one -- which is exactly the "one short of the
    *  whole ring" that head-equals-tail-means-empty forces. Setting the tail
    *  to 0 as well, which reads as the obvious thing to do, hands the card an
    *  EMPTY ring: it would receive nothing at all and set no error to say
    *  why. */
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, (uint32_t)E1000_RX_MASK);

    e1000_rx_next = 0;

    /* No receive interrupt delay. Both timers exist to coalesce interrupts,
    *  and setting either of them means the card holds a received frame back
    *  until the timer expires. With RDTR at zero the card raises RXT0 as soon
    *  as a frame has landed, which is what a kernel that copies the frame
    *  into a queue and returns actually wants -- the coalescing this kernel
    *  needs is the receive TASK, not the card. Written explicitly rather than
    *  left at the reset value, because "the reset value happens to be zero"
    *  is not the same statement as "zero is what this driver wants". */
    e1000_write(E1000_RDTR, 0);
    e1000_write(E1000_RADV, 0);
}

static void e1000_setup_tx_ring(void)
{
    uint32_t i;

    for(i = 0; i < (uint32_t)E1000_TX_DESCS; i++)
    {
        e1000_tx_ring[i].addr_low  = e1000_tx_buf_phys + i * E1000_BUF_SIZE;
        e1000_tx_ring[i].addr_high = 0;
        e1000_tx_ring[i].length    = 0;
        e1000_tx_ring[i].cso       = 0;
        e1000_tx_ring[i].cmd       = 0;
        e1000_tx_ring[i].status    = 0;
        e1000_tx_ring[i].css       = 0;
        e1000_tx_ring[i].special   = 0;
    }

    e1000_write(E1000_TDBAL, e1000_ring_phys + E1000_TX_RING_OFF);
    e1000_write(E1000_TDBAH, 0);
    e1000_write(E1000_TDLEN, (uint32_t)(E1000_TX_DESCS * 16UL));

    /* Head AND tail at zero here, which is the opposite of the receive ring
    *  and means the same thing: an empty ring. On the transmit side empty is
    *  correct -- there is nothing to send yet, and every send moves the tail
    *  forward by one. */
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    e1000_tx_next     = 0;
    e1000_tx_reap     = 0;
    e1000_tx_inflight = 0;
}

/* --- Reset -----------------------------------------------------------------
*
*  Returns 0 when the card came back. The sequence is short and every step of
*  it is load bearing:
*
*    - MASK EVERY INTERRUPT FIRST, by writing all ones to IMC. The handler is
*      not installed at this point and the PIC line may already be open from
*      whatever the firmware left behind, so a card that raised an interrupt
*      now would be an interrupt with nowhere to go, repeatedly, on a level
*      triggered line: the machine would not get past this line.
*
*    - THEN RESET, by setting CTRL.RST. Everything else in CTRL is written as
*      zero along with it, which is deliberate rather than lazy -- the bits
*      that matter are the ones that must NOT be set, and PHY_RST in
*      particular would leave the physical layer held in reset afterwards,
*      giving a card that answers every register and never sees a link.
*
*    - WAIT FOR IT TO CLEAR RATHER THAN ASSUME. RST clears itself when the
*      reset completes. The manual allows the card 10 ms and forbids touching
*      any other register in the meantime, hence the delay before the first
*      poll; the poll after it is bounded so that a card which never answers
*      costs a tenth of a second rather than the machine.
*
*    - AND MASK AGAIN AFTERWARDS, plus one read of ICR. A reset restores the
*      interrupt mask to its default and leaves causes standing; without this
*      the card can assert its line the moment the handler goes in, reporting
*      a cause that belongs to the state before the reset. */
static int e1000_reset(void)
{
    uint32_t timeout;

    e1000_write(E1000_IMC, 0xFFFFFFFFUL);
    e1000_flush();

    e1000_write(E1000_CTRL, E1000_CTRL_RST);
    e1000_flush();

    e1000_delay_us(10000UL);

    for(timeout = 0; timeout < E1000_RESET_TIMEOUT; timeout++)
    {
        if((e1000_read(E1000_CTRL) & E1000_CTRL_RST) == 0)
        {
            break;
        }
        e1000_delay_us(1);
    }

    if(timeout >= E1000_RESET_TIMEOUT)
    {
        return -1;
    }

    e1000_write(E1000_IMC, 0xFFFFFFFFUL);
    (void)e1000_read(E1000_ICR);

    return 0;
}

/* Puts the card back to sleep. Reached on one path only: the card came up
*  perfectly and netdev_register() turned it away because another driver had
*  already registered. Leaving it running would be a card DMAing frames into a
*  ring nobody drains -- out of memory that is handed back here -- and raising
*  an interrupt for each one, on a line it very likely shares with the card
*  that IS in use, so the second adapter would not merely be idle, it would
*  slow the first one down.
*
*  THE INTERRUPT TABLE IS NOT TOUCHED HERE, and that absence is load bearing
*  rather than an oversight. irq_routines[] in irq.c holds ONE handler per
*  line, so irq_uninstall_handler() does not remove "our" handler, it empties
*  the line. On the machine this path exists for -- two network cards, which
*  QEMU puts on IRQ 11 together and a real board very often does too -- that
*  line belongs to the card that is in use, and clearing it leaves the working
*  card with no handler at all: a machine that boots, says "net: up", and
*  never receives a frame. That was not a hypothesis; it is what this driver
*  did until the bring-up below was reordered to install its handler only
*  AFTER netdev_register() has said yes, so that a card which stands down has
*  never been in the table to be removed from. */
static void e1000_shutdown(void)
{
    e1000_write(E1000_IMC, 0xFFFFFFFFUL);
    e1000_write(E1000_RCTL, 0);
    e1000_write(E1000_TCTL, 0);
    e1000_flush();

    e1000_free_memory();
    e1000_ready = 0;
}

/* --- Finding and mapping the card ------------------------------------------
*
*  pci_io_base() is not used here and cannot be: it exists to turn an I/O BAR
*  into a port number and returns 0 for a memory one, which is the right
*  answer to the wrong question. This card's registers are in a MEMORY BAR,
*  so the raw value out of the enumeration record is decoded here instead.
*
*  BAR1 is an I/O window on some members of the family and would make
*  pci_io_base() work -- and it is not worth having. It reaches the same
*  registers through an index/data port pair, i.e. two port writes per
*  register access instead of one memory access, and it does not exist on
*  every part. A driver built on it would be slower everywhere and absent
*  somewhere.
*
*  The low four bits of a memory BAR are not address: bit 0 is 0 to say it is
*  memory at all, bits 2:1 are the type, and bit 3 is prefetchability. A
*  driver that masked only bit 0 off would be up to 14 bytes wrong. */
static int e1000_map_registers(const pci_device *dev)
{
    uint32_t bar;
    uint32_t type;
    void *mapped;

    bar = dev->bar[0];

    if((bar & 0x01UL) != 0)
    {
        printf("e1000: BAR0 is an I/O region, not the register block\n");
        return -1;
    }

    /* Type 0 is a 32 bit BAR, type 2 a 64 bit one whose upper half lives in
    *  BAR1. These parts use 32 bit, but the check is here rather than assumed
    *  -- and a 64 bit BAR with anything in its upper half is refused outright,
    *  because this kernel has no way to reach memory above 4 GiB. */
    type = (bar >> 1) & 0x03UL;
    if(type == 0x02UL)
    {
        if(dev->bar[1] != 0)
        {
            printf("e1000: register block is above 4 GiB, out of reach\n");
            return -1;
        }
    }
    else if(type != 0x00UL)
    {
        printf("e1000: BAR0 has an unknown memory type\n");
        return -1;
    }

    e1000_mmio_phys = bar & 0xFFFFFFF0UL;
    if(e1000_mmio_phys == 0)
    {
        printf("e1000: firmware assigned no address to the register block\n");
        return -1;
    }

    /* The one call that has to happen before any register is touched. It
    *  returns a pointer into the kernel's MMIO window, mapped uncached --
    *  which matters as much as the mapping itself, since a cached mapping
    *  would let the processor answer a register read out of a cache line it
    *  filled a moment ago and never see the card change anything. */
    mapped = vmm_map_mmio(e1000_mmio_phys, (uint32_t)E1000_BAR_SIZE);
    if(mapped == 0)
    {
        printf("e1000: could not map the %d KiB register block at 0x%X\n",
               (int)(E1000_BAR_SIZE / 1024UL), (int)e1000_mmio_phys);
        return -1;
    }

    e1000_regs = (volatile uint32_t *)mapped;
    return 0;
}

/* Looks through the table pci_init() built rather than walking the bus again.
*  Two ids, so pci_find() is called twice rather than the table being iterated
*  by hand -- pci_find() IS the table lookup, and doing it twice is cheaper to
*  read than a loop over pci_count() that reimplements it. */
static const pci_device *e1000_find(void)
{
    const pci_device *dev;

    dev = pci_find(E1000_VENDOR_ID, E1000_DEV_82540EM);
    if(dev != 0)
    {
        return dev;
    }

    return pci_find(E1000_VENDOR_ID, E1000_DEV_82545EM);
}

/* --- Bring-up --------------------------------------------------------------
*
*  The register writes below happen in this order because each one depends on
*  the last, and the two ends of the sequence are the ones worth stating:
*
*    - THE CARD IS RESET BEFORE ANYTHING IS CONFIGURED, so that nothing here
*      is building on whatever the firmware left in a register.
*
*    - THE ENGINES GO ON BEFORE THE INTERRUPTS, and the interrupts go on after
*      netdev_register() has accepted the card. The full argument for that
*      last part is at the bottom of this function, next to the two lines it
*      is about; the short version is that a second card which stands down
*      must not have touched the interrupt table on its way through. */
int e1000_init(void)
{
    const pci_device *dev;
    uint32_t i;
    int rc;

    if(e1000_ready)
    {
        return 0;
    }

    dev = e1000_find();
    if(dev == 0)
    {
        return -1;      /* no card; net_init() copes with that */
    }

    e1000_device_id = dev->device;
    e1000_irq = dev->irq;
    if(e1000_irq > 15)
    {
        printf("e1000: no usable interrupt line (%d)\n", (int)e1000_irq);
        return -1;
    }

    /* MEMORY space rather than I/O space, which is the one line of this
    *  function that differs from the RTL8139's for a reason other than the
    *  chip: the registers are reached by memory access, so PCI_CMD_IO would
    *  enable a window nothing uses and leave the one that matters closed.
    *  Bus mastering is the same requirement as ever and is the whole game --
    *  without it the card fetches no descriptor and the driver below works
    *  perfectly against hardware that does nothing. */
    pci_enable(dev, (uint16_t)(PCI_CMD_MEMORY | PCI_CMD_MASTER));

    if(e1000_map_registers(dev) != 0)
    {
        return -1;
    }

    if(e1000_reset() != 0)
    {
        printf("e1000: reset did not complete\n");
        return -1;
    }

    /* Link up, and let the PHY negotiate the speed. SLU is a request rather
    *  than a report: on these parts the MAC keeps the link down until this
    *  bit is set, so a driver that programs everything else correctly and
    *  omits it gets a card that is configured, enabled, answering, and not
    *  connected to anything. ASDE next to it tells the MAC to take its speed
    *  and duplex from what the PHY negotiated instead of from the FRCSPD and
    *  FRCDPLX bits, which are left clear. */
    e1000_write(E1000_CTRL, E1000_CTRL_SLU | E1000_CTRL_ASDE);
    e1000_flush();

    /* Flow control off, by clearing the four registers that configure it.
    *  Their reset values are not all zero on every part, and a card with a
    *  flow control address programmed will both send PAUSE frames when its
    *  receive FIFO fills and honour ones it receives -- neither of which this
    *  driver has any code for, and the second of which is a transmitter that
    *  stops for a reason nothing here can observe. */
    e1000_write(E1000_FCAL, 0);
    e1000_write(E1000_FCAH, 0);
    e1000_write(E1000_FCT, 0);
    e1000_write(E1000_FCTTV, 0);

    if(e1000_read_mac() != 0)
    {
        printf("e1000: could not read a usable MAC address\n");
        return -1;
    }

    /* Write the address back into RAL0/RAH0 with AV set. It is very likely
    *  already there -- that is where it was just read from -- but it is not
    *  there when the EEPROM was the source, and this is the register the
    *  receive filter compares against. Writing it unconditionally means the
    *  filter and the address this driver announces are the same six bytes by
    *  construction rather than by luck. RAL must be written before RAH: the
    *  hardware latches the entry when AV goes in. */
    e1000_write(E1000_RAL0,
                (uint32_t)e1000_mac_addr[0] |
                ((uint32_t)e1000_mac_addr[1] << 8) |
                ((uint32_t)e1000_mac_addr[2] << 16) |
                ((uint32_t)e1000_mac_addr[3] << 24));
    e1000_write(E1000_RAH0,
                (uint32_t)e1000_mac_addr[4] |
                ((uint32_t)e1000_mac_addr[5] << 8) |
                E1000_RAH_AV);

    /* The multicast table array, all 128 dwords of it, cleared. Its contents
    *  are undefined after a reset on some parts, and every bit that happens
    *  to be set is a multicast group this card will accept frames for -- a
    *  ring filling with traffic nobody asked for on a busy segment. Nothing
    *  in this kernel joins a multicast group, so the correct table is an
    *  empty one. */
    for(i = 0; i < (uint32_t)E1000_MTA_ENTRIES; i++)
    {
        e1000_write(E1000_MTA + i * 4UL, 0);
    }

    if(e1000_alloc_memory() != 0)
    {
        return -1;
    }

    e1000_setup_rx_ring();
    e1000_setup_tx_ring();
    e1000_flush();

    /* Receive: our own address and broadcast, 2048 byte buffers, and the CRC
    *  stripped so the length in each descriptor is the frame the stack wants.
    *  What is NOT set matters as much:
    *
    *    - UPE and MPE stay clear, so no promiscuous mode. BAM is set on its
    *      own because ARP is broadcast and nothing resolves without it.
    *    - SBP stays clear: a frame the card knows is damaged is of no use to
    *      anybody above here.
    *    - LPE stays clear, which caps the accepted frame at 1522 bytes and is
    *      what makes "a frame never spans two descriptors" true.
    *    - BSEX stays clear, which is what makes the 00 in the size field mean
    *      2048 bytes rather than 2048 times sixteen. */
    e1000_write(E1000_RCTL,
                E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NONE |
                E1000_RCTL_RDMTS_H2 | E1000_RCTL_MO_0 |
                E1000_RCTL_SZ_2048 | E1000_RCTL_SECRC);

    /* Transmit: the gap first, then the engine. PSP pads a short frame out to
    *  60 bytes in hardware, which is why the send path below does not do it
    *  by hand the way the RTL8139's had to -- and it pads with zeroes rather
    *  than with whatever was left in the buffer, so it is also the safe way
    *  round. */
    e1000_write(E1000_TIPG, E1000_TIPG_DEFAULT);
    e1000_write(E1000_TCTL,
                E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT_16 |
                E1000_TCTL_COLD_FD | E1000_TCTL_RTLC);
    e1000_flush();

    /* No interrupt throttling. ITR would put a floor under the gap between
    *  two interrupts, which is a sensible thing to want on a gigabit link and
    *  a pointless one here: this kernel cannot generate enough traffic for
    *  the interrupt rate to be the bottleneck, and a floor is one more reason
    *  for a frame to arrive late while somebody is debugging why it did. */
    e1000_write(E1000_ITR, 0);

    e1000_link  = ((e1000_read(E1000_STATUS) & E1000_STATUS_LU) != 0);
    e1000_ready = 1;
    e1000_build_info();

    rc = netdev_register(&e1000_netdev_ops);
    if(rc != 0)
    {
        /* Another card got there first, which netdev.h is explicit is not a
        *  failure to report as one: this card is fine and is simply not the
        *  one in use. Said in one line so that a machine with two adapters
        *  explains itself, and then shut down -- see e1000_shutdown(). */
        printf("e1000: %s, but another card is already in use\n",
               e1000_info_buf);
        e1000_shutdown();
        return -1;
    }

    /* ONLY NOW THE INTERRUPT, and in this order so that everything the card
    *  could report already has somewhere to land: the handler, then the PIC
    *  line, then the mask that lets the card pull it.
    *
    *  After the registration rather than before it, which is the part worth
    *  knowing and which this driver got wrong first. irq_routines[] holds one
    *  handler per line, so a second card that installed before finding out it
    *  was second would overwrite the incumbent's handler and then, standing
    *  down, clear the line entirely -- taking the working card's receive path
    *  with it. Doing it here means a card that stands down never touched the
    *  table.
    *
    *  The receiver has been enabled for a few instructions by this point with
    *  no interrupt possible, which is deliberate and harmless: frames that
    *  arrive in the gap sit in the ring, their cause bit stands in ICR, and
    *  the IMS write below is what turns a standing cause into a line. Nothing
    *  is lost. The reverse order has no such harmless gap -- interrupts before
    *  the rings would be a handler reading a ring that does not exist. */
    irq_install_handler((int)e1000_irq, e1000_interrupt);
    e1000_unmask_irq(e1000_irq);
    e1000_write(E1000_IMS, E1000_INT_MASK);
    e1000_flush();

    printf("%s\n", e1000_info_buf);
    printf("e1000: %d receive and %d transmit descriptors, MAC from %s\n",
           (int)E1000_RX_DESCS, (int)E1000_TX_DESCS, e1000_mac_source);

    return 0;
}

/* --- The netdev_ops behind the pointers ------------------------------------ */

static const uint8_t *e1000_mac(void)
{
    return e1000_mac_addr;
}

static const char *e1000_describe(void)
{
    /* Rebuilt rather than returned as it stood, because one thing in it can
    *  have changed since bring-up: the link. This is called from "ifconfig",
    *  in task context, which is precisely the moment somebody wants to know
    *  whether the cable is in. */
    e1000_build_info();
    return e1000_info_buf;
}

/* The frame is copied into a buffer this driver owns, for the reasons the
*  RTL8139's send path gives and which are if anything stronger here: the card
*  fetches it by physical address, so it has to be physically contiguous and
*  its physical address has to be knowable, and it has to stay untouched until
*  the card has fetched it. The stack promises none of the three about
*  whatever it hands down.
*
*  ONE DESCRIPTOR PER FRAME. The hardware can scatter a frame across several,
*  which is what a driver that sent the stack's buffer directly would need;
*  copying into one 2048 byte buffer makes every frame exactly one descriptor
*  with EOP set, and the reap loop above correspondingly simple. */
static int e1000_send(const void *frame, uint32_t len)
{
    volatile e1000_tx_desc *desc;
    uint32_t index;

    if(!e1000_ready)
    {
        return -1;
    }
    if(frame == 0 || len == 0 || len > E1000_TX_MAX)
    {
        return -1;
    }

    /* Take back whatever the card has finished with since the last send. This
    *  is the only place it happens, which is why it is the first thing here
    *  rather than the last. */
    e1000_transmit_reap();

    /* ONE SLOT IS ALWAYS LEFT EMPTY, and this is the check that keeps it so.
    *  Filling the last one would move TDT onto TDH, which the card reads as
    *  an empty ring -- so a full ring of frames would sit there unsent, and
    *  since no descriptor would ever complete, nothing would ever reclaim
    *  one either. A refusal here is recoverable; that is not. */
    if(e1000_tx_inflight >= (uint32_t)(E1000_TX_DESCS - 1))
    {
        return -1;      /* the caller retries */
    }

    index = e1000_tx_next;
    desc  = &e1000_tx_ring[index];

    memcpy(e1000_tx_buf + index * E1000_BUF_SIZE, frame, (size_t)len);

    /* The address is rewritten on every send even though it never changes.
    *  It costs two stores and it means the descriptor is fully described by
    *  this block rather than half here and half in e1000_setup_tx_ring(),
    *  which is what makes it readable next to the manual. */
    desc->addr_low  = e1000_tx_buf_phys + index * E1000_BUF_SIZE;
    desc->addr_high = 0;
    desc->length    = (uint16_t)len;
    desc->cso       = 0;
    desc->css       = 0;
    desc->special   = 0;

    /* Cleared before the command goes in: DD is what "the card is done with
    *  this" means, and a descriptor handed over with a stale DD standing
    *  would be reclaimed by the very next reap while the card was still
    *  reading out of its buffer. */
    desc->status = 0;

    /* EOP because this is the whole frame, IFCS so the card appends the
    *  ethernet CRC -- netdev.h makes that the driver's job and every card
    *  this kernel is likely to meet does it in hardware -- and RS so the card
    *  writes DD back when it is finished, which is the only way the reap loop
    *  can know. Written last of the descriptor's fields, before the tail. */
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;

    e1000_tx_next = (index + 1UL) & (uint32_t)E1000_TX_MASK;
    e1000_tx_inflight++;

    /* THE HANDOVER. Everything above is memory the card is not looking at
    *  yet; this store is what tells it to. It has to be the last one, and the
    *  compiler must not move it earlier, which is what the volatile ring
    *  pointer and the volatile register access together guarantee. */
    e1000_write(E1000_TDT, e1000_tx_next);

    return 0;
}
