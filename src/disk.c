/*
 * Disk management for RP2350 - adapted from pico-286
 * Uses FatFS for SD card access
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <hardware/gpio.h>
#include "i386.h"
#include "ff.h"
#include "blockdev.h"
#include "ems.h"
#include "vga.h"

extern FATFS fs;

int hdcount = 0;

static uint8_t sectorbuffer[512];

struct struct_fdd {
    FIL fil;
    /* Where the sectors actually come from: this file, or a drive the host
     * has.  Everything below the controllers goes through it. */
    BlockDev dev;
    char* name;
    uint32_t usable_size;
    uint16_t cyls;
    uint16_t sects;
    uint16_t heads;
    uint8_t readonly;
    uint8_t drive_type;  /* BIOS/CMOS type: 1=360K 2=1.2M 3=720K 4=1.44M 5=2.88M */
} fdd[2] = {
    [0] = { .drive_type = 4 },  // A: floppy, default 1.44M
    [1] = { .drive_type = 4 },  // B: floppy, default 1.44M
};  // 0-1: floppy, 2-3: HDD, 4: CD-ROM

struct struct_ata {
    FIL fil;
    BlockDev dev;
    char* name;
    FSIZE_t usable_size;
    uint16_t cyls;
    uint16_t sects;
    uint16_t heads;
    uint8_t iscdrom;
    uint8_t drive_type;
} ata[4] = { 0 };


static CPUI386 *disk_cpu = NULL;
static uint8_t *disk_mem = NULL;
static VGAState *disk_vga = NULL;
void disk_set_vga(VGAState *vga) { disk_vga = vga; }

/* Copy sector data to guest memory, routing VGA IOMEM through vga_mem_write */
static inline void disk_copy_to_guest(uint8_t *phys_mem, uint32_t guest,
                                      const uint8_t *buf, uint32_t len)
{
    if (disk_vga && guest >= 0xA0000 && guest + len <= 0xC0000) {
        for (uint32_t i = 0; i < len; i++)
            vga_mem_write(disk_vga, guest - 0xA0000 + i, buf[i]);
        return;
    }
    ems_copy_to_guest(phys_mem, guest, buf, len);
}

/* Copy from guest memory, routing VGA IOMEM through vga_mem_read */
static inline void disk_copy_from_guest(uint8_t *phys_mem, uint32_t guest,
                                        uint8_t *buf, uint32_t len)
{
    if (disk_vga && guest >= 0xA0000 && guest + len <= 0xC0000) {
        for (uint32_t i = 0; i < len; i++)
            buf[i] = vga_mem_read(disk_vga, guest - 0xA0000 + i);
        return;
    }
    ems_copy_from_guest(phys_mem, guest, buf, len);
}
static void (*disk_cmos_update_cb)(uint8_t type_a, uint8_t type_b) = NULL;
void disk_set_cmos_callback(void (*cb)(uint8_t, uint8_t)) { disk_cmos_update_cb = cb; }

static void (*disk_fdc_mediachange_cb)(int drive) = NULL;
void disk_set_fdc_mediachange_callback(void (*cb)(int drive)) { disk_fdc_mediachange_cb = cb; }

/*
 * Three parameters, not two.
 *
 * disk.h declares this callback with a was_present flag and pc.c defines it
 * with one - only the pointer here was a parameter short, so what the
 * controller read as "was there a disc before this one" was whatever the
 * register happened to hold.  It decides whether the guest is given a unit
 * attention, which is what makes it re-read the table of contents.
 */
static void (*disk_cdrom_change_cb)(int drive, const char *filename,
                                    int was_present) = NULL;
void disk_set_cdrom_change_callback(void (*cb)(int drive, const char *filename,
                                               int was_present)) { disk_cdrom_change_cb = cb; }

/* Установить FDPT (Fixed Disk Parameter Table) и INT 41h/46h векторы.
 * Вызывается при каждом INT 13h для HDD — перезаписывает то что мог
 * поставить SeaBIOS во время boot.
 * Раскладка IBM AT BIOS: +00 word cyls, +02 byte heads,
 * +04..07 precomp (0xFFFF), +09 control, +0D word landing zone, +0F byte sects. */
static void install_fdpt(void) {
    /* Всегда перезаписываем — SeaBIOS может восстановить свои векторы */
#define FDPT(base, c, h, s) do { \
    uint16_t _c=(c); uint8_t _h=(h),_s=(s),_ctrl=(_h>8)?0x08:0x00; \
    disk_mem[(base)+0x00]=_c&0xFF; disk_mem[(base)+0x01]=_c>>8; \
    disk_mem[(base)+0x02]=_h;      disk_mem[(base)+0x03]=0; \
    disk_mem[(base)+0x04]=0xFF;    disk_mem[(base)+0x05]=0xFF; /* reduced write */ \
    disk_mem[(base)+0x06]=0xFF;    disk_mem[(base)+0x07]=0xFF; /* write precomp */ \
    disk_mem[(base)+0x08]=0;       disk_mem[(base)+0x09]=_ctrl; \
    disk_mem[(base)+0x0A]=0;       disk_mem[(base)+0x0B]=0; \
    disk_mem[(base)+0x0C]=0; \
    disk_mem[(base)+0x0D]=_c&0xFF; disk_mem[(base)+0x0E]=_c>>8; /* landing zone */ \
    disk_mem[(base)+0x0F]=_s; /* sectors per track */ \
} while(0)
    if (ata[0].name) {
        FDPT(0x522, ata[0].cyls, ata[0].heads, ata[0].sects);
        disk_mem[0x104]=0x22; disk_mem[0x105]=0x05;
        disk_mem[0x106]=0x00; disk_mem[0x107]=0x00;
    }
    if (ata[1].name) {
        FDPT(0x532, ata[1].cyls, ata[1].heads, ata[1].sects);
        disk_mem[0x118]=0x32; disk_mem[0x119]=0x05;
        disk_mem[0x11A]=0x00; disk_mem[0x11B]=0x00;
    }
    // TODO: ata[2,3]
#undef FDPT
}

static void update_floppy_cmos(void) {
    if (!disk_cmos_update_cb) return;
    uint8_t ta = fdd[0].drive_type;
    uint8_t tb = fdd[1].drive_type;
    disk_cmos_update_cb(ta, tb);
}

// Detect fixed VHD (footer at end)
static int detect_vhd(FIL *file, FSIZE_t size) {
    if (size < 512) return 0;
    UINT br;
    f_lseek(file, size - 512);
    if (FR_OK != f_read(file, sectorbuffer, 512, &br) || br != 512)
        return 0;
    // Footer starts with "conectix"
    if (memcmp(sectorbuffer, "conectix", 8) == 0) {
        return 1;
    }
    return 0;
}

void disk_set_cpu(CPUI386 *cpu) {
    disk_cpu = cpu;
    disk_mem = cpu_get_phys_mem(cpu);
}

/* The map is this file's to free; f_close() only forgets the pointer. */
static void close_image(FIL *pf)
{
#if FF_USE_FASTSEEK
    DWORD *tbl = pf->cltbl;
    pf->cltbl = 0;
    free(tbl);
#endif
    f_close(pf);
}

void ejectdisk(uint8_t drivenum, bool is_fdd) {
    if (drivenum < 2 && is_fdd && fdd[drivenum].name) {
        blk_unbind(&fdd[drivenum].dev);
        close_image(&fdd[drivenum].fil);
        free(fdd[drivenum].name);
        fdd[drivenum].name = 0;
        /* Empty floppy drive must fall back to default 1.44M type */
        fdd[drivenum].drive_type = 4;
        fdd[drivenum].cyls = 80;
        fdd[drivenum].heads = 2;
        fdd[drivenum].sects = 18;
        update_floppy_cmos();
        if (disk_fdc_mediachange_cb)
            disk_fdc_mediachange_cb(drivenum);
        return;
    }
    if (drivenum < 4 && ata[drivenum].iscdrom && ata[drivenum].name) {
        /* ATA eject: CD (from GUI or insertdisk) */
        blk_unbind(&ata[drivenum].dev);
        close_image(&ata[drivenum].fil);
        free(ata[drivenum].name);
        ata[drivenum].name = 0;
        if (disk_cdrom_change_cb)
            disk_cdrom_change_cb(drivenum, NULL, 1);
        return;
    }
    if (drivenum < 4 && ata[drivenum].name) {
        /* HDD eject (e.g. from GUI for HDD drives) */
        blk_unbind(&ata[drivenum].dev);
        close_image(&ata[drivenum].fil);
        free(ata[drivenum].name);
        ata[drivenum].name = 0;
        hdcount--;
    }
}

/*
 * Tell FatFs where the image's clusters are, once, instead of on every seek.
 *
 * f_lseek() walks the FAT chain from the start of the file whenever it is
 * asked to go backwards, and a disk image is gigabytes long.  Measured while
 * Tyrian loaded a level: 1760 seeks cost 3479 ms, against 508 ms for the
 * 880 KB those seeks were positioning for - seven times more time spent
 * finding the data than reading it.  The guest task is also the one that
 * refills the sound queue, so a stall that long empties it, and that is the
 * dropout heard during a load.
 *
 * A cluster link map turns the walk into a lookup in memory.  Fast seek asks
 * in return that the file not be extended while the map is attached, which a
 * fixed-size image never is.  If the map cannot be built the drive still
 * works, just as slowly as before.
 */
static void enable_fast_seek(FIL *pf)
{
#if FF_USE_FASTSEEK
    /* Two words per fragment plus a header.  A freshly written image is one
     * fragment; this leaves room for a badly scattered one before asking. */
    DWORD n = 256;
    for (int attempt = 0; attempt < 2; attempt++) {
        DWORD *tbl = malloc(n * sizeof *tbl);
        if (!tbl) break;
        tbl[0] = n;
        pf->cltbl = tbl;
        const FRESULT res = f_lseek(pf, CREATE_LINKMAP);
        if (res == FR_OK) { f_lseek(pf, 0); return; }
        /* On FR_NOT_ENOUGH_CORE the table's first word holds the size it
         * wanted, so the second attempt is the last one needed. */
        const DWORD needed = tbl[0];
        pf->cltbl = 0;
        free(tbl);
        if (res != FR_NOT_ENOUGH_CORE || needed <= n) break;
        n = needed;
    }
    pf->cltbl = 0;
#else
    (void)pf;
#endif
}

/*
 * The geometry a diskette of this size has.
 *
 * Pulled out of insertdisk() because it is needed twice: once when a drive is
 * given something to read, and again when a real drive reports that the
 * diskette in it has been swapped for another one.  The BIOS's CHS arithmetic
 * is done with these numbers, so a 720K diskette read as though it had
 * eighteen sectors to a track is read in the wrong places.
 */
static void fdd_geometry_from_size(uint8_t drivenum, FSIZE_t size)
{
    uint16_t cyls, heads, sects, drive_type;

    switch (size) {
        case 163840:  cyls=40; heads=1; sects=8;  drive_type=1; break; //160K
        case 184320:  cyls=40; heads=1; sects=9;  drive_type=1; break; //180K
        case 327680:  cyls=40; heads=2; sects=8;  drive_type=1; break; //320K
        case 368640:  cyls=40; heads=2; sects=9;  drive_type=1; break; //360K
        case 655360:  cyls=80; heads=2; sects=8;  drive_type=3; break; //640K
        case 737280:  cyls=80; heads=2; sects=9;  drive_type=3; break; //720K
        case 1228800: cyls=80; heads=2; sects=15; drive_type=2; break; //1.2M
        case 1474560: cyls=80; heads=2; sects=18; drive_type=4; break; //1.44M
        case 1556480: cyls=80; heads=2; sects=19; drive_type=4; break; //1.49M
        case 1638400: cyls=80; heads=2; sects=20; drive_type=4; break; //1.60M
        case 1720320: cyls=80; heads=2; sects=21; drive_type=4; break; //DMF
        case 1763328: cyls=82; heads=2; sects=21; drive_type=4; break; //tomsrtbt
        case 1802240: cyls=80; heads=2; sects=22; drive_type=4; break;
        case 1884160: cyls=80; heads=2; sects=23; drive_type=4; break;
        case 1966080: cyls=80; heads=2; sects=24; drive_type=4; break;
        case 2048000: cyls=80; heads=2; sects=25; drive_type=4; break;
        case 2129920: cyls=80; heads=2; sects=26; drive_type=4; break;
        case 2211840: cyls=80; heads=2; sects=27; drive_type=4; break;
        case 2293760: cyls=80; heads=2; sects=28; drive_type=4; break;
        case 2375680: cyls=80; heads=2; sects=29; drive_type=4; break;
        case 2457600: cyls=80; heads=2; sects=30; drive_type=4; break;
        case 2949120: cyls=80; heads=2; sects=36; drive_type=5; break; //2.88M
        default: cyls=80; heads=2; sects=18; drive_type=4;
    }
    fdd[drivenum].drive_type = drive_type;
    fdd[drivenum].usable_size = size;
    fdd[drivenum].cyls = cyls;
    fdd[drivenum].heads = heads;
    fdd[drivenum].sects = sects;
}

uint8_t insertdisk(uint8_t drivenum, bool is_fdd, bool is_cd, const char *pathname) {
    if ((is_fdd && drivenum >= 2) || drivenum >= 4) return false;
    /* Before anything is ejected below: a disc that replaces another one is
     * what the guest has to be told about. */
    const int cd_was_present = !is_fdd && is_cd
                             && ata[drivenum].iscdrom && ata[drivenum].name != 0;
    // Build full path (files are in 386/ directory)
    char path[256];
    snprintf(path, sizeof(path), "386/%s", pathname);

    /* Circle normally exposes images read-only.  The explicit build option
     * below is intentionally narrower than general SD write access: only the
     * already-configured virtual image below 386/ can be opened for update. */
#if defined(CIRCLE_BUILD) && !defined(CIRCLE_DISK_WRITE)
    BYTE fmode = FA_READ;
#else
    /* CD-ROMs are read-only; regular disks need write access */
    BYTE fmode = is_cd ? FA_READ : (FA_READ | FA_WRITE);
#endif
    FIL* pf = is_fdd ? &fdd[drivenum].fil : &ata[drivenum].fil;
    BlockDev *bd = is_fdd ? &fdd[drivenum].dev : &ata[drivenum].dev;

    /*
     * A real drive rather than an image.
     *
     * "usb:" names a device the host has enumerated, optionally followed by
     * the name it knows it by - "usb:" alone means the first USB floppy.
     * Everything past this point is the same for both: the size decides the
     * geometry, and the controllers above only ever see a block source.
     */
    const int from_host = strncmp(pathname, "usb:", 4) == 0;
    FSIZE_t size;

    if (from_host) {
        if (blk_present(bd)) ejectdisk(drivenum, is_fdd);
        if (!blk_bind_host(bd, pathname[4] ? pathname + 4 : "ufd1"))
            return 0;
        size = (FSIZE_t)blk_size(bd);
        /*
         * An empty drive is still a drive.  The geometry has to come from
         * somewhere before there is a diskette to read it from, and 1.44 MB
         * is what a USB floppy is; the guest then sees a drive that is not
         * ready, which is exactly what a real one looks like.
         */
        if (size == 0 && is_fdd) size = 1474560;
        if (is_fdd) fdd[drivenum].name = strdup(pathname);
        else        ata[drivenum].name = strdup(pathname);
    } else {
    if (pf->obj.fs) {
        /* Eject whatever is currently in the drive before inserting new image */
        if (is_fdd)
            ejectdisk(drivenum, true);
        else if (is_cd)
            ejectdisk(drivenum, false);
        else
            ejectdisk(drivenum, false);
    }
    FRESULT fres = f_open(pf, path, fmode);
    int opened_readonly = (fmode == FA_READ);
    if (FR_OK != fres) {
        /* Fall back to read-only if write-open failed (e.g. write-protected card) */
        if (fmode != FA_READ) {
            fres = f_open(pf, path, FA_READ);
            opened_readonly = 1;
        }
    }
    if (FR_OK != fres) {
        return 0;
    }
    enable_fast_seek(pf);
    blk_bind_file(bd, pf, opened_readonly);
    if(is_fdd) fdd[drivenum].name = strdup(pathname);
    else ata[drivenum].name = strdup(pathname);
    size = f_size(pf);
    }

    /* A diskette in a drive is not a VHD, and has no footer to discount. */
    int is_vhd = from_host ? 0 : detect_vhd(pf, size);
    FSIZE_t usable_size = size;

    if (is_vhd) {
        // Fixed VHD: subtract 512-byte footer
        usable_size -= 512;
        //printf("disk: '%s': VHD detected, data size %u bytes\n", pathname, (unsigned)usable_size);
    }

    /* CD-ROM images are read-only sector images (2048-byte sectors logically,
     * but the block layer always uses 512-byte sectors for IDE).
     * Skip geometry/size validation for CD-ROMs. */
    if (is_cd) {
        size_t iso_sectors = size / 512;  /* nb_sectors for block layer */
        ata[drivenum].iscdrom    = 1;
        ata[drivenum].cyls       = 0;
        ata[drivenum].heads      = 0;
        ata[drivenum].sects      = 0;
        if (disk_cdrom_change_cb)
            disk_cdrom_change_cb(drivenum, path, cd_was_present);
        return 1;
    }
    // Validate size constraints (non-CD-ROM only)
    /* Allow HDD images up to 8 GiB. The old ~503 MiB limit was inherited
     * from the original CHS-oriented disk layer. X86Pi's IDE path uses
     * LBA28 and FatFS is built with exFAT/64-bit FSIZE_t, so a 7 GiB image
     * is representable end-to-end. */
    if (usable_size < 360ULL * 1024ULL ||
        usable_size > 8ULL * 1024ULL * 1024ULL * 1024ULL ||
        (usable_size & 511ULL)) {
        if (from_host) blk_unbind(bd);
        else           f_close(pf);
        return 0;
    }
    // Determine geometry (cyls, heads, sects)
    uint16_t cyls = 0, heads = 0, sects = 0, drive_type = 47;
    if (!is_fdd) {  // Hard disk
        /*
         * Legacy BIOS CHS translation.
         *
         * The original HDD limit (0x1f782000 bytes) was exactly
         * 1023 cylinders * 16 heads * 63 sectors * 512 bytes. Simply
         * lifting that limit leaves large images reporting >1023 cylinders,
         * which DOS FORMAT rejects as invalid device geometry.
         *
         * For larger disks expose the traditional translated geometry
         * 255 heads / 63 sectors. A 7 GiB image becomes ~913 cylinders,
         * comfortably inside the BIOS 10-bit cylinder limit. LBA28 still
         * carries the full sector count through ATA IDENTIFY.
         */
        const FSIZE_t legacy_chs_limit =
            1023ULL * 16ULL * 63ULL * 512ULL;
        const FSIZE_t translated_chs_limit =
            1023ULL * 255ULL * 63ULL * 512ULL;

        sects = 63;
        heads = (usable_size > legacy_chs_limit) ? 255 : 16;

        if (usable_size > translated_chs_limit) {
            f_close(pf);
            return 0;
        }

        // Keep MBR geometry detection only for legacy-sized images.
        // Large images must keep stable 255/63 translation across reboots.
        if (usable_size <= legacy_chs_limit) {
            UINT br;
            f_lseek(pf, 0);
            if (FR_OK == f_read(pf, sectorbuffer, 512, &br) && br == 512
                && sectorbuffer[510] == 0x55 && sectorbuffer[511] == 0xAA) {
                for (int p = 0; p < 4; p++) {
                    uint8_t *pe = &sectorbuffer[0x1BE + p * 16];
                    if (pe[4] == 0) continue;
                    uint8_t end_h = pe[5];
                    uint8_t end_s = pe[6] & 0x3F;
                    if (end_s > 0 && end_h > 0) {
                        heads = end_h + 1;
                        sects = end_s;
                    }
                }
            }
        }

        cyls = (uint16_t)(usable_size / ((FSIZE_t)sects * heads * 512ULL));
        ata[drivenum].drive_type = drive_type;
        ata[drivenum].usable_size = usable_size;
        ata[drivenum].cyls = cyls;
        ata[drivenum].heads = heads;
        ata[drivenum].sects = sects;
        hdcount++;
    } else {  // Floppy disk
        fdd_geometry_from_size(drivenum, usable_size);
#if defined(CIRCLE_BUILD) && !defined(CIRCLE_DISK_WRITE)
        fdd[drivenum].readonly = 1;
#else
        fdd[drivenum].readonly = 0;
#endif
        // Update CMOS floppy type if floppy
        update_floppy_cmos();
        if (disk_fdc_mediachange_cb)
            disk_fdc_mediachange_cb(drivenum);
    }
    return 1;
}

//=============================================================================
// Public API for Disk UI
//=============================================================================

/*
 * A real drive has told the block layer that what is in it is a different
 * diskette.  Everything above reads it through the geometry held here, so
 * that has to follow the medium rather than stay at whatever was in the drive
 * when it was bound - and the controller is told, which is what the change
 * line on a real cable would have done.
 */
void blk_host_media_changed(BlockDev *d)
{
    for (uint8_t i = 0; i < 2; i++) {
        if (&fdd[i].dev != d) continue;

        fdd_geometry_from_size(i, (FSIZE_t)blk_size(d));
        update_floppy_cmos();
        if (disk_fdc_mediachange_cb)
            disk_fdc_mediachange_cb(i);
        return;
    }

    /*
     * A disc in a real CD drive, changed the same way.  There is no geometry
     * to work out - a CD is 2048 byte blocks and the controller asks the
     * block layer how many - but the guest has to be told, or it goes on
     * believing the table of contents it read from the disc before.  The
     * drive stays bound whether or not there is a disc in it; the ATAPI
     * emulation reads a size of zero as an empty drive.
     */
    for (uint8_t i = 0; i < 4; i++) {
        if (&ata[i].dev != d || !ata[i].iscdrom) continue;

        if (disk_cdrom_change_cb)
            disk_cdrom_change_cb(i, ata[i].name, 1);
        return;
    }
}

// Check if disk is inserted in specified drive
uint8_t ata_is_inserted(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return !!ata[drivenum].name;
}

uint8_t fdd_is_inserted(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return !!fdd[drivenum].name;
}

// Set CD-ROM flag for a drive
void disk_set_cdrom(uint8_t drivenum, uint8_t iscdrom) {
    if (drivenum >= 4) return;
    ata[drivenum].iscdrom = iscdrom;
}

// Check if drive is CD-ROM
uint8_t ata_is_cdrom(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return ata[drivenum].iscdrom;
}

const char* fdd_get_filename(int i) {
    if (i >= 2) return NULL;
    return fdd[i].name;
}

const char* ata_get_filename(int i) {
    if (i >= 4) return NULL;
    return ata[i].name;
}

BlockDev* fdd_get_dev(uint8_t drivenum) {
    return drivenum < 2 ? &fdd[drivenum].dev : 0;
}

BlockDev* ata_get_dev(uint8_t drivenum) {
    return drivenum < 4 ? &ata[drivenum].dev : 0;
}

FIL* fdd_get_file(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return &fdd[drivenum].fil;
}

uint16_t fdd_get_cyls(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return fdd[drivenum].cyls;
}
uint16_t fdd_get_heads(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return fdd[drivenum].heads;
}
uint16_t fdd_get_sects(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return fdd[drivenum].sects;
}
uint32_t fdds_types() { return ((fdd[1].drive_type & 0xF) << 4) | (fdd[0].drive_type & 0xF); }

FIL* ata_get_file(uint8_t drivenum) {
    if (drivenum >= 4) return NULL;
    return &ata[drivenum].fil;
}
uint16_t ata_get_cyls(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return ata[drivenum].cyls;
}
uint16_t ata_get_heads(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return ata[drivenum].heads;
}
uint16_t ata_get_sects(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return ata[drivenum].sects;
}
