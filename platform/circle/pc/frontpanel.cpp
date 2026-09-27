#include "frontpanel.h"

#define PIN_POWER_BUTTON    5
#define PIN_RESET_BUTTON    6
#define PIN_POWER_LED       12
#define PIN_DISK_LED        13

#define POLL_US             10000u
/* Long enough to see: a disk command is over in well under a millisecond. */
#define DISK_LIGHT_US       50000u

CFrontPanel::CFrontPanel()
    : m_PowerButton(PIN_POWER_BUTTON, GPIOModeInputPullUp),
      m_ResetButton(PIN_RESET_BUTTON, GPIOModeInputPullUp),
      m_PowerLed(PIN_POWER_LED, GPIOModeOutput),
      m_DiskLed(PIN_DISK_LED, GPIOModeOutput),
      m_NextPoll(0), m_PowerSince(0), m_DiskUntil(0), m_DiskCount(0),
      m_PowerDown(false), m_ResetDown(false), m_PowerSent(false), m_DiskOn(false),
      m_PowerSamples(0), m_ResetSamples(0)
{
    m_PowerLed.Write(LOW);
    m_DiskLed.Write(LOW);
}

void CFrontPanel::SetPower(bool on)
{
    m_PowerLed.Write(on ? HIGH : LOW);
    if (!on) {
        m_DiskLed.Write(LOW);
        m_DiskOn = false;
    }
}

/*
 * A switch bounces for a few milliseconds when it closes and opens, so a
 * button only changes state after reading the same way on three polls in a
 * row - 20-30 ms, still far quicker than a finger.
 */
static bool debounce(bool pressed_now, bool &state, unsigned char &samples)
{
    if (pressed_now == state) { samples = 0; return false; }
    if (++samples < 3) return false;
    samples = 0;
    state = pressed_now;
    return true;
}

CFrontPanel::Event CFrontPanel::Poll(uint64_t now_us, uint64_t hold_us, uint32_t disk_count)
{
    if (disk_count != m_DiskCount) {
        m_DiskCount = disk_count;
        m_DiskUntil = now_us + DISK_LIGHT_US;
        if (!m_DiskOn) { m_DiskLed.Write(HIGH); m_DiskOn = true; }
    }
    if (now_us < m_NextPoll) return EventNone;
    m_NextPoll = now_us + POLL_US;
    if (m_DiskOn && now_us >= m_DiskUntil) { m_DiskLed.Write(LOW); m_DiskOn = false; }

    Event ev = EventNone;
    if (debounce(m_ResetButton.Read() == LOW, m_ResetDown, m_ResetSamples) && m_ResetDown)
        ev = EventReset;
    if (debounce(m_PowerButton.Read() == LOW, m_PowerDown, m_PowerSamples)) {
        m_PowerSince = now_us;
        m_PowerSent = false;
    }
    /* Once per press, when it has been held long enough. */
    if (m_PowerDown && !m_PowerSent && now_us - m_PowerSince >= hold_us) {
        m_PowerSent = true;
        ev = EventPower;
    }
    return ev;
}
