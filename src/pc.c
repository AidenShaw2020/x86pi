/* Measurement build only: skip the periodic OPL refill so the ceiling of
 * moving it off core 0 can be read directly.  Audio is silent with this on;
 * never ship it. */
#ifndef FRANK_NO_ADLIB
#define FRANK_NO_ADLIB 0
#endif

#include "pc.h"
#include "audiodiag.h"
#include "ide.h"
#include "blockdev.h"
#include "dss.h"
#include "misc.h"
#include "gameport.h"

/* Guest accesses in 0x220-0x22F, counted before the enable flag is consulted:
 * a driver that finds no card either asked and disliked the answer, or never
 * asked at all, and those call for different work. */
uint32_t g_sb_port_reads, g_sb_port_writes;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <hardware/watchdog.h>

/* The Circle PC diagnostic build records wrapper-level I/O only.  The
 * production targets compile CIRCLE_PC_IO_NOTE() to a no-op. */
#if defined(CIRCLE_PC_DIAG)
#include "circle_pc_diag.h"
#else
#define CIRCLE_PC_IO_NOTE(...) ((void)0)
#endif

#include "mpu401.c.inl"
void netredirect_init(CPUI386 *cpu, int enable);

#ifdef USEKVM
#define cpu_raise_irq cpukvm_raise_irq
#define cpu_get_cycle cpukvm_get_cycle
#else
#define cpu_raise_irq cpui386_raise_irq
#define cpu_get_cycle cpui386_get_cycle
#endif

/* ---- Emulink FDD: simple virtual floppy on ports 0xF1F0/0xF1F4 ----------
 * Protocol (matches tiny386 / this BIOS):
 *   OUT 0xF1F0, cmd    – set command (resets argi to 0)
 *   OUT 0xF1F4, arg    – push argument (up to 4); executes after 3rd arg
 *   IN  0xF1F0         – read 32-bit status / result
 *   REP INSB  0xF1F4   – bulk read data (cmd 0x101)
 *   REP OUTSB 0xF1F4   – bulk write data (cmd 0x102)
 *
 * Commands:
 *   0x000 – identify: status = 0xAA55FF00
 *   0x100 – probe drives: status bit6=drv0 present, bit2=drv1 present
 *   0x101 – read sectors:  args[0]=drive, args[1]=CHS(c<<16|h<<8|s), args[2]=count
 *   0x102 – write sectors: same args, then REP OUTSB
 * -------------------------------------------------------------------------*/

void io_trace_note(int kind, int port, int val);

static void emulink_exec(PC *pc)
{
	switch (pc->emulink.cmd) {
	case 0x000:
		pc->emulink.status = 0xaa55ff00;
		pc->emulink.cmd    = -1;
		break;
	case 0x100:
		pc->emulink.status = fdds_types();
		pc->emulink.cmd = -1;
		/*
		 * This query is the BIOS's floppy setup: the SeaBIOS here reads
		 * diskettes through emulink and never touches the controller, so
		 * it never does the other half of what floppy_setup() does on a
		 * real BIOS - enable_hwirq(6).  IRQ 6 stayed masked into the OS,
		 * and Windows 95's floppy driver, which drives the FDC itself
		 * and counts on the BIOS having opened the line, reset the
		 * controller on every access to A: and waited out a timeout for
		 * an interrupt the PIC was holding back.  So open it here, where
		 * the BIOS would have.
		 */
		if (pc->emulink.status) {
			const uint32_t imr = i8259_ioport_read(pc->pic, 0x21);
			if (imr & 0x40) {
				i8259_ioport_write(pc->pic, 0x21, imr & ~0x40u);
				io_trace_note('w', 0x21, (int)(imr & ~0x40u));
			}
		}
		break;
	case 0x101: /* read */
	case 0x102: /* write */
		if (pc->emulink.argi == 3) {
			uint8_t  drv  = (uint8_t)pc->emulink.args[0];
			uint32_t chs  = pc->emulink.args[1];
			uint32_t cnt  = pc->emulink.args[2];
			if (drv >= 2 || !fdd_is_inserted(drv)) {
				pc->emulink.status = 0x80; /* error */
				pc->emulink.cmd    = -1;
				break;
			}
			int c = (int)(chs >> 16);
			int h = (int)((chs >> 8) & 0xff);
			int s = (int)(chs & 0xff);
			uint16_t heads = fdd_get_heads(drv);
			uint16_t sects = fdd_get_sects(drv);
			if (heads == 0 || sects == 0) {
				pc->emulink.status = 0x80;
				pc->emulink.cmd    = -1;
				break;
			}
			uint32_t lba = (uint32_t)(c * heads + h) * sects + (uint32_t)(s - 1);
			BlockDev *dev = fdd_get_dev(drv);
			if (blk_seek(dev, (uint64_t)lba * 512u) < 0) {
				pc->emulink.status = 0x80;
				pc->emulink.cmd    = -1;
			} else {
				pc->emulink.status    = 0;
				pc->emulink.dataleft  = (int)(cnt * 512u);
			}
		}
		break;
	default:
		break;
	}
}

static uint32_t emulink_read32(PC *pc)
{
	return pc->emulink.status;
}

static void emulink_cmd_write(PC *pc, uint32_t val)
{
	pc->emulink.cmd  = (int)val;
	pc->emulink.argi = 0;
	emulink_exec(pc);
}

static void emulink_arg_write(PC *pc, uint32_t val)
{
	if (pc->emulink.argi < 4)
		pc->emulink.args[pc->emulink.argi++] = val;
	emulink_exec(pc);
}

/* bulk read: called from pc_io_read_string for port 0xF1F4 */
static int emulink_data_read(PC *pc, uint8_t *buf, int size, int count)
{
	if (pc->emulink.cmd == 0x101 && pc->emulink.argi == 3) {
		uint8_t drv = (uint8_t)pc->emulink.args[0];
		if (!fdd_is_inserted(drv)) goto err;
		int len = size * count;
		if (len > pc->emulink.dataleft) goto err;
		BlockDev *dev = fdd_get_dev(drv);
		if (blk_read(dev, buf, (unsigned)len) != len) goto err;
		pc->emulink.dataleft -= len;
		if (pc->emulink.dataleft == 0) {
			pc->emulink.cmd    = -1;
			pc->emulink.status = 0;
		}
		return count;
	}
err:
	pc->emulink.cmd    = -1;
	pc->emulink.status = 0x80;
	return count;
}

/* bulk write: called from pc_io_write_string for port 0xF1F4 */
static int emulink_data_write(PC *pc, uint8_t *buf, int size, int count)
{
	if (pc->emulink.cmd == 0x102 && pc->emulink.argi == 3) {
		uint8_t drv = (uint8_t)pc->emulink.args[0];
		if (!fdd_is_inserted(drv)) goto err;
		int len = size * count;
		if (len > pc->emulink.dataleft) goto err;
		BlockDev *dev = fdd_get_dev(drv);
		if (blk_write(dev, buf, (unsigned)len) != len) goto err;
		/* The BIOS's own floppy write ends here, so this is where what it
		 * wrote has to be on the media rather than in a buffer. */
		blk_sync(dev);
		pc->emulink.dataleft -= len;
		if (pc->emulink.dataleft == 0) {
			pc->emulink.cmd    = -1;
			pc->emulink.status = 0;
		}
		return count;
	}
err:
	pc->emulink.cmd    = -1;
	pc->emulink.status = 0x80;
	return count;
}

/* --------------------------------------------------------------------------*/
#if TRACE_PORTS
static FIL ports_log;
#include <stdarg.h>
void debug_write(const char *fmt, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;
	UINT bw;
	f_write(&ports_log, buf, n, &bw);
	f_sync(&ports_log);
}
#else
#define debug_write(...) (void)0
#endif

static __always_inline u8 _pc_io_read(void *o, int addr)
{
	PC *pc = o;
	u8 val;

	/* Before the switch: this is one guest instruction in six while a game
	 * waits for the frame to end, and nothing else comes close. */
	if (addr == 0x3da)
		return (u8)vga_status1_read(pc->vga);

	switch(addr) {
	case 0x20: case 0x21: case 0xa0: case 0xa1:
		val = i8259_ioport_read(pc->pic, addr);
		return val;
	case 0x3f8: case 0x3f9: case 0x3fa: case 0x3fb:
	case 0x3fc: case 0x3fd: case 0x3fe: case 0x3ff:
		val = 0xff;
		if (pc->enable_serial)
			val = u8250_reg_read(pc->serial, addr - 0x3f8);
		return val;
	case 0x2f8: case 0x2f9: case 0x2fa: case 0x2fb:
	case 0x2fc: case 0x2fd: case 0x2fe: case 0x2ff:
	case 0x2e8: case 0x2e9: case 0x2ea: case 0x2eb:
	case 0x2ec: case 0x2ed: case 0x2ee: case 0x2ef:
	case 0x3e8: case 0x3e9: case 0x3ea: case 0x3eb:
	case 0x3ec: case 0x3ed: case 0x3ee: case 0x3ef:
		return 0;
	case 0x42:
		/* read delay for PIT channel 2 */
		/* certain guest code needs it to drive pc speaker properly */
		usleep(0);
		/* fall through */
	case 0x40: case 0x41: case 0x43:
		val = i8254_ioport_read(pc->pit, addr);
		return val;
	case 0x70: case 0x71:
		val = cmos_ioport_read(pc->cmos, addr);
		return val;
	case 0x1f0: case 0x1f1: case 0x1f2: case 0x1f3:
	case 0x1f4: case 0x1f5: case 0x1f6: case 0x1f7:
		return ide_ioport_read(pc->ide, addr - 0x1f0);
	case 0x170: case 0x171: case 0x172: case 0x173:
	case 0x174: case 0x175: case 0x176: case 0x177:
		return ide_ioport_read(pc->ide2, addr - 0x170);
	case 0x3f6:
		return ide_status_read(pc->ide);
	case 0x376:
		return ide_status_read(pc->ide2);
	/* FDC ports 0x3F0-0x3F5, 0x3F7 (0x3F6 = IDE alt-status, handled above) */
	case 0x3f0: case 0x3f1: case 0x3f2: case 0x3f3:
	case 0x3f4: case 0x3f5:
	case 0x3f7:
		if (pc->fdc)
			return fdc_ioport_read(pc->fdc, addr);
		return 0xff;
	case 0x3c0: case 0x3c1: case 0x3c2: case 0x3c3:
	case 0x3c4: case 0x3c5: case 0x3c6: case 0x3c7:
	case 0x3c8: case 0x3c9: case 0x3ca: case 0x3cb:
	case 0x3cc: case 0x3cd: case 0x3ce: case 0x3cf:
	case 0x3d0: case 0x3d1: case 0x3d2: case 0x3d3:
	case 0x3d4: case 0x3d5: case 0x3d6: case 0x3d7:
	case 0x3d8: case 0x3d9: case 0x3da: case 0x3db:
	case 0x3dc: case 0x3dd: case 0x3de: case 0x3df:
		val = vga_ioport_read(pc->vga, addr);
		return val;
	case 0x92:
		return pc->port92;
	case 0x60:
		val = kbd_read_data(pc->i8042, addr);
		return val;
	case 0x64:
		val = kbd_read_status(pc->i8042, addr);
		return val;
	case 0x61:
		val = pcspk_ioport_read(pc->pcspk);
		return val;
	case 0x220: case 0x221: case 0x222: case 0x223:
	case 0x228: case 0x229:
	case 0x388: case 0x389: case 0x38a: case 0x38b:
		if (pc->adlib_enabled)
			return adlib_read(pc->adlib, addr);
		return 0xFF;
	case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff:
		val = i440fx_read_data(pc->i440fx, addr - 0xcfc, 0);
		return val;
	/* The network card, when the host has an adapter to put behind it. */
	case 0x300: case 0x301: case 0x302: case 0x303:
	case 0x304: case 0x305: case 0x306: case 0x307:
	case 0x308: case 0x309: case 0x30a: case 0x30b:
	case 0x30c: case 0x30d: case 0x30e: case 0x30f:
		if (!pc->ne2000) return 0xff;
		return ne2000_ioport_read(pc->ne2000, addr);
	case 0x310:
		if (!pc->ne2000) return 0xff;
		return ne2000_asic_ioport_read(pc->ne2000, addr);
	case 0x31f:
		if (!pc->ne2000) return 0xff;
		return ne2000_reset_ioport_read(pc->ne2000, addr);
	/*
	 * 0x10-0x1f is the 8237's own alias of 0x00-0x0f.  The chip has four
	 * address inputs and its chip select is decoded from the high bits being
	 * zero, so on a real PC every port from 0x00 to 0x1f selects DMA
	 * controller 1 - and drivers use that.  Supaplex's BLASTER.SND reads the
	 * channel 1 current-count register at 0x13 and waits for it to move;
	 * measured at forty thousand reads inside the window where the game
	 * fails.  Undecoded, those reads answered with the bus default, the wait
	 * never finished, and the driver masked the channel, uninstalled its
	 * interrupt handler and restarted the application.
	 */
	case 0x00: case 0x01: case 0x02: case 0x03:
	case 0x04: case 0x05: case 0x06: case 0x07:
	case 0x10: case 0x11: case 0x12: case 0x13:
	case 0x14: case 0x15: case 0x16: case 0x17:
		val = i8257_read_chan(pc->isa_dma, addr & 0x07, 1);
		return val;
	case 0xf1f4: {
		/* emulink: single-byte read (BIOS probes this way too) */
		uint32_t v32 = emulink_read32(pc);
		return (u8)(v32 & 0xff);
	}
	case 0x08: case 0x09: case 0x0a: case 0x0b:
	case 0x0c: case 0x0d: case 0x0e: case 0x0f:
	case 0x18: case 0x19: case 0x1a: case 0x1b:
	case 0x1c: case 0x1d: case 0x1e: case 0x1f:
		val = i8257_read_cont(pc->isa_dma, addr & 0x07, 1);
		/* Who is reading the status register, and from where.  Reading
		 * it clears the terminal-count bits, so two reads of the same
		 * instruction would hand the second one a zero - which is what
		 * Sierra's card detection ends up seeing. */
		if ((addr & 0x07) == 0) {
			uint32_t dcs, dip; int dhalt;
			cpui386_get_state(pc->cpu, &dcs, &dip, &dhalt);
			i8257_diag_note_reader(dcs, dip, (uint32_t)val);
		}
		return val;
	case 0x81: case 0x82: case 0x83: case 0x87:
		val = i8257_read_page(pc->isa_dma, addr - 0x80);
		return val;
	case 0x481: case 0x482: case 0x483: case 0x487:
		val = i8257_read_pageh(pc->isa_dma, addr - 0x480);
		return val;
	case 0xc0: case 0xc1:
		/* SN76489 is write-only; return 0xFF on read when Tandy active */
		if (pc->tandy_enabled)
			return 0xff;
		val = i8257_read_chan(pc->isa_hdma, addr - 0xc0, 1);
		return val;
	case 0xc2: case 0xc4: case 0xc6:
	case 0xc8: case 0xca: case 0xcc: case 0xce:
		val = i8257_read_chan(pc->isa_hdma, addr - 0xc0, 1);
		return val;
	case 0xd0: case 0xd2: case 0xd4: case 0xd6:
	case 0xd8: case 0xda: case 0xdc: case 0xde:
		val = i8257_read_cont(pc->isa_hdma, addr - 0xd0, 1);
		return val;
	case 0x89: case 0x8a: case 0x8b: case 0x8f:
		val = i8257_read_page(pc->isa_hdma, addr - 0x88);
		return val;
	case 0x489: case 0x48a: case 0x48b: case 0x48f:
		val = i8257_read_pageh(pc->isa_hdma, addr - 0x488);
		return val;
	case 0x225:
		if (pc->sb16_enabled) {
			return sb16_mixer_read(pc->sb16, addr);
		}
		return 0xFF;
	case 0x226: case 0x22a: case 0x22c: case 0x22d: case 0x22e: case 0x22f:
		g_sb_port_reads++;
		if (pc->sb16_enabled) {
			return sb16_dsp_read(pc->sb16, addr);
		}
		return 0xFF;
	case 0x200: case 0x201: case 0x202: case 0x203:
	case 0x204: case 0x205: case 0x206: case 0x207:
		/* Analog game port. When no joystick is configured this returns
		 * 0xF0 - axes timed out, no buttons - which is exactly what an
		 * empty adapter reads, so probing games behave as before. */
		if (pc->joystick_enabled)
			return gameport_read();
		return 0xf0;
	case 0x27A: // Covox Speech Thing
		return 0;
	// MPU-401
	case 0x330:
	case 0x331:
		if (pc->mpu401_enabled)
	        return mpu401_read(addr);
		return 0xFF;
	// Disney Sound Source
	case 0x378:
	case 0x379:
		if (pc->dss_enabled)
			return dss_in(addr);
		return 0xFF;
	/* LPT data ports (Covox): write-only DAC, reads return 0xFF */
	case 0x278:
		return 0xff;
	/* LPT status ports: bit7=nBusy(1=ready), bits6..3=1 (idle/ready) */
	case 0x279:
		return 0xf8;
	/* LPT control ports */
	case 0x37a:
		return 0x04;
	default:
		//fprintf(stderr, "in 0x%x <= 0x%x\n", addr, 0xff);
		return 0xff;
	}
}

/* The one port worth naming; see io_fast_port in i386.h. */
static u8 pc_in_status1(void *o)
{
	return (u8)vga_status1_read(((PC *)o)->vga);
}

/*
 * The floppy controller's conversation, kept for the exception dump.
 *
 * Windows 95 waits on A: with IRQ 6 raised by the controller and masked at
 * the PIC, and nothing else says who asked for what: every access to the
 * FDC, the PIC masks and DMA channel 2 is noted with the instruction that
 * made it, and the controller's own IRQ edges go in the same ring.  A read
 * that repeats - a driver polling the status register - bumps a count.
 */
#define IOT_N 128
struct IoTrace {
	uint32_t t_us, eip, hits;
	uint16_t port, cs;
	uint8_t  kind, val;          /* kind: 'r' 'w' or 'i' (irq6 level) */
};
static struct IoTrace iot_ring[IOT_N];
static uint32_t iot_head;
static PC *iot_pc;

void io_trace_note(int kind, int port, int val)
{
	extern uint32_t g_exc_frozen;
	if (g_exc_frozen) return;
	CPUI386 *cpu = iot_pc ? iot_pc->cpu : NULL;
	const uint16_t cs = cpu ? (uint16_t)cpu->seg[SEG_CS].sel : 0;
	const uint32_t eip = cpu ? (uint32_t)cpu->ip : 0;
	struct IoTrace *last = iot_head ? &iot_ring[(iot_head - 1) % IOT_N] : NULL;
	if (last && last->kind == kind && last->port == port &&
	    last->val == (uint8_t)val && last->eip == eip && last->cs == cs) {
		last->hits++;
		last->t_us = time_us_32();
		return;
	}
	struct IoTrace *e = &iot_ring[iot_head++ % IOT_N];
	e->t_us = time_us_32(); e->eip = eip; e->hits = 1;
	e->port = (uint16_t)port; e->cs = cs;
	e->kind = (uint8_t)kind; e->val = (uint8_t)val;
}

int io_trace_line(int idx, char *out, int cap)
{
	const uint32_t held = iot_head < IOT_N ? iot_head : IOT_N;
	if ((uint32_t)idx >= held) return 0;
	const uint32_t first = iot_head < IOT_N ? 0 : iot_head % IOT_N;
	const struct IoTrace *e = &iot_ring[(first + idx) % IOT_N];
	return snprintf(out, cap, "%lu.%06lus x%lu %c %03x %02x at %04x:%08lx",
		(unsigned long)(e->t_us / 1000000u),
		(unsigned long)(e->t_us % 1000000u),
		(unsigned long)e->hits, e->kind, e->port, e->val,
		e->cs, (unsigned long)e->eip);
}

static inline int iot_port(int addr)
{
	return (addr >= 0x3f0 && addr <= 0x3f7 && addr != 0x3f6) ||
	       addr == 0x04 || addr == 0x05 || addr == 0x81 ||
	       addr == 0x0a || addr == 0x0b || addr == 0x0c || addr == 0x08;
}

/* The master mask is rewritten around every timer tick; only a change of
 * the floppy's bit is worth a slot. */
static uint8_t iot_last_imr = 0xff;
static inline void iot_imr(PC *pc, int addr, u8 val)
{
	if (addr != 0x21 || !((val ^ iot_last_imr) & 0x40)) {
		if (addr == 0x21) iot_last_imr = val;
		return;
	}
	iot_last_imr = val;
	iot_pc = pc;
	io_trace_note('w', addr, val);
}

static u8 pc_io_read(void *o, int addr) {
	frank_diag_port((uint32_t)addr, 0);
	u8 r = _pc_io_read(o, addr);
	if (__builtin_expect(iot_port(addr), 0)) { iot_pc = o; io_trace_note('r', addr, r); }
	CIRCLE_PC_IO_NOTE(0, 1, 0, addr, r, 0);
	debug_write("R8: %ph <- %02Xh\n", addr, r);
	return r;
}

static __always_inline u16 _pc_io_read16(void *o, int addr)
{
	PC *pc = o;
	u16 val;

	switch(addr) {
	case 0x1ce: case 0x1cf:
		val = vbe_read(pc->vga, addr - 0x1ce);
		return val;
	/* IDE ports */
	case 0x1f0:
		return ide_data_readw(pc->ide);
	case 0x170:
		return ide_data_readw(pc->ide2);
	case 0xcf8:
		val = i440fx_read_addr(pc->i440fx, 0, 1);
		return val;
	case 0xcfc: case 0xcfe:
		val = i440fx_read_data(pc->i440fx, addr - 0xcfc, 1);
		return val;
	/* The card's data port, which a driver reads sixteen bits at a time. */
	case 0x310:
		if (!pc->ne2000) return 0xffff;
		return ne2000_asic_ioport_read(pc->ne2000, addr);
	case 0x220:
		if (pc->adlib_enabled)
			return adlib_read(pc->adlib, addr);
		return 0xFFFF;
	/* Game port. Games normally use IN AL, but a 16-bit read must not
	 * silently return 0 - that reads as "axes already timed out" and the
	 * stick looks stuck at one extreme. */
	case 0x200: case 0x201: case 0x202: case 0x203:
	case 0x204: case 0x205: case 0x206: case 0x207:
		if (pc->joystick_enabled)
			return 0xff00u | gameport_read();
		return 0xfff0u;
	default:
		/*
		 * An 8-bit device answers a word cycle on the ISA bus with two
		 * byte cycles, on addr and addr+1 - the write path below already
		 * splits that way.  Returning 0 was worse than merely wrong: an
		 * unclaimed bus reads 0xff, and a value that never changes hangs
		 * any loop waiting for a port to move.  Skunny times itself with
		 * exactly such a loop - IN AX, 0x40 twice, spin while the two are
		 * equal - and waited forever on a counter frozen at zero.
		 */
		return (u16)pc_io_read(o, addr) |
		       ((u16)pc_io_read(o, (addr + 1) & 0xffff) << 8);
	}
}

static u16 pc_io_read16(void *o, int addr) {
	u16 r = _pc_io_read16(o, addr);
	CIRCLE_PC_IO_NOTE(0, 2, 0, addr, r, 0);
	debug_write("R16: %ph <- %04Xh\n", addr, r);
	return r;
}

static __always_inline u32 _pc_io_read32(void *o, int addr)
{
	PC *pc = o;
	u32 val;
	switch(addr) {
	/* IDE ports */
	case 0x1f0:
		return ide_data_readl(pc->ide);
	case 0x170:
		return ide_data_readl(pc->ide2);
	case 0x3cc:
		return (get_uticks() - pc->boot_start_time) / 1000;
	case 0xcf8:
		val = i440fx_read_addr(pc->i440fx, 0, 2);
		return val;
	case 0xcfc:
		val = i440fx_read_data(pc->i440fx, 0, 2);
		return val;
	/* Emulink FDD status port */
	case 0xf1f0:
		return emulink_read32(pc);
	default:
		/* The same split one level up: two word cycles. */
		return (u32)pc_io_read16(o, addr) |
		       ((u32)pc_io_read16(o, (addr + 2) & 0xffff) << 16);
	}
}

static u32 pc_io_read32(void *o, int addr) {
	frank_diag_port((uint32_t)addr, 0);
	u32 r = _pc_io_read32(o, addr);
	CIRCLE_PC_IO_NOTE(0, 4, 0, addr, r, 0);
	debug_write("R32: %ph <- %08Xh\n", addr, r);
	return r;
}

static int pc_io_read_string(void *o, int addr, uint8_t *buf, int size, int count)
{
	debug_write("RS: %ph [%d / %d]\n", addr, size, count);
	PC *pc = o;
	int completed;
	switch(addr) {
	case 0x1f0:
		completed = ide_data_read_string(pc->ide, buf, size, count);
		break;
	case 0x170:
		completed = ide_data_read_string(pc->ide2, buf, size, count);
		break;
	case 0xf1f4:
		completed = emulink_data_read(pc, buf, size, count);
		break;
	default:
		completed = 0;
		break;
	}
	CIRCLE_PC_IO_NOTE(0, size, 1, addr, 0, completed);
	return completed;
}

#if EMULATE_LTEMS
uint8_t ems_pages[4] = {0};

inline static void out_ems(const uint16_t port, const uint8_t data) {
    ems_pages[port & 3] = data;
}
#endif

static void pc_io_write_impl(void *o, int addr, u8 val)
{
	frank_diag_port((uint32_t)addr, 1);
	debug_write("W8: %ph -> %02Xh\n", addr, val);
	PC *pc = o;
	if (__builtin_expect(iot_port(addr), 0)) { iot_pc = pc; io_trace_note('w', addr, val); }
	if (addr == 0x21) iot_imr(pc, addr, val);
	switch(addr) {
	case 0x80: case 0xed:
		/* used by linux, for io delay */
		return;
	case 0x20: case 0x21: case 0xa0: case 0xa1:
		i8259_ioport_write(pc->pic, addr, val);
		return;
	case 0x3f8: case 0x3f9: case 0x3fa: case 0x3fb:
	case 0x3fc: case 0x3fd: case 0x3fe: case 0x3ff:
		u8250_reg_write(pc->serial, addr - 0x3f8, val);
		return;
#if EMULATE_LTEMS
    case 0x260: case 0x261: case 0x262: case 0x263:
		out_ems(addr, val);
        return;
#endif
	case 0x2f8: case 0x2f9: case 0x2fa: case 0x2fb:
	case 0x2fc: case 0x2fd: case 0x2fe: case 0x2ff:
	case 0x2e8: case 0x2e9: case 0x2ea: case 0x2eb:
	case 0x2ec: case 0x2ed: case 0x2ee: case 0x2ef:
	case 0x3e8: case 0x3e9: case 0x3ea: case 0x3eb:
	case 0x3ec: case 0x3ed: case 0x3ee: case 0x3ef:
		return;
	case 0x40: case 0x41: case 0x42: case 0x43:
		i8254_ioport_write(pc->pit, addr, val);
		return;
	case 0x70: case 0x71:
		cmos_ioport_write(pc->cmos, addr, val);
		return;
	/* IDE ports */
	case 0x1f0: case 0x1f1: case 0x1f2: case 0x1f3:
	case 0x1f4: case 0x1f5: case 0x1f6: case 0x1f7:
		ide_ioport_write(pc->ide, addr - 0x1f0, val);
		return;
	case 0x170: case 0x171: case 0x172: case 0x173:
	case 0x174: case 0x175: case 0x176: case 0x177:
		ide_ioport_write(pc->ide2, addr - 0x170, val);
		return;
	case 0x3f6:
		ide_cmd_write(pc->ide, val);
		return;
	case 0x376:
		ide_cmd_write(pc->ide2, val);
		return;
	/* FDC ports 0x3F0-0x3F5, 0x3F7 */
	case 0x3f0: case 0x3f1: case 0x3f2: case 0x3f3:
	case 0x3f4: case 0x3f5:
	case 0x3f7:
		if (pc->fdc)
			fdc_ioport_write(pc->fdc, addr, val);
		return;
	case 0x3c0: case 0x3c1: case 0x3c2: case 0x3c3:
	case 0x3c4: case 0x3c5: case 0x3c6: case 0x3c7:
	case 0x3c8: case 0x3c9: case 0x3ca: case 0x3cb:
	case 0x3cc: case 0x3cd: case 0x3ce: case 0x3cf:
	case 0x3d0: case 0x3d1: case 0x3d2: case 0x3d3:
	case 0x3d4: case 0x3d5: case 0x3d6: case 0x3d7:
	case 0x3d8: case 0x3d9: case 0x3da: case 0x3db:
	case 0x3dc: case 0x3dd: case 0x3de: case 0x3df:
		vga_ioport_write(pc->vga, addr, val);
		return;
	case 0x402:
		return;
	case 0x92:
		pc->port92 = val;
		cpu_set_a20(pc->cpu, (val >> 1) & 1);
		return;
	case 0x60:
		kbd_write_data(pc->i8042, addr, val);
		return;
	case 0x64:
		kbd_write_command(pc->i8042, addr, val);
		return;
	case 0x61:
		FRANK_DIAG_COUNT(port61);
		pcspk_ioport_write(pc->pcspk, val);
		return;
	case 0x220: case 0x221: case 0x222: case 0x223:
	case 0x228: case 0x229:
	case 0x388: case 0x389: case 0x38a: case 0x38b:
		if (pc->adlib_enabled)
			adlib_write(pc->adlib, addr, val);
		return;
	case 0x8900:
		switch (val) {
		case 'S': if (pc->shutdown_state == 0) pc->shutdown_state = 1; break;
		case 'h': if (pc->shutdown_state == 1) pc->shutdown_state = 2; break;
		case 'u': if (pc->shutdown_state == 2) pc->shutdown_state = 3; break;
		case 't': if (pc->shutdown_state == 3) pc->shutdown_state = 4; break;
		case 'd': if (pc->shutdown_state == 4) pc->shutdown_state = 5; break;
		case 'o': if (pc->shutdown_state == 5) pc->shutdown_state = 6; break;
		case 'w': if (pc->shutdown_state == 6) pc->shutdown_state = 7; break;
		case 'n': if (pc->shutdown_state == 7) pc->shutdown_state = 8; break;
		default : pc->shutdown_state = 0; break;
		}
		return;
	case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff:
		i440fx_write_data(pc->i440fx, addr - 0xcfc, val, 0);
		return;
	case 0x300: case 0x301: case 0x302: case 0x303:
	case 0x304: case 0x305: case 0x306: case 0x307:
	case 0x308: case 0x309: case 0x30a: case 0x30b:
	case 0x30c: case 0x30d: case 0x30e: case 0x30f:
		if (pc->ne2000) ne2000_ioport_write(pc->ne2000, addr, val);
		return;
	case 0x310:
		if (pc->ne2000) ne2000_asic_ioport_write(pc->ne2000, addr, val);
		return;
	case 0x31f:
		if (pc->ne2000) ne2000_reset_ioport_write(pc->ne2000, addr, val);
		return;
	case 0x00: case 0x01: case 0x02: case 0x03:
	case 0x04: case 0x05: case 0x06: case 0x07:
	case 0x10: case 0x11: case 0x12: case 0x13:
	case 0x14: case 0x15: case 0x16: case 0x17:
		i8257_write_chan(pc->isa_dma, addr & 0x07, val, 1);
		return;
	case 0x08: case 0x09: case 0x0a: case 0x0b:
	case 0x0c: case 0x0d: case 0x0e: case 0x0f:
	case 0x18: case 0x19: case 0x1a: case 0x1b:
	case 0x1c: case 0x1d: case 0x1e: case 0x1f:
		i8257_write_cont(pc->isa_dma, addr & 0x07, val, 1);
		return;
	case 0x81: case 0x82: case 0x83: case 0x87:
		i8257_write_page(pc->isa_dma, addr - 0x80, val);
		return;
	case 0x481: case 0x482: case 0x483: case 0x487:
		i8257_write_pageh(pc->isa_dma, addr - 0x480, val);
		return;
	/* 0xC0/0xC1: SN76489 data port when Tandy enabled, hdma ch0 otherwise.
	 * 0xC2-0xCE: always hdma (Tandy only occupied 0xC0). */
	case 0xc0: case 0xc1:
		if (pc->tandy_enabled)
			sn76489_out(val);
		else
			i8257_write_chan(pc->isa_hdma, addr - 0xc0, val, 1);
		return;
	case 0xc2: case 0xc4: case 0xc6:
	case 0xc8: case 0xca: case 0xcc: case 0xce:
		i8257_write_chan(pc->isa_hdma, addr - 0xc0, val, 1);
		return;
	case 0xd0: case 0xd2: case 0xd4: case 0xd6:
	case 0xd8: case 0xda: case 0xdc: case 0xde:
		i8257_write_cont(pc->isa_hdma, addr - 0xd0, val, 1);
		return;
	case 0x89: case 0x8a: case 0x8b: case 0x8f:
		i8257_write_page(pc->isa_hdma, addr - 0x88, val);
		return;
	case 0x489: case 0x48a: case 0x48b: case 0x48f:
		i8257_write_pageh(pc->isa_hdma, addr - 0x488, val);
		return;
	case 0x224:
		if (pc->sb16_enabled) {
			sb16_mixer_write_indexb(pc->sb16, addr, val);
		}
		return;
	case 0x225:
		if (pc->sb16_enabled) {
			sb16_mixer_write_datab(pc->sb16, addr, val);
		}
		return;
	case 0x226: case 0x22c:
		FRANK_DIAG_COUNT(sb_dsp);
		g_sb_port_writes++;
		if (pc->sb16_enabled) {
			sb16_dsp_write(pc->sb16, addr, val);
		}
		return;
	/* Tandy 3-Voice Sound (SN76489) - additional alias ports.
	 * Primary port 0xC0 is handled above.
	 * 0x1E0: Tandy 1000 SX/TX/HX data port
	 * 0x2C0: Tandy 1000 A/B mirror */
	case 0x1E0: case 0x2C0:
		if (pc->tandy_enabled)
			sn76489_out(val);
		return;
	/* Covox Speech Thing (parallel port DAC)
	 * 0x278 = LPT2 data. */
	case 0x278:
		FRANK_DIAG_COUNT(covox);
		if (pc->covox_enabled)
			pc->covox_sample = val;
		return;
	// MPU-401
	case 0x330:
	case 0x331:
		if (pc->mpu401_enabled)
			mpu401_write(addr, val);
		return;
	// Disney Sound Source
    case 0x378:
    case 0x37A:
		if (pc->dss_enabled)
			dss_out(addr, val);
		return;
	/* Game port: any write fires the axis one-shots. The value written
	 * is irrelevant on real hardware and is ignored here too. */
	case 0x200: case 0x201: case 0x202: case 0x203:
	case 0x204: case 0x205: case 0x206: case 0x207:
		if (pc->joystick_enabled)
			gameport_write();
		return;
	/* LPT status/control ports are read-only - writes ignored */
	case 0x379: case 0x279:
	case 0x27a:
		return;
	default:
///		fprintf(stderr, "out 0x%x => 0x%x\n", val, addr);
		return;
	}
}

static void pc_io_write(void *o, int addr, u8 val)
{
	pc_io_write_impl(o, addr, val);
	/* Record after dispatch so a nested byte transaction from an ISA-wide
	 * fallback cannot replace the externally visible write. */
	CIRCLE_PC_IO_NOTE(1, 1, 0, addr, val, 0);
}

static void pc_io_write16_impl(void *o, int addr, u16 val)
{
	frank_diag_port((uint32_t)addr, 1);
	debug_write("W16: %ph -> %04Xh\n", addr, val);
	PC *pc = o;
	switch(addr) {
	/* IDE ports */
	case 0x1f0:
		ide_data_writew(pc->ide, val);
		return;
	case 0x170:
		ide_data_writew(pc->ide2, val);
		return;
	/* A common AdLib fast path packs the register number in AL and its
	 * value in AH, then uses one OUT DX,AX.  The YM3812 is an 8-bit device,
	 * so the ISA word cycle addresses its adjacent index/data ports. */
	case 0x220: case 0x222: case 0x228:
	case 0x388: case 0x38a:
		pc_io_write(o, addr, (uint8_t)val);
		pc_io_write(o, addr + 1, (uint8_t)(val >> 8));
		return;
    case 0x260: case 0x261: case 0x262: case 0x263:
		pc_io_write(o, addr, (uint8_t) val);
		pc_io_write(o, addr + 1, val >> 8);
        return;
	case 0x3c0: case 0x3c1: case 0x3c2: case 0x3c3:
	case 0x3c4: case 0x3c5: case 0x3c6: case 0x3c7:
	case 0x3c8: case 0x3c9: case 0x3ca: case 0x3cb:
	case 0x3cc: case 0x3cd: case 0x3ce: case 0x3cf:
	case 0x3d0: case 0x3d1: case 0x3d2: case 0x3d3:
	case 0x3d4: case 0x3d5: case 0x3d6: case 0x3d7:
	case 0x3d8: case 0x3d9: case 0x3da: case 0x3db:
	case 0x3dc: case 0x3dd: case 0x3de:
		vga_ioport_write(pc->vga, addr, val & 0xff);
		vga_ioport_write(pc->vga, addr + 1, (val >> 8) & 0xff);
		return;
	case 0x1ce: case 0x1cf:
		vbe_write(pc->vga, addr - 0x1ce, val);
		return;
	case 0xcfc: case 0xcfe:
		i440fx_write_data(pc->i440fx, addr - 0xcfc, val, 1);
		return;
	case 0x310:
		if (pc->ne2000) ne2000_asic_ioport_write(pc->ne2000, addr, val);
		return;
	default:
///		fprintf(stderr, "outw 0x%x => 0x%x\n", val, addr);
		break;
	}
}

static void pc_io_write16(void *o, int addr, u16 val)
{
	pc_io_write16_impl(o, addr, val);
	CIRCLE_PC_IO_NOTE(1, 2, 0, addr, val, 0);
}

static void pc_io_write32_impl(void *o, int addr, u32 val)
{
	frank_diag_port((uint32_t)addr, 1);
	debug_write("W32: %ph -> %08Xh\n", addr, val);
	PC *pc = o;
	switch(addr) {
	/* IDE ports */
	case 0x1f0:
		ide_data_writel(pc->ide, val);
		return;
	case 0x170:
		ide_data_writel(pc->ide2, val);
		return;
	case 0xcf8:
		i440fx_write_addr(pc->i440fx, 0, val, 2);
		return;
	case 0xcfc:
		i440fx_write_data(pc->i440fx, 0, val, 2);
		return;
	/* Emulink FDD command/data ports */
	case 0xf1f0:
		emulink_cmd_write(pc, val);
		return;
	case 0xf1f4:
		emulink_arg_write(pc, val);
		return;
    case 0x260: case 0x261: case 0x262: case 0x263:
		pc_io_write16(o, addr, (uint16_t) val);
		pc_io_write16(o, addr + 2, val >> 16);
        return;
	default:
///		do_log(stderr, "outd 0x%x => 0x%x\n", val, addr);
		break;
	}
}

static void pc_io_write32(void *o, int addr, u32 val)
{
	pc_io_write32_impl(o, addr, val);
	CIRCLE_PC_IO_NOTE(1, 4, 0, addr, val, 0);
}

static int pc_io_write_string(void *o, int addr, uint8_t *buf, int size, int count)
{
	debug_write("WS: %ph [%d / %d]\n", addr, size, count);
	PC *pc = o;
	int completed;
	switch(addr) {
	case 0x1f0:
		completed = ide_data_write_string(pc->ide, buf, size, count);
		break;
	case 0x170:
		completed = ide_data_write_string(pc->ide2, buf, size, count);
		break;
	case 0xf1f4:
		completed = emulink_data_write(pc, buf, size, count);
		break;
	default:
		completed = 0;
		break;
	}
	CIRCLE_PC_IO_NOTE(1, size, 1, addr, 0, completed);
	return completed;
}

#if defined(CIRCLE_BUILD)
/*
 * The display, drawn on a core of its own.
 *
 * vga_refresh() converts the guest's video memory into the framebuffer at
 * every retrace, and it ran inside pc_step(): 4.8% of core 0 in Tyrian's
 * demo, and far more whenever a redraw is heavy - the planar path once took
 * 8.6 ms a frame, and for all of that time nothing polled the timer and the
 * guest lost IRQ0 ticks.  Core 0 now only asks, and core 2 draws.
 * Measured: Doom II -timedemo demo1 went from 15.03 to 15.9 fps (two clean
 * boots each), and Tyrian's lost timer ticks stopped piling up.  A game
 * that paces itself on the retrace, like Tyrian, is not faster: it spends
 * the time it is given waiting for the next one.
 *
 * It draws while the guest runs, so a frame can show a write the guest made
 * half-way through it - which is what a real card scanning out of the same
 * memory shows too.  Everything the renderer sizes its loops by is read
 * once into locals and its reads of video memory are bounded as they were;
 * the fields it writes (the last_* caches, the VBE row shadow, the cursor
 * phase) are its own.  The exceptions are held off with pc_vga_hold(): the
 * menus, which swap the surface the redraw copies from, and a change of
 * video memory size.
 */
static PC *g_vga_pc;
static uint32_t g_vga_req;      /* bit 0 wanted, bit 1 in full */
static uint32_t g_vga_busy;     /* core 2 is inside vga_refresh() */
static uint32_t g_vga_hold;     /* core 0 wants it idle */
#if defined(CIRCLE_PC_STATS)
/* What each helper core did, for the STATS line: loop turns, sleeps, time
 * asleep in us, and turns that found work.  Indexed by core. */
uint32_t g_core_iter[4], g_core_wfe[4], g_core_wfe_us[4], g_core_work[4];
#define CORE_WFE(c) do { const uint32_t t_ = get_uticks(); g_core_wfe[c]++; \
	__asm__ volatile ("wfe"); g_core_wfe_us[c] += get_uticks() - t_; } while (0)
#else
#define CORE_WFE(c) __asm__ volatile ("wfe")
#endif

void pc_vga_request(int full)
{
	__atomic_or_fetch(&g_vga_req, 1u | (full ? 2u : 0u), __ATOMIC_RELEASE);
	__asm__ volatile ("dsb ish\n\tsev" ::: "memory");
}

void pc_vga_hold(int on)
{
	if (!on) {
		__atomic_store_n(&g_vga_hold, 0, __ATOMIC_SEQ_CST);
		__asm__ volatile ("dsb ish\n\tsev" ::: "memory");
		return;
	}
	__atomic_store_n(&g_vga_hold, 1, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&g_vga_busy, __ATOMIC_SEQ_CST))
		__asm__ volatile ("yield");
}

void pc_vga_core_run(void)
{
	for (;;) {
#if defined(CIRCLE_PC_STATS)
		g_core_iter[2]++;
#endif
		PC *pc = __atomic_load_n(&g_vga_pc, __ATOMIC_ACQUIRE);
		if (!pc || !(__atomic_load_n(&g_vga_req, __ATOMIC_ACQUIRE) & 1u) ||
		    __atomic_load_n(&g_vga_hold, __ATOMIC_ACQUIRE)) {
			CORE_WFE(2);
			continue;
		}
#if defined(CIRCLE_PC_STATS)
		g_core_work[2]++;
#endif
		/* Busy first, hold second: pc_vga_hold() sets hold first and
		 * busy second, so one of the two always sees the other. */
		__atomic_store_n(&g_vga_busy, 1, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&g_vga_hold, __ATOMIC_SEQ_CST)) {
			__atomic_store_n(&g_vga_busy, 0, __ATOMIC_SEQ_CST);
			continue;
		}
		const uint32_t req = __atomic_exchange_n(&g_vga_req, 0, __ATOMIC_ACQ_REL);
		vga_refresh(pc->vga, pc->redraw, pc->redraw_data, (req & 2u) != 0);
		__atomic_store_n(&g_vga_busy, 0, __ATOMIC_SEQ_CST);
	}
}
#endif

void pc_vga_step(void *o)
{
	PC *pc = o;
	int refresh = vga_step(pc->vga);
	if (refresh) {
		vga_refresh(pc->vga, pc->redraw, pc->redraw_data, 0);
	}
}

/* Where pc_step() spends what is not the interpreter, for the STATS build:
 * timers and keyboard, DMA, SB16, FDC, network, poll hook. */
uint32_t g_step_dev_us[6], g_step_steps;

void pc_step(PC *pc)
{
#if defined(CIRCLE_PC_STATS)
	g_step_steps++;
#endif
	FRANK_PROF_T(ft_total);
	FRANK_PROF_T(ft_dev);
	/* reset_request is handled in main.c via load_bios_and_reset() */
#if defined(CIRCLE_PC_STATS)
	uint32_t t_dev = get_uticks(), t_now;
#define STEP_T(i) (t_now = get_uticks(), g_step_dev_us[i] += t_now - t_dev, t_dev = t_now)
#else
#define STEP_T(i) ((void)0)
#endif
	int refresh = vga_step(pc->vga);
	i8254_update_irq(pc->pit);
	cmos_update_irq(pc->cmos);
	if (pc->enable_serial)
		u8250_update(pc->serial);
	kbd_step(pc->i8042);
	STEP_T(0);
	i8257_dma_run(pc->isa_dma);
	i8257_dma_run(pc->isa_hdma);
	STEP_T(1);
	if (pc->sb16_enabled)
		sb16_poll(pc->sb16);
	STEP_T(2);
	if (pc->fdc) fdc_tick(pc->fdc);
	STEP_T(3);
	/* Anything the adapter has heard, handed to the card.  Here rather than
	 * on a timer because it has to happen between instructions. */
	if (pc->ne2000) ne2000_step(pc->ne2000);
	STEP_T(4);
	FRANK_PROF_ADD(ft_dev, prof_dev);
#if !defined(BUILD_ESP32) && !defined(CIRCLE_BUILD)
	pc->poll(pc->redraw_data);
	if (refresh) {
		vga_refresh(pc->vga, pc->redraw, pc->redraw_data,
			    pc->full_update != 0);
		if (pc->full_update == 2)
			pc->full_update = 0;
	}
#else
	{
		if (pc->poll) pc->poll(pc->redraw_data);
	}
	STEP_T(5);
	if (refresh && pc->redraw) {
		/* Drawn on core 2: see pc_vga_core_run(). */
		pc_vga_request(pc->full_update != 0);
		if (pc->full_update == 2)
			pc->full_update = 0;
	}
#endif
#ifdef USEKVM
	cpukvm_step(pc->cpu, 4096);
#else
#if defined(BUILD_ESP32)
	cpui386_step(pc->cpu, 512);
/*
 * The Circle target belongs in this branch too.
 *
 * Nothing below is specific to the RP2350: it is the generic pacing for
 * this emulator - how often the PIT is polled so IRQ0 edges are not merged,
 * and the interleaving that refills the OPL ring sixteen times per
 * pc_step().  The guard named one board only because that is where it was
 * written.
 *
 * A Circle build therefore fell through to the plain
 * cpui386_step(pc->cpu, 10240) below, which calls adlib_core0() never.
 * With no producer the OPL ring stayed empty for the whole session:
 * measured on hardware, adlib_getsample() underran about 43,500 times a
 * second at a 44,100 Hz output rate and returned its held last value, which
 * had never been anything but zero.  The chip was fine and the music driver
 * was writing thousands of key-ons; nothing was asking the chip to render.
 * That is the "AdLib plays nothing" report, and the coarser PIT cadence in
 * the fallback is the likely reason PC speaker music fared badly as well.
 */
#elif defined(CIRCLE_BUILD)
	/* Reload below 16384 means the guest wants faster than ~73 Hz. Read once
	 * per pc_step(), not per pass - it cannot change underneath us here.
	 *
	 * A single fixed polling cadence is not sufficient here.  Prehistorik
	 * uses a reload of 1066 (~1119 Hz), for which one poll per four 256-insn
	 * passes is adequate.  Jill 3, Secret Agent and Electro Body can program
	 * reloads around 196 (~6087 Hz).  Polling those only every fourth pass
	 * caps delivered IRQ0 edges at roughly 1.8 kHz: the PIC stores an edge as
	 * one pending bit, so i8254_update_irq() cannot recover the merged edges.
	 * That made the games' wall clock run about 3.3x slow even though guest
	 * instruction throughput remained normal.
	 *
	 * Select a conservative cadence from the latched reload.  The smallest
	 * reloads are checked after every 256 guest instructions; slower custom
	 * ticks retain the cheaper cadence, and the standard DOS tick keeps the
	 * original single 4096-instruction call below. */
	const int pit_reload = pit_get_initial_count(pc->pit, 0);
	const int pit_fast = pit_reload < 16384;

	FRANK_DIAG_COUNT(pc_steps);
	FRANK_DIAG_SET(pit_ch0_count, (uint32_t)pit_reload);
	FRANK_DIAG_SET(uticks, get_uticks());
	/*
	 * The cadence below was chosen on a board doing 1.8 MIPS, where one
	 * pc_step() of 4096 instructions took about 2.3 ms.  Circle runs the
	 * same 4096 instructions in well under a millisecond, so the same
	 * divisor leaves far longer gaps in wall-clock terms than it did
	 * there - and the PIC's request bit does not queue, so every edge
	 * that lands while one is still pending is simply lost.
	 *
	 * Measured on hardware with Prehistorik's reload of 1066: the PIT
	 * produced its full 1119 edges a second, the PIC accepted 750 and
	 * dropped 370, and the speaker music played slow in proportion.
	 * Checking on every pass closes that gap; at this speed it costs
	 * sixteen timer reads per pc_step, a few milliseconds a second.
	 */
	const unsigned pit_poll_mask = 0u;
	/*
	 * Below 512 the reload is not a music tick, it is a sample clock, and
	 * one poll per 256-instruction pass is no longer enough.
	 *
	 * Electro Body programs channel 0 to 140 - 8523 Hz - and hands the OPL
	 * one 6-bit sample per IRQ0.  Polling every 256 instructions delivers
	 * 7381 edges/s at the 1.76 MIPS this interpreter sustains, measured on
	 * the board, and the guest actually consumed 6762/s: its music ran at
	 * 79% speed while nothing in the counters looked wrong, because
	 * i8254_update_irq() faithfully pulses the line for every period it
	 * missed and the PIC merges those pulses into the single IRR bit it
	 * has.  A period that elapses entirely between two polls is not late,
	 * it is gone.
	 *
	 * Splitting the pass four ways puts a poll every 64 instructions, about
	 * 27 kHz, three times the fastest reload these games ask for.  The cost
	 * is confined to them: the standard 18.2 Hz tick still takes the single
	 * 4096-instruction call at the bottom of this function.
	 */
	const int pit_subdiv = pit_reload < 512 ? 4 : 1;
	const int pit_chunk = 256 / pit_subdiv;

	/*
	 * One call of 4096 instructions, or sixteen passes when the guest has
	 * asked for a fast tick.
	 *
	 * The AdLib used to be rendered here too, interleaved with the
	 * interpreter sixteen times per pc_step().  It is synthesised on a core
	 * of its own now (see adlib_synth_run()), and the interleave only
	 * chopped the interpreter into 256-instruction calls - which also capped
	 * every JIT chain at 256 instructions.
	 */
	if (pit_fast) {
		/* Games that reprogram channel 0 for their music. */
		for (int i = 0; i < 16; ++i) {
			for (int j = 0; j < pit_subdiv; ++j) {
				cpui386_step(pc->cpu, pit_chunk);
				if (pit_subdiv > 1)
					i8254_update_irq(pc->pit);
			}
			if (pit_subdiv == 1 &&
			    ((unsigned)i & pit_poll_mask) == pit_poll_mask)
				i8254_update_irq(pc->pit);
		}
	} else {
		cpui386_step(pc->cpu, 4096);
	}
#else
	cpui386_step(pc->cpu, 10240);
#endif
#endif
	FRANK_PROF_ADD(ft_total, prof_total);
	FRANK_DIAG_COUNT(prof_steps);
#if SUBSYS_PROFILE
	if (++g_prof.steps >= PROF_REPORT_STEPS)
		prof_report();
#endif

#ifdef I386_PROFILE
	/* Dump profile every ~10M instructions */
	static uint32_t prof_dump_counter = 0;
	prof_dump_counter += 4096;
	if (prof_dump_counter >= 10000000) {
		i386_profile_dump();
		i386_profile_reset();
		prof_dump_counter = 0;
	}
#endif
}

static void raise_irq(void *o, PicState2 *s)
{
	cpu_raise_irq(o);
}

static int read_irq(void *o)
{
	PicState2 *s = o;
	return i8259_read_irq(s);
}

static void set_irq(void *o, int irq, int level)
{
	PicState2 *s = o;
	return i8259_set_irq(s, irq, level);
}

static void set_pci_vga_bar(void *opaque, int bar_num, uint32_t addr, bool enabled)
{
	PC *pc = opaque;
	if (enabled)
		pc->pci_vga_ram_addr = addr;
	else
		pc->pci_vga_ram_addr = -1;
#ifdef USEKVM
	if (enabled)
		cpukvm_register_mem(pc->cpu, 2, addr, pc->vga_mem_size,
				    pc->vga_mem);
	else
		cpukvm_register_mem(pc->cpu, 2, addr, 0,
				    NULL);
#endif
}

/* Guest accesses to the linear framebuffer, by width, for the STATS build. */
uint32_t g_lfb_rd[3], g_lfb_wr[3], g_lfb_str, g_lfb_str_bytes;

static u8 iomem_read8(void *iomem, uword addr)
{
	PC *pc = iomem;
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
#if defined(CIRCLE_PC_STATS)
		g_lfb_rd[0]++;
#endif
		addr -= vga_addr2;
		if (addr < pc->vga_mem_size)
			return pc->vga_mem[addr];
		else
			return 0;
	}
	return vga_mem_read(pc->vga, addr - 0xa0000);
}

static void iomem_write8(void *iomem, uword addr, u8 val)
{
	PC *pc = iomem;
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
		g_vram_writes++;
#if defined(CIRCLE_PC_STATS)
		g_lfb_wr[0]++;
#endif
		addr -= vga_addr2;
		if (addr < pc->vga_mem_size)
			pc->vga_mem[addr] = val;
		return;
	}
	vga_mem_write(pc->vga, addr - 0xa0000, val);
}

/*
 * Wide reads from the linear framebuffer in one load, like the writes below.
 *
 * They were two, or four, trips through iomem_read8(), each testing the
 * aperture again - and Windows 95 reads its screen back constantly: 4.5
 * million 16-bit reads in ten seconds of opening the Start menu.
 */
static u16 iomem_read16(void *iomem, uword addr)
{
	PC *pc = iomem;
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
#if defined(CIRCLE_PC_STATS)
		g_lfb_rd[1]++;
#endif
		addr -= vga_addr2;
		if (addr + 1 < pc->vga_mem_size)
			return *(uint16_t *)&(pc->vga_mem[addr]);
		return 0;
	}
	return iomem_read8(iomem, addr) |
		((u16) iomem_read8(iomem, addr + 1) << 8);
}

static void iomem_write16(void *iomem, uword addr, u16 val)
{
	PC *pc = iomem;
	// fast path for vga ram
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
		g_vram_writes++;
#if defined(CIRCLE_PC_STATS)
		g_lfb_wr[1]++;
#endif
		addr -= vga_addr2;
		if (addr + 1 < pc->vga_mem_size)
			*(uint16_t *)&(pc->vga_mem[addr]) = val;
		return;
	}
	vga_mem_write16(pc->vga, addr - 0xa0000, val);
}

static u32 iomem_read32(void *iomem, uword addr)
{
	PC *pc = iomem;
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
#if defined(CIRCLE_PC_STATS)
		g_lfb_rd[2]++;
#endif
		addr -= vga_addr2;
		if (addr + 3 < pc->vga_mem_size)
			return *(uint32_t *)&(pc->vga_mem[addr]);
		return 0;
	}
	return iomem_read16(iomem, addr) |
		((u32) iomem_read16(iomem, addr + 2) << 16);
}

static void iomem_write32(void *iomem, uword addr, u32 val)
{
	PC *pc = iomem;
	// fast path for vga ram
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
		g_vram_writes++;
#if defined(CIRCLE_PC_STATS)
		g_lfb_wr[2]++;
#endif
		addr -= vga_addr2;
		if (addr + 3 < pc->vga_mem_size)
			*(uint32_t *)&(pc->vga_mem[addr]) = val;
		return;
	}
	vga_mem_write32(pc->vga, addr - 0xa0000, val);
}

static bool iomem_write_string(void *iomem, uword addr, uint8_t *buf, int len)
{
	PC *pc = iomem;
	// fast path for vga ram
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
		g_vram_writes++;
#if defined(CIRCLE_PC_STATS)
		g_lfb_str++; g_lfb_str_bytes += (uint32_t)len;
#endif
		addr -= vga_addr2;
		if (addr + len < pc->vga_mem_size) {
			memcpy(pc->vga_mem + addr, buf, len);
			return true;
		}
		return false;
	}
	return vga_mem_write_string(pc->vga, addr - 0xa0000, buf, len);
}

static void pc_reset_request(void *p)
{
	PC *pc = p;
	pc->reset_request = 1;
}

static CMOS *_pc_cmos_for_floppy = NULL;
/*
 * Bit 1 of the equipment byte is what software asks when it wants to know
 * whether there is a coprocessor.  CR0's ET bit says the same thing to code
 * that looks there instead, and the two have to agree: a machine that sets
 * one and not the other is one no real PC ever was.
 */
static uint8_t _pc_cmos_equipment = 0x41;   /* two floppies, no 387 */
static void cmos_floppy_update(uint8_t ta, uint8_t tb) {
    cmos_set_floppy_types(_pc_cmos_for_floppy, ta, tb);
    cmos_set(_pc_cmos_for_floppy, 0x14, _pc_cmos_equipment);
    cmos_update_checksum(_pc_cmos_for_floppy);
}

static PC *_pc_for_fdc = NULL;
static void fdc_mediachange_notify(int drive) {
    if (_pc_for_fdc && _pc_for_fdc->fdc)
        fdc_media_changed(_pc_for_fdc->fdc, drive);
}

/* CD-ROM media change callback: called by disk layer when a CD-ROM drive
 * is inserted (filename != NULL) or ejected (filename == NULL).
 *
 * drivenum = ata[] index (0..3), NOT the diskui selected_drive (0..4):
 *   ata[0] -> ide,  drive 0  (primary master)
 *   ata[1] -> ide,  drive 1  (primary slave)
 *   ata[2] -> ide2, drive 0  (secondary master)  <- DRIVE_CDROM_E via diskui
 *   ata[3] -> ide2, drive 1  (secondary slave)
 */
static PC *_pc_for_cdrom = NULL;
static void cdrom_change_notify(int drivenum, const char *filename, int was_present) {
    if (!_pc_for_cdrom) return;
    IDEIFState *ide = drivenum < 2 ? _pc_for_cdrom->ide : _pc_for_cdrom->ide2;
    int ide_drive;
    switch (drivenum) {
        case 0: ide_drive = 0; break;
        case 1: ide_drive = 1; break;
        case 2: ide_drive = 0; break;
        case 3: ide_drive = 1; break;
        default: return;
    }
    BlockDev *dev = filename ? ata_get_dev(drivenum) : NULL;
    ide_change_cd(ide, ide_drive, dev, was_present);
}

PC *pc_new(SimpleFBDrawFunc *redraw, void (*poll)(void *), void *redraw_data,
	   u8 *fb, PCConfig *conf)
{
#if TRACE_PORTS
	f_open(&ports_log, "ports.log", FA_WRITE | FA_CREATE_ALWAYS);
#endif
	PC *pc = pcmalloc(sizeof(PC));
#ifdef CIRCLE_BUILD
    /* Room for the largest the settings can ask for, whatever this machine
     * is being told it has; see phys_mem_capacity in pc.h. */
    long mem_capacity = conf->mem_size > PC_MAX_MEM_SIZE ? conf->mem_size
                                                         : PC_MAX_MEM_SIZE;
    char *mem = (char *)pcmalloc(mem_capacity);
    if (!mem) { mem_capacity = conf->mem_size; mem = (char *)pcmalloc(mem_capacity); }
    if (!pc || !mem) return NULL;
#else
    char *mem = (uint8_t*)0x11000000;
#endif
	CPU_CB *cb = NULL;
	memset(mem, 0, mem_capacity);
	frank_diag_arm();
#ifdef BUILD_ESP32
	extern char *pcram;
	extern long pcram_len;
	pcram = mem + 0xa0000;
	pcram_len = 0xc0000 - 0xa0000;
#endif
#ifdef USEKVM
	pc->cpu = cpukvm_new(mem, conf->mem_size, &cb);
#else
	pc->cpu = cpui386_new(conf->cpu_gen, mem, conf->mem_size, &cb);
	if (conf->fpu) {
		cpui386_enable_fpu(pc->cpu);
		_pc_cmos_equipment |= 0x02;
	} else {
		_pc_cmos_equipment &= (uint8_t)~0x02;
	}
#endif
	pc->bios = conf->bios;
	pc->vga_bios = conf->vga_bios;
	pc->linuxstart = conf->linuxstart;
	pc->kernel = conf->kernel;
	pc->initrd = conf->initrd;
	pc->cmdline = conf->cmdline;
	pc->enable_serial = conf->enable_serial;
#if !defined(_WIN32) && !defined(__wasm__) && !defined(CIRCLE_BUILD)
	if (pc->enable_serial)
		CaptureKeyboardInput();
#endif
	pc->full_update = 0;

	pc->pic = i8259_init(raise_irq, pc->cpu);
	cb->pic = pc->pic;
	cb->pic_read_irq = read_irq;

	pc->pit = i8254_init(0, pc->pic, set_irq);
	pc->serial = u8250_init(4, pc->pic, set_irq);
	pc->cmos = cmos_init(conf->mem_size, 8, pc->pic, set_irq);
	_pc_cmos_for_floppy = pc->cmos;

	/* Set up INT 13h disk handler (real mode - DOS) */
	disk_set_cpu(pc->cpu);
	disk_set_cmos_callback(cmos_floppy_update);

#ifndef CIRCLE_BUILD
	netredirect_init(pc->cpu, conf->redirector);
#endif

	/*
	 * Outside the guard above, which is for the hosted redirector alone.
	 * This was inside it by mistake and so never ran on this target: the
	 * port was left unset, which is a null pointer the first time the
	 * guest reads port zero.  The DMA controller's half of that fix is
	 * below, where the controller exists.
	 */
	pc->cpu->io_fast_port = 0x3da;
	pc->cpu->io_fast_fn = pc_in_status1;
	pc->cpu->io_fast_arg = pc;

	/* Set up IDE emulation (protected mode - Win95) */
	pc->ide  = ide_allocate(14, pc->pic, set_irq);
	pc->ide2 = ide_allocate(15, pc->pic, set_irq);

	/* Register CD-ROM callback BEFORE insertdisk so the callback fires
	 * correctly when insertdisk opens a configured CD image below. */
	_pc_for_cdrom = pc;
	disk_set_cdrom_change_callback(cdrom_change_notify);

	/* Attach hard disks and configured CD-ROMs.
	 * ide_attach_cd MUST come before insertdisk for CD slots: insertdisk
	 * immediately fires disk_cdrom_change_cb which calls ide_change_cd,
	 * and that requires drives[n] to already exist. */
	for (int i = 0; i < 4; i++) {
		if (!conf->ata[i] || conf->ata[i][0] == 0)
			continue;
		if (conf->iscd[i]) {
			/* Attach ATAPI slot first, then open the image */
			if (i < 2)
				ide_attach_cd(pc->ide, i);
			else
				ide_attach_cd(pc->ide2, i - 2);
			insertdisk(i, false, true, conf->ata[i]);
		} else {
			/* HDD: insertdisk opens the file, then attach */
			insertdisk(i, false, false, conf->ata[i]);
			BlockDev *dev = ata_get_dev(i);
			if (blk_present(dev))
				ide_attach_ata(i < 2 ? pc->ide : pc->ide2,
				               i < 2 ? i : i - 2,
				               dev,
				               ata_get_cyls(i),
				               ata_get_heads(i),
				               ata_get_sects(i));
		}
	}

	/* CD-ROM E: always present on ide2/drive0 (secondary master).
	 * Only attach if cdc= didn't already claim that slot (ata[2]). */
	if (!ide_has_drive(pc->ide2, 0))
		ide_attach_cd(pc->ide2, 0);



	ide_fill_cmos(pc->ide, pc->cmos, cmos_set);

	/* we have emulation for 2 FDDs (CMOS 0x10):
		биты 7-4 = тип A:
		биты 3-0 = тип B:
		значение 4 = 1.44MB 3.5"
	*/
//	cmos_set(pc->cmos, 0x10, 0x44); // A: = 1.44MB, B: = 1.44MB
//	cmos_set(pc->cmos, 0x14, 0x41); // бит 0 = флоппи есть, биты 7-6 = 01 = два дисковода
	/* Checksum ПОСЛЕ всех записей в диапазон 0x10-0x2D */
//	cmos_update_checksum(pc->cmos);

	int piix3_devfn;
	pc->i440fx = i440fx_init(&pc->pcibus, &piix3_devfn);
	pc->pci_ide = piix3_ide_init(pc->pcibus, piix3_devfn + 1);

	pc->phys_mem = mem;
	pc->phys_mem_size = conf->mem_size;
	pc->phys_mem_capacity = mem_capacity;

	cb->io = pc;
	cb->io_read8 = pc_io_read;
	cb->io_write8 = pc_io_write;
	cb->io_read16 = pc_io_read16;
	cb->io_write16 = pc_io_write16;
	cb->io_read32 = pc_io_read32;
	cb->io_write32 = pc_io_write32;
	cb->io_read_string = pc_io_read_string;
	cb->io_write_string = pc_io_write_string;

	pc->boot_start_time = 0;

	/* Use the whole buffer, ignoring whatever vga_mem the config says.
	 * This ensures Wolf3D's three video pages (dword offsets 0 / 16640 /
	 * 33280, up to byte 133120) are never dropped; old SD-card configs with
	 * vga_mem=128K would otherwise leave the third page zeroed (black) due to
	 * the size check in vga_mem_write.
	 *
	 * The buffer is allocated at this size, so the two cannot disagree; when
	 * they did - a buffer built at 128 KB against a constant still saying 256
	 * - the memset below ran off the end and zeroed 128 KB of whatever
	 * followed it. */
#ifndef EMU_VGA_MEM_SIZE_KB
#define EMU_VGA_MEM_SIZE_KB 256
#endif
	/*
	 * What the configuration asked for, if it asked for anything sane.
	 *
	 * This used to ignore the setting and always take the constant, so
	 * the machine had 256 KB whatever the file said and the log reported
	 * the number nobody was using.  A card that small has no room for a
	 * second page at 640x400, which is why Red Alert drew into a page the
	 * display never showed and the screen stayed black.
	 *
	 * The size must be a power of two: the VESA bank register is masked
	 * with (size >> 16) - 1.
	 */
	pc->vga_mem_size = (uword)EMU_VGA_MEM_SIZE_KB << 10;
	if (conf->vga_mem_size >= (64 << 10) && conf->vga_mem_size <= (16 << 20) &&
	    (conf->vga_mem_size & (conf->vga_mem_size - 1)) == 0)
		pc->vga_mem_size = (uword)conf->vga_mem_size;
	/* Room for the largest card the menu offers, so that changing the size
	 * later is a restart rather than a rebuild; see vga_mem_capacity. */
	pc->vga_mem_capacity = pc->vga_mem_size > PC_MAX_VGA_MEM_SIZE
			     ? pc->vga_mem_size : PC_MAX_VGA_MEM_SIZE;
	pc->vga_mem = (uint8_t *)pcmalloc(pc->vga_mem_capacity);
	if (!pc->vga_mem) {
		pc->vga_mem_capacity = pc->vga_mem_size;
		pc->vga_mem = (uint8_t *)pcmalloc(pc->vga_mem_capacity);
	}
	if (!pc->vga_mem) return NULL;
	memset(pc->vga_mem, 0, pc->vga_mem_capacity);
	pc->vga = vga_init(pc->vga_mem, pc->vga_mem_size, pc->vga_mem_capacity,
			   fb, conf->width, conf->height);
	vga_set_force_8dm(pc->vga, conf->vga_force_8dm);
	/* Off presents a plain ISA VGA with no PCI function and no linear
	 * framebuffer; see FRANK_PCI_VGA.  Keen 4 announces "SVGA Compatibility
	 * Mode Enabled" and then mis-detects everything, so it is worth being able
	 * to look like a 1991 card. */
#ifndef FRANK_PCI_VGA
#define FRANK_PCI_VGA 1
#endif
	pc->pci_vga = FRANK_PCI_VGA
		? vga_pci_init(pc->vga, pc->pcibus, pc, set_pci_vga_bar) : NULL;
	pc->pci_vga_ram_addr = -1;
	disk_set_vga(pc->vga);

	/* Attach floppy disks using INT 13h disk handler */
	const char **fdd = conf->fdd;
	for (int i = 0; i < 2; i++) {
		if (!fdd[i] || fdd[i][0] == 0)
			continue;
		/* Floppy drives use drivenum 0 and 1 */
		insertdisk(i, true, false, fdd[i]);
	}

	cb->iomem = pc;
	cb->iomem_read8 = iomem_read8;
	cb->iomem_write8 = iomem_write8;
	cb->iomem_read16 = iomem_read16;
	cb->iomem_write16 = iomem_write16;
	cb->iomem_read32 = iomem_read32;
	cb->iomem_write32 = iomem_write32;
	cb->iomem_write_string = iomem_write_string;

	pc->redraw = redraw;
	pc->redraw_data = redraw_data;
	pc->poll = poll;

	pc->i8042 = i8042_init(&(pc->kbd), &(pc->mouse),
			       1, 12, pc->pic, set_irq,
			       pc, pc_reset_request);
	i8042_set_cpu(pc->cpu);
	pc->adlib = adlib_new();
	/*
	 * The network card, at the address every DOS packet driver defaults to.
	 *
	 * It is created whether or not the host has an adapter: the card is
	 * what the guest's driver looks for, and a machine whose card appears
	 * only sometimes is harder to configure than one whose cable is
	 * unplugged.  Nothing leaves it until the host binds itself to it with
	 * ne2000_set_host().
	 */
	pc->ne2000 = isa_ne2000_init(0x300, 3, pc->pic, set_irq);
	pc->isa_dma = i8257_new(pc->phys_mem, pc->phys_mem_size,
				0x00, 0x80, 0x480, 0);
	pc->isa_hdma = i8257_new(pc->phys_mem, pc->phys_mem_size,
				 0xc0, 0x88, 0x488, 1);
	/*
	 * Here, and not with the other repair a hundred and sixty lines above,
	 * where it was: pc->isa_dma is assigned on the line before this one, so
	 * up there it was read before it was written.  Whatever pcmalloc()
	 * happened to leave in that word decided what followed - zero, and the
	 * controller never learned the CPU at all, so the prefetch still was
	 * not dropped when it moved bytes into guest memory and the fix was no
	 * fix; anything else, and the call wrote eight bytes through it.  With
	 * USB storage attached the heap held something that looked like a
	 * pointer, and the machine took a data abort before it ever booted.
	 */
	i8257_set_cpu(pc->isa_dma, pc->cpu);
	/* Emulink FDD – virtual floppy on ports 0xF1F0/0xF1F4 (required by BIOS) */
	memset(&pc->emulink, 0, sizeof(pc->emulink));
	pc->emulink.cmd = -1;

	/* FDC (Intel 8272A/82077AA) – port I/O 0x3F0-0x3F7, DMA ch2, IRQ 6.
	 * Created after isa_dma/pic, and floppy images already inserted above,
	 * so fdc_media_changed fires correctly on subsequent insert/eject. */
	pc->fdc = fdc_new(pc->pic, pc->isa_dma);
	_pc_for_fdc = pc;
	disk_set_fdc_mediachange_callback(fdc_mediachange_notify);
	pc->sb16 = sb16_new(0x220, 5,
			    pc->isa_dma, pc->isa_hdma,
			    pc->pic, set_irq);
	pc->pcspk = pcspk_init(pc->pit);
	sn76489_reset();

	// Audio/mouse enable flags default to enabled
	// These can be disabled via config_set_* functions at runtime
	pc->adlib_enabled = 1;
	pc->sb16_enabled = 1;
	pc->pcspk_enabled = 1;
	pc->tandy_enabled = 0;
	pc->covox_enabled = 1;
	pc->mpu401_enabled = 1;
	pc->covox_sample  = 0;
	pc->dss_enabled = 0;
	pc->mouse_enabled = 1;

	pc->port92 = 0x2;
	pc->shutdown_state = 0;
	pc->reset_request = 0;
#if defined(CIRCLE_BUILD)
	__atomic_store_n(&g_vga_pc, pc, __ATOMIC_RELEASE);
#endif
	return pc;
}

/*
 * Change what the machine is.
 *
 * Only four things here are settled when the machine is built rather than
 * read as it runs: how much memory it has, how much of it is on the video
 * card, which processor it claims to be, and whether a floating-point unit is
 * fitted.  Both buffers are already as large as the settings allow, the
 * generation is only read by CPUID, and the coprocessor is one allocation -
 * so all four can be changed in place, and the guest reset that follows sees
 * a different machine.
 *
 * The alternative would have been to build a new machine and free the old
 * one, and there is no teardown: fourteen devices are allocated here and
 * nothing gives them back.  This is the smaller and safer of the two.
 */
void pc_set_machine(PC *pc, long mem_size, long vga_mem_size,
		   int cpu_gen, int fpu)
{
	if (!pc || !pc->cpu) return;

	/*
	 * The card's memory, on the same terms as the machine's: the buffer is
	 * already the largest size on offer, so this only decides how much of
	 * it the VGA will admit to.  A size that is not a power of two is
	 * refused rather than rounded, because the VESA bank register is
	 * masked with (size >> 16) - 1 and a mask off by one bank would send
	 * writes to the wrong sixty-four kilobytes.
	 */
	if (vga_mem_size >= 64 * 1024 && vga_mem_size <= pc->vga_mem_capacity &&
	    (vga_mem_size & (vga_mem_size - 1)) == 0 &&
	    vga_mem_size != pc->vga_mem_size) {
		if (vga_mem_size > pc->vga_mem_size)
			memset(pc->vga_mem + pc->vga_mem_size, 0,
			       (size_t)(vga_mem_size - pc->vga_mem_size));
		pc->vga_mem_size = (int)vga_mem_size;
		vga_set_ram_size(pc->vga, pc->vga_mem_size);
	}

	if (mem_size < 1024 * 1024) mem_size = 1024 * 1024;
	if (mem_size > pc->phys_mem_capacity) mem_size = pc->phys_mem_capacity;

	/* Anything the guest could not reach before has to come up empty, the
	 * same as memory that has just been powered on. */
	if (mem_size > pc->phys_mem_size)
		memset(pc->phys_mem + pc->phys_mem_size, 0,
		       (size_t)(mem_size - pc->phys_mem_size));

	pc->phys_mem_size = mem_size;
	cpui386_set_mem_size(pc->cpu, mem_size);
	cpui386_set_gen(pc->cpu, cpu_gen);
	cpui386_set_fpu(pc->cpu, fpu);

	if (pc->cmos) {
		cmos_set_mem_size(pc->cmos, mem_size);
		cmos_update_checksum(pc->cmos);
	}
	/* CR0's ET bit and the equipment byte both say whether there is a
	 * coprocessor, and the reset below is what puts ET back. */
	_pc_cmos_equipment = fpu ? (uint8_t)(_pc_cmos_equipment | 0x02)
	                         : (uint8_t)(_pc_cmos_equipment & ~0x02);
	if (pc->cmos) {
		cmos_set(pc->cmos, 0x14, _pc_cmos_equipment);
		cmos_update_checksum(pc->cmos);
	}
}

/*
 * Hide the BIOS's Plug and Play installation structure.
 *
 * SeaBIOS carries a PnP BIOS header but answers almost none of its calls -
 * it has no device nodes to give.  Windows 95 finds the header and takes the
 * machine for a Plug and Play one, which changes two things: the PnP BIOS
 * device fails to start (Code 24), and the PCI bus is expected to come from
 * that BIOS's node list rather than from detection, so it never appears at
 * all - and with it no PCI device, the display adapter included, is ever
 * offered a driver.  Without the header, Windows detects the PCI BIOS and
 * installs the bus itself, as on any pre-PnP PCI machine.
 *
 * The header is found the way an operating system finds it: "$PnP" on a
 * 16-byte boundary in the F segment, version 1.0, length 0x21.  The same
 * four bytes appear inside SeaBIOS's code as constants, which is why the
 * alignment and the fields are checked and not just the signature.
 */
static void hide_pnp_bios(PC *pc)
{
	uint8_t *f = (uint8_t *)pc->phys_mem + 0xf0000;
	for (int off = 0; off + 0x21 <= 0x10000; off += 16) {
		if (memcmp(f + off, "$PnP", 4) == 0 && f[off + 4] == 0x10 &&
		    f[off + 5] == 0x21) {
			f[off + 3] = 'X';
			return;
		}
	}
}

void load_bios_and_reset(PC *pc)
{
	int bios_size = 0;
	/* What the guest wrote is made durable before it starts over. */
	ide_sync_idle(1);
	if (pc->bios && pc->bios[0])
		bios_size = load_rom(pc->phys_mem, pc->bios, 0x100000, 1);
	hide_pnp_bios(pc);

	// Only load VGA BIOS if main BIOS doesn't overlap with 0xC0000
	// 256KB BIOS starts at 0xC0000, so VGA BIOS would overwrite it
	int bios_start = 0x100000 - bios_size;
	if (pc->vga_bios && pc->vga_bios[0] && bios_start >= 0xC8000) {
		load_rom(pc->phys_mem, pc->vga_bios, 0xc0000, 0);
	} else if (pc->vga_bios && pc->vga_bios[0]) {
		printf("Skipping VGA BIOS - main BIOS overlaps at 0x%x\n", bios_start);
	}
	sn76489_reset();

	// Debug: verify BIOS loaded at reset vector
	uint8_t *reset_vec = (uint8_t *)pc->phys_mem + 0xFFFF0;
	printf("Reset vector at 0xFFFF0: %02x %02x %02x %02x %02x %02x %02x %02x\n",
	       reset_vec[0], reset_vec[1], reset_vec[2], reset_vec[3],
	       reset_vec[4], reset_vec[5], reset_vec[6], reset_vec[7]);
	printf("phys_mem=%p, reset_vec=%p\n", pc->phys_mem, reset_vec);

#ifndef USEKVM
	if (pc->kernel && pc->kernel[0]) {
		int start_addr = 0x10000;
		int cmdline_addr = 0xf800;
		int kernel_size = load_rom(pc->phys_mem, pc->kernel, 0x00100000, 0);
		int initrd_size = 0;
		if (pc->initrd && pc->initrd[0])
			initrd_size = load_rom(pc->phys_mem, pc->initrd, 0x00400000, 0);
		if (pc->cmdline && pc->cmdline[0])
			strcpy(pc->phys_mem + cmdline_addr, pc->cmdline);
		else
			strcpy(pc->phys_mem + cmdline_addr, "");

		load_rom(pc->phys_mem, pc->linuxstart, start_addr, 0);
		cpui386_reset_pm(pc->cpu, 0x10000);
		cpui386_set_gpr(pc->cpu, 0, pc->phys_mem_size);
		cpui386_set_gpr(pc->cpu, 3, initrd_size);
		cpui386_set_gpr(pc->cpu, 1, cmdline_addr);
		cpui386_set_gpr(pc->cpu, 2, kernel_size);
	} else {
		cpui386_reset(pc->cpu);
	}
	// Debug: print CPU state after reset
	uint32_t cs, ip;
	int halt;
	cpui386_get_state(pc->cpu, &cs, &ip, &halt);
	printf("CPU after reset: CS=%04lx IP=%08lx halt=%d\n",
	       (unsigned long)cs, (unsigned long)ip, halt);
#endif
}

static long parse_mem_size(const char *value)
{
	int len = strlen(value);
	long a = atol(value);
	if (len) {
		switch (value[len - 1]) {
		case 'G': a *= 1024 * 1024 * 1024; break;
		case 'M': a *= 1024 * 1024; break;
		case 'K': a *= 1024; break;
		}
	}
	return a;
}

int parse_conf_ini(void* user, const char* section,
		   const char* name, const char* value)
{
	PCConfig *conf = user;
#define SEC(a) (strcmp(section, a) == 0)
#define NAME(a) (strcmp(name, a) == 0)
	// Support both [pc] and [386] sections for compatibility
	if (SEC("pc") || SEC("386")) {
		if (NAME("bios")) {
			conf->bios = strdup(value);
		} else if (NAME("vga_bios")) {
			conf->vga_bios = strdup(value);
		} else if (NAME("mem_size") || NAME("mem")) {
			conf->mem_size = parse_mem_size(value);
		} else if (NAME("cpu")) {
			conf->cpu_gen = atoi(value);
		} else if (NAME("hda")) {
			conf->ata[0] = strdup(value);
			conf->iscd[0] = 0;
		} else if (NAME("hdb")) {
			conf->ata[1] = strdup(value);
			conf->iscd[1] = 0;
		} else if (NAME("hdc")) {
			conf->ata[2] = strdup(value);
			conf->iscd[2] = 0;
		} else if (NAME("hdd")) {
			conf->ata[3] = strdup(value);
			conf->iscd[3] = 0;
		} else if (NAME("cda")) {
			conf->ata[0] = strdup(value);
			conf->iscd[0] = 1;
		} else if (NAME("cdb")) {
			conf->ata[1] = strdup(value);
			conf->iscd[1] = 1;
		} else if (NAME("cdc")) {
			conf->ata[2] = strdup(value);
			conf->iscd[2] = 1;
		} else if (NAME("cdd")) {
			conf->ata[3] = strdup(value);
			conf->iscd[3] = 1;
		} else if (NAME("fda")) {
			conf->fdd[0] = strdup(value);
		} else if (NAME("fdb")) {
			conf->fdd[1] = strdup(value);
		} else if (NAME("redirector")) {
			conf->redirector = atoi(value);
		} else if (NAME("linuxstart")) {
			conf->linuxstart = strdup(value);
		} else if (NAME("kernel")) {
			conf->kernel = strdup(value);
		} else if (NAME("initrd")) {
			conf->initrd = strdup(value);
		} else if (NAME("cmdline")) {
			conf->cmdline = strdup(value);
		} else if (NAME("enable_serial")) {
			conf->enable_serial = atoi(value);
		} else if (NAME("vga_force_8dm")) {
			conf->vga_force_8dm = atoi(value);
		}
	} else if (SEC("display")) {
		if (NAME("width")) {
			conf->width = atoi(value);
		} else if (NAME("height")) {
			conf->height = atoi(value);
		}
	} else if (SEC("cpu")) {
		if (NAME("gen")) {
			conf->cpu_gen = atoi(value);
		} else if (NAME("fpu")) {
			conf->fpu = atoi(value);
		}
	}
#undef SEC
#undef NAME
	return 1;
}
