// SPDX-License-Identifier: GPL-3.0-or-later
/* Host-only regression for borrowed-RAM ownership and reset-vector policy.
 * Build this with AddressSanitizer together with cpu_smoke.c, src/i386.c and
 * src/fpu.c. Each caller-owned allocation is freed exactly once here. */
#include "bios_probe.h"
extern "C" {
#include "../../../src/i386.h"
}
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" int tiny386_real_reset_vector_probe(char *, uint32_t, uint32_t,
                                                Tiny386ResetProbe *);

/* The interpreter references this Pico timing hook in an instruction path not
 * used by this regression. Supply a deterministic host definition so ASan can
 * link the unmodified reset-vector probe. */
extern "C" uint64_t time_us_64(void) { return 0; }

/* The host build compiles i386.c with -Dmalloc=tiny386_poison_malloc.  Every
 * CPU allocation therefore starts as non-zero memory, making the regression
 * deterministic: the callback fields must be explicitly initialised by
 * cpui386_new(), not merely happen to be clear in the system allocator. */
extern "C" void *tiny386_poison_malloc(size_t size)
{
    void *p = std::malloc(size);
    if (p) ::memset(p, 0xa5, size);
    return p;
}

static void delete_host_cpu(CPUI386 *cpu)
{
    if (!cpu) return;
    std::free(cpu->tlb.tab);
    cpui386_delete(cpu);
}

static bool int2f_false(CPUI386 *, void *opaque)
{
    int *calls = static_cast<int *>(opaque);
    ++*calls;
    return false; /* continue through the guest IVT */
}

static bool int2f_true(CPUI386 *, void *opaque)
{
    int *calls = static_cast<int *>(opaque);
    ++*calls;
    return true; /* host handled: do not enter the guest IVT */
}

static int run_int2f_case()
{
    const uint32_t ram_size = 2u << 20;
    char *ram = static_cast<char *>(std::calloc(1, ram_size));
    if (!ram) return 1;

    /* Reset vector -> 1000:0100.  The guest sets a real-mode stack, invokes
     * INT 2Fh, then halts.  IVT[2f] -> 2000:0100, MOV AX,BEEF / IRET. */
    const uint8_t reset_vector[] = {0xea, 0x00, 0x01, 0x00, 0x10};
    const uint8_t guest[] = {0xbc, 0x00, 0x80, 0xcd, 0x2f, 0xf4};
    const uint8_t ivt_handler[] = {0xb8, 0xef, 0xbe, 0xcf};
    ::memcpy(ram + 0xffff0, reset_vector, sizeof reset_vector);
    ::memcpy(ram + 0x10100, guest, sizeof guest);
    ::memcpy(ram + 0x20100, ivt_handler, sizeof ivt_handler);
    ram[0x2f * 4 + 0] = 0x00;
    ram[0x2f * 4 + 1] = 0x01;
    ram[0x2f * 4 + 2] = 0x00;
    ram[0x2f * 4 + 3] = 0x20;

    CPU_CB *cb = nullptr;
    CPUI386 *cpu = cpui386_new(3, ram, ram_size, &cb);
    if (!cpu || !cb || cpu->int2f_handler || cpu->int2f_opaque) {
        delete_host_cpu(cpu);
        std::free(ram);
        return 2; /* catches poisoned uninitialised callback fields */
    }

    int calls = 0;
    cpu_set_int2f_handler(cpu, int2f_false, &calls);
    cpui386_reset(cpu);
    if (cpu->int2f_handler != int2f_false || cpu->int2f_opaque != &calls) {
        delete_host_cpu(cpu); std::free(ram); return 3;
    }
    for (unsigned i = 0; i < 32 && !cpu->halt; ++i)
        cpui386_step(cpu, 1);
    const bool guest_path = cpu->halt && calls == 1 && cpu_getax(cpu) == 0xbeef;

    /* A true host result must survive reset and suppress the guest IVT. */
    calls = 0;
    cpu_set_int2f_handler(cpu, int2f_true, &calls);
    cpui386_reset(cpu);
    if (cpu->int2f_handler != int2f_true || cpu->int2f_opaque != &calls) {
        delete_host_cpu(cpu); std::free(ram); return 4;
    }
    for (unsigned i = 0; i < 32 && !cpu->halt; ++i)
        cpui386_step(cpu, 1);
    const bool host_path = cpu->halt && calls == 1 && cpu_getax(cpu) != 0xbeef;

    delete_host_cpu(cpu);
    std::free(ram);
    return guest_path && host_path ? 0 : 5;
}

static int run_case(uint8_t opcode, uint16_t ip, uint16_t cs,
                    int expected_status, int expected_step)
{
    const uint32_t size = 0x10000u;
    const uint32_t base = 0xf0000u;
    char *ram = (char *)calloc(1, TINY386_BIOS_RAM_SIZE);
    if (!ram) return 1;
    ram[0xffff0] = (char)opcode;
    ram[0xffff1] = (char)ip;
    ram[0xffff2] = (char)(ip >> 8);
    ram[0xffff3] = (char)cs;
    ram[0xffff4] = (char)(cs >> 8);
    Tiny386ResetProbe result = {};
    int status = tiny386_real_reset_vector_probe(ram, base, size, &result);
    int failed = status != expected_status || result.step_done != expected_step;
    free(ram);                 /* the probe borrows RAM and never frees it */
    return failed;
}

int main(void)
{
    int failures = 0;
    failures += run_case(0x90, 0, 0, 0, 0);       /* non-EA: skipped */
    failures += run_case(0xea, 0, 0x2000, -5, 0); /* target outside BIOS */
    failures += run_case(0xea, 0, 0xf000, 0, 1);  /* one FAR-JMP step */
    failures += run_int2f_case();
    printf("reset-vector ownership regression %s\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
