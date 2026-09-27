/**
 * X86Pi - i386 PC emulator
 *
 * Disk UI - on-screen disk manager for inserting/ejecting disk images
 * at runtime. Triggered by Win+F12 hotkey.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#ifndef DISKUI_H
#define DISKUI_H

#include <stdint.h>
#include <stdbool.h>
#include "uikeys.h"

// Drive types
typedef enum {
    DRIVE_FDD0 = 0,    // Floppy A:
    DRIVE_FDD1 = 1,    // Floppy B:
    // Hard Disks or CD-ROM
    DRIVE_ATA0_0 = 2,  
    DRIVE_ATA0_1 = 3,
    DRIVE_ATA1_0 = 4,
    DRIVE_ATA1_1 = 5,
    DRIVE_TOTAL = 6
} DiskUIDrive;

// Drive info for UI display
typedef struct {
    const char *label;       // "A:", "B:", etc.
    const char *type_name;   // "Floppy", "Hard Disk", "CD-ROM"
} DriveInfo;

// Initialize disk UI system
void diskui_init(void);

// Open disk menu (shows OSD, returns immediately)
void diskui_open(void);

// Close disk menu (hides OSD)
void diskui_close(void);

// Check if disk menu is currently open
bool diskui_is_open(void);

// Handle keyboard input
// keycode: Linux keycode
// is_down: true for key press, false for release
// Returns: true if key was consumed by disk UI
bool diskui_handle_key(int keycode, bool is_down);

// Get info about a drive
const DriveInfo* diskui_get_drive_info(DiskUIDrive drive);

/* Set when a hard disk was changed, which only takes effect on a reset.
 * The kernel loop clears it. */
extern bool diskui_reset_requested_flag;

#endif // DISKUI_H
