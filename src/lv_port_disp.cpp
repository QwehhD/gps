#include "Arduino.h"
#include <lvgl.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

static lv_display_t *display = NULL;

// TFT_eSPI only initialises the panel and draws the splash text in setup().
// After that the panel is driven through ESP-IDF's esp_lcd: address commands
// and DMA pixel transfers go through the same SPI driver, so the peripheral
// is always in the state that driver expects. (Mixing TFT_eSPI's direct
// register writes with DMA transfers left the panel ignoring every frame.)
static esp_lcd_panel_io_handle_t panel_io = NULL;

// Set from the SPI interrupt when a pixel transfer has gone out.
static volatile bool transfer_done = true;

// Two render buffers: LVGL draws into one while DMA sends the other to the
// panel. Bigger chunks matter because LVGL re-runs the whole scene's draw
// calls for every chunk. DMA needs internal, word-aligned RAM, which static
// arrays are.
#define BUF_LINES 68
static uint16_t buf1[240 * BUF_LINES] __attribute__((aligned(4)));
static uint16_t buf2[240 * BUF_LINES] __attribute__((aligned(4)));

// LVGL redraws at most this often. A moving navigation frame takes 16-19 ms
// on the ESP32-S3 (measured, SPI transfer overlapped), so 25 ms gives a
// steady 40 fps; a period shorter than the frame time would make frames
// alternate between one and two periods, which reads as stutter.
#define REFRESH_PERIOD_MS 25

// GC9A01 commands used per chunk.
#define LCD_CMD_CASET 0x2A
#define LCD_CMD_RASET 0x2B
#define LCD_CMD_RAMWR 0x2C

// 1 = print frame rate and render/flush timings to Serial every 2 s.
#define LV_PORT_PERF_LOG 0

#if LV_PORT_PERF_LOG
static uint32_t perf_render_start_us;
static uint32_t perf_render_us;
static uint32_t perf_flush_us;
static uint32_t perf_frames;
static uint32_t perf_px;
static uint32_t perf_window_ms;
static uint32_t perf_last_start_us;
static uint32_t perf_max_gap_us;

static void perf_render_start_cb(lv_event_t *e)
{
    (void)e;
    perf_render_start_us = micros();
    if (perf_last_start_us != 0 && perf_render_start_us - perf_last_start_us > perf_max_gap_us)
    {
        perf_max_gap_us = perf_render_start_us - perf_last_start_us;
    }
    perf_last_start_us = perf_render_start_us;
}

static void perf_render_ready_cb(lv_event_t *e)
{
    (void)e;
    perf_render_us += micros() - perf_render_start_us;
    perf_frames++;
    uint32_t now = millis();
    if (now - perf_window_ms >= 2000)
    {
        float secs = (now - perf_window_ms) / 1000.0f;
        Serial.printf("[perf] %.1f fps | longest gap %.1f ms | render+flush %.1f ms/frame (flush %.1f ms) | %lu px/frame\n",
                      perf_frames / secs, perf_max_gap_us / 1000.0f,
                      perf_frames ? perf_render_us / 1000.0f / perf_frames : 0.0f,
                      perf_frames ? perf_flush_us / 1000.0f / perf_frames : 0.0f,
                      perf_frames ? (unsigned long)(perf_px / perf_frames) : 0UL);
        perf_render_us = perf_flush_us = perf_frames = perf_px = perf_max_gap_us = 0;
        perf_window_ms = now;
    }
}
#endif

// Runs in the SPI interrupt, so it only flips a flag; LVGL picks it up in
// flush_wait() below.
static bool IRAM_ATTR on_transfer_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    transfer_done = true;
    return false;
}

// LVGL calls this before it reuses a buffer that is still being sent.
static void flush_wait(lv_display_t *disp)
{
    (void)disp;
    while (!transfer_done)
    {
    }
}

static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
#if LV_PORT_PERF_LOG
    uint32_t t0 = micros();
#endif

    // The panel takes RGB565 high byte first.
    lv_draw_rgb565_swap(px_map, w * h);

    const uint8_t cols[4] = {(uint8_t)(area->x1 >> 8), (uint8_t)area->x1, (uint8_t)(area->x2 >> 8), (uint8_t)area->x2};
    const uint8_t rows[4] = {(uint8_t)(area->y1 >> 8), (uint8_t)area->y1, (uint8_t)(area->y2 >> 8), (uint8_t)area->y2};
    esp_lcd_panel_io_tx_param(panel_io, LCD_CMD_CASET, cols, sizeof(cols));
    esp_lcd_panel_io_tx_param(panel_io, LCD_CMD_RASET, rows, sizeof(rows));

    // Queued as DMA and returns right away: LVGL renders the next chunk into
    // the other buffer meanwhile, and waits in flush_wait() before reuse.
    transfer_done = false;
    esp_lcd_panel_io_tx_color(panel_io, LCD_CMD_RAMWR, px_map, w * h * 2);

#if LV_PORT_PERF_LOG
    perf_flush_us += micros() - t0;
    perf_px += w * h;
#endif
}

// LVGL reads real elapsed time from millis(), so animations and the refresh
// period stay correct no matter how long each loop() iteration takes.
static uint32_t tick_get_cb(void)
{
    return millis();
}

void lv_port_disp_init(void)
{
    lv_tick_set_cb(tick_get_cb);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = TFT_MOSI;
    bus.miso_io_num = -1;
    bus.sclk_io_num = TFT_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = sizeof(buf1);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = TFT_CS;
    io.dc_gpio_num = TFT_DC;
    io.spi_mode = 0;
    io.pclk_hz = SPI_FREQUENCY;
    io.trans_queue_depth = 4;
    io.on_color_trans_done = on_transfer_done;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &panel_io));

    display = lv_display_create(240, 240);
    // Partial mode: only invalidated areas are rendered and pushed over SPI,
    // so small per-frame changes (progress arc, moving road) stay cheap.
    lv_display_set_buffers(display, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, disp_flush);
    lv_display_set_flush_wait_cb(display, flush_wait);
    lv_timer_set_period(lv_display_get_refr_timer(display), REFRESH_PERIOD_MS);
#if LV_PORT_PERF_LOG
    lv_display_add_event_cb(display, perf_render_start_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(display, perf_render_ready_cb, LV_EVENT_RENDER_READY, NULL);
#endif
}
