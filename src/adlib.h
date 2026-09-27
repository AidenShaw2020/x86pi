#ifndef ADLIB_H
#define ADLIB_H

#include <stdint.h>

/*
 * A YMF262 (OPL3), synthesised by Nuked OPL3 on a core of its own.
 *
 * Core 0 decodes the ports, keeps the timers and the status register, and
 * queues every register write with the output sample it takes effect at.
 * Core 1 renders the chip into a stereo ring of output frames, applying
 * each queued write when its rendering reaches that sample, and the mixer,
 * also on core 1, drains the ring.
 */

/*
 * Output frames in the ring, and how far ahead of the mixer the synthesis
 * core renders.
 *
 * The lead is also the latency of every register write, and what keeps the
 * interval between two writes right: a write is stamped with the mixer's
 * position plus the lead, so two writes some samples apart in the guest land
 * that many samples apart in the output.  That is what register-0x40 sample
 * playback (Electro Body writes the total level at 8.5 kHz) depends on.
 */
#ifndef ADLIB_RING_FRAMES
#define ADLIB_RING_FRAMES   1024u   /* power of two */
#endif
#ifndef ADLIB_LEAD_SAMPLES
#define ADLIB_LEAD_SAMPLES  768u
#endif

typedef struct AdlibState AdlibState;

void adlib_write(void *opaque, uint32_t nport, uint32_t val);
uint32_t adlib_read(void *opaque, uint32_t nport);
AdlibState *adlib_new();
/* Every register back to zero, as when the machine is switched on. */
void adlib_power_on(AdlibState *s);

/* The mixer, core 0: the next stereo frame, and how many are ready. */
void adlib_getframe(AdlibState *s, int *left, int *right);
uint32_t adlib_ready(AdlibState *s);

/* The synthesis core: applies due writes and renders one batch; returns
 * nonzero if there was anything to do, zero before adlib_new() or when the
 * ring is as far ahead as it may go. */
int adlib_synth_step(void);

#if ADLIB_OPL_LOG
/* The guest's OPL register writes, timed: see opl_log() in adlib.c.  Each
 * entry is two words, microseconds since the start and port<<8|val. */
void adlib_log_start(void);
uint32_t adlib_log_take(const uint32_t **entries);
#endif

#endif /* ADLIB_H */
