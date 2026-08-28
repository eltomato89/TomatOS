/* TomatOS - VGA graphics, mode 13h
*  Desc: 320x200 at 256 colours, switched by programming the registers
*        directly -- there is no BIOS to call from protected mode.
*
*  The framebuffer lives at physical 0xA0000, one byte per pixel, and is
*  reached through the direct mapping like any other memory mapped hardware.
*  A pixel is simply framebuffer[y * 320 + x], which is what makes this mode
*  the pleasant one to start with: no planes, no bit shifting, no bank
*  switching.
*
*  Switching back to text mode is the part that needs care. The text mode
*  font lives in plane 2 of the same video memory the graphics mode uses as
*  its framebuffer, so drawing destroys it. vga_set_mode() saves the glyphs
*  before leaving text mode and restores them on the way back -- without
*  that, the console returns to a screen full of blanks.
*/
#ifndef __VGA_H
#define __VGA_H

#include "typedefs.h"

#define VGA_WIDTH        320
#define VGA_HEIGHT       200
#define VGA_PIXELS       (VGA_WIDTH * VGA_HEIGHT)
#define VGA_FB_PHYS      0xA0000

#define VGA_MODE_TEXT      0    /* mode 3, 80x25 characters */
#define VGA_MODE_GRAPHICS  1    /* mode 13h, 320x200x256    */

/* Switches modes. Returns 0 on success. Setting the mode already in effect
*  is a no-op, so it is safe to call repeatedly. */
extern int vga_set_mode(int mode);

extern int vga_mode(void);

/* Start of the framebuffer, or 0 outside graphics mode. */
extern uint8_t *vga_framebuffer(void);

/* Sets one of the 256 palette entries. The DAC takes six bits per channel,
*  so components run 0..63, not 0..255. */
extern void vga_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/* --- Drawing (vgadraw.c) -------------------------------------------------
*  Everything clips against the screen edges, so a caller may pass
*  coordinates that lie partly or wholly outside without checking first. */

extern void vga_clear(uint8_t colour);
extern void vga_pixel(int x, int y, uint8_t colour);
extern uint8_t vga_pixel_at(int x, int y);
extern void vga_hline(int x, int y, int len, uint8_t colour);
extern void vga_vline(int x, int y, int len, uint8_t colour);
extern void vga_line(int x0, int y0, int x1, int y1, uint8_t colour);
extern void vga_rect(int x, int y, int w, int h, uint8_t colour);
extern void vga_fill(int x, int y, int w, int h, uint8_t colour);
extern void vga_circle(int cx, int cy, int radius, uint8_t colour);
extern void vga_disc(int cx, int cy, int radius, uint8_t colour);

/* Text through the built-in 8x8 font. A colour of 0xFF for bg means
*  transparent, so glyphs can be drawn over a picture. */
extern void vga_char(int x, int y, char c, uint8_t fg, uint8_t bg);
extern void vga_string(int x, int y, const char *s, uint8_t fg, uint8_t bg);

/* --- The font (font8x8.c) ------------------------------------------------ */
#define FONT_WIDTH   8
#define FONT_HEIGHT  8
/* One byte per row, most significant bit leftmost. Entries 0..127. */
extern const uint8_t font8x8[128][FONT_HEIGHT];

#endif
