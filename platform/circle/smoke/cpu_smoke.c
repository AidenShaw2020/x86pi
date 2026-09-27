// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../src/i386.h"
#include "bios_probe.h"
#include <stdlib.h>
#include <string.h>
static int no_irq(void *p) { (void)p; return -1; }
static u8 r8(void *p, int a) { (void)p; (void)a; abort(); return 0; }
static u16 r16(void *p, int a) { (void)p; (void)a; abort(); return 0; }
static u32 r32(void *p, int a) { (void)p; (void)a; abort(); return 0; }
static void w8(void *p, int a, u8 v) { (void)p; (void)a; (void)v; abort(); }
static void w16(void *p, int a, u16 v) { (void)p; (void)a; (void)v; abort(); }
static void w32(void *p, int a, u32 v) { (void)p; (void)a; (void)v; abort(); }
static int rs(void *p,int a,uint8_t *b,int n,int w) {
    (void)p;(void)a;(void)b;(void)n;(void)w;abort();return 0;
}
static u8 mr8(void *p,uword a) { return r8(p,(int)a); }
static u16 mr16(void *p,uword a) { return r16(p,(int)a); }
static u32 mr32(void *p,uword a) { return r32(p,(int)a); }
static void mw8(void *p,uword a,u8 v) { w8(p,(int)a,v); }
static void mw16(void *p,uword a,u16 v) { w16(p,(int)a,v); }
static void mw32(void *p,uword a,u32 v) { w32(p,(int)a,v); }
static bool mws(void *p,uword a,uint8_t *b,int n) {
    (void)p;(void)a;(void)b;(void)n;abort();return false;
}

/* Keep the smoke callbacks deliberately hostile: this milestone must not
 * accidentally turn an I/O or MMIO access into a fake success. */
static void install_callbacks(CPU_CB *cb)
{
    cb->pic_read_irq=no_irq;
    cb->io_read8=r8; cb->io_read16=r16; cb->io_read32=r32;
    cb->io_write8=w8; cb->io_write16=w16; cb->io_write32=w32;
    cb->io_read_string=rs; cb->io_write_string=rs;
    cb->iomem_read8=mr8; cb->iomem_read16=mr16; cb->iomem_read32=mr32;
    cb->iomem_write8=mw8; cb->iomem_write16=mw16; cb->iomem_write32=mw32;
    cb->iomem_write_string=mws;
}

static CPUI386 *new_smoke_cpu(char *ram, CPU_CB **cb)
{
    CPUI386 *cpu = cpui386_new(3, ram, 2u<<20, cb);
    if (cpu && cb) install_callbacks(*cb);
    return cpu;
}

static void delete_smoke_cpu(CPUI386 *cpu)
{
    if (!cpu) return;
    /* The current core destructor does not release its separate TLB
     * allocation.  The smoke owns that allocation explicitly. */
    free(cpu->tlb.tab);
    cpui386_delete(cpu);
}

int tiny386_pm_smoke(void)
{
    // xor eax,eax; mov ecx,1000; add eax,3; loop -5;
    // mov [0x20000],eax; hlt. Exactly 3000 must be stored.
    static const u8 program[] = {
        0x31,0xc0, 0xb9,0xe8,0x03,0,0,
        0x83,0xc0,0x03, 0xe2,0xfb,
        0xa3,0,0,0x02,0, 0xf4
    };
    char *ram = calloc(1, 2u<<20);
    if (!ram) return -1;
    CPU_CB *cb = NULL;
    CPUI386 *cpu = new_smoke_cpu(ram, &cb);
    if (!cpu || !cb) { delete_smoke_cpu(cpu); free(ram); return -2; }
    memcpy(ram+0x10000, program, sizeof program);
    cpui386_reset_pm(cpu,0x10000);
    cpui386_set_gpr(cpu,4,0x90000);
    for (unsigned i=0;i<100 && !cpu->halt;i++) cpui386_step(cpu,100);
    u32 result=0;
    memcpy(&result,ram+0x20000,sizeof result);
    int ok=cpu->halt && result==3000 && cpu->gprx[1].r32==0;
    delete_smoke_cpu(cpu);
    free(ram);
    return ok ? 0 : -3;
}

/*
 * Reset-vector smoke.  This intentionally starts through the architectural
 * 386 reset state instead of cpui386_reset_pm(): the first fetch must come
 * from F000:FFF0 and the far jump must establish F000:0100.
 *
 * The test program is deliberately small and has no BIOS or device
 * dependency:
 *
 *   mov ax,2000h / mov ds,ax / mov ss,ax / mov sp,7ff0h
 *   mov ax,1234h / mov [0100h],ax / mov bx,[0100h]
 *   push bx / xor bx,bx / pop bx / mov [0102h],bx
 *   mov cx,3 / xor ax,ax / inc ax / loop -3 / mov [0104h],ax / hlt
 *
 * Results live at physical 20100..20105, outside the reset/code area.  The
 * result area is invalidated with a sentinel before every reset so the second
 * run cannot pass by reusing the first run's RAM writes.
 */
enum { REAL_CS = 1, REAL_SS = 2, REAL_DS = 3 };
enum { REAL_RESULT = 0x20100, REAL_STACK = 0x27ff0 };

static const u8 real_program[] = {
    0xb8, 0x00, 0x20,             /* mov ax,2000h */
    0x8e, 0xd8,                   /* mov ds,ax */
    0x8e, 0xd0,                   /* mov ss,ax */
    0xbc, 0xf0, 0x7f,             /* mov sp,7ff0h */
    0xb8, 0x34, 0x12,             /* mov ax,1234h */
    0xa3, 0x00, 0x01,             /* mov [0100h],ax */
    0x8b, 0x1e, 0x00, 0x01,       /* mov bx,[0100h] */
    0x53,                         /* push bx */
    0x31, 0xdb,                   /* xor bx,bx */
    0x5b,                         /* pop bx */
    0x89, 0x1e, 0x02, 0x01,       /* mov [0102h],bx */
    0xb9, 0x03, 0x00,             /* mov cx,3 */
    0x31, 0xc0,                   /* xor ax,ax */
    0x40,                         /* inc ax */
    0xe2, 0xfd,                   /* loop -3 (back to inc ax) */
    0xa3, 0x04, 0x01,             /* mov [0104h],ax */
    0xf4                          /* hlt */
};

static int real_reset_state_ok(const CPUI386 *cpu)
{
    /* PE (bit 0) and PG (bit 31) must both be clear after a real reset. */
    return cpu->seg[REAL_CS].sel == 0xf000 &&
           cpu->seg[REAL_CS].base == 0xf0000 &&
           cpu->ip == 0xfff0 && cpu->next_ip == 0xfff0 &&
           cpu->code16 && (cpu->cr0 & 0x80000001u) == 0;
}

static int real_run_once(CPUI386 *cpu, char *ram)
{
    uint16_t word0, word1, word2;
    uint16_t stack_word;

    /* Do not let stale output from a preceding reset satisfy this run. */
    memset(ram + REAL_RESULT, 0xa5, 6);
    memset(ram + REAL_STACK - 2, 0xa5, 2);
    cpui386_reset(cpu);
    if (!real_reset_state_ok(cpu)) return -1;

    for (unsigned i = 0; i < 64 && !cpu->halt; ++i)
        cpui386_step(cpu, 64);
    memcpy(&word0, ram + REAL_RESULT + 0, sizeof word0);
    memcpy(&word1, ram + REAL_RESULT + 2, sizeof word1);
    memcpy(&word2, ram + REAL_RESULT + 4, sizeof word2);
    memcpy(&stack_word, ram + REAL_STACK - 2, sizeof stack_word);

    return cpu->halt &&
           word0 == 0x1234 && word1 == 0x1234 && word2 == 3 &&
           stack_word == 0x1234 && cpu->gprx[4].r16 == 0x7ff0 &&
           cpu->seg[REAL_CS].sel == 0xf000 &&
           cpu->seg[REAL_CS].base == 0xf0000 &&
           cpu->seg[REAL_DS].sel == 0x2000 &&
           cpu->seg[REAL_DS].base == 0x20000 &&
           cpu->seg[REAL_SS].sel == 0x2000 &&
           cpu->seg[REAL_SS].base == 0x20000 ? 0 : -2;
}

int tiny386_real_smoke(void)
{
    char *ram = calloc(1, 2u<<20);
    if (!ram) return -1;

    /* Reset vector: JMP FAR F000:0100, then a 16-bit program at F0100. */
    static const u8 reset_vector[] = { 0xea, 0x00, 0x01, 0x00, 0xf0 };
    memcpy(ram + 0xffff0, reset_vector, sizeof reset_vector);
    memcpy(ram + 0xf0100, real_program, sizeof real_program);

    CPU_CB *cb = NULL;
    CPUI386 *cpu = new_smoke_cpu(ram, &cb);
    if (!cpu || !cb) { delete_smoke_cpu(cpu); free(ram); return -2; }

    /* Same CPU and same reset vector twice; real_run_once invalidates output
     * before each cpui386_reset and therefore catches stale RAM acceptance. */
    int first = real_run_once(cpu, ram);
    int second = real_run_once(cpu, ram);
    delete_smoke_cpu(cpu);
    free(ram);
    return first == 0 && second == 0 ? 0 : (first ? first : second);
}

int tiny386_real_reset_vector_probe(char *ram, uint32_t bios_base,
                                    uint32_t bios_size, Tiny386ResetProbe *out)
{
    if (!ram || !out || bios_base > 0x100000u ||
        bios_size < TINY386_BIOS_MIN_SIZE ||
        bios_size > TINY386_BIOS_MAX_SIZE ||
        bios_base + bios_size > 0x100000u) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    CPU_CB *cb = NULL;
    CPUI386 *cpu = new_smoke_cpu(ram, &cb);
    if (!cpu || !cb) { delete_smoke_cpu(cpu); return -2; }

    cpui386_reset(cpu);
    out->reset_linear = (uint32_t)(cpu->seg[REAL_CS].base + cpu->next_ip);
    out->reset_ok = cpu->seg[REAL_CS].sel == 0xf000 &&
                    cpu->seg[REAL_CS].base == 0xf0000 &&
                    cpu->ip == 0xfff0 && cpu->next_ip == 0xfff0 &&
                    out->reset_linear == 0xffff0u && cpu->code16 &&
                    (cpu->cr0 & 0x80000001u) == 0;
    if (!out->reset_ok) {
        delete_smoke_cpu(cpu);
        return -3;
    }

    /* The reset vector is five bytes; do not decode or execute anything if
     * it is not the expected 16-bit FAR JMP form. */
    if ((uint8_t)ram[0xffff0] != 0xea) {
        delete_smoke_cpu(cpu);
        /* A valid reset state and loaded image are still a PASS.  This probe
         * intentionally does not guess an alternate BIOS entry encoding. */
        return 0;
    }
    out->vector_ip = (uint16_t)((uint8_t)ram[0xffff1] |
                                ((uint16_t)(uint8_t)ram[0xffff2] << 8));
    out->vector_cs = (uint16_t)((uint8_t)ram[0xffff3] |
                                ((uint16_t)(uint8_t)ram[0xffff4] << 8));
    out->vector_ok = 1;
    out->target_linear = ((uint32_t)out->vector_cs << 4) + out->vector_ip;
    out->target_ok = out->target_linear >= bios_base &&
                     out->target_linear < bios_base + bios_size &&
                     out->target_linear < TINY386_BIOS_RAM_SIZE;
    if (!out->target_ok) {
        delete_smoke_cpu(cpu);
        return -5;
    }

    /* Exactly one architectural step: execute the FAR JMP itself.  The BIOS
     * target is intentionally not stepped or interpreted by this probe. */
    cpui386_step(cpu, 1);
    out->step_done = 1;
    const int ok = !cpu->halt && cpu->seg[REAL_CS].sel == out->vector_cs &&
                   cpu->seg[REAL_CS].base == ((uint32_t)out->vector_cs << 4) &&
                   cpu->next_ip == out->vector_ip;
    delete_smoke_cpu(cpu);
    return ok ? 0 : -6;
}
