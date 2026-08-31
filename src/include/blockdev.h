/* TomatOS - block devices
*  Desc: One way to read and write 512 byte sectors, whatever is underneath.
*
*  WHY THIS EXISTS. The filesystem sat directly on ata_read() and ata_write(),
*  which was honest while ATA was the only way to reach a sector. It is not any
*  more: a USB stick is a block device too, and it is reached through four
*  layers that have nothing in common with an IDE controller -- a host
*  controller, the USB core, a bulk transport, and SCSI commands inside that.
*
*  So this is the join. A driver registers what it can do; the filesystem asks
*  for a sector and never learns which kind of hardware answered. It is the
*  same shape as usb_hc_ops one layer down, and for the same reason: the thing
*  above should survive the thing below being replaced.
*
*  DEVICE NUMBERS ARE STABLE, and that is a decision rather than an accident.
*  ATA drives keep numbers 0..3 -- their bus position, exactly as before, so a
*  machine with a disk on the secondary slave still calls it 3 whether or not
*  anything is plugged into USB. Removable devices take numbers above that, in
*  the order they were found. A scheme that packed everything down to the first
*  free slot would renumber a hard disk because somebody plugged a stick in,
*  and "mount 1" would mean different things on two boots of one machine.
*
*  SECTORS ARE 512 BYTES throughout. Real media exist with 4096, and a driver
*  for one would have to translate rather than report it, because the FAT code
*  and the boot chain both assume 512 in more places than are worth finding.
*  Said here so that the assumption is written down somewhere.
*/
#ifndef __BLOCKDEV_H
#define __BLOCKDEV_H

#include "typedefs.h"

#define BLK_SECTOR_SIZE  512

/* Numbers 0..3 are the ATA drives, 4 and up are whatever else registers.
*  Eight is two IDE channels' worth plus room for a few sticks. */
#define BLK_MAX_DEVICES  8
#define BLK_ATA_FIRST    0
#define BLK_ATA_COUNT    4
#define BLK_REMOVABLE_FIRST (BLK_ATA_FIRST + BLK_ATA_COUNT)

/* What a driver provides. read and write take a device NUMBER, because a
*  driver typically speaks for several -- ata for four, a USB driver for one
*  per stick -- and threading a private pointer through would buy nothing here.
*
*  Both return 0 on success and negative on failure. Neither may be called from
*  interrupt context: ATA polls and USB waits on transfers, so both can block,
*  and both are called from a task. */
typedef struct
{
    const char *name;          /* "ATA", "USB storage"                      */

    int (*read)(int dev, uint32_t lba, uint32_t count, void *buf);
    int (*write)(int dev, uint32_t lba, uint32_t count, const void *buf);

    /* How many sectors, or 0 if the device is not there. This doubles as the
    *  presence test, which is deliberate: a device with no size is not a
    *  device anyone can use, and two answers that could disagree would be two
    *  chances to get it wrong. */
    uint32_t (*sectors)(int dev);

    /* A short phrase for the shell -- the model string, the vendor. Never
    *  null, and never longer than a table column. */
    const char *(*describe)(int dev);
} blk_ops;

/* Registers a driver for one device number. Returns 0, or negative if the
*  number is out of range or already taken. A driver registers each device
*  separately so that a stick appearing later does not disturb the ones
*  already there. */
extern int blk_register(int dev, const blk_ops *ops);

/* Gives a number back, for a device that was unplugged. Anything mounted on
*  it is the caller's problem to deal with first -- this does not know about
*  filesystems and must not. */
extern void blk_unregister(int dev);

/* Brings up the drivers that are always there, which today means asking the
*  ATA driver to claim 0..3. Called once at boot, after ata_init(). */
extern void blk_init(void);

/* The interface everything above uses. A device that is not registered, or a
*  range that runs past the end of the medium, is refused rather than passed
*  down -- a driver should not have to defend itself against a caller that did
*  not check. */
extern int blk_read(int dev, uint32_t lba, uint32_t count, void *buf);
extern int blk_write(int dev, uint32_t lba, uint32_t count, const void *buf);

extern int      blk_present(int dev);
extern uint32_t blk_sectors(int dev);

/* Two names: what the medium is, and what kind of bus it is on. The second is
*  what tells a user why a device appeared or vanished between two boots. Both
*  never null. */
extern const char *blk_describe(int dev);
extern const char *blk_bus(int dev);

/* How many device numbers are in use, for a shell that wants to say so. */
extern int blk_count(void);

/* Bytes actually read and written since boot, across every device. The
*  filesystem has its own counter for what it wrote; this one includes what it
*  read, and what anything else did. */
extern uint32_t blk_bytes_read(void);
extern uint32_t blk_bytes_written(void);

/* Why the last failure failed, in words. Empty when nothing has. */
extern const char *blk_last_error(void);

#endif
