/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: C Code entry
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (fiesenb@gmail.com)
*/

#include <system.h>
#include <stdio.h>
#include <hardware.h>
#define cpuid(in, a, b, c, d) __asm__("cpuid": "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (in));

void *memcpy(void *dest, const void *src, size_t count)
{
    const char *sp = (const char *)src;
    char *dp = (char *)dest;
    for(; count != 0; count--) *dp++ = *sp++;
    return dest;
}

void *memset(void *dest, char val, size_t count)
{
    char *temp = (char *)dest;
    for( ; count != 0; count--) *temp++ = val;
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

int kernel()
{
    void err_handler(int);
	unsigned long ebx, unused;
    init_video();
    printf("\n\nTomatOS/x86 boot v0.2\n");
	
	detect_cpu();
	if(checkCPUID()==1)
	   {
		printf("CPU Supported\n");
	   } else {
		printf("CPU Not Supported\n");
		panic("An unsupported CPU ID has been detected\nTomatOS requires a i386 or above");
		return 0;
	   }
	printf("%i MB Memory (%i KB)\n", (getRamSize('K')/1024), getRamSize('K'));
	printf("\n\nLoading TomatOS/x86\n");
	printf("Loading Driver Components.\n");
    gdt_install();    
    idt_install();
    isrs_install();
    irq_install();
	mt_install();
    pic_install();
    keyboard_install();
	printf("Initializing Serial Port\n");
	init_serial();
	printf("Enabling A20 Gate\n");
	//EnableA20Gate();
	printf("Protected Mode Kernel Running.\n\n");

	getchn(); // flush last keyboard character set by EnableA20Gate();
	__asm__ __volatile__ ("sti");
    
	extern void main();
	int task_console = taskmgr_add_task( main, "KONSOLE", TASK_PRIORITY_REALTIME );
	taskmgr_task_start(task_console);
}
