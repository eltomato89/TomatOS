/* TomatOS - FAT12/FAT16/FAT32 filesystem
*  Desc: Enough to list a directory, and to read and write a file on a real
*        disk.
*
*  Three formats rather than one, and that is not indecision. FAT12 carries the
*  floppy path, the boot image is FAT16 by construction (see the geometry block
*  in the Makefile, which argues the cluster-count boundaries at length), and
*  FAT32 is what any USB stick larger than 2 GB actually is -- so without it
*  the mass storage driver could only read media this kernel formatted itself.
*
*  Writing is the first thing in this kernel that can DESTROY data. Everything
*  before it was additive: a wrong packet is dropped, a wrong pixel is
*  overwritten, a wrong page faults. A wrong sector write takes a file with it,
*  and on real hardware it takes somebody's file. That shapes what is here --
*  see the notes on the write functions, and in particular the one on the two
*  FAT copies, which is the difference between a filesystem another operating
*  system will still mount and one it will offer to repair.
*
*  FAT12 and FAT16 differ only in how wide a FAT entry is -- 12 or 16 bits --
*  and in where the root directory lives. Both are handled here; FAT32 is
*  not, because it moves the root directory into the cluster chain and needs
*  a different layout throughout.
*
*  Long file names are ignored: entries are 8.3, and VFAT long name entries
*  are skipped rather than assembled. That keeps the directory walk simple
*  and costs nothing a shell needs today.
*
*  No allocation happens here. Directory reads are index based and fill a
*  caller supplied struct, so the filesystem works with the kernel heap
*  present or absent.
*/
#ifndef __FAT_H
#define __FAT_H

#include "typedefs.h"

#define FAT_NAME_MAX  13   /* 8 + '.' + 3 + NUL */

typedef struct
{
    char     name[FAT_NAME_MAX];
    uint32_t size;              /* bytes; 0 for a directory */
    uint32_t first_cluster;
    int      is_dir;
} fat_dirent;

/* Reads the boot sector of the given device and works out the layout.
*  Returns 0 on success, negative if it holds nothing recognisable.
*
*  "drive" is a BLOCK DEVICE number now, not an ATA drive -- see blockdev.h.
*  The numbers 0..3 still mean exactly the ATA drives they always did, so
*  nothing that mounted drive 0 has to change; what is new is that 4 and up can
*  be a USB stick, and this file no longer knows the difference. */
extern int fat_mount(int drive);

extern int fat_mounted(void);

/* "FAT12" or "FAT16", or "" when nothing is mounted. */
extern const char *fat_type(void);

/* Volume label from the boot sector, or "" if it has none. */
extern const char *fat_label(void);

/* Volume size, in KIBIBYTES rather than bytes.
*
*  Bytes in 32 bits stop at 4 GB, which was survivable while a volume was a
*  32 MB image or a floppy and is not once a USB stick is in the picture: FAT32
*  exists precisely for media above the 2 GB FAT16 ceiling, so a byte count
*  could not describe the volumes the format is for. Kibibytes carry 4 TB in
*  the same 32 bits, past what a 32-bit LBA can address at all.
*
*  Renamed rather than quietly re-scaled. A function that keeps its name and
*  changes its unit breaks every caller silently, and two of them in syscall.c
*  compare a byte offset against this -- exactly the arithmetic that would go
*  wrong without a word from the compiler.
*
*  Rounded DOWN, both of them. A free count that rounds up says there is room
*  for a file that will not fit. */
extern uint32_t fat_total_kib(void);
extern uint32_t fat_free_kib(void);
extern uint32_t fat_cluster_bytes(void);

/* Entry number index of a directory, "" or "/" meaning the root.
*  Returns 0 when it filled out, 1 when there are no more entries, and a
*  negative value on error. Deleted and volume label entries are skipped, so
*  indices are dense. */
extern int fat_readdir(const char *path, int index, fat_dirent *out);

/* Size of a file in bytes. Returns 0 on success. */
extern int fat_size(const char *path, uint32_t *size);

/* Reads at most len bytes from offset. Returns the number of bytes read, or
*  negative on error. A short result means end of file. */
extern int fat_read(const char *path, uint32_t offset, uint32_t len, void *buf);

extern const char *fat_last_error(void);

/* ---------------------------------------------------------------------------
*  Writing
*
*  Non-zero when the mounted volume can be written to at all: something is
*  mounted, the drive answered an identify, and the geometry left room to write
*  back. A caller checks this before offering to save anything, because failing
*  after the user has typed a filename is a worse answer than saying so first. */
extern int fat_writable(void);

/* Creates an empty file. Fails if it exists -- overwriting is a decision the
*  caller has to make deliberately, with fat_truncate() or fat_delete().
*
*  The name has to survive the 8.3 conversion: at most eight characters, an
*  optional dot and at most three more, and nothing outside the character set
*  FAT allows. A name that does not is refused rather than mangled, because a
*  file the caller cannot name again is worse than a file that was not created.
*  Returns 0, or negative with fat_last_error() explaining. */
extern int fat_create(const char *path);

/* Writes len bytes at offset, growing the file and its cluster chain as
*  needed. Writing past the end zero-fills the gap, which is what every
*  filesystem does and what a caller seeking forward expects.
*
*  Returns the number of bytes written -- which can be short when the volume
*  fills up, and a caller has to look at the number rather than assume. A
*  partial write leaves a consistent filesystem: the directory entry and the
*  chain describe exactly the bytes that made it. */
extern int fat_write(const char *path, uint32_t offset, uint32_t len,
                     const void *buf);

/* Sets the file's length. Shrinking releases the clusters past the new end;
*  growing zero-fills. Truncating to 0 is how a caller overwrites a file it
*  did not create. Returns 0 or negative. */
extern int fat_truncate(const char *path, uint32_t size);

/* Removes a file and releases its clusters. Refuses a directory: emptying one
*  first is the caller's business, and there is no fat_rmdir() to pair with it.
*  Returns 0 or negative. */
extern int fat_delete(const char *path);

/* Bytes actually written to the disk since mount, for a shell that wants to
*  show that something happened. */
extern uint32_t fat_bytes_written(void);

#endif
