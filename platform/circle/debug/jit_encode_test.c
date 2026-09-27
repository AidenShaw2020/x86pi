/*
 * Check src/jit/arm64_emit.h against the assembler.
 *
 * A wrong bit in an instruction encoding is not a crash: it is a valid
 * instruction that does something else, and the guest then misbehaves much
 * later somewhere that points nowhere near the mistake.  So this builds each
 * instruction twice - once through the header, once as a line of assembly
 * for aarch64-linux-gnu-as - and compares the words.
 *
 * It prints the assembly to standard output when given "--asm", which is how
 * jit_encode_test.sh gets the reference; with no argument it reads the
 * assembled words back and compares.  Run it with debug/jit_encode_test.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../../src/jit/arm64_emit.h"

/*
 * The cases.
 *
 * Each names the assembly it should produce and the calls that should
 * produce it.  Adding an instruction to the emitter means adding a line
 * here in the same change; an emitter function with no case is the one that
 * will be wrong.
 */
#define CASES(X) \
	X("movz w5, #0x1234",              a64_movz(b, 0, 5, 0x1234, 0)) \
	X("movz x7, #0xbeef, lsl #16",     a64_movz(b, 1, 7, 0xbeef, 1)) \
	X("movk w3, #0xcafe, lsl #16",     a64_movk(b, 0, 3, 0xcafe, 1)) \
	X("movn w9, #0x000f",              a64_movn(b, 0, 9, 0x000f, 0)) \
	X("movz w1, #0x0042",              a64_mov_imm32(b, 1, 0x42)) \
	X("movz w2, #0x8000, lsl #16",     a64_mov_imm32(b, 2, 0x80000000u)) \
	X("movn w4, #0x0000",              a64_mov_imm32(b, 4, 0xffffffffu)) \
	X("add w0, w1, #0x10",             a64_add_imm(b, 0, 0, 1, 0x10)) \
	X("add x20, x21, #0x8",            a64_add_imm(b, 1, 20, 21, 8)) \
	X("sub w6, w7, #0x1",              a64_sub_imm(b, 0, 6, 7, 1)) \
	X("adds w8, w9, #0x2",             a64_adds_imm(b, 0, 8, 9, 2)) \
	X("subs w10, w11, #0x3",           a64_subs_imm(b, 0, 10, 11, 3)) \
	X("cmp w12, #0x7f",                a64_cmp_imm(b, 0, 12, 0x7f)) \
	X("add w13, w14, w15",             a64_add_reg(b, 0, 13, 14, 15)) \
	X("add x16, x17, x18",             a64_add_reg(b, 1, 16, 17, 18)) \
	X("sub w19, w20, w21",             a64_sub_reg(b, 0, 19, 20, 21)) \
	X("adds w22, w23, w24",            a64_adds_reg(b, 0, 22, 23, 24)) \
	X("subs w25, w26, w27",            a64_subs_reg(b, 0, 25, 26, 27)) \
	X("cmp w28, w29",                  a64_cmp_reg(b, 0, 28, 29)) \
	X("and w0, w1, w2",                a64_and_reg(b, 0, 0, 1, 2)) \
	X("ands w3, w4, w5",               a64_ands_reg(b, 0, 3, 4, 5)) \
	X("tst w6, w7",                    a64_tst_reg(b, 0, 6, 7)) \
	X("orr w8, w9, w10",               a64_orr_reg(b, 0, 8, 9, 10)) \
	X("eor w11, w12, w13",             a64_eor_reg(b, 0, 11, 12, 13)) \
	X("mov w14, w15",                  a64_mov_reg(b, 0, 14, 15)) \
	X("mov x19, x0",                   a64_mov_reg(b, 1, 19, 0)) \
	X("bfi w20, w21, #8, #8",          a64_bfi(b, 0, 20, 21, 8, 8)) \
	X("bfi w22, w23, #0, #16",         a64_bfi(b, 0, 22, 23, 0, 16)) \
	X("ubfx w24, w25, #8, #8",         a64_ubfx(b, 0, 24, 25, 8, 8)) \
	X("sxtb w26, w27",         a64_sxtb(b, 0, 26, 27)) \
	X("ldr w0, [x19, #16]",            a64_ldr32(b, 0, 19, 16)) \
	X("str w1, [x19, #4092]",          a64_str32(b, 1, 19, 4092)) \
	X("ldr x2, [x19, #24]",            a64_ldr64(b, 2, 19, 24)) \
	X("str x3, [x20, #8]",             a64_str64(b, 3, 20, 8)) \
	X("ldrb w4, [x19, #7]",            a64_ldrb(b, 4, 19, 7)) \
	X("strb w5, [x19, #255]",          a64_strb(b, 5, 19, 255)) \
	X("ldrh w6, [x19, #6]",            a64_ldrh(b, 6, 19, 6)) \
	X("strh w7, [x19, #510]",          a64_strh(b, 7, 19, 510)) \
	X("stp x29, x30, [sp, #-16]!",     a64_stp_pre(b, 29, 30, A64_SP, -16)) \
	X("ldp x29, x30, [sp], #16",       a64_ldp_post(b, 29, 30, A64_SP, 16)) \
	X("stp x19, x20, [sp, #-32]!",     a64_stp_pre(b, 19, 20, A64_SP, -32)) \
	X("b . + 16",                      a64_b(b, 4)) \
	X("b . - 8",                       a64_b(b, -2)) \
	X("bl . + 4",                      a64_bl(b, 1)) \
	X("b.eq . + 12",                   a64_bcond(b, A64_EQ, 3)) \
	X("b.ne . - 4",                    a64_bcond(b, A64_NE, -1)) \
	X("b.hi . + 8",                    a64_bcond(b, A64_HI, 2)) \
	X("br x8",                         a64_br(b, 8)) \
	X("blr x9",                        a64_blr(b, 9)) \
	X("ret x30",                       a64_ret(b, A64_LR)) \
	X("cbz w10, . + 20",               a64_cbz32(b, 10, 5)) \
	X("cbnz w11, . - 12",              a64_cbnz32(b, 11, -3))

static const char *const names[] = {
#define X(text, call) text,
	CASES(X)
#undef X
};
#define NCASES ((int)(sizeof names / sizeof *names))

static void build(A64Buf *b)
{
#define X(text, call) call;
	CASES(X)
#undef X
}

int main(int argc, char **argv)
{
	uint32_t mem[NCASES * 4];
	A64Buf buf, *b = &buf;
	a64_init(b, mem, sizeof mem);
	build(b);

	if (b->overflow) {
		fprintf(stderr, "emit buffer overflowed\n");
		return 2;
	}
	if ((int)b->n != NCASES) {
		fprintf(stderr, "each case must emit exactly one word: "
			"%d cases produced %u words\n", NCASES, b->n);
		return 2;
	}

	if (argc > 1 && !strcmp(argv[1], "--asm")) {
		printf(".text\n");
		for (int i = 0; i < NCASES; i++)
			printf("\t%s\n", names[i]);
		return 0;
	}

	/* Compare against the words the assembler produced, one per line as
	 * hexadecimal, on standard input. */
	int bad = 0;
	for (int i = 0; i < NCASES; i++) {
		char line[128];
		if (!fgets(line, sizeof line, stdin)) {
			fprintf(stderr, "reference ended after %d of %d\n", i, NCASES);
			return 2;
		}
		unsigned long want = strtoul(line, NULL, 16);
		if ((unsigned long)b->buf[i] != want) {
			printf("MISMATCH  %-34s emitted %08lx want %08lx\n",
			       names[i], (unsigned long)b->buf[i], want);
			bad++;
		}
	}
	printf("%d instructions checked, %d wrong\n", NCASES, bad);
	return bad ? 1 : 0;
}
