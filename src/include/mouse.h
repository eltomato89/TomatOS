/* TomatOS - PS/2 mouse
*  Desc: Where the pointer is, and which buttons are down.
*
*  The mouse hangs off the same 8042 controller as the keyboard, on its
*  auxiliary port and IRQ 12. That is worth knowing for two reasons: the
*  controller has to be told to enable that port at all, and every command
*  meant for the mouse has to be prefixed so the controller forwards it
*  instead of answering it itself.
*
*  A packet is three bytes -- flags, then dx, then dy -- and three details in
*  it are the ones that get implementations wrong:
*
*    - The stream can DESYNCHRONISE. There is no framing, so a dropped or
*      spurious byte shifts every packet afterwards and the pointer flies off.
*      Bit 3 of the first byte is always set, which is the only anchor
*      available for finding the boundary again.
*    - dx and dy are 9 bit signed values, with the sign living in the flags
*      byte rather than in the data byte. Treating them as plain signed chars
*      works until a fast movement, and then the pointer jumps backwards.
*    - Y COUNTS UP, and screens count down. Every mouse driver inverts it
*      somewhere; doing it in one place is the difference between a pointer
*      that works and one that works in half the code.
*
*  THIS INTERFACE IS DELIBERATELY NOT ABOUT PS/2. Nothing above it should
*  learn where the events came from: the plan is that a USB HID driver later
*  fills the same queue, and everything built on top -- a GUI, a shell command
*  -- keeps working without knowing that anything changed. So there is nothing
*  here about scancodes, ports or packet formats, and nothing here should gain
*  any.
*/
#ifndef __MOUSE_H
#define __MOUSE_H

#include "typedefs.h"

#define MOUSE_BUTTON_LEFT    0x01
#define MOUSE_BUTTON_RIGHT   0x02
#define MOUSE_BUTTON_MIDDLE  0x04

/* How many events are held before the oldest is dropped. A pointer that moves
*  across the screen produces a packet every few milliseconds, so this only has
*  to bridge the gap between two turns of whoever is reading -- it is not a
*  history and nothing should treat it as one. */
#define MOUSE_QUEUE_SIZE  64

typedef struct
{
    int16_t  x;         /* position after this event, inside the bounds     */
    int16_t  y;
    int16_t  dx;        /* movement this event carried, sign as on screen   */
    int16_t  dy;
    uint8_t  buttons;   /* which are down now, MOUSE_BUTTON_*               */
    uint8_t  changed;   /* which changed with this event -- 0 for a move    */
    uint32_t time_ms;   /* uptime when the packet arrived                   */
} mouse_event;

/* Brings the auxiliary port up and installs the interrupt handler. Returns 0,
*  or negative when there is no mouse -- which is an ordinary outcome, not a
*  failure: a machine may have none, and everything below then reports a
*  pointer that never moves rather than refusing to work. */
extern int mouse_init(void);
extern int mouse_present(void);

/* Where the pointer is now, and what is held down. Cheap, and safe to call at
*  any time -- these are the natural things for a shell command to print and
*  for a GUI to draw a cursor at. */
extern int mouse_x(void);
extern int mouse_y(void);
extern int mouse_buttons(void);

/* The area the pointer may move in, in pixels, and where it starts. Set this
*  to the screen the pointer is being drawn on; without it the driver has no
*  idea how far right is, and a pointer that can leave the screen is one the
*  user cannot get back. Both are clamped, so a caller cannot place the pointer
*  outside its own bounds. */
extern void mouse_set_bounds(int width, int height);
extern void mouse_set_position(int x, int y);

/* Takes the next event, or returns 0 when there is none. Never blocks.
*
*  Reading the position instead is enough for something that only wants to know
*  where the pointer is. The queue is for anything that has to see WHAT
*  HAPPENED rather than where things ended up -- a click is an event, and a
*  press and release between two looks at mouse_buttons() is invisible. */
extern int mouse_poll(mouse_event *out);

/* The channel to block on with task_wait() while there is nothing to read.
*  Woken from the interrupt for every packet that produced an event, so the
*  usual rule applies: a wake means "look again", and the caller re-tests. */
extern const void *mouse_wait_channel(void);

/* Counters, for a shell that wants to show whether the hardware is actually
*  saying anything.
*
*  packets counts what arrived from WHATEVER pointer is in use, PS/2 or
*  injected, because a count that stayed at zero while the pointer visibly
*  moved reads as a broken driver. The other three are properties of an
*  unframed byte stream and stay PS/2 only: resyncs is the interesting one --
*  a stream that keeps losing its framing points at the controller or a lost
*  interrupt, and a pointer that jumps is otherwise very hard to tell from a
*  driver bug. USB has framing and cannot lose it, so those stay at zero
*  there, which is the truth rather than a gap. */
extern uint32_t mouse_packets(void);
extern uint32_t mouse_resyncs(void);
extern uint32_t mouse_overflows(void);
extern uint32_t mouse_dropped(void);

/* What the device turned out to be, for "mouse" to print: whether the wheel
*  was negotiated, how many buttons it reports, and which bus it is on. A short
*  phrase, never null. */
extern const char *mouse_describe(void);

/* Delivers movement and buttons from a pointing device that is not the one on
*  the 8042 -- today a USB HID mouse. This is what the promise at the top of
*  this file cashes in: nothing above learns which bus the pointer is on.
*
*  dx and dy are in SCREEN orientation: positive y is down. That is the same
*  direction a HID boot mouse already reports and the OPPOSITE of what a PS/2
*  mouse sends, which counts up. The inversion belongs to whichever driver
*  needs it, and doing it here as well would invert twice for one of them --
*  which is a pointer that works and a pointer that moves the wrong way, from
*  code that looks identical.
*
*  buttons is the full current state, MOUSE_BUTTON_*, not a change: what
*  changed is worked out here, so a driver that only knows which buttons are
*  down right now -- which is all a HID report says -- does not have to
*  remember the previous one.
*
*  Safe from interrupt context, and from a task; a USB driver that polls will
*  be in the latter. name is what mouse_describe() should say afterwards, or 0
*  to leave it alone. */
extern void mouse_inject(int dx, int dy, uint8_t buttons, const char *name);

#endif
