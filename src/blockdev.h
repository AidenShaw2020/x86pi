/*
 * Where a drive's sectors come from.
 *
 * Every emulated drive here used to be a FatFS file, and the controllers said
 * so: fdc.c and ide.c took a FIL * and called f_lseek(), f_read() and
 * f_write() on it directly.  That is fine until the sectors are somewhere
 * else - a real diskette in a USB drive, a real disc in a USB CD-ROM - and
 * then there is no seam to put them behind.
 *
 * This is that seam, and it is deliberately shaped like the thing it
 * replaces: a cursor, a read and a write that move it, and a size.  The
 * conversion is then a rename rather than a rewrite, and a controller cannot
 * tell a file from a drive.
 *
 * Reads and writes return the number of bytes moved, which is short only at
 * the end of the media, or negative on a fault.
 */
#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BlockDev BlockDev;

struct BlockDev {
    /* Null in every one of these means "no media": the drive exists, there is
     * nothing in it, and the controller answers accordingly. */
    int      (*read)(BlockDev *d, void *buf, unsigned len);
    int      (*write)(BlockDev *d, const void *buf, unsigned len);
    int      (*seek)(BlockDev *d, uint64_t off);
    uint64_t (*size)(BlockDev *d);
    int      (*sync)(BlockDev *d);  /* 0 on success, -1 on failure */
    void      *ctx;          /* the FIL, the USB device, whatever it is */
    uint64_t   pos;          /* the cursor, for backends that need it kept */
    uint8_t    readonly;
};

static inline int blk_present(BlockDev *d) { return d && d->read; }

/*
 * Told when a real drive reports that what is in it is a different medium.
 * The block layer notices - it is the only thing talking to the drive - and
 * the layer that knows about drives has to follow: the geometry comes from
 * the size, and the controller wants to know.  Implemented by disk.c.
 */
void blk_host_media_changed(BlockDev *d);

static inline int blk_seek(BlockDev *d, uint64_t off)
{
    return blk_present(d) && d->seek ? d->seek(d, off) : -1;
}

static inline int blk_read(BlockDev *d, void *buf, unsigned len)
{
    return blk_present(d) ? d->read(d, buf, len) : -1;
}

static inline int blk_write(BlockDev *d, const void *buf, unsigned len)
{
    return blk_present(d) && d->write && !d->readonly
         ? d->write(d, buf, len) : -1;
}

static inline uint64_t blk_size(BlockDev *d)
{
    return blk_present(d) && d->size ? d->size(d) : 0;
}

static inline int blk_sync(BlockDev *d)
{
    return blk_present(d) && d->sync ? d->sync(d) : -1;
}

/* Read or write at a position in one call, which is what most callers want
 * and what every caller did by hand. */
static inline int blk_pread(BlockDev *d, uint64_t off, void *buf, unsigned len)
{
    if (blk_seek(d, off) < 0) return -1;
    return blk_read(d, buf, len);
}

static inline int blk_pwrite(BlockDev *d, uint64_t off, const void *buf,
                             unsigned len)
{
    if (blk_seek(d, off) < 0) return -1;
    return blk_write(d, buf, len);
}

/* Bind a drive to a FatFS file already opened by the caller; see disk.c. */
struct FIL_s;
void blk_bind_file(BlockDev *d, void *fil, int readonly);
void blk_unbind(BlockDev *d);

/*
 * Bind a drive to a device the host provides - on the Circle target, a USB
 * floppy or CD-ROM that Circle has enumerated.  The host side implements
 * this; a target without one leaves it returning zero and nothing changes.
 * `which` names the device the way the host knows it, such as "ufd1".
 */
int blk_bind_host(BlockDev *d, const char *which);

/* Whether such a device is there at all, for a menu that has to decide
 * whether to offer it. */
int blk_host_available(const char *which);

#ifdef __cplusplus
}
#endif

#endif /* BLOCKDEV_H */
