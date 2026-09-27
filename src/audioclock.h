#ifndef AUDIOCLOCK_H
#define AUDIOCLOCK_H

/*
 * The output frame "now", as a clock, for stamping a device's register
 * writes with the frame they take effect at.
 *
 * The mixer takes frames in bursts - the output queue frees a thousand at a
 * time and a pass takes up to 512 - so its position stands still for ten or
 * twenty milliseconds and then jumps.  Stamped with that position, every
 * write made meanwhile lands on one frame and the music plays on a grid of
 * that coarseness; with Nuked OPL3 the user heard the Tyrian demo as flat,
 * without rhythm or bass, while an offline render of the same writes at
 * their logged times sounded right.  So the frame comes from the
 * microsecond clock instead, steered slowly towards the mixer's position so
 * that it keeps the output's own rate without its bursts.
 *
 * The mixer steers it on core 1 and the guest's port writes read it on core
 * 0, so it is published under a sequence count, odd while being written.
 */

#include <stdint.h>

uint32_t get_uticks();

typedef struct AudioClock {
    uint32_t seq;
    uint32_t us;
    int64_t  f16;       /* frames, Q16, at us */
    uint8_t  live;
} AudioClock;

#define AUDIOCLOCK_FRAMES_PER_US_Q32 \
    ((int64_t)(((uint64_t)SOUND_FREQUENCY << 32) / 1000000u))

static inline int64_t audioclock_at(int64_t f16, uint32_t us, uint32_t now)
{
    return f16 + (((int64_t)(uint32_t)(now - us) * AUDIOCLOCK_FRAMES_PER_US_Q32) >> 16);
}

/* The mixer, every 64 frames it takes; the only writer.  consumed is the
 * number of frames it has taken, ever. */
static inline void audioclock_steer(AudioClock *c, int64_t consumed)
{
    const uint32_t now = get_uticks();
    const int64_t est = audioclock_at(c->f16, c->us, now);
    const int64_t err = (consumed << 16) - est;
    int64_t f16;
    /* A first frame, or a gap far past any burst (the device was off, the
     * machine stalled): start over from the mixer. */
    if (!c->live || err > ((int64_t)8192 << 16) || err < -((int64_t)8192 << 16))
        f16 = consumed << 16;
    else
        f16 = est + err / 256;
    const uint32_t seq = c->seq;
    __atomic_store_n(&c->seq, seq + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&c->f16, f16, __ATOMIC_RELAXED);
    __atomic_store_n(&c->us, now, __ATOMIC_RELAXED);
    __atomic_store_n(&c->live, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&c->seq, seq + 2u, __ATOMIC_RELEASE);
}

/* The frame now, from any core; false until the mixer has run. */
static inline int audioclock_read(AudioClock *c, uint32_t *frame)
{
    uint32_t s1, s2, us;
    int64_t f16;
    uint8_t live;
    do {
        s1 = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
        f16 = __atomic_load_n(&c->f16, __ATOMIC_RELAXED);
        us = __atomic_load_n(&c->us, __ATOMIC_RELAXED);
        live = __atomic_load_n(&c->live, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = __atomic_load_n(&c->seq, __ATOMIC_RELAXED);
    } while ((s1 & 1u) || s1 != s2);
    if (!live) return 0;
    *frame = (uint32_t)(audioclock_at(f16, us, get_uticks()) >> 16);
    return 1;
}

/*
 * How far ahead of the clock a write is stamped: the frames a device's ring
 * is rendered ahead of the mixer, plus how far the mixer's bursts put it
 * ahead of the clock.  A write stamped behind what has already been
 * rendered would be applied late.
 */
#define AUDIOCLOCK_STAMP_LEAD(ring_lead) ((ring_lead) + 1024u)

#endif /* AUDIOCLOCK_H */
