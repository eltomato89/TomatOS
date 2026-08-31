TomatOS on a disk
=================

This volume is read by src/fs/blockdev.c, which reaches either
src/drivers/block/ata.c (ATA PIO, LBA28) or a USB stick, and
by src/fs/fat.c, which handles FAT12, FAT16 and FAT32 and both
reads and writes.

Layout:
  /BIN    user programs, one ELF each
  /DOCS   text files, one level down

The contents come from sysroot/ in the source tree, plus the
programs the build produces. Rebuild it with "make disk".
