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
 * reported by the bootloader's memory map, see pmm_init() in src/pmm.c. */

void init_serial() {
   outportb(PORT + 1, 0x00);    // Disable all interrupts
   outportb(PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
   outportb(PORT + 0, 0x0C);    // Set divisor to C (lo byte) 9600 baud
   outportb(PORT + 1, 0x00);    //                  (hi byte)
   outportb(PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
   outportb(PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
   outportb(PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

/*

void init_serial() {
   outportb(PORT + 1, 0x00);    // Disable all interrupts
   outportb(PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
   outportb(PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
   outportb(PORT + 1, 0x00);    //                  (hi byte)
   outportb(PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
   outportb(PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
   outportb(PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}
*/

int serial_received() {
   return inportb(PORT + 5) & 1;
}

char read_serial() {
   while (serial_received() == 0);

   return inportb(PORT);
}

int is_transmit_empty() {
   return inportb(PORT + 5) & 0x20;
}

void write_serial(char a) {
   while (is_transmit_empty() == 0);

   outportb(PORT,a);
}

void writes_serial(char *str) {
	char string[128];
	int i;
	strcpy(string, str);
	
	for(i=0; i <= strlen(string)-1; i++)
	{
		switch(string[i])
		{
			case '\n':	write_serial(13);
						write_serial(10);
						break;
			default:	write_serial(string[i]);
		}
		
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

int floppy_detect(char drive)
{
	unsigned char c;
	outportb(0x70, 0x10);
	c = inportb(0x71);
	int a,b;
	a = c >> 4; // get the high nibble
	b = c & 0xF; // get the low nibble by ANDing out the high nibble
	if(drive == 'a') return a;
	if(drive == 'b') return b;
	
	return 0;
}

char *floppy_getDriveType(int num)
{
	/* Six entries - the array used to be declared as [5], which lost the
	 * last type. The range check catches unknown nibbles. */
	static char *drive_type[6] = { "no floppy drive",
							"360kb 5.25in floppy drive",
							"1.2mb 5.25in floppy drive",
							"720kb 3.5in", "1.44mb 3.5in",
							"2.88mb 3.5in"};
	if(num < 0 || num > 5) return "unknown floppy drive";
	return drive_type[num];
}
