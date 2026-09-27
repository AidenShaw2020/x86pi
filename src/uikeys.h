#pragma once
/*
 * Linux keycodes, which is what both input paths hand to the menus: the USB
 * keyboard is translated from HID usages before it reaches the queue, and the
 * serial link carries these codes directly.
 */
#define KEY_ESC        1
#define KEY_1          2
#define KEY_ENTER     28
#define KEY_A         30
#define KEY_B         48
#define KEY_C         46
#define KEY_D         32
#define KEY_E         18
#define KEY_F         33
#define KEY_N         49
#define KEY_Y         21
#define KEY_SPACE     57
#define KEY_F11       87
#define KEY_F12       88
#define KEY_UP       103
#define KEY_LEFT     105
#define KEY_RIGHT    106
#define KEY_DOWN     108
#define KEY_PAGEUP   104
#define KEY_PAGEDOWN 109
#define KEY_DELETE   111
#define KEY_LEFTMETA 125
#define KEY_RIGHTMETA 126
