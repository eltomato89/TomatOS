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
*  movement would cost 3 MiB of blit per event. So the picture is rebuilt only
*  inside the rectangles that changed:
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
*  nothing against one 3 MiB blit.
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
#define GUI_BORDER        2     /* frame thickness, all four sides       */
#define GUI_TITLE_H      24     /* title bar height, inside the border   */
#define GUI_TEXT_SCALE    2     /* 8x8 font doubled: 16 pixel capitals   */
#define GUI_LINE_H       20     /* baseline to baseline in the body      */
#define GUI_PAD           8

/* A window may be dragged off the edges, but never so far that its title bar
*  becomes unreachable -- a window with no grabbable strip left is a window
*  that can never be brought back. This many pixels of title bar always stay
*  on screen. */
#define GUI_KEEP_ON      40


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
*  having to choose colours that avoid it. */
#define GUI_CURSOR_W     12
#define GUI_CURSOR_H     17
#define GUI_CURSOR_SCALE  2     /* drawn at 24x34; 12x17 is a speck at 1024x768 */

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
	int         x;
	int         y;
	int         w;
	int         h;
	const char *title;
	int         body;
	int         live;   /* lines of content that change with time, 0 = none */
} gui_window;

/* Deliberately overlapping. A window system where nothing overlaps never has
*  to answer the question it exists to answer, and a screenshot of one proves
*  nothing about raising or about exposure. */
static gui_window gui_win[GUI_WINDOWS] = {
	{  60,  90, 380, 150, "Clock",     GUI_BODY_CLOCK,   4 },
	{ 300, 200, 360, 160, "Pointer",   GUI_BODY_MOUSE,   5 },
	{ 520, 320, 460, 220, "Metrics",   GUI_BODY_METRICS, 6 },
	{ 110, 420, 470, 260, "Read me",   GUI_BODY_HELP,    0 }
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
	r->y = win->y + GUI_BORDER + GUI_TITLE_H;
	r->w = win->w - 2 * GUI_BORDER;
	r->h = win->h - 2 * GUI_BORDER - GUI_TITLE_H;

	if(r->w < 0) r->w = 0;
	if(r->h < 0) r->h = 0;
}

/* The part of a window that changes on its own, as opposed to when it moves.
*
*  This is NOT the body. It is the lines of the body that carry a clock or a
*  counter, and the difference is worth a function: the Metrics body is
*  456x180 pixels and its five live lines are 456x116, so damaging the body
*  ten times a second costs 500 KiB of blit per second more than damaging the
*  lines does. An idle window system should be idle, and the way to make it
*  idle is to be specific about what actually changed. */
static void win_live_rect(const gui_window *win, gfx_rect *r)
{
	int used;

	win_body_rect(win, r);

	used = win->live * GUI_LINE_H + 2 * GUI_PAD;
	if(r->h > used)
		r->h = used;
}

/* The rectangle the pointer covers at a given position. The hotspot is the
*  arrow's tip, which is its top left pixel, so the rectangle starts there. */
static void cursor_rect(int x, int y, gfx_rect *r)
{
	r->x = x;
	r->y = y;
	r->w = GUI_CURSOR_W * GUI_CURSOR_SCALE;
	r->h = GUI_CURSOR_H * GUI_CURSOR_SCALE;
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
*  window. That is 56 span calls per damage rectangle, almost all of which
*  return after one comparison, against a fill of the same rectangle. */
static void paint_desktop(gfx_surface *c)
{
	int i;

	gfx_clear(c, col_desk);

	for(i = 0; i < c->w; i += 32)
		gfx_vline(c, i, 0, c->h, col_grid);
	for(i = 0; i < c->h; i += 32)
		gfx_hline(c, 0, i, c->w, col_grid);

	/* 16 pixels per character at scale 2, so 62 characters is the most that
	*  fits across 1024 pixels with a margin. gfx_text() would clip the rest
	*  rather than wrap it, which is correct and still looks like a mistake,
	*  so the string is written to fit. */
	gfx_text(c, 16, 12,
	         "TomatOS window system: drag a title, click to raise, Esc out",
	         GUI_TEXT_SCALE, col_title_text);
}

/* One line of body text, at line number "line" counting from 0. */
static void body_line(gfx_surface *c, const gfx_rect *body, int line,
                      const char *text, gfx_color colour)
{
	gfx_text(c, body->x + GUI_PAD, body->y + GUI_PAD + line * GUI_LINE_H,
	         text, GUI_TEXT_SCALE, colour);
}

static void paint_body_clock(gfx_surface *c, const gfx_rect *body)
{
	char line[64];
	int bar_x, bar_y, bar_w, filled;

	format_uptime(line, sizeof(line), gui_uptime);
	body_line(c, body, 0, "Up", col_dim);
	gfx_text(c, body->x + GUI_PAD + 3 * GFX_FONT_WIDTH * GUI_TEXT_SCALE,
	         body->y + GUI_PAD, line, GUI_TEXT_SCALE, col_text);

	/* A bar that fills across each second. The digits above prove the clock
	*  is running; the bar proves the program is redrawing between the
	*  digits changing, which a still picture of a running clock cannot. */
	bar_x = body->x + GUI_PAD;
	bar_y = body->y + GUI_PAD + GUI_LINE_H + 6;
	bar_w = body->w - 2 * GUI_PAD;

	if(bar_w > 0)
	{
		filled = (bar_w * ((gui_uptime % 1000) / 10)) / 100;
		gfx_fill_rect(c, bar_x, bar_y, bar_w, 14, col_shadow);
		gfx_fill_rect(c, bar_x, bar_y, filled, 14, col_bar);
		gfx_draw_rect(c, bar_x, bar_y, bar_w, 14, col_frame);
	}

	snprintf(line, sizeof(line), "%lu events, %lu frames",
	         gui_events, gui_frames);
	body_line(c, body, 3, line, col_dim);
}

static void paint_body_mouse(gfx_surface *c, const gfx_rect *body)
{
	char line[64];
	int i, bx;

	snprintf(line, sizeof(line), "at %4d,%-4d", gui_ptr_x, gui_ptr_y);
	body_line(c, body, 0, line, col_text);

	snprintf(line, sizeof(line), "step %4d,%-4d", gui_last_dx, gui_last_dy);
	body_line(c, body, 1, line, col_dim);

	/* Three squares, filled while the matching button is down. Squares and
	*  not text because a pressed button lasts a fraction of a second and a
	*  word that appears and disappears is harder to catch than a box that
	*  fills in. */
	bx = body->x + GUI_PAD;
	for(i = 0; i < 3; i++)
	{
		int on = (gui_buttons & (1u << i)) != 0;

		gfx_fill_rect(c, bx + i * 34, body->y + GUI_PAD + 2 * GUI_LINE_H + 4,
		              26, 22, on ? col_bar : col_shadow);
		gfx_draw_rect(c, bx + i * 34, body->y + GUI_PAD + 2 * GUI_LINE_H + 4,
		              26, 22, col_frame);
		gfx_char(c, bx + i * 34 + 6,
		         body->y + GUI_PAD + 2 * GUI_LINE_H + 7,
		         (i == 0) ? 'L' : ((i == 1) ? 'R' : 'M'),
		         GUI_TEXT_SCALE, on ? col_title_text : col_dim);
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
	              win->w - 2 * GUI_BORDER, GUI_TITLE_H,
	              active ? col_title_on : col_title_off);

	tx = win->x + GUI_BORDER + GUI_PAD;
	ty = win->y + GUI_BORDER + (GUI_TITLE_H - GFX_FONT_HEIGHT * GUI_TEXT_SCALE) / 2;
	gfx_text(c, tx, ty, win->title, GUI_TEXT_SCALE,
	         active ? col_title_text : col_dim);

	gfx_hline(c, win->x + GUI_BORDER, win->y + GUI_BORDER + GUI_TITLE_H,
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
*  drawn twice instead of ten separate blocks. */
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
			              gui_ptr_x + col * GUI_CURSOR_SCALE,
			              gui_ptr_y + row * GUI_CURSOR_SCALE,
			              run * GUI_CURSOR_SCALE, GUI_CURSOR_SCALE,
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
	if(y >= w->y + GUI_BORDER && y < w->y + GUI_BORDER + GUI_TITLE_H &&
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
		nx = clampi(nx, GUI_KEEP_ON - w->w, c->w - GUI_KEEP_ON);
		ny = clampi(ny, 0, c->h - GUI_KEEP_ON);

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
				*  damaging it here would put 165 KiB of window
				*  content into every frame that the pointer moved
				*  through, and a pointer moving is the case the
				*  whole damage list exists to make cheap. Left
				*  out, a frame in which nothing happened but the
				*  pointer moving damages two 24x34 rectangles and
				*  nothing else: 6528 bytes when they are apart,
				*  and 3264 when a slow move overlaps them into
				*  one, against the 3145728 a full screen costs.
				*  That is what the Metrics window reports as
				*  "cheapest", and it would be a fiction about
				*  this program if this line were here. */
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
