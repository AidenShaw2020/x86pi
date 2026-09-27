#pragma once
/*
 * Emitting AArch64, one instruction at a time.
 *
 * This is the bottom layer of the translator and the one where a mistake is
 * silent: a wrong bit in an encoding produces a valid instruction that does
 * something else, and the guest then misbehaves minutes later in a way that
 * points nowhere near here.  So every function below is checked against the
 * assembler's own output by debug/jit_encode_test.sh, which builds the same
 * instructions twice - once through this header, once through
 * aarch64-linux-gnu-as - and compares the words.
 *
 * Only what the translator actually emits is here.  Adding an instruction
 * means adding its case to that test in the same change.
 */

#include <stdint.h>

/* Register numbers are the architectural ones: 0-30, plus 31 which reads as
 * the zero register in most encodings and as the stack pointer in a few. */
#define A64_ZR 31
#define A64_SP 31
#define A64_LR 30

typedef struct {
	uint32_t *buf;      /* where the words go */
	uint32_t cap;       /* how many words fit */
	uint32_t n;         /* how many are written */
	int overflow;       /* set once a write did not fit; never unset */
} A64Buf;

static inline void a64_init(A64Buf *b, void *mem, uint32_t bytes)
{
	b->buf = (uint32_t *)mem;
	b->cap = bytes / 4;
	b->n = 0;
	b->overflow = 0;
}

/*
 * One word.
 *
 * A full buffer is not an error to report upwards from every call site; the
 * translator checks `overflow` once, when it has finished a block, and throws
 * the block away.  Writing nothing after the first failure keeps that safe.
 */
static inline void a64_word(A64Buf *b, uint32_t w)
{
	if (b->n < b->cap)
		b->buf[b->n++] = w;
	else
		b->overflow = 1;
}

/* Condition codes, in the architecture's numbering. */
enum {
	A64_EQ = 0x0, A64_NE = 0x1, A64_CS = 0x2, A64_CC = 0x3,
	A64_MI = 0x4, A64_PL = 0x5, A64_VS = 0x6, A64_VC = 0x7,
	A64_HI = 0x8, A64_LS = 0x9, A64_GE = 0xa, A64_LT = 0xb,
	A64_GT = 0xc, A64_LE = 0xd, A64_AL = 0xe,
};

/* ---------------------------------------------------------------------- */
/* Moving constants                                                        */
/* ---------------------------------------------------------------------- */

/* MOVZ Rd, #imm16, LSL #(16*shift) - clears the rest of the register. */
static inline void a64_movz(A64Buf *b, int sf, int rd, uint16_t imm, int shift)
{
	a64_word(b, (sf ? 0xd2800000u : 0x52800000u) |
		    ((uint32_t)shift << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}

/* MOVK Rd, #imm16, LSL #(16*shift) - leaves the rest of the register. */
static inline void a64_movk(A64Buf *b, int sf, int rd, uint16_t imm, int shift)
{
	a64_word(b, (sf ? 0xf2800000u : 0x72800000u) |
		    ((uint32_t)shift << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}

/* MOVN Rd, #imm16, LSL #(16*shift) - the bitwise complement. */
static inline void a64_movn(A64Buf *b, int sf, int rd, uint16_t imm, int shift)
{
	a64_word(b, (sf ? 0x92800000u : 0x12800000u) |
		    ((uint32_t)shift << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}

/* Any 32-bit constant, in as few words as it takes. */
static inline void a64_mov_imm32(A64Buf *b, int rd, uint32_t v)
{
	if ((v >> 16) == 0) {
		a64_movz(b, 0, rd, (uint16_t)v, 0);
	} else if ((v & 0xffffu) == 0) {
		a64_movz(b, 0, rd, (uint16_t)(v >> 16), 1);
	} else if ((~v >> 16) == 0) {
		a64_movn(b, 0, rd, (uint16_t)~v, 0);
	} else {
		a64_movz(b, 0, rd, (uint16_t)v, 0);
		a64_movk(b, 0, rd, (uint16_t)(v >> 16), 1);
	}
}

/* Any 64-bit constant.  Used for helper addresses, so it does not try to be
 * clever: four words at worst, and the common case of a small value or one
 * with zero halves is shorter. */
static inline void a64_mov_imm64(A64Buf *b, int rd, uint64_t v)
{
	int started = 0;
	for (int i = 0; i < 4; i++) {
		uint16_t part = (uint16_t)(v >> (16 * i));
		if (part == 0 && (started || i != 3))
			continue;
		if (!started) {
			a64_movz(b, 1, rd, part, i);
			started = 1;
		} else {
			a64_movk(b, 1, rd, part, i);
		}
	}
	if (!started)
		a64_movz(b, 1, rd, 0, 0);
}

/* ---------------------------------------------------------------------- */
/* Arithmetic and logic                                                    */
/* ---------------------------------------------------------------------- */

/* op Rd, Rn, #imm12 (optionally shifted left by 12). */
static inline void a64_addsub_imm(A64Buf *b, uint32_t base, int sf, int rd,
				  int rn, uint32_t imm12, int shift12)
{
	a64_word(b, base | (sf ? 0x80000000u : 0u) |
		    ((uint32_t)shift12 << 22) | ((imm12 & 0xfffu) << 10) |
		    ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void a64_add_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm)
	{ a64_addsub_imm(b, 0x11000000u, sf, rd, rn, imm, 0); }
static inline void a64_sub_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm)
	{ a64_addsub_imm(b, 0x51000000u, sf, rd, rn, imm, 0); }
static inline void a64_adds_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm)
	{ a64_addsub_imm(b, 0x31000000u, sf, rd, rn, imm, 0); }
static inline void a64_subs_imm(A64Buf *b, int sf, int rd, int rn, uint32_t imm)
	{ a64_addsub_imm(b, 0x71000000u, sf, rd, rn, imm, 0); }
/* CMP Rn, #imm is SUBS with the result discarded. */
static inline void a64_cmp_imm(A64Buf *b, int sf, int rn, uint32_t imm)
	{ a64_subs_imm(b, sf, A64_ZR, rn, imm); }

/* op Rd, Rn, Rm, LSL #amount. */
static inline void a64_shifted_reg(A64Buf *b, uint32_t base, int sf, int rd,
				   int rn, int rm, int shift_type, int amount)
{
	a64_word(b, base | (sf ? 0x80000000u : 0u) |
		    ((uint32_t)shift_type << 22) | ((uint32_t)rm << 16) |
		    ((uint32_t)amount << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void a64_add_reg(A64Buf *b, int sf, int rd, int rn, int rm)
	{ a64_shifted_reg(b, 0x0b000000u, sf, rd, rn, rm, 0, 0); }
static inline void a64_sub_reg(A64Buf *b, int sf, int rd, int rn, int rm)
	{ a64_shifted_reg(b, 0x4b000000u, sf, rd, rn, rm, 0, 0); }
static inline void a64_adds_reg(A64Buf *b, int sf, int rd, int rn, int rm)
	{ a64_shifted_reg(b, 0x2b000000u, sf, rd, rn, rm, 0, 0); }
static inline void a64_subs_reg(A64Buf *b, int sf, int rd, int rn, int rm)
	{ a64_shifted_reg(b, 0x6b000000u, sf, rd, rn, rm, 0, 0); }
static inline void a64_cmp_reg(A64Buf *b, int sf, int rn, int rm)
	{ a64_subs_reg(b, sf, A64_ZR, rn, rm); }

static inline void a64_and_reg(A64Buf *b, int sf, int rd, int rn, int rm)
	{ a64_shifted_reg(b, 0x0a000000u, sf, rd, rn, rm, 0, 0); }
static inline void a64_ands_reg(A64Buf *b, int sf, int rd, int rn, int rm)
	{ a64_shifted_reg(b, 0x6a000000u, sf, rd, rn, rm, 0, 0); }
/* TST Rn, Rm is ANDS with the result discarded - the whole point of the
 * translator for a guest TEST followed by a conditional jump. */
static inline void a64_tst_reg(A64Buf *b, int sf, int rn, int rm)
	{ a64_ands_reg(b, sf, A64_ZR, rn, rm); }
static inline void a64_orr_reg(A64Buf *b, int sf, int rd, int rn, int rm)
	{ a64_shifted_reg(b, 0x2a000000u, sf, rd, rn, rm, 0, 0); }
static inline void a64_eor_reg(A64Buf *b, int sf, int rd, int rn, int rm)
	{ a64_shifted_reg(b, 0x4a000000u, sf, rd, rn, rm, 0, 0); }
/* MOV Rd, Rm is ORR Rd, ZR, Rm. */
static inline void a64_mov_reg(A64Buf *b, int sf, int rd, int rm)
	{ a64_orr_reg(b, sf, rd, A64_ZR, rm); }

/* ---------------------------------------------------------------------- */
/* Bitfield moves                                                          */
/* ---------------------------------------------------------------------- */

/*
 * BFI Rd, Rn, #lsb, #width - insert the low `width` bits of Rn at `lsb`.
 *
 * This is how a guest byte or word register is written without disturbing
 * the rest of the 32-bit register, which x86 requires and which cost the
 * previous attempt at this a real bug.
 */
static inline void a64_bfi(A64Buf *b, int sf, int rd, int rn, int lsb, int width)
{
	int regsize = sf ? 64 : 32;
	uint32_t immr = (uint32_t)((regsize - lsb) & (regsize - 1));
	uint32_t imms = (uint32_t)(width - 1);
	a64_word(b, (sf ? 0xb3400000u : 0x33000000u) |
		    (immr << 16) | (imms << 10) |
		    ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* SXTB Rd, Rn - the low byte, sign extended, which is the form the
 * interpreter keeps a byte result in. */
static inline void a64_sxtb(A64Buf *b, int sf, int rd, int rn)
{
	a64_word(b, (sf ? 0x93400000u : 0x13000000u) |
		    (7u << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* UBFX Rd, Rn, #lsb, #width - extract, zero extended. */
static inline void a64_ubfx(A64Buf *b, int sf, int rd, int rn, int lsb, int width)
{
	a64_word(b, (sf ? 0xd3400000u : 0x53000000u) |
		    ((uint32_t)lsb << 16) | ((uint32_t)(lsb + width - 1) << 10) |
		    ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* ---------------------------------------------------------------------- */
/* Memory                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * Load and store with an unsigned byte offset, which is what reaching into
 * the guest's state structure needs.  The offset is scaled by the access
 * size, so it must be a multiple of it; the caller knows its field offsets
 * and they always are.
 */
static inline void a64_ldst_imm(A64Buf *b, uint32_t base, int rt, int rn,
				uint32_t off, int scale)
{
	a64_word(b, base | (((off >> scale) & 0xfffu) << 10) |
		    ((uint32_t)rn << 5) | (uint32_t)rt);
}

static inline void a64_ldr32(A64Buf *b, int rt, int rn, uint32_t off)
	{ a64_ldst_imm(b, 0xb9400000u, rt, rn, off, 2); }
static inline void a64_str32(A64Buf *b, int rt, int rn, uint32_t off)
	{ a64_ldst_imm(b, 0xb9000000u, rt, rn, off, 2); }
static inline void a64_ldr64(A64Buf *b, int rt, int rn, uint32_t off)
	{ a64_ldst_imm(b, 0xf9400000u, rt, rn, off, 3); }
static inline void a64_str64(A64Buf *b, int rt, int rn, uint32_t off)
	{ a64_ldst_imm(b, 0xf9000000u, rt, rn, off, 3); }
static inline void a64_ldrb(A64Buf *b, int rt, int rn, uint32_t off)
	{ a64_ldst_imm(b, 0x39400000u, rt, rn, off, 0); }
static inline void a64_strb(A64Buf *b, int rt, int rn, uint32_t off)
	{ a64_ldst_imm(b, 0x39000000u, rt, rn, off, 0); }
static inline void a64_ldrh(A64Buf *b, int rt, int rn, uint32_t off)
	{ a64_ldst_imm(b, 0x79400000u, rt, rn, off, 1); }
static inline void a64_strh(A64Buf *b, int rt, int rn, uint32_t off)
	{ a64_ldst_imm(b, 0x79000000u, rt, rn, off, 1); }

/* STP Rt, Rt2, [Rn, #off]! and LDP Rt, Rt2, [Rn], #off - the two halves of
 * a frame.  The offset is in bytes and must be a multiple of eight. */
static inline void a64_stp_pre(A64Buf *b, int rt, int rt2, int rn, int off)
{
	uint32_t imm7 = (uint32_t)((off / 8) & 0x7f);
	a64_word(b, 0xa9800000u | (imm7 << 15) | ((uint32_t)rt2 << 10) |
		    ((uint32_t)rn << 5) | (uint32_t)rt);
}

static inline void a64_ldp_post(A64Buf *b, int rt, int rt2, int rn, int off)
{
	uint32_t imm7 = (uint32_t)((off / 8) & 0x7f);
	a64_word(b, 0xa8c00000u | (imm7 << 15) | ((uint32_t)rt2 << 10) |
		    ((uint32_t)rn << 5) | (uint32_t)rt);
}

/* ---------------------------------------------------------------------- */
/* Control flow                                                            */
/* ---------------------------------------------------------------------- */

/* B to a word offset from this instruction. */
static inline void a64_b(A64Buf *b, int32_t words)
	{ a64_word(b, 0x14000000u | ((uint32_t)words & 0x03ffffffu)); }
/* BL to a word offset from this instruction. */
static inline void a64_bl(A64Buf *b, int32_t words)
	{ a64_word(b, 0x94000000u | ((uint32_t)words & 0x03ffffffu)); }
/* B.cond to a word offset from this instruction. */
static inline void a64_bcond(A64Buf *b, int cond, int32_t words)
	{ a64_word(b, 0x54000000u | (((uint32_t)words & 0x7ffffu) << 5) |
		      (uint32_t)cond); }
static inline void a64_br(A64Buf *b, int rn)
	{ a64_word(b, 0xd61f0000u | ((uint32_t)rn << 5)); }
static inline void a64_blr(A64Buf *b, int rn)
	{ a64_word(b, 0xd63f0000u | ((uint32_t)rn << 5)); }
static inline void a64_ret(A64Buf *b, int rn)
	{ a64_word(b, 0xd65f0000u | ((uint32_t)rn << 5)); }
static inline void a64_cbz32(A64Buf *b, int rt, int32_t words)
	{ a64_word(b, 0x34000000u | (((uint32_t)words & 0x7ffffu) << 5) |
		      (uint32_t)rt); }
static inline void a64_cbnz32(A64Buf *b, int rt, int32_t words)
	{ a64_word(b, 0x35000000u | (((uint32_t)words & 0x7ffffu) << 5) |
		      (uint32_t)rt); }

/*
 * A forward branch whose target is not known yet.
 *
 * The emitter writes the instruction with a zero offset and remembers where
 * it is; a64_patch_here() fills the offset in once the target is reached.
 */
static inline uint32_t a64_branch_hole(A64Buf *b) { return b->n; }

static inline void a64_patch_here(A64Buf *b, uint32_t at)
{
	if (at >= b->n || b->overflow)
		return;
	int32_t words = (int32_t)(b->n - at);
	uint32_t w = b->buf[at];
	if ((w & 0xfc000000u) == 0x14000000u || (w & 0xfc000000u) == 0x94000000u) {
		b->buf[at] = (w & 0xfc000000u) | ((uint32_t)words & 0x03ffffffu);
	} else {
		/* B.cond, CBZ and CBNZ all carry the offset in the same place. */
		b->buf[at] = (w & ~(0x7ffffu << 5)) |
			     (((uint32_t)words & 0x7ffffu) << 5);
	}
}
