#ifndef IDE_H
#define IDE_H
/*
 * IDE emulation
 * 
 * Copyright (c) 2003-2016 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdint.h>
#include "ff.h"

typedef struct IDEIFState IDEIFState;

IDEIFState *ide_allocate(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level));

/* Attach HDD: opens file internally */
int ide_attach(IDEIFState *s, int drive, const char *filename);

/* Attach HDD using already-open FIL (avoids FatFS double-open).
 * Geometry is pre-detected by the disk layer (insertdisk). */
struct BlockDev;
int ide_attach_ata(IDEIFState *s, int drive, struct BlockDev *dev,
                   int cylinders, int heads, int sectors);

/* Attach empty ATAPI CD-ROM slot (no image loaded yet) */
int ide_attach_cd(IDEIFState *s, int drive);

/* Returns non-zero if a drive (0=master, 1=slave) is attached */
int ide_has_drive(IDEIFState *s, int drive);

/* Hot-swap the disc: the drive's block source on insert, NULL to eject.
 * That source may be a file or a real drive; see blockdev.h. */
void ide_change_cd(IDEIFState *sif, int drive, struct BlockDev *dev,
                   int was_present);

void     ide_data_writew(void *opaque, uint32_t val);
uint32_t ide_data_readw(void *opaque);
void     ide_data_writel(void *opaque, uint32_t val);
uint32_t ide_data_readl(void *opaque);
int      ide_data_write_string(void *opaque, uint8_t *buf, int size, int count);
int      ide_data_read_string(void *opaque, uint8_t *buf, int size, int count);

void     ide_ioport_write(void *opaque, uint32_t offset, uint32_t val);
uint32_t ide_ioport_read(void *opaque, uint32_t offset);
void     ide_cmd_write(void *opaque, uint32_t val);
uint32_t ide_status_read(void *opaque);

/* Sectors the host would not write, and reads that came up short.  Both are
 * silent corruption from the guest's point of view; see ide.c. */
extern uint32_t g_ide_write_lost, g_ide_read_short;
/* Which ATA commands the disk has been given, indexed by opcode. */
extern uint32_t g_ide_cmd_hist[256];
/* Every command any drive has been given: the front panel's disk light. */
extern uint32_t g_ide_activity;
extern uint32_t g_ide_tf_unselected, g_ide_cmd_errors;
/* One line per recent disk command, oldest first; 0 past the last. */
int ide_trace_line(int i, char *buf, int size);
/* Flush drives written since the last flush once the disk has been quiet
 * for a while, or at once with force; see ide.c. */
void ide_sync_idle(int force);

#include "pci.h"
PCIDevice *piix3_ide_init(PCIBus *pci_bus, int devfn);

void ide_fill_cmos(IDEIFState *s, void *cmos,
                   uint8_t (*set)(void *cmos, int addr, uint8_t val));

#endif /* IDE_H */
