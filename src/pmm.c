/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Physischer Speichermanager (Frame-Allocator)
*
*  Verwaltet den physischen Speicher in Seiten zu 4 KiB. Die Belegung steht
*  in einer Bitmap: ein Bit je Frame, 1 = belegt, 0 = frei. Die Bitmap liegt
*  direkt hinter dem Kernel-Image, denn zu diesem Zeitpunkt gibt es noch
*  kein malloc().
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <mm.h>

/* Vom Linkerskript gestellte Marken. Als Array deklariert, damit der Name
*  selbst schon die Adresse ist - ein "extern uint32_t kernel_end;" wuerde
*  den Speicherinhalt an dieser Stelle lesen statt der Adresse. */
extern char kernel_start[];
extern char kernel_end[];

/* Rueckgabe von frame_find_free(), wenn nichts frei ist. Frame 0xFFFFF ist
*  der letzte moegliche Index, 0xFFFFFFFF kann also nie ein gueltiger sein. */
#define PMM_NO_FRAME    0xFFFFFFFFUL

/* Kleinste Adresse, ab der wir ueberhaupt Frames vergeben. Alles darunter
*  gehoert BIOS, EBDA und dem Textmodus-Puffer bei 0xB8000. */
#define PMM_LOW_LIMIT   0x00100000UL

/* Quelle der Speicherinformation - beide Durchlaeufe muessen dieselbe sein */
#define PMM_SRC_MMAP    0   /* Multiboot-Memory-Map */
#define PMM_SRC_MEM     1   /* nur mem_lower / mem_upper */
#define PMM_SRC_GUESS   2   /* gar nichts brauchbares gemeldet */

/* --- Zustand -------------------------------------------------------------- */

static uint32_t *pmm_bitmap = 0;        /* Basis der Bitmap                   */
static uint32_t  pmm_bitmap_bytes = 0;  /* ihre Groesse in Bytes              */
static uint32_t  pmm_frames = 0;        /* Anzahl verwalteter Frames          */
static uint32_t  pmm_used = 0;          /* davon belegt                       */
static uint32_t  pmm_avail_bytes = 0;   /* nutzbarer Speicher laut Map        */
static uint32_t  pmm_top_incl = 0;      /* hoechste nutzbare Byte-Adresse     */
static int       pmm_have_top = 0;      /* wurde ueberhaupt etwas gefunden?   */
static uint32_t  pmm_hint = 0;          /* Wortindex, ab dem gesucht wird     */

/* --- Bit-Ebene ------------------------------------------------------------ */

/* 1 = belegt. Ausserhalb der Bitmap gilt alles als belegt, damit sich kein
*  Aufrufer versehentlich Speicher jenseits des Verwalteten nimmt. */
static int frame_test(uint32_t idx)
{
	if (idx >= pmm_frames) return 1;
	return (pmm_bitmap[idx >> 5] & ((uint32_t)1 << (idx & 31))) != 0;
}

/* Beide Markierer sind bewusst idempotent: sie zaehlen nur, wenn sich das
*  Bit wirklich aendert. Damit kann weder doppeltes Belegen noch doppeltes
*  Freigeben die Zaehler aus dem Tritt bringen. */
static void frame_mark_used(uint32_t idx)
{
	if (idx >= pmm_frames) return;
	if (pmm_bitmap[idx >> 5] & ((uint32_t)1 << (idx & 31))) return;
	pmm_bitmap[idx >> 5] |= (uint32_t)1 << (idx & 31);
	pmm_used++;
}

static void frame_mark_free(uint32_t idx)
{
	if (idx >= pmm_frames) return;
	if (!(pmm_bitmap[idx >> 5] & ((uint32_t)1 << (idx & 31)))) return;
	pmm_bitmap[idx >> 5] &= ~((uint32_t)1 << (idx & 31));
	pmm_used--;
}

/* --- Bereichs-Ebene ------------------------------------------------------- */

/* Rechnet (base, len) in die letzte enthaltene Byte-Adresse um. Wir rechnen
*  durchgehend mit dieser inklusiven Endadresse, weil base+len bei einem
*  Bereich, der bis an die 4-GiB-Grenze reicht, ueberlaufen wuerde.
*  Rueckgabe 0, wenn der Bereich leer ist. */
static int region_bounds(uint32_t base, uint32_t len, uint32_t *end_incl)
{
	uint32_t e;

	if (len == 0) return 0;
	e = base + len - 1;
	if (e < base) e = 0xFFFFFFFFUL;	/* Ueberlauf: bei 4 GiB abschneiden */
	*end_incl = e;
	return 1;
}

/* Belegen: nach aussen runden. Jeder Frame, den der Bereich auch nur
*  anschneidet, gilt als belegt - lieber ein Frame zu viel gesperrt als
*  einer, in dem noch fremde Daten stehen. */
static void region_mark_used(uint32_t base, uint32_t len)
{
	uint32_t end_incl;
	uint32_t first;
	uint32_t last;
	uint32_t i;

	if (pmm_frames == 0) return;
	if (!region_bounds(base, len, &end_incl)) return;

	first = base >> 12;
	last  = end_incl >> 12;
	if (first >= pmm_frames) return;
	if (last >= pmm_frames) last = pmm_frames - 1;

	for (i = first; i <= last; i++)
		frame_mark_used(i);
}

/* Freigeben: nach innen runden. Nur Frames, die vollstaendig im Bereich
*  liegen, werden frei - ein angeschnittener Frame am Rand bleibt gesperrt. */
static void region_mark_free(uint32_t base, uint32_t len)
{
	uint32_t end_incl;
	uint32_t first;
	uint32_t last;
	uint32_t i;

	if (pmm_frames == 0) return;
	if (!region_bounds(base, len, &end_incl)) return;

	/* Passt ueberhaupt ein ganzer Frame hinein? */
	if (end_incl < PMM_FRAME_SIZE - 1) return;
	if (base > 0xFFFFFFFFUL - (PMM_FRAME_SIZE - 1)) return;

	first = (base + (PMM_FRAME_SIZE - 1)) >> 12;	/* Anfang aufrunden */
	last  = (end_incl - (PMM_FRAME_SIZE - 1)) >> 12;	/* Ende abrunden  */

	if (first > last) return;
	if (first >= pmm_frames) return;
	if (last >= pmm_frames) last = pmm_frames - 1;

	for (i = first; i <= last; i++)
		frame_mark_free(i);
}

/* Erster Durchlauf: hoechste Adresse und Gesamtmenge merken. */
static void region_note(uint32_t base, uint32_t len)
{
	uint32_t end_incl;
	uint32_t clen;

	if (!region_bounds(base, len, &end_incl)) return;

	if (!pmm_have_top || end_incl > pmm_top_incl)
	{
		pmm_top_incl = end_incl;
		pmm_have_top = 1;
	}

	clen = end_incl - base + 1;
	if (pmm_avail_bytes > 0xFFFFFFFFUL - clen) pmm_avail_bytes = 0xFFFFFFFFUL;
	else pmm_avail_bytes += clen;
}

/* --- Multiboot-Memory-Map ------------------------------------------------- */

/* pass == 0: nur messen, pass != 0: verfuegbare Bereiche freigeben. */
static void pmm_walk_mmap(multiboot_info *mbi, int pass)
{
	multiboot_mmap_entry *ent;
	uint32_t addr;
	uint32_t end;
	uint32_t next;

	addr = mbi->mmap_addr;
	end  = addr + mbi->mmap_length;
	if (end < addr) return;		/* unsinnige Laengenangabe */

	while (addr < end)
	{
		/* Reicht der Rest ueberhaupt fuer einen vollstaendigen Eintrag? */
		if (end - addr < (uint32_t)sizeof(multiboot_mmap_entry)) break;

		ent = (multiboot_mmap_entry *)addr;

		/* size zaehlt die Bytes NACH dem size-Feld; unter 20 kann kein
		*  gueltiger Eintrag liegen. Abbrechen statt endlos drehen. */
		if (ent->size < 20) break;

		/* Nur echtes RAM, und nur was ein 32-Bit-Kernel adressieren kann:
		*  alles mit gesetzten oberen Haelften liegt jenseits von 4 GiB. */
		if (ent->type == MULTIBOOT_MEMORY_AVAILABLE &&
		    ent->addr_high == 0 && ent->len_high == 0)
		{
			if (pass == 0) region_note(ent->addr_low, ent->len_low);
			else           region_mark_free(ent->addr_low, ent->len_low);
		}

		/* Der klassische Stolperstein: NICHT sizeof() weiterspringen. */
		next = addr + ent->size + 4;
		if (next <= addr) break;	/* Ueberlauf oder Stillstand */
		addr = next;
	}
}

/* Rueckfallweg fuer karge Bootloader: mem_lower / mem_upper in KiB. */
static void pmm_walk_meminfo(multiboot_info *mbi, int pass)
{
	uint32_t lower;
	uint32_t upper;

	lower = mbi->mem_lower;
	upper = mbi->mem_upper;

	/* Unterhalb von 1 MiB gibt es hoechstens 640 KiB nutzbares RAM, und
	*  oberhalb kappen wir so, dass 0x100000 + upper*1024 nicht ueberlaeuft. */
	if (lower > 640) lower = 640;
	if (upper > 0x003FF000UL) upper = 0x003FF000UL;

	if (pass == 0)
	{
		region_note(0, lower << 10);
		region_note(PMM_LOW_LIMIT, upper << 10);
	}
	else
	{
		region_mark_free(0, lower << 10);
		region_mark_free(PMM_LOW_LIMIT, upper << 10);
	}
}

/* --- Aufbau --------------------------------------------------------------- */

void pmm_init(multiboot_info *mbi)
{
	uint32_t kstart;
	uint32_t kend;
	uint32_t bmp;
	int src;

	pmm_bitmap       = 0;
	pmm_bitmap_bytes = 0;
	pmm_frames       = 0;
	pmm_used         = 0;
	pmm_avail_bytes  = 0;
	pmm_top_incl     = 0;
	pmm_have_top     = 0;
	pmm_hint         = 0;

	kstart = (uint32_t)kernel_start;
	kend   = (uint32_t)kernel_end;

	/* 1. Durchlauf: Woher wissen wir, wieviel Speicher da ist? */
	src = PMM_SRC_GUESS;

	if (mbi != 0 && (mbi->flags & MULTIBOOT_INFO_MEM_MAP) != 0 &&
	    mbi->mmap_length >= (uint32_t)sizeof(multiboot_mmap_entry))
	{
		pmm_walk_mmap(mbi, 0);
		if (pmm_have_top) src = PMM_SRC_MMAP;
	}

	if (src == PMM_SRC_GUESS && mbi != 0 &&
	    (mbi->flags & MULTIBOOT_INFO_MEMORY) != 0)
	{
		pmm_walk_meminfo(mbi, 0);
		if (pmm_have_top) src = PMM_SRC_MEM;
	}

	/* Notnagel. Auch der Rueckfallweg kann Unsinn liefern - wenn hinter dem
	*  Kernel-Image angeblich nichts mehr kommt, ist die Angabe wertlos. */
	if (!pmm_have_top || pmm_top_incl < kend)
	{
		printf("PMM: Bootloader meldet keinen brauchbaren Speicher, nehme 16 MiB an\n");
		pmm_top_incl    = 0;
		pmm_have_top    = 0;
		pmm_avail_bytes = 0;
		src = PMM_SRC_GUESS;
		region_note(PMM_LOW_LIMIT, 0x00F00000UL);	/* 1 MiB .. 16 MiB */
	}

	/* Frames: pmm_top_incl ist die letzte nutzbare Byte-Adresse, der Frame
	*  mit diesem Index existiert also noch - daher das +1. */
	pmm_frames       = (pmm_top_incl >> 12) + 1;
	pmm_bitmap_bytes = ((pmm_frames + 31) >> 5) * 4;

	/* Die Bitmap kommt direkt hinter das Kernel-Image (4 Byte ausgerichtet,
	*  weil wir sie wortweise durchsuchen). */
	bmp = ((uint32_t)kernel_end + 3) & ~(uint32_t)3;

	if (bmp < kend || pmm_bitmap_bytes == 0 ||
	    bmp > 0xFFFFFFFFUL - pmm_bitmap_bytes ||
	    bmp + pmm_bitmap_bytes - 1 > pmm_top_incl)
	{
		pmm_frames = 0;
		panic("PMM: kein Platz fuer die Frame-Bitmap");
		return;
	}

	pmm_bitmap = (uint32_t *)bmp;

	/* Erst gilt alles als belegt. Die Fuellbits im letzten Wort oberhalb von
	*  pmm_frames bleiben dadurch dauerhaft 1 und koennen nie vergeben
	*  werden - freigegeben wird nur ueber Indizes < pmm_frames. */
	memset(pmm_bitmap, (char)0xFF, (size_t)pmm_bitmap_bytes);
	pmm_used = pmm_frames;

	/* Dann die tatsaechlich verfuegbaren Bereiche freigeben. */
	if (src == PMM_SRC_MMAP)     pmm_walk_mmap(mbi, 1);
	else if (src == PMM_SRC_MEM) pmm_walk_meminfo(mbi, 1);
	else                         region_mark_free(PMM_LOW_LIMIT, 0x00F00000UL);

	/* Und zuletzt alles wieder sperren, was nicht vergeben werden darf. */
	frame_mark_used(0);				/* Adresse 0 bleibt ungueltig    */
	region_mark_used(0, PMM_LOW_LIMIT);		/* BIOS, EBDA, VGA bei 0xB8000   */
	region_mark_used(kstart, kend - kstart);	/* das Kernel-Image selbst       */
	region_mark_used(bmp, pmm_bitmap_bytes);	/* die Bitmap selbst             */

	/* Die Multiboot-Strukturen legt der Bootloader irgendwo ins RAM ab -
	*  meist unter 1 MiB, garantiert ist das aber nicht. Sperren, damit sie
	*  nicht spaeter unter dem Kernel weg ueberschrieben werden. */
	if (mbi != 0)
	{
		region_mark_used((uint32_t)mbi, (uint32_t)sizeof(multiboot_info));
		if (src == PMM_SRC_MMAP && mbi->mmap_length <= 0x10000UL)
			region_mark_used(mbi->mmap_addr, mbi->mmap_length);
	}

	printf("PMM: %d KiB nutzbar, %d Frames zu 4 KiB, %d frei\n",
	       (int)(pmm_avail_bytes >> 10), (int)pmm_frames,
	       (int)(pmm_frames - pmm_used));
	printf("PMM: Bitmap bei 0x%X, %d Bytes\n", (int)bmp, (int)pmm_bitmap_bytes);
}

/* --- Suche ---------------------------------------------------------------- */

/* Niedrigstes Nullbit im Wort w, oder PMM_NO_FRAME. */
static uint32_t word_first_free(uint32_t w)
{
	uint32_t bits;
	uint32_t b;
	uint32_t idx;

	bits = pmm_bitmap[w];
	for (b = 0; b < 32; b++)
	{
		if ((bits & ((uint32_t)1 << b)) == 0)
		{
			idx = (w << 5) + b;
			if (idx < pmm_frames) return idx;
			return PMM_NO_FRAME;
		}
	}
	return PMM_NO_FRAME;
}

/* Erster freier Frame. Startet beim Hinweis und wickelt einmal um. */
static uint32_t frame_find_free(void)
{
	uint32_t words;
	uint32_t w;
	uint32_t idx;

	words = (pmm_frames + 31) >> 5;
	if (pmm_hint >= words) pmm_hint = 0;

	for (w = pmm_hint; w < words; w++)
	{
		if (pmm_bitmap[w] == 0xFFFFFFFFUL) continue;
		idx = word_first_free(w);
		if (idx != PMM_NO_FRAME) { pmm_hint = w; return idx; }
	}
	for (w = 0; w < pmm_hint; w++)
	{
		if (pmm_bitmap[w] == 0xFFFFFFFFUL) continue;
		idx = word_first_free(w);
		if (idx != PMM_NO_FRAME) { pmm_hint = w; return idx; }
	}
	return PMM_NO_FRAME;
}

/* --- Oeffentliche Schnittstelle ------------------------------------------- */

void *pmm_alloc_frame(void)
{
	uint32_t idx;

	if (pmm_frames == 0) return 0;

	idx = frame_find_free();
	if (idx == PMM_NO_FRAME) return 0;

	frame_mark_used(idx);
	return (void *)(idx << 12);
}

void pmm_free_frame(void *frame)
{
	uint32_t addr;
	uint32_t idx;

	if (frame == 0) return;			/* Nullzeiger ist erlaubt */
	if (pmm_frames == 0) return;

	addr = (uint32_t)frame;
	if ((addr & (PMM_FRAME_SIZE - 1)) != 0) return;	/* nicht ausgerichtet */

	idx = addr >> 12;
	if (idx >= pmm_frames) return;

	/* frame_mark_free() zaehlt nur herunter, wenn das Bit auch wirklich
	*  gesetzt war - ein doppeltes free() bleibt damit folgenlos. */
	if ((idx >> 5) < pmm_hint) pmm_hint = idx >> 5;
	frame_mark_free(idx);
}

void *pmm_alloc_frames(uint32_t count)
{
	uint32_t i;
	uint32_t j;
	uint32_t run;
	uint32_t start;

	if (count == 0) return 0;
	if (count == 1) return pmm_alloc_frame();
	if (pmm_frames == 0 || count > pmm_frames) return 0;

	run   = 0;
	start = 0;

	/* i laeuft nie ueber pmm_frames hinaus, die Bitmap wird also nur
	*  innerhalb ihrer Grenzen gelesen. Ein Lauf gilt erst als gefunden,
	*  wenn count Frames tatsaechlich geprueft wurden. */
	for (i = 0; i < pmm_frames; i++)
	{
		/* Volle Woerter ueberspringen: 32 belegte Frames am Stueck. */
		if ((i & 31) == 0 && pmm_bitmap[i >> 5] == 0xFFFFFFFFUL)
		{
			run = 0;
			i += 31;	/* der Schleifenkopf zaehlt die 32. dazu */
			continue;
		}

		if (frame_test(i))
		{
			run = 0;
			continue;
		}

		if (run == 0) start = i;
		run++;

		if (run == count)
		{
			for (j = 0; j < count; j++)
				frame_mark_used(start + j);
			return (void *)(start << 12);
		}
	}

	return 0;
}

void pmm_free_frames(void *frames, uint32_t count)
{
	uint32_t addr;
	uint32_t idx;
	uint32_t i;

	if (frames == 0 || count == 0) return;
	if (pmm_frames == 0) return;

	addr = (uint32_t)frames;
	if ((addr & (PMM_FRAME_SIZE - 1)) != 0) return;

	idx = addr >> 12;
	/* So formuliert, dass idx + count nicht ueberlaufen kann. */
	if (idx >= pmm_frames || count > pmm_frames - idx) return;

	if ((idx >> 5) < pmm_hint) pmm_hint = idx >> 5;

	for (i = 0; i < count; i++)
		frame_mark_free(idx + i);
}

/* --- Kennzahlen ----------------------------------------------------------- */

uint32_t pmm_total_frames(void)
{
	return pmm_frames;
}

uint32_t pmm_used_frames(void)
{
	return pmm_used;
}

uint32_t pmm_free_frames_count(void)
{
	return pmm_frames - pmm_used;
}

/* Nutzbarer Speicher laut Memory-Map, also die Summe der als AVAILABLE
*  gemeldeten Bereiche - nicht dasselbe wie pmm_total_frames() * 4096, das
*  die Loecher zwischen den Bereichen mitzaehlt. */
uint32_t pmm_total_bytes(void)
{
	return pmm_avail_bytes;
}
