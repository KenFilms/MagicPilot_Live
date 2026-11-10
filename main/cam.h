/*
 * Blackmagic camera state and control commands (Blackmagic Camera Control protocol over BLE).
 *
 * The BLE link (cam_ble.c) feeds every notification into cam_handle_packets(), which updates the
 * state below. The UI reads a copy with cam_get() and is told what changed by cam_take_changes().
 * The cam_set_* commands encode a control packet and queue it for the camera; most of them also
 * update the local state right away so the UI reacts before the camera confirms.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CAM_LINK_DOWN,          /* Bluetooth not available */
    CAM_LINK_IDLE,          /* waiting before the next scan */
    CAM_LINK_SCANNING,
    CAM_LINK_CONNECTING,
    CAM_LINK_PIN,           /* the camera asks for its PIN */
    CAM_LINK_CONNECTED,
} cam_link_t;

/* Change flags returned by cam_take_changes() */
#define CAM_CH_LINK         (1u << 0)
#define CAM_CH_VALUES       (1u << 1)       /* ISO, shutter, frame rate, white balance, iris, exposure mode */
#define CAM_CH_TIMECODE     (1u << 2)
#define CAM_CH_TRANSPORT    (1u << 3)       /* record state and active card */
#define CAM_CH_MEDIA        (1u << 4)       /* remaining record time */
#define CAM_CH_POWER        (1u << 5)
#define CAM_CH_FORMAT       (1u << 6)       /* codec, resolution, dynamic range, sharpening, LUT */
#define CAM_CH_FOCUS        (1u << 7)
#define CAM_CH_SLATE        (1u << 8)

/* Writes the camera did not take (cam_check_refused) */
#define CAM_REFUSED_CODEC   (1u << 0)       /* BRAW <-> ProRes */
#define CAM_REFUSED_FORMAT  (1u << 1)       /* resolution or frame rates, e.g. ProRes resolutions */
#define CAM_REFUSED_RANGE   (1u << 2)       /* dynamic range, e.g. Film */

/* Recording format flags (parameter 1.9) */
#define CAM_FMT_FILE_MRATE      (1 << 0)
#define CAM_FMT_SENSOR_MRATE    (1 << 1)
#define CAM_FMT_OFF_SPEED       (1 << 2)
#define CAM_FMT_INTERLACED      (1 << 3)
#define CAM_FMT_WINDOWED        (1 << 4)
#define CAM_FMT_RATE_FLAGS      (CAM_FMT_FILE_MRATE | CAM_FMT_SENSOR_MRATE | CAM_FMT_OFF_SPEED)

#define CAM_FOCUS_MAX           2048        /* the focus slider's far end (0 is near) */

/* Overlay enables (parameter 3.0) and the displays they apply to */
#define CAM_OVL_STATUS          (1 << 0)
#define CAM_OVL_GUIDES          (1 << 1)
#define CAM_OVL_CLEAN_FEED      (1 << 2)
#define CAM_DISPLAY_LCD         (1 << 0)
#define CAM_DISPLAY_HDMI        (1 << 1)

/* Exposure and focus tools (parameter 4.1) */
#define CAM_TOOL_ZEBRA          (1 << 0)
#define CAM_TOOL_FOCUS_ASSIST   (1 << 1)
#define CAM_TOOL_FALSE_COLOR    (1 << 2)

/* Codecs (parameter 10.0) */
#define CAM_CODEC_BRAW          3
#define CAM_CODEC_PRORES        2

typedef struct {
    cam_link_t link;
    char status[48];                /* link status for the user, e.g. "Searching for camera" */
    char name[32];                  /* advertised camera name */

    int32_t iso;                    /* 0 = not reported */
    int32_t shutter_angle;          /* degrees x 100, 0 = not reported */
    int32_t shutter_speed;          /* 1/x second, 0 = not reported */
    bool shutter_speed_mode;        /* the camera measures the shutter as a speed: it reports that, not the angle */
    bool have_format;
    int16_t file_fps, sensor_fps;   /* project and sensor frame rate */
    int16_t width, height;
    int16_t format_flags;           /* CAM_FMT_* */
    bool have_wb;
    int16_t wb_kelvin, wb_tint;
    bool have_iris;
    int16_t iris_av;                /* aperture value, fixed16 (f-number = sqrt(2) ^ av) */
    bool have_iris_norm;
    int16_t iris_norm;              /* 0 (wide open) .. 2048 (closed), fixed16, as the BMPCC6K reports it */
    int16_t focus;                  /* focus slider, 0 (near) .. 2048 (far): 1024 (50 %) is where the lens was at
                                       connect or after autofocus, unless the camera reports its focus */
    int8_t ae_mode;                 /* 0 manual, 1 iris, 2 shutter, 3 iris + shutter, 4 shutter + iris; -1 = not reported */
    int8_t dynamic_range;           /* 0 film, 1 video, 2 extended video; -1 = not reported */
    int8_t sharpening;              /* 0 off, 1 low, 2 medium, 3 high; -1 = not reported */
    int8_t codec, codec_variant;    /* -1 = not reported */
    int8_t lut;                     /* display LUT: 0 none, 1 custom, 2 film to video, 3 film to extended video */
    bool lut_enabled;
    uint8_t tools;                  /* exposure and focus tools (CAM_TOOL_*); the BMPCC6K does not report them,
                                       so this is what was last sent */
    uint8_t overlay_bits;           /* overlay enables (parameter 3.0, CAM_OVL_*) as reported or last sent */
    bool have_overlays;             /* the camera's own guides (parameter 3.3): */
    int8_t guide_style;             /* frame guides: 0 off, 1 2.4:1, 2 2.39:1, 3 2.35:1, 4 1.85:1, 5 16:9, ... */
    int8_t guide_opacity;           /* 0 .. 100 */
    int8_t safe_area;               /* percent, 0 off */
    int8_t grid;                    /* bits: thirds, cross hairs, centre dot, horizon */

    uint8_t transport_mode;         /* 0 preview, 1 play, 2 record */
    int8_t transport_speed;         /* playback: 1 normal, 2, 4, 8 faster, negative backwards, 0 paused */
    uint8_t transport_flags;        /* bit 0: loop, bit 5: slot 1 active, bit 6: slot 2 active */
    int8_t slot_medium[2];          /* 0 CFast, 1 SD, 2 SSD/USB; -1 = not reported */
    int32_t slot_remaining[3];      /* seconds of recording left per slot; -1 = not reported */

    int32_t voltage_mv;             /* 0 = not reported */
    uint16_t power_flags;           /* third value: bit 1 set while on external power (seen as 2, the camera shows "AC") */

    bool have_tc;
    uint8_t tc[4];                  /* BCD: frames, seconds, minutes, hours */
    uint8_t tc_source;              /* 0 timecode, 1 clip */
} cam_state_t;

/* Slate (metadata, group 12; undocumented, see docs/BT_findings.md) */
#define CAM_SLATE_TEXT      57              /* longest text + 1: a packet holds 56 bytes of data */
/* Longest text one write takes: the camera's 64-byte MTU leaves 61 bytes, an 8-byte header and padding to 4 */
#define CAM_SLATE_SEND_MAX  52
enum {
    CAM_SLATE_REEL,                         /* read only on the BMPCC6K */
    CAM_SLATE_TAGS,
    CAM_SLATE_SCENE,
    CAM_SLATE_TAKE,
    CAM_SLATE_GOOD_TAKE,
    CAM_SLATE_CAMERA_ID,
    CAM_SLATE_OPERATOR,
    CAM_SLATE_DIRECTOR,
    CAM_SLATE_PROJECT,
    CAM_SLATE_LENS_TYPE,
    CAM_SLATE_IRIS,
    CAM_SLATE_FOCAL_LENGTH,
    CAM_SLATE_DISTANCE,
    CAM_SLATE_FILTER,
    CAM_SLATE_MODE,                         /* read only */
    CAM_SLATE_TARGET,                       /* read only */
    CAM_SLATE_PARAMS,
};

typedef struct {
    bool have;                              /* the camera reported its slate */
    int16_t reel;
    int8_t shot;                            /* -1 none, 0 WS, 1 CU, 2 MS, 3 BCU, 4 MCU, 5 ECU */
    int8_t interior;                        /* 0 exterior, 1 interior */
    int8_t day;                             /* 0 night, 1 day */
    int8_t take;
    int8_t take_tag;                        /* -1 none, 0 PU, 1 VFX, 2 SER */
    bool good_take;
    int8_t mode;                            /* 0 recording, 1 playback */
    char text[CAM_SLATE_PARAMS][CAM_SLATE_TEXT];    /* the text parameters, by number */
} cam_slate_t;

/* Link down ("Starting Bluetooth") and no camera values; call before cam_ble_start() */
void cam_init(void);

/* Copy of the current state */
void cam_get(cam_state_t *out);
void cam_get_slate(cam_slate_t *out);

/* Bits for everything that changed since the last call */
uint32_t cam_take_changes(void);

/* Called (from any task) whenever something changed */
void cam_set_change_callback(void (*cb)(void));

/* ---- Used by the BLE link ---- */
/* Link state and its status text for the user (NULL = ""); raises CAM_CH_LINK */
void cam_set_link(cam_link_t link, const char *status);
/* Name the camera advertises (NULL = ""); raises CAM_CH_LINK */
void cam_set_name(const char *name);
/* Decodes a notification, one or more control packets, into the state; calls the change callback if anything changed */
void cam_handle_packets(const uint8_t *data, size_t len);
/* Forgets all the camera reported (at connect and disconnect); the focus slider goes back to 50 % */
void cam_reset_values(void);

/* ---- Commands ---- */
/* ISO (1.14), e.g. 800 */
void cam_set_iso(int32_t iso);
/* Shutter angle in degrees x 100 (1.11), e.g. 18000 for 180 degrees */
void cam_set_shutter_angle(int32_t angle100);
void cam_set_shutter_speed(int32_t speed);  /* 1/speed second */
/* Project frame rate (1.9), the sensor's too unless off-speed. Like the other recording format commands below,
   it does nothing until the camera has reported its format */
void cam_set_project_fps(int16_t fps, bool mrate);   /* mrate: 24, 30, 60 as 23.98, 29.97, 59.94 */
/* Off-speed on, with the sensor at sensor_fps, or off: the sensor back at the project rate (sensor_fps unused) */
void cam_set_off_speed(bool on, int16_t sensor_fps);
/* Project and sensor rate in one write, with the M-rate and off-speed flags of rate_flags (presets) */
void cam_set_frame_rates(int16_t project_fps, int16_t sensor_fps, int16_t rate_flags);
/* The same with the resolution too when width > 0: one write, which the camera takes or refuses as a whole (a
   resolution that allows the frame rates only together with them) */
void cam_set_format(int16_t project_fps, int16_t sensor_fps, int16_t rate_flags, int16_t width, int16_t height,
                    bool windowed);
/* Resolution; windowed: read from a window of the sensor rather than the whole of it */
void cam_set_resolution(int16_t width, int16_t height, bool windowed);
/* White balance (1.2): kelvin 2500 .. 10000, tint -50 .. 50 */
void cam_set_wb(int16_t kelvin, int16_t tint);
/* One-shot auto white balance (1.3); the state is left to the camera's reports */
void cam_auto_wb(void);
void cam_iris_step(int steps);              /* + = narrower (higher f-number) */
void cam_set_iris_norm(int16_t norm);       /* 0 (wide open) .. 2048 */
/* Moves the focus slider (0 near .. 2048 far); the lens moves by the difference (a focus offset),
 * which needs no absolute lens position */
void cam_set_focus(int16_t focus);
void cam_autofocus(void);                   /* one-shot autofocus; the slider returns to 50 % */
/* Auto exposure mode (1.10): 0 manual, 1 iris, 2 shutter, 3 iris + shutter, 4 shutter + iris */
void cam_set_ae_mode(int8_t mode);
/* Starts or stops recording (transport mode 2 or 0, 10.1); the state is left to the camera's report */
void cam_record(bool on);
/* Playback (transport mode 1, tried on the BMPCC6K): the last clip of the active card, at speed (1 normal,
   2, 4, 8 faster, negative backwards, 0 paused) */
void cam_play(int8_t speed);
/* Playback loop on or off (10.1 flags bit 0), the active card kept */
void cam_play_loop(bool loop);
void cam_clip_step(bool next);              /* the next or previous clip (10.2) */
void cam_live(void);                        /* back from playback to the live picture (transport mode 0) */
/* What the camera's timecode display shows (4.7): 0 timecode, 1 clip counter (the BMPCC6K's values) */
void cam_set_tc_source(uint8_t source);
/* Codec (CAM_CODEC_*) and its variant, the quality (10.0); the BMPCC6K refuses BRAW <-> ProRes */
void cam_set_codec(int8_t codec, int8_t variant);
/* Dynamic range (1.7): 0 film, 1 video, 2 extended video; the BMPCC6K refuses film */
void cam_set_dynamic_range(int8_t mode);
/* Writes of codec, recording format or dynamic range the camera did not take (CAM_REFUSED_*): their
   values are back to the camera's. Call regularly */
uint8_t cam_check_refused(void);
/* Sharpening (1.8): 0 off, 1 low, 2 medium, 3 high */
void cam_set_sharpening(int8_t level);
void cam_set_tools(uint8_t tools, uint8_t displays);   /* CAM_TOOL_*, CAM_DISPLAY_* */
/* The camera's own guides (3.3): frame guide style, its opacity 0 .. 100, safe area percent (0 off), grid bits */
void cam_set_overlays(int8_t guide_style, int8_t guide_opacity, int8_t safe_area, int8_t grid);
void cam_set_overlay_enables(uint8_t bits, uint8_t displays);   /* CAM_OVL_*, CAM_DISPLAY_* */
void cam_set_slate_text(uint8_t param, const char *text);   /* "" clears it */
/* Scene tags (12.1): shot type, 0 exterior or 1 interior, 0 night or 1 day (values as in cam_slate_t) */
void cam_set_slate_tags(int8_t shot, int8_t interior, int8_t day);
/* Take number and take tag (12.3): -1 none, 0 PU, 1 VFX, 2 SER */
void cam_set_slate_take(int8_t take, int8_t tag);
/* Marks the take good, or not (12.4) */
void cam_set_slate_good_take(bool good);

/* ---- Formatting helpers ---- */
/* f-number of a fixed16 aperture value: sqrt(2) ^ (av / 2048) */
float cam_iris_fnumber(int16_t av);
/* Fixed16 aperture value of an f-number */
int16_t cam_iris_av(float fnumber);
/* Frame rate as text: "24", or "23.98" with mrate or as the camera may report it (23; also 29, 47, 59) */
void cam_fmt_fps(char *out, size_t size, int16_t fps, bool mrate);
/* Shutter angle (degrees x 100) as text, to a tenth of a degree: "180°", "172.8°" */
void cam_fmt_shutter(char *out, size_t size, int32_t angle100);
void cam_fmt_shutter_speed(char *out, size_t size, int32_t speed);    /* "1/50" */
/* Fixed16 aperture value as an f-number: two decimals below f10 ("f2.80"), then one ("f11.0") */
void cam_fmt_iris(char *out, size_t size, int16_t av);
/* BCD timecode (frames, seconds, minutes, hours) as "HH:MM:SS:FF" */
void cam_fmt_timecode(char *out, size_t size, const uint8_t tc[4]);
