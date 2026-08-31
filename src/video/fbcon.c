/* TomatOS - Framebuffer console
*  Desc: The console for a machine that came up in a graphics mode.
*
*  The header states the two constraints; this file states what was done
*  about them.
*
*  1. THE SHADOW BUFFER
*
*  The framebuffer cannot be touched when the first line is printed: it sits
*  outside the direct mapping and only the vmm can reach it, which is up long
*  after the first output. So every entry point writes into a character
*  buffer in .bss unconditionally, and only ALSO draws when the framebuffer
*  is mapped. fbcon_activate() maps it and then paints the whole buffer, so
*  the boot messages that were printed before the mapping existed appear the
*  moment it does. Nothing is lost, and no caller has to know which of the
*  two phases it is in.
*
*  2. SCROLLING
*
*  Scrolling one line at 1024x768x32 means 47 * 16 * 4096 = about 3 MB of
*  pixels, and a "cat" of a long file does it once per line. There are two
*  ways to do it, and the shadow buffer is what makes the second possible:
*
*    a) Move the pixels. memcpy() of 3080192 bytes up by one cell height,
*       then clear the bottom cell row. This kernel's memcpy() (kernel.c) is
*       a byte loop, so that is 3 million iterations of
*       load/store/increment/branch -- call it 12 million instructions. And
*       worse than the count suggests: half of that traffic is READS FROM THE
*       FRAMEBUFFER. A framebuffer is memory mapped hardware across the bus
*       and is mapped uncached; a read from it costs one to two orders of
*       magnitude more than a write, which is the whole reason graphics code
*       is written to be write-only.
*
*    b) Re-rasterise from the shadow buffer. Worst case, every one of the
*       128x48 = 6144 cells: 6144 * 16 rows * 8 pixels = 786432 pixel writes,
*       roughly 3 to 4 instructions each once the fg/bg values are hoisted
*       out of the loops, so about 2.5 to 3 million instructions -- and every
*       one of them is a WRITE. Plus a 22 KB move of the shadow buffer: the
*       move is whole rows of FBCON_MAX_COLS, not of the 128 columns this
*       mode uses, so it is 47 * 240 * 2 bytes rather than 47 * 128 * 2.
*
*  And (b) does not have to be the worst case, because the shadow buffer says
*  what is already on the screen: after the scroll, cell (c,r) has to show
*  what cell (c,r+1) shows now, so it only needs redrawing where the two
*  DIFFER. Real console output is mostly trailing blanks and matching
*  indentation, and those columns cost one 16-bit compare instead of 128
*  pixel writes. The compare sweep over all 6144 cells is about 30000
*  instructions, one percent of a full repaint, so it pays for itself as soon
*  as it skips one cell in a hundred.
*
*  Measured, one scroll at 1024x768x32, mean of 200, on a host build of this
*  file against the kernel's own byte-loop memcpy:
*
*      re-rasterise, typical console text   0.268 ms
*      re-rasterise, every cell different   0.510 ms
*      move the pixels                      1.162 ms
*
*  So the worst case of (b) is still better than twice as fast as (a), and
*  the ordinary case better than four times. That measurement runs entirely
*  in cached RAM, which is the most favourable setting (a) can possibly have
*  -- against a real uncached framebuffer its 3 MB of reads cost far more
*  than they do there. The margin on the machine is therefore wider, not
*  narrower, than these figures.
*
*  The framebuffer is therefore never read, only written -- true for the
*  scroll, the clear, the cursor and the glyphs alike.
*
*  3. HANDING THE SCREEN OVER
*
*  There is one screen and more than one thing that wants to draw on it: the
*  console, "gfx", and now a ring 3 program that has the framebuffer mapped
*  into its own address space. fbcon_suspend() and fbcon_resume() are how the
*  console steps aside for one of them.
*
*  This costs the file almost nothing, because the shadow buffer already
*  solves the hard half. Suspension does not stop the console WORKING, it
*  stops it PAINTING: every entry point still writes its characters into the
*  shadow buffer exactly as before, and only the pixel half is skipped. So a
*  boot message, a status line or a whole screen of scrolled output printed
*  while somebody else owns the screen is not lost -- it is remembered, and
*  fbcon_resume() paints all of it at once out of the buffer. That is the same
*  trick fbcon_activate() plays on the output that came before the mapping
*  existed, and the same one gfx_leave() plays with fbcon_repaint(); it is
*  merely made available to a caller that is not in the kernel.
*
*  Every write to the framebuffer in this file goes through exactly two
*  functions, fill_rect() and draw_cell(), so that is where the routing is
*  enforced -- see may_paint(). Gating the entry points alone would leave the
*  question "did I find all of them?" open forever, and a single path that
*  kept painting would put console characters on top of the other program's
*  window.
*
*  A note on what is NOT here: no printf(). printf() is routed to this file
*  once the console has handed over, so a single diagnostic inside a drawing
*  path would recurse. Everything reports through return values and
*  fbcon_info().
*
*  Locking is also absent on purpose. scrn.c holds the console lock (it masks
*  interrupts) across the calls it makes here, so a second lock would nest
*  for no gain.
*/
#include <system.h>
#include <vmm.h>
#include <multiboot.h>
#include <fbcon.h>

/* --- The mapping ---------------------------------------------------------
*
*  vmm_map_mmio() from vmm.h does the whole of it:
*
*      void *vmm_map_mmio(uint32_t phys, uint32_t size);
*
*  It returns a kernel pointer to the byte at phys, or 0, mapped uncached in
*  a window at the top of the kernel half and visible in every address space.
*  The mapping is never released, which suits a console exactly -- it takes
*  the framebuffer at activation and keeps it for the life of the machine.
*
*  Mapping by hand with vmm_map() was rejected. The framebuffer typically
*  lives around 0xFD000000, far outside the direct mapping, so it needs a
*  virtual window of its own and a policy for where to put that window and
*  how to make it reachable from every page directory. That is the vmm's
*  business, not the console's. */

/* --- The font ------------------------------------------------------------
*
*  One byte per glyph row, most significant bit leftmost, 256 glyphs (CP437,
*  which is what printf() in scrn.c already translates its umlauts into).
*  Lives in src/video/font8x16.c. Declared here for the same reason as above.
*
*  FBCON_CELL_W and FBCON_CELL_H used to be defined here. They are in fbcon.h
*  now, next to the two public macros that are computed from them. */
extern const uint8_t font8x16[256][FBCON_CELL_H];

/* --- The shadow buffer ---------------------------------------------------
*
*  One 16-bit cell per character position, laid out exactly like the VGA text
*  buffer: character in the low byte, attribute in the high byte. That is not
*  nostalgia -- it makes a cell a single word to compare, which is what the
*  scroll leans on, and it makes the whole buffer one memcpy to shift.
*
*  The size follows FBCON_MAX_WIDTH/HEIGHT and is not chosen here: 1920x1080
*  with an 8x16 cell is 240x67 cells, so 31.4 KiB of .bss. A larger mode is
*  clipped to it rather than refused, so the console works on a screen this
*  buffer cannot fully describe -- it just does not use the far right and the
*  bottom of it. Verified against a 1920x1200 and a 2560x1440 mode: both come
*  out as a 240x67 console, and paint_all() still fills the WHOLE screen with
*  the background first, so the part outside the console is black rather than
*  whatever the BIOS left there.
*
*  The figure is stated as a consequence rather than as a constant because it
*  has already been wrong once: it said "160x64 cells, 20 KiB, covers
*  1280x1024" for as long as FBCON_MAX_* said 1920x1080, and the scroll
*  measurements below inherited the same stale column count. */
static uint16_t shadow[FBCON_MAX_ROWS][FBCON_MAX_COLS];

/* Geometry in characters. Before fbcon_describe() has been called these
*  describe the VGA text screen, so that output mirrored into the shadow
*  buffer during early boot lands at the coordinates it was written for. */
static int fb_cols = 80;
static int fb_rows = 25;

/* What the bootloader reported, verbatim. */
static uint32_t fb_phys;
static uint32_t fb_pitch;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_bpp;
static uint32_t fb_type = MULTIBOOT_FRAMEBUFFER_EGA_TEXT;

/* Non-zero once a usable GRAPHICS mode has been described -- which happens
*  long before it can be mapped. This, not the mapping, is what tells the
*  console where to send its output.
*
*  The distinction is the whole point of the shadow buffer. Between
*  fbcon_describe() and fbcon_activate() lies almost the entire boot: the
*  banner, the memory map, the driver lines. On a machine that came up in a
*  graphics mode there is no text buffer for any of it -- writes to 0xB8000
*  go nowhere. So scrn.c has to route into the shadow buffer from the first
*  character, and fbcon_activate() paints the result. Tying this flag to
*  fb_mem instead would send exactly that output to an address nobody
*  displays, and the screen would start at whatever line the mapping
*  happened to succeed on. */
static int fb_owns_console = 0;

/* What was made of it. fb_mem is 0 until fbcon_activate() succeeds, and
*  fb_mem being non-zero is the single "output goes here now" flag. */
static uint8_t *fb_mem;
static uint32_t fb_bytes;      /* bytes per pixel, 1..4 */
static uint32_t fb_mapped_size;

/* Cursor. drawn says whether the block is currently on the screen, so that
*  anything which repaints over it can simply clear the flag instead of
*  having to erase it first.
*
*  While the console is suspended the flag is kept exactly as it would have
*  been had the painting happened -- see the note at fb_suspended. */
static int csr_col;
static int csr_row;
static int csr_drawn;

/* Non-zero while somebody else owns the screen. The console then updates the
*  shadow buffer as usual and paints nothing; fbcon_resume() puts the whole
*  console back from the buffer.
*
*  A flag and not a counter, because the header refuses nesting rather than
*  counting it: two owners of one screen cannot both have it, and the one that
*  lost would find out by having its picture drawn over. A count would hand
*  out the screen twice and hide it.
*
*  It can only ever be set while fb_mem is non-zero -- fbcon_suspend() refuses
*  otherwise -- which is why fbcon_activate() need not consider it: by the time
*  anything can suspend, activation has already returned, and it returns early
*  for a framebuffer that is mapped. fbcon_describe() clears it all the same,
*  since a new mode invalidates every claim on the old one. */
static int fb_suspended;

/* --- Colour --------------------------------------------------------------
*
*  The 16 console colours in the shades the VGA DAC has used since the EGA.
*  Everything else in this file works from the pixel values derived from
*  these once, at activation, and never converts a colour again. */
static const uint8_t console_rgb[16][3] = {
	{ 0x00, 0x00, 0x00 },   /*  0 black          */
	{ 0x00, 0x00, 0xAA },   /*  1 blue           */
	{ 0x00, 0xAA, 0x00 },   /*  2 green          */
	{ 0x00, 0xAA, 0xAA },   /*  3 cyan           */
	{ 0xAA, 0x00, 0x00 },   /*  4 red            */
	{ 0xAA, 0x00, 0xAA },   /*  5 magenta        */
	{ 0xAA, 0x55, 0x00 },   /*  6 brown          */
	{ 0xAA, 0xAA, 0xAA },   /*  7 light grey     */
	{ 0x55, 0x55, 0x55 },   /*  8 dark grey      */
	{ 0x55, 0x55, 0xFF },   /*  9 light blue     */
	{ 0x55, 0xFF, 0x55 },   /* 10 light green    */
	{ 0x55, 0xFF, 0xFF },   /* 11 light cyan     */
	{ 0xFF, 0x55, 0x55 },   /* 12 light red      */
	{ 0xFF, 0x55, 0xFF },   /* 13 light magenta  */
	{ 0xFF, 0xFF, 0x55 },   /* 14 yellow         */
	{ 0xFF, 0xFF, 0xFF }    /* 15 white          */
};

/* The 16 colours as ready made pixel values for the mode in effect. */
static uint32_t palette[16];

/* --- Channel positions ---------------------------------------------------
*
*  fbcon_describe() is not given the red/green/blue field positions and mask
*  sizes, and this file may not add a parameter to it. Three ways out were
*  considered:
*
*    - Guess from the pixel data. Impossible: nothing can be read back that
*      was not written first, and a wrong guess paints black on black.
*    - Reach into the multiboot info. It is not reachable from here, and
*      making it reachable means a header.
*    - Derive the layout from the depth, and refuse every depth for which
*      there is no single universal answer.
*
*  The third is what happens, because at these depths the layout is not
*  actually in doubt:
*
*      32 bpp   8:8:8 at 16/8/0, the top byte ignored   (XRGB8888)
*      24 bpp   8:8:8 at 16/8/0, three bytes per pixel  (BGR in memory)
*      16 bpp   5:6:5 at 11/5/0
*      15 bpp   5:5:5 at 10/5/0
*
*  This is not a hopeful guess. Our own boot/vbe.inc synthesises exactly
*  these when a BIOS leaves the direct colour fields blank, VBE has reported
*  them for every packed pixel mode since 2.0, and a card that answered
*  otherwise at 24 or 32 bpp would break every other operating system too.
*  Any other depth -- 4, 12, 30, 64 -- is refused by fbcon_activate() and the
*  console stays in text mode, which is the outcome the header asks for: a
*  clean refusal beats a screen full of wrong colours.
*
*  Should the real fields ever be plumbed through (a struct framebuffer in a
*  header of its own is what kernel.c already says it wants), the only change
*  needed here is to fill these six variables from them instead of from
*  format_from_depth(). Nothing else in the file knows where they came
*  from. */
static uint8_t ch_pos[3];      /* red, green, blue field position */
static uint8_t ch_size[3];     /* red, green, blue mask size      */

/* VGA DAC, used only for an indexed mode. */
#define DAC_INDEX_PORT  0x3C8
#define DAC_DATA_PORT   0x3C9

/* --- Small helpers ------------------------------------------------------- */

/* May a pixel be written right now? Two entirely different reasons for no,
*  and every painting path in this file wants them both:
*
*    fb_mem == 0    there is nowhere to write -- text mode boot, or a
*                   graphics mode described but not yet mapped.
*    fb_suspended   there is somewhere, but it is not ours at the moment.
*
*  The two are deliberately not distinguished at the point of use, because the
*  handling is identical: write the character into the shadow buffer, draw
*  nothing, and let whoever paints the buffer next put it on the screen.
*  fbcon_activate() and fbcon_resume() are those two painters respectively,
*  and both go through paint_all(), so the outcome is the same either way. */
static int may_paint(void)
{
	return fb_mem != 0 && !fb_suspended;
}

/* First byte of a pixel row. Always derived from the pitch, never from the
*  width: hardware pads rows, and a 1024 pixel wide 32 bpp mode may well have
*  a pitch of 4224 rather than 4096. Computing y * width * bpp/8 is what
*  makes a picture slant across the screen. */
static uint8_t *row_ptr(uint32_t y)
{
	return fb_mem + y * fb_pitch;
}

static uint32_t rgb_to_pixel(uint8_t r, uint8_t g, uint8_t b)
{
	return (((uint32_t)r >> (8 - ch_size[0])) << ch_pos[0])
	     | (((uint32_t)g >> (8 - ch_size[1])) << ch_pos[1])
	     | (((uint32_t)b >> (8 - ch_size[2])) << ch_pos[2]);
}

/* Fills a rectangle with one pixel value. Clips against the screen, so a
*  caller may hand in a cell that hangs over the edge of a mode whose size is
*  not a multiple of the cell.
*
*  The depth is decided once, outside both loops. A switch inside the inner
*  loop would be a branch per pixel on the hottest path in the file. */
static void fill_rect(int x, int y, int w, int h, uint32_t pixel)
{
	uint8_t *dst;
	int i;

	/* One of the two doors to the framebuffer, and therefore one of the two
	*  places the suspension is enforced. The callers test as well, where
	*  testing lets them skip work worth skipping; this test is what makes
	*  the guarantee hold for a caller that forgot -- including one added
	*  later. It costs a load and a branch per rectangle, against sixteen
	*  thousand pixel writes for a single cell sized one. */
	if(!may_paint())
		return;

	if(x < 0) { w += x; x = 0; }
	if(y < 0) { h += y; y = 0; }
	if(w <= 0 || h <= 0)
		return;
	if((uint32_t)x >= fb_width || (uint32_t)y >= fb_height)
		return;
	if((uint32_t)(x + w) > fb_width)
		w = (int)fb_width - x;
	if((uint32_t)(y + h) > fb_height)
		h = (int)fb_height - y;

	dst = row_ptr((uint32_t)y) + (uint32_t)x * fb_bytes;

	switch(fb_bytes)
	{
		case 4:
			for(; h > 0; h--)
			{
				uint32_t *p = (uint32_t *)dst;
				for(i = 0; i < w; i++)
					p[i] = pixel;
				dst += fb_pitch;
			}
			break;
		case 3:
			for(; h > 0; h--)
			{
				uint8_t *p = dst;
				for(i = 0; i < w; i++)
				{
					p[0] = (uint8_t)pixel;
					p[1] = (uint8_t)(pixel >> 8);
					p[2] = (uint8_t)(pixel >> 16);
					p += 3;
				}
				dst += fb_pitch;
			}
			break;
		case 2:
			for(; h > 0; h--)
			{
				uint16_t *p = (uint16_t *)dst;
				for(i = 0; i < w; i++)
					p[i] = (uint16_t)pixel;
				dst += fb_pitch;
			}
			break;
		default:
			for(; h > 0; h--)
			{
				uint8_t *p = dst;
				for(i = 0; i < w; i++)
					p[i] = (uint8_t)pixel;
				dst += fb_pitch;
			}
			break;
	}
}

/* Rasterises one cell. The hot path of the whole file: a full repaint calls
*  this 6144 times.
*
*  Everything that does not change per pixel is hoisted out -- the two pixel
*  values, the destination pointer, the depth -- so the inner loop is a shift,
*  a test and a store. The glyph row is consumed from the top bit down, which
*  is the order the font is stored in. */
static void draw_cell(int col, int row, unsigned char c, int attrib)
{
	const uint8_t *glyph;
	uint8_t *dst;
	uint32_t fg;
	uint32_t bg;
	unsigned int bits;
	int y;
	int x;

	/* The other door to the framebuffer; see fill_rect(). One test against
	*  the 128 pixel writes of a cell, so it does not show up in the scroll
	*  measurement at the top of the file. */
	if(!may_paint())
		return;

	glyph = font8x16[c];
	fg = palette[attrib & 0x0F];
	bg = palette[(attrib >> 4) & 0x0F];
	dst = row_ptr((uint32_t)row * FBCON_CELL_H)
	    + (uint32_t)col * FBCON_CELL_W * fb_bytes;

	switch(fb_bytes)
	{
		case 4:
			for(y = 0; y < FBCON_CELL_H; y++)
			{
				uint32_t *p = (uint32_t *)dst;
				bits = glyph[y];
				for(x = 0; x < FBCON_CELL_W; x++)
				{
					p[x] = (bits & 0x80u) ? fg : bg;
					bits <<= 1;
				}
				dst += fb_pitch;
			}
			break;
		case 3:
			for(y = 0; y < FBCON_CELL_H; y++)
			{
				uint8_t *p = dst;
				bits = glyph[y];
				for(x = 0; x < FBCON_CELL_W; x++)
				{
					uint32_t v = (bits & 0x80u) ? fg : bg;
					p[0] = (uint8_t)v;
					p[1] = (uint8_t)(v >> 8);
					p[2] = (uint8_t)(v >> 16);
					p += 3;
					bits <<= 1;
				}
				dst += fb_pitch;
			}
			break;
		case 2:
			for(y = 0; y < FBCON_CELL_H; y++)
			{
				uint16_t *p = (uint16_t *)dst;
				bits = glyph[y];
				for(x = 0; x < FBCON_CELL_W; x++)
				{
					p[x] = (uint16_t)((bits & 0x80u) ? fg : bg);
					bits <<= 1;
				}
				dst += fb_pitch;
			}
			break;
		default:
			for(y = 0; y < FBCON_CELL_H; y++)
			{
				uint8_t *p = dst;
				bits = glyph[y];
				for(x = 0; x < FBCON_CELL_W; x++)
				{
					p[x] = (uint8_t)((bits & 0x80u) ? fg : bg);
					bits <<= 1;
				}
				dst += fb_pitch;
			}
			break;
	}
}

/* The cursor is an underline in the bottom two rows of the cell, the shape
*  the VGA has by default, drawn in the character's own foreground colour.
*  Erasing it is a redraw of the cell from the shadow buffer -- there is
*  nothing to save and restore, and nothing is read back from the screen.
*
*  Drawing it is a function of its own because there are two callers:
*  fbcon_cursor(), which moves it, and fbcon_repaint(), which has just painted
*  over it and has to put it back where it was.
*
*  While the console is suspended both keep csr_drawn moving exactly as they
*  would otherwise, and paint nothing. The flag then no longer says "the block
*  is on the screen" -- nothing of the console's is -- it says "a block belongs
*  at csr_col/csr_row", which is precisely the question fbcon_resume() has to
*  answer, and it answers it by handing the flag to fbcon_repaint()'s existing
*  bookkeeping. Keeping the flag still instead would lose a cursor the user
*  expects back; a second flag would be a second thing to keep in step with
*  fbcon_putc(), which clears this one when a character lands on the cursor
*  cell. */
static void draw_cursor(void)
{
	uint16_t cell;

	if(fb_mem == 0)
		return;

	csr_drawn = 1;
	if(fb_suspended)
		return;

	cell = shadow[csr_row][csr_col];
	fill_rect(csr_col * FBCON_CELL_W,
		  csr_row * FBCON_CELL_H + FBCON_CELL_H - 2,
		  FBCON_CELL_W, 2,
		  palette[(cell >> 8) & 0x0F]);
}

static void erase_cursor(void)
{
	uint16_t cell;

	if(!csr_drawn)
		return;
	csr_drawn = 0;
	if(!may_paint())
		return;         /* nothing of ours is on the screen to erase */

	cell = shadow[csr_row][csr_col];
	draw_cell(csr_col, csr_row, (unsigned char)(cell & 0xFF), (int)(cell >> 8));
}

/* --- Public interface ---------------------------------------------------- */

void fbcon_describe(uint32_t addr, uint32_t pitch, uint32_t width,
                    uint32_t height, uint8_t bpp, uint8_t type)
{
	fb_phys   = addr;
	fb_pitch  = pitch;
	fb_width  = width;
	fb_height = height;
	fb_bpp    = bpp;
	fb_type   = type;

	/* A second description invalidates whatever was mapped for the first.
	*  This is not a case that arises in the kernel -- the bootloader
	*  describes the screen once -- but leaving a stale pointer live would
	*  mean drawing into a window that no longer describes this mode. The
	*  shadow buffer is deliberately NOT touched: its whole purpose is to
	*  outlive the moments where the screen is unavailable. */
	fb_mem = 0;
	csr_drawn = 0;
	fb_owns_console = 0;

	/* And no claim on the old screen survives into the new one. A suspension
	*  left standing here would silence the console for good: nothing would
	*  paint, and the only caller that could lift it is holding a mapping of a
	*  framebuffer that has just stopped being the screen. */
	fb_suspended = 0;

	if(type == MULTIBOOT_FRAMEBUFFER_EGA_TEXT)
	{
		/* For a text mode the bootloader reports width and height in
		*  CHARACTERS, not pixels. Nothing here will ever be activated,
		*  but the geometry the shell reports should still be true. */
		fb_cols = (width  > 0 && width  <= FBCON_MAX_COLS) ? (int)width  : 80;
		fb_rows = (height > 0 && height <= FBCON_MAX_ROWS) ? (int)height : 25;
		return;
	}

	if(width < FBCON_CELL_W || height < FBCON_CELL_H)
	{
		/* Not a screen a character fits on. Keep the text geometry so the
		*  shadow buffer stays usable and let fbcon_activate() refuse. */
		return;
	}

	fb_cols = (int)(width / FBCON_CELL_W);
	fb_rows = (int)(height / FBCON_CELL_H);

	/* Clipped, not refused: a bigger screen is used as far as the shadow
	*  buffer describes it. */
	if(fb_cols > FBCON_MAX_COLS)
		fb_cols = FBCON_MAX_COLS;
	if(fb_rows > FBCON_MAX_ROWS)
		fb_rows = FBCON_MAX_ROWS;

	if(csr_col >= fb_cols)
		csr_col = fb_cols - 1;
	if(csr_row >= fb_rows)
		csr_row = fb_rows - 1;

	/* From here the console belongs to us, even though there is nothing to
	*  draw on yet. Everything printed until fbcon_activate() lands in the
	*  shadow buffer and is painted then. */
	fb_owns_console = 1;
}

/* Fills ch_pos/ch_size for the depth in effect. Returns 0 for a depth this
*  console will not serve; see the long comment at the declarations. */
static int format_from_depth(void)
{
	switch(fb_bpp)
	{
		case 32:
		case 24:
			ch_pos[0] = 16; ch_size[0] = 8;   /* red   */
			ch_pos[1] =  8; ch_size[1] = 8;   /* green */
			ch_pos[2] =  0; ch_size[2] = 8;   /* blue  */
			return 1;
		case 16:
			ch_pos[0] = 11; ch_size[0] = 5;
			ch_pos[1] =  5; ch_size[1] = 6;
			ch_pos[2] =  0; ch_size[2] = 5;
			return 1;
		case 15:
			ch_pos[0] = 10; ch_size[0] = 5;
			ch_pos[1] =  5; ch_size[1] = 5;
			ch_pos[2] =  0; ch_size[2] = 5;
			return 1;
		default:
			return 0;
	}
}

/* An indexed mode has no channels at all, only a palette, and the console
*  needs no more than sixteen entries of it. Writing them removes the last
*  guess from this file: the pixel value for colour n becomes n, whatever the
*  bootloader or the BIOS left in the DAC.
*
*  The DAC takes six bits per channel unless VBE function 4F08h was used to
*  switch it to eight, which nothing here does -- hence the shift by two. */
static void program_dac(void)
{
	int i;

	for(i = 0; i < 16; i++)
	{
		outportb(DAC_INDEX_PORT, (unsigned char)i);
		outportb(DAC_DATA_PORT, (unsigned char)(console_rgb[i][0] >> 2));
		outportb(DAC_DATA_PORT, (unsigned char)(console_rgb[i][1] >> 2));
		outportb(DAC_DATA_PORT, (unsigned char)(console_rgb[i][2] >> 2));
	}
}

/* Every way out of fbcon_activate() that fails goes through here. Giving
*  the console back to text mode matters: fb_owns_console was set by
*  fbcon_describe(), so leaving it set after a failed mapping would have
*  scrn.c writing into a shadow buffer nothing ever paints -- a blank
*  screen instead of a visible fallback. Whatever early output the buffer
*  already holds is lost with it; a machine that reports a graphics mode
*  the kernel cannot map has no place to show it either way. */
static int fbcon_give_up(void)
{
	fb_owns_console = 0;
	fb_mem = 0;
	return -1;
}

/* Paints the whole screen from the shadow buffer. Two callers want exactly
*  this and it must not exist twice: fbcon_activate(), where it is the moment
*  everything printed before the mapping appears, and fbcon_repaint(), where it
*  is how the console comes back after something drew over it. A second copy
*  would drift -- and the two would then disagree about what the console looks
*  like, which is the one thing the shadow buffer exists to settle.
*
*  The screen is painted black first, in one pass, which also covers the margin
*  a mode whose size is not a multiple of the cell leaves on the right and at
*  the bottom. Then only the cells that are not already black are drawn -- a
*  cell holding a space (or the zero a .bss buffer starts out as) on a black
*  background is already on the screen. On a boot screen that is most of it.
*
*  This is the one place that assumes something about the font, namely that
*  glyph 0x20 and glyph 0 are blank. Every 8x16 CP437 font has them blank; a
*  font that did not would lose its spaces here, and nowhere else.
*
*  The cursor is NOT drawn: it has just been painted over, and whether it
*  should come back is a question only the caller can answer. Both of them
*  leave csr_drawn correct on their own way out. */
static void paint_all(void)
{
	int col;
	int row;

	fill_rect(0, 0, (int)fb_width, (int)fb_height, palette[0]);

	for(row = 0; row < fb_rows; row++)
	{
		for(col = 0; col < fb_cols; col++)
		{
			uint16_t cell = shadow[row][col];
			unsigned char c = (unsigned char)(cell & 0xFF);
			int attrib = (int)(cell >> 8);

			if((attrib & 0xF0) == 0 && (c == 0 || c == ' '))
				continue;
			draw_cell(col, row, c, attrib);
		}
	}
}

int fbcon_activate(void)
{
	uint32_t size;
	void *virt;
	int i;

	if(fb_mem != 0)
		return 0;                       /* already running */

	if(fb_phys == 0 || fb_type == MULTIBOOT_FRAMEBUFFER_EGA_TEXT)
		return fbcon_give_up();

	if(fb_width < FBCON_CELL_W || fb_height < FBCON_CELL_H)
		return fbcon_give_up();

	if(fb_type == MULTIBOOT_FRAMEBUFFER_INDEXED)
	{
		if(fb_bpp != 8)
			return fbcon_give_up();
		fb_bytes = 1;
		for(i = 0; i < 16; i++)
			palette[i] = (uint32_t)i;
	}
	else if(fb_type == MULTIBOOT_FRAMEBUFFER_RGB)
	{
		if(!format_from_depth())
			return fbcon_give_up();
		fb_bytes = (fb_bpp + 7) / 8;
		for(i = 0; i < 16; i++)
			palette[i] = rgb_to_pixel(console_rgb[i][0],
						  console_rgb[i][1],
						  console_rgb[i][2]);
	}
	else
	{
		return fbcon_give_up();                      /* a type we do not know */
	}

	/* The pitch is the row stride and has to be at least a row wide. A
	*  bootloader that reported nonsense here would otherwise have every
	*  row of the picture overlap the one above it, and the last row would
	*  run off the end of the mapping. */
	if(fb_pitch < fb_width * fb_bytes)
		return fbcon_give_up();

	/* pitch * height must not wrap. It never does for a real mode -- 4224 *
	*  1024 is barely 4 MB -- but the product decides how much is mapped,
	*  and a wrapped one would map a window smaller than what is drawn
	*  into. */
	if(fb_height > (0xFFFFFFFFu / fb_pitch))
		return fbcon_give_up();
	size = fb_pitch * fb_height;

	virt = vmm_map_mmio(fb_phys, size);
	if(virt == 0)
		return fbcon_give_up();                      /* stays in text mode */

	fb_mem = (uint8_t *)virt;
	fb_mapped_size = size;

	if(fb_type == MULTIBOOT_FRAMEBUFFER_INDEXED)
		program_dac();

	/* Now the point of the whole exercise: everything printed before this
	*  moment appears. */
	paint_all();

	/* Nothing has drawn a cursor yet, and paint_all() draws none. */
	csr_drawn = 0;
	return 0;
}

/* The console, back on the screen after somebody else used it.
*
*  The shadow buffer is the only record of what the console showed, so this is
*  the whole of the recovery: no saved pixels, nothing read back from the card.
*
*  The cursor is the one piece of state paint_all() cannot work out for itself,
*  because it is not in the shadow buffer. It has been painted over either way,
*  so csr_drawn is cleared BEFORE the paint -- if anything below faulted, the
*  flag would still describe the screen truthfully -- and the block is then put
*  back only if it was on the screen to begin with. Leaving a cursor visible
*  where there was none would be as wrong as losing one. */
void fbcon_repaint(void)
{
	int had_cursor;

	/* Also a no-op while the screen belongs to somebody else: a repaint is
	*  the largest piece of painting in the file, and doing it under a
	*  suspension would replace the other program's picture with the console
	*  in one stroke. fbcon_resume() lifts the suspension first and then calls
	*  this, which is the one way the console comes back. */
	if(!may_paint())
		return;

	had_cursor = csr_drawn;
	csr_drawn = 0;

	paint_all();

	if(had_cursor)
		draw_cursor();
}

int fbcon_active(void)
{
	return fb_owns_console;
}

int fbcon_cols(void)
{
	return fb_cols;
}

int fbcon_rows(void)
{
	return fb_rows;
}

void fbcon_putc(int col, int row, unsigned char c, int attrib)
{
	if(col < 0 || row < 0 || col >= fb_cols || row >= fb_rows)
		return;

	shadow[row][col] = (uint16_t)(c | ((attrib & 0xFF) << 8));

	/* The cursor sits in this cell and has just been painted over. Say so
	*  rather than erasing it: erasing would redraw the cell a second time,
	*  and the caller moves the cursor right after every character anyway. */
	if(csr_drawn && col == csr_col && row == csr_row)
		csr_drawn = 0;

	/* The shadow buffer above is written unconditionally; only the pixels
	*  below depend on there being a screen of ours to put them on. That
	*  order is the whole of "suspended output is remembered, not lost". */
	if(!may_paint())
		return;

	draw_cell(col, row, c, attrib);
}

void fbcon_clear(int attrib)
{
	uint16_t blank;
	int row;

	blank = (uint16_t)(' ' | ((attrib & 0xFF) << 8));

	for(row = 0; row < FBCON_MAX_ROWS; row++)
		memsetw(&shadow[row][0], blank, FBCON_MAX_COLS);

	csr_drawn = 0;

	if(!may_paint())
		return;

	/* The whole screen, not just the cells: the margin on the right and at
	*  the bottom belongs to the console too, and leaving it in the previous
	*  colour is exactly the sort of edge that looks like a bug. One
	*  fill_rect() of the background is also strictly cheaper than
	*  rasterising 6144 spaces, which is the same picture. */
	fill_rect(0, 0, (int)fb_width, (int)fb_height,
		  palette[(attrib >> 4) & 0x0F]);
}

/* Scrolls up by one line. See the strategy discussion at the top of the
*  file: the pixels are never moved, they are re-rasterised from the shadow
*  buffer, and only where the new content of a cell differs from what that
*  cell already shows. */
void fbcon_scroll(int attrib)
{
	uint16_t blank;
	int col;
	int row;

	blank = (uint16_t)(' ' | ((attrib & 0xFF) << 8));

	/* The cursor comes off the screen first, and this is not tidiness: the
	*  redraw below is a DIFFERENTIAL one, and its comparison is only valid
	*  while the screen actually shows what the shadow buffer says.
	*
	*  The cursor is the one thing that breaks that. It is painted over a
	*  cell without being recorded in the shadow buffer, so for the
	*  comparison the cell is unchanged -- the cell is skipped as "already
	*  correct", and the underline stays where it was. The csr_drawn = 0 at
	*  the end then tells everything afterwards that there is no cursor to
	*  erase, so it is never cleaned up: a permanent mark, one per scroll,
	*  accumulating along the bottom row for as long as the machine runs.
	*
	*  erase_cursor() redraws the cell underneath and clears the flag, which
	*  restores the invariant the comparison needs. */
	erase_cursor();

	if(fb_rows < 2)
	{
		fbcon_clear(attrib);
		return;
	}

	/* Drawing happens BEFORE the shadow buffer is shifted, because the
	*  comparison needs both the old and the new content of a cell, and
	*  after the shift the old one is gone. draw_cell() is handed the value
	*  explicitly for that reason.
	*
	*  Suspended, this whole block is skipped, and skipping it is not merely
	*  an optimisation -- the differential redraw would be WRONG here, not
	*  just wasted. Its premise is that the screen already shows what the
	*  shadow buffer says, which is what lets it leave a cell alone when the
	*  new content equals the old. Under a suspension that premise is exactly
	*  what has stopped holding: the screen shows somebody else's picture. A
	*  redraw on those terms would paint the differing cells over that
	*  picture and, worse, leave the matching ones untouched -- a console
	*  half drawn into a window it does not own.
	*
	*  Nothing is lost by skipping it, because the shadow buffer below is
	*  shifted either way and paint_all() in fbcon_resume() rebuilds every
	*  cell from it unconditionally: it fills the screen with the background
	*  first and then draws each non-blank cell, so it never assumes anything
	*  about what is on the screen and cannot inherit a stale pixel. The
	*  scroll while suspended therefore costs one memcpy of at most 31 KB --
	*  (FBCON_MAX_ROWS - 1) * FBCON_MAX_COLS * 2 -- and no pixels at all,
	*  however many screens' worth of text go past. */
	if(may_paint())
	{
		for(row = 0; row < fb_rows - 1; row++)
		{
			for(col = 0; col < fb_cols; col++)
			{
				uint16_t cell = shadow[row + 1][col];

				if(cell == shadow[row][col])
					continue;       /* already correct */
				draw_cell(col, row,
					  (unsigned char)(cell & 0xFF),
					  (int)(cell >> 8));
			}
		}

		/* The line that comes in at the bottom, on the same terms. */
		for(col = 0; col < fb_cols; col++)
		{
			if(shadow[fb_rows - 1][col] == blank)
				continue;
			draw_cell(col, fb_rows - 1, ' ', attrib & 0xFF);
		}
	}

	/* One memcpy for the shadow buffer: whole rows including the unused
	*  columns to the right, because that keeps it a single contiguous move
	*  of at most 31 KB instead of one call per row -- 22 KB at 1024x768,
	*  where only 48 of the 67 rows are in use. Forward copy with the
	*  destination BELOW the source, which is what kernel.c's byte loop
	*  handles correctly for overlapping ranges. */
	memcpy(&shadow[0][0], &shadow[1][0],
	       (size_t)((fb_rows - 1) * FBCON_MAX_COLS * (int)sizeof(uint16_t)));
	memsetw(&shadow[fb_rows - 1][0], blank, FBCON_MAX_COLS);

	/* erase_cursor() at the top already cleared it, and it did so having
	*  actually taken the cursor off the screen -- which this assignment on
	*  its own never did. Kept as a statement of the postcondition rather
	*  than because anything still needs it: after a scroll there is no
	*  cursor on the screen, and scrn.c draws a new one at the new position. */
	csr_drawn = 0;
}

void fbcon_cursor(int col, int row)
{
	if(col < 0 || row < 0 || col >= fb_cols || row >= fb_rows)
		return;

	if(csr_drawn && col == csr_col && row == csr_row)
		return;

	erase_cursor();

	csr_col = col;
	csr_row = row;

	draw_cursor();
}

/* --- The surface, for code that draws rather than prints ------------------
*
*  fbdraw.c is the caller these exist for. Everything it needs to put a pixel
*  somewhere is here and nothing else is: the console keeps its shadow buffer,
*  its palette and its cell arithmetic to itself.
*
*  All six report the MAPPED framebuffer, so they answer 0 in three different
*  situations that come to the same thing for a caller -- a machine that booted
*  into text mode, a graphics mode described but not yet activated, and one the
*  vmm could not map. In none of them is there an address a drawing routine may
*  write to, and a geometry without an address would only invite one to be
*  computed. fbcon_pixels() is therefore the single test worth making. */

uint32_t fbcon_width(void)
{
	return (fb_mem != 0) ? fb_width : 0;
}

uint32_t fbcon_height(void)
{
	return (fb_mem != 0) ? fb_height : 0;
}

uint32_t fbcon_pitch(void)
{
	/* The row stride as the card reported it, never width * bytes. See the
	*  note at row_ptr(): a 1024 pixel wide 32 bpp mode may perfectly well
	*  have a pitch of 4224, and computing it instead of asking is what makes
	*  a picture shear across the screen. */
	return (fb_mem != 0) ? fb_pitch : 0;
}

uint32_t fbcon_bpp(void)
{
	return (fb_mem != 0) ? fb_bpp : 0;
}

uint8_t *fbcon_pixels(void)
{
	return fb_mem;
}

/* The same framebuffer as fbcon_pixels(), named the way a page table names it.
*  The caller this exists for is a ring 3 program: it cannot use the kernel's
*  window, which lives in the kernel half of every address space and is not
*  reachable from user mode, so it needs the physical address to have the
*  frames mapped into its own.
*
*  Tied to fb_mem, exactly like the five above, and that is a decision worth
*  stating because fb_phys itself is known much earlier -- fbcon_describe()
*  records it before paging can even reach it, and it is a property of the card
*  rather than of anything the kernel did. Reporting it from then on would still
*  be wrong, for two reasons:
*
*    - Nothing has been checked yet. fbcon_activate() is where the depth is
*      established as one this file understands, where the pitch is checked
*      against the width, and where pitch * height is checked for wrapping. A
*      caller handed the address before that has the one number that lets it
*      map memory and none of the numbers that say how much or in what format
*      -- fbcon_width(), fbcon_pitch() and fbcon_bpp() all answer 0 until the
*      same moment. An address without a size is an invitation to compute one.
*
*    - It may not be a screen. If activation failed the console fell back to
*      text mode and what is on the display is 0xB8000, not this. Handing out
*      the address anyway would give a program a mapping of a region nobody is
*      scanning out, and it would draw into it and see nothing.
*
*  So the answer before fbcon_activate() has succeeded is 0, which the header
*  already tells every caller to check, and the check it asks for is the one
*  worth making: 0 means there is no screen here to be given away. */
uint32_t fbcon_phys(void)
{
	return (fb_mem != 0) ? fb_phys : 0;
}

/* Nearest of the sixteen colours program_dac() actually put in the DAC.
*
*  An indexed mode has no pixel format to pack a colour into -- a pixel is an
*  index and nothing else -- so the only truthful answer is the index whose
*  programmed colour is closest to the one asked for. Sixteen entries is all
*  this file ever programs, so sixteen is all that may be named.
*
*  Plain squared distance in RGB. It is not perceptually right, and for a
*  choice between sixteen widely separated colours it does not need to be. The
*  largest possible sum is 3 * 255^2 = 195075, so nothing here comes near
*  overflowing an int. */
static uint32_t nearest_console_colour(uint8_t r, uint8_t g, uint8_t b)
{
	int best = 0;
	int best_d = 0x7FFFFFFF;
	int i;

	for(i = 0; i < 16; i++)
	{
		int dr = (int)r - (int)console_rgb[i][0];
		int dg = (int)g - (int)console_rgb[i][1];
		int db = (int)b - (int)console_rgb[i][2];
		int d = dr * dr + dg * dg + db * db;

		if(d < best_d)
		{
			best_d = d;
			best = i;
		}
	}

	return (uint32_t)best;
}

uint32_t fbcon_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	/* Before activation there is no format to pack into: ch_pos and ch_size
	*  are still zero, and rgb_to_pixel() would shift every channel out of
	*  existence and hand back a plausible looking black. Saying 0 here is the
	*  same value, but it is the answer to a question that was asked too
	*  early rather than a colour. */
	if(fb_mem == 0)
		return 0;

	if(fb_type == MULTIBOOT_FRAMEBUFFER_INDEXED)
		return nearest_console_colour(r, g, b);

	/* The channel positions the mode reports, via format_from_depth(), and
	*  not an assumption about 32 bpp being 0x00RRGGBB. */
	return rgb_to_pixel(r, g, b);
}

/* --- Handing the screen over ---------------------------------------------
*
*  See section 3 of the file comment for what suspension is. What follows is
*  what the two ends of it have to do beyond setting the flag, which is in
*  both cases the cursor and nothing else.
*
*  Refusal. Two things are refused, and both return -1 rather than being
*  distinguished, because a caller can do exactly one thing with either: not
*  draw. 0 means the screen is yours; anything else means it is not, and a
*  program that goes on to draw after a non-zero return draws over a console
*  that is still painting into the same pixels.
*
*    - Already suspended. Nesting is refused rather than counted, per the
*      header. The second caller must not take a screen the first is using,
*      and must not be given a way to hand back a screen it never held: with
*      a count, the inner resume would return nothing to the outer owner
*      while the outer resume would repaint over a picture still being drawn.
*
*    - No mapped framebuffer -- a text mode boot, or a graphics mode that was
*      described and could not be mapped. There is nothing here to hand over:
*      fbcon_phys() is 0 for the same reason, so the caller has nothing to
*      map, and the console it would be silencing is not this file's at all
*      (scrn.c is writing to 0xB8000, which no flag here touches). Answering
*      "yes, take it" would be a lie in the one direction that matters, so
*      the whole sequence fails at the first step and the caller reports that
*      the machine has no framebuffer to give.
*
*  A caller that is refused should leave the screen alone and say so; there is
*  nothing to retry, since neither cause clears itself with time. The one thing
*  it must not do is call fbcon_resume() anyway -- which is why resume is a
*  no-op unless a suspension is actually standing, so that a caller which does
*  it in a cleanup path cannot repaint the console over the owner's picture. */

int fbcon_suspend(void)
{
	int had_cursor;

	if(fb_mem == 0)
		return -1;
	if(fb_suspended)
		return -1;

	/* The cursor comes off the screen while painting is still allowed. It is
	*  the console's one mark that is NOT in the shadow buffer, so nothing
	*  later would clean it up: a block left in the corner of somebody else's
	*  window, in the console's colours, that the owner did not draw and
	*  cannot erase without knowing it is there.
	*
	*  The flag is put back afterwards on purpose. erase_cursor() clears it,
	*  which is the truth about the pixels and the wrong thing to remember --
	*  the console wants a cursor at csr_col/csr_row and will want one again
	*  when it gets the screen back. From here to fbcon_resume() the flag
	*  means "a cursor belongs here" rather than "a cursor is on the screen",
	*  which is the reading draw_cursor() and erase_cursor() keep up while
	*  suspended, and fbcon_repaint() turns it back into the other one. */
	had_cursor = csr_drawn;
	erase_cursor();
	csr_drawn = had_cursor;

	fb_suspended = 1;
	return 0;
}

void fbcon_resume(void)
{
	/* Idempotent, and a no-op when nothing was suspended. Resume is the
	*  thing a caller reaches for in an error path, where it is not always
	*  obvious whether the suspend succeeded -- and where repainting the
	*  console over a screen somebody else still owns would be the worst
	*  possible answer. */
	if(!fb_suspended)
		return;

	/* Order matters: the flag first, or the repaint below would be gated by
	*  may_paint() and do nothing at all. */
	fb_suspended = 0;

	/* And that is the whole of coming back. Everything printed while the
	*  screen was somebody else's is in the shadow buffer, in the order it
	*  was printed, scrolled as far as it scrolled; fbcon_repaint() paints
	*  all of it and restores the cursor from the flag maintained above.
	*  Nothing needs to be known about what the other program drew, because
	*  the repaint fills the screen before it draws a single cell. */
	fbcon_repaint();
}

int fbcon_suspended(void)
{
	return fb_suspended;
}

/* --- Description for the shell -------------------------------------------
*
*  Formatted by hand into a static buffer. printf() is not an option: its
*  output is routed here, and the number conversions in str.c hand back
*  pointers into shared static buffers that a second task could be using. */
static char info_buf[112];

static char *fmt_dec(char *p, uint32_t v)
{
	char tmp[12];
	int n;

	n = 0;
	if(v == 0)
	{
		*p++ = '0';
		return p;
	}
	while(v != 0 && n < 12)
	{
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while(n > 0)
		*p++ = tmp[--n];
	return p;
}

static char *fmt_hex(char *p, uint32_t v)
{
	static const char digits[] = "0123456789ABCDEF";
	int shift;
	int started;

	*p++ = '0';
	*p++ = 'x';
	started = 0;
	for(shift = 28; shift >= 0; shift -= 4)
	{
		int d = (int)((v >> shift) & 0x0Fu);
		if(d != 0 || started || shift == 0)
		{
			*p++ = digits[d];
			started = 1;
		}
	}
	return p;
}

static char *fmt_str(char *p, const char *s)
{
	while(*s != '\0')
		*p++ = *s++;
	return p;
}

const char *fbcon_info(void)
{
	char *p;

	p = info_buf;

	if(fb_phys == 0 || fb_type == MULTIBOOT_FRAMEBUFFER_EGA_TEXT)
	{
		fmt_str(p, "no framebuffer, VGA text mode");
		info_buf[sizeof(info_buf) - 1] = '\0';
		return info_buf;
	}

	p = fmt_dec(p, fb_width);
	*p++ = 'x';
	p = fmt_dec(p, fb_height);
	*p++ = 'x';
	p = fmt_dec(p, fb_bpp);
	p = fmt_str(p, (fb_type == MULTIBOOT_FRAMEBUFFER_INDEXED)
			? " indexed at " : " RGB at ");
	p = fmt_hex(p, fb_phys);

	if(fb_mem != 0)
	{
		p = fmt_str(p, " -> ");
		p = fmt_hex(p, (uint32_t)fb_mem);
		p = fmt_str(p, ", ");
		p = fmt_dec(p, (uint32_t)fb_cols);
		*p++ = 'x';
		p = fmt_dec(p, (uint32_t)fb_rows);
		p = fmt_str(p, " cells, ");
		p = fmt_dec(p, fb_mapped_size >> 10);
		p = fmt_str(p, " KiB");
	}
	else
	{
		p = fmt_str(p, " (not mapped)");
	}

	*p = '\0';
	return info_buf;
}
