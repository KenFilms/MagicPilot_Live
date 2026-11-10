/*
 * MagicPilot LIVE – M5Stack Tab5 HDMI monitor and Bluetooth remote for the BMPCC6K
 *
 * Project by: Ken Friedl
 * License: LGPL-3.0-or-later
 * 
 * HDMI source -> USB HDMI capture adapter (UVC) -> Tab5 USB-A (ESP32-P4 USB 2.0 HS host)
 *   MJPEG frames: HW JPEG decoder -> RGB565 -> PPA (rotate + scale) -> LCD frame buffer
 *   YUY2  frames: PPA (YUV->RGB565 + rotate + scale)                        -> LCD frame buffer
 * The camera UI (overlay.c, drawn by ui.c from what the camera reports over Bluetooth, cam_ble.c)
 * is composited on top and the frame buffer is flipped on a fixed refresh cadence.
 *
 * Band mode (integer vertical scale): the PPA reads PSRAM slowly (~46 MB/s) but SRAM fast, so an
 * AXI GDMA (~114 MB/s from PSRAM) copies bands of source rows into internal SRAM and the PPA
 * rotates each band from there into the frame buffer while the GDMA fetches the next band. The
 * overlay is composited into each band while it is in SRAM (CPU access to PSRAM is too slow to
 * do it in the frame buffer every frame).
 *
 * The panel is 720x1280 portrait, the picture is shown in landscape (1280x720).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_async_memcpy.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "nvs_flash.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "overlay.h"
#include "cam.h"
#include "cam_ble.h"
#include "ui.h"

static const char *TAG = "hdmi";

/* Panel geometry (native portrait) and the landscape view shown to the user */
#define LCD_W               BSP_LCD_H_RES           /* 720  */
#define LCD_H               BSP_LCD_V_RES           /* 1280 */
#define VIEW_W              LCD_H                   /* 1280 */
#define VIEW_H              LCD_W                   /* 720  */
#define FB_SIZE             (LCD_W * LCD_H * 2)     /* RGB565 */

/* Largest capture resolution we accept (limits the JPEG output buffer) */
#define MAX_SRC_W           1920
#define MAX_SRC_H           1088

#define UVC_FRAME_BUFFERS   3
#define UVC_URBS            4
#define UVC_URB_SIZE        (24 * 1024)


#define VIDEO_TIMEOUT_US    300000                  /* no frame for this long: show the still screen */

#if CONFIG_HDMI_ROTATION_270
#define PPA_ROTATION        PPA_SRM_ROTATION_ANGLE_270
#else
#define PPA_ROTATION        PPA_SRM_ROTATION_ANGLE_90
#endif
#define ROT270              (PPA_ROTATION == PPA_SRM_ROTATION_ANGLE_270)

#define ALIGN_UP(x, a)      ((((x) + (a) - 1) / (a)) * (a))

/* ------------------------------------------------------------------------- */
/* Types                                                                     */
/* ------------------------------------------------------------------------- */

typedef enum {
    APP_EVT_DEVICE_CONNECTED,
    APP_EVT_DEVICE_DISCONNECTED,
    APP_EVT_LIVE_VIEW,              /* the live view setting changed */
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    bool on;                        /* APP_EVT_LIVE_VIEW */
    uint8_t dev_addr;
    uint8_t stream_index;
    uvc_host_stream_format_t format;
    uvc_host_stream_hdl_t stream;
} app_evt_t;

/* How a source picture is mapped into the portrait frame buffer */
typedef struct {
    uint32_t src_w, src_h;          /* visible source size */
    uint32_t pic_w, pic_h;          /* source buffer size (JPEG output is MCU padded) */
    uint32_t crop_x, crop_y, crop_w, crop_h; /* the part of the source that is shown */
    uint32_t scale_x16, scale_y16;  /* PPA scale factors in 1/16 steps */
    uint32_t out_x, out_y, out_w, out_h; /* destination rectangle in frame buffer coordinates */
    uint32_t band_rows;             /* source rows per band, 0 = direct render */
    bool covers_screen;             /* the picture fills the whole frame buffer: no borders to clear */
} layout_t;

/* One frame for the band renderer */
typedef struct {
    const void *src;                /* NULL: black picture */
    ppa_srm_color_mode_t src_cm;
    ppa_color_range_t yuv_range;
    void *fb;
    layout_t l;
    bool composite;                 /* composite the overlay into the bands (1:1 RGB565 layouts) */
} band_job_t;

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static esp_lcd_panel_handle_t s_panel;
/*
 * Triple buffering: one buffer is scanned out, one may be queued for the next refresh and the
 * third can be rendered into without waiting for VSYNC.
 */
#define FB_COUNT            3
static void *s_fb[FB_COUNT];
static int s_back_fb = 1;                   /* DPI driver starts scanning fb[0] */
static uint32_t s_fb_layout_id[FB_COUNT];   /* layout each buffer was last rendered with, 0 = still screen */
static uint32_t s_layout_id;                /* bumped with every change of s_layout; 0 until the first frame */
static layout_t s_layout;                   /* the layout frames are rendered with: the base layout or its zoom */

/* Accumulated per-stage time (us) for the periodic statistics */
static int64_t s_t_decode, s_t_wait, s_t_srm, s_t_present, s_t_pace, s_t_render;
static volatile int64_t s_t_overlay;        /* overlay compositing (bands and fallback) */
static volatile int64_t s_t_overlay_wait;   /* of which waiting for the overlay lock */
static volatile int64_t s_t_gdma_wait, s_t_ppa_submit, s_t_ppa_wait;  /* band pipeline breakdown */
static int64_t s_render_t0;                 /* start of the render job in flight */
static volatile int64_t s_render_t1;        /* its completion (set by the PPA ISR / blit task) */

static SemaphoreHandle_t s_vsync_sem;
static volatile uint32_t s_vsync_count;
static uint32_t s_present_vsync[FB_COUNT];  /* refresh counter when each buffer was presented */
static bool s_presented[FB_COUNT];          /* presented at least once: its s_present_vsync is valid */
static int s_ready_fb = -1;                 /* rendered, waiting for its refresh slot */
static uint32_t s_last_present_vsync;       /* refresh counter at the latest presentation, for the pacing */
static uint32_t s_pace = 1;                 /* refreshes per shown frame */
static uint32_t s_vsync_period_us = 16667;  /* measured at boot */

static ppa_client_handle_t s_ppa_srm;
static ppa_client_handle_t s_ppa_fill;
static jpeg_decoder_handle_t s_jpeg;
static uint8_t *s_dec_buf[2];               /* JPEG output, double buffered: decode N+1 while PPA renders N */
static size_t s_dec_buf_size;
static int s_dec_idx;                       /* the buffer the next JPEG is decoded into */

/* PPA scale/rotate job that is still running in the background */
static SemaphoreHandle_t s_srm_done_sem;
static bool s_srm_pending;
static bool s_pending_composited;           /* the overlay went in with the bands */

/* Band renderer */
#define BAND_BUFS           3               /* GDMA fills one, the CPU composites one, the PPA reads one */
static ppa_client_handle_t s_ppa_band_nb;   /* non-blocking SRM client of the band pipeline */
static SemaphoreHandle_t s_band_ppa_sem;    /* given when a band rotation completes */
static async_memcpy_handle_t s_mcp;         /* AXI GDMA that copies the bands from PSRAM */
static uint8_t *s_band_buf[BAND_BUFS];      /* source bands in internal SRAM */
static size_t s_band_buf_size;              /* 0 = band mode unavailable */
static SemaphoreHandle_t s_copy_sem[BAND_BUFS]; /* given while no GDMA copy into the buffer is in flight */
static QueueHandle_t s_band_q;              /* band_job_t */
static layout_t s_blank_layout;             /* 1280x720 1:1, for the still screen */
static uint32_t s_blank_gen = UINT32_MAX;   /* overlay generation of the last still screen */

/* Second core for the overlay: composites the lower 60 % of a band while the blit task does the upper 40 % */
static TaskHandle_t s_ov_helper;
static SemaphoreHandle_t s_ov_done;
static struct {
    uint8_t *band;
    size_t row_bytes;
    uint32_t y0, h;
} s_ov_part;

static QueueHandle_t s_frame_q;             /* uvc_host_frame_t * */
static QueueHandle_t s_app_evt_q;           /* app_evt_t */
static SemaphoreHandle_t s_stream_mutex;    /* guards s_stream and s_stream_active */
static uvc_host_stream_hdl_t s_stream;      /* the open capture stream, NULL if none */
static bool s_stream_active;                /* frames of s_stream are rendered (false while it closes) */
static volatile bool s_need_blank = true;   /* redraw the still screen even if the overlay has not changed */
static int64_t s_last_frame_us;             /* arrival of the latest frame, for VIDEO_TIMEOUT_US */

/* ------------------------------------------------------------------------- */
/* Display                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * DSI DMA interrupt at the end of every refresh, where the buffer presented last takes over: counts the
 * refresh and wakes wait_vsync(). The driver requires this callback in IRAM.
 */
static IRAM_ATTR bool on_frame_buf_complete(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    s_vsync_count++;
    xSemaphoreGiveFromISR(s_vsync_sem, &woken);
    return woken == pdTRUE;
}

/*
 * Start the panel and its backlight through the BSP and take the DPI driver's frame buffers, which the
 * BSP allocates (CONFIG_BSP_LCD_DPI_BUFFER_NUMS, at least FB_COUNT). Hooks up the refresh interrupt.
 */
static void display_init(void)
{
    const bsp_display_config_t disp_cfg = {
        .dsi_bus = {
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(bsp_display_new(&disp_cfg, &s_panel, &io));
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, FB_COUNT, &s_fb[0], &s_fb[1], &s_fb[2]));

    s_vsync_sem = xSemaphoreCreateBinary();
    assert(s_vsync_sem);
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_frame_buf_complete = on_frame_buf_complete,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL));

    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "disp_on_off failed: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(CONFIG_HDMI_BACKLIGHT_PERCENT));
}

/* Wait until the refresh count reaches target; false after 100 ms without a refresh */
static bool wait_vsync(uint32_t target)
{
    while ((int32_t)(s_vsync_count - target) < 0) {
        if (xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            return false;
        }
    }
    return true;
}

/*
 * Block until the back buffer is no longer scanned out by the DSI DMA. That is the case once the
 * buffer presented after it has been picked up, i.e. a refresh completed since that presentation.
 */
static void wait_back_buffer_free(void)
{
    const int successor = (s_back_fb + 1) % FB_COUNT;
    if (s_presented[successor] && !wait_vsync(s_present_vsync[successor] + 1)) {
        ESP_LOGW(TAG, "VSYNC timeout");
    }
}

/* The back buffer is complete: queue it for presentation and move on to the next buffer */
static void mark_back_buffer_ready(void)
{
    s_ready_fb = s_back_fb;
    s_back_fb = (s_back_fb + 1) % FB_COUNT;
}

/*
 * Make the ready buffer visible; the DPI driver switches to it at the next frame boundary.
 * With pacing, wait until s_pace refreshes have passed since the previous frame was presented,
 * so that every frame stays on screen for the same number of refreshes.
 */
static void present_ready_buffer(bool paced)
{
    if (s_ready_fb < 0) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    if (paced && s_pace > 1) {
        /* A buffer presented at count c is shown from refresh c + 1 until the next one takes over */
        wait_vsync(s_last_present_vsync + s_pace);
    }
    int64_t t1 = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb[s_ready_fb]));
    s_last_present_vsync = s_vsync_count;
    s_present_vsync[s_ready_fb] = s_vsync_count;
    s_presented[s_ready_fb] = true;
    s_ready_fb = -1;
    s_t_pace += t1 - t0;
    s_t_present += esp_timer_get_time() - t1;
}

/* The whole frame buffer black */
static esp_err_t clear_fb(void *fb)
{
    ppa_fill_oper_config_t op = {
        .out = {
            .buffer = fb,
            .buffer_size = FB_SIZE,
            .pic_w = LCD_W,
            .pic_h = LCD_H,
            .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = LCD_W,
        .fill_block_h = LCD_H,
        .fill_argb_color = { .val = 0xFF000000u },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_fill(s_ppa_fill, &op);
}

/* Slow path: composite the overlay into a whole frame buffer and write it back for the display DMA */
static void composite_fb(void *fb)
{
    const int64_t t0 = esp_timer_get_time();
    overlay_lock();
    overlay_composite_fb(fb, LCD_W, LCD_H, ROT270);
    overlay_unlock();
    esp_cache_msync(fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_t_overlay += esp_timer_get_time() - t0;
}

/* Still screen: black with the overlay, shown without HDMI picture or behind a full-screen page */
static void render_finish(void);

static void render_blank(void)
{
    render_finish();
    present_ready_buffer(false);
    wait_back_buffer_free();
    void *fb = s_fb[s_back_fb];
    s_blank_gen = overlay_generation();
    /* Band mode: black bands (no source) with the overlay, rotated in by the blit task and waited for here */
    if (s_blank_layout.band_rows) {
        const band_job_t job = {
            .src = NULL,
            .src_cm = PPA_SRM_COLOR_MODE_RGB565,
            .fb = fb,
            .l = s_blank_layout,
            .composite = true,
        };
        xQueueSend(s_band_q, &job, portMAX_DELAY);
        xSemaphoreTake(s_srm_done_sem, portMAX_DELAY);
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(clear_fb(fb));
        composite_fb(fb);
    }
    /* No picture layout in this buffer: the next picture rendered into it clears its borders first */
    s_fb_layout_id[s_back_fb] = 0;
    mark_back_buffer_ready();
    present_ready_buffer(false);
}

/* ------------------------------------------------------------------------- */
/* Scaling / layout                                                          */
/* ------------------------------------------------------------------------- */

/* Same integer math the PPA driver uses: int * len + frag * len / 16 */
static inline uint32_t scaled_len(uint32_t len, uint32_t s16)
{
    return (len * s16) / 16;
}

/* v limited to lo..hi */
static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* A scale in 1/16 steps kept within the PPA's range (1/16 to 255) */
static uint32_t clamp_scale16(uint32_t s16)
{
    return (uint32_t)clampi((int)s16, 1, 255 * 16);
}

/*
 * Source rows per band for band mode, or 0 if the layout has to be rendered directly. Each band
 * of source rows becomes rows * scale frame buffer columns, so the vertical scale must be an
 * integer for the bands to join without seams. Bands are copied by the GDMA, which needs
 * 64-byte aligned addresses and sizes.
 */
static uint32_t pick_band_rows(const layout_t *l)
{
#if CONFIG_HDMI_BAND_RENDER
    const size_t row_bytes = (size_t)l->pic_w * 2;
    if (s_band_buf_size == 0 || l->scale_y16 % 16 != 0 || row_bytes % 64 != 0) {
        return 0;
    }
    uint32_t rows = s_band_buf_size / row_bytes;
    if (rows > CONFIG_HDMI_BAND_ROWS) {
        rows = CONFIG_HDMI_BAND_ROWS;
    }
    rows &= ~1u;
    return rows >= 2 ? rows : 0;
#else
    return 0;
#endif
}

/*
 * Map a src_w x src_h source (in a pic_w x pic_h buffer) onto the landscape view, centred. FILL scales
 * each axis on its own to cover the view and crops the few pixels over; FIT keeps the aspect ratio and
 * leaves black borders. Also decides between band and direct rendering.
 */
static void compute_layout(uint32_t src_w, uint32_t src_h, uint32_t pic_w, uint32_t pic_h, layout_t *l)
{
    memset(l, 0, sizeof(*l));
    l->src_w = src_w;
    l->src_h = src_h;
    l->pic_w = pic_w;
    l->pic_h = pic_h;

#if CONFIG_HDMI_SCALE_FIT
    uint32_t sx = (VIEW_W * 16) / src_w;
    uint32_t sy = (VIEW_H * 16) / src_h;
    uint32_t s = clamp_scale16(sx < sy ? sx : sy);
    l->scale_x16 = s;
    l->scale_y16 = s;
    l->crop_w = src_w;
    l->crop_h = src_h;
#else
    /* Smallest representable scale that reaches the screen size; crop the few pixels that overflow */
    l->scale_x16 = clamp_scale16((VIEW_W * 16 + src_w - 1) / src_w);
    l->scale_y16 = clamp_scale16((VIEW_H * 16 + src_h - 1) / src_h);
    uint32_t max_w = (VIEW_W * 16) / l->scale_x16;
    uint32_t max_h = (VIEW_H * 16) / l->scale_y16;
    l->crop_w = src_w < max_w ? src_w : max_w;
    l->crop_h = src_h < max_h ? src_h : max_h;
#endif
    l->crop_w &= ~1u;   /* YUV422 input requires even width/offset */
    l->crop_x = ((src_w - l->crop_w) / 2) & ~1u;
    l->crop_y = (src_h - l->crop_h) / 2;

    /* After a 90/270 rotation the landscape width becomes the frame buffer height */
    uint32_t view_w = scaled_len(l->crop_w, l->scale_x16);
    uint32_t view_h = scaled_len(l->crop_h, l->scale_y16);
    l->out_w = view_h;
    l->out_h = view_w;
    l->out_x = (LCD_W - l->out_w) / 2;
    l->out_y = (LCD_H - l->out_h) / 2;
    l->covers_screen = (l->out_w == LCD_W && l->out_h == LCD_H);
    l->band_rows = pick_band_rows(l);

    ESP_LOGI(TAG, "Layout: src %"PRIu32"x%"PRIu32" (buf %"PRIu32"x%"PRIu32") crop %"PRIu32"x%"PRIu32"@%"PRIu32",%"PRIu32
             " scale %.4f/%.4f -> view %"PRIu32"x%"PRIu32"%s, %s %"PRIu32,
             src_w, src_h, pic_w, pic_h, l->crop_w, l->crop_h, l->crop_x, l->crop_y,
             l->scale_x16 / 16.0, l->scale_y16 / 16.0, view_w, view_h,
             l->covers_screen ? " (full screen)" : " (letterboxed)",
             l->band_rows ? "band rows" : "direct render", l->band_rows);
}

/*
 * 2x zoom (pinch on the touch screen): half of the visible source around the point the UI asks
 * for, scaled twice as much. With a 1280x720 source that is a 640x360 crop at scale 2, which the
 * band renderer handles; the camera UI is then composited into the frame buffer.
 */
static layout_t s_base_layout;              /* the source fitted to the screen */
static bool s_zoomed;                       /* s_layout is the zoom of s_base_layout */
static uint32_t s_zoom_x, s_zoom_y;         /* its crop, in source pixels */

/* Set s_layout to the zoom the UI asks for, or back to the base layout; every change bumps s_layout_id */
static void apply_zoom(void)
{
    float vx, vy;
    const bool on = ui_zoom(&vx, &vy);
    const layout_t *b = &s_base_layout;
    if (!on) {
        if (s_zoomed) {
            s_layout = *b;
            s_zoomed = false;
            s_layout_id++;
            ESP_LOGI(TAG, "Zoom off");
        }
        return;
    }
    layout_t z = *b;
    z.scale_x16 = b->scale_x16 * 2;
    z.scale_y16 = b->scale_y16 * 2;
    z.crop_w = (b->crop_w / 2) & ~1u;
    z.crop_h = b->crop_h / 2;
    /* The point of the picture under the view position, kept inside the visible source */
    const int sx = (int)b->crop_x + (int)(vx * b->crop_w / VIEW_W) - (int)z.crop_w / 2;
    const int sy = (int)b->crop_y + (int)(vy * b->crop_h / VIEW_H) - (int)z.crop_h / 2;
    const int max_x = (int)(b->crop_x + b->crop_w - z.crop_w), max_y = (int)(b->crop_y + b->crop_h - z.crop_h);
    z.crop_x = (uint32_t)clampi(sx, (int)b->crop_x, max_x) & ~1u;
    z.crop_y = (uint32_t)clampi(sy, (int)b->crop_y, max_y);
    if (s_zoomed && z.crop_x == s_zoom_x && z.crop_y == s_zoom_y) {
        return;
    }
    z.band_rows = pick_band_rows(&z);
    if (!s_zoomed) {
        ESP_LOGI(TAG, "Zoom 2x: crop %"PRIu32"x%"PRIu32" at %"PRIu32",%"PRIu32", %s", z.crop_w, z.crop_h, z.crop_x,
                 z.crop_y, z.band_rows ? "bands" : "direct render");
    }
    s_layout = z;
    s_zoomed = true;
    s_zoom_x = z.crop_x;
    s_zoom_y = z.crop_y;
    s_layout_id++;
}

/* Called for every frame: a new source size gets a new base layout, then the UI's zoom is applied */
static void update_layout(uint32_t src_w, uint32_t src_h, uint32_t pic_w, uint32_t pic_h)
{
    if (s_layout_id == 0 || s_base_layout.src_w != src_w || s_base_layout.src_h != src_h ||
            s_base_layout.pic_w != pic_w || s_base_layout.pic_h != pic_h) {
        compute_layout(src_w, src_h, pic_w, pic_h, &s_base_layout);
        s_layout = s_base_layout;
        s_zoomed = false;
        s_layout_id++;
    }
    apply_zoom();
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

/* PPA interrupt: a direct render job is done (render_finish() waits for this) */
static bool on_srm_done(ppa_client_handle_t client, ppa_event_data_t *event_data, void *user_data)
{
    BaseType_t woken = pdFALSE;
    s_render_t1 = esp_timer_get_time();
    xSemaphoreGiveFromISR(s_srm_done_sem, &woken);
    return woken == pdTRUE;
}

/* GDMA interrupt: the copy into band buffer cb_args is done */
static bool on_copy_done(async_memcpy_handle_t mcp, async_memcpy_event_t *event, void *cb_args)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_copy_sem[(int)(intptr_t)cb_args], &woken);
    return woken == pdTRUE;
}

/* The PPA job of a layout: its crop of src, scaled and rotated into its place in the frame buffer fb */
static ppa_srm_oper_config_t srm_op(const layout_t *l, const void *src, ppa_srm_color_mode_t cm,
                                    ppa_color_range_t range, void *fb)
{
    return (ppa_srm_oper_config_t) {
        .in = {
            .buffer = src,
            .pic_w = l->pic_w,
            .pic_h = l->pic_h,
            .block_w = l->crop_w,
            .block_h = l->crop_h,
            .block_offset_x = l->crop_x,
            .block_offset_y = l->crop_y,
            .srm_cm = cm,
            .yuv_range = range,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = fb,
            .buffer_size = FB_SIZE,
            .pic_w = LCD_W,
            .pic_h = LCD_H,
            .block_offset_x = l->out_x,
            .block_offset_y = l->out_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_ROTATION,
        .scale_x = l->scale_x16 / 16.0f,
        .scale_y = l->scale_y16 / 16.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
}

/* Start a GDMA copy of `rows` source rows from row `y` into band buffer `b` (black without a source) */
static esp_err_t band_fetch(const band_job_t *job, int b, uint32_t y, uint32_t rows)
{
    const size_t row_bytes = (size_t)job->l.pic_w * 2;
    if (!job->src) {
        memset(s_band_buf[b], 0, rows * row_bytes);
        return ESP_OK;
    }
    xSemaphoreTake(s_copy_sem[b], portMAX_DELAY);
    esp_err_t err = esp_async_memcpy(s_mcp, s_band_buf[b], (uint8_t *)job->src + y * row_bytes,
                                     rows * row_bytes, on_copy_done, (void *)(intptr_t)b);
    if (err != ESP_OK) {
        xSemaphoreGive(s_copy_sem[b]);
    }
    return err;
}

/* Wait until no GDMA copy into band buffer b is in flight */
static void band_wait(int b)
{
    xSemaphoreTake(s_copy_sem[b], portMAX_DELAY);
    xSemaphoreGive(s_copy_sem[b]);
}

/* Composite the overlay into h band rows that show view rows y0..; the overlay lock must be held */
static void composite_rows(uint8_t *band, size_t row_bytes, uint32_t y0, uint32_t h)
{
    for (uint32_t r = 0; r < h; r++) {
        overlay_composite_row((uint16_t *)(band + r * row_bytes), y0 + r);
    }
}

/* Runs on core 1; the blit task holds the overlay lock while this works */
static void overlay_helper_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        composite_rows(s_ov_part.band, s_ov_part.row_bytes, s_ov_part.y0, s_ov_part.h);
        xSemaphoreGive(s_ov_done);
    }
}

/* Composites the overlay into a band of picture rows y0.. (1:1 layout: band rows are view rows) */
static void band_overlay(uint8_t *band, size_t row_bytes, uint32_t y0, uint32_t h)
{
    uint32_t first = h, last = 0;
    for (uint32_t r = 0; r < h; r++) {
        if (!overlay_row_empty(y0 + r)) {
            first = r < first ? r : first;
            last = r;
        }
    }
    if (first == h) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    overlay_lock();
    s_t_overlay_wait += esp_timer_get_time() - t0;
    const uint32_t n = last + 1 - first;
    if (s_ov_helper && n >= 8) {
        /* Both cores: the helper takes the lower 60 % (core 0 also serves USB and Bluetooth) */
        const uint32_t half = n * 2 / 5;
        s_ov_part.band = band + (first + half) * row_bytes;
        s_ov_part.row_bytes = row_bytes;
        s_ov_part.y0 = y0 + first + half;
        s_ov_part.h = n - half;
        xTaskNotifyGive(s_ov_helper);
        composite_rows(band + first * row_bytes, row_bytes, y0 + first, half);
        xSemaphoreTake(s_ov_done, portMAX_DELAY);
    } else {
        composite_rows(band + first * row_bytes, row_bytes, y0 + first, n);
    }
    overlay_unlock();
}

/* PPA interrupt: a band rotation is done, so the PPA can take the next band */
static bool on_band_ppa_done(ppa_client_handle_t client, ppa_event_data_t *event_data, void *user_data)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_band_ppa_sem, &woken);
    return woken == pdTRUE;
}

/*
 * Render one frame in bands of `rows` source rows, through three SRAM buffers: while the GDMA
 * copies band k + 1 from PSRAM, the CPU composites the overlay into band k and the PPA rotates
 * band k - 1 into the frame buffer, where a band becomes rows * scale frame buffer columns.
 */
static esp_err_t render_bands(const band_job_t *job, uint32_t rows)
{
    const layout_t *l = &job->l;
    const uint32_t sy = l->scale_y16 / 16;
    const uint32_t n = (l->crop_h + rows - 1) / rows;
    bool ppa_busy = false;

    esp_err_t err = band_fetch(job, 0, l->crop_y, rows < l->crop_h ? rows : l->crop_h);
    for (uint32_t k = 0; k < n && err == ESP_OK; k++) {
        const int b = k % BAND_BUFS;
        const uint32_t y = k * rows;
        const uint32_t h = (y + rows <= l->crop_h) ? rows : l->crop_h - y;
        int64_t tw = esp_timer_get_time();
        band_wait(b);
        s_t_gdma_wait += esp_timer_get_time() - tw;
        if (k + 1 < n) {
            /* buffer (k + 1) % 3 was last read by the PPA for band k - 2, which has completed */
            const uint32_t y1 = y + rows;
            err = band_fetch(job, (k + 1) % BAND_BUFS, l->crop_y + y1, (y1 + rows <= l->crop_h) ? rows : l->crop_h - y1);
            if (err != ESP_OK) {
                break;
            }
        }
        /* The CPU works on band k while the PPA rotates band k - 1 and the GDMA fetches band k + 1 */
        if (job->composite) {
            const int64_t t0 = esp_timer_get_time();
            band_overlay(s_band_buf[b], (size_t)l->pic_w * 2, y, h);
            s_t_overlay += esp_timer_get_time() - t0;
        }
        /* Band k lands k bands in from the frame buffer's left edge (90 degrees) or right edge (270) */
        const uint32_t cols = h * sy;
        /* The band: its h rows from the top of its buffer (the vertical scale sy is an integer here) */
        ppa_srm_oper_config_t op = srm_op(l, s_band_buf[b], job->src_cm, job->yuv_range, job->fb);
        op.in.pic_h = h;
        op.in.block_h = h;
        op.in.block_offset_y = 0;
        op.out.block_offset_x = l->out_x + (ROT270 ? l->out_w - y * sy - cols : y * sy);
        tw = esp_timer_get_time();
        if (ppa_busy) {
            xSemaphoreTake(s_band_ppa_sem, portMAX_DELAY);
        }
        const int64_t ts = esp_timer_get_time();
        s_t_ppa_wait += ts - tw;
        err = ppa_do_scale_rotate_mirror(s_ppa_band_nb, &op);
        s_t_ppa_submit += esp_timer_get_time() - ts;
        ppa_busy = err == ESP_OK;
    }
    if (ppa_busy) {
        xSemaphoreTake(s_band_ppa_sem, portMAX_DELAY);
    }
    for (int b = 0; b < BAND_BUFS; b++) {
        band_wait(b);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Band render failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Runs band jobs so that the render task can decode the next JPEG meanwhile */
static void blit_task(void *arg)
{
    band_job_t job;
    while (true) {
        if (xQueueReceive(s_band_q, &job, portMAX_DELAY) == pdTRUE) {
            render_bands(&job, job.l.band_rows);
            s_render_t1 = esp_timer_get_time();
            xSemaphoreGive(s_srm_done_sem);
        }
    }
}

/* Wait for the background render job, add the overlay if the bands did not, and queue the buffer */
static void render_finish(void)
{
    if (!s_srm_pending) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_srm_done_sem, portMAX_DELAY);
    s_srm_pending = false;
    int64_t t1 = esp_timer_get_time();
    s_t_render += s_render_t1 - s_render_t0;

    if (!s_pending_composited) {
        composite_fb(s_fb[s_back_fb]);
    }
    mark_back_buffer_ready();
    s_t_srm += t1 - t0;
    s_t_present += esp_timer_get_time() - t1;
}

/*
 * Start scaling/rotating a picture into the back buffer, then present the previous frame on its
 * refresh slot (so the pacing wait overlaps the new render job). With background = true the call
 * returns while the job runs and render_finish() completes it; `src` must stay valid until then.
 */
static void render_start(const void *src, ppa_srm_color_mode_t src_cm, ppa_color_range_t range, bool background)
{
    const layout_t *l = &s_layout;

    render_finish();
    int64_t t0 = esp_timer_get_time();
    wait_back_buffer_free();
    s_t_wait += esp_timer_get_time() - t0;
    void *fb = s_fb[s_back_fb];

    /* First frame of this layout in this buffer: black where the picture will not reach */
    if (s_fb_layout_id[s_back_fb] != s_layout_id) {
        if (!l->covers_screen) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(clear_fb(fb));
        }
        s_fb_layout_id[s_back_fb] = s_layout_id;
    }

    s_render_t0 = esp_timer_get_time();
    s_pending_composited = false;
    /* Bands need a GDMA-aligned source (UVC frame buffers may not be) */
    if (l->band_rows && ((uintptr_t)src % 64) == 0) {
        /* The overlay goes into the bands when band rows are view rows: RGB565 shown 1:1 */
        const bool one_to_one = src_cm == PPA_SRM_COLOR_MODE_RGB565 && l->crop_w == VIEW_W && l->crop_h == VIEW_H &&
                                l->scale_x16 == 16 && l->scale_y16 == 16;
        band_job_t job = {
            .src = src,
            .src_cm = src_cm,
            .yuv_range = range,
            .fb = fb,
            .l = *l,
            .composite = one_to_one,
        };
        s_pending_composited = one_to_one;
        xQueueSend(s_band_q, &job, portMAX_DELAY);
    } else {
        ppa_srm_oper_config_t op = srm_op(l, src, src_cm, range, fb);
        esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_srm, &op);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PPA SRM failed: %s", esp_err_to_name(err));
            present_ready_buffer(true);
            return;
        }
    }
    s_srm_pending = true;

    present_ready_buffer(true);
    if (!background) {
        render_finish();
        present_ready_buffer(true);
    }
}

/*
 * Decode an MJPEG frame into `out` (s_dec_buf_size bytes) as RGB565.
 * Returns false if the frame is unusable.
 */
static bool decode_mjpeg(const uvc_host_frame_t *frame, uint8_t *out)
{
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(frame->data, frame->data_len, &info) != ESP_OK ||
            info.width == 0 || info.height == 0) {
        return false;
    }

    /* The decoder writes whole MCUs: width/height are padded to the MCU size */
    uint32_t mcu_w = 8, mcu_h = 8;
    if (info.sample_method == JPEG_DOWN_SAMPLING_YUV420) {
        mcu_w = 16;
        mcu_h = 16;
    } else if (info.sample_method == JPEG_DOWN_SAMPLING_YUV422) {
        mcu_w = 16;
    }
    uint32_t pic_w = ALIGN_UP(info.width, mcu_w);
    uint32_t pic_h = ALIGN_UP(info.height, mcu_h);
    if ((size_t)pic_w * pic_h * 2 > s_dec_buf_size) {
        ESP_LOGW(TAG, "JPEG %"PRIu32"x%"PRIu32" too large", info.width, info.height);
        return false;
    }

    const uint32_t expected = pic_w * pic_h * 2;
    const jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
#if CONFIG_HDMI_JPEG_SWAP_RB
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
#else
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
#endif
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_jpeg, &cfg, frame->data, frame->data_len, out, s_dec_buf_size, &out_size);
    if (err == ESP_OK && out_size != expected) {
        ESP_LOGW(TAG, "Unexpected JPEG output size %"PRIu32" (expected %"PRIu32")", out_size, expected);
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        return false;
    }
    update_layout(info.width, info.height, pic_w, pic_h);
    return true;
}

/*
 * Pick the number of refreshes per shown frame. Showing frames faster than the source delivers
 * them is pointless, and the pipeline must finish a frame within the slot (decode and render run
 * in parallel, so the slower of the two counts). Recomputed every statistics period.
 */
static void update_pacing(float src_fps, int64_t frame_cost_us)
{
#if CONFIG_HDMI_PACING_FIXED
    s_pace = CONFIG_HDMI_PACING_REFRESHES;
#elif CONFIG_HDMI_PACING_AUTO
    const uint32_t period = s_vsync_period_us;
    uint32_t by_source = 1;
    if (src_fps > 0) {
        const float refresh_hz = 1e6f / period;
        by_source = (uint32_t)(refresh_hz / src_fps + 0.9f);   /* ceil with ~10 % tolerance */
    }
    /* 1 ms margin for overlay, flip and task switches */
    uint32_t by_cost = (uint32_t)((frame_cost_us + 1000 + period - 1) / period);
    /*
     * Keep the source's cadence while the frame cost is within 10 % of its slot: an occasional late
     * frame (shown one refresh longer) is less visible than dropping every third frame of a 30 fps
     * source at 3 refreshes per frame.
     */
    if (by_cost > by_source && frame_cost_us + 1000 <= (int64_t)by_source * period * 11 / 10) {
        by_cost = by_source;
    }
    const uint32_t pace = (uint32_t)clampi((int)(by_source > by_cost ? by_source : by_cost), 1, 6);
    if (pace != s_pace) {
        ESP_LOGI(TAG, "Pacing: %"PRIu32" refreshes per frame (%.1f fps)", pace, 1e6f / (period * pace));
        s_pace = pace;
    }
#else
    s_pace = 1;
#endif
}

/*
 * Render task (core 1): shows the newest frame of the capture stream, or the still screen while there is
 * no moving picture or a full-screen page hides it. Logs statistics every 5 s and adapts the pacing.
 */
static void render_task(void *arg)
{
    uint32_t frames = 0, bad = 0, dropped = 0;
    float fmt_fps = 0;
    int64_t t_last = esp_timer_get_time();

    while (true) {
        uvc_host_frame_t *frame = NULL;
        /* Without a moving picture the still screen is redrawn whenever the overlay changes */
        const bool still = !s_stream_active || ui_fullscreen() ||
                           esp_timer_get_time() - s_last_frame_us > VIDEO_TIMEOUT_US;
        /* Look again soon while a frame is in flight (to present it) or the still screen may need redrawing */
        const TickType_t wait = pdMS_TO_TICKS(s_srm_pending || still ? 5 : 100);
        if (xQueueReceive(s_frame_q, &frame, wait) != pdTRUE) {
            /* No new frame yet: complete the one in flight so it becomes visible */
            render_finish();
            present_ready_buffer(true);
            if (still && (s_need_blank || overlay_generation() != s_blank_gen)) {
                s_need_blank = false;
                render_blank();
            }
            continue;
        }

        /* Held until the frame is back with the driver, so that close_stream() cannot change the stream meanwhile */
        xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
        if (s_stream_active) {
            s_last_frame_us = esp_timer_get_time();
        }
        if (!s_stream_active || ui_fullscreen()) {
            /* A full-screen page hides the picture: hand frames straight back */
            uvc_host_frame_return(s_stream, frame);
            xSemaphoreGive(s_stream_mutex);
            if (s_stream_active && overlay_generation() != s_blank_gen) {
                render_blank();
            }
            continue;
        }

        /* Always show the newest frame to keep latency low */
        uvc_host_frame_t *newer;
        while (xQueueReceive(s_frame_q, &newer, 0) == pdTRUE) {
            uvc_host_frame_return(s_stream, frame);
            frame = newer;
            dropped++;
        }

        const uvc_host_stream_format_t *fmt = &frame->vs_format;
        if (fmt->fps != fmt_fps) {
            fmt_fps = fmt->fps;
            update_pacing(fmt_fps, 0);  /* refined from measurements after the first period */
        }
        /* fmt is part of the frame, which goes back to the driver before an MJPEG frame is rendered */
        const bool mjpeg = fmt->format == UVC_VS_FORMAT_MJPEG;
        uint8_t *dec = s_dec_buf[s_dec_idx];
        bool ok;
        if (mjpeg) {
            /* Runs on the JPEG engine while the PPA still renders the previous frame from the other buffer */
            const int64_t td = esp_timer_get_time();
            ok = decode_mjpeg(frame, dec);
            s_t_decode += esp_timer_get_time() - td;
        } else {
            /* YUY2 is rendered straight from the frame, before it goes back */
            ok = fmt->format == UVC_VS_FORMAT_YUY2 && frame->data_len >= (size_t)fmt->h_res * fmt->v_res * 2 &&
                 (fmt->h_res % 2) == 0;
            if (ok) {
                update_layout(fmt->h_res, fmt->v_res, fmt->h_res, fmt->v_res);
                render_start(frame->data, PPA_SRM_COLOR_MODE_YUV422_YUYV, PPA_COLOR_RANGE_LIMIT, false);
            }
        }
        uvc_host_frame_return(s_stream, frame);
        xSemaphoreGive(s_stream_mutex);
        if (ok && mjpeg) {
            /* JPEG YCbCr is full range */
            render_start(dec, PPA_SRM_COLOR_MODE_RGB565, PPA_COLOR_RANGE_FULL, true);
            s_dec_idx ^= 1;
        }
        if (ok) {
            frames++;
        } else {
            bad++;
        }

        /* Every 5 s: averages per shown frame, then the pacing adjusted to the measured frame cost */
        int64_t now = esp_timer_get_time();
        if (now - t_last >= 5000000) {
            uint32_t n = frames ? frames : 1;
            ESP_LOGI(TAG, "%.1f fps shown (pace %"PRIu32"), %"PRIu32" dropped, %"PRIu32" bad | avg ms: decode %.1f, "
                     "render %.1f (overlay %.1f, lock wait %.1f), render wait %.1f, vsync wait %.1f, pace wait %.1f, flip %.1f",
                     frames * 1e6 / (double)(now - t_last), s_pace, dropped, bad,
                     s_t_decode / 1000.0 / n, s_t_render / 1000.0 / n, s_t_overlay / 1000.0 / n,
                     s_t_overlay_wait / 1000.0 / n, s_t_srm / 1000.0 / n,
                     s_t_wait / 1000.0 / n, s_t_pace / 1000.0 / n, s_t_present / 1000.0 / n);
            if (frames > 0) {
                update_pacing(fmt_fps, (s_t_decode > s_t_render ? s_t_decode : s_t_render) / n);
            }
            ESP_LOGI(TAG, "Bands avg ms: GDMA wait %.1f, PPA submit %.1f, PPA wait %.1f | internal RAM free %u KB (lowest %u KB)",
                     s_t_gdma_wait / 1000.0 / n, s_t_ppa_submit / 1000.0 / n, s_t_ppa_wait / 1000.0 / n,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
            s_t_gdma_wait = s_t_ppa_submit = s_t_ppa_wait = 0;
            frames = bad = dropped = 0;
            s_t_decode = s_t_wait = s_t_srm = s_t_present = s_t_pace = s_t_render = s_t_overlay = s_t_overlay_wait = 0;
            t_last = now;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* UVC                                                                       */
/* ------------------------------------------------------------------------- */

/* Name of a stream format, for the log and the UI */
static const char *format_name(enum uvc_host_stream_format f)
{
    switch (f) {
    case UVC_VS_FORMAT_MJPEG: return "MJPEG";
    case UVC_VS_FORMAT_YUY2:  return "YUY2";
    case UVC_VS_FORMAT_H264:  return "H264";
    case UVC_VS_FORMAT_H265:  return "H265";
    case UVC_VS_FORMAT_NV12:  return "NV12";
    default:                  return "?";
    }
}

/* Frame rate of a UVC frame interval (in 100 ns units); 0 for an interval of 0 */
static float interval_to_fps(uint32_t interval)
{
    return interval ? 10000000.0f / (float)interval : 0.0f;
}

/* Frame rate to ask for: the supported one closest to CONFIG_HDMI_PREFERRED_FPS; 0 (device default) if unknown */
static float pick_fps(const uvc_host_frame_info_t *f)
{
    const float pref = CONFIG_HDMI_PREFERRED_FPS;
    if (f->interval_type == 0) {
        if (f->interval_min == 0) {
            return 0;
        }
        uint32_t iv = (uint32_t)(10000000.0f / pref);
        if (iv < f->interval_min) {
            iv = f->interval_min;
        }
        if (f->interval_max && iv > f->interval_max) {
            iv = f->interval_max;
        }
        return interval_to_fps(iv);
    }

    int n = f->interval_type < CONFIG_UVC_INTERVAL_ARRAY_SIZE ? f->interval_type : CONFIG_UVC_INTERVAL_ARRAY_SIZE;
    float best = 0, best_diff = 1e9f;
    for (int i = 0; i < n; i++) {
        float fps = interval_to_fps(f->interval[i]);
        float diff = fps > pref ? fps - pref : pref - fps;
        if (fps > 0 && diff < best_diff) {
            best_diff = diff;
            best = fps;
        }
    }
    return best ? best : interval_to_fps(f->default_interval);
}

/* Prefer MJPEG at the configured resolution, then 16:9, then the closest size; YUY2 as fallback */
static bool choose_format(const uvc_host_frame_info_t *list, size_t n, uvc_host_stream_format_t *out)
{
    const int64_t pref_w = CONFIG_HDMI_PREFERRED_WIDTH, pref_h = CONFIG_HDMI_PREFERRED_HEIGHT;
    int best = -1;
    int64_t best_score = INT64_MIN;

    for (size_t i = 0; i < n; i++) {
        const uvc_host_frame_info_t *f = &list[i];
        if (f->format != UVC_VS_FORMAT_MJPEG && f->format != UVC_VS_FORMAT_YUY2) {
            continue;
        }
        if (f->h_res < 16 || f->v_res < 16 || f->h_res > MAX_SRC_W || f->v_res > MAX_SRC_H) {
            continue;
        }
        int64_t w = f->h_res, h = f->v_res;
        int64_t score = 0;
        if (f->format == UVC_VS_FORMAT_MJPEG) {
            score += 1LL << 40;
        }
        if (w == pref_w && h == pref_h) {
            score += 1LL << 36;
        }
        if (w * 9 == h * 16) {
            score += 1LL << 34;
        }
        int64_t d = w * h - pref_w * pref_h;
        score -= d < 0 ? -d : d;
        if (score > best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    if (best < 0) {
        return false;
    }
    out->format = list[best].format;
    out->h_res = list[best].h_res;
    out->v_res = list[best].v_res;
    out->fps = pick_fps(&list[best]);
    return true;
}

/* UVC driver task: a capture device has connected; choose its format and ask the main task to open it */
static void uvc_driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        return;
    }
    const uint8_t addr = event->device_connected.dev_addr;
    const uint8_t idx = event->device_connected.uvc_stream_index;
    size_t n = event->device_connected.frame_info_num;
    ESP_LOGI(TAG, "UVC device connected: addr %u, stream %u, %u frame formats", addr, idx, (unsigned)n);
    if (n == 0) {
        return;
    }

    uvc_host_frame_info_t *list = calloc(n, sizeof(uvc_host_frame_info_t));
    if (!list) {
        return;
    }
    if (uvc_host_get_frame_list(addr, idx, (uvc_host_frame_info_t (*)[])list, &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            ESP_LOGI(TAG, "  %-5s %4ux%-4u default %.1f fps", format_name(list[i].format),
                     list[i].h_res, list[i].v_res, interval_to_fps(list[i].default_interval));
        }
        app_evt_t evt = {
            .type = APP_EVT_DEVICE_CONNECTED,
            .dev_addr = addr,
            .stream_index = idx,
        };
        if (choose_format(list, n, &evt.format)) {
            xQueueSend(s_app_evt_q, &evt, 0);
        } else {
            ESP_LOGE(TAG, "No usable MJPEG/YUY2 format (max %dx%d) on this device", MAX_SRC_W, MAX_SRC_H);
        }
    }
    free(list);
}

/* UVC driver task: stream events; a disconnection goes to the main task, which closes the stream */
static void uvc_stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB transfer error: %s", esp_err_to_name(event->transfer_error.error));
        break;
    case UVC_HOST_DEVICE_DISCONNECTED: {
        ESP_LOGW(TAG, "HDMI adapter disconnected");
        app_evt_t evt = {
            .type = APP_EVT_DEVICE_DISCONNECTED,
            .stream = event->device_disconnected.stream_hdl,
        };
        xQueueSend(s_app_evt_q, &evt, 0);
        break;
    }
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "Frame buffer overflow (frame larger than buffer)");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGD(TAG, "Frame buffer underflow (display busy, frame skipped)");
        break;
    default:
        break;
    }
}

/* UVC driver task: a complete frame, passed on to the render task */
static bool uvc_frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    if (xQueueSend(s_frame_q, &frame, 0) != pdTRUE) {
        return true;    /* not queued: hand the buffer straight back to the driver */
    }
    return false;       /* render_task returns it with uvc_host_frame_return() */
}

/* Main task: open and start the stream in the chosen format, retrying once at the device's default frame rate */
static void open_stream(const app_evt_t *evt)
{
    uvc_host_stream_format_t fmt = evt->format;
    const size_t frame_size = (fmt.format == UVC_VS_FORMAT_YUY2)
                              ? (size_t)fmt.h_res * fmt.v_res * 2
                              : (size_t)fmt.h_res * fmt.v_res; /* generous upper bound for MJPEG */

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1) {
            fmt.fps = 0;    /* retry with the device's default frame rate */
        }
        ESP_LOGI(TAG, "Opening %s %ux%u @ %.1f fps", format_name(fmt.format), fmt.h_res, fmt.v_res, fmt.fps);

        uvc_host_stream_config_t cfg = {
            .event_cb = uvc_stream_event_cb,
            .frame_cb = uvc_frame_cb,
            .user_ctx = NULL,
            .usb = {
                .dev_addr = evt->dev_addr,
                .vid = UVC_HOST_ANY_VID,
                .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = evt->stream_index,
            },
            .vs_format = fmt,
            .advanced = {
                .number_of_frame_buffers = UVC_FRAME_BUFFERS,
                .frame_size = frame_size,
                .frame_heap_caps = MALLOC_CAP_SPIRAM,
                .number_of_urbs = UVC_URBS,
                .urb_size = UVC_URB_SIZE,
            },
        };

        uvc_host_stream_hdl_t stream = NULL;
        esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(5000), &stream);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Stream open failed: %s", esp_err_to_name(err));
            continue;
        }

        xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
        s_stream = stream;
        s_stream_active = true;
        xSemaphoreGive(s_stream_mutex);

        err = uvc_host_stream_start(stream);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Streaming started");
            char info[40];
            snprintf(info, sizeof(info), "%ux%u %s %.0f fps", fmt.h_res, fmt.v_res, format_name(fmt.format), fmt.fps);
            ui_set_video(true, info);
            return;
        }
        ESP_LOGW(TAG, "Stream start failed: %s", esp_err_to_name(err));

        xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
        s_stream_active = false;
        s_stream = NULL;
        xSemaphoreGive(s_stream_mutex);
        uvc_host_stream_close(stream);
    }
    ESP_LOGE(TAG, "Could not start the HDMI capture stream");
}

/* Main task, after a disconnection: if it is our stream, stop rendering it, return its frames and close it */
static void close_stream(uvc_host_stream_hdl_t stream)
{
    if (!stream || stream != s_stream) {
        return;
    }

    /* Stop rendering and hand every frame we still hold back to the driver */
    xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
    s_stream_active = false;
    uvc_host_frame_t *frame;
    while (xQueueReceive(s_frame_q, &frame, 0) == pdTRUE) {
        uvc_host_frame_return(stream, frame);
    }
    xSemaphoreGive(s_stream_mutex);

    /* The render task may still be returning its current frame */
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 20; i++) {
        err = uvc_host_stream_close(stream);
        if (err != ESP_ERR_INVALID_STATE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream close failed: %s", esp_err_to_name(err));
    }

    xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
    s_stream = NULL;
    xSemaphoreGive(s_stream_mutex);
    s_need_blank = true;
    ui_set_video(false, NULL);
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

/* Average panel refresh period, needed for frame pacing */
static void measure_refresh(void)
{
    const int n = 30;
    /* Drop a stale event, then start timing at a refresh boundary */
    xSemaphoreTake(s_vsync_sem, 0);
    if (xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "No VSYNC events, assuming 60 Hz");
        return;
    }
    const uint32_t c0 = s_vsync_count;
    const int64_t t0 = esp_timer_get_time();
    if (!wait_vsync(c0 + n)) {
        ESP_LOGW(TAG, "VSYNC events stopped, assuming 60 Hz");
        return;
    }
    s_vsync_period_us = (uint32_t)((esp_timer_get_time() - t0) / (s_vsync_count - c0));
    ESP_LOGI(TAG, "Panel refresh: %.2f Hz", 1e6 / s_vsync_period_us);
}

/*
 * Set up the band renderer: its PPA client, the AXI GDMA, three band buffers in internal RAM, the blit
 * task (core 0) and the overlay helper (core 1). Without CONFIG_HDMI_BAND_RENDER, the GDMA or the RAM,
 * s_band_buf_size stays 0 and every layout is rendered directly.
 */
static void band_init(void)
{
    const ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&cfg, &s_ppa_band_nb));
    s_band_ppa_sem = xSemaphoreCreateBinary();
    assert(s_band_ppa_sem);
    const ppa_event_callbacks_t band_cbs = {
        .on_trans_done = on_band_ppa_done,
    };
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(s_ppa_band_nb, &band_cbs));

#if CONFIG_HDMI_BAND_RENDER
    async_memcpy_config_t mcp_cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    mcp_cfg.dma_burst_size = 64;
    if (esp_async_memcpy_install_gdma_axi(&mcp_cfg, &s_mcp) != ESP_OK) {
        mcp_cfg.dma_burst_size = 16;
        if (esp_async_memcpy_install_gdma_axi(&mcp_cfg, &s_mcp) != ESP_OK) {
            ESP_LOGW(TAG, "No AXI GDMA for band copies, band mode disabled");
            return;
        }
    }

    /* Three band buffers in internal SRAM, sized for 1280-pixel rows; smaller if memory is short */
    for (uint32_t rows = CONFIG_HDMI_BAND_ROWS & ~1u; rows >= 4; rows -= rows > 8 ? 8 : 4) {
        const size_t size = (size_t)rows * VIEW_W * 2;
        bool all = true;
        for (int b = 0; b < BAND_BUFS; b++) {
            s_band_buf[b] = heap_caps_aligned_calloc(128, 1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
            all &= s_band_buf[b] != NULL;
        }
        if (all) {
            s_band_buf_size = size;
            break;
        }
        for (int b = 0; b < BAND_BUFS; b++) {
            heap_caps_free(s_band_buf[b]);
            s_band_buf[b] = NULL;
        }
    }
    if (!s_band_buf_size) {
        ESP_LOGW(TAG, "Not enough internal RAM for band buffers, band mode disabled");
        return;
    }
    for (int b = 0; b < BAND_BUFS; b++) {
        s_copy_sem[b] = xSemaphoreCreateBinary();
        assert(s_copy_sem[b]);
        xSemaphoreGive(s_copy_sem[b]);
    }
    s_band_q = xQueueCreate(1, sizeof(band_job_t));
    assert(s_band_q);
    BaseType_t ok = xTaskCreatePinnedToCore(blit_task, "blit", 4 * 1024, NULL, 6, NULL, 0);
    assert(ok == pdPASS);
    s_ov_done = xSemaphoreCreateBinary();
    assert(s_ov_done);
    ok = xTaskCreatePinnedToCore(overlay_helper_task, "ov_help", 3 * 1024, NULL, 6, &s_ov_helper, 1);
    assert(ok == pdPASS);
    ESP_LOGI(TAG, "Band mode: %d x %u KB internal RAM, %u KB internal free", BAND_BUFS,
             (unsigned)(s_band_buf_size / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
#endif
}

/*
 * Live view on: powers the USB-A port, starting the USB host and the UVC driver the first time.
 * Off (remote only): the port's 5 V go off, so the adapter disconnects and its stream closes.
 * Runs in the main task, which owns the BSP's IO expander after start-up.
 */
static bool s_usb_started;

static void live_view_apply(bool on)
{
    if (on && !s_usb_started) {
        /* Powers the USB-A port (5 V) and installs the USB host library on the HS controller */
        ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true));
        const uvc_host_driver_config_t uvc_cfg = {
            .driver_task_stack_size = 6 * 1024,
            .driver_task_priority = 11,
            .xCoreID = 0,
            .create_background_task = true,
            .event_cb = uvc_driver_event_cb,
        };
        ESP_ERROR_CHECK(uvc_host_install(&uvc_cfg));
        s_usb_started = true;
        ESP_LOGI(TAG, "Waiting for HDMI capture adapter on USB-A...");
    } else if (s_usb_started) {
        bsp_feature_enable(BSP_FEATURE_USB, on);
        ESP_LOGI(TAG, "Live view %s", on ? "on: USB-A powered" : "off: USB-A switched off");
    } else {
        ESP_LOGI(TAG, "Live view off: remote only, USB-A not powered");
    }
}

/* Whether a USB device is still there: a connection event can be older than the device's leaving again */
static bool device_present(uint8_t addr)
{
    uint8_t addrs[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) != ESP_OK) {
        return true;
    }
    for (int i = 0; i < n; i++) {
        if (addrs[i] == addr) {
            return true;
        }
    }
    ESP_LOGI(TAG, "HDMI adapter at address %u has left again: waiting for it", addr);
    return false;
}

/* From the UI task: the main task applies the change */
static void live_view_request(bool on)
{
    const app_evt_t evt = {.type = APP_EVT_LIVE_VIEW, .on = on};
    xQueueSend(s_app_evt_q, &evt, 0);
}

/*
 * Start-up. Afterwards the main task handles the events: it opens and closes the capture stream and
 * switches the live view (USB-A power).
 */
void app_main(void)
{
    /* NVS keeps the camera pairing and the display settings */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    /*
     * Bluetooth's ESP32-C6 and the HDMI adapter first: both take seconds to start, which now pass while
     * the display starts. The adapter appears on USB about 1.5 s after it is powered, leaves again and is
     * back 1.5 s later; the event loop below only opens it once everything else is ready.
     */
    cam_init();
    cam_ble_start();
    s_frame_q = xQueueCreate(UVC_FRAME_BUFFERS, sizeof(uvc_host_frame_t *));
    s_app_evt_q = xQueueCreate(8, sizeof(app_evt_t));
    s_stream_mutex = xSemaphoreCreateMutex();
    assert(s_frame_q && s_app_evt_q && s_stream_mutex);
    live_view_apply(ui_live_view());
    display_init();
    overlay_init();

    const ppa_client_config_t srm_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&srm_cfg, &s_ppa_srm));
    s_srm_done_sem = xSemaphoreCreateBinary();
    assert(s_srm_done_sem);
    const ppa_event_callbacks_t srm_cbs = {
        .on_trans_done = on_srm_done,
    };
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(s_ppa_srm, &srm_cbs));
    const ppa_client_config_t fill_cfg = {
        .oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&fill_cfg, &s_ppa_fill));

    const jpeg_decode_engine_cfg_t jpeg_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&jpeg_cfg, &s_jpeg));
    const jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    for (int i = 0; i < 2; i++) {
        s_dec_buf[i] = jpeg_alloc_decoder_mem(MAX_SRC_W * MAX_SRC_H * 2, &mem_cfg, &s_dec_buf_size);
        assert(s_dec_buf[i]);
    }

    measure_refresh();
    band_init();
    /* Layout of the still screen; its band_rows are 0 if band mode is unavailable */
    compute_layout(VIEW_W, VIEW_H, VIEW_W, VIEW_H, &s_blank_layout);

    ui_start();

    BaseType_t ok = xTaskCreatePinnedToCore(render_task, "render", 6 * 1024, NULL, 5, NULL, 1);
    assert(ok == pdPASS);

    ui_set_live_view_handler(live_view_request);

    while (true) {
        app_evt_t evt;
        if (xQueueReceive(s_app_evt_q, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt.type) {
        case APP_EVT_DEVICE_CONNECTED:
            if (s_stream == NULL && device_present(evt.dev_addr)) {
                open_stream(&evt);
            }
            break;
        case APP_EVT_DEVICE_DISCONNECTED:
            close_stream(evt.stream);
            break;
        case APP_EVT_LIVE_VIEW:
            live_view_apply(evt.on);
            break;
        }
    }
}
