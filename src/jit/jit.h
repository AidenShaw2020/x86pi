#pragma once
/*
 * Translating guest code to AArch64.
 *
 * Off unless the build defines TINY386_JIT.  With it off every entry point
 * below compiles to nothing, so the interpreter is byte for byte what it was
 * - which is the condition this work was started under.
 *
 * The translator never has to handle an instruction.  A block ends at the
 * first opcode it does not know, with the guest's state exact and next_ip
 * pointing at that instruction, and the interpreter carries on.  So coverage
 * can grow one opcode at a time and a gap is slow rather than wrong.
 */

#include "../i386.h"

#ifdef TINY386_JIT

/* Set up the code arena.  Returns false if it could not be had, in which
 * case everything below stays inert and the interpreter runs alone. */
bool jit_init(CPUI386 *cpu);

/*
 * Run translated code at the guest's current address, if there is any.
 *
 * Returns the number of guest instructions executed, or zero if nothing was
 * run and the interpreter should proceed.  The caller does not need to know
 * whether a block existed, was compiled now, or was refused.
 */
int jit_run(CPUI386 *cpu);

/* Throw away every translation.  Called when guest memory that holds
 * translated code is written, and when the machine is reset. */
void jit_flush(void);

/* A store landed at this physical address; drop anything translated from
 * that page.  Cheap enough for the store path: one shift and a test. */
void jit_note_write(uint32_t phys_addr);

/*
 * Check the translator against itself at startup.
 *
 * Builds blocks whose effect is known, runs them, and compares the guest
 * state with what it should be.  A wrong encoding or a wrong register
 * mapping shows up here, in milliseconds, instead of in a game minutes
 * later.  Writes a one-line verdict into `msg`; returns the number of
 * failures.
 */
int jit_selftest(char *msg, int len);

/*
 * Ask what the translator would do with the instruction the guest is about
 * to run, without translating anything.
 *
 * Coverage decides whether a translator is worth having: the previous
 * attempt on the other board reached five or six per cent of instructions
 * and measured no gain at all.  This counts how long the runs of
 * translatable instructions are and which opcode ends each one, weighted by
 * how often the guest actually gets there - so the next opcode to implement
 * is chosen from what the guest runs rather than from what seems likely.
 */
#ifdef JIT_PROFILE
void jit_profile(CPUI386 *cpu);
int jit_profile_report(char *out, int len);
#else
/* It runs the whole translator on every instruction, which costs more than
 * the interpreter does; it answers what to write next, not how fast this is.
 * Build with JITPROF=1 when asking the first question. */
#define jit_profile(cpu)         ((void)0)
#define jit_profile_report(o, l) (0)
#endif

/* Counters, for the periodic report. */
extern uint32_t g_jit_blocks, g_jit_compiles, g_jit_flushes;
extern uint64_t g_jit_insns;

#else

#define jit_init(cpu)            (false)
#define jit_run(cpu)             (0)
#define jit_flush()              ((void)0)
#define jit_note_write(addr)     ((void)0)
#define jit_selftest(msg, len)   (0)
#define jit_profile(cpu)         ((void)0)
#define jit_profile_report(o, l) (0)

#endif
