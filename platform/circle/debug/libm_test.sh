#!/bin/sh
# Compare pc/libm.c against the host's libm.  Run from the repository root, or
# under docker:
#   docker run --rm -v <repo>:/work -w /work gcc:13 sh platform/circle/debug/libm_test.sh
set -e
O=${TMPDIR:-/tmp}
# The routines under test are renamed so both libraries can be linked at once.
R="-Dexp2=mine_exp2 -Dlog2=mine_log2 -Dsin=mine_sin -Dcos=mine_cos -Dtan=mine_tan \
-Datan=mine_atan -Datan2=mine_atan2 -Dpow=mine_pow -Dfrexp=mine_frexp \
-Dsqrt=mine_sqrt -Dfabs=mine_fabs -Dceil=mine_ceil -Dfloor=mine_floor \
-Dtrunc=mine_trunc -Dnearbyint=mine_nearbyint -Dcopysign=mine_copysign"
gcc -O2 -std=c11 $R -c platform/circle/pc/libm.c -o "$O/libm_mine.o"
gcc -O2 -std=c11 -c platform/circle/debug/libm_test.c -o "$O/libm_test.o"
gcc "$O/libm_test.o" "$O/libm_mine.o" -lm -o "$O/libm_test"
"$O/libm_test"
