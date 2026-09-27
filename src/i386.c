#include "i386.h"
#include "jit/jit.h"
#include "audiodiag.h"
#include <pico.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#if defined(CIRCLE_PC_DIAG)
#include "circle_pc_diag.h"
#else
#define CIRCLE_PC_IRQ_DELIVERED(...) ((void)0)
#endif

#ifdef BUILD_ESP32
#include "esp_attr.h"
#else
#define IRAM_ATTR 
#define IRAM_ATTR_CPU_EXEC1 
#endif

#define I386_OPT1
#ifndef __wasm__
#define I386_OPT2
#endif

#define I386_ENABLE_FPU 1


#ifdef I386_ENABLE_FPU
#include "fpu.h"
#else
#define fpu_new(...) NULL
#define fpu_exec1(...) false
#define fpu_exec2(...) false
#define fpu_delete(...)
#endif

/* See the interrupt note in the step loop. */
uint32_t g_intr_taken;
uint32_t g_intr_blocked_if;

/* Prefetch buffer: holds 4 bytes fetched as one 32-bit aligned read.
 * cpu->prefetch_base is the physical address of the aligned 4-byte slot currently
 * in the buffer (always a multiple of 4).  (u32)-1 means "invalid / empty".
 * Invalidated automatically when the physical address of next_ip falls outside
 * the current 4-byte slot */
//static u32 cpu->prefetch_base = (u32)-1;
//static u8  cpu->prefetch[16] __attribute__((aligned(4))) = {0};

// #define DEBUG_CPU 1
#ifdef DEBUG_CPU
#include <stdarg.h>
#include "ff.h"

static u8 opcode;
void dolog(const char *fmt, ...)
{
	static FIL _tf;
	static int _tf_open = 0;
    if (!_tf_open) _tf_open = (f_open(&_tf, "386/cpu.txt", FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS) == FR_OK);
    if (!_tf_open) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return;
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    UINT bw;
    f_write(&_tf, buf, len, &bw);
    f_sync(&_tf);
}
#else
#define dolog(...) (void)0
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define wordmask ((uword) ((sword) -1))
#define TRY(f) if(!(f)) { return false; }
#define TRYL(f) if(unlikely(!(f))) { return false; }
#define TRY1(f) if(unlikely(!(f))) { dolog("TRY1 @ %s %d\n", __func__, __LINE__); cpu_abort(cpu, -1); }

#define THROW(ex, err) do { \
    dolog("THROW ex=%d err=%x eip=%08x cs=%04x %s:%d\n", \
          (ex), (unsigned)(err), cpu->ip, cpu->seg[SEG_CS].sel,  __func__, __LINE__); \
    frank_diag_exc(cpu->seg[SEG_CS].base, cpu->ip, (uint32_t)(ex), \
                   (uint32_t)(err), (uint32_t)cpu->flags); \
    cpu->excno = (ex); cpu->excerr = (err); \
	return false; \
} while(0)
#define THROW0(ex) do { \
    dolog("THROW0 ex=%d eip=%08x cs=%04x op=%02x %s:%d\n", (ex), cpu->ip, cpu->seg[SEG_CS].sel, \
          opcode, __func__, __LINE__); \
	frank_diag_exc(cpu->seg[SEG_CS].base, cpu->ip, (uint32_t)(ex), \
	               0u /* opcode is not in scope at every THROW0 site */, (uint32_t)cpu->flags); \
	cpu->excno = (ex); \
	return false; \
} while(0)

// the second branchless version works better on gcc
//#define SET_BIT(w, f, m) ((w) ^= ((-(uword)(f)) ^ (w)) & (m))
#define SET_BIT(w, f, m) ((w) = ((w) & ~((uword)(m))) | ((-(uword)(f)) & (m)))
//#define SET_BIT(w, f, m) do { if (f) (w) |= (m); else (w) &= ~(m); } while (0)

enum {
	EX_DE,
	EX_DB,
	EX_NMI,
	EX_BP,
	EX_OF,
	EX_BR,
	EX_UD,
	EX_NM,
	EX_DF,
	EX_INT9,
	EX_TS,
	EX_NP,
	EX_SS,
	EX_GP,
	EX_PF,
};



enum {
	CF = 0x1,
	/* 1 0x2 */
	PF = 0x4,
	/* 0 0x8 */
	AF = 0x10,
	/* 0 0x20 */
	ZF = 0x40,
	SF = 0x80,
	TF = 0x100,
	IF = 0x200,
	DF = 0x400,
	OF = 0x800,
	IOPL = 0x3000,
	NT = 0x4000,
	/* 0 0x8000 */
	RF = 0x10000,
	VM = 0x20000,
};

enum {
	SEG_D_BIT = 1 << 14,
	SEG_B_BIT = 1 << 14,
};

#ifdef I386_OPT1
#define REGi(x) (cpu->gprx[x].r32)
#else
#define REGi(x) (cpu->gpr[x])
#endif
#define SEGi(x) (cpu->seg[x].sel)

static void cpu_debug(CPUI386 *cpu);

void cpu_abort(CPUI386 *cpu, int code)
{
	dolog("abort: %d %x cycle %ld\n", code, code, cpu->cycle);
	cpu_debug(cpu);
	abort();
}

static inline uword sext8(u8 a)
{
	return (sword) (s8) a;
}

static inline uword sext16(u16 a)
{
	return (sword) (s16) a;
}

static inline uword sext32(u32 a)
{
	return (sword) (s32) a;
}

#ifdef I386_OPT1
/* only works on hosts that are little-endian and support unaligned access */
static inline u8 pload8_local(CPUI386 *cpu, uword addr)
{
	return cpu->phys_mem[addr];
}

static inline u16 pload16_local(CPUI386 *cpu, uword addr)
{
	return *(u16 *)&(cpu->phys_mem[addr]);
}

static inline u32 pload32_local(CPUI386 *cpu, uword addr)
{
	return *(u32 *)&(cpu->phys_mem[addr]);
}

static inline void pstore8_local(CPUI386 *cpu, uword addr, u8 val)
{
	cpu->phys_mem[addr] = val;
}

static inline void pstore16_local(CPUI386 *cpu, uword addr, u16 val)
{
	*(u16 *)&(cpu->phys_mem[addr]) = val;
}

static inline void pstore32_local(CPUI386 *cpu, uword addr, u32 val)
{
	*(u32 *)&(cpu->phys_mem[addr]) = val;
}
#else
static inline u8 pload8_local(CPUI386 *cpu, uword addr)
{
	return cpu->phys_mem[addr];
}

static inline u16 pload16_local(CPUI386 *cpu, uword addr)
{
	u8 *mem = (u8 *) cpu->phys_mem;
	return mem[addr] | (mem[addr + 1] << 8);
}

static inline u32 pload32_local(CPUI386 *cpu, uword addr)
{
	u8 *mem = (u8 *) cpu->phys_mem;
	return mem[addr] | (mem[addr + 1] << 8) |
		(mem[addr + 2] << 16) | (mem[addr + 3] << 24);
}

static inline void pstore8_local(CPUI386 *cpu, uword addr, u8 val)
{
	cpu->phys_mem[addr] = val;
}

static inline void pstore16_local(CPUI386 *cpu, uword addr, u16 val)
{
	cpu->phys_mem[addr] = val;
	cpu->phys_mem[addr + 1] = val >> 8;
}

static inline void pstore32_local(CPUI386 *cpu, uword addr, u32 val)
{
	cpu->phys_mem[addr] = val;
	cpu->phys_mem[addr + 1] = val >> 8;
	cpu->phys_mem[addr + 2] = val >> 16;
	cpu->phys_mem[addr + 3] = val >> 24;
}
#endif

/*
 * Remote window dispatch.
 *
 * A slice of guest physical memory can be served out of the slave
 * RP2350's SRAM, which is measured at 89 core cycles per access against
 * 182 for the master's own PSRAM — twice as fast. See remote_mem.h.
 *
 * The check sits here rather than in load8()/store8() on purpose. Those
 * already range-check every access, so hooking there would have cost
 * nothing — but page-table walks and instruction prefetch call pload32()
 * directly, and a window that quietly mishandled either would fail in
 * ways that look nothing like a memory fault.
 *
 * When REMOTE_MEM is not compiled in, is_remote() is a constant false
 * and every one of these folds back into the bare local access.
 */
static inline u8 pload8(CPUI386 *cpu, uword addr)
{
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) return remote_read8(addr);
#endif
	return pload8_local(cpu, addr);
}

static inline u16 pload16(CPUI386 *cpu, uword addr)
{
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) return remote_read16(addr);
#endif
	return pload16_local(cpu, addr);
}

static inline u32 pload32(CPUI386 *cpu, uword addr)
{
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) return remote_read32(addr);
#endif
	return pload32_local(cpu, addr);
}

/*
 * The system BIOS is ROM on a PC, and a store to it has to be dropped rather
 * than land in RAM.
 *
 * Prehistorik 2 looks for a debugger by reading the INT 3 vector, poking the
 * byte it points at, and folding the result into an opcode of its own:
 *
 *      lds  bx, [000c]        ; the INT 3 vector - F000:06F4 under SeaBIOS
 *      mov  al, [bx]
 *      xor  byte [bx], 55     ; a no-op on a real machine: that is ROM
 *      sub  al, [bx]          ; so al comes out zero
 *      add  cs:[62b8], al     ; and the opcode is left alone
 *
 * With the BIOS sitting in plain writable RAM the xor sticks instead, al comes
 * out 0x00 - 0x55 = 0xab, and the game's own `push ax` at 10BB:62B8 becomes
 * `sti`.  That routine then pops four registers against three pushes, returns
 * two bytes off, and runs away into the interrupt vector table until it hits
 * an invalid opcode.  The corruption happens once, while the level loads; the
 * game only dies later, when it first calls the routine - which is why this
 * looked for two sessions like an intermittent fault caused by moving or by
 * touching an enemy.
 *
 * Only the last 64 KB is protected.  A 256 KB BIOS image also occupies
 * 0xc0000-0xeffff, but so do the upper memory blocks EMM386 hands out and DOS
 * loads drivers into, and blocking writes there would break far more than it
 * fixes.  0xf0000-0xfffff is ROM on every PC and nothing legitimately writes
 * to it - the BIOS image itself is put there by load_rom(), which memcpy()s
 * into phys_mem and never comes through here.
 */
/* GUEST_ prefixed: the SDK already defines ROM_BASE for the RP2350. */
#define GUEST_ROM_BASE 0xf0000u
#define GUEST_ROM_SIZE 0x10000u

static inline bool __attribute__((always_inline)) in_rom(uword addr)
{
	return (addr - GUEST_ROM_BASE) < GUEST_ROM_SIZE;
}

/*
 * Blocking the region outright stops the machine booting, because SeaBIOS
 * keeps mutable globals in the F segment and writes them during POST - just
 * as a real chipset allows while the PAM registers hold the window open.
 * Every one of the nine writes a boot makes was measured, and they agree:
 *
 *   0xf7f28 <- 1          from cs_base 0, ip 0x0f365d
 *   0xf30c8 <- 0x800000   from cs_base 0, ip 0x0ef0cd
 *   0xf7304, 0xf7320..0xf7330                cs_base 0, ip 0x0e974a..0x0e9777
 *
 * All of them come from a flat 32-bit code segment with an EIP far above
 * 0xffff, which is only reachable in protected mode: this is POST, before the
 * BIOS would have closed PAM again.  Everything from DOS onwards runs in real
 * or V86 mode, where a real machine has ROM there and nothing can write to it.
 *
 * Emulating PAM properly would be the exact answer, but the i440FX host
 * bridge only stores what the guest writes to its PAM registers (see
 * i440fx_init), so nothing acts on them.  The mode test costs one
 * compare on a path that is already off the hot road - only addresses inside
 * the 64 KB reach it - and separates the two cases exactly as observed.
 */
static inline bool __attribute__((always_inline)) rom_write_allowed(CPUI386 *cpu)
{
	return (cpu->cr0 & 1) && !(cpu->flags & VM);
}

/*
 * The writes that were dropped, so a regression is visible rather than
 * silent: 32 entries of {address, value, CS base, IP} at guest 0xb4800, in
 * the gap between the shadow copy and the port histograms, with the running
 * total just past the end of the ring.
 */
/*
 * Ordinary memory, not a fixed address - the same mistake as the AdLib and
 * Sound Blaster rings, and this one is on the store path, so it fired on
 * every write a guest aimed at ROM.  0x110b4800 is PSRAM on the RP2350 and
 * about 286 MB into a Raspberry Pi's ordinary RAM, which is memory Circle
 * is entitled to be using.
 */
#define ROMLOG_N    32u
static volatile uint32_t romlog_block[ROMLOG_N * 4u + 1u];
#define ROMLOG_RING romlog_block
static uint32_t romlog_head;

static inline void __attribute__((always_inline))
rom_write_log(CPUI386 *cpu, uword addr, uint32_t val)
{
	uint32_t n = romlog_head++;
	if (n < ROMLOG_N) {
		volatile uint32_t *e = ROMLOG_RING + n * 4u;
		e[0] = addr;
		e[1] = val;
		e[2] = cpu->seg[SEG_CS].base;
		e[3] = cpu->ip;
	}
	/* The count keeps going past the ring so the total is readable too. */
	ROMLOG_RING[ROMLOG_N * 4u] = romlog_head;
}

#if defined(CPU_STACK_WATCH)
/*
 * Who overwrites four bytes of a Windows 95 stack frame.
 *
 * DetectESDI in SYSDETMG.DLL keeps the two ports it probes, 0x1f0 and 0x170,
 * as one dword at [bp-0x22], stored with a single MOV of 0x017001f0.  By the
 * time it adds 0x206 to the first of them for the control port, the dword
 * has become something else - 0x5edef90e once, 0 another time - and
 * Windows records a disk controller at a port that does not exist.  The
 * detection code is correct on real machines, so the write comes from this
 * emulator.
 *
 * So the watch arms itself on that exact store, at the physical address it
 * went to, and then records every write and every word read that touches
 * those four bytes, with enough of the machine's state to name the
 * instruction.  A read that should have happened and is missing says the
 * address itself was computed differently.
 */
static uword sw_phys = (uword)-1;
static void sw_note(CPUI386 *cpu, uword addr, int size, u32 val, int kind);
#define SW_HIT(addr, size) \
	(unlikely((uword)(addr) - sw_phys < 4u || sw_phys - (uword)(addr) < (uword)(size)))
/* For writers inside macros, where #if cannot go. */
#define SW_RANGE(cpu, addr, len, kind) \
	do { if (SW_HIT(addr, len)) sw_note(cpu, addr, (int)(len), 0, kind); } while (0)
#else
#define SW_RANGE(cpu, addr, len, kind) ((void)0)
#endif

static inline void pstore8(CPUI386 *cpu, uword addr, u8 val)
{
#if defined(CPU_STACK_WATCH)
	if (SW_HIT(addr, 1)) sw_note(cpu, addr, 1, val, 'W');
#endif
	if (unlikely(in_rom(addr)) && !rom_write_allowed(cpu)) {
		rom_write_log(cpu, addr, val);
		return;
	}
	frank_diag_wp(addr, val, cpu->seg[SEG_CS].base, cpu->ip);
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) { remote_write8(addr, val); return; }
#endif
	pstore8_local(cpu, addr, val);
}

static inline void pstore16(CPUI386 *cpu, uword addr, u16 val)
{
#if defined(CPU_STACK_WATCH)
	if (SW_HIT(addr, 2)) sw_note(cpu, addr, 2, val, 'W');
#endif
	if (unlikely(in_rom(addr)) && !rom_write_allowed(cpu)) {
		rom_write_log(cpu, addr, val);
		return;
	}
	frank_diag_wp(addr, val, cpu->seg[SEG_CS].base, cpu->ip);
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) { remote_write16(addr, val); return; }
#endif
	pstore16_local(cpu, addr, val);
}

static inline void pstore32(CPUI386 *cpu, uword addr, u32 val)
{
#if defined(CPU_STACK_WATCH)
	if (SW_HIT(addr, 4)) sw_note(cpu, addr, 4, val, 'W');
#endif
	if (unlikely(in_rom(addr)) && !rom_write_allowed(cpu)) {
		rom_write_log(cpu, addr, val);
		return;
	}
	frank_diag_wp(addr, val, cpu->seg[SEG_CS].base, cpu->ip);
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) { remote_write32(addr, val); return; }
#endif
	pstore32_local(cpu, addr, val);
}


static int  get_CF (CPUI386 *cpu)
{
	if (cpu->cc.mask & CF) {
		switch(cpu->cc.op) {
		case CC_ADC:
			return cpu->cc.dst <= cpu->cc.src2;
		case CC_ADD:
			return cpu->cc.dst < cpu->cc.src2;
		case CC_SBB:
			return cpu->cc.src1 <= cpu->cc.src2;
		case CC_SUB:
			return cpu->cc.src1 < cpu->cc.src2;
		case CC_NEG8: case CC_NEG16: case CC_NEG32:
			return cpu->cc.dst != 0;
		case CC_DEC8: case CC_DEC16: case CC_DEC32:
		case CC_INC8: case CC_INC16: case CC_INC32:
			assert(false); // should not happen
		case CC_IMUL8:
			return sext8(cpu->cc.dst) != cpu->cc.dst;
		case CC_IMUL16:
			return sext16(cpu->cc.dst) != cpu->cc.dst;
		case CC_IMUL32:
			return (((s32) cpu->cc.dst) >> 31) != cpu->cc.dst2;
		case CC_MUL8:
			return (cpu->cc.dst >> 8) != 0;
		case CC_MUL16:
			return (cpu->cc.dst >> 16) != 0;
		case CC_MUL32:
			return (cpu->cc.dst2) != 0;
		case CC_SHL:
		case CC_SHR:
		case CC_SAR:
			return cpu->cc.dst2 & 1;
		case CC_SHLD:
			return cpu->cc.dst2 >> 31;
		case CC_SHRD:
			return cpu->cc.dst2 & 1;
		case CC_BSF:
		case CC_BSR:
			return 0;
		case CC_AND:
		case CC_OR:
		case CC_XOR:
			return 0;
		}
	} else {
		return !!(cpu->flags & CF);
	}
	assert(false);
}

const static u8 parity_tab[256] = {
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
};

static inline int get_PF(CPUI386 *cpu)
{
	if (cpu->cc.mask & PF) {
		return parity_tab[cpu->cc.dst & 0xff];
	} else {
		return !!(cpu->flags & PF);
	}
}

static inline int get_AF(CPUI386 *cpu)
{
	if (cpu->cc.mask & AF) {
		switch(cpu->cc.op) {
		case CC_ADC:
		case CC_ADD:
		case CC_SBB:
		case CC_SUB:
			return ((cpu->cc.src1 ^ cpu->cc.src2 ^ cpu->cc.dst) >> 4) & 1;
		case CC_NEG8: case CC_NEG16: case CC_NEG32:
			return (cpu->cc.dst & 0xf) != 0;
		case CC_DEC8: case CC_DEC16: case CC_DEC32:
			return (cpu->cc.dst & 0xf) == 0xf;
		case CC_INC8: case CC_INC16: case CC_INC32:
			return (cpu->cc.dst & 0xf) == 0;
		case CC_IMUL8: case CC_IMUL16: case CC_IMUL32:
		case CC_MUL8: case CC_MUL16: case CC_MUL32:
			return 0;
		case CC_SAR:
		case CC_SHL:
		case CC_SHR:
		case CC_SHLD:
		case CC_SHRD:
		case CC_BSF:
		case CC_BSR:
		case CC_AND:
		case CC_OR:
		case CC_XOR:
			return 0;
		}
	} else {
		return !!(cpu->flags & AF);
	}
	assert(false);
}

static int IRAM_ATTR get_ZF(CPUI386 *cpu)
{
	if (cpu->cc.mask & ZF) {
		return cpu->cc.dst == 0;
	} else {
		return !!(cpu->flags & ZF);
	}
}

static int IRAM_ATTR get_SF(CPUI386 *cpu)
{
	if (cpu->cc.mask & SF) {
		return cpu->cc.dst >> (sizeof(uword) * 8 - 1);
	} else {
		return !!(cpu->flags & SF);
	}
}

static int IRAM_ATTR get_OF(CPUI386 *cpu)
{
	if (cpu->cc.mask & OF) {
		switch(cpu->cc.op) {
		case CC_ADC:
		case CC_ADD:
			return (~(cpu->cc.src1 ^ cpu->cc.src2) & (cpu->cc.dst ^ cpu->cc.src2)) >> (sizeof(uword) * 8 - 1);
		case CC_SBB:
		case CC_SUB:
			return ((cpu->cc.src1 ^ cpu->cc.src2) & (cpu->cc.dst ^ cpu->cc.src1)) >> (sizeof(uword) * 8 - 1);
		case CC_DEC8:
			return cpu->cc.dst == sext8((u8) ~(1u << 7));
		case CC_DEC16:
			return cpu->cc.dst == sext16((u16) ~(1u << 15));
		case CC_DEC32:
			return cpu->cc.dst == sext32((u32) ~(1u << 31));
		case CC_INC8: case CC_NEG8:
			return cpu->cc.dst == sext8(1u << 7);
		case CC_INC16: case CC_NEG16:
			return cpu->cc.dst == sext16(1u << 15);
		case CC_INC32: case CC_NEG32:
			return cpu->cc.dst == sext32(1u << 31);
		case CC_IMUL8: case CC_IMUL16: case CC_IMUL32:
		case CC_MUL8: case CC_MUL16: case CC_MUL32:
			return get_CF(cpu);
		case CC_SAR:
			return 0;
		case CC_SHL:
			return (cpu->cc.dst >> (sizeof(uword) * 8 - 1)) ^ (cpu->cc.dst2 & 1);
		case CC_SHR:
			return (cpu->cc.src1 >> (sizeof(uword) * 8 - 1));
		case CC_SHLD:
		case CC_SHRD:
			return (cpu->cc.src1 ^ cpu->cc.dst) >> (sizeof(uword) * 8 - 1);
		case CC_BSF:
		case CC_BSR:
			return 0;
		case CC_AND:
		case CC_OR:
		case CC_XOR:
			return 0;
		}
		assert(false);
	} else {
		return !!(cpu->flags & OF);
	}
	assert(false);
}

static void IRAM_ATTR refresh_flags(CPUI386 *cpu)
{
	/*
	 * Only materialise the flags that are actually pending.
	 *
	 * cc.mask says which flags are still held lazily in cc.op/dst/src.
	 * For every other flag the getters simply return the bit already in
	 * cpu->flags, so calling them writes back what is already there —
	 * six out-of-line calls (get_CF and friends are real functions, not
	 * inlined) and six big switches to compute nothing.
	 *
	 * Measured at 12.4% of core-0 time in a Wolf3D profile, which is
	 * what makes a guard this simple worth having: PUSHF, LAHF and every
	 * interrupt entry land here, and real-mode DOS code does all three
	 * constantly.
	 *
	 * Equivalent by construction: when a mask bit is clear the getter is
	 * defined to return the current cpu->flags bit, so skipping it
	 * cannot change the result.
	 */
	uword mask = cpu->cc.mask;
	if (mask & CF) SET_BIT(cpu->flags, get_CF(cpu), CF);
	if (mask & PF) SET_BIT(cpu->flags, get_PF(cpu), PF);
	if (mask & AF) SET_BIT(cpu->flags, get_AF(cpu), AF);
	if (mask & ZF) SET_BIT(cpu->flags, get_ZF(cpu), ZF);
	if (mask & SF) SET_BIT(cpu->flags, get_SF(cpu), SF);
	if (mask & OF) SET_BIT(cpu->flags, get_OF(cpu), OF);
}

static inline int get_IOPL(CPUI386 *cpu)
{
	return (cpu->flags & IOPL) >> 12;
}

/*
 * FRANK_WORKLOAD_PROFILE_V88
 *
 * Window-scoped workload counters for the Win+F7 .. Win+F8 capture.
 *
 * v8.7.3 compiled the single hottest entry of the v8.7.2 JIT reject list
 * (the F000:8E15 byte-clear micro-loop) and the Symantec-under-EMM386 score
 * did not move at all, while v8.5.1 -> v8.6 produced byte-identical counters.
 * Two consecutive A/Bs therefore say the reject list is not where the EMM386
 * time is.  Choosing the next optimisation needs numbers no previous capture
 * contained:
 *
 *   - guest instructions retired inside the window, so native_guest_insns
 *     becomes a real coverage fraction instead of a guess;
 *   - how much of the window is VM86+paging at all;
 *   - what the paging path itself costs: TLB refills and full TLB flushes are
 *     page-table traffic against PSRAM, measured by this project at ~182 core
 *     cycles per uncached access (see the remote-memory note above pload8());
 *   - how much of the window is service work (software INT, hardware IRQ,
 *     faults) rather than benchmark compute.
 *
 * Every counter below is incremented only on a path that is already far more
 * expensive than an SRAM increment - TLB refill, TLB flush, exception entry,
 * software interrupt - or once per taken backward branch, which already calls
 * the JIT dispatcher.  None of them sits on the per-instruction decode path,
 * and none of them changes emulated behaviour.
 */
volatile u32 g_wl_tlb_refills __attribute__((used));
volatile u32 g_wl_tlb_clears __attribute__((used));
volatile u32 g_wl_exc_pf __attribute__((used));
volatile u32 g_wl_exc_gp __attribute__((used));
volatile u32 g_wl_exc_other __attribute__((used));
volatile u32 g_wl_hw_irq __attribute__((used));
/*
 * FRANK_WL_RAM_BUDGET_V89_1
 *
 * Counters that land inside a RAM-resident function are far more
 * expensive than they look.  cpu_exec1(), nj_exec_loop(), tlb_refill()
 * and translate*() are all , so their code sits in
 * .data, and at v8.7.3 .data ended exactly 8 bytes below the 4 KB
 * boundary that .bss is aligned to.  Any growth there therefore costs a
 * further 4096 bytes of link padding on top of the code itself, taken
 * straight out of the malloc heap that pc_new() draws on.
 *
 * The v8.9 build did exactly that: three counters inlined at the 13
 * NJ_HOT_BACKEDGE sites pushed RAM from 91.66% to 92.50%, and the board
 * came up with no HDMI signal.  codeprofile.h already records the same
 * failure mode from the other direction - .bss growth leaving pc_new()
 * with nothing, the emulator reaching vga_initialized and never
 * finishing init.
 *
 * So the backedge counters are gone.  They did their job: they are what
 * proved 99.5% of backedges were being dropped at the TF guard.  What is
 * kept below costs nothing in RAM-resident code, or is small enough to
 * measure and stay under the boundary.
 */

/*
 * FRANK_TF_V88
 *
 * Single-step debug exceptions are not implemented by this emulator.
 * EX_DB is declared in the exception enum and never thrown; cpu->dr[] is
 * zeroed at reset and otherwise only moved in and out by MOV DR.  TF is
 * therefore stored, pushed, popped and cleared on interrupt entry, but it
 * never causes a trap, and no guest single-step handler ever runs to
 * clear it again.  Once some guest sets it, it stays set for the rest of
 * the session.
 *
 * That is exactly what happens under HIMEM + EMM386.  In the
 * jitstats001 capture (Symantec under M602 + EMM386, score 8.7) the CPU
 * mode at dump time is flags=00023347, i.e. VM | IOPL=3 | IF | TF, and
 * the counters say the same thing independently:
 *
 *   window_backedges                       = 2338872
 *   hits                                   =    6842
 *   misses * 256 (sampled discovery)       =    4864
 *   ------------------------------------------------
 *   backedges that got past the TF test    =   11706  (0.5%)
 *
 * so 99.5% of all taken backward branches returned at the TF guard and
 * the JIT was switched off for the whole run: native_coverage_ppm=4070,
 * against 745770 for the identical benchmark without EMM386, which
 * scores 345 instead of 8.7.
 *
 * The guard exists to preserve single-step semantics that the
 * interpreter does not implement, so it protects nothing while costing
 * the entire JIT.  A JIT block executes exactly what the interpreter
 * would have executed, so running one with TF set is indistinguishable
 * from interpreting with TF set - which is what already happens today.
 *
 * If TF -> #DB is ever implemented, set this back to 1 and both guards
 * below become live again.
 */
#define NJ_SINGLE_STEP_IMPLEMENTED 0

/*
 * Runtime form of the guard rather than #if.
 *
 * Deleting the test outright cost 408 bytes of .data: nj_try_execute()
 * is always_inline at 13 NJ_HOT_BACKEDGE sites inside cpu_exec1(), which
 * is , and losing the early return let GCC materialise
 * more of the body at each one.  .data ended 8 bytes below the 4 KB
 * boundary .bss is aligned to, so those 408 bytes pulled in a further
 * 4096 of link padding and the board came up with no HDMI signal - the
 * failure mode codeprofile.h already records, where pc_new() cannot get
 * its heap and vga_hw_set_vga_state() is therefore never reached.
 *
 * Keeping the branch and testing a variable the compiler cannot fold
 * preserves the generated shape, so the size stays where it was.  The
 * flag is 0, so the guard never fires; it also makes the whole change
 * A/B-testable on one firmware image if that is ever wanted.
 */

/* MMU */
#define CR0_PG (1<<31)
#define CR0_WP (0x10000)
#ifdef BUILD_ESP32
#define tlb_size 256
#else
#define tlb_size 512
#endif
typedef struct {
	enum {
		ADDR_OK1,
		ADDR_OK2,
	} res;
	uword addr1;
	uword addr2;
} OptAddr;

static void tlb_clear(CPUI386 *cpu)
{
	g_wl_tlb_clears++;
	for (int i = 0; i < tlb_size; i++) {
		cpu->tlb.tab[i].lpgno = -1;
	}
	cpu->ifetch.laddr = -1;
	cpu->prefetch_base = (u32)-1;
}

static int pte_lookup[2][4][2][2] = { //[wp != 0][(pte >> 1) & 3][cpl > 0][rwm > 1]
	{ // wp == 0
		{ {0, 0}, {1, 1} }, // s,r
		{ {0, 0}, {1, 1} }, // s,w
		{ {0, 0}, {0, 1} }, // u,r
		{ {0, 0}, {0, 0} }, // u,w
	},
	{ // wp == 1
		{ {0, 1}, {1, 1} }, // s,r
		{ {0, 0}, {1, 1} }, // s,w
		{ {0, 1}, {0, 1} }, // u,r
		{ {0, 0}, {0, 0} }, // u,w
	}
};

static bool IRAM_ATTR tlb_refill(CPUI386 *cpu, struct tlb_entry *ent, uword lpgno)
{
	g_wl_tlb_refills++;

	uword base_addr = cpu->cr3 & ~0xfff;
	uword i = lpgno >> 10;
	uword j = lpgno & 1023;

	u8 *mem = (u8 *) cpu->phys_mem;
	uword pde = pload32(cpu, base_addr + i * 4);
	if (!(pde & 1))
		return false;
	mem[base_addr + i * 4] |= 1 << 5; // accessed

	uword base_addr2 = pde & ~0xfff;
	uword pte = pload32(cpu, base_addr2 + j * 4);
	if (!(pte & 1))
		return false;

	mem[base_addr2 + j * 4] |= 1 << 5; // accessed
//	mem[base_addr2 + j * 4] |= 1 << 6; // dirty

	ent->lpgno = lpgno;
	ent->xaddr = (pte & ~0xfff) ^ (lpgno << 12);
	pte = pte & ((pde & 7) | 0xfffffff8);
	{
		const int (*row)[2] = pte_lookup[!!(cpu->cr0 & CR0_WP)][(pte >> 1) & 3];
		ent->deny[0] = (u8)row[0][0];
		ent->deny[1] = (u8)row[0][1];
		ent->deny[2] = (u8)row[1][0];
		ent->deny[3] = (u8)row[1][1];
	}
	ent->ppte = &(mem[base_addr2 + j * 4]);
	return true;
}

static bool IRAM_ATTR translate_lpgno(CPUI386 *cpu, int rwm, uword lpgno, uword laddr, int cpl, uword *paddr)
{
	struct tlb_entry *ent = &(cpu->tlb.tab[lpgno % tlb_size]);
	if (ent->lpgno != lpgno) {
		if (!tlb_refill(cpu, ent, lpgno)) {
			cpu->cr2 = laddr;
			cpu->excno = EX_PF;
			cpu->excerr = 0;
			if (rwm & 2)
				cpu->excerr |= 2;
			if (cpl)
				cpu->excerr |= 4;
			return false;
		}
	}
	if (ent->deny[((cpl > 0) << 1) | (rwm > 1)]) {
		cpu->cr2 = laddr;
		cpu->excno = EX_PF;
		cpu->excerr = 1;
		if (rwm & 2)
			cpu->excerr |= 2;
		if (cpl)
			cpu->excerr |= 4;
		ent->lpgno = -1;
		return false;
	}
	*paddr = ent->xaddr ^ laddr;
	if (rwm & 2) {
		/*
		 * Set the PTE dirty bit only when it is actually clear.
		 *
		 * This is a read-modify-write of guest memory, and guest memory is
		 * PSRAM: the unconditional OR turned every guest write into an extra
		 * store, on a page that is almost always dirty already after the
		 * first write to it.  Testing first keeps the load - which the cache
		 * serves - and drops the store.
		 */
		u8 *ppte = ent->ppte;
		if (unlikely(!(*ppte & (1 << 6))))
			*ppte |= 1 << 6;
	}
	return true;
}

static bool IRAM_ATTR translate_laddr(CPUI386 *cpu, OptAddr *res, int rwm, uword laddr, int size, int cpl)
{
	if (cpu->cr0 & CR0_PG) {
		uword lpgno = laddr >> 12;
		uword paddr;
		TRY(translate_lpgno(cpu, rwm, lpgno, laddr, cpl, &paddr));
		res->res = ADDR_OK1;
		res->addr1 = paddr;
		if ((laddr & 0xfff) > 0x1000 - size) {
			lpgno++;
			TRY(translate_lpgno(cpu, rwm, lpgno, lpgno << 12, cpl, &paddr));
			res->res = ADDR_OK2;
			res->addr2 = paddr;
		}
	} else {
		res->res = ADDR_OK1;
		res->addr1 = laddr;
	}
	return true;
}

/*
 * How many accesses the segment bounds refused.
 *
 * This emulator used not to check segment limits at all: a read or a write
 * past the end of a segment quietly succeeded where a real 386 raises #GP.
 * That matters for 16-bit Windows, which is built on segment limits and
 * whose own pointer validation depends on the processor refusing the
 * access.  Counted so the effect of the check can be seen rather than
 * assumed.
 */
uint32_t g_seg_refused;

/* Set when the rings have been stopped on demand; see cpu_exclog_freeze(). */
uint32_t g_exc_frozen;

/*
 * The last few refusals, in full.
 *
 * A count says the check fires; it does not say whether it was right to.
 * Each of these can be held against the descriptor the guest loaded - the
 * offset asked for, the width, and the bounds that segment was given - and
 * decided one way or the other.  Eight is enough: they arrive in ones and
 * twos, minutes apart.
 */
/*
 * What the checks caught, kept so a whole run can be judged from one pass.
 *
 * Re-running the guest to add one more probe costs twenty minutes, so these
 * rings record everything that might be wanted rather than the one thing the
 * current guess needs: the refusals with the call stack that asked for them,
 * the segments the processor made unusable, and the entries into V86 mode.
 * Consecutive identical events are collapsed with a count, so a machine stuck
 * in a loop leaves the run-up to it in the ring instead of overwriting it.
 */
#define SEG_REF_N 24
#define SEG_RA_N  4

struct SegRefRec {
	uint32_t t_us, cs_base, ip, addr, lo, hi, esp, eflags, hits;
	uint64_t ra[SEG_RA_N];
	uint16_t sel, flags;
	uint8_t  seg, size, rwm, cpl;
	uint8_t  code[8], codelen;
};

/*
 * A call through a gate, as the gate described it.
 *
 * A 286 gate carries word parameters and a 386 gate carries dwords; copying
 * the wrong width reads twice as far up the caller's stack and hands the
 * callee halves of two parameters.  Nothing else in a run says which width
 * was used, so it is recorded here next to the stack it was read from.
 */
struct GateRec {
	uint32_t t_us, cs_base, ip, oldsp, hits;
	uint16_t sel, newcs, oldss;
	uint8_t  gt, wc, cpl, newdpl;
};
static struct GateRec gate_ent[SEG_REF_N];
static uint32_t gate_ent_head;
static struct SegRefRec seg_ref[SEG_REF_N];
static uint32_t seg_ref_head;

struct SegClrRec {
	uint32_t t_us, cs_base, ip, hits;
	uint16_t sel, flags;
	uint8_t  seg, cpl;
};
static struct SegClrRec seg_clr[SEG_REF_N];
static uint32_t seg_clr_head;

struct V86EntRec {
	uint32_t t_us, from_cs_base, from_ip, eip, esp, eflags, hits;
	uint16_t cs, ss, ds, es;
};
static struct V86EntRec v86_ent[SEG_REF_N];
static uint32_t v86_ent_head;

static const char *const seg_nm[8] = { "es","cs","ss","ds","fs","gs","ldt","tr" };

/* The newest record, or NULL before anything has been kept. */
#define RING_LAST(ring, head) \
	((head) ? &(ring)[((head) - 1) % SEG_REF_N] : (void *)0)

void seg_note_cleared(CPUI386 *cpu, int seg)
{
	if (g_exc_frozen) return;
	struct SegClrRec *last = RING_LAST(seg_clr, seg_clr_head);
	if (last && last->seg == seg && last->sel == (uint16_t)cpu->seg[seg].sel &&
	    last->ip == cpu->ip && last->cs_base == cpu->seg[SEG_CS].base) {
		last->hits++;
		last->t_us = time_us_32();
		return;
	}
	struct SegClrRec *e = &seg_clr[seg_clr_head++ % SEG_REF_N];
	e->t_us    = time_us_32();
	e->cs_base = cpu->seg[SEG_CS].base;
	e->ip      = cpu->ip;
	e->sel     = (uint16_t)cpu->seg[seg].sel;
	e->flags   = (uint16_t)cpu->seg[seg].flags;
	e->seg     = (uint8_t)seg;
	e->cpl     = (uint8_t)cpu->cpl;
	e->hits    = 1;
}

/*
 * Not inlined, so the return addresses name the code that asked for the
 * access.  Four levels, because segcheck is reached through translate and its
 * out-of-line half, and which read it is - the ESP slot of a return frame, the
 * SS slot, a POPAD - is the whole question.
 */
static int seg_note_busy;
static bool IRAM_ATTR translate8r(CPUI386 *cpu, OptAddr *res, int seg, uword addr);
static inline bool __attribute__((always_inline)) in_iomem(uword addr);

__attribute__((noinline))
static void seg_note_refusal(CPUI386 *cpu, int rwm, int seg, uword addr, int size)
{
	if (g_exc_frozen) return;
	struct SegRefRec *last = RING_LAST(seg_ref, seg_ref_head);
	if (last && last->seg == seg && last->addr == addr &&
	    last->size == size && last->ip == cpu->ip &&
	    last->cs_base == cpu->seg[SEG_CS].base) {
		last->hits++;
		last->t_us = time_us_32();
		return;
	}
	struct SegRefRec *e = &seg_ref[seg_ref_head++ % SEG_REF_N];
	e->t_us    = time_us_32();
	e->cs_base = cpu->seg[SEG_CS].base;
	e->ip      = cpu->ip;
	e->addr    = addr;
	e->lo      = cpu->seg[seg].lo;
	e->hi      = cpu->seg[seg].hi;
#ifdef I386_OPT1
	e->esp     = cpu->gprx[4].r32;
#else
	e->esp     = cpu->gpr[4];
#endif
	e->eflags  = cpu->flags;
	e->sel     = (uint16_t)cpu->seg[seg].sel;
	e->flags   = (uint16_t)cpu->seg[seg].flags;
	e->seg     = (uint8_t)seg;
	e->size    = (uint8_t)size;
	e->rwm     = (uint8_t)rwm;
	e->cpl     = (uint8_t)cpu->cpl;
	e->hits    = 1;
	e->codelen = 0;
	/*
	 * The instruction the access belongs to.  Reading it goes back through
	 * the interpreter's own translation, so the exception state is put back
	 * afterwards and a second entry here is refused outright.
	 */
	if (!seg_note_busy) {
		seg_note_busy = 1;
		const int save_no = cpu->excno;
		const uword save_err = cpu->excerr;
		const uword save_cr2 = cpu->cr2;
		OptAddr res;
		if (translate8r(cpu, &res, SEG_CS, cpu->ip) &&
		    !in_iomem(res.addr1) &&
		    res.addr1 + 8 <= (uword)cpu->phys_mem_size) {
			int n = 8;
			const int to_edge = 4096 - (int)(res.addr1 & 4095);
			if (to_edge < n) n = to_edge;
			for (int i = 0; i < n; i++)
				e->code[i] = ((u8 *)cpu->phys_mem)[res.addr1 + i];
			e->codelen = (uint8_t)n;
		}
		cpu->excno = save_no;
		cpu->excerr = save_err;
		cpu->cr2 = save_cr2;
		seg_note_busy = 0;
	}
	e->ra[0] = (uint64_t)(uintptr_t)__builtin_return_address(0);
	e->ra[1] = (uint64_t)(uintptr_t)__builtin_return_address(1);
	e->ra[2] = (uint64_t)(uintptr_t)__builtin_return_address(2);
	e->ra[3] = (uint64_t)(uintptr_t)__builtin_return_address(3);
}

#if defined(CPU_STACK_WATCH)
/* What touched the watched bytes, first ones kept: the culprit is the first
 * foreign write, and whatever reuses the stack afterwards would bury it. */
#define SW_N 40
struct SwRec {
	uint32_t t_ms, cs_base, ip, esp, ebp, ss_base, cr3, addr, val, flags;
	uint16_t cs, ss, ds, es;
	uint8_t  kind, size, cpl, codelen;
	uint8_t  code[12];
};
static struct SwRec sw_ring[SW_N];
static uint32_t sw_count, sw_arms;
static uint16_t sw_ss;
static uint32_t sw_off;           /* the slot's offset in SS */
static int sw_track_on;
static uint16_t sw_last_ss, sw_last_cs;
static uint32_t sw_last_esp, sw_last_csbase, sw_last_ip, sw_last_flags;

static void sw_note(CPUI386 *cpu, uword addr, int size, u32 val, int kind)
{
	if (kind == 'A') {
		sw_count = 0; sw_arms++;
		/* [bp-0x22] is where the MOV that armed us stored. */
		sw_ss = (uint16_t)cpu->seg[SEG_SS].sel;
		sw_off = (REGi(5) - 0x22u) & 0xffffu;
		sw_track_on = 1;
		sw_last_ss = sw_ss; sw_last_esp = REGi(4); sw_last_flags = cpu->flags;
		sw_last_cs = (uint16_t)cpu->seg[SEG_CS].sel;
		sw_last_csbase = cpu->seg[SEG_CS].base; sw_last_ip = cpu->ip;
	}
	if (sw_count >= SW_N) { sw_count++; return; }
	struct SwRec *e = &sw_ring[sw_count++];
	e->t_ms    = time_us_32() / 1000u;
	e->kind    = (uint8_t)kind;
	e->size    = (uint8_t)size;
	e->addr    = addr;
	e->val     = val;
	e->cs      = (uint16_t)cpu->seg[SEG_CS].sel;
	e->cs_base = cpu->seg[SEG_CS].base;
	e->ip      = cpu->ip;
	e->ss      = (uint16_t)cpu->seg[SEG_SS].sel;
	e->ss_base = cpu->seg[SEG_SS].base;
	e->ds      = (uint16_t)cpu->seg[SEG_DS].sel;
	e->es      = (uint16_t)cpu->seg[SEG_ES].sel;
	e->esp     = REGi(4);
	e->ebp     = REGi(5);
	e->cr3     = cpu->cr3;
	e->flags   = cpu->flags;
	e->cpl     = (uint8_t)cpu->cpl;
	e->codelen = 0;
	/* The instruction's bytes, when they can be reached without faulting;
	 * the same care as seg_note_refusal() takes. */
	if (!seg_note_busy) {
		seg_note_busy = 1;
		const int save_no = cpu->excno;
		const uword save_err = cpu->excerr;
		const uword save_cr2 = cpu->cr2;
		OptAddr res;
		if (translate8r(cpu, &res, SEG_CS, cpu->ip) &&
		    !in_iomem(res.addr1) &&
		    res.addr1 + 12 <= (uword)cpu->phys_mem_size) {
			int n = 12;
			const int to_edge = 4096 - (int)(res.addr1 & 4095);
			if (to_edge < n) n = to_edge;
			for (int i = 0; i < n; i++)
				e->code[i] = ((u8 *)cpu->phys_mem)[res.addr1 + i];
			e->codelen = (uint8_t)n;
		}
		cpu->excno = save_no;
		cpu->excerr = save_err;
		cpu->cr2 = save_cr2;
		seg_note_busy = 0;
	}
}

/*
 * How the stack pointer got there.
 *
 * The first foreign writes came from KRNL386 running with SS:SP pointing at
 * the watched slot, well above where DetectESDI's own SP was.  So after the
 * arm, every instruction boundary compares SS:SP with the one before: the
 * record is made when SP enters the window around the slot, from below or
 * from another stack, and it names the instruction that did it - an IRET
 * from the VMM, a MOV SP, an LSS.
 */

static void sw_track(CPUI386 *cpu)
{
	const uint16_t ss = (uint16_t)cpu->seg[SEG_SS].sel;
	const uint32_t sp = REGi(4) & 0xffffu;
	const uint32_t lo = sw_off - 0x40u, hi = sw_off + 0x40u;
	const int in_now = ss == sw_ss && !(cpu->flags & VM) && sp >= lo && sp <= hi;
	const int in_before = sw_last_ss == sw_ss && !(sw_last_flags & VM) &&
			      (sw_last_esp & 0xffffu) >= lo && (sw_last_esp & 0xffffu) <= hi;
	if (in_now && !in_before && sw_count < SW_N) {
		struct SwRec *e = &sw_ring[sw_count++];
		e->t_ms    = time_us_32() / 1000u;
		e->kind    = 'J';
		e->size    = 0;
		e->addr    = sw_last_esp;                 /* SP before */
		e->val     = ((uint32_t)sw_last_ss << 16) | (sw_last_flags & VM ? 1u : 0u);
		e->cs      = sw_last_cs;                  /* the instruction that did it */
		e->cs_base = sw_last_csbase;
		e->ip      = sw_last_ip;
		e->ss      = ss;
		e->ss_base = cpu->seg[SEG_SS].base;
		e->ds      = (uint16_t)cpu->seg[SEG_DS].sel;
		e->es      = (uint16_t)cpu->seg[SEG_ES].sel;
		e->esp     = REGi(4);                     /* SP after */
		e->ebp     = REGi(5);
		e->cr3     = cpu->cr3;
		e->flags   = cpu->flags;
		e->cpl     = (uint8_t)cpu->cpl;
		e->codelen = 0;
		if (!seg_note_busy) {
			seg_note_busy = 1;
			const int save_no = cpu->excno;
			const uword save_err = cpu->excerr;
			const uword save_cr2 = cpu->cr2;
			OptAddr res;
			if (translate_laddr(cpu, &res, 1, sw_last_csbase + sw_last_ip, 1, 0) &&
			    !in_iomem(res.addr1) &&
			    res.addr1 + 12 <= (uword)cpu->phys_mem_size) {
				int n = 12;
				const int to_edge = 4096 - (int)(res.addr1 & 4095);
				if (to_edge < n) n = to_edge;
				for (int i = 0; i < n; i++)
					e->code[i] = ((u8 *)cpu->phys_mem)[res.addr1 + i];
				e->codelen = (uint8_t)n;
			}
			cpu->excno = save_no;
			cpu->excerr = save_err;
			cpu->cr2 = save_cr2;
			seg_note_busy = 0;
		}
	}
	sw_last_ss = ss;
	sw_last_esp = REGi(4);
	sw_last_flags = cpu->flags;
	sw_last_cs = (uint16_t)cpu->seg[SEG_CS].sel;
	sw_last_csbase = cpu->seg[SEG_CS].base;
	sw_last_ip = cpu->ip;
}

void cpu_watch_bus_write(CPUI386 *cpu, uint32_t addr, uint32_t len, uint32_t tag)
{
	if (cpu && SW_HIT(addr, len)) sw_note(cpu, addr, (int)len, tag, 'D');
}

int cpu_watch_line(int idx, char *out, int cap)
{
	if (idx == 0)
		return snprintf(out, cap, "armed %lu times, %lu touches since the last, at phys %08lx",
				(unsigned long)sw_arms, (unsigned long)sw_count,
				(unsigned long)sw_phys);
	idx--;
	if (idx < 0 || (uint32_t)idx >= sw_count || idx >= SW_N) return 0;
	const struct SwRec *e = &sw_ring[idx];
	char code[40];
	int n = 0;
	code[0] = 0;
	for (int i = 0; i < e->codelen && n < (int)sizeof code - 3; i++)
		n += snprintf(code + n, sizeof code - n, "%02x", e->code[i]);
	return snprintf(out, cap,
		"%lu.%03lus %c%u @%08lx=%08lx cs=%04x base=%08lx ip=%08lx cpl=%u%s "
		"ss=%04x base=%08lx esp=%08lx ebp=%08lx ds=%04x es=%04x fl=%08lx cr3=%08lx [%s]",
		(unsigned long)(e->t_ms / 1000u), (unsigned long)(e->t_ms % 1000u),
		e->kind, e->size, (unsigned long)e->addr, (unsigned long)e->val,
		e->cs, (unsigned long)e->cs_base, (unsigned long)e->ip, e->cpl,
		(e->flags & VM) ? " V86" : "",
		e->ss, (unsigned long)e->ss_base, (unsigned long)e->esp,
		(unsigned long)e->ebp, e->ds, e->es, (unsigned long)e->flags,
		(unsigned long)e->cr3, code);
}
#endif

void v86_note_entry(CPUI386 *cpu, uword eip, uword cs, uword eflags,
		    uword esp, uword ss, uword ds, uword es)
{
	if (g_exc_frozen) return;
	struct V86EntRec *last = RING_LAST(v86_ent, v86_ent_head);
	if (last && last->cs == (uint16_t)cs && last->eip == eip &&
	    last->ss == (uint16_t)ss && last->esp == esp &&
	    last->eflags == eflags && last->from_ip == cpu->ip) {
		last->hits++;
		last->t_us = time_us_32();
		return;
	}
	struct V86EntRec *e = &v86_ent[v86_ent_head++ % SEG_REF_N];
	e->t_us         = time_us_32();
	e->from_cs_base = cpu->seg[SEG_CS].base;
	e->from_ip      = cpu->ip;
	e->eip          = eip;
	e->esp          = esp;
	e->eflags       = eflags;
	e->cs           = (uint16_t)cs;
	e->ss           = (uint16_t)ss;
	e->ds           = (uint16_t)ds;
	e->es           = (uint16_t)es;
	e->hits         = 1;
}

/*
 * One record per call, oldest first, so nothing is lost to a line length.
 * Returns 0 once the ring is exhausted.
 */
#define RING_PICK(ring, head, idx, decl) \
	const uint32_t held = (head) < SEG_REF_N ? (head) : SEG_REF_N; \
	if ((uint32_t)(idx) >= held) return 0; \
	const uint32_t first = (head) < SEG_REF_N ? 0 : (head) % SEG_REF_N; \
	decl = &(ring)[(first + (idx)) % SEG_REF_N];

int cpu_seg_ref_line(int idx, char *out, int cap)
{
	RING_PICK(seg_ref, seg_ref_head, idx, const struct SegRefRec *e);
	int n = snprintf(out, cap,
		"%lu.%03lus x%lu %s=%04x fl=%04x at %08lx:%08lx off=%08lx w=%u %s "
		"bounds %08lx..%08lx esp=%08lx d=%ld cpl=%u efl=%08lx "
		"ra %lx %lx %lx %lx",
		(unsigned long)(e->t_us / 1000000u),
		(unsigned long)(e->t_us / 1000u % 1000u),
		(unsigned long)e->hits,
		seg_nm[e->seg & 7], e->sel, e->flags,
		(unsigned long)e->cs_base, (unsigned long)e->ip,
		(unsigned long)e->addr, (unsigned)e->size,
		(e->rwm & 2) ? "wr" : "rd",
		(unsigned long)e->lo, (unsigned long)e->hi,
		(unsigned long)e->esp,
		(long)((long)e->addr - (long)e->esp),
		(unsigned)e->cpl, (unsigned long)e->eflags,
		(unsigned long)e->ra[0], (unsigned long)e->ra[1],
		(unsigned long)e->ra[2], (unsigned long)e->ra[3]);
	for (int i = 0; i < e->codelen && n + 4 < cap; i++)
		n += snprintf(out + n, cap - n, " %02x", e->code[i]);
	return n;
}

void gate_note_call(CPUI386 *cpu, int sel, int newcs, int gt, int wc,
		    int newdpl, uword oldsp)
{
	if (g_exc_frozen) return;
	struct GateRec *last = RING_LAST(gate_ent, gate_ent_head);
	if (last && last->sel == (uint16_t)sel && last->ip == cpu->ip &&
	    last->cs_base == cpu->seg[SEG_CS].base && last->oldsp == oldsp) {
		last->hits++;
		last->t_us = time_us_32();
		return;
	}
	struct GateRec *e = &gate_ent[gate_ent_head++ % SEG_REF_N];
	e->t_us    = time_us_32();
	e->cs_base = cpu->seg[SEG_CS].base;
	e->ip      = cpu->ip;
	e->oldsp   = oldsp;
	e->sel     = (uint16_t)sel;
	e->newcs   = (uint16_t)newcs;
	e->oldss   = (uint16_t)cpu->seg[SEG_SS].sel;
	e->gt      = (uint8_t)gt;
	e->wc      = (uint8_t)wc;
	e->cpl     = (uint8_t)cpu->cpl;
	e->newdpl  = (uint8_t)newdpl;
	e->hits    = 1;
}

int cpu_gate_line(int idx, char *out, int cap)
{
	RING_PICK(gate_ent, gate_ent_head, idx, const struct GateRec *e);
	return snprintf(out, cap,
		"%lu.%03lus x%lu gate %04x type=%u wc=%u %s params -> cs=%04x "
		"dpl %u<-cpl %u at %08lx:%08lx oldss:sp=%04x:%08lx reach=+%u",
		(unsigned long)(e->t_us / 1000000u),
		(unsigned long)(e->t_us / 1000u % 1000u),
		(unsigned long)e->hits,
		e->sel, (unsigned)e->gt, (unsigned)e->wc,
		e->gt == 4 ? "word" : "dword", e->newcs,
		(unsigned)e->newdpl, (unsigned)e->cpl,
		(unsigned long)e->cs_base, (unsigned long)e->ip,
		e->oldss, (unsigned long)e->oldsp,
		(unsigned)(e->wc ? (e->gt == 4 ? 2 : 4) * (e->wc - 1) : 0));
}

int cpu_seg_clr_line(int idx, char *out, int cap)
{
	RING_PICK(seg_clr, seg_clr_head, idx, const struct SegClrRec *e);
	return snprintf(out, cap,
		"%lu.%03lus x%lu %s was %04x fl=%04x at %08lx:%08lx cpl=%u",
		(unsigned long)(e->t_us / 1000000u),
		(unsigned long)(e->t_us / 1000u % 1000u),
		(unsigned long)e->hits,
		seg_nm[e->seg & 7], e->sel, e->flags,
		(unsigned long)e->cs_base, (unsigned long)e->ip,
		(unsigned)e->cpl);
}

int cpu_v86_entry_line(int idx, char *out, int cap)
{
	RING_PICK(v86_ent, v86_ent_head, idx, const struct V86EntRec *e);
	return snprintf(out, cap,
		"%lu.%03lus x%lu iret at %08lx:%08lx -> %04x:%08lx "
		"ss:sp=%04x:%08lx ds=%04x es=%04x fl=%08lx",
		(unsigned long)(e->t_us / 1000000u),
		(unsigned long)(e->t_us / 1000u % 1000u),
		(unsigned long)e->hits,
		(unsigned long)e->from_cs_base, (unsigned long)e->from_ip,
		e->cs, (unsigned long)e->eip,
		e->ss, (unsigned long)e->esp, e->ds, e->es,
		(unsigned long)e->eflags);
}

/*
 * Where the guest is, sampled.
 *
 * A machine that is wedged in ring 0 raises no exception at all - no page
 * fault, no trap, nothing the exception ring can see - so the only way to name
 * the loop is to look at CS:EIP now and then.  One sample every 4096
 * instructions costs a predictable branch; consecutive samples in the same
 * place are collapsed, so a tight spin shows as one line with a large count
 * and the run-up to it survives.
 */
struct PcRec {
	uint32_t t_us, cs_base, ip, eflags, hits;
	uint16_t cs, ss;
	uint8_t  cpl;
};
static struct PcRec pc_ring[SEG_REF_N];
static uint32_t pc_head;

static void pc_note(CPUI386 *cpu)
{
	if (g_exc_frozen) return;
	struct PcRec *last = RING_LAST(pc_ring, pc_head);
	if (last && last->ip == cpu->ip &&
	    last->cs_base == cpu->seg[SEG_CS].base) {
		last->hits++;
		last->t_us = time_us_32();
		return;
	}
	struct PcRec *e = &pc_ring[pc_head++ % SEG_REF_N];
	e->t_us    = time_us_32();
	e->cs_base = cpu->seg[SEG_CS].base;
	e->ip      = cpu->ip;
	e->eflags  = cpu->flags;
	e->cs      = (uint16_t)cpu->seg[SEG_CS].sel;
	e->ss      = (uint16_t)cpu->seg[SEG_SS].sel;
	e->cpl     = (uint8_t)cpu->cpl;
	e->hits    = 1;
}

int cpu_pc_line(int idx, char *out, int cap)
{
	RING_PICK(pc_ring, pc_head, idx, const struct PcRec *e);
	return snprintf(out, cap,
		"%lu.%03lus x%lu cs=%04x base=%08lx eip=%08lx ss=%04x cpl=%u%s fl=%08lx",
		(unsigned long)(e->t_us / 1000000u),
		(unsigned long)(e->t_us / 1000u % 1000u),
		(unsigned long)e->hits, e->cs,
		(unsigned long)e->cs_base, (unsigned long)e->ip, e->ss,
		(unsigned)e->cpl, (e->eflags & VM) ? " V86" : "",
		(unsigned long)e->eflags);
}

int cpu_seg_limit_report(char *out, int cap)
{
	return snprintf(out, cap,
		"%lu refused by segment bounds, %lu made unusable, "
		"%lu entries into V86",
		(unsigned long)g_seg_refused, (unsigned long)seg_clr_head,
		(unsigned long)v86_ent_head);
}

/*
 * Where control went, and from where.
 *
 * The sampler above names a loop but not the road into it, and a machine
 * that has run off into zeroed memory overwrites its whole ring on the way.
 * Win95 setup's mini-Windows did exactly that: it left protected mode and
 * ended up in real mode with SS:SP inside the vector table, executing text,
 * and nothing kept said how it got there.  So with CPU_XFER_TRACE every
 * change of the code segment's base and every flip of CR0.PE is recorded
 * with the instruction it came from; a real-mode interrupt that would push
 * onto the vector table freezes this and the other rings (see call_isr).
 */
#define XFER_N 64
struct XferRec {
	uint32_t t_us, from_base, from_ip, to_base, to_ip, esp, eflags, hits;
	uint16_t from_cs, to_cs, ss;
	uint8_t  cpl, pe;
};
static struct XferRec xfer_ring[XFER_N];
static uint32_t xfer_head;
#if defined(CPU_XFER_TRACE)
static uint32_t xfer_last_base, xfer_last_ip, xfer_last_pe;
static uint16_t xfer_last_cs;

static void xfer_note(CPUI386 *cpu)
{
	const uint32_t base = cpu->seg[SEG_CS].base;
	const uint32_t pe = cpu->cr0 & 1;
	if (!g_exc_frozen) {
		struct XferRec *last = xfer_head ? &xfer_ring[(xfer_head - 1) % XFER_N] : 0;
		if (last && last->from_base == xfer_last_base &&
		    last->from_ip == xfer_last_ip && last->to_base == base &&
		    last->to_ip == cpu->ip && last->pe == pe) {
			last->hits++;
			last->t_us = time_us_32();
		} else {
			struct XferRec *e = &xfer_ring[xfer_head++ % XFER_N];
			e->t_us      = time_us_32();
			e->from_base = xfer_last_base;
			e->from_ip   = xfer_last_ip;
			e->from_cs   = xfer_last_cs;
			e->to_base   = base;
			e->to_ip     = cpu->ip;
			e->to_cs     = (uint16_t)cpu->seg[SEG_CS].sel;
			e->ss        = (uint16_t)cpu->seg[SEG_SS].sel;
			e->esp       = cpu->gprx[4].r32;
			e->eflags    = cpu->flags;
			e->cpl       = (uint8_t)cpu->cpl;
			e->pe        = (uint8_t)pe;
			e->hits      = 1;
		}
	}
	xfer_last_base = base;
	xfer_last_pe = pe;
	xfer_last_cs = (uint16_t)cpu->seg[SEG_CS].sel;
}
#endif

int cpu_xfer_line(int idx, char *out, int cap)
{
	const uint32_t held = xfer_head < XFER_N ? xfer_head : XFER_N;
	if ((uint32_t)idx >= held) return 0;
	const uint32_t first = xfer_head < XFER_N ? 0 : xfer_head % XFER_N;
	const struct XferRec *e = &xfer_ring[(first + idx) % XFER_N];
	return snprintf(out, cap,
		"%lu.%03lus x%lu %04x:%08lx (%08lx) -> %04x:%08lx (%08lx) %s "
		"ss:esp=%04x:%08lx cpl=%u fl=%08lx",
		(unsigned long)(e->t_us / 1000000u),
		(unsigned long)(e->t_us / 1000u % 1000u),
		(unsigned long)e->hits,
		e->from_cs, (unsigned long)e->from_ip, (unsigned long)e->from_base,
		e->to_cs, (unsigned long)e->to_ip, (unsigned long)e->to_base,
		e->pe ? ((e->eflags & VM) ? "V86" : "PM") : "real",
		e->ss, (unsigned long)e->esp, (unsigned)e->cpl,
		(unsigned long)e->eflags);
}

static bool IRAM_ATTR segcheck(CPUI386 *cpu, int rwm, int seg, uword addr, int size)
{
	/*
	 * The offsets this segment accepts, and a refusal if the access is
	 * outside them.  A null selector is covered by the same test:
	 * loading one leaves lo above hi, so nothing it addresses is in
	 * range.  Real mode and virtual-8086 keep the bounds wide open, so
	 * they are unaffected and need no test of the mode here.
	 *
	 * The order matters.  Comparing size against hi - addr alone is not
	 * enough: when addr is already past hi that subtraction wraps to a
	 * huge number and the access would be let through.
	 */
	const uword lo = cpu->seg[seg].lo;
	const uword hi = cpu->seg[seg].hi;
	if (addr < lo || addr > hi || (uword)(size - 1) > hi - addr) {
		g_seg_refused++;
		seg_note_refusal(cpu, rwm, seg, addr, size);
		THROW(seg == SEG_SS ? EX_SS : EX_GP, 0);
	}
	return true;
}

/*
 * Everything translate() does not want inline: the segment-limit rejection,
 * a TLB miss and its refill, a page fault, and an access that straddles two
 * pages and therefore needs two translations.
 */
static bool __attribute__((noinline)) IRAM_ATTR
translate_out_of_line(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr,
		      int size, int cpl)
{
	uword laddr = cpu->seg[seg].base + addr;

	TRYL(segcheck(cpu, rwm, seg, addr, size));

	return translate_laddr(cpu, res, rwm, laddr, size, cpl);
}

/*
 * Address translation, with the case that actually happens kept in one
 * function.
 *
 * This used to be four nested out-of-line calls for a plain TLB hit -
 * translate() called segcheck(), then translate_laddr(), which called
 * translate_lpgno() - and every instruction with a memory operand pays for
 * all of them.  None of the four is expensive; the call sequences are, and on
 * a machine spending 252 host cycles per guest instruction they are a
 * measurable share of the whole interpreter.  `translate` plus
 * `translate_laddr` alone were 9.4% of core 0 in the DRACIHIS profile, and the
 * register spills the calls force on cpu_exec1() land in *its* 41.4%.
 *
 * So the hit path - segment check passes, paging on, TLB entry present and
 * permitted, access inside one page - is straight-line code here, and
 * everything else is one call to translate_out_of_line().  This is the same
 * shape as the instruction-fetch change that was worth 12.9%: a small inline
 * test, a noinline miss.  Crucially it does *not* inline into the ~200 call
 * sites, so it costs almost nothing in SRAM, which is 90.85% full.
 *
 * The behaviour is unchanged, including the order in which exceptions are
 * raised: a segment rejection is tested before anything paging-related, and
 * every case the fast path declines is re-done from scratch by the slow one.
 */
/*
 * Address translation, with the case that actually happens kept in one function.
 *
 * Tried and reverted: packing rwm/seg/size into one word so all four
 * arguments fit in registers, and again with the width baked into three
 * specialised copies so the folding survived.  Both were slower - 3.395 and
 * 3.471 MIPS against 3.551 on the Tyrian demo.  GCC's own constprop clones
 * were already specialising more than the width, and hand-packing takes that
 * away.  The seven-parameter signature stays.
 */
#if defined(CIRCLE_PC_XLAT)
/* What reaching an operand in guest memory costs, split between the common
 * path and the walk.  One guest instruction in three touches memory and the
 * per-opcode costs put those at two to three hundred cycles, which is more
 * than twice an average instruction; this says where it goes. */
uint64_t g_xlat_cycles, g_xlat_calls, g_xlat_slow;
#define XLAT_T0(v) asm volatile ("mrs %0, pmccntr_el0" : "=r"(v))
#endif

static bool IRAM_ATTR translate(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr, int size, int cpl)
{
#if defined(CIRCLE_PC_XLAT)
	uint64_t _x0, _x1;
	XLAT_T0(_x0);
#endif
	assert(seg != -1);
	uword laddr = cpu->seg[seg].base + addr;

	/*
	 * segcheck()'s rejection, tested here so its call disappears.
	 *
	 * Two loads and three comparisons, with no test of the processor's
	 * mode: real mode and virtual-8086 are given bounds that accept
	 * everything when their segments are loaded, so they take this same
	 * path and reach the same answer.
	 */
	if (unlikely(addr < cpu->seg[seg].lo || addr > cpu->seg[seg].hi ||
		     (uword)(size - 1) > cpu->seg[seg].hi - addr))
	{
#if defined(CIRCLE_PC_XLAT)
		XLAT_T0(_x1); g_xlat_cycles += _x1 - _x0; g_xlat_calls++; g_xlat_slow++;
#endif
		return translate_out_of_line(cpu, res, rwm, seg, addr, size, cpl);
	}

	if (likely(cpu->cr0 & CR0_PG)) {
		uword lpgno = laddr >> 12;

		struct tlb_entry *ent = &(cpu->tlb.tab[lpgno % tlb_size]);

		if (unlikely(ent->lpgno != lpgno) ||
		    unlikely(ent->deny[((cpl > 0) << 1) | (rwm > 1)]) ||
		    unlikely((laddr & 0xfff) > 0x1000 - size)) {
#if defined(CIRCLE_PC_XLAT)
			XLAT_T0(_x1); g_xlat_cycles += _x1 - _x0; g_xlat_calls++; g_xlat_slow++;
#endif
			return translate_out_of_line(cpu, res, rwm, seg, addr, size, cpl);
		}

		if (rwm & 2) {
			u8 *ppte = ent->ppte;
			if (unlikely(!(*ppte & (1 << 6))))
				*ppte |= 1 << 6;
		}
		res->res = ADDR_OK1;
		res->addr1 = ent->xaddr ^ laddr;
#if defined(CIRCLE_PC_XLAT)
		XLAT_T0(_x1); g_xlat_cycles += _x1 - _x0; g_xlat_calls++;
#endif
		return true;
	}

	res->res = ADDR_OK1;
	res->addr1 = laddr;
#if defined(CIRCLE_PC_XLAT)
	XLAT_T0(_x1); g_xlat_cycles += _x1 - _x0; g_xlat_calls++;
#endif
	return true;
}

static bool IRAM_ATTR translate8r(CPUI386 *cpu, OptAddr *res, int seg, uword addr)
{
	assert(seg != -1);
	uword laddr = cpu->seg[seg].base + addr;

	TRYL(segcheck(cpu, 1, seg, addr, 1));

	if (cpu->cr0 & CR0_PG) {
		uword lpgno = laddr >> 12;
		struct tlb_entry *ent = &(cpu->tlb.tab[lpgno % tlb_size]);
		if (ent->lpgno != lpgno) {
			if (!tlb_refill(cpu, ent, lpgno)) {
				cpu->cr2 = laddr;
				cpu->excno = EX_PF;
				cpu->excerr = 0;
				if (cpu->cpl)
					cpu->excerr |= 4;
				return false;
			}
		}
		if (ent->deny[(cpu->cpl > 0) << 1]) {
			cpu->cr2 = laddr;
			cpu->excno = EX_PF;
			cpu->excerr = 1;
			if (cpu->cpl)
				cpu->excerr |= 4;
			ent->lpgno = -1;
			return false;
		}
		res->res = ADDR_OK1;
		res->addr1 = ent->xaddr ^ laddr;
	} else {
		res->res = ADDR_OK1;
		res->addr1 = laddr;
	}

	return true;
}

static inline bool translate8(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr)
{
	return translate(cpu, res, rwm, seg, addr, 1, cpu->cpl);
}

static inline bool translate16(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr)
{
	return translate(cpu, res, rwm, seg, addr, 2, cpu->cpl);
}

static inline bool translate32(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr)
{
	return translate(cpu, res, rwm, seg, addr, 4, cpu->cpl);
}

static inline bool __attribute__((always_inline)) in_iomem(uword addr)
{
	/*
	 * Almost every DOS RAM access is below the VGA aperture.  Reject that
	 * common case with one ordered comparison; only the uncommon high address
	 * needs the second half of the aperture/device test.
	 */
	if (likely(addr < 0xa0000u))
		return false;
	return addr < 0xc0000u || addr >= 0xe0000000u;
}

static u8 IRAM_ATTR load8(CPUI386 *cpu, OptAddr *res)
{
	uword addr = res->addr1;
	if (in_iomem(addr) && cpu->cb.iomem_read8)
		return cpu->cb.iomem_read8(cpu->cb.iomem, addr);
	if (unlikely(addr >= cpu->phys_mem_size)) {
		return 0;
	}
	return pload8(cpu, addr);
}

static u16 IRAM_ATTR load16(CPUI386 *cpu, OptAddr *res)
{
	if (in_iomem(res->addr1) && cpu->cb.iomem_read16)
		return cpu->cb.iomem_read16(cpu->cb.iomem, res->addr1);
	if (unlikely(res->addr1 >= cpu->phys_mem_size)) {
		return 0;
	}
#if defined(CPU_STACK_WATCH)
	if (SW_HIT(res->addr1, 2) && res->res == ADDR_OK1) {
		const u16 v = pload16(cpu, res->addr1);
		sw_note(cpu, res->addr1, 2, v, 'R');
		return v;
	}
#endif
	if (likely(res->res == ADDR_OK1))
		return pload16(cpu, res->addr1);
	else
		return pload8(cpu, res->addr1) | (pload8(cpu, res->addr2) << 8);
}

static u32 IRAM_ATTR load32(CPUI386 *cpu, OptAddr *res)
{
	if (in_iomem(res->addr1) && cpu->cb.iomem_read32)
		return cpu->cb.iomem_read32(cpu->cb.iomem, res->addr1);
	if (unlikely(res->addr1 >= cpu->phys_mem_size)) {
		return 0;
	}
	if (likely(res->res == ADDR_OK1)) {
		return pload32(cpu, res->addr1);
	} else {
		switch(res->addr1 & 0xf) {
		case 0xf:
			return pload8(cpu, res->addr1) | (pload16(cpu, res->addr2) << 8) |
				(pload8(cpu, res->addr2 + 2) << 24);
		case 0xe:
			return pload16(cpu, res->addr1) | (pload16(cpu, res->addr2) << 16);
		case 0xd:
			return pload8(cpu, res->addr1) | (pload16(cpu, res->addr1 + 1) << 8) |
				(pload8(cpu, res->addr2) << 24);
		}
	}
	assert(false);
}

/*
 * A write that lands in the line the interpreter is fetching from.
 *
 * The prefetch buffer used to be thrown away at every branch, which is what
 * made a guest that rewrites bytes and jumps to them work - DOS
 * decompressors and overlay loaders do it constantly.  It also threw the
 * line away on every taken branch in every loop, and a loop body is usually
 * shorter than the thirty-two bytes held: measured on Tyrian, a conditional
 * jump cost 126 cycles, nearly all of it refilling what it had just
 * discarded.
 *
 * Invalidating here instead is what the branch sites were standing in for.
 * The comparison is on physical addresses because that is what a store
 * has in hand, and because two linear addresses can reach the same byte.
 */
/*
 * For a writer that reaches guest memory without going through store8().
 *
 * The DMA controller is the one that matters: a floppy read lands wherever
 * the guest asked, which can be the bytes the interpreter is fetching from.
 * Working out whether it overlapped would cost more than it saves, and a
 * transfer is rare next to a store, so it discards the line outright.
 */
void cpu_prefetch_invalidate(CPUI386 *cpu)
{
	if (cpu) cpu->prefetch_base = (u32)-1;
}

static inline __attribute__((always_inline))
void prefetch_note_write(CPUI386 *cpu, uword addr)
{
	if (unlikely(((u32)addr & ~(u32)31) == cpu->prefetch_pbase))
		cpu->prefetch_base = (u32)-1;
#ifdef TINY386_JIT
	jit_note_write((uint32_t)addr);
#endif
}

static void IRAM_ATTR store8(CPUI386 *cpu, OptAddr *res, u8 val)
{
	uword addr = res->addr1;
	if (in_iomem(addr) && cpu->cb.iomem_write8) {
		cpu->cb.iomem_write8(cpu->cb.iomem, addr, val);
		return;
	}
	if (unlikely(addr >= cpu->phys_mem_size)) {
		return;
	}
	prefetch_note_write(cpu, addr);
	pstore8(cpu, addr, val);
}

static void IRAM_ATTR store16(CPUI386 *cpu, OptAddr *res, u16 val)
{
	if (in_iomem(res->addr1) && cpu->cb.iomem_write16) {
		cpu->cb.iomem_write16(cpu->cb.iomem, res->addr1, val);
		return;
	}
	if (unlikely(res->addr1 >= cpu->phys_mem_size)) {
		return;
	}
	prefetch_note_write(cpu, res->addr1);
	prefetch_note_write(cpu, res->addr1 + 1);
	if (likely(res->res == ADDR_OK1)) {
		pstore16(cpu, res->addr1, val);
	} else {
		pstore8(cpu, res->addr1, val);
		pstore8(cpu, res->addr2, val >> 8);
	}
}

static void IRAM_ATTR store32(CPUI386 *cpu, OptAddr *res, u32 val)
{
	if (in_iomem(res->addr1) && cpu->cb.iomem_write32) {
		cpu->cb.iomem_write32(cpu->cb.iomem, res->addr1, val);
		return;
	}
	if (unlikely(res->addr1 >= cpu->phys_mem_size)) {
		return;
	}
#if defined(CPU_STACK_WATCH)
	/* The MOV that fills DetectESDI's port pair arms the watch. */
	if (unlikely(val == 0x017001f0u) && res->res == ADDR_OK1)
		sw_phys = res->addr1, sw_note(cpu, res->addr1, 4, val, 'A');
#endif
	prefetch_note_write(cpu, res->addr1);
	prefetch_note_write(cpu, res->addr1 + 3);
	if (likely(res->res == ADDR_OK1)) {
		pstore32(cpu, res->addr1, val);
	} else {
		switch(res->addr1 & 0xf) {
		case 0xf:
			pstore8(cpu, res->addr1, val);
			pstore16(cpu, res->addr2, val >> 8);
			pstore8(cpu, res->addr2 + 2, val >> 24);
			break;
		case 0xe:
			pstore16(cpu, res->addr1, val);
			pstore16(cpu, res->addr2, val >> 16);
			break;
		case 0xd:
			pstore8(cpu, res->addr1, val);
			pstore16(cpu, res->addr1 + 1, val >> 8);
			pstore8(cpu, res->addr2, val >> 24);
			break;
		}
	}
}

#define LOADSTORE(BIT) \
bool cpu_load ## BIT(CPUI386 *cpu, int seg, uword addr, u ## BIT *res) \
{ \
	OptAddr o; \
	TRY(translate ## BIT(cpu, &o, 1, seg, addr)); \
	*res = load ## BIT(cpu, &o); \
	return true; \
} \
\
bool cpu_store ## BIT(CPUI386 *cpu, int seg, uword addr, u ## BIT val) \
{ \
	OptAddr o; \
	TRY(translate ## BIT(cpu, &o, 2, seg, addr)); \
	store ## BIT(cpu, &o, val); \
	return true; \
} \

LOADSTORE(8)
LOADSTORE(16)
LOADSTORE(32)

/*
 * Instruction prefetch buffer: 32 bytes loaded as eight 32-bit reads from a
 * 32-byte-aligned physical address.  cpu->prefetch_base holds that physical base
 * address (always a multiple of 32), or (u32)-1 when invalid.
 *
 * The width is the XIP cache's line size deliberately.  A 16-byte fill took
 * one PSRAM miss and used half of what that miss brought in, so the next
 * refill sixteen bytes later was free anyway - the cost was the refill path
 * itself, and peek8_slow() was 8.9% of core 0 in the DRACIHIS profile.  A
 * 32-byte line never straddles a 4 KB page either, since 4096 divides evenly
 * by 32, so nothing about the paging checks changes.
 *
 * Invalidation is implicit: any jump/call/ret changes next_ip so that the
 * resulting paddr falls outside [cpu->prefetch_base, cpu->prefetch_base+16), causing
 * an automatic refill on the very next fetch.  No explicit flush is needed
 * at branch sites.
 *
 * cpu->prefetch[] is aligned to 4 bytes so the four pload32 calls are natural.
 */

/* Refill: load 32 bytes (8 x u32) from the 32-byte-aligned block that
 * contains paddr.  Caller guarantees paddr is in plain RAM and within the
 * current ifetch page. */
static inline void __attribute__((always_inline))
prefetch_fill(CPUI386 *cpu, uword laddr, uword paddr)
{
	u32 pbase = paddr & ~(u32)31;

	/*
	 * Tag the 32-byte instruction-prefetch line by linear address.
	 * Paging preserves the low 12 address bits, so laddr and paddr have
	 * the same offset within a 32-byte line.  The physical address is
	 * still used for the actual PSRAM reads below.
	 *
	 * This removes the ifetch page test + physical-address XOR from the
	 * FAST_FETCH hit path, which is one of the hottest paths on Z2.
	 */
	cpu->prefetch_base = laddr & ~(u32)31;
	cpu->prefetch_pbase = pbase;

	u32* prefetch = (u32*)cpu->prefetch;
	*prefetch++ = pload32(cpu, pbase);
	*prefetch++ = pload32(cpu, pbase + 4);
	*prefetch++ = pload32(cpu, pbase + 8);
	*prefetch++ = pload32(cpu, pbase + 12);
	*prefetch++ = pload32(cpu, pbase + 16);
	*prefetch++ = pload32(cpu, pbase + 20);
	*prefetch++ = pload32(cpu, pbase + 24);
	*prefetch++ = pload32(cpu, pbase + 28);
}

/* True if laddr is covered by the current prefetch buffer. */
/*
 * Every control transfer invalidates the prefetch buffer, and it has to.
 *
 * The tag is a linear address and the test below is exact, so it looks as
 * though a near branch could keep the line: the target either falls inside
 * the thirty-two bytes or misses the tag.  Removing the invalidation from
 * the twenty-one near Jcc/JMP/LOOP/CALL/RET sites was tried, and it wedged
 * the board so hard that the debug port could no longer examine either core
 * - it booted once and hung on the next reset.
 *
 * The reason is that nothing else invalidates this buffer on a write.  The
 * store path does not touch it; only tlb_clear() and the fetch paths do.  So
 * the invalidation at each branch is what makes self-modifying code work -
 * a guest that rewrites bytes and then jumps to them, which DOS
 * decompressors and overlay loaders do constantly - and it is load-bearing
 * rather than conservative.  Anyone who wants those refills back has to
 * invalidate on stores to the fetch page first.
 */
#define PREFETCH_HIT(laddr) \
	(likely(((uword)(laddr) - cpu->prefetch_base) < 32u))

/*
 * Instruction fetch is 21% of core-0 time, and peek8 was being called
 * out of line from 473 sites — one call, prologue and return per
 * instruction *byte*. IRAM_ATTR is , an explicit
 * section attribute, and GCC will not inline a function that carries
 * one, so the `static` here was never enough.
 *
 * The split below inlines only the hit path: two compares and a byte
 * load, ~20 bytes per site. Everything else — prefetch refill, page
 * miss, the full TLB walk — stays out of line.
 */
static bool __attribute__((noinline)) IRAM_ATTR
peek8_miss(CPUI386 *cpu, u8 *val);

static bool IRAM_ATTR peek8_slow(CPUI386 *cpu, u8 *val)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;

	/*
	 * The prefetch line is tagged by linear address, so a hit is valid without
	 * consulting the cached translation first.  Test it before ifetch.laddr:
	 * this is the common path and avoids a page-tag load/XOR/compare for every
	 * interpreted instruction byte.  FAST_FETCH already relies on the same
	 * invariant; keeping it here gives Z2 most of that hot-path win without
	 * duplicating the test at hundreds of call sites.
	 */
	if (PREFETCH_HIT(laddr)) {
		*val = cpu->prefetch[laddr & 31];
		return true;
	}

	/* Keep the overwhelmingly common hit path small; the noinline miss helper
	 * also prevents its large register-save set from infecting every hit. */
	return peek8_miss(cpu, val);
}

static bool __attribute__((noinline)) IRAM_ATTR
peek8_miss(CPUI386 *cpu, u8 *val)
{
	/* One recomputation per 32-byte refill is cheaper than carrying laddr into
	 * the helper and forcing another callee-saved register on every hit. */
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;

	if (likely((laddr ^ cpu->ifetch.laddr) < 4096)) {
		uword paddr = cpu->ifetch.xaddr ^ laddr;
		prefetch_fill(cpu, laddr, paddr);
		*val = cpu->prefetch[laddr & 31];
		return true;
	}
	/* ifetch page miss: full TLB translate */
	OptAddr res;
	TRY(translate8r(cpu, &res, SEG_CS, cpu->next_ip));
	cpu->ifetch.laddr = laddr & (~4095ul);
	cpu->ifetch.xaddr = res.addr1 ^ laddr;
	if (!in_iomem(res.addr1) && res.addr1 + 31 < cpu->phys_mem_size) {
		prefetch_fill(cpu, laddr, res.addr1);
		*val = cpu->prefetch[laddr & 31];
	} else {
		cpu->prefetch_base = (u32)-1;
		*val = load8(cpu, &res);
	}
	return true;
}

/*
 * Gated because it costs ~12 KB of RAM.
 *
 * The win is board-independent, but master SRAM is not: C2 sits at 88.9%
 * with it and the emulator stops booting around 91% when pc_new() can no
 * longer allocate. M1/M2/PC/Z2 were at 88.3% before this change and 90.7%
 * after — still building, but close enough to a threshold I have actually
 * hit that enabling it on boards I cannot boot-test would be careless.
 *
 * Leave it off here.  It looks like free RAM on a board with most of a
 * gigabyte, but peek8_slow() below already tests the prefetch line itself -
 * see its comment - so switching this on only duplicates that test at every
 * one of the several hundred fetch sites, and stops the compiler inlining
 * the small leaf where it judges that worthwhile.  Measured on Tyrian over
 * matching windows of the demo: 74.4 million guest instructions a second
 * became 63.6 million, and host instructions per guest instruction rose by a
 * third.  The gate is not a leftover from the small board; it is superseded.
 */
#if FAST_FETCH
static inline __attribute__((always_inline))
bool peek8(CPUI386 *cpu, u8 *val)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;
	if (likely(PREFETCH_HIT(laddr))) {
		*val = cpu->prefetch[laddr & 31];
		return true;
	}
	return peek8_slow(cpu, val);
}
#else
#define peek8 peek8_slow
#endif

#ifdef TINY386_JIT
/*
 * Instruction bytes for the translator, from the prefetch line only.
 *
 * Translation must read exactly what execution would - same segment, same
 * page - or a block gets built from bytes the guest cannot see.  Going
 * through the full walk would mean handling a page fault raised while
 * compiling, which is a fault the guest never took; refusing anything
 * outside the line already loaded avoids the question entirely, at the
 * price of blocks no longer than the thirty-two bytes held.
 */
bool jit_peek_byte(CPUI386 *cpu, uint32_t off, u8 *val)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip + off;
	if (!PREFETCH_HIT(laddr))
		return false;
	*val = cpu->prefetch[laddr & 31];
	return true;
}

bool jit_phys_of(CPUI386 *cpu, uint32_t off, uint32_t *phys)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip + off;
	if (!PREFETCH_HIT(laddr))
		return false;
	*phys = cpu->prefetch_pbase | (uint32_t)(laddr & 31);
	return true;
}
#endif

static bool IRAM_ATTR fetch8(CPUI386 *cpu, u8 *val)
{
	TRY(peek8(cpu, val));
	cpu->next_ip++;
	return true;
}

static bool IRAM_ATTR fetch16(CPUI386 *cpu, u16 *val)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;
	if (likely((laddr ^ cpu->ifetch.laddr) < 4095)) {
		uword paddr = cpu->ifetch.xaddr ^ laddr;
		if (!PREFETCH_HIT(laddr))
			prefetch_fill(cpu, laddr, paddr);
		unsigned off = laddr & 31;
		if (likely(off <= 30)) {
			/* Both bytes inside the buffer */
			*val = cpu->prefetch[off] | ((u16)cpu->prefetch[off + 1] << 8);
		} else {
			/* Byte 1 is last byte of current block; byte 0 already in buffer.
			 * Read second byte via pload (still within ifetch page). */
			u8 lo = cpu->prefetch[31];
			u8 hi = pload8(cpu, paddr + 1);
			*val = lo | ((u16)hi << 8);
		}
	} else {
		OptAddr res;
		TRY(translate16(cpu, &res, 1, SEG_CS, cpu->next_ip));
		*val = load16(cpu, &res);
		cpu->prefetch_base = (u32)-1;
	}
	cpu->next_ip += 2;
	return true;
}

static bool IRAM_ATTR fetch32(CPUI386 *cpu, u32 *val)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;
	if (likely((laddr ^ cpu->ifetch.laddr) < 4093)) {
		uword paddr = cpu->ifetch.xaddr ^ laddr;
		if (!PREFETCH_HIT(laddr))
			prefetch_fill(cpu, laddr, paddr);
		unsigned off = laddr & 31;
		if (likely(off <= 28)) {
			/* All 4 bytes inside the buffer */
			*val = cpu->prefetch[off]
			     | ((u32)cpu->prefetch[off + 1] << 8)
			     | ((u32)cpu->prefetch[off + 2] << 16)
			     | ((u32)cpu->prefetch[off + 3] << 24);
		} else {
			/* Spans two prefetch lines: read the remaining bytes
			 * directly and let the next fetch refill. */
			*val = pload32(cpu, paddr);
			cpu->prefetch_base = (u32)-1;
		}
	} else {
		OptAddr res;
		TRY(translate32(cpu, &res, 1, SEG_CS, cpu->next_ip));
		*val = load32(cpu, &res);
		cpu->prefetch_base = (u32)-1;
	}
	cpu->next_ip += 4;
	return true;
}

/* insts decode && execute */
static inline bool modsib32(CPUI386 *cpu, int mod, int rm, uword *addr, int *seg)
{
	if (rm == 4) {
		u8 sib;
		TRY(fetch8(cpu, &sib));
		int b = sib & 7;
		if (b == 5 && mod == 0) {
			TRY(fetch32(cpu, addr));
		} else {
			*addr = REGi(b);
			// sp bp as base register
			if ((b == 4 || b == 5) && *seg == -1)
				*seg = SEG_SS;
		}
		int i = (sib >> 3) & 7;
		if (i != 4)
			*addr += REGi(i) << (sib >> 6);
	} else if (rm == 5 && mod == 0) {
		TRY(fetch32(cpu, addr));
	} else {
		*addr = REGi(rm);
		// bp as base register
		if (rm == 5 && *seg == -1)
			*seg = SEG_SS;
	}
	if (mod == 1) {
		u8 imm8;
		TRY(fetch8(cpu, &imm8));
		*addr += (s8) imm8;
	} else if (mod == 2) {
		u32 imm32;
		TRY(fetch32(cpu, &imm32));
		*addr += (s32) imm32;
	}
	if (*seg == -1)
		*seg = SEG_DS;
	return true;
}

static inline bool modsib16(CPUI386 *cpu, int mod, int rm, uword *addr, int *seg)
{
	if (rm == 6 && mod == 0) {
		u16 imm16;
		TRY(fetch16(cpu, &imm16));
		*addr = imm16;
	} else {
		switch(rm) {
		case 0: *addr = REGi(3) + REGi(6); break;
		case 1: *addr = REGi(3) + REGi(7); break;
		case 2: *addr = REGi(5) + REGi(6); break;
		case 3: *addr = REGi(5) + REGi(7); break;
		case 4: *addr = REGi(6); break;
		case 5: *addr = REGi(7); break;
		case 6: *addr = REGi(5); break;
		case 7: *addr = REGi(3); break;
		}
		if (mod == 1) {
			u8 imm8;
			TRY(fetch8(cpu, &imm8));
			*addr += (s8) imm8;
		} else if (mod == 2) {
			u16 imm16;
			TRY(fetch16(cpu, &imm16));
			*addr += imm16;
		}
		*addr &= 0xffff;
	}
	if (*seg == -1) {
		if (rm == 2 || rm == 3)
			*seg = SEG_SS;
		else if (mod != 0 && rm == 6)
			*seg = SEG_SS;
		else
			*seg = SEG_DS;
	}
	return true;
}

static bool IRAM_ATTR modsib(CPUI386 *cpu, int adsz16, int mod, int rm, uword *addr, int *seg)
{
	if (adsz16) return modsib16(cpu, mod, rm, addr, seg);
	else return modsib32(cpu, mod, rm, addr, seg);
}

static bool read_desc(CPUI386 *cpu, int sel, uword *w1, uword *w2)
{
	OptAddr meml;
	sel = sel & 0xffff;
	uword off = sel & ~0x7;
	uword base;
	uword limit;
	if (sel & 0x4) {
		base = cpu->seg[SEG_LDT].base;
		limit = cpu->seg[SEG_LDT].limit;
	} else {
		base = cpu->gdt.base;
		limit = cpu->gdt.limit;
	}

	if (off + 7 > limit) {
		dolog("read_desc: sel %04x base %x limit %x off %x\n", sel, base, limit, off);
		THROW(EX_GP, sel & ~0x3);
	}
	if (w1) {
		TRY(translate_laddr(cpu, &meml, 1, base + off, 4, 0));
		*w1 = load32(cpu, &meml);
	}
	TRY(translate_laddr(cpu, &meml, 1, base + off + 4, 4, 0));
	*w2 = load32(cpu, &meml);
	return true;
}

/*
 * The offsets a loaded segment will accept.  See the note in i386.h.
 *
 * Called from every place that writes seg[].limit, so the pair can never be
 * left behind by a path that sets the limit and forgets the rest; a stale
 * pair would refuse accesses the guest is entitled to make.
 */
static void seg_set_bounds(CPUI386 *cpu, int seg)
{
	const uword flags = cpu->seg[seg].flags;
	const uword limit = cpu->seg[seg].limit;
	/* A system segment (S clear) is not reached through the data paths;
	 * leave it open so nothing it is used for starts faulting. */
	const int is_data = (flags & 0x10) && !(flags & 0x08);
	if (is_data && (flags & 0x04)) {
		cpu->seg[seg].lo = limit + 1;
		cpu->seg[seg].hi = (flags & SEG_B_BIT) ? 0xffffffffu : 0xffffu;
	} else if (flags & 0x10) {
		cpu->seg[seg].lo = 0;
		cpu->seg[seg].hi = limit;
	} else {
		cpu->seg[seg].lo = 0;
		cpu->seg[seg].hi = 0xffffffffu;
	}
}

static bool set_seg(CPUI386 *cpu, int seg, int sel)
{
	if (seg == SEG_CS) {
		/* The stack is read straight out of physical memory: every guest
		 * that gets here runs the low megabyte identity-mapped, and a
		 * diagnostic read must never fault or walk page tables itself. */
		uint32_t ssp = cpu->seg[SEG_SS].base +
			(REGi(4) & (uint32_t)cpu->sp_mask);
		const uint8_t *stk = (ssp + 8 <= (uint32_t)cpu->phys_mem_size) ?
			cpu->phys_mem + ssp : 0;
		frank_diag_cs(cpu->seg[SEG_CS].base, cpu->ip, (uint32_t)sel,
			      ssp, (uint32_t)cpu->flags, stk);
	}
	sel = sel & 0xffff;
	if (!(cpu->cr0 & 1) || (cpu->flags & VM)) {
		cpu->seg[seg].sel = sel;
		cpu->seg[seg].base = sel << 4;
		cpu->seg[seg].limit = 0xffff;
		cpu->seg[seg].flags = 0; // D_BIT is not set
		/* Real mode and virtual-8086 are left unchecked, as before. */
		cpu->seg[seg].lo = 0;
		cpu->seg[seg].hi = 0xffffffffu;
		if (seg == SEG_CS) {
			cpu->cpl = cpu->flags & VM ? 3 : 0;
			cpu->code16 = true;
		}
		if (seg == SEG_SS) {
			cpu->sp_mask = 0xffff;
		}
		return true;
	}

	/* Protected mode */
	if ((sel & ~0x3) == 0) {
		switch(seg) {
		case SEG_DS:
		case SEG_ES:
		case SEG_FS:
		case SEG_GS:
			/* Null selector is allowed; mark segment unusable. */
			cpu->seg[seg].sel = sel;
			cpu->seg[seg].base = 0;
			cpu->seg[seg].limit = 0;
			cpu->seg[seg].flags = 0;
			cpu->seg[seg].lo = 1;      /* above hi: nothing is valid */
			cpu->seg[seg].hi = 0;
			return true;
		case SEG_LDT:
			/* LLDT with null selector invalidates LDTR. */
			cpu->seg[seg].sel = 0;
			cpu->seg[seg].base = 0;
			cpu->seg[seg].limit = 0;
			cpu->seg[seg].flags = 0;
			cpu->seg[seg].lo = 0;
			cpu->seg[seg].hi = 0xffffffffu;
			return true;
		case SEG_SS:
		case SEG_CS:
		case SEG_TR:
			THROW(EX_GP, 0);
		default:
			THROW(EX_GP, 0);
		}
	}

	uword w1, w2;
	TRY(read_desc(cpu, sel, &w1, &w2));

	// TODO: various permission checks
	bool s = (w2 >> 12) & 1;
	bool p = (w2 >> 15) & 1;
	if (sel & ~0x3) {
		switch(seg) {
		case SEG_DS: case SEG_ES: case SEG_FS: case SEG_GS:
			if (!s) {
				THROW(EX_GP, sel & ~0x3);
			}
		}
		if (!p) THROW((seg == SEG_SS ? EX_SS : EX_NP), sel & ~0x3);
	}

	cpu->seg[seg].sel = sel;
	cpu->seg[seg].base = (w1 >> 16) | ((w2 & 0xff) << 16) | (w2 & 0xff000000);
	cpu->seg[seg].limit = (w2 & 0xf0000) | (w1 & 0xffff);
	if (w2 & 0x00800000)
		cpu->seg[seg].limit = (cpu->seg[seg].limit << 12) | 0xfff;
	cpu->seg[seg].flags = (w2 >> 8) & 0xffff;
	seg_set_bounds(cpu, seg);
	if (seg == SEG_CS) {
		cpu->cpl = sel & 3;
		cpu->code16 = !(cpu->seg[SEG_CS].flags & SEG_D_BIT);
	}
	if (seg == SEG_SS) {
		cpu->sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
	}
	return true;
}

void seg_note_cleared(CPUI386 *cpu, int seg);

static inline void clear_segs(CPUI386 *cpu)
{
	int segs[] = { SEG_DS, SEG_ES, SEG_FS, SEG_GS };
	for (int i = 0; i < 4; i++) {
		uword w2 = cpu->seg[segs[i]].flags << 8;
		bool is_dataseg = !((w2 >> 11) & 1);
		int dpl = (w2 >> 13) & 0x3;
		bool conforming = (w2 >> 8) & 0x4;
		if (is_dataseg || !conforming) {
			if (dpl < cpu->cpl) {
				if (cpu->seg[segs[i]].sel & ~0x3)
					seg_note_cleared(cpu, segs[i]);
				cpu->seg[segs[i]].sel = 0;
				cpu->seg[segs[i]].base = 0;
				cpu->seg[segs[i]].limit = 0;
				cpu->seg[segs[i]].lo = 1;
				cpu->seg[segs[i]].hi = 0;
				cpu->seg[segs[i]].flags = 0;
			}
		}
	}
}

/*
 * addressing modes
 */
#define _(rwm, inst) inst()

#define E_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(rm, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, laddr ## BIT, saddr ## BIT) \
	}

#define Eb(...) E_helper(8, , __VA_ARGS__)
#define Ev(...) if (opsz16) { E_helper(16, w, __VA_ARGS__) } else { E_helper(32, d, __VA_ARGS__) }

#define EG_helper(PM, BT, BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (PM && (!(cpu->cr0 & 1) || (cpu->flags & VM))) THROW0(EX_UD); \
	if (mod == 3) { \
		INST ## SUFFIX(rm, reg, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		if (BT) addr += lreg ## BIT(reg) / BIT * (BIT / 8); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, reg, laddr ## BIT, saddr ## BIT, lreg ## BIT, sreg ## BIT) \
	}

#define EbGb(...) EG_helper(false, false, 8, , __VA_ARGS__)
#define EwGw(...) EG_helper(false, false, 16, , __VA_ARGS__)
#define PMEwGw(...) EG_helper(true, false, 16, , __VA_ARGS__)
#define EvGv(...) if (opsz16) { EG_helper(false, false, 16, w, __VA_ARGS__) } else { EG_helper(false, false, 32, d, __VA_ARGS__) }
#define BTEvGv(...) if (opsz16) { EG_helper(false, true, 16, w, __VA_ARGS__) } else { EG_helper(false, true, 32, d, __VA_ARGS__) }

#define EGIb_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	u8 imm8; \
	if (mod == 3) { \
		TRY(fetch8(cpu, &imm8)); \
		INST ## SUFFIX(rm, reg, imm8, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT, limm, 0) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(fetch8(cpu, &imm8)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, reg, imm8, laddr ## BIT, saddr ## BIT, lreg ## BIT, sreg ## BIT, limm, 0) \
	}

#define EvGvIb(...) if (opsz16) { EGIb_helper(16, w, __VA_ARGS__) } else { EGIb_helper(32, d, __VA_ARGS__) }

#define EGCL_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(rm, reg, 1, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT, lreg8, sreg8) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, reg, 1, laddr ## BIT, saddr ## BIT, lreg ## BIT, sreg ## BIT, lreg8, sreg8) \
	}

#define EvGvCL(...) if (opsz16) { EGCL_helper(16, w, __VA_ARGS__) } else { EGCL_helper(32, d, __VA_ARGS__) }

#define EI_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	u ## BIT imm ## BIT; \
	if (mod == 3) { \
		TRY(fetch ## BIT(cpu, &imm ## BIT)); \
		INST ## SUFFIX(rm, imm ## BIT, lreg ## BIT, sreg ## BIT, limm, 0) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(fetch ## BIT(cpu, &imm ## BIT)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, imm ## BIT, laddr ## BIT, saddr ## BIT, limm, 0) \
	}

#define EbIb(...) EI_helper(8, , __VA_ARGS__)
#define EvIv(...) if (opsz16) { EI_helper(16, w, __VA_ARGS__) } else { EI_helper(32, d, __VA_ARGS__) }

#define EIb_helper(BT, BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	u8 imm8; \
	u ## BIT imm ## BIT; \
	if (mod == 3) { \
		TRY(fetch8(cpu, &imm8)); \
		imm ## BIT = (s ## BIT) ((s8) imm8); \
		INST ## SUFFIX(rm, imm ## BIT, lreg ## BIT, sreg ## BIT, limm, 0) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(fetch8(cpu, &imm8)); \
		imm ## BIT = (s ## BIT) ((s8) imm8); \
		if (BT) addr += imm ## BIT / BIT * (BIT / 8); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, imm ## BIT, laddr ## BIT, saddr ## BIT, limm, 0) \
	}

#define EvIb(...) if (opsz16) { EIb_helper(false, 16, w, __VA_ARGS__) } else { EIb_helper(false, 32, d, __VA_ARGS__) }
#define BTEvIb(...) if (opsz16) { EIb_helper(true, 16, w, __VA_ARGS__) } else { EIb_helper(true, 32, d, __VA_ARGS__) }

#define E1_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(rm, 1, lreg ## BIT, sreg ## BIT, limm, 0) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, 1, laddr ## BIT, saddr ## BIT, limm, 0) \
	}

#define Eb1(...) E1_helper(8, , __VA_ARGS__)
#define Ev1(...) if (opsz16) { E1_helper(16, w, __VA_ARGS__) } else { E1_helper(32, d, __VA_ARGS__) }

#define ECL_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(rm, 1, lreg ## BIT, sreg ## BIT, lreg8, sreg8) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, 1, laddr ## BIT, saddr ## BIT, lreg8, sreg8) \
	}

#define EbCL(...) ECL_helper(8, , __VA_ARGS__)
#define EvCL(...) if (opsz16) { ECL_helper(16, w, __VA_ARGS__) } else { ECL_helper(32, d, __VA_ARGS__) }

#define GE_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(reg, rm, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(reg, &meml, lreg ## BIT, sreg ## BIT, laddr ## BIT, saddr ## BIT) \
	}

#define GbEb(...) GE_helper(8, , __VA_ARGS__)
#define GvEv(...) if (opsz16) { GE_helper(16, w, __VA_ARGS__) } else { GE_helper(32, d, __VA_ARGS__) }

#define GvM_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(reg, rm, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		INST ## SUFFIX(reg, addr, lreg ## BIT, sreg ## BIT, limm, 0) \
	}
#define GvM(...) if (opsz16) { GvM_helper(16, w, __VA_ARGS__) } else { GvM_helper(32, d, __VA_ARGS__) }

#define GvMp_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) THROW0(EX_UD); \
	else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		INST ## SUFFIX(reg, addr, lreg ## BIT, sreg ## BIT, limm, 0) \
	}
#define GvMp(...) if (opsz16) { GvMp_helper(16, w, __VA_ARGS__) } else { GvMp_helper(32, d, __VA_ARGS__) }

#define GE_helper2(BIT, SUFFIX, BIT2, SUFFIX2, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX ## SUFFIX2(reg, rm, lreg ## BIT, sreg ## BIT, lreg ## BIT2, sreg ## BIT2) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT2(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX ## SUFFIX2(reg, &meml, lreg ## BIT, sreg ## BIT, laddr ## BIT2, saddr ## BIT2) \
	}

#define GvEb(...) if (opsz16) { GE_helper2(16, w, 8, b, __VA_ARGS__) } else { GE_helper2(32, d, 8, b, __VA_ARGS__) }
#define GvEw(...) if (opsz16) { GE_helper2(16, w, 16, w, __VA_ARGS__) } else { GE_helper2(32, d, 16, w, __VA_ARGS__) }

#define GEI_helperI2(BIT, SUFFIX, BIT2, SUFFIX2, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		u ## BIT2 imm ## BIT2; \
		TRY(fetch ## BIT2(cpu, &imm ## BIT2)); \
		INST ## SUFFIX ## I ## SUFFIX2(reg, rm, imm ## BIT2, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		u ## BIT2 imm ## BIT2; \
		TRY(fetch ## BIT2(cpu, &imm ## BIT2)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX ## I ## SUFFIX2(reg, &meml, imm ## BIT2, lreg ## BIT, sreg ## BIT, laddr ## BIT, saddr ## BIT) \
	}

#define GvEvIb(...) if (opsz16) { GEI_helperI2(16, w, 8, b, __VA_ARGS__) } else { GEI_helperI2(32, d, 8, b, __VA_ARGS__) }
#define GvEvIv(...) if (opsz16) { GEI_helperI2(16, w, 16, w, __VA_ARGS__) } else { GEI_helperI2(32, d, 32, d, __VA_ARGS__) }

#define ALIb(rwm, INST) \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST(0, imm8, lreg8, sreg8, limm, 0)

#define AXIb(rwm, INST) \
	if (opsz16) { \
		u8 imm8; \
		TRY(fetch8(cpu, &imm8)); \
		INST ## w(0, imm8, lreg16, sreg16, limm, 0) \
	} else { \
		u8 imm8; \
		TRY(fetch8(cpu, &imm8)); \
		INST ## d(0, imm8, lreg32, sreg32, limm, 0) \
	}

#define IbAL(rwm, INST) \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST(imm8, 0, limm, 0, lreg8, sreg8)

#define IbAX(rwm, INST) \
	if (opsz16) { \
		u8 imm8; \
		TRY(fetch8(cpu, &imm8)); \
		INST ## w(imm8, 0, limm, 0, lreg16, sreg16) \
	} else { \
		u8 imm8; \
		TRY(fetch8(cpu, &imm8)); \
		INST ## d(imm8, 0, limm, 0, lreg32, sreg32) \
	}

#define DXAL(rwm, INST) \
	INST(2, 0, lreg16, sreg16, lreg8, sreg8)

#define DXAX(rwm, INST) \
	if (opsz16) { \
		INST ## w(2, 0, lreg16, sreg16, lreg16, sreg16) \
	} else { \
		INST ## d(2, 0, lreg16, sreg16, lreg32, sreg32) \
	}

#define ALDX(rwm, INST) \
	INST(0, 2, lreg8, sreg8, lreg16, sreg16)

#define AXDX(rwm, INST) \
	if (opsz16) { \
		INST ## w(0, 2, lreg16, sreg16, lreg16, sreg16) \
	} else { \
		INST ## d(0, 2, lreg32, sreg32, lreg16, sreg16) \
	}

#define AXIv(rwm, INST) \
	if (opsz16) { \
		u16 imm16; \
		TRY(fetch16(cpu, &imm16)); \
		INST ## w(0, imm16, lreg16, sreg16, limm, 0) \
	} else { \
		u32 imm32; \
		TRY(fetch32(cpu, &imm32)); \
		INST ## d(0, imm32, lreg32, sreg32, limm, 0) \
	}

#define ALOb(rwm, INST) \
	if (adsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	TRY(translate8(cpu, &meml, rwm, curr_seg, addr)); \
	INST(0, &meml, lreg8, sreg8, laddr8, saddr8)

#define AXOv(rwm, INST) \
	if (adsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	if (opsz16) { \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## w(0, &meml, lreg16, sreg16, laddr16, saddr16) \
	} else { \
		TRY(translate32(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## d(0, &meml, lreg32, sreg32, laddr32, saddr32) \
	}

#define ObAL(rwm, INST) \
	if (adsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	TRY(translate8(cpu, &meml, rwm, curr_seg, addr)); \
	INST(&meml, 0, laddr8, saddr8, lreg8, sreg8)

#define OvAX(rwm, INST) \
	if (adsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	if (opsz16) { \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## w(&meml, 0, laddr16, saddr16, lreg16, sreg16) \
	} else { \
		TRY(translate32(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## d(&meml, 0, laddr32, saddr32, lreg32, sreg32) \
	}

#define PlusRegv(rwm, INST) \
	if (opsz16) { \
		INST ## w((b1 & 7), lreg16, sreg16) \
	} else { \
		INST ## d((b1 & 7), lreg32, sreg32) \
	}

#define PlusRegIb(rwm, INST) \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST((b1 & 7), imm8, lreg8, sreg8, limm, 0)

#define PlusRegIv(rwm, INST) \
	if (opsz16) { \
		u16 imm16; \
		TRY(fetch16(cpu, &imm16)); \
		INST ## w((b1 & 7), imm16, lreg16, sreg16, limm, 0) \
	} else { \
		u32 imm32; \
		TRY(fetch32(cpu, &imm32)); \
		INST ## d((b1 & 7), imm32, lreg32, sreg32, limm, 0) \
	}

#define Ib(rwm, INST) \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST(imm8, limm, 0)

/*
 * A conditional jump carries its condition in the third field.
 *
 * Every other instruction uses that field for its read/write mode, and
 * the jumps had no use for it - so it carries the condition code here,
 * which is the whole point: see COND_CC() for what that saves.
 */
#define Jb(cc, INST) \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST(imm8, limm, cc)

#define Iw(rwm, INST) \
	u16 imm16; \
	TRY(fetch16(cpu, &imm16)); \
	INST(imm16, limm, 0)

#define IwIb(rwm, INST) \
	u16 imm16; \
	TRY(fetch16(cpu, &imm16)); \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST(imm16, imm8, limm, 0, limm, 0)

#define Iv(rwm, INST) \
	if (opsz16) { \
		u16 imm16; \
		TRY(fetch16(cpu, &imm16)); \
		INST ## w(imm16, limm, 0) \
	} else { \
		u32 imm32; \
		TRY(fetch32(cpu, &imm32)); \
		INST ## d(imm32, limm, 0) \
	}

#define Jv(cc, INST) \
	if (adsz16) { \
		u16 imm16; \
		TRY(fetch16(cpu, &imm16)); \
		INST ## w(imm16, limm, cc); \
	} else { \
		u32 imm32; \
		TRY(fetch32(cpu, &imm32)); \
		INST ## d(imm32, limm, cc); \
	}
#define Av Iv

#define Ap(rwm, INST) \
	u16 seg; \
	if (opsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	TRY(fetch16(cpu, &seg)); \
	INST(addr, seg)

#define Ep(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) THROW0(EX_UD); \
	else { \
		u16 seg; \
		u32 off; \
		OptAddr moff, mseg; \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		if (opsz16) { \
			TRY(translate16(cpu, &moff, rwm, curr_seg, addr)); \
			TRY(translate16(cpu, &mseg, rwm, curr_seg, addr + 2)); \
			off = laddr16(&moff); \
			seg = laddr16(&mseg); \
		} else { \
			TRY(translate32(cpu, &moff, rwm, curr_seg, addr)); \
			TRY(translate16(cpu, &mseg, rwm, curr_seg, addr + 4)); \
			off = laddr32(&moff); \
			seg = laddr16(&mseg); \
		} \
		INST(off, seg) \
	}

#define Ms(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) THROW0(EX_UD); \
	else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		INST(addr) \
	}

#define Ew(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		if (opsz16) { \
			INST(rm, lreg16, sreg16) \
		} else { \
			INST(rm, lreg32, sreg32) \
		} \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST(&meml, laddr16, saddr16) \
	}

#define EwSw(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		if (opsz16) { \
			INST(rm, reg, lreg16, sreg16, lseg, 0) \
		} else { \
			INST(rm, reg, lreg32, sreg32, lseg, 0) \
		} \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST(&meml, reg, laddr16, saddr16, lseg, 0) \
	}

#define SwEw(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST(reg, rm, lseg, 0, lreg16, sreg16) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST(reg, &meml,lseg, 0, laddr16, saddr16) \
	}

#define limm(i) i
#ifdef I386_OPT1
/*
 * A byte register, without asking which half it is.
 *
 * The eight byte registers are the low and second bytes of the first four
 * words, and their numbering says which: AL..BL are 0..3 and AH..BH are
 * 4..7, so register (i & 3) and byte (i >> 2).  Written out that way the
 * address is three integer operations instead of a comparison and a choice
 * between two addresses - and byte operations are a large part of what a
 * DOS game runs: TEST r/m8,r8 alone is a fifth of what Tyrian executes.
 */
#define reg8p(i) (&((u8 *)cpu->gprx)[(((i) & 3) << 2) | ((i) >> 2)])
#define lreg8(i) (*reg8p(i))
#define sreg8(i, v) (*reg8p(i) = (v))
#define lreg16(i) (cpu->gprx[i].r16)
#define sreg16(i, v) (cpu->gprx[i].r16 = (v))
#define lreg32(i) (REGi(i))
#define sreg32(i, v) ((REGi(i)) = (v))
#else
#define lreg8(i) ((u8) ((i) > 3 ? REGi((i) - 4) >> 8 : REGi((i))))
#define sreg8(i, v) ((i) > 3 ? \
		     (REGi((i) - 4) = (REGi((i) - 4) & (wordmask ^ 0xff00)) | (((v) & 0xff) << 8)) : \
		     (REGi((i)) = (REGi((i)) & (wordmask ^ 0xff)) | ((v) & 0xff)))
#define lreg16(i) ((u16) REGi((i)))
#define sreg16(i, v) (REGi((i)) = (REGi((i)) & (wordmask ^ 0xffff)) | ((v) & 0xffff))
#define lreg32(i) ((u32) REGi((i)))
#define sreg32(i, v) (REGi((i)) = (REGi((i)) & (wordmask ^ 0xffffffff)) | ((v) & 0xffffffff))
#endif
#define laddr8(addr) load8(cpu, addr)
#define saddr8(addr, v) store8(cpu, addr, v)
#define laddr16(addr) load16(cpu, addr)
#define saddr16(addr, v) store16(cpu, addr, v)
#define laddr32(addr) load32(cpu, addr)
#define saddr32(addr, v) store32(cpu, addr, v)
#define lseg(i) ((u16) SEGi((i)))
#define set_sp(v, mask) (sreg32(4, ((v) & mask) | (lreg32(4) & ~mask)))

/*
 * instructions
 */
#define ACOP_helper(NAME1, NAME2, BIT, OP, a, b, la, sa, lb, sb) \
	int cf = get_CF(cpu); \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.src2 = sext ## BIT(lb(b)); \
	cpu->cc.dst = sext ## BIT(cpu->cc.src1 OP cpu->cc.src2 OP cf); \
	cpu->cc.op = cf ? CC_ ## NAME1 : CC_ ## NAME2; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define AOP0_helper(NAME, BIT, OP, a, b, la, sa, lb, sb) \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.src2 = sext ## BIT(lb(b)); \
	cpu->cc.dst = sext ## BIT(cpu->cc.src1 OP cpu->cc.src2); \
	cpu->cc.op = CC_ ## NAME; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF;

#define LOP0_helper(NAME, BIT, OP, a, b, la, sa, lb, sb) \
	cpu->cc.dst = sext ## BIT(la(a) OP lb(b)); \
	cpu->cc.op = CC_ ## NAME; \
	cpu->cc.mask = CF | PF | ZF | SF | OF;

#define AOP_helper(NAME1, BIT, OP, a, b, la, sa, lb, sb) \
	AOP0_helper(NAME1, BIT, OP, a, b, la, sa, lb, sb) \
	sa(a, cpu->cc.dst);

#define LOP_helper(NAME1, BIT, OP, a, b, la, sa, lb, sb) \
	LOP0_helper(NAME1, BIT, OP, a, b, la, sa, lb, sb) \
	sa(a, cpu->cc.dst);

#define INCDEC_helper(NAME, BIT, OP, a, la, sa) \
	int cf = get_CF(cpu); \
	cpu->cc.dst = sext ## BIT(sext ## BIT(la(a)) OP 1); \
	cpu->cc.op = CC_ ## NAME ## BIT; \
	SET_BIT(cpu->flags, cf, CF); \
	cpu->cc.mask = PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define NEG_helper(BIT, a, la, sa) \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.dst = sext ## BIT(-cpu->cc.src1); \
	cpu->cc.op = CC_NEG ## BIT; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define ADCb(...) ACOP_helper(ADC, ADD,  8, +, __VA_ARGS__)
#define ADCw(...) ACOP_helper(ADC, ADD, 16, +, __VA_ARGS__)
#define ADCd(...) ACOP_helper(ADC, ADD, 32, +, __VA_ARGS__)
#define SBBb(...) ACOP_helper(SBB, SUB,  8, -, __VA_ARGS__)
#define SBBw(...) ACOP_helper(SBB, SUB, 16, -, __VA_ARGS__)
#define SBBd(...) ACOP_helper(SBB, SUB, 32, -, __VA_ARGS__)
#define ADDb(...) AOP_helper(ADD,  8, +, __VA_ARGS__)
#define ADDw(...) AOP_helper(ADD, 16, +, __VA_ARGS__)
#define ADDd(...) AOP_helper(ADD, 32, +, __VA_ARGS__)
#define SUBb(...) AOP_helper(SUB,  8, -, __VA_ARGS__)
#define SUBw(...) AOP_helper(SUB, 16, -, __VA_ARGS__)
#define SUBd(...) AOP_helper(SUB, 32, -, __VA_ARGS__)
#define ORb(...)  LOP_helper(OR,   8, |, __VA_ARGS__)
#define ORw(...)  LOP_helper(OR,  16, |, __VA_ARGS__)
#define ORd(...)  LOP_helper(OR,  32, |, __VA_ARGS__)
#define ANDb(...) LOP_helper(AND,  8, &, __VA_ARGS__)
#define ANDw(...) LOP_helper(AND, 16, &, __VA_ARGS__)
#define ANDd(...) LOP_helper(AND, 32, &, __VA_ARGS__)
#define XORb(...) LOP_helper(XOR,  8, ^, __VA_ARGS__)
#define XORw(...) LOP_helper(XOR, 16, ^, __VA_ARGS__)
#define XORd(...) LOP_helper(XOR, 32, ^, __VA_ARGS__)
#define CMPb(...)  AOP0_helper(SUB,  8, -, __VA_ARGS__)
#define CMPw(...)  AOP0_helper(SUB, 16, -, __VA_ARGS__)
#define CMPd(...)  AOP0_helper(SUB, 32, -, __VA_ARGS__)
#define TESTb(...) LOP0_helper(AND,  8, &, __VA_ARGS__)
#define TESTw(...) LOP0_helper(AND, 16, &, __VA_ARGS__)
#define TESTd(...) LOP0_helper(AND, 32, &, __VA_ARGS__)
#define INCb(...) INCDEC_helper(INC,  8, +, __VA_ARGS__)
#define INCw(...) INCDEC_helper(INC, 16, +, __VA_ARGS__)
#define INCd(...) INCDEC_helper(INC, 32, +, __VA_ARGS__)
#define DECb(...) INCDEC_helper(DEC,  8, -, __VA_ARGS__)
#define DECw(...) INCDEC_helper(DEC, 16, -, __VA_ARGS__)
#define DECd(...) INCDEC_helper(DEC, 32, -, __VA_ARGS__)
#define NOTb(a, la, sa) sa(a, ~la(a));
#define NOTw(a, la, sa) sa(a, ~la(a));
#define NOTd(a, la, sa) sa(a, ~la(a));
#define NEGb(...) NEG_helper(8,  __VA_ARGS__)
#define NEGw(...) NEG_helper(16, __VA_ARGS__)
#define NEGd(...) NEG_helper(32, __VA_ARGS__)

#define SHL_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y = (lb(b)) & 0x1f; \
	if (y) { \
		cpu->cc.dst = sext ## BIT(x << y); \
		cpu->cc.dst2 = ((x >> (BIT - y)) & 1); \
		cpu->cc.op = CC_SHL; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SHLb(...) SHL_helper(8, __VA_ARGS__)
#define SHLw(...) SHL_helper(16, __VA_ARGS__)
#define SHLd(...) SHL_helper(32, __VA_ARGS__)

#define ROL_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y0 = lb(b); \
	uword y = y0 & (BIT - 1); \
	uword res = x; \
	if (y) { \
		res = sext ## BIT((x << y) | (x >> (BIT - y))); \
		sa(a, res); \
	} \
	if (y0) { \
		int cf1 = res & 1; \
		int of1 = (res >> (sizeof(uword) * 8 - 1)) ^ cf1; \
		SET_BIT(cpu->flags, cf1, CF); \
		SET_BIT(cpu->flags, of1, OF); \
		cpu->cc.mask &= ~(CF | OF); \
	}

#define ROLb(...) ROL_helper(8, __VA_ARGS__)
#define ROLw(...) ROL_helper(16, __VA_ARGS__)
#define ROLd(...) ROL_helper(32, __VA_ARGS__)

#define RCL_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y = ((lb(b)) & 0x1f) % (BIT + 1); \
	if (y) { \
		uword cf = get_CF(cpu); \
		uword res = sext ## BIT((x << y) | (cf << (y - 1)) | (y != 1 ? (x >> (BIT + 1 - y)) : 0)); \
		int cf1 = (x >> (BIT - y)) & 1; \
		int of1 = (res >> (sizeof(uword) * 8 - 1)) ^ cf1; \
		SET_BIT(cpu->flags, cf1, CF); \
		SET_BIT(cpu->flags, of1, OF); \
		cpu->cc.mask &= ~(CF | OF); \
		sa(a, res); \
	}

#define RCLb(...) RCL_helper(8, __VA_ARGS__)
#define RCLw(...) RCL_helper(16, __VA_ARGS__)
#define RCLd(...) RCL_helper(32, __VA_ARGS__)

#define RCR_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y = ((lb(b)) & 0x1f) % (BIT + 1); \
	if (y) { \
		uword cf = get_CF(cpu); \
		uword res = sext ## BIT((x >> y) | (cf << (BIT - y)) | (y != 1 ? (x << (BIT + 1 - y)) : 0)); \
		int cf1 = (sext ## BIT(x << (BIT - y)) >> (BIT - 1)) & 1; \
		int of1 = ((res ^ (res << 1)) >> (BIT - 1)) & 1; \
		SET_BIT(cpu->flags, cf1, CF); \
		SET_BIT(cpu->flags, of1, OF); \
		cpu->cc.mask &= ~(CF | OF); \
		sa(a, res); \
	}

#define RCRb(...) RCR_helper(8, __VA_ARGS__)
#define RCRw(...) RCR_helper(16, __VA_ARGS__)
#define RCRd(...) RCR_helper(32, __VA_ARGS__)

#define ROR_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y0 = lb(b); \
	uword y = y0 & (BIT - 1); \
	uword res = x; \
	if (y) { \
		res = sext ## BIT((x >> y) | (x << (BIT - y))); \
		sa(a, res); \
	} \
	if (y0) { \
		int cf1 = (res >> (BIT - 1)) & 1; \
		int of1 = ((res ^ (res << 1)) >> (BIT - 1)) & 1; \
		SET_BIT(cpu->flags, cf1, CF); \
		SET_BIT(cpu->flags, of1, OF); \
		cpu->cc.mask &= ~(CF | OF); \
	}

#define RORb(...) ROR_helper(8, __VA_ARGS__)
#define RORw(...) ROR_helper(16, __VA_ARGS__)
#define RORd(...) ROR_helper(32, __VA_ARGS__)

#define SHR_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y = (lb(b)) & 0x1f; \
	if (y) { \
		cpu->cc.src1 = sext ## BIT(x); \
		cpu->cc.dst = sext ## BIT(x >> y); \
		cpu->cc.dst2 = (x >> (y - 1)) & 1; \
		cpu->cc.op = CC_SHR; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SHRb(...) SHR_helper(8, __VA_ARGS__)
#define SHRw(...) SHR_helper(16, __VA_ARGS__)
#define SHRd(...) SHR_helper(32, __VA_ARGS__)

#define SHLD_helper(BIT, a, b, c, la, sa, lb, sb, lc, sc) \
	int count = (lc(c)) & 0x1f; \
	uword x = la(a); \
	uword y = lb(b); \
	if (count) { \
		cpu->cc.src1 = sext ## BIT(x); \
		if (BIT < count) {  /* undocumented */ \
			uword z = x; x = y; y = z; \
			count -= BIT; \
		} \
		cpu->cc.dst = sext ## BIT((x << count) | (y >> (BIT - count))); \
		if (count == 1) { \
			cpu->cc.dst2 = sext ## BIT(x); \
		} else { \
			cpu->cc.dst2 = sext ## BIT((x << (count - 1)) | (count == 1 ? 0 : (y >> (BIT - (count - 1))))); \
		} \
		cpu->cc.op = CC_SHLD; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SHLDw(...) SHLD_helper(16, __VA_ARGS__)
#define SHLDd(...) SHLD_helper(32, __VA_ARGS__)

#define SHRD_helper(BIT, a, b, c, la, sa, lb, sb, lc, sc) \
	int count = (lc(c)) & 0x1f; \
	uword x = la(a); \
	uword y = lb(b); \
	if (count) { \
		if (BIT < count) {  /* undocumented */ \
			uword z = x; x = y; y = z; \
			count -= BIT; \
		} \
		cpu->cc.src1 = sext ## BIT(x); \
		cpu->cc.dst = sext ## BIT((x >> count) | (y << (BIT - count))); \
		if (count == 1) { \
			cpu->cc.dst2 = sext ## BIT(x); \
		} else { \
			cpu->cc.dst2 = sext ## BIT((x >> (count - 1)) | (y << (BIT - (count - 1)))); \
		} \
		cpu->cc.op = CC_SHRD; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SHRDw(...) SHRD_helper(16, __VA_ARGS__)
#define SHRDd(...) SHRD_helper(32, __VA_ARGS__)

// ">>"
#define SAR_helper(BIT, a, b, la, sa, lb, sb) \
	sword x = sext ## BIT(la(a)); \
	sword y = (lb(b)) & 0x1f; \
	if (y) { \
		cpu->cc.dst = x >> y; \
		cpu->cc.dst2 = (x >> (y - 1)) & 1; \
		cpu->cc.op = CC_SAR; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SARb(...) SAR_helper(8, __VA_ARGS__)
#define SARw(...) SAR_helper(16, __VA_ARGS__)
#define SARd(...) SAR_helper(32, __VA_ARGS__)

#define IMUL2w(a, b, la, sa, lb, sb) \
	cpu->cc.src1 = sext16(la(a)); \
	cpu->cc.src2 = sext16(lb(b)); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_IMUL16; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define IMUL2d(a, b, la, sa, lb, sb) \
	cpu->cc.src1 = sext32(la(a)); \
	cpu->cc.src2 = sext32(lb(b)); \
	int64_t res = (int64_t) (s32) cpu->cc.src1 * (int64_t) (s32) cpu->cc.src2; \
	cpu->cc.dst = res; \
	cpu->cc.dst2 = res >> 32; \
	cpu->cc.op = CC_IMUL32; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define IMUL2wI_helper(BIT, BITI, a, b, c, la, sa, lb, sb) \
	cpu->cc.src1 = sext ## BIT(lb(b)); \
	cpu->cc.src2 = sext ## BITI(c); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_IMUL ## BIT; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define IMUL2dI_helper(BIT, BITI, a, b, c, la, sa, lb, sb) \
	cpu->cc.src1 = sext ## BIT(lb(b)); \
	cpu->cc.src2 = sext ## BITI(c); \
	int64_t res = (int64_t) (s32) cpu->cc.src1 * (int64_t) (s32) cpu->cc.src2; \
	cpu->cc.dst = res; \
	cpu->cc.dst2 = res >> 32; \
	cpu->cc.op = CC_IMUL ## BIT; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define IMUL2wIb(...) IMUL2wI_helper(16, 8, __VA_ARGS__)
#define IMUL2wIw(...) IMUL2wI_helper(16, 16, __VA_ARGS__)
#define IMUL2dIb(...) IMUL2dI_helper(32, 8, __VA_ARGS__)
#define IMUL2dId(...) IMUL2dI_helper(32, 32, __VA_ARGS__)

#define IMULb(a, la, sa) \
	cpu->cc.src1 = sext8(lreg8(0)); \
	cpu->cc.src2 = sext8(la(a)); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_IMUL8; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg16(0, cpu->cc.dst);

#define IMULw(a, la, sa) \
	cpu->cc.src1 = sext16(lreg16(0)); \
	cpu->cc.src2 = sext16(la(a)); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_IMUL16; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg16(0, cpu->cc.dst); \
	sreg16(2, (cpu->cc.dst >> 16));

#define IMULd(a, la, sa) \
	cpu->cc.src1 = sext32(lreg32(0)); \
	cpu->cc.src2 = sext32(la(a)); \
	int64_t res = (int64_t) (s32) cpu->cc.src1 * (int64_t) (s32) cpu->cc.src2; \
	cpu->cc.dst = res; \
	cpu->cc.dst2 = res >> 32; \
	cpu->cc.op = CC_IMUL32; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg32(0, cpu->cc.dst); \
	sreg32(2, cpu->cc.dst2);

#define MULb(a, la, sa) \
	cpu->cc.src1 = lreg8(0); \
	cpu->cc.src2 = la(a); \
	cpu->cc.dst = sext16(cpu->cc.src1 * cpu->cc.src2); \
	cpu->cc.op = CC_MUL8; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg16(0, cpu->cc.dst);

#define MULw(a, la, sa) \
	cpu->cc.src1 = lreg16(0); \
	cpu->cc.src2 = la(a); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_MUL16; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg16(0, cpu->cc.dst); \
	sreg16(2, (cpu->cc.dst >> 16));

#define MULd(a, la, sa) \
	cpu->cc.src1 = lreg32(0); \
	cpu->cc.src2 = la(a); \
	uint64_t res = (uint64_t) cpu->cc.src1 * (uint64_t) cpu->cc.src2; \
	cpu->cc.dst = res; \
	cpu->cc.dst2 = res >> 32; \
	cpu->cc.op = CC_MUL32; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg32(0, cpu->cc.dst); \
	sreg32(2, cpu->cc.dst2);

#define IDIVb(a, la, sa) \
	sword src1 = sext16(lreg16(0)); \
	sword src2 = sext8(la(a)); \
	if (src2 == 0) THROW0(EX_DE); \
	sword res = src1 / src2; \
	if (res > 127 || res < -128) THROW0(EX_DE); \
	sreg8(0, res); \
	sreg8(4, src1 % src2);

#define IDIVw(a, la, sa) \
	sword src1 = sext32(lreg16(0) | (lreg16(2)<< 16)); \
	sword src2 = sext16(la(a)); \
	if (src2 == 0) THROW0(EX_DE); \
	sword res = src1 / src2; \
	if (res > 32767 || res < -32768) THROW0(EX_DE); \
	sreg16(0, res); \
	sreg16(2, src1 % src2);

#define IDIVd(a, la, sa) \
	int64_t src1 = (((uint64_t) lreg32(2)) << 32) | lreg32(0); \
	int64_t src2 = (sword) (la(a));	\
	if (src2 == 0) THROW0(EX_DE); \
	int64_t res = src1 / src2; \
	if (res > 2147483647 || res < -2147483648) THROW0(EX_DE); \
	sreg32(0, res); \
	sreg32(2, src1 % src2);

#define DIVb(a, la, sa) \
	uword src1 = lreg16(0); \
	uword src2 = la(a); \
	if (src2 == 0) THROW0(EX_DE); \
	uword res = src1 / src2; \
	if (res > 0xff) THROW0(EX_DE); \
	/* bypass the Cyrix 5/2 test */ \
	if (src1 == 0x5 && src2 == 0x2) { cpu->cc.mask &= ~ZF; cpu->flags |= ZF; } \
	sreg8(0, res); \
	sreg8(4, src1 % src2);

#define DIVw(a, la, sa) \
	uword src1 = lreg16(0) | (lreg16(2)<< 16); \
	uword src2 = la(a); \
	if (src2 == 0) THROW0(EX_DE); \
	uword res = src1 / src2; \
	if (res > 0xffff) THROW0(EX_DE); \
	/* bypass the NexGen 0x5555/2 test */ \
	if (src1 == 0x5555 && src2 == 0x2) { cpu->cc.mask &= ~ZF; cpu->flags &= ~ZF; } \
	sreg16(0, res); \
	sreg16(2, src1 % src2);

#define DIVd(a, la, sa) \
	uint64_t src1 = (((uint64_t) lreg32(2)) << 32) | lreg32(0); \
	uint64_t src2 = la(a); \
	if (src2 == 0) THROW0(EX_DE); \
	uint64_t res = src1 / src2; \
	if (res > 0xffffffff) THROW0(EX_DE); \
	sreg32(0, res); \
	sreg32(2, src1 % src2);

#define BT_helper(BIT, a, b, la, sa, lb, sb) \
	int bb = lb(b) % BIT; \
	bool bit = (la(a) >> bb) & 1; \
	cpu->cc.mask &= ~CF; \
	SET_BIT(cpu->flags, bit, CF);

#define BTw(...) BT_helper(16, __VA_ARGS__)
#define BTd(...) BT_helper(32, __VA_ARGS__)

#define BTX_helper(BIT, OP, a, b, la, sa, lb, sb) \
	int bb = lb(b) % BIT; \
	bool bit = (la(a) >> bb) & 1; \
	sa(a, la(a) OP (1 << bb)); \
	cpu->cc.mask &= ~CF; \
	SET_BIT(cpu->flags, bit, CF);

#define BTSw(...) BTX_helper(16, |, __VA_ARGS__)
#define BTSd(...) BTX_helper(32, |, __VA_ARGS__)
#define BTRw(...) BTX_helper(16, & ~, __VA_ARGS__)
#define BTRd(...) BTX_helper(32, & ~, __VA_ARGS__)
#define BTCw(...) BTX_helper(16, ^, __VA_ARGS__)
#define BTCd(...) BTX_helper(32, ^, __VA_ARGS__)

#define BSF_helper(BIT, a, b, la, sa, lb, sb) \
	u ## BIT src = lb(b); \
	u ## BIT temp = 0; \
	cpu->cc.mask = 0; \
	if (src == 0) { \
		cpu->flags |= ZF; \
	} else { \
		cpu->flags &= ~ZF; \
		while ((src & 1) == 0) { \
			temp++; \
			src >>= 1; \
		} \
		sa(a, temp); \
	}

#define BSFw(...) BSF_helper(16, __VA_ARGS__)
#define BSFd(...) BSF_helper(32, __VA_ARGS__)

#define BSR_helper(BIT, a, b, la, sa, lb, sb) \
	s ## BIT src = lb(b); \
	u ## BIT temp = BIT - 1; \
	cpu->cc.mask = 0; \
	if (src == 0) { \
		cpu->flags |= ZF; \
	} else { \
		cpu->flags &= ~ZF; \
		while (src >= 0) { \
			temp--; \
			src <<= 1; \
		} \
		sa(a, temp); \
	}

#define BSRw(...) BSR_helper(16, __VA_ARGS__)
#define BSRd(...) BSR_helper(32, __VA_ARGS__)

#define MOVb(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVw(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVd(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVSeg(a, b, la, sa, lb, sb) \
	if (a == SEG_CS) THROW0(EX_UD); \
	if (a == SEG_SS) stepcount++; \
	TRY(set_seg(cpu, a, lb(b)));
#define MOVZXdb(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVZXwb(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVZXww(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVZXdw(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVSXdb(a, b, la, sa, lb, sb) sa(a, sext8(lb(b)));
#define MOVSXwb(a, b, la, sa, lb, sb) sa(a, sext8(lb(b)));
#define MOVSXww(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVSXdw(a, b, la, sa, lb, sb) sa(a, sext16(lb(b)));

#define XCHG(a, b, la, sa, lb, sb) \
	uword tmp = lb(b); \
	sb(b, la(a)); \
	sa(a, tmp);
#define XCHGb(...) XCHG(__VA_ARGS__)
#define XCHGw(...) XCHG(__VA_ARGS__)
#define XCHGd(...) XCHG(__VA_ARGS__)

#define XCHGAX() \
	if (opsz16) { \
		int reg = b1 & 7; \
		uword tmp = lreg16(reg); \
		sreg16(reg, lreg16(0)); \
		sreg16(0, tmp); \
	} else { \
		int reg = b1 & 7; \
		uword tmp = lreg32(reg); \
		sreg32(reg, lreg32(0)); \
		sreg32(0, tmp); \
	}

#define LEAd(a, b, la, sa, lb, sb) \
	if (mod == 3) THROW0(EX_UD); \
	sa(a, lb(b));
#define LEAw LEAd

#define CBW_CWDE() \
	if (opsz16) sreg16(0, sext8(lreg8(0))); \
	else sreg32(0, sext16(lreg16(0)));

#define CWD_CDQ() \
	if (opsz16) sreg16(2, sext16(-(sext16(lreg16(0)) >> 31))); \
	else sreg32(2, sext32(-(sext32(lreg32(0)) >> 31)));

#define MOVFC() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int rm = modrm & 7; \
	if (reg == 0) { \
		sreg32(rm, cpu->cr0); \
	} else if (reg == 2) { \
		sreg32(rm, cpu->cr2); \
	} else if (reg == 3) { \
		sreg32(rm, cpu->cr3); \
	} else if (reg == 4) { \
		sreg32(rm, 0); \
	} else THROW0(EX_UD);

#define MOVTC() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int rm = modrm & 7; \
	if (reg == 0) { \
		u32 new_cr0 = lreg32(rm); \
		if ((new_cr0 ^ cpu->cr0) & (CR0_PG | CR0_WP | 1)) \
			tlb_clear(cpu); \
		if (cpu->fpu) new_cr0 |= 0x10; \
		cpu->cr0 = new_cr0; \
	} else if (reg == 2) { \
		cpu->cr2 = lreg32(rm); \
	} else if (reg == 3) { \
		cpu->cr3 = lreg32(rm); \
		tlb_clear(cpu); \
	} else if (reg == 4) { \
	} else THROW0(EX_UD);

/*
 * INT3 has to take the same route as INT n in virtual-8086 mode.
 *
 * Throwing #BP straight away bypassed the IOPL check that INT() does, so a
 * guest running under EMM386 got a breakpoint exception delivered to the
 * monitor instead of a software interrupt to reflect.  EMM386 does not
 * reflect an unexpected #BP - it halts with "has detected error #83 in an
 * application" - which is why several games (Summer Challenge among them)
 * died the moment EMM386 was loaded and were fine without it: in real mode
 * IVT[3] points at an IRET and the call is harmless.  They call INT 3 on
 * purpose, with arguments in SI/DI, as a hook for a resident program.
 *
 * The #GP is a fault, so cpu->ip must still point at the INT3 itself; only
 * the #BP path, a trap, advances to the next instruction.
 */
#define INT3() \
	if (cpu->flags & VM) { \
		if (get_IOPL(cpu) < 3) THROW(EX_GP, 0); \
		uword oldip = cpu->ip; \
		cpu->ip = cpu->next_ip; \
		if (!call_isr(cpu, 3, false, 0)) { \
			cpu->ip = oldip; \
			return false; \
		} \
	} else { \
		cpu->ip = cpu->next_ip; \
		THROW0(EX_BP); \
	}

#define INTO() \
	if (get_OF(cpu)) { \
		cpu->ip = cpu->next_ip; \
		THROW0(EX_OF); \
	}

static bool call_isr(CPUI386 *cpu, int no, bool pusherr, int ext);

#define INT(i, li, _) \
	/*dolog("int %02x %08x %04x:%08x\n", li(i), REGi[0], SEGi(SEG_CS), cpu->ip);*/ \
	if ((cpu->flags & VM)) { \
		if(get_IOPL(cpu) < 3) THROW(EX_GP, 0); \
	} \
	uword oldip = cpu->ip; \
	cpu->ip = cpu->next_ip; \
	if (!call_isr(cpu, li(i), false, 0)) { \
		cpu->ip = oldip; \
		return false; \
	}

#define IRET() \
	if ((cpu->cr0 & 1) && (!(cpu->flags & VM) || get_IOPL(cpu) < 3)) { \
		TRY(pmret(cpu, opsz16, 0, true)); \
        cpu->prefetch_base = (u32)-1; \
	} else { \
		OptAddr meml1, meml2, meml3; \
		uword sp = lreg32(4); \
		register uword newip; \
		if (opsz16) { \
			/* ip */ TRY(translate16(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
			newip = laddr16(&meml1); \
			/* cs */ TRY(translate16(cpu, &meml2, 1, SEG_SS, (sp + 2) & sp_mask)); \
			int newcs = laddr16(&meml2); \
			/* flags */ TRY(translate16(cpu, &meml3, 1, SEG_SS, (sp + 4) & sp_mask)); \
			uword oldflags = cpu->flags; \
			if (cpu->flags & VM) cpu->flags = (cpu->flags & (0xffff0000 | IOPL)) | (laddr16(&meml3) & ~IOPL); \
			else cpu->flags = (cpu->flags & 0xffff0000) | laddr16(&meml3); \
			cpu->flags &= EFLAGS_MASK; \
			cpu->flags |= 0x2; \
			if (!set_seg(cpu, SEG_CS, newcs)) { cpu->flags = oldflags; return false; } \
			cpu->cc.mask = 0; \
			set_sp(sp + 6, sp_mask); \
		} else { \
			/* eip */ TRY(translate32(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
			newip = laddr32(&meml1); \
			/* cs (pop as dword, selector is low 16 bits) */ \
			TRY(translate32(cpu, &meml2, 1, SEG_SS, (sp + 4) & sp_mask)); \
			int newcs = laddr32(&meml2); \
			/* eflags */ TRY(translate32(cpu, &meml3, 1, SEG_SS, (sp + 8) & sp_mask)); \
			uword oldflags = cpu->flags; \
			if (cpu->flags & VM) cpu->flags = (cpu->flags & IOPL) | (laddr32(&meml3) & ~IOPL); \
			else cpu->flags = laddr32(&meml3); \
			cpu->flags &= EFLAGS_MASK; \
			cpu->flags |= 0x2; \
			if (!set_seg(cpu, SEG_CS, newcs)) { cpu->flags = oldflags; return false; } \
			cpu->cc.mask = 0; \
			set_sp(sp + 12, sp_mask); \
		} \
		cpu->next_ip = newip; \
        cpu->prefetch_base = (u32)-1; \
	} \
	if (cpu->intr && (cpu->flags & IF)) return true;

#define RETFARw(i, li, _) \
	if ((cpu->cr0 & 1) && !(cpu->flags & VM)) { \
		TRY(pmret(cpu, opsz16, li(i), false)); \
	} else { \
		uword sp = lreg32(4); \
		OptAddr meml1, meml2; \
		register uword newip; \
		if (opsz16) { \
			/* ip */ TRY(translate16(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
			newip = laddr16(&meml1); \
			/* cs */ TRY(translate16(cpu, &meml2, 1, SEG_SS, (sp + 2) & sp_mask)); \
			int newcs = laddr16(&meml2); \
			TRY(set_seg(cpu, SEG_CS, newcs)); \
			set_sp(sp + 4 + li(i), sp_mask); \
		} else { \
			/* ip */ TRY(translate32(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
			newip = laddr32(&meml1); \
			/* cs */ TRY(translate32(cpu, &meml2, 1, SEG_SS, (sp + 4) & sp_mask)); \
			int newcs = laddr32(&meml2); \
			TRY(set_seg(cpu, SEG_CS, newcs)); \
			set_sp(sp + 8 + li(i), sp_mask); \
		} \
		cpu->next_ip = newip; \
		cpu->prefetch_base = (u32)-1; \
	}

#define RETFAR() RETFARw(0, limm, 0)

/*
 * A HLT with interrupts off is the machine switching itself off.
 *
 * Only an NMI or a reset leaves it, and nothing on this machine raises an
 * NMI, so it is permanent: it is what "it is now safe to turn off your
 * computer" is on a real PC.  Windows 95 ends its shutdown that way in its
 * own code, and SeaBIOS answers APM "set power state: off" the same way
 * once it finds no ACPI register to write.  So it is reported rather than
 * emulated for ever.  Idling - DOS, Windows, APM's CPU idle - halts with
 * interrupts on and is untouched; a HLT from virtual-8086 mode never gets
 * here, it traps to the monitor.
 *
 * A machine that has crashed halts the same way when the garbage it runs
 * happens to hold CLI and HLT, and switching the board off then throws away
 * everything the rings kept about the crash.  Win95 setup's mini-Windows did
 * that from real mode, sliding through zeroed memory.  So a real-mode HLT
 * counts only from the BIOS segment, where SeaBIOS's APM "off" is, and not
 * at all once the rings were frozen because the stack went into the vector
 * table (see call_isr).
 */
#define HLT() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	if (unlikely(!(cpu->flags & IF)) && !g_exc_frozen && \
	    ((cpu->cr0 & 1) || cpu->seg[SEG_CS].base == 0xf0000)) \
		cpu->power_off = true; \
	cpu->halt = true; return true;
#define NOP()

#define LAHF() \
	refresh_flags(cpu); \
	cpu->cc.mask = 0; \
	sreg8(4, cpu->flags);

#define SAHF() \
	cpu->cc.mask &= OF; \
	cpu->flags = (cpu->flags & (wordmask ^ 0xff)) | lreg8(4); \
	cpu->flags &= EFLAGS_MASK; \
	cpu->flags |= 0x2;

#define CMC() \
	int cf = get_CF(cpu); \
	cpu->cc.mask &= ~CF; \
	SET_BIT(cpu->flags, !cf, CF);

#define CLC() \
	cpu->cc.mask &= ~CF; \
	cpu->flags &= ~CF;

#define STC() \
	cpu->cc.mask &= ~CF; \
	cpu->flags |= CF;

#define CLI() \
	if (get_IOPL(cpu) < cpu->cpl) THROW(EX_GP, 0); \
	cpu->flags &= ~IF;

/* STI: interrupts enabled at the end of the **next** instruction */
#define STI() \
	if (get_IOPL(cpu) < cpu->cpl) THROW(EX_GP, 0); \
	cpu->flags |= IF; \
	if (cpu->intr || stepcount < 2) stepcount = 2;

#define CLD() \
	cpu->flags &= ~DF;

#define STD() \
	cpu->flags |= DF;

#define PUSHb(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	uword val = sext8(la(a)); \
	if (opsz16) { \
		TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask)); \
		set_sp(sp - 2, sp_mask); \
		saddr16(&meml1, val); \
	} else { \
		TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask)); \
		set_sp(sp - 4, sp_mask); \
		saddr32(&meml1, val); \
	}

#define PUSHw(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	uword val = sext16(la(a)); \
	TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask)); \
	set_sp(sp - 2, sp_mask); \
	saddr16(&meml1, val);

#define PUSHd(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	uword val = sext32(la(a)); \
	TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask)); \
	set_sp(sp - 4, sp_mask); \
	saddr32(&meml1, val);

#define POPRegw(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	TRY(translate16(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
	u16 src = laddr16(&meml1); \
	set_sp(sp + 2, sp_mask); \
	sa(a, src);

#define POPRegd(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	TRY(translate32(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
	u32 src = laddr32(&meml1); \
	set_sp(sp + 4, sp_mask); \
	sa(a, src);

#define POP_helper(BIT) \
	OptAddr meml1; \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	uword sp = lreg32(4); \
	TRY(translate ## BIT(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
	u ## BIT src = laddr ## BIT(&meml1); \
	set_sp(sp + BIT / 8, sp_mask); \
	if (mod == 3) { \
		sreg ## BIT(rm, src); \
	} else { \
		if (!modsib(cpu, adsz16, mod, rm, &addr, &curr_seg) || \
		    !translate ## BIT(cpu, &meml, 2, curr_seg, addr)) { \
			set_sp(sp, sp_mask); \
			return false; \
		} \
		saddr ## BIT(&meml, src); \
	}

#define POP() if (opsz16) { POP_helper(16) } else { POP_helper(32) }

#define PUSHF() \
	if ((cpu->flags & VM) && get_IOPL(cpu) < 3) THROW(EX_GP, 0); \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 2, SEG_SS, (sp - 2) & sp_mask)); \
		refresh_flags(cpu); \
		cpu->cc.mask = 0; \
		set_sp(sp - 2, sp_mask); \
		saddr16(&meml, cpu->flags); \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 2, SEG_SS, (sp - 4) & sp_mask)); \
		refresh_flags(cpu); \
		cpu->cc.mask = 0; \
		set_sp(sp - 4, sp_mask); \
		saddr32(&meml, cpu->flags & ~(RF | VM)); \
	}

#define EFLAGS_MASK_386 0x37fd7
#define EFLAGS_MASK_486 0x77fd7
#define EFLAGS_MASK_586 0x277fd7
#define EFLAGS_MASK (cpu->flags_mask)

#define POPF() \
	if ((cpu->flags & VM) && get_IOPL(cpu) < 3) THROW(EX_GP, 0); \
	uword mask = VM; \
	if (cpu->cr0 & 1) { \
		if (cpu->cpl > 0) mask |= IOPL; \
		if (get_IOPL(cpu) < cpu->cpl) mask |= IF; \
	} \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 2, sp_mask); \
		cpu->flags = (cpu->flags & (0xffff0000 | mask)) | (laddr16(&meml) & ~mask); \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 4, sp_mask); \
		cpu->flags = (cpu->flags & mask) | (laddr32(&meml) & ~mask); \
	} \
	cpu->flags &= EFLAGS_MASK; \
	cpu->flags |= 0x2; \
	cpu->cc.mask = 0; \
	if (cpu->intr && (cpu->flags & IF)) return true;

#define PUSHSeg(seg) \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 2, SEG_SS, (sp - 2) & sp_mask)); \
		set_sp(sp - 2, sp_mask); \
		saddr16(&meml, lseg(seg)); \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 2, SEG_SS, (sp - 4) & sp_mask)); \
		set_sp(sp - 4, sp_mask); \
		saddr32(&meml, lseg(seg)); \
	}
#define PUSH_ES() PUSHSeg(SEG_ES)
#define PUSH_CS() PUSHSeg(SEG_CS)
#define PUSH_SS() PUSHSeg(SEG_SS)
#define PUSH_DS() PUSHSeg(SEG_DS)
#define PUSH_FS() PUSHSeg(SEG_FS)
#define PUSH_GS() PUSHSeg(SEG_GS)

#define POPSeg(seg) \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		TRY(set_seg(cpu, seg, laddr16(&meml))); \
		set_sp(sp + 2, sp_mask); \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		TRY(set_seg(cpu, seg, laddr32(&meml))); \
		set_sp(sp + 4, sp_mask); \
	}
#define POP_ES() POPSeg(SEG_ES)
#define POP_SS() POPSeg(SEG_SS) stepcount++;
#define POP_DS() POPSeg(SEG_DS)
#define POP_FS() POPSeg(SEG_FS)
#define POP_GS() POPSeg(SEG_GS)

#define PUSHA_helper(BIT, BYTE) \
	uword sp = lreg32(4); \
	OptAddr meml1, meml2, meml3, meml4; \
	OptAddr meml5, meml6, meml7, meml8; \
	TRY(translate ## BIT(cpu, &meml1, 2, SEG_SS, (sp - BYTE * 1) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml2, 2, SEG_SS, (sp - BYTE * 2) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml3, 2, SEG_SS, (sp - BYTE * 3) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml4, 2, SEG_SS, (sp - BYTE * 4) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml5, 2, SEG_SS, (sp - BYTE * 5) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml6, 2, SEG_SS, (sp - BYTE * 6) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml7, 2, SEG_SS, (sp - BYTE * 7) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml8, 2, SEG_SS, (sp - BYTE * 8) & sp_mask)); \
	saddr ## BIT(&meml1, lreg ## BIT(0)); \
	saddr ## BIT(&meml2, lreg ## BIT(1)); \
	saddr ## BIT(&meml3, lreg ## BIT(2)); \
	saddr ## BIT(&meml4, lreg ## BIT(3)); \
	saddr ## BIT(&meml5, sp); \
	saddr ## BIT(&meml6, lreg ## BIT(5)); \
	saddr ## BIT(&meml7, lreg ## BIT(6)); \
	saddr ## BIT(&meml8, lreg ## BIT(7)); \
	set_sp(sp - BYTE * 8, sp_mask);
#define PUSHA() if (opsz16) { PUSHA_helper(16, 2) } else { PUSHA_helper(32, 4) }

#define POPA_helper(BIT, BYTE) \
	uword sp = lreg32(4); \
	OptAddr meml1, meml2, meml3, meml4; \
	OptAddr meml5, meml6, meml7; \
	TRY(translate ## BIT(cpu, &meml1, 1, SEG_SS, (sp + BYTE * 0) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml2, 1, SEG_SS, (sp + BYTE * 1) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml3, 1, SEG_SS, (sp + BYTE * 2) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml4, 1, SEG_SS, (sp + BYTE * 4) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml5, 1, SEG_SS, (sp + BYTE * 5) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml6, 1, SEG_SS, (sp + BYTE * 6) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml7, 1, SEG_SS, (sp + BYTE * 7) & sp_mask)); \
	sreg ## BIT(7, laddr ## BIT(&meml1)); \
	sreg ## BIT(6, laddr ## BIT(&meml2)); \
	sreg ## BIT(5, laddr ## BIT(&meml3)); \
	sreg ## BIT(3, laddr ## BIT(&meml4)); \
	sreg ## BIT(2, laddr ## BIT(&meml5)); \
	sreg ## BIT(1, laddr ## BIT(&meml6)); \
	sreg ## BIT(0, laddr ## BIT(&meml7)); \
	set_sp(sp + BYTE * 8, sp_mask);
#define POPA() if (opsz16) { POPA_helper(16, 2) } else { POPA_helper(32, 4) }

// string operations
#define stdi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 2, SEG_ES, lreg ## ABIT(7))); \
	saddr ## BIT(&meml, ax); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define ldsi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, curr_seg, lreg ## ABIT(6))); \
	ax = laddr ## BIT(&meml); \
	sreg ## ABIT(6, lreg ## ABIT(6) + dir);

#define lddi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, SEG_ES, lreg ## ABIT(7))); \
	ax = laddr ## BIT(&meml); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define ldsistdi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, curr_seg, lreg ## ABIT(6))); \
	ax = laddr ## BIT(&meml); \
	TRY(translate ## BIT(cpu, &meml, 2, SEG_ES, lreg ## ABIT(7))); \
	saddr ## BIT(&meml, ax); \
	sreg ## ABIT(6, lreg ## ABIT(6) + dir); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define ldsilddi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, curr_seg, lreg ## ABIT(6))); \
	ax0 = laddr ## BIT(&meml); \
	TRY(translate ## BIT(cpu, &meml, 1, SEG_ES, lreg ## ABIT(7))); \
	ax = laddr ## BIT(&meml); \
	sreg ## ABIT(6, lreg ## ABIT(6) + dir); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define xdir8 int dir = (cpu->flags & DF) ? -1 : 1;
#define xdir16 int dir = (cpu->flags & DF) ? -2 : 2;
#define xdir32 int dir = (cpu->flags & DF) ? -4 : 4;

#define STOS_helper2(BIT, ABIT) \
	OptAddr memld; \
	uword cx = lreg ## ABIT(1); \
	while (cx) { \
		TRY(translate ## BIT(cpu, &memld, 2, SEG_ES, lreg ## ABIT(7))); \
		if (memld.addr1 % (BIT / 8)) { \
			/* slow path */ \
			while (lreg ## ABIT(1)) { \
				stdi(BIT, ABIT) \
				sreg ## ABIT(1, lreg ## ABIT(1) - 1); \
			} \
			break; \
		} \
		uword count = cx; \
		int countd; \
		if (dir > 0) countd = (4096 - (memld.addr1 & 4095)) / (BIT / 8); \
		else countd = 1 + (memld.addr1 & 4095) / (BIT / 8); \
		if (countd < count) \
			count = countd; \
		for (uword i = 0; i <= count - 1; i++) { \
			saddr ## BIT(&memld, ax); \
			memld.addr1 += dir; \
		} \
		sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
		sreg ## ABIT(1, cx - count); \
		cx = lreg ## ABIT(1); \
	}

#define STOS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax = REGi(0); \
	if (rep == 0) { \
		if (adsz16) { stdi(BIT, 16) } else { stdi(BIT, 32) } \
	} else { \
		if (adsz16) { STOS_helper2(BIT, 16) } else { STOS_helper2(BIT, 32) } \
	}

#define LODS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { ldsi(BIT, 16) } else { ldsi(BIT, 32) } \
		sreg ## BIT(0, ax); \
	} else { \
		if (adsz16) { \
			while (lreg16(1)) { \
				ldsi(BIT, 16) \
				sreg ## BIT(0, ax); \
				sreg16(1, lreg16(1) - 1); \
			} \
		} else { \
			while (lreg32(1)) { \
				ldsi(BIT, 32) \
				sreg ## BIT(0, ax); \
				sreg32(1, lreg32(1) - 1); \
			} \
		} \
	}

#define SCAS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax0 = REGi(0); \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { lddi(BIT, 16) } else { lddi(BIT, 32) } \
		cpu->cc.src1 = sext ## BIT(ax0); \
		cpu->cc.src2 = sext ## BIT(ax); \
		cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
		cpu->cc.op = CC_SUB; \
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	} else { \
		if (adsz16) { \
			while (lreg16(1)) { \
				lddi(BIT, 16) \
				sreg16(1, lreg16(1) - 1); \
				cpu->cc.src1 = sext ## BIT(ax0); \
				cpu->cc.src2 = sext ## BIT(ax); \
				cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
				cpu->cc.op = CC_SUB; \
				cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
				bool zf = get_ZF(cpu); \
				if ((zf && rep == 2) || (!zf && rep == 1)) break; \
			} \
		} else { \
			while (lreg32(1)) { \
				lddi(BIT, 32) \
				sreg32(1, lreg32(1) - 1); \
				cpu->cc.src1 = sext ## BIT(ax0); \
				cpu->cc.src2 = sext ## BIT(ax); \
				cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
				cpu->cc.op = CC_SUB; \
				cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
				bool zf = get_ZF(cpu); \
				if ((zf && rep == 2) || (!zf && rep == 1)) break; \
			} \
		} \
	}

#define MOVS_helper2(BIT, ABIT) \
	OptAddr memls, memld; \
	uword cx = lreg ## ABIT(1); \
	while (cx) { \
		TRY(translate ## BIT(cpu, &memls, 1, curr_seg, lreg ## ABIT(6))); \
		TRY(translate ## BIT(cpu, &memld, 2, SEG_ES, lreg ## ABIT(7))); \
		if (memls.addr1 % (BIT / 8) || memld.addr1 % (BIT / 8)) { \
			/* slow path */ \
			while (lreg ## ABIT(1)) { \
				ldsistdi(BIT, ABIT) \
				sreg ## ABIT(1, lreg ## ABIT(1) - 1); \
			} \
			break; \
		} \
		uword count = cx; \
		int counts, countd; \
		if (dir > 0) { \
			counts = (4096 - (memls.addr1 & 4095)) / (BIT / 8); \
			countd = (4096 - (memld.addr1 & 4095)) / (BIT / 8); \
		} else { \
			counts = 1 + (memls.addr1 & 4095) / (BIT / 8); \
			countd = 1 + (memld.addr1 & 4095) / (BIT / 8); \
		} \
		if (counts < count) \
			count = counts; \
		if (countd < count) \
			count = countd; \
		if (cpu->cb.iomem_write_string && in_iomem(memld.addr1) && \
		    dir > 0  && in_iomem(memld.addr1 + count - 1) && \
		    (memls.addr1 | 4095) < cpu->phys_mem_size && \
		    !in_iomem(memls.addr1) && !in_iomem(memls.addr1 | 4095)) { \
			if (cpu->cb.iomem_write_string( \
				    cpu->cb.iomem, memld.addr1, \
				    cpu->phys_mem + memls.addr1, count * dir)) { \
				sreg ## ABIT(6, lreg ## ABIT(6) + count * dir); \
				sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
				sreg ## ABIT(1, cx - count); \
				cx = lreg ## ABIT(1); \
				continue; \
			} \
		} \
		for (uword i = 0; i <= count - 1; i++) { \
			store ## BIT(cpu, &memld, load ## BIT(cpu, &memls)); \
			memld.addr1 += dir; \
			memls.addr1 += dir; \
		} \
		sreg ## ABIT(6, lreg ## ABIT(6) + count * dir); \
		sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
		sreg ## ABIT(1, cx - count); \
		cx = lreg ## ABIT(1); \
	}

#define MOVS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { ldsistdi(BIT, 16) } else { ldsistdi(BIT, 32) } \
	} else { \
		if (adsz16) { MOVS_helper2(BIT, 16) } else { MOVS_helper2(BIT, 32) } \
	}

#define CMPS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax0, ax; \
	if (rep == 0) { \
		if (adsz16) { ldsilddi(BIT, 16) } else { ldsilddi(BIT, 32) } \
		cpu->cc.src1 = sext ## BIT(ax0); \
		cpu->cc.src2 = sext ## BIT(ax); \
		cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
		cpu->cc.op = CC_SUB; \
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	} else { \
		if (adsz16) { \
			while (lreg16(1)) { \
				ldsilddi(BIT, 16) \
				sreg16(1, lreg16(1) - 1); \
				cpu->cc.src1 = sext ## BIT(ax0); \
				cpu->cc.src2 = sext ## BIT(ax); \
				cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
				cpu->cc.op = CC_SUB; \
				cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
				bool zf = get_ZF(cpu); \
				if ((zf && rep == 2) || (!zf && rep == 1)) break; \
			} \
		} else { \
			while (lreg32(1)) { \
				ldsilddi(BIT, 32) \
				sreg32(1, lreg32(1) - 1); \
				cpu->cc.src1 = sext ## BIT(ax0); \
				cpu->cc.src2 = sext ## BIT(ax); \
				cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
				cpu->cc.op = CC_SUB; \
				cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
				bool zf = get_ZF(cpu); \
				if ((zf && rep == 2) || (!zf && rep == 1)) break; \
			} \
		} \
	}

#define STOSb() STOS_helper(8)
#define LODSb() LODS_helper(8)
#define SCASb() SCAS_helper(8)
#define MOVSb() MOVS_helper(8)
#define CMPSb() CMPS_helper(8)
#define STOS() if (opsz16) { STOS_helper(16) } else { STOS_helper(32) }
#define LODS() if (opsz16) { LODS_helper(16) } else { LODS_helper(32) }
#define SCAS() if (opsz16) { SCAS_helper(16) } else { SCAS_helper(32) }
#define MOVS() if (opsz16) { MOVS_helper(16) } else { MOVS_helper(32) }
#define CMPS() if (opsz16) { CMPS_helper(16) } else { CMPS_helper(32) }

#define indxstdi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 2, SEG_ES, lreg ## ABIT(7))); \
	ax = cpu->cb.io_read ## BIT(cpu->cb.io, lreg16(2)); \
	saddr ## BIT(&meml, ax); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define INS_helper2(BIT, ABIT) \
	OptAddr memld; \
	uword cx = lreg ## ABIT(1); \
	while (cx) { \
		TRY(translate ## BIT(cpu, &memld, 2, SEG_ES, lreg ## ABIT(7))); \
		if (memld.addr1 % (BIT / 8)) { \
			/* slow path */ \
			while (lreg ## ABIT(1)) { \
				indxstdi(BIT, ABIT) \
				sreg ## ABIT(1, lreg ## ABIT(1) - 1); \
			} \
			break; \
		} \
		uword count = cx; \
		int countd; \
		if (dir > 0) countd = (4096 - (memld.addr1 & 4095)) / (BIT / 8); \
		else countd = 1 + (memld.addr1 & 4095) / (BIT / 8); \
		if (countd < count) \
			count = countd; \
		if (cpu->cb.io_read_string && dir > 0 && \
		    (memld.addr1 | 4095) < cpu->phys_mem_size && \
		    !in_iomem(memld.addr1) && !in_iomem(memld.addr1 | 4095)) { \
			int count1 = cpu->cb.io_read_string( \
				cpu->cb.io, lreg16(2), \
				cpu->phys_mem + memld.addr1, dir, count); \
			if (count1 > 0) { \
				/* This writes guest memory without going through store8(), \
				 * so nothing else would notice code being loaded here. \
				 * A string input is rare; discard the line outright. */ \
				cpu->prefetch_base = (u32)-1; \
				SW_RANGE(cpu, memld.addr1, count1 * (BIT / 8), 'I'); \
				frank_diag_wp_range(memld.addr1, \
					count1 * (BIT / 8), 0x1451u, lreg16(2)); \
				count = count1; \
				sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
				sreg ## ABIT(1, cx - count); \
				cx = lreg ## ABIT(1); \
				continue; \
			} \
		} \
		for (uword i = 0; i <= count - 1; i++) { \
			ax = cpu->cb.io_read ## BIT(cpu->cb.io, lreg16(2)); \
			saddr ## BIT(&memld, ax); \
			memld.addr1 += dir; \
		} \
		sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
		sreg ## ABIT(1, cx - count); \
		cx = lreg ## ABIT(1); \
	}

#define INS_helper(BIT) \
	TRY(check_ioperm(cpu, lreg16(2), BIT)); \
	xdir ## BIT \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { indxstdi(BIT, 16) } else { indxstdi(BIT, 32) } \
	} else { \
		if (rep != 1 && rep != 2) THROW0(EX_UD); \
		if (adsz16) { INS_helper2(BIT, 16) } else { INS_helper2(BIT, 32) } \
	}

#define INSb() INS_helper(8)
#define INS() if (opsz16) { INS_helper(16) } else { INS_helper(32) }

#define ldsioutdx(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, curr_seg, lreg ## ABIT(6))); \
	ax = laddr ## BIT(&meml); \
	cpu->cb.io_write ## BIT(cpu->cb.io, lreg16(2), ax); \
	sreg ## ABIT(6, lreg ## ABIT(6) + dir);

#define OUTS_helper2(BIT, ABIT) \
	OptAddr memls; \
	uword cx = lreg ## ABIT(1); \
	while (cx) { \
		TRY(translate ## BIT(cpu, &memls, 1, curr_seg, lreg ## ABIT(6))); \
		if (memls.addr1 % (BIT / 8)) { \
			/* slow path */ \
			while (lreg ## ABIT(1)) { \
				ldsioutdx(BIT, ABIT) \
				sreg ## ABIT(1, lreg ## ABIT(1) - 1); \
			} \
			break; \
		} \
		uword count = cx; \
		int counts; \
		if (dir > 0) counts = (4096 - (memls.addr1 & 4095)) / (BIT / 8); \
		else counts = 1 + (memls.addr1 & 4095) / (BIT / 8); \
		if (counts < count) \
			count = counts; \
		if (cpu->cb.io_write_string && dir > 0 && \
		    (memls.addr1 | 4095) < cpu->phys_mem_size && \
		    !in_iomem(memls.addr1) && !in_iomem(memls.addr1 | 4095)) { \
			int count1 = cpu->cb.io_write_string( \
				cpu->cb.io, lreg16(2), \
				cpu->phys_mem + memls.addr1, dir, count); \
			if (count1 > 0) { \
				count = count1; \
				sreg ## ABIT(6, lreg ## ABIT(6) + count * dir); \
				sreg ## ABIT(1, cx - count); \
				cx = lreg ## ABIT(1); \
				continue; \
			} \
		} \
		for (uword i = 0; i <= count - 1; i++) { \
			ax = laddr ## BIT(&memls); \
			cpu->cb.io_write ## BIT(cpu->cb.io, lreg16(2), ax); \
			memls.addr1 += dir; \
		} \
		sreg ## ABIT(6, lreg ## ABIT(6) + count * dir); \
		sreg ## ABIT(1, cx - count); \
		cx = lreg ## ABIT(1); \
	}

#define OUTS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	TRY(check_ioperm(cpu, lreg16(2), BIT)); \
	xdir ## BIT \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { ldsioutdx(BIT, 16) } else { ldsioutdx(BIT, 32) } \
	} else { \
		if (rep != 1 && rep != 2) THROW0(EX_UD); \
		if (adsz16) { \
			OUTS_helper2(BIT, 16) \
		} else { \
			OUTS_helper2(BIT, 32) \
		} \
	}

#define OUTSb() OUTS_helper(8)
#define OUTS() if (opsz16) { OUTS_helper(16) } else { OUTS_helper(32) }

/*
 * Native JIT dispatch is attached to actual taken backward branches instead
 * of the cpu_exec1 loop head.  This is the key difference from v3:
 * sequential instructions pay exactly zero JIT-dispatch instructions.
 *
 * stepcount is cpu_exec1's remaining instruction budget and is intentionally
 * referenced by this macro at its expansion sites.
 */
#define NJ_HOT_BACKEDGE(d) do { (void)(d); } while (0)

#define JCXZb(i, li, _) \
	sword d = sext8(li(i)); \
	if (adsz16) { \
		if (lreg16(1) == 0) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	} else { \
		if (lreg32(1) == 0) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	}

#define LOOPb(i, li, _) \
	sword d = sext8(li(i)); \
	if (adsz16) { \
		sreg16(1, lreg16(1) - 1); \
		if (lreg16(1)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	} else { \
		sreg32(1, lreg32(1) - 1); \
		if (lreg32(1)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	}

#define LOOPEb(i, li, _) \
	sword d = sext8(li(i)); \
	if (adsz16) { \
		sreg16(1, lreg16(1) - 1); \
		if (lreg16(1) && get_ZF(cpu)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	} else { \
		sreg32(1, lreg32(1) - 1); \
		if (lreg32(1) && get_ZF(cpu)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	}

#define LOOPNEb(i, li, _) \
	sword d = sext8(li(i)); \
	if (adsz16) { \
		sreg16(1, lreg16(1) - 1); \
		if (lreg16(1) && !get_ZF(cpu)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	} else { \
		sreg32(1, lreg32(1) - 1); \
		if (lreg32(1) && !get_ZF(cpu)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	}

/*
 * The condition, chosen by a number the caller knows.
 *
 * This used to switch on the opcode byte at run time, which is a
 * sixteen-way jump table and an indirect branch for every conditional
 * jump - and conditional jumps are 23% of everything Tyrian executes,
 * against 0.7 mispredicted branches per guest instruction overall.  The
 * opcode already names the condition, so the dispatch that reached this
 * code has answered the question once already.  Passed a constant, the
 * compiler folds all of this away to the one test that is wanted.
 */
#define COND_CC(n) \
	int cond; \
	switch(n) { \
	case 0x0: cond =  get_OF(cpu); break; \
	case 0x1: cond = !get_OF(cpu); break; \
	case 0x2: cond =  get_CF(cpu); break; \
	case 0x3: cond = !get_CF(cpu); break; \
	case 0x4: cond =  get_ZF(cpu); break; \
	case 0x5: cond = !get_ZF(cpu); break; \
	case 0x6: cond =  get_ZF(cpu) ||  get_CF(cpu); break; \
	case 0x7: cond = !get_ZF(cpu) && !get_CF(cpu); break; \
	case 0x8: cond =  get_SF(cpu); break; \
	case 0x9: cond = !get_SF(cpu); break; \
	case 0xa: cond =  get_PF(cpu); break; \
	case 0xb: cond = !get_PF(cpu); break; \
	case 0xc: cond =  get_SF(cpu) != get_OF(cpu); break; \
	case 0xd: cond =  get_SF(cpu) == get_OF(cpu); break; \
	case 0xe: cond =  get_ZF(cpu) || get_SF(cpu) != get_OF(cpu); break; \
	case 0xf: cond = !get_ZF(cpu) && get_SF(cpu) == get_OF(cpu); break; \
	}

/*
 * A taken conditional jump keeps the prefetched line.
 *
 * It used to discard it, which is what made self-modifying code work before
 * prefetch_note_write() existed.  A loop body is usually shorter than the
 * thirty-two bytes held, so discarding it here meant refilling on every
 * iteration of every loop: a quarter of everything Tyrian executes is a
 * taken conditional jump, and one measured 126 cycles.
 *
 * Only this site changes.  JMP, CALL, RET and LOOP still discard, so a
 * decompressor that writes bytes and jumps to them is covered twice over.
 */
#define JCC_common(cc, d) \
	COND_CC(cc) \
	if (cond) { cpu->next_ip += d; NJ_HOT_BACKEDGE(d); }

#define COND() COND_CC(b1 & 0xf)

#define SETCCb(a, la, sa) \
	COND() \
	sa(a, cond);

#define JCCb(i, li, cc) \
	sword d = sext8(li(i)); \
	JCC_common(cc, d)

#define JCCw(i, li, cc) \
	sword d = sext16(li(i)); \
	JCC_common(cc, d)

#define JCCd(i, li, cc) \
	sword d = sext32(li(i)); \
	JCC_common(cc, d)

#define JMPb(i, li, _) \
	sword d = sext8(li(i)); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d);

#define JMPw(i, li, _) \
	sword d = sext16(li(i)); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d);

#define JMPd(i, li, _) \
	sword d = sext32(li(i)); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d);

#define JMPABSw(i, li, _) \
	cpu->next_ip = li(i); cpu->prefetch_base = (u32)-1;

#define JMPABSd(i, li, _) \
	cpu->next_ip = li(i); cpu->prefetch_base = (u32)-1;

#define JMPFAR(addr, seg) \
	if ((cpu->cr0 & 1) && !(cpu->flags & VM)) { \
		TRY(pmcall(cpu, opsz16, addr, seg, true)); \
	} else { \
	    TRY(set_seg(cpu, SEG_CS, seg)); \
	    cpu->next_ip = addr; \
	} \
	cpu->prefetch_base = (u32)-1;

#define CALLFAR(addr, seg) \
	if ((cpu->cr0 & 1) && !(cpu->flags & VM)) { \
		TRY(pmcall(cpu, opsz16, addr, seg, false)); \
	} else { \
	OptAddr meml1, meml2; \
	uword sp = lreg32(4); \
	if (opsz16) { \
		TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask)); \
		TRY(translate16(cpu, &meml2, 2, SEG_SS, (sp - 4) & sp_mask)); \
		set_sp(sp - 4, sp_mask); \
		saddr16(&meml1, cpu->seg[SEG_CS].sel); \
		saddr16(&meml2, cpu->next_ip); \
	} else { \
		TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask)); \
		TRY(translate32(cpu, &meml2, 2, SEG_SS, (sp - 8) & sp_mask)); \
		set_sp(sp - 8, sp_mask); \
		saddr32(&meml1, cpu->seg[SEG_CS].sel); \
		saddr32(&meml2, cpu->next_ip); \
	} \
	TRY(set_seg(cpu, SEG_CS, seg)); \
	cpu->next_ip = addr; \
	} \
	cpu->prefetch_base = (u32)-1;

#define CALLw(i, li, _) \
	sword d = sext16(li(i)); \
	uword sp = lreg32(4); \
	TRY(translate16(cpu, &meml, 2, SEG_SS, (sp - 2) & sp_mask)); \
	set_sp(sp - 2, sp_mask); \
	saddr16(&meml, cpu->next_ip); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1;

#define CALLd(i, li, _) \
	sword d = sext32(li(i)); \
	uword sp = lreg32(4); \
	TRY(translate32(cpu, &meml, 2, SEG_SS, (sp - 4) & sp_mask)); \
	set_sp(sp - 4, sp_mask); \
	saddr32(&meml, cpu->next_ip); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1;

#define CALLABSw(i, li, _) \
	uword nip = li(i); \
	uword sp = lreg32(4); \
	TRY(translate16(cpu, &meml, 2, SEG_SS, (sp - 2) & sp_mask)); \
	set_sp(sp - 2, sp_mask); \
	saddr16(&meml, cpu->next_ip); \
	cpu->next_ip = nip; cpu->prefetch_base = (u32)-1;

#define CALLABSd(i, li, _) \
	uword nip = li(i); \
	uword sp = lreg32(4); \
	TRY(translate32(cpu, &meml, 2, SEG_SS, (sp - 4) & sp_mask)); \
	set_sp(sp - 4, sp_mask); \
	saddr32(&meml, cpu->next_ip); \
	cpu->next_ip = nip; cpu->prefetch_base = (u32)-1;

#define FRANK_RET_WATCH() \
	{ \
		uint32_t dssp = cpu->seg[SEG_SS].base + (sp & sp_mask); \
		const uint8_t *dstk = (dssp >= 32 && \
			dssp + 32 <= (uint32_t)cpu->phys_mem_size) ? \
			cpu->phys_mem + dssp - 32 : 0; \
		frank_diag_ret(cpu->next_ip, cpu->seg[SEG_CS].base, cpu->ip, \
			       dssp, dstk); \
	}

#define RETw(i, li, _) \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 2 + li(i), sp_mask); \
		cpu->next_ip = laddr16(&meml); \
		FRANK_RET_WATCH() \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 4 + li(i), sp_mask); \
		cpu->next_ip = laddr32(&meml); \
		FRANK_RET_WATCH() \
	} \
	cpu->prefetch_base = (u32)-1;

#define RET() RETw(0, limm, 0)

static bool enter_helper(
	CPUI386 *cpu,
	bool opsz16,
	uword sp_mask,
	int level,
	int allocsz
) {
	assert(level != 0);
	uword temp;
	OptAddr meml1, memsrc;

	uword sp = lreg32(4);
	if (opsz16) {
		TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask));
		sp = (sp - 2) & sp_mask;
		set_sp(sp, sp_mask);
		saddr16(&meml1, lreg16(5));
		temp = lreg16(4);
	} else {
		TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask));
		sp = (sp - 4) & sp_mask;
		set_sp(sp, sp_mask);
		saddr32(&meml1, lreg32(5));
		temp = lreg32(4);
	}

	for (int i = 0; i < level - 1; i++) {
		if (opsz16) {
			if (sp_mask == 0xffff) {
				sreg16(5, lreg16(5) - 2);
			} else {
				sreg32(5, lreg32(5) - 2);
			}
			/* push word ptr [SS:BP] */
			TRY(translate16(cpu, &memsrc, 1, SEG_SS, lreg16(5) & sp_mask));
			sp = (sp - 2) & sp_mask;
			TRY(translate16(cpu, &meml1, 2, SEG_SS, sp));
			set_sp(sp, sp_mask);
			saddr16(&meml1, laddr16(&memsrc));
		} else {
			if (sp_mask == 0xffff) {
				sreg16(5, lreg16(5) - 4);
			} else {
				sreg32(5, lreg32(5) - 4);
			}
			/* push dword ptr [SS:EBP] */
			TRY(translate32(cpu, &memsrc, 1, SEG_SS, lreg32(5) & sp_mask));
			sp = (sp - 4) & sp_mask;
			TRY(translate32(cpu, &meml1, 2, SEG_SS, sp));
			set_sp(sp, sp_mask);
			saddr32(&meml1, laddr32(&memsrc));
		}
	}

	if (opsz16) {
		sp = (sp - 2) & sp_mask;
		TRY(translate16(cpu, &meml1, 2, SEG_SS, sp));
		set_sp((sp - allocsz) & sp_mask, sp_mask);
		saddr16(&meml1, temp);
		sreg16(5, temp);
	} else {
		sp = (sp - 4) & sp_mask;
		TRY(translate32(cpu, &meml1, 2, SEG_SS, sp));
		set_sp((sp - allocsz) & sp_mask, sp_mask);
		saddr32(&meml1, temp);
		sreg32(5, temp);
	}
	return true;
}

#define ENTER(i16, i8, l16, s16, l8, s8) \
	OptAddr meml1; \
	int level = l8(i8) % 32; \
	if (level == 0) { \
	uword sp = lreg32(4); \
	if (opsz16) { \
		TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask)); \
		set_sp(sp - 2 - l16(i16), sp_mask); \
		saddr16(&meml1, lreg16(5)); \
		sreg16(5, (sp - 2) & sp_mask); \
	} else { \
		TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask)); \
		set_sp(sp - 4 - l16(i16), sp_mask); \
		saddr32(&meml1, lreg32(5)); \
		sreg32(5, (sp - 4) & sp_mask); \
	} \
	} else { \
		TRY(enter_helper(cpu, opsz16, sp_mask, level, l16(i16))); \
	}

#define LEAVE() \
	uword sp = lreg32(5); \
	if (opsz16) { \
		TRY(translate16(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 2, sp_mask); \
		sreg16(5, laddr16(&meml)); \
	} else { \
		TRY(translate32(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 4, sp_mask); \
		sreg32(5, laddr32(&meml)); \
	}

#define SXXX(addr) \
	OptAddr meml1, meml2; \
	TRY(translate16(cpu, &meml1, 2, curr_seg, addr)); \
	TRY(translate32(cpu, &meml2, 2, curr_seg, addr + 2)); \

#define SGDT(addr) \
	SXXX(addr) \
	store16(cpu, &meml1, cpu->gdt.limit); \
	store32(cpu, &meml2, cpu->gdt.base);

#define SIDT(addr) \
	SXXX(addr) \
	store16(cpu, &meml1, cpu->idt.limit); \
	store32(cpu, &meml2, cpu->idt.base);

#define LXXX(addr) \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	OptAddr meml1, meml2; \
	TRY(translate16(cpu, &meml1, 1, curr_seg, addr)); \
	TRY(translate32(cpu, &meml2, 1, curr_seg, addr + 2)); \
	u16 limit = load16(cpu, &meml1); \
	u32 base = load32(cpu, &meml2); \
	if (opsz16) base &= 0xffffff;

#define LGDT(addr) \
	LXXX(addr) \
	cpu->gdt.base = base; \
	cpu->gdt.limit = limit;

#define LIDT(addr) \
	LXXX(addr) \
	cpu->idt.base = base; \
	cpu->idt.limit = limit;

#define LLDT(a, la, sa) \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	TRY(set_seg(cpu, SEG_LDT, la(a)));

#define SLDT(a, la, sa) \
	sa(a, cpu->seg[SEG_LDT].sel);

#define LTR(a, la, sa) \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	TRY(set_seg(cpu, SEG_TR, la(a)));

#define STR(a, la, sa) \
	sa(a, cpu->seg[SEG_TR].sel);

#define MOVFD() \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int rm = modrm & 7; \
	sreg32(rm, cpu->dr[reg]);

#define MOVTD() \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int rm = modrm & 7; \
	cpu->dr[reg] = lreg32(rm);

#define MOVFT() \
	TRY(fetch8(cpu, &modrm));
#define MOVTT() \
	TRY(fetch8(cpu, &modrm));

#define SMSW(addr, laddr, saddr) \
	saddr(addr, cpu->cr0 & 0xffff);

#define LMSW(addr, laddr, saddr) \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	cpu->cr0 = (cpu->cr0 & ((~0xf) | 1)) | (laddr(addr) & 0xf);

#define LSEGd(NAME, reg, addr, lreg32, sreg32, laddr32, saddr32) \
	OptAddr meml1, meml2; \
	if (adsz16) addr = addr & 0xffff; \
	TRY(translate32(cpu, &meml1, 1, curr_seg, addr)); \
	TRY(translate16(cpu, &meml2, 1, curr_seg, addr + 4)); \
	u32 r = load32(cpu, &meml1); \
	u32 s = load16(cpu, &meml2); \
	TRY(set_seg(cpu, SEG_ ## NAME, s)); \
	sreg32(reg, r);

#define LSEGw(NAME, reg, addr, lreg16, sreg16, laddr16, saddr16) \
	OptAddr meml1, meml2; \
	if (adsz16) addr = addr & 0xffff; \
	TRY(translate16(cpu, &meml1, 1, curr_seg, addr)); \
	TRY(translate16(cpu, &meml2, 1, curr_seg, addr + 2)); \
	u32 r = load16(cpu, &meml1); \
	u32 s = load16(cpu, &meml2); \
	TRY(set_seg(cpu, SEG_ ## NAME, s)); \
	sreg16(reg, r);

#define LESd(...) LSEGd(ES, __VA_ARGS__)
#define LSSd(...) LSEGd(SS, __VA_ARGS__)
#define LDSd(...) LSEGd(DS, __VA_ARGS__)
#define LFSd(...) LSEGd(FS, __VA_ARGS__)
#define LGSd(...) LSEGd(GS, __VA_ARGS__)
#define LESw(...) LSEGw(ES, __VA_ARGS__)
#define LSSw(...) LSEGw(SS, __VA_ARGS__)
#define LDSw(...) LSEGw(DS, __VA_ARGS__)
#define LFSw(...) LSEGw(FS, __VA_ARGS__)
#define LGSw(...) LSEGw(GS, __VA_ARGS__)

/*
 * Whether the permission bitmap has to be consulted at all.
 *
 * Real mode, and protected mode at a privilege level the flags already
 * allow, may touch any port, and two register tests say so.  check_ioperm()
 * below is a call that translates two task-segment addresses and loads from
 * them, and on the hot path it returns the same answer every time.  It
 * measured nineteen cycles against the ninety of the port read itself.
 */
#define ioperm_is_free(cpu) \
	(!((cpu)->cr0 & 1) || \
	 (!((cpu)->flags & VM) && (cpu)->cpl <= get_IOPL(cpu)))

static bool  check_ioperm (CPUI386 *cpu, int port, int bit)
{
	bool allow = true;
	if ((cpu->cr0 & 1) && (cpu->cpl > get_IOPL(cpu) || (cpu->flags & VM))) {
		allow = false;
		if (cpu->seg[SEG_TR].limit >= 103) {
			OptAddr meml;
			TRY(translate(cpu, &meml, 1, SEG_TR, 102, 2, 0));
			u32 iobase = load16(cpu, &meml);
			if (iobase + port / 8 < cpu->seg[SEG_TR].limit) {
				TRY(translate(cpu, &meml, 1, SEG_TR, iobase + port / 8, 2, 0));
				u16 perm = load16(cpu, &meml);
				int len = bit / 8;
				unsigned bit_index = port & 0x7;
				unsigned mask = (1 << len) - 1;
				if (!((perm >> bit_index) & mask))
					allow = true;
			}
		}
	}

	if (!allow) THROW(EX_GP, 0);
	return true;
}

/*
 * What one port read costs, split between deciding whether it is allowed
 * and performing it.  The instruction mix says one guest instruction in
 * twelve is this, so an answer here is worth more than a guess anywhere
 * else.  The cycle counter is one system register read.
 */
#if defined(CIRCLE_PC_STATS)
uint64_t g_io_perm_cycles, g_io_read_cycles;
uint32_t g_io_count;
#define IO_CYC(v) do { asm volatile ("mrs %0, pmccntr_el0" : "=r"(v)); } while (0)
#define INb(a, b, la, sa, lb, sb) \
	int port = lb(b); \
	uint64_t _c0, _c1, _c2; \
	IO_CYC(_c0); \
	if (unlikely(!ioperm_is_free(cpu))) { TRY(check_ioperm(cpu, port, 8)); } \
	IO_CYC(_c1); \
	sa(a, likely(port == cpu->io_fast_port) \
	      ? cpu->io_fast_fn(cpu->io_fast_arg) \
	      : cpu->cb.io_read8(cpu->cb.io, port)); \
	IO_CYC(_c2); \
	g_io_perm_cycles += _c1 - _c0; \
	g_io_read_cycles += _c2 - _c1; \
	g_io_count++;
#else
#define INb(a, b, la, sa, lb, sb) \
	int port = lb(b); \
	if (unlikely(!ioperm_is_free(cpu))) { TRY(check_ioperm(cpu, port, 8)); } \
	sa(a, likely(port == cpu->io_fast_port) \
	      ? cpu->io_fast_fn(cpu->io_fast_arg) \
	      : cpu->cb.io_read8(cpu->cb.io, port));
#endif

#define INw(a, b, la, sa, lb, sb) \
	int port = lb(b); \
	if (unlikely(!ioperm_is_free(cpu))) { TRY(check_ioperm(cpu, port, 16)); } \
	sa(a, cpu->cb.io_read16(cpu->cb.io, port));

#define INd(a, b, la, sa, lb, sb) \
	int port = lb(b); \
	if (unlikely(!ioperm_is_free(cpu))) { TRY(check_ioperm(cpu, port, 32)); } \
	sa(a, cpu->cb.io_read32(cpu->cb.io, port));

#define OUTb(a, b, la, sa, lb, sb) \
	int port = la(a); \
	if (unlikely(!ioperm_is_free(cpu))) { TRY(check_ioperm(cpu, port, 8)); } \
	cpu->cb.io_write8(cpu->cb.io, port, lb(b));

#define OUTw(a, b, la, sa, lb, sb) \
	int port = la(a); \
	if (unlikely(!ioperm_is_free(cpu))) { TRY(check_ioperm(cpu, port, 16)); } \
	cpu->cb.io_write16(cpu->cb.io, port, lb(b));

#define OUTd(a, b, la, sa, lb, sb) \
	int port = la(a); \
	if (unlikely(!ioperm_is_free(cpu))) { TRY(check_ioperm(cpu, port, 32)); } \
	cpu->cb.io_write32(cpu->cb.io, port, lb(b));

#define CLTS() \
	cpu->cr0 &= ~(1 << 3);

#define ESC() \
	if (cpu->cr0 & 0xc) THROW0(EX_NM); \
	else { \
		TRY(fetch8(cpu, &modrm)); \
		int mod = modrm >> 6; \
		int rm = modrm & 7; \
		int op = b1 - 0xd8; \
		int group = (modrm >> 3) & 7; \
		if (mod != 3) { \
			TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
			if (cpu->fpu) { \
				TRY(fpu_exec2(cpu->fpu, cpu, opsz16, op, group, curr_seg, addr)); \
			} \
		} else { \
			int reg = modrm & 7; \
			if (cpu->fpu) { \
				TRY(fpu_exec1(cpu->fpu, cpu, op, group, reg)); \
			} \
		} \
	}

#define WAIT() \
	if ((cpu->cr0 & 0xa) == 0xa) THROW0(EX_NM);

// ...
#define AAD(i, li, _) \
	u8 al = lreg8(0); \
	u8 ah = lreg8(4); \
	u8 imm = li(i); \
	u8 res = al + ah * imm; \
	sreg8(0, res); \
	sreg8(4, 0); \
	cpu->flags &= ~(OF | AF | CF); /* undocumented */ \
	cpu->cc.dst = sext8(res); \
	cpu->cc.mask = ZF | SF | PF;

#define AAM(i, li, _) \
	u8 al = lreg8(0); \
	u8 imm = li(i); \
	u8 res = al % imm; \
	sreg8(4, al / imm); \
	sreg8(0, res); \
	cpu->flags &= ~(OF | AF | CF); /* undocumented */ \
	cpu->cc.dst = sext8(res); \
	cpu->cc.mask = ZF | SF | PF;

#define SALC() \
	if (get_CF(cpu)) sreg8(0, 0xff); else sreg8(0, 0x00);

#define XLAT() \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	if (adsz16) { \
		addr = lreg16(3) + lreg8(0); \
		addr &= 0xffff; \
		TRY(translate8(cpu, &meml, 1, curr_seg, addr)); \
		sreg8(0, laddr8(&meml)); \
	} else { \
		addr = lreg32(3) + lreg8(0); \
		TRY(translate8(cpu, &meml, 1, curr_seg, addr)); \
		sreg8(0, laddr8(&meml)); \
	}

#define DAA() \
	u8 al = lreg8(0); \
	int cf = get_CF(cpu); \
	cpu->flags &= ~CF; \
	if ((al & 0xf) > 9 || get_AF(cpu)) { \
		sreg8(0, al + 6); \
		if (cf || al > 0xff - 6) cpu->flags |= CF; \
		cpu->flags |= AF; \
	} else { \
		cpu->flags &= ~AF; \
	} \
	if (al > 0x99 || cf) { \
		sreg8(0, lreg8(0) + 0x60); \
		cpu->flags |= CF; \
	} \
	cpu->cc.dst = sext8(lreg8(0)); \
	cpu->cc.mask = ZF | SF | PF;

#define DAS() \
	u8 al = lreg8(0); \
	int cf = get_CF(cpu); \
	cpu->flags &= ~CF; \
	if ((al & 0xf) > 9 || get_AF(cpu)) { \
		sreg8(0, al - 6); \
		if (cf || al < 6) cpu->flags |= CF; \
		cpu->flags |= AF; \
	} else { \
		cpu->flags &= ~AF; \
	} \
	if (al > 0x99 || cf) { \
		sreg8(0, lreg8(0) - 0x60); \
		cpu->flags |= CF; \
	} \
	cpu->cc.dst = sext8(lreg8(0)); \
	cpu->cc.mask = ZF | SF | PF;

#define AAA() \
	if ((lreg8(0) & 0xf) > 9 || get_AF(cpu)) { \
		sreg16(0, lreg16(0) + 0x106); \
		cpu->flags |= AF | CF; \
	} else { \
		cpu->flags &= ~(AF | CF); \
	} \
	cpu->cc.mask = ZF | SF | PF; \
	sreg8(0, lreg8(0) & 0xf);

#define AAS() \
	if ((lreg8(0) & 0xf) > 9 || get_AF(cpu)) { \
		sreg16(0, lreg16(0) - 6); \
		sreg8(4, lreg8(4) - 1); \
		cpu->flags |= AF | CF; \
	} else { \
		cpu->flags &= ~(AF | CF); \
	} \
	cpu->cc.mask = ZF | SF | PF; \
	sreg8(0, lreg8(0) & 0xf);

static bool larsl_helper(CPUI386 *cpu, int sel, uword *ar, uword *sl, int *zf)
{
	sel = sel & 0xffff;

	if (!(cpu->cr0 & 1) || (cpu->flags & VM))
		THROW0(EX_UD);

	if ((sel & ~0x3) == 0) {
		*zf = 0;
		return true;
	}

	uword w1, w2;
	if (!read_desc(cpu, sel, &w1, &w2)) {
		*zf = 0;
		return true;
	}

	if ((w2 >> 12) & 1) {
		int dpl = (w2 >> 13) & 0x3;
		if (((w2 >> 10) & 0x3) != 0x3 && (cpu->cpl > dpl || (sel & 0x3) > dpl)) {
			*zf = 0;
			return true;
		}
	} else {
		int type = (w2 >> 8) & 0xf;
		if (ar) {
			switch (type) {
			case 0: case 6: case 7: case 8: case 10:
			case 13: case 14: case 15:
				*zf = 0;
				return true;
			}
		}
		if (sl) {
			switch (type) {
			case 0: case 4: case 5: case 6: case 7: case 8:
			case 10: case 12: case 13: case 14: case 15:
				*zf = 0;
				return true;
			}
		}
	}

	if (ar)
		*ar = w2 & 0x00ffff00;
	if (sl) {
		*sl = (w2 & 0xf0000) | (w1 & 0xffff);
		if (w2 & 0x00800000)
			*sl = (*sl << 12) | 0xfff;
	}

	*zf = 1;
	return true;
}

static bool verrw_helper(CPUI386 *cpu, int sel, int wr, int *zf)
{
	sel = sel & 0xffff;

	if (!(cpu->cr0 & 1) || (cpu->flags & VM))
		THROW0(EX_UD);

	if ((sel & ~0x3) == 0) {
		*zf = 0;
		return true;
	}

	uword w1, w2;
	if (!read_desc(cpu, sel, &w1, &w2)) {
		*zf = 0;
		return true;
	}

	if (((w2 >> 12) & 0x1) == 0) {
		*zf = 0;
		return true;
	}

	int dpl = (w2 >> 13) & 0x3;
	if (((w2 >> 10) & 0x3) != 0x3 && (cpu->cpl > dpl || (sel & 0x3) > dpl)) {
		*zf = 0;
		return true;
	}

	if (((w2 >> 11) & 0x1) == 0) {
		/* data */
		if (wr && ((w2 >> 9) & 0x1) == 0) {
			*zf = 0;
			return true;
		}
	} else {
		/* code */
		if (!wr && ((w2 >> 9) & 0x1) == 0) {
			*zf = 0;
			return true;
		}
	}

	*zf = 1;
	return true;
}

#define LARdw(a, b, la, sa, lb, sb) \
	uword res; \
	int zf; \
	TRY(larsl_helper(cpu, lb(b), &res, NULL, &zf)); \
	if (zf) { \
		sa(a, res); \
		cpu->flags |= ZF; \
	} else { \
		cpu->flags &= ~ZF; \
	} \
	cpu->cc.mask &= ~ZF;
#define LARww LARdw

#define LSLdw(a, b, la, sa, lb, sb) \
	uword res; \
	int zf; \
	TRY(larsl_helper(cpu, lb(b), NULL, &res, &zf)); \
	if (zf) { \
		sa(a, res); \
		cpu->flags |= ZF; \
	} else { \
		cpu->flags &= ~ZF; \
	} \
	cpu->cc.mask &= ~ZF;
#define LSLww LSLdw

#define VERR(a, la, sa) \
	int zf; \
	TRY(verrw_helper(cpu, la(a), 0, &zf)); \
	cpu->cc.mask &= ~ZF; \
	SET_BIT(cpu->flags, zf, ZF);

#define VERW(a, la, sa) \
	int zf; \
	TRY(verrw_helper(cpu, la(a), 1, &zf)); \
	cpu->cc.mask &= ~ZF; \
	SET_BIT(cpu->flags, zf, ZF);

#define ARPL(a, b, la, sa, lb, sb) \
	if (!(cpu->cr0 & 1) || (cpu->flags & VM)) THROW0(EX_UD); \
	u16 dst = la(a); \
	u16 src = lb(b); \
	if ((dst & 3) < (src & 3)) { \
		cpu->flags |= ZF; \
		sa(a, ((dst & ~3) | (src & 3))); \
	} else { \
		cpu->flags &= ~ZF; \
	} \
	cpu->cc.mask &= ~ZF;

#define GvMa GvM
#define BOUND_helper(BIT, a, b, la, sa, lb, sb) \
	OptAddr meml1, meml2; \
	s ## BIT idx = la(a); \
	uword addr1 = lb(b); \
	TRY(translate ## BIT(cpu, &meml1, 3, curr_seg, addr1)); \
	TRY(translate ## BIT(cpu, &meml2, 3, curr_seg, addr1 + BIT / 8)); \
	s ## BIT lo = load ## BIT(cpu, &meml1); \
	s ## BIT hi = load ## BIT(cpu, &meml2); \
	if (idx < lo || idx > hi) { \
		if (cpu->cr0 & 1) THROW0(EX_BR); \
	}
#define BOUNDw(...) BOUND_helper(16, __VA_ARGS__)
#define BOUNDd(...) BOUND_helper(32, __VA_ARGS__)

// 486...
#define CMPXCH_helper(BIT, a, b, la, sa, lb, sb) \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.src2 = sext ## BIT(lreg ## BIT(0)); \
	cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
	cpu->cc.op = CC_SUB; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	if (cpu->cc.dst == 0) sa(a, lb(b)); else sreg ## BIT(0, cpu->cc.src1); 

#define XADD_helper(BIT, a, b, la, sa, lb, sb) \
	u ## BIT dst = la(a); \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.src2 = sext ## BIT(lb(b)); \
	cpu->cc.dst = sext ## BIT(cpu->cc.src1 + cpu->cc.src2); \
	cpu->cc.op = CC_ADD; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sb(b, dst); \
	sa(a, cpu->cc.dst);

#define CMPXCHb(...) CMPXCH_helper(8, __VA_ARGS__)
#define CMPXCHw(...) CMPXCH_helper(16, __VA_ARGS__)
#define CMPXCHd(...) CMPXCH_helper(32, __VA_ARGS__)
#define XADDb(...) XADD_helper(8, __VA_ARGS__)
#define XADDw(...) XADD_helper(16, __VA_ARGS__)
#define XADDd(...) XADD_helper(32, __VA_ARGS__)

#define INVLPG(addr) tlb_clear(cpu);

#define BSWAPw(a, la, sa) THROW0(EX_UD);

#define BSWAPd(a, la, sa) \
	u32 src = la(a); \
	u32 dst = ((src & 0xff) << 24) | (((src >> 8) & 0xff) << 16) | (((src >> 16) & 0xff) << 8) | ((src >> 24) & 0xff); \
	sa(a, dst);

#define WBINVD()

// 586 and later...
#define UD0() THROW0(EX_UD);

#if defined(I386_ENABLE_SSE3)
#define CPUID_SIMD_FEATURE2 0x1
#else
#define CPUID_SIMD_FEATURE2 0x0
#endif
#if defined(I386_ENABLE_SSE2)
#define CPUID_SIMD_FEATURE 0x7800000
#elif defined(I386_ENABLE_SSE)
#define CPUID_SIMD_FEATURE 0x3800000
#elif defined(I386_ENABLE_MMX)
#define CPUID_SIMD_FEATURE 0x800000
#else
#define CPUID_SIMD_FEATURE 0x0
#endif

#define CPUID() \
	switch (REGi(0)) { \
	case 0: \
		REGi(0) = 1; \
		REGi(3) = 0x594e4954; \
		REGi(2) = 0x20363833; \
		REGi(1) = 0x20555043; \
		break; \
	case 1: \
		REGi(0) = 0 | (0 << 4) | (cpu->gen << 8); \
		REGi(3) = 0; \
		REGi(2) = 0x100; \
		REGi(1) = 0; \
		if (cpu->fpu) REGi(2) |= 1; \
		if (cpu->gen > 5) REGi(2) |= 0x8820; \
		if (cpu->gen > 5 && cpu->fpu) { \
			REGi(2) |= CPUID_SIMD_FEATURE; \
			REGi(1) |= CPUID_SIMD_FEATURE2; \
		} \
		break; \
	default: \
		REGi(0) = 0; \
		REGi(3) = 0; \
		REGi(2) = 0; \
		REGi(1) = 0; \
		break; \
	}

#include <hardware/timer.h>
#define RDTSC() \
	uint64_t tsc = time_us_64(); \
	REGi(0) = tsc; \
	REGi(2) = tsc >> 32;

#define Mq Ms
#define CMPXCH8B(addr) \
	OptAddr meml1, meml2; \
	TRY(translate32(cpu, &meml1, 3, curr_seg, addr)); \
	TRY(translate32(cpu, &meml2, 3, curr_seg, addr + 4)); \
	uword lo = load32(cpu, &meml1); \
	uword hi = load32(cpu, &meml2); \
	if (REGi(0) == lo && REGi(2) == hi) { \
		cpu->flags |= ZF; \
		store32(cpu, &meml1, REGi(3)); \
		store32(cpu, &meml2, REGi(1)); \
	} else { \
		cpu->flags &= ~ZF; \
		REGi(0) = lo; \
		REGi(2) = hi; \
	} \
	cpu->cc.mask &= ~ZF;

#define CMOVw(a, b, la, sa, lb, sb) \
	COND() \
	if (cond) sa(a, lb(b));

#define CMOVd(a, b, la, sa, lb, sb) \
	COND() \
	if (cond) sa(a, lb(b));

#define WRMSR() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	switch (REGi(1)) { \
	case 0x174: cpu->sysenter.cs = REGi(0); break; \
	case 0x176: cpu->sysenter.eip = REGi(0); break; \
	case 0x175: cpu->sysenter.esp = REGi(0); break; \
	default: cpu_debug(cpu); THROW(EX_GP, 0); \
	}

#define RDMSR() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	switch (REGi(1)) { \
	case 0x174: REGi(0) = cpu->sysenter.cs; REGi(2) = 0; break; \
	case 0x176: REGi(0) = cpu->sysenter.eip; REGi(2) = 0; break; \
	case 0x175: REGi(0) = cpu->sysenter.esp; REGi(2) = 0; break; \
	default: cpu_debug(cpu); THROW(EX_GP, 0); \
	}

static void __sysenter(CPUI386 *cpu, int pl, int cs)
{
	cpu->seg[SEG_CS].sel = (cs & 0xfffc) | pl;
	cpu->seg[SEG_CS].base = 0;
	cpu->seg[SEG_CS].limit = 0xffffffff;
	cpu->seg[SEG_CS].flags = SEG_D_BIT | 0x5b | (pl << 5);
	seg_set_bounds(cpu, SEG_CS);
	cpu->cpl = pl;
	cpu->code16 = false;
	cpu->sp_mask = 0xffffffff;
	cpu->seg[SEG_SS].sel = ((cs + 8) & 0xfffc) | pl;
	cpu->seg[SEG_SS].base = 0;
	cpu->seg[SEG_SS].limit = 0xffffffff;
	cpu->seg[SEG_SS].flags = SEG_B_BIT | 0x53 | (pl << 5);
	seg_set_bounds(cpu, SEG_SS);
}

#define SYSENTER() \
	if (!(cpu->cr0 & 1) || (cpu->sysenter.cs & ~0x3) == 0) THROW(EX_GP, 0); \
	cpu->flags &= ~(VM | IF); \
	__sysenter(cpu, 0, cpu->sysenter.cs); \
	REGi(4) = cpu->sysenter.esp; \
	cpu->next_ip = cpu->sysenter.eip; cpu->prefetch_base = (u32)-1;

#define SYSEXIT() \
	if (!(cpu->cr0 & 1) || (cpu->sysenter.cs & ~0x3) == 0 || cpu->cpl) THROW(EX_GP, 0); \
	__sysenter(cpu, 3, cpu->sysenter.cs + 16); \
	REGi(4) = REGi(1); \
	cpu->next_ip = REGi(2); cpu->prefetch_base = (u32)-1;

#if defined(I386_ENABLE_MMX) || defined(I386_ENABLE_SSE)
#define SIMD_i386_c
#include "simd.inc"
#undef SIMD_i386_c
#endif

static void pc_note(CPUI386 *cpu);
#if defined(CPU_XFER_TRACE)
static void xfer_note(CPUI386 *cpu);
#endif
static bool pmcall(CPUI386 *cpu, bool opsz16, uword addr, int sel, bool isjmp);
void gate_note_call(CPUI386 *cpu, int sel, int newcs, int gt, int wc,
		    int newdpl, uword oldsp);
static bool IRAM_ATTR pmret(CPUI386 *cpu, bool opsz16, int off, bool isiret);

#define ARGCOUNT_IMPL(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, ...) _17
#define ARGCOUNT(...) ARGCOUNT_IMPL(~, ## __VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define PASTE0(a, b) a ## b
#define PASTE(a, b) PASTE0(a, b)
#define C_1(_1)      CX(_1)
#define C_2(_1, ...) CX(_1) C_1(__VA_ARGS__)
#define C_3(_1, ...) CX(_1) C_2(__VA_ARGS__)
#define C_4(_1, ...) CX(_1) C_3(__VA_ARGS__)
#define C_5(_1, ...) CX(_1) C_4(__VA_ARGS__)
#define C_6(_1, ...) CX(_1) C_5(__VA_ARGS__)
#define C_7(_1, ...) CX(_1) C_6(__VA_ARGS__)
#define C_8(_1, ...) CX(_1) C_7(__VA_ARGS__)
#define C_9(_1, ...) CX(_1) C_8(__VA_ARGS__)
#define C_10(_1, ...) CX(_1) C_9(__VA_ARGS__)
#define C_11(_1, ...) CX(_1) C_10(__VA_ARGS__)
#define C_12(_1, ...) CX(_1) C_11(__VA_ARGS__)
#define C_13(_1, ...) CX(_1) C_12(__VA_ARGS__)
#define C_14(_1, ...) CX(_1) C_13(__VA_ARGS__)
#define C_15(_1, ...) CX(_1) C_14(__VA_ARGS__)
#define C_16(_1, ...) CX(_1) C_15(__VA_ARGS__)
#define C(...) PASTE(C_, ARGCOUNT(__VA_ARGS__))(__VA_ARGS__)







/*
 * The diagnostic notes above live inside #if NATIVE_JIT, but call_isr()
 * uses NJ_ISR_NOTE() unconditionally. Without the JIT the macro would be
 * taken for a function and the link would fail, so give it a no-op form.
 */
#ifndef NJ_ISR_NOTE
#define NJ_ISR_NOTE(why, w1v, w2v) do { } while (0)
#endif

#if defined(CIRCLE_PC_STATS)
uint32_t g_opcode_hist[256];
/*
 * What each opcode costs, closed out at the top of the next instruction.
 *
 * Under its own switch rather than the general statistics, because it is one
 * counter read on the hot path - about four per cent - and that is enough to
 * make its numbers incomparable with measurements taken without it.  Build
 * with -DCIRCLE_PC_OPCOST=1 when asking which opcode to look at next; it is
 * what showed that a conditional jump cost 126 cycles.
 */
uint64_t g_opcode_cycles[256];
#if defined(CIRCLE_PC_OPCOST)
static uint64_t g_op_t0;
static int g_op_prev = -1;
#endif

/*
 * Where the guest is, sampled rather than counted.
 *
 * The opcode mix says what is being run but not by whom, and the two
 * questions have different answers: a machine that is busy for three minutes
 * with no disk activity is running some particular loop, and the mix alone
 * cannot tell a decompressor from a wait.  So every sixty-fourth instruction
 * the linear address is dropped into a small table, sixteen bytes to a slot,
 * which is fine enough to name a loop and coarse enough to survive in a
 * few hundred bytes.
 *
 * The table is direct-mapped and an address that finds someone else there
 * takes a count off them rather than evicting at once, so a genuinely hot
 * address survives a stream of cold ones colliding with it.  It is an
 * approximation and the counts are not exact; what it is asked for is which
 * handful of addresses matter, and for that it is enough.
 */
#define HOT_SLOTS 512
static uint32_t g_hot_tag[HOT_SLOTS];
static uint32_t g_hot_count[HOT_SLOTS];
static uint32_t g_hot_samples;

/*
 * The code itself, taken from the prefetch as the guest runs through it.
 *
 * Naming the address is only half an answer - what matters is what the
 * routine does, and that means its bytes.  Reading guest memory from outside
 * would mean walking the page tables by hand; the interpreter has already
 * done that work, so the line it is executing from is simply copied out as
 * it passes.  The window is set to the hottest address by the last report,
 * so the second window after a run collects the code the first one found.
 */
static uint32_t g_hot_code_base;
static uint8_t g_hot_code[256];
static uint8_t g_hot_code_have;

static inline void hot_note(CPUI386 *cpu, uint32_t linear)
{
	const uint32_t key = linear >> 4;
	const uint32_t i = (key ^ (key >> 9) ^ (key >> 18)) & (HOT_SLOTS - 1);
	g_hot_samples++;
	if (g_hot_code_have != 0xff) {
		const uint32_t line = linear & ~31u;
		const uint32_t at = line - g_hot_code_base;
		if (at < 256 && !(g_hot_code_have & (1u << (at >> 5))) &&
		    PREFETCH_HIT(linear)) {
			memcpy(g_hot_code + at, cpu->prefetch, 32);
			g_hot_code_have |= (uint8_t)(1u << (at >> 5));
		}
	}
	if (g_hot_tag[i] == key) { g_hot_count[i]++; return; }
	if (g_hot_count[i]) { g_hot_count[i]--; return; }
	g_hot_tag[i] = key;
	g_hot_count[i] = 1;
}

/* Return which 32-byte blocks have actually been collected.  A hot loop may
 * occupy only one line and never execute all eight lines in its window. */
int i386_hot_code(uint32_t *base, const uint8_t **bytes)
{
	*base = g_hot_code_base;
	*bytes = g_hot_code;
	return g_hot_code_have;
}

int i386_hot_report(char *out, int len)
{
	int n = 0;
	uint32_t total = g_hot_samples;
	for (int k = 0; k < 6 && n < len; k++) {
		int best = -1;
		uint32_t bv = 0;
		for (int i = 0; i < HOT_SLOTS; i++)
			if (g_hot_count[i] > bv) { bv = g_hot_count[i]; best = i; }
		if (best < 0 || !bv || !total) break;
		if (!k && g_hot_code_have != 0xff) {
			/* Aim the next window's collection at whatever came
			 * top of this one. */
			const uint32_t hot = g_hot_tag[best] << 4;
			if ((hot & ~255u) != g_hot_code_base) {
				g_hot_code_base = hot & ~255u;
				g_hot_code_have = 0;
			}
		}
		n += snprintf(out + n, (size_t)(len - n), "%08lx:%lu%% ",
			      (unsigned long)g_hot_tag[best] << 4,
			      (unsigned long)(bv * 100 / total));
		g_hot_count[best] = 0;
	}
	memset(g_hot_tag, 0, sizeof g_hot_tag);
	memset(g_hot_count, 0, sizeof g_hot_count);
	g_hot_samples = 0;
	return n;
}
#endif

static bool IRAM_ATTR_CPU_EXEC1 cpu_exec1(CPUI386 *cpu, int stepcount)
{
#ifndef I386_OPT2
#define eswitch(b) switch(b)
#define ecase(a)   case a
#define ebreak     break
#define edefault   default
#define default_ud cpu_debug(cpu); THROW0(EX_UD)
#undef CX
#define CX(_1) case _1:
#else
#define eswitch(b)
#define ecase(a)   f ## a
#define ebreak     continue
#define edefault   f0xf1
#define default_ud THROW0(EX_UD)
#undef CX
#define CX(_1) f ## _1:
#endif

	u8 b1;
	u8 modrm;
	OptAddr meml;
	uword addr;
	for (; stepcount > 0; ) {
	stepcount--;
	bool code16 = cpu->code16;
	uword sp_mask = cpu->sp_mask;

	if (code16) cpu->next_ip &= 0xffff;
	cpu->ip = cpu->next_ip;
#if defined(CPU_STACK_WATCH)
	if (unlikely(sw_track_on)) sw_track(cpu);
#endif
#if defined(CPU_XFER_TRACE)
	if (unlikely(cpu->seg[SEG_CS].base != xfer_last_base ||
		     (cpu->cr0 & 1) != xfer_last_pe))
		xfer_note(cpu);
	xfer_last_ip = cpu->ip;
#endif
#ifndef CIRCLE_PC_NO_UART
	/* Diagnostic only, and the sampler cannot be read back without the
	 * serial link - so a build made for the card carries no branch. */
	if (!(cpu->cycle & 0xfff)) pc_note(cpu);
#endif
#if defined(CIRCLE_PC_STATS)
	if (!(cpu->cycle & 63))
		hot_note(cpu, cpu->seg[SEG_CS].base + cpu->ip);
#endif
	frank_diag_ip(cpu->ip, cpu->seg[SEG_CS].base, REGi(4) & (uint32_t)cpu->sp_mask);
	frank_diag_shadow(cpu->phys_mem);
#if BB_PROFILE
	{ static uint32_t bb_prev; bb_note(cpu->ip, bb_prev); bb_prev = cpu->ip; }
#endif
#ifdef TINY386_JIT
	{
		/* Translated code runs whole instructions, so the step count is
			 * what it covered; the loop picks up wherever it stopped. */
		jit_profile(cpu);
		/*
		 * Ask only now and then.
		 *
		 * Looking a block up means indexing two tables that together are
		 * larger than the first level data cache, and doing that for every
		 * guest instruction cost more than the translator saved.  A loop
		 * worth translating comes round thousands of times, so it is found
		 * just as surely by asking every sixteenth instruction, and once a
		 * block is entered it stays inside itself anyway.
		 */
		int done = (cpu->cycle & 15) ? 0 : jit_run(cpu);
		if (done > 0) {
			cpu->cycle += done - 1;
			stepcount -= done - 1;
			continue;
		}
	}
#endif
	TRY(fetch8(cpu, &b1));
#if defined(CIRCLE_PC_STATS)
	/* What the guest actually runs, so that optimising the interpreter can
	 * start from its own instruction mix rather than from a guess about which
	 * opcodes matter.  One increment on the hot path, and only when the
	 * periodic statistics are built. */
#if defined(CIRCLE_PC_OPCOST)
	{
		uint64_t _t;
		asm volatile ("mrs %0, pmccntr_el0" : "=r"(_t));
		if (g_op_prev >= 0) g_opcode_cycles[g_op_prev] += _t - g_op_t0;
		g_op_t0 = _t;
		g_op_prev = b1;
	}
#endif
	g_opcode_hist[b1]++;
#endif
#if DEBUG_CPU
	opcode = b1;
#endif
	cpu->cycle++;

#ifndef I386_OPT1
	if (verbose) {
		cpu_debug(cpu);
	}
#endif
	// prefix
	bool opsz16 = code16;
	bool adsz16 = code16;
	int rep = 0;
	/*bool lock = false;*/
	int curr_seg = -1;
#ifndef I386_OPT2
	for (;;) {
#define HANDLE_PREFIX(C, STMT) \
		if (b1 == C) { \
			STMT; \
			TRY(fetch8(cpu, &b1)); \
			continue; \
		}
		HANDLE_PREFIX(0x26, curr_seg = SEG_ES)
		HANDLE_PREFIX(0x2e, curr_seg = SEG_CS)
		HANDLE_PREFIX(0x36, curr_seg = SEG_SS)
		HANDLE_PREFIX(0x3e, curr_seg = SEG_DS)
		HANDLE_PREFIX(0x64, curr_seg = SEG_FS)
		HANDLE_PREFIX(0x65, curr_seg = SEG_GS)
		HANDLE_PREFIX(0x66, opsz16 = !code16)
		HANDLE_PREFIX(0x67, adsz16 = !code16)
		HANDLE_PREFIX(0xf3, rep = 1) // REP
		HANDLE_PREFIX(0xf2, rep = 2) // REPNE
		HANDLE_PREFIX(0xf0, /*lock = true*/)
#undef HANDLE_PREFIX
		break;
	}
#else
	static const void *pfxlabel[] = {
/* 0x00 */	&&f0x00, &&f0x01, &&f0x02, &&f0x03, &&f0x04, &&f0x05, &&f0x06, &&f0x07,
/* 0x08 */	&&f0x08, &&f0x09, &&f0x0a, &&f0x0b, &&f0x0c, &&f0x0d, &&f0x0e, &&f0x0f,
/* 0x10 */	&&f0x10, &&f0x11, &&f0x12, &&f0x13, &&f0x14, &&f0x15, &&f0x16, &&f0x17,
/* 0x18 */	&&f0x18, &&f0x19, &&f0x1a, &&f0x1b, &&f0x1c, &&f0x1d, &&f0x1e, &&f0x1f,
/* 0x20 */	&&f0x20, &&f0x21, &&f0x22, &&f0x23, &&f0x24, &&f0x25, &&pfx26, &&f0x27,
/* 0x28 */	&&f0x28, &&f0x29, &&f0x2a, &&f0x2b, &&f0x2c, &&f0x2d, &&pfx2e, &&f0x2f,
/* 0x30 */	&&f0x30, &&f0x31, &&f0x32, &&f0x33, &&f0x34, &&f0x35, &&pfx36, &&f0x37,
/* 0x38 */	&&f0x38, &&f0x39, &&f0x3a, &&f0x3b, &&f0x3c, &&f0x3d, &&pfx3e, &&f0x3f,
/* 0x40 */	&&f0x40, &&f0x41, &&f0x42, &&f0x43, &&f0x44, &&f0x45, &&f0x46, &&f0x47,
/* 0x48 */	&&f0x48, &&f0x49, &&f0x4a, &&f0x4b, &&f0x4c, &&f0x4d, &&f0x4e, &&f0x4f,
/* 0x50 */	&&f0x50, &&f0x51, &&f0x52, &&f0x53, &&f0x54, &&f0x55, &&f0x56, &&f0x57,
/* 0x58 */	&&f0x58, &&f0x59, &&f0x5a, &&f0x5b, &&f0x5c, &&f0x5d, &&f0x5e, &&f0x5f,
/* 0x60 */	&&f0x60, &&f0x61, &&f0x62, &&f0x63, &&pfx64, &&pfx65, &&pfx66, &&pfx67,
/* 0x68 */	&&f0x68, &&f0x69, &&f0x6a, &&f0x6b, &&f0x6c, &&f0x6d, &&f0x6e, &&f0x6f,
/* 0x70 */	&&f0x70, &&f0x71, &&f0x72, &&f0x73, &&f0x74, &&f0x75, &&f0x76, &&f0x77,
/* 0x78 */	&&f0x78, &&f0x79, &&f0x7a, &&f0x7b, &&f0x7c, &&f0x7d, &&f0x7e, &&f0x7f,
/* 0x80 */	&&f0x80, &&f0x81, &&f0x82, &&f0x83, &&f0x84, &&f0x85, &&f0x86, &&f0x87,
/* 0x88 */	&&f0x88, &&f0x89, &&f0x8a, &&f0x8b, &&f0x8c, &&f0x8d, &&f0x8e, &&f0x8f,
/* 0x90 */	&&f0x90, &&f0x91, &&f0x92, &&f0x93, &&f0x94, &&f0x95, &&f0x96, &&f0x97,
/* 0x98 */	&&f0x98, &&f0x99, &&f0x9a, &&f0x9b, &&f0x9c, &&f0x9d, &&f0x9e, &&f0x9f,
/* 0xa0 */	&&f0xa0, &&f0xa1, &&f0xa2, &&f0xa3, &&f0xa4, &&f0xa5, &&f0xa6, &&f0xa7,
/* 0xa8 */	&&f0xa8, &&f0xa9, &&f0xaa, &&f0xab, &&f0xac, &&f0xad, &&f0xae, &&f0xaf,
/* 0xb0 */	&&f0xb0, &&f0xb1, &&f0xb2, &&f0xb3, &&f0xb4, &&f0xb5, &&f0xb6, &&f0xb7,
/* 0xb8 */	&&f0xb8, &&f0xb9, &&f0xba, &&f0xbb, &&f0xbc, &&f0xbd, &&f0xbe, &&f0xbf,
/* 0xc0 */	&&f0xc0, &&f0xc1, &&f0xc2, &&f0xc3, &&f0xc4, &&f0xc5, &&f0xc6, &&f0xc7,
/* 0xc8 */	&&f0xc8, &&f0xc9, &&f0xca, &&f0xcb, &&f0xcc, &&f0xcd, &&f0xce, &&f0xcf,
/* 0xd0 */	&&f0xd0, &&f0xd1, &&f0xd2, &&f0xd3, &&f0xd4, &&f0xd5, &&f0xd6, &&f0xd7,
/* 0xd8 */	&&f0xd8, &&f0xd9, &&f0xda, &&f0xdb, &&f0xdc, &&f0xdd, &&f0xde, &&f0xdf,
/* 0xe0 */	&&f0xe0, &&f0xe1, &&f0xe2, &&f0xe3, &&f0xe4, &&f0xe5, &&f0xe6, &&f0xe7,
/* 0xe8 */	&&f0xe8, &&f0xe9, &&f0xea, &&f0xeb, &&f0xec, &&f0xed, &&f0xee, &&f0xef,
/* 0xf0 */	&&pfxf0, &&f0xf1, &&pfxf2, &&pfxf3, &&f0xf4, &&f0xf5, &&f0xf6, &&f0xf7,
/* 0xf8 */	&&f0xf8, &&f0xf9, &&f0xfa, &&f0xfb, &&f0xfc, &&f0xfd, &&f0xfe, &&f0xff,
	};
	goto *pfxlabel[b1];
#define HANDLE_PREFIX(C, STMT) \
		pfx ## C: { \
			STMT; \
			TRY(fetch8(cpu, &b1)); \
			goto *pfxlabel[b1]; \
		}
		HANDLE_PREFIX(26, curr_seg = SEG_ES)
		HANDLE_PREFIX(2e, curr_seg = SEG_CS)
		HANDLE_PREFIX(36, curr_seg = SEG_SS)
		HANDLE_PREFIX(3e, curr_seg = SEG_DS)
		HANDLE_PREFIX(64, curr_seg = SEG_FS)
		HANDLE_PREFIX(65, curr_seg = SEG_GS)
		HANDLE_PREFIX(66, opsz16 = !code16)
		HANDLE_PREFIX(67, adsz16 = !code16)
		HANDLE_PREFIX(f3, rep = 1) // REP
		HANDLE_PREFIX(f2, rep = 2) // REPNE
		HANDLE_PREFIX(f0, /*lock = true*/)
#undef HANDLE_PREFIX
#endif
	eswitch(b1) {
#define I(_case, _rm, _rwm, _op) _case { _rm(_rwm, _op); ebreak; }
#include "i386ins.def"
#undef I

#undef CX
#define CX(_1) case _1:
#define GRPBEG TRY(peek8(cpu, &modrm)); switch((modrm >> 3) & 7) {
#define GRPCASE(_case, _rm, _rwm, _op) _case { _rm(_rwm, _op); ebreak; }
#define GRPEND default: default_ud; } ebreak;

	ecase(0x80): ecase(0x82): { // G1b
GRPBEG
#define IG1b GRPCASE
#include "i386ins.def"
#undef IG1b
GRPEND
	}

	ecase(0x81): { // G1v
GRPBEG
#define IG1v GRPCASE
#include "i386ins.def"
#undef IG1v
GRPEND
	}

	ecase(0x83): { // G1vIb
GRPBEG
#define IG1vIb GRPCASE
#include "i386ins.def"
#undef IG1vIb
GRPEND
	}

	ecase(0xc0): { // G2b
GRPBEG
#define IG2b GRPCASE
#include "i386ins.def"
#undef IG2b
GRPEND
	}

	ecase(0xc1): { // G2v
GRPBEG
#define IG2v GRPCASE
#include "i386ins.def"
#undef IG2v
GRPEND
	}

	ecase(0xd0): { // G2b1
GRPBEG
#define IG2b1 GRPCASE
#include "i386ins.def"
#undef IG2b1
GRPEND
	}

	ecase(0xd1): { // G2v1
GRPBEG
#define IG2v1 GRPCASE
#include "i386ins.def"
#undef IG2v1
GRPEND
	}

	ecase(0xd2): { // G2bC
GRPBEG
#define IG2bC GRPCASE
#include "i386ins.def"
#undef IG2bC
GRPEND
	}

	ecase(0xd3): { // G2v1
GRPBEG
#define IG2vC GRPCASE
#include "i386ins.def"
#undef IG2vC
GRPEND
	}

	ecase(0xf6): { // G3b
GRPBEG
#define IG3b GRPCASE
#include "i386ins.def"
#undef IG3b
GRPEND
	}

	ecase(0xf7): { // G3v
GRPBEG
#define IG3v GRPCASE
#include "i386ins.def"
#undef IG3v
GRPEND
	}

	ecase(0xfe): { // G4
GRPBEG
#define IG4 GRPCASE
#include "i386ins.def"
#undef IG4
GRPEND
	}

	ecase(0xff): { // G5
GRPBEG
#define IG5 GRPCASE
#include "i386ins.def"
#undef IG5
GRPEND
	}

	ecase(0x0f): { // two byte
		TRY(fetch8(cpu, &b1));
		switch(b1) {
#define I2(_case, _rm, _rwm, _op) _case { _rm(_rwm, _op); ebreak; }
#include "i386ins.def"
#undef I2

		case 0x00: { // G6
GRPBEG
#define IG6 GRPCASE
#include "i386ins.def"
#undef IG6
GRPEND
		}

		case 0x01: { // G7
GRPBEG
#define IG7 GRPCASE
#include "i386ins.def"
#undef IG7
GRPEND
		}

		case 0xba: { // G8
GRPBEG
#define IG8 GRPCASE
#include "i386ins.def"
#undef IG8
GRPEND
		}

		case 0xc7: { // G9
GRPBEG
#define IG9 GRPCASE
#include "i386ins.def"
#undef IG9
GRPEND
		}
		default: default_ud;
		}
		ebreak;
	}

	edefault: default_ud;
	}
	}
	return true;
}

// XXX: incomplete
enum { TS_JMP, TS_CALL, TS_IRET };
static bool task_switch(CPUI386 *cpu, int tss, int sw_type)
{
	OptAddr meml;
	int oldtss = cpu->seg[SEG_TR].sel;
	int tr_type = cpu->seg[SEG_TR].flags & 0xf;
	assert (tr_type == 9 || tr_type == 11);

	TRY1(translate(cpu, &meml, 2, SEG_TR, 0x20, 4, 0));
	store32(cpu, &meml, cpu->next_ip);

	refresh_flags(cpu);
	TRY1(translate(cpu, &meml, 2, SEG_TR, 0x24, 4, 0));
	if (sw_type == TS_IRET)
		store32(cpu, &meml, cpu->flags & ~NT);
	else
		store32(cpu, &meml, cpu->flags);

	for (int i = 0; i < 8; i++) {
		TRY1(translate(cpu, &meml, 2, SEG_TR, 0x28 + 4 * i, 4, 0));
		store32(cpu, &meml, REGi(i));
	}

	for (int i = 0; i < 6; i++) {
		TRY1(translate(cpu, &meml, 2, SEG_TR, 0x48 + 4 * i, 4, 0));
		store32(cpu, &meml, cpu->seg[i].sel);
	}

	// clear busy bit
	if (sw_type == TS_JMP || sw_type == TS_IRET) {
		uword addr = cpu->gdt.base + (cpu->seg[SEG_TR].sel & ~0x7);
		TRY1(translate_laddr(cpu, &meml, 3, addr + 4, 4, 0));
		store32(cpu, &meml, load32(cpu, &meml) & ~(1 << 9));
	}

	TRY1(set_seg(cpu, SEG_TR, tss));
	int new_tr_type = cpu->seg[SEG_TR].flags & 0xf;
	assert(new_tr_type == 9 || new_tr_type == 11);

	// set busy bit
	if (sw_type == TS_JMP || sw_type == TS_CALL) {
		uword addr = cpu->gdt.base + (tss & ~0x7);
		TRY1(translate_laddr(cpu, &meml, 3, addr + 4, 4, 0));
		store32(cpu, &meml, load32(cpu, &meml) | (1 << 9));
		cpu->seg[SEG_TR].flags |= 2;
	}

	cpu->cr0 |= 1 << 3; // set TS bit

	TRY1(translate(cpu, &meml, 1, SEG_TR, 0x60, 4, 0));
	TRY1(set_seg(cpu, SEG_LDT, load32(cpu, &meml)));

	for (int i = 0; i < 8; i++) {
		TRY1(translate(cpu, &meml, 1, SEG_TR, 0x28 + 4 * i, 4, 0));
		REGi(i) = load32(cpu, &meml);
	}

	for (int i = 0; i < 6; i++) {
		TRY1(translate(cpu, &meml, 1, SEG_TR, 0x48 + 4 * i, 4, 0));
		TRY1(set_seg(cpu, i, load32(cpu, &meml)));
	}

	TRY1(translate(cpu, &meml, 1, SEG_TR, 0x20, 4, 0));
	cpu->next_ip = load32(cpu, &meml); cpu->prefetch_base = (u32)-1;

	TRY1(translate(cpu, &meml, 1, SEG_TR, 0x24, 4, 0));
	cpu->flags = load32(cpu, &meml);
	cpu->flags &= EFLAGS_MASK;
	cpu->flags |= 0x2;
	if (sw_type == TS_CALL) {
		TRY1(translate(cpu, &meml, 2, SEG_TR, 0, 4, 0));
		store32(cpu, &meml, oldtss);
		cpu->flags |= NT;
	}

	TRY1(translate(cpu, &meml, 1, SEG_TR, 0x1c, 4, 0));
	cpu->cr3 = load32(cpu, &meml);
	tlb_clear(cpu);

	return true;
}

static bool pmcall(CPUI386 *cpu, bool opsz16, uword addr, int sel, bool isjmp)
{
	sel = sel & 0xffff;
	uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;

	if ((sel & ~0x3) == 0) THROW(EX_GP, 0);

	uword w1, w2;
	TRY(read_desc(cpu, sel, &w1, &w2));

	int s = (w2 >> 12) & 1;
	int dpl = (w2 >> 13) & 0x3;
	int p = (w2 >> 15) & 1;
	if (!p) {
//		dolog("pmcall: seg not present %04x\n", sel);
		THROW(EX_NP, sel & ~0x3);
	}

	if (s) {
		bool code = (w2 >> 8) & 0x8;
		bool conforming = (w2 >> 8) & 0x4;
		if (!code) THROW(EX_GP, sel & ~0x3);
		if (conforming) {
			// call conforming code segment
			if (dpl > cpu->cpl) THROW(EX_GP, sel & ~0x3);
			sel = (sel & 0xfffc) | cpu->cpl;
		} else {
			// call nonconforming code segment
			if ((sel & 0x3) > cpu->cpl || dpl != cpu->cpl)
				THROW(EX_GP, sel & ~0x3);
			sel = (sel & 0xfffc) | cpu->cpl;
		}

		if (!isjmp) {
			OptAddr meml1, meml2;
			uword sp = lreg32(4);
			if (opsz16) {
				TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask, 2, 0));
				TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 4) & sp_mask, 2, 0));
				set_sp(sp - 4, sp_mask);
				saddr16(&meml1, cpu->seg[SEG_CS].sel);
				saddr16(&meml2, cpu->next_ip);
			} else {
				TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask, 4, 0));
				TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 8) & sp_mask, 4, 0));
				set_sp(sp - 8, sp_mask);
				saddr32(&meml1, cpu->seg[SEG_CS].sel);
				saddr32(&meml2, cpu->next_ip);
			}
		}
//		if ((sel & 3) != cpu->cpl)
//			dolog("pmcall PVL %d => %d\n", cpu->cpl, sel & 3);
		TRY1(set_seg(cpu, SEG_CS, sel));
		cpu->next_ip = addr; cpu->prefetch_base = (u32)-1;
	} else {
		int newcs = w1 >> 16;
		uword newip = (w1 & 0xffff) | (w2 & 0xffff0000);
		int gt = (w2 >> 8) & 0xf;
		int wc = w2 & 31;

		if (dpl < cpu->cpl || dpl < (sel & 3))
			THROW(EX_GP, sel & ~0x3);

		// only 32bit TSS is supported now
		int tr_type = cpu->seg[SEG_TR].flags & 0xf;
		if (tr_type == 9 || tr_type == 11) {
			if (gt == 9) {
				// 32 bit TSS avail segs
				return task_switch(cpu, sel,
						   isjmp ? TS_JMP : TS_CALL);
			}

			if (gt == 5) {
				// task gates
				return task_switch(cpu, newcs,
						   isjmp ? TS_JMP : TS_CALL);
			}
		}

		if (gt != 4 && gt != 12) {
			fprintf(stderr, "gate type = %d\n", gt);
			cpu_abort(cpu, -203);
		}

		// call gates
		// examine code segment selector in call gate descriptor
		if ((newcs & ~0x3) == 0) THROW(EX_GP, 0);
		uword neww2;
		TRY(read_desc(cpu, newcs, NULL, &neww2));

		// if not code segment
		if (((neww2 >> 11) & 0x3) != 0x3)
			THROW(EX_GP, newcs & ~0x3);

		int newdpl = (neww2 >> 13) & 0x3;
		int newp = (neww2 >> 15) & 1;
		if (!newp) THROW(EX_NP, newcs & ~0x3);
		if (newdpl > cpu->cpl) THROW(EX_GP, newcs & ~0x3);

		bool conforming = (neww2 >> 8) & 0x4;
		bool gate16 = (gt == 4);
		if (!conforming && newdpl < cpu->cpl) {
			// more privilege
			OptAddr msp0, mss0;
			uword oldss = cpu->seg[SEG_SS].sel;
			uword oldsp = REGi(4);
			uword params[31];
			uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
			gate_note_call(cpu, sel, newcs, gt, wc, newdpl, oldsp);

			if (!gate16) {
				for (int i = 0; i < wc; i++) {
					OptAddr meml;
					TRY(translate(cpu, &meml, 1, SEG_SS, (oldsp + 4 * i) & sp_mask, 4, 0));
					params[i] = laddr32(&meml);
				}
			} else {
				for (int i = 0; i < wc; i++) {
					OptAddr meml;
					TRY(translate(cpu, &meml, 1, SEG_SS, (oldsp + 2 * i) & sp_mask, 2, 0));
					params[i] = laddr16(&meml);
				}
			}

			if (!(cpu->seg[SEG_TR].flags & 0x8)) {
				TRY(translate(cpu, &msp0, 1, SEG_TR, 2 + 4 * newdpl, 2, 0));
				TRY(translate(cpu, &mss0, 1, SEG_TR, 4 + 4 * newdpl, 2, 0));
				// TODO: Check SS...
				REGi(4) = load16(cpu, &msp0);
				TRY(set_seg(cpu, SEG_SS, load16(cpu, &mss0)));
			} else {
				TRY(translate(cpu, &msp0, 1, SEG_TR, 4 + 8 * newdpl, 4, 0));
				TRY(translate(cpu, &mss0, 1, SEG_TR, 8 + 8 * newdpl, 4, 0));
				// TODO: Check SS...
				REGi(4) = load32(cpu, &msp0);
				TRY(set_seg(cpu, SEG_SS, load32(cpu, &mss0)));
			}
			sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;

			if (!isjmp) {
			if (!gate16) {
				OptAddr meml1, meml2, meml3, meml4;
				uword sp = lreg32(4);
				TRY1(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
				TRY1(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
				TRY1(translate(cpu, &meml3, 2, SEG_SS, (sp - 4 * (3 + wc)) & sp_mask, 4, 0));
				TRY1(translate(cpu, &meml4, 2, SEG_SS, (sp - 4 * (4 + wc)) & sp_mask, 4, 0));

				for (int i = 0; i < wc; i++) {
					OptAddr meml;
					TRY1(translate(cpu, &meml, 2, SEG_SS, (sp - 4 * (2 + wc - i)) & sp_mask, 4, 0));
					saddr32(&meml, params[i]);
				}

				saddr32(&meml1, oldss);
				saddr32(&meml2, oldsp);
				saddr32(&meml3, cpu->seg[SEG_CS].sel);
				saddr32(&meml4, cpu->next_ip);
				set_sp(sp - 4 * (4 + wc), sp_mask);
			} else {
				OptAddr meml1, meml2, meml3, meml4;
				uword sp = lreg32(4);
				TRY1(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
				TRY1(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
				TRY1(translate(cpu, &meml3, 2, SEG_SS, (sp - 2 * (3 + wc)) & sp_mask, 2, 0));
				TRY1(translate(cpu, &meml4, 2, SEG_SS, (sp - 2 * (4 + wc)) & sp_mask, 2, 0));

				for (int i = 0; i < wc; i++) {
					OptAddr meml;
					TRY1(translate(cpu, &meml, 2, SEG_SS, (sp - 2 * (2 + wc - i)) & sp_mask, 2, 0));
					saddr16(&meml, params[i]);
				}

				saddr16(&meml1, oldss);
				saddr16(&meml2, oldsp);
				saddr16(&meml3, cpu->seg[SEG_CS].sel);
				saddr16(&meml4, cpu->next_ip);
				set_sp(sp - 2 * (4 + wc), sp_mask);
			}
			}
			newcs = (newcs & 0xfffc) | newdpl;
		} else {
			// same privilege
			if (!isjmp) {
			OptAddr meml1, meml2;
			uword sp = lreg32(4);
			uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
			if (gate16) {
				TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
				TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
				saddr16(&meml1, cpu->seg[SEG_CS].sel);
				saddr16(&meml2, cpu->next_ip);
				set_sp(sp - 2 * 2, sp_mask);
			} else {
				TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
				TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
				saddr32(&meml1, cpu->seg[SEG_CS].sel);
				saddr32(&meml2, cpu->next_ip);
				set_sp(sp - 4 * 2, sp_mask);
			}
			}
			newcs = (newcs & 0xfffc) | cpu->cpl;
		}

		TRY1(set_seg(cpu, SEG_CS, newcs));

		cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
	}
	return true;
}

// 0: exception
// 1: intra PVL
// 2: inter PVL
// 3: from v8086
static int __call_isr_check_cs(CPUI386 *cpu, int sel, int ext, int *csdpl)
{
	sel = sel & 0xffff;
	OptAddr meml;
	uword off = sel & ~0x7;
	uword base;
	uword limit;
	if (sel & 0x4) {
		base = cpu->seg[SEG_LDT].base;
		limit = cpu->seg[SEG_LDT].limit;
	} else {
		base = cpu->gdt.base;
		limit = cpu->gdt.limit;
	}
	if ((sel & ~0x3) == 0 || off + 7 > limit) {
		dolog("__call_isr_check_cs null/limit: sel=%04x off=%x limit=%x\n", sel, off, limit);
    	THROW(EX_GP, ext);
	}

	TRY1(translate_laddr(cpu, &meml, 1, base + off + 4, 4, 0));
	uword w2 = load32(cpu, &meml);
	int s = (w2 >> 12) & 1;
	bool code = (w2 >> 8) & 0x8;
	bool conforming = (w2 >> 8) & 0x4;
	int dpl = (w2 >> 13) & 0x3;
	int p = (w2 >> 15) & 1;
	*csdpl = dpl;
	if (!s || !code || dpl > cpu->cpl) {
	    dolog("__call_isr_check_cs: sel=%04x s=%d code=%d dpl=%d cpl=%d ext=%d w2=%08x\n",
    	      sel, s, code, dpl, cpu->cpl, ext, w2);
		THROW(EX_GP, (sel & ~0x3) | ext);
	}

	if (!p) THROW(EX_NP, sel & ~0x3);

	if (!conforming && dpl < cpu->cpl) {
		if (!(cpu->flags & VM)) {
			return 2;
		} else {
			if (dpl != 0) {
				dolog("__call_isr_check_cs fail1: %d %d %d\n", conforming, dpl, cpu->cpl);
				THROW(EX_GP, (sel & ~0x3) | ext);
			} else {
				return 3;
			}
		}
	} else {
		if (cpu->flags & VM) {
			THROW(EX_GP, (sel & ~0x3) | ext);
		} else {
			if (conforming || dpl == cpu->cpl) {
				return 1;
			} else {
				dolog("__call_isr_check_cs fail2: %d %d %d\n", conforming, dpl, cpu->cpl);
				THROW(EX_GP, (sel & ~0x3) | ext);
			}
		}
	}
	__builtin_unreachable();
}

static bool IRAM_ATTR call_isr(CPUI386 *cpu, int no, bool pusherr, int ext)
{
	/* INT 2Fh network-attached-drive handler hook - intercept in V86 and real mode */
	if (no == 0x2F && cpu->int2f_handler && (!(cpu->cr0 & 1) || (cpu->flags & VM))) {
		if (cpu->int2f_handler(cpu, cpu->int2f_opaque)) return true; /* handled */
	}
	#if DEBUG_CPU
	if (cpu->flags & VM && no >= 0x20) {
		dolog("V86 INT %02xh\n", no);
	}
	#endif
	if (!(cpu->cr0 & 1)) {
		/* REAL-ADDRESS-MODE */
		uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
		OptAddr meml;
		uword base = cpu->idt.base;
		int off = no * 4;
		TRY1(translate_laddr(cpu, &meml, 1, base + off, 4, 0));
		uword w1 = load32(cpu, &meml);
		int newcs = w1 >> 16;
		uword newip = w1 & 0xffff;

		OptAddr meml1, meml2, meml3;
		uword sp = lreg32(4);
		TRY1(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
		TRY1(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
		TRY1(translate(cpu, &meml3, 2, SEG_SS, (sp - 2 * 3) & sp_mask, 2, 0));
		refresh_flags(cpu);
		cpu->cc.mask = 0;
		saddr16(&meml1, cpu->flags);
		saddr16(&meml2, cpu->seg[SEG_CS].sel);
		saddr16(&meml3, cpu->ip);
		sreg32(4, (sp - 2 * 3) & sp_mask);
		/* A frame going into the vector table means the machine has
		 * already lost its stack; keep the rings as they are now. */
		if (cpu->seg[SEG_SS].base + ((sp - 2 * 3) & sp_mask) < 0x400)
			cpu_exclog_freeze();

		TRY1(set_seg(cpu, SEG_CS, newcs));
		cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
		cpu->ip = newip;
		cpu->flags &= ~(IF|TF);
		return true;
	}

	/* PROTECTED-MODE */
	OptAddr meml;
	uword base = cpu->idt.base;
	int off = no << 3;
	if (off + 7 > cpu->idt.limit) {
		dolog("call_isr error0 %d %d\n", off, cpu->idt.limit);
		NJ_ISR_NOTE(1, 0, 0);
		THROW(EX_GP, off | 2 | ext);
	}

	TRY1(translate_laddr(cpu, &meml, 1, base + off, 4, 0));
	uword w1 = load32(cpu, &meml);
	TRY1(translate_laddr(cpu, &meml, 1, base + off + 4, 4, 0));
	uword w2 = load32(cpu, &meml);

	int gt = (w2 >> 8) & 0xf;
	if (gt != 6 && gt != 7 && gt != 0xe && gt != 0xf && gt != 5) {
//		dolog("call_isr error1 gt=%d\n", gt);
		NJ_ISR_NOTE(2, w1, w2);
		THROW(EX_GP, off | 2 | ext);
	}

	int dpl = (w2 >> 13) & 0x3;
	if (!ext && dpl < cpu->cpl) { NJ_ISR_NOTE(3, w1, w2); THROW(EX_GP, off | 2); }

	int p = (w2 >> 15) & 1;
	if (!p) {
		dolog("call_isr error3\n");
		NJ_ISR_NOTE(4, w1, w2);
		THROW(EX_NP, off | 2 | ext);
	}

	/* task gate */
	if (gt == 5)
		return task_switch(cpu, w1 >> 16, TS_CALL);

	/* TRAP-OR-INTERRUPT-GATE */
	int newcs = w1 >> 16;
	uword newip = (w1 & 0xffff) | (w2 & 0xffff0000);
	bool gate16 = gt == 6 || gt == 7;

	int csdpl;
	switch(__call_isr_check_cs(cpu, newcs, ext, &csdpl)) {
	case 0: {
		return false;
	}
	case 1: /* intra PVL */ {
		OptAddr meml1, meml2, meml3, meml4;
		uword sp = lreg32(4);
		uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
		if (gate16) {
			TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml3, 2, SEG_SS, (sp - 2 * 3) & sp_mask, 2, 0));
			if (pusherr) {
				TRY(translate(cpu, &meml4, 2, SEG_SS, (sp - 2 * 4) & sp_mask, 2, 0));
			}

			refresh_flags(cpu);
			cpu->cc.mask = 0;
			saddr16(&meml1, cpu->flags);

			saddr16(&meml2, cpu->seg[SEG_CS].sel);
			saddr16(&meml3, cpu->ip);
			if (pusherr) {
				saddr16(&meml4, cpu->excerr);
				dolog("EX intra PVL G16 INT %02xh %04x:%04x err=%04x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
				set_sp(sp - 2 * 4, sp_mask);
			} else {
				set_sp(sp - 2 * 3, sp_mask);
			}
		} else {
			TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml3, 2, SEG_SS, (sp - 4 * 3) & sp_mask, 4, 0));
			if (pusherr) {
				TRY(translate(cpu, &meml4, 2, SEG_SS, (sp - 4 * 4) & sp_mask, 4, 0));
			}

			refresh_flags(cpu);
			cpu->cc.mask = 0;
			saddr32(&meml1, cpu->flags);

			saddr32(&meml2, cpu->seg[SEG_CS].sel);
			saddr32(&meml3, cpu->ip);
			if (pusherr) {
				saddr32(&meml4, cpu->excerr);
				dolog("EX intra PVL INT %02xh %04x:%08x err=%08x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
				set_sp(sp - 4 * 4, sp_mask);
			} else {
				set_sp(sp - 4 * 3, sp_mask);
			}
		}
		newcs = (newcs & (~3)) | cpu->cpl;
		break;
	}
	case 2: /* inter PVL */ {
//		dolog("call_isr %d %x PVL %d => %d\n", no, no, cpu->cpl, csdpl);
		OptAddr msp0, mss0;
		int newpl = csdpl;
		uword oldss = cpu->seg[SEG_SS].sel;
		uword oldsp = REGi(4);
		uword newss, newsp;
		if (cpu->seg[SEG_TR].flags & 0x8) {
			TRY(translate(cpu, &msp0, 1, SEG_TR, 4 + 8 * newpl, 4, 0));
			TRY(translate(cpu, &mss0, 1, SEG_TR, 8 + 8 * newpl, 4, 0));
			newsp = load32(cpu, &msp0);
			newss = load32(cpu, &mss0) & 0xffff;
		} else {
			TRY(translate(cpu, &msp0, 1, SEG_TR, 2 + 4 * newpl, 2, 0));
			TRY(translate(cpu, &mss0, 1, SEG_TR, 4 + 4 * newpl, 2, 0));
			newsp = load16(cpu, &msp0);
			newss = load16(cpu, &mss0);
		}

		REGi(4) = newsp;
		TRY(set_seg(cpu, SEG_SS, newss));
		uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
		OptAddr meml1, meml2, meml3, meml4, meml5, meml6;
		uword sp = lreg32(4);
		if (gate16) {
			TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml3, 2, SEG_SS, (sp - 2 * 3) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml4, 2, SEG_SS, (sp - 2 * 4) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml5, 2, SEG_SS, (sp - 2 * 5) & sp_mask, 2, 0));
			if (pusherr) {
				TRY(translate(cpu, &meml6, 2, SEG_SS, (sp - 2 * 6) & sp_mask, 2, 0));
			}
			saddr16(&meml1, oldss);
			saddr16(&meml2, oldsp);

			refresh_flags(cpu);
			cpu->cc.mask = 0;
			saddr16(&meml3, cpu->flags);

			saddr16(&meml4, cpu->seg[SEG_CS].sel);
			saddr16(&meml5, cpu->ip);
			if (pusherr) {
				saddr16(&meml6, cpu->excerr);
				dolog("EX inter PVL G16 INT %02xh %04x:%04x err=%04x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
				set_sp(sp - 2 * 6, sp_mask);
			} else {
				set_sp(sp - 2 * 5, sp_mask);
			}
		} else {
			TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml3, 2, SEG_SS, (sp - 4 * 3) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml4, 2, SEG_SS, (sp - 4 * 4) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml5, 2, SEG_SS, (sp - 4 * 5) & sp_mask, 4, 0));
			if (pusherr) {
				TRY(translate(cpu, &meml6, 2, SEG_SS, (sp - 4 * 6) & sp_mask, 4, 0));
			}
			saddr32(&meml1, oldss);
			saddr32(&meml2, oldsp);

			refresh_flags(cpu);
			cpu->cc.mask = 0;
			saddr32(&meml3, cpu->flags);

			saddr32(&meml4, cpu->seg[SEG_CS].sel);
			saddr32(&meml5, cpu->ip);
			if (pusherr) {
				saddr32(&meml6, cpu->excerr);
				dolog("EX inter PVL INT %02xh %04x:%08x err=%08x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
				sreg32(4, sp - 4 * 6);
			} else {
				sreg32(4, sp - 4 * 5);
			}
		}
		newcs = (newcs & (~3)) | newpl;
		break;
	}
	case 3: /* from v8086 */ {
//		dolog("int from v8086\n");
		if (csdpl != 0) cpu_abort(cpu, -205);
		if (gate16) cpu_abort(cpu, -206);
//		dolog("call_isr %d %x PVL %d => 0\n", no, no, cpu->cpl, csdpl);
		OptAddr msp0, mss0;
		int newpl = 0;
		uword oldss = cpu->seg[SEG_SS].sel;
		uword oldsp = REGi(4);
		uword newss, newsp;
		if (!(cpu->seg[SEG_TR].flags & 0x8)) cpu_abort(cpu, -207);
		TRY(translate(cpu, &msp0, 1, SEG_TR, 4 + 8 * newpl, 4, 0));
		TRY(translate(cpu, &mss0, 1, SEG_TR, 8 + 8 * newpl, 4, 0));
		newsp = load32(cpu, &msp0);
		newss = load32(cpu, &mss0) & 0xffff;
		uword oldflags = cpu->flags;
		cpu->flags &= ~VM;
		REGi(4) = newsp;
		if (!set_seg(cpu, SEG_SS, newss)) {
			cpu->flags = oldflags;
			REGi(4) = oldsp;
			return false;
		}

		uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
		OptAddr memlg, memlf, memld, memle;
		OptAddr meml1, meml2, meml3, meml4, meml5, meml6;
		uword sp = lreg32(4);
		TRY1(translate(cpu, &memlg, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
		TRY1(translate(cpu, &memlf, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
		TRY1(translate(cpu, &memld, 2, SEG_SS, (sp - 4 * 3) & sp_mask, 4, 0));
		TRY1(translate(cpu, &memle, 2, SEG_SS, (sp - 4 * 4) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 5) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 6) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml3, 2, SEG_SS, (sp - 4 * 7) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml4, 2, SEG_SS, (sp - 4 * 8) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml5, 2, SEG_SS, (sp - 4 * 9) & sp_mask, 4, 0));
		if (pusherr) {
			TRY(translate(cpu, &meml6, 2, SEG_SS, (sp - 4 * 10) & sp_mask, 4, 0));
		}
		saddr32(&memlg, cpu->seg[SEG_GS].sel);
		saddr32(&memlf, cpu->seg[SEG_FS].sel);
		saddr32(&memld, cpu->seg[SEG_DS].sel);
		saddr32(&memle, cpu->seg[SEG_ES].sel);
		saddr32(&meml1, oldss);
		saddr32(&meml2, oldsp);

		refresh_flags(cpu);
		cpu->cc.mask = 0;
		saddr32(&meml3, cpu->flags | VM);

		saddr32(&meml4, cpu->seg[SEG_CS].sel);
		saddr32(&meml5, cpu->ip);
		if (pusherr) {
			saddr32(&meml6, cpu->excerr);
			dolog("EX v8086 INT %02xh %04x:%08x err=%08x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
			set_sp(sp - 4 * 10, sp_mask);
		} else {
			set_sp(sp - 4 * 9, sp_mask);
		}

		newcs = (newcs & (~3)) | newpl;
		TRY1(set_seg(cpu, SEG_DS, 0));
		TRY1(set_seg(cpu, SEG_ES, 0));
		TRY1(set_seg(cpu, SEG_FS, 0));
		TRY1(set_seg(cpu, SEG_GS, 0));
		cpu->flags &= ~(TF | RF | NT);
		TRY1(set_seg(cpu, SEG_CS, newcs));
		cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
		cpu->ip = newip;
		if (gt == 0x6 || gt == 0xe)
			cpu->flags &= ~IF;
		return true;
	}
	default: assert(false);
	}
	TRY1(set_seg(cpu, SEG_CS, newcs));
	cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
	cpu->ip = newip;
	cpu->flags &= ~(TF | RF | NT);
	if (gt == 0x6 || gt == 0xe)
		cpu->flags &= ~IF;
	return true;
}

static bool __pmiret_check_cs_same(CPUI386 *cpu, int sel)
{
	sel = sel & 0xffff;
	if ((sel & ~0x3) == 0) {
		dolog("__pmiret_check_cs_same: sel %04x\n", sel);
		THROW(EX_GP, sel & ~0x3);
	}
	uword w2;
	TRY(read_desc(cpu, sel, NULL, &w2));

	int s = (w2 >> 12) & 1;
	bool code = (w2 >> 8) & 0x8;
	bool conforming = (w2 >> 8) & 0x4;
	int dpl = (w2 >> 13) & 0x3;
	int p = (w2 >> 15) & 1;

	if (!s || !code) THROW(EX_GP, sel & ~0x3);

	if (!conforming) {
		if (dpl != cpu->cpl) THROW(EX_GP, sel & ~0x3);
	} else {
		if (dpl > cpu->cpl) THROW(EX_GP, sel & ~0x3);
	}

	if (!p) {
//		dolog("__pmiret_check_cs_same: seg not present %04x\n", sel);
		THROW(EX_NP, sel & ~0x3);
	}
	return true;
}

static bool __pmiret_check_cs_outer(CPUI386 *cpu, int sel)
{
	sel = sel & 0xffff;
	if ((sel & ~0x3) == 0) {
		dolog("__pmiret_check_cs_outer: sel %04x\n", sel);
		THROW(EX_GP, sel & ~0x3);
	}
	uword w2;
	TRY(read_desc(cpu, sel, NULL, &w2));

	int s = (w2 >> 12) & 1;
	bool code = (w2 >> 8) & 0x8;
	bool conforming = (w2 >> 8) & 0x4;
	int dpl = (w2 >> 13) & 0x3;
	int p = (w2 >> 15) & 1;
	int rpl = sel & 3;
	
	if (!s || !code) THROW(EX_GP, sel & ~0x3);

	if (!conforming) {
		if (dpl != rpl) THROW(EX_GP, sel & ~0x3);
	} else {
    	// conforming: DPL must be <= RPL (Intel SDM Vol.2, IRET, outer privilege)
    	if (dpl > rpl) {
			dolog("__pmiret_check_cs_outer: DPL (%04x) must be <= RPL (%04x) %04x\n", dpl, rpl, sel);
			THROW(EX_GP, sel & ~0x3);
		}
	}

	if (!p) {
		dolog("__pmiret_check_cs_outer: seg not present %04x\n", sel);
		THROW(EX_NP, sel & ~0x3);
	}
	return true;
}

void v86_note_entry(CPUI386 *cpu, uword, uword, uword, uword, uword, uword, uword);

static bool pmret(CPUI386 *cpu, bool opsz16, int off, bool isiret)
{
	/* RETF imm16's count, before IRET reuses off for the flags slot. */
	const uword imm = isiret ? 0 : (uword)off;
	if (isiret) {
		if ((cpu->flags & VM)) THROW(EX_GP, 0);
		if ((cpu->flags & NT)) {
			OptAddr meml;
			TRY(translate(cpu, &meml, 1, SEG_TR, 0, 2, 0));
			int tssback = laddr16(&meml);
			dolog("IRET NT: tss curr: %04x back: %04x\n",
			      cpu->seg[SEG_TR].sel, tssback);
			// win2000 needs it...
			if (tssback == 0) THROW(EX_TS, 0);
			return task_switch(cpu, tssback, TS_IRET);
		}
		if (opsz16)
			off += 2;
		else
			off += 4;
	}

	OptAddr meml1, meml2, meml3, meml4, meml5;
	uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
	uword sp = lreg32(4);
	uword oldflags = cpu->flags;
	uword newip;
	int newcs;
	uword newflags = 0; // make the compiler happy
	if (opsz16) {
		/* ip */ TRY(translate(cpu, &meml1, 1, SEG_SS, sp & sp_mask, 2, 0));
		/* cs */ TRY(translate(cpu, &meml2, 1, SEG_SS, (sp + 2) & sp_mask, 2, 0));
		if (isiret) {
			/* flags */ TRY(translate(cpu, &meml3, 1, SEG_SS, (sp + 4) & sp_mask, 2, 0));
			newflags = (oldflags & 0xffff0000) | laddr16(&meml3);
		}
		newip = laddr16(&meml1);
		newcs = laddr16(&meml2);
	} else {
		/* ip */ TRY(translate(cpu, &meml1, 1, SEG_SS, sp & sp_mask, 4, 0));
		/* cs */ TRY(translate(cpu, &meml2, 1, SEG_SS, (sp + 4) & sp_mask, 4, 0));
		if (isiret) {
			/* flags */ TRY(translate(cpu, &meml3, 1, SEG_SS, (sp + 8) & sp_mask, 4, 0));
			newflags = laddr32(&meml3);
		}
		newip = laddr32(&meml1);
		newcs = laddr32(&meml2);
	}

	if (isiret) {
		uword mask = 0;
		if (cpu->cpl > 0) mask |= IOPL;
		if (get_IOPL(cpu) < cpu->cpl) mask |= IF;
		newflags = (oldflags & mask) | (newflags & ~mask);
		newflags &= EFLAGS_MASK;
		newflags |= 0x2;
	}

	if (isiret && (newflags & VM)) {
		if (cpu->cpl != 0) cpu_abort(cpu, -208);
		// return to v8086
//		dolog("pmiret PVL %d => %d (vm) %04x:%08x\n", cpu->cpl, 3, newcs, newip);
		OptAddr meml_vmes, meml_vmds, meml_vmfs, meml_vmgs;
		if (opsz16) cpu_abort(cpu, -209);
		TRY(translate(cpu, &meml4, 1, SEG_SS, (sp + 12) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml5, 1, SEG_SS, (sp + 16) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml_vmes, 1, SEG_SS, (sp + 20) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml_vmds, 1, SEG_SS, (sp + 24) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml_vmfs, 1, SEG_SS, (sp + 28) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml_vmgs, 1, SEG_SS, (sp + 32) & sp_mask, 4, 0));
		dolog("IRET->V86 raw: eip=%08x cs=%08x fl=%08x esp=%08x ss=%08x, sp=%08x eip=%08x cs=%04x fl=%08x v86esp=%08x v86ss=%04x\n",
		      laddr32(&meml1), laddr32(&meml2), laddr32(&meml3),
		      laddr32(&meml4), laddr32(&meml5),
			  sp, newip, newcs, newflags,
              laddr32(&meml4), laddr32(&meml5));
		v86_note_entry(cpu, newip, newcs, newflags,
			       laddr32(&meml4), laddr32(&meml5),
			       laddr32(&meml_vmds), laddr32(&meml_vmes));
		cpu->flags = newflags;
		TRY1(set_seg(cpu, SEG_CS, newcs));
		cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
		TRY1(set_seg(cpu, SEG_SS, laddr32(&meml5)));
		TRY1(set_seg(cpu, SEG_ES, laddr32(&meml_vmes)));
		TRY1(set_seg(cpu, SEG_DS, laddr32(&meml_vmds)));
		TRY1(set_seg(cpu, SEG_FS, laddr32(&meml_vmfs)));
		TRY(set_seg(cpu, SEG_GS, laddr32(&meml_vmgs)));
		set_sp(laddr32(&meml4), 0xffffffff);
	} else {
		int rpl = newcs & 3;
		if (rpl < cpu->cpl) THROW(EX_GP, newcs & ~0x3);
		if (rpl == cpu->cpl) {
			// return to same level
			TRY(__pmiret_check_cs_same(cpu, newcs));
//			dolog("pmiret PVL %d => %d %04x:%08x\n", cpu->cpl, newcs & 3, newcs, newip);
			if (isiret)
				cpu->flags = newflags;
			TRY1(set_seg(cpu, SEG_CS, newcs));

			if (opsz16) {
				set_sp(sp + 4 + off, sp_mask);
			} else {
				set_sp(sp + 8 + off, sp_mask);
			}
			cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
		} else {
			// return to outer level
			TRY(__pmiret_check_cs_outer(cpu, newcs));
			uword newsp;
			uword newss;
//			dolog("pmiret PVL %d => %d %04x:%08x\n", cpu->cpl, newcs & 3, newcs, newip);
			if (opsz16) {
				/* sp */ TRY(translate(cpu, &meml4, 1, SEG_SS, (sp + 4 + off) & sp_mask, 2, 0));
				/* ss */ TRY(translate(cpu, &meml5, 1, SEG_SS, (sp + 6 + off) & sp_mask, 2, 0));
				newsp = laddr16(&meml4);
				newss = laddr16(&meml5);
			} else {
				/* sp */ TRY(translate(cpu, &meml4, 1, SEG_SS, (sp + 8 + off) & sp_mask, 4, 0));
				/* ss */ TRY(translate(cpu, &meml5, 1, SEG_SS, (sp + 12 + off) & sp_mask, 4, 0));
				newsp = laddr32(&meml4);
				newss = laddr32(&meml5);
			}

			if (isiret)
				cpu->flags = newflags;
			TRY1(set_seg(cpu, SEG_CS, newcs));
			TRY1(set_seg(cpu, SEG_SS, newss));
			uword newsp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
			/*
			 * RETF imm16 releases the parameters on both stacks.  A call
			 * gate with a parameter count copies them from the caller's
			 * stack to the inner one, so the caller's copy is still there
			 * and the return has to step over it too - the inner release
			 * is the "+ off" in the reads above.
			 *
			 * Leaving it out put the caller's SP that many bytes low.  The
			 * caller was Windows 95's disk detection, which reaches a probe
			 * through a ring 0 call gate with two word parameters: its next
			 * POP SI took one of the parameters - the port number - instead
			 * of its own saved pointer, and it went on to read the control
			 * port from ss:[0x1f0] rather than from its frame.  That is where
			 * the controller at FB14 came from.
			 */
			set_sp(newsp + imm, newsp_mask);
			cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
			clear_segs(cpu);
		}
	}
	if (isiret)
		cpu->cc.mask = 0;
	return true;
}


/*
 * The exceptions the guest took, kept for afterwards.
 *
 * A fault that appears in a different place on every run cannot be caught by
 * watching for it: by the time a person sees the message box, the instruction
 * that caused it is thousands of instructions in the past.  So every exception
 * delivered to the guest is recorded here as it goes by, and the ring can be
 * read out later over the serial link.
 *
 * Two things keep the ring from filling with noise.  Page faults are counted
 * but not kept - a paged guest takes them constantly and by design - and a
 * repeat of the same vector at the same CS:EIP bumps a counter on the entry
 * already there instead of adding another.  That last rule is what makes the
 * ring survive a V86 monitor trapping the same IN instruction forty thousand
 * times: it costs one slot, not the whole ring.
 *
 * The cost on the machine is a handful of stores on a path that is already
 * out of line, so this is always on rather than behind a build switch.  An
 * exception that only happens on the run nobody was instrumenting is exactly
 * the one worth having.
 */
/* CPU_EXCLOG_N is in i386.h, where the readout is declared. */
struct CpuExcRec {
	uint32_t t_us;
	uint32_t cs_base;
	uint32_t eip;
	uint32_t cr2;
	uint32_t excerr;
	uint32_t eflags;
	uint32_t repeat;
	uint32_t esp;
	uint16_t cs_sel;
	uint8_t  no;
	uint8_t  cpl;
	uint8_t  code[8];
	uint8_t  codelen;
	/*
	 * Enough state to answer "was this fault right?".
	 *
	 * A vector and an address say a fault happened; they do not say
	 * whether it should have.  The selectors say which segment the
	 * instruction used, the registers say what offset it asked for, and
	 * the descriptor named in the error code says what the guest had told
	 * the processor about that selector.  With those three, a rejection
	 * can be checked against the tables instead of guessed at.
	 */
	uint16_t sel[6];        /* ES, CS, SS, DS, FS, GS */
	uint32_t gpr[8];        /* EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI */
	uint32_t desc_lo, desc_hi;
	uint8_t  desc_ok;
	/* The top of the guest's stack, which is where a far return finds the
	 * selector it was refused - and where a fault handler finds the frame
	 * it was given.  Sixteen words, because the frame a protected-mode
	 * monitor reflects to a handler is longer than a far return's two. */
	uint32_t stk[16];
	uint8_t  stk_ok;
};
uint32_t g_exc_hist[32];
uint32_t g_exc_triple;          /* delivery failed twice: the machine reset */
uint32_t g_exc_v86_monitor;     /* faults that are the V86 monitor working */
/*
 * The first invalid opcode, kept where nothing can push it out.
 *
 * An exception that happens once in a run of many minutes is gone from the
 * ring long before anyone thinks to read it - the ring holds sixty-four
 * entries and the machine keeps faulting for its own reasons.  #UD is the
 * one vector where a single occurrence is worth the whole story, because it
 * means the guest ran an instruction this emulator does not have.  So the
 * first one is copied aside and never overwritten.
 */
static struct CpuExcRec exc_first_ud;
static int exc_have_ud;
/*
 * The ring stops when the fault being chased arrives.
 *
 * That fault repeats thousands of times a second - the guest's handler
 * cannot get past it, so it comes straight back - and a running ring holds
 * nothing but the storm.  Frozen at the first one, it holds the path that
 * led there instead, which is the only part worth reading.  The counters
 * keep running either way.
 */
static int exc_triggered;
static int exc_after_left;
/* Faults since the guest last got somewhere; see the note at the trigger. */
uint32_t g_exc_frozen_at;
static struct CpuExcRec exc_ring[CPU_EXCLOG_N];
static uint32_t exc_head;       /* next slot to fill */
static uint32_t exc_kept;       /* entries written, for ordering the readout */

/*
 * A fault that is the V86 monitor doing its job, rather than something wrong.
 *
 * A 386 in virtual-8086 mode with IOPL below three faults on INT, IRET, CLI,
 * STI, PUSHF and POPF by design: that is how EMM386 and the Windows VMM get
 * control of a DOS program's interrupts, and a machine sitting at a prompt
 * produces ten thousand of them a second.  Keeping them would mean the ring
 * held a tenth of a second of idling and nothing else.  They are counted and
 * dropped; anything else in V86 - an instruction that really did fault - is
 * kept.
 */
static int v86_routine_fault(const uint8_t *code, int len)
{
	if (len < 1) return 0;
	switch (code[0]) {
	case 0xcd: case 0xcc: case 0xce:        /* INT n, INT3, INTO   */
	case 0xcf:                              /* IRET                */
	case 0xfa: case 0xfb:                   /* CLI, STI            */
	case 0x9c: case 0x9d:                   /* PUSHF, POPF         */
		return 1;
	}
	return 0;
}

static void exclog_note(CPUI386 *cpu, int no)
{
	if (no >= 0 && no < 32) g_exc_hist[no]++;

	/*
	 * Whether the guest is still getting somewhere.
	 *
	 * A segment-not-present fault means it is loading the next piece of
	 * code it needs; a page fault means the same of data; and anything
	 * raised in virtual-8086 mode is the monitor doing its job.  Any of
	 * those resets this.  What is left to count is a machine raising
	 * faults and going nowhere, which is what the end of a failed run
	 * looks like and what no correct sequence does for long: in a
	 * working run there is one other fault between segment loads, and
	 * at worst a handful.
	 */
	/*
	 * Nothing here decides for itself that the run has ended.
	 *
	 * Three different rules were tried - the first #GP(0), a fault that
	 * repeats with every register unchanged, and a long run without a
	 * segment load - and all three fired on a guest that was still making
	 * progress, freezing the ring minutes before the failure it was meant
	 * to catch.  The guest raises every one of those legitimately.  So the
	 * ring now runs until someone who can see the screen stops it; see
	 * cpu_exclog_freeze().
	 */

	/* Counted above, not kept: see the note on noise. */
	if (no == EX_PF) return;
	if (exc_triggered && exc_after_left <= 0) return;

	/*
	 * The bytes at CS:EIP, which is what turns "#GP somewhere" into an
	 * instruction - and what decides below whether this fault is worth a
	 * slot at all.  Reading them goes through the interpreter's own
	 * translation, which can itself raise, so the exception being
	 * delivered is put back afterwards exactly as it was.
	 */
	uint8_t code[8];
	int codelen = 0;
	{
		const int save_no = cpu->excno;
		const uword save_err = cpu->excerr;
		const uword save_cr2 = cpu->cr2;
		OptAddr res;
		if (translate8r(cpu, &res, SEG_CS, cpu->ip) &&
		    !in_iomem(res.addr1) && res.addr1 + 8 <= (uword)cpu->phys_mem_size) {
			/* Stop at the page edge rather than walking into the
			 * next one, which may not be mapped at all. */
			int n = 8;
			const int to_edge = 4096 - (int)(res.addr1 & 4095);
			if (to_edge < n) n = to_edge;
			for (int i = 0; i < n; i++)
				code[i] = ((u8 *)cpu->phys_mem)[res.addr1 + i];
			codelen = n;
		}
		cpu->excno = save_no;
		cpu->excerr = save_err;
		cpu->cr2 = save_cr2;
	}

	if (no == EX_GP && (cpu->flags & VM) && v86_routine_fault(code, codelen)) {
		g_exc_v86_monitor++;
		return;
	}

	const uint32_t cs_base = cpu->seg[SEG_CS].base;
	if (exc_kept) {
		struct CpuExcRec *prev =
			&exc_ring[(exc_head + CPU_EXCLOG_N - 1) % CPU_EXCLOG_N];
		if (prev->no == (uint8_t)no && prev->eip == cpu->ip &&
		    prev->cs_base == cs_base) {
			prev->repeat++;
			prev->t_us = time_us_32();
			return;
		}
	}

	struct CpuExcRec *r = &exc_ring[exc_head];
	exc_head = (exc_head + 1) % CPU_EXCLOG_N;
	exc_kept++;

	r->t_us    = time_us_32();
	r->cs_base = cs_base;
	r->cs_sel  = (uint16_t)cpu->seg[SEG_CS].sel;
	r->eip     = cpu->ip;
	r->cr2     = cpu->cr2;
	r->excerr  = cpu->excerr;
	r->eflags  = cpu->flags;
	r->esp     = lreg32(4);
	r->no      = (uint8_t)no;
	r->cpl     = (uint8_t)cpu->cpl;
	r->repeat  = 0;
	r->codelen = (uint8_t)codelen;
	for (int i = 0; i < codelen; i++) r->code[i] = code[i];

	for (int i = 0; i < 6; i++) r->sel[i] = (uint16_t)cpu->seg[i].sel;
	for (int i = 0; i < 8; i++) r->gpr[i] = REGi(i);

	/*
	 * What the guest's own tables say about the selector the processor
	 * refused.  Read the way the interpreter reads it, so a descriptor
	 * that is itself unreachable comes back marked rather than invented;
	 * the exception being delivered is put back afterwards.
	 */
	r->desc_ok = 0;
	r->desc_lo = r->desc_hi = 0;
	if ((cpu->excerr & 0xfff8) && !(cpu->flags & VM)) {
		const int save_no = cpu->excno;
		const uword save_err = cpu->excerr;
		const uword save_cr2 = cpu->cr2;
		uword w1 = 0, w2 = 0;
		if (read_desc(cpu, (int)(cpu->excerr & 0xfff8), &w1, &w2)) {
			r->desc_lo = (uint32_t)w1;
			r->desc_hi = (uint32_t)w2;
			r->desc_ok = 1;
		}
		cpu->excno = save_no;
		cpu->excerr = save_err;
		cpu->cr2 = save_cr2;
	}

	/*
	 * The words the faulting instruction would have found on its stack.
	 * Read through the interpreter's own path so an unreadable stack
	 * comes back marked, and the delivered exception is put back after.
	 */
	r->stk_ok = 0;
	{
		const int save_no = cpu->excno;
		const uword save_err = cpu->excerr;
		const uword save_cr2 = cpu->cr2;
		const uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT
				    ? 0xffffffffu : 0xffffu;
		int got = 0;
		for (int i = 0; i < 16; i++) {
			OptAddr m;
			const uword off = (r->esp + (uword)(i * 4)) & sp_mask;
			/* A diagnostic peek past SS must not count as a guest refusal. */
			if (off < cpu->seg[SEG_SS].lo ||
			    off > cpu->seg[SEG_SS].hi ||
			    3u > cpu->seg[SEG_SS].hi - off) break;
			if (!translate32(cpu, &m, 1, SEG_SS, off)) break;
			r->stk[i] = (uint32_t)load32(cpu, &m);
			got++;
		}
		for (int i = got; i < 16; i++) r->stk[i] = 0;
		r->stk_ok = (uint8_t)got;
		cpu->excno = save_no;
		cpu->excerr = save_err;
		cpu->cr2 = save_cr2;
	}

	/*
	 * The trigger.  A general protection fault outside virtual-8086 mode
	 * with no selector in its error code is the one this ring exists for:
	 * it is a segment the instruction could not use, and it is not
	 * anything the V86 monitor does for a living.
	 */
	/*
	 * The trigger is a fault the machine cannot get past.
	 *
	 * Which vector it is turns out not to matter, and picking one in
	 * advance was wrong twice.  What distinguishes the end of a run is
	 * that the same fault comes back with nothing whatever changed -
	 * same vector, same error code, same address, same registers - with
	 * only the handler's own return fault in between.  Everything else
	 * the guest raises, however often, moves something on.
	 */
	if (exc_triggered && exc_after_left > 0) exc_after_left--;

	if (no == EX_UD && !exc_have_ud) {
		exc_first_ud = *r;
		exc_have_ud = 1;
	}
	/* Real mode has no monitor that uses #UD on purpose, so one there is
	 * code running where there is none - Win95 setup's mini-Windows
	 * landing in its own data after leaving protected mode.  Keep what
	 * led up to it before the loop that follows overwrites it. */
	if (no == EX_UD && !(cpu->cr0 & 1))
		cpu_exclog_freeze();
}

/*
 * One kept exception, oldest first, as a line of text.  Returns 0 when the
 * slot has never been written, so a caller can simply walk 0..CPU_EXCLOG_N-1.
 */
int cpu_exclog_report(int idx, char *out, int cap)
{
	static const char *const name[20] = {
		"#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
		"#DF", "x09", "#TS", "#NP", "#SS", "#GP", "#PF", "x0f",
		"#MF", "#AC", "#MC", "#XM"
	};
	if (idx < 0 || idx >= CPU_EXCLOG_N) return 0;
	const uint32_t held = exc_kept < CPU_EXCLOG_N ? exc_kept : CPU_EXCLOG_N;
	if ((uint32_t)idx >= held) return 0;
	const uint32_t first = exc_kept < CPU_EXCLOG_N
			     ? 0 : exc_head % CPU_EXCLOG_N;
	const struct CpuExcRec *r = &exc_ring[(first + idx) % CPU_EXCLOG_N];

	char code[3 * 8 + 1];
	int c = 0;
	for (int i = 0; i < r->codelen && c + 3 < (int)sizeof code; i++)
		c += snprintf(code + c, sizeof code - c, "%02x ", r->code[i]);
	code[c] = 0;

	char desc[40];
	if (r->desc_ok)
		snprintf(desc, sizeof desc, " desc=%08lx:%08lx",
			 (unsigned long)r->desc_hi, (unsigned long)r->desc_lo);
	else if (r->excerr & 0xfff8)
		snprintf(desc, sizeof desc, " desc=unreadable");
	else
		desc[0] = 0;

	return snprintf(out, cap,
		"%8lu.%03lus %s err=%04lx %04lx:%08lx cr2=%08lx "
		"fl=%08lx cpl=%u%s x%lu [%s]%s "
		"es=%04x ds=%04x ss=%04x fs=%04x gs=%04x "
		"ax=%08lx bx=%08lx cx=%08lx dx=%08lx si=%08lx di=%08lx "
		"bp=%08lx sp=%08lx stk=%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,"
		"%08lx,%08lx,%08lx,%08lx,%08lx,%08lx/%u",
		(unsigned long)(r->t_us / 1000000u),
		(unsigned long)(r->t_us / 1000u % 1000u),
		r->no < 20 ? name[r->no] : "???",
		(unsigned long)r->excerr,
		(unsigned long)r->cs_sel, (unsigned long)r->eip,
		(unsigned long)r->cr2,
		(unsigned long)r->eflags, (unsigned)r->cpl,
		(r->eflags & VM) ? " V86" : "",
		(unsigned long)(r->repeat + 1), code, desc,
		r->sel[0], r->sel[3], r->sel[2], r->sel[4], r->sel[5],
		(unsigned long)r->gpr[0], (unsigned long)r->gpr[3],
		(unsigned long)r->gpr[1], (unsigned long)r->gpr[2],
		(unsigned long)r->gpr[6], (unsigned long)r->gpr[7],
		(unsigned long)r->gpr[5], (unsigned long)r->gpr[4],
		(unsigned long)r->stk[0], (unsigned long)r->stk[1],
		(unsigned long)r->stk[2], (unsigned long)r->stk[3],
		(unsigned long)r->stk[4], (unsigned long)r->stk[5],
		(unsigned long)r->stk[6], (unsigned long)r->stk[7],
		(unsigned long)r->stk[8], (unsigned long)r->stk[9],
		(unsigned long)r->stk[10], (unsigned long)r->stk[11],
		(unsigned)r->stk_ok);
}

/*
 * Start the ring again, from the host.
 *
 * Deciding in advance which fault is the interesting one has been wrong
 * twice: the guest raises the same fault legitimately, over and over, and
 * only sometimes fails to get past it.  Being able to empty the ring at the
 * moment the machine is visibly stuck removes the guessing - whatever it is
 * doing then is what fills the ring.
 */
/*
 * Stop the rings where they are.
 *
 * The failure shows on the screen before it shows anywhere else, so the
 * person watching is the trigger: they say when, and what the machine was
 * doing at that moment is still in the ring.
 */
void cpu_exclog_freeze(void)
{
	if (exc_triggered) return;
	exc_triggered = 1;
	exc_after_left = 0;
	g_exc_frozen = 1;
	g_exc_frozen_at = time_us_32();
}

void cpu_exclog_rearm(void)
{
	g_exc_frozen = 0;
	exc_triggered = 0;
	exc_after_left = 0;
	exc_head = 0;
	exc_kept = 0;
	exc_have_ud = 0;
	seg_ref_head = 0;
	seg_clr_head = 0;
	v86_ent_head = 0;
	gate_ent_head = 0;
	pc_head = 0;
	xfer_head = 0;
	g_exc_frozen_at = 0;
}

/* The first invalid opcode of the run, whether or not the ring still holds
 * it.  Returns 0 when there has not been one. */
int cpu_exc_first_ud_report(char *out, int cap)
{
	if (!exc_have_ud) return 0;
	const struct CpuExcRec *r = &exc_first_ud;
	char code[3 * 8 + 1];
	int c = 0;
	for (int i = 0; i < r->codelen && c + 3 < (int)sizeof code; i++)
		c += snprintf(code + c, sizeof code - c, "%02x ", r->code[i]);
	code[c] = 0;
	return snprintf(out, cap,
		"%8lu.%03lus #UD %04lx:%08lx esp=%08lx fl=%08lx cpl=%u%s [%s]",
		(unsigned long)(r->t_us / 1000000u),
		(unsigned long)(r->t_us / 1000u % 1000u),
		(unsigned long)r->cs_sel, (unsigned long)r->eip,
		(unsigned long)r->esp, (unsigned long)r->eflags,
		(unsigned)r->cpl, (r->eflags & VM) ? " V86" : "", code);
}

/* How many of each vector the guest has taken, plus the page faults that the
 * ring deliberately does not keep. */
int cpu_exc_hist_report(char *out, int cap)
{
	static const char *const name[20] = {
		"#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
		"#DF", "x09", "#TS", "#NP", "#SS", "#GP", "#PF", "x0f",
		"#MF", "#AC", "#MC", "#XM"
	};
	int n = 0;
	for (int i = 0; i < 20 && n + 24 < cap; i++)
		if (g_exc_hist[i])
			n += snprintf(out + n, cap - n, "%s=%lu ", name[i],
				      (unsigned long)g_exc_hist[i]);
	if (g_exc_v86_monitor && n + 32 < cap)
		n += snprintf(out + n, cap - n, "(of which V86-monitor=%lu) ",
			      (unsigned long)g_exc_v86_monitor);
	if (g_exc_triple && n + 24 < cap)
		n += snprintf(out + n, cap - n, "TRIPLE=%lu ",
			      (unsigned long)g_exc_triple);
	if (g_exc_frozen_at && n + 40 < cap)
		n += snprintf(out + n, cap - n, "| triggered at %lu.%03lus",
			      (unsigned long)(g_exc_frozen_at / 1000000u),
			      (unsigned long)(g_exc_frozen_at / 1000u % 1000u));
	return n;
}

void cpui386_step(CPUI386 *cpu, int stepcount)
{
#if GUEST_PC_DIAG
	/*
	 * Where the guest is, sampled once every sixteen calls.
	 *
	 * A game that hangs with the CPU busy - park and zoop both spin after
	 * the DOS/4GW banner at 74% native coverage - says nothing through the
	 * exception counters or the JIT statistics: nothing faults and nothing
	 * is refused.  The only question is which address it is going round.
	 * The ring is in the PSRAM diagnostic hole, so it costs no SRAM.
	 */
	{
		static u32 nj_pcsample_tick;
		/*
		 * Slot 4 freezes the ring.  A guest that jumps into unmapped
		 * memory faults on every step afterwards, so a running ring
		 * holds nothing but the storm; frozen at the first invalid
		 * opcode it holds the path that got there instead.  Slot 5
		 * selects every step rather than every sixteenth, which is what
		 * that path needs.
		 */
		if (!((volatile u32 *)(0x11000000u + 0x000ab000u))[4] &&
		    (++nj_pcsample_tick &
		     (((volatile u32 *)(0x11000000u + 0x000ab000u))[5] ? 0u : 15u)) == 0u) {
			volatile u32 *r = (volatile u32 *)(0x11000000u + 0x000ab000u);
			const u32 h = r[0] % 256u;
			/*
			 * The physical address too: under EMM386 the guest runs
			 * paged, so a linear EIP says nothing about where the
			 * bytes are and the host cannot read the instruction that
			 * is spinning.  translate8r() is the interpreter's own
			 * read path; it can raise, so the exception record is put
			 * back afterwards.
			 */
			OptAddr res;
			const int save_excno = cpu->excno;
			const uword save_excerr = cpu->excerr;
			u32 phys = 0xffffffffu;
			if (translate8r(cpu, &res, SEG_CS, cpu->ip))
				phys = (u32)res.addr1;
			cpu->excno = save_excno;
			cpu->excerr = save_excerr;
			/*
			 * A probe the host can point anywhere: slot 1 holds a
			 * linear address, and slots 2 and 3 come back with its
			 * physical address and the dword there.  Without it a
			 * paged guest's variables cannot be read from outside at
			 * all - the page tables are the guest's, not ours.
			 */
			if (r[1]) {
				OptAddr pr;
				if (translate8r(cpu, &pr, SEG_DS, r[1])) {
					r[2] = (u32)pr.addr1;
					r[3] = *(volatile u32 *)(0x11000000u + pr.addr1);
				} else {
					r[2] = 0xffffffffu;
					r[3] = 0;
				}
				cpu->excno = save_excno;
				cpu->excerr = save_excerr;
			}
			r[8u + 3u * h] = cpu->seg[SEG_CS].base;
			r[8u + 3u * h + 1u] = cpu->ip | (cpu->halt ? 0x80000000u : 0u);
			r[8u + 3u * h + 2u] = phys;
			r[0]++;
		}
	}
#endif

	frank_diag_trace(cpu->seg[SEG_CS].base, cpu->ip);
	/*
	 * Why a pending interrupt is not being taken.
	 *
	 * The PIC latches one request per line, so a tick arriving while the
	 * previous one is still unacknowledged is lost outright.  Measured on
	 * hardware, a third of IRQ0 went that way while a guest's music ran
	 * slow, and polling the timer four times more often changed nothing -
	 * so the delay is here, not in the PIT.  These two counters separate
	 * a guest that is holding interrupts off itself from an emulator that
	 * is not offering them often enough.
	 */
	if (cpu->intr) {
		if (cpu->flags & IF) g_intr_taken++;
		else g_intr_blocked_if++;
	}
	if ((cpu->flags & IF) && cpu->intr) {
		cpu->intr = false;
		cpu->halt = false;
		int no = cpu->cb.pic_read_irq(cpu->cb.pic);
		/* The request was withdrawn or masked before we got here, so
		 * INTR is no longer asserted and there is nothing to take; see
		 * i8259_read_irq(). */
		if (no < 0) goto no_irq;
		CIRCLE_PC_IRQ_DELIVERED(no);
		g_wl_hw_irq++;
		cpu->ip = cpu->next_ip;
		if (!call_isr(cpu, no, false, 1)) {
			exclog_note(cpu, cpu->excno);
			if (!call_isr(cpu, EX_DF, true, 1)) {
				g_exc_triple++;
				cpui386_reset(cpu);
				return;
			}
		}
	}
no_irq:

	if (cpu->halt) {
		usleep(1);
		return;
	}

	if (!cpu_exec1(cpu, stepcount)) {
		bool pusherr = false;
		switch (cpu->excno) {
		case EX_PF: g_wl_exc_pf++; break;
		case EX_GP: g_wl_exc_gp++; break;
		default:    g_wl_exc_other++; break;
		}
		/*
		 * The delivered exception, for the host tools.  This is the one
		 * place worth recording it: THROW expands at hundreds of sites and
		 * putting the record there added 8 KB of .data and took the board
		 * over the edge, while every exception the guest actually takes
		 * passes through here.  The block is in PSRAM, so it costs no SRAM.
		 */
		switch (cpu->excno) {
		case EX_DF: case EX_TS: case EX_NP: case EX_SS: case EX_GP:
		case EX_PF:
			pusherr = true;
		}
		cpu->next_ip = cpu->ip;
		exclog_note(cpu, cpu->excno);

		if (cpu->excno == EX_DF) {
			if (!call_isr(cpu, EX_DF, true, 1)) {
				g_exc_triple++;
				cpui386_reset(cpu);
				return;
			}
		} else if (!call_isr(cpu, cpu->excno, pusherr, 1)) {
			exclog_note(cpu, cpu->excno);
			if (!call_isr(cpu, EX_DF, true, 1)) {
				g_exc_triple++;
				cpui386_reset(cpu);
				return;
			}
		}
	}
}

void cpu_setax(CPUI386 *cpu, u16 ax)
{
	sreg16(0, ax);
}

u16 cpu_getax(CPUI386 *cpu)
{
	return lreg16(0);
}

void cpu_setexc(CPUI386 *cpu, int excno, uword excerr)
{
	frank_diag_exc(cpu->seg[SEG_CS].base, cpu->ip, (uint32_t)excno,
	               (uint32_t)excerr, (uint32_t)cpu->flags);
	cpu->excno = excno;
	cpu->excerr = excerr;
}

void cpu_setflags(CPUI386 *cpu, uword set_mask, uword clear_mask)
{
	if (cpu->cc.mask & (set_mask | clear_mask)) {
		refresh_flags(cpu);
		cpu->cc.mask = 0;
	}
	cpu->flags |= set_mask;
	cpu->flags &= ~clear_mask;
	cpu->flags &= EFLAGS_MASK;
}

uword cpu_getflags(CPUI386 *cpu)
{
	if (cpu->cc.mask) {
		refresh_flags(cpu);
		cpu->cc.mask = 0;
	}
	return cpu->flags;
}

void cpui386_reset(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++) {
		REGi(i) = 0;
	}
	cpu->flags = 0x2;
	cpu->cpl = 0;
	cpu->code16 = true;
	cpu->sp_mask = 0xffff;
	cpu->halt = false;
	cpu->power_off = false;

	for (int i = 0; i < 8; i++) {
		cpu->seg[i].sel = 0;
		cpu->seg[i].base = 0;
		cpu->seg[i].limit = 0;
		cpu->seg[i].flags = 0;
		/* Out of reset the machine is in real mode, where the limit
		 * is not checked; see the note in i386.h. */
		cpu->seg[i].lo = 0;
		cpu->seg[i].hi = 0xffffffffu;
	}
	cpu->seg[2].flags = (1 << 22);
	cpu->seg[1].flags = (1 << 22);

	cpu->ip = 0xfff0;
	cpu->next_ip = cpu->ip; cpu->prefetch_base = (u32)-1;
	cpu->seg[SEG_CS].sel = 0xf000;
	cpu->seg[SEG_CS].base = 0xf0000;

	cpu->idt.base = 0;
	cpu->idt.limit = 0x3ff;
	cpu->gdt.base = 0;
	cpu->gdt.limit = 0;

	cpu->cr0 = cpu->fpu ? 0x10 : 0;
	cpu->cr2 = 0;
	cpu->cr3 = 0;
	for (int i = 0; i < 8; i++)
		cpu->dr[i] = 0;

	cpu->cc.mask = 0;
	tlb_clear(cpu);

	cpu->sysenter.cs = 0;
	cpu->sysenter.eip = 0;
	cpu->sysenter.esp = 0;
}

void cpui386_reset_pm(CPUI386 *cpu, uint32_t start_addr)
{
	cpui386_reset(cpu);
	cpu->cr0 = 1;
	cpu->seg[SEG_CS].sel = 0x8;
	cpu->seg[SEG_CS].base = 0;
	cpu->seg[SEG_CS].limit = 0xffffffff;
	cpu->seg[SEG_CS].flags = SEG_D_BIT;
	cpu->seg[SEG_CS].lo = 0;
	cpu->seg[SEG_CS].hi = 0xffffffffu;
	cpu->next_ip = start_addr;
	cpu->cpl = 0;
	cpu->code16 = false;
	cpu->sp_mask = 0xffffffff;
	cpu->seg[SEG_SS].sel = 0x10;
	cpu->seg[SEG_SS].base = 0;
	cpu->seg[SEG_SS].limit = 0xffffffff;
	cpu->seg[SEG_SS].flags = SEG_B_BIT;
	cpu->seg[SEG_SS].lo = 0;
	cpu->seg[SEG_SS].hi = 0xffffffffu;

	cpu->seg[SEG_DS] = cpu->seg[SEG_SS];
	cpu->seg[SEG_ES] = cpu->seg[SEG_SS];
}

void IRAM_ATTR cpui386_raise_irq(CPUI386 *cpu)
{
	cpu->intr = true;
}

void cpui386_set_gpr(CPUI386 *cpu, int i, u32 val)
{
	sreg32(i, val);
}

long IRAM_ATTR cpui386_get_cycle(CPUI386 *cpu)
{
	return cpu->cycle;
}

/*
 * FRANK_WORKLOAD_PROFILE_V88: mode snapshot taken when the stats file is
 * written.  Together with the backedge split it distinguishes "the guest was
 * not in VM86+paging during the window" from "it was, and the JIT still saw
 * nothing worth compiling" - two situations that every previous capture in
 * this sequence left indistinguishable.
 */
void cpui386_diag_mode(CPUI386 *cpu, u32 out[5])
{
	out[0] = (u32)cpu->cr0;
	out[1] = (u32)cpu->flags;
	out[2] = (u32)cpu->cr3;
	out[3] = (u32)cpu->cpl;
	out[4] = (u32)cpu->code16;
}

CPUI386 *cpui386_new(int gen, char *phys_mem, long phys_mem_size, CPU_CB **cb)
{
	/*
	 * Clear the diagnostic latches.
	 *
	 * They live in the PSRAM hole at guest physical 0xa8000, and psram_test()
	 * does not leave the whole of it zeroed - slots read back as values like
	 * 997109, which is enough to make a "have I latched yet" flag say yes
	 * before anything has happened.  Two diagnostics were read as real
	 * findings that way before this was noticed, so zero them here, once, at
	 * the only point that is guaranteed to run after the memory test and
	 * before the guest.
	 */
#if NJIT_EXIT_RING
	{
		volatile u32 *d_ = (volatile u32 *)(0x11000000u + 0x000a8000u);
		for (unsigned k = NJ_V6_EXC; k <= NJ_V6_ISR_SEEN; k++)
			d_[k] = 0u;
	}
#endif

	CPUI386 *cpu = malloc(sizeof(CPUI386));
	/* These callbacks are optional and are consulted from the real-mode INT
	 * 2F path.  malloc() is deliberately not assumed to return zeroed memory:
	 * leaving either field uninitialised can branch through a stale/poisoned
	 * function pointer during BIOS/DOS startup.  cpui386_reset() intentionally
	 * leaves them alone so a configured host hook survives a CPU reset. */
	cpu->int2f_handler = NULL;
	cpu->int2f_opaque = NULL;
	switch (gen) {
	case 3: cpu->flags_mask = EFLAGS_MASK_386; break;
	case 4: cpu->flags_mask = EFLAGS_MASK_486; break;
	case 5: case 6: cpu->flags_mask = EFLAGS_MASK_586; break;
	default: assert(false);
	}
	cpu->gen = gen;

	cpu->tlb.size = tlb_size;
#ifdef BUILD_ESP32
	{
		extern void *pcmalloc(long size);
		size_t tlb_bytes = sizeof(struct tlb_entry) * tlb_size;
		cpu->tlb.tab = malloc(tlb_bytes);
		if (!cpu->tlb.tab)
			cpu->tlb.tab = pcmalloc(tlb_bytes);
	}
#else
	cpu->tlb.tab = malloc(sizeof(struct tlb_entry) * tlb_size);
#endif

	cpu->phys_mem = (u8 *) phys_mem;
	cpu->phys_mem_size = phys_mem_size;

	cpu->cycle = 0;

	cpu->intr = false;

	cpu->fpu = NULL;

	cpui386_reset(cpu);

	memset(&(cpu->cb), 0, sizeof(CPU_CB));
	if (cb)
		*cb = &(cpu->cb);
	return cpu;
}

void cpui386_enable_fpu(CPUI386 *cpu)
{
	if (!cpu->fpu)
		cpu->fpu = fpu_new();
}

/*
 * What the processor is, after it has been built.
 *
 * The generation is read in one place - CPUID - and the memory size is a
 * bound on physical accesses, so both can be changed while the emulator is
 * stopped between instructions.  That is what lets the settings menu offer a
 * restart that means something without a teardown for every device.
 */
void cpui386_set_gen(CPUI386 *cpu, int gen)
{
	cpu->gen = gen;
}

void cpui386_set_fpu(CPUI386 *cpu, int enabled)
{
	if (enabled && !cpu->fpu) {
		cpu->fpu = fpu_new();
	} else if (!enabled && cpu->fpu) {
		fpu_delete(cpu->fpu);
		cpu->fpu = NULL;
	}
}

void cpui386_set_mem_size(CPUI386 *cpu, long size)
{
	cpu->phys_mem_size = size;
}

void cpui386_delete(CPUI386 *cpu)
{
	if (cpu->fpu)
		fpu_delete(cpu->fpu);
	free(cpu);
}

#if !defined(_WIN32) && !defined(__wasm__) && !defined(CIRCLE_BUILD)
void cpui386_set_verbose() // for debugging
{
	verbose = true;
	freopen("/tmp/xlog", "w", stderr);
	setlinebuf(stderr);
}
#endif

static void cpu_debug(CPUI386 *cpu)
{
	static int nest;
	if (nest >= 1)
		return;
	nest++;
	bool code32 = cpu->seg[SEG_CS].flags & SEG_D_BIT;
	bool stack32 = cpu->seg[SEG_SS].flags & SEG_B_BIT;

	dolog("IP %08x|AX %08x|CX %08x|DX %08x|BX %08x|SP %08x|BP %08x|SI %08x|DI %08x|FL %08x|CS %04x|DS %04x|SS %04x|ES %04x|FS %04x|GS %04x|CR0 %08x|CR2 %08x|CR3 %08x|CPL %d|IOPL %d|CSBASE %08x/%08x|DSBASE %08x/%08x|SSBASE %08x/%08x|ESBASE %08x/%08x|GSBASE %08x/%08x %c%c\n",
		cpu->ip, REGi(0), REGi(1), REGi(2), REGi(3),
		REGi(4), REGi(5), REGi(6), REGi(7),
		cpu->flags, SEGi(SEG_CS), SEGi(SEG_DS), SEGi(SEG_SS),
		SEGi(SEG_ES), SEGi(SEG_FS), SEGi(SEG_GS),
		cpu->cr0, cpu->cr2, cpu->cr3, cpu->cpl, get_IOPL(cpu),
		cpu->seg[SEG_CS].base, cpu->seg[SEG_CS].limit,
		cpu->seg[SEG_DS].base, cpu->seg[SEG_DS].limit,
		cpu->seg[SEG_SS].base, cpu->seg[SEG_SS].limit,
		cpu->seg[SEG_ES].base, cpu->seg[SEG_ES].limit,
		cpu->seg[SEG_GS].base, cpu->seg[SEG_GS].limit,
		code32 ? 'D' : ' ', stack32 ? 'B' : ' ');
	uword cr2, excno, excerr;
	cr2 = cpu->cr2;
	excno = cpu->excno;
	excerr = cpu->excerr;
	dolog("code: ");
	for (int i = 0; i < 32; i++) {
		OptAddr res;
		if(translate8(cpu, &res, 1, SEG_CS, cpu->ip + i))
			dolog(" %02x", load8(cpu, &res));
		else
			dolog(" ??");
	}
	dolog("\n");
	dolog("stack: ");
	uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
	for (int i = 0; i < 32; i++) {
		OptAddr res;
		if(translate8(cpu, &res, 1, SEG_SS, (REGi(4) + i) & sp_mask))
			dolog(" %02x", load8(cpu, &res));
		else
			dolog(" ??");
	}
	dolog("\n");
	dolog("stkf : ");
	for (int i = 0; i < 32; i++) {
		OptAddr res;
		if(translate8(cpu, &res, 1, SEG_SS, (REGi(5) + i) & sp_mask))
			dolog(" %02x", load8(cpu, &res));
		else
			dolog(" ??");
	}
	dolog("\n");

	cpu->cr2 = cr2;
	cpu->excno = excno;
	cpu->excerr = excerr;
	nest--;
}

void cpu_set_a20(CPUI386 *cpu, int enabled)
{
	cpu->a20_mask = enabled ? 0xFFFFFFFFu : 0xFFEFFFFFu;
}

int cpu_get_a20(CPUI386 *cpu)
{
	return cpu->a20_mask >> 20 & 1;
}

void cpui386_get_state(CPUI386 *cpu, uint32_t *cs, uint32_t *ip, int *halt)
{
	*cs = cpu->seg[1].sel;  // SEG_CS = 1
	*ip = cpu->ip;
	*halt = cpu->halt ? 1 : 0;
}

u8 *cpu_get_phys_mem(CPUI386 *cpu) { return cpu->phys_mem; }


// Register accessors for disk/BIOS emulation
u8 cpu_get_al(CPUI386 *cpu) { return lreg8(0); }
u8 cpu_get_ah(CPUI386 *cpu) { return lreg8(4); }
u8 cpu_get_bl(CPUI386 *cpu) { return lreg8(3); }
u8 cpu_get_bh(CPUI386 *cpu) { return lreg8(7); }
u8 cpu_get_cl(CPUI386 *cpu) { return lreg8(1); }
u8 cpu_get_ch(CPUI386 *cpu) { return lreg8(5); }
u8 cpu_get_dl(CPUI386 *cpu) { return lreg8(2); }
u8 cpu_get_dh(CPUI386 *cpu) { return lreg8(6); }

void cpu_set_al(CPUI386 *cpu, u8 val) { sreg8(0, val); }
void cpu_set_ah(CPUI386 *cpu, u8 val) { sreg8(4, val); }
void cpu_set_bl(CPUI386 *cpu, u8 val) { sreg8(3, val); }
void cpu_set_bh(CPUI386 *cpu, u8 val) { sreg8(7, val); }
void cpu_set_cl(CPUI386 *cpu, u8 val) { sreg8(1, val); }
void cpu_set_ch(CPUI386 *cpu, u8 val) { sreg8(5, val); }
void cpu_set_dl(CPUI386 *cpu, u8 val) { sreg8(2, val); }
void cpu_set_dh(CPUI386 *cpu, u8 val) { sreg8(6, val); }

u16 cpu_get_bx(CPUI386 *cpu) { return lreg16(3); }
u16 cpu_get_cx(CPUI386 *cpu) { return lreg16(1); }
u16 cpu_get_dx(CPUI386 *cpu) { return lreg16(2); }
u16 cpu_get_es(CPUI386 *cpu) { return cpu->seg[SEG_ES].sel; }

void cpu_set_bx(CPUI386 *cpu, u16 val) { sreg16(3, val); }
void cpu_set_cx(CPUI386 *cpu, u16 val) { sreg16(1, val); }
void cpu_set_dx(CPUI386 *cpu, u16 val) { sreg16(2, val); }
u16 cpu_get_bp(CPUI386 *cpu) { return lreg16(5); }
u16 cpu_get_si(CPUI386 *cpu) { return lreg16(6); }
u16 cpu_get_di(CPUI386 *cpu) { return lreg16(7); }
void cpu_set_si(CPUI386 *cpu, u16 val) { sreg16(6, val); }
void cpu_set_di(CPUI386 *cpu, u16 val) { sreg16(7, val); }
u16 cpu_get_ds(CPUI386 *cpu) { return cpu->seg[SEG_DS].sel; }
u16 cpu_get_ss(CPUI386 *cpu) { return cpu->seg[SEG_SS].sel; }

void cpu_set_cf(CPUI386 *cpu, int val)
{
	if (cpu->cc.mask & CF) {
		refresh_flags(cpu);
		cpu->cc.mask = 0;
	}
	if (val)
		cpu->flags |= CF;
	else
		cpu->flags &= ~CF;
}

int cpu_get_cf(CPUI386 *cpu)
{
	if (cpu->cc.mask & CF) {
		refresh_flags(cpu);
		cpu->cc.mask = 0;
	}
	return (cpu->flags & CF) ? 1 : 0;
}

void cpu_set_int2f_handler(CPUI386 *cpu, int2f_handler_t handler, void *opaque)
{
	cpu->int2f_handler = handler;
	cpu->int2f_opaque  = opaque;
}
