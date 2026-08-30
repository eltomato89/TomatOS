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
*  The numbers are not a guess at "big enough": they are the highest mode this
*  system can end up in, divided by the cell size. That mode is named once, in
*  FBCON_MAX_WIDTH/HEIGHT below, and everything that has to agree about it
*  derives from there:
*
*    - vbe_res_table in boot/vbe.inc, which our own stage 2 negotiates from
*    - the video request in the Multiboot header in src/start.asm, which GRUB
*      honours and QEMU's -kernel loader does not
*    - the back buffer in user/gfxlib.c, which a ring 3 program draws into
*
*  Raising this raises all four, and each costs memory in a different place --
*  the shadow buffer here, a frame per page of the program's .bss there, and
*  the framebuffer itself on the card. A mode the hardware does not offer is
*  simply not chosen: stage 2 walks the table downwards and takes the highest
*  the card actually reports, so the same binary lands at 1920x1080 on one
*  machine and 640x480 on another.
*
*  If any of the four falls behind the others, the failure is quiet rather
*  than loud -- the console would use the top left corner of a larger mode and
*  the rest of the screen would keep whatever was on it. */
#define FBCON_MAX_WIDTH   1920
#define FBCON_MAX_HEIGHT  1080

#define FBCON_MAX_COLS   (FBCON_MAX_WIDTH / FBCON_CELL_W)
#define FBCON_MAX_ROWS   (FBCON_MAX_HEIGHT / FBCON_CELL_H)

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

/* The framebuffer's PHYSICAL address, for code that has to map it somewhere
*  other than where the kernel put it -- which today means handing it to a ring
*  3 program. 0 when there is none. */
extern uint32_t fbcon_phys(void);

/* Hands the screen to somebody else, and takes it back.
*
*  While suspended the console keeps its shadow buffer up to date and paints
*  nothing: everything printed meanwhile is remembered and appears at
*  fbcon_resume(), which repaints the whole screen from it. That is the same
*  arrangement "gfx" uses to draw over the console and put it back, generalised
*  so that a ring 3 program can do it -- and it is why the console does not
*  have to be told anything about what the other program drew.
*
*  Nesting is not supported and is refused rather than counted: two owners of
*  one screen is not a thing that can be made to work, and the caller that
*  loses would find out by having its drawing overwritten. */
extern int fbcon_suspend(void);
extern void fbcon_resume(void);
extern int fbcon_suspended(void);

#endif
