/**
 * X86Pi - i386 PC emulator
 *
 * Disk UI - on-screen disk manager for inserting/ejecting disk images
 * at runtime. Triggered by Win+F12 hotkey.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#include "diskui.h"
#include "osd.h"
#include "uikeys.h"
#include "disk.h"
#include "blockdev.h"
#include "config_save.h"
#include "ff.h"
#include <string.h>
#include <strings.h>  // For strcasecmp
#include <stdio.h>

// Menu states
typedef enum {
    MENU_CLOSED,
    MENU_MAIN,          // Drive selection
    MENU_FILE_BROWSER   // File selection for a drive
} MenuState;

// Drive table — matches DiskUIDrive enum order from diskui.h
// NOTE: update diskui.h (DriveInfo, DiskUIDrive, DRIVE_TOTAL) to match
static const DriveInfo drive_table[DRIVE_TOTAL] = {
    { "FDD-0",  "Floppy"   },  // DRIVE_FDD0
    { "FDD-1",  "Floppy"   },  // DRIVE_FDD1
    { "ATA0-0", "ATA Disk" },  // DRIVE_ATA0_0
    { "ATA0-1", "ATA Disk" },  // DRIVE_ATA0_1
    { "ATA1-0", "ATA Disk" },  // DRIVE_ATA1_0
    { "ATA1-1", "ATA Disk" },  // DRIVE_ATA1_1
};

// File listing (reduced size to save SRAM)
#define MAX_FILES        24
#define MAX_FILENAME_LEN 32

// Menu state
static MenuState menu_state   = MENU_CLOSED;
static int selected_row       = 0;  // Current row in main menu (0..DRIVE_TOTAL-1)
static int selected_file      = 0;
static int file_scroll_offset = 0;
// selected_row intentionally persists between open/close — preserves position

// Pending changes: track what the user wants for each drive
// Empty string = eject, non-empty = new filename
static char pending_filename[DRIVE_TOTAL][MAX_FILENAME_LEN];
static bool pending_changed[DRIVE_TOTAL];  // true if user modified this drive
static bool reboot_required;               // true if any ATA drive was changed
static char file_list[MAX_FILES][MAX_FILENAME_LEN];
static int  file_count   = 0;

// Bottom row index: either "Save and Exit" or "Save and Reboot"
#define MAIN_MENU_ROWS  (DRIVE_TOTAL + 1)
#define ROW_ACTION       DRIVE_TOTAL

// UI dimensions — height adapts to number of rows
#define MENU_X      10
#define MENU_Y      4
#define MENU_W      60
#define MENU_H      (8 + MAIN_MENU_ROWS)  // border(2) + rows + blank + notification + blank + action + border(2)

#define FILE_X      12
#define FILE_Y      7
#define FILE_W      56
#define FILE_H      14
#define FILE_VISIBLE (FILE_H - 4)

// Forward declarations
static void draw_main_menu(void);
static void draw_file_browser(void);
static void scan_disk_images(int drive_idx);
static void select_file(void);
static void eject_pending(void);
static void apply_and_close(void);
static void reset_pending(void);

/* Read and cleared by the kernel loop, which owns resetting the guest. */
bool diskui_reset_requested_flag;

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static const char *get_drive_filename(int drive_idx) {
    if (drive_idx < 2) {
        return fdd_get_filename(drive_idx);      // FDD-0 / FDD-1
    } else {
        return ata_get_filename(drive_idx - 2);  // ATA0-0 .. ATA1-1
    }
}

// Get the display filename for a drive (pending or current)
static const char *get_display_filename(int drive_idx) {
    if (pending_changed[drive_idx]) {
        if (pending_filename[drive_idx][0] == '\0') return NULL;  // pending eject
        return pending_filename[drive_idx];
    }
    return get_drive_filename(drive_idx);
}

/* The extension, or NULL.  Circle has no such routine of its own, and the
 * toolchain's header declares the C library one as a C++ overload pair,
 * so it cannot simply be supplied in the compatibility layer. */
static const char *file_ext(const char *name)
{
    const char *dot = 0;
    for (; *name; ++name) if (*name == '.') dot = name;
    return dot;
}

static bool file_is_iso(const char *filename) {
    if (!filename) return false;
    const char *ext = file_ext(filename);
    if (!ext) return false;
    if (strcasecmp(ext, ".iso") == 0) return true;
    return false;
}

// Returns true if the extension is valid for the given drive type.
static bool ext_accepted_for_drive(const char *ext, int drive_idx) {
    /* A CD image only makes sense on the ATA channels. */
    if (strcasecmp(ext, ".iso") == 0) return drive_idx >= 2;
    if (strcasecmp(ext, ".img") == 0) return true;
    if (strcasecmp(ext, ".ima") == 0) return true;
    if (strcasecmp(ext, ".vhd") == 0) return true;
    /*
     * ".bin" is deliberately not accepted.  The system and video BIOS live
     * in this same directory under that extension, so offering it put
     * bios.bin at the top of the list of things to put in drive A - and
     * mounting the machine's own firmware as a floppy is not a mistake
     * worth making easy.
     */
    return false;
}

static void reset_pending(void) {
    for (int i = 0; i < DRIVE_TOTAL; i++) {
        pending_changed[i] = false;
        pending_filename[i][0] = '\0';
    }
    reboot_required = false;
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

void diskui_init(void) {
    osd_init();
    menu_state    = MENU_CLOSED;
    selected_row  = 0;
    selected_file = 0;
    file_count    = 0;
    reset_pending();
}

void diskui_open(void) {
    if (menu_state != MENU_CLOSED) return;

    reset_pending();
    menu_state = MENU_MAIN;
    osd_clear();
    osd_show();
    draw_main_menu();
}

void diskui_close(void) {
    menu_state = MENU_CLOSED;
    reset_pending();
    osd_hide();
}

bool diskui_is_open(void) {
    return menu_state != MENU_CLOSED;
}

const DriveInfo* diskui_get_drive_info(DiskUIDrive drive) {
    if (drive < 0 || drive >= DRIVE_TOTAL) return NULL;
    return &drive_table[drive];
}

// --------------------------------------------------------------------------
// Drawing
// --------------------------------------------------------------------------

static void draw_main_menu(void) {

    osd_draw_box(MENU_X, MENU_Y, MENU_W, MENU_H, OSD_ATTR_BORDER);
    osd_fill(MENU_X + 1, MENU_Y + 1, MENU_W - 2, MENU_H - 2, ' ', OSD_ATTR_NORMAL);
    osd_print_center(MENU_Y, " Disk Manager ", OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    // Drive rows
    for (int i = 0; i < DRIVE_TOTAL; i++) {
        int y = MENU_Y + 2 + i;
        uint8_t attr = (i == selected_row) ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;

        osd_fill(MENU_X + 2, y, MENU_W - 4, 1, ' ', attr);

        char line[64];
        /* What is actually in the slot, rather than what the table guessed.
         * An ATA slot holds a CD-ROM as readily as a disk - the USB CD drive
         * is the secondary master - and calling it a disk is simply wrong. */
        const char *type = drive_table[i].type_name;
        if (i >= DRIVE_ATA0_0 && ata_is_cdrom((uint8_t)(i - DRIVE_ATA0_0)))
            type = "CD-ROM";
        snprintf(line, sizeof(line), "[%-7s] %-10s", drive_table[i].label, type);
        osd_print(MENU_X + 2, y, line, attr);

        const char *filename = get_display_filename(i);
        if (filename) {
            char truncated[24];
            strncpy(truncated, filename, 23);
            truncated[23] = '\0';
            osd_print(MENU_X + 22, y, truncated, attr);
        } else {
            osd_print(MENU_X + 22, y, "[empty]", OSD_ATTR(OSD_LIGHTGRAY, OSD_BLUE));
        }

        if (filename) {
            osd_print(MENU_X + MENU_W - 12, y, "[Eject] ", attr);
        } else {
            osd_print(MENU_X + MENU_W - 12, y, "[Select]", attr);
        }
    }

    // Blank line + reboot notification + blank line
    int notify_y = MENU_Y + 3 + DRIVE_TOTAL;
    if (reboot_required) {
        osd_print_center(notify_y, "! Reboot required for HDD changes !", OSD_ATTR(OSD_WHITE, OSD_RED));
    }

    // Action row
    int action_y = MENU_Y + 5 + DRIVE_TOTAL;
    {
        uint8_t attr = (selected_row == ROW_ACTION) ? OSD_ATTR_SELECTED : OSD_ATTR_HIGHLIGHT;
        osd_fill(MENU_X + 2, action_y, MENU_W - 4, 1, ' ', attr);
        if (reboot_required) {
            osd_print_center(action_y, "[ Save and Reboot ]", attr);
        } else {
            osd_print_center(action_y, "[ Save and Exit ]", attr);
        }
    }

    int help_y = MENU_Y + MENU_H - 2;
    osd_print_center(help_y, "\x18/\x19: Navigate   Enter: Select/Eject   Esc: Cancel", OSD_ATTR_HIGHLIGHT);
}

static void draw_file_browser(void) {

    osd_draw_box(FILE_X, FILE_Y, FILE_W, FILE_H, OSD_ATTR_BORDER);
    osd_fill(FILE_X + 1, FILE_Y + 1, FILE_W - 2, FILE_H - 2, ' ', OSD_ATTR_NORMAL);

    char title[48];
    snprintf(title, sizeof(title), " Select Image for %s ", drive_table[selected_row].label);
    osd_print_center(FILE_Y, title, OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    int visible_files = FILE_VISIBLE;
    for (int i = 0; i < visible_files && (file_scroll_offset + i) < file_count; i++) {
        int file_idx = file_scroll_offset + i;
        int y = FILE_Y + 1 + i;
        uint8_t attr = (file_idx == selected_file) ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;

        osd_fill(FILE_X + 2, y, FILE_W - 4, 1, ' ', attr);

        if (file_idx == selected_file) {
            osd_print(FILE_X + 2, y, ">", attr);
        }
        /* "usb:ufd1" reads as nothing at all in a list of file names. */
        char shown[MAX_FILENAME_LEN + 16];
        if (strncmp(file_list[file_idx], "usb:ufd", 7) == 0)
            snprintf(shown, sizeof shown, "USB floppy drive %s",
                     file_list[file_idx] + 7);
        else
            snprintf(shown, sizeof shown, "%s", file_list[file_idx]);
        osd_print(FILE_X + 4, y, shown, attr);
    }

    if (file_scroll_offset > 0) {
        osd_putchar(FILE_X + FILE_W - 3, FILE_Y + 1, '\x1e', OSD_ATTR_HIGHLIGHT);
    }
    if (file_scroll_offset + visible_files < file_count) {
        osd_putchar(FILE_X + FILE_W - 3, FILE_Y + FILE_H - 2, '\x1f', OSD_ATTR_HIGHLIGHT);
    }

    if (file_count == 0) {
        osd_print_center(FILE_Y + FILE_H / 2, "No disk images found in 386/", OSD_ATTR_DISABLED);
    }

    int help_y = FILE_Y + FILE_H - 2;
    osd_fill(FILE_X + 1, help_y, FILE_W - 2, 1, ' ', OSD_ATTR_NORMAL);
    osd_print(FILE_X + 2, help_y, "\x18/\x19: Navigate   Enter: Select   Esc: Cancel", OSD_ATTR_HIGHLIGHT);
}

// --------------------------------------------------------------------------
// File scanning
// --------------------------------------------------------------------------

static void scan_disk_images(int drive_idx) {
    DIR dir;
    FILINFO fno;
    FRESULT res;

    file_count = 0;
    memset(file_list, 0, sizeof(file_list));

    res = f_opendir(&dir, "SD:/386");
    if (res != FR_OK) return;

    while (file_count < MAX_FILES) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;

        if (fno.fattrib & AM_DIR) continue;

        const char *ext = file_ext(fno.fname);
        if (!ext) continue;

        if (ext_accepted_for_drive(ext, drive_idx)) {
            strncpy(file_list[file_count], fno.fname, MAX_FILENAME_LEN - 1);
            file_list[file_count][MAX_FILENAME_LEN - 1] = '\0';
            file_count++;
        }
    }

    f_closedir(&dir);

    // Sort alphabetically (bubble sort)
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = 0; j < file_count - i - 1; j++) {
            if (strcasecmp(file_list[j], file_list[j + 1]) > 0) {
                char temp[MAX_FILENAME_LEN];
                strcpy(temp, file_list[j]);
                strcpy(file_list[j], file_list[j + 1]);
                strcpy(file_list[j + 1], temp);
            }
        }
    }

    /*
     * The drive itself, above the images.
     *
     * A USB floppy that the host has enumerated is offered as "usb:", which
     * is what insertdisk() understands; it goes first because it is the one
     * entry that is not a file and would otherwise be lost among them.  Only
     * for the floppy drives, which are the only ones that can have one.
     */
    if (drive_idx < 2) {
        /* There may be more than one drive, so each is offered by the name
         * the host knows it by rather than assuming the first. */
        for (int n = 2; n >= 1; n--) {
            char which[8];
            snprintf(which, sizeof which, "ufd%d", n);
            if (!blk_host_available(which) || file_count >= MAX_FILES)
                continue;
            for (int i = file_count; i > 0; i--)
                strcpy(file_list[i], file_list[i - 1]);
            snprintf(file_list[0], MAX_FILENAME_LEN, "usb:%s", which);
            file_count++;
        }
    }

    selected_file = 0;
    file_scroll_offset = 0;
}

// --------------------------------------------------------------------------
// Actions
// --------------------------------------------------------------------------

static void select_file(void) {
    if (file_count == 0 || selected_file >= file_count) return;

    int drive_idx = selected_row;
    strncpy(pending_filename[drive_idx], file_list[selected_file], MAX_FILENAME_LEN - 1);
    pending_filename[drive_idx][MAX_FILENAME_LEN - 1] = '\0';
    pending_changed[drive_idx] = true;

    if (drive_idx >= 2) reboot_required = true;

    menu_state = MENU_MAIN;
    draw_main_menu();
}

static void eject_pending(void) {
    int drive_idx = selected_row;
    pending_filename[drive_idx][0] = '\0';
    pending_changed[drive_idx] = true;

    if (drive_idx >= 2) reboot_required = true;

    draw_main_menu();
}

static void apply_and_close(void) {
    // Apply all pending changes
    for (int i = 0; i < DRIVE_TOTAL; i++) {
        if (!pending_changed[i]) continue;

        if (pending_filename[i][0] == '\0') {
            // Eject
            if (i < 2) {
                ejectdisk(i, true);
            } else {
                ejectdisk(i - 2, false);
            }
        } else {
            // Insert
            if (i < 2) {
                insertdisk(i, true, false, pending_filename[i]);
            } else {
                int ata_index = i - 2;
                bool is_cdrom = file_is_iso(pending_filename[i]);
                insertdisk(ata_index, false, is_cdrom, pending_filename[i]);
            }
        }
    }

    config_save_disks();

    /* Swapping a hard disk under a running DOS is not something the guest
     * can be expected to survive, so those changes take effect on a reset.
     * Floppies are designed to be changed while the machine runs and take
     * effect immediately. */
    if (reboot_required) diskui_reset_requested_flag = true;

    diskui_close();
}

// --------------------------------------------------------------------------
// Input handling
// --------------------------------------------------------------------------

bool diskui_handle_key(int keycode, bool is_down) {
    if (!is_down) return true;

    switch (menu_state) {
        case MENU_MAIN:
            switch (keycode) {
                case KEY_UP:
                    selected_row = (selected_row > 0) ? selected_row - 1 : MAIN_MENU_ROWS - 1;
                    draw_main_menu();
                    break;

                case KEY_DOWN:
                    selected_row = (selected_row < MAIN_MENU_ROWS - 1) ? selected_row + 1 : 0;
                    draw_main_menu();
                    break;

                case KEY_ENTER: {
                    if (selected_row == ROW_ACTION) {
                        apply_and_close();
                        break;
                    }
                    const char *filename = get_display_filename(selected_row);
                    if (filename) {
                        eject_pending();
                    } else {
                        scan_disk_images(selected_row);
                        menu_state = MENU_FILE_BROWSER;
                        draw_file_browser();
                    }
                    break;
                }

                case KEY_ESC:
                    diskui_close();
                    break;

                // Quick selection by drive number
                case KEY_A: selected_row = DRIVE_FDD0;   draw_main_menu(); break;
                case KEY_B: selected_row = DRIVE_FDD1;   draw_main_menu(); break;
                case KEY_C: selected_row = DRIVE_ATA0_0; draw_main_menu(); break;
                case KEY_D: selected_row = DRIVE_ATA0_1; draw_main_menu(); break;
                case KEY_E: selected_row = DRIVE_ATA1_0; draw_main_menu(); break;
                case KEY_F: selected_row = DRIVE_ATA1_1; draw_main_menu(); break;
            }
            break;

        case MENU_FILE_BROWSER:
            switch (keycode) {
                case KEY_UP:
                    if (file_count == 0) break;
                    if (selected_file > 0) {
                        selected_file--;
                    } else {
                        selected_file = file_count - 1;
                        file_scroll_offset = file_count - FILE_VISIBLE;
                        if (file_scroll_offset < 0) file_scroll_offset = 0;
                    }
                    if (selected_file < file_scroll_offset) {
                        file_scroll_offset = selected_file;
                    }
                    draw_file_browser();
                    break;

                case KEY_DOWN:
                    if (file_count == 0) break;
                    if (selected_file < file_count - 1) {
                        selected_file++;
                    } else {
                        selected_file = 0;
                        file_scroll_offset = 0;
                    }
                    if (selected_file >= file_scroll_offset + FILE_VISIBLE) {
                        file_scroll_offset = selected_file - FILE_VISIBLE + 1;
                    }
                    draw_file_browser();
                    break;

                case KEY_ENTER:
                    select_file();
                    break;

                case KEY_ESC:
                    menu_state = MENU_MAIN;
                    draw_main_menu();
                    break;
            }
            break;

        default:
            break;
    }

    return true;
}


