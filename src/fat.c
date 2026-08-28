/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: FAT12/FAT16 filesystem, read only.
*
*  Sits directly on ata_read(). Everything this file knows about the volume
*  comes out of sector 0, the BIOS Parameter Block, and is turned once at
*  mount time into the four numbers the rest of the code actually needs:
*  where the FAT starts, where the root directory starts, where the data
*  area starts, and how many data clusters there are.
*
*  Three things in FAT are traps, and each has a comment of its own further
*  down, so only the summary here:
*
*    - The FAT type is NOT the "FAT12"/"FAT16" string in the boot sector.
*      That field is documentation, it is frequently wrong, and Microsoft's
*      own specification says not to look at it. The type follows from the
*      count of data clusters and from nothing else. See fat_mount().
*
*    - The root directory of FAT12/16 is a fixed run of sectors right behind
*      the FATs. It is not a cluster chain and it has no cluster number, so
*      every directory walk here has two shapes. Subdirectories are ordinary
*      chains. See struct fat_dir.
*
*    - A FAT12 entry is twelve bits and straddles byte boundaries. See
*      fat_next_cluster().
*
*  No allocation. Two static 512 byte buffers do all the work: one caches a
*  FAT sector, one caches a data or directory sector. They are separate on
*  purpose - walking a chain alternates between the two areas constantly,
*  and a single buffer would reload a sector on every step. This keeps the
*  promise fat.h makes, that a directory read never touches the heap, and it
*  means the filesystem works before heap_init() has ever run.
*
*  Read only: nothing in here writes a sector. ata_write() is never called.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <string.h>
#include <ata.h>
#include <fat.h>

/* --- On disk constants ---------------------------------------------------- */

/* BIOS Parameter Block, byte offsets into sector 0. Read one byte at a time
*  (see rd16/rd32): the fields are not aligned - BPB_TotSec32 sits at offset
*  32, BPB_RootEntCnt at 17 - and a struct overlay would either need packing
*  or would silently read the wrong bytes. */
#define BPB_BytsPerSec   11
#define BPB_SecPerClus   13
#define BPB_RsvdSecCnt   14
#define BPB_NumFATs      16
#define BPB_RootEntCnt   17
#define BPB_TotSec16     19
#define BPB_FATSz16      22
#define BPB_TotSec32     32
#define BS_BootSig       38     /* 0x29 -> volume id, label and type follow */
#define BS_VolLab        43     /* 11 bytes, space padded                   */

#define BOOT_SIG_OFF     510    /* 0x55 0xAA                                */

/* Directory entry, byte offsets into a 32 byte entry. */
#define DIR_Name          0     /* 11 bytes, 8.3, space padded              */
#define DIR_Attr         11
#define DIR_FstClusLO    26
#define DIR_FileSize     28

#define DIR_ENTRY_SIZE   32
#define DIR_PER_SECTOR   (ATA_SECTOR_SIZE / DIR_ENTRY_SIZE)

/* Attribute bits */
#define ATTR_READ_ONLY   0x01
#define ATTR_HIDDEN      0x02
#define ATTR_SYSTEM      0x04
#define ATTR_VOLUME_ID   0x08
#define ATTR_DIRECTORY   0x10
#define ATTR_ARCHIVE     0x20
/* A long file name entry is marked by exactly this combination, never by a
*  subset - that is what makes it invisible to a driver that only knows 8.3. */
#define ATTR_LONG_NAME   (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)

/* First byte of a directory entry */
#define DIRENT_FREE      0x00   /* this and everything after it is unused   */
#define DIRENT_DELETED   0xE5
#define DIRENT_E5        0x05   /* stands for a literal 0xE5 in the name    */

/* The two cluster counts that separate the three FAT flavours. They are
*  exact and they are not negotiable: a volume with 4084 data clusters is
*  FAT12, one with 4085 is FAT16, and reading either with the other's entry
*  width produces garbage rather than an error. */
#define FAT12_MAX_CLUSTERS  4085
#define FAT16_MAX_CLUSTERS  65525

/* --- Mounted volume ------------------------------------------------------- */

static int      fat_is_mounted = 0;
static int      fat_drive = 0;
static int      fat_bits = 0;           /* 12 or 16                          */

static uint32_t fat_spc = 0;            /* sectors per cluster               */
static uint32_t fat_num_fats = 0;
static uint32_t fat_sec_per_fat = 0;
static uint32_t fat_root_entries = 0;

static uint32_t fat_fat_lba = 0;        /* first sector of the first FAT     */
static uint32_t fat_root_lba = 0;       /* first sector of the root area     */
static uint32_t fat_root_sectors = 0;   /* length of the root area           */
static uint32_t fat_data_lba = 0;       /* sector of cluster 2               */
static uint32_t fat_clusters = 0;       /* number of DATA clusters           */

static char     fat_vol_label[12];
static const char *fat_err = "";

/* Sector caches. fat_buf holds a sector of the FAT, dat_buf a sector of the
*  data area or of the root directory. Valid flags rather than an impossible
*  lba, because sector 0 is a perfectly ordinary lba. */
static uint8_t  fat_buf[ATA_SECTOR_SIZE];
static uint32_t fat_buf_lba = 0;
static int      fat_buf_valid = 0;

static uint8_t  dat_buf[ATA_SECTOR_SIZE];
static uint32_t dat_buf_lba = 0;
static int      dat_buf_valid = 0;

/* A directory in the only two shapes FAT12/16 has. is_root selects between
*  them; the fields of the other shape are then meaningless. */
typedef struct
{
    int      is_root;       /* 1: fixed root area, 0: cluster chain          */
    uint32_t cluster;       /* first cluster, subdirectories only            */
} fat_dir;

/* --- Little endian field access ------------------------------------------- */

static uint32_t rd16(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8);
}

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off]
         | ((uint32_t)p[off + 1] << 8)
         | ((uint32_t)p[off + 2] << 16)
         | ((uint32_t)p[off + 3] << 24);
}

/* --- Cached sector reads -------------------------------------------------- */

static int fat_get_fat_sector(uint32_t lba)
{
    if (fat_buf_valid && fat_buf_lba == lba)
        return 0;

    fat_buf_valid = 0;
    if (ata_read(fat_drive, lba, 1, fat_buf) < 0)
    {
        fat_err = "read error in the FAT";
        return -1;
    }
    fat_buf_lba = lba;
    fat_buf_valid = 1;
    return 0;
}

static int fat_get_data_sector(uint32_t lba)
{
    if (dat_buf_valid && dat_buf_lba == lba)
        return 0;

    dat_buf_valid = 0;
    if (ata_read(fat_drive, lba, 1, dat_buf) < 0)
    {
        fat_err = "read error in the data area";
        return -1;
    }
    dat_buf_lba = lba;
    dat_buf_valid = 1;
    return 0;
}

/* One byte out of the first FAT, by byte offset from its start. Going
*  through this rather than through a sector index is what makes the FAT12
*  straddle case below fall out for free: a twelve bit entry whose two bytes
*  land in different sectors reads exactly like any other. */
static int fat_byte(uint32_t off, uint8_t *out)
{
    uint32_t sec = off / ATA_SECTOR_SIZE;

    if (sec >= fat_sec_per_fat)
    {
        fat_err = "FAT entry outside the FAT";
        return -1;
    }
    if (fat_get_fat_sector(fat_fat_lba + sec) < 0)
        return -1;

    *out = fat_buf[off % ATA_SECTOR_SIZE];
    return 0;
}

/* --- The FAT itself ------------------------------------------------------- */

/* Raw value of FAT entry n.
*
*  FAT16 is a plain array of 16 bit words. FAT12 is the interesting one: the
*  entries are packed three nibbles each, so entry n starts at byte offset
*
*      n + n/2                 (i.e. 1.5 bytes per entry, rounded down)
*
*  and the two bytes there hold one and a half entries. Which half belongs to
*  n depends on the parity of n:
*
*      byte:     |    b0     |    b1     |
*      nibbles:  | L0 |  H0  |  L1 | H1  |
*      even n:   entry = ((b1 & 0x0F) << 8) | b0      -> low 12 bits
*      odd  n:   entry = (b1 << 4) | (b0 >> 4)        -> high 12 bits
*
*  Both cases are the same expression, b0 | (b1 << 8), followed by either
*  masking off the top four bits or shifting them down by four - which is
*  how it is written below, because the two-expression form is where the
*  transcription errors happen.
*
*  Note that off+1 may cross into the next sector of the FAT. fat_byte()
*  handles that; nothing here has to care. */
static int fat_entry(uint32_t n, uint32_t *out)
{
    uint8_t b0, b1;
    uint32_t off, v;

    if (fat_bits == 12)
        off = n + (n / 2);
    else
        off = n * 2;

    if (fat_byte(off, &b0) < 0)
        return -1;
    if (fat_byte(off + 1, &b1) < 0)
        return -1;

    v = (uint32_t)b0 | ((uint32_t)b1 << 8);

    if (fat_bits == 12)
    {
        if (n & 1)
            v >>= 4;            /* odd:  high twelve bits of the pair */
        else
            v &= 0x0FFF;        /* even: low twelve bits of the pair  */
    }

    *out = v;
    return 0;
}

/* End of chain marker. Everything at or above this is a terminator; the one
*  value below it, 0xFF7 / 0xFFF7, marks a bad cluster and is not a valid
*  link either, which the range check in fat_next_cluster() catches. */
static uint32_t fat_eoc_min(void)
{
    return (fat_bits == 12) ? 0x0FF8UL : 0xFFF8UL;
}

static int fat_cluster_valid(uint32_t c)
{
    return (c >= 2 && c <= fat_clusters + 1);
}

static uint32_t fat_cluster_lba(uint32_t c)
{
    return fat_data_lba + (c - 2) * fat_spc;
}

/* Follows one link. Returns 0 and writes *next, 1 at the end of the chain,
*  or a negative value on a read error or a link that cannot be followed. */
static int fat_next_cluster(uint32_t c, uint32_t *next)
{
    uint32_t v;

    if (fat_entry(c, &v) < 0)
        return -1;

    if (v >= fat_eoc_min())
        return 1;

    if (!fat_cluster_valid(v))
    {
        /* Free (0), reserved (1) or bad (0xFF7). None of them may appear in
        *  a chain, and following one would read an arbitrary sector. */
        fat_err = "corrupt cluster chain";
        return -1;
    }

    *next = v;
    return 0;
}

/* Walks steps links forward from c.
*
*  This is the only place a chain is followed, and it is where the loop guard
*  lives. A corrupt FAT can point a cluster at itself, or build a ring of
*  any length; both are perfectly valid FAT entries and a naive walk follows
*  them until the machine is switched off. The bound used is the number of
*  data clusters on the volume: no honest chain can be longer than that,
*  because a cluster appears in at most one chain and at most once in it. So
*  any walk that gets that far has met a cycle, and the walk is refused.
*
*  Returns 0 (arrived, *c updated), 1 (chain ended first) or negative. */
static int fat_walk(uint32_t *c, uint32_t steps)
{
    uint32_t cur = *c;
    uint32_t done = 0;
    int r;

    if (!fat_cluster_valid(cur))
    {
        fat_err = "invalid start cluster";
        return -1;
    }

    while (done < steps)
    {
        if (done > fat_clusters)
        {
            fat_err = "cyclic cluster chain";
            return -1;
        }
        r = fat_next_cluster(cur, &cur);
        if (r != 0)
            return r;
        done++;
    }

    *c = cur;
    return 0;
}

/* --- 8.3 names ------------------------------------------------------------ */

static char fat_upper(char c)
{
    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c;
}

/* Case insensitive comparison of two NUL terminated names. There is no
*  strncasecmp in this kernel and the names are short, so it is spelled out. */
static int fat_name_eq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (fat_upper(*a) != fat_upper(*b))
            return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* Turns the eleven padded bytes of a directory entry into something
*  printable: trailing spaces gone, a dot only when there is an extension.
*  out must hold FAT_NAME_MAX bytes.
*
*  The first byte is special. 0x00 and 0xE5 never reach here - they mean end
*  of directory and deleted entry and are filtered before - but 0x05 does,
*  and it stands for a literal 0xE5 in the name. That substitution exists
*  because 0xE5 is a legal lead byte in several code pages and would
*  otherwise be indistinguishable from a deletion mark. */
static void fat_name_of(const uint8_t *e, char *out)
{
    uint32_t base = 8;
    uint32_t ext = 3;
    uint32_t i;
    uint32_t n = 0;

    while (base > 0 && e[base - 1] == ' ')
        base--;
    while (ext > 0 && e[8 + ext - 1] == ' ')
        ext--;

    for (i = 0; i < base; i++)
        out[n++] = (char)e[i];

    if (n > 0 && (uint8_t)out[0] == DIRENT_E5)
        out[0] = (char)(uint8_t)DIRENT_DELETED;

    if (ext > 0)
    {
        out[n++] = '.';
        for (i = 0; i < ext; i++)
            out[n++] = (char)e[8 + i];
    }

    out[n] = '\0';
}

/* Is this a real 8.3 entry, i.e. not deleted, not a long name fragment and
*  not the volume label? The label check has to come after the long name
*  check: a long name entry has ATTR_VOLUME_ID set as part of its marker. */
static int fat_entry_usable(const uint8_t *e)
{
    uint8_t attr = e[DIR_Attr];

    if (e[0] == DIRENT_DELETED)
        return 0;
    if ((attr & ATTR_LONG_NAME) == ATTR_LONG_NAME)
        return 0;
    if (attr & ATTR_VOLUME_ID)
        return 0;
    return 1;
}

static void fat_fill_dirent(const uint8_t *e, fat_dirent *out)
{
    fat_name_of(e + DIR_Name, out->name);
    out->is_dir = (e[DIR_Attr] & ATTR_DIRECTORY) ? 1 : 0;
    out->first_cluster = rd16(e, DIR_FstClusLO);
    out->size = out->is_dir ? 0 : rd32(e, DIR_FileSize);
}

/* --- Directory access ----------------------------------------------------- */

/* Raw entry number n of a directory, copied into out (32 bytes).
*  Returns 0, 1 when n is past the end of the directory, or negative.
*
*  The whole asymmetry of FAT12/16 lives in this function. The root
*  directory is a fixed run of fat_root_sectors sectors starting at
*  fat_root_lba, with room for exactly fat_root_entries entries and no way
*  to grow. A subdirectory is a file like any other: a cluster chain, walked
*  with fat_walk(), and it ends when the chain ends. */
static int fat_dir_entry(const fat_dir *d, uint32_t n, uint8_t *out)
{
    uint32_t lba;
    uint32_t within;
    uint32_t per_cluster;
    uint32_t cluster;
    int r;

    if (d->is_root)
    {
        if (n >= fat_root_entries)
            return 1;
        if (n / DIR_PER_SECTOR >= fat_root_sectors)
            return 1;
        lba = fat_root_lba + n / DIR_PER_SECTOR;
        within = n % DIR_PER_SECTOR;
    }
    else
    {
        per_cluster = fat_spc * DIR_PER_SECTOR;

        /* Independent of fat_walk()'s own guard: n comes from a caller that
        *  counts upwards, so refuse an entry number no directory on this
        *  volume could have before walking anything at all. */
        if (n / per_cluster > fat_clusters)
        {
            fat_err = "directory too long";
            return -1;
        }

        cluster = d->cluster;
        r = fat_walk(&cluster, n / per_cluster);
        if (r != 0)
            return r;

        within = n % per_cluster;
        lba = fat_cluster_lba(cluster) + within / DIR_PER_SECTOR;
        within = within % DIR_PER_SECTOR;
    }

    if (fat_get_data_sector(lba) < 0)
        return -1;

    memcpy(out, dat_buf + within * DIR_ENTRY_SIZE, DIR_ENTRY_SIZE);
    return 0;
}

/* Entry number index of a directory, counting only usable entries so that
*  the indices a caller sees are dense. Returns 0, 1 past the last entry, or
*  negative. */
static int fat_dir_nth(const fat_dir *d, int index, fat_dirent *out)
{
    uint8_t e[DIR_ENTRY_SIZE];
    uint32_t n = 0;
    int seen = 0;
    int r;

    for (;;)
    {
        r = fat_dir_entry(d, n, e);
        if (r != 0)
            return r;
        n++;

        if (e[0] == DIRENT_FREE)
            return 1;
        if (!fat_entry_usable(e))
            continue;

        if (seen == index)
        {
            fat_fill_dirent(e, out);
            return 0;
        }
        seen++;
    }
}

/* Looks a single 8.3 component up in a directory. Returns 0 when found,
*  1 when not, negative on error. */
static int fat_dir_lookup(const fat_dir *d, const char *name, fat_dirent *out)
{
    uint8_t e[DIR_ENTRY_SIZE];
    char cand[FAT_NAME_MAX];
    uint32_t n = 0;
    int r;

    for (;;)
    {
        r = fat_dir_entry(d, n, e);
        if (r != 0)
            return r;
        n++;

        if (e[0] == DIRENT_FREE)
            return 1;
        if (!fat_entry_usable(e))
            continue;

        fat_name_of(e + DIR_Name, cand);
        if (fat_name_eq(cand, name))
        {
            fat_fill_dirent(e, out);
            return 0;
        }
    }
}

/* --- Paths ---------------------------------------------------------------- */

/* Pulls the next component out of a path, advancing *p past it. Returns 1
*  when a component was produced, 0 at the end of the path, -1 when the
*  component is longer than 8.3 can be. */
static int fat_path_next(const char **p, char *comp)
{
    const char *s = *p;
    int n = 0;

    while (*s == '/')
        s++;
    if (*s == '\0')
    {
        *p = s;
        return 0;
    }

    while (*s != '\0' && *s != '/')
    {
        if (n >= FAT_NAME_MAX - 1)
            return -1;
        comp[n++] = *s++;
    }
    comp[n] = '\0';
    *p = s;
    return 1;
}

/* Resolves an absolute path.
*
*  On success *is_root says whether the path denotes the root directory
*  itself - "" and "/" do - in which case out is not touched, because the
*  root has no directory entry anywhere to describe it. Otherwise out
*  describes the final component.
*
*  Returns 0, 1 when some component does not exist, negative on error. */
static int fat_resolve(const char *path, fat_dirent *out, int *is_root)
{
    char comp[FAT_NAME_MAX];
    fat_dir dir;
    const char *p = path;
    int r;

    dir.is_root = 1;
    dir.cluster = 0;
    *is_root = 1;

    if (path == 0)
    {
        fat_err = "no path given";
        return -1;
    }

    for (;;)
    {
        r = fat_path_next(&p, comp);
        if (r < 0)
        {
            fat_err = "path component is not a valid 8.3 name";
            return -1;
        }
        if (r == 0)
            return 0;                   /* path exhausted */

        /* A component after something that is not a directory cannot
        *  resolve, and neither can one after the first miss. */
        if (!*is_root)
        {
            if (!out->is_dir)
            {
                fat_err = "path component is not a directory";
                return -1;
            }
            /* ".." in a first level subdirectory carries cluster 0, which
            *  is FAT's way of saying "the root" - it has no cluster. */
            if (out->first_cluster == 0)
            {
                dir.is_root = 1;
                dir.cluster = 0;
            }
            else
            {
                dir.is_root = 0;
                dir.cluster = out->first_cluster;
            }
        }

        r = fat_dir_lookup(&dir, comp, out);
        if (r != 0)
        {
            if (r > 0)
                fat_err = "no such file or directory";
            return r;
        }
        *is_root = 0;
    }
}

/* The directory a path names, for readdir. */
static int fat_dir_of(const char *path, fat_dir *d)
{
    fat_dirent de;
    int is_root;
    int r;

    r = fat_resolve(path, &de, &is_root);
    if (r != 0)
        return r;

    if (is_root)
    {
        d->is_root = 1;
        d->cluster = 0;
        return 0;
    }
    if (!de.is_dir)
    {
        fat_err = "not a directory";
        return -1;
    }
    if (de.first_cluster == 0)
    {
        d->is_root = 1;
        d->cluster = 0;
        return 0;
    }
    d->is_root = 0;
    d->cluster = de.first_cluster;
    return 0;
}

/* --- Mounting ------------------------------------------------------------- */

static void fat_reset(void)
{
    fat_is_mounted = 0;
    fat_bits = 0;
    fat_spc = 0;
    fat_num_fats = 0;
    fat_sec_per_fat = 0;
    fat_root_entries = 0;
    fat_fat_lba = 0;
    fat_root_lba = 0;
    fat_root_sectors = 0;
    fat_data_lba = 0;
    fat_clusters = 0;
    fat_buf_valid = 0;
    dat_buf_valid = 0;
    fat_vol_label[0] = '\0';
}

int fat_mount(int drive)
{
    uint8_t bs[ATA_SECTOR_SIZE];
    uint32_t bytes_per_sec;
    uint32_t reserved;
    uint32_t total_sectors;
    uint32_t root_bytes;
    uint32_t meta_sectors;
    uint32_t data_sectors;
    uint32_t i;
    int last;

    fat_reset();
    fat_drive = drive;
    fat_err = "";

    if (drive < 0 || drive >= ATA_MAX_DRIVES)
    {
        fat_err = "no such drive";
        return -1;
    }
    if (ata_read(drive, 0, 1, bs) < 0)
    {
        fat_err = "cannot read the boot sector";
        return -1;
    }

    if (bs[BOOT_SIG_OFF] != 0x55 || bs[BOOT_SIG_OFF + 1] != 0xAA)
    {
        fat_err = "no 0x55AA boot signature - not a FAT volume";
        return -1;
    }

    /* Everything below assumes 512 byte sectors, from the sector caches to
    *  DIR_PER_SECTOR to the ATA layer itself. A volume formatted with a
    *  different sector size is refused rather than mounted with every
    *  offset quietly wrong. */
    bytes_per_sec = rd16(bs, BPB_BytsPerSec);
    if (bytes_per_sec != ATA_SECTOR_SIZE)
    {
        fat_err = "bytes per sector is not 512 - unsupported";
        return -1;
    }

    fat_spc = bs[BPB_SecPerClus];
    if (fat_spc == 0)
    {
        fat_err = "sectors per cluster is zero - not a FAT volume";
        return -1;
    }

    reserved = rd16(bs, BPB_RsvdSecCnt);
    if (reserved == 0)
    {
        fat_err = "reserved sector count is zero - not a FAT volume";
        return -1;
    }

    fat_num_fats = bs[BPB_NumFATs];
    if (fat_num_fats == 0)
    {
        fat_err = "no FAT on the volume";
        return -1;
    }

    fat_root_entries = rd16(bs, BPB_RootEntCnt);
    fat_sec_per_fat = rd16(bs, BPB_FATSz16);

    /* A zero BPB_FATSz16 means the size lives in BPB_FATSz32, which only
    *  FAT32 has - and FAT32 is out of scope here, so this is a rejection
    *  rather than a second place to look. */
    if (fat_sec_per_fat == 0)
    {
        fat_err = "FAT size is zero - looks like FAT32, unsupported";
        return -1;
    }

    /* Total sectors sits in one of two fields. The 16 bit one is used when
    *  it fits; when it does not, it is zero and the 32 bit one carries the
    *  value. A volume that has neither is not usable. */
    total_sectors = rd16(bs, BPB_TotSec16);
    if (total_sectors == 0)
        total_sectors = rd32(bs, BPB_TotSec32);
    if (total_sectors == 0)
    {
        fat_err = "total sector count is zero - not a FAT volume";
        return -1;
    }

    /* Layout. Root directory entries are 32 bytes each and the area is
    *  rounded up to whole sectors; on FAT12/16 it always divides evenly in
    *  practice, but the rounding costs nothing and a malformed BPB should
    *  not shift the data area by half a sector. */
    root_bytes = fat_root_entries * DIR_ENTRY_SIZE;
    fat_root_sectors = (root_bytes + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;

    fat_fat_lba = reserved;
    fat_root_lba = reserved + fat_num_fats * fat_sec_per_fat;
    fat_data_lba = fat_root_lba + fat_root_sectors;

    meta_sectors = fat_data_lba;
    if (meta_sectors >= total_sectors)
    {
        fat_err = "boot sector describes a volume with no data area";
        return -1;
    }

    /* --- The one calculation that decides the FAT type -------------------
    *
    *  Not the "FAT12"/"FAT16" string at offset 54. That field is advisory,
    *  is wrong on plenty of real media, and Microsoft's own specification
    *  says explicitly that it must not be used to determine the type. The
    *  type is a function of the number of data clusters and of nothing
    *  else:
    *
    *      data sectors  = total - reserved - FATs - root directory
    *      data clusters = data sectors / sectors per cluster
    *
    *      < 4085   -> FAT12
    *      < 65525  -> FAT16
    *      otherwise   FAT32, which this driver does not implement
    *
    *  The division truncates, and that is correct: a trailing partial
    *  cluster is not addressable and is not counted. */
    data_sectors = total_sectors - meta_sectors;
    fat_clusters = data_sectors / fat_spc;

    if (fat_clusters == 0)
    {
        fat_err = "volume has no data clusters";
        return -1;
    }
    if (fat_clusters < FAT12_MAX_CLUSTERS)
        fat_bits = 12;
    else if (fat_clusters < FAT16_MAX_CLUSTERS)
        fat_bits = 16;
    else
    {
        fat_bits = 0;
        fat_err = "too many clusters - this is FAT32, unsupported";
        return -1;
    }

    /* The FAT has to be big enough for the entries the cluster count
    *  implies, or fat_byte() would refuse links near the end of the volume
    *  and a file would break off for no visible reason. Entries 0 and 1 are
    *  reserved and are counted. */
    {
        uint32_t need_bytes;

        if (fat_bits == 12)
            need_bytes = ((fat_clusters + 2) * 3 + 1) / 2;
        else
            need_bytes = (fat_clusters + 2) * 2;

        if (fat_sec_per_fat * (uint32_t)ATA_SECTOR_SIZE < need_bytes)
        {
            fat_err = "FAT is too small for the cluster count";
            fat_bits = 0;
            return -1;
        }
    }

    /* Volume label out of the extended boot record, if there is one. */
    fat_vol_label[0] = '\0';
    if (bs[BS_BootSig] == 0x29)
    {
        last = -1;
        for (i = 0; i < 11; i++)
        {
            fat_vol_label[i] = (char)bs[BS_VolLab + i];
            if (fat_vol_label[i] != ' ')
                last = (int)i;
        }
        fat_vol_label[last + 1] = '\0';
    }

    fat_is_mounted = 1;
    return 0;
}

int fat_mounted(void)
{
    return fat_is_mounted;
}

const char *fat_type(void)
{
    if (!fat_is_mounted)
        return "";
    return (fat_bits == 12) ? "FAT12" : "FAT16";
}

const char *fat_label(void)
{
    if (!fat_is_mounted)
        return "";
    return fat_vol_label;
}

/* --- Sizes ---------------------------------------------------------------- */

uint32_t fat_cluster_bytes(void)
{
    if (!fat_is_mounted)
        return 0;
    return fat_spc * (uint32_t)ATA_SECTOR_SIZE;
}

/* clusters * cluster size, saturating. A FAT16 volume with 64 sectors per
*  cluster is already at 2 GiB, and there is no 64 bit arithmetic here, so
*  the multiplication is checked instead of widened. */
static uint32_t fat_bytes_of(uint32_t clusters)
{
    uint32_t cb = fat_cluster_bytes();

    if (cb == 0)
        return 0;
    if (clusters > 0xFFFFFFFFUL / cb)
        return 0xFFFFFFFFUL;
    return clusters * cb;
}

uint32_t fat_total_bytes(void)
{
    if (!fat_is_mounted)
        return 0;
    return fat_bytes_of(fat_clusters);
}

/* Counts free clusters by scanning the FAT. Entries 0 and 1 are reserved
*  and are not part of the data area, so the scan runs over the cluster
*  numbers 2 .. fat_clusters + 1. Sequential, so the FAT sector cache turns
*  the whole scan into one pass over the FAT. */
uint32_t fat_free_bytes(void)
{
    uint32_t c;
    uint32_t v;
    uint32_t free_clusters = 0;

    if (!fat_is_mounted)
        return 0;

    for (c = 2; c <= fat_clusters + 1; c++)
    {
        if (fat_entry(c, &v) < 0)
            return 0;
        if (v == 0)
            free_clusters++;
    }
    return fat_bytes_of(free_clusters);
}

/* --- Public directory and file access ------------------------------------- */

int fat_readdir(const char *path, int index, fat_dirent *out)
{
    fat_dir d;
    int r;

    if (!fat_is_mounted)
    {
        fat_err = "no volume mounted";
        return -1;
    }
    if (out == 0 || index < 0)
    {
        fat_err = "invalid argument";
        return -1;
    }

    fat_err = "";
    r = fat_dir_of(path, &d);
    if (r != 0)
        return (r > 0) ? -1 : r;    /* a missing directory is an error here */

    return fat_dir_nth(&d, index, out);
}

int fat_size(const char *path, uint32_t *size)
{
    fat_dirent de;
    int is_root;
    int r;

    if (!fat_is_mounted)
    {
        fat_err = "no volume mounted";
        return -1;
    }
    if (size == 0)
    {
        fat_err = "invalid argument";
        return -1;
    }

    fat_err = "";
    r = fat_resolve(path, &de, &is_root);
    if (r != 0)
        return -1;
    if (is_root || de.is_dir)
    {
        fat_err = "is a directory";
        return -1;
    }

    *size = de.size;
    return 0;
}

int fat_read(const char *path, uint32_t offset, uint32_t len, void *buf)
{
    fat_dirent de;
    uint32_t cluster;
    uint32_t cluster_bytes;
    uint32_t pos;                   /* byte offset inside the cluster */
    uint32_t done = 0;
    uint32_t chunk;
    int is_root;
    int r;

    if (!fat_is_mounted)
    {
        fat_err = "no volume mounted";
        return -1;
    }
    if (buf == 0)
    {
        fat_err = "invalid argument";
        return -1;
    }

    fat_err = "";
    r = fat_resolve(path, &de, &is_root);
    if (r != 0)
        return -1;
    if (is_root || de.is_dir)
    {
        fat_err = "is a directory";
        return -1;
    }

    if (offset >= de.size)
        return 0;                   /* nothing left, a short read of zero */
    if (len > de.size - offset)
        len = de.size - offset;
    if (len == 0)
        return 0;
    /* The return type is int, so a single call can never report more than
    *  this many bytes even if the caller asks for more. */
    if (len > 0x7FFFFFFFUL)
        len = 0x7FFFFFFFUL;

    if (de.first_cluster == 0)
    {
        fat_err = "file has a size but no cluster";
        return -1;
    }

    cluster_bytes = fat_spc * (uint32_t)ATA_SECTOR_SIZE;
    cluster = de.first_cluster;

    /* Skip whole clusters to reach the offset. fat_walk() carries the cycle
    *  guard; a chain that ends before the offset means the directory entry
    *  claims a size the chain does not back up. */
    r = fat_walk(&cluster, offset / cluster_bytes);
    if (r != 0)
    {
        if (r > 0)
            fat_err = "cluster chain shorter than the file size";
        return -1;
    }
    pos = offset % cluster_bytes;

    /* One pass, one sector at a time, advancing the chain as it goes - the
    *  chain is never re-walked from the front, so a long file costs one FAT
    *  lookup per cluster rather than one per sector. */
    while (done < len)
    {
        if (fat_get_data_sector(fat_cluster_lba(cluster)
                                + pos / ATA_SECTOR_SIZE) < 0)
            return -1;

        chunk = ATA_SECTOR_SIZE - (pos % ATA_SECTOR_SIZE);
        if (chunk > len - done)
            chunk = len - done;

        memcpy((uint8_t *)buf + done, dat_buf + (pos % ATA_SECTOR_SIZE),
               chunk);
        done += chunk;
        pos += chunk;

        if (pos >= cluster_bytes && done < len)
        {
            r = fat_next_cluster(cluster, &cluster);
            if (r != 0)
            {
                if (r > 0)
                    fat_err = "cluster chain shorter than the file size";
                return -1;
            }
            pos = 0;
        }
    }

    return (int)done;
}

const char *fat_last_error(void)
{
    return fat_err;
}
