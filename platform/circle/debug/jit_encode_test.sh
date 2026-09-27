#!/bin/sh
# Check the AArch64 emitter against the assembler.
#
# jit_encode_test.c emits each instruction twice: once through
# src/jit/arm64_emit.h, and once as a line of assembly which
# aarch64-linux-gnu-as turns into the reference word.  Any difference is a
# wrong encoding, which is the kind of mistake that does not announce itself.
#
# Runs entirely on the host - no board, no emulator - so it is the fast loop
# for the layer where mistakes are hardest to find later.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=${TMPDIR:-/tmp}/jit-encode-$$
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

cc -O1 -Wall -Wextra -o "$work/emit" "$root/platform/circle/debug/jit_encode_test.c"

"$work/emit" --asm > "$work/ref.s"
aarch64-linux-gnu-as -o "$work/ref.o" "$work/ref.s"
# One word per line, in order, as hexadecimal.  objdump prints the address,
# the word and the mnemonic; the word is the second field.
aarch64-linux-gnu-objdump -d --insn-width=4 "$work/ref.o" \
	| sed -n 's/^[ \t]*[0-9a-f]\+:[ \t]*\([0-9a-f]\{8\}\).*/\1/p' > "$work/ref.txt"

"$work/emit" < "$work/ref.txt"
