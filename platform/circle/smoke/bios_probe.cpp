// SPDX-License-Identifier: GPL-3.0-or-later
#include "bios_probe.h"
#include "../../../src/ini.h"
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char config_path[] = "SD:/386/config.ini";

enum {
    BIOS_OK = 0,
    BIOS_CONFIG_OPEN = 1,
    BIOS_CONFIG_SIZE = 2,
    BIOS_CONFIG_READ = 3,
    BIOS_CONFIG_NUL = 4,
    BIOS_CONFIG_PARSE = 5,
    BIOS_CONFIG_KEY = 6,
    BIOS_PATH = 7,
    BIOS_OPEN = 8,
    BIOS_SIZE = 9,
    BIOS_READ = 10,
    BIOS_CLOSE = 11,
    BIOS_ALLOC = 12,
    BIOS_RESET = 13
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int)(crc & 1u));
    }
    return crc;
}

static int is_target_section(const char *section)
{
    return strcmp(section, "pc") == 0 || strcmp(section, "386") == 0;
}

typedef struct bios_ini_state {
    char value[TINY386_BIOS_PATH_CAP];
    int found;
    int invalid;
} bios_ini_state;

static int bios_ini_handler(void *opaque, const char *section,
                            const char *name, const char *value)
{
    bios_ini_state *state = (bios_ini_state *)opaque;
    if (!is_target_section(section) || strcmp(name, "bios") != 0)
        return 1;
    if (!value || !*value) {
        state->invalid = 1;
        return 0;
    }
    size_t length = strlen(value);
    if (length >= sizeof(state->value)) {
        state->invalid = 1;
        return 0;
    }
    /* ini_parse_string's value is temporary; last matching key wins. */
    memcpy(state->value, value, length + 1);
    state->found = 1;
    return 1;
}

int tiny386_bios_select(const char *config, char *value, uint32_t cap)
{
    if (!config || !value || cap == 0)
        return -1;
    bios_ini_state state = {};
    if (ini_parse_string(config, bios_ini_handler, &state) != 0 ||
        state.invalid || !state.found || strlen(state.value) >= cap)
        return -1;
    memcpy(value, state.value, strlen(state.value) + 1);
    return 0;
}

int tiny386_bios_normalize_path(const char *value, char *path, uint32_t cap)
{
    static const char prefix[] = "SD:/386/";
    if (!value || !path || cap == 0 || !*value)
        return -1;
    if (value[0] == '/' || value[0] == '\\')
        return -1;

    size_t prefix_len = sizeof(prefix) - 1;
    size_t out = 0;
    if (prefix_len + 1 > cap)
        return -1;
    memcpy(path, prefix, prefix_len);
    out = prefix_len;

    size_t component_start = 0;
    size_t value_len = strlen(value);
    for (size_t i = 0; i <= value_len; ++i) {
        const char c = value[i];
        if (c == ':')
            return -1;
        const int separator = c == '/' || c == '\\' || c == '\0';
        if (separator) {
            const size_t component_len = i - component_start;
            if (component_len == 2 && value[component_start] == '.' &&
                value[component_start + 1] == '.')
                return -1;
            if (component_len == 0 && c != '\0')
                return -1;
            for (size_t j = component_start; j < i; ++j) {
                if (out + 1 >= cap)
                    return -1;
                path[out++] = value[j] == '\\' ? '/' : value[j];
            }
            if (c != '\0') {
                if (out + 1 >= cap)
                    return -1;
                path[out++] = '/';
            }
            component_start = i + 1;
        }
    }
    /* A trailing slash is not a BIOS file. */
    if (out == prefix_len || path[out - 1] == '/')
        return -1;
    path[out] = '\0';
    return 0;
}

int tiny386_bios_map_base(uint32_t size, uint32_t *base, uint32_t *end)
{
    if (!base || !end || size < TINY386_BIOS_MIN_SIZE ||
        size > TINY386_BIOS_MAX_SIZE)
        return -1;
    *base = 0x100000u - size;
    *end = 0x100000u;
    return 0;
}

static int read_config(char *config, uint32_t *size_out,
                       bios_ini_state *state)
{
    FIL file;
    FRESULT result = f_open(&file, config_path, FA_READ);
    if (result != FR_OK)
        return BIOS_CONFIG_OPEN;
    const FSIZE_t file_size = f_size(&file);
    if (file_size > (FSIZE_t)TINY386_BIOS_CONFIG_CAP) {
        (void)f_close(&file);
        return BIOS_CONFIG_SIZE;
    }
    const uint32_t size = (uint32_t)file_size;

    uint32_t offset = 0;
    while (offset < size) {
        const UINT want = (UINT)((size - offset) > 1024u ? 1024u :
                                 (size - offset));
        UINT got = 0;
        result = f_read(&file, config + offset, want, &got);
        if (result != FR_OK || got != want) {
            (void)f_close(&file);
            return BIOS_CONFIG_READ;
        }
        offset += got;
    }
    result = f_close(&file);
    if (result != FR_OK)
        return BIOS_CONFIG_READ;
    for (uint32_t i = 0; i < size; ++i)
        if (config[i] == '\0')
            return BIOS_CONFIG_NUL;
    config[size] = '\0';
    if (ini_parse_string(config, bios_ini_handler, state) != 0)
        return BIOS_CONFIG_PARSE;
    if (state->invalid || !state->found)
        return BIOS_CONFIG_KEY;
    *size_out = size;
    return BIOS_OK;
}

static int load_bios(const char *path, char *ram, Tiny386BiosProbe *out)
{
    FIL file;
    FRESULT result = f_open(&file, path, FA_READ);
    if (result != FR_OK)
        return BIOS_OPEN;
    const FSIZE_t file_size = f_size(&file);
    if (file_size > (FSIZE_t)TINY386_BIOS_MAX_SIZE) {
        (void)f_close(&file);
        return BIOS_SIZE;
    }
    const uint32_t size = (uint32_t)file_size;
    uint32_t base = 0, end = 0;
    if (tiny386_bios_map_base(size, &base, &end) != 0) {
        (void)f_close(&file);
        return BIOS_SIZE;
    }

    uint32_t crc = 0xffffffffu;
    uint32_t offset = 0;
    uint8_t *destination = (uint8_t *)ram + base;
    while (offset < size) {
        const UINT want = (UINT)((size - offset) > 4096u ? 4096u :
                                 (size - offset));
        UINT got = 0;
        result = f_read(&file, destination + offset, want, &got);
        if (result != FR_OK || got != want) {
            (void)f_close(&file);
            return BIOS_READ;
        }
        crc = crc32_update(crc, destination + offset, got);
        offset += got;
    }
    result = f_close(&file);
    if (result != FR_OK)
        return BIOS_CLOSE;

    out->bios_size = size;
    out->bios_base = base;
    out->bios_end = end;
    out->bios_crc32 = crc ^ 0xffffffffu;
    memcpy(out->bios_last16, destination + size - 16u, 16u);
    return BIOS_OK;
}

int tiny386_bios_probe(Tiny386BiosProbe *out)
{
    if (!out)
        return BIOS_ALLOC;
    memset(out, 0, sizeof(*out));

    char *config = (char *)malloc(TINY386_BIOS_CONFIG_CAP + 1u);
    if (!config) {
        out->status = BIOS_ALLOC;
        return out->status;
    }
    bios_ini_state ini = {};
    int result = read_config(config, &out->config_size, &ini);
    out->config_status = result;
    if (result == BIOS_OK) {
        memcpy(out->bios_value, ini.value, sizeof(out->bios_value));
        out->config_complete = 1;
    }
    free(config);
    if (result != BIOS_OK) {
        out->status = result;
        return result;
    }
    if (tiny386_bios_normalize_path(out->bios_value, out->bios_path,
                                    sizeof(out->bios_path)) != 0) {
        out->status = BIOS_PATH;
        out->load_status = BIOS_PATH;
        return out->status;
    }

    char *ram = (char *)calloc(1, TINY386_BIOS_RAM_SIZE);
    if (!ram) {
        out->status = BIOS_ALLOC;
        out->load_status = BIOS_ALLOC;
        return out->status;
    }
    result = load_bios(out->bios_path, ram, out);
    out->load_status = result;
    if (result == BIOS_OK) {
        out->load_complete = 1;
        result = tiny386_real_reset_vector_probe(ram, out->bios_base,
                                                 out->bios_size, &out->reset);
        if (result != 0)
            out->status = BIOS_RESET;
        else {
            out->probe_complete = 1;
            out->status = BIOS_OK;
        }
    } else {
        out->status = result;
    }
    free(ram);
    return out->status;
}
