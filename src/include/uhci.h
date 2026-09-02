/* TomatOS - UHCI host controller
*  Desc: The bottom third of the USB stack, for Intel's USB 1.1 controller.
*
*  UHCI is the simplest of the four host controller interfaces and the reason
*  the stack starts here: its registers are in I/O SPACE rather than memory,
*  which this kernel already does well, and its data structures are three small
*  things -- a frame list, transfer descriptors and queue heads.
*
*  What makes it awkward is that the controller reads those structures itself,
*  by DMA. So they live at physical addresses the kernel has to hand over
*  literally, they have alignment requirements the hardware enforces, and the
*  order in which fields are written matters: the controller may be walking a
*  list while it is being built. Everything in the implementation that looks
*  overcautious about ordering is about that.
*
*  Speed: 12 Mbit/s full speed, 1.5 Mbit/s low speed. A keyboard or mouse is
*  low speed and needs nothing more. A disk at full speed reads about a
*  megabyte a second, which is slow and is the honest ceiling of USB 1.1 --
*  not a shortcoming of this driver.
*
*  THERE IS USUALLY MORE THAN ONE. A UHCI controller is a function of the
*  chipset rather than a card: a PIIX3 has one, an ICH4 three, an ICH9 six, and
*  they SHARE the machine's sockets between them -- two each. So "the first
*  UHCI on the PCI bus" is very often a controller with nothing plugged into
*  it, which reports two empty ports perfectly correctly and leaves the
*  keyboard on the third one undiscovered. This driver brings up every UHCI it
*  finds and presents their root ports to usb.c as one flat numbering, so that
*  nothing above has to know there is more than one.
*
*  AND THE SOCKETS MAY NOT BE ON A UHCI AT ALL. On a chipset that has both, the
*  sockets belong to an EHCI, which routes them either to itself or to its UHCI
*  companions; the BIOS routes them to itself, so a full speed keyboard comes
*  up at 480 Mbit/s where this kernel cannot see it and every UHCI reports an
*  empty bus. uhci_init() hands them back before it brings anything up. That is
*  one register write and it is described at length in the implementation, next
*  to the four checks that have to pass before it is made.
*
*  This is deliberately not the controller a modern machine has. See usb.h for
*  why the layering exists and what is meant to survive an xHCI driver later.
*/
#ifndef __UHCI_H
#define __UHCI_H

#include "typedefs.h"

/* Hands the root ports back from any EHCI that is holding them, then finds
*  EVERY UHCI controller on the PCI bus and brings each one up: BIOS handoff,
*  host controller reset, the frame list, and the schedule. Registers itself
*  with the USB core once, on behalf of all of them, if at least one came up.
*
*  Returns 0 when at least one controller is scheduling, or negative when there
*  is none -- which is the ordinary outcome on a machine QEMU started without
*  "-usb", and on real hardware built in the last decade. Nothing here may keep
*  the boot from finishing. */
extern int uhci_init(void);

/* Non-zero when at least one controller came up. */
extern int uhci_present(void);

/* How many did, which is not the same as how many are on the bus -- the report
*  line says both. Zero when none. */
extern int uhci_controllers(void);

/* What became of the root ports of the EHCI controllers on this machine, in
*  the few words a boot line has room for: that they were handed to the
*  companions, or that they were left alone and why. EMPTY STRING when the
*  machine has no EHCI at all, which is what a caller should test rather than
*  printing an empty clause. Never null. */
extern const char *uhci_ehci_note(void);

/* One line for the shell: how many controllers there are, where each one is
*  and what it found. Never null. */
extern const char *uhci_info(void);

/* How many frames the FIRST controller has processed since it was started,
*  read off its own frame number register. The single most useful number for
*  telling a controller that is running from one that was set up and never
*  started -- which looks identical from every other angle.
*
*  One controller and not a sum over all of them, deliberately: the number is
*  useful because it has an expected value, one frame per millisecond, and a
*  sum over three controllers reads three times that and turns a healthy
*  machine into a suspicious one. The per-controller totals are on the
*  uhci_info() line, where each has a controller named next to it. */
extern uint32_t uhci_frames(void);

#endif
