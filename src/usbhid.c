/* TomatOS - USB HID boot protocol driver
*  Desc: Keyboards and mice on the USB bus, delivered into the same two
*        queues the PS/2 drivers fill.
*
*  WHAT THIS FILE IS FOR. src/usb.c enumerates whatever is plugged in and stops
*  there: it addresses a device, reads its descriptors and configures it, and
*  then hands the result to whoever recognises the class triple. This is the
*  driver for class 3 (HID), subclass 1 (boot interface), protocol 1 (keyboard)
*  and 2 (mouse). Nothing above it learns that a key or a movement came off the
*  USB bus rather than off the 8042 -- that is the whole promise mouse.h and
*  system.h make in the comments above mouse_inject() and kb_inject(), and this
*  file is what cashes it in.
*
*  WHY BOOT PROTOCOL AND NOT THE REAL ONE. A HID device normally describes the
*  layout of its reports with a REPORT DESCRIPTOR: a little stack language of
*  usage pages, logical minima, report sizes and collections, which has to be
*  parsed before a single byte of a report means anything. It is the honest way
*  to talk to a gamepad, a tablet or a keyboard with media keys on it, and it
*  is several hundred lines of parser before the first key arrives.
*
*  Boot protocol is the fixed alternative that exists for exactly this
*  situation. Every keyboard and every mouse that claims subclass 1 must
*  support two report layouts that are written down in the HID specification
*  itself -- appendix B -- so there is nothing to parse: eight bytes for a
*  keyboard, three for a mouse, and both are described below. It is what a BIOS
*  uses to let you into the setup screen, which is why device makers actually
*  get it right. The price is that no key beyond the 104 of a PC keyboard and
*  no third axis beyond a wheel can be reported, and that is a price this
*  kernel is happy to pay for now.
*
*  Boot protocol is not the default, though. A device comes up in REPORT
*  protocol and has to be told; SET_PROTOCOL below is not decoration, and a
*  device that never receives it sends a layout nobody agreed on. On QEMU that
*  looks deceptively close to working, because its report layout for a mouse
*  happens to start with the same three bytes -- which is exactly the kind of
*  thing that hides the missing request until real hardware is plugged in.
*
*  WHAT IS DELIBERATELY NOT HERE:
*
*    - The report descriptor parser, as above.
*    - The LEDs. Setting Caps Lock's light is a SET_REPORT on the output
*      report, and there is nothing to light it for: kb.c ignores Caps Lock
*      entirely, so this driver ignores it too (see the layout notes).
*    - AltGr. kb.c has no AltGr either, so the German layout here reaches the
*      same characters the PS/2 keyboard reaches and no more; see below.
*    - Hot plug. usb.c enumerates once at boot and has no port-change path, so
*      a device plugged in later is not found. A device REMOVED, on the other
*      hand, has to be survived, because this file is the only thing still
*      talking to it -- see usbhid_poll_one().
*/

#include <system.h>
#include <string.h>
#include <usb.h>
#include <mouse.h>

/* --- The class protocol -------------------------------------------------- */

/* Subclass 1 is "boot interface": the device promises the two fixed report
*  layouts. A HID device with subclass 0 has only its report descriptor, and
*  this driver has nothing to say to it. */
#define HID_SUBCLASS_BOOT       0x01
#define HID_PROTOCOL_KEYBOARD   0x01
#define HID_PROTOCOL_MOUSE      0x02

/* Class requests, on the INTERFACE. wIndex is the interface number for all of
*  them, which is why usb_device.iface_number is kept. */
#define HID_REQ_GET_REPORT      0x01
#define HID_REQ_GET_IDLE        0x02
#define HID_REQ_GET_PROTOCOL    0x03
#define HID_REQ_SET_REPORT      0x09
#define HID_REQ_SET_IDLE        0x0A
#define HID_REQ_SET_PROTOCOL    0x0B

/* wValue for SET_PROTOCOL. 0 is boot, 1 is report -- and the numbering is the
*  wrong way round from the way it reads, so it is spelled out here rather than
*  written as a bare 0 at the call site. */
#define HID_BOOT_PROTOCOL       0
#define HID_REPORT_PROTOCOL     1

/* bmRequestType for a class request to an interface, host to device. */
#define HID_OUT_TO_IFACE  (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_IFACE)

/* CLEAR_FEATURE(ENDPOINT_HALT) -- feature selector 0, recipient endpoint. The
*  one recovery this driver attempts; see usbhid_poll_one(). */
#define USB_FEATURE_ENDPOINT_HALT 0

/* --- Report layouts ------------------------------------------------------ */

/* The boot keyboard report, HID 1.11 appendix B.1:
*
*      byte 0   modifier bitmap, see HID_MOD_* below
*      byte 1   reserved, always 0 -- OEM use, and no OEM uses it
*      byte 2   \
*      ...       > six key slots, each a usage ID, 0 for an empty slot
*      byte 7   /
*
*  IT IS A STATE AND NOT AN EVENT, and everything difficult about a USB
*  keyboard follows from that one sentence. The device does not say "the A key
*  went down"; it says "A is among the keys that are down right now", and it
*  keeps saying so, unchanged, for as long as the finger is there. Turning that
*  back into keystrokes is usbhid_keyboard_report(). */
#define HID_KBD_REPORT_LEN  8
#define HID_KBD_SLOT_FIRST  2
#define HID_KBD_SLOT_LAST   7

/* Modifier bits in byte 0. Left and right are separate, which is more than
*  kb.c can tell apart and more than anything here needs. */
#define HID_MOD_LCTRL   0x01
#define HID_MOD_LSHIFT  0x02
#define HID_MOD_LALT    0x04
#define HID_MOD_LGUI    0x08
#define HID_MOD_RCTRL   0x10
#define HID_MOD_RSHIFT  0x20
#define HID_MOD_RALT    0x40
#define HID_MOD_RGUI    0x80
#define HID_MOD_SHIFT   (HID_MOD_LSHIFT | HID_MOD_RSHIFT)

/* The first usage ID that names a real key. 0 is an empty slot and 1, 2 and 3
*  are ErrorRollOver, POSTFail and ErrorUndefined -- error codes that live in
*  the same slots as keys do and must never be looked up in a table. */
#define HID_USAGE_FIRST_KEY 4
#define HID_USAGE_ROLLOVER  0x01

/* The boot mouse report, HID 1.11 appendix B.2:
*
*      byte 0   button bitmap, bit 0 left, bit 1 right, bit 2 middle
*      byte 1   dx, signed 8 bit
*      byte 2   dy, signed 8 bit
*
*  A device may append a wheel byte, and QEMU's usb-mouse does when it is asked
*  for four bytes, so four is what is asked for and the fourth byte is dropped:
*  mouse.h has no wheel and inventing one here would be a change to that
*  interface rather than to this file. */
#define HID_MOUSE_REPORT_LEN 3
#define HID_MOUSE_ASK_LEN    4

/* The largest report this driver ever asks for, and the size of the buffer. */
#define HID_MAX_REPORT_LEN   8

/* --- Tuning -------------------------------------------------------------- */

/* Interfaces this driver binds at once. Two is what a PC has; USB_MAX_DEVICES
*  is eight and there is no hub support, so four is already generous. */
#define USBHID_MAX_IFACES 4

/* How long the poll task sleeps when EVERY device answered "nothing".
*
*  THE ENDPOINT ASKS FOR TEN MILLISECONDS -- bInterval is 10 on both of QEMU's
*  devices and on essentially every real keyboard and mouse -- and both
*  directions of ignoring it cost something. Polling faster wastes bus frames
*  and CPU on a device that is guaranteed to say nothing new; polling slower
*  loses nothing (the report is a state, so the next poll still carries it) but
*  shows up directly as lag between the hand and the screen.
*
*  This is a sleep ON TOP of the controller's own window, not the poll rate by
*  itself. usb_interrupt_in() is synchronous and uhci.c gives an interrupt
*  transfer ten milliseconds to complete before it reports the NAK, so an idle
*  device already costs ten milliseconds of blocked task per poll and the round
*  over two devices already takes about twenty. What this constant buys is that
*  the task is BLOCKED rather than merely slow between rounds, which is the
*  difference the scheduler can see: a blocked task is not elected at all,
*  while a task that returns immediately is handed a full slice to spend on
*  nothing. Ten milliseconds keeps the worst case round -- mouse window,
*  keyboard window, sleep -- at about thirty, which is under the threshold
*  where typing starts to feel behind the fingers. */
#define USBHID_POLL_MS 10

/* Auto-repeat, and the decision to have it at all.
*
*  A PS/2 keyboard repeats IN HARDWARE: hold a key and the keyboard itself
*  sends the scancode again, at a rate the 8042 can be told to change. A USB
*  keyboard does nothing of the kind. It reports the key as held and that is
*  all it will ever do; the six slots keep saying the same thing until the
*  finger comes off. So a driver that only turns new slots into characters
*  gives the user exactly one character per key press, forever -- and the first
*  thing anybody notices is that backspace no longer eats a line.
*
*  This driver therefore repeats in software, and the alternative -- not
*  repeating, and documenting it -- was rejected for that one reason: the
*  keyboard would behave visibly differently from the PS/2 one on the same
*  machine, and the whole point of kb_inject() is that it should not.
*
*  The numbers are the PS/2 defaults, which is what the rest of this machine
*  would have been doing: about half a second before the first repeat, then
*  roughly ten a second. Only the most recently pressed key repeats, exactly as
*  a real keyboard does -- press a second key and it takes over.
*
*  The repeat is not free of the poll rate: it can only fire when the poll task
*  gets a turn, so the real interval is USBHID_REPEAT_RATE_MS rounded up to the
*  next round of the loop. At 100 ms against a ~30 ms round that is close
*  enough that nobody can tell. */
#define USBHID_REPEAT_DELAY_MS 500
#define USBHID_REPEAT_RATE_MS  100

/* Consecutive hard errors before an interface is given up as gone.
*
*  A device that is unplugged does not announce it. usb.c has no port change
*  path, so the first this driver hears of it is that usb_interrupt_in() starts
*  failing: the controller sends the token, nothing answers, the descriptor's
*  error counter runs out and uhci.c reports USB_ESTALL (a halted queue) or
*  USB_ETIMEOUT. Retrying that forever is a task spinning on a device that does
*  not exist, at ten milliseconds a go, for as long as the machine is up.
*
*  Ten in a row rather than one, because a single error is not the same thing
*  as a missing device -- a stalled endpoint is recoverable and is tried once
*  (see usbhid_poll_one) -- and because the count is reset by any answer at
*  all, including the NAK that says the device is merely idle. Ten consecutive
*  failures with no NAK in between is not a glitch. */
#define USBHID_MAX_ERRORS 10

/* Room for the one line usbhid_describe() hands the shell. */
#define USBHID_TEXT 56

/* --- The German layout --------------------------------------------------- */

/* USAGE IDS ARE NOT SCANCODES, and this is the part of the file that has to be
*  right or the keyboard is worse than useless.
*
*  kb.c's two tables are indexed by PS/2 set 1 scancodes and hold a German
*  layout in CP437 -- CP437 because that is what the VGA text mode renders, so
*  an umlaut is one byte like 0x81 and not any of the several things 'ü' could
*  mean elsewhere. A USB keyboard reports something else entirely: HID usage
*  IDs, which are numbered along the rows of a US keyboard and carry no layout
*  at all. Usage 0x1C is "the key where a US keyboard has Y", and on a German
*  keyboard that key is Z.
*
*  So this is a second table for the same layout in a different alphabet, and
*  the requirement on it is not that it be a good German layout but that it be
*  THE SAME ONE: every usage ID that names a key kb.c has a character for must
*  produce that exact character, in both shift states. That correspondence is
*  pure data and is testable on the host without any hardware -- extract these
*  four arrays and kb.c's two, walk the usage-to-scancode map, compare. It was.
*
*  Where the two tables deliberately disagree, and why:
*
*    - The '<' '>' '|' key next to the left shift (usage 0x64, scancode 0x56)
*      is 0 here because it is 0 in kb.c. Parity wins over usefulness: a
*      character that only one of the two keyboards can type is a worse bug
*      than a key that does nothing on both.
*    - AltGr is ignored, because kb.c ignores it. '@', '\', '{', '}', '[', ']'
*      and '~' are therefore unreachable from either keyboard on this machine.
*      Adding them here alone would be the same asymmetry.
*    - Caps Lock is ignored, because kb.c neither tracks it nor lights it.
*    - Ctrl and Alt do not change the character, because they do not in kb.c.
*      Ctrl+C is 'c' on both keyboards.
*    - Shift+ß is 'ß' and not '?', and shift+´ is '`'. Both are what kb.c's
*      table says, right or wrong; this file's job is to match it, not to
*      correct it. Fixing them means fixing kbdde_b[] and this table together.
*    - The keypad is where the correspondence genuinely runs out. Its '*', '-'
*      and '+' have unprefixed scancodes and match. Its digits are 0 in kb.c
*      (the scancodes are the ones the arrow keys share) and are 0 here. Its
*      Enter and its '/' are E0-prefixed, and kb.c drops the E0 byte and then
*      reads the second byte out of the ordinary table -- which turns keypad
*      Enter into '\n' by luck and keypad '/' into '-' by accident. The luck is
*      matched; the accident is not, and usage 0x54 is 0 rather than either a
*      replicated bug or a '/' that only the USB keyboard could type.
*
*  Everything from usage 0x68 upwards -- F13 and above, the international and
*  media keys, and the modifiers at 0xE0..0xE7 -- is off the end of the table
*  and reads as no character. The modifiers never appear in a key slot anyway;
*  they arrive in the modifier byte, which is the other thing that separates
*  this from a scancode stream: shift is not a key that produces a character,
*  it is a bit that changes what the other keys produce. */

#define USBHID_USAGE_MAX 0x68

/* Unshifted. CP437 code points, matching kbdde_s[] key for key. */
static const unsigned char usbhid_de_plain[USBHID_USAGE_MAX] =
{
    /* 0x00 */ 0,    0,    0,    0,      /* reserved, and the three errors  */
    /* 0x04 */ 'a',  'b',  'c',  'd',
    /* 0x08 */ 'e',  'f',  'g',  'h',
    /* 0x0C */ 'i',  'j',  'k',  'l',
    /* 0x10 */ 'm',  'n',  'o',  'p',
    /* 0x14 */ 'q',  'r',  's',  't',
    /* 0x18 */ 'u',  'v',  'w',  'x',
    /* 0x1C */ 'z',  'y',                /* QWERTZ: the two that swap       */
    /* 0x1E */ '1',  '2',
    /* 0x20 */ '3',  '4',  '5',  '6',
    /* 0x24 */ '7',  '8',  '9',  '0',
    /* 0x28 */ '\n', 27,   '\b', '\t',   /* Enter, Escape, Backspace, Tab   */
    /* 0x2C */ ' ',
    /* 0x2D */ 0xE1,                     /* ss -- CP437 sharp s             */
    /* 0x2E */ 0x27,                     /* the acute key; CP437 has no     */
                                         /* acute accent, so kb.c uses '\'' */
    /* 0x2F */ 0x81,                     /* ue                              */
    /* 0x30 */ '+',
    /* 0x31 */ '#',
    /* 0x32 */ '#',                      /* non-US '#', same key on a       */
                                         /* German keyboard                 */
    /* 0x33 */ 0x94,                     /* oe                              */
    /* 0x34 */ 0x84,                     /* ae                              */
    /* 0x35 */ '^',
    /* 0x36 */ ',',  '.',  '-',
    /* 0x39 */ 0,                         /* Caps Lock                      */
    /* 0x3A */ 0, 0, 0, 0, 0, 0,          /* F1..F6                         */
    /* 0x40 */ 0, 0, 0, 0, 0, 0,          /* F7..F12                        */
    /* 0x46 */ 0, 0, 0,                   /* PrintScreen, ScrollLock, Pause */
    /* 0x49 */ 0, 0, 0,                   /* Insert, Home, PageUp           */
    /* 0x4C */ 0, 0, 0,                   /* Delete, End, PageDown          */
    /* 0x4F */ 0, 0, 0, 0,                /* Right, Left, Down, Up          */
    /* 0x53 */ 0,                         /* Num Lock                       */
    /* 0x54 */ 0,                         /* keypad '/' -- see the note     */
    /* 0x55 */ '*',  '-',  '+',           /* keypad, as kb.c has them       */
    /* 0x58 */ '\n',                      /* keypad Enter                   */
    /* 0x59 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, /* keypad 1..9, 0 in kb.c         */
    /* 0x62 */ 0, 0,                      /* keypad 0 and '.'               */
    /* 0x64 */ 0,                         /* the '<' key -- 0 in kb.c       */
    /* 0x65 */ 0, 0, 0                    /* Application, Power, keypad '=' */
};

/* With either shift held. CP437 again, matching kbdde_b[]. */
static const unsigned char usbhid_de_shift[USBHID_USAGE_MAX] =
{
    /* 0x00 */ 0,    0,    0,    0,
    /* 0x04 */ 'A',  'B',  'C',  'D',
    /* 0x08 */ 'E',  'F',  'G',  'H',
    /* 0x0C */ 'I',  'J',  'K',  'L',
    /* 0x10 */ 'M',  'N',  'O',  'P',
    /* 0x14 */ 'Q',  'R',  'S',  'T',
    /* 0x18 */ 'U',  'V',  'W',  'X',
    /* 0x1C */ 'Z',  'Y',
    /* 0x1E */ '!',  '"',
    /* 0x20 */ 0x15, '$',  '%',  '&',    /* 0x15 is CP437 section sign      */
    /* 0x24 */ '/',  '(',  ')',  '=',
    /* 0x28 */ '\n', 27,   '\b', '\t',
    /* 0x2C */ ' ',
    /* 0x2D */ 0xE1,                     /* kb.c gives ss here too          */
    /* 0x2E */ '`',
    /* 0x2F */ 0x9A,                     /* UE                              */
    /* 0x30 */ '*',
    /* 0x31 */ 0x27,                     /* '\'' -- shift of the '#' key    */
    /* 0x32 */ 0x27,
    /* 0x33 */ 0x99,                     /* OE                              */
    /* 0x34 */ 0x8E,                     /* AE                              */
    /* 0x35 */ 0xF8,                     /* degree sign                     */
    /* 0x36 */ ';',  ':',  '_',
    /* 0x39 */ 0,
    /* 0x3A */ 0, 0, 0, 0, 0, 0,
    /* 0x40 */ 0, 0, 0, 0, 0, 0,
    /* 0x46 */ 0, 0, 0,
    /* 0x49 */ 0, 0, 0,
    /* 0x4C */ 0, 0, 0,
    /* 0x4F */ 0, 0, 0, 0,
    /* 0x53 */ 0,
    /* 0x54 */ 0,
    /* 0x55 */ '*',  '-',  '+',
    /* 0x58 */ '\n',
    /* 0x59 */ 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x62 */ 0, 0,
    /* 0x64 */ 0,
    /* 0x65 */ 0, 0, 0
};

/* --- State --------------------------------------------------------------- */

typedef struct
{
    int         used;
    int         live;          /* 0 once the device stopped answering      */
    usb_device *dev;           /* into usb.c's table, which never moves    */
    int         endpoint;      /* the interrupt IN endpoint number         */
    int         length;        /* bytes asked for on every poll            */
    int         keyboard;      /* 1 for a keyboard, 0 for a mouse          */
    int         errors;        /* consecutive hard errors                  */
    int         halt_cleared;  /* a stall has already been cleared once    */

    /* Keyboard only. The previous report, which is the entire reason a state
    *  can be turned back into presses. */
    uint8_t     prev[HID_KBD_REPORT_LEN];

    /* Keyboard only. The key that is repeating and when it is next due; 0 for
    *  none. See USBHID_REPEAT_DELAY_MS. */
    uint8_t     repeat_usage;
    uint32_t    repeat_due;

    char        text[USBHID_TEXT];
} usbhid_iface;

static usbhid_iface usbhid_ifaces[USBHID_MAX_IFACES];
static int          usbhid_iface_count = 0;
static int          usbhid_started     = 0;
static int          usbhid_task_pid    = -1;
static int          usbhid_task_running = 0;

static uint32_t usbhid_stat_reports = 0;
static uint32_t usbhid_stat_keys    = 0;
static uint32_t usbhid_stat_moves   = 0;
static uint32_t usbhid_stat_errors  = 0;

/* The channel the poll task sleeps on. Nothing ever wakes it: there is no
*  interrupt path from a UHCI interrupt endpoint in this kernel, so the wait is
*  a timed sleep and the address is only here because task_wait() identifies a
*  wait by one. Named rather than passed as 0 so that a future controller with
*  a completion interrupt has somewhere obvious to wake. */
static const int usbhid_channel = 0;

/* What goes into usb_device.driver.
*
*  Static storage, because usb.c keeps the pointer rather than a copy -- a
*  local buffer here would leave the table pointing at a dead stack frame.
*
*  SEVEN CHARACTERS, because "lsusb" prints this in a column nine wide and
*  keeps eight of them, so "hid-mouse" arrives as "hid-mous" -- a truncation
*  that reads exactly like a bug. "ptr" rather than "mouse" for that reason and
*  because mouse.h calls the thing a pointer throughout; the row already says
*  "HID boot mouse" in its last column, so nothing is lost. */
static const char usbhid_name_kbd[]   = "hid-kbd";
static const char usbhid_name_mouse[] = "hid-ptr";
static const char usbhid_name_gone[]  = "removed";

/* What mouse_describe() says once a report has come in this way. Same
*  requirement: mouse.c stores the pointer. */
static const char usbhid_mouse_desc[] = "3 buttons, USB boot protocol";

/* --- Small helpers ------------------------------------------------------- */

/* Wrap safe "is now at or past deadline". Both are milliseconds of uptime from
*  timer_get_ticks(), which wraps after about 49 days; comparing them with <
*  would then say the deadline is in the future forever. The difference is the
*  thing that stays meaningful across the wrap, and half the range is the
*  largest interval this can distinguish -- four orders of magnitude more than
*  the half second this is ever asked about. */
static int usbhid_time_reached(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000UL;
}

static uint32_t usbhid_now(void)
{
    return (uint32_t)timer_get_ticks();
}

/* Two string builders, because there is no snprintf here and printf is
*  deliberately not included: a driver that prints a line per report buries the
*  console it is trying to type into. Both take and return the write position
*  and always leave the buffer terminated. */
static int usbhid_put(char *out, int at, int max, const char *text)
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

static int usbhid_put_uint(char *out, int at, int max, unsigned int value)
{
    char tmp[12];
    int  n;

    n = 0;
    do
    {
        tmp[n] = (char)('0' + (value % 10u));
        n++;
        value /= 10u;
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

/* The character a usage ID produces, given the modifier byte that came with
*  it. 0 for every key that has none -- a function key, an arrow, a modifier
*  that somehow reached a slot -- which is exactly what kb_inject() ignores, so
*  the caller does not have to special case it. */
static unsigned char usbhid_char(uint8_t usage, uint8_t modifiers)
{
    if(usage >= USBHID_USAGE_MAX)
        return 0;

    if((modifiers & HID_MOD_SHIFT) != 0)
        return usbhid_de_shift[usage];

    return usbhid_de_plain[usage];
}

/* Whether a usage ID appears in the six key slots of a report. This is the
*  whole of "was it already down": a slot is a set member and the order the
*  device puts them in is not defined, so a key that moves from slot 3 to slot
*  2 between two reports has not been pressed again. */
static int usbhid_slot_held(const uint8_t *report, uint8_t usage)
{
    int i;

    for(i = HID_KBD_SLOT_FIRST; i <= HID_KBD_SLOT_LAST; i++)
    {
        if(report[i] == usage)
            return 1;
    }

    return 0;
}

/* --- The keyboard -------------------------------------------------------- */

/* Turns one boot keyboard report into key presses.
*
*  THE PHANTOM STATE COMES FIRST, before anything is compared. A boot keyboard
*  has six slots and a key matrix that cannot always tell which keys are down
*  when several are; when it cannot, it does not guess -- it fills every slot
*  with ErrorRollOver (usage 1) and sends that. The report is the device saying
*  "too many, I do not know", and it means both halves of what follows are
*  wrong: the six 1s are not six keys, and the keys that WERE down are not in
*  the report either.
*
*  So the report is dropped whole, and -- this is the part that is easy to get
*  wrong -- prev is left alone. Storing a rollover report as the previous state
*  would make every key that is still held look newly pressed on the next
*  ordinary report, so mashing a handful of keys would produce a burst of
*  characters on release. Keeping the last state the device actually knew means
*  the rollover costs nothing at all: the next real report compares against
*  what was true before it, and the keys that never moved are still not new.
*
*  AFTER THAT IT IS A SET DIFFERENCE. A usage in the new report and not in the
*  old one went down; that is a press and it produces a character. A usage in
*  the old one and not the new came up, and there is nothing to deliver for it
*  -- kb_inject() takes characters, not events, so a release has no
*  representation. Releases still have to be NOTICED, though, and for two
*  reasons: they are what stops the repeat, and without keeping prev at all a
*  key held down would either be pressed once and never again or pressed on
*  every single poll, depending on which mistake was made.
*
*  Modifiers are not keys and are not in the slots. Shift arrives as a bit in
*  byte 0 and changes what the OTHER keys produce; pressing shift on its own
*  changes the byte, matches no slot, and correctly produces nothing. */
static void usbhid_keyboard_report(usbhid_iface *hid, const uint8_t *report)
{
    unsigned char ch;
    uint8_t       usage;
    int           i;

    for(i = HID_KBD_SLOT_FIRST; i <= HID_KBD_SLOT_LAST; i++)
    {
        if(report[i] == HID_USAGE_ROLLOVER)
            return;
    }

    for(i = HID_KBD_SLOT_FIRST; i <= HID_KBD_SLOT_LAST; i++)
    {
        usage = report[i];

        /* 0 is an empty slot; 2 and 3 are the other two error codes, which a
        *  device sends alone rather than six times and which name no key. */
        if(usage < HID_USAGE_FIRST_KEY)
            continue;

        if(usbhid_slot_held(hid->prev, usage))
            continue;

        /* A new key takes the repeat over from whatever had it, whether or not
        *  it produces a character itself. That is what a real keyboard does --
        *  press shift while A is repeating and the A stops -- and it is also
        *  what keeps a key with no character from leaving the previous one
        *  repeating under it. */
        hid->repeat_usage = usage;
        hid->repeat_due   = usbhid_now() + USBHID_REPEAT_DELAY_MS;

        ch = usbhid_char(usage, report[0]);
        if(ch != 0)
        {
            kb_inject(ch);
            usbhid_stat_keys++;
        }
    }

    /* The repeating key came up: stop. Checked against the NEW report, so a
    *  key released in the same report that pressed another one is handled in
    *  the right order -- the press above already moved the repeat, and this
    *  only fires if whatever holds it now is gone. */
    if(hid->repeat_usage != 0 && !usbhid_slot_held(report, hid->repeat_usage))
        hid->repeat_usage = 0;

    memcpy(hid->prev, report, HID_KBD_REPORT_LEN);
}

/* One turn of the auto-repeat, called from the poll loop whether or not a
*  report arrived -- which is the point: the device says nothing at all while a
*  key is held, so a repeat driven by incoming reports would never fire.
*
*  The character is looked up again every time rather than remembered, so
*  pressing shift while a letter is held changes the letter being repeated,
*  which is what happens on a PS/2 keyboard for the same reason: the repeat is
*  a scancode there and goes through the same shift-dependent table. The
*  modifiers come out of prev[0], which is the last state the device reported
*  and therefore the current one. */
static void usbhid_repeat_tick(usbhid_iface *hid)
{
    unsigned char ch;
    uint32_t      now;

    if(!hid->keyboard || hid->repeat_usage == 0)
        return;

    now = usbhid_now();
    if(!usbhid_time_reached(now, hid->repeat_due))
        return;

    hid->repeat_due = now + USBHID_REPEAT_RATE_MS;

    ch = usbhid_char(hid->repeat_usage, hid->prev[0]);
    if(ch != 0)
    {
        kb_inject(ch);
        usbhid_stat_keys++;
    }
}

/* --- The mouse ----------------------------------------------------------- */

/* Turns one boot mouse report into a pointer update.
*
*  THE SIGN OF DY IS THE WHOLE OF THE DIFFICULTY, and it is invisible in the
*  source either way: both versions are one assignment and only one of them
*  moves the pointer in the direction the hand went. mouse.h settles it -- an
*  injected event is in SCREEN orientation, positive y downwards -- and a HID
*  boot mouse already reports it that way, positive y towards the user. So dy
*  goes through untouched. A PS/2 packet, which counts upwards, is flipped
*  inside mouse.c before it reaches the same code; flipping here as well would
*  invert one of the two devices, and the two lines of code would look
*  identical.
*
*  dx and dy are plain signed bytes, which is the one place a boot mouse is
*  SIMPLER than PS/2: there is no ninth sign bit hiding in the button byte and
*  therefore no way to get the sign extension subtly wrong on fast movement.
*  The cast to int8_t is still doing real work -- the buffer is unsigned, and
*  without it every leftward movement would read as a large rightward one.
*
*  The button bitmap is already MOUSE_BUTTON_LEFT, _RIGHT and _MIDDLE in that
*  order, so it is masked to three bits and passed through as the full current
*  state, which is what mouse_inject() asks for. Any further buttons a device
*  reports are dropped, because mouse.h has names for exactly three.
*
*  A report that moved nothing and changed nothing is not filtered here:
*  mouse.c already drops it, and doing it in both places would mean two
*  definitions of "nothing happened" that could drift apart. */
static void usbhid_mouse_report(const uint8_t *report)
{
    int     dx;
    int     dy;
    uint8_t buttons;

    buttons = (uint8_t)(report[0] & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT |
                                     MOUSE_BUTTON_MIDDLE));
    dx      = (int)(int8_t)report[1];
    dy      = (int)(int8_t)report[2];

    mouse_inject(dx, dy, buttons, usbhid_mouse_desc);
    usbhid_stat_moves++;
}

/* --- Claiming ------------------------------------------------------------ */

/* Finds the interrupt IN endpoint of a device, as an index into dev->endpoint.
*  A boot device has exactly one and it is the only way it ever says anything;
*  a device without one is not usable and is not claimed. */
static int usbhid_find_endpoint(const usb_device *dev)
{
    int i;

    for(i = 0; i < dev->endpoints; i++)
    {
        if(dev->endpoint[i].type != USB_XFER_INTERRUPT)
            continue;

        if((dev->endpoint[i].direction & USB_DIR_IN) == 0)
            continue;

        return i;
    }

    return -1;
}

/* Builds the line usbhid_describe() hands back, once, at claim time.
*  Everything in it is fixed for the life of the interface except the last
*  clause, which usbhid_lost() rewrites. */
static void usbhid_build_text(usbhid_iface *hid, int interval, int boot_ok)
{
    int at;

    at = usbhid_put(hid->text, 0, USBHID_TEXT,
                    hid->keyboard ? "keyboard, port " : "mouse, port ");
    at = usbhid_put_uint(hid->text, at, USBHID_TEXT,
                         (unsigned int)hid->dev->port);
    at = usbhid_put(hid->text, at, USBHID_TEXT, ", ep ");
    at = usbhid_put_uint(hid->text, at, USBHID_TEXT,
                         (unsigned int)hid->endpoint);
    at = usbhid_put(hid->text, at, USBHID_TEXT, ", ");
    at = usbhid_put_uint(hid->text, at, USBHID_TEXT, (unsigned int)interval);
    at = usbhid_put(hid->text, at, USBHID_TEXT, " ms");

    /* Only said when it is not the ordinary answer. A device that refused
    *  SET_PROTOCOL is still being read, and if what arrives turns out to be
    *  nonsense this line is the first place to look. */
    if(!boot_ok)
        usbhid_put(hid->text, at, USBHID_TEXT, ", boot not ack'd");
}

/* Takes one enumerated device, if it is a boot keyboard or mouse.
*
*  Returns 1 when the interface was claimed. Everything that can go wrong here
*  is an ordinary outcome -- a HID device that is not a boot device, a device
*  another driver already took, no endpoint -- and none of it is worth a line
*  on the console at boot. */
static int usbhid_claim(usb_device *dev)
{
    usbhid_iface *hid;
    int           ep;
    int           boot_ok;
    int           result;

    if(usbhid_iface_count >= USBHID_MAX_IFACES)
        return 0;

    if(dev->iface_class != USB_CLASS_HID)
        return 0;

    /* Subclass 1 is the promise that the two fixed report layouts exist. A HID
    *  device without it describes itself only in its report descriptor, and
    *  this driver has no parser. */
    if(dev->iface_subclass != HID_SUBCLASS_BOOT)
        return 0;

    if(dev->iface_protocol != HID_PROTOCOL_KEYBOARD &&
       dev->iface_protocol != HID_PROTOCOL_MOUSE)
        return 0;

    /* usb.c sets this to "none" and nothing else writes it yet, but the check
    *  is what keeps a second driver from binding the same interface the day
    *  there is one. */
    if(dev->driver != 0 && strcmp(dev->driver, "none") != 0)
        return 0;

    ep = usbhid_find_endpoint(dev);
    if(ep < 0)
        return 0;

    hid = &usbhid_ifaces[usbhid_iface_count];
    memset(hid, 0, sizeof(usbhid_iface));

    hid->dev      = dev;
    hid->keyboard = (dev->iface_protocol == HID_PROTOCOL_KEYBOARD);
    hid->endpoint = (int)dev->endpoint[ep].address;

    /* SET_PROTOCOL(boot). THE REQUEST THAT MAKES THE REST OF THIS FILE TRUE.
    *
    *  A HID device powers up in REPORT protocol, in which the meaning of every
    *  byte comes from its report descriptor -- which this driver has not read
    *  and cannot read. Boot protocol is the layout in the specification, and
    *  the device is in it only after being told. wValue is 0 for boot, wIndex
    *  is the interface; there is no data stage.
    *
    *  A STALL HERE IS NOT FATAL AND THE INTERFACE IS STILL CLAIMED. A device
    *  that refuses the request is nearly always one that has no report
    *  protocol to leave -- boot-only silicon, common in cheap mice -- so
    *  giving up would refuse a device that would have worked. It is recorded
    *  in the description instead, because the OTHER possible cause is a device
    *  that stayed in report protocol and is about to send a layout nobody
    *  agreed on, and that is very hard to recognise from the symptoms. */
    result = usb_control(dev, HID_OUT_TO_IFACE, HID_REQ_SET_PROTOCOL,
                         HID_BOOT_PROTOCOL, (uint16_t)dev->iface_number, 0, 0);
    boot_ok = (result >= 0);

    /* SET_IDLE with a duration of 0, which means INFINITE: report only when
    *  something changes.
    *
    *  The default is not that. A HID device's idle rate defaults to 500 ms for
    *  a keyboard -- and to whatever the device likes, often 0, for a mouse --
    *  and a non-zero idle rate means the device re-sends its last report that
    *  often even though nothing happened. Every one of those costs a bus
    *  transaction, a wake of this task, and a full pass through the press
    *  detection above, all to conclude that the state is what it already was.
    *  Worse, it makes the interesting case indistinguishable from the boring
    *  one: with idle off, a report ARRIVING means something changed, which is
    *  a property worth having.
    *
    *  wValue is the duration in the high byte (0) and the report ID in the low
    *  byte (0, meaning every report). A device that stalls this is left at its
    *  default and still works -- the extra reports are redundant, not wrong --
    *  so the result is deliberately not checked. */
    usb_control(dev, HID_OUT_TO_IFACE, HID_REQ_SET_IDLE,
                0, (uint16_t)dev->iface_number, 0, 0);

    /* How many bytes to ask for. A keyboard's report is exactly eight; a mouse
    *  sends three and QEMU's appends a wheel byte when four are asked for, so
    *  four is asked for and the fourth is ignored. Either is clamped by what
    *  the endpoint says it can move in one packet, because asking for more
    *  than that makes the controller build a second transaction the device
    *  will never answer. */
    if(hid->keyboard)
        hid->length = HID_KBD_REPORT_LEN;
    else
        hid->length = HID_MOUSE_ASK_LEN;

    if(hid->length > (int)dev->endpoint[ep].max_packet)
        hid->length = (int)dev->endpoint[ep].max_packet;

    if(hid->length > HID_MAX_REPORT_LEN)
        hid->length = HID_MAX_REPORT_LEN;

    /* A device whose endpoint cannot carry even the short report is not one
    *  this driver can read, and claiming it would mean polling forever for
    *  something that can never arrive. */
    if(hid->length < HID_MOUSE_REPORT_LEN)
        return 0;

    hid->used = 1;
    hid->live = 1;

    usbhid_build_text(hid, (int)dev->endpoint[ep].interval, boot_ok);

    dev->driver = hid->keyboard ? usbhid_name_kbd : usbhid_name_mouse;

    usbhid_iface_count++;
    return 1;
}

/* --- Polling ------------------------------------------------------------- */

/* Marks an interface gone and stops everything that was still happening on its
*  behalf. The device object stays in usb.c's table -- there is no removal path
*  there and inventing one from here would be reaching into another file's
*  invariants -- so what changes is the driver name the shell prints and this
*  file's willingness to keep talking to it. */
static void usbhid_lost(usbhid_iface *hid)
{
    hid->live = 0;

    /* A keyboard unplugged with a key held would otherwise repeat that key for
    *  as long as the machine is up, which is the one failure of this kind the
    *  user cannot ignore. */
    hid->repeat_usage = 0;

    if(hid->dev != 0)
        hid->dev->driver = usbhid_name_gone;
}

/* One poll of one interface. Returns 1 when a report arrived, so the caller
*  can tell a busy round from an idle one.
*
*  THE THREE ANSWERS, and the middle one is the one that gets drivers wrong:
*
*    positive  bytes arrived, a report to act on
*    zero      NAK: the device was asked and had nothing to say. THIS IS THE
*              NORMAL CASE and not an error -- an idle keyboard answers this
*              way every ten milliseconds for as long as nobody types, and
*              usb.h says so in as many words above the interrupt_in hook.
*              Counting it as a failure would declare every idle device broken
*              within a fraction of a second.
*    negative  the transfer failed. A stall is recoverable once; anything that
*              keeps failing is a device that is no longer there.
*
*  THE UNPLUG PATH runs through the negative case, because there is no other.
*  usb.c enumerates at boot and has no port change handling, so nothing tells
*  this driver that a device was removed; the controller simply starts getting
*  no answer, retries three times per descriptor, and reports a halted queue.
*  Ten of those in a row with no NAK in between and the interface is given up
*  -- see USBHID_MAX_ERRORS. The alternative, retrying forever, is a task that
*  spends ten milliseconds of every round on a device that has been physically
*  unplugged, for the rest of the uptime. */
static int usbhid_poll_one(usbhid_iface *hid)
{
    uint8_t buffer[HID_MAX_REPORT_LEN];
    int     result;

    /* Zeroed rather than left alone, because a short read fills only part of
    *  it and the rest would be the previous report's bytes -- which for a
    *  keyboard would be six key slots that are not in this report at all. */
    memset(buffer, 0, sizeof(buffer));

    result = usb_interrupt_in(hid->dev, hid->endpoint, buffer, hid->length);

    if(result == 0)
    {
        /* A NAK is an answer. The device is there and it is idle, so whatever
        *  errors came before it were transient. */
        hid->errors       = 0;
        hid->halt_cleared = 0;
        return 0;
    }

    if(result < 0)
    {
        hid->errors++;
        usbhid_stat_errors++;

        /* One attempt at recovery, and only for a stall. An interrupt IN
        *  endpoint that stalls is HALTED: it will refuse every transfer until
        *  the halt is cleared, so retrying without this achieves nothing.
        *  CLEAR_FEATURE(ENDPOINT_HALT) unhalts it and, by specification,
        *  resets the data toggle at BOTH ends -- so the host's copy has to be
        *  put back to DATA0 as well, or every packet afterwards is discarded
        *  as a retransmission and the endpoint looks dead in a way that is
        *  indistinguishable from the device being gone.
        *
        *  Attempted once per run of errors: if the device has actually been
        *  unplugged this control transfer fails too, and repeating it would
        *  double the time spent discovering that. */
        if(result == USB_ESTALL && !hid->halt_cleared)
        {
            int i;

            hid->halt_cleared = 1;

            usb_control(hid->dev,
                        USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                        USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
                        (uint16_t)(USB_DIR_IN | hid->endpoint), 0, 0);

            for(i = 0; i < hid->dev->endpoints; i++)
            {
                if((int)hid->dev->endpoint[i].address == hid->endpoint &&
                   (hid->dev->endpoint[i].direction & USB_DIR_IN) != 0)
                {
                    hid->dev->endpoint[i].toggle = 0;
                }
            }
        }

        if(hid->errors >= USBHID_MAX_ERRORS)
            usbhid_lost(hid);

        return 0;
    }

    hid->errors       = 0;
    hid->halt_cleared = 0;
    usbhid_stat_reports++;

    if(hid->keyboard)
    {
        /* A boot keyboard report is eight bytes and a device that sends fewer
        *  is not speaking the protocol. The buffer was zeroed, so the missing
        *  slots read as empty and the worst case is that held keys look
        *  released -- which is recoverable -- rather than that stale bytes
        *  look like keys. */
        usbhid_keyboard_report(hid, buffer);
    }
    else if(result >= HID_MOUSE_REPORT_LEN)
    {
        usbhid_mouse_report(buffer);
    }

    return 1;
}

/* The poll task.
*
*  WHY THERE IS A TASK AT ALL. usb_interrupt_in() is synchronous: it builds a
*  transfer descriptor, hands it to the controller and waits for the frame to
*  come round. uhci.c deliberately installs no interrupt handler -- QEMU puts
*  the controller on the same IRQ line as the network card, and taking it would
*  cost the network -- so there is no completion callback and nothing to hang a
*  driver off. Something has to sit there and ask, and in this kernel that is a
*  task.
*
*  IT BLOCKS RATHER THAN SPINS, which is the difference between a driver that
*  costs nothing on an idle machine and one that costs a full scheduling slice
*  forever. task_wait() on a channel nothing wakes is a timed sleep, and that
*  is exactly what is wanted here; the idiom in system.h about testing the
*  condition with interrupts off does not apply, because there is no condition
*  -- nothing can be missed by sleeping, since the next poll finds the state
*  whatever happened while this task was away. That is the one mercy of a
*  report that carries state instead of events.
*
*  A round that DID produce a report yields instead of sleeping, so a mouse
*  being dragged is drained at the rate the device produces reports rather than
*  one report per sleep. task_yield() still gives the rest of the slice up, so
*  this cannot starve anything.
*
*  IT ENDS when every interface it was given has been unplugged. A task looping
*  over an empty list is the same waste as one retrying a device that is gone,
*  and there is no hot plug path that could give it something to do again. */
static void usbhid_task(void)
{
    int worked;
    int alive;
    int i;

    for(;;)
    {
        worked = 0;
        alive  = 0;

        for(i = 0; i < usbhid_iface_count; i++)
        {
            if(!usbhid_ifaces[i].live)
                continue;

            alive = 1;

            if(usbhid_poll_one(&usbhid_ifaces[i]))
                worked = 1;
        }

        /* After the polling, not before: a key pressed in this very round
        *  gets its repeat deadline set above and must not have it tested in
        *  the same pass. */
        for(i = 0; i < usbhid_iface_count; i++)
        {
            if(usbhid_ifaces[i].live)
                usbhid_repeat_tick(&usbhid_ifaces[i]);
        }

        if(!alive)
            break;

        if(worked)
            task_yield();
        else
            task_wait(&usbhid_channel, USBHID_POLL_MS);
    }

    taskmgr_task_exit(taskmgr_get_currpid(), 0);

    /* AND THEN IT MUST NOT RETURN, which is not obvious and cost a triple
    *  fault to find out. taskmgr_task_exit() only marks the slot: it says so
    *  itself, and it has to, because it runs on the stack of the very task it
    *  is ending. This task therefore keeps executing until the next tick
    *  elects somebody else -- and if it runs off the end of this function in
    *  the meantime it returns to whatever the initial stack frame holds, which
    *  is zero. The symptom is "Task N aborted: Page Fault, eip 0x00000000" and
    *  it names this task while pointing at nothing.
    *
    *  Every other kernel task in this tree sidesteps the question by looping
    *  forever and never ending at all. This one does end, so it says so and
    *  then stops, in the only way a task that is no longer runnable can:
    *  halting until the scheduler takes the CPU away for good. Interrupts are
    *  on here -- task_wait() restored them and nothing since has touched them
    *  -- so the halt is left by the very next timer tick. */
    for(;;)
        __asm__ __volatile__ ("hlt");
}

/* --- Starting the task, which is later than creating it ------------------ */

/* WHY THE TASK IS NOT SIMPLY STARTED WHERE IT IS CREATED. This cost a boot
*  that stopped dead one line after the USB devices were listed, and net.c
*  carries the same trap and the same answer above net_drain_start().
*
*  usbhid_init() may be called from the boot path -- kernel.c's network_init()
*  is the obvious place for it, right after usb_init() -- and THE BOOT PATH IS
*  NOT A TASK. It runs on the boot stack, and schedule() saves the context it
*  interrupted only when there is a current task to save it into. Until the
*  first switch there is none, so the first timer tick after ANY task becomes
*  runnable elects that task and throws the boot away: everything kernel.c
*  still had to do, the console task included, never happens. The machine ends
*  up with a perfectly working USB keyboard and nothing to type into.
*
*  Creating the task is free -- taskmgr_add_task() leaves the slot suspended
*  and the scheduler cannot elect what it cannot see. Only the start has to
*  wait, and what it waits for is the scheduler being demonstrably up already:
*  taskmgr_get_currpid() returns -1 until it has elected somebody, and the
*  somebody it elects first is the console task, since that is the first task
*  the kernel creates.
*
*  This is safe from interrupt context, which is where the deferred version
*  runs from: taskmgr_task_start() only stores a state, exactly as task_wake()
*  does, and the pid was validated when the task was created so none of its
*  complaining paths can be reached. */
static void usbhid_start_handler(struct regs *r);

static void usbhid_start_task(void)
{
    if(usbhid_task_running || usbhid_task_pid < 0)
        return;

    if(taskmgr_get_currpid() < 0)
        return;

    usbhid_task_running = 1;

    /* Removed before the start rather than after, so this cannot run twice if
    *  a tick lands between the two. timer_notify_handlers() reads the slot
    *  into a local before calling, so clearing it from inside the call is
    *  exactly as safe as clearing it from anywhere else. */
    timer_uninstall_handler(usbhid_start_handler);

    taskmgr_task_start(usbhid_task_pid);
}

static void usbhid_start_handler(struct regs *r)
{
    (void)r;
    usbhid_start_task();
}

/* --- Bringing it up ------------------------------------------------------ */

/* Looks over what usb.c enumerated, claims every boot keyboard and mouse, and
*  starts the one task that polls all of them.
*
*  Call it once, after usb_init(). Either from the boot path, next to
*  usb_init() itself, or from a task -- both work, and the difference between
*  them is entirely inside usbhid_start_task(), which is where it is explained.
*
*  Safe on a machine with no USB controller and on one with a controller and
*  nothing plugged in: both leave the interface count at zero and start no
*  task, and every accessor below then answers the way it would for a machine
*  that has none.
*
*  Nothing is printed. Whether a device was found is a question for the shell,
*  which has usbhid_count() and usbhid_describe() to answer it with, and a
*  driver that writes to the console during boot competes with the code that is
*  trying to lay the console out. */
void usbhid_init(void)
{
    const usb_device *dev;
    int               count;
    int               i;

    if(usbhid_started)
        return;

    usbhid_started = 1;

    if(!usb_present())
        return;

    count = usb_device_count();

    for(i = 0; i < count; i++)
    {
        dev = usb_device_get(i);
        if(dev == 0)
            continue;

        /* The const is dropped deliberately and it is not a trick. usb.h hands
        *  out a read-only view so that nothing browsing the table can damage
        *  it, but the objects behind it are ordinary mutable statics in usb.c
        *  and a class driver is precisely the caller that is entitled to write
        *  to one: usb_control() takes a non-const device, and claiming an
        *  interface means writing the driver name into it. There is no
        *  usb_claim() to do it properly, and adding one would mean editing
        *  usb.c, which this file does not. */
        usbhid_claim((usb_device *)dev);
    }

    if(usbhid_iface_count == 0)
        return;

    /* TASK_PRIORITY_LOW, for the same reason net.c gives its receive task the
    *  same: priority here is the LENGTH of a slice and not a right to run
    *  first, the scheduler is round robin over everything runnable, and this
    *  task spends nearly all of its slice blocked inside the controller
    *  anyway. A longer slice would lengthen the cycle for every other task and
    *  buy this one nothing. */
    usbhid_task_pid = taskmgr_add_task((void *)usbhid_task, "USB HID",
                                       TASK_PRIORITY_LOW);
    if(usbhid_task_pid < 0)
        return;

    /* Started here when this was called from a task, and from the next timer
    *  tick after the scheduler is up when it was called from the boot path.
    *  See usbhid_start_task() for why the second case cannot be the first.
    *
    *  A full handler table -- sixteen slots, and nothing else in this kernel
    *  installs one -- would leave the task suspended forever. It is not worth
    *  a fallback: the fallback that suggests itself, starting it anyway, is
    *  precisely the hang this exists to avoid. */
    usbhid_start_task();

    if(!usbhid_task_running)
        timer_install_handler(usbhid_start_handler);
}

/* --- What the shell can ask ---------------------------------------------- */

/* Whether anything is being polled right now. An interface that was claimed
*  and has since been unplugged does not count, which is the useful reading:
*  the question is "is there a USB keyboard working", not "was there one at
*  boot". */
int usbhid_present(void)
{
    int i;

    for(i = 0; i < usbhid_iface_count; i++)
    {
        if(usbhid_ifaces[i].live)
            return 1;
    }

    return 0;
}

/* How many interfaces were claimed, gone ones included, because that is the
*  number usbhid_describe() indexes and a device that vanished is worth a line
*  saying so. */
int usbhid_count(void)
{
    return usbhid_iface_count;
}

/* One line about one claimed interface, for example
*
*      keyboard, port 0, ep 1, 10 ms
*      mouse, port 1, ep 1, 10 ms (removed)
*
*  Never null, so a caller can print it without checking; an index that names
*  nothing reads as such rather than as an empty line. */
const char *usbhid_describe(int index)
{
    static char text[USBHID_TEXT + 12];
    int         at;

    if(index < 0 || index >= usbhid_iface_count)
        return "no such interface";

    at = usbhid_put(text, 0, (int)sizeof(text), usbhid_ifaces[index].text);

    if(!usbhid_ifaces[index].live)
        usbhid_put(text, at, (int)sizeof(text), " (removed)");

    return text;
}

/* Counters, for a shell that wants to show whether anything is actually
*  arriving. reports is every report that came off the wire, keys is characters
*  handed to kb_inject() -- including repeats, so it is larger than the number
*  of presses -- moves is mouse reports delivered, and errors is failed
*  transfers, which is the one to look at when a device stopped working: it
*  climbing while reports stands still is a device that is gone or an endpoint
*  that is halted. NAKs are in none of them, because a NAK is not an event. */
uint32_t usbhid_reports(void)
{
    return usbhid_stat_reports;
}

uint32_t usbhid_keys(void)
{
    return usbhid_stat_keys;
}

uint32_t usbhid_moves(void)
{
    return usbhid_stat_moves;
}

uint32_t usbhid_errors(void)
{
    return usbhid_stat_errors;
}
