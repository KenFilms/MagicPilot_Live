/*
 * Blackmagic camera state and control commands. See cam.h.
 *
 * Control packet layout (Blackmagic Camera Control protocol): destination (0xFF), payload length,
 * command (0 = change configuration), reserved, then category, parameter, data type, operation and
 * the value, padded to a multiple of 4 bytes. Several packets may arrive in one notification.
 * Packet decoding and the timecode filtering follow MagicPilot_Remote (ble_protocol.h).
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cam.h"
#include "cam_ble.h"
#include "esp_timer.h"

static const char *TAG = "cam";

enum {
    T_VOID = 0,
    T_INT8 = 1,
    T_INT16 = 2,
    T_INT32 = 3,
    T_STRING = 5,
    T_FIXED16 = 128,
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static cam_state_t s_st;
static cam_slate_t s_slate;
static uint32_t s_changes;
static void (*s_change_cb)(void);

/*
 * Timecode and clip counter. The camera sends the one it shows (4.7): the running timecode, or its
 * clip counter while recording and once when it starts showing it. After connecting, that packet
 * comes just before the report of which one it shows.
 */
static uint32_t s_pending_tc = UINT32_MAX;     /* candidate frame count after a jump */
static uint8_t s_tc_raw[4];
static bool s_have_raw_tc;
static uint8_t s_clip[4];                       /* the camera's clip counter */
static int8_t s_shown = -1;                     /* what the camera reported it shows: 0 timecode, 1 clip; -1 = not yet */
static uint8_t s_early[4];                      /* a packet from before that report */
static bool s_have_early;

/*
 * Settings the BMPCC6K may refuse over Bluetooth: BRAW <-> ProRes, ProRes resolutions, Film. It answers
 * every change it takes, so a write without an answer, or with another value, was refused: the state
 * goes back to what it last reported.
 */
#define CONFIRM_US      2000000
static int8_t s_codec_cam[2] = {-1, -1};        /* as last reported */
static int16_t s_fmt_cam[5];
static bool s_have_fmt_cam;
static int8_t s_range_cam = -1, s_range_sent;
static int64_t s_codec_wait, s_fmt_wait, s_range_wait;   /* deadline of an answer, 0 = none awaited */
static uint8_t s_refused;

/* Raises change flags for cam_take_changes() (call with s_mux held) */
static void changed(uint32_t bits)
{
    s_changes |= bits;
}

/* Tells the UI through the change callback (call without s_mux held) */
static void notify(void)
{
    if (s_change_cb) {
        s_change_cb();
    }
}

void cam_init(void)
{
    taskENTER_CRITICAL(&s_mux);
    memset(&s_st, 0, sizeof(s_st));
    s_st.link = CAM_LINK_DOWN;
    snprintf(s_st.status, sizeof(s_st.status), "Starting Bluetooth");
    taskEXIT_CRITICAL(&s_mux);
    cam_reset_values();
}

void cam_get(cam_state_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_st;
    taskEXIT_CRITICAL(&s_mux);
}

void cam_get_slate(cam_slate_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_slate;
    taskEXIT_CRITICAL(&s_mux);
}

uint32_t cam_take_changes(void)
{
    taskENTER_CRITICAL(&s_mux);
    const uint32_t c = s_changes;
    s_changes = 0;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

void cam_set_change_callback(void (*cb)(void))
{
    s_change_cb = cb;
}

void cam_set_link(cam_link_t link, const char *status)
{
    taskENTER_CRITICAL(&s_mux);
    s_st.link = link;
    snprintf(s_st.status, sizeof(s_st.status), "%s", status ? status : "");
    changed(CAM_CH_LINK);
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Link: %s", status ? status : "");
    notify();
}

void cam_set_name(const char *name)
{
    taskENTER_CRITICAL(&s_mux);
    snprintf(s_st.name, sizeof(s_st.name), "%s", name ? name : "");
    changed(CAM_CH_LINK);
    taskEXIT_CRITICAL(&s_mux);
    notify();
}

void cam_reset_values(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_st.iso = 0;
    s_st.shutter_angle = 0;
    s_st.shutter_speed = 0;
    s_st.shutter_speed_mode = false;
    s_st.have_format = false;
    s_st.have_wb = false;
    s_st.have_iris = false;
    s_st.have_iris_norm = false;
    s_st.focus = CAM_FOCUS_MAX / 2;
    s_st.ae_mode = -1;
    s_st.dynamic_range = -1;
    s_st.sharpening = -1;
    s_st.codec = s_st.codec_variant = -1;
    s_st.lut = 0;
    s_st.lut_enabled = false;
    s_st.tools = 0;
    s_st.overlay_bits = CAM_OVL_STATUS;
    s_st.have_overlays = false;
    s_st.transport_mode = 0;
    s_st.transport_flags = 0;
    s_st.slot_medium[0] = s_st.slot_medium[1] = -1;
    s_st.slot_remaining[0] = s_st.slot_remaining[1] = s_st.slot_remaining[2] = -1;
    s_st.voltage_mv = 0;
    s_st.power_flags = 0;
    s_st.have_tc = false;
    s_st.tc_source = 0;
    memset(&s_slate, 0, sizeof(s_slate));
    changed(CAM_CH_VALUES | CAM_CH_TIMECODE | CAM_CH_TRANSPORT | CAM_CH_MEDIA | CAM_CH_POWER | CAM_CH_FORMAT |
            CAM_CH_FOCUS | CAM_CH_SLATE);
    s_have_raw_tc = false;
    s_pending_tc = UINT32_MAX;
    memset(s_clip, 0, sizeof(s_clip));
    s_shown = -1;
    s_have_early = false;
    s_codec_cam[0] = s_codec_cam[1] = -1;
    s_have_fmt_cam = false;
    s_range_cam = -1;
    s_codec_wait = s_fmt_wait = s_range_wait = 0;
    s_refused = 0;
    taskEXIT_CRITICAL(&s_mux);
    notify();
}

/* ---- Incoming packets ---- */

/* The recording format (file fps, sensor fps, width, height, flags) as the state's (call with s_mux held) */
static void format_to_state(const int16_t f[5])
{
    s_st.file_fps = f[0];
    s_st.sensor_fps = f[1];
    s_st.width = f[2];
    s_st.height = f[3];
    s_st.format_flags = f[4];
}

/* Little-endian 32-bit value at d */
static int32_t rd32(const uint8_t *d)
{
    return (int32_t)((uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24));
}

/* Little-endian 16-bit value at d */
static int16_t rd16(const uint8_t *d)
{
    return (int16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
}

/* v holds two BCD digits whose value is below limit */
static bool valid_bcd(uint8_t v, uint8_t limit)
{
    const uint8_t hi = v >> 4, lo = v & 0x0F;
    return hi <= 9 && lo <= 9 && hi * 10 + lo < limit;
}

/* Value of two BCD digits */
static uint32_t bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0F);
}

/* Frame count of a BCD timecode (frames, seconds, minutes, hours) at fps frames per second */
static uint32_t tc_frames(const uint8_t *b, uint32_t fps)
{
    return ((bcd(b[3]) * 60 + bcd(b[2])) * 60 + bcd(b[1])) * fps + bcd(b[0]);
}

/* What the timecode display shows: the timecode, or the camera's clip counter */
static void update_tc_display_locked(void)
{
    if (s_st.tc_source == 0) {
        if (!s_have_raw_tc) {
            return;
        }
        memcpy(s_st.tc, s_tc_raw, 4);
    } else {
        memcpy(s_st.tc, s_clip, 4);
    }
    s_st.have_tc = true;
    changed(CAM_CH_TIMECODE);
}

/* Stores a timecode or clip counter packet; invalid values and one-off timecode jumps are rejected */
static void set_timecode(const uint8_t *b)
{
    if (!valid_bcd(b[0], 60) || !valid_bcd(b[1], 60) || !valid_bcd(b[2], 60) || !valid_bcd(b[3], 24)) {
        return;
    }
    if (s_shown < 0 && !s_have_early) {
        /* Which one it is follows in the camera's report (4.7) */
        memcpy(s_early, b, 4);
        s_have_early = true;
        return;
    }
    /* Without that report, by what the Tab5 shows */
    if (s_shown < 0 ? s_st.tc_source == 1 : s_shown == 1) {
        memcpy(s_clip, b, 4);
        update_tc_display_locked();
        return;
    }
    /* The timecode counts at the project rate; 30 until that is known */
    const uint32_t fps = s_st.have_format && s_st.file_fps > 0 && s_st.file_fps <= 60 ? (uint32_t)s_st.file_fps : 30;
    const uint32_t count = tc_frames(b, fps);
    if (s_have_raw_tc) {
        const uint32_t prev = tc_frames(s_tc_raw, fps);
        /* A jump of more than 3 s only counts once a packet a few frames later confirms it */
        if (count > prev + fps * 3 || prev > count + fps * 3) {
            if (s_pending_tc == UINT32_MAX || count <= s_pending_tc || count > s_pending_tc + 4) {
                s_pending_tc = count;
                return;
            }
        }
        s_pending_tc = UINT32_MAX;
    }
    memcpy(s_tc_raw, b, 4);
    s_have_raw_tc = true;
    update_tc_display_locked();
}

/* Slate: the BMPCC6K reports all of it after connecting and every change */
static void handle_slate(uint8_t param, uint8_t type, const uint8_t *d, size_t n)
{
    cam_slate_t *s = &s_slate;
    if (type == T_STRING) {
        if (param >= CAM_SLATE_PARAMS) {
            return;
        }
        /* A NUL ends the text: a single NUL byte is how a field is cleared */
        const size_t len = n < CAM_SLATE_TEXT - 1 ? n : CAM_SLATE_TEXT - 1;
        memcpy(s->text[param], d, len);
        s->text[param][len] = 0;
    } else {
        switch (param) {
        case CAM_SLATE_REEL:
            if (n < 2) {
                return;
            }
            s->reel = rd16(d);
            break;
        case CAM_SLATE_TAGS:
            if (n < 3) {
                return;
            }
            s->shot = (int8_t)d[0];
            s->interior = (int8_t)d[1];
            s->day = (int8_t)d[2];
            break;
        case CAM_SLATE_TAKE:
            if (n < 2) {
                return;
            }
            s->take = (int8_t)d[0];
            s->take_tag = (int8_t)d[1];
            break;
        case CAM_SLATE_GOOD_TAKE:
            if (n < 1) {
                return;
            }
            s->good_take = d[0] != 0;
            break;
        case CAM_SLATE_MODE:
            if (n < 1) {
                return;
            }
            s->mode = (int8_t)d[0];
            break;
        default:
            return;
        }
    }
    s->have = true;
    changed(CAM_CH_SLATE);
}

/* One packet (n bytes of data at d) into the state; anything unknown or too short is ignored (call with s_mux held) */
static void handle_packet(uint8_t cat, uint8_t param, uint8_t type, const uint8_t *d, size_t n)
{
    if (cat == 12) {
        handle_slate(param, type, d, n);
        return;
    }
    switch ((cat << 8) | param) {
    case (1 << 8) | 14:     /* ISO */
        if (type == T_INT32 && n >= 4) {
            s_st.iso = rd32(d);
            changed(CAM_CH_VALUES);
        }
        break;
    case (1 << 8) | 11:     /* shutter angle x 100: the camera measures the shutter as an angle */
        if (type == T_INT32 && n >= 4) {
            s_st.shutter_angle = rd32(d);
            s_st.shutter_speed_mode = false;
            changed(CAM_CH_VALUES);
        }
        break;
    case (1 << 8) | 12:     /* shutter speed 1/x: the camera measures the shutter as a speed */
        if (type == T_INT32 && n >= 4) {
            s_st.shutter_speed = rd32(d);
            s_st.shutter_speed_mode = true;
            changed(CAM_CH_VALUES);
        }
        break;
    case (1 << 8) | 2:      /* white balance, tint */
        if (type == T_INT16 && n >= 4) {
            s_st.wb_kelvin = rd16(d);
            s_st.wb_tint = rd16(d + 2);
            s_st.have_wb = true;
            changed(CAM_CH_VALUES);
        }
        break;
    case (1 << 8) | 9:      /* recording format */
        if (type == T_INT16 && n >= 10) {
            for (int i = 0; i < 5; i++) {
                s_fmt_cam[i] = rd16(d + 2 * i);
            }
            s_have_fmt_cam = true;
            format_to_state(s_fmt_cam);
            s_st.have_format = true;
            s_fmt_wait = 0;
            changed(CAM_CH_VALUES | CAM_CH_FORMAT);
        }
        break;
    case (1 << 8) | 10:     /* auto exposure mode */
        if (n >= 1) {
            s_st.ae_mode = (int8_t)d[0];
            changed(CAM_CH_VALUES);
        }
        break;
    case (1 << 8) | 7:      /* dynamic range */
        if (n >= 1) {
            s_st.dynamic_range = (int8_t)d[0];
            s_range_cam = (int8_t)d[0];
            if (s_range_wait && s_range_cam != s_range_sent) {
                s_refused |= CAM_REFUSED_RANGE;     /* it took another one */
            }
            s_range_wait = 0;
            changed(CAM_CH_FORMAT);
        }
        break;
    case (1 << 8) | 8:      /* sharpening */
        if (n >= 1) {
            s_st.sharpening = (int8_t)d[0];
            changed(CAM_CH_FORMAT);
        }
        break;
    case (1 << 8) | 15:     /* display LUT */
        if (n >= 2) {
            s_st.lut = (int8_t)d[0];
            s_st.lut_enabled = d[1] != 0;
            changed(CAM_CH_FORMAT);
        }
        break;
    case (3 << 8) | 0:      /* overlay enables */
        if (n >= 2) {
            s_st.overlay_bits = (uint8_t)(rd16(d) & 7);
            changed(CAM_CH_FORMAT);
        }
        break;
    case (3 << 8) | 3:      /* frame guides, their opacity, safe area, grid */
        if (n >= 4) {
            s_st.guide_style = (int8_t)d[0];
            s_st.guide_opacity = (int8_t)d[1];
            s_st.safe_area = (int8_t)d[2];
            s_st.grid = (int8_t)d[3];
            s_st.have_overlays = true;
            changed(CAM_CH_FORMAT);
        }
        break;
    case (4 << 8) | 1:      /* exposure and focus tools */
        if (type == T_INT16 && n >= 2) {
            s_st.tools = (uint8_t)(rd16(d) & 7);
            changed(CAM_CH_FORMAT);
        }
        break;
    case (0 << 8) | 0:      /* focus */
        if (type == T_FIXED16 && n >= 2) {
            const int16_t f = rd16(d);
            s_st.focus = f < 0 ? 0 : (f > CAM_FOCUS_MAX ? CAM_FOCUS_MAX : f);
            changed(CAM_CH_FOCUS);
        }
        break;
    case (0 << 8) | 2:      /* aperture value */
        if (type == T_FIXED16 && n >= 2) {
            s_st.iris_av = rd16(d);
            s_st.have_iris = true;
            changed(CAM_CH_VALUES);
        }
        break;
    case (0 << 8) | 3:      /* aperture, normalised */
        if (type == T_FIXED16 && n >= 2) {
            s_st.iris_norm = rd16(d);
            s_st.have_iris_norm = true;
            changed(CAM_CH_VALUES);
        }
        break;
    case (10 << 8) | 0:     /* codec */
        if (n >= 2) {
            s_st.codec = (int8_t)d[0];
            s_st.codec_variant = (int8_t)d[1];
            s_codec_cam[0] = (int8_t)d[0];
            s_codec_cam[1] = (int8_t)d[1];
            s_codec_wait = 0;
            changed(CAM_CH_FORMAT);
        }
        break;
    case (10 << 8) | 1: {   /* transport mode */
        if (n < 1) {
            break;
        }
        const uint8_t prev = s_st.transport_mode;
        s_st.transport_mode = d[0];
        if (n >= 2) {
            s_st.transport_speed = (int8_t)d[1];
        }
        /* A new recording counts from zero */
        if (s_st.transport_mode == 2 && prev != 2) {
            memset(s_clip, 0, sizeof(s_clip));
        }
        if (n >= 5) {
            s_st.transport_flags = d[2];
            s_st.slot_medium[0] = (int8_t)d[3];
            s_st.slot_medium[1] = (int8_t)d[4];
        }
        update_tc_display_locked();
        changed(CAM_CH_TRANSPORT | CAM_CH_TIMECODE);
        break;
    }
    case (9 << 8) | 0:      /* battery: voltage (mV), level, power flags */
        if (type == T_INT16 && n >= 2) {
            s_st.voltage_mv = (uint16_t)rd16(d);
            s_st.power_flags = n >= 6 ? (uint16_t)rd16(d + 4) : 0;
            changed(CAM_CH_POWER);
        }
        break;
    case (9 << 8) | 2:      /* remaining record time per slot */
        if (type == T_INT16) {
            /* Slot 1, slot 2 and USB; the fourth value's meaning is unknown */
            for (size_t i = 0; i < 3 && (i + 1) * 2 <= n; i++) {
                s_st.slot_remaining[i] = rd16(d + i * 2);
            }
            changed(CAM_CH_MEDIA);
        }
        break;
    case (4 << 8) | 7:      /* timecode source */
        if (n >= 1) {
            s_st.tc_source = d[0];
            const bool first = s_shown < 0;
            s_shown = (int8_t)d[0];
            if (first && s_have_early) {
                /* The packet from before this report is what the camera shows */
                s_have_early = false;
                set_timecode(s_early);
            }
            update_tc_display_locked();
            changed(CAM_CH_TIMECODE);
        }
        break;
    case (9 << 8) | 4:      /* timecode */
        if (n >= 4) {
            set_timecode(d);
        }
        break;
    default:
        break;
    }
}

/* A packet (FF, length, command 0) starts at off */
static bool starts_packet(const uint8_t *b, size_t len, size_t off)
{
    return off + 4 <= len && b[off] == 0xFF && b[off + 2] == 0;
}

void cam_handle_packets(const uint8_t *bytes, size_t length)
{
    taskENTER_CRITICAL(&s_mux);
    size_t off = 0;
    while (off + 4 <= length) {
        const uint8_t *p = bytes + off;
        const uint8_t payload = p[1];
        const size_t raw = 4 + payload;
        const size_t padded = 4 + ((payload + 3) & ~(size_t)3);
        const size_t remaining = length - off;
        /* Truncated, or a command other than 0: the next packet cannot be found, so stop */
        if (raw > remaining || p[2] != 0) {
            break;
        }
        /* The camera may or may not pad packets to 4 bytes: decide by where the next one starts */
        size_t plen = raw;
        if (padded > raw && padded <= remaining) {
            const bool raw_next = starts_packet(bytes, length, off + raw);
            const bool padded_next = starts_packet(bytes, length, off + padded);
            if ((padded_next && !raw_next) || remaining == padded) {
                plen = padded;
            }
        }
        /* Category, parameter, type and operation (2 in every report, so not read), then the data */
        if (payload >= 4) {
            handle_packet(p[4], p[5], p[6], p + 8, payload - 4);
        }
        off += plen;
    }
    const bool any = s_changes != 0;
    taskEXIT_CRITICAL(&s_mux);
    if (any) {
        notify();
    }
}

/* ---- Commands ---- */

/*
 * Builds a control packet and queues it. coalesce: a newer packet replaces a queued one of the same
 * parameter, or for offsets (op 1) adds to it.
 */
static void send(uint8_t cat, uint8_t param, uint8_t type, uint8_t op, const void *value, size_t n, bool coalesce)
{
    uint8_t p[64] = { 0xFF, (uint8_t)(4 + n), 0, 0, cat, param, type, op };
    if (n > sizeof(p) - 8) {
        return;
    }
    memcpy(p + 8, value, n);
    const size_t len = (8 + n + 3) & ~(size_t)3;
    uint16_t key = 0;
    if (coalesce) {
        key = (uint16_t)((cat << 8) | param | 0x8000 | (op == 1 ? CAM_BLE_KEY_SUM : 0));
    }
    cam_ble_send(p, len, key);
}

/* Stores v at d, little-endian */
static void put16(uint8_t *d, int16_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)((uint16_t)v >> 8);
}

/* Stores v at d, little-endian */
static void put32(uint8_t *d, int32_t v)
{
    for (int i = 0; i < 4; i++) {
        d[i] = (uint8_t)((uint32_t)v >> (8 * i));
    }
}

/* Raises change flags and tells the UI (call without s_mux held) */
static void update(uint32_t bits)
{
    taskENTER_CRITICAL(&s_mux);
    changed(bits);
    taskEXIT_CRITICAL(&s_mux);
    notify();
}

/* A 32-bit value of category 1 (ISO, shutter): sent, and taken as the camera's */
static void set32(uint8_t param, int32_t value, int32_t *field)
{
    uint8_t v[4];
    put32(v, value);
    send(1, param, T_INT32, 0, v, 4, true);
    taskENTER_CRITICAL(&s_mux);
    *field = value;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_VALUES);
}

/* An 8-bit value of category 1: sent, and taken as the camera's */
static void set8(uint8_t param, int8_t value, int8_t *field, uint32_t bits)
{
    send(1, param, T_INT8, 0, &value, 1, true);
    taskENTER_CRITICAL(&s_mux);
    *field = value;
    taskEXIT_CRITICAL(&s_mux);
    update(bits);
}

void cam_set_iso(int32_t iso)
{
    set32(14, iso, &s_st.iso);
}

void cam_set_shutter_angle(int32_t angle100)
{
    set32(11, angle100, &s_st.shutter_angle);
}

void cam_set_shutter_speed(int32_t speed)
{
    set32(12, speed, &s_st.shutter_speed);
}

/*
 * The recording format carries everything at once; the fields not being changed are sent back as reported.
 * The sensor rate is sent off-speed only (else it is the project rate). An answer is awaited unless the
 * format is the camera's already.
 */
static void set_format(int16_t fps, int16_t sensor, int16_t w, int16_t h, int16_t flags, uint32_t bits)
{
    const bool off = flags & CAM_FMT_OFF_SPEED;
    const int16_t f[5] = {fps, sensor, w, h, flags};
    uint8_t v[10];
    for (int i = 0; i < 5; i++) {
        put16(v + 2 * i, i == 1 && !off ? 0 : f[i]);
    }
    send(1, 9, T_INT16, 0, v, 10, true);
    taskENTER_CRITICAL(&s_mux);
    const bool same = s_have_fmt_cam && fps == s_fmt_cam[0] && w == s_fmt_cam[2] && h == s_fmt_cam[3] &&
                      flags == s_fmt_cam[4] && (!off || sensor == s_fmt_cam[1]);
    s_fmt_wait = same ? 0 : esp_timer_get_time() + CONFIRM_US;
    format_to_state(f);
    taskEXIT_CRITICAL(&s_mux);
    update(bits);
}

void cam_set_project_fps(int16_t fps, bool mrate)
{
    cam_state_t s;
    cam_get(&s);
    if (!s.have_format) {
        return;
    }
    const bool off_speed = s.format_flags & CAM_FMT_OFF_SPEED;
    /* The sensor runs at the project rate unless off-speed, with its M-rate flag */
    int16_t flags = (int16_t)(s.format_flags & ~CAM_FMT_FILE_MRATE);
    if (mrate) {
        flags |= CAM_FMT_FILE_MRATE;
    }
    if (!off_speed) {
        flags = (int16_t)((flags & ~CAM_FMT_SENSOR_MRATE) | (mrate ? CAM_FMT_SENSOR_MRATE : 0));
    }
    set_format(fps, off_speed ? s.sensor_fps : fps, s.width, s.height, flags, CAM_CH_VALUES);
}

void cam_set_off_speed(bool on, int16_t sensor_fps)
{
    cam_state_t s;
    cam_get(&s);
    if (!s.have_format) {
        return;
    }
    int16_t flags = on ? (s.format_flags | CAM_FMT_OFF_SPEED) : (s.format_flags & ~CAM_FMT_OFF_SPEED);
    if (!on) {
        /* The sensor at the project rate again, so with its M-rate flag too (as cam_set_project_fps has it) */
        flags = (int16_t)((flags & ~CAM_FMT_SENSOR_MRATE) | ((flags & CAM_FMT_FILE_MRATE) ? CAM_FMT_SENSOR_MRATE : 0));
    }
    set_format(s.file_fps, on ? sensor_fps : s.file_fps, s.width, s.height, flags, CAM_CH_VALUES);
}

void cam_set_format(int16_t project_fps, int16_t sensor_fps, int16_t rate_flags, int16_t width, int16_t height,
                    bool windowed)
{
    cam_state_t s;
    cam_get(&s);
    if (!s.have_format) {
        return;
    }
    int16_t flags = (int16_t)((s.format_flags & ~CAM_FMT_RATE_FLAGS) | (rate_flags & CAM_FMT_RATE_FLAGS));
    uint32_t bits = CAM_CH_VALUES;
    if (width > 0) {
        flags = (int16_t)(windowed ? flags | CAM_FMT_WINDOWED : flags & ~CAM_FMT_WINDOWED);
        bits |= CAM_CH_FORMAT;
    } else {
        width = s.width;
        height = s.height;
    }
    set_format(project_fps, (flags & CAM_FMT_OFF_SPEED) ? sensor_fps : project_fps, width, height, flags, bits);
}

void cam_set_frame_rates(int16_t project_fps, int16_t sensor_fps, int16_t rate_flags)
{
    cam_set_format(project_fps, sensor_fps, rate_flags, 0, 0, false);
}

void cam_set_resolution(int16_t width, int16_t height, bool windowed)
{
    cam_state_t s;
    cam_get(&s);
    if (!s.have_format) {
        return;
    }
    const int16_t flags = windowed ? (s.format_flags | CAM_FMT_WINDOWED) : (s.format_flags & ~CAM_FMT_WINDOWED);
    set_format(s.file_fps, s.sensor_fps, width, height, flags, CAM_CH_FORMAT);
}

void cam_set_wb(int16_t kelvin, int16_t tint)
{
    uint8_t v[4];
    put16(v, kelvin);
    put16(v + 2, tint);
    send(1, 2, T_INT16, 0, v, 4, true);
    taskENTER_CRITICAL(&s_mux);
    s_st.wb_kelvin = kelvin;
    s_st.wb_tint = tint;
    s_st.have_wb = true;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_VALUES);
}

void cam_auto_wb(void)
{
    send(1, 3, T_VOID, 0, NULL, 0, false);
}

void cam_iris_step(int steps)
{
    uint8_t v[2];
    put16(v, (int16_t)steps);
    send(0, 4, T_INT16, 1, v, 2, false);
}

void cam_set_slate_text(uint8_t param, const char *text)
{
    if (param >= CAM_SLATE_PARAMS) {
        return;
    }
    size_t n = strlen(text);
    n = n < CAM_SLATE_SEND_MAX ? n : CAM_SLATE_SEND_MAX;
    /* The BMPCC6K ignores an empty text; a single NUL byte clears the field */
    const uint8_t nul = 0;
    send(12, param, T_STRING, 0, n ? (const void *)text : (const void *)&nul, n ? n : 1, true);
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_slate.text[param], text, n);
    s_slate.text[param][n] = 0;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_SLATE);
}

void cam_set_slate_tags(int8_t shot, int8_t interior, int8_t day)
{
    const int8_t v[3] = {shot, interior, day};
    send(12, CAM_SLATE_TAGS, T_INT8, 0, v, 3, true);
    taskENTER_CRITICAL(&s_mux);
    s_slate.shot = shot;
    s_slate.interior = interior;
    s_slate.day = day;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_SLATE);
}

void cam_set_slate_take(int8_t take, int8_t tag)
{
    const int8_t v[2] = {take, tag};
    send(12, CAM_SLATE_TAKE, T_INT8, 0, v, 2, true);
    taskENTER_CRITICAL(&s_mux);
    s_slate.take = take;
    s_slate.take_tag = tag;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_SLATE);
}

void cam_set_slate_good_take(bool good)
{
    const uint8_t v = good;
    send(12, CAM_SLATE_GOOD_TAKE, T_VOID, 0, &v, 1, true);
    taskENTER_CRITICAL(&s_mux);
    s_slate.good_take = good;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_SLATE);
}

/* A bit field (three bits), then the displays it applies to: sent, and taken as the camera's */
static void set_enables(uint8_t cat, uint8_t param, uint8_t bits, uint8_t displays, uint8_t *field)
{
    uint8_t v[4];
    put16(v, bits & 7);
    put16(v + 2, displays);
    send(cat, param, T_INT16, 0, v, 4, true);
    taskENTER_CRITICAL(&s_mux);
    *field = bits & 7;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_FORMAT);
}

void cam_set_tools(uint8_t tools, uint8_t displays)
{
    set_enables(4, 1, tools, displays, &s_st.tools);
}

void cam_set_overlay_enables(uint8_t bits, uint8_t displays)
{
    set_enables(3, 0, bits, displays, &s_st.overlay_bits);
}

void cam_set_overlays(int8_t guide_style, int8_t guide_opacity, int8_t safe_area, int8_t grid)
{
    const int8_t v[4] = {guide_style, guide_opacity, safe_area, grid};
    send(3, 3, T_INT8, 0, v, 4, true);
    taskENTER_CRITICAL(&s_mux);
    s_st.guide_style = guide_style;
    s_st.guide_opacity = guide_opacity;
    s_st.safe_area = safe_area;
    s_st.grid = grid;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_FORMAT);
}

void cam_set_focus(int16_t focus)
{
    focus = focus < 0 ? 0 : (focus > CAM_FOCUS_MAX ? CAM_FOCUS_MAX : focus);
    taskENTER_CRITICAL(&s_mux);
    const int16_t delta = (int16_t)(focus - s_st.focus);
    s_st.focus = focus;
    taskEXIT_CRITICAL(&s_mux);
    if (delta == 0) {
        return;
    }
    /* The BMPCC6K does not report where the lens is, so the lens moves by the slider's move */
    uint8_t v[2];
    put16(v, delta);
    send(0, 0, T_FIXED16, 1, v, 2, true);
    update(CAM_CH_FOCUS);
}

void cam_autofocus(void)
{
    const uint8_t none = 0;
    send(0, 1, T_VOID, 0, &none, 0, false);
    /* The camera only acknowledges the autofocus: the lens's new position becomes the slider's 50 % */
    taskENTER_CRITICAL(&s_mux);
    s_st.focus = CAM_FOCUS_MAX / 2;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_FOCUS);
}

void cam_set_iris_norm(int16_t norm)
{
    uint8_t v[2];
    put16(v, norm);
    send(0, 3, T_FIXED16, 0, v, 2, true);
    taskENTER_CRITICAL(&s_mux);
    s_st.iris_norm = norm;
    s_st.have_iris_norm = true;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_VALUES);
}

void cam_set_ae_mode(int8_t mode)
{
    set8(10, mode, &s_st.ae_mode, CAM_CH_VALUES);
}

void cam_record(bool on)
{
    const uint8_t mode = on ? 2 : 0;
    send(10, 1, T_INT8, 0, &mode, 1, false);
}

void cam_play(int8_t speed)
{
    const int8_t v[2] = { 1, speed };
    send(10, 1, T_INT8, 0, v, 2, false);
    taskENTER_CRITICAL(&s_mux);
    s_st.transport_mode = 1;
    s_st.transport_speed = speed;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_TRANSPORT);
}

void cam_play_loop(bool loop)
{
    cam_state_t s;
    cam_get(&s);
    /* With the card flags as reported, so that the active card stays */
    const int8_t v[3] = { 1, s.transport_speed, (int8_t)((s.transport_flags & ~1) | (loop ? 1 : 0)) };
    send(10, 1, T_INT8, 0, v, 3, false);
    taskENTER_CRITICAL(&s_mux);
    s_st.transport_flags = (uint8_t)v[2];
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_TRANSPORT);
}

void cam_clip_step(bool next)
{
    const int8_t v = next ? 1 : 0;
    send(10, 2, T_INT8, 0, &v, 1, false);
}

void cam_live(void)
{
    const uint8_t mode = 0;
    send(10, 1, T_INT8, 0, &mode, 1, false);
    taskENTER_CRITICAL(&s_mux);
    s_st.transport_mode = 0;
    s_st.transport_speed = 0;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_TRANSPORT);
}

void cam_set_tc_source(uint8_t source)
{
    send(4, 7, T_INT8, 0, &source, 1, true);
    taskENTER_CRITICAL(&s_mux);
    s_st.tc_source = source;
    update_tc_display_locked();
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_TIMECODE);
}

void cam_set_codec(int8_t codec, int8_t variant)
{
    const int8_t v[2] = { codec, variant };
    send(10, 0, T_INT8, 0, v, 2, true);
    taskENTER_CRITICAL(&s_mux);
    s_codec_wait = codec == s_codec_cam[0] && variant == s_codec_cam[1] ? 0 : esp_timer_get_time() + CONFIRM_US;
    s_st.codec = codec;
    s_st.codec_variant = variant;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_FORMAT);
}

void cam_set_dynamic_range(int8_t mode)
{
    send(1, 7, T_INT8, 0, &mode, 1, true);
    taskENTER_CRITICAL(&s_mux);
    s_range_sent = mode;
    s_range_wait = mode == s_range_cam ? 0 : esp_timer_get_time() + CONFIRM_US;
    s_st.dynamic_range = mode;
    taskEXIT_CRITICAL(&s_mux);
    update(CAM_CH_FORMAT);
}

uint8_t cam_check_refused(void)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (s_codec_wait && now > s_codec_wait) {
        s_codec_wait = 0;
        s_refused |= CAM_REFUSED_CODEC;
    }
    if (s_fmt_wait && now > s_fmt_wait) {
        s_fmt_wait = 0;
        s_refused |= CAM_REFUSED_FORMAT;
    }
    if (s_range_wait && now > s_range_wait) {
        s_range_wait = 0;
        s_refused |= CAM_REFUSED_RANGE;
    }
    const uint8_t r = s_refused;
    s_refused = 0;
    /* Back to what the camera has */
    if ((r & CAM_REFUSED_CODEC) && s_codec_cam[0] >= 0) {
        s_st.codec = s_codec_cam[0];
        s_st.codec_variant = s_codec_cam[1];
    }
    if ((r & CAM_REFUSED_FORMAT) && s_have_fmt_cam) {
        format_to_state(s_fmt_cam);
    }
    if ((r & CAM_REFUSED_RANGE) && s_range_cam >= 0) {
        s_st.dynamic_range = s_range_cam;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (r) {
        update(CAM_CH_FORMAT | CAM_CH_VALUES);
    }
    return r;
}

void cam_set_sharpening(int8_t level)
{
    set8(8, level, &s_st.sharpening, CAM_CH_FORMAT);
}
