#include "Arduino.h"
#include "TFT_eSPI.h"
#include <lvgl.h>
#include "driver/spi_master.h"

extern TFT_eSPI tft;

// TFT_eSPI's DMA device and SPI host (globals in its ESP32-S3 processor file).
extern spi_device_handle_t dmaHAL;
extern spi_host_device_t spi_host;

static lv_display_t *display = NULL;

// Two render buffers: LVGL draws into one while DMA sends the other to the
// panel. 68 lines is the most that stays under TFT_eSPI's 16384-pixel single
// DMA transfer limit; fewer, bigger chunks matter because LVGL re-runs the
// whole scene's draw calls for every chunk. DMA needs internal, word-aligned
// RAM, which static arrays are.
#define BUF_LINES 68
static uint16_t buf1[240 * BUF_LINES] __attribute__((aligned(4)));
static uint16_t buf2[240 * BUF_LINES] __attribute__((aligned(4)));

// LVGL redraws at most this often. A moving navigation frame takes 18-22 ms
// on the ESP32-S3 (measured, SPI transfer overlapped), so 25 ms gives a
// steady 40 fps; a period shorter than the frame time would make frames
// alternate between one and two periods, which reads as stutter.
#define REFRESH_PERIOD_MS 25

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
        Serial.printf("[perf] %.1f fps | longest gap %.1f ms | render+flush %.1f ms/frame (flush wait %.1f ms) | %lu px/frame\n",
                      perf_frames / secs, perf_max_gap_us / 1000.0f,
                      perf_frames ? perf_render_us / 1000.0f / perf_frames : 0.0f,
                      perf_frames ? perf_flush_us / 1000.0f / perf_frames : 0.0f,
                      perf_frames ? (unsigned long)(perf_px / perf_frames) : 0UL);
        perf_render_us = perf_flush_us = perf_frames = perf_px = perf_max_gap_us = 0;
        perf_window_ms = now;
    }
}
#endif

// Workaround for TFT_eSPI 2.5.43 on ESP32-S3: its end-of-transfer callback
// clears SPI_DMA_CONF_REG(spi_host) with spi_host = SPI2_HOST = 1, but this
// core's REG_SPI_BASE() takes the peripheral number (FSPI = 2) and maps 1 to
// address 0, so the very first DMA transfer crashes (StoreProhibited, 0x30).
// The DMA device is re-registered with a callback that clears the right
// register; that hands the SPI peripheral back to TFT_eSPI's CPU-driven
// writes (commands, address window) between transfers.
static void IRAM_ATTR dma_end_cb(spi_transaction_t *trans)
{
    (void)trans;
    WRITE_PERI_REG(SPI_DMA_CONF_REG(SPI_PORT), 0);
}

static void fix_tft_dma_callback(void)
{
    spi_bus_remove_device(dmaHAL);
    spi_device_interface_config_t devcfg = {};
    devcfg.mode = TFT_SPI_MODE;
    devcfg.clock_speed_hz = SPI_FREQUENCY;
    devcfg.spics_io_num = -1; // TFT_eSPI drives CS itself
    devcfg.flags = SPI_DEVICE_NO_DUMMY;
    devcfg.queue_size = 1;
    devcfg.post_cb = dma_end_cb;
    ESP_ERROR_CHECK(spi_bus_add_device(spi_host, &devcfg, &dmaHAL));
}

static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
#if LV_PORT_PERF_LOG
    uint32_t t0 = micros();
#endif

    // Waits for the previous chunk's transfer, byte-swaps this chunk in place
    // and starts sending it, then returns right away. The buffer is handed
    // back to LVGL immediately: with two buffers it renders into the other
    // one next, and the next flush waits for this transfer before reuse.
    tft.pushImageDMA(area->x1, area->y1, w, h, (uint16_t *)px_map);

#if LV_PORT_PERF_LOG
    perf_flush_us += micros() - t0;
    perf_px += w * h;
#endif
    lv_display_flush_ready(disp);
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

    // From here on the panel is driven only through DMA. The SPI transaction
    // stays open for good: endWrite() would block until each transfer ends,
    // which is exactly the wait DMA is meant to hide.
    tft.initDMA();
    fix_tft_dma_callback();
    tft.setSwapBytes(true);
    tft.startWrite();

    display = lv_display_create(240, 240);
    // Partial mode: only invalidated areas are rendered and pushed over SPI,
    // so small per-frame changes (progress arc, moving road) stay cheap.
    lv_display_set_buffers(display, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, disp_flush);
    lv_timer_set_period(lv_display_get_refr_timer(display), REFRESH_PERIOD_MS);
#if LV_PORT_PERF_LOG
    lv_display_add_event_cb(display, perf_render_start_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(display, perf_render_ready_cb, LV_EVENT_RENDER_READY, NULL);
#endif
}
