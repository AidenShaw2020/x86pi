/*
 * The settings behind the on-screen menus.  See config_save.h.
 */

#include "config_save.h"
#include "pc.h"
#include "disk.h"
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The same file the kernel reads at boot, on the same mount. */
#define CONFIG_PATH "SD:/386/config.ini"

static PC *bound_pc;
static bool changed;
static bool restart_needed;

/* Defaults match what pc_new() sets up, so a machine with no [X86Pi]
 * section in its ini behaves exactly as it did before there was a menu. */
static int cfg_volume       = 16;
static int cfg_mem_mb       = 8;
static int cfg_vga_kb       = 4096;
static int cfg_cpu_gen      = 4;
static int cfg_fpu          = 0;
static int cfg_pcspeaker    = 1;
static int cfg_adlib        = 1;
static int cfg_soundblaster = 1;
static int cfg_mpu401       = 1;
static int cfg_tandy        = 0;
static int cfg_covox        = 1;
static int cfg_dss          = 0;
static int cfg_mouse        = 1;
static int cfg_mouse_inv_y  = 0;
static int cfg_mouse_speed  = 100;   /* percent */
static int cfg_joystick     = 0;
static int cfg_power_restore = POWER_RESTORE_ON;
static int cfg_power_button  = 0;

/* Read by the audio mixer and the mouse handler on the host side. */
int g_audio_volume = 16;
int g_mouse_invert_y = 0;
int g_mouse_speed = 100;

void config_bind_pc(PC *pc) { bound_pc = pc; }

static void apply_live(void)
{
    g_audio_volume = cfg_volume;
    g_mouse_invert_y = cfg_mouse_inv_y;
    g_mouse_speed = cfg_mouse_speed;
    if (!bound_pc) return;
    bound_pc->pcspk_enabled    = cfg_pcspeaker;
    bound_pc->adlib_enabled    = cfg_adlib;
    bound_pc->sb16_enabled     = cfg_soundblaster;
    bound_pc->mpu401_enabled   = cfg_mpu401;
    bound_pc->tandy_enabled    = cfg_tandy;
    bound_pc->covox_enabled    = cfg_covox;
    bound_pc->dss_enabled      = cfg_dss;
    bound_pc->mouse_enabled    = cfg_mouse;
    bound_pc->joystick_enabled = cfg_joystick;
}

void config_apply_all(void) { apply_live(); }

bool config_has_changes(void)   { return changed; }
bool config_needs_restart(void) { return restart_needed; }
void config_clear_changes(void) { changed = false; restart_needed = false; }

#define LIVE_SETTING(name, var)                       \
    int config_get_##name(void) { return var; }       \
    void config_set_##name(int v)                     \
    {                                                 \
        if (var == v) return;                         \
        var = v; changed = true; apply_live();        \
    }

LIVE_SETTING(volume,       cfg_volume)
LIVE_SETTING(pcspeaker,    cfg_pcspeaker)
LIVE_SETTING(adlib,        cfg_adlib)
LIVE_SETTING(soundblaster, cfg_soundblaster)
LIVE_SETTING(mpu401,       cfg_mpu401)
LIVE_SETTING(tandy,        cfg_tandy)
LIVE_SETTING(covox,        cfg_covox)
LIVE_SETTING(dss,          cfg_dss)
LIVE_SETTING(mouse,        cfg_mouse)
LIVE_SETTING(mouse_invert_y, cfg_mouse_inv_y)
LIVE_SETTING(mouse_speed,  cfg_mouse_speed)
LIVE_SETTING(joystick,     cfg_joystick)
LIVE_SETTING(power_restore, cfg_power_restore)
LIVE_SETTING(power_button, cfg_power_button)

/* These two describe the machine rather than its devices, so they cannot
 * take effect until it is built again. */
static const int mem_386[]  = { 1, 2, 4, 8, 16, 32, 64 };
static const int mem_486[]  = { 4, 8, 16, 32, 64, 128 };
static const int mem_586[]  = { 8, 16, 32, 64, 128, 256 };

const int *config_mem_options(int *count)
{
    switch (cfg_cpu_gen) {
    case 3:  *count = (int)(sizeof mem_386 / sizeof *mem_386); return mem_386;
    case 4:  *count = (int)(sizeof mem_486 / sizeof *mem_486); return mem_486;
    default: *count = (int)(sizeof mem_586 / sizeof *mem_586); return mem_586;
    }
}

/* Bring the size into what the new generation's boards could hold, keeping as
 * much of it as that allows. */
static void clamp_mem_to_generation(void)
{
    int n = 0;
    const int *opt = config_mem_options(&n);
    if (cfg_mem_mb < opt[0]) { cfg_mem_mb = opt[0]; return; }
    if (cfg_mem_mb > opt[n - 1]) { cfg_mem_mb = opt[n - 1]; return; }
    for (int i = 0; i < n; i++) if (opt[i] == cfg_mem_mb) return;
    for (int i = n - 1; i >= 0; i--) if (opt[i] < cfg_mem_mb) { cfg_mem_mb = opt[i]; return; }
}

int config_get_mem_size_mb(void) { return cfg_mem_mb; }
void config_set_mem_size_mb(int mb)
{
    if (cfg_mem_mb == mb) return;
    cfg_mem_mb = mb; changed = true; restart_needed = true;
}

/*
 * The video card's memory, in kilobytes.
 *
 * Powers of two only: the VESA bank register is masked with the size in
 * 64 KB banks less one, so anything else would send a bank's worth of writes
 * somewhere the card did not intend.  256 KB is what a real VGA had and is
 * enough for the standard modes; the larger sizes are what the VESA modes and
 * a second display page need, and Red Alert wanted more than a megabyte
 * before it would show anything at all.
 */
static const int vga_kb_options[] = { 256, 512, 1024, 2048, 4096, 8192 };

const int *config_vga_mem_options(int *count)
{
    *count = (int)(sizeof vga_kb_options / sizeof *vga_kb_options);
    return vga_kb_options;
}

int config_get_vga_mem_kb(void) { return cfg_vga_kb; }
void config_set_vga_mem_kb(int kb)
{
    if (cfg_vga_kb == kb) return;
    cfg_vga_kb = kb; changed = true; restart_needed = true;
}

int config_get_fpu(void) { return cfg_fpu; }
void config_set_fpu(int enabled)
{
    if (cfg_fpu == enabled) return;
    cfg_fpu = enabled; changed = true; restart_needed = true;
}

int config_get_cpu_gen(void) { return cfg_cpu_gen; }
void config_set_cpu_gen(int gen)
{
    if (cfg_cpu_gen == gen) return;
    cfg_cpu_gen = gen; changed = true; restart_needed = true;
    /*
     * Every Pentium has a floating-point unit.  There is no part to
     * correspond to one without, and CPUID would then report a family of 5
     * with its FPU bit clear - which software is entitled to treat as
     * impossible, and would answer by using x87 instructions that this
     * machine would quietly ignore.  A 486 is different: the SX really did
     * ship without one, so that choice stays open.
     */
    if (gen >= 5) cfg_fpu = 1;
    clamp_mem_to_generation();
}

void config_set_from_ini(const char *name, const char *value)
{
    const int v = atoi(value);
    if      (!strcmp(name, "volume"))       cfg_volume = v < 0 ? 0 : v > 16 ? 16 : v;
    else if (!strcmp(name, "pcspeaker"))    cfg_pcspeaker = v != 0;
    else if (!strcmp(name, "adlib"))        cfg_adlib = v != 0;
    else if (!strcmp(name, "soundblaster")) cfg_soundblaster = v != 0;
    else if (!strcmp(name, "mpu401"))       cfg_mpu401 = v != 0;
    else if (!strcmp(name, "tandy"))        cfg_tandy = v != 0;
    else if (!strcmp(name, "covox"))        cfg_covox = v != 0;
    else if (!strcmp(name, "dss"))          cfg_dss = v != 0;
    else if (!strcmp(name, "mouse"))        cfg_mouse = v != 0;
    else if (!strcmp(name, "mouse_invert_y")) cfg_mouse_inv_y = v != 0;
    else if (!strcmp(name, "mouse_speed"))  cfg_mouse_speed = v < 25 ? 25 : v > 400 ? 400 : v;
    else if (!strcmp(name, "joystick"))     cfg_joystick = v != 0;
    else if (!strcmp(name, "power_restore")) cfg_power_restore = v < 0 || v > 2 ? POWER_RESTORE_ON : v;
    else if (!strcmp(name, "power_button")) cfg_power_button = v != 0;
}

/* The memory size and processor come from the [pc] section the kernel already
 * parses, so the menu starts from what the machine was actually built with
 * rather than from the defaults above. */
void config_note_machine(int mem_mb, int cpu_gen)
{
    if (mem_mb > 0) cfg_mem_mb = mem_mb;
    if (cpu_gen > 0) cfg_cpu_gen = cpu_gen;
}

void config_note_vga_mem(int kb)
{
    int n = 0;
    const int *opt = config_vga_mem_options(&n);
    /* Only a size the menu can also express, so that opening the menu and
     * leaving it without touching anything cannot quietly change the
     * machine. */
    for (int i = 0; i < n; i++)
        if (opt[i] == kb) { cfg_vga_kb = kb; return; }
}

void config_note_fpu(int fpu) { cfg_fpu = fpu != 0; }

/* ------------------------------------------------------------------------ */
/* Writing it back                                                           */
/* ------------------------------------------------------------------------ */

static bool put(FIL *fp, const char *line)
{
    UINT bw = 0;
    const UINT n = (UINT)strlen(line);
    return f_write(fp, line, n, &bw) == FR_OK && bw == n;
}

static bool putf(FIL *fp, const char *key, int value)
{
    char line[64];
    snprintf(line, sizeof line, "%s=%d\n", key, value);
    return put(fp, line);
}

/*
 * The file is rewritten whole rather than edited, because there is no reader
 * here that could preserve what it does not understand.  Everything the
 * kernel parses is written back, so a save never loses the machine's
 * definition - that was the failure mode worth designing against.
 */
static bool write_config(void)
{
    FIL fp;
    char line[128];

    if (f_open(&fp, CONFIG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;

    bool ok = true;
    ok &= put(&fp, "; Written by the on-screen settings menu.\n\n[pc]\n");
    snprintf(line, sizeof line, "mem=%dM\n", cfg_mem_mb);
    ok &= put(&fp, line);
    snprintf(line, sizeof line, "vga_mem=%dK\n", cfg_vga_kb);
    ok &= put(&fp, line);
    ok &= putf(&fp, "cpu", cfg_cpu_gen);
    ok &= putf(&fp, "fpu", cfg_fpu);
    ok &= put(&fp, "bios=bios.bin\n");
    ok &= put(&fp, "vga_bios=vgabios.bin\n");

    ok &= put(&fp, "\n; Disk images\n");
    for (int i = 0; i < 2; i++) {
        const char *f = fdd_get_filename(i);
        if (f && f[0]) {
            snprintf(line, sizeof line, "fd%c=%s\n", 'a' + i, f);
            ok &= put(&fp, line);
        }
    }
    for (int i = 0; i < 4; i++) {
        const char *f = ata_get_filename(i);
        if (f && f[0]) {
            snprintf(line, sizeof line, "%s%c=%s\n",
                     ata_is_cdrom(i) ? "cd" : "hd", 'a' + i, f);
            ok &= put(&fp, line);
        }
    }

    ok &= put(&fp, "\n[X86Pi]\n");
    ok &= putf(&fp, "volume", cfg_volume);
    ok &= putf(&fp, "pcspeaker", cfg_pcspeaker);
    ok &= putf(&fp, "adlib", cfg_adlib);
    ok &= putf(&fp, "soundblaster", cfg_soundblaster);
    ok &= putf(&fp, "mpu401", cfg_mpu401);
    ok &= putf(&fp, "tandy", cfg_tandy);
    ok &= putf(&fp, "covox", cfg_covox);
    ok &= putf(&fp, "dss", cfg_dss);
    ok &= putf(&fp, "mouse", cfg_mouse);
    ok &= putf(&fp, "mouse_invert_y", cfg_mouse_inv_y);
    ok &= putf(&fp, "mouse_speed", cfg_mouse_speed);
    ok &= putf(&fp, "joystick", cfg_joystick);
    ok &= putf(&fp, "power_restore", cfg_power_restore);
    ok &= putf(&fp, "power_button", cfg_power_button);

    return f_close(&fp) == FR_OK && ok;
}

bool config_save_all(void)
{
    const bool ok = write_config();
    if (ok) changed = false;
    return ok;
}

bool config_save_disks(void) { return write_config(); }
