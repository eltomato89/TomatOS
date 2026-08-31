/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: UHCI host controller driver -- the bottom third of the USB stack
*
*  See src/include/uhci.h for what this is and src/include/usb.h for the line
*  it sits under. What follows is what the implementation had to decide, and
*  the reasoning is written down at length because almost every one of these
*  decisions fails SILENTLY when it is wrong: a controller that answers every
*  register read and moves no data, a device that ignores every second packet,
*  a keyboard that reports itself broken on every machine.
*
*  FOUR THINGS DECIDE WHETHER THIS WORKS AT ALL.
*
*    - THE BIOS MAY STILL OWN THE CONTROLLER. A PC BIOS emulates a PS/2
*      keyboard out of a USB one by trapping port 60h/64h accesses and driving
*      the controller itself from SMM. That trap is armed in PCI config space,
*      not in the controller's registers, so a driver that resets the hardware
*      and starts scheduling without disarming it leaves two drivers on one
*      controller, each rewriting the other's frame list. Disarming it is one
*      config write and is the first thing uhci_init() does; see
*      uhci_release_bios().
*
*    - BUS MASTERING AND PHYSICAL ADDRESSES. The controller reads the frame
*      list, the queue heads and the transfer descriptors ITSELF, by DMA. They
*      therefore live at physical addresses out of pmm_alloc_frames() -- which
*      hands out physical addresses directly and does not need V2P() -- and
*      every kernel access to the same memory goes through P2V(). src/rtl8139.c
*      says the same thing about its rings and is the model this file follows.
*      Without PCI_CMD_MASTER the controller reads nothing and reports nothing.
*
*    - ALIGNMENT IS PART OF THE ADDRESS. The low bits of every link pointer are
*      flags: bit 0 terminates the list, bit 1 says the target is a queue head
*      rather than a transfer descriptor, bit 2 asks for depth-first
*      processing. So the frame list must be 4 KiB aligned and descriptors 16
*      byte aligned not because unaligned would be slow, but because an
*      unaligned address IS a different address with different flags. Frames
*      from the pmm are 4 KiB aligned, which makes both hold by construction;
*      uhci_alloc() checks it anyway rather than assuming.
*
*    - THE CONTROLLER IS WALKING THE LIST WHILE IT IS BUILT. The ordering rules
*      that follow from that are set out above uhci_publish() and are the
*      reason anything here looks overcautious.
*
*  WHAT IS NOT HERE. Isochronous transfers (they need bandwidth reservation and
*  nothing in this kernel wants audio), suspend and resume, and hubs -- which
*  is a usb.c question, not a controller one, but it means the two root ports
*  are the whole bus.
*/

#include <system.h>
#include <mm.h>
#include <vmm.h>
#include <pci.h>
#include <usb.h>
#include <uhci.h>

/* --- Identity on the PCI bus ---------------------------------------------
*
*  Class 0x0C subclass 0x03 is "USB host controller" and is shared by all four
*  incompatible interfaces. The prog-if byte is what separates them, and it is
*  not a nicety: an OHCI controller answers the same class match and has its
*  registers in MEMORY space with a completely different layout, so a driver
*  that matched on class and subclass alone would write a frame list pointer
*  into somebody else's register file. */
#define UHCI_PCI_CLASS      0x0C
#define UHCI_PCI_SUBCLASS   0x03
#define UHCI_PCI_PROGIF     0x00

/* Legacy support register, PCI config offset 0xC0. Two bit groups matter:
*
*    0x00F0  the four "trap on a 60h/64h read or write" enables. These are the
*            PS/2 keyboard emulation: with any of them set, an access to the
*            PS/2 data or command port raises an SMI and the BIOS services it
*            by talking to THIS controller behind the kernel's back.
*    0x2000  routes the controller's interrupt to a PCI interrupt line.
*
*  The remaining bits 0x8F00 are write-one-to-clear status. Writing 0x8F00 back
*  therefore acknowledges every pending status bit and writes a zero into every
*  enable in one go, which is exactly the handoff: emulation off, SMI traps
*  off, interrupt routing off.
*
*  Clearing 0x2000 as well is deliberate and not collateral damage -- this
*  driver polls (see uhci_wait_queue()) and specifically does not want the
*  controller asserting a shared PCI interrupt line. */
#define UHCI_PCI_LEGSUP     0xC0
#define UHCI_LEGSUP_CLEAR   0x8F00
#define UHCI_LEGSUP_TRAPS   0x00F0
#define UHCI_LEGSUP_PIRQ    0x2000

/* --- Registers, relative to the I/O base ----------------------------------
*
*  UHCI is unusual in living in I/O space rather than memory. Width matters:
*  everything below is a 16 bit register except FRBASEADD, which is 32, and
*  SOFMOD, which is 8. Accessing one with the wrong width does not fault, it
*  reads or writes nonsense. */
#define UHCI_USBCMD         0x00   /* word:  command                          */
#define UHCI_USBSTS         0x02   /* word:  status, write 1 to clear         */
#define UHCI_USBINTR        0x04   /* word:  interrupt enables                */
#define UHCI_FRNUM          0x06   /* word:  current frame, 11 bits           */
#define UHCI_FRBASEADD      0x08   /* dword: frame list, PHYSICAL             */
#define UHCI_SOFMOD         0x0C   /* byte:  start-of-frame timing            */
#define UHCI_PORTSC1        0x10   /* word:  first root port                  */

/* The base address register. The UHCI specification fixes it at PCI config
*  offset 0x20, i.e. BAR4, and a PIIX3 really does put it there while leaving
*  BARs 0..3 at zero. Scanning for "the first I/O BAR" happens to find the same
*  register on that chip and would find the wrong one on a device that decodes
*  something else lower down, so BAR4 is tried first and the scan is only the
*  fallback. */
#define UHCI_BAR            4

/* --- USBCMD --------------------------------------------------------------- */
#define UHCI_CMD_RS         0x0001 /* run/stop -- the bit that is forgotten   */
#define UHCI_CMD_HCRESET    0x0002 /* host controller reset, self clearing    */
#define UHCI_CMD_GRESET     0x0004 /* global reset, drives USB reset on ports */
#define UHCI_CMD_EGSM       0x0008 /* enter global suspend                    */
#define UHCI_CMD_FGR        0x0010 /* force global resume                     */
#define UHCI_CMD_SWDBG      0x0020 /* software debug, single step             */
#define UHCI_CMD_CF         0x0040 /* "configured", advisory, for the BIOS    */
#define UHCI_CMD_MAXP       0x0080 /* 64 rather than 32 byte packets late in  */
                                   /* a frame                                 */

/* --- USBSTS, all write-one-to-clear --------------------------------------- */
#define UHCI_STS_USBINT     0x0001 /* a TD with IOC finished, or a short one  */
#define UHCI_STS_ERRINT     0x0002 /* a TD finished with an error             */
#define UHCI_STS_RESUME     0x0004 /* a device asked to resume the bus        */
#define UHCI_STS_HSE        0x0008 /* PCI bus error -- the HC has halted      */
#define UHCI_STS_HCPE       0x0010 /* the HC choked on a descriptor and halted*/
#define UHCI_STS_HALTED     0x0020 /* the HC is not running                   */
#define UHCI_STS_ALL        0x003F
#define UHCI_STS_FATAL      (UHCI_STS_HSE | UHCI_STS_HCPE)

/* --- FRNUM ----------------------------------------------------------------
*  Eleven bits, so it counts 0..1023 and wraps once a second. See
*  uhci_frame_sample() for why that matters. */
#define UHCI_FRNUM_MASK     0x07FF
#define UHCI_FRAME_COUNT    1024

/* --- PORTSC ---------------------------------------------------------------
*
*  Bits 1 and 3 are write-one-to-clear: writing a one ACKNOWLEDGES the change
*  rather than causing it. Any read-modify-write of this register that does not
*  mask them out first silently swallows a connect event somebody else was
*  waiting for, which is a device that is never noticed. uhci_port_state() is
*  the only way this file reads the register for that reason.
*
*  Bit 7 reads as one on a port that exists and is how the number of root ports
*  is found -- the specification does not fix it at two. */
#define UHCI_PORT_CCS       0x0001 /* a device is connected                   */
#define UHCI_PORT_CSC       0x0002 /* connection changed (write 1 to clear)   */
#define UHCI_PORT_PE        0x0004 /* port enabled                            */
#define UHCI_PORT_PEDC      0x0008 /* enable changed (write 1 to clear)       */
#define UHCI_PORT_LINE      0x0030 /* raw D+/D- state                         */
#define UHCI_PORT_RD        0x0040 /* resume detected                         */
#define UHCI_PORT_ALWAYS1   0x0080 /* reserved, reads 1 on a real port        */
#define UHCI_PORT_LSDA      0x0100 /* the device is LOW speed                 */
#define UHCI_PORT_RESET     0x0200 /* drive USB reset on this port            */
#define UHCI_PORT_SUSPEND   0x1000
#define UHCI_PORT_CHANGE    (UHCI_PORT_CSC | UHCI_PORT_PEDC)

#define UHCI_MAX_PORTS      8

/* --- Link pointers --------------------------------------------------------
*
*  Every link -- frame list entry, queue head horizontal, queue head element,
*  transfer descriptor next -- is a physical address with these three flags in
*  the bits alignment leaves free.
*
*  DEPTH FIRST is the one worth understanding. Without it the controller runs
*  ONE descriptor of a queue and then moves on to the next queue, coming back
*  to this one in the following frame: a 4 KiB bulk read in 64 byte packets
*  would take 64 frames, i.e. 64 milliseconds, and the bus would be idle for
*  almost all of them. With it the controller follows the chain immediately and
*  the same read finishes inside two or three frames. It is set on every link
*  WITHIN a transfer and never between queues. */
#define UHCI_LINK_TERM      0x00000001UL
#define UHCI_LINK_QH        0x00000002UL
#define UHCI_LINK_VF        0x00000004UL

/* --- Transfer descriptor, control and status word ------------------------- */
#define UHCI_TD_ACTLEN      0x000007FFUL /* bytes moved, encoded as len-1     */
#define UHCI_TD_BITSTUFF    0x00020000UL
#define UHCI_TD_CRCTIMEO    0x00040000UL /* no answer, or a corrupt one       */
#define UHCI_TD_NAK         0x00080000UL /* "not now" -- NOT an error         */
#define UHCI_TD_BABBLE      0x00100000UL /* the device would not stop talking */
#define UHCI_TD_DBUFERR     0x00200000UL /* our buffer under/overran          */
#define UHCI_TD_STALLED     0x00400000UL /* "I do not support that"           */
#define UHCI_TD_ACTIVE      0x00800000UL /* the controller still owns this TD */
#define UHCI_TD_IOC         0x01000000UL /* interrupt on completion           */
#define UHCI_TD_ISO         0x02000000UL
#define UHCI_TD_LS          0x04000000UL /* the device is low speed           */
#define UHCI_TD_SPD         0x20000000UL /* stop the queue on a short packet  */

/* The error counter, bits 27..28. Three attempts before the controller gives
*  up and sets CRCTIMEO; zero would mean "never give up", which turns a device
*  that has come unplugged mid-transfer into a descriptor that stays active
*  forever and a transfer that only the timeout ends. A NAK does not decrement
*  it -- that is the hardware distinction this driver leans on everywhere. */
#define UHCI_TD_CERR3       0x18000000UL

/* Everything that is a genuine failure. NAK is deliberately not in here, and
*  neither is a short packet -- a short packet is not a bit at all, it is the
*  ABSENCE of every bit in this mask on a descriptor that moved fewer bytes
*  than it asked for. */
#define UHCI_TD_ERRORS      (UHCI_TD_BITSTUFF | UHCI_TD_CRCTIMEO |  \
                             UHCI_TD_BABBLE | UHCI_TD_DBUFERR |     \
                             UHCI_TD_STALLED)

/* --- Transfer descriptor, token word -------------------------------------- */
#define UHCI_PID_IN         0x69UL
#define UHCI_PID_OUT        0xE1UL
#define UHCI_PID_SETUP      0x2DUL

/* --- Geometry -------------------------------------------------------------
*
*  Three allocations, all from the frame allocator because it is the only one
*  that promises physically contiguous blocks at a known physical address:
*
*    1 frame   the frame list: 1024 entries of 4 bytes, and the hardware wants
*              it 4 KiB aligned, so this is exactly one frame and no rounding.
*    2 frames  queue heads, setup packets and the descriptor pool.
*    3 frames  one bounce buffer per transfer slot.
*
*  24 KiB in total, claimed once at bring-up and never released.
*
*  Layout of the descriptor block:
*
*      0    ..  159   10 queue heads, 16 bytes each
*      160  ..  207   3 setup packet buffers, 16 bytes each
*      208  ..  255   padding, so the descriptor pool starts 256 byte aligned
*      256  .. 8191   248 transfer descriptors, 32 bytes each
*
*  A transfer descriptor is 16 bytes of hardware and 16 bytes of nothing. The
*  padding is not waste: it keeps every descriptor 32 byte aligned, which
*  satisfies the hardware's 16 byte requirement with room to spare and makes
*  the index-to-address arithmetic a shift. */
#define UHCI_QH_COUNT       10
#define UHCI_QH_SIZE        16
#define UHCI_SETUP_OFFSET   160
#define UHCI_SETUP_SIZE     16
#define UHCI_TD_OFFSET      256
#define UHCI_TD_SIZE        32
#define UHCI_DESC_FRAMES    2
#define UHCI_DESC_BYTES     (UHCI_DESC_FRAMES * PMM_FRAME_SIZE)

/* --- The schedule ---------------------------------------------------------
*
*  Eight interrupt queue heads for the intervals 1, 2, 4 ... 128 frames, then
*  control, then bulk. They are chained horizontally longest interval first:
*
*      int128 -> int64 -> ... -> int2 -> int1 -> control -> bulk -> end
*
*  and frame list entry n points INTO that chain at the queue head for the
*  longest interval that is due in frame n, which is decided by the number of
*  trailing zero bits in n. Frame 0 enters at int128 and runs everything; frame
*  1 enters at int1 and runs only the every-frame endpoints, control and bulk;
*  frame 8 enters at int8. Each entry point is a single dword in the frame
*  list, so the whole periodic tree costs 4 KiB and no per-frame work.
*
*  WHY THE ORDER IS THIS WAY ROUND, and what it means with two devices
*  attached: a queue head whose head descriptor is NAKed is abandoned by the
*  controller, which moves on horizontally to the next queue in the same frame.
*  So an idle keyboard -- which NAKs every single poll, forever -- costs the
*  disk behind it one NAK handshake per frame and nothing else. Hang everything
*  off one queue instead and that same idle keyboard is a permanent roadblock
*  in front of the disk: the classic "works alone, starves in pairs" failure,
*  and it is not a crash, it is a machine that gets slower when you plug in a
*  second thing. Interrupt first, control second, bulk last is also the right
*  priority for latency, because interrupt endpoints are the ones with a
*  deadline and bulk is by definition the traffic that can wait. */
#define UHCI_INT_QHS        8
#define UHCI_QH_CONTROL     8
#define UHCI_QH_BULK        9

/* --- Transfer slots -------------------------------------------------------
*
*  A queue head has ONE element pointer, so one queue can carry one transfer at
*  a time. There are three slots, one per queue class, each owning its own
*  region of the descriptor pool and its own bounce buffer, so a control
*  transfer, a bulk transfer and an interrupt poll can be in flight at once and
*  only block each other for the descriptor pool they do not share.
*
*  Two interrupt endpoints -- a keyboard and a mouse -- do serialise against
*  each other, because they contend for the one interrupt slot. Each poll holds
*  it for at most UHCI_INT_TIMEOUT_MS, so two devices are polled alternately at
*  half the rate rather than one of them being locked out; that is a real limit
*  and it is the price of a fixed slot table instead of a descriptor allocator.
*  Lifting it means giving each ENDPOINT its own queue head, which the skeleton
*  above is already shaped for. */
#define UHCI_SLOT_CONTROL   0
#define UHCI_SLOT_BULK      1
#define UHCI_SLOT_INT       2
#define UHCI_SLOT_COUNT     3

/* 80 descriptors per slot, 240 of the 248 the pool holds. Enough for 4096
*  bytes in 64 byte packets plus the setup and status stages of a control
*  transfer. A low speed endpoint uses 8 byte packets, so the same 80
*  descriptors are only 624 bytes there -- which is far more than any
*  descriptor a keyboard or mouse has, and a caller that asks for more gets
*  USB_EINVAL rather than a truncated answer. */
#define UHCI_TDS_PER_SLOT   80
#define UHCI_MAX_TRANSFER   4096
#define UHCI_BOUNCE_FRAMES  UHCI_SLOT_COUNT

/* --- Timeouts -------------------------------------------------------------
*
*  usb.h requires every transfer to be bounded, and these are the bounds. They
*  are milliseconds because a USB frame is a millisecond, so they read directly
*  as "this many frames of patience".
*
*  The interrupt one is different in kind from the other two. It is not a limit
*  on how long a transfer may take, it is how long a poll of an idle endpoint
*  is allowed to sit in the schedule before the answer "nothing yet" is
*  returned. Ten frames is long enough that a keypress arriving at any point in
*  the window is picked up in the same call, and short enough that two devices
*  sharing the interrupt slot are each polled fifty times a second. */
#define UHCI_CONTROL_TIMEOUT_MS  1000
#define UHCI_BULK_TIMEOUT_MS     1000
#define UHCI_INT_TIMEOUT_MS      10
#define UHCI_SLOT_TIMEOUT_MS     2000

/* How often the descriptor is looked at while waiting. One frame: checking
*  more often cannot see anything new, because the controller only touches a
*  descriptor when it runs the frame it hangs off. */
#define UHCI_POLL_MS             1

/* Hardware reset timings. The host controller reset is self clearing and takes
*  microseconds; 100 ms is the bound before it is called dead. The global reset
*  has to be driven for at least 10 ms by the specification, and 50 ms is what
*  every other driver uses because some devices need the longer pulse to notice
*  it at all. A port reset is the same USB reset on one port. */
#define UHCI_HCRESET_TIMEOUT_MS  100
#define UHCI_GRESET_MS           50
#define UHCI_PORT_RESET_MS       50
#define UHCI_PORT_ENABLE_MS      50

/* The fallback delay loop, used only when interrupts are off and the
*  millisecond counter is therefore frozen. One access to the unused POST port
*  0x80 was about a microsecond on the hardware this convention comes from; it
*  is far from that on anything modern or emulated, so this branch is
*  approximate by construction. It exists so that a caller with interrupts
*  disabled gets a delay that is too short rather than a machine that never
*  comes back, and every path that can reach it is one where the alternative is
*  worse. */
#define UHCI_SPINS_PER_MS        1000

#define UHCI_EFLAGS_IF           0x200

/* --- The structures the controller reads ----------------------------------
*
*  volatile throughout, because the controller writes into them by DMA while
*  this code reads them. Without it the compiler is entitled to hoist the
*  status read out of the wait loop and spin on a register copy of a value that
*  changed in memory ten frames ago -- a transfer that always times out, on a
*  bus where everything actually worked.
*
*  x86 DMA is cache coherent, so nothing here needs a flush; what it does need
*  is that the compiler not reorder the stores, which is what the barrier in
*  uhci_publish() is for. */
typedef struct
{
    volatile uint32_t link;     /* next descriptor, with the flag bits        */
    volatile uint32_t status;   /* control and status, written back by the HC */
    volatile uint32_t token;    /* PID, address, endpoint, toggle, length     */
    volatile uint32_t buffer;   /* data, PHYSICAL                             */
    uint32_t          pad[4];   /* the specification leaves these to software */
} uhci_td;

typedef struct
{
    volatile uint32_t link;     /* horizontal: the next queue in this frame   */
    volatile uint32_t element;  /* vertical: the transfer in this queue       */
    uint32_t          pad[2];
} uhci_qh;

/* What the wait loop found. Kept apart from the USB_E* codes because the
*  mapping is not one to one: a short packet is a legitimate end of a data
*  stage, and PENDING is only an error once the deadline has passed. */
#define UHCI_Q_PENDING  0   /* the controller has not finished               */
#define UHCI_Q_DONE     1   /* every descriptor ran, full length             */
#define UHCI_Q_SHORT    2   /* a descriptor moved fewer bytes, no error bit  */
#define UHCI_Q_STALL    3   /* the endpoint said no                          */
#define UHCI_Q_ERROR    4   /* a real failure                                */

/* --- State ---------------------------------------------------------------- */
static uint16_t uhci_io;                  /* I/O base                        */
static uint8_t  uhci_irq;                 /* interrupt line, reported only    */
static int      uhci_ports;               /* root ports actually present      */
static int      uhci_ready;               /* the controller is running        */

static uint32_t uhci_frame_phys;          /* what FRBASEADD is told           */
static volatile uint32_t *uhci_frame_list; /* the same, through P2V()         */

static uint32_t uhci_desc_phys;
static uint8_t *uhci_desc_virt;
static volatile uhci_qh *uhci_qhs;
static volatile uhci_td *uhci_tds;

static uint32_t uhci_bounce_phys;
static uint8_t *uhci_bounce_virt;

/* One flag per slot. Guarded by interrupts-off rather than an atomic, which is
*  sufficient and not a shortcut: this is a uniprocessor kernel and the only
*  thing that can take the CPU away between the test and the set is an
*  interrupt. */
static volatile int uhci_slot_busy[UHCI_SLOT_COUNT];

/* The wait channel for each slot. A channel is nothing but an address -- see
*  system.h, nothing is ever read through it -- and it is a separate array
*  rather than the address of the flag above only because task_wait() takes a
*  plain const void * and the flag has to stay volatile for the code that does
*  read it. */
static char uhci_slot_channel[UHCI_SLOT_COUNT];

/* The frame counter. FRNUM is eleven bits and wraps every 1024 frames, i.e.
*  every second, so the register alone cannot answer "how many frames since the
*  controller started". uhci_frame_sample() folds each reading into a 32 bit
*  total and is driven from the timer tick at 1 kHz -- one frame per tick, so a
*  wrap cannot be missed. */
static volatile uint32_t uhci_frame_total;
static volatile uint16_t uhci_frame_last;
static int uhci_frame_hooked;

static char uhci_info_buf[160];
static const char uhci_no_hc[] = "UHCI: no controller";

/* --- Port access ----------------------------------------------------------
*
*  system.h declares an inportw() that nothing defines and has no 32 bit
*  accessor at all; src/ata.c and src/rtl8139.c both ran into the same gap and
*  both kept their helpers local. A header this file does not own is no place
*  to fix that, so these are static and named apart from the global ones. */

static uint16_t uhci_inw(uint16_t port)
{
    uint16_t rv;
    __asm__ __volatile__ ("inw %1, %0" : "=a" (rv) : "dN" (port));
    return rv;
}

static void uhci_outw(uint16_t port, uint16_t value)
{
    __asm__ __volatile__ ("outw %0, %1" : : "a" (value), "dN" (port));
}

static uint32_t uhci_inl(uint16_t port)
{
    uint32_t rv;
    __asm__ __volatile__ ("inl %1, %0" : "=a" (rv) : "dN" (port));
    return rv;
}

static void uhci_outl(uint16_t port, uint32_t value)
{
    __asm__ __volatile__ ("outl %0, %1" : : "a" (value), "dN" (port));
}

static unsigned long uhci_irq_save(void)
{
    unsigned long flags;
    __asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
    return flags;
}

static void uhci_irq_restore(unsigned long flags)
{
    __asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

static unsigned long uhci_eflags(void)
{
    unsigned long flags;
    __asm__ __volatile__ ("pushfl; popl %0" : "=r" (flags) : : "memory");
    return flags;
}

/* --- Waiting for time to pass ---------------------------------------------
*
*  Three cases, and the middle one is the reason this is not just a call to
*  sleep():
*
*    - a task is running with interrupts on: sleep() blocks it, which is what
*      the port reset delays should cost the machine (fifty milliseconds of
*      somebody else's CPU rather than fifty of a spin).
*    - no task yet, interrupts on: sleep() falls through to timer_wait(), which
*      halts until the tick. Correct, just not a task switch.
*    - interrupts off: the millisecond counter cannot advance, so anything
*      based on it would never return. Only the port loop is left, and it is
*      approximate. A caller in that state has already given up on timing.
*/
static void uhci_spin(int iterations)
{
    int i;

    for(i = 0; i < iterations; i++)
    {
        (void)inportb(0x80);
    }
}

static void uhci_delay_ms(int ms)
{
    if(ms <= 0)
    {
        return;
    }

    if((uhci_eflags() & UHCI_EFLAGS_IF) != 0)
    {
        sleep(ms);
    }
    else
    {
        uhci_spin(ms * UHCI_SPINS_PER_MS);
    }
}

/* --- The frame counter ---------------------------------------------------- */

/* Folds one reading of FRNUM into the running total. Must be called with
*  interrupts off: it is called from the timer handler, which already is, and
*  from uhci_frames(), which arranges it.
*
*  The subtraction is masked to eleven bits so that the wrap from 1023 back to
*  0 reads as a delta of one rather than of minus 1023. That only holds as long
*  as fewer than 1024 frames pass between two samples, which is what pinning
*  this to the 1 kHz tick buys. */
static void uhci_frame_sample(void)
{
    uint16_t now;

    if(!uhci_ready)
    {
        return;
    }

    now = (uint16_t)(uhci_inw((uint16_t)(uhci_io + UHCI_FRNUM)) & UHCI_FRNUM_MASK);
    uhci_frame_total += (uint32_t)((now - uhci_frame_last) & UHCI_FRNUM_MASK);
    uhci_frame_last   = now;
}

static void uhci_frame_tick(struct regs *r)
{
    (void)r;
    uhci_frame_sample();
}

uint32_t uhci_frames(void)
{
    unsigned long flags;
    uint32_t total;

    if(!uhci_ready)
    {
        return 0;
    }

    /* Sample here as well as on the tick. It costs one port read, it makes the
    *  answer current rather than up to a millisecond old, and it is what keeps
    *  the number meaningful if the timer hook could not be installed. */
    flags = uhci_irq_save();
    uhci_frame_sample();
    total = uhci_frame_total;
    uhci_irq_restore(flags);

    return total;
}

/* --- The description string -----------------------------------------------
*
*  Composed by hand into a fixed buffer, as src/rtl8139.c and src/ata.c do:
*  there is no snprintf() here, and this file has no printf() either -- it
*  reports through return values and through this one line. */

static int uhci_put_str(int pos, const char *s)
{
    while(*s != '\0' && pos < (int)sizeof(uhci_info_buf) - 1)
    {
        uhci_info_buf[pos++] = *s++;
    }
    return pos;
}

static int uhci_put_hex(int pos, uint32_t value, int digits)
{
    static const char digit[] = "0123456789ABCDEF";
    int shift;

    shift = (digits - 1) * 4;
    while(shift >= 0 && pos < (int)sizeof(uhci_info_buf) - 1)
    {
        uhci_info_buf[pos++] = digit[(value >> shift) & 0x0FUL];
        shift -= 4;
    }
    return pos;
}

static int uhci_put_dec(int pos, uint32_t value)
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

    while(n > 0 && pos < (int)sizeof(uhci_info_buf) - 1)
    {
        uhci_info_buf[pos++] = tmp[--n];
    }
    return pos;
}

static void uhci_set_info(const char *text)
{
    int pos;

    pos = uhci_put_str(0, text);
    uhci_info_buf[pos] = '\0';
}

const char *uhci_info(void)
{
    if(uhci_info_buf[0] == '\0')
    {
        return uhci_no_hc;
    }
    return uhci_info_buf;
}

int uhci_present(void)
{
    return uhci_ready;
}

/* --- Descriptor arithmetic ------------------------------------------------
*
*  Everything the controller reads is addressed physically; everything this
*  file reads is addressed through P2V(). The two are computed from the same
*  base and are never mixed: a function that returns a uint32_t here is
*  returning something for the hardware, a function that returns a pointer is
*  returning something for the kernel. */

static uint32_t uhci_qh_phys(int index)
{
    return uhci_desc_phys + (uint32_t)(index * UHCI_QH_SIZE);
}

static uint32_t uhci_td_phys(int index)
{
    return uhci_desc_phys + (uint32_t)UHCI_TD_OFFSET +
           (uint32_t)index * (uint32_t)UHCI_TD_SIZE;
}

static uint32_t uhci_setup_phys(int slot)
{
    return uhci_desc_phys + (uint32_t)UHCI_SETUP_OFFSET +
           (uint32_t)(slot * UHCI_SETUP_SIZE);
}

static uint8_t *uhci_setup_virt(int slot)
{
    return uhci_desc_virt + UHCI_SETUP_OFFSET + slot * UHCI_SETUP_SIZE;
}

static uint32_t uhci_bounce_slot_phys(int slot)
{
    return uhci_bounce_phys + (uint32_t)slot * (uint32_t)PMM_FRAME_SIZE;
}

static uint8_t *uhci_bounce_slot_virt(int slot)
{
    return uhci_bounce_virt + slot * PMM_FRAME_SIZE;
}

/* The length fields are encoded as "bytes minus one" in eleven bits, so zero
*  bytes is 0x7FF and 1024 bytes -- the largest a single packet may be -- is
*  0x3FF. Getting this backwards produces a transfer that is one byte short of
*  everything, which looks like a device problem and is not. */
static uint32_t uhci_maxlen(int length)
{
    if(length <= 0)
    {
        return 0x7FFUL;
    }
    return ((uint32_t)(length - 1)) & 0x7FFUL;
}

static int uhci_actlen(uint32_t status)
{
    uint32_t n;

    n = status & UHCI_TD_ACTLEN;
    if(n == 0x7FFUL)
    {
        return 0;
    }
    return (int)(n + 1UL);
}

/* How many bytes the descriptor ASKED for, read back out of its own token.
*  Keeping the question inside the descriptor is what makes the short packet
*  test in uhci_scan() self contained -- it does not need to be told what the
*  packet size was. */
static int uhci_tokenlen(uint32_t token)
{
    uint32_t n;

    n = (token >> 21) & 0x7FFUL;
    if(n == 0x7FFUL)
    {
        return 0;
    }
    return (int)(n + 1UL);
}

static uint32_t uhci_token(uint32_t pid, uint32_t address, uint32_t endpoint,
                           uint32_t toggle, int length)
{
    return pid |
           ((address & 0x7FUL) << 8) |
           ((endpoint & 0x0FUL) << 15) |
           ((toggle & 1UL) << 19) |
           (uhci_maxlen(length) << 21);
}

/* --- Making a transfer reachable, and unreachable again -------------------
*
*  THE ORDERING PROBLEM. The controller walks the frame list once a
*  millisecond, forever, from the moment RS is set. It is not asked whether it
*  may; it is not stopped while a transfer is built. So a descriptor chain that
*  is half written must never be reachable from the frame list, or the
*  controller will execute whatever happens to be in the fields that are not
*  filled in yet -- an old buffer address, an old device address, an active bit
*  left over from three transfers ago.
*
*  THE CONCLUSION, and it is what every function below is arranged around:
*
*    1. The skeleton -- the frame list and the horizontal links between queue
*       heads -- is built ONCE, before FRBASEADD is written and before RS is
*       set, and is then never modified again. There is no ordering question
*       about memory the controller has not been pointed at yet.
*
*    2. At run time the only field that ever changes is a queue head's ELEMENT
*       pointer, plus the descriptors it will come to point at. A descriptor
*       chain is built completely -- buffer contents, token, status, and the
*       links between its own members -- while no queue head refers to it, so
*       none of those writes is observable by the controller in any order at
*       all.
*
*    3. Publication is then ONE naturally aligned 32 bit store into the element
*       pointer. A 32 bit aligned store is indivisible with respect to a PCI
*       master's 32 bit read, so the controller sees either the terminate value
*       that was there before or the finished chain. There is no third
*       possibility and therefore no window.
*
*    4. A compiler barrier goes in front of that store. x86 does not reorder
*       stores with respect to other stores, so no processor fence is needed;
*       what is needed is that the COMPILER not move the publishing store above
*       the descriptor writes, which it is otherwise entitled to do since it
*       cannot see who reads them.
*
*    5. Retraction is the mirror image and has one extra step. Writing the
*       terminate value into the element pointer makes the chain unreachable,
*       but the controller may have been INSIDE the queue at that instant, and
*       it will still write the status of the descriptor it was executing back
*       to memory afterwards. So on the abort path -- a timeout, an error, a
*       poll that found only NAKs -- the descriptors are not touched again
*       until the frame number has advanced, which is the bound on how long the
*       controller can still be interested in them. On the normal path there is
*       nothing to wait for: the controller advanced the element pointer to the
*       terminate value itself, which is how we knew the transfer had finished.
*/
static void uhci_publish(volatile uhci_qh *qh, uint32_t element)
{
    __asm__ __volatile__ ("" : : : "memory");
    qh->element = element;
    __asm__ __volatile__ ("" : : : "memory");
}

static void uhci_retract(volatile uhci_qh *qh)
{
    uhci_publish(qh, UHCI_LINK_TERM);
}

/* Waits until the controller has moved on to another frame, so that anything
*  it was in the middle of is finished with. Bounded: five frames is already
*  four more than it can need, and a controller that has stopped answering must
*  not turn this into a hang. */
static void uhci_wait_frame(void)
{
    uint16_t start;
    int i;

    start = (uint16_t)(uhci_inw((uint16_t)(uhci_io + UHCI_FRNUM)) & UHCI_FRNUM_MASK);

    for(i = 0; i < 5; i++)
    {
        if((uint16_t)(uhci_inw((uint16_t)(uhci_io + UHCI_FRNUM)) & UHCI_FRNUM_MASK) != start)
        {
            return;
        }
        uhci_delay_ms(UHCI_POLL_MS);
    }
}

/* --- Slots ----------------------------------------------------------------
*
*  One transfer per queue class at a time. The wait uses the idiom system.h
*  spells out, and it needs it: the release comes from another TASK, so the
*  lost wakeup race is real here -- test with interrupts off, block with
*  interrupts off, re-test after every wake.
*
*  A caller that cannot block -- the boot path before there is a task, or one
*  that arrived with interrupts already off -- gets a straight refusal if the
*  slot is busy rather than a spin. There is exactly one caller in that state
*  (enumeration during bring-up) and nothing else can be holding the slot then. */
static int uhci_slot_take(int slot)
{
    unsigned long flags;
    unsigned int  deadline;
    int taken;

    taken    = 0;
    deadline = (unsigned int)timer_get_ticks() + (unsigned int)UHCI_SLOT_TIMEOUT_MS;

    flags = uhci_irq_save();

    if((flags & UHCI_EFLAGS_IF) == 0 || taskmgr_get_currpid() < 0)
    {
        if(!uhci_slot_busy[slot])
        {
            uhci_slot_busy[slot] = 1;
            taken = 1;
        }
        uhci_irq_restore(flags);
        return taken;
    }

    while(uhci_slot_busy[slot])
    {
        if((int)(deadline - (unsigned int)timer_get_ticks()) <= 0)
        {
            break;
        }
        task_wait(&uhci_slot_channel[slot], UHCI_POLL_MS * 4);
    }

    if(!uhci_slot_busy[slot])
    {
        uhci_slot_busy[slot] = 1;
        taken = 1;
    }

    uhci_irq_restore(flags);
    return taken;
}

static void uhci_slot_give(int slot)
{
    unsigned long flags;

    flags = uhci_irq_save();
    uhci_slot_busy[slot] = 0;
    uhci_irq_restore(flags);

    task_wake(&uhci_slot_channel[slot]);
}

/* --- Reading a finished chain ---------------------------------------------
*
*  SHORT PACKET VERSUS ERROR, which is the distinction the hardware makes least
*  obvious. Both look identical from the outside: the transfer stopped before
*  it had moved everything it was asked to move. The difference is entirely in
*  the status word of the descriptor that stopped it:
*
*      inactive, no bit of UHCI_TD_ERRORS set, fewer bytes than asked for
*          -> a short packet. That is not a failure, it is the device saying
*             "that is all I have", and a control transfer's data stage relies
*             on exactly that to end. Every byte that was moved is good.
*
*      inactive, UHCI_TD_STALLED set
*          -> the endpoint refused the request. Also not a transfer failure:
*             usb.h says so, and enumeration deliberately provokes one to find
*             out what a device does not support.
*
*      inactive, any other bit of UHCI_TD_ERRORS set
*          -> a genuine failure. CRCTIMEO in particular means the device did
*             not answer at all after three tries.
*
*  AND NAK IS NEITHER. A NAKed descriptor is still ACTIVE: the controller
*  records the NAK in bit 19, leaves the descriptor alone and tries again in
*  the next frame, and the error counter is not touched. So a NAK can never
*  appear as a completion here at all -- it is only visible as "still active,
*  with the NAK bit set", which is precisely how uhci_data_transfer() tells an
*  idle keyboard (return 0) from a broken one (USB_ETIMEOUT). A driver that
*  treated bit 19 as an error would report a failure on every poll of every
*  idle device, which is to say on almost every poll there is.
*
*  On return *done is the number of descriptors the controller has finished
*  with, which is what the data toggle is advanced by. */
static int uhci_scan(int first, int count, int *done)
{
    volatile uhci_td *td;
    uint32_t status;
    int i;

    for(i = 0; i < count; i++)
    {
        td     = &uhci_tds[first + i];
        status = td->status;

        if((status & UHCI_TD_ACTIVE) != 0)
        {
            *done = i;
            return UHCI_Q_PENDING;
        }

        if((status & UHCI_TD_STALLED) != 0)
        {
            *done = i;
            return UHCI_Q_STALL;
        }

        if((status & UHCI_TD_ERRORS) != 0)
        {
            *done = i;
            return UHCI_Q_ERROR;
        }

        if(uhci_actlen(status) < uhci_tokenlen(td->token))
        {
            *done = i + 1;
            return UHCI_Q_SHORT;
        }
    }

    *done = count;
    return UHCI_Q_DONE;
}

/* Bytes actually moved by a run of descriptors, counting only those the
*  controller has finished with and did not fail. Stops at the first that is
*  still active or carries an error, so a partial transfer reports what really
*  arrived rather than what was asked for. */
static int uhci_moved(int first, int count)
{
    volatile uhci_td *td;
    uint32_t status;
    int total;
    int i;

    total = 0;
    for(i = 0; i < count; i++)
    {
        td     = &uhci_tds[first + i];
        status = td->status;

        if((status & (UHCI_TD_ACTIVE | UHCI_TD_ERRORS)) != 0)
        {
            break;
        }
        total += uhci_actlen(status);
    }
    return total;
}

/* Notices a controller that has fallen over. A PCI bus error or a descriptor
*  the controller could not parse both HALT it, which would otherwise present
*  as every transfer from then on timing out with no explanation. Clearing the
*  status and setting RS again is the only recovery available without tearing
*  the whole schedule down, and it is worth trying because the alternative is a
*  USB stack that is dead until reboot. */
static int uhci_check_controller(void)
{
    uint16_t status;

    status = uhci_inw((uint16_t)(uhci_io + UHCI_USBSTS));

    /* The benign bits accumulate and mean nothing to a driver that polls, but
    *  leaving them standing would hide the next fatal one behind them. */
    if((status & (UHCI_STS_USBINT | UHCI_STS_ERRINT | UHCI_STS_RESUME)) != 0)
    {
        uhci_outw((uint16_t)(uhci_io + UHCI_USBSTS),
                  (uint16_t)(status & (UHCI_STS_USBINT | UHCI_STS_ERRINT |
                                       UHCI_STS_RESUME)));
    }

    if((status & UHCI_STS_FATAL) != 0)
    {
        uhci_outw((uint16_t)(uhci_io + UHCI_USBSTS), (uint16_t)UHCI_STS_FATAL);
        uhci_outw((uint16_t)(uhci_io + UHCI_USBCMD),
                  (uint16_t)(UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP));
        return -1;
    }

    return 0;
}

/* --- Waiting for a transfer ----------------------------------------------
*
*  WHAT WAS CHOSEN, AND WHY IT IS NOT AN INTERRUPT. UHCI can raise a PCI
*  interrupt when a descriptor with IOC finishes, and enabling it would let a
*  transfer block on a channel that the handler wakes. It is deliberately not
*  done, for a reason that belongs to this kernel rather than to UHCI:
*  irq_install_handler() keeps ONE handler per line, and a PIIX3 routes its
*  UHCI function to the same PCI interrupt as the other devices in the same
*  slot -- on the machine this is tested on, the same line as the RTL8139 the
*  network stack drives. Installing here would silently unhook that card, and
*  fixing it properly means a shared-interrupt chain in irq.c, which is not
*  this file's to write.
*
*  So completion is a bit in a descriptor, and this polls for it -- but not in
*  a busy loop. The task blocks in task_wait() for one frame at a time, so it
*  is TASK_STATE_BLOCKED and off the run queue for the whole of a transfer,
*  costing one port read and one descriptor read per millisecond. Nothing wakes
*  the channel, so the timeout IS the poll interval; that is also why the
*  interrupts-off part of the system.h idiom is not needed here. That idiom
*  exists to close the race between testing a condition and blocking, and the
*  race needs a WAKE to lose. There is none: the condition is changed by the
*  controller writing memory, which no interrupt state can hide and which is
*  still true whenever the loop next looks. The re-test after every wait is
*  kept, because it is what makes the loop correct either way.
*
*  Polling one frame apart is also the highest useful rate. The controller only
*  touches a descriptor when it runs the frame that descriptor hangs off, so
*  looking twice in one frame cannot see anything the first look did not. */
static int uhci_wait_queue(int first, int count, int timeout_ms, int *done)
{
    unsigned int deadline;
    int can_block;
    int spins;
    int state;

    can_block = (uhci_eflags() & UHCI_EFLAGS_IF) != 0;
    deadline  = (unsigned int)timer_get_ticks() + (unsigned int)timeout_ms;
    spins     = timeout_ms * UHCI_SPINS_PER_MS;

    for(;;)
    {
        state = uhci_scan(first, count, done);
        if(state != UHCI_Q_PENDING)
        {
            return state;
        }

        if(uhci_check_controller() != 0)
        {
            return UHCI_Q_ERROR;
        }

        if(can_block)
        {
            if((int)(deadline - (unsigned int)timer_get_ticks()) <= 0)
            {
                break;
            }
            uhci_delay_ms(UHCI_POLL_MS);
        }
        else
        {
            /* Interrupts are off, so the millisecond counter is frozen and the
            *  deadline above can never be reached. A counted loop is all that
            *  is left; it is approximate, and it is bounded, which is the
            *  property that actually matters. */
            spins -= UHCI_SPINS_PER_MS;
            if(spins <= 0)
            {
                break;
            }
            uhci_spin(UHCI_SPINS_PER_MS);
        }
    }

    /* One last look before giving up: the deadline may have passed in the same
    *  millisecond the controller finished. */
    return uhci_scan(first, count, done);
}

/* --- Building descriptors ------------------------------------------------- */

/* Fills in one descriptor. The chain link is written by the caller once the
*  whole chain exists, and none of it is reachable by the controller yet, so
*  the order within this function is free. */
static void uhci_fill_td(int index, uint32_t status, uint32_t token,
                         uint32_t buffer)
{
    volatile uhci_td *td;

    td         = &uhci_tds[index];
    td->link   = UHCI_LINK_TERM;
    td->buffer = buffer;
    td->token  = token;
    td->status = status;
}

/* Chains a run of descriptors, depth first between them and terminating after
*  the last. See UHCI_LINK_VF for what depth first buys. */
static void uhci_chain(int first, int count)
{
    int i;

    for(i = 0; i < count - 1; i++)
    {
        uhci_tds[first + i].link =
            uhci_td_phys(first + i + 1) | UHCI_LINK_VF;
    }
    uhci_tds[first + count - 1].link = UHCI_LINK_TERM;
}

/* The status word every descriptor of a transfer starts from: three attempts
*  before the controller gives up, active, and the low speed flag when the
*  device on the other end is a keyboard or a mouse. Getting the low speed flag
*  wrong does not produce an error, it produces a device that never answers. */
static uint32_t uhci_base_status(const usb_device *dev)
{
    uint32_t status;

    status = UHCI_TD_CERR3 | UHCI_TD_ACTIVE;
    if(dev->speed == USB_SPEED_LOW)
    {
        status |= UHCI_TD_LS;
    }
    return status;
}

/* --- Endpoints ------------------------------------------------------------
*
*  The toggle lives in usb_endpoint and is maintained here, so a transfer to an
*  endpoint the core has not recorded cannot be done correctly and is refused
*  rather than guessed at. Guessing would mean a private toggle store that
*  silently drifts out of step with the one usb.h says is authoritative, and
*  the symptom of a wrong toggle is a device that ignores every second packet
*  -- which looks like flaky hardware and is not.
*
*  The number is matched on the low four bits so that a caller may pass either
*  the endpoint number or the full endpoint address with its direction bit. An
*  exact direction match wins; a number-only match is accepted after, for a
*  core that records the number alone. */
static usb_endpoint *uhci_find_endpoint(usb_device *dev, int endpoint, int in)
{
    usb_endpoint *fallback;
    int want;
    int num;
    int i;
    int n;

    fallback = 0;
    num      = endpoint & 0x0F;
    want     = in ? USB_DIR_IN : USB_DIR_OUT;

    n = dev->endpoints;
    if(n > USB_MAX_ENDPOINTS)
    {
        n = USB_MAX_ENDPOINTS;
    }

    for(i = 0; i < n; i++)
    {
        if((dev->endpoint[i].address & 0x0F) != (uint8_t)num)
        {
            continue;
        }
        if((int)dev->endpoint[i].direction == want)
        {
            return &dev->endpoint[i];
        }
        if(fallback == 0)
        {
            fallback = &dev->endpoint[i];
        }
    }

    return fallback;
}

/* --- Control transfers ----------------------------------------------------
*
*  Three stages in one queue: SETUP, an optional data stage of as many packets
*  as the endpoint's size needs, and a status stage in the opposite direction.
*
*  THE TOGGLE RULE HERE IS NOT THE BULK RULE, and this is the part that is
*  easiest to get wrong because it looks like it should be. A bulk or interrupt
*  endpoint's toggle is a property of the ENDPOINT and survives from one
*  transfer to the next, which is why usb_endpoint carries a field for it. A
*  control transfer's toggle is a property of the TRANSFER and starts again
*  every time:
*
*      SETUP   always DATA0
*      data    first packet DATA1, alternating from there
*      STATUS  always DATA1
*
*  So endpoint zero's stored toggle is meaningless and this function neither
*  reads nor writes it. Carrying a persistent toggle across control transfers
*  would put the device out of step on the second request it is ever asked.
*
*  SHORT PACKET DETECT is set on the data stage descriptors when the data stage
*  reads IN, and only then. It makes the controller stop the queue at a
*  descriptor that came back short instead of running the rest, which is what
*  has to happen: a device that answers a 64 byte request with 18 bytes has
*  nothing more to give, and the remaining IN descriptors would be NAKed
*  forever. Stopping the queue also stops the status stage, so it is restarted
*  explicitly below. Doing it that way rather than relying on where exactly the
*  controller left the element pointer keeps this correct whichever of the two
*  defensible readings of that corner of the specification the silicon took. */
static int uhci_op_control(usb_device *dev, const usb_setup *setup,
                           void *data, int length)
{
    volatile uhci_qh *qh;
    uint32_t base;
    uint32_t address;
    uint32_t data_pid;
    uint32_t status_pid;
    uint8_t *bounce;
    uint32_t bounce_phys;
    int slot;
    int first;
    int index;
    int packet;
    int chunk;
    int offset;
    int toggle;
    int data_tds;
    int total_tds;
    int state;
    int done;
    int moved;
    int result;
    int in;

    if(!uhci_ready || dev == 0 || setup == 0)
    {
        return USB_EINVAL;
    }
    if(length < 0 || length > UHCI_MAX_TRANSFER)
    {
        return USB_EINVAL;
    }
    if(length > 0 && data == 0)
    {
        return USB_EINVAL;
    }

    /* Endpoint zero's packet size comes from the device descriptor, and during
    *  the first seconds of enumeration it is not known yet -- the core reads
    *  the first eight bytes of that descriptor precisely to find it out. Eight
    *  is what every device must support until then and is the right default. */
    packet = (int)dev->max_packet0;
    if(packet < 8)
    {
        packet = 8;
    }
    if(packet > 64)
    {
        packet = 64;
    }

    data_tds  = (length + packet - 1) / packet;
    total_tds = data_tds + 2;
    if(total_tds > UHCI_TDS_PER_SLOT)
    {
        return USB_EINVAL;
    }

    slot        = UHCI_SLOT_CONTROL;
    first       = slot * UHCI_TDS_PER_SLOT;
    qh          = &uhci_qhs[UHCI_QH_CONTROL];
    bounce      = uhci_bounce_slot_virt(slot);
    bounce_phys = uhci_bounce_slot_phys(slot);
    address     = (uint32_t)dev->address;
    in          = (setup->request_type & USB_DIR_IN) != 0;
    data_pid    = in ? UHCI_PID_IN : UHCI_PID_OUT;

    /* The status stage runs the other way round from the data stage, and IN
    *  when there is no data stage to be the other way round from. */
    status_pid = (length > 0 && in) ? UHCI_PID_OUT : UHCI_PID_IN;

    if(!uhci_slot_take(slot))
    {
        return USB_ETIMEOUT;
    }

    base = uhci_base_status(dev);

    /* The caller's buffer is of no use to the controller: it has to be
    *  physically contiguous, reachable by physical address and untouched until
    *  the controller has finished with it, none of which anything above this
    *  line promises. So it is copied through a buffer this driver owns, for
    *  the same reasons src/rtl8139.c copies a frame into a transmit slot. */
    memcpy(uhci_setup_virt(slot), setup, sizeof(usb_setup));
    if(!in && length > 0)
    {
        memcpy(bounce, data, (size_t)length);
    }

    index = first;

    uhci_fill_td(index, base,
                 uhci_token(UHCI_PID_SETUP, address, 0, 0,
                            (int)sizeof(usb_setup)),
                 uhci_setup_phys(slot));
    index++;

    toggle = 1;
    offset = 0;
    while(offset < length)
    {
        chunk = length - offset;
        if(chunk > packet)
        {
            chunk = packet;
        }

        uhci_fill_td(index,
                     in ? (base | UHCI_TD_SPD) : base,
                     uhci_token(data_pid, address, 0, (uint32_t)toggle, chunk),
                     bounce_phys + (uint32_t)offset);

        index++;
        offset += chunk;
        toggle ^= 1;
    }

    uhci_fill_td(index, base,
                 uhci_token(status_pid, address, 0, 1, 0),
                 0);
    index++;

    total_tds = index - first;
    uhci_chain(first, total_tds);

    uhci_publish(qh, uhci_td_phys(first));

    state = uhci_wait_queue(first, total_tds, UHCI_CONTROL_TIMEOUT_MS, &done);

    /* A short data stage stopped the queue before the status stage. The status
    *  stage still has to happen -- a control transfer that never acknowledges
    *  leaves the device waiting -- so it is published on its own and waited
    *  for again. Its own outcome does not change the byte count; a device that
    *  fails the acknowledgement after handing over good data has still handed
    *  over good data. */
    if(state == UHCI_Q_SHORT && done < total_tds)
    {
        uhci_publish(qh, uhci_td_phys(first + total_tds - 1));
        uhci_wait_queue(first + total_tds - 1, 1, UHCI_CONTROL_TIMEOUT_MS,
                        &done);
        state = UHCI_Q_SHORT;
    }

    moved = uhci_moved(first + 1, data_tds);

    switch(state)
    {
        case UHCI_Q_DONE:
        case UHCI_Q_SHORT:
            result = moved;
            break;
        case UHCI_Q_STALL:
            result = USB_ESTALL;
            break;
        case UHCI_Q_PENDING:
            result = USB_ETIMEOUT;
            break;
        default:
            result = USB_EIO;
            break;
    }

    uhci_retract(qh);

    /* Only the abort path has to wait for the controller to lose interest; on
    *  the normal path it advanced the element pointer to the terminate value
    *  itself, which is how the wait above knew the transfer had ended. */
    if(state != UHCI_Q_DONE && state != UHCI_Q_SHORT)
    {
        uhci_wait_frame();
    }

    if(in && result > 0)
    {
        if(result > length)
        {
            result = length;    /* a babbling device cannot overrun the caller */
        }
        memcpy(data, bounce, (size_t)result);
    }

    uhci_slot_give(slot);

    return result;
}

/* --- Bulk and interrupt ---------------------------------------------------
*
*  One code path, because they differ only in which queue they hang off, how
*  long they are given, and whether a device with nothing to say is an answer
*  or a failure.
*
*  THE TOGGLE HERE IS THE ENDPOINT'S and persists between calls, which is the
*  whole reason usb_endpoint has a field for it. It is advanced by exactly the
*  number of descriptors the controller finished with -- not by the number that
*  were built. That distinction is the correctness of the whole thing:
*
*    - a poll that only ever got NAKs finished nothing, so the toggle does not
*      move. It must not: no packet crossed the wire, and moving it would put
*      the endpoint out of step for the rest of its life.
*    - a transfer that ended short finished the short descriptor too, and that
*      packet did cross the wire, so it counts.
*    - a transfer that ended in an error advances by the descriptors BEFORE the
*      failing one, which are the ones that really moved. What the toggle
*      should be after a STALL is not this layer's decision -- clearing the
*      halt condition with CLEAR_FEATURE resets the endpoint's toggle to DATA0
*      at the device, and the class driver that clears it is the one that has
*      to say so here. */
static int uhci_data_transfer(usb_device *dev, int endpoint, void *buf,
                              int length, int in, int slot,
                              volatile uhci_qh *qh, int timeout_ms, int nak_ok)
{
    usb_endpoint *ep;
    uint32_t base;
    uint32_t address;
    uint32_t pid;
    uint8_t *bounce;
    uint32_t bounce_phys;
    int first;
    int index;
    int packet;
    int chunk;
    int offset;
    int toggle;
    int count;
    int state;
    int done;
    int moved;
    int result;

    if(!uhci_ready || dev == 0 || buf == 0)
    {
        return USB_EINVAL;
    }
    if(length <= 0 || length > UHCI_MAX_TRANSFER)
    {
        return USB_EINVAL;
    }

    ep = uhci_find_endpoint(dev, endpoint, in);
    if(ep == 0)
    {
        return USB_EINVAL;
    }

    packet = (int)ep->max_packet;
    if(packet <= 0)
    {
        packet = (dev->speed == USB_SPEED_LOW) ? 8 : 64;
    }
    if(packet > 64)
    {
        packet = 64;    /* full speed bulk and interrupt cannot exceed this */
    }

    count = (length + packet - 1) / packet;
    if(count > UHCI_TDS_PER_SLOT)
    {
        return USB_EINVAL;
    }

    if(!uhci_slot_take(slot))
    {
        return USB_ETIMEOUT;
    }

    first       = slot * UHCI_TDS_PER_SLOT;
    bounce      = uhci_bounce_slot_virt(slot);
    bounce_phys = uhci_bounce_slot_phys(slot);
    address     = (uint32_t)dev->address;
    pid         = in ? UHCI_PID_IN : UHCI_PID_OUT;
    base        = uhci_base_status(dev);
    toggle      = ep->toggle & 1;

    if(!in)
    {
        memcpy(bounce, buf, (size_t)length);
    }

    index  = first;
    offset = 0;
    while(offset < length)
    {
        chunk = length - offset;
        if(chunk > packet)
        {
            chunk = packet;
        }

        uhci_fill_td(index,
                     in ? (base | UHCI_TD_SPD) : base,
                     uhci_token(pid, address, (uint32_t)(endpoint & 0x0F),
                                (uint32_t)toggle, chunk),
                     bounce_phys + (uint32_t)offset);

        index++;
        offset += chunk;
        toggle ^= 1;
    }

    count = index - first;
    uhci_chain(first, count);

    uhci_publish(qh, uhci_td_phys(first));

    state = uhci_wait_queue(first, count, timeout_ms, &done);

    moved = uhci_moved(first, count);

    /* done counts the descriptors the controller finished with, whatever the
    *  outcome; that is exactly what the toggle advanced by. */
    ep->toggle = (uint8_t)((ep->toggle + (uint32_t)done) & 1UL);

    switch(state)
    {
        case UHCI_Q_DONE:
        case UHCI_Q_SHORT:
            result = moved;
            break;
        case UHCI_Q_STALL:
            result = USB_ESTALL;
            break;
        case UHCI_Q_PENDING:
            /* Nothing finished inside the window. If the descriptor that is
            *  still active has the NAK bit set, the device was asked and said
            *  "not now", which for an interrupt endpoint is the answer rather
            *  than a fault -- an idle keyboard does this every time it is
            *  polled, forever. For bulk it is a stall of a different kind and
            *  the caller has to know. */
            if(nak_ok &&
               (uhci_tds[first + done].status & UHCI_TD_NAK) != 0)
            {
                result = 0;
            }
            else if(nak_ok && moved == 0)
            {
                /* Still active and not even NAKed: the frame the descriptor
                *  hangs off may simply not have come round inside the window.
                *  For a poll that is also "nothing yet". */
                result = 0;
            }
            else
            {
                result = USB_ETIMEOUT;
            }
            break;
        default:
            result = USB_EIO;
            break;
    }

    uhci_retract(qh);

    if(state != UHCI_Q_DONE && state != UHCI_Q_SHORT)
    {
        uhci_wait_frame();
    }

    if(in && result > 0)
    {
        if(result > length)
        {
            result = length;
        }
        memcpy(buf, bounce, (size_t)result);
    }

    uhci_slot_give(slot);

    return result;
}

/* Which interrupt queue an endpoint belongs on: the largest power of two that
*  is no bigger than the interval it asked for, capped at 128 frames. An
*  interval of zero, which a device that did not fill the field in properly
*  will report, lands on the every-frame queue, which is slower for the bus and
*  never wrong. */
static volatile uhci_qh *uhci_interval_qh(int interval)
{
    int k;

    k = 0;
    while(k + 1 < UHCI_INT_QHS && (1 << (k + 1)) <= interval)
    {
        k++;
    }
    return &uhci_qhs[k];
}

static int uhci_op_interrupt_in(usb_device *dev, int endpoint, void *buf,
                                int length)
{
    usb_endpoint *ep;
    volatile uhci_qh *qh;

    if(!uhci_ready || dev == 0)
    {
        return USB_EINVAL;
    }

    ep = uhci_find_endpoint(dev, endpoint, 1);
    if(ep == 0)
    {
        return USB_EINVAL;
    }

    qh = uhci_interval_qh((int)ep->interval);

    return uhci_data_transfer(dev, endpoint, buf, length, 1,
                              UHCI_SLOT_INT, qh, UHCI_INT_TIMEOUT_MS, 1);
}

static int uhci_op_bulk(usb_device *dev, int endpoint, void *buf, int length,
                        int in)
{
    return uhci_data_transfer(dev, endpoint, buf, length, in ? 1 : 0,
                              UHCI_SLOT_BULK, &uhci_qhs[UHCI_QH_BULK],
                              UHCI_BULK_TIMEOUT_MS, 0);
}

/* --- Root ports ----------------------------------------------------------- */

static uint16_t uhci_port_reg(int port)
{
    return (uint16_t)(uhci_io + UHCI_PORTSC1 + port * 2);
}

/* Reads PORTSC with the two write-one-to-clear bits masked off, so the value
*  can be written straight back without acknowledging a change nobody has
*  looked at yet. Every read-modify-write of this register in this file goes
*  through here; the one place that DOES want to acknowledge says so by putting
*  the bits back in explicitly. */
static uint16_t uhci_port_state(int port)
{
    return (uint16_t)(uhci_inw(uhci_port_reg(port)) & ~UHCI_PORT_CHANGE);
}

/* How many root ports there are. The specification does not fix it at two --
*  that is a property of the PIIX3 and of nothing else -- and bit 7 of a port
*  register reads as one on a port that exists, which is the discoverable
*  answer. A register that reads all ones is a port that is not decoded at all. */
static int uhci_count_ports(void)
{
    uint16_t value;
    int n;

    for(n = 0; n < UHCI_MAX_PORTS; n++)
    {
        value = uhci_inw(uhci_port_reg(n));
        if(value == 0xFFFF || (value & UHCI_PORT_ALWAYS1) == 0)
        {
            break;
        }
    }
    return n;
}

static int uhci_op_port_count(void)
{
    return uhci_ports;
}

/* Resets one root port and reports what answered.
*
*  The sequence is fixed by the USB specification and every step of it is load
*  bearing. Drive reset for at least ten milliseconds (fifty here, because some
*  devices need the longer pulse before they notice); release it; only THEN
*  enable the port, because a port enabled while reset is asserted enables
*  nothing; and wait for the enable to read back, because it does not take
*  effect instantly and a device addressed too early simply does not answer.
*
*  The speed is read AFTER the reset and not before. Low speed is signalled by
*  which of D+ and D- the device pulls up, and the port only reports it once
*  the device has been reset and is driving the line properly. Getting it wrong
*  is not an error either: it is a device that never answers a single
*  transaction, because the controller sends every packet at the wrong rate. */
static int uhci_op_port_reset(int port, int *speed)
{
    uint16_t reg;
    uint16_t state;
    int i;

    if(!uhci_ready || port < 0 || port >= uhci_ports)
    {
        return USB_ENODEV;
    }

    reg   = uhci_port_reg(port);
    state = uhci_port_state(port);

    if((state & UHCI_PORT_CCS) == 0)
    {
        /* Empty. Drop any enable left over from a device that has been
        *  unplugged and acknowledge the change bits, so the next look at this
        *  port starts from a clean reading rather than from the ghost of the
        *  last one. */
        uhci_outw(reg, (uint16_t)((state & ~UHCI_PORT_PE) | UHCI_PORT_CHANGE));
        return 0;
    }

    /* Reset. The change bits are masked out of the value being written, so
    *  driving reset does not quietly acknowledge the connect that led here. */
    uhci_outw(reg, (uint16_t)(state | UHCI_PORT_RESET));
    uhci_delay_ms(UHCI_PORT_RESET_MS);

    state = uhci_port_state(port);
    uhci_outw(reg, (uint16_t)(state & ~UHCI_PORT_RESET));

    /* A few microseconds between releasing reset and enabling. Nothing in this
    *  kernel measures microseconds, and the port loop below tolerates the
    *  enable not taking on the first attempt, so this is a token delay rather
    *  than a guarantee. */
    uhci_spin(10);

    for(i = 0; i < UHCI_PORT_ENABLE_MS; i++)
    {
        state = uhci_inw(reg);

        if((state & UHCI_PORT_CCS) == 0)
        {
            /* The device went away during its own reset. Not an error, just
            *  nothing there any more. */
            uhci_outw(reg, (uint16_t)((state & ~(uint16_t)UHCI_PORT_PE) |
                                      UHCI_PORT_CHANGE));
            return 0;
        }

        if((state & UHCI_PORT_CHANGE) != 0)
        {
            /* Acknowledge and look again. A connect or enable change raised by
            *  our own reset is expected and must be cleared, or it stands
            *  forever and hides the next real one. Writing the value back
            *  unchanged is the acknowledgement: the change bits are set in it
            *  exactly where a change is pending, and writing a one to one of
            *  them is what clears it. */
            uhci_outw(reg, state);
            uhci_delay_ms(1);
            continue;
        }

        if((state & UHCI_PORT_PE) != 0)
        {
            break;
        }

        uhci_outw(reg, (uint16_t)((state & ~UHCI_PORT_CHANGE) | UHCI_PORT_PE));
        uhci_delay_ms(1);
    }

    state = uhci_inw(reg);
    if((state & UHCI_PORT_PE) == 0)
    {
        return 0;       /* nothing usable came up */
    }

    if(speed != 0)
    {
        *speed = ((state & UHCI_PORT_LSDA) != 0) ? USB_SPEED_LOW
                                                 : USB_SPEED_FULL;
    }

    return 1;
}

static const usb_hc_ops uhci_ops =
{
    "UHCI",
    uhci_op_port_count,
    uhci_op_port_reset,
    uhci_op_control,
    uhci_op_interrupt_in,
    uhci_op_bulk
};

/* --- Bring-up ------------------------------------------------------------- */

/* The BIOS handoff. See UHCI_PCI_LEGSUP above for what the bits are; the point
*  of doing it FIRST, before the controller is touched at all, is that a BIOS
*  servicing an SMI trap between the reset and the frame list write would be
*  driving the controller at the same moment this driver is configuring it, and
*  the two would fight over registers neither knows the other is touching.
*
*  Returns the value that was there, for the report -- "what the BIOS had" is
*  worth showing, because 0x0000 and 0x2000 are the two normal answers and
*  anything with 0x00F0 in it means keyboard emulation really was live. */
static uint16_t uhci_release_bios(const pci_device *dev)
{
    uint16_t before;

    before = pci_read16(dev->bus, dev->slot, dev->func, UHCI_PCI_LEGSUP);
    pci_write16(dev->bus, dev->slot, dev->func, UHCI_PCI_LEGSUP,
                (uint16_t)UHCI_LEGSUP_CLEAR);

    return before;
}

/* Global reset, then host controller reset.
*
*  Both are needed and they do different things. The global reset drives a USB
*  reset out of every root port and puts the whole thing, including anything
*  plugged in, back to a known state -- which is how a controller the BIOS left
*  half configured, with a device already addressed, is made to forget. The
*  host controller reset then clears the controller's own registers.
*
*  The host controller reset is self clearing and is WAITED for rather than
*  assumed. That is the step that most obviously appears to work while doing
*  nothing: the write always succeeds, and a controller that never finishes the
*  reset answers every subsequent register read with whatever it had before. */
static int uhci_reset(void)
{
    int timeout;

    uhci_outw((uint16_t)(uhci_io + UHCI_USBCMD), (uint16_t)UHCI_CMD_GRESET);
    uhci_delay_ms(UHCI_GRESET_MS);
    uhci_outw((uint16_t)(uhci_io + UHCI_USBCMD), 0);
    uhci_delay_ms(10);

    uhci_outw((uint16_t)(uhci_io + UHCI_USBCMD), (uint16_t)UHCI_CMD_HCRESET);

    for(timeout = 0; timeout < UHCI_HCRESET_TIMEOUT_MS; timeout++)
    {
        if((uhci_inw((uint16_t)(uhci_io + UHCI_USBCMD)) & UHCI_CMD_HCRESET) == 0)
        {
            break;
        }
        uhci_delay_ms(1);
    }

    if((uhci_inw((uint16_t)(uhci_io + UHCI_USBCMD)) & UHCI_CMD_HCRESET) != 0)
    {
        return -1;
    }

    /* Nothing may raise an interrupt: this driver polls, and a controller that
    *  asserts a shared PCI line nobody handles is a line that stays asserted. */
    uhci_outw((uint16_t)(uhci_io + UHCI_USBINTR), 0);
    uhci_outw((uint16_t)(uhci_io + UHCI_USBSTS), (uint16_t)UHCI_STS_ALL);

    return 0;
}

/* Everything the controller reads, out of the frame allocator, because it is
*  the only allocator here that promises a contiguous block at a known physical
*  address -- the heap promises neither. The range check against
*  DIRECT_MAP_LIMIT is what makes P2V() -- the only way the kernel can touch
*  the same memory afterwards -- defined at all.
*
*  The alignment check is not defensive theatre. A frame from the pmm is 4 KiB
*  aligned by construction, which is exactly what the frame list needs and more
*  than the descriptors need; but the low bits of every pointer written into
*  these structures are FLAGS, so if that ever stopped being true the failure
*  would be a controller following addresses that are off by a few bytes and
*  carry the wrong flags, which is unreadable from any symptom. */
static int uhci_alloc(void)
{
    uint32_t phys;

    phys = (uint32_t)pmm_alloc_frames(1);
    if(phys == 0 || (phys & 0x0FFFUL) != 0 ||
       phys > (uint32_t)DIRECT_MAP_LIMIT - (uint32_t)(PMM_FRAME_SIZE - 1))
    {
        if(phys != 0)
        {
            pmm_free_frames((void *)phys, 1);
        }
        return -1;
    }
    uhci_frame_phys = phys;
    uhci_frame_list = (volatile uint32_t *)P2V(phys);

    phys = (uint32_t)pmm_alloc_frames((uint32_t)UHCI_DESC_FRAMES);
    if(phys == 0 || (phys & 0x0FFFUL) != 0 ||
       phys > (uint32_t)DIRECT_MAP_LIMIT - (uint32_t)(UHCI_DESC_BYTES - 1))
    {
        if(phys != 0)
        {
            pmm_free_frames((void *)phys, (uint32_t)UHCI_DESC_FRAMES);
        }
        pmm_free_frames((void *)uhci_frame_phys, 1);
        uhci_frame_phys = 0;
        uhci_frame_list = 0;
        return -1;
    }
    uhci_desc_phys = phys;
    uhci_desc_virt = (uint8_t *)P2V(phys);
    uhci_qhs       = (volatile uhci_qh *)uhci_desc_virt;
    uhci_tds       = (volatile uhci_td *)(uhci_desc_virt + UHCI_TD_OFFSET);
    memset(uhci_desc_virt, 0, (size_t)UHCI_DESC_BYTES);

    phys = (uint32_t)pmm_alloc_frames((uint32_t)UHCI_BOUNCE_FRAMES);
    if(phys == 0 ||
       phys > (uint32_t)DIRECT_MAP_LIMIT -
              (uint32_t)(UHCI_BOUNCE_FRAMES * PMM_FRAME_SIZE - 1))
    {
        if(phys != 0)
        {
            pmm_free_frames((void *)phys, (uint32_t)UHCI_BOUNCE_FRAMES);
        }
        pmm_free_frames((void *)uhci_desc_phys, (uint32_t)UHCI_DESC_FRAMES);
        pmm_free_frames((void *)uhci_frame_phys, 1);
        uhci_frame_phys = 0;
        uhci_frame_list = 0;
        uhci_desc_phys  = 0;
        uhci_desc_virt  = 0;
        return -1;
    }
    uhci_bounce_phys = phys;
    uhci_bounce_virt = (uint8_t *)P2V(phys);

    return 0;
}

static void uhci_free(void)
{
    if(uhci_bounce_phys != 0)
    {
        pmm_free_frames((void *)uhci_bounce_phys, (uint32_t)UHCI_BOUNCE_FRAMES);
        uhci_bounce_phys = 0;
    }
    if(uhci_desc_phys != 0)
    {
        pmm_free_frames((void *)uhci_desc_phys, (uint32_t)UHCI_DESC_FRAMES);
        uhci_desc_phys = 0;
    }
    if(uhci_frame_phys != 0)
    {
        pmm_free_frames((void *)uhci_frame_phys, 1);
        uhci_frame_phys = 0;
    }
}

/* The number of trailing zero bits, which decides how often a frame runs the
*  longer-interval queues. Frame 0 has none to count and gets the longest. */
static int uhci_entry_level(int frame)
{
    int level;

    if(frame == 0)
    {
        return UHCI_INT_QHS - 1;
    }

    level = 0;
    while((frame & 1) == 0 && level < UHCI_INT_QHS - 1)
    {
        frame >>= 1;
        level++;
    }
    return level;
}

/* Builds the skeleton described above UHCI_INT_QHS. Runs before FRBASEADD is
*  written and before the controller is started, so there is no ordering
*  question here at all -- which is the entire reason the skeleton is built
*  once and then treated as immutable. */
static void uhci_build_schedule(void)
{
    int i;

    for(i = UHCI_INT_QHS - 1; i > 0; i--)
    {
        uhci_qhs[i].link    = uhci_qh_phys(i - 1) | UHCI_LINK_QH;
        uhci_qhs[i].element = UHCI_LINK_TERM;
    }

    uhci_qhs[0].link    = uhci_qh_phys(UHCI_QH_CONTROL) | UHCI_LINK_QH;
    uhci_qhs[0].element = UHCI_LINK_TERM;

    uhci_qhs[UHCI_QH_CONTROL].link    = uhci_qh_phys(UHCI_QH_BULK) |
                                        UHCI_LINK_QH;
    uhci_qhs[UHCI_QH_CONTROL].element = UHCI_LINK_TERM;

    /* Bulk ends the chain. It is deliberately NOT looped back to itself for
    *  bandwidth reclamation: that trick fills the tail of every frame with
    *  retries of whatever is at the head of the bulk queue, which is fine when
    *  that endpoint is answering and is a permanent stream of NAK handshakes
    *  when it is not. The bandwidth it reclaims is not worth a bus that is
    *  never idle. */
    uhci_qhs[UHCI_QH_BULK].link    = UHCI_LINK_TERM;
    uhci_qhs[UHCI_QH_BULK].element = UHCI_LINK_TERM;

    for(i = 0; i < UHCI_FRAME_COUNT; i++)
    {
        uhci_frame_list[i] =
            uhci_qh_phys(uhci_entry_level(i)) | UHCI_LINK_QH;
    }
}

/* Starts the controller and then CHECKS that it started.
*
*  This is the step uhci_frames() exists for. Writing RS always succeeds, the
*  registers all read back plausibly afterwards, every port still reports what
*  is plugged into it, and a controller that is not actually scheduling looks
*  exactly like one that is -- until the first transfer times out with no
*  explanation. The frame number is the one thing that can tell them apart, so
*  it is read, waited on and read again before this reports success. */
static int uhci_start(void)
{
    uint16_t before;
    uint16_t after;
    int i;

    uhci_outl((uint16_t)(uhci_io + UHCI_FRBASEADD), uhci_frame_phys);

    /* Read it back. The frame list pointer is the one register whose value the
    *  controller has to have accepted before anything else means anything, and
    *  a write to a controller that is not really there -- or that is still
    *  held in reset -- lands nowhere and reads back as zero or as all ones.
    *  Only the top twenty bits are the address; the rest is reserved. */
    if((uhci_inl((uint16_t)(uhci_io + UHCI_FRBASEADD)) & 0xFFFFF000UL) !=
       (uhci_frame_phys & 0xFFFFF000UL))
    {
        return -1;
    }

    uhci_outw((uint16_t)(uhci_io + UHCI_FRNUM), 0);
    outportb((uint16_t)(uhci_io + UHCI_SOFMOD), 0x40);
    uhci_outw((uint16_t)(uhci_io + UHCI_USBSTS), (uint16_t)UHCI_STS_ALL);

    uhci_outw((uint16_t)(uhci_io + UHCI_USBCMD),
              (uint16_t)(UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP));

    before = (uint16_t)(uhci_inw((uint16_t)(uhci_io + UHCI_FRNUM)) &
                        UHCI_FRNUM_MASK);

    for(i = 0; i < 20; i++)
    {
        uhci_delay_ms(1);
        after = (uint16_t)(uhci_inw((uint16_t)(uhci_io + UHCI_FRNUM)) &
                           UHCI_FRNUM_MASK);
        if(after != before)
        {
            uhci_frame_last  = after;
            uhci_frame_total = 0;
            return 0;
        }
    }

    return -1;
}

static void uhci_stop(void)
{
    uhci_outw((uint16_t)(uhci_io + UHCI_USBCMD), 0);
    uhci_outw((uint16_t)(uhci_io + UHCI_USBINTR), 0);
    uhci_outw((uint16_t)(uhci_io + UHCI_USBSTS), (uint16_t)UHCI_STS_ALL);
}

static const pci_device *uhci_find(void)
{
    const pci_device *dev;
    int count;
    int i;

    count = pci_count();
    for(i = 0; i < count; i++)
    {
        dev = pci_get(i);
        if(dev == 0)
        {
            continue;
        }
        if(dev->class_code == UHCI_PCI_CLASS &&
           dev->subclass   == UHCI_PCI_SUBCLASS &&
           dev->prog_if    == UHCI_PCI_PROGIF)
        {
            return dev;
        }
    }
    return 0;
}

static void uhci_build_info(uint16_t legsup_before, uint16_t legsup_after)
{
    int pos;

    pos = uhci_put_str(0, "UHCI at I/O 0x");
    pos = uhci_put_hex(pos, (uint32_t)uhci_io, 4);
    pos = uhci_put_str(pos, ", IRQ ");
    pos = uhci_put_dec(pos, (uint32_t)uhci_irq);
    pos = uhci_put_str(pos, ", ");
    pos = uhci_put_dec(pos, (uint32_t)uhci_ports);
    pos = uhci_put_str(pos, " ports, LEGSUP 0x");
    pos = uhci_put_hex(pos, (uint32_t)legsup_before, 4);
    pos = uhci_put_str(pos, "->0x");
    pos = uhci_put_hex(pos, (uint32_t)legsup_after, 4);
    pos = uhci_put_str(pos, ((legsup_before & UHCI_LEGSUP_TRAPS) != 0)
                            ? " (BIOS emulation was on)"
                            : " (BIOS emulation was off)");
    pos = uhci_put_str(pos, ", polled");

    uhci_info_buf[pos] = '\0';
}

int uhci_init(void)
{
    const pci_device *dev;
    uint16_t legsup_before;
    uint16_t legsup_after;
    uint16_t io;
    int i;

    if(uhci_ready)
    {
        return 0;
    }

    dev = uhci_find();
    if(dev == 0)
    {
        /* The ordinary outcome on a machine QEMU started without a USB
        *  controller, and on real hardware built in the last decade. Nothing
        *  here may keep the boot from finishing. */
        uhci_set_info("UHCI: no controller (PCI class 0C:03 prog-if 00)");
        return -1;
    }

    io = pci_io_base(dev, UHCI_BAR);
    if(io == 0)
    {
        for(i = 0; i < 6; i++)
        {
            io = pci_io_base(dev, i);
            if(io != 0)
            {
                break;
            }
        }
    }
    if(io == 0)
    {
        uhci_set_info("UHCI: the controller has no I/O region");
        return -1;
    }

    uhci_io  = io;
    uhci_irq = dev->irq;

    /* Before anything else: take the controller off the BIOS. */
    legsup_before = uhci_release_bios(dev);
    legsup_after  = pci_read16(dev->bus, dev->slot, dev->func,
                               UHCI_PCI_LEGSUP);

    /* I/O space so the registers answer, and bus mastering because the
    *  controller fetches the frame list, the queue heads and every descriptor
    *  by itself. Without the second one it runs, reports no error anywhere,
    *  and moves not one byte. */
    pci_enable(dev, (uint16_t)(PCI_CMD_IO | PCI_CMD_MASTER));

    if(uhci_reset() != 0)
    {
        uhci_set_info("UHCI: the host controller reset never completed");
        return -1;
    }

    uhci_ports = uhci_count_ports();
    if(uhci_ports <= 0)
    {
        uhci_set_info("UHCI: the controller reports no root ports");
        return -1;
    }
    if(uhci_ports > UHCI_MAX_PORTS)
    {
        uhci_ports = UHCI_MAX_PORTS;
    }

    if(uhci_alloc() != 0)
    {
        uhci_set_info("UHCI: no contiguous memory for the schedule");
        return -1;
    }

    uhci_build_schedule();

    for(i = 0; i < UHCI_SLOT_COUNT; i++)
    {
        uhci_slot_busy[i] = 0;
    }

    if(uhci_start() != 0)
    {
        uhci_stop();
        uhci_free();
        uhci_set_info("UHCI: started, but the frame number never advanced");
        return -1;
    }

    uhci_ready = 1;

    /* The frame number register is eleven bits and wraps once a second, so the
    *  running total is kept by sampling it from the timer tick -- one sample
    *  per frame, which cannot miss a wrap. uhci_frames() also samples on
    *  demand, so losing this hook costs accuracy over long idle stretches and
    *  nothing else. */
    uhci_frame_hooked = (timer_install_handler(uhci_frame_tick) >= 0);

    uhci_build_info(legsup_before, legsup_after);

    if(usb_register_hc(&uhci_ops) != 0)
    {
        uhci_ready = 0;
        if(uhci_frame_hooked)
        {
            timer_uninstall_handler(uhci_frame_tick);
            uhci_frame_hooked = 0;
        }
        uhci_stop();
        uhci_free();
        uhci_set_info("UHCI: the USB core refused the registration");
        return -1;
    }

    return 0;
}
