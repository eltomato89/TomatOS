/* TomatOS - VGA mode 13h drawing primitives
*  Desc: Pixels, lines, rectangles, circles and text on the 320x200 linear
*        framebuffer that vga.c hands out.
*
*  Two rules run through the whole file.
*
*  First, every entry point asks vga_framebuffer() for the framebuffer and
*  gives up when it returns 0, which is what happens outside graphics mode.
*  Writing through that null pointer would fault rather than corrupt anything
*  -- the null page is deliberately unmapped -- but a caller that draws a
*  splash screen before the mode is set deserves a no-op, not a page fault.
*
*  Second, everything clips. The header promises callers may pass coordinates
*  that lie partly or wholly off screen, so the clipping happens here, once,
*  instead of in every caller. That is not a nicety: the framebuffer is
*  exactly 64000 bytes and the heap keeps its own bookkeeping right behind
*  whatever it hands out, so a single unchecked write past the end corrupts a
*  data structure somewhere else entirely and the crash surfaces minutes
*  later in code that did nothing wrong.
*
*  The arithmetic is integer throughout. The kernel builds with
*  -mgeneral-regs-only and never saves an FPU context on a task switch, so a
*  single float in here would either fail to compile or, worse, quietly
*  scramble another task's registers.
*/
#include <system.h>
#include <vga.h>

/* A bg of 0xFF means "leave the background alone", as documented in vga.h.
*  It costs colour 255 as a text background, which is the usual bargain for
*  transparency in a palette mode. */
#define VGA_TRANSPARENT  0xFF

/* Coordinates further out than this are ignored by the primitives that have
*  to do arithmetic on them (line, rect, circle, text).
*
*  The clipper multiplies a coordinate difference by another one, and the
*  circle steps 2*x terms; with everything inside +/-16384 those products stay
*  below 2^30 and cannot overflow a 32-bit int, which is what would turn a
*  clip test into a wrong answer and let a write escape the framebuffer. A
*  screen 320 pixels wide has no honest use for a coordinate fifty times its
*  own size, so treating those as a caller bug and drawing nothing loses no
*  real picture.
*
*  vga_hline() and vga_vline() are free of this restriction: their clipping is
*  written so that no intermediate can overflow, and they are the two the rest
*  of the file leans on. */
#define DRAW_COORD_MAX   16384

/* Outcode bits for the line clipper (Cohen-Sutherland). */
#define CLIP_LEFT    1
#define CLIP_RIGHT   2
#define CLIP_ABOVE   4
#define CLIP_BELOW   8

static int coord_sane(int v)
{
	return (v > -DRAW_COORD_MAX && v < DRAW_COORD_MAX);
}

/* Fills the whole visible screen. */
void vga_clear(uint8_t colour)
{
	uint8_t *fb = vga_framebuffer();

	if (fb == 0)
		return;

	/* memset() takes the value as a char, so a colour above 127 arrives
	*  here as a negative char. The byte that lands in the framebuffer is
	*  the same either way -- only the type on the way in is signed. */
	memset(fb, (char)colour, (size_t)VGA_PIXELS);
}

void vga_pixel(int x, int y, uint8_t colour)
{
	uint8_t *fb = vga_framebuffer();

	if (fb == 0)
		return;
	if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT)
		return;

	fb[y * VGA_WIDTH + x] = colour;
}

/* Reads a pixel back. Anything off screen, and every call outside graphics
*  mode, reads as colour 0 -- the same answer as an untouched screen, so a
*  caller that samples a neighbourhood around an edge needs no special case. */
uint8_t vga_pixel_at(int x, int y)
{
	uint8_t *fb = vga_framebuffer();

	if (fb == 0)
		return 0;
	if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT)
		return 0;

	return fb[y * VGA_WIDTH + x];
}

/* The workhorse: every fill in the file ends up here, so it is a memset of a
*  row segment rather than a loop over pixels.
*
*  The clipping is deliberately written without ever forming x + len, which is
*  the expression that would overflow for a wild length. Instead the run is
*  shortened from the left by adding the (negative) x, and from the right
*  against VGA_WIDTH - x, where x is by then known to be 0..319. */
void vga_hline(int x, int y, int len, uint8_t colour)
{
	uint8_t *fb = vga_framebuffer();

	if (fb == 0)
		return;
	if (len <= 0)
		return;
	if (y < 0 || y >= VGA_HEIGHT)
		return;
	if (x >= VGA_WIDTH)
		return;

	if (x < 0)
	{
		len += x;		/* x is negative: this shortens the run */
		x = 0;
		if (len <= 0)
			return;		/* entirely left of the screen */
	}

	if (len > VGA_WIDTH - x)
		len = VGA_WIDTH - x;

	memset(fb + y * VGA_WIDTH + x, (char)colour, (size_t)len);
}

/* The vertical counterpart. No memset here -- consecutive pixels of a column
*  are 320 bytes apart -- but the clipping is the same shape. */
void vga_vline(int x, int y, int len, uint8_t colour)
{
	uint8_t *fb = vga_framebuffer();
	uint8_t *p;
	int i;

	if (fb == 0)
		return;
	if (len <= 0)
		return;
	if (x < 0 || x >= VGA_WIDTH)
		return;
	if (y >= VGA_HEIGHT)
		return;

	if (y < 0)
	{
		len += y;
		y = 0;
		if (len <= 0)
			return;
	}

	if (len > VGA_HEIGHT - y)
		len = VGA_HEIGHT - y;

	p = fb + y * VGA_WIDTH + x;
	for (i = 0; i < len; i++)
	{
		*p = colour;
		p += VGA_WIDTH;
	}
}

/* Which edges a point lies beyond, or 0 for a point on the screen. */
static int vga_outcode(int x, int y)
{
	int code = 0;

	if (x < 0)
		code |= CLIP_LEFT;
	else if (x > VGA_WIDTH - 1)
		code |= CLIP_RIGHT;

	if (y < 0)
		code |= CLIP_ABOVE;
	else if (y > VGA_HEIGHT - 1)
		code |= CLIP_BELOW;

	return code;
}

/* Integer division rounded to the nearest whole number rather than truncated
*  towards zero, which is what C would do. The clipper picks a pixel to stand
*  for a point that really lies between two of them, and truncation always
*  errs the same way; rounding halves the error and, more to the point, stops
*  it from leaning consistently in one direction. */
static int vga_div_round(int num, int den)
{
	if (den < 0)
	{
		num = -num;
		den = -den;
	}

	if (num >= 0)
		return (num + den / 2) / den;

	return -((-num + den / 2) / den);
}

/* Cohen-Sutherland: trims a segment to the screen rectangle and returns 1 if
*  anything is left of it, 0 if the whole segment is invisible.
*
*  Clipping the endpoints before rastering, rather than testing every pixel
*  inside the Bresenham loop, is what keeps a line from (-9000,-9000) to
*  (9000,9000) from costing eighteen thousand iterations to draw 200 pixels.
*
*  Every intersection is computed from the original endpoints, never from an
*  endpoint an earlier pass has already moved. That distinction matters more
*  than it looks: each computed point is rounded to a whole pixel, so clipping
*  the second end against a line that starts at an already-rounded first end
*  tilts the whole segment, and the tilt shows up as a visible bend at the far
*  end. Measured on random lines, that compounding cost a little over two
*  pixels of deviation; holding the original direction brings it back inside
*  one, which is the best a raster line can do anyway.
*
*  The rounding can still place a computed point one pixel outside the edge it
*  was meant to sit on. That is harmless -- the next pass recomputes the
*  outcode and clips it again -- but it is also the one way this loop could
*  cycle, so the number of passes is capped. Four is the theoretical maximum,
*  two per endpoint; at eight we give up and draw nothing, which is a lost
*  line at worst and never a stray write.
*
*  The one visible cost of rounding is that an endpoint can land half a pixel
*  inside the edge and the line then stops one pixel short of the border --
*  about one clipped line in a hundred, and only ever in the outermost pixel
*  or two of the screen. Nothing is ever dropped anywhere else along the run.
*
*  Neither divisor can be zero: vga_line() sends the purely horizontal and
*  purely vertical cases to vga_hline()/vga_vline() before this is reached, so
*  the original dx and dy are both non-zero here. */
static int vga_clip_line(int *px0, int *py0, int *px1, int *py1)
{
	int ox0 = *px0, oy0 = *py0, ox1 = *px1, oy1 = *py1;
	int dx = ox1 - ox0, dy = oy1 - oy0;
	int x0 = ox0, y0 = oy0, x1 = ox1, y1 = oy1;
	int c0 = vga_outcode(x0, y0);
	int c1 = vga_outcode(x1, y1);
	int c, x, y, pass;

	for (pass = 0; pass < 8; pass++)
	{
		if ((c0 | c1) == 0)
		{
			/* Both ends on screen: done. */
			*px0 = x0; *py0 = y0;
			*px1 = x1; *py1 = y1;
			return 1;
		}

		if ((c0 & c1) != 0)
			return 0;	/* both ends beyond the same edge */

		/* Pick an endpoint that is still outside and pull it in to the
		*  first edge it violates. */
		c = (c0 != 0) ? c0 : c1;

		if ((c & CLIP_ABOVE) != 0)
		{
			y = 0;
			x = ox0 + vga_div_round(dx * (y - oy0), dy);
		}
		else if ((c & CLIP_BELOW) != 0)
		{
			y = VGA_HEIGHT - 1;
			x = ox0 + vga_div_round(dx * (y - oy0), dy);
		}
		else if ((c & CLIP_LEFT) != 0)
		{
			x = 0;
			y = oy0 + vga_div_round(dy * (x - ox0), dx);
		}
		else
		{
			x = VGA_WIDTH - 1;
			y = oy0 + vga_div_round(dy * (x - ox0), dx);
		}

		if (c == c0)
		{
			x0 = x;
			y0 = y;
			c0 = vga_outcode(x0, y0);
		}
		else
		{
			x1 = x;
			y1 = y;
			c1 = vga_outcode(x1, y1);
		}
	}

	return 0;
}

/* Bresenham, all eight octants.
*
*  The two degenerate cases are handed to vga_hline()/vga_vline() before any
*  of this runs: a horizontal run is one memset instead of 320 conditional
*  steps, and a vertical one skips the error term entirely.
*
*  After clipping both endpoints are on screen, and Bresenham never leaves the
*  bounding box of its endpoints, so every pixel is inside. The bounds test in
*  the loop is therefore redundant -- and stays anyway, because it costs two
*  compares against a value already in a register and it is the difference
*  between a clipper bug being a cosmetic glitch and a clipper bug being heap
*  corruption. */
void vga_line(int x0, int y0, int x1, int y1, uint8_t colour)
{
	uint8_t *fb;
	int dx, dy, sx, sy, err, e2;

	fb = vga_framebuffer();
	if (fb == 0)
		return;
	if (!coord_sane(x0) || !coord_sane(y0) || !coord_sane(x1) || !coord_sane(y1))
		return;

	if (y0 == y1)
	{
		/* Horizontal: hand it to the memset path. */
		if (x0 <= x1)
			vga_hline(x0, y0, x1 - x0 + 1, colour);
		else
			vga_hline(x1, y0, x0 - x1 + 1, colour);
		return;
	}

	if (x0 == x1)
	{
		if (y0 <= y1)
			vga_vline(x0, y0, y1 - y0 + 1, colour);
		else
			vga_vline(x0, y1, y0 - y1 + 1, colour);
		return;
	}

	if (!vga_clip_line(&x0, &y0, &x1, &y1))
		return;

	dx = x1 - x0;
	if (dx < 0)
		dx = -dx;
	dy = y1 - y0;
	if (dy < 0)
		dy = -dy;

	sx = (x0 < x1) ? 1 : -1;
	sy = (y0 < y1) ? 1 : -1;
	err = dx - dy;

	for (;;)
	{
		if (x0 >= 0 && x0 < VGA_WIDTH && y0 >= 0 && y0 < VGA_HEIGHT)
			fb[y0 * VGA_WIDTH + x0] = colour;

		if (x0 == x1 && y0 == y1)
			break;

		/* One test decides the x step and one the y step; a diagonal
		*  step is both of them firing on the same pass, which is what
		*  covers the octants around 45 degrees without a special
		*  case. */
		e2 = err + err;
		if (e2 > -dy)
		{
			err -= dy;
			x0 += sx;
		}
		if (e2 < dx)
		{
			err += dx;
			y0 += sy;
		}
	}
}

/* Outlined rectangle: (x,y) is the top left corner, w and h are the outer
*  dimensions, so the right edge sits at x + w - 1.
*
*  Every one of the four sides clips itself, which is what makes a rectangle
*  straddling two edges come out right -- the two visible sides are drawn
*  clipped and the two invisible ones are dropped by the length tests inside
*  hline/vline. The vertical sides skip the rows the horizontal ones already
*  covered, so no corner pixel is written twice. */
void vga_rect(int x, int y, int w, int h, uint8_t colour)
{
	if (w <= 0 || h <= 0)
		return;
	if (!coord_sane(x) || !coord_sane(y) || !coord_sane(w) || !coord_sane(h))
		return;

	vga_hline(x, y, w, colour);
	if (h > 1)
		vga_hline(x, y + h - 1, w, colour);

	if (h > 2)
	{
		vga_vline(x, y + 1, h - 2, colour);
		if (w > 1)
			vga_vline(x + w - 1, y + 1, h - 2, colour);
	}
}

/* Filled rectangle.
*
*  The vertical extent is clipped here so that a height of ten thousand does
*  not mean ten thousand calls that each decide they have nothing to do; the
*  horizontal extent is left to vga_hline(), which has to test it anyway. */
void vga_fill(int x, int y, int w, int h, uint8_t colour)
{
	int i;

	if (w <= 0 || h <= 0)
		return;
	if (!coord_sane(x) || !coord_sane(y) || !coord_sane(w) || !coord_sane(h))
		return;

	if (y < 0)
	{
		h += y;
		y = 0;
		if (h <= 0)
			return;
	}
	if (y >= VGA_HEIGHT)
		return;
	if (h > VGA_HEIGHT - y)
		h = VGA_HEIGHT - y;

	for (i = 0; i < h; i++)
		vga_hline(x, y + i, w, colour);
}

/* True when a circle of this radius cannot touch the screen at all, so the
*  midpoint loop can be skipped rather than run for thousands of steps that
*  each clip away to nothing. */
static int vga_circle_offscreen(int cx, int cy, int radius)
{
	return (cx + radius < 0 || cx - radius > VGA_WIDTH - 1 ||
	        cy + radius < 0 || cy - radius > VGA_HEIGHT - 1);
}

/* Outlined circle, midpoint algorithm -- the integer relative of Bresenham.
*
*  It walks an eighth of the circle, from the top towards the 45 degree
*  diagonal, and mirrors each step into the other seven octants. The decision
*  variable d tracks whether the ideal circle has fallen below the midpoint
*  between the two candidate pixels; when it has, y steps in.
*
*  Clipping is per point, through vga_pixel(). That is the right trade here:
*  the eight mirrored points of one step land in eight different places, so
*  there is no run to clip as a whole, and the loop is at most radius steps
*  long. A circle centred off screen simply has most of its points dropped. */
void vga_circle(int cx, int cy, int radius, uint8_t colour)
{
	int x, y, d;

	if (radius < 0)
		return;
	if (!coord_sane(cx) || !coord_sane(cy) || !coord_sane(radius))
		return;

	if (radius == 0)
	{
		vga_pixel(cx, cy, colour);
		return;
	}

	if (vga_circle_offscreen(cx, cy, radius))
		return;

	x = 0;
	y = radius;
	d = 1 - radius;

	while (x <= y)
	{
		vga_pixel(cx + x, cy + y, colour);
		vga_pixel(cx - x, cy + y, colour);
		vga_pixel(cx + x, cy - y, colour);
		vga_pixel(cx - x, cy - y, colour);
		vga_pixel(cx + y, cy + x, colour);
		vga_pixel(cx - y, cy + x, colour);
		vga_pixel(cx + y, cy - x, colour);
		vga_pixel(cx - y, cy - x, colour);

		if (d < 0)
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

/* Filled circle. Same walk as vga_circle(), but each step draws four spans
*  instead of eight points: the two rows at +/-y that the octant being walked
*  produces, and the two rows at +/-x that its mirror produces. Together they
*  cover every row of the disc, some of them twice, which costs a repeated
*  memset and no correctness.
*
*  All four spans go through vga_hline(), so the clipping -- including a disc
*  centred off screen, where cx - x is negative and the span has to be trimmed
*  on the left -- is the same clipping the rest of the file uses. */
void vga_disc(int cx, int cy, int radius, uint8_t colour)
{
	int x, y, d;

	if (radius < 0)
		return;
	if (!coord_sane(cx) || !coord_sane(cy) || !coord_sane(radius))
		return;

	if (radius == 0)
	{
		vga_pixel(cx, cy, colour);
		return;
	}

	if (vga_circle_offscreen(cx, cy, radius))
		return;

	x = 0;
	y = radius;
	d = 1 - radius;

	while (x <= y)
	{
		vga_hline(cx - x, cy + y, 2 * x + 1, colour);
		vga_hline(cx - x, cy - y, 2 * x + 1, colour);
		vga_hline(cx - y, cy + x, 2 * y + 1, colour);
		vga_hline(cx - y, cy - x, 2 * y + 1, colour);

		if (d < 0)
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

/* One glyph of the built-in 8x8 font at (x,y), which is its top left corner.
*
*  The font covers 0..127. A char with the high bit set -- a byte of a UTF-8
*  sequence, or a code page character that came in from a file -- has no glyph
*  to draw, so it is drawn as '?'. Substituting rather than skipping keeps the
*  fact that something was lost visible on screen, and it keeps a string of
*  them from looking like an unexplained gap.
*
*  A bg of VGA_TRANSPARENT leaves the unset bits of the glyph untouched, so
*  text can go over a picture; any other bg paints the full 8x8 cell. */
void vga_char(int x, int y, char c, uint8_t fg, uint8_t bg)
{
	uint8_t *fb;
	const uint8_t *glyph;
	int index, row, col, px, py;
	uint8_t bits;

	fb = vga_framebuffer();
	if (fb == 0)
		return;
	if (!coord_sane(x) || !coord_sane(y))
		return;

	/* Whole cell off screen: nothing to do. */
	if (x >= VGA_WIDTH || y >= VGA_HEIGHT)
		return;
	if (x + FONT_WIDTH <= 0 || y + FONT_HEIGHT <= 0)
		return;

	index = (int)(unsigned char)c;
	if (index > 127)
		index = '?';
	glyph = font8x8[index];

	for (row = 0; row < FONT_HEIGHT; row++)
	{
		py = y + row;
		if (py < 0 || py >= VGA_HEIGHT)
			continue;

		bits = glyph[row];
		for (col = 0; col < FONT_WIDTH; col++)
		{
			px = x + col;
			if (px < 0 || px >= VGA_WIDTH)
				continue;

			/* One byte per row, most significant bit leftmost. */
			if ((bits & (0x80 >> col)) != 0)
				fb[py * VGA_WIDTH + px] = fg;
			else if (bg != VGA_TRANSPARENT)
				fb[py * VGA_WIDTH + px] = bg;
		}
	}
}

/* A run of glyphs on one line, FONT_WIDTH apart.
*
*  It stops at the right edge instead of wrapping: wrapping into the next row
*  of pixels would put the tail of the string eight pixels below the head and
*  one character in from the left, which looks like a bug rather than a line
*  break. A caller that wants wrapping knows where its own line breaks belong.
*
*  Characters that start left of the screen are still stepped over so that the
*  visible part of a string with a negative x stays in the right columns. */
void vga_string(int x, int y, const char *s, uint8_t fg, uint8_t bg)
{
	if (s == 0)
		return;
	if (!coord_sane(x) || !coord_sane(y))
		return;
	if (y >= VGA_HEIGHT || y + FONT_HEIGHT <= 0)
		return;

	while (*s != '\0')
	{
		if (x >= VGA_WIDTH)
			break;		/* right edge reached, no wrap */

		vga_char(x, y, *s, fg, bg);
		x += FONT_WIDTH;
		s++;
	}
}
