/*
 * Drawing the boot logo.
 *
 * The unpacking is the other half of platform/circle/tools/mklogo.py: a flag
 * byte, then eight items, each either a literal byte or a two byte reference
 * to something already unpacked.  Twenty lines, and it is the reason the
 * picture costs the kernel 118K instead of 533K.
 */
#include "bootlogo.h"

#include <stdlib.h>

static void unpack(const unsigned char *in, unsigned in_size,
                   unsigned char *out, unsigned out_size)
{
    unsigned i = 0, o = 0;

    while (i < in_size && o < out_size) {
        unsigned flags = in[i++];
        int bit;

        for (bit = 0; bit < 8 && o < out_size; bit++) {
            if (i >= in_size)
                return;
            if (flags & (1u << bit)) {
                if (i + 1 >= in_size)
                    return;
                /* Stored one less than it is, so that a distance of the
                   full window still fits in twelve bits. */
                unsigned distance = (((unsigned)in[i] << 4) | (in[i + 1] >> 4)) + 1u;
                unsigned length = (in[i + 1] & 0x0Fu) + 3u;
                i += 2;
                if (distance > o)
                    return;
                while (length-- && o < out_size) {
                    out[o] = out[o - distance];
                    o++;
                }
            } else {
                out[o++] = in[i++];
            }
        }
    }
}

void bootlogo_draw(void *framebuffer, unsigned width, unsigned height,
                   unsigned pitch)
{
    const unsigned logo_w = bootlogo_width, logo_h = bootlogo_height;
    const unsigned raw_size = logo_w * logo_h * 2u;

    if (!framebuffer || width < logo_w || height < logo_h)
        return;

    unsigned char *raw = (unsigned char *)malloc(raw_size);
    if (!raw)
        return;
    /* Black, so that a picture which somehow does not unpack in full shows
     * what there is on a black field rather than a band of whatever the heap
     * was holding. */
    for (unsigned i = 0; i < raw_size; i++)
        raw[i] = 0;

    unpack(bootlogo_packed, bootlogo_packed_size, raw, raw_size);

    const unsigned left = (width - logo_w) / 2u;
    const unsigned top = (height - logo_h) / 2u;

    for (unsigned y = 0; y < logo_h; y++) {
        unsigned int *row = (unsigned int *)
            ((unsigned char *)framebuffer + (unsigned long)(top + y) * pitch)
            + left;
        const unsigned char *src = raw + (unsigned long)y * logo_w * 2u;

        for (unsigned x = 0; x < logo_w; x++) {
            unsigned v = src[x * 2u] | ((unsigned)src[x * 2u + 1] << 8);
            /* 5:6:5 back out to 8:8:8, with the top bits repeated in the low
             * ones so that white stays white rather than 0xF8F8F8. */
            unsigned r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            row[x] = ((r << 3 | r >> 2) << 16)
                   | ((g << 2 | g >> 4) << 8)
                   | (b << 3 | b >> 2);
        }
    }

    free(raw);
}
