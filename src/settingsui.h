#pragma once
/*
 * The settings menu, opened with Win+F11.  See settingsui.c.
 */
#include <stdbool.h>

void settingsui_init(void);
void settingsui_open(void);
void settingsui_close(void);
bool settingsui_is_open(void);

/* Returns true when the menu consumed the key, which it does for every key
 * while it is open. */
bool settingsui_handle_key(int keycode, bool is_down);

/* Set when the user asked for the machine to be rebuilt after saving.  The
 * kernel services it with pc_set_machine() and a guest reset. */
bool settingsui_restart_requested(void);
void settingsui_clear_restart(void);
