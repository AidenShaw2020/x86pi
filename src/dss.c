// https://archive.org/details/dss-programmers-guide
#pragma GCC optimize("Ofast")
#include "audio.h"
#include "dss.h"
#define INLINE inline

#define FIFO_BUFFER_SIZE 16

/*
 * A single-producer, single-consumer ring: the port writes push on core 0
 * and the mixer pops on core 1.  Head and tail run free and each is written
 * by one side only; a shared count decremented on one core and incremented
 * on the other loses updates and drifts.
 */
static uint8_t fifo_buffer[FIFO_BUFFER_SIZE] = { 0 };
static uint8_t fifo_head = 0;  // Write position, core 0
static uint8_t fifo_tail = 0;  // Read position, the mixer
static uint8_t dss_data;

static INLINE uint8_t fifo_count(void) {
    return (uint8_t)(__atomic_load_n(&fifo_head, __ATOMIC_ACQUIRE) -
                     __atomic_load_n(&fifo_tail, __ATOMIC_ACQUIRE));
}

int16_t dss_sample() {
    static uint8_t held = 0;
    const uint8_t t = fifo_tail;
    if ((uint8_t)(__atomic_load_n(&fifo_head, __ATOMIC_ACQUIRE) - t) == 0) {
        return held;
    }
    held = fifo_buffer[t & (FIFO_BUFFER_SIZE - 1)];
    __atomic_store_n(&fifo_tail, (uint8_t)(t + 1), __ATOMIC_RELEASE);
    return ((int16_t)held - 128) << 8;
}

static INLINE void fifo_push_byte(uint8_t value) {
    if (__builtin_expect(fifo_count() >= FIFO_BUFFER_SIZE, 0))
        return;

    const uint8_t h = fifo_head;
    fifo_buffer[h & (FIFO_BUFFER_SIZE - 1)] = value;
    __atomic_store_n(&fifo_head, (uint8_t)(h + 1), __ATOMIC_RELEASE);
}

static INLINE uint8_t fifo_is_full() {
    return fifo_count() == FIFO_BUFFER_SIZE ? 0x40 : 0x00;
}

uint8_t dss_in(const uint16_t portnum) {
    return portnum & 1 ? fifo_is_full() : dss_data;
}

void dss_out(uint16_t portnum, uint8_t value) {
    static uint8_t control = 0;

    if (portnum == 0x378) {
        dss_data = value;          // только запомнить
        return;
    }

    if (portnum == 0x37A) {
        // pin17 = SELECTIN = control bit3 (инвертированный).
        // Rising edge on pin17 => bit3 1->0
        const uint8_t old_b3 = control & 0x08;
        const uint8_t new_b3 = value   & 0x08;

        if (old_b3 && !new_b3) {
            fifo_push_byte(dss_data);
        }

        control = value;
    }
}
