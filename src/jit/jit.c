/*
 * Translating guest code to AArch64.  See jit.h for what this promises.
 *
 * The shape of it
 * ---------------
 * A block is a run of guest instructions translated into one stretch of
 * AArch64 and called as an ordinary function.  On entry it loads the guest's
 * eight general registers into eight host registers and keeps them there; on
 * exit it stores them back.  That is where the win is meant to come from:
 * the interpreter reaches through a pointer for every operand, and measuring
 * it showed the machine stalled on dependent loads far more than on anything
 * else.
 *
 * What it does not do yet
 * -----------------------
 * Flags.  Every instruction translated so far leaves them alone, so nothing
 * here can get them wrong.  They are the next step and the one that has to
 * be right: the interpreter defers them, and a translator can do better
 * still by seeing that a comparison is consumed by the very next jump - but
 * it is also where the previous attempt at this went wrong.
 *
 * Memory operands, branches and anything with a prefix end a block.
 */

#include "jit.h"

#ifdef TINY386_JIT

#include "arm64_emit.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------- */
/* Where the guest's state lives, as far as translated code is concerned   */
/* ---------------------------------------------------------------------- */

/*
 * x19 holds the CPU structure and x20 to x27 hold the guest's eight
 * registers, in the x86 numbering: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI.
 * x28 holds next_ip.  All of those are callee-saved, so a call out to a
 * helper cannot lose them, and the block saves them for its own caller.
 */
#define R_CPU    19
#define R_GPR(i) (20 + (i))
#define R_IP     28
#define R_T0     0
#define R_T1     1
#define R_T2     2

#define OFF_GPR(i) ((uint32_t)(offsetof(CPUI386, gprx) + 4u * (i)))
#define OFF_NEXTIP ((uint32_t)offsetof(CPUI386, next_ip))

/*
 * The bits of EFLAGS and the deferred-result mask that this needs.
 *
 * i386.c has the full enumeration.  These are repeated rather than shared
 * because fpu.c already has an enum using the same names, so moving them to
 * the header collides with it.
 */
#define F_VM            0x20000u
#define CC_MASK_LOGIC   0x8c5u          /* CF | PF | ZF | SF | OF */

/* Iterations a block may make round its own loop before returning, so that
 * a guest waiting for the frame does not keep the emulator from servicing
 * its devices. */
#ifndef JIT_LOOP_BUDGET
#define JIT_LOOP_BUDGET 256
#endif

#define OFF_CC_DST   ((uint32_t)offsetof(CPUI386, cc.dst))
#define OFF_CC_OP    ((uint32_t)offsetof(CPUI386, cc.op))
#define OFF_CC_MASK  ((uint32_t)offsetof(CPUI386, cc.mask))
#define OFF_CR0      ((uint32_t)offsetof(CPUI386, cr0))
#define OFF_FLAGS    ((uint32_t)offsetof(CPUI386, flags))
#define OFF_CPL      ((uint32_t)offsetof(CPUI386, cpl))
#define OFF_CB_IO    ((uint32_t)(offsetof(CPUI386, cb) + offsetof(CPU_CB, io)))
#define OFF_CB_IN8   ((uint32_t)(offsetof(CPUI386, cb) + offsetof(CPU_CB, io_read8)))
#define OFF_ACC      ((uint32_t)offsetof(CPUI386, njl_acc))
#define OFF_LIMIT    ((uint32_t)offsetof(CPUI386, njl_limit))


/* ---------------------------------------------------------------------- */
/* The arena and the block table                                           */
/* ---------------------------------------------------------------------- */

/*
 * The arena has to live where code may be executed from, which on this
 * target is a narrow place.  Circle marks every page above _etext
 * privileged-execute-never, so the heap and the stack are out: a block
 * emitted there aborts on its first instruction, which is exactly how this
 * was found.  Pages below _etext are both writable and executable, so the
 * arena is a static array put into .text by its section attribute.
 *
 * That costs image size, because .text is loaded, which is why it is 64 KB
 * and not the megabyte the board could spare.  Growing it means giving the
 * arena a section of its own that is allocated but not loaded, and that
 * means editing the linker script in the pinned Circle tree.
 */
#ifndef JIT_ARENA_BYTES
#define JIT_ARENA_BYTES (64u << 10)
#endif
#ifndef JIT_BLOCKS
#define JIT_BLOCKS 1024u                /* direct mapped, power of two */
#endif
/* A block stops here whatever comes next, so one runaway translation cannot
 * eat the arena. */
#ifndef JIT_MAX_INSNS
#define JIT_MAX_INSNS 64
#endif

typedef struct {
	uint32_t laddr;         /* guest linear address this block starts at */
	uint32_t valid;         /* zero means the slot is empty */
	uint32_t page;          /* physical page it was translated from */
	uint32_t insns;         /* how many guest instructions it covers */
	uint32_t (*fn)(CPUI386 *);
} JitBlock;

static uint8_t arena_store[JIT_ARENA_BYTES]
	__attribute__((section(".text.jitarena"), aligned(64)));
static uint8_t *arena;
static uint32_t arena_used;
static JitBlock blocks[JIT_BLOCKS];
/* One bit per 4 KB of guest memory below 16 MB: does any block come from
 * there?  A store outside the marked pages costs a shift and a test. */
static uint8_t code_pages[(16u << 20) / 4096u / 8u];
/* How often each address has been entered, before it is worth translating.
 * More slots than blocks, because this is what decides which addresses get
 * one. */
#ifndef JIT_HOT_SLOTS
#define JIT_HOT_SLOTS 8192u
#endif
#ifndef JIT_HOT_THRESHOLD
#define JIT_HOT_THRESHOLD 64
#endif
static uint16_t hotness[JIT_HOT_SLOTS];

uint32_t g_jit_blocks, g_jit_compiles, g_jit_flushes;
uint64_t g_jit_insns;

/* Implemented by the host; makes writes to the arena visible to instruction
 * fetch.  On this target that is Circle's cache maintenance. */
void jit_sync_code(void *addr, uint32_t bytes);

bool jit_init(CPUI386 *cpu)
{
	(void)cpu;
	if (arena)
		return true;
	arena = arena_store;
	memset(blocks, 0, sizeof blocks);
	memset(code_pages, 0, sizeof code_pages);
	arena_used = 0;
	return true;
}

void jit_flush(void)
{
	memset(blocks, 0, sizeof blocks);
	memset(code_pages, 0, sizeof code_pages);
	memset(hotness, 0, sizeof hotness);
	arena_used = 0;
	g_jit_blocks = 0;
	g_jit_flushes++;
}

void jit_note_write(uint32_t phys_addr)
{
	uint32_t page = phys_addr >> 12;
	if (page >= (16u << 20) / 4096u)
		return;
	if (code_pages[page >> 3] & (uint8_t)(1u << (page & 7)))
		jit_flush();
}

static void mark_code_page(uint32_t phys_addr)
{
	uint32_t page = phys_addr >> 12;
	if (page < (16u << 20) / 4096u)
		code_pages[page >> 3] |= (uint8_t)(1u << (page & 7));
}

/* ---------------------------------------------------------------------- */
/* Reading the guest's instruction bytes at translation time               */
/* ---------------------------------------------------------------------- */

/*
 * Implemented in i386.c, which owns the fetch path.
 *
 * Translation must read exactly what execution would, through the same
 * segment and page translation, or a block will be compiled from bytes the
 * guest cannot see.  Returns false if the byte is not reachable, which ends
 * the block.
 */
bool jit_peek_byte(CPUI386 *cpu, uint32_t off, uint8_t *val);
/* The physical address the block starts at, for the write guard. */
bool jit_phys_of(CPUI386 *cpu, uint32_t off, uint32_t *phys);

/* ---------------------------------------------------------------------- */
/* Prologue and epilogue                                                   */
/* ---------------------------------------------------------------------- */

static void emit_prologue(A64Buf *b)
{
	/* Save what the procedure call standard says belongs to the caller.
	 * Five pairs: the frame, then the eight registers the guest lives in
	 * plus the one holding next_ip and the one holding the CPU. */
	a64_stp_pre(b, 29, 30, A64_SP, -16);
	a64_stp_pre(b, 19, 20, A64_SP, -16);
	a64_stp_pre(b, 21, 22, A64_SP, -16);
	a64_stp_pre(b, 23, 24, A64_SP, -16);
	a64_stp_pre(b, 25, 26, A64_SP, -16);
	a64_stp_pre(b, 27, 28, A64_SP, -16);

	a64_mov_reg(b, 1, R_CPU, 0);            /* x19 = cpu, the argument */
	for (int i = 0; i < 8; i++)
		a64_ldr32(b, R_GPR(i), R_CPU, OFF_GPR(i));
	a64_ldr32(b, R_IP, R_CPU, OFF_NEXTIP);

	/* Nothing retired yet, and this many turns round any loop. */
	a64_mov_imm32(b, R_T0, 0);
	a64_str32(b, R_T0, R_CPU, OFF_ACC);
	a64_mov_imm32(b, R_T0, JIT_LOOP_BUDGET);
	a64_str32(b, R_T0, R_CPU, OFF_LIMIT);
}

static void emit_epilogue(A64Buf *b, uint32_t extra)
{
	for (int i = 0; i < 8; i++)
		a64_str32(b, R_GPR(i), R_CPU, OFF_GPR(i));
	a64_str32(b, R_IP, R_CPU, OFF_NEXTIP);
	/* Everything the invocation retired: what the loop counted, plus
	 * whatever this last partial pass managed. */
	a64_ldr32(b, 0, R_CPU, OFF_ACC);
	if (extra)
		a64_add_imm(b, 0, 0, 0, extra);

	a64_ldp_post(b, 27, 28, A64_SP, 16);
	a64_ldp_post(b, 25, 26, A64_SP, 16);
	a64_ldp_post(b, 23, 24, A64_SP, 16);
	a64_ldp_post(b, 21, 22, A64_SP, 16);
	a64_ldp_post(b, 19, 20, A64_SP, 16);
	a64_ldp_post(b, 29, 30, A64_SP, 16);
	a64_ret(b, A64_LR);
}

/* ---------------------------------------------------------------------- */
/* Translating one instruction                                             */
/* ---------------------------------------------------------------------- */


typedef struct {
	uint32_t off;           /* guest byte offset of the instruction in hand */
	uint32_t insns;         /* how many were translated before this one */
	uint32_t loop_start;    /* word index of the block's first body word */
	/*
	 * A register whose being zero is exactly the guest's ZF, or -1.
	 *
	 * This is the whole point of a translator over an interpreter here.
	 * The interpreter must write the deferred flag state and then read it
	 * back to answer the jump, because it cannot see what comes next; a
	 * block can see that the comparison is consumed by the very next
	 * instruction and test the value directly.  Only valid for the one
	 * instruction that follows.
	 */
	int zreg;
	int finished;           /* the instruction closed the block itself */
} Ctx;

/*
 * The guest's byte registers are the low and second bytes of the first four
 * words: AL to BL are 0 to 3, AH to BH are 4 to 7.  Writing one must leave
 * the rest of the register alone, which is what BFI is for - and which is
 * where the previous attempt at a translator had a real bug.
 */
static void emit_load_r8(A64Buf *b, int dst, int n)
{
	if (n < 4)
		a64_ubfx(b, 0, dst, R_GPR(n), 0, 8);
	else
		a64_ubfx(b, 0, dst, R_GPR(n - 4), 8, 8);
}

static void emit_store_r8(A64Buf *b, int n, int src)
{
	if (n < 4)
		a64_bfi(b, 0, R_GPR(n), src, 0, 8);
	else
		a64_bfi(b, 0, R_GPR(n - 4), src, 8, 8);
}

static void emit_epilogue(A64Buf *b, uint32_t extra);

/* Leave the block: advance the guest's instruction pointer past what ran,
 * and hand back how many instructions that was. */
static void emit_exit(A64Buf *b, uint32_t guest_delta, uint32_t extra)
{
	if (guest_delta)
		a64_add_imm(b, 0, R_IP, R_IP, guest_delta);
	emit_epilogue(b, extra);
}

/* The deferred flag state for a logical operation, written exactly as the
 * interpreter writes it, so that anything reading flags later - including
 * the interpreter once the block has ended - gets the same answer. */
static void emit_logic_flags(A64Buf *b, int result, int scratch)
{
	a64_sxtb(b, 0, scratch, result);
	a64_str32(b, scratch, R_CPU, OFF_CC_DST);
	a64_mov_imm32(b, scratch, CC_AND);
	a64_str32(b, scratch, R_CPU, OFF_CC_OP);
	a64_mov_imm32(b, scratch, CC_MASK_LOGIC);
	a64_str32(b, scratch, R_CPU, OFF_CC_MASK);
}

/*
 * Returns the instruction's length in guest bytes, or zero to end the block.
 *
 * Only what is listed here is translated.  Everything else returns zero and
 * the caller closes the block with next_ip pointing at it, so a gap is slow
 * rather than wrong.
 */
static int translate_one(A64Buf *b, CPUI386 *cpu, Ctx *ctx)
{
	uint8_t op, modrm, imm;
	const uint32_t off = ctx->off;
	if (!jit_peek_byte(cpu, off, &op))
		return 0;

	switch (op) {
	case 0x90:                              /* NOP */
		ctx->zreg = -1;
		return 1;

	case 0x88:                              /* MOV r/m8, r8 */
	case 0x8a:                              /* MOV r8, r/m8 */
	case 0x89:                              /* MOV r/m32, r32 */
	case 0x8b: {                            /* MOV r32, r/m32 */
		if (!jit_peek_byte(cpu, off + 1, &modrm))
			return 0;
		if ((modrm >> 6) != 3)          /* register form only, for now */
			return 0;
		const int reg = (modrm >> 3) & 7;
		const int rm = modrm & 7;
		if (op == 0x89)
			a64_mov_reg(b, 0, R_GPR(rm), R_GPR(reg));
		else if (op == 0x8b)
			a64_mov_reg(b, 0, R_GPR(reg), R_GPR(rm));
		else if (op == 0x88) {
			emit_load_r8(b, R_T0, reg);
			emit_store_r8(b, rm, R_T0);
		} else {
			emit_load_r8(b, R_T0, rm);
			emit_store_r8(b, reg, R_T0);
		}
		ctx->zreg = -1;
		return 2;
	}

	case 0x84: {                            /* TEST r/m8, r8 */
		if (!jit_peek_byte(cpu, off + 1, &modrm))
			return 0;
		if ((modrm >> 6) != 3)
			return 0;
		emit_load_r8(b, R_T0, modrm & 7);
		emit_load_r8(b, R_T1, (modrm >> 3) & 7);
		a64_and_reg(b, 0, R_T0, R_T0, R_T1);
		emit_logic_flags(b, R_T0, R_T1);
		ctx->zreg = R_T0;
		return 2;
	}

	case 0xa8: {                            /* TEST AL, imm8 */
		if (!jit_peek_byte(cpu, off + 1, &imm))
			return 0;
		emit_load_r8(b, R_T0, 0);
		a64_mov_imm32(b, R_T1, imm);
		a64_and_reg(b, 0, R_T0, R_T0, R_T1);
		emit_logic_flags(b, R_T0, R_T1);
		ctx->zreg = R_T0;
		return 2;
	}

	case 0xec: {                            /* IN AL, DX */
		/*
		 * Whether the access is allowed is two register tests in the
		 * case that matters - real mode, or a privilege the flags
		 * already permit - and a walk of the task segment's bitmap
		 * otherwise.  Only the first is translated; the rest leaves
		 * the block, and the interpreter does it properly.
		 */
		a64_ldr32(b, R_T1, R_CPU, OFF_CR0);
		a64_mov_imm32(b, R_T2, 1);
		a64_tst_reg(b, 0, R_T1, R_T2);
		const uint32_t h_ok = a64_branch_hole(b);
		a64_bcond(b, A64_EQ, 0);        /* real mode: allowed */

		a64_ldr32(b, R_T1, R_CPU, OFF_FLAGS);
		a64_mov_imm32(b, R_T2, F_VM);
		a64_tst_reg(b, 0, R_T1, R_T2);
		const uint32_t h_bail1 = a64_branch_hole(b);
		a64_bcond(b, A64_NE, 0);        /* virtual 8086: leave */

		a64_ubfx(b, 0, R_T2, R_T1, 12, 2);      /* IOPL */
		a64_ldr32(b, R_T1, R_CPU, OFF_CPL);
		a64_cmp_reg(b, 0, R_T1, R_T2);
		const uint32_t h_bail2 = a64_branch_hole(b);
		a64_bcond(b, A64_HI, 0);        /* cpl above IOPL: leave */

		a64_patch_here(b, h_ok);
		a64_ldr64(b, 0, R_CPU, OFF_CB_IO);
		a64_ubfx(b, 0, 1, R_GPR(2), 0, 16);     /* the port is DX */
		a64_ldr64(b, 2, R_CPU, OFF_CB_IN8);
		a64_blr(b, 2);
		emit_store_r8(b, 0, 0);                 /* AL takes the result */
		const uint32_t h_done = a64_branch_hole(b);
		a64_b(b, 0);

		a64_patch_here(b, h_bail1);
		a64_patch_here(b, h_bail2);
		emit_exit(b, off, ctx->insns);
		a64_patch_here(b, h_done);
		ctx->zreg = -1;                 /* the call had the scratch */
		return 1;
	}

	case 0x74:                              /* JZ rel8 */
	case 0x75: {                            /* JNZ rel8 */
		if (!jit_peek_byte(cpu, off + 1, &imm))
			return 0;
		if (ctx->zreg < 0)
			return 0;               /* nothing to test directly */
		const uint32_t next_off = off + 2;
		const int32_t target = (int32_t)next_off + (int32_t)(int8_t)imm;
		/*
		 * Only a jump back to the top of this same block, which is
		 * what a wait loop is.  Anything else would need the block to
		 * hold two futures, and block chaining is a later step.
		 */
		if (target != 0)
			return 0;

		const uint32_t h_take = a64_branch_hole(b);
		if (op == 0x74)
			a64_cbz32(b, ctx->zreg, 0);
		else
			a64_cbnz32(b, ctx->zreg, 0);

		/* Not taken: the block is done and the guest carries on. */
		emit_exit(b, next_off, ctx->insns + 1);

		a64_patch_here(b, h_take);
		/*
		 * Taken.  Count what the pass retired and spend one of the
		 * loop's iterations; when they run out the block returns with
		 * the guest exactly at its first instruction.
		 */
		a64_ldr32(b, R_T1, R_CPU, OFF_ACC);
		a64_add_imm(b, 0, R_T1, R_T1, ctx->insns + 1);
		a64_str32(b, R_T1, R_CPU, OFF_ACC);
		a64_ldr32(b, R_T1, R_CPU, OFF_LIMIT);
		a64_subs_imm(b, 0, R_T1, R_T1, 1);
		a64_str32(b, R_T1, R_CPU, OFF_LIMIT);
		a64_bcond(b, A64_NE,
			  (int32_t)ctx->loop_start - (int32_t)b->n);
		emit_exit(b, 0, 0);

		ctx->finished = 1;
		return 2;
	}

	default:
		return 0;
	}
}

/* ---------------------------------------------------------------------- */
/* Asking what coverage would be, without compiling                        */
/* ---------------------------------------------------------------------- */

static uint64_t prof_insns, prof_taken, prof_runs, prof_run_len;
static uint32_t prof_hist[32];          /* run lengths, saturating */
static uint64_t prof_block[256];        /* what ended each run */

#ifdef JIT_PROFILE
void jit_profile(CPUI386 *cpu)
{
	uint8_t op;
	if (!jit_peek_byte(cpu, 0, &op))
		return;         /* not reachable from the prefetch line; ignore */
	/* The real translator decides, so this cannot drift from it: the same
	 * function, into a buffer whose contents are thrown away. */
	static uint32_t sink[16];
	A64Buf b;
	a64_init(&b, sink, sizeof sink);

	Ctx ctx;
	ctx.off = 0; ctx.insns = 0; ctx.loop_start = 0;
	/* Say a zero test is available: the question is whether the opcode is
	 * known, not whether it happens to follow the right neighbour. */
	ctx.zreg = R_T0; ctx.finished = 0;

	prof_insns++;
	if (translate_one(&b, cpu, &ctx) > 0) {
		prof_taken++;
		prof_run_len++;
		return;
	}
	prof_block[op]++;
	prof_runs++;
	prof_hist[prof_run_len < 31 ? prof_run_len : 31]++;
	prof_run_len = 0;
}

int jit_profile_report(char *out, int len)
{
	if (!prof_insns || len <= 0)
		return 0;
	/* The four opcodes that stop the most runs, which is the list of what
	 * to implement next. */
	int n = snprintf(out, (size_t)len,
			 "%lu%% of instructions translatable, runs avg %lu.%02lu, "
			 "stopped by",
			 (unsigned long)(prof_taken * 100 / prof_insns),
			 (unsigned long)(prof_runs ? prof_taken / prof_runs : 0),
			 (unsigned long)(prof_runs ? prof_taken * 100 / prof_runs % 100 : 0));
	uint64_t tot[256];
	memcpy(tot, prof_block, sizeof tot);
	for (int k = 0; k < 4 && n < len; k++) {
		int best = -1;
		uint64_t bv = 0;
		for (int i = 0; i < 256; i++)
			if (tot[i] > bv) { bv = tot[i]; best = i; }
		if (best < 0) break;
		n += snprintf(out + n, (size_t)(len - n), " %02x:%lu%%",
			      best, (unsigned long)(bv * 100 / prof_runs));
		tot[best] = 0;
	}
	prof_insns = prof_taken = prof_runs = 0;
	memset(prof_block, 0, sizeof prof_block);
	memset(prof_hist, 0, sizeof prof_hist);
	return n;
}
#endif

/* ---------------------------------------------------------------------- */
/* Compiling and running a block                                           */
/* ---------------------------------------------------------------------- */

static JitBlock *compile(CPUI386 *cpu, uint32_t laddr)
{
	/* Room for the prologue, the body, and an epilogue at every exit an
	 * instruction may need; three of them is the worst case so far. */
	const uint32_t room = 4u * (128 + 40 * JIT_MAX_INSNS);
	if (arena_used + room > JIT_ARENA_BYTES)
		jit_flush();

	A64Buf buf;
	a64_init(&buf, arena + arena_used, room);
	emit_prologue(&buf);

	Ctx ctx;
	ctx.off = 0;
	ctx.insns = 0;
	ctx.loop_start = buf.n;
	ctx.zreg = -1;
	ctx.finished = 0;

	while (ctx.insns < JIT_MAX_INSNS) {
		const int len = translate_one(&buf, cpu, &ctx);
		if (!len)
			break;
		ctx.off += (uint32_t)len;
		ctx.insns++;
		if (ctx.finished)
			break;
	}

	/*
	 * Only a block that loops back into itself is worth having.
	 *
	 * Entering one costs a prologue that loads eight guest registers and
	 * an epilogue that stores them, about fifty instructions; a block of
	 * one or two guest instructions spends more on that than the
	 * interpreter spends running them, and measuring it that way made the
	 * whole machine a quarter slower.  A loop amortises the entry over
	 * every turn it makes, which is what the frame wait does hundreds of
	 * times.  Anything else is marked so it is not tried again.
	 */
	if (!ctx.finished || !ctx.insns || buf.overflow) {
		JitBlock *no = &blocks[laddr & (JIT_BLOCKS - 1)];
		no->laddr = laddr;
		no->valid = 2;
		no->fn = NULL;
		return NULL;
	}

	/* An instruction that closed the block wrote its own exits; otherwise
	 * the guest carries on from just past what was translated. */
	if (!ctx.finished)
		emit_exit(&buf, ctx.off, ctx.insns);
	if (buf.overflow)
		return NULL;

	uint32_t phys;
	if (!jit_phys_of(cpu, 0, &phys))
		return NULL;

	void *code = arena + arena_used;
	const uint32_t bytes = buf.n * 4u;
	arena_used += (bytes + 15u) & ~15u;
	jit_sync_code(code, bytes);

	JitBlock *blk = &blocks[laddr & (JIT_BLOCKS - 1)];
	blk->laddr = laddr;
	blk->page = phys >> 12;
	blk->insns = ctx.insns;
	blk->fn = (uint32_t (*)(CPUI386 *))code;
	if (!blk->valid)
		g_jit_blocks++;
	blk->valid = 1;
	mark_code_page(phys);
	g_jit_compiles++;
	return blk;
}

int jit_run(CPUI386 *cpu)
{
	if (!arena)
		return 0;
	/* Thirty-two bit code only for now.  A sixteen-bit guest needs the
	 * operand size threading through every case, and there is no sense
	 * doing that before the flags are in. */
	if (cpu->code16)
		return 0;

	const uint32_t laddr = (uint32_t)(cpu->seg[SEG_CS].base + cpu->next_ip);
	JitBlock *blk = &blocks[laddr & (JIT_BLOCKS - 1)];
	if (blk->laddr == laddr && blk->valid == 2)
		return 0;               /* known not to be worth translating */
	if (blk->valid != 1 || blk->laddr != laddr) {
		/*
		 * Wait until the address has been seen often before trying to
		 * translate it.
		 *
		 * The guest runs over far more addresses than this table has
		 * slots, so translating on first sight means compiling
		 * constantly and throwing the result away on the next
		 * collision - which measured a quarter slower than not having
		 * a translator at all.  A count first means the work is only
		 * done for code that comes round again, which is the only
		 * code a translation can pay for.
		 */
		uint16_t *hot = &hotness[laddr & (JIT_HOT_SLOTS - 1)];
		if (*hot < JIT_HOT_THRESHOLD) {
			(*hot)++;
			return 0;
		}
		*hot = 0;
		blk = compile(cpu, laddr);
		if (!blk)
			return 0;
	}
	const int n = (int)blk->fn(cpu);
	g_jit_insns += (uint64_t)n;
	return n;
}

/* ---------------------------------------------------------------------- */
/* The startup check                                                       */
/* ---------------------------------------------------------------------- */

/*
 * Build blocks by hand, run them, and see whether the registers moved the
 * way they should.  This catches a wrong register mapping or a wrong
 * prologue, which the encoding test above cannot: that one proves each
 * instruction is what it claims to be, not that the right ones were chosen.
 */
int jit_selftest(char *msg, int len)
{
	if (!arena) {
		if (len > 0) snprintf(msg, (size_t)len, "no arena");
		return 1;
	}

	CPUI386 st;
	int bad = 0;
	A64Buf buf;

	/* MOV EBX, EAX then MOV ECX, EBX, by hand. */
	a64_init(&buf, arena, JIT_ARENA_BYTES);
	emit_prologue(&buf);
	a64_mov_reg(&buf, 0, R_GPR(3), R_GPR(0));
	a64_mov_reg(&buf, 0, R_GPR(1), R_GPR(3));
	a64_add_imm(&buf, 0, R_IP, R_IP, 4);
	emit_epilogue(&buf, 2);
	jit_sync_code(arena, buf.n * 4u);

	memset(&st, 0, sizeof st);
	st.gprx[0].r32 = 0x11223344u;
	st.gprx[1].r32 = 0x55555555u;
	st.next_ip = 0x1000;
	uint32_t got = ((uint32_t (*)(CPUI386 *))arena)(&st);
	if (got != 2 || st.gprx[3].r32 != 0x11223344u ||
	    st.gprx[1].r32 != 0x11223344u || st.next_ip != 0x1004) {
		if (len > 0)
			snprintf(msg, (size_t)len,
				 "move: n=%lu ebx=%08lx ecx=%08lx ip=%08lx",
				 (unsigned long)got, (unsigned long)st.gprx[3].r32,
				 (unsigned long)st.gprx[1].r32,
				 (unsigned long)st.next_ip);
		return ++bad;
	}

	/* The byte registers, which is where a partial write goes wrong: AH
	 * must move without touching AL, and the rest of EAX must survive. */
	a64_init(&buf, arena, JIT_ARENA_BYTES);
	emit_prologue(&buf);
	emit_load_r8(&buf, R_T0, 0);            /* AL */
	emit_store_r8(&buf, 4, R_T0);           /* -> AH */
	emit_load_r8(&buf, R_T0, 7);            /* BH */
	emit_store_r8(&buf, 3, R_T0);           /* -> BL */
	emit_epilogue(&buf, 2);
	jit_sync_code(arena, buf.n * 4u);

	memset(&st, 0, sizeof st);
	st.gprx[0].r32 = 0xdeadbe5au;           /* AL = 5a, AH = be */
	st.gprx[3].r32 = 0xcafe9900u;           /* BL = 00, BH = 99 */
	((uint32_t (*)(CPUI386 *))arena)(&st);
	if (st.gprx[0].r32 != 0xdead5a5au || st.gprx[3].r32 != 0xcafe9999u) {
		if (len > 0)
			snprintf(msg, (size_t)len, "byte: eax=%08lx ebx=%08lx",
				 (unsigned long)st.gprx[0].r32,
				 (unsigned long)st.gprx[3].r32);
		return ++bad;
	}

	/*
	 * The deferred flag state a logical operation leaves behind, which the
	 * interpreter reads once the block ends.  Getting this wrong would not
	 * show up as a crash but as a jump going the wrong way, much later.
	 */
	a64_init(&buf, arena, JIT_ARENA_BYTES);
	emit_prologue(&buf);
	emit_load_r8(&buf, R_T0, 0);            /* AL */
	a64_mov_imm32(&buf, R_T1, 0x0f);
	a64_and_reg(&buf, 0, R_T0, R_T0, R_T1);
	emit_logic_flags(&buf, R_T0, R_T1);
	emit_epilogue(&buf, 1);
	jit_sync_code(arena, buf.n * 4u);

	memset(&st, 0, sizeof st);
	st.gprx[0].r32 = 0x1234565au;           /* AL = 5a, so 5a & 0f = 0a */
	((uint32_t (*)(CPUI386 *))arena)(&st);
	if (st.cc.dst != 0x0a || st.cc.op != CC_AND || st.cc.mask != 0x8c5u) {
		if (len > 0)
			snprintf(msg, (size_t)len, "flags: dst=%lx op=%d mask=%lx",
				 (unsigned long)st.cc.dst, st.cc.op,
				 (unsigned long)st.cc.mask);
		return ++bad;
	}

	/* And the sign extension, which is what the interpreter stores: a
	 * result with bit seven set has to read back negative. */
	memset(&st, 0, sizeof st);
	st.gprx[0].r32 = 0x000000ffu;
	a64_init(&buf, arena, JIT_ARENA_BYTES);
	emit_prologue(&buf);
	emit_load_r8(&buf, R_T0, 0);
	a64_mov_imm32(&buf, R_T1, 0xf0);
	a64_and_reg(&buf, 0, R_T0, R_T0, R_T1);
	emit_logic_flags(&buf, R_T0, R_T1);
	emit_epilogue(&buf, 1);
	jit_sync_code(arena, buf.n * 4u);
	((uint32_t (*)(CPUI386 *))arena)(&st);
	if (st.cc.dst != 0xfffffff0u) {
		if (len > 0)
			snprintf(msg, (size_t)len, "sign: dst=%lx",
				 (unsigned long)st.cc.dst);
		return ++bad;
	}

	if (len > 0)
		snprintf(msg, (size_t)len, "moves, partial writes and flags correct");
	return 0;
}

#endif /* TINY386_JIT */
