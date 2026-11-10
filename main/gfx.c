/*
 * Drawing primitives for the overlay canvas. See gfx.h.
 *
 * Shapes are drawn from signed distance functions: each pixel's coverage is 0.5 - distance to the
 * outline, clamped to 0..1, which gives one pixel of anti-aliasing at every edge.
 */

#include <math.h>
#include <string.h>
#include "gfx.h"
#include "overlay.h"

static int s_cx0 = 0, s_cy0 = 0, s_cx1 = OV_W, s_cy1 = OV_H;   /* clip rectangle, x1 and y1 exclusive */

void gfx_clip(int x, int y, int w, int h)
{
    s_cx0 = x < 0 ? 0 : x;
    s_cy0 = y < 0 ? 0 : y;
    s_cx1 = x + w > OV_W ? OV_W : x + w;
    s_cy1 = y + h > OV_H ? OV_H : y + h;
}

void gfx_clip_reset(void)
{
    gfx_clip(0, 0, OV_W, OV_H);
}

/* Clips a rectangle to the clip area; false if nothing is left */
static bool clip_rect(int *x, int *y, int *w, int *h)
{
    int x0 = *x < s_cx0 ? s_cx0 : *x;
    int y0 = *y < s_cy0 ? s_cy0 : *y;
    int x1 = *x + *w > s_cx1 ? s_cx1 : *x + *w;
    int y1 = *y + *h > s_cy1 ? s_cy1 : *y + *h;
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return true;
}

/* Colour premultiplied by a weight 0..32, as RGB565 */
static inline uint32_t premul(uint32_t rgb, uint32_t wa)
{
    const uint32_t r = (((rgb >> 19) & 31) * wa) >> 5;
    const uint32_t g = (((rgb >> 10) & 63) * wa) >> 5;
    const uint32_t b = (((rgb >> 3) & 31) * wa) >> 5;
    return (r << 11) | (g << 5) | b;
}

/* Paints rgb with opacity a8 (0..255) over a canvas pixel */
static inline void blend(ov_px_t *p, uint32_t rgb, uint32_t a8)
{
    const uint32_t wa = (a8 * 32 + 127) / 255;
    if (!wa) {
        return;
    }
    const uint32_t inv = 32 - wa;
    const ov_px_t px = *p;
    /* Over: the weight left for the picture and the colour so far both keep inv / 32 */
    *p = ((((px >> 16) * inv) >> 5) << 16) | (uint16_t)(premul(rgb, wa) + ov_scale565((uint16_t)px, inv));
}

void gfx_clear(int x, int y, int w, int h)
{
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }
    for (int r = y; r < y + h; r++) {
        ov_px_t *p = overlay_row(r) + x;
        for (int i = 0; i < w; i++) {
            p[i] = OV_CLEAR;
        }
    }
    overlay_mark_dirty(x, y, w, h);
}

void gfx_set(int x, int y, int w, int h, uint32_t rgb, uint8_t a)
{
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }
    const uint32_t wa = (a * 32u + 127) / 255;
    const ov_px_t v = ((32 - wa) << 16) | premul(rgb, wa);
    for (int r = y; r < y + h; r++) {
        ov_px_t *p = overlay_row(r) + x;
        for (int i = 0; i < w; i++) {
            p[i] = v;
        }
    }
    overlay_mark_dirty(x, y, w, h);
}

void gfx_fill(int x, int y, int w, int h, uint32_t rgb, uint8_t a)
{
    if (a == 255) {
        gfx_set(x, y, w, h, rgb, a);
        return;
    }
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }
    for (int r = y; r < y + h; r++) {
        ov_px_t *p = overlay_row(r) + x;
        for (int i = 0; i < w; i++) {
            blend(&p[i], rgb, a);
        }
    }
    overlay_mark_dirty(x, y, w, h);
}

/* v limited to 0..1 */
static inline float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

/* Signed distance from a pixel centre to a rounded box (negative inside) */
static inline float rbox_sdf(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    const float qx = fabsf(px - cx) - (hw - r);
    const float qy = fabsf(py - cy) - (hh - r);
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    const float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

/* Paints the pixels of a bounding box with the coverage computed by cov(px, py) */
#define SHAPE_LOOP(bx, by, bw, bh, COV)                                         \
    do {                                                                        \
        int x_ = (bx), y_ = (by), w_ = (bw), h_ = (bh);                         \
        if (!clip_rect(&x_, &y_, &w_, &h_)) {                                   \
            return;                                                             \
        }                                                                       \
        for (int yy = y_; yy < y_ + h_; yy++) {                                 \
            ov_px_t *row_ = overlay_row(yy);                                    \
            const float py = yy + 0.5f;                                         \
            for (int xx = x_; xx < x_ + w_; xx++) {                             \
                const float px = xx + 0.5f;                                     \
                const float c_ = (COV);                                         \
                if (c_ > 0.004f) {                                              \
                    blend(&row_[xx], rgb, (uint32_t)(a * c_ + 0.5f));           \
                }                                                               \
            }                                                                   \
        }                                                                       \
        overlay_mark_dirty(x_, y_, w_, h_);                                     \
    } while (0)

/* Corner radius for a w x h box: at most half its shorter side */
static float rrect_radius(int r, float hw, float hh)
{
    const float m = hw < hh ? hw : hh;
    return r < m ? r : m;
}

void gfx_rrect(int x, int y, int w, int h, int r, uint32_t rgb, uint8_t a)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (r <= 0) {
        gfx_fill(x, y, w, h, rgb, a);
        return;
    }
    const float hw = w / 2.0f, hh = h / 2.0f, rr = rrect_radius(r, hw, hh);
    const float cx = x + hw, cy = y + hh;
    SHAPE_LOOP(x, y, w, h, clamp01(0.5f - rbox_sdf(px, py, cx, cy, hw, hh, rr)));
}

void gfx_rrect_stroke(int x, int y, int w, int h, int r, float t, uint32_t rgb, uint8_t a)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    const float hw = w / 2.0f, hh = h / 2.0f, rr = rrect_radius(r, hw, hh);
    const float ri = rr - t > 0 ? rr - t : 0;
    const float cx = x + hw, cy = y + hh;
    SHAPE_LOOP(x, y, w, h,
               clamp01(0.5f - rbox_sdf(px, py, cx, cy, hw, hh, rr)) -
               clamp01(0.5f - rbox_sdf(px, py, cx, cy, hw - t, hh - t, ri)));
}

void gfx_circle(float cx, float cy, float r, uint32_t rgb, uint8_t a)
{
    const int x0 = (int)floorf(cx - r - 1), y0 = (int)floorf(cy - r - 1);
    const int n = (int)ceilf(2 * r + 2);
    SHAPE_LOOP(x0, y0, n, n, clamp01(0.5f - (sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r)));
}

void gfx_ring(float cx, float cy, float r, float t, uint32_t rgb, uint8_t a)
{
    const int x0 = (int)floorf(cx - r - 1), y0 = (int)floorf(cy - r - 1);
    const int n = (int)ceilf(2 * r + 2);
    const float mid = r - t / 2;
    SHAPE_LOOP(x0, y0, n, n, clamp01(0.5f - (fabsf(sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - mid) - t / 2)));
}

/* Distance from a point to the segment a-b */
static inline float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    const float vx = bx - ax, vy = by - ay;
    const float len2 = vx * vx + vy * vy;
    float k = len2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0;
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    const float dx = px - (ax + k * vx), dy = py - (ay + k * vy);
    return sqrtf(dx * dx + dy * dy);
}

void gfx_line(float x0, float y0, float x1, float y1, float t, uint32_t rgb, uint8_t a)
{
    const float m = t / 2 + 1;
    const int bx = (int)floorf(fminf(x0, x1) - m), by = (int)floorf(fminf(y0, y1) - m);
    const int bw = (int)ceilf(fabsf(x1 - x0) + 2 * m) + 1, bh = (int)ceilf(fabsf(y1 - y0) + 2 * m) + 1;
    SHAPE_LOOP(bx, by, bw, bh, clamp01(0.5f - (seg_dist(px, py, x0, y0, x1, y1) - t / 2)));
}

/* Distance of a point outside a convex polygon given by its edge lines (negative inside) */
static inline float poly_dist(float px, float py, const float *ea, const float *eb, const float *ec, int n)
{
    float d = ea[0] * px + eb[0] * py + ec[0];
    for (int i = 1; i < n; i++) {
        const float di = ea[i] * px + eb[i] * py + ec[i];
        d = di > d ? di : d;
    }
    return d;
}

void gfx_poly(const float *xs, const float *ys, int n, uint32_t rgb, uint8_t a)
{
    if (n < 3 || n > GFX_POLY_MAX) {
        return;
    }
    /* Outward unit normal of every edge, whichever way round the corners go */
    float area = 0;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        area += xs[i] * ys[j] - xs[j] * ys[i];
    }
    const float sgn = area > 0 ? 1.0f : -1.0f;
    float ea[GFX_POLY_MAX], eb[GFX_POLY_MAX], ec[GFX_POLY_MAX];
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    int m = 0;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const float dx = xs[j] - xs[i], dy = ys[j] - ys[i], len = sqrtf(dx * dx + dy * dy);
        x0 = fminf(x0, xs[i]);
        x1 = fmaxf(x1, xs[i]);
        y0 = fminf(y0, ys[i]);
        y1 = fmaxf(y1, ys[i]);
        if (len < 1e-4f) {
            continue;
        }
        ea[m] = sgn * dy / len;
        eb[m] = -sgn * dx / len;
        ec[m] = -(ea[m] * xs[i] + eb[m] * ys[i]);
        m++;
    }
    if (m < 3) {
        return;
    }
    const int bx = (int)floorf(x0) - 1, by = (int)floorf(y0) - 1;
    const int bw = (int)ceilf(x1) - bx + 2, bh = (int)ceilf(y1) - by + 2;
    SHAPE_LOOP(bx, by, bw, bh, clamp01(0.5f - poly_dist(px, py, ea, eb, ec, m)));
}

/* ---- Text ---- */

/* Next code point of a UTF-8 string, advancing *s; sequences of up to 3 bytes, other bytes as they are */
static uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    if (c >= 0xE0 && p[0] && p[1]) {
        c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
        p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
        p += 1;
    }
    *s = (const char *)p;
    return c;
}

/* Glyph of a code point (binary search), NULL if the font does not have it */
static const gfx_glyph_t *find_glyph(const gfx_font_t *f, uint32_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp) {
            return &f->glyphs[mid];
        }
        if (f->glyphs[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

/* Paints a glyph with its bitmap's top left at (x, y); alpha maps the 4-bit coverage to opacity */
static void draw_glyph(const gfx_font_t *f, const gfx_glyph_t *g, int x, int y, uint32_t rgb, const uint8_t *alpha)
{
    int cx = x, cy = y, cw = g->w, ch = g->h;
    if (!clip_rect(&cx, &cy, &cw, &ch)) {
        return;
    }
    const int pitch = (g->w + 1) / 2;
    const uint8_t *bits = f->bits + g->off;
    for (int r = cy - y; r < cy - y + ch; r++) {
        ov_px_t *row = overlay_row(y + r);
        const uint8_t *src = bits + r * pitch;
        for (int c = cx - x; c < cx - x + cw; c++) {
            const uint32_t v = (src[c >> 1] >> ((c & 1) * 4)) & 15;
            if (v) {
                blend(&row[x + c], rgb, alpha[v]);
            }
        }
    }
    overlay_mark_dirty(cx, cy, cw, ch);
}

/* Coverage 0..15 of a glyph pixel, 0 outside the glyph */
static inline uint32_t glyph_cov(const uint8_t *bits, int pitch, int w, int h, int r, int c)
{
    if ((unsigned)r >= (unsigned)h || (unsigned)c >= (unsigned)w) {
        return 0;
    }
    return (bits[r * pitch + (c >> 1)] >> ((c & 1) * 4)) & 15;
}

/* One pixel of black around a glyph: its coverage spread to the neighbours, diagonals at 2/3 */
static void draw_outline(const gfx_font_t *f, const gfx_glyph_t *g, int x, int y, const uint8_t *alpha)
{
    int cx = x - 1, cy = y - 1, cw = g->w + 2, ch = g->h + 2;
    if (!clip_rect(&cx, &cy, &cw, &ch)) {
        return;
    }
    const int pitch = (g->w + 1) / 2;
    const uint8_t *bits = f->bits + g->off;
    for (int r = cy - y; r < cy - y + ch; r++) {
        ov_px_t *row = overlay_row(y + r);
        for (int c = cx - x; c < cx - x + cw; c++) {
            uint32_t m = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    uint32_t v = glyph_cov(bits, pitch, g->w, g->h, r + dr, c + dc);
                    if (dr && dc) {
                        v = v * 2 / 3;
                    }
                    m = v > m ? v : m;
                }
            }
            if (m) {
                blend(&row[x + c], 0, alpha[m]);
            }
        }
    }
    overlay_mark_dirty(cx, cy, cw, ch);
}

/*
 * Opacity per coverage level of the text and of its outline, which keeps light text readable on
 * light parts of the picture. Dark text gets none: it would only look bold. False without outline.
 */
#define OUTLINE_PCT     70

static bool text_alphas(uint32_t rgb, uint8_t a, uint8_t *alpha, uint8_t *outline)
{
    const uint32_t luma = (((rgb >> 16) & 255) * 77 + ((rgb >> 8) & 255) * 150 + (rgb & 255) * 29) >> 8;
    const uint32_t oa = luma >= 96 ? a * OUTLINE_PCT / 100 : 0;
    for (int v = 0; v < 16; v++) {
        alpha[v] = (uint8_t)((a * v + 7) / 15);
        outline[v] = (uint8_t)((oa * v + 7) / 15);
    }
    return oa > 0;
}

static int digit_cell16(const gfx_font_t *f);

/*
 * Glyphs of s from pen16 (1/16 px), or only their outlines; digits in cells of the widest digit if
 * mono. Returns the pen position after the text.
 */
static int text_run(const gfx_font_t *f, int pen16, int baseline, const char *s, bool mono, bool outline,
                    uint32_t rgb, const uint8_t *alpha)
{
    const int cell = mono ? digit_cell16(f) : 0;
    while (*s) {
        const uint32_t cp = utf8_next(&s);
        const gfx_glyph_t *g = find_glyph(f, cp);
        if (!g) {
            continue;
        }
        const bool digit = mono && cp >= '0' && cp <= '9';
        const int adv = digit ? cell : g->adv16;
        const int shift = digit ? (cell - g->adv16) / 2 : 0;
        if (g->w) {
            /* Bitmaps go on whole pixels: the pen is rounded */
            const int gx = (pen16 + shift + 8) / 16 + g->left, gy = baseline - g->top;
            if (outline) {
                draw_outline(f, g, gx, gy, alpha);
            } else {
                draw_glyph(f, g, gx, gy, rgb, alpha);
            }
        }
        pen16 += adv;
    }
    return pen16;
}

/* Text from pen16, with the outline unless plain; returns the pen after it */
static int text_draw(const gfx_font_t *f, int pen16, int baseline, const char *s, uint32_t rgb, uint8_t a, bool mono,
                     bool plain)
{
    uint8_t alpha[16], outline[16];
    if (text_alphas(rgb, a, alpha, outline) && !plain) {
        /* All outlines first, so that none darkens a neighbouring glyph */
        text_run(f, pen16, baseline, s, mono, true, 0, outline);
    }
    return text_run(f, pen16, baseline, s, mono, false, rgb, alpha);
}

int gfx_text(const gfx_font_t *f, int x, int baseline, const char *s, uint32_t rgb, uint8_t a)
{
    return (text_draw(f, x * 16, baseline, s, rgb, a, false, false) - x * 16 + 8) / 16;
}

int gfx_text_plain(const gfx_font_t *f, int x, int baseline, const char *s, uint32_t rgb, uint8_t a)
{
    return (text_draw(f, x * 16, baseline, s, rgb, a, false, true) - x * 16 + 8) / 16;
}

int gfx_text_width(const gfx_font_t *f, const char *s)
{
    int w16 = 0;
    while (*s) {
        const gfx_glyph_t *g = find_glyph(f, utf8_next(&s));
        if (g) {
            w16 += g->adv16;
        }
    }
    return (w16 + 8) / 16;
}

int gfx_text_at(const gfx_font_t *f, int x, int baseline, gfx_align_t align, const char *s, uint32_t rgb, uint8_t a)
{
    const int w = gfx_text_width(f, s);
    if (align == GFX_CENTER) {
        x -= w / 2;
    } else if (align == GFX_RIGHT) {
        x -= w;
    }
    return gfx_text(f, x, baseline, s, rgb, a);
}

/* Advance of the widest digit, the cell width of gfx_text_mono */
static int digit_cell16(const gfx_font_t *f)
{
    int cell = 0;
    for (char c = '0'; c <= '9'; c++) {
        const gfx_glyph_t *g = find_glyph(f, (uint32_t)c);
        if (g && g->adv16 > cell) {
            cell = g->adv16;
        }
    }
    return cell;
}

int gfx_mono_advance16(const gfx_font_t *f, uint32_t cp)
{
    if (cp >= '0' && cp <= '9') {
        return digit_cell16(f);
    }
    const gfx_glyph_t *g = find_glyph(f, cp);
    return g ? g->adv16 : 0;
}

int gfx_text_mono(const gfx_font_t *f, int x, int baseline, const char *s, uint32_t rgb, uint8_t a)
{
    return (gfx_text_mono16(f, x * 16, baseline, s, rgb, a) - x * 16 + 8) / 16;
}

int gfx_text_mono16(const gfx_font_t *f, int pen16, int baseline, const char *s, uint32_t rgb, uint8_t a)
{
    return text_draw(f, pen16, baseline, s, rgb, a, true, false);
}
