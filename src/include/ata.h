/* TomatOS - ATA/IDE block device access (PIO, LBA28)
*  Desc: Reading and writing sectors, the layer a filesystem sits on.
*
*  Programmed I/O: the CPU moves every word through the data port itself.
*  Slower than DMA, but it needs no bus master, no PCI enumeration and no
*  physical region table -- and the point of this layer is to make a
*  filesystem possible, not to be fast.
*
*  LBA28 addresses up to 2^28 sectors, i.e. 128 GiB, which is beyond
*  anything this kernel will be handed.
*/
#ifndef __ATA_H
#define __ATA_H

#include "typedefs.h"

#define ATA_SECTOR_SIZE   512
#define ATA_MAX_DRIVES    4     /* primary/secondary bus, master/slave each */

/* Probes both buses and identifies what is attached. Safe to call with no
*  controller present. */
extern void ata_init(void);

extern int ata_present(int drive);

/* Size in sectors, or 0 if the drive is absent. */
extern uint32_t ata_sectors(int drive);

/* Model string from IDENTIFY, or "" -- useful to see what was found. */
extern const char *ata_model(int drive);

/* Reads count sectors starting at lba into buf, which must hold
*  count * ATA_SECTOR_SIZE bytes. Returns 0 on success, negative on error.
*  Sectors are transferred one at a time; a drive that reports an error or
*  never becomes ready aborts the whole call rather than returning partial
*  data the caller might mistake for a short read. */
extern int ata_read(int drive, uint32_t lba, uint32_t count, void *buf);

extern int ata_write(int drive, uint32_t lba, uint32_t count, const void *buf);

/* Description of the last failure, for the shell to print. */
extern const char *ata_last_error(void);

#endif
