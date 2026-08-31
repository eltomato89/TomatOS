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
*  This is deliberately not the controller a modern machine has. See usb.h for
*  why the layering exists and what is meant to survive an xHCI driver later.
*/
#ifndef __UHCI_H
#define __UHCI_H

#include "typedefs.h"

/* Finds a UHCI controller on the PCI bus and brings it up: BIOS handoff, host
*  controller reset, the frame list, and the schedule. Registers itself with
*  the USB core on success.
*
*  Returns 0, or negative when there is none -- which is the ordinary outcome
*  on a machine QEMU started without "-usb", and on real hardware built in the
*  last decade. Nothing here may keep the boot from finishing. */
extern int uhci_init(void);

extern int uhci_present(void);

/* One line for the shell: where the controller is and what it found. Never
*  null. */
extern const char *uhci_info(void);

/* How many frames the controller has processed since it was started, read off
*  its own frame number register. The single most useful number for telling a
*  controller that is running from one that was set up and never started --
*  which looks identical from every other angle. */
extern uint32_t uhci_frames(void);

#endif
