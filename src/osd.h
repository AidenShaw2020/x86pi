#pragma once
/*
 * On-screen menus.
 *
 * The menus are written against an 80x25 grid of character cells, which is
 * what the original on-screen display offered and what the menu code below
 * still thinks in.  The grid is only a layout system now: the renderer draws
 * real pixels into the 32-bit surface, so a panel is a rounded rectangle with
 * a shadow rather than a run of CP437 corner glyphs, and the background is
 * the paused guest frame dimmed down rather than a field of shade characters.
 *
 * Keeping the cell API means a menu can be written, read and reasoned about
 * in rows and columns without knowing any of that.
 */

#include <stdint.h>
#include <stdbool.h>

#define OSD_COLS 80
#define OSD_ROWS 25
#define OSD_CELL_W 8
#define OSD_CELL_H 16

/*
 * Colour indices, in the order the IBM text attribute byte uses them.  The
 * renderer maps each one to a modern colour rather than the 1981 RGBI value;
 * the names say what the caller means, not what a CGA card would emit.
 */
#define OSD_BLACK        0x00
#define OSD_BLUE         0x01
#define OSD_GREEN        0x02
#define OSD_CYAN         0x03
#define OSD_RED          0x04
#define OSD_MAGENTA      0x05
#define OSD_BROWN        0x06
#define OSD_LIGHTGRAY    0x07
#define OSD_DARKGRAY     0x08
#define OSD_LIGHTBLUE    0x09
#define OSD_LIGHTGREEN   0x0A
#define OSD_LIGHTCYAN    0x0B
#define OSD_LIGHTRED     0x0C
#define OSD_LIGHTMAGENTA 0x0D
#define OSD_YELLOW       0x0E
#define OSD_WHITE        0x0F

#define OSD_ATTR(fg, bg) (uint8_t)(((bg) << 4) | (fg))

/* A cell whose background is OSD_BLACK is transparent: the dimmed guest frame
 * shows through it.  That is what the area outside the panels uses. */
#define OSD_ATTR_BACKDROP   OSD_ATTR(OSD_LIGHTGRAY, OSD_BLACK)
#define OSD_ATTR_NORMAL     OSD_ATTR(OSD_WHITE, OSD_BLUE)
#define OSD_ATTR_HIGHLIGHT  OSD_ATTR(OSD_YELLOW, OSD_BLUE)
#define OSD_ATTR_SELECTED   OSD_ATTR(OSD_WHITE, OSD_LIGHTBLUE)
#define OSD_ATTR_TITLE      OSD_ATTR(OSD_WHITE, OSD_RED)
#define OSD_ATTR_BORDER     OSD_ATTR(OSD_LIGHTCYAN, OSD_BLUE)
#define OSD_ATTR_DISABLED   OSD_ATTR(OSD_DARKGRAY, OSD_BLUE)
#define OSD_ATTR_HINT       OSD_ATTR(OSD_LIGHTGRAY, OSD_BLUE)
#define OSD_ATTR_VALUE      OSD_ATTR(OSD_LIGHTCYAN, OSD_BLUE)

#ifdef __cplusplus
extern "C" {
#endif

void osd_init(void);

/*
 * Showing the menu takes a copy of what is on screen, because the renderer
 * builds every frame from that copy and the guest is paused, so the surface
 * itself is never refreshed again while the menu is up.
 */
void osd_show(void);
void osd_hide(void);
bool osd_is_visible(void);

void osd_clear(void);
void osd_putchar(int x, int y, char ch, uint8_t attr);
void osd_print(int x, int y, const char *str, uint8_t attr);
void osd_print_center(int y, const char *str, uint8_t attr);

/*
 * Panels.  A box is a raised surface with a shadow, not a border of glyphs,
 * so the cells inside it are left for the caller to fill with text.  At most
 * OSD_MAX_PANELS may be open at once, which is enough for a menu and a
 * confirmation dialog on top of it.
 */
#define OSD_MAX_PANELS 4
void osd_draw_box(int x, int y, int w, int h, uint8_t attr);
void osd_draw_box_titled(int x, int y, int w, int h, const char *title, uint8_t attr);

void osd_fill(int x, int y, int w, int h, char ch, uint8_t attr);
uint8_t *osd_get_buffer(void);

/*
 * Paint the menu into a 32-bit surface.  The grid is centred, so a 640x480
 * surface leaves a forty-pixel band above and below that is drawn as part of
 * the dimmed backdrop.
 */
/* True once since the last call if the grid changed; the caller repaints
 * only then. */
bool osd_take_dirty(void);

void osd_render(uint8_t *surface, uint32_t pitch, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif
