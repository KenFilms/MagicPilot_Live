/*
 * Camera UI: touch input, actions and overlay redraws. See ui.h; drawing is in ui_draw.c.
 *
 * The UI task polls the touch controller, turns touches into taps, slider drags and held
 * steppers, sends camera commands (cam.h) and redraws the parts of the overlay that changed,
 * either because of a touch or because the camera reported something.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_lcd_touch.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "overlay.h"
#include "gfx.h"
#include "ui.h"
#include "ui_draw.h"
#include "cam.h"
#include "cam_ble.h"
#include "tab5_bat.h"
#include "sd_presets.h"

static const char *TAG = "ui";

#define POLL_MS             10
#define TAP_SLOP            36          /* a touch that moves further is not a tap */
#define REPEAT_DELAY_US     450000
#define REPEAT_US           110000
#define TOAST_US            2500000
#define SAVE_DELAY_US       2000000
#define SETTINGS_VERSION    5           /* 4 added live_view, 5 simple_view */
#define BATTERY_US          2000000     /* Tab5 battery read interval */
#define PRESETS_VERSION     4           /* 2 added the names, 3 codec and quality, 4 the resolution */
#define IRIS_REPORT_US      800000      /* how long a preset load waits for the camera to confirm an iris step */
#define IRIS_SETTLE_US      150000      /* and then for further reports while the lens settles */
#define IRIS_MAX_STEPS      30
#define IRIS_CLOSE_ENOUGH   100         /* fixed16 aperture value, about 1/20 stop */
#define FOCUS_REDRAW_US     33000       /* the focus slider follows a drag at most ~30 times a second */
#define LONG_PRESS_US       600000      /* holding the slate this long opens the slate page */

/* Parts of the overlay to redraw */
#define D_ALL               (1u << 0)
#define D_TOP               (1u << 1)
#define D_TC                (1u << 2)
#define D_BOTTOM            (1u << 3)
#define D_STATUS            (1u << 5)
#define D_POWER             (1u << 6)
#define D_RECORD            (1u << 7)
#define D_SLOTS             (1u << 8)
#define D_M5BAT             (1u << 9)
#define D_PRESET            (1u << 10)
#define D_FOCUS             (1u << 11)
#define D_SLATE             (1u << 12)
#define D_SLATE_QUICK       (1u << 13)
#define D_FORMAT            (1u << 14)

static ui_model_t s_m;                  /* owned by the UI task */
static TaskHandle_t s_task;
static esp_lcd_touch_handle_t s_tp;
static volatile bool s_fullscreen;
static uint32_t s_dirty = D_ALL;
static int64_t s_toast_until;
static int64_t s_save_at;               /* when to save the changed settings, 0 = nothing to save */
static char s_power_drawn[12];          /* power badge text on screen */
static char s_m5bat_drawn[12];          /* Tab5 battery text on screen */
static int64_t s_battery_at;            /* next Tab5 battery read */
static int64_t s_focus_drawn_at;
static int64_t s_status_off_at;         /* when to switch the camera's HDMI status text off, 0 = done */

/* 2x zoom, read by the renderer every frame */
static portMUX_TYPE s_zoom_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_zoom_on;
static float s_zoom_vx, s_zoom_vy;      /* view point in the middle of the zoomed picture */

/* Pinch tracking */
static bool s_pinch;                    /* two fingers came down: no taps or drags until all are up */
static bool s_pinch_done;               /* this pinch zoomed in or out already: one change per pinch */
static float s_pinch_d0;                /* finger distance as the pinch began */
static void (*s_live_view_handler)(bool on);
/* Quality last used in each codec and Blackmagic RAW mode: 3:1, Q0, HQ until the camera reports one */
static int8_t s_last_braw = 2, s_last_braw_bitrate = 2, s_last_braw_quality = 0, s_last_prores = 0;
static int s_battery_avg;               /* smoothed Tab5 battery voltage, mV */
static int s_power_votes;               /* readings in a row that disagree with m5_external */
static bool s_power_known;
static int16_t s_custom_kelvin, s_custom_tint;  /* the camera's last white balance that was no preset, for CWB */

/* Mailboxes filled by other tasks */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_video_info[40];
static bool s_video_new;
/* Live view as shown (s_m.live) follows the adapter: without a picture for VIDEO_GRACE_US the screen is as
   with live view off (a stream restart doesn't count), until a picture comes again. The adapter gets
   VIDEO_START_US after the start and after live view is switched on */
#define VIDEO_GRACE_US      3000000
#define VIDEO_START_US      10000000
static int64_t s_video_seen_at, s_video_wait_until;

/* Preset load: the values go out at once, then the iris is stepped to the saved aperture */
static struct {
    bool active;
    bool waiting;                       /* an iris step is out, waiting for the camera to report it */
    int slot;
    int16_t target, before;             /* aperture to reach, and the camera's before the step */
    int dir, steps;                     /* last step's direction (0 = none yet), steps sent */
    int64_t deadline;                   /* for the camera's report of the step */
    int64_t next_at;                    /* the next step */
    char note[16];                      /* what to set on the camera by hand: the preset's codec, or its ProRes
                                           resolution; "" = nothing */
} s_load;

/* Touch tracking */
static bool s_down;
static int s_x0, s_y0, s_x, s_y;        /* where the finger landed, and at the last poll */
static ui_hit_t s_press;
static bool s_moved;
static int64_t s_press_at;
static bool s_long_done;                /* the press was a long press: no tap on release */
static bool s_tapped;                   /* the press already acted as the finger landed */
static int64_t s_next_repeat;

/* ---- Settings ---- */

/* Settings of a first start; settings_load() keeps them where NVS holds nothing usable */
static void settings_defaults(void)
{
    s_m.set = (ui_settings_t){
        .grid_style = 2,
        .guide_ratio = 0,
        .safe_pct = 90,
        .brightness = CONFIG_HDMI_BACKLIGHT_PERCENT,
        .power_voltage = false,
        .m5bat_voltage = false,
        .live_view = true,
        .simple_view = false,
    };
}

/* The settings from NVS: a version byte, then ui_settings_t; the defaults if there are none of a known version */
static void settings_load(void)
{
    settings_defaults();
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t blob[1 + sizeof(ui_settings_t)];
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, "set", blob, &len) == ESP_OK) {
        /* Older versions had the same fields, up to the first one they lacked */
        if ((len == sizeof(blob) && blob[0] == SETTINGS_VERSION) ||
            (len == 1 + offsetof(ui_settings_t, live_view) && blob[0] == 3) ||
            (len == 1 + offsetof(ui_settings_t, simple_view) && blob[0] == 4)) {
            memcpy(&s_m.set, blob + 1, len - 1);
        }
        if (s_m.set.guide_ratio >= UI_GUIDE_COUNT) {
            s_m.set.guide_ratio = 0;
        }
    }
    nvs_close(h);
}

/* Stores the settings in NVS at once; changes usually go through settings_changed() instead */
static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint8_t blob[1 + sizeof(ui_settings_t)];
    blob[0] = SETTINGS_VERSION;
    memcpy(blob + 1, &s_m.set, sizeof(ui_settings_t));
    nvs_set_blob(h, "set", blob, sizeof(blob));
    nvs_commit(h);
    nvs_close(h);
}

/* The settings go to NVS SAVE_DELAY_US after the last change (take_updates), so a drag writes them once */
static void settings_changed(void)
{
    s_save_at = esp_timer_get_time() + SAVE_DELAY_US;
}

/* ---- Camera presets, kept in NVS ---- */

/* A preset as version 2 stored it: without codec and quality; version 1 also without the name */
typedef struct {
    bool used;
    uint8_t fps_flags;
    int16_t fps, sensor_fps, wb_kelvin, wb_tint, iris_av;
    int32_t iso, shutter;
    char name[UI_PRESET_NAME_MAX + 1];
} preset_v2_t;

/* Version 3: without the resolution, which version 4 added at the end */
typedef struct {
    bool used;
    uint8_t fps_flags;
    int16_t fps, sensor_fps, wb_kelvin, wb_tint, iris_av;
    int32_t iso, shutter;
    char name[UI_PRESET_NAME_MAX + 1];
    int8_t codec, codec_variant;
} preset_v3_t;

static uint8_t s_blob[1 + sizeof(s_m.presets)];  /* the presets as in NVS: a version byte, then the array */

/* The presets from NVS, converted from versions 1 and 2; all empty if there are none of a known version */
static void presets_load(void)
{
    memset(s_m.presets, 0, sizeof(s_m.presets));
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_blob);
    if (nvs_get_blob(h, "presets", s_blob, &len) == ESP_OK) {
        if (len == sizeof(s_blob) && s_blob[0] == PRESETS_VERSION) {
            memcpy(s_m.presets, s_blob + 1, sizeof(s_m.presets));
        } else if (s_blob[0] == 3 && len == 1 + UI_PRESET_COUNT * sizeof(preset_v3_t)) {
            /* A version 3 preset is the start of one of now, which ends with the resolution: not saved */
            for (int i = 0; i < UI_PRESET_COUNT; i++) {
                memcpy(&s_m.presets[i], s_blob + 1 + i * sizeof(preset_v3_t), offsetof(ui_preset_t, width));
                s_m.presets[i].width = s_m.presets[i].height = 0;
                s_m.presets[i].windowed = false;
            }
        } else if ((s_blob[0] == 2 && len == 1 + UI_PRESET_COUNT * sizeof(preset_v2_t)) ||
                   (s_blob[0] == 1 && len == 1 + UI_PRESET_COUNT * offsetof(preset_v2_t, name))) {
            /* Each as long as its version stored it: a version 1 preset ends before the name */
            const size_t size = (len - 1) / UI_PRESET_COUNT;
            for (int i = 0; i < UI_PRESET_COUNT; i++) {
                preset_v2_t o = {0};
                memcpy(&o, s_blob + 1 + i * size, size);
                s_m.presets[i] = (ui_preset_t){
                    .used = o.used, .fps_flags = o.fps_flags, .fps = o.fps, .sensor_fps = o.sensor_fps,
                    .wb_kelvin = o.wb_kelvin, .wb_tint = o.wb_tint, .iris_av = o.iris_av, .iso = o.iso,
                    .shutter = o.shutter, .codec = -1, .codec_variant = -1,
                };
                memcpy(s_m.presets[i].name, o.name, sizeof(o.name));
            }
        }
    }
    nvs_close(h);
    /* Each name ends within its array, whatever NVS held */
    int used = 0;
    for (int i = 0; i < UI_PRESET_COUNT; i++) {
        s_m.presets[i].name[UI_PRESET_NAME_MAX] = 0;
        used += s_m.presets[i].used;
    }
    ESP_LOGI(TAG, "%d presets, stored as version %d", used, len > 0 ? s_blob[0] : 0);
}

/* Stores all presets in NVS; false if that failed */
static bool presets_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    s_blob[0] = PRESETS_VERSION;
    memcpy(s_blob + 1, s_m.presets, sizeof(s_m.presets));
    const bool ok = nvs_set_blob(h, "presets", s_blob, sizeof(s_blob)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* ---- Mailboxes ---- */

bool ui_fullscreen(void)
{
    return s_fullscreen;
}

bool ui_zoom(float *vx, float *vy)
{
    taskENTER_CRITICAL(&s_zoom_mux);
    const bool on = s_zoom_on;
    *vx = s_zoom_vx;
    *vy = s_zoom_vy;
    taskEXIT_CRITICAL(&s_zoom_mux);
    return on;
}

/* Zoom in on the view point (vx, vy), or out; the middle of a 2x picture stays a quarter screen in */
static void zoom_set(bool on, float vx, float vy)
{
    vx = vx < OV_W / 4.0f ? OV_W / 4.0f : (vx > OV_W * 0.75f ? OV_W * 0.75f : vx);
    vy = vy < OV_H / 4.0f ? OV_H / 4.0f : (vy > OV_H * 0.75f ? OV_H * 0.75f : vy);
    taskENTER_CRITICAL(&s_zoom_mux);
    s_zoom_on = on;
    s_zoom_vx = vx;
    s_zoom_vy = vy;
    taskEXIT_CRITICAL(&s_zoom_mux);
    if (s_m.zoomed != on) {
        s_m.zoomed = on;
        s_dirty |= D_STATUS;
    }
}

bool ui_live_view(void)
{
    /* Also asked before ui_start(): the USB-A port is powered first thing */
    static bool loaded;
    if (!loaded) {
        settings_load();
        loaded = true;
    }
    return s_m.set.live_view;
}

void ui_set_live_view_handler(void (*handler)(bool on))
{
    s_live_view_handler = handler;
}

void ui_set_video(bool on, const char *info)
{
    taskENTER_CRITICAL(&s_mux);
    snprintf(s_video_info, sizeof(s_video_info), "%s", on && info ? info : "");
    s_video_new = true;
    taskEXIT_CRITICAL(&s_mux);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* cam.c's change callback, from any task: wakes the UI task to take the changes */
static void cam_changed(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* ---- Helpers ---- */

/* What a message redraws: the pages with a status bar whole, otherwise the status band */
static uint32_t toast_dirty(void)
{
    return s_m.page == PAGE_PRESETS || s_m.page == PAGE_MENU || s_m.page == PAGE_SLATE ? D_ALL : D_STATUS;
}

/* Shows a message (printf format) for TOAST_US, in place of any before it */
static void __attribute__((format(printf, 1, 2))) toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_m.toast, sizeof(s_m.toast), fmt, ap);
    va_end(ap);
    s_toast_until = esp_timer_get_time() + TOAST_US;
    s_dirty |= toast_dirty();
}

/* True if the camera is connected; otherwise says so in a message */
static bool need_camera(void)
{
    if (s_m.cam.link == CAM_LINK_CONNECTED) {
        return true;
    }
    toast("Camera not connected");
    return false;
}

/* Display the monitoring buttons act on: HDMI, which the Tab5 shows, or without live view the camera's LCD */
static uint8_t mon_display(void)
{
    return s_m.live ? CAM_DISPLAY_HDMI : CAM_DISPLAY_LCD;
}

/* Clean feed and frame guides on that display; the LCD keeps its status text */
static void mon_overlays(uint8_t bits)
{
    cam_set_overlay_enables(bits | (s_m.live ? 0 : CAM_OVL_STATUS), mon_display());
}

/* The camera's overlay settings are known (needed to change one of them) */
static bool have_overlays(void)
{
    if (!need_camera()) {
        return false;
    }
    if (!s_m.cam.have_overlays) {
        toast("No overlay settings from the camera yet");
        return false;
    }
    return true;
}

/*
 * The camera's grid or safe area on or off: grid type none and safe area 0 % hide them. It draws them
 * only on outputs where its menu switches them on, which it does not report, hence the reminder.
 */
static void set_grid_safe(bool grid, bool on)
{
    const cam_state_t *c = &s_m.cam;
    if (!have_overlays()) {
        return;
    }
    const ui_settings_t *s = &s_m.set;
    const bool was_on = grid ? c->grid : c->safe_area;
    /* grid_style 0 thirds, 1 cross hairs, 2 both: the camera's bits 1, 2, 3 */
    cam_set_overlays(c->guide_style, c->guide_opacity,
                     grid ? c->safe_area : (on ? (int8_t)s->safe_pct : 0),
                     grid ? (on ? (int8_t)(s->grid_style + 1) : 0) : c->grid);
    if (on && !was_on) {
        toast("Shows if %s is on for %s on the camera", grid ? "Grid" : "Safe Area", s_m.live ? "HDMI" : "LCD");
    }
}

/* The camera's frame guides, clean feed kept as it is */
static void set_guides(bool on)
{
    if (!need_camera()) {
        return;
    }
    const bool clean = s_m.cam.overlay_bits & CAM_OVL_CLEAN_FEED;
    mon_overlays((on ? CAM_OVL_GUIDES : 0) | (clean ? CAM_OVL_CLEAN_FEED : 0));
}

/* The camera's frame guide style and opacity; style 0 (off) it ignores, so GUIDE switches them */
static void set_guide_style(int8_t style, int8_t opacity)
{
    const cam_state_t *c = &s_m.cam;
    if (have_overlays()) {
        cam_set_overlays(style, opacity, c->safe_area, c->grid);
    }
}

/* Closes the quick slate (scene and take above the slate) if it is open */
static void close_slate_quick(void)
{
    if (s_m.slate_quick) {
        s_m.slate_quick = false;
        s_dirty |= D_SLATE_QUICK;
    }
}

/* Opens panel p, or closes it if it is open already (a second tap on its button); POP_NONE closes any */
static void open_popup(ui_popup_t p)
{
    s_m.popup = s_m.popup == p ? POP_NONE : p;
    /* While the camera plays clips, its panel instead of none */
    if (s_m.popup == POP_NONE && s_m.cam.transport_mode == 1) {
        s_m.popup = POP_PLAYBACK;
    }
    /* Its off-speed rows as they are: they only slide when off-speed is switched */
    s_m.fps_rows = ui_off_speed(&s_m) ? 1.0f : 0.0f;
    close_slate_quick();
    s_dirty |= D_TOP | D_STATUS | D_BOTTOM;   /* the frame icon is in the status band */
}

/* Shows page p, or with PAGE_NONE the main screen, without a panel (but playback's while the camera plays) */
static void open_page(ui_page_t p)
{
    s_m.slate_quick = false;
    s_m.page = p;
    s_m.popup = p == PAGE_NONE && s_m.cam.transport_mode == 1 ? POP_PLAYBACK : POP_NONE;
    s_fullscreen = p != PAGE_NONE;
    s_dirty |= D_ALL;
}

/* ---- Playback ---- */

/* A control of the playback panel (ui_pb_t), sent to the camera */
static void playback_action(int a)
{
    const cam_state_t *c = &s_m.cam;
    const int8_t s = c->transport_speed;
    switch (a) {
    case PB_PREV:
    case PB_NEXT:
        cam_clip_step(a == PB_NEXT);
        break;
    case PB_PLAY:
        cam_play(s == 0 ? 1 : 0);
        break;
    case PB_FF:
        cam_play(s >= 2 && s < 8 ? s * 2 : 2);         /* 2x, 4x, 8x, then 2x again */
        break;
    case PB_REW:
        cam_play(s <= -2 && s > -8 ? s * 2 : -2);
        break;
    case PB_LIVE:
        cam_live();
        s_m.popup = POP_NONE;
        s_dirty |= D_TOP | D_STATUS;
        break;
    case PB_LOOP:
        cam_play_loop(!(c->transport_flags & 1));
        break;
    case PB_GOOD_TAKE:
        /* While the camera plays, its slate is the clip's */
        if (s_m.slate.have) {
            cam_set_slate_good_take(!s_m.slate.good_take);
        }
        break;
    default:
        break;
    }
    s_dirty |= D_BOTTOM;
}

/* v limited to lo .. hi */
static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Sets the backlight to the brightness setting */
static void apply_brightness(void)
{
    bsp_display_brightness_set(s_m.set.brightness);
}

/* ---- Value changes ---- */

/* A rate of UI_PROJECT_FPS: 23.98, 29.97 and 59.94 go as 24, 30 and 60 with the M-rate flag */
static void set_project_fps(int16_t fps)
{
    const bool mrate = fps == 23 || fps == 29 || fps == 59;
    cam_set_project_fps(mrate ? fps + 1 : fps, mrate);
}

/* The project frame rate one step up (dir 1) or down (-1) in UI_PROJECT_FPS */
static void step_project_fps(int dir)
{
    /* Up to what the resolution allows */
    const int n = ui_project_fps_count(&s_m.cam), i = ui_project_fps_index(&s_m.cam);
    set_project_fps(UI_PROJECT_FPS[clampi(i + dir, 0, n - 1)]);
}

/* Off-speed recording at sensor rate fps, kept within UI_OFFSPEED_MIN .. what the resolution allows */
static void set_off_speed_fps(int fps)
{
    cam_set_off_speed(true, (int16_t)clampi(fps, UI_OFFSPEED_MIN, ui_max_fps(&s_m.cam)));
}

/* Off-speed off, or on: at the sensor rate if that is above the project rate, otherwise at twice it */
static void toggle_off_speed(void)
{
    if (ui_off_speed(&s_m)) {
        cam_set_off_speed(false, 0);
    } else {
        const int fps = s_m.cam.sensor_fps > s_m.cam.file_fps ? s_m.cam.sensor_fps : s_m.cam.file_fps * 2;
        set_off_speed_fps(fps);
    }
}

/* < (dir -1) or > (dir 1) on the open panel; index 0 the first value, 1 the tint, 2 the FPS panel's project rate */
static void step_value(int index, int dir)
{
    const cam_state_t *c = &s_m.cam;
    if (s_m.popup == POP_TOOLS) {
        ui_settings_t *s = &s_m.set;
        /* Guides, grid and safe area are the camera's: switched on with the new ratio, type or size */
        if (s_m.tool != TOOL_BRIGHTNESS && !need_camera()) {
            return;
        }
        switch (s_m.tool) {
        case TOOL_GUIDES:
            s->guide_ratio = (uint8_t)((s->guide_ratio + UI_GUIDE_COUNT + dir) % UI_GUIDE_COUNT);
            set_guide_style(UI_GUIDE_RATIOS[s->guide_ratio].style, c->guide_opacity);
            if (!(c->overlay_bits & CAM_OVL_GUIDES)) {
                set_guides(true);
            }
            break;
        case TOOL_GRID:
            s->grid_style = (uint8_t)((s->grid_style + 3 + dir) % 3);
            set_grid_safe(true, true);
            break;
        case TOOL_SAFE:
            s->safe_pct = (uint8_t)clampi(s->safe_pct + dir * 5, 5, 100);
            set_grid_safe(false, true);
            break;
        case TOOL_BRIGHTNESS:
            s->brightness = (uint8_t)clampi(s->brightness + dir * 5, 10, 100);
            apply_brightness();
            break;
        default:
            break;
        }
        settings_changed();
        s_dirty |= D_BOTTOM;
        return;
    }
    if (!need_camera()) {
        return;
    }
    switch (s_m.popup) {
    case POP_ISO: {
        const int i = ui_nearest_index(UI_ISO_STEPS, UI_ISO_STEP_COUNT, c->iso);
        cam_set_iso(UI_ISO_STEPS[clampi(i + dir, 0, UI_ISO_STEP_COUNT - 1)]);
        break;
    }
    case POP_SHUTTER: {
        if (ui_shutter_auto(c->ae_mode)) {
            break;      /* auto exposure drives it */
        }
        if (c->shutter_speed_mode) {
            /* As the camera measures it: speeds, none slower than one frame */
            const int fps = ui_exposure_fps(c);
            int lo = 0;
            while (lo < UI_SHUTTER_SPEED_COUNT - 1 && UI_SHUTTER_SPEEDS[lo] < fps) {
                lo++;
            }
            const int i = ui_nearest_index(UI_SHUTTER_SPEEDS, UI_SHUTTER_SPEED_COUNT, c->shutter_speed);
            cam_set_shutter_speed(UI_SHUTTER_SPEEDS[clampi(i + dir, lo, UI_SHUTTER_SPEED_COUNT - 1)]);
            break;
        }
        const int i = ui_nearest_index(UI_SHUTTER_STEPS, UI_SHUTTER_STEP_COUNT, c->shutter_angle);
        cam_set_shutter_angle(UI_SHUTTER_STEPS[clampi(i + dir, 0, UI_SHUTTER_STEP_COUNT - 1)]);
        break;
    }
    case POP_IRIS:
        if (!ui_iris_auto(c->ae_mode)) {
            cam_iris_step(dir);     /* "<" opens up (lower f-number) */
        }
        break;
    case POP_FPS:
        /* Index 2: the project frame rate's own row while off-speed */
        if (ui_off_speed(&s_m) && index != 2) {
            set_off_speed_fps(c->sensor_fps + dir);
        } else {
            step_project_fps(dir);
        }
        break;
    case POP_WB:
        if (!c->have_wb) {
            break;
        }
        if (index == 0) {
            /* Move to the next multiple of the step, even from between steps */
            const int k = c->wb_kelvin;
            const int next = dir > 0 ? (k / UI_WB_STEP + 1) * UI_WB_STEP : ((k + UI_WB_STEP - 1) / UI_WB_STEP - 1) * UI_WB_STEP;
            cam_set_wb((int16_t)clampi(next, UI_WB_MIN, UI_WB_MAX), c->wb_tint);
        } else {
            cam_set_wb(c->wb_kelvin, (int16_t)clampi(c->wb_tint + dir, -50, 50));
        }
        break;
    default:
        break;
    }
}

/* index: which of a panel's sliders; 1 is the FPS panel's project frame rate row while off-speed */
static void slide(int index, float f)
{
    const cam_state_t *c = &s_m.cam;
    if (s_m.popup == POP_TOOLS) {
        if (s_m.tool == TOOL_GUIDES) {
            /* The camera's frame guide opacity */
            const int8_t v = (int8_t)lroundf(f * 100);
            if (c->link == CAM_LINK_CONNECTED && c->have_overlays && v != c->guide_opacity) {
                set_guide_style(c->guide_style, v);
            }
        } else if (s_m.tool == TOOL_BRIGHTNESS) {
            const uint8_t v = (uint8_t)(10 + lroundf(f * 90));
            if (v != s_m.set.brightness) {
                s_m.set.brightness = v;
                apply_brightness();
                s_dirty |= D_BOTTOM;
                settings_changed();
            }
        }
        return;
    }
    if (c->link != CAM_LINK_CONNECTED) {
        return;
    }
    switch (s_m.popup) {
    case POP_ISO: {
        const int32_t iso = UI_ISO_STEPS[lroundf(f * (UI_ISO_STEP_COUNT - 1))];
        if (iso != c->iso) {
            cam_set_iso(iso);
        }
        break;
    }
    case POP_IRIS: {
        const int16_t norm = (int16_t)lroundf(f * 2048);
        if (!c->have_iris_norm || abs(norm - c->iris_norm) >= 16) {
            cam_set_iris_norm(norm);
        }
        break;
    }
    case POP_FPS:
        if (ui_off_speed(&s_m) && index != 1) {
            const int fps = UI_OFFSPEED_MIN + (int)lroundf(f * (ui_max_fps(c) - UI_OFFSPEED_MIN));
            if (fps != c->sensor_fps) {
                set_off_speed_fps(fps);
            }
        } else {
            const int16_t fps = UI_PROJECT_FPS[lroundf(f * (ui_project_fps_count(c) - 1))];
            if (fps != ui_project_fps(c)) {
                set_project_fps(fps);
            }
        }
        break;
    case POP_WB:
        if (c->have_wb) {
            const int k = UI_WB_MIN + (int)lroundf(f * (UI_WB_MAX - UI_WB_MIN) / UI_WB_STEP) * UI_WB_STEP;
            if (k != c->wb_kelvin) {
                cam_set_wb((int16_t)k, c->wb_tint);
            }
        }
        break;
    default:
        break;
    }
}

/* The open panel's switch: off-speed, auto exposure (iris, shutter), or the camera's guides, grid or safe area */
static void toggle(void)
{
    if (s_m.popup == POP_TOOLS) {
        if (s_m.tool == TOOL_GUIDES) {
            set_guides(!(s_m.cam.overlay_bits & CAM_OVL_GUIDES));
        } else if (s_m.tool == TOOL_GRID) {
            set_grid_safe(true, !s_m.cam.grid);
        } else if (s_m.tool == TOOL_SAFE) {
            set_grid_safe(false, !s_m.cam.safe_area);
        }
        return;
    }
    if (!need_camera()) {
        return;
    }
    const int8_t ae = s_m.cam.ae_mode;
    if (s_m.popup == POP_FPS) {
        toggle_off_speed();
    } else if (s_m.popup == POP_IRIS) {
        cam_set_ae_mode(ui_iris_auto(ae) ? 0 : 1);
    } else if (s_m.popup == POP_SHUTTER) {
        cam_set_ae_mode(ui_shutter_auto(ae) ? 0 : 2);
    }
}

/* Preset button i of the open panel (no camera preset): a value or, under auto exposure, an exposure mode */
static void preset(int i)
{
    if (!need_camera()) {
        return;
    }
    const cam_state_t *c = &s_m.cam;
    switch (s_m.popup) {
    case POP_ISO:
        if (i < UI_ISO_PRESET_COUNT) {
            cam_set_iso(UI_ISO_PRESETS[i]);
        }
        break;
    case POP_WB:
        if (i < UI_WB_PRESET_COUNT) {
            const ui_wb_preset_t *p = &UI_WB_PRESETS[i];
            if (p->kelvin > 0) {
                cam_set_wb(p->kelvin, p->tint);
            } else if (p->kelvin < 0) {
                /* CWB: back to the last white balance that was no preset */
                if (s_custom_kelvin > 0) {
                    cam_set_wb(s_custom_kelvin, s_custom_tint);
                } else {
                    toast("No custom white balance yet");
                }
            } else {
                cam_auto_wb();
                toast("Auto white balance");
            }
        }
        break;
    case POP_FPS:
        if (i < ui_offspeed_preset_count(c)) {
            set_off_speed_fps(UI_OFFSPEED_PRESETS[i]);
        }
        break;
    case POP_IRIS:
        if (i < UI_AE_MODE_COUNT && ui_iris_auto(c->ae_mode)) {
            cam_set_ae_mode(UI_IRIS_AE_MODES[i]);
        }
        break;
    case POP_SHUTTER:
        if (i < UI_SHUTTER_PRESET_COUNT) {
            if (ui_shutter_auto(c->ae_mode)) {
                cam_set_ae_mode(UI_SHUTTER_AE_MODES[i]);
            } else if (!c->shutter_speed_mode) {
                cam_set_shutter_angle(UI_SHUTTER_PRESETS[i]);
            } else if (ui_exposure_fps(c) > 0) {
                cam_set_shutter_speed(ui_speed_from_angle(UI_SHUTTER_PRESETS[i], ui_exposure_fps(c)));
            }
        }
        break;
    default:
        break;
    }
}

/* ---- Camera presets (as in MagicPilot_Remote) ---- */

/* A message about a preset; fmt has one %s for its name */
static void preset_status(const char *fmt, int slot)
{
    char name[UI_PRESET_NAME_MAX + 8];
    ui_preset_name(&s_m, slot, name, sizeof(name));
    toast(fmt, name);
}

/* Marks preset i (-1: none) as the last loaded or saved, whose name the main screen shows */
static void set_active_preset(int i)
{
    if (s_m.preset_active != i) {
        s_m.preset_active = (int8_t)i;
        s_dirty |= D_PRESET;
    }
}

/* All presets to or from the microSD card in the Tab5 */
static void presets_to_sd(void)
{
    switch (sd_presets_save(s_m.presets, UI_PRESET_COUNT)) {
    case SD_OK:       toast("Saved to SD card: " SD_PRESETS_FILE); break;
    case SD_NO_CARD:  toast("No SD card in the Tab5"); break;
    default:          toast("Could not write to the SD card"); break;
    }
}

/* On the second LOAD FROM SD tap: the presets read are stored in NVS too; the last loaded one is forgotten */
static void presets_from_sd(void)
{
    /* Replaces the presets only when the file has some; back to the old ones if they can't be stored */
    static ui_preset_t before[UI_PRESET_COUNT];
    memcpy(before, s_m.presets, sizeof(before));
    int n = 0;
    switch (sd_presets_load(s_m.presets, UI_PRESET_COUNT, &n)) {
    case SD_OK:
        if (!presets_save()) {
            memcpy(s_m.presets, before, sizeof(before));
            toast("Could not store the presets from SD");
            break;
        }
        set_active_preset(-1);
        toast("%d preset%s loaded from the SD card", n, n == 1 ? "" : "s");
        break;
    case SD_NO_CARD:  toast("No SD card in the Tab5"); break;
    case SD_NO_FILE:  toast("No " SD_PRESETS_FILE " on the SD card"); break;
    default:          toast(SD_PRESETS_FILE " can't be read"); break;
    }
}

/* Stores the camera's current values; the iris is optional because some lenses don't report it */
static void preset_save(int i)
{
    const cam_state_t *c = &s_m.cam;
    /* The shutter as the camera measures it: an angle, or a speed stored negative */
    const int32_t shutter = c->shutter_speed_mode ? -c->shutter_speed : c->shutter_angle;
    if (c->link != CAM_LINK_CONNECTED || c->iso <= 0 || shutter == 0 || !c->have_format || c->file_fps <= 0 ||
        !c->have_wb || c->wb_kelvin <= 0) {
        toast("Connect camera to save preset");
        return;
    }
    const ui_preset_t before = s_m.presets[i];
    s_m.presets[i] = (ui_preset_t){
        .used = true,
        .fps_flags = (uint8_t)(c->format_flags & CAM_FMT_RATE_FLAGS),
        .fps = c->file_fps,
        .sensor_fps = c->sensor_fps,
        .wb_kelvin = c->wb_kelvin,
        .wb_tint = c->wb_tint,
        .iris_av = c->have_iris ? c->iris_av : INT16_MIN,
        .iso = c->iso,
        .shutter = shutter,
        .codec = c->codec,
        .codec_variant = c->codec_variant,
        .width = c->width,
        .height = c->height,
        .windowed = c->format_flags & CAM_FMT_WINDOWED,
    };
    if (!presets_save()) {
        s_m.presets[i] = before;
        toast("Could not save the preset");
        return;
    }
    set_active_preset(i);
    const ui_preset_t *p = &s_m.presets[i];
    ESP_LOGI(TAG, "Preset %d saved: ISO %ld, shutter %ld, fps %d/%d flags %d, WB %d K tint %d, iris %d, codec %d/%d, "
             "%dx%d%s", i + 1, (long)p->iso, (long)p->shutter, p->fps, p->sensor_fps, p->fps_flags, p->wb_kelvin,
             p->wb_tint, p->iris_av, p->codec, p->codec_variant, p->width, p->height, p->windowed ? " windowed" : "");
    preset_status("%s saved", i);
}

/* Ends a preset load with a message; ok: the iris reached the saved aperture, or there was none to reach */
static void preset_load_done(bool ok)
{
    s_load.active = false;
    ESP_LOGI(TAG, "Preset %d %s (%d iris steps, aperture %d, target %d)", s_load.slot + 1, ok ? "loaded" : "load incomplete",
             s_load.steps, s_m.cam.iris_av, s_load.target);
    char name[UI_PRESET_NAME_MAX + 8];
    ui_preset_name(&s_m, s_load.slot, name, sizeof(name));
    if (s_load.note[0]) {
        /* The camera takes no switch between BRAW and ProRes, nor a ProRes resolution, over Bluetooth */
        toast("%s %s, set %s on camera", name, ok ? "loaded" : "incomplete", s_load.note);
    } else {
        toast(ok ? "%s loaded" : "%s load incomplete", name);
    }
}

/* Sends the preset's values; the iris follows step by step in preset_tick() */
static void preset_load(int i)
{
    if (s_m.cam.link != CAM_LINK_CONNECTED) {
        toast("Connect camera to load preset");
        return;
    }
    const ui_preset_t *p = &s_m.presets[i];
    if (!p->used) {
        preset_status("%s is empty", i);
        return;
    }
    if (s_load.active) {
        toast("Preset load in progress");
        return;
    }
    /* Only what differs: no needless writes, and the recording format only for a new frame rate */
    const cam_state_t *c = &s_m.cam;
    if (c->iso != p->iso) {
        cam_set_iso(p->iso);
    }
    /*
     * The resolution goes with the frame rates in one write, which the camera takes or refuses as a whole. Over
     * Bluetooth it takes no switch between BRAW and ProRes, nor a ProRes resolution: those go into a note.
     */
    s_load.note[0] = 0;
    const bool other_codec = p->codec >= 0 && c->codec >= 0 && p->codec != c->codec;
    const bool new_res = p->width > 0 && (p->width != c->width || p->height != c->height ||
                                          p->windowed != ((c->format_flags & CAM_FMT_WINDOWED) != 0));
    bool set_res = false;
    if (other_codec) {
        snprintf(s_load.note, sizeof(s_load.note), "%s", ui_codec_name(p->codec));
    } else if (new_res && c->codec == CAM_CODEC_PRORES) {
        ui_fmt_resolution(p->width, p->height, s_load.note, sizeof(s_load.note));
    } else {
        set_res = new_res;
    }
    /* Off speed no faster than the resolution allows, the preset's when it comes along */
    cam_state_t at = *c;
    if (set_res) {
        at.width = p->width;
        at.height = p->height;
        at.format_flags = (int16_t)(p->windowed ? at.format_flags | CAM_FMT_WINDOWED : at.format_flags & ~CAM_FMT_WINDOWED);
    }
    const int16_t sensor_fps = (int16_t)clampi(p->sensor_fps, 0, ui_max_fps(&at));
    /* The shutter as the camera measures it now, converted at the preset's frame rate if saved the other way */
    const int fps = (p->fps_flags & CAM_FMT_OFF_SPEED) ? sensor_fps : p->fps;
    if (c->shutter_speed_mode) {
        const int32_t speed = p->shutter < 0 ? -p->shutter : ui_speed_from_angle(p->shutter, fps);
        if (speed > 0 && c->shutter_speed != speed) {
            cam_set_shutter_speed(speed);
        }
    } else {
        const int32_t angle = p->shutter > 0 ? p->shutter : ui_angle_from_speed(-p->shutter, fps);
        if (angle > 0 && c->shutter_angle != angle) {
            cam_set_shutter_angle(angle);
        }
    }
    if (c->have_format && (set_res || c->file_fps != p->fps || (c->format_flags & CAM_FMT_RATE_FLAGS) != p->fps_flags ||
                           ((p->fps_flags & CAM_FMT_OFF_SPEED) && c->sensor_fps != sensor_fps))) {
        cam_set_format(p->fps, sensor_fps, p->fps_flags, set_res ? p->width : 0, p->height, p->windowed);
    }
    if (!c->have_wb || c->wb_kelvin != p->wb_kelvin || c->wb_tint != p->wb_tint) {
        cam_set_wb(p->wb_kelvin, p->wb_tint);
    }
    /* Codec and quality: the quality within the camera's codec (another codec is in the note) */
    if (!other_codec && p->codec >= 0 && c->codec >= 0 && p->codec_variant >= 0 && p->codec_variant != c->codec_variant) {
        cam_set_codec(p->codec, p->codec_variant);
    }
    set_active_preset(i);
    s_load.slot = i;
    s_load.steps = 0;
    s_load.target = p->iris_av;
    ESP_LOGI(TAG, "Loading preset %d", i + 1);
    if (p->iris_av == INT16_MIN || !s_m.cam.have_iris) {
        preset_load_done(true);
        return;
    }
    s_load.active = true;
    s_load.waiting = false;
    s_load.dir = 0;
    s_load.next_at = 0;
    preset_status("Loading %s…", i);
}

/*
 * Iris of a preset load: one step at a time, each confirmed by the camera's aperture report. A
 * reversal of direction means the saved stop was passed, so the lens is at the nearest stop.
 */
static void preset_tick(void)
{
    if (!s_load.active) {
        return;
    }
    const cam_state_t *c = &s_m.cam;
    if (c->link != CAM_LINK_CONNECTED || !c->have_iris) {
        s_load.active = false;
        return;
    }
    const int64_t now = esp_timer_get_time();
    s_toast_until = now + TOAST_US;     /* "Loading preset" stays up until the end */
    /* A step counts once the camera reports another aperture; without that by the deadline the load ends */
    if (s_load.waiting) {
        if (c->iris_av == s_load.before) {
            if (now >= s_load.deadline) {
                preset_load_done(false);
            }
            return;
        }
        s_load.waiting = false;
        s_load.next_at = now + IRIS_SETTLE_US;
    }
    if (now < s_load.next_at) {
        return;
    }
    const int16_t before = c->iris_av;
    const int dir = s_load.target > before ? 1 : -1;
    if (abs(before - s_load.target) < IRIS_CLOSE_ENOUGH || (s_load.dir != 0 && dir != s_load.dir)) {
        preset_load_done(true);
        return;
    }
    if (s_load.steps++ >= IRIS_MAX_STEPS) {
        preset_load_done(false);
        return;
    }
    cam_iris_step(dir);
    s_load.before = before;
    s_load.dir = dir;
    s_load.waiting = true;
    s_load.deadline = now + IRIS_REPORT_US;
}

/* Empties preset i (second CLEAR tap) and stores that; the preset stays as it was if NVS fails */
static void preset_clear(int i)
{
    if (!s_m.presets[i].used) {
        preset_status("%s is already empty", i);
        return;
    }
    char name[UI_PRESET_NAME_MAX + 8];
    ui_preset_name(&s_m, i, name, sizeof(name));
    /* The name goes too: an empty preset is "Preset <n>" again */
    const ui_preset_t before = s_m.presets[i];
    s_m.presets[i] = (ui_preset_t){0};
    if (!presets_save()) {
        s_m.presets[i] = before;
        toast("Could not clear the preset");
        return;
    }
    if (s_m.preset_active == i) {
        set_active_preset(-1);
    }
    ESP_LOGI(TAG, "Preset %d cleared", i + 1);
    toast("%s cleared", name);
}

/* Stores a new name; the default name ("Preset <n>") is stored as no name */
static void preset_rename(int i, const char *text)
{
    char name[UI_PRESET_NAME_MAX + 1], def[16];
    snprintf(name, sizeof(name), "%s", text);
    snprintf(def, sizeof(def), "Preset %d", i + 1);
    if (strcmp(name, def) == 0) {
        name[0] = 0;
    }
    if (strcmp(name, s_m.presets[i].name) == 0) {
        return;
    }
    char before[sizeof(name)];
    strcpy(before, s_m.presets[i].name);
    strcpy(s_m.presets[i].name, name);
    if (!presets_save()) {
        strcpy(s_m.presets[i].name, before);
        toast("Could not rename the preset");
        return;
    }
    ESP_LOGI(TAG, "Preset %d renamed to \"%s\"", i + 1, name);
    if (s_m.preset_active == i) {
        s_dirty |= D_PRESET;
    }
    preset_status("Renamed to %s", i);
}

/* ---- On-screen keyboard ---- */

/* The keyboard for a preset's name (slot) or, with slot -1, a slate text (param): starting with its text, up to
   max characters (at most CAM_SLATE_TEXT - 1) */
static void open_keyboard(int slot, uint8_t param, const char *text, int max)
{
    s_m.kb_slot = (int8_t)slot;
    s_m.kb_param = param;
    s_m.kb_max = (uint8_t)max;
    snprintf(s_m.kb_text, (size_t)max + 1, "%s", text);
    s_m.kb_len = (uint8_t)strlen(s_m.kb_text);
    s_m.kb_shift = s_m.kb_len == 0;         /* a new text starts with a capital */
    open_page(PAGE_KEYBOARD);
}

/* Sends the typed slate text if it changed */
static void slate_save_text(void)
{
    if (s_m.cam.link == CAM_LINK_CONNECTED && strcmp(s_m.kb_text, s_m.slate.text[s_m.kb_param]) != 0) {
        cam_set_slate_text(s_m.kb_param, s_m.kb_text);
    }
}

/* A key of the on-screen keyboard: a printable ASCII character, or UI_KB_* */
static void kb_key(int code)
{
    s_dirty |= D_ALL;
    switch (code) {
    case UI_KB_SHIFT:
        /* Off, next letter, caps lock */
        s_m.kb_shift = (uint8_t)((s_m.kb_shift + 1) % 3);
        return;
    case UI_KB_DEL:
        if (s_m.kb_len > 0) {
            s_m.kb_text[--s_m.kb_len] = 0;
        }
        if (s_m.kb_len == 0 && s_m.kb_shift == 0) {
            s_m.kb_shift = 1;
        }
        return;
    case UI_KB_CANCEL:
        open_page(s_m.kb_slot >= 0 ? PAGE_PRESETS : PAGE_SLATE);
        return;
    case UI_KB_SAVE:
        /* Without trailing spaces */
        while (s_m.kb_len > 0 && s_m.kb_text[s_m.kb_len - 1] == ' ') {
            s_m.kb_text[--s_m.kb_len] = 0;
        }
        if (s_m.kb_slot >= 0) {
            preset_rename(s_m.kb_slot, s_m.kb_text);
            open_page(PAGE_PRESETS);
        } else {
            slate_save_text();
            open_page(PAGE_SLATE);
        }
        return;
    default:
        break;
    }
    if (code < ' ' || code > '~' || s_m.kb_len >= s_m.kb_max || (code == ' ' && s_m.kb_len == 0)) {
        return;
    }
    char c = (char)code;
    if (c >= 'a' && c <= 'z' && s_m.kb_shift) {
        c = (char)(c - 'a' + 'A');
        if (s_m.kb_shift == 1) {
            s_m.kb_shift = 0;
        }
    }
    s_m.kb_text[s_m.kb_len++] = c;
    s_m.kb_text[s_m.kb_len] = 0;
}

/* ---- Slate ---- */

/* A control of the slate page (ui_slate_item_t): a text opens the keyboard, the rest goes to the camera at once */
static void slate_item(int item)
{
    const cam_slate_t *s = &s_m.slate;
    if (!need_camera() || !s->have) {
        return;
    }
    const int param = ui_slate_item_param(item);
    if (param >= 0) {
        open_keyboard(-1, (uint8_t)param, s_m.slate.text[param], ui_slate_text_max((uint8_t)param));
        return;
    }
    switch (item) {
    case SL_TAKE_DEC:
    case SL_TAKE_INC:
        cam_set_slate_take((int8_t)clampi(s->take + (item == SL_TAKE_INC ? 1 : -1), 1, 99), s->take_tag);
        break;
    case SL_GOOD_TAKE:
        cam_set_slate_good_take(!s->good_take);
        break;
    case SL_EXTERIOR:
    case SL_INTERIOR:
        cam_set_slate_tags(s->shot, item == SL_INTERIOR, s->day);
        break;
    case SL_DAY:
    case SL_NIGHT:
        cam_set_slate_tags(s->shot, s->interior, item == SL_DAY);
        break;
    case SL_CAMERA_DEC:
    case SL_CAMERA_INC: {
        /* Camera letter, A … Z */
        const char c = s->text[CAM_SLATE_CAMERA_ID][0];
        const int k = ((c >= 'A' && c <= 'Z' ? c - 'A' : 0) + (item == SL_CAMERA_INC ? 1 : 25)) % 26;
        const char id[2] = {(char)('A' + k), 0};
        cam_set_slate_text(CAM_SLATE_CAMERA_ID, id);
        break;
    }
    default:
        if (item >= SL_TAKE_TAG && item < SL_TAKE_TAG + 4) {
            cam_set_slate_take(s->take, (int8_t)(item - SL_TAKE_TAG - 1));
        } else if (item >= SL_SHOT && item < SL_SHOT + UI_SLATE_SHOT_COUNT) {
            cam_set_slate_tags(UI_SLATE_SHOTS[item - SL_SHOT], s->interior, s->day);
        }
        break;
    }
}

/* Scene number one up or down: the number at its start changes, letters after it go */
static void scene_step(int dir)
{
    const int n = clampi(atoi(s_m.slate.text[CAM_SLATE_SCENE]) + dir, 1, 9999);
    char t[8];
    snprintf(t, sizeof(t), "%d", n);
    cam_set_slate_text(CAM_SLATE_SCENE, t);
}

/* ▲ ▼ and buttons of the quick slate: the scene, then what the slate page also has */
static void slate_quick_step(int index)
{
    static const int items[6] = {SL_TAKE_INC, SL_TAKE_DEC, SL_INTERIOR, SL_EXTERIOR, SL_DAY, SL_NIGHT};
    if (index >= 2) {
        slate_item(items[index - 2]);
    } else if (need_camera() && s_m.slate.have) {
        scene_step(index == 0 ? 1 : -1);
    }
}

/* Settings the camera did not take: it shows its own again, say where to set them */
static void refused_tick(void)
{
    const uint8_t r = cam_check_refused();
    if (r & CAM_REFUSED_CODEC) {
        toast("BRAW/ProRes: switch it on the camera");
    } else if (r & CAM_REFUSED_RANGE) {
        toast("Set this dynamic range on the camera");
    } else if (r & CAM_REFUSED_FORMAT) {
        toast(s_m.cam.codec == CAM_CODEC_PRORES ? "ProRes resolutions: set them on the camera"
                                                : "The camera did not take this setting");
    }
}

/* ---- Focus ---- */

/* Focus from the slider position, 0 (near) .. 1 (far) */
static void focus_to(float f)
{
    if (s_m.cam.link != CAM_LINK_CONNECTED) {
        return;
    }
    const int16_t pos = (int16_t)lroundf(f * CAM_FOCUS_MAX);
    if (abs(pos - s_m.cam.focus) >= 4) {
        cam_set_focus(pos);
    }
}

/* A control of the settings menu (ui_menu_item_t); all but forget pairing and live view need the camera */
static void menu_item(int item)
{
    if (item == MI_FORGET) {
        cam_ble_forget();
        toast("Pairing removed");
        return;
    }
    if (item >= MI_LIVE_VIEW) {
        const bool on = item == MI_LIVE_VIEW;
        if (on != s_m.set.live_view) {
            /* Saved at once: it decides whether the USB-A port is powered at the next start */
            s_m.set.live_view = on;
            s_video_wait_until = esp_timer_get_time() + VIDEO_START_US;
            settings_save();
            if (s_live_view_handler) {
                s_live_view_handler(on);
            }
            s_dirty |= D_ALL;
        }
        return;
    }
    if (!need_camera()) {
        return;
    }
    const cam_state_t *c = &s_m.cam;
    /* A codec or quality mode comes back with the quality last used in it */
    if (item == MI_CODEC_BRAW) {
        if (c->codec != CAM_CODEC_BRAW) {
            cam_set_codec(CAM_CODEC_BRAW, s_last_braw);
        }
    } else if (item == MI_CODEC_PRORES) {
        if (c->codec != CAM_CODEC_PRORES) {
            cam_set_codec(CAM_CODEC_PRORES, s_last_prores);
        }
    } else if (item == MI_BRAW_BITRATE || item == MI_BRAW_QUALITY) {
        const bool quality = item == MI_BRAW_QUALITY;
        if (c->codec != CAM_CODEC_BRAW || ui_braw_quality(c->codec_variant) != quality) {
            cam_set_codec(CAM_CODEC_BRAW, quality ? s_last_braw_quality : s_last_braw_bitrate);
        }
    } else if (item >= MI_CODEC_VARIANT && item < MI_RESOLUTION) {
        const ui_codec_t *k = &UI_CODECS[item - MI_CODEC_VARIANT];
        cam_set_codec(k->codec, k->variant);
    } else if (item >= MI_RESOLUTION && item < MI_DYN_RANGE) {
        const ui_resolution_t *r = &UI_RESOLUTIONS[item - MI_RESOLUTION];
        if (!ui_resolution_available(r, c->codec)) {
            return;
        }
        if (!ui_resolution_takes_fps(r, c)) {
            toast("%s: up to %d fps. Lower the frame rate first", r->label, r->max_fps);
            return;
        }
        /* Off speed above what the new resolution allows comes down to its maximum first */
        if (ui_off_speed(&s_m) && c->sensor_fps > r->max_fps) {
            cam_set_off_speed(true, r->max_fps);
        }
        cam_set_resolution(r->w, r->h, r->windowed);
    } else if (item >= MI_DYN_RANGE && item < MI_FPS_DEC) {
        cam_set_dynamic_range((int8_t)(item - MI_DYN_RANGE));
    } else if (item == MI_FPS_DEC || item == MI_FPS_INC) {
        step_project_fps(item == MI_FPS_INC ? 1 : -1);
    } else if (item == MI_OFFSPEED_TOGGLE) {
        toggle_off_speed();
    } else if (item == MI_OFFSPEED_DEC || item == MI_OFFSPEED_INC) {
        if (ui_off_speed(&s_m)) {
            set_off_speed_fps(c->sensor_fps + (item == MI_OFFSPEED_INC ? 1 : -1));
        }
    } else if (item >= MI_SHARPEN && item < MI_TC_SOURCE) {
        cam_set_sharpening((int8_t)(item - MI_SHARPEN));
    } else if (item >= MI_TC_SOURCE && item < MI_FORGET) {
        cam_set_tc_source((uint8_t)(item - MI_TC_SOURCE));
    }
}

/* ---- Touch ---- */

/* The pressed control (s_press, of kind k) repeats while held: the steps up and down, and DEL */
static bool repeats(ui_hit_kind_t k)
{
    return k == HIT_DEC || k == HIT_INC || (k == HIT_SLATE_QUICK && s_press.index < 4) ||
           (k == HIT_KB_KEY && s_press.index == UI_KB_DEL) ||
           (k == HIT_SLATE_ITEM && (s_press.index == SL_TAKE_DEC || s_press.index == SL_TAKE_INC ||
                                    s_press.index == SL_CAMERA_DEC || s_press.index == SL_CAMERA_INC)) ||
           (k == HIT_MENU_ITEM && (s_press.index == MI_FPS_DEC || s_press.index == MI_FPS_INC ||
                                   s_press.index == MI_OFFSPEED_DEC || s_press.index == MI_OFFSPEED_INC));
}

/* Actions that happen as soon as the finger lands */
static void press(void)
{
    switch (s_press.kind) {
    case HIT_SLIDER:
        slide(s_press.index, s_press.frac);
        break;
    case HIT_DEC:
    case HIT_INC:
        step_value(s_press.index, s_press.kind == HIT_INC ? 1 : -1);
        break;
    case HIT_PIN_KEY:
        if (s_press.index <= 9 && s_m.pin_len < 6) {
            s_m.pin[s_m.pin_len++] = (char)('0' + s_press.index);
            s_m.pin[s_m.pin_len] = 0;
        } else if (s_press.index == 10 && s_m.pin_len > 0) {
            s_m.pin[--s_m.pin_len] = 0;
        } else if (s_press.index == 11 && s_m.pin_len == 6) {
            cam_ble_submit_pin((uint32_t)atoi(s_m.pin));
            open_page(PAGE_NONE);
            toast("Pairing");
        }
        s_dirty |= D_ALL;
        break;
    case HIT_KB_KEY:
        kb_key(s_press.index);
        break;
    case HIT_SLATE_ITEM:
        slate_item(s_press.index);
        break;
    case HIT_MENU_ITEM:
        menu_item(s_press.index);
        break;
    case HIT_SLATE_QUICK:
        slate_quick_step(s_press.index);
        break;
    case HIT_FOCUS:
        if (need_camera()) {
            focus_to(s_press.frac);
        }
        break;
    default:
        break;
    }
}

/* Actions of a tap: buttons act as the finger lands, the picture and the slate as it lifts without having moved */
static void tap(void)
{
    const ui_hit_t h = s_press;
    if (s_m.page == PAGE_PRESETS && s_m.preset_armed >= 0 && h.kind != HIT_PRESETS_CLEAR &&
        !(h.kind == HIT_PRESETS_SD_LOAD && s_m.preset_armed == UI_PRESET_ARMED_SD)) {
        /* Any other tap takes back the first CLEAR or LOAD FROM SD tap */
        s_m.preset_armed = -1;
        s_dirty |= D_ALL;
    }
    switch (h.kind) {
    case HIT_VIDEO:
        if (s_m.popup != POP_NONE) {
            open_popup(POP_NONE);
        }
        close_slate_quick();
        break;
    case HIT_FRAME:
        open_popup(POP_TOOLS);
        break;
    case HIT_PLAY_CLIP:
        if (need_camera()) {
            cam_play(1);
            open_popup(POP_PLAYBACK);
        }
        break;
    case HIT_PLAYBACK:
        playback_action(h.index);
        break;
    case HIT_FPS:
    case HIT_SHUTTER:
    case HIT_IRIS:
    case HIT_ISO:
    case HIT_WB: {
        static const ui_popup_t map[] = {
            [HIT_FPS] = POP_FPS, [HIT_SHUTTER] = POP_SHUTTER, [HIT_IRIS] = POP_IRIS, [HIT_ISO] = POP_ISO, [HIT_WB] = POP_WB,
        };
        /* Its own panel closes without the camera too */
        if (s_m.popup == map[h.kind] || need_camera()) {
            open_popup(map[h.kind]);
        }
        break;
    }
    case HIT_TC:
        if (need_camera()) {
            cam_set_tc_source(s_m.cam.tc_source ? 0 : 1);
        }
        break;
    case HIT_BT:
    case HIT_SETTINGS:
    case HIT_FORMAT:
        /* Bluetooth opens the REMOTE tab; settings and the format RECORD, where codec and resolution are set */
        s_m.menu_tab = h.kind == HIT_BT;
        s_m.menu_page = 0;
        open_page(PAGE_MENU);
        break;
    case HIT_RECORD:
        if (need_camera()) {
            cam_record(s_m.cam.transport_mode != 2);
        }
        break;
    case HIT_POWER:
        /* AC (as on the camera) or the battery voltage */
        s_m.set.power_voltage = !s_m.set.power_voltage;
        settings_changed();
        s_dirty |= D_POWER;
        break;
    case HIT_MON_TOGGLE: {
        const cam_state_t *c = &s_m.cam;
        if (!need_camera()) {
            break;
        }
        if (!ui_mon_enabled(&s_m, h.index)) {
            toast("Clean feed is on");
            break;
        }
        const bool guides = c->overlay_bits & CAM_OVL_GUIDES, clean = c->overlay_bits & CAM_OVL_CLEAN_FEED;
        switch (h.index) {
        case MON_CLEAN_FEED:
            /* On HDMI, which the Tab5 shows (the camera's status text stays off there), or the LCD */
            mon_overlays((guides ? CAM_OVL_GUIDES : 0) | (clean ? 0 : CAM_OVL_CLEAN_FEED));
            break;
        case MON_ZEBRA:
            cam_set_tools(c->tools ^ CAM_TOOL_ZEBRA, mon_display());
            break;
        case MON_FOCUS_ASSIST:
            cam_set_tools(c->tools ^ CAM_TOOL_FOCUS_ASSIST, mon_display());
            break;
        case MON_FALSE_COLOR:
            cam_set_tools(c->tools ^ CAM_TOOL_FALSE_COLOR, mon_display());
            break;
        case MON_GUIDE:
            set_guides(!guides);
            break;
        case MON_GRID:
            set_grid_safe(true, !c->grid);
            break;
        case MON_SAFE_AREA:
            set_grid_safe(false, !c->safe_area);
            break;
        default:
            break;
        }
        s_dirty |= D_STATUS;
        break;
    }
    case HIT_ZOOM_OUT:
        zoom_set(false, 0, 0);
        break;
    case HIT_VIEW:
        /* Simple view: no slate and codec/quality on the main screen */
        s_m.set.simple_view = !s_m.set.simple_view;
        settings_changed();
        close_slate_quick();
        s_dirty |= D_STATUS | D_BOTTOM;
        break;
    case HIT_M5BAT:
        /* Tab5 battery: percentage or voltage */
        s_m.set.m5bat_voltage = !s_m.set.m5bat_voltage;
        settings_changed();
        s_dirty |= D_M5BAT;
        break;
    case HIT_PRESETS_OPEN:
        /* On the page of the loaded preset */
        s_m.preset_armed = -1;
        s_m.preset_page = (uint8_t)(s_m.preset_active >= 0 ? s_m.preset_active / UI_PRESETS_PER_PAGE : 0);
        open_page(PAGE_PRESETS);
        break;
    case HIT_PRESETS_SAVE:
        preset_save(h.index);
        s_dirty |= D_ALL;
        break;
    case HIT_PRESETS_LOAD:
        preset_load(h.index);
        s_dirty |= D_ALL;
        break;
    case HIT_PRESETS_RENAME:
        open_keyboard(h.index, 0, s_m.presets[h.index].name, UI_PRESET_NAME_MAX);
        break;
    case HIT_SLATE_OPEN:
        /* Short press: scene and take above the slate (a long press opens the slate page) */
        s_m.slate_quick = !s_m.slate_quick;
        s_dirty |= D_SLATE_QUICK;
        break;
    case HIT_SLATE_TAB:
        s_m.slate_tab = (uint8_t)h.index;
        s_dirty |= D_ALL;
        break;
    case HIT_AF:
        if (need_camera()) {
            cam_autofocus();
            toast("Autofocus");
        }
        break;
    case HIT_PRESETS_SD_SAVE:
        presets_to_sd();
        break;
    case HIT_PRESETS_SD_LOAD:
        if (s_m.preset_armed == UI_PRESET_ARMED_SD) {
            s_m.preset_armed = -1;
            presets_from_sd();
        } else {
            s_m.preset_armed = UI_PRESET_ARMED_SD;
            toast("Tap again to replace all presets from SD");
        }
        s_dirty |= D_ALL;
        break;
    case HIT_PRESETS_CLEAR:
        if (s_m.preset_armed == h.index) {
            s_m.preset_armed = -1;
            preset_clear(h.index);
        } else {
            s_m.preset_armed = (int8_t)h.index;
            preset_status("Tap CLEAR again to erase %s", h.index);
        }
        s_dirty |= D_ALL;
        break;
    case HIT_TOGGLE:
        toggle();
        break;
    case HIT_PRESET:
        preset(h.index);
        break;
    case HIT_TOOL:
        if (h.index < TOOL_COUNT) {
            s_m.tool = (ui_tool_t)h.index;
            s_dirty |= D_BOTTOM;
        }
        break;
    case HIT_PIN_CANCEL:
        cam_ble_cancel_pin();
        open_page(PAGE_NONE);
        break;
    case HIT_MENU_TAB:
        s_m.menu_tab = (uint8_t)h.index;
        s_m.menu_page = 0;
        s_dirty |= D_ALL;
        break;
    case HIT_PAGE_BACK:
        open_page(PAGE_NONE);
        break;
    case HIT_PAGE_PREV:
    case HIT_PAGE_NEXT: {
        uint8_t *page = s_m.page == PAGE_PRESETS ? &s_m.preset_page : &s_m.menu_page;
        *page = (uint8_t)clampi(*page + (h.kind == HIT_PAGE_NEXT ? 1 : -1), 0, ui_page_count(&s_m) - 1);
        s_dirty |= D_ALL;
        break;
    }
    default:
        break;
    }
}

/* Up to two touches in panel coordinates (the landscape view, from the portrait touch panel) */
static int read_touch(int *xs, int *ys)
{
    if (!s_tp || esp_lcd_touch_read_data(s_tp) != ESP_OK) {
        return 0;
    }
    esp_lcd_touch_point_data_t pt[2];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(s_tp, pt, &n, 2) != ESP_OK) {
        return 0;
    }
    for (int i = 0; i < n && i < 2; i++) {
#if CONFIG_HDMI_ROTATION_270
        xs[i] = pt[i].y;
        ys[i] = BSP_LCD_H_RES - 1 - pt[i].x;
#else
        xs[i] = BSP_LCD_V_RES - 1 - pt[i].y;
        ys[i] = pt[i].x;
#endif
#if CONFIG_HDMI_TOUCH_FLIP
        xs[i] = OV_W - 1 - xs[i];
        ys[i] = OV_H - 1 - ys[i];
#endif
    }
    return n < 2 ? n : 2;
}

/* Two fingers: spreading them zooms in 2x on the point between them, pinching zooms out */
static void pinch(const int *xs, const int *ys)
{
    const float dx = (float)(xs[1] - xs[0]), dy = (float)(ys[1] - ys[0]), d = sqrtf(dx * dx + dy * dy);
    if (!s_pinch) {
        s_pinch = true;
        s_pinch_done = false;
        s_pinch_d0 = d;
        s_moved = true;                 /* the first finger's touch is no tap */
        return;
    }
    if (s_pinch_done || s_m.page != PAGE_NONE) {
        return;
    }
    /* Only the live picture zooms */
    if (!s_m.zoomed && s_m.live && d > s_pinch_d0 * 1.3f) {
        zoom_set(true, (xs[0] + xs[1]) / 2.0f, (ys[0] + ys[1]) / 2.0f);
        s_pinch_done = true;
    } else if (s_m.zoomed && d < s_pinch_d0 * 0.75f) {
        zoom_set(false, 0, 0);
        s_pinch_done = true;
    }
}

/* Reads the touch panel and acts on it: press, tap, drag, long press, repeats of a held control, pinch */
static void poll_touch(void)
{
    int xs[2] = {0, 0}, ys[2] = {0, 0};
    const int n = read_touch(xs, ys);
    const int64_t now = esp_timer_get_time();
    if (n == 2) {
        pinch(xs, ys);
        s_down = true;
        return;
    }
    if (s_pinch) {
        /* After a pinch, nothing until every finger is up */
        if (n == 0) {
            s_pinch = false;
            s_down = false;
        }
        return;
    }
    const bool down = n == 1;
    const int x = xs[0], y = ys[0];
    if (down && !s_down) {
        s_x0 = s_x = x;
        s_y0 = s_y = y;
        s_moved = false;
        s_press = ui_hit_test(&s_m, x, y);
        ESP_LOGD(TAG, "touch %d,%d -> %d/%d", x, y, s_press.kind, s_press.index);
        s_next_repeat = now + REPEAT_DELAY_US;
        s_press_at = now;
        s_long_done = false;
        press();
        /* Buttons act as the finger lands. The picture (drag, pinch) and the slate (tap or hold) wait
           for it to lift; sliders and focus follow the finger */
        const ui_hit_kind_t k = s_press.kind;
        s_tapped = k != HIT_VIDEO && k != HIT_SLATE_OPEN && k != HIT_SLIDER && k != HIT_FOCUS;
        if (s_tapped) {
            tap();
        }
    } else if (down) {
        if (abs(x - s_x0) > TAP_SLOP || abs(y - s_y0) > TAP_SLOP) {
            s_moved = true;
        }
        if (s_m.zoomed && s_press.kind == HIT_VIDEO && s_moved) {
            /* One finger moves the zoomed area: the picture follows the finger, at half its travel */
            float vx, vy;
            ui_zoom(&vx, &vy);
            zoom_set(true, vx - (x - s_x) / 2.0f, vy - (y - s_y) / 2.0f);
        }
        s_x = x;
        s_y = y;
        if (s_press.kind == HIT_SLIDER) {
            slide(s_press.index, ui_slider_frac_at(&s_m, x));
        } else if (s_press.kind == HIT_FOCUS) {
            focus_to(ui_focus_frac_at(&s_m, x));
        } else if (s_press.kind == HIT_SLATE_OPEN && !s_moved && !s_long_done && now - s_press_at >= LONG_PRESS_US) {
            /* Long press on the slate: the full slate page */
            s_long_done = true;
            open_page(PAGE_SLATE);
        } else if (repeats(s_press.kind) && !s_moved && now >= s_next_repeat) {
            s_next_repeat = now + REPEAT_US;
            press();
        }
    } else if (s_down) {
        /* Lifted: a tap on the picture or the slate acts now, unless the finger moved or it was a long press */
        if (!s_tapped && !s_moved && !s_long_done && s_press.kind != HIT_SLIDER && s_press.kind != HIT_FOCUS) {
            tap();
        }
    }
    s_down = down;
}

/* ---- Redraw ---- */

/*
 * Takes in what changed and sets the redraw flags: the camera's state and slate, the HDMI picture, the
 * Tab5 battery; also does what is due (camera status text off, message timeout, settings save).
 */
static void take_updates(void)
{
    const uint32_t ch = cam_take_changes();
    const cam_link_t before = s_m.cam.link;
    if (ch) {
        cam_get(&s_m.cam);
    }
    if (ch & CAM_CH_LINK) {
        s_dirty |= D_TOP | D_STATUS | D_BOTTOM;
        if (s_m.cam.link == CAM_LINK_CONNECTED && before != CAM_LINK_CONNECTED) {
            /* Once the camera has sent its settings */
            s_status_off_at = esp_timer_get_time() + 2000000;
        }
        if (s_m.cam.link == CAM_LINK_PIN && before != CAM_LINK_PIN) {
            s_m.pin_len = 0;
            s_m.pin[0] = 0;
            open_page(PAGE_PIN);
        } else if (s_m.page == PAGE_PIN && s_m.cam.link != CAM_LINK_PIN) {
            open_page(PAGE_NONE);
        }
        /* Panels of camera values close with the link; the tools panel stays (it has the Tab5's brightness) */
        if (s_m.cam.link != CAM_LINK_CONNECTED && s_m.popup != POP_NONE && s_m.popup != POP_TOOLS) {
            s_m.popup = POP_NONE;
        }
        snprintf(s_m.cp_version, sizeof(s_m.cp_version), "%s", cam_ble_cp_version());
    }
    if (ch & CAM_CH_VALUES) {
        s_dirty |= D_TOP;
        const cam_state_t *c = &s_m.cam;
        /* A white balance that is none of the presets is the one CWB goes back to */
        if (c->have_wb && c->wb_kelvin > 0 && ui_wb_preset_index(c->wb_kelvin, c->wb_tint) < 0) {
            s_custom_kelvin = c->wb_kelvin;
            s_custom_tint = c->wb_tint;
        }
        if (s_m.popup != POP_NONE) {
            s_dirty |= D_BOTTOM;
        }
    }
    if (ch & CAM_CH_POWER) {
        /* Reported every second; the badge usually reads the same */
        char p[sizeof(s_power_drawn)];
        ui_power_text(&s_m, p, sizeof(p));
        if (strcmp(p, s_power_drawn) != 0) {
            s_dirty |= D_POWER;
        }
    }
    if (ch & CAM_CH_TIMECODE) {
        s_dirty |= D_TC;
    }
    if (ch & CAM_CH_TRANSPORT) {
        s_dirty |= D_TC | D_RECORD | D_SLOTS;
        /* Playback started on the camera opens its panel, and leaving it closes the panel */
        if (s_m.cam.transport_mode == 1 && s_m.popup != POP_PLAYBACK && s_m.page == PAGE_NONE) {
            open_popup(POP_PLAYBACK);
        } else if (s_m.cam.transport_mode != 1 && s_m.popup == POP_PLAYBACK) {
            s_m.popup = POP_NONE;
            s_dirty |= D_TOP | D_STATUS | D_BOTTOM;
        }
        if (s_m.popup == POP_PLAYBACK) {
            s_dirty |= D_BOTTOM;
        }
    }
    if (ch & CAM_CH_MEDIA) {
        s_dirty |= D_SLOTS;
    }
    if (ch & CAM_CH_FORMAT) {
        s_dirty |= D_STATUS | D_FORMAT;
        if (s_m.popup == POP_TOOLS) {
            s_dirty |= D_BOTTOM;
        }
        /* The camera's frame guide, grid type and safe area, to switch them on with next time */
        const cam_state_t *c = &s_m.cam;
        ui_settings_t *s = &s_m.set;
        const int guide = c->have_overlays ? ui_guide_index(c->guide_style) : -1;
        if (guide >= 0 && guide != s->guide_ratio) {
            s->guide_ratio = (uint8_t)guide;
            settings_changed();
        }
        const int style = (c->grid & 3) - 1;
        if (c->have_overlays && style >= 0 && style != s->grid_style) {
            s->grid_style = (uint8_t)style;
            settings_changed();
        }
        if (c->have_overlays && c->safe_area > 0 && c->safe_area != s->safe_pct) {
            s->safe_pct = (uint8_t)c->safe_area;
            settings_changed();
        }
        if (c->codec == CAM_CODEC_PRORES && c->codec_variant >= 0) {
            s_last_prores = c->codec_variant;
        } else if (c->codec == CAM_CODEC_BRAW && c->codec_variant >= 0) {
            s_last_braw = c->codec_variant;
            if (ui_braw_quality(c->codec_variant)) {
                s_last_braw_quality = c->codec_variant;
            } else {
                s_last_braw_bitrate = c->codec_variant;
            }
        }
    }
    if (ch & CAM_CH_FOCUS) {
        s_dirty |= D_FOCUS;
    }
    if (ch & CAM_CH_SLATE) {
        cam_get_slate(&s_m.slate);
        s_dirty |= D_SLATE;
        if (s_m.popup == POP_PLAYBACK) {
            s_dirty |= D_BOTTOM;        /* the clip and its good take */
        }
    }
    /* Pages are redrawn whole, for what they show of the camera (the keyboard: nothing) */
    uint32_t page_ch = 0;
    switch (s_m.page) {
    case PAGE_NONE:
    case PAGE_KEYBOARD:
        break;
    case PAGE_PRESETS:
        page_ch = CAM_CH_LINK;
        break;
    case PAGE_SLATE:
        page_ch = CAM_CH_LINK | CAM_CH_SLATE;
        break;
    default:
        page_ch = ~(CAM_CH_TIMECODE | CAM_CH_FOCUS | CAM_CH_SLATE);
        break;
    }
    if (ch & page_ch) {
        s_dirty |= D_ALL;
    }

    /* The HDMI format from ui_set_video(); the menu shows it */
    taskENTER_CRITICAL(&s_mux);
    const bool video = s_video_new;
    if (video) {
        memcpy(s_m.hdmi_info, s_video_info, sizeof(s_m.hdmi_info));
        s_video_new = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (video && s_m.page == PAGE_MENU) {
        s_dirty |= D_ALL;
    }

    const int64_t now = esp_timer_get_time();
    /* Live view as shown, see VIDEO_GRACE_US */
    if (s_m.hdmi_info[0]) {
        s_video_seen_at = now;
    }
    const bool live = s_m.set.live_view && (s_m.hdmi_info[0] || now < s_video_wait_until ||
                                            (s_video_seen_at && now - s_video_seen_at < VIDEO_GRACE_US));
    if (live != s_m.live) {
        s_m.live = live;
        s_dirty |= D_ALL;
        if (!live) {
            zoom_set(false, 0, 0);
        }
        ESP_LOGI(TAG, "%s", live ? "Picture from the HDMI adapter: live view" : "No HDMI adapter: shown as without live view");
    }
    if (now >= s_battery_at) {
        /* Smoothed, so the percentage does not flicker with the load */
        s_battery_at = now + BATTERY_US;
        const int mv = tab5_bat_mv();
        s_battery_avg = mv <= 0 ? 0 : (s_battery_avg <= 0 ? mv : (s_battery_avg * 3 + mv) / 4);
        s_m.m5_mv = (int16_t)s_battery_avg;
        s_m.m5_pct = (int8_t)tab5_bat_percent(s_battery_avg);
        /* Outside power (USB-C) unless current flows out of a pack; changed after two readings agree */
        const bool external = mv <= 0 || tab5_bat_discharge_ma() < 50;
        s_power_votes = external == s_m.m5_external ? 0 : s_power_votes + 1;
        if (s_power_votes >= 2 || s_m.m5_pct < 0 || !s_power_known) {
            s_m.m5_external = external;
            s_power_votes = 0;
            s_power_known = true;
        }
        char t[sizeof(s_m5bat_drawn)];
        ui_m5bat_text(&s_m, t, sizeof(t));
        if (strcmp(t, s_m5bat_drawn) != 0) {
            /* Pages show it in their title bar and are redrawn whole; the PIN page has none */
            s_dirty |= s_m.page != PAGE_NONE && s_m.page != PAGE_PIN ? D_ALL : D_M5BAT;
            ESP_LOGI(TAG, "Tab5 battery %d mV, %d %%, %s", s_m.m5_mv, s_m.m5_pct,
                     s_m.m5_external ? "on outside power" : "on battery");
        }
    }
    if (s_status_off_at && now >= s_status_off_at) {
        /* The camera's own status text stays off the HDMI picture, which the Tab5 shows with its own */
        s_status_off_at = 0;
        if (s_m.cam.link == CAM_LINK_CONNECTED) {
            cam_set_overlay_enables(s_m.cam.overlay_bits & ~CAM_OVL_STATUS, CAM_DISPLAY_HDMI);
            ESP_LOGI(TAG, "Camera status text off on HDMI");
        }
    }
    if (s_m.toast[0] && now > s_toast_until) {
        s_m.toast[0] = 0;
        s_dirty |= toast_dirty();
    }
    if (s_save_at && now > s_save_at) {
        s_save_at = 0;
        settings_save();
    }
}

/* Draws what s_dirty asks for and hands the overlay to the renderer */
static void redraw(void)
{
    const int64_t now = esp_timer_get_time();
    uint32_t d = s_dirty;
    s_dirty = 0;
    if (d & (D_ALL | D_BOTTOM)) {
        d &= ~D_FOCUS;                  /* drawn with the rest */
    } else if ((d & D_FOCUS) && now - s_focus_drawn_at < FOCUS_REDRAW_US) {
        /* Too soon after the last: left for a later loop */
        d &= ~D_FOCUS;
        s_dirty = D_FOCUS;
    }
    /* Pages cover the main screen's parts: they are only drawn whole */
    if (s_m.page != PAGE_NONE) {
        d &= D_ALL;
    }
    if (!d) {
        return;
    }
    if (d & D_FOCUS) {
        s_focus_drawn_at = now;
    }
    /* Only this task draws; overlay_commit() swaps the result in for the renderer */
    if (d & D_ALL) {
        ui_draw_all(&s_m);
    } else {
        if (d & D_TOP) {
            ui_draw_top(&s_m);
        } else {
            if (d & D_TC) {
                ui_draw_timecode(&s_m);
            }
            if (d & D_POWER) {
                ui_draw_power(&s_m);
            }
        }
        if (d & D_STATUS) {
            ui_draw_status(&s_m);       /* with the Tab5 battery */
        } else if (d & D_M5BAT) {
            ui_draw_m5bat(&s_m);
        }
        if ((d & D_SLATE_QUICK) || ((d & D_SLATE) && s_m.slate_quick)) {
            ui_draw_slate_quick(&s_m);
        }
        if (d & D_BOTTOM) {
            ui_draw_bottom(&s_m);
        } else {
            if (d & D_RECORD) {
                ui_draw_record(&s_m);
            }
            if (d & D_SLOTS) {
                ui_draw_slots(&s_m);
            }
            if (d & D_PRESET) {
                ui_draw_preset_name(&s_m);
            }
            if (d & D_FOCUS) {
                ui_draw_focus(&s_m);
            }
            if (d & D_SLATE) {
                ui_draw_slate(&s_m);
            }
            if (d & D_FORMAT) {
                ui_draw_format(&s_m);
            }
        }
    }
    /* What the badges show now, to tell whether a camera or battery report changes them */
    ui_power_text(&s_m, s_power_drawn, sizeof(s_power_drawn));
    ui_m5bat_text(&s_m, s_m5bat_drawn, sizeof(s_m5bat_drawn));
    overlay_commit();
}





/* The FPS panel's off-speed rows slide up or down in FPS_ROWS_US after off-speed is switched */
#define FPS_ROWS_US         180000

/* Moves s_m.fps_rows a step towards where off-speed puts the rows; at once while the FPS panel is closed */
static void fps_rows_tick(void)
{
    static int64_t last;
    const float target = ui_off_speed(&s_m) ? 1.0f : 0.0f;
    if (s_m.popup != POP_FPS || s_m.fps_rows == target) {
        s_m.fps_rows = target;
        last = 0;
        return;
    }
    const int64_t now = esp_timer_get_time();
    const float step = (float)(last ? now - last : POLL_MS * 1000) / FPS_ROWS_US;
    last = now;
    s_m.fps_rows = target > s_m.fps_rows ? fminf(target, s_m.fps_rows + step) : fmaxf(target, s_m.fps_rows - step);
    s_dirty |= D_BOTTOM;
}

/* The UI loop: every POLL_MS, or sooner when the camera or the HDMI input reports a change */
static void ui_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
        poll_touch();
        take_updates();
        preset_tick();
        refused_tick();
        fps_rows_tick();
        redraw();
    }
}

void ui_start(void)
{
    /* In the caller's task: the touch enable line is on the IO expander, whose BSP access is not thread-safe */
    if (bsp_touch_new(NULL, &s_tp) != ESP_OK) {
        ESP_LOGE(TAG, "Touch controller not found");
        s_tp = NULL;
    }
    settings_load();
    presets_load();
    s_m.preset_active = -1;
    s_m.preset_armed = -1;
    s_video_wait_until = esp_timer_get_time() + VIDEO_START_US;
    s_m.live = s_m.set.live_view;
    apply_brightness();
    tab5_bat_init();
    s_m.m5_pct = -1;
    cam_get(&s_m.cam);
    cam_get_slate(&s_m.slate);
    cam_set_change_callback(cam_changed);
    s_dirty = D_ALL;
    /* Core 1: the band renderer (blit task) composites on core 0 and must not wait for drawing */
    xTaskCreatePinnedToCore(ui_task, "ui", 6 * 1024, NULL, 4, &s_task, 1);
}
