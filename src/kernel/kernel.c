/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: C Code entry
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (fiesenb@gmail.com)
*/

#include <system.h>
#include <stdio.h>
#include <string.h>
#include <hardware.h>
#include <mm.h>
#include <syscall.h>
#include <vmm.h>
#include <fbcon.h>
#include <mouse.h>
#include <usb.h>
#include <exec.h>
#include <ata.h>
#include <blockdev.h>
#include <fat.h>
#include <pci.h>
#include <rtl8139.h>
#include <e1000.h>
#include <net.h>
#include <dns.h>
#include <tcp.h>
#define cpuid(in, a, b, c, d) __asm__("cpuid": "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (in));

/* --- memcpy() / memset() --------------------------------------------------
*
*  Both used to move one byte per iteration, which is what a 2011 tutorial
*  kernel starts out with and what everything above them has quietly paid for
*  ever since. The numbers are not subtle: a 1024x768x32 back buffer is
*  3145728 bytes, so a single blit of one frame was three million iterations
*  of load, store, two increments and a branch. The framebuffer console's
*  scroll, the ELF loader's segment copies and the network stack's ring
*  buffers all sit on the same two functions.
*
*  What follows moves 32 bits at a time. Measured in the kernel with
*  timer_get_ticks(), 64 repetitions of the 3145728 byte case, on a heap
*  buffer, so 192 MB moved per figure:
*
*      memcpy  byte loop  64 ms  ->  rep movsl  17 ms   (1.00 -> 0.27 ms/blit)
*      memset  byte loop  52 ms  ->  rep stosl   6 ms   (0.81 -> 0.09 ms/clear)
*
*  A factor of 3.8 for the copy and 8.7 for the clear. The clear gains more
*  because a store-only loop was already close to what the store buffer can
*  retire, so replacing it removes almost all of the remaining instruction
*  overhead, while the copy stays bounded by moving 6 MB through the cache
*  either way. Note what that also says: the byte loop was never 32 times
*  slower than a dword move. Out-of-order execution hides most of a tight
*  byte loop, and the honest reason to change this is not the factor but that
*  the factor is paid once per frame, forever.
*
*  Three decisions went into it, and each one is a decision rather than a
*  style preference:
*
*  1. rep movsl / rep stosl instead of a C uint32_t loop.
*
*     This is not taste, it is what the compiler actually produces. GCC 16 at
*     the -O1 this kernel is built with turns
*
*         while(words--) { *(uint32_t *)dp = *(const uint32_t *)sp;
*                          dp += 4; sp += 4; }
*
*     into a six instruction loop body -- load, store, two adds, compare,
*     branch -- and neither unrolls it nor recognises it as a block move.
*     That is the byte loop with a quarter of the iterations and nothing more.
*     "rep movsl" is ONE instruction for the whole run, and on every x86 since
*     the 486 the string move is the path the hardware optimises: it issues
*     full cache-line transfers instead of one bus cycle per dword.
*
*     There is a second reason, and on a freestanding kernel it is the more
*     dangerous one. A C loop that copies memory is exactly the pattern
*     -ftree-loop-distribute-patterns replaces with a call to memcpy(). It is
*     off at -O1 and -fno-builtin is in the flags as well, so it does not
*     happen today -- but the failure mode if either ever changes is memcpy()
*     compiled into a call to itself: an infinite recursion inside the one
*     function every other file depends on. Inline assembly cannot be turned
*     into a library call, which takes the question off the table for good.
*     Inline assembly is also what the rest of this file already uses for
*     port I/O, so it is nothing new here.
*
*     -mgeneral-regs-only rules out the SSE paths a hosted libc would take;
*     32 bits at a time is the widest this kernel may move.
*
*  2. The DESTINATION is aligned, not the source.
*
*     Only one of the two can be: dest and src may differ in their low two
*     bits, and no number of head bytes ever makes those agree. So the head
*     loop picks a side, and the measurement says the choice is worth less
*     than one might think. Timed inside the kernel, 64 copies of 3145728
*     bytes between two heap buffers:
*
*         dest and src both aligned                 17 ms
*         dest and src both off by one (mutually
*           aligned once the head loop has run)     18 ms
*         dest off by one, src aligned              43 ms
*         dest aligned, src off by one              50 ms
*         ... and the same two with the SOURCE
*         aligned by the head loop instead:         42 ms / 51 ms
*
*     The last two lines are the point. Aligning the source instead of the
*     destination gives 42/51 against 43/50 -- the same numbers, inside the
*     1 ms resolution of the tick counter. What actually costs a factor of
*     2.5 is that the two sides are MUTUALLY misaligned, and neither choice
*     can fix that. On a CPU with cheap unaligned access the decision is
*     therefore free, and the reason it is the destination anyway is a case
*     this measurement does not contain: a copy INTO the framebuffer. That
*     memory is write-combining, and an unaligned store there can break the
*     combining buffer apart and turn one burst into two partial bus writes,
*     which a misaligned load never does. When the destination is MMIO, the
*     destination is the side to align; when it is RAM, it does not matter.
*     Aligning the destination is right in both.
*
*  3. The copy direction stays FORWARD, and that is load bearing.
*
*     fbcon_scroll() copies its shadow buffer up by one row -- destination
*     BELOW source, ranges overlapping -- and its comment says in so many
*     words that it relies on the forward byte loop this replaces. It is the
*     only caller in the kernel that overlaps at all; the other ring buffers
*     (net.c, tcp.c) copy between separate buffers, and heap.c's realloc()
*     copies into a fresh block. There is no memmove() on the kernel side --
*     user/lib/lib.c has one, but that is a different binary.
*
*     "rep movsb"/"rep movsl" with DF clear ascend, so the direction is kept.
*     What is NOT kept is byte-for-byte equivalence with the old loop when
*     dest and src overlap by fewer than four bytes: writing a dword at dp
*     touches sp[-3..0] before they are read, whereas the byte loop would have
*     read each one first. That difference is only visible for src - dest in
*     1..3, i.e. the "replicate a small pattern forwards" trick. Nothing here
*     does that -- fbcon's two rows are 256 bytes apart -- and the small-copy
*     path below is still a plain byte loop, but it is the one property a
*     future caller must not assume.
*
*  The threshold: below MEMOP_BLOCK_MIN bytes the byte loop wins outright.
*  Computing head/words/tail and starting three "rep" instructions costs on
*  the order of a few dozen cycles of setup, and the kernel's most frequent
*  copy by count is ETH_ALEN -- six bytes, in net.c's frame builders. Sixteen
*  is where the two are roughly even and is deliberately generous: a copy that
*  small is never on a path that matters.
*/
#define MEMOP_BLOCK_MIN 16

void *memcpy(void *dest, const void *src, size_t count)
{
    unsigned char *dp;
    const unsigned char *sp;
    size_t head;
    size_t words;
    size_t tail;

    dp = (unsigned char *)dest;
    sp = (const unsigned char *)src;

    if(count < MEMOP_BLOCK_MIN)
    {
        while(count != 0)
        {
            *dp++ = *sp++;
            count--;
        }
        return dest;
    }

    /* Bytes up to the next 4 byte boundary of the DESTINATION: 0 when dp is
    *  already aligned, otherwise 4 - (dp & 3). Written as the negation so
    *  there is no branch and no subtraction that could wrap. count is at
    *  least MEMOP_BLOCK_MIN here, so head can never exceed it. */
    head = (size_t)((0u - (uint32_t)dp) & 3u);
    count -= head;
    words = count >> 2;
    tail  = count & 3u;

    /* Three statements rather than one block with the counts in scratch
    *  registers: each one names edi/esi/ecx as read-write operands, so the
    *  compiler threads the updated pointers from one to the next itself and
    *  there is no input register that could be allocated on top of an output
    *  -- the classic way an inline "rep" sequence goes wrong.
    *
    *  cld belongs to the first of them. The System V ABI says DF is clear at
    *  every function boundary and start.asm clears it once at entry, so this
    *  is belt and braces -- but a single interrupt stub that ever left DF set
    *  would otherwise turn every copy in the kernel into a backwards one, and
    *  that is not a bug anybody wants to find from the outside.
    *
    *  A "rep" with ecx == 0 is a no-op, so the head and tail statements cost
    *  one predictable branch each when there is nothing for them to do. */
    __asm__ __volatile__("cld\n\trep movsb"
        : "+D"(dp), "+S"(sp), "+c"(head)
        : : "memory", "cc");
    __asm__ __volatile__("rep movsl"
        : "+D"(dp), "+S"(sp), "+c"(words)
        : : "memory", "cc");
    __asm__ __volatile__("rep movsb"
        : "+D"(dp), "+S"(sp), "+c"(tail)
        : : "memory", "cc");

    return dest;
}

/* The signature is the one in system.h and does not change: val is a char,
*  not the int a hosted memset() takes. Every caller in the kernel passes a
*  character or a zero, so widening it would buy nothing and would silently
*  change the meaning of the dozens of existing call sites -- memset(p, 256,
*  n) means something different in the two versions.
*
*  What the char DOES force is the cast on the way in. Plain char is signed on
*  x86, so memset(p, 0xFF, n) arrives as -1, and multiplying that by
*  0x01010101 as a signed value is signed overflow. The cast to unsigned char
*  first is what makes the pattern 0xFFFFFFFF instead of undefined. */
void *memset(void *dest, char val, size_t count)
{
    unsigned char *dp;
    uint32_t byte;
    uint32_t pattern;
    size_t head;
    size_t words;
    size_t tail;

    dp = (unsigned char *)dest;
    byte = (uint32_t)(unsigned char)val;

    if(count < MEMOP_BLOCK_MIN)
    {
        while(count != 0)
        {
            *dp++ = (unsigned char)byte;
            count--;
        }
        return dest;
    }

    /* One byte smeared across all four lanes. The multiply is a single imul
    *  and beats the three shift/or pairs GCC would otherwise emit. */
    pattern = byte * 0x01010101u;

    head = (size_t)((0u - (uint32_t)dp) & 3u);
    count -= head;
    words = count >> 2;
    tail  = count & 3u;

    /* eax is an input only, and both "+D"/"+c" are hard register constraints,
    *  so nothing can be allocated over it. The head and tail runs want the
    *  byte in al, the middle one the full pattern in eax; the two are handed
    *  in separately rather than shifted about inside the asm. */
    __asm__ __volatile__("cld\n\trep stosb"
        : "+D"(dp), "+c"(head)
        : "a"(byte) : "memory", "cc");
    __asm__ __volatile__("rep stosl"
        : "+D"(dp), "+c"(words)
        : "a"(pattern) : "memory", "cc");
    __asm__ __volatile__("rep stosb"
        : "+D"(dp), "+c"(tail)
        : "a"(byte) : "memory", "cc");

    return dest;
}

unsigned short *memsetw(unsigned short *dest, unsigned short val, size_t count)
{
    unsigned short *temp = (unsigned short *)dest;
    for( ; count != 0; count--) *temp++ = val;
    return dest;
}

unsigned char inportb (unsigned short _port)
{
    unsigned char rv;
    __asm__ __volatile__ ("inb %1, %0" : "=a" (rv) : "dN" (_port));
    return rv;
}

void outportw(unsigned short port, unsigned short value)
{
	__asm__ __volatile__ ("outw %%ax,%%dx": :"dN"(port), "a"(value));
} 

void outportb (unsigned short _port, unsigned char _data)
{
    __asm__ __volatile__ ("outb %1, %0" : : "dN" (_port), "a" (_data));
}

void reboot() 
{ 
	printf("Killing all Processes ..\n");
	taskmgr_killall();
	sleep(500);
	
	printf("Rebooting ..");
	sleep(3000);

    unsigned int temp;
    do { 
       temp = inportb( 0x64 ); 
       if( temp & 1 ) inportb( 0x60 ); 
    }
	while ( temp & 2 ); 
 
    outportb(0x64, 0xFE); 
}


int checkCPUID(void)
{
	unsigned long eax, ecx;
   __asm__ __volatile__("pushf; pop %0; mov %0, %1; xor $0x200000, %0; push %0; popf; pushf; pop %0" : "=a" (eax), "=c" (ecx) : : "cc");
   if(eax == ecx)
   {
      return 0; //Not Supported
   } else {
      return 1; //Supported
   }
}

/* Prints the usable regions of the multiboot memory map. Deliberately compact:
*  text mode only has 25 lines, and the boot messages have to fit into them.
*  Reserved regions are of no interest here, pmm_init() evaluates those.
*
*  mbi is already a virtual pointer (kernel() converted it). Its mmap_addr
*  field, however, is still a raw PHYSICAL address from the bootloader and
*  gets its own P2V() below - once, into mmap, not per entry.
*/
static void print_memory_map(multiboot_info *mbi)
{
	multiboot_mmap_entry *entry;
	uint8_t *mmap;
	uint32_t offset;
	uint32_t len_kib;
	uint32_t total_kib;
	int shown;

	if(!(mbi->flags & MULTIBOOT_INFO_MEM_MAP))
	{
		printf("Memory map: none received from the bootloader\n");
		return;
	}

	/* The map itself has to lie in the directly mapped window, otherwise
	*  P2V() would produce an address that is not backed by anything. */
	if(mbi->mmap_addr == 0 || mbi->mmap_addr > DIRECT_MAP_LIMIT)
	{
		printf("Memory map: address 0x%X out of the direct mapping\n",
			mbi->mmap_addr);
		return;
	}

	mmap = (uint8_t *)P2V(mbi->mmap_addr);
	total_kib = 0;
	shown = 0;
	offset = 0;

	printf("Memory map (usable):\n");
	while(offset < mbi->mmap_length)
	{
		entry = (multiboot_mmap_entry *)(mmap + offset);
		/* size only counts from the field after it, hence the extra 4 */
		offset += entry->size + 4;

		if(entry->type != MULTIBOOT_MEMORY_AVAILABLE) continue;
		/* A 32-bit kernel cannot reach anything above 4 GiB anyway */
		if(entry->addr_high != 0) continue;

		if(entry->len_high != 0)
			len_kib = (0xFFFFFFFFu - entry->addr_low) / 1024u;
		else
			len_kib = entry->len_low / 1024u;

		total_kib += len_kib;

		if(shown < 6)
			printf("  0x%X  %i KiB  type %i\n", entry->addr_low, len_kib, entry->type);
		else if(shown == 6)
			printf("  ...\n");
		shown++;
	}

	printf("  Total: %i KiB (%i MiB) in %i regions\n",
		total_kib, (total_kib / 1024u), shown);
}

/* --- The kernel command line ----------------------------------------------
*
*  The string a Multiboot loader passes to the kernel itself, as opposed to
*  the per-module command lines exec.c reads. QEMU supplies it with -append,
*  GRUB with whatever follows the file name on the "multiboot" line. Our own
*  stage 2 supplies none at all: boot/stage2.asm never sets MB_FLAG_CMDLINE,
*  so on the bootdisk every option below is simply absent, which is exactly
*  the behaviour the bootdisk needs.
*
*  Exactly one option exists so far, "text" -- see cmdline_text_mode().
*
*  The reading follows what exec.c's module_name_from_cmdline() already does,
*  and for the same reasons: the address is PHYSICAL, it may be zero, it may
*  point outside the direct mapping, and the string may not be terminated at
*  all. All three are answered by refusing to read rather than by faulting,
*  and the length is bounded against the end of the direct mapping rather
*  than trusted.
*/
#define CMDLINE_MAX 256

/* Non-zero if opt appears in the command line as a word of its own.
*
*  Whole words, not a substring search, and that is not pedantry. QEMU's
*  Multiboot loader prepends the kernel's file name to whatever -append says,
*  so the string this sees is "kernel.elf text" and not "text"; GRUB does the
*  same with the path from the "multiboot" line. A substring match would fire
*  on any file name that happens to contain the option -- booting a kernel
*  called "textmode.elf" would silently disable the framebuffer. Whitespace
*  on both sides is the only rule, so a path can say anything it likes as
*  long as it is not the bare word. */
static int cmdline_has(multiboot_info *mbi, const char *opt)
{
	const char *cmd;
	uint32_t room;
	int limit;
	int i;
	int j;
	int start;

	if(mbi == 0) return 0;
	if(!(mbi->flags & MULTIBOOT_INFO_CMDLINE)) return 0;
	if(mbi->cmdline == 0 || mbi->cmdline > DIRECT_MAP_LIMIT) return 0;

	room  = DIRECT_MAP_LIMIT - mbi->cmdline + 1;
	limit = CMDLINE_MAX;
	if(room < (uint32_t)limit) limit = (int)room;

	cmd = (const char *)P2V(mbi->cmdline);

	i = 0;
	while(i < limit && cmd[i] != '\0')
	{
		/* Skip the separators, then take everything up to the next one
		*  as one word and compare it in full. */
		if(cmd[i] == ' ' || cmd[i] == '\t')
		{
			i++;
			continue;
		}

		start = i;
		while(i < limit && cmd[i] != '\0' && cmd[i] != ' ' && cmd[i] != '\t')
			i++;

		for(j = 0; opt[j] != '\0'; j++)
			if(start + j >= i || cmd[start + j] != opt[j]) break;

		/* Both ended together: same length, same characters. */
		if(opt[j] == '\0' && start + j == i) return 1;
	}

	return 0;
}

/* "text" on the kernel command line: use the VGA text console and ignore any
*  framebuffer the loader reports.
*
*  Why this exists at all. The Multiboot header in start.asm now asks for
*  1024x768x32, so a loader that honours it hands us a graphics mode and the
*  framebuffer console takes the screen. That is what we want by default and
*  it is also a one-way door: from protected mode there is no int 0x10 and
*  therefore no way to ask for a different mode, so a kernel that only ever
*  used what it was given could never be told to stay in text mode again. A
*  request is a preference and not a guarantee, but the fallback it produces
*  is an accident of the loader -- "it happened not to work" -- and that is
*  not the same thing as being able to choose.
*
*  What it does and what it does NOT do, because the difference matters and
*  is easy to get wrong. This makes the KERNEL behave as though no
*  framebuffer had been reported: the console stays on 0xB8000, fb_usable()
*  says no, and "gfx" finds no surface. It does not, and cannot, put the
*  DISPLAY back into a text mode -- that needs int 0x10, the loader is the
*  last code that could still call it, and by the time this runs the loader
*  is gone.
*
*  Which is why the option is refused when it cannot be carried out. If the
*  loader reports a framebuffer whose type is not EGA text, then the screen
*  IS a graphics mode, and a kernel that dutifully moved its console to
*  0xB8000 would be writing into memory nothing is displaying: a black
*  screen, no boot messages, no shell, and no way to find out why. That was
*  measured rather than assumed -- GRUB honours the video request in the
*  header even with "set gfxpayload=text" in front of the multiboot line, and
*  the result is exactly that black screen. Going blind on request is not
*  serving the request, so in that case the framebuffer console keeps the
*  screen and framebuffer_init() prints one line saying the option was seen,
*  what stopped it, and where to ask instead.
*
*  So, per boot path:
*
*    - "make run" (QEMU -kernel): QEMU's Multiboot loader does not implement
*      the video request at all -- it prints "multiboot knows VBE. we don't"
*      on stderr and reports no framebuffer -- so the machine is in text mode
*      either way, the option is honoured, and it is what makes the kernel's
*      behaviour deliberate rather than incidental. This is the case it is
*      for, and the one the Makefile wires up with -append.
*    - GRUB: sets the mode, so the option is refused and reported. Text mode
*      there is the loader's to give: build without MULTIBOOT_VIDEO_MODE, or
*      boot the kernel some other way.
*    - The bootdisk: stage 2 passes no command line at all (it never sets
*      MB_FLAG_CMDLINE), so none of this can be reached and nothing changes.
*
*  The spelling is the bare word "text". It reads as what it means on the
*  command line one actually types -- -append "text" -- and it is the word
*  Linux has used for the same thing for thirty years, which is worth more
*  than a more precise "novideo" or "nofb" that has to be looked up. */
static int cmdline_text_mode(multiboot_info *mbi)
{
	return cmdline_has(mbi, "text");
}

/* --- The framebuffer the bootloader handed over ---------------------------
*
*  A graphics mode cannot be established from protected mode: a VBE mode needs
*  int 0x10 and that is real mode only. Whoever boots us therefore sets the
*  mode and describes the result in the multiboot info, and all this kernel
*  can do is read what it got. GRUB reports the plain EGA text buffer here
*  when no video mode was requested; our own stage 2 reports whatever VBE mode
*  it selected before leaving real mode.
*
*  The description is kept in file statics with the accessors below rather
*  than in a struct in a header, because no header may be touched in this
*  step. What this really wants to be is one "struct framebuffer" and a
*  "const struct framebuffer *fb_info(void)" in a src/include/framebuffer.h -
*  seven accessors for what is one immutable record are six too many, and a
*  framebuffer console would have to call all of them to draw a single pixel.
*  Until that header exists, a user declares what it needs:
*
*      extern uint32_t fb_base(void);          physical base address
*      extern uint32_t fb_pitch_bytes(void);   bytes per row
*      extern uint32_t fb_pixel_width(void);
*      extern uint32_t fb_pixel_height(void);
*      extern uint32_t fb_bits_per_pixel(void);
*      extern uint32_t fb_kind(void);          MULTIBOOT_FRAMEBUFFER_*
*      extern int      fb_usable(void);        safe to write to right now
*/
static uint32_t fb_addr;	/* physical base, 0 = nothing was reported   */
static uint32_t fb_pitch;	/* bytes per row - NOT width * bpp / 8       */
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_bpp;
static uint32_t fb_type = MULTIBOOT_FRAMEBUFFER_EGA_TEXT;
static int fb_reachable;	/* P2V() can name it at all                  */
static int fb_mapped;		/* and the page is actually present          */

/* "text" was on the kernel command line, and what came of it. Read once, in
*  framebuffer_describe(), and remembered rather than parsed again in
*  framebuffer_init(): the two must not be able to disagree, and the second of
*  them runs several hundred lines and one page directory later.
*
*  Two flags because the answer has three states, not two: not asked for,
*  asked for and done, asked for and impossible. See cmdline_text_mode(). */
static int fb_text_forced;	/* honoured: the console stays on 0xB8000    */
static int fb_text_refused;	/* asked for, but the display is graphics    */

uint32_t fb_base(void)           { return fb_addr; }
uint32_t fb_pitch_bytes(void)    { return fb_pitch; }
uint32_t fb_pixel_width(void)    { return fb_width; }
uint32_t fb_pixel_height(void)   { return fb_height; }
uint32_t fb_bits_per_pixel(void) { return fb_bpp; }
uint32_t fb_kind(void)           { return fb_type; }

/* The only question a caller that wants to draw should ask: a framebuffer
*  that is a text buffer, that P2V() cannot name, or whose pages are not
*  present is not something to write into. */
int fb_usable(void)
{
	return fb_addr != 0
		&& fb_type != MULTIBOOT_FRAMEBUFFER_EGA_TEXT
		&& fb_reachable
		&& fb_mapped;
}

/* Hands the mode over to the framebuffer console, before a single character
*  has been printed. This is the first thing kernel() does with the multiboot
*  info, and the position is the point of it.
*
*  fbcon_describe() touches nothing but a shadow buffer in .bss. It needs no
*  mapping, no vmm and no heap, so the only thing keeping it from being the
*  very first statement of the kernel is that mbi has to be a validated and
*  converted pointer first - which is why the two multiboot checks moved
*  ahead of the banner rather than this call moving behind them.
*
*  What is at stake is everything printed before it. On a machine that came
*  up in a graphics mode there is no text buffer behind 0xB8000 that anybody
*  looks at: characters written there are simply gone, and they are not in
*  the shadow buffer either, so fbcon_activate() cannot replay them later.
*  Every line that would tell one why a boot went wrong is exactly the kind
*  of line one loses that way.
*
*  init_video() is behind this call for the same reason. Its cls() is console
*  output like any other - scrn.c asks fbcon_active() which screen to clear -
*  so it only clears the right one once the console knows what it drives. Run
*  the other way round, cls() would blank a text buffer nobody displays and
*  leave the shadow buffer full of the NUL bytes .bss starts out with.
*
*  A framebuffer above 4 GiB is passed on as address zero rather than as its
*  truncated low half: a 32-bit kernel cannot map it at all, and half an
*  address is worse than none. The console then stays in text mode, which is
*  what the no-framebuffer case below does as well - both are documented as
*  safe by fbcon.h. */
static void framebuffer_describe(multiboot_info *mbi)
{
	uint32_t addr;

	/* Read here and nowhere else, and read before the framebuffer flag is
	*  even looked at: this is the first line of the kernel that consults
	*  the command line, and everything that reacts to it is below. */
	fb_text_forced = cmdline_text_mode(mbi);

	/* The refusal, and it is decided here rather than in framebuffer_init()
	*  because here is where it takes effect: this is the call that hands
	*  the console its screen, and by the time framebuffer_init() runs the
	*  console has already been painted.
	*
	*  The test is the reported TYPE and nothing else. A type other than EGA
	*  text is the loader saying it put the display into a graphics mode,
	*  which is precisely the situation in which 0xB8000 shows nobody
	*  anything. Whether the kernel can go on to map that framebuffer, and
	*  whether the console then succeeds in taking it over, are later and
	*  separate questions - fbcon_activate() answers them and falls back to
	*  text mode on its own if it cannot. That fallback is blind on such a
	*  machine, but it is the pre-existing behaviour for a framebuffer the
	*  kernel cannot use and is not made worse by anything here. */
	if(fb_text_forced
	   && (mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER)
	   && mbi->framebuffer_type != MULTIBOOT_FRAMEBUFFER_EGA_TEXT)
	{
		fb_text_refused = 1;
		fb_text_forced  = 0;
	}

	/* "text" and "no framebuffer reported" are described to the console in
	*  exactly the same way, on purpose. The console has one text mode, not
	*  two, and a second path into it would be a second thing to keep
	*  correct for no gain. Whatever the loader put in the framebuffer
	*  fields is dropped on the floor here and is never recorded, so
	*  everything downstream -- fb_usable(), "gfx", the shell -- sees a
	*  machine that has no graphics surface, which is what was asked for. */
	if(fb_text_forced || !(mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER))
	{
		fbcon_describe(0, 0, 0, 0, 0, MULTIBOOT_FRAMEBUFFER_EGA_TEXT);
		return;
	}

	addr = (mbi->framebuffer_addr_high == 0) ? mbi->framebuffer_addr_low : 0;

	fbcon_describe(addr, mbi->framebuffer_pitch,
		mbi->framebuffer_width, mbi->framebuffer_height,
		(uint8_t)mbi->framebuffer_bpp, (uint8_t)mbi->framebuffer_type);
}

/* Records what the bootloader reported and says so in one line.
*
*  Needs a live vmm: the second of the two checks below asks vmm_is_mapped(),
*  which has nothing to answer with before vmm_init() has built the tables.
*  Hence its place in kernel() and not next to print_memory_map(). */
static void framebuffer_init(multiboot_info *mbi)
{
	const char *kind;
	const char *status;

	/* Asked for by hand, so it is reported by hand: the line says the mode
	*  was chosen rather than merely absent, and it names the option that
	*  chose it, which is the one thing somebody staring at an unexpected
	*  80x25 screen needs to know. Nothing is recorded in fb_* -- see
	*  framebuffer_describe(). */
	if(fb_text_forced)
	{
		printf("Framebuffer: disabled by \"text\" on the command line\n");
		return;
	}

	if(!(mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER))
	{
		/* GRUB Legacy never sets the bit, and a Multiboot loader that
		*  does not implement the video request of our header leaves it
		*  out as well -- QEMU's own -kernel loader is exactly that case
		*  and says so on stderr. That is not a failure: it means the
		*  machine is still in the VGA text mode scrn.c drives. */
		printf("Framebuffer: none reported, VGA text mode\n");
		return;
	}

	fb_type   = mbi->framebuffer_type;
	fb_width  = mbi->framebuffer_width;
	fb_height = mbi->framebuffer_height;
	fb_bpp    = mbi->framebuffer_bpp;
	fb_pitch  = mbi->framebuffer_pitch;
	fb_addr   = mbi->framebuffer_addr_low;

	switch(fb_type)
	{
		case MULTIBOOT_FRAMEBUFFER_INDEXED:  kind = "indexed";  break;
		case MULTIBOOT_FRAMEBUFFER_RGB:      kind = "RGB";      break;
		case MULTIBOOT_FRAMEBUFFER_EGA_TEXT: kind = "EGA text"; break;
		default:                             kind = "unknown";  break;
	}

	/* Two questions, independent of each other, and neither may be assumed.
	*
	*  1. Can P2V() name it? The direct mapping covers physical memory below
	*     DIRECT_MAP_LIMIT (1 GiB) and nothing else, so a 64-bit address -
	*     framebuffer_addr_high non-zero - or a low half beyond that limit
	*     has no virtual alias at all. There is no address to hand out then,
	*     and none is invented: reaching such a framebuffer means giving it a
	*     mapping of its own with vmm_map(), which is a follow-up step. */
	fb_reachable = (mbi->framebuffer_addr_high == 0
			&& fb_addr != 0
			&& fb_addr <= DIRECT_MAP_LIMIT);

	/*  2. Is it mapped at all? vmm_init() maps RAM as the memory map reports
	*     it, but a framebuffer is memory mapped hardware and need not appear
	*     in that map - a card answering at 0xFD000000 typically does not, so
	*     its pages are simply absent. Writing there would page fault, which
	*     is why this is checked and reported rather than assumed. Only worth
	*     asking where P2V() applies in the first place. */
	fb_mapped = fb_reachable ? vmm_is_mapped((uint32_t)P2V(fb_addr)) : 0;

	if(mbi->framebuffer_addr_high != 0)
		status = "above 4 GiB, unreachable";
	else if(fb_addr == 0)
		status = "no address reported";
	else if(!fb_reachable)
		status = "outside the direct mapping";
	else if(!fb_mapped)
		status = "not mapped";
	else
		status = "mapped";

	/* One line, assembled in three calls because the 64-bit case cannot use
	*  the same conversion: %X prints an unpadded 32-bit value, so gluing two
	*  of them together would silently misrepresent the address. */
	printf("Framebuffer: %s %ix%i %i bpp, pitch %i, at ",
		kind, fb_width, fb_height, fb_bpp, fb_pitch);

	if(mbi->framebuffer_addr_high != 0)
		printf("0x%X:0x%X", mbi->framebuffer_addr_high, fb_addr);
	else
		printf("0x%X", fb_addr);

	/* The last field is the one thing here that is not a property of the
	*  hardware but a decision: fbcon_activate() ran a few lines further up
	*  and either took the screen over or did not. Worth saying in the same
	*  line, because the two halves explain each other - "outside the direct
	*  mapping, console active" is not a contradiction, it is the console
	*  having mapped what P2V() cannot name. Two words rather than the
	*  geometry, so the EGA text case stays inside 80 columns; the shell
	*  reports the full mode with "gfx -i". */
	printf(" (%s, %s)\n", status,
		fbcon_active() ? "console active" : "text mode");

	/* Only ever printed when somebody actually typed the option, so the
	*  boot output of every normal run is one line shorter than this
	*  function looks. It costs a row precisely where a row is worth it:
	*  the alternative to this line is a user who asked for text mode,
	*  did not get it, and has nothing on screen explaining that. */
	if(fb_text_refused)
		printf("  \"text\" ignored: the loader set this mode, ask it instead\n");
}

/* Reports the modules the bootloader loaded alongside the kernel, in exactly
*  one line - the boot output has to fit into 25 rows, and the names are the
*  only thing one cannot look up again later with the shell.
*
*  Without modules nothing is printed at all: a kernel booted without a
*  single program is the normal case while there is no filesystem, and a
*  line saying "none" would cost a row for saying nothing. The shell reports
*  an empty list itself when someone actually asks for one.
*
*  The names are cut off once the line gets close to the 80 column width,
*  because a wrapped line costs a second row - exactly what we are avoiding. */
static void print_modules(void)
{
	int count;
	int i;
	int len;
	int used;
	const char *name;

	count = exec_module_count();
	if(count <= 0) return;

	printf("Modules: %i (", count);

	used = 0;
	for(i = 0; i < count; i++)
	{
		name = exec_module_name(i);
		if(name == 0) name = "?";
		len = (int)strlen(name);

		/* The first name is always printed, however long it is - a line
		*  that only says "(...)" would be worse than a long one. */
		if(i > 0 && used + 2 + len > 48)
		{
			printf(", ...");
			break;
		}

		if(i > 0)
		{
			printf(", ");
			used += 2;
		}

		printf("%s", name);
		used += len;
	}

	printf(")\n");
}

/* At most max characters of s. printf() knows no precision, and both strings
*  that end up on the disk lines below come from somewhere else - a model
*  string out of the drive's IDENTIFY data, an error text out of the
*  filesystem. A line that runs past 80 columns wraps and costs a second row,
*  which is exactly what the whole boot output is trying not to do. */
static void print_capped(const char *s, int max)
{
	int i;

	if(s == 0) return;

	for(i = 0; i < max && s[i] != '\0'; i++) putch((unsigned char)s[i]);
}

/* Brings the disk up and mounts what is on it, in at most two lines: one for
*  the drive, one for the filesystem.
*
*  Which drive: the first one that carries something mountable, tried from 0
*  upwards. Drive 0 is where an image handed to qemu as -hda ends up, so it is
*  the answer in practically every case here - but insisting on it would be
*  wrong on real hardware, where drive 0 can just as well be an empty slot or
*  the CD-ROM the machine booted from while the data sits on the slave. Trying
*  the others costs one IDENTIFY per drive, all of them already done by
*  ata_init(), and a boot sector read for each drive that is actually there.
*  The first mount that succeeds ends the search, so a machine with two
*  filesystems gets the lower drive - deterministic, and changeable from the
*  shell later.
*
*  No disk at all is a normal case, not a failure: "make run" without an image
*  boots exactly like this, and so does hardware with nothing attached. It
*  prints nothing at all then, for the same reason print_modules() says
*  nothing about an empty module list - a row spent on "no disk" is a row
*  taken away from something that has news. Whatever the outcome, this returns
*  and the shell comes up: ata_init() is documented as safe with no controller
*  present, and a drive that answers nothing simply is not present(). */
static void disk_init(void)
{
	uint32_t sectors;
	int drive;
	int first;		/* first drive that exists at all        */
	int mounted;		/* first one that holds a filesystem     */

	ata_init();

	/* The block layer, immediately after the driver whose devices it claims.
	*  blk_init() takes numbers 0..3 for ATA whether or not a drive answered on
	*  each -- claiming them is what stops a USB stick later taking a number a
	*  hard disk would have had, which would make "mount 1" mean different
	*  things on two boots of one machine.
	*
	*  The filesystem talks only to this now, so nothing below this line may be
	*  skipped: without it fat_mount() finds device 0 unregistered and reports
	*  no filesystem on a machine that has one. */
	blk_init();

	first   = -1;
	mounted = -1;

	for(drive = 0; drive < ATA_MAX_DRIVES; drive++)
	{
		if(!ata_present(drive)) continue;

		if(first < 0) first = drive;

		if(fat_mount(drive) == 0)
		{
			mounted = drive;
			break;
		}
	}

	if(first < 0) return;

	drive   = (mounted >= 0) ? mounted : first;
	sectors = ata_sectors(drive);

	/* 512 byte sectors, so 2048 of them make a MiB. The size is printed
	*  rather than the sector count, which says nothing at a glance - and in
	*  KiB below a megabyte, because a 1.44 MiB floppy image is a perfectly
	*  normal FAT12 medium here and "0 MiB" would be a useless line. */
	printf("Disk: hd%i, ", drive);
	if(sectors >= 2048u)
		printf("%i MiB, ", (int)(sectors / 2048u));
	else
		printf("%i KiB, ", (int)(sectors / 2u));
	print_capped(ata_model(drive), 40);
	printf("\n");

	if(mounted < 0)
	{
		/* One line, and it names the reason: a disk that is there but
		*  holds nothing this kernel understands is the case one wants
		*  explained, unlike no disk at all. */
		printf("Filesystem: none (");
		print_capped(fat_last_error(), 46);
		printf(")\n");
		return;
	}

	printf("Filesystem: %s on hd%i, %i KiB of %i KiB free\n",
		fat_type(), drive,
		(int)fat_free_kib(),
		(int)fat_total_kib());
}

/* Tries to mount whatever removable block device turned up after disk_init()
*  had already run -- a USB stick, today.
*
*  Split from disk_init() rather than folded into it because the two happen at
*  different times for a real reason: ATA is there from the moment its driver
*  probes, while a stick needs four layers that only exist once the PCI bus has
*  been walked. Trying both from one place would mean either mounting nothing
*  until the network stack is up, or looking for a device that cannot be there
*  yet.
*
*  Silent when there is nothing, like disk_init(): a machine with no stick is
*  the ordinary case and a row saying so is a row taken from something that has
*  news. */
static void removable_init(void)
{
	int dev;

	for(dev = BLK_REMOVABLE_FIRST; dev < BLK_MAX_DEVICES; dev++)
	{
		if(!blk_present(dev))
			continue;

		if(fat_mount(dev) != 0)
			continue;

		printf("Filesystem: %s on %s (%s), %i KiB of %i KiB free\n",
			fat_type(), blk_describe(dev), blk_bus(dev),
			(int)fat_free_kib(),
			(int)fat_total_kib());
		return;
	}
}

/* ---------------------------------------------------------------------------
*  network_init() - PCI bus, network card, protocol stack
*
*  Three layers, and the order between them is not a matter of taste: pci_init()
*  enumerates the bus, rtl8139_init() looks up its card in what the enumeration
*  found, and net_init() has a MAC address to work with only once the card
*  answered. Nothing here probes hardware blindly; without the PCI scan the
*  kernel has no idea a network card exists at all.
*
*  Placed after disk_init() and after sti, and that placement is the point:
*  the card delivers received frames through an interrupt, so it is only from
*  here on that anything can arrive. Bringing it up earlier would work too -
*  irq_install_handler() just fills a table entry - but the frames would pile
*  up in the receive ring with nobody reading it, for no gain.
*
*  No card is the normal case: "make run NET=0", and every machine that has
*  none. rtl8139_init() then reports nothing found, net_init() marks the stack
*  down, and the shell comes up with ifconfig able to say why rather than
*  showing zeros. Nothing below may keep the boot from finishing.
*
*  The address is not configured here. QEMU's user network has no DHCP client
*  on our side, so it has to be set by hand from the shell:
*
*      ifconfig 10.0.2.15 255.255.255.0 10.0.2.2
*      ping 10.0.2.2
*
*  Hardcoding those would be wrong the moment the kernel meets a real network,
*  and a DHCP client is a protocol of its own - deliberately not part of this
*  step. */
static void network_init(void)
{
	pci_init();

	/* USB before the network, and that order is the interesting part: both
	*  ask the same PCI enumeration what is on the bus, but QEMU puts its UHCI
	*  controller on IRQ 11 -- the line the RTL8139 also lands on. The UHCI
	*  driver polls rather than installing a handler there, precisely so that
	*  it cannot unhook the card; see the reasoning in src/drivers/usb/uhci.c. Bringing it
	*  up first means a machine where that ever changed would fail loudly here
	*  rather than quietly losing the network later.
	*
	*  No controller is the ordinary outcome, on QEMU without "-device
	*  piix3-usb-uhci" and on any machine built in the last decade, which has
	*  xHCI and nothing else. usb_init() says what it found in one line and
	*  returns either way. */
	usb_init();

	/* The class driver, after the core has enumerated -- it looks through the
	*  device table the core filled in rather than at the bus. It creates its
	*  polling task and starts it, which is safe here for a reason that used to
	*  be this module's problem and is now nobody's: taskmgr_task_start() holds
	*  a start made on the boot path until taskmgr_boot_complete() at the end of
	*  this file. See the block above boot_handed_over in tasks.c for what that
	*  is protecting against. */
	usbhid_init();

	/* Mass storage, after HID and after blk_init() -- it registers block
	*  devices, so the layer that owns the numbers has to exist first. */
	usbmsc_init();

	/* Every network driver in the tree gets a look at the bus, and the first
	*  one whose card is present and comes up takes the stack -- see the note
	*  about one card at a time in netdev.h. Each of these reports what it
	*  found and returns either way; a machine with neither card is an
	*  ordinary machine that has no network.
	*
	*  The order decides only which card wins on a machine that has both, and
	*  the loser shuts its hardware back down rather than leaving a DMA engine
	*  running. e1000 first because it is the faster part and the one the
	*  machines this was written for actually have. */
	e1000_init();
	rtl8139_init();

	/* Asks whether anything registered. It no longer probes for a card
	*  itself: a stack that has to enumerate the cards it might be running on
	*  has been decoupled from one card rather than from the hardware. */
	net_init();

	/* The resolver binds its UDP port here rather than on first use: binding
	*  is the only thing about it that can fail for a reason worth reporting,
	*  and a lookup that has to bind first would discover that halfway into a
	*  command. It asks nobody anything until dns_resolve() is called, and it
	*  does not need the address yet -- the DNS server's address comes from
	*  the DHCP lease, which is obtained later and from the shell. */
	dns_init();

	/* TCP allocates nothing here either: a connection's buffers come from the
	*  heap when it opens, so an idle machine pays for the table of control
	*  blocks and nothing else. */
	tcp_init();
}

/* Entry point from start.asm. mbi_phys is the pointer the bootloader left in
*  ebx: a PHYSICAL address. The kernel runs in the higher half, so that value
*  is not a usable pointer - it is converted once, right here, and everything
*  below (print_memory_map(), pmm_init()) sees only the virtual mbi. */
int kernel(uint32_t magic, multiboot_info *mbi_phys)
{
	extern void main();
	multiboot_info *mbi;
	int task_console;

	/* The two multiboot checks come first, ahead of the banner, and that is
	*  a consequence of the framebuffer console rather than a tidy-up.
	*
	*  Nothing may be printed before framebuffer_describe() has run, or it is
	*  lost on a graphics mode boot (see the comment there). The description
	*  can only be read out of mbi, and mbi is only a pointer worth
	*  dereferencing once these two checks have passed - so they have to
	*  happen before any output, not after the first three lines of it.
	*
	*  That leaves exactly one thing that still goes to text mode alone: the
	*  failure messages of these two checks. It is not a hole that can be
	*  closed. Without a usable multiboot info there is no geometry to
	*  describe, so there is no framebuffer console to say them on either,
	*  and on a machine that booted into a graphics mode the panic screen is
	*  invisible. The condition it reports - a bootloader that did not leave
	*  a multiboot info behind - is not one a graphics mode makes any more
	*  likely, and our own stage 2, which is what sets a VBE mode in the
	*  first place, is precisely the code that gets both of these right.
	*
	*  Each branch brings the text console up itself. init_video() cannot be
	*  hoisted above them for that: it clears the screen, and cls() asks
	*  fbcon_active() which screen it is clearing.
	*
	*  Without the magic we do not know whether ebx points at a multiboot
	*  info structure at all. Everything beyond this would be guesswork.
	*  Checked before the conversion: P2V() on garbage yields garbage. */
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC)
	{
		init_video();
		printf("Multiboot magic: expected %X, got %X\n",
			MULTIBOOT_BOOTLOADER_MAGIC, magic);
		panic("No multiboot compliant bootloader.\nTomatOS needs the memory information from the bootloader.");
		return 0;
	}

	/* The magic only vouches for the register, not for the pointer. P2V()
	*  is defined for physical addresses inside the direct mapping alone, so
	*  anything above DIRECT_MAP_LIMIT (or a null pointer) cannot be turned
	*  into something dereferenceable - better to say so than to fault in
	*  print_memory_map() with no handler installed yet. */
	if((uint32_t)mbi_phys == 0 || (uint32_t)mbi_phys > DIRECT_MAP_LIMIT)
	{
		init_video();
		printf("Multiboot info at 0x%X, outside the direct mapping\n",
			(uint32_t)mbi_phys);
		panic("Multiboot info pointer out of range.\nTomatOS cannot reach the memory information.");
		return 0;
	}

	mbi = (multiboot_info *)P2V(mbi_phys);

	/* The earliest point the console can be told what it drives, and
	*  therefore the point it is told. Everything below this is recorded in
	*  the shadow buffer and survives to be painted; everything above it
	*  would not. */
	framebuffer_describe(mbi);

	/* Now the screen may be cleared - and on a graphics boot this is the
	*  clear that counts. There is no 80x25 buffer on such a machine, so
	*  cls() blanks the shadow buffer instead of 0xB8000, and the pixels
	*  follow when fbcon_activate() paints it. The leading blank lines of the
	*  banner are for the text case, where they separate our output from the
	*  bootloader's; a framebuffer console has 48 rows and does not miss the
	*  two. */
	init_video();
	printf("\n\nTomatOS/x86 boot v0.2\n");

	/* One line on the split, because it is exactly what one wants to see
	*  first when a higher half mapping goes wrong. */
	printf("Higher half: kernel 0x%X virt = 0x%X phys\n",
		KERNEL_VIRTUAL_START, V2P(KERNEL_VIRTUAL_START));

	detect_cpu();
	if(checkCPUID()==1)
	   {
		printf("CPU Supported\n");
	   } else {
		printf("CPU Not Supported\n");
		panic("An unsupported CPU ID has been detected\nTomatOS requires a i386 or above");
		return 0;
	   }
	/* Set up memory management. The size now comes from the bootloader's
	*  memory map - no more destructive test writes across the address
	*  space.
	*  Order: pmm_init() only needs the memory map, vmm_init() takes its page
	*  tables from the ready pmm, heap_init() then grows into an already
	*  mapped range, and all three must come before mt_install() so that
	*  tasks can request memory as well.
	*
	*  Address spaces make that last point stricter rather than adding a step.
	*  From mt_install() on, a task may own a page directory of its own, and
	*  it shares only the upper quarter - everything from KERNEL_VIRTUAL_BASE
	*  up - with the kernel space vmm_init() built. Sharing works at
	*  directory-entry granularity (see vmm.h), so a kernel mapping that needs
	*  a brand new page table would end up in the directory that happens to be
	*  active and nowhere else. Every kernel page table must therefore exist
	*  before the first task space is created:
	*    - vmm_init() maps all usable RAM up front, so the kernel half is
	*      complete the moment it returns. Nothing has to be inserted between
	*      it and mt_install().
	*    - heap_init(), and every later heap growth, only takes frames out of
	*      that already mapped window via P2V() and never calls vmm_map(). It
	*      adds no page table, which is why the heap stays visible in every
	*      task space even though it keeps growing long after mt_install().
	*  Anything added later that maps a fresh kernel range belongs here, in
	*  front of mt_install(), for exactly this reason. vmm_create_space()
	*  itself needs nothing but a ready pmm and the direct mapping, both of
	*  which are in place well before the first task exists.
	*
	*  The programs hang off the end of that same chain. exec_init() reads the
	*  module list out of the multiboot info, so it needs the converted mbi
	*  just like pmm_init() does, and it reads the module command lines
	*  through P2V() - which means the direct mapping vmm_init() built has to
	*  be live. It only writes down addresses, names and sizes; the ELF
	*  parsing, the frames and the address space all come later, in
	*  exec_spawn(), on demand from the shell. Hence its place: behind
	*  heap_init(), where memory management is complete and nothing moves
	*  around any more, and in front of mt_install(), because from the console
	*  task onwards someone may ask for the list at any time. It adds no
	*  kernel mapping of its own, so the rule above about page tables and
	*  task spaces does not concern it.
	*
	*  exec_init() records where the modules are and does not copy their
	*  contents, which is safe because pmm_init() has already locked them:
	*  region_mark_used() is called for the descriptor array at mods_addr,
	*  then once per module for mod_start..mod_end and once for its command
	*  line. Without that the frames would sit inside a region the memory map
	*  reports as available, the allocator would hand them out, and the first
	*  heap growth or task page directory would overwrite a program long
	*  before anybody tried to load it - a failure that looks like a broken
	*  ELF header rather than like what it is. That was a real hole once and
	*  this comment described it; it is closed, and the code that closes it is
	*  in pmm_init() rather than here, because recording an address here would
	*  not have made the memory behind it any safer.
	*
	*  The disk hangs off the very end of the boot, behind the "sti", and it
	*  is the one step that is not placed by what it needs from memory
	*  management. Its lower bound is the same as everywhere here: ata_init()
	*  and fat_mount() may allocate - the filesystem takes a buffer for the
	*  FAT out of the heap - so heap_init() has to be done, and it is, long
	*  before. The upper bound is the one that decides it. The driver polls,
	*  so it needs no interrupt of its own and none of the IRQ machinery is a
	*  precondition. But a PIO driver that waits for a drive to become ready
	*  may well wait with sleep(), and sleep() is timer_wait(), which spins on
	*  timer_ticks until the timer IRQ moves it on. Before pic_install() the
	*  PIT is not programmed and no handler is installed; before the "sti" the
	*  interrupt cannot arrive at all, and timer_wait() then notices that
	*  interrupts are off and drops its hlt - but the loop itself has no way
	*  out. One sleep() in a driver placed further up would therefore not be
	*  slow, it would be the end of the boot, on precisely the machines whose
	*  drives are slow to answer. Behind the "sti" every mechanism a polling
	*  driver might reach for actually works, and the price is only that the
	*  two disk lines appear below "Protected Mode Kernel Running" instead of
	*  in the middle of the driver block.
	*
	*  Still in front of the console task, though, and that part is not
	*  cosmetic: the shell may ask fat_readdir() something with its first
	*  command, and by the time it can be typed the mount is either done or
	*  known to have failed. Nothing here needs a task or a scheduler - it
	*  runs on the boot stack like everything else in this function.
	*
	*  The kernel is linked for 0xC0100000 but loaded at 0x00100000, and by
	*  the time this function runs it already executes from the higher half:
	*  start.asm puts up a provisional mapping and jumps up there before
	*  calling us. Two consequences run through the whole boot path:
	*    - Every address that comes from outside the kernel is PHYSICAL and
	*      has to pass P2V() before it is dereferenced. That is the multiboot
	*      pointer above, the mmap_addr inside it, and later everything
	*      pmm_alloc_frame() hands out. The other direction, V2P(), is for
	*      what the MMU reads: page directory and table entries.
	*    - The two views differ by the constant KERNEL_VIRTUAL_BASE, and only
	*      for physical memory below DIRECT_MAP_LIMIT. Outside that window
	*      the macros mean nothing, hence the range checks.
	*
	*  Paging is reloaded here, before idt_install()/isrs_install(), i.e.
	*  without a page fault handler. That is deliberate:
	*    - The IDT gates reference a GDT selector, so pulling idt_install()
	*      forward would mean pulling gdt_install() forward as well and
	*      rearranging the whole driver block for no real gain.
	*    - vmm_init() builds the real directory over the same higher half
	*      window start.asm already provides and maps all usable RAM into it,
	*      so every pointer keeps its value across the write to CR3. There is
	*      no access in this window that could legitimately fault.
	*    - Should it fault anyway, it happens on the very first instruction
	*      after the new directory is loaded: a reproducible triple fault
	*      right after this message, not a subtle bug. A handler would not
	*      help either, because a broken mapping would just as likely swallow
	*      the handler itself.
	*  The page fault handler is for the faults that come later - null
	*  pointer dereferences, writes into the read-only kernel text - and
	*  those all happen well after isrs_install().
	*
	*  The framebuffer console is split across this chain in two places, and
	*  neither of them is negotiable.
	*
	*  framebuffer_describe() is already done, far above, before the first
	*  character - see the comment on it. What is left here is
	*  fbcon_activate(), which maps the framebuffer and paints the shadow
	*  buffer onto it, and it is bounded from both sides.
	*
	*  Its lower bound is vmm_init(). The address the bootloader reports is
	*  physical and belongs to memory mapped hardware, not to RAM: QEMU's
	*  card answers at 0xFD000000, which is above DIRECT_MAP_LIMIT, so P2V()
	*  has no name for it, and it appears in no memory map, so vmm_init()
	*  never mapped it. It has to be given a mapping of its own with
	*  vmm_map_mmio(), and there is nothing to map with before vmm_init() has
	*  built the tables. Hence the call one line behind it, which is the
	*  earliest instruction at which it can succeed at all.
	*
	*  Its upper bound is mt_install(), and that is the rule the block above
	*  states, hitting a real case for the first time. The kernel half is
	*  shared between address spaces at page DIRECTORY ENTRY granularity, so
	*  a kernel mapping that needs a brand new page table lands only in the
	*  directory that was active when it was made. The framebuffer window is
	*  exactly such a mapping - it is nowhere near the direct mapping and no
	*  existing kernel table covers it. Made after the first task space
	*  exists, it would be present in whichever directory happened to be
	*  loaded and absent from every other, and the console would page fault
	*  the moment the scheduler switched into a task with a space of its own.
	*  Doing it here means every directory vmm_create_space() makes later
	*  copies the entry along with the rest of the kernel half.
	*
	*  vmm.h now also reserves the page tables for that window inside
	*  vmm_init() for the same reason, which makes the upper bound belt and
	*  braces rather than the only thing standing between us and the fault.
	*  The call stays here regardless: relying on a reservation elsewhere to
	*  make a late mapping safe is how the rule gets forgotten the next time
	*  something wants a kernel range of its own.
	*
	*  Painting happens here as well, which is why the boot output is on the
	*  screen from this line onwards rather than from the shell. Everything
	*  above - the memory map, the higher half line, even the banner - was
	*  written into the shadow buffer and is drawn in one go.
	*
	*  framebuffer_init() then reports, and it reports afterwards on purpose:
	*  its last field says whether the console took the framebuffer over, and
	*  that is only knowable once fbcon_activate() has returned. Its own two
	*  checks are unchanged and still answer a different question - whether
	*  the direct mapping reaches the framebuffer, which is what fb_usable()
	*  is about - and vmm_is_mapped() still needs the tables vmm_init() built.
	*/
	print_memory_map(mbi);
	pmm_init(mbi);
	vmm_init();
	fbcon_activate();
	/* No extra line for the address space: the directory this prints IS the
	*  kernel space, so naming it here costs nothing, while a second message
	*  would cost one of the 25 rows the boot output has to fit into. The
	*  table count already includes whatever fbcon_activate() needed for the
	*  framebuffer window. */
	printf("Paging on: %i page tables, kernel space 0x%X\n",
		vmm_table_count(), vmm_kernel_space());
	framebuffer_init(mbi);
	heap_init();
	printf("%i MB Memory (%i KB) in %i Frames\n",
		(pmm_total_bytes() / (1024u * 1024u)),
		(pmm_total_bytes() / 1024u),
		pmm_total_frames());
	exec_init(mbi);
	print_modules();

	printf("\n\nLoading TomatOS/x86\n");
	printf("Loading Driver Components.\n");
    gdt_install();
    idt_install();
    isrs_install();
    irq_install();
	/* After idt_install(), which clears the table: syscall_install() adds
	*  vector 0x80 as the one gate ring 3 may reach. gdt_install() has
	*  already loaded the TSS the CPU needs to find a kernel stack when a
	*  ring 3 task traps into the kernel. */
	syscall_install();
	mt_install();
    pic_install();
    keyboard_install();

	/* The mouse hangs off the same 8042 controller as the keyboard, so it goes
	*  in right after it -- and after it on purpose, not before: bringing the
	*  auxiliary port up means reading and rewriting the controller's shared
	*  configuration byte, and mouse_init() masks IRQ 1 across that because the
	*  controller answers a config read by raising the KEYBOARD interrupt.
	*  Doing this while the keyboard handler is already installed is what makes
	*  that masking meaningful; doing it before would leave a byte in the buffer
	*  with nobody to blame.
	*
	*  No mouse is an ordinary outcome, not a failure -- a machine may have
	*  none, and QEMU can be told to leave the controller out entirely. It
	*  reports through its return value rather than printing, so nothing is said
	*  here either: "mouse" shows what was found, and a line at boot saying a
	*  mouse exists would be a row spent on news nobody asked for. */
	mouse_init();
	printf("Initializing Serial Port\n");
	init_serial();
	printf("Enabling A20 Gate\n");
	//EnableA20Gate();
	printf("Protected Mode Kernel Running.\n\n");

	getchn(); // flush last keyboard character set by EnableA20Gate();
	__asm__ __volatile__ ("sti");

	/* Disk and filesystem: last, because this is the first point at which a
	*  driver may wait for hardware with sleep(). See the ordering comment
	*  above. Says nothing when there is no drive. */
	disk_init();

	/* Network last: it is the only driver whose work starts by itself, from
	*  an interrupt, and there is nothing above it that waits on the result. */
	network_init();

	/* And then the disks again, because USB came up in there.
	*
	*  disk_init() ran before it and could only see the ATA drives; a stick is
	*  reached through the PCI bus, a host controller, the USB core and a SCSI
	*  transport, none of which existed at that point. Rather than move the
	*  whole of network_init() ahead of the filesystem -- which would put the
	*  network stack before the disk it may want to read from -- the mount is
	*  simply tried again for whatever appeared meanwhile.
	*
	*  Only if nothing is mounted yet. A machine with a hard disk keeps it: a
	*  stick that silently took over the filesystem because it was plugged in
	*  at boot would be a surprise, and the ATA drives are the ones the boot
	*  chain itself came from. */
	if(!fat_mounted())
		removable_init();

	task_console = taskmgr_add_task( main, "CONSOLE", TASK_PRIORITY_REALTIME );
	taskmgr_task_start(task_console);

	/* Nothing this function created has been runnable until now -- see the
	*  block above boot_handed_over in tasks.c. This is the line that hands
	*  the machine over, and it is the last one on purpose: after it the very
	*  next tick may elect somebody, and everything above has to be done.
	*
	*  The console is released by this call like anything else. It is not
	*  started separately, because a boot path that had to start one task by
	*  hand and let the others be released would be two rules where there is
	*  one. */
	taskmgr_boot_complete();

	/* From here on the timer IRQ takes over: the scheduler jumps into the
	 * console task. We return to start.asm, which waits there in an
	 * endless loop for the first task switch. */
	return 0;
}
