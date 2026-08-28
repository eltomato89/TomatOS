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

/* Voreinstellung des Timers in Hz. Ersetzt das frühere, unbenutzte
*  "#define FREQ 40" -- es gab zwei konkurrierende Frequenzangaben, benutzt
*  wurde aber immer nur die Variable frequency. */
#define TIMER_DEFAULT_HZ 1000

/* Obergrenzen für timer_wait(). Beide Werte sind so gewählt, dass das
*  Produkt (Millisekunden * Hz) sicher in einen 32-Bit-long passt. */
#define TIMER_WAIT_MAX_MS 600000L /* max. 10 Minuten am Stück warten */
#define TIMER_MAX_HZ      3000L   /* darüber wird die Wartezeit gedeckelt */

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

#define CMOS_A_UPDATE_IN_PROGRESS 0x80 /* Statusregister A, Bit 7 */
#define CMOS_B_24_HOUR_MODE       0x02 /* Statusregister B, Bit 1 */
#define CMOS_B_BINARY_MODE        0x04 /* Statusregister B, Bit 2 */
#define CMOS_HOURS_PM_FLAG        0x80 /* nur im 12-Stunden-Modus */

/* Wie oft maximal auf das Ende eines RTC-Updates gewartet wird. Ein Update
*  dauert unter 2 ms; die Schleife ist nur eine Notbremse, damit kaputte
*  oder emulierte Hardware den Kernel nicht dauerhaft blockiert. */
#define CMOS_UIP_MAX_SPINS 1000000UL

/* Wie oft die Register maximal doppelt gelesen werden, bis zwei
*  aufeinanderfolgende Lesungen identisch sind. */
#define CMOS_READ_MAX_ATTEMPTS 10

/* Die RTC läuft üblicherweise in UTC, die Statusleiste soll aber Ortszeit
*  anzeigen. 2 entspricht MESZ (Mitteleuropäische Sommerzeit, UTC+2), für
*  MEZ (Winterzeit) wäre hier 1 einzutragen. Eine automatische
*  Sommerzeit-Umschaltung gibt es bewusst nicht.
*
*  Einschränkung: verschoben wird ausschließlich die Stunde. Ein durch den
*  Offset ausgelöster Tageswechsel zieht Tag/Monat/Jahr NICHT mit -- für
*  eine Uhr in der Statusleiste wäre das Overkill. */
#define TIMEZONE_OFFSET_HOURS 2

#define BCD2BIN(val) (((val) & 0x0F) + ((val) >> 4) * 10)

/* This will keep track of how many ticks that the system
*  has been running for */
static volatile int timer_ticks=0;
/* Sekundenzähler seit dem Start. Wird bislang von niemandem ausgelesen,
*  aber vom Handler mitgeführt. */
static volatile int seconds=0;
static int frequency = TIMER_DEFAULT_HZ;

extern void irqMT();

/* lokale Hilfsfunktionen -- stehen in keinem Header */
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
   /* frequency == 0 würde hier eine Division durch Null und damit ein #DE
   *  mitten im IRQ-Handler auslösen. */
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

	/* Erst die Handler-Tabelle leeren, dann den IRQ scharf schalten --
	*  sonst könnte ein früher Timer-Interrupt in eine Tabelle laufen,
	*  die gleich darauf überschrieben wird. */
	for(i=0; i <= 15; i++)
		timer_handlers[i] = 0;

	if(frequency <= 0)
		frequency = TIMER_DEFAULT_HZ;

	counter = PIT_FREQ / frequency;
	/* Der Teiler ist 16 Bit breit; 0 bedeutet beim PIT 65536. */
	if(counter < 1)
		counter = 1;
	if(counter > 65535)
		counter = 65535;

	outportb(0x43, 0x34);
	outportb(0x40, counter & 0xFF);
	outportb(0x40, (counter >> 8) & 0xFF);

	irq_install_handler(0, timer_handler);
}

void sleep(int ticks)
{
	timer_wait(ticks);
}

/* Wartet die angegebene Zahl von Millisekunden ab. */
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

	/* Millisekunden -> Timer-Ticks. Erst multiplizieren, dann teilen: die
	*  alte Rechnung ticks/(1000/frequency) rundete den Kehrwert vorher auf
	*  eine ganze Zahl ab und wurde für frequency > 1000 sogar zu einer
	*  Division durch Null. Durch die Deckelung von ms und hz kann das
	*  Produkt nicht überlaufen. */
	wait_ticks = (ms * hz) / 1000L;
	if(wait_ticks <= 0)
		wait_ticks = 1; /* mindestens einen Tick warten */

	/* Wenn die Interrupts gesperrt sind, darf hier kein hlt stehen -- die
	*  CPU käme nie wieder daraus hervor. Dann bleibt nur die (ebenso
	*  aussichtslose, aber wenigstens unterbrechbare) Warteschleife. */
	__asm__ __volatile__ ("pushfl; popl %0" : "=r" (flags) : : "memory");
	irqs_enabled = (flags & 0x200) != 0;

	start = (unsigned int)timer_ticks;

	/* Differenzvergleich in unsigned: timer_ticks läuft irgendwann über,
	*  die Differenz zweier Momentaufnahmen bleibt aber korrekt. Die alte
	*  Fassung verglich einen volatile int gegen einen unsigned long -- der
	*  Vergleich kippte beim Überlauf und die Schleife lief endlos. */
	while(((unsigned int)timer_ticks - start) < (unsigned int)wait_ticks)
	{
		/* hlt ist hier zulässig: der Timer-IRQ läuft weiter, weckt die CPU
		*  bei jedem Tick und führt bei Bedarf auch den Taskwechsel durch.
		*  Nach einem Taskwechsel setzt der Task die Schleife an dieser
		*  Stelle fort und prüft die Bedingung erneut. */
		if(irqs_enabled)
			__asm__ __volatile__ ("hlt");
	}
}

/* Laufzeit seit dem Start in Millisekunden. Es gibt (noch) keinen Prototyp
*  dafür in den Headern -- src/include/time.h deklariert stattdessen
*  uptime() und get_ticks(), die nirgends definiert sind. */
int timer_get_ticks()
{
	int hz;
	int t;

	hz = frequency;
	if(hz <= 0)
		hz = TIMER_DEFAULT_HZ;

	t = timer_ticks;
	/* aufgeteilt gerechnet, damit t * 1000 nicht überläuft */
	return (t / hz) * 1000 + ((t % hz) * 1000) / hz;
}

/* ------------------------------------------------------------------ */
/* RTC auslesen                                                        */
/* ------------------------------------------------------------------ */

static unsigned char cmos_read_register(unsigned char reg)
{
	outportb(CMOS_ADDR_PORT, reg);
	return inportb(CMOS_DATA_PORT);
}

/* Wartet, bis das "Update in progress"-Bit in Statusregister A gelöscht
*  ist. Liefert 1 wenn die RTC bereit ist, 0 wenn die Obergrenze erreicht
*  wurde (kaputte oder emulierte Hardware). */
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

/* Liest die Register 0x00-0x09 roh aus (noch ohne BCD-Umwandlung und ohne
*  Zeitzone), nachdem ein laufendes Update abgewartet wurde. */
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

	/* EFLAGS sichern und erst dann die Interrupts sperren. Ein
	*  bedingungsloses sti am Ende würde die Interrupts auch dann wieder
	*  einschalten, wenn der Aufrufer sie absichtlich gesperrt hatte. */
	__asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");

	/* Zweimal lesen und wiederholen, bis zwei aufeinanderfolgende Lesungen
	*  identisch sind. Zusammen mit dem Update-in-Progress-Check verhindert
	*  das halb aktualisierte Werte (klassisch: aus 12:59:59 wird 12:00:59).
	*  Bricht nach CMOS_READ_MAX_ATTEMPTS ab, damit die Funktion nie
	*  endlos blockiert. */
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

	/* Statusregister B, Bit 1: 1 = 24-Stunden-Modus, 0 = 12-Stunden-Modus.
	*  Im 12-Stunden-Modus markiert Bit 7 der Stundenregister PM; das Bit
	*  muss vor der BCD-Umwandlung ausmaskiert werden. */
	is_24h = (status_b & CMOS_B_24_HOUR_MODE) != 0;
	is_pm  = !is_24h && (now.hours & CMOS_HOURS_PM_FLAG) != 0;
	now.hours &= (unsigned int)~CMOS_HOURS_PM_FLAG;

	/* Statusregister B, Bit 2: 1 = binär, 0 = BCD. Die alte Fassung fragte
	*  Bit 5 (Alarm-Interrupt-Enable) ab und traf den BCD-Fall nur durch
	*  Zufall, weil dieses Bit üblicherweise 0 ist. */
	if(!(status_b & CMOS_B_BINARY_MODE))
	{
		now.seconds = BCD2BIN(now.seconds);
		now.minutes = BCD2BIN(now.minutes);
		now.hours   = BCD2BIN(now.hours);
		now.days    = BCD2BIN(now.days);
		now.months  = BCD2BIN(now.months);
		now.years   = BCD2BIN(now.years);
	}

	/* Erst jetzt, NACH der BCD-Umwandlung, das Jahrhundert aufschlagen --
	*  das CMOS-Jahresregister liefert nur zwei Stellen. */
	now.years += 2000; //we are in the 21'st century ya' know...

	/* 12-Stunden-Modus auf 0..23 umrechnen: 12 AM -> 0, 12 PM -> 12. */
	if(!is_24h)
	{
		now.hours = now.hours % 12;
		if(is_pm)
			now.hours += 12;
	}

	/* Zeitzone. In int gerechnet, damit ein negativer Offset nicht auf
	*  einen riesigen unsigned-Wert unterläuft (das tat vorher schlicht
	*  "hours - 1" bei hours == 0). */
	local_hours = (int)(now.hours % 24) + TIMEZONE_OFFSET_HOURS;
	local_hours = ((local_hours % 24) + 24) % 24;
	now.hours = (unsigned int)local_hours;

	return now;
}
