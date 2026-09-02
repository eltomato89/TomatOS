/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Realtek RTL8139 fast ethernet driver
*
*  The card is found on the PCI bus (vendor 0x10EC, device 0x8139) and driven
*  entirely through its I/O port window -- BAR0. The second BAR maps the same
*  registers into memory, but ports need no mapping and every register here
*  is at most 32 bits wide, so there is nothing to gain from it.
*
*  Three things decide whether this driver works at all, and all three are
*  silent when they are wrong:
*
*    - BUS MASTERING. The card does not wait to be asked: it writes received
*      frames into main memory by itself and reads transmit buffers out of
*      it. Without PCI_CMD_MASTER in the command register the chip answers
*      every register read, reports a link, sets no error anywhere -- and
*      never receives a single byte.
*
*    - PHYSICAL ADDRESSES. RBSTART and TSAD0..3 are read by the card's own
*      DMA engine, which does not go through the MMU. Everything handed over
*      is therefore a physical address out of pmm_alloc_frames() (the frame
*      allocator returns physical addresses and hands out contiguous blocks,
*      which the heap does not promise), and every kernel access to the same
*      memory goes through P2V(). Mixing the two views up gives a card that
*      DMAs into a random frame -- usually somebody else's.
*
*    - THE CAPR OFFSET. The receive pointer register reads and writes 16 less
*      than the offset actually meant. Forgetting it produces a driver that
*      receives exactly one packet and then sits there forever, because the
*      card believes the ring is full.
*
*  The receive side is one ring the card fills continuously; the transmit
*  side is four slots used round robin. No descriptors, no scatter-gather.
*
*  WHAT THIS FILE IS NOW, AND WHAT IT IS NOT. It is a driver and nothing else.
*  It used to be the network as far as the rest of the kernel was concerned:
*  net.c called rtl8139_send() and rtl8139_mac() by name and the shell asked
*  rtl8139_present() whether the machine had a network at all, which is why a
*  machine with an Intel card was told it had none. So the four functions those
*  callers used are static now and are handed to netdev_register() as a
*  netdev_ops instead; see rtl8139.h for the list and netdev.h for the
*  argument. rtl8139_init() is all that is left of the outside surface, and it
*  is here because finding a card is the one thing that cannot be asked of the
*  layer whose whole existence depends on a driver having found one.
*
*  Nothing above this file may assume an RTL8139 any more, and equally nothing
*  in this file has to care what is above it. The receive direction is the
*  exception that proves it: rtl_receive() still calls net_receive() directly,
*  because netdev.h has no receive member on purpose -- a card pushes, so there
*  is no moment for the stack to choose and therefore nothing to abstract.
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
#include <rtl8139.h>

/* --- Identity ------------------------------------------------------------ */
#define RTL_VENDOR_ID     0x10EC
#define RTL_DEVICE_ID     0x8139

/* --- Register offsets, relative to the I/O base --------------------------
*
*  Width matters: IDR and CR are byte registers, CAPR/CBR/IMR/ISR are words,
*  RBSTART/TSAD/TSD/TCR/RCR are dwords. Accessing one of them with the wrong
*  width does not fault, it just returns or writes nonsense. */
#define RTL_IDR0          0x00   /* 6 bytes: the MAC address                 */
#define RTL_TSD0          0x10   /* 4 dwords: transmit status / command      */
#define RTL_TSAD0         0x20   /* 4 dwords: transmit buffer, PHYSICAL      */
#define RTL_RBSTART       0x30   /* dword: receive ring, PHYSICAL            */
#define RTL_CR            0x37   /* byte: command                            */
#define RTL_CAPR          0x38   /* word: current address of packet read     */
#define RTL_CBR           0x3A   /* word: current buffer address (card side) */
#define RTL_IMR           0x3C   /* word: interrupt mask                     */
#define RTL_ISR           0x3E   /* word: interrupt status, write 1 to clear */
#define RTL_TCR           0x40   /* dword: transmit configuration            */
#define RTL_RCR           0x44   /* dword: receive configuration             */
#define RTL_CONFIG1       0x52   /* byte: power management                   */

/* --- Command register (0x37) --------------------------------------------- */
#define RTL_CR_BUFE       0x01   /* receive buffer empty -- nothing to read  */
#define RTL_CR_TE         0x04   /* transmitter enable                       */
#define RTL_CR_RE         0x08   /* receiver enable                          */
#define RTL_CR_RST        0x10   /* software reset, clears itself when done  */

/* --- Interrupt bits, identical in IMR and ISR ---------------------------- */
#define RTL_INT_ROK       0x0001 /* a frame was received                     */
#define RTL_INT_RER       0x0002 /* receive error                            */
#define RTL_INT_TOK       0x0004 /* a frame went out                         */
#define RTL_INT_TER       0x0008 /* transmit error                           */
#define RTL_INT_RXOVW     0x0010 /* receive ring overflowed                  */
#define RTL_INT_FOVW      0x0040 /* receive FIFO overflowed                  */
#define RTL_INT_SYSERR    0x8000 /* PCI bus error -- something is badly off  */

#define RTL_INT_MASK      (RTL_INT_ROK | RTL_INT_RER | RTL_INT_TOK |  \
                           RTL_INT_TER | RTL_INT_RXOVW | RTL_INT_FOVW | \
                           RTL_INT_SYSERR)

/* --- Receive configuration (0x44) ---------------------------------------- */
#define RTL_RCR_AAP       0x00000001UL   /* accept all -- promiscuous        */
#define RTL_RCR_APM       0x00000002UL   /* accept frames matching our MAC   */
#define RTL_RCR_AM        0x00000004UL   /* accept multicast                 */
#define RTL_RCR_AB        0x00000008UL   /* accept broadcast (ARP needs it)  */
#define RTL_RCR_AR        0x00000010UL   /* accept runts                     */
#define RTL_RCR_AER       0x00000020UL   /* accept errored frames            */
#define RTL_RCR_WRAP      0x00000080UL   /* do NOT wrap a frame at the end   */
#define RTL_RCR_MXDMA_UNL 0x00000700UL   /* burst size: unlimited            */
#define RTL_RCR_RBLEN_32K 0x00001000UL   /* ring length selector, see below  */
#define RTL_RCR_RXFTH_NON 0x0000E000UL   /* no FIFO threshold: whole frame   */

/* --- Transmit configuration (0x40) --------------------------------------- */
#define RTL_TCR_CLRABT    0x00000001UL   /* retransmit after an abort        */
#define RTL_TCR_MXDMA_2K  0x00000700UL   /* burst size: 2048 bytes           */
#define RTL_TCR_IFG_STD   0x03000000UL   /* standard interframe gap          */

/* --- Transmit status / command (0x10 + n*4) ------------------------------ */
#define RTL_TSD_SIZE      0x00001FFFUL   /* byte count of the frame          */
#define RTL_TSD_OWN       0x00002000UL   /* 1 = the card is done with it     */
#define RTL_TSD_TUN       0x00004000UL   /* FIFO underrun                    */
#define RTL_TSD_TOK       0x00008000UL   /* transmit finished successfully   */
#define RTL_TSD_TABT      0x40000000UL   /* aborted, transmitter is stuck    */

/* Early transmit threshold, in units of 32 bytes, at bits 16..21. 48 * 32 =
*  1536 bytes is more than any frame we send, so the card only starts on the
*  wire once the whole frame sits in its FIFO. That trades a little latency
*  for never underrunning. */
#define RTL_TSD_THRESH    (48UL << 16)

/* --- Header the card puts in front of every received frame ---------------- */
#define RTL_RX_ROK        0x0001 /* the frame is intact                      */
#define RTL_RX_HDR_LEN    4      /* status word + length word                */
#define RTL_RX_EARLY      0xFFF0 /* length while the DMA is still running    */

/* --- Ring geometry --------------------------------------------------------
*
*  The card offers 8K, 16K, 32K or 64K, always "plus 16" bytes, selected by
*  RBLEN. 32 KiB is the middle ground taken here: it holds roughly twenty
*  full sized frames, so a burst survives the time between two interrupts,
*  and it still costs only nine frames of physical memory.
*
*  On top of that come two additions:
*
*    - the 16 bytes the hardware always adds, and
*    - 1536 bytes of slack, because RCR_WRAP is left CLEAR below, i.e. the
*      card is allowed to run a frame that starts near the end of the ring
*      straight past the end instead of splitting it. That is what makes the
*      read path a plain linear copy with no wrap case in it -- but only if
*      the memory behind the ring really belongs to us. Without the slack
*      the card would scribble up to 1.5 KiB into whatever follows.
*
*  32768 + 16 + 1536 = 34320 bytes, which is 9 frames = 36864 bytes. */
#define RTL_RX_RING       32768UL
#define RTL_RX_PAD        16UL
#define RTL_RX_SLACK      1536UL
#define RTL_RX_ALLOC      (RTL_RX_RING + RTL_RX_PAD + RTL_RX_SLACK)
#define RTL_RX_FRAMES     ((RTL_RX_ALLOC + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE)

/* Frames handled in one interrupt. The handler runs with interrupts off and
*  the whole machine waits for it, so the drain loop is bounded even if the
*  card keeps the ring non-empty. Anything left behind is picked up by the
*  next ROK, and at 64 frames per interrupt that never happens in practice. */
#define RTL_RX_BUDGET     64

/* Four transmit slots of 2 KiB each: 8192 bytes = 2 frames, page aligned, so
*  every slot address is dword aligned as the card requires. */
#define RTL_TX_SLOTS      4
#define RTL_TX_SLOT_SIZE  2048UL
#define RTL_TX_ALLOC      (RTL_TX_SLOTS * RTL_TX_SLOT_SIZE)
#define RTL_TX_FRAMES     (RTL_TX_ALLOC / PMM_FRAME_SIZE)

/* Shortest legal ethernet frame without the CRC. Shorter payloads are padded
*  with zeroes -- the card does not do it for us. */
#define RTL_TX_MIN        60UL

/* Longest frame we accept from the stack: header plus MTU, without CRC. The
*  card appends the CRC itself. */
#define RTL_TX_MAX        (ETH_HDR_LEN + ETH_MTU)

/* Bounded wait for the reset bit, counted in register reads. One read of a
*  PCI I/O port costs on the order of a microsecond, so this is roughly a
*  tenth of a second -- a reset takes microseconds. A card that is not there
*  or is wedged must not park the kernel in a spin loop. */
#define RTL_RESET_TIMEOUT 100000

/* --- State --------------------------------------------------------------- */
static uint16_t rtl_io;                  /* I/O base from BAR0               */
static uint8_t  rtl_irq;                 /* interrupt line PCI reported      */
static uint8_t  rtl_mac_addr[ETH_ALEN];
static int      rtl_ready;

static uint32_t rtl_rx_phys;             /* what the card is told            */
static uint8_t *rtl_rx_virt;             /* how the kernel reads the same    */
static uint32_t rtl_rx_pos;              /* read offset inside the ring      */

static uint32_t rtl_tx_phys;
static uint8_t *rtl_tx_virt;
static int      rtl_tx_next;             /* next slot to fill, round robin   */

static char     rtl_info_buf[96];
static const char rtl_no_card[] = "RTL8139: not present";

/* --- Port access ----------------------------------------------------------
*
*  system.h declares an inportw() that nothing in the kernel defines, and
*  there is no 32 bit accessor at all. src/drivers/block/ata.c ran into the same gap and
*  kept its helpers local; a header this file does not own is no place to
*  fix that, so the four below are static and named apart from the global
*  ones to avoid any collision the day they do appear. */

static uint16_t rtl_inw(uint16_t port)
{
    uint16_t rv;
    __asm__ __volatile__ ("inw %1, %0" : "=a" (rv) : "dN" (port));
    return rv;
}

static void rtl_outw(uint16_t port, uint16_t value)
{
    __asm__ __volatile__ ("outw %0, %1" : : "a" (value), "dN" (port));
}

static uint32_t rtl_inl(uint16_t port)
{
    uint32_t rv;
    __asm__ __volatile__ ("inl %1, %0" : "=a" (rv) : "dN" (port));
    return rv;
}

static void rtl_outl(uint16_t port, uint32_t value)
{
    __asm__ __volatile__ ("outl %0, %1" : : "a" (value), "dN" (port));
}

/* --- The description string -----------------------------------------------
*
*  Composed by hand into a fixed buffer, the same way ata.c builds its error
*  messages: there is no snprintf() here. */

static int rtl_put_str(int pos, const char *s)
{
    while(*s != '\0' && pos < (int)sizeof(rtl_info_buf) - 1)
    {
        rtl_info_buf[pos++] = *s++;
    }
    return pos;
}

static int rtl_put_hex(int pos, uint32_t value, int digits)
{
    static const char digit[] = "0123456789ABCDEF";
    int shift;

    shift = (digits - 1) * 4;
    while(shift >= 0 && pos < (int)sizeof(rtl_info_buf) - 1)
    {
        rtl_info_buf[pos++] = digit[(value >> shift) & 0x0FUL];
        shift -= 4;
    }
    return pos;
}

static int rtl_put_dec(int pos, uint32_t value)
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

    while(n > 0 && pos < (int)sizeof(rtl_info_buf) - 1)
    {
        rtl_info_buf[pos++] = tmp[--n];
    }
    return pos;
}

static void rtl_build_info(void)
{
    int pos;
    int i;

    pos = rtl_put_str(0, "RTL8139 at I/O 0x");
    pos = rtl_put_hex(pos, (uint32_t)rtl_io, 4);
    pos = rtl_put_str(pos, ", IRQ ");
    pos = rtl_put_dec(pos, (uint32_t)rtl_irq);
    pos = rtl_put_str(pos, ", MAC ");

    for(i = 0; i < ETH_ALEN; i++)
    {
        if(i != 0)
        {
            pos = rtl_put_str(pos, ":");
        }
        pos = rtl_put_hex(pos, (uint32_t)rtl_mac_addr[i], 2);
    }

    rtl_info_buf[pos] = '\0';
}

/* --- Interrupt controller --------------------------------------------------
*
*  irq_remap() in irq.c opens both masks completely, so in the normal case
*  there is nothing left to do here. Clearing the one bit anyway costs two
*  port accesses and makes the driver independent of that, and it documents
*  the part that is easy to get wrong: a line above 7 hangs off the SLAVE
*  PIC, so its mask lives at 0xA1 -- and the slave only reaches the CPU
*  through the cascade on IRQ 2, which has to be open in the master mask as
*  well. The matching end of interrupt for the slave is already sent by
*  irq_handler() for every vector from 40 upwards, so the handler installed
*  below has nothing to acknowledge at the PIC itself. */

static void rtl_unmask_irq(uint8_t irq)
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

/* --- Receive ---------------------------------------------------------------
*
*  The card writes into the ring continuously and puts a four byte header in
*  front of every frame: a status word and a length. The length counts the
*  CRC the card checked and no longer needs, so the stack gets len - 4.
*
*  The read pointer is ours; CAPR tells the card how far we have consumed.
*  Everything up to CAPR is fair game for the card to overwrite, which is
*  why CAPR is written only AFTER net_receive() has returned. That also
*  makes it safe to hand the stack a pointer straight into the ring instead
*  of copying the frame out first. */

/* Puts the receiver back to a known state. Reached when the header does not
*  make sense -- trusting a corrupt length would walk the read pointer off
*  the ring and turn one bad frame into a permanently broken driver. */
static void rtl_rx_reset(void)
{
    uint8_t cr;

    cr = inportb(rtl_io + RTL_CR);
    outportb(rtl_io + RTL_CR, (uint8_t)(cr & ~RTL_CR_RE));

    rtl_rx_pos = 0;
    rtl_outl(rtl_io + RTL_RBSTART, rtl_rx_phys);
    rtl_outw(rtl_io + RTL_CAPR, (uint16_t)(0 - RTL_RX_PAD));

    outportb(rtl_io + RTL_CR, (uint8_t)(cr | RTL_CR_RE));
}

static void rtl_receive(void)
{
    const uint8_t *hdr;
    uint16_t status;
    uint16_t length;
    int budget;

    budget = RTL_RX_BUDGET;

    while((inportb(rtl_io + RTL_CR) & RTL_CR_BUFE) == 0)
    {
        if(--budget < 0)
        {
            break;
        }

        /* Read byte by byte: the header is dword aligned, but this way the
        *  code does not depend on it and no aliasing question arises. */
        hdr    = rtl_rx_virt + rtl_rx_pos;
        status = (uint16_t)(hdr[0] | (hdr[1] << 8));
        length = (uint16_t)(hdr[2] | (hdr[3] << 8));

        /* The card publishes this length while it is still copying the
        *  frame in. Nothing to do but come back later. */
        if(length == RTL_RX_EARLY)
        {
            break;
        }

        /* Check the status BEFORE trusting the length. A frame can be
        *  flagged bad, and a length outside these bounds cannot be a frame
        *  at all -- both mean the ring is no longer describable. */
        if((status & RTL_RX_ROK) == 0 ||
           length < (uint16_t)(ETH_HDR_LEN + 4) ||
           length > (uint16_t)ETH_FRAME_MAX)
        {
            rtl_rx_reset();
            return;
        }

        /* The frame body follows the header contiguously, even when it
        *  crosses the end of the ring: RCR_WRAP is clear, so the card wrote
        *  the tail into the slack allocated behind the ring. */
        net_receive(hdr + RTL_RX_HDR_LEN, (uint32_t)length - 4UL);

        /* Advance dword aligned, then fold back into the ring. The ring
        *  length is a power of two, so the mask is the modulo. */
        rtl_rx_pos = (rtl_rx_pos + (uint32_t)length + RTL_RX_HDR_LEN + 3UL) & ~3UL;
        rtl_rx_pos &= (RTL_RX_RING - 1UL);

        /* And the quirk: CAPR reads and writes 16 less than the offset it
        *  actually denotes. The subtraction is done in 16 bits and is meant
        *  to wrap around for the first sixteen bytes of the ring. */
        rtl_outw(rtl_io + RTL_CAPR, (uint16_t)(rtl_rx_pos - RTL_RX_PAD));
    }
}

/* --- Transmit ------------------------------------------------------------- */

/* Called for TOK and TER. There is nothing to reap -- the OWN bit the send
*  path looks at is set by the hardware itself -- but an abort after too many
*  collisions stops the transmitter until CLRABT is written, and that would
*  otherwise be a card that silently sends nothing more. */
static void rtl_transmit_done(void)
{
    uint32_t tsd;
    int i;

    for(i = 0; i < RTL_TX_SLOTS; i++)
    {
        tsd = rtl_inl((uint16_t)(rtl_io + RTL_TSD0 + i * 4));
        if((tsd & RTL_TSD_TABT) != 0)
        {
            rtl_outl(rtl_io + RTL_TCR,
                     rtl_inl(rtl_io + RTL_TCR) | RTL_TCR_CLRABT);
        }
    }
}

/* --- Interrupt handler -----------------------------------------------------
*
*  Order matters twice over. The status is acknowledged in ISR by writing the
*  bits back BEFORE the work is done: the card keeps the line asserted as
*  long as a bit stands, and acknowledging afterwards races with the next
*  frame, which would arrive, set ROK again and then have its bit wiped by
*  our late write -- a receive that stops for no visible reason.
*
*  And the whole thing is bounded. This runs with interrupts disabled; a loop
*  that keeps up with a busy card would never give the rest of the system a
*  turn, so at most four rounds are taken and the drain itself has a budget. */
static void rtl_interrupt(struct regs *r)
{
    uint16_t status;
    int rounds;

    (void)r;    /* the register frame is of no interest here */

    for(rounds = 0; rounds < 4; rounds++)
    {
        status = rtl_inw(rtl_io + RTL_ISR);
        if(status == 0)
        {
            break;      /* not ours -- the line may be shared */
        }

        rtl_outw(rtl_io + RTL_ISR, status);

        /* An overflow is handled like a normal receive: draining the ring
        *  and moving CAPR on is exactly what frees the card up again. */
        if((status & (RTL_INT_ROK | RTL_INT_RER |
                      RTL_INT_RXOVW | RTL_INT_FOVW)) != 0)
        {
            rtl_receive();
        }

        if((status & (RTL_INT_TOK | RTL_INT_TER)) != 0)
        {
            rtl_transmit_done();
        }

        if((status & RTL_INT_SYSERR) != 0)
        {
            /* A PCI bus error. Nothing sensible can be done from interrupt
            *  context; the receiver is put back into a defined state so the
            *  ring at least stays consistent. */
            rtl_rx_reset();
        }
    }
}

/* --- What this driver offers the netdev layer -----------------------------
*
*  The four functions behind these pointers are the four that used to be
*  declared in rtl8139.h and called by name from net.c and the shell. Nothing
*  about what they do changed; what changed is that they are static and are
*  reached only through here, so that the stack above never learns which chip
*  carried its frame. rtl8139.h says the same thing at more length.
*
*  Declared here rather than defined here because the transmit path belongs at
*  the bottom of the file next to the ring it fills, and the struct has to
*  exist before rtl8139_init() can hand it over. netdev_ops has no presence
*  member, and that absence is the point: being registered IS being present,
*  so the old rtl8139_present() has no counterpart and is gone rather than
*  hidden. */
static const uint8_t *rtl8139_mac(void);
static const char    *rtl8139_info(void);
static int            rtl8139_send(const void *frame, uint32_t len);

static const netdev_ops rtl_netdev_ops =
{
    "RTL8139",
    rtl8139_send,
    rtl8139_mac,
    rtl8139_info
};

/* --- Bring-up ------------------------------------------------------------- */

/* Both buffers come from the frame allocator: contiguous, physical, and on a
*  32 bit machine necessarily below 4 GiB. The extra check is that the block
*  is inside the directly mapped window, because P2V() -- the only way the
*  kernel can touch it afterwards -- is defined nowhere else. */
static int rtl_alloc_buffers(void)
{
    uint32_t phys;

    phys = (uint32_t)pmm_alloc_frames((uint32_t)RTL_RX_FRAMES);
    if(phys == 0)
    {
        printf("RTL8139: no contiguous memory for the receive ring\n");
        return -1;
    }
    if(phys > (uint32_t)DIRECT_MAP_LIMIT - (RTL_RX_ALLOC - 1UL))
    {
        pmm_free_frames((void *)phys, (uint32_t)RTL_RX_FRAMES);
        printf("RTL8139: receive ring outside the direct mapping\n");
        return -1;
    }
    rtl_rx_phys = phys;
    rtl_rx_virt = (uint8_t *)P2V(phys);
    memset(rtl_rx_virt, 0, (size_t)RTL_RX_ALLOC);

    phys = (uint32_t)pmm_alloc_frames((uint32_t)RTL_TX_FRAMES);
    if(phys == 0 || phys > (uint32_t)DIRECT_MAP_LIMIT - (RTL_TX_ALLOC - 1UL))
    {
        if(phys != 0)
        {
            pmm_free_frames((void *)phys, (uint32_t)RTL_TX_FRAMES);
        }
        pmm_free_frames((void *)rtl_rx_phys, (uint32_t)RTL_RX_FRAMES);
        rtl_rx_phys = 0;
        rtl_rx_virt = 0;
        printf("RTL8139: no memory for the transmit buffers\n");
        return -1;
    }
    rtl_tx_phys = phys;
    rtl_tx_virt = (uint8_t *)P2V(phys);
    memset(rtl_tx_virt, 0, (size_t)RTL_TX_ALLOC);

    return 0;
}

/* Puts the card back to sleep and gives its memory back. Reached from exactly
*  one place: a second RTL8139 in a machine that already has a card in use.
*
*  It is not optional tidying. The card is fully up by the time registration is
*  asked for -- netdev.h requires that, since a driver registers when it is up
*  and not when it is found -- so a card that then stands down is a card with
*  its receiver enabled and a DMA engine writing arriving frames into a ring
*  that nothing will ever read again, and whose frames are about to be handed
*  back to the allocator. Leaving it running would be a slow corruption of
*  whatever gets those frames next, with no symptom pointing anywhere near the
*  network.
*
*  Order matters: the command register goes to zero FIRST, which stops both
*  engines and therefore all DMA, and only then is the memory released. The
*  interrupt mask is cleared before that so a frame already in flight cannot
*  raise a line whose handler is not installed yet -- see rtl8139_init(), which
*  installs it only after registration succeeded, precisely so that this path
*  has no handler to remove. That matters more than it sounds: a second card
*  sharing an interrupt line with the first would otherwise unhook the card
*  that IS in use on its way out. */
static void rtl_stand_down(void)
{
    rtl_outw(rtl_io + RTL_IMR, 0);
    outportb(rtl_io + RTL_CR, 0);

    if(rtl_rx_phys != 0)
    {
        pmm_free_frames((void *)rtl_rx_phys, (uint32_t)RTL_RX_FRAMES);
        rtl_rx_phys = 0;
        rtl_rx_virt = 0;
    }
    if(rtl_tx_phys != 0)
    {
        pmm_free_frames((void *)rtl_tx_phys, (uint32_t)RTL_TX_FRAMES);
        rtl_tx_phys = 0;
        rtl_tx_virt = 0;
    }

    rtl_ready = 0;
}

int rtl8139_init(void)
{
    const pci_device *dev;
    int timeout;
    int i;

    if(rtl_ready)
    {
        return 0;
    }

    dev = pci_find(RTL_VENDOR_ID, RTL_DEVICE_ID);
    if(dev == 0)
    {
        /* No Realtek on this bus, which is the ordinary case on most machines
        *  and says nothing about whether the machine has a network: another
        *  driver may find its own card, and net_init() asks netdev_present()
        *  rather than asking any driver. Silent on purpose -- a line here
        *  would appear on every boot of every machine without this chip. */
        return -1;
    }

    rtl_io = pci_io_base(dev, 0);
    if(rtl_io == 0)
    {
        printf("RTL8139: BAR0 is not an I/O region\n");
        return -1;
    }

    rtl_irq = dev->irq;
    if(rtl_irq > 15)
    {
        printf("RTL8139: no usable interrupt line (%d)\n", (int)rtl_irq);
        return -1;
    }

    /* I/O space so the registers answer, and bus mastering so the card may
    *  reach memory on its own. The second one is the whole game. */
    pci_enable(dev, (uint16_t)(PCI_CMD_IO | PCI_CMD_MASTER));

    /* Out of sleep first -- a card in a low power state ignores the reset. */
    outportb(rtl_io + RTL_CONFIG1, 0x00);

    /* Software reset, and then WAIT for the bit to clear rather than
    *  assuming it has. Bounded, so a card that never answers costs a tenth
    *  of a second instead of the machine. */
    outportb(rtl_io + RTL_CR, RTL_CR_RST);
    timeout = RTL_RESET_TIMEOUT;
    while((inportb(rtl_io + RTL_CR) & RTL_CR_RST) != 0)
    {
        if(--timeout <= 0)
        {
            printf("RTL8139: reset did not complete\n");
            return -1;
        }
    }

    for(i = 0; i < ETH_ALEN; i++)
    {
        rtl_mac_addr[i] = inportb((unsigned short)(rtl_io + RTL_IDR0 + i));
    }

    if(rtl_alloc_buffers() != 0)
    {
        return -1;
    }

    /* The ring, as a physical address -- this is the register the MMU knows
    *  nothing about. */
    rtl_outl(rtl_io + RTL_RBSTART, rtl_rx_phys);
    rtl_rx_pos = 0;
    rtl_outw(rtl_io + RTL_CAPR, (uint16_t)(0 - RTL_RX_PAD));

    rtl_tx_next = 0;

    /* Accept what is addressed to us, plus multicast and broadcast -- ARP
    *  would not work without the last one. Promiscuous mode and errored
    *  frames stay off. WRAP is deliberately CLEAR: the card may write a
    *  frame past the end of the ring into the slack behind it, which is
    *  what keeps the read path free of a wrap case. */
    rtl_outl(rtl_io + RTL_RCR,
             RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_AB |
             RTL_RCR_MXDMA_UNL | RTL_RCR_RBLEN_32K | RTL_RCR_RXFTH_NON);

    rtl_outl(rtl_io + RTL_TCR, RTL_TCR_IFG_STD | RTL_TCR_MXDMA_2K);

    /* The engines on, and the interrupt mask deliberately still shut. The card
    *  now receives into the ring and would take a frame from rtl8139_send(),
    *  which is what netdev.h means by a driver registering when it is up: the
    *  transmit path needs no interrupt at all, because the OWN bit it waits on
    *  is set by the hardware. What is still missing is only the notification
    *  that something arrived, and nothing above this file can ask for that
    *  before net_init() has run. */
    outportb(rtl_io + RTL_CR, (uint8_t)(RTL_CR_TE | RTL_CR_RE));

    rtl_ready = 1;
    rtl_build_info();

    /* And the offer, which is the point at which this driver stops being the
    *  network and starts being one candidate for it. Everything netdev_ops
    *  promises holds of this card by now -- it answers with its own MAC, it
    *  takes a frame, and it can describe itself -- which is why the offer is
    *  made here and not at the top of the function where it would have been
    *  cheaper to make and false to believe.
    *
    *  A refusal means another driver registered first, on a machine with two
    *  cards. netdev.h is explicit that this is not a failure of this card, so
    *  it is worded as what it is; what it IS is a card that must now stop,
    *  since the alternative is a receiver filling a ring nobody drains out of
    *  memory that is about to be given back. */
    if(netdev_register(&rtl_netdev_ops) != 0)
    {
        printf("%s\n", rtl_info_buf);
        printf("RTL8139: a network card is already in use -- standing down\n");
        rtl_stand_down();
        return -1;
    }

    /* Only now the interrupt, and in this order so that everything the card
    *  could report already has somewhere to land: the handler, then the line,
    *  then the mask that lets the card pull it.
    *
    *  After the registration rather than before it, which is the part worth
    *  knowing. A second card that stands down has then never touched the
    *  interrupt table -- and on a machine where both cards landed on one line,
    *  installing first would mean unhooking the card that is actually in use
    *  on the way back out. */
    irq_install_handler((int)rtl_irq, rtl_interrupt);
    rtl_unmask_irq(rtl_irq);
    rtl_outw(rtl_io + RTL_IMR, RTL_INT_MASK);

    printf("%s\n", rtl_info_buf);
    printf("RTL8139: %d KiB receive ring, %d bytes allocated, %d transmit slots\n",
           (int)(RTL_RX_RING / 1024UL), (int)RTL_RX_ALLOC, RTL_TX_SLOTS);

    return 0;
}

static const uint8_t *rtl8139_mac(void)
{
    return rtl_mac_addr;
}

static const char *rtl8139_info(void)
{
    if(!rtl_ready)
    {
        return rtl_no_card;
    }
    return rtl_info_buf;
}

/* The frame is copied into a slot buffer this driver owns. The caller's
*  buffer is of no use to the card: it has to be physically contiguous, it
*  has to be reachable by physical address, and it has to stay untouched
*  until the card has fetched it -- none of which the stack promises about
*  whatever it hands down here.
*
*  Only the slot at rtl_tx_next is examined. Slots are filled in order and
*  the card works them in the same order, so that one is always the oldest
*  outstanding: if it is still busy, so are all the others. */
static int rtl8139_send(const void *frame, uint32_t len)
{
    uint8_t *buf;
    uint16_t tsd_port;
    uint32_t slot;
    uint32_t size;

    if(!rtl_ready)
    {
        return -1;
    }
    if(frame == 0 || len == 0 || len > RTL_TX_MAX)
    {
        return -1;
    }

    slot     = (uint32_t)rtl_tx_next;
    tsd_port = (uint16_t)(rtl_io + RTL_TSD0 + slot * 4UL);

    /* OWN reads back as 1 once the card is done with the slot -- and it is 1
    *  after a reset, so an untouched slot counts as free. */
    if((rtl_inl(tsd_port) & RTL_TSD_OWN) == 0)
    {
        return -1;      /* all four in flight; the caller retries */
    }

    buf  = rtl_tx_virt + slot * RTL_TX_SLOT_SIZE;
    size = len;

    memcpy(buf, frame, (size_t)len);

    /* Pad short frames with zeroes. The wire minimum is 60 bytes plus the
    *  CRC the card appends, and the card does not pad by itself. */
    if(size < RTL_TX_MIN)
    {
        memset(buf + size, 0, (size_t)(RTL_TX_MIN - size));
        size = RTL_TX_MIN;
    }

    /* Address first, and physical again. Writing the descriptor clears OWN
    *  and starts the transfer, so it has to come last. */
    rtl_outl((uint16_t)(rtl_io + RTL_TSAD0 + slot * 4UL),
             rtl_tx_phys + slot * RTL_TX_SLOT_SIZE);
    rtl_outl(tsd_port, RTL_TSD_THRESH | (size & RTL_TSD_SIZE));

    rtl_tx_next = (int)((slot + 1UL) & (RTL_TX_SLOTS - 1UL));

    return 0;
}
