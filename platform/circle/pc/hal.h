#pragma once
#include <stdint.h>
#include <circle/screen.h>
#include "../../src/i386.h"
/*
 * The tiny386 VGA renderer always owns a 640x480, 32-bit surface with a
 * 2560-byte stride.  A Circle display can have a different physical mode or
 * pitch (in particular when config.txt requests 1080p), so never hand its
 * buffer to vga_init() unless it is bit-for-bit that layout.
 */
struct CirclePcVideo {
    CScreenDevice *screen;
    uint8_t *physical;
    uint8_t *vga_surface;
    uint32_t physical_width;
    uint32_t physical_height;
    uint32_t physical_pitch;
    uint32_t physical_size;
    uint32_t offset_x;
    uint32_t offset_y;
    int staging;
};
extern "C" void circle_pc_redraw(void *, int, int, int, int);
