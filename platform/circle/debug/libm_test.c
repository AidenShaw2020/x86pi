/*
 * Check platform/circle/pc/libm.c against the host's own maths library.
 *
 * The kernel cannot link glibc's libm, so those routines were written out
 * again; this is what says whether they are right.  It reports the worst
 * error seen over each function's interesting range, measured in units in the
 * last place, so "close enough" is a number rather than an opinion.
 *
 * Build and run with debug/libm_test.sh.
 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The routines under test, named apart so both can be linked. */
double mine_exp2(double);
double mine_log2(double);
double mine_sin(double);
double mine_cos(double);
double mine_tan(double);
double mine_atan2(double, double);
double mine_pow(double, double);
double mine_frexp(double, int *);

static double ulps(double got, double want)
{
    if (got == want) return 0.0;
    if (!isfinite(got) || !isfinite(want)) return got == want ? 0.0 : 1e30;
    int e;
    frexp(want, &e);
    const double ulp = ldexp(1.0, e - 53);
    return fabs(got - want) / ulp;
}

static uint64_t seed = 88172645463325252ull;
static double rnd(double lo, double hi)
{
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    return lo + (hi - lo) * ((double)(seed >> 11) / 9007199254740992.0);
}

#define CHECK1(name, mine, ref, lo, hi, n)                                   \
    do {                                                                     \
        double worst = 0, at = 0;                                            \
        for (long i = 0; i < (n); i++) {                                      \
            const double x = rnd(lo, hi);                                     \
            const double u = ulps(mine(x), ref(x));                           \
            if (u > worst) { worst = u; at = x; }                             \
        }                                                                     \
        printf("%-10s %-28s worst %8.2f ulp  at % .17g\n",                    \
               name, "[" #lo ", " #hi "]", worst, at);                        \
        if (worst > 8.0) bad++;                                               \
    } while (0)

int main(void)
{
    int bad = 0;

    CHECK1("exp2", mine_exp2, exp2, -1.0, 1.0, 200000);
    CHECK1("exp2", mine_exp2, exp2, -700.0, 700.0, 200000);
    CHECK1("log2", mine_log2, log2, 1e-300, 1.0, 200000);
    CHECK1("log2", mine_log2, log2, 1.0, 1e300, 200000);
    CHECK1("log2", mine_log2, log2, 0.5, 2.0, 200000);
    CHECK1("sin", mine_sin, sin, -3.14159265358979, 3.14159265358979, 200000);
    CHECK1("sin", mine_sin, sin, -1000.0, 1000.0, 200000);
    CHECK1("sin", mine_sin, sin, -1e9, 1e9, 200000);
    CHECK1("cos", mine_cos, cos, -3.14159265358979, 3.14159265358979, 200000);
    CHECK1("cos", mine_cos, cos, -1e9, 1e9, 200000);
    CHECK1("tan", mine_tan, tan, -0.7, 0.7, 200000);
    CHECK1("tan", mine_tan, tan, -1000.0, 1000.0, 200000);

    /* atan2 over the whole circle, both signs of both arguments. */
    {
        double worst = 0, ax = 0, ay = 0;
        for (long i = 0; i < 400000; i++) {
            const double y = rnd(-1e6, 1e6), x = rnd(-1e6, 1e6);
            const double u = ulps(mine_atan2(y, x), atan2(y, x));
            if (u > worst) { worst = u; ay = y; ax = x; }
        }
        printf("%-10s %-28s worst %8.2f ulp  at % .17g, % .17g\n",
               "atan2", "[-1e6, 1e6]^2", worst, ay, ax);
        if (worst > 8.0) bad++;
    }

    /* pow is only ever called with a base of two by the emulation, so that is
     * where it has to be right: F2XM1 asks for a fraction, FSCALE a whole
     * number. */
    {
        double worst = 0, at = 0;
        for (long i = 0; i < 200000; i++) {
            const double y = rnd(-1.0, 1.0);
            const double u = ulps(mine_pow(2.0, y), pow(2.0, y));
            if (u > worst) { worst = u; at = y; }
        }
        printf("%-10s %-28s worst %8.2f ulp  at % .17g\n",
               "pow(2,y)", "[-1, 1]", worst, at);
        if (worst > 8.0) bad++;
    }
    for (int n = -1000; n <= 1000; n++) {
        if (mine_pow(2.0, (double)n) != pow(2.0, (double)n)) {
            printf("pow(2, %d) is not exact\n", n);
            bad++;
            break;
        }
    }
    printf("%-10s %-28s exact\n", "pow(2,n)", "n in [-1000, 1000]");

    /* frexp has to agree exactly: it is bit manipulation, not arithmetic. */
    {
        int bad_frexp = 0;
        for (long i = 0; i < 200000; i++) {
            const double x = rnd(-1e300, 1e300);
            int e1 = 0, e2 = 0;
            if (mine_frexp(x, &e1) != frexp(x, &e2) || e1 != e2) { bad_frexp++; break; }
        }
        printf("%-10s %-28s %s\n", "frexp", "[-1e300, 1e300]",
               bad_frexp ? "MISMATCH" : "exact");
        bad += bad_frexp;
    }

    printf("\n%s\n", bad ? "FAILED" : "all within 8 ulp");
    return bad ? 1 : 0;
}
