/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: Block device layer - one way to reach a 512 byte sector
*
*  The join between a filesystem and whatever can hand it a sector. blockdev.h
*  explains why the layer exists at all and why device numbers are stable; this
*  file is the bookkeeping behind it, and it is deliberately thin. It owns a
*  table of device numbers, a pair of byte counters and a sentence describing
*  the last failure. Everything else it forwards.
*
*  WHAT THIS LAYER IS FOR, beyond dispatch: it is the place where a request is
*  checked. A driver reaches hardware - it programs a task file, it builds a
*  SCSI command block - and every argument it has to distrust is an argument it
*  has to re-check in code that is already hard enough. So the arguments that
*  can be judged without touching hardware are judged once, here, and a driver
*  below this line may assume a non null buffer, a non zero count and a range
*  that lies inside the medium it just reported the size of.
*
*  THE OPS STRUCT IS COPIED, not referenced. A driver hands over five words;
*  storing the pointer would be cheaper and would also mean that the lifetime
*  of that struct becomes this layer's problem - a USB driver that keeps its
*  ops in a per device block and frees the block when the stick is pulled would
*  leave a dangling pointer behind in the table, and it would be found weeks
*  later as a jump into freed memory rather than immediately. Five words per
*  device number is 160 bytes of BSS for the whole table, which is nothing next
*  to that. Only the function pointers inside the copy still point out of this
*  file, and those refer to code, which is resident for as long as the kernel
*  is: this kernel has no module unloading.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <ata.h>
#include <blockdev.h>

/* --- The device table ------------------------------------------------------
*
*  Indexed by device number, so a number is a slot and nothing is ever moved.
*  See blockdev.h: that stability is the point, not an implementation detail
*  worth optimising away.
*
*  gen is what makes a device disappearing safe to observe. It is taken from a
*  counter that only ever grows, so a slot that is unregistered and registered
*  again - a stick pulled and pushed back into the same port - never looks like
*  the one that was there before, which a plain "still used" flag could not
*  tell apart. A transfer takes a copy of it before it calls down and compares
*  afterwards; see blk_transfer(). */
typedef struct
{
    blk_ops  ops;       /* a copy of what the driver registered              */
    int      used;
    uint32_t gen;
} blk_slot;

static blk_slot blk_slots[BLK_MAX_DEVICES];

/* Never reset, and never reused. Wrapping it would take 2^32 hotplug events. */
static uint32_t blk_generation;

/* What actually moved, in bytes, since boot. These are counted after a
*  transfer returned success and never before: a failed read has moved some
*  unknowable prefix of what was asked for, and counting the request rather
*  than the result would turn a broken disk into an impressive throughput
*  figure. They wrap at 4 GiB, which is what a 32 bit counter of bytes does and
*  is fine for a number a shell prints. */
static uint32_t blk_read_total;
static uint32_t blk_write_total;

/* The ATA driver, wrapped. Its four entry points already have exactly the
*  shapes blk_ops asks for - ata_read() and ata_write() take a drive number
*  first, ata_sectors() reports 0 for a drive that is not there, and
*  ata_model() returns "" rather than null - so no adapter functions are
*  needed. The wrapping lives here rather than in ata.c on purpose: ata.c is a
*  driver for a disk controller and has no reason to know that anything sits
*  above it. */
static const blk_ops blk_ata_ops =
{
    "ATA",
    ata_read,
    ata_write,
    ata_sectors,
    ata_model
};

static const char blk_empty[] = "";
static const char blk_none[]  = "none";

/* --- Error reporting -------------------------------------------------------
*
*  Composed by hand into a fixed buffer, the same way ata.c does it and for the
*  same reason: there is no snprintf() here, and "sector 100+8 is past the end
*  of device 4 (64 sectors)" tells whoever reads it what to fix, where "I/O
*  error" only tells them that something is wrong. */

static char blk_error_buf[96];
static int  blk_error_len;

static void blk_err_reset(void)
{
    blk_error_buf[0] = '\0';
    blk_error_len = 0;
}

static void blk_err_str(const char *s)
{
    while(*s != '\0' && blk_error_len < (int)sizeof(blk_error_buf) - 1)
    {
        blk_error_buf[blk_error_len++] = *s++;
    }
    blk_error_buf[blk_error_len] = '\0';
}

static void blk_err_uint(uint32_t v)
{
    char tmp[12];
    int  n;

    n = 0;
    do
    {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while(v != 0);

    while(n > 0)
    {
        n--;
        if(blk_error_len < (int)sizeof(blk_error_buf) - 1)
        {
            blk_error_buf[blk_error_len++] = tmp[n];
        }
    }
    blk_error_buf[blk_error_len] = '\0';
}

/* "blk: dev 4: " - every message starts this way. The device number is what
*  the user typed at the shell, so it is what the message should name. */
static void blk_err_begin(int dev)
{
    blk_err_reset();
    blk_err_str("blk: dev ");
    if(dev >= 0 && dev < BLK_MAX_DEVICES)
    {
        blk_err_uint((uint32_t)dev);
    }
    else
    {
        blk_err_str("?");
    }
    blk_err_str(": ");
}

/* --- Registration ---------------------------------------------------------- */

static int blk_valid_dev(int dev)
{
    return (dev >= 0 && dev < BLK_MAX_DEVICES);
}

int blk_register(int dev, const blk_ops *ops)
{
    if(!blk_valid_dev(dev))
    {
        blk_err_begin(dev);
        blk_err_str("device number out of range (0..");
        blk_err_uint((uint32_t)(BLK_MAX_DEVICES - 1));
        blk_err_str(")");
        return -1;
    }

    /* A half filled ops struct would pass registration and then crash on the
    *  first call, in a driver whose author has long stopped looking. Every
    *  entry point below is called unconditionally by this file, so every one
    *  of them is required here. */
    if(ops == 0 || ops->name == 0 || ops->read == 0 || ops->write == 0 ||
       ops->sectors == 0 || ops->describe == 0)
    {
        blk_err_begin(dev);
        blk_err_str("incomplete driver operations");
        return -1;
    }

    /* Two drivers claiming one number is refused rather than resolved. There
    *  is no answer that is right: dropping the newcomer loses a device that is
    *  physically there, and replacing the incumbent pulls the floor out from
    *  under whatever is mounted on it, from a layer that is not allowed to
    *  know that anything is mounted at all. It is a bug in the caller - the
    *  numbering rules in blockdev.h exist precisely so that two drivers do not
    *  reach for the same number - and a bug is worth reporting, not papering
    *  over. The incumbent stays, because it is the one that may be in use. */
    if(blk_slots[dev].used)
    {
        blk_err_begin(dev);
        blk_err_str("already registered by ");
        blk_err_str(blk_slots[dev].ops.name);
        return -1;
    }

    blk_slots[dev].ops  = *ops;     /* copied, see the file header             */
    blk_slots[dev].used = 1;
    blk_slots[dev].gen  = ++blk_generation;
    return 0;
}

/* Giving a number back. A read that is already running on this device is NOT
*  interrupted - there is nothing here that could interrupt it, and the driver
*  is somewhere inside a bulk transfer to hardware that has just been pulled
*  out of its socket. What happens instead is that it fails on its own, because
*  the transfer it is waiting for will time out, and blk_transfer() then sees
*  that the generation it copied before the call no longer matches and reports
*  a removal rather than a driver error. Even a transfer that somehow succeeded
*  in the gap is failed on the way out: the sector may be from a device that is
*  no longer the device this number means, and a filesystem must not cache it.
*
*  The slot is wiped rather than only flagged, so that a stale entry cannot be
*  followed by mistake. What is mounted on the device is the caller's problem,
*  as blockdev.h says - this layer cannot unmount anything without knowing what
*  a filesystem is, and knowing that is exactly what it is here to avoid. */
void blk_unregister(int dev)
{
    if(!blk_valid_dev(dev) || !blk_slots[dev].used)
    {
        return;
    }

    blk_slots[dev].used         = 0;
    blk_slots[dev].gen          = ++blk_generation;
    blk_slots[dev].ops.name     = 0;
    blk_slots[dev].ops.read     = 0;
    blk_slots[dev].ops.write    = 0;
    blk_slots[dev].ops.sectors  = 0;
    blk_slots[dev].ops.describe = 0;
}

/* Called once at boot, after ata_init(). All four ATA numbers are claimed,
*  including the ones no drive answered on: the claim is what keeps 0..3
*  meaning "the drive on that bus position" on a machine where somebody plugs a
*  stick in before the second disk arrives. An empty slot costs nothing - it
*  reports 0 sectors, so blk_present() says no and blk_count() does not count
*  it - and it cannot be taken by a removable device that would then answer to
*  a hard disk's number on the next boot. */
void blk_init(void)
{
    int dev;

    for(dev = 0; dev < BLK_ATA_COUNT; dev++)
    {
        blk_register(BLK_ATA_FIRST + dev, &blk_ata_ops);
    }
}

/* --- Transfers ------------------------------------------------------------- */

/* The body of both blk_read() and blk_write(). Everything that can be decided
*  without hardware is decided before the driver is entered, and the slot is
*  looked at once more afterwards.
*
*  buf is void * rather than const void * for the write path; the const is put
*  back at the call into the driver. Splitting the checks into two near
*  identical copies to preserve it would be the more expensive mistake. */
static int blk_transfer(int dev, uint32_t lba, uint32_t count, void *buf,
                        int writing)
{
    blk_slot *s;
    uint32_t  total;
    uint32_t  gen;
    int       r;
    int     (*fn_read)(int, uint32_t, uint32_t, void *);
    int     (*fn_write)(int, uint32_t, uint32_t, const void *);

    if(!blk_valid_dev(dev) || !blk_slots[dev].used)
    {
        blk_err_begin(dev);
        blk_err_str("no such device");
        return -1;
    }

    s = &blk_slots[dev];

    /* A null buffer is refused here rather than being dereferenced two layers
    *  down. There is no address 0 mapping in this kernel, so the alternative
    *  is a page fault inside a driver, reported against whichever sector
    *  happened to be in flight. */
    if(buf == 0)
    {
        blk_err_begin(dev);
        blk_err_str("no buffer");
        return -1;
    }

    /* A zero sector transfer is refused rather than quietly succeeding. It
    *  cannot be served - there is no such thing as reading no sectors - and
    *  the only way a caller asks for one is by computing a length wrong. Told
    *  "success", that caller goes on to use a buffer nothing has written,
    *  which is the same bug found much later and much further away. Note that
    *  ata.c treats a zero count as a no-op instead; down there it genuinely is
    *  one, because a driver is entitled to assume its caller checked, and this
    *  is the caller that checks. */
    if(count == 0)
    {
        blk_err_begin(dev);
        blk_err_str("zero sector transfer");
        return -1;
    }

    /* The size doubles as the presence test, as blockdev.h says. Asking now
    *  also means a removable device that has already gone answers 0 here and
    *  is refused before a driver is entered at all. */
    total = s->ops.sectors(dev);
    if(total == 0)
    {
        blk_err_begin(dev);
        blk_err_str("device is not there");
        return -1;
    }

    /* The range check, written as two subtractions so that no sum is ever
    *  formed. The obvious form, lba + count > total, is wrong on exactly the
    *  input an attacker would pick: with lba = 0xFFFFFF00 and count = 0x200
    *  the sum wraps to 0x100, which is comfortably inside a small disk, and
    *  the request is passed down to a driver that will happily seek to
    *  0xFFFFFF00 and write 512 sectors of somebody else's data. count > total
    *  is tested first so that total - count cannot go below zero, and the pair
    *  then rejects every lba + count that would exceed 2^32 as well, because
    *  such an lba is necessarily greater than total - count for any total that
    *  fits in 32 bits. */
    if(count > total || lba > total - count)
    {
        blk_err_begin(dev);
        blk_err_str("sector ");
        blk_err_uint(lba);
        blk_err_str("+");
        blk_err_uint(count);
        blk_err_str(" is past the end (");
        blk_err_uint(total);
        blk_err_str(" sectors)");
        return -1;
    }

    /* Everything the driver is about to be called through is copied out of the
    *  table first. The call below can block for as long as a USB transfer
    *  takes to time out, and another task may unregister the device in that
    *  window; from this point on the table is only read again to find out
    *  whether that happened. */
    gen      = s->gen;
    fn_read  = s->ops.read;
    fn_write = s->ops.write;

    if(writing)
    {
        r = fn_write(dev, lba, count, (const void *)buf);
    }
    else
    {
        r = fn_read(dev, lba, count, buf);
    }

    /* Did the device survive the call? A mismatch means it was unregistered
    *  while the transfer was running, or unregistered and something else
    *  registered in its place. Either way what is in the buffer belongs to a
    *  device that no longer answers to this number, and success would be a
    *  lie whatever the driver returned. */
    if(!s->used || s->gen != gen)
    {
        blk_err_begin(dev);
        blk_err_str("device was removed during the transfer");
        return -1;
    }

    if(r < 0)
    {
        blk_err_begin(dev);
        blk_err_str(writing ? "write" : "read");
        blk_err_str(" failed at sector ");
        blk_err_uint(lba);
        blk_err_str(" (");
        blk_err_str(s->ops.name);
        blk_err_str(")");
        return -1;
    }

    /* Only now, and only what moved. The multiplication wraps at 4 GiB along
    *  with the counter it feeds, which is the arithmetic a wrapping counter
    *  wants anyway. */
    if(writing)
    {
        blk_write_total += count * (uint32_t)BLK_SECTOR_SIZE;
    }
    else
    {
        blk_read_total += count * (uint32_t)BLK_SECTOR_SIZE;
    }

    return 0;
}

int blk_read(int dev, uint32_t lba, uint32_t count, void *buf)
{
    return blk_transfer(dev, lba, count, buf, 0);
}

int blk_write(int dev, uint32_t lba, uint32_t count, const void *buf)
{
    /* The const is cast away to reach one shared body and put back before the
    *  driver is called. Nothing between here and there writes through it. */
    return blk_transfer(dev, lba, count, (void *)buf, 1);
}

/* --- What is there --------------------------------------------------------- */

int blk_present(int dev)
{
    if(!blk_valid_dev(dev) || !blk_slots[dev].used)
    {
        return 0;
    }
    return (blk_slots[dev].ops.sectors(dev) != 0);
}

uint32_t blk_sectors(int dev)
{
    if(!blk_valid_dev(dev) || !blk_slots[dev].used)
    {
        return 0;
    }
    return blk_slots[dev].ops.sectors(dev);
}

/* Never null, as blockdev.h promises, so a caller may print it without a test.
*  A driver that returns null anyway gets an empty string rather than a crash
*  in whatever was about to walk the string. */
const char *blk_describe(int dev)
{
    const char *s;

    if(!blk_valid_dev(dev) || !blk_slots[dev].used)
    {
        return blk_empty;
    }

    s = blk_slots[dev].ops.describe(dev);
    if(s == 0)
    {
        return blk_empty;
    }
    return s;
}

/* The bus name is the driver's name, which is the honest answer to "why did
*  this appear between two boots": a device on "USB storage" was plugged in,
*  one on "ATA" was not. A number nothing has registered is on no bus. */
const char *blk_bus(int dev)
{
    if(!blk_valid_dev(dev) || !blk_slots[dev].used)
    {
        return blk_none;
    }
    return blk_slots[dev].ops.name;
}

/* Numbers that are registered AND answer with a size. A registered but empty
*  ATA slot is not a device anybody can use, and counting it would have a
*  machine with one disk report four. */
int blk_count(void)
{
    int dev;
    int n;

    n = 0;
    for(dev = 0; dev < BLK_MAX_DEVICES; dev++)
    {
        if(blk_slots[dev].used && blk_slots[dev].ops.sectors(dev) != 0)
        {
            n++;
        }
    }
    return n;
}

uint32_t blk_bytes_read(void)
{
    return blk_read_total;
}

uint32_t blk_bytes_written(void)
{
    return blk_write_total;
}

const char *blk_last_error(void)
{
    return blk_error_buf;
}
