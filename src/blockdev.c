/*
 * The file-backed drive, and the hook for a real one.  See blockdev.h.
 */

#include "blockdev.h"
#include <string.h>

#ifdef CIRCLE_BUILD
#include "ff.h"

static int file_seek(BlockDev *d, uint64_t off)
{
    FIL *f = (FIL *)d->ctx;
    if (!f) return -1;
    if (f_lseek(f, (FSIZE_t)off) != FR_OK) return -1;
    d->pos = off;
    return 0;
}

static int file_read(BlockDev *d, void *buf, unsigned len)
{
    FIL *f = (FIL *)d->ctx;
    UINT br = 0;
    if (!f) return -1;
    if (f_read(f, buf, len, &br) != FR_OK) return -1;
    d->pos += br;
    return (int)br;
}

static int file_write(BlockDev *d, const void *buf, unsigned len)
{
    FIL *f = (FIL *)d->ctx;
    UINT bw = 0;
    if (!f) return -1;
    if (f_write(f, buf, len, &bw) != FR_OK) return -1;
    d->pos += bw;
    return (int)bw;
}

static uint64_t file_size(BlockDev *d)
{
    FIL *f = (FIL *)d->ctx;
    return f ? (uint64_t)f_size(f) : 0;
}

static int file_sync(BlockDev *d)
{
    FIL *f = (FIL *)d->ctx;
    return f && f_sync(f) == FR_OK ? 0 : -1;
}

void blk_bind_file(BlockDev *d, void *fil, int readonly)
{
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->ctx = fil;
    d->read = file_read;
    d->write = file_write;
    d->seek = file_seek;
    d->size = file_size;
    d->sync = file_sync;
    d->readonly = (uint8_t)(readonly != 0);
}
#else
void blk_bind_file(BlockDev *d, void *fil, int readonly)
{
    (void)fil; (void)readonly;
    if (d) memset(d, 0, sizeof *d);
}
#endif

void blk_unbind(BlockDev *d)
{
    if (d) memset(d, 0, sizeof *d);
}

/*
 * No host devices unless the platform provides them.  The Circle target
 * defines this in hal.cpp; anything else links this and a drive can only
 * ever be a file, which is what it was before.
 */
__attribute__((weak)) int blk_bind_host(BlockDev *d, const char *which)
{
    (void)d; (void)which;
    return 0;
}

__attribute__((weak)) int blk_host_available(const char *which)
{
    (void)which;
    return 0;
}
