/* TomatOS - USB core
*  Desc: Devices, descriptors and transfers, independent of the controller.
*
*  WHY THIS FILE EXISTS SEPARATELY FROM THE CONTROLLER. Roughly two thirds of a
*  USB stack is the same whatever silicon is underneath: resetting a port,
*  giving the device an address, reading its descriptors, working out what it
*  is, and then speaking the class protocol -- keyboard, mouse, disk. Only the
*  bottom third, moving bytes to an endpoint, is controller specific, and there
*  are four incompatible ways to do that: UHCI, OHCI, EHCI and xHCI.
*
*  This kernel starts with UHCI because it is the simplest and QEMU has it.
*  Modern hardware does not: a machine built in the last decade has xHCI and
*  nothing else. Everything above this line is meant to survive that change,
*  which is the whole reason there is a line.
*
*  So a controller driver implements usb_hc_ops and registers it, and nothing
*  above ever learns which one answered.
*
*  DELIBERATELY ABSENT for now: hubs (so one device per root port), isochronous
*  transfers (audio and video, which need bandwidth reservation), power
*  management, and anything about USB 3 -- its bus is electrically separate and
*  is reached only through xHCI.
*/
#ifndef __USB_H
#define __USB_H

#include "typedefs.h"

/* Devices tracked at once. One per root port, and the number of root ports is
*  not two: a UHCI controller has two, but a machine has as many UHCI
*  controllers as its chipset gives it and the driver drives all of them, so an
*  ICH9 presents six ports and an ICH4 four. Eight covers every chipset that
*  has ever shipped six, and it leaves room for the hub support that is not
*  here yet.
*
*  What happens past it is not a crash and not silence: usb_enum_port() refuses
*  the ninth device with USB_ENOMEM, still moves it off address 0 so it cannot
*  shout over anything, and usb_last_error() says why. */
#define USB_MAX_DEVICES   8

/* Endpoints recorded per device. A boot keyboard needs one, a disk two. */
#define USB_MAX_ENDPOINTS 4

/* Bus speeds. Low speed is 1.5 Mbit and is what keyboards and mice use; full
*  speed is 12 Mbit and is what a USB 1.1 disk gets. The distinction is not
*  cosmetic -- it changes the packet size a control transfer may use and, on
*  some controllers, how the transfer is scheduled. */
#define USB_SPEED_LOW     0
#define USB_SPEED_FULL    1

/* Standard request types, the bmRequestType byte. */
#define USB_DIR_OUT       0x00
#define USB_DIR_IN        0x80
#define USB_TYPE_STANDARD 0x00
#define USB_TYPE_CLASS    0x20
#define USB_RECIP_DEVICE  0x00
#define USB_RECIP_IFACE   0x01
#define USB_RECIP_ENDPOINT 0x02

/* Standard requests. */
#define USB_REQ_GET_STATUS        0
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_SET_FEATURE       3
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_GET_CONFIGURATION 8
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE     11

/* Descriptor types. */
#define USB_DESC_DEVICE    1
#define USB_DESC_CONFIG    2
#define USB_DESC_STRING    3
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT  5

/* Interface classes this kernel cares about. */
#define USB_CLASS_HID          0x03
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HUB          0x09

/* Endpoint attributes, the low two bits of bmAttributes. */
#define USB_XFER_CONTROL   0
#define USB_XFER_ISOC      1
#define USB_XFER_BULK      2
#define USB_XFER_INTERRUPT 3

/* Errors. Negative, so a byte count stays distinguishable. */
#define USB_ENODEV   (-1)   /* nothing there, or no controller            */
#define USB_ESTALL   (-2)   /* the endpoint said no -- a real answer      */
#define USB_ETIMEOUT (-3)   /* the transfer never completed               */
#define USB_EIO      (-4)   /* the controller reported an error           */
#define USB_EINVAL   (-5)   /* an argument makes no sense                 */
#define USB_ENOMEM   (-6)   /* no buffer or no device slot                */

/* --- What comes off the wire -------------------------------------------- */

/* The eight bytes that start every control transfer. Little endian on the
*  wire, which is also this machine's order, so unlike the network stack
*  nothing here needs converting -- USB was specified for exactly this CPU. */
typedef struct
{
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed)) usb_setup;

typedef struct
{
    uint8_t  length;
    uint8_t  type;
    uint16_t usb_version;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  max_packet0;      /* for endpoint 0 -- 8, 16, 32 or 64        */
    uint16_t vendor;
    uint16_t product;
    uint16_t device_version;
    uint8_t  manufacturer_str;
    uint8_t  product_str;
    uint8_t  serial_str;
    uint8_t  configurations;
} __attribute__((packed)) usb_device_desc;

typedef struct
{
    uint8_t  length;
    uint8_t  type;
    uint16_t total_length;     /* this descriptor AND everything after it  */
    uint8_t  interfaces;
    uint8_t  value;            /* what SET_CONFIGURATION wants             */
    uint8_t  config_str;
    uint8_t  attributes;
    uint8_t  max_power;        /* in 2 mA units                            */
} __attribute__((packed)) usb_config_desc;

typedef struct
{
    uint8_t length;
    uint8_t type;
    uint8_t number;
    uint8_t alternate;
    uint8_t endpoints;
    uint8_t iface_class;
    uint8_t iface_subclass;
    uint8_t iface_protocol;
    uint8_t iface_str;
} __attribute__((packed)) usb_interface_desc;

typedef struct
{
    uint8_t  length;
    uint8_t  type;
    uint8_t  address;          /* bit 7 is the direction, low 4 the number */
    uint8_t  attributes;
    uint16_t max_packet;
    uint8_t  interval;         /* polling interval in frames               */
} __attribute__((packed)) usb_endpoint_desc;

/* --- A device ----------------------------------------------------------- */

typedef struct
{
    uint8_t  address;          /* 1..127, as assigned by SET_ADDRESS       */
    uint8_t  direction;        /* USB_DIR_IN or USB_DIR_OUT                */
    uint8_t  type;             /* USB_XFER_*                               */
    uint8_t  interval;
    uint16_t max_packet;
    uint8_t  toggle;           /* DATA0/DATA1, kept per endpoint           */
} usb_endpoint;

typedef struct
{
    int      used;

    /* The root port it is plugged into, in the flat numbering usb_hc_ops
    *  describes. It is not merely a label: it is what the controller driver
    *  maps back to the controller this device is on, so it must survive for as
    *  long as the device does. */
    int      port;
    int      speed;            /* USB_SPEED_*                              */
    uint8_t  address;          /* 0 until SET_ADDRESS succeeds             */
    uint16_t max_packet0;

    uint16_t vendor;
    uint16_t product;

    /* From the interface descriptor. One interface is supported; a device
    *  with several is bound on the first one a driver claims. */
    uint8_t  iface_class;
    uint8_t  iface_subclass;
    uint8_t  iface_protocol;
    uint8_t  iface_number;
    uint8_t  config_value;

    usb_endpoint endpoint[USB_MAX_ENDPOINTS];
    int          endpoints;

    /* Which class driver took it, for the shell to show. Never null. */
    const char  *driver;
} usb_device;

/* --- What a controller has to provide ----------------------------------- */

/* The line between this file and the silicon. A controller driver fills one
*  of these in and hands it to usb_register_hc(); nothing above ever sees it
*  again.
*
*  Every call is synchronous and bounded: this kernel has a wait mechanism but
*  no asynchronous completion path, and a transfer that never finishes must
*  become an error rather than a hang. Timeouts belong to the controller,
*  which is the only layer that knows how long its hardware takes.
*
*  All of them run in TASK context. A controller may use interrupts internally
*  but must not require the caller to be in one. */
typedef struct
{
    const char *name;          /* "UHCI", for the shell                    */

    /* How many root ports, and what is on one. reset() returns 1 when a
    *  device answered and fills in the speed, 0 when the port is empty.
    *
    *  ONE FLAT NUMBERING, 0..port_count()-1, even when the driver is running
    *  several controllers -- which the UHCI one does, because a chipset gives
    *  a machine three or six of them and the sockets are split between them.
    *  There is deliberately no controller argument anywhere in this structure:
    *  a driver that has more than one maps the port number back to the
    *  controller itself, and the layer above is spared a concept it has no use
    *  for. usb_device.port carries that number, so a control transfer to a
    *  device can be routed from the device alone. */
    int (*port_count)(void);
    int (*port_reset)(int port, int *speed);

    /* A control transfer. data may be null when length is 0. Returns the
    *  number of bytes moved in the data stage, or a negative error.
    *  A STALL is USB_ESTALL and is a legitimate answer -- it is how a device
    *  says "I do not support that request", and enumeration relies on it. */
    int (*control)(usb_device *dev, const usb_setup *setup,
                   void *data, int length);

    /* One interrupt IN transfer. Returns bytes moved, 0 when the device had
    *  nothing to say (NAK, which is the normal case for an idle keyboard and
    *  must not be an error), or negative. */
    int (*interrupt_in)(usb_device *dev, int endpoint, void *buf, int length);

    /* Bulk, in either direction. Returns bytes moved or negative. */
    int (*bulk)(usb_device *dev, int endpoint, void *buf, int length, int in);
} usb_hc_ops;

extern int usb_register_hc(const usb_hc_ops *ops);

/* --- The core ------------------------------------------------------------ */

/* Brings up whatever controller is present and enumerates what is plugged in.
*  Safe on a machine with no USB at all. Called once at boot. */
extern void usb_init(void);

extern int usb_present(void);
extern const char *usb_hc_name(void);

extern int usb_device_count(void);
extern const usb_device *usb_device_get(int index);

/* A control transfer on a device the core knows about, with the setup packet
*  spelled out. This is what a class driver uses for its own requests -- SET
*  PROTOCOL for a keyboard, GET MAX LUN for a disk. */
extern int usb_control(usb_device *dev, uint8_t request_type, uint8_t request,
                       uint16_t value, uint16_t index,
                       void *data, uint16_t length);

/* The two transfer kinds a class driver needs beyond control. Both go
*  straight to the controller; they are here so a driver includes one header. */
extern int usb_interrupt_in(usb_device *dev, int endpoint, void *buf, int length);
extern int usb_bulk(usb_device *dev, int endpoint, void *buf, int length, int in);

/* Counters, for the shell. */
extern uint32_t usb_transfers(void);
extern uint32_t usb_errors(void);

/* Why the last thing that failed, failed. Empty when nothing has. */
extern const char *usb_last_error(void);

/* --- The HID class driver ------------------------------------------------
*
*  Declared here rather than in a header of its own because it is the first
*  class driver and there is exactly one; a second would be the moment to
*  split them apart.
*
*  It claims interfaces of class 3 subclass 1 -- boot protocol keyboards and
*  mice -- and delivers what they say into the queues the PS/2 drivers already
*  fill, through kb_inject() and mouse_inject(). Nothing above learns which bus
*  a keypress came from, which is the whole point of those two entry points. */
extern void usbhid_init(void);

extern int  usbhid_present(void);
extern int  usbhid_count(void);

/* One line about the claimed interface at "index": what it is, which port, and
*  whether it has since been unplugged. A static buffer, never null. */
extern const char *usbhid_describe(int index);

/* What has actually come off the wire. reports excludes NAKs, so a keyboard
*  nobody is touching leaves it still -- which is what makes it useful: a
*  number that climbs while nothing is being typed means the device is
*  chattering, and one that stays at zero while keys are pressed means the
*  polling is not reaching it. */
extern uint32_t usbhid_reports(void);
extern uint32_t usbhid_keys(void);
extern uint32_t usbhid_moves(void);
extern uint32_t usbhid_errors(void);

/* --- The mass storage class driver ---------------------------------------
*
*  Claims interfaces of class 8 subclass 6 protocol 0x50 -- SCSI over the
*  bulk-only transport -- and registers each usable unit with the block layer,
*  so a stick is mounted by fat_mount() exactly as a hard disk is. See
*  blockdev.h for why the numbers it gets are above the ATA range.
*
*  No task of its own: every transfer runs on the caller's, inside blk_read()
*  or blk_write(), with a lock per interface so two tasks cannot interleave
*  commands on one pipe. */
extern void usbmsc_init(void);

extern int  usbmsc_present(void);
extern int  usbmsc_count(void);

/* One line about unit "index" -- what it is, how big, and why it was refused
*  if it was. Never null. */
extern const char *usbmsc_describe(int index);

/* The block device number the unit took, or -1 for one that was refused --
*  a sector size that is not 512, or a medium that never became ready. */
extern int usbmsc_blkdev(int index);

/* commands is every SCSI command issued. The three failure counts are
*  deliberately separate, because they mean different things to whoever is
*  looking: failures is the DEVICE rejecting a command, which is a real answer
*  and often an expected one (no medium in the slot); errors is the transport
*  losing it; resets is how often the two ends had to be put back in step. */
extern uint32_t usbmsc_commands(void);
extern uint32_t usbmsc_failures(void);
extern uint32_t usbmsc_errors(void);
extern uint32_t usbmsc_stalls(void);
extern uint32_t usbmsc_resets(void);

extern const char *usbmsc_last_error(void);

#endif
