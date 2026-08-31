/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: PS/2 mouse driver -- the device half of <mouse.h>
*
*  Notes: No warranty expressed or implied. Use at own risk.
*
*  mouse.h is deliberately silent about buses, ports and packet formats, and
*  this file is the only place in the kernel that knows about any of them. The
*  bargain is that a USB HID driver can later fill the same queue and move the
*  same pointer, and nothing above notices; so nothing below is exported, and
*  no PS/2 detail leaks out through a header. The one place the bus is named
*  outward is mouse_describe(), which exists precisely so a concrete driver can
*  say what it turned out to be for a human to read -- a USB driver would
*  answer "USB HID mouse" there and the contract would be unchanged.
*
*  The mouse shares the 8042 with the keyboard (src/kb.c), and the two drivers
*  overlap in exactly two places: port 0x60, and the controller's configuration
*  byte. Both are handled here rather than shared, because there is nothing to
*  share -- kb.c reads a byte when IRQ 1 says so, and this file never touches
*  anything the keyboard depends on without putting it back.
*
*  There is no printing anywhere in this file, not even on the error paths.
*  mouse_init() reports through its return value because it may run before a
*  console exists, and the interrupt handler must not print because a pointer
*  moving across the screen produces a hundred packets a second and would bury
*  the console under itself within a second of the first movement.
*/

#include <system.h>
#include <mouse.h>

/* --- The 8042 controller ---------------------------------------------------
*
*  Two ports and one rule: NEVER touch either of them without reading the
*  status register first. Writing while the input buffer is still full loses
*  the byte, and reading while the output buffer is empty hands back the last
*  byte a second time. Both produce a driver that works on the machine it was
*  written on and nowhere else, because whether the buffer happens to be clear
*  depends on how fast the controller is relative to the CPU.
*/
#define PS2_DATA               0x60
#define PS2_STATUS             0x64    /* read  */
#define PS2_COMMAND            0x64    /* write */

#define PS2_STAT_OBF           0x01    /* output buffer full: a byte is there */
#define PS2_STAT_IBF           0x02    /* input buffer full: do not write yet */
#define PS2_STAT_AUX           0x20    /* the byte came from the AUX port     */

#define PS2_CMD_READ_CONFIG    0x20
#define PS2_CMD_WRITE_CONFIG   0x60
#define PS2_CMD_AUX_DISABLE    0xA7
#define PS2_CMD_AUX_ENABLE     0xA8
#define PS2_CMD_TO_AUX         0xD4    /* forward the NEXT data byte to the
                                        * mouse instead of executing it       */

#define PS2_CFG_KBD_IRQ        0x01
#define PS2_CFG_AUX_IRQ        0x02    /* IRQ 12 is generated at all          */
#define PS2_CFG_KBD_CLOCK_OFF  0x10    /* set = keyboard port disabled        */
#define PS2_CFG_AUX_CLOCK_OFF  0x20    /* set = auxiliary port disabled       */

/* --- Device commands, and the answers they give --------------------------- */
#define MOUSE_CMD_RESET        0xFF
#define MOUSE_CMD_SET_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE       0xF4

#define MOUSE_REPLY_ACK        0xFA
#define MOUSE_REPLY_RESEND     0xFE
#define MOUSE_REPLY_SELFTEST   0xAA

/* Device ids, as reported by the third byte of the reset reply. Only 0x00 can
*  turn up here: a reset always puts an IntelliMouse back into three byte mode,
*  which is one of the reasons the reset is done at all rather than trusting
*  whatever state the BIOS left the device in. The others are listed so an
*  unexpected one is named rather than reported as "unknown". */
#define MOUSE_ID_STANDARD      0x00
#define MOUSE_ID_WHEEL         0x03
#define MOUSE_ID_5BUTTON       0x04

/* --- The packet ------------------------------------------------------------
*
*  Three bytes: flags, dx, dy. The flags byte carries the buttons, the two
*  sign bits, the two overflow bits, and one bit that is always set.
*/
#define PKT_LEFT               0x01
#define PKT_RIGHT              0x02
#define PKT_MIDDLE             0x04
#define PKT_ALWAYS_ONE         0x08    /* the only framing there is           */
#define PKT_X_SIGN             0x10
#define PKT_Y_SIGN             0x20
#define PKT_X_OVERFLOW         0x40
#define PKT_Y_OVERFLOW         0x80

/* --- Timeouts --------------------------------------------------------------
*
*  EVERY wait is bounded, and the reason is not robustness in the abstract: a
*  machine with no mouse is the ordinary case, not the exceptional one, and on
*  such a machine every one of these waits runs to its end. An unbounded wait
*  anywhere in the bring-up is a kernel that boots on the developer's machine
*  and hangs on a server.
*
*  Each wait is bounded TWICE, because neither bound alone is enough:
*
*    - In milliseconds, off timer_get_ticks(). That is the bound that means
*      something, and it is the same on a 486 and on a modern CPU.
*    - In iterations. mouse_init() may run before timer_install(), and it does
*      run with the scheduler live; if the clock is not moving yet then the
*      millisecond bound never expires and the loop is the hang it was supposed
*      to prevent. One iteration is one read of the status port, which is an
*      ISA cycle of roughly a microsecond however fast the CPU is, so a
*      thousand iterations per millisecond is the right order of magnitude and
*      is generous rather than tight -- it is a backstop, not a schedule.
*/
#define PS2_SPINS_PER_MS       1000UL
#define PS2_WAIT_MS            250     /* ordinary command turnaround         */
#define PS2_RESET_MS           800     /* the self test is the slow one       */

/* How long a packet may take to arrive in full before what has been collected
*  so far is treated as debris rather than as the beginning of a packet. At the
*  default sample rate of 100 Hz the three bytes of one packet arrive within a
*  fraction of a millisecond of each other, and the next packet is 10 ms away,
*  so anything approaching this gap means a byte was lost. See the resync
*  discussion at mouse_handler(). */
#define MOUSE_PACKET_GAP_MS    50

/* Where the pointer may go before anything has said how big the screen is.
*
*  There is no right answer available at init time: the driver comes up before
*  anyone has decided what the pointer will be drawn on, and mouse.h makes
*  mouse_set_bounds() the way to say so. This is the standard VGA graphics
*  resolution, which is the mode this kernel draws in, and it is a placeholder
*  -- the point of it is only that the pointer is inside SOME rectangle from
*  the first packet onwards, so that no caller ever sees a coordinate it cannot
*  explain. */
#define MOUSE_DEFAULT_WIDTH    640
#define MOUSE_DEFAULT_HEIGHT   480

/* int16_t is what mouse.h stores a coordinate in, so no rectangle may be
*  larger than what fits in one. */
#define MOUSE_MAX_BOUND        32767

/* IRQ 12 and the slave PIC. */
#define MOUSE_IRQ              12
#define PIC_MASTER_MASK        0x21
#define PIC_SLAVE_MASK         0xA1
#define PIC_CASCADE_IRQ        2

/* EFLAGS.IF, as in kb.c. */
#define MOUSE_EFLAGS_IF        0x200

/* --- State -----------------------------------------------------------------
*
*  Split by who writes it, which is the whole of the concurrency argument in
*  this file:
*
*      mouse_present_flag  mouse_init() only, before the handler exists.
*      cur_x, cur_y        the interrupt, and the two setters below with
*                          interrupts off.
*      cur_buttons         the interrupt only.
*      bound_w, bound_h    mouse_set_bounds() only, with interrupts off.
*      pkt, pkt_phase      the interrupt only.
*      the counters        the interrupt only.
*      mq_head             the interrupt only.
*      mq_tail             mouse_poll() only.
*/
static int             mouse_present_flag = 0;
static unsigned char   mouse_device_id    = MOUSE_ID_STANDARD;

static volatile int16_t cur_x = 0;
static volatile int16_t cur_y = 0;
static volatile uint8_t cur_buttons = 0;

static int bound_w = MOUSE_DEFAULT_WIDTH;
static int bound_h = MOUSE_DEFAULT_HEIGHT;

static unsigned char pkt[3];
static int           pkt_phase = 0;
static unsigned int  pkt_last_ms = 0;

static volatile uint32_t stat_packets   = 0;
static volatile uint32_t stat_resyncs   = 0;
static volatile uint32_t stat_overflows = 0;
static volatile uint32_t stat_dropped   = 0;

/* --- The event queue -------------------------------------------------------
*
*  The same producer/consumer situation as net.c's receive ring, and the same
*  shape: free running indices that are never reduced modulo anything, one
*  context writing each, occupancy as head - tail in unsigned arithmetic so
*  that the wrap needs no special case and no slot has to be sacrificed to
*  telling "full" from "empty" apart. The position of an entry is index &
*  MOUSE_QUEUE_MASK, which is why the size has to be a power of two, and the
*  typedef below is there so that a future edit of mouse.h that makes it 50
*  fails to compile rather than quietly indexing off the end.
*
*  Entries are fixed size here, where net.c needed a byte ring with length
*  prefixed records: a mouse_event is twelve bytes whether it carries a click
*  or a movement, so there is no padding to save and nothing a byte ring would
*  buy except the split-record code to go with it.
*
*  WHERE THIS DIFFERS FROM net.c, and why. net.c takes care never to mask
*  interrupts on either side, because its consumer runs once per frame on a
*  saturated link and its producer copies 1518 bytes; a cli there is measurable
*  and the lock-free argument is worth its complexity. Here the consumer copies
*  twelve bytes at most a hundred times a second, so masking costs nothing
*  measurable at all -- and it buys something the lock-free version cannot
*  give: net.c has to REFUSE a second concurrent drainer, because two readers
*  would both advance the tail. mouse.h invites two readers by design; drawing
*  a cursor and a shell command that reports clicks are exactly the two things
*  it names, and they are separate tasks. So mouse_poll() takes the event with
*  interrupts off, which on this uniprocessor also excludes the scheduler --
*  the only thing that could hand the tail to a second task is the timer
*  interrupt -- and two pollers then interleave safely instead of corrupting
*  the queue. The producer masks nothing: it is already in interrupt context.
*
*  The single-writer discipline and the publication order are kept regardless,
*  because they are what makes the PRODUCER safe: the entry is filled in first,
*  and only then does mq_head move, with a compiler barrier in between, so a
*  half written event is never visible. The barrier is against the compiler
*  alone -- producer and consumer are the same CPU with an interrupt in
*  between, and an interrupt is a serialising event.
*
*  OVERFLOW: the NEWEST event is dropped and mouse_dropped() counts it.
*
*  That is net.c's policy, and it is adopted for net.c's structural reason --
*  dropping the oldest instead would mean the interrupt advancing mq_tail,
*  which belongs to the consumer and may be in use by a consumer that was
*  preempted halfway through reading the very entry being reclaimed. It would
*  buy a nicer loss policy at the price of the entire argument above.
*
*  But the reason it is ACCEPTABLE here is not net.c's reason, and that is
*  worth being clear about, because for a pointer the newest events are
*  ordinarily the interesting ones and keeping sixty-four stale ones would be
*  the wrong trade. What rescues it is that the position and the button mask do
*  NOT live in the queue: mouse_x(), mouse_y() and mouse_buttons() are updated
*  by the interrupt on every packet and are current no matter how full the
*  queue is. So a reader that falls behind loses the intermediate steps of a
*  movement, never where the pointer ended up -- it can always recover the
*  truth by asking. A network stack has no equivalent recovery for a frame it
*  never saw, which is why net.c had to argue the point from what TCP does with
*  a loss. Sixty-four events is two thirds of a second of continuous movement
*  at the default sample rate; a reader further behind than that has a problem
*  mouse_dropped() is there to show.
*/
#define MOUSE_QUEUE_MASK  (MOUSE_QUEUE_SIZE - 1)

typedef char mouse_queue_size_must_be_a_power_of_two[
    ((MOUSE_QUEUE_SIZE & MOUSE_QUEUE_MASK) == 0 && MOUSE_QUEUE_SIZE > 0) ? 1 : -1];

static volatile mouse_event mq[MOUSE_QUEUE_SIZE];
static volatile uint32_t    mq_head = 0;
static volatile uint32_t    mq_tail = 0;

/* The channel a task blocks on while the queue is empty.
*
*  The address of the producer's index, in kb.c's idiom: the thing that is
*  waited for is the arrival of an event, and mq_head moving IS that arrival.
*  Nothing is ever read through the pointer. The cast is only about dropping
*  the volatile qualifier, which task_wake() does not take and which would
*  otherwise be a warning about a discarded qualifier rather than a real
*  conversion. */
#define MOUSE_WAIT_CHANNEL ((const void *)&mq_head)

/* --- Interrupt state, as in kb.c and net.c --------------------------------
*
*  Saving the flags and clearing IF is the only form that is safe in a caller
*  that may already have interrupts off: a plain enable() at the end would hand
*  them back on to somebody who had deliberately switched them off. Both files
*  have a private pair of these and neither exports one; a handful of lines is
*  cheaper than a new cross-file interface for six instructions. */
static unsigned long mouse_irq_save(void)
{
    unsigned long flags;

    __asm__ __volatile__ ("pushfl; popl %0; cli"
                          : "=r" (flags) : : "memory");
    return flags;
}

static void mouse_irq_restore(unsigned long flags)
{
    __asm__ __volatile__ ("pushl %0; popfl"
                          : : "r" (flags) : "memory", "cc");
}

/* --- Bounded access to the controller ------------------------------------ */

/* Waits for one status bit to reach the wanted state. Returns 1 when it did,
*  0 on timeout. See the note on the timeout constants for why there are two
*  bounds and not one. */
static int ps2_wait_status(unsigned char mask, int want_set, int ms)
{
    unsigned long spins;
    unsigned long limit;
    unsigned int  start;
    unsigned char st;

    limit = (unsigned long)ms * PS2_SPINS_PER_MS;
    start = (unsigned int)timer_get_ticks();

    for(spins = 0; spins < limit; spins++)
    {
        st = inportb(PS2_STATUS);

        if(want_set)
        {
            if((st & mask) != 0)
                return 1;
        }
        else
        {
            if((st & mask) == 0)
                return 1;
        }

        /* The clock is consulted only every 256th turn. timer_get_ticks() is a
        *  division and a modulo, and this loop is the hot path of every single
        *  controller access; at roughly a microsecond per turn the granularity
        *  that costs is a quarter of a millisecond on a wait measured in
        *  hundreds. The difference is taken in unsigned arithmetic so that the
        *  tick counter wrapping does not turn a bound into an eternity. */
        if((spins & 0xFF) == 0)
        {
            if(((unsigned int)timer_get_ticks() - start) > (unsigned int)ms)
                return 0;
        }
    }

    return 0;
}

/* A command byte to the controller itself. */
static int ps2_write_command(unsigned char cmd)
{
    if(!ps2_wait_status(PS2_STAT_IBF, 0, PS2_WAIT_MS))
        return 0;

    outportb(PS2_COMMAND, cmd);
    return 1;
}

/* A data byte to the controller -- the operand of a command, or the byte that
*  a preceding 0xD4 will have forwarded to the mouse. */
static int ps2_write_data(unsigned char data)
{
    if(!ps2_wait_status(PS2_STAT_IBF, 0, PS2_WAIT_MS))
        return 0;

    outportb(PS2_DATA, data);
    return 1;
}

static int ps2_read_data(unsigned char *out, int ms)
{
    if(!ps2_wait_status(PS2_STAT_OBF, 1, ms))
        return 0;

    *out = inportb(PS2_DATA);
    return 1;
}

/* Empties the output buffer.
*
*  Called before the bring-up begins and again after it ends, and the second
*  one is the one that matters. A byte left in the buffer by an unread reply
*  does not stay a reply: it is handed to the interrupt handler as the FIRST
*  BYTE OF A PACKET, and from that moment every packet is one byte out of step
*  and the pointer flies off on the first movement. The bound is there because
*  a controller that answers every read is a controller this loop would never
*  leave; thirty-two bytes is far more than any reply this file provokes. */
static void ps2_flush(void)
{
    int i;

    for(i = 0; i < 32; i++)
    {
        if((inportb(PS2_STATUS) & PS2_STAT_OBF) == 0)
            return;

        (void)inportb(PS2_DATA);
    }
}

/* Sends one command to the MOUSE and collects its acknowledgement.
*
*  Two things are going on. The 0xD4 prefix is what makes the controller
*  forward the next data byte to the auxiliary port instead of executing it
*  itself -- without it, 0xF4 is not "enable reporting" but whatever the 8042
*  makes of a command byte 0xF4.
*
*  And the mouse answers every command with 0xFA. Reading that answer is not
*  politeness, it is how this file finds out there is a mouse at all -- an
*  empty auxiliary port answers nothing, and the timeout is the detection. Not
*  reading it is worse than not knowing: the byte stays in the output buffer
*  and turns up later as the first byte of a packet.
*
*  0xFE is "resend", which a device sends when it did not understand the byte
*  on the wire. One retry, because the second failure is a real one.
*
*  Anything else that turns up ahead of the acknowledgement is a leftover from
*  before -- a reply nobody collected, a byte the BIOS provoked -- and it is
*  discarded rather than treated as a refusal. That distinction matters: a
*  driver that gives up on the first unexpected byte reports "no mouse" on a
*  machine that has one and merely had something stale in its buffer, and no
*  amount of flushing beforehand can rule that out, because the stale byte may
*  arrive after the flush. The search is bounded so that a device answering
*  nothing but noise still ends in the timeout rather than in a loop; the
*  bound is small because there is nothing legitimate to skip past. Every
*  command this file sends goes out before 0xF4, so no real packet can be in
*  the buffer to be mistaken for garbage and thrown away. */
static int mouse_command(unsigned char cmd)
{
    unsigned char reply;
    int           attempt;
    int           skipped;

    for(attempt = 0; attempt < 2; attempt++)
    {
        if(!ps2_write_command(PS2_CMD_TO_AUX))
            return 0;

        if(!ps2_write_data(cmd))
            return 0;

        for(skipped = 0; skipped < 4; skipped++)
        {
            if(!ps2_read_data(&reply, PS2_WAIT_MS))
                return 0;

            if(reply == MOUSE_REPLY_ACK)
                return 1;

            if(reply == MOUSE_REPLY_RESEND)
                break;
        }
    }

    return 0;
}

/* --- The interrupt controller ---------------------------------------------
*
*  IRQ 12 is on the SLAVE PIC, so the same path rtl8139.c needed for IRQ 11:
*  the mask bit lives at 0xA1 and counts from 8, and the slave only reaches the
*  CPU through the cascade on IRQ 2, which has to be open in the MASTER mask as
*  well. irq_remap() in irq.c opens both masks completely, so in the normal
*  case this changes nothing; doing it anyway costs four port accesses and
*  makes the driver independent of that. The end of interrupt for the slave is
*  already sent by irq_handler() for every vector from 40 upwards, and IRQ 12
*  is vector 44, so the handler below has nothing to acknowledge at the PIC.
*
*  What IRQ 12 needs BEYOND what IRQ 11 needed is all on the other side of the
*  line, and it is the larger half:
*
*    - The line does not exist until the 8042 is told to make it. The RTL8139
*      is wired to its line by the PCI bus and only has to be told to raise it;
*      the auxiliary port is disabled at the controller (0xA8 turns it on) AND
*      its interrupt is separately gated by a bit in the controller's
*      configuration byte, AND the device itself will not send anything until
*      0xF4. Three switches, in three different places, all of which default to
*      off.
*    - The line is SHARED WITH THE KEYBOARD in the sense that matters: both
*      devices deliver through port 0x60. IRQ 11 could read its card's
*      registers and be sure they were its own. The handler here cannot, and
*      has to ask the status register which port a byte came from before
*      consuming it -- see mouse_handler().
*/
static void mouse_unmask_irq(void)
{
    unsigned char mask;

    mask = inportb(PIC_SLAVE_MASK);
    outportb(PIC_SLAVE_MASK, (unsigned char)(mask & ~(1 << (MOUSE_IRQ - 8))));

    mask = inportb(PIC_MASTER_MASK);
    outportb(PIC_MASTER_MASK, (unsigned char)(mask & ~(1 << PIC_CASCADE_IRQ)));
}

/* --- Keeping the keyboard out of the bring-up ------------------------------
*
*  This is not a precaution, it is a fix for a failure that was reproduced
*  before it was written: without it the bring-up gives up at its second step
*  and reports no mouse on a machine that has one.
*
*  The reason is the shared buffer, seen from the other side. The controller
*  answers "read configuration byte" by putting the byte in the output buffer
*  -- and it is a byte from the KEYBOARD side of the controller, so the
*  controller raises IRQ 1 for it. kb.c's handler is installed by then and does
*  exactly what a keyboard handler should: it reads port 0x60. The
*  configuration byte is gone before the poll below ever looks, the wait runs
*  to its timeout, and the driver concludes there is no mouse.
*
*  Everything from the configuration byte onwards is protected by the
*  configuration byte itself, which is written with the keyboard's clock and
*  interrupt switched off. The read that fetches it cannot be, because it is
*  what the write is built from -- so that one read needs the interrupt held
*  off at the PIC instead.
*
*  Masking the line rather than clearing IF is the narrower instrument and the
*  better one: the timer keeps running, so the millisecond half of every bound
*  in this file stays meaningful, and the scheduler is not held off for what
*  can be the better part of a second on a machine with nothing attached. The
*  only interrupt handlers in this kernel that touch port 0x60 are IRQ 1's and
*  this file's own, and this file's is not installed yet.
*
*  Only bit 1 is put back, and deliberately not the whole byte:
*  mouse_unmask_irq() clears bit 2 in the same register in between, and
*  restoring a snapshot taken before that would undo it. */
static unsigned char mouse_hold_keyboard_irq(void)
{
    unsigned char mask;

    mask = inportb(PIC_MASTER_MASK);
    outportb(PIC_MASTER_MASK, (unsigned char)(mask | (1 << 1)));
    return mask;
}

static void mouse_release_keyboard_irq(unsigned char saved)
{
    unsigned char mask;

    mask = inportb(PIC_MASTER_MASK);
    mask = (unsigned char)((mask & ~(1 << 1)) | (saved & (1 << 1)));
    outportb(PIC_MASTER_MASK, mask);
}

/* --- Position ------------------------------------------------------------ */

static int mouse_clamp(int v, int limit)
{
    if(v < 0)
        return 0;
    if(v > limit - 1)
        return limit - 1;
    return v;
}

/* --- Turning a packet into an event ---------------------------------------
*
*  Called from the interrupt with three bytes that have passed the framing
*  check. Everything the header warns about is here.
*/
/* Everything that happens once a movement and a button state are known,
*  wherever they came from. Split out of mouse_process_packet() so that
*  mouse_inject() -- a USB HID mouse, today -- runs the identical code rather
*  than a second copy of it that drifts. The one thing the callers must agree
*  on before getting here is the SIGN of dy: screen orientation, positive
*  down, per mouse.h. A PS/2 packet has to be flipped first; a HID report does
*  not. Doing it here would be doing it twice for one of them.
*
*  Runs with interrupts off when the PS/2 handler calls it, and in task
*  context when a polled USB driver does. It touches the queue's producer end
*  and the position, which is the interrupt's half of the split -- so a task
*  side caller has to hold the guard, which mouse_inject() does. */
static void mouse_deliver(int dx, int dy, uint8_t buttons, unsigned int now_ms)
{
    uint8_t  changed;
    int      nx;
    int      ny;
    uint32_t head;
    uint32_t tail;
    uint32_t slot;

    /* CLAMPED ON EVERY UPDATE, not only when someone asks. A pointer that is
    *  allowed off its rectangle is a pointer the user cannot get back: the
    *  counts that took it out there have to be undone one for one before it
    *  reappears, and there is nothing on screen during that to say which way
    *  to move. */
    nx = mouse_clamp((int)cur_x + dx, bound_w);
    ny = mouse_clamp((int)cur_y + dy, bound_h);

    changed = (uint8_t)(buttons ^ cur_buttons);

    cur_x       = (int16_t)nx;
    cur_y       = (int16_t)ny;
    cur_buttons = buttons;

    /* A packet that moved nothing and changed nothing produces no event. The
    *  device only sends when something happened, so this is rare -- an
    *  overflow packet with no button change is the usual way to get here --
    *  but an event that says nothing would still cost a queue slot and a walk
    *  over the task table to wake somebody who then finds nothing to do. */
    if(dx == 0 && dy == 0 && changed == 0)
        return;

    head = mq_head;
    tail = mq_tail;

    if((uint32_t)(head - tail) >= (uint32_t)MOUSE_QUEUE_SIZE)
    {
        stat_dropped++;
        return;
    }

    slot = head & MOUSE_QUEUE_MASK;

    /* dx and dy are the movement the DEVICE reported, with the Y inversion
    *  applied and nothing else -- not the difference between the old and new
    *  positions. The two differ only at an edge, where the position stopped
    *  and the hand did not, and there the reported delta is the one piece of
    *  information a caller cannot reconstruct: x and y already say where the
    *  pointer ended up, so a delta that merely repeats that would carry
    *  nothing. Anything that wants the on-screen step has it as the difference
    *  between two consecutive events' x. */
    mq[slot].x       = (int16_t)nx;
    mq[slot].y       = (int16_t)ny;
    mq[slot].dx      = (int16_t)dx;
    mq[slot].dy      = (int16_t)dy;
    mq[slot].buttons = buttons;
    mq[slot].changed = changed;
    mq[slot].time_ms = (uint32_t)now_ms;

    /* Publication: everything above is in memory the consumer does not look
    *  at, because it only reads below mq_head. The barrier keeps the compiler
    *  from moving the index ahead of the fields. */
    __asm__ __volatile__ ("" : : : "memory");
    mq_head = head + 1;

    /* The whole of what the interrupt owes anybody who is waiting. task_wake()
    *  walks the task table, sets states and returns -- no printing, no drain,
    *  no switch, exactly as in kb.c's keyboard handler. The scheduler picks it
    *  up at the next tick. */
    task_wake(MOUSE_WAIT_CHANNEL);
}

static void mouse_process_packet(unsigned int now_ms)
{
    unsigned char flags;
    uint8_t       buttons;
    int           dx;
    int           dy;

    flags = pkt[0];
    stat_packets++;

    /* The buttons are translated bit by bit rather than masked across.
    *  The three device bits happen to sit in the same order and the same
    *  positions as MOUSE_BUTTON_LEFT/RIGHT/MIDDLE, so "buttons = flags & 7"
    *  would be correct today -- and it would be a PS/2 packet layout smuggled
    *  into a value that mouse.h defines, which is exactly the coupling this
    *  file exists to prevent. Three ifs, and the coincidence stays a
    *  coincidence. */
    buttons = 0;
    if(flags & PKT_LEFT)
        buttons |= MOUSE_BUTTON_LEFT;
    if(flags & PKT_RIGHT)
        buttons |= MOUSE_BUTTON_RIGHT;
    if(flags & PKT_MIDDLE)
        buttons |= MOUSE_BUTTON_MIDDLE;

    if(flags & (PKT_X_OVERFLOW | PKT_Y_OVERFLOW))
    {
        /* OVERFLOW: the movement is thrown away, the buttons are kept.
        *
        *  An overflow bit says the device's counter saturated, so the number
        *  in the data byte is not the movement -- it is the movement modulo
        *  512, with the true magnitude known only to be at least 256. Using it
        *  is not "using the value anyway", it is inventing a number: a
        *  pointer told to move by whatever fell out of the truncation lands
        *  somewhere unrelated to where the hand went, and at the edge of the
        *  screen it lands on the opposite side. Zero is not the truth either,
        *  but it is the one wrong answer that is never surprising -- the
        *  pointer stops for one packet and the next one, ten milliseconds
        *  later, carries on from where the hand actually is.
        *
        *  Dropping the whole packet was the alternative and is rejected for
        *  one reason: the button bits in the same packet did NOT overflow.
        *  They are exact, they are cheap to keep, and a press that is thrown
        *  away is invisible forever -- mouse.h says so itself, a press and
        *  release between two looks at mouse_buttons() cannot be recovered.
        *  Losing a click to salvage a movement that was already lost is a bad
        *  trade in both directions.
        *
        *  In practice this fires almost never on real movement: at the default
        *  100 Hz sample rate it takes more than 255 counts inside ten
        *  milliseconds. When it does fire it usually means the stream is out
        *  of step and a data byte is being read as a flags byte, which is a
        *  further reason not to believe the numbers -- and mouse_overflows()
        *  next to mouse_resyncs() is what makes that visible. */
        stat_overflows++;
        dx = 0;
        dy = 0;
    }
    else
    {
        /* NINE BIT SIGNED, with the sign bit in the flags byte and the low
        *  eight bits in the data byte. Casting the data byte to a signed char
        *  and ignoring the flags is the classic version of this bug: it works
        *  for every small movement and inverts on every fast one, so it
        *  survives testing and fails in use. Subtracting 256 when the sign bit
        *  is set is the sign extension, written so that it is obviously the
        *  sign extension. */
        dx = (int)pkt[1];
        if(flags & PKT_X_SIGN)
            dx -= 256;

        dy = (int)pkt[2];
        if(flags & PKT_Y_SIGN)
            dy -= 256;

        /* Y COUNTS UP AND SCREENS COUNT DOWN. This one line is the whole of
        *  the inversion for the entire kernel, and it is here rather than
        *  anywhere above so that there is exactly one of it: mouse.h promises
        *  that a mouse_event's dy already has the sign it will be drawn with,
        *  and every consumer that had to remember to flip it would be a
        *  consumer that eventually forgets. */
        dy = -dy;
    }

    mouse_deliver(dx, dy, buttons, now_ms);
}

/* --- The interrupt --------------------------------------------------------
*
*  RESYNCHRONISATION. There is no framing in the stream: three bytes arrive,
*  then three more, and nothing says which is which. One lost or one spurious
*  byte shifts every packet from then on, so a flags byte is read as a dy and
*  the pointer flies off and stays off -- the stream never recovers by itself.
*
*  Two things put it back, and both are needed because neither is sufficient:
*
*    - BIT 3 OF THE FIRST BYTE IS ALWAYS SET. That is the only anchor the
*      protocol offers. A byte that arrives where a first byte is expected
*      without that bit cannot be a first byte, so it is debris: it is dropped,
*      counted, and the next byte is tried as the first byte instead. That
*      alone re-frames the stream whenever the byte that lands in the first
*      position happens to lack the bit -- which for random movement data is
*      most of the time, so a shifted stream typically re-frames within a
*      packet or two.
*    - A GAP IN TIME. Bit 3 cannot help when the misaligned byte happens to
*      have it set, and that is not rare: a dy of 8..15 has it. But the three
*      bytes of one packet arrive within a fraction of a millisecond of each
*      other and packets are ten milliseconds apart, so a half-collected packet
*      that has been sitting for MOUSE_PACKET_GAP_MS is not half a packet, it
*      is the remains of one whose third byte was lost. Throwing it away puts
*      the next byte to arrive back in the first position, where the bit 3 test
*      can judge it.
*
*  Both are counted as resyncs, because mouse.h is right that this is the
*  interesting counter: a pointer that jumps is otherwise almost impossible to
*  tell apart from a driver bug, and a resync count that climbs while the mouse
*  is still says the controller or a lost interrupt is the problem.
*
*  WHOSE BYTE IS IT. The keyboard delivers through the same port 0x60, so the
*  status register is asked twice before anything is read: OBF, because reading
*  an empty buffer returns the previous byte again and would inject a duplicate
*  into the middle of a packet; and the AUX bit, because a byte from the
*  keyboard consumed here is a keystroke kb.c will never see AND a byte in the
*  middle of a mouse packet that has no business there. When the AUX bit is
*  clear the byte is left where it is and IRQ 1 collects it.
*
*  THE LOOP. One interrupt normally carries one byte. Draining until the buffer
*  is empty costs one extra status read in that ordinary case and recovers the
*  packet whose interrupt was lost -- which is the event that desynchronises
*  the stream in the first place. The bound is there for the same reason
*  ps2_flush() has one.
*/
static void mouse_handler(struct regs *r)
{
    unsigned char status;
    unsigned char byte;
    unsigned int  now;
    int           guard;

    /* The register frame is not needed: this handler neither inspects the
    *  interrupted context nor switches away from it. */
    (void)r;

    for(guard = 0; guard < 16; guard++)
    {
        status = inportb(PS2_STATUS);

        if((status & PS2_STAT_OBF) == 0)
            return;
        if((status & PS2_STAT_AUX) == 0)
            return;

        byte = inportb(PS2_DATA);
        now  = (unsigned int)timer_get_ticks();

        if(pkt_phase != 0 && (now - pkt_last_ms) > (unsigned int)MOUSE_PACKET_GAP_MS)
        {
            stat_resyncs++;
            pkt_phase = 0;
        }
        pkt_last_ms = now;

        if(pkt_phase == 0 && (byte & PKT_ALWAYS_ONE) == 0)
        {
            stat_resyncs++;
            continue;
        }

        pkt[pkt_phase] = byte;
        pkt_phase++;

        if(pkt_phase >= 3)
        {
            pkt_phase = 0;
            mouse_process_packet(now);
        }
    }
}

/* --- Bring-up --------------------------------------------------------------
*
*  THE ORDER MATTERS, and every step of it can go wrong.
*
*   0. Hold IRQ 1 off at the PIC for the duration. See
*      mouse_hold_keyboard_irq() for what goes wrong without it, which is not
*      hypothetical: the bring-up fails at step 2 and reports no mouse on a
*      machine that has one.
*
*   1. Drain whatever is in the output buffer. The BIOS may have left a reply
*      there, and it would otherwise be read as the answer to step 3.
*
*   2. Read the configuration byte and write back a version with BOTH
*      interrupts off and the KEYBOARD port disabled, keeping every other bit.
*
*      The auxiliary interrupt is off because the whole bring-up is a
*      conversation of commands and replies read by POLLING, and an interrupt
*      that fired in the middle of it would hand an acknowledgement to a
*      handler that is not installed yet, or worse to one that is and would
*      file it as the first byte of a packet. The keyboard is disabled for the
*      mirror image of that: a keystroke during the bring-up puts a scancode in
*      the same buffer this file is polling, and it would be read as the
*      mouse's answer. That is a fraction of a second during boot, and every
*      exit from here puts the byte back exactly as it was found -- including
*      the failure exits, because a driver that gives up and leaves the
*      keyboard switched off has done far more damage than the mouse it did
*      not find.
*
*      A failure here means the controller is not answering at all. Nothing has
*      been changed yet, so there is nothing to undo: report no mouse.
*
*   3. 0xA8, enable the auxiliary port. On timeout: restore and report no
*      mouse.
*
*   4. 0xFF, reset. THIS IS THE DETECTION. A mouse answers 0xFA, then runs its
*      self test and answers 0xAA, then its device id. An empty port answers
*      nothing, and the bounded wait for the acknowledgement is what turns that
*      silence into a return value instead of a hang. On timeout, or on a self
*      test byte that is not 0xAA: put the configuration byte back, disable the
*      auxiliary port again so nothing is left half enabled, and report no
*      mouse. That is not an error -- mouse.h is explicit that a machine with
*      no mouse is an ordinary outcome, and everything above then draws a
*      pointer that never moves rather than refusing to run.
*
*      The reset is also what makes the device id trustworthy and what puts a
*      mouse the BIOS had already talked into four byte packets back into three
*      byte ones. See the note on IntelliMouse below.
*
*   5. 0xF6, set defaults: 100 samples a second, resolution 4 counts per
*      millimetre, scaling 1:1, reporting off. A known starting point instead
*      of whatever the firmware felt like.
*
*   6. 0xF4, enable reporting -- until this the mouse is awake and silent.
*      A failure at 5 or 6 means a device that answered its reset and then
*      stopped cooperating, which is indistinguishable from having no mouse as
*      far as anything above is concerned: there will be no packets. Same
*      cleanup, same return.
*
*   7. Drain again. This is the step whose absence produces the bug that is
*      hardest to see: any byte still in the buffer becomes the first byte of
*      the first packet, and the stream is one byte out of step from the very
*      first movement.
*
*   8. Install the handler, open the PIC, and only then set the auxiliary
*      interrupt bit in the configuration byte. The handler exists before the
*      first interrupt can be generated, which is the whole point of doing it
*      in this order.
*
*   9. One last look at the buffer. Between the drain in step 7 and the write
*      in step 8 the device is running and nothing is collecting from it, so a
*      byte can land in a window a few microseconds wide -- and on a controller
*      that raises IRQ 12 on the OBF EDGE rather than its level, that byte
*      raised no interrupt (the bit was off) and no further one will ever be
*      raised while it sits there. That is a mouse that is dead from boot for
*      no visible reason. Reading it clears the buffer and the next movement
*      raises a fresh interrupt; the handler starts at phase 0 and the bit 3
*      test sorts out what the remains were.
*
*  INTELLIMOUSE IS DELIBERATELY NOT NEGOTIATED.
*
*  The four byte mode with the scroll wheel is entered by a fixed knock --
*  three set-sample-rate commands with the arguments 200, 100, 80 -- and this
*  file never sends it. Three reasons, in order of weight:
*
*    - THE PACKET LENGTH CHANGES IF THE DEVICE ACCEPTS. A driver that
*      negotiates and then parses three bytes reads every fourth byte as the
*      start of the next packet and is permanently, unrecoverably out of step.
*      That is the failure the brief calls out, and the way to be sure of not
*      having it is not to be able to have it: the knock is not in this file,
*      so it cannot be sent by accident.
*    - THERE IS NOWHERE TO PUT A WHEEL. mouse_event has x, y, dx, dy, buttons,
*      changed and a timestamp, and mouse.h is frozen. A wheel that arrives and
*      is discarded is cost with no benefit.
*    - The reset in step 4 guarantees the starting point. An IntelliMouse that
*      some BIOS already switched into four byte mode is put back into three
*      byte mode by 0xFF, so this driver is not merely not asking for the mode
*      -- it is actively ensuring it is off, which matters because the device
*      would otherwise have been left in it.
*
*  The device id is read and kept anyway, purely so that mouse_describe() can
*  say what turned up. After a reset it is 0x00 on everything.
*/

/* Undoes what the bring-up did and reports the absence of a mouse. Every
*  failure path goes through here so that none of them can forget the
*  configuration byte. */
static int mouse_init_give_up(unsigned char orig_config, int aux_enabled,
                              unsigned char saved_pic_mask)
{
    if(aux_enabled)
        (void)ps2_write_command(PS2_CMD_AUX_DISABLE);

    if(ps2_write_command(PS2_CMD_WRITE_CONFIG))
        (void)ps2_write_data(orig_config);

    ps2_flush();
    mouse_release_keyboard_irq(saved_pic_mask);

    mouse_present_flag = 0;
    return -1;
}

int mouse_init(void)
{
    unsigned char orig_config;
    unsigned char config;
    unsigned char reply;
    unsigned char saved_pic_mask;

    mouse_present_flag = 0;
    mouse_device_id    = MOUSE_ID_STANDARD;
    pkt_phase          = 0;
    pkt_last_ms        = (unsigned int)timer_get_ticks();

    cur_x       = (int16_t)(bound_w / 2);
    cur_y       = (int16_t)(bound_h / 2);
    cur_buttons = 0;

    /* 0. */
    saved_pic_mask = mouse_hold_keyboard_irq();

    /* 1. */
    ps2_flush();

    /* 2. */
    if(!ps2_write_command(PS2_CMD_READ_CONFIG))
    {
        mouse_release_keyboard_irq(saved_pic_mask);
        return -1;
    }
    if(!ps2_read_data(&orig_config, PS2_WAIT_MS))
    {
        mouse_release_keyboard_irq(saved_pic_mask);
        return -1;
    }

    config = (unsigned char)(orig_config & ~(PS2_CFG_AUX_IRQ |
                                             PS2_CFG_KBD_IRQ |
                                             PS2_CFG_AUX_CLOCK_OFF));
    config = (unsigned char)(config | PS2_CFG_KBD_CLOCK_OFF);

    if(!ps2_write_command(PS2_CMD_WRITE_CONFIG))
        return mouse_init_give_up(orig_config, 0, saved_pic_mask);
    if(!ps2_write_data(config))
        return mouse_init_give_up(orig_config, 0, saved_pic_mask);

    /* 3. */
    if(!ps2_write_command(PS2_CMD_AUX_ENABLE))
        return mouse_init_give_up(orig_config, 0, saved_pic_mask);

    ps2_flush();

    /* 4. */
    if(!mouse_command(MOUSE_CMD_RESET))
        return mouse_init_give_up(orig_config, 1, saved_pic_mask);

    if(!ps2_read_data(&reply, PS2_RESET_MS) || reply != MOUSE_REPLY_SELFTEST)
        return mouse_init_give_up(orig_config, 1, saved_pic_mask);

    /* The id completes the reset reply. A device that stops after the self
    *  test leaves nothing dangerous behind -- step 7 drains whatever did
    *  arrive -- so a missing id is not fatal, it only leaves the description
    *  saying "standard". */
    if(ps2_read_data(&reply, PS2_WAIT_MS))
        mouse_device_id = reply;

    /* 5. and 6. */
    if(!mouse_command(MOUSE_CMD_SET_DEFAULTS))
        return mouse_init_give_up(orig_config, 1, saved_pic_mask);
    if(!mouse_command(MOUSE_CMD_ENABLE))
        return mouse_init_give_up(orig_config, 1, saved_pic_mask);

    /* 7. */
    ps2_flush();

    /* The keyboard's line goes back on here rather than at the end, and the
    *  order is the point. A keystroke that arrived while the line was masked
    *  left the request latched in the PIC, and it is delivered the instant the
    *  mask lifts; kb.c's handler will read port 0x60 once. Doing that now,
    *  with the buffer just emptied and the mouse's own interrupt still off,
    *  costs the keyboard one meaningless read of an empty buffer. Doing it
    *  after step 8 would have that read land on a real packet byte and put the
    *  stream one byte out of step before the first movement. */
    mouse_release_keyboard_irq(saved_pic_mask);

    /* 8. */
    pkt_phase   = 0;
    pkt_last_ms = (unsigned int)timer_get_ticks();

    irq_install_handler(MOUSE_IRQ, mouse_handler);
    mouse_unmask_irq();

    config = (unsigned char)((orig_config & ~PS2_CFG_AUX_CLOCK_OFF) |
                             PS2_CFG_AUX_IRQ);

    if(!ps2_write_command(PS2_CMD_WRITE_CONFIG))
    {
        irq_uninstall_handler(MOUSE_IRQ);
        return mouse_init_give_up(orig_config, 1, saved_pic_mask);
    }
    if(!ps2_write_data(config))
    {
        irq_uninstall_handler(MOUSE_IRQ);
        return mouse_init_give_up(orig_config, 1, saved_pic_mask);
    }

    mouse_present_flag = 1;

    /* 9. */
    if((inportb(PS2_STATUS) & (PS2_STAT_OBF | PS2_STAT_AUX)) ==
       (PS2_STAT_OBF | PS2_STAT_AUX))
    {
        ps2_flush();
    }

    return 0;
}

/* --- What mouse.h promises ------------------------------------------------ */

int mouse_present(void)
{
    return mouse_present_flag;
}

/* Reading one aligned word cannot be torn on this machine, so these need no
*  interrupt masking: the worst that happens is that a caller reads the
*  position as it was one packet ago, and one packet ago is ten milliseconds
*  ago. A caller that needs x and y to belong to the SAME event has to take
*  them from mouse_poll(), which is what the queue is for. */
int mouse_x(void)
{
    return (int)cur_x;
}

int mouse_y(void)
{
    return (int)cur_y;
}

int mouse_buttons(void)
{
    return (int)cur_buttons;
}

void mouse_set_bounds(int width, int height)
{
    unsigned long flags;

    if(width < 1)
        width = 1;
    if(height < 1)
        height = 1;
    if(width > MOUSE_MAX_BOUND)
        width = MOUSE_MAX_BOUND;
    if(height > MOUSE_MAX_BOUND)
        height = MOUSE_MAX_BOUND;

    /* The rectangle and the position it constrains have to change together.
    *  With interrupts on, a packet landing between the two writes would clamp
    *  the pointer against one rectangle and store it under the other, and the
    *  window is exactly wide enough for the case this is called in -- a mode
    *  switch, while the mouse is being moved. */
    flags = mouse_irq_save();

    bound_w = width;
    bound_h = height;

    /* The pointer may be outside the new rectangle: shrinking the screen is
    *  the ordinary way to get there, and leaving it out there is the thing
    *  mouse.h says must not happen. */
    cur_x = (int16_t)mouse_clamp((int)cur_x, bound_w);
    cur_y = (int16_t)mouse_clamp((int)cur_y, bound_h);

    mouse_irq_restore(flags);
}

void mouse_set_position(int x, int y)
{
    unsigned long flags;

    flags = mouse_irq_save();

    /* Clamped like any other update. A caller that has just set its own bounds
    *  and then places the pointer outside them is not making a statement about
    *  the bounds, it is making a mistake -- most often an off-by-one at the
    *  right or bottom edge, where width and height are not valid coordinates.
    *  Honouring it would put the pointer somewhere the next packet would drag
    *  it back from and nothing would explain the jump. */
    cur_x = (int16_t)mouse_clamp(x, bound_w);
    cur_y = (int16_t)mouse_clamp(y, bound_h);

    mouse_irq_restore(flags);
}

int mouse_poll(mouse_event *out)
{
    unsigned long flags;
    uint32_t      tail;
    uint32_t      slot;

    if(out == 0)
        return 0;

    flags = mouse_irq_save();

    tail = mq_tail;

    if(mq_head == tail)
    {
        mouse_irq_restore(flags);
        return 0;
    }

    slot = tail & MOUSE_QUEUE_MASK;

    out->x       = mq[slot].x;
    out->y       = mq[slot].y;
    out->dx      = mq[slot].dx;
    out->dy      = mq[slot].dy;
    out->buttons = mq[slot].buttons;
    out->changed = mq[slot].changed;
    out->time_ms = mq[slot].time_ms;

    /* The bytes are out of the queue before the space is given back. */
    __asm__ __volatile__ ("" : : : "memory");
    mq_tail = tail + 1;

    mouse_irq_restore(flags);
    return 1;
}

const void *mouse_wait_channel(void)
{
    return MOUSE_WAIT_CHANNEL;
}

uint32_t mouse_packets(void)
{
    return stat_packets;
}

uint32_t mouse_resyncs(void)
{
    return stat_resyncs;
}

uint32_t mouse_overflows(void)
{
    return stat_overflows;
}

uint32_t mouse_dropped(void)
{
    return stat_dropped;
}

/* Set by mouse_inject() to whatever the other driver calls itself, and
*  reported instead of the PS/2 description once something has come in that
*  way. Null until then. */
static const char *foreign_name = 0;

/* Delivers movement from a pointing device that is not on the 8042. See
*  mouse.h for the contract, and in particular for why dy arrives already in
*  screen orientation rather than being flipped here.
*
*  It takes the interrupt guard where mouse_process_packet() does not need to:
*  the PS/2 handler is already inside an interrupt, while this may be called
*  from a task -- a polled USB driver is -- and mouse_deliver() writes the
*  producer end of a queue whose other end is read by tasks. Without the guard
*  a timer tick between the two stores that publish an event would let a
*  consumer see a slot that has been counted and not filled in. */
void mouse_inject(int dx, int dy, uint8_t buttons, const char *name)
{
    unsigned long flags;

    flags = mouse_irq_save();

    /* Marking the pointer present is what makes everything above this file
    *  start believing it: mouse_present() gates the shell command and the
    *  bounds. A machine with no PS/2 mouse and a USB one has to answer yes. */
    mouse_present_flag = 1;
    if(name != 0)
        foreign_name = name;

    /* Counted here as well, because mouse_packets() is one of the numbers
    *  mouse.h promises says whether the hardware is saying anything -- and a
    *  pointer that plainly moved while the count stayed at zero reads as a
    *  broken driver. It was exactly that on screen before this line: position
    *  (416,300) under the words "no packet has arrived yet".
    *
    *  The other three counters stay PS/2 only and that is right rather than an
    *  oversight: resyncs and overflows are properties of an unframed byte
    *  stream, which USB does not have, and a USB report that goes nowhere is
    *  counted by the class driver instead. */
    stat_packets++;

    mouse_deliver(dx, dy, buttons, (unsigned int)timer_get_ticks());

    mouse_irq_restore(flags);
}

const char *mouse_describe(void)
{
    /* Whatever last spoke wins. A machine with both a PS/2 and a USB pointer
    *  has one cursor, and naming the one that is actually moving it is more
    *  use than naming the one that was found first. */
    if(foreign_name != 0)
        return foreign_name;

    if(!mouse_present_flag)
        return "no mouse";

    switch(mouse_device_id)
    {
        case MOUSE_ID_STANDARD:
            return "PS/2 mouse, 3 buttons";
        case MOUSE_ID_WHEEL:
            return "PS/2 wheel mouse, 3 buttons, wheel not used";
        case MOUSE_ID_5BUTTON:
            return "PS/2 mouse, 5 buttons, only 3 reported";
        default:
            return "PS/2 mouse, unrecognised type";
    }
}
