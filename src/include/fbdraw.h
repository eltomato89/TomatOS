/* TomatOS - drawing into the framebuffer
*  Desc: The same shapes vga.h offers for mode 13h, drawn into the linear
*        framebuffer the bootloader established instead.
*
*  Why this exists at all. There are two ways to get a picture on this machine
*  and they have almost nothing in common:
*
*    - Mode 13h, 320x200 with 256 palette entries, reached at RUNTIME by
*      programming the VGA registers. vga.c does that, and it works from the
*      text console because the registers are all it needs.
*    - A VBE framebuffer, 1024x768 in millions of colours, established by our
*      own stage 2 BEFORE the kernel starts. It cannot be entered later: VBE
*      is a real mode BIOS interface, and once the kernel is in protected mode
*      it is out of reach without a v86 monitor.
*
*  So a machine booted with "make run-bootdisk" is already in a graphics mode,
*  a far better one, and switching it to 13h would be a downgrade it could
*  never come back from -- there would be no way to restore 1024x768. Drawing
*  into the framebuffer that is already there is the only sensible answer, and
*  it is what this file is for.
*
*  The API deliberately mirrors vga.h down to the argument order, so that code
*  which draws can be written once and pointed at either. That includes the
*  colour type: a caller passes an INDEX, not an RGB triple, and sets up the
*  palette first. Mode 13h has no choice about that -- its hardware palette is
*  what colour means there -- and making the framebuffer path take RGB instead
*  would have meant two versions of every drawing routine above it.
*
*  The coordinate space is the framebuffer's own, so fbdraw_width() is 1024
*  where vga.h's VGA_WIDTH is a compile time 320. Anything drawing on both has
*  to ask rather than assume, which is the honest position anyway: the mode is
*  negotiated at boot and 800x600 or 640x480 are equally possible outcomes.
*/
#ifndef __FBDRAW_H
#define __FBDRAW_H

#include "typedefs.h"

/* Palette entries. The same 256 as mode 13h, so an index means the same thing
*  on both paths and a caller can program one table into both. */
#define FBDRAW_COLOURS   256

/* Non-zero when there is a framebuffer to draw on, i.e. when the machine
*  booted into a graphics mode. Everything below is a no-op otherwise, so a
*  caller that forgets to check draws nothing rather than faulting -- but it
*  should check, because there is a second way to get a picture and this is
*  how you find out which one you have. */
extern int fbdraw_available(void);

/* The surface. Pixels, not characters. */
extern int fbdraw_width(void);
extern int fbdraw_height(void);

/* Sets one palette entry, 0..255 per channel, exactly like vga_palette().
*  Nothing is written to hardware -- there is no hardware palette in a direct
*  colour mode -- so this only fills in the table the drawing routines below
*  translate through. That difference matters in one visible way: changing an
*  entry does NOT change pixels already on the screen, whereas in mode 13h it
*  does, because there the screen holds indices rather than colours. */
extern void fbdraw_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/* The shapes. Coordinates outside the surface are clipped, not refused, and
*  not drawn -- the same contract vga.c offers, so that a caller need not
*  bound every calculation itself. */
extern void fbdraw_clear(uint8_t colour);
extern void fbdraw_pixel(int x, int y, uint8_t colour);
extern void fbdraw_hline(int x, int y, int len, uint8_t colour);
extern void fbdraw_vline(int x, int y, int len, uint8_t colour);
extern void fbdraw_line(int x0, int y0, int x1, int y1, uint8_t colour);
extern void fbdraw_rect(int x, int y, int w, int h, uint8_t colour);
extern void fbdraw_fill(int x, int y, int w, int h, uint8_t colour);
extern void fbdraw_circle(int cx, int cy, int radius, uint8_t colour);
extern void fbdraw_disc(int cx, int cy, int radius, uint8_t colour);

/* Text, from the same 8x8 font vga.c draws with, so a caption looks the same
*  on both paths. scale multiplies each glyph pixel into a scale x scale block;
*  1 is the font's own size, which is tiny on a 1024x768 screen and is why this
*  takes a scale where vga_char() does not. */
extern void fbdraw_char(int x, int y, char c, int scale, uint8_t colour);
extern void fbdraw_string(int x, int y, const char *s, int scale, uint8_t colour);

#endif
