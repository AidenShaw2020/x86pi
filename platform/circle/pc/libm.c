/*
 * The part of the C maths library that the x87 emulation needs.
 *
 * Circle builds without a host C library, and the cross toolchain's libm is
 * glibc's: linking it into a bare-metal kernel pulls in objects that expect
 * errno to be thread-local, and nothing here sets up a TLS block for them.
 * That is why the FPU was switched off for this target rather than ported.
 *
 * Fourteen functions is what "no libm" amounted to, and eight of them are a
 * single AArch64 instruction.  The six that are not - exp2, log2, sin, cos,
 * tan and atan2 - are ordinary argument reduction followed by a series, and
 * they are checked against glibc off-target by libm_test.c rather than taken
 * on trust.
 *
 * Accuracy is aimed at a few units in the last place.  The emulated x87 keeps
 * its registers in double, so its mantissa is already eleven bits shorter
 * than a real 387's, and that is the larger error by far.
 */

#include <stdint.h>

/* Declared here rather than taken from <math.h>: the off-target test renames
 * every one of them so that this file and the host library can be linked
 * side by side, and a header would then disagree with the definitions. */
double fabs(double), sqrt(double), ceil(double), floor(double);
double trunc(double), nearbyint(double), copysign(double, double);
double frexp(double, int *), exp2(double), log2(double), pow(double, double);
double sin(double), cos(double), tan(double), atan(double), atan2(double, double);

/* ------------------------------------------------------------------------ */
/* The ones the processor does itself                                        */
/* ------------------------------------------------------------------------ */

#if defined(__aarch64__)
#define FPU_OP1(insn, x) ({ double r_; __asm__ ("" insn " %d0, %d1" : "=w"(r_) : "w"(x)); r_; })
double fabs(double x)      { return FPU_OP1("fabs",   x); }
double sqrt(double x)      { return FPU_OP1("fsqrt",  x); }
double ceil(double x)      { return FPU_OP1("frintp", x); }
double floor(double x)     { return FPU_OP1("frintm", x); }
double trunc(double x)     { return FPU_OP1("frintz", x); }
double nearbyint(double x) { return FPU_OP1("frinti", x); }
#else
/* Only for the off-target test, where the host's own routines are the ones
 * being compared against and these are never the interesting part. */
double fabs(double x)      { return __builtin_fabs(x); }
double sqrt(double x)      { return __builtin_sqrt(x); }
double ceil(double x)      { return __builtin_ceil(x); }
double floor(double x)     { return __builtin_floor(x); }
double trunc(double x)     { return __builtin_trunc(x); }
double nearbyint(double x) { return __builtin_nearbyint(x); }
#endif

static inline uint64_t bits(double x) { uint64_t u; __builtin_memcpy(&u, &x, 8); return u; }
static inline double   dbl(uint64_t u) { double x; __builtin_memcpy(&x, &u, 8); return x; }

double copysign(double x, double y)
{
    return dbl((bits(x) & 0x7fffffffffffffffULL) | (bits(y) & 0x8000000000000000ULL));
}

double frexp(double x, int *e)
{
    uint64_t u = bits(x);
    int ex = (int)((u >> 52) & 0x7ffu);
    if (ex == 0x7ff || x == 0.0) { *e = 0; return x; }   /* inf, NaN, zeroes */
    if (ex == 0) {                                       /* subnormal */
        x *= 18014398509481984.0;                        /* 2^54 */
        u = bits(x);
        ex = (int)((u >> 52) & 0x7ffu) - 54;
    }
    *e = ex - 1022;                                      /* mantissa in [0.5,1) */
    return dbl((u & ~(0x7ffULL << 52)) | (1022ULL << 52));
}

/* x * 2^n, by arithmetic rather than by building the exponent, so overflow,
 * underflow and the gradual loss into subnormals all behave. */
static double scale2(double x, int n)
{
    if (n > 1023) {
        x *= 8.98846567431158e307;  /* 2^1023 */
        n -= 1023;
        if (n > 1023) { x *= 8.98846567431158e307; n -= 1023; if (n > 1023) n = 1023; }
    } else if (n < -1022) {
        x *= 2.2250738585072014e-308 * 4.450147717014403e-308 * 4.503599627370496e15;
        n += 1022;
        if (n < -1022) {
            x *= 2.2250738585072014e-308 * 4.450147717014403e-308 * 4.503599627370496e15;
            n += 1022;
            if (n < -1022) n = -1022;
        }
    }
    return x * dbl((uint64_t)(n + 1023) << 52);
}

/* ------------------------------------------------------------------------ */
/* exp2, and pow - which this emulator only ever calls with a base of two     */
/* ------------------------------------------------------------------------ */

/*
 * 2^x = 2^n * 2^f, with n whole and |f| <= 1/2.  The coefficients are
 * (ln 2)^k / k!, taken far enough that the first term dropped is below the
 * last bit of the result over the whole of that interval.
 */
static double exp2_frac(double f)
{
    static const double c[] = {
        6.93147180559945286e-01, 2.40226506959100712e-01, 5.55041086648215799e-02,
        9.61812910762847716e-03, 1.33335581464284434e-03, 1.54035303933816110e-04,
        1.52527338040598403e-05, 1.32154867901443094e-06, 1.01780860092391030e-07,
        7.05491162282175690e-09, 4.44553827187081526e-10, 2.56784359934882012e-11,
        1.36930210424376389e-12,
    };
    double r = c[12];
    for (int i = 11; i >= 0; i--) r = r * f + c[i];
    return 1.0 + f * r;
}

double exp2(double x)
{
    if (x != x) return x;                      /* NaN */
    if (x >= 1025.0) return dbl(0x7ff0000000000000ULL);
    if (x <= -1080.0) return 0.0;
    const double n = nearbyint(x);
    return scale2(exp2_frac(x - n), (int)n);
}

double pow(double x, double y)
{
    if (y == 0.0) return 1.0;
    if (x != x || y != y) return x != x ? x : y;
    if (x == 1.0) return 1.0;

    /* An integer exponent is exact when the base is a power of two, which is
     * the only way FSCALE uses this. */
    const double ny = nearbyint(y);
    if (x == 2.0 && ny == y && y > -2000.0 && y < 2000.0)
        return scale2(1.0, (int)y);

    if (x > 0.0) return exp2(y * log2(x));

    if (x == 0.0) return y > 0.0 ? 0.0 : dbl(0x7ff0000000000000ULL);

    /* Negative base: defined only for a whole exponent. */
    if (ny != y) return dbl(0x7ff8000000000000ULL);     /* NaN */
    const double m = exp2(y * log2(-x));
    return ((int64_t)ny & 1) ? -m : m;
}

/* ------------------------------------------------------------------------ */
/* log2                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * log2(x) = e + log2(m), with m brought into [sqrt(1/2), sqrt(2)] so that
 * s = (m-1)/(m+1) stays under 0.172 and the odd series in s converges fast.
 */
double log2(double x)
{
    if (x != x) return x;
    if (x < 0.0) return dbl(0x7ff8000000000000ULL);      /* NaN */
    if (x == 0.0) return -dbl(0x7ff0000000000000ULL);
    if (x == dbl(0x7ff0000000000000ULL)) return x;

    int e;
    double m = frexp(x, &e);                             /* m in [0.5, 1) */
    if (m < 0.70710678118654752440) { m *= 2.0; e--; }

    const double s = (m - 1.0) / (m + 1.0);
    const double s2 = s * s;
    /* log(m) = 2 * sum s^(2k+1)/(2k+1) */
    double sum = 1.0 / 21.0;
    static const double inv[] = {
        1.0/19.0, 1.0/17.0, 1.0/15.0, 1.0/13.0, 1.0/11.0,
        1.0/9.0,  1.0/7.0,  1.0/5.0,  1.0/3.0,  1.0
    };
    for (int i = 0; i < 10; i++) sum = sum * s2 + inv[i];
    /* 2/ln2 */
    return (double)e + s * sum * 2.88539008177792681472;
}

/* ------------------------------------------------------------------------ */
/* sin, cos, tan                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Argument reduction: x = k * pi/2 + r, with |r| <= pi/4.
 *
 * The usual trick is to hold pi/2 in pieces whose leading bits are zero, so
 * that k times a piece is exact.  That only works while k is small - each
 * piece carries a fixed number of significant bits, and once k has enough of
 * its own the product no longer fits.  Beyond about 1.6e6 it stops working,
 * and sin() of 9.35e8 came back wrong by everything.
 *
 * A fused multiply-add gives the exact product of any two doubles as a head
 * and a tail, so pi/2 can be held in ordinary full-precision pieces and each
 * product split exactly.  Four pieces carry pi/2 to about 210 bits, which is
 * more than any double argument can ask for.  The first subtraction is exact
 * by Sterbenz's lemma, x and k*pi/2 being within a factor of two of each
 * other, so nothing is lost before the small terms are taken off.
 */
#define PI2_0  1.57079632679489655800e+00
#define PI2_1  6.12323399573676603587e-17
#define PI2_2 -1.49738490485916983294e-33
#define PI2_3  5.56227110431682640773e-50
#define TWO_OVER_PI 6.36619772367581382433e-01

static int reduce_half_pi(double x, double *rhead, double *rtail)
{
    const double kd = nearbyint(x * TWO_OVER_PI);

    /* p is the head of kd*PI2_0 and e its exact tail. */
    double p = kd * PI2_0;
    double e = __builtin_fma(kd, PI2_0, -p);
    double r = (x - p) - e;

    p = kd * PI2_1;
    e = __builtin_fma(kd, PI2_1, -p);
    r = (r - p) - e;

    r -= kd * PI2_2;
    r -= kd * PI2_3;

    /* Split the result so the kernels can use the bits below the head. */
    const double head = r;
    *rhead = head;
    *rtail = r - head;
    return (int)((int64_t)kd & 3);
}

static double sin_core(double r)          /* |r| <= pi/4 */
{
    static const double c[] = {
        -1.66666666666666657415e-01,  8.33333333333329961475e-03,
        -1.98412698412589187999e-04,  2.75573192101527564362e-06,
        -2.50521067982745845270e-08,  1.60590431721336815300e-10,
        -7.64712219139183857119e-13,  2.81009972710884058136e-15,
    };
    const double r2 = r * r;
    double s = c[7];
    for (int i = 6; i >= 0; i--) s = s * r2 + c[i];
    return r + r * r2 * s;
}

static double cos_core(double r)          /* |r| <= pi/4 */
{
    static const double c[] = {
        4.16666666666666643537e-02, -1.38888888888888057971e-03,
        2.48015873015657842395e-05, -2.75573192101527564362e-07,
        2.08767569870480217294e-09, -1.14707451146503341740e-11,
        4.77947733238737981841e-14, -1.55753008819523471917e-16,
    };
    const double r2 = r * r;
    double s = c[7];
    for (int i = 6; i >= 0; i--) s = s * r2 + c[i];
    return 1.0 - 0.5 * r2 + r2 * r2 * s;
}

/* The reduction hands back a head and a tail; adding the tail back in is what
 * the last few bits of a large argument come down to. */
static void sincos_at(double a, double b, double *sn, double *cs)
{
    const double sa = sin_core(a), ca = cos_core(a);
    *sn = sa + b * ca;
    *cs = ca - b * sa;
}

static int is_special(double x)
{
    return x != x || fabs(x) == dbl(0x7ff0000000000000ULL);
}

double sin(double x)
{
    if (is_special(x)) return dbl(0x7ff8000000000000ULL);
    double a, b, sn, cs;
    const int q = reduce_half_pi(x, &a, &b);
    sincos_at(a, b, &sn, &cs);
    switch (q) {
    case 0:  return  sn;
    case 1:  return  cs;
    case 2:  return -sn;
    default: return -cs;
    }
}

double cos(double x)
{
    if (is_special(x)) return dbl(0x7ff8000000000000ULL);
    double a, b, sn, cs;
    const int q = reduce_half_pi(x, &a, &b);
    sincos_at(a, b, &sn, &cs);
    switch (q) {
    case 0:  return  cs;
    case 1:  return -sn;
    case 2:  return -cs;
    default: return  sn;
    }
}

double tan(double x)
{
    if (is_special(x)) return dbl(0x7ff8000000000000ULL);
    double a, b, sn, cs;
    const int q = reduce_half_pi(x, &a, &b);
    sincos_at(a, b, &sn, &cs);
    return (q & 1) ? -cs / sn : sn / cs;
}

/* ------------------------------------------------------------------------ */
/* atan, atan2                                                               */
/* ------------------------------------------------------------------------ */

#define PI      3.14159265358979323846
#define PI_2    1.57079632679489661923
#define PI_6    0.52359877559829887308

/* atan(t) for |t| <= tan(pi/12), where the odd series still converges in a
 * reasonable number of terms. */
static double atan_core(double t)
{
    const double t2 = t * t;
    static const double inv[] = {
        1.0/29.0, 1.0/27.0, 1.0/25.0, 1.0/23.0, 1.0/21.0, 1.0/19.0, 1.0/17.0,
        1.0/15.0, 1.0/13.0, 1.0/11.0, 1.0/9.0,  1.0/7.0,  1.0/5.0,  1.0/3.0,
    };
    double sum = -1.0 / 31.0;
    for (int i = 0; i < 14; i++) sum = -(sum * t2 - inv[i]);
    return t * (1.0 - t2 * sum);
}

static double atan_pos(double x)          /* x >= 0, finite */
{
    if (x > 1.0) return PI_2 - atan_pos(1.0 / x);
    if (x > 0.26794919243112270647) {     /* tan(pi/12) */
        /* atan(x) = pi/6 + atan((x*sqrt3 - 1) / (sqrt3 + x)) */
        const double sqrt3 = 1.73205080756887729353;
        return PI_6 + atan_core((x * sqrt3 - 1.0) / (sqrt3 + x));
    }
    return atan_core(x);
}

double atan(double x)
{
    if (x != x) return x;
    return x < 0.0 ? -atan_pos(-x) : atan_pos(x);
}

double atan2(double y, double x)
{
    if (x != x || y != y) return dbl(0x7ff8000000000000ULL);
    if (y == 0.0) {
        /* Zero keeps its sign, which is what puts the result on the right
         * side of the branch cut. */
        if (bits(x) >> 63) return (bits(y) >> 63) ? -PI : PI;
        return y;
    }
    if (x == 0.0) return (bits(y) >> 63) ? -PI_2 : PI_2;

    const double a = atan_pos(fabs(y) / fabs(x));
    const double q = (bits(x) >> 63) ? PI - a : a;
    return (bits(y) >> 63) ? -q : q;
}
