/*
 * Desktop preview of the overlay UI. Draws a set of UI states with the firmware's own drawing and
 * compositing code over a background picture and writes each as raw RGB (see render.py). With --touch
 * it prints instead what ui_hit_test finds in each of them (see tools/check).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "overlay.h"
#include "gfx.h"
#include "ui_draw.h"

static uint16_t bg[OV_W * OV_H];
static bool s_touch;                /* --touch: the touch map of every scene instead of its picture */

/* The model every scene starts from: connected to a camera that has reported its values */
static void connected_state(ui_model_t *m)
{
    memset(m, 0, sizeof(*m));
    cam_state_t *c = &m->cam;
    c->link = CAM_LINK_CONNECTED;
    strcpy(c->status, "Connected");
    strcpy(c->name, "A:89550706");
    c->iso = 400;
    c->shutter_angle = 36000;
    c->have_format = true;
    c->file_fps = 24;
    c->sensor_fps = 48;
    c->width = 6144;
    c->height = 3456;
    c->format_flags = CAM_FMT_OFF_SPEED;
    c->have_wb = true;
    c->wb_kelvin = 4000;
    c->wb_tint = 15;
    c->have_iris = true;
    c->iris_av = cam_iris_av(1.8f);
    c->ae_mode = 0;
    c->dynamic_range = 0;
    c->sharpening = 0;
    c->codec = CAM_CODEC_BRAW;
    c->codec_variant = 4;
    c->lut = 2;
    c->lut_enabled = true;
    c->transport_mode = 0;
    c->transport_flags = 1 << 6;
    c->slot_medium[0] = 0;
    c->slot_medium[1] = 1;
    c->slot_remaining[0] = 0;
    c->slot_remaining[1] = 848;
    c->slot_remaining[2] = 0;
    c->voltage_mv = 15500;
    c->have_tc = true;
    c->tc[0] = 0x18;
    c->tc[1] = 0x08;
    c->tc[2] = 0x32;
    c->tc[3] = 0x13;
    c->tc_source = 0;
    c->focus = 1024;
    m->set = (ui_settings_t){true, 2, false, 0, 60, false, 90, 95, false, false, true};
    strcpy(m->hdmi_info, "1280x720 MJPEG 30 fps");
    m->m5_mv = 7836;
    m->m5_pct = 78;
    m->preset_active = -1;
    m->preset_armed = -1;
    strcpy(m->cp_version, "1.4.1");
    /* The slate as the BMPCC6K reported it */
    cam_slate_t *s = &m->slate;
    s->have = true;
    s->reel = 1;
    s->shot = -1;
    s->interior = 1;
    s->day = 1;
    s->take = 1;
    s->take_tag = -1;
    strcpy(s->text[CAM_SLATE_SCENE], "1");
    strcpy(s->text[CAM_SLATE_CAMERA_ID], "A");
    strcpy(s->text[CAM_SLATE_LENS_TYPE], "Canon EF 50mm f/1.4 USM");
    strcpy(s->text[CAM_SLATE_IRIS], "f1.40");
    strcpy(s->text[CAM_SLATE_FOCAL_LENGTH], "50mm");
    strcpy(s->text[CAM_SLATE_TARGET], "Next Clip");
}

/* Presets for the preset scenes: full ones, one without name, iris or codec, a named unused slot, the longest */
static void some_presets(ui_model_t *m)
{
    m->presets[0] = (ui_preset_t){true, 0, 24, 24, 5600, 0, cam_iris_av(2.8f), 800, 18000, "Day exterior", CAM_CODEC_BRAW, 4,
                                  6144, 3456, false};
    m->presets[1] = (ui_preset_t){true, CAM_FMT_OFF_SPEED, 24, 60, 3200, 10, cam_iris_av(4.0f), 1250, 17280, "Interview A-Cam", CAM_CODEC_PRORES, 0,
                                  4096, 2160, true};
    m->presets[2] = (ui_preset_t){true, CAM_FMT_FILE_MRATE | CAM_FMT_SENSOR_MRATE, 24, 24, 4300, -6, INT16_MIN, 25600, 34560, "", -1, -1};
    m->presets[4].name[0] = 0;
    strcpy(m->presets[5].name, "Night");
    /* The longest summary and name */
    m->presets[6] = (ui_preset_t){true, CAM_FMT_OFF_SPEED | CAM_FMT_FILE_MRATE, 24, 120, 10000, -50, cam_iris_av(16.0f), 25600, 17280,
                                  "WWWWWWWWWWWWWWWW", CAM_CODEC_PRORES, 3, 3728, 3104, true};
}

static void write_frame(const char *dir, const char *name);

/* What a touch finds every 3 px: a line where it changes along a row (the index only counts with a kind) */
static void touch_map(const ui_model_t *m, const char *name)
{
    for (int y = 0; y < OV_H; y += 3) {
        int last_k = -1, last_i = 0, last_f = 0;
        for (int x = 0; x < OV_W; x += 3) {
            const ui_hit_t h = ui_hit_test(m, x, y);
            const int i = h.kind == HIT_NONE ? 0 : h.index, f = (int)lroundf(h.frac * 10000);
            if (h.kind != last_k || i != last_i || f != last_f) {
                printf("%s %d %d: %d %d %d\n", name, y, x, h.kind, i, f);
                last_k = h.kind;
                last_i = i;
                last_f = f;
            }
        }
    }
}

/* Draws a scene whole and writes it to dir/name.rgb, or with --touch prints its touch map instead */
static void render(const ui_model_t *m, const char *dir, const char *name)
{
    /* The FPS panel's off-speed rows as the firmware has them when they don't slide, unless a scene sets them */
    static ui_model_t c;
    c = *m;
    c.live = c.set.live_view;       /* with the adapter's picture */
    if (c.popup == POP_FPS && c.fps_rows == 0) {
        c.fps_rows = (c.cam.format_flags & CAM_FMT_OFF_SPEED) ? 1.0f : 0.0f;
    }
    ui_draw_all(&c);
    overlay_commit();
    if (s_touch) {
        touch_map(&c, name);
        return;
    }
    write_frame(dir, name);
}

/* The overlay as drawn so far over the background, as 8-bit RGB in dir/name.rgb (nothing with --touch) */
static void write_frame(const char *dir, const char *name)
{
    if (s_touch) {
        return;
    }
    static uint16_t row[OV_W];
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.rgb", dir, name);
    FILE *f = fopen(path, "wb");
    for (int y = 0; y < OV_H; y++) {
        memcpy(row, bg + y * OV_W, sizeof(row));
        overlay_composite_row(row, y);
        uint8_t out[OV_W * 3];
        for (int x = 0; x < OV_W; x++) {
            const uint16_t p = row[x];
            out[x * 3 + 0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
            out[x * 3 + 1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            out[x * 3 + 2] = (uint8_t)((p & 31) * 255 / 31);
        }
        fwrite(out, 1, sizeof(out), f);
    }
    fclose(f);
    printf("%s\n", path);
}

/* Every scene in turn, each from connected_state() or the scene before; the tick and drag ones redraw a part only */
int main(int argc, char **argv)
{
    s_touch = argc == 2 && strcmp(argv[1], "--touch") == 0;
    if (argc != 3 && !s_touch) {
        fprintf(stderr, "usage: preview <background.rgb565> <out_dir>\n       preview --touch\n");
        return 1;
    }
    if (!s_touch) {
        FILE *f = fopen(argv[1], "rb");
        if (!f || fread(bg, 2, OV_W * OV_H, f) != OV_W * OV_H) {
            fprintf(stderr, "cannot read background\n");
            return 1;
        }
        fclose(f);
    }
    overlay_init();
    const char *dir = s_touch ? "" : argv[2];
    ui_model_t m;

    connected_state(&m);
    render(&m, dir, "main");
    m.zoomed = true;
    render(&m, dir, "main_zoomed");

    /* Timecode tick: only the changed digits are redrawn */
    connected_state(&m);
    render(&m, dir, "tc_before");
    m.cam.tc[0] = 0x19;
    m.cam.tc[1] = 0x09;
    ui_draw_timecode(&m);
    overlay_commit();
    write_frame(dir, "tc_after_tick");

    connected_state(&m);
    m.cam.link = CAM_LINK_SCANNING;
    strcpy(m.cam.status, "Searching for camera");
    render(&m, dir, "disconnected");

    connected_state(&m);
    m.cam.transport_mode = 2;
    m.cam.tc_source = 1;
    render(&m, dir, "recording");

    static const ui_popup_t pops[] = {POP_ISO, POP_WB, POP_FPS, POP_SHUTTER, POP_IRIS, POP_TOOLS};
    static const char *pop_names[] = {"pop_iso", "pop_wb", "pop_fps", "pop_shutter", "pop_iris", "pop_tools"};
    for (int i = 0; i < 6; i++) {
        connected_state(&m);
        m.popup = pops[i];
        if (pops[i] == POP_TOOLS) {
            m.set.guides = true;
            m.tool = TOOL_GUIDES;
        }
        render(&m, dir, pop_names[i]);
    }

    /* A camera that measures the shutter as a speed: 1/48 at 24 fps (180 degrees) */
    connected_state(&m);
    m.popup = POP_SHUTTER;
    m.cam.format_flags = 0;
    m.cam.file_fps = 24;
    m.cam.shutter_speed_mode = true;
    m.cam.shutter_speed = 48;
    render(&m, dir, "pop_shutter_speed");
    /* Auto exposure driving the shutter: Shutter + Iris */
    connected_state(&m);
    m.popup = POP_SHUTTER;
    m.cam.ae_mode = 4;
    render(&m, dir, "pop_shutter_auto");

    connected_state(&m);
    m.popup = POP_FPS;
    m.cam.format_flags = 0;
    m.cam.sensor_fps = 24;
    render(&m, dir, "pop_fps_normal");
    /* 2.8K goes up to 120 fps: all off-speed presets */
    connected_state(&m);
    m.popup = POP_FPS;
    m.cam.width = 2880;
    m.cam.height = 1512;
    m.cam.format_flags |= CAM_FMT_WINDOWED;
    m.cam.sensor_fps = 120;
    render(&m, dir, "pop_fps_120");
    /* Playback: 4x forwards, looping, the clip marked a good take */
    connected_state(&m);
    m.popup = POP_PLAYBACK;
    m.cam.transport_mode = 1;
    m.cam.transport_speed = 4;
    m.cam.transport_flags = 0x41;
    m.slate.have = true;
    m.slate.good_take = true;
    strcpy(m.slate.text[CAM_SLATE_TARGET], "UNTITLED/B002_1236_C004");
    render(&m, dir, "pop_playback");
    m.set.live_view = false;
    render(&m, dir, "remote_playback");
    connected_state(&m);
    m.popup = POP_FPS;
    m.cam.width = 2880;
    m.cam.height = 1512;
    m.cam.format_flags |= CAM_FMT_WINDOWED;
    m.cam.sensor_fps = 120;
    /* Halfway up after off-speed was switched on */
    m.fps_rows = 0.5f;
    render(&m, dir, "pop_fps_sliding");
    m.fps_rows = 0;

    connected_state(&m);
    m.page = PAGE_PIN;
    strcpy(m.pin, "482");
    m.pin_len = 3;
    render(&m, dir, "page_pin");

    connected_state(&m);
    m.page = PAGE_MENU;
    render(&m, dir, "page_menu_record1");
    m.cam.codec_variant = 8;
    render(&m, dir, "page_menu_record1_q3");
    m.cam.codec = CAM_CODEC_PRORES;
    m.cam.codec_variant = 2;
    m.cam.width = 4096;
    m.cam.height = 2160;
    render(&m, dir, "page_menu_record1_prores");
    m.cam.codec = CAM_CODEC_BRAW;
    m.cam.codec_variant = 4;
    /* 60 fps project: 6K (up to 50 fps) greyed out */
    m.cam.file_fps = 60;
    m.cam.format_flags = CAM_FMT_WINDOWED;
    render(&m, dir, "page_menu_record1_60p");
    m.cam.file_fps = 24;
    m.cam.format_flags = CAM_FMT_OFF_SPEED;
    m.cam.width = 6144;
    m.cam.height = 3456;
    m.menu_page = 1;
    render(&m, dir, "page_menu_record2");
    /* Video reported with the camera's LUT on: Film or Video */
    m.cam.dynamic_range = 1;
    render(&m, dir, "page_menu_record2_unsure");
    m.cam.dynamic_range = 0;
    m.menu_tab = 1;
    m.menu_page = 0;
    render(&m, dir, "page_menu_remote");

    connected_state(&m);
    m.cam.power_flags = 2;
    render(&m, dir, "power_ac");
    m.set.power_voltage = true;
    render(&m, dir, "power_voltage");
    m.set.m5bat_voltage = true;
    m.m5_pct = 12;
    render(&m, dir, "m5bat_voltage_low");

    connected_state(&m);
    some_presets(&m);
    m.preset_active = 1;
    m.cam.focus = 860;
    m.slate.reel = 3;
    m.slate.take = 12;
    m.slate.shot = 2;
    m.slate.take_tag = 0;
    strcpy(m.slate.text[CAM_SLATE_SCENE], "23A");
    strcpy(m.slate.text[CAM_SLATE_PROJECT], "Interview Series");
    strcpy(m.slate.text[CAM_SLATE_DIRECTOR], "Kim Lee");
    strcpy(m.slate.text[CAM_SLATE_OPERATOR], "Alex Brandt-Whitmore");
    render(&m, dir, "main_preset");
    m.slate_quick = true;
    render(&m, dir, "main_slate_quick");
    m.slate_quick = false;
    m.set.simple_view = true;
    render(&m, dir, "main_simple");
    m.set.simple_view = false;
    m.cam.tools = CAM_TOOL_ZEBRA | CAM_TOOL_FALSE_COLOR;
    m.cam.have_overlays = true;
    m.cam.safe_area = 90;
    m.cam.grid = 3;
    strcpy(m.toast, "Tap CLEAR again to erase Interview A-Cam");
    render(&m, dir, "main_mon_toggles");
    m.cam.overlay_bits = CAM_OVL_CLEAN_FEED;
    m.toast[0] = 0;
    render(&m, dir, "main_mon_clean");
    m.cam.tools = 0;
    m.cam.overlay_bits = 0;
    /* Focus drag: only the slider and the percentage are redrawn */
    m.cam.focus = 1700;
    ui_draw_focus(&m);
    overlay_commit();
    write_frame(dir, "focus_after_drag");
    render(&m, dir, "focus_full_redraw");
    m.preset_active = 6;
    m.cam.focus = 2048;
    render(&m, dir, "main_preset_long");
    m.preset_active = 2;
    m.cam.focus = 0;
    render(&m, dir, "main_preset_default");
    m.page = PAGE_PRESETS;
    render(&m, dir, "page_presets");
    m.preset_page = 1;
    m.preset_armed = 6;
    strcpy(m.toast, "Tap CLEAR again to erase WWWWWWWWWWWWWWWW");
    render(&m, dir, "page_presets_armed");
    m.preset_page = 0;
    m.preset_armed = UI_PRESET_ARMED_SD;
    strcpy(m.toast, "Tap again to replace all presets from SD");
    render(&m, dir, "page_presets_sd_armed");
    m.preset_armed = -1;

    m.page = PAGE_KEYBOARD;
    m.kb_slot = 1;
    strcpy(m.kb_text, "Interview A-Cam");
    m.kb_len = (uint8_t)strlen(m.kb_text);
    m.kb_shift = 0;
    render(&m, dir, "page_keyboard");
    m.kb_slot = 3;
    m.kb_text[0] = 0;
    m.kb_len = 0;
    m.kb_shift = 1;
    render(&m, dir, "page_keyboard_empty");

    connected_state(&m);
    m.page = PAGE_SLATE;
    m.slate.shot = 2;
    m.slate.good_take = true;
    m.slate.take = 12;
    m.slate.take_tag = 0;
    strcpy(m.slate.text[CAM_SLATE_SCENE], "23A");
    render(&m, dir, "page_slate_clip");
    m.slate_tab = 1;
    strcpy(m.slate.text[CAM_SLATE_PROJECT], "Interview Series");
    strcpy(m.slate.text[CAM_SLATE_DIRECTOR], "Kim Lee");
    render(&m, dir, "page_slate_project");
    m.slate_tab = 2;
    render(&m, dir, "page_slate_lens");
    m.page = PAGE_KEYBOARD;
    m.kb_slot = -1;
    m.kb_param = CAM_SLATE_LENS_TYPE;
    m.kb_max = (uint8_t)ui_slate_text_max(CAM_SLATE_LENS_TYPE);
    strcpy(m.kb_text, "Zeiss CP.3 50mm T2.1 with a very long description");
    m.kb_len = (uint8_t)strlen(m.kb_text);
    render(&m, dir, "page_keyboard_slate");
    connected_state(&m);
    m.page = PAGE_SLATE;
    m.cam.link = CAM_LINK_SCANNING;
    render(&m, dir, "page_slate_disconnected");

    connected_state(&m);
    m.page = PAGE_PRESETS;
    m.cam.link = CAM_LINK_SCANNING;
    strcpy(m.cam.status, "Searching for camera");
    m.m5_mv = 0;
    render(&m, dir, "page_presets_empty");

    /* Auto exposure driving the iris only: its modes instead of the slider */
    connected_state(&m);
    m.popup = POP_IRIS;
    m.cam.ae_mode = 1;
    render(&m, dir, "pop_iris_auto");

    /* Without live view: bigger controls, the focus slider on its own row */
    connected_state(&m);
    m.set.live_view = false;
    m.set.simple_view = false;
    m.cam.focus = 1300;
    m.cam.overlay_bits = 0;
    m.cam.tools = CAM_TOOL_ZEBRA;
    m.cam.have_overlays = true;
    m.cam.grid = 1;
    m.slate.have = true;
    m.slate.reel = 3;
    m.slate.take = 12;
    m.slate.interior = 1;
    m.slate.day = 1;
    m.slate.shot = -1;
    m.slate.take_tag = -1;
    strcpy(m.slate.text[CAM_SLATE_SCENE], "23A");
    strcpy(m.slate.text[CAM_SLATE_PROJECT], "Interview Series");
    strcpy(m.slate.text[CAM_SLATE_DIRECTOR], "Kim Lee");
    m.preset_active = 1;
    render(&m, dir, "remote_main");
    m.slate_quick = true;
    render(&m, dir, "remote_slate_quick");
    m.slate_quick = false;
    m.set.simple_view = true;
    render(&m, dir, "remote_simple");
    m.set.simple_view = false;
    m.popup = POP_WB;
    render(&m, dir, "remote_wb");
    m.popup = POP_SHUTTER;
    render(&m, dir, "remote_shutter");
    m.popup = POP_FPS;
    render(&m, dir, "remote_fps");
    m.popup = POP_IRIS;
    m.cam.ae_mode = 3;
    render(&m, dir, "remote_iris_auto");
    return 0;
}
