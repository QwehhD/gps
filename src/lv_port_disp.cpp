#include "Arduino.h"
#include <lvgl.h>
#include <string.h>
#include "driver/spi_master.h"
#include "soc/gpio_reg.h"

static lv_display_t *display = NULL;

// TFT_eSPI only initialises the panel and draws the splash text in setup().
// After that the panel is driven straight through ESP-IDF's spi_master:
// commands and DMA pixel transfers go through the same driver, so the SPI
// peripheral is always in the state that driver expects. (Mixing TFT_eSPI's
// direct register writes with DMA transfers left the panel ignoring every
// frame.) esp_lcd is not used because in this IDF (4.4) it only lets one
// pixel transfer be in flight, which would block on every frame bigger than
// one transfer.
static spi_device_handle_t lcd_spi = NULL;

// One full-screen render buffer, used in DIRECT mode: LVGL re-runs the whole
// scene's draw calls for every render pass, and that per-pass cost dominates
// the frame, so any invalidated area must fit in one pass. Sending is still
// asynchronous: DMA pushes the frame out during the wait for the next
// refresh. DMA needs internal, word-aligned RAM, which a static array is.
static uint16_t buf[240 * 240] __attribute__((aligned(4)));

// The ESP32-S3 SPI DMA moves at most 32 KB per transaction, so pixels are
// sent in transactions of up to this many (64 full rows, 30 KB).
#define TX_MAX_PIXELS (240 * 64)

// A frame is a run of queued transactions: address window, then for every
// pixel chunk a write command and its data. 4 + 2 * 4 chunks = 12 at most.
#define TX_QUEUE 16
static spi_transaction_t tx[TX_QUEUE];
static uint8_t tx_queued = 0;

// Set from the SPI interrupt when the frame's last transaction has gone out.
static volatile bool frame_sent = true;

// transaction.user flags
#define TX_DATA 1u // D/C line high (parameters/pixels) instead of low (command)
#define TX_LAST 2u // last transaction of the frame

// LVGL redraws at most this often. A moving navigation frame takes 10-17 ms
// on the ESP32-S3 (measured), so 25 ms gives a
// steady 40 fps; a period shorter than the frame time would make frames
// alternate between one and two periods, which reads as stutter.
#define REFRESH_PERIOD_MS 25

// GC9A01 commands used per frame.
#define LCD_CMD_CASET 0x2A
#define LCD_CMD_RASET 0x2B
#define LCD_CMD_RAMWR 0x2C
#define LCD_CMD_RAMWR_CONTINUE 0x3C

// 1 = print frame rate and render/flush timings to Serial every 2 s. Can
// also be set per build with -DLV_PORT_PERF_LOG=1.
#ifndef LV_PORT_PERF_LOG
#define LV_PORT_PERF_LOG 0
#endif

#if LV_PORT_PERF_LOG
static uint32_t perf_render_start_us;
static uint32_t perf_render_us;
static uint32_t perf_flush_us;
static uint32_t perf_frames;
static uint32_t perf_px;
static uint32_t perf_window_ms;
static uint32_t perf_last_start_us;
static uint32_t perf_max_gap_us;
static uint32_t perf_max_render_us;

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
    uint32_t render_us = micros() - perf_render_start_us;
    perf_render_us += render_us;
    if (render_us > perf_max_render_us)
    {
        perf_max_render_us = render_us;
    }
    perf_frames++;
    uint32_t now = millis();
    if (now - perf_window_ms >= 2000)
    {
        float secs = (now - perf_window_ms) / 1000.0f;
        Serial.printf("[perf] %.1f fps | longest gap %.1f ms | render+flush %.1f ms/frame, max %.1f (flush %.1f ms) | %lu px/frame\n",
                      perf_frames / secs, perf_max_gap_us / 1000.0f,
                      perf_frames ? perf_render_us / 1000.0f / perf_frames : 0.0f, perf_max_render_us / 1000.0f,
                      perf_frames ? perf_flush_us / 1000.0f / perf_frames : 0.0f,
                      perf_frames ? (unsigned long)(perf_px / perf_frames) : 0UL);
        perf_render_us = perf_flush_us = perf_frames = perf_px = perf_max_gap_us = perf_max_render_us = 0;
        perf_window_ms = now;
    }
}
#endif

// Both run in the SPI interrupt, so they only touch registers and a flag.
static void IRAM_ATTR lcd_pre_transfer(spi_transaction_t *t)
{
    if ((uint32_t)t->user & TX_DATA)
    {
        REG_WRITE(GPIO_OUT_W1TS_REG, 1u << TFT_DC);
    }
    else
    {
        REG_WRITE(GPIO_OUT_W1TC_REG, 1u << TFT_DC);
    }
}

static void IRAM_ATTR lcd_post_transfer(spi_transaction_t *t)
{
    if ((uint32_t)t->user & TX_LAST)
    {
        frame_sent = true;
    }
}

static void queue_tx(uint32_t flags, const void *data, size_t len)
{
    spi_transaction_t *t = &tx[tx_queued++];
    memset(t, 0, sizeof(*t));
    t->user = (void *)(uintptr_t)flags;
    t->length = len * 8;
    if (len <= sizeof(t->tx_data))
    {
        t->flags = SPI_TRANS_USE_TXDATA;
        memcpy(t->tx_data, data, len);
    }
    else
    {
        t->tx_buffer = data;
    }
    spi_device_queue_trans(lcd_spi, t, portMAX_DELAY);
}

static void queue_cmd(uint8_t cmd)
{
    queue_tx(0, &cmd, 1);
}

// LVGL calls this before it renders into the buffer while it is still being
// sent.
static void flush_wait(lv_display_t *disp)
{
    (void)disp;
    while (!frame_sent)
    {
    }
}

// Rows changed during the current refresh. In DIRECT mode every invalidated
// area is rendered straight into its place in the full-screen buffer, so the
// areas don't have to wait for each other's transfer; they all go out
// together after the last one.
static int32_t dirty_y1 = INT32_MAX;
static int32_t dirty_y2 = -1;

static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    dirty_y1 = LV_MIN(dirty_y1, area->y1);
    dirty_y2 = LV_MAX(dirty_y2, area->y2);
    if (!lv_display_flush_is_last(disp))
    {
        // Nothing is being sent yet, so LVGL may render the next area now.
        lv_display_flush_ready(disp);
        return;
    }
#if LV_PORT_PERF_LOG
    uint32_t t0 = micros();
#endif

    // The previous frame has fully gone out (flush_wait); collect its
    // finished transactions so their queue slots are free again.
    for (uint8_t i = 0; i < tx_queued; i++)
    {
        spi_transaction_t *done;
        spi_device_get_trans_result(lcd_spi, &done, portMAX_DELAY);
    }
    tx_queued = 0;

    // Send the changed rows at full width: they are contiguous in the buffer.
    uint32_t y1 = (uint32_t)dirty_y1;
    uint32_t h = (uint32_t)(dirty_y2 - dirty_y1 + 1);
    dirty_y1 = INT32_MAX;
    dirty_y2 = -1;
    const uint8_t cols[4] = {0, 0, 0, 239};
    const uint8_t rows[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)((y1 + h - 1) >> 8), (uint8_t)(y1 + h - 1)};
    const uint8_t *src = px_map + y1 * 240 * 2;

    // Everything is queued for DMA and this returns right away; the panel
    // keeps filling the same window across RAMWR + RAMWR_CONTINUE. LVGL waits
    // in flush_wait() before it renders into the buffer again.
    frame_sent = false;
    queue_cmd(LCD_CMD_CASET);
    queue_tx(TX_DATA, cols, sizeof(cols));
    queue_cmd(LCD_CMD_RASET);
    queue_tx(TX_DATA, rows, sizeof(rows));
    uint32_t rows_per_tx = TX_MAX_PIXELS / 240;
    uint32_t tx_count = (h + rows_per_tx - 1) / rows_per_tx;
    for (uint32_t i = 0; i < tx_count; i++)
    {
        uint32_t first_row = i * rows_per_tx;
        uint32_t tx_rows = LV_MIN(rows_per_tx, h - first_row);
        queue_cmd(i == 0 ? LCD_CMD_RAMWR : LCD_CMD_RAMWR_CONTINUE);
        queue_tx(TX_DATA | (i + 1 == tx_count ? TX_LAST : 0), src + first_row * 240 * 2, tx_rows * 240 * 2);
    }

#if LV_PORT_PERF_LOG
    perf_flush_us += micros() - t0;
    perf_px += h * 240;
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
    bus.max_transfer_sz = TX_MAX_PIXELS * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {};
    dev.mode = 0;
    dev.clock_speed_hz = SPI_FREQUENCY;
    dev.spics_io_num = TFT_CS;
    dev.queue_size = TX_QUEUE;
    dev.flags = SPI_DEVICE_NO_DUMMY;
    dev.pre_cb = lcd_pre_transfer;
    dev.post_cb = lcd_post_transfer;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &lcd_spi));

    display = lv_display_create(240, 240);
    // The panel takes RGB565 high byte first; rendering in that order keeps
    // the persistent DIRECT-mode buffer ready to send as is.
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    // Direct mode: only invalidated areas are rendered (in place) and only
    // their rows are pushed over SPI, so small changes stay cheap.
    lv_display_set_buffers(display, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, disp_flush);
    lv_display_set_flush_wait_cb(display, flush_wait);
    lv_timer_set_period(lv_display_get_refr_timer(display), REFRESH_PERIOD_MS);
#if LV_PORT_PERF_LOG
    lv_display_add_event_cb(display, perf_render_start_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(display, perf_render_ready_cb, LV_EVENT_RENDER_READY, NULL);
#endif
}
