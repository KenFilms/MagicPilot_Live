/*
 * Drawing primitives for the overlay canvas (landscape coordinates, see overlay.h).
 *
 * Colours are 0xRRGGBB, alpha 0..255. Shapes and text are anti-aliased. Every primitive marks the
 * area it touched as changed; call overlay_commit() when a redraw is complete. Only one task (the
 * UI) draws, and it does so without the overlay lock (see overlay.h).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t cp;            /* Unicode code point */
    uint16_t adv16;         /* advance in 1/16 px */
    uint32_t off;           /* offset of the 4-bit bitmap (rows padded to whole bytes) */
    uint8_t w, h;           /* bitmap size */
    int8_t left, top;       /* bitmap position relative to the pen (x) and the baseline (up) */
} gfx_glyph_t;

typedef struct {
    const uint8_t *bits;           /* all bitmaps, 2 pixels per byte, the left one in the low nibble */
    const gfx_glyph_t *glyphs;     /* sorted by code point */
    uint16_t count;                /* number of glyphs */
    uint8_t ascent, descent, line_h, cap_h; /* px: above and below the baseline, line spacing, capital height */
} gfx_font_t;

typedef enum {
    GFX_LEFT,
    GFX_CENTER,
    GFX_RIGHT,
} gfx_align_t;

/* Restrict drawing to a rectangle; gfx_clip_reset() restores the whole canvas */
void gfx_clip(int x, int y, int w, int h);
void gfx_clip_reset(void);

/* Make an area transparent */
void gfx_clear(int x, int y, int w, int h);
/* Replace an area with a colour of the given opacity (the picture shows through the rest) */
void gfx_set(int x, int y, int w, int h, uint32_t rgb, uint8_t a);
/* Paint over an area (blend) */
void gfx_fill(int x, int y, int w, int h, uint32_t rgb, uint8_t a);

/* Filled rounded rectangle, corner radius r (at most half the shorter side; 0 or less: square corners) */
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t rgb, uint8_t a);
/* Outline of thickness t along the inside of the rounded rectangle gfx_rrect() would fill */
void gfx_rrect_stroke(int x, int y, int w, int h, int r, float t, uint32_t rgb, uint8_t a);
/* Filled circle of radius r around (cx, cy), in the units of the rectangles: pixel x spans x..x + 1 */
void gfx_circle(float cx, float cy, float r, uint32_t rgb, uint8_t a);
/* Ring around (cx, cy) from radius r - t to r */
void gfx_ring(float cx, float cy, float r, float t, uint32_t rgb, uint8_t a);
/* Line with round caps, thickness t */
void gfx_line(float x0, float y0, float x1, float y1, float t, uint32_t rgb, uint8_t a);
/* Convex polygon with up to GFX_POLY_MAX corners, in either order */
#define GFX_POLY_MAX 8
void gfx_poly(const float *xs, const float *ys, int n, uint32_t rgb, uint8_t a);

/* Text on a baseline; returns the advance width */
int gfx_text(const gfx_font_t *f, int x, int baseline, const char *s, uint32_t rgb, uint8_t a);
/* The advance width gfx_text() would return, without drawing */
int gfx_text_width(const gfx_font_t *f, const char *s);
/* Text aligned at x (left edge, centre or right edge) */
int gfx_text_at(const gfx_font_t *f, int x, int baseline, gfx_align_t align, const char *s, uint32_t rgb, uint8_t a);
/* Text without the dark outline that keeps light text readable over the picture: for text on a solid fill */
int gfx_text_plain(const gfx_font_t *f, int x, int baseline, const char *s, uint32_t rgb, uint8_t a);
/* Text with every digit in a cell of the widest digit (timecode, values that tick) */
int gfx_text_mono(const gfx_font_t *f, int x, int baseline, const char *s, uint32_t rgb, uint8_t a);
/* The same with the pen in 1/16 px (returns the pen after the text), and one character's advance */
int gfx_text_mono16(const gfx_font_t *f, int pen16, int baseline, const char *s, uint32_t rgb, uint8_t a);
int gfx_mono_advance16(const gfx_font_t *f, uint32_t cp);
