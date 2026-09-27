#pragma once
#include <circle/actled.h>
#include <circle/cputhrottle.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/multicore.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
extern "C" {
#include "../../../src/pc.h"
#include "../../../src/i8042.h"
}
#include "hal.h"
#include "audio_circle.h"
#include "ds3231.h"
#include "frontpanel.h"
extern "C" {
#include "../../../src/gmsynth.h"
}

/*
 * The other three cores.  Core 1 is the sound - the OPL3 and the mixer -
 * core 2 draws the display (pc_vga_core_run()), and core 3 plays the
 * General MIDI SoundFont (gmsynth_step()).
 */
class CSynthCores : public CMultiCoreSupport {
public:
    CSynthCores() : CMultiCoreSupport(CMemorySystem::Get()) {}
    void Run(unsigned nCore) override;
    /* Hands core 1 the machine to mix, once it exists. */
    void StartAudio(CircleAudio *audio, PC *pc);
private:
    CircleAudio *m_Audio = nullptr;
    PC *m_PC = nullptr;
};

class CKernel {
public:
    CKernel();
    boolean Initialize();
    void Run();
private:
    void LoadSoundFont();
    void StartClock();
    bool LoadConfig(PCConfig *config, char *storage, unsigned storage_size);
    void UpdateInput();
    void DrainInput();
    void DrainSerialKeys();
    void DumpExcLog();
    void DumpGuestMemory(uint32_t addr, unsigned len);
#if ADLIB_OPL_LOG
    void DumpOplLog(void);
#endif
    /* The guest switched itself off: stop the board.  Does not return. */
    void PowerOff(const char *why);
    void PowerOn();
    void SetBlank(bool blank);
    bool LoadPowerState();
    void SavePowerState(bool on);
    /* Win+F11 and Win+F12 open the menus.  Returns true when the key
     * belongs to a menu and must not reach the guest. */
    bool HandleUiKey(bool down, unsigned char code);
    void QueueKey(bool down, unsigned char keycode);
    void QueueMouse(unsigned buttons, int dx, int dy, int wheel);
    static void KeyboardRaw(unsigned char modifiers, const unsigned char raw_keys[6], void *arg);
    static void MouseRaw(unsigned buttons, int dx, int dy, int wheel, void *arg);
    static void KeyboardRemoved(CDevice *device, void *context);
    static void MouseRemoved(CDevice *device, void *context);
    /*
     * Declaration order matters here, and Circle's own sample 26-cpustress
     * carries the same comment.  CCPUThrottle's constructor calls
     * CKernelOptions::Get(), so the options object has to exist first; with
     * no CKernelOptions member that call returns a null pointer and faults
     * during CKernel's construction, before the logger exists to report it.
     * The board then sits in halt() showing the firmware's colour splash
     * with a silent UART, which is exactly what it did.
     */
    CActLED m_LED;
    CKernelOptions m_Options;
    CDeviceNameService m_Devices;
    /* Without this the SoC stays at CPUSpeedLow, which is half the Pi 3's
     * rated clock.  It needs Update() called regularly so the thermal
     * limiter can act; the guest loop does that once a second. */
    CCPUThrottle m_CPUThrottle;
    CScreenDevice m_Screen;
    CExceptionHandler m_Exceptions;
    /*
     * The serial device stays in polling mode, deliberately.
     *
     * Polling means Write() spins on the transmit FIFO, so a long log line is
     * dead time for the whole loop; with diagnostics printing a few hundred
     * characters a second that exceeded the audio queue's depth and broke the
     * sound up once per line.  Interrupt-driven mode fixes that and was tried,
     * and it cost far more than it saved: halt() masks interrupts, so the
     * panic message an exception handler writes on its way down stays in the
     * transmit buffer and is never sent.  A crash then leaves a silent UART
     * and nothing to go on.
     *
     * Blocking writes are the right trade here because the periodic
     * diagnostics are off unless STATS=1 is asked for, and when they are on,
     * their cost is a known artefact rather than a mystery.
     */
    CInterruptSystem m_Interrupt;
    CSerialDevice m_Serial;
    unsigned m_SerialPoll;
    CTimer m_Timer;
    CLogger m_Log;
    CSynthCores m_Cores;
    CUSBHCIDevice m_USB;
    CircleAudio m_Audio;
    CDs3231 m_RTC;
    CFrontPanel m_Panel;
    bool m_PowerStateSaved = true;
    /* A press of a front-panel button sent over the serial link, for a rig
     * with nobody at the case; see DrainSerialKeys(). */
    unsigned char m_SerialPanel = 0;
    CEMMCDevice m_SD;
    FATFS m_FS;
    CirclePcVideo m_Video;
    PC *m_PC;
    CUSBKeyboardDevice * volatile m_Keyboard;
    CMouseDevice * volatile m_Mouse;
    bool m_USBReady;
    /* When the boot logo went up, so that it is given a moment before the
     * guest's first frame lands on top of it; zero if it never did. */
    uint64_t m_LogoShownAt = 0;
    /* Where the menu is drawn before it is shown, and whether it was open on
     * the pass before this one; see the main loop. */
    /* When DrainInput() last ran, so that a gap in it can cancel a repeat
     * whose release may have gone unheard; see there. */
    uint64_t m_LastDrain = 0;
    uint8_t *m_OsdCompose = nullptr;
    bool m_OsdWasOpen = false;
    /* Turns of the main loop that took longer than LongTurnUs, split by
     * where the time went.  The guest sees such a turn as the board
     * standing still - the timer has to drop what it could not deliver -
     * so the dump names the phase that held it. */
    static const unsigned LongTurnN = 16;
    static const unsigned LongTurnUs = 50000;
    struct LongTurn {
        uint32_t t_ms, total, input, step, after, vga, ide, write, sync, cycles, ip;
        uint16_t cs;
    };
    LongTurn m_LongTurns[LongTurnN] = {};
    unsigned m_LongTurnHead = 0;
    uint64_t m_NextUSBPoll;
    bool m_MetaDown;
    unsigned char m_LastModifiers;
    unsigned char m_LastRawKeys[6];

    struct KeyEvent { unsigned char down, code; };
    static constexpr unsigned KeyQueueSize = 64;
    volatile unsigned char m_KeyHead, m_KeyTail;
    KeyEvent m_KeyQueue[KeyQueueSize];
    /* Frame decoder for keystrokes arriving on the serial link. */
    unsigned char m_SerialKeyState;
    unsigned char m_SerialKeyDown;
    unsigned char m_RepeatKey;
    uint64_t m_NextKeyRepeat;

    struct MouseEvent { signed char dx, dy, wheel; unsigned char buttons; };
    static constexpr unsigned MouseQueueSize = 64;
    volatile unsigned char m_MouseHead, m_MouseTail;
    MouseEvent m_MouseQueue[MouseQueueSize];
};
