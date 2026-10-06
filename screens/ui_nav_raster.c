// Hot pixel loops: let sqrtf map to the FPU instruction instead of the
// errno-setting library call.
#pragma GCC optimize("O2", "fast-math")

#include "ui_nav_raster.h"

#include <math.h>
#include <string.h>

// Widest clip area nav_raster_fill() handles.
#define NAV_RASTER_MAX_WIDTH 320

bool nav_raster_begin(nav_raster_t *r, lv_layer_t *layer)
{
    // Same pattern LVGL itself uses before reading a layer's pixels.
    while (layer->draw_task_head)
    {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }

    lv_draw_buf_t *draw_buf = layer->draw_buf;
    lv_color_format_t cf = (lv_color_format_t)draw_buf->header.cf;
    if (cf != LV_COLOR_FORMAT_RGB565 && cf != LV_COLOR_FORMAT_RGB565_SWAPPED)
    {
        return false;
    }
    r->buf = draw_buf->data;
    r->stride = draw_buf->header.stride;
    r->buf_x1 = layer->buf_area.x1;
    r->buf_y1 = layer->buf_area.y1;
    r->clip = layer->_clip_area;
    r->swapped = cf == LV_COLOR_FORMAT_RGB565_SWAPPED;
    return true;
}

static inline uint16_t to_565(lv_color_t c)
{
    return (uint16_t)(((c.red & 0xF8) << 8) | ((c.green & 0xFC) << 3) | (c.blue >> 3));
}

static inline uint16_t *pixel_row(const nav_raster_t *r, int32_t y)
{
    return (uint16_t *)(r->buf + (y - r->buf_y1) * r->stride) - r->buf_x1;
}

// Converts between RGB565 and the buffer's byte order (the swap is its own
// inverse).
static inline uint16_t order(const nav_raster_t *r, uint16_t v)
{
    return r->swapped ? (uint16_t)((v << 8) | (v >> 8)) : v;
}

void nav_raster_fill(const nav_raster_t *r, lv_color_t color)
{
    // One row of the colour, then memcpy (much faster than a pixel loop).
    static uint16_t pattern[NAV_RASTER_MAX_WIDTH];
    int32_t w = r->clip.x2 - r->clip.x1 + 1;
    if (w <= 0 || w > (int32_t)(sizeof(pattern) / sizeof(pattern[0])))
    {
        return;
    }
    uint16_t v = order(r, to_565(color));
    for (int32_t i = 0; i < w; i++)
    {
        pattern[i] = v;
    }
    for (int32_t y = r->clip.y1; y <= r->clip.y2; y++)
    {
        memcpy(&pixel_row(r, y)[r->clip.x1], pattern, (size_t)w * 2);
    }
}

// Blends `src` (RGB565) over the pixel with coverage `a` (0..32).
static inline void blend(const nav_raster_t *r, uint16_t *px, uint16_t src, uint32_t a)
{
    uint32_t dst = order(r, *px);
    // Classic RGB565 blend: spread to 0x07E0F81F so all three channels are
    // mixed with one multiply.
    uint32_t s = (src | ((uint32_t)src << 16)) & 0x07E0F81Fu;
    uint32_t d = (dst | (dst << 16)) & 0x07E0F81Fu;
    uint32_t m = (d + (((s - d) * a) >> 5)) & 0x07E0F81Fu;
    *px = order(r, (uint16_t)(m | (m >> 16)));
}

void nav_raster_segment(const nav_raster_t *r, float x1, float y1, float x2, float y2,
                        float width, lv_color_t color)
{
    float rad = width * 0.5f;
    float reach = rad + 0.5f; // anti-aliased fringe ends here
    float inner = rad - 0.5f; // fully covered inside this
    float reach2 = reach * reach;
    float inner2 = inner > 0.0f ? inner * inner : -1.0f;

    // Pixel centres are at integer + 0.5.
    x1 += 0.5f;
    y1 += 0.5f;
    x2 += 0.5f;
    y2 += 0.5f;
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    // Unit direction along the segment (any direction for a dot).
    float ux = len > 0.0f ? dx / len : 1.0f;
    float uy = len > 0.0f ? dy / len : 0.0f;

    int32_t ry1 = (int32_t)floorf(fminf(y1, y2) - reach);
    int32_t ry2 = (int32_t)ceilf(fmaxf(y1, y2) + reach);
    int32_t bx1 = (int32_t)floorf(fminf(x1, x2) - reach);
    int32_t bx2 = (int32_t)ceilf(fmaxf(x1, x2) + reach);
    ry1 = LV_MAX(ry1, r->clip.y1);
    ry2 = LV_MIN(ry2, r->clip.y2);
    bx1 = LV_MAX(bx1, r->clip.x1);
    bx2 = LV_MIN(bx2, r->clip.x2);
    if (ry1 > ry2 || bx1 > bx2)
    {
        return;
    }

    uint16_t src = to_565(color);
    for (int32_t y = ry1; y <= ry2; y++)
    {
        float py = y + 0.5f;

        // Only the part of the segment within `reach` of this row matters:
        // its x-extent (plus reach) bounds the pixels worth testing.
        int32_t xs = bx1;
        int32_t xe = bx2;
        if (dy != 0.0f)
        {
            float t0 = (py - reach - y1) / dy;
            float t1 = (py + reach - y1) / dy;
            if (t0 > t1)
            {
                float tmp = t0;
                t0 = t1;
                t1 = tmp;
            }
            t0 = fmaxf(t0, 0.0f);
            t1 = fminf(t1, 1.0f);
            if (t0 > t1)
            {
                // Row lies just past the segment's ends: only the nearer end
                // cap can reach it.
                t0 = t1 = fabsf(py - y1) < fabsf(py - y2) ? 0.0f : 1.0f;
            }
            float xa = x1 + dx * t0;
            float xb = x1 + dx * t1;
            xs = LV_MAX(xs, (int32_t)floorf(fminf(xa, xb) - reach));
            xe = LV_MIN(xe, (int32_t)ceilf(fmaxf(xa, xb) + reach));
        }
        if (xs > xe)
        {
            continue;
        }

        // Position of the first pixel relative to the segment: `along` it
        // (0..len between the ends) and `across` it (signed distance from
        // the centre line). Both change linearly with x, so each next pixel
        // just adds a constant.
        float px = xs + 0.5f;
        float along = (px - x1) * ux + (py - y1) * uy;
        float across = (py - y1) * ux - (px - x1) * uy;
        uint16_t *row = pixel_row(r, y);
        for (int32_t x = xs; x <= xe; x++, along += ux, across -= uy)
        {
            float d2;
            if (along < 0.0f)
            {
                d2 = along * along + across * across;
            }
            else if (along > len)
            {
                float past = along - len;
                d2 = past * past + across * across;
            }
            else
            {
                d2 = across * across;
            }
            if (d2 >= reach2)
            {
                continue;
            }
            if (d2 <= inner2)
            {
                row[x] = order(r, src);
                continue;
            }
            float cov = reach - sqrtf(d2); // 0..1 across the fringe
            blend(r, &row[x], src, (uint32_t)(cov * 32.0f));
        }
    }
}
