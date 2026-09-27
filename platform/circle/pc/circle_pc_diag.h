/* Passive PC-target diagnostic state.  This header is intentionally usable
 * from the shared C device model: all instrumentation compiles to no-ops
 * outside the explicitly selected CIRCLE_PC_DIAG artifact. */
#pragma once

#include <stdint.h>

#if defined(CIRCLE_PC_DIAG)
#define CIRCLE_PC_IO_RING 32u

typedef struct CirclePcIoDiag {
    uint32_t seq;
    uint16_t port;
    uint8_t direction;   /* 0 = IN, 1 = OUT */
    uint8_t width;       /* 1, 2, 4; string uses element width */
    uint8_t is_string;
    uint8_t reserved[3];
    uint32_t value;      /* IN result or OUT payload */
    int32_t string_count;/* completed elements; zero for scalar */
} CirclePcIoDiag;

/* A complete copy is deliberately small (32 x 16 bytes).  It is sampled
 * once per UART report, rather than printing from the I/O or IRQ hot paths. */
typedef struct CirclePcDiagSnapshot {
    uint32_t io_seq;
    uint32_t irq0_raised;
    uint32_t irq0_delivered;
    CirclePcIoDiag io[CIRCLE_PC_IO_RING];
} CirclePcDiagSnapshot;

#ifdef __cplusplus
extern "C" {
#endif
void circle_pc_diag_io(uint8_t direction, uint8_t width, uint8_t is_string,
                       uint16_t port, uint32_t value, int32_t string_count);
void circle_pc_diag_irq0_raised(void);
void circle_pc_diag_irq_delivered(uint8_t vector);
void circle_pc_diag_snapshot(CirclePcDiagSnapshot *out);
#ifdef __cplusplus
}
#endif

#define CIRCLE_PC_IO_NOTE(dir, width, string, port, value, count) \
    circle_pc_diag_io((dir), (width), (string), (uint16_t)(port), \
                      (uint32_t)(value), (int32_t)(count))
#define CIRCLE_PC_IRQ0_RAISED() circle_pc_diag_irq0_raised()
#define CIRCLE_PC_IRQ_DELIVERED(vector) circle_pc_diag_irq_delivered((uint8_t)(vector))
#else
#define CIRCLE_PC_IO_NOTE(...) ((void)0)
#define CIRCLE_PC_IRQ0_RAISED() ((void)0)
#define CIRCLE_PC_IRQ_DELIVERED(...) ((void)0)
#endif
