#pragma once
#include <stdint.h>

/* IBM CP437, 8x16, one byte per scanline.  The menus need the box-drawing
 * and shade glyphs at 0xB0-0xDF, which a Latin-1 console font does not have. */
extern const uint8_t font_8x16[4096];
