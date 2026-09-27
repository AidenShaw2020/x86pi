#pragma once
#include <stdint.h>
#define __always_inline inline __attribute__((always_inline))
/*
 * A barrier that is actually one.
 *
 * This was a compiler barrier and nothing else, which on a part with one
 * core in order is the whole of what the name promises and on this one is
 * not: the Pi 3 has four, they reorder, and the emulator runs the machine on
 * core 0 while the mixer consumes its audio on core 1.  So every __dmb() in
 * the emulator - including the one that publishes the Sound Blaster's ring
 * pointer and the one behind the interrupt handoff - was stopping the
 * compiler and letting the hardware do as it pleased.
 *
 * What that sounds like: Draci historie speaks, and the ring hands out one
 * byte the DMA had not written yet - 0x01 with silence either side of it,
 * which is full-scale negative in unsigned eight-bit and is heard as a click.
 */
#define __dmb() __asm__ __volatile__("dmb ish" ::: "memory")
typedef unsigned int uint;
uint32_t time_us_32(void);
uint64_t time_us_64(void);
void sleep_us(uint32_t);
void sleep_ms(uint32_t);
int get_core_num(void);
int32_t __fast_mul(int32_t, int32_t);

uint32_t get_uticks(void);

/*
 * The guest clock without a call.
 *
 * get_uticks() lives in hal.cpp, so every caller pays a call across a
 * translation unit and the lazily initialised scale is tested again inside.
 * That measured twenty-four cycles against eleven for the counter read
 * alone, and the VGA status register - one guest instruction in six while a
 * game waits for the frame - reads the clock every time.
 *
 * The scale is the same 32.32 fixed point value hal.cpp derives from
 * CNTFRQ_EL0.  Until it has been worked out, fall back to the function; the
 * branch is perfectly predicted after the first call.
 */
extern uint64_t g_uticks_scale;
static inline uint32_t get_uticks_fast(void)
{
    if (__builtin_expect(g_uticks_scale != 0, 1)) {
        uint64_t c;
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c));
        return (uint32_t)(((unsigned __int128)c * g_uticks_scale) >> 32);
    }
    return get_uticks();
}
