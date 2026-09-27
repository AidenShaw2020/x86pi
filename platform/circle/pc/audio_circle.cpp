#include "audio_circle.h"
extern "C" int g_audio_volume;
#include <circle/timer.h>

extern "C" {
#include "../../../src/adlib.h"
#include "../../../src/gmsynth.h"
#include "../../../src/dss.h"
#include "../../../src/pcspk.h"
#include "../../../src/sb16.h"
}

CircleAudio::CircleAudio(CInterruptSystem *interrupt)
: m_Analog(interrupt, SampleRate, 1024u), m_HDMI(interrupt, SampleRate, 384u * 2u),
  m_AnalogReady(false), m_HDMIReady(false),
  m_DSSPhase(0), m_DSSValue(0), m_CovoxDC(0),
  m_Log(nullptr), m_NextStat(0), m_StatAnalogFrames(0), m_StatHDMIFrames(0),
  m_StatRendered(0), m_StatPeak(0), m_StatAdlibPeak(0), m_StatSbPeak(0),
  m_Dump{}, m_DumpCount(0), m_DumpDone(false), m_DumpAfter(0) {}

static bool start_sink(CSoundBaseDevice &device, CLogger &log,
                       const char *name, unsigned queue_ms, unsigned rate)
{
    if (!device.AllocateQueue(queue_ms)) {
        log.Write("AUDIO", LogWarning, "%s: queue allocation failed", name);
        return false;
    }
    device.SetWriteFormat(SoundFormatSigned16, 2);
    if (!device.Start()) {
        log.Write("AUDIO", LogWarning, "%s: start failed", name);
        return false;
    }
    log.Write("AUDIO", LogNotice, "%s stereo started at %u Hz, %u ms queue",
              name, rate, queue_ms);
    return true;
}

bool CircleAudio::Initialize(CLogger &log)
{
    /* Both sinks are optional and independent.  A board with no HDMI audio
     * sink, or one whose analogue jack is not wired, still gets sound from
     * the other; only losing both leaves the guest silent. */
    m_Log = &log;
    /* Start capturing once a guest has had time to make a noise. */
    m_DumpAfter = CTimer::GetClockTicks64() + 30000000u;
    m_NextStat = CTimer::GetClockTicks64() + 10000000u;
    m_AnalogReady = start_sink(m_Analog, log, "analogue (3.5mm)", QueueMillis, SampleRate);
    m_HDMIReady = start_sink(m_HDMI, log, "HDMI", QueueMillis, SampleRate);
    if (!m_AnalogReady && !m_HDMIReady) {
        log.Write("AUDIO", LogError, "no audio sink came up");
        return false;
    }
    return true;
}

static inline int clamp_audio_sample(int value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return value;
}

bool CircleAudio::WriteTo(CSoundBaseDevice &device, bool &ready, unsigned frames)
{
    if (!ready || !frames) return ready;
    const unsigned capacity = device.GetQueueSizeFrames();
    const unsigned queued = device.GetQueueFramesAvail();
    const unsigned room = capacity > queued ? capacity - queued : 0u;
    if (!room) return true;                 /* full: this device is running slow */
    const unsigned n = frames < room ? frames : room;
    const int bytes = (int)(n * sizeof m_Frames[0] * 2u);
    const int written = device.Write(m_Frames, bytes);
    if (written != bytes) ready = false;
    if (written > 0) {
        const unsigned accepted = (unsigned)written / (sizeof m_Frames[0] * 2u);
        if (&device == (CSoundBaseDevice *)&m_Analog) m_StatAnalogFrames += accepted;
        else m_StatHDMIFrames += accepted;
    }
    return ready;
}

void CircleAudio::ReportStats(int pc_adlib_on)
{
#if !defined(CIRCLE_PC_STATS)
    (void)pc_adlib_on;                 /* diagnostics are opt-in; see kernel.cpp */
    return;
#else
    if (!m_Log) return;
    const uint64_t now = CTimer::GetClockTicks64();
    if (now < m_NextStat) return;
    m_NextStat = now + 10000000u;
    m_Log->Write("AUDIO", LogNotice,
        "10s: rendered=%u peak=%d adlib_peak=%d sb_peak=%d adlib_on=%d | "
        "analogue %s frames=%u queue=%u/%u | HDMI %s frames=%u queue=%u/%u",
        m_StatRendered, m_StatPeak, m_StatAdlibPeak, m_StatSbPeak, pc_adlib_on,
        m_AnalogReady ? "ok" : "DEAD", m_StatAnalogFrames,
        m_Analog.GetQueueFramesAvail(), m_Analog.GetQueueSizeFrames(),
        m_HDMIReady ? "ok" : "DEAD", m_StatHDMIFrames,
        m_HDMI.GetQueueFramesAvail(), m_HDMI.GetQueueSizeFrames());
    m_StatRendered = 0; m_StatPeak = 0; m_StatAdlibPeak = 0; m_StatSbPeak = 0;
    m_StatAnalogFrames = 0; m_StatHDMIFrames = 0;
#endif
}

/*
 * How many frames to render is a question for the output queue, not for the
 * clock.
 *
 * This used to measure the real time since the previous call and render that
 * many frames, capped, with the remainder thrown away.  Both halves of that
 * are wrong on this target.  A queue is drained by its hardware at exactly
 * SampleRate, so wall-clock accounting only duplicates what the hardware
 * already knows - and it duplicates it from a task that is regularly
 * suspended inside a blocking SD read, which is when the timing is least
 * trustworthy.  Worse, the cap and the discarded remainder made the deficit
 * permanent: a stall long enough to drain the queue could only ever be
 * followed by writes worth the real time that had passed since the last call,
 * never more.  So the queue never climbed back to its starting depth and the
 * stream spent the rest of the session riding the underrun edge.
 *
 * Free space in the queue is the whole answer.  Keeping it full produces
 * SampleRate frames per second on average, by construction; after a stall the
 * following calls refill it a bounded chunk at a time.  Every device in the
 * mixer below advances its own phase by one frame per call - see
 * pcspk_sample() - so rendering in larger batches changes the pitch of
 * nothing.
 *
 * Two sinks, one stream, two crystals.  The analogue jack and HDMI each
 * derive 44.1 kHz from their own clock, so they cannot both be kept exactly
 * full from a single rendered buffer.  Pacing from the slower of the two
 * would starve the faster permanently, and pacing from the faster would make
 * the slower drop frames on every write.  So the analogue jack paces, because
 * that is the output someone is listening on, and HDMI takes as much of the
 * same buffer as it has room for.  The drift between two nominal 44.1 kHz
 * clocks is parts per million, so HDMI absorbs it in its 50 ms of queue and
 * gives up at most a few frames an hour.
 */
void CircleAudio::Pump(PC *pc)
{
    if (!pc) return;
    /* Before the early returns below: a pacer that has stopped must still be
     * visible in the log, otherwise the symptom is indistinguishable from a
     * guest that is simply not making any sound. */
    ReportStats(pc->adlib_enabled ? 1 : 0);
    CSoundBaseDevice &pacer = m_AnalogReady ? (CSoundBaseDevice &)m_Analog
                                            : (CSoundBaseDevice &)m_HDMI;
    bool &pacer_ready = m_AnalogReady ? m_AnalogReady : m_HDMIReady;
    if (!pacer_ready || !pacer.IsActive()) return;

    const unsigned capacity = pacer.GetQueueSizeFrames();
    const unsigned queued = pacer.GetQueueFramesAvail();
    if (queued >= capacity) return;
    unsigned due = capacity - queued;
    if (due > FramesPerPump) due = FramesPerPump;
    /*
     * No more than the OPL has ready.
     *
     * It is rendered on core 1, ADLIB_LEAD_SAMPLES ahead of this, and the
     * queue frees in blocks of up to a thousand frames: taking a whole block
     * at once ran the OPL's ring dry, measured at 1000-1600 frames per ten
     * seconds of Tyrian, each one held at the last value - a click.  Taking
     * fewer only means another pass sooner; the queue holds 50 ms and core 1
     * renders about four times faster than real time.
     */
    if (pc->adlib_enabled) {
        const uint32_t ready = adlib_ready(pc->adlib);
        if (due > ready) due = ready;
        if (!due) return;
    }
    /*
     * No more than the Sound Blaster has been given, while it is playing and
     * the output still has something in hand.
     *
     * A pass renders up to FramesPerPump frames in one go, and the card can
     * only have what the DMA has copied - which for a single-cycle transfer
     * is at most half a block ahead of playback.  Prehistorik 2 plays 168-byte
     * blocks at 8403 Hz: 20 ms, ten of them in the ring.  One pass of 512
     * frames wanted twelve, so every block ran dry part-way through,
     * stretched from 20 ms to 23, and its completion interrupt came only
     * once the ring was empty - a hole at every block, 43 a second, heard
     * as a loud buzz over the game's music.  Taking only what is there lets
     * core 0 top the ring up between passes and the interrupt arrive while
     * the tail is still playing, as it is meant to.  Below a quarter of the
     * queue the output comes first.
     */
    if (pc->sb16_enabled && queued > capacity / 4) {
        const uint32_t ready = sb16_frames_ready(pc->sb16);
        if (due > ready) due = ready;
        if (!due) return;
    }
    /* The same for the SoundFont synthesiser on core 3. */
    if (pc->mpu401_enabled && gmsynth_active()) {
        const uint32_t ready = gmsynth_ready();
        if (due > ready) due = ready;
        if (!due) return;
    }

    for (unsigned i = 0; i < due; ++i) {
        int left = 0, right = 0;
        if (pc->pcspk_enabled) {
            /*
             * Band-limited, and added rather than assigned.
             *
             * This used to test the naive generator and slam the output to a
             * fixed level, which threw away the shape of the wave and let its
             * harmonics above half the sample rate fold back into the audible
             * band - the rasp that some notes had and others did not.  See
             * pcspk_sample() for what replaces it.  Assigning also
             * discarded whatever the other devices had contributed, which was
             * only invisible while the speaker was the loudest thing here.
             */
            const int sample = pcspk_sample(pc->pcspk);
            left += sample; right += sample;
        }
        if (pc->covox_enabled) {
            /*
             * The Covox is a latch on the LPT data port and holds whatever
             * was written to it until something writes again.  The BIOS
             * detects a parallel port by writing AAh and reading it back, and
             * covox_sample starts at 0, so with nothing playing this channel
             * contributed a constant offset to every frame.  On real hardware
             * the DAC's output capacitor removes exactly that; model the
             * capacitor by tracking the latch's own slow mean and sending
             * only what moves relative to it.  Real Covox playback is a
             * sample stream well above the corner (about 14 Hz at a 512 frame
             * time constant) and passes through untouched.
             *
             * The RP2350 mixer in src/main.c sums the same expression and has
             * the same offset; it is left alone because only this target can
             * be measured today.
             */
            const int latch = ((int)pc->covox_sample - 127) << 8;
            m_CovoxDC += ((latch << 8) - m_CovoxDC) >> 9;
            const int sample = latch - (m_CovoxDC >> 8);
            left += sample; right += sample;
        }
        if (pc->tandy_enabled) {
            const int sample = sn76489_sample();
            left += sample; right += sample;
        }
        if (pc->mpu401_enabled) {
            if (gmsynth_active()) {
                int l, r;
                gmsynth_getframe(&l, &r);
                left += l; right += r;
            } else {
                const int sample = midi_sample();
                left += sample; right += sample;
            }
        }
        if (pc->adlib_enabled) {
            int l, r;
            adlib_getframe(pc->adlib, &l, &r);
            const int mag = (l < 0 ? -l : l) > (r < 0 ? -r : r)
                          ? (l < 0 ? -l : l) : (r < 0 ? -r : r);
            if (mag > m_StatAdlibPeak) m_StatAdlibPeak = mag;
            left += l; right += r;
        }
        if (pc->sb16_enabled) {
            /* What the card contributes on its own, so a mix that clips can
             * be attributed rather than guessed at. */
            const int before = left;
            sb16_getsample(pc->sb16, &right, &left);
            const int d = left - before;
            const int mag = d < 0 ? -d : d;
            if (mag > m_StatSbPeak) m_StatSbPeak = mag;
        }
        if (pc->dss_enabled) {
            m_DSSPhase += 7000u;
            if (m_DSSPhase >= SampleRate) {
                m_DSSPhase -= SampleRate;
                m_DSSValue = dss_sample();
            }
            left += m_DSSValue; right += m_DSSValue;
        }
        /* One master level for everything, set from the settings menu.
         * Sixteen is unity, so a machine that never opens the menu mixes
         * exactly as it did before there was one. */
        if (g_audio_volume != 16) {
            left  = (left  * g_audio_volume) >> 4;
            right = (right * g_audio_volume) >> 4;
        }
        const int l = clamp_audio_sample(left);
        const int r = clamp_audio_sample(right);
        if (m_DumpCount < DumpFrames &&
            CTimer::GetClockTicks64() >= m_DumpAfter && (l || r)) {
            m_Dump[m_DumpCount++] = (int16_t)l;
        }
        m_Frames[i * 2] = (int16_t)l;
        m_Frames[i * 2 + 1] = (int16_t)r;
        const int mag = (l < 0 ? -l : l) > (r < 0 ? -r : r)
                      ? (l < 0 ? -l : l) : (r < 0 ? -r : r);
        if (mag > m_StatPeak) m_StatPeak = mag;
    }
    m_StatRendered += due;

#if defined(CIRCLE_PC_STATS)
    if (m_DumpCount >= DumpFrames && m_Log) {
        for (unsigned i = 0; i < DumpFrames; i += 16) {
            char line[16 * 8 + 1];
            int n = 0;
            for (unsigned j = 0; j < 16; ++j)
                n += snprintf(line + n, (int)(sizeof line) - n, "%d ", m_Dump[i + j]);
            m_Log->Write("WAVE", LogNotice, "%03u: %s", i, line);
        }
        /* Re-arm rather than stop: the fault is intermittent, so one window
         * into the output is not enough to catch it. */
        m_DumpCount = 0;
        m_DumpAfter = CTimer::GetClockTicks64() + 15000000u;
    }
#endif
    WriteTo(m_Analog, m_AnalogReady, due);
    WriteTo(m_HDMI, m_HDMIReady, due);
}
