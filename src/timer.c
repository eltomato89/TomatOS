/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Timer Driver
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (fiesenb@gmail.com)
*/

#include <system.h>
#include <stdio.h>

#define PIT_FREQ 1193181 // die Standardfrequenz des PIT
#define FREQ 40 //20 // die Frequenz die wir haben wollen
#define BCD2BIN(val) (((val) & 0x0F) + ((val) >> 4) * 10)

/* This will keep track of how many ticks that the system
*  has been running for */
static volatile int timer_ticks=0;
static volatile int seconds=0;
static int frequency = 1000;

extern void irqMT();

void *timer_handlers[16] =
{
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};



/* This installs a custom IRQ handler for the given IRQ */
int timer_install_handler(void (*handler)(struct regs *r))
{
    
	int i = 0;
	for(i=0; i <= 15; i++)
	{
		if(timer_handlers[i] == 0)
		{
			timer_handlers[i] = handler;
			return i;
		}
	}
	return -1;
}

/* This clears the handler for a given IRQ */
int timer_uninstall_handler(void (*handler)(struct regs *r))
{
	int i=0;
	for(i=0; i <= 15; i++)
	{
		if(timer_handlers[i] == handler)
		{
			timer_handlers[i] = 0;
			return i;
		}
	}
	return -1;
}

void timer_notify_handlers(struct regs *r)
{
	int i;
	for(i=0; i <= 15; i++)
	{
		if(timer_handlers[i] != 0)
		{
			void (*handler)(struct regs *r);
			handler = timer_handlers[i];
			if (handler)
			{
				handler(r);
			}
		}	
	}
}

void timer_handler(struct regs *r)
{
   timer_ticks++;
   if (timer_ticks%frequency==0)
   {
      seconds++;
   }
   
   timer_notify_handlers(r);
}

void timer_setInterval(int hz)
{
	frequency = hz;
}

void pic_install()
{
	int counter;
	counter = PIT_FREQ / frequency;
	outportb(0x43, 0x34);
	outportb(0x40, counter & 0xFF);
	outportb(0x40, counter >> 8);
	
	irq_install_handler(0, timer_handler);
	
	
	int i=0;
	for(i=0; i <= 15; i++)
		timer_handlers[i] = 0;
	
}

void sleep(int ticks)
{
	timer_wait(ticks);
}

void timer_wait(int ticks)
{
   unsigned long eticks;

   eticks = timer_ticks + ticks/(1000/frequency);
   while(timer_ticks <= eticks)
   { }
}

int timer_get_ticks()
{
   return timer_ticks*(1000/frequency);
}

datetime cmos_readtime()
{
	datetime now;
	
	__asm__ __volatile__ ("cli"); //disable interrupts

	//get the date and time from the CMOS chip
	outportb(0x70,0x00);//get seconds
	unsigned int seconds = inportb(0x71);
	
	outportb(0x70,0x02);//get minutes
	unsigned int minutes = inportb(0x71);

	outportb(0x70,0x04);//get hours
	unsigned int hours = inportb(0x71);

	outportb(0x70,0x07);//get days (in the month)
	unsigned int days = inportb(0x71);

	outportb(0x70,0x08);//get months
	unsigned int months = inportb(0x71);

	outportb(0x70,0x09);//get years
	unsigned int years = inportb(0x71);
	
	//if in BCD mode convert
	outportb(0x70,0x0B);//the first status port
	char status = inportb(0x71);
	if(!(status & 0x20))//if the clock is in BCD mode
	{
		seconds = BCD2BIN(seconds);
		minutes = BCD2BIN(minutes);
		hours = BCD2BIN(hours);
		days = BCD2BIN(days);
		months = BCD2BIN(months);
		years = BCD2BIN(years);
	}

	years += 2000; //we are in the 21'st century ya' know...
	
	now.seconds = seconds;
	now.minutes = minutes;
	now.hours = hours-1;
	now.days = days;
	now.months = months;
	now.years = years;
	
	__asm__ __volatile__ ("sti"); //enable interrupts
	
	return now;
}
