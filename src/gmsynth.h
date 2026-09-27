#ifndef GMSYNTH_H
#define GMSYNTH_H

#include <stdint.h>

/*
 * General MIDI behind the MPU-401: a SoundFont played by TinySoundFont on a
 * core of its own.
 *
 * Core 0 hands over each complete MIDI message as the guest writes it,
 * stamped with the output frame it takes effect at.  The synthesis core
 * renders a stereo ring and applies each message when its rendering reaches
 * that frame; the mixer drains the ring.  Without a SoundFont loaded none of
 * this runs and the MPU-401 keeps its built-in sine-wave synthesiser.
 */

/* Core 0, at start: parse a SoundFont held in memory.  The buffer can be
 * freed afterwards.  Returns nonzero when it loaded. */
int gmsynth_load(const void *sf2, uint32_t size);
int gmsynth_active(void);

/* Core 0: one MIDI byte as the guest sent it to the MPU-401's data port. */
void gmsynth_byte(uint8_t b);
/* Every note off and every controller back, as when the machine is switched on. */
void gmsynth_power_on(void);

/* The mixer: the next stereo frame, and how many are ready. */
void gmsynth_getframe(int *left, int *right);
uint32_t gmsynth_ready(void);

/* The synthesis core: apply due messages and render one batch; nonzero if
 * there was anything to do. */
int gmsynth_step(void);

#endif /* GMSYNTH_H */
