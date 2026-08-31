/* TomatOS - drawing from ring 3
*  Desc: The primitives gfxlib.h promises, plus the back buffer they draw
*        into and the blit that puts it on the screen.
*
*  gfxlib.h says what this is for and how it differs from src/video/fbdraw.c. This
*  file says where the memory came from and what it costs, because those are
*  the two questions a back buffer raises and neither has an obvious answer in
*  a program with no malloc and a 4 KiB stack.
*
*  ------------------------------------------------------------------------
*  Where 8.3 MB of back buffer comes from
*  ------------------------------------------------------------------------
*  There are exactly three places a TomatOS program can put a byte:
*
*    - the stack, which is one 4 KiB page (USER_STACK_SLOT_SIZE in
*      src/kernel/tasks.c is 8 KiB, and the lower half of it is an unmapped guard
*      page). A frame of 1920x1080x32 is two thousand times that, so this is
*      not a candidate for even a fraction of it;
*    - the heap, which does not exist. There is no malloc in user space and
*      no system call that would grow the address space, deliberately: the
*      kernel's loader maps exactly the segments the ELF declares and nothing
*      afterwards ever adds a page to a running task;
*    - file scope, which is .bss.
*
*  So it is .bss, and .bss turns out to be the right answer rather than the
*  only one left. user/user.ld puts .bss in the single PT_LOAD segment, where
*  it is the whole reason p_memsz comes out larger than p_filesz, and
*  src/kernel/exec.c's loader allocates a frame per page of that segment and zeroes
*  everything past p_filesz. The 8.3 MB therefore costs nothing on the FAT
*  volume -- the ELF on disk stays around 20 KiB -- and arrives already
*  cleared, which is one memset of 8.3 MB that never has to run.
*
*  WHAT IT DOES COST is a physical frame per page, eagerly, at every start:
*  2025 frames for the buffer alone, whatever mode the machine came up in.
*  That is 8.3 MB of a guest the Makefile now gives 64 MB, and it is the
*  reason the Makefile gives it 64 rather than the 32 it ran on before.
*
*  IT NO LONGER FITS IN ONE PAGE TABLE, and that is the one claim raising the
*  ceiling actually invalidated. user.ld links at 0x00400000, which is 4 MiB
*  aligned and therefore starts a page directory entry of its own; one page
*  table covers 4 MiB, and 8.3 MB of buffer plus a few pages of code reaches
*  to roughly 0x00BF0000, which is two page tables and within a few hundred
*  kilobytes of needing a third. Nothing has to be done about that: src/kernel/exec.c
*  maps a segment page by page through vmm_map_in(), which allocates a page
*  table whenever the directory entry is empty and reports "no memory for a
*  page table" if it cannot. The cost is one more 4 KiB frame, not a rewrite.
*
*  The size is fixed at the largest mode that can turn up rather than fitted
*  to the mode that did, because there is no way to fit it afterwards -- the
*  mode is not known until sys_mapfb() answers, and by then every byte the
*  program will ever have is already mapped. A mode larger than GFX_MAX_WIDTH
*  x GFX_MAX_HEIGHT is refused by gfx_open(); a smaller one leaves the tail of
*  the array unused, which is the ORDINARY case and not a failure: 640x480x32
*  uses 1.2 MB of the 8.3 and never touches the rest.
*
*  Which makes one rule load-bearing. THE UNUSED TAIL IS ONLY UNUSED IF EVERY
*  OFFSET IS COMPUTED FROM THE NEGOTIATED WIDTH AND PITCH, never from
*  GFX_MAX_WIDTH. gfx_open() gives the canvas a pitch of width * bytes for the
*  width the card reported, and row_at() is the only place in this file that
*  turns a row number into an address -- so there is exactly one line to be
*  right about. Getting it wrong is the bug that draws a correct picture on
*  the machine that was tested and a sheared one on the next.
*
*  ------------------------------------------------------------------------
*  What a frame costs, and why the damage list exists
*  ------------------------------------------------------------------------
*  A full screen is width * height * bytes: 3145728 bytes at 1024x768x32 and
*  8294400 at 1920x1080x32. Two things have to be said about moving that many:
*
*  1. NOT WITH memcpy(). user/lib/lib.c's memcpy() is a byte at a time loop, and
*     says so in as many words -- "nothing in a user program moves enough
*     memory for it to matter" was true of every program on this disk until
*     this one. Eight million byte loads and eight million byte stores per
*     frame is four times the memory traffic a dword copy needs and four
*     times the loop overhead. copy_row() below is that dword copy, and it is
*     the single reason this file does not simply call memcpy().
*
*  2. NOT ALL OF IT. Even at dword width, a full flush is megabytes across
*     the bus, and the pointer moving one pixel changes a couple of thousand
*     of them. gfx_damage() collects the rectangles that actually changed and
*     gfx_flush() copies only those. gui.c measures both and puts the numbers
*     on screen, so the claim is checkable rather than asserted -- and the
*     ratio between them grows with the mode.
*
*  ------------------------------------------------------------------------
*  Where the glyphs come from
*  ------------------------------------------------------------------------
*  user/include/lib.h has no font and there is no font file on the volume to load, so
*  the table below is a COPY of the first 128 glyphs of src/video/font8x8.c. A copy
*  and not a reference, for the same reason user/include/syscall.h is a copy of the
*  kernel's call numbers: this program links against user/lib.o and nothing
*  else, and reaching into src/ would make a ring 3 binary depend on a kernel
*  object it is not built with.
*
*  src/video/font8x8.c REMAINS THE AUTHORITY. Each glyph there carries a picture of
*  itself in the comment above it and the picture is what to fix if a
*  character comes out wrong; this table is eight bytes per row with the code
*  spelled out beside it, and a change there has to be brought here by hand.
*  The layout is the same -- one byte per glyph row, top row first, most
*  significant bit leftmost, so 0x80 lights only the left edge -- which is
*  what makes a caption drawn by this program and one drawn by the kernel
*  console look identical, and that identity is the point of copying these
*  particular glyphs rather than inventing new ones.
*/
#include "syscall.h"
#include "lib.h"
#include "gfxlib.h"


/* ------------------------------------------------------------------ */
/* The font                                                            */
/* ------------------------------------------------------------------ */

/* 128 glyphs, GFX_FONT_HEIGHT bytes each. Codes 0x00..0x1F are blank and
*  0x7F is a hollow box, so an attempt to draw an undrawable code is visible
*  rather than silent. Copied from src/video/font8x8.c -- see the header note. */
static const unsigned char gfx_font[128][GFX_FONT_HEIGHT] = {
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 00   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 01   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 02   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 03   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 04   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 05   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 06   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 07   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 08   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 09   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 0A   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 0B   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 0C   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 0D   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 0E   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 0F   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 10   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 11   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 12   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 13   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 14   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 15   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 16   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 17   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 18   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 19   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 1A   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 1B   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 1C   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 1D   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 1E   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 1F   */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 20   */
	{ 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00 },  /* 21 ! */
	{ 0x6C, 0x6C, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 22 " */
	{ 0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00 },  /* 23 # */
	{ 0x18, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x18, 0x00 },  /* 24 $ */
	{ 0x62, 0x66, 0x0C, 0x18, 0x30, 0x66, 0x46, 0x00 },  /* 25 % */
	{ 0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00 },  /* 26 & */
	{ 0x18, 0x18, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 27 ' */
	{ 0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00 },  /* 28 ( */
	{ 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00 },  /* 29 ) */
	{ 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 },  /* 2A * */
	{ 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00 },  /* 2B + */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30 },  /* 2C , */
	{ 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00 },  /* 2D - */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 },  /* 2E . */
	{ 0x02, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00 },  /* 2F / */
	{ 0x3C, 0x66, 0x6E, 0x7E, 0x76, 0x66, 0x3C, 0x00 },  /* 30 0 */
	{ 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 },  /* 31 1 */
	{ 0x3C, 0x66, 0x06, 0x1C, 0x30, 0x60, 0x7E, 0x00 },  /* 32 2 */
	{ 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 },  /* 33 3 */
	{ 0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00 },  /* 34 4 */
	{ 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 },  /* 35 5 */
	{ 0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 },  /* 36 6 */
	{ 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 },  /* 37 7 */
	{ 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 },  /* 38 8 */
	{ 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00 },  /* 39 9 */
	{ 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00 },  /* 3A : */
	{ 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30 },  /* 3B ; */
	{ 0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00 },  /* 3C < */
	{ 0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00 },  /* 3D = */
	{ 0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x00 },  /* 3E > */
	{ 0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00 },  /* 3F ? */
	{ 0x3C, 0x66, 0x6E, 0x6E, 0x60, 0x62, 0x3C, 0x00 },  /* 40 @ */
	{ 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 },  /* 41 A */
	{ 0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00 },  /* 42 B */
	{ 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00 },  /* 43 C */
	{ 0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00 },  /* 44 D */
	{ 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 },  /* 45 E */
	{ 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00 },  /* 46 F */
	{ 0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00 },  /* 47 G */
	{ 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 },  /* 48 H */
	{ 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 },  /* 49 I */
	{ 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00 },  /* 4A J */
	{ 0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00 },  /* 4B K */
	{ 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00 },  /* 4C L */
	{ 0xC6, 0xEE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00 },  /* 4D M */
	{ 0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00 },  /* 4E N */
	{ 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },  /* 4F O */
	{ 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00 },  /* 50 P */
	{ 0x3C, 0x66, 0x66, 0x66, 0x66, 0x6C, 0x36, 0x00 },  /* 51 Q */
	{ 0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00 },  /* 52 R */
	{ 0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00 },  /* 53 S */
	{ 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },  /* 54 T */
	{ 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },  /* 55 U */
	{ 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 },  /* 56 V */
	{ 0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00 },  /* 57 W */
	{ 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00 },  /* 58 X */
	{ 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00 },  /* 59 Y */
	{ 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00 },  /* 5A Z */
	{ 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00 },  /* 5B [ */
	{ 0x40, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00 },  /* 5C \ */
	{ 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00 },  /* 5D ] */
	{ 0x18, 0x3C, 0x66, 0x42, 0x00, 0x00, 0x00, 0x00 },  /* 5E ^ */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF },  /* 5F _ */
	{ 0x18, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00 },  /* 60 ` */
	{ 0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00 },  /* 61 a */
	{ 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00 },  /* 62 b */
	{ 0x00, 0x00, 0x3C, 0x66, 0x60, 0x66, 0x3C, 0x00 },  /* 63 c */
	{ 0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00 },  /* 64 d */
	{ 0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00 },  /* 65 e */
	{ 0x1C, 0x30, 0x30, 0x7C, 0x30, 0x30, 0x30, 0x00 },  /* 66 f */
	{ 0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x3C },  /* 67 g */
	{ 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00 },  /* 68 h */
	{ 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00 },  /* 69 i */
	{ 0x0C, 0x00, 0x1C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38 },  /* 6A j */
	{ 0x60, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x00 },  /* 6B k */
	{ 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 },  /* 6C l */
	{ 0x00, 0x00, 0xEC, 0xFE, 0xD6, 0xC6, 0xC6, 0x00 },  /* 6D m */
	{ 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00 },  /* 6E n */
	{ 0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00 },  /* 6F o */
	{ 0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60 },  /* 70 p */
	{ 0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x06 },  /* 71 q */
	{ 0x00, 0x00, 0x6C, 0x76, 0x60, 0x60, 0x60, 0x00 },  /* 72 r */
	{ 0x00, 0x00, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x00 },  /* 73 s */
	{ 0x30, 0x30, 0x7C, 0x30, 0x30, 0x36, 0x1C, 0x00 },  /* 74 t */
	{ 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00 },  /* 75 u */
	{ 0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 },  /* 76 v */
	{ 0x00, 0x00, 0xC6, 0xC6, 0xD6, 0xFE, 0x6C, 0x00 },  /* 77 w */
	{ 0x00, 0x00, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00 },  /* 78 x */
	{ 0x00, 0x00, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x3C },  /* 79 y */
	{ 0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x7E, 0x00 },  /* 7A z */
	{ 0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00 },  /* 7B { */
	{ 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18 },  /* 7C | */
	{ 0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0x00 },  /* 7D } */
	{ 0x00, 0x00, 0x32, 0x4C, 0x00, 0x00, 0x00, 0x00 },  /* 7E ~ */
	{ 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00 }   /* 7F   */
};

/* The table really is 128 glyphs of GFX_FONT_HEIGHT bytes. gfx_char() indexes
*  it with a character value and would run off the end of a short one, so this
*  is checked at compile time -- a negative array size is a hard error and
*  cannot be ignored the way a warning can. Same guard as src/video/font8x8.c's. */
typedef char gfx_font_size_check[
	(sizeof(gfx_font) == 128 * GFX_FONT_HEIGHT) ? 1 : -1];


/* ------------------------------------------------------------------ */
/* The back buffer                                                     */
/* ------------------------------------------------------------------ */

/* Declared as 32-bit words rather than bytes for one reason: alignment. copy_row()
*  and the 32 bpp span writer both move dwords, and while an unaligned dword
*  access is merely slower on x86 and not an error, a buffer that is aligned
*  by construction costs nothing and removes the question. An array of
*  unsigned long is 4-byte aligned by the ABI; an array of unsigned char is
*  aligned however the compiler felt.
*
*  An array of unsigned int is also exactly (W * H * BYTES) / 4 entries long
*  on every target, which an array of unsigned long would not be.
*
*  Nothing else in this file ever refers to it by this type -- gfx_open()
*  hands its address to gfx_surface_init() and everything afterwards goes
*  through the surface. */
static unsigned int gfx_back[
	(GFX_MAX_WIDTH * GFX_MAX_HEIGHT * GFX_MAX_BYTES) / 4];

/* The division by four above is only lossless while the product is a multiple
*  of four, and a future GFX_MAX_BYTES of 3 with an odd width would silently
*  make the array short by a pixel -- which is a write past the end of .bss on
*  the last row of the last frame and nowhere else. Checked here rather than
*  hoped for, the same way the font table is. */
typedef char gfx_back_size_check[
	(sizeof(gfx_back) >= GFX_BACK_BYTES) ? 1 : -1];


/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static sys_fbinfo  gfx_info;      /* what sys_mapfb() reported                */
static gfx_surface gfx_scr;       /* the framebuffer itself                   */
static gfx_surface gfx_buf;       /* gfx_back, described                      */
static int         gfx_have_fb;   /* non-zero between open and close          */

static gfx_rect    gfx_dmg[GFX_DAMAGE_MAX];
static int         gfx_dmg_n;


/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */

int gfx_surface_init(gfx_surface *s, void *mem,
                     int w, int h, unsigned long pitch,
                     unsigned long bpp,
                     unsigned char red_pos, unsigned char red_size,
                     unsigned char green_pos, unsigned char green_size,
                     unsigned char blue_pos, unsigned char blue_size)
{
	int bytes;

	if(s == 0)
		return 0;

	s->mem = 0;               /* refused until proven otherwise */

	bytes = (int)((bpp + 7) / 8);

	if(mem == 0 || w <= 0 || h <= 0 || bytes < 1 || bytes > GFX_MAX_BYTES)
		return 0;

	/* The overflow argument the clipping rests on bounds the surface as
	*  well as the coordinates: sf_w and sf_h are multiplied and added as
	*  freely as anything a caller passes. */
	if(w > GFX_COORD_MAX || h > GFX_COORD_MAX)
		return 0;

	/* A pitch shorter than a row would put the right hand end of every row
	*  into the next one, and the last row past the end of the block. The
	*  product cannot overflow: w is at most 16384 and bytes at most 4. */
	if(pitch < (unsigned long)w * (unsigned long)bytes)
		return 0;

	/* Channel positions have to name bits of a pixel this wide. A position
	*  of 24 in a 16 bpp mode would shift the channel out of existence,
	*  which is not dangerous but is silently wrong, and a size of 0 would
	*  make gfx_rgb() shift by 8 and produce nothing. */
	if(red_size == 0 || green_size == 0 || blue_size == 0 ||
	   red_size > 8 || green_size > 8 || blue_size > 8)
		return 0;
	if((unsigned int)red_pos + red_size > 32 ||
	   (unsigned int)green_pos + green_size > 32 ||
	   (unsigned int)blue_pos + blue_size > 32)
		return 0;

	s->mem        = (unsigned char *)mem;
	s->w          = w;
	s->h          = h;
	s->pitch      = pitch;
	s->bytes      = bytes;
	s->bpp        = bpp;
	s->red_pos    = red_pos;
	s->red_size   = red_size;
	s->green_pos  = green_pos;
	s->green_size = green_size;
	s->blue_pos   = blue_pos;
	s->blue_size  = blue_size;

	gfx_clip_none(s);
	return 1;
}

gfx_color gfx_rgb(const gfx_surface *s,
                  unsigned char r, unsigned char g, unsigned char b)
{
	if(s == 0 || s->mem == 0)
		return 0;

	/* Truncate each channel from the top to the width the mode carries,
	*  then move it to where the mode keeps it. Identical arithmetic to
	*  src/video/fbcon.c's rgb_to_pixel(), on purpose: a colour named by the same
	*  triple has to come out the same on both sides of the gate. */
	return (((gfx_color)r >> (8 - s->red_size))   << s->red_pos)
	     | (((gfx_color)g >> (8 - s->green_size)) << s->green_pos)
	     | (((gfx_color)b >> (8 - s->blue_size))  << s->blue_pos);
}

void gfx_clip_none(gfx_surface *s)
{
	if(s == 0)
		return;

	s->clip.x = 0;
	s->clip.y = 0;
	s->clip.w = s->w;
	s->clip.h = s->h;
}

void gfx_clip_set(gfx_surface *s, int x, int y, int w, int h)
{
	int x1, y1;

	if(s == 0)
		return;

	/* An out-of-range request becomes an empty window rather than a wide
	*  one. Refusing would leave the previous window in force, and a caller
	*  that then painted would paint outside the rectangle it asked for --
	*  the one failure mode a repaint loop must not have. */
	if(w <= 0 || h <= 0 ||
	   x > GFX_COORD_MAX || y > GFX_COORD_MAX ||
	   x < -GFX_COORD_MAX || y < -GFX_COORD_MAX ||
	   w > GFX_COORD_MAX || h > GFX_COORD_MAX)
	{
		s->clip.x = 0;
		s->clip.y = 0;
		s->clip.w = 0;
		s->clip.h = 0;
		return;
	}

	/* Everything here is inside +/-2 * GFX_COORD_MAX, so the sums are
	*  nowhere near overflowing. */
	x1 = x + w;
	y1 = y + h;

	if(x < 0)        x = 0;
	if(y < 0)        y = 0;
	if(x1 > s->w)    x1 = s->w;
	if(y1 > s->h)    y1 = s->h;

	if(x1 <= x || y1 <= y)
	{
		s->clip.x = 0;
		s->clip.y = 0;
		s->clip.w = 0;
		s->clip.h = 0;
		return;
	}

	s->clip.x = x;
	s->clip.y = y;
	s->clip.w = x1 - x;
	s->clip.h = y1 - y;
}


void gfx_clip_get(const gfx_surface *s, gfx_rect *out)
{
	if(out == 0)
		return;

	if(s == 0)
	{
		out->x = 0;
		out->y = 0;
		out->w = 0;
		out->h = 0;
		return;
	}

	*out = s->clip;
}

void gfx_clip_intersect(gfx_surface *s, int x, int y, int w, int h)
{
	int x0, y0, x1, y1;

	if(s == 0)
		return;

	/* An out-of-range request empties the window, exactly as
	*  gfx_clip_set() does, and for the same reason: leaving the previous
	*  window in force would let a caller paint outside the rectangle it
	*  asked for. */
	if(w <= 0 || h <= 0 ||
	   x > GFX_COORD_MAX || y > GFX_COORD_MAX ||
	   x < -GFX_COORD_MAX || y < -GFX_COORD_MAX ||
	   w > GFX_COORD_MAX || h > GFX_COORD_MAX)
	{
		s->clip.x = 0;
		s->clip.y = 0;
		s->clip.w = 0;
		s->clip.h = 0;
		return;
	}

	x0 = (x > s->clip.x) ? x : s->clip.x;
	y0 = (y > s->clip.y) ? y : s->clip.y;
	x1 = x + w;
	y1 = y + h;
	if(s->clip.x + s->clip.w < x1) x1 = s->clip.x + s->clip.w;
	if(s->clip.y + s->clip.h < y1) y1 = s->clip.y + s->clip.h;

	if(x1 <= x0 || y1 <= y0)
	{
		s->clip.x = 0;
		s->clip.y = 0;
		s->clip.w = 0;
		s->clip.h = 0;
		return;
	}

	s->clip.x = x0;
	s->clip.y = y0;
	s->clip.w = x1 - x0;
	s->clip.h = y1 - y0;
}


/* ------------------------------------------------------------------ */
/* Writing pixels                                                      */
/* ------------------------------------------------------------------ */

/* First byte of a row. The only place a row address is formed, so that "read
*  the pitch, never compute it" is a property of one function rather than a
*  rule a dozen call sites have to remember. */
static unsigned char *row_at(const gfx_surface *s, int y)
{
	return s->mem + (unsigned long)y * s->pitch;
}

/* One pixel, at a pointer the caller has already bounded. The four cases are
*  the four depths that can turn up, and 24 bpp is the awkward one: three
*  bytes is not a type the machine has and consecutive pixels are not aligned
*  to anything at all, so it has to be three byte stores, low byte first.
*  Writing it as a dword store of the low three bytes plus a fixup would
*  scribble over the first byte of the NEXT pixel; a masked read-modify-write
*  would read the framebuffer, which nothing here does. */
static void store_pixel(const gfx_surface *s, unsigned char *p, gfx_color pixel)
{
	switch(s->bytes)
	{
		case 4:
			*(unsigned int *)p = pixel;
			break;
		case 3:
			p[0] = (unsigned char)pixel;
			p[1] = (unsigned char)(pixel >> 8);
			p[2] = (unsigned char)(pixel >> 16);
			break;
		case 2:
			*(unsigned short *)p = (unsigned short)pixel;
			break;
		default:
			*p = (unsigned char)pixel;
			break;
	}
}

/* A run of pixels along one row, from a pointer and a length the caller has
*  already bounded. The workhorse: every fill, every horizontal line and every
*  row of a glyph ends up here.
*
*  The depth is decided ONCE, outside the loop. That is the whole reason this
*  is not a loop over store_pixel(): a branch per pixel on a 1024 pixel row is
*  1024 branches to answer a question whose answer cannot change during the
*  call. */
static void span_raw(const gfx_surface *s, unsigned char *dst, int len,
                     gfx_color pixel)
{
	int i;

	switch(s->bytes)
	{
		case 4:
		{
			unsigned int *p = (unsigned int *)dst;
			for(i = 0; i < len; i++)
				p[i] = pixel;
			break;
		}
		case 3:
		{
			unsigned char *p = dst;
			for(i = 0; i < len; i++)
			{
				p[0] = (unsigned char)pixel;
				p[1] = (unsigned char)(pixel >> 8);
				p[2] = (unsigned char)(pixel >> 16);
				p += 3;
			}
			break;
		}
		case 2:
		{
			unsigned short *p = (unsigned short *)dst;
			for(i = 0; i < len; i++)
				p[i] = (unsigned short)pixel;
			break;
		}
		default:
		{
			unsigned char *p = dst;
			for(i = 0; i < len; i++)
				p[i] = (unsigned char)pixel;
			break;
		}
	}
}


/* ------------------------------------------------------------------ */
/* Clipping                                                            */
/* ------------------------------------------------------------------ */

/* A horizontal run, clipped to the clip window. This is where the contract
*  gfxlib.h states is honoured, and it is honoured edge for edge:
*
*    len <= 0        nothing is drawn, and a zero width fill is not an error
*    y outside       nothing is drawn
*    x past the      nothing is drawn
*      right edge
*    x left of the   the run is SHORTENED FROM THE LEFT and starts at the left
*      left edge     edge, so the part of it that is inside appears in the
*                    columns it belongs in -- it is not shifted, and it does
*                    not wrap into the previous row, which is the failure a
*                    picture hides and a guard pixel catches
*    too long        the run is cut off at the right edge, again without
*                    wrapping into the next row
*
*  x + len is NEVER formed. A caller may legitimately pass a length of a
*  million and the sum would overflow before the test could reject it.
*
*  The left cut is computed in UNSIGNED arithmetic, which is where this
*  differs from src/video/fbdraw.c's hspan(). fbdraw.c writes "len += x" with x
*  negative, which is right for every value it can actually be handed but
*  undefined for x near INT_MIN. clip.x - x has a true value between 1 and
*  2^31 + 16384 whenever this branch is taken, which does not fit an int but
*  does fit an unsigned int exactly, so the subtraction below is the correct
*  distance for every input rather than for the reasonable ones. It costs
*  nothing and it is one fewer thing a caller has to be careful about.
*
*  Callers must have a usable surface: this reads the clip window and checks
*  nothing else, which is what lets a fill of 768 rows validate once instead
*  of 768 times. */
static void hspan(const gfx_surface *s, int x, int y, int len, gfx_color pixel)
{
	unsigned int cut;

	if(len <= 0)
		return;
	if(y < s->clip.y || y >= s->clip.y + s->clip.h)
		return;
	if(x >= s->clip.x + s->clip.w)
		return;

	if(x < s->clip.x)
	{
		cut = (unsigned int)s->clip.x - (unsigned int)x;
		if(cut >= (unsigned int)len)
			return;         /* entirely left of the clip window */
		len -= (int)cut;
		x = s->clip.x;
	}

	if(len > s->clip.x + s->clip.w - x)
		len = s->clip.x + s->clip.w - x;

	span_raw(s, row_at(s, y) + (unsigned long)x * s->bytes, len, pixel);
}

/* The vertical counterpart, clipped the same way against the top and bottom.
*  Consecutive pixels of a column are a whole pitch apart, so there is no run
*  to hand to span_raw() -- but the depth still leaves the loop, for the same
*  reason it does there. */
static void vspan(const gfx_surface *s, int x, int y, int len, gfx_color pixel)
{
	unsigned char *p;
	unsigned int cut;
	int i;

	if(len <= 0)
		return;
	if(x < s->clip.x || x >= s->clip.x + s->clip.w)
		return;
	if(y >= s->clip.y + s->clip.h)
		return;

	if(y < s->clip.y)
	{
		cut = (unsigned int)s->clip.y - (unsigned int)y;
		if(cut >= (unsigned int)len)
			return;
		len -= (int)cut;
		y = s->clip.y;
	}

	if(len > s->clip.y + s->clip.h - y)
		len = s->clip.y + s->clip.h - y;

	p = row_at(s, y) + (unsigned long)x * s->bytes;

	switch(s->bytes)
	{
		case 4:
			for(i = 0; i < len; i++)
			{
				*(unsigned int *)p = pixel;
				p += s->pitch;
			}
			break;
		case 3:
			for(i = 0; i < len; i++)
			{
				p[0] = (unsigned char)pixel;
				p[1] = (unsigned char)(pixel >> 8);
				p[2] = (unsigned char)(pixel >> 16);
				p += s->pitch;
			}
			break;
		case 2:
			for(i = 0; i < len; i++)
			{
				*(unsigned short *)p = (unsigned short)pixel;
				p += s->pitch;
			}
			break;
		default:
			for(i = 0; i < len; i++)
			{
				*p = (unsigned char)pixel;
				p += s->pitch;
			}
			break;
	}
}

/* One pixel, clipped. Anything outside the clip window is dropped, which is
*  what lets Bresenham step without bounding anything. */
static void point(const gfx_surface *s, int x, int y, gfx_color pixel)
{
	if(x < s->clip.x || x >= s->clip.x + s->clip.w)
		return;
	if(y < s->clip.y || y >= s->clip.y + s->clip.h)
		return;

	store_pixel(s, row_at(s, y) + (unsigned long)x * s->bytes, pixel);
}

static int coord_sane(int v)
{
	return (v > -GFX_COORD_MAX && v < GFX_COORD_MAX);
}

/* Non-zero when the surface can be drawn on at all and the clip window is not
*  empty. Every public primitive starts here, so a surface that
*  gfx_surface_init() refused, or one clipped to nothing, silently draws
*  nothing instead of faulting. */
static int usable(const gfx_surface *s)
{
	return (s != 0 && s->mem != 0 && s->clip.w > 0 && s->clip.h > 0);
}


/* ------------------------------------------------------------------ */
/* The shapes                                                          */
/* ------------------------------------------------------------------ */

void gfx_clear(gfx_surface *s, gfx_color colour)
{
	unsigned char *dst;
	int y;

	if(!usable(s))
		return;

	/* Row by row rather than one flat run across the whole block: the rows
	*  of a padded surface are not contiguous, and a span that crossed the
	*  padding would write into it -- which on the last row is outside the
	*  surface. And it is the clip window that is cleared, not the surface,
	*  which is what makes this usable as the first step of a repaint of one
	*  damage rectangle. */
	dst = row_at(s, s->clip.y) + (unsigned long)s->clip.x * s->bytes;

	for(y = 0; y < s->clip.h; y++)
	{
		span_raw(s, dst, s->clip.w, colour);
		dst += s->pitch;
	}
}

void gfx_pixel(gfx_surface *s, int x, int y, gfx_color colour)
{
	if(!usable(s))
		return;

	point(s, x, y, colour);
}

void gfx_hline(gfx_surface *s, int x, int y, int len, gfx_color colour)
{
	if(!usable(s))
		return;

	hspan(s, x, y, len, colour);
}

void gfx_vline(gfx_surface *s, int x, int y, int len, gfx_color colour)
{
	if(!usable(s))
		return;

	vspan(s, x, y, len, colour);
}

/* Outcode bits for the line clipper (Cohen-Sutherland). */
#define CLIP_LEFT    1
#define CLIP_RIGHT   2
#define CLIP_ABOVE   4
#define CLIP_BELOW   8

/* Which edges of the clip window a point lies beyond, or 0 for one inside. */
static int outcode(const gfx_surface *s, int x, int y)
{
	int code = 0;

	if(x < s->clip.x)
		code |= CLIP_LEFT;
	else if(x > s->clip.x + s->clip.w - 1)
		code |= CLIP_RIGHT;

	if(y < s->clip.y)
		code |= CLIP_ABOVE;
	else if(y > s->clip.y + s->clip.h - 1)
		code |= CLIP_BELOW;

	return code;
}

/* Integer division rounded to nearest rather than truncated towards zero. The
*  clipper picks a pixel to stand for a point that really lies between two of
*  them, and truncation always errs the same way; rounding halves the error
*  and stops it leaning consistently in one direction. */
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

/* Cohen-Sutherland, as in src/video/fbdraw.c and for the same reasons: it trims the
*  segment before rastering, so a line from (-9000,-9000) to (9000,9000) costs
*  the pixels it actually draws and not eighteen thousand clipped-away steps.
*
*  Every intersection is computed from the ORIGINAL endpoints, never from one
*  a previous pass has already moved and rounded; clipping the second end
*  against an already-rounded first end tilts the whole segment, and the tilt
*  shows as a visible bend at the far end. The pass count is capped because a
*  rounded point can land one pixel outside the edge it was meant to sit on,
*  which the next pass clips again -- harmless, but the one way this loop
*  could cycle.
*
*  Neither divisor can be zero: gfx_line() sends the purely horizontal and
*  purely vertical cases to hspan()/vspan() before this is reached. */
static int clip_line(const gfx_surface *s, int *px0, int *py0,
                     int *px1, int *py1)
{
	int ox0 = *px0, oy0 = *py0, ox1 = *px1, oy1 = *py1;
	int dx = ox1 - ox0, dy = oy1 - oy0;
	int x0 = ox0, y0 = oy0, x1 = ox1, y1 = oy1;
	int c0 = outcode(s, x0, y0);
	int c1 = outcode(s, x1, y1);
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
			y = s->clip.y;
			x = ox0 + div_round(dx * (y - oy0), dy);
		}
		else if((c & CLIP_BELOW) != 0)
		{
			y = s->clip.y + s->clip.h - 1;
			x = ox0 + div_round(dx * (y - oy0), dy);
		}
		else if((c & CLIP_LEFT) != 0)
		{
			x = s->clip.x;
			y = oy0 + div_round(dy * (x - ox0), dx);
		}
		else
		{
			x = s->clip.x + s->clip.w - 1;
			y = oy0 + div_round(dy * (x - ox0), dx);
		}

		if(c == c0)
		{
			x0 = x;
			y0 = y;
			c0 = outcode(s, x0, y0);
		}
		else
		{
			x1 = x;
			y1 = y;
			c1 = outcode(s, x1, y1);
		}
	}

	return 0;
}

/* Bresenham, all eight octants.
*
*  The two degenerate cases go to hspan()/vspan() first: a horizontal run
*  becomes one span write instead of hundreds of conditional steps, and a
*  vertical one skips the error term entirely.
*
*  After clipping both endpoints are inside the clip window and Bresenham
*  never leaves the bounding box of its endpoints, so every pixel is inside.
*  The bounds test in point() is therefore redundant -- and stays, because it
*  costs two compares against values already in registers and it is the
*  difference between a clipper bug being a cosmetic glitch and a clipper bug
*  being a write into somebody else's page. */
void gfx_line(gfx_surface *s, int x0, int y0, int x1, int y1, gfx_color colour)
{
	int dx, dy, sx, sy, err, e2;

	if(!usable(s))
		return;
	if(!coord_sane(x0) || !coord_sane(y0) || !coord_sane(x1) || !coord_sane(y1))
		return;

	if(y0 == y1)
	{
		if(x0 <= x1)
			hspan(s, x0, y0, x1 - x0 + 1, colour);
		else
			hspan(s, x1, y0, x0 - x1 + 1, colour);
		return;
	}

	if(x0 == x1)
	{
		if(y0 <= y1)
			vspan(s, x0, y0, y1 - y0 + 1, colour);
		else
			vspan(s, x0, y1, y0 - y1 + 1, colour);
		return;
	}

	if(!clip_line(s, &x0, &y0, &x1, &y1))
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
		point(s, x0, y0, colour);

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

/* Outlined rectangle. Every one of the four sides clips itself, which is what
*  makes a rectangle straddling two edges come out right -- the two visible
*  sides are drawn clipped and the two invisible ones are dropped by the
*  length tests inside hspan()/vspan(). The vertical sides skip the rows the
*  horizontal ones already covered, so no corner pixel is written twice. */
void gfx_draw_rect(gfx_surface *s, int x, int y, int w, int h, gfx_color colour)
{
	if(w <= 0 || h <= 0)
		return;
	if(!coord_sane(x) || !coord_sane(y) || !coord_sane(w) || !coord_sane(h))
		return;
	if(!usable(s))
		return;

	hspan(s, x, y, w, colour);
	if(h > 1)
		hspan(s, x, y + h - 1, w, colour);

	if(h > 2)
	{
		vspan(s, x, y + 1, h - 2, colour);
		if(w > 1)
			vspan(s, x + w - 1, y + 1, h - 2, colour);
	}
}

/* Filled rectangle.
*
*  The vertical extent is clipped here so that a height of ten thousand does
*  not mean ten thousand span calls that each decide they have nothing to do;
*  the horizontal extent is settled once and then reused, because the
*  horizontal answer is the same for every row of a rectangle and there is no
*  sense recomputing it h times. */
void gfx_fill_rect(gfx_surface *s, int x, int y, int w, int h, gfx_color colour)
{
	unsigned char *dst;
	int i;

	if(w <= 0 || h <= 0)
		return;
	if(!coord_sane(x) || !coord_sane(y) || !coord_sane(w) || !coord_sane(h))
		return;
	if(!usable(s))
		return;

	if(y < s->clip.y)
	{
		h -= s->clip.y - y;
		y = s->clip.y;
		if(h <= 0)
			return;
	}
	if(y >= s->clip.y + s->clip.h)
		return;
	if(h > s->clip.y + s->clip.h - y)
		h = s->clip.y + s->clip.h - y;

	if(x >= s->clip.x + s->clip.w)
		return;
	if(x < s->clip.x)
	{
		w -= s->clip.x - x;
		x = s->clip.x;
		if(w <= 0)
			return;
	}
	if(w > s->clip.x + s->clip.w - x)
		w = s->clip.x + s->clip.w - x;

	dst = row_at(s, y) + (unsigned long)x * s->bytes;

	for(i = 0; i < h; i++)
	{
		span_raw(s, dst, w, colour);
		dst += s->pitch;
	}
}

/* One glyph at (x,y), which is its top left corner, every pixel of it blown
*  up into a scale x scale block.
*
*  The background is always left alone: there is no bg parameter and so no
*  choice to make, which is both the cheaper option and the one that lets a
*  caption sit on top of something without a box around it.
*
*  The font covers 0..127. A character with the high bit set -- a byte of a
*  UTF-8 sequence, or a code page character out of a file -- has no glyph, so
*  it is drawn as '?'. Substituting rather than skipping keeps the fact that
*  something was lost visible, and keeps a run of them from looking like an
*  unexplained gap.
*
*  Set bits are coalesced into runs before being drawn, so the crossbar of an
*  'E' is one span of five blocks rather than five separate ones. At scale 1
*  that saves the call overhead; at scale 3, where a run is 15 pixels wide and
*  3 rows deep, it is the difference between 15 span calls and 3. */
void gfx_char(gfx_surface *s, int x, int y, char c, int scale, gfx_color colour)
{
	const unsigned char *glyph;
	unsigned int bits;
	int index, row, col, run, i, py;

	if(scale < 1 || scale > GFX_MAX_SCALE)
		return;
	if(!coord_sane(x) || !coord_sane(y))
		return;
	if(!usable(s))
		return;

	/* Whole cell outside the clip window: nothing to do. Tested before the
	*  glyph is looked up, because this is the common case in a string that
	*  runs off an edge and it should cost as little as possible. */
	if(x >= s->clip.x + s->clip.w || y >= s->clip.y + s->clip.h)
		return;
	if(x + GFX_FONT_WIDTH * scale <= s->clip.x ||
	   y + GFX_FONT_HEIGHT * scale <= s->clip.y)
		return;

	index = (int)(unsigned char)c;
	if(index > 127)
		index = '?';
	glyph = gfx_font[index];

	for(row = 0; row < GFX_FONT_HEIGHT; row++)
	{
		bits = glyph[row];
		if(bits == 0)
			continue;       /* an empty glyph row is most of a font */

		py = y + row * scale;
		col = 0;

		while(col < GFX_FONT_WIDTH)
		{
			/* One byte per row, most significant bit leftmost. */
			if((bits & (0x80u >> col)) == 0)
			{
				col++;
				continue;
			}

			run = 1;
			while(col + run < GFX_FONT_WIDTH &&
			      (bits & (0x80u >> (col + run))) != 0)
				run++;

			/* The block, scale rows deep. hspan() clips each row on
			*  its own, so a glyph hanging over any edge -- or over
			*  two of them -- needs no case of its own here. */
			for(i = 0; i < scale; i++)
				hspan(s, x + col * scale, py + i,
				      run * scale, colour);

			col += run;
		}
	}
}

void gfx_text(gfx_surface *s, int x, int y, const char *text, int scale,
              gfx_color colour)
{
	if(text == 0)
		return;
	if(scale < 1 || scale > GFX_MAX_SCALE)
		return;
	if(!coord_sane(x) || !coord_sane(y))
		return;
	if(!usable(s))
		return;
	if(y >= s->clip.y + s->clip.h ||
	   y + GFX_FONT_HEIGHT * scale <= s->clip.y)
		return;

	while(*text != '\0')
	{
		if(x >= s->clip.x + s->clip.w)
			break;          /* right edge reached, no wrap */

		gfx_char(s, x, y, *text, scale, colour);
		x += GFX_FONT_WIDTH * scale;

		/* A string long enough to walk past the coordinate limit would
		*  break the overflow argument the clipping rests on. It cannot
		*  produce a visible pixel from here on either, since x only
		*  grows. */
		if(!coord_sane(x))
			break;

		text++;
	}
}

int gfx_text_width(const char *text, int scale)
{
	if(text == 0 || scale < 1)
		return 0;

	return (int)strlen(text) * GFX_FONT_WIDTH * scale;
}


/* ------------------------------------------------------------------ */
/* Opening and closing the screen                                      */
/* ------------------------------------------------------------------ */

int gfx_open(void)
{
	int rc;
	int bytes;

	if(gfx_have_fb)
		return 0;               /* already ours */

	rc = sys_mapfb(&gfx_info);
	if(rc < 0)
		return rc;              /* SYS_ENODEV, SYS_EBUSY, ... */

	gfx_have_fb = 1;

	bytes = (int)((gfx_info.bpp + 7) / 8);

	/* The back buffer is a fixed array and cannot grow, so a mode larger
	*  than it is refused here rather than drawn into a corner of. The
	*  screen goes straight back: a refusal must not leave the console
	*  suspended behind a program that has given up.
	*
	*  Each of the three dimensions is tested on its own rather than as one
	*  product, because the product is what has to be prevented from
	*  overflowing before it can be compared -- and because "wider than
	*  1920" is something a caller can put in a message, whereas "more than
	*  8294400 bytes" is not. The byte budget is then asserted below as
	*  well, on numbers now known to be small enough to multiply. */
	if(gfx_info.width  > (unsigned long)GFX_MAX_WIDTH ||
	   gfx_info.height > (unsigned long)GFX_MAX_HEIGHT ||
	   bytes > GFX_MAX_BYTES)
	{
		gfx_close();
		return SYS_ENOMEM;
	}

	/* The invariant the whole arrangement rests on, stated in the one form
	*  that cannot drift: the rows the canvas will actually use, at the
	*  pitch it will actually use, fit in the array. Width, height and depth
	*  are each within their limit by now, so the product is at most
	*  GFX_BACK_BYTES and cannot overflow. */
	if(gfx_info.width * (unsigned long)bytes * gfx_info.height >
	   GFX_BACK_BYTES)
	{
		gfx_close();
		return SYS_ENOMEM;
	}

	/* The mapping has to actually contain what the geometry claims. The
	*  test is written as a division so that pitch * height cannot overflow
	*  on the way to being compared -- and it is deliberately strict about
	*  the padding after the last row, because the alternative is a blit
	*  that walks off the end of the mapping on the very last row of the
	*  very last frame, which is the hardest kind of fault to find. */
	if(gfx_info.addr == 0 || gfx_info.pitch == 0 ||
	   gfx_info.height > gfx_info.size / gfx_info.pitch)
	{
		gfx_close();
		return SYS_EINVAL;
	}

	if(!gfx_surface_init(&gfx_scr, (void *)gfx_info.addr,
	                     (int)gfx_info.width, (int)gfx_info.height,
	                     gfx_info.pitch, gfx_info.bpp,
	                     gfx_info.red_pos, gfx_info.red_size,
	                     gfx_info.green_pos, gfx_info.green_size,
	                     gfx_info.blue_pos, gfx_info.blue_size))
	{
		gfx_close();
		return SYS_EINVAL;
	}

	/* The canvas gets the SCREEN'S pixel format and a pitch of its own.
	*
	*  Same format, because that is what makes gfx_flush() a copy: a canvas
	*  in some canonical 32 bpp layout would need a per-pixel conversion on
	*  the way out, on the hottest path in the program, to buy nothing at
	*  all -- the drawing code is depth-agnostic already.
	*
	*  Its own pitch, packed to width * bytes, because nothing pads it: the
	*  padding in a framebuffer's pitch is the card's business, and copying
	*  it here would only make the buffer bigger. The two pitches therefore
	*  differ in general, which is exactly why gfx_blit() copies row by row
	*  and never treats either surface as one flat block.
	*
	*  THE NEGOTIATED WIDTH AND NOT GFX_MAX_WIDTH. The array is sized for
	*  the ceiling and the surface is sized for the mode, and those are two
	*  different numbers on every machine that did not get the top mode.
	*  Padding the canvas out to the maximum pitch instead would still draw
	*  a correct picture here -- and would make every row of the blit start
	*  in the wrong place the moment gfx_blit() reads a row address from it,
	*  which is the diagonal shear. */
	if(!gfx_surface_init(&gfx_buf, gfx_back,
	                     (int)gfx_info.width, (int)gfx_info.height,
	                     gfx_info.width * (unsigned long)bytes, gfx_info.bpp,
	                     gfx_info.red_pos, gfx_info.red_size,
	                     gfx_info.green_pos, gfx_info.green_size,
	                     gfx_info.blue_pos, gfx_info.blue_size))
	{
		gfx_close();
		return SYS_EINVAL;
	}

	gfx_dmg_n = 0;
	return 0;
}

void gfx_close(void)
{
	if(!gfx_have_fb)
		return;

	gfx_have_fb = 0;
	gfx_scr.mem = 0;
	gfx_buf.mem = 0;
	gfx_dmg_n = 0;

	sys_unmapfb();
}

gfx_surface *gfx_canvas(void)
{
	return &gfx_buf;
}

gfx_surface *gfx_screen(void)
{
	return &gfx_scr;
}


/* ------------------------------------------------------------------ */
/* Getting the picture onto the screen                                 */
/* ------------------------------------------------------------------ */

/* One row of the blit.
*
*  Dwords first, then whatever is left over. Four bytes at a time is four
*  times fewer loop iterations and four times fewer bus transactions than the
*  byte loop in user/lib/lib.c's memcpy(), and on a full screen that is the
*  difference between a frame and a slideshow.
*
*  The dword accesses are NOT guaranteed to be aligned. The two surfaces have
*  different pitches and a blit starts at an arbitrary column, so either
*  pointer can land on any byte. On x86 an unaligned dword access is legal and
*  merely slower -- and this file is x86 only by construction, since it is
*  compiled -m32 for a kernel that has no other port -- so the alternative,
*  branching on alignment and falling back to bytes, would cost more on the
*  common aligned case than it saves on the rare unaligned one.
*
*  The tail is bytes because a length that is not a multiple of four occurs
*  for 24 bpp and for 16 bpp with an odd column count, and reading a whole
*  dword to write three bytes of it would read past the end of the row. */
static void copy_row(unsigned char *dst, const unsigned char *src,
                     unsigned long n)
{
	unsigned long words;
	unsigned long i;
	unsigned int *d;
	const unsigned int *sw;

	words = n / 4;
	d = (unsigned int *)dst;
	sw = (const unsigned int *)src;

	for(i = 0; i < words; i++)
		d[i] = sw[i];

	for(i = words * 4; i < n; i++)
		dst[i] = src[i];
}

unsigned long gfx_blit(int x, int y, int w, int h)
{
	unsigned char *dst;
	const unsigned char *src;
	unsigned long bytes_per_row;
	unsigned long total;
	int i;
	int limit_w;
	int limit_h;

	if(gfx_scr.mem == 0 || gfx_buf.mem == 0)
		return 0;
	if(w <= 0 || h <= 0)
		return 0;
	if(!coord_sane(x) || !coord_sane(y) || !coord_sane(w) || !coord_sane(h))
		return 0;

	/* Clipped against BOTH surfaces. They have the same geometry here, but
	*  saying so and relying on it are different things, and the cost is two
	*  comparisons against a blit of a hundred thousand pixels. */
	limit_w = (gfx_scr.w < gfx_buf.w) ? gfx_scr.w : gfx_buf.w;
	limit_h = (gfx_scr.h < gfx_buf.h) ? gfx_scr.h : gfx_buf.h;

	if(x < 0)
	{
		w += x;
		x = 0;
		if(w <= 0)
			return 0;
	}
	if(y < 0)
	{
		h += y;
		y = 0;
		if(h <= 0)
			return 0;
	}
	if(x >= limit_w || y >= limit_h)
		return 0;
	if(w > limit_w - x)
		w = limit_w - x;
	if(h > limit_h - y)
		h = limit_h - y;

	bytes_per_row = (unsigned long)w * (unsigned long)gfx_scr.bytes;

	dst = row_at(&gfx_scr, y) + (unsigned long)x * gfx_scr.bytes;
	src = row_at(&gfx_buf, y) + (unsigned long)x * gfx_buf.bytes;

	total = 0;
	for(i = 0; i < h; i++)
	{
		copy_row(dst, src, bytes_per_row);
		dst += gfx_scr.pitch;
		src += gfx_buf.pitch;
		total += bytes_per_row;
	}

	return total;
}

/* True when two rectangles share at least one pixel. Touching edges do not
*  count: two rectangles that merely abut are cheaper blitted separately than
*  merged, because the merge of an L-shaped pair covers the empty corner too. */
static int rect_overlap(const gfx_rect *a, const gfx_rect *b)
{
	if(a->x + a->w <= b->x || b->x + b->w <= a->x)
		return 0;
	if(a->y + a->h <= b->y || b->y + b->h <= a->y)
		return 0;
	return 1;
}

/* a becomes the smallest rectangle containing both. */
static void rect_union(gfx_rect *a, const gfx_rect *b)
{
	int x1 = a->x + a->w;
	int y1 = a->y + a->h;

	if(b->x < a->x)              a->x = b->x;
	if(b->y < a->y)              a->y = b->y;
	if(b->x + b->w > x1)         x1 = b->x + b->w;
	if(b->y + b->h > y1)         y1 = b->y + b->h;

	a->w = x1 - a->x;
	a->h = y1 - a->y;
}

void gfx_damage(int x, int y, int w, int h)
{
	gfx_rect r;
	int i;
	int merged;

	if(gfx_scr.mem == 0)
		return;
	if(w <= 0 || h <= 0)
		return;
	if(!coord_sane(x) || !coord_sane(y) || !coord_sane(w) || !coord_sane(h))
		return;

	/* Clipped to the screen on the way in, once, so that everything after
	*  this point -- the overlap tests, the unions, the bounding box -- runs
	*  on rectangles that are already known to be inside it. A damage list
	*  of on-screen rectangles cannot merge into an off-screen one. */
	r.x = x;
	r.y = y;
	r.w = w;
	r.h = h;

	if(r.x < 0)
	{
		r.w += r.x;
		r.x = 0;
	}
	if(r.y < 0)
	{
		r.h += r.y;
		r.y = 0;
	}
	if(r.w <= 0 || r.h <= 0)
		return;
	if(r.x >= gfx_scr.w || r.y >= gfx_scr.h)
		return;
	if(r.w > gfx_scr.w - r.x)
		r.w = gfx_scr.w - r.x;
	if(r.h > gfx_scr.h - r.y)
		r.h = gfx_scr.h - r.y;

	/* Absorb every pending rectangle this one touches, and repeat: a merge
	*  makes the rectangle bigger, which can bring it into contact with one
	*  that was previously clear of it. The outer loop runs at most
	*  GFX_DAMAGE_MAX times because each pass removes at least one entry. */
	for(;;)
	{
		merged = 0;

		for(i = 0; i < gfx_dmg_n; i++)
		{
			if(!rect_overlap(&r, &gfx_dmg[i]))
				continue;

			rect_union(&r, &gfx_dmg[i]);
			gfx_dmg[i] = gfx_dmg[gfx_dmg_n - 1];
			gfx_dmg_n--;
			merged = 1;
			break;
		}

		if(!merged)
			break;
	}

	if(gfx_dmg_n >= GFX_DAMAGE_MAX)
	{
		/* The array is full and this rectangle touches none of them.
		*  Everything collapses into one bounding box: a bigger blit,
		*  never a wrong picture, and the only outcome that does not
		*  need memory this program does not have. */
		for(i = 0; i < gfx_dmg_n; i++)
			rect_union(&r, &gfx_dmg[i]);

		gfx_dmg[0] = r;
		gfx_dmg_n = 1;
		return;
	}

	gfx_dmg[gfx_dmg_n] = r;
	gfx_dmg_n++;
}

int gfx_damage_count(void)
{
	return gfx_dmg_n;
}

int gfx_damage_at(int index, gfx_rect *out)
{
	if(out == 0 || index < 0 || index >= gfx_dmg_n)
		return 0;

	*out = gfx_dmg[index];
	return 1;
}

void gfx_damage_clear(void)
{
	gfx_dmg_n = 0;
}

unsigned long gfx_flush(void)
{
	unsigned long total;
	int i;

	total = 0;
	for(i = 0; i < gfx_dmg_n; i++)
		total += gfx_blit(gfx_dmg[i].x, gfx_dmg[i].y,
		                  gfx_dmg[i].w, gfx_dmg[i].h);

	gfx_dmg_n = 0;
	return total;
}
