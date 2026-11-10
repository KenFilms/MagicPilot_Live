/*
 * Overlay layer: canvas, run-length encoding of changed rows, compositing. See overlay.h.
 *
 * Each row's runs and literal pixels live in one block, so compositing a row is one read of a
 * table in internal RAM and one sequential read of the block (PSRAM latency, not bandwidth, was
 * the cost when runs and pixels were spread over several allocations). Every row has two blocks:
 * a commit builds the changed rows in their spare blocks, re-encoding only the changed 256-pixel
 * zones of the canvas and copying the others, then swaps them in with the lock held for a few
 * microseconds. Drawing therefore never stalls the renderer, which holds the lock while it
 * composites.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "overlay.h"

static const char *TAG = "overlay";

#define OV_ZONE_W       256                     /* re-encoding granularity */
#define OV_ZONES        (OV_W / OV_ZONE_W)
#define OV_LIT          0x80000000u             /* run value: offset of its pixels among the row's literal pixels */
#define OV_MIN_UNIFORM  4                       /* shorter stretches of equal pixels stay literal, unless isolated */

typedef struct {
    uint16_t x, len;
    uint32_t px;                                /* the run's pixel value, or OV_LIT | offset */
} ov_run_t;

typedef struct {
    uint16_t n;                                 /* runs, followed in memory by the literal pixels */
    uint16_t zone_first[OV_ZONES + 1];          /* first run of each zone */
    ov_run_t runs[];
} ov_block_t;

typedef struct {
    ov_block_t *blk[2];
    uint32_t cap[2];                            /* block sizes in bytes */
    uint8_t active;                             /* the block swapped in last; the other one is the spare */
} ov_row_t;

static ov_px_t *s_canvas;                       /* PSRAM: the canvas the UI draws into */
static ov_row_t *s_rows;                        /* PSRAM: both blocks of every row */
static const ov_block_t *s_active[OV_H];        /* internal RAM: the block the compositor reads, NULL = empty */
static const ov_block_t **s_next;              /* PSRAM: the same after the commit in progress */
static uint8_t s_dirty[OV_H];                   /* zones of each row to re-encode */
static uint16_t *s_changed;                     /* PSRAM: rows rebuilt by the commit in progress */
static ov_run_t *s_tmp_runs;                    /* one row being assembled */
static ov_px_t *s_tmp_lit;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_gen;

void overlay_init(void)
{
    s_canvas = heap_caps_aligned_alloc(64, (size_t)OV_W * OV_H * sizeof(ov_px_t), MALLOC_CAP_SPIRAM);
    s_rows = heap_caps_calloc(OV_H, sizeof(ov_row_t), MALLOC_CAP_SPIRAM);
    s_tmp_runs = heap_caps_malloc(OV_W * sizeof(ov_run_t), MALLOC_CAP_SPIRAM);
    s_tmp_lit = heap_caps_malloc(OV_W * sizeof(ov_px_t), MALLOC_CAP_SPIRAM);
    s_next = heap_caps_calloc(OV_H, sizeof(*s_next), MALLOC_CAP_SPIRAM);
    s_changed = heap_caps_calloc(OV_H, sizeof(*s_changed), MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    assert(s_canvas && s_rows && s_tmp_runs && s_tmp_lit && s_next && s_changed && s_lock);
    for (size_t i = 0; i < (size_t)OV_W * OV_H; i++) {
        s_canvas[i] = OV_CLEAR;
    }
}

ov_px_t *overlay_row(int y)
{
    return s_canvas + (size_t)y * OV_W;
}

void overlay_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void overlay_unlock(void)
{
    xSemaphoreGive(s_lock);
}

uint32_t overlay_generation(void)
{
    return s_gen;
}

bool overlay_row_empty(int y)
{
    return s_active[y] == NULL;
}

void overlay_mark_dirty(int x, int y, int w, int h)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > OV_W) {
        w = OV_W - x;
    }
    if (y + h > OV_H) {
        h = OV_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    /* Bits z0..z1: the zones the area overlaps */
    const int z0 = x / OV_ZONE_W, z1 = (x + w - 1) / OV_ZONE_W;
    const uint8_t mask = (uint8_t)(((1u << (z1 + 1)) - 1) & ~((1u << z0) - 1));
    for (int r = y; r < y + h; r++) {
        s_dirty[r] |= mask;
    }
}

/* A block's literal pixels, stored right after its runs */
static inline const ov_px_t *block_lit(const ov_block_t *b)
{
    return (const ov_px_t *)(b->runs + b->n);
}

/*
 * Appends the runs of one zone of a canvas row: uniform runs for stretches of equal pixels
 * (long ones, or isolated ones such as thin lines), literal runs for the rest; transparent
 * pixels get none.
 */
static void encode_zone(const ov_px_t *row, int z, int *n, int *nlit)
{
    const int x0 = z * OV_ZONE_W, x1 = x0 + OV_ZONE_W;
    int x = x0;
    while (x < x1) {
        const ov_px_t px = row[x];
        if (px == OV_CLEAR) {
            x++;
            continue;
        }
        int e = x + 1;
        while (e < x1 && row[e] == px) {
            e++;
        }
        const bool isolated = (x == x0 || row[x - 1] == OV_CLEAR) && (e == x1 || row[e] == OV_CLEAR);
        if (e - x >= OV_MIN_UNIFORM || isolated) {
            s_tmp_runs[(*n)++] = (ov_run_t){ (uint16_t)x, (uint16_t)(e - x), px };
            x = e;
            continue;
        }
        /* Literal run: up to the next transparent pixel or the next long uniform stretch */
        const int s = x;
        x = e;
        while (x < x1 && row[x] != OV_CLEAR) {
            e = x + 1;
            while (e < x1 && row[e] == row[x] && e - x < OV_MIN_UNIFORM) {
                e++;
            }
            if (e - x >= OV_MIN_UNIFORM) {
                break;
            }
            x = e;
        }
        s_tmp_runs[(*n)++] = (ov_run_t){ (uint16_t)s, (uint16_t)(x - s), OV_LIT | (uint32_t)*nlit };
        memcpy(s_tmp_lit + *nlit, row + s, (x - s) * sizeof(ov_px_t));
        *nlit += x - s;
    }
}

/* Builds row y in its spare block: dirty zones from the canvas, the others copied from the active block */
static void build_row(int y, uint8_t dirty)
{
    ov_row_t *r = &s_rows[y];
    /* What the row shows now (NULL: nothing, even if an old block is still allocated) */
    const ov_block_t *old = s_active[y];
    const ov_px_t *old_lit = old ? block_lit(old) : NULL;
    uint16_t first[OV_ZONES + 1];
    int n = 0, nlit = 0;
    for (int z = 0; z < OV_ZONES; z++) {
        first[z] = (uint16_t)n;
        if ((dirty & (1u << z)) || !old) {
            encode_zone(overlay_row(y), z, &n, &nlit);
            continue;
        }
        for (int i = old->zone_first[z]; i < old->zone_first[z + 1]; i++) {
            ov_run_t run = old->runs[i];
            if (run.px & OV_LIT) {
                memcpy(s_tmp_lit + nlit, old_lit + (run.px & 0xFFFF), run.len * sizeof(ov_px_t));
                run.px = OV_LIT | (uint32_t)nlit;
                nlit += run.len;
            }
            s_tmp_runs[n++] = run;
        }
    }
    first[OV_ZONES] = (uint16_t)n;

    /* The compositor never reads the spare block: s_active[y] is the other block or NULL */
    const int spare = r->active ^ 1;
    if (n == 0) {
        s_next[y] = NULL;
        return;
    }
    const uint32_t size = sizeof(ov_block_t) + n * sizeof(ov_run_t) + nlit * sizeof(ov_px_t);
    /* Blocks only grow, in 256-byte steps, so that most commits allocate nothing */
    if (size > r->cap[spare]) {
        const uint32_t cap = (size + 255) & ~255u;
        ov_block_t *b = heap_caps_realloc(r->blk[spare], cap, MALLOC_CAP_SPIRAM);
        if (!b) {
            ESP_LOGE(TAG, "Out of memory for row %d", y);
            s_next[y] = NULL;
            return;
        }
        r->blk[spare] = b;
        r->cap[spare] = cap;
    }
    ov_block_t *b = r->blk[spare];
    b->n = (uint16_t)n;
    memcpy(b->zone_first, first, sizeof(first));
    memcpy(b->runs, s_tmp_runs, n * sizeof(ov_run_t));
    memcpy((ov_px_t *)block_lit(b), s_tmp_lit, nlit * sizeof(ov_px_t));
    s_next[y] = b;
}

void overlay_commit(void)
{
    int changed = 0;
    for (int y = 0; y < OV_H; y++) {
        if (s_dirty[y]) {
            build_row(y, s_dirty[y]);
            s_dirty[y] = 0;
            s_changed[changed++] = (uint16_t)y;
        }
    }
    if (!changed) {
        return;
    }
    /* Swap in the new rows; the compositor only reads them with the lock held */
    overlay_lock();
    for (int i = 0; i < changed; i++) {
        const int y = s_changed[i];
        if (s_next[y]) {
            s_rows[y].active ^= 1;
        }
        s_active[y] = s_next[y];
    }
    s_gen++;
    overlay_unlock();
}

/* ---- Compositing ---- */

/* A canvas pixel over a picture pixel: colour + picture * weight / 32 */
static inline uint16_t blend_px(uint16_t dst, ov_px_t px)
{
    const uint32_t w = px >> 16;
    return w == 0 ? (uint16_t)px : (uint16_t)(px + ov_scale565(dst, w));
}

/* Black shade leaving 1/4, 1/2 or 3/4 of the picture: shifts on two pixels per word */
static inline uint32_t shade2(uint32_t v, uint32_t w)
{
    switch (w) {
    case 8:  return (v >> 2) & 0x39E739E7u;
    case 16: return (v >> 1) & 0x7BEF7BEFu;
    default: return v - ((v >> 2) & 0x39E739E7u);     /* 24: no borrow, each field loses its own quarter */
    }
}

/* Shades n words (2 pixels each), unrolled; one loop per shade so the shift is not re-decided per word */
#define SHADE_LOOP(W)                                                           \
    do {                                                                        \
        uint32_t i = 0;                                                         \
        for (; i + 4 <= n2; i += 4) {                                           \
            const uint32_t a = w32[i], b = w32[i + 1], c = w32[i + 2], e = w32[i + 3]; \
            w32[i] = shade2(a, W);                                              \
            w32[i + 1] = shade2(b, W);                                          \
            w32[i + 2] = shade2(c, W);                                          \
            w32[i + 3] = shade2(e, W);                                          \
        }                                                                       \
        for (; i < n2; i++) {                                                   \
            w32[i] = shade2(w32[i], W);                                         \
        }                                                                       \
    } while (0)

/* Shades n2 words (two pixels each, 4-byte aligned) with a weight of 8, 16 or 24 */
static void shade_words(uint32_t *w32, uint32_t n2, uint32_t w)
{
    if (w == 8) {
        SHADE_LOOP(8);
    } else if (w == 16) {
        SHADE_LOOP(16);
    } else {
        SHADE_LOOP(24);
    }
}

/* Composites a uniform run over n picture pixels; opaque runs and the black shades take fast paths */
static void blend_uniform(uint16_t *d, uint32_t n, ov_px_t px)
{
    const uint32_t w = px >> 16;
    const uint16_t c = (uint16_t)px;
    if (w == 0) {
        for (uint32_t i = 0; i < n; i++) {
            d[i] = c;
        }
    } else if (c == 0 && (w == 8 || w == 16 || w == 24)) {
        /* Word access: a first pixel off the 4-byte boundary, and an odd last one, are shaded on their own */
        if ((uintptr_t)d & 2) {
            *d = (uint16_t)shade2(*d, w);
            d++;
            n--;
        }
        shade_words((uint32_t *)d, n / 2, w);
        if (n & 1) {
            d[n - 1] = (uint16_t)shade2(d[n - 1], w);
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            d[i] = (uint16_t)(c + ov_scale565(d[i], w));
        }
    }
}

void overlay_composite_row(uint16_t *row, int y)
{
    const ov_block_t *b = s_active[y];
    if (!b) {
        return;
    }
    const ov_px_t *lit = block_lit(b);
    for (int i = 0; i < b->n; i++) {
        const ov_run_t r = b->runs[i];
        uint16_t *d = row + r.x;
        if (!(r.px & OV_LIT)) {
            blend_uniform(d, r.len, r.px);
            continue;
        }
        const ov_px_t *s = lit + (r.px & 0xFFFF);
        for (uint32_t k = 0; k < r.len; k++) {
            d[k] = blend_px(d[k], s[k]);
        }
    }
}

void overlay_composite_fb(uint16_t *fb, int fb_w, int fb_h, bool rot270)
{
    /* Work through the view in vertical strips so the frame buffer lines stay in the cache */
    const int strip = 64;
    for (int x0 = 0; x0 < OV_W; x0 += strip) {
        const int x1 = x0 + strip;
        /* A strip lies inside one zone and runs never cross zones, so only that zone's runs can reach it */
        const int z = x0 / OV_ZONE_W;
        for (int y = 0; y < OV_H; y++) {
            const ov_block_t *b = s_active[y];
            if (!b) {
                continue;
            }
            const int fx = rot270 ? fb_w - 1 - y : y;
            const ov_px_t *lit = block_lit(b);
            for (int i = b->zone_first[z]; i < b->zone_first[z + 1]; i++) {
                const ov_run_t r = b->runs[i];
                const int a = r.x > x0 ? r.x : x0;
                const int e = r.x + r.len < x1 ? r.x + r.len : x1;
                for (int x = a; x < e; x++) {
                    const int fy = rot270 ? x : fb_h - 1 - x;
                    uint16_t *d = fb + fy * fb_w + fx;
                    const ov_px_t px = (r.px & OV_LIT) ? lit[(r.px & 0xFFFF) + (x - r.x)] : r.px;
                    *d = blend_px(*d, px);
                }
            }
        }
    }
}
