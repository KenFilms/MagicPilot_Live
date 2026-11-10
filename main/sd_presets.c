/*
 * Camera presets on the microSD card. See sd_presets.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "cJSON.h"
#include "bsp/m5stack_tab5.h"
#include "sd_presets.h"

#define MOUNT_POINT     "/sdcard"
#define DIR_PATH        MOUNT_POINT "/MPLIVE"
#define FILE_PATH       MOUNT_POINT "/" SD_PRESETS_FILE
#define TMP_PATH        DIR_PATH "/PRESETS.TMP"
#define FILE_MAX        16384

static const char *TAG = "sd";
static sd_pwr_ctrl_handle_t s_ldo;      /* the card's supply (LDO 4), made once */

/* Mounts the card's FAT file system at MOUNT_POINT; NULL if there is no card or it can't be mounted */
static sdmmc_card_t *mount(void)
{
    if (!s_ldo) {
        const sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = 4};
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_ldo) != ESP_OK) {
            ESP_LOGE(TAG, "No supply for the card");
            s_ldo = NULL;
            return NULL;
        }
    }
    /* Slot 0 only: the host already runs for the ESP32-C6 on slot 1, and unmounting releases just slot 0 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.pwr_ctrl_handle = s_ldo;
    const sdmmc_slot_config_t slot = {
        .clk = BSP_SD_CLK,
        .cmd = BSP_SD_CMD,
        .d0 = BSP_SD_D0,
        .d1 = BSP_SD_D1,
        .d2 = BSP_SD_D2,
        .d3 = BSP_SD_D3,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
    };
    const esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No card: %s", esp_err_to_name(err));
        return NULL;
    }
    return card;
}

/* Unmounts the card, releasing slot 0 only (see mount) */
static void unmount(sdmmc_card_t *card)
{
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
}

/* Codec and quality as the RECORD menu names them, e.g. "BRAW" and "8:1" (a quality the menu has no name for as its
   number); both null for a preset saved without them */
static void add_codec(cJSON *o, const ui_preset_t *p)
{
    const char *codec = ui_codec_name(p->codec);
    if (!codec) {
        cJSON_AddNullToObject(o, "codec");
        cJSON_AddNullToObject(o, "quality");
        return;
    }
    cJSON_AddStringToObject(o, "codec", codec);
    for (int i = 0; i < UI_CODEC_COUNT; i++) {
        if (UI_CODECS[i].codec == p->codec && UI_CODECS[i].variant == p->codec_variant) {
            cJSON_AddStringToObject(o, "quality", UI_CODECS[i].label);
            return;
        }
    }
    if (p->codec_variant >= 0) {
        cJSON_AddNumberToObject(o, "quality", p->codec_variant);
    } else {
        cJSON_AddNullToObject(o, "quality");
    }
}

/* The resolution by the RECORD menu's name ("6K", "4K DCI"), else as "WxH", " windowed" when it is; null if the
   preset was saved without */
static void add_resolution(cJSON *o, const ui_preset_t *p)
{
    if (p->width <= 0) {
        cJSON_AddNullToObject(o, "resolution");
        return;
    }
    const ui_resolution_t *r = ui_resolution_find(p->width, p->height, p->windowed);
    char t[32];
    if (r) {
        snprintf(t, sizeof(t), "%s", r->label);
    } else {
        snprintf(t, sizeof(t), "%dx%d%s", p->width, p->height, p->windowed ? " windowed" : "");
    }
    cJSON_AddStringToObject(o, "resolution", t);
}

/* The presets file's text, every used preset with its slot (free it with cJSON_free); NULL without memory */
static char *presets_json(const ui_preset_t *presets, int count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "app", "MagicPilot LIVE");
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "units", "shutter: degrees x 100, or -x for 1/x s; iris_av: aperture value x 2048 "
                                           "(f-number = sqrt(2) ^ av), null = none; fps_flags: 1 project rate "
                                           "x 1000/1001, 2 sensor rate x 1000/1001, 4 off-speed; codec, quality "
                                           "and resolution as in the RECORD menu (BRAW 3:1 ... Q5, ProRes HQ ... PXY; "
                                           "6K ... HD, other sizes as WxH), null = not saved");
    cJSON *list = cJSON_AddArrayToObject(root, "presets");
    for (int i = 0; i < count; i++) {
        const ui_preset_t *p = &presets[i];
        if (!p->used) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i + 1);
        cJSON_AddStringToObject(o, "name", p->name);
        cJSON_AddNumberToObject(o, "iso", p->iso);
        cJSON_AddNumberToObject(o, "fps", p->fps);
        cJSON_AddNumberToObject(o, "sensor_fps", p->sensor_fps);
        cJSON_AddNumberToObject(o, "fps_flags", p->fps_flags);
        cJSON_AddNumberToObject(o, "shutter", p->shutter);
        cJSON_AddNumberToObject(o, "wb_kelvin", p->wb_kelvin);
        cJSON_AddNumberToObject(o, "wb_tint", p->wb_tint);
        if (p->iris_av == INT16_MIN) {
            cJSON_AddNullToObject(o, "iris_av");
        } else {
            cJSON_AddNumberToObject(o, "iris_av", p->iris_av);
        }
        add_codec(o, p);
        add_resolution(o, p);
        cJSON_AddItemToArray(list, o);
    }
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

sd_result_t sd_presets_save(const ui_preset_t *presets, int count)
{
    char *text = presets_json(presets, count);
    if (!text) {
        return SD_WRITE_FAILED;
    }
    sdmmc_card_t *card = mount();
    if (!card) {
        cJSON_free(text);
        return SD_NO_CARD;
    }
    mkdir(DIR_PATH, 0777);  /* fails harmlessly when it is there */
    bool ok = false;
    FILE *f = fopen(TMP_PATH, "w");
    if (f) {
        ok = fputs(text, f) >= 0;
        ok = fclose(f) == 0 && ok;
    }
    /* The old file goes only once the new one is written */
    if (ok) {
        remove(FILE_PATH);
        ok = rename(TMP_PATH, FILE_PATH) == 0;
    } else {
        remove(TMP_PATH);
    }
    unmount(card);
    cJSON_free(text);
    ESP_LOGI(TAG, "Presets %s %s", ok ? "saved to" : "could not be saved to", FILE_PATH);
    return ok ? SD_OK : SD_WRITE_FAILED;
}

/* The number under key in o, as an int; false if it is missing or not a number */
static bool get_int(const cJSON *o, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valueint;
    return true;
}

/* Codec and quality by their names in any case; as numbers too, the way files of the first version have them */
static void get_codec(const cJSON *o, ui_preset_t *p)
{
    const cJSON *codec = cJSON_GetObjectItemCaseSensitive(o, "codec");
    const cJSON *quality = cJSON_GetObjectItemCaseSensitive(o, "quality");
    int v;
    p->codec = p->codec_variant = -1;
    if (cJSON_IsString(codec)) {
        for (int c = 0; ui_codec_name(c); c++) {
            if (strcasecmp(codec->valuestring, ui_codec_name(c)) == 0) {
                p->codec = (int8_t)c;
            }
        }
    } else if (get_int(o, "codec", &v) && ui_codec_name(v)) {
        p->codec = (int8_t)v;
    }
    if (p->codec < 0) {
        return;
    }
    if (cJSON_IsString(quality)) {
        for (int i = 0; i < UI_CODEC_COUNT; i++) {
            if (UI_CODECS[i].codec == p->codec && strcasecmp(quality->valuestring, UI_CODECS[i].label) == 0) {
                p->codec_variant = UI_CODECS[i].variant;
            }
        }
    } else if (get_int(o, "quality", &v) || get_int(o, "codec_variant", &v)) {
        p->codec_variant = (int8_t)v;
    }
}

/* The resolution by its menu name in any case, or as "WxH" with " windowed" if it is */
static void get_resolution(const cJSON *o, ui_preset_t *p)
{
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(o, "resolution");
    p->width = p->height = 0;
    p->windowed = false;
    if (!cJSON_IsString(res)) {
        return;
    }
    for (int i = 0; i < UI_RESOLUTION_COUNT; i++) {
        if (strcasecmp(res->valuestring, UI_RESOLUTIONS[i].label) == 0) {
            p->width = UI_RESOLUTIONS[i].w;
            p->height = UI_RESOLUTIONS[i].h;
            p->windowed = UI_RESOLUTIONS[i].windowed;
            return;
        }
    }
    int w, h;
    if (sscanf(res->valuestring, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= INT16_MAX && h <= INT16_MAX) {
        p->width = (int16_t)w;
        p->height = (int16_t)h;
        p->windowed = strstr(res->valuestring, "windowed") != NULL;
    }
}

/*
 * Fills out (count presets, all unused to begin with) from a presets file's text: each entry that has what a
 * preset needs to be sent to the camera, its other values as when saved without them. Returns how many it filled,
 * 0 if the text is no presets file.
 */
static int presets_parse(const char *text, ui_preset_t *out, int count)
{
    cJSON *root = cJSON_Parse(text);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "presets");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return 0;
    }
    const cJSON *o;
    cJSON_ArrayForEach(o, list) {
        int slot, iso, fps, shutter, kelvin, v;
        if (!get_int(o, "slot", &slot) || slot < 1 || slot > count || !get_int(o, "iso", &iso) || iso <= 0 ||
            !get_int(o, "fps", &fps) || fps <= 0 || !get_int(o, "shutter", &shutter) || shutter == 0 ||
            !get_int(o, "wb_kelvin", &kelvin) || kelvin <= 0) {
            continue;
        }
        ui_preset_t *p = &out[slot - 1];
        memset(p, 0, sizeof(*p));
        p->used = true;
        p->iso = iso;
        p->fps = (int16_t)fps;
        p->shutter = shutter;
        p->wb_kelvin = (int16_t)kelvin;
        p->sensor_fps = (int16_t)(get_int(o, "sensor_fps", &v) && v > 0 ? v : fps);
        p->fps_flags = (uint8_t)(get_int(o, "fps_flags", &v) ? v & CAM_FMT_RATE_FLAGS : 0);
        p->wb_tint = (int16_t)(get_int(o, "wb_tint", &v) ? v : 0);
        p->iris_av = (int16_t)(get_int(o, "iris_av", &v) ? v : INT16_MIN);
        get_codec(o, p);
        get_resolution(o, p);
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
        if (cJSON_IsString(name)) {
            snprintf(p->name, sizeof(p->name), "%s", name->valuestring);
        }
    }
    cJSON_Delete(root);
    int n = 0;
    for (int i = 0; i < count; i++) {
        n += out[i].used;
    }
    return n;
}

sd_result_t sd_presets_load(ui_preset_t *presets, int count, int *loaded)
{
    *loaded = 0;
    sdmmc_card_t *card = mount();
    if (!card) {
        return SD_NO_CARD;
    }
    FILE *f = fopen(FILE_PATH, "r");
    if (!f) {
        unmount(card);
        return SD_NO_FILE;
    }
    char *buf = malloc(FILE_MAX + 1);
    const size_t n = buf ? fread(buf, 1, FILE_MAX, f) : 0;
    fclose(f);
    unmount(card);
    ui_preset_t *out = calloc(count, sizeof(ui_preset_t));
    if (buf && out) {
        buf[n] = 0;
        *loaded = presets_parse(buf, out, count);
    }
    free(buf);
    /* The presets only change when the file has some */
    if (*loaded) {
        memcpy(presets, out, count * sizeof(ui_preset_t));
    }
    free(out);
    ESP_LOGI(TAG, "%d presets read from %s", *loaded, FILE_PATH);
    return *loaded ? SD_OK : SD_BAD_FILE;
}
