/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: VGA mode switching. Text mode 3 (80x25) <-> mode 13h (320x200x256),
*        plus access to the DAC palette.
*
*  There is no BIOS here. "int 0x10" is a real mode service and the kernel
*  never leaves protected mode, so the mode is set the way the BIOS itself
*  sets it: by writing the five VGA register files by hand.
*
*      Miscellaneous Output   0x3C2 (write only, 0x3CC reads it back)
*      Sequencer              0x3C4 index / 0x3C5 data      ->  5 registers
*      CRT Controller         0x3D4 index / 0x3D5 data      -> 25 registers
*      Graphics Controller    0x3CE index / 0x3CF data      ->  9 registers
*      Attribute Controller   0x3C0 index AND data          -> 21 registers
*
*  Three details decide whether this works at all, and all three are silent
*  when they are wrong -- no fault, no error, just a wrong or black picture.
*  They are described at the places that handle them:
*
*    1. CRTC registers 0..7 are write protected, see vga_write_regs().
*    2. The Attribute Controller shares one port for index and data and is
*       driven by a flip-flop, see vga_write_regs() again.
*    3. The text mode font lives in plane 2 of the very memory mode 13h uses
*       as its framebuffer, see vga_font_save() / vga_font_restore().
*
*  Notes: No warranty expressed or implied. Use at own risk. */

#include <system.h>
#include <vmm.h>
#include <vga.h>

/* --- Ports ---------------------------------------------------------------
*
*  The CRTC and the input status register live at 0x3D4/0x3DA in colour mode
*  and at 0x3B4/0x3BA in monochrome mode; which pair is active is decided by
*  bit 0 of the Miscellaneous Output register. Both tables below set that bit
*  (0x63 and 0x67 are odd numbers), and the Misc register is the first thing
*  written, so from that point on the colour addresses are the correct ones. */
#define VGA_MISC_WRITE      0x3C2
#define VGA_SEQ_INDEX       0x3C4
#define VGA_SEQ_DATA        0x3C5
#define VGA_DAC_MASK        0x3C6
#define VGA_DAC_WRITE_INDEX 0x3C8
#define VGA_DAC_DATA        0x3C9
#define VGA_GC_INDEX        0x3CE
#define VGA_GC_DATA         0x3CF
#define VGA_CRTC_INDEX      0x3D4
#define VGA_CRTC_DATA       0x3D5

/* Index and data both, alternating, driven by an internal flip-flop. */
#define VGA_AC_INDEX        0x3C0
#define VGA_AC_DATA         0x3C0

/* Reading this resets the Attribute Controller flip-flop to "index next". */
#define VGA_INPUT_STATUS_1  0x3DA

/* Number of registers per file, i.e. the layout of the tables below. */
#define VGA_NUM_SEQ_REGS    5
#define VGA_NUM_CRTC_REGS  25
#define VGA_NUM_GC_REGS     9
#define VGA_NUM_AC_REGS    21

/* One Misc byte in front, then the four files in the order above. */
#define VGA_NUM_REGS  (1 + VGA_NUM_SEQ_REGS + VGA_NUM_CRTC_REGS + \
                       VGA_NUM_GC_REGS + VGA_NUM_AC_REGS)

/* Offsets of the individual files inside a table. */
#define VGA_OFF_MISC   0
#define VGA_OFF_SEQ    (VGA_OFF_MISC + 1)
#define VGA_OFF_CRTC   (VGA_OFF_SEQ  + VGA_NUM_SEQ_REGS)
#define VGA_OFF_GC     (VGA_OFF_CRTC + VGA_NUM_CRTC_REGS)
#define VGA_OFF_AC     (VGA_OFF_GC   + VGA_NUM_GC_REGS)

/* The text mode font: 256 glyphs, and the hardware reserves 32 bytes per
*  glyph in plane 2 regardless of how many scan lines the glyph actually uses
*  (16 for the 8x16 font of mode 3). Saving the padding as well costs 4 KiB of
*  .bss and saves having to know the font height. */
#define VGA_FONT_GLYPHS      256
#define VGA_FONT_GLYPH_SIZE   32
#define VGA_FONT_BYTES  (VGA_FONT_GLYPHS * VGA_FONT_GLYPH_SIZE)

/* --- The register tables -------------------------------------------------
*
*  Both tables are the standard IBM VGA values for their mode. They are the
*  dumps published by Chris Giese in his public domain "modes.c" (the register
*  level VGA example that the OSDev wiki article "VGA Hardware" reproduces),
*  which in turn are what a real BIOS leaves behind after INT 0x10 / AH=0 for
*  mode 0x03 and mode 0x13. They were not derived here; deriving CRTC timing
*  from a dot clock is a different exercise entirely, and any deviation from
*  the BIOS values would only make the picture less likely to appear.
*
*  Layout of each row: Misc, Sequencer, CRTC, Graphics Controller, Attribute
*  Controller -- exactly the order vga_write_regs() writes them in. */

/* Mode 13h: 320x200, 256 colours, one linear byte per pixel at 0xA0000.
*
*  The values that make this mode what it is:
*    SEQ  0x04 = 0x0E   chain-4 on -- the four planes appear as one linear
*                       64 KiB block, which is why a pixel is a plain
*                       framebuffer[y * 320 + x]
*    CRTC 0x09 = 0x41   maximum scan line 1, i.e. every line is doubled;
*                       that is how 200 lines fill a 400 line raster
*    CRTC 0x13 = 0x28   offset 40, so one row is 40 * 8 = 320 bytes
*    CRTC 0x14 = 0x40 / CRTC 0x17 = 0xA3   doubleword mode off, byte mode on
*    GC   0x05 = 0x40   256 colour shift mode
*    GC   0x06 = 0x05   graphics mode, memory window A0000..AFFFF
*    AC   0x10 = 0x41   graphics mode, 8 bit colour (one byte = one index) */
static const uint8_t vga_regs_13h[VGA_NUM_REGS] =
{
	/* Miscellaneous Output */
	0x63,
	/* Sequencer 0x00..0x04 */
	0x03, 0x01, 0x0F, 0x00, 0x0E,
	/* CRT Controller 0x00..0x18 */
	0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
	0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
	0xFF,
	/* Graphics Controller 0x00..0x08 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
	0xFF,
	/* Attribute Controller 0x00..0x14 */
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	0x41, 0x00, 0x0F, 0x00, 0x00
};

/* Mode 3: 80x25 characters, 8x16 font, 16 colours out of 64.
*
*  The values that make this mode what it is:
*    SEQ  0x04 = 0x02   odd/even addressing, chain-4 off -- even addresses
*                       reach plane 0 (characters), odd ones plane 1
*                       (attributes), and plane 2 is free for the font
*    CRTC 0x09 = 0x4F   maximum scan line 15, i.e. a 16 line character cell
*    CRTC 0x0A = 0x0D / CRTC 0x0B = 0x0E   cursor start and end scan line
*    CRTC 0x13 = 0x28   offset 40, one row is 40 * 2 = 80 character cells
*    GC   0x05 = 0x10   odd/even mode
*    GC   0x06 = 0x0E   alphanumeric mode, memory window B8000..BFFFF
*    AC   0x00..0x0F    the EGA compatible palette: attributes 0..7 map to
*                       DAC entries 0x00..0x07 and 8..15 to 0x38..0x3F, which
*                       is precisely the mapping vga_load_text_palette()
*                       fills the DAC for
*    AC   0x10 = 0x0C   text mode (bit 0 clear), line graphics enable and
*                       blink enable -- the latter is why attribute bit 7
*                       blinks in text mode instead of selecting a bright
*                       background */
static const uint8_t vga_regs_text[VGA_NUM_REGS] =
{
	/* Miscellaneous Output */
	0x67,
	/* Sequencer 0x00..0x04 */
	0x03, 0x00, 0x03, 0x00, 0x02,
	/* CRT Controller 0x00..0x18 */
	0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
	0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
	0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
	0xFF,
	/* Graphics Controller 0x00..0x08 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00,
	0xFF,
	/* Attribute Controller 0x00..0x14 */
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
	0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	0x0C, 0x00, 0x0F, 0x08, 0x00
};

/* --- State ---------------------------------------------------------------
*
*  The kernel is handed over by the boot loader in text mode 3, so that is
*  the state this file starts in. Nothing here probes the hardware for the
*  current mode: there is no reliable way to do so, and every mode this
*  kernel ever enters comes through vga_set_mode(). */
static int vga_current_mode = VGA_MODE_TEXT;

/* The glyph bitmaps, rescued from plane 2 before the first switch away from
*  text mode. 8 KiB of .bss, allocated whether it is ever used or not -- the
*  alternative would be a heap allocation at exactly the moment when failing
*  is least acceptable, because by then the font is about to be overwritten. */
static uint8_t vga_font_backup[VGA_FONT_BYTES];
static int vga_font_valid = 0;

/* --- Serialising against the rest of the system --------------------------
*
*  The screen has several writers. The console task prints through putch(),
*  the status bar task writes row 0 directly, and any ring 3 task can reach
*  the same memory through SYS_WRITE -- see the lock comment in scrn.c. A
*  mode switch is not one write but a few hundred port accesses plus an 8 KiB
*  copy out of video memory, and a task switch landing in the middle of that
*  would leave the adapter half programmed while somebody else writes to it.
*
*  Masking interrupts is the lock on a single processor, and it is the same
*  lock scrn.c already uses. EFLAGS is saved rather than ending with sti, so
*  a caller that already holds the console lock does not get interrupts back
*  halfway through its own critical section.
*
*  What this does NOT solve, deliberately: once the mode is 13h, a task that
*  still calls putch() writes characters into what is now the framebuffer,
*  and they show up as coloured noise. That cannot be fixed from in here --
*  the adapter has one set of memory and no way to hide it. The callers are
*  the ones that have to agree, which is what vga_mode() is for: the console
*  can consult it and drop output while graphics mode is on, and a program
*  that wants the screen should quiesce the status bar before switching.
*  Until somebody does that, the rule is simply "do not print while drawing". */
static unsigned int vga_lock(void)
{
	unsigned int flags;
	__asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
	return flags;
}

static void vga_unlock(unsigned int flags)
{
	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

/* --- Indexed register access ---------------------------------------------
*
*  Every one of these files works the same way: write the register number to
*  the index port, then read or write the data port. The Attribute Controller
*  is the exception and is handled inline in vga_write_regs(). */
static void vga_seq_write(uint8_t index, uint8_t value)
{
	outportb(VGA_SEQ_INDEX, index);
	outportb(VGA_SEQ_DATA, value);
}

static uint8_t vga_seq_read(uint8_t index)
{
	outportb(VGA_SEQ_INDEX, index);
	return inportb(VGA_SEQ_DATA);
}

static void vga_gc_write(uint8_t index, uint8_t value)
{
	outportb(VGA_GC_INDEX, index);
	outportb(VGA_GC_DATA, value);
}

static uint8_t vga_gc_read(uint8_t index)
{
	outportb(VGA_GC_INDEX, index);
	return inportb(VGA_GC_DATA);
}

static void vga_crtc_write(uint8_t index, uint8_t value)
{
	outportb(VGA_CRTC_INDEX, index);
	outportb(VGA_CRTC_DATA, value);
}

static uint8_t vga_crtc_read(uint8_t index)
{
	outportb(VGA_CRTC_INDEX, index);
	return inportb(VGA_CRTC_DATA);
}

/* --- Writing a whole register table --------------------------------------
*
*  Order matters: Misc first, because it selects the dot clock and decides
*  whether the CRTC answers at 0x3D4 or 0x3B4, and the Attribute Controller
*  last, because its final write is what switches the picture back on. */
static void vga_write_regs(const uint8_t *regs)
{
	int i;
	uint8_t value;

	/* Miscellaneous Output. */
	outportb(VGA_MISC_WRITE, regs[VGA_OFF_MISC]);

	/* Sequencer. */
	for(i = 0; i < VGA_NUM_SEQ_REGS; i++)
		vga_seq_write((uint8_t)i, regs[VGA_OFF_SEQ + i]);

	/* CRTC registers 0..7 are WRITE PROTECTED by bit 7 of index 0x11
	*  ("vertical retrace end"). While that bit is set the adapter accepts
	*  the writes and throws them away: no fault, no status bit, nothing --
	*  the horizontal and vertical timing simply keeps the values of the mode
	*  we are trying to leave, and the result is a rolling picture or no
	*  picture at all. So it is cleared before the table goes in.
	*
	*  Bit 7 of index 0x03 goes the other way. On the original VGA it is
	*  reserved, on clones it is the "enable vertical retrace access" bit that
	*  makes registers 0x10 and 0x11 reachable in the first place, and it has
	*  to stay set for the rest of this loop to have any effect. */
	vga_crtc_write(0x03, (uint8_t)(vga_crtc_read(0x03) | 0x80));
	vga_crtc_write(0x11, (uint8_t)(vga_crtc_read(0x11) & 0x7F));

	for(i = 0; i < VGA_NUM_CRTC_REGS; i++)
	{
		value = regs[VGA_OFF_CRTC + i];

		/* Both tables already carry the right bits here (0x82 at index
		*  0x03, bit 7 clear at index 0x11), so this only guards against a
		*  table being edited later. It costs nothing and it removes the one
		*  way this function could re-lock the registers it just unlocked
		*  while it is still writing through them. */
		if(i == 0x03) value = (uint8_t)(value | 0x80);
		if(i == 0x11) value = (uint8_t)(value & 0x7F);

		vga_crtc_write((uint8_t)i, value);
	}

	/* Graphics Controller. */
	for(i = 0; i < VGA_NUM_GC_REGS; i++)
		vga_gc_write((uint8_t)i, regs[VGA_OFF_GC + i]);

	/* Attribute Controller. This one does not have an index port and a data
	*  port -- it has ONE port, 0x3C0, and an internal flip-flop that decides
	*  whether the next write to it is taken as an index or as data. The
	*  flip-flop is not readable and there is no way to set it directly; the
	*  only way to get it into a known state is to READ the input status
	*  register at 0x3DA, which resets it to "index next" as a side effect.
	*
	*  Hence the read at the top of every iteration. Doing it once before the
	*  loop would be enough on a machine where nothing else touches 0x3DA,
	*  but an interrupt handler or a badly timed read elsewhere would put the
	*  flip-flop out of phase and from then on every index would be written as
	*  data and every value as an index -- which programs the palette with
	*  garbage and is very hard to see. Resetting per register is two extra
	*  I/O cycles each and makes the sequence self-correcting.
	*
	*  Note also that writing an index with bit 5 clear disables video
	*  output; that is why the screen is dark for the duration of the loop
	*  and why the final write below matters so much. */
	for(i = 0; i < VGA_NUM_AC_REGS; i++)
	{
		(void)inportb(VGA_INPUT_STATUS_1);
		outportb(VGA_AC_INDEX, (uint8_t)i);
		outportb(VGA_AC_DATA, regs[VGA_OFF_AC + i]);
	}

	/* Index 0x20: bit 5 is the "palette address source" bit, and setting it
	*  hands the internal palette back to the display and re-enables video
	*  output. Without this last write everything above is correctly
	*  programmed and the screen stays black. */
	(void)inportb(VGA_INPUT_STATUS_1);
	outportb(VGA_AC_INDEX, 0x20);
}

/* --- Reaching plane 2 ----------------------------------------------------
*
*  In text mode the video memory is split by function: plane 0 holds the
*  characters, plane 1 the attributes, plane 2 the FONT, plane 3 is unused.
*  The CPU normally sees planes 0 and 1 interleaved at 0xB8000 through
*  odd/even addressing and cannot reach plane 2 at all.
*
*  To get at the glyphs the adapter has to be talked into a flat, single
*  plane view for a moment: odd/even off on both the Sequencer and the
*  Graphics Controller side, plane 2 selected for reading and for writing,
*  and the memory window moved to the full 64 KiB at 0xA0000 so that the
*  address the copy uses is unambiguous.
*
*  The five registers involved are saved and put back afterwards rather than
*  reset to a fixed value, so this works in both directions: called before
*  the mode 13h table is written it leaves text mode intact, called after the
*  mode 3 table is written it leaves mode 3 intact. */
struct vga_plane_state
{
	uint8_t seq_map_mask;   /* Sequencer 0x02  -- which planes writes go to */
	uint8_t seq_mem_mode;   /* Sequencer 0x04  -- chain-4 and odd/even       */
	uint8_t gc_read_map;    /* GC 0x04         -- which plane reads come from */
	uint8_t gc_mode;        /* GC 0x05         -- odd/even, shift, write mode */
	uint8_t gc_misc;        /* GC 0x06         -- the memory window          */
};

static void vga_plane2_enter(struct vga_plane_state *saved)
{
	saved->seq_map_mask = vga_seq_read(0x02);
	saved->seq_mem_mode = vga_seq_read(0x04);
	saved->gc_read_map  = vga_gc_read(0x04);
	saved->gc_mode      = vga_gc_read(0x05);
	saved->gc_misc      = vga_gc_read(0x06);

	/* Map mask: writes reach plane 2 and nothing else. */
	vga_seq_write(0x02, 0x04);
	/* Memory mode: extended memory (bit 1), odd/even addressing off
	*  (bit 2 = "sequential"), chain-4 off (bit 3). */
	vga_seq_write(0x04, 0x07);
	/* Read map select: reads come from plane 2. */
	vga_gc_write(0x04, 0x02);
	/* Mode: write mode 0, read mode 0, odd/even off, no shift interleave. */
	vga_gc_write(0x05, 0x00);
	/* Misc: memory map select 01 = the 64 KiB window at 0xA0000..0xAFFFF,
	*  chain odd/even off. This is what makes VGA_FB_PHYS the right address
	*  for the copy no matter which mode we came from -- in text mode the
	*  window would otherwise sit at 0xB8000 and the copy would read the
	*  characters on screen instead of the font. */
	vga_gc_write(0x06, 0x04);
}

static void vga_plane2_leave(const struct vga_plane_state *saved)
{
	vga_seq_write(0x02, saved->seq_map_mask);
	vga_seq_write(0x04, saved->seq_mem_mode);
	vga_gc_write(0x04, saved->gc_read_map);
	vga_gc_write(0x05, saved->gc_mode);
	vga_gc_write(0x06, saved->gc_misc);
}

/* Copies the 256 glyphs out of plane 2 into vga_font_backup. Must be called
*  while text mode is still programmed, i.e. before the mode 13h table goes
*  in -- afterwards chain-4 is on and plane 2 holds pixels, not glyphs. */
static void vga_font_save(void)
{
	struct vga_plane_state saved;

	vga_plane2_enter(&saved);
	memcpy(vga_font_backup, P2V(VGA_FB_PHYS), (size_t)VGA_FONT_BYTES);
	vga_plane2_leave(&saved);
}

/* And back again. Called after the mode 3 table has been written, so that
*  the registers vga_plane2_leave() restores are mode 3's own. */
static void vga_font_restore(void)
{
	struct vga_plane_state saved;

	vga_plane2_enter(&saved);
	memcpy(P2V(VGA_FB_PHYS), vga_font_backup, (size_t)VGA_FONT_BYTES);
	vga_plane2_leave(&saved);
}

/* --- The palette ---------------------------------------------------------
*
*  Setting a mode without setting the palette produces the right pixels in
*  the wrong colours, and it is a confusing failure because everything looks
*  like it is working. Mode 3 and mode 13h want completely different DAC
*  contents, so each direction of the switch reloads all 256 entries. */

/* The EGA colour value in DAC entry n, for the 64 EGA colours.
*
*  An EGA colour is six bits, r g b R G B, where R G B (bits 2..0) are the
*  primary bits and r g b (bits 5..3) the secondary ones. A channel's level
*  is therefore primary * 2 + secondary, i.e. 0..3, and the VGA DAC takes
*  six bits per channel, so the four levels are 0, 21, 42 and 63.
*
*  Entry 7 comes out as (42,42,42) light grey, entry 0x38 as (21,21,21) dark
*  grey and entry 0x3F as white -- which is exactly why the mode 3 attribute
*  table maps attributes 8..15 to DAC 0x38..0x3F. */
static void vga_ega_colour(int n, int *r, int *g, int *b)
{
	*r = (((n >> 2) & 1) * 2 + ((n >> 5) & 1)) * 21;
	*g = (((n >> 1) & 1) * 2 + ((n >> 4) & 1)) * 21;
	*b = (((n >> 0) & 1) * 2 + ((n >> 3) & 1)) * 21;
}

/* Attribute 0..15 of a text mode screen reaches these DAC entries, because
*  that is what the Attribute Controller palette in vga_regs_text says. The
*  same sixteen colours are what mode 13h puts in DAC 0..15. */
static const uint8_t vga_text_dac_map[16] =
{
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
	0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
};

/* The sixteen grey levels the BIOS puts in entries 16..31 of the mode 13h
*  palette. Not a linear ramp -- it is weighted towards the bright end. */
static const uint8_t vga_grey_ramp[16] =
{
	0x00, 0x05, 0x08, 0x0B, 0x0E, 0x11, 0x14, 0x18,
	0x1C, 0x20, 0x24, 0x28, 0x2D, 0x32, 0x38, 0x3F
};

/* The 24 fully saturated, full brightness hues that the 216 colour block of
*  the mode 13h palette is built from. The wheel starts at blue and runs
*  blue -> magenta -> red -> yellow -> green -> cyan -> blue, four steps per
*  sector, using the levels 0, 16, 31, 47 and 63. */
static const uint8_t vga_hue_wheel[24][3] =
{
	{  0,  0, 63 }, { 16,  0, 63 }, { 31,  0, 63 }, { 47,  0, 63 },
	{ 63,  0, 63 }, { 63,  0, 47 }, { 63,  0, 31 }, { 63,  0, 16 },
	{ 63,  0,  0 }, { 63, 16,  0 }, { 63, 31,  0 }, { 63, 47,  0 },
	{ 63, 63,  0 }, { 47, 63,  0 }, { 31, 63,  0 }, { 16, 63,  0 },
	{  0, 63,  0 }, {  0, 63, 16 }, {  0, 63, 31 }, {  0, 63, 47 },
	{  0, 63, 63 }, {  0, 47, 63 }, {  0, 31, 63 }, {  0, 16, 63 }
};

/* The three brightness levels of the 216 colour block, and for each of them
*  the floor that the three saturation levels compress the hue towards. Full
*  saturation keeps the floor at 0, the two lower ones lift it to roughly a
*  half and roughly three quarters of the brightness. These are the values a
*  real BIOS programs. */
static const uint8_t vga_block_value[3] = { 63, 28, 16 };
static const uint8_t vga_block_floor[3][3] =
{
	{ 0, 31, 45 },   /* brightness 63: saturation full, medium, low */
	{ 0, 14, 20 },   /* brightness 28 */
	{ 0,  8, 11 }    /* brightness 16 */
};

/* Compresses one hue component into [floor, value]. */
static int vga_mix(int component, int floor, int value)
{
	return floor + (component * (value - floor)) / 63;
}

/* The palette mode 3 expects: the 64 EGA colours in DAC entries 0..63, the
*  rest black. Text mode never addresses an entry above 63 -- the attribute
*  table only ever selects one of the sixteen listed in vga_text_dac_map --
*  but leaving mode 13h's colours in the upper entries would be untidy and
*  would confuse anyone reading the DAC back. */
static void vga_load_text_palette(void)
{
	int i, r, g, b;

	outportb(VGA_DAC_MASK, 0xFF);

	for(i = 0; i < 64; i++)
	{
		vga_ega_colour(i, &r, &g, &b);
		vga_palette((uint8_t)i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
	}

	for(i = 64; i < 256; i++)
		vga_palette((uint8_t)i, 0, 0, 0);
}

/* The standard 256 colour palette of mode 13h, in the layout the BIOS uses:
*
*      0..15    the sixteen EGA colours, so that a program using colour
*               numbers 0..15 gets what it would get in text mode
*     16..31    sixteen greys
*     32..247   216 colours: nine blocks of 24 hues, the blocks running
*               through three brightness levels and, within each, three
*               saturation levels
*    248..255   black, unused padding
*
*  This is computed rather than stored as a 768 byte dump. The layout is what
*  matters -- artwork and palette animation written for mode 13h assume these
*  ranges -- and the arithmetic below reproduces it. */
static void vga_load_graphics_palette(void)
{
	int i, v, s, h, idx;
	int value, floor;
	int r, g, b;

	outportb(VGA_DAC_MASK, 0xFF);

	/* 0..15: the sixteen standard colours. */
	for(i = 0; i < 16; i++)
	{
		vga_ega_colour((int)vga_text_dac_map[i], &r, &g, &b);
		vga_palette((uint8_t)i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
	}

	/* 16..31: the grey ramp. */
	for(i = 0; i < 16; i++)
		vga_palette((uint8_t)(16 + i), vga_grey_ramp[i],
		            vga_grey_ramp[i], vga_grey_ramp[i]);

	/* 32..247: three brightness levels, three saturations each, 24 hues. */
	idx = 32;
	for(v = 0; v < 3; v++)
	{
		for(s = 0; s < 3; s++)
		{
			value = (int)vga_block_value[v];
			floor = (int)vga_block_floor[v][s];

			for(h = 0; h < 24; h++)
			{
				r = vga_mix((int)vga_hue_wheel[h][0], floor, value);
				g = vga_mix((int)vga_hue_wheel[h][1], floor, value);
				b = vga_mix((int)vga_hue_wheel[h][2], floor, value);
				vga_palette((uint8_t)idx, (uint8_t)r, (uint8_t)g, (uint8_t)b);
				idx++;
			}
		}
	}

	/* 248..255: black. */
	for(i = 248; i < 256; i++)
		vga_palette((uint8_t)i, 0, 0, 0);
}

/* --- Public interface ---------------------------------------------------- */

void vga_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	/* Write the entry number once, then the three components in order. The
	*  DAC advances its own index afterwards, which is what makes loading a
	*  whole palette a single index write plus 768 data writes -- but writing
	*  the index per entry as we do here is what makes this function safe to
	*  call on its own from anywhere.
	*
	*  Six bits per channel, so 0..63 and not 0..255; anything above is
	*  masked off rather than silently wrapping into the next component. */
	outportb(VGA_DAC_WRITE_INDEX, index);
	outportb(VGA_DAC_DATA, (uint8_t)(r & 0x3F));
	outportb(VGA_DAC_DATA, (uint8_t)(g & 0x3F));
	outportb(VGA_DAC_DATA, (uint8_t)(b & 0x3F));
}

int vga_mode(void)
{
	return vga_current_mode;
}

uint8_t *vga_framebuffer(void)
{
	/* Physical 0xA0000 is memory mapped hardware, not RAM, and the kernel
	*  runs in the higher half -- so it is reached through the direct mapping
	*  like the text buffer in scrn.c. vmm_init() maps every frame including
	*  the region below 1 MiB, so there is nothing to map here.
	*
	*  Only valid while mode 13h is programmed: in text mode the same
	*  addresses are not the framebuffer, and handing out a pointer to them
	*  would mean a caller quietly scribbling over plane 2. */
	if(vga_current_mode != VGA_MODE_GRAPHICS) return 0;

	return (uint8_t *)P2V(VGA_FB_PHYS);
}

int vga_set_mode(int mode)
{
	unsigned int flags;

	if(mode != VGA_MODE_TEXT && mode != VGA_MODE_GRAPHICS) return -1;
	if(mode == vga_current_mode) return 0;

	flags = vga_lock();

	if(mode == VGA_MODE_GRAPHICS)
	{
		/* The font first, and while text mode is still programmed. Once the
		*  mode 13h table is in, chain-4 is on and the bytes that were the
		*  glyphs are pixels -- there is no second chance.
		*
		*  Only on the first switch away from text mode. The glyphs never
		*  change afterwards (nothing in this kernel loads a font), so every
		*  later save would copy back what is already in the buffer, and if
		*  anything ever went wrong in between it would copy the damage. */
		if(!vga_font_valid)
		{
			vga_font_save();
			vga_font_valid = 1;
		}

		vga_write_regs(vga_regs_13h);
		vga_load_graphics_palette();
	}
	else
	{
		vga_write_regs(vga_regs_text);
		vga_load_text_palette();

		/* And now the font goes back into plane 2, which mode 13h has been
		*  using as the third quarter of its framebuffer. Without this the
		*  console comes back to a screen of blanks: the characters and
		*  attributes in planes 0 and 1 are still there and still being
		*  scanned out, but every glyph they point at is whatever pixels the
		*  last drawing left behind.
		*
		*  After the mode 3 table, not before, so that the registers
		*  vga_plane2_leave() puts back are mode 3's own. */
		if(vga_font_valid) vga_font_restore();
	}

	vga_current_mode = mode;

	vga_unlock(flags);
	return 0;
}
