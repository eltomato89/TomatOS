/* TomatOS - gui: a window system in ring 3
*  Desc: A pointer, four windows, a Z order, and Escape to give the screen
*        back. The first TomatOS program that owns the screen.
*
*  ------------------------------------------------------------------------
*  What is actually hard here
*  ------------------------------------------------------------------------
*  Drawing a window is a filled rectangle, a bar across the top and some text.
*  That part is gfxlib.c's problem and it is solved. The window system is the
*  part where a window MOVES, because moving it uncovers whatever was behind
*  it, and "whatever was behind it" is a thing nobody has kept a copy of.
*
*  There are two standard answers and this program takes the second:
*
*    SAVE-UNDER. Before drawing something on top of the screen, copy the
*    pixels it will cover into a buffer, and copy them back when it goes away.
*    It is the cheapest answer for one small moving object and it is why the
*    technique exists. It is also why it does not survive contact with a
*    window system: the save has to happen in exactly the reverse order of the
*    draws, a window raised while another is being dragged invalidates a save
*    that has already been taken, and every saved region needs memory sized in
*    advance -- in a program with no malloc, memory sized in advance for the
*    worst case is memory sized for every window at once.
*
*    REPAINT FROM THE MODEL. Keep no pixels at all. Keep the windows, their
*    positions and their order, and rebuild the picture from those whenever
*    part of it changes. Nothing can be stale because nothing is kept.
*
*  The second answer costs redrawing, and redrawing everything on every mouse
*  movement would cost a whole screen of blit per event -- 3 MiB at 1024x768
*  and 7.9 at 1920x1080. So the picture is rebuilt only inside the rectangles
*  that changed:
*
*      1. something moves; the program records the rectangle it left and the
*         rectangle it arrived in as damage (gfx_damage());
*      2. for each damage rectangle, the clip window is set to it and the
*         WHOLE SCENE is painted -- desktop, then every window from the bottom
*         of the Z order up, then the pointer. Clipping makes "the whole
*         scene" cost only the pixels inside that rectangle;
*      3. the damage rectangles are copied to the screen (gfx_flush()).
*
*  Step 2 is where the exposed area gets repaired, and it gets repaired
*  without anybody having thought about exposure at all: the desktop is
*  painted there because the desktop is painted first, and any window that was
*  underneath is painted over it because it comes later in the Z order. The
*  same three lines handle a window moving, a window being raised, the pointer
*  passing over a title bar, and the clock ticking behind a window that covers
*  half of it. That is the whole design, and its cost is that a pixel may be
*  painted several times per frame -- once per window that overlaps it. With
*  four windows that is at most five paints of a small rectangle, which is
*  nothing against one full screen blit.
*
*  THE POINTER FALLS OUT OF THIS FOR FREE. It is painted last into the canvas
*  on every repaint, so it is over everything; when it moves, its old
*  rectangle is damaged and the repaint of that rectangle simply does not
*  include a pointer any more. There is no save-under buffer for it either,
*  and no ordering problem between it and a window being dragged underneath
*  it. A back buffer is what makes that free: with direct drawing the pointer
*  would have to be lifted before anything else could be drawn and put back
*  afterwards, which is the flicker.
*
*  ------------------------------------------------------------------------
*  Where the frames go
*  ------------------------------------------------------------------------
*  sys_input() blocks with a timeout, so the loop is not a spin: the task is
*  descheduled while the user is not doing anything. Damage is ACCUMULATED
*  across events and only flushed when the input call times out -- meaning the
*  queue has run dry -- or when the frame budget has expired. Dragging a
*  window produces a burst of mouse events, and without that coalescing each
*  one would blit two window sized rectangles; with it, a burst becomes one
*  frame.
*
*  ------------------------------------------------------------------------
*  How big a window is, and why it is not a number in this file
*  ------------------------------------------------------------------------
*  The mode is negotiated at boot: this same binary lands at 1920x1080 on one
*  machine and 640x480 on another (see the note on GFX_MAX_WIDTH in
*  user/gfxlib.h). A window measured in pixels is therefore a window measured
*  against nothing -- 470 pixels is two thirds of a 640 pixel screen and a
*  quarter of a 1920 pixel one.
*
*  There are two obvious answers and both are wrong on their own:
*
*    FIXED PIXELS. Windows stay legible and get visually smaller as the
*    screen grows -- but the font does not shrink with them, so eventually
*    the text is the only thing at its old size and the windows look
*    stranded in the middle of a desktop they no longer relate to.
*
*    A FRACTION OF THE SCREEN. The proportions hold at every size -- but the
*    font is FIXED AT 8x8 and cannot follow, so the text gets relatively
*    smaller until a window is mostly empty, and on the way down a window
*    that is "a quarter of the screen" stops being able to hold its own
*    contents at 640x480.
*
*  So neither: a window is measured in CHARACTER CELLS, and the cell is the
*  one thing chosen from the mode. Each window declares how many columns and
*  rows of text it has to hold -- gui_win[] below carries those two numbers
*  and nothing else about its size -- and every pixel dimension in this file
*  is derived from the cell by gui_metrics().
*
*  THE FLOOR IS THE FONT. With the cell at its natural 8x8 a window is as
*  small as it can be while still holding its text: the widest line the help
*  window prints is 29 characters, so that window is 29 * 8 + padding + frame
*  = 242 pixels and there is no honest way to make it 200. That is the size
*  used at 1024x768 and below, and it is why the windows there are now less
*  than half the area they were.
*
*  Above 1280 pixels wide the cell doubles to 16x16 and every window doubles
*  with it. One step and not a continuous scale, because the font can only be
*  blown up by whole pixels and a 1.5x glyph is a smear. The effect is that
*  1024x768 and 1920x1080 show windows of about the same size RELATIVE TO THE
*  SCREEN -- 128 by 96 cells against 120 by 67 -- while 640x480, where 80 by
*  60 cells is all there is, gets windows that are proportionally larger
*  because they have run into the floor and cannot be smaller.
*
*  ------------------------------------------------------------------------
*  Leaving
*  ------------------------------------------------------------------------
*  Escape calls gfx_close(), which calls sys_unmapfb(), which makes the kernel
*  repaint its console. Every exit path goes through it, including the one for
*  a screen that could not be set up in the first place: a program that leaves
*  without giving the screen back leaves a screen nobody owns and nobody
*  repaints, and the only way out of that is to reboot.
*
*  No framebuffer at all is an ORDINARY outcome, not a failure of this
*  program. "make run" boots into text mode because QEMU's -kernel loader does
*  not implement the Multiboot video request, so sys_mapfb() answers
*  SYS_ENODEV, and the right response is to say which boot does have a
*  framebuffer and stop.
*/
#include "syscall.h"
#include "lib.h"
#include "gfxlib.h"


/* ------------------------------------------------------------------ */
/* Exit statuses                                                       */
/* ------------------------------------------------------------------ */

/* main()'s return value is what the kernel records for the task, so it is the
*  only thing a caller that did not read the screen can go on. "There was no
*  screen" is deliberately distinct from "something went wrong": the first is
*  a property of how the machine booted and the second is a fault. */
#define GUI_OK           0
#define GUI_NO_SCREEN    1
#define GUI_FAILED       2


/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

/* How long sys_input() is allowed to wait. It bounds how long the clock can
*  be stale when nothing is happening, and it is what turns the loop from a
*  spin into a sleep. Not 0: sys_input() reads 0 as "wait forever", which is
*  right for a program with nothing else to do and wrong for one with a
*  clock in a window. */
#define GUI_INPUT_MS    20

/* The frame budget. Damage collected during a burst of mouse events is held
*  back until either the queue runs dry or this many milliseconds have passed,
*  so a fast drag becomes 60 frames a second rather than one frame per event.
*/
#define GUI_FRAME_MS    16

/* How often the live contents -- the clock, the pointer readout, the counters
*  -- are refreshed. EVERYTHING that changes with time goes through this one
*  tick rather than being damaged where it changes, so that the cost of the
*  live content is a fixed ten frames a second whatever the user is doing, and
*  a frame caused by the pointer moving carries only the pointer. */
#define GUI_TICK_MS    100

/* Rounds in the startup measurement. Enough that the millisecond resolution
*  of sys_uptime() is not the dominant error -- one full screen blit is
*  measured in milliseconds, so eight of them are measured to about a tenth of
*  one. */
#define GUI_BENCH_ROUNDS  8


/* ------------------------------------------------------------------ */
/* Window geometry                                                     */
/* ------------------------------------------------------------------ */

#define GUI_WINDOWS       4

/* The frame, and the only measurement in this file that is a plain number of
*  pixels. Two, because paint_window() draws the bevel as a light pair of
*  lines and a dark pair and a one pixel bevel is invisible at any of these
*  resolutions -- and because a border that grew with the cell would be four
*  pixels of pure chrome on every edge at 1920x1080, which is exactly the
*  heaviness this sizing pass set out to remove. */
#define GUI_BORDER        2

/* Where the cell doubles. Below this the font is drawn at its own 8x8, which
*  is the floor: it is the smallest a window holding its text can be. At or
*  above it the glyph is 16x16, which is the same apparent size on a 1920 wide
*  screen that 8x8 has on a 1024 wide one.
*
*  The height is tested too, so a wide but short mode -- 1280x720 is the one
*  that turns up -- is not given windows that then do not fit vertically. */
#define GUI_SCALE2_WIDTH   1280
#define GUI_SCALE2_HEIGHT   720

/* Derived from the mode by gui_metrics(), and read by everything else. They
*  are variables rather than macros for the reason the whole file exists: the
*  mode is not known until sys_mapfb() has answered. */
static int gui_scale;         /* 1 or 2: the font's magnification            */
static int gui_cell_w;        /* 8 or 16: one character                       */
static int gui_cell_h;
static int gui_line_h;        /* baseline to baseline in a body               */
static int gui_pad;           /* body inset, all four sides                   */
static int gui_title_h;       /* title bar height, inside the border          */
static int gui_keep_on;       /* draggable strip that stays on screen         */
static int gui_grid;          /* desktop grid spacing                         */
static int gui_margin;        /* screen edge to the window group              */

/* How much two windows next to each other in the cascade overlap. Enough to
*  be unmistakably an overlap and not a coincidence, since showing the Z order
*  is the reason the windows overlap at all. */
static int gui_overlap_x;
static int gui_overlap_y;

/* Fills the lot in from the surface. Every number below is a multiple of the
*  cell or of the scale, so the whole layout doubles in one step and nothing
*  can be left behind at its old size -- which is what a pile of independent
*  #defines would have made easy. */
static void gui_metrics(const gfx_surface *s)
{
	gui_scale = (s->w >= GUI_SCALE2_WIDTH && s->h >= GUI_SCALE2_HEIGHT)
	          ? 2 : 1;

	gui_cell_w = GFX_FONT_WIDTH  * gui_scale;
	gui_cell_h = GFX_FONT_HEIGHT * gui_scale;

	/* One scale-unit of leading above and below a line of text. At scale 1
	*  that is 8 pixels of glyph in a 10 pixel line, which is the density
	*  the kernel console uses and is readable; anything tighter runs the
	*  descenders of one line into the capitals of the next. */
	gui_line_h  = gui_cell_h + 2 * gui_scale;

	/* Chrome, and it is deliberately thin. The title bar used to be 24
	*  pixels around a 16 pixel glyph and the body inset 8 -- half a
	*  character of empty frame on every side of every window, which is
	*  most of what made them look heavy. Two scale-units above and below
	*  the glyph is enough to stop the text touching the frame. */
	gui_title_h = gui_cell_h + 4 * gui_scale;
	gui_pad     = 3 * gui_scale;

	/* A window may be dragged off the edges, but never so far that its
	*  title bar becomes unreachable -- a window with no grabbable strip
	*  left can never be brought back. Five characters of it always stay on
	*  screen, which is a strip the pointer can hit at either scale. */
	gui_keep_on = 5 * gui_cell_w;

	/* Four cells, so the desktop grid has the same density in characters
	*  at every resolution -- and so the number of lines paint_desktop()
	*  draws stays around thirty, whatever the mode. */
	gui_grid    = 4 * gui_cell_w;

	gui_margin  = gui_cell_w;

	gui_overlap_x = 5 * gui_cell_w;
	gui_overlap_y = gui_title_h + gui_line_h;
}


/* ------------------------------------------------------------------ */
/* The pointer                                                         */
/* ------------------------------------------------------------------ */

/* The classic arrow, as a picture rather than as a bitmap: 'X' is the outline,
*  '.' is the fill and a space is transparent. Written this way for the same
*  reason src/font8x8.c draws its glyphs in the comments -- the picture IS the
*  definition, and a mistake in it is visible in the source instead of hiding
*  in a hex constant.
*
*  Two colours and not one: a single colour arrow disappears the moment it
*  crosses something of the same colour, and an outlined one is visible over
*  the title bars, the window bodies and the desktop alike without anyone
*  having to choose colours that avoid it.
*
*  It is drawn at gui_scale, like everything else: 12x17 where the font is 8x8
*  and 24x34 where it is 16x16. Twelve by seventeen is not a speck -- it is
*  very nearly the size the classic X11 arrow had on the screens this
*  resolution came from -- and tying it to the same scale as the text is what
*  stops the pointer from being the one thing that does not change when the
*  mode does. */
#define GUI_CURSOR_W     12
#define GUI_CURSOR_H     17

static const char *const gui_cursor[GUI_CURSOR_H] = {
	"X           ",
	"XX          ",
	"X.X         ",
	"X..X        ",
	"X...X       ",
	"X....X      ",
	"X.....X     ",
	"X......X    ",
	"X.......X   ",
	"X........X  ",
	"X.....XXXXX ",
	"X..X..X     ",
	"X.X X..X    ",
	"XX  X..X    ",
	"X    X..X   ",
	"     X..X   ",
	"      XXX   "
};


/* ------------------------------------------------------------------ */
/* The model                                                           */
/* ------------------------------------------------------------------ */

/* What each window puts in its body. The content is a switch and not a
*  function pointer because there are four of them and a function pointer
*  table would be four more names to read before finding out that they all do
*  the same kind of thing. */
#define GUI_BODY_CLOCK    0
#define GUI_BODY_MOUSE    1
#define GUI_BODY_METRICS  2
#define GUI_BODY_HELP     3

typedef struct
{
	int         x;      /* filled in by gui_layout(), never by hand         */
	int         y;
	int         w;
	int         h;
	int         cols;   /* characters of text the body has to hold          */
	int         rows;   /* lines of it                                      */
	const char *title;
	int         body;
	int         live;   /* lines of content that change with time, 0 = none */
} gui_window;

/* The four windows, stated as CONTENT and not as pixels. cols and rows are
*  the size of the widest and tallest thing the matching paint_body_*() can
*  print, counted from those functions rather than estimated:
*
*    Clock    "1234 events, 5678 frames" is the long line, four lines deep
*             once the progress bar has taken the second slot
*    Pointer  "step -123,-123 " plus the three button squares, five deep
*    Metrics  "12345 ms in flush total" and "1234 frames 5678 KiB", six deep
*    Read me  "      and the console with it" at 29 characters is the widest
*             line in the whole program, and eleven lines is the deepest
*
*  A line longer than cols is not a fault -- paint_window() clips the body, so
*  it is cut off at the frame -- but it is a window that has stopped holding
*  its contents, so these numbers are the ones to change when the text is.
*
*  x, y, w and h are all computed. Deliberately overlapping, which the cascade
*  in gui_layout() guarantees at every resolution: a window system where
*  nothing overlaps never has to answer the question it exists to answer, and
*  a screenshot of one proves nothing about raising or about exposure. */
static gui_window gui_win[GUI_WINDOWS] = {
	{ 0, 0, 0, 0, 24,  4, "Clock",   GUI_BODY_CLOCK,   4 },
	{ 0, 0, 0, 0, 16,  5, "Pointer", GUI_BODY_MOUSE,   5 },
	{ 0, 0, 0, 0, 26,  6, "Metrics", GUI_BODY_METRICS, 6 },
	{ 0, 0, 0, 0, 29, 11, "Read me", GUI_BODY_HELP,    0 }
};

/* Back to front. gui_z[0] is the bottom window and gui_z[GUI_WINDOWS-1] is the
*  top one, so painting is a walk from 0 upwards and hit testing is the same
*  walk backwards. Storing the order as a list of indices rather than as a
*  field inside each window means raising is one memmove of a few ints and
*  cannot leave two windows claiming the same depth. */
static int gui_z[GUI_WINDOWS] = { 0, 1, 2, 3 };


/* ------------------------------------------------------------------ */
/* Live state                                                          */
/* ------------------------------------------------------------------ */

static int gui_ptr_x;            /* pointer, in screen pixels             */
static int gui_ptr_y;
static unsigned long gui_buttons;

static int gui_drag = -1;        /* window index being dragged, or -1     */
static int gui_drag_dx;          /* grab offset inside that window        */
static int gui_drag_dy;

static int gui_uptime;           /* milliseconds, refreshed every tick    */
static int gui_last_dx;          /* the last mouse step, for the display  */
static int gui_last_dy;
static unsigned long gui_events; /* input events seen                     */

/* Measurements. Everything here is put on the screen by the Metrics window,
*  so the claims gfxlib.h makes about the cost of a frame can be read off the
*  running program instead of taken on trust. */
static int           gui_bench_blit_us;   /* one full screen blit          */
static int           gui_bench_paint_us;  /* one full scene repaint        */
static unsigned long gui_frames;
static unsigned long gui_flush_bytes;     /* total copied to the screen    */
static unsigned long gui_flush_rects;
static int           gui_flush_ms;        /* total spent inside gfx_flush  */
static unsigned long gui_last_bytes;      /* the most recent frame         */
static int           gui_last_rects;
static unsigned long gui_min_bytes;       /* the cheapest frame so far     */

/* The colours, packed once against the screen's format at startup. A
*  gfx_color belongs to the surface it was made for; there is only one format
*  in this program because gfxlib.c gives the canvas the screen's. */
static gfx_color col_desk;
static gfx_color col_grid;
static gfx_color col_body;
static gfx_color col_frame;
static gfx_color col_light;
static gfx_color col_shadow;
static gfx_color col_title_on;
static gfx_color col_title_off;
static gfx_color col_title_text;
static gfx_color col_text;
static gfx_color col_dim;
static gfx_color col_bar;
static gfx_color col_cursor_fill;
static gfx_color col_cursor_edge;


/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int clampi(int v, int lo, int hi)
{
	if(v < lo) return lo;
	if(v > hi) return hi;
	return v;
}


/* ------------------------------------------------------------------ */
/* Laying the windows out                                              */
/* ------------------------------------------------------------------ */

/* The outer size of a window that has to hold cols x rows of text. The two
*  functions are the single definition of "how big is a window", and
*  win_body_rect() is their exact inverse -- change one and the text starts
*  landing on the frame. */
static int win_width_for(int cols)
{
	return 2 * GUI_BORDER + 2 * gui_pad + cols * gui_cell_w;
}

static int win_height_for(int rows)
{
	return 2 * GUI_BORDER + gui_title_h + 2 * gui_pad + rows * gui_line_h;
}

/* Places the windows in a cascade running down and to the right, each one
*  starting gui_overlap_x/y before the previous one ends, with the steps
*  shrunk by "tighten" (out of 256) when the full cascade would not fit.
*
*  A cascade rather than a grid, for one reason: EVERY CONSECUTIVE PAIR
*  OVERLAPS BY CONSTRUCTION, at any resolution and any window size, because
*  the step is the window's own size minus the overlap and never more. A grid
*  of quadrants overlaps only when the windows happen to be more than half the
*  cell wide, which is true at 640x480 and false at 1920x1080 -- and a window
*  system whose Z order stops being visible on the big screen is exactly the
*  thing that cannot be checked from the source.
*
*  Positions come out relative to (0,0); gui_layout() moves the whole group. */
static void layout_cascade(int tighten)
{
	int i, x, y, step;

	x = 0;
	y = 0;

	for(i = 0; i < GUI_WINDOWS; i++)
	{
		gui_win[i].x = x;
		gui_win[i].y = y;

		/* A window narrower than the overlap would step backwards and
		*  put the next one to its LEFT, which is not a cascade and is
		*  the one way this loop could produce a shape nobody intended.
		*  Zero instead: two windows exactly on top of each other still
		*  show a Z order, just less of one. */
		step = gui_win[i].w - gui_overlap_x;
		if(step < 0) step = 0;
		x += (step * tighten) / 256;

		step = gui_win[i].h - gui_overlap_y;
		if(step < 0) step = 0;
		y += (step * tighten) / 256;
	}
}

/* The bounding box of the whole cascade, measured from (0,0). Not simply the
*  last window's far corner: the cascade ends with the widest window but there
*  is no rule saying it must, and a bounding box that assumed so would be
*  wrong the moment the order changed. */
static void layout_extent(int *out_w, int *out_h)
{
	int i, w, h;

	w = 0;
	h = 0;

	for(i = 0; i < GUI_WINDOWS; i++)
	{
		if(gui_win[i].x + gui_win[i].w > w)
			w = gui_win[i].x + gui_win[i].w;
		if(gui_win[i].y + gui_win[i].h > h)
			h = gui_win[i].y + gui_win[i].h;
	}

	*out_w = w;
	*out_h = h;
}

/* Sizes and places every window for the screen actually in front of us.
*
*  Two steps. First the sizes, which come from the content and from nothing
*  else. Then the cascade, tightened until the group fits between the margins:
*  tightening only ever increases the overlap, so a screen too small for the
*  natural cascade loses spread and never loses a window off the edge -- which
*  is the failure that matters, because a window whose title bar is off screen
*  cannot be dragged back.
*
*  The search is a walk down from 256 in steps of 4 rather than anything
*  cleverer. It runs once, at startup, over four rectangles; a binary search
*  would be the same answer reached less obviously.
*
*  Finally the group is centred. Centred and not anchored top left, because at
*  1920x1080 four windows sized for their text cannot fill the screen however
*  they are arranged -- the honest choice is between a cluster in the middle
*  and a cluster in the corner, and the middle is the one that looks
*  deliberate. */
static void gui_layout(const gfx_surface *s)
{
	int i, tighten, w, h, avail_w, avail_h, ox, oy;

	for(i = 0; i < GUI_WINDOWS; i++)
	{
		gui_win[i].w = win_width_for(gui_win[i].cols);
		gui_win[i].h = win_height_for(gui_win[i].rows);
	}

	avail_w = s->w - 2 * gui_margin;
	avail_h = s->h - 2 * gui_margin;

	for(tighten = 256; tighten > 0; tighten -= 4)
	{
		layout_cascade(tighten);
		layout_extent(&w, &h);

		if(w <= avail_w && h <= avail_h)
			break;
	}

	/* Even fully stacked the group can be larger than the screen, on a
	*  mode too small to hold the widest window at all. Nothing can be done
	*  about that here and nothing needs to be: the windows are placed at
	*  the margin and gfxlib clips whatever hangs over. */
	if(tighten <= 0)
	{
		layout_cascade(0);
		layout_extent(&w, &h);
	}

	ox = gui_margin + (avail_w - w) / 2;
	oy = gui_margin + (avail_h - h) / 2;
	if(ox < 0) ox = 0;
	if(oy < 0) oy = 0;

	for(i = 0; i < GUI_WINDOWS; i++)
	{
		gui_win[i].x += ox;
		gui_win[i].y += oy;
	}
}

/* The rectangle a window occupies on the screen, frame included. Every damage
*  calculation goes through this rather than repeating x, y, w, h, so that a
*  window growing a shadow or a resize grip later cannot leave a damage
*  rectangle a few pixels short -- which shows up as a thin line of stale
*  pixels that nothing ever repaints. */
static void win_rect(const gui_window *win, gfx_rect *r)
{
	r->x = win->x;
	r->y = win->y;
	r->w = win->w;
	r->h = win->h;
}

/* The area inside the frame and below the title bar: where the content goes,
*  and the only part that has to be damaged when only the content changed. */
static void win_body_rect(const gui_window *win, gfx_rect *r)
{
	r->x = win->x + GUI_BORDER;
	r->y = win->y + GUI_BORDER + gui_title_h;
	r->w = win->w - 2 * GUI_BORDER;
	r->h = win->h - 2 * GUI_BORDER - gui_title_h;

	if(r->w < 0) r->w = 0;
	if(r->h < 0) r->h = 0;
}

/* The part of a window that changes on its own, as opposed to when it moves:
*  the first "live" lines of its body, and never its frame or its title bar.
*
*  Sizing windows to their contents took most of this function's work away and
*  it is worth saying so rather than leaving a reader to wonder. All three live
*  windows now declare as many live lines as they have rows, because every line
*  they print is a clock or a counter -- so the live rectangle IS the body for
*  all three and the trim never fires today. What it still buys is the chrome:
*  the tick damages 192x46 of a 202x62 Clock at scale 1 rather than all of it,
*  which is a third of the pixels for free, ten times a second, forever.
*
*  It stays because the trim is the part that cannot be reconstructed later.
*  The moment a window grows a static line under its live ones -- a caption, a
*  legend, a key -- damaging the whole body starts repainting text that never
*  changes, and that is a cost nobody would go looking for. */
static void win_live_rect(const gui_window *win, gfx_rect *r)
{
	int used;

	win_body_rect(win, r);

	used = win->live * gui_line_h + 2 * gui_pad;
	if(r->h > used)
		r->h = used;
}

/* The rectangle the pointer covers at a given position. The hotspot is the
*  arrow's tip, which is its top left pixel, so the rectangle starts there. */
static void cursor_rect(int x, int y, gfx_rect *r)
{
	r->x = x;
	r->y = y;
	r->w = GUI_CURSOR_W * gui_scale;
	r->h = GUI_CURSOR_H * gui_scale;
}

static void damage_rect(const gfx_rect *r)
{
	gfx_damage(r->x, r->y, r->w, r->h);
}

/* Formats milliseconds as h:mm:ss.t. Written out rather than done with a
*  single printf because the fields need different widths and zero padding,
*  and because the tenths have to come from the same division as the seconds
*  or the two disagree for one frame every second. */
static void format_uptime(char *buf, size_t size, int ms)
{
	int total;
	int tenths, seconds, minutes, hours;

	total = (ms < 0) ? 0 : ms;

	tenths  = (total / 100) % 10;
	seconds = (total / 1000) % 60;
	minutes = (total / 60000) % 60;
	hours   = total / 3600000;

	snprintf(buf, size, "%d:%02d:%02d.%d", hours, minutes, seconds, tenths);
}


/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

/* The desktop. A flat colour plus a grid, and the grid is not decoration: it
*  is the evidence. A region that a window has just been dragged off has to
*  come back with the grid lines running through it, continuous with their
*  neighbours. A repaint that got the exposed area wrong -- painted it in the
*  wrong colour, offset it by a pixel, or left it alone -- shows up
*  immediately as a broken line, where a flat background would have hidden all
*  three.
*
*  The lines are drawn across the whole surface and clipped away by the clip
*  window. The spacing is four character cells rather than a fixed 32 pixels,
*  which keeps that count near sixty at every resolution -- at a fixed 32 a
*  1920x1080 desktop would cost 94 span calls per damage rectangle instead of
*  47 -- and keeps the grid the same density relative to the text. Almost all
*  of the calls return after one comparison, against a fill of the same
*  rectangle. */
static void paint_desktop(gfx_surface *c)
{
	int i;

	gfx_clear(c, col_desk);

	for(i = 0; i < c->w; i += gui_grid)
		gfx_vline(c, i, 0, c->h, col_grid);
	for(i = 0; i < c->h; i += gui_grid)
		gfx_hline(c, 0, i, c->w, col_grid);

	/* 59 characters, which is 472 pixels at scale 1 and 944 at scale 2 --
	*  inside the margin on the narrowest screen either scale is used on
	*  (640 and 1280). gfx_text() would clip the rest rather than wrap it,
	*  which is correct and still looks like a mistake, so the string is
	*  written to fit rather than trusted to be cut off tidily. */
	gfx_text(c, 2 * gui_cell_w, gui_cell_h,
	         "TomatOS window system: drag a title, click to raise, Esc out",
	         gui_scale, col_title_text);
}

/* One line of body text, at line number "line" counting from 0. */
static void body_line(gfx_surface *c, const gfx_rect *body, int line,
                      const char *text, gfx_color colour)
{
	gfx_text(c, body->x + gui_pad, body->y + gui_pad + line * gui_line_h,
	         text, gui_scale, colour);
}

static void paint_body_clock(gfx_surface *c, const gfx_rect *body)
{
	char line[64];
	int bar_x, bar_y, bar_w, bar_h, filled;

	format_uptime(line, sizeof(line), gui_uptime);
	body_line(c, body, 0, "Up", col_dim);
	gfx_text(c, body->x + gui_pad + 3 * gui_cell_w,
	         body->y + gui_pad, line, gui_scale, col_text);

	/* A bar that fills across each second. The digits above prove the clock
	*  is running; the bar proves the program is redrawing between the
	*  digits changing, which a still picture of a running clock cannot.
	*
	*  It lives in the second line's slot, inset by one scale-unit top and
	*  bottom, rather than at a fixed offset of its own. That is what keeps
	*  it out of the third line at scale 1: at 20 pixels a line an absolute
	*  14 pixel bar with a 6 pixel offset fitted, and at 10 it would have
	*  reached four pixels into the line below. */
	bar_x = body->x + gui_pad;
	bar_y = body->y + gui_pad + gui_line_h + gui_scale;
	bar_w = body->w - 2 * gui_pad;
	bar_h = gui_line_h - 2 * gui_scale;

	if(bar_w > 0)
	{
		filled = (bar_w * ((gui_uptime % 1000) / 10)) / 100;
		gfx_fill_rect(c, bar_x, bar_y, bar_w, bar_h, col_shadow);
		gfx_fill_rect(c, bar_x, bar_y, filled, bar_h, col_bar);
		gfx_draw_rect(c, bar_x, bar_y, bar_w, bar_h, col_frame);
	}

	snprintf(line, sizeof(line), "%lu events, %lu frames",
	         gui_events, gui_frames);
	body_line(c, body, 3, line, col_dim);
}

static void paint_body_mouse(gfx_surface *c, const gfx_rect *body)
{
	char line[64];
	int i, bx, by, box, step, inset;

	snprintf(line, sizeof(line), "at %4d,%-4d", gui_ptr_x, gui_ptr_y);
	body_line(c, body, 0, line, col_text);

	snprintf(line, sizeof(line), "step %4d,%-4d", gui_last_dx, gui_last_dy);
	body_line(c, body, 1, line, col_dim);

	/* Three squares, filled while the matching button is down. Squares and
	*  not text because a pressed button lasts a fraction of a second and a
	*  word that appears and disappears is harder to catch than a box that
	*  fills in.
	*
	*  A box is one line high and holds one glyph centred in it, so the row
	*  of them occupies the third line's slot exactly and needs no space
	*  reserved for it in the window's row count. */
	box   = gui_line_h;
	step  = box + 2 * gui_scale;
	inset = (box - gui_cell_w) / 2;

	bx = body->x + gui_pad;
	by = body->y + gui_pad + 2 * gui_line_h;

	for(i = 0; i < 3; i++)
	{
		int on = (gui_buttons & (1u << i)) != 0;

		gfx_fill_rect(c, bx + i * step, by, box, box,
		              on ? col_bar : col_shadow);
		gfx_draw_rect(c, bx + i * step, by, box, box, col_frame);
		/* The letter is dark when the button is up and white when it is
		*  down, and in both cases it is the strongest contrast available
		*  against the fill behind it. It used to be col_dim on col_shadow --
		*  0x505050 on 0x707068, two mid greys a shade apart, which is legible
		*  at 8x8 only if you already know what it says.
		*
		*  The state is carried by the FILL, which is what a real button does:
		*  the label says which button it is and does not change, the surface
		*  says whether it is pressed. Signalling the state by dimming the
		*  label instead makes the one thing that must stay readable the thing
		*  that gets harder to read. */
		gfx_char(c, bx + i * step + inset, by + (box - gui_cell_h) / 2,
		         (i == 0) ? 'L' : ((i == 1) ? 'R' : 'M'),
		         gui_scale, on ? col_title_text : col_text);
	}

	body_line(c, body, 4, (gui_drag >= 0) ? "dragging" : "", col_text);
}

static void paint_body_metrics(gfx_surface *c, const gfx_rect *body)
{
	char line[64];

	snprintf(line, sizeof(line), "full blit  %d.%02d ms",
	         gui_bench_blit_us / 1000, (gui_bench_blit_us / 10) % 100);
	body_line(c, body, 0, line, col_text);

	snprintf(line, sizeof(line), "full paint %d.%02d ms",
	         gui_bench_paint_us / 1000, (gui_bench_paint_us / 10) % 100);
	body_line(c, body, 1, line, col_text);

	snprintf(line, sizeof(line), "frame %d rect %lu B",
	         gui_last_rects, gui_last_bytes);
	body_line(c, body, 2, line, col_dim);

	snprintf(line, sizeof(line), "%lu frames %lu KiB",
	         gui_frames, gui_flush_bytes / 1024);
	body_line(c, body, 3, line, col_dim);

	snprintf(line, sizeof(line), "%d ms in flush total", gui_flush_ms);
	body_line(c, body, 4, line, col_dim);

	snprintf(line, sizeof(line), "cheapest %lu B", gui_min_bytes);
	body_line(c, body, 5, line, col_text);
}

static void paint_body_help(gfx_surface *c, const gfx_rect *body)
{
	body_line(c, body, 0, "Drag  a title bar moves it",  col_text);
	body_line(c, body, 1, "Click anywhere raises it",    col_text);
	body_line(c, body, 2, "Tab   raises the bottom one", col_text);
	body_line(c, body, 3, "Esc   gives the screen back", col_text);
	body_line(c, body, 4, "      and the console with it", col_dim);
	body_line(c, body, 5, "q     the same as Esc",        col_dim);

	/* Not a property of this program, and said anyway because it is the
	*  first thing that looks like a bug in it. The shell waits RUN_WAIT_MS
	*  (30 seconds, src/main.c) for a program it started and then goes back
	*  to its prompt -- where it reads the keyboard. There is ONE key slot
	*  in src/kb.c and SYS_GETCH and SYS_INPUT both take from it, so from
	*  that moment the shell wins every keystroke and Escape never arrives
	*  here. The pointer is unaffected: the mouse has a queue of its own. */
	body_line(c, body, 7, "After 30 s the shell takes",  col_dim);
	body_line(c, body, 8, "the keyboard back and Esc",   col_dim);
	body_line(c, body, 9, "stops arriving; the mouse",   col_dim);
	body_line(c, body, 10, "keeps working.",             col_dim);
}

/* One window, frame and all. "active" is only about the colour of the title
*  bar: the top of the Z order is the one that would receive a keystroke if
*  this program had anything to type into, and showing which one that is
*  costs a colour. */
static void paint_window(gfx_surface *c, const gui_window *win, int active)
{
	gfx_rect body;
	gfx_rect saved;
	int tx, ty;

	/* Body first, then the frame over it, so a body a pixel too large
	*  cannot paint over the frame. */
	gfx_fill_rect(c, win->x, win->y, win->w, win->h, col_body);

	/* A raised edge: light along the top and left, dark along the bottom
	*  and right. Two lines each, and the reason for two rather than one is
	*  that a single pixel bevel is invisible at this resolution. */
	gfx_hline(c, win->x, win->y, win->w, col_light);
	gfx_hline(c, win->x + 1, win->y + 1, win->w - 2, col_light);
	gfx_vline(c, win->x, win->y, win->h, col_light);
	gfx_vline(c, win->x + 1, win->y + 1, win->h - 2, col_light);
	gfx_hline(c, win->x, win->y + win->h - 1, win->w, col_shadow);
	gfx_hline(c, win->x + 1, win->y + win->h - 2, win->w - 2, col_shadow);
	gfx_vline(c, win->x + win->w - 1, win->y, win->h, col_shadow);
	gfx_vline(c, win->x + win->w - 2, win->y + 1, win->h - 2, col_shadow);

	/* The title bar. Its height is what the hit test uses to decide that a
	*  press starts a drag, so the two must agree -- see gui_press(). */
	gfx_fill_rect(c, win->x + GUI_BORDER, win->y + GUI_BORDER,
	              win->w - 2 * GUI_BORDER, gui_title_h,
	              active ? col_title_on : col_title_off);

	tx = win->x + GUI_BORDER + gui_pad;
	ty = win->y + GUI_BORDER + (gui_title_h - gui_cell_h) / 2;
	gfx_text(c, tx, ty, win->title, gui_scale,
	         active ? col_title_text : col_dim);

	gfx_hline(c, win->x + GUI_BORDER, win->y + GUI_BORDER + gui_title_h,
	          win->w - 2 * GUI_BORDER, col_frame);

	win_body_rect(win, &body);

	/* The contents are clipped to the body ON TOP OF the damage rectangle
	*  already in force, so a line of text longer than the window is cut off
	*  at the frame instead of running out onto the desktop. Saved and put
	*  back rather than reset, because the outer clip is the damage
	*  rectangle being repainted and losing it would let the next window
	*  paint over the whole screen. */
	gfx_clip_get(c, &saved);
	gfx_clip_intersect(c, body.x, body.y, body.w, body.h);

	switch(win->body)
	{
		case GUI_BODY_CLOCK:   paint_body_clock(c, &body);   break;
		case GUI_BODY_MOUSE:   paint_body_mouse(c, &body);   break;
		case GUI_BODY_METRICS: paint_body_metrics(c, &body); break;
		default:               paint_body_help(c, &body);    break;
	}

	gfx_clip_set(c, saved.x, saved.y, saved.w, saved.h);
}

/* The pointer, painted last so it is over everything. Row by row and run by
*  run rather than pixel by pixel, so that the horizontal stretches of the
*  arrow are one span each -- at scale 2 a run of five is a 10 pixel span
*  drawn twice instead of ten separate blocks. Painted at gui_scale, so the
*  pointer is 12x17 where the text is 8x8 and 24x34 where it is 16x16. */
static void paint_cursor(gfx_surface *c)
{
	int row, col, run;
	char ch;

	for(row = 0; row < GUI_CURSOR_H; row++)
	{
		col = 0;
		while(col < GUI_CURSOR_W)
		{
			ch = gui_cursor[row][col];
			if(ch == ' ')
			{
				col++;
				continue;
			}

			run = 1;
			while(col + run < GUI_CURSOR_W &&
			      gui_cursor[row][col + run] == ch)
				run++;

			gfx_fill_rect(c,
			              gui_ptr_x + col * gui_scale,
			              gui_ptr_y + row * gui_scale,
			              run * gui_scale, gui_scale,
			              (ch == 'X') ? col_cursor_edge : col_cursor_fill);

			col += run;
		}
	}
}

/* The entire picture, into whatever part of the canvas the clip window
*  allows. Bottom of the Z order first, so a later window covers an earlier
*  one; the pointer last, so it covers everything.
*
*  This is called once per damage rectangle and knows nothing about damage.
*  That is the point: there is exactly one function that says what the screen
*  looks like, and every case -- a move, a raise, a tick of the clock, the
*  very first frame -- is that one function with a different clip window. */
static void paint_scene(gfx_surface *c)
{
	int i;

	paint_desktop(c);

	for(i = 0; i < GUI_WINDOWS; i++)
		paint_window(c, &gui_win[gui_z[i]], i == GUI_WINDOWS - 1);

	paint_cursor(c);
}

/* Repaints every pending damage rectangle and copies them to the screen.
*
*  The damage list is read before anything is painted and is not added to
*  while the loop runs -- painting does not damage anything, by construction,
*  because painting is a pure function of the model. */
static void gui_frame(void)
{
	gfx_surface *c = gfx_canvas();
	gfx_rect r;
	int n, i, t0, t1;

	n = gfx_damage_count();
	if(n == 0)
		return;

	for(i = 0; i < n; i++)
	{
		if(!gfx_damage_at(i, &r))
			continue;

		gfx_clip_set(c, r.x, r.y, r.w, r.h);
		paint_scene(c);
	}
	gfx_clip_none(c);

	t0 = sys_uptime();
	gui_last_bytes = gfx_flush();
	t1 = sys_uptime();

	/* The cheapest frame is the interesting one: it is a frame in which
	*  nothing happened but the pointer moving, and it is what the damage
	*  list is FOR. Kept as a minimum rather than averaged, because the
	*  average is dominated by the periodic content refresh and would hide
	*  exactly the number worth knowing. */
	if(gui_last_bytes != 0 &&
	   (gui_min_bytes == 0 || gui_last_bytes < gui_min_bytes))
		gui_min_bytes = gui_last_bytes;

	gui_last_rects = n;
	gui_flush_rects += (unsigned long)n;
	gui_flush_bytes += gui_last_bytes;
	gui_flush_ms += (t1 - t0);
	gui_frames++;
}


/* ------------------------------------------------------------------ */
/* Hit testing and the Z order                                         */
/* ------------------------------------------------------------------ */

/* Which window is under a point, as a position in the Z order, or -1.
*
*  THE WALK IS FROM THE TOP DOWN and stops at the first window that contains
*  the point. That order is the whole of the answer to "what happens when I
*  click where two windows overlap": the one in front gets it, because it is
*  the one the user can see there. Walking from the bottom up would return the
*  window the user cannot see, and would do it only in the overlapping
*  region -- which is the kind of bug that looks like the mouse being
*  inaccurate rather than like a wrong loop. */
static int hit_test(int x, int y)
{
	int i;
	const gui_window *w;

	for(i = GUI_WINDOWS - 1; i >= 0; i--)
	{
		w = &gui_win[gui_z[i]];

		if(x >= w->x && x < w->x + w->w &&
		   y >= w->y && y < w->y + w->h)
			return i;
	}

	return -1;
}

/* Moves the window at position zi to the top of the Z order, everything above
*  it sliding down one. Returns non-zero when the order actually changed, so a
*  click on the window that is already in front does not damage anything.
*
*  RAISING HAPPENS AFTER THE HIT TEST, NEVER BEFORE. Raising changes the
*  answer the hit test gives, so a hit test run afterwards would always name
*  the window that was just raised -- which is only accidentally the right
*  answer, and stops being right the moment a click lands on the desktop
*  between two windows. The order is: decide what was clicked, then act on it.
*
*  It also matters for the drag. The grab offset is worked out from the
*  window the hit test found, and raising does not move a window, so the
*  offset stays valid -- which is why one press can both raise a partly
*  covered window and start dragging it, instead of the user having to click
*  once to raise and again to drag. */
static int raise_window(int zi)
{
	gfx_rect r;
	int id;
	int i;

	if(zi < 0 || zi >= GUI_WINDOWS)
		return 0;
	if(zi == GUI_WINDOWS - 1)
		return 0;               /* already in front */

	/* TWO rectangles change, and only one of them is obvious.
	*
	*  The window coming forward is the obvious one: parts of it that were
	*  covered are not covered any more.
	*
	*  The window that WAS in front is the one that gets forgotten. It does
	*  not move and nothing uncovers it, but paint_window() draws it
	*  differently now -- an inactive title bar is a different colour -- so
	*  its pixels change where nothing about its geometry did. Leaving it
	*  out is a bug that hides: the raised window's own damage repaints
	*  whatever it overlaps, so the stale part is only the strip of the old
	*  title bar that nothing else happened to cover, and it looks like a
	*  rendering glitch rather than like a missing damage rectangle. A
	*  screenshot caught exactly that -- half a title bar red, half grey.
	*
	*  Its WHOLE rectangle and not just its title bar. The title bar is the
	*  only part that differs today, and tying this line to that fact would
	*  make it a second place to remember the moment paint_window() learns
	*  to draw anything else differently for the window in front. A window
	*  sized blit on a click is not a cost worth that risk. */
	win_rect(&gui_win[gui_z[GUI_WINDOWS - 1]], &r);
	damage_rect(&r);

	id = gui_z[zi];
	for(i = zi; i < GUI_WINDOWS - 1; i++)
		gui_z[i] = gui_z[i + 1];
	gui_z[GUI_WINDOWS - 1] = id;

	win_rect(&gui_win[id], &r);
	damage_rect(&r);

	return 1;
}


/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

/* A press. Decides what was hit, raises it, and starts a drag if the press
*  landed on the title bar. */
static void gui_press(int x, int y)
{
	int zi;
	gui_window *w;

	zi = hit_test(x, y);
	if(zi < 0)
	{
		gui_drag = -1;
		return;                 /* the desktop; nothing to raise */
	}

	w = &gui_win[gui_z[zi]];

	/* raise_window() records its own damage: it is the only place that
	*  knows both which window came forward and which one stopped being in
	*  front, and those are the two that change. */
	raise_window(zi);

	/* The title bar strip, measured exactly as paint_window() draws it. The
	*  body is not draggable: dragging by the body is a thing some window
	*  systems offer with a modifier key, and offering it without one makes
	*  a click on a button impossible to tell from the start of a drag. */
	if(y >= w->y + GUI_BORDER && y < w->y + GUI_BORDER + gui_title_h &&
	   x >= w->x + GUI_BORDER && x < w->x + w->w - GUI_BORDER)
	{
		gui_drag = gui_z[GUI_WINDOWS - 1];
		gui_drag_dx = x - w->x;
		gui_drag_dy = y - w->y;
	}
	else
	{
		gui_drag = -1;
	}
}

/* The pointer moved. Three things can need repainting: where the pointer was,
*  where it is now, and -- if a window is being dragged -- where that window
*  was and where it is now.
*
*  The old and new rectangles are damaged separately rather than as one
*  bounding box. gfx_damage() merges them when they overlap, which they do for
*  a slow drag, and keeps them apart when they do not, which is what stops a
*  fast flick across the screen from repainting everything in between. */
static void gui_motion(int x, int y)
{
	gfx_surface *c = gfx_canvas();
	gfx_rect r;
	gui_window *w;
	int nx, ny;

	/* The pointer's old position, damaged first. Repainting that rectangle
	*  is what erases the pointer: the repaint draws the scene there without
	*  a pointer in it, because the pointer is no longer there. */
	cursor_rect(gui_ptr_x, gui_ptr_y, &r);
	damage_rect(&r);

	if(gui_drag >= 0)
	{
		w = &gui_win[gui_drag];

		nx = x - gui_drag_dx;
		ny = y - gui_drag_dy;

		/* Kept reachable: enough of the title bar stays on screen that
		*  it can always be grabbed again, and the top edge never goes
		*  above the screen, because a title bar above the top edge is a
		*  window that can be moved down but never up. */
		nx = clampi(nx, gui_keep_on - w->w, c->w - gui_keep_on);
		ny = clampi(ny, 0, c->h - gui_keep_on);

		if(nx != w->x || ny != w->y)
		{
			win_rect(w, &r);        /* where it was */
			damage_rect(&r);

			w->x = nx;
			w->y = ny;

			win_rect(w, &r);        /* where it is now */
			damage_rect(&r);
		}
	}

	gui_ptr_x = clampi(x, 0, c->w - 1);
	gui_ptr_y = clampi(y, 0, c->h - 1);

	cursor_rect(gui_ptr_x, gui_ptr_y, &r);
	damage_rect(&r);
}

/* The periodic refresh: the windows whose content is a function of time get
*  their BODY damaged, not their whole rectangle. The frame and the title bar
*  have not changed, and repainting a body is a third of the pixels. */
static void gui_tick(void)
{
	gfx_rect r;
	int i;

	for(i = 0; i < GUI_WINDOWS; i++)
	{
		if(gui_win[i].live <= 0)
			continue;

		win_live_rect(&gui_win[i], &r);
		damage_rect(&r);
	}
}


/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

static void setup_colours(void)
{
	gfx_surface *s = gfx_screen();

	col_desk        = gfx_rgb(s, 0x1E, 0x3A, 0x4A);
	col_grid        = gfx_rgb(s, 0x28, 0x48, 0x5A);
	col_body        = gfx_rgb(s, 0xC8, 0xC8, 0xC0);
	col_frame       = gfx_rgb(s, 0x30, 0x30, 0x30);
	col_light       = gfx_rgb(s, 0xF0, 0xF0, 0xE8);
	col_shadow      = gfx_rgb(s, 0x70, 0x70, 0x68);
	col_title_on    = gfx_rgb(s, 0xC0, 0x30, 0x18);   /* tomato            */
	col_title_off   = gfx_rgb(s, 0x88, 0x88, 0x80);
	col_title_text  = gfx_rgb(s, 0xFF, 0xFF, 0xFF);
	col_text        = gfx_rgb(s, 0x10, 0x10, 0x10);
	col_dim         = gfx_rgb(s, 0x50, 0x50, 0x50);
	col_bar         = gfx_rgb(s, 0x20, 0x90, 0x40);
	col_cursor_fill = gfx_rgb(s, 0xFF, 0xFF, 0xFF);
	col_cursor_edge = gfx_rgb(s, 0x00, 0x00, 0x00);
}

/* Times what the two expensive things actually cost, so the Metrics window
*  can report numbers rather than adjectives.
*
*  Both are timed by repetition: sys_uptime() has millisecond resolution and a
*  single operation is a few milliseconds at most, so one measurement would be
*  20 percent quantisation error. GUI_BENCH_ROUNDS of them divided back down
*  is good to about a tenth of a millisecond.
*
*  It is safe to run this with the finished picture already in the canvas: the
*  blits put the same picture on the screen eight times and the repaints
*  rebuild the same canvas eight times, so nothing flickers and nothing is
*  lost. */
static void benchmark(void)
{
	gfx_surface *c = gfx_canvas();
	int t0, t1, i;

	t0 = sys_uptime();
	for(i = 0; i < GUI_BENCH_ROUNDS; i++)
		gfx_blit(0, 0, c->w, c->h);
	t1 = sys_uptime();
	gui_bench_blit_us = ((t1 - t0) * 1000) / GUI_BENCH_ROUNDS;

	t0 = sys_uptime();
	for(i = 0; i < GUI_BENCH_ROUNDS; i++)
		paint_scene(c);
	t1 = sys_uptime();
	gui_bench_paint_us = ((t1 - t0) * 1000) / GUI_BENCH_ROUNDS;
}

/* Says why there is no picture, in the terms the person in front of the
*  machine can act on. SYS_ENODEV is the one that needs the paragraph: it is
*  not a fault, it is how "make run" boots, and the answer is a different
*  boot rather than a different program. */
static void explain_no_screen(int rc)
{
	switch(rc)
	{
		case SYS_ENODEV:
			printf("gui: this machine booted into text mode -- there is no\n");
			printf("     framebuffer to draw on, which is normal and not a\n");
			printf("     fault. QEMU's \"-kernel\" loader, which \"make run\"\n");
			printf("     uses, does not implement the Multiboot video request.\n");
			printf("     Boot the ISO or the boot disk instead:\n");
			printf("         make run-iso\n");
			printf("         make run-bootdisk\n");
			break;

		case SYS_EBUSY:
			printf("gui: another task is holding the screen. Only one may:\n");
			printf("     the kernel console lives on it too and stops\n");
			printf("     painting while somebody else has it.\n");
			break;

		case SYS_ENOMEM:
			printf("gui: the screen is larger than %dx%d, which is more than\n",
			       GFX_MAX_WIDTH, GFX_MAX_HEIGHT);
			printf("     the back buffer compiled into this program can hold.\n");
			printf("     That buffer is .bss and cannot grow at run time, so\n");
			printf("     the way out is a smaller mode or a taller ceiling in\n");
			printf("     user/gfxlib.h -- which costs 4 more bytes of memory\n");
			printf("     per pixel, in every program that draws.\n");
			break;

		case SYS_ENOSYS:
			printf("gui: this kernel has no sys_mapfb() -- it is older than\n");
			printf("     this program. Nothing to do but boot a newer one.\n");
			break;

		default:
			printf("gui: could not take the screen, error %d.\n", rc);
			break;
	}
}


/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	sys_input_event ev;
	gfx_surface *c;
	int rc;
	int next_tick;
	int last_flush;
	int now;
	int running;

	/* There are no options. Anything on the command line is a mistake, and
	*  saying so beats ignoring it. */
	if(argc > 1)
	{
		printf("gui: takes no arguments (\"%s\" was given).\n", argv[1]);
		printf("Syntax: gui\n");
		printf("        Escape gives the screen back.\n");
		return GUI_FAILED;
	}

	rc = gfx_open();
	if(rc < 0)
	{
		explain_no_screen(rc);
		return (rc == SYS_ENODEV) ? GUI_NO_SCREEN : GUI_FAILED;
	}

	c = gfx_canvas();
	setup_colours();

	/* Everything about how big things are, in this order and before
	*  anything is drawn or hit tested: the metrics come from the mode, the
	*  window sizes come from the metrics, and the placement comes from the
	*  sizes. Nothing in this program has a position or a size until these
	*  two calls have run. */
	gui_metrics(c);
	gui_layout(c);

	/* The pointer starts in the middle. The kernel has a position of its
	*  own and the first mouse event will overwrite this, but there may
	*  never be one -- a machine with no mouse still gets a window system,
	*  it just gets one with a pointer that does not move. */
	gui_ptr_x = c->w / 2;
	gui_ptr_y = c->h / 2;
	gui_uptime = sys_uptime();

	/* The first frame is the one case where the whole screen really is new:
	*  what is on it is the console's last picture, and none of it is ours.
	*  So it is painted unclipped and blitted whole, without going through
	*  the damage list at all. */
	gfx_clip_none(c);
	paint_scene(c);
	gfx_blit(0, 0, c->w, c->h);

	benchmark();

	/* The benchmark left the canvas correct but the counters interesting,
	*  so put the numbers on the screen straight away rather than waiting
	*  for the first tick. */
	gui_tick();
	gui_frame();

	now = sys_uptime();
	next_tick = now + GUI_TICK_MS;
	last_flush = now;
	running = 1;

	while(running)
	{
		rc = sys_input(&ev, GUI_INPUT_MS);

		if(rc == 1)
		{
			gui_events++;

			if(ev.type == SYS_INPUT_KEY)
			{
				if(ev.key == 27 || ev.key == 'q' || ev.key == 'Q')
				{
					running = 0;
					continue;
				}

				if(ev.key == '\t')
				{
					/* Raises the bottom window. Worth having for
					*  its own sake, and it is also the only way
					*  to demonstrate raising on a machine whose
					*  mouse buttons are not available. */
					raise_window(0);
				}
			}
			else if(ev.type == SYS_INPUT_MOUSE)
			{
				gui_last_dx = ev.dx;
				gui_last_dy = ev.dy;

				/* A press is "the button is down now and it
				*  changed", which is what distinguishes it from
				*  a movement with the button held. */
				if((ev.changed & 0x01) != 0)
				{
					if((ev.buttons & 0x01) != 0)
						gui_press(ev.x, ev.y);
					else
						gui_drag = -1;
				}

				gui_buttons = ev.buttons;
				gui_motion(ev.x, ev.y);

				/* The Pointer window is deliberately NOT damaged
				*  here, although it is stale the moment an event
				*  arrives. It refreshes on the periodic tick with
				*  the others instead, and the reason is worth the
				*  tenth of a second of lag it costs:
				*
				*  damaging it here would put a window's worth of
				*  content into every frame that the pointer moved
				*  through, and a pointer moving is the case the
				*  whole damage list exists to make cheap. Left
				*  out, a frame in which nothing happened but the
				*  pointer moving damages two pointer sized
				*  rectangles and nothing else -- two 24x34 at
				*  scale 2 is 6528 bytes, or 3264 when a slow move
				*  overlaps them into one, against the 8294400 a
				*  full 1920x1080 screen costs. At scale 1 the
				*  pointer is 12x17 and those numbers are 1632 and
				*  816 against 3145728. Three orders of magnitude
				*  either way. That is what the Metrics window
				*  reports as "cheapest", and it would be a fiction
				*  about this program if this line were here. */
			}
		}

		now = sys_uptime();

		if(now - next_tick >= 0)
		{
			gui_uptime = now;
			gui_tick();
			next_tick = now + GUI_TICK_MS;
		}

		/* Flush when the queue has run dry (rc == 0, the input call
		*  timed out) or when the frame budget is up. Holding damage
		*  back through a burst of motion events is what turns a drag
		*  into frames instead of into one blit per event. */
		if(gfx_damage_count() != 0 &&
		   (rc != 1 || now - last_flush >= GUI_FRAME_MS))
		{
			gui_frame();
			last_flush = now;
		}
	}

	/* The screen goes back and the kernel repaints its console over
	*  whatever this program left there. Everything below this line prints
	*  to a console that is visible again. */
	gfx_close();

	printf("gui: %lu frames, %lu damage rectangles, %lu KiB copied to the\n",
	       gui_frames, gui_flush_rects, gui_flush_bytes / 1024);
	printf("     screen in %d ms. One full screen would have been %lu KiB\n",
	       gui_flush_ms,
	       ((unsigned long)c->w * (unsigned long)c->h *
	        (unsigned long)c->bytes) / 1024);
	printf("     per frame; the cheapest frame here was %lu bytes.\n",
	       gui_min_bytes);

	return GUI_OK;
}
