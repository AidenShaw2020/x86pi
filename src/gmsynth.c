/*
 * General MIDI: a SoundFont played by TinySoundFont on its own core.
 * See gmsynth.h.
 *
 * The MPU-401 used to drive a synthesiser of sine waves and noise, which is
 * what every game set to "General MIDI" or "Roland" played through.  This
 * plays the SoundFont on the SD card instead, on core 3, which had nothing
 * to do.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "gmsynth.h"
#include "audioclock.h"

/* TinySoundFont, with the few C library pieces it needs pointed at what this
 * target has: no stdio, and exp/log from the exp2/log2 in pc/libm.c. */
double pow(double, double), sqrt(double), tan(double), exp2(double), log2(double);
#define TSF_NO_STDIO
#define TSF_POW(x, y)  pow((x), (y))
#define TSF_POWF(x, y) ((float)pow((x), (y)))
#define TSF_EXPF(x)    ((float)exp2((x) * 1.4426950408889634))
#define TSF_LOG(x)     (log2(x) * 0.6931471805599453)
#define TSF_TAN(x)     tan(x)
#define TSF_LOG10(x)   (log2(x) * 0.30102999566398120)
#define TSF_SQRT(x)    sqrt(x)
#define TSF_SQRTF(x)   ((float)sqrt(x))
#define TSF_IMPLEMENTATION
#include "tsf/tsf.h"

/* Frames in the ring, and how far ahead of the mixer core 3 renders. */
#define GM_RING_FRAMES  2048u
#define GM_LEAD_FRAMES  1024u
#define GM_STAMP_LEAD   AUDIOCLOCK_STAMP_LEAD(GM_LEAD_FRAMES)
#define GM_RENDER_MAX   64u
/* Past this TinySoundFont cuts off a releasing voice - a click - or drops
 * the note.  Doom II sits at 20-30 but reached 111 at the start of its
 * title music; core 3 is 3-4% busy at those counts. */
#define GM_MAX_VOICES   128

_Static_assert((GM_RING_FRAMES & (GM_RING_FRAMES - 1u)) == 0, "power of two");
_Static_assert(GM_LEAD_FRAMES < GM_RING_FRAMES, "lead inside the ring");

/* Messages on their way to core 3: stamp[] is the output frame each takes
 * effect at, msg[] is status | data1 << 8 | data2 << 16, or GM_MSG_RESET. */
#define GM_CMD_SLOTS 4096u
#define GM_MSG_RESET 0x01000000u
static uint32_t gm_cmd_stamp[GM_CMD_SLOTS];
static uint32_t gm_cmd[GM_CMD_SLOTS];
static uint32_t gm_cmd_w;               /* core 0 */
static uint32_t gm_cmd_r;               /* core 3 */

static tsf *g_tsf;                      /* published by gmsynth_load() */
static uint32_t g_ring[GM_RING_FRAMES]; /* left in the low half */
static uint32_t g_wpos;                 /* core 3 */
static uint32_t g_rpos;                 /* the mixer */
static uint32_t g_last;
static int64_t g_consumed;
static AudioClock g_clk;

/* For the STATS line: the most voices sounding at once (TinySoundFont takes
 * one when all GM_MAX_VOICES are busy), the loudest output sample and how
 * many were clipped. */
uint32_t g_gm_voices_max, g_gm_peak, g_gm_clipped;

/* How loud the synthesiser is against the other sources, as a multiplier
 * on TinySoundFont's float output.  Half scale: a SoundFont played at its
 * own level with several voices clips, and the FM and the Sound Blaster
 * peak at about a third of full scale. */
#define GM_LEVEL 16384.0f

#define ld_acq(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define st_rel(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* Wakes the synthesis core out of wfe.  A no-op off the board, where the
 * offline check in debug/oplreplay builds this file. */
#if defined(__aarch64__)
#define gm_kick() __asm__ volatile ("dsb ish\n\tsev" ::: "memory")
#else
#define gm_kick() ((void)0)
#endif

static void gm_setup_channels(tsf *f)
{
    for (int ch = 0; ch < 16; ch++) {
        tsf_channel_set_bank(f, ch, 0);
        tsf_channel_set_presetnumber(f, ch, 0, ch == 9);
        tsf_channel_set_pitchrange(f, ch, 2.0f);
    }
}

int gmsynth_load(const void *sf2, uint32_t size)
{
    tsf *f = tsf_load_memory(sf2, (int)size);
    if (!f) return 0;
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, SOUND_FREQUENCY, 0.0f);
    /* Voices and channels allocated here, on core 0, so the synthesis core
     * never has to: past the limit the oldest voice is taken. */
    tsf_set_max_voices(f, GM_MAX_VOICES);
    gm_setup_channels(f);
    st_rel(&g_tsf, f);
    return 1;
}

int gmsynth_active(void)
{
    return ld_acq(&g_tsf) != NULL;
}

static void gm_queue(uint32_t msg)
{
    const uint32_t w = gm_cmd_w;
    while (w - ld_acq(&gm_cmd_r) >= GM_CMD_SLOTS)
        gm_kick();
    uint32_t frame;
    if (!audioclock_read(&g_clk, &frame))
        frame = ld_acq(&g_rpos);
    gm_cmd_stamp[w & (GM_CMD_SLOTS - 1u)] = frame + GM_STAMP_LEAD;
    gm_cmd[w & (GM_CMD_SLOTS - 1u)] = msg;
    st_rel(&gm_cmd_w, w + 1u);
    gm_kick();
}

/*
 * The MIDI byte stream, parsed on core 0.
 *
 * Running status is kept: after a channel message, data bytes alone repeat
 * it, and a MIDI driver sends a run of notes that way.  The MPU-401 code this
 * replaces restarted only on a status byte and dropped the rest.  Real-time
 * bytes (F8-FF) may arrive anywhere and change nothing.  Of system
 * exclusive, only the resets are acted on: GM System On and the Roland GS
 * reset, which a game sends before it starts and which should silence
 * everything and restore the default instruments.
 */
static uint8_t gm_status, gm_need, gm_have, gm_data[2];
static uint8_t gm_sysex[12], gm_sysex_n, gm_in_sysex;

static void gm_sysex_end(void)
{
    static const uint8_t gm_on[] = { 0x7E, 0x7F, 0x09, 0x01 };
    static const uint8_t gs_reset[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00 };
    if ((gm_sysex_n == sizeof gm_on && !memcmp(gm_sysex, gm_on, sizeof gm_on)) ||
        (gm_sysex_n >= sizeof gs_reset && !memcmp(gm_sysex, gs_reset, sizeof gs_reset)))
        gm_queue(GM_MSG_RESET);
}

void gmsynth_power_on(void)
{
    gm_in_sysex = 0;
    gm_status = 0;
    if (gmsynth_active()) gm_queue(GM_MSG_RESET);
}

void gmsynth_byte(uint8_t b)
{
    if (b >= 0xF8) return;
    if (b & 0x80) {
        if (gm_in_sysex) {
            gm_in_sysex = 0;
            if (b == 0xF7) { gm_sysex_end(); return; }
        }
        if (b == 0xF0) {
            gm_in_sysex = 1;
            gm_sysex_n = 0;
            gm_status = 0;
            return;
        }
        if (b >= 0xF0) {            /* system common: ends running status */
            gm_status = 0;
            return;
        }
        gm_status = b;
        gm_need = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
        gm_have = 0;
        return;
    }
    if (gm_in_sysex) {
        if (gm_sysex_n < sizeof gm_sysex) gm_sysex[gm_sysex_n] = b;
        if (gm_sysex_n < 255) gm_sysex_n++;
        return;
    }
    if (!gm_status) return;
    gm_data[gm_have++] = b;
    if (gm_have < gm_need) return;
    gm_have = 0;
    gm_queue((uint32_t)gm_status | ((uint32_t)gm_data[0] << 8) |
             (gm_need == 2 ? (uint32_t)gm_data[1] << 16 : 0));
}

static void gm_apply(tsf *f, uint32_t m)
{
    if (m == GM_MSG_RESET) {
        tsf_reset(f);
        for (int ch = 0; ch < 16; ch++)
            tsf_channel_midi_control(f, ch, 121, 0);
        gm_setup_channels(f);
        return;
    }
    const int ch = (int)(m & 0x0F);
    const int d1 = (int)((m >> 8) & 0x7F), d2 = (int)((m >> 16) & 0x7F);
    switch (m & 0xF0) {
    case 0x80: tsf_channel_note_off(f, ch, d1); break;
    case 0x90:
        if (d2) tsf_channel_note_on(f, ch, d1, (float)d2 / 127.0f);
        else tsf_channel_note_off(f, ch, d1);
        break;
    case 0xB0: tsf_channel_midi_control(f, ch, d1, d2); break;
    case 0xC0: tsf_channel_set_presetnumber(f, ch, d1, ch == 9); break;
    case 0xE0: tsf_channel_set_pitchwheel(f, ch, d1 | (d2 << 7)); break;
    default: break;             /* aftertouch: not modelled */
    }
}

int gmsynth_step(void)
{
    tsf *const f = ld_acq(&g_tsf);
    if (!f) return 0;
    int work = 0;
    const uint32_t w = g_wpos;
    const uint32_t cw = ld_acq(&gm_cmd_w);
    uint32_t cr = gm_cmd_r;
    /* Due messages, in order; past half full, all of them, so core 0 can
     * never wedge in gm_queue(). */
    while (cr != cw) {
        const uint32_t i = cr & (GM_CMD_SLOTS - 1u);
        if ((int32_t)(gm_cmd_stamp[i] - w) > 0 && cw - cr < GM_CMD_SLOTS / 2u)
            break;
        gm_apply(f, gm_cmd[i]);
        cr++;
        work = 1;
    }
    st_rel(&gm_cmd_r, cr);

    const uint32_t target = ld_acq(&g_rpos) + GM_LEAD_FRAMES;
    if ((int32_t)(target - w) <= 0) return work;
    uint32_t end = target;
    if (cr != cw) {
        uint32_t stamp = gm_cmd_stamp[cr & (GM_CMD_SLOTS - 1u)];
        if ((int32_t)(stamp - (w + 1u)) < 0) stamp = w + 1u;
        if ((int32_t)(stamp - end) < 0) end = stamp;
    }
    uint32_t n = end - w;
    if (n > GM_RENDER_MAX) n = GM_RENDER_MAX;

    float buf[2 * GM_RENDER_MAX];
    tsf_render_float(f, buf, (int)n, 0);
    {
        const uint32_t v = (uint32_t)tsf_active_voice_count(f);
        if (v > g_gm_voices_max) g_gm_voices_max = v;
    }
    for (uint32_t k = 0; k < n; k++) {
        int32_t l = (int32_t)(buf[2 * k] * GM_LEVEL);
        int32_t r = (int32_t)(buf[2 * k + 1] * GM_LEVEL);
        const uint32_t m = (uint32_t)((l < 0 ? -l : l) > (r < 0 ? -r : r) ? (l < 0 ? -l : l) : (r < 0 ? -r : r));
        if (m > g_gm_peak) g_gm_peak = m;
        if (m > 32767) g_gm_clipped++;
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        g_ring[(w + k) & (GM_RING_FRAMES - 1u)] =
            (uint32_t)(uint16_t)l | ((uint32_t)(uint16_t)r << 16);
    }
    st_rel(&g_wpos, w + n);
    return 1;
}

uint32_t gmsynth_ready(void)
{
    return ld_acq(&g_wpos) - g_rpos;
}

void gmsynth_getframe(int *left, int *right)
{
    const uint32_t r = g_rpos;
    uint32_t f;
    if (r == ld_acq(&g_wpos)) {
        f = g_last;
    } else {
        f = g_ring[r & (GM_RING_FRAMES - 1u)];
        g_last = f;
        st_rel(&g_rpos, r + 1u);
        if ((++g_consumed & 63) == 0)
            audioclock_steer(&g_clk, g_consumed);
    }
    *left = (int16_t)(f & 0xffffu);
    *right = (int16_t)(f >> 16);
}
