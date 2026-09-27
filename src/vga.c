/*
 * Dummy VGA device
 * 
 * Copyright (c) 2003-2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
// Enable Ofast optimization for VGA emulation
#pragma GCC optimize("Ofast")

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "vga.h"
#include "pci.h"

#ifdef BUILD_ESP32
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif


#ifdef BUILD_ESP32
void *pcmalloc(long size);
#else
#define pcmalloc malloc
#endif

inline static int after_eq(uint32_t a, uint32_t b)
{
    return (a - b) < (1u << 31);
}

#if BPP == 32
static void vga_draw_glyph8(uint8_t *d, int linesize,
                            const uint8_t *font_ptr, int h,
                            uint32_t fgcol, uint32_t bgcol)
{
    uint32_t font_data, xorcol;

    xorcol = bgcol ^ fgcol;
    do {
        font_data = font_ptr[0];
        ((uint32_t *)d)[0] = (-((font_data >> 7)) & xorcol) ^ bgcol;
        ((uint32_t *)d)[1] = (-((font_data >> 6) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[2] = (-((font_data >> 5) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[3] = (-((font_data >> 4) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[4] = (-((font_data >> 3) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[5] = (-((font_data >> 2) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[6] = (-((font_data >> 1) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[7] = (-((font_data >> 0) & 1) & xorcol) ^ bgcol;
        font_ptr += 4;
        d += linesize;
    } while (--h);
}

static void vga_draw_glyph9(uint8_t *d, int linesize,
                            const uint8_t *font_ptr, int h,
                            uint32_t fgcol, uint32_t bgcol,
                            int dup9)
{
    uint32_t font_data, xorcol, v;

    xorcol = bgcol ^ fgcol;
    do {
        font_data = font_ptr[0];
        ((uint32_t *)d)[0] = (-((font_data >> 7)) & xorcol) ^ bgcol;
        ((uint32_t *)d)[1] = (-((font_data >> 6) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[2] = (-((font_data >> 5) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[3] = (-((font_data >> 4) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[4] = (-((font_data >> 3) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[5] = (-((font_data >> 2) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[6] = (-((font_data >> 1) & 1) & xorcol) ^ bgcol;
        v = (-((font_data >> 0) & 1) & xorcol) ^ bgcol;
        ((uint32_t *)d)[7] = v;
        if (dup9)
            ((uint32_t *)d)[8] = v;
        else
            ((uint32_t *)d)[8] = bgcol;
        font_ptr += 4;
        d += linesize;
    } while (--h);
}
#elif BPP == 16
static void vga_draw_glyph8(uint8_t *d, int linesize,
                            const uint8_t *font_ptr, int h,
                            uint32_t fgcol, uint32_t bgcol)
{
    uint32_t font_data, xorcol;

    xorcol = bgcol ^ fgcol;
    do {
        font_data = font_ptr[0];
        ((uint16_t *)d)[0] = (-((font_data >> 7)) & xorcol) ^ bgcol;
        ((uint16_t *)d)[1] = (-((font_data >> 6) & 1) & xorcol) ^ bgcol;
        ((uint16_t *)d)[2] = (-((font_data >> 5) & 1) & xorcol) ^ bgcol;
        ((uint16_t *)d)[3] = (-((font_data >> 4) & 1) & xorcol) ^ bgcol;
        ((uint16_t *)d)[4] = (-((font_data >> 3) & 1) & xorcol) ^ bgcol;
        ((uint16_t *)d)[5] = (-((font_data >> 2) & 1) & xorcol) ^ bgcol;
        ((uint16_t *)d)[6] = (-((font_data >> 1) & 1) & xorcol) ^ bgcol;
        ((uint16_t *)d)[7] = (-((font_data >> 0) & 1) & xorcol) ^ bgcol;
        font_ptr += 4;
        d += linesize;
    } while (--h);
}

static void vga_draw_glyph9(uint8_t *d, int linesize,
                            const uint8_t *font_ptr, int h,
                            uint32_t fgcol, uint32_t bgcol,
                            int dup9)
{
    uint32_t font_data, xorcol, v;

    xorcol = bgcol ^ fgcol;
    do {
        font_data = font_ptr[0];
        ((uint16_t *)d)[0] = ((-((font_data >> 7)) & xorcol) ^ bgcol);
        ((uint16_t *)d)[1] = ((-((font_data >> 6) & 1) & xorcol) ^ bgcol);
        ((uint16_t *)d)[2] = ((-((font_data >> 5) & 1) & xorcol) ^ bgcol);
        ((uint16_t *)d)[3] = ((-((font_data >> 4) & 1) & xorcol) ^ bgcol);
        ((uint16_t *)d)[4] = ((-((font_data >> 3) & 1) & xorcol) ^ bgcol);
        ((uint16_t *)d)[5] = ((-((font_data >> 2) & 1) & xorcol) ^ bgcol);
        ((uint16_t *)d)[6] = ((-((font_data >> 1) & 1) & xorcol) ^ bgcol);
        v = (-((font_data >> 0) & 1) & xorcol) ^ bgcol;
        ((uint16_t *)d)[7] = v;
        if (dup9)
            ((uint16_t *)d)[8] = v;
        else
            ((uint16_t *)d)[8] = bgcol;
        font_ptr += 4;
        d += linesize;
    } while (--h);
}

static inline uint32_t c69(uint16_t c)
{
    // 0000 0000 0000 rrrr rggg gggb bbbb
    // 0000 0rrr rr00 0ggg ggg0 000b bbbb
    return (c & 0x1f) | ((c & 0x7e0) << 4) | ((c & 0xf800) << 7);
}

static inline uint16_t c96(uint32_t c)
{
    // 0000 0rrr rr00 0ggg ggg0 000b bbbb
    // 0000 0000 0000 rrrr rggg gggb bbbb
    uint16_t t = (c & 0x1f) | ((c & 0x7e00) >> 4) | ((c & 0x7c0000) >> 7);
#ifdef SWAP_BYTEORDER_BPP16
    return (t << 8) | (t >> 8);
#else
    return t;
#endif
}

static void scale_3_2(uint8_t *dst, int dst_stride, uint8_t *src, int w)
{
   const static int shift[4][4] = {
       { 2, 0, 1, 0 },
       { 1, 2, 0, 0 },
       { 0, 0, 2, 1 },
       { 0, 1, 0, 2 }
   };
   int ww = w / 3 * 2;
   int idx = 0;
   for (int j = 0; j < 2; j++, idx ^= 2,
#ifdef SWAPXY
                dst += BPP / 8
#else
                dst += dst_stride
#endif
           ) {
       uint8_t *dst1 = dst;
       uint8_t *src1 = src + (j * w) * (BPP / 8);
       for (int k = 0; k < ww; k++, idx ^= 1) {
           int kk = k / 2 * 3 + (k & 1);
           uint16_t *p0 = (uint16_t *) (src1 + kk * (BPP / 8));
           uint16_t *p1 = p0 + 1;
           uint16_t *p2 = p0 + w;
           uint16_t *p3 = p1 + w;
           int sh0 = shift[idx][0];
           int sh1 = shift[idx][1];
           int sh2 = shift[idx][2];
           int sh3 = shift[idx][3];
           *(uint16_t *)dst1 = c96(((c69(*p0) << sh0) + (c69(*p1) << sh1) +
                                    (c69(*p2) << sh2) + (c69(*p3) << sh3)) >> 3);
#ifdef SWAPXY
           dst1 += dst_stride;
#else
           dst1 += BPP / 8;
#endif
       }
   }
}

static void scale_3_3(uint8_t *dst, int dst_stride, uint8_t *src, int w)
{
   for (int j = 0; j < 3; j++,
#ifdef SWAPXY
                dst += BPP / 8
#else
                dst += dst_stride
#endif
           ) {
       uint8_t *dst1 = dst;
       uint8_t *src1 = src + (j * w) * (BPP / 8);
       for (int k = 0; k < w; k++) {
           *(uint16_t *)dst1 = *(uint16_t *) (src1 + k * (BPP / 8));
#ifdef SWAPXY
           dst1 += dst_stride;
#else
           dst1 += BPP / 8;
#endif
       }
   }
}

#else
#error "bad bpp"
#endif

static const uint8_t cursor_glyph[32] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

#if BPP == 32
static inline int c6_to_8(int v)
{
    int b;
    v &= 0x3f;
    b = v & 1;
    return (v << 2) | (b << 1) | b;
}

static inline unsigned int rgb_to_pixel(unsigned int r, unsigned int g,
                                        unsigned int b)
{
    /*
     * One packing for every target, including Circle.
     *
     * A CIRCLE_BUILD branch here used to return (b << 16) | (g << 8) | r,
     * on the reasoning that the Pi firmware framebuffer is RGB while this
     * renderer's 32-bit target is BGR.  It was recorded as a verified fix
     * for a wrong red/blue palette, but it is what produces one.  The image
     * whose SHA-256 is 29cf4751... is the build carrying that branch and is
     * what the SD card boots, and on it every cyan in M602 comes out yellow
     * and every blue comes out red: a plain red/blue exchange.
     *
     * Nothing in Circle asks for the other order either.  It never sends the
     * SET_PIXEL_ORDER mailbox tag, so the component order is whatever the
     * firmware defaults to, and it declares a 32-bit surface as ARGB8888 -
     * red at bits 16..23, which is exactly this expression.
     */
    return (r << 16) | (g << 8) | b;
}

static int update_palette256(VGAState *s, uint32_t *palette)
{
    int full_update, i;
    uint32_t v, col;

    full_update = 0;
    v = 0;
    for(i = 0; i < 256; i++) {
        if (s->dac_8bit) {
          col = rgb_to_pixel(s->palette[v],
                             s->palette[v + 1],
                             s->palette[v + 2]);
        } else {
          col = rgb_to_pixel(c6_to_8(s->palette[v]),
                             c6_to_8(s->palette[v + 1]),
                             c6_to_8(s->palette[v + 2]));
        }
        if (col != palette[i]) {
            full_update = 1;
            palette[i] = col;
        }
        v += 3;
    }
    return full_update;
}

static int update_palette16(VGAState *s, uint32_t *palette)
{
    int full_update, i;
    uint32_t v, col;

    full_update = 0;
    for(i = 0; i < 16; i++) {
        v = s->ar[i];
        if (s->ar[0x10] & 0x80)
            v = ((s->ar[0x14] & 0xf) << 4) | (v & 0xf);
        else
            v = ((s->ar[0x14] & 0xc) << 4) | (v & 0x3f);
        v = v * 3;
        col = rgb_to_pixel(c6_to_8(s->palette[v]),
                           c6_to_8(s->palette[v + 1]),
                           c6_to_8(s->palette[v + 2]));
        if (col != palette[i]) {
            full_update = 1;
            palette[i] = col;
        }
    }
    return full_update;
}
#elif BPP == 16
static int update_palette256(VGAState *s, uint32_t *palette)
{
    int full_update, i;
    uint32_t v, col;

    full_update = 0;
    v = 0;
    for(i = 0; i < 256; i++) {
        if (s->dac_8bit) {
            col = ((s->palette[v + 2] >> 3)) |
                ((s->palette[v + 1] >> 2) << 5) |
                ((s->palette[v] >> 3) << 11);
        } else {
            col = (s->palette[v + 2] >> 1) |
                ((s->palette[v + 1]) << 5) |
                ((s->palette[v] >> 1) << 11);
        }
        if (col != palette[i]) {
            full_update = 1;
            palette[i] = col;
        }
        v += 3;
    }
    return full_update;
}

static int update_palette16(VGAState *s, uint32_t *palette)
{
    int full_update, i;
    uint32_t v, col;

    full_update = 0;
    for(i = 0; i < 16; i++) {
        v = s->ar[i];
        if (s->ar[0x10] & 0x80)
            v = ((s->ar[0x14] & 0xf) << 4) | (v & 0xf);
        else
            v = ((s->ar[0x14] & 0xc) << 4) | (v & 0x3f);
        v = v * 3;
        col = (s->palette[v + 2] >> 1) |
              ((s->palette[v + 1]) << 5) |
              ((s->palette[v] >> 1) << 11);
        if (col != palette[i]) {
            full_update = 1;
            palette[i] = col;
        }
    }
    return full_update;
}
#else
#error "bad bpp"
#endif

/* VGA CRT controller register indices */
#define VGA_CRTC_H_TOTAL        0
#define VGA_CRTC_H_DISP         1
#define VGA_CRTC_H_BLANK_START  2
#define VGA_CRTC_H_BLANK_END    3
#define VGA_CRTC_H_SYNC_START   4
#define VGA_CRTC_H_SYNC_END     5
#define VGA_CRTC_V_TOTAL        6
#define VGA_CRTC_OVERFLOW       7
#define VGA_CRTC_PRESET_ROW     8
#define VGA_CRTC_MAX_SCAN       9
#define VGA_CRTC_CURSOR_START   0x0A
#define VGA_CRTC_CURSOR_END     0x0B
#define VGA_CRTC_START_HI       0x0C
#define VGA_CRTC_START_LO       0x0D
#define VGA_CRTC_CURSOR_HI      0x0E
#define VGA_CRTC_CURSOR_LO      0x0F
#define VGA_CRTC_V_SYNC_START   0x10
#define VGA_CRTC_V_SYNC_END     0x11
#define VGA_CRTC_V_DISP_END     0x12
#define VGA_CRTC_OFFSET         0x13
#define VGA_CRTC_UNDERLINE      0x14
#define VGA_CRTC_V_BLANK_START  0x15
#define VGA_CRTC_V_BLANK_END    0x16
#define VGA_CRTC_MODE           0x17
#define VGA_CRTC_LINE_COMPARE   0x18
#define VGA_CRTC_REGS           VGA_CRT_C

/* VGA sequencer register indices */
#define VGA_SEQ_RESET           0x00
#define VGA_SEQ_CLOCK_MODE      0x01
#define VGA_SEQ_PLANE_WRITE     0x02
#define VGA_SEQ_CHARACTER_MAP   0x03
#define VGA_SEQ_MEMORY_MODE     0x04

/* VGA sequencer register bit masks */
#define VGA_SR01_CHAR_CLK_8DOTS 0x01 /* bit 0: character clocks 8 dots wide are generated */
#define VGA_SR01_SCREEN_OFF     0x20 /* bit 5: Screen is off */
#define VGA_SR02_ALL_PLANES     0x0F /* bits 3-0: enable access to all planes */
#define VGA_SR04_EXT_MEM        0x02 /* bit 1: allows complete mem access to 256K */
#define VGA_SR04_SEQ_MODE       0x04 /* bit 2: directs system to use a sequential addressing mode */
#define VGA_SR04_CHN_4M         0x08 /* bit 3: selects modulo 4 addressing for CPU access to display memory */

/* VGA graphics controller register indices */
#define VGA_GFX_SR_VALUE        0x00
#define VGA_GFX_SR_ENABLE       0x01
#define VGA_GFX_COMPARE_VALUE   0x02
#define VGA_GFX_DATA_ROTATE     0x03
#define VGA_GFX_PLANE_READ      0x04
#define VGA_GFX_MODE            0x05
#define VGA_GFX_MISC            0x06
#define VGA_GFX_COMPARE_MASK    0x07
#define VGA_GFX_BIT_MASK        0x08

/* VGA graphics controller bit masks */
#define VGA_GR06_GRAPHICS_MODE  0x01

static bool vbe_enabled(VGAState *s)
{
    return s->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_ENABLED;
}


/*
 * Sanity check vbe register writes.
 *
 * As we don't have a way to signal errors to the guest in the bochs
 * dispi interface we'll go adjust the registers to the closest valid
 * value.
 */
static void vbe_fixup_regs(VGAState *s)
{
    uint16_t *r = s->vbe_regs;
    uint32_t bits, linelength, /*maxy,*/ offset;

    if (!vbe_enabled(s)) {
        /* vbe is turned off -- nothing to do */
        return;
    }

    /* check depth */
    switch (r[VBE_DISPI_INDEX_BPP]) {
    case 4:
    case 8:
    case 16:
    case 24:
    case 32:
        bits = r[VBE_DISPI_INDEX_BPP];
        break;
    case 15:
        bits = 16;
        break;
    default:
        bits = r[VBE_DISPI_INDEX_BPP] = 8;
        break;
    }

    /* check width */
    r[VBE_DISPI_INDEX_XRES] &= ~7u;
    if (r[VBE_DISPI_INDEX_XRES] == 0) {
        r[VBE_DISPI_INDEX_XRES] = 8;
    }
//    if (r[VBE_DISPI_INDEX_XRES] > VBE_DISPI_MAX_XRES) {
//        r[VBE_DISPI_INDEX_XRES] = VBE_DISPI_MAX_XRES;
//    }
    r[VBE_DISPI_INDEX_VIRT_WIDTH] &= ~7u;
//    if (r[VBE_DISPI_INDEX_VIRT_WIDTH] > VBE_DISPI_MAX_XRES) {
//        r[VBE_DISPI_INDEX_VIRT_WIDTH] = VBE_DISPI_MAX_XRES;
//    }
    if (r[VBE_DISPI_INDEX_VIRT_WIDTH] < r[VBE_DISPI_INDEX_XRES]) {
        r[VBE_DISPI_INDEX_VIRT_WIDTH] = r[VBE_DISPI_INDEX_XRES];
    }

    /* check height */
    linelength = r[VBE_DISPI_INDEX_VIRT_WIDTH] * bits / 8;
//    maxy = s->vbe_size / linelength;
    if (r[VBE_DISPI_INDEX_YRES] == 0) {
        r[VBE_DISPI_INDEX_YRES] = 1;
    }
//    if (r[VBE_DISPI_INDEX_YRES] > VBE_DISPI_MAX_YRES) {
//        r[VBE_DISPI_INDEX_YRES] = VBE_DISPI_MAX_YRES;
//    }
//    if (r[VBE_DISPI_INDEX_YRES] > maxy) {
//        r[VBE_DISPI_INDEX_YRES] = maxy;
//    }

    /* check offset */
//    if (r[VBE_DISPI_INDEX_X_OFFSET] > VBE_DISPI_MAX_XRES) {
//        r[VBE_DISPI_INDEX_X_OFFSET] = VBE_DISPI_MAX_XRES;
//    }
//    if (r[VBE_DISPI_INDEX_Y_OFFSET] > VBE_DISPI_MAX_YRES) {
//        r[VBE_DISPI_INDEX_Y_OFFSET] = VBE_DISPI_MAX_YRES;
//    }
    offset = r[VBE_DISPI_INDEX_X_OFFSET] * bits / 8;
    offset += r[VBE_DISPI_INDEX_Y_OFFSET] * linelength;
//    if (offset + r[VBE_DISPI_INDEX_YRES] * linelength > s->vbe_size) {
//        r[VBE_DISPI_INDEX_Y_OFFSET] = 0;
//        offset = r[VBE_DISPI_INDEX_X_OFFSET] * bits / 8;
//        if (offset + r[VBE_DISPI_INDEX_YRES] * linelength > s->vbe_size) {
//            r[VBE_DISPI_INDEX_X_OFFSET] = 0;
//            offset = 0;
//        }
//    }

    /* update vga state */
//    r[VBE_DISPI_INDEX_VIRT_HEIGHT] = maxy;
    s->vbe_line_offset = linelength;
    s->vbe_start_addr  = offset / 4;
}

static void vbe_update_vgaregs(VGAState *s)
{
    int h, shift_control;

    if (!vbe_enabled(s)) {
        /* vbe is turned off -- nothing to do */
        return;
    }

    /* graphic mode + memory map 1 */
    s->gr[VGA_GFX_MISC] = (s->gr[VGA_GFX_MISC] & ~0x0c) | 0x04 |
        VGA_GR06_GRAPHICS_MODE;
    s->cr[VGA_CRTC_MODE] |= 3; /* no CGA modes */
    s->cr[VGA_CRTC_OFFSET] = s->vbe_line_offset >> 3;
    /* width */
    s->cr[VGA_CRTC_H_DISP] =
        (s->vbe_regs[VBE_DISPI_INDEX_XRES] >> 3) - 1;
    /* height (only meaningful if < 1024) */
    h = s->vbe_regs[VBE_DISPI_INDEX_YRES] - 1;
    s->cr[VGA_CRTC_V_DISP_END] = h;
    s->cr[VGA_CRTC_OVERFLOW] = (s->cr[VGA_CRTC_OVERFLOW] & ~0x42) |
        ((h >> 7) & 0x02) | ((h >> 3) & 0x40);
    /* line compare to 1023 */
    s->cr[VGA_CRTC_LINE_COMPARE] = 0xff;
    s->cr[VGA_CRTC_OVERFLOW] |= 0x10;
    s->cr[VGA_CRTC_MAX_SCAN] |= 0x40;

    if (s->vbe_regs[VBE_DISPI_INDEX_BPP] == 4) {
        shift_control = 0;
        s->sr/*_vbe*/[VGA_SEQ_CLOCK_MODE] &= ~8; /* no double line */
    } else {
        shift_control = 2;
        /* set chain 4 mode */
        s->sr/*_vbe*/[VGA_SEQ_MEMORY_MODE] |= VGA_SR04_CHN_4M;
        /* activate all planes */
        s->sr/*_vbe*/[VGA_SEQ_PLANE_WRITE] |= VGA_SR02_ALL_PLANES;
    }
    s->gr[VGA_GFX_MODE] = (s->gr[VGA_GFX_MODE] & ~0x60) |
        (shift_control << 5);
    s->cr[VGA_CRTC_MAX_SCAN] &= ~0x9f; /* no double scan */
}

#if !defined(SCALE_3_2) && !defined(SWAPXY) && BPP == 32
/*
 * Text a scan line at a time, for what the cell renderer below cannot show.
 *
 * That renderer draws whole character cells from the start address down, so
 * it has no place for the split screen (below the line compare the card
 * fetches from address zero again), a preset row scan (the top row starts
 * part-way into its cells), pixel panning, or more rows than its cache holds.
 * Prehistorik 2's crack intro uses the first three at once: a text screen
 * scrolling smoothly in both directions above line 129, and the HYBRID logo
 * with its wavy borders fixed below it, in the memory at address zero.  Only
 * the scroller ever appeared.
 *
 * Everything is redrawn on every refresh; this path is taken only while one
 * of those registers is in use.
 */
static void vga_text_lines(VGAState *s, const uint8_t *const font_base[2],
                           int cols, int cwidth, int cheight,
                           uint32_t chars_per_row, uint32_t start_addr,
                           SimpleFBDrawFunc *redraw_func, void *opaque)
{
    FBDevice *fb_dev = s->fb_dev;
    const uint8_t *vram = s->vga_ram;
    int lines = (s->cr[0x12] | ((s->cr[0x07] & 0x02) << 7) |
                 ((s->cr[0x07] & 0x40) << 3)) + 1;
    if (cols > MAX_TEXT_WIDTH) cols = MAX_TEXT_WIDTH;
    int w = cols * cwidth;
    if (w > fb_dev->width) w = fb_dev->width;
    if (lines > fb_dev->height) lines = fb_dev->height;
    const int x1 = (fb_dev->width - w) / 2, y1 = (fb_dev->height - lines) / 2;
    if (x1 != s->last_draw_x || y1 != s->last_draw_y ||
        w != s->last_draw_w || lines != s->last_draw_h) {
        memset(fb_dev->fb_data, 0, (size_t)fb_dev->height * fb_dev->stride);
        s->last_draw_x = x1; s->last_draw_y = y1;
        s->last_draw_w = w; s->last_draw_h = lines;
    }
    /* So that the cell renderer repaints everything when it takes over. */
    s->last_width = -1;

    const uint32_t line_compare = s->cr[0x18] | ((s->cr[0x07] & 0x10) << 4) |
                                  ((s->cr[0x09] & 0x40) << 3);
    const int dscan = s->cr[0x09] >> 7;
    /* Attribute register 13h: in nine-dot modes 8 means no shift and 0-7
     * shift one to eight dots; in eight-dot modes 0-7 are the shift. */
    const int p = s->ar[0x13] & 0x0f;
    int pan = cwidth == 9 ? (p >= 8 ? 0 : p + 1) : (p & 7);
    const uint32_t cursor_addr = (s->cr[0x0e] << 8) | s->cr[0x0f];
    const unsigned cstart = s->cr[0x0a], cend = s->cr[0x0b] & 0x1f;

    uint32_t row_addr = start_addr + ((s->cr[0x08] >> 5) & 3);
    int rs = s->cr[0x08] & 0x1f, dcount = 0, split = 0;
    static uint32_t buf[(MAX_TEXT_WIDTH + 1) * 9];

    for (int y = 0; y < lines; y++) {
        if (!split && (uint32_t)y > line_compare) {
            split = 1;
            row_addr = 0;
            rs = 0;
            dcount = 0;
            /* Pixel panning compatibility: the panel does not scroll. */
            if (s->ar[0x10] & 0x20) pan = 0;
        }
        uint32_t *o = buf;
        for (int cx = 0; cx <= cols; cx++) {    /* one more for the panning */
            const uint32_t a = ((row_addr + cx) * 4u) & 0x1fffcu;
            const unsigned ch = vram[a], at = vram[a + 1];
            unsigned bg = at >> 4;
            int blinks = 0;
            if (s->ar[0x10] & 0x08) {
                blinks = (bg & 8) != 0;
                bg &= 7;
            }
            const uint32_t bgc = s->last_palette[bg];
            uint32_t fgc = s->last_palette[at & 0x0f];
            if (blinks && !s->cursor_visible_phase) fgc = bgc;
            unsigned bits = rs < cheight ? font_base[(at >> 3) & 1][(32u * ch + rs) * 4u] : 0;
            if (row_addr + cx == cursor_addr && !(cstart & 0x20) &&
                s->cursor_visible_phase &&
                rs >= (int)(cstart & 0x1f) && rs <= (int)cend)
                bits = 0xff;
            for (int b = 7; b >= 0; b--)
                *o++ = ((bits >> b) & 1) ? fgc : bgc;
            if (cwidth == 9)
                *o++ = (ch >= 0xb0 && ch <= 0xdf && (s->ar[0x10] & 0x04) &&
                        (bits & 1)) ? fgc : bgc;
        }
        memcpy(fb_dev->fb_data + (size_t)(y1 + y) * fb_dev->stride + (size_t)x1 * 4,
               buf + pan, (size_t)w * 4);
        /* Scan doubling clocks the row scan counter every other line. */
        if (dscan && !dcount) {
            dcount = 1;
            continue;
        }
        dcount = 0;
        if (++rs >= cheight) {
            rs = 0;
            row_addr += chars_per_row;
        }
    }
    redraw_func(opaque, 0, 0, fb_dev->width, fb_dev->height);
}
#endif

/* the text refresh is just for debugging and initial boot message, so
   it is very incomplete */
static void vga_text_refresh(VGAState *s,
                             SimpleFBDrawFunc *redraw_func, void *opaque,
                             int full_update)
{
    FBDevice *fb_dev = s->fb_dev;
    int width, height, cwidth, cheight, cy, cx, x1, y1, width1, height1;
    int cx_min, cx_max, dup9;
    uint32_t ch_attr, line_offset, start_addr, ch_addr, ch_addr1, ch, cattr;
    uint8_t *vga_ram, *dst;
    const uint8_t *font_ptr;
    uint32_t fgcol, bgcol, cursor_offset, cursor_start, cursor_end;
    uint32_t now = get_uticks();
    if (after_eq(now, s->cursor_blink_time)) {
        s->cursor_blink_time = now + 133333;
        s->cursor_visible_phase = !s->cursor_visible_phase;
        /* Cells are normally redrawn only when their character or
         * attribute changes, and a blinking cell changes neither - so
         * without this the phase would flip and nothing would repaint.
         * Seven and a half times a second over a text screen is cheap. */
        full_update = 1;
    }

    full_update = full_update || update_palette16(s, s->last_palette);

    vga_ram = s->vga_ram;

    const uint8_t *font_base[2];
    uint32_t v = s->sr[0x3];
    font_base[0] = vga_ram + (((v >> 4) & 1) | ((v << 1) & 6)) * 8192 * 4 + 2;
    font_base[1] = vga_ram + (((v >> 5) & 1) | ((v >> 1) & 6)) * 8192 * 4 + 2;
    
    line_offset = s->cr[0x13];
    line_offset <<= 3;

    start_addr = s->cr[0x0d] | (s->cr[0x0c] << 8);
    
    cheight = (s->cr[9] & 0x1f) + 1;
    cwidth = 8;
    if (!s->force_8dm && !(s->sr[1] & 0x01))
        cwidth++;

    width = (s->cr[0x01] + 1);
    height = s->cr[0x12] |
        ((s->cr[0x07] & 0x02) << 7) |
        ((s->cr[0x07] & 0x40) << 3);
    height = (height + 1) / cheight;
    
    width1 = width * cwidth;
    height1 = height * cheight;
#if !defined(SCALE_3_2) && !defined(SWAPXY) && BPP == 32
    {
        const uint32_t lc = s->cr[0x18] | ((s->cr[0x07] & 0x10) << 4) |
                            ((s->cr[0x09] & 0x40) << 3);
        const int p = s->ar[0x13] & 0x0f;
        const int pan = cwidth == 9 ? (p >= 8 ? 0 : p + 1) : (p & 7);
        if ((s->cr[0x08] & 0x7f) || pan || lc + 1 < (uint32_t)height1 ||
            height > MAX_TEXT_HEIGHT || cheight > 16) {
            vga_text_lines(s, font_base, width, cwidth, cheight,
                           line_offset / 4, start_addr, redraw_func, opaque);
            return;
        }
    }
#endif
#if defined(SCALE_3_2) || defined(SWAPXY)
#ifdef SCALE_3_2
    if (fb_dev->width * 3 / 2 < width1 || fb_dev->height * 3 / 2 < height1 ||
        width > MAX_TEXT_WIDTH || height > MAX_TEXT_HEIGHT || cheight > 16)
        return; /* not enough space */
    x1 = (fb_dev->width * 3 / 2 - width1) / 3;
    y1 = (fb_dev->height * 3 / 2 - height1) / 3;
    full_update = 1;
#else
    if (fb_dev->width < width1 || fb_dev->height < height1 ||
        width > MAX_TEXT_WIDTH || height > MAX_TEXT_HEIGHT || cheight > 16)
        return; /* not enough space */
    x1 = (fb_dev->width - width1) / 2;
    y1 = (fb_dev->height - height1) / 2;
    full_update = 1;
#endif
#else
    if (fb_dev->width < width1 || fb_dev->height < height1 ||
        width > MAX_TEXT_WIDTH || height > MAX_TEXT_HEIGHT)
        return; /* not enough space */
    x1 = (fb_dev->width - width1) / 2;
    y1 = (fb_dev->height - height1) / 2;
    int stride = fb_dev->stride;
#endif
    if (s->last_line_offset != line_offset ||
        s->last_start_addr != start_addr ||
        s->last_width != width ||
        s->last_height != height) {
        s->last_line_offset = line_offset;
        s->last_start_addr = start_addr;
        s->last_width = width;
        s->last_height = height;
        full_update = 1;
    }
       
    /* update cursor position */
    cursor_offset = ((s->cr[0x0e] << 8) | s->cr[0x0f]) - start_addr;
    cursor_start = s->cr[0xa];
    cursor_end = s->cr[0xb];
    if (cursor_offset != s->last_cursor_offset ||
        cursor_start != s->last_cursor_start ||
        cursor_end != s->last_cursor_end) {
#ifndef FULL_UPDATE
        /* force refresh of characters with the cursor */
        if (s->last_cursor_offset < MAX_TEXT_WIDTH * MAX_TEXT_HEIGHT)
            s->last_ch_attr[s->last_cursor_offset] = -1;
        if (cursor_offset < MAX_TEXT_WIDTH * MAX_TEXT_HEIGHT)
            s->last_ch_attr[cursor_offset] = -1;
#endif
        s->last_cursor_offset = cursor_offset;
        s->last_cursor_start = cursor_start;
        s->last_cursor_end = cursor_end;
    }

    ch_addr1 = (start_addr * 4);
    cursor_offset = (start_addr + cursor_offset) * 4;
    
#if 0
    printf("text refresh %dx%d font=%dx%d start_addr=0x%x line_offset=0x%x\n",
           width, height, cwidth, cheight, start_addr, line_offset);
#endif
#if defined(SCALE_3_2) || defined(SWAPXY)
    int cb = 6;
    int nb = (width + cb - 1) / cb;
    int cxbegin = 0;
    int cxend = cb > width ? width : cb;
    int stride = (cxend - cxbegin) * cwidth * (BPP / 8);
    for (int b = 0; b < nb; b++)
    {
    int yt = 0;
    int yy = 0;
    ch_addr1 = (start_addr * 4) + cxbegin * 4;
#endif
    for(cy = 0; cy < height; cy++) {
        ch_addr = ch_addr1;
#if defined(SCALE_3_2) || defined(SWAPXY)
        dst = s->tmpbuf + yt * stride;
#else
        dst = fb_dev->fb_data + (y1 + cy * cheight) * stride + x1 * (BPP / 8);
#endif
        cx_min = width;
        cx_max = -1;
#if defined(SCALE_3_2) || defined(SWAPXY)
        for(cx = 0; cx < cxend - cxbegin; cx++) {
#else
        for(cx = 0; cx < width; cx++) {
#endif
            ch_attr = *(uint16_t *)(vga_ram + (ch_addr & 0x1fffe));
#ifdef FULL_UPDATE
            if (1) {
#else
            if (full_update || ch_attr != s->last_ch_attr[cy * width + cx] || cursor_offset == ch_addr) {
                s->last_ch_attr[cy * width + cx] = ch_attr;
#endif
                cx_min = cx_min > cx ? cx : cx_min;
                cx_max = cx_max < cx ? cx : cx_max;
                ch = ch_attr & 0xff;
                cattr = ch_attr >> 8;

                font_ptr = font_base[(cattr >> 3) & 1] + 32 * 4 * ch;
                /*
                 * Attribute bit 7 is the blink flag, not a fourth
                 * background colour bit.
                 *
                 * Taking the background as cattr >> 4 folded that bit
                 * into the palette index, so a blinking cell came out
                 * as the bright version of its background and never
                 * blinked: brown (6) became yellow (14), which is the
                 * yellow block seen over the selected entry in
                 * Prehistorik's setup screen.
                 *
                 * The bit only means blink while Attribute Controller
                 * register 10h bit 3 is set; with it clear, bit 7 really
                 * does select a bright background, so honour the mode
                 * rather than always masking.
                 */
                unsigned bg_index = cattr >> 4;
                int cell_blinks = 0;
                if (s->ar[0x10] & 0x08) {
                    cell_blinks = (bg_index & 8) != 0;
                    bg_index &= 7;
                }
                bgcol = s->last_palette[bg_index];
                fgcol = s->last_palette[cattr & 0x0f];
                /* Off phase: draw the glyph in its own background, so
                 * the cell keeps its colours and only the character
                 * disappears. */
                if (cell_blinks && !s->cursor_visible_phase) fgcol = bgcol;
                if (cwidth == 8) {
                    vga_draw_glyph8(dst, stride, font_ptr, cheight,
                                    fgcol, bgcol);
                } else {
                    dup9 = 0;
                    if (ch >= 0xb0 && ch <= 0xdf && (s->ar[0x10] & 0x04))
                        dup9 = 1;
                    vga_draw_glyph9(dst, stride, font_ptr, cheight,
                                    fgcol, bgcol, dup9);
                }
                /* cursor display */
                if (cursor_offset == ch_addr && !(cursor_start & 0x20) && s->cursor_visible_phase) {
                    int line_start, line_last, h;
                    uint8_t *dst1;
                    line_start = cursor_start & 0x1f;
                    line_last = cursor_end & 0x1f;

                    /* Handle invalid cursor shape - use underline as fallback */
                    if (line_last < line_start || line_start >= cheight) {
                        line_start = cheight > 2 ? cheight - 2 : 0;
                        line_last = cheight - 1;
                    }
                    if (line_last > cheight - 1)
                        line_last = cheight - 1;

                    h = line_last - line_start + 1;
                    dst1 = dst + stride * line_start;
                    if (cwidth == 8) {
                        vga_draw_glyph8(dst1, stride,
                                        cursor_glyph,
                                        h, fgcol, bgcol);
                    } else {
                        vga_draw_glyph9(dst1, stride,
                                        cursor_glyph,
                                        h, fgcol, bgcol, 1);
                    }
                }
            }
            ch_addr += 4;
            dst += (BPP / 8) * cwidth;
        }
#if defined(SCALE_3_2) || defined(SWAPXY)
        int k;
        for (k = 0; k < yt + cheight - 1; k += 3) {
#ifdef SCALE_3_2
#ifdef SWAPXY
                int ii0 = (BPP / 8) * ((y1 + yy) + (x1 + cxbegin * cwidth * 2 / 3) * fb_dev->height);
#else
                int ii0 = (BPP / 8) * ((y1 + yy) * fb_dev->width + x1 + cxbegin * cwidth * 2 / 3);
#endif
                scale_3_2(fb_dev->fb_data + ii0, fb_dev->stride,
                          s->tmpbuf + k * stride, stride / (BPP / 8));
                yy += 2;
#else
#ifdef SWAPXY
                int ii0 = (BPP / 8) * ((y1 + yy) + (x1 + cxbegin * cwidth) * fb_dev->height);
#else
                int ii0 = (BPP / 8) * ((y1 + yy) * fb_dev->width + x1 + cxbegin * cwidth);
#endif
                scale_3_3(fb_dev->fb_data + ii0, fb_dev->stride,
                          s->tmpbuf + k * stride, stride / (BPP / 8));
                yy += 3;
#endif
        }
        yt = k - (yt + cheight - 1);
        if (yt != 0) {
                yt = 3 - yt;
                memcpy(s->tmpbuf, s->tmpbuf + (k - 3) * stride, yt * stride);
        }
#endif
//        if (cx_max >= cx_min) {
//            redraw_func(opaque,
//                        x1 + cx_min * cwidth, y1 + cy * cheight,
//                        (cx_max - cx_min + 1) * cwidth, cheight);
//        }
        ch_addr1 += line_offset;
    }
#if defined(SCALE_3_2) || defined(SWAPXY)
    cxbegin += cb;
    cxend += cb;
    if (cxend > width) cxend = width;
    stride = (cxend - cxbegin) * cwidth * (BPP / 8);
    }
#endif
    redraw_func(opaque, 0, 0, fb_dev->width, fb_dev->height);
}

/*
 * Whether a row still holds what was drawn, a word at a time.
 *
 * Circle's memcmp() is a byte loop with a compare and a branch per byte;
 * over a 640x480 High Colour frame that alone was 2.8 ms, sixty times a
 * second, for a screen that had not changed.  Only equality matters here,
 * so eight bytes are compared at a time and the differences OR-ed together.
 */
static bool vbe_row_same(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i = 0;
    if ((((uintptr_t)a | (uintptr_t)b) & 7) == 0) {
        const uint64_t *pa = (const uint64_t *)a, *pb = (const uint64_t *)b;
        const size_t words = n / 8;
        size_t w = 0;
        for (; w + 4 <= words; w += 4) {
            if ((pa[w] ^ pb[w]) | (pa[w + 1] ^ pb[w + 1]) |
                (pa[w + 2] ^ pb[w + 2]) | (pa[w + 3] ^ pb[w + 3]))
                return false;
        }
        for (; w < words; w++)
            if (pa[w] != pb[w]) return false;
        i = words * 8;
    }
    for (; i < n; i++)
        if (a[i] != b[i]) return false;
    return true;
}

static void vga_graphic_refresh(VGAState *s,
                                SimpleFBDrawFunc *redraw_func, void *opaque,
                                int full_update)
{
    FBDevice *fb_dev = s->fb_dev;
    int w = (s->cr[0x01] + 1) * 8;
    int h = s->cr[0x12] |
        ((s->cr[0x07] & 0x02) << 7) |
        ((s->cr[0x07] & 0x40) << 3);
    h++;

    int shift_control = (s->gr[0x05] >> 5) & 3;
    int double_scan = (s->cr[0x09] >> 7);
    int multi_scan, multi_run;
    if (shift_control != 1) {
        multi_scan = (((s->cr[0x09] & 0x1f) + 1) << double_scan) - 1;
    } else {
        /* in CGA modes, multi_scan is ignored */
        /* XXX: is it correct ? */
        multi_scan = double_scan;
    }
    multi_run = multi_scan;

    uint32_t start_addr = s->cr[0x0d] | (s->cr[0x0c] << 8);
    uint32_t line_offset = s->cr[0x13];
    line_offset <<= 3;
    /*
     * The split screen.
     *
     * When the vertical counter passes the line compare value the display
     * starts fetching from address zero again, so the bottom of the screen
     * shows a fixed panel while everything above it scrolls.  Epic Pinball
     * uses it for the strip under the flippers; without it that strip showed
     * whatever happened to follow the scrolled table in memory, which is the
     * top of the table itself.
     */
    uint32_t line_compare = s->cr[0x18] |
        ((s->cr[0x07] & 0x10) << 4) |
        ((s->cr[0x09] & 0x40) << 3);
    if (vbe_enabled(s)) {
        line_offset = s->vbe_line_offset;
        start_addr = s->vbe_start_addr;
        line_compare = 0xffffffffu;
    }
    uint32_t addr1 = 4 * start_addr;
    uint8_t *vram = s->vga_ram;
    uint32_t palette[256];
    int xdiv = 1;
    int bpp = 4;
    if (shift_control == 0 || shift_control == 1) {
        update_palette16(s, palette);
        if (s->sr[0x01] & 8) {
            /*
             * Sequencer register 1 bit 3 halves the dot clock, so one CRTC
             * character time still covers eight source pixels but sixteen
             * dots on the display.  The CRTC's horizontal display end then
             * counts half the screen width, while w here is measured in
             * display dots.
             *
             * The doubling used to be applied to the CGA shift mode only, so
             * the planar 320x200 sixteen-colour mode 0Dh, whose sequencer
             * register 1 is 09h, drew w = 320 with xdiv = 2, that is source
             * pixels 0..159: the left half of every line, stretched two to
             * one, inside a 320 dot box in the middle of the surface.
             * Everything past the halfway point of the picture was never
             * drawn at all.
             *
             * The condition belongs to the divided dot clock rather than to
             * the shift mode, so it applies to both.  Modes that leave bit 3
             * clear - 10h and 12h at 640 dots - are untouched, and
             * shift_control 2 (mode 13h, Mode X) never reaches this branch:
             * it takes xdiv from the 256-colour path below, where the CRTC
             * already counts the full 640 dots.
             */
            xdiv = 2;
            w *= 2;
        }
    } else {
        if (!vbe_enabled(s)) {
            update_palette256(s, palette);
            xdiv = 2;
            bpp = 8;
        } else {
            bpp = s->vbe_regs[VBE_DISPI_INDEX_BPP];
            if (bpp == 8)
                update_palette256(s, palette);
        }
    }

    int y1 = 0;
    int i0 = 0;
#if defined(SCALE_3_2) || defined(SWAPXY)
#ifdef SCALE_3_2
    int hx = fb_dev->height * 3 / 2;
    int wx = fb_dev->width * 3 / 2;
    if (h < hx)
#ifdef SWAPXY
        i0 += (hx - h) / 3 * (BPP / 8);
#else
        i0 += (hx - h) / 3 * fb_dev->stride;
#endif
    else
        h = hx;
    if (w < wx)
#ifdef SWAPXY
        i0 += (wx - w) / 3 * fb_dev->stride;
#else
        i0 += (wx - w) / 3 * (BPP / 8);
#endif
    else
        w = wx;
#else
    int hx = fb_dev->height;
    int wx = fb_dev->width;
    if (h < hx)
        i0 += (hx - h) / 2 * (BPP / 8);
    else
        h = hx;
    if (w < wx)
        i0 += (wx - w) / 2 * fb_dev->stride;
    else
        w = wx;
#endif
    int yyt = 0;
    int yy = 0;
#else
    int hx = fb_dev->height;
    int wx = fb_dev->width;
    /* Where the picture sits inside the surface, kept so that only the part
     * of it that was drawn has to be pushed to the display. */
    int draw_x = 0, draw_y = 0;
    if (h < hx) {
        draw_y = (hx - h) / 2;
        i0 += draw_y * fb_dev->stride;
    } else {
        h = hx;
    }
    if (w < wx) {
        draw_x = (wx - w) / 2;
        i0 += draw_x * (BPP / 8);
    } else {
        w = wx;
    }

    /*
     * If the picture is smaller than it was, the difference is still on the
     * screen: the renderer writes only what it draws, so nothing clears the
     * rows and columns it has given up.  Clear once when the rectangle
     * changes and push the whole surface that time.
     */
    if (draw_x != s->last_draw_x || draw_y != s->last_draw_y ||
        w != s->last_draw_w || h != s->last_draw_h) {
        if (s->last_draw_w | s->last_draw_h) {
            memset(fb_dev->fb_data, 0,
                   (size_t)fb_dev->width * fb_dev->height * (BPP / 8));
            full_update = 1;
        }
        s->last_draw_x = draw_x;
        s->last_draw_y = draw_y;
        s->last_draw_w = w;
        s->last_draw_h = h;
    }

#endif
    /*
     * y is already the scan line, not the source row: h comes from the
     * vertical display end and multi_scan governs how often the source
     * address advances, not how often this loop runs.  Scaling it by the
     * scan doubling put the split at half its proper height, which showed
     * as two panes with a gap instead of a score strip at the bottom.
     */
    int split = 0;

#if !defined(SCALE_3_2) && !defined(SWAPXY) && BPP == 32
    /*
     * VBE rows: converted in one pass each, and only when they changed.
     *
     * A 640x480 High Colour desktop was converted in full sixty times a
     * second, one switch on the depth per pixel, and pushed to the display
     * in full: 7.3 ms a frame, 43 per cent of the machine, measured with
     * Windows 95 doing nothing more than opening its Start menu.  The VRAM
     * each row was drawn from is now kept, a row whose bytes are the same
     * is skipped, and only the band that changed is pushed.  Comparing is
     * far cheaper than converting, and it needs nothing from the paths that
     * write the VRAM - linear framebuffer, banks, strings or DMA.
     */
    const int vbe_bytes = bpp == 8 ? 1 : bpp == 15 || bpp == 16 ? 2 :
                          bpp == 32 ? 4 : 0;
    const int vbe_rows = vbe_enabled(s) && xdiv == 1 && vbe_bytes > 0;
    const size_t vbe_row_bytes = (size_t)w * (size_t)vbe_bytes;
    int vbe_y0 = h, vbe_y1 = -1;
    if (vbe_rows) {
        const size_t need = vbe_row_bytes * (size_t)h;
        if (need > s->vbe_shadow_cap) {
            free(s->vbe_shadow);
            s->vbe_shadow = malloc(need);
            s->vbe_shadow_cap = s->vbe_shadow ? need : 0;
            full_update = 1;
        }
        /* An 8-bit mode redraws everything when its palette changes. */
        if (bpp == 8 && s->vbe_shadow_palgen != s->palette_gen) {
            s->vbe_shadow_palgen = s->palette_gen;
            full_update = 1;
        }
        /*
         * Nothing written, nothing moved: nothing to compare.
         *
         * Even word at a time, looking at 600 KB to learn that a desktop
         * nobody touched is still the same costs about a millisecond a
         * frame.  Every path that stores into VRAM counts itself in
         * g_vram_writes, and the start address, pitch, depth and size are
         * what else could make the same memory show differently.
         */
        const uint32_t key[5] = { addr1, line_offset, (uint32_t)bpp,
                                  (uint32_t)w, (uint32_t)h };
        if (!full_update && s->vbe_shadow &&
            s->vbe_shadow_writes == g_vram_writes &&
            memcmp(key, s->vbe_shadow_key, sizeof key) == 0) {
            g_vga_refresh_skips++;
            return;
        }
        s->vbe_shadow_writes = g_vram_writes;
        memcpy(s->vbe_shadow_key, key, sizeof key);
    }
#endif

    for (int y = 0; y < h; y++) {
        if (!split && (uint32_t)y > line_compare) {
            split = 1;
            addr1 = 0;
            y1 = 0;
        }
        uint32_t addr = addr1;
        /*
         * CR17 bits 0 and 1 put the row scan counter on address lines 13 and
         * 14: the CGA's interleaved banks.  What reaches them is the row scan
         * counter - the line within the character row - and scan doubling
         * clocks that counter at half the line rate.
         *
         * In the CGA shift mode each y1 is a source line of a two-line row,
         * so y1 is that counter.  Everywhere else y1 counts whole character
         * rows, and using it made every other row come from the other bank.
         * Prehistorik 2 draws its level map in mode 0Dh with CR17 bit 0
         * clear, doubled lines and one line per row: the counter never
         * leaves zero on a real card, and here every second row showed the
         * empty bank above - a picture striped with black.
         */
        if ((s->cr[0x17] & 3) != 3) {
            const unsigned rs = shift_control == 1 ? (unsigned)y1 :
                (unsigned)(multi_scan - multi_run) >> double_scan;
            if (!(s->cr[0x17] & 1)) {
                const int shift = 14 + ((s->cr[0x17] >> 6) & 1);
                addr = (addr & ~(1u << shift)) | ((rs & 1) << shift);
            }
            if (!(s->cr[0x17] & 2)) {
                addr = (addr & ~0x8000u) | ((rs & 2) << 14);
            }
        }
#if !defined(SCALE_3_2) && !defined(SWAPXY) && BPP == 32
        if (vbe_rows && s->vbe_shadow &&
            (size_t)addr + vbe_row_bytes <= (size_t)s->vga_ram_size) {
            uint8_t *shadow = s->vbe_shadow + (size_t)y * vbe_row_bytes;
            const uint8_t *src = vram + addr;
            if (!full_update && vbe_row_same(src, shadow, vbe_row_bytes))
                goto row_done;
            memcpy(shadow, src, vbe_row_bytes);
            uint32_t *dst = (uint32_t *)(fb_dev->fb_data + i0 +
                                         (size_t)y * fb_dev->width * 4u);
            switch (bpp) {
            case 8:
                for (int x = 0; x < w; x++) dst[x] = palette[src[x]];
                break;
            case 15:
                for (int x = 0; x < w; x++) {
                    const uint32_t k = src[2 * x] | (src[2 * x + 1] << 8);
                    dst[x] = ((k & 0x1f) << 3) | (((k >> 5) & 0x1f) << 11) |
                             (((k >> 10) & 0x1f) << 19);
                }
                break;
            case 16:
                for (int x = 0; x < w; x++) {
                    const uint32_t k = src[2 * x] | (src[2 * x + 1] << 8);
                    dst[x] = ((k & 0x1f) << 3) | (((k >> 5) & 0x3f) << 10) |
                             (((k >> 11) & 0x1f) << 19);
                }
                break;
            default: /* 32 */
                memcpy(dst, src, vbe_row_bytes);
                break;
            }
            if (y < vbe_y0) vbe_y0 = y;
            vbe_y1 = y;
            goto row_done;
        }
#endif
#if !defined(SCALE_3_2) && !defined(SWAPXY) && BPP == 32
        /*
         * Planar sixteen-colour rows, eight source pixels at a time.
         *
         * The generic loop below reads the same four plane bytes once per
         * pixel, divides to find the source pixel and multiplies to find the
         * destination, which measured about forty host cycles a pixel - a
         * full 640x400 redraw took 8.6 ms.  That is not merely slow: the
         * renderer runs inside pc_step(), so for those 8.6 ms nothing polls
         * the timer, about ten IRQ0 ticks pile up, and the 8259 keeps one
         * request bit and loses the rest.  Measured on hardware, that single
         * pause accounted for 341 of the 1119 ticks a second the guest asked
         * for, and it is why timer-paced music ran slow while the game
         * itself, which paces on the retrace, looked fine.
         *
         * So this loads each plane group once and unpacks its eight pixels
         * with shifts, walks the destination with a pointer, and handles the
         * halved dot clock by storing the colour twice rather than dividing.
         * The output is identical to the generic path; only the arithmetic
         * per pixel changes.
         */
        /*
         * Mode 13h, and the one Tyrian spends its time in.
         *
         * The generic loop below divides by xdiv once per output pixel to
         * find the source byte, which for a 640-wide surface is 640 divisions
         * a line and 256000 a frame.  The renderer was taking 2467 ms of
         * every ten seconds on Tyrian's title screen - a quarter of the wall
         * clock - against 1270 ms for a planar game that had already been
         * given a fast path.
         *
         * Each source byte is one palette entry and covers xdiv pixels, so
         * walk the source once and store the colour that many times.
         */
        if (shift_control == 2 && bpp == 8) {
            const int src_pixels = w / xdiv;
            const uint8_t *src = vram + addr;
            uint32_t *dst = (uint32_t *)(fb_dev->fb_data + i0 +
                                         (BPP / 8) * (y * fb_dev->width));
            if (xdiv == 2) {
                for (int sx = 0; sx < src_pixels; ++sx) {
                    const uint32_t c = palette[src[sx]];
                    *dst++ = c;
                    *dst++ = c;
                }
            } else {
                for (int sx = 0; sx < src_pixels; ++sx)
                    *dst++ = palette[src[sx]];
            }
        } else if (shift_control == 0) {
            const int src_pixels = w / xdiv;
            uint32_t *dst = (uint32_t *)(fb_dev->fb_data + i0 +
                                         (BPP / 8) * (y * fb_dev->width));
            for (int sx = 0; sx < src_pixels; sx += 8) {
                const uint32_t base = addr + 4 * (uint32_t)(sx >> 3);
                const uint32_t p0 = vram[base + 0], p1 = vram[base + 1];
                const uint32_t p2 = vram[base + 2], p3 = vram[base + 3];
                int n = src_pixels - sx;
                if (n > 8) n = 8;
                for (int b = 0; b < n; ++b) {
                    const int sh = 7 - b;
                    const uint32_t k = ((p0 >> sh) & 1u) |
                                       (((p1 >> sh) & 1u) << 1) |
                                       (((p2 >> sh) & 1u) << 2) |
                                       (((p3 >> sh) & 1u) << 3);
                    const uint32_t c = palette[k];
                    *dst++ = c;
                    if (xdiv == 2) *dst++ = c;
                }
            }
        } else
#endif
        for (int x = 0; x < w; x++) {
            int x1 = x / xdiv;
            uint32_t color;
            if (shift_control == 0) {
#if 0
                static int ega_debug = 0;
                if (ega_debug < 3 && x == 0 && y == 0) {
                    printf("[EGA] w=%d h=%d xdiv=%d line_offset=%d addr1=0x%x\n",
                           w, h, xdiv, line_offset, addr1);
                    printf("[EGA] cr17=%02x cr09=%02x multi_scan=%d double_scan=%d\n",
                           s->cr[0x17], s->cr[0x09], multi_scan, double_scan);
                    printf("[EGA] vram[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                           vram[0], vram[1], vram[2], vram[3], vram[4], vram[5], vram[6], vram[7]);
                    ega_debug++;
                }
#endif
                int k = ((vram[addr + 4 * (x1 >> 3)] >> (7 - (x1 & 7))) & 1) << 0;
                k |= ((vram[addr + 4 * (x1 >> 3) + 1] >> (7 - (x1 & 7))) & 1) << 1;
                k |= ((vram[addr + 4 * (x1 >> 3) + 2] >> (7 - (x1 & 7))) & 1) << 2;
                k |= ((vram[addr + 4 * (x1 >> 3) + 3] >> (7 - (x1 & 7))) & 1) << 3;
                color = palette[k];
            } else if (shift_control == 1) {
                int k;
                /* Check if this is CGA 640x200 2-color mode (1bpp) vs 320x200 4-color (2bpp)
                 * Use original CRTC width to distinguish: 640 = 1bpp, 320 = 2bpp */
                int crtc_width = (s->cr[0x01] + 1) * 8;
#if 0
                static int cga_debug = 0;
                if (cga_debug < 5 && x == 0 && y == 0) {
                    printf("[CGA] shift_control=%d crtc_width=%d w=%d xdiv=%d cr17=%02x sr01=%02x\n",
                           shift_control, crtc_width, w, xdiv, s->cr[0x17], s->sr[0x01]);
                    cga_debug++;
                }
#endif
                if (crtc_width >= 640) {
                    /* CGA mode 6: 640x200, 1 bit per pixel, 8 pixels per byte
                     * All pixels in plane 0 only, so don't add plane offset */
                    k = ((vram[addr + 4 * (x1 >> 3)] >> (7 - (x1 & 7))) & 1);
                } else {
                    /* CGA mode 4/5: 320x200, 2 bits per pixel, 4 pixels per byte
                     * Pixels split across planes 0 and 1 */
                    k = ((vram[addr + 4 * (x1 >> 3) + ((x1 & 4) >> 2)] >>
                          (6 - 2 * (x1 & 3))) & 3);
                }
                color = palette[k];
            } else
#if BPP == 32
            {
                switch (bpp) {
                case 8: {
                    int k = vram[addr + x1];
                    color = palette[k];
                    break;
                }
                case 15: {
                    int k = vram[addr + 2 * x1] | (vram[addr + 2 * x1 + 1] << 8);
                    int b = (k & ((1 << 5) - 1)) << 3;
                    int g = ((k >> 5) & ((1 << 5) - 1)) << 3;
                    int r = ((k >> 10) & ((1 << 5) - 1)) << 3;
                    color = b | (g << 8) | (r << 16);
                    break;
                }
                case 16: {
                    int k = vram[addr + 2 * x1] | (vram[addr + 2 * x1 + 1] << 8);
                    int b = (k & ((1 << 5) - 1)) << 3;
                    int g = ((k >> 5) & ((1 << 6) - 1)) << 2;
                    int r = ((k >> 11) & ((1 << 5) - 1)) << 3;
                    color = b | (g << 8) | (r << 16);
                    break;
                }
                case 24: {
                    color = vram[addr + 3 * x1] |
                        (vram[addr + 3 * x1 + 1] << 8) |
                        (vram[addr + 3 * x1 + 2] << 16);
                    break;
                }
                case 32: {
                    color = vram[addr + 4 * x1] |
                        (vram[addr + 4 * x1 + 1] << 8) |
                        (vram[addr + 4 * x1 + 2] << 16) |
                        (vram[addr + 4 * x1 + 3] << 24);
                    break;
                }
                default:
                    /*
                     * An unexpected depth is a reason to draw black, not to
                     * kill the machine.  abort() here reaches _exit(), which
                     * is an infinite loop: the board shows a black screen,
                     * the guest is frozen mid-instruction, nothing is logged
                     * and no reset happens.  The identical mistake in
                     * i8254.c cost a long SWD hunt before Aladdin's real
                     * cause - a legal PIT mode - turned up behind it.
                     */
                    color = 0;
                    break;
                }
            }
            /*
             * One aligned 32-bit store, not four byte stores.  i is a
             * multiple of four because both the row stride and the
             * centring offset i0 are, so the destination is always
             * naturally aligned.  Four byte stores cost four bus
             * transactions per pixel, which dominated the profile on
             * the Circle target before its surface moved to cached
             * memory, and still cost three quarters of the stores
             * afterwards.
             */
            int i = (BPP / 8) * (y * fb_dev->width + x) + i0;
            *(uint32_t *)(fb_dev->fb_data + i) = color;
#elif BPP == 16
            {
                switch (bpp) {
                case 8: {
                    color = palette[vram[addr + x1]];
                    break;
                }
                case 15: {
                    int k = vram[addr + 2 * x1] | (vram[addr + 2 * x1 + 1] << 8);
                    color = (k & 0x1f) | ((k & ~0x1f) << 1);
                    break;
                }
                case 16: {
                    color = vram[addr + 2 * x1] | (vram[addr + 2 * x1 + 1] << 8);
                    break;
                }
                case 24: {
                    color = ((vram[addr + 3 * x1] >> 3)) |
                        ((vram[addr + 3 * x1 + 1] >> 2) << 5) |
                        ((vram[addr + 3 * x1 + 2] >> 3) << 11);
                    break;
                }
                case 32: {
                    color = ((vram[addr + 4 * x1] >> 3)) |
                        ((vram[addr + 4 * x1 + 1] >> 2) << 5) |
                        ((vram[addr + 4 * x1 + 2] >> 3) << 11);
                    break;
                }
                default:
                    /*
                     * An unexpected depth is a reason to draw black, not to
                     * kill the machine.  abort() here reaches _exit(), which
                     * is an infinite loop: the board shows a black screen,
                     * the guest is frozen mid-instruction, nothing is logged
                     * and no reset happens.  The identical mistake in
                     * i8254.c cost a long SWD hunt before Aladdin's real
                     * cause - a legal PIT mode - turned up behind it.
                     */
                    color = 0;
                    break;
                }
            }
#if defined(SCALE_3_2) || defined(SWAPXY)
            int i = (BPP / 8) * (yyt * w + x);
            s->tmpbuf[i + 0] = color;
            s->tmpbuf[i + 1] = color >> 8;
#else
            int i = (BPP / 8) * (y * fb_dev->width + x) + i0;
            fb_dev->fb_data[i + 0] = color;
            fb_dev->fb_data[i + 1] = color >> 8;
#endif
#else
#error "bad bpp"
#endif
        }
#if !defined(SCALE_3_2) && !defined(SWAPXY) && BPP == 32
    row_done:
#endif
        if (!multi_run) {
            /* A CGA row is two source lines; any other row is one step. */
            int mask = shift_control == 1 ? (s->cr[0x17] & 3) ^ 3 : 0;
            if ((y1 & mask) == mask)
                addr1 += line_offset;
            y1++;
            multi_run = multi_scan;
        } else {
            multi_run--;
        }
#if defined(SCALE_3_2) || defined(SWAPXY)
        yyt++;
        if (yyt == 3) {
#ifdef SWAPXY
            int ii0 = (BPP / 8) * yy + i0;
#else
            int ii0 = (BPP / 8) * (yy * fb_dev->width) + i0;
#endif
#ifdef SCALE_3_2
            scale_3_2(fb_dev->fb_data + ii0, fb_dev->stride, s->tmpbuf, w);
            yyt = 0;
            yy += 2;
#else
            scale_3_3(fb_dev->fb_data + ii0, fb_dev->stride, s->tmpbuf, w);
            yyt = 0;
            yy += 3;
#endif
        }
#endif
    }
    /*
     * Only the band that was drawn.
     *
     * A 320x200 mode doubled to 640x400 leaves forty rows of black above and
     * below, and the guest never writes them; copying them to the display
     * every frame was a sixth of a copy that already costs more than the
     * conversion feeding it.  A full update still pushes the whole surface,
     * which is what clears those bands when the mode changes or a menu
     * closes over them.
     */
#if !defined(SCALE_3_2) && !defined(SWAPXY) && BPP == 32
    if (vbe_rows && s->vbe_shadow && !full_update) {
        if (vbe_y1 >= vbe_y0)
            redraw_func(opaque, draw_x, draw_y + vbe_y0, w, vbe_y1 - vbe_y0 + 1);
        return;
    }
#endif
    if (full_update)
        redraw_func(opaque, 0, 0, fb_dev->width, fb_dev->height);
    else
        redraw_func(opaque, draw_x, draw_y, w, h);
}

static void simplefb_clear(FBDevice *fb_dev,
               SimpleFBDrawFunc *redraw_func, void *opaque)
{
    memset(fb_dev->fb_data, 0, fb_dev->width * fb_dev->height * (BPP / 8));
}

/* Update VGA retrace status based on timing.
 * This must be called frequently to ensure games polling 0x3DA see the
 * retrace bits toggle. Called from both vga_step() and vga_ioport_read(). */
/*
 * The caller passes the time in because the hot caller already has it.
 *
 * A game waiting for the frame to end polls 0x3DA, and one guest instruction
 * in six is that read - so what the read costs is what the emulator costs.
 * It used to take the clock twice, once here and once for the horizontal
 * blank below, and each of those is twenty-four cycles out of a read that
 * measured ninety-two.  One read serves both: no two parts of a single port
 * read need to disagree about what time it is.
 */
static int vga_update_retrace_at(VGAState *s, uint32_t now)
{
    int ret = 0;
    if (after_eq(now, s->retrace_time)) {
        /*
         * The next phase is due at a fixed interval from when this one was
         * due, not from the moment it was noticed.
         *
         * Scheduling from "now" made every phase at least as long as it
         * should be and usually longer, because the state is only looked at
         * when the guest reads the status register or pc_step comes round,
         * and during the fifteen milliseconds a game spends drawing it does
         * neither.  Each frame then carried its own lateness into the next,
         * and the display settled at about 53 Hz instead of 60 - slow, and
         * slow by an amount that moved with how busy the scene was.
         *
         * If it has fallen far behind - the guest was stopped in a menu, or
         * the machine was reset - start again from now rather than trying to
         * catch up through thousands of frames.
         */
        uint32_t base = s->retrace_time;
        if ((int32_t)(now - base) > 100000) base = now;
        if (s->retrace_phase == 0) {
            s->st01 |= ST01_DISP_ENABLE;
            s->retrace_phase = 1;
            s->retrace_time = base + 833;
        } else if (s->retrace_phase == 1) {
            s->st01 |= ST01_V_RETRACE;
            s->retrace_phase = 2;
            s->retrace_time = base + 833;
            s->retrace_pending = 1;
            ret = 1;
        } else {
            s->st01 &= ~(ST01_V_RETRACE | ST01_DISP_ENABLE);
            s->retrace_phase = 0;
            /* 833 + 833 + 15000 is 16.67 ms, which is 60 Hz. */
            s->retrace_time = base + 15000;
        }
    }
    return ret;
}

static int vga_update_retrace(VGAState *s)
{
    return vga_update_retrace_at(s, get_uticks());
}

int vga_step(VGAState *s)
{
    vga_update_retrace(s);
    const int pending = s->retrace_pending;
    s->retrace_pending = 0;
    return pending;
}

/* Host-side cost of one full software redraw, read once a second by the
 * Circle kernel.  The renderer walks every pixel of the visible area on
 * every retrace, so this is the first number to look at when the guest
 * runs slowly. */
uint32_t g_vga_refresh_us;
uint32_t g_vga_refresh_calls;
uint32_t g_vga_refresh_skips;
uint32_t g_vram_writes;

void vga_refresh(VGAState *s,
                 SimpleFBDrawFunc *redraw_func, void *opaque, int full_update)
{
    const uint32_t vga_refresh_t0 = get_uticks();
    FBDevice *fb_dev = s->fb_dev;
    int graphic_mode;
    if (!(s->ar_index & 0x20)) {
        /* blank */
        graphic_mode = 0;
    } else if (s->gr[0x06] & 1) {
        /* graphic mode */
        graphic_mode = 2;
    } else {
        /* text mode */
        graphic_mode = 1;
    }
#if 0
    static int last_mode = -1;
    if (graphic_mode != last_mode) {
        printf("VGA mode change: %d -> %d (ar_index=0x%02x gr6=0x%02x)\n",
               last_mode, graphic_mode, s->ar_index, s->gr[0x06]);
        last_mode = graphic_mode;
    }
#endif

    if (graphic_mode != s->graphic_mode) {
        s->graphic_mode = graphic_mode;
        /* A released region stays released until something says the mode has
         * changed; this is that something.  It only sets a flag. */
        full_update = 1;
        s->cursor_blink_time = get_uticks();
        simplefb_clear(fb_dev, redraw_func, opaque);
    }

    if (s->graphic_mode == 2) {
        vga_graphic_refresh(s, redraw_func, opaque, full_update);
    } else if (s->graphic_mode == 1) {
        vga_text_refresh(s, redraw_func, opaque, full_update);
    }
    g_vga_refresh_us += get_uticks() - vga_refresh_t0;
    g_vga_refresh_calls++;
}

/* force some bits to zero */
static const uint8_t sr_mask[8] = {
    (uint8_t)~0xfc,
    (uint8_t)~0xc2,
    (uint8_t)~0xf0,
    (uint8_t)~0xc0,
    (uint8_t)~0xf1,
    (uint8_t)~0xff,
    (uint8_t)~0xff,
    (uint8_t)~0x00,
};

static const uint8_t gr_mask[16] = {
    (uint8_t)~0xf0, /* 0x00 */
    (uint8_t)~0xf0, /* 0x01 */
    (uint8_t)~0xf0, /* 0x02 */
    (uint8_t)~0xe0, /* 0x03 */
    (uint8_t)~0xfc, /* 0x04 */
    (uint8_t)~0x84, /* 0x05 */
    (uint8_t)~0xf0, /* 0x06 */
    (uint8_t)~0xf0, /* 0x07 */
    (uint8_t)~0x00, /* 0x08 */
    (uint8_t)~0xff, /* 0x09 */
    (uint8_t)~0xff, /* 0x0a */
    (uint8_t)~0xff, /* 0x0b */
    (uint8_t)~0xff, /* 0x0c */
    (uint8_t)~0xff, /* 0x0d */
    (uint8_t)~0xff, /* 0x0e */
    (uint8_t)~0xff, /* 0x0f */
};

/*
 * The input status register, on its own and reachable without the port
 * dispatch.
 *
 * A game waiting for the frame to end polls this and nothing else: one
 * guest instruction in six on Tyrian is a read of 0x3DA.  Every one of them
 * walked the whole of _pc_io_read()'s switch over every port a PC has,
 * to reach a handler that tests for this register first anyway.
 */
uint32_t vga_status1_read(VGAState *s)
{
    int val;

        /* The vertical retrace bit comes from the VGA/HDMI ISR, which has the
         * real display timing, so Wolf3D polling this port still sees exact
         * hardware vblank timing.
         *
         * Bit 0 cannot come from there.  It is Display Enable NOT: on real
         * hardware it reads 1 during horizontal *and* vertical blanking and
         * therefore toggles once per scanline.  A per-frame ISR can only hold
         * it steady across the whole active area, and it held the opposite
         * polarity as well, so a game that counts scanlines by watching this
         * bit counted frames instead.  Lemmings does exactly that - sixty
         * iterations of "wait for active, wait for blanking" to find the line
         * its split screen starts on - which cost sixty frames per game frame
         * and made the game roughly five hundred times too slow.
         *
         * So the horizontal component is derived here from the display
         * ISR's own scanline clock.  It has to be that clock and not a
         * free-running one: Lemmings counts sixty scanlines from the end of
         * vertical retrace and reloads the palette there, for the split
         * between its level view and its status bar.  A phase unrelated to
         * the real scanout put that reload at a different screen position
         * every frame, which showed as coloured fragments flickering across
         * the level - the drawing was correct and provably static, and only
         * the palette moved. */
        /*
         * Advance the retrace state here, not only once per pc_step().
         *
         * A game waiting for the vertical retrace reads this port in a tight
         * loop, and what it is waiting for used to move only when pc_step()
         * came round - about every 640 microseconds, against phases of 833.
         * Each phase therefore took a whole number of those, so the frame
         * period came out near 17.9 ms rather than 16.67, and it moved with
         * however long pc_step() happened to take: a game pacing itself on
         * this ran slow, and by a different amount in different scenes.
         *
         * The read already has the time in hand, so the state machine can run
         * from here and the edges land where they should.
         */
        const uint32_t now = get_uticks_fast();
        vga_update_retrace_at(s, now);
        /*
         * Horizontal blanking, free-running.
         *
         * Two branches used to come first, for the case where an interrupt
         * service routine had left the time the current line stops blanking
         * in hblank_until.  Nothing has ever written that field - it is read
         * here and nowhere else - so both were dead, and they were dead on
         * the hottest path the emulator has.  A line is 31.8 us, which is
         * what the thirty-two below stands for.
         */
        const bool hblank = (now & 31u) < VGA_HBLANK_US;
        val = s->st01 & ~ST01_DISP_ENABLE;
        if ((val & ST01_V_RETRACE) || hblank)
            val |= ST01_DISP_ENABLE;
        s->ar_flip_flop = 0;
    return val;
}

uint32_t vga_ioport_read(VGAState *s, uint32_t addr)
{
    int val, index;

    /* Always handle status registers 0x3BA and 0x3DA regardless of color mode.
     * Some games (like Goblins) poll 0x3BA for vertical retrace even in color mode.
     * Update retrace status on each read so tight polling loops see changes. */
    if (addr == 0x3ba || addr == 0x3da) {
        val = vga_status1_read(s);
        goto done;
    }

    /* check port range access depending on color/monochrome mode */
    if ((addr >= 0x3b0 && addr <= 0x3bf && (s->msr & MSR_COLOR_EMULATION)) ||
        (addr >= 0x3d0 && addr <= 0x3df && !(s->msr & MSR_COLOR_EMULATION))) {
        val = 0xff;
    } else {
        switch(addr) {
        case 0x3c0:
            if (s->ar_flip_flop == 0) {
                val = s->ar_index;
            } else {
                val = 0;
            }
            break;
        case 0x3c1:
            index = s->ar_index & 0x1f;
            if (index < 21)
                val = s->ar[index];
            else
                val = 0;
            break;
        case 0x3c2:
            val = s->st00;
            break;
        case 0x3c4:
            val = s->sr_index;
            break;
        case 0x3c5:
            val = s->sr[s->sr_index];
#ifdef DEBUG_VGA_REG
            printf("vga: read SR%x = 0x%02x\n", s->sr_index, val);
#endif
            break;
        case 0x3c7:
            val = s->dac_state;
            break;
        case 0x3c8:
            val = s->dac_write_index;
            break;
        case 0x3c9:
            val = s->palette[s->dac_read_index * 3 + s->dac_sub_index];
            if (++s->dac_sub_index == 3) {
                s->dac_sub_index = 0;
                s->dac_read_index++;
            }
            break;
        case 0x3ca:
            val = s->fcr;
            break;
        case 0x3cc:
            val = s->msr;
            break;
        case 0x3ce:
            val = s->gr_index;
            break;
        case 0x3cf:
            val = s->gr[s->gr_index];
#ifdef DEBUG_VGA_REG
            printf("vga: read GR%x = 0x%02x\n", s->gr_index, val);
#endif
            break;
        case 0x3b4:
        case 0x3d4:
            val = s->cr_index;
            break;
        case 0x3b5:
        case 0x3d5:
            val = s->cr[s->cr_index];
#ifdef DEBUG_VGA_REG
            printf("vga: read CR%x = 0x%02x\n", s->cr_index, val);
#endif
            break;
        /* Note: 0x3ba and 0x3da are handled before the switch statement
         * to ensure they work regardless of color/monochrome mode */
        default:
            val = 0x00;
            break;
        }
    }
done:
#if defined(DEBUG_VGA)
    printf("VGA: read addr=0x%04x data=0x%02x\n", addr, val);
#endif
    return val;
}

void vga_ioport_write(VGAState *s, uint32_t addr, uint32_t val)
{
    int index;

    /* check port range access depending on color/monochrome mode */
    if ((addr >= 0x3b0 && addr <= 0x3bf && (s->msr & MSR_COLOR_EMULATION)) ||
        (addr >= 0x3d0 && addr <= 0x3df && !(s->msr & MSR_COLOR_EMULATION)))
        return;

#ifdef DEBUG_VGA
    printf("VGA: write addr=0x%04x data=0x%02x\n", addr, val);
#endif

    switch(addr) {
    case 0x3c0:
        if (s->ar_flip_flop == 0) {
            val &= 0x3f;
            s->ar_index = val;
        } else {
            index = s->ar_index & 0x1f;
            switch(index) {
            case 0x00 ... 0x0f:
                s->ar[index] = val & 0x3f;
                s->palette_dirty = 1; s->palette_gen++;  // Palette index mapping changed
                break;
            case 0x10:
                s->ar[index] = val & ~0x10;
                s->palette_dirty = 1; s->palette_gen++;  // Affects palette selection
                break;
            case 0x11:
                s->ar[index] = val;
                break;
            case 0x12:
                s->ar[index] = val & ~0xc0;
                break;
            case 0x13:
                s->ar[index] = val & ~0xf0;
                break;
            case 0x14:
                s->ar[index] = val & ~0xf0;
                s->palette_dirty = 1; s->palette_gen++;  // Affects palette color select
                break;
            default:
                break;
            }
        }
        s->ar_flip_flop ^= 1;
        break;
    case 0x3c2:
        s->msr = val & ~0x10;
        break;
    case 0x3c4:
        s->sr_index = val & 7;
        break;
    case 0x3c5:
#ifdef DEBUG_VGA_REG
        printf("vga: write SR%x = 0x%02x\n", s->sr_index, val);
#endif
        s->sr[s->sr_index] = val & sr_mask[s->sr_index];
        break;
    case 0x3c7:
        s->dac_read_index = val;
        s->dac_sub_index = 0;
        s->dac_state = 3;
        break;
    case 0x3c8:
        s->dac_write_index = val;
        s->dac_sub_index = 0;
        s->dac_state = 0;
        break;
    case 0x3c9:
        s->dac_cache[s->dac_sub_index] = val;
        if (++s->dac_sub_index == 3) {
            memcpy(&s->palette[s->dac_write_index * 3], s->dac_cache, 3);
            s->palette_dirty = 1; s->palette_gen++;
            s->dac_sub_index = 0;
            s->dac_write_index++;
        }
        break;
    case 0x3ce:
        s->gr_index = val & 0x0f;
        break;
    case 0x3cf:
#ifdef DEBUG_VGA_REG
        printf("vga: write GR%x = 0x%02x\n", s->gr_index, val);
#endif
        s->gr[s->gr_index] = val & gr_mask[s->gr_index];
        break;
    case 0x3b4:
    case 0x3d4:
        s->cr_index = val;
        break;
    case 0x3b5:
    case 0x3d5:
#ifdef DEBUG_VGA_REG
        printf("vga: write CR%x = 0x%02x\n", s->cr_index, val);
#endif
        /* handle CR0-7 protection */
        if ((s->cr[0x11] & 0x80) && s->cr_index <= 7) {
            /* can always write bit 4 of CR7 */
            if (s->cr_index == 7)
                s->cr[7] = (s->cr[7] & ~0x10) | (val & 0x10);
            return;
        }
        switch(s->cr_index) {
        case 0x01: /* horizontal display end */
        case 0x07:
        case 0x09:
        case 0x0c: /* start address high */
        case 0x0d: /* start address low */
        case 0x12: /* vertical display end */
        default:
            s->cr[s->cr_index] = val;
            break;
        }
        break;
    case 0x3ba:
    case 0x3da:
        s->fcr = val & 0x10;
        break;
    }
}

#define VGA_IO(base) \
static uint32_t vga_read_ ## base(void *opaque, uint32_t addr, int size_log2)\
{\
    return vga_ioport_read(opaque, base + addr);\
}\
static void vga_write_ ## base(void *opaque, uint32_t addr, uint32_t val, int size_log2)\
{\
    return vga_ioport_write(opaque, base + addr, val);\
}

void vbe_write(VGAState *s, uint32_t offset, uint32_t val)
{
    if (offset == 0) {
        s->vbe_index = val;
    } else {
#ifdef DEBUG_VBE
        printf("VBE write: index=0x%04x val=0x%04x\n", s->vbe_index, val);
#endif
        switch(s->vbe_index) {
        case VBE_DISPI_INDEX_ID:
            if (val >= VBE_DISPI_ID0 && val <= VBE_DISPI_ID5)
                s->vbe_regs[s->vbe_index] = val;
            break;
        case VBE_DISPI_INDEX_ENABLE:
            if ((val & VBE_DISPI_ENABLED) &&
                !(s->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_ENABLED)) {
                s->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] =
                    s->vbe_regs[VBE_DISPI_INDEX_XRES];
                s->vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] =
                    s->vbe_regs[VBE_DISPI_INDEX_YRES];
                s->vbe_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
                s->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;
            } else {
                s->bank_offset = 0;
            }
            s->dac_8bit = (val & VBE_DISPI_8BIT_DAC) > 0;
            s->vbe_regs[s->vbe_index] = val;
            vbe_fixup_regs(s);
            vbe_update_vgaregs(s);
            /* clear the screen */
            if (!(val & VBE_DISPI_NOCLEARMEM)) {
                g_vram_writes++;
                memset(s->vga_ram, 0,
                       s->vbe_regs[VBE_DISPI_INDEX_YRES] * s->vbe_line_offset);
            }
            break;
        case VBE_DISPI_INDEX_XRES:
        case VBE_DISPI_INDEX_YRES:
        case VBE_DISPI_INDEX_BPP:
        case VBE_DISPI_INDEX_VIRT_WIDTH:
        case VBE_DISPI_INDEX_VIRT_HEIGHT:
        case VBE_DISPI_INDEX_X_OFFSET:
        case VBE_DISPI_INDEX_Y_OFFSET:
            s->vbe_regs[s->vbe_index] = val;
            vbe_fixup_regs(s);
            vbe_update_vgaregs(s);
            break;
        case VBE_DISPI_INDEX_BANK:
            val &= (s->vga_ram_size >> 16) - 1;
            s->vbe_regs[s->vbe_index] = val;
            s->bank_offset = (val << 16);
            break;
        }
    }
}

uint32_t vbe_read(VGAState *s, uint32_t offset)
{
    uint32_t val;

    if (offset == 0) {
        val = s->vbe_index;
    } else {
        if (s->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_GETCAPS) {
            switch(s->vbe_index) {
            case VBE_DISPI_INDEX_XRES:
#ifdef SCALE_3_2
                val = s->fb_dev->width * 3 / 2;
#else
                val = s->fb_dev->width;
#endif
                break;
            case VBE_DISPI_INDEX_YRES:
#ifdef SCALE_3_2
                val = s->fb_dev->height * 3 / 2;
#else
                val = s->fb_dev->height;
#endif
                break;
            case VBE_DISPI_INDEX_BPP:
                val = 32;
                break;
            default:
                goto read_reg;
            }
        } else {
        read_reg:
            if (s->vbe_index < VBE_DISPI_INDEX_NB)
                val = s->vbe_regs[s->vbe_index];
            else
                val = 0;
        }
#ifdef DEBUG_VBE
        printf("VBE read: index=0x%04x val=0x%04x\n", s->vbe_index, val);
#endif
    }
    return val;
}

#define cbswap_32(__x) \
((uint32_t)( \
                (((uint32_t)(__x) & (uint32_t)0x000000ffUL) << 24) | \
                (((uint32_t)(__x) & (uint32_t)0x0000ff00UL) <<  8) | \
                (((uint32_t)(__x) & (uint32_t)0x00ff0000UL) >>  8) | \
                (((uint32_t)(__x) & (uint32_t)0xff000000UL) >> 24) ))

#ifdef HOST_WORDS_BIGENDIAN
#define PAT(x) cbswap_32(x)
#else
#define PAT(x) (x)
#endif

#ifdef HOST_WORDS_BIGENDIAN
#define GET_PLANE(data, p) (((data) >> (24 - (p) * 8)) & 0xff)
#else
#define GET_PLANE(data, p) (((data) >> ((p) * 8)) & 0xff)
#endif

static const uint32_t mask16[16] = {
    PAT(0x00000000),
    PAT(0x000000ff),
    PAT(0x0000ff00),
    PAT(0x0000ffff),
    PAT(0x00ff0000),
    PAT(0x00ff00ff),
    PAT(0x00ffff00),
    PAT(0x00ffffff),
    PAT(0xff000000),
    PAT(0xff0000ff),
    PAT(0xff00ff00),
    PAT(0xff00ffff),
    PAT(0xffff0000),
    PAT(0xffff00ff),
    PAT(0xffffff00),
    PAT(0xffffffff),
};

#define VGA_SEQ_RESET           0x00
#define VGA_SEQ_CLOCK_MODE      0x01
#define VGA_SEQ_PLANE_WRITE     0x02
#define VGA_SEQ_CHARACTER_MAP   0x03
#define VGA_SEQ_MEMORY_MODE     0x04

#define VGA_SR01_CHAR_CLK_8DOTS 0x01 /* bit 0: character clocks 8 dots wide are generated */
#define VGA_SR01_SCREEN_OFF     0x20 /* bit 5: Screen is off */
#define VGA_SR02_ALL_PLANES     0x0F /* bits 3-0: enable access to all planes */
#define VGA_SR04_EXT_MEM        0x02 /* bit 1: allows complete mem access to 256K */
#define VGA_SR04_SEQ_MODE       0x04 /* bit 2: directs system to use a sequential addressing mode */
#define VGA_SR04_CHN_4M         0x08 /* bit 3: selects modulo 4 addressing for CPU access to display memory */

#define VGA_GFX_SR_VALUE        0x00
#define VGA_GFX_SR_ENABLE       0x01
#define VGA_GFX_COMPARE_VALUE   0x02
#define VGA_GFX_DATA_ROTATE     0x03
#define VGA_GFX_PLANE_READ      0x04
#define VGA_GFX_MODE            0x05
#define VGA_GFX_MISC            0x06
#define VGA_GFX_COMPARE_MASK    0x07
#define VGA_GFX_BIT_MASK        0x08

//#define DEBUG_VGA_MEM
//#define TARGET_FMT_plx "%x"
void IRAM_ATTR vga_mem_write16(VGAState *s, uint32_t addr, uint16_t val16)
{
    g_vram_writes++;
    if (!(s->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M)) {
        vga_mem_write(s, addr, val16);
        vga_mem_write(s, addr + 1, val16 >> 8);
        return;
    }
    uint32_t val = val16;

    int memory_map_mode, plane, mask;

#ifdef DEBUG_VGA_MEM
    printf("vga: [0x" TARGET_FMT_plx "] = 0x%02x\n", addr, val);
#endif
    /* convert to VGA memory offset */
    memory_map_mode = (s->gr[VGA_GFX_MISC] >> 2) & 3;
    addr &= 0x1ffff;
    switch(memory_map_mode) {
    case 0:
        break;
    case 1:
        if (addr >= 0x10000)
            return;
        addr += s->bank_offset;
        break;
    case 2:
        addr -= 0x10000;
        if (addr >= 0x8000)
            return;
        break;
    default:
    case 3:
        addr -= 0x18000;
        if (addr >= 0x8000)
            return;
        break;
    }

        /* VBE banking puts bank_offset up to 0x30000 on top of a 64 KB
         * window, so even this unchecked chain-4 store can reach the
         * region lent to the JIT.  See njit_vga_arena.h. */
    /* chain 4 mode : simplest access */
    plane = addr & 3;
    mask = (1 << plane);
    if (s->sr[VGA_SEQ_PLANE_WRITE] & mask) {
        * (uint16_t *) &(s->vga_ram[addr]) = val;
    }
}

void IRAM_ATTR vga_mem_write32(VGAState *s, uint32_t addr, uint32_t val)
{
    g_vram_writes++;
    if (!(s->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M)) {
        vga_mem_write(s, addr, val);
        vga_mem_write(s, addr + 1, val >> 8);
        vga_mem_write(s, addr + 2, val >> 16);
        vga_mem_write(s, addr + 3, val >> 24);
        return;
    }

    int memory_map_mode, plane, mask;

#ifdef DEBUG_VGA_MEM
    printf("vga: [0x" TARGET_FMT_plx "] = 0x%02x\n", addr, val);
#endif
    /* convert to VGA memory offset */
    memory_map_mode = (s->gr[VGA_GFX_MISC] >> 2) & 3;
    addr &= 0x1ffff;
    switch(memory_map_mode) {
    case 0:
        break;
    case 1:
        if (addr >= 0x10000)
            return;
        addr += s->bank_offset;
        break;
    case 2:
        addr -= 0x10000;
        if (addr >= 0x8000)
            return;
        break;
    default:
    case 3:
        addr -= 0x18000;
        if (addr >= 0x8000)
            return;
        break;
    }

        /* VBE banking puts bank_offset up to 0x30000 on top of a 64 KB
         * window, so even this unchecked chain-4 store can reach the
         * region lent to the JIT.  See njit_vga_arena.h. */
    /* chain 4 mode : simplest access */
    plane = addr & 3;
    mask = (1 << plane);
    if (s->sr[VGA_SEQ_PLANE_WRITE] & mask) {
        * (uint32_t *) &(s->vga_ram[addr]) = val;
    }
}

bool IRAM_ATTR vga_mem_write_string(VGAState *s, uint32_t addr, uint8_t *buf, int len)
{
    g_vram_writes++;
    if (!(s->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M)) {
        return false;
    }

    int memory_map_mode, plane, mask;

#ifdef DEBUG_VGA_MEM
    printf("vga: [0x" TARGET_FMT_plx "] = 0x%02x\n", addr, val);
#endif
    /* convert to VGA memory offset */
    memory_map_mode = (s->gr[VGA_GFX_MISC] >> 2) & 3;
    addr &= 0x1ffff;
    switch(memory_map_mode) {
    case 0:
        break;
    case 1:
        if (addr >= 0x10000)
            return false;
        addr += s->bank_offset;
        break;
    case 2:
        addr -= 0x10000;
        if (addr >= 0x8000)
            return false;
        break;
    default:
    case 3:
        addr -= 0x18000;
        if (addr >= 0x8000)
            return false;
        break;
    }

    /* chain 4 mode : simplest access */
    plane = addr & 3;
    mask = (1 << plane);
    if (s->sr[VGA_SEQ_PLANE_WRITE] & mask) {
        memcpy(s->vga_ram + addr, buf, len);
        return true;
    }
    return false;
}

void IRAM_ATTR vga_mem_write(VGAState *s, uint32_t addr, uint8_t val8)
{
    g_vram_writes++;
    uint32_t val = val8;

    int memory_map_mode, plane, write_mode, b, func_select, mask;
    uint32_t write_mask, bit_mask, set_mask;

#ifdef DEBUG_VGA_MEM
    printf("vga: [0x" TARGET_FMT_plx "] = 0x%02x\n", addr, val);
#endif
    /* convert to VGA memory offset */
    memory_map_mode = (s->gr[VGA_GFX_MISC] >> 2) & 3;
    addr &= 0x1ffff;
    switch(memory_map_mode) {
    case 0:
        break;
    case 1:
        if (addr >= 0x10000)
            return;
        addr += s->bank_offset;
        break;
    case 2:
        addr -= 0x10000;
        if (addr >= 0x8000)
            return;
        break;
    default:
    case 3:
        addr -= 0x18000;
        if (addr >= 0x8000)
            return;
        break;
    }

    if (s->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M) {
        /* chain 4 mode : simplest access */
        plane = addr & 3;
        mask = (1 << plane);
        if (s->sr[VGA_SEQ_PLANE_WRITE] & mask) {
            s->vga_ram[addr] = val;
#ifdef DEBUG_VGA_MEM
            printf("vga: chain4: [0x" TARGET_FMT_plx "]\n", addr);
#endif
//            s->plane_updated |= mask; /* only used to detect font change */
//            memory_region_set_dirty(&s->vram, addr, 1);
        }
    /*
     * Odd/even (text) mapping.
     *
     * The test used to be gr[5] bit 4 alone.  That is the Graphics Mode
     * register's odd/even bit, and it is not the one that decides how a CPU
     * address reaches the planes: gr[6] bit 1, Chain Odd/Even, is what makes
     * address bit 0 select the plane.  Software may leave gr[5] clear while
     * gr[6] still says chain odd/even, and then this fell through to the
     * planar branch below, which stores one *dword* per guest byte - so a
     * character and its attribute landed in two adjacent cells instead of
     * one.
     *
     * Commander Keen 4's loader does exactly that.  Everything DOS had
     * already written stayed correct, and everything the loader wrote after
     * it cleared gr[5] came out with the characters two cells apart:
     * "S_V_G_A_ _C_o_m_p...", and the memory figures it patches into its own
     * template never replaced the "xxxxx" placeholders.  Measured on the
     * board at that moment: gr[5]=0x00, gr[6]=0x0e, sr[4]=0x02.
     */
    } else if ((s->gr[VGA_GFX_MODE] & 0x10) || (s->gr[VGA_GFX_MISC] & 0x02)) {
        /* odd/even mode (aka text mode mapping) */
        plane = (s->gr[VGA_GFX_PLANE_READ] & 2) | (addr & 1);
        mask = (1 << plane);
        if (s->sr[VGA_SEQ_PLANE_WRITE] & mask) {
            addr = ((addr & ~1) << 1) | plane;
            if (addr >= s->vga_ram_size) {
                return;
            }
            s->vga_ram[addr] = val;
#ifdef DEBUG_VGA_MEM
            printf("vga: odd/even: [0x" TARGET_FMT_plx "]\n", addr);
#endif
//            s->plane_updated |= mask; /* only used to detect font change */
//            memory_region_set_dirty(&s->vram, addr, 1);
        }
    } else {
        /* standard VGA latched access */
        write_mode = s->gr[VGA_GFX_MODE] & 3;
        switch(write_mode) {
        default:
        case 0:
            /* rotate */
            b = s->gr[VGA_GFX_DATA_ROTATE] & 7;
            val = ((val >> b) | (val << (8 - b))) & 0xff;
            val |= val << 8;
            val |= val << 16;

            /* apply set/reset mask */
            set_mask = mask16[s->gr[VGA_GFX_SR_ENABLE]];
            val = (val & ~set_mask) |
                (mask16[s->gr[VGA_GFX_SR_VALUE]] & set_mask);
            bit_mask = s->gr[VGA_GFX_BIT_MASK];
            break;
        case 1:
            val = s->latch;
            goto do_write;
        case 2:
            val = mask16[val & 0x0f];
            bit_mask = s->gr[VGA_GFX_BIT_MASK];
            break;
        case 3:
            /* rotate */
            b = s->gr[VGA_GFX_DATA_ROTATE] & 7;
            val = (val >> b) | (val << (8 - b));

            bit_mask = s->gr[VGA_GFX_BIT_MASK] & val;
            val = mask16[s->gr[VGA_GFX_SR_VALUE]];
            break;
        }

        /* apply logical operation */
        func_select = s->gr[VGA_GFX_DATA_ROTATE] >> 3;
        switch(func_select) {
        case 0:
        default:
            /* nothing to do */
            break;
        case 1:
            /* and */
            val &= s->latch;
            break;
        case 2:
            /* or */
            val |= s->latch;
            break;
        case 3:
            /* xor */
            val ^= s->latch;
            break;
        }

        /* apply bit mask */
        bit_mask |= bit_mask << 8;
        bit_mask |= bit_mask << 16;
        val = (val & bit_mask) | (s->latch & ~bit_mask);

    do_write:
        /* mask data according to sr[2] */
        mask = s->sr[VGA_SEQ_PLANE_WRITE];
//        s->plane_updated |= mask; /* only used to detect font change */
        write_mask = mask16[mask];
        /* The only path in this file that can reach the JIT's borrowed region
         * at the top of the buffer; chain 4 and odd/even are both bounded
         * below byte 131072.  See njit_vga_arena.h. */
        if (addr * sizeof(uint32_t) >= s->vga_ram_size) {
            return;
        }
        ((uint32_t *)s->vga_ram)[addr] =
            (((uint32_t *)s->vga_ram)[addr] & ~write_mask) |
            (val & write_mask);
#ifdef DEBUG_VGA_MEM
        printf("vga: latch: [0x" TARGET_FMT_plx "] mask=0x%08x val=0x%08x\n",
               addr * 4, write_mask, val);
#endif
//        memory_region_set_dirty(&s->vram, addr << 2, sizeof(uint32_t));
    }
}

uint8_t vga_mem_read(VGAState *s, uint32_t addr)
{
    int memory_map_mode, plane;
    uint32_t ret;

    /* convert to VGA memory offset */
    memory_map_mode = (s->gr[VGA_GFX_MISC] >> 2) & 3;
    addr &= 0x1ffff;
    switch(memory_map_mode) {
    case 0:
        break;
    case 1:
        if (addr >= 0x10000)
            return 0xff;
        addr += s->bank_offset;
        break;
    case 2:
        addr -= 0x10000;
        if (addr >= 0x8000)
            return 0xff;
        break;
    default:
    case 3:
        addr -= 0x18000;
        if (addr >= 0x8000)
            return 0xff;
        break;
    }

    if (s->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M) {
        /* chain 4 mode : simplest access */
//        assert(addr < s->vram_size);
        ret = s->vga_ram[addr];
    } else if ((s->gr[VGA_GFX_MODE] & 0x10) || (s->gr[VGA_GFX_MISC] & 0x02)) {
        /* odd/even mode; the read path has to agree with the write path
         * above, or a read-modify-write scrambles the screen. */
        plane = (s->gr[VGA_GFX_PLANE_READ] & 2) | (addr & 1);
        addr = ((addr & ~1) << 1) | plane;
        if (addr >= s->vga_ram_size) { // s->vram_size) {
            return 0xff;
        }
        ret = s->vga_ram[addr];
    } else {
        /* standard VGA latched access */
        if (addr * sizeof(uint32_t) >= s->vga_ram_size) {//s->vram_size) {
            return 0xff;
        }
        s->latch = ((uint32_t *)s->vga_ram)[addr];

        if (!(s->gr[VGA_GFX_MODE] & 0x08)) {
            /* read mode 0 */
            plane = s->gr[VGA_GFX_PLANE_READ];
            ret = GET_PLANE(s->latch, plane);
        } else {
            /* read mode 1 */
            ret = (s->latch ^ mask16[s->gr[VGA_GFX_COMPARE_VALUE]]) &
                mask16[s->gr[VGA_GFX_COMPARE_MASK]];
            ret |= ret >> 16;
            ret |= ret >> 8;
            ret = (~ret) & 0xff;
        }
    }
    return ret;
}

static void vga_initmode(VGAState *s);

VGAState *vga_init(char *vga_ram, int vga_ram_size, int vga_ram_capacity,
                   uint8_t *fb, int width, int height)
{
    VGAState *s;

    s = pcmalloc(sizeof(*s));
    memset(s, 0, sizeof(*s));
    FBDevice *fb_dev = pcmalloc(sizeof(FBDevice));
    s->fb_dev = fb_dev;
    memset(s->fb_dev, 0, sizeof(FBDevice));
    s->graphic_mode = 0;
    s->cursor_blink_time = get_uticks();
    s->cursor_visible_phase = 1;
    s->retrace_time = get_uticks();
    s->retrace_phase = 0;
    fb_dev->width = width;
    fb_dev->height = height;
#ifdef SWAPXY
    fb_dev->stride = height * (BPP / 8);
#else
    fb_dev->stride = width * (BPP / 8);
#endif
    fb_dev->fb_data = fb;

    s->vga_ram = (uint8_t *) vga_ram;
    s->vga_ram_size = vga_ram_size;
    s->vga_ram_capacity = vga_ram_capacity < vga_ram_size ? vga_ram_size
                                                          : vga_ram_capacity;

    s->vbe_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID5;
    s->vbe_regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] = s->vga_ram_size >> 16;

    vga_initmode(s);
    return s;
}

void vga_set_ram_size(VGAState *s, int vga_ram_size)
{
    if (!s || vga_ram_size > s->vga_ram_capacity) return;
    s->vga_ram_size = vga_ram_size;
    s->vbe_regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] = vga_ram_size >> 16;
}

void vga_set_force_8dm(VGAState *s, int v)
{
    s->force_8dm = v;
}

PCIDevice *vga_pci_init(VGAState *s, PCIBus *bus,
                        void *o, void (*set_bar)(void *, int, uint32_t, bool))
{
    PCIDevice *d;
    d = pci_register_device(bus, "VGA", -1, 0x1234, 0x1111, 0x00, 0x0300);

    /* Sized from the buffer rather than from the size in force, because the
     * menu can raise that afterwards and the aperture cannot be re-registered
     * under a guest that has already mapped it. */
    uint32_t bar_size;
    bar_size = 1;
    while (bar_size < (uint32_t)s->vga_ram_capacity)
        bar_size <<= 1;
    pci_register_bar(d, 0, bar_size, PCI_ADDRESS_SPACE_MEM, o, set_bar);
    return d;
}

// from vgabios
// stdvga mode 2
const static uint8_t pal_ega[] = {
    0x00,0x00,0x00, 0x00,0x00,0x2a, 0x00,0x2a,0x00, 0x00,0x2a,0x2a,
    0x2a,0x00,0x00, 0x2a,0x00,0x2a, 0x2a,0x2a,0x00, 0x2a,0x2a,0x2a,
    0x00,0x00,0x15, 0x00,0x00,0x3f, 0x00,0x2a,0x15, 0x00,0x2a,0x3f,
    0x2a,0x00,0x15, 0x2a,0x00,0x3f, 0x2a,0x2a,0x15, 0x2a,0x2a,0x3f,
    0x00,0x15,0x00, 0x00,0x15,0x2a, 0x00,0x3f,0x00, 0x00,0x3f,0x2a,
    0x2a,0x15,0x00, 0x2a,0x15,0x2a, 0x2a,0x3f,0x00, 0x2a,0x3f,0x2a,
    0x00,0x15,0x15, 0x00,0x15,0x3f, 0x00,0x3f,0x15, 0x00,0x3f,0x3f,
    0x2a,0x15,0x15, 0x2a,0x15,0x3f, 0x2a,0x3f,0x15, 0x2a,0x3f,0x3f,
    0x15,0x00,0x00, 0x15,0x00,0x2a, 0x15,0x2a,0x00, 0x15,0x2a,0x2a,
    0x3f,0x00,0x00, 0x3f,0x00,0x2a, 0x3f,0x2a,0x00, 0x3f,0x2a,0x2a,
    0x15,0x00,0x15, 0x15,0x00,0x3f, 0x15,0x2a,0x15, 0x15,0x2a,0x3f,
    0x3f,0x00,0x15, 0x3f,0x00,0x3f, 0x3f,0x2a,0x15, 0x3f,0x2a,0x3f,
    0x15,0x15,0x00, 0x15,0x15,0x2a, 0x15,0x3f,0x00, 0x15,0x3f,0x2a,
    0x3f,0x15,0x00, 0x3f,0x15,0x2a, 0x3f,0x3f,0x00, 0x3f,0x3f,0x2a,
    0x15,0x15,0x15, 0x15,0x15,0x3f, 0x15,0x3f,0x15, 0x15,0x3f,0x3f,
    0x3f,0x15,0x15, 0x3f,0x15,0x3f, 0x3f,0x3f,0x15, 0x3f,0x3f,0x3f
};

const static uint8_t actl[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x0c, 0x00, 0x0f, 0x08 };

const static uint8_t sequ[] = { 0x00, 0x03, 0x00, 0x02 };

const static uint8_t grdc[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0e, 0x0f, 0xff };

const static uint8_t crtc[] = {
    0x5f, 0x4f, 0x50, 0x82, 0x55, 0x81, 0xbf, 0x1f,
    0x00, 0x4f, 0x0d, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x9c, 0x8e, 0x8f, 0x28, 0x1f, 0x96, 0xb9, 0xa3,
    0xff };

const static uint8_t vgafont16[256 * 16] = {
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7e, 0x81, 0xa5, 0x81, 0x81, 0xbd, 0x99, 0x81, 0x81, 0x7e, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7e, 0xff, 0xdb, 0xff, 0xff, 0xc3, 0xe7, 0xff, 0xff, 0x7e, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x6c, 0xfe, 0xfe, 0xfe, 0xfe, 0x7c, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x7c, 0xfe, 0x7c, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x18, 0x3c, 0x3c, 0xe7, 0xe7, 0xe7, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x18, 0x3c, 0x7e, 0xff, 0xff, 0x7e, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x3c, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xe7, 0xc3, 0xc3, 0xe7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x66, 0x42, 0x42, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00,
 0xff, 0xff, 0xff, 0xff, 0xff, 0xc3, 0x99, 0xbd, 0xbd, 0x99, 0xc3, 0xff, 0xff, 0xff, 0xff, 0xff,
 0x00, 0x00, 0x1e, 0x0e, 0x1a, 0x32, 0x78, 0xcc, 0xcc, 0xcc, 0xcc, 0x78, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3c, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3f, 0x33, 0x3f, 0x30, 0x30, 0x30, 0x30, 0x70, 0xf0, 0xe0, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7f, 0x63, 0x7f, 0x63, 0x63, 0x63, 0x63, 0x67, 0xe7, 0xe6, 0xc0, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x18, 0x18, 0xdb, 0x3c, 0xe7, 0x3c, 0xdb, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfe, 0xf8, 0xf0, 0xe0, 0xc0, 0x80, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x02, 0x06, 0x0e, 0x1e, 0x3e, 0xfe, 0x3e, 0x1e, 0x0e, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x18, 0x3c, 0x7e, 0x18, 0x18, 0x18, 0x7e, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7f, 0xdb, 0xdb, 0xdb, 0x7b, 0x1b, 0x1b, 0x1b, 0x1b, 0x1b, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x7c, 0xc6, 0x60, 0x38, 0x6c, 0xc6, 0xc6, 0x6c, 0x38, 0x0c, 0xc6, 0x7c, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xfe, 0xfe, 0xfe, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x18, 0x3c, 0x7e, 0x18, 0x18, 0x18, 0x7e, 0x3c, 0x18, 0x7e, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x18, 0x3c, 0x7e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x0c, 0xfe, 0x0c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x60, 0xfe, 0x60, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xc0, 0xc0, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x66, 0xff, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x38, 0x7c, 0x7c, 0xfe, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0xfe, 0xfe, 0x7c, 0x7c, 0x38, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x18, 0x3c, 0x3c, 0x3c, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x66, 0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x6c, 0x6c, 0xfe, 0x6c, 0x6c, 0x6c, 0xfe, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00,
 0x18, 0x18, 0x7c, 0xc6, 0xc2, 0xc0, 0x7c, 0x06, 0x06, 0x86, 0xc6, 0x7c, 0x18, 0x18, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0xc2, 0xc6, 0x0c, 0x18, 0x30, 0x60, 0xc6, 0x86, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x38, 0x6c, 0x6c, 0x38, 0x76, 0xdc, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x30, 0x30, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x0c, 0x18, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x18, 0x0c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x3c, 0xff, 0x3c, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x30, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x02, 0x06, 0x0c, 0x18, 0x30, 0x60, 0xc0, 0x80, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3c, 0x66, 0xc3, 0xc3, 0xdb, 0xdb, 0xc3, 0xc3, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x18, 0x38, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7c, 0xc6, 0x06, 0x0c, 0x18, 0x30, 0x60, 0xc0, 0xc6, 0xfe, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7c, 0xc6, 0x06, 0x06, 0x3c, 0x06, 0x06, 0x06, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x0c, 0x1c, 0x3c, 0x6c, 0xcc, 0xfe, 0x0c, 0x0c, 0x0c, 0x1e, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xfe, 0xc0, 0xc0, 0xc0, 0xfc, 0x06, 0x06, 0x06, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x38, 0x60, 0xc0, 0xc0, 0xfc, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xfe, 0xc6, 0x06, 0x06, 0x0c, 0x18, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0x7e, 0x06, 0x06, 0x06, 0x0c, 0x78, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0c, 0x06, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x60, 0x30, 0x18, 0x0c, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0x0c, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xde, 0xde, 0xde, 0xdc, 0xc0, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x10, 0x38, 0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xfc, 0x66, 0x66, 0x66, 0x7c, 0x66, 0x66, 0x66, 0x66, 0xfc, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3c, 0x66, 0xc2, 0xc0, 0xc0, 0xc0, 0xc0, 0xc2, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xf8, 0x6c, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x6c, 0xf8, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xfe, 0x66, 0x62, 0x68, 0x78, 0x68, 0x60, 0x62, 0x66, 0xfe, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xfe, 0x66, 0x62, 0x68, 0x78, 0x68, 0x60, 0x60, 0x60, 0xf0, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3c, 0x66, 0xc2, 0xc0, 0xc0, 0xde, 0xc6, 0xc6, 0x66, 0x3a, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc6, 0xc6, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3c, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x1e, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0xcc, 0xcc, 0xcc, 0x78, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xe6, 0x66, 0x66, 0x6c, 0x78, 0x78, 0x6c, 0x66, 0x66, 0xe6, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xf0, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x62, 0x66, 0xfe, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc3, 0xe7, 0xff, 0xff, 0xdb, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc6, 0xe6, 0xf6, 0xfe, 0xde, 0xce, 0xc6, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xfc, 0x66, 0x66, 0x66, 0x7c, 0x60, 0x60, 0x60, 0x60, 0xf0, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xd6, 0xde, 0x7c, 0x0c, 0x0e, 0x00, 0x00,
 0x00, 0x00, 0xfc, 0x66, 0x66, 0x66, 0x7c, 0x6c, 0x66, 0x66, 0x66, 0xe6, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0x60, 0x38, 0x0c, 0x06, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xff, 0xdb, 0x99, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0x66, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xdb, 0xdb, 0xff, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc3, 0xc3, 0x66, 0x3c, 0x18, 0x18, 0x3c, 0x66, 0xc3, 0xc3, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc3, 0xc3, 0xc3, 0x66, 0x3c, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xff, 0xc3, 0x86, 0x0c, 0x18, 0x30, 0x60, 0xc1, 0xc3, 0xff, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3c, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x80, 0xc0, 0xe0, 0x70, 0x38, 0x1c, 0x0e, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x10, 0x38, 0x6c, 0xc6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
 0x30, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xe0, 0x60, 0x60, 0x78, 0x6c, 0x66, 0x66, 0x66, 0x66, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xc0, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x1c, 0x0c, 0x0c, 0x3c, 0x6c, 0xcc, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xfe, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x38, 0x6c, 0x64, 0x60, 0xf0, 0x60, 0x60, 0x60, 0x60, 0xf0, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x7c, 0x0c, 0xcc, 0x78, 0x00,
 0x00, 0x00, 0xe0, 0x60, 0x60, 0x6c, 0x76, 0x66, 0x66, 0x66, 0x66, 0xe6, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x18, 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x06, 0x06, 0x00, 0x0e, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x66, 0x66, 0x3c, 0x00,
 0x00, 0x00, 0xe0, 0x60, 0x60, 0x66, 0x6c, 0x78, 0x78, 0x6c, 0x66, 0xe6, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xe6, 0xff, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7c, 0x60, 0x60, 0xf0, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x7c, 0x0c, 0x0c, 0x1e, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc, 0x76, 0x66, 0x60, 0x60, 0x60, 0xf0, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0x60, 0x38, 0x0c, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x10, 0x30, 0x30, 0xfc, 0x30, 0x30, 0x30, 0x30, 0x36, 0x1c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0xc3, 0xc3, 0xc3, 0x66, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0xc3, 0xc3, 0xdb, 0xdb, 0xff, 0x66, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0x66, 0x3c, 0x18, 0x3c, 0x66, 0xc3, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7e, 0x06, 0x0c, 0xf8, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xcc, 0x18, 0x30, 0x60, 0xc6, 0xfe, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x0e, 0x18, 0x18, 0x18, 0x70, 0x18, 0x18, 0x18, 0x18, 0x0e, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x70, 0x18, 0x18, 0x18, 0x0e, 0x18, 0x18, 0x18, 0x18, 0x70, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x76, 0xdc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x6c, 0xc6, 0xc6, 0xc6, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3c, 0x66, 0xc2, 0xc0, 0xc0, 0xc0, 0xc2, 0x66, 0x3c, 0x0c, 0x06, 0x7c, 0x00, 0x00,
 0x00, 0x00, 0xcc, 0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x0c, 0x18, 0x30, 0x00, 0x7c, 0xc6, 0xfe, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x10, 0x38, 0x6c, 0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xcc, 0x00, 0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x60, 0x30, 0x18, 0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x38, 0x6c, 0x38, 0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x3c, 0x66, 0x60, 0x60, 0x66, 0x3c, 0x0c, 0x06, 0x3c, 0x00, 0x00, 0x00,
 0x00, 0x10, 0x38, 0x6c, 0x00, 0x7c, 0xc6, 0xfe, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc6, 0x00, 0x00, 0x7c, 0xc6, 0xfe, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x60, 0x30, 0x18, 0x00, 0x7c, 0xc6, 0xfe, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x66, 0x00, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x18, 0x3c, 0x66, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x60, 0x30, 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xc6, 0x00, 0x10, 0x38, 0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
 0x38, 0x6c, 0x38, 0x00, 0x38, 0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
 0x18, 0x30, 0x60, 0x00, 0xfe, 0x66, 0x60, 0x7c, 0x60, 0x60, 0x66, 0xfe, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x6e, 0x3b, 0x1b, 0x7e, 0xd8, 0xdc, 0x77, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x3e, 0x6c, 0xcc, 0xcc, 0xfe, 0xcc, 0xcc, 0xcc, 0xcc, 0xce, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x10, 0x38, 0x6c, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc6, 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x60, 0x30, 0x18, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x30, 0x78, 0xcc, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x60, 0x30, 0x18, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc6, 0x00, 0x00, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7e, 0x06, 0x0c, 0x78, 0x00,
 0x00, 0xc6, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xc6, 0x00, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x18, 0x18, 0x7e, 0xc3, 0xc0, 0xc0, 0xc0, 0xc3, 0x7e, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x38, 0x6c, 0x64, 0x60, 0xf0, 0x60, 0x60, 0x60, 0x60, 0xe6, 0xfc, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xc3, 0x66, 0x3c, 0x18, 0xff, 0x18, 0xff, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xfc, 0x66, 0x66, 0x7c, 0x62, 0x66, 0x6f, 0x66, 0x66, 0x66, 0xf3, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x0e, 0x1b, 0x18, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x18, 0x18, 0x18, 0xd8, 0x70, 0x00, 0x00,
 0x00, 0x18, 0x30, 0x60, 0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x0c, 0x18, 0x30, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x18, 0x30, 0x60, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x18, 0x30, 0x60, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x76, 0xdc, 0x00, 0xdc, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00,
 0x76, 0xdc, 0x00, 0xc6, 0xe6, 0xf6, 0xfe, 0xde, 0xce, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x3c, 0x6c, 0x6c, 0x3e, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x38, 0x6c, 0x6c, 0x38, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x30, 0x30, 0x00, 0x30, 0x30, 0x60, 0xc0, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xc0, 0xc0, 0xc0, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x06, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xc0, 0xc0, 0xc2, 0xc6, 0xcc, 0x18, 0x30, 0x60, 0xce, 0x9b, 0x06, 0x0c, 0x1f, 0x00, 0x00,
 0x00, 0xc0, 0xc0, 0xc2, 0xc6, 0xcc, 0x18, 0x30, 0x66, 0xce, 0x96, 0x3e, 0x06, 0x06, 0x00, 0x00,
 0x00, 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x3c, 0x3c, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x6c, 0xd8, 0x6c, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xd8, 0x6c, 0x36, 0x6c, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44,
 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa,
 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xf8, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x18, 0x18, 0x18, 0x18, 0x18, 0xf8, 0x18, 0xf8, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0xf6, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x18, 0xf8, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x36, 0x36, 0x36, 0x36, 0x36, 0xf6, 0x06, 0xf6, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x06, 0xf6, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x36, 0x36, 0x36, 0x36, 0x36, 0xf6, 0x06, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x18, 0x18, 0x18, 0x18, 0x18, 0xf8, 0x18, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1f, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x1f, 0x18, 0x1f, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x37, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x36, 0x36, 0x36, 0x36, 0x36, 0x37, 0x30, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x30, 0x37, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x36, 0x36, 0x36, 0x36, 0x36, 0xf7, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xf7, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x36, 0x36, 0x36, 0x36, 0x36, 0x37, 0x30, 0x37, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x36, 0x36, 0x36, 0x36, 0x36, 0xf7, 0x00, 0xf7, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x18, 0x18, 0x18, 0x18, 0x18, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x1f, 0x18, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x18, 0x1f, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
 0x18, 0x18, 0x18, 0x18, 0x18, 0xff, 0x18, 0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0,
 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f,
 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xdc, 0xd8, 0xd8, 0xd8, 0xdc, 0x76, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x78, 0xcc, 0xcc, 0xcc, 0xd8, 0xcc, 0xc6, 0xc6, 0xc6, 0xcc, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xfe, 0xc6, 0xc6, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0xfe, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0xfe, 0xc6, 0x60, 0x30, 0x18, 0x30, 0x60, 0xc6, 0xfe, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0xd8, 0xd8, 0xd8, 0xd8, 0xd8, 0x70, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7c, 0x60, 0x60, 0xc0, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x76, 0xdc, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x7e, 0x18, 0x3c, 0x66, 0x66, 0x66, 0x3c, 0x18, 0x7e, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x38, 0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0x6c, 0x38, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x38, 0x6c, 0xc6, 0xc6, 0xc6, 0x6c, 0x6c, 0x6c, 0x6c, 0xee, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x1e, 0x30, 0x18, 0x0c, 0x3e, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0xdb, 0xdb, 0xdb, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x03, 0x06, 0x7e, 0xdb, 0xdb, 0xf3, 0x7e, 0x60, 0xc0, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x1c, 0x30, 0x60, 0x60, 0x7c, 0x60, 0x60, 0x60, 0x30, 0x1c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x00, 0xfe, 0x00, 0x00, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x30, 0x18, 0x0c, 0x06, 0x0c, 0x18, 0x30, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x0c, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0c, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x0e, 0x1b, 0x1b, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xd8, 0xd8, 0xd8, 0x70, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x7e, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xdc, 0x00, 0x76, 0xdc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x38, 0x6c, 0x6c, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x0f, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0xec, 0x6c, 0x6c, 0x3c, 0x1c, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xd8, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x70, 0xd8, 0x30, 0x60, 0xc8, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void vga_initmode(VGAState *s)
{
    /* The screen clear below walks every dword of the buffer. */
    for (int i = 0; i < 64*3; i++)
        s->palette[i] = pal_ega[i];
    s->palette_dirty = 1; s->palette_gen++;

    for (int i = 0; i <= 0x13; i++)
        s->ar[i] = actl[i];
    s->ar[0x14] = 0;

    s->sr[0] = 0x3;
    for (int i = 0; i < 4; i++)
        s->sr[i + 1] = sequ[i];

    for (int i = 0; i <= 8; i++)
        s->gr[i] = grdc[i];

    for (int i = 0; i <= 0x18; i++)
        s->cr[i] = crtc[i];

    s->msr = 0x67;

    // clear screen
    for (int i = 0; i < s->vga_ram_size / 4; i++) {
        s->vga_ram[i * 4] = 0x20;
        s->vga_ram[i * 4 + 1] = 0x07;
    }

    // load font
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < 16; j++) {
            s->vga_ram[i * 32 * 4 + j * 4 + 2] = vgafont16[i * 16 + j];
        }
    }

    /* The buffer has just been rewritten from the bottom and the new mode may
     * well be one that cannot reach the top, so put it back on offer.  This is
     * the mode-set boundary; graphic_mode alone does not change when a game
     * moves between planar and chain 4. */
    s->ar_index = 0x20;
}

//=============================================================================
// Accessor functions for hardware VGA driver integration
//=============================================================================

int vga_dump_state(VGAState *s, char *buf, int size)
{
    int n = 0;
#define DUMP(...) do { if (n < size) n += snprintf(buf + n, size - n, __VA_ARGS__); } while (0)
    DUMP("msr=%02x graphic_mode=%d gr6=%02x ar10=%02x sr1=%02x sr3=%02x sr4=%02x force_8dm=%d fb=%dx%d\n",
         s->msr, s->graphic_mode, s->gr[6], s->ar[0x10], s->sr[1], s->sr[3], s->sr[4],
         s->force_8dm, s->fb_dev->width, s->fb_dev->height);
    DUMP("cr:");
    for (int i = 0; i < 0x19; i++) DUMP(" %02x", s->cr[i]);
    DUMP("\nsr:");
    for (int i = 0; i < 5; i++) DUMP(" %02x", s->sr[i]);
    DUMP(" gr:");
    for (int i = 0; i < 9; i++) DUMP(" %02x", s->gr[i]);
    DUMP(" ar10-14:");
    for (int i = 0x10; i < 0x15; i++) DUMP(" %02x", s->ar[i]);
    /* What vga_text_refresh() computes from them. */
    const int cheight = (s->cr[9] & 0x1f) + 1;
    const int cwidth = (!s->force_8dm && !(s->sr[1] & 0x01)) ? 9 : 8;
    const int width = s->cr[0x01] + 1;
    const int vde = s->cr[0x12] | ((s->cr[0x07] & 0x02) << 7) | ((s->cr[0x07] & 0x40) << 3);
    const int rows = (vde + 1) / cheight;
    const unsigned start = s->cr[0x0d] | (s->cr[0x0c] << 8);
    DUMP("\ntext geometry: %d cols x %d rows, cell %dx%d, %dx%d pixels, doublescan=%d, line_offset=%d, start=%04x, fits=%d\n",
         width, rows, cwidth, cheight, width * cwidth, rows * cheight, (s->cr[9] >> 7) & 1,
         s->cr[0x13] * 2, start,
         !(s->fb_dev->width < width * cwidth || s->fb_dev->height < rows * cheight ||
           width > MAX_TEXT_WIDTH || rows > MAX_TEXT_HEIGHT));
    /* Which characters the screen is made of, and the first of the most
     * used ones as they stand in the font. */
    uint32_t count[256] = { 0 };
    for (int i = 0; i < width * rows && i < 132 * 132; i++) {
        const uint32_t a = ((start + i) * 4u) & 0x1fffcu;
        if ((int)a < s->vga_ram_size) count[s->vga_ram[a]]++;
    }
    int top[4] = { -1, -1, -1, -1 };
    for (int c = 0; c < 256; c++)
        for (int k = 0; k < 4; k++)
            if (count[c] && (top[k] < 0 || count[c] > count[top[k]])) {
                for (int m = 3; m > k; m--) top[m] = top[m - 1];
                top[k] = c;
                break;
            }
    const uint32_t v = s->sr[3];
    const uint8_t *font = s->vga_ram + (((v >> 4) & 1) | ((v << 1) & 6)) * 8192 * 4 + 2;
    for (int k = 0; k < 4 && top[k] >= 0; k++) {
        DUMP("char %02x x%u font:", top[k], (unsigned)count[top[k]]);
        for (int r = 0; r < 16; r++) DUMP(" %02x", font[(32 * top[k] + r) * 4]);
        DUMP("\n");
    }
    DUMP("row0 ch/at:");
    for (int i = 0; i < 40; i++) {
        const uint32_t a = ((start + i) * 4u) & 0x1fffcu;
        DUMP(" %02x%02x", s->vga_ram[a], s->vga_ram[a + 1]);
    }
    DUMP("\n");
#undef DUMP
    return n < size ? n : size - 1;
}

// Visible columns are derived from CRTC Horizontal Display End (index 0x01).
// In text modes this is 39 (40 cols) or 79 (80 cols).
int vga_get_text_cols(VGAState *s) {
    if (!s) return 80;
    int cols = (int)s->cr[0x01] + 1;
    if (cols == 40 || cols == 80) return cols;
    // Fallback: clamp to sane values
    return (cols < 60) ? 40 : 80;
}

/* Get current VGA mode: 0=blank, 1=text, 2=graphics */
int vga_get_mode(VGAState *s)
{
    if (!(s->ar_index & 0x20)) {
        return 0;  // blank
    } else if (s->gr[0x06] & 1) {
        return 2;  // graphics
    } else {
        return 1;  // text
    }
}

/* Get VGA start address (for scrolling) */
uint16_t vga_get_start_addr(VGAState *s)
{
    return (s->cr[0x0c] << 8) | s->cr[0x0d];
}

/* Get VGA panning (horizontal pixel scrolling, 0-7) */
uint8_t vga_get_panning(VGAState *s)
{
    return s->ar[0x13] & 0x0F;
}

/* Get cursor info for external VGA drivers */
void vga_get_cursor_info(VGAState *s, int *x, int *y, int *start, int *end, int *visible)
{
    if (!s) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (start) *start = 0;
        if (end) *end = 0;
        if (visible) *visible = 0;
        return;
    }

    // Get cursor offset from CRT registers (0x0E=high, 0x0F=low)
    uint16_t cursor_pos = (s->cr[0x0e] << 8) | s->cr[0x0f];
    uint16_t start_addr = (s->cr[0x0c] << 8) | s->cr[0x0d];
    int cursor_offset = cursor_pos - start_addr;  // Relative to visible screen

    // Calculate x, y from offset
    int width = (s->cr[0x01] + 1);
    if (width <= 0) width = 80;

    if (x) *x = cursor_offset % width;
    if (y) *y = cursor_offset / width;
    if (start) *start = s->cr[0x0a] & 0x1f;
    if (end) *end = s->cr[0x0b] & 0x1f;

    // Cursor is hidden if start > end or if cursor disable bit is set
    if (visible) *visible = !((s->cr[0x0a] & 0x20) || ((s->cr[0x0a] & 0x1f) > (s->cr[0x0b] & 0x1f)));
}

/* Legacy cursor function (for compatibility) */
void vga_get_cursor(VGAState *s, int *x, int *y, int *start, int *end)
{
    vga_get_cursor_info(s, x, y, start, end, NULL);
}

/* Get pointer to VGA DAC palette (768 bytes: 256 colors × 3 RGB values, each 0-63) */
const uint8_t* vga_get_palette(VGAState *s)
{
    return s->palette;
}

/* Check if palette was modified since last call (clears the flag) */
int vga_is_palette_dirty(VGAState *s)
{
    int dirty = s->palette_dirty;
    s->palette_dirty = 0;
    return dirty;
}

/* Get EGA 16-color palette (applies AC palette register indirection)
 * Fills palette16 with 16 entries of RGB triplets (48 bytes total)
 */
void vga_get_palette16(VGAState *s, uint8_t *palette16)
{
    for (int i = 0; i < 16; i++) {
        int v = s->ar[i];
        if (s->ar[0x10] & 0x80)
            v = ((s->ar[0x14] & 0xf) << 4) | (v & 0xf);
        else
            v = ((s->ar[0x14] & 0xc) << 4) | (v & 0x3f);
        v = v * 3;
        palette16[i * 3 + 0] = s->palette[v + 0];  // R
        palette16[i * 3 + 1] = s->palette[v + 1];  // G
        palette16[i * 3 + 2] = s->palette[v + 2];  // B
    }
}

/* Get detailed graphics mode information for hardware rendering
 * Returns: 0=text, 1=CGA 4-color, 2=EGA 16-color, 3=VGA 256-color (mode 13h),
 *          4=CGA 2-color, 5=Mode X (VGA 256-color planar unchained)
 * Also fills in width, height if pointers are non-NULL
 */
int vga_get_graphics_mode(VGAState *s, int *width, int *height)
{
    // Check if display is enabled
    if (!(s->ar_index & 0x20)) {
        return 0;  // blank/text
    }

    // Check if graphics mode
    if (!(s->gr[0x06] & 1)) {
        return 0;  // text mode
    }

    // Get shift_control to determine graphics mode type
    int shift_control = (s->gr[0x05] >> 5) & 3;

    // Calculate dimensions
    int w = (s->cr[0x01] + 1) * 8;
    int h = s->cr[0x12] |
        ((s->cr[0x07] & 0x02) << 7) |
        ((s->cr[0x07] & 0x40) << 3);
    h++;

    // Handle double-scan and multi-scan for height calculation
    int double_scan = (s->cr[0x09] >> 7);
    int multi_scan = 1;
    if (shift_control != 1) {
        multi_scan = (((s->cr[0x09] & 0x1f) + 1) << double_scan);
    }
    if (multi_scan > 1) {
        h = (h + multi_scan - 1) / multi_scan;
    }

    // For VGA 256-color mode (shift_control == 2), CRTC width is doubled.
    if (shift_control == 2) {
        w = w / 2;
    }

    if (width)  *width  = w;
    if (height) *height = h;

    // -----------------------------------------------------------------------
    // Mode X detection — check SR[4] BEFORE shift_control.
    //
    // Wolf3D calls VGAWRITEMODE(0/1/2) constantly, overwriting GR[5] bits 5-6
    // (shift_control). SR[4] is set once by VL_DePlaneVGA and never changed.
    //   VL_DePlaneVGA: SR[4] = (old & ~8) | 4  →  chain4=0 (bit3), seq=1 (bit2)
    // -----------------------------------------------------------------------
    int rv;
    if (!(s->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M) &&   /* chain4 OFF */
         (s->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_SEQ_MODE) &&  /* sequential ON */
         (s->ar[0x10] & 0x40) &&                               /* 8-bit DAC color */
         w >= 256 && w <= 400) {
        /*
         * The width used to have to be exactly 320, and Mode X is not a
         * single resolution: 256, 320, 360 and 400 wide are all ordinary
         * tweaks of it.  Dyna Blaster programs 256x232 - CRTC 64 characters
         * with the 256-colour shift, so w comes out 256 - and fell through
         * to chain 4, which reads the same memory as one linear byte per
         * pixel instead of four interleaved planes.  Half the picture, drawn
         * over itself.
         *
         * Nothing is lost by widening it.  The attribute controller's 8-bit
         * DAC bit above is what separates a 256-colour mode from a 16-colour
         * planar one, and that is the distinction the width was standing in
         * for; a 640-wide EGA mode fails that test on its own.  The width is
         * still bounded because shift_control, which would say this directly,
         * cannot be trusted - Wolf3D rewrites GR[5] on every write mode
         * change, which is what the note above is about.
         */
        rv = 5;  // Mode X
    } else if (shift_control == 0) {
        if ((s->gr[0x06] & 0x0C) == 0x0C && w >= 640)
            rv = 4;  // CGA 2-color
        else if (!(s->cr[0x17] & 0x01) && w >= 640)
            rv = 4;  // CGA 2-color
        else
            rv = 2;  // EGA planar 16-color
    } else if (shift_control == 1) {
        rv = (w >= 640) ? 4 : 1;  // CGA 2- or 4-color
    } else {
        rv = 3;  // Mode 13h chain4 256-color
    }

    return rv;
}

/* Get VGA line offset (bytes per scanline in video memory)
 * For EGA planar mode, this is the number of uint32_t words per line
 */
int vga_get_line_offset(VGAState *s)
{
    // cr[0x13] is the line offset in words (2 bytes each)
    // For planar mode, each "word" is a 32-bit value (4 planes packed)
    return s->cr[0x13];
}

/* Get Line Compare register (scanline where video address resets to 0) */
int vga_get_line_compare(VGAState *s)
{
    int lc = s->cr[0x18] |
             ((s->cr[0x07] & 0x10) << 4) |
             ((s->cr[0x09] & 0x40) << 3);
    return lc;
}

/* Check if VGA is in Vertical Retrace */
bool vga_in_retrace(VGAState *s)
{
    return (s->st01 & ST01_V_RETRACE) != 0;
}

/* Get cursor blink phase (1 = visible, 0 = hidden during blink)
 * Also updates the blink state based on time for hardware VGA drivers
 * that don't use vga_display_update_text */
int vga_get_cursor_blink_phase(VGAState *s)
{
    uint32_t now = get_uticks();
    if (after_eq(now, s->cursor_blink_time)) {
        s->cursor_blink_time = now + 133333;  // ~3.75 Hz blink rate
        s->cursor_visible_phase = !s->cursor_visible_phase;
    }
    return s->cursor_visible_phase;
}

/* Get character cell height (from CRT register 0x09 max scan line + 1)
 * Typically 8 for CGA-style modes or 16 for VGA text mode */
int vga_get_char_height(VGAState *s)
{
    if (!s) return 16;
    int cheight = (s->cr[0x09] & 0x1f) + 1;
    if (cheight <= 0) cheight = 16;
    return cheight;
}

/* Get pointer to font data for character `ch` in font bank A (SR3 bits 1:0,5:4).
 * Returns a pointer into vga_ram plane 2, stride = 4 bytes between rows.
 * `cheight` rows are valid starting from the returned pointer.
 * Returns NULL if vga_state is NULL.
 */
const uint8_t * vga_get_font_ptr(VGAState *s, uint8_t ch, int attr_bit3)
{
    if (!s) return NULL;
    /* SR3 font select: same decode as vga_text_refresh */
    uint32_t v = s->sr[0x3];
    int bank = attr_bit3 & 1;
    uint32_t slot;
    if (bank == 0)
        slot = (((v >> 4) & 1) | ((v << 1) & 6));   /* font A */
    else
        slot = (((v >> 5) & 1) | ((v >> 1) & 6));   /* font B */
    /* Each slot is 8192 uint32_t words = 8192*4 bytes. Plane 2 byte = offset+2. */
    return s->vga_ram + slot * 8192 * 4 + 2 + (uint32_t)ch * 32 * 4;
}
