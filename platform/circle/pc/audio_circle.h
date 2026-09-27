#pragma once

#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/sound/pwmsoundbasedevice.h>

extern "C" {
#include "../../../src/pc.h"
}

/* Host-side sink for the unchanged tiny386 device mixer.  It is driven from
 * the guest task loop, never from a DMA callback, so emulated device state
 * remains single-threaded. */
class CircleAudio {
public:
    explicit CircleAudio(CInterruptSystem *interrupt);
    bool Initialize(CLogger &log);
    void Pump(PC *pc);

private:
    static constexpr unsigned SampleRate = 44100;
    /*
     * Output queue depth.  This is the only thing standing between a
     * blocking SD read and a hole in the stream: the guest task is what fills
     * the queue, and while FatFS is inside f_read() it fills nothing.  Fifty
     * milliseconds is roughly fourteen kilobytes of Circle heap per device on
     * a board with a gigabyte of it.
     */
    static constexpr unsigned QueueMillis = 50;
    /*
     * Frames rendered by one Pump() call.  It bounds the catch-up burst after
     * a stall, and it must stay at or below ADLIB_LEAD_SAMPLES: the OPL ring
     * is only refilled by pc_step(), so samples asked for beyond the lead come
     * back as the held last value rather than as music.  See ADLIB_NBUF and
     * ADLIB_LEAD_SAMPLES in platform/circle/Makefile.
     */
    static constexpr unsigned FramesPerPump = 512;

    bool WriteTo(CSoundBaseDevice &device, bool &ready, unsigned frames);
    void ReportStats(int pc_adlib_on);

    /* Analogue first: it is the pacing device, so it is also the one whose
     * queue decides how much gets rendered.  See the clock note in Pump(). */
    CPWMSoundBaseDevice m_Analog;
    CHDMISoundBaseDevice m_HDMI;
    bool m_AnalogReady;
    bool m_HDMIReady;
    int16_t m_Frames[FramesPerPump * 2];
    unsigned m_DSSPhase;
    int m_DSSValue;
    /*
     * Once-a-second counters, so a silent output can be told apart from a
     * silent guest without guessing.  Peak is the largest absolute sample the
     * mixer produced; the per-sink frame counts and queue depths say whether
     * those samples reached the hardware.
     */
    CLogger *m_Log;
    uint64_t m_NextStat;
    unsigned m_StatAnalogFrames;
    unsigned m_StatHDMIFrames;
    unsigned m_StatRendered;
    int m_StatPeak;
    int m_StatAdlibPeak;
    int m_StatSbPeak;
    /*
     * A one-shot capture of what the mixer really produces.
     *
     * Every measurement so far has come back through an HDMI grabber or a
     * sound card, and both resample; a square wave read through a resampler
     * rings around its edges whatever the source did, so neither can settle
     * a question about the shape of our own output.  This dumps the frames
     * as they leave the mixer, before any hardware sees them.
     */
    static constexpr unsigned DumpFrames = 256;
    int16_t m_Dump[DumpFrames];
    unsigned m_DumpCount;
    bool m_DumpDone;
    uint64_t m_DumpAfter;
    /* Q8 running mean of the Covox latch; see the DC note in Pump(). */
    int m_CovoxDC;
};
