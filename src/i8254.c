/*
 * QEMU 8253/8254 interval timer emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>
#include "i8254.h"
#include "audiodiag.h"
#include <pico.h>
#if defined(CIRCLE_PC_DIAG)
#include "circle_pc_diag.h"
#else
#define CIRCLE_PC_IRQ0_RAISED() ((void)0)
#endif
//#define DEBUG_PIT

#define RW_STATE_LSB 1
#define RW_STATE_MSB 2
#define RW_STATE_WORD0 3
#define RW_STATE_WORD1 4

typedef struct PITChannelState {
	uint32_t count; /* can be 65536 */
	uint16_t latched_count;
	uint8_t count_latched;
	uint8_t status_latched;
	uint8_t status;
	uint8_t read_state;
	uint8_t write_state;
	uint8_t write_latch;
	uint8_t rw_mode;
	uint8_t mode;
	uint8_t bcd; /* not supported */
	uint8_t gate; /* timer start */

	uint32_t count_load_time;
	uint32_t last_irq_count;
	int irq;
} PITChannelState;

/* Edges actually handed to the PIC, so a guest whose music runs slow can be
 * checked against the rate it programmed instead of against the rate the
 * catch-up counter believes it delivered. */
uint32_t g_pit_irq0_edges;

struct PITState {
	PITChannelState channels[3];
	void *pic;
	void (*set_irq)(void *pic, int irq, int level);
};

uint32_t get_uticks();
static int pit_get_count(PITChannelState *s)
{
	uint32_t d;
	int counter;

	d = ((uint64_t) (get_uticks() - s->count_load_time)) * PIT_FREQ / 1000000;
	switch(s->mode) {
	case 0:
	case 1:
	case 4:
	case 5:
		counter = (s->count - d) & 0xffff;
		break;
	case 3:
		/* XXX: may be incorrect for odd counts */
		counter = s->count - ((2 * d) % s->count);
		break;
	default:
		counter = s->count - (d % s->count);
		break;
	}
	return counter;
}

/* get pit output bit */
static int pit_get_out1(PITChannelState *s, uint32_t current_time)
{
	uint32_t d;
	int out;

	d = ((uint64_t) (current_time - s->count_load_time)) * PIT_FREQ / 1000000;
	switch(s->mode) {
	default:
	case 0:
		out = (d >= s->count);
		break;
	case 1:
		out = (d < s->count);
		break;
	case 2:
		if ((d % s->count) == 0 && d != 0)
			out = 1;
		else
			out = 0;
		break;
	case 3:
		out = (d % s->count) < ((s->count + 1) >> 1);
		break;
	case 4:
	case 5:
		out = (d == s->count);
		break;
	}
	return out;
}

static inline void pit_load_count(PITState *pit, PITChannelState *s, int val)
{
	if (val == 0)
		val = 0x10000;
	s->count_load_time = get_uticks();
	s->last_irq_count = 0;
	s->count = val;
}

/* if already latched, do not latch again */
static void pit_latch_count(PITChannelState *s)
{
	if (!s->count_latched) {
		s->latched_count = pit_get_count(s);
		s->count_latched = s->rw_mode;
	}
}

void i8254_ioport_write(PITState *pit, uint32_t addr, uint32_t val)
{
	int channel, access;
	PITChannelState *s;

	addr &= 3;
	if (addr == 3) {
		channel = val >> 6;
		if (channel == 3) {
			/* read back command */
			for(channel = 0; channel < 3; channel++) {
				s = &pit->channels[channel];
				if (val & (2 << channel)) {
					if (!(val & 0x20)) {
						pit_latch_count(s);
					}
					if (!(val & 0x10) && !s->status_latched) {
						/* status latch */
						/* XXX: add BCD and null count */
						s->status = (pit_get_out1(s, get_uticks()) << 7) |
							(s->rw_mode << 4) |
							(s->mode << 1) |
							s->bcd;
						s->status_latched = 1;
					}
				}
			}
		} else {
			s = &pit->channels[channel];
			access = (val >> 4) & 3;
			if (access == 0) {
				pit_latch_count(s);
			} else {
				s->rw_mode = access;
				s->read_state = access;
				s->write_state = access;

				/*
				 * The 8254 decodes the three mode bits as 000..101 and
				 * aliases 110 and 111 onto modes 2 and 3.  Storing the raw
				 * field meant a perfectly legal square-wave control word
				 * written as 111 - which is what Disney's Aladdin writes -
				 * arrived at i8254_update_irq() as "mode 7", missed its
				 * switch and hit abort().  That panics the firmware into
				 * _exit(), which is a black screen with the guest frozen
				 * mid-instruction, no reboot and no fault recorded.
				 */
				s->mode = (val >> 1) & 7;
				if (s->mode >= 6)
					s->mode -= 4;
				s->bcd = val & 1;
				/* XXX: update irq timer ? */
			}
		}
	} else {
		s = &pit->channels[addr];
		switch(s->write_state) {
		default:
		case RW_STATE_LSB:
			pit_load_count(pit, s, val);
			break;
		case RW_STATE_MSB:
			pit_load_count(pit, s, val << 8);
			break;
		case RW_STATE_WORD0:
			s->write_latch = val;
			s->write_state = RW_STATE_WORD1;
			break;
		case RW_STATE_WORD1:
			pit_load_count(pit, s, s->write_latch | (val << 8));
			s->write_state = RW_STATE_WORD0;
			break;
		}
	}
}

uint32_t i8254_ioport_read(PITState *pit, uint32_t addr)
{
	int ret, count;
	PITChannelState *s;

	addr &= 3;
	s = &pit->channels[addr];
	if (s->status_latched) {
		s->status_latched = 0;
		ret = s->status;
	} else if (s->count_latched) {
		switch(s->count_latched) {
		default:
		case RW_STATE_LSB:
			ret = s->latched_count & 0xff;
			s->count_latched = 0;
			break;
		case RW_STATE_MSB:
			ret = s->latched_count >> 8;
			s->count_latched = 0;
			break;
		case RW_STATE_WORD0:
			ret = s->latched_count & 0xff;
			s->count_latched = RW_STATE_MSB;
			break;
		}
	} else {
		switch(s->read_state) {
		default:
		case RW_STATE_LSB:
			count = pit_get_count(s);
			ret = count & 0xff;
			break;
		case RW_STATE_MSB:
			count = pit_get_count(s);
			ret = (count >> 8) & 0xff;
			break;
		case RW_STATE_WORD0:
			count = pit_get_count(s);
			ret = count & 0xff;
			s->read_state = RW_STATE_WORD1;
			break;
		case RW_STATE_WORD1:
			count = pit_get_count(s);
			ret = (count >> 8) & 0xff;
			s->read_state = RW_STATE_WORD0;
			break;
		}
	}
	return ret;
}

static void pit_reset(void *opaque)
{
	PITState *pit = opaque;
	PITChannelState *s;
	int i;

	for(i = 0;i < 3; i++) {
		s = &pit->channels[i];
		s->mode = 3;
		s->gate = (i != 2);
		pit_load_count(pit, s, 0);
		s->irq = -1;
	}
}

/* How often anything asks the timer to catch up, and how many edges one
 * such call emitted back to back.  A burst is the only way two edges can
 * reach the PIC with no guest instruction between them. */
uint32_t g_pit_update_calls;
uint32_t g_pit_burst_edges;
/* The gap between consecutive catch-up checks.  Two edges can only leave
 * one call if the timer advanced past two whole reload periods since the
 * previous one, so the largest gap says whether such a pause exists at
 * all, and the sum says how much of the second is spent inside them. */
uint32_t g_pit_gap_max_us;
uint32_t g_pit_gap_over_us;
static uint32_t pit_last_poll_us;
/* How far behind a periodic channel ever fell, in PIT ticks, and how many
 * channel 0 periods were dropped rather than delivered late; see below. */
#define PIT_MAX_LAG_MS 50
uint32_t g_pit_lag_max;
uint32_t g_pit_irq0_dropped;

void i8254_update_irq(PITState *pit)
{
	g_pit_update_calls++;
	uint32_t uticks = get_uticks();
	if (pit_last_poll_us) {
		const uint32_t gap = uticks - pit_last_poll_us;
		if (gap > g_pit_gap_max_us) g_pit_gap_max_us = gap;
		if (gap > 894u) g_pit_gap_over_us += gap;
	}
	pit_last_poll_us = uticks;
	PITChannelState *s = pit->channels;
	uint32_t d = ((uint64_t) (uticks - s->count_load_time)) * PIT_FREQ / 1000000;

	FRANK_DIAG_COUNT(pit_polls);

	switch(s->mode) {
	case 2:
	case 3:
		/* if d >= last_irq_count + count */
		if (s->last_irq_count + s->count - d >= 0x80000000) {
			int i = 0;
			/*
			 * A backlog older than PIT_MAX_LAG_US is dropped, keeping one.
			 *
			 * The catch-up below is for a poll that came a little late,
			 * not for a board that stopped: when the host is held up - an
			 * SD card sitting on a write for a few hundred milliseconds -
			 * the guest did not run either, and a real PC never shows a
			 * program the ticks of time it did not live through.  Handing
			 * them over anyway puts them back to back, one per EOI, and
			 * Windows 95 setup's DOS extender cannot take that: it
			 * reflects IRQ0 to protected mode and back with interrupts
			 * open for two instructions on the way out, each queued tick
			 * lands in that window and nests another 0x300-byte frame,
			 * and a few hundred milliseconds of ticks overran its frame
			 * pool.  The unwind then returned through a clobbered frame
			 * into data at 2f2c:005b and the machine ran text.
			 *
			 * The window still covers any ordinary late poll at the
			 * fastest rates programs use, so the lost-tick problem the
			 * one-edge rule fixed does not come back.
			 */
			const uint32_t lag = d - (s->last_irq_count + s->count);
			if (lag > g_pit_lag_max) g_pit_lag_max = lag;
			if (lag > PIT_FREQ / 1000 * PIT_MAX_LAG_MS) {
				const uint32_t periods = lag / s->count;
				s->last_irq_count += periods * s->count;
				if (s->irq == 0) g_pit_irq0_dropped += periods;
			}
			/*
			 * One edge per call, not a burst.
			 *
			 * A backlog used to be flushed ten at a time, and the ten went
			 * to the PIC with no guest instruction between them.  Its
			 * request register is a bitmask, not a queue, so nine of the
			 * ten were simply gone - the guest saw one tick where ten had
			 * elapsed, and anything paced by the timer ran slow in
			 * proportion.  Measured here at 341 lost ticks a second out of
			 * 1119, all of them second-or-later edges of such a burst.
			 *
			 * Emitting one and leaving the rest for the following calls
			 * costs nothing and loses nothing: last_irq_count advances by
			 * exactly one period each time, so the catch-up rate is still
			 * bounded by real elapsed time and can never invent a tick.
			 * The guest runs between calls, acknowledges, and takes the
			 * next one.  At the polling rate this emulator already uses
			 * that drains a backlog far faster than it can build.
			 *
			 */
			const int catch_up_limit = 1;
			while (s->last_irq_count + s->count - d >= 0x80000000 && i < catch_up_limit) {
				if (s->irq != -1) {
					if (s->irq == 0) { g_pit_irq0_edges++; if (i > 0) g_pit_burst_edges++; }
					pit->set_irq(pit->pic, s->irq, 1);
					pit->set_irq(pit->pic, s->irq, 0);
					CIRCLE_PC_IRQ0_RAISED();
					frank_diag_irq0();
				}
				s->last_irq_count += s->count;
				// avoid wraparound
				if (uticks - s->count_load_time > (1u << 31)) {
					pit_load_count(pit, s, s->count);
				}
				i++;
			}
		}
		break;

	case 0:
	case 4:
		/*
		 * One-shot modes: OUT rises once, `count` periods after the
		 * counter was loaded, and stays there until it is loaded again.
		 * pit_load_count() clears last_irq_count, so it doubles as the
		 * "has not fired yet" flag.
		 */
		if (!s->last_irq_count && (int32_t)(d - s->count) >= 0) {
			if (s->irq != -1) {
				if (s->irq == 0) g_pit_irq0_edges++;
				pit->set_irq(pit->pic, s->irq, 1);
				pit->set_irq(pit->pic, s->irq, 0);
				CIRCLE_PC_IRQ0_RAISED();
				frank_diag_irq0();
			}
			s->last_irq_count = s->count;
		}
		break;

	case 1:
	case 5:
		/*
		 * Retriggerable one-shots, started by the gate rather than by
		 * the counter load.  Channel 0's gate is tied high on a PC, so
		 * these never produce an edge here.
		 */
		break;

	default:
		/*
		 * Unreachable: the mode field is three bits and 110/111 are
		 * folded onto 2/3 where it is latched.  It used to be abort(),
		 * which turned any guest that programmed a legal but
		 * unimplemented mode into a dead board rather than into silence
		 * on one interrupt line.  Never do that from a device model.
		 */
		break;
	}
}

PITState *i8254_init(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level))
{
	PITState *pit = malloc(sizeof(PITState));
	memset(pit, 0, sizeof(PITState));
	pit->pic = pic;
	pit->set_irq = set_irq;

	pit_reset(pit);
	pit->channels[0].irq = irq;
	return pit;
}

int pit_get_out(PITState *pit, int channel)
{
	PITChannelState *s = &pit->channels[channel];
	uint32_t uticks = get_uticks();
	return pit_get_out1(s, uticks);
}

int pit_get_gate(PITState *pit, int channel)
{
	PITChannelState *s = &pit->channels[channel];
	return s->gate;
}

/* val must be 0 or 1 */
void pit_set_gate(PITState *pit, int channel, int val)
{
	PITChannelState *s = &pit->channels[channel];

	switch(s->mode) {
	default:
	case 0:
	case 4:
		/* XXX: just disable/enable counting */
		break;
	case 1:
	case 5:
		if (s->gate < val) {
			/* restart counting on rising edge */
			s->count_load_time = get_uticks();
		}
		break;
	case 2:
	case 3:
		if (s->gate < val) {
			/* restart counting on rising edge */
			s->count_load_time = get_uticks();
		}
		/* XXX: disable/enable counting */
		break;
	}
	s->gate = val;
}

int pit_get_initial_count(PITState *pit, int channel)
{
	PITChannelState *s = &pit->channels[channel];
	return s->count;
}

int pit_get_mode(PITState *pit, int channel)
{
	PITChannelState *s = &pit->channels[channel];
	return s->mode;
}
