#pragma once
#include <circle/gpiopin.h>
#include <stdint.h>

/*
 * A PC case's front panel on the GPIO header, all of it within pins 29-34:
 *
 *   pin 29  GPIO5   power button, to ground
 *   pin 30  ground  for both buttons
 *   pin 31  GPIO6   reset button, to ground
 *   pin 32  GPIO12  power light, anode
 *   pin 33  GPIO13  disk light, anode
 *   pin 34  ground  for both lights' cathodes
 *
 * The buttons use the Pi's own pull-ups, so a case's bare switches go
 * straight on.  The lights are driven at 3.3 V and want a series resistor
 * each - a motherboard header has one on the board, a GPIO pin does not.
 *
 * Nothing needs to be fitted: with nothing on the pins the buttons read as
 * released and the lights drive nothing.
 */
class CFrontPanel {
public:
    enum Event { EventNone, EventPower, EventReset };

    CFrontPanel();

    /*
     * Called on every turn of the main loop; looks at the pins at most
     * every 10 ms, so it costs one clock read otherwise.  hold_us is how
     * long the power button must be held before it counts: 0 for a press.
     * disk_count is anything that changes when a drive is busy.
     */
    Event Poll(uint64_t now_us, uint64_t hold_us, uint32_t disk_count);

    void SetPower(bool on);
    bool PowerHeld() const { return m_PowerDown; }

private:
    CGPIOPin m_PowerButton, m_ResetButton, m_PowerLed, m_DiskLed;
    uint64_t m_NextPoll, m_PowerSince, m_DiskUntil;
    uint32_t m_DiskCount;
    bool m_PowerDown, m_ResetDown, m_PowerSent, m_DiskOn;
    unsigned char m_PowerSamples, m_ResetSamples;
};
