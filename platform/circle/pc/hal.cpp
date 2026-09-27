#include "hal.h"
#include <circle/timer.h>
#include <circle/synchronize.h>
#include <fatfs/ff.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The guest's clock, read straight from the counter.
 *
 * This is on the hot path and not by a little: a game that waits for the
 * vertical retrace reads port 0x3DA in a tight loop, and on Tyrian one guest
 * instruction in twelve is that IN.  Every one of them lands here.
 *
 * Circle's CTimer::GetClockTicks does the same job correctly and more
 * carefully than is needed here: an instruction barrier, which flushes the
 * pipeline, then a read of CNTFRQ_EL0 that never changes, then a 64-bit
 * multiply and a 64-bit divide - and a divide is tens of cycles on this part.
 * The frequency is read once instead, and turned into a 32.32 fixed-point
 * scale so the conversion is one multiply-high.  The barrier is dropped: the
 * counter may be read a few cycles early, which cannot matter at microsecond
 * resolution.
 */
/* Shared with get_uticks_fast() in pc/compat/pico.h, which is what the hot
 * paths use; see the note there. */
extern "C" uint64_t g_uticks_scale;
uint64_t g_uticks_scale;                 /* microseconds per tick, 32.32 */
#define s_us_scale g_uticks_scale

static inline uint64_t counter_ticks(void)
{
    uint64_t c;
    asm volatile ("mrs %0, cntpct_el0" : "=r"(c));
    return c;
}

static inline uint64_t counter_to_us(uint64_t ticks)
{
    if (!s_us_scale) {
        uint64_t freq;
        asm volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
        if (!freq) freq = 19200000;              /* the Pi 3's crystal */
        s_us_scale = ((uint64_t)1000000 << 32) / freq;
    }
    return (uint64_t)(((__uint128_t)ticks * s_us_scale) >> 32);
}

extern "C" uint64_t time_us_64(void) { return counter_to_us(counter_ticks()); }
extern "C" uint32_t get_uticks(void) { return (uint32_t)time_us_64(); }
extern "C" uint32_t time_us_32(void) { return get_uticks(); }
extern "C" void sleep_us(uint32_t us) { if (us) CTimer::SimpleusDelay(us); }
extern "C" void sleep_ms(uint32_t ms) { if (ms) CTimer::SimpleusDelay(ms * 1000u); }
extern "C" uint64_t make_timeout_time_ms(uint32_t ms) { return time_us_64() + (uint64_t)ms * 1000u; }
extern "C" bool time_reached(uint64_t t) { return time_us_64() >= t; }

extern "C" void *pcmalloc(long size)
{
    if (size <= 0) return 0;
    return malloc((size_t) size);
}

extern "C" int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
    if (!phys_mem || !file || !file[0]) return -1;
    char path[256];
    const int n = snprintf(path, sizeof path, "386/%s", file);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return -1;
    const FSIZE_t fs = f_size(&fp);
    if (fs == 0 || fs > 0x40000u) { f_close(&fp); return -1; }
    const size_t size = (size_t) fs;
    uint8_t *dest = (uint8_t *) phys_mem;
    if (backward) {
        if (addr < size) { f_close(&fp); return -1; }
        dest += addr - size;
    } else {
        dest += addr;
    }
    UINT got = 0;
    const FRESULT fr = f_read(&fp, dest, (UINT)size, &got);
    const FRESULT close = f_close(&fp);
    if (fr != FR_OK || close != FR_OK || got != size) return -1;
    return (int) size;
}

/*
 * Make code the translator just wrote visible to instruction fetch.
 *
 * The data cache holds the words it stored and the instruction cache may
 * still hold whatever was at that address before, so both have to be told.
 * Skipping this is how a translator ends up running the previous tenant of
 * its arena.
 */
extern "C" void jit_sync_code(void *addr, uint32_t bytes)
{
    if (!bytes) return;
    /* Circle offers the range form for data and a whole-cache sweep for
     * instructions; there is no range form for the latter here, and a
     * translation is rare next to everything else, so the sweep is fine. */
    CleanAndInvalidateDataCacheRange((uintptr)addr, bytes);
    SyncDataAndInstructionCache();
}

extern "C" void circle_pc_redraw(void *opaque, int x, int y, int w, int h)
{
    CirclePcVideo *video = (CirclePcVideo *) opaque;
    if (!video || !video->screen) return;

    if (video->staging && video->physical && video->vga_surface) {
        /* vga_refresh currently redraws the full virtual surface, but honour
         * a future partial rectangle too.  Clip before doing any pointer
         * arithmetic so diagnostic builds cannot write past Circle's buffer. */
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x > 640 || y > 480 || w <= 0 || h <= 0) {
            return;
        }
        if (x + w > 640) w = 640 - x;
        if (y + h > 480) h = 480 - y;
        if ((uint32_t)x >= video->physical_width ||
            (uint32_t)y >= video->physical_height) {
            return;
        }
        uint32_t max_w = video->physical_width - video->offset_x;
        uint32_t max_h = video->physical_height - video->offset_y;
        if ((uint32_t)x >= max_w || (uint32_t)y >= max_h) {
            return;
        }
        if ((uint32_t)w > max_w - (uint32_t)x) w = (int)(max_w - (uint32_t)x);
        if ((uint32_t)h > max_h - (uint32_t)y) h = (int)(max_h - (uint32_t)y);
        /* When the renderer already drew into the scanout buffer there is
         * nothing to move. */
        if (video->vga_surface == video->physical) return;
        const size_t bytes = (size_t)w * 4u;
        const uint64_t dst_off0 =
            (uint64_t)(video->offset_y + (uint32_t)y) * video->physical_pitch +
            (uint64_t)(video->offset_x + (uint32_t)x) * 4u;
        const uint64_t src_off0 = (uint64_t)y * 640u * 4u + (uint64_t)x * 4u;
        /*
         * One call when the rectangle really is one run of memory at both
         * ends - full width, no centring offset, and a pitch with nothing
         * after the pixels - which is every mode this target runs.  Row by
         * row, memcpy had to find its footing afresh for each 2560-byte run.
         */
        if (bytes == video->physical_pitch && x == 0 && video->offset_x == 0 &&
            dst_off0 + bytes * (size_t)h <= video->physical_size) {
            memcpy(video->physical + dst_off0, video->vga_surface + src_off0,
                   bytes * (size_t)h);
        } else {
            for (int row = 0; row < h; ++row) {
                const uint64_t dst_off = dst_off0 + (uint64_t)row * video->physical_pitch;
                const uint64_t src_off = src_off0 + (uint64_t)row * 640u * 4u;
                if (dst_off + bytes > video->physical_size) break;
                memcpy(video->physical + dst_off, video->vga_surface + src_off, bytes);
            }
        }
    }
    /* CScreen::Update() copies Circle's private terminal backing store to the
     * physical framebuffer.  After VGA takes ownership that store contains
     * only Circle's blank cursor, so calling Update would overwrite the just
     * rendered guest frame.  The raw framebuffer is scanout memory and needs
     * no present operation. */
}

/* ---------------------------------------------------------------------- */
/* A drive the host has, standing in for a disk image                       */
/* ---------------------------------------------------------------------- */

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/device.h>
#include <circle/usb/usbfloppydevice.h>
#include <circle/usb/usbmassdevice.h>
#include <circle/netdevice.h>
#include "../../../src/blockdev.h"
#include "../../../src/ne2000.h"

/*
 * Circle hands out its block devices as CDevice, which seeks and reads in
 * bytes but insists on whole sectors underneath: the USB floppy driver
 * refuses an offset or a length that is not a multiple of 512.  The
 * controllers above do not know that - ide.c streams a sector out four bytes
 * at a time - so one sector is held here and served from.
 *
 * The cached sector is also what makes a write work: a partial write is a
 * read of the sector, the change, and the whole sector back, which is what
 * the drive requires and what a real controller does with its own buffer.
 */
namespace {

const unsigned HostSectorSize = 512;

/*
 * A whole track at a time, not a sector.
 *
 * A diskette turns at 300 revolutions a minute, so a sector comes back under
 * the head once every two hundred milliseconds - and a drive asked for one
 * sector waits for it.  Measured: the data phase of a one-sector READ(10) is
 * 209 ms, near enough exactly one revolution, whichever sector it is.  Ask
 * for eighteen and the same revolution delivers all of them, which is what
 * turns ten minutes of reading into thirty seconds.
 *
 * Eight, not eighteen, and that is not about the diskette.  A whole track
 * in one command is 9216 bytes, and a transfer that size stalls this drive:
 * the data phase fails, the drive then refuses even SEND DIAGNOSTIC, and
 * the next read stalls the same way - measured, over and over, while nine
 * sectors and fewer never failed once.  Eight is four kilobytes, a round
 * page, comfortably the safe side of wherever the limit really lies, and
 * still eight sectors for each revolution waited out instead of one.
 */
const unsigned HostBlockSectors = 8;
const unsigned HostBlockSize = HostSectorSize * HostBlockSectors;

struct HostBlk {
    /*
     * Held by name, not by pointer.
     *
     * A drive that wedges is re-attached in software, which destroys the
     * device object and builds a new one - so a pointer kept here would be
     * a pointer into freed memory the moment that happens.  The name
     * service costs a string compare and is always right.
     */
    char name[8];
    uint64_t sectors;
    uint64_t cached;            /* first sector in the buffer, or ~0 */
    unsigned cached_count;      /* how many of them are valid */
    bool     dirty;
    /*
     * A drive that has stopped answering.
     *
     * One of these can wedge and then refuse everything, and a guest that is
     * booting from it will ask for ever - which hangs the whole machine at
     * the BIOS with no way back, not even to the menu, because the emulator
     * is inside a USB transfer whenever anyone presses a key.  So after a
     * few refusals in a row the drive is treated as empty: the guest gets a
     * clean error, the BIOS moves on to the hard disk, and the machine stays
     * usable.  Putting a diskette in again is an eject and a re-select in
     * the disk menu, which rebinds it.
     */
    unsigned fails;
    /* A change has been seen and not yet passed on; see host_load(). */
    bool     announce;
    /*
     * Aligned, because Circle checks IS_CACHE_ALIGNED on the buffer it is
     * handed and quietly allocates a bounce buffer from the DMA heap for
     * every transfer that is not - nine kilobytes of allocate, copy and free
     * on every track read, and a heap that has to survive it.
     */
    alignas(64) uint8_t buf[HostBlockSize];
};

HostBlk s_host[4];
unsigned s_host_used;

CDevice *host_device(HostBlk *h)
{
    return CDeviceNameService::Get ()->GetDevice (h->name, TRUE);
}

bool host_flush(HostBlk *h)
{
    CDevice *device = host_device(h);
    if (!device) return false;
    if (!h->dirty) return true;
    const u64 at = h->cached * HostSectorSize;
    const unsigned len = h->cached_count * HostSectorSize;
    if (device->Seek(at) != at) return false;
    if (device->Write(h->buf, len) != (int)len) return false;
    h->dirty = false;
    return true;
}

unsigned s_host_moans;

/* The block of consecutive sectors that this one falls in. */
/*
 * Two, not six.
 *
 * Every attempt runs inside the guest's own execution, so a read that keeps
 * trying is a machine that has stopped: the user switched M602 to A: and the
 * whole thing froze while the drive was asked over and over.  Failing early
 * hands the guest an error it can show, and the drive is asked again in a
 * couple of seconds anyway.
 */
const unsigned HostGiveUpAfter = 2;

/*
 * A diskette that has been swapped for another one.
 *
 * Nothing above this notices by itself: the guest asks for a sector, the
 * block it falls in is already held here, and it is served without the drive
 * being touched - which after a swap is a page of the diskette that is now in
 * somebody's hand.  So the drive is asked, and it answers "medium may have
 * changed" exactly once per change; PollMedium() rate limits that to four
 * times a second, which is cheaper than it sounds next to a 200 ms
 * revolution.
 *
 * Only drives with removable media are asked - a USB floppy and a USB CD.
 * The other host devices here, a hard disk on a card, cannot have their media
 * changed behind the emulator's back.
 */
bool host_medium_changed(HostBlk *h, CDevice *device)
{
    if (strncmp(h->name, "ufd", 3) == 0)
        return ((CUSBFloppyDiskDevice *)device)->PollMedium();
    if (strncmp(h->name, "ucd", 3) == 0)
        return ((CUSBBulkOnlyMassStorageDevice *)device)->PollMedium();
    return false;
}

bool host_load(BlockDev *d, HostBlk *h, uint64_t sector)
{
    CDevice *device = host_device(h);
    if (!device) return false;
    if (host_medium_changed(h, device)) {
        /*
         * What is held belongs to the diskette that has gone, and so does
         * anything not yet written back - there is nowhere to put it now.
         * The size goes too, because the new diskette need not be the size
         * of the old one; it is learned again below.
         */
        h->cached = (uint64_t)-1;
        h->cached_count = 0;
        h->dirty = false;
        h->sectors = 0;
        h->announce = true;
    }
    /*
     * The size is asked for rather than remembered, because the drive may
     * have been empty when it was bound and have a diskette in it now.
     *
     * Zero does not mean stop.  It means the driver has not looked yet, and
     * it looks inside the read below - the only place it is allowed to touch
     * the drive at all.  Refusing here because the size was zero is how a
     * diskette put in after the machine started could never be found: the
     * one call that would have noticed it was the one being skipped.
     */
    const uint64_t was = h->sectors;
    h->sectors = device->GetSize() / HostSectorSize;
    /*
     * A diskette that has just appeared is not the one whose tracks are in
     * the buffer.  Whenever the drive goes from empty to holding something,
     * whatever is cached belongs to the diskette before it and has to go.
     */
    if (was == 0 && h->sectors != 0) {
        h->cached = (uint64_t)-1;
        h->cached_count = 0;
        h->dirty = false;
    }
    if (h->cached != (uint64_t)-1
        && sector >= h->cached && sector < h->cached + h->cached_count)
        return true;
    if (!host_flush(h)) return false;

    uint64_t first = sector - (sector % HostBlockSectors);
    unsigned count = HostBlockSectors;
    if (h->sectors && first + count > h->sectors)
        count = (unsigned)(h->sectors - first);
    const u64 want = first * HostSectorSize;
    const u64 got = device->Seek(want);
    if (got != want) {
        if (s_host_moans++ < 4)
            CLogger::Get ()->Write ("hostblk", LogWarning,
                "seek to %lu answered %lu", (unsigned long)want,
                (unsigned long)got);
        return false;
    }
    const unsigned len = count * HostSectorSize;
    const int n = device->Read(h->buf, len);
    if (n != (int)len) {
        if (s_host_moans++ < 4)
            CLogger::Get ()->Write ("hostblk", LogWarning,
                "read of %u sectors at %lu returned %d",
                count, (unsigned long)first, n);
        return false;
    }
    h->cached = first;
    h->cached_count = count;
    h->fails = 0;
    /*
     * Asked again, because the read above is what made the drive look at the
     * diskette: the size taken before it was the drive's answer from before
     * it had one.
     */
    if (h->sectors == 0)
        h->sectors = device->GetSize() / HostSectorSize;
    /*
     * And now, with a size to tell them, the layers above are told that this
     * is a different diskette - not when the change was noticed, because at
     * that moment nobody could have said how big the new one was.
     */
    if (h->announce) {
        h->announce = false;
        blk_host_media_changed(d);
    }
    return true;
}

/*
 * Nothing is latched.
 *
 * There used to be a count here that gave up on the drive after a couple of
 * refusals, on the grounds that a wedged one could hang the machine.  It
 * cannot any more - a failing read now returns in a fraction of a second -
 * and the latch did real harm: the first read from DOS failed, the second
 * was never attempted, and Retry could not work.  Every request the guest
 * makes gets a real attempt.
 */
bool host_failed(HostBlk *h)
{
    h->fails++;
    return false;
}

int host_seek(BlockDev *d, uint64_t off)
{
    HostBlk *h = (HostBlk *)d->ctx;
    if (!h) return -1;
    /*
     * A size of zero is not a size of zero; see host_load().
     *
     * This is what made booting from a real diskette take two attempts.  A
     * USB floppy answers GetSize() with nothing until the medium has been
     * established, and that happens inside the first read - so through the
     * whole of the BIOS's first read the size here was still zero, and this
     * refused every offset except zero.  The boot sector was therefore the
     * one sector the guest could have, its boot record asked for the root
     * directory at block 19, and the seek to it was turned down without the
     * drive being asked anything: "Disk I/O error", and a keypress fixed it
     * because by then a size had been learned.
     *
     * So an unknown size is asked about again here, and is no longer taken
     * as a reason to refuse: the read that follows is what establishes the
     * medium, and it is the only thing allowed to touch the drive.
     */
    if (h->sectors == 0) {
        CDevice *device = host_device(h);
        if (device) h->sectors = device->GetSize() / HostSectorSize;
    }
    if (h->sectors && off > h->sectors * HostSectorSize) return -1;
    d->pos = off;
    return 0;
}

int host_read(BlockDev *d, void *buf, unsigned len)
{
    HostBlk *h = (HostBlk *)d->ctx;
    uint8_t *out = (uint8_t *)buf;
    unsigned done = 0;
    if (!h) return -1;
    while (done < len) {
        const uint64_t sector = d->pos / HostSectorSize;
        /* An unknown size is not a size of zero; see host_load(). */
        if (h->sectors && sector >= h->sectors) break;
        if (!host_load(d, h, sector)) {
            host_failed(h);
            return done ? (int)done : -1;
        }
        /* However much of what is now held runs on from here. */
        const unsigned within =
            (unsigned)(d->pos - h->cached * HostSectorSize);
        unsigned take = h->cached_count * HostSectorSize - within;
        if (take > len - done) take = len - done;
        memcpy(out + done, h->buf + within, take);
        d->pos += take;
        done += take;
    }
    return (int)done;
}

int host_write(BlockDev *d, const void *buf, unsigned len)
{
    HostBlk *h = (HostBlk *)d->ctx;
    const uint8_t *in = (const uint8_t *)buf;
    unsigned done = 0;
    if (!h) return -1;
    while (done < len) {
        const uint64_t sector = d->pos / HostSectorSize;
        /* An unknown size is not a size of zero; see host_load(). */
        if (h->sectors && sector >= h->sectors) break;
        /* Read the block in first: a write of part of it must not lose the
         * rest, and the block is a whole track. */
        if (!host_load(d, h, sector)) {
            host_failed(h);
            return done ? (int)done : -1;
        }
        const unsigned within =
            (unsigned)(d->pos - h->cached * HostSectorSize);
        unsigned take = h->cached_count * HostSectorSize - within;
        if (take > len - done) take = len - done;
        memcpy(h->buf + within, in + done, take);
        h->dirty = true;
        d->pos += take;
        done += take;
        /*
         * Not flushed here.
         *
         * Flushing on every call wrote the whole eight-sector block again
         * for each sector the guest touched - eight revolutions where one
         * would do, and SYS took the best part of a minute.  The block is
         * written when the next one displaces it, which host_load() does,
         * and the controllers call blk_sync() when the guest's operation
         * ends: what must not happen is a write that reaches nothing at
         * all, and that is what sync is for.
         */
    }
    return (int)done;
}

uint64_t host_size(BlockDev *d)
{
    HostBlk *h = (HostBlk *)d->ctx;
    if (!h) return 0;
    CDevice *device = host_device(h);
    if (!device) return 0;
    /*
     * A drive that has not been looked in does not know what is in it.
     *
     * The ATAPI emulation asks how big the disc is before it will read
     * anything, and answers the guest "no disc" when that is zero - so a
     * size that is only learned by reading is never learned at all, and a
     * CD in the drive stayed invisible however many times the guest asked.
     * So the drive is looked in here, which it rate limits while it is
     * empty.  The floppy needs none of this: its BIOS reads sector zero
     * whatever the size says.
     */
    if (strncmp(h->name, "ucd", 3) == 0 && device->GetSize() == 0)
        ((CUSBBulkOnlyMassStorageDevice *)device)->CheckMedia();
    return device->GetSize();
}

int host_sync(BlockDev *d)
{
    HostBlk *h = (HostBlk *)d->ctx;
    return h && host_flush(h) ? 0 : -1;
}

}  /* namespace */

/* ---------------------------------------------------------------------- */
/* The board's Ethernet, behind the guest's network card                    */
/* ---------------------------------------------------------------------- */

/*
 * A bridge, not a stack.
 *
 * Circle has a TCP/IP stack and this uses none of it: the guest's own DOS or
 * Windows does that, and what it wants underneath is a wire.  So frames go
 * out through the adapter exactly as the guest wrote them, and whatever the
 * adapter hears goes to the guest exactly as it arrived - which puts the
 * machine on the same network as everything else, with its own address from
 * the same DHCP server, rather than behind something of ours.
 *
 * The adapter accepts what is addressed to itself, so the card is given the
 * adapter's own MAC address; see ne2000_set_mac().
 */
static int net_send(void *ctx, const uint8_t *buf, int len)
{
    CNetDevice *net = (CNetDevice *)ctx;
    if (!net || len <= 0) return 0;
    return net->SendFrame(buf, (unsigned)len) ? 1 : 0;
}

static int net_poll(void *ctx, uint8_t *buf, int capacity)
{
    CNetDevice *net = (CNetDevice *)ctx;
    if (!net || capacity < FRAME_BUFFER_SIZE) {
        /*
         * Circle's drivers write up to a whole frame buffer whatever the
         * frame's length, so a caller with a smaller buffer than that is one
         * this cannot safely answer.
         */
        return 0;
    }
    unsigned length = 0;
    if (!net->ReceiveFrame(buf, &length)) return 0;
    return (int)length;
}

/* Bound once, when the machine is built; null when the board has no adapter
 * at all, in which case the guest sees a card with nothing behind it. */
extern "C" int net_bind_host(NE2000State *card)
{
    CNetDevice *net = CNetDevice::GetNetDevice(0);
    if (!net || !card) return 0;

    const CMACAddress *mac = net->GetMACAddress();
    if (mac) {
        u8 address[MAC_ADDRESS_SIZE];
        mac->CopyTo(address);
        ne2000_set_mac(card, address);
    }

    NE2000Host host;
    host.send = net_send;
    host.poll = net_poll;
    host.ctx = net;
    ne2000_set_host(&host);
    return 1;
}

extern "C" int blk_bind_host(BlockDev *d, const char *which)
{
    if (!d || !which) return 0;
    CDevice *dev = CDeviceNameService::Get ()->GetDevice (which, TRUE);
    if (!dev) return 0;
    /* An empty drive binds too: it is a drive either way, and a diskette
     * put in later is found by the driver when someone next asks. */
    if (s_host_used >= sizeof s_host / sizeof s_host[0]) return 0;

    HostBlk *h = &s_host[s_host_used++];
    memset (h, 0, sizeof *h);
    strncpy (h->name, which, sizeof h->name - 1);
    h->sectors = dev->GetSize () / HostSectorSize;
    h->cached = (uint64_t)-1;

    memset (d, 0, sizeof *d);
    d->ctx = h;
    d->read = host_read;
    d->write = host_write;
    d->seek = host_seek;
    d->size = host_size;
    d->sync = host_sync;
    return 1;
}

extern "C" int blk_host_available(const char *which)
{
    return which && CDeviceNameService::Get ()->GetDevice (which, TRUE) != 0;
}
