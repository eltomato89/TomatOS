/* TomatOS - v0.1 pre Alpha
*  By:   Jens Köhler (eltomato@googlemail.com)
*  Desc: ATA/IDE block device driver (programmed I/O, LBA28)
*
*  Two channels, two drives each. Everything happens through port I/O: the
*  CPU itself moves all 256 words of a sector through the data port. That is
*  slow, but it needs neither a bus master nor PCI enumeration, and the point
*  of this layer is to make a filesystem possible at all.
*
*  This driver polls. No IRQ handler is installed, and the nIEN bit in the
*  device control register is set on every channel so that the drives never
*  raise IRQ 14/15 into a handler that does not exist.
*
*  Two details decide whether this works on real hardware rather than only
*  in an emulator:
*
*    - Selecting a drive costs time. After writing the drive/head register
*      the status register needs roughly 400 ns before it describes the
*      newly selected drive instead of the previous one. The conventional
*      way to spend that time is to read the ALTERNATE status port four
*      times: it has the same contents as the regular status port but,
*      unlike it, does not acknowledge a pending interrupt. All polling in
*      this file therefore uses the alternate port.
*
*    - Every wait is bounded. A missing, wedged or half dead drive must not
*      park the kernel in a spin loop that nothing can interrupt - the
*      driver runs with interrupts of its own disabled at the drive, and a
*      hang here is a hang of the whole machine. Each loop counts down and
*      reports a timeout instead.
*
*  Notes: No warranty expressed or implied. Use at own risk.
*/

#include <system.h>
#include <stdio.h>
#include <ata.h>

/* --- Register offsets ------------------------------------------------------
*
*  Command block, relative to the channel base (0x1F0 / 0x170). Several
*  offsets mean one thing on read and another on write, hence the pairs. */
#define ATA_REG_DATA        0   /* 16 bit wide, r/w                          */
#define ATA_REG_ERROR       1   /* read                                      */
#define ATA_REG_FEATURES    1   /* write                                     */
#define ATA_REG_SECCOUNT    2
#define ATA_REG_LBA0        3   /* LBA bits 0-7                              */
#define ATA_REG_LBA1        4   /* LBA bits 8-15                             */
#define ATA_REG_LBA2        5   /* LBA bits 16-23                            */
#define ATA_REG_DRIVE       6   /* drive select + LBA bits 24-27             */
#define ATA_REG_STATUS      7   /* read - acknowledges the interrupt         */
#define ATA_REG_COMMAND     7   /* write                                     */

/* Control block, relative to the channel control port (0x3F6 / 0x376). */
#define ATA_REG_ALTSTATUS   0   /* read - same bits, no side effect          */
#define ATA_REG_CONTROL     0   /* write                                     */

/* --- Status bits ---------------------------------------------------------- */
#define ATA_SR_ERR   0x01   /* an error occurred, details in the error reg    */
#define ATA_SR_DRQ   0x08   /* the data port wants a word moved              */
#define ATA_SR_DF    0x20   /* device fault                                  */
#define ATA_SR_RDY   0x40   /* ready to accept a command                     */
#define ATA_SR_BSY   0x80   /* busy; every other bit is meaningless          */

/* --- Device control bits -------------------------------------------------- */
#define ATA_CTL_NIEN 0x02   /* 1 = the drive does not assert its IRQ line     */

/* --- Commands ------------------------------------------------------------- */
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY    0xEC

/* --- Layout of the IDENTIFY block ----------------------------------------- */
#define ATA_ID_WORDS        256
#define ATA_ID_MODEL        27  /* words 27-46: 40 characters, byte swapped   */
#define ATA_ID_MODEL_WORDS  20
#define ATA_ID_LBA28_LOW    60  /* words 60-61: LBA28 sector count            */
#define ATA_ID_LBA28_HIGH   61

#define ATA_MODEL_LEN       (ATA_ID_MODEL_WORDS * 2)   /* 40 characters       */

/* Highest sector an LBA28 command can name: the address is 28 bits wide. */
#define ATA_LBA28_MAX       0x0FFFFFFFUL

/* --- Timeouts --------------------------------------------------------------
*
*  Counted in loop iterations, and one iteration is one port read. A port
*  read to the IDE control block costs on the order of a microsecond, so the
*  numbers below are roughly a tenth of a second while probing and half a
*  second while transferring. That is far more than a healthy drive needs to
*  answer a single sector command, and short enough that four dead sockets do
*  not noticeably delay the boot. */
#define ATA_TIMEOUT_PROBE    100000UL
#define ATA_TIMEOUT_XFER     500000UL

/* --- State ---------------------------------------------------------------- */

typedef struct
{
    unsigned short base;        /* command block base port                    */
    unsigned short ctrl;        /* control block port                         */
    int            slave;       /* 0 = master, 1 = slave                      */
    int            present;
    uint32_t       sectors;     /* LBA28 capacity, 0 when absent              */
    char           model[ATA_MODEL_LEN + 1];
} ata_drive;

static ata_drive ata_drives[ATA_MAX_DRIVES];

/* Names for the boot message, in the order the drives are numbered. */
static const char *ata_names[ATA_MAX_DRIVES] = { "hda", "hdb", "hdc", "hdd" };

/* The IDENTIFY block lives in the BSS rather than on the stack: half a
*  kilobyte is a lot to ask of a kernel stack, and it is needed only inside
*  ata_identify(). */
static unsigned short ata_id_buf[ATA_ID_WORDS];

static char ata_error_buf[96];
static int  ata_error_len;
static const char ata_empty[] = "";

/* --- Error reporting -------------------------------------------------------
*
*  Composed by hand into a fixed buffer. There is no snprintf() here, and a
*  message that names the drive and shows the status byte is worth far more
*  to whoever reads it than a bare "I/O error". */

static void err_reset(void)
{
    ata_error_buf[0] = '\0';
    ata_error_len = 0;
}

static void err_str(const char *s)
{
    while(*s != '\0' && ata_error_len < (int)sizeof(ata_error_buf) - 1)
    {
        ata_error_buf[ata_error_len++] = *s++;
    }
    ata_error_buf[ata_error_len] = '\0';
}

static void err_uint(uint32_t v)
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
        if(ata_error_len < (int)sizeof(ata_error_buf) - 1)
        {
            ata_error_buf[ata_error_len++] = tmp[n];
        }
    }
    ata_error_buf[ata_error_len] = '\0';
}

static void err_hex8(unsigned char v)
{
    static const char digits[] = "0123456789ABCDEF";
    char tmp[5];

    tmp[0] = '0';
    tmp[1] = 'x';
    tmp[2] = digits[(v >> 4) & 0x0F];
    tmp[3] = digits[v & 0x0F];
    tmp[4] = '\0';
    err_str(tmp);
}

/* "ata: hdb: " - every message starts this way. */
static void err_begin(int drive)
{
    err_reset();
    err_str("ata: ");
    if(drive >= 0 && drive < ATA_MAX_DRIVES)
    {
        err_str(ata_names[drive]);
    }
    else
    {
        err_str("?");
    }
    err_str(": ");
}

/* --- Word wide port I/O ----------------------------------------------------
*
*  The data port is 16 bits wide and a sector is 256 words. system.h declares
*  an inportw(), but nothing in the kernel defines it, so the two helpers
*  below are local - a header this file does not own is no place to fix that.
*  They use the string instructions, which move a whole sector in one go. */

static void ata_insw(unsigned short port, void *dest, unsigned int words)
{
    __asm__ __volatile__ ("cld; rep insw"
                          : "+D" (dest), "+c" (words)
                          : "d" (port)
                          : "memory");
}

static void ata_outsw(unsigned short port, const void *src, unsigned int words)
{
    __asm__ __volatile__ ("cld; rep outsw"
                          : "+S" (src), "+c" (words)
                          : "d" (port)
                          : "memory");
}

/* --- Polling ---------------------------------------------------------------

*  Everything below reads the ALTERNATE status port. Reading the regular one
*  would clear a pending interrupt, and while nIEN keeps the drive from
*  asserting the line at all, polling the port without side effects is the
*  habit worth keeping. */

static unsigned char ata_altstatus(ata_drive *d)
{
    return inportb(d->ctrl + ATA_REG_ALTSTATUS);
}

/* The mandatory pause after a write to the drive/head register. Four reads
*  of the alternate status port take at least 400 ns on any bus that has ever
*  carried an IDE controller, and cost nothing else. */
static void ata_delay400(ata_drive *d)
{
    inportb(d->ctrl + ATA_REG_ALTSTATUS);
    inportb(d->ctrl + ATA_REG_ALTSTATUS);
    inportb(d->ctrl + ATA_REG_ALTSTATUS);
    inportb(d->ctrl + ATA_REG_ALTSTATUS);
}

/* Selects a drive and waits out the 400 ns. lba is only consulted for its
*  top four bits; pass 0 for commands that take no address. */
static void ata_select(ata_drive *d, uint32_t lba)
{
    unsigned char sel;

    /* nIEN is set on every selection rather than once at startup: it costs a
    *  single port write, and it survives whatever a BIOS or a previous
    *  operating system left in the control register. */
    outportb(d->ctrl + ATA_REG_CONTROL, ATA_CTL_NIEN);

    sel = (unsigned char)(0xE0 | (d->slave << 4) | (int)((lba >> 24) & 0x0F));
    outportb(d->base + ATA_REG_DRIVE, sel);
    ata_delay400(d);
}

/* An empty socket usually reads back 0xFF on every port: nothing drives the
*  bus, and the pull-ups win. That is not a status value - BSY and ERR cannot
*  both be set - so it is worth recognising before waiting on anything. */
static int ata_floating(unsigned char status)
{
    return (status == 0xFF);
}

/* Waits for BSY to clear. Returns the last status read, and stores 0xFF in
*  *timedout when the patience ran out. */
static unsigned char ata_wait_busy(ata_drive *d, uint32_t limit, int *timedout)
{
    unsigned char status;

    *timedout = 0;
    status = 0;

    while(limit != 0)
    {
        status = ata_altstatus(d);
        if(ata_floating(status))
        {
            *timedout = 1;
            return status;
        }
        if((status & ATA_SR_BSY) == 0)
        {
            return status;
        }
        limit--;
    }

    *timedout = 1;
    return status;
}

/* Waits until the drive is ready to move a sector: BSY clear and DRQ set.
*  Returns 0 on success, -1 with ata_error_buf filled in otherwise. ERR and
*  DF are checked explicitly - waiting for a DRQ that an errored drive will
*  never raise is exactly the kind of hang this driver must not have. */
static int ata_wait_drq(int drive, ata_drive *d, uint32_t limit)
{
    unsigned char status;
    int           timedout;

    status = ata_wait_busy(d, limit, &timedout);

    if(timedout)
    {
        err_begin(drive);
        err_str(ata_floating(status) ? "drive stopped responding" :
                                       "timeout waiting for BSY to clear");
        return -1;
    }

    if((status & ATA_SR_ERR) != 0)
    {
        err_begin(drive);
        err_str("command failed, status ");
        err_hex8(status);
        err_str(", error ");
        err_hex8(inportb(d->base + ATA_REG_ERROR));
        return -1;
    }

    if((status & ATA_SR_DF) != 0)
    {
        err_begin(drive);
        err_str("device fault, status ");
        err_hex8(status);
        return -1;
    }

    if((status & ATA_SR_DRQ) == 0)
    {
        err_begin(drive);
        err_str("no data requested, status ");
        err_hex8(status);
        return -1;
    }

    return 0;
}

/* Waits for a command that transfers no data to finish - a cache flush, or
*  the tail end of a write. Same bounded wait, same error checks, only DRQ is
*  not expected. */
static int ata_wait_done(int drive, ata_drive *d, uint32_t limit)
{
    unsigned char status;
    int           timedout;

    status = ata_wait_busy(d, limit, &timedout);

    if(timedout)
    {
        err_begin(drive);
        err_str("timeout waiting for the drive to finish");
        return -1;
    }

    if((status & (ATA_SR_ERR | ATA_SR_DF)) != 0)
    {
        err_begin(drive);
        err_str("command failed, status ");
        err_hex8(status);
        err_str(", error ");
        err_hex8(inportb(d->base + ATA_REG_ERROR));
        return -1;
    }

    return 0;
}

/* --- IDENTIFY --------------------------------------------------------------

*  The model string sits in words 27-46 as 40 characters, and every word
*  holds its two characters the wrong way round: the high byte comes first.
*  Copying the block straight out is the classic way to end up with
*  "QMEU AHDRDSIK" on the screen. The loop below swaps each pair back.
*
*  The string is padded with spaces, not terminated, so the tail has to be
*  trimmed by hand. Anything unprintable is replaced rather than sent to the
*  console, where a stray control byte would move the cursor. */
static void ata_extract_model(ata_drive *d, const unsigned short *id)
{
    int           i;
    int           n;
    unsigned char c;

    for(i = 0; i < ATA_ID_MODEL_WORDS; i++)
    {
        c = (unsigned char)(id[ATA_ID_MODEL + i] >> 8);
        d->model[i * 2] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';

        c = (unsigned char)(id[ATA_ID_MODEL + i] & 0x00FF);
        d->model[i * 2 + 1] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    d->model[ATA_MODEL_LEN] = '\0';

    /* Trim the trailing padding. */
    n = ATA_MODEL_LEN;
    while(n > 0 && d->model[n - 1] == ' ')
    {
        n--;
    }
    d->model[n] = '\0';

    /* And any leading padding, by shifting the rest down. */
    i = 0;
    while(d->model[i] == ' ')
    {
        i++;
    }
    if(i > 0)
    {
        n = 0;
        while(d->model[i + n] != '\0')
        {
            d->model[n] = d->model[i + n];
            n++;
        }
        d->model[n] = '\0';
    }
}

/* Probes one drive. Returns 1 when a usable LBA28 ATA disk answered.
*  Everything that goes wrong here is a non-event: an empty socket is the
*  normal case, not an error worth reporting. */
static int ata_identify(int drive)
{
    ata_drive     *d;
    unsigned char  status;
    unsigned char  back_lo;
    unsigned char  back_hi;
    int            timedout;
    uint32_t       sectors;

    d = &ata_drives[drive];

    ata_select(d, 0);

    /* Floating bus: nothing is attached to this channel at all. */
    status = ata_altstatus(d);
    if(ata_floating(status))
    {
        return 0;
    }

    /* A second, stricter test. Many controllers answer for the master when
    *  the slave is absent, so a plausible status byte proves nothing on its
    *  own. Two scratch registers are written with a pattern and read back;
    *  only a drive that is really there remembers it. */
    outportb(d->base + ATA_REG_SECCOUNT, 0x55);
    outportb(d->base + ATA_REG_LBA0, 0xAA);
    back_lo = inportb(d->base + ATA_REG_SECCOUNT);
    back_hi = inportb(d->base + ATA_REG_LBA0);
    if(back_lo != 0x55 || back_hi != 0xAA)
    {
        return 0;
    }

    /* Zero the address registers: after IDENTIFY, a device that is not a
    *  plain ATA disk leaves its signature in them. */
    outportb(d->base + ATA_REG_SECCOUNT, 0x00);
    outportb(d->base + ATA_REG_LBA0, 0x00);
    outportb(d->base + ATA_REG_LBA1, 0x00);
    outportb(d->base + ATA_REG_LBA2, 0x00);

    outportb(d->base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay400(d);

    /* Status 0 after the command means there is no drive. */
    status = ata_altstatus(d);
    if(status == 0x00 || ata_floating(status))
    {
        return 0;
    }

    status = ata_wait_busy(d, ATA_TIMEOUT_PROBE, &timedout);
    if(timedout)
    {
        return 0;
    }

    /* ATAPI and SATA devices reply to IDENTIFY with an error and put their
    *  signature into LBA1/LBA2. Neither speaks the LBA28 read and write
    *  commands this driver issues, so both are left alone. */
    if(inportb(d->base + ATA_REG_LBA1) != 0x00 ||
       inportb(d->base + ATA_REG_LBA2) != 0x00)
    {
        return 0;
    }

    if((status & (ATA_SR_ERR | ATA_SR_DF)) != 0)
    {
        return 0;
    }
    if((status & ATA_SR_DRQ) == 0)
    {
        return 0;
    }

    ata_insw(d->base + ATA_REG_DATA, ata_id_buf, ATA_ID_WORDS);

    sectors = (uint32_t)ata_id_buf[ATA_ID_LBA28_LOW] |
              ((uint32_t)ata_id_buf[ATA_ID_LBA28_HIGH] << 16);

    /* A disk that reports no LBA28 capacity is either ancient CHS-only
    *  hardware or larger than LBA28 can address. Neither is something this
    *  driver can serve safely. */
    if(sectors == 0)
    {
        return 0;
    }
    if(sectors > ATA_LBA28_MAX)
    {
        sectors = ATA_LBA28_MAX;
    }

    d->sectors = sectors;
    ata_extract_model(d, ata_id_buf);
    d->present = 1;

    return 1;
}

/* --- Public interface ------------------------------------------------------ */

void ata_init(void)
{
    static unsigned short bases[ATA_MAX_DRIVES] = { 0x1F0, 0x1F0, 0x170, 0x170 };
    static unsigned short ctrls[ATA_MAX_DRIVES] = { 0x3F6, 0x3F6, 0x376, 0x376 };
    int i;

    err_reset();

    for(i = 0; i < ATA_MAX_DRIVES; i++)
    {
        ata_drives[i].base     = bases[i];
        ata_drives[i].ctrl     = ctrls[i];
        ata_drives[i].slave    = i & 1;
        ata_drives[i].present  = 0;
        ata_drives[i].sectors  = 0;
        ata_drives[i].model[0] = '\0';

        /* Silence the channel before touching it. Doing this for the slave
        *  as well is redundant but harmless, and it means no code path can
        *  reach a command with interrupts still enabled. */
        outportb(ata_drives[i].ctrl + ATA_REG_CONTROL, ATA_CTL_NIEN);
    }

    for(i = 0; i < ATA_MAX_DRIVES; i++)
    {
        if(ata_identify(i))
        {
            /* One line per drive, and no line at all for an empty socket -
            *  the screen has 25 rows and the boot has more to say. Sectors
            *  are 512 bytes, so 2048 of them make a MiB. */
            printf("ATA: %s %s, %u MiB\n",
                   ata_names[i],
                   ata_drives[i].model,
                   (int)(ata_drives[i].sectors / 2048UL));
        }
    }
}

int ata_present(int drive)
{
    if(drive < 0 || drive >= ATA_MAX_DRIVES)
    {
        return 0;
    }
    return ata_drives[drive].present;
}

uint32_t ata_sectors(int drive)
{
    if(drive < 0 || drive >= ATA_MAX_DRIVES)
    {
        return 0;
    }
    return ata_drives[drive].sectors;
}

const char *ata_model(int drive)
{
    if(drive < 0 || drive >= ATA_MAX_DRIVES || !ata_drives[drive].present)
    {
        return ata_empty;
    }
    return ata_drives[drive].model;
}

const char *ata_last_error(void)
{
    return ata_error_buf;
}

/* Checks the arguments common to a read and a write, and reports why they
*  are wrong rather than letting the drive find out. */
static int ata_check_range(int drive, uint32_t lba, uint32_t count,
                           const void *buf)
{
    ata_drive *d;

    if(drive < 0 || drive >= ATA_MAX_DRIVES || !ata_drives[drive].present)
    {
        err_begin(drive);
        err_str("no such drive");
        return -1;
    }

    if(buf == 0)
    {
        err_begin(drive);
        err_str("no buffer");
        return -1;
    }

    if(count == 0)
    {
        return 0;
    }

    d = &ata_drives[drive];

    /* Written as a subtraction so that lba + count cannot wrap around and
    *  turn an out of range request into a valid looking one. */
    if(count > d->sectors || lba > d->sectors - count)
    {
        err_begin(drive);
        err_str("sector ");
        err_uint(lba);
        err_str("+");
        err_uint(count);
        err_str(" is past the end of the disk (");
        err_uint(d->sectors);
        err_str(" sectors)");
        return -1;
    }

    return 0;
}

/* Programs the task file for a single sector transfer at lba. */
static int ata_prepare(int drive, ata_drive *d, uint32_t lba)
{
    unsigned char status;
    int           timedout;

    ata_select(d, lba);

    status = ata_altstatus(d);
    if(ata_floating(status))
    {
        err_begin(drive);
        err_str("drive vanished from the bus");
        return -1;
    }

    /* The task file may only be written while the drive is idle. */
    ata_wait_busy(d, ATA_TIMEOUT_XFER, &timedout);
    if(timedout)
    {
        err_begin(drive);
        err_str("stuck busy before the command");
        return -1;
    }

    outportb(d->base + ATA_REG_FEATURES, 0x00);
    outportb(d->base + ATA_REG_SECCOUNT, 1);
    outportb(d->base + ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
    outportb(d->base + ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    outportb(d->base + ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));

    return 0;
}

/* The body of both ata_read() and ata_write(). One sector per command: the
*  multi sector form would be faster, but it needs the drive's block size to
*  know where the DRQ boundaries fall, and a filesystem does not care.
*
*  A failure anywhere aborts the whole call. Returning a partial transfer
*  would hand the caller a buffer that is half stale data and no way to tell
*  which half. */
static int ata_transfer(int drive, uint32_t lba, uint32_t count,
                        void *buf, int writing)
{
    ata_drive     *d;
    unsigned char *p;
    uint32_t       i;

    if(ata_check_range(drive, lba, count, buf) != 0)
    {
        return -1;
    }
    if(count == 0)
    {
        return 0;
    }

    d = &ata_drives[drive];
    p = (unsigned char *)buf;

    for(i = 0; i < count; i++)
    {
        if(ata_prepare(drive, d, lba + i) != 0)
        {
            return -1;
        }

        outportb(d->base + ATA_REG_COMMAND,
                 writing ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);
        ata_delay400(d);

        if(ata_wait_drq(drive, d, ATA_TIMEOUT_XFER) != 0)
        {
            return -1;
        }

        if(writing)
        {
            ata_outsw(d->base + ATA_REG_DATA, p, ATA_SECTOR_SIZE / 2);

            /* The drive raises BSY while it takes the sector out of the
            *  data buffer; the result of the write only shows up in the
            *  status afterwards. */
            ata_delay400(d);
            if(ata_wait_done(drive, d, ATA_TIMEOUT_XFER) != 0)
            {
                return -1;
            }
        }
        else
        {
            ata_insw(d->base + ATA_REG_DATA, p, ATA_SECTOR_SIZE / 2);
            ata_delay400(d);
        }

        p += ATA_SECTOR_SIZE;
    }

    if(writing)
    {
        /* Without this the sectors may still be sitting in the drive's write
        *  cache, and a power cut - or a reboot from the shell - loses them
        *  even though every command reported success. */
        if(ata_prepare(drive, d, lba) != 0)
        {
            return -1;
        }
        outportb(d->base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
        ata_delay400(d);
        if(ata_wait_done(drive, d, ATA_TIMEOUT_XFER) != 0)
        {
            return -1;
        }
    }

    err_reset();
    return 0;
}

int ata_read(int drive, uint32_t lba, uint32_t count, void *buf)
{
    return ata_transfer(drive, lba, count, buf, 0);
}

int ata_write(int drive, uint32_t lba, uint32_t count, const void *buf)
{
    /* The cast drops a const the transfer never uses: with writing set, the
    *  buffer is only ever read. Keeping one body for both directions is
    *  worth more than the constness of a pointer that stays inside this
    *  file. */
    return ata_transfer(drive, lba, count, (void *)buf, 1);
}
