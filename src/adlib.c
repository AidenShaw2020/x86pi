/*
 * QEMU Proxy for OPL2/3 emulation by MAME team
 *
 * Copyright (c) 2004-2005 Vassili Karpov (malc)
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

#include <pico.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "adlib.h"
#include "audiodiag.h"
#include "nuked/opl3.h"
#include "audioclock.h"
/*
 * A ring of what the guest does to the OPL, for the host tools.
 *
 * AdLib detection is a fixed handshake - mask the timers, reset, read the
 * status, start timer 1, wait, read again - and a game that decides the card
 * is absent has seen one of those reads return the wrong thing.  The totals
 * cannot show that; the order can.  Lives in the PSRAM diagnostic block, so
 * it costs no SRAM.
 */
/*
 * On the Sound Blaster's own diagnostic page, and it records the *first*
 * events after the host zeroes the head rather than the last.
 *
 * A driver that fails its FM detection then sits in a poll loop forever, so
 * a wrap-around ring read afterwards holds nothing but thousands of
 * identical status reads - which is exactly what it looked like on Jones.
 * What decides the failure is the opening handshake, so the ring is armed
 * from the host just before the game is launched and stops when it is full.
 */
/*
 * Ordinary memory, not a fixed address - and see the same note in sb16.c.
 *
 * 0x110aa000 is PSRAM on the RP2350 and someone else's RAM anywhere else,
 * so on a Raspberry Pi this ring was writing into whatever happened to live
 * there.  sb16.c pointed its own diagnostic block at the identical address,
 * so the two also overwrote each other.  Nothing here reads the block back
 * to decide anything, which is why this one only corrupted memory instead
 * of crashing the machine the way the Sound Blaster's version did.
 */
#define OPL_DIAG_WORDS 1024u            /* OPL_DIAG_RING(360) + OPL_DIAG_N(512) */
static volatile uint32_t opl_diag_block[OPL_DIAG_WORDS];
#define OPL_DIAG opl_diag_block
#define OPL_DIAG_HEAD  352
#define OPL_DIAG_RING  360      /* 360..871: reg<<16 | val<<8 | isread */
#define OPL_DIAG_N     512
static inline void opl_diag_note(unsigned reg, unsigned val, unsigned isread)
{
    const uint32_t h = OPL_DIAG[OPL_DIAG_HEAD];
    if (h >= OPL_DIAG_N) return;
    OPL_DIAG[OPL_DIAG_RING + h] = (reg << 16) | ((val & 0xffu) << 8) | isread;
    OPL_DIAG[OPL_DIAG_HEAD] = h + 1u;
}


/*
 * The chip: a YMF262, synthesised by Nuked OPL3 on its own core.
 *
 * This was two emu8950 OPL2 instances, one per register bank, averaged into
 * mono on core 0 between guest instructions.  That could not do what an OPL3
 * adds - stereo, four-operator voices, waveforms 4-7 - and the linear
 * renderer it needed had no rhythm mode at all, so every game's drums were
 * dropped on the way to the chip.  Nuked OPL3 is exact, and at 24% of a Pi 3
 * core (measured on Canyon.mid, against 4.5% for the two OPL2s) it is too
 * expensive to share core 0 with the guest; core 1 has nothing else to do.
 */

/* Guest register writes on their way to the synthesis core: stamp[] is the
 * output frame each takes effect at, cmd[] is bank<<16 | reg<<8 | value.
 * Deep because a write waits there for the whole lead - ~17 ms - and a
 * driver clearing the chip writes hundreds of registers in far less. */
#define ADLIB_CMD_SLOTS 16384u
static uint32_t adlib_cmd_stamp[ADLIB_CMD_SLOTS];
static uint32_t adlib_cmd[ADLIB_CMD_SLOTS];
static uint32_t adlib_cmd_w;            /* written by core 0 */
static uint32_t adlib_cmd_r;            /* written by the synthesis core */

struct AdlibState {
    uint16_t adlibregmem[5];
    uint16_t adlib_register;    /* bit 8: bank 1 */
    uint8_t  adlibstatus;
    uint8_t  opl3_new;          /* NEW bit, register 0x105 */

    /* Stereo frames, left in the low half.  wpos and rpos are free-running
     * counters, masked on use: the synthesis core advances wpos, the mixer
     * rpos, each with a release store after the data it covers. */
    uint32_t ring[ADLIB_RING_FRAMES];
    uint32_t wpos;
    uint32_t rpos;
    uint32_t last;              /* mixer: held across an underrun */

    AudioClock clk;             /* see audioclock.h */
    int64_t  consumed;          /* frames the mixer has taken, ever */

    opl3_chip chip;             /* the synthesis core's alone */
    /* Its resampler: where the output sits between the chip samples old
     * and cur, in Q16, and the left sample held back one chip sample; see
     * adlib_render_frame(). */
    uint32_t rs_frac;
    int16_t  rs_old[2], rs_cur[2], rs_lprev;
};

_Static_assert((ADLIB_RING_FRAMES & (ADLIB_RING_FRAMES - 1u)) == 0,
               "ADLIB ring size must be a power of two");
_Static_assert(ADLIB_LEAD_SAMPLES < ADLIB_RING_FRAMES,
               "ADLIB lead must leave at least one free ring frame");

static AdlibState *g_adlib_live;

/* Register writes are stamped from the output clock; see audioclock.h. */
#define ADLIB_STAMP_LEAD AUDIOCLOCK_STAMP_LEAD(ADLIB_LEAD_SAMPLES)

#define ld_acq(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define st_rel(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* Wakes the synthesis core out of wfe when core 0 queues a write; it sleeps
 * whenever the ring is as far ahead as it may go. */
static inline void adlib_kick(void)
{
    __asm__ volatile ("dsb ish\n\tsev" ::: "memory");
}

/*
 * Register writes by kind, for the STATS line, so "the music is silent" can
 * be narrowed without guessing: a driver that is playing notes writes
 * key-ons constantly.
 */
uint32_t g_adlib_writes;
uint32_t g_adlib_w_keyon;   /* 0xB0-0xB8: key-on, block, F-number high */
uint32_t g_adlib_w_fnum;    /* 0xA0-0xA8: F-number low                 */
uint32_t g_adlib_w_level;   /* 0x40-0x55: KSL and total level          */
uint32_t g_adlib_w_env;     /* 0x20-0x35, 0x60-0x95: envelope shape    */
uint32_t g_adlib_w_other;   /* anything else                          */
/* The synthesis core's side: the loudest frame it produced, frames it ran
 * the chip for, and frames it wrote as silence without running it. */
uint32_t g_adlib_render_peak;
uint32_t g_adlib_rendered;
uint32_t g_adlib_skips;
/* The mixer's side: how far ahead the ring was at its last frame, frames
 * taken, and frames the ring had not got to yet. */
uint32_t g_adlib_fill;
uint32_t g_adlib_calls;
uint32_t g_adlib_underrun_total;

/*
 * Which OPL3 features the guest actually uses, per bank, for the STATS line:
 * waveform numbers written to E0-F5, key-ons, C0 writes carrying the stereo
 * bits, the last 0x104 four-operator mask and register 1 (WSE in bit 5).
 */
uint32_t g_opl3_ws[2][8], g_opl3_keyons[2], g_opl3_c0_lr[2], g_opl3_c0_cnt[2];
uint32_t g_opl3_4op_last, g_opl3_4op_nonzero, g_opl3_reg1_last;
volatile uint32_t g_opl3_bank1_writes;

static inline void opl3_note(int bank, unsigned reg, unsigned val)
{
#if !defined(CIRCLE_PC_STATS)
    (void)bank; (void)reg; (void)val;
#else
    g_adlib_writes++;
    if (reg >= 0xb0u && reg <= 0xb8u) g_adlib_w_keyon++;
    else if (reg >= 0xa0u && reg <= 0xa8u) g_adlib_w_fnum++;
    else if (reg >= 0x40u && reg <= 0x55u) g_adlib_w_level++;
    else if ((reg >= 0x20u && reg <= 0x35u) ||
             (reg >= 0x60u && reg <= 0x95u)) g_adlib_w_env++;
    else if (reg != 0xbdu) g_adlib_w_other++;
    if (bank) g_opl3_bank1_writes++;

    if (reg >= 0xe0u && reg <= 0xf5u) g_opl3_ws[bank][val & 7u]++;
    else if (reg >= 0xb0u && reg <= 0xb8u && (val & 0x20u)) g_opl3_keyons[bank]++;
    else if (reg >= 0xc0u && reg <= 0xc8u) {
        if (val & 0x30u) g_opl3_c0_lr[bank]++;
        if (val & 0x01u) g_opl3_c0_cnt[bank]++;
    } else if (bank == 0 && reg == 0x01u) g_opl3_reg1_last = val;
    else if (bank == 1 && reg == 0x04u) {
        g_opl3_4op_last = val;
        if (val & 0x3fu) g_opl3_4op_nonzero++;
    }
#endif
}

#if ADLIB_OPL_LOG
/*
 * Every register write the guest makes, with the time it made it, so the
 * same music can be rendered off the board by this chip model and by a
 * reference one.  Built with OPLLOG=1; started and read out over the serial
 * link (0xF9 and 0xF8, see DrainSerialKeys()).
 */
#define ADLIB_LOG_N 131072u
static uint32_t *g_opl_log;
static uint32_t g_opl_log_n, g_opl_log_on, g_opl_log_t0;

void adlib_log_start(void)
{
    if (!g_opl_log) g_opl_log = malloc(ADLIB_LOG_N * 2u * sizeof(uint32_t));
    g_opl_log_n = 0;
    g_opl_log_t0 = get_uticks();
    g_opl_log_on = g_opl_log != NULL;
}

uint32_t adlib_log_take(const uint32_t **entries)
{
    g_opl_log_on = 0;
    *entries = g_opl_log;
    return g_opl_log_n;
}

/* Raw port writes, port<<8|value: the replay decodes index and data itself,
 * so a port this file does not handle still shows up. */
static inline void opl_log(unsigned port, unsigned val)
{
    if (!g_opl_log_on || g_opl_log_n >= ADLIB_LOG_N) return;
    g_opl_log[2u * g_opl_log_n] = get_uticks() - g_opl_log_t0;
    g_opl_log[2u * g_opl_log_n + 1u] = ((port & 0xffffu) << 8) | (val & 0xffu);
    g_opl_log_n++;
}

/* A status read, logged as port | 0x8000 with the value returned.  The
 * delay loops read it dozens of times in a row, so a read the same as the
 * entry before it is left out. */
static inline void opl_log_read(unsigned port, unsigned val)
{
    const uint32_t e = (((port | 0x8000u) & 0xffffu) << 8) | (val & 0xffu);
    if (g_opl_log_n && g_opl_log[2u * g_opl_log_n - 1u] == e) return;
    opl_log(port | 0x8000u, val);
}
#else
static inline void opl_log(unsigned port, unsigned val)
{
    (void)port; (void)val;
}
static inline void opl_log_read(unsigned port, unsigned val)
{
    (void)port; (void)val;
}
#endif

/*
 * Queue one register write, stamped with the frame it takes effect at: see
 * ADLIB_STAMP_LEAD.
 *
 * Full means the synthesis core is behind by sixteen thousand writes; it
 * applies early rather than waiting once the queue is half full (see
 * adlib_synth_run), so this wait ends.  Dropping the write instead would
 * leave a wrong instrument sounding for as long as the note lasts.
 */
static void adlib_queue(AdlibState *s, unsigned reg16, unsigned val)
{
    const uint32_t w = adlib_cmd_w;
    while (w - ld_acq(&adlib_cmd_r) >= ADLIB_CMD_SLOTS)
        adlib_kick();
    uint32_t frame;
    if (!audioclock_read(&s->clk, &frame))
        frame = ld_acq(&s->rpos);
    adlib_cmd_stamp[w & (ADLIB_CMD_SLOTS - 1u)] = frame + ADLIB_STAMP_LEAD;
    adlib_cmd[w & (ADLIB_CMD_SLOTS - 1u)] = (reg16 << 8) | (val & 0xffu);
    st_rel(&adlib_cmd_w, w + 1u);
    adlib_kick();
}

/* Not a register: the chip as it is when power is applied.  Queued like a
 * write so that it lands in order with them, on the core that owns the chip. */
#define ADLIB_CMD_RESET 0x200u

void adlib_power_on(AdlibState *s)
{
    s->adlib_register = 0;
    adlib_queue(s, ADLIB_CMD_RESET, 0);
}

void adlib_write(void *opaque, uint32_t nport, uint32_t val)
{
    AdlibState *s = opaque;

    opl_log(nport, val);
    /*
     * One address latch for both banks, as on the YMF262.
     *
     * The chip is reachable at these port pairs:
     *
     *   0x388/0x389, 0x38A/0x38B  the AdLib ports, both banks
     *   0x220/0x221, 0x222/0x223  SB Pro / SB16 FM, both banks
     *   0x228/0x229               the Sound Blaster's AdLib-compatible FM
     *
     * An OPL3 has a single address register: writing an index port loads
     * it with the register number and, from which port it was, the bank.
     * The data ports are all the same port - a data write goes to whatever
     * address was loaded last, whichever pair it arrives through.
     *
     * This was modelled as two separate index registers, one per bank, with
     * each data port writing its own bank.  The Windows 95 Sound Blaster 16
     * FM driver selects bank 1 at 0x222 and writes the data at 0x221,
     * measured on the board: 4674 index writes at 0x222 and not one data
     * write at 0x223 in a minute of Canyon.mid.  So every bank-1 write -
     * half the music, and the OPL3 enable - landed in bank 0 at whatever
     * register 0x220 had last selected, overwriting live voices with values
     * meant for others.  That is the "sounds out of tune, like bad
     * synthesis" report, and not something the waveforms could fix.
     */
    switch (nport) {
        case 0x388: case 0x228: case 0x220:
            frank_diag_opl_write(nport, (uint8_t)val, (uint8_t)val, 0);
            s->adlib_register = (uint16_t)(val & 0xffu);
            return;
        case 0x38a: case 0x222:
            /* In OPL2 compatibility mode the chip drops the bank bit of
             * every address but 0x105, the register that leaves the mode
             * (ymfm, from tests on the real part). */
            s->adlib_register = (uint16_t)(val & 0xffu);
            if (s->opl3_new || (val & 0xffu) == 0x05)
                s->adlib_register |= 0x100u;
            return;
        case 0x389: case 0x229: case 0x221:
        case 0x38b: case 0x223:
            break;
        default:
            return;
    }

    const unsigned reg = s->adlib_register & 0xffu;
    if (s->adlib_register & 0x100u) {
        opl3_note(1, reg, val);
        if (reg == 0x05)
            s->opl3_new = val & 1u;
        adlib_queue(s, s->adlib_register, val);
        return;
    }

    frank_diag_opl_write(nport, reg, (uint8_t)val, 1);
    opl_diag_note(reg, (unsigned)val, 0);
    opl3_note(0, reg, val);
    /* Registers 2-4 are the timers, which live here rather than in the
     * chip: see adlib_read(). */
    if (reg <= 4)
        s->adlibregmem[reg] = (uint16_t)val;
    if (reg == 4 && (val & 0x80)) {
        s->adlibstatus = 0;
        s->adlibregmem[4] = 0;
    }
    adlib_queue(s, reg, val);
}

uint32_t adlib_read(void *opaque, uint32_t nport)
{
    AdlibState *s = opaque;
    switch (nport) {
        case 0x388: case 0x389:
        case 0x228: case 0x229:
        case 0x220: case 0x221:
            FRANK_DIAG_COUNT(opl_status);
            /*
             * Status from the timer control register, honouring the masks.
             *
             * Register 4 is: bit 0 start timer 1, bit 1 start timer 2, bit 5
             * mask timer 2, bit 6 mask timer 1, bit 7 reset the IRQ.  A
             * masked timer still runs but its flag never reaches the status
             * byte.  The old expression ignored the mask bits entirely and
             * derived the flags from the start bits alone, which happens to
             * satisfy the usual detection - 0x60, 0x80, read 0x00, 0x21,
             * read 0xC0 - and fails any other order.
             *
             * Fox does it differently: reg 1 <- 0x20, reg 4 <- 0x63, read.
             * 0x63 starts both timers and masks both, so the answer is 0x00;
             * the old code returned 0xE0, the game concluded there was no
             * AdLib and fell back to the PC speaker.
             */
            {
                const unsigned tc = s->adlibregmem[4];
                unsigned st = 0;
                if ((tc & 0x01u) && !(tc & 0x40u)) st |= 0x40u;  /* timer 1 */
                if ((tc & 0x02u) && !(tc & 0x20u)) st |= 0x20u;  /* timer 2 */
                if (st) st |= 0x80u;                             /* IRQ */
                s->adlibstatus = (uint8_t)st;
            }
            opl_diag_note(0xfe, s->adlibstatus, 1);
            opl_log_read(nport, s->adlibstatus);
            return s->adlibstatus;
    }
    return 0xFF;
}

AdlibState *adlib_new()
{
    AdlibState *s = malloc(sizeof(AdlibState));
    if (!s) return NULL;
    memset(s, 0, sizeof(AdlibState));
    /* The chip's own rate: the resampling is adlib_render_frame()'s. */
    OPL3_Reset(&s->chip, 49716);
    adlib_cmd_r = adlib_cmd_w = 0;
    st_rel(&g_adlib_live, s);
    adlib_kick();
    return s;
}

uint32_t adlib_ready(AdlibState *s)
{
    return ld_acq(&s->wpos) - s->rpos;
}

void adlib_getframe(AdlibState *s, int *left, int *right)
{
    const uint32_t r = s->rpos;
    const uint32_t w = ld_acq(&s->wpos);
    g_adlib_calls++;
    g_adlib_fill = w - r;
    uint32_t f;
    if (r == w) {
        /* Hold the last frame rather than returning to zero: an underrun in
         * the middle of a sampled stream carries a large DC offset, and
         * snapping that to silence and back is a click far louder than the
         * gap it fills. */
        g_adlib_underrun_total++;
        f = s->last;
    } else {
        f = s->ring[r & (ADLIB_RING_FRAMES - 1u)];
        s->last = f;
        st_rel(&s->rpos, r + 1u);
        if ((++s->consumed & 63) == 0)
            audioclock_steer(&s->clk, s->consumed);
        /* No kick: the mixer runs on the synthesis core itself, which
         * loops round to render as soon as this pass ends. */
    }
    *left = (int16_t)(f & 0xffffu);
    *right = (int16_t)(f >> 16);
}

/*
 * The synthesis core.
 *
 * Renders until the ring is ADLIB_LEAD_SAMPLES ahead of the mixer, stopping
 * at each queued write's stamp to apply it first.  One call does one batch
 * and says whether there was anything to do; core 1 calls it in a loop with
 * the mixer and sleeps when neither has work.  The pacing comes from the
 * mixer's own index, not from a clock: the mixer *is* the output clock, so
 * this cannot drift against it.
 *
 * After a second of silence with nothing written the chip is not run at
 * all, and the ring is filled with zeros instead.  Every voice has decayed
 * by then, and only the free-running LFO and envelope timers go unadvanced,
 * which nothing can hear; the core then costs next to nothing while a game
 * is not using FM.
 */
#define ADLIB_QUIET_FRAMES ((uint32_t)SOUND_FREQUENCY)
#define ADLIB_RENDER_MAX   64u

/*
 * One output frame from the chip: its 49716 Hz samples, linearly
 * interpolated to the output rate, with the two channels lined up.
 *
 * Nuked OPL3 produces the right output one chip sample after the left -
 * L[n] equals R[n+1] exactly, for a tone in both - which the real card's
 * DAC does not: a recording of a real OPL3 has its channels the same.
 * Heard in stereo that is 20 us and nothing; summed to mono, as any capture
 * or single speaker does, it is a comb that takes 1.9 dB off 10 kHz.  So the
 * left sample is held back one.
 *
 * Nuked's own resampler did the interpolation before, with a ratio kept in
 * whole 1/1024ths - every note 0.036% sharp.  This one steps in Q16.
 */
#define ADLIB_RS_STEP ((uint32_t)((49716ull * 65536u + SOUND_FREQUENCY / 2u) / SOUND_FREQUENCY))

static void adlib_render_frame(AdlibState *s, int16_t st[2])
{
    s->rs_frac += ADLIB_RS_STEP;
    while (s->rs_frac >= 0x10000u) {
        s->rs_frac -= 0x10000u;
        int16_t b[2];
        OPL3_Generate(&s->chip, b);
        s->rs_old[0] = s->rs_cur[0];
        s->rs_old[1] = s->rs_cur[1];
        s->rs_cur[0] = s->rs_lprev;
        s->rs_cur[1] = b[1];
        s->rs_lprev = b[0];
    }
    const int64_t f = (int64_t)s->rs_frac;
    st[0] = (int16_t)(s->rs_old[0] + (((int64_t)(s->rs_cur[0] - s->rs_old[0]) * f) >> 16));
    st[1] = (int16_t)(s->rs_old[1] + (((int64_t)(s->rs_cur[1] - s->rs_old[1]) * f) >> 16));
}

/* The synthesis core's own state across calls.  Key-on bits as last
 * written: B0-B8 per bank, and the rhythm bits of 0xBD; see the key-off rule
 * below. */
static uint32_t g_synth_quiet;
static uint8_t g_synth_keyed[2][9];
static uint8_t g_synth_rhythm;

int adlib_synth_step(void)
{
    AdlibState *s = ld_acq(&g_adlib_live);
    if (!s) return 0;
    uint32_t quiet = g_synth_quiet;
    uint8_t (*const keyed)[9] = g_synth_keyed;
    uint8_t rhythm = g_synth_rhythm;
    int work = 0;
    {
        const uint32_t w = s->wpos;
        const uint32_t cw = ld_acq(&adlib_cmd_w);
        uint32_t cr = adlib_cmd_r;

        /* Due writes, in order.  Past half full the queue is drained
         * without waiting for the stamps, so a guest that outruns the
         * lead can never wedge core 0 in adlib_queue().
         *
         * A write that releases a key ends the batch: the chip has to run
         * at least one sample with the key up, or it never sees it go up.
         * Nuked OPL3 samples the key once per chip sample, as the chip
         * does, and a key-off and the next key-on in the same frame are no
         * key-off at all - the note is not restarted.  Tyrian writes them
         * 55 us apart (the median; 89% of its notes under 100 us, from its
         * register log), two or three output frames, and a guest running
         * faster than the real machine could put them in one. */
        while (cr != cw) {
            const uint32_t i = cr & (ADLIB_CMD_SLOTS - 1u);
            if ((int32_t)(adlib_cmd_stamp[i] - w) > 0 &&
                cw - cr < ADLIB_CMD_SLOTS / 2u)
                break;
            const uint32_t c = adlib_cmd[i];
            if ((c >> 8) == ADLIB_CMD_RESET) {
                OPL3_Reset(&s->chip, 49716);
                memset(keyed, 0, sizeof g_synth_keyed);
                rhythm = 0;
                cr++;
                work = 1;
                continue;
            }
            const unsigned bank = (c >> 16) & 1u, reg = (c >> 8) & 0xffu;
            const unsigned val = c & 0xffu;
            OPL3_WriteReg(&s->chip, (uint16_t)(c >> 8), (uint8_t)val);
            quiet = 0;
            cr++;
            int released = 0;
            if (reg >= 0xb0u && reg <= 0xb8u) {
                const uint8_t on = (val & 0x20u) != 0;
                released = keyed[bank][reg - 0xb0u] && !on;
                keyed[bank][reg - 0xb0u] = on;
            } else if (reg == 0xbdu && !bank) {
                const uint8_t on = (val & 0x20u) ? (uint8_t)(val & 0x1fu) : 0;
                released = (rhythm & ~on) != 0;
                rhythm = on;
            }
            work = 1;
            if (released)
                break;
        }
        st_rel(&adlib_cmd_r, cr);

        const uint32_t target = ld_acq(&s->rpos) + ADLIB_LEAD_SAMPLES;
        if ((int32_t)(target - w) <= 0)
            goto out;
        uint32_t end = target;
        if (cr != cw) {
            /* At least one frame before the next write, which may already
             * be due if a key-off ended the batch above.  One output frame
             * runs the chip for at least one of its own samples. */
            uint32_t stamp = adlib_cmd_stamp[cr & (ADLIB_CMD_SLOTS - 1u)];
            if ((int32_t)(stamp - (w + 1u)) < 0) stamp = w + 1u;
            if ((int32_t)(stamp - end) < 0) end = stamp;
        }
        uint32_t n = end - w;
        if (n > ADLIB_RENDER_MAX) n = ADLIB_RENDER_MAX;

        if (quiet >= ADLIB_QUIET_FRAMES) {
            for (uint32_t k = 0; k < n; k++)
                s->ring[(w + k) & (ADLIB_RING_FRAMES - 1u)] = 0;
            g_adlib_skips += n;
        } else {
            for (uint32_t k = 0; k < n; k++) {
                int16_t st[2];
                adlib_render_frame(s, st);
                /* At the chip's own level.  This was halved to match what
                 * the two averaged OPL2 banks gave in OPL3 mode, but an OPL2
                 * game ran one bank at full level - so its music came out
                 * about 7 dB under the Sound Blaster's effects (Tyrian's
                 * FM peaks: 10-12k before, 5k after), and the user heard it
                 * as flat and dull. */
                const int32_t l = st[0], r = st[1];
                s->ring[(w + k) & (ADLIB_RING_FRAMES - 1u)] =
                    (uint32_t)(uint16_t)l | ((uint32_t)(uint16_t)r << 16);
                if (l | r) {
                    quiet = 0;
                    const uint32_t m = (uint32_t)(l < 0 ? -l : l);
                    if (m > g_adlib_render_peak) g_adlib_render_peak = m;
                } else {
                    quiet++;
                }
            }
            g_adlib_rendered += n;
        }
        st_rel(&s->wpos, w + n);
        work = 1;
    }
out:
    g_synth_quiet = quiet;
    g_synth_rhythm = rhythm;
    return work;
}
