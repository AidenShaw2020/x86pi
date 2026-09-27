#pragma once
/*
 * The Cortex-A53's own performance counters.
 *
 * "The interpreter takes 170 cycles a guest instruction" is a number without
 * an explanation: it could be too many host instructions, or the right number
 * of them stalled on memory, and those call for opposite work.  The part can
 * answer that directly - it counts retired instructions, cache refills and
 * mispredicted branches - and reading them costs two system registers per
 * ten-second window.
 *
 * Kept out of the ordinary build: enabling the PMU is a write to a system
 * register, which is cheap, but nothing here should depend on a debug
 * facility being on.
 */

#include <circle/types.h>

namespace Pmu {

/* Cortex-A53 event numbers, from its technical reference manual. */
static const u32 EventInstRetired   = 0x08;
static const u32 EventL1DRefill     = 0x03;
static const u32 EventL2DRefill     = 0x17;
static const u32 EventBranchMispred = 0x10;
static const u32 EventL1IRefill     = 0x01;
static const u32 EventL1IAccess     = 0x14;

/*
 * The A53's own stall counters, which the architecture does not define and
 * this part does.
 *
 * The events above say how much work there was and how often a cache or a
 * predictor missed; between them they accounted for twelve cycles of the
 * hundred and forty-seven a guest instruction costs here.  The rest is the
 * pipeline standing still, and these say which queue it is standing in -
 * which is the difference between code layout being worth doing and being
 * worth nothing.  Build with STALLS=1 to count these instead.
 */
static const u32 EventIQStall       = 0xe0;     /* instruction queue, other */
static const u32 EventICStall       = 0xe1;     /* waiting on instruction cache */
static const u32 EventDecodeStall   = 0xe3;
static const u32 EventInterlockStall= 0xe4;     /* register interlock, other */
static const u32 EventAguStall      = 0xe5;     /* address generation */
static const u32 EventLoadStall     = 0xe7;

static inline void SelectEvent(u32 nCounter, u32 nEvent)
{
    asm volatile ("msr pmselr_el0, %0" :: "r"((u64)nCounter));
    asm volatile ("isb");
    asm volatile ("msr pmxevtyper_el0, %0" :: "r"((u64)nEvent));
}

static inline u64 ReadEvent(u32 nCounter)
{
    u64 v;
    asm volatile ("msr pmselr_el0, %0" :: "r"((u64)nCounter));
    asm volatile ("isb");
    asm volatile ("mrs %0, pmxevcntr_el0" : "=r"(v));
    return v & 0xffffffffull;
}

static inline u64 ReadCycles(void)
{
    u64 v;
    asm volatile ("mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}

static inline void Start(void)
{
    /* PMCR_EL0: E enables the counters, P and C reset the event counters and
     * the cycle counter.  The cycle counter is left dividing by one, because
     * a ten-second window fits in 64 bits many times over. */
    asm volatile ("msr pmcr_el0, %0" :: "r"((u64)((1u << 0) | (1u << 1) | (1u << 2))));
#if defined(CIRCLE_PC_PMU_STALLS)
    SelectEvent(0, EventICStall);
    SelectEvent(1, EventLoadStall);
    SelectEvent(2, EventAguStall);
    SelectEvent(3, EventInterlockStall);
    SelectEvent(4, EventDecodeStall);
    SelectEvent(5, EventIQStall);
#else
    SelectEvent(0, EventInstRetired);
    SelectEvent(1, EventL1DRefill);
    SelectEvent(2, EventL2DRefill);
    SelectEvent(3, EventBranchMispred);
    SelectEvent(4, EventL1IRefill);
    SelectEvent(5, EventL1IAccess);
#endif
    /* The six event counters and, at bit 31, the cycle counter.  The A53
     * implements six; any it does not simply read back zero. */
    asm volatile ("msr pmcntenset_el0, %0" :: "r"((u64)(0x3fu | (1u << 31))));
    asm volatile ("isb");
}

}
