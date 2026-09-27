#ifndef I386_H
#define I386_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef int32_t s32;
typedef int16_t s16;
typedef int8_t s8;

typedef u32 uword;
typedef s32 sword;

/* Enable optimized register layout (union-based) */
#ifndef I386_OPT1
#define I386_OPT1
#endif

/* Forward declaration for FPU */
typedef struct FPU FPU;

/* CPU callback structure - must be defined before CPUI386 */
typedef struct {
	void *pic;
	int (*pic_read_irq)(void *);

	void *io;
	u8 (*io_read8)(void *, int);
	void (*io_write8)(void *, int, u8);
	u16 (*io_read16)(void *, int);
	void (*io_write16)(void *, int, u16);
	u32 (*io_read32)(void *, int);
	void (*io_write32)(void *, int, u32);
	int (*io_read_string)(void *, int, uint8_t *, int, int);
	int (*io_write_string)(void *, int, uint8_t *, int, int);

	void *iomem;
	u8 (*iomem_read8)(void *, uword);
	void (*iomem_write8)(void *, uword, u8);
	u16 (*iomem_read16)(void *, uword);
	void (*iomem_write16)(void *, uword, u16);
	u32 (*iomem_read32)(void *, uword);
	void (*iomem_write32)(void *, uword, u32);
	bool (*iomem_write_string)(void *, uword, uint8_t *, int);
} CPU_CB;

/* TLB entry structure */
struct tlb_entry {
	uword lpgno;
	uword xaddr;
	/*
	 * Whether the access is refused, for each privilege and direction:
	 * index [(cpl > 0) << 1 | (rwm > 1)].
	 *
	 * Held here rather than as a pointer into the permission table.  A
	 * pointer means two dependent loads to answer one question, and this
	 * is on the path every operand in guest memory takes - thirty-nine
	 * million times every ten seconds on Tyrian.
	 */
	u8 deny[4];
	u8 *ppte;
};

/*
 * CPUI386 structure - main CPU state
 * Defined here so JIT compiler can access fields directly
 */
/* How a deferred flag result is to be interpreted.  Here rather than in
 * i386.c because the translator writes the same fields. */
/* lazy flags */
enum {
	CC_ADC, CC_ADD,	CC_SBB, CC_SUB,
	CC_NEG8, CC_NEG16, CC_NEG32,
	CC_DEC8, CC_DEC16, CC_DEC32,
	CC_INC8, CC_INC16, CC_INC32,
	CC_IMUL8, CC_IMUL16, CC_IMUL32,	CC_MUL8, CC_MUL16, CC_MUL32,
	CC_SAR, CC_SHL, CC_SHR,
	CC_SHLD, CC_SHRD, CC_BSF, CC_BSR,
	CC_AND, CC_OR, CC_XOR,
};

/* Which entry of seg[] is which.  Here rather than in i386.c because the
 * translator indexes the same array. */
enum {
	SEG_ES = 0,
	SEG_CS,
	SEG_SS,
	SEG_DS,
	SEG_FS,
	SEG_GS,
	SEG_LDT,
	SEG_TR,
};

struct CPUI386 {
#ifdef I386_OPT1
	union {
		u32 r32;
		u16 r16;
		u8 r8[2];
	} gprx[8];
#else
	uword gpr[8];
#endif
	uword ip, next_ip;
	uword flags;
	uword flags_mask;
	int cpl;
	bool code16;
	uword sp_mask;
	bool halt;
	/* Halted in the BIOS with interrupts off: the APM "power off" path,
	 * after which nothing but a reset runs again.  See HLT in i386.c. */
	bool power_off;

	FPU *fpu;

	struct {
		uword sel;
		uword base;
		uword limit;
		uword flags;
		/*
		 * The offsets this segment will actually accept, worked out
		 * when it is loaded so the check on every access is two
		 * comparisons rather than a reading of the type bits.
		 *
		 * For an ordinary segment that is 0..limit.  For an
		 * expand-down one - a stack that grows downwards - the valid
		 * range is the other side of the limit, so lo is limit+1 and
		 * hi is the top the B bit allows.  Where no check applies
		 * (real mode, virtual-8086) the pair is opened wide, which
		 * keeps those modes exactly as they were.  A null selector
		 * gets lo above hi, so every access through it fails.
		 */
		uword lo, hi;
	} seg[8];

	struct {
		uword base;
		uword limit;
	} idt, gdt;

	uword cr0, cr2, cr3;

	uword dr[8];

	struct {
		unsigned long laddr;
		uword xaddr;
	} ifetch;

	struct {
		int op;
		uword dst;
		uword dst2;
		uword src1;
		uword src2;
		uword mask;
	} cc;

	struct {
		int size;
		struct tlb_entry *tab;
	} tlb;

	u8 *phys_mem;
	u32 prefetch_base;
	/* The same line by physical address, so a write can be told to land in
	 * it whatever linear address the writer used.  See store8(). */
	u32 prefetch_pbase;
	/* Instruction prefetch line.  32 bytes, not 16: the RP2350's XIP cache
	 * line is 32 bytes, so a wider fill costs the same one miss into PSRAM
	 * and halves how often peek8_slow() has to refill. */
	u8  prefetch[32] __attribute__((aligned(4)));
	long phys_mem_size;

	long cycle;

	int excno;
	uword excerr;

	/*
	 * One port answered without going through the dispatch.
	 *
	 * A game waiting for the frame reads 0x3DA and nothing else, and that
	 * is one guest instruction in six.  The general path is an indirect
	 * call into pc.c, a wrapper, and a test for this very port before the
	 * switch; naming it here leaves the call and drops the rest.
	 */
	int io_fast_port;
	u8 (*io_fast_fn)(void *);
	void *io_fast_arg;

	bool intr;
	CPU_CB cb;

	int gen;
	struct {
		uword cs, eip, esp;
	} sysenter;

	/* INT 2Fh network attached drive handler hook */
	bool (*int2f_handler)(struct CPUI386 *cpu, void *opaque);
	void *int2f_opaque;

	u32 a20_mask;  /* 0xFFFFFFFF = A20 on, 0xFFEFFFFF = A20 off */

	/*
	 * Native-JIT block linking accounting.  Generated code that jumps
	 * straight back into its own body instead of returning to the C
	 * dispatcher has to carry the retired-instruction count and the
	 * dispatcher's step budget somewhere; there is no spare ARM register
	 * once the eight guest GPRs, the CPU pointer, the budget/next-IP
	 * carrier and the memory-guard scratch are allocated, so both live
	 * here and are addressed off r12.
	 *
	 * njl_acc is cleared and njl_limit is set by nj_exec_loop() before
	 * every native entry; only self-linking blocks read or write them.
	 */
	u32 njl_acc;
	u32 njl_limit;
	/* Instruction index inside the block that the last native link
	 * jumped to, so an exit stub's static position still yields the
	 * number actually retired. */
	u32 njl_pos;
};

typedef struct CPUI386 CPUI386;

/*
 * The exceptions the guest took, for a fault that cannot be reproduced on
 * demand.  cpu_exclog_report() writes one kept entry, oldest first, and
 * returns 0 once the ring runs out; cpu_exc_hist_report() writes the totals,
 * page faults included - those are counted but not kept.  See i386.c.
 */
#define CPU_EXCLOG_N 64
int cpu_exclog_report(int idx, char *out, int cap);
int cpu_exc_hist_report(char *out, int cap);
/* The first #UD of the run, kept apart from the ring so it cannot be lost. */
int cpu_exc_first_ud_report(char *out, int cap);
/* What a segment limit check would have refused, had this emulator made one. */
int cpu_seg_limit_report(char *out, int cap);
int cpu_seg_ref_line(int idx, char *out, int cap);
int cpu_seg_clr_line(int idx, char *out, int cap);
int cpu_v86_entry_line(int idx, char *out, int cap);
int cpu_gate_line(int idx, char *out, int cap);
int cpu_pc_line(int idx, char *out, int cap);
/* Changes of code segment and of CR0.PE; empty unless CPU_XFER_TRACE. */
int cpu_xfer_line(int idx, char *out, int cap);
/* Empty the ring and let it fill again; see i386.c. */
void cpu_exclog_rearm(void);
void cpu_exclog_freeze(void);

CPUI386 *cpui386_new(int gen, char *phys_mem, long phys_mem_size, CPU_CB **cb);
void cpui386_delete(CPUI386 *cpu);
void cpui386_enable_fpu(CPUI386 *cpu);
/* Drop the instruction prefetch; for writers that bypass the store path. */
void cpu_prefetch_invalidate(CPUI386 *cpu);
/*
 * A watch on four bytes of guest memory, for finding who overwrites them.
 * Built only with CPU_STACK_WATCH; see i386.c.  The bus hook is for writers
 * that do not go through the CPU's store path (DMA); tag says which.
 */
#if defined(CPU_STACK_WATCH)
void cpu_watch_bus_write(CPUI386 *cpu, uint32_t addr, uint32_t len, uint32_t tag);
int cpu_watch_line(int idx, char *out, int cap);
#else
#define cpu_watch_bus_write(cpu, addr, len, tag) ((void)0)
#endif
void cpui386_set_gen(CPUI386 *cpu, int gen);
#if defined(CIRCLE_PC_STATS)
extern uint32_t g_opcode_hist[256];
extern uint64_t g_io_perm_cycles, g_io_read_cycles;
extern uint32_t g_io_count;
/* The half-dozen guest addresses the sampler saw most, newest window first;
 * writes at most len bytes and returns how many. */
int i386_hot_report(char *out, int len);
/* Bitmask of 32-byte blocks captured around the hottest address.  Missing
 * blocks must not be presented as guest code. */
int i386_hot_code(uint32_t *base, const uint8_t **bytes);
#endif
void cpui386_set_fpu(CPUI386 *cpu, int enabled);
void cpui386_set_mem_size(CPUI386 *cpu, long size);
void cpui386_reset(CPUI386 *cpu);
void cpui386_reset_pm(CPUI386 *cpu, uint32_t start_addr);
void cpui386_step(CPUI386 *cpu, int stepcount);
void cpui386_raise_irq(CPUI386 *cpu);
void cpui386_set_gpr(CPUI386 *cpu, int i, u32 val);
long cpui386_get_cycle(CPUI386 *cpu);
void cpui386_get_state(CPUI386 *cpu, uint32_t *cs, uint32_t *ip, int *halt);

bool cpu_load8(CPUI386 *cpu, int seg, uword addr, u8 *res);
bool cpu_store8(CPUI386 *cpu, int seg, uword addr, u8 val);
bool cpu_load16(CPUI386 *cpu, int seg, uword addr, u16 *res);
bool cpu_store16(CPUI386 *cpu, int seg, uword addr, u16 val);
bool cpu_load32(CPUI386 *cpu, int seg, uword addr, u32 *res);
bool cpu_store32(CPUI386 *cpu, int seg, uword addr, u32 val);
void cpu_setax(CPUI386 *cpu, u16 ax);
u16 cpu_getax(CPUI386 *cpu);
void cpu_setexc(CPUI386 *cpu, int excno, uword excerr);
void cpu_setflags(CPUI386 *cpu, uword set_mask, uword clear_mask);
uword cpu_getflags(CPUI386 *cpu);
void cpu_abort(CPUI386 *cpu, int code);

// Register accessors for disk/BIOS emulation
// 8-bit registers
u8 cpu_get_al(CPUI386 *cpu);
u8 cpu_get_ah(CPUI386 *cpu);
u8 cpu_get_bl(CPUI386 *cpu);
u8 cpu_get_bh(CPUI386 *cpu);
u8 cpu_get_cl(CPUI386 *cpu);
u8 cpu_get_ch(CPUI386 *cpu);
u8 cpu_get_dl(CPUI386 *cpu);
u8 cpu_get_dh(CPUI386 *cpu);
void cpu_set_al(CPUI386 *cpu, u8 val);
void cpu_set_ah(CPUI386 *cpu, u8 val);
void cpu_set_bl(CPUI386 *cpu, u8 val);
void cpu_set_bh(CPUI386 *cpu, u8 val);
void cpu_set_cl(CPUI386 *cpu, u8 val);
void cpu_set_ch(CPUI386 *cpu, u8 val);
void cpu_set_dl(CPUI386 *cpu, u8 val);
void cpu_set_dh(CPUI386 *cpu, u8 val);
// 16-bit registers
u16 cpu_get_bx(CPUI386 *cpu);
u16 cpu_get_cx(CPUI386 *cpu);
u16 cpu_get_dx(CPUI386 *cpu);
u16 cpu_get_es(CPUI386 *cpu);
void cpu_set_bx(CPUI386 *cpu, u16 val);
void cpu_set_cx(CPUI386 *cpu, u16 val);
void cpu_set_dx(CPUI386 *cpu, u16 val);
// Carry flag
u16 cpu_get_bp(CPUI386 *cpu);
u16 cpu_get_si(CPUI386 *cpu);
u16 cpu_get_di(CPUI386 *cpu);
void cpu_set_si(CPUI386 *cpu, u16 val);
void cpu_set_di(CPUI386 *cpu, u16 val);
u16 cpu_get_ds(CPUI386 *cpu);
u16 cpu_get_ss(CPUI386 *cpu);
void cpu_set_cf(CPUI386 *cpu, int val);
int cpu_get_cf(CPUI386 *cpu);
// Physical memory access
u8 *cpu_get_phys_mem(CPUI386 *cpu);
long cpu_get_phys_mem_size(CPUI386 *cpu);
// A20 gate control
void cpu_set_a20(CPUI386 *cpu, int enabled);
int cpu_get_a20(CPUI386 *cpu);

typedef bool (*int2f_handler_t)(CPUI386 *cpu, void *opaque);
void cpu_set_int2f_handler(CPUI386 *cpu, int2f_handler_t handler, void *opaque);

/* Profiling support (enable with -DI386_PROFILE) */
#ifdef I386_PROFILE
void i386_profile_dump(void);
void i386_profile_reset(void);
#endif

#endif /* I386_H */
