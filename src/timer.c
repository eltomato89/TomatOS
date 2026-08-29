/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Timer Driver
*
*  Notes: No warranty expressed or implied. Use at own risk.
*  Some content is copyright by Brandon F. (fiesenb@gmail.com)
*/

#include <system.h>
#include <stdio.h>

#define PIT_FREQ 1193181 // the standard frequency of the PIT

/* Default timer setting in Hz. Replaces the former, unused
*  "#define FREQ 40" -- there were two competing frequency definitions, but
*  only the variable frequency was ever used. */
#define TIMER_DEFAULT_HZ 1000

/* Upper limits for timer_wait(). Both values are chosen so that the
*  product (milliseconds * Hz) safely fits into a 32 bit long. */
#define TIMER_WAIT_MAX_MS 600000L /* wait at most 10 minutes in one go */
#define TIMER_MAX_HZ      3000L   /* above this the wait time is capped */

/* ------------------------------------------------------------------ */
/* CMOS / RTC                                                          */
/* ------------------------------------------------------------------ */
#define CMOS_ADDR_PORT 0x70
#define CMOS_DATA_PORT 0x71

#define CMOS_REG_SECONDS  0x00
#define CMOS_REG_MINUTES  0x02
#define CMOS_REG_HOURS    0x04
#define CMOS_REG_DAYS     0x07
#define CMOS_REG_MONTHS   0x08
#define CMOS_REG_YEARS    0x09
#define CMOS_REG_STATUS_A 0x0A
#define CMOS_REG_STATUS_B 0x0B

#define CMOS_A_UPDATE_IN_PROGRESS 0x80 /* status register A, bit 7 */
#define CMOS_B_24_HOUR_MODE       0x02 /* status register B, bit 1 */
#define CMOS_B_BINARY_MODE        0x04 /* status register B, bit 2 */
#define CMOS_HOURS_PM_FLAG        0x80 /* only in 12 hour mode */

/* How long we wait at most for an RTC update to finish. An update takes
*  less than 2 ms; the loop is only an emergency brake so that broken or
*  emulated hardware cannot block the kernel permanently. */
#define CMOS_UIP_MAX_SPINS 1000000UL

/* How often the registers are read twice at most, until two consecutive
*  readings are identical. */
#define CMOS_READ_MAX_ATTEMPTS 10

/* The RTC usually runs in UTC, but the status bar is supposed to show local
*  time. 2 corresponds to CEST (Central European Summer Time, UTC+2); for
*  CET (winter time) 1 would have to be entered here. There is deliberately
*  no automatic daylight saving switch.
*
*  Limitation: only the hour is shifted. A day rollover triggered by the
*  offset does NOT carry over into day/month/year -- for a clock in the
*  status bar that would be overkill. */
#define TIMEZONE_OFFSET_HOURS 2

#define BCD2BIN(val) (((val) & 0x0F) + ((val) >> 4) * 10)

/* This will keep track of how many ticks that the system
*  has been running for */
static volatile int timer_ticks=0;
/* Seconds counter since startup. Nobody reads it so far, but the handler
*  keeps it up to date. */
static volatile int seconds=0;
static int frequency = TIMER_DEFAULT_HZ;

extern void irqMT();

/* local helper functions -- not declared in any header */
static unsigned char cmos_read_register(unsigned char reg);
static int cmos_wait_ready(void);
static void cmos_read_raw(datetime *out);
static int cmos_equal(const datetime *a, const datetime *b);

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
   /* frequency == 0 would cause a division by zero here and thus a #DE
   *  right in the middle of the IRQ handler. */
   if (frequency > 0 && timer_ticks%frequency==0)
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
	int i;

	/* Clear the handler table first, then arm the IRQ -- otherwise an early
	*  timer interrupt could run into a table that is about to be
	*  overwritten. */
	for(i=0; i <= 15; i++)
		timer_handlers[i] = 0;

	if(frequency <= 0)
		frequency = TIMER_DEFAULT_HZ;

	counter = PIT_FREQ / frequency;
	/* The divisor is 16 bits wide; on the PIT 0 means 65536. */
	if(counter < 1)
		counter = 1;
	if(counter > 65535)
		counter = 65535;

	outportb(0x43, 0x34);
	outportb(0x40, counter & 0xFF);
	outportb(0x40, (counter >> 8) & 0xFF);

	irq_install_handler(0, timer_handler);
}

/* Sleeps for the given number of milliseconds.
*
*  This used to be one line - "timer_wait(ticks)" - and that line is why a ping
*  took 34 milliseconds to come back. timer_wait() halts until the tick counter
*  has moved far enough, but the task stays TASK_STATE_RUNNING while it does, so
*  the scheduler keeps electing it, and every one of those elections is a full
*  time slice handed to a task that has nothing to do but halt again. A task
*  sleeping fifty milliseconds does not merely wait fifty milliseconds, it
*  spends its whole share of the CPU waiting, and everything else on the machine
*  waits behind it.
*
*  A sleep is a wait like any other; it simply has no channel, only a deadline.
*  That is what task_wait() already does with a null channel - nothing can wake
*  it, task_wake() refuses the null address outright - so the timeout machinery
*  in tasks.c covers this case without a line of its own. The task goes to
*  TASK_STATE_BLOCKED, the scheduler passes it over instead of electing it, and
*  the deadline check in schedule() puts it back to RUNNING on the tick its time
*  is up.
*
*  The loop around it is the same discipline system.h asks of every other
*  caller: task_wait() returning 1 means "something ended your wait", not "your
*  deadline passed", and a task can be taken out of BLOCKED from outside - a
*  suspend and a later start does it. So the remaining time is recomputed
*  against an absolute deadline and the wait is entered again, rather than
*  trusting one call to have slept the whole span. Interrupts are held off
*  around the test for the reason system.h gives: it is the condition test and
*  the block that have to be one indivisible step.
*
*  TWO FALLBACKS keep the old behaviour where the new one cannot work.
*
*    - No task is running. disk_init() sits at the very end of the boot, behind
*      the "sti" but in front of the console task, and a PIO driver waiting for
*      a slow drive gets here with no task to block; so does anything the kernel
*      does before multitasking exists. task_wait() answers such a caller with
*      "timed out" immediately rather than halting the machine, which would turn
*      every driver delay into no delay at all.
*    - Interrupts are off. The caller is inside a critical section it built out
*      of cli, and blocking would both re-enable interrupts underneath it and
*      hand the CPU to a task that may be about to walk into the very data
*      structure it is holding. timer_wait() is what this did before and is at
*      least no worse: it spins, which gets nowhere with the timer masked, but
*      it does not break anybody's invariant. It is also the exact case
*      timer_wait() already documents its no-hlt branch for.
*
*  Both fall through to timer_wait(), which is now reached from nowhere else. */
void sleep(int ms)
{
	unsigned int  deadline;
	unsigned long flags;
	long          capped;
	int           remaining;

	if(ms <= 0)
		return;

	/* Same cap timer_wait() applies, applied here too because the deadline
	*  below is computed before it ever reaches that function. */
	capped = ms;
	if(capped > TIMER_WAIT_MAX_MS)
		capped = TIMER_WAIT_MAX_MS;
	ms = (int) capped;

	if(taskmgr_get_currpid() < 0)
	{
		timer_wait(ms);
		return;
	}

	__asm__ __volatile__ ("pushfl; popl %0" : "=r" (flags) : : "memory");
	if((flags & 0x200) == 0)
	{
		timer_wait(ms);
		return;
	}

	deadline = (unsigned int) timer_get_ticks() + (unsigned int) ms;

	__asm__ __volatile__ ("cli" : : : "memory");

	for(;;)
	{
		/* Unsigned subtraction back into a signed difference: the millisecond
		*  counter wraps, and only the difference of two snapshots stays
		*  meaningful across the wrap. Same reason as the loop in
		*  timer_wait(). */
		remaining = (int) (deadline - (unsigned int) timer_get_ticks());
		if(remaining <= 0)
			break;

		/* 0 means the deadline passed, which is the ordinary way out. */
		if(task_wait(0, remaining) == 0)
			break;
	}

	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

/* Waits for the given number of milliseconds without ever giving up the CPU.
*
*  The primitive behind sleep()'s two fallbacks, and only reached through them:
*  before there is a task to block, and inside a caller that is holding
*  interrupts off. Everything with a task and with interrupts on goes through
*  the wait in sleep() instead and leaves the processor to somebody else. */
void timer_wait(int ticks)
{
	long ms;
	long hz;
	long wait_ticks;
	unsigned int start;
	unsigned long flags;
	int irqs_enabled;

	if(ticks <= 0)
		return;

	ms = ticks;
	if(ms > TIMER_WAIT_MAX_MS)
		ms = TIMER_WAIT_MAX_MS;

	hz = frequency;
	if(hz <= 0)
		hz = TIMER_DEFAULT_HZ;
	if(hz > TIMER_MAX_HZ)
		hz = TIMER_MAX_HZ;

	/* Milliseconds -> timer ticks. Multiply first, then divide: the old
	*  calculation ticks/(1000/frequency) truncated the reciprocal to a whole
	*  number beforehand and even turned into a division by zero for
	*  frequency > 1000. Because ms and hz are capped, the product cannot
	*  overflow. */
	wait_ticks = (ms * hz) / 1000L;
	if(wait_ticks <= 0)
		wait_ticks = 1; /* wait at least one tick */

	/* If interrupts are disabled, there must be no hlt here -- the CPU would
	*  never come out of it again. In that case only the (equally hopeless,
	*  but at least interruptible) busy loop remains. */
	__asm__ __volatile__ ("pushfl; popl %0" : "=r" (flags) : : "memory");
	irqs_enabled = (flags & 0x200) != 0;

	start = (unsigned int)timer_ticks;

	/* Difference comparison in unsigned: timer_ticks overflows eventually,
	*  but the difference between two snapshots stays correct. The old
	*  version compared a volatile int against an unsigned long -- that
	*  comparison flipped on overflow and the loop ran forever. */
	while(((unsigned int)timer_ticks - start) < (unsigned int)wait_ticks)
	{
		/* hlt is allowed here: the timer IRQ keeps running, wakes the CPU on
		*  every tick and also performs the task switch when needed. After a
		*  task switch the task resumes the loop at this point and checks the
		*  condition again. */
		if(irqs_enabled)
			__asm__ __volatile__ ("hlt");
	}
}

/* Uptime since startup in milliseconds. There is (as yet) no prototype for
*  this in the headers -- src/include/time.h instead declares uptime() and
*  get_ticks(), which are defined nowhere. */
int timer_get_ticks()
{
	int hz;
	int t;

	hz = frequency;
	if(hz <= 0)
		hz = TIMER_DEFAULT_HZ;

	t = timer_ticks;
	/* computed in two parts so that t * 1000 does not overflow */
	return (t / hz) * 1000 + ((t % hz) * 1000) / hz;
}

/* ------------------------------------------------------------------ */
/* Reading the RTC                                                     */
/* ------------------------------------------------------------------ */

static unsigned char cmos_read_register(unsigned char reg)
{
	outportb(CMOS_ADDR_PORT, reg);
	return inportb(CMOS_DATA_PORT);
}

/* Waits until the "update in progress" bit in status register A is clear.
*  Returns 1 if the RTC is ready, 0 if the upper limit was reached (broken
*  or emulated hardware). */
static int cmos_wait_ready(void)
{
	unsigned long spins;

	for(spins = 0; spins < CMOS_UIP_MAX_SPINS; spins++)
	{
		if(!(cmos_read_register(CMOS_REG_STATUS_A) & CMOS_A_UPDATE_IN_PROGRESS))
			return 1;
	}
	return 0;
}

/* Reads registers 0x00-0x09 raw (still without BCD conversion and without
*  the time zone), after waiting for a running update to finish. */
static void cmos_read_raw(datetime *out)
{
	cmos_wait_ready();

	out->seconds = cmos_read_register(CMOS_REG_SECONDS);
	out->minutes = cmos_read_register(CMOS_REG_MINUTES);
	out->hours   = cmos_read_register(CMOS_REG_HOURS);
	out->days    = cmos_read_register(CMOS_REG_DAYS);
	out->months  = cmos_read_register(CMOS_REG_MONTHS);
	out->years   = cmos_read_register(CMOS_REG_YEARS);
}

static int cmos_equal(const datetime *a, const datetime *b)
{
	return a->seconds == b->seconds
	    && a->minutes == b->minutes
	    && a->hours   == b->hours
	    && a->days    == b->days
	    && a->months  == b->months
	    && a->years   == b->years;
}

datetime cmos_readtime()
{
	datetime now;
	datetime prev;
	unsigned long flags;
	unsigned char status_b;
	int attempt;
	int is_pm;
	int is_24h;
	int local_hours;

	/* Save EFLAGS and only then disable the interrupts. An unconditional sti
	*  at the end would re-enable the interrupts even when the caller had
	*  deliberately disabled them. */
	__asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");

	/* Read twice and repeat until two consecutive readings are identical.
	*  Together with the update-in-progress check this prevents half updated
	*  values (the classic one: 12:59:59 turns into 12:00:59). Gives up after
	*  CMOS_READ_MAX_ATTEMPTS so that the function never blocks forever. */
	cmos_read_raw(&prev);
	for(attempt = 0; attempt < CMOS_READ_MAX_ATTEMPTS; attempt++)
	{
		cmos_read_raw(&now);
		if(cmos_equal(&now, &prev))
			break;
		prev = now;
	}

	status_b = cmos_read_register(CMOS_REG_STATUS_B);

	__asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");

	/* Status register B, bit 1: 1 = 24 hour mode, 0 = 12 hour mode. In 12
	*  hour mode bit 7 of the hours register marks PM; that bit has to be
	*  masked out before the BCD conversion. */
	is_24h = (status_b & CMOS_B_24_HOUR_MODE) != 0;
	is_pm  = !is_24h && (now.hours & CMOS_HOURS_PM_FLAG) != 0;
	now.hours &= (unsigned int)~CMOS_HOURS_PM_FLAG;

	/* Status register B, bit 2: 1 = binary, 0 = BCD. The old version tested
	*  bit 5 (alarm interrupt enable) and hit the BCD case only by chance,
	*  because that bit is usually 0. */
	if(!(status_b & CMOS_B_BINARY_MODE))
	{
		now.seconds = BCD2BIN(now.seconds);
		now.minutes = BCD2BIN(now.minutes);
		now.hours   = BCD2BIN(now.hours);
		now.days    = BCD2BIN(now.days);
		now.months  = BCD2BIN(now.months);
		now.years   = BCD2BIN(now.years);
	}

	/* Only now, AFTER the BCD conversion, add the century -- the CMOS year
	*  register only supplies two digits. */
	now.years += 2000; //we are in the 21'st century ya' know...

	/* Convert 12 hour mode to 0..23: 12 AM -> 0, 12 PM -> 12. */
	if(!is_24h)
	{
		now.hours = now.hours % 12;
		if(is_pm)
			now.hours += 12;
	}

	/* Time zone. Computed in int so that a negative offset does not
	*  underflow into a huge unsigned value (which is exactly what plain
	*  "hours - 1" did when hours == 0). */
	local_hours = (int)(now.hours % 24) + TIMEZONE_OFFSET_HOURS;
	local_hours = ((local_hours % 24) + 24) % 24;
	now.hours = (unsigned int)local_hours;

	return now;
}
