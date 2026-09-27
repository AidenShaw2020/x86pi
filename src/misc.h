#ifndef MISC_H
#define MISC_H

#include <stdint.h>

typedef struct U8250 U8250;
U8250 *u8250_init(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level));
uint8_t u8250_reg_read(U8250 *uart, int off);
void u8250_reg_write(U8250 *uart, int off, uint8_t val);
void u8250_update(U8250 *uart);
void CaptureKeyboardInput();

typedef struct CMOS CMOS;
CMOS *cmos_init(long mem_size, int irq, void *pic, void (*set_irq)(void *pic, int irq, int level));
void cmos_update_irq(CMOS *s);
uint8_t cmos_ioport_read(CMOS *cmos, int addr);
void cmos_ioport_write(CMOS *cmos, int addr, uint8_t val);

uint8_t cmos_set(void *cmos, int addr, uint8_t val);
void cmos_update_checksum(void *cmos);
void cmos_set_mem_size(CMOS *c, long mem_size);
void cmos_set_floppy_types(CMOS *c, uint8_t type_a, uint8_t type_b);

/* The clock as a date, local time, years 2000-2099: how a battery-backed
 * clock on the board hands it over and takes it back. */
typedef struct CmosDateTime {
	int year, month, day, hour, min, sec;
} CmosDateTime;
void cmos_set_clock(CMOS *s, const CmosDateTime *dt);
/* Once after the guest has set the clock and then left it alone for a
 * moment: the time it set, as it now stands. */
int cmos_clock_changed(CMOS *s, CmosDateTime *dt);

/* EMULINK removed - disk operations use INT 13h disk handler instead */

#endif /* MISC_H */
