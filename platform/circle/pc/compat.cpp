/* Circle PC compatibility translation unit.  Keep this freestanding: the
 * PC model was written against the hosted C library, while Circle deliberately
 * builds without a host libc.  These small routines cover only the APIs that
 * survive into the PC target after section garbage collection. */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <circle/timer.h>

extern "C" void tiny386_circle_pc_compat_anchor(void) {}
extern "C" int32_t __fast_mul(int32_t a, int32_t b) { return (int32_t)((int64_t)a * b); }
/* The core this runs on.  sb16.c asks, so that an interrupt its mixer side
 * wants raised from core 1 is handed to core 0 instead of touching the 8259
 * there; a constant 0 would let it race the guest's interrupt acknowledge. */
extern "C" int get_core_num(void)
{
    uint64_t mpidr;
    asm volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
    return (int)(mpidr & 3u);
}

#if defined(CIRCLE_PC_DIAG)
#include "circle_pc_diag.h"

static volatile CirclePcIoDiag s_CirclePcIoRing[CIRCLE_PC_IO_RING] = {};
static volatile uint32_t s_CirclePcIoSeq = 0;
static volatile uint32_t s_CirclePcIrq0Raised = 0;
static volatile uint32_t s_CirclePcIrq0Delivered = 0;

extern "C" void circle_pc_diag_io(uint8_t direction, uint8_t width,
                                    uint8_t is_string, uint16_t port,
                                    uint32_t value, int32_t string_count)
{
    /* This target runs guest execution and UART logging on one core.  Publish
     * each slot's sequence last nevertheless, so a future asynchronous
     * observer can reject a torn record without touching device behavior. */
    const uint32_t seq = s_CirclePcIoSeq + 1u;
    volatile CirclePcIoDiag *entry = &s_CirclePcIoRing[seq & (CIRCLE_PC_IO_RING - 1u)];
    entry->direction = direction;
    entry->width = width;
    entry->is_string = is_string;
    entry->port = port;
    entry->value = value;
    entry->string_count = string_count;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    entry->seq = seq;
    s_CirclePcIoSeq = seq;
}

extern "C" void circle_pc_diag_irq0_raised(void)
{
    s_CirclePcIrq0Raised++;
}

extern "C" void circle_pc_diag_irq_delivered(uint8_t vector)
{
    /* The IBM PC/AT master PIC is initialized by the BIOS to base 08h.  Do
     * not infer delivery from CPU INTR: that latch can legitimately be a
     * different IRQ, or be withdrawn before the CPU acknowledge. */
    if (vector == 0x08u) s_CirclePcIrq0Delivered++;
}

extern "C" void circle_pc_diag_snapshot(CirclePcDiagSnapshot *out)
{
    if (!out) return;
    uint32_t before, after;
    do {
        before = s_CirclePcIoSeq;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        out->irq0_raised = s_CirclePcIrq0Raised;
        out->irq0_delivered = s_CirclePcIrq0Delivered;
        for (unsigned i = 0; i < CIRCLE_PC_IO_RING; ++i) {
            out->io[i].seq = s_CirclePcIoRing[i].seq;
            out->io[i].port = s_CirclePcIoRing[i].port;
            out->io[i].direction = s_CirclePcIoRing[i].direction;
            out->io[i].width = s_CirclePcIoRing[i].width;
            out->io[i].is_string = s_CirclePcIoRing[i].is_string;
            out->io[i].value = s_CirclePcIoRing[i].value;
            out->io[i].string_count = s_CirclePcIoRing[i].string_count;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = s_CirclePcIoSeq;
    } while (before != after);
    out->io_seq = after;
}
#endif

extern "C" long strtol(const char *s, char **endp, int base)
{
    const char *p = s; int neg = 0; long value = 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p == '-' || *p == '+') { neg = *p == '-'; ++p; }
    if (base == 0) { base = 10; if (p[0] == '0') { base = 8; ++p; if (*p == 'x' || *p == 'X') { base = 16; ++p; } } }
    else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    const char *first = p;
    for (;;) {
        unsigned d;
        if (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
        else if (*p >= 'a' && *p <= 'z') d = (unsigned)(*p - 'a' + 10);
        else if (*p >= 'A' && *p <= 'Z') d = (unsigned)(*p - 'A' + 10);
        else break;
        if (d >= (unsigned)base) break;
        value = value * base + (long)d; ++p;
    }
    if (endp) *endp = (char *)((p == first) ? s : p);
    return neg ? -value : value;
}

extern "C" long atol(const char *s) { return strtol(s, 0, 10); }

extern "C" char *strdup(const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s) + 1; char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static size_t out_char(char *dst, size_t cap, size_t at, char c)
{ if (at + 1 < cap) dst[at] = c; return at + 1; }
static size_t out_text(char *dst, size_t cap, size_t at, const char *s)
{ while (*s) at = out_char(dst, cap, at, *s++); return at; }
/* Left- and right-justified strings.  Menus lay their columns out with
 * "%-10s" and this printf used to emit the format itself instead. */
static size_t out_text_padded(char *dst, size_t cap, size_t at,
                              const char *s, int width, int left)
{
    int len = 0; while (s[len]) ++len;
    int pad = width > len ? width - len : 0;
    if (!left) while (pad--) at = out_char(dst, cap, at, ' ');
    at = out_text(dst, cap, at, s);
    if (left) while (pad--) at = out_char(dst, cap, at, ' ');
    return at;
}
static size_t out_number(char *dst, size_t cap, size_t at, unsigned long v,
                         int neg, unsigned base, int width, int zero, int upper)
{
    char tmp[32]; int n = 0;
    do { unsigned d = (unsigned)(v % base); tmp[n++] = d < 10 ? (char)('0'+d) : (char)((upper?'A':'a')+d-10); v /= base; } while (v && n < (int)sizeof tmp);
    int total = n + neg;
    while (total < width && !zero) { at = out_char(dst,cap,at,' '); ++total; }
    if (neg) at = out_char(dst,cap,at,'-');
    while (total < width) { at = out_char(dst,cap,at,'0'); ++total; }
    while (n) at = out_char(dst,cap,at,tmp[--n]);
    return at;
}

extern "C" int vsnprintf(char *dst, size_t cap, const char *fmt, __builtin_va_list ap)
{
    size_t at = 0;
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') { at = out_char(dst, cap, at, *p); continue; }
        ++p; if (!*p) break;
        int zero = 0, width = 0, left = 0;
        for (;; ++p) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else break;
        }
        while (*p >= '0' && *p <= '9') { width = width*10 + (*p++ - '0'); }
        int lng = 0; if (*p == 'l') { lng = 1; ++p; if (*p == 'l') ++p; }
        switch (*p) {
        case '%': at = out_char(dst,cap,at,'%'); break;
        case 'c': at = out_char(dst,cap,at,(char)__builtin_va_arg(ap,int)); break;
        case 's': { const char *s = __builtin_va_arg(ap,const char *); at = out_text_padded(dst,cap,at,s?s:"(null)",width,left); break; }
        case 'd': { long v = lng ? __builtin_va_arg(ap,long) : (long)__builtin_va_arg(ap,int); unsigned long u = v < 0 ? (unsigned long)(-(v+1))+1 : (unsigned long)v; at = out_number(dst,cap,at,u,v<0,10,width,zero,0); break; }
        case 'u': { unsigned long v = lng ? __builtin_va_arg(ap,unsigned long) : (unsigned long)__builtin_va_arg(ap,unsigned); at = out_number(dst,cap,at,v,0,10,width,zero,0); break; }
        case 'x': case 'X': { unsigned long v = lng ? __builtin_va_arg(ap,unsigned long) : (unsigned long)__builtin_va_arg(ap,unsigned); at = out_number(dst,cap,at,v,0,16,width,zero,*p=='X'); break; }
        default: at = out_char(dst,cap,at,'%'); at = out_char(dst,cap,at,*p); break;
        }
    }
    if (cap) dst[at < cap ? at : cap-1] = 0;
    return (int)at;
}
extern "C" int snprintf(char *dst, size_t cap, const char *fmt, ...)
{ __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = vsnprintf(dst,cap,fmt,ap); __builtin_va_end(ap); return n; }

/* ini.c's hosted FILE API is dead-stripped; keep link behavior explicit if a
 * toolchain elects to retain it.  Runtime config uses FatFs directly. */
struct __circle_file { int unused; };
extern "C" struct __circle_file *fopen(const char *, const char *) { return 0; }
extern "C" char *fgets(char *, int, struct __circle_file *) { return 0; }
extern "C" int fclose(struct __circle_file *) { return -1; }
extern "C" int usleep(unsigned int us) { if (us) CTimer::SimpleusDelay(us); return 0; }
extern "C" void abort(void) { for (;;) { } }

extern "C" uintptr_t __stack_chk_guard = (uintptr_t)0x5a5aa5a5u;
extern "C" void __stack_chk_fail(void) { abort(); }
