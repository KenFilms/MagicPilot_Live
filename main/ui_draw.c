/*
 * Camera UI drawing and layout. See ui_draw.h.
 *
 * View: 1280 x 720 landscape. The status bar (0..TOP_H) and the bottom bar (BOT_Y..720) sit right
 * on the picture, like on the camera's own screen; the status bar keeps no background even while a
 * parameter panel is open. A panel replaces the bottom bar: an optional row of quick values
 * (PRE_Y..ROW_Y) and the control row (ROW_Y..720) with slider, value stepper and toggle, on 25 % and
 * 50 % black. Full screen pages: PIN entry, the settings menu, the camera presets, the slate and the
 * keyboard for preset names and slate texts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "overlay.h"
#include "gfx.h"
#include "fonts.h"
#include "ui_draw.h"

#define W               OV_W
#define H               OV_H
#define TOP_H           72
#define STATUS_Y        TOP_H
#define STATUS_H        64
/* The left column: the name in a box in the corner, then the frame icon, the LUT badge and the 2x badge */
#define FRAME_Y         (STATUS_Y + 20)             /* centre of the frame icon */
#define LUT_Y           (STATUS_Y + 42)             /* top of the LUT badge */
#define ZOOM_Y          (STATUS_Y + 66)             /* top of the 2x badge while zoomed */
#define ZOOM_W          96                          /* the 2x badge's size */
#define ZOOM_H          56

/*
 * Controls under the status bar: camera-like over the live picture, or bigger and easier to press
 * without it (remote only), with the focus slider on a row of its own. s scales the controls, and
 * their fonts go one size up; the live layout is s = 1.
 */
typedef struct {
    float s;
    const gfx_font_t *f13, *f15, *f20, *f26, *f32;  /* the live layout's fonts at this size */
    int mon_x, mon_x1, mon_y, mon_w, mon_h, mon_gap;    /* on/off buttons under the status bar: mon_w
                                                           wide, or (mon_w 0) fitted to their labels up to mon_x1 */
    int toast_y, status_h;                          /* message under those buttons; band with both */
    int clear_y;                                    /* top of everything at the bottom */
    int bot_y, label_y;                             /* bottom bar, baseline of the labels above it */
    int slate_bottom;
    int fmt_x, fmt_y, fmt_h;                        /* recording format: box, its fonts and baselines */
    const gfx_font_t *fmt_small, *fmt_big;
    int fmt_y1, fmt_y2, fmt_y3;
    int preset_x, preset_y;
    int rec_cx, rec_cy, slot_x, slot_dy, slot_w;    /* card slots: a row each, slot_dy apart */
    int focus_x0, focus_x1, focus_y, focus_h, focus_label_y, focus_top, focus_bottom;
    int af_x, af_y, af_w, af_h;
    int play_x, play_y, play_w, play_h;             /* play clip, right of the record button */
    int pre_y, row_y, row_cy;                       /* panels: presets row, value row and its centre */
} layout_t;

static const layout_t LAYOUT_LIVE = {
    1.0f, &font_13b, &font_15b, &font_20s, &font_26s, &font_32s,
    100, 1084, STATUS_Y + 10, 0, 48, 6,
    STATUS_Y + STATUS_H + 6, STATUS_H + 72,
    580,
    612, 642,
    710,
    210, 650, 56, &font_13b, &font_20s, 15, 34, 49,
    344, 650,
    559, 667, 694, 28, 167,
    877, 1160, 652, 34, 642, 602, 720,
    1176, 634, 88, 66,
    614, 634, 66, 66,
    580, 644, 684,
};

static const layout_t LAYOUT_REMOTE = {
    1.3f, &font_15b, &font_20s, &font_26s, &font_32s, &font_46s,
    22, 0, 150, 168, 70, 10,
    232, 204,
    396,
    584, 600,
    712,
    312, 610, 88, &font_20s, &font_32s, 24, 58, 80,
    539, 610,
    835, 646, 1050, 36, 214,
    16, 1076, 432, 60, 420, 400, 540,
    1096, 410, 168, 112,
    907, 603, 86, 86,
    440, 560, 640,
};

/* The simple view over the picture: no slate or recording format, the bottom bar as big as without the
   picture and spread over the whole width, the focus slider in it. Panels keep the live layout */
static const layout_t LAYOUT_LIVE_SIMPLE = {
    1.3f, &font_15b, &font_20s, &font_26s, &font_32s, &font_46s,
    100, 1084, STATUS_Y + 10, 0, 48, 6,
    STATUS_Y + STATUS_H + 6, STATUS_H + 72,
    580,
    584, 600,
    710,
    210, 650, 56, &font_13b, &font_20s, 15, 34, 49,
    16, 610,
    284, 646, 458, 36, 214,
    712, 1114, 620, 44, 600, 590, 720,
    1154, 610, 110, 86,
    356, 603, 86, 86,
    580, 644, 684,
};

/* The simple view without live view: no slate or recording format, the record button in the middle */
static const layout_t LAYOUT_REMOTE_SIMPLE = {
    1.3f, &font_15b, &font_20s, &font_26s, &font_32s, &font_46s,
    22, 0, 150, 168, 70, 10,
    232, 204,
    396,
    584, 600,
    712,
    280, 610, 88, &font_20s, &font_32s, 24, 58, 80,
    362, 610,
    640, 646, 838, 36, 214,
    16, 1076, 432, 60, 420, 400, 540,
    1096, 410, 168, 112,
    712, 603, 86, 86,
    440, 560, 640,
};

static const layout_t *L = &LAYOUT_LIVE;

/* Every drawing and hit test entry picks the layout: bigger controls without the live picture */
static void use_layout(const ui_model_t *m)
{
    if (m->live) {
        L = m->set.simple_view && m->popup == POP_NONE ? &LAYOUT_LIVE_SIMPLE : &LAYOUT_LIVE;
    } else {
        L = m->set.simple_view ? &LAYOUT_REMOTE_SIMPLE : &LAYOUT_REMOTE;
    }
}

#define S(v)            ((int)lroundf((v) * L->s))
#define BOT_Y           (L->bot_y)
#define PRE_Y           (L->pre_y)
#define ROW_Y           (L->row_y)
#define ROW_CY          (L->row_cy)

#define SHADE_A         128         /* 50 % black: the compositor's fast path */
#define PRESET_A        64          /* 25 % black: also a fast path */

#define C_WHITE         0xFFFFFF
#define C_LABEL         0xD3D7DC
#define C_DIM           0x8A8F96
#define C_BLUE          0x0A86DE
#define C_BT_BLUE       0x2E9BFF
#define C_RED           0xEC3520
#define C_NAVY          0x152238
#define C_PAGE_BG       0x1C2942
#define C_CELL          0x33446A
#define C_CYAN          0x2AAEE6
/* Presets page, in MagicPilot_Remote's colours */
#define C_BAR           0x101010
#define C_GREY          0x848484
#define C_GREEN         0x00FC00
#define C_CONNECT       0x18518C
#define C_ORANGE        0xFFA600    /* MagicPilot_Remote's selected value cell: what is selected or on */
#define C_ORANGE_DIM    0xA87800    /* the focus slider up to its marker */
#define C_DARK_GREY     0x404040
#define C_VIOLET        0x8A2BE2    /* the view button in the advanced view */

/* Status bar columns (left edge of label and value) */
#define X_FRAME_C       52
#define X_FPS           132
#define X_SHUT          270
#define X_IRIS          404
#define X_TC            506
#define X_ISO           828
#define X_WB            916
#define X_TINT          1016
#define X_BT            1076
#define X_PWR           1096
#define X_SET_C         1218
#define LABEL_BASE      26
#define VALUE_BASE      58

/* ---- Value lists ---- */

const int32_t UI_ISO_STEPS[] = {100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500,
                                3200, 4000, 5000, 6400, 8000, 10000, 12800, 16000, 20000, 25600};
const int UI_ISO_STEP_COUNT = sizeof(UI_ISO_STEPS) / sizeof(UI_ISO_STEPS[0]);
const int32_t UI_ISO_PRESETS[] = {200, 400, 1250, 3200, 8000};
const int UI_ISO_PRESET_COUNT = sizeof(UI_ISO_PRESETS) / sizeof(UI_ISO_PRESETS[0]);
/* Degrees x 100 */
const int32_t UI_SHUTTER_STEPS[] = {1120, 1500, 2250, 3000, 3750, 4500, 6000, 7200, 7500, 9000, 10800, 12000, 14400,
                                    15000, 17280, 18000, 21600, 27000, 28800, 32400, 34560, 36000};
const int UI_SHUTTER_STEP_COUNT = sizeof(UI_SHUTTER_STEPS) / sizeof(UI_SHUTTER_STEPS[0]);
/* 23, 29, 59 are 23.98, 29.97, 59.94 on the camera */
const int16_t UI_PROJECT_FPS[] = {23, 24, 25, 29, 30, 50, 59, 60};
const int UI_PROJECT_FPS_COUNT = sizeof(UI_PROJECT_FPS) / sizeof(UI_PROJECT_FPS[0]);

int16_t ui_project_fps(const cam_state_t *c)
{
    const bool m = c->format_flags & CAM_FMT_FILE_MRATE;
    return m && (c->file_fps == 24 || c->file_fps == 30 || c->file_fps == 60) ? c->file_fps - 1 : c->file_fps;
}
const int16_t UI_OFFSPEED_PRESETS[] = {24, 25, 30, 33, 48, 50, 60, 100, 120};
const int UI_OFFSPEED_PRESET_COUNT = sizeof(UI_OFFSPEED_PRESETS) / sizeof(UI_OFFSPEED_PRESETS[0]);
/* As on the camera: its presets with their tints, then custom and auto white balance */
const ui_wb_preset_t UI_WB_PRESETS[] = {
    {"Sunlight", 5600, 10}, {"Tungsten", 3200, 0}, {"Fluorescent", 4000, 15}, {"Shade", 4500, 15},
    {"Cloud", 6500, 10}, {"CWB", -1, 0}, {"AWB", 0, 0},
};
const int UI_WB_PRESET_COUNT = sizeof(UI_WB_PRESETS) / sizeof(UI_WB_PRESETS[0]);

int ui_wb_preset_index(int16_t kelvin, int16_t tint)
{
    for (int i = 0; i < UI_WB_PRESET_COUNT; i++) {
        if (UI_WB_PRESETS[i].kelvin > 0 && UI_WB_PRESETS[i].kelvin == kelvin && UI_WB_PRESETS[i].tint == tint) {
            return i;
        }
    }
    return -1;
}
const ui_guide_t UI_GUIDE_RATIOS[] = {
    {"2.40:1", 1}, {"2.39:1", 2}, {"2.35:1", 3}, {"2:1", 8}, {"1.85:1", 4},
    {"16:9", 5}, {"14:9", 6}, {"4:3", 7}, {"1:1", 10}, {"4:5", 9},
};
const int UI_GUIDE_COUNT = sizeof(UI_GUIDE_RATIOS) / sizeof(UI_GUIDE_RATIOS[0]);

int ui_guide_index(int8_t style)
{
    for (int i = 0; i < UI_GUIDE_COUNT; i++) {
        if (UI_GUIDE_RATIOS[i].style == style) {
            return i;
        }
    }
    return -1;
}
/* As on the camera's RECORD page: Blackmagic RAW without Ultra HD and HD, ProRes sensor windowed */
#define RES_BRAW        (1 << CAM_CODEC_BRAW)
#define RES_PRORES      (1 << CAM_CODEC_PRORES)
const ui_resolution_t UI_RESOLUTIONS[] = {
    {"6K", "6K", 6144, 3456, false, RES_BRAW, 50},
    {"6K 2.4:1", "6K 2.4:1", 6144, 2560, true, RES_BRAW, 60},
    {"5.7K 17:9", "5.7K 17:9", 5744, 3024, true, RES_BRAW, 60},
    {"4K DCI", "4K DCI", 4096, 2160, true, RES_BRAW | RES_PRORES, 60},
    {"3.7K Anamorphic", "3.7K Anam.", 3728, 3104, true, RES_BRAW, 60},
    {"2.8K 17:9", "2.8K 17:9", 2880, 1512, true, RES_BRAW, 120},
    /* Scaled from the whole sensor, which runs at most 50 fps (HD measured in HQ and Proxy: 50 taken, 60 not) */
    {"Ultra HD", "Ultra HD", 3840, 2160, false, RES_PRORES, 50},
    {"HD", "HD", 1920, 1080, false, RES_PRORES, 50},
};
const int UI_RESOLUTION_COUNT = sizeof(UI_RESOLUTIONS) / sizeof(UI_RESOLUTIONS[0]);

const ui_resolution_t *ui_resolution_find(int16_t w, int16_t h, bool windowed)
{
    for (int i = 0; i < UI_RESOLUTION_COUNT; i++) {
        if (UI_RESOLUTIONS[i].w == w && UI_RESOLUTIONS[i].h == h && UI_RESOLUTIONS[i].windowed == windowed) {
            return &UI_RESOLUTIONS[i];
        }
    }
    return NULL;
}

void ui_fmt_resolution(int16_t w, int16_t h, char *out, size_t size)
{
    out[0] = 0;
    if (w <= 0) {
        return;
    }
    snprintf(out, size, "%dx%d", w, h);
    for (int i = 0; i < UI_RESOLUTION_COUNT; i++) {
        if (UI_RESOLUTIONS[i].w == w && UI_RESOLUTIONS[i].h == h) {
            snprintf(out, size, "%s", UI_RESOLUTIONS[i].short_label);
        }
    }
}

int ui_max_fps(const cam_state_t *c)
{
    /* HD from a window of the sensor instead goes up to 120 fps */
    if (c->width == 1920 && c->height == 1080 && (c->format_flags & CAM_FMT_WINDOWED)) {
        return 120;
    }
    for (int i = 0; i < UI_RESOLUTION_COUNT; i++) {
        if (UI_RESOLUTIONS[i].w == c->width && UI_RESOLUTIONS[i].h == c->height) {
            return UI_RESOLUTIONS[i].max_fps;
        }
    }
    return 60;
}

int ui_project_fps_count(const cam_state_t *c)
{
    int n = 0;
    while (n < UI_PROJECT_FPS_COUNT && UI_PROJECT_FPS[n] <= ui_max_fps(c)) {
        n++;
    }
    return n > 0 ? n : 1;
}

int ui_project_fps_index(const cam_state_t *c)
{
    const int fps = ui_project_fps(c), n = ui_project_fps_count(c);
    int best = 0;
    for (int i = 1; i < n; i++) {
        if (abs(UI_PROJECT_FPS[i] - fps) < abs(UI_PROJECT_FPS[best] - fps)) {
            best = i;
        }
    }
    return best;
}

int ui_offspeed_preset_count(const cam_state_t *c)
{
    int n = 0;
    while (n < UI_OFFSPEED_PRESET_COUNT && UI_OFFSPEED_PRESETS[n] <= ui_max_fps(c)) {
        n++;
    }
    return n;
}
const ui_codec_t UI_CODECS[] = {
    /* Blackmagic RAW: constant bitrate 3:1 … 12:1, constant quality Q0 … Q5 (protocol variants) */
    {"3:1", CAM_CODEC_BRAW, 2, false}, {"5:1", CAM_CODEC_BRAW, 3, false},
    {"8:1", CAM_CODEC_BRAW, 4, false}, {"12:1", CAM_CODEC_BRAW, 5, false},
    {"Q0", CAM_CODEC_BRAW, 0, true}, {"Q1", CAM_CODEC_BRAW, 7, true},
    {"Q3", CAM_CODEC_BRAW, 8, true}, {"Q5", CAM_CODEC_BRAW, 1, true},
    {"HQ", CAM_CODEC_PRORES, 0, false}, {"422", CAM_CODEC_PRORES, 1, false},
    {"LT", CAM_CODEC_PRORES, 2, false}, {"PXY", CAM_CODEC_PRORES, 3, false},
};
const int UI_CODEC_COUNT = sizeof(UI_CODECS) / sizeof(UI_CODECS[0]);

bool ui_braw_quality(int8_t variant)
{
    for (int i = 0; i < UI_CODEC_COUNT; i++) {
        if (UI_CODECS[i].codec == CAM_CODEC_BRAW && UI_CODECS[i].variant == variant) {
            return UI_CODECS[i].quality;
        }
    }
    return false;
}

/* The camera reports Film as Video (1) while its display LUT is on (seen on the BMPCC6K): then it can be either */
static bool range_unsure(const cam_state_t *c)
{
    return c->dynamic_range == 1 && c->lut_enabled && c->lut;
}

bool ui_resolution_available(const ui_resolution_t *r, int8_t codec)
{
    return codec < 0 || codec > 7 || (r->codecs & (1 << codec));
}

bool ui_resolution_takes_fps(const ui_resolution_t *r, const cam_state_t *c)
{
    return !c->have_format || ui_project_fps(c) <= r->max_fps;
}

const int32_t UI_SHUTTER_PRESETS[UI_SHUTTER_PRESET_COUNT] = {9000, 18000, 36000};
/* Auto exposure modes (1.10) a panel offers while auto exposure drives its value, as on the camera */
const int8_t UI_SHUTTER_AE_MODES[UI_AE_MODE_COUNT] = {2, 4, 3};
const int8_t UI_IRIS_AE_MODES[UI_AE_MODE_COUNT] = {1, 3, 4};
static const char *const AE_LABELS[5] = {"", "Iris", "Shutter", "Iris + Shutter", "Shutter + Iris"};
#define AE_X(i)         (200 + (i) * 340)    /* centres of the auto exposure modes */
#define SHUTTER_PRESET_X(i) (440 + (i) * 190)

bool ui_shutter_auto(int8_t ae_mode)
{
    return ae_mode == 2 || ae_mode == 3 || ae_mode == 4;
}

bool ui_iris_auto(int8_t ae_mode)
{
    return ae_mode == 1 || ae_mode == 3 || ae_mode == 4;
}

/* The open panel's auto exposure modes, shown instead of its value while auto exposure drives it */
static const int8_t *panel_ae_modes(const ui_model_t *m)
{
    if (m->popup == POP_SHUTTER && ui_shutter_auto(m->cam.ae_mode)) {
        return UI_SHUTTER_AE_MODES;
    }
    if (m->popup == POP_IRIS && ui_iris_auto(m->cam.ae_mode)) {
        return UI_IRIS_AE_MODES;
    }
    return NULL;
}
const int32_t UI_SHUTTER_SPEEDS[] = {24, 25, 30, 40, 48, 50, 60, 96, 100, 120, 125, 160, 200, 250, 320, 400, 500,
                                     640, 800, 1000, 1250, 1600, 2000};
const int UI_SHUTTER_SPEED_COUNT = sizeof(UI_SHUTTER_SPEEDS) / sizeof(UI_SHUTTER_SPEEDS[0]);

int ui_exposure_fps(const cam_state_t *c)
{
    if (!c->have_format) {
        return 0;
    }
    return (c->format_flags & CAM_FMT_OFF_SPEED) ? c->sensor_fps : c->file_fps;
}

int32_t ui_speed_from_angle(int32_t angle100, int fps)
{
    return angle100 > 0 ? (int32_t)lroundf(36000.0f * fps / angle100) : 0;
}

int32_t ui_angle_from_speed(int32_t speed, int fps)
{
    if (speed <= 0) {
        return 0;
    }
    const int32_t a = (int32_t)lroundf(36000.0f * fps / speed);
    return a < 100 ? 100 : (a > 36000 ? 36000 : a);
}

/* The camera's shutter as it measures it: "180°" or "1/50"; false if not reported */
static bool fmt_cam_shutter(const cam_state_t *c, char *out, size_t size)
{
    if (c->shutter_speed_mode ? c->shutter_speed <= 0 : c->shutter_angle <= 0) {
        return false;
    }
    if (c->shutter_speed_mode) {
        cam_fmt_shutter_speed(out, size, c->shutter_speed);
    } else {
        cam_fmt_shutter(out, size, c->shutter_angle);
    }
    return true;
}

/* ---- Small helpers ---- */

/* The Bluetooth link to the camera is up */
static bool connected(const ui_model_t *m)
{
    return m->cam.link == CAM_LINK_CONNECTED;
}

/* The camera is connected and recording */
static bool recording(const ui_model_t *m)
{
    return connected(m) && m->cam.transport_mode == 2;
}

bool ui_off_speed(const ui_model_t *m)
{
    return m->cam.have_format && (m->cam.format_flags & CAM_FMT_OFF_SPEED);
}

/* "<" or ">" of size k */
static void chevron_k(int cx, int cy, bool right, uint32_t col, float k)
{
    const float d = right ? k : -k;
    gfx_line(cx - 6 * d, cy - 13 * k, cx + 6 * d, cy, 3.2f * k, col, 255);
    gfx_line(cx + 6 * d, cy, cx - 6 * d, cy + 13 * k, 3.2f * k, col, 255);
}

/* "<" or ">" unscaled, for the full-screen pages */
static void chevron(int cx, int cy, bool right, uint32_t col)
{
    chevron_k(cx, cy, right, col, 1);
}

#define TOGGLE_W        S(100)
#define TOGGLE_H        S(34)

/* ON/OFF switch of a panel, at the layout's size: yellow when on, as the menus' selection */
static void toggle_pill(int x, int y, bool on)
{
    const int w = TOGGLE_W, h = TOGGLE_H;
    if (on) {
        gfx_rrect(x, y, w, h, h / 2, C_ORANGE, 255);
        gfx_circle(x + w - h / 2.0f, y + h / 2.0f, S(11), C_WHITE, 255);
        gfx_text_at(L->f20, x + S(36), y + S(25), GFX_CENTER, "ON", 0x000000, 255);
    } else {
        gfx_rrect(x, y, w, h, h / 2, C_BAR, 230);
        gfx_rrect_stroke(x, y, w, h, h / 2, 2, C_GREY, 255);
        gfx_circle(x + h / 2.0f, y + h / 2.0f, S(11), C_LABEL, 255);
        gfx_text_at(L->f20, x + S(64), y + S(25), GFX_CENTER, "OFF", C_WHITE, 255);
    }
}

/* A switch with its label (if any) just above its right end */
static void labelled_toggle(int x, int y, const char *label, bool on)
{
    if (label) {
        gfx_text_at(L->f13, x + TOGGLE_W, y - S(8), GFX_RIGHT, label, C_WHITE, 255);
    }
    toggle_pill(x, y, on);
}

/* f limited to 0..1 */
static float clamp01(float f)
{
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

/* Panel slider from x0 to x1 on the line cy: orange up to the white knob at frac (0..1), grey beyond */
static void slider(int x0, int x1, int cy, float frac)
{
    frac = clamp01(frac);
    const int kx = x0 + (int)lroundf(frac * (x1 - x0));
    gfx_rrect(x0, cy - S(4), x1 - x0, S(8), S(4), C_GREY, 255);
    gfx_rrect(x0, cy - S(4), kx - x0 + S(4), S(8), S(4), C_ORANGE, 255);
    gfx_circle(kx, cy, S(13), C_WHITE, 255);
}

/* "<  value  >" centred on cx */
static void stepper(int cx, int cy, const char *value)
{
    chevron_k(cx - S(105), cy, false, C_WHITE, L->s);
    gfx_text_at(L->f32, cx, cy + S(12), GFX_CENTER, value, C_WHITE, 255);
    chevron_k(cx + S(105), cy, true, C_WHITE, L->s);
}

/* ---- Status bar ---- */

/* "A" after a status bar label, as on the camera: auto exposure drives this value */
static void auto_badge(int x, const char *label, bool sel)
{
    const int bx = x + gfx_text_width(&font_15b, label) + 6;
    gfx_rrect(bx, LABEL_BASE - 12, 14, 14, 3, sel ? C_ORANGE : C_WHITE, 255);
    gfx_text_at(&font_13b, bx + 7, LABEL_BASE - 1, GFX_CENTER, "A", 0x000000, 255);
}

/* Label and value; yellow while its panel is open */
static void top_item(int x, const char *label, const char *value, bool sel)
{
    gfx_text(&font_15b, x, LABEL_BASE, label, sel ? C_ORANGE : C_LABEL, 255);
    gfx_text(&font_26s, x, VALUE_BASE, value, sel ? C_ORANGE : C_WHITE, 255);
}

/* Frame icon of the display tools: a rounded frame with corner marks inside, centred on (cx, cy) */
static void frame_icon(int cx, int cy, uint32_t col)
{
    gfx_rrect_stroke(cx - 19, cy - 14, 38, 28, 5, 2, col, 255);
    const int ix = cx - 11, iy = cy - 7, iw = 22, ih = 14, l = 6;
    gfx_fill(ix, iy, l, 2, col, 255);
    gfx_fill(ix, iy, 2, l, col, 255);
    gfx_fill(ix + iw - l, iy, l, 2, col, 255);
    gfx_fill(ix + iw - 2, iy, 2, l, col, 255);
    gfx_fill(ix, iy + ih - 2, l, 2, col, 255);
    gfx_fill(ix, iy + ih - l, 2, l, col, 255);
    gfx_fill(ix + iw - l, iy + ih - 2, l, 2, col, 255);
    gfx_fill(ix + iw - 2, iy + ih - l, 2, l, col, 255);
}

/* A box over the picture: 50 % black, outlined */
static void shaded_box(int x, int y, int w, int h, int r, float t, uint32_t line)
{
    gfx_rrect(x, y, w, h, r, 0, SHADE_A);
    gfx_rrect_stroke(x, y, w, h, r, t, line, 255);
}

/* The name in the corner, in a box as the menu's cells: MagicPilot, and LIVE spread to its width below */
static void name_box(void)
{
    const char *name = "MagicPilot", *live = "LIVE";
    const int pad = 8, tw = gfx_text_width(&font_17b, name);
    const int x = 6, y = 6, w = tw + 2 * pad, h = TOP_H - 12;
    shaded_box(x, y, w, h, 6, 2, C_GREY);
    const int b1 = y + pad + font_17b.cap_h, b2 = y + h - pad;
    gfx_text(&font_17b, x + pad, b1, name, C_WHITE, 255);
    /* L I V E: the first letter at the left, the last at the right, the others evenly between */
    int lw[4], sum = 0;
    for (int i = 0; i < 4; i++) {
        const char c[2] = {live[i], 0};
        lw[i] = gfx_text_width(&font_23b, c);
        sum += lw[i];
    }
    const float gap = (float)(tw - sum) / 3;
    float lx = x + pad;
    for (int i = 0; i < 4; i++) {
        const char c[2] = {live[i], 0};
        gfx_text(&font_23b, (int)lroundf(lx), b2, c, C_WHITE, 255);
        lx += lw[i] + gap;
    }
}

/* Settings icon: three slider lines with their knobs, centred on (cx, cy) */
static void settings_icon(int cx, int cy)
{
    const int x0 = cx - 20, x1 = cx + 20;
    const int knob[3] = {cx + 10, cx - 10, cx + 6};
    for (int i = 0; i < 3; i++) {
        const int y = cy - 10 + i * 10;
        gfx_fill(x0, y - 1, knob[i] - 6 - x0, 2, C_WHITE, 255);
        gfx_fill(knob[i] + 6, y - 1, x1 - knob[i] - 6, 2, C_WHITE, 255);
        gfx_ring(knob[i], y, 4.5f, 2, C_WHITE, 255);
    }
}

/* Bluetooth symbol, 22 high, centred on (cx, cy) */
static void bt_icon(int cx, int cy, uint32_t col)
{
    const float t = 2.2f, top = cy - 11, bot = cy + 11;
    gfx_line(cx, top, cx, bot, t, col, 255);
    gfx_line(cx, top, cx + 6, top + 6, t, col, 255);
    gfx_line(cx + 6, top + 6, cx - 6, bot - 6, t, col, 255);
    gfx_line(cx, bot, cx + 6, bot - 6, t, col, 255);
    gfx_line(cx + 6, bot - 6, cx - 6, top + 6, t, col, 255);
}

void ui_power_text(const ui_model_t *m, char *out, size_t size)
{
    const cam_state_t *c = &m->cam;
    if (!connected(m) || c->voltage_mv <= 0) {
        snprintf(out, size, "--");
        return;
    }
    /* Bit 1: on outside power, which the camera too shows as AC */
    if ((c->power_flags & 2) && !m->set.power_voltage) {
        snprintf(out, size, "AC");
    } else {
        const int tenths = (c->voltage_mv + 50) / 100;
        snprintf(out, size, "%d.%dV", tenths / 10, tenths % 10);
    }
}

/* Battery badge: a label over its value in a box (camera in the status bar, Tab5 right below it) */
/* Outlined like the menu's cells; col: white, or red for a low battery */
static void badge(int label_base, int y, const char *label, const char *value, uint32_t col)
{
    gfx_text_at(&font_15b, X_PWR + 30, label_base, GFX_CENTER, label, C_LABEL, 255);
    shaded_box(X_PWR, y, 60, 26, 5, 2, col == C_WHITE ? C_GREY : col);
    gfx_text_at(&font_15b, X_PWR + 30, y + 19, GFX_CENTER, value, col, 255);
}

/* The camera's power badge (CAM) in the status bar */
static void power_badge(const ui_model_t *m)
{
    char v[12];
    ui_power_text(m, v, sizeof(v));
    badge(LABEL_BASE, 34, "CAM", v, C_WHITE);
}

/* The Tab5's battery: red when low */
static uint32_t m5_colour(const ui_model_t *m)
{
    return m->m5_pct >= 0 && m->m5_pct < 15 ? C_RED : C_WHITE;
}

/* The Tab5's battery badge (M5 Tab), under the camera's */
static void m5_badge(const ui_model_t *m)
{
    char v[12];
    ui_m5bat_text(m, v, sizeof(v));
    badge(STATUS_Y + 22, STATUS_Y + 30, "M5 Tab", v, m5_colour(m));
}

/* Simple / advanced view, under the settings icon: LITE on green, PRO on violet */
#define X_VIEW          1172
#define VIEW_W          96
#define VIEW_H          34

static void view_badge(const ui_model_t *m)
{
    const bool simple = m->set.simple_view;
    const char *v = simple ? "LITE" : "PRO";
    gfx_text_at(&font_15b, X_VIEW + VIEW_W / 2, STATUS_Y + 22, GFX_CENTER, "VIEW", C_LABEL, 255);
    /* Edged with a thin black line, and a thin white one outside it */
    gfx_rrect(X_VIEW - 2, STATUS_Y + 28, VIEW_W + 4, VIEW_H + 4, 8, C_WHITE, 255);
    gfx_rrect(X_VIEW - 1, STATUS_Y + 29, VIEW_W + 2, VIEW_H + 2, 7, 0x000000, 255);
    gfx_rrect(X_VIEW, STATUS_Y + 30, VIEW_W, VIEW_H, 6, simple ? C_GREEN : C_VIOLET, 255);
    gfx_text_plain(&font_23b, X_VIEW + VIEW_W / 2 - gfx_text_width(&font_23b, v) / 2,
                   STATUS_Y + 30 + (VIEW_H + font_23b.cap_h) / 2, v, simple ? 0x000000 : C_WHITE, 255);
}

void ui_draw_power(const ui_model_t *m)
{
    gfx_clear(X_PWR - 4, 0, 68, TOP_H);
    power_badge(m);
}

/* The value whose panel is open, outlined like a selected menu cell */
static void highlight_at(int x, int y, int w, int h)
{
    shaded_box(x, y, w, h, 6, 3, C_ORANGE);
}

/* The same round a value in the status bar, from x, w wide */
static void highlight(int x, int w)
{
    highlight_at(x, 5, w, TOP_H - 10);
}

/* What the timecode area shows, so that a tick redraws only the digits that changed */
static struct {
    bool valid;
    char text[16];
    uint32_t col;
    bool badge;
} s_tc;

/* Same characters in the same places, apart from digits (digits share one cell width) */
static bool same_layout(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        const bool da = *a >= '0' && *a <= '9', db = *b >= '0' && *b <= '9';
        if (da != db || (!da && *a != *b)) {
            return false;
        }
    }
    return *a == *b;
}

void ui_draw_timecode(const ui_model_t *m)
{
    char tc[16] = "--:--:--:--";
    const bool have = connected(m) && m->cam.have_tc;
    if (have) {
        cam_fmt_timecode(tc, sizeof(tc), m->cam.tc);
    }
    const uint32_t col = recording(m) ? C_RED : C_WHITE;
    const bool badge = have && m->cam.tc_source == 0;
    /* Only digits changed: redraw just those, each cleared and clipped to its cell, at its pen in the full text */
    if (s_tc.valid && col == s_tc.col && badge == s_tc.badge && same_layout(tc, s_tc.text)) {
        int pen16 = X_TC * 16;
        for (int i = 0; tc[i]; i++) {
            const int adv = gfx_mono_advance16(&font_46s, (uint8_t)tc[i]);
            if (tc[i] != s_tc.text[i]) {
                const int cx = pen16 / 16, cw = (pen16 + adv + 15) / 16 - cx;
                const char one[2] = {tc[i], 0};
                gfx_clear(cx, 0, cw, TOP_H);
                gfx_clip(cx, 0, cw, TOP_H);
                gfx_text_mono16(&font_46s, pen16, 54, one, col, 255);
                gfx_clip_reset();
            }
            pen16 += adv;
        }
        strcpy(s_tc.text, tc);
        return;
    }

    const int x0 = X_TC - 8, w = X_ISO - 14 - x0;
    gfx_clear(x0, 0, w, TOP_H);
    const int tw = gfx_text_mono(&font_46s, X_TC, 54, tc, col, 255);
    if (badge) {
        const int bx = X_TC + tw + 10;
        gfx_rrect_stroke(bx, 38, 30, 18, 3, 2, C_WHITE, 220);
        gfx_text_at(&font_13b, bx + 15, 52, GFX_CENTER, "TC", C_WHITE, 255);
    }
    s_tc.valid = true;
    strcpy(s_tc.text, tc);
    s_tc.col = col;
    s_tc.badge = badge;
}

static void value_text(const ui_model_t *m, ui_popup_t popup, int i, char *out, size_t size);

void ui_draw_top(const ui_model_t *m)
{
    gfx_clear(0, 0, W, TOP_H);
    s_tc.valid = false;
    const bool on = connected(m);
    const cam_state_t *c = &m->cam;
    char v[24];

    switch (m->popup) {
    case POP_FPS:     highlight(X_FPS - 14, 136); break;
    case POP_SHUTTER: highlight(X_SHUT - 14, 126); break;
    case POP_IRIS:    highlight(X_IRIS - 14, 100); break;
    case POP_ISO:     highlight(X_ISO - 14, 88); break;
    case POP_WB:      highlight(X_WB - 14, X_BT - 18 - (X_WB - 14)); break;
    default: break;
    }
    const ui_popup_t p = m->popup;
    name_box();

    /* Off-speed as sensor/project rate, e.g. 48/24 (the panel's value_text() has the sensor rate alone) */
    strcpy(v, "--");
    if (on && c->have_format) {
        char p[12];
        cam_fmt_fps(p, sizeof(p), c->file_fps, c->format_flags & CAM_FMT_FILE_MRATE);
        if (ui_off_speed(m)) {
            snprintf(v, sizeof(v), "%d/%s", c->sensor_fps, p);
        } else {
            snprintf(v, sizeof(v), "%s", p);
        }
    }
    top_item(X_FPS, "FPS", v, p == POP_FPS);

    value_text(m, POP_SHUTTER, 0, v, sizeof(v));
    top_item(X_SHUT, "SHUTTER", v, p == POP_SHUTTER);
    if (on && ui_shutter_auto(c->ae_mode)) {
        auto_badge(X_SHUT, "SHUTTER", p == POP_SHUTTER);
    }

    value_text(m, POP_IRIS, 0, v, sizeof(v));
    top_item(X_IRIS, "IRIS", v, p == POP_IRIS);
    if (on && ui_iris_auto(c->ae_mode)) {
        auto_badge(X_IRIS, "IRIS", p == POP_IRIS);
    }

    value_text(m, POP_ISO, 0, v, sizeof(v));
    top_item(X_ISO, "ISO", v, p == POP_ISO);

    value_text(m, POP_WB, 0, v, sizeof(v));
    top_item(X_WB, "WB", v, p == POP_WB);
    value_text(m, POP_WB, 1, v, sizeof(v));
    top_item(X_TINT, "TINT", v, p == POP_WB);

    const uint32_t bt = c->link == CAM_LINK_CONNECTED ? C_BT_BLUE : (c->link == CAM_LINK_DOWN ? C_DIM : C_WHITE);
    bt_icon(X_BT, 36, bt);
    power_badge(m);
    settings_icon(X_SET_C, 36);
    ui_draw_timecode(m);
}

/* On/off buttons under the status bar (advanced view): the camera's monitoring tools and its guides */
#define MON_X           (L->mon_x)
#define MON_Y           (L->mon_y)
#define MON_W           (L->mon_w)
#define MON_H           (L->mon_h)
#define MON_GAP         (L->mon_gap)
#define MON_FONT        (&font_20s)

static const char *const MON_LABELS[MON_COUNT] = {
    "CLEAN FEED", "ZEBRA", "F.-ASSIST", "GUIDE", "GRID", "SAFE AREA", "FALSE COLOR",
};

/* Left edge and width of a button */
/* Equal widths, or each as wide as its label plus an equal share of the rest of the row */
static void mon_button_pos(int i, int *x, int *w)
{
    if (MON_W) {
        *x = MON_X + i * (MON_W + MON_GAP);
        *w = MON_W;
        return;
    }
    int text = 0;
    for (int k = 0; k < MON_COUNT; k++) {
        text += gfx_text_width(MON_FONT, MON_LABELS[k]);
    }
    const float pad = (float)(L->mon_x1 - MON_X - (MON_COUNT - 1) * MON_GAP - text) / MON_COUNT;
    float fx = MON_X;
    for (int k = 0; k < i; k++) {
        fx += gfx_text_width(MON_FONT, MON_LABELS[k]) + pad + MON_GAP;
    }
    *x = (int)lroundf(fx);
    *w = (int)lroundf(fx + gfx_text_width(MON_FONT, MON_LABELS[i]) + pad) - *x;
}

/* The tool or overlay of on/off button i (MON_*) is on at the camera; false while not connected */
static bool mon_state(const ui_model_t *m, int i)
{
    const cam_state_t *c = &m->cam;
    const bool on = connected(m);
    switch (i) {
    case MON_CLEAN_FEED:   return on && (c->overlay_bits & CAM_OVL_CLEAN_FEED);
    case MON_ZEBRA:        return on && (c->tools & CAM_TOOL_ZEBRA);
    case MON_FOCUS_ASSIST: return on && (c->tools & CAM_TOOL_FOCUS_ASSIST);
    case MON_FALSE_COLOR:  return on && (c->tools & CAM_TOOL_FALSE_COLOR);
    case MON_GUIDE:        return on && (c->overlay_bits & CAM_OVL_GUIDES);
    /* Set on (the camera shows them where its menu switches them on for an output, not reported) */
    case MON_GRID:         return on && c->have_overlays && c->grid;
    case MON_SAFE_AREA:    return on && c->have_overlays && c->safe_area;
    default:               return false;
    }
}

bool ui_mon_enabled(const ui_model_t *m, int i)
{
    /* Clean feed greys out the others */
    return i == MON_CLEAN_FEED || !mon_state(m, MON_CLEAN_FEED);
}

/* The on/off buttons under the status bar; the others greyed out while clean feed is on */
static void draw_mon_buttons(const ui_model_t *m)
{
    for (int i = 0; i < MON_COUNT; i++) {
        int x, w;
        mon_button_pos(i, &x, &w);
        const bool enabled = ui_mon_enabled(m, i), lit = enabled && mon_state(m, i);
        /* On: outlined in yellow, as a selected menu cell */
        shaded_box(x, MON_Y, w, MON_H, 6, lit ? 3 : 1, lit ? C_ORANGE : enabled ? C_GREY : 0x4A4A4A);
        gfx_text_at(MON_FONT, x + w / 2, MON_Y + MON_H / 2 + MON_FONT->cap_h / 2, GFX_CENTER, MON_LABELS[i],
                    lit ? C_ORANGE : enabled ? C_WHITE : C_DIM, 255);
    }
}

/*
 * Without live view the quick slate reaches over the focus row and the band under the status bar:
 * back on top after redrawing those (a full redraw draws it last anyway)
 */
static bool s_drawing_all;
static void draw_slate_quick(const ui_model_t *m);
static void restore_quick(const ui_model_t *m)
{
    if (!s_drawing_all && !m->live && m->slate_quick && m->popup == POP_NONE && m->page == PAGE_NONE) {
        draw_slate_quick(m);
    }
}

/* Messages, the LUT badge and (advanced view) the on/off buttons in the band under the status bar */
void ui_draw_status(const ui_model_t *m)
{
    /* With the strip under the band, where messages go in the advanced view */
    use_layout(m);
    gfx_clear(0, STATUS_Y, W, L->status_h);
    /* The frame icon under the name box: display tools */
    if (m->popup == POP_TOOLS) {
        highlight_at(X_FRAME_C - 30, FRAME_Y - 19, 60, 38);
    }
    frame_icon(X_FRAME_C, FRAME_Y, m->popup == POP_TOOLS ? C_ORANGE : C_WHITE);
    if (connected(m) && m->cam.lut_enabled && m->cam.lut) {
        gfx_rrect_stroke(X_FRAME_C - 17, LUT_Y, 34, 18, 3, 2, C_WHITE, 230);
        gfx_text_at(&font_13b, X_FRAME_C, LUT_Y + 14, GFX_CENTER, "LUT", C_WHITE, 255);
    }
    const bool buttons = !m->set.simple_view;
    if (buttons) {
        draw_mon_buttons(m);
    }
    const char *msg = m->toast[0] ? m->toast : (connected(m) ? "" : m->cam.status);
    if (msg[0]) {
        /* In the band, or under it when the buttons are there */
        const int y = buttons ? L->toast_y : STATUS_Y + 12;
        const int tw = gfx_text_width(&font_20s, msg) + 40;
        gfx_rrect(W / 2 - tw / 2, y, tw, 36, 18, 0, 170);
        gfx_text_at(&font_20s, W / 2, y + 25, GFX_CENTER, msg, C_WHITE, 255);
    }
    m5_badge(m);
    view_badge(m);
    /* 2x badge while zoomed, under the frame icon; tap to zoom out */
    if (m->zoomed) {
        shaded_box(X_FRAME_C - ZOOM_W / 2, ZOOM_Y, ZOOM_W, ZOOM_H, 12, 4, C_ORANGE);
        gfx_text_at(&font_46s, X_FRAME_C, ZOOM_Y + ZOOM_H / 2 + font_46s.cap_h / 2, GFX_CENTER, "2x", C_ORANGE, 255);
    }
    restore_quick(m);
}

void ui_draw_m5bat(const ui_model_t *m)
{
    if (m->page != PAGE_NONE) {
        return;
    }
    gfx_clear(X_PWR - 10, STATUS_Y, 80, STATUS_H);
    m5_badge(m);
}

/* ---- Bottom bar ---- */

#define REC_CX      (L->rec_cx)
#define REC_CY      (L->rec_cy)
#define RING_T      (3.5f * L->s)   /* the record button's white ring, also round the play button */
#define C_RING      0xD8D8D8
#define SLOT_X      (L->slot_x)
#define SLOT_DY     (L->slot_dy)
#define SLOT_W      (L->slot_w)
#define FMT_X       (L->fmt_x)      /* recording format, right of the slate */
#define FMT_Y       (L->fmt_y)      /* under its title, level with the preset button */
#define FMT_W       (gfx_text_width(L->f15, FMT_TITLE))   /* as wide as its title */
#define FMT_TITLE   "CODEC/QUALITY"
#define FMT_H       (L->fmt_h)
#define SLATE_X     16              /* slate: clapperboard with reel, scene and take */
#define SLATE_W     S(180)
#define PRESET_X    (L->preset_x)   /* preset label and button, between the film icon and the record button */
#define PRESET_Y    (L->preset_y)
#define PRESET_W    S(150)
#define PRESET_H    S(56)
#define FOCUS_X0    (L->focus_x0)   /* focus slider: right of the card slots, or its own row */
#define FOCUS_X1    (L->focus_x1)
#define FOCUS_Y     (L->focus_y)
#define FOCUS_H     (L->focus_h)
#define AF_X        (L->af_x)
#define AF_Y        (L->af_y)
#define AF_W        (L->af_w)
#define AF_H        (L->af_h)
#define PLAY_X      (L->play_x)
#define PLAY_Y      (L->play_y)
#define PLAY_W      (L->play_w)
#define PLAY_H      (L->play_h)

/* Filled triangle w wide and h high, centred on (cx, cy), its tip to one side */
enum { TIP_RIGHT, TIP_LEFT, TIP_UP, TIP_DOWN };
static void tri(float cx, float cy, float w, float h, int tip, uint32_t col)
{
    const float l = cx - w / 2, r = cx + w / 2, t = cy - h / 2, b = cy + h / 2;
    const float xs[4][3] = {{l, l, r}, {r, r, l}, {l, r, cx}, {l, r, cx}};
    const float ys[4][3] = {{t, b, cy}, {t, b, cy}, {b, b, t}, {t, t, b}};
    gfx_poly(xs[tip], ys[tip], 3, col, 255);
}

/* Transport icons, h high, centred on cx, cy */
static void pb_icon(int which, float cx, float cy, float h, bool playing, uint32_t col)
{
    switch (which) {
    case PB_PLAY:
        if (playing) {
            gfx_fill((int)(cx - h * 0.35f), (int)(cy - h / 2), (int)(h * 0.25f), (int)h, col, 255);
            gfx_fill((int)(cx + h * 0.1f), (int)(cy - h / 2), (int)(h * 0.25f), (int)h, col, 255);
        } else {
            tri(cx + h * 0.05f, cy, h * 0.85f, h, TIP_RIGHT, col);
        }
        break;
    case PB_FF:
    case PB_REW: {
        const int tip = which == PB_FF ? TIP_RIGHT : TIP_LEFT;
        tri(cx - h * 0.3f, cy, h * 0.6f, h * 0.8f, tip, col);
        tri(cx + h * 0.3f, cy, h * 0.6f, h * 0.8f, tip, col);
        break;
    }
    case PB_NEXT:
    case PB_PREV: {
        const bool right = which == PB_NEXT;
        tri(cx + (right ? -h * 0.12f : h * 0.12f), cy, h * 0.7f, h, right ? TIP_RIGHT : TIP_LEFT, col);
        gfx_fill((int)(right ? cx + h * 0.28f : cx - h * 0.43f), (int)(cy - h / 2), (int)(h * 0.15f), (int)h, col, 255);
        break;
    }
    default:
        break;
    }
}

/* Play clip, right of the record button: a big triangle in a box starts the camera's playback */
static void draw_play_button(const ui_model_t *m)
{
    const bool on = connected(m);
    shaded_box(PLAY_X, PLAY_Y, PLAY_W, PLAY_H, S(8), RING_T, C_RING);
    pb_icon(PB_PLAY, PLAY_X + PLAY_W / 2.0f, PLAY_Y + PLAY_H / 2.0f, PLAY_H * 0.46f, false, on ? C_WHITE : C_DIM);
}

/* Recording time left as hh:mm:ss, two digits each, also under an hour */
static void fmt_remaining(char *out, size_t size, int32_t s)
{
    snprintf(out, size, "%02ld:%02ld:%02ld", (long)(s / 3600), (long)(s / 60 % 60), (long)(s % 60));
}


/* The camera records to this slot, as its transport flags (10.1) say; they have none for the SSD (slot 2) */
static bool slot_active(const ui_model_t *m, int slot)
{
    return connected(m) && slot < 2 && (m->cam.transport_flags & (1 << (5 + slot)));
}

/* Media present: the camera reports recording time for it, or records to it */
static bool slot_present(const ui_model_t *m, int slot)
{
    return connected(m) && (m->cam.slot_remaining[slot] > 0 || slot_active(m, slot));
}

/* Media as the camera reports them (10.1): slot 1 CFast, slot 2 SD, slot 3 the SSD on USB-C */
enum { MEDIUM_CFAST, MEDIUM_SD, MEDIUM_SSD };
static const char *const MEDIUM_LABELS[3] = {"CFast", "SD", "SSD"};

/* Medium in a slot (MEDIUM_*): as reported for slots 0 and 1, else the slot's usual one; slot 2 is the SSD */
static int medium_kind(const ui_model_t *m, int slot)
{
    const int k = slot < 2 ? m->cam.slot_medium[slot] : MEDIUM_SSD;
    return k >= MEDIUM_CFAST && k <= MEDIUM_SSD ? k : slot;
}

/* Width of a medium's card: its name plus room for the connector, the cut corner or the screws */
static int card_w(int kind)
{
    return gfx_text_width(L->f15, MEDIUM_LABELS[kind]) + (kind == MEDIUM_SD ? S(16) : kind == MEDIUM_SSD ? S(22) : S(18));
}

/* The medium as a small card with its name: CFast with its connector slot along one edge, SD with its
   cut corner, the SSD as a 2.5" drive with screws in its corners. fill under the outline line; ink for
   the name and details */
static void card_icon(int kind, int x, int cy, uint32_t fill, uint8_t fill_a, uint32_t line, uint32_t ink)
{
    const int h = S(24), y = cy - h / 2, w = card_w(kind);
    int tx = x + w / 2;
    if (kind == MEDIUM_SD) {
        const float c = S(7), x1 = x + w, y1 = y + h;
        const float xs[5] = {x, x1 - c, x1, x1, x};
        const float ys[5] = {y, y, y + c, y1, y1};
        gfx_poly(xs, ys, 5, fill, fill_a);
        for (int i = 0; i < 5; i++) {
            gfx_line(xs[i], ys[i], xs[(i + 1) % 5], ys[(i + 1) % 5], 2.0f, line, 255);
        }
    } else if (kind == MEDIUM_CFAST) {
        gfx_rrect(x, y, w, h, S(3), fill, fill_a);
        gfx_rrect_stroke(x, y, w, h, S(3), 2, line, 255);
        gfx_rrect(x + S(4), y + S(5), S(4), h - S(10), S(1), ink, 255);
        tx = x + S(8) + (w - S(8)) / 2;
    } else {
        gfx_rrect(x, y, w, h, S(3), fill, fill_a);
        gfx_rrect_stroke(x, y, w, h, S(3), 2, line, 255);
        const int in = S(5);
        for (int k = 0; k < 4; k++) {
            gfx_circle(k & 1 ? x + w - in : x + in, k & 2 ? y + h - in : y + in, 1.6f * L->s, ink, 255);
        }
    }
    gfx_text_at(L->f15, tx, cy + L->f15->cap_h / 2, GFX_CENTER, MEDIUM_LABELS[kind], ink, 255);
}

/* A row per slot, level with the record button: the medium, then the recording time left. The card the
   camera records to is yellow with a green dot beside it, as on MagicPilot_Remote; both red while recording */
static void draw_slots(const ui_model_t *m)
{
    const int col = card_w(MEDIUM_CFAST);   /* the widest card; the others centred in its column */
    const int x0 = SLOT_X + S(14);          /* after the dot */
    for (int i = 0; i < 3; i++) {
        const int cy = REC_CY + (i - 1) * SLOT_DY, kind = medium_kind(m, i);
        const int x = x0 + (col - card_w(kind)) / 2;
        const bool present = slot_present(m, i);
        if (slot_active(m, i)) {
            const uint32_t c = recording(m) ? C_RED : C_ORANGE;
            gfx_circle(x - S(8), cy, S(4), recording(m) ? C_RED : C_GREEN, 255);
            card_icon(kind, x, cy, c, 255, c, recording(m) ? C_WHITE : 0x000000);
        } else {
            card_icon(kind, x, cy, 0, SHADE_A, present ? C_WHITE : C_GREY, present ? C_WHITE : C_DIM);
        }
        char text[16];
        uint32_t col_text = C_DIM;
        if (!connected(m) || m->cam.slot_remaining[i] < 0) {
            strcpy(text, "--");
        } else if (!present) {
            strcpy(text, i == 2 ? "No Drive" : "No Card");
        } else {
            fmt_remaining(text, sizeof(text), m->cam.slot_remaining[i]);
            col_text = C_WHITE;
        }
        gfx_text(L->f20, x0 + col + S(8), cy + L->f20->cap_h / 2, text, col_text, 255);
    }
}

/* Record button: a red dot in a ring, the ring red and the dot bigger while recording */
static void draw_record(const ui_model_t *m)
{
    const bool rec = recording(m);
    gfx_circle(REC_CX, REC_CY, S(32), C_NAVY, 235);
    gfx_ring(REC_CX, REC_CY, S(33), RING_T, rec ? C_RED : C_RING, 255);
    gfx_circle(REC_CX, REC_CY, rec ? S(17) : S(15), C_RED, 255);
}

void ui_m5bat_text(const ui_model_t *m, char *out, size_t size)
{
    if (m->m5_external) {
        snprintf(out, size, "USB-C");
    } else if (m->m5_mv <= 0) {
        snprintf(out, size, "--");
    } else if (m->set.m5bat_voltage) {
        snprintf(out, size, "%d.%02dV", m->m5_mv / 1000, (m->m5_mv % 1000) / 10);
    } else {
        snprintf(out, size, "%d%%", m->m5_pct);
    }
}

void ui_draw_record(const ui_model_t *m)
{
    if (m->popup != POP_NONE || m->page != PAGE_NONE) {
        return;
    }
    use_layout(m);
    gfx_clear(REC_CX - S(40), BOT_Y, S(80), H - BOT_Y);
    draw_record(m);
    restore_quick(m);
}

void ui_draw_slots(const ui_model_t *m)
{
    if (m->popup != POP_NONE || m->page != PAGE_NONE) {
        return;
    }
    use_layout(m);
    gfx_clear(SLOT_X - 8, BOT_Y, SLOT_W + 16, H - BOT_Y);
    draw_slots(m);
    restore_quick(m);
}

void ui_preset_name(const ui_model_t *m, int i, char *out, size_t size)
{
    if (m->presets[i].name[0]) {
        snprintf(out, size, "%s", m->presets[i].name);
    } else {
        snprintf(out, size, "Preset %d", i + 1);
    }
}

/* Shortens s (with room for 3 more bytes) with an ellipsis until it is at most max_w wide */
static void fit_text(const gfx_font_t *f, char *s, int max_w)
{
    size_t n = strlen(s);
    while (gfx_text_width(f, s) > max_w) {
        /* Drop the ellipsis and one more character (all bytes of it), then put the ellipsis back */
        if (n >= 3 && strcmp(s + n - 3, "…") == 0) {
            n -= 3;
        }
        if (n == 0) {
            break;
        }
        do {
            n--;
        } while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80);
        strcpy(s + n, "…");
        n += 3;
    }
}

/* Preset button left of the record button: "Preset Menu", or the loaded preset's name */
static void draw_preset_button(const ui_model_t *m)
{
    gfx_text(L->f15, PRESET_X, L->label_y, "PRESET", C_WHITE, 255);
    gfx_rrect(PRESET_X, PRESET_Y, PRESET_W, PRESET_H, S(10), C_NAVY, 255);
    gfx_rrect_stroke(PRESET_X, PRESET_Y, PRESET_W, PRESET_H, S(10), 2, 0xD8D8D8, 255);
    const int cx = PRESET_X + PRESET_W / 2, cy = PRESET_Y + PRESET_H / 2;
    if (m->preset_active < 0) {
        gfx_text_at(L->f20, cx, cy + L->f20->cap_h / 2, GFX_CENTER, "Preset Menu", C_WHITE, 255);
        return;
    }
    char name[UI_PRESET_NAME_MAX + 8];
    ui_preset_name(m, m->preset_active, name, sizeof(name));
    const int max_w = PRESET_W - S(14);
    /* As large as it fits, smaller for longer names, shortened only beyond that */
    const gfx_font_t *f = gfx_text_width(L->f26, name) <= max_w   ? L->f26
                          : gfx_text_width(L->f20, name) <= max_w ? L->f20
                                                                   : L->f15;
    fit_text(f, name, max_w);
    gfx_text_at(f, cx, cy + f->cap_h / 2, GFX_CENTER, name, C_WHITE, 255);
}

void ui_draw_preset_name(const ui_model_t *m)
{
    if (m->popup != POP_NONE || m->page != PAGE_NONE) {
        return;
    }
    use_layout(m);
    gfx_clear(PRESET_X - 8, BOT_Y, PRESET_W + 16, H - BOT_Y);
    draw_preset_button(m);
    restore_quick(m);
}

/* Focus percentage and slider: what changes while focusing */
static void draw_focus_value(const ui_model_t *m)
{
    const bool known = connected(m);
    char t[8] = "--";
    if (known) {
        snprintf(t, sizeof(t), "%d%%", (m->cam.focus * 100 + CAM_FOCUS_MAX / 2) / CAM_FOCUS_MAX);
    }
    gfx_text_at(L->f15, FOCUS_X1, L->focus_label_y, GFX_RIGHT, t, C_WHITE, 255);
    const int w = FOCUS_X1 - FOCUS_X0;
    gfx_set(FOCUS_X0, FOCUS_Y, w, FOCUS_H, 0, SHADE_A);
    gfx_rrect_stroke(FOCUS_X0, FOCUS_Y, w, FOCUS_H, 2, 2, C_GREY, 255);
    if (known) {
        /* The white marker over the grey frame */
        const int pos = m->cam.focus * (w - 4) / CAM_FOCUS_MAX;
        if (pos > 0) {
            gfx_fill(FOCUS_X0 + 2, FOCUS_Y + 2, pos, FOCUS_H - 4, C_ORANGE_DIM, 255);
        }
        gfx_fill(FOCUS_X0 + 2 + pos - S(3), FOCUS_Y - S(3), S(6), FOCUS_H + S(6), C_WHITE, 255);
    }
}

/* Focus slider and AF button right of the card slots, like MagicPilot_Remote's focus page */
static void draw_focus(const ui_model_t *m)
{
    gfx_text(L->f15, FOCUS_X0, L->focus_label_y, "FOCUS", C_WHITE, 255);
    draw_focus_value(m);
    gfx_text(L->f13, FOCUS_X0, FOCUS_Y + FOCUS_H + S(17), "NEAR", C_LABEL, 255);
    gfx_text_at(L->f13, FOCUS_X1, FOCUS_Y + FOCUS_H + S(17), GFX_RIGHT, "FAR", C_LABEL, 255);
    shaded_box(AF_X, AF_Y, AF_W, AF_H, S(8), 2, C_GREY);
    gfx_text_at(L->f26, AF_X + AF_W / 2, AF_Y + AF_H / 2 + L->f26->cap_h / 2, GFX_CENTER, "AF", C_WHITE, 255);
}

void ui_draw_focus(const ui_model_t *m)
{
    if (m->popup != POP_NONE || m->page != PAGE_NONE) {
        return;
    }
    /* Just the percentage and the slider (with the knob's overhang): a drag redraws this often */
    use_layout(m);
    gfx_clear(FOCUS_X1 - S(70), L->focus_label_y - S(18), S(78), S(22));
    gfx_clear(FOCUS_X0 - 4, FOCUS_Y - S(4), FOCUS_X1 - FOCUS_X0 + 8, FOCUS_H + S(8));
    draw_focus_value(m);
    restore_quick(m);
}

float ui_focus_frac_at(const ui_model_t *m, int x)
{
    use_layout(m);
    return clamp01((float)(x - FOCUS_X0) / (FOCUS_X1 - FOCUS_X0));
}

/* ---- Slate on the main screen: a clapperboard with the camera's slate, like a real one ---- */

const int8_t UI_SLATE_SHOTS[UI_SLATE_SHOT_COUNT] = {-1, 0, 2, 4, 1, 3, 5};
static const char *const SHOT_LABELS[UI_SLATE_SHOT_COUNT] = {"NONE", "WS", "MS", "MCU", "CU", "BCU", "ECU"};
static const char *const TAKE_TAGS[4] = {"NONE", "PU", "VFX", "SER"};

#define C_SLATE_BLACK   0x121212
#define C_SLATE_EDGE    0x7A7A7A
#define SLATE_BOTTOM    (L->slate_bottom)
#define SLATE_STICK_T   S(10)       /* striped clapper band on the board */
#define SLATE_ROW_H     S(17)
#define SLATE_BIG_H     S(38)       /* reel, scene and take */
/* Highest the slate reaches (director and camera operator both shown) */
#define SLATE_TOP       (SLATE_BOTTOM - slate_board_h(true, true) - SLATE_STICK_T - 1)

/* Clips a convex polygon to lo <= u <= hi; returns the number of corners left */
static int clip_u(const float *u, const float *v, int n, float lo, float hi, float *ou, float *ov)
{
    float tu[GFX_POLY_MAX], tv[GFX_POLY_MAX];
    int m = 0;
    for (int i = 0; i < n && m < GFX_POLY_MAX - 1; i++) {
        const int j = (i + 1) % n;
        const bool in_i = u[i] >= lo, in_j = u[j] >= lo;
        if (in_i) {
            tu[m] = u[i];
            tv[m++] = v[i];
        }
        if (in_i != in_j) {
            tu[m] = lo;
            tv[m++] = v[i] + (lo - u[i]) / (u[j] - u[i]) * (v[j] - v[i]);
        }
    }
    int r = 0;
    for (int i = 0; i < m && r < GFX_POLY_MAX - 1; i++) {
        const int j = (i + 1) % m;
        const bool in_i = tu[i] <= hi, in_j = tu[j] <= hi;
        if (in_i) {
            ou[r] = tu[i];
            ov[r++] = tv[i];
        }
        if (in_i != in_j) {
            ou[r] = hi;
            ov[r++] = tv[i] + (hi - tu[i]) / (tu[j] - tu[i]) * (tv[j] - tv[i]);
        }
    }
    return r;
}

/*
 * A clapper stick with alternating black and white diagonal stripes. (hx, hy) is its lower left
 * corner, the hinge; ang lifts the right end (radians); lean tilts the stripes right (1) or left (-1).
 */
static void clapper_stick(float hx, float hy, float len, float ang, int lean)
{
    const float c = cosf(ang), s = sinf(ang), t = SLATE_STICK_T;
    float xs[GFX_POLY_MAX], ys[GFX_POLY_MAX];
    const float ru[4] = {0, len, len, 0}, rv[4] = {0, 0, t, t};
    for (int i = 0; i < 4; i++) {
        xs[i] = hx + ru[i] * c - rv[i] * s;
        ys[i] = hy - ru[i] * s - rv[i] * c;
    }
    gfx_poly(xs, ys, 4, C_SLATE_BLACK, 255);
    /* White stripes: parallelograms slanted by lean, a period apart, cut off at the stick's ends */
    const float stripe = 11 * L->s, period = 22 * L->s;
    for (float a = -period; a < len + period; a += period) {
        const float pu[4] = {a, a + stripe, a + stripe + lean * t, a + lean * t}, pv[4] = {0, 0, t, t};
        float cu[GFX_POLY_MAX], cv[GFX_POLY_MAX];
        const int n = clip_u(pu, pv, 4, 0, len, cu, cv);
        if (n < 3) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            xs[i] = hx + cu[i] * c - cv[i] * s;
            ys[i] = hy - cu[i] * s - cv[i] * c;
        }
        gfx_poly(xs, ys, n, C_WHITE, 255);
    }
    /* A thin edge, so the black parts stand out on dark pictures */
    for (int i = 0; i < 4; i++) {
        const int j = (i + 1) % 4;
        gfx_line(hx + ru[i] * c - rv[i] * s, hy - ru[i] * s - rv[i] * c, hx + ru[j] * c - rv[j] * s,
                 hy - ru[j] * s - rv[j] * c, 1.2f, C_SLATE_EDGE, 255);
    }
}

/* A label and its value on one row of the slate */
static void slate_row(int y, const char *label, const char *value)
{
    gfx_text(L->f13, SLATE_X + S(6), y, label, C_LABEL, 255);
    char t[CAM_SLATE_TEXT + 4];
    snprintf(t, sizeof(t), "%s", value);
    fit_text(L->f15, t, SLATE_W - S(58));
    gfx_text(L->f15, SLATE_X + S(52), y, t, C_WHITE, 255);
}

/* Chalk line across the board */
static void slate_line(int y)
{
    gfx_fill(SLATE_X + S(4), y, SLATE_W - S(8), 1, C_WHITE, 200);
}

/* Board height: director and camera operator only take a row when the camera has them */
static int slate_board_h(bool dir, bool cam)
{
    return 2 + (2 + dir + cam) * SLATE_ROW_H + SLATE_BIG_H + 2 + (dir || cam) + 2;
}

/* Which of director and camera operator the slate shows: those the camera has a name for */
static void slate_names(const ui_model_t *m, bool *dir, bool *cam)
{
    const bool on = connected(m) && m->slate.have;
    *dir = on && m->slate.text[CAM_SLATE_DIRECTOR][0];
    *cam = on && m->slate.text[CAM_SLATE_OPERATOR][0];
}

/* Top of the slate (its striped band) */
static int slate_top(const ui_model_t *m)
{
    bool dir, cam;
    slate_names(m, &dir, &cam);
    return SLATE_BOTTOM - slate_board_h(dir, cam) - SLATE_STICK_T - 1;
}

/*
 * The slate left of the preset button, laid out like a real one under its striped clapper band:
 * production, reel, scene and take, director and camera operator (when known), then
 * interior/exterior, day/night, shot type and take tag. Tap: quick slate; hold: slate page.
 */
static void draw_slate_board(const ui_model_t *m)
{
    const cam_slate_t *s = &m->slate;
    const bool on = connected(m) && s->have;
    bool dir, cam;
    slate_names(m, &dir, &cam);
    const int x = SLATE_X, w = SLATE_W, h = slate_board_h(dir, cam), y = SLATE_BOTTOM - h;
    /* Board under the clapper band, with a soft shadow */
    gfx_rrect(x + S(3), y - SLATE_STICK_T - 1 + S(3), w, h + SLATE_STICK_T + 1, S(4), 0, 110);
    gfx_rrect(x, y, w, h, S(3), C_SLATE_BLACK, 255);
    gfx_rrect_stroke(x, y, w, h, S(3), 1, C_SLATE_EDGE, 255);
    clapper_stick(x, y - 1, w, 0, 1);

    int ry = y + 2;
    slate_row(ry + S(13), "PROD.", on ? s->text[CAM_SLATE_PROJECT] : "");
    ry += SLATE_ROW_H;
    slate_line(ry++);
    /* Reel, scene and take in big letters */
    const struct { int x0, w; const char *label; } col[3] = {
        {0, S(46), "REEL"}, {S(48), S(86), "SCENE"}, {S(136), S(44), "TAKE"},
    };
    gfx_fill(x + S(46), ry, 2, SLATE_BIG_H, C_WHITE, 200);
    gfx_fill(x + S(134), ry, 2, SLATE_BIG_H, C_WHITE, 200);
    char v[3][CAM_SLATE_TEXT + 4];
    snprintf(v[0], sizeof(v[0]), "%d", s->reel);
    snprintf(v[1], sizeof(v[1]), "%s", s->text[CAM_SLATE_SCENE][0] ? s->text[CAM_SLATE_SCENE] : "–");
    snprintf(v[2], sizeof(v[2]), "%d", s->take);
    for (int i = 0; i < 3; i++) {
        gfx_text(L->f13, x + col[i].x0 + S(6), ry + S(11), col[i].label, C_LABEL, 255);
        if (!on) {
            strcpy(v[i], "–");
        }
        const gfx_font_t *f = gfx_text_width(L->f26, v[i]) <= col[i].w - S(8) ? L->f26 : L->f20;
        fit_text(f, v[i], col[i].w - S(8));
        gfx_text_at(f, x + col[i].x0 + col[i].w / 2, ry + S(35), GFX_CENTER, v[i], C_WHITE, 255);
    }
    ry += SLATE_BIG_H;
    slate_line(ry++);
    if (dir) {
        slate_row(ry + S(13), "DIR.", s->text[CAM_SLATE_DIRECTOR]);
        ry += SLATE_ROW_H;
    }
    if (cam) {
        slate_row(ry + S(13), "CAM.", s->text[CAM_SLATE_OPERATOR]);
        ry += SLATE_ROW_H;
    }
    if (dir || cam) {
        slate_line(ry++);
    }
    /* Interior/exterior, day/night, shot type and take tag */
    if (on) {
        const char *tags[4] = {s->interior ? "INT" : "EXT", s->day ? "DAY" : "NIGHT", "", ""};
        for (int k = 0; k < UI_SLATE_SHOT_COUNT; k++) {
            if (UI_SLATE_SHOTS[k] == s->shot && s->shot >= 0) {
                tags[2] = SHOT_LABELS[k];
            }
        }
        if (s->take_tag >= 0 && s->take_tag <= 2) {
            tags[3] = TAKE_TAGS[s->take_tag + 1];
        }
        int tx = x + S(6);
        for (int i = 0; i < 4; i++) {
            if (tags[i][0]) {
                tx += gfx_text(L->f13, tx, ry + S(13), tags[i], C_WHITE, 255) + S(12);
            }
        }
    }
}

/*
 * Quick slate above the slate: scene and take, each with a triangle above and below to step it,
 * and buttons for interior/exterior and day/night
 */
#define SQ_X        SLATE_X
#define SQ_W        SLATE_W
#define SQ_STEP_H   S(172)      /* scene and take with their triangles */
#define SQ_BTN_H    S(42)
#define SQ_H        (SQ_STEP_H + 2 * (SQ_BTN_H + S(8)) + S(6))
#define SQ_Y        (SLATE_TOP - S(8) - SQ_H)

static void draw_slate_quick(const ui_model_t *m)
{
    const cam_slate_t *s = &m->slate;
    const bool on = connected(m) && s->have;
    if (!m->live) {
        /* On black, as the screen is without a picture: drawn again it stays the same */
        gfx_set(SQ_X, SQ_Y, SQ_W, SQ_H, 0, 255);
    }
    gfx_rrect(SQ_X, SQ_Y, SQ_W, SQ_H, S(8), C_NAVY, 255);
    gfx_rrect_stroke(SQ_X, SQ_Y, SQ_W, SQ_H, S(8), 2, C_WHITE, 255);
    gfx_fill(SQ_X + SQ_W / 2 - 1, SQ_Y + S(14), 2, SQ_STEP_H - S(28), C_GREY, 255);
    static const char *labels[2] = {"SCENE", "TAKE"};
    char v[2][CAM_SLATE_TEXT + 4];
    snprintf(v[0], sizeof(v[0]), "%s", s->text[CAM_SLATE_SCENE][0] ? s->text[CAM_SLATE_SCENE] : "–");
    snprintf(v[1], sizeof(v[1]), "%d", s->take);
    for (int i = 0; i < 2; i++) {
        const int cx = SQ_X + SQ_W / 4 + i * SQ_W / 2, max_w = SQ_W / 2 - S(12);
        gfx_text_at(L->f13, cx, SQ_Y + S(24), GFX_CENTER, labels[i], C_LABEL, 255);
        tri(cx, SQ_Y + S(54), S(34), S(20), TIP_UP, on ? C_WHITE : C_DIM);
        if (!on) {
            strcpy(v[i], "–");
        }
        const gfx_font_t *f = gfx_text_width(L->f32, v[i]) <= max_w ? L->f32 : L->f20;
        fit_text(f, v[i], max_w);
        gfx_text_at(f, cx, SQ_Y + S(106), GFX_CENTER, v[i], C_WHITE, 255);
        tri(cx, SQ_Y + S(150), S(34), S(20), TIP_DOWN, on ? C_WHITE : C_DIM);
    }
    /* Interior/exterior and day/night */
    gfx_fill(SQ_X + S(8), SQ_Y + SQ_STEP_H, SQ_W - S(16), 1, C_GREY, 255);
    static const char *labels2[4] = {"INT", "EXT", "DAY", "NIGHT"};
    const bool sel[4] = {s->interior == 1, s->interior == 0, s->day == 1, s->day == 0};
    const int bw = (SQ_W - S(20)) / 2;
    for (int i = 0; i < 4; i++) {
        const int bx = SQ_X + S(6) + (i % 2) * (bw + S(8)), by = SQ_Y + SQ_STEP_H + S(8) + (i / 2) * (SQ_BTN_H + S(8));
        const bool lit = on && sel[i];
        gfx_rrect(bx, by, bw, SQ_BTN_H, S(6), C_NAVY, 255);
        gfx_rrect_stroke(bx, by, bw, SQ_BTN_H, S(6), lit ? 3 : 2, lit ? C_ORANGE : C_GREY, 255);
        gfx_text_at(L->f20, bx + bw / 2, by + SQ_BTN_H / 2 + S(7), GFX_CENTER, labels2[i],
                    !on ? C_DIM : lit ? C_ORANGE : C_WHITE, 255);
    }
}

void ui_draw_slate_quick(const ui_model_t *m)
{
    if (m->page != PAGE_NONE) {
        return;
    }
    use_layout(m);
    gfx_clear(SQ_X - 4, SQ_Y - 4, SQ_W + 8, SQ_H + 8);
    if (!m->live) {
        /* It lies over the buttons and the focus row: those whole again (with it on top, if open) */
        ui_draw_status(m);
        ui_draw_bottom(m);
        return;
    }
    if (m->slate_quick && m->popup == POP_NONE) {
        draw_slate_quick(m);
    }
}

void ui_draw_slate(const ui_model_t *m)
{
    if (m->popup != POP_NONE || m->page != PAGE_NONE || m->set.simple_view) {
        return;
    }
    /* The slate reaches above the bottom bar */
    use_layout(m);
    gfx_clear(SLATE_X - 4, SLATE_TOP - 4, SLATE_W + 12, H - SLATE_TOP + 4);
    draw_slate_board(m);
    restore_quick(m);
}

const char *ui_codec_name(int codec)
{
    static const char *const names[4] = {"DNG", "DNxHD", "ProRes", "BRAW"};
    return codec >= 0 && codec < 4 ? names[codec] : NULL;
}

/* Codec and quality, e.g. "BRAW 3:1" or "ProRes HQ"; "" if unknown */
static void fmt_codec(int8_t codec, int8_t variant, char *out, size_t size)
{
    const char *name = ui_codec_name(codec);
    out[0] = 0;
    if (!name) {
        return;
    }
    const char *q = "";
    for (int i = 0; i < UI_CODEC_COUNT; i++) {
        if (UI_CODECS[i].codec == codec && UI_CODECS[i].variant == variant) {
            q = UI_CODECS[i].label;
        }
    }
    snprintf(out, size, "%s %s", name, q);
}

/* Resolution, e.g. "6K" or "4K DCI", else the size in pixels; "" if unknown */
static void format_resolution(const cam_state_t *c, char *out, size_t size)
{
    ui_fmt_resolution(c->have_format ? c->width : 0, c->height, out, size);
}

/*
 * Recording format: codec and quality, resolution, dynamic range, white on 50 % black. Opens the
 * menu where they are set.
 */
static void draw_format(const ui_model_t *m)
{
    gfx_text(L->f15, FMT_X, L->label_y, FMT_TITLE, C_WHITE, 255);
    gfx_rrect(FMT_X, FMT_Y, FMT_W, FMT_H, S(6), 0, SHADE_A);
    if (!m->live) {
        /* On the black screen without live view the box needs an edge to read as a button */
        gfx_rrect_stroke(FMT_X, FMT_Y, FMT_W, FMT_H, S(6), 2, C_GREY, 255);
    }
    static const char *const ranges[3] = {"FILM", "VIDEO", "EXT VIDEO"};
    char codec[24] = "", res[24] = "", range[16] = "–";
    if (connected(m)) {
        fmt_codec(m->cam.codec, m->cam.codec_variant, codec, sizeof(codec));
        format_resolution(&m->cam, res, sizeof(res));
        if (range_unsure(&m->cam)) {
            strcpy(range, "FILM/VIDEO");
        } else if (m->cam.dynamic_range >= 0 && m->cam.dynamic_range <= 2) {
            snprintf(range, sizeof(range), "%s", ranges[m->cam.dynamic_range]);
        }
    }
    const int cx = FMT_X + FMT_W / 2, max_w = FMT_W - S(12);
    if (!codec[0]) {
        strcpy(codec, "–");
    }
    if (!res[0]) {
        strcpy(res, "–");
    }
    fit_text(L->fmt_small, codec, max_w);
    gfx_text_at(L->fmt_small, cx, FMT_Y + L->fmt_y1, GFX_CENTER, codec, C_WHITE, 255);
    const gfx_font_t *f = gfx_text_width(L->fmt_big, res) <= max_w ? L->fmt_big : L->f15;
    fit_text(f, res, max_w);
    gfx_text_at(f, cx, FMT_Y + L->fmt_y2, GFX_CENTER, res, C_WHITE, 255);
    fit_text(L->fmt_small, range, max_w);
    gfx_text_at(L->fmt_small, cx, FMT_Y + L->fmt_y3, GFX_CENTER, range, C_WHITE, 255);
}

void ui_draw_format(const ui_model_t *m)
{
    if (m->popup != POP_NONE || m->page != PAGE_NONE || m->set.simple_view) {
        return;
    }
    /* With the title above the box */
    use_layout(m);
    gfx_clear(FMT_X - 4, BOT_Y, PRESET_X - 8 - (FMT_X - 4), H - BOT_Y);
    draw_format(m);
    restore_quick(m);
}

/* The bottom bar without a panel: no background, like on the camera */
static void draw_bottom_bar(const ui_model_t *m)
{
    if (!m->set.simple_view) {
        draw_slate_board(m);
    }
    draw_preset_button(m);
    draw_record(m);
    draw_slots(m);
    draw_play_button(m);
    if (!m->set.simple_view) {
        draw_format(m);
    }
    draw_focus(m);
}

/* ---- Playback: an info row (clip, state, loop, good take) above the transport row ---- */

#define PB_BTN_W        S(84)
#define PB_BTN_H        S(52)
#define PB_GAP          S(14)
#define PB_LIVE_X       S(16)
#define PB_LIVE_W       S(130)
#define PB_GOOD_X       (W - S(16) - TOGGLE_W)        /* the switches in the info row */
#define PB_LOOP_X       (PB_GOOD_X - S(150))
#define PB_SWITCH_Y     (ROW_Y - S(8) - TOGGLE_H)

/* Left edge of the transport button i (PB_PREV .. PB_NEXT), centred as a group */
static int pb_x(int i)
{
    return W / 2 - (5 * PB_BTN_W + 4 * PB_GAP) / 2 + i * (PB_BTN_W + PB_GAP);
}

/* The button the camera's speed lights: forwards, backwards or playing */
static int pb_lit(const cam_state_t *c)
{
    return c->transport_speed > 1 ? PB_FF : c->transport_speed < 0 ? PB_REW : c->transport_speed == 1 ? PB_PLAY : -1;
}

/* Playback panel in place of the bottom bar: the info row with LOOP and GOOD TAKE, the transport row below */
static void draw_playback(const ui_model_t *m)
{
    const cam_state_t *c = &m->cam;
    /* Info row: the clip (the camera's slate target while playing) and the state */
    gfx_set(0, PRE_Y, W, ROW_Y - PRE_Y, 0, PRESET_A);
    gfx_fill(0, ROW_Y - 1, W, 1, C_WHITE, 120);
    const char *target = m->slate.text[CAM_SLATE_TARGET], *slash = strrchr(target, '/');
    char clip[CAM_SLATE_TEXT + 4];
    snprintf(clip, sizeof(clip), "%s", slash ? slash + 1 : target[0] ? target : "–");
    const int s = c->transport_speed;
    char state[24];
    if (s == 0) {
        strcpy(state, "PAUSED");
    } else if (s == 1) {
        strcpy(state, "PLAYING");
    } else {
        snprintf(state, sizeof(state), "%dx %s", s > 0 ? s : -s, s > 0 ? "FORWARD" : "BACKWARD");
    }
    const int cy = (PRE_Y + ROW_Y) / 2;
    gfx_text(L->f13, S(16), cy - S(8), "PLAYBACK", C_LABEL, 255);
    const int sw = gfx_text_width(L->f20, state);
    fit_text(L->f20, clip, PB_LOOP_X - S(40) - S(16) - sw - S(24));
    const int cw = gfx_text(L->f20, S(16), cy + S(16), clip, C_WHITE, 255);
    gfx_text(L->f20, S(16) + cw + S(24), cy + S(16), state, s == 0 ? C_WHITE : C_ORANGE, 255);
    labelled_toggle(PB_LOOP_X, PB_SWITCH_Y, "LOOP", c->transport_flags & 1);
    labelled_toggle(PB_GOOD_X, PB_SWITCH_Y, "GOOD TAKE", m->slate.have && m->slate.good_take);

    /* Transport row: LIVE, then previous, backwards, play/pause, forwards, next */
    gfx_set(0, ROW_Y, W, H - ROW_Y, 0, SHADE_A);
    const int by = ROW_CY - PB_BTN_H / 2;
    shaded_box(PB_LIVE_X, by, PB_LIVE_W, PB_BTN_H, S(8), 2, C_GREY);
    gfx_text_at(L->f20, PB_LIVE_X + PB_LIVE_W / 2, ROW_CY + L->f20->cap_h / 2, GFX_CENTER, "LIVE", C_WHITE, 255);
    const int lit = pb_lit(c);
    for (int i = PB_PREV; i <= PB_NEXT; i++) {
        const int x = pb_x(i);
        const uint32_t col = i == lit ? C_ORANGE : C_WHITE;
        shaded_box(x, by, PB_BTN_W, PB_BTN_H, S(8), i == lit ? 3 : 2, i == lit ? C_ORANGE : C_GREY);
        pb_icon(i, x + PB_BTN_W / 2.0f, ROW_CY, S(22), s != 0, col);
    }
}

/* ---- Parameter panels ---- */

typedef struct {
    int slider_x0, slider_x1;   /* slider_x1 = 0: no slider */
    int val_x[2];               /* centres of the value steppers, 0 = none */
    int toggle_x;               /* 0 = no toggle */
    const char *toggle_label;
    int presets;                /* number of cells in the presets row */
} row_layout_t;

/* Where the open panel's control row has its slider, steppers and switch, and how many presets go above it */
static row_layout_t row_layout(const ui_model_t *m)
{
    row_layout_t l = {0};
    switch (m->popup) {
    case POP_ISO:
        l = (row_layout_t){40, 930, {1105, 0}, 0, NULL, UI_ISO_PRESET_COUNT};
        break;
    case POP_IRIS:
        /* No slider or stepper while auto exposure drives the iris */
        l = ui_iris_auto(m->cam.ae_mode) ? (row_layout_t){0, 0, {0, 0}, 1130, "AUTO EXPOSURE", 0}
                                         : (row_layout_t){40, 640, {800, 0}, 1130, "AUTO EXPOSURE", 0};
        break;
    case POP_SHUTTER:
        /* No stepper while auto exposure drives the shutter */
        l = (row_layout_t){0, 0, {ui_shutter_auto(m->cam.ae_mode) ? 0 : 170, 0}, 1130, "AUTO EXPOSURE", 0};
        break;
    case POP_FPS:
        l = (row_layout_t){40, 640, {820, 0}, 1130, "OFF SPEED",
                           ui_off_speed(m) ? ui_offspeed_preset_count(&m->cam) : 0};
        break;
    case POP_WB:
        l = (row_layout_t){40, 520, {700, 1105}, 0, NULL, UI_WB_PRESET_COUNT};
        break;
    case POP_TOOLS:
        /* Guides: opacity, ratio and switch; brightness: no switch; grid and safe area: no slider */
        switch (m->tool) {
        case TOOL_GUIDES:     l = (row_layout_t){230, 900, {1105, 0}, 40, NULL, TOOL_COUNT}; break;
        case TOOL_BRIGHTNESS: l = (row_layout_t){230, 900, {1105, 0}, 0, NULL, TOOL_COUNT}; break;
        default:              l = (row_layout_t){0, 0, {1105, 0}, 40, NULL, TOOL_COUNT}; break;
        }
        break;
    default:
        break;
    }
    return l;
}

int ui_nearest_index(const int32_t *list, int n, int32_t v)
{
    int best = 0;
    int32_t dist = INT32_MAX;
    for (int i = 0; i < n; i++) {
        const int32_t d = list[i] > v ? list[i] - v : v - list[i];
        if (d < dist) {
            dist = d;
            best = i;
        }
    }
    return best;
}

/* Off-speed in the FPS panel: the project frame rate gets a row of its own above the presets, its
   slider and stepper where the off-speed ones are */
#define PROJ_H          S(76)
#define PROJ_Y          (PRE_Y - PROJ_H)
#define PROJ_CY         (PROJ_Y + PROJ_H / 2 + S(6))

/* The open panel has that row: the FPS panel while recording off-speed */
static bool proj_row(const ui_model_t *m)
{
    return m->popup == POP_FPS && ui_off_speed(m);
}

/* Slider position of the project frame rate among those the resolution allows, 0..1 */
static float project_frac(const cam_state_t *c)
{
    const int n = ui_project_fps_count(c);
    return n > 1 ? (float)ui_project_fps_index(c) / (n - 1) : 0;
}

/* Slider position of the current value, 0..1 */
static float slider_frac(const ui_model_t *m)
{
    const cam_state_t *c = &m->cam;
    switch (m->popup) {
    case POP_ISO:
        return c->iso > 0 ? (float)ui_nearest_index(UI_ISO_STEPS, UI_ISO_STEP_COUNT, c->iso) / (UI_ISO_STEP_COUNT - 1) : 0;
    case POP_IRIS:
        /* Open on the left, like the camera */
        if (c->have_iris_norm) {
            return c->iris_norm / 2048.0f;
        }
        if (c->have_iris) {
            /* f/1.4 .. f/22 */
            return (c->iris_av / 2048.0f - 0.97f) / (8.92f - 0.97f);
        }
        return 0;
    case POP_FPS:
        if (ui_off_speed(m)) {
            return (float)(c->sensor_fps - UI_OFFSPEED_MIN) / (ui_max_fps(c) - UI_OFFSPEED_MIN);
        }
        return project_frac(c);
    case POP_WB:
        return c->have_wb ? (float)(c->wb_kelvin - UI_WB_MIN) / (UI_WB_MAX - UI_WB_MIN) : 0;
    case POP_TOOLS:
        return m->tool == TOOL_GUIDES ? m->cam.guide_opacity / 100.0f : (m->set.brightness - 10) / 90.0f;
    default:
        return 0;
    }
}

/* Text of the value stepper i of a panel; also the status bar's values */
static void value_text(const ui_model_t *m, ui_popup_t popup, int i, char *out, size_t size)
{
    const cam_state_t *c = &m->cam;
    const bool on = connected(m);
    strcpy(out, "--");
    switch (popup) {
    case POP_ISO:
        if (on && c->iso > 0) {
            snprintf(out, size, "%ld", (long)c->iso);
        }
        break;
    case POP_IRIS:
        if (on && c->have_iris) {
            cam_fmt_iris(out, size, c->iris_av);
        }
        break;
    case POP_SHUTTER:
        if (on) {
            fmt_cam_shutter(c, out, size);
        }
        break;
    case POP_FPS:
        if (on && c->have_format) {
            if (ui_off_speed(m)) {
                snprintf(out, size, "%d", c->sensor_fps);
            } else {
                cam_fmt_fps(out, size, c->file_fps, c->format_flags & CAM_FMT_FILE_MRATE);
            }
        }
        break;
    case POP_WB:
        if (on && c->have_wb) {
            snprintf(out, size, i == 0 ? "%dK" : "%d", i == 0 ? c->wb_kelvin : c->wb_tint);
        }
        break;
    case POP_TOOLS:
        switch (m->tool) {
        case TOOL_GUIDES:     snprintf(out, size, "%s", UI_GUIDE_RATIOS[m->set.guide_ratio].label); break;
        case TOOL_GRID:       snprintf(out, size, "%s", m->set.grid_style == 0 ? "Thirds" : m->set.grid_style == 1 ? "Cross" : "Both"); break;
        case TOOL_SAFE:       snprintf(out, size, "%d%%", m->set.safe_pct); break;
        case TOOL_BRIGHTNESS: snprintf(out, size, "%d%%", m->set.brightness); break;
        default: break;
        }
        break;
    default:
        break;
    }
}

/* The open panel's switch is on: auto exposure, off-speed, or the tool's overlay at the camera */
static bool toggle_state(const ui_model_t *m)
{
    const int8_t ae = m->cam.ae_mode;
    switch (m->popup) {
    case POP_IRIS:    return ui_iris_auto(ae);
    case POP_SHUTTER: return ui_shutter_auto(ae);
    case POP_FPS:     return ui_off_speed(m);
    case POP_TOOLS:
        return mon_state(m, m->tool == TOOL_GUIDES ? MON_GUIDE : m->tool == TOOL_GRID ? MON_GRID : MON_SAFE_AREA);
    default:          return false;
    }
}

/* Index of the highlighted preset, -1 = none */
static int preset_selected(const ui_model_t *m)
{
    const cam_state_t *c = &m->cam;
    if (!connected(m) && m->popup != POP_TOOLS) {
        return -1;
    }
    switch (m->popup) {
    case POP_ISO:
        for (int i = 0; i < UI_ISO_PRESET_COUNT; i++) {
            if (UI_ISO_PRESETS[i] == c->iso) {
                return i;
            }
        }
        return -1;
    case POP_WB:
        /* A preset, or CWB as on the camera for any other white balance */
        if (!c->have_wb) {
            return -1;
        }
        if (ui_wb_preset_index(c->wb_kelvin, c->wb_tint) >= 0) {
            return ui_wb_preset_index(c->wb_kelvin, c->wb_tint);
        }
        for (int i = 0; i < UI_WB_PRESET_COUNT; i++) {
            if (UI_WB_PRESETS[i].kelvin < 0) {
                return i;
            }
        }
        return -1;
    case POP_FPS:
        for (int i = 0; i < ui_offspeed_preset_count(c); i++) {
            if (UI_OFFSPEED_PRESETS[i] == c->sensor_fps) {
                return i;
            }
        }
        return -1;
    case POP_TOOLS:
        return m->tool;
    default:
        return -1;
    }
}

/* Outline of overlapping circles {x, y, r} standing on a flat base: each circle's arc outside the others */
static void cloud_outline(const float (*c)[3], int n, float base, uint32_t col)
{
    for (int i = 0; i < n; i++) {
        float px = 0, py = 0;
        bool prev = false;
        for (int k = 0; k <= 48; k++) {
            const float a = k * 2 * 3.14159f / 48;
            const float x = c[i][0] + c[i][2] * cosf(a), y = c[i][1] + c[i][2] * sinf(a);
            bool out = y <= base + 0.5f;
            for (int j = 0; j < n && out; j++) {
                out = j == i || hypotf(x - c[j][0], y - c[j][1]) > c[j][2] - 0.3f;
            }
            if (out && prev) {
                gfx_line(px, py, x, y, 2, col, 255);
            }
            px = x;
            py = y;
            prev = out;
        }
    }
    gfx_line(c[0][0], base, c[n - 1][0], base, 2, col, 255);
}

/* White balance preset icons, after the camera's */
static void wb_icon(int i, int cx, int cy, uint32_t col)
{
    switch (i) {
    case 0:     /* sun */
        gfx_ring(cx, cy, 8, 2, col, 255);
        for (int k = 0; k < 8; k++) {
            const float a = k * 3.14159f / 4;
            gfx_line(cx + 12 * cosf(a), cy + 12 * sinf(a), cx + 16 * cosf(a), cy + 16 * sinf(a), 2, col, 255);
        }
        break;
    case 1:     /* bulb */
        gfx_ring(cx, cy - 3, 8, 2, col, 255);
        gfx_fill(cx - 4, cy + 7, 8, 2, col, 255);
        gfx_fill(cx - 3, cy + 11, 6, 2, col, 255);
        for (int k = 0; k < 5; k++) {
            const float a = 3.14159f + k * 3.14159f / 4;
            gfx_line(cx + 12 * cosf(a), cy - 3 + 12 * sinf(a), cx + 16 * cosf(a), cy - 3 + 16 * sinf(a), 2, col, 255);
        }
        break;
    case 2:     /* fluorescent tube */
        gfx_rrect_stroke(cx - 16, cy - 5, 32, 10, 5, 2, col, 255);
        for (int k = -1; k <= 1; k++) {
            gfx_line(cx + k * 9, cy - 9, cx + k * 11, cy - 14, 2, col, 255);
            gfx_line(cx + k * 9, cy + 9, cx + k * 11, cy + 14, 2, col, 255);
        }
        break;
    case 3: {   /* shade: a house with a long roof to the right, hatched beneath it */
        gfx_line(cx - 15, cy - 1, cx - 6, cy - 13, 2, col, 255);
        gfx_line(cx - 6, cy - 13, cx + 16, cy + 6, 2, col, 255);
        gfx_line(cx - 13, cy - 3, cx - 13, cy + 12, 2, col, 255);
        gfx_line(cx - 13, cy + 12, cx + 16, cy + 12, 2, col, 255);
        for (int k = 0; k < 4; k++) {
            gfx_line(cx - 2 + k * 5, cy + 11, cx + 3 + k * 5, cy + 3, 1.6f, col, 255);
        }
        break;
    }
    case 4: {   /* cloud */
        const float c[3][3] = {{cx - 9.0f, cy + 3.0f, 6}, {cx, cy - 2.0f, 9}, {cx + 9.0f, cy + 2.0f, 7}};
        cloud_outline(c, 3, cy + 9.0f, col);
        break;
    }
    default:    /* CWB and AWB, boxed as on the camera */
        gfx_rrect_stroke(cx - 22, cy - 12, 44, 24, 3, 2, col, 255);
        gfx_text_at(&font_13b, cx, cy + 5, GFX_CENTER, UI_WB_PRESETS[i].label, col, 255);
        break;
    }
}

/* Tool icons of the tools panel */
static void tool_icon(int i, int cx, int cy, uint32_t col)
{
    switch (i) {
    case TOOL_GUIDES:
        gfx_rrect_stroke(cx - 16, cy - 11, 32, 22, 3, 2, col, 255);
        gfx_fill(cx - 16, cy - 7, 32, 2, col, 255);
        gfx_fill(cx - 16, cy + 5, 32, 2, col, 255);
        break;
    case TOOL_GRID:
        gfx_rrect_stroke(cx - 16, cy - 11, 32, 22, 2, 2, col, 255);
        for (int k = 1; k <= 2; k++) {
            gfx_fill(cx - 16 + 32 * k / 3 - 1, cy - 11, 2, 22, col, 255);
            gfx_fill(cx - 16, cy - 11 + 22 * k / 3 - 1, 32, 2, col, 255);
        }
        break;
    case TOOL_SAFE:
        gfx_rrect_stroke(cx - 16, cy - 11, 32, 22, 3, 2, col, 255);
        gfx_rrect_stroke(cx - 9, cy - 5, 18, 10, 1, 2, col, 255);
        break;
    default:    /* brightness */
        gfx_circle(cx, cy, 6, col, 255);
        for (int k = 0; k < 8; k++) {
            const float a = k * 3.14159f / 4;
            gfx_line(cx + 10 * cosf(a), cy + 10 * sinf(a), cx + 14 * cosf(a), cy + 14 * sinf(a), 2.4f, col, 255);
        }
        break;
    }
}

/* A choice in a panel, centred on cx: orange and underlined while it is the current one, as the menus' selection */
static void choice_text(int cx, int base, int line_y, const char *t, bool current)
{
    gfx_text_at(L->f26, cx, base, GFX_CENTER, t, current ? C_ORANGE : C_WHITE, 255);
    if (current) {
        const int tw = gfx_text_width(L->f26, t);
        gfx_fill(cx - tw / 2, line_y, tw, S(3), C_ORANGE, 255);
    }
}

/* dy: moved down, while the FPS panel's rows slide */
static void draw_presets(const ui_model_t *m, int n, int dy)
{
    gfx_set(0, PRE_Y + dy, W, ROW_Y - PRE_Y, 0, PRESET_A);
    const int sel = preset_selected(m);
    const int cw = W / n;
    const int cy = (PRE_Y + ROW_Y) / 2 + dy;
    for (int i = 0; i < n; i++) {
        const int cx = i * cw + cw / 2;
        /* The current one in yellow and underlined, as the menus' selection */
        const uint32_t col = i == sel ? C_ORANGE : C_WHITE;
        if (m->popup == POP_WB || m->popup == POP_TOOLS) {
            if (m->popup == POP_WB) {
                wb_icon(i, cx, cy, col);
            } else {
                tool_icon(i, cx, cy, col);
            }
            if (i == sel) {
                gfx_fill(cx - S(20), cy + S(20), S(40), S(3), C_ORANGE, 255);
            }
            continue;
        }
        char t[12];
        snprintf(t, sizeof(t), "%ld", (long)(m->popup == POP_ISO ? UI_ISO_PRESETS[i] : UI_OFFSPEED_PRESETS[i]));
        choice_text(cx, cy + S(10), cy + S(17), t, i == sel);
    }
    gfx_fill(0, ROW_Y - 1 + dy, W, 1, C_WHITE, 120);
}

/* The FPS panel's rows above its own: what the FPS panel cleared above the bottom bar, cleared once more
   after it closes */
static int s_proj_y0 = -1, s_proj_y1;

/* The FPS panel's off-speed rows: the project frame rate and the off-speed presets. They slide up from
   behind the panel's row as off-speed goes on, and back down as it goes off */
static void draw_fps_rows(const ui_model_t *m, const row_layout_t *l)
{
    const cam_state_t *c = &m->cam;
    /* fps_rows moves at an even pace; eased, the rows slow down as they reach the top and leave it slowly */
    const float e = 1.0f - (1.0f - m->fps_rows) * (1.0f - m->fps_rows);
    const int dy = (int)lroundf((1.0f - e) * (ROW_Y - PROJ_Y));
    gfx_clip(0, 0, W, ROW_Y);
    gfx_set(0, PROJ_Y + dy, W, PROJ_H, 0, SHADE_A);
    gfx_fill(0, PRE_Y - 1 + dy, W, 1, C_WHITE, 120);
    gfx_text(L->f13, l->slider_x0, PROJ_CY - S(22) + dy, "PROJECT FRAME RATE", C_WHITE, 255);
    slider(l->slider_x0, l->slider_x1, PROJ_CY + dy, project_frac(c));
    char t[16] = "--";
    if (connected(m) && c->have_format) {
        cam_fmt_fps(t, sizeof(t), c->file_fps, c->format_flags & CAM_FMT_FILE_MRATE);
    }
    stepper(l->val_x[0], PROJ_CY + dy, t);
    const int n = ui_offspeed_preset_count(c);
    if (n > 0) {
        draw_presets(m, n, dy);
    }
    gfx_clip_reset();
}

/* Open panel in place of the bottom bar: the presets row (or the FPS panel's off-speed rows), the control row */
static void draw_panel(const ui_model_t *m)
{
    if (m->popup == POP_PLAYBACK) {
        draw_playback(m);
        return;
    }
    const row_layout_t l = row_layout(m);
    if (m->popup == POP_FPS) {
        if (m->fps_rows > 0) {
            draw_fps_rows(m, &l);
        }
    } else if (l.presets) {
        draw_presets(m, l.presets, 0);
    }
    gfx_set(0, ROW_Y, W, H - ROW_Y, 0, SHADE_A);
    if (l.slider_x1) {
        if (m->popup == POP_FPS) {
            gfx_text(L->f13, l.slider_x0, ROW_CY - S(22), ui_off_speed(m) ? "OFF SPEED FRAME RATE" : "PROJECT FRAME RATE",
                     C_WHITE, 255);
        }
        slider(l.slider_x0, l.slider_x1, ROW_CY, slider_frac(m));
    }
    for (int i = 0; i < 2; i++) {
        if (l.val_x[i]) {
            char t[24];
            value_text(m, m->popup, i, t, sizeof(t));
            stepper(l.val_x[i], ROW_CY, t);
        }
    }
    if (l.toggle_x) {
        /* Level with the row's values, its label just above */
        labelled_toggle(l.toggle_x, ROW_CY - S(17), l.toggle_label, toggle_state(m));
    }
    const int8_t *ae = panel_ae_modes(m);
    if (ae) {
        /* As on the camera: the auto exposure modes instead of the value */
        for (int i = 0; i < UI_AE_MODE_COUNT; i++) {
            choice_text(AE_X(i), ROW_CY + S(4), ROW_CY + S(10), AE_LABELS[ae[i]], m->cam.ae_mode == ae[i]);
        }
    } else if (m->popup == POP_SHUTTER) {
        /* 90, 180 and 360 degrees, as speeds at the frame rate where the camera measures those */
        const cam_state_t *c = &m->cam;
        const int fps = ui_exposure_fps(c);
        for (int i = 0; i < UI_SHUTTER_PRESET_COUNT; i++) {
            char t[16];
            bool current;
            if (c->shutter_speed_mode) {
                const int32_t speed = ui_speed_from_angle(UI_SHUTTER_PRESETS[i], fps);
                if (!speed) {
                    continue;
                }
                cam_fmt_shutter_speed(t, sizeof(t), speed);
                current = c->shutter_speed == speed;
            } else {
                cam_fmt_shutter(t, sizeof(t), UI_SHUTTER_PRESETS[i]);
                current = c->shutter_angle == UI_SHUTTER_PRESETS[i];
            }
            choice_text(SHUTTER_PRESET_X(i), ROW_CY + S(4), ROW_CY + S(10), t, current);
        }
    }
}

void ui_draw_bottom(const ui_model_t *m)
{
    if (m->page != PAGE_NONE) {
        return;
    }
    use_layout(m);
    if (m->popup == POP_FPS) {
        s_proj_y0 = PROJ_Y;
        s_proj_y1 = L->clear_y;
    }
    if (s_proj_y0 >= 0) {
        gfx_clear(0, s_proj_y0, W, s_proj_y1 - s_proj_y0);
        if (m->popup != POP_FPS) {
            s_proj_y0 = -1;
        }
    }
    gfx_clear(0, L->clear_y, W, H - L->clear_y);
    if (m->popup == POP_NONE) {
        draw_bottom_bar(m);
    } else {
        draw_panel(m);
    }
    restore_quick(m);
}

/* ---- Full screen pages ---- */

#define PIN_BOX_X       390
#define PIN_BOX_Y       160
#define PIN_KEY_X       401
#define PIN_KEY_Y       282
#define PIN_KEY_W       150
#define PIN_KEY_H       76
#define PIN_GAP         14
#define PIN_CANCEL_X    40          /* Cancel, bottom left */
#define PIN_CANCEL_Y    640
#define PIN_CANCEL_W    180
#define PIN_CANCEL_H    56

/* Top left corner of key k: 1 to 9, then DEL, 0 and OK */
static void pin_key_pos(int k, int *x, int *y)
{
    *x = PIN_KEY_X + (k % 3) * (PIN_KEY_W + PIN_GAP);
    *y = PIN_KEY_Y + (k / 3) * (PIN_KEY_H + PIN_GAP);
}

/* PIN entry for pairing: six digit boxes (the next one outlined), the keypad with OK once all six are in, Cancel */
static void draw_pin_page(const ui_model_t *m)
{
    gfx_text_at(&font_32s, W / 2, 86, GFX_CENTER, "Pair with your camera", C_WHITE, 255);
    gfx_text_at(&font_20s, W / 2, 126, GFX_CENTER, "Enter the 6-digit PIN shown on the camera's screen", C_LABEL, 255);
    for (int i = 0; i < 6; i++) {
        const int x = PIN_BOX_X + i * 86;
        gfx_rrect(x, PIN_BOX_Y, 70, 84, 8, C_CELL, 255);
        if (i == m->pin_len) {
            gfx_rrect_stroke(x, PIN_BOX_Y, 70, 84, 8, 2, C_CYAN, 255);
        }
        if (i < m->pin_len) {
            const char d[2] = {m->pin[i], 0};
            gfx_text_at(&font_46s, x + 35, PIN_BOX_Y + 60, GFX_CENTER, d, C_WHITE, 255);
        }
    }
    static const char *keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "OK"};
    for (int k = 0; k < 12; k++) {
        int x, y;
        pin_key_pos(k, &x, &y);
        const bool ok = k == 11;
        const bool enabled = !ok || m->pin_len == 6;
        gfx_rrect(x, y, PIN_KEY_W, PIN_KEY_H, 10, ok && enabled ? C_BLUE : C_CELL, 255);
        gfx_text_at(k == 9 || ok ? &font_26s : &font_32s, x + PIN_KEY_W / 2, y + PIN_KEY_H / 2 + 11, GFX_CENTER,
                    keys[k], enabled ? C_WHITE : C_DIM, 255);
    }
    gfx_rrect_stroke(PIN_CANCEL_X, PIN_CANCEL_Y, PIN_CANCEL_W, PIN_CANCEL_H, 8, 2, C_LABEL, 255);
    gfx_text_at(&font_20s, PIN_CANCEL_X + PIN_CANCEL_W / 2, PIN_CANCEL_Y + PIN_CANCEL_H / 2 + 8, GFX_CENTER, "Cancel",
                C_WHITE, 255);
}

/* Geometry of the menu controls, shared with the hit test */
#define MENU_X0         40
#define MENU_X1         1240
#define MENU_TABS       2

typedef struct {
    int x, y, w, h;
    int item;
} menu_cell_t;

/* Fills the cells of the current menu page; returns their number */
static int menu_cells(const ui_model_t *m, menu_cell_t *out)
{
    int n = 0;
    const cam_state_t *c = &m->cam;
    if (m->menu_tab == 0 && m->menu_page == 0) {
        const int w2 = (MENU_X1 - MENU_X0) / 2;
        out[n++] = (menu_cell_t){MENU_X0, 114, w2 - 2, 80, MI_CODEC_BRAW};
        out[n++] = (menu_cell_t){MENU_X0 + w2, 114, w2, 80, MI_CODEC_PRORES};
        /* Quality as on the camera: ProRes HQ … PXY; Blackmagic RAW constant bitrate or constant
           quality, then that mode's four settings */
        if (c->codec == CAM_CODEC_PRORES) {
            const int vw = (MENU_X1 - MENU_X0) / 4;
            int k = 0;
            for (int i = 0; i < UI_CODEC_COUNT; i++) {
                if (UI_CODECS[i].codec == CAM_CODEC_PRORES) {
                    out[n++] = (menu_cell_t){MENU_X0 + k++ * vw, 200, vw - 2, 76, MI_CODEC_VARIANT + i};
                }
            }
        } else {
            const int vw = (MENU_X1 - MENU_X0) / 6;
            const bool quality = c->codec == CAM_CODEC_BRAW && ui_braw_quality(c->codec_variant);
            out[n++] = (menu_cell_t){MENU_X0, 200, vw - 2, 76, MI_BRAW_BITRATE};
            out[n++] = (menu_cell_t){MENU_X0 + vw, 200, vw - 2, 76, MI_BRAW_QUALITY};
            int k = 2;
            for (int i = 0; i < UI_CODEC_COUNT; i++) {
                if (UI_CODECS[i].codec == CAM_CODEC_BRAW && UI_CODECS[i].quality == quality) {
                    out[n++] = (menu_cell_t){MENU_X0 + k++ * vw, 200, vw - 2, 76, MI_CODEC_VARIANT + i};
                }
            }
        }
        const int rw = (MENU_X1 - MENU_X0) / 4;
        for (int i = 0; i < UI_RESOLUTION_COUNT; i++) {
            out[n++] = (menu_cell_t){MENU_X0 + (i % 4) * rw, 322 + (i / 4) * 116, rw - 2, 110, MI_RESOLUTION + i};
        }
    } else if (m->menu_tab == 0 && m->menu_page == 1) {
        static const int8_t dr_mode[3] = {1, 2, 0};     /* shown as Video, Extended Video, Film */
        const int dw = (MENU_X1 - MENU_X0) / 3;
        for (int i = 0; i < 3; i++) {
            out[n++] = (menu_cell_t){MENU_X0 + i * dw, 114, dw - 2, 80, MI_DYN_RANGE + dr_mode[i]};
        }
        out[n++] = (menu_cell_t){MENU_X0, 260, 80, 80, MI_FPS_DEC};
        out[n++] = (menu_cell_t){MENU_X0 + 342, 260, 80, 80, MI_FPS_INC};
        out[n++] = (menu_cell_t){MENU_X0 + 480, 260, 160, 80, MI_OFFSPEED_TOGGLE};
        out[n++] = (menu_cell_t){MENU_X0 + 700, 260, 80, 80, MI_OFFSPEED_DEC};
        out[n++] = (menu_cell_t){MENU_X1 - 80, 260, 80, 80, MI_OFFSPEED_INC};
        const int sw = (MENU_X1 - MENU_X0) / 4;
        for (int i = 0; i < 4; i++) {
            out[n++] = (menu_cell_t){MENU_X0 + i * sw, 406, sw - 2, 80, MI_SHARPEN + i};
        }
    } else if (m->menu_tab == 1) {
        out[n++] = (menu_cell_t){MENU_X0, 214, 280, 72, MI_FORGET};
        out[n++] = (menu_cell_t){MENU_X0, 342, 280, 72, MI_TC_SOURCE + 0};
        out[n++] = (menu_cell_t){MENU_X0 + 286, 342, 280, 72, MI_TC_SOURCE + 1};
        out[n++] = (menu_cell_t){640, 342, 297, 72, MI_LIVE_VIEW};
        out[n++] = (menu_cell_t){943, 342, 297, 72, MI_LIVE_VIEW + 1};
    }
    return n;
}

int ui_page_count(const ui_model_t *m)
{
    if (m->page == PAGE_PRESETS) {
        return (UI_PRESET_COUNT + UI_PRESETS_PER_PAGE - 1) / UI_PRESETS_PER_PAGE;
    }
    return m->page == PAGE_MENU && m->menu_tab == 0 ? 2 : 1;
}

/* Full-screen pages: the title bar; at the bottom a bar with BACK, and < > with more than one page; under
   it the status bar */
#define TITLE_H         56
#define BAR_Y           590
#define BAR_H           58
#define BAR_BACK_W      300
#define BAR_ARROW_W     120         /* < and > */
#define BAR_PREV_X      (W - 16 - 2 * BAR_ARROW_W - 12)
#define BAR_NEXT_X      (W - 16 - BAR_ARROW_W)
#define STATUS_BAR_Y    662

/* ---- Presets page: MagicPilot_Remote's preset page, six presets per page in two columns ---- */

#define PS_ROWS         3           /* cards per column */
#define PS_COL_X        16
#define PS_COL_DX       632
#define PS_CARD_W       616
#define PS_CARD_H       162
#define PS_ROW_Y        68
#define PS_ROW_DY       172
#define PS_BTN_X        20          /* SAVE, from the card's left edge; LOAD and CLEAR follow */
#define PS_BTN_Y        100         /* from the card's top */
#define PS_BTN_DX       196
#define PS_BTN_W        184
#define PS_BTN_H        48
#define PS_RENAME_H     96          /* name and values above the buttons: tap to rename */
#define PS_SD_W         250         /* SAVE TO SD and LOAD FROM SD, right of BACK */
#define PS_SD_SAVE_X    (PS_COL_X + BAR_BACK_W + 16)
#define PS_SD_LOAD_X    (PS_SD_SAVE_X + PS_SD_W + 16)

/* Top left corner of the r-th card on the page: presets 1-3 of the page on the left, 4-6 on the right */
static void preset_card_pos(int r, int *x, int *y)
{
    *x = PS_COL_X + (r / PS_ROWS) * PS_COL_DX;
    *y = PS_ROW_Y + (r % PS_ROWS) * PS_ROW_DY;
}

/* SAVE, LOAD or CLEAR; the outline is only drawn where it differs from the fill */
static void action_button(int x, int y, uint32_t fill, uint32_t outline, uint32_t text_col, const char *label)
{
    gfx_rrect(x, y, PS_BTN_W, PS_BTN_H, 8, fill, 255);
    if (outline != fill) {
        gfx_rrect_stroke(x, y, PS_BTN_W, PS_BTN_H, 8, 2, outline, 255);
    }
    gfx_text_at(&font_20s, x + PS_BTN_W / 2, y + PS_BTN_H / 2 + 7, GFX_CENTER, label, text_col, 255);
}

/* Pencil after a preset's name: tap the name to rename the preset */
static void pencil_icon(int x, int y, uint32_t col)
{
    gfx_line(x + 5, y + 10, x + 13, y + 2, 3.5f, col, 255);
    gfx_line(x + 1, y + 14, x + 4, y + 11, 1.5f, col, 255);
}

/* One preset on the r-th card of the page: its name and saved values, SAVE, LOAD and CLEAR below */
static void draw_preset_card(const ui_model_t *m, int i, int r)
{
    int x, y;
    preset_card_pos(r, &x, &y);
    const ui_preset_t *p = &m->presets[i];
    gfx_rrect_stroke(x, y, PS_CARD_W, PS_CARD_H, 8, 2, C_GREY, 255);
    char t[96];
    ui_preset_name(m, i, t, sizeof(t));
    fit_text(&font_26s, t, PS_CARD_W - 40 - 44);
    const int tw = gfx_text(&font_26s, x + 20, y + 40, t, C_WHITE, 255);
    pencil_icon(x + 20 + tw + 12, y + 22, C_GREY);
    if (p->used) {
        char fps[16], rate[12], shutter[12], iris[12] = "";
        cam_fmt_fps(rate, sizeof(rate), p->fps, p->fps_flags & CAM_FMT_FILE_MRATE);
        if (p->fps_flags & CAM_FMT_OFF_SPEED) {
            snprintf(fps, sizeof(fps), "%d/%s", p->sensor_fps, rate);
        } else {
            snprintf(fps, sizeof(fps), "%s", rate);
        }
        if (p->shutter < 0) {
            cam_fmt_shutter_speed(shutter, sizeof(shutter), -p->shutter);
        } else {
            cam_fmt_shutter(shutter, sizeof(shutter), p->shutter);
        }
        if (p->iris_av != INT16_MIN) {
            cam_fmt_iris(iris, sizeof(iris), p->iris_av);
        }
        snprintf(t, sizeof(t), "ISO %ld   FPS %s   SHUT %s", (long)p->iso, fps, shutter);
        gfx_text(&font_15b, x + 20, y + 66, t, C_WHITE, 255);
        char codec[24];
        fmt_codec(p->codec, p->codec_variant, codec, sizeof(codec));
        char res[16];
        ui_fmt_resolution(p->width, p->height, res, sizeof(res));
        snprintf(t, sizeof(t), "WB %dK   TINT %+d   %s%s%s%s%s", p->wb_kelvin, p->wb_tint, iris,
                 iris[0] && codec[0] ? "   " : "", codec, (iris[0] || codec[0]) && res[0] ? "   " : "", res);
        gfx_text(&font_15b, x + 20, y + 88, t, C_WHITE, 255);
    } else {
        gfx_text(&font_15b, x + 20, y + 66, "EMPTY", C_GREY, 255);
    }
    const int bx = x + PS_BTN_X, by = y + PS_BTN_Y;
    const bool armed = m->preset_armed == i;
    action_button(bx, by, C_CONNECT, C_CONNECT, C_WHITE, "SAVE");
    action_button(bx + PS_BTN_DX, by, 0, p->used ? C_GREEN : C_GREY, p->used ? C_WHITE : C_GREY, "LOAD");
    action_button(bx + 2 * PS_BTN_DX, by, armed ? C_RED : 0, armed || p->used ? C_RED : C_GREY,
                  armed || p->used ? C_WHITE : C_GREY, armed ? "SURE?" : "CLEAR");
}

/* Title bar of the presets and keyboard pages: name and Tab5 battery on the left, title in the middle */
static void title_bar(const ui_model_t *m, const char *title)
{
    gfx_fill(0, 0, W, TITLE_H, C_BAR, 255);
    gfx_fill(0, TITLE_H - 2, W, 2, C_GREY, 255);
    const int nw = gfx_text(&font_26s, 24, 38, "MagicPilot LIVE", C_WHITE, 255);
    if (m->m5_mv > 0) {
        char v[12];
        ui_m5bat_text(m, v, sizeof(v));
        const uint32_t col = m5_colour(m);
        const int bx = 24 + nw + 14, bw = gfx_text_width(&font_15b, v) + 20;
        gfx_rrect_stroke(bx, 15, bw, 26, 6, 2, col, 255);
        gfx_text_at(&font_15b, bx + bw / 2, 33, GFX_CENTER, v, col, 255);
    }
    if (title) {
        gfx_text_at(&font_20s, W / 2, 36, GFX_CENTER, title, C_WHITE, 255);
    }
}

/* Tabs in the title bar: n of width w in the middle; the selected one orange, underlined but for inset at
   either end */
static int tab_x(int n, int w, int i)
{
    return W / 2 - n * w / 2 + i * w;
}

/* Draws the tabs with these labels, sel the selected one */
static void draw_tabs(const char *const tabs[], int n, int w, int inset, int sel)
{
    for (int i = 0; i < n; i++) {
        const int x = tab_x(n, w, i);
        gfx_text_at(&font_23s, x + w / 2, 36, GFX_CENTER, tabs[i], i == sel ? C_ORANGE : C_WHITE, 255);
        if (i == sel) {
            gfx_fill(x + inset, TITLE_H - 6, w - 2 * inset, 4, C_ORANGE, 255);
        }
    }
}

/* The tab at x, or -1 */
static int tab_at(int n, int w, int x)
{
    for (int i = 0; i < n; i++) {
        if (x >= tab_x(n, w, i) && x < tab_x(n, w, i) + w) {
            return i;
        }
    }
    return -1;
}

/* A button in the bar at the bottom; red while it waits for a second tap, as CLEAR */
static void bar_button(int x, int w, const char *label, bool armed)
{
    if (armed) {
        gfx_rrect(x, BAR_Y, w, BAR_H, 8, C_RED, 255);
    } else {
        gfx_rrect_stroke(x, BAR_Y, w, BAR_H, 8, 2, C_GREY, 255);
    }
    gfx_text_at(&font_20s, x + w / 2, BAR_Y + BAR_H / 2 + 7, GFX_CENTER, label, C_WHITE, 255);
}

/* BACK's left edge, in line with the buttons above it: the presets' cards, or the menu's and the slate's */
static int back_x(const ui_model_t *m)
{
    return m->page == PAGE_PRESETS ? PS_COL_X : MENU_X0;
}

/* BACK in the bar at the bottom of the page */
static void back_button(const ui_model_t *m)
{
    bar_button(back_x(m), BAR_BACK_W, "BACK", false);
}

/* < or >, grey on the first or last page */
static void page_button(int x, bool next, bool enabled)
{
    gfx_rrect_stroke(x, BAR_Y, BAR_ARROW_W, BAR_H, 8, 2, C_GREY, 255);
    chevron(x + BAR_ARROW_W / 2, BAR_Y + BAR_H / 2, next, enabled ? C_WHITE : C_GREY);
}

/* "PAGE n / m" centred on cx, < and > on the right */
static void pager(int cx, int page, int pages)
{
    char pg[24];
    snprintf(pg, sizeof(pg), "PAGE %d / %d", page + 1, pages);
    gfx_text_at(&font_20s, cx, BAR_Y + BAR_H / 2 + 7, GFX_CENTER, pg, C_WHITE, 255);
    page_button(BAR_PREV_X, false, page > 0);
    page_button(BAR_NEXT_X, true, page + 1 < pages);
}

/* The link: "Connected to" the camera, or what is going on */
static void link_text(const ui_model_t *m, char *out, size_t size)
{
    if (connected(m)) {
        snprintf(out, size, "Connected to %s", m->cam.name[0] ? m->cam.name : "camera");
    } else {
        snprintf(out, size, "%s", m->cam.status);
    }
}

/* Status bar: the last message, otherwise the link */
static void status_bar(const ui_model_t *m)
{
    gfx_fill(0, STATUS_BAR_Y, W, H - STATUS_BAR_Y, C_BAR, 255);
    gfx_fill(0, STATUS_BAR_Y, W, 2, C_GREY, 255);
    char t[64];
    if (m->toast[0]) {
        snprintf(t, sizeof(t), "%s", m->toast);
    } else {
        link_text(m, t, sizeof(t));
    }
    gfx_text_at(&font_20s, W / 2, STATUS_BAR_Y + 37, GFX_CENTER, t, C_WHITE, 255);
}

/* Presets page: the page's six cards, then BACK, SAVE TO SD, LOAD FROM SD and the pager in the bar below */
static void draw_presets_page(const ui_model_t *m)
{
    title_bar(m, "PRESETS");

    for (int r = 0; r < UI_PRESETS_PER_PAGE; r++) {
        const int i = m->preset_page * UI_PRESETS_PER_PAGE + r;
        if (i < UI_PRESET_COUNT) {
            draw_preset_card(m, i, r);
        }
    }
    back_button(m);
    const bool armed = m->preset_armed == UI_PRESET_ARMED_SD;
    bar_button(PS_SD_SAVE_X, PS_SD_W, "SAVE TO SD", false);
    bar_button(PS_SD_LOAD_X, PS_SD_W, armed ? "REPLACE ALL?" : "LOAD FROM SD", armed);
    pager((PS_SD_LOAD_X + PS_SD_W + BAR_PREV_X) / 2, m->preset_page, ui_page_count(m));
    status_bar(m);
}

/* ---- Menu: RECORD and REMOTE, in MagicPilot_Remote's look like the presets page ---- */

#define MENU_TAB_W      200

/* Option: MagicPilot_Remote's value cell, a grey outline, orange when selected */
static void menu_cell(int x, int y, int w, int h, const char *text, const char *sub, bool sel, bool enabled)
{
    gfx_rrect_stroke(x, y, w, h, 6, sel ? 3 : 2, sel ? C_ORANGE : enabled ? C_GREY : C_DARK_GREY, 255);
    const uint32_t col = !enabled ? C_GREY : sel ? C_ORANGE : C_WHITE;
    const char *nl = strchr(text, '\n');
    if (sub) {
        /* Resolutions: the name, the size in pixels under it; after a line break a smaller grey line */
        const char *nl2 = strchr(sub, '\n');
        char size[32];
        snprintf(size, sizeof(size), "%.*s", nl2 ? (int)(nl2 - sub) : (int)strlen(sub), sub);
        const int total = font_29s.cap_h + 11 + font_17b.cap_h + (nl2 ? 9 + font_15b.cap_h : 0);
        const int base = y + (h - total) / 2 + font_29s.cap_h, base2 = base + 11 + font_17b.cap_h;
        gfx_text_at(&font_29s, x + w / 2, base, GFX_CENTER, text, col, 255);
        gfx_text_at(&font_17b, x + w / 2, base2, GFX_CENTER, size, enabled ? C_LABEL : C_GREY, 255);
        if (nl2) {
            gfx_text_at(&font_15b, x + w / 2, base2 + 9 + font_15b.cap_h, GFX_CENTER, nl2 + 1, C_GREY, 255);
        }
    } else if (nl) {
        /* Two lines at the line break */
        char first[32];
        snprintf(first, sizeof(first), "%.*s", (int)(nl - text), text);
        gfx_text_at(&font_23s, x + w / 2, y + h / 2 - 4, GFX_CENTER, first, col, 255);
        gfx_text_at(&font_23s, x + w / 2, y + h / 2 + 4 + font_23s.cap_h, GFX_CENTER, nl + 1, col, 255);
    } else {
        gfx_text_at(&font_23s, x + w / 2, y + h / 2 + font_23s.cap_h / 2, GFX_CENTER, text, col, 255);
    }
}

/* Grey label above a group, as MagicPilot_Remote's cell labels */
static void menu_section(int x, int y, const char *title)
{
    gfx_text(&font_17b, x, y, title, C_GREY, 255);
}

/* < or > beside a value, as MagicPilot_Remote's - and + buttons */
static void menu_step(const menu_cell_t *e, bool next, bool enabled)
{
    gfx_rrect_stroke(e->x, e->y, e->w, e->h, 8, 2, enabled ? C_WHITE : C_DARK_GREY, 255);
    chevron(e->x + e->w / 2, e->y + e->h / 2, next, enabled ? C_WHITE : C_GREY);
}

/* Settings menu: the cells of the tab's current page with their section labels, BACK and the pager */
static void draw_menu_page(const ui_model_t *m)
{
    static const char *tabs[MENU_TABS] = {"RECORD", "REMOTE"};
    title_bar(m, NULL);
    draw_tabs(tabs, MENU_TABS, MENU_TAB_W, 30, m->menu_tab);

    const cam_state_t *c = &m->cam;
    const bool on = connected(m);
    menu_cell_t cells[40];
    const int n = menu_cells(m, cells);
    for (int i = 0; i < n; i++) {
        const menu_cell_t *e = &cells[i];
        bool sel = false;
        char text[32] = "";
        const char *sub = NULL;
        char subbuf[40];
        /* Without the camera only what is the Tab5's own works: forgetting the pairing, live view */
        bool enabled = on || e->item == MI_FORGET || e->item >= MI_LIVE_VIEW;
        if (e->item == MI_CODEC_BRAW || e->item == MI_CODEC_PRORES) {
            const int8_t codec = e->item == MI_CODEC_BRAW ? CAM_CODEC_BRAW : CAM_CODEC_PRORES;
            sel = on && c->codec == codec;
            strcpy(text, e->item == MI_CODEC_BRAW ? "Blackmagic RAW" : "ProRes");
        } else if (e->item == MI_BRAW_BITRATE || e->item == MI_BRAW_QUALITY) {
            sel = on && c->codec == CAM_CODEC_BRAW && ui_braw_quality(c->codec_variant) == (e->item == MI_BRAW_QUALITY);
            strcpy(text, e->item == MI_BRAW_QUALITY ? "Constant\nQuality" : "Constant\nBitrate");
        } else if (e->item >= MI_CODEC_VARIANT && e->item < MI_RESOLUTION) {
            const ui_codec_t *k = &UI_CODECS[e->item - MI_CODEC_VARIANT];
            sel = on && c->codec == k->codec && c->codec_variant == k->variant;
            strcpy(text, k->label);
        } else if (e->item >= MI_RESOLUTION && e->item < MI_DYN_RANGE) {
            const ui_resolution_t *r = &UI_RESOLUTIONS[e->item - MI_RESOLUTION];
            sel = on && c->have_format && c->width == r->w && c->height == r->h;
            enabled = enabled && ui_resolution_available(r, c->codec) && ui_resolution_takes_fps(r, c);
            strcpy(text, r->label);
            snprintf(subbuf, sizeof(subbuf), "%d x %d\nup to %d fps", r->w, r->h, r->max_fps);
            sub = subbuf;
        } else if (e->item >= MI_DYN_RANGE && e->item < MI_FPS_DEC) {
            const int mode = e->item - MI_DYN_RANGE;
            sel = on && c->dynamic_range == mode && !(mode < 2 && range_unsure(c));
            strcpy(text, mode == 0 ? "Film" : mode == 1 ? "Video" : "Extended Video");
        } else if (e->item == MI_FPS_DEC || e->item == MI_FPS_INC) {
            menu_step(e, e->item == MI_FPS_INC, on);
            continue;
        } else if (e->item == MI_OFFSPEED_DEC || e->item == MI_OFFSPEED_INC) {
            menu_step(e, e->item == MI_OFFSPEED_INC, on && ui_off_speed(m));
            continue;
        } else if (e->item == MI_OFFSPEED_TOGGLE) {
            toggle_pill(e->x + 30, e->y + (e->h - TOGGLE_H) / 2, ui_off_speed(m));
            continue;
        } else if (e->item >= MI_SHARPEN && e->item < MI_TC_SOURCE) {
            static const char *lv[4] = {"Off", "Low", "Medium", "High"};
            sel = on && c->sharpening == e->item - MI_SHARPEN;
            strcpy(text, lv[e->item - MI_SHARPEN]);
        } else if (e->item >= MI_TC_SOURCE && e->item < MI_FORGET) {
            sel = on && c->tc_source == e->item - MI_TC_SOURCE;
            strcpy(text, e->item == MI_TC_SOURCE ? "Timecode" : "Clip");
        } else if (e->item == MI_FORGET) {
            /* Red as the presets' CLEAR */
            gfx_rrect_stroke(e->x, e->y, e->w, e->h, 8, 2, C_RED, 255);
            gfx_text_at(&font_23s, e->x + e->w / 2, e->y + e->h / 2 + font_23s.cap_h / 2, GFX_CENTER, "Forget camera",
                        C_WHITE, 255);
            continue;
        } else if (e->item >= MI_LIVE_VIEW) {
            sel = m->set.live_view == (e->item == MI_LIVE_VIEW);
            strcpy(text, e->item == MI_LIVE_VIEW ? "HDMI via USB" : "Off (remote only)");
        }
        menu_cell(e->x, e->y, e->w, e->h, text, sub, sel, enabled);
    }

    char t[48];
    if (m->menu_tab == 0 && m->menu_page == 0) {
        menu_section(MENU_X0, 104, "CODEC AND QUALITY");
        menu_section(MENU_X0, 312, c->codec == CAM_CODEC_PRORES ? "RESOLUTION - SENSOR WINDOWED" : "RESOLUTION");
    } else if (m->menu_tab == 0 && m->menu_page == 1) {
        menu_section(MENU_X0, 104, "DYNAMIC RANGE");
        if (on && range_unsure(c)) {
            gfx_text(&font_17b, MENU_X0, 216, "Film or Video: with its LUT on, the camera reports both as Video", C_GREY,
                     255);
        }
        menu_section(MENU_X0, 250, "PROJECT FRAME RATE");
        menu_section(MENU_X0 + 480, 250, "OFF SPEED RECORDING");
        menu_section(MENU_X0 + 700, 250, "OFF SPEED FRAME RATE");
        menu_section(MENU_X0, 396, "DETAIL SHARPENING");
        strcpy(t, "--");
        if (on && c->have_format) {
            char p[12];
            cam_fmt_fps(p, sizeof(p), c->file_fps, c->format_flags & CAM_FMT_FILE_MRATE);
            snprintf(t, sizeof(t), "%s fps", p);
        }
        menu_cell(MENU_X0 + 86, 260, 250, 80, t, NULL, false, on);
        strcpy(t, "--");
        if (on && c->have_format) {
            snprintf(t, sizeof(t), "%d fps", ui_off_speed(m) ? c->sensor_fps : c->file_fps);
        }
        menu_cell(MENU_X0 + 786, 260, MENU_X1 - 86 - (MENU_X0 + 786), 80, t, NULL, false, on && ui_off_speed(m));
    } else {
        menu_section(MENU_X0, 112, "BLUETOOTH");
        link_text(m, t, sizeof(t));
        gfx_text(&font_29s, MENU_X0, 162, t, C_WHITE, 255);
        gfx_text(&font_17b, MENU_X0, 198, "The PIN is asked for again after forgetting the camera.", C_GREY, 255);
        menu_section(MENU_X0, 330, "TIMECODE DISPLAY");
        menu_section(640, 330, "LIVE VIEW");
        /* How the picture gets to the Tab5 */
        gfx_text(&font_23s, 640, 450, "Cable: Camera HDMI into an HDMI to USB", C_LABEL, 255);
        gfx_text(&font_23s, 640, 481, "adapter in the Tab5's USB-A port.", C_LABEL, 255);
        gfx_text(&font_23s, 640, 512, "Wireless (optional): HDMI transmitter and", C_LABEL, 255);
        gfx_text(&font_23s, 640, 543, "receiver into an HDMI to USB adapter in", C_LABEL, 255);
        gfx_text(&font_23s, 640, 574, "the Tab5's USB-A port.", C_LABEL, 255);
        menu_section(MENU_X0, 454, "ABOUT");
        gfx_text(&font_23s, MENU_X0, 490, "MagicPilot LIVE", C_WHITE, 255);
        snprintf(t, sizeof(t), "HDMI input: %s",
                 !m->set.live_view ? "off (remote only)" : m->hdmi_info[0] ? m->hdmi_info : "none");
        gfx_text(&font_23s, MENU_X0, 523, t, C_WHITE, 255);
        snprintf(t, sizeof(t), "ESP32-C6 firmware: %s", m->cp_version[0] ? m->cp_version : "--");
        gfx_text(&font_23s, MENU_X0, 556, t, C_WHITE, 255);
    }
    if (!on && m->menu_tab == 0) {
        gfx_text_at(&font_20s, W / 2, 556, GFX_CENTER, "Connect the camera to change these settings", C_GREY, 255);
    }

    back_button(m);
    if (ui_page_count(m) > 1) {
        pager(W / 2, m->menu_page, ui_page_count(m));
    }
    status_bar(m);
}

/* ---- Slate page: the camera's slate with the tabs of the camera's own slate ---- */

#define SLATE_TABS      3


/* The slate text's name, as its label */
static const char *slate_text_name(uint8_t param)
{
    switch (param) {
    case CAM_SLATE_SCENE:        return "SCENE";
    case CAM_SLATE_CAMERA_ID:    return "CAMERA";
    case CAM_SLATE_OPERATOR:     return "CAMERA OPERATOR";
    case CAM_SLATE_DIRECTOR:     return "DIRECTOR";
    case CAM_SLATE_PROJECT:      return "PROJECT";
    case CAM_SLATE_LENS_TYPE:    return "LENS TYPE";
    case CAM_SLATE_IRIS:         return "IRIS";
    case CAM_SLATE_FOCAL_LENGTH: return "FOCAL LENGTH";
    case CAM_SLATE_DISTANCE:     return "DISTANCE";
    case CAM_SLATE_FILTER:       return "FILTER";
    default:                     return "";
    }
}

int ui_slate_text_max(uint8_t param)
{
    switch (param) {
    case CAM_SLATE_SCENE:     return 8;
    case CAM_SLATE_OPERATOR:
    case CAM_SLATE_DIRECTOR:
    case CAM_SLATE_PROJECT:   return 32;
    case CAM_SLATE_LENS_TYPE: return CAM_SLATE_SEND_MAX;
    default:                  return 20;
    }
}

int ui_slate_item_param(int item)
{
    switch (item) {
    case SL_SCENE:        return CAM_SLATE_SCENE;
    case SL_PROJECT:      return CAM_SLATE_PROJECT;
    case SL_DIRECTOR:     return CAM_SLATE_DIRECTOR;
    case SL_OPERATOR:     return CAM_SLATE_OPERATOR;
    case SL_LENS_TYPE:    return CAM_SLATE_LENS_TYPE;
    case SL_IRIS:         return CAM_SLATE_IRIS;
    case SL_FOCAL_LENGTH: return CAM_SLATE_FOCAL_LENGTH;
    case SL_DISTANCE:     return CAM_SLATE_DISTANCE;
    case SL_FILTER:       return CAM_SLATE_FILTER;
    default:              return -1;
    }
}

#define SLATE_TAB_W     180
#define SL_ROW_Y(r)     (110 + (r) * 114)   /* top of the r-th row of controls, its label 12 above */
#define SL_H            66
#define SL_HALF_W       ((MENU_X1 - MENU_X0 - 30) / 2)      /* lens data: two columns */
#define SL_RIGHT_X      (MENU_X1 - SL_HALF_W)

/* Fills the controls of the current slate tab; returns their number */
static int slate_cells(const ui_model_t *m, menu_cell_t *out)
{
    int n = 0;
    if (m->slate_tab == 0) {
        out[n++] = (menu_cell_t){MENU_X0 + 160, SL_ROW_Y(0), 330, SL_H, SL_SCENE};
        out[n++] = (menu_cell_t){640, SL_ROW_Y(0), 70, SL_H, SL_TAKE_DEC};
        out[n++] = (menu_cell_t){852, SL_ROW_Y(0), 70, SL_H, SL_TAKE_INC};
        out[n++] = (menu_cell_t){960, SL_ROW_Y(0), 210, SL_H, SL_GOOD_TAKE};
        for (int i = 0; i < 4; i++) {
            out[n++] = (menu_cell_t){MENU_X0 + i * 152, SL_ROW_Y(1), 150, SL_H, SL_TAKE_TAG + i};
        }
        for (int i = 0; i < UI_SLATE_SHOT_COUNT; i++) {
            out[n++] = (menu_cell_t){MENU_X0 + i * 152, SL_ROW_Y(2), 150, SL_H, SL_SHOT + i};
        }
        out[n++] = (menu_cell_t){MENU_X0, SL_ROW_Y(3), 200, SL_H, SL_EXTERIOR};
        out[n++] = (menu_cell_t){MENU_X0 + 202, SL_ROW_Y(3), 200, SL_H, SL_INTERIOR};
        out[n++] = (menu_cell_t){MENU_X0 + 456, SL_ROW_Y(3), 200, SL_H, SL_DAY};
        out[n++] = (menu_cell_t){MENU_X0 + 658, SL_ROW_Y(3), 200, SL_H, SL_NIGHT};
    } else if (m->slate_tab == 1) {
        out[n++] = (menu_cell_t){MENU_X0, SL_ROW_Y(0), MENU_X1 - MENU_X0, SL_H, SL_PROJECT};
        out[n++] = (menu_cell_t){MENU_X0, SL_ROW_Y(1), MENU_X1 - MENU_X0, SL_H, SL_DIRECTOR};
        out[n++] = (menu_cell_t){MENU_X0, SL_ROW_Y(2), 70, SL_H, SL_CAMERA_DEC};
        out[n++] = (menu_cell_t){MENU_X0 + 230, SL_ROW_Y(2), 70, SL_H, SL_CAMERA_INC};
        out[n++] = (menu_cell_t){MENU_X0, SL_ROW_Y(3), MENU_X1 - MENU_X0, SL_H, SL_OPERATOR};
    } else {
        out[n++] = (menu_cell_t){MENU_X0, SL_ROW_Y(0), MENU_X1 - MENU_X0, SL_H, SL_LENS_TYPE};
        out[n++] = (menu_cell_t){MENU_X0, SL_ROW_Y(1), SL_HALF_W, SL_H, SL_IRIS};
        out[n++] = (menu_cell_t){SL_RIGHT_X, SL_ROW_Y(1), SL_HALF_W, SL_H, SL_FOCAL_LENGTH};
        out[n++] = (menu_cell_t){MENU_X0, SL_ROW_Y(2), SL_HALF_W, SL_H, SL_DISTANCE};
        out[n++] = (menu_cell_t){SL_RIGHT_X, SL_ROW_Y(2), SL_HALF_W, SL_H, SL_FILTER};
    }
    return n;
}

/* A slate text with a pencil, in a grey outline as the menu's cells: tap to type it */
static void slate_text_cell(const menu_cell_t *e, const char *value, bool enabled)
{
    gfx_rrect_stroke(e->x, e->y, e->w, e->h, 6, 2, enabled ? C_GREY : C_DARK_GREY, 255);
    char t[CAM_SLATE_TEXT + 4];
    snprintf(t, sizeof(t), "%s", value[0] ? value : "–");
    fit_text(&font_23s, t, e->w - 70);
    gfx_text(&font_23s, e->x + 20, e->y + e->h / 2 + font_23s.cap_h / 2, t, enabled && value[0] ? C_WHITE : C_GREY, 255);
    if (enabled) {
        pencil_icon(e->x + e->w - 40, e->y + e->h / 2 - 8, C_GREY);
    }
}

/* Value between the < > of a stepper */
static void slate_value(int x, int y, int w, const char *value, bool on)
{
    gfx_rrect_stroke(x, y, w, SL_H, 6, 2, on ? C_GREY : C_DARK_GREY, 255);
    gfx_text_at(&font_29s, x + w / 2, y + SL_H / 2 + font_29s.cap_h / 2, GFX_CENTER, on && value[0] ? value : "–",
                on ? C_WHITE : C_GREY, 255);
}

/* The camera's slate, in the menu's look: tabs in the title bar, BACK and the status bar below */
static void draw_slate_page(const ui_model_t *m)
{
    static const char *tabs[SLATE_TABS] = {"CLIP", "PROJECT", "LENS DATA"};
    title_bar(m, NULL);
    draw_tabs(tabs, SLATE_TABS, SLATE_TAB_W, 24, m->slate_tab);

    const cam_slate_t *s = &m->slate;
    const bool on = connected(m) && s->have;
    menu_cell_t cells[24];
    const int n = slate_cells(m, cells);
    for (int i = 0; i < n; i++) {
        const menu_cell_t *e = &cells[i];
        const int param = ui_slate_item_param(e->item);
        if (param >= 0) {
            menu_section(e->x, e->y - 12, slate_text_name(param));
            slate_text_cell(e, on ? s->text[param] : "", on);
        } else if (e->item == SL_TAKE_DEC || e->item == SL_CAMERA_DEC || e->item == SL_TAKE_INC ||
                   e->item == SL_CAMERA_INC) {
            menu_step(e, e->item == SL_TAKE_INC || e->item == SL_CAMERA_INC, on);
        } else if (e->item == SL_GOOD_TAKE) {
            toggle_pill(e->x, e->y + (e->h - TOGGLE_H) / 2, on && s->good_take);
        } else if (e->item >= SL_TAKE_TAG && e->item < SL_SHOT) {
            const int k = e->item - SL_TAKE_TAG;
            menu_cell(e->x, e->y, e->w, e->h, TAKE_TAGS[k], NULL, on && s->take_tag == k - 1, on);
        } else if (e->item >= SL_SHOT && e->item < SL_EXTERIOR) {
            const int k = e->item - SL_SHOT;
            menu_cell(e->x, e->y, e->w, e->h, SHOT_LABELS[k], NULL, on && s->shot == UI_SLATE_SHOTS[k], on);
        } else if (e->item == SL_EXTERIOR || e->item == SL_INTERIOR) {
            menu_cell(e->x, e->y, e->w, e->h, e->item == SL_EXTERIOR ? "EXT" : "INT", NULL,
                      on && s->interior == (e->item == SL_INTERIOR), on);
        } else if (e->item == SL_DAY || e->item == SL_NIGHT) {
            menu_cell(e->x, e->y, e->w, e->h, e->item == SL_DAY ? "DAY" : "NIGHT", NULL,
                      on && s->day == (e->item == SL_DAY), on);
        }
    }

    char t[CAM_SLATE_TEXT + 16];
    if (m->slate_tab == 0) {
        menu_section(MENU_X0, SL_ROW_Y(0) - 12, "REEL");
        menu_section(640, SL_ROW_Y(0) - 12, "TAKE");
        menu_section(960, SL_ROW_Y(0) - 12, "GOOD TAKE");
        menu_section(MENU_X0, SL_ROW_Y(1) - 12, "TAKE TAG");
        menu_section(MENU_X0, SL_ROW_Y(2) - 12, "SHOT TYPE");
        menu_section(MENU_X0, SL_ROW_Y(3) - 12, "LOCATION");
        menu_section(MENU_X0 + 456, SL_ROW_Y(3) - 12, "TIME OF DAY");
        /* The reel is the camera's: not writable over Bluetooth */
        snprintf(t, sizeof(t), "%d", s->reel);
        menu_cell(MENU_X0, SL_ROW_Y(0), 140, SL_H, on ? t : "–", "ON CAMERA", false, false);
        snprintf(t, sizeof(t), "%d", s->take);
        slate_value(712, SL_ROW_Y(0), 138, on ? t : "", on);
        if (on) {
            menu_section(980, SL_ROW_Y(3) - 12, "SLATE FOR");
            snprintf(t, sizeof(t), "%s%s", s->text[CAM_SLATE_TARGET][0] ? s->text[CAM_SLATE_TARGET] : "–",
                     s->mode == 1 ? " (playback)" : "");
            fit_text(&font_23s, t, MENU_X1 - 980);
            gfx_text(&font_23s, 980, SL_ROW_Y(3) + SL_H / 2 + font_23s.cap_h / 2, t, C_WHITE, 255);
        }
    } else if (m->slate_tab == 1) {
        menu_section(MENU_X0, SL_ROW_Y(2) - 12, "CAMERA");
        slate_value(MENU_X0 + 76, SL_ROW_Y(2), 148, on ? s->text[CAM_SLATE_CAMERA_ID] : "", on);
    } else {
        gfx_text(&font_17b, MENU_X0, SL_ROW_Y(3) + 10, "Electronic lenses fill these in by themselves; for other "
                 "lenses, tap a field and type it.", C_GREY, 255);
    }
    if (!connected(m)) {
        gfx_text_at(&font_23s, W / 2, 568, GFX_CENTER, "Connect the camera to see and change the slate", C_GREY, 255);
    } else if (!s->have) {
        gfx_text_at(&font_20s, W / 2, 568, GFX_CENTER, "Waiting for the camera's slate", C_GREY, 255);
    }
    back_button(m);
    status_bar(m);
}

/* ---- On-screen keyboard: renames a preset, types a slate text ---- */

#define KB_X            66
#define KB_W            1148
#define KB_U            104         /* key width; wider keys span several units */
#define KB_GAP          12
#define KB_Y            172
#define KB_ROW_DY       108
#define KB_KEY_H        96
#define KB_FIELD_Y      70
#define KB_FIELD_H      80

typedef struct {
    int x, y, w, h;
    int code;                   /* character, or UI_KB_* */
} kb_key_t;

/* Fills the keys of the keyboard (44); returns their number */
static int kb_keys(kb_key_t *out)
{
    static const char *rows[3] = {"1234567890", "qwertyuiop", "asdfghjkl-"};
    int n = 0;
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 10; i++) {
            out[n++] = (kb_key_t){KB_X + i * (KB_U + KB_GAP), KB_Y + r * KB_ROW_DY, KB_U, KB_KEY_H, rows[r][i]};
        }
    }
    /* Shift, z..m, delete (1.5 + 7 + 1.5 units), then cancel, ".", space, "/", save (2 + 1 + 4 + 1 + 2) */
    const int y3 = KB_Y + 3 * KB_ROW_DY, y4 = KB_Y + 4 * KB_ROW_DY, wide = KB_U + (KB_U + KB_GAP) / 2;
    int x = KB_X;
    out[n++] = (kb_key_t){x, y3, wide, KB_KEY_H, UI_KB_SHIFT};
    x += wide + KB_GAP;
    for (const char *c = "zxcvbnm"; *c; c++) {
        out[n++] = (kb_key_t){x, y3, KB_U, KB_KEY_H, *c};
        x += KB_U + KB_GAP;
    }
    out[n++] = (kb_key_t){x, y3, wide, KB_KEY_H, UI_KB_DEL};
    static const struct { int units, code; } last[5] = {
        {2, UI_KB_CANCEL}, {1, '.'}, {4, ' '}, {1, '/'}, {2, UI_KB_SAVE},
    };
    x = KB_X;
    for (int i = 0; i < 5; i++) {
        const int w = last[i].units * (KB_U + KB_GAP) - KB_GAP;
        out[n++] = (kb_key_t){x, y4, w, KB_KEY_H, last[i].code};
        x += w + KB_GAP;
    }
    return n;
}

/* Keyboard page: what is being typed in the title, the text field with cursor and length, the keys */
static void draw_keyboard_page(const ui_model_t *m)
{
    char t[48];
    if (m->kb_slot >= 0) {
        snprintf(t, sizeof(t), "RENAME PRESET %d", m->kb_slot + 1);
    } else {
        snprintf(t, sizeof(t), "SLATE: %s", slate_text_name(m->kb_param));
    }
    title_bar(m, t);

    /* Text field, cursor at the end; while a preset name is empty the default name shows in grey */
    gfx_rrect_stroke(KB_X, KB_FIELD_Y, KB_W, KB_FIELD_H, 8, 2, C_GREY, 255);
    const int base = KB_FIELD_Y + KB_FIELD_H / 2 + font_32s.cap_h / 2, tx = KB_X + 24;
    const gfx_font_t *f = gfx_text_width(&font_32s, m->kb_text) <= KB_W - 150 ? &font_32s : &font_20s;
    const int tw = gfx_text(f, tx, base, m->kb_text, C_WHITE, 255);
    if (m->kb_len == 0 && m->kb_slot >= 0) {
        snprintf(t, sizeof(t), "Preset %d", m->kb_slot + 1);
        gfx_text(&font_32s, tx + 8, base, t, C_GREY, 255);
    }
    gfx_fill(tx + tw + 2, base - font_32s.cap_h - 6, 3, font_32s.cap_h + 14, C_WHITE, 255);
    snprintf(t, sizeof(t), "%d/%d", m->kb_len, m->kb_max);
    gfx_text_at(&font_15b, KB_X + KB_W - 20, base - 4, GFX_RIGHT, t, C_GREY, 255);

    kb_key_t keys[48];
    const int n = kb_keys(keys);
    for (int i = 0; i < n; i++) {
        const kb_key_t *k = &keys[i];
        uint32_t fill = 0, outline = C_GREY;
        f = &font_20s;
        char label[8];
        switch (k->code) {
        case UI_KB_SHIFT:
            strcpy(label, m->kb_shift == 2 ? "CAPS" : "SHIFT");
            if (m->kb_shift) {
                fill = outline = C_CONNECT;
            }
            break;
        case UI_KB_DEL:    strcpy(label, "DEL"); break;
        case UI_KB_CANCEL: strcpy(label, "CANCEL"); break;
        case UI_KB_SAVE:   strcpy(label, "SAVE"); fill = outline = C_CONNECT; break;
        case ' ':          strcpy(label, "SPACE"); break;
        default:
            f = &font_32s;
            label[0] = (char)(m->kb_shift && k->code >= 'a' && k->code <= 'z' ? k->code - 'a' + 'A' : k->code);
            label[1] = 0;
            break;
        }
        gfx_rrect(k->x, k->y, k->w, k->h, 8, fill, 255);
        if (outline != fill) {
            gfx_rrect_stroke(k->x, k->y, k->w, k->h, 8, 2, outline, 255);
        }
        gfx_text_at(f, k->x + k->w / 2, k->y + k->h / 2 + f->cap_h / 2, GFX_CENTER, label, C_WHITE, 255);
    }
}

void ui_draw_all(const ui_model_t *m)
{
    gfx_clip_reset();
    s_tc.valid = false;
    /* The pages do not scale: their switches (toggle_pill) at the live layout's size */
    L = &LAYOUT_LIVE;
    if (m->page != PAGE_NONE) {
        gfx_set(0, 0, W, H, m->page == PAGE_PIN ? C_PAGE_BG : 0, 255);
        switch (m->page) {
        case PAGE_PIN:      draw_pin_page(m); break;
        case PAGE_MENU:     draw_menu_page(m); break;
        case PAGE_PRESETS:  draw_presets_page(m); break;
        case PAGE_KEYBOARD: draw_keyboard_page(m); break;
        case PAGE_SLATE:    draw_slate_page(m); break;
        default: break;
        }
        return;
    }
    use_layout(m);
    s_drawing_all = true;
    gfx_clear(0, 0, W, H);
    ui_draw_top(m);
    ui_draw_status(m);
    ui_draw_bottom(m);
    s_drawing_all = false;
    if (m->slate_quick && m->popup == POP_NONE) {
        draw_slate_quick(m);
    }
}

/* ---- Hit testing ---- */

float ui_slider_frac_at(const ui_model_t *m, int x)
{
    const row_layout_t l = row_layout(m);
    if (!l.slider_x1) {
        return 0;
    }
    return clamp01((float)(x - l.slider_x0) / (l.slider_x1 - l.slider_x0));
}

/* -1 on the < of the value stepper centred on cx, 1 on its >, otherwise 0 */
static int stepper_side(int x, int cx)
{
    if (x >= cx - S(150) && x < cx - S(150) + S(95)) {
        return -1;
    }
    return x >= cx + S(55) && x < cx + S(55) + S(95) ? 1 : 0;
}

/* (x, y) lies in the rectangle rx, ry, rw, rh */
static bool in(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* The item of the cell at (x, y), or -1 */
static int cell_at(const menu_cell_t *cells, int n, int x, int y)
{
    for (int i = 0; i < n; i++) {
        if (in(x, y, cells[i].x, cells[i].y, cells[i].w, cells[i].h)) {
            return cells[i].item;
        }
    }
    return -1;
}

/* A button in the bar at the bottom of a page, with some room for fingers */
static bool in_bar(int x, int y, int bx, int bw)
{
    return in(x, y, bx - 6, BAR_Y - 10, bw + 12, BAR_H + 16);
}

ui_hit_t ui_hit_test(const ui_model_t *m, int x, int y)
{
    ui_hit_t h = {HIT_NONE, 0, 0};
    use_layout(m);
    if (m->page == PAGE_PIN) {
        for (int k = 0; k < 12; k++) {
            int kx, ky;
            pin_key_pos(k, &kx, &ky);
            if (in(x, y, kx - 6, ky - 6, PIN_KEY_W + 12, PIN_KEY_H + 12)) {
                h.kind = HIT_PIN_KEY;
                /* Keys 1..9, DEL, 0 and OK as the digits 1..9, 10 (delete), 0 and 11 (OK) */
                h.index = k == 9 ? 10 : k == 10 ? 0 : k == 11 ? 11 : k + 1;
                return h;
            }
        }
        if (in(x, y, PIN_CANCEL_X - 20, PIN_CANCEL_Y - 20, PIN_CANCEL_W + 40, H - (PIN_CANCEL_Y - 20))) {
            h.kind = HIT_PIN_CANCEL;
        }
        return h;
    }
    /* The bar at the bottom of the menu, presets and slate pages: BACK, and < > with more than one page */
    if (m->page == PAGE_MENU || m->page == PAGE_PRESETS || m->page == PAGE_SLATE) {
        const bool pages = ui_page_count(m) > 1;
        if (in_bar(x, y, back_x(m), BAR_BACK_W)) {
            h.kind = HIT_PAGE_BACK;
        } else if (pages && in_bar(x, y, BAR_PREV_X, BAR_ARROW_W)) {
            h.kind = HIT_PAGE_PREV;
        } else if (pages && in_bar(x, y, BAR_NEXT_X, BAR_ARROW_W)) {
            h.kind = HIT_PAGE_NEXT;
        }
        if (h.kind != HIT_NONE) {
            return h;
        }
    }
    if (m->page == PAGE_MENU) {
        if (y < TITLE_H + 10) {
            h.index = tab_at(MENU_TABS, MENU_TAB_W, x);
            h.kind = h.index >= 0 ? HIT_MENU_TAB : HIT_NONE;
            return h;
        }
        menu_cell_t cells[40];
        h.index = cell_at(cells, menu_cells(m, cells), x, y);
        h.kind = h.index >= 0 ? HIT_MENU_ITEM : HIT_NONE;
        return h;
    }
    if (m->page == PAGE_PRESETS) {
        if (in_bar(x, y, PS_SD_SAVE_X, PS_SD_W)) {
            h.kind = HIT_PRESETS_SD_SAVE;
            return h;
        }
        if (in_bar(x, y, PS_SD_LOAD_X, PS_SD_W)) {
            h.kind = HIT_PRESETS_SD_LOAD;
            return h;
        }
        static const ui_hit_kind_t actions[3] = {HIT_PRESETS_SAVE, HIT_PRESETS_LOAD, HIT_PRESETS_CLEAR};
        for (int r = 0; r < UI_PRESETS_PER_PAGE; r++) {
            const int i = m->preset_page * UI_PRESETS_PER_PAGE + r;
            if (i >= UI_PRESET_COUNT) {
                break;
            }
            int cx, cy;
            preset_card_pos(r, &cx, &cy);
            for (int b = 0; b < 3; b++) {
                if (in(x, y, cx + PS_BTN_X + b * PS_BTN_DX - 6, cy + PS_BTN_Y - 6, PS_BTN_W + 12, PS_BTN_H + 12)) {
                    h.kind = actions[b];
                    h.index = i;
                    return h;
                }
            }
            /* The name and values: rename */
            if (in(x, y, cx, cy, PS_CARD_W, PS_RENAME_H)) {
                h.kind = HIT_PRESETS_RENAME;
                h.index = i;
                return h;
            }
        }
        return h;
    }
    if (m->page == PAGE_SLATE) {
        if (y < TITLE_H + 10) {
            h.index = tab_at(SLATE_TABS, SLATE_TAB_W, x);
            h.kind = h.index >= 0 ? HIT_SLATE_TAB : HIT_NONE;
            return h;
        }
        menu_cell_t cells[24];
        h.index = cell_at(cells, slate_cells(m, cells), x, y);
        h.kind = h.index >= 0 ? HIT_SLATE_ITEM : HIT_NONE;
        return h;
    }
    if (m->page == PAGE_KEYBOARD) {
        kb_key_t keys[48];
        const int n = kb_keys(keys);
        for (int i = 0; i < n; i++) {
            const kb_key_t *k = &keys[i];
            if (in(x, y, k->x - KB_GAP / 2, k->y - KB_GAP / 2, k->w + KB_GAP, k->h + KB_GAP)) {
                h.kind = HIT_KB_KEY;
                h.index = k->code;
                return h;
            }
        }
        return h;
    }

    /* Status bar (a little taller than drawn, for fingers) */
    if (y < TOP_H + 16) {
        static const struct { int x0, x1; ui_hit_kind_t k; } cols[] = {
            {0, 118, HIT_FRAME}, {118, 262, HIT_FPS}, {262, 394, HIT_SHUTTER}, {394, 496, HIT_IRIS},
            {496, 806, HIT_TC}, {806, 904, HIT_ISO}, {904, 1058, HIT_WB}, {1058, 1094, HIT_BT},
            {1094, 1170, HIT_POWER}, {1170, W, HIT_SETTINGS},
        };
        for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
            if (x >= cols[i].x0 && x < cols[i].x1) {
                h.kind = cols[i].k;
                return h;
            }
        }
        return h;
    }
    /* Tab5 battery badge, under the camera's; the view badge right of it */
    if (y < STATUS_Y + STATUS_H + 4 && x >= X_PWR - 12 && x < X_PWR + 74) {
        h.kind = HIT_M5BAT;
        return h;
    }
    if (y < STATUS_Y + STATUS_H + 4 && x >= X_VIEW - 4) {
        h.kind = HIT_VIEW;
        return h;
    }
    if (in(x, y, 0, STATUS_Y, 96, LUT_Y - STATUS_Y + 18)) {
        h.kind = HIT_FRAME;
        return h;
    }
    /* Quick slate, over the on/off buttons without live view: above the middle of scene or take steps up,
       below it down; then the buttons */
    if (m->slate_quick && m->popup == POP_NONE && in(x, y, SQ_X, SQ_Y, SQ_W, SQ_H)) {
        h.kind = HIT_SLATE_QUICK;
        const int right = x >= SQ_X + SQ_W / 2;
        if (y < SQ_Y + SQ_STEP_H) {
            h.index = right * 2 + (y >= SQ_Y + SQ_STEP_H / 2);
        } else {
            h.index = 4 + (y >= SQ_Y + SQ_STEP_H + S(8) + SQ_BTN_H + S(4)) * 2 + right;
        }
        return h;
    }
    if (!m->set.simple_view && y >= MON_Y - 8 && y < MON_Y + MON_H + 8) {
        for (int i = 0; i < MON_COUNT; i++) {
            int bx, bw;
            mon_button_pos(i, &bx, &bw);
            if (x >= bx - MON_GAP / 2 && x < bx + bw + MON_GAP / 2) {
                h.kind = HIT_MON_TOGGLE;
                h.index = i;
                return h;
            }
        }
    }
    /* The 2x badge while zoomed, under the frame icon's area; after the on/off buttons, whose margin
       reaches its corner */
    if (m->zoomed && in(x, y, X_FRAME_C - ZOOM_W / 2 - 10, ZOOM_Y - 6, ZOOM_W + 20, ZOOM_H + 16)) {
        h.kind = HIT_ZOOM_OUT;
        return h;
    }

    /* Playback panel: the picture above it, the two switches in its info row, LIVE and transport below */
    if (m->popup == POP_PLAYBACK) {
        if (y < PRE_Y) {
            h.kind = HIT_VIDEO;
        } else if (y < ROW_Y) {
            h.kind = HIT_PANEL;
            if (in(x, y, PB_LOOP_X - 20, PRE_Y, TOGGLE_W + 40, ROW_Y - PRE_Y)) {
                h.kind = HIT_PLAYBACK;
                h.index = PB_LOOP;
            } else if (in(x, y, PB_GOOD_X - 20, PRE_Y, TOGGLE_W + 40, ROW_Y - PRE_Y)) {
                h.kind = HIT_PLAYBACK;
                h.index = PB_GOOD_TAKE;
            }
        } else {
            h.kind = HIT_PANEL;
            if (in(x, y, PB_LIVE_X - 10, ROW_Y, PB_LIVE_W + 20, H - ROW_Y)) {
                h.kind = HIT_PLAYBACK;
                h.index = PB_LIVE;
            }
            for (int i = PB_PREV; i <= PB_NEXT; i++) {
                if (in(x, y, pb_x(i) - PB_GAP / 2, ROW_Y, PB_BTN_W + PB_GAP, H - ROW_Y)) {
                    h.kind = HIT_PLAYBACK;
                    h.index = i;
                }
            }
        }
        return h;
    }
    if (m->popup != POP_NONE) {
        const row_layout_t l = row_layout(m);
        /* The project frame rate row (off-speed): its stepper as value 2, its slider as slider 1 */
        if (proj_row(m) && y >= PROJ_Y && y < PRE_Y) {
            const int side = stepper_side(x, l.val_x[0]);
            h.kind = side < 0 ? HIT_DEC : side > 0 ? HIT_INC : HIT_PANEL;
            h.index = 2;
            if (!side && x >= l.slider_x0 - 30 && x < l.slider_x1 + 30) {
                h.kind = HIT_SLIDER;
                h.index = 1;
                h.frac = ui_slider_frac_at(m, x);
            }
            return h;
        }
        const int top = l.presets ? PRE_Y : ROW_Y;
        if (y < top) {
            h.kind = HIT_VIDEO;
            return h;
        }
        if (y < ROW_Y) {
            h.kind = HIT_PANEL;
            if (l.presets) {
                h.kind = m->popup == POP_TOOLS ? HIT_TOOL : HIT_PRESET;
                h.index = x * l.presets / W;
            }
            return h;
        }
        h.kind = HIT_PANEL;
        if (l.toggle_x && in(x, y, l.toggle_x - 20, ROW_Y, TOGGLE_W + 40, H - ROW_Y)) {
            h.kind = HIT_TOGGLE;
            return h;
        }
        for (int i = 0; i < 2; i++) {
            const int side = l.val_x[i] ? stepper_side(x, l.val_x[i]) : 0;
            if (side) {
                h.kind = side < 0 ? HIT_DEC : HIT_INC;
                h.index = i;
                return h;
            }
        }
        if (l.slider_x1 && x >= l.slider_x0 - 30 && x < l.slider_x1 + 30) {
            h.kind = HIT_SLIDER;
            h.frac = ui_slider_frac_at(m, x);
            return h;
        }
        /* The auto exposure modes, or the shutter's presets, in the control row: HIT_PRESET with their index */
        const int8_t *ae = panel_ae_modes(m);
        if (ae || m->popup == POP_SHUTTER) {
            for (int i = 0; i < UI_SHUTTER_PRESET_COUNT; i++) {
                const int cx = ae ? AE_X(i) : SHUTTER_PRESET_X(i), hw = ae ? 160 : 80;
                if (x >= cx - hw && x < cx + hw) {
                    h.kind = HIT_PRESET;
                    h.index = i;
                    return h;
                }
            }
        }
        return h;
    }

    /* The slate reaches above the bottom bar */
    if (!m->set.simple_view && in(x, y, SLATE_X - 6, slate_top(m) - 6, FMT_X - SLATE_X, H - slate_top(m) + 6)) {
        h.kind = HIT_SLATE_OPEN;
        return h;
    }
    if (in(x, y, PLAY_X - 8, PLAY_Y - 8, PLAY_W + 16, PLAY_H + 16)) {
        h.kind = HIT_PLAY_CLIP;
        return h;
    }
    /* Focus slider and AF button: in the bottom bar, or on their own row */
    if (y >= L->focus_top && y < L->focus_bottom) {
        if (x >= AF_X - 8) {
            h.kind = HIT_AF;
            return h;
        }
        if (x >= FOCUS_X0 - 16) {
            h.kind = HIT_FOCUS;
            h.frac = ui_focus_frac_at(m, x);
            return h;
        }
    }
    /* Bottom bar: the record button, and left of it the slate, the recording format and the preset button */
    if (y >= BOT_Y - 10) {
        if ((x - REC_CX) * (x - REC_CX) + (y - REC_CY) * (y - REC_CY) < S(50) * S(50)) {
            h.kind = HIT_RECORD;
        } else if (x < PRESET_X - 6) {
            if (!m->set.simple_view) {
                h.kind = x < FMT_X - 6 ? HIT_SLATE_OPEN : HIT_FORMAT;
            }
        } else if (x < REC_CX - S(50)) {
            h.kind = HIT_PRESETS_OPEN;
        }
        return h;
    }
    h.kind = HIT_VIDEO;
    return h;
}
