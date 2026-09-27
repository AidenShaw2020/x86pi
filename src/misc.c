#include "misc.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <time.h>
#include <unistd.h>

#if !defined(_WIN32) && !defined(__wasm__) && !defined(CIRCLE_BUILD)
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#endif
#ifdef BUILD_ESP32
#include "driver/uart.h"
#endif

#if !defined(_WIN32) && !defined(__wasm__) && !defined(CIRCLE_BUILD)
static void CtrlC(int _)
{
	exit( 0 );
}

static void ResetKeyboardInput()
{
	// Re-enable echo, etc. on keyboard.
	struct termios term;
	tcgetattr(0, &term);
	term.c_lflag |= ICANON | ECHO;
	tcsetattr(0, TCSANOW, &term);
}

// Override keyboard, so we can capture all keyboard input for the VM.
void CaptureKeyboardInput()
{
	// Hook exit, because we want to re-enable keyboard.
#ifndef BUILD_ESP32
	atexit(ResetKeyboardInput);
	signal(SIGINT, CtrlC);
#endif

	struct termios term;
	tcgetattr(0, &term);
	term.c_lflag &= ~(ICANON | ECHO | ISIG); // Disable echo as well
	tcsetattr(0, TCSANOW, &term);
}

static int ReadKBByte()
{
#ifdef BUILD_ESP32
	char data;
	if (uart_read_bytes(0, &data, 1, 20 / portTICK_PERIOD_MS) > 0) {
		return data;
	}
	return -1;
#else
	char rxchar = 0;
	int rread = read(fileno(stdin), (char*)&rxchar, 1);
	if( rread > 0 ) // Tricky: getchar can't be used with arrow keys.
		return rxchar;
	else
		abort();
#endif
}

static int IsKBHit()
{
#ifdef BUILD_ESP32
	size_t len;
	if (uart_get_buffered_data_len(0, &len) == ESP_OK) {
		if (len)
			return 1;
	}
	return 0;
#else
	int byteswaiting;
	ioctl(0, FIONREAD, &byteswaiting);
	return !!byteswaiting;
#endif
}
#endif

/* sysprog21/semu */
struct U8250 {
	uint8_t dll, dlh;
	uint8_t lcr;
	uint8_t ier;
	uint8_t mcr;
	uint8_t ioready;
	int out_fd;
	uint8_t in;

	int irq;
	void *pic;
	void (*set_irq)(void *pic, int irq, int level);
};

U8250 *u8250_init(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level))
{
	U8250 *s = malloc(sizeof(U8250));
	memset(s, 0, sizeof(U8250));
	s->out_fd = 1;

	s->irq = irq;
	s->pic = pic;
	s->set_irq = set_irq;
	return s;
}

struct CMOS {
	uint8_t data[128];
	int index;
	int irq;
	uint32_t irq_timeout;
	uint32_t irq_period;
	void *pic;
	void (*set_irq)(void *pic, int irq, int level);
	/* The clock, as seconds since 2000-01-01 00:00 at time_us_64() == 0;
	 * see cmos_update_time(). */
	int64_t rtc_base_s;
	/* When the guest last wrote a time or date register, 0 once the
	 * change has been handed on by cmos_clock_changed(). */
	uint64_t rtc_set_us;
};

static int bin2bcd(int a)
{
	return ((a / 10) << 4) | (a % 10);
}

static int month_from_str(const char *m)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (int i = 0; i < 12; i++) {
        if (strncmp(m, months + i * 3, 3) == 0)
            return i + 1;
    }
    return 1;
}

static int bcd2bin(int a)
{
	return ((a >> 4) & 15) * 10 + (a & 15);
}

/* Days since 2000-01-01 for a date in 2000..2099, and back. */
static int64_t rtc_days(int y, int m, int d)
{
	static const int cum[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
	int64_t days = (int64_t)(y - 2000) * 365 + (y - 2000 + 3) / 4;
	days += cum[(m - 1) % 12] + (d - 1);
	if (m > 2 && y % 4 == 0) days++;
	return days;
}

static int64_t rtc_from_regs(CMOS *s)
{
	int year = bcd2bin(s->data[0x32]) * 100 + bcd2bin(s->data[9]);
	if (year < 2000 || year > 2099) year = 2000 + bcd2bin(s->data[9]);
	int month = bcd2bin(s->data[8]); if (month < 1 || month > 12) month = 1;
	int day = bcd2bin(s->data[7]); if (day < 1 || day > 31) day = 1;
	return rtc_days(year, month, day) * 86400 +
	       bcd2bin(s->data[4]) * 3600 + bcd2bin(s->data[2]) * 60 +
	       bcd2bin(s->data[0]);
}

static int64_t rtc_elapsed_s(void)
{
	return (int64_t)(time_us_64() / 1000000u);
}

/* Seconds since 2000-01-01 as a date; returns the day of the week, counting
 * Sunday as 1 the way the register does. */
static int rtc_split(int64_t t, CmosDateTime *dt)
{
	if (t < 0) t = 0;
	int64_t days = t / 86400;
	const int secs = (int)(t % 86400);
	/* 2000-01-01 was a Saturday. */
	const int wday = (int)((days + 6) % 7) + 1;
	int year = 2000;
	for (;;) {
		const int len = (year % 4 == 0) ? 366 : 365;
		if (days < len) break;
		days -= len; year++;
	}
	static const int mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
	int month = 1;
	for (; month <= 12; month++) {
		const int len = mdays[month - 1] + (month == 2 && year % 4 == 0);
		if (days < len) break;
		days -= len;
	}
	dt->year = year;
	dt->month = month;
	dt->day = (int)days + 1;
	dt->hour = secs / 3600;
	dt->min = (secs / 60) % 60;
	dt->sec = secs % 60;
	return wday;
}

/*
 * The time and date registers, from a clock that runs.
 *
 * They used to be set to the moment this file was compiled on every read,
 * so the seconds never moved.  Anything that waits for the RTC to tick
 * waited for ever: with BootMulti=1 in MSDOS.SYS, Windows 95's IO.SYS
 * times its "Starting Windows 95" pause off the clock and sat there until
 * a key was pressed, and Windows itself came up at the same minute every
 * boot.  The clock starts from a DS3231 on the board when there is one
 * (cmos_set_clock()), and from the build time when there is not; either
 * way it moves on from there, and a time the guest sets (DOS TIME and
 * DATE) is kept - and given back to the DS3231 (cmos_clock_changed()).
 */
static void cmos_update_time(CMOS *s)
{
	CmosDateTime dt;
	const int wday = rtc_split(s->rtc_base_s + rtc_elapsed_s(), &dt);
	s->data[0] = bin2bcd(dt.sec);
	s->data[2] = bin2bcd(dt.min);
	s->data[4] = bin2bcd(dt.hour);
	s->data[6] = bin2bcd(wday);
	s->data[7] = bin2bcd(dt.day);
	s->data[8] = bin2bcd(dt.month);
	s->data[9] = bin2bcd(dt.year % 100);
	s->data[0x32] = bin2bcd(dt.year / 100);
}

void cmos_set_clock(CMOS *s, const CmosDateTime *dt)
{
	s->rtc_base_s = rtc_days(dt->year, dt->month, dt->day) * 86400 +
	                dt->hour * 3600 + dt->min * 60 + dt->sec - rtc_elapsed_s();
}

int cmos_clock_changed(CMOS *s, CmosDateTime *dt)
{
	/* The registers are set one at a time - seconds, minutes, hours - so
	 * wait for the last of them rather than hand on a time half set. */
	if (!s->rtc_set_us || time_us_64() - s->rtc_set_us < 100000u)
		return 0;
	s->rtc_set_us = 0;
	rtc_split(s->rtc_base_s + rtc_elapsed_s(), dt);
	return 1;
}

/* Where the clock starts: the moment this file was compiled. */
static void cmos_start_clock(CMOS *s)
{
    /* Parse compile date: "Mmm dd yyyy" */
    const char *d = __DATE__;

    int month = month_from_str(d);
    int day = (d[4] == ' ') ? (d[5] - '0') : (10 * (d[4] - '0') + (d[5] - '0'));
    int year = (d[7] - '0') * 1000 +
               (d[8] - '0') * 100 +
               (d[9] - '0') * 10 +
               (d[10] - '0');

    /* Optional: parse compile time */
    const char *t = __TIME__;
    int hour = (t[0] - '0') * 10 + (t[1] - '0');
    int min  = (t[3] - '0') * 10 + (t[4] - '0');
    int sec  = (t[6] - '0') * 10 + (t[7] - '0');

    s->data[0] = bin2bcd(sec);
    s->data[2] = bin2bcd(min);
    s->data[4] = bin2bcd(hour);

    /* weekday неизвестен — можно оставить 1 */
    s->data[6] = bin2bcd(1);

    s->data[7] = bin2bcd(day);
    s->data[8] = bin2bcd(month);
    s->data[9] = bin2bcd(year % 100);

    /* century */
    s->data[0x32] = bin2bcd(year / 100);
    s->rtc_base_s = rtc_from_regs(s) - rtc_elapsed_s();
}

void cmos_set_floppy_types(CMOS *c, uint8_t type_a, uint8_t type_b) {
    if (!c) return;
    c->data[0x10] = ((type_a & 0xF) << 4) | (type_b & 0xF);
}

CMOS *cmos_init(long mem_size, int irq, void *pic, void (*set_irq)(void *pic, int irq, int level))
{
	CMOS *c = malloc(sizeof(CMOS));
	memset(c, 0, sizeof(CMOS));
	c->irq = irq;
	c->pic = pic;
	c->set_irq = set_irq;

	cmos_start_clock(c);
	c->data[0x10] = 0x44;  /* floppy: A=1.44M(4), B=1.44M(4) */
	c->data[10] = 0x26;
	c->data[11] = 0x02;
	c->data[12] = 0x00;
	c->data[13] = 0x80;
	cmos_set_mem_size(c, mem_size);
	return c;
}

/* Split out of cmos_init() so that a machine whose memory changes can have
 * these rewritten without being built again. */
void cmos_set_mem_size(CMOS *c, long mem_size)
{
	c->data[0x30] = c->data[0x31] = 0;
	c->data[0x34] = c->data[0x35] = 0;
	if (mem_size >= 1024 * 1024) {
		if (mem_size >= 64 * 1024 * 1024) {
			mem_size -= 16 * 1024 * 1024;
			c->data[0x35] = mem_size >> 24;
			c->data[0x34] = mem_size >> 16;
		} else {
			mem_size -= 1024 * 1024;
			uint32_t ext_kb = mem_size >> 10;
			c->data[0x31] = ext_kb >> 8;
			c->data[0x30] = ext_kb & 0xff;
		}
	}
	/* old AT extended memory field */
	c->data[0x16] = c->data[0x31];
	c->data[0x15] = c->data[0x30];
	// extended memory above 16MB mirror
	c->data[0x17] = c->data[0x35];
	c->data[0x18] = c->data[0x34];
}

static void u8250_update_interrupts(U8250 *uart)
{
	if (uart->ier & uart->ioready) {
		uart->set_irq(uart->pic, uart->irq, 1);
	} else {
		uart->set_irq(uart->pic, uart->irq, 0);
	}
}

uint8_t u8250_reg_read(U8250 *uart, int off)
{
	uint8_t val;
	switch (off) {
	case 0:
		if (uart->lcr & (1 << 7)) { /* DLAB */
			val = uart->dll;
			break;
		}
		val = uart->in;
		uart->ioready &= ~1;
		u8250_update_interrupts(uart);
		break;
	case 1:
		if (uart->lcr & (1 << 7)) { /* DLAB */
			val = uart->dlh;
			break;
		}
		val = uart->ier;
		break;
	case 2:
		val = (uart->ier & uart->ioready) ? 0 : 1;
		break;
	case 3:
		val = uart->lcr;
		break;
	case 4:
		val = uart->mcr;
		break;
	case 5:
		/* LSR = no error, TX done & ready */
		val = 0x60 | (uart->ioready & 1);
		break;
	case 6:
		/* MSR = carrier detect, no ring, data ready, clear to send. */
		val = 0xb0;
		break;
		/* no scratch register, so we should be detected as a plain 8250. */
	default:
		val = 0;
	}
	return val;
}

void u8250_reg_write(U8250 *uart, int off, uint8_t val)
{
	switch (off) {
	case 0:
		if (uart->lcr & (1 << 7)) {
			uart->dll = val;
			break;
		} else {
#if !defined(__wasm__) && !defined(CIRCLE_BUILD)
			ssize_t r;
			do {
				r = write(uart->out_fd, &val, 1);
			} while (r == -1 && errno == EINTR);
#elif defined(DEBUG_ENABLED)
			putchar(val);
#endif
		}
		break;
	case 1:
		if (uart->lcr & (1 << 7)) {
			uart->dlh = val;
			break;
		} else {
			uart->ier = val;
			if (uart->ier & 2)
				uart->ioready |= 2;
			else
				uart->ioready &= ~2;
			u8250_update_interrupts(uart);
		}
		break;
	case 3:
		uart->lcr = val;
		break;
	case 4:
		uart->mcr = val;
		break;
	}
}

void u8250_update(U8250 *uart)
{
#if !defined(_WIN32) && !defined(__wasm__) && !defined(CIRCLE_BUILD)
	if (IsKBHit()) {
		if (!(uart->ioready & 1)) {
			uart->in = ReadKBByte();
			uart->ioready |= 1;
			u8250_update_interrupts(uart);
		}
	}
#else
	(void)uart;  // Suppress unused parameter warning
#endif
}

#define CMOS_FREQ 32768
#define RTC_REG_A               10
#define RTC_REG_B               11
#define RTC_REG_C               12
#define RTC_REG_D               13
#define REG_A_UIP 0x80
#define REG_B_SET 0x80
#define REG_B_PIE 0x40
#define REG_B_AIE 0x20
#define REG_B_UIE 0x10

static uint32_t cmos_get_timer(CMOS *s)
{
	#ifdef CIRCLE_BUILD
	(void)s;
	return (uint32_t)(((uint64_t)time_us_64() * CMOS_FREQ) / 1000000u);
	#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * CMOS_FREQ +
		((uint64_t)ts.tv_nsec * CMOS_FREQ / 1000000000);
	#endif
}

static void cmos_update_timer(CMOS *s)
{
	int period_code;

	period_code = s->data[RTC_REG_A] & 0x0f;
	if ((s->data[RTC_REG_B] & REG_B_PIE) &&
	    period_code != 0) {
		if (period_code <= 2)
			period_code += 7;
		s->irq_period = 1 << (period_code - 1);
		s->irq_timeout = (cmos_get_timer(s) + s->irq_period) &
			~(s->irq_period - 1);
	}
}

void cmos_update_irq(CMOS *s)
{
	uint32_t d;
	if (s->data[RTC_REG_B] & REG_B_PIE) {
		d = cmos_get_timer(s) - s->irq_timeout;
		if ((int32_t)d >= 0) {
			/* this is not what the real RTC does. Here we sent the IRQ
			   immediately */
			s->data[RTC_REG_C] |= 0xc0;
			s->set_irq(s->pic, s->irq, 1);
			s->set_irq(s->pic, s->irq, 0);
			/* update for the next irq */
			s->irq_timeout += s->irq_period;
		}
	}
}

uint8_t cmos_ioport_read(CMOS *cmos, int addr)
{
	if (addr == 0x70)
		return 0xff;
	cmos_update_time(cmos);
	uint8_t val = cmos->data[cmos->index];
	/*
	 * Reading Status Register C clears it on a real MC146818, which is how
	 * the periodic-interrupt handler knows it has consumed the event.  We
	 * only ever set IRQF|PF here and never cleared it, so a handler that
	 * loops "out 70h,0Ch / in al,71h / test al,0E0h / jnz" - AT-SLOW.COM,
	 * shipped with Theme Park, does exactly that - never left the loop and
	 * the game hung before DOS/4GW was even reached.
	 */
	if (cmos->index == RTC_REG_C)
		cmos->data[RTC_REG_C] = 0;
	return val;
}

void cmos_ioport_write(CMOS *cmos, int addr, uint8_t val)
{
	if (addr == 0x70)
		cmos->index = val & 0x7f;
	else {
		CMOS *s = cmos;
		switch(s->index) {
		case RTC_REG_A:
			s->data[RTC_REG_A] = (val & ~REG_A_UIP) |
				(s->data[RTC_REG_A] & REG_A_UIP);
			cmos_update_timer(s);
			break;
		case RTC_REG_B:
			s->data[s->index] = val;
			cmos_update_timer(s);
			break;
		case 0: case 2: case 4: case 7: case 8: case 9: case 0x32:
			/* Setting the clock: keep the time the guest gave it. */
			cmos_update_time(s);
			s->data[s->index] = val;
			s->rtc_base_s = rtc_from_regs(s) - rtc_elapsed_s();
			s->rtc_set_us = time_us_64() | 1;
			break;
		default:
			s->data[s->index] = val;
			break;
		}
	}
}

uint8_t cmos_set(void *cmos, int addr, uint8_t val)
{
	CMOS *s = cmos;
	if (addr < 128) {
		s->data[addr] = val;
	}
	return val;
}

/* Пересчитать CMOS checksum (0x10..0x2D) и записать в 0x2E/0x2F.
 * Вызывать после любых изменений CMOS, до старта BIOS. */
void cmos_update_checksum(void *cmos)
{
	CMOS *s = cmos;
	uint16_t sum = 0;
	for (int i = 0x10; i <= 0x2D; i++)
		sum += s->data[i];
	s->data[0x2E] = (sum >> 8) & 0xFF;
	s->data[0x2F] = sum & 0xFF;
}

/* EMULINK removed - disk operations use INT 13h disk handler instead */
