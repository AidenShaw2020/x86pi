/*
 * The settings menu, on Win+F11.
 *
 * Every row is one entry in the table below, including the headings, so the
 * layout is data rather than a sequence of draw calls and the cursor only has
 * to know how to skip what it cannot select.
 */

#include "settingsui.h"
#include "uikeys.h"
#include "config_save.h"
#include "osd.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    STATE_CLOSED,
    STATE_MAIN,
    STATE_SAVED,           /* a short confirmation over the menu */
    STATE_CONFIRM_RESTART, /* saved, and something needs the machine rebuilt */
    STATE_CONFIRM_BIGMEM   /* more memory than DOS will map */
} State;

/*
 * When a change takes hold, which is not the same question for every row.
 *
 * EFFECT_RESTART is a real requirement rather than caution: CR0's ET bit is
 * set from whether a coprocessor exists, and only at reset, and the BIOS
 * reads the memory size and the equipment byte out of the CMOS when it posts.
 * The processor generation is softer - CPUID is the only thing that reads it
 * - but software asks once, at startup, so changing it under a running DOS
 * would only make the machine disagree with what is already installed.
 *
 * EFFECT_DEVICE is immediate and complete in one direction only.  The enable
 * flags gate both the port responses and the mixer, so switching a device off
 * silences it at once; switching one on cannot make a program that has
 * already chosen its sound card go and look again.
 */
typedef enum {
    EFFECT_NOW,       /* host side: nothing in the guest has to agree */
    EFFECT_DEVICE,    /* the hardware changes under whatever is running */
    EFFECT_RESTART
} RowEffect;

typedef enum {
    ROW_HEADING,
    ROW_TOGGLE,
    ROW_CHOICE,           /* one of a list of numbers */
    ROW_VOLUME
} RowKind;

typedef struct {
    RowKind kind;
    /* Null when the label depends on the rest of the configuration; see
     * row_label(). */
    const char *label;
    int (*get)(void);
    void (*set)(int);
    const int *options;
    int option_count;
    RowEffect effect;
} Row;

static const int cpu_options[] = { 3, 4, 5 };
static const int mouse_speed_options[] = { 25, 50, 75, 100, 125, 150, 200, 300, 400 };
static const int power_restore_options[] = { POWER_RESTORE_OFF, POWER_RESTORE_ON, POWER_RESTORE_LAST };
static const int power_button_options[] = { 0, 1 };

static const Row rows[] = {
    { ROW_HEADING, "Machine",              0, 0, 0, 0, EFFECT_NOW },
    /* Null options: the sizes depend on the processor, because it is the
     * board rather than the chip that limits them.  Fetched when the row is
     * used; see row_options(). */
    { ROW_CHOICE,  "Memory",               config_get_mem_size_mb, config_set_mem_size_mb,
                                           0, 0, EFFECT_RESTART },
    { ROW_CHOICE,  "Video memory",         config_get_vga_mem_kb, config_set_vga_mem_kb,
                                           0, 0, EFFECT_RESTART },
    { ROW_CHOICE,  "Processor",            config_get_cpu_gen, config_set_cpu_gen,
                                           cpu_options, 3, EFFECT_RESTART },
    { ROW_TOGGLE,  0,                      config_get_fpu, config_set_fpu, 0, 0, EFFECT_RESTART },

    { ROW_HEADING, "Sound",                0, 0, 0, 0, EFFECT_NOW },
    { ROW_VOLUME,  "Volume",               config_get_volume, config_set_volume, 0, 0, EFFECT_NOW },
    { ROW_TOGGLE,  "PC speaker",           config_get_pcspeaker, config_set_pcspeaker, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "AdLib",                config_get_adlib, config_set_adlib, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Sound Blaster",        config_get_soundblaster, config_set_soundblaster, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Roland MPU-401",       config_get_mpu401, config_set_mpu401, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Tandy 3-voice",        config_get_tandy, config_set_tandy, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Covox on LPT2",        config_get_covox, config_set_covox, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Disney Sound Source",  config_get_dss, config_set_dss, 0, 0, EFFECT_DEVICE },

    { ROW_HEADING, "Input",                0, 0, 0, 0, EFFECT_NOW },
    { ROW_TOGGLE,  "Mouse",                config_get_mouse, config_set_mouse, 0, 0, EFFECT_DEVICE },
    { ROW_CHOICE,  "Mouse speed",          config_get_mouse_speed, config_set_mouse_speed,
                                           mouse_speed_options, 9, EFFECT_NOW },
    { ROW_TOGGLE,  "Invert mouse Y",       config_get_mouse_invert_y, config_set_mouse_invert_y, 0, 0, EFFECT_NOW },
    { ROW_TOGGLE,  "Joystick",             config_get_joystick, config_set_joystick, 0, 0, EFFECT_DEVICE },

    { ROW_HEADING, "Power",                0, 0, 0, 0, EFFECT_NOW },
    { ROW_CHOICE,  "When power returns",   config_get_power_restore, config_set_power_restore,
                                           power_restore_options, 3, EFFECT_NOW },
    { ROW_CHOICE,  "Power button",         config_get_power_button, config_set_power_button,
                                           power_button_options, 2, EFFECT_NOW },
};
#define ROW_COUNT ((int)(sizeof rows / sizeof rows[0]))

#define PANEL_X  13
#define PANEL_Y  1
#define PANEL_W  54
#define PANEL_H  24
/* The rows between the title and the two note lines above the footer; the
 * list scrolls when there are more. */
#define LIST_TOP   (PANEL_Y + 2)
#define LIST_ROWS  (PANEL_Y + PANEL_H - 4 - LIST_TOP)

static State state;
static int cursor = 1;                 /* row 0 is a heading */
static int scroll;                     /* the first row on screen */
static bool restart_requested;
static int mem_before_confirm;         /* to put back if the answer is no */

bool settingsui_restart_requested(void) { return restart_requested; }
void settingsui_clear_restart(void) { restart_requested = false; }

static void draw(void);

void settingsui_init(void) { state = STATE_CLOSED; cursor = 1; scroll = 0; }

bool settingsui_is_open(void) { return state != STATE_CLOSED; }

void settingsui_open(void)
{
    if (state != STATE_CLOSED) return;
    state = STATE_MAIN;
    osd_show();
    draw();
}

void settingsui_close(void)
{
    state = STATE_CLOSED;
    osd_hide();
}

/* ------------------------------------------------------------------------ */

/*
 * What the floating-point row is called depends on the processor, because the
 * part it describes does: a separate chip beside a 386, the difference
 * between a 486SX and a 486DX, and not a choice at all on a Pentium.
 */
static const char *row_label(const Row *r)
{
    if (r->label) return r->label;
    switch (config_get_cpu_gen()) {
    case 3:  return "387 coprocessor";
    case 4:  return "Integrated FPU (486DX)";
    default: return "Integrated FPU";
    }
}

static const int *row_options(const Row *r, int *count)
{
    if (r->options) { *count = r->option_count; return r->options; }
    if (r->set == config_set_vga_mem_kb) return config_vga_mem_options(count);
    return config_mem_options(count);
}

static int fpu_is_fixed(const Row *r)
{
    return r->get == config_get_fpu && config_get_cpu_gen() >= 5;
}

static const char *cpu_name(int gen)
{
    switch (gen) {
    case 3:  return "386";
    case 4:  return "486";
    case 5:  return "Pentium";
    default: return "?";
    }
}

static void format_value(const Row *r, char *out, size_t cap)
{
    const int v = r->get ? r->get() : 0;
    switch (r->kind) {
    case ROW_TOGGLE:
        if (fpu_is_fixed(r)) snprintf(out, cap, "always");
        else                 snprintf(out, cap, "%s", v ? "on" : "off");
        break;
    case ROW_VOLUME: {
        /* A bar reads faster than a number for something you adjust by ear. */
        char bar[20];
        const int filled = (v * 12 + 8) / 16;
        for (int i = 0; i < 12; i++) bar[i] = i < filled ? '\xDB' : '\xB0';
        bar[12] = 0;
        snprintf(out, cap, "%s %2d", bar, v);
        break;
    }
    case ROW_CHOICE:
        if (r->set == config_set_cpu_gen)
            snprintf(out, cap, "%s", cpu_name(v));
        else if (r->set == config_set_vga_mem_kb) {
            /* Kilobytes below a megabyte, because 256 KB is the size this
             * card is remembered by and "0 MB" would be nonsense. */
            if (v < 1024) snprintf(out, cap, "%d KB", v);
            else          snprintf(out, cap, "%d MB", v / 1024);
        }
        else if (r->set == config_set_mouse_speed)
            snprintf(out, cap, "%d %%", v);
        else if (r->set == config_set_power_restore)
            snprintf(out, cap, "%s", v == POWER_RESTORE_OFF ? "stay off" :
                                     v == POWER_RESTORE_ON  ? "power on" : "last state");
        else if (r->set == config_set_power_button)
            snprintf(out, cap, "%s", v ? "hold 4 s to switch off" : "press to switch off");
        else
            snprintf(out, cap, "%d MB", v);
        break;
    default:
        out[0] = 0;
        break;
    }
}

static void draw(void)
{
    osd_clear();
    osd_draw_box_titled(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, "Settings",
                        OSD_ATTR(OSD_WHITE, OSD_LIGHTBLUE));

    const int x = PANEL_X + 2;

    /* Keep the cursor on screen, and the heading above it with it. */
    const int want = cursor > 0 && rows[cursor - 1].kind == ROW_HEADING ? cursor - 1 : cursor;
    if (want < scroll) scroll = want;
    if (cursor >= scroll + LIST_ROWS) scroll = cursor - LIST_ROWS + 1;
    if (scroll > 0)
        osd_putchar(PANEL_X + PANEL_W - 2, LIST_TOP, '\x1E', OSD_ATTR_HINT);
    if (scroll + LIST_ROWS < ROW_COUNT)
        osd_putchar(PANEL_X + PANEL_W - 2, LIST_TOP + LIST_ROWS - 1, '\x1F', OSD_ATTR_HINT);

    for (int i = scroll; i < ROW_COUNT && i < scroll + LIST_ROWS; i++) {
        const Row *r = &rows[i];
        const int y = LIST_TOP + i - scroll;

        if (r->kind == ROW_HEADING) {
            osd_print(x, y, row_label(r), OSD_ATTR(OSD_YELLOW, OSD_BLUE));
            continue;
        }

        const bool selected = (i == cursor);
        const uint8_t label_attr = selected ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;
        const uint8_t value_attr = selected ? OSD_ATTR_SELECTED : OSD_ATTR_VALUE;

        if (selected) {
            /* The bar runs the width of the list so the rounded ends line up
             * with the panel rather than with the longest label. */
            for (int c = x - 1; c < PANEL_X + PANEL_W - 2; c++)
                osd_putchar(c, y, ' ', OSD_ATTR_SELECTED);
        }

        osd_print(x + 1, y, row_label(r), label_attr);

        char value[40];
        format_value(r, value, sizeof value);
        int vx = PANEL_X + PANEL_W - 4 - (int)strlen(value);
        osd_print(vx, y, value, value_attr);

        if (selected) {
            osd_putchar(vx - 2, y, '\x11', value_attr);   /* left arrow  */
            osd_putchar(PANEL_X + PANEL_W - 3, y, '\x10', value_attr);
        }
        if (r->effect == EFFECT_RESTART)
            osd_putchar(x, y, '\x07', selected ? OSD_ATTR_SELECTED
                                               : OSD_ATTR(OSD_BROWN, OSD_BLUE));
    }

    const int footer = PANEL_Y + PANEL_H - 2;
    osd_print(x, footer,
              "\x18\x19 choose   \x1B\x1A change   Enter save   Esc close",
              OSD_ATTR_HINT);

    /*
     * Only the rows that need the machine restarted say anything.  The
     * others were labelled too - "applies now", and a note that a running
     * program will not re-detect a device - and at a glance that reads as
     * part of the list rather than as a remark about the row under the
     * cursor.  A line that is always there stops being read.
     */
    if (rows[cursor].effect == EFFECT_RESTART)
        osd_print(x, footer - 2, "\x07 takes effect when the machine restarts",
                  OSD_ATTR(OSD_BROWN, OSD_BLUE));

    if (config_has_changes())
        osd_print(x, footer - 1, "changed, not yet saved", OSD_ATTR_HINT);

    if (state == STATE_SAVED) {
        const int w = 34, h = 3;
        const int dx = (OSD_COLS - w) / 2, dy = 10;
        osd_draw_box(dx, dy, w, h, OSD_ATTR(OSD_WHITE, OSD_GREEN));
        osd_print_center(dy + 1, "Saved to 386/config.ini",
                         OSD_ATTR(OSD_WHITE, OSD_BLUE));
    } else if (state == STATE_CONFIRM_RESTART) {
        const int w = 50, h = 4;
        const int dx = (OSD_COLS - w) / 2, dy = 9;
        osd_draw_box(dx, dy, w, h, OSD_ATTR(OSD_WHITE, OSD_BROWN));
        osd_print_center(dy + 1, "Saved. Restart the machine now?",
                         OSD_ATTR(OSD_WHITE, OSD_BLUE));
        osd_print_center(dy + 2, "Y restart    N keep running",
                         OSD_ATTR_HINT);
    } else if (state == STATE_CONFIRM_BIGMEM) {
        /*
         * Measured, not guessed: MEM reports the same 66,057,376 bytes of
         * extended memory whether the machine was built with 64 MB or with
         * 128.  The limit is HIMEM.SYS's own, and a Pentium board of the
         * period with 128 MB fitted behaved in exactly this way under DOS.
         * Worth asking about rather than leaving to look like a fault.
         */
        const int w = 52, h = 5;
        const int dx = (OSD_COLS - w) / 2, dy = 9;
        osd_draw_box(dx, dy, w, h, OSD_ATTR(OSD_WHITE, OSD_RED));
        osd_print_center(dy + 1, "DOS reaches only the first 64 MB",
                         OSD_ATTR(OSD_WHITE, OSD_BLUE));
        osd_print_center(dy + 2, "The rest is fitted and unreachable",
                         OSD_ATTR_HINT);
        osd_print_center(dy + 3, "Y keep it    N go back to 64 MB",
                         OSD_ATTR_HINT);
    }
}

/* ------------------------------------------------------------------------ */

static void move_cursor(int delta)
{
    int i = cursor;
    for (int guard = 0; guard < ROW_COUNT; guard++) {
        i += delta;
        if (i < 0) i = ROW_COUNT - 1;
        if (i >= ROW_COUNT) i = 0;
        if (rows[i].kind != ROW_HEADING) { cursor = i; return; }
    }
}

static void change(int delta)
{
    const Row *r = &rows[cursor];
    if (!r->get || !r->set) return;
    if (fpu_is_fixed(r)) return;
    const int v = r->get();

    switch (r->kind) {
    case ROW_TOGGLE:
        r->set(!v);
        break;
    case ROW_VOLUME: {
        int n = v + delta;
        if (n < 0) n = 0;
        if (n > 16) n = 16;
        r->set(n);
        break;
    }
    case ROW_CHOICE: {
        if (r->get == config_get_mem_size_mb) mem_before_confirm = v;
        int n = 0;
        const int *opt = row_options(r, &n);
        int idx = 0;
        for (int i = 0; i < n; i++)
            if (opt[i] == v) { idx = i; break; }
        idx += delta;
        if (idx < 0) idx = n - 1;
        if (idx >= n) idx = 0;
        r->set(opt[idx]);
        /* Ask on the way past 64 MB, not on every press above it, so that
         * stepping from 128 to 256 does not repeat the question. */
        if (r->get == config_get_mem_size_mb && opt[idx] > 64 && v <= 64)
            state = STATE_CONFIRM_BIGMEM;
        break;
    }
    default:
        break;
    }
}

bool settingsui_handle_key(int keycode, bool is_down)
{
    if (state == STATE_CLOSED) return false;
    if (!is_down) return true;

    if (state == STATE_SAVED) {           /* any key dismisses it */
        state = STATE_MAIN;
        draw();
        return true;
    }

    if (state == STATE_CONFIRM_RESTART) {
        if (keycode == KEY_Y || keycode == KEY_ENTER) {
            restart_requested = true;
            config_clear_changes();
            settingsui_close();
        } else if (keycode == KEY_N || keycode == KEY_ESC) {
            state = STATE_MAIN;
            config_clear_changes();
            draw();
        }
        return true;
    }

    if (state == STATE_CONFIRM_BIGMEM) {
        if (keycode == KEY_N || keycode == KEY_ESC)
            config_set_mem_size_mb(mem_before_confirm > 64 ? 64 : mem_before_confirm);
        if (keycode == KEY_Y || keycode == KEY_N ||
            keycode == KEY_ENTER || keycode == KEY_ESC) {
            state = STATE_MAIN;
            draw();
        }
        return true;
    }

    switch (keycode) {
    case KEY_UP:    move_cursor(-1); draw(); break;
    case KEY_DOWN:  move_cursor(+1); draw(); break;
    case KEY_LEFT:  change(-1); draw(); break;
    case KEY_RIGHT: change(+1); draw(); break;
    case KEY_SPACE: change(+1); draw(); break;
    case KEY_ENTER:
        if (config_save_all()) {
            state = config_needs_restart() ? STATE_CONFIRM_RESTART : STATE_SAVED;
            if (state == STATE_SAVED) config_clear_changes();
        }
        draw();
        break;
    case KEY_ESC:
        settingsui_close();
        break;
    default:
        break;
    }
    return true;
}
