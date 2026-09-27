/*
 * On-screen menus: the cell grid, and the renderer that turns it into pixels.
 *
 * See osd.h for why the menus are written in rows and columns while what
 * reaches the screen is rounded panels over a dimmed copy of the guest frame.
 */

#include "osd.h"
#include "font8x16.h"
#include <string.h>
#include <stdlib.h>

#define GRID_W (OSD_COLS * OSD_CELL_W)   /* 640 */
#define GRID_H (OSD_ROWS * OSD_CELL_H)   /* 400 */

static uint8_t osd_buffer[OSD_COLS * OSD_ROWS * 2];
static bool osd_visible;
/* Set by anything that changes the grid.  The renderer is a full-surface
 * blend, so it runs when there is something new to show and not once per
 * pass through the loop. */
static bool osd_dirty;

struct panel { int x, y, w, h; uint8_t attr; char title[40]; };
static struct panel panels[OSD_MAX_PANELS];
static int panel_count;

/*
 * The backdrop.
 *
 * Every frame is built from this copy rather than from the surface, because
 * dimming the surface in place would darken it again on each redraw until the
 * picture was gone.  It is taken when the menu opens; the guest is paused by
 * then, so nothing will change underneath.
 */
static uint32_t *backdrop;
static uint32_t backdrop_w, backdrop_h;

/* ------------------------------------------------------------------------ */
/* Palette                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * The attribute byte carries IBM's colour numbering because that is what the
 * menus were written against.  What those numbers mean here is a slate-blue
 * scheme that suits a panel with a shadow rather than the saturated RGBI set
 * a CGA card would have produced.
 */
static const uint32_t palette[16] = {
    0x000000,  /* black        - transparent, see cell_is_backdrop() */
    0x1e2634,  /* blue         - the panel surface */
    0x3fb27f,  /* green        - success */
    0x7fc4d8,  /* cyan         - secondary text */
    0xe05c5c,  /* red          - the title bar, and warnings */
    0xb07fd0,  /* magenta */
    0xc79a4a,  /* brown        - values that differ from what is saved */
    0xc9d4e3,  /* light gray   - ordinary text */
    0x69748a,  /* dark gray    - disabled text */
    0x3b6ea5,  /* light blue   - the selection bar */
    0x86d6a4,  /* light green */
    0x8fd3e8,  /* light cyan   - values */
    0xff8a80,  /* light red */
    0xd9a7ef,  /* light magenta */
    0xf2c66d,  /* yellow       - headings and keys */
    0xffffff,  /* white        - emphasis */
};

#define PANEL_BG      palette[OSD_BLUE]
#define PANEL_EDGE    0x39435a
#define PANEL_RADIUS  7
#define SHADOW_DROP   6
/* How far the menu is lifted off centre to clear the wordmark below it. */
#define WORDMARK_LIFT 10

/* ------------------------------------------------------------------------ */
/* The cell grid                                                             */
/* ------------------------------------------------------------------------ */

void osd_init(void)
{
    osd_clear();
    osd_visible = false;
}

void osd_clear(void)
{
    for (int i = 0; i < OSD_COLS * OSD_ROWS; i++) {
        osd_buffer[i * 2] = ' ';
        osd_buffer[i * 2 + 1] = OSD_ATTR_BACKDROP;
    }
    panel_count = 0;
    osd_dirty = true;
}

void osd_putchar(int x, int y, char ch, uint8_t attr)
{
    osd_dirty = true;
    if (x < 0 || x >= OSD_COLS || y < 0 || y >= OSD_ROWS) return;
    const int idx = (y * OSD_COLS + x) * 2;
    osd_buffer[idx] = (uint8_t)ch;
    osd_buffer[idx + 1] = attr;
}

void osd_print(int x, int y, const char *str, uint8_t attr)
{
    while (*str && x < OSD_COLS) osd_putchar(x++, y, *str++, attr);
}

void osd_print_center(int y, const char *str, uint8_t attr)
{
    int x = (OSD_COLS - (int)strlen(str)) / 2;
    if (x < 0) x = 0;
    osd_print(x, y, str, attr);
}

void osd_fill(int x, int y, int w, int h, char ch, uint8_t attr)
{
    for (int row = y; row < y + h && row < OSD_ROWS; row++)
        for (int col = x; col < x + w && col < OSD_COLS; col++)
            osd_putchar(col, row, ch, attr);
}

uint8_t *osd_get_buffer(void) { return osd_buffer; }

static void panel_add(int x, int y, int w, int h, uint8_t attr, const char *title)
{
    if (w < 2 || h < 2) return;
    /* Clear the cells the panel covers so whatever was behind it - the
     * backdrop, or a panel it is drawn over - does not print through. */
    const uint8_t fill = OSD_ATTR(attr & 0x0f, OSD_BLUE);
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            osd_putchar(col, row, ' ', fill);

    if (panel_count >= OSD_MAX_PANELS) return;
    struct panel *p = &panels[panel_count++];
    p->x = x; p->y = y; p->w = w; p->h = h; p->attr = attr;
    p->title[0] = 0;
    if (title) {
        size_t n = strlen(title);
        if (n >= sizeof p->title) n = sizeof p->title - 1;
        memcpy(p->title, title, n);
        p->title[n] = 0;
    }
}

void osd_draw_box(int x, int y, int w, int h, uint8_t attr)
{
    panel_add(x, y, w, h, attr, 0);
}

void osd_draw_box_titled(int x, int y, int w, int h, const char *title, uint8_t attr)
{
    panel_add(x, y, w, h, attr, title);
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

bool osd_is_visible(void) { return osd_visible; }

void osd_show(void) { osd_visible = true; osd_dirty = true; }

bool osd_take_dirty(void) { const bool d = osd_dirty; osd_dirty = false; return d; }

void osd_hide(void)
{
    osd_visible = false;
    free(backdrop);
    backdrop = 0;
    backdrop_w = backdrop_h = 0;
}

static inline uint32_t blend(uint32_t a, uint32_t b, unsigned t)  /* t: 0..256 */
{
    const unsigned ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    const unsigned br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    const unsigned r = (ar * (256u - t) + br * t) >> 8;
    const unsigned g = (ag * (256u - t) + bg * t) >> 8;
    const unsigned bl = (ab * (256u - t) + bb * t) >> 8;
    return (r << 16) | (g << 8) | bl;
}

static void capture_backdrop(const uint8_t *surface, uint32_t pitch,
                             uint32_t width, uint32_t height)
{
    if (backdrop && backdrop_w == width && backdrop_h == height) return;
    free(backdrop);
    backdrop = (uint32_t *)malloc((size_t)width * height * 4u);
    backdrop_w = width; backdrop_h = height;
    if (!backdrop) return;
    for (uint32_t y = 0; y < height; y++)
        memcpy(backdrop + (size_t)y * width, surface + (size_t)y * pitch, width * 4u);
}

/*
 * The backdrop: the guest's own frame, taken down to about a fifth and pulled
 * towards the panel colour, with a gentle vertical gradient over it.  Leaving
 * the picture faintly visible says which program the menu is sitting on top
 * of, which a solid fill or a field of shade characters does not.
 */
static void paint_backdrop(uint8_t *surface, uint32_t pitch,
                           uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        uint32_t *dst = (uint32_t *)(surface + (size_t)y * pitch);
        const uint32_t *src = backdrop ? backdrop + (size_t)y * width : 0;
        /* 0 at the top, 40/256 at the bottom. */
        const unsigned shade = (unsigned)((y * 40u) / (height ? height : 1u));
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t frame = src ? src[x] & 0xffffffu : 0u;
            uint32_t c = blend(frame, 0x121722, 200u);
            c = blend(c, 0x000000, shade);
            dst[x] = c;
        }
    }
}

/* Whether (px, py) lies inside a rectangle with rounded corners. */
static inline int in_rounded(int px, int py, int w, int h, int r)
{
    int cx = -1, cy = -1;
    if (px < r)          cx = r;        else if (px >= w - r) cx = w - 1 - r;
    if (py < r)          cy = r;        else if (py >= h - r) cy = h - 1 - r;
    if (cx < 0 || cy < 0) return 1;     /* along an edge, not in a corner */
    const int dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

static void fill_rounded(uint8_t *surface, uint32_t pitch,
                         uint32_t width, uint32_t height,
                         int x0, int y0, int w, int h, int r,
                         uint32_t colour, unsigned alpha)
{
    for (int py = 0; py < h; py++) {
        const int sy = y0 + py;
        if (sy < 0 || (uint32_t)sy >= height) continue;
        uint32_t *dst = (uint32_t *)(surface + (size_t)sy * pitch);
        for (int px = 0; px < w; px++) {
            const int sx = x0 + px;
            if (sx < 0 || (uint32_t)sx >= width) continue;
            if (!in_rounded(px, py, w, h, r)) continue;
            dst[sx] = alpha >= 256u ? colour : blend(dst[sx] & 0xffffffu, colour, alpha);
        }
    }
}

static void stroke_rounded(uint8_t *surface, uint32_t pitch,
                           uint32_t width, uint32_t height,
                           int x0, int y0, int w, int h, int r, uint32_t colour)
{
    for (int py = 0; py < h; py++) {
        const int sy = y0 + py;
        if (sy < 0 || (uint32_t)sy >= height) continue;
        uint32_t *dst = (uint32_t *)(surface + (size_t)sy * pitch);
        for (int px = 0; px < w; px++) {
            const int sx = x0 + px;
            if (sx < 0 || (uint32_t)sx >= width) continue;
            /* On the edge: inside the shape, but with a neighbour outside. */
            if (!in_rounded(px, py, w, h, r)) continue;
            if (in_rounded(px - 1, py, w, h, r) && in_rounded(px + 1, py, w, h, r) &&
                in_rounded(px, py - 1, w, h, r) && in_rounded(px, py + 1, w, h, r) &&
                px > 0 && py > 0 && px < w - 1 && py < h - 1)
                continue;
            dst[sx] = colour;
        }
    }
}

static void draw_glyph(uint8_t *surface, uint32_t pitch,
                       uint32_t width, uint32_t height,
                       int x0, int y0, uint8_t ch, uint32_t fg)
{
    const uint8_t *rows = &font_8x16[(unsigned)ch * 16u];
    for (int gy = 0; gy < OSD_CELL_H; gy++) {
        const int sy = y0 + gy;
        if (sy < 0 || (uint32_t)sy >= height) continue;
        const uint8_t bits = rows[gy];
        if (!bits) continue;
        uint32_t *dst = (uint32_t *)(surface + (size_t)sy * pitch);
        for (int gx = 0; gx < OSD_CELL_W; gx++) {
            /* This table stores bit 0 as the leftmost pixel, not the
             * usual MSB-first order. */
            if (!(bits & (1u << gx))) continue;
            const int sx = x0 + gx;
            if (sx < 0 || (uint32_t)sx >= width) continue;
            dst[sx] = fg;
        }
    }
}

/*
 * The machine's name, under everything else.
 *
 * The grid is 640x400 in a 640x480 frame, so there is a band at the bottom
 * that no menu can reach.  The wordmark goes there, drawn at twice the font
 * size and in the logo's two colours, with a rule running out to each side -
 * present when you look for it and out of the way when you do not.
 *
 * It sits on the bottom edge rather than in the middle of the band, because
 * the settings panel uses every row it has and its shadow comes down past
 * the grid: centred, the two were touching.
 */
static void draw_glyph_2x(uint8_t *surface, uint32_t pitch,
                          uint32_t width, uint32_t height,
                          int x0, int y0, uint8_t ch, uint32_t fg)
{
    const uint8_t *rows = &font_8x16[(unsigned)ch * 16u];
    for (int gy = 0; gy < OSD_CELL_H * 2; gy++) {
        const int sy = y0 + gy;
        if (sy < 0 || (uint32_t)sy >= height) continue;
        const uint8_t bits = rows[gy / 2];
        if (!bits) continue;
        uint32_t *dst = (uint32_t *)(surface + (size_t)sy * pitch);
        for (int gx = 0; gx < OSD_CELL_W * 2; gx++) {
            if (!(bits & (1u << (gx / 2)))) continue;
            const int sx = x0 + gx;
            if (sx < 0 || (uint32_t)sx >= width) continue;
            dst[sx] = fg;
        }
    }
}

static void draw_wordmark(uint8_t *surface, uint32_t pitch,
                          uint32_t width, uint32_t height, int y)
{
    static const char left[] = "X86";
    static const char right[] = "Pi";
    const int cell = OSD_CELL_W * 2;
    const int text_w = (int)(sizeof left - 1 + sizeof right - 1) * cell;
    const int x = ((int)width - text_w) / 2;

    if (y < 0 || y + OSD_CELL_H * 2 > (int)height) return;

    int at = x;
    for (const char *c = left; *c; c++, at += cell)
        draw_glyph_2x(surface, pitch, width, height, at, y, (uint8_t)*c,
                      palette[OSD_LIGHTGRAY]);
    for (const char *c = right; *c; c++, at += cell)
        draw_glyph_2x(surface, pitch, width, height, at, y, (uint8_t)*c,
                      palette[OSD_LIGHTRED]);

    /* The rules, well clear of the letters and fading out towards the edge. */
    const int rule_y = y + OSD_CELL_H;
    if (rule_y < 0 || (uint32_t)rule_y >= height) return;
    uint32_t *row = (uint32_t *)(surface + (size_t)rule_y * pitch);
    const int gap = 18, reach = 96;
    for (int i = 0; i < reach; i++) {
        const unsigned t = (unsigned)(200 - i * 200 / reach);
        const int lx = x - gap - i, rx = x + text_w + gap + i;
        if (lx >= 0) row[lx] = blend(row[lx] & 0xffffffu, palette[OSD_LIGHTBLUE], t);
        if (rx < (int)width) row[rx] = blend(row[rx] & 0xffffffu, palette[OSD_LIGHTBLUE], t);
    }
}

static void draw_panel(uint8_t *surface, uint32_t pitch,
                       uint32_t width, uint32_t height,
                       const struct panel *p, int ox, int oy)
{
    const int x = ox + p->x * OSD_CELL_W - 6;
    const int y = oy + p->y * OSD_CELL_H - 8;
    const int w = p->w * OSD_CELL_W + 12;
    const int h = p->h * OSD_CELL_H + 16;

    /* Shadow first, as a soft stack rather than one hard offset copy. */
    for (int i = 3; i >= 1; i--)
        fill_rounded(surface, pitch, width, height,
                     x + i * (SHADOW_DROP / 3), y + i * (SHADOW_DROP / 3),
                     w, h, PANEL_RADIUS + 2, 0x000000, 40u);

    fill_rounded(surface, pitch, width, height, x, y, w, h, PANEL_RADIUS,
                 PANEL_BG, 256u);
    /* A lighter band across the top gives the surface somewhere to catch the
     * light, which flat fills at this size look wrong without. */
    fill_rounded(surface, pitch, width, height, x, y, w, OSD_CELL_H + 12,
                 PANEL_RADIUS, blend(PANEL_BG, 0xffffff, 16u), 256u);
    stroke_rounded(surface, pitch, width, height, x, y, w, h, PANEL_RADIUS,
                   PANEL_EDGE);

    if (p->title[0]) {
        const int len = (int)strlen(p->title);
        const int tx = x + (w - len * OSD_CELL_W) / 2;
        const int ty = y + 4;
        /* The title sits on a tinted plate so it reads as a heading. */
        fill_rounded(surface, pitch, width, height,
                     tx - 10, ty - 3, len * OSD_CELL_W + 20, OSD_CELL_H + 6,
                     (OSD_CELL_H + 6) / 2, palette[p->attr >> 4], 256u);
        for (int i = 0; i < len; i++)
            draw_glyph(surface, pitch, width, height,
                       tx + i * OSD_CELL_W, ty, (uint8_t)p->title[i],
                       palette[OSD_WHITE]);
    }
}

/* Which panel a cell belongs to, counting from the front: the one drawn last
 * owns it.  -1 means the cell is on the backdrop. */
static int topmost_panel(int col, int row)
{
    for (int i = panel_count - 1; i >= 0; i--) {
        const struct panel *p = &panels[i];
        if (col >= p->x && col < p->x + p->w && row >= p->y && row < p->y + p->h)
            return i;
    }
    return -1;
}

/*
 * A run of cells that share a background other than the panel's own is drawn
 * as one rounded bar, so a selected row has round ends instead of looking
 * like eight-pixel blocks stuck together.
 */
static void draw_row_backgrounds(uint8_t *surface, uint32_t pitch,
                                 uint32_t width, uint32_t height,
                                 int row, int ox, int oy, int layer)
{
    int col = 0;
    while (col < OSD_COLS) {
        const uint8_t bg = osd_buffer[(row * OSD_COLS + col) * 2 + 1] >> 4;
        if (bg == OSD_BLACK || bg == OSD_BLUE || topmost_panel(col, row) != layer) {
            col++;
            continue;
        }
        int end = col;
        while (end < OSD_COLS &&
               (osd_buffer[(row * OSD_COLS + end) * 2 + 1] >> 4) == bg &&
               topmost_panel(end, row) == layer) end++;
        const int x = ox + col * OSD_CELL_W - 3;
        const int w = (end - col) * OSD_CELL_W + 6;
        const int y = oy + row * OSD_CELL_H;
        fill_rounded(surface, pitch, width, height, x, y, w, OSD_CELL_H,
                     OSD_CELL_H / 2, palette[bg], 256u);
        col = end;
    }
}

/* Everything that belongs to one layer: the highlight bars, then the text. */
static void draw_layer(uint8_t *surface, uint32_t pitch,
                       uint32_t width, uint32_t height, int ox, int oy, int layer)
{
    for (int row = 0; row < OSD_ROWS; row++) {
        draw_row_backgrounds(surface, pitch, width, height, row, ox, oy, layer);
        for (int col = 0; col < OSD_COLS; col++) {
            if (topmost_panel(col, row) != layer) continue;
            const uint8_t ch = osd_buffer[(row * OSD_COLS + col) * 2];
            const uint8_t attr = osd_buffer[(row * OSD_COLS + col) * 2 + 1];
            if (ch == ' ' || ch == 0) continue;
            draw_glyph(surface, pitch, width, height,
                       ox + col * OSD_CELL_W, oy + row * OSD_CELL_H,
                       ch, palette[attr & 0x0f]);
        }
    }
}

/*
 * Everything drawn so far, taken down.
 *
 * A dialog is a panel over a menu, and without this the menu's own rows show
 * up beside it at full strength - half-covered words next to something that
 * has a shadow and is meant to be in front of them.  Dimming what is behind
 * is what makes the dialog read as being on top rather than mixed in.
 */
static void scrim(uint8_t *surface, uint32_t pitch, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        uint32_t *dst = (uint32_t *)(surface + (size_t)y * pitch);
        for (uint32_t x = 0; x < width; x++)
            dst[x] = blend(dst[x] & 0xffffffu, 0x0b0e14, 130u);
    }
}

void osd_render(uint8_t *surface, uint32_t pitch, uint32_t width, uint32_t height)
{
    if (!surface || width < GRID_W || height < GRID_H) return;

    capture_backdrop(surface, pitch, width, height);
    paint_backdrop(surface, pitch, width, height);

    const int ox = (int)(width - GRID_W) / 2;
    /*
     * Not quite centred: the menu is lifted by half the space under it so
     * that the wordmark on the bottom edge has room to breathe.
     */
    int oy = (int)(height - GRID_H) / 2;
    if (oy >= WORDMARK_LIFT) oy -= WORDMARK_LIFT;

    if ((int)height >= GRID_H + OSD_CELL_H * 2)
        draw_wordmark(surface, pitch, width, height,
                      (int)height - OSD_CELL_H * 2 - 4);

    draw_layer(surface, pitch, width, height, ox, oy, -1);

    for (int i = 0; i < panel_count; i++) {
        if (i > 0) scrim(surface, pitch, width, height);
        draw_panel(surface, pitch, width, height, &panels[i], ox, oy);
        draw_layer(surface, pitch, width, height, ox, oy, i);
    }
}
