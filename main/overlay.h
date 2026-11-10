/*
 * Overlay layer: the camera UI drawn on top of the HDMI picture.
 *
 * The UI draws into a landscape canvas (OV_W x OV_H, the orientation the user sees). Every canvas
 * pixel is stored ready for compositing: premultiplied RGB565 in bits 0-15 and the weight left for
 * the picture underneath (0..32) in bits 16-21, so out = color + picture * weight / 32.
 *
 * After drawing, overlay_commit() run-length encodes the changed rows (uniform runs, such as the
 * shaded bars, store one value; other runs keep a copy of their pixels). Compositing a row then
 * only touches the pixels the overlay covers. The video renderer composites the rows into the
 * internal RAM bands before the PPA rotates them onto the screen, or into the frame buffer as a
 * fallback.
 *
 * Drawing and committing are done by one task (the UI) without the lock: the commit encodes into
 * spare buffers and swaps them in with the lock held briefly. The renderer holds the lock while
 * it composites.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define OV_W            1280
#define OV_H            720
#define OV_CLEAR        (32u << 16)             /* fully transparent canvas pixel */

typedef uint32_t ov_px_t;

/* Allocate the canvas (all transparent) and the row tables; once, before anything draws or composites */
void overlay_init(void);

/* Canvas row pointer (landscape) */
ov_px_t *overlay_row(int y);

/* Held by the renderer while it composites, and by overlay_commit() while it swaps rows in */
void overlay_lock(void);
void overlay_unlock(void);

/* Record that a canvas area changed; done by the gfx primitives */
void overlay_mark_dirty(int x, int y, int w, int h);

/* Re-encode the changed rows and swap them in; bumps the generation if anything changed */
void overlay_commit(void);

/* Incremented by every commit that changed something; the renderer re-presents a still screen */
uint32_t overlay_generation(void);

/* True if the overlay covers nothing in this row */
bool overlay_row_empty(int y);

/* Composite one landscape row (OV_W RGB565 pixels). Caller holds the lock. */
void overlay_composite_row(uint16_t *row, int y);

/*
 * Composite the whole overlay into a portrait frame buffer (fb_w x fb_h RGB565) that shows the
 * landscape view rotated: landscape (x, y) lands at fb + fy * fb_w + fx with
 *   fx = rot270 ? fb_w - 1 - y : y,  fy = rot270 ? x : fb_h - 1 - x.
 * Slow path for layouts that cannot use the band composite. Caller holds the lock.
 */
void overlay_composite_fb(uint16_t *fb, int fb_w, int fb_h, bool rot270);

/* Blend helpers shared with the renderer */
static inline uint16_t ov_scale565(uint16_t p, uint32_t weight)
{
    /* Green moved to the upper half-word: one multiply scales all three channels by weight / 32 (0..32) */
    uint32_t x = (p | ((uint32_t)p << 16)) & 0x07E0F81Fu;
    x = ((x * weight) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(x | (x >> 16));
}
