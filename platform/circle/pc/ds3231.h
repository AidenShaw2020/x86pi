#pragma once
#include <circle/i2cmaster.h>
extern "C" {
#include "../../../src/misc.h"
}

/*
 * A DS3231 battery-backed clock on the Pi's I2C1 (GPIO 2 and 3), where the
 * guest's CMOS clock starts from and where a time the guest sets is kept.
 *
 * Optional: with nothing on the bus the controller sees no acknowledge and
 * gives up within one byte, so looking for the module costs nothing when it
 * is not fitted and the machine simply starts from the build time as before.
 */
class CDs3231 {
public:
    CDs3231();

    /* Looks for the module; false when there is none. */
    bool Initialize();
    bool Present() const { return m_Present; }

    /* False when the module is missing or its clock is not to be trusted:
     * the oscillator stopped - a flat or missing battery - since the time
     * was last set. */
    bool Read(CmosDateTime *dt);
    bool Write(const CmosDateTime *dt);

private:
    int ReadRegs(unsigned char reg, unsigned char *buf, unsigned n);

    CI2CMaster m_I2C;
    bool m_Present;
};
