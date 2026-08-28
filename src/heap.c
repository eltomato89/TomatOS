/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Kernel-Heap. malloc()/free() auf Basis des physischen
*        Frame-Allocators aus pmm.c.
*
*  Bauart: eine einzige, nach Adressen sortierte doppelt verkettete Liste
*  ueber *alle* Bloecke (belegte wie freie), jeder mit einem Kopf davor.
*  Die Adressordnung ist der Kniff: Verschmelzen zweier freier Nachbarn
*  ist damit ein Blick auf prev/next, und die Frage "grenzen die beiden
*  wirklich aneinander?" beantwortet ein Adressvergleich -- noetig, weil
*  der Heap aus mehreren, nicht zwangslaeufig zusammenhaengenden
*  Frame-Bloecken des pmm besteht.
*
*  Ein Buddy- oder Slab-Allocator waere fuer einen Kernel dieser Groesse
*  ueberdimensioniert; die Freispeicherliste reicht voellig.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <mm.h>

/* Ausrichtung aller zurueckgegebenen Zeiger. 8 Byte, damit spaetere
*  Strukturen mit 64-Bit-Feldern (uint64, double) sauber liegen und der
*  Prozessor keine Zugriffe ueber Wortgrenzen aufteilen muss. */
#define HEAP_ALIGN          8

/* Kennung im Blockkopf. Damit erkennt free() Zeiger, die gar nicht vom
*  Heap stammen, statt die Verwaltung still zu zerlegen -- im Ring 0 ohne
*  Speicherschutz der einzige Schutz, den wir haben. */
#define HEAP_MAGIC          0xA110CA7EUL

/* Anfangsgroesse und Mindest-Nachschlag, jeweils in Frames (4 KiB).
*  16 Frames = 64 KiB. Lieber in groesseren Schritten wachsen, als den
*  pmm bei jedem malloc() zu belaestigen. */
#define HEAP_INITIAL_FRAMES 16
#define HEAP_GROW_FRAMES    16

/* Obergrenze fuer eine einzelne Anforderung. Verhindert, dass die
*  Aufrundung auf ganze Frames weiter unten ueberlaeuft. */
#define HEAP_MAX_ALLOC      0x3FFFFFFFUL

struct heap_block
{
    uint32_t magic;             /* HEAP_MAGIC, sonst kein gueltiger Block */
    uint32_t size;              /* Nutzlast in Bytes, stets Vielfaches von 8 */
    uint32_t used;              /* 0 = frei, 1 = belegt */
    uint32_t fill;              /* haelt den Kopf auf 24 Byte (s.u.) */
    struct heap_block *prev;    /* Vorgaenger in Adressreihenfolge */
    struct heap_block *next;    /* Nachfolger in Adressreihenfolge */
};

/* 24 Byte, ein Vielfaches von HEAP_ALIGN. Zusammen damit, dass jede
*  Nutzlastgroesse auf 8 aufgerundet wird und die pmm-Frames an 4-KiB-Grenzen
*  liegen, ist jeder Blockanfang -- und damit jede Nutzlast -- 8-ausgerichtet. */
#define HEAP_HEADER_SIZE    ((uint32_t)sizeof(struct heap_block))

/* Kleinste Nutzlast, die ein Block noch tragen darf. */
#define HEAP_MIN_PAYLOAD    HEAP_ALIGN

/* Uebersetzungszeit-Zusicherung: waere der Kopf kein Vielfaches von
*  HEAP_ALIGN, kaeme jede zweite Nutzlast schief heraus. Bricht die
*  Uebersetzung mit "negative array size" ab, falls das je verletzt wird. */
typedef char heap_header_alignment_check
    [(sizeof(struct heap_block) % HEAP_ALIGN) == 0 ? 1 : -1];

/* Aufrunden auf die naechste Ausrichtungsgrenze. */
#define HEAP_ALIGN_UP(x)    (((x) + (uint32_t)(HEAP_ALIGN - 1)) & ~(uint32_t)(HEAP_ALIGN - 1))

static struct heap_block *heap_first = 0;   /* Kopf der Adressliste */
static uint32_t heap_bytes = 0;             /* insgesamt vom pmm geholt */
static int heap_ready = 0;

/* --- Interne Helfer ------------------------------------------------------ */

/* Liegt b unmittelbar hinter der Nutzlast von a? Nur dann duerfen die
*  beiden verschmolzen werden -- Listennachbarschaft allein genuegt nicht,
*  weil zwei getrennte pmm-Anforderungen eine Luecke lassen koennen. */
static int heap_adjacent(struct heap_block *a, struct heap_block *b)
{
    if(a == 0 || b == 0)
        return 0;
    return ((unsigned char *)a + HEAP_HEADER_SIZE + a->size) == (unsigned char *)b;
}

/* Schluckt den Nachfolger. Aufrufer garantiert: beide frei und benachbart.
*  Der Kopf des Nachfolgers verschwindet in der Nutzlast und wird deshalb
*  entwertet -- ein noch herumliegender Zeiger darauf faellt dann auf. */
static void heap_merge_next(struct heap_block *blk)
{
    struct heap_block *n;

    n = blk->next;
    blk->size += HEAP_HEADER_SIZE + n->size;
    blk->next = n->next;
    if(n->next != 0)
        n->next->prev = blk;
    n->magic = 0;
}

/* Verschmilzt einen frisch freigegebenen Block mit beiden Nachbarn.
*  Erst nach hinten, dann nach vorne: so bleibt hoechstens ein einziger
*  freier Block uebrig, egal in welcher Reihenfolge freigegeben wurde. */
static void heap_coalesce(struct heap_block *blk)
{
    if(blk->next != 0 && blk->next->used == 0 && heap_adjacent(blk, blk->next))
        heap_merge_next(blk);

    if(blk->prev != 0 && blk->prev->used == 0 && heap_adjacent(blk->prev, blk))
        heap_merge_next(blk->prev);
}

/* Teilt blk so, dass vorne genau need Byte Nutzlast stehen. Der Rest wird
*  ein eigener freier Block -- aber nur, wenn er noch einen Kopf *und* eine
*  brauchbare Nutzlast traegt. Sonst bleibt der Ueberhang beim Block, das
*  ist billiger als ein unbenutzbarer Splitter. */
static void heap_split(struct heap_block *blk, uint32_t need)
{
    struct heap_block *rest;

    if(blk->size < need + HEAP_HEADER_SIZE + HEAP_MIN_PAYLOAD)
        return;

    rest = (struct heap_block *)((unsigned char *)blk + HEAP_HEADER_SIZE + need);
    rest->magic = HEAP_MAGIC;
    rest->size  = blk->size - need - HEAP_HEADER_SIZE;
    rest->used  = 0;
    rest->fill  = 0;
    rest->prev  = blk;
    rest->next  = blk->next;

    if(rest->next != 0)
        rest->next->prev = rest;
    blk->next = rest;
    blk->size = need;
}

/* Haengt einen frisch vom pmm geholten Bereich als freien Block in die
*  Adressliste ein und versucht sofort, ihn mit den Nachbarn zu verschmelzen.
*  Der pmm darf uns Frames unterhalb wie oberhalb des bisherigen Heaps geben,
*  darum wird an der richtigen Stelle einsortiert und nicht bloss angehaengt. */
static void heap_add_region(void *base, uint32_t bytes)
{
    struct heap_block *blk;
    struct heap_block *p;

    if(base == 0 || bytes < HEAP_HEADER_SIZE + HEAP_MIN_PAYLOAD)
        return;

    blk = (struct heap_block *)base;
    blk->magic = HEAP_MAGIC;
    blk->size  = bytes - HEAP_HEADER_SIZE;
    blk->used  = 0;
    blk->fill  = 0;
    blk->prev  = 0;
    blk->next  = 0;

    heap_bytes += bytes;

    if(heap_first == 0)
    {
        heap_first = blk;
        return;
    }

    if((uint32_t)blk < (uint32_t)heap_first)
    {
        blk->next = heap_first;
        heap_first->prev = blk;
        heap_first = blk;
    }
    else
    {
        p = heap_first;
        while(p->next != 0 && (uint32_t)p->next < (uint32_t)blk)
            p = p->next;

        blk->prev = p;
        blk->next = p->next;
        if(p->next != 0)
            p->next->prev = blk;
        p->next = blk;
    }

    heap_coalesce(blk);
}

/* Fordert beim pmm so viele Frames nach, dass need Byte Nutzlast
*  hineinpassen -- mindestens aber HEAP_GROW_FRAMES. Liefert 1 bei Erfolg. */
static int heap_grow(uint32_t need)
{
    uint32_t bytes;
    uint32_t frames;
    uint32_t minimum;
    void *base;

    if(need > HEAP_MAX_ALLOC)
        return 0;

    bytes = need + HEAP_HEADER_SIZE;
    minimum = (bytes + (uint32_t)(PMM_FRAME_SIZE - 1)) / (uint32_t)PMM_FRAME_SIZE;

    frames = minimum;
    if(frames < (uint32_t)HEAP_GROW_FRAMES)
        frames = (uint32_t)HEAP_GROW_FRAMES;

    base = pmm_alloc_frames(frames);
    if(base == 0 && frames != minimum)
    {
        /* Fuer den grosszuegigen Wunsch war kein zusammenhaengender Block
        *  mehr da -- zweiter Versuch mit dem, was wirklich noetig ist. */
        frames = minimum;
        base = pmm_alloc_frames(frames);
    }
    if(base == 0)
        return 0;

    heap_add_region(base, frames * (uint32_t)PMM_FRAME_SIZE);
    return 1;
}

/* Erster passender freier Block (First-Fit). Bei den paar Dutzend Bloecken
*  eines Hobbykernels ist Best-Fit den Aufwand nicht wert. */
static struct heap_block *heap_find(uint32_t need)
{
    struct heap_block *blk;

    blk = heap_first;
    while(blk != 0)
    {
        if(blk->used == 0 && blk->size >= need)
            return blk;
        blk = blk->next;
    }
    return 0;
}

/* Vom Nutzzeiger zurueck auf den Blockkopf, mit Plausibilitaetspruefung.
*  Liefert 0 und meldet den Fehler, wenn der Zeiger nicht vom Heap stammt. */
static struct heap_block *heap_block_of(void *ptr)
{
    struct heap_block *blk;

    /* Billiger Vortest vor dem Dereferenzieren: jede Nutzlast des Heaps ist
    *  8-ausgerichtet und liegt hinter einem Kopf, also nie ganz unten. */
    if((((uint32_t)ptr) & (uint32_t)(HEAP_ALIGN - 1)) != 0 ||
       ((uint32_t)ptr) < HEAP_HEADER_SIZE)
    {
        printf("heap: Zeiger %X stammt nicht vom Heap (Ausrichtung)\n",
               (uint32_t)ptr);
        return 0;
    }

    blk = (struct heap_block *)((unsigned char *)ptr - HEAP_HEADER_SIZE);
    if(blk->magic != HEAP_MAGIC)
    {
        printf("heap: ungueltiger Zeiger %X (Magic %X)\n",
               (uint32_t)ptr, blk->magic);
        return 0;
    }

    return blk;
}

/* Rundet eine Nutzeranforderung auf die interne Blockgroesse auf.
*  malloc(0) landet dabei auf HEAP_MIN_PAYLOAD (siehe malloc()). */
static uint32_t heap_round(size_t size)
{
    uint32_t need;

    need = HEAP_ALIGN_UP((uint32_t)size);
    if(need == 0)
        need = HEAP_MIN_PAYLOAD;
    return need;
}

/* --- Oeffentliche Schnittstelle ------------------------------------------ */

void heap_init(void)
{
    if(heap_ready != 0)
        return;

    heap_ready = 1;
    heap_first = 0;
    heap_bytes = 0;

    if(heap_grow((uint32_t)HEAP_INITIAL_FRAMES * (uint32_t)PMM_FRAME_SIZE
                 - HEAP_HEADER_SIZE) == 0)
        printf("heap: kein Speicher fuer den Heap-Anfang vom pmm\n");
}

/* malloc(0) liefert einen gueltigen, eindeutigen Zeiger auf einen Block
*  mit HEAP_MIN_PAYLOAD Byte Nutzlast -- also *keinen* Nullzeiger. So muss
*  kein Aufrufer die 0 als Sonderfall behandeln, und der Rueckgabewert darf
*  ganz normal an free() oder realloc() gehen. Dereferenzieren darf man ihn
*  natuerlich nicht. (C erlaubt beide Auslegungen; das ist unsere.) */
void *malloc(size_t size)
{
    struct heap_block *blk;
    uint32_t need;

    /* size_t ist in diesem Projekt "int", also vorzeichenbehaftet. Eine
    *  negative Groesse ist ein Programmierfehler und wird abgewiesen,
    *  statt sie als knapp 4 GiB zu deuten. */
    if(size < 0)
    {
        printf("heap: malloc mit negativer Groesse (%d)\n", size);
        return 0;
    }

    /* Falls jemand den Heap vor heap_init() benutzt: still nachholen. */
    if(heap_ready == 0)
        heap_init();

    need = heap_round(size);
    if(need > HEAP_MAX_ALLOC)
        return 0;

    blk = heap_find(need);
    if(blk == 0)
    {
        /* Nichts Passendes da -- beim pmm nachfordern und einmal
        *  wiederholen. Nach dem Verschmelzen kann auch ein bereits
        *  vorhandener Block der richtige sein, darum die volle Suche. */
        if(heap_grow(need) == 0)
            return 0;
        blk = heap_find(need);
        if(blk == 0)
            return 0;
    }

    heap_split(blk, need);
    blk->used = 1;

    return (void *)((unsigned char *)blk + HEAP_HEADER_SIZE);
}

void free(void *ptr)
{
    struct heap_block *blk;

    /* free(0) ist ausdruecklich folgenlos. */
    if(ptr == 0)
        return;

    blk = heap_block_of(ptr);
    if(blk == 0)
        return;                 /* Fehler wurde bereits gemeldet */

    if(blk->used == 0)
    {
        printf("heap: doppeltes free() auf %X\n", (uint32_t)ptr);
        return;
    }

    blk->used = 0;
    heap_coalesce(blk);
}

void *calloc(size_t num, size_t size)
{
    uint32_t bytes;
    void *ptr;

    if(num < 0 || size < 0)
    {
        printf("heap: calloc mit negativer Groesse (%d * %d)\n", num, size);
        return 0;
    }

    if(num == 0 || size == 0)
        return malloc(0);

    /* Ueberlaufschutz per Division statt 64-Bit-Multiplikation: passt
    *  num nicht mehr in den erlaubten Bereich geteilt durch size, dann
    *  wuerde das Produkt umlaufen. */
    if((uint32_t)num > HEAP_MAX_ALLOC / (uint32_t)size)
    {
        printf("heap: calloc-Ueberlauf (%d * %d)\n", num, size);
        return 0;
    }

    bytes = (uint32_t)num * (uint32_t)size;

    ptr = malloc((size_t)bytes);
    if(ptr != 0)
        memset(ptr, 0, (size_t)bytes);

    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    struct heap_block *blk;
    void *neu;
    uint32_t need;
    uint32_t copy;

    if(size < 0)
    {
        printf("heap: realloc mit negativer Groesse (%d)\n", size);
        return 0;
    }

    /* realloc(0, n) ist malloc(n) ... */
    if(ptr == 0)
        return malloc(size);

    /* ... und realloc(p, 0) ist free(p). */
    if(size == 0)
    {
        free(ptr);
        return 0;
    }

    blk = heap_block_of(ptr);
    if(blk == 0)
        return 0;

    if(blk->used == 0)
    {
        printf("heap: realloc auf bereits freigegebenem Block %X\n",
               (uint32_t)ptr);
        return 0;
    }

    need = heap_round(size);
    if(need > HEAP_MAX_ALLOC)
        return 0;

    /* Passt schon: hoechstens den Ueberhang abschneiden, Zeiger bleibt. */
    if(blk->size >= need)
    {
        heap_split(blk, need);
        if(blk->next != 0 && blk->next->used == 0)
            heap_coalesce(blk->next);
        return ptr;
    }

    /* Waechst der direkt folgende freie Nachbar mit hinein, sparen wir uns
    *  Kopieren und Umziehen komplett. */
    if(blk->next != 0 && blk->next->used == 0 && heap_adjacent(blk, blk->next) &&
       blk->size + HEAP_HEADER_SIZE + blk->next->size >= need)
    {
        heap_merge_next(blk);
        heap_split(blk, need);
        return ptr;
    }

    neu = malloc(size);
    if(neu == 0)
        return 0;               /* alter Block bleibt unangetastet gueltig */

    /* Nur so viel kopieren, wie der alte Block wirklich hatte -- er ist
    *  hier immer kleiner als der neue, aber die Klammer schadet nicht. */
    copy = blk->size;
    if(copy > need)
        copy = need;

    memcpy(neu, ptr, (size_t)copy);
    free(ptr);

    return neu;
}

/* Belegt: Nutzlast der benutzten Bloecke plus deren Verwaltungskoepfe.
*  Wird durch Durchlaufen der Liste bestimmt statt mitgezaehlt -- bei den
*  Blockzahlen hier vernachlaessigbar und dafuer nicht driftanfaellig. */
uint32_t heap_used(void)
{
    struct heap_block *blk;
    uint32_t sum;

    sum = 0;
    blk = heap_first;
    while(blk != 0)
    {
        if(blk->used != 0)
            sum += HEAP_HEADER_SIZE + blk->size;
        blk = blk->next;
    }
    return sum;
}

/* Insgesamt beim pmm angeforderter Speicher in Bytes. */
uint32_t heap_total(void)
{
    return heap_bytes;
}
