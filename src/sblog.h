#ifndef SBLOG_H
#define SBLOG_H
/*
 * The Sound Blaster's side of the conversation, for a driver that makes the
 * card do something wrong: every access to its ports and to the DMA
 * controller, and every interrupt it raises or drops, with the time.  Built
 * with SBLOG=1; read out over the serial link (0xF4, see DrainSerialKeys()).
 * Core 0 only.
 */
#include <stdint.h>

/* Tags: the port, | SBLOG_READ for a read; SBLOG_IRQ for the card's line. */
#define SBLOG_READ 0x10000u
#define SBLOG_IRQ  0x20000u

#if SB_IO_LOG
void sblog_note(uint32_t tag, uint8_t val);
uint32_t sblog_take(const uint32_t **entries, uint32_t *first);
/* The bytes the DMA has handed the card, in order, the last 64 KB. */
void sblog_pcm(const uint8_t *b, int n);
uint32_t sblog_pcm_take(const uint8_t **ring);
static inline int sblog_port(int addr)
{
    return (addr >= 0x220 && addr <= 0x22f) || addr <= 0x0f ||
           (addr >= 0x80 && addr <= 0x8f) || (addr >= 0xc0 && addr <= 0xdf);
}
#else
static inline void sblog_note(uint32_t tag, uint8_t val) { (void)tag; (void)val; }
static inline int sblog_port(int addr) { (void)addr; return 0; }
static inline void sblog_pcm(const uint8_t *b, int n) { (void)b; (void)n; }
#endif

#endif
