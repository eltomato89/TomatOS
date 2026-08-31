/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: FAT12/FAT16/FAT32 filesystem, reading and writing.
*
*  Sits on blk_read() and blk_write(), and on nothing more specific than
*  that. It used to call ata_read() and ata_write() by name, which was honest
*  while ATA was the only way to reach a sector; a USB stick is a block device
*  too, and it answers through four layers that have nothing in common with an
*  IDE controller. So the drive number this file carries is a BLOCK DEVICE
*  number now: 0..3 still mean exactly the ATA drives they always did, and 4
*  and up can be a stick, and nothing below cares which.
*
*  Everything this file knows about the volume comes out of sector 0, the BIOS
*  Parameter Block, and is turned once at mount time into the four numbers the
*  rest of the code actually needs: where the FAT starts, where the root
*  directory starts, where the data area starts, and how many data clusters
*  there are.
*
*  Those four numbers are derived exactly once, in fat_mount(). The write
*  half below reuses them and never recomputes an offset of its own: two
*  derivations of where the FAT begins is the bug that eats a volume, and
*  the second one is always the one nobody tested.
*
*  A FAT32 volume adds two more to that list and neither is an exception to
*  it: the first cluster of the root directory, which is where a directory
*  walk starts instead of at a fixed sector, and the sector holding FSInfo.
*  Both come out of the same BPB read at the same moment as the other four.
*
*  Four things in FAT are traps, and each has a comment of its own further
*  down, so only the summary here:
*
*    - The FAT type is NOT the "FAT12"/"FAT16"/"FAT32" string in the boot
*      sector. That field is documentation, it is frequently wrong - several
*      formatters stamp "FAT32" into volumes that are nothing of the sort -
*      and Microsoft's own specification says not to look at it. The type
*      follows from the count of data clusters and from nothing else. One
*      calculation answers both boundaries, 4085 and 65525. See fat_mount().
*
*    - The root directory of FAT12/16 is a fixed run of sectors right behind
*      the FATs. It is not a cluster chain and it has no cluster number, so
*      a directory walk on those two has two shapes. On FAT32 it is an
*      ordinary chain starting at BPB_RootClus, which REMOVES the second
*      shape rather than adding a third. See struct fat_dir and
*      fat_dir_root().
*
*    - A FAT12 entry is twelve bits and straddles byte boundaries. See
*      fat_next_cluster().
*
*    - A FAT32 entry is thirty-two bits of which only the low TWENTY-EIGHT
*      are the cluster number. The top four belong to whoever wrote the
*      volume and must come back unchanged, so every write to one is a
*      read-modify-write. See fat_set_entry(), where the mask that does it
*      is the only difference from FAT16.
*
*  No allocation. Two static 512 byte buffers do all the work: one caches a
*  FAT sector, one caches a data or directory sector. They are separate on
*  purpose - walking a chain alternates between the two areas constantly,
*  and a single buffer would reload a sector on every step. This keeps the
*  promise fat.h makes, that a directory read never touches the heap, and it
*  means the filesystem works before heap_init() has ever run.
*
*  BEING STATIC, they are also the reason there is a lock in this file now.
*  Two tasks in here at once share them, and one can be handed the other's
*  sector. Against ATA that needed an unlucky preemption; against a device
*  whose driver sleeps waiting for a transfer it is what happens every time.
*  See the lock section below the buffers for the shape of it and for what
*  becomes of a lock whose owner is killed while holding it.
*
*  That split used to be worth a few hundred microseconds of PIO per step.
*  On a removable device it is worth twenty times that: a stick on USB 1.1
*  moves roughly a megabyte a second, and every sector this driver asks for
*  costs a whole bulk transaction rather than a burst of inw. Nothing about
*  the caches changes because of that - they were already the difference
*  between one read per step and two - but the reason they are here is no
*  longer "it is a bit faster".
*
*  --- Writing ---------------------------------------------------------------
*
*  This is the first code in the kernel that can DESTROY data. Everything
*  before it was additive; a wrong sector write takes a file with it, and on
*  real hardware it takes somebody's file. Four rules run through the whole
*  write half, and every one of them costs something:
*
*    1. Every sector write goes out through the buffer that caches it.
*       A write that bypassed the cache would leave dat_buf or fat_buf
*       describing a sector that no longer exists on the disk, and the next
*       read would hand back the old bytes. So a write is always: load the
*       sector into its cache, modify it there, push it back. The one
*       exception is a full 512 byte overwrite, where the load is skipped
*       and the cache is simply claimed for that lba - see fat_claim_data().
*
*    2. Every allocator change lands in EVERY FAT copy, backups first,
*       the primary last. See fat_put_fat_sector().
*
*    3. A cluster is marked used BEFORE anything is written into it, and is
*       zero filled before it is linked into a chain. Both orders are the
*       cautious one: a crash can then only ever leak a cluster, never let
*       two files share one. Leaked clusters are one "fsck -r" away; a
*       cross linked pair is not recoverable at all.
*
*    4. The directory entry is the commit point, and it is written at the
*       moment that makes the file describe the SMALLER of the two states.
*       Growing: data, then chain, then directory entry. Shrinking and
*       deleting: directory entry first, then release the clusters. Either
*       way a power cut leaves the file at its old extent or shorter, with
*       at worst some unreferenced clusters. There is no journal here and
*       there will not be one; the ordering is all the protection there is.
*
*  All four are statements about what is ON THE MEDIUM when the next write is
*  issued, and until recently none of them said so, because against ATA it was
*  free: ata_transfer() ends every write with a cache flush. A stick
*  acknowledges a SCSI WRITE when it has the data rather than when the flash
*  does, so on removable media the rules were the order the writes were ASKED
*  for and nothing more. fat_sync() is what puts the guarantee back, at the
*  four transitions the rules above actually name - see the comment
*  above it, which also says what a flush that FAILS means and why every
*  caller answers it by stopping short of the dangerous step.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <string.h>
#include <blockdev.h>
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

/* The FAT32 half of the BPB. Everything from offset 36 on is a DIFFERENT
*  structure on a FAT32 volume than on a FAT12/16 one - the extended boot
*  record simply starts later, because the four fields below were pushed in
*  front of it. So BS_BootSig and BS_VolLab move too, and reading the
*  FAT12/16 pair on a FAT32 volume lands in the middle of BPB_RootClus. */
#define BPB_FATSz32      36
#define BPB_ExtFlags     40
#define BPB_FSVer        42
#define BPB_RootClus     44
#define BPB_FSInfo       48
#define BS32_BootSig     66
#define BS32_VolLab      71

/* FSInfo, byte offsets into its own sector. The two signatures are what
*  make it worth reading at all: a sector that does not carry both of them
*  is not FSInfo, whatever BPB_FSInfo said, and the free count in it is then
*  somebody else's bytes rather than a number. */
#define FSI_LeadSig       0     /* 0x41615252, "RRaA"                       */
#define FSI_StrucSig    484     /* 0x61417272, "rrAa"                       */
#define FSI_Free_Count  488     /* 0xFFFFFFFF -> not known                  */
#define FSI_Nxt_Free    492     /* 0xFFFFFFFF -> no hint                    */
#define FSI_TrailSig    508     /* 0xAA550000                               */

#define FSI_LEAD_SIG    0x41615252UL
#define FSI_STRUC_SIG   0x61417272UL
#define FSI_TRAIL_SIG   0xAA550000UL
#define FSI_UNKNOWN     0xFFFFFFFFUL

#define BOOT_SIG_OFF     510    /* 0x55 0xAA                                */

/* Directory entry, byte offsets into a 32 byte entry. Everything from
*  DIR_NTRes on is only ever touched by the write half; the read path needs
*  the name, the attribute, the first cluster and the size and nothing else. */
#define DIR_Name          0     /* 11 bytes, 8.3, space padded              */
#define DIR_Attr         11
#define DIR_NTRes        12     /* reserved, must be written as zero        */
#define DIR_CrtTimeTenth 13
#define DIR_CrtTime      14
#define DIR_CrtDate      16
#define DIR_LstAccDate   18
#define DIR_FstClusHI    20     /* FAT32 only; must be zero on FAT12/16     */
#define DIR_WrtTime      22
#define DIR_WrtDate      24
#define DIR_FstClusLO    26
#define DIR_FileSize     28

#define DIR_ENTRY_SIZE   32
#define DIR_PER_SECTOR   (BLK_SECTOR_SIZE / DIR_ENTRY_SIZE)

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
*  FAT12, one with 4085 is FAT16, one with 65524 is still FAT16 and one with
*  65525 is FAT32, and reading any of them with another's entry width
*  produces garbage rather than an error.
*
*  The second boundary is the entire reason FAT32 is in this file. 65524
*  clusters is where FAT16 stops, which at the largest cluster size anybody
*  formats with is 2 GB - so every stick above that is FAT32 whatever the
*  label on it says, and a driver without this could only read media this
*  kernel had formatted itself. */
#define FAT12_MAX_CLUSTERS  4085
#define FAT16_MAX_CLUSTERS  65525

/* The part of a FAT32 entry that is a cluster number. The other four bits
*  are reserved, they are not ours, and the two places that touch them are
*  fat_entry(), which masks them away on the way in, and fat_set_entry(),
*  which leaves them alone on the way out. Everything in between works on 28
*  bit numbers and cannot get this wrong because it never sees the bits.
*
*  Overwriting them is the classic FAT32 bug precisely because nothing
*  complains: the volume mounts, files read back, fsck passes, and a volume
*  another driver wrote has had four bits per entry silently zeroed. */
#define FAT32_MASK  0x0FFFFFFFUL

/* --- Mounted volume ------------------------------------------------------- */

static int      fat_is_mounted = 0;
static int      fat_drive = 0;          /* block device number, not an ATA   */
                                        /* drive - see blockdev.h            */
static int      fat_bits = 0;           /* 12, 16 or 32                      */

static uint32_t fat_spc = 0;            /* sectors per cluster               */
static uint32_t fat_num_fats = 0;       /* copies the LAYOUT has room for    */
static uint32_t fat_sec_per_fat = 0;
static uint32_t fat_root_entries = 0;   /* zero on FAT32                     */

static uint32_t fat_fat_lba = 0;        /* first sector of the ACTIVE FAT    */
static uint32_t fat_fat_copies = 0;     /* copies that are actually mirrored */
static uint32_t fat_root_lba = 0;       /* first sector of the root area     */
static uint32_t fat_root_sectors = 0;   /* length of the root area           */
static uint32_t fat_root_cluster = 0;   /* FAT32: the root chain starts here */
static uint32_t fat_data_lba = 0;       /* sector of cluster 2               */
static uint32_t fat_clusters = 0;       /* number of DATA clusters           */
static uint32_t fat_total_sectors = 0;  /* what the BPB claims the volume is */

static char     fat_vol_label[12];
static const char *fat_err = "";

/* Where the next search for a free cluster starts. Purely a hint: it is
*  never trusted, the scan validates every entry it looks at and wraps, and
*  a wrong value costs a slower search and nothing else. Kept because
*  without it every allocation of a multi cluster file rescans the FAT from
*  cluster 2, which turns writing one file into a quadratic walk. */
static uint32_t fat_alloc_hint = 2;

/* --- The free cluster count ------------------------------------------------
*
*  How many clusters read as free, once somebody has been made to find out.
*
*  Counting them means reading the WHOLE FAT, which the comment above
*  fat_count_free() calls the most expensive thing in this file: on a stick a
*  FAT16 volume with a megabyte of FAT is two thousand bulk round trips, and a
*  FAT32 stick has more FAT than that. So the number is counted once and then
*  KEPT, and every allocation and every release adjusts it by one. Those are
*  the only two operations in the file that change how many clusters are free,
*  and both of them already hold the old value of the entry they are about to
*  write, so the bookkeeping costs nothing at all.
*
*  fat_free_known is what separates "zero clusters are free" from "nobody has
*  counted". It goes back to zero whenever a FAT write failed in a way that
*  leaves this driver unable to say whether the entry changed - the next
*  question then pays for a fresh scan, which is the honest price of not
*  knowing.
*
*  On FAT32 the first value can come from FSInfo instead of from a scan, which
*  is the whole reason FSInfo is read - see fat_fsinfo_load(). The mechanism
*  itself is the same on all three formats deliberately: a counter that only
*  ran on the format nobody boots is a counter whose errors are found by
*  somebody else. */
static uint32_t fat_free_count = 0;
static int      fat_free_known = 0;

/* FSInfo, on the volumes that have one. Sector 0 is never FSInfo - that is
*  the boot sector - so zero doubles as "this volume has none", the same way
*  the valid flags on the sector caches avoid an impossible lba. */
static uint32_t fat_fsinfo_lba = 0;

/* Set when the count or the hint has moved since FSInfo was last written, so
*  that a run of operations that allocate nothing writes nothing. */
static int      fat_fsinfo_dirty = 0;

/* Bytes handed to blk_write() since the volume was mounted. This counts
*  METADATA too - the FAT copies and the directory entry are writes to the
*  medium like any other, and a counter that hid them would understate what
*  this driver has done to somebody's disk. Saving ten bytes therefore shows
*  up as several kilobytes, and that is the honest number.
*
*  Not the same number as blk_bytes_written(), and deliberately kept next to
*  it rather than replaced by it: that one counts every device since boot and
*  includes whatever else wrote, this one counts what this volume was made to
*  swallow since it was mounted. */
static uint32_t fat_written = 0;

/* Set when a FAT update failed part way through the copies, so that the
*  copies on the medium may no longer agree. Once that has happened the
*  driver stops writing to this volume entirely until it is mounted again:
*  see fat_writable(). Piling further changes onto a filesystem whose
*  bookkeeping is already known to be inconsistent is how a recoverable
*  problem turns into an unrecoverable one, and refusing to write is the one
*  action that cannot make anything worse. */
static int fat_write_locked = 0;

/* Sector caches. fat_buf holds a sector of the FAT, dat_buf a sector of the
*  data area or of the root directory. Valid flags rather than an impossible
*  lba, because sector 0 is a perfectly ordinary lba. */
static uint8_t  fat_buf[BLK_SECTOR_SIZE];
static uint32_t fat_buf_lba = 0;
static int      fat_buf_valid = 0;

static uint8_t  dat_buf[BLK_SECTOR_SIZE];
static uint32_t dat_buf_lba = 0;
static int      dat_buf_valid = 0;

/* --- One task at a time ----------------------------------------------------
*
*  WHY THIS IS HERE NOW, when it was not before. Everything in this file works
*  out of the two static sector caches above and the geometry statics above
*  them, and until recently that was survivable for a reason nobody had
*  written down: ATA NEVER YIELDS. A transfer against an IDE controller is a
*  bounded spin inside ata.c, so two tasks could only meet in the middle of
*  one if the timer preempted the first at exactly the wrong instruction.
*  Possible, rare, and never seen.
*
*  A USB stick took that accident away. usbmsc.c waits for a transfer with
*  sleep(), which BLOCKS the task, so the scheduler is guaranteed to elect
*  somebody else in the middle of every single sector operation. The
*  interleaving that used to need bad luck is now what happens every time.
*
*  And it is reachable from two directions at once: twenty-four call sites in
*  syscall.c, which is ring 3, and sixteen in main.c, which is the shell. The
*  shell listing a directory while a program reads a file is two tasks in this
*  file, and a shared dat_buf means one of them can be handed the other's
*  sector - dat_buf_valid says a sector is cached and dat_buf_lba says which,
*  and neither says who put it there. That is not a slow filesystem, it is a
*  wrong one, and on the write half it is a wrong one that writes.
*
*  So: one task inside the sector caches at a time.
*
*  WHAT IT IS. A flag and the pid that owns it, taken and given back with
*  interrupts off. syscall.c has the closest relative on the machine - the
*  resolver lock around sysnet_dns_take() - and this one deliberately differs
*  from it in one place. That one takes the flag with an xchg and then records
*  the owner in a second store, which leaves a window: a task preempted
*  between the two holds a lock whose owner still reads as whatever the last
*  release left, and a sweep like the one below would look at that stale pid,
*  find it dead and break a lock that was just legitimately taken. On one CPU
*  cli is both stronger and simpler than xchg, and it makes the flag and its
*  owner ONE indivisible update, so the window does not exist. There is no
*  second processor for xchg to defend against here; there is a timer.
*
*  WHAT IT PROTECTS. fat_buf and dat_buf and their validity flags, the
*  geometry fat_mount() derives, fat_alloc_hint, and the read-only latch. Not
*  fat_err - see fat_last_error() at the bottom of the read half.
*
*  WHO DOES NOT TAKE IT, and this is a decision rather than an oversight:
*  fat_mounted(), fat_type(), fat_label(), fat_cluster_bytes(),
*  fat_total_kib(), fat_bytes_written() and fat_last_error() read statics
*  and do arithmetic on them. They touch no cache, issue no transfer and can
*  therefore not be interleaved into anything: the worst a preemption costs
*  them is an answer that was true a moment ago. Making them block would put a
*  status line behind a file copy for no gain at all. */

/* Longest anybody waits to get in before being told the filesystem is busy.
*
*  This is a backstop and not the recovery mechanism - fat_lock_reap() below
*  is that, and it deals with the case this bound cannot: an owner that is
*  never going to run again. What is left for the bound is an owner that is
*  alive but stuck, which today means a driver retrying against a device that
*  has stopped answering, and for that the only useful behaviour is to give
*  the waiter an error rather than a machine that appears to have died.
*
*  Fifteen seconds is chosen above the longest honest hold and below the point
*  where a user concludes the machine has crashed. The longest honest hold is
*  fat_free_kib() on a stick, on the one call that has to count: a FAT16
*  volume with a megabyte of FAT is two thousand bulk round trips and takes
*  seconds, which is written down at fat_count_free() and is exactly the
*  number this has to sit above. A FAT32 stick has more FAT than that, and the
*  cached count and FSInfo are what keep it from being paid twice rather than
*  a reason to raise this. It is the same reasoning, and coincidentally the
*  same value, as SYS_NET_RECV_MS.
*
*  FAT_LOCK_POLL_MS is not a polling interval in the old sense - a release
*  wakes the channel, so a waiter normally never reaches it. It is the bound
*  on ONE wait, and it exists so that a waiter comes back to fat_lock_reap()
*  at a known rate. A lock left behind by a task that died wakes nobody, so a
*  wait with no bound at all would be a wait for a wake that is never coming;
*  a quarter of a second is how long that case can last. */
#define FAT_LOCK_MS       15000
#define FAT_LOCK_POLL_MS    250

/* Not volatile, and that is an argument rather than an omission. Every read
*  of either sits inside a cli block whose inline assembly carries a "memory"
*  clobber, and the one loop that re-reads fat_lock across a point where it
*  can change - fat_lock_wait() - does so around a call to task_wait(), which
*  is external and is handed &fat_lock. The compiler therefore has to assume
*  both may have been written and reloads them. volatile would say the same
*  thing less precisely and would force a cast at every call that takes the
*  address as a channel. */
static int fat_lock = 0;
static int fat_lock_owner = -1;

/* Interrupts off, and back to what the caller had. net.c, kb.c, uhci.c and
*  syscall.c each carry a pair of these and every one of them is static to its
*  file; there is no global one, so this file carries its own rather than
*  inventing a cross-file interface for six lines of inline assembly.
*
*  pushfl before cli, so a caller that already had interrupts off gets them
*  back off. That matters here: a system call arrives through a TRAP gate and
*  therefore with IF set, but the shell can reach this file from anywhere. */
static unsigned long fat_irq_save(void)
{
    unsigned long flags;

    __asm__ __volatile__ ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
    return flags;
}

static void fat_irq_restore(unsigned long flags)
{
    __asm__ __volatile__ ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

/* Milliseconds since a reading of timer_get_ticks(), which counts in
*  milliseconds of uptime and wraps. The unsigned subtraction is correct
*  across the wrap and the signed one is not, which is the same argument
*  tasks.c makes above time_reached(). */
static uint32_t fat_ms_since(uint32_t start)
{
    return (uint32_t)timer_get_ticks() - start;
}

/* Is the task that holds the lock never going to give it back?
*
*  EXITED counts as well as ABORTED, for the reason sysnet_reap() gives about
*  connections: a program that returned without finishing what it started
*  holds the lock exactly as firmly as one that faulted, and it is the more
*  ordinary of the two. SUSPENDED does not count - a suspended task is alive,
*  the shell suspends tasks, and taking the lock away from one would be this
*  file manufacturing the corruption it exists to prevent.
*
*  A holder with no task behind it is the kernel itself: fat_mount() runs on
*  the boot path before the shell exists, and taskmgr_get_currpid() answers -1
*  there. That holder cannot be aborted out from under the lock, because there
*  is no slot to abort - if it faults the machine panics - so it is never
*  reaped. Without this test taskmgr_task_state(-1) would answer
*  TASK_STATE_NULL and every such holder would read as dead. */
static int fat_lock_dead(int pid)
{
    int state;

    if (pid < 0)
        return 0;

    state = taskmgr_task_state(pid);

    return (state == TASK_STATE_ABORTED
         || state == TASK_STATE_EXITED
         || state == TASK_STATE_NULL);
}

/* Breaks a lock whose owner is not going to give it back.
*
*  A task can stop running between any two instructions: a page fault aborts
*  it, taskmgr_killall() aborts it, and neither runs another instruction of
*  whatever function was holding this. A lock nobody can break is a filesystem
*  nobody can use after one program faults at the wrong moment, and "reboot"
*  is not a repair. Every acquisition sweeps first, which costs one state
*  lookup - the same arrangement, and the same price, as sysnet_reap().
*
*  THE CACHES GO WITH IT, and that is what this does that the resolver's sweep
*  does not. The DNS lock guards a state machine that dns_cancel() puts back;
*  this one guards two sector caches, and a task that died in here may have
*  left a MODIFIED sector in one of them. fat_patch_fat() edits fat_buf and
*  then writes it out; dying between those two leaves a buffer that no longer
*  describes the disk, and handing it to the next task would let that task
*  write somebody else's abandoned edit back as though it were current. The
*  caches are only ever caches, so dropping them costs a re-read and nothing
*  else.
*
*  THE VOLUME IS NOT LATCHED READ ONLY, and it deliberately is not. A task
*  that died in the middle of a write left the medium in one of the states the
*  ordering above is built to leave - the file at its old extent, at worst
*  with some unreferenced clusters. That is the crash case, reached without a
*  crash, and it is recoverable by definition. fat_write_locked is for the one
*  thing that is not: FAT copies known to disagree. */
static void fat_lock_reap(void)
{
    unsigned long flags;
    int broke = 0;

    flags = fat_irq_save();
    if (fat_lock != 0 && fat_lock_dead(fat_lock_owner))
    {
        fat_buf_valid = 0;
        dat_buf_valid = 0;
        fat_lock_owner = -1;
        fat_lock = 0;
        broke = 1;
    }
    fat_irq_restore(flags);

    if (broke)
        task_wake(&fat_lock);
}

/* Takes it if it is free. Returns 1 when it is now ours, 0 when somebody else
*  has it. The flag and the owner are written under the same cli, which is the
*  whole point - see the note on the resolver lock above. */
static int fat_lock_grab(void)
{
    unsigned long flags;
    int free_now;

    flags = fat_irq_save();
    free_now = (fat_lock == 0);
    if (free_now)
    {
        fat_lock = 1;
        fat_lock_owner = taskmgr_get_currpid();
    }
    fat_irq_restore(flags);

    return free_now;
}

/* Gives it back and lets a waiter know. The owner is cleared with the flag
*  rather than after it, so no instant exists at which the lock is free and
*  still records a holder.
*
*  task_wake() outside the cli: it only changes task states and the scheduler
*  does the rest at the next tick, so nothing can switch tasks underneath it.
*  A task that grabs the lock in the gap between the restore and the wake
*  costs the waiters one extra turn around their loop, which is exactly what
*  the re-test in fat_lock_wait() is for. */
static void fat_lock_release(void)
{
    unsigned long flags;

    flags = fat_irq_save();
    fat_lock_owner = -1;
    fat_lock = 0;
    fat_irq_restore(flags);

    task_wake(&fat_lock);
}

/* Blocks until the lock looks free or the bound above expires.
*
*  This is the idiom system.h spells out above task_wait(), and none of it is
*  optional. The condition is tested with interrupts ALREADY OFF, so nothing
*  can change it between the test and the block - which on one CPU means no
*  other task can run at all in that window, and a release therefore cannot
*  slip past unnoticed. And it is tested AGAIN after every wake, so a wake
*  that arrives while this task is marked blocked but has not yet stopped
*  running is not lost; it costs one more turn.
*
*  Waiting rather than spinning is the entire reason this is not a busy loop.
*  A spin would hold the CPU against the very task that has to finish before
*  the lock can come back, on a scheduler that is not preemptive between
*  equals in any useful sense, and on a USB transfer that means spinning
*  through the whole time slice of the task that is sleeping on the transfer.
*
*  No task running - the boot path, before the shell exists - is a case
*  task_wait() answers with an immediate timeout rather than a block, which
*  here would be a spin. sleep() is the honest wait in that context, and there
*  is nothing to contend with there anyway. */
static void fat_lock_wait(void)
{
    unsigned long flags;

    if (taskmgr_get_currpid() < 0)
    {
        sleep(FAT_LOCK_POLL_MS);
        return;
    }

    flags = fat_irq_save();
    while (fat_lock != 0)
    {
        if (!task_wait(&fat_lock, FAT_LOCK_POLL_MS))
            break;                  /* the bound expired; go and reap */
    }
    fat_irq_restore(flags);
}

/* The way in. Returns 0 holding the lock, or -1 with fat_err set.
*
*  Every public entry point that touches a cache or the medium is a thin shell
*  around this, its body, and fat_lock_release(). That shape is not decoration
*  - it is the answer to RECURSION, and there was some. Two paths in this file
*  reached from one public function into another:
*
*    fat_write_ready() -> fat_writable(),  used by all four write calls
*    fat_bytes_of()    -> fat_cluster_bytes(), used by fat_total_bytes()
*                                              and fat_free_bytes()
*
*  The second of those is gone as of the move to kibibytes: fat_kib_of() works
*  in SECTORS and reaches fat_spc directly, because a cluster count in bytes
*  is the thing that stopped fitting in 32 bits. The rule it was an instance
*  of has not gone anywhere, and the one remaining instance is why the split
*  is still the shape below.
*
*  With a plain flag and no split, the first of those would have deadlocked on
*  the first fat_create() anybody ever tried. The alternative was a recursive
*  lock - a depth counter next to the owner - and it was rejected because it
*  buys the right to be careless about which functions are entry points and
*  which are internal, and that carelessness is what put the call there in the
*  first place. The split makes it a compile time property instead: a body
*  never calls a shell, so there is nothing to count. fat_write_ready() calls
*  fat_writable_unlocked(), and the second path stopped existing rather than
*  being fixed - which is the better outcome of the two and not one this
*  arrangement can be relied on to produce.
*
*  EVERY WAY OUT RELEASES, which in a file with this many error returns is the
*  other half of the argument for the split. The bodies below return -1 from
*  thirty or so places between them and not one of them has to remember to
*  unlock, because the shell is the only thing that ever locked. */
static int fat_lock_acquire(void)
{
    uint32_t start;

    fat_lock_reap();
    if (fat_lock_grab())
        return 0;

    start = (uint32_t)timer_get_ticks();

    for (;;)
    {
        fat_lock_wait();

        fat_lock_reap();
        if (fat_lock_grab())
            return 0;

        if (fat_ms_since(start) >= FAT_LOCK_MS)
        {
            /* Written without the lock, which fat_last_error() already had to
            *  tolerate - see the note there. Degrading to an error is the
            *  whole purpose of the bound: a lock lost despite the reaping
            *  above must not become a machine that stops. */
            fat_err = "the filesystem is busy";
            return -1;
        }
    }
}

/* A directory in the only two shapes a FAT volume has. is_root selects
*  between them; the fields of the other shape are then meaningless.
*
*  is_root means THE FIXED ROOT AREA and not "this is the root directory",
*  and the distinction is what keeps FAT32 from needing a third shape. On
*  FAT12/16 the root is a fixed run of sectors with no cluster number, so it
*  is the first shape and everything else is the second. On FAT32 the root is
*  a chain like any other directory, so it is the SECOND shape - is_root is 0
*  and cluster holds BPB_RootClus - and the first shape never occurs at all.
*  Nothing below therefore had to learn about FAT32: the two places that build
*  one of these went through fat_dir_root() and fat_dir_at(), and every walk,
*  lookup and slot search downstream was already written for a chain. */
typedef struct
{
    int      is_root;       /* 1: fixed root area, 0: cluster chain          */
    uint32_t cluster;       /* first cluster, chains only                    */
} fat_dir;

/* The root directory, in whichever shape this volume has it. Every path
*  resolution starts here, so this is the one place that has to know that
*  FAT32 put the root into the data area. */
static void fat_dir_root(fat_dir *d)
{
    if (fat_bits == 32)
    {
        d->is_root = 0;
        d->cluster = fat_root_cluster;
    }
    else
    {
        d->is_root = 1;
        d->cluster = 0;
    }
}

/* The directory a directory ENTRY names.
*
*  Cluster 0 is FAT's way of saying "the root", and it is not a corner case:
*  the ".." entry of every first level subdirectory carries it, on FAT32 as
*  much as on FAT12/16, because the specification says ".." points at 0 when
*  the parent is the root even though the root there has a perfectly good
*  cluster number of its own. So the mapping is needed on all three formats
*  and it is needed in exactly one place. */
static void fat_dir_at(fat_dir *d, uint32_t cluster)
{
    if (cluster == 0)
    {
        fat_dir_root(d);
        return;
    }
    d->is_root = 0;
    d->cluster = cluster;
}

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

/* The exact inverses, used only when building a directory entry. Written a
*  byte at a time for the same reason rd16/rd32 read a byte at a time: the
*  fields are not aligned, and a short store through a cast would be a
*  misaligned access on a struct nobody packed. */
static void wr16(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off]     = (uint8_t)(v & 0xFF);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off]     = (uint8_t)(v & 0xFF);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

/* --- Failures that came from the medium ------------------------------------
*
*  Every message in this file used to be a plain constant, and for the ones
*  about the filesystem it still is: "corrupt cluster chain" says everything
*  there is to say. The ones about the DEVICE cannot be constants any more.
*
*  Two things changed under them. blk_read() refuses requests that ata_read()
*  passed straight down to the hardware - a device number nothing is
*  registered on, a range that runs off the end of the medium - so a failed
*  read no longer means the disk said no; it can equally mean the question
*  was not one that could be asked. blk_last_error() is where that difference
*  is written down, and dropping it on the floor would leave "read error in
*  the FAT" standing in front of a device that was never read at all.
*
*  And the device is worth naming, by BUS as well as by model. On a fixed
*  disk the only interesting failure is a bad sector; on a removable one the
*  usual failure is that somebody pulled it out, and blk_bus() is what lets a
*  user see that the thing that vanished was the stick and not the disk.
*
*  One static buffer, because fat_last_error() hands back a pointer that
*  outlives the call and this file allocates nothing. It holds one message at
*  a time: the next failure overwrites it, which is exactly the lifetime the
*  constants had. */
static char fat_msg[192];

static void fat_msg_cat(uint32_t *n, const char *s)
{
    if (s == 0)
        return;
    while (*s != '\0' && *n + 1 < sizeof(fat_msg))
        fat_msg[(*n)++] = *s++;
    fat_msg[*n] = '\0';
}

/* what, then the device in brackets, then - when with_reason is set and the
*  block layer has something to say - why it refused. with_reason is off for
*  the callers that are reporting a STATE rather than a failed call, where
*  blk_last_error() would be about some older, unrelated request. */
static void fat_dev_err(const char *what, int with_reason)
{
    uint32_t n = 0;
    const char *what_it_is;
    const char *why;

    fat_msg[0] = '\0';
    fat_msg_cat(&n, what);

    /* blockdev.h promises neither of these is null, but blk_describe() is
    *  empty for a number nothing is registered on - which is exactly the
    *  case a message about a device that vanished has to survive. Then the
    *  bus stands alone and reads "(none)", rather than "( on none)". */
    what_it_is = blk_describe(fat_drive);
    fat_msg_cat(&n, " (");
    if (what_it_is[0] != '\0')
    {
        fat_msg_cat(&n, what_it_is);
        fat_msg_cat(&n, " on ");
    }
    fat_msg_cat(&n, blk_bus(fat_drive));
    fat_msg_cat(&n, ")");

    if (with_reason)
    {
        why = blk_last_error();
        if (why != 0 && why[0] != '\0')
        {
            fat_msg_cat(&n, ": ");
            fat_msg_cat(&n, why);
        }
    }

    fat_err = fat_msg;
}

/* --- The ordering barrier --------------------------------------------------
*
*  Waits until everything written to this volume so far is ON THE MEDIUM, and
*  not merely acknowledged by whatever is holding it.
*
*  The write half below is built on four ordering rules and every one of them
*  is a statement about what has already landed when the next write is issued.
*  Against ATA that held for free and nothing here said so, because nothing had
*  to: ata_transfer() ends every write with a CACHE FLUSH command, so a return
*  of 0 meant the sectors were out of the drive's buffer. blk_write() promises
*  nothing of the sort and blockdev.h says so out loud - a stick acknowledges a
*  SCSI WRITE(10) when it HAS the data, not when the flash does - and without
*  this call the careful orderings would be the order the writes were asked for
*  and nothing more.
*
*  A flush is not free and this is not called per sector. blockdev.h is blunt
*  about the price: on a slow device each one is a round trip. It is called at
*  the TRANSITIONS the ordering argument actually names and nowhere else -
*  four of them, at seven places, and each place is marked with the sentence
*  in the ordering rules that requires it:
*
*    rule 2  the backup FAT copies before the primary
*              fat_put_fat_sector(), once per FAT sector write, and only on a
*              volume that has more than one copy
*    rule 3  the zeros - and the mark that took the cluster - before the link
*              fat_file_cluster() growing a file,
*              fat_dir_free_slot() growing a directory
*    rule 4  the data and the chain before the directory entry
*              fat_commit(), i.e. once per file operation that grows
*    rule 4  the directory entry before the clusters are released
*              fat_truncate() twice - the shortened entry, and the new end
*              marker before the tail - and fat_delete() once
*
*  What that costs, per operation, on a two-FAT volume: a create is nothing
*  unless the directory has to grow; an append is one per cluster allocated
*  (its FAT write) plus one more for the link plus one for the commit; a
*  delete or a shrink is one plus one per FAT sector the released chain
*  touches. The last of those is the expensive one and it is expensive for a
*  reason this file cannot fix from here: fat_free_chain() clears one entry
*  per cluster and each clearing is its own write of the FAT sector, so a long
*  chain pays per cluster rather than per sector. Batching those would mean a
*  dirty flag on fat_buf and a different write path, which is a change to the
*  ordering rather than to its cost.
*
*  FSInfo is not on that list and is not an omission from it. It carries a
*  cached free count and an allocation hint, both of which every driver -
*  including this one - is required to re-derive rather than believe, so no
*  ordering can be lost by it landing early, late or not at all. See
*  fat_fsinfo_store(), which is the only write in this file with no barrier
*  anywhere near it and says why.
*
*  What is deliberately NOT here is a flush at the END of an operation. That
*  would buy durability - "the file is really there now" - and durability is
*  not what the rules above promise; they promise that whatever survives is
*  consistent. Adding one would cost a round trip on every call to buy a
*  guarantee nothing in fat.h makes.
*
*  On a device whose writes already land - every ATA drive - blk_flush() has
*  nothing to do and answers 0, so a hard disk pays for none of this.
*
*  A FLUSH THAT FAILS IS NOT NOTHING. It means the ordering the code was about
*  to rely on did not happen, and the only question left is what the next step
*  would have been. Every caller below answers it the same way and it is the
*  answer the whole file already gives: the next step is always the DANGEROUS
*  one - linking a cluster whose zeros may not be there, committing a size
*  whose data may not be there, releasing clusters a directory entry may still
*  claim - so it is not taken. What is left behind is the state the rules were
*  aiming at anyway, a file at its old extent with at worst some unreferenced
*  clusters. A leak is one fsck away; the thing not being risked here is a
*  cross link, which is not repairable at all. Carrying on would be defensible
*  where the next step is the safe one, and there is no such point below.
*
*  what is the phrase fat_dev_err() puts in front of the device name, so the
*  message says which ordering was lost rather than only that one was. */
static int fat_sync(const char *what)
{
    if (blk_flush(fat_drive) == 0)
        return 0;

    fat_dev_err(what, 1);
    return -1;
}

/* --- Cached sector reads -------------------------------------------------- */

static int fat_get_fat_sector(uint32_t lba)
{
    if (fat_buf_valid && fat_buf_lba == lba)
        return 0;

    fat_buf_valid = 0;
    if (blk_read(fat_drive, lba, 1, fat_buf) < 0)
    {
        fat_dev_err("read error in the FAT", 1);
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
    if (blk_read(fat_drive, lba, 1, dat_buf) < 0)
    {
        fat_dev_err("read error in the data area", 1);
        return -1;
    }
    dat_buf_lba = lba;
    dat_buf_valid = 1;
    return 0;
}

/* --- Cached sector writes --------------------------------------------------
*
*  The counterparts of the two functions above, and the ONLY places in this
*  file that call blk_write(). Both work on the contents of the cache buffer
*  the matching read would have filled, so a sector is never written from
*  somewhere the cache does not know about - which is what keeps the caches
*  describing the disk rather than a version of it that used to be true.
*
*  WHAT "THE WRITE LANDED" MEANS. The ordering rules this file is built on -
*  backups before the primary, data before the chain, the chain before the
*  directory entry - are worth something only if an earlier write is on the
*  medium before a later one is issued. blk_write() returning 0 does not say
*  that: on a removable device it says the stick has the bytes, not that the
*  flash does. fat_sync() is what turns "issued" back into "landed", and it is
*  called at the transitions those rules name rather than after every sector.
*  Neither of the two functions below flushes on its own account - a flush
*  belongs to an ordering, and an ordering belongs to the caller that has one.
*  The single exception is inside fat_put_fat_sector(), because the ordering
*  it has to keep is between two writes it makes itself. */

/* Writes the FAT sector currently held in fat_buf to every FAT copy that is
*  mirrored on the volume. sec is the sector's index WITHIN a FAT, not an lba,
*  and fat_buf must be holding that sector of the ACTIVE copy - which it is,
*  because fat_byte() only ever loads it from there.
*
*  A FAT16 volume normally carries two copies, and BPB_NumFATs says so; the
*  count is read rather than assumed, because a volume with one copy or with
*  three is legal and mformat will happily produce either. Every allocator
*  change has to land in all of them: copies that disagree are what makes
*  Linux and Windows offer to REPAIR the volume, which is how a user finds
*  out something went wrong - afterwards.
*
*  fat_fat_copies rather than fat_num_fats, and on FAT12/16 they are always
*  the same number. FAT32 alone can turn mirroring OFF in BPB_ExtFlags, and
*  then exactly one copy is live and the others hold whatever they held when
*  it was switched off. Writing all of them there would not corrupt anything
*  this driver reads, but it would overwrite copies another driver is entitled
*  to treat as stale history, and fat_mount() has already pointed fat_fat_lba
*  at the live one - so the loop below is over one copy and the barrier costs
*  nothing. See fat_mount().
*
*  The copies are written from the last one down to the first, so the
*  primary is the last thing to change. Nothing here is atomic and nothing
*  can be, but the primary is the copy every reader actually uses, so a
*  failure part way through leaves the volume behaving exactly as it did
*  before the call rather than half way into a change that was abandoned.
*
*  What cannot be avoided is the window between two copies: they are two
*  sector writes and no medium makes them one. If the second one fails - or
*  the power goes - the copies disagree, and fsck.vfat will say so ("FATs
*  differ but appear to be intact"). Writing the primary LAST is what makes
*  that survivable: the primary is the copy every reader uses, so what is
*  left behind is the volume exactly as it was, with a mirror that has run
*  ahead. The other order would leave the working copy holding a change the
*  caller was told had failed.
*
*  When it does happen, the volume is latched read only. The copies are
*  known to differ and this driver cannot tell how far the damage would
*  spread if it kept going, so it stops.
*
*  Any failure also invalidates the cache: fat_buf then holds a
*  modification that the disk does not have, and handing that back to a
*  later read would be worse than the failed write itself.
*
*  AND ALL OF THAT IS ABOUT ORDER, so on a device that only queues writes it
*  needs fat_sync() to still mean anything. "The primary is the last thing to
*  change" is a claim about the medium, not about the order two calls were
*  made in: a stick that has both sectors in its buffer can commit them either
*  way round, and the way round it must not choose is the one that leaves the
*  copy every reader uses holding a change the caller was told had failed.
*  There is one barrier for this, after all the backups and before the
*  primary, not one between every pair - the argument only ever distinguishes
*  the primary from the rest. A volume with a single FAT has no backup to
*  order against and pays nothing.
*
*  A failed barrier here is the one place in the file where the state it
*  leaves behind is the latched one rather than a leak. The backups were
*  issued and cannot be un-issued, the primary will not be written, and this
*  driver has no way to find out whether the copies now agree - which is
*  exactly the condition fat_write_locked exists for. So it stops, for the
*  same reason and with the same words as a copy that failed outright. */
static int fat_put_fat_sector(uint32_t sec)
{
    uint32_t k;

    for (k = fat_fat_copies; k > 0; k--)
    {
        /* Rule 2: every allocator change lands in every copy, BACKUPS FIRST,
        *  the primary last. This is where "first" stops being a figure of
        *  speech on a device that buffers. */
        if (k == 1 && fat_fat_copies > 1)
        {
            if (fat_sync("the device would not confirm the backup FAT copies"
                         " - volume is now read only") < 0)
            {
                fat_buf_valid = 0;
                fat_write_locked = 1;
                return -1;
            }
        }

        if (blk_write(fat_drive,
                      fat_fat_lba + (k - 1) * fat_sec_per_fat + sec,
                      1, fat_buf) < 0)
        {
            fat_buf_valid = 0;

            /* Copies after this one were already written, so the volume no
            *  longer has a consistent set. Nothing else may be written to
            *  it until it is mounted again. */
            if (k < fat_fat_copies)
            {
                fat_write_locked = 1;
                fat_dev_err("FAT copies left inconsistent"
                            " - volume is now read only", 1);
                return -1;
            }

            fat_dev_err("write error in the FAT", 1);
            return -1;
        }
        fat_written += BLK_SECTOR_SIZE;
    }
    return 0;
}

/* Writes dat_buf back to the sector it was read from. */
static int fat_put_data_sector(void)
{
    if (!dat_buf_valid)
    {
        fat_err = "nothing cached to write back";
        return -1;
    }
    if (blk_write(fat_drive, dat_buf_lba, 1, dat_buf) < 0)
    {
        dat_buf_valid = 0;
        fat_dev_err("write error in the data area", 1);
        return -1;
    }
    fat_written += BLK_SECTOR_SIZE;
    return 0;
}

/* Claims the data cache for lba WITHOUT reading it. Only legal when the
*  caller then fills all BLK_SECTOR_SIZE bytes before calling
*  fat_put_data_sector(); anything less would write out whatever the buffer
*  happened to hold from an unrelated sector. It exists because a full
*  sector overwrite otherwise pays for a read whose every byte is discarded,
*  and sequential writing is nothing but full sector overwrites.
*
*  That read used to cost a PIO burst. On a stick it costs a bulk
*  transaction each way, so on the slowest device this driver can be given
*  the saving is the difference between one transfer per sector written and
*  two. The rule it depends on is unchanged and is not negotiable: fill all
*  BLK_SECTOR_SIZE bytes, or do not claim. */
static void fat_claim_data(uint32_t lba)
{
    dat_buf_lba = lba;
    dat_buf_valid = 1;
}

/* One byte out of the first FAT, by byte offset from its start. Going
*  through this rather than through a sector index is what makes the FAT12
*  straddle case below fall out for free: a twelve bit entry whose two bytes
*  land in different sectors reads exactly like any other. */
static int fat_byte(uint32_t off, uint8_t *out)
{
    uint32_t sec = off / BLK_SECTOR_SIZE;

    if (sec >= fat_sec_per_fat)
    {
        fat_err = "FAT entry outside the FAT";
        return -1;
    }
    if (fat_get_fat_sector(fat_fat_lba + sec) < 0)
        return -1;

    *out = fat_buf[off % BLK_SECTOR_SIZE];
    return 0;
}

/* --- The FAT itself ------------------------------------------------------- */

/* Raw value of FAT entry n.
*
*  FAT16 is a plain array of 16 bit words and FAT32 one of 32 bit ones, of
*  which ONLY THE LOW 28 BITS are the cluster number. The top four are
*  reserved, they belong to whatever wrote the volume, and they are masked off
*  here rather than compared against - an entry with them set is an ordinary
*  entry and not a broken one. Everything downstream, the end of chain test
*  included, therefore sees a 28 bit number and never has to remember to mask
*  again. fat_set_entry() is the other half of that promise: it puts the four
*  bits back exactly as it found them.
*
*  FAT12 is the interesting one: the
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
    uint8_t b0, b1, b2, b3;
    uint32_t off, v;

    if (fat_bits == 32)
    {
        off = n * 4;

        if (fat_byte(off, &b0) < 0)
            return -1;
        if (fat_byte(off + 1, &b1) < 0)
            return -1;
        if (fat_byte(off + 2, &b2) < 0)
            return -1;
        if (fat_byte(off + 3, &b3) < 0)
            return -1;

        v = (uint32_t)b0
          | ((uint32_t)b1 << 8)
          | ((uint32_t)b2 << 16)
          | ((uint32_t)b3 << 24);

        *out = v & FAT32_MASK;
        return 0;
    }

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
*  value below it, 0xFF7 / 0xFFF7 / 0x0FFFFFF7, marks a bad cluster and is not
*  a valid link either, which the range check in fat_next_cluster() catches.
*
*  The FAT32 value is a 28 bit one and the comparison it is used in is a
*  comparison of 28 bit numbers, because fat_entry() has already masked the
*  reserved bits off. Testing an unmasked entry against 0x0FFFFFF8 is the way
*  this goes wrong: an ordinary link with a reserved bit set reads as a number
*  above every terminator, and the file ends where it does not. */
static uint32_t fat_eoc_min(void)
{
    if (fat_bits == 32)
        return 0x0FFFFFF8UL;
    return (fat_bits == 12) ? 0x0FF8UL : 0xFFF8UL;
}

/* The terminator this driver WRITES. Any value from fat_eoc_min() up ends a
*  chain when read, but only one of them should be produced, and the width
*  is what makes them different values: twelve bits of ones is 0x0FFF,
*  sixteen bits of ones is 0xFFFF. Writing the FAT16 marker into a FAT12
*  entry stores 0x0FFF's neighbour nibbles wrong and leaves 0xFFF anyway;
*  writing the FAT12 marker into a FAT16 entry stores 0x0FFF, which is below
*  0xFFF8 and therefore not a terminator at all - the file would read on
*  into cluster 4095, i.e. into whatever else lives there. Hence one
*  function rather than one constant.
*
*  On FAT32 it is 0x0FFFFFFF, twenty-eight bits of ones and not thirty-two:
*  the four above them are not this driver's to set any more than they are
*  its to clear, and fat_set_entry() masks them out of every value it is
*  given - a terminator included.
*
*  The value just below the terminators, 0x0FF7 / 0xFFF7 / 0x0FFFFFF7, marks
*  a bad cluster. The allocator never hands one out because it only ever takes
*  an entry that reads as 0, and a bad cluster is not 0. */
static uint32_t fat_eoc(void)
{
    if (fat_bits == 32)
        return FAT32_MASK;
    return (fat_bits == 12) ? 0x0FFFUL : 0xFFFFUL;
}

static int fat_cluster_valid(uint32_t c)
{
    return (c >= 2 && c <= fat_clusters + 1);
}

/* --- Writing the FAT -------------------------------------------------------
*
*  Read-modify-write of the two or four bytes a FAT entry occupies, in every
*  mirrored copy of the FAT. off is the byte offset of the FIRST of them from
*  the start of a FAT; m[i] selects which bits of byte i belong to the entry,
*  and only those are replaced. n is 2 or 4.
*
*  THE MASKS ARE THE WHOLE POINT, and they are the point twice over now.
*
*  On FAT16 both are 0xFF and the function is a two byte store. On FAT12 one
*  of the two bytes is SHARED with the neighbouring entry - three nibbles per
*  entry means every second entry splits a byte with the one before or after
*  it - and the neighbour's nibble has to come through untouched. Getting that
*  wrong does not produce a visibly broken filesystem: it silently rewrites
*  the length of some other file's chain.
*
*  On FAT32 nothing is shared with a neighbour, but the top four bits of the
*  fourth byte are RESERVED and belong to whatever last wrote the volume. The
*  mask for that byte is 0x0F, and it is the same mechanism doing the same job
*  one bit-field over: bits this driver did not put there come back as they
*  were. That failure is quieter still than the FAT12 one - the volume mounts,
*  files read, fsck passes - and it is the standard way a FAT32 driver
*  corrupts a volume it did not create.
*
*  And the bytes need not be in the same sector. Offset off may be the last
*  byte of one sector with off+1 the first byte of the next, which happens for
*  every FAT12 entry whose offset is 511 modulo 512 - entry 340, 682, 1024 and
*  so on. Reading across that seam is free (fat_byte() goes through the cache
*  one byte at a time); writing across it is a read-modify-write of two
*  different sectors, in every FAT copy, and the loop below spells that case
*  out rather than leaving it to fall out of anything.
*
*  A FAT32 entry never splits, because four bytes at a four byte aligned
*  offset cannot straddle a 512 byte boundary. The loop handles it anyway,
*  because a loop that handles the general case has no untested branch in it
*  - and because the sentence above is a property of the arithmetic in
*  fat_set_entry(), not of this function. */
static int fat_patch_fat(uint32_t off, const uint8_t *v, const uint8_t *m,
                         uint32_t n)
{
    uint32_t i = 0;
    uint32_t j;
    uint32_t sec;
    uint32_t idx;

    if ((off + n - 1) / BLK_SECTOR_SIZE >= fat_sec_per_fat)
    {
        fat_err = "FAT entry outside the FAT";
        return -1;
    }

    /* One round of writes per sector the entry touches, which is one in
    *  every case but the FAT12 split. The entry is only fully written once
    *  the last round has landed; a power cut in between leaves half an entry
    *  - unavoidable without a journal, and the reason the allocation order
    *  elsewhere in this file is arranged so that the half written entry is
    *  the one nobody is following yet. */
    while (i < n)
    {
        sec = (off + i) / BLK_SECTOR_SIZE;

        if (fat_get_fat_sector(fat_fat_lba + sec) < 0)
            return -1;

        for (j = i; j < n && (off + j) / BLK_SECTOR_SIZE == sec; j++)
        {
            idx = (off + j) % BLK_SECTOR_SIZE;
            fat_buf[idx] = (uint8_t)((fat_buf[idx] & (uint8_t)~m[j])
                                     | (uint8_t)(v[j] & m[j]));
        }

        if (fat_put_fat_sector(sec) < 0)
            return -1;

        i = j;
    }

    return 0;
}

/* Sets FAT entry n to v. The exact inverse of fat_entry(), and deliberately
*  written to mirror its layout comment nibble for nibble:
*
*      byte:     |    b0     |    b1     |
*      nibbles:  | L0 |  H0  |  L1 | H1  |
*      even n:   entry = ((b1 & 0x0F) << 8) | b0
*      odd  n:   entry = (b1 << 4) | (b0 >> 4)
*
*  so an even entry owns all of b0 and the LOW nibble of b1, and an odd one
*  owns the HIGH nibble of b0 and all of b1. Those are the masks below.
*
*  AND THE SAME ARGUMENT MAKES THE FAT32 CASE. A 32 bit entry owns all of
*  bytes 0, 1 and 2 and only the LOW NIBBLE of byte 3, because the four bits
*  above it are reserved. They are not free space, they are not zero on a
*  volume somebody else formatted, and they are not this driver's to touch:
*  the mask 0x0F on the last byte is what carries them through a write
*  unchanged, and it is the ONE place in this file where that happens. There
*  is no second path to a FAT entry - fat_patch_fat() is private to this
*  function and to nothing else - so this mask is the whole of the guarantee.
*
*  The value is masked as well as the byte. A caller that passed something
*  with the top bits set - fat_eoc() would, if it were written as thirty-two
*  ones - must not be able to smuggle them in through v; and v & FAT32_MASK
*  makes the two halves of the rule agree instead of relying on every caller
*  to have got it right.
*
*  Entries 0 and 1 are not clusters. Entry 0 holds the media descriptor and
*  entry 1 the dirty flags - on FAT32 that is where the clean shutdown and
*  hard error bits live - and a driver that writes a chain link into either
*  has just told every other operating system that the volume needs checking.
*  Neither is ever a legal cluster number, so refusing them here costs nothing
*  and closes the case for good. */
static int fat_set_entry(uint32_t n, uint32_t v)
{
    uint32_t off;
    uint8_t val[4];
    uint8_t msk[4];

    if (!fat_cluster_valid(n))
    {
        fat_err = "refusing to write a FAT entry that is not a cluster";
        return -1;
    }

    if (fat_bits == 32)
    {
        off = n * 4;
        v &= FAT32_MASK;

        val[0] = (uint8_t)(v & 0xFF);
        msk[0] = 0xFF;
        val[1] = (uint8_t)((v >> 8) & 0xFF);
        msk[1] = 0xFF;
        val[2] = (uint8_t)((v >> 16) & 0xFF);
        msk[2] = 0xFF;
        /* The reserved four bits live in the top half of this byte, and
        *  0x0F is what leaves them exactly as they were found. */
        val[3] = (uint8_t)((v >> 24) & 0x0F);
        msk[3] = 0x0F;

        return fat_patch_fat(off, val, msk, 4);
    }

    if (fat_bits == 12)
    {
        off = n + (n / 2);

        if (n & 1)
        {
            val[0] = (uint8_t)((v << 4) & 0xF0);
            msk[0] = 0xF0;
            val[1] = (uint8_t)((v >> 4) & 0xFF);
            msk[1] = 0xFF;
        }
        else
        {
            val[0] = (uint8_t)(v & 0xFF);
            msk[0] = 0xFF;
            val[1] = (uint8_t)((v >> 8) & 0x0F);
            msk[1] = 0x0F;
        }
    }
    else
    {
        off = n * 2;
        val[0] = (uint8_t)(v & 0xFF);
        msk[0] = 0xFF;
        val[1] = (uint8_t)((v >> 8) & 0xFF);
        msk[1] = 0xFF;
    }

    return fat_patch_fat(off, val, msk, 2);
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

/* The first cluster out of a directory entry.
*
*  A cluster number is split across two fields sixteen bits apart, and only
*  FAT32 uses the upper one. On FAT12/16 DIR_FstClusHI is reserved: the
*  specification says it is zero, and a driver that reads it anyway is a
*  driver that follows a stray value some other tool left there into a
*  cluster the volume does not have. So it is read on FAT32 and ignored
*  everywhere else, which is a decision about which field to trust rather
*  than an optimisation. */
static uint32_t fat_first_cluster(const uint8_t *e)
{
    uint32_t c = rd16(e, DIR_FstClusLO);

    if (fat_bits == 32)
        c |= rd16(e, DIR_FstClusHI) << 16;

    return c;
}

static void fat_fill_dirent(const uint8_t *e, fat_dirent *out)
{
    fat_name_of(e + DIR_Name, out->name);
    out->is_dir = (e[DIR_Attr] & ATTR_DIRECTORY) ? 1 : 0;
    out->first_cluster = fat_first_cluster(e);
    out->size = out->is_dir ? 0 : rd32(e, DIR_FileSize);
}

/* --- Directory access ----------------------------------------------------- */

/* Where entry number n of a directory lives: the sector holding it and its
*  index within that sector. Returns 0, 1 when n is past the end of the
*  directory, or negative.
*
*  The whole asymmetry of FAT12/16 lives in this function. The root
*  directory is a fixed run of fat_root_sectors sectors starting at
*  fat_root_lba, with room for exactly fat_root_entries entries and no way
*  to grow. A subdirectory is a file like any other: a cluster chain, walked
*  with fat_walk(), and it ends when the chain ends.
*
*  A FAT32 volume only ever reaches the second branch, root directory
*  included, which is why adding FAT32 did not add a case here. Neither
*  branch changed at all.
*
*  Split out of fat_dir_entry() so that the write half can name a directory
*  slot without a second copy of this arithmetic. */
static int fat_dir_locate(const fat_dir *d, uint32_t n,
                          uint32_t *lba, uint32_t *within)
{
    uint32_t per_cluster;
    uint32_t cluster;
    uint32_t off;
    int r;

    if (d->is_root)
    {
        if (n >= fat_root_entries)
            return 1;
        if (n / DIR_PER_SECTOR >= fat_root_sectors)
            return 1;
        *lba = fat_root_lba + n / DIR_PER_SECTOR;
        *within = n % DIR_PER_SECTOR;
        return 0;
    }

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

    off = n % per_cluster;
    *lba = fat_cluster_lba(cluster) + off / DIR_PER_SECTOR;
    *within = off % DIR_PER_SECTOR;
    return 0;
}

/* Raw entry number n of a directory, copied into out (32 bytes).
*  Returns 0, 1 when n is past the end of the directory, or negative. */
static int fat_dir_entry(const fat_dir *d, uint32_t n, uint8_t *out)
{
    uint32_t lba;
    uint32_t within;
    int r;

    r = fat_dir_locate(d, n, &lba, &within);
    if (r != 0)
        return r;

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

/* Where a directory entry sits on the disk, so that it can be written back.
*  Not a file handle: it is only valid as long as nothing moves the entry,
*  and nothing here ever does. */
typedef struct
{
    uint32_t lba;           /* the sector holding it                         */
    uint32_t within;        /* its index inside that sector                  */
} fat_slot;

/* Looks a single 8.3 component up in a directory, handing back the raw
*  entry and, when slot is not null, where it lives. Returns 0 when found,
*  1 when not, negative on error.
*
*  The comparison runs over the PRINTABLE form of the name, i.e. through
*  fat_name_of(), and is case insensitive. That matters for creating a file
*  as much as for opening one: "exists already" has to mean exactly what the
*  read path would find, or fat_create() would cheerfully write a second
*  entry that shadows the first. */
static int fat_dir_find(const fat_dir *d, const char *name,
                        uint8_t *e, fat_slot *slot)
{
    char cand[FAT_NAME_MAX];
    uint32_t lba, within;
    uint32_t n = 0;
    int r;

    for (;;)
    {
        r = fat_dir_locate(d, n, &lba, &within);
        if (r != 0)
            return r;
        if (fat_get_data_sector(lba) < 0)
            return -1;
        memcpy(e, dat_buf + within * DIR_ENTRY_SIZE, DIR_ENTRY_SIZE);
        n++;

        if (e[0] == DIRENT_FREE)
            return 1;
        if (!fat_entry_usable(e))
            continue;

        fat_name_of(e + DIR_Name, cand);
        if (fat_name_eq(cand, name))
        {
            if (slot != 0)
            {
                slot->lba = lba;
                slot->within = within;
            }
            return 0;
        }
    }
}

/* Looks a single 8.3 component up in a directory. Returns 0 when found,
*  1 when not, negative on error. */
static int fat_dir_lookup(const fat_dir *d, const char *name, fat_dirent *out)
{
    uint8_t e[DIR_ENTRY_SIZE];
    int r;

    r = fat_dir_find(d, name, e, 0);
    if (r == 0)
        fat_fill_dirent(e, out);
    return r;
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

    fat_dir_root(&dir);
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
            *  is FAT's way of saying "the root" - see fat_dir_at(). */
            fat_dir_at(&dir, out->first_cluster);
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
        fat_dir_root(d);
        return 0;
    }
    if (!de.is_dir)
    {
        fat_err = "not a directory";
        return -1;
    }
    fat_dir_at(d, de.first_cluster);
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
    fat_fat_copies = 0;
    fat_root_lba = 0;
    fat_root_sectors = 0;
    fat_root_cluster = 0;
    fat_data_lba = 0;
    fat_clusters = 0;
    fat_total_sectors = 0;
    fat_buf_valid = 0;
    dat_buf_valid = 0;
    fat_alloc_hint = 2;
    fat_free_count = 0;
    fat_free_known = 0;
    fat_fsinfo_lba = 0;
    fat_fsinfo_dirty = 0;
    fat_written = 0;
    fat_write_locked = 0;
    fat_vol_label[0] = '\0';
}

/* --- FSInfo ----------------------------------------------------------------
*
*  A FAT32 volume carries a sector - sector 1 in practice, but BPB_FSInfo says
*  which - holding a cached count of free clusters and a hint at where the
*  next free one is. Reading it is worth a great deal and believing it is not,
*  and those two sentences are the whole design.
*
*  WHY IT IS WORTH READING. Counting free clusters means reading the entire
*  FAT, which on a stick is the most expensive operation in this file by a
*  wide margin - thousands of bulk round trips for a number a shell wants to
*  print in a status line. FSInfo is one sector for the same answer.
*
*  WHY IT IS NOT BELIEVED. Nothing keeps it right. It is written by whatever
*  last had the volume mounted, it is not updated on every change even by the
*  drivers that maintain it, and a power cut leaves whatever was there. A
*  driver that trusted a free-cluster HINT as a statement about which clusters
*  are free would hand out one that is in use, and that is a cross link - the
*  one outcome nothing can repair.
*
*  SO HERE IS WHAT EACH FIELD IS ALLOWED TO DO.
*
*    FSI_Nxt_Free becomes fat_alloc_hint and nothing else. That variable
*    already documents itself as never trusted: the allocator reads every
*    entry it considers and only takes one that reads as 0, so a hint that is
*    wrong costs a slower search and cannot cost anything else. It is range
*    checked here anyway, because a hint outside the volume would make the
*    first scan wrap.
*
*    FSI_Free_Count becomes the initial value of fat_free_count, which is only
*    ever REPORTED - by fat_free_kib(), to a caller that wants to draw a bar.
*    The allocator never consults it. So the worst a wrong count can do is
*    misstate free space, and the worst that can do is make a write fail with
*    "no free clusters left" that a user thought would fit.
*
*  AND IT IS VALIDATED, three ways before any of that: both signatures and the
*  trailing one have to be exactly right, the count has to be 0xFFFFFFFF or no
*  larger than the number of data clusters on the volume, and the hint has to
*  be a cluster this volume has. Anything else and the sector is treated as
*  though it were not there - the count stays unknown, and the first call that
*  wants one pays for a scan. The scan is the fallback for every one of those
*  paths and it never goes away.
*
*  Returns nothing: a volume without a usable FSInfo is an ordinary volume,
*  not a failed mount. */
static void fat_fsinfo_load(uint32_t sec, uint32_t reserved)
{
    uint32_t count;
    uint32_t next;

    if (sec == 0 || sec == 0xFFFFUL)
        return;                     /* the BPB says there is none */

    /* Inside the reserved area, or it is not a sector this volume set aside
    *  for the purpose and something else owns those bytes. Compared against
    *  the reserved count rather than against fat_fat_lba, which does not mean
    *  the same thing on a volume with mirroring switched off. */
    if (sec >= reserved)
        return;

    if (fat_get_data_sector(sec) < 0)
    {
        /* Not a mount failure. The volume is readable, this one sector was
        *  not, and the count simply stays unknown. */
        fat_err = "";
        return;
    }

    if (rd32(dat_buf, FSI_LeadSig) != FSI_LEAD_SIG
     || rd32(dat_buf, FSI_StrucSig) != FSI_STRUC_SIG
     || rd32(dat_buf, FSI_TrailSig) != FSI_TRAIL_SIG)
        return;

    fat_fsinfo_lba = sec;

    count = rd32(dat_buf, FSI_Free_Count);
    if (count != FSI_UNKNOWN && count <= fat_clusters)
    {
        fat_free_count = count;
        fat_free_known = 1;
    }

    next = rd32(dat_buf, FSI_Nxt_Free);
    if (next != FSI_UNKNOWN && fat_cluster_valid(next))
        fat_alloc_hint = next;
}

/* Writes the count and the hint back, if this volume has somewhere to put
*  them and either has moved.
*
*  DELIBERATELY OUTSIDE THE ORDERING RULES. Every fat_sync() in this file
*  guards a transition where one write landing without another can cross-link
*  two files or leave a size describing bytes nobody wrote. Nothing here can:
*  FSInfo is a hint by definition, this driver re-derives both numbers from
*  the FAT the moment they look wrong, and every other driver is required to
*  treat them the same way. So there is no barrier before it and none after,
*  and it is written after the commit point rather than before - which means
*  that on a device that buffers it may reach the medium first. That is
*  allowed for the same reason: the field it lands in is one nobody may act
*  on without checking.
*
*  What a crash leaves is therefore a stale number, and a stale number is
*  exactly the case the validation in fat_fsinfo_load() exists for - and a
*  crash mid-operation has left leaked clusters too, so the volume was going
*  to be handed to fsck either way.
*
*  A FAILED WRITE IS NOT REPORTED, and that is the one thing here that needs
*  arguing. The caller is a shell that has just run a real operation and has
*  its own result and its own error message; failing a successful delete
*  because a hint could not be cached would be reporting a problem that did
*  not happen, and overwriting fat_err would replace the reason a real failure
*  failed with the reason a cache update did. So the message is saved and put
*  back, and the dirty flag stays set so the next operation tries again. */
static void fat_fsinfo_store(void)
{
    const char *saved;

    if (fat_fsinfo_lba == 0 || !fat_fsinfo_dirty)
        return;

    saved = fat_err;

    if (fat_get_data_sector(fat_fsinfo_lba) == 0)
    {
        wr32(dat_buf, FSI_Free_Count,
             fat_free_known ? fat_free_count : FSI_UNKNOWN);
        wr32(dat_buf, FSI_Nxt_Free, fat_alloc_hint);

        if (fat_put_data_sector() == 0)
            fat_fsinfo_dirty = 0;
    }

    fat_err = saved;
}

static int fat_mount_unlocked(int drive)
{
    uint8_t bs[BLK_SECTOR_SIZE];
    uint32_t bytes_per_sec;
    uint32_t reserved;
    uint32_t total_sectors;
    uint32_t root_bytes;
    uint32_t meta_sectors;
    uint32_t data_sectors;
    uint32_t ext_flags;
    uint32_t active;
    uint32_t lab_off;
    uint32_t i;
    int last;

    fat_reset();
    fat_drive = drive;
    fat_err = "";

    /* A block device number, and BLK_MAX_DEVICES is the whole table rather
    *  than the four ATA slots: 0..3 are still the IDE drives, 4 and up are
    *  whatever registered later. Only the range is tested here. Whether
    *  anything is actually there is blk_read()'s answer to give, and taking
    *  it from there rather than asking blk_present() first keeps the number
    *  of places that decide "no device" at one. */
    if (drive < 0 || drive >= BLK_MAX_DEVICES)
    {
        fat_err = "no such block device";
        return -1;
    }
    if (blk_read(drive, 0, 1, bs) < 0)
    {
        fat_dev_err("cannot read the boot sector", 1);
        return -1;
    }

    if (bs[BOOT_SIG_OFF] != 0x55 || bs[BOOT_SIG_OFF + 1] != 0xAA)
    {
        fat_err = "no 0x55AA boot signature - not a FAT volume";
        return -1;
    }

    /* Everything below assumes 512 byte sectors, from the sector caches to
    *  DIR_PER_SECTOR to the block layer itself - blockdev.h fixes
    *  BLK_SECTOR_SIZE at 512 for every driver and says so out loud, because
    *  a device with 4096 byte sectors would have to translate rather than
    *  report it. A volume formatted with a different sector size is refused
    *  rather than mounted with every offset quietly wrong. */
    bytes_per_sec = rd16(bs, BPB_BytsPerSec);
    if (bytes_per_sec != BLK_SECTOR_SIZE)
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

    /* THE 16 BIT FIELDS ARE ZERO ON FAT32 AND THE 32 BIT ONES CARRY THE
    *  VALUE, for both of the two fields that exist twice. This is where a
    *  FAT32 volume gets mistaken for a broken FAT16 one: read BPB_FATSz16,
    *  find zero, and conclude the FAT has no size.
    *
    *  Neither field is chosen by the format - the format has not been
    *  decided yet at this point and cannot be, because deciding it needs the
    *  cluster count and the cluster count needs these numbers. The rule is
    *  the one the specification actually states: the 16 bit field is used
    *  when it is non-zero, and when it is zero the 32 bit one is the truth.
    *  It reads the same for both fields and it does not need to know the
    *  format, which is what makes it correct in this order. */
    fat_sec_per_fat = rd16(bs, BPB_FATSz16);
    if (fat_sec_per_fat == 0)
        fat_sec_per_fat = rd32(bs, BPB_FATSz32);
    if (fat_sec_per_fat == 0)
    {
        fat_err = "FAT size is zero - not a FAT volume";
        return -1;
    }

    total_sectors = rd16(bs, BPB_TotSec16);
    if (total_sectors == 0)
        total_sectors = rd32(bs, BPB_TotSec32);
    if (total_sectors == 0)
    {
        fat_err = "total sector count is zero - not a FAT volume";
        return -1;
    }
    /* Kept, because fat_writable() has to be able to say whether the volume
    *  the BPB describes actually fits on the drive before anything is
    *  written to it. */
    fat_total_sectors = total_sectors;

    /* Layout. Root directory entries are 32 bytes each and the area is
    *  rounded up to whole sectors; on FAT12/16 it always divides evenly in
    *  practice, but the rounding costs nothing and a malformed BPB should
    *  not shift the data area by half a sector.
    *
    *  BPB_RootEntCnt is zero on FAT32, so the area is zero sectors long and
    *  the data area starts directly behind the FATs. That falls out of the
    *  same arithmetic rather than needing a branch, which is the reason this
    *  runs before the type is known and can. */
    root_bytes = fat_root_entries * DIR_ENTRY_SIZE;
    fat_root_sectors = (root_bytes + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE;

    fat_fat_lba = reserved;
    fat_fat_copies = fat_num_fats;
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
    *  Not the "FAT12"/"FAT16"/"FAT32" string at offset 54 or at offset 82.
    *  Those fields are advisory, they are wrong on plenty of real media -
    *  several formatters stamp "FAT32   " into anything they make - and
    *  Microsoft's own specification says explicitly that they must not be
    *  used to determine the type. The type is a function of the number of
    *  data clusters and of nothing else:
    *
    *      data sectors  = total - reserved - FATs - root directory
    *      data clusters = data sectors / sectors per cluster
    *
    *      < 4085   -> FAT12
    *      < 65525  -> FAT16
    *      otherwise   FAT32
    *
    *  The division truncates, and that is correct: a trailing partial
    *  cluster is not addressable and is not counted.
    *
    *  THE THIRD BRANCH IS THE SAME CALCULATION AS THE OTHER TWO. Adding
    *  FAT32 added one comparison against one more constant and did not
    *  change a line of the arithmetic above it, which is the argument for
    *  having written it this way in the first place: the boundary at 65525
    *  is where FAT16 runs out of entries, exactly as 4085 is where FAT12
    *  does, and both are properties of the entry width rather than of
    *  anything a formatter writes down. */
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
        fat_bits = 32;

    /* A BPB THAT CONTRADICTS ITSELF, in either direction.
    *
    *  The cluster count decides the type and nothing else does, which leaves
    *  the question of what to do with a volume whose FIELDS were laid out for
    *  one type while its cluster count says another. BPB_FATSz16 is the field
    *  that gives it away, because it is the one that is zero on FAT32 and
    *  never zero on FAT12/16.
    *
    *  Both directions are refused rather than resolved, and the case is not
    *  hypothetical: mformat will happily produce a volume with a FAT32 BPB
    *  and 65273 clusters if it is told to, and fsck.vfat warns about exactly
    *  that one. Reading it as the cluster count says would give a volume with
    *  no root directory at all - BPB_RootEntCnt is zero, so the fixed root
    *  area is zero sectors long and every listing comes back empty - and
    *  reading it as the fields say would mean letting a formatter's choice
    *  override the one calculation this file insists is the answer.
    *
    *  So neither. A volume this driver cannot read the same way twice is one
    *  it says no to, with a message that names what is wrong rather than
    *  leaving somebody to wonder why their files vanished. */
    if ((fat_bits == 32) != (rd16(bs, BPB_FATSz16) == 0))
    {
        fat_bits = 0;
        fat_err = "boot sector layout and cluster count disagree on the type";
        return -1;
    }

    /* --- What only a FAT32 volume has ------------------------------------
    *
    *  Everything below is read AFTER the type is known, and every one of
    *  these fields would be some other structure's bytes on a FAT12/16
    *  volume: offset 36 onwards is the extended boot record there, so
    *  BPB_RootClus would land in the middle of a volume serial number. */
    if (fat_bits == 32)
    {
        /* A version this driver has never seen. The field is defined so
        *  that a future layout can be refused rather than misread, and
        *  refusing is the only thing to do with it: the fields below may
        *  mean something else entirely in a version that does not exist
        *  yet. */
        if (rd16(bs, BPB_FSVer) != 0)
        {
            fat_bits = 0;
            fat_err = "FAT32 version is not 0 - unsupported";
            return -1;
        }

        /* The specification requires both of these to be zero on FAT32,
        *  and the layout computed above already assumed it. A volume that
        *  breaks it would have had its data area put in the wrong place by
        *  the arithmetic further up, so this is a check on a number that
        *  has already been used rather than a check before use - which is
        *  why it refuses instead of correcting. */
        if (fat_root_entries != 0 || fat_root_sectors != 0)
        {
            fat_bits = 0;
            fat_err = "FAT32 volume with a fixed root directory - malformed";
            return -1;
        }

        fat_root_cluster = rd32(bs, BPB_RootClus) & FAT32_MASK;
        if (!fat_cluster_valid(fat_root_cluster))
        {
            fat_bits = 0;
            fat_err = "root directory starts at a cluster that does not exist";
            return -1;
        }

        /* MIRRORING CAN BE TURNED OFF, and only on FAT32. Bit 7 of
        *  BPB_ExtFlags means exactly one FAT is live and the low four bits
        *  say which; the others hold whatever they held when it was
        *  switched off and are not copies of anything.
        *
        *  Honouring it costs four lines and buys two things. Reads come out
        *  of the copy that is actually maintained rather than out of a
        *  stale one, which on such a volume is the difference between the
        *  current filesystem and last week's. And writes stop overwriting
        *  copies this driver was told are not mirrors, which is the same
        *  courtesy the reserved bits above are given: bytes another driver
        *  owns come back as they were.
        *
        *  Every formatter in ordinary use leaves this at zero, so the
        *  common path is the one line below it. */
        ext_flags = rd16(bs, BPB_ExtFlags);
        if (ext_flags & 0x0080)
        {
            active = ext_flags & 0x000F;
            if (active >= fat_num_fats)
            {
                fat_bits = 0;
                fat_err = "active FAT is one the volume does not have";
                return -1;
            }
            fat_fat_lba = reserved + active * fat_sec_per_fat;
            fat_fat_copies = 1;
        }
    }

    /* The FAT has to be big enough for the entries the cluster count
    *  implies, or fat_byte() would refuse links near the end of the volume
    *  and a file would break off for no visible reason. Entries 0 and 1 are
    *  reserved and are counted. */
    {
        uint32_t need_bytes;

        if (fat_bits == 12)
            need_bytes = ((fat_clusters + 2) * 3 + 1) / 2;
        else if (fat_bits == 16)
            need_bytes = (fat_clusters + 2) * 2;
        else
            need_bytes = (fat_clusters + 2) * 4;

        if (fat_sec_per_fat * (uint32_t)BLK_SECTOR_SIZE < need_bytes)
        {
            fat_err = "FAT is too small for the cluster count";
            fat_bits = 0;
            return -1;
        }
    }

    /* Volume label out of the extended boot record, if there is one. It sits
    *  thirty-two bytes further along on FAT32, because the four fields above
    *  were pushed in front of it - reading the FAT12/16 offsets there would
    *  return the middle of BPB_RootClus as a name. */
    lab_off = (fat_bits == 32) ? (uint32_t)BS32_VolLab : (uint32_t)BS_VolLab;

    fat_vol_label[0] = '\0';
    if (bs[(fat_bits == 32) ? BS32_BootSig : BS_BootSig] == 0x29)
    {
        last = -1;
        for (i = 0; i < 11; i++)
        {
            fat_vol_label[i] = (char)bs[lab_off + i];
            if (fat_vol_label[i] != ' ')
                last = (int)i;
        }
        fat_vol_label[last + 1] = '\0';
    }

    /* Last, because it validates against fat_clusters and reads a sector of
    *  its own through the data cache, so everything it leans on has to be
    *  derived already. A volume without one, or with one that does not
    *  validate, mounts exactly the same - see fat_fsinfo_load(). */
    if (fat_bits == 32)
        fat_fsinfo_load(rd16(bs, BPB_FSInfo), reserved);

    fat_is_mounted = 1;
    return 0;
}

/* Mounting takes the lock like everything else that reads a sector, and it
*  needs it more than most: fat_reset() clears the geometry every other
*  function derives its offsets from, and a task walking a chain while those
*  go to zero would be computing lbas from half a volume. fat_reset() clearing
*  fat_is_mounted first is what makes the failure path safe rather than what
*  makes the concurrency safe. */
int fat_mount(int drive)
{
    int r;

    if (fat_lock_acquire() < 0)
        return -1;

    r = fat_mount_unlocked(drive);

    fat_lock_release();
    return r;
}

/* --- The entry points that do not lock -------------------------------------
*
*  Everything from here to fat_total_kib() reads statics and does arithmetic
*  on them: no sector, no transfer, nothing that can block and so nothing a
*  second task can be interleaved into. The lock section near the top of this
*  file lists them and says why making them block would cost a status line a
*  file copy's worth of waiting for an answer that cannot be wrong in any way
*  a caller could act on. */

int fat_mounted(void)
{
    return fat_is_mounted;
}

const char *fat_type(void)
{
    if (!fat_is_mounted)
        return "";
    if (fat_bits == 12)
        return "FAT12";
    return (fat_bits == 16) ? "FAT16" : "FAT32";
}

const char *fat_label(void)
{
    if (!fat_is_mounted)
        return "";
    return fat_vol_label;
}

/* --- Sizes ---------------------------------------------------------------- */

/* Kept as bytes, and it is the one size here that can stay that way: a
*  cluster is at most 32 KiB by every formatter and at most 32 MiB by the
*  widest reading of the specification, so it fits in 32 bits with room to
*  spare. It is a unit a caller uses to round a file size, not a unit anybody
*  measures a volume in.
*
*  MUST NOT take the lock, and this is the one unlocked entry point where that
*  used to be a requirement rather than a preference - fat_bytes_of() called
*  it from inside the lock. That caller is gone with the move to kibibytes and
*  the requirement with it, but a multiplication of two statics does not need
*  a lock in any case. */
uint32_t fat_cluster_bytes(void)
{
    if (!fat_is_mounted)
        return 0;
    return fat_spc * (uint32_t)BLK_SECTOR_SIZE;
}

/* --- Volume sizes, in kibibytes --------------------------------------------
*
*  WHY NOT BYTES ANY MORE. A byte count in 32 bits stops at 4 GB. That was
*  survivable while a volume was a floppy or a 32 MB image and it stopped
*  being survivable the moment a USB stick was in the picture, because FAT32
*  exists precisely for media above the 2 GB FAT16 ceiling - a byte count
*  cannot describe the volumes the format was added for. Kibibytes carry 4 TB
*  in the same 32 bits, which is past what a 32 bit LBA can address at all, so
*  the unit cannot be the limit again.
*
*  AND THERE IS NO 64 BIT ARITHMETIC HERE to reach for instead. The way this
*  is computed is the way the constraint asks for: think in SECTORS, which is
*  what the volume is measured in and what can never overflow because a 32 bit
*  LBA is the largest sector number the block layer can name, and convert to
*  kibibytes at the very end by dividing by two. The intermediate that would
*  have overflowed - the byte count - is never formed at all.
*
*  ROUNDED DOWN, both of them, which the odd case makes a real decision rather
*  than a consequence of integer division. A volume with one sector per
*  cluster and an odd number of free clusters has half a kibibyte this cannot
*  express, and it is reported as absent. A free count that rounded UP would
*  say there is room for a file that will not fit, and a caller that believed
*  it would get a short write it had been told would not happen. */
static uint32_t fat_kib_of(uint32_t clusters)
{
    uint32_t sectors;

    if (fat_spc == 0)
        return 0;

    /* Cannot overflow on any volume that mounted: fat_clusters was computed
    *  as data_sectors / fat_spc, so this product is at most data_sectors and
    *  therefore at most the sector count of the medium. The test is here for
    *  the caller that has not mounted anything yet and for a BPB with an
    *  absurd sectors-per-cluster, and it saturates rather than wraps because
    *  a size that reads as tiny is worse than one that reads as enormous. */
    if (clusters > 0xFFFFFFFFUL / fat_spc)
        return 0xFFFFFFFFUL;

    sectors = clusters * fat_spc;

    return sectors / (1024UL / BLK_SECTOR_SIZE);
}

uint32_t fat_total_kib(void)
{
    if (!fat_is_mounted)
        return 0;
    return fat_kib_of(fat_clusters);
}

/* Counts free clusters by scanning the FAT. Entries 0 and 1 are reserved
*  and are not part of the data area, so the scan runs over the cluster
*  numbers 2 .. fat_clusters + 1. Sequential, so the FAT sector cache turns
*  the whole scan into one pass over the FAT.
*
*  One pass is still one sector transfer per 512 bytes of FAT, and on a stick
*  that is the most expensive thing in this file by a wide margin: a FAT16
*  volume with a megabyte of FAT is two thousand bulk round trips, seconds
*  rather than the milliseconds the same call costs on IDE. A FAT32 stick is
*  worse again - four bytes an entry over millions of clusters - which is why
*  the count is cached the moment it has been made and why FSInfo exists to
*  save making it at all.
*
*  This remains the FALLBACK and it never goes away: FSInfo is a hint that can
*  be missing, unreadable, unsigned or simply wrong, and every one of those
*  paths ends here. It is also the longest anything holds the lock, which is
*  why FAT_LOCK_MS is set where it is.
*
*  Returns 0 with *out set, or negative when the FAT could not be read - which
*  is a different answer from a count of zero, and the reason this does not
*  simply return the number. */
static int fat_count_free(uint32_t *out)
{
    uint32_t c;
    uint32_t v;
    uint32_t free_clusters = 0;

    for (c = 2; c <= fat_clusters + 1; c++)
    {
        if (fat_entry(c, &v) < 0)
            return -1;
        if (v == 0)
            free_clusters++;
    }

    *out = free_clusters;
    return 0;
}

static uint32_t fat_free_kib_unlocked(void)
{
    uint32_t n;

    if (!fat_is_mounted)
        return 0;

    /* Counted once and then kept - by FSInfo at mount time on FAT32, by this
    *  scan otherwise - and adjusted by one at each of the two places that
    *  change how many clusters are free. See the note above fat_free_count. */
    if (!fat_free_known)
    {
        if (fat_count_free(&n) < 0)
            return 0;
        fat_free_count = n;
        fat_free_known = 1;

        /* Worth writing back once somebody has paid for it, so that the next
        *  mount of this volume does not pay again. Not written from here:
        *  this is a query, a query that writes to the medium is a surprise,
        *  and the next operation that changes the volume anyway will carry
        *  it out for free. */
        fat_fsinfo_dirty = 1;
    }

    return fat_kib_of(fat_free_count);
}

uint32_t fat_free_kib(void)
{
    uint32_t r;

    /* Nothing to report rather than a number that was never counted. This
    *  reads the same as an unreadable FAT, which is the existing answer for
    *  "the count could not be made", and fat_last_error() says which. */
    if (fat_lock_acquire() < 0)
        return 0;

    r = fat_free_kib_unlocked();

    fat_lock_release();
    return r;
}

/* --- Public directory and file access --------------------------------------
*
*  From here down every entry point is a shell: take the lock, run the body,
*  give the lock back. The bodies are what the functions used to be, unchanged
*  apart from their names, and none of them has to release anything on the way
*  out of its twenty-odd error returns - which is precisely why the split is
*  the shape it is. See fat_lock_acquire(). */

static int fat_readdir_unlocked(const char *path, int index, fat_dirent *out)
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

int fat_readdir(const char *path, int index, fat_dirent *out)
{
    int r;

    if (fat_lock_acquire() < 0)
        return -1;

    r = fat_readdir_unlocked(path, index, out);

    fat_lock_release();
    return r;
}

static int fat_size_unlocked(const char *path, uint32_t *size)
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

int fat_size(const char *path, uint32_t *size)
{
    int r;

    if (fat_lock_acquire() < 0)
        return -1;

    r = fat_size_unlocked(path, size);

    fat_lock_release();
    return r;
}

static int fat_read_unlocked(const char *path, uint32_t offset, uint32_t len,
                             void *buf)
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

    cluster_bytes = fat_spc * (uint32_t)BLK_SECTOR_SIZE;
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
                                + pos / BLK_SECTOR_SIZE) < 0)
            return -1;

        chunk = BLK_SECTOR_SIZE - (pos % BLK_SECTOR_SIZE);
        if (chunk > len - done)
            chunk = len - done;

        memcpy((uint8_t *)buf + done, dat_buf + (pos % BLK_SECTOR_SIZE),
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

/* The whole read is one hold of the lock, from the path resolution to the
*  last sector, and that is the point rather than a side effect: the chain
*  walk caches a FAT sector, the copy out caches a data sector, and the two
*  alternate. Letting go between sectors would hand the caches to somebody
*  else and re-read both on every step - and worse, it would let another task
*  delete the file half way through this one reading it.
*
*  syscall.c caps a single SYS_READ at a page for its own reasons, which
*  happens to bound how long ring 3 can keep this held. */
int fat_read(const char *path, uint32_t offset, uint32_t len, void *buf)
{
    int r;

    if (fat_lock_acquire() < 0)
        return -1;

    r = fat_read_unlocked(path, offset, len, buf);

    fat_lock_release();
    return r;
}

/* Deliberately not locked, and it is the one entry point where that is a
*  compromise rather than a free choice.
*
*  fat_err is a single pointer for the whole file, so a caller reads back the
*  last message ANY task produced rather than the one its own call produced,
*  and two tasks failing at once can hand one of them the other's reason. That
*  was already true before there was a lock - it is what one static and two
*  tasks means - and taking the lock here would not fix it: the overwrite
*  happens inside the other task's own critical section, not next to it.
*
*  Fixing it properly needs the message to travel with the call, i.e. an out
*  parameter or a per-task slot, and both change what fat.h promises. What is
*  here instead is that the message is only ever wrong about WHICH failure it
*  describes, never wrong about the volume: every string is either a constant
*  or fat_msg, which names the device it is about.
*
*  Blocking here would also be wrong on its own terms. This is what a caller
*  reaches for immediately after something failed, and making it wait behind
*  the task that is still running is turning an error report into a second
*  delay. */
const char *fat_last_error(void)
{
    return fat_err;
}

/* ===========================================================================
*  Writing
*
*  Everything below this line can destroy data. The four rules it follows are
*  in the file header; what is repeated here is only the reasoning that does
*  not fit above a single function.
* ======================================================================== */

/* Can this volume be written to at all?
*
*  Three things have to hold, and they are checked here rather than at the
*  first blk_write() because fat.h promises a caller can ask BEFORE the user
*  has typed a filename - failing afterwards is a worse answer. On a
*  removable device that promise is worth more than it ever was on a disk:
*  the honest answer changes while the machine is running.
*
*    - something is mounted,
*    - the device still reports a size, and
*    - no earlier FAT update was left half applied across the copies, and
*    - the volume the BPB describes fits inside the size the device reports.
*      A BPB claiming more sectors than the medium has is either a truncated
*      image or a lie, and either way the last cluster of the data area is
*      off the end of the medium. blk_write() would refuse that one sector;
*      refusing the whole volume is the honest answer, because a filesystem
*      whose tail cannot be written is not one to hand a user.
*
*  The second test used to read "the drive answered an IDENTIFY", which was
*  ATA's way of saying a drive was there. A stick answers no IDENTIFY - its
*  size comes back from a SCSI READ CAPACITY, four layers away - so the
*  question has to be asked the way blockdev.h defines it, and blockdev.h
*  deliberately makes presence and size ONE answer: a device with no size is
*  not a device anyone can use, and two answers that could disagree would be
*  two chances to get it wrong. So there is no separate presence call below,
*  and a stick that was pulled out reports zero sectors and fails here.
*
*  Split in two because it is one of the two places where a public entry point
*  in this file reaches another one: fat_write_ready() asks this question with
*  the lock already held, so the answer has to be reachable without taking it
*  again. The shell below is what an outside caller gets, and the body is what
*  fat_write_ready() calls. blk_sectors() is a cached number on both drivers,
*  so neither version blocks on the device. */
static int fat_writable_unlocked(void)
{
    uint32_t sectors;

    if (!fat_is_mounted)
        return 0;

    /* Latched by a FAT update that failed between the copies. Cleared only
    *  by a fresh fat_mount(), which re-reads the layout and gives whoever
    *  is in charge a chance to run a repair tool first. */
    if (fat_write_locked)
        return 0;

    sectors = blk_sectors(fat_drive);
    if (sectors == 0 || sectors < fat_total_sectors)
        return 0;

    /* fat_clusters * fat_spc cannot overflow: fat_clusters was computed as
    *  data_sectors / fat_spc, so the product is at most data_sectors. */
    if (fat_data_lba + fat_clusters * fat_spc > sectors)
        return 0;

    return 1;
}

int fat_writable(void)
{
    int r;

    /* Not writable is the answer a lock this call could not get has to give.
    *  It is what fat.h asks this function for - can anything be saved - and
    *  "no, and here is why" is a better thing to tell a user before they have
    *  typed a filename than a yes that the next call will contradict. */
    if (fat_lock_acquire() < 0)
        return 0;

    r = fat_writable_unlocked();

    fat_lock_release();
    return r;
}

uint32_t fat_bytes_written(void)
{
    return fat_written;
}

/* Shared entry check for every function that modifies the volume. */
static int fat_write_ready(void)
{
    if (!fat_is_mounted)
    {
        fat_err = "no volume mounted";
        return -1;
    }
    if (!fat_writable_unlocked())
    {
        /* Named rather than left as a bare sentence, because the most
        *  ordinary way to reach it is now that the medium is gone: on a
        *  removable device this is what "somebody pulled the stick out"
        *  looks like from up here. No blk_last_error() with it - nothing
        *  was just asked of the device, so whatever is in there belongs to
        *  some older request and would be a guess dressed up as a reason. */
        fat_dev_err("the mounted volume cannot be written to", 0);
        return -1;
    }
    fat_err = "";
    return 0;
}

/* --- Timestamps ------------------------------------------------------------
*
*  FAT packs a date into sixteen bits - seven bits of year counted from
*  1980, four of month, five of day - and a time into another sixteen, with
*  the seconds halved because five bits do not hold sixty. Every tool shows
*  these fields, so they are worth writing.
*
*  But only when the clock is believable. An entry with a zero date is legal
*  and every tool renders it as blank or as 1980-00-00; a WRONG one is worse
*  than none, because nothing flags it - the file just sorts to the wrong
*  place forever, and on a machine whose CMOS battery is flat that is every
*  file it ever writes. So the reading is range checked, and a reading that
*  fails the check produces zeros rather than a plausible looking guess.
*
*  The range is the one the packing can represent: 1980 through 2107. The
*  CMOS code in timer.c adds 2000 to a two digit year, so anything below
*  1980 is unreachable in practice and the low end of the check only guards
*  against that changing.
*
*  Returns 1 and fills *date and *time, or 0 when the clock is not usable. */
static int fat_now(uint32_t *date, uint32_t *time)
{
    datetime t;

    t = cmos_readtime();

    if (t.years < 1980 || t.years > 2107)
        return 0;
    if (t.months < 1 || t.months > 12)
        return 0;
    if (t.days < 1 || t.days > 31)
        return 0;
    if (t.hours > 23 || t.minutes > 59 || t.seconds > 59)
        return 0;

    *date = ((t.years - 1980) << 9) | (t.months << 5) | t.days;
    *time = (t.hours << 11) | (t.minutes << 5) | (t.seconds / 2);
    return 1;
}

/* --- Allocation ------------------------------------------------------------
*
*  Fills a whole cluster with zeros.
*
*  Every cluster this driver hands to a file is zeroed before the file can
*  see it. That is not free - it doubles the sector writes of a sequential
*  write, and on a stick a sector write is a bulk transaction rather than a
*  burst of outw, so the doubling is felt - and it is done anyway for two
*  reasons, neither of which is about speed. It means a gap left by
*  seeking past the end of a file reads back as zeros without any of the
*  size arithmetic elsewhere having to be exactly right, and it means the
*  slack at the end of the last cluster cannot hand a new file the contents
*  of a deleted one. The second is the reason that matters: an arithmetic
*  slip in a gap calculation would otherwise leak somebody else's file. */
static int fat_zero_cluster(uint32_t c)
{
    uint32_t lba;
    uint32_t i;

    if (!fat_cluster_valid(c))
    {
        fat_err = "refusing to zero a cluster that is not one";
        return -1;
    }

    lba = fat_cluster_lba(c);
    memset(dat_buf, 0, BLK_SECTOR_SIZE);

    for (i = 0; i < fat_spc; i++)
    {
        fat_claim_data(lba + i);
        if (fat_put_data_sector() < 0)
            return -1;
    }
    return 0;
}

/* Takes a free cluster and marks it as the end of a chain.
*
*  Returns 0 with *out set, 1 when the volume is full, or negative on an I/O
*  error. Full is not an error: fat.h makes a short write part of the
*  contract, so the caller has to be able to tell "no room" from "the disk
*  said no", and both from success.
*
*  The entry is set to the end-of-chain marker before the function returns,
*  which is what makes the cluster taken. It has to happen here rather than
*  at the caller, because a caller that allocates two clusters in a row
*  would otherwise be handed the same one twice - the second scan would
*  still see it as free. The cost is that a crash between this and the data
*  going in leaves a one cluster chain nobody references: a leak, reported
*  by fsck as an unreferenced cluster and reclaimed by it, and nothing more.
*  The other order - write the data, then mark the cluster used - would let
*  a crash leave a cluster that reads as free while holding a file's bytes,
*  so the next allocation hands it to a second file. That is a cross link,
*  and no repair tool can undo it without losing one of the two files. */
static int fat_alloc_cluster(uint32_t *out)
{
    uint32_t c;
    uint32_t v;
    uint32_t scanned;

    c = fat_alloc_hint;
    if (!fat_cluster_valid(c))
        c = 2;

    for (scanned = 0; scanned < fat_clusters; scanned++)
    {
        if (fat_entry(c, &v) < 0)
            return -1;

        if (v == 0)
        {
            if (fat_set_entry(c, fat_eoc()) < 0)
            {
                /* The write may have landed in some copies and not others,
                *  or in one sector of a split FAT12 entry and not the other,
                *  so whether this cluster is now taken is not something this
                *  driver can say. A count that might be one out is worse
                *  than no count: the next question pays for a scan. */
                fat_free_known = 0;
                fat_fsinfo_dirty = 1;
                return -1;
            }

            fat_alloc_hint = (c + 1 > fat_clusters + 1) ? 2 : c + 1;

            /* One cluster fewer, and the FSInfo copy of both numbers is now
            *  behind. Free rather than a second pass over the FAT: the entry
            *  that changed was read a few lines up, so its old value is
            *  known and the new one was just written.
            *
            *  A count of zero here is a count that has just been DISPROVED -
            *  a free cluster was found on a volume the number said had none
            *  - so it is thrown away rather than clamped. That is the second
            *  half of how FSInfo is validated: the checks at mount time catch
            *  a number that is impossible, and this catches one that is
            *  merely wrong, at the first moment the FAT itself contradicts
            *  it. Clamping would keep the wrong number for ever. */
            if (fat_free_known)
            {
                if (fat_free_count == 0)
                    fat_free_known = 0;
                else
                    fat_free_count--;
            }
            fat_fsinfo_dirty = 1;

            *out = c;
            return 0;
        }

        c++;
        if (c > fat_clusters + 1)
            c = 2;                  /* wrap: the hint may start anywhere */
    }

    fat_err = "no free clusters left on the volume";
    return 1;
}

/* Releases a chain, from its first cluster to its end.
*
*  The link has to be read before the entry is cleared, or the rest of the
*  chain becomes unreachable half way through. A failure part way leaves the
*  tail allocated - a leak, which is the failure mode this whole file leans
*  towards on purpose. */
static int fat_free_chain(uint32_t c)
{
    uint32_t next;
    uint32_t steps = 0;
    int r;

    while (fat_cluster_valid(c))
    {
        if (steps > fat_clusters)
        {
            fat_err = "cyclic cluster chain";
            return -1;
        }
        steps++;

        r = fat_next_cluster(c, &next);
        if (r < 0)
            return -1;              /* corrupt or unreadable: stop here */

        if (fat_set_entry(c, 0) < 0)
        {
            /* Same argument as in fat_alloc_cluster(): a FAT write that
            *  failed part way leaves this driver unable to say whether the
            *  cluster is free, so it stops claiming to know the count. */
            fat_free_known = 0;
            fat_fsinfo_dirty = 1;
            return -1;
        }

        /* And the mirror image of the disproof in fat_alloc_cluster(): a
        *  cluster was just released on a volume the count said was already
        *  entirely free, so the count is wrong and is dropped rather than
        *  held at the ceiling. */
        if (fat_free_known)
        {
            if (fat_free_count >= fat_clusters)
                fat_free_known = 0;
            else
                fat_free_count++;
        }
        fat_fsinfo_dirty = 1;

        /* Freeing below the hint makes the space visible to the next
        *  allocation without a wrap around scan. */
        if (c < fat_alloc_hint)
            fat_alloc_hint = c;

        if (r > 0)
            return 0;               /* c was the last cluster */
        c = next;
    }

    return 0;
}

/* Walks to the last cluster of a chain. */
static int fat_chain_last(uint32_t c, uint32_t *out)
{
    uint32_t next;
    uint32_t steps = 0;
    int r;

    if (!fat_cluster_valid(c))
    {
        fat_err = "invalid start cluster";
        return -1;
    }

    for (;;)
    {
        if (steps > fat_clusters)
        {
            fat_err = "cyclic cluster chain";
            return -1;
        }
        steps++;

        r = fat_next_cluster(c, &next);
        if (r < 0)
            return -1;
        if (r > 0)
        {
            *out = c;
            return 0;
        }
        c = next;
    }
}

/* --- 8.3 names, the other way round ----------------------------------------
*
*  Is this byte allowed in an 8.3 name? The forbidden set is the one FAT has
*  always had: the control characters, the space (which is padding, not a
*  character), the shell and path punctuation, and 0x7F.
*
*  Bytes from 0x80 up are allowed through unchanged, because fat_name_of()
*  already passes them through in the other direction - they are code page
*  characters, and refusing them here would make a file the read path can
*  display impossible to create. */
static int fat_name_char_ok(uint8_t c)
{
    if (c < 0x20 || c == 0x7F)
        return 0;
    if (c >= 0x80)
        return 1;

    switch (c)
    {
    case ' ':  case '"':  case '*':  case '+':  case ',':
    case '.':  case '/':  case ':':  case ';':  case '<':
    case '=':  case '>':  case '?':  case '[':  case '\\':
    case ']':  case '|':
        return 0;
    default:
        return 1;
    }
}

/* Turns a printable name into the eleven padded bytes of a directory entry.
*  out must hold 11 bytes. Returns 0, or negative with fat_err set.
*
*  This is the exact inverse of fat_name_of(), and it has to be, or a file
*  this driver creates is invisible to the driver's own read path. The three
*  places that pin it down:
*
*    - fat_name_of() strips trailing spaces from base and extension, so the
*      entry is space padded here.
*    - fat_name_of() only ever emits a dot when the extension is non empty,
*      so "FOO." is refused: it would come back as "FOO" and the caller
*      would not be able to name its own file again.
*    - fat_name_of() maps a leading 0x05 back to 0xE5, so a name starting
*      with 0xE5 is stored as 0x05 here. Without that substitution the entry
*      would be indistinguishable from a deleted one and would vanish on the
*      next directory scan.
*
*  Anything that does not fit is REFUSED rather than mangled or truncated. A
*  file the caller cannot name again is worse than a file that was not
*  created, and a silently truncated name is exactly that file. "." and ".."
*  fall out of the empty base rule and are refused with everything else,
*  which is deliberate: creating an entry called ".." in a directory would
*  redirect its parent link. */
static int fat_make_83(const char *name, uint8_t *out)
{
    const char *p = name;
    uint32_t i;
    int n;

    for (i = 0; i < 11; i++)
        out[i] = ' ';

    if (name == 0 || *name == '\0')
    {
        fat_err = "empty file name";
        return -1;
    }

    n = 0;
    while (*p != '\0' && *p != '.')
    {
        if (n >= 8)
        {
            fat_err = "name is longer than eight characters";
            return -1;
        }
        if (!fat_name_char_ok((uint8_t)*p))
        {
            fat_err = "name contains a character FAT does not allow";
            return -1;
        }
        out[n] = (uint8_t)fat_upper(*p);
        n++;
        p++;
    }

    if (n == 0)
    {
        fat_err = "name has no part before the dot";
        return -1;
    }

    if (*p == '.')
    {
        p++;
        n = 0;
        while (*p != '\0')
        {
            if (*p == '.')
            {
                fat_err = "name has more than one dot";
                return -1;
            }
            if (n >= 3)
            {
                fat_err = "extension is longer than three characters";
                return -1;
            }
            if (!fat_name_char_ok((uint8_t)*p))
            {
                fat_err = "name contains a character FAT does not allow";
                return -1;
            }
            out[8 + n] = (uint8_t)fat_upper(*p);
            n++;
            p++;
        }

        if (n == 0)
        {
            fat_err = "name ends in a dot";
            return -1;
        }
    }

    if (out[0] == DIRENT_DELETED)
        out[0] = DIRENT_E5;

    return 0;
}

/* --- Paths, split at the last component ------------------------------------
*
*  Creating, deleting and writing all need the directory a name lives in,
*  which fat_resolve() does not hand back - it resolves the name itself.
*  This walks every component but the last, leaving *dir on the containing
*  directory and last holding the final component.
*
*  One component is always held back rather than looked up, which is what
*  makes the final one exempt: a file that does not exist yet must not be a
*  lookup failure here. Returns 0, or negative. */
static int fat_resolve_parent(const char *path, fat_dir *dir, char *last)
{
    char comp[FAT_NAME_MAX];
    fat_dirent de;
    const char *p = path;
    int have = 0;
    int r;

    if (path == 0)
    {
        fat_err = "no path given";
        return -1;
    }

    fat_dir_root(dir);

    for (;;)
    {
        r = fat_path_next(&p, comp);
        if (r < 0)
        {
            fat_err = "path component is not a valid 8.3 name";
            return -1;
        }
        if (r == 0)
            break;

        if (have)
        {
            /* The component held back turned out to be an intermediate one
            *  after all: it has to exist and it has to be a directory. */
            r = fat_dir_lookup(dir, last, &de);
            if (r != 0)
            {
                if (r > 0)
                    fat_err = "no such file or directory";
                return -1;
            }
            if (!de.is_dir)
            {
                fat_err = "path component is not a directory";
                return -1;
            }
            /* ".." in a first level subdirectory carries cluster 0, which
            *  is FAT's way of saying "the root" - see fat_dir_at(). */
            fat_dir_at(dir, de.first_cluster);
        }

        strcpy(last, comp);
        have = 1;
    }

    if (!have)
    {
        fat_err = "path names the root directory, not a file";
        return -1;
    }
    return 0;
}

/* --- Directory slots -------------------------------------------------------
*
*  Finds a slot for a new entry.
*
*  A free slot is one that was never used (first byte 0x00) or one freed
*  earlier (0xE5), and the difference is not cosmetic: 0x00 means this entry
*  AND EVERY ENTRY AFTER IT is unused, so a scan stops there. That is why
*  the loop below can stop at the first 0x00 without looking further, and
*  also why writing an entry into a 0x00 slot needs no terminator behind it:
*  the slot after it was already 0x00 by the same rule, and on a volume
*  where that is not true the directory was already broken before this
*  driver touched it.
*
*  A full root directory is a hard error ON FAT12/16 - there the root is a
*  fixed run of sectors and cannot grow, and the count of them was fixed when
*  the volume was formatted. A full subdirectory is a chain like any other and
*  is extended by one zeroed cluster, whose 32 byte entries are all 0x00 and
*  therefore exactly a valid empty tail.
*
*  On FAT32 the root is one of those chains, so it takes the second path and
*  grows. Nothing here tests for the format to decide that: d->is_root is only
*  ever set for the fixed area, so the volumes that do not have one never
*  reach the error at all. */
static int fat_dir_free_slot(const fat_dir *d, fat_slot *slot)
{
    uint32_t lba, within;
    uint32_t n = 0;
    uint32_t last, nc;
    uint8_t first;
    int r;

    for (;;)
    {
        r = fat_dir_locate(d, n, &lba, &within);
        if (r < 0)
            return -1;
        if (r > 0)
            break;                  /* ran out of directory */

        if (fat_get_data_sector(lba) < 0)
            return -1;

        first = dat_buf[within * DIR_ENTRY_SIZE];
        if (first == DIRENT_FREE || first == DIRENT_DELETED)
        {
            slot->lba = lba;
            slot->within = within;
            return 0;
        }
        n++;
    }

    if (d->is_root)
    {
        fat_err = "the root directory is full";
        return -1;
    }

    r = fat_chain_last(d->cluster, &last);
    if (r < 0)
        return -1;

    r = fat_alloc_cluster(&nc);
    if (r != 0)
    {
        if (r > 0)
            fat_err = "no room left to extend the directory";
        return -1;
    }
    if (fat_zero_cluster(nc) < 0)
        return -1;
    /* Linked only once it holds zeros. The other order would splice a
    *  cluster of stale bytes into a directory, and stale bytes in a
    *  directory are entries.
    *
    *  "Holds zeros" has to mean the medium holds them. The barrier also
    *  carries the mark-as-used from fat_alloc_cluster() over ahead of the
    *  link, which is the same rule one step earlier: a link that lands
    *  without the mark leaves a cluster inside a chain that reads as free,
    *  and the next allocation hands it to somebody else. Both writes are
    *  before this point and one barrier orders them both.
    *
    *  Failing here leaves nc taken and unreferenced - a leak, reclaimed by
    *  fsck - and the directory exactly one cluster shorter than it wanted to
    *  be, which is the state it was in when this function was entered. */
    if (fat_sync("the device would not confirm the new directory cluster"
                 " before it is linked") < 0)
        return -1;
    if (fat_set_entry(last, nc) < 0)
        return -1;

    slot->lba = fat_cluster_lba(nc);
    slot->within = 0;
    return 0;
}

/* --- Files -----------------------------------------------------------------
*
*  Everything a write needs to know about a file: where its directory entry
*  is, what the entry currently says, and a cursor into its chain so that
*  writing sequentially does not re-walk the chain from the front for every
*  cluster. */
typedef struct
{
    fat_slot slot;
    uint32_t first;         /* first cluster, 0 when the file has none      */
    uint32_t size;
    uint8_t  attr;
    uint32_t cur_index;     /* cursor: chain index ...                      */
    uint32_t cur_cluster;   /* ... and the cluster that sits there          */
} fat_file;

/* Opens an existing file for writing. Refuses a directory, refuses a read
*  only file, and refuses an entry whose size and chain contradict each
*  other rather than trying to make sense of it. */
static int fat_open(const char *path, fat_file *f)
{
    fat_dir dir;
    char last[FAT_NAME_MAX];
    uint8_t e[DIR_ENTRY_SIZE];
    int r;

    if (fat_resolve_parent(path, &dir, last) < 0)
        return -1;

    r = fat_dir_find(&dir, last, e, &f->slot);
    if (r != 0)
    {
        if (r > 0)
            fat_err = "no such file or directory";
        return -1;
    }

    if (e[DIR_Attr] & ATTR_DIRECTORY)
    {
        fat_err = "is a directory";
        return -1;
    }
    if (e[DIR_Attr] & ATTR_READ_ONLY)
    {
        fat_err = "file is read only";
        return -1;
    }

    f->first = fat_first_cluster(e);
    f->size = rd32(e, DIR_FileSize);
    f->attr = e[DIR_Attr];
    f->cur_index = 0;
    f->cur_cluster = f->first;

    if (f->size != 0 && f->first == 0)
    {
        fat_err = "file has a size but no cluster";
        return -1;
    }
    if (f->first != 0 && !fat_cluster_valid(f->first))
    {
        fat_err = "file starts at a cluster that does not exist";
        return -1;
    }

    return 0;
}

/* Writes the file's size and first cluster back into its directory entry.
*
*  THIS IS THE COMMIT POINT. It is one sector write, and until it lands
*  nothing a caller can see has changed: the chain may have grown, clusters
*  may have been taken, but the entry still describes the old file, and a
*  power cut here costs some unreferenced clusters and no data at all. Every
*  function below is arranged around calling this at the moment that leaves
*  the file describing the smaller of its two states.
*
*  The last write time is stamped here rather than at each of the callers,
*  so that no path can modify a file and forget to age it.
*
*  AND IT IS WHERE THE ORDERING BARRIER BELONGS, for the same reason it is the
*  commit point. Rule 4 says the entry is written at the moment that makes the
*  file describe the smaller of its two states, which is a statement about the
*  medium: an entry that reaches the flash before the data and the chain it
*  describes is an entry that names bytes nobody wrote. One barrier here
*  covers everything the operation issued - every data sector fat_put() wrote,
*  every cluster it took, every link it made - because a flush is about the
*  device and not about a particular sector. That is the whole reason this
*  costs one round trip per file operation rather than one per sector.
*
*  It sits inside this function rather than at its two callers so that a third
*  caller cannot be added without it. Growing is the only direction that needs
*  it; the shrinking paths commit first and then have their own barrier on the
*  other side, before anything is released. Those therefore pay one round trip
*  with nothing outstanding to order, which is the price of the barrier being
*  a property of the commit point rather than a flag a caller passes - and a
*  flag is exactly the kind of thing a later caller forgets to set.
*
*  Failing means not committing, which leaves the file exactly as it was plus
*  whatever clusters the attempt took - unreferenced, reclaimable, and the
*  failure mode this file leans towards everywhere else. */
static int fat_commit(fat_file *f)
{
    uint8_t *e;
    uint32_t date = 0;
    uint32_t time = 0;

    if (fat_sync("the device would not confirm the data and the chain"
                 " before the directory entry") < 0)
        return -1;

    if (fat_get_data_sector(f->slot.lba) < 0)
        return -1;

    e = dat_buf + f->slot.within * DIR_ENTRY_SIZE;

    wr16(e, DIR_FstClusLO, f->first & 0xFFFF);
    wr32(e, DIR_FileSize, f->size);

    /* The other half of the cluster number, and it is written on every
    *  format rather than only on FAT32 - with a different value and for a
    *  different reason.
    *
    *  On FAT32 it is the top twelve bits of the first cluster, and a driver
    *  that left it alone would produce a file whose chain starts somewhere in
    *  the first 65536 clusters whatever the file actually uses.
    *
    *  On FAT12/16 it is reserved and must be zero. Nothing here reads it -
    *  fat_first_cluster() ignores it below FAT32 precisely because it cannot
    *  be trusted there - but a non zero value would make a FAT32 aware tool
    *  follow a cluster number this volume does not have, so it is written as
    *  zero rather than merely not written. */
    wr16(e, DIR_FstClusHI, (fat_bits == 32) ? ((f->first >> 16) & 0xFFFF) : 0);

    /* The archive bit means "changed since the last backup", and this is
    *  precisely that change. */
    e[DIR_Attr] = (uint8_t)(f->attr | ATTR_ARCHIVE);

    if (fat_now(&date, &time))
    {
        wr16(e, DIR_WrtTime, time);
        wr16(e, DIR_WrtDate, date);
        wr16(e, DIR_LstAccDate, date);
    }

    return fat_put_data_sector();
}

/* The cluster holding chain index idx, allocating and linking when asked.
*  Returns 0 with *out set, 1 when the chain does not reach that far and
*  either allocation was not asked for or the volume is full, or negative.
*
*  The cursor turns sequential writing into one FAT lookup per cluster
*  instead of one per cluster squared; it is only ever a cache of a walk
*  this function could redo from f->first, and any doubt resets it. */
static int fat_file_cluster(fat_file *f, uint32_t idx, int allocate,
                            uint32_t *out)
{
    uint32_t c, nc, next;
    int r;

    if (f->first == 0)
    {
        if (!allocate)
            return 1;

        r = fat_alloc_cluster(&nc);
        if (r != 0)
            return r;
        if (fat_zero_cluster(nc) < 0)
            return -1;

        /* No link to write: the file had no chain, so the new cluster IS
        *  the chain. It becomes reachable only when fat_commit() puts it in
        *  the directory entry, which is exactly the ordering wanted. */
        f->first = nc;
        f->cur_index = 0;
        f->cur_cluster = nc;
    }

    if (f->cur_cluster == 0 || f->cur_index > idx)
    {
        f->cur_index = 0;
        f->cur_cluster = f->first;
    }

    c = f->cur_cluster;

    while (f->cur_index < idx)
    {
        if (f->cur_index > fat_clusters)
        {
            fat_err = "cyclic cluster chain";
            return -1;
        }

        r = fat_next_cluster(c, &next);
        if (r < 0)
            return -1;

        if (r > 0)
        {
            if (!allocate)
                return 1;

            r = fat_alloc_cluster(&nc);
            if (r != 0)
                return r;
            if (fat_zero_cluster(nc) < 0)
                return -1;
            /* Linked last, and only once the cluster holds zeros: until
            *  this store the chain still ends at c, so a crash leaks nc
            *  rather than extending the file with whatever nc used to
            *  contain.
            *
            *  Rule 3, and the barrier is what makes "only once" true on a
            *  device that buffers. It orders two earlier writes ahead of the
            *  link, not one: the zeros, and fat_alloc_cluster()'s mark of nc
            *  as an end of chain. A link that landed without the mark would
            *  put a cluster that still reads as free inside a chain, and the
            *  next allocation would hand it to a second file - a cross link,
            *  which is the one outcome no repair tool can undo.
            *
            *  Failing leaves nc taken and referenced by nobody: a leak, and
            *  the file at the length it already had. */
            if (fat_sync("the device would not confirm the zeroed cluster"
                         " before it is linked into the chain") < 0)
                return -1;
            if (fat_set_entry(c, nc) < 0)
                return -1;
            next = nc;
        }

        c = next;
        f->cur_index++;
        f->cur_cluster = c;
    }

    *out = c;
    return 0;
}

/* Puts len bytes at byte offset pos into the file, growing the chain as it
*  goes. src == 0 writes zeros, which is how a gap is filled.
*
*  Returns the number of bytes that actually landed. A short result means
*  the volume filled up or a sector write failed; either way the bytes
*  reported are on the disk and the ones after them are not, which is what
*  lets the caller commit a size that describes exactly the truth.
*
*  Deliberately does NOT touch the directory entry. Every caller commits,
*  once, when it knows the final size. */
static uint32_t fat_put(fat_file *f, uint32_t pos, uint32_t len,
                        const uint8_t *src)
{
    uint32_t cluster_bytes = fat_spc * (uint32_t)BLK_SECTOR_SIZE;
    uint32_t done = 0;
    uint32_t idx, off, lba, in, chunk, cluster;
    int r;

    while (done < len)
    {
        idx = (pos + done) / cluster_bytes;
        off = (pos + done) % cluster_bytes;

        if (idx >= fat_clusters)
        {
            fat_err = "file would be longer than the volume";
            break;
        }

        r = fat_file_cluster(f, idx, 1, &cluster);
        if (r != 0)
            break;                  /* full or broken; fat_err is set */

        lba = fat_cluster_lba(cluster) + off / BLK_SECTOR_SIZE;
        in = off % BLK_SECTOR_SIZE;
        chunk = BLK_SECTOR_SIZE - in;
        if (chunk > len - done)
            chunk = len - done;

        if (chunk == BLK_SECTOR_SIZE)
        {
            /* A whole sector is replaced, so reading it first would throw
            *  away every byte it returned. in is necessarily 0 here, and
            *  the memcpy/memset below covers all 512 bytes. */
            fat_claim_data(lba);
        }
        else if (fat_get_data_sector(lba) < 0)
        {
            break;
        }

        if (src != 0)
            memcpy(dat_buf + in, src + done, chunk);
        else
            memset(dat_buf + in, 0, chunk);

        if (fat_put_data_sector() < 0)
            break;

        done += chunk;
    }

    return done;
}

/* --- The public write half -------------------------------------------------
*
*  Same shape as the read half: a shell that locks, a body that does the work
*  and returns from wherever it likes. On this side the lock is doing more
*  than keeping the sector caches straight - it is what makes an operation
*  atomic against the rest of the machine. Two tasks interleaving
*  fat_alloc_cluster() would each scan the FAT, each find the same free entry
*  and each take it, which is a cross link built without a crash and without a
*  single bug in the ordering the file spends four rules on. */

static int fat_create_unlocked(const char *path)
{
    fat_dir dir;
    fat_slot slot;
    char last[FAT_NAME_MAX];
    uint8_t name83[11];
    uint8_t e[DIR_ENTRY_SIZE];
    uint32_t date = 0;
    uint32_t time = 0;
    int r;

    if (fat_write_ready() < 0)
        return -1;

    if (fat_resolve_parent(path, &dir, last) < 0)
        return -1;

    /* The name is converted BEFORE anything is looked up or written, so a
    *  name that cannot be stored costs no disk access at all and cannot
    *  half create anything. */
    if (fat_make_83(last, name83) < 0)
        return -1;

    r = fat_dir_find(&dir, last, e, 0);
    if (r < 0)
        return -1;
    if (r == 0)
    {
        fat_err = "file already exists";
        return -1;
    }

    if (fat_dir_free_slot(&dir, &slot) < 0)
        return -1;

    /* Built whole in memory and stored with a single sector write: there is
    *  no moment at which half an entry is on the disk. */
    memset(e, 0, DIR_ENTRY_SIZE);
    memcpy(e, name83, 11);
    e[DIR_Attr] = ATTR_ARCHIVE;

    /* First cluster 0 and size 0. An empty file owns no clusters - that is
    *  what every FAT driver writes, and what fat_read() here already
    *  understands. Allocating one up front would make an empty file cost a
    *  cluster and would need a second write to undo. */
    if (fat_now(&date, &time))
    {
        wr16(e, DIR_CrtTime, time);
        wr16(e, DIR_CrtDate, date);
        wr16(e, DIR_LstAccDate, date);
        wr16(e, DIR_WrtTime, time);
        wr16(e, DIR_WrtDate, date);
    }

    if (fat_get_data_sector(slot.lba) < 0)
        return -1;
    memcpy(dat_buf + slot.within * DIR_ENTRY_SIZE, e, DIR_ENTRY_SIZE);

    /* No barrier after this one, and that is deliberate: the entry write is
    *  the last thing the operation does, so there is no later write whose
    *  ordering against it could matter. If the directory had to be extended
    *  to hold it, the link that made the new cluster reachable was already
    *  ordered behind the zeros in fat_dir_free_slot(); the entry landing
    *  without that link leaves nothing anybody can reach, and the link
    *  landing without the entry leaves an empty slot. Neither is a state that
    *  needs repairing.
    *
    *  What is therefore NOT promised is durability: fat_create() returning 0
    *  means the entry was issued, not that the flash has it. That is the same
    *  promise every write call here makes, and making it stronger would cost
    *  a round trip on every operation to buy something no ordering rule asks
    *  for. */
    return fat_put_data_sector();
}

int fat_create(const char *path)
{
    int r;

    if (fat_lock_acquire() < 0)
        return -1;

    r = fat_create_unlocked(path);

    /* Still inside the lock, and in the SHELL rather than in the body.
    *  There are four of these and between them thirty-odd ways out of a
    *  body, several of which change the free count on their way to
    *  reporting a failure; putting the write-back at the one point every
    *  one of them passes through is the same argument that made the shells
    *  the only thing that ever unlocks. It does nothing at all unless
    *  something moved, and it cannot change what this call returns - see
    *  fat_fsinfo_store(). */
    fat_fsinfo_store();

    fat_lock_release();
    return r;
}

/* Writes len bytes at offset.
*
*  Order of operations, which is the whole of the crash safety here:
*
*    1. If offset is past the end of the file, the gap is filled with zeros
*       FIRST. Doing it first rather than after the data means a write that
*       runs out of room part way through the gap ends with a file that is
*       longer and entirely zero, never one whose tail is the caller's data
*       with unwritten rubbish in front of it.
*    2. The data goes into the chain, which grows as needed.
*    3. The directory entry is written LAST, with the size the bytes that
*       actually landed imply.
*
*  So a power cut before step 3 leaves the file exactly as it was, plus some
*  unreferenced clusters that fsck reclaims. A power cut during step 3
*  leaves either the old entry or the new one - it is a single sector.
*
*  A short return is not an error to hide: it is the contract, and the
*  filesystem is consistent at every one of its values because the size
*  committed in step 3 is computed from what step 1 and 2 reported. */
static int fat_write_unlocked(const char *path, uint32_t offset, uint32_t len,
                              const void *buf)
{
    fat_file f;
    uint32_t gap, got, wrote;
    uint32_t was;

    if (fat_write_ready() < 0)
        return -1;
    if (buf == 0)
    {
        fat_err = "invalid argument";
        return -1;
    }

    /* The return type is int, so a single call can never report more than
    *  this many bytes even if the caller asks for more. */
    if (len > 0x7FFFFFFFUL)
        len = 0x7FFFFFFFUL;

    /* A FAT size is a 32 bit byte count. Refusing the overflow here means
    *  nothing below has to wonder whether pos + done wrapped. */
    if (offset > 0xFFFFFFFFUL - len)
    {
        fat_err = "write runs past the largest size FAT can record";
        return -1;
    }

    if (fat_open(path, &f) < 0)
        return -1;

    if (len == 0)
        return 0;

    was = f.size;

    if (offset > f.size)
    {
        gap = offset - f.size;
        got = fat_put(&f, f.size, gap, 0);
        if (got < gap)
        {
            /* Out of room inside the gap. The file grew by the zeros that
            *  landed and by nothing else, which is a state a caller can
            *  make sense of; none of its data was written, so zero. */
            f.size += got;
            if (f.size != was && fat_commit(&f) < 0)
                return -1;
            return 0;
        }
        f.size = offset;
    }

    wrote = fat_put(&f, offset, len, (const uint8_t *)buf);

    if (offset + wrote > f.size)
        f.size = offset + wrote;

    /* Nothing landed and the file did not grow, so there is nothing to
    *  commit. Writing the entry anyway would age a file that did not
    *  change, and would attach any cluster the failed attempt had already
    *  taken to a file that holds none of its bytes; leaving the entry alone
    *  turns that cluster into a plain unreferenced one instead. */
    if (wrote == 0 && f.size == was)
        return 0;

    if (fat_commit(&f) < 0)
        return -1;

    return (int)wrote;
}

int fat_write(const char *path, uint32_t offset, uint32_t len,
              const void *buf)
{
    int r;

    if (fat_lock_acquire() < 0)
        return -1;

    r = fat_write_unlocked(path, offset, len, buf);

    fat_fsinfo_store();

    fat_lock_release();
    return r;
}

/* Sets the file's length.
*
*  The two directions commit in opposite orders, for the same reason:
*
*    Growing  - the zeros go in first, the entry last. A crash leaves the
*               old, shorter file.
*    Shrinking - the entry goes FIRST, the clusters are released after. A
*               crash leaves a short file with a chain longer than it needs,
*               i.e. unreferenced clusters. The other order would leave the
*               entry claiming a size the chain no longer backs, and reading
*               that file would run off the end of its chain and into
*               whatever the freed clusters were given to next.
*
*  In both directions the state a crash can leave is the file at its old
*  extent or at its new smaller one, never something in between. */
static int fat_truncate_unlocked(const char *path, uint32_t size)
{
    fat_file f;
    uint32_t cluster_bytes;
    uint32_t keep;
    uint32_t old_first;
    uint32_t last, tail;
    uint32_t got;
    int r;

    if (fat_write_ready() < 0)
        return -1;
    if (fat_open(path, &f) < 0)
        return -1;

    if (size == f.size)
        return 0;

    if (size > f.size)
    {
        got = fat_put(&f, f.size, size - f.size, 0);
        if (got == 0)
            return -1;              /* nothing landed; fat_err says why */
        f.size += got;
        if (fat_commit(&f) < 0)
            return -1;
        if (f.size != size)
            return -1;              /* short: fat_err says why */
        return 0;
    }

    cluster_bytes = fat_spc * (uint32_t)BLK_SECTOR_SIZE;

    /* Clusters to keep, rounded up. Written as a division plus a remainder
    *  test rather than (size + cluster_bytes - 1) / cluster_bytes, which
    *  overflows for a size near 2^32. */
    keep = size / cluster_bytes;
    if (size % cluster_bytes != 0)
        keep++;

    old_first = f.first;
    if (keep == 0)
        f.first = 0;
    f.size = size;

    if (fat_commit(&f) < 0)
        return -1;

    if (old_first == 0)
        return 0;                   /* nothing was allocated to begin with */

    /* Rule 4 in the shrinking direction: the entry goes FIRST, and "first"
    *  has to mean on the medium. Releasing a cluster the entry may still
    *  claim is how the next allocation hands it to a second file while this
    *  one still reads through it - a cross link, from the one order this
    *  function exists to avoid. Placed after the early return above, so a
    *  truncate that releases nothing pays nothing.
    *
    *  Failing means releasing nothing at all: the file is short, its chain is
    *  as long as it ever was, and every cluster past the new end is
    *  unreferenced. That is a leak and it is the intended one. */
    if (fat_sync("the device would not confirm the shortened directory entry"
                 " before the clusters are released") < 0)
        return -1;

    if (keep == 0)
        return fat_free_chain(old_first);

    /* Cut the chain after the last cluster that is still in use. The new
    *  end marker goes in BEFORE the tail is released: the other way round
    *  would leave the kept part linking into clusters that already read as
    *  free, and the next allocation would hand one of them to another
    *  file. */
    f.first = old_first;
    f.cur_index = 0;
    f.cur_cluster = old_first;

    r = fat_file_cluster(&f, keep - 1, 0, &last);
    if (r < 0)
        return -1;
    if (r > 0)
        return 0;                   /* chain was already short - nothing to cut */

    r = fat_next_cluster(last, &tail);
    if (r < 0)
        return -1;
    if (r > 0)
        return 0;                   /* already ends there */

    if (fat_set_entry(last, fat_eoc()) < 0)
        return -1;

    /* The other half of the same sentence, and the one the comment above
    *  already insists on: the new end marker BEFORE the tail is released. If
    *  the frees land and the marker does not, the kept part of the chain runs
    *  on into clusters that read as free, and the next allocation splices one
    *  of them into another file. Failing here leaves the whole tail
    *  allocated, which is a leak and nothing worse. */
    if (fat_sync("the device would not confirm the new end-of-chain marker"
                 " before the tail is released") < 0)
        return -1;

    return fat_free_chain(tail);
}

int fat_truncate(const char *path, uint32_t size)
{
    int r;

    if (fat_lock_acquire() < 0)
        return -1;

    r = fat_truncate_unlocked(path, size);

    fat_fsinfo_store();

    fat_lock_release();
    return r;
}

/* Removes a file.
*
*  The directory entry is marked deleted FIRST, then the chain is released.
*  A crash in between leaves clusters allocated to a file that no longer
*  exists - fsck calls them unreferenced, offers to reclaim them, and no
*  data is lost either way.
*
*  The other order is the tempting one, because it never leaks. It is also
*  the one that can destroy a second file: with the chain freed and the
*  entry still there, the next fat_create() takes those clusters, and now
*  two directory entries describe the same sectors. Writing either file
*  corrupts the other, and no repair tool can separate them again - it can
*  only pick one and lose the rest. A leak is recoverable, a cross link is
*  not, so the leak is what this chooses.
*
*  Only the first byte of the entry changes. The rest, including the first
*  cluster, stays exactly as it was: that is what FAT has always done, and
*  it is what makes an undelete possible at all. */
static int fat_delete_unlocked(const char *path)
{
    fat_dir dir;
    fat_slot slot;
    char last[FAT_NAME_MAX];
    uint8_t e[DIR_ENTRY_SIZE];
    uint32_t first;
    int r;

    if (fat_write_ready() < 0)
        return -1;

    if (fat_resolve_parent(path, &dir, last) < 0)
        return -1;

    r = fat_dir_find(&dir, last, e, &slot);
    if (r != 0)
    {
        if (r > 0)
            fat_err = "no such file or directory";
        return -1;
    }

    /* Refused rather than handled: emptying a directory first is the
    *  caller's business, and there is no fat_rmdir() to pair with this. */
    if (e[DIR_Attr] & ATTR_DIRECTORY)
    {
        fat_err = "is a directory";
        return -1;
    }
    if (e[DIR_Attr] & ATTR_READ_ONLY)
    {
        fat_err = "file is read only";
        return -1;
    }

    first = fat_first_cluster(e);

    if (fat_get_data_sector(slot.lba) < 0)
        return -1;
    dat_buf[slot.within * DIR_ENTRY_SIZE] = DIRENT_DELETED;
    if (fat_put_data_sector() < 0)
        return -1;

    if (first == 0)
        return 0;                   /* an empty file owns no clusters */

    /* The deletion mark has to be on the medium before a cluster is given
    *  back. The comment above spells out why the other order is the one that
    *  destroys a second file, and on a device that buffers, "first" is only
    *  true if somebody asks.
    *
    *  Failing means the chain is not released: the entry says the file is
    *  gone, its clusters stay allocated, and fsck calls them unreferenced and
    *  offers to reclaim them. The caller is told the delete failed, which is
    *  the honest answer - this cannot tell whether the mark landed, and a
    *  caller that retries gets "no such file or directory" and loses
    *  nothing. */
    if (fat_sync("the device would not confirm the deleted directory entry"
                 " before the clusters are released") < 0)
        return -1;

    return fat_free_chain(first);
}

int fat_delete(const char *path)
{
    int r;

    if (fat_lock_acquire() < 0)
        return -1;

    r = fat_delete_unlocked(path);

    fat_fsinfo_store();

    fat_lock_release();
    return r;
}
