#include "Arduino.h"
#include "TFT_eSPI.h"
#include <lvgl.h>

extern TFT_eSPI tft;

static lv_display_t *display = NULL;
static uint8_t buf[240 * 240 * 2]; // RGB565: 2 bytes per pixel

static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)px_map, w * h, true);
    tft.endWrite();

    lv_display_flush_ready(disp);
}

void lv_port_disp_init(void)
{
    display = lv_display_create(240, 240);
    lv_display_set_buffers(display, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, disp_flush);
}

void lv_port_tick_inc(void)
{
    lv_tick_inc(1);
}
