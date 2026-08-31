/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: USB core -- enumeration, descriptors and the transfer wrappers.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*
*  usb.h says why this file is separate from the controller: two thirds of a
*  USB stack does not depend on the silicon, and the day this kernel grows an
*  xHCI driver, everything here is meant to keep working unchanged. What that
*  description leaves out is what makes THIS file hard, and it is not the
*  layering. It is that enumeration is a sequence, that every step of it can go
*  wrong quietly, and that after the very first transfer every byte it works
*  from was chosen by a device that may be broken, may be counterfeit, and in
*  any case was never tested against this kernel.
*
*  1. THE ORDER IS NOT FREE.
*
*  A device that has just been reset answers at address 0, and it keeps
*  answering there until SET_ADDRESS moves it. UHCI's two root ports sit on one
*  electrical bus, so two devices in that state answer the same transfer at the
*  same time and the result is garbage that looks like a hardware fault. That is
*  the whole reason usb_init() resets and enumerates ONE port at a time and
*  finishes with a device before it touches the next port -- not tidiness, but
*  the only way the bus stays unambiguous.
*
*  The same fact drives what happens when enumeration fails half way (point 5).
*
*  2. THE FIRST TRANSFER IS A CHICKEN AND EGG PROBLEM.
*
*  A control transfer has to be split into packets of the endpoint's maximum
*  size, so the controller needs bMaxPacketSize0 before it can move a single
*  byte. bMaxPacketSize0 is byte 7 OF THE DEVICE DESCRIPTOR, which can only be
*  read by a control transfer. See usb_read_device_desc() for how that is
*  broken; it is the standard answer and it is the step most likely to be got
*  subtly wrong, because getting it wrong still works on every device whose
*  packet size happens to be 8.
*
*  3. total_length COMES OFF THE WIRE.
*
*  The configuration descriptor is a header followed by the interface and
*  endpoint descriptors belonging to it, and the header carries the length of
*  the whole block. So it takes two reads: the 9 byte header, then the block.
*  Nothing checks that the number in the header is true. A device may claim
*  4 KB and send 34 bytes, or claim 34 and send 4 KB, and both happen -- the
*  first from broken firmware, the second from firmware that is lying. The
*  walk is therefore bounded by HOW MANY BYTES ACTUALLY ARRIVED and never by
*  what the device said, and the read itself is bounded by the buffer.
*
*  4. A DESCRIPTOR LENGTH OF ZERO IS AN ENDLESS LOOP.
*
*  Walking the block means stepping from one descriptor to the next by each
*  one's own bLength byte. A bLength of 0 never advances; a bLength of 200 in
*  a 34 byte block reads off the end. usb_parse_config() is written so that
*  termination is a property of the arithmetic and not of the data: the cursor
*  provably grows by at least two every turn, and every field is checked to lie
*  inside what arrived before it is read. That is the same rule dns.c's name
*  decoder is written to and for the same reason.
*
*  5. A DEVICE THAT FAILS HALF WAY MUST STILL LEAVE THE BUS USABLE.
*
*  The tempting handling -- note the failure and carry on to the next port --
*  is exactly what makes the next device fail too, because the failed one is
*  still sitting at address 0 shouting over it. usb_enum_port() therefore
*  always tries to move a failed device OFF address 0 even though it will never
*  be used, and addresses are handed out from a counter that never reuses a
*  value within a boot, so a failure can never leave two devices sharing one
*  address later on. See usb_enum_port().
*
*  Half filled table entries are structurally impossible rather than carefully
*  avoided: enumeration builds the device in a local, and the table is written
*  once, at the end, only when everything succeeded.
*
*  6. WHAT IS NOT HERE.
*
*  No hot plug: ports are looked at once, at boot, so the table only ever
*  grows and usb_device_get() can index it directly. No hubs, so one device
*  per root port. No isochronous endpoints -- they are skipped on purpose in
*  the walk rather than recorded and left unusable, because there are only
*  USB_MAX_ENDPOINTS slots and an audio device would otherwise fill them all
*  with endpoints nothing can drive. No string descriptors: they cost two more
*  transfers per device to produce a name nothing needs yet.
*
*  Printing: enumeration happens once, at boot, and what was found is worth a
*  line. Nothing here prints per transfer -- a mouse produces 125 interrupt
*  transfers a second and a printf in that path would be the only thing the
*  machine does.
*/

#include <system.h>
#include <stdio.h>
#include <usb.h>
#include <uhci.h>

/* --- Limits and delays --------------------------------------------------- */

/* The largest address SET_ADDRESS may assign. 0 means "unaddressed", so the
*  usable range is 1..127. */
#define USB_ADDRESS_MAX 127

/* The configuration block is read into this. 512 bytes is far more than the
*  34 a mouse and the 59 a composite keyboard produce, and a block bigger than
*  this is truncated rather than refused: what arrived is still walkable, and
*  the interface a driver wants is at the front. A device that needs more than
*  512 bytes to describe itself has more interfaces than this kernel can use
*  anyway. */
#define USB_CONFIG_BUF 512

/* TRSTRCY, the reset recovery time: a device may ignore everything for 10 ms
*  after its port is reset. The controller's port_reset() may well wait this
*  out itself, but it is not required to by usb_hc_ops and 10 ms once per port
*  at boot is not a cost worth arguing about. */
#define USB_RESET_RECOVERY_MS 10

/* The settling delay after SET_ADDRESS. The device answers the status stage
*  from its OLD address and only then switches, so a transfer sent to the new
*  address too early is answered by nobody.
*
*  The specification (USB 2.0, 9.2.6.3) calls this TSETADDR and requires the
*  device to accept a setup packet at the new address within 2 ms. 2 ms is what
*  a device must manage, not what devices actually manage: cheap hubs and
*  keyboards are known to need more, which is why Linux waits 10 ms here as
*  well. This kernel's timer runs at 1 kHz, so 2 ms is only two ticks and a
*  single missed tick would eat most of the margin. 10 ms costs 10 ms once per
*  device at boot and removes the whole class of "enumerates on my machine but
*  not on yours" failures, which is the worst kind to debug because it looks
*  like a driver bug and is a timing bug. */
#define USB_SET_ADDRESS_SETTLE_MS 10

/* --- State --------------------------------------------------------------- */

/* The one controller. usb.h promises nothing above this line learns which one
*  it is, so nothing here ever looks at anything but the function pointers. */
static const usb_hc_ops *usb_hc = 0;

/* The device table. Kept compacted: entries 0..usb_devices-1 are used and
*  nothing is ever removed, because there is no hot unplug path. */
static usb_device usb_dev[USB_MAX_DEVICES];
static int        usb_devices = 0;

/* Addresses are handed out from here and NEVER REUSED within a boot.
*
*  The obvious scheme -- address = table index + 1 -- is wrong in exactly the
*  case that matters. A device that fails half way through enumeration keeps
*  whatever address it was given, because taking it back would put it at
*  address 0 again where it would shout over the next port (see point 5 in the
*  header comment). Its table slot, however, is never claimed. With index based
*  addresses the next device gets that slot, that address, and a silent
*  collision with a device still answering to it. A counter that only ever goes
*  up cannot produce that, and 127 addresses against USB_MAX_DEVICES slots
*  means it cannot run out in practice either. */
static uint8_t usb_next_address = 1;

/* The configuration block buffer. Static rather than on the stack because 512
*  bytes is more than a kernel task's stack wants to give up, and rather than
*  from the heap because enumeration is strictly one device at a time and there
*  is nothing to share it with. */
static uint8_t usb_config_buf[USB_CONFIG_BUF];

static uint32_t usb_xfer_count = 0;
static uint32_t usb_err_count  = 0;

/* Never null, so callers can print it without checking. */
static const char *usb_error_text = "";

/* --- Small helpers ------------------------------------------------------- */

/* printf's %X drops leading zeros, and a USB id reads wrong without them:
*  QEMU's keyboard is 0627:0001 and "627:1" does not look like the same thing
*  when it is compared against what QEMU says it plugged in. Four digits,
*  always. */
static void usb_hex16(char *out, uint16_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    int i;

    for(i = 0; i < 4; i++)
        out[i] = digits[(value >> ((3 - i) * 4)) & 0x0F];

    out[4] = '\0';
}

static const char *usb_class_name(uint8_t iface_class)
{
    switch(iface_class)
    {
        case USB_CLASS_HID:          return "HID";
        case USB_CLASS_MASS_STORAGE: return "mass storage";
        case USB_CLASS_HUB:          return "hub";
        default:                     break;
    }

    return "unknown class";
}

/* The only two speeds this stack knows. A controller is supposed to report one
*  of them, but the value is written through a pointer it was handed and there
*  is no way to tell a controller that did not touch it from one that did, so
*  anything else becomes full speed -- the safe assumption, since it only makes
*  the control endpoint packet size negotiation do more work rather than less.
*
*  Applied at BOTH resets. The second one (see usb_read_device_desc) reports
*  the speed again and it went straight into the device the first time this was
*  written, which put an unchecked value into usb_device.speed even though the
*  first reset had validated one. */
static int usb_speed_or_full(int speed)
{
    if(speed != USB_SPEED_LOW && speed != USB_SPEED_FULL)
        return USB_SPEED_FULL;

    return speed;
}

/* Whether this kernel has, or will plausibly have, a driver for the class.
*  Used only to choose between the interfaces of a composite device; see
*  usb_parse_config(). */
static int usb_class_is_known(uint8_t iface_class)
{
    return iface_class == USB_CLASS_HID || iface_class == USB_CLASS_MASS_STORAGE;
}

/* --- Registration -------------------------------------------------------- */

/* First controller wins. A second one is refused rather than replacing the
*  first, because devices already enumerated hold no pointer back to the
*  controller that found them -- swapping it would leave them addressed on a
*  bus nothing talks to any more. */
int usb_register_hc(const usb_hc_ops *ops)
{
    if(!ops || !ops->name || !ops->port_count || !ops->port_reset || !ops->control)
        return USB_EINVAL;

    if(usb_hc)
        return USB_EINVAL;

    usb_hc = ops;
    return 0;
}

int usb_present(void)
{
    return usb_hc != 0;
}

const char *usb_hc_name(void)
{
    return usb_hc ? usb_hc->name : "none";
}

int usb_device_count(void)
{
    return usb_devices;
}

const usb_device *usb_device_get(int index)
{
    if(index < 0 || index >= usb_devices)
        return 0;

    return &usb_dev[index];
}

uint32_t usb_transfers(void)
{
    return usb_xfer_count;
}

uint32_t usb_errors(void)
{
    return usb_err_count;
}

const char *usb_last_error(void)
{
    return usb_error_text;
}

/* --- The transfer wrappers ----------------------------------------------- */

/* All three do the same three things: check what the controller is entitled to
*  assume, count the transfer, and hand the controller's answer back UNCHANGED.
*
*  Unchanged matters most for USB_ESTALL. A stall is how a device says "I do
*  not support that request", it is a legitimate answer to a legitimate
*  question, and enumeration relies on being able to ask questions that may be
*  refused. Translating it into a generic failure here would take that away
*  from every caller, so it is passed through and, deliberately, is not counted
*  as an error. */

int usb_control(usb_device *dev, uint8_t request_type, uint8_t request,
                uint16_t value, uint16_t index,
                void *data, uint16_t length)
{
    usb_setup setup;
    int       result;

    if(!usb_hc || !dev)
        return USB_EINVAL;

    /* A length with nowhere to put the bytes is a caller bug, and one the
    *  controller would answer by writing through a null pointer. */
    if(length != 0 && !data)
        return USB_EINVAL;

    setup.request_type = request_type;
    setup.request      = request;
    setup.value        = value;
    setup.index        = index;
    setup.length       = length;

    usb_xfer_count++;
    result = usb_hc->control(dev, &setup, data, (int)length);

    if(result < 0 && result != USB_ESTALL)
        usb_err_count++;

    return result;
}

int usb_interrupt_in(usb_device *dev, int endpoint, void *buf, int length)
{
    int result;

    if(!usb_hc || !usb_hc->interrupt_in || !dev || !buf)
        return USB_EINVAL;

    if(length <= 0 || endpoint < 0 || endpoint > 15)
        return USB_EINVAL;

    usb_xfer_count++;
    result = usb_hc->interrupt_in(dev, endpoint, buf, length);

    /* 0 is a NAK: the device had nothing to say. For an idle keyboard that is
    *  the answer to nearly every poll, so it is emphatically not an error. */
    if(result < 0 && result != USB_ESTALL)
        usb_err_count++;

    return result;
}

int usb_bulk(usb_device *dev, int endpoint, void *buf, int length, int in)
{
    int result;

    if(!usb_hc || !usb_hc->bulk || !dev || !buf)
        return USB_EINVAL;

    if(length <= 0 || endpoint < 0 || endpoint > 15)
        return USB_EINVAL;

    usb_xfer_count++;
    result = usb_hc->bulk(dev, endpoint, buf, length, in);

    if(result < 0 && result != USB_ESTALL)
        usb_err_count++;

    return result;
}

/* --- Standard requests --------------------------------------------------- */

/* wValue of a GET_DESCRIPTOR is the type in the high byte and the index in the
*  low one. Spelled out here once rather than at each of the three call sites,
*  because getting it the wrong way round returns a plausible looking error
*  instead of an obvious one. */
static int usb_get_descriptor(usb_device *dev, uint8_t type, uint8_t index,
                              void *buf, uint16_t length)
{
    return usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                       USB_REQ_GET_DESCRIPTOR,
                       (uint16_t)(((uint16_t)type << 8) | index), 0,
                       buf, length);
}

/* Moves the device from whatever address it currently answers on to a fresh
*  one. The request itself goes to the OLD address -- dev->address is not
*  updated until the device has acknowledged, because the controller builds the
*  transfer from it.
*
*  Returns 0, or a negative error with dev->address left alone. */
static int usb_assign_address(usb_device *dev)
{
    uint8_t address;
    int     result;

    if(usb_next_address > USB_ADDRESS_MAX)
        return USB_ENOMEM;

    address = usb_next_address;
    usb_next_address++;

    result = usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_SET_ADDRESS, address, 0, 0, 0);
    if(result < 0)
        return result;

    /* The device answered the status stage from the old address and switches
    *  only afterwards. Anything sent to the new address before it has settled
    *  is answered by nobody at all. */
    sleep(USB_SET_ADDRESS_SETTLE_MS);

    dev->address = address;
    return 0;
}

/* --- The device descriptor, and the chicken and egg ---------------------- */

/* Reads the 18 byte device descriptor, working out the endpoint 0 packet size
*  on the way.
*
*  HOW THE CIRCLE IS BROKEN. A control transfer must be split into packets of
*  bMaxPacketSize0 bytes, so the controller needs that number; the number lives
*  in the descriptor the transfer is meant to fetch. The way out is a property
*  of the specification rather than a trick: the smallest legal
*  bMaxPacketSize0 is 8, so EVERY device can move a first data packet of 8
*  bytes, whatever its real size is. And bMaxPacketSize0 sits at offset 7 --
*  inside those first 8 bytes.
*
*  So: ask for 8 bytes with the packet size set to 8. The data stage is then
*  exactly one packet long and no device can disagree about where it ends.
*  Byte 7 of the answer is the real size; from then on the full 18 byte read
*  uses it.
*
*  Doing it the lazy way instead -- assuming 8, or asking for all 18 straight
*  away -- appears to work perfectly, on every device whose packet size really
*  is 8. Low speed devices, which is every keyboard and mouse, are required to
*  use 8. So the bug is invisible on exactly the hardware one tests with and
*  shows up on the full speed device someone else owns.
*
*  Between the short read and SET_ADDRESS the port is reset a second time. That
*  is not in the specification; it is what Windows does, and therefore what
*  devices are tested against, and a fair number of them need it to come out of
*  the short read in a state where they will accept an address. Its failure is
*  ignored on purpose: the device answered a control transfer a moment ago so
*  it is certainly there, and whether the second reset happened or not it is in
*  the same Default state at address 0 either way. */
static int usb_read_device_desc(usb_device *dev, usb_device_desc *out)
{
    uint8_t first[8];
    uint8_t packet0;
    int     speed;
    int     result;

    dev->max_packet0 = 8;

    result = usb_get_descriptor(dev, USB_DESC_DEVICE, 0, first, 8);
    if(result < 8)
        return result < 0 ? result : USB_EIO;

    /* Byte 0 is bLength and byte 1 bDescriptorType. A device that answers a
    *  request for the device descriptor with something else is broken in a way
    *  that makes everything after this meaningless. */
    if(first[0] < (uint8_t)sizeof(usb_device_desc) || first[1] != USB_DESC_DEVICE)
        return USB_EIO;

    packet0 = first[7];

    /* Only 8, 16, 32 and 64 are legal, and low speed may only use 8. A bad
    *  value is corrected to 8 rather than refused: 8 is legal for every device
    *  that exists, so the transfer still works, whereas passing a 0 down to
    *  the controller would have it divide by it. */
    if(packet0 != 8 && packet0 != 16 && packet0 != 32 && packet0 != 64)
        packet0 = 8;
    if(dev->speed == USB_SPEED_LOW)
        packet0 = 8;

    dev->max_packet0 = packet0;

    /* The Windows compatible second reset. Result deliberately unused. */
    speed = dev->speed;
    if(usb_hc->port_reset(dev->port, &speed) > 0)
    {
        dev->speed = usb_speed_or_full(speed);
        sleep(USB_RESET_RECOVERY_MS);
    }

    result = usb_assign_address(dev);
    if(result < 0)
        return result;

    /* Now at its own address and with the real packet size, so the whole thing
    *  can be read. */
    result = usb_get_descriptor(dev, USB_DESC_DEVICE, 0, out,
                               (uint16_t)sizeof(usb_device_desc));
    if(result < (int)sizeof(usb_device_desc))
        return result < 0 ? result : USB_EIO;

    if(out->type != USB_DESC_DEVICE)
        return USB_EIO;

    return 0;
}

/* --- Walking the configuration block ------------------------------------- */

/* Records one endpoint descriptor in the device, if there is room and if it is
*  something this kernel can drive. Returns 1 when it was taken. */
static int usb_take_endpoint(usb_device *dev, const uint8_t *raw)
{
    usb_endpoint_desc  desc;
    usb_endpoint      *ep;
    uint8_t            type;
    uint8_t            number;

    memcpy(&desc, raw, sizeof(desc));

    number = (uint8_t)(desc.address & 0x0F);
    type   = (uint8_t)(desc.attributes & 0x03);

    /* Endpoint 0 is the control endpoint every device has; it is never listed
    *  in a configuration and a device that lists it is describing something
    *  that cannot be addressed as anything else. */
    if(number == 0)
        return 0;

    /* Isochronous is deliberately absent from this stack -- it needs bandwidth
    *  reservation that no controller here implements. Recording one would only
    *  spend one of four slots on an endpoint nothing can use. */
    if(type == USB_XFER_ISOC)
        return 0;

    /* A maximum packet size of 0 describes an endpoint that can never move a
    *  byte, and would be a division by zero in a controller splitting a
    *  transfer. 1024 is the largest any USB 2.0 endpoint may declare. */
    if(desc.max_packet == 0 || desc.max_packet > 1024)
        return 0;

    if(dev->endpoints >= USB_MAX_ENDPOINTS)
        return 0;

    ep = &dev->endpoint[dev->endpoints];
    ep->address    = number;
    ep->direction  = (uint8_t)(desc.address & USB_DIR_IN);
    ep->type       = type;
    ep->interval   = desc.interval;
    ep->max_packet = desc.max_packet;

    /* Both ends start a fresh configuration on DATA0. Kept per endpoint
    *  because it is per endpoint: a device's IN and OUT toggles are
    *  independent of each other. */
    ep->toggle = 0;

    dev->endpoints++;
    return 1;
}

/* Walks the block of descriptors that follows the configuration header and
*  fills in the interface and endpoints of dev.
*
*  HOW THE WALK IS BOUNDED, which is the whole point of this function. The
*  bytes come from a device and nothing about them is trustworthy.
*
*    - avail is how many bytes ACTUALLY ARRIVED, never wTotalLength. A device
*      that claims more than it sent cannot make this read past the end of the
*      buffer, and one that claims less simply gets less of itself parsed.
*    - The loop condition needs two bytes present before it reads bLength and
*      bDescriptorType, so the header of a descriptor is never read off the end
*      by the check that is meant to prevent reading off the end.
*    - A bLength below 2 ends the walk. It is the endless loop: the cursor
*      would advance by 0 or by less than the header just consumed, forever, in
*      a kernel with no way out of it.
*    - A descriptor whose length runs past avail ends the walk, before any of
*      its fields are touched.
*    - Every descriptor is additionally required to be at least as long as the
*      struct that is copied out of it, so a truncated interface descriptor
*      claiming bLength 4 cannot have nine bytes read from it.
*
*  Because bLength is at least 2 and the cursor advances by bLength, the cursor
*  grows every turn. Termination follows from the arithmetic, not from the data
*  being sensible.
*
*  WHICH INTERFACE. usb_device holds one, per usb.h, but a real device may
*  offer several -- QEMU's usb-kbd does not, a webcam does, and a printer that
*  also claims to be a mass storage device does. The rule is: take the first
*  one seen, and upgrade later to the first one whose class this kernel
*  actually has a driver for, discarding the endpoints collected for the
*  previous choice. That is usb.h's "bound on the first one a driver claims"
*  as closely as it can be implemented before any driver has been asked. The
*  others are counted and reported, and are otherwise ignored -- their
*  endpoints must NOT be collected, because an endpoint belongs to the
*  interface it follows and mixing two interfaces' endpoints into one device
*  produces a device that stalls on its first transfer.
*
*  Alternate settings are skipped entirely. An alternate other than 0 is not
*  active until SET_INTERFACE selects it, so its endpoints do not exist on a
*  freshly configured device.
*
*  Returns 1 when an interface was found, 0 when the block held none. */
static int usb_parse_config(usb_device *dev, const uint8_t *buf, int avail,
                            int *interfaces_seen)
{
    usb_interface_desc iface;
    int  pos;
    int  len;
    int  type;
    int  have_iface;
    int  chosen_known;
    int  in_iface;
    int  known;

    have_iface    = 0;
    chosen_known  = 0;
    in_iface      = 0;
    *interfaces_seen = 0;
    dev->endpoints   = 0;

    pos = 0;
    while(pos + 2 <= avail)
    {
        len  = buf[pos];
        type = buf[pos + 1];

        /* The endless loop, and the only way out of it. */
        if(len < 2)
            break;

        /* Runs past what arrived. Checked before any field is read. */
        if(pos + len > avail)
            break;

        if(type == USB_DESC_INTERFACE && len >= (int)sizeof(usb_interface_desc))
        {
            memcpy(&iface, buf + pos, sizeof(iface));

            if(iface.alternate != 0)
            {
                /* Not active until SET_INTERFACE. Everything up to the next
                *  interface descriptor belongs to it and is skipped with it. */
                in_iface = 0;
            }
            else
            {
                (*interfaces_seen)++;
                known = usb_class_is_known(iface.iface_class);

                if(!have_iface || (known && !chosen_known))
                {
                    dev->iface_class    = iface.iface_class;
                    dev->iface_subclass = iface.iface_subclass;
                    dev->iface_protocol = iface.iface_protocol;
                    dev->iface_number   = iface.number;

                    /* Switching choice throws away the endpoints belonging to
                    *  the interface being abandoned. They are collected again
                    *  from here on, for the new one. */
                    dev->endpoints = 0;

                    have_iface   = 1;
                    chosen_known = known;
                    in_iface     = 1;
                }
                else
                {
                    in_iface = 0;
                }
            }
        }
        else if(type == USB_DESC_ENDPOINT && in_iface &&
                len >= (int)sizeof(usb_endpoint_desc))
        {
            usb_take_endpoint(dev, buf + pos);
        }

        /* Class specific descriptors -- a HID descriptor, an audio control
        *  block -- land here and are stepped over by their own length. That is
        *  the reason the walk is a walk and not a cast onto a struct. */
        pos += len;
    }

    return have_iface;
}

/* Reads the configuration, in the two halves it arrives in, and parses it.
*  Returns 0 or a negative error. */
static int usb_read_config(usb_device *dev, int *interfaces_seen)
{
    usb_config_desc header;
    int             want;
    int             got;
    int             avail;
    int             claimed;

    /* First read: the 9 byte header only, because until it has been read there
    *  is no way to know how much there is. */
    got = usb_get_descriptor(dev, USB_DESC_CONFIG, 0, &header,
                            (uint16_t)sizeof(header));
    if(got < (int)sizeof(header))
        return got < 0 ? got : USB_EIO;

    if(header.type != USB_DESC_CONFIG || header.length < sizeof(header))
        return USB_EIO;

    claimed = header.total_length;

    /* A block that does not even contain its own header is a broken device. */
    if(claimed < (int)sizeof(header))
        return USB_EIO;

    /* Second read: the whole block, clamped to the buffer. Truncation is not
    *  an error -- the interface a driver wants comes first and what arrives is
    *  walkable regardless -- but nothing beyond USB_CONFIG_BUF is ever asked
    *  for, so a device claiming 64 KB cannot turn this into a buffer
    *  overflow by being believed. */
    want = claimed;
    if(want > (int)sizeof(usb_config_buf))
        want = (int)sizeof(usb_config_buf);

    memset(usb_config_buf, 0, sizeof(usb_config_buf));

    got = usb_get_descriptor(dev, USB_DESC_CONFIG, 0, usb_config_buf,
                            (uint16_t)want);
    if(got < (int)sizeof(header))
        return got < 0 ? got : USB_EIO;

    /* What the walk may look at is what ARRIVED, and no more. A device that
    *  asked for 200 bytes to be believed and then sent 34 gets 34 walked. */
    avail = got;
    if(avail > want)
        avail = want;

    /* The header came a second time inside the block, and it need not agree
    *  with the first copy -- a device is free to be inconsistent, or to be
    *  malicious about which of the two reads it lies in. Believe whichever of
    *  the two lengths is SMALLER, since a shorter bound can only make the walk
    *  safer, and never let it exceed what arrived. */
    memcpy(&header, usb_config_buf, sizeof(header));
    if(header.total_length >= sizeof(header) && (int)header.total_length < avail)
        avail = (int)header.total_length;

    dev->config_value = header.value;

    if(!usb_parse_config(dev, usb_config_buf, avail, interfaces_seen))
        return USB_EIO;

    return 0;
}

/* --- Enumerating one port ------------------------------------------------ */

/* Everything a device needs to become usable, in the order the bus insists on.
*  Returns 1 when a device was added to the table, 0 when the port was empty,
*  and a negative error when something answered but could not be brought up.
*
*  The device is built in a LOCAL. Nothing is written into the table until the
*  last step has succeeded, so a failure at any point cannot leave a half
*  filled entry that later code would read as a working device. */
static int usb_enum_port(int port)
{
    usb_device      dev;
    usb_device_desc desc;
    char            vendor_hex[8];
    char            product_hex[8];
    int             speed;
    int             result;
    int             interfaces;

    speed = USB_SPEED_FULL;

    result = usb_hc->port_reset(port, &speed);
    if(result <= 0)
        return 0;                       /* nothing plugged in, which is normal */

    speed = usb_speed_or_full(speed);

    sleep(USB_RESET_RECOVERY_MS);

    memset(&dev, 0, sizeof(dev));
    dev.used        = 1;
    dev.port        = port;
    dev.speed       = speed;
    dev.address     = 0;
    dev.max_packet0 = 8;
    dev.driver      = "none";

    /* No slot left. Handled here, before anything is asked of the device, but
    *  the device is still moved off address 0 below -- a device this kernel
    *  cannot use must still not be allowed to answer for one it can. */
    if(usb_devices >= USB_MAX_DEVICES)
    {
        usb_error_text = "USB: no free device slot";
        usb_assign_address(&dev);
        return USB_ENOMEM;
    }

    result = usb_read_device_desc(&dev, &desc);
    if(result < 0)
    {
        usb_error_text = "USB: device descriptor failed";
        goto failed;
    }

    dev.vendor  = desc.vendor;
    dev.product = desc.product;

    result = usb_read_config(&dev, &interfaces);
    if(result < 0)
    {
        usb_error_text = "USB: configuration descriptor failed";
        goto failed;
    }

    /* SET_CONFIGURATION is what actually turns the device on. Before it, the
    *  endpoints just recorded do not exist and a transfer to any of them is
    *  answered by nothing -- which is why this is the last step and why a
    *  failure here means the whole device is unusable rather than merely
    *  half configured.
    *
    *  bConfigurationValue is passed through exactly as the device gave it.
    *  A value of 0 is reserved for "unconfigured" and a device reporting it is
    *  broken, but cheap ones do, and sending back what was received is more
    *  likely to work than second guessing it. */
    result = usb_control(&dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_SET_CONFIGURATION, dev.config_value, 0, 0, 0);
    if(result < 0)
    {
        usb_error_text = "USB: SET_CONFIGURATION failed";
        goto failed;
    }

    /* Committed, in one write. */
    usb_dev[usb_devices] = dev;
    usb_devices++;

    usb_hex16(vendor_hex, dev.vendor);
    usb_hex16(product_hex, dev.product);
    printf("USB: port %d: %s:%s %s (class %d/%d/%d), %d endpoint(s)%s\n",
           port, vendor_hex, product_hex, usb_class_name(dev.iface_class),
           (int)dev.iface_class, (int)dev.iface_subclass, (int)dev.iface_protocol,
           dev.endpoints, interfaces > 1 ? ", extra interfaces ignored" : "");

    return 1;

failed:
    /* THE STATE THE BUS IS LEFT IN, which is the part that decides whether the
    *  NEXT port works.
    *
    *  A device that failed after SET_ADDRESS keeps its address. Taking it back
    *  would mean sending SET_ADDRESS 0, which puts it back where it answers
    *  every transfer aimed at an unaddressed device -- exactly on top of the
    *  next port's device during its own enumeration. The address is one that
    *  will never be handed out again, so leaving it costs nothing but one of
    *  127 numbers.
    *
    *  A device that failed BEFORE it got an address is the dangerous case: it
    *  is still at 0. Moving it away is attempted here, and if that fails too
    *  there is nothing more this layer can do -- usb_hc_ops has no way to
    *  disable a port -- so the failure is reported and enumeration goes on,
    *  which is still better than stopping and finding nothing at all.
    *
    *  The table is untouched either way: dev is a local and was never copied
    *  into it. */
    if(dev.address == 0)
        usb_assign_address(&dev);

    return result;
}

/* --- Bringing the whole thing up ----------------------------------------- */

void usb_init(void)
{
    int ports;
    int port;

    /* The controller registers itself from its own init. Guarded on usb_hc so
    *  that a main.c which brings the controller up itself does not have it
    *  initialised twice, and one which does not still gets a working bus. */
    if(!usb_hc)
        uhci_init();

    if(!usb_hc)
    {
        /* The ordinary outcome on a machine QEMU started without a controller,
        *  and on any machine built in the last decade, which has xHCI and
        *  nothing this kernel drives. Nothing here may keep the boot from
        *  finishing. */
        printf("USB: no controller found\n");
        return;
    }

    ports = usb_hc->port_count();
    if(ports < 0)
        ports = 0;

    printf("USB: %s, %d root port(s)\n", usb_hc->name, ports);

    /* ONE PORT AT A TIME, START TO FINISH. Resetting both ports first and then
    *  addressing them would leave two devices answering at address 0 on the
    *  same electrical bus, and both would reply to the first control transfer.
    *  See point 1 in the file header. */
    for(port = 0; port < ports; port++)
        usb_enum_port(port);
}
