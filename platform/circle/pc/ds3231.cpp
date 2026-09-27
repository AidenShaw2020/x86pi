#include "ds3231.h"

#define DS3231_ADDR     0x68
#define DS3231_TIME     0x00    /* seconds .. year, seven registers */
#define DS3231_STATUS   0x0f
#define STATUS_OSF      0x80    /* the oscillator has stopped at some point */

static int bcd(unsigned char v) { return (v >> 4) * 10 + (v & 15); }
static unsigned char tobcd(int v) { return (unsigned char)(((v / 10) << 4) | (v % 10)); }

CDs3231::CDs3231() : m_I2C(1), m_Present(false)
{
}

int CDs3231::ReadRegs(unsigned char reg, unsigned char *buf, unsigned n)
{
    return m_I2C.WriteReadRepeatedStart(DS3231_ADDR, &reg, 1, buf, n);
}

bool CDs3231::Initialize()
{
    if (!m_I2C.Initialize())
        return false;
    unsigned char status;
    m_Present = ReadRegs(DS3231_STATUS, &status, 1) == 1;
    return m_Present;
}

bool CDs3231::Read(CmosDateTime *dt)
{
    unsigned char r[7], status;
    if (!m_Present || ReadRegs(DS3231_STATUS, &status, 1) != 1 ||
        (status & STATUS_OSF) || ReadRegs(DS3231_TIME, r, 7) != 7)
        return false;
    dt->sec = bcd(r[0] & 0x7f);
    dt->min = bcd(r[1] & 0x7f);
    if (r[2] & 0x40)    /* 12-hour mode, from whatever set it last */
        dt->hour = bcd(r[2] & 0x1f) % 12 + ((r[2] & 0x20) ? 12 : 0);
    else
        dt->hour = bcd(r[2] & 0x3f);
    dt->day = bcd(r[4] & 0x3f);
    dt->month = bcd(r[5] & 0x1f);
    /* The century bit is the chip's own count of 2099 rolling over; the
     * CMOS clock only goes as far as 2099, so leave it out. */
    dt->year = 2000 + bcd(r[6]);
    return dt->sec < 60 && dt->min < 60 && dt->hour < 24 &&
           dt->day >= 1 && dt->day <= 31 && dt->month >= 1 && dt->month <= 12;
}

bool CDs3231::Write(const CmosDateTime *dt)
{
    if (!m_Present)
        return false;
    /* Day of the week, Monday 1; the chip only counts it on. */
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    const int y = dt->year - (dt->month < 3);
    const int dow = (y + y / 4 - y / 100 + y / 400 + t[dt->month - 1] + dt->day) % 7;
    const unsigned char w[8] = {
        DS3231_TIME, tobcd(dt->sec), tobcd(dt->min), tobcd(dt->hour),
        (unsigned char)(dow ? dow : 7), tobcd(dt->day), tobcd(dt->month),
        tobcd(dt->year % 100),
    };
    if (m_I2C.Write(DS3231_ADDR, w, sizeof w) != (int)sizeof w)
        return false;
    /* The time is good again: clear the stopped-oscillator flag. */
    unsigned char status;
    if (ReadRegs(DS3231_STATUS, &status, 1) != 1)
        return false;
    const unsigned char s[2] = { DS3231_STATUS, (unsigned char)(status & ~STATUS_OSF) };
    return m_I2C.Write(DS3231_ADDR, s, 2) == 2;
}
