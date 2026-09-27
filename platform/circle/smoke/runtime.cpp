// SPDX-License-Identifier: GPL-3.0-or-later
#include <circle/logger.h>
#include <circle/startup.h>
#include <circle/timer.h>
#include <stdarg.h>
extern "C" {
#include "compat/stdio.h"
#include "../../../src/fpu.h"
FILE *stderr = nullptr;
void abort(void) { CLogger::Get()->Write("CPU", LogPanic, "Smoke runtime abort"); halt(); }
int fprintf(FILE *, const char *format, ...) {
    va_list args; va_start(args, format);
    CLogger::Get()->WriteV("CPU", LogError, format, args);
    va_end(args); return 0;
}
FILE *freopen(const char *, const char *, FILE *) { abort(); return nullptr; }
void setlinebuf(FILE *) { abort(); }
int usleep(unsigned int us) { CTimer::SimpleusDelay(us); return 0; }
uint64_t time_us_64(void) { return CTimer::GetClockTicks64(); }
// No x87 emulation in this bounded smoke target. Fail loudly if called.
// Production integration must link the real fpu.c with a bare-metal math libc.
FPU *fpu_new() { abort(); return nullptr; }
void fpu_delete(FPU *) { abort(); }
bool fpu_exec1(FPU *, void *, int, int, unsigned int) { abort(); return false; }
bool fpu_exec2(FPU *, void *, bool, int, int, int, uint32_t) { abort(); return false; }
}
