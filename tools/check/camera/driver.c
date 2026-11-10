/*
 * Every camera command of main/cam.c, run on a computer: prints each Bluetooth packet it sends, and after
 * each command the change flags and a checksum of the camera state. Format reports from the camera are fed
 * in, and the clock is moved past answer deadlines, so that refused changes are taken back too.
 * Built and compared by tools/check/check.py.
 */
#include <stdio.h>
#include <string.h>
#include "cam.h"
#include "cam_ble.h"

static int64_t g_now = 1000000;
/* Stub of the firmware's clock (microseconds): g_now, which LATER() moves on */
int64_t esp_timer_get_time(void) { return g_now; }

/* Stub of the BLE link: prints each packet with its coalescing key */
bool cam_ble_send(const uint8_t *p, size_t len, uint16_t key)
{
    printf("  send key %04x:", key);
    for (size_t i = 0; i < len; i++) {
        printf(" %02x", p[i]);
    }
    printf("\n");
    return true;
}

/* Prints, after what, the change flags raised since the last call and a checksum of the whole camera state */
static void state(const char *what)
{
    cam_state_t s;
    cam_get(&s);
    const uint32_t ch = cam_take_changes();
    printf("%s: changes %03x state", what, ch);
    const uint8_t *b = (const uint8_t *)&s;
    unsigned sum = 0;
    for (size_t i = 0; i < sizeof(s); i++) {
        sum = sum * 31 + b[i];
    }
    printf(" %08x\n", sum);
}

/* A report from the camera: category, parameter, type, then data */
static void report(uint8_t cat, uint8_t par, uint8_t type, const uint8_t *d, uint8_t n)
{
    uint8_t p[64] = {0xFF, (uint8_t)(4 + n), 0, 0, cat, par, type, 2};
    memcpy(p + 8, d, n);
    cam_handle_packets(p, (size_t)((8 + n + 3) & ~3));
}

/* A report of n 16-bit values (type 2), e.g. the recording format */
static void rep16(uint8_t cat, uint8_t par, const int16_t *v, int n)
{
    uint8_t d[32];
    for (int i = 0; i < n; i++) {
        d[2 * i] = (uint8_t)v[i];
        d[2 * i + 1] = (uint8_t)(v[i] >> 8);
    }
    report(cat, par, 2, d, (uint8_t)(2 * n));
}

/* DO runs a command and prints what it changed; LATER(us) moves the clock on, then runs cam_check_refused() */
#define DO(call) do { printf("%s\n", #call); call; state("  after"); } while (0)
#define LATER(us) do { g_now += (us); printf("check_refused at +%d: %02x\n", (int)(us), cam_check_refused()); state("  after"); } while (0)

/* Connects, feeds in the camera's format and ISO, then runs every command, with LATER() checking for refusals */
int main(void)
{
    cam_init();
    cam_set_link(CAM_LINK_CONNECTED, "Connected");
    state("connected");
    const int16_t fmt[5] = {25, 25, 6144, 3456, 0};
    rep16(1, 9, fmt, 5);
    state("format reported");
    const int32_t iso = 800;
    report(1, 14, 3, (const uint8_t *)&iso, 4);
    state("iso reported");

    DO(cam_set_iso(400));
    DO(cam_set_iso(25600));
    DO(cam_set_shutter_angle(18000));
    DO(cam_set_shutter_angle(17280));
    DO(cam_set_shutter_speed(50));
    DO(cam_set_shutter_speed(2000));
    DO(cam_set_project_fps(24, false));
    DO(cam_set_project_fps(30, true));
    LATER(3000000);
    DO(cam_set_off_speed(true, 60));
    DO(cam_set_off_speed(true, 120));
    DO(cam_set_project_fps(24, true));      /* 23.98 while off-speed: the sensor keeps its own M-rate flag */
    DO(cam_set_off_speed(false, 0));
    LATER(3000000);
    DO(cam_set_frame_rates(25, 50, 0));
    DO(cam_set_frame_rates(24, 60, CAM_FMT_OFF_SPEED | CAM_FMT_FILE_MRATE));
    DO(cam_set_format(30, 120, CAM_FMT_OFF_SPEED, 2880, 1512, true));     /* resolution with the frame rates */
    DO(cam_set_resolution(1920, 1080, true));
    DO(cam_set_resolution(3840, 2160, false));
    LATER(3000000);
    rep16(1, 9, fmt, 5);
    state("format reported again");
    DO(cam_set_resolution(4096, 2160, false));
    rep16(1, 9, fmt, 5);
    state("format reported back (refused)");
    LATER(3000000);
    DO(cam_set_wb(5600, 10));
    DO(cam_set_wb(3200, -20));
    DO(cam_auto_wb());
    DO(cam_iris_step(1));
    DO(cam_iris_step(-2));
    DO(cam_set_iris_norm(512));
    DO(cam_set_slate_text(CAM_SLATE_SCENE, "23A"));
    DO(cam_set_slate_tags(2, 1, 0));
    DO(cam_set_slate_take(5, 1));
    DO(cam_set_slate_good_take(true));
    DO(cam_set_tools(0x05, 0x03));
    DO(cam_set_tools(0x00, 0x01));
    DO(cam_set_overlay_enables(0x0E, 0x01));
    DO(cam_set_overlay_enables(0x00, 0x02));
    DO(cam_set_overlays(1, 60, 90, 3));
    DO(cam_set_focus(1000));
    DO(cam_set_focus(-5));
    DO(cam_set_focus(3000));
    DO(cam_autofocus());
    DO(cam_set_ae_mode(2));
    DO(cam_set_ae_mode(0));
    DO(cam_set_sharpening(2));
    DO(cam_set_sharpening(0));
    DO(cam_record(true));
    DO(cam_record(false));
    DO(cam_play(1));
    DO(cam_play_loop(true));
    DO(cam_play_loop(false));
    DO(cam_clip_step(true));
    DO(cam_live());
    DO(cam_set_tc_source(1));
    DO(cam_set_codec(3, 1));
    DO(cam_set_codec(2, 0));
    LATER(3000000);
    DO(cam_set_dynamic_range(1));
    LATER(3000000);
    DO(cam_reset_values());
    return 0;
}
