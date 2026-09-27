/*
 * An NE2000 in the guest, with the board's own Ethernet adapter behind it.
 *
 * The card is the one DOS and Windows already have drivers for - a packet
 * driver, or the NE2000 that Windows 95 ships with - which is the whole
 * reason for choosing a part this old.  What it is bridged to is a real
 * adapter rather than a network stack of the host's own: frames go out as
 * they are and come back as they are, so the guest is on the same network
 * as everything else, with its own address from the same DHCP server.
 *
 * The host provides the two functions in NE2000Host and nothing else.
 */
#ifndef NE2000_H
#define NE2000_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NE2000State NE2000State;

typedef struct {
    /* A frame the guest is sending.  Returns non-zero if it went. */
    int (*send)(void *ctx, const uint8_t *buf, int len);
    /* A frame for the guest, if there is one: the length, or 0. */
    int (*poll)(void *ctx, uint8_t *buf, int capacity);
    void *ctx;
} NE2000Host;

void ne2000_ioport_write(void *opaque, uint32_t addr, uint32_t val);
uint32_t ne2000_ioport_read(void *opaque, uint32_t addr);
void ne2000_reset_ioport_write(void *opaque, uint32_t addr, uint32_t val);
uint32_t ne2000_reset_ioport_read(void *opaque, uint32_t addr);
void ne2000_asic_ioport_write(void *opaque, uint32_t addr, uint32_t val);
uint32_t ne2000_asic_ioport_read(void *opaque, uint32_t addr);

/* Called from the machine's step: hands the card whatever has arrived. */
void ne2000_step(NE2000State *s);
/* Where the frames go and come from; null unbinds. */
void ne2000_set_host(const NE2000Host *host);
/* The address the card answers to; see the .c for why it is the adapter's. */
void ne2000_set_mac(NE2000State *s, const uint8_t mac[6]);

/* Implemented by the platform: puts the board's own Ethernet behind the
 * card, and gives the card that adapter's address.  Zero if there is no
 * adapter, which is not an error - the guest then has a card with an
 * unplugged cable, which is a thing a PC can be. */
int net_bind_host(NE2000State *card);

NE2000State *isa_ne2000_init(int base, int irq,
                             void *pic,
                             void (*set_irq)(void *pic, int irq, int level));

#ifdef __cplusplus
}
#endif

#endif /* NE2000_H */
