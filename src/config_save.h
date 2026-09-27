#pragma once
/*
 * The settings the on-screen menus change, and writing them back to
 * 386/config.ini on the SD card.
 *
 * Two kinds live here.  Most take effect the moment they are set, because
 * they are enable flags the PC model reads on every access; the rest - how
 * much memory the machine has, which processor it claims to be - can only be
 * applied by building the machine again, so the menu offers a restart.
 */

#include <stdbool.h>
#include "pc.h"

/* The machine the live settings act on.  Until this is called, setting a
 * value records it and changes nothing. */
void config_bind_pc(PC *pc);

/* Copy the current values onto the bound machine.  Called once after the
 * machine is built, so a saved configuration is in force from the start. */
void config_apply_all(void);

bool config_save_all(void);
bool config_save_disks(void);

/* True when something has been changed since the last save, and when that
 * something needs the machine rebuilt. */
bool config_has_changes(void);
bool config_needs_restart(void);
void config_clear_changes(void);

int  config_get_volume(void);
void config_set_volume(int level);        /* 0..16, 16 is unity gain */

int  config_get_mem_size_mb(void);
void config_set_mem_size_mb(int mb);

/*
 * The memory on the video card, in kilobytes, and the sizes it may take.
 *
 * A real VGA had 256 KB and that is all the standard modes need, but the VESA
 * modes address far more of it and a game that draws into a second display
 * page needs room for one: with 256 KB, Red Alert's 640x400 page never
 * existed and the screen stayed black.  The sizes are powers of two because
 * the bank register is masked with the size.
 */
int  config_get_vga_mem_kb(void);
void config_set_vga_mem_kb(int kb);
const int *config_vga_mem_options(int *count);

int  config_get_fpu(void);
void config_set_fpu(int enabled);        /* a 387, fitted or not */

/*
 * The memory sizes a machine of the configured generation could have been
 * built with.  A 386 board took a few megabytes and a good one thirty-two; a
 * Pentium board of the same era took ten times that.  The processor could
 * address four gigabytes throughout - it is the board that decides.
 */
const int *config_mem_options(int *count);

int  config_get_cpu_gen(void);
void config_set_cpu_gen(int gen);         /* 3 = 386, 4 = 486, 5 = 586 */

int  config_get_pcspeaker(void);
void config_set_pcspeaker(int enabled);
int  config_get_adlib(void);
void config_set_adlib(int enabled);
int  config_get_soundblaster(void);
void config_set_soundblaster(int enabled);
int  config_get_mpu401(void);
void config_set_mpu401(int enabled);
int  config_get_tandy(void);
void config_set_tandy(int enabled);
int  config_get_covox(void);
void config_set_covox(int enabled);
int  config_get_dss(void);
void config_set_dss(int enabled);

int  config_get_mouse(void);
void config_set_mouse(int enabled);
int  config_get_mouse_invert_y(void);
void config_set_mouse_invert_y(int invert);
/* How far the pointer moves for a given hand movement, in percent of
 * what the mouse reports; applied in QueueMouse() in the Circle kernel. */
int  config_get_mouse_speed(void);
void config_set_mouse_speed(int percent);
int  config_get_joystick(void);
void config_set_joystick(int enabled);

/*
 * The front panel's power button, as an ATX board's BIOS set it up.
 *
 * What the machine does when the board gets its power back: stay off until
 * the button is pressed, come straight on, or go back to however it was left
 * - which is the only one of the three that has to remember anything.
 * And whether one press switches a running machine off, or the button has
 * to be held for four seconds so that a knock cannot.
 */
enum { POWER_RESTORE_OFF = 0, POWER_RESTORE_ON = 1, POWER_RESTORE_LAST = 2 };
int  config_get_power_restore(void);
void config_set_power_restore(int mode);
int  config_get_power_button(void);
void config_set_power_button(int hold);   /* 1 = hold four seconds */

/* What the machine was actually built with, so the menu starts from that
 * rather than from the defaults. */
void config_note_machine(int mem_mb, int cpu_gen);
void config_note_vga_mem(int kb);
void config_note_fpu(int fpu);

/* Set from the ini at boot; the name is the key inside [X86Pi]. */
void config_set_from_ini(const char *name, const char *value);
