/* TomatOS - drawing from ring 3
*  Desc: The shapes of src/video/fbdraw.c, drawn into a buffer of the program's own
*        and copied onto the screen in pieces.
*
*  ------------------------------------------------------------------------
*  Why this exists next to fbdraw.c
*  ------------------------------------------------------------------------
*  src/video/fbdraw.c already draws lines, rectangles and text into the very same
*  framebuffer. None of it is reachable from here: this is a separate binary
*  on a FAT volume, linked -nostdlib against user/lib.o and nothing else, and
*  the constraint that user/include/syscall.h states -- a program must not depend on
*  kernel internals -- is not a style rule, it is the reason a program on disk
*  can be older than the kernel that runs it. So the shapes are written again.
*
*  What is NOT written again is the thinking. Three decisions are copied out
*  of fbdraw.c deliberately and are the reason this file is short:
*
*    - a row address is formed from the PITCH the card reported and never from
*      width * bytes, because cards pad rows and a computed stride draws a
*      picture that shears a little further sideways with every row;
*    - the depth is decided ONCE per span rather than once per pixel, so a
*      1024 pixel row costs one branch and not 1024;
*    - coordinates outside the surface are CLIPPED and drawn as far as they
*      reach, never refused, so a caller need not bound every calculation.
*
*  ------------------------------------------------------------------------
*  The three differences, and why each one had to change
*  ------------------------------------------------------------------------
*  1. THERE IS A BACK BUFFER, AND IT IS THE POINT
*
*  fbdraw.c draws a still picture: a caption, a box, a splash screen. Drawing
*  those straight into the framebuffer is fine, because nobody watches them
*  arrive. A window system draws a MOVING picture, and drawing that straight
*  into the framebuffer fails twice over -- the user sees each shape appear
*  one at a time, and moving anything means erasing it first, so every frame
*  contains a moment where the thing being moved is simply not there. That
*  moment is the flicker.
*
*  So everything here draws into ordinary memory -- gfx_canvas() -- and the
*  changed part is copied across afterwards by gfx_flush(). The screen only
*  ever sees finished pictures.
*
*  2. THE SURFACE IS AN ARGUMENT, NOT A GLOBAL
*
*  fbdraw.c has exactly one surface, so it keeps it in file scope statics and
*  refreshes them from fbcon.c at the top of every primitive. Here there are
*  two -- the canvas and the screen -- and the primitives have to be pointed
*  at one of them. It costs a parameter and it buys the thing that made this
*  library testable: a gfx_surface can also be pointed at an ordinary array
*  on a host machine, with a deliberately awkward pitch, and every clipping
*  claim below can then be checked against a sentinel in the row padding
*  instead of against a screenshot.
*
*  3. COLOURS ARE PIXEL VALUES, NOT INDICES
*
*  fbdraw.h explains why its callers pass an index: mode 13h has a hardware
*  palette, an index is what colour MEANS there, and mirroring vga.h let the
*  same drawing code target both. Neither half of that argument survives the
*  trip to ring 3. There is no mode 13h here -- sys_mapfb() hands over a
*  direct colour surface or nothing at all -- so a palette would be a 256
*  entry table translating from a namespace nobody else speaks, plus a lookup
*  on every call, to arrive at the value gfx_rgb() could have produced once at
*  startup. A gfx_color is therefore the packed pixel itself, made by
*  gfx_rgb() against a particular surface, and the caller keeps the handful it
*  uses in variables of its own.
*
*  That does mean a gfx_color belongs to the surface it was made for. Mixing
*  them across two surfaces of different depths draws the wrong colour -- not
*  out of bounds, just wrong -- and the canvas is deliberately created with
*  the screen's own format so that in this program there is only ever one.
*
*  ------------------------------------------------------------------------
*  Clipping, stated once
*  ------------------------------------------------------------------------
*  Every primitive clips to the surface, and additionally to the clip window
*  set by gfx_clip_set(). The contract is fbdraw.c's, edge for edge:
*
*      a zero or negative width          nothing is drawn, and is not an error
*      entirely outside                  nothing is drawn
*      partly outside                    the inside part is drawn IN PLACE --
*                                        never shifted, never wrapped into the
*                                        neighbouring row
*
*  "Never wrapped into the neighbouring row" is the one that matters. A run
*  that is shifted instead of shortened looks like a cosmetic bug; a run that
*  wraps writes past the end of the last row, which on the screen surface is
*  past the end of the mapping.
*
*  The clip window is not a second contract, it is the same one applied to a
*  smaller rectangle, and it exists for the redraw strategy in gui.c: a frame
*  repaints the whole scene into the canvas but only inside the rectangles
*  that actually changed, and the clip window is what makes "the whole scene"
*  cost the changed pixels instead of all of them.
*/
#ifndef __USER_GFXLIB_H
#define __USER_GFXLIB_H

#include "syscall.h"


/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

/* The largest screen this library will accept, and therefore the size of the
*  back buffer that has to exist for it. It is the same ceiling
*  src/include/fbcon.h names as FBCON_MAX_WIDTH/HEIGHT, and that header lists
*  the four places that have to agree about it -- vbe_res_table in
*  boot/vbe.inc, the Multiboot video request in src/kernel/start.asm, the console's
*  shadow buffer, and this. The number cannot be included from there: a ring 3
*  binary must not depend on kernel internals, for the reason user/include/syscall.h
*  gives, so this is a COPY and raising one means raising the other by hand.
*
*  It is not a guess about what might turn up. Stage 2 walks vbe_res_table
*  downwards and takes the highest mode the card actually reports, so the mode
*  is NEGOTIATED AT BOOT and the same binary lands at 1920x1080 on one machine
*  and 640x480 on another. This is the top of that table; everything below it
*  is reached by leaving the tail of the buffer unused.
*
*  A mode bigger than this is REFUSED by gfx_open() rather than drawn into
*  partially. That is a deliberate choice and not the same one the console
*  makes: fbcon.c CLIPS a larger mode, because a console that refuses to print
*  is a machine with no way to say what went wrong, whereas this program can
*  exit and let the console say it. Half a screen of window system and half a
*  screen of whatever the console left behind, with a pointer that walks off
*  into the half nobody owns, is worse than a message saying why. */
#define GFX_MAX_WIDTH    1920
#define GFX_MAX_HEIGHT   1080
#define GFX_MAX_BYTES       4    /* 32 bpp, the widest pixel there is here   */

/* What the back buffer therefore costs: 8294400 bytes at the ceiling. Stated
*  as a name because it is the number that matters to a caller -- it is .bss,
*  and src/kernel/exec.c's loader allocates a physical frame per page of it at every
*  start, so this is 8.3 MB of real memory taken from a 64 MB guest whatever
*  mode the machine actually came up in. */
#define GFX_BACK_BYTES  ((unsigned long)GFX_MAX_WIDTH * GFX_MAX_HEIGHT * \
                         GFX_MAX_BYTES)

/* Coordinates further out than this are ignored by the primitives that do
*  arithmetic on them, for the reason fbdraw.c gives at its own copy of this
*  constant: the line clipper multiplies one coordinate difference by another,
*  and with everything inside +/-16384 those products stay below 2^30 and
*  cannot overflow a 32-bit int. A wrong product turns a clip test into a
*  wrong answer, and a wrong answer lets a write escape the surface. */
#define GFX_COORD_MAX   16384

/* The font cell. Eight by eight, the same glyphs the kernel console draws
*  with -- see the note at gfx_char(). */
#define GFX_FONT_WIDTH      8
#define GFX_FONT_HEIGHT     8

/* An 8x8 glyph blown up by more than this is not text any more, and the cap
*  keeps x + GFX_FONT_WIDTH * scale inside GFX_COORD_MAX for any x the
*  coordinate test admits: 8 * 512 = 4096, so the sum stays below 20480. */
#define GFX_MAX_SCALE     512

/* How many separate changed rectangles a frame may carry before they are all
*  merged into one bounding box. See gfx_damage(). */
#define GFX_DAMAGE_MAX     16


/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* A packed pixel value for one particular surface, as gfx_rgb() made it.
*  Deliberately not a struct and not an index: it is the exact bit pattern
*  that goes into the framebuffer, so a fill writes it and converts nothing.
*
*  unsigned INT and not unsigned long, although the two are the same width on
*  this ILP32 target. The distinction is not pedantry: gfxlib.c casts a pixel
*  pointer to this type to store a 32 bpp pixel in one go, and the host test
*  harness compiles the same file for an LP64 machine, where "unsigned long"
*  would make that one store write eight bytes and walk into the next pixel.
*  A packed pixel is 32 bits wide at most on every mode this system can
*  produce, and unsigned int is 32 bits wide on every target it can be built
*  for. */
typedef unsigned int gfx_color;

typedef struct
{
	int x;
	int y;
	int w;
	int h;
} gfx_rect;

/* Somewhere to draw. Filled in by gfx_surface_init() and never by hand: the
*  validation there is what everything below is allowed to assume.
*
*  mem == 0 means "this surface is not usable", which is the state a rejected
*  gfx_surface_init() leaves behind, and every primitive checks it. A caller
*  that ignores the return value therefore draws nothing rather than through a
*  null pointer. */
typedef struct
{
	unsigned char *mem;          /* first byte of row 0, 0 when unusable     */
	int            w;            /* pixels                                   */
	int            h;
	unsigned long  pitch;        /* BYTES between two rows -- not w * bytes  */
	int            bytes;        /* bytes per pixel, 1..4                    */
	unsigned long  bpp;          /* bits per pixel, 32/24/16/15/8            */

	/* Where the channels sit, straight out of sys_fbinfo. Kept so that
	*  gfx_rgb() can pack against the format the card actually reported
	*  rather than against the assumption that 32 bpp means 0x00RRGGBB. */
	unsigned char  red_pos;
	unsigned char  red_size;
	unsigned char  green_pos;
	unsigned char  green_size;
	unsigned char  blue_pos;
	unsigned char  blue_size;

	/* The clip window, already intersected with the surface, so the
	*  primitives can trust it without re-testing it against w and h. */
	gfx_rect       clip;
} gfx_surface;


/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */

/* Describes a block of memory as a surface, and says whether it can be drawn
*  on. Returns 1 on success, 0 on refusal -- and a refusal clears s->mem, so a
*  caller that draws anyway draws nothing.
*
*  The checks are the ones fbdraw.c's fb_sync() makes, and they are not
*  ceremony. This library is one bad number away from writing outside the
*  block it was given:
*
*    - a pitch shorter than a row puts the right hand end of every row into
*      the next one, and the last row past the end of the block;
*    - a depth outside 1..4 bytes has no case in the pixel store;
*    - a surface larger than GFX_COORD_MAX breaks the overflow argument that
*      the clipping rests on.
*
*  bpp is what the card reported (32, 24, 16, 15 or 8) and the byte width is
*  derived from it as (bpp + 7) / 8, so 15 and 16 are both two bytes -- they
*  differ only in where the channels sit, which is what the six channel
*  arguments say and what gfx_rgb() then packs against. */
extern int gfx_surface_init(gfx_surface *s, void *mem,
                            int w, int h, unsigned long pitch,
                            unsigned long bpp,
                            unsigned char red_pos, unsigned char red_size,
                            unsigned char green_pos, unsigned char green_size,
                            unsigned char blue_pos, unsigned char blue_size);

/* Packs a colour for this surface. 0..255 per channel, the full range a
*  channel can carry -- NOT the 0..63 the VGA DAC takes, so code carried over
*  from a mode 13h palette has to be scaled up or every colour comes out at a
*  quarter brightness.
*
*  Channels narrower than eight bits are truncated from the top, which is the
*  same thing src/video/fbcon.c's rgb_to_pixel() does, so a colour looks the same on
*  both sides of the system call gate. */
extern gfx_color gfx_rgb(const gfx_surface *s,
                         unsigned char r, unsigned char g, unsigned char b);

/* Narrows what the primitives may touch to this rectangle, intersected with
*  the surface. An empty intersection is legal and means "draw nothing", which
*  is what makes a repaint loop over damage rectangles safe to write without
*  special cases.
*
*  gfx_clip_none() restores the whole surface. */
extern void gfx_clip_set(gfx_surface *s, int x, int y, int w, int h);
extern void gfx_clip_none(gfx_surface *s);

/* Narrows the clip window FURTHER, to the intersection with what is already in
*  force. This is the one a window system needs and gfx_clip_set() is not: the
*  outer clip is the damage rectangle being repainted, and a window painting
*  its contents has to stay inside its own body as well, without losing the
*  damage rectangle. gfx_clip_set() would throw the outer one away and let the
*  contents paint over the rest of the screen.
*
*  gfx_clip_get() reads the window back, so a caller can save it before
*  narrowing and put it back with gfx_clip_set() afterwards -- the saved
*  rectangle is already inside the surface, so setting it restores it exactly.
*  Together they are a clip stack without a stack: the caller keeps the saved
*  rectangle in a local, which nests as deeply as the call graph does and
*  cannot be popped in the wrong order. */
extern void gfx_clip_intersect(gfx_surface *s, int x, int y, int w, int h);
extern void gfx_clip_get(const gfx_surface *s, gfx_rect *out);


/* ------------------------------------------------------------------ */
/* The screen, and the buffer in front of it                           */
/* ------------------------------------------------------------------ */

/* Takes the screen and prepares the back buffer.
*
*  Returns 0 on success. On failure it returns the negative SYS_* code, and
*  the two a caller has to tell apart are:
*
*    SYS_ENODEV   the machine booted into text mode and there is no
*                 framebuffer at all. An ORDINARY outcome -- "make run" boots
*                 exactly like that, because QEMU's -kernel loader does not
*                 implement the Multiboot video request -- and a program that
*                 meets it should say so and exit, not draw nowhere.
*    SYS_EBUSY    another task is holding the screen.
*
*  SYS_ENOMEM means the mode is larger than GFX_MAX_WIDTH x GFX_MAX_HEIGHT, so
*  the back buffer compiled into this program cannot hold a frame of it. The
*  screen is handed straight back before returning, so a refusal never leaves
*  the console suspended.
*
*  Nothing is drawn and the screen is not cleared: what is on it is still the
*  console's last frame until the caller paints and flushes. */
extern int gfx_open(void);

/* Gives the screen back and lets the kernel repaint its console. Safe to call
*  when gfx_open() failed or was never called, so an exit path does not have
*  to remember which. */
extern void gfx_close(void);

/* The back buffer: ordinary memory, the same geometry and pixel format as the
*  screen, and the surface everything should draw into. Valid only between a
*  successful gfx_open() and gfx_close(); before that its mem is 0 and drawing
*  into it does nothing. */
extern gfx_surface *gfx_canvas(void);

/* The screen itself. Exposed for its geometry and format, not so that anyone
*  draws into it: reads are uncached device memory and writes are visible the
*  instant they happen, which is the flicker the back buffer exists to avoid.
*  gfx_flush() is the one thing that should ever write here. */
extern gfx_surface *gfx_screen(void);


/* ------------------------------------------------------------------ */
/* Damage, and getting the picture onto the screen                     */
/* ------------------------------------------------------------------ */

/* Copies a rectangle of the canvas onto the screen. Clipped to both surfaces.
*  Returns the number of bytes written, which is what makes the cost of a
*  frame measurable rather than a matter of opinion.
*
*  Most callers want gfx_damage() and gfx_flush() instead; this is the one
*  they are built on and is worth having on its own for the first frame, where
*  the whole screen is genuinely new. */
extern unsigned long gfx_blit(int x, int y, int w, int h);

/* Records that a rectangle of the canvas has changed and will need copying.
*
*  WHY THIS IS NOT OPTIONAL. A full screen is width * height * bytes, which is
*  3 MiB at 1024x768x32 and 7.9 MiB at 1920x1080x32. That is the whole of it
*  across the bus per frame, and it is the whole of it the CPU has to touch
*  even when the only thing that moved was a pointer a couple of dozen pixels
*  across. Measured on this machine (the numbers are on screen in gui.c, which
*  times both), a full flush is tens of milliseconds and a pointer's worth of
*  damage is under one -- the difference between a pointer that follows the
*  mouse and a pointer that lags behind it. The bigger the mode, the more this
*  is the difference between a window system and a slideshow.
*
*  Rectangles that intersect are merged into their bounding box, because
*  blitting a pixel twice is pure waste; the merge repeats until nothing
*  intersects, so no two pending rectangles ever overlap. When more than
*  GFX_DAMAGE_MAX survive that, everything is merged into one bounding box
*  instead -- a deliberate surrender to a fixed size array, and the right one:
*  the result is a bigger blit, never a wrong picture. */
extern void gfx_damage(int x, int y, int w, int h);

/* The pending rectangles, so the caller can repaint each one before it is
*  copied. gfx_damage_at() returns 0 for an index that is not there. */
extern int  gfx_damage_count(void);
extern int  gfx_damage_at(int index, gfx_rect *out);

/* Copies every pending rectangle to the screen and forgets them. Returns the
*  total number of bytes written. */
extern unsigned long gfx_flush(void);

/* Throws the pending rectangles away without copying anything. For the case
*  where the caller has decided to repaint everything instead. */
extern void gfx_damage_clear(void);


/* ------------------------------------------------------------------ */
/* The shapes                                                          */
/* ------------------------------------------------------------------ */

/* Fills the whole clip window. Row by row rather than one flat run, because
*  the rows of a padded surface are not contiguous and a single span across
*  the padding would write into it -- which on the last row is outside the
*  surface entirely. */
extern void gfx_clear(gfx_surface *s, gfx_color colour);

extern void gfx_pixel(gfx_surface *s, int x, int y, gfx_color colour);
extern void gfx_hline(gfx_surface *s, int x, int y, int len, gfx_color colour);
extern void gfx_vline(gfx_surface *s, int x, int y, int len, gfx_color colour);
extern void gfx_line(gfx_surface *s, int x0, int y0, int x1, int y1,
                     gfx_color colour);

/* (x,y) is the top left corner and w and h are the OUTER dimensions, so the
*  right edge sits at x + w - 1. gfx_draw_rect() is the outline and
*  gfx_fill_rect() is the solid one, matching fbdraw_rect() and fbdraw_fill().
*  A width or height of zero draws nothing and is not an error. */
extern void gfx_draw_rect(gfx_surface *s, int x, int y, int w, int h,
                          gfx_color colour);
extern void gfx_fill_rect(gfx_surface *s, int x, int y, int w, int h,
                          gfx_color colour);

/* Text, from a private copy of the kernel's 8x8 font -- see the table at the
*  top of gfxlib.c for why it is a copy and not a reference.
*
*  scale blows each glyph pixel up into a scale x scale block; 1 is the font's
*  own size, and it takes a scale because the font is FIXED at 8x8 while the
*  screen is not. Eight pixels is a readable character up to about 1024x768
*  and a smudge at 1920x1080, so a caller that wants text the same apparent
*  size on both has to pick the scale from the mode -- which is what gui.c
*  does, and it is the only knob it has. The background is never touched, so a
*  glyph drawn over something leaves that something showing between the
*  strokes.
*
*  gfx_text() stops at the right edge of the clip window instead of wrapping:
*  wrapping would put the tail of a string a row below the head and one cell
*  in from the left, which reads as a bug and not as a line break. */
extern void gfx_char(gfx_surface *s, int x, int y, char c, int scale,
                     gfx_color colour);
extern void gfx_text(gfx_surface *s, int x, int y, const char *text, int scale,
                     gfx_color colour);

/* How wide gfx_text() would draw this string, in pixels. Saves every caller
*  that wants to centre a caption from writing strlen(s) * 8 * scale and
*  getting the 8 from somewhere. */
extern int gfx_text_width(const char *text, int scale);

#endif
