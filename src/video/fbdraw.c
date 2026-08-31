/* TomatOS - drawing into the VBE framebuffer
*  Desc: The shapes of vgadraw.c, drawn into the linear framebuffer the
*        bootloader established instead of into mode 13h.
*
*  fbdraw.h says why this exists at all; this file says what had to be done
*  differently. Everything here is the mode 13h code with four changes, and
*  those four are the whole of the difference:
*
*  1. THE ADDRESS OF A PIXEL
*
*  Mode 13h is fb[y * 320 + x] and always will be. Here a row starts at
*  y * PITCH, and the pitch is what the card reported, not width * bytes per
*  pixel. Cards pad rows -- a 1024 pixel wide 32 bpp mode with a pitch of 4224
*  is an ordinary thing to meet -- and computing the stride instead of asking
*  for it produces a picture that shears a little further to one side with
*  every row. Nothing in this file forms a row address any way but through
*  row_at().
*
*  2. THE SIZE OF A PIXEL
*
*  One byte, always, in mode 13h. Here it is four, three, two or two again,
*  because our stage 2 negotiates the mode with the BIOS and 32, 24, 16 and 15
*  bits are all outcomes it accepts (boot/vbe.inc ranks them in that order).
*  So every write goes through a switch on the depth -- but never a switch per
*  pixel: the depth is decided once, outside both loops, exactly as fbcon.c
*  does it. 24 bpp is the awkward one and is spelled out at store_pixel().
*
*  3. COLOUR
*
*  A caller passes an index, as it does in mode 13h, because the point of
*  mirroring vga.h is that drawing code can be written once. Mode 13h has a
*  hardware palette to translate it; a direct colour mode has none, so the
*  translation happens here, through a table of ready made PIXEL VALUES. It is
*  built when fbdraw_palette() is called and rebuilt whenever the surface
*  changes underneath it, and it means the colour arithmetic runs 256 times
*  per palette rather than once per pixel drawn.
*
*  4. THE FRAMEBUFFER IS NOT MEMORY
*
*  It is an uncached window onto the card across the bus. fbcon.c measured what
*  that costs and built its whole scrolling strategy around never reading one;
*  the same rule holds here without exception. Nothing in this file reads a
*  pixel back -- which is also why there is no fbdraw_pixel_at() to match
*  vga_pixel_at(), and why "transparent" text needs no read either: a glyph
*  writes its set bits and simply leaves the rest of the cell alone.
*
*  Everything else is deliberately the same as vgadraw.c, down to the argument
*  order and to what happens at the edges. Coordinates outside the surface are
*  clipped and drawn as far as they reach, never refused -- see the clipping
*  note at hspan(). The arithmetic is integer throughout, because the kernel
*  builds with -mgeneral-regs-only and never saves an FPU context on a task
*  switch, so a single float in here would either fail to compile or quietly
*  scramble another task's registers.
*/
#include <system.h>
#include <fbcon.h>
#include <fbdraw.h>
#include <vga.h>

/* Coordinates further out than this are ignored by the primitives that have to
*  do arithmetic on them (line, rect, circle, text), for the reason vgadraw.c
*  gives at the same constant: the line clipper multiplies one coordinate
*  difference by another, and with everything inside +/-16384 those products
*  stay below 2^30 and cannot overflow a 32-bit int. A wrong product there
*  turns a clip test into a wrong answer, and a wrong answer lets a write
*  escape the surface.
*
*  It also bounds the surface itself: fb_sync() refuses a mode wider or taller
*  than this, so sf_w and sf_h are safe to multiply and add as freely as the
*  coordinates are. No mode the bootloader can establish comes anywhere near --
*  1024x768 is the largest in vbe_res_table.
*
*  hspan() and vspan(), and with them fbdraw_hline() and fbdraw_vline(), are
*  free of the restriction: their clipping is written so that no intermediate
*  can overflow, and they are the two the rest of the file leans on. */
#define DRAW_COORD_MAX   16384

/* An 8x8 glyph blown up by more than this is not text any more, and the cap
*  is what keeps x + FONT_WIDTH * scale inside the range above for any x the
*  coordinate test admits: 8 * 512 = 4096, so the sum stays below 20480. */
#define DRAW_MAX_SCALE   512

/* Outcode bits for the line clipper (Cohen-Sutherland). */
#define CLIP_LEFT    1
#define CLIP_RIGHT   2
#define CLIP_ABOVE   4
#define CLIP_BELOW   8

/* --- The surface ---------------------------------------------------------
*
*  A cached copy of what fbcon.c reports, refreshed by fb_sync() at the top of
*  every public entry point and used by everything below it.
*
*  Caching rather than asking per pixel is the difference between a fill of
*  1024x768 costing 786432 stores and costing 786432 stores plus five function
*  calls and a division each. The refresh itself is those five calls once per
*  primitive, which even for a disc -- one hspan() per row, and hspan() does
*  NOT refresh -- is a few hundred nanoseconds against a screen full of
*  pixels. */
static uint8_t *sf_mem;        /* mapped framebuffer, 0 when there is none */
static int      sf_w;
static int      sf_h;
static uint32_t sf_pitch;      /* byte distance between two rows           */
static int      sf_bytes;      /* bytes per pixel, 1..4                    */
static uint32_t sf_bpp;        /* bits per pixel, only to spot a change    */

/* --- The palette ---------------------------------------------------------
*
*  Both halves of it. The triples are what the caller set and are kept because
*  the pixel values cannot be recovered from a packed pixel once the format
*  changes; the pixel values are what the drawing routines use, so that a fill
*  never converts a colour more than once.
*
*  Everything starts black, and deliberately so: a caller that draws without
*  setting a palette gets a black picture rather than a screen of whatever
*  sixteen colours this file happened to think were reasonable. fbdraw.h asks
*  callers to set the palette first for exactly that reason. */
static uint8_t  pal_rgb[FBDRAW_COLOURS][3];
static uint32_t pal[FBDRAW_COLOURS];

/* Rebuilds the pixel values from the triples. Called when the format the
*  triples were translated against is not the format now in effect -- which
*  covers the ordinary case as well as the awkward one: a palette set BEFORE
*  fbcon_activate() succeeded was translated against no format at all and came
*  out as 256 blacks, and this is what repairs it. */
static void palette_retranslate(void)
{
	int i;

	for(i = 0; i < FBDRAW_COLOURS; i++)
		pal[i] = fbcon_rgb(pal_rgb[i][0], pal_rgb[i][1], pal_rgb[i][2]);
}

/* Refreshes the cache and says whether there is anything to draw on.
*
*  The checks are not ceremony. fbcon.c has made all of them before mapping
*  anything, but this file is one bad number away from writing outside the
*  mapping, and repeating four comparisons per primitive is a great deal
*  cheaper than a fault in a page nobody owns:
*
*    - a pitch shorter than a row would put the right hand end of every row
*      into the next one, and the last row past the end of the mapping;
*    - a depth outside 1..4 bytes has no store_pixel() case;
*    - a surface larger than DRAW_COORD_MAX breaks the overflow argument the
*      clipping rests on.
*
*  A refusal clears sf_mem, so a later call re-reads everything rather than
*  drawing through a stale pointer. */
static int fb_sync(void)
{
	uint8_t *mem;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bpp;
	int bytes;

	mem    = fbcon_pixels();
	width  = fbcon_width();
	height = fbcon_height();
	pitch  = fbcon_pitch();
	bpp    = fbcon_bpp();
	bytes  = (int)((bpp + 7) / 8);

	if(mem == 0 || width == 0 || height == 0 || bytes < 1 || bytes > 4)
	{
		sf_mem = 0;
		return 0;
	}
	if(width > (uint32_t)DRAW_COORD_MAX || height > (uint32_t)DRAW_COORD_MAX)
	{
		sf_mem = 0;
		return 0;
	}
	if(pitch < width * (uint32_t)bytes)
	{
		sf_mem = 0;
		return 0;
	}

	/* A different mapping or a different depth means the pixel values in
	*  pal[] describe a format that is no longer there. */
	if(mem != sf_mem || bpp != sf_bpp)
	{
		sf_mem   = mem;
		sf_bpp   = bpp;
		sf_w     = (int)width;
		sf_h     = (int)height;
		sf_pitch = pitch;
		sf_bytes = bytes;
		palette_retranslate();
		return 1;
	}

	sf_w     = (int)width;
	sf_h     = (int)height;
	sf_pitch = pitch;
	sf_bytes = bytes;
	return 1;
}

/* First byte of a row. The only place a row address is formed, so that "read
*  the pitch, never compute it" is a property of one function rather than a
*  rule twelve call sites have to remember. */
static uint8_t *row_at(int y)
{
	return sf_mem + (uint32_t)y * sf_pitch;
}

/* --- Writing pixels ------------------------------------------------------
*
*  One pixel, at a pointer the caller has already bounded.
*
*  The four cases are the four depths the bootloader can hand over, and they
*  differ in more than width:
*
*    4  32 bpp. One store. The pitch of every mode that reports 32 bpp is a
*       multiple of four, so this is aligned in practice -- and on x86 an
*       unaligned dword store is legal anyway, merely slower, so a card with a
*       peculiar pitch costs speed here and not correctness.
*    3  24 bpp. THE AWKWARD ONE. Three bytes, so a pixel is not a type the
*       machine has and consecutive pixels are not aligned to anything at all
*       -- pixel 1 starts at byte 3. It has to be three separate byte stores,
*       low byte first, which is little endian order and therefore blue, green,
*       red in memory for the 8:8:8 layout at 16/8/0 that fbcon.c reports for
*       this depth. Writing it as a dword store of the low three bytes plus a
*       fixup, or as a masked read-modify-write, would be quicker in
*       instructions and wrong in both directions: the first scribbles over the
*       first byte of the NEXT pixel, and the second reads the framebuffer,
*       which this file does not do.
*    2  16 and 15 bpp both. They differ in where the channels sit, and that
*       difference has already been dealt with -- fbcon_rgb() packed it -- so
*       by the time a value arrives here the two are the same store.
*    1  8 bpp indexed. Not a mode our stage 2 will select (boot/vbe.inc leaves
*       8 bpp out on purpose), but fbcon.c accepts one from multiboot, and a
*       byte store is a poor reason to refuse a screen. */
static void store_pixel(uint8_t *p, uint32_t pixel)
{
	switch(sf_bytes)
	{
		case 4:
			*(uint32_t *)p = pixel;
			break;
		case 3:
			p[0] = (uint8_t)pixel;
			p[1] = (uint8_t)(pixel >> 8);
			p[2] = (uint8_t)(pixel >> 16);
			break;
		case 2:
			*(uint16_t *)p = (uint16_t)pixel;
			break;
		default:
			*p = (uint8_t)pixel;
			break;
	}
}

/* A run of pixels along one row, from a pointer and a length the caller has
*  already bounded. The workhorse: every fill, every horizontal line, every row
*  of a disc and every row of a glyph ends up here.
*
*  The depth is decided ONCE, outside the loop. That is the whole reason this
*  is not a loop over store_pixel(): a branch per pixel on a 1024 pixel row is
*  1024 branches to answer a question whose answer cannot change during the
*  call. */
static void span_raw(uint8_t *dst, int len, uint32_t pixel)
{
	int i;

	switch(sf_bytes)
	{
		case 4:
		{
			uint32_t *p = (uint32_t *)dst;
			for(i = 0; i < len; i++)
				p[i] = pixel;
			break;
		}
		case 3:
		{
			uint8_t *p = dst;
			for(i = 0; i < len; i++)
			{
				p[0] = (uint8_t)pixel;
				p[1] = (uint8_t)(pixel >> 8);
				p[2] = (uint8_t)(pixel >> 16);
				p += 3;
			}
			break;
		}
		case 2:
		{
			uint16_t *p = (uint16_t *)dst;
			for(i = 0; i < len; i++)
				p[i] = (uint16_t)pixel;
			break;
		}
		default:
		{
			uint8_t *p = dst;
			for(i = 0; i < len; i++)
				p[i] = (uint8_t)pixel;
			break;
		}
	}
}

/* --- Clipping ------------------------------------------------------------
*
*  A horizontal run, clipped to the surface. This is where the contract
*  vgadraw.c states is honoured, and it is honoured edge for edge:
*
*    len <= 0        nothing is drawn, and a zero width fill is not an error
*    y outside       nothing is drawn
*    x >= width      nothing is drawn
*    x < 0           the run is SHORTENED FROM THE LEFT and starts at column 0,
*                    so the part of it that is on screen appears in the columns
*                    it belongs in -- it is not shifted, and it does not wrap
*                    into the previous row, which is the failure a picture
*                    hides and a guard pixel catches
*    x + len > width the run is cut off at the right edge, again without
*                    wrapping into the next row
*
*  x + len is never formed. A caller may legitimately pass a length of a
*  million -- vga_disc() hands its spans in unbounded and so does this file's
*  -- and the sum would overflow before the test could reject it. Instead the
*  run is shortened from the left by adding the negative x, and from the right
*  against sf_w - x, where x is by then known to lie in 0..sf_w-1. Neither
*  expression can overflow whatever the caller passed.
*
*  Callers must have called fb_sync(): this reads the cache and does not
*  refresh it, which is what lets a disc of 768 rows refresh once instead of
*  768 times. */
static void hspan(int x, int y, int len, uint32_t pixel)
{
	if(len <= 0)
		return;
	if(y < 0 || y >= sf_h)
		return;
	if(x >= sf_w)
		return;

	if(x < 0)
	{
		len += x;               /* x is negative: this shortens the run */
		x = 0;
		if(len <= 0)
			return;         /* entirely left of the surface */
	}

	if(len > sf_w - x)
		len = sf_w - x;

	span_raw(row_at(y) + (uint32_t)x * sf_bytes, len, pixel);
}

/* The vertical counterpart, clipped the same way against the top and bottom.
*  Consecutive pixels of a column are a whole pitch apart, so there is no run
*  to hand to span_raw() -- but the depth still leaves the loop, for the same
*  reason it does there. */
static void vspan(int x, int y, int len, uint32_t pixel)
{
	uint8_t *p;
	int i;

	if(len <= 0)
		return;
	if(x < 0 || x >= sf_w)
		return;
	if(y >= sf_h)
		return;

	if(y < 0)
	{
		len += y;
		y = 0;
		if(len <= 0)
			return;
	}

	if(len > sf_h - y)
		len = sf_h - y;

	p = row_at(y) + (uint32_t)x * sf_bytes;

	switch(sf_bytes)
	{
		case 4:
			for(i = 0; i < len; i++)
			{
				*(uint32_t *)p = pixel;
				p += sf_pitch;
			}
			break;
		case 3:
			for(i = 0; i < len; i++)
			{
				p[0] = (uint8_t)pixel;
				p[1] = (uint8_t)(pixel >> 8);
				p[2] = (uint8_t)(pixel >> 16);
				p += sf_pitch;
			}
			break;
		case 2:
			for(i = 0; i < len; i++)
			{
				*(uint16_t *)p = (uint16_t)pixel;
				p += sf_pitch;
			}
			break;
		default:
			for(i = 0; i < len; i++)
			{
				*p = (uint8_t)pixel;
				p += sf_pitch;
			}
			break;
	}
}

/* One pixel, clipped. Anything off the surface is dropped, which is what lets
*  the circle walk mirror eight points per step without bounding any of them. */
static void point(int x, int y, uint32_t pixel)
{
	if(x < 0 || x >= sf_w || y < 0 || y >= sf_h)
		return;

	store_pixel(row_at(y) + (uint32_t)x * sf_bytes, pixel);
}

static int coord_sane(int v)
{
	return (v > -DRAW_COORD_MAX && v < DRAW_COORD_MAX);
}

/* --- Public interface ---------------------------------------------------- */

int fbdraw_available(void)
{
	return fb_sync();
}

int fbdraw_width(void)
{
	if(!fb_sync())
		return 0;
	return sf_w;
}

int fbdraw_height(void)
{
	if(!fb_sync())
		return 0;
	return sf_h;
}

/* Sets one palette entry. Nothing reaches the hardware -- a direct colour mode
*  has no DAC to program -- so this only fills in the table the primitives
*  translate through, and pixels already on the screen keep the colour they
*  were drawn in. fbdraw.h says so; it is the one visible difference from
*  vga_palette(), where changing an entry recolours the screen.
*
*  Components are 0..255 here, the full range a channel can carry, and NOT the
*  0..63 the VGA DAC takes. Code that programs both palettes from one table has
*  to scale, or every colour it sets on this path comes out at a quarter of the
*  brightness it has in mode 13h. */
void fbdraw_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	pal_rgb[index][0] = r;
	pal_rgb[index][1] = g;
	pal_rgb[index][2] = b;

	/* Translated now, so that a fill does not have to. When there is no
	*  surface yet fbcon_rgb() answers 0, and the retranslate in fb_sync()
	*  puts it right the moment there is one. */
	pal[index] = fbcon_rgb(r, g, b);
}

/* Fills the whole visible surface, margin included.
*
*  Row by row rather than one flat run: the rows of a padded mode are not
*  contiguous, and a single span across the padding would write into it. The
*  padding is off screen and harmless to look at, but on the last row it lies
*  outside the mapping. */
void fbdraw_clear(uint8_t colour)
{
	uint32_t pixel;
	uint8_t *dst;
	int y;

	if(!fb_sync())
		return;

	pixel = pal[colour];
	dst = sf_mem;

	for(y = 0; y < sf_h; y++)
	{
		span_raw(dst, sf_w, pixel);
		dst += sf_pitch;
	}
}

void fbdraw_pixel(int x, int y, uint8_t colour)
{
	if(!fb_sync())
		return;

	point(x, y, pal[colour]);
}

void fbdraw_hline(int x, int y, int len, uint8_t colour)
{
	if(!fb_sync())
		return;

	hspan(x, y, len, pal[colour]);
}

void fbdraw_vline(int x, int y, int len, uint8_t colour)
{
	if(!fb_sync())
		return;

	vspan(x, y, len, pal[colour]);
}

/* Which edges a point lies beyond, or 0 for a point on the surface. */
static int outcode(int x, int y)
{
	int code = 0;

	if(x < 0)
		code |= CLIP_LEFT;
	else if(x > sf_w - 1)
		code |= CLIP_RIGHT;

	if(y < 0)
		code |= CLIP_ABOVE;
	else if(y > sf_h - 1)
		code |= CLIP_BELOW;

	return code;
}

/* Integer division rounded to nearest rather than truncated towards zero. The
*  clipper picks a pixel to stand for a point that really lies between two of
*  them, and truncation always errs the same way; rounding halves the error and
*  stops it leaning consistently in one direction. */
static int div_round(int num, int den)
{
	if(den < 0)
	{
		num = -num;
		den = -den;
	}

	if(num >= 0)
		return (num + den / 2) / den;

	return -((-num + den / 2) / den);
}

/* Cohen-Sutherland, as in vgadraw.c and for the same reasons: it trims the
*  segment before rastering, so a line from (-9000,-9000) to (9000,9000) costs
*  the pixels it actually draws and not eighteen thousand clipped-away steps.
*
*  Every intersection is computed from the ORIGINAL endpoints, never from one a
*  previous pass has already moved and rounded; clipping the second end against
*  an already-rounded first end tilts the whole segment, and the tilt shows as
*  a visible bend at the far end. The pass count is capped because a rounded
*  point can land one pixel outside the edge it was meant to sit on, which the
*  next pass clips again -- harmless, but the one way this loop could cycle.
*
*  Neither divisor can be zero: fbdraw_line() sends the purely horizontal and
*  purely vertical cases to hspan()/vspan() before this is reached. */
static int clip_line(int *px0, int *py0, int *px1, int *py1)
{
	int ox0 = *px0, oy0 = *py0, ox1 = *px1, oy1 = *py1;
	int dx = ox1 - ox0, dy = oy1 - oy0;
	int x0 = ox0, y0 = oy0, x1 = ox1, y1 = oy1;
	int c0 = outcode(x0, y0);
	int c1 = outcode(x1, y1);
	int c, x, y, pass;

	for(pass = 0; pass < 8; pass++)
	{
		if((c0 | c1) == 0)
		{
			*px0 = x0; *py0 = y0;
			*px1 = x1; *py1 = y1;
			return 1;
		}

		if((c0 & c1) != 0)
			return 0;       /* both ends beyond the same edge */

		c = (c0 != 0) ? c0 : c1;

		if((c & CLIP_ABOVE) != 0)
		{
			y = 0;
			x = ox0 + div_round(dx * (y - oy0), dy);
		}
		else if((c & CLIP_BELOW) != 0)
		{
			y = sf_h - 1;
			x = ox0 + div_round(dx * (y - oy0), dy);
		}
		else if((c & CLIP_LEFT) != 0)
		{
			x = 0;
			y = oy0 + div_round(dy * (x - ox0), dx);
		}
		else
		{
			x = sf_w - 1;
			y = oy0 + div_round(dy * (x - ox0), dx);
		}

		if(c == c0)
		{
			x0 = x;
			y0 = y;
			c0 = outcode(x0, y0);
		}
		else
		{
			x1 = x;
			y1 = y;
			c1 = outcode(x1, y1);
		}
	}

	return 0;
}

/* Bresenham, all eight octants.
*
*  The two degenerate cases are handed to hspan()/vspan() first: a horizontal
*  run becomes one span write instead of hundreds of conditional steps, and a
*  vertical one skips the error term entirely.
*
*  After clipping both endpoints are on the surface and Bresenham never leaves
*  the bounding box of its endpoints, so every pixel is inside. The bounds test
*  in point() is therefore redundant -- and stays, because it costs two
*  compares against values already in registers and it is the difference
*  between a clipper bug being a cosmetic glitch and a clipper bug being a
*  write into somebody else's page.
*
*  The depth switch is inside point() here, one branch per pixel, and stays
*  there: a line is at most a screen diagonal, about 1280 pixels, and four
*  copies of this loop to hoist one predictable branch out of it would be four
*  copies of the error term arithmetic to get wrong. The fills, which are three
*  orders of magnitude more pixels, are the ones that got the hoisting. */
void fbdraw_line(int x0, int y0, int x1, int y1, uint8_t colour)
{
	uint32_t pixel;
	int dx, dy, sx, sy, err, e2;

	if(!fb_sync())
		return;
	if(!coord_sane(x0) || !coord_sane(y0) || !coord_sane(x1) || !coord_sane(y1))
		return;

	pixel = pal[colour];

	if(y0 == y1)
	{
		if(x0 <= x1)
			hspan(x0, y0, x1 - x0 + 1, pixel);
		else
			hspan(x1, y0, x0 - x1 + 1, pixel);
		return;
	}

	if(x0 == x1)
	{
		if(y0 <= y1)
			vspan(x0, y0, y1 - y0 + 1, pixel);
		else
			vspan(x0, y1, y0 - y1 + 1, pixel);
		return;
	}

	if(!clip_line(&x0, &y0, &x1, &y1))
		return;

	dx = x1 - x0;
	if(dx < 0)
		dx = -dx;
	dy = y1 - y0;
	if(dy < 0)
		dy = -dy;

	sx = (x0 < x1) ? 1 : -1;
	sy = (y0 < y1) ? 1 : -1;
	err = dx - dy;

	for(;;)
	{
		point(x0, y0, pixel);

		if(x0 == x1 && y0 == y1)
			break;

		/* One test decides the x step and one the y step; a diagonal step
		*  is both firing on the same pass, which is what covers the
		*  octants around 45 degrees without a special case. */
		e2 = err + err;
		if(e2 > -dy)
		{
			err -= dy;
			x0 += sx;
		}
		if(e2 < dx)
		{
			err += dx;
			y0 += sy;
		}
	}
}

/* Outlined rectangle: (x,y) is the top left corner and w and h are the outer
*  dimensions, so the right edge sits at x + w - 1.
*
*  Every one of the four sides clips itself, which is what makes a rectangle
*  straddling two edges come out right -- the two visible sides are drawn
*  clipped and the two invisible ones are dropped by the length tests inside
*  hspan()/vspan(). The vertical sides skip the rows the horizontal ones
*  already covered, so no corner pixel is written twice. */
void fbdraw_rect(int x, int y, int w, int h, uint8_t colour)
{
	uint32_t pixel;

	if(w <= 0 || h <= 0)
		return;
	if(!coord_sane(x) || !coord_sane(y) || !coord_sane(w) || !coord_sane(h))
		return;
	if(!fb_sync())
		return;

	pixel = pal[colour];

	hspan(x, y, w, pixel);
	if(h > 1)
		hspan(x, y + h - 1, w, pixel);

	if(h > 2)
	{
		vspan(x, y + 1, h - 2, pixel);
		if(w > 1)
			vspan(x + w - 1, y + 1, h - 2, pixel);
	}
}

/* Filled rectangle.
*
*  The vertical extent is clipped here so that a height of ten thousand does
*  not mean ten thousand span calls that each decide they have nothing to do;
*  the horizontal extent is left to hspan(), which has to test it anyway.
*
*  The rows are walked with a pointer rather than through hspan() once the
*  horizontal clip has been settled, because the horizontal answer is the same
*  for every row of a rectangle -- there is no sense recomputing it h times. */
void fbdraw_fill(int x, int y, int w, int h, uint8_t colour)
{
	uint32_t pixel;
	uint8_t *dst;
	int i;

	if(w <= 0 || h <= 0)
		return;
	if(!coord_sane(x) || !coord_sane(y) || !coord_sane(w) || !coord_sane(h))
		return;
	if(!fb_sync())
		return;

	if(y < 0)
	{
		h += y;
		y = 0;
		if(h <= 0)
			return;
	}
	if(y >= sf_h)
		return;
	if(h > sf_h - y)
		h = sf_h - y;

	if(x >= sf_w)
		return;
	if(x < 0)
	{
		w += x;
		x = 0;
		if(w <= 0)
			return;
	}
	if(w > sf_w - x)
		w = sf_w - x;

	pixel = pal[colour];
	dst = row_at(y) + (uint32_t)x * sf_bytes;

	for(i = 0; i < h; i++)
	{
		span_raw(dst, w, pixel);
		dst += sf_pitch;
	}
}

/* True when a circle of this radius cannot touch the surface at all, so the
*  midpoint loop can be skipped rather than run for thousands of steps that
*  each clip away to nothing. */
static int circle_offscreen(int cx, int cy, int radius)
{
	return (cx + radius < 0 || cx - radius > sf_w - 1 ||
	        cy + radius < 0 || cy - radius > sf_h - 1);
}

/* Outlined circle, midpoint algorithm -- the integer relative of Bresenham.
*
*  It walks an eighth of the circle, from the top towards the 45 degree
*  diagonal, and mirrors each step into the other seven octants. The decision
*  variable d tracks whether the ideal circle has fallen below the midpoint
*  between the two candidate pixels; when it has, y steps in.
*
*  Clipping is per point, through point(). That is the right trade here: the
*  eight mirrored points of one step land in eight different places, so there
*  is no run to clip as a whole, and the loop is at most radius steps long. A
*  circle centred off screen simply has most of its points dropped. */
void fbdraw_circle(int cx, int cy, int radius, uint8_t colour)
{
	uint32_t pixel;
	int x, y, d;

	if(radius < 0)
		return;
	if(!coord_sane(cx) || !coord_sane(cy) || !coord_sane(radius))
		return;
	if(!fb_sync())
		return;

	pixel = pal[colour];

	if(radius == 0)
	{
		point(cx, cy, pixel);
		return;
	}

	if(circle_offscreen(cx, cy, radius))
		return;

	x = 0;
	y = radius;
	d = 1 - radius;

	while(x <= y)
	{
		point(cx + x, cy + y, pixel);
		point(cx - x, cy + y, pixel);
		point(cx + x, cy - y, pixel);
		point(cx - x, cy - y, pixel);
		point(cx + y, cy + x, pixel);
		point(cx - y, cy + x, pixel);
		point(cx + y, cy - x, pixel);
		point(cx - y, cy - x, pixel);

		if(d < 0)
		{
			d += 2 * x + 3;
		}
		else
		{
			d += 2 * (x - y) + 5;
			y--;
		}
		x++;
	}
}

/* Filled circle. The same walk, but each step draws four spans instead of
*  eight points: the two rows at +/-y the walked octant produces, and the two
*  at +/-x its mirror produces. Together they cover every row of the disc, some
*  of them twice, which costs a repeated span write and no correctness.
*
*  All four go through hspan(), so a disc centred off screen -- where cx - x is
*  negative and the span has to be trimmed on the left -- is clipped by the
*  same code as everything else. */
void fbdraw_disc(int cx, int cy, int radius, uint8_t colour)
{
	uint32_t pixel;
	int x, y, d;

	if(radius < 0)
		return;
	if(!coord_sane(cx) || !coord_sane(cy) || !coord_sane(radius))
		return;
	if(!fb_sync())
		return;

	pixel = pal[colour];

	if(radius == 0)
	{
		point(cx, cy, pixel);
		return;
	}

	if(circle_offscreen(cx, cy, radius))
		return;

	x = 0;
	y = radius;
	d = 1 - radius;

	while(x <= y)
	{
		hspan(cx - x, cy + y, 2 * x + 1, pixel);
		hspan(cx - x, cy - y, 2 * x + 1, pixel);
		hspan(cx - y, cy + x, 2 * y + 1, pixel);
		hspan(cx - y, cy - x, 2 * y + 1, pixel);

		if(d < 0)
		{
			d += 2 * x + 3;
		}
		else
		{
			d += 2 * (x - y) + 5;
			y--;
		}
		x++;
	}
}

/* One glyph of the built-in 8x8 font at (x,y), which is its top left corner,
*  every pixel of it blown up into a scale x scale block. The font is the same
*  table vga_char() draws with, so a caption reads the same on both paths --
*  which is the point of it: 8x8 is legible at 320x200 and is a smudge at
*  1024x768, and the scale is what makes the same text usable on both.
*
*  The background is always left alone. vga_char() takes a bg and a value that
*  means "transparent"; this one has no bg parameter at all, so there is no
*  choice to make -- and no reason to want one, since not touching the
*  background is also the cheaper of the two and the only one that never reads
*  the framebuffer.
*
*  The font covers 0..127. A char with the high bit set -- a byte of a UTF-8
*  sequence, or a code page character out of a file -- has no glyph, so it is
*  drawn as '?'. Substituting rather than skipping keeps the fact that
*  something was lost visible, and keeps a run of them from looking like an
*  unexplained gap.
*
*  Set bits are coalesced into runs before being drawn, so the crossbar of an
*  'E' is one span of five blocks rather than five separate ones. At scale 1
*  that saves the call overhead; at scale 6, where a run is 30 pixels wide and
*  6 rows deep, it is the difference between 30 span calls and 6. */
void fbdraw_char(int x, int y, char c, int scale, uint8_t colour)
{
	const uint8_t *glyph;
	uint32_t pixel;
	unsigned int bits;
	int index, row, col, run, i, py;

	if(scale < 1 || scale > DRAW_MAX_SCALE)
		return;
	if(!coord_sane(x) || !coord_sane(y))
		return;
	if(!fb_sync())
		return;

	/* Whole cell off the surface: nothing to do. Tested before the glyph is
	*  looked up, because this is the common case in a string that runs off
	*  an edge and it should cost as little as possible. */
	if(x >= sf_w || y >= sf_h)
		return;
	if(x + FONT_WIDTH * scale <= 0 || y + FONT_HEIGHT * scale <= 0)
		return;

	index = (int)(unsigned char)c;
	if(index > 127)
		index = '?';
	glyph = font8x8[index];
	pixel = pal[colour];

	for(row = 0; row < FONT_HEIGHT; row++)
	{
		bits = glyph[row];
		if(bits == 0)
			continue;       /* an empty glyph row is most of a font */

		py = y + row * scale;
		col = 0;

		while(col < FONT_WIDTH)
		{
			/* One byte per row, most significant bit leftmost. */
			if((bits & (0x80u >> col)) == 0)
			{
				col++;
				continue;
			}

			run = 1;
			while(col + run < FONT_WIDTH &&
			      (bits & (0x80u >> (col + run))) != 0)
				run++;

			/* The block, scale rows deep. hspan() clips each row on
			*  its own, so a glyph hanging over any edge -- or over
			*  two of them -- needs no case of its own here. */
			for(i = 0; i < scale; i++)
				hspan(x + col * scale, py + i,
				      run * scale, pixel);

			col += run;
		}
	}
}

/* A run of glyphs on one line, FONT_WIDTH * scale apart.
*
*  It stops at the right edge instead of wrapping: wrapping into the next row
*  of pixels would put the tail of the string a cell below the head and one
*  character in from the left, which looks like a bug rather than a line break.
*  A caller that wants wrapping knows where its own line breaks belong.
*
*  Characters that start left of the surface are still stepped over, so the
*  visible part of a string with a negative x stays in the columns it
*  belongs in. */
void fbdraw_string(int x, int y, const char *s, int scale, uint8_t colour)
{
	if(s == 0)
		return;
	if(scale < 1 || scale > DRAW_MAX_SCALE)
		return;
	if(!coord_sane(x) || !coord_sane(y))
		return;
	if(!fb_sync())
		return;
	if(y >= sf_h || y + FONT_HEIGHT * scale <= 0)
		return;

	while(*s != '\0')
	{
		if(x >= sf_w)
			break;          /* right edge reached, no wrap */

		fbdraw_char(x, y, *s, scale, colour);
		x += FONT_WIDTH * scale;

		/* A string long enough to walk past the coordinate limit would
		*  break the overflow argument the clipping rests on. It cannot
		*  produce a visible pixel from here on either, since x only
		*  grows. */
		if(!coord_sane(x))
			break;

		s++;
	}
}
