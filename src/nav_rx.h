// Receiving navigation data over BLE (NAV_SOURCE_BLE).
//
// The BLE callbacks run in the Bluetooth task, loop() and LVGL in the
// Arduino task. The Bluetooth side reassembles frames (nav_frag.h), decodes
// the message (nav_codec.h) and overwrites a FreeRTOS queue of length 1;
// loop() peeks the newest copy. Nothing here touches LVGL.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nav_sim.h"

// Data older than this counts as stale: the display shows "no signal".
#define NAV_RX_STALE_MS 3000

// Creates the queue. Call from setup() before BLE starts.
void nav_rx_init(void);

// Bluetooth task: link events and every write starting with NAV_FRAG_TYPE.
void nav_rx_on_connect(void);
void nav_rx_on_disconnect(void);
void nav_rx_on_frame(const uint8_t *frame, size_t len);

// loop(): copies the newest decoded message into *nav and its age in ms.
// Returns false if nothing valid arrived since the link came up.
bool nav_rx_latest(nav_data_t *nav, uint32_t *age_ms);

// loop(): prints reassembly/decode/loss counters and free internal heap
// every few seconds while connected (and once more after a disconnect).
void nav_rx_log_stats(bool connected);
