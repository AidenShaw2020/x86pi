// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINY386_BIOS_CONFIG_CAP  (16u << 10)
#define TINY386_BIOS_PATH_CAP    256u
#define TINY386_BIOS_RAM_SIZE    (2u << 20)
#define TINY386_BIOS_MIN_SIZE    16u
#define TINY386_BIOS_MAX_SIZE    (0x40000u)

typedef struct tiny386_reset_probe {
    int reset_ok;
    int vector_ok;
    int target_ok;
    int step_done;
    uint32_t reset_linear;
    uint16_t vector_cs;
    uint16_t vector_ip;
    uint32_t target_linear;
} Tiny386ResetProbe;

typedef struct tiny386_bios_probe {
    int status;
    int config_status;
    int load_status;
    int config_complete;
    int load_complete;
    int probe_complete;
    uint32_t config_size;
    uint32_t bios_size;
    uint32_t bios_base;
    uint32_t bios_end;
    uint32_t bios_crc32;
    char bios_value[TINY386_BIOS_PATH_CAP];
    char bios_path[TINY386_BIOS_PATH_CAP];
    uint8_t bios_last16[16];
    Tiny386ResetProbe reset;
} Tiny386BiosProbe;

/* Pure helpers kept public so host tests can cover path policy and the BIOS
 * size-to-1MiB mapping without mounting an SD card or invoking Circle. */
int tiny386_bios_normalize_path(const char *value, char *path, uint32_t cap);
int tiny386_bios_map_base(uint32_t size, uint32_t *base, uint32_t *end);
int tiny386_bios_select(const char *config, char *value, uint32_t cap);

/* Called by the C++ SD/FAT probe after it has loaded a BIOS into the supplied
 * 2 MiB guest RAM. RAM is borrowed for this call: the caller retains ownership
 * and must free it after the function returns. It performs no BIOS instruction
 * other than the one reset vector step, and leaves device/MMIO callbacks
 * aborting. */
int tiny386_real_reset_vector_probe(char *ram, uint32_t bios_base,
                                    uint32_t bios_size, Tiny386ResetProbe *out);

/* The SD volume must already be mounted read-only by the caller. */
int tiny386_bios_probe(Tiny386BiosProbe *out);

#ifdef __cplusplus
}
#endif
