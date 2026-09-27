#include "kernel.h"
#include <circle/bcmpropertytags.h>
#include <circle/netdevice.h>
#include <circle/usb/usbfloppydevice.h>
#include "../../../src/blockdev.h"
extern "C" {
#include "../../../src/sblog.h"
#include "../../../src/osd.h"
#include "../../../src/settingsui.h"
#include "../../../src/diskui.h"
#include "../../../src/config_save.h"
}
#include "circle_pc_diag.h"
#include <circle/memory.h>
#include <circle/startup.h>
#include "pmu.h"
#if ADLIB_OPL_LOG
extern "C" void adlib_log_start(void);
extern "C" uint32_t adlib_log_take(const uint32_t **entries);
#endif
#include <fatfs/ff.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
extern "C" {
#include "../../../src/jit/jit.h"
#include "bootlogo.h"
}

extern "C" uint64_t g_opcode_cycles[256];
extern "C" uint64_t g_xlat_cycles, g_xlat_calls, g_xlat_slow;
extern "C" uint32_t g_ide_activity;
extern "C" int g_audio_volume;
extern "C" uint64_t g_ide_us, g_ide_bytes, g_ide_calls,
                    g_ide_max_us, g_ide_seek_us, g_ide_seeks;


struct PcConfigStorage {
    char bios[128], vga_bios[128], fdd0[128], fdd1[128], ata0[128], ata1[128];
    char mem[24], vga_mem[24];
    int cpu, fpu, redirector;
};

static long parse_size(const char *s)
{
    if (!s || !s[0]) return 0;
    char *end = 0; long v = strtol(s, &end, 0);
    if (!end || end == s || v <= 0) return 0;
    if (*end == 'K' || *end == 'k') v *= 1024L;
    else if (*end == 'M' || *end == 'm') v *= 1024L * 1024L;
    else if (*end == 'G' || *end == 'g') v *= 1024L * 1024L * 1024L;
    return v;
}

static void copy_value(char *dst, size_t cap, const char *value)
{
    if (!dst || !cap) return;
    while (*value && isspace((unsigned char)*value)) ++value;
    size_t n = strlen(value);
    while (n && isspace((unsigned char)value[n-1])) --n;
    if (n >= cap) n = cap - 1;
    memcpy(dst, value, n); dst[n] = 0;
}

static int config_handler(void *user, const char *section, const char *name, const char *value)
{
    PcConfigStorage *s = (PcConfigStorage *) user;
    if (!section || !name || !value) return 1;
    if (!strcmp(name, "bios") && (!strcmp(section,"pc") || !strcmp(section,"386"))) copy_value(s->bios,sizeof s->bios,value);
    else if (!strcmp(name,"vga_bios")) copy_value(s->vga_bios,sizeof s->vga_bios,value);
    else if (!strcmp(name,"fda")) copy_value(s->fdd0,sizeof s->fdd0,value);
    else if (!strcmp(name,"fdb")) copy_value(s->fdd1,sizeof s->fdd1,value);
    else if (!strcmp(name,"hda")) copy_value(s->ata0,sizeof s->ata0,value);
    else if (!strcmp(name,"hdb")) copy_value(s->ata1,sizeof s->ata1,value);
    else if (!strcmp(name,"mem")) copy_value(s->mem,sizeof s->mem,value);
    else if (!strcmp(name,"vga_mem")) copy_value(s->vga_mem,sizeof s->vga_mem,value);
    else if (!strcmp(name,"cpu") || !strcmp(name,"gen")) s->cpu = atoi(value);
    else if (!strcmp(name,"fpu")) s->fpu = atoi(value) != 0;
    else if (!strcmp(name,"redirector")) s->redirector = atoi(value) != 0;
    /* "frank-386" is what the section was called before the project had its
       own name; cards written by an older build still say it. */
    else if (section && (!strcmp(section, "X86Pi") || !strcmp(section, "frank-386")))
        config_set_from_ini(name, value);
    return 1;
}

#if defined(CIRCLE_PC_STATS)
extern "C" uint32_t g_core_iter[4], g_core_wfe[4], g_core_wfe_us[4], g_core_work[4];
extern "C" uint32_t g_gm_voices_max, g_gm_peak, g_gm_clipped;
extern "C" uint32_t get_uticks();
#endif

void CSynthCores::StartAudio(CircleAudio *audio, PC *pc)
{
    __atomic_store_n(&m_Audio, audio, __ATOMIC_RELEASE);
    __atomic_store_n(&m_PC, pc, __ATOMIC_RELEASE);
    asm volatile ("dsb ish\n\tsev" ::: "memory");
}

/*
 * Core 1: the sound.  The OPL3 is rendered here, and the mixer that feeds
 * the audio devices runs here too.
 *
 * The mixer ran on core 0 between guest steps: 3-4% of core 0 in Tyrian's
 * demo, and whenever the guest stood still - a long disk read, a heavy frame
 * - the sound stood still with it.  Its sources are rings and latches the
 * guest's port writes fill: the OPL's own, the Sound Blaster's (which hands
 * the interrupts it wants raised back to core 0; see get_core_num()), the
 * MIDI synthesiser's voices and the DSS FIFO, both made safe for it.
 *
 * The audio DMA interrupts land on core 0 and wake nothing here, so the
 * generic timer's event stream wakes this core's wfe every 107 us; the
 * guest's OPL writes wake it too.
 *
 * The sleep clears this core's own event first (sevl, then a wfe that
 * returns at once).  Circle's spinlocks sev on release, and the mixer takes
 * the sound devices' locks on every pass, so without that the core woke
 * itself: 2.1 million turns a second, 0.5 s of every 10 asleep, the SoC at
 * 61 C and the CPU throttle halving the clock every other ten seconds.  An
 * event from core 0 that the clearing swallows is at most 107 us late.
 */
static inline void core_sleep(void)
{
    asm volatile ("sevl\n\twfe\n\twfe" ::: "memory");
}

/* Makes this core's wfe return every 107 us: the generic timer's event
 * stream on bit 10 of a 19.2 MHz counter. */
static void core_event_stream(void)
{
    uint64_t k;
    asm volatile ("mrs %0, cntkctl_el1" : "=r" (k));
    k = (k & ~0xF0ull) | (10ull << 4) | (1ull << 2);   /* EVNTI 10, EVNTEN */
    asm volatile ("msr cntkctl_el1, %0\n\tisb" :: "r" (k));
}
void CSynthCores::Run(unsigned nCore)
{
    if (nCore == 2) {
        pc_vga_core_run();
        return;
    }
    if (nCore == 3) {
        /* The SoundFont synthesiser; see gmsynth.c.  Idle until one is
         * loaded, which only happens at start. */
        core_event_stream();
        for (;;) {
            const int work = gmsynth_step();
#if defined(CIRCLE_PC_STATS)
            g_core_iter[3]++;
            if (work) g_core_work[3]++;
            if (!work) {
                const uint32_t t = get_uticks();
                g_core_wfe[3]++;
                core_sleep();
                g_core_wfe_us[3] += get_uticks() - t;
            }
#else
            if (!work)
                core_sleep();
#endif
        }
    }
    if (nCore != 1)
        return;
    core_event_stream();
    for (;;) {
        const int work = adlib_synth_step();
        CircleAudio *const audio = __atomic_load_n(&m_Audio, __ATOMIC_ACQUIRE);
        PC *const pc = __atomic_load_n(&m_PC, __ATOMIC_ACQUIRE);
        if (audio && pc)
            audio->Pump(pc);
#if defined(CIRCLE_PC_STATS)
        g_core_iter[1]++;
        if (work) g_core_work[1]++;
        if (!work) {
            const uint32_t t = get_uticks();
            g_core_wfe[1]++;
            core_sleep();
            g_core_wfe_us[1] += get_uticks() - t;
        }
#else
        if (!work)
            core_sleep();
#endif
    }
}

CKernel::CKernel()
: m_CPUThrottle(CPUSpeedMaximum), m_Screen(640, 480),
  m_Timer(&m_Interrupt),
  m_Log(LogDebug, &m_Timer),
  m_USB(&m_Interrupt, &m_Timer, TRUE), m_Audio(&m_Interrupt),
  m_SD(&m_Interrupt, &m_Timer, &m_LED),
  m_FS{}, m_Video{}, m_PC(nullptr), m_Keyboard(nullptr), m_Mouse(nullptr),
 m_USBReady(false), m_NextUSBPoll(0), m_LastModifiers(0), m_LastRawKeys{},
  m_MetaDown(false),
  m_SerialPoll(0),
  m_KeyHead(0), m_KeyTail(0), m_KeyQueue{}, m_SerialKeyState(0),
  m_SerialKeyDown(0), m_RepeatKey(0), m_NextKeyRepeat(0),
  m_MouseHead(0), m_MouseTail(0),
  m_MouseQueue{} {}

/* Circle gives USB usages while the 8042 model expects Linux/AT set-1
 * keycodes.  Keeping the conversion here lets all guest keyboard protocol,
 * typematic handling and IRQ delivery remain in the common emulator code. */
static unsigned char hid_to_linux_keycode(unsigned char code)
{
    switch (code) {
    case 0x04: return 30; case 0x05: return 48; case 0x06: return 46;
    case 0x07: return 32; case 0x08: return 18; case 0x09: return 33;
    case 0x0a: return 34; case 0x0b: return 35; case 0x0c: return 23;
    case 0x0d: return 36; case 0x0e: return 37; case 0x0f: return 38;
    case 0x10: return 50; case 0x11: return 49; case 0x12: return 24;
    case 0x13: return 25; case 0x14: return 16; case 0x15: return 19;
    case 0x16: return 31; case 0x17: return 20; case 0x18: return 22;
    case 0x19: return 47; case 0x1a: return 17; case 0x1b: return 45;
    case 0x1c: return 21; case 0x1d: return 44;
    case 0x1e: return 2;  case 0x1f: return 3;  case 0x20: return 4;
    case 0x21: return 5;  case 0x22: return 6;  case 0x23: return 7;
    case 0x24: return 8;  case 0x25: return 9;  case 0x26: return 10;
    case 0x27: return 11;
    case 0x28: return 28; case 0x29: return 1;  case 0x2a: return 14;
    case 0x2b: return 15; case 0x2c: return 57; case 0x2d: return 12;
    case 0x2e: return 13; case 0x2f: return 26; case 0x30: return 27;
    case 0x31: return 43; case 0x33: return 39; case 0x34: return 40;
    case 0x35: return 41; case 0x36: return 51; case 0x37: return 52;
    case 0x38: return 53; case 0x39: return 58;
    case 0x3a: return 59; case 0x3b: return 60; case 0x3c: return 61;
    case 0x3d: return 62; case 0x3e: return 63; case 0x3f: return 64;
    case 0x40: return 65; case 0x41: return 66; case 0x42: return 67;
    case 0x43: return 68; case 0x44: return 87; case 0x45: return 88;
    case 0x46: return 99; case 0x47: return 70; case 0x48: return 119;
    case 0x49: return 110; case 0x4a: return 102; case 0x4b: return 104;
    case 0x4c: return 111; case 0x4d: return 107; case 0x4e: return 109;
    case 0x4f: return 106; case 0x50: return 105; case 0x51: return 108;
    case 0x52: return 103; case 0x53: return 69;  case 0x54: return 98;
    case 0x55: return 55;  case 0x56: return 74;  case 0x57: return 78;
    case 0x58: return 96;  case 0x59: return 79;  case 0x5a: return 80;
    case 0x5b: return 81;  case 0x5c: return 75;  case 0x5d: return 76;
    case 0x5e: return 77;  case 0x5f: return 71;  case 0x60: return 72;
    case 0x61: return 73;  case 0x62: return 82;  case 0x63: return 83;
    case 0xe0: return 29;  case 0xe1: return 42;  case 0xe2: return 56;
    case 0xe3: return 125; case 0xe4: return 97;  case 0xe5: return 54;
    case 0xe6: return 100; case 0xe7: return 126;
    default: return 0;
    }
}

static bool hid_key_present(const unsigned char keys[6], unsigned char key)
{
    for (unsigned i = 0; i < 6; ++i) if (keys[i] == key) return true;
    return false;
}

void CKernel::QueueKey(bool down, unsigned char code)
{
    const unsigned char next = (unsigned char)((m_KeyHead + 1u) & (KeyQueueSize - 1u));
    if (next == m_KeyTail) return;       // Drop only a burst beyond 63 transitions.
    m_KeyQueue[m_KeyHead] = { (unsigned char)(down ? 1 : 0), code };
    __asm__ __volatile__("dmb ishst" ::: "memory");
    m_KeyHead = next;
}

void CKernel::QueueMouse(unsigned buttons, int dx, int dy, int wheel)
{
    /* Flight-sim convention, and a preference about this machine rather
     * than a property of the mouse, so it is applied here. */
    extern int g_mouse_invert_y;
    if (g_mouse_invert_y) dy = -dy;
    /*
     * Speed from the settings menu, in percent.  The part of a count that
     * does not make a whole one is carried to the next report, so a slow
     * setting still moves on a slow hand instead of rounding every small
     * movement away; and a fast one is split over as many packets as it
     * needs rather than clipped at the 127 a PS/2 packet can carry.
     */
    extern int g_mouse_speed;
    static int rem_x, rem_y;
    const int speed = g_mouse_speed > 0 ? g_mouse_speed : 100;
    const int sx = dx * speed + rem_x, sy = dy * speed + rem_y;
    dx = sx / 100; rem_x = sx - dx * 100;
    dy = sy / 100; rem_y = sy - dy * 100;
    if (wheel > 127) wheel = 127; else if (wheel < -127) wheel = -127;
    do {
        const unsigned char next = (unsigned char)((m_MouseHead + 1u) & (MouseQueueSize - 1u));
        if (next == m_MouseTail) return;
        const int px = dx > 127 ? 127 : dx < -127 ? -127 : dx;
        const int py = dy > 127 ? 127 : dy < -127 ? -127 : dy;
        m_MouseQueue[m_MouseHead] = { (signed char)px, (signed char)py,
                                       (signed char)wheel, (unsigned char)(buttons & 7u) };
        __asm__ __volatile__("dmb ishst" ::: "memory");
        m_MouseHead = next;
        dx -= px; dy -= py; wheel = 0;
    } while (dx || dy);
}

void CKernel::KeyboardRaw(unsigned char modifiers, const unsigned char raw_keys[6], void *arg)
{
    CKernel *self = static_cast<CKernel *>(arg);
    static const unsigned char modifier_usage[8] = { 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7 };
    const unsigned char changed = (unsigned char)(modifiers ^ self->m_LastModifiers);
    for (unsigned bit = 0; bit < 8; ++bit)
        if (changed & (1u << bit)) self->QueueKey((modifiers & (1u << bit)) != 0, modifier_usage[bit]);
    for (unsigned i = 0; i < 6; ++i)
        if (self->m_LastRawKeys[i] && !hid_key_present(raw_keys, self->m_LastRawKeys[i]))
            self->QueueKey(false, self->m_LastRawKeys[i]);
    for (unsigned i = 0; i < 6; ++i)
        if (raw_keys[i] && !hid_key_present(self->m_LastRawKeys, raw_keys[i]))
            self->QueueKey(true, raw_keys[i]);
    self->m_LastModifiers = modifiers;
    memcpy(self->m_LastRawKeys, raw_keys, 6);
}

void CKernel::MouseRaw(unsigned buttons, int dx, int dy, int wheel, void *arg)
{
    static_cast<CKernel *>(arg)->QueueMouse(buttons, dx, dy, wheel);
}

/*
 * A keyboard that goes away is a keyboard whose keys are no longer held.
 *
 * This used to forget the state and nothing else, which left every key that
 * happened to be down at that moment down for good: the guest never got the
 * break code, and the typematic here went on repeating that key at thirty a
 * second until some other key arrived to displace it.  What it looked like
 * was a DOS prompt printing itself down the screen for ever after a CD was
 * swapped - swapping one stalls the bus long enough for Circle to drop the
 * keyboard, and the key being held at the time was the Return that had just
 * run the command.
 *
 * So the keys are released on the way out, in the same queue as any other
 * key event, and the repeat goes with them.
 */
void CKernel::KeyboardRemoved(CDevice *, void *context)
{
    CKernel *self = static_cast<CKernel *>(context);
    static const unsigned char modifier_usage[8] = {
        0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7 };

    for (unsigned i = 0; i < 6; ++i)
        if (self->m_LastRawKeys[i]) self->QueueKey(false, self->m_LastRawKeys[i]);
    for (unsigned bit = 0; bit < 8; ++bit)
        if (self->m_LastModifiers & (1u << bit))
            self->QueueKey(false, modifier_usage[bit]);

    self->m_Keyboard = nullptr;
    self->m_LastModifiers = 0;
    memset(self->m_LastRawKeys, 0, sizeof self->m_LastRawKeys);
}

void CKernel::MouseRemoved(CDevice *, void *context)
{
    static_cast<CKernel *>(context)->m_Mouse = nullptr;
}

void CKernel::UpdateInput()
{
    if (!m_USBReady) return;
    const uint64_t now = CTimer::GetClockTicks64();
    if (now < m_NextUSBPoll) return;
    m_NextUSBPoll = now + 2000u; // USB topology polling at 500 Hz; reports remain interrupt driven.
    const boolean changed = m_USB.UpdatePlugAndPlay();
    const uint64_t t_pnp = CTimer::GetClockTicks64();
    if (changed && !m_Keyboard) {
        m_Keyboard = static_cast<CUSBKeyboardDevice *>(m_Devices.GetDevice("ukbd1", FALSE));
        if (m_Keyboard) {
            m_Keyboard->RegisterRemovedHandler(KeyboardRemoved, this);
            m_Keyboard->RegisterKeyStatusHandlerRaw(KeyboardRaw, FALSE, this);
            m_Log.Write("USB", LogNotice, "keyboard attached (ukbd1)");
        }
    }
    if (changed && !m_Mouse) {
        m_Mouse = static_cast<CMouseDevice *>(m_Devices.GetDevice("mouse1", FALSE));
        if (m_Mouse) {
            m_Mouse->RegisterRemovedHandler(MouseRemoved, this);
            m_Mouse->RegisterStatusHandler(MouseRaw, this);
            m_Log.Write("USB", LogNotice, "mouse attached: buttons=%u wheel=%u",
                        m_Mouse->GetButtonCount(), m_Mouse->HasWheel() ? 1u : 0u);
        }
    }
    const uint64_t t_leds0 = CTimer::GetClockTicks64();
    if (m_Keyboard) m_Keyboard->UpdateLEDs();
    const uint64_t t_leds1 = CTimer::GetClockTicks64();
    /* Both are USB transfers the board waits out, guest stopped; a slow one
     * is named here so a stall in the input phase has an owner. */
    if (t_pnp - now > 20000u || t_leds1 - t_leds0 > 20000u)
        m_Log.Write("USB", LogNotice, "slow input: plug-and-play %lu ms, LEDs %lu ms",
                    (unsigned long)((t_pnp - now) / 1000),
                    (unsigned long)((t_leds1 - t_leds0) / 1000));
}

/*
 * Keystrokes injected over the same serial link that carries the log.
 *
 * Nothing on this target ever read from the UART: the bootloader does, but
 * once the kernel is running the line is write-only.  That left every test
 * needing a person at the board's own keyboard to start a game or walk a
 * setup menu, which is most of what made a debugging round trip slow.
 *
 * The frame is deliberately trivial because there is exactly one writer and
 * no other traffic in this direction: FEh, then a 1 for press or 0 for
 * release, then the Linux/AT keycode - the same code hid_to_linux_keycode()
 * produces, so both paths hand ps2_put_keycode() identical values.  A byte
 * that is not FEh while idle is discarded, so line noise cannot be mistaken
 * for a keystroke, and a truncated frame resynchronises on the next FEh.
 */
bool CKernel::HandleUiKey(bool down, unsigned char code)
{
    if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) {
        m_MetaDown = down;
        /* Let the guest see it too: a game may use it, and holding it is not
         * by itself a request for anything here. */
        return false;
    }

    const bool ui_open = settingsui_is_open() || diskui_is_open();

    if (down && m_MetaDown && (code == KEY_F11 || code == KEY_F12)) {
        if (ui_open) {
            if (settingsui_is_open()) settingsui_close();
            if (diskui_is_open()) diskui_close();
        } else if (code == KEY_F11) {
            settingsui_open();
        } else {
            diskui_open();
        }
        return true;
    }

    if (!ui_open) return false;
    if (settingsui_is_open()) return settingsui_handle_key(code, down);
    return diskui_handle_key(code, down);
}

/*
 * Keystrokes arriving over the serial link, which is how the test rig types
 * into the guest.
 *
 * Reading the UART means reading a peripheral register, and a peripheral
 * read is not a memory read: it crosses a slow bus and costs on the order of
 * a hundred cycles.  Doing that once per turn of the main loop is a real
 * cost for something a person could not produce faster than a few times a
 * second, so it is asked rarely.  A key still arrives within a fraction of a
 * millisecond.
 */
void CKernel::DrainSerialKeys()
{
    if (!m_PC) return;
#ifdef CIRCLE_PC_NO_UART
    return;             /* the link is not used on a card that boots alone */
#else
    /* Every sixteenth turn, not every two hundred and fifty-sixth: the
     * receiver holds only sixteen bytes and a frame read too late is a
     * keystroke the guest never sees.  One in sixteen already removes all
     * but a few per cent of the peripheral reads. */
    if (++m_SerialPoll & 0xf) return;
    unsigned char buf[32];
    const int got = m_Serial.Read(buf, sizeof buf);
    if (got <= 0) return;
    /* 0xfa, a little-endian 32-bit physical address and a 16-bit length:
     * print that much guest memory.  The code a crashed guest ran is still
     * there after the crash, and it is the only copy with the load-time
     * fixups applied. */
    static unsigned char memReq[6];
    static int memReqGot = -1;
    for (int i = 0; i < got; ++i) {
        const unsigned char b = buf[i];
        if (memReqGot >= 0) {
            memReq[memReqGot++] = b;
            if (memReqGot == 6) {
                memReqGot = -1;
                DumpGuestMemory(memReq[0] | memReq[1] << 8 | memReq[2] << 16 |
                                (uint32_t)memReq[3] << 24,
                                memReq[4] | memReq[5] << 8);
            }
            continue;
        }
        if (m_SerialKeyState == 0 && b == 0xfau) {
            memReqGot = 0;
            continue;
        }
        switch (m_SerialKeyState) {
        case 0:
            if (b == 0xfeu) m_SerialKeyState = 1;
            /* Not a keystroke: read out the exceptions the guest has taken.
             * The fault being chased lands in a different place on every run,
             * so the only way to see it is to ask after it has happened. */
            else if (b == 0xfdu) DumpExcLog();
            /* Empty the exception ring so it fills with whatever the guest
             * is doing now, rather than with how it got here. */
            else if (b == 0xfcu) {
                cpu_exclog_rearm();
                m_Log.Write("EXC", LogNotice, "ring re-armed");
            }
            /* Stop the ring where it is.  The failure shows on the screen
             * first, so whoever is watching says when - every rule the
             * emulator used to decide that for itself fired early. */
            else if (b == 0xfbu) {
                cpu_exclog_freeze();
                m_Log.Write("EXC", LogNotice, "ring frozen");
            }
            /* The front panel's power and reset buttons. */
            else if (b == 0xf7u) m_SerialPanel = CFrontPanel::EventPower;
            else if (b == 0xf6u) m_SerialPanel = CFrontPanel::EventReset;
#if SB_IO_LOG
            /* The Sound Blaster's port, DMA and interrupt log, newest 8192. */
            else if (b == 0xf4u) {
                const uint32_t *e;
                uint32_t first;
                const uint32_t n = sblog_take(&e, &first);
                if (n - first > 8192u) first = n - 8192u;
                m_Log.Write("SBLOG", LogNotice, "begin %lu", (unsigned long)(n - first));
                char line[8 * 18 + 1];
                for (uint32_t i = first; i < n; i += 8) {
                    int len = 0;
                    for (uint32_t j = i; j < n && j < i + 8; j++) {
                        const uint32_t *x = e + (j % 16384u) * 2u;
                        len += snprintf(line + len, sizeof line - len, "%08lx:%07lx ",
                                        (unsigned long)x[0], (unsigned long)x[1]);
                    }
                    m_Log.Write("SBLOG", LogNotice, "%s", line);
                }
                m_Log.Write("SBLOG", LogNotice, "end");
            }
            /* The bytes the card was given, the last 32 KB. */
            else if (b == 0xf3u) {
                const uint8_t *r;
                const uint32_t n = sblog_pcm_take(&r);
                const uint32_t first = n > 32768u ? n - 32768u : 0;
                m_Log.Write("SBPCM", LogNotice, "begin %lu at %lu", (unsigned long)(n - first),
                            (unsigned long)first);
                static const char hex[] = "0123456789abcdef";
                char line[2 * 64 + 1];
                for (uint32_t i = first; i < n; i += 64) {
                    int len = 0;
                    for (uint32_t j = i; j < n && j < i + 64; j++) {
                        const uint8_t v = r[j & 0xffffu];
                        line[len++] = hex[v >> 4];
                        line[len++] = hex[v & 15];
                    }
                    line[len] = 0;
                    m_Log.Write("SBPCM", LogNotice, "%s", line);
                }
                m_Log.Write("SBPCM", LogNotice, "end");
            }
#endif
            /* The video card's registers and what the renderer makes of
             * them, for a picture that comes out wrong. */
            else if (b == 0xf5u) {
                static char dump[2048];
                vga_dump_state(m_PC->vga, dump, sizeof dump);
                for (char *line = dump, *end; *line; line = end) {
                    end = strchr(line, '\n');
                    if (end) *end++ = 0; else end = line + strlen(line);
                    m_Log.Write("VGA", LogNotice, "%s", line);
                }
            }
#if ADLIB_OPL_LOG
            /* Record the guest's OPL register writes, and print them. */
            else if (b == 0xf9u) {
                adlib_log_start();
                m_Log.Write("OPLLOG", LogNotice, "recording");
            }
            else if (b == 0xf8u) DumpOplLog();
#endif
            break;
        case 1:
            m_SerialKeyDown = (b != 0);
            m_SerialKeyState = 2;
            break;
        default:
            if (b && !HandleUiKey(m_SerialKeyDown != 0, b))
                ps2_put_keycode(m_PC->kbd, m_SerialKeyDown != 0, b);
            m_SerialKeyState = 0;
            break;
        }
    }
#endif
}

#if ADLIB_OPL_LOG
void CKernel::DumpOplLog(void)
{
    const uint32_t *e;
    const uint32_t n = adlib_log_take(&e);
    m_Log.Write("OPLLOG", LogNotice, "begin %lu", (unsigned long)n);
    for (uint32_t i = 0; i < n; i += 8) {
        char line[160];
        int p = 0;
        for (uint32_t j = i; j < n && j < i + 8; j++)
            p += snprintf(line + p, sizeof line - p, "%08lx:%06lx ",
                          (unsigned long)e[2 * j], (unsigned long)e[2 * j + 1]);
        m_Log.Write("OPLLOG", LogNotice, "%s", line);
    }
    m_Log.Write("OPLLOG", LogNotice, "end");
}
#endif

void CKernel::DumpGuestMemory(uint32_t addr, unsigned len)
{
    if (!m_PC) return;
    const uint32_t size = (uint32_t)m_PC->phys_mem_size;
    for (unsigned off = 0; off < len; off += 32) {
        char line[160];
        int n = snprintf(line, sizeof line, "%08lx:", (unsigned long)(addr + off));
        for (unsigned j = 0; j < 32 && off + j < len; ++j) {
            const uint32_t a = addr + off + j;
            if (a >= size) break;
            n += snprintf(line + n, sizeof line - n, " %02x",
                          (unsigned char)m_PC->phys_mem[a]);
        }
        m_Log.Write("MEM", LogNotice, "%s", line);
    }
    m_Log.Write("MEM", LogNotice, "end");
}

/* Counted in ide.c; its header pulls in the PCI declarations, which this
 * file has no other use for. */
extern "C" uint32_t g_ide_write_lost, g_ide_read_short;
extern "C" uint32_t g_ide_seek_failed, g_ide_sync_failed;
extern "C" uint32_t g_ide_cmd_hist[256];
extern "C" uint32_t g_ide_tf_unselected, g_ide_cmd_errors;
extern "C" int ide_trace_line(int i, char *buf, int size);
extern "C" void ide_sync_idle(int force);
extern "C" int io_trace_line(int idx, char *out, int cap);
extern "C" int sb16_diag_line(char *out, int cap);
extern "C" uint64_t g_ide_us, g_ide_bytes, g_ide_calls, g_ide_max_us,
                    g_ide_seek_us, g_ide_seeks;
extern "C" uint32_t g_intr_taken, g_intr_blocked_if;
/* How far the PIT fell behind and what it dropped; see i8254.c. */
extern "C" uint32_t g_pit_lag_max, g_pit_irq0_dropped, g_pit_gap_max_us;
extern "C" uint64_t g_ide_sync_us, g_ide_syncs, g_ide_sync_max_us, g_ide_write_max_us;
extern "C" uint64_t g_ide_write_us, g_ide_write_bytes, g_ide_writes;

/*
 * What the guest has faulted on, in order.
 *
 * Asked for over the serial link rather than written as it happens: a V86
 * monitor traps often enough that logging each one would itself change how
 * the machine runs, and the interesting entry is only interesting next to
 * the ones around it.  See the ring in i386.c.
 */
void CKernel::DumpExcLog()
{
    char line[2600];
    const uint32_t pic = i8259_debug_master(m_PC->pic);
    m_Log.Write("EXC", LogNotice,
                "cpu: cs=%04x base=%08lx ip=%08lx next=%08lx flags=%08lx "
                "cpl=%u halt=%u intr=%u | pic irr=%02x imr=%02x isr=%02x "
                "irq taken=%lu blocked-if=%lu",
                (unsigned)m_PC->cpu->seg[1].sel,
                (unsigned long)m_PC->cpu->seg[1].base,
                (unsigned long)m_PC->cpu->ip,
                (unsigned long)m_PC->cpu->next_ip,
                (unsigned long)m_PC->cpu->flags,
                (unsigned)m_PC->cpu->cpl,
                m_PC->cpu->halt ? 1u : 0u,
                m_PC->cpu->intr ? 1u : 0u,
                (unsigned)((pic >> 8) & 0xffu),
                (unsigned)((pic >> 16) & 0xffu),
                (unsigned)((pic >> 24) & 0xffu),
                (unsigned long)g_intr_taken,
                (unsigned long)g_intr_blocked_if);
    /* Where the descriptor tables are, so the segments named in the rings
     * can be looked up with a memory read (0xfa). */
    m_Log.Write("EXC", LogNotice,
                "tables: gdt=%08lx/%04lx idt=%08lx/%04lx cr0=%08lx ss=%04x/%08lx",
                (unsigned long)m_PC->cpu->gdt.base,
                (unsigned long)m_PC->cpu->gdt.limit,
                (unsigned long)m_PC->cpu->idt.base,
                (unsigned long)m_PC->cpu->idt.limit,
                (unsigned long)m_PC->cpu->cr0,
                (unsigned)m_PC->cpu->seg[2].sel,
                (unsigned long)m_PC->cpu->seg[2].base);
    m_Log.Write("EXC", LogNotice,
                "pit: fell %lu us behind at most, %lu irq0 periods dropped, "
                "poll gap max %lu us",
                (unsigned long)((uint64_t)g_pit_lag_max * 1000000u / 1193182u),
                (unsigned long)g_pit_irq0_dropped,
                (unsigned long)g_pit_gap_max_us);
    if (cpu_exc_hist_report(line, sizeof line) > 0)
        m_Log.Write("EXC", LogNotice, "totals: %s", line);
    if (cpu_exc_first_ud_report(line, sizeof line) > 0)
        m_Log.Write("EXC", LogNotice, "first UD: %s", line);
    if (cpu_seg_limit_report(line, sizeof line) > 0)
        m_Log.Write("EXC", LogNotice, "segments: %s", line);
    for (int i = 0; cpu_seg_ref_line(i, line, sizeof line) > 0; ++i)
        m_Log.Write("EXC", LogNotice, "ref %2d: %s", i, line);
    for (int i = 0; cpu_seg_clr_line(i, line, sizeof line) > 0; ++i)
        m_Log.Write("EXC", LogNotice, "clr %2d: %s", i, line);
    for (int i = 0; cpu_v86_entry_line(i, line, sizeof line) > 0; ++i)
        m_Log.Write("EXC", LogNotice, "v86 %2d: %s", i, line);
    for (int i = 0; cpu_gate_line(i, line, sizeof line) > 0; ++i)
        m_Log.Write("EXC", LogNotice, "gate %2d: %s", i, line);
    for (int i = 0; cpu_pc_line(i, line, sizeof line) > 0; ++i)
        m_Log.Write("EXC", LogNotice, "pc %2d: %s", i, line);
    if (sb16_diag_line(line, sizeof line) > 0)
        m_Log.Write("EXC", LogNotice, "sb16: %s", line);
    for (int i = 0; io_trace_line(i, line, sizeof line) > 0; ++i)
        m_Log.Write("EXC", LogNotice, "io %3d: %s", i, line);
    for (int i = 0; cpu_xfer_line(i, line, sizeof line) > 0; ++i)
        m_Log.Write("EXC", LogNotice, "xfer %2d: %s", i, line);
    m_Log.Write("EXC", LogNotice,
                "disk: %lu writes lost, %lu short reads | %lu KB in %lu ms, "
                "%lu seeks in %lu ms",
                (unsigned long)g_ide_write_lost, (unsigned long)g_ide_read_short,
                (unsigned long)(g_ide_bytes / 1024), (unsigned long)(g_ide_us / 1000),
                (unsigned long)g_ide_seeks, (unsigned long)(g_ide_seek_us / 1000));
    m_Log.Write("EXC", LogNotice, "disk failures: %lu seeks, %lu syncs",
                (unsigned long)g_ide_seek_failed,
                (unsigned long)g_ide_sync_failed);
    {
        const unsigned held = m_LongTurnHead < LongTurnN ? m_LongTurnHead : LongTurnN;
        const unsigned first = m_LongTurnHead < LongTurnN ? 0 : m_LongTurnHead % LongTurnN;
        m_Log.Write("EXC", LogNotice, "long turns: %u over %u ms", m_LongTurnHead,
                    LongTurnUs / 1000);
        for (unsigned i = 0; i < held; ++i) {
            const LongTurn &e = m_LongTurns[(first + i) % LongTurnN];
            m_Log.Write("EXC", LogNotice,
                        "turn %2u: %lu.%03lus %lu ms = input %lu + step %lu "
                        "(vga %lu, read %lu, write %lu, flush %lu, %lu insns) + after %lu | at %04x:%08lx",
                        i, (unsigned long)(e.t_ms / 1000), (unsigned long)(e.t_ms % 1000),
                        (unsigned long)(e.total / 1000), (unsigned long)(e.input / 1000),
                        (unsigned long)(e.step / 1000), (unsigned long)(e.vga / 1000),
                        (unsigned long)(e.ide / 1000), (unsigned long)(e.write / 1000),
                        (unsigned long)(e.sync / 1000),
                        (unsigned long)e.cycles, (unsigned long)(e.after / 1000),
                        (unsigned)e.cs, (unsigned long)e.ip);
        }
    }
    m_Log.Write("EXC", LogNotice,
                "disk stalls: longest read %lu us, write %lu us, flush %lu us; "
                "%lu flushes took %lu ms; %lu writes, %lu KB in %lu ms",
                (unsigned long)g_ide_max_us, (unsigned long)g_ide_write_max_us,
                (unsigned long)g_ide_sync_max_us, (unsigned long)g_ide_syncs,
                (unsigned long)(g_ide_sync_us / 1000),
                (unsigned long)g_ide_writes, (unsigned long)(g_ide_write_bytes / 1024),
                (unsigned long)(g_ide_write_us / 1000));
    {
        char cmds[224];
        int n = 0;
        for (int i = 0; i < 256 && n + 16 < (int)sizeof cmds; i++)
            if (g_ide_cmd_hist[i])
                n += snprintf(cmds + n, sizeof cmds - n, "%02x=%lu ",
                              i, (unsigned long)g_ide_cmd_hist[i]);
        m_Log.Write("EXC", LogNotice, "ata: %s", cmds);
    }
    m_Log.Write("EXC", LogNotice,
                "ata: %lu commands ended in error, %lu ran on task-file "
                "writes made under another selection",
                (unsigned long)g_ide_cmd_errors,
                (unsigned long)g_ide_tf_unselected);
    for (int i = 0; ide_trace_line(i, line, sizeof line) > 0; ++i)
        m_Log.Write("EXC", LogNotice, "ata %2d: %s", i, line);
#if defined(CPU_STACK_WATCH)
    for (int i = 0; cpu_watch_line(i, line, sizeof line) > 0; ++i)
        m_Log.Write("EXC", LogNotice, "watch %2d: %s", i, line);
#endif
    for (int i = 0; i < CPU_EXCLOG_N; i++) {
        if (cpu_exclog_report(i, line, sizeof line) <= 0) continue;
        m_Log.Write("EXC", LogNotice, "%2d %s", i, line);
    }
    m_Log.Write("EXC", LogNotice, "end");
}

/*
 * The machine is switched off - by the guest, or by the power button - and
 * stays off until the power button is pressed, as an ATX PC does.
 *
 * A Raspberry Pi 3 cannot remove its own power; what software can do is
 * stop.  The guest stops, the screen is blanked, which lets most monitors go
 * to standby, the sound is muted and the power light goes out.  USB stays
 * powered, so that the keyboard and the drives are there when the machine
 * comes back: switching the host controller off would mean bringing it up
 * again from nothing.
 *
 * The disk images need nothing more than the sync: the controllers sync the
 * image at the end of every write command, so whatever the guest wrote
 * before it was switched off is already on the card.
 */
void CKernel::PowerOff(const char *why)
{
    ide_sync_idle(1);   /* the last writes, before the power goes */
    m_Log.Write("PC", LogNotice, "%s; off until the power button is pressed", why);
    SavePowerState(false);
    SetBlank(true);
    m_Panel.SetPower(false);
    g_audio_volume = 0;
    for (;;) {
        DrainSerialKeys();
        CFrontPanel::Event ev = m_Panel.Poll(CTimer::GetClockTicks64(), 0, g_ide_activity);
        if (m_SerialPanel) { ev = (CFrontPanel::Event)m_SerialPanel; m_SerialPanel = 0; }
        if (ev == CFrontPanel::EventPower) break;
        CTimer::SimpleMsDelay(1);
    }
    PowerOn();
}

/*
 * A cold start: memory cleared and the sound chips back to their power-on
 * state before the BIOS posts - not Ctrl+Alt+Del, which keeps both.
 */
void CKernel::PowerOn()
{
    m_Log.Write("PC", LogNotice, "power button: switching on");
    memset(m_PC->phys_mem, 0, (size_t)m_PC->phys_mem_size);
    adlib_power_on(m_PC->adlib);
    gmsynth_power_on();
    m_PC->shutdown_state = 0;
    load_bios_and_reset(m_PC);
    config_apply_all();     /* the volume, muted while it was off */
    m_KeyTail = m_KeyHead;  /* nothing typed at a switched-off machine */
    SetBlank(false);
    m_Panel.SetPower(true);
    SavePowerState(true);
}

void CKernel::SetBlank(bool blank)
{
    CBcmPropertyTags Tags;
    TPropertyTagSimple Blank;
    Blank.nValue = blank ? 1 : 0;
    Tags.GetTag(0x00040002 /* blank screen */, &Blank, sizeof Blank, 4);
}

/*
 * Whether the machine was on when the board last lost power, for "last
 * state".  Kept in a file of its own rather than in config.ini, which is
 * the menu's to write: saving it there would also save whatever the menu
 * had changed and not been asked to keep.  Written only when it changes.
 */
#define POWER_STATE_PATH "SD:/386/power.sta"

bool CKernel::LoadPowerState()
{
    FIL f;
    char c = '1';           /* never written: it was on */
    UINT br = 0;
    if (f_open(&f, POWER_STATE_PATH, FA_READ) == FR_OK) {
        f_read(&f, &c, 1, &br);
        f_close(&f);
    }
    return c != '0';
}

void CKernel::SavePowerState(bool on)
{
    if (on == m_PowerStateSaved) return;
    FIL f;
    UINT bw = 0;
    if (f_open(&f, POWER_STATE_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    const char c = on ? '1' : '0';
    f_write(&f, &c, 1, &bw);
    if (f_close(&f) == FR_OK && bw == 1) m_PowerStateSaved = on;
}

void CKernel::DrainInput()
{
    if (!m_PC) return;
    while (m_KeyTail != m_KeyHead) {
        const KeyEvent event = m_KeyQueue[m_KeyTail];
        __asm__ __volatile__("dmb ish" ::: "memory");
        m_KeyTail = (unsigned char)((m_KeyTail + 1u) & (KeyQueueSize - 1u));
        const unsigned char code = hid_to_linux_keycode(event.code);
        /*
         * HID reports a held key as state, whereas an AT keyboard emits
         * typematic make codes itself.  Re-create just that behaviour here;
         * modifiers never repeat and a newer ordinary key becomes the active
         * typematic key, like a physical PC keyboard.
         *
         * This is done before the menus get a look, and not after.  It used to
         * sit below a "continue" that ran whenever a menu consumed the key -
         * so while a menu was open no release ever cleared m_RepeatKey, and
         * whatever had last been held went on repeating into the guest behind
         * it.
         */
        if (event.code < 0xe0u) {
            if (event.down) {
                m_RepeatKey = event.code;
                m_NextKeyRepeat = CTimer::GetClockTicks64() + 500000u;
            } else if (m_RepeatKey == event.code) {
                m_RepeatKey = 0;
            }
        }
#if defined(CIRCLE_PC_STATS)
        if (settingsui_is_open() || diskui_is_open())
            m_Log.Write("KEY", LogNotice, "%s %u",
                        event.down ? "down" : "up  ", (unsigned)code);
#endif
        if (code && HandleUiKey(event.down != 0, code)) continue;
        if (code) ps2_put_keycode(m_PC->kbd, event.down != 0, code);
    }
    const uint64_t key_now = CTimer::GetClockTicks64();
    /*
     * After the machine has been stopped, nothing here knows what is held.
     *
     * A key repeats because a release has not arrived yet, and releases
     * arrive from the USB keyboard - which is not being serviced while this
     * loop sits inside a transfer to some other device on the same bus.  A
     * CD drive spinning up does that for a second or more, and every
     * millisecond of it is a release that has not been heard.  Carrying on
     * repeating across such a gap invents keystrokes the person never made,
     * which is what filled a DOS screen with prompts; dropping the repeat
     * costs at worst one repeat of a key that really was held down.
     */
    if (m_LastDrain && key_now - m_LastDrain > 150000u) m_RepeatKey = 0;
    m_LastDrain = key_now;

    if (m_RepeatKey && key_now >= m_NextKeyRepeat) {
        const unsigned char code = hid_to_linux_keycode(m_RepeatKey);
        /*
         * A held key repeats in the menus too, and at a rate meant for
         * reading rather than for typing: thirty a second walks a list far
         * faster than anyone can follow.  The repeats used to go past the
         * menus entirely and into the guest, which is the wrong place for
         * them while a menu is up.
         */
        const bool ui = settingsui_is_open() || diskui_is_open();
        if (code && !HandleUiKey(true, code))
            ps2_put_keycode(m_PC->kbd, true, code);
        const uint64_t interval = ui ? 120000u   // about eight a second
                                     : 33333u;   // 30 Hz, near the AT rate
        m_NextKeyRepeat += interval;
        /*
         * A machine that was stopped does not owe the guest the keystrokes
         * it missed.
         *
         * The step used to be added to the last due time whatever the clock
         * said, so a second spent inside a USB transfer - a CD drive
         * spinning up, say - left thirty repeats outstanding, and they went
         * into the guest as fast as this loop could run.  What that looks
         * like is a DOS prompt printing itself down the screen: measured at
         * twelve repeats of the keypad Return in twenty milliseconds.  Late
         * is late; the next one is due from now.
         */
        if (m_NextKeyRepeat < key_now) m_NextKeyRepeat = key_now + interval;
    }
    while (m_MouseTail != m_MouseHead) {
        const MouseEvent event = m_MouseQueue[m_MouseTail];
        __asm__ __volatile__("dmb ish" ::: "memory");
        m_MouseTail = (unsigned char)((m_MouseTail + 1u) & (MouseQueueSize - 1u));
        ps2_mouse_event(m_PC->mouse, event.dx, event.dy, event.wheel, event.buttons);
    }
}

static void log_guest_display_diag(CLogger &log, PC *pc)
{
    if (!pc || !pc->cpu || !pc->vga) return;
    CPUI386 *cpu = pc->cpu;
    int graphics_width = 0, graphics_height = 0;
    const int mode = vga_get_mode(pc->vga);
    const int graphics = vga_get_graphics_mode(pc->vga, &graphics_width, &graphics_height);
    const int cols = vga_get_text_cols(pc->vga);
    const uint16_t start = vga_get_start_addr(pc->vga);
    log.Write("DISPLAY", LogNotice,
        "VGA mode=%d graphics=%d %dx%d textcols=%d start=%04x lineoff=%d",
        mode, graphics, graphics_width, graphics_height, cols, (unsigned)start,
        vga_get_line_offset(pc->vga));

    char cells[3 * 80 + 1];
    const size_t vga_size = (size_t)pc->vga_mem_size;
    for (unsigned i = 0; i < 80; ++i) {
        const size_t offset = (((size_t)start + i) * 4u) & 0x1fffeu;
        if (offset + 1u < vga_size) {
            const uint8_t ch = (uint8_t)pc->vga_mem[offset];
            const uint8_t attr = (uint8_t)pc->vga_mem[offset + 1u];
            static const char hex[] = "0123456789ABCDEF";
            cells[3*i] = (ch >= 32 && ch < 127) ? (char)ch : '.';
            cells[3*i + 1] = hex[attr >> 4];
            cells[3*i + 2] = hex[attr & 15];
        } else {
            cells[3*i] = '?'; cells[3*i + 1] = '?'; cells[3*i + 2] = '?';
        }
    }
    cells[sizeof cells - 1] = 0;
    log.Write("DISPLAY", LogNotice, "text80 char+attr=%s", cells);

    const uint32_t cs = cpu->seg[1].sel;
    const uint64_t gdt_desc = (uint64_t)cpu->gdt.base + (cs & ~7u);
    if ((cs & ~7u) <= cpu->gdt.limit && gdt_desc + 8u <= (uint64_t)pc->phys_mem_size) {
        const uint8_t *d = (const uint8_t *)pc->phys_mem + gdt_desc;
        uint32_t desc_base = (uint32_t)d[2] | ((uint32_t)d[3] << 8) |
            ((uint32_t)d[4] << 16) | ((uint32_t)d[7] << 24);
        uint32_t desc_limit = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
            ((uint32_t)(d[6] & 0x0fu) << 16);
        if (d[6] & 0x80u) desc_limit = (desc_limit << 12) | 0xfffu;
        log.Write("DISPLAY", LogNotice,
            "GDT CS=%04lx base=%08lx limit=%08lx desc=%02x%02x%02x%02x%02x%02x%02x%02x",
            (unsigned long)cs, (unsigned long)cpu->seg[1].base,
            (unsigned long)cpu->seg[1].limit, d[7], d[6], d[5], d[4], d[3], d[2], d[1], d[0]);
        log.Write("DISPLAY", LogNotice,
            "CS decoded base=%08lx limit=%08lx type=%02x S=%u DPL=%u P=%u D=%u G=%u",
            (unsigned long)desc_base, (unsigned long)desc_limit,
            (unsigned)(d[5] & 0x1fu), (unsigned)((d[5] >> 4) & 1u),
            (unsigned)((d[5] >> 5) & 3u), (unsigned)((d[5] >> 7) & 1u),
            (unsigned)((d[6] >> 6) & 1u), (unsigned)((d[6] >> 7) & 1u));
    } else {
        log.Write("DISPLAY", LogNotice, "GDT CS=%04lx descriptor out of guest RAM", (unsigned long)cs);
    }
    const uint64_t linear = (uint64_t)cpu->seg[1].base + cpu->ip;
    if (linear + 32u <= (uint64_t)pc->phys_mem_size) {
        const uint8_t *code = (const uint8_t *)pc->phys_mem + linear;
        char bytes[3 * 32 + 1];
        for (unsigned i = 0; i < 32; ++i) {
            static const char hex[] = "0123456789ABCDEF";
            bytes[3*i] = hex[code[i] >> 4]; bytes[3*i + 1] = hex[code[i] & 15];
            bytes[3*i + 2] = i == 31 ? 0 : ' ';
        }
        bytes[sizeof bytes - 1] = 0;
        log.Write("DISPLAY", LogNotice, "code linear=%08lx bytes=%s", (unsigned long)linear, bytes);
    } else {
        log.Write("DISPLAY", LogNotice, "code linear=%08lx out of guest RAM", (unsigned long)linear);
    }
}

#if defined(CIRCLE_PC_DIAG)
/* This is deliberately a raw, read-only snapshot.  In particular it never
 * invokes the emulator's address translation helpers: doing so while the
 * guest is paged can alter its TLB/exception bookkeeping, defeating the
 * purpose of a passive diagnostic image. */
static uint32_t guest_reg32(const CPUI386 *cpu, unsigned reg)
{
#ifdef I386_OPT1
    return cpu->gprx[reg].r32;
#else
    return (uint32_t)cpu->gpr[reg];
#endif
}

static uint32_t raw_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool is_pic_port(uint16_t port)
{
    return port == 0x20u || port == 0x21u || port == 0xa0u || port == 0xa1u;
}

static void log_recent_nonpic_io(CLogger &log, const CirclePcDiagSnapshot &diag)
{
    const uint32_t first = diag.io_seq > CIRCLE_PC_IO_RING ?
        diag.io_seq - CIRCLE_PC_IO_RING + 1u : 1u;
    unsigned reported = 0;
    for (uint32_t seq = diag.io_seq; seq >= first && reported < 8u; --seq) {
        const CirclePcIoDiag &io = diag.io[seq & (CIRCLE_PC_IO_RING - 1u)];
        if (io.seq != seq || is_pic_port(io.port)) continue;
        log.Write("EXEC", LogNotice,
            "recent-io #%lu %s%u port=%04x value=%08lx%s%ld",
            (unsigned long)seq, io.direction ? "OUT" : "IN", (unsigned)io.width,
            (unsigned)io.port, (unsigned long)io.value,
            io.is_string ? " elements=" : "", (long)(io.is_string ? io.string_count : 0));
        ++reported;
    }
    if (!reported) log.Write("EXEC", LogNotice, "recent-io: no non-PIC access in retained window");
}

static void log_guest_execution_diag(CLogger &log, PC *pc, const char *reason)
{
    if (!pc || !pc->cpu) return;
    CPUI386 *cpu = pc->cpu;
    const uint32_t eax = guest_reg32(cpu, 0), ecx = guest_reg32(cpu, 1);
    const uint32_t edx = guest_reg32(cpu, 2), ebx = guest_reg32(cpu, 3);
    const uint32_t esp = guest_reg32(cpu, 4), ebp = guest_reg32(cpu, 5);
    const uint32_t esi = guest_reg32(cpu, 6), edi = guest_reg32(cpu, 7);
    const uint32_t stack_offset = esp & (uint32_t)cpu->sp_mask;
    const uint64_t stack_linear = (uint64_t)cpu->seg[2].base + stack_offset;
    log.Write("EXEC", LogNotice,
        "%s CS=%04lx:EIP=%08lx SS=%04lx:ESP=%08lx stack-linear=%08lx CR0=%08lx CR3=%08lx flags=%08lx",
        reason, (unsigned long)cpu->seg[1].sel, (unsigned long)cpu->ip,
        (unsigned long)cpu->seg[2].sel, (unsigned long)esp,
        (unsigned long)stack_linear, (unsigned long)cpu->cr0,
        (unsigned long)cpu->cr3, (unsigned long)cpu->flags);
    log.Write("EXEC", LogNotice,
        "EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx",
        (unsigned long)eax, (unsigned long)ebx, (unsigned long)ecx,
        (unsigned long)edx, (unsigned long)esi, (unsigned long)edi,
        (unsigned long)ebp, (unsigned long)esp);
    if (!(cpu->cr0 & 0x80000000u) && stack_linear + 16u <= (uint64_t)pc->phys_mem_size) {
        const uint8_t *stack = (const uint8_t *)pc->phys_mem + stack_linear;
        log.Write("EXEC", LogNotice, "stack raw=%08lx %08lx %08lx %08lx",
            (unsigned long)raw_le32(stack), (unsigned long)raw_le32(stack + 4),
            (unsigned long)raw_le32(stack + 8), (unsigned long)raw_le32(stack + 12));
    } else if (cpu->cr0 & 0x80000000u) {
        log.Write("EXEC", LogNotice, "stack raw intentionally not read: guest paging is enabled");
    } else {
        log.Write("EXEC", LogNotice, "stack raw out of guest RAM");
    }
    log_guest_display_diag(log, pc);
    CirclePcDiagSnapshot diag{};
    circle_pc_diag_snapshot(&diag);
    log_recent_nonpic_io(log, diag);
}
#endif

boolean CKernel::Initialize()
{
    /* Interrupts first: the serial device below is interrupt driven. */
    if (!m_Interrupt.Initialize()) return FALSE;
#ifdef CIRCLE_PC_NO_UART
    /*
     * No log to the serial port.
     *
     * Every line written goes out a character at a time on a polled UART at
     * 115200 baud, which takes the machine away from the guest for as long
     * as the line lasts - long enough to break the sound up.  On a card that
     * boots on its own there is nobody reading it, so the logger keeps its
     * buffer and writes to nothing.
     */
    if (!m_Log.Initialize(0)) return FALSE;
#else
    if (!m_Serial.Initialize(115200) || !m_Log.Initialize(&m_Serial)) return FALSE;
#endif
    /* Say something as soon as there is anywhere to say it.  Everything above
     * this line fails silently by construction, and the three Initialize()
     * calls below used to as well. */
    m_Log.Write("BOOT", LogNotice, "logger up; ARM clock %u Hz", m_CPUThrottle.GetClockRate());
    if (!m_Timer.Initialize()) { m_Log.Write("BOOT", LogError, "timer init failed"); return FALSE; }
    if (!m_Screen.Initialize()) { m_Log.Write("BOOT", LogError, "screen init failed"); return FALSE; }
    /*
     * And no cursor blinking in the corner while it comes up.
     *
     * Circle's console puts one on the screen as soon as it is initialised,
     * and until the logo goes up there is nothing else on there - so the
     * first thing this machine showed was somebody else's cursor.  Circle's
     * own escape for hiding it; see doc/screen.txt in the Circle tree.
     */
    { static const char hide_cursor[] = "[?25l";
      m_Screen.Write(hide_cursor, sizeof hide_cursor - 1); }
    m_USBReady = m_USB.Initialize() != FALSE;
    if (!m_USBReady) m_Log.Write("USB", LogError, "USB host initialization failed; guest remains usable without USB input");
    m_Video.screen = &m_Screen;
    m_Log.Write("BOOT", LogNotice, "X86Pi: USB input; guest writes limited to configured disk images");
    m_Log.Write("CPU", LogNotice, "ARM clock %u Hz", m_CPUThrottle.GetClockRate());

    /*
     * What the firmware thinks of the supply.
     *
     * A sagging five volts is the one fault that looks like every other
     * fault: USB transfers that stall for no reason, a floppy drive that
     * stops answering, an SD card that reads short.  Chasing one of those
     * in software while the board is browning out is wasted work, so the
     * board is asked outright and the answer is in the log next to
     * everything else.  Bits 0-3 are happening now, bits 16-19 have
     * happened since boot; bit 0 is under-voltage.  See the firmware's
     * get_throttled.
     */
    CBcmPropertyTags Tags;
    TPropertyTagSimple Throttled;
    if (Tags.GetTag (PROPTAG_GET_THROTTLED, &Throttled, sizeof Throttled, 4))
    {
        const u32 nNow = Throttled.nValue & 0xF;
        const u32 nEver = (Throttled.nValue >> 16) & 0xF;
        if (nNow || nEver)
            m_Log.Write("POWER", LogWarning,
                        "throttled 0x%X: under-voltage %s%s", Throttled.nValue,
                        (nNow & 1) ? "now" : "not now",
                        (nEver & 1) ? ", and has happened" : "");
        else
            m_Log.Write("POWER", LogNotice, "supply is fine");
    }
    /* Last, as Circle asks: the other cores start running from here. */
    if (!m_Cores.Initialize()) {
        m_Log.Write("CORES", LogError, "secondary cores did not start");
        return FALSE;
    }
    return TRUE;
}

/*
 * The General MIDI SoundFont, from SD:/386/gm.sf2.
 *
 * Read whole and handed to TinySoundFont, which keeps its own float copy of
 * the samples, so the file's buffer is freed again.  Without the file the
 * MPU-401 keeps its built-in sine-wave synthesiser.
 */
/* The guest's clock, from the DS3231 when one is fitted. */
void CKernel::StartClock()
{
    if (!m_RTC.Initialize()) {
        m_Log.Write("RTC", LogNotice, "no DS3231; the clock starts from the build time");
        return;
    }
    CmosDateTime dt;
    if (!m_RTC.Read(&dt)) {
        m_Log.Write("RTC", LogWarning, "DS3231 has lost the time; starting from the build time until the guest sets it");
        return;
    }
    cmos_set_clock(m_PC->cmos, &dt);
    m_Log.Write("RTC", LogNotice, "DS3231 %04d-%02d-%02d %02d:%02d:%02d",
                dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
}

void CKernel::LoadSoundFont()
{
    static const char path[] = "SD:/386/gm.sf2";
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        m_Log.Write("MIDI", LogNotice, "no %s; MPU-401 keeps its built-in synthesiser", path);
        return;
    }
    const uint64_t t0 = CTimer::GetClockTicks64();
    const FSIZE_t size = f_size(&f);
    uint8_t *buf = size ? (uint8_t *)malloc((size_t)size) : nullptr;
    UINT got = 0;
    const FRESULT fr = buf ? f_read(&f, buf, (UINT)size, &got) : FR_NOT_ENOUGH_CORE;
    f_close(&f);
    if (fr != FR_OK || got != size) {
        m_Log.Write("MIDI", LogWarning, "%s: read %lu of %lu bytes (%u)", path,
                    (unsigned long)got, (unsigned long)size, (unsigned)fr);
        free(buf);
        return;
    }
    const uint64_t t1 = CTimer::GetClockTicks64();
    const int ok = gmsynth_load(buf, (uint32_t)size);
    free(buf);
    m_Log.Write("MIDI", ok ? LogNotice : LogWarning,
                "%s: %lu KB, read in %lu ms, %s in %lu ms", path,
                (unsigned long)(size / 1024), (unsigned long)((t1 - t0) / 1000),
                ok ? "loaded" : "NOT a SoundFont", (unsigned long)((CTimer::GetClockTicks64() - t1) / 1000));
}

bool CKernel::LoadConfig(PCConfig *config, char *storage, unsigned storage_size)
{
    FIL f; if (f_open(&f, "SD:/386/config.ini", FA_READ) != FR_OK) return false;
    FSIZE_t fs = f_size(&f);
    if (!fs || fs >= storage_size) { f_close(&f); return false; }
    UINT got = 0; FRESULT fr = f_read(&f, storage, (UINT)fs, &got); FRESULT cl = f_close(&f);
    if (fr != FR_OK || cl != FR_OK || got != fs) return false;
    for (UINT i=0; i<got; ++i) if (storage[i] == 0) return false;
    storage[got] = 0;
    static PcConfigStorage parsed; memset(&parsed, 0, sizeof parsed); parsed.cpu = 4;
    if (ini_parse_string(storage, config_handler, &parsed) != 0 || !parsed.bios[0]) return false;
    /* The hosted redirector performs host file operations.  Refuse the
     * configuration explicitly until a Circle-safe, read-only transport is
     * implemented; never silently pretend it is active. */
    if (parsed.redirector) return false;
    memset(config, 0, sizeof *config);
    config->bios = parsed.bios; config->vga_bios = parsed.vga_bios;
    config->fdd[0] = parsed.fdd0; config->fdd[1] = parsed.fdd1;
    /*
     * A USB floppy drive is drive A unless the configuration says otherwise.
     *
     * It is what anyone plugging one in expects, and it costs nothing: the
     * drive is bound by name and is not touched until the guest asks for it.
     * An image named in the ini still wins, and the disk menu can still put
     * something else there.
     */
    if ((!config->fdd[0] || !config->fdd[0][0]) && blk_host_available("ufd1"))
        config->fdd[0] = "usb:ufd1";
#ifdef UFD_PROBE_ONLY
    /* Temporary, for one experiment: keep the guest's hands off the USB
     * drive so that the probe in UpdateInput() is the only thing reading it. */
    if (config->fdd[0] && strncmp(config->fdd[0], "usb:", 4) == 0)
        config->fdd[0] = 0;
#endif
    /*
     * And a USB CD drive is the secondary master, for the same reason: it is
     * where a CD-ROM lives on a machine of this age, nothing in the ini can
     * name one, and the drive is not touched until the guest asks.  An empty
     * drawer is still a drive - the guest sees a CD-ROM with no disc in it,
     * which is what it would see on the real thing.
     */
    if ((!config->ata[2] || !config->ata[2][0]) && blk_host_available("ucd1")) {
        config->ata[2] = "usb:ucd1";
        config->iscd[2] = 1;
    }
    config->ata[0] = parsed.ata0; config->ata[1] = parsed.ata1;
    config->cpu_gen = parsed.cpu;
    /* There is an x87 now - src/fpu.c, over the freestanding maths routines
     * in pc/libm.c - so this is the machine's own choice again.  A 386 with
     * no 387 fitted is a perfectly ordinary machine and stays the default. */
    config->fpu = parsed.fpu; config->redirector = parsed.redirector;
    config->mem_size = parse_size(parsed.mem); if (!config->mem_size) config->mem_size = 8L*1024*1024;
    /*
     * Four megabytes unless the file says otherwise.
     *
     * The old default of 256 KB is what a 1990 card had, and it is too small
     * for a VESA mode with a second page: Red Alert drew into a page the
     * display never showed.  This board has most of a gigabyte, so there is
     * no reason to be mean about it.
     */
    config->vga_mem_size = parse_size(parsed.vga_mem);
    if (!config->vga_mem_size) config->vga_mem_size = 4*1024*1024;
    config->width = 640; config->height = 480; config->enable_serial = 0;
    /* Circle's physical HDMI surface is 640 pixels wide.  BIOS mode 03h is
     * normally 80 x 9 = 720 pixels; the shared text renderer correctly
     * refuses to draw it into a 640-wide surface.  Force eight-dot cells so
     * the same 80 columns occupy exactly 640 pixels.  This changes only host
     * presentation, not the guest's VGA registers or memory layout. */
    config->vga_force_8dm = 1;
    /* 64 MB used to be the ceiling here.  The CMOS carries sizes above 16 MB
     * in bytes 0x34 and 0x35, so the BIOS can see more than that, and the
     * board reports over nine hundred megabytes of free heap - a Pentium
     * with 256 MB is a machine that existed and one this can build. */
    return config->mem_size >= 1024*1024 && config->mem_size <= 256L*1024*1024 &&
           config->vga_mem_size >= 64*1024 && config->vga_mem_size <= 16*1024*1024 &&
           (config->vga_mem_size & (config->vga_mem_size - 1)) == 0;
}

/*
 * One line per video-geometry change, in the ordinary image as well as the
 * diagnostic one.
 *
 * A guest that comes up with part of its picture missing is a question about
 * what the CRTC was actually programmed with, and that question cannot be
 * answered from the HDMI output.  printf() is compiled out on this target
 * (see pc/compat/stdio.h), so the shared renderer cannot report it; these are
 * the same registers vga_graphic_refresh() derives its target rectangle
 * from, read after the fact and logged only when one of them moves.
 */
static void log_video_geometry(CLogger &log, PC *pc)
{
    VGAState *s = pc ? pc->vga : nullptr;
    if (!s) return;

    const uint8_t regs[8] = {
        s->sr[0x01], s->gr[0x05], s->gr[0x06], s->cr[0x01],
        s->cr[0x07], s->cr[0x09], s->cr[0x12], s->cr[0x13],
    };
    static uint8_t last[8];
    static bool seen = false;
    if (seen && memcmp(regs, last, sizeof regs) == 0) return;
    memcpy(last, regs, sizeof regs);
    seen = true;

    const int graphics = (s->ar_index & 0x20) && (s->gr[0x06] & 1);
    const int shift_control = (s->gr[0x05] >> 5) & 3;
    const int double_scan = s->cr[0x09] >> 7;
    int w = ((int)s->cr[0x01] + 1) * 8;
    int h = s->cr[0x12] | ((s->cr[0x07] & 0x02) << 7) | ((s->cr[0x07] & 0x40) << 3);
    h++;
    const int multi_scan = shift_control != 1
        ? ((((s->cr[0x09] & 0x1f) + 1) << double_scan) - 1)
        : double_scan;
    int xdiv = 1;
    if (shift_control == 0 || shift_control == 1) {
        if (s->sr[0x01] & 8) { xdiv = 2; w *= 2; }
    } else {
        xdiv = 2;
    }

    log.Write("VIDEO", LogNotice,
        "geometry %s shift=%d dots=%dx%d xdiv=%d multi_scan=%d "
        "sr01=%02x gr05=%02x gr06=%02x cr01=%02x cr07=%02x cr09=%02x cr12=%02x cr13=%02x",
        graphics ? "graphics" : "text/blank", shift_control, w, h, xdiv, multi_scan,
        regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7]);
}

/* Redraw cost accumulated by the shared VGA renderer; see vga_refresh(). */
extern "C" uint32_t g_vga_refresh_us;
extern "C" uint32_t g_vga_refresh_calls;
extern "C" uint32_t g_vga_refresh_skips;
extern "C" uint32_t g_step_dev_us[6], g_step_steps;
extern "C" uint32_t g_lfb_rd[3], g_lfb_wr[3], g_lfb_str, g_lfb_str_bytes;
/* Register writes by kind; see opl3_note() in adlib.c. */
extern "C" uint32_t g_adlib_writes;
extern "C" uint32_t g_opl3_ws[2][8], g_opl3_keyons[2], g_opl3_c0_lr[2], g_opl3_c0_cnt[2];
extern "C" uint32_t g_opl3_4op_last, g_opl3_4op_nonzero, g_opl3_reg1_last;
extern "C" volatile uint32_t g_opl3_bank1_writes;
extern "C" uint32_t g_adlib_w_keyon, g_adlib_w_fnum, g_adlib_w_level,
                    g_adlib_w_env, g_adlib_w_other;
extern "C" uint32_t g_adlib_render_peak, g_adlib_rendered;
extern "C" uint32_t g_sb_port_reads, g_sb_port_writes;
extern "C" uint32_t g_sb16_starves, g_sb16_minfill;
extern "C" uint32_t g_sb16_dma_calls, g_sb16_dma_bytes;
extern "C" uint32_t g_sb16_gap_frames, g_sb16_gap_runs, g_sb16_gap_short,
                    g_sb16_gap_worst;
extern "C" uint32_t g_sb16_dbg_freq, g_sb16_dbg_fmt, g_sb16_dbg_stereo;
extern "C" uint32_t g_sb16_dbg_bits, g_sb16_dbg_have;
extern "C" uint8_t g_sb16_dbg_bytes[32];
extern "C" volatile uint32_t g_wl_tlb_refills, g_wl_tlb_clears;
extern "C" uint32_t g_adlib_underrun_total;
extern "C" uint32_t g_adlib_fill, g_adlib_skips, g_adlib_calls;
/* IRQ0 edges the PIT actually handed to the PIC; see i8254.c. */
extern "C" uint32_t g_pit_irq0_edges;
extern "C" uint32_t g_pit_update_calls, g_pit_burst_edges;
extern "C" uint32_t g_pit_gap_max_us, g_pit_gap_over_us;
/* IRQ0 requests the PIC accepted, and those lost because the request bit
 * was still pending; see pic_set_irq1() in i8259.c. */
extern "C" uint32_t g_pic_irq0_accepted, g_pic_irq0_merged;
/* Pending-interrupt outcomes seen by the CPU core; see i386.c. */
extern "C" uint32_t g_intr_taken, g_intr_blocked_if;
/* Time the guest spends inside its IRQ0 handler; see i8259.c. */
extern "C" uint32_t g_isr0_us_total, g_isr0_count;
/* What each lost IRQ0 edge was blocked behind; see pic_set_irq1(). */
extern "C" uint32_t g_pic_irq0_m_masked, g_pic_irq0_m_inservice, g_pic_irq0_m_other;

void CKernel::Run()
{
    if (!m_SD.Initialize()) { m_Log.Write("SD", LogError, "SD init failed"); return; }
    if (f_mount(&m_FS, "SD:", 1) != FR_OK) { m_Log.Write("SD", LogError, "mount failed"); return; }
    char config_text[16385]; PCConfig config{};
    if (!LoadConfig(&config, config_text, sizeof config_text)) {
        m_Log.Write("CFG", LogError, "config.ini invalid or missing"); return;
    }
    config_note_machine((int)(config.mem_size / (1024 * 1024)), config.cpu_gen);
    config_note_vga_mem((int)(config.vga_mem_size / 1024));
    config_note_fpu(config.fpu);
    m_Log.Write("CFG", LogNotice, "BIOS=%s RAM=%luK VGA=%luK CPU=%d FPU=%d", config.bios,
                (unsigned long)(config.mem_size/1024), (unsigned long)(config.vga_mem_size/1024),
                config.cpu_gen, config.fpu);
    CBcmFrameBuffer *framebuffer = m_Screen.GetFrameBuffer();
    uint8_t *fb = framebuffer ? (uint8_t *)(uintptr_t)framebuffer->GetBuffer() : nullptr;
    if (!fb || !framebuffer) { m_Log.Write("VIDEO", LogError, "framebuffer unavailable"); return; }
    m_Video.physical = fb;
    m_Video.physical_width = framebuffer->GetWidth();
    m_Video.physical_height = framebuffer->GetHeight();
    m_Video.physical_pitch = framebuffer->GetPitch();
    m_Video.physical_size = framebuffer->GetSize();
    const uint32_t depth = framebuffer->GetDepth();
    m_Log.Write("VIDEO", LogNotice, "Circle framebuffer %lux%lu depth=%lu pitch=%lu bytes=%lu",
        (unsigned long)m_Video.physical_width, (unsigned long)m_Video.physical_height,
        (unsigned long)depth, (unsigned long)m_Video.physical_pitch,
        (unsigned long)m_Video.physical_size);
    if (depth != 32 || m_Video.physical_pitch < m_Video.physical_width * 4u ||
        (uint64_t)m_Video.physical_pitch * m_Video.physical_height > m_Video.physical_size) {
        m_Log.Write("VIDEO", LogError, "unsupported physical framebuffer layout"); return;
    }
    /*
     * Always render into ordinary RAM and blit, never into scanout memory.
     *
     * When the physical surface happened to be exactly 640x480 with a 2560
     * byte pitch this used to hand the renderer the framebuffer itself, to
     * save a copy.  The copy was the cheap part.  The framebuffer the
     * firmware hands back is not write-back cached, and the shared renderer
     * emits four separate byte stores per pixel, so every pixel became four
     * uncached bus transactions: one full-screen EGA redraw measured 23.5 ms,
     * and at the 23 redraws a second this guest asks for, the renderer alone
     * took 540 ms of every second - 56% of the machine, against 2 ms a second
     * for the same code drawing text.
     *
     * A staging buffer is ordinary cached memory, so the per-pixel stores hit
     * the cache and reach the framebuffer as whole cache lines through one
     * memcpy per frame.
     */
    /*
     * Draw straight into the scanout buffer.
     *
     * Worth trying again only because the graphics memory is no longer
     * mapped as Device: with that mapping every store went out on its own
     * and a full redraw measured 23.5 ms, which is why the staging buffer
     * exists.  Mapped Normal but non-cacheable the processor may merge
     * them, and if it does, both the second write and the whole copy go
     * away - and the copy was two thirds of the display cost.  It is:
     * the display fell from 1090 ms of every ten seconds to 478.
     *
     * This needs build.sh to have given that memory the Normal
     * mapping; against an unpatched Circle it would be slower than
     * the staging buffer, not faster.
     */
    if (m_Video.physical_pitch == 640u * 4u && m_Video.offset_x == 0 &&
        m_Video.offset_y == 0 && m_Video.physical_height >= 480u) {
        m_Video.vga_surface = m_Video.physical;
        m_Video.staging = 1;
        /*
         * Clear it, because nothing else will.
         *
         * The renderer writes only the rows the guest's mode actually
         * covers, so whatever the firmware left in the rest stays on the
         * screen - which showed up as a band of noise below the table in
         * Epic Pinball, a game that does not use the whole 480 lines.  The
         * staging path cleared both buffers and this branch jumped over it.
         */
        memset(m_Video.physical, 0, m_Video.physical_size);
        m_Log.Write("VIDEO", LogNotice, "drawing straight into the framebuffer");
        goto video_ready;
    }
    m_Video.vga_surface = (uint8_t *)malloc(640u * 480u * 4u);
    if (!m_Video.vga_surface) { m_Log.Write("VIDEO", LogError, "VGA staging allocation failed"); return; }
    memset(m_Video.vga_surface, 0, 640u * 480u * 4u);
    memset(fb, 0, m_Video.physical_size);
    m_Video.offset_x = m_Video.physical_width > 640u ? (m_Video.physical_width - 640u) / 2u : 0u;
    m_Video.offset_y = m_Video.physical_height > 480u ? (m_Video.physical_height - 480u) / 2u : 0u;
    m_Video.staging = 1;
video_ready:
    /*
     * The name of the machine, while it is still coming up.
     *
     * There is a second or so between the framebuffer existing and the guest's
     * BIOS writing to it, and until now that second was a black screen - on a
     * machine whose whole point is that it looks like a PC from the front.
     * The picture goes straight into the framebuffer, not into the staging
     * buffer, so the first thing the guest draws replaces it without anything
     * having to clear it.
     */
    bootlogo_draw(m_Video.physical, m_Video.physical_width,
                  m_Video.physical_height, m_Video.physical_pitch);
    m_LogoShownAt = CTimer::GetClockTicks64();
    m_Log.Write("VIDEO", LogNotice, "VGA staging 640x480 pitch=2560 centered at %lu,%lu",
        (unsigned long)m_Video.offset_x, (unsigned long)m_Video.offset_y);
    /* What the guest's memory has to come out of.  Worth saying out loud:
     * the configured size is what gets asked for, and if it is not there the
     * machine does not come up at all. */
    m_Log.Write("MEM", LogNotice, "board %lu MB, heap free %lu MB, guest wants %lu MB",
                (unsigned long)(CMemorySystem::Get()->GetMemSize() >> 20),
                (unsigned long)(CMemorySystem::Get()->GetHeapFreeSpace(HEAP_ANY) >> 20),
                (unsigned long)(config.mem_size >> 20));

    m_PC = pc_new(circle_pc_redraw, nullptr, &m_Video, m_Video.vga_surface, &config);
    if (!m_PC && config.mem_size > 8L * 1024 * 1024) {
        /*
         * Falling back rather than stopping.  The memory size is chosen from
         * a menu, and the file it is saved in lives on the card - so a
         * machine that refuses to start because someone asked for more memory
         * than there is could only be rescued by taking the card out.
         */
        m_Log.Write("MEM", LogWarning, "%lu MB refused; falling back to 8 MB",
                    (unsigned long)(config.mem_size >> 20));
        config.mem_size = 8L * 1024 * 1024;
        m_PC = pc_new(circle_pc_redraw, nullptr, &m_Video, m_Video.vga_surface, &config);
    }
    if (!m_PC) { m_Log.Write("PC", LogError, "pc_new failed"); return; }
    LoadSoundFont();
    StartClock();

    /*
     * The guest's network card, put on the same wire as the board.
     *
     * Nothing of ours is listening on it - Circle's own stack is not started
     * - so the guest is simply another machine on the network: it asks the
     * DHCP server for an address itself and is reachable like anything else.
     */
    if (net_bind_host(m_PC->ne2000)) {
        CNetDevice *net = CNetDevice::GetNetDevice(0);
        CString mac;
        if (net && net->GetMACAddress()) net->GetMACAddress()->Format(&mac);
        /*
         * Asked twice, because the bit latches.
         *
         * Link status in the PHY's status register is latching-low: once the
         * link has been down it reads down until someone reads it, whatever
         * the cable is doing now.  Straight after boot that is always, so a
         * single read says "unplugged" about a perfectly good cable.
         */
        boolean link = FALSE;
        if (net) { net->IsLinkUp(); link = net->IsLinkUp(); }
        m_Log.Write("NET", LogNotice, "NE2000 at 0x300 irq 3, on %s%s",
                    (const char *)mac, link ? "" : " (no link)");
    } else {
        m_Log.Write("NET", LogNotice, "no Ethernet adapter; the card has nothing behind it");
    }
    if (!m_Audio.Initialize(m_Log)) {
        /* Leave the guest devices present, but do not advance their audio
         * streams if the physical HDMI sink did not come up. */
        m_PC->adlib_enabled = 0;
        m_PC->sb16_enabled = 0;
        m_PC->pcspk_enabled = 0;
        m_PC->covox_enabled = 0;
        m_PC->dss_enabled = 0;
    }

    /* The menus act on this machine, and a saved configuration is in
     * force from the first instruction rather than from the first time
     * someone opens a menu. */
    config_bind_pc(m_PC);
    config_apply_all();
    osd_init();
    settingsui_init();
    diskui_init();

    load_bios_and_reset(m_PC);
#ifdef TINY386_JIT
    {
        /* Before a guest instruction runs: if the translator is wrong about
         * something this simple, nothing after this point is trustworthy. */
        char verdict[96];
        if (!jit_init(m_PC->cpu)) {
            m_Log.Write("JIT", LogWarning, "no arena; interpreter only");
        } else {
            const int bad = jit_selftest(verdict, sizeof verdict);
            m_Log.Write(bad ? "JIT" : "JIT", bad ? LogError : LogNotice,
                        bad ? "SELFTEST FAILED: %s" : "selftest: %s", verdict);
        }
    }
#endif
    /*
     * Long enough to have been seen.
     *
     * Everything between the logo going up and here - the card, the guest's
     * memory, the BIOS - can take well under a second, and a logo that
     * flashes past is worse than none.  This waits only for what is left of
     * two seconds, so on a slow start it costs nothing at all.
     */
    if (m_LogoShownAt) {
        const uint64_t until = m_LogoShownAt + 2000000u;
        while (CTimer::GetClockTicks64() < until) {
            CTimer::SimpleusDelay(1000);
        }
    }
    m_Log.Write("PC", LogNotice, "BIOS reset complete; entering guest execution");
#if defined(CIRCLE_PC_DIAG)
    uint64_t next_diag_us = CTimer::GetClockTicks64() + 250000u;
    uint64_t display_diag_us = CTimer::GetClockTicks64() + 2000000u;
    /* The known protected-mode hand-off reaches 00124a58 around 14 seconds
     * after guest execution begins.  Keep the fallback later than that so it
     * cannot consume the one bounded snapshot before the target is seen. */
    const uint64_t execution_diag_fallback_us = CTimer::GetClockTicks64() + 25000000u;
    uint64_t target_seen_us = 0;
    bool display_diag_done = false;
    bool execution_diag_done = false;
    long last_cycle = cpui386_get_cycle(m_PC->cpu);
    uint32_t last_io_seq = 0;
    uint32_t last_cs = ~0u;
    uint32_t last_ip = ~0u;
    m_Log.Write("DIAG", LogNotice, "passive guest/I-O trace active; no device behavior changed");
#endif
    /*
     * Where the wall clock actually goes, once a second.
     *
     * "Slow" is not a diagnosis, and the three candidates here cost very
     * different things to fix: the interpreter itself (this build has no
     * JIT), the software VGA renderer, which walks every pixel of the visible
     * area on every retrace, and the OPL synthesis folded into pc_step().
     * Splitting them costs two clock reads per iteration and settles the
     * question in one run.
     */
#if defined(CIRCLE_PC_STATS)
    Pmu::Start();
    uint64_t pmu_cyc = Pmu::ReadCycles(), pmu_ins = Pmu::ReadEvent(0);
    uint64_t pmu_l1 = Pmu::ReadEvent(1), pmu_l2 = Pmu::ReadEvent(2);
    uint64_t pmu_br = Pmu::ReadEvent(3);
    uint64_t pmu_ir = Pmu::ReadEvent(4), pmu_ia = Pmu::ReadEvent(5);
#endif
    uint64_t perf_next = CTimer::GetClockTicks64() + 10000000u;
    uint64_t perf_step_us = 0, perf_audio_us = 0;
#if defined(CIRCLE_PC_STATS)
    uint64_t perf_gap_max = 0, perf_gap_prev = CTimer::GetClockTicks64(), perf_dry = 0;
#endif
    uint32_t perf_vga_us = g_vga_refresh_us, perf_vga_calls = g_vga_refresh_calls;
    long perf_cycle = cpui386_get_cycle(m_PC->cpu);
    /* From here core 1 mixes the sound; see CSynthCores::Run(). */
    m_Cores.StartAudio(&m_Audio, m_PC);
    /* The board has power again: come on, or wait for the button. */
    m_PowerStateSaved = LoadPowerState();
    const int restore = config_get_power_restore();
    if (restore == POWER_RESTORE_OFF || (restore == POWER_RESTORE_LAST && !m_PowerStateSaved))
        PowerOff("power returned");
    else {
        m_Panel.SetPower(true);
        SavePowerState(true);
    }
    for (unsigned tick=0;;++tick) {
        const uint64_t turn_t0 = CTimer::GetClockTicks64();
        const uint32_t turn_vga = g_vga_refresh_us;
        const uint64_t turn_ide = g_ide_us, turn_sync = g_ide_sync_us, turn_wr = g_ide_write_us;
        const long turn_cycle = cpui386_get_cycle(m_PC->cpu);
        UpdateInput();
        DrainInput();
        DrainSerialKeys();
        CFrontPanel::Event panel = m_Panel.Poll(turn_t0, config_get_power_button() ? 4000000u : 0u,
                                                g_ide_activity);
        if (m_SerialPanel) { panel = (CFrontPanel::Event)m_SerialPanel; m_SerialPanel = 0; }
        if (panel == CFrontPanel::EventReset) {
            m_Log.Write("PC", LogNotice, "reset button");
            m_PC->reset_request = 1;
        } else if (panel == CFrontPanel::EventPower)
            PowerOff("power button");
        CmosDateTime clock_set;
        if (m_RTC.Present() && cmos_clock_changed(m_PC->cmos, &clock_set)) {
            const bool ok = m_RTC.Write(&clock_set);
            m_Log.Write("RTC", ok ? LogNotice : LogWarning, "%s %04d-%02d-%02d %02d:%02d:%02d",
                        ok ? "guest set the clock:" : "could not store the guest's time",
                        clock_set.year, clock_set.month, clock_set.day,
                        clock_set.hour, clock_set.min, clock_set.sec);
        }
        const bool ui_open = settingsui_is_open() || diskui_is_open();
        const uint64_t perf_t0 = CTimer::GetClockTicks64();
        if (!ui_open) pc_step(m_PC);
        const uint64_t perf_t1 = CTimer::GetClockTicks64();
#if defined(CIRCLE_PC_STATS)
        {
            /* The distance between two of these is how long the guest stood
             * still - for the sound too, whose sources it feeds. */
            const uint64_t gap = perf_t1 - perf_gap_prev;
            if (gap > perf_gap_max) perf_gap_max = gap;
            if (gap > 50000u) perf_dry++;   /* longer than the queue itself */
            perf_gap_prev = perf_t1;
        }
#endif
        ide_sync_idle(0);
        /*
         * What the supply is doing, while the machine runs.
         *
         * Asked once at boot this says nothing about the moment that
         * matters: a CD drive spinning up draws its current then, and a
         * five volt rail that sags takes the USB hub down with everything
         * on it - which is seen from up here as a keyboard that vanished
         * and a guest that fell over.  The firmware latches it, so this
         * only has to notice the change.
         */
        {
            static uint64_t next_power;
            static u32 last_power = 0xFFFFFFFFu;
            const uint64_t now = CTimer::GetClockTicks64();
            if (now >= next_power) {
                next_power = now + 1000000u;
                CBcmPropertyTags Tags;
                TPropertyTagSimple Throttled;
                if (Tags.GetTag (PROPTAG_GET_THROTTLED, &Throttled,
                                 sizeof Throttled, 4)
                    && Throttled.nValue != last_power) {
                    last_power = Throttled.nValue;
                    m_Log.Write("POWER", LogWarning, "throttled now 0x%X",
                                Throttled.nValue);
                }
            }
        }
        if (ui_open) {
            m_PC->paused = 1;
            /* Core 2 draws the guest's frame into the same surface, and the
             * menu below swaps the surface the redraw copies from. */
            pc_vga_hold(1);
            /*
             * The menu is drawn somewhere else and shown in one go.
             *
             * The renderer writes straight into the scanout buffer, which is
             * right for the guest - it is what took the display cost down -
             * but wrong for this: every keypress repaints the whole frame,
             * backdrop first and panels after, and the screen is being read
             * out the whole time.  What that looks like is the menu blinking
             * each time the selection moves.  Composed off-screen and copied
             * over in one pass, there is no half-drawn frame to catch.
             */
            if (!m_OsdCompose)
                m_OsdCompose = (uint8_t *)malloc(640u * 480u * 4u);
            if (osd_take_dirty() && m_OsdCompose && m_Video.vga_surface) {
                if (!m_OsdWasOpen) {
                    /* The menu dims the frame it opened over, so the first
                     * pass has to be given that frame to work from. */
                    memcpy(m_OsdCompose, m_Video.vga_surface, 640u * 480u * 4u);
                }
                osd_render(m_OsdCompose, 640u * 4u, 640u, 480u);
                uint8_t *const guest = m_Video.vga_surface;
                m_Video.vga_surface = m_OsdCompose;
                circle_pc_redraw(&m_Video, 0, 0, 640, 480);
                m_Video.vga_surface = guest;
            }
            m_OsdWasOpen = true;
        } else if (m_PC->paused) {
            m_OsdWasOpen = false;
            /* Put the guest's own picture back.  It has not changed while the
             * menu covered it, so nothing else would repaint it - and the
             * bands above and below a 640x400 mode are never written by the
             * guest at all, so they keep whatever the menu left there. */
            m_PC->paused = 0;
            if (m_Video.vga_surface) memset(m_Video.vga_surface, 0, 640u * 480u * 4u);
            m_PC->full_update = 2;
            pc_vga_hold(0);
        }
        if (settingsui_restart_requested()) {
            /*
             * Not a change under a running guest: the menu has had it stopped
             * the whole time, this alters what the machine is, and the reset
             * below starts it again from the power-on vector.  DOS does not
             * survive it and is not meant to - the BIOS posts again and reads
             * the memory size back out of the CMOS, which is what a real PC
             * does when that setting is changed and the box is rebooted.
             */
            settingsui_clear_restart();
            pc_set_machine(m_PC, (long)config_get_mem_size_mb() * 1024 * 1024,
                           (long)config_get_vga_mem_kb() * 1024,
                           config_get_cpu_gen(), config_get_fpu());
            m_Log.Write("PC", LogNotice,
                        "machine now %d MB, video %d KB, cpu %d, fpu %d",
                        config_get_mem_size_mb(), config_get_vga_mem_kb(),
                        config_get_cpu_gen(), config_get_fpu());
            m_PC->reset_request = 1;
        }
        /* A changed hard disk is already swapped underneath; the reset is how
         * DOS gets told to look again. */
        if (diskui_reset_requested_flag) {
            diskui_reset_requested_flag = false;
            m_PC->reset_request = 1;
        }
        /*
         * Ctrl+Alt+Del.  The keyboard controller turns it into command FEh,
         * i8042.c calls the reset hook and pc.c raises pc->reset_request -
         * and there the chain ended, because the comment in pc.c says the
         * flag is serviced in main.c, which is the RP2350 front end.  This
         * loop never looked at it, so the guest could not be rebooted from
         * inside at all and a wedged DOS meant power-cycling the board.
         */
        if (m_PC->reset_request) {
            m_PC->reset_request = 0;
            m_Log.Write("PC", LogNotice, "guest requested reset; reloading BIOS");
            load_bios_and_reset(m_PC);
        }
        /* Windows' "Shut down" and everything else that halts with
         * interrupts off; also the Bochs "Shutdown" port.  See HLT(). */
        if (m_PC->cpu->power_off || m_PC->shutdown_state == 8)
            PowerOff("guest switched the machine off");
        const uint64_t perf_t2 = CTimer::GetClockTicks64();
        perf_step_us += perf_t1 - perf_t0;
        perf_audio_us += perf_t2 - perf_t1;
        if (perf_t2 - turn_t0 > LongTurnUs) {
            LongTurn &e = m_LongTurns[m_LongTurnHead++ % LongTurnN];
            e.t_ms   = (uint32_t)(perf_t2 / 1000u);
            e.total  = (uint32_t)(perf_t2 - turn_t0);
            e.input  = (uint32_t)(perf_t0 - turn_t0);
            e.step   = (uint32_t)(perf_t1 - perf_t0);
            e.after  = (uint32_t)(perf_t2 - perf_t1);
            e.vga    = g_vga_refresh_us - turn_vga;
            e.ide    = (uint32_t)(g_ide_us - turn_ide);
            e.sync   = (uint32_t)(g_ide_sync_us - turn_sync);
            e.write  = (uint32_t)(g_ide_write_us - turn_wr);
            e.cycles = (uint32_t)(cpui386_get_cycle(m_PC->cpu) - turn_cycle);
            e.cs     = (uint16_t)m_PC->cpu->seg[1].sel;
            e.ip     = (uint32_t)m_PC->cpu->ip;
        }
        /* Off unless asked for: these lines are long, and even buffered they
         * are work the guest does not need.  Build with -DCIRCLE_PC_STATS=1
         * when measuring. */
#if defined(CIRCLE_PC_STATS)
        if (perf_t2 >= perf_next) {
            const long cycle = cpui386_get_cycle(m_PC->cpu);
            const uint32_t vga_us = g_vga_refresh_us - perf_vga_us;
            const uint32_t vga_calls = g_vga_refresh_calls - perf_vga_calls;
            m_CPUThrottle.Update();
#ifdef TINY386_JIT
            {
                char line[192];
                if (jit_profile_report(line, sizeof line) > 0)
                    m_Log.Write("JITP", LogNotice, "%s", line);
            }
#endif
            {
                m_Log.Write("DISK", LogNotice,
                    "%lu reads %lu KB in %lu ms (worst %lu us) | %lu seeks in %lu ms"
                    " | audio gap worst %lu us, %lu over the queue",
                    (unsigned long)g_ide_calls, (unsigned long)(g_ide_bytes / 1024),
                    (unsigned long)(g_ide_us / 1000), (unsigned long)g_ide_max_us,
                    (unsigned long)g_ide_seeks, (unsigned long)(g_ide_seek_us / 1000),
                    (unsigned long)perf_gap_max, (unsigned long)perf_dry);
                g_ide_us = g_ide_bytes = g_ide_calls = g_ide_max_us = 0;
                g_ide_seek_us = g_ide_seeks = 0;
                perf_gap_max = 0; perf_dry = 0;
            }
            {
                /*
                 * What the part itself says the interpreter is doing.  The
                 * counters are 32-bit and wrap inside a ten-second window at
                 * this clock, so each is taken as a difference in 32 bits.
                 */
                const uint64_t c = Pmu::ReadCycles(), ins = Pmu::ReadEvent(0);
                const uint64_t l1 = Pmu::ReadEvent(1), l2 = Pmu::ReadEvent(2);
                const uint64_t br = Pmu::ReadEvent(3);
                const uint64_t ir = Pmu::ReadEvent(4), ia = Pmu::ReadEvent(5);
                const uint32_t dins = (uint32_t)(ins - pmu_ins);
                const uint32_t dl1 = (uint32_t)(l1 - pmu_l1);
                const uint32_t dl2 = (uint32_t)(l2 - pmu_l2);
                const uint32_t dbr = (uint32_t)(br - pmu_br);
                const uint32_t dir = (uint32_t)(ir - pmu_ir);
                const uint32_t dia = (uint32_t)(ia - pmu_ia);
                const uint64_t dcyc = c - pmu_cyc;
                const unsigned long gi = (unsigned long)(cycle - perf_cycle);
                /* Per thousand guest instructions, so the numbers stay whole. */
#if defined(CIRCLE_PC_PMU_STALLS)
                m_Log.Write("STALL", LogNotice,
                    "cycles per 1000 guest insn: %lu total | icache %lu, "
                    "load %lu, agu %lu, interlock %lu, decode %lu, queue %lu",
                    gi ? (unsigned long)(dcyc * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dins * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dl1 * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dl2 * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dbr * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dir * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dia * 1000ull / gi) : 0ul);
#else
                m_Log.Write("CPU", LogNotice,
                    "per 1000 guest insn: %lu host insn, %lu cycles, "
                    "%lu L1D refills, %lu L2D refills, %lu mispredicts, "
                    "%lu L1I refills of %lu fetches | IPC %lu.%02lu",
                    gi ? (unsigned long)(dins * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dcyc * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dl1 * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dl2 * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dbr * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dir * 1000ull / gi) : 0ul,
                    gi ? (unsigned long)(dia * 1000ull / gi) : 0ul,
                    dcyc ? (unsigned long)(dins * 100ull / dcyc) / 100ul : 0ul,
                    dcyc ? (unsigned long)(dins * 100ull / dcyc) % 100ul : 0ul);
#endif
                pmu_cyc = c; pmu_ins = ins; pmu_l1 = l1; pmu_l2 = l2; pmu_br = br;
                pmu_ir = ir; pmu_ia = ia;

                char hist[128];
                int n = 0, shown = 0;
                uint32_t used[256];
                memcpy(used, g_opcode_hist, sizeof used);
                while (shown < 8) {
                    unsigned best = 0;
                    for (unsigned i = 1; i < 256; i++)
                        if (used[i] > used[best]) best = i;
                    if (!used[best]) break;
                    const unsigned pct = gi ? (unsigned)(used[best] * 1000ull / gi) : 0;
                    n += snprintf(hist + n, (int)(sizeof hist) - n,
                                  "%02x:%u.%u%% ", best, pct / 10, pct % 10);
                    used[best] = 0;
                    shown++;
                }
                {
                    /* The eight opcodes that matter, with what each costs.
                     * The mix above consumed its own copy, so take a fresh
                     * one rather than reporting whatever it left behind. */
                    memcpy(used, g_opcode_hist, sizeof used);
                    char c[256]; int n = 0;
                    /* Ranked by the time they actually take, which is not
                     * the same question as how often they run. */
                    {
                        uint64_t tot[256];
                        memcpy(tot, g_opcode_cycles, sizeof tot);
                        uint64_t all = 0;
                        for (int i = 0; i < 256; i++) all += tot[i];
                        char t[256]; int m = 0;
                        for (int k = 0; k < 8; k++) {
                            int best = -1; uint64_t bv = 0;
                            for (int i = 0; i < 256; i++)
                                if (tot[i] > bv) { bv = tot[i]; best = i; }
                            if (best < 0 || !all) break;
                            m += snprintf(t + m, sizeof t - m, "%02x:%lu%% ", best,
                                (unsigned long)(tot[best] * 100 / all));
                            tot[best] = 0;
                        }
                        m_Log.Write("OPCT", LogNotice, "%s", t);
                    }
                    for (int k = 0; k < 8; k++) {
                        int best = -1; uint32_t bv = 0;
                        for (int i = 0; i < 256; i++)
                            if (used[i] > bv) { bv = used[i]; best = i; }
                        if (best < 0) break;
                        n += snprintf(c + n, sizeof c - n, "%02x:%luc ", best,
                            (unsigned long)(g_opcode_cycles[best] / (used[best] ? used[best] : 1)));
                        used[best] = 0;
                    }
                    m_Log.Write("OPC", LogNotice, "%s", c);
                }
                memset(g_opcode_cycles, 0, sizeof g_opcode_cycles);
                memset(g_opcode_hist, 0, sizeof g_opcode_hist);
                m_Log.Write("MIX", LogNotice, "%s", hist);
                {
                    char hot[128];
                    if (i386_hot_report(hot, sizeof hot) > 0)
                        m_Log.Write("HOT", LogNotice, "%s", hot);
                    uint32_t base = 0;
                    const uint8_t *code = 0;
                    static uint32_t dumped;
                    static unsigned dumped_mask;
                    const unsigned have = (unsigned)i386_hot_code(&base, &code);
                    if (base != dumped) {
                        dumped = base;
                        dumped_mask = 0;
                    }
                    for (int i = 0; i < 256; i += 32) {
                        const unsigned bit = 1u << (i / 32);
                        if ((have & bit) && !(dumped_mask & bit)) {
                            char line[128];
                            int p = 0;
                            for (int j = 0; j < 32; j++)
                                p += snprintf(line + p, (int)(sizeof line) - p,
                                              "%02x", code[i + j]);
                            m_Log.Write("HOTC", LogNotice, "%08lx %s",
                                        (unsigned long)(base + i), line);
                            dumped_mask |= bit;
                        }
                    }
                }
#if defined(CIRCLE_PC_XLAT)
                m_Log.Write("XLAT", LogNotice,
                    "%lu operand translations, %lu cycles each, %lu%% took the walk",
                    (unsigned long)g_xlat_calls,
                    g_xlat_calls ? (unsigned long)(g_xlat_cycles / g_xlat_calls) : 0ul,
                    g_xlat_calls ? (unsigned long)(g_xlat_slow * 100 / g_xlat_calls) : 0ul);
                g_xlat_cycles = g_xlat_calls = g_xlat_slow = 0;
#endif
                m_Log.Write("IO", LogNotice,
                    "%lu byte reads: %lu cycles deciding, %lu doing"
                    " | %lu cycles per 1000 guest insn",
                    (unsigned long)g_io_count,
                    g_io_count ? (unsigned long)(g_io_perm_cycles / g_io_count) : 0ul,
                    g_io_count ? (unsigned long)(g_io_read_cycles / g_io_count) : 0ul,
                    gi ? (unsigned long)((g_io_perm_cycles + g_io_read_cycles)
                                         * 1000ull / gi) : 0ul);
                {
                    /* The Sound Blaster conversation since the last window,
                     * while there is one: a driver that gives up does so in
                     * the first few dozen accesses. */
                    m_Log.Write("SBIO", LogNotice,
                                "card %s | dsp ports: %lu reads, %lu writes",
                                m_PC->sb16_enabled ? "on" : "off",
                                (unsigned long)g_sb_port_reads,
                                (unsigned long)g_sb_port_writes);
                    m_Log.Write("SBIO", LogNotice,
                                "starved %lu frames, smallest fill %lu bytes",
                                (unsigned long)g_sb16_starves,
                                (unsigned long)(g_sb16_minfill == 0xffffffffu
                                                ? 0u : g_sb16_minfill));
                    m_Log.Write("SBIO", LogNotice,
                                "dma callback %lu times, %lu bytes copied",
                                (unsigned long)g_sb16_dma_calls,
                                (unsigned long)g_sb16_dma_bytes);
                    g_sb16_dma_calls = g_sb16_dma_bytes = 0;
                    m_Log.Write("SBGAP", LogNotice,
                        "%lu holes, %lu of them inside an effect,"
                        " longest %lu ms | %lu ms of silence in all",
                        (unsigned long)g_sb16_gap_runs,
                        (unsigned long)g_sb16_gap_short,
                        (unsigned long)(g_sb16_gap_worst * 1000u / SOUND_FREQUENCY),
                        (unsigned long)(g_sb16_gap_frames * 1000u / SOUND_FREQUENCY));
                    g_sb16_gap_runs = g_sb16_gap_short = 0;
                    g_sb16_gap_worst = g_sb16_gap_frames = 0;
                    if (g_sb16_dbg_have) {
                        char b[110];
                        int n = 0;
                        for (int i = 0; i < 32; i++)
                            n += snprintf(b + n, (int)(sizeof b) - n, "%02x ",
                                          g_sb16_dbg_bytes[i]);
                        m_Log.Write("SBFMT", LogNotice,
                                    "%lu Hz, fmt %lu, stereo %lu, adpcm %lu bits"
                                    " | loudest byte is %lu from centre",
                                    (unsigned long)g_sb16_dbg_freq,
                                    (unsigned long)g_sb16_dbg_fmt,
                                    (unsigned long)g_sb16_dbg_stereo,
                                    (unsigned long)g_sb16_dbg_bits,
                                    (unsigned long)g_sb16_dbg_have);
                        m_Log.Write("SBFMT", LogNotice, "%s", b);
                        g_sb16_dbg_have = 0;
                    }
                    g_sb16_starves = 0;
                    g_sb16_minfill = 0xffffffffu;
                    g_sb_port_reads = g_sb_port_writes = 0;
                    uint32_t ring[48];
                    const unsigned rn = sb16_take_ring(ring, 48);
                    for (unsigned i = 0; i < rn; i += 8) {
                        char line[160];
                        int p = 0;
                        for (unsigned j = i; j < rn && j < i + 8; j++)
                            p += snprintf(line + p, (int)(sizeof line) - p,
                                          "%03x%c%02x ", ring[j] >> 16,
                                          (ring[j] & 1) ? '>' : '<',
                                          (ring[j] >> 8) & 0xff);
                        m_Log.Write("SB", LogNotice, "%s", line);
                    }
                }
                g_io_perm_cycles = g_io_read_cycles = 0;
                g_io_count = 0;
            }
            /* Everything above is a rate, and a rate means nothing without
             * the clock it was measured at.  The throttle lowers this when
             * the part gets hot. */
            m_Log.Write("CLK", LogNotice, "arm %u MHz (max %u), %u C",
                        m_CPUThrottle.GetClockRate() / 1000000,
                        m_CPUThrottle.GetMaxClockRate() / 1000000,
                        m_CPUThrottle.GetTemperature());
            m_Log.Write("PERF", LogNotice,
                "10s: guest=%lu kIPS | pc_step=%lu ms (vga %lu ms in %lu refreshes)"
                " | after=%lu ms | irq0 %lu/%lu | opl w=%lu keyon=%lu chip=%lu"
                " | opl rend=%lu quiet=%lu fill=%lu mixed=%lu underrun=%lu"
                " | tlb refill=%lu clear=%lu",
                /* Per second, not per window.  This divided a ten-second
                 * count by a thousand and called the answer kIPS, so every
                 * reading taken from it was ten times the real rate - which
                 * made the machine look like a Pentium when the processor's
                 * own counters were saying 486. */
                (unsigned long)((cycle - perf_cycle) / 10000),
                (unsigned long)(perf_step_us / 1000), (unsigned long)(vga_us / 1000),
                (unsigned long)vga_calls, (unsigned long)(perf_audio_us / 1000),
                (unsigned long)g_pic_irq0_accepted, (unsigned long)g_pit_irq0_edges,
                (unsigned long)g_adlib_writes, (unsigned long)g_adlib_w_keyon,
                (unsigned long)g_adlib_render_peak,
                (unsigned long)g_adlib_rendered, (unsigned long)g_adlib_skips,
                (unsigned long)g_adlib_fill, (unsigned long)g_adlib_calls,
                (unsigned long)g_adlib_underrun_total,
                (unsigned long)g_wl_tlb_refills,
                (unsigned long)g_wl_tlb_clears);
            /* How the guest draws: framebuffer accesses by width, and how
             * many refreshes found nothing written since the last. */
            m_Log.Write("LFB", LogNotice,
                "rd8/16/32=%lu/%lu/%lu wr8/16/32=%lu/%lu/%lu str=%lu (%lu KB) | refresh skipped %lu",
                (unsigned long)g_lfb_rd[0], (unsigned long)g_lfb_rd[1], (unsigned long)g_lfb_rd[2],
                (unsigned long)g_lfb_wr[0], (unsigned long)g_lfb_wr[1], (unsigned long)g_lfb_wr[2],
                (unsigned long)g_lfb_str, (unsigned long)(g_lfb_str_bytes / 1024),
                (unsigned long)g_vga_refresh_skips);
            {
                char line[256];
                int p = 0;
                for (int b = 0; b < 2; b++) {
                    p += snprintf(line + p, sizeof line - p, "bank%d keyon %lu lr %lu cnt %lu ws", b,
                                  (unsigned long)g_opl3_keyons[b], (unsigned long)g_opl3_c0_lr[b],
                                  (unsigned long)g_opl3_c0_cnt[b]);
                    for (int w = 0; w < 8; w++)
                        p += snprintf(line + p, sizeof line - p, "%c%lu", w ? '/' : ' ',
                                      (unsigned long)g_opl3_ws[b][w]);
                    p += snprintf(line + p, sizeof line - p, " | ");
                }
                snprintf(line + p, sizeof line - p, "reg1 %02lx 4op %02lx (%lu) bank1 w %lu",
                         (unsigned long)g_opl3_reg1_last, (unsigned long)g_opl3_4op_last,
                         (unsigned long)g_opl3_4op_nonzero, (unsigned long)g_opl3_bank1_writes);
                m_Log.Write("OPL3", LogNotice, "%s", line);
                memset(g_opl3_ws, 0, sizeof g_opl3_ws);
                memset(g_opl3_keyons, 0, sizeof g_opl3_keyons);
                memset(g_opl3_c0_lr, 0, sizeof g_opl3_c0_lr);
                memset(g_opl3_c0_cnt, 0, sizeof g_opl3_c0_cnt);
                g_opl3_4op_nonzero = 0; g_opl3_bank1_writes = 0;
            }
            m_Log.Write("CORES", LogNotice,
                "core1 %lu turns, %lu with work, %lu sleeps, asleep %lu ms | "
                "core2 %lu turns, %lu with work, %lu sleeps, asleep %lu ms | "
                "core3 %lu turns, %lu with work, %lu sleeps, asleep %lu ms",
                (unsigned long)g_core_iter[1], (unsigned long)g_core_work[1],
                (unsigned long)g_core_wfe[1], (unsigned long)(g_core_wfe_us[1] / 1000),
                (unsigned long)g_core_iter[2], (unsigned long)g_core_work[2],
                (unsigned long)g_core_wfe[2], (unsigned long)(g_core_wfe_us[2] / 1000),
                (unsigned long)g_core_iter[3], (unsigned long)g_core_work[3],
                (unsigned long)g_core_wfe[3], (unsigned long)(g_core_wfe_us[3] / 1000));
            memset(g_core_iter, 0, sizeof g_core_iter); memset(g_core_work, 0, sizeof g_core_work);
            memset(g_core_wfe, 0, sizeof g_core_wfe); memset(g_core_wfe_us, 0, sizeof g_core_wfe_us);
            m_Log.Write("MIDI", LogNotice, "most voices at once %lu, peak %lu, clipped %lu",
                        (unsigned long)g_gm_voices_max, (unsigned long)g_gm_peak,
                        (unsigned long)g_gm_clipped);
            g_gm_voices_max = g_gm_peak = g_gm_clipped = 0;
            m_Log.Write("STEP", LogNotice,
                "%lu steps | pit/kbd %lu ms, dma %lu, sb16 %lu, fdc %lu, net %lu, poll %lu",
                (unsigned long)g_step_steps,
                (unsigned long)(g_step_dev_us[0] / 1000), (unsigned long)(g_step_dev_us[1] / 1000),
                (unsigned long)(g_step_dev_us[2] / 1000), (unsigned long)(g_step_dev_us[3] / 1000),
                (unsigned long)(g_step_dev_us[4] / 1000), (unsigned long)(g_step_dev_us[5] / 1000));
            memset(g_step_dev_us, 0, sizeof g_step_dev_us); g_step_steps = 0;
            memset(g_lfb_rd, 0, sizeof g_lfb_rd); memset(g_lfb_wr, 0, sizeof g_lfb_wr);
            g_lfb_str = g_lfb_str_bytes = g_vga_refresh_skips = 0;
            perf_next = perf_t2 + 10000000u;
            perf_step_us = perf_audio_us = 0;
            perf_vga_us = g_vga_refresh_us; perf_vga_calls = g_vga_refresh_calls;
            perf_cycle = cycle;
        }
#else
        (void)perf_next; (void)perf_step_us; (void)perf_audio_us;
        (void)perf_vga_us; (void)perf_vga_calls; (void)perf_cycle;
#endif
        /* Logging is blocking on a polled UART, and a long line costs tens of
         * milliseconds during which no device is serviced.  While measuring
         * interrupt delivery that is not background noise, it is the thing
         * being measured, so keep this one off unless it is what is wanted. */
#if defined(CIRCLE_PC_VIDEO_TRACE)
        log_video_geometry(m_Log, m_PC);
#else
        (void)log_video_geometry;
#endif
#if defined(CIRCLE_PC_DIAG)
        const uint64_t now_us = CTimer::GetClockTicks64();
        if (now_us >= next_diag_us) {
            const long cycle = cpui386_get_cycle(m_PC->cpu);
            const uint32_t cs_base = m_PC->cpu->seg[1].base; /* CS */
            const uint32_t ip = m_PC->cpu->ip;
            CirclePcDiagSnapshot diag{};
            circle_pc_diag_snapshot(&diag);
            const CirclePcIoDiag &io = diag.io[diag.io_seq & (CIRCLE_PC_IO_RING - 1u)];
            const uint32_t pic = i8259_debug_master(m_PC->pic);
            const int pit_reload = pit_get_initial_count(m_PC->pit, 0);
            const int pit_mode = pit_get_mode(m_PC->pit, 0);
            m_Log.Write("DIAG", LogNotice,
                "cycles=%lu dcycles=%ld CS=%04lx IP=%08lx linear=%08lx "
                "DS=%08lx SS=%08lx ES=%08lx flags=%08lx cr0=%08lx halt=%u "
                "io_seq=%lu dio=%ld %s%u port=%04x value=%08lx str=%ld",
                (unsigned long)cycle, cycle - last_cycle,
                (unsigned long)m_PC->cpu->seg[1].sel, (unsigned long)ip,
                (unsigned long)(cs_base + ip),
                (unsigned long)m_PC->cpu->seg[3].base, /* DS */
                (unsigned long)m_PC->cpu->seg[2].base, /* SS */
                (unsigned long)m_PC->cpu->seg[0].base, /* ES */
                (unsigned long)m_PC->cpu->flags, (unsigned long)m_PC->cpu->cr0,
                m_PC->cpu->halt ? 1u : 0u,
                (unsigned long)diag.io_seq, (long)(diag.io_seq - last_io_seq),
                io.direction ? "OUT" : "IN", (unsigned)io.width,
                (unsigned)io.port, (unsigned long)io.value,
                (long)(io.is_string ? io.string_count : 0));
            m_Log.Write("DIAG", LogNotice,
                "PIT0 reload=%u mode=%u PIC master last=%02x irr=%02x imr=%02x isr=%02x "
                "INTR=%u IRQ0 raised=%lu delivered=%lu I/O-window=%lu..%lu",
                (unsigned)pit_reload, (unsigned)pit_mode,
                (unsigned)(pic & 0xffu), (unsigned)((pic >> 8) & 0xffu),
                (unsigned)((pic >> 16) & 0xffu), (unsigned)((pic >> 24) & 0xffu),
                m_PC->cpu->intr ? 1u : 0u,
                (unsigned long)diag.irq0_raised,
                (unsigned long)diag.irq0_delivered,
                (unsigned long)(diag.io_seq > CIRCLE_PC_IO_RING ?
                    diag.io_seq - CIRCLE_PC_IO_RING + 1u : 1u),
                (unsigned long)diag.io_seq);
            if (last_cs != m_PC->cpu->seg[1].sel || last_ip != ip) {
                m_Log.Write("DIAG", LogNotice,
                    "guest CS:IP change %04lx:%08lx (linear %08lx)",
                    (unsigned long)m_PC->cpu->seg[1].sel, (unsigned long)ip,
                    (unsigned long)(cs_base + ip));
                last_cs = m_PC->cpu->seg[1].sel;
                last_ip = ip;
            }
            last_cycle = cycle;
            last_io_seq = diag.io_seq;
            /* First report is after 250 ms, all later reports are 1 Hz. */
            next_diag_us = now_us + 1000000u;
        }
        if (!display_diag_done && now_us >= display_diag_us) {
            log_guest_display_diag(m_Log, m_PC);
            display_diag_done = true;
        }
        /* The former PC diagnostic observed the Windows boot path at
         * 00000048:00000258 (linear 00124a58) with paging enabled.  Do not
         * change execution to catch it: observe it after each ordinary
         * pc_step(), then take one richer snapshot two seconds later.  The
         * twenty-five-second fallback keeps the diagnostic bounded if the guest
         * never reaches that exact path. */
        const uint32_t current_linear = m_PC->cpu->seg[1].base + m_PC->cpu->ip;
        if (!target_seen_us && (m_PC->cpu->cr0 & 1u) && current_linear == 0x00124a58u) {
            target_seen_us = now_us;
            m_Log.Write("EXEC", LogNotice,
                "observed protected-mode target linear=00124a58; detailed snapshot due in 2 seconds");
        }
        if (!execution_diag_done &&
            ((target_seen_us && now_us - target_seen_us >= 2000000u) ||
             (!target_seen_us && now_us >= execution_diag_fallback_us))) {
            log_guest_execution_diag(m_Log, m_PC,
                target_seen_us ? "2s after linear 00124a58" :
                "25s fallback; linear 00124a58 not observed");
            execution_diag_done = true;
        }
#endif
        /* Do not call m_Screen.Update() here.  Its terminal compositor owns
         * a separate blank backing buffer and would periodically paint it
         * over the VGA framebuffer; Circle scans out the raw buffer directly. */
    }
}
