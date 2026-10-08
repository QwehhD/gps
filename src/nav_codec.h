// Binary wire format for navigation data sent from the phone over BLE.
//
// One 'N' message carries one complete route window (the whole nav_data_t
// the display needs), little-endian:
//
//   offset  size  field
//   0       1     message type, 'N' (0x4E)
//   1       1     sequence number, wraps 255 -> 0 (lost/reordered detection)
//   2       1     maneuver (nav_maneuver_t)
//   3       1     flags: bit 0 = route valid (set exactly when N >= 2),
//                 bits 1-7 reserved (sent as 0, ignored on receive)
//   4       2     distance_to_turn_m, uint16, metres
//   6       2     total_distance_m, uint16, tens of metres
//   8       1     speed_kmh, uint8
//   9       1     route point count N (0-32)
//   10      4*N   route points: int16 x, int16 y, in 1/16 unit
//   ...     1     side road count M (0-8)
//   ...           per side road: point count K (2-4), then K * (int16 x, int16 y)
//
// Coordinates are rounded to the nearest 1/16 unit, so every decoded point
// lies within 1/32 unit (0.125 px at 4 px per unit) of the original; the
// int16 range covers +-2048 units. bearing_deg and ble_connected are not on
// the wire. There is no checksum: the BLE link layer already has a CRC.
//
// Plain C with no Arduino/LVGL dependency, so the host unit tests
// (pio test -e native) build it as-is.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nav_sim.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define NAV_MSG_TYPE_NAV 0x4E // 'N'
#define NAV_MSG_HEADER_SIZE 10
#define NAV_MSG_POINT_SIZE 4
#define NAV_MSG_MAX_SIZE (NAV_MSG_HEADER_SIZE + NAV_MSG_POINT_SIZE * NAV_SCHEMATIC_MAX_POINTS + 1 + \
                          NAV_SIDE_ROADS_MAX * (1 + NAV_MSG_POINT_SIZE * NAV_SIDE_ROAD_MAX_POINTS)) // 275

#define NAV_MSG_FLAG_ROUTE_VALID 0x01

// Coordinates travel as int16 in 1/NAV_MSG_COORD_SCALE unit.
#define NAV_MSG_COORD_SCALE 16

    typedef enum
    {
        NAV_DECODE_OK = 0,
        NAV_DECODE_ERR_ARG,         // buf or out is NULL
        NAV_DECODE_ERR_TRUNCATED,   // packet ends before its counts say it should
        NAV_DECODE_ERR_LENGTH,      // bytes left over after the last field
        NAV_DECODE_ERR_TYPE,        // first byte is not a known message type
        NAV_DECODE_ERR_MANEUVER,    // maneuver outside nav_maneuver_t
        NAV_DECODE_ERR_FLAGS,       // route-valid bit disagrees with N
        NAV_DECODE_ERR_ROUTE_COUNT, // N > NAV_SCHEMATIC_MAX_POINTS
        NAV_DECODE_ERR_SIDE_COUNT,  // M > NAV_SIDE_ROADS_MAX
        NAV_DECODE_ERR_SIDE_POINTS, // a side road's K outside 2..NAV_SIDE_ROAD_MAX_POINTS
    } nav_decode_status_t;

    // Packs nav into buf as one 'N' message. Out-of-range values are clamped
    // to what the field can hold (counts to their maximum, distances and
    // speed to 0..max, coordinates to the int16 range, NaN to 0), and side
    // roads with fewer than 2 points are left out, so every packet it writes
    // decodes. Returns the packet length, or 0 if an argument is NULL, the
    // maneuver is not a nav_maneuver_t value, or cap is too small
    // (NAV_MSG_MAX_SIZE always fits).
    size_t nav_encode(const nav_data_t *nav, uint8_t *buf, size_t cap, uint8_t seq);

    // Unpacks one 'N' message. The whole packet is validated before anything
    // is written, so on error *out and *seq are left untouched. On success
    // every field of *out is set; bearing_deg and ble_connected, which are
    // not on the wire, become 0/false for the caller to fill in. seq may be
    // NULL.
    nav_decode_status_t nav_decode(const uint8_t *buf, size_t len, nav_data_t *out, uint8_t *seq);

    // Short name of a status for logs, e.g. "truncated".
    const char *nav_decode_status_str(nav_decode_status_t status);

#ifdef __cplusplus
}
#endif
