/* TomatOS - Framebuffer console
*  Desc: The console when the machine came up in a graphics mode.
*
*  Text mode gives 80x25 cells the hardware renders itself. A linear
*  framebuffer gives pixels and nothing else, so every character has to be
*  drawn -- and 1024x768 with an 8x16 cell is 128x48 characters, far more
*  room than text mode ever had.
*
*  Two things shape this design.
*
*  The framebuffer is not reachable when the first line is printed. Paging
*  is on from start.asm, but its boot directory maps only the low 4 MiB,
*  and a card's framebuffer commonly sits at something like 0xFD000000 --
*  outside the direct mapping entirely. It can only be mapped once the vmm
*  is up, which is well after the first output. So the console keeps a
*  SHADOW BUFFER of characters in .bss and writes there unconditionally;
*  when the framebuffer becomes available the whole buffer is drawn at once.
*  Nothing said during early boot is lost, which is exactly when losing it
*  would hurt most.
*
*  Scrolling costs real work here. Moving one line in text mode is 3840
*  bytes; at 1024x768x32 it is about three megabytes. The shadow buffer is
*  what makes a cheaper strategy possible, since scrolling it is a memmove
*  of a few kilobytes -- what to do about the pixels is up to the
*  implementation, and it should say what it chose.
*/
#ifndef __FBCON_H
#define __FBCON_H

#include "typedefs.h"

/* Upper bounds for the shadow buffer, which is statically allocated. A
*  larger mode is clipped to this rather than refused.
*
*  The numbers are not a guess at "big enough": they are the highest mode the
*  bootloader can actually establish, divided by the cell size. vbe_res_table
*  in boot/vbe.inc lists 1024x768, 800x600 and 640x480 and nothing above, and
*  the cell is 8x16 -- so 1024/8 by 768/16. Multiboot cannot raise that
*  either, because start.asm deliberately does not set the VIDEO_MODE flag,
*  which means a framebuffer only ever comes from our own stage 2.
*
*  They were 160x64 before, sized for a 1280x1024 nothing can produce. That
*  cost 8 KiB of .bss for cells no mode reaches, and 20 percent of every
*  scroll, since fbcon_scroll() moves whole rows of MAX_COLS.
*
*  Keeping the coupling in mind matters: adding a row to vbe_res_table means
*  raising these two, otherwise the console silently uses the top left corner
*  of the new mode instead of all of it. */
#define FBCON_MAX_COLS   (1024 / 8)
#define FBCON_MAX_ROWS   (768 / 16)

/* Records what the bootloader handed over, before anything can be mapped.
*  Safe to call with no framebuffer; fbcon_active() then stays false and
*  the console keeps using text mode.
*    addr  - PHYSICAL address of the framebuffer
*    type  - MULTIBOOT_FRAMEBUFFER_* from multiboot.h */
extern void fbcon_describe(uint32_t addr, uint32_t pitch, uint32_t width,
                           uint32_t height, uint8_t bpp, uint8_t type);

/* Maps the framebuffer and paints the shadow buffer onto it. Call once the
*  vmm can map, i.e. after vmm_init(). Returns 0 on success; on failure the
*  console simply stays in text mode. */
extern int fbcon_activate(void);

/* Non-zero once output goes to the framebuffer rather than to 0xB8000. */
extern int fbcon_active(void);

/* Geometry in characters, for the shell to report. */
extern int fbcon_cols(void);
extern int fbcon_rows(void);

/* The console proper. These mirror what scrn.c does for text mode, so it
*  can hand over to them when fbcon_active() is true. Coordinates are in
*  characters, colours are the usual 16 console colours. */
extern void fbcon_putc(int col, int row, unsigned char c, int attrib);
extern void fbcon_clear(int attrib);
extern void fbcon_scroll(int attrib);
extern void fbcon_cursor(int col, int row);

/* Description for the shell: resolution, depth and where it is mapped. */
extern const char *fbcon_info(void);

/* Paints the whole console from the shadow buffer. fbcon_activate() has always
*  done this once; it is a function of its own now because something else needs
*  it: a program that drew over the screen has to put the console back, and the
*  shadow buffer is the only record of what was on it. Safe to call at any
*  time, and a no-op when the console is not on a framebuffer. */
extern void fbcon_repaint(void);

/* The framebuffer's geometry, for code that wants to draw into it rather than
*  print into it. Width and height are pixels, not characters -- fbcon_cols()
*  and fbcon_rows() are the character counts. All return 0 when there is no
*  framebuffer, which is the case a caller has to check before using any of
*  them: on a machine booted into text mode there is nothing here to draw on.
*
*  The pitch is the byte distance between two rows and is NOT width * bytes per
*  pixel. A card is free to pad a row, and several do; computing it instead of
*  reading it is the classic way to get a picture that shears diagonally. */
extern uint32_t fbcon_width(void);
extern uint32_t fbcon_height(void);
extern uint32_t fbcon_pitch(void);
extern uint32_t fbcon_bpp(void);

/* The mapped framebuffer, as the kernel sees it, or 0 if there is none. This
*  is a window onto the card. Writes go straight to the screen, and reads are
*  slow enough that nothing here should ever do one. */
extern uint8_t *fbcon_pixels(void);

/* Packs a colour into whatever the mode's pixel format is. The channel
*  positions come from the mode, not from an assumption -- 32 bit is usually
*  but not always 0x00RRGGBB, and 16 bit is 5-6-5 rather than 5-5-5 on most
*  cards but not all. */
extern uint32_t fbcon_rgb(uint8_t r, uint8_t g, uint8_t b);

#endif
