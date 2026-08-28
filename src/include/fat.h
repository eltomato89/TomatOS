/* TomatOS - FAT12/FAT16 filesystem, read only for now
*  Desc: Enough to list a directory and read a file off a real disk.
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

/* Reads the boot sector of the given drive and works out the layout.
*  Returns 0 on success, negative if the drive holds nothing recognisable. */
extern int fat_mount(int drive);

extern int fat_mounted(void);

/* "FAT12" or "FAT16", or "" when nothing is mounted. */
extern const char *fat_type(void);

/* Volume label from the boot sector, or "" if it has none. */
extern const char *fat_label(void);

extern uint32_t fat_total_bytes(void);
extern uint32_t fat_free_bytes(void);
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

#endif
