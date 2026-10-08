// Implementation of the BLE navigation receiver declared in nav_rx.h.
#include "nav_rx.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "nav_codec.h"
#include "nav_frag.h"

namespace
{

    constexpr uint32_t STATS_PERIOD_MS = 5000;

    struct RxItem
    {
        nav_data_t nav;
        uint32_t rx_ms; // millis() when it was decoded
    };

    QueueHandle_t g_queue = nullptr;

    // Bluetooth task only. Static, not on the stack: that task's stack is
    // just 3 KB (CONFIG_BT_BTC_TASK_STACK_SIZE).
    nav_frag_t g_frag;
    RxItem g_item;
    bool g_have_seq = false;
    uint8_t g_last_seq = 0;

    // Written by the Bluetooth task, read by loop() for the log only.
    volatile uint32_t g_decoded = 0;
    volatile uint32_t g_decode_errors = 0;
    volatile uint32_t g_lost_by_seq = 0;
    volatile nav_decode_status_t g_last_error = NAV_DECODE_OK;

} // namespace

void nav_rx_init(void)
{
    nav_frag_init(&g_frag);
    g_queue = xQueueCreate(1, sizeof(RxItem));
}

void nav_rx_on_connect(void)
{
    nav_frag_reset(&g_frag);
    g_have_seq = false; // the sender's counter starts afresh
}

void nav_rx_on_disconnect(void)
{
    nav_frag_reset(&g_frag);
    g_have_seq = false;
    // Old data must not look valid after a quick reconnect.
    xQueueReset(g_queue);
}

void nav_rx_on_frame(const uint8_t *frame, size_t len)
{
    const uint8_t *msg = nullptr;
    size_t msg_len = 0;
    if (nav_frag_push(&g_frag, frame, len, &msg, &msg_len) != NAV_FRAG_COMPLETE)
    {
        return;
    }

    uint8_t seq = 0;
    nav_decode_status_t status = nav_decode(msg, msg_len, &g_item.nav, &seq);
    if (status != NAV_DECODE_OK)
    {
        g_decode_errors++;
        g_last_error = status;
        return;
    }
    if (g_have_seq)
    {
        g_lost_by_seq += (uint8_t)(seq - g_last_seq - 1);
    }
    g_have_seq = true;
    g_last_seq = seq;
    g_decoded++;

    g_item.rx_ms = millis();
    xQueueOverwrite(g_queue, &g_item);
}

bool nav_rx_latest(nav_data_t *nav, uint32_t *age_ms)
{
    static RxItem item; // ~600 bytes, kept off the loop task's stack
    if (xQueuePeek(g_queue, &item, 0) != pdTRUE)
    {
        return false;
    }
    *nav = item.nav;
    *age_ms = millis() - item.rx_ms;
    return true;
}

void nav_rx_log_stats(bool connected)
{
    static uint32_t last_ms = millis();
    static uint32_t last_decoded = 0;
    static bool was_connected = true; // one line at boot, then while connected
    uint32_t now = millis();
    if (now - last_ms < STATS_PERIOD_MS)
    {
        return;
    }
    if (!connected && !was_connected)
    {
        last_ms = now; // idle: nothing new to report
        return;
    }
    was_connected = connected;
    uint32_t decoded = g_decoded;
    Serial.printf("ble: %s | msgs %lu (%.1f/s) | frames: complete %lu, dropped %lu (missing chunk), bad %lu, dup %lu"
                  " | decode errors %lu%s%s | lost by seq %lu | internal heap free %u B (min %u, largest %u)\n",
                  connected ? "connected" : "disconnected",
                  (unsigned long)decoded, (decoded - last_decoded) * 1000.0f / (now - last_ms),
                  (unsigned long)g_frag.stats.complete, (unsigned long)g_frag.stats.dropped,
                  (unsigned long)g_frag.stats.bad_frames, (unsigned long)g_frag.stats.duplicates,
                  (unsigned long)g_decode_errors, g_decode_errors ? ", last: " : "",
                  g_decode_errors ? nav_decode_status_str(g_last_error) : "",
                  (unsigned long)g_lost_by_seq,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    last_ms = now;
    last_decoded = decoded;
}
