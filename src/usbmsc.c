/* TomatOS - USB mass storage class driver
*  Desc: SCSI commands inside the bulk-only transport, presented upwards as
*        ordinary 512 byte block devices.
*
*  WHERE THIS SITS. src/usb.c enumerates whatever is plugged in and stops at
*  the class triple; src/blockdev.h says what a thing that can hand over a
*  sector has to look like. This file is the whole distance between those two
*  sentences, and it is longer than it sounds because there are three protocols
*  stacked on top of each other before a sector appears:
*
*      blk_read(4, 0, 1, buf)          the filesystem's question
*        -> READ(10), a SCSI command   ten bytes, big endian, from 1986
*          -> bulk-only transport      three USB transfers per command
*            -> usb_bulk()             the controller moves the bytes
*
*  Each layer has its own idea of what an error is, and most of the difficulty
*  here is refusing to let one layer's opinion be read as another's. A device
*  that says "I will not do that" has answered the question; a device that says
*  nothing has not. Those are the same negative number to a caller that does
*  not look, and one of them means the data in the buffer is somebody else's.
*
*  WHY BULK-ONLY AND NOT CBI. There are two transports in the mass storage
*  class. CBI (control/bulk/interrupt) puts the command in a control transfer
*  and is what the earliest sticks and the floppy drives used; bulk-only puts
*  it in a wrapper on the bulk OUT endpoint. Bulk-only won completely -- the
*  specification for CBI was withdrawn in 2003, and every device made since is
*  0x50 -- so this file implements bulk-only and claims nothing else. A device
*  with protocol 0x00 or 0x01 is left unclaimed rather than half-driven.
*
*  WHY SCSI TRANSPARENT AND NOT RBC OR ATAPI. Subclass 6 means "the command
*  block is a SCSI command and the device does not care which SCSI standard you
*  think you are speaking". Subclasses 2 and 5 are ATAPI command sets for
*  CD-ROMs and floppies, and subclass 4 is UFI. Every USB stick is subclass 6;
*  the others are twelve-byte command blocks with different opcodes, and none
*  of them are 512 byte media anyway (see the sector size decision below).
*
*  WHAT IS DELIBERATELY NOT HERE:
*
*    - Hot plug. usb.c enumerates once at boot and has no port change path, so
*      a stick plugged in later is not found. One REMOVED while mounted has to
*      be survived, though, because this file keeps talking to it -- see
*      usbmsc_lost().
*    - Caching, read-ahead and write-back. Every blk_read() is a real command
*      on the wire. The filesystem above has its own ideas about that and this
*      is not the layer to have a second set.
*    - SYNCHRONIZE CACHE, START STOP UNIT and PREVENT ALLOW MEDIUM REMOVAL.
*      The first matters for a device with a write cache and no power-loss
*      protection, and it is the honest thing to send before an unmount that
*      this kernel does not have; it is written down here rather than done.
*    - MODE SENSE, and therefore the write protect bit. A write to a
*      write-protected device is attempted and fails with a sense key this file
*      does report, which is a worse user experience and the same amount of
*      data loss, i.e. none.
*    - READ CAPACITY(16) and the 64 bit LBA. This kernel has no 64 bit
*      arithmetic in it on purpose, and a device with more than 2^32 sectors is
*      refused rather than silently truncated -- see usbmsc_read_capacity().
*/

#include <system.h>
#include <string.h>
#include <usb.h>
#include <blockdev.h>

/* --- What this file offers upwards ---------------------------------------
*
*  There is no usbmsc.h, for the same reason there is no usbhid.h: usb.h
*  declares the HID driver's entry points inline because it was the first class
*  driver and there was exactly one. This is the second, and the moment to
*  split them into headers is the moment somebody edits usb.h -- which is
*  frozen. So the declarations live here, and any caller (kernel.c for the
*  init, main.c for the shell command) repeats the four or five prototypes it
*  actually uses. That is ugly and it is written down as such.
*
*  usbmsc_init() is called once, after usb_init() and after blk_init(), from
*  the same place usbhid_init() is called. Order matters against blk_init()
*  only: the ATA driver claims numbers 0..3 there, and this registers above
*  them, so registering first would work but would leave a stick numbered 4 on
*  a machine where blk_init() then failed to claim anything -- confusing rather
*  than wrong. */
extern void usbmsc_init(void);

extern int  usbmsc_present(void);
extern int  usbmsc_count(void);

/* One line about the unit at "index": what it is, how big, which block device
*  number it took. A static buffer, never null. */
extern const char *usbmsc_describe(int index);

/* The block device number the unit at "index" registered on, or -1 for a unit
*  that was found and refused. This is what lets a shell say "mount 4" next to
*  the line describing the stick rather than making the user guess. */
extern int  usbmsc_blkdev(int index);

/* Counters, for a shell that wants to show whether the transport is healthy.
*  commands is every SCSI command attempted; failures is the ones the DEVICE
*  rejected (a real answer, with a sense code behind it); errors is the ones
*  the TRANSPORT lost, which is the number that matters when a stick stops
*  working; stalls and resets are the two recovery paths, and a resets count
*  that climbs is a device or a controller in trouble. */
extern uint32_t usbmsc_commands(void);
extern uint32_t usbmsc_failures(void);
extern uint32_t usbmsc_errors(void);
extern uint32_t usbmsc_stalls(void);
extern uint32_t usbmsc_resets(void);

/* Why the last thing that failed, failed. Empty when nothing has. */
extern const char *usbmsc_last_error(void);

/* --- The class ----------------------------------------------------------- */

/* Subclass 6 is "SCSI transparent command set", protocol 0x50 is bulk-only.
*  Both are checked: a subclass 2 device (ATAPI) speaks twelve byte command
*  blocks with different opcodes, and driving it with the commands below would
*  produce plausible looking failures rather than obvious ones. */
#define MSC_SUBCLASS_SCSI       0x06
#define MSC_PROTOCOL_BULK_ONLY  0x50

/* The two class requests, both to the INTERFACE. */
#define MSC_REQ_RESET           0xFF   /* Bulk-Only Mass Storage Reset      */
#define MSC_REQ_GET_MAX_LUN     0xFE

#define MSC_IN_FROM_IFACE  (USB_DIR_IN  | USB_TYPE_CLASS | USB_RECIP_IFACE)
#define MSC_OUT_TO_IFACE   (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_IFACE)

/* CLEAR_FEATURE(ENDPOINT_HALT): feature selector 0, recipient endpoint. The
*  same constant usbhid.c defines for the same request; it is two lines and a
*  shared header for it would be an edit to usb.h. */
#define USB_FEATURE_ENDPOINT_HALT 0

/* --- The two wrappers ----------------------------------------------------
*
*  A bulk-only command is three transfers and never fewer:
*
*      1. 31 bytes OUT   the Command Block Wrapper, carrying the SCSI command
*      2. the data       IN or OUT, or nothing at all
*      3. 13 bytes IN    the Command Status Wrapper
*
*  Both wrappers are LITTLE endian, because they are USB structures. The SCSI
*  command inside the CBW is BIG endian, because it is SCSI. They are eleven
*  bytes apart in the same buffer and getting them the same way round is the
*  single most productive way to make a 16 MiB stick report itself as 4 TiB.
*
*  THE TAG is the whole reason the status can be trusted. It is an arbitrary
*  number the host puts in the CBW, and the device must echo it in the CSW. A
*  CSW carrying a different one is not a status for the command that was just
*  sent -- it is a leftover from an earlier one, which is exactly what a device
*  that was interrupted mid-command produces. Believing it means reporting one
*  command's success as another's, and on a read that means handing the
*  filesystem the previous sector under the current sector's number. See
*  usbmsc_check_csw(). */

#define CBW_SIGNATURE   0x43425355UL   /* "USBC", little endian on the wire */
#define CSW_SIGNATURE   0x53425355UL   /* "USBS"                            */

#define CBW_FLAG_IN     0x80           /* bit 7 of bmCBWFlags: device to host */

#define CBW_LENGTH      31
#define CSW_LENGTH      13
#define CBW_CB_MAX      16

typedef struct
{
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;      /* what the host promises to move             */
    uint8_t  flags;            /* CBW_FLAG_IN, or 0 for out and for no data  */
    uint8_t  lun;              /* low four bits                              */
    uint8_t  cb_length;        /* 1..16, the real length of the SCSI command */
    uint8_t  cb[CBW_CB_MAX];
} __attribute__((packed)) msc_cbw;

typedef struct
{
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;          /* promised minus processed; see below        */
    uint8_t  status;
} __attribute__((packed)) msc_csw;

/* bCSWStatus. Three values and they mean three completely different things.
*
*    0  the command was carried out.
*    1  THE COMMAND FAILED AND THE TRANSPORT DID NOT. The device understood
*       the wrapper perfectly and is refusing the SCSI command inside it --
*       no medium, not ready yet, write protected, bad block. The transport is
*       healthy and the way to find out why is REQUEST SENSE, which is another
*       full three-phase command. Treating this as a bus error means resetting
*       a working device because a disk was empty.
*    2  phase error. The device has lost track of where it is in the protocol
*       and the only defined way back is the class's own reset recovery, which
*       is heavier than clearing a stall and is the only thing that clears it.
*       A retry without the reset talks to a device that is not listening. */
#define CSW_PASS        0
#define CSW_FAIL        1
#define CSW_PHASE_ERROR 2

/* --- The SCSI commands used ----------------------------------------------
*
*  Five, and every one of them is here because something above cannot be
*  answered without it. Opcodes are the ones from SBC/SPC that every device
*  claiming subclass 6 implements; anything more adventurous is what the
*  "deliberately not here" list at the top is for. */
#define SCSI_TEST_UNIT_READY   0x00
#define SCSI_REQUEST_SENSE     0x03
#define SCSI_INQUIRY           0x12
#define SCSI_READ_CAPACITY10   0x25
#define SCSI_READ10            0x28
#define SCSI_WRITE10           0x2A

#define SCSI_CDB6              6
#define SCSI_CDB10             10

#define SCSI_INQUIRY_LEN       36
#define SCSI_SENSE_LEN         18
#define SCSI_CAPACITY_LEN      8

/* Sense keys, from the fixed format sense data. Only the four this file acts
*  on are named; the rest are reported as a number and no more. */
#define SENSE_NO_SENSE         0x00
#define SENSE_NOT_READY        0x02
#define SENSE_MEDIUM_ERROR     0x03
#define SENSE_ILLEGAL_REQUEST  0x05
#define SENSE_UNIT_ATTENTION   0x06
#define SENSE_DATA_PROTECT     0x07

/* Additional sense codes worth telling apart. 04/01 is "becoming ready",
*  i.e. spinning up, and is the one that deserves to be waited out; 3A is "no
*  medium", which is a card reader with no card in it and is not going to
*  improve by asking again. */
#define ASC_NOT_READY_BECOMING 0x04
#define ASC_NO_MEDIUM          0x3A

/* Peripheral device type, the low five bits of INQUIRY byte 0. 0x00 is a
*  direct access block device (every stick) and 0x0E is the reduced block
*  command set (a few flash devices). 0x05 is a CD-ROM, which is 2048 byte
*  sectors and is refused later anyway; naming these two is what lets a
*  non-block device be refused before a capacity is even asked for. */
#define SCSI_TYPE_DIRECT       0x00
#define SCSI_TYPE_RBC          0x0E

/* The peripheral QUALIFIER, INQUIRY byte 0 bits 5..7. 3 means "this logical
*  unit number is not connected", which is how a device with three LUNs and one
*  card in it answers for the other two. It is an ordinary answer and the LUN
*  is skipped, not failed. */
#define SCSI_QUALIFIER_NONE    0x03

/* --- Tuning -------------------------------------------------------------- */

/* Interfaces claimed at once. USB_MAX_DEVICES is eight and there are two root
*  ports with no hub support, so two is already more than can be plugged in. */
#define USBMSC_MAX_IFACES 2

/* Units -- (interface, LUN) pairs -- brought up at once. blockdev.h has room
*  for exactly this many above the ATA drives, so a fifth would have nowhere to
*  register and there is no point discovering it. */
#define USBMSC_MAX_UNITS  (BLK_MAX_DEVICES - BLK_REMOVABLE_FIRST)

/* The highest LUN this file will probe, whatever GET_MAX_LUN says. The
*  specification allows 15; a device claiming that many would fill the block
*  device table on its own and squeeze out the second port. */
#define USBMSC_MAX_LUN    3

/* Sectors moved in one SCSI command.
*
*  EIGHT, AND IT IS THE CONTROLLER'S NUMBER RATHER THAN THIS FILE'S. uhci.c
*  refuses a bulk transfer longer than 4096 bytes -- it builds one transfer
*  descriptor per 64 byte packet out of a fixed pool and has 80 of them per
*  slot -- so a ninth sector would come back USB_EINVAL without a byte
*  crossing the wire. Chunking is needed regardless, because the filesystem
*  above is entitled to ask for a cluster at a time and a cluster can be 64
*  sectors; the only thing the constant decides is how many commands that
*  costs. It lives here rather than in a header because usb.h deliberately does
*  not expose a controller's limits, and a future controller that can do more
*  would make this number pessimistic rather than wrong. */
#define USBMSC_MAX_SECTORS 8

/* How long to keep asking a device that says it is not ready yet.
*
*  A stick answers TEST UNIT READY immediately. A spinning disk in a USB
*  enclosure, and a card reader that has just had a card pushed in, answer
*  "not ready, becoming ready" for as long as it takes -- typically a second,
*  occasionally five. So the wait is generous, and it is spent ONLY on that one
*  sense code: "no medium" and every other failure give up on the first answer,
*  because none of them get better by asking again and five seconds per empty
*  card slot at every boot is five seconds nobody agreed to.
*
*  A UNIT ATTENTION -- "I have been reset since you last spoke to me", which a
*  device produces at power-up and after the reset recovery below -- is retried
*  too, without the sleep. It is the classic "ask it twice" case: the condition
*  is cleared by the very act of reporting it, so the second command succeeds. */
#define USBMSC_READY_TRIES   50
#define USBMSC_READY_WAIT_MS 100

/* Attempts at one read or write before it is handed back as a failure. Two,
*  because the recoveries below are worth exactly one retry each: a stall was
*  cleared, or a reset recovery was done, and if the command fails a second
*  time the problem is not one this file knows how to fix. Retrying forever
*  turns an unplugged stick into a task that never returns. */
#define USBMSC_TRIES 2

/* How long a command waits for the transport lock before giving up, and how
*  many times. The lock is held for the length of one SCSI command, which the
*  controller bounds at one second per transfer, so three seconds of waiting is
*  "the other task's command is genuinely still running" and anything longer is
*  a leak worth reporting rather than waiting out. */
#define USBMSC_LOCK_WAIT_MS 1000
#define USBMSC_LOCK_TRIES   3

/* Room for the lines handed to the shell and to blk_describe(). */
#define USBMSC_TEXT  72
#define USBMSC_MODEL 32

/* --- State --------------------------------------------------------------- */

/* One claimed interface: the pair of bulk endpoints and everything that is
*  shared by every LUN behind them. The transport lock is here and not in the
*  unit, because the endpoints are what is shared -- two tasks reading two LUNs
*  of one card reader are two command wrappers going down the same pipe, and
*  the second CBW arriving before the first CSW has been collected is precisely
*  the tag mismatch this file spends a page explaining. */
typedef struct
{
    int         used;
    int         live;          /* 0 once the device stopped answering       */
    usb_device *dev;           /* into usb.c's table, which never moves     */
    int         ep_in;         /* bulk IN endpoint number                   */
    int         ep_out;        /* bulk OUT endpoint number                  */
    uint8_t     max_lun;

    /* The next tag to put in a CBW. Starts at something recognisable rather
    *  than at 0, so that a wrapper dumped from a bus trace is obviously this
    *  driver's and a zeroed buffer read back as a CSW does not match. */
    uint32_t    tag;

    /* The transport lock. See usbmsc_acquire(). */
    int         busy;

    /* The sense data of the last command that the device rejected, kept per
    *  interface because it is fetched under the same lock as the command that
    *  produced it and read immediately afterwards. */
    uint8_t     sense_key;
    uint8_t     sense_asc;
    uint8_t     sense_ascq;
    int         sense_valid;
} usbmsc_iface;

/* One logical unit, which is what becomes a block device. */
typedef struct
{
    int           used;
    usbmsc_iface *bus;
    uint8_t       lun;

    /* Sectors of 512 bytes, as a COUNT. READ CAPACITY returns the last LBA and
    *  the conversion happens once, here, at bring-up. Everything else in this
    *  file compares against a count. */
    uint32_t      sectors;

    /* The block device number it registered on, or -1 for a unit that was
    *  found and refused -- an odd sector size, a capacity that will not fit.
    *  Such a unit stays in the table on purpose: "there is a stick here and
    *  this is why you cannot mount it" is worth a line, and a unit that simply
    *  vanished is a bug report nobody can act on. */
    int           blk_dev;

    char          model[USBMSC_MODEL];
    char          text[USBMSC_TEXT];
} usbmsc_unit;

static usbmsc_iface usbmsc_ifaces[USBMSC_MAX_IFACES];
static usbmsc_unit  usbmsc_units[USBMSC_MAX_UNITS];
static int          usbmsc_iface_count = 0;
static int          usbmsc_unit_count  = 0;
static int          usbmsc_started     = 0;

static uint32_t usbmsc_stat_commands = 0;
static uint32_t usbmsc_stat_failures = 0;
static uint32_t usbmsc_stat_errors   = 0;
static uint32_t usbmsc_stat_stalls   = 0;
static uint32_t usbmsc_stat_resets   = 0;

static const char *usbmsc_error_text = "";

/* What goes into usb_device.driver. Static storage, because usb.c keeps the
*  pointer rather than a copy. Seven characters, because "lsusb" prints this
*  in a column nine wide and keeps eight of them. */
static const char usbmsc_name[]      = "usb-msc";
static const char usbmsc_name_gone[] = "removed";

/* What blk_bus() reports for every device this file registers. blockdev.h asks
*  for a short phrase and names this one in its own comment. */
static const char usbmsc_bus_name[] = "USB storage";

/* --- Small helpers ------------------------------------------------------- */

/* Interrupts off and back to exactly what they were. The same four lines kb.c,
*  mouse.c and net.c each carry privately; there is no global pair, and a new
*  cross-file interface for eight lines is not worth an edit to system.h. */
static unsigned long usbmsc_irq_save(void)
{
    unsigned long flags;

    __asm__ __volatile__ ("pushfl; popl %0; cli"
                          : "=r" (flags) : : "memory");
    return flags;
}

static void usbmsc_irq_restore(unsigned long flags)
{
    __asm__ __volatile__ ("pushl %0; popfl"
                          : : "r" (flags) : "memory", "cc");
}

/* Two string builders, the same pair usbhid.c carries and for the same reason:
*  there is no snprintf here and printf is deliberately not included, because a
*  driver that writes to the console during boot competes with the code laying
*  the console out. Both take and return the write position and always leave
*  the buffer terminated. */
static int usbmsc_put(char *out, int at, int max, const char *text)
{
    while(*text != '\0' && at < max - 1)
    {
        out[at] = *text;
        at++;
        text++;
    }

    out[at] = '\0';
    return at;
}

static int usbmsc_put_uint(char *out, int at, int max, uint32_t value)
{
    char tmp[12];
    int  n;

    n = 0;
    do
    {
        tmp[n] = (char)('0' + (int)(value % 10UL));
        n++;
        value /= 10UL;
    } while(value != 0 && n < 11);

    while(n > 0 && at < max - 1)
    {
        n--;
        out[at] = tmp[n];
        at++;
    }

    out[at] = '\0';
    return at;
}

/* --- Big endian, which SCSI is and this machine is not --------------------
*
*  USB is little endian and so is an i386, which is why usb.h says in as many
*  words that nothing in it needs converting. SCSI is big endian, was specified
*  for machines that were, and does not care what the host is. So every
*  multi-byte number inside a command block and inside a SCSI data-in buffer
*  has to be turned round by hand, and there are exactly four places where that
*  matters:
*
*    - the LBA in READ(10) and WRITE(10)      put_be32
*    - the block count in the same            put_be16
*    - the last LBA from READ CAPACITY        get_be32
*    - the block length from READ CAPACITY    get_be32
*
*  Getting the last two the wrong way round is the classic symptom: a 16 MiB
*  stick has 32767 sectors of 512 bytes, so its READ CAPACITY answers
*  00 00 7F FF / 00 00 02 00. Read as little endian that is 4294934528 sectors
*  of 33554432 bytes -- a device that reports several petabytes and a sector
*  size no allocator will hand out. The failure is loud, which is the only
*  merciful thing about it.
*
*  Written as byte pokes rather than as a cast to a struct, because a packed
*  struct with a __builtin_bswap in it would hide the one thing this code
*  exists to make visible. */
static void usbmsc_put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)((value >> 24) & 0xFFUL);
    p[1] = (uint8_t)((value >> 16) & 0xFFUL);
    p[2] = (uint8_t)((value >> 8)  & 0xFFUL);
    p[3] = (uint8_t)(value         & 0xFFUL);
}

static void usbmsc_put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)((value >> 8) & 0xFF);
    p[1] = (uint8_t)(value        & 0xFF);
}

static uint32_t usbmsc_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
            (uint32_t)p[3];
}

/* --- The transport lock --------------------------------------------------
*
*  WHY THERE IS ONE AT ALL. blockdev.h says neither read nor write may be
*  called from interrupt context and that both may block, which means both are
*  called from a task -- and this kernel has several. The shell can be reading
*  a directory while a program reads a file; both end up in blk_read() on the
*  same stick. Without a lock the second task's CBW goes down the pipe between
*  the first task's CBW and its CSW, and what comes back is one CSW carrying
*  the other command's tag and one command's data delivered under the other's
*  LBA. That is not a hang or a crash: it is the wrong sector, silently, which
*  is the worst failure a block device has.
*
*  A flag and a wait rather than disabling interrupts, because the thing being
*  protected takes milliseconds -- three USB transfers, up to a second each --
*  and a critical section that long with interrupts off would stop the clock.
*
*  Bounded, and it gives up rather than waiting forever: a caller that gets a
*  refusal reports a failed read, whereas a caller that never returns takes its
*  task with it. From the boot path, before there is a scheduler, task_wait()
*  reports a timeout immediately -- and that is correct here, because with one
*  thread of execution the flag can only be set by a command that never
*  released it, which is a bug to report rather than to wait out. */
static int usbmsc_acquire(usbmsc_iface *bus)
{
    unsigned long flags;
    int           tries;

    tries = 0;

    flags = usbmsc_irq_save();

    while(bus->busy)
    {
        if(!task_wait(&bus->busy, USBMSC_LOCK_WAIT_MS))
        {
            tries++;
            if(tries >= USBMSC_LOCK_TRIES)
            {
                usbmsc_irq_restore(flags);
                usbmsc_error_text = "USB storage: the transport stayed busy";
                return 0;
            }
        }
    }

    bus->busy = 1;
    usbmsc_irq_restore(flags);
    return 1;
}

static void usbmsc_release(usbmsc_iface *bus)
{
    unsigned long flags;

    flags = usbmsc_irq_save();
    bus->busy = 0;
    usbmsc_irq_restore(flags);

    task_wake(&bus->busy);
}

/* --- Recovery -------------------------------------------------------------
*
*  Two mechanisms, and the whole point is that they are different sizes.
*
*  A STALL ON A BULK ENDPOINT is the small one and is not an error in the bus
*  sense at all. The device is saying "I am not going to move those bytes", and
*  in this class it says it on purpose: a READ(10) for a block that does not
*  exist ends with the data endpoint stalled and a perfectly good CSW waiting
*  behind it. The endpoint is HALTED until it is cleared, so every subsequent
*  transfer on it fails identically and a driver that does not clear it looks
*  exactly like a driver talking to an unplugged device.
*
*  CLEAR_FEATURE(ENDPOINT_HALT) unhalts it and, by specification, resets the
*  data toggle to DATA0 AT BOTH ENDS. So the host's copy has to be put back as
*  well, or the very next packet is discarded by the device as a
*  retransmission, and the one after that, forever -- off by one packet is not
*  a corruption that shows up later, it is an endpoint that goes permanently
*  quiet. usb.h keeps the host's copy in usb_endpoint.toggle for exactly this,
*  and usbhid.c does the same thing for its interrupt endpoint. */
static usb_endpoint *usbmsc_endpoint(usbmsc_iface *bus, int number, int in)
{
    int i;

    for(i = 0; i < bus->dev->endpoints; i++)
    {
        if((int)bus->dev->endpoint[i].address != number)
            continue;

        if(in && (bus->dev->endpoint[i].direction & USB_DIR_IN) == 0)
            continue;

        if(!in && (bus->dev->endpoint[i].direction & USB_DIR_IN) != 0)
            continue;

        return &bus->dev->endpoint[i];
    }

    return 0;
}

/* Clears one halted bulk endpoint and puts the host's toggle back to match.
*  Returns 1 when the device acknowledged; a device that cannot even answer a
*  control transfer is gone, and saying so is more useful than retrying. */
static int usbmsc_clear_halt(usbmsc_iface *bus, int number, int in)
{
    usb_endpoint *ep;
    int           result;

    usbmsc_stat_stalls++;

    result = usb_control(bus->dev,
                         USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                         USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
                         (uint16_t)((in ? USB_DIR_IN : USB_DIR_OUT) | number),
                         0, 0);

    /* The toggle is reset whether or not the request was acknowledged. If it
    *  succeeded the device is at DATA0 and the host must match; if it failed
    *  the endpoint is in an unknown state and DATA0 is the only value that is
    *  ever right after a clear, so it is no worse. Leaving it alone would be
    *  the one choice that is wrong in both cases. */
    ep = usbmsc_endpoint(bus, number, in);
    if(ep != 0)
        ep->toggle = 0;

    return (result >= 0);
}

/* Reset recovery, the heavy one. USB Mass Storage Bulk-Only 5.3.4.
*
*  WHAT IT IS FOR. A phase error (CSW status 2) or a CSW that is not a CSW at
*  all -- wrong signature, wrong tag, wrong length -- means the two ends
*  disagree about which phase of which command they are in. Nothing smaller
*  fixes that: clearing a halt on one endpoint leaves the device still
*  convinced it owes a data phase, and the next command's CBW is read as that
*  data. Which is how a driver ends up writing a command wrapper into a sector.
*
*  THREE STEPS AND ALL THREE ARE REQUIRED:
*
*    1. the class request itself, which tells the device to abandon whatever
*       command it thinks it is in the middle of and get ready for a CBW. It
*       explicitly does NOT clear the endpoint halts and explicitly does NOT
*       reset the data toggles -- the specification says the device preserves
*       both across it, which is why the next two steps exist.
*    2. CLEAR_FEATURE(ENDPOINT_HALT) on the bulk IN endpoint.
*    3. the same on the bulk OUT endpoint.
*
*  Each clear resets that endpoint's toggle at both ends, so after all three
*  the two sides agree on the phase AND on the toggle, which is the actual
*  definition of "recovered".
*
*  Returns 1 when every step was acknowledged. A device that fails any of them
*  is not answering control transfers either, which means it is gone; the
*  caller marks it lost rather than driving a command into it. */
static int usbmsc_reset_recovery(usbmsc_iface *bus)
{
    int ok;

    usbmsc_stat_resets++;

    ok = (usb_control(bus->dev, MSC_OUT_TO_IFACE, MSC_REQ_RESET,
                      0, (uint16_t)bus->dev->iface_number, 0, 0) >= 0);

    /* Both halts are cleared even when the reset request itself failed. The
    *  request is the only step that can be skipped without leaving the pipes
    *  in a worse state, and a device that refused it may still unhalt. */
    if(!usbmsc_clear_halt(bus, bus->ep_in, 1))
        ok = 0;

    if(!usbmsc_clear_halt(bus, bus->ep_out, 0))
        ok = 0;

    /* The stall counter is for stalls the DEVICE raised. The two clears above
    *  are part of the reset sequence and are raised by nothing, so they are
    *  taken back out; otherwise every reset would read as two stalls and the
    *  one counter that says "this device keeps halting its endpoints" would
    *  say it about a device that never did. */
    usbmsc_stat_stalls -= 2;

    if(!ok)
        usbmsc_error_text = "USB storage: reset recovery was not acknowledged";

    return ok;
}

/* Marks an interface, and every unit behind it, as gone.
*
*  The block device numbers are handed back, because blockdev.h says a
*  registered device that answers 0 sectors is not a device anyone can use and
*  that giving the number back is how a driver says so. Anything mounted on it
*  is the filesystem's problem, which blockdev.h also says in as many words.
*
*  The usb_device object stays in usb.c's table -- there is no removal path
*  there and inventing one from here would be reaching into another file's
*  invariants -- so what changes is the driver name the shell prints. */
static void usbmsc_lost(usbmsc_iface *bus)
{
    int i;

    if(!bus->live)
        return;

    bus->live = 0;

    if(bus->dev != 0)
        bus->dev->driver = usbmsc_name_gone;

    for(i = 0; i < usbmsc_unit_count; i++)
    {
        if(usbmsc_units[i].bus != bus)
            continue;

        if(usbmsc_units[i].blk_dev >= 0)
        {
            blk_unregister(usbmsc_units[i].blk_dev);
            usbmsc_units[i].blk_dev = -1;
        }

        usbmsc_units[i].sectors = 0;
    }

    usbmsc_error_text = "USB storage: the device stopped answering";
}

/* --- The bulk-only transport ---------------------------------------------
*
*  One SCSI command, all three phases. Everything above this function speaks
*  SCSI; everything below it speaks USB.
*
*  Returns:
*     0  the device carried the command out. "moved" holds how many data bytes
*        really crossed, which is not always what was asked for -- see the
*        residue.
*     1  the device REJECTED the command. The transport is fine and there is a
*        sense code to be had; the caller decides whether to ask for it.
*    -1  the transport failed. Nothing about the data buffer is trustworthy,
*        recovery has already been attempted, and the caller may retry once.
*
*  It does NOT take the lock. Both callers hold it -- usbmsc_command() takes it
*  around the command and its automatic REQUEST SENSE, because a sense code
*  belongs to the command that produced it and a second task's command in
*  between would clear it. */
static int usbmsc_transport(usbmsc_iface *bus, uint8_t lun,
                            const uint8_t *cdb, int cdb_len,
                            void *data, int length, int in, int *moved)
{
    msc_cbw  cbw;
    msc_csw  csw;
    uint32_t tag;
    uint32_t processed;
    int      result;
    int      actual;
    int      csw_tries;

    if(moved != 0)
        *moved = 0;

    if(bus->dev == 0 || !bus->live)
        return -1;

    if(cdb_len < 1 || cdb_len > CBW_CB_MAX)
        return -1;

    if(length < 0 || (length > 0 && data == 0))
        return -1;

    usbmsc_stat_commands++;

    /* --- Phase 1: the command wrapper ------------------------------------ */

    /* A fresh tag for every command, and it is never reused inside a run of
    *  retries either: a retry that reuses the tag cannot tell its own CSW from
    *  the stale CSW of the attempt that failed, which is the exact confusion
    *  the tag exists to prevent. */
    tag = bus->tag;
    bus->tag++;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature   = CBW_SIGNATURE;
    cbw.tag         = tag;
    cbw.data_length = (uint32_t)length;
    cbw.flags       = (uint8_t)((length > 0 && in) ? CBW_FLAG_IN : 0);
    cbw.lun         = (uint8_t)(lun & 0x0F);
    cbw.cb_length   = (uint8_t)cdb_len;
    memcpy(cbw.cb, cdb, (size_t)cdb_len);

    result = usb_bulk(bus->dev, bus->ep_out, &cbw, CBW_LENGTH, 0);

    if(result == USB_ESTALL)
    {
        /* A stall on the command phase means the device would not even take
        *  the wrapper. That is not a command failing, it is the pipe being in
        *  the wrong state, and clearing the halt alone leaves the device still
        *  waiting for whatever it was waiting for. Reset recovery. */
        usbmsc_stat_errors++;
        usbmsc_error_text = "USB storage: the command wrapper was stalled";
        if(!usbmsc_reset_recovery(bus))
            usbmsc_lost(bus);
        return -1;
    }

    if(result != CBW_LENGTH)
    {
        /* Short or failed. A CBW is 31 bytes and a device that took fewer has
        *  half a command; there is no way to finish it and no way to know what
        *  the device will do with what it has. */
        usbmsc_stat_errors++;
        usbmsc_error_text = "USB storage: the command wrapper was not sent";
        if(!usbmsc_reset_recovery(bus))
            usbmsc_lost(bus);
        return -1;
    }

    /* --- Phase 2: the data, if there is any ------------------------------ */

    actual = 0;

    if(length > 0)
    {
        result = usb_bulk(bus->dev, in ? bus->ep_in : bus->ep_out,
                          data, length, in);

        if(result == USB_ESTALL)
        {
            /* THE ONE PLACE A STALL IS ORDINARY. The device is refusing to
            *  move the rest of the data -- because the command failed, or
            *  because it has less to give than was asked for -- and it still
            *  owes a CSW, which is sitting behind the halted endpoint and
            *  explains why. So the halt is cleared and the status phase goes
            *  ahead; giving up here would throw away the one piece of
            *  information that says what happened.
            *
            *  Nothing moved as far as this file knows: uhci.c reports the
            *  stall rather than a byte count, so actual stays 0 and the
            *  residue in the CSW becomes the only account of the data phase.
            *  That is exactly what the residue is for. */
            if(!usbmsc_clear_halt(bus, in ? bus->ep_in : bus->ep_out, in))
            {
                usbmsc_stat_errors++;
                usbmsc_lost(bus);
                return -1;
            }

            actual = 0;
        }
        else if(result < 0)
        {
            usbmsc_stat_errors++;
            usbmsc_error_text = "USB storage: the data phase failed";
            if(!usbmsc_reset_recovery(bus))
                usbmsc_lost(bus);
            return -1;
        }
        else
        {
            actual = result;
            if(actual > length)
                actual = length;
        }

        /* A short read leaves the tail of the caller's buffer holding
        *  whatever was in it before. For a block device that is the previous
        *  sector, and handing it back under this sector's number is the
        *  failure this whole file is written to avoid. The caller is told the
        *  read was short and should not look -- but zeroing costs nothing
        *  next to a USB transfer and turns "somebody else's data" into
        *  "obviously nothing", which is the difference between a bug that is
        *  found and one that is not. */
        if(in && actual < length)
            memset((uint8_t *)data + actual, 0, (size_t)(length - actual));
    }

    /* --- Phase 3: the status wrapper ------------------------------------- */

    /* Read up to twice. The specification's own recovery: if the status
    *  transfer stalls, clear the halt and try once more; if it stalls again
    *  the device is not going to produce a CSW and the only way forward is
    *  reset recovery. */
    result = 0;

    for(csw_tries = 0; csw_tries < 2; csw_tries++)
    {
        memset(&csw, 0, sizeof(csw));

        result = usb_bulk(bus->dev, bus->ep_in, &csw, CSW_LENGTH, 1);

        if(result != USB_ESTALL)
            break;

        if(!usbmsc_clear_halt(bus, bus->ep_in, 1))
        {
            usbmsc_stat_errors++;
            usbmsc_lost(bus);
            return -1;
        }
    }

    if(result != CSW_LENGTH)
    {
        usbmsc_stat_errors++;
        usbmsc_error_text = "USB storage: no status wrapper came back";
        if(!usbmsc_reset_recovery(bus))
            usbmsc_lost(bus);
        return -1;
    }

    /* --- Is this a status wrapper, and is it MINE? -----------------------
    *
    *  Three checks and the middle one is the one that earns its keep.
    *
    *  The signature says these thirteen bytes are a CSW rather than the tail
    *  of a data phase the device is still sending.
    *
    *  THE TAG SAYS IT IS THE ANSWER TO THE COMMAND JUST SENT. A CSW carrying
    *  a different tag is a real thing that happens: a command that was
    *  abandoned -- a timeout that expired while the device was still working,
    *  a retry issued too early -- leaves its CSW in the endpoint, and the next
    *  read collects it. Every field in it is plausible. The status is very
    *  likely PASS, because the abandoned command probably did succeed. So a
    *  driver that does not compare the tag reports success for a command that
    *  was never answered, and on a read it reports the OTHER command's sector
    *  contents as this command's data -- the previous sector under the current
    *  number, silently, with no error anywhere.
    *
    *  WHAT IS DONE ABOUT IT: the data is discarded, the command is reported as
    *  a transport failure, and RESET RECOVERY is run. Not a simple retry --
    *  a mismatched tag means the two ends disagree about how many commands
    *  have been issued, and reading another CSW to "catch up" is a guess about
    *  how far behind the device is. The reset is the only operation defined to
    *  put both ends back at "waiting for a CBW", and the caller's retry then
    *  starts from a state both sides agree on. */
    if(csw.signature != CSW_SIGNATURE)
    {
        usbmsc_stat_errors++;
        usbmsc_error_text = "USB storage: the status wrapper is not one";
        if(!usbmsc_reset_recovery(bus))
            usbmsc_lost(bus);
        return -1;
    }

    if(csw.tag != tag)
    {
        usbmsc_stat_errors++;
        usbmsc_error_text = "USB storage: the status answered another command";
        if(!usbmsc_reset_recovery(bus))
            usbmsc_lost(bus);
        return -1;
    }

    /* --- The residue -----------------------------------------------------
    *
    *  dCSWDataResidue is what the device did NOT do: the difference between
    *  the length promised in the CBW and the length it actually processed. A
    *  device that returns less than was asked for is not necessarily failing
    *  -- a READ CAPACITY answered in eight bytes when twelve were offered is
    *  a residue of four and a perfectly successful command -- but a caller
    *  that ignores it reports whatever was left in the buffer as data.
    *
    *  A residue larger than the promised length is nonsense and is treated as
    *  a broken CSW rather than clamped: the arithmetic below would underflow
    *  into an enormous "processed", and the device has just demonstrated it is
    *  not filling the field in properly.
    *
    *  TWO ACCOUNTS OF THE SAME NUMBER, and the smaller one wins. The
    *  controller says how many bytes it moved; the device says how many it
    *  processed. They agree on a healthy transfer. When they do not -- the
    *  controller ran short and the device thinks it delivered everything, or
    *  the reverse -- the honest answer is the one that claims less, because
    *  the bytes past it were either never sent or never received and in both
    *  cases are not in the buffer. */
    if(csw.residue > (uint32_t)length)
    {
        usbmsc_stat_errors++;
        usbmsc_error_text = "USB storage: the residue is larger than the request";
        if(!usbmsc_reset_recovery(bus))
            usbmsc_lost(bus);
        return -1;
    }

    processed = (uint32_t)length - csw.residue;

    if(processed < (uint32_t)actual)
        actual = (int)processed;

    if(moved != 0)
        *moved = actual;

    /* --- The status ------------------------------------------------------ */

    if(csw.status == CSW_PASS)
        return 0;

    if(csw.status == CSW_FAIL)
    {
        /* NOT A TRANSPORT FAILURE. The device understood everything and is
        *  refusing the SCSI command inside the wrapper. The pipes are healthy,
        *  the tag matched, the residue is meaningful -- and there is a sense
        *  code waiting that says why. Resetting here would be resetting a
        *  working device because a card slot was empty. */
        usbmsc_stat_failures++;
        return 1;
    }

    /* CSW_PHASE_ERROR, or a status this specification does not define.
    *
    *  A phase error is the device saying it has lost track of the protocol --
    *  it expected data and got a wrapper, or the reverse. It is explicitly NOT
    *  recoverable by clearing a halt, and it is explicitly not a SCSI failure:
    *  there is no sense code behind it, because no SCSI command was ever
    *  properly received. Reset recovery is the defined and only response, and
    *  it is what is implemented here. An undefined status is treated the same
    *  way, because a device producing one is in a state nothing here models. */
    usbmsc_stat_errors++;
    usbmsc_error_text = "USB storage: the device reported a phase error";

    if(!usbmsc_reset_recovery(bus))
        usbmsc_lost(bus);

    return -1;
}

/* --- REQUEST SENSE --------------------------------------------------------
*
*  Why a command failing needs a second command to explain it. CSW status 1
*  carries no reason at all -- one bit of "no" -- and the reason is held by the
*  device until the next command clears it. So finding out means issuing
*  REQUEST SENSE, which is itself a full three-phase bulk-only command with its
*  own wrapper, its own data phase and its own status.
*
*  IT GOES THROUGH usbmsc_transport() AND NOT usbmsc_command(), and that is not
*  a shortcut: usbmsc_command() fetches sense automatically when a command
*  fails, so a REQUEST SENSE routed through it that itself failed would fetch
*  sense for the sense fetch, forever. One level, by construction.
*
*  A REQUEST SENSE that fails is not retried either. A device that cannot say
*  why it said no is a device that is not going to be talked into working, and
*  the caller is told there is no sense data rather than being given zeros that
*  read as "no error". */
static void usbmsc_request_sense(usbmsc_iface *bus, uint8_t lun)
{
    uint8_t cdb[SCSI_CDB6];
    uint8_t sense[SCSI_SENSE_LEN];
    int     moved;
    int     result;

    bus->sense_valid = 0;
    bus->sense_key   = 0;
    bus->sense_asc   = 0;
    bus->sense_ascq  = 0;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = SCSI_SENSE_LEN;

    memset(sense, 0, sizeof(sense));

    result = usbmsc_transport(bus, lun, cdb, SCSI_CDB6,
                              sense, SCSI_SENSE_LEN, 1, &moved);

    /* Fourteen bytes is where the three fields this file reads end. A device
    *  that answered with fewer has not described the failure, and treating a
    *  short answer as a full one would read ASC and ASCQ out of the zeroed
    *  tail -- 00/00, which decodes as "nothing is wrong". */
    if(result != 0 || moved < 14)
        return;

    /* Fixed format sense data, SPC-3 4.5.3:
    *
    *      byte 0   response code, 0x70 current or 0x71 deferred; the top bit
    *               is "information field is valid" and is not read here
    *      byte 2   the sense KEY, in the low four bits
    *      byte 12  the additional sense code (ASC)
    *      byte 13  its qualifier (ASCQ)
    *
    *  A response code of 0x72 or 0x73 is DESCRIPTOR format, which puts the key
    *  in byte 1 and the ASC in bytes 2 and 3 instead. No device claiming
    *  subclass 6 has ever been seen to use it, and misreading one as fixed
    *  format would report the key as a response code -- so it is refused
    *  rather than guessed at. */
    if((sense[0] & 0x7E) != 0x70)
        return;

    bus->sense_key   = (uint8_t)(sense[2] & 0x0F);
    bus->sense_asc   = sense[12];
    bus->sense_ascq  = sense[13];
    bus->sense_valid = 1;
}

/* --- One SCSI command, with the lock and the sense fetch -----------------
*
*  What the rest of this file calls. Same three return values as the transport;
*  the difference is that a 1 comes back with bus->sense_* already filled in,
*  fetched under the same lock, so the caller can decide what the failure means
*  without racing another task's command for the answer. */
static int usbmsc_command(usbmsc_iface *bus, uint8_t lun,
                          const uint8_t *cdb, int cdb_len,
                          void *data, int length, int in, int *moved)
{
    int result;

    if(!usbmsc_acquire(bus))
        return -1;

    bus->sense_valid = 0;

    result = usbmsc_transport(bus, lun, cdb, cdb_len, data, length, in, moved);

    if(result == 1)
        usbmsc_request_sense(bus, lun);

    usbmsc_release(bus);

    return result;
}

/* Whether a failure is one that is worth trying again.
*
*  Two sense keys, and both mean "ask me again" rather than "no":
*
*    UNIT ATTENTION (6) is the device announcing that something changed since
*    it was last spoken to -- it was powered up, it was reset (including by the
*    reset recovery above), the medium was swapped. The condition is CLEARED BY
*    BEING REPORTED, so the very next command succeeds. This is the "a device
*    may say not ready until it has been asked twice" case, and the second ask
*    is all it takes.
*
*    NOT READY (2) with ASC 04 is a device that is spinning up or a card that
*    has just been inserted. It is worth waiting for, and the caller sleeps
*    between attempts. NOT READY with ASC 3A is "no medium" -- an empty card
*    slot -- and no amount of asking produces one, so it is not retryable and
*    an empty reader does not cost five seconds at every boot. */
static int usbmsc_retryable(usbmsc_iface *bus)
{
    if(!bus->sense_valid)
        return 0;

    if(bus->sense_key == SENSE_UNIT_ATTENTION)
        return 1;

    if(bus->sense_key == SENSE_NOT_READY &&
       bus->sense_asc == ASC_NOT_READY_BECOMING)
        return 1;

    return 0;
}

/* The failure, in words, for the shell. Only the keys that a user can act on
*  are spelled out; the rest are honestly reported as a number, because a wrong
*  guess about what sense key 4 means is worse than the number. */
static const char *usbmsc_sense_text(usbmsc_iface *bus)
{
    if(!bus->sense_valid)
        return "the device refused the command and would not say why";

    switch(bus->sense_key)
    {
        case SENSE_NO_SENSE:
            return "the device refused the command but reports no error";
        case SENSE_NOT_READY:
            if(bus->sense_asc == ASC_NO_MEDIUM)
                return "there is no medium in the drive";
            return "the device is not ready";
        case SENSE_MEDIUM_ERROR:
            return "the medium is damaged at that block";
        case SENSE_ILLEGAL_REQUEST:
            return "the device rejected the command as illegal";
        case SENSE_UNIT_ATTENTION:
            return "the medium was changed or the device was reset";
        case SENSE_DATA_PROTECT:
            return "the medium is write protected";
        default:
            break;
    }

    return "the device refused the command";
}

/* --- The SCSI commands ---------------------------------------------------- */

/* TEST UNIT READY, with the waiting.
*
*  Six zero bytes and no data phase: the entire command is "are you there and
*  can you do anything". Returns 1 when the device said yes.
*
*  THE LOOP IS THE POINT. A device is entitled to say "not ready" and mean
*  "not YET": a disk in an enclosure spinning up, a card reader that has just
*  had a card pushed in, or -- most commonly of all -- a device that has just
*  been reset and owes exactly one UNIT ATTENTION before it will talk. Giving
*  up on the first answer means a stick that works perfectly is reported as
*  absent, which is not a subtle failure and is not a rare one. Waiting forever
*  means an empty card slot holds up the boot for as long as the machine is on.
*
*  So: retry only the two sense codes that mean "later" (usbmsc_retryable),
*  sleep only for the one that means "spinning up", and give up immediately on
*  everything else. A device that is genuinely absent costs one command. */
static int usbmsc_test_unit_ready(usbmsc_iface *bus, uint8_t lun)
{
    uint8_t cdb[SCSI_CDB6];
    int     result;
    int     tries;
    int     broken;

    broken = 0;

    for(tries = 0; tries < USBMSC_READY_TRIES; tries++)
    {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = SCSI_TEST_UNIT_READY;

        result = usbmsc_command(bus, lun, cdb, SCSI_CDB6, 0, 0, 0, 0);

        if(result == 0)
            return 1;

        if(result < 0)
        {
            /* A transport error, which is a DIFFERENT budget from the fifty
            *  attempts above and has to be, because the two failures cost
            *  different amounts of time. A device answering "not ready yet"
            *  answers in microseconds and the hundred milliseconds between
            *  attempts is this file's own choice; a device that has stopped
            *  answering costs the controller's full timeout on each of three
            *  transfers plus a reset recovery, so fifty of those would be a
            *  boot that stops for two minutes on one broken stick.
            *
            *  Recovery has already run inside the transport, and a device that
            *  survived it owes exactly one UNIT ATTENTION -- so retrying is
            *  worth doing, and USBMSC_TRIES times is enough to collect it. */
            broken++;
            if(!bus->live || broken >= USBMSC_TRIES)
                return 0;

            continue;
        }

        if(!usbmsc_retryable(bus))
            return 0;

        if(bus->sense_key == SENSE_NOT_READY)
            sleep(USBMSC_READY_WAIT_MS);
    }

    usbmsc_error_text = "USB storage: the unit never became ready";
    return 0;
}

/* INQUIRY: what the thing says it is.
*
*  36 bytes, and only four things in them are used -- the peripheral qualifier
*  and device type in byte 0, and the vendor and product strings in bytes 8..31.
*  The strings are space padded ASCII of fixed width and are NOT terminated,
*  which is why they are copied out by length and trimmed rather than treated
*  as strings.
*
*  Asked FIRST, before TEST UNIT READY, and that order matters for a card
*  reader: INQUIRY is answered by a LUN with nothing in it, and its qualifier
*  is how a device says "this slot is empty" without failing a command. TEST
*  UNIT READY on that LUN would be a failure with a sense code, which reads the
*  same as a broken device.
*
*  Returns 1 when the LUN is present and is a block device this file can drive. */
static int usbmsc_inquiry(usbmsc_iface *bus, usbmsc_unit *unit)
{
    uint8_t cdb[SCSI_CDB6];
    uint8_t data[SCSI_INQUIRY_LEN];
    int     moved;
    int     result;
    int     type;
    int     qualifier;
    int     at;
    int     i;
    int     last;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = SCSI_INQUIRY_LEN;

    memset(data, 0, sizeof(data));

    result = usbmsc_command(bus, unit->lun, cdb, SCSI_CDB6,
                            data, SCSI_INQUIRY_LEN, 1, &moved);

    if(result != 0)
        return 0;

    /* Byte 0 alone decides whether there is anything here, so a reply that did
    *  not even reach byte 8 has told us nothing about the strings and is not
    *  worth reading them out of. */
    if(moved < 8)
        return 0;

    qualifier = (int)((data[0] >> 5) & 0x07);
    type      = (int)(data[0] & 0x1F);

    /* Qualifier 3: the device supports this LUN but there is nothing attached
    *  to it. An empty slot in a four-slot reader. Not a failure. */
    if(qualifier == SCSI_QUALIFIER_NONE)
        return 0;

    if(type != SCSI_TYPE_DIRECT && type != SCSI_TYPE_RBC)
        return 0;

    /* Vendor (8 bytes) then product (16), with a space between and the
    *  trailing padding trimmed. Copied by length: they are fixed width fields
    *  and a device is entitled to fill every byte, so there is no terminator
    *  to find. */
    at   = 0;
    last = moved < 32 ? moved : 32;

    for(i = 8; i < last; i++)
    {
        if(i == 16 && at > 0)
        {
            /* The join between the two fields. One space, and only if the
            *  vendor field held anything. */
            while(at > 0 && unit->model[at - 1] == ' ')
                at--;

            if(at > 0 && at < USBMSC_MODEL - 1)
            {
                unit->model[at] = ' ';
                at++;
            }
        }

        if(data[i] < 0x20 || data[i] > 0x7E)
            continue;

        if(at >= USBMSC_MODEL - 1)
            break;

        unit->model[at] = (char)data[i];
        at++;
    }

    while(at > 0 && unit->model[at - 1] == ' ')
        at--;

    unit->model[at] = '\0';

    if(at == 0)
        usbmsc_put(unit->model, 0, USBMSC_MODEL, "USB storage");

    return 1;
}

/* READ CAPACITY(10): how big it is, and the two ways of getting that wrong.
*
*  Eight bytes come back, big endian, and they are the two numbers this file
*  cares about most:
*
*      bytes 0..3   the LAST LBA
*      bytes 4..7   the block length in bytes
*
*  IT IS THE LAST LBA AND NOT THE COUNT. A 16 MiB stick of 512 byte sectors
*  answers 32766, not 32767. Reporting that number as the size means the
*  filesystem believes there is one more sector than there is, and the sector
*  it eventually reads is off the end of the medium -- the device fails the
*  command, or worse, wraps. It is one addition, it is invisible in every test
*  that does not touch the last sector, and it is why the verification for this
*  driver reads the last sector specifically.
*
*  THE BLOCK LENGTH IS CHECKED AND A DEVICE THAT IS NOT 512 IS REFUSED.
*  blockdev.h says sectors are 512 bytes throughout and that a driver for other
*  media "would have to translate rather than report it". Translating is
*  genuinely easy in one direction -- read the 4096 byte block, hand back the
*  512 byte slice -- and genuinely nasty in the other: a 512 byte write into a
*  4096 byte block is a read-modify-write, which is a second copy of the medium
*  in memory, a torn-write window on every write, and a cache this layer has
*  said twice that it does not have. A half-translation that reads and refuses
*  to write would be a filesystem that mounts and then fails at the first
*  directory update, which is worse than not mounting.
*
*  So: refused, and the reason is kept in the unit's description so that the
*  shell can say "4096 byte sectors" rather than leaving a stick that plainly
*  enumerated with no explanation for why it is not there. Two thirds of the
*  work of supporting it is already done -- the unit exists and is described --
*  and what is missing is written down here rather than half-built.
*
*  Returns 1 when the unit is usable; fills unit->sectors with a COUNT. */
static int usbmsc_read_capacity(usbmsc_iface *bus, usbmsc_unit *unit)
{
    uint8_t  cdb[SCSI_CDB10];
    uint8_t  data[SCSI_CAPACITY_LEN];
    uint32_t last_lba;
    uint32_t block;
    int      moved;
    int      result;
    int      at;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_CAPACITY10;

    memset(data, 0, sizeof(data));

    result = usbmsc_command(bus, unit->lun, cdb, SCSI_CDB10,
                            data, SCSI_CAPACITY_LEN, 1, &moved);

    if(result != 0 || moved < SCSI_CAPACITY_LEN)
    {
        usbmsc_error_text = "USB storage: READ CAPACITY did not answer";
        return 0;
    }

    last_lba = usbmsc_get_be32(&data[0]);
    block    = usbmsc_get_be32(&data[4]);

    if(block != BLK_SECTOR_SIZE)
    {
        at = usbmsc_put(unit->text, 0, USBMSC_TEXT, "refused: ");
        at = usbmsc_put_uint(unit->text, at, USBMSC_TEXT, block);
        usbmsc_put(unit->text, at, USBMSC_TEXT,
                   " byte sectors, and this kernel is 512 throughout");

        usbmsc_error_text = "USB storage: the sector size is not 512";
        return 0;
    }

    /* 0xFFFFFFFF is the defined "the capacity does not fit in 32 bits, ask
    *  READ CAPACITY(16)" answer -- a device larger than 2 TiB. It is also the
    *  one value where the +1 below overflows to zero, which would report a
    *  2 TiB disk as an empty one. Refused on both counts: there is no 64 bit
    *  arithmetic in this kernel by decision, so there is nothing to truncate
    *  it to that would not be a lie. */
    if(last_lba == 0xFFFFFFFFUL)
    {
        usbmsc_put(unit->text, 0, USBMSC_TEXT,
                   "refused: larger than 2 TiB, which needs a 64 bit LBA");
        usbmsc_error_text = "USB storage: the capacity does not fit 32 bits";
        return 0;
    }

    /* The last LBA plus one. The whole reason this function has a comment. */
    unit->sectors = last_lba + 1UL;

    if(unit->sectors == 0)
    {
        usbmsc_put(unit->text, 0, USBMSC_TEXT, "refused: it reports no sectors");
        usbmsc_error_text = "USB storage: the unit reports no sectors";
        return 0;
    }

    return 1;
}

/* --- Reading and writing -------------------------------------------------- */

/* One READ(10) or WRITE(10). Up to USBMSC_MAX_SECTORS blocks; the caller
*  chunks.
*
*  The command block, and every field in it is big endian:
*
*      byte 0     opcode
*      byte 1     flags -- 0, this file asks for no force-unit-access and no
*                 disabled page-out
*      bytes 2..5 the LBA, big endian
*      byte 6     group number, 0
*      bytes 7..8 the block count, big endian
*      byte 9     control, 0
*
*  Returns 0 on success, negative on failure. */
static int usbmsc_rw10(usbmsc_unit *unit, uint32_t lba, uint32_t count,
                       void *buf, int write)
{
    usbmsc_iface *bus;
    uint8_t       cdb[SCSI_CDB10];
    int           length;
    int           moved;
    int           result;
    int           tries;

    bus    = unit->bus;
    length = (int)count * BLK_SECTOR_SIZE;

    for(tries = 0; tries < USBMSC_TRIES; tries++)
    {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = (uint8_t)(write ? SCSI_WRITE10 : SCSI_READ10);
        usbmsc_put_be32(&cdb[2], lba);
        usbmsc_put_be16(&cdb[7], (uint16_t)count);

        moved = 0;

        result = usbmsc_command(bus, unit->lun, cdb, SCSI_CDB10,
                                buf, length, write ? 0 : 1, &moved);

        if(result == 0)
        {
            /* THE RESIDUE IS CHECKED EVEN WHEN THE DEVICE SAID PASS, and this
            *  is the check that separates "the read worked" from "no error was
            *  reported". A device is allowed to process less than was asked
            *  for and still call it a success -- that is what a residue on a
            *  passing command means -- and for a block read it means part of
            *  the buffer never arrived. The tail was zeroed in the transport,
            *  so what a caller that ignored this would get is a sector of
            *  zeros where its data should be: entirely plausible, silently
            *  wrong, and indistinguishable from a genuinely empty sector.
            *
            *  Retried once rather than failed outright, because a short
            *  transfer is exactly the kind of thing that succeeds the second
            *  time; if it happens twice the caller is told. */
            if(moved == length)
                return 0;

            usbmsc_error_text = write
                ? "USB storage: the write moved fewer bytes than asked"
                : "USB storage: the read returned fewer bytes than asked";
            continue;
        }

        if(result < 0)
        {
            /* Transport failure. Recovery has already been attempted inside
            *  the transport; a device that did not survive it is gone and
            *  retrying is a second second spent finding that out again. */
            if(!bus->live)
                return -1;

            continue;
        }

        /* The device rejected the command, and there is a sense code. Retry
        *  only the two codes that mean "later" -- above all UNIT ATTENTION,
        *  which is what a device produces on the command after a reset and
        *  which is cleared by being reported. Everything else is a real no:
        *  a bad block, a write to protected medium, an LBA past the end. */
        if(usbmsc_retryable(bus))
            continue;

        usbmsc_error_text = usbmsc_sense_text(bus);
        return -1;
    }

    if(usbmsc_error_text[0] == '\0')
        usbmsc_error_text = "USB storage: the transfer did not complete";

    return -1;
}

/* --- The block device interface ------------------------------------------ */

/* From a block device number back to the unit. Linear over at most four
*  entries; a table indexed by number would need the ATA range in it too. */
static usbmsc_unit *usbmsc_by_blkdev(int dev)
{
    int i;

    for(i = 0; i < usbmsc_unit_count; i++)
    {
        if(usbmsc_units[i].used && usbmsc_units[i].blk_dev == dev)
            return &usbmsc_units[i];
    }

    return 0;
}

/* Shared by read and write: everything that has to be true before a single
*  byte is worth moving. Returns the unit, or 0 with the reason recorded.
*
*  blockdev.h promises that blk_read() already refuses a range past the end of
*  the medium, and this checks it again. Not out of distrust: the check here is
*  against THIS unit's sector count, which is the number that came off the
*  wire, and it is the one that stops an LBA the device would reject -- or, on
*  a device that does not range check, would answer with something. The
*  overflow test is the one blockdev.h's version cannot make on its behalf:
*  lba + count wrapping past 2^32 turns a request past the end into a request
*  at the beginning, which passes every "is the end within the medium" test
*  ever written. */
static usbmsc_unit *usbmsc_check(int dev, uint32_t lba, uint32_t count,
                                 const void *buf)
{
    usbmsc_unit *unit;

    unit = usbmsc_by_blkdev(dev);

    if(unit == 0)
    {
        usbmsc_error_text = "USB storage: no such device";
        return 0;
    }

    if(unit->bus == 0 || !unit->bus->live || unit->sectors == 0)
    {
        usbmsc_error_text = "USB storage: the device is no longer there";
        return 0;
    }

    if(buf == 0)
    {
        usbmsc_error_text = "USB storage: no buffer";
        return 0;
    }

    if(count == 0)
    {
        usbmsc_error_text = "USB storage: a transfer of no sectors";
        return 0;
    }

    if(lba + count < lba || lba + count > unit->sectors)
    {
        usbmsc_error_text = "USB storage: the range runs past the end";
        return 0;
    }

    return unit;
}

static int usbmsc_blk_read(int dev, uint32_t lba, uint32_t count, void *buf)
{
    usbmsc_unit *unit;
    uint32_t     chunk;

    unit = usbmsc_check(dev, lba, count, buf);
    if(unit == 0)
        return -1;

    while(count > 0)
    {
        chunk = count;
        if(chunk > USBMSC_MAX_SECTORS)
            chunk = USBMSC_MAX_SECTORS;

        if(usbmsc_rw10(unit, lba, chunk, buf, 0) < 0)
            return -1;

        lba   += chunk;
        count -= chunk;
        buf    = (void *)((uint8_t *)buf + chunk * BLK_SECTOR_SIZE);
    }

    return 0;
}

/* The const is dropped on the way into the transport, and that is the one
*  place in this file where it happens. usb_bulk() takes a void * because it
*  serves both directions with one function; the OUT path only ever reads from
*  the buffer, and uhci.c copies it into a bounce buffer before the controller
*  sees it. Nothing writes through this pointer. */
static int usbmsc_blk_write(int dev, uint32_t lba, uint32_t count,
                            const void *buf)
{
    usbmsc_unit *unit;
    uint32_t     chunk;
    uint8_t     *from;

    unit = usbmsc_check(dev, lba, count, buf);
    if(unit == 0)
        return -1;

    from = (uint8_t *)buf;

    while(count > 0)
    {
        chunk = count;
        if(chunk > USBMSC_MAX_SECTORS)
            chunk = USBMSC_MAX_SECTORS;

        if(usbmsc_rw10(unit, lba, chunk, from, 1) < 0)
            return -1;

        lba   += chunk;
        count -= chunk;
        from  += chunk * BLK_SECTOR_SIZE;
    }

    return 0;
}

/* blockdev.h makes this the presence test as well as the size, deliberately:
*  a device with no size is not a device anyone can use. So a unit whose
*  interface has been marked lost answers 0 here without any further flag, and
*  everything above stops asking. */
static uint32_t usbmsc_blk_sectors(int dev)
{
    usbmsc_unit *unit;

    unit = usbmsc_by_blkdev(dev);

    if(unit == 0 || unit->bus == 0 || !unit->bus->live)
        return 0;

    return unit->sectors;
}

static const char *usbmsc_blk_describe(int dev)
{
    usbmsc_unit *unit;

    unit = usbmsc_by_blkdev(dev);

    if(unit == 0)
        return "no such device";

    if(unit->bus == 0 || !unit->bus->live)
        return "unplugged";

    return unit->model;
}

static const blk_ops usbmsc_blk_ops =
{
    usbmsc_bus_name,
    usbmsc_blk_read,
    usbmsc_blk_write,
    usbmsc_blk_sectors,
    usbmsc_blk_describe
};

/* --- Claiming ------------------------------------------------------------- */

/* Finds the bulk IN and bulk OUT endpoints. A bulk-only device has exactly one
*  of each and cannot work without both, so a device missing either is not
*  claimed rather than half-claimed. Returns 1 when both were found. */
static int usbmsc_find_endpoints(usbmsc_iface *bus)
{
    const usb_device *dev;
    int               i;

    dev = bus->dev;

    bus->ep_in  = -1;
    bus->ep_out = -1;

    for(i = 0; i < dev->endpoints; i++)
    {
        if(dev->endpoint[i].type != USB_XFER_BULK)
            continue;

        if((dev->endpoint[i].direction & USB_DIR_IN) != 0)
        {
            if(bus->ep_in < 0)
                bus->ep_in = (int)dev->endpoint[i].address;
        }
        else
        {
            if(bus->ep_out < 0)
                bus->ep_out = (int)dev->endpoint[i].address;
        }
    }

    return (bus->ep_in >= 0 && bus->ep_out >= 0);
}

/* GET_MAX_LUN, and why a stall is the answer rather than a failure.
*
*  The request asks how many logical units are behind the one interface: a
*  plain stick has one and answers 0, a four-slot card reader answers 3. It is
*  a class request with a one byte data stage.
*
*  A DEVICE THAT STALLS IT HAS ONE LUN. The specification says so in as many
*  words -- a device that does not support multiple LUNs may stall the request
*  -- and a great many devices do exactly that. Treating the stall as a failure
*  refuses a perfectly ordinary stick; treating the stalled transfer's
*  untouched buffer as the answer is worse, because the buffer holds whatever
*  was on the stack and a driver would then probe 200 LUNs.
*
*  So the byte is only believed when the transfer actually returned one, and
*  the value is capped: the specification's own maximum is 15, and this file's
*  is lower still because the block device table has four slots in it. */
static uint8_t usbmsc_get_max_lun(usbmsc_iface *bus)
{
    uint8_t value;
    int     result;

    value = 0;

    result = usb_control(bus->dev, MSC_IN_FROM_IFACE, MSC_REQ_GET_MAX_LUN,
                         0, (uint16_t)bus->dev->iface_number, &value, 1);

    if(result != 1)
        return 0;

    if(value > USBMSC_MAX_LUN)
        value = USBMSC_MAX_LUN;

    return value;
}

/* Builds the line usbmsc_describe() hands back for a unit that came up. A unit
*  that was refused already has its own text explaining why, written where the
*  refusal happened, and this does not overwrite it. */
static void usbmsc_build_text(usbmsc_unit *unit)
{
    uint32_t mib;
    int      at;

    /* Size in whole MiB, computed in SECTORS. A capacity in bytes overflows 32
    *  bits at 4 GiB, and this kernel has no 64 bit arithmetic, so anything
    *  that multiplies sectors by 512 first is a driver that reports a 6 GB
    *  stick as 1.7 GB. 2048 sectors is one MiB, and dividing first cannot
    *  overflow anything. */
    mib = unit->sectors / (1024UL * 1024UL / BLK_SECTOR_SIZE);

    at = usbmsc_put(unit->text, 0, USBMSC_TEXT, unit->model);
    at = usbmsc_put(unit->text, at, USBMSC_TEXT, ", ");
    at = usbmsc_put_uint(unit->text, at, USBMSC_TEXT, mib);
    at = usbmsc_put(unit->text, at, USBMSC_TEXT, " MiB (");
    at = usbmsc_put_uint(unit->text, at, USBMSC_TEXT, unit->sectors);
    at = usbmsc_put(unit->text, at, USBMSC_TEXT, " sectors)");

    if(unit->bus->max_lun != 0)
    {
        at = usbmsc_put(unit->text, at, USBMSC_TEXT, ", LUN ");
        at = usbmsc_put_uint(unit->text, at, USBMSC_TEXT,
                             (uint32_t)unit->lun);
    }

    if(unit->blk_dev >= 0)
    {
        at = usbmsc_put(unit->text, at, USBMSC_TEXT, ", device ");
        usbmsc_put_uint(unit->text, at, USBMSC_TEXT, (uint32_t)unit->blk_dev);
    }
    else
    {
        usbmsc_put(unit->text, at, USBMSC_TEXT, ", no block device number free");
    }
}

/* Brings one logical unit up and registers it. Returns 1 when a unit was added
*  to the table, whether or not it became a block device -- a refused unit is
*  still worth a line saying why. Returns 0 when there is nothing there at all,
*  which is the ordinary answer for an empty slot of a card reader. */
static int usbmsc_claim_lun(usbmsc_iface *bus, uint8_t lun)
{
    usbmsc_unit *unit;
    int          number;

    if(usbmsc_unit_count >= USBMSC_MAX_UNITS)
        return 0;

    unit = &usbmsc_units[usbmsc_unit_count];
    memset(unit, 0, sizeof(usbmsc_unit));

    unit->bus     = bus;
    unit->lun     = lun;
    unit->blk_dev = -1;

    /* INQUIRY first: it is the one command an empty or unsupported LUN answers
    *  cleanly, so it is what separates "nothing here" from "something broken". */
    if(!usbmsc_inquiry(bus, unit))
        return 0;

    unit->used = 1;
    usbmsc_unit_count++;

    if(!usbmsc_test_unit_ready(bus, lun))
    {
        usbmsc_put(unit->text, 0, USBMSC_TEXT, unit->model);
        usbmsc_put(unit->text, (int)strlen(unit->model), USBMSC_TEXT,
                   ", not ready");
        return 1;
    }

    if(!usbmsc_read_capacity(bus, unit))
    {
        /* usbmsc_read_capacity() wrote the reason into unit->text itself when
        *  it refused for a reason worth naming. When it did not -- the command
        *  simply failed -- there is still a unit and it still deserves a line. */
        if(unit->text[0] == '\0')
            usbmsc_put(unit->text, 0, USBMSC_TEXT,
                       "the capacity could not be read");
        return 1;
    }

    /* Numbers above the ATA drives, first free one. blk_register() refuses a
    *  number that is taken, which is what makes "first free" a loop rather
    *  than a count: the ATA driver may have claimed fewer than four, and a
    *  second stick registers after the first without either of them knowing
    *  the other exists. */
    for(number = BLK_REMOVABLE_FIRST; number < BLK_MAX_DEVICES; number++)
    {
        if(blk_register(number, &usbmsc_blk_ops) == 0)
        {
            unit->blk_dev = number;
            break;
        }
    }

    usbmsc_build_text(unit);
    return 1;
}

/* Takes one enumerated device, if it is a bulk-only SCSI mass storage device.
*  Returns 1 when the interface was claimed.
*
*  Everything that can go wrong here is an ordinary outcome -- a device of
*  another class, one another driver already took, a mass storage device
*  speaking CBI or ATAPI -- and none of it is worth a line on the console at
*  boot. */
static int usbmsc_claim(usb_device *dev)
{
    usbmsc_iface *bus;
    uint8_t       lun;

    if(usbmsc_iface_count >= USBMSC_MAX_IFACES)
        return 0;

    if(dev->iface_class != USB_CLASS_MASS_STORAGE)
        return 0;

    if(dev->iface_subclass != MSC_SUBCLASS_SCSI)
        return 0;

    if(dev->iface_protocol != MSC_PROTOCOL_BULK_ONLY)
        return 0;

    /* usb.c sets this to "none". The check is what keeps two drivers from
    *  binding one interface. */
    if(dev->driver != 0 && strcmp(dev->driver, "none") != 0)
        return 0;

    bus = &usbmsc_ifaces[usbmsc_iface_count];
    memset(bus, 0, sizeof(usbmsc_iface));

    bus->dev = dev;

    if(!usbmsc_find_endpoints(bus))
        return 0;

    bus->used = 1;
    bus->live = 1;

    /* A recognisable starting tag rather than 0 or 1. Two things fall out of
    *  it: a wrapper seen in a bus trace is obviously this driver's, and a
    *  buffer that was never filled -- all zeros, or the 0xFF of an unwritten
    *  read -- cannot accidentally match a tag this file issued. */
    bus->tag = 0x544F4D41UL;   /* "TOMA" */

    dev->driver = usbmsc_name;

    usbmsc_iface_count++;

    /* A device that has just been powered up and enumerated owes a UNIT
    *  ATTENTION to the first command on every LUN. usbmsc_test_unit_ready()
    *  absorbs it -- that is what makes the retry loop there load bearing on a
    *  device that is otherwise perfectly healthy. */
    bus->max_lun = usbmsc_get_max_lun(bus);

    for(lun = 0; lun <= bus->max_lun; lun++)
    {
        if(!usbmsc_claim_lun(bus, lun))
        {
            /* An empty LUN is not the end of the list -- a reader with a card
            *  in slot 2 and nothing in slots 0 and 1 is ordinary -- so the
            *  loop continues rather than stopping at the first gap. */
            continue;
        }
    }

    return 1;
}

/* --- Bringing it up ------------------------------------------------------- */

/* Looks over what usb.c enumerated and claims every bulk-only SCSI device.
*
*  Call it once, after usb_init() and after blk_init(). Safe on a machine with
*  no USB controller and on one with a controller and nothing plugged in: both
*  leave the counts at zero and every accessor below then answers the way it
*  would for a machine that has none.
*
*  NO TASK IS CREATED, and that is the difference from usbhid.c. A HID driver
*  has to poll, because a keyboard only speaks when it is asked; a block device
*  speaks only when it is spoken to. Every transfer this file makes is inside
*  somebody else's blk_read() or blk_write(), on that caller's task, which is
*  exactly what blockdev.h says will happen. So none of usbhid.c's careful
*  business about starting a task from the boot path applies here -- there is
*  nothing to start.
*
*  It does, however, TALK TO THE DEVICE while running, which usbhid_init() does
*  too: INQUIRY, TEST UNIT READY and READ CAPACITY per LUN. On the boot path
*  that is a handful of milliseconds for a healthy stick, and up to five
*  seconds for one that says it is still spinning up -- bounded, and the bound
*  is USBMSC_READY_TRIES.
*
*  Nothing is printed, for the same reason usbhid.c prints nothing: whether a
*  stick was found is a question for the shell, which has usbmsc_count() and
*  usbmsc_describe() to answer it. */
void usbmsc_init(void)
{
    const usb_device *dev;
    int               count;
    int               i;

    if(usbmsc_started)
        return;

    usbmsc_started = 1;

    if(!usb_present())
        return;

    count = usb_device_count();

    for(i = 0; i < count; i++)
    {
        dev = usb_device_get(i);
        if(dev == 0)
            continue;

        /* The const is dropped deliberately, exactly as usbhid.c does and for
        *  the same reason: usb.h hands out a read-only view so that nothing
        *  browsing the table can damage it, but the objects behind it are
        *  ordinary mutable statics in usb.c, and a class driver is precisely
        *  the caller entitled to write to one -- usb_control() takes a
        *  non-const device, and claiming an interface means writing the driver
        *  name into it. There is no usb_claim() to do it properly, and adding
        *  one would mean editing usb.c, which this file does not. */
        usbmsc_claim((usb_device *)dev);
    }
}

/* --- What the shell can ask ----------------------------------------------- */

/* Whether anything is usable right now. A unit that was found and refused, or
*  one whose device has been unplugged, does not count -- the question is "is
*  there a USB disk working", not "was there one at boot". */
int usbmsc_present(void)
{
    int i;

    for(i = 0; i < usbmsc_unit_count; i++)
    {
        if(usbmsc_units[i].used &&
           usbmsc_units[i].blk_dev >= 0 &&
           usbmsc_units[i].bus != 0 &&
           usbmsc_units[i].bus->live)
        {
            return 1;
        }
    }

    return 0;
}

/* How many units were found, refused and unplugged ones included, because that
*  is the number usbmsc_describe() indexes and both are worth a line. */
int usbmsc_count(void)
{
    return usbmsc_unit_count;
}

/* One line about one unit, for example
*
*      QEMU QEMU HARDDISK, 16 MiB (32768 sectors), device 4
*      Generic Flash Reader, not ready
*      refused: 2048 byte sectors, and this kernel is 512 throughout
*      SanDisk Cruzer, 7640 MiB (15646720 sectors), device 5 (unplugged)
*
*  Never null, so a caller can print it without checking; an index that names
*  nothing reads as such rather than as an empty line. */
const char *usbmsc_describe(int index)
{
    static char text[USBMSC_TEXT + 16];
    int         at;

    if(index < 0 || index >= usbmsc_unit_count)
        return "no such unit";

    at = usbmsc_put(text, 0, (int)sizeof(text), usbmsc_units[index].text);

    if(usbmsc_units[index].bus == 0 || !usbmsc_units[index].bus->live)
        usbmsc_put(text, at, (int)sizeof(text), " (unplugged)");

    return text;
}

int usbmsc_blkdev(int index)
{
    if(index < 0 || index >= usbmsc_unit_count)
        return -1;

    return usbmsc_units[index].blk_dev;
}

/* Counters. commands is every SCSI command attempted, including the automatic
*  REQUEST SENSE fetches; failures is the ones the device rejected, which is a
*  real answer and not necessarily a problem -- a card reader with three empty
*  slots produces several at every boot; errors is the ones the transport lost,
*  which is the number that matters when a stick stops working. stalls and
*  resets are the two recovery paths, and a resets count above zero on a
*  healthy machine is worth looking into: it means the two ends of the
*  transport disagreed about which command they were in. */
uint32_t usbmsc_commands(void)
{
    return usbmsc_stat_commands;
}

uint32_t usbmsc_failures(void)
{
    return usbmsc_stat_failures;
}

uint32_t usbmsc_errors(void)
{
    return usbmsc_stat_errors;
}

uint32_t usbmsc_stalls(void)
{
    return usbmsc_stat_stalls;
}

uint32_t usbmsc_resets(void)
{
    return usbmsc_stat_resets;
}

const char *usbmsc_last_error(void)
{
    return usbmsc_error_text;
}
