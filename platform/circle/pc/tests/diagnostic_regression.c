/*
 * Hosted regression for the minimal pre-DOS interrupt path used by the
 * Circle PC diagnostic image.  This is deliberately a device-model test,
 * not a Circle test: time is fake, so 54,926 us is reproducible.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i386.h"
#include "i8254.h"
#include "i8259.h"
#include "misc.h"

static uint32_t s_now_us;
uint32_t get_uticks(void) { return s_now_us; }
uint32_t time_us_32(void) { return s_now_us; }
uint64_t time_us_64(void) { return s_now_us; }
void sleep_us(uint32_t us) { s_now_us += us; }
void sleep_ms(uint32_t ms) { s_now_us += ms * 1000u; }
int usleep(unsigned int us) { s_now_us += us; return 0; }

static CPUI386 *s_cpu;
static void pic_raise_cpu(void *opaque, PicState2 *pic)
{
    (void)opaque; (void)pic;
    assert(s_cpu);
    cpui386_raise_irq(s_cpu);
}

static void pic_program_at(PicState2 *pic)
{
    /* ICW1..4: master at 08h, slave at 70h, cascading IRQ2, 8086 mode. */
    i8259_ioport_write(pic, 0x20, 0x11);
    i8259_ioport_write(pic, 0xa0, 0x11);
    i8259_ioport_write(pic, 0x21, 0x08);
    i8259_ioport_write(pic, 0xa1, 0x70);
    i8259_ioport_write(pic, 0x21, 0x04);
    i8259_ioport_write(pic, 0xa1, 0x02);
    i8259_ioport_write(pic, 0x21, 0x01);
    i8259_ioport_write(pic, 0xa1, 0x01);
    i8259_ioport_write(pic, 0x21, 0x00);
    i8259_ioport_write(pic, 0xa1, 0x00);
}

static void test_cmos_ports(PicState2 *pic)
{
    CMOS *cmos = cmos_init(8L * 1024L * 1024L, 8, pic,
                            (void (*)(void *, int, int)) i8259_set_irq);
    assert(cmos);
    assert(cmos_ioport_read(cmos, 0x70) == 0xff);
    cmos_ioport_write(cmos, 0x70, 0x2e);
    cmos_ioport_write(cmos, 0x71, 0x5a);
    cmos_ioport_write(cmos, 0x70, 0x2e);
    assert(cmos_ioport_read(cmos, 0x71) == 0x5a);
    free(cmos);
}

static int test_pit_pic_cpu(PicState2 *pic)
{
    uint8_t *ram = calloc(1, 1024 * 1024);
    assert(ram);
    CPU_CB *cb = 0;
    s_cpu = cpui386_new(4, (char *)ram, 1024 * 1024, &cb);
    assert(s_cpu && cb);
    cb->pic = pic;
    cb->pic_read_irq = (int (*)(void *)) i8259_read_irq;

    /* A real-mode program that first halts with IF set.  IRQ0 vector 08h
     * points to a bare IRET; after it returns the NOP reaches the second HLT. */
    cpui386_reset(s_cpu);
    s_cpu->seg[1].sel = 0;
    s_cpu->seg[1].base = 0;
    s_cpu->seg[1].limit = 0xffff;
    s_cpu->ip = s_cpu->next_ip = 0x200;
    s_cpu->seg[2].sel = 0;
    s_cpu->seg[2].base = 0;
    s_cpu->seg[2].limit = 0xffff;
    s_cpu->sp_mask = 0xffff;
    s_cpu->gprx[4].r16 = 0x8000;
    ram[0x200] = 0xfb;             /* STI */
    ram[0x201] = 0xf4;             /* HLT */
    ram[0x202] = 0x90;             /* NOP */
    ram[0x203] = 0xf4;             /* HLT */
    ram[0x20] = 0x00;              /* IVT[8] offset */
    ram[0x21] = 0x03;
    ram[0x22] = 0x00;              /* IVT[8] segment */
    ram[0x23] = 0x00;
    ram[0x300] = 0xcf;             /* IRET */

    cpui386_step(s_cpu, 8);
    assert(s_cpu->halt && (s_cpu->flags & 0x200));

    PITState *pit = i8254_init(0, pic, (void (*)(void *, int, int)) i8259_set_irq);
    assert(pit);
    i8254_ioport_write(pit, 0x43, 0x36); /* ch0, lo/hi, mode 3 */
    i8254_ioport_write(pit, 0x40, 0x00); /* 65536 encoded as zero */
    i8254_ioport_write(pit, 0x40, 0x00);
    assert(pit_get_mode(pit, 0) == 3);
    assert(pit_get_initial_count(pit, 0) == 65536);

    /* At 54,926 us the integer conversion reaches exactly 65,536 PIT
     * clocks.  This currently implemented edge detector uses a strict
     * rollover test, so it observes the edge at the following microsecond.
     * Keep that fact explicit: changing it is a device behavior fix, not
     * something a passive diagnostic image may smuggle in. */
    s_now_us = 54926;
    i8254_update_irq(pit);
    assert(!s_cpu->intr);
    s_now_us = 54927;
    i8254_update_irq(pit);
    assert(s_cpu->intr);
    assert(((i8259_debug_master(pic) >> 8) & 1u) == 1u); /* master IRR0 */

    cpui386_step(s_cpu, 16);
    assert(s_cpu->halt);
    assert(s_cpu->ip == 0x203); /* vector 8 + IRET resumed after first HLT */
    assert(((i8259_debug_master(pic) >> 24) & 1u) == 1u); /* ISR0 until EOI */

    free(pit);
    cpui386_delete(s_cpu);
    s_cpu = 0;
    free(ram);
    return 0;
}

int main(void)
{
    PicState2 *pic = i8259_init(pic_raise_cpu, 0);
    assert(pic);
    pic_program_at(pic);
    test_cmos_ports(pic);
    const int rc = test_pit_pic_cpu(pic);
    if (rc) return rc;
    free(pic);
    puts("Circle PC PIT/PIC/CMOS/CPU regression PASS");
    return 0;
}
