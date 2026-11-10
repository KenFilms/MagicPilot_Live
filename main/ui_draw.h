/*
 * Camera UI drawing and layout, in the style of the BMPCC6K's own screen (status bar on top,
 * record button and card slots at the bottom, parameter panels that slide over the bottom bar).
 *
 * Everything here works from a ui_model_t snapshot only (no tasks, no hardware), so the same code
 * draws on the Tab5 and in the desktop preview (tools/preview). The layout constants are shared
 * by drawing and by hit testing so they cannot drift apart.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cam.h"

typedef enum {
    POP_NONE,
    POP_FPS,
    POP_SHUTTER,
    POP_IRIS,
    POP_ISO,
    POP_WB,
    POP_TOOLS,
    POP_PLAYBACK,               /* the camera plays clips: transport, loop, good take */
} ui_popup_t;

/* Controls of the playback panel (HIT_PLAYBACK index) */
typedef enum {
    PB_PREV,                    /* previous clip */
    PB_REW,                     /* backwards: -2, -4, -8 */
    PB_PLAY,                    /* play / pause */
    PB_FF,                      /* forwards: 2, 4, 8 */
    PB_NEXT,                    /* next clip */
    PB_LIVE,                    /* back to the live picture */
    PB_LOOP,
    PB_GOOD_TAKE,
} ui_pb_t;

typedef enum {
    PAGE_NONE,
    PAGE_PIN,
    PAGE_MENU,
    PAGE_PRESETS,
    PAGE_KEYBOARD,              /* renames a preset or types a slate text */
    PAGE_SLATE,
} ui_page_t;

/* Camera preset: the settings MagicPilot_Remote's presets hold, plus off-speed recording and a name */
#define UI_PRESET_COUNT     12
#define UI_PRESETS_PER_PAGE 6
#define UI_PRESET_NAME_MAX  16
#define UI_PRESET_ARMED_SD  UI_PRESET_COUNT
typedef struct {
    bool used;
    uint8_t fps_flags;          /* CAM_FMT_FILE_MRATE, CAM_FMT_SENSOR_MRATE and CAM_FMT_OFF_SPEED as saved */
    int16_t fps;                /* project frame rate */
    int16_t sensor_fps;         /* sensor frame rate (off-speed) */
    int16_t wb_kelvin, wb_tint;
    int16_t iris_av;            /* fixed16 aperture value, INT16_MIN if the lens did not report one */
    int32_t iso;
    int32_t shutter;            /* degrees x 100, or -x for a shutter speed of 1/x second */
    char name[UI_PRESET_NAME_MAX + 1];  /* "" = "Preset <n>" */
    int8_t codec, codec_variant;        /* CAM_CODEC_* and its quality (10.0), -1 = not saved */
    int16_t width, height;              /* resolution, 0 = not saved */
    bool windowed;                      /* the sensor windowed for it (CAM_FMT_WINDOWED) */
} ui_preset_t;

/* Keys of the on-screen keyboard (HIT_KB_KEY index) that are not characters */
#define UI_KB_SHIFT         1
#define UI_KB_DEL           8
#define UI_KB_SAVE          13
#define UI_KB_CANCEL        27

typedef enum {
    TOOL_GUIDES,
    TOOL_GRID,
    TOOL_SAFE,
    TOOL_BRIGHTNESS,
    TOOL_COUNT,
} ui_tool_t;

/* Settings of the Tab5 itself, kept in NVS */
typedef struct {
    bool grid;                  /* unused since the camera draws the grid (kept for the saved layout) */
    uint8_t grid_style;         /* the camera's grid when switched on: 0 thirds, 1 crosshair, 2 both */
    bool guides;                /* unused since the camera draws the frame guides (kept for the saved layout) */
    uint8_t guide_ratio;        /* the camera's frame guide, index into UI_GUIDE_RATIOS */
    uint8_t guide_opacity;      /* unused since the camera draws the frame guides (kept for the saved layout) */
    bool safe;                  /* unused since the camera draws the safe area (kept for the saved layout) */
    uint8_t safe_pct;           /* the camera's safe area when switched on, percent */
    uint8_t brightness;         /* backlight, percent */
    bool power_voltage;         /* power badge shows the voltage even on external power (tap toggles) */
    bool m5bat_voltage;         /* Tab5 battery shows the voltage instead of the percentage (tap toggles) */
    bool live_view;             /* picture from the HDMI capture adapter on USB-A; off = remote only */
    bool simple_view;           /* main screen without slate and codec/quality */
} ui_settings_t;

typedef struct {
    cam_state_t cam;
    ui_settings_t set;
    ui_popup_t popup;
    ui_tool_t tool;             /* tool shown in the tools panel */
    ui_page_t page;
    uint8_t menu_tab;           /* 0 record, 1 remote */
    uint8_t menu_page;
    char pin[7];
    uint8_t pin_len;
    char toast[48];             /* short message under the status bar, "" = none */
    char hdmi_info[40];         /* HDMI input format, "" = none */
    int16_t m5_mv;              /* Tab5 battery voltage, 0 = unknown */
    int8_t m5_pct;              /* Tab5 battery charge, -1 = unknown */
    bool m5_external;           /* the Tab5 runs on outside power (USB-C), not on its battery */
    ui_preset_t presets[UI_PRESET_COUNT];
    int8_t preset_active;       /* last loaded or saved preset, -1 = none */
    int8_t preset_armed;        /* preset waiting for the second CLEAR tap, UI_PRESET_ARMED_SD the second
                                   LOAD FROM SD tap, -1 = none */
    uint8_t preset_page;        /* page of the presets page (UI_PRESETS_PER_PAGE presets each) */
    int8_t kb_slot;             /* preset being renamed, -1 = a slate text */
    uint8_t kb_param;           /* slate text being typed (CAM_SLATE_*) */
    uint8_t kb_max;             /* longest text */
    char kb_text[CAM_SLATE_TEXT];
    uint8_t kb_len;
    uint8_t kb_shift;           /* 0 lower case, 1 next letter upper case, 2 caps lock */
    cam_slate_t slate;
    uint8_t slate_tab;          /* 0 clip, 1 project, 2 lens data */
    bool slate_quick;           /* scene and take with ▲ ▼ above the slate (short press on it) */
    bool zoomed;                /* the picture is zoomed in 2x (pinch) */
    bool live;                  /* live view as shown: on in the settings and the HDMI adapter delivers a picture;
                                   without the adapter the screen is as with live view off */
    float fps_rows;             /* FPS panel: how far its off-speed rows (project frame rate, presets) are up,
                                   0 .. 1, animated when off-speed is switched */
    char cp_version[16];        /* ESP32-C6 firmware */
} ui_model_t;

/* ---- Value lists shared by drawing and the controls ---- */
extern const int32_t UI_ISO_STEPS[];
extern const int UI_ISO_STEP_COUNT;
extern const int32_t UI_ISO_PRESETS[];
extern const int UI_ISO_PRESET_COUNT;
extern const int32_t UI_SHUTTER_STEPS[];
extern const int UI_SHUTTER_STEP_COUNT;
extern const int32_t UI_SHUTTER_SPEEDS[];   /* 1/x second, for a camera that measures the shutter as a speed */
extern const int UI_SHUTTER_SPEED_COUNT;
extern const int16_t UI_PROJECT_FPS[];   /* 23, 29 and 59 stand for 23.98, 29.97 and 59.94 */
extern const int UI_PROJECT_FPS_COUNT;
int16_t ui_project_fps(const cam_state_t *c);   /* the camera's project rate as in UI_PROJECT_FPS */
extern const int16_t UI_OFFSPEED_PRESETS[];
extern const int UI_OFFSPEED_PRESET_COUNT;
#define UI_OFFSPEED_MIN     5
/* What the camera's resolution allows: the highest sensor rate, the project rates and off-speed
   presets up to it (as many from the start of their lists) */
int ui_max_fps(const cam_state_t *c);
int ui_project_fps_count(const cam_state_t *c);
int ui_project_fps_index(const cam_state_t *c);    /* of those, the one nearest the camera's project rate */
int ui_offspeed_preset_count(const cam_state_t *c);
#define UI_WB_MIN           2500
#define UI_WB_MAX           10000
#define UI_WB_STEP          50
typedef struct {
    const char *label;
    int16_t kelvin;             /* 0 = auto white balance (AWB), -1 = the last custom one (CWB) */
    int8_t tint;
} ui_wb_preset_t;
extern const ui_wb_preset_t UI_WB_PRESETS[];
extern const int UI_WB_PRESET_COUNT;
int ui_wb_preset_index(int16_t kelvin, int16_t tint);   /* the preset with these values, -1 = custom */
typedef struct {
    const char *label;
    int8_t style;               /* the camera's frame guide (3.3) */
} ui_guide_t;
extern const ui_guide_t UI_GUIDE_RATIOS[];
extern const int UI_GUIDE_COUNT;
int ui_guide_index(int8_t style);   /* index into UI_GUIDE_RATIOS, -1 = not there */
typedef struct {
    const char *label;
    const char *short_label;    /* for the format box on the main screen */
    int16_t w, h;
    bool windowed;
    uint8_t codecs;             /* codecs the camera records it in: bits 1 << CAM_CODEC_* */
    uint8_t max_fps;            /* highest sensor rate the camera allows there */
} ui_resolution_t;
extern const ui_resolution_t UI_RESOLUTIONS[];
extern const int UI_RESOLUTION_COUNT;
/* The menu's resolution of that size, windowed or not; NULL if the menu has none */
const ui_resolution_t *ui_resolution_find(int16_t w, int16_t h, bool windowed);
/* A resolution's short name as in the format box ("6K", "4K DCI"), else its size in pixels; "" for width 0 */
void ui_fmt_resolution(int16_t w, int16_t h, char *out, size_t size);
typedef struct {
    const char *label;
    int8_t codec, variant;
    bool quality;               /* Blackmagic RAW constant quality (Q0 … Q5), else constant bitrate */
} ui_codec_t;
extern const ui_codec_t UI_CODECS[];
extern const int UI_CODEC_COUNT;
/* Name of a codec (CAM_CODEC_*), e.g. "BRAW" or "ProRes"; NULL if there is none of that number */
const char *ui_codec_name(int codec);
/* Blackmagic RAW variant is one of constant quality */
bool ui_braw_quality(int8_t variant);
/* The camera records this resolution in this codec (any codec if it is unknown) */
bool ui_resolution_available(const ui_resolution_t *r, int8_t codec);
/* The resolution records at the camera's project frame rate */
bool ui_resolution_takes_fps(const ui_resolution_t *r, const cam_state_t *c);

/* Shutter angles on the shutter panel's preset row, in 1/100 degree; as speeds where the camera uses those */
#define UI_SHUTTER_PRESET_COUNT 3
extern const int32_t UI_SHUTTER_PRESETS[UI_SHUTTER_PRESET_COUNT];

/* Auto exposure modes (1.10) on the shutter panel while auto exposure drives the shutter: Shutter,
   Shutter + Iris, Iris + Shutter */
#define UI_AE_MODE_COUNT 3
extern const int8_t UI_SHUTTER_AE_MODES[UI_AE_MODE_COUNT];
extern const int8_t UI_IRIS_AE_MODES[UI_AE_MODE_COUNT];
/* The auto exposure mode (cam_state_t's ae_mode) drives the shutter, or the iris */
bool ui_shutter_auto(int8_t ae_mode);
bool ui_iris_auto(int8_t ae_mode);

/* Frame rate the shutter refers to (the sensor rate when off-speed), 0 if not reported */
int ui_exposure_fps(const cam_state_t *c);
/* Shutter angle x 100 and speed 1/x of the same exposure at a frame rate */
int32_t ui_speed_from_angle(int32_t angle100, int fps);
int32_t ui_angle_from_speed(int32_t speed, int fps);

/* ---- Drawing (caller holds the overlay lock and commits afterwards) ---- */
/* The whole screen: the page shown, or the main screen with the open panel or the quick slate */
void ui_draw_all(const ui_model_t *m);
/* The main screen's status bar along the top, the timecode included */
void ui_draw_top(const ui_model_t *m);
/* Just the status bar's timecode; a tick redraws only the digits that changed */
void ui_draw_timecode(const ui_model_t *m);
/* The bottom bar, or the open panel in its place; nothing while a page is shown */
void ui_draw_bottom(const ui_model_t *m);
void ui_draw_status(const ui_model_t *m);
/* Small parts that the camera updates often */
void ui_draw_power(const ui_model_t *m);
void ui_draw_record(const ui_model_t *m);
void ui_draw_slots(const ui_model_t *m);
void ui_draw_m5bat(const ui_model_t *m);
void ui_draw_preset_name(const ui_model_t *m);
void ui_draw_focus(const ui_model_t *m);
void ui_draw_slate(const ui_model_t *m);
void ui_draw_slate_quick(const ui_model_t *m);
void ui_draw_format(const ui_model_t *m);

/* Name of a preset as shown: its own, or "Preset <n>" */
void ui_preset_name(const ui_model_t *m, int i, char *out, size_t size);

/* Slate: shot types in the order shown (camera codes), and the texts that can be typed */
#define UI_SLATE_SHOT_COUNT 7
extern const int8_t UI_SLATE_SHOTS[UI_SLATE_SHOT_COUNT];
/* Longest text the keyboard takes for a slate text (CAM_SLATE_*), in characters */
int ui_slate_text_max(uint8_t param);
/* Slate text behind a slate page control (CAM_SLATE_*), -1 for other controls */
int ui_slate_item_param(int item);

/* Text of the camera and the Tab5 battery badges */
void ui_power_text(const ui_model_t *m, char *out, size_t size);
void ui_m5bat_text(const ui_model_t *m, char *out, size_t size);

/* ---- Hit testing ---- */
typedef enum {
    HIT_NONE,
    HIT_VIDEO,                  /* the picture area (closes panels) */
    HIT_FRAME,
    HIT_FPS,
    HIT_SHUTTER,
    HIT_IRIS,
    HIT_TC,
    HIT_ISO,
    HIT_WB,
    HIT_BT,
    HIT_POWER,
    HIT_SETTINGS,
    HIT_RECORD,
    HIT_M5BAT,
    HIT_VIEW,                   /* simple / advanced view badge */
    HIT_ZOOM_OUT,               /* the 2x badge while zoomed */
    HIT_MON_TOGGLE,             /* index, see ui_mon_toggle_t */
    HIT_PRESETS_OPEN,           /* preset button left of the record button */
    HIT_PRESETS_SAVE,           /* index = preset */
    HIT_PRESETS_LOAD,
    HIT_PRESETS_CLEAR,
    HIT_PRESETS_RENAME,
    HIT_PRESETS_SD_SAVE,        /* all presets to the microSD card */
    HIT_PRESETS_SD_LOAD,        /* all presets from it */
    HIT_KB_KEY,                 /* index = character, or UI_KB_* */
    HIT_SLATE_OPEN,             /* slate left of the preset button: tap = quick slate, hold = slate page */
    HIT_SLATE_QUICK,            /* index 0/1 scene up/down, 2/3 take up/down, 4 INT, 5 EXT, 6 DAY, 7 NIGHT */
    HIT_SLATE_TAB,              /* index */
    HIT_SLATE_ITEM,             /* index, see ui_slate_item_t */
    HIT_FOCUS,                  /* focus slider, frac = 0 (near) .. 1 (far) */
    HIT_FORMAT,                 /* recording format: opens codec and resolution in the menu */
    HIT_AF,
    HIT_PANEL,                  /* inside a panel, but on no control */
    HIT_PLAY_CLIP,              /* the play button right of the record button */
    HIT_PLAYBACK,               /* index, see ui_pb_t */
    HIT_SLIDER,                 /* frac = 0..1 */
    HIT_DEC,                    /* index 0 = first value, 1 = second value (tint) */
    HIT_INC,
    HIT_TOGGLE,
    HIT_PRESET,                 /* index */
    HIT_TOOL,                   /* index */
    HIT_PIN_KEY,                /* index 0..9 digits, 10 delete, 11 OK */
    HIT_PIN_CANCEL,
    HIT_MENU_TAB,               /* index */
    HIT_MENU_ITEM,              /* index, see ui_menu_item_t */
    HIT_PAGE_BACK,              /* the bar at the bottom of the menu, presets and slate pages */
    HIT_PAGE_PREV,              /* the page's pages, see ui_page_count */
    HIT_PAGE_NEXT,
} ui_hit_kind_t;

/* Controls of the settings menu (HIT_MENU_ITEM index) */
typedef enum {
    MI_CODEC_BRAW = 0,
    MI_CODEC_PRORES,
    MI_BRAW_BITRATE,            /* Blackmagic RAW constant bitrate */
    MI_BRAW_QUALITY,            /* Blackmagic RAW constant quality */
    MI_CODEC_VARIANT,           /* + index into UI_CODECS */
    MI_RESOLUTION = 20,         /* + resolution index */
    MI_DYN_RANGE = 40,          /* + mode */
    MI_FPS_DEC = 50,
    MI_FPS_INC,
    MI_OFFSPEED_TOGGLE,
    MI_OFFSPEED_DEC,
    MI_OFFSPEED_INC,
    MI_SHARPEN = 60,            /* + level */
    MI_TC_SOURCE = 70,          /* + source */
    MI_FORGET = 80,
    MI_LIVE_VIEW = 90,          /* + 0 HDMI capture on USB-A, + 1 off */
} ui_menu_item_t;

/* Controls of the slate page (HIT_SLATE_ITEM index) */
typedef enum {
    SL_SCENE = 0,
    SL_TAKE_DEC,
    SL_TAKE_INC,
    SL_GOOD_TAKE,
    SL_TAKE_TAG = 10,           /* + tag + 1 (none, PU, VFX, SER) */
    SL_SHOT = 20,               /* + index into UI_SLATE_SHOTS */
    SL_EXTERIOR = 30,
    SL_INTERIOR,
    SL_DAY,
    SL_NIGHT,
    SL_PROJECT = 40,
    SL_DIRECTOR,
    SL_CAMERA_DEC,
    SL_CAMERA_INC,
    SL_OPERATOR,
    SL_LENS_TYPE = 50,
    SL_IRIS,
    SL_FOCAL_LENGTH,
    SL_DISTANCE,
    SL_FILTER,
} ui_slate_item_t;

/* On/off buttons under the status bar (advanced view) */
typedef enum {
    MON_CLEAN_FEED,
    MON_ZEBRA,
    MON_FOCUS_ASSIST,
    MON_GUIDE,
    MON_GRID,
    MON_SAFE_AREA,
    MON_FALSE_COLOR,
    MON_COUNT,
} ui_mon_toggle_t;

typedef struct {
    ui_hit_kind_t kind;
    int index;
    float frac;
} ui_hit_t;

/* What a touch at (x, y) is on, on the screen as drawn from m: the kind, and index or frac where it has them */
ui_hit_t ui_hit_test(const ui_model_t *m, int x, int y);

/* Position 0..1 on the open panel's slider for a touch at x (for drags) */
float ui_slider_frac_at(const ui_model_t *m, int x);

/* An on/off button under the status bar can be used (clean feed greys out the others) */
bool ui_mon_enabled(const ui_model_t *m, int i);

/* Index of the value in list nearest to v */
int ui_nearest_index(const int32_t *list, int n, int32_t v);

/* Recording off-speed: the sensor at its own rate, not the project's */
bool ui_off_speed(const ui_model_t *m);

/* Position 0..1 on the focus slider for a touch at x */
float ui_focus_frac_at(const ui_model_t *m, int x);

/* Pages of the page shown: the settings menu tab's, or the presets'; 1 for the others */
int ui_page_count(const ui_model_t *m);
