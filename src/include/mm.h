/* TomatOS - Speicherverwaltung
*  Desc: Physischer Frame-Allocator und Kernel-Heap.
*
*  Zweistufig: pmm_* vergibt physische 4-KiB-Frames anhand der Memory-Map
*  des Bootloaders, darauf setzt der Heap mit malloc()/free() auf.
*/
#ifndef __MM_H
#define __MM_H

#include "typedefs.h"
#include "multiboot.h"

#define PMM_FRAME_SIZE  4096

/* --- Physischer Speicher (pmm.c) ---------------------------------------- */

/* Baut die Frame-Bitmap aus der Multiboot-Memory-Map auf. Frames, die vom
 * Kernel-Image selbst oder von der Bitmap belegt sind, werden als benutzt
 * markiert. Muss vor jeder anderen pmm-Funktion laufen. */
extern void pmm_init(multiboot_info *mbi);

/* Liefert einen freien 4-KiB-Frame oder 0, wenn keiner mehr da ist.
 * Der Inhalt ist nicht genullt. */
extern void *pmm_alloc_frame(void);

/* Gibt einen zuvor per pmm_alloc_frame() geholten Frame zurueck.
 * Ein Nullzeiger ist erlaubt und bewirkt nichts. */
extern void pmm_free_frame(void *frame);

/* Zusammenhaengender Block aus count Frames, oder 0. Fuer den Heap. */
extern void *pmm_alloc_frames(uint32_t count);
extern void pmm_free_frames(void *frames, uint32_t count);

/* Kennzahlen, in Frames. Gedacht fuer den mem-Befehl und die Statusleiste. */
extern uint32_t pmm_total_frames(void);
extern uint32_t pmm_used_frames(void);
extern uint32_t pmm_free_frames_count(void);

/* Insgesamt nutzbarer Speicher in Bytes laut Memory-Map. */
extern uint32_t pmm_total_bytes(void);

/* --- Kernel-Heap (heap.c) ------------------------------------------------ */

/* Richtet den Heap ein. Setzt ein initialisiertes pmm voraus. */
extern void heap_init(void);

extern void *malloc(size_t size);
extern void free(void *ptr);
extern void *calloc(size_t num, size_t size);
extern void *realloc(void *ptr, size_t size);

/* Kennzahlen in Bytes: belegt (Nutzdaten inkl. Verwaltung) und
 * insgesamt vom Heap beim pmm angeforderter Speicher. */
extern uint32_t heap_used(void);
extern uint32_t heap_total(void);

#endif
