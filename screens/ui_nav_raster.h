// Small software rasterizer for the navigation screen's map layer.
//
// LVGL's own line renderer costs ~0.1-0.3 ms per line on the ESP32-S3 (mask
// setup per line, masks evaluated over the whole bounding box), which is too
// slow for a map made of dozens of lines redrawn every frame. This draws
// straight into the layer's pixel buffer instead: anti-aliased thick segments
// with round ends, only visiting the pixels near each segment.
#ifndef UI_NAV_RASTER_H
#define UI_NAV_RASTER_H

#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        uint8_t *buf;    // first byte of the layer buffer
        int32_t stride;  // bytes per buffer row
        int32_t buf_x1;  // screen coordinates of the buffer's first pixel
        int32_t buf_y1;
        lv_area_t clip;  // screen area that may be written (inclusive)
        bool swapped;    // RGB565 stored high byte first
    } nav_raster_t;

    // Prepares drawing straight into `layer`, after letting LVGL finish every
    // draw task already queued on it (so nothing drawn here is overwritten
    // later by something meant to be below it). Returns false if the layer's
    // pixel format is not RGB565, in which case nothing may be drawn.
    bool nav_raster_begin(nav_raster_t *r, lv_layer_t *layer);

    // Fills the whole clip area.
    void nav_raster_fill(const nav_raster_t *r, lv_color_t color);

    // Anti-aliased segment of the given width with round ends, from (x1,y1)
    // to (x2,y2) in screen pixels (pixel centres at integer + 0.5).
    void nav_raster_segment(const nav_raster_t *r, float x1, float y1, float x2, float y2,
                            float width, lv_color_t color);

#ifdef __cplusplus
}
#endif

#endif
