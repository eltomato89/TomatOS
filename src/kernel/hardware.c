/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Interrupt Service Routines installer and exceptions
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by http://brynet.biz.tm - <brynet@gmail.com>
*/


#include <system.h>
#include <string.h>
#include <stdio.h>
/* Required Declarations */
int do_intel(void);
int do_amd(void);
void printregs(int eax, int ebx, int ecx, int edx);

#define cpuid(in, a, b, c, d) __asm__("cpuid": "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (in));
#define PORT 0x3f8   /* COM1 */

/* Simply call this function detect_cpu(); */
int detect_cpu(void) { /* or main() if your trying to port this as an independant application */
	unsigned long ebx, unused;
	cpuid(0, unused, ebx, unused, unused);
	switch(ebx) {
		case 0x756e6547: /* Intel Magic Code */
		do_intel();
		break;
		case 0x68747541: /* AMD Magic Code */
		do_amd();
		break;
		default:
		printf("Unknown x86 CPU Detected\n");
		break;
	}
	return 0;
}

/* Intel Specific brand list */
char *Intel[] = {
	"Brand ID Not Supported.", 
	"Intel(R) Celeron(R) processor", 
	"Intel(R) Pentium(R) III processor", 
	"Intel(R) Pentium(R) III Xeon(R) processor", 
	"Intel(R) Pentium(R) III processor", 
	"Reserved", 
	"Mobile Intel(R) Pentium(R) III processor-M", 
	"Mobile Intel(R) Celeron(R) processor", 
	"Intel(R) Pentium(R) 4 processor", 
	"Intel(R) Pentium(R) 4 processor", 
	"Intel(R) Celeron(R) processor", 
	"Intel(R) Xeon(R) Processor", 
	"Intel(R) Xeon(R) processor MP", 
	"Reserved", 
	"Mobile Intel(R) Pentium(R) 4 processor-M", 
	"Mobile Intel(R) Pentium(R) Celeron(R) processor", 
	"Reserved", 
	"Mobile Genuine Intel(R) processor", 
	"Intel(R) Celeron(R) M processor", 
	"Mobile Intel(R) Celeron(R) processor", 
	"Intel(R) Celeron(R) processor", 
	"Mobile Geniune Intel(R) processor", 
	"Intel(R) Pentium(R) M processor", 
	"Mobile Intel(R) Celeron(R) processor"
};

/* This table is for those brand strings that have two values depending on the processor signature. It should have the same number of entries as the above table. */
char *Intel_Other[] = {
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Intel(R) Celeron(R) processor", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Intel(R) Xeon(R) processor MP", 
	"Reserved", 
	"Reserved", 
	"Intel(R) Xeon(R) processor", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved", 
	"Reserved"
};

/* Intel-specific information */
int do_intel(void) {
	printf("Intel Specific Features:\n");
	unsigned long eax, ebx, ecx, edx, max_eax, signature, unused;
	int model, family, type, brand, stepping, reserved;
	int extended_family = -1;
	cpuid(1, eax, ebx, unused, unused);
	model = (eax >> 4) & 0xf;
	family = (eax >> 8) & 0xf;
	type = (eax >> 12) & 0x3;
	brand = ebx & 0xff;
	stepping = eax & 0xf;
	reserved = eax >> 14;
	signature = eax;
	printf("Type %d - ", type);
	switch(type) {
		case 0:
		printf("Original OEM");
		break;
		case 1:
		printf("Overdrive");
		break;
		case 2:
		printf("Dual-capable");
		break;
		case 3:
		printf("Reserved");
		break;
	}
	printf("\n");
	printf("Family %d - ", family);
	switch(family) {
		case 3:
		printf("i386");
		break;
		case 4:
		printf("i486");
		break;
		case 5:
		printf("Pentium");
		break;
		case 6:
		printf("Pentium Pro");
		break;
		case 15:
		printf("Pentium 4");
	}
	printf("\n");
	if(family == 15) {
		extended_family = (eax >> 20) & 0xff;
		printf("Extended family %d\n", extended_family);
	}
	printf("Model %d - ", model);
	switch(family) {
		case 3:
		break;
		case 4:
		switch(model) {
			case 0:
			case 1:
			printf("DX");
			break;
			case 2:
			printf("SX");
			break;
			case 3:
			printf("487/DX2");
			break;
			case 4:
			printf("SL");
			break;
			case 5:
			printf("SX2");
			break;
			case 7:
			printf("Write-back enhanced DX2");
			break;
			case 8:
			printf("DX4");
			break;
		}
		break;
		case 5:
		switch(model) {
			case 1:
			printf("60/66");
			break;
			case 2:
			printf("75-200");
			break;
			case 3:
			printf("for 486 system");
			break;
			case 4:
			printf("MMX");
			break;
		}
		break;
		case 6:
		switch(model) {
			case 1:
			printf("Pentium Pro");
			break;
			case 3:
			printf("Pentium II Model 3");
			break;
			case 5:
			printf("Pentium II Model 5/Xeon/Celeron");
			break;
			case 6:
			printf("Celeron");
			break;
			case 7:
			printf("Pentium III/Pentium III Xeon - external L2 cache");
			break;
			case 8:
			printf("Pentium III/Pentium III Xeon - internal L2 cache");
			break;
		}
		break;
		case 15:
		break;
	}
	printf("\n");
	cpuid(0x80000000, max_eax, unused, unused, unused);

	if(max_eax >= 0x80000004) {
		printf("Brand: ");
		if(max_eax >= 0x80000002) {
			cpuid(0x80000002, eax, ebx, ecx, edx);
			printregs(eax, ebx, ecx, edx);
		}
		if(max_eax >= 0x80000003) {
			cpuid(0x80000003, eax, ebx, ecx, edx);
			printregs(eax, ebx, ecx, edx);
		}
		if(max_eax >= 0x80000004) {
			cpuid(0x80000004, eax, ebx, ecx, edx);
			printregs(eax, ebx, ecx, edx);
		}
		printf("\n");
	} else if(brand > 0) {
		printf("Brand %d - ", brand);
		if(brand < 0x18) {
			if(signature == 0x000006B1 || signature == 0x00000F13) {
				printf("%s\n", Intel_Other[brand]);
			} else {
				printf("%s\n", Intel[brand]);
			}
		} else {
			printf("Reserved\n");
		}
	}
	printf("Stepping: %d Reserved: %d\n", stepping, reserved);
	return 0;
}

/* Print Registers */
void printregs(int eax, int ebx, int ecx, int edx) {
	int j;
	char string[17];
	string[16] = '\0';
	for(j = 0; j < 4; j++) {
		string[j] = eax >> (8 * j);
		string[j + 4] = ebx >> (8 * j);
		string[j + 8] = ecx >> (8 * j);
		string[j + 12] = edx >> (8 * j);
	}
	printf("%s", string);
}

/* AMD-specific information */
int do_amd(void) {
	printf("AMD Specific Features:\n");
	unsigned long extended, eax, ebx, ecx, edx, unused;
	int family, model, stepping, reserved;
	cpuid(1, eax, unused, unused, unused);
	model = (eax >> 4) & 0xf;
	family = (eax >> 8) & 0xf;
	stepping = eax & 0xf;
	reserved = eax >> 12;
	printf("Family: %d Model: %d [", family, model);
	switch(family) {
		case 4:
		printf("486 Model %d", model);
		break;
		case 5:
		switch(model) {
			case 0:
			case 1:
			case 2:
			case 3:
			case 6:
			case 7:
			printf("K6 Model %d", model);
			break;
			case 8:
			printf("K6-2 Model 8");
			break;
			case 9:
			printf("K6-III Model 9");
			break;
			default:
			printf("K5/K6 Model %d", model);
			break;
		}
		break;
		case 6:
		switch(model) {
			case 1:
			case 2:
			case 4:
			printf("Athlon Model %d", model);
			break;
			case 3:
			printf("Duron Model 3");
			break;
			case 6:
			printf("Athlon MP/Mobile Athlon Model 6");
			break;
			case 7:
			printf("Mobile Duron Model 7");
			break;
			default:
			printf("Duron/Athlon Model %d", model);
			break;
		}
		break;
	}
	printf("]\n");
	cpuid(0x80000000, extended, unused, unused, unused);
	if(extended == 0) {
		return 0;
	}
	if(extended >= 0x80000002) {
		unsigned int j;
		printf("Detected Processor Name: ");
		for(j = 0x80000002; j <= 0x80000004; j++) {
			cpuid(j, eax, ebx, ecx, edx);
			printregs(eax, ebx, ecx, edx);
		}
		printf("\n");
	}
	if(extended >= 0x80000007) {
		cpuid(0x80000007, unused, unused, unused, edx);
		if(edx & 1) {
			printf("Temperature Sensing Diode Detected!\n");
		}
	}
	printf("Stepping: %d Reserved: %d\n", stepping, reserved);
	return 0;
}


/* getRamSize() is gone. The function determined the amount of memory by
 * destructive test writes across the address space - it was the cause of a
 * boot hang and overwrote foreign regions while doing so. The memory is now
 * reported by the bootloader's memory map, see pmm_init() in src/mm/pmm.c. */

/* --- COM1 -----------------------------------------------------------------
*
*  The serial port is the kernel's second console. Everything putch() puts on
*  the screen is also written out here, character for character, by
*  serial_console_putc() at the bottom of this section - which is what makes a
*  headless boot, a machine whose panel stays dark, and an automated test that
*  reads the boot messages back all possible at once. Nothing about that works
*  if writing a byte can block forever, so most of what follows is about the
*  two ways it could: a port that is not there at all, and a port that is
*  there but never reports its transmitter free again.
*
*  What the port is doing right now. This is the whole of the state:
*
*    -1  not looked at yet. The next character to be written probes for a
*        UART and programs it.
*     0  there is nothing here to write to, or there was and it stopped
*        answering. Every write from now on is a no-op and costs one compare.
*     1  probed, programmed, usable.
*
*  It starts unprobed rather than being brought up by init_serial() alone,
*  because the first thing the kernel prints happens long before init_serial()
*  is reached: the banner starts at kernel.c:1105 and init_serial() is called
*  at kernel.c:1332, some two hundred lines of driver setup later. Those early
*  lines are exactly the ones worth having on a wire - they are the ones that
*  say what the machine looks like and they are the ones a boot that dies
*  half way through never gets past. Probing on first use rather than on a
*  call from the boot path means the log starts with the first character the
*  kernel ever prints, whenever that is.
*
*  Only ever changed from putch(), which holds the console lock, so no two
*  writers can race for it. A torn update would be harmless anyway: the worst
*  it could do is program the UART twice. */
static int serial_state = -1;

/* How long we are prepared to wait for the transmitter holding register to
*  report itself empty, counted in reads of the line status register rather
*  than in microseconds.
*
*  It has to be a spin count and not a time, because this runs from putch(),
*  which runs with the interrupts masked and is called from interrupt context.
*  There is no timer to ask and no sleep to take: the only clock available
*  here is "how many bus cycles have gone by".
*
*  One inportb of an I/O port is a bus cycle, on the order of a microsecond on
*  real hardware, so 20000 of them is roughly 20 milliseconds. One character
*  at 115200 baud takes 87 microseconds to shift out, and the wait we expect
*  is never longer than the character currently in the shift register plus the
*  one in the holding register - some 175 microseconds. The bound is therefore
*  a hundred times the longest wait that can legitimately happen, which is the
*  point: it must never fire in normal operation, and it must always fire
*  before the machine looks hung.
*
*  A timeout is not treated as a hiccup to be retried. There is no flow
*  control on this line - the UART shifts bits out whether or not anything is
*  listening - so a transmitter that does not free up is broken hardware, not
*  a slow reader. The port is marked dead and never polled again, which is
*  what keeps the worst case at ONE bounded wait for the whole run rather than
*  one per character: 20 milliseconds once, instead of 20 milliseconds times
*  the couple of thousand characters in the boot banner. */
#define SERIAL_TX_SPINS 20000

/* Is there a UART at 0x3F8?
*
*  A ThinkPad T430 has no serial port; QEMU always does. On a machine without
*  one the outportb goes nowhere, which is harmless, but the inportb reads a
*  floating bus and answers 0xFF forever (0x00 on some chipsets) - and a
*  status register that permanently reads "transmitter busy" is precisely the
*  hang this whole section exists to avoid. So the question has to be settled
*  before the first character, not discovered by waiting for one.
*
*  The scratch register at PORT+7 answers it. It is a byte of storage in the
*  chip that has no effect on anything, present on every 16450 and later,
*  which is every PC serial port since the AT. Two complementary patterns are
*  written and read back: a bus with nothing on it reads all ones or all
*  zeroes and cannot return both. Nothing is transmitted, so the probe costs
*  four bus cycles and cannot itself wait for anything.
*
*  The loopback test through the modem control register is the other common
*  way to ask this. It is not used here because it puts a byte through the
*  shift register, which takes a character time to come back - a wait, in the
*  one function that must not have one. */
static int serial_probe(void)
{
	if(inportb(PORT + 5) == 0xFF)
		return 0;

	outportb(PORT + 7, 0x5A);
	if(inportb(PORT + 7) != 0x5A)
		return 0;

	outportb(PORT + 7, 0xA5);
	if(inportb(PORT + 7) != 0xA5)
		return 0;

	return 1;
}

/* 115200 baud, 8N1: divisor 1, the fastest the chip's 1.8432 MHz clock
*  divides down to and the rate every terminal program and every "-serial"
*  backend defaults to.
*
*  The speed is not cosmetic. The console mirror waits for the transmitter
*  before every character, so on real hardware the boot is no faster than the
*  wire. The banner is 1261 bytes at the prompt, which is 12610 bits with the
*  start and stop bit each byte carries: 1.3 seconds at the 9600 baud this
*  port used to be set to, and 0.11 seconds at 115200. A second and a bit
*  added to every boot is the difference between a mirror that can be left on
*  and one that has to be argued about. */
static void serial_program(void)
{
	outportb(PORT + 1, 0x00);    /* no interrupts: this driver polls        */
	outportb(PORT + 3, 0x80);    /* DLAB on, the divisor is behind it       */
	outportb(PORT + 0, 0x01);    /* divisor 1 (lo) = 115200 baud            */
	outportb(PORT + 1, 0x00);    /*             (hi)                        */
	outportb(PORT + 3, 0x03);    /* DLAB off again, 8 bits, no parity, 1 stop */
	outportb(PORT + 2, 0xC7);    /* FIFOs on and cleared, 14 byte trigger   */
	outportb(PORT + 4, 0x0B);    /* DTR, RTS, OUT2                          */
}

/* Probe and program, once. Everything that writes calls this first, so the
*  port is ready whoever gets there first - the boot banner through putch() or
*  the explicit call from kernel.c. */
static int serial_ready(void)
{
	if(serial_state < 0)
	{
		if(serial_probe())
		{
			serial_program();
			serial_state = 1;
		}
		else
		{
			serial_state = 0;
		}
	}

	return serial_state;
}

/* Called from the boot path at kernel.c:1332. By then the console mirror has
*  almost certainly brought the port up already and this does nothing, which
*  is deliberate rather than wasteful: reprogramming a UART means setting DLAB
*  and rewriting the divisor, and doing that while the FIFO still holds the
*  tail of the banner would garble the characters still on their way out. */
void init_serial() {
	serial_ready();
}

int serial_received() {
   return inportb(PORT + 5) & 1;
}

/* Blocks until a character arrives, which is the right thing for a reader and
*  the reason there is no bound here - "no input yet" is not a fault. The
*  presence check is still needed: on a machine with no port the status
*  register reads 0xFF, bit 0 included, and this would hand back a stream of
*  garbage forever. Nothing in the kernel calls it today. */
char read_serial() {
   if(!serial_ready()) return 0;

   while (serial_received() == 0);

   return inportb(PORT);
}

int is_transmit_empty() {
   return inportb(PORT + 5) & 0x20;
}

/* One raw byte, exactly as given: no newline translation, no filtering. The
*  callers above this decide what a byte should look like; this one only gets
*  it onto the wire, or gives up trying.
*
*  See SERIAL_TX_SPINS for why the wait is bounded and why running out of it
*  switches the port off for good rather than trying again next time. */
void write_serial(char a) {
   int spins;

   if(!serial_ready()) return;

   for(spins = SERIAL_TX_SPINS; spins > 0; spins--)
   {
      if(is_transmit_empty()) {
         outportb(PORT, a);
         return;
      }
   }

   serial_state = 0;
}

/* One character of console output.
*
*  This is what putch() calls, and it is called for every character that
*  reaches the screen from anywhere - the boot banner, the shell, a ring 3
*  program through SYS_WRITE - because putch() is the single place all of them
*  meet. See the call graph in src/video/scrn.c.
*
*  Two things are done to the byte on the way, and both are about the log
*  being readable as text afterwards rather than about fidelity to the screen:
*
*  A newline becomes CR LF. The screen console treats a bare '\n' as "margin
*  and one line down"; a terminal and a file both want the pair, and a log
*  full of lone line feeds staircases across the screen of whoever reads it.
*  This is the ONLY place the translation happens - write_serial() above is
*  deliberately raw, so a character cannot pick up a second carriage return by
*  passing through two layers that both mean well.
*
*  Anything outside printable ASCII is reduced. The screen console is CP437:
*  printf() translates the UTF-8 umlauts into 0x84, 0x94 and friends, and
*  those bytes are not valid UTF-8. A single one of them in the log makes grep
*  call the whole file binary and answer "Binary file matches" instead of the
*  line - which would defeat the point of having the log at all. The seven
*  umlauts are spelled out again on the way past, which is exactly the
*  translation printf() did on the way in, run backwards: "Jens Koehler" reads
*  better in a log than "Jens K?hler" and costs a table of seven entries. Any
*  other high byte - a box drawing character, say - becomes a question mark,
*  because there is no honest ASCII for it. The control characters below a
*  space that putch() itself ignores are dropped here for the same reason it
*  ignores them: they put nothing on the screen, so they have no business in a
*  mirror of it. Backspace, tab and carriage return are passed through,
*  because they mean on a terminal what they mean on the screen. */
void serial_console_putc(unsigned char c)
{
	if(c == '\n')
	{
		write_serial(13);
		write_serial(10);
		return;
	}

	if(c == '\b' || c == '\t' || c == '\r' || (c >= ' ' && c < 0x7F))
	{
		write_serial((char)c);
		return;
	}

	if(c < 0x80)
		return;

	switch(c)
	{
		case 0x8E: write_serial('A'); write_serial('E'); return;  /* AE */
		case 0x84: write_serial('a'); write_serial('e'); return;  /* ae */
		case 0x99: write_serial('O'); write_serial('E'); return;  /* OE */
		case 0x94: write_serial('o'); write_serial('e'); return;  /* oe */
		case 0x9A: write_serial('U'); write_serial('E'); return;  /* UE */
		case 0x81: write_serial('u'); write_serial('e'); return;  /* ue */
		case 0xE1: write_serial('s'); write_serial('s'); return;  /* sz */
		default:   write_serial('?'); return;
	}
}

/* A whole string. Walks the caller's string rather than copying it: the old
*  version copied into a 128 byte buffer on the stack with strcpy() first, so
*  a string longer than that wrote over its own return address. Its loop
*  condition was "i <= strlen(s) - 1", and strlen() returns a size_t: for an
*  empty string that is not -1 but the largest unsigned there is, and the
*  comparison promotes i to match, so the loop walked off the end of the
*  buffer instead of not running at all. Nothing in the kernel calls this,
*  which is the only reason neither ever went off. */
void writes_serial(char *str)
{
	while(*str != '\0')
	{
		serial_console_putc((unsigned char)*str);
		str++;
	}
}

void audio_switch_on()
{
	outportb(0x61,inportb(0x61) | 3);
}

void audio_switch_off()
{
	outportb(0x61,inportb(0x61) &~3);
}

void audio_sound(unsigned frequency)
{
	unsigned divisor;
	divisor = 1193180L/frequency;
	outportb(0x43,0xB6);
	outportb(0x42,divisor&0xFF);
	outportb(0x42,divisor >> 8);
	
}

void audio_beep(unsigned frequency, int duration)
{
	audio_switch_on();
	audio_sound(frequency);
	sleep(duration);
	audio_switch_off();
}

/* floppy_detect() and floppy_getDriveType() are gone. They read CMOS byte
 * 0x10 and turned the two nibbles into drive descriptions, and nothing ever
 * called them: no header declared them, so nothing outside this file could,
 * and nothing inside it did either. They were also the last code here that
 * declared variables in the middle of a block, which this kernel does not do.
 *
 * Nothing was lost with them. The kernel boots from ATA (src/drivers/block/ata.c) and
 * mounts FAT off it; there is no floppy driver for a detection result to
 * feed, and "make run-floppy" boots an image through the BIOS without ever
 * asking this file about it. */
