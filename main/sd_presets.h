/*
 * Camera presets on the Tab5's microSD card: MPLIVE/PRESETS.TXT, readable JSON. The card is mounted only
 * while the file is written or read; it shares the SDMMC host with the ESP32-C6 link (slot 1), so only
 * its own slot 0 is set up and released.
 */

#pragma once

#include "ui_draw.h"

typedef enum {
    SD_OK,
    SD_NO_CARD,         /* no card, or one that can't be mounted (not FAT) */
    SD_NO_FILE,
    SD_BAD_FILE,        /* not JSON, or no presets in it */
    SD_WRITE_FAILED,
} sd_result_t;

#define SD_PRESETS_FILE     "MPLIVE/PRESETS.TXT"

/* Writes every used preset, with its slot */
sd_result_t sd_presets_save(const ui_preset_t *presets, int count);

/* Fills presets (count of them) from the file: its slots, the others empty. *loaded: the presets read */
sd_result_t sd_presets_load(ui_preset_t *presets, int count, int *loaded);
