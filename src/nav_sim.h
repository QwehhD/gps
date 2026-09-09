// Navigation dummy-data simulator.
//
// Generates fake but smoothly-changing navigation data (heading, speed,
// distance-to-turn, maneuver type, schematic road-shape points) so the LVGL
// UI can be validated on real hardware before any sensor (QMC5883L), IMU or
// BLE phone-app link exists. See docs/navigation-protocol.md for the field
// meanings and how this maps to the future BLE payload.
//
// This module has no dependency on TFT_eSPI/LVGL/Wire — it only produces
// data. Swapping dummy data for real sensor/BLE data later means replacing
// the body of nav_sim_update() (or bypassing this module entirely) without
// touching any UI code, since both sides only ever see `nav_data_t`.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Set to 1 to drive the UI entirely from this simulator (no QMC5883L I2C
// traffic, no real GPS/BLE data is read). Set to 0 once real sensors/BLE
// are wired up.
#define USE_DUMMY_DATA 1

#ifdef __cplusplus
extern "C"
{
#endif

    // Maneuver icon shown for the upcoming turn.
    typedef enum
    {
        NAV_MANEUVER_STRAIGHT = 0,
        NAV_MANEUVER_TURN_LEFT,
        NAV_MANEUVER_TURN_RIGHT,
        NAV_MANEUVER_U_TURN,
        NAV_MANEUVER_COUNT
    } nav_maneuver_t;

    // A single point of the schematic road-shape line, in local/relative
    // coordinates (NOT lat/lon), heading-up oriented, arbitrary small unit
    // scale (e.g. meters ahead / meters left-right) centered on the rider.
    typedef struct
    {
        float x;
        float y;
    } nav_point_t;

#define NAV_SCHEMATIC_MAX_POINTS 8

    // Mirrors the planned BLE "NAV:" payload fields one-to-one, plus a
    // connection flag for the (not-yet-implemented) phone-app link. This is
    // the single struct the UI layer reads from, regardless of whether the
    // data actually came from this simulator or from real sensors/BLE.
    typedef struct
    {
        float bearing_deg;                                  // 0-359.9, compass heading
        float distance_to_turn_m;                            // counts down toward the next maneuver
        nav_maneuver_t maneuver;                              // upcoming maneuver type
        float total_distance_m;                               // remaining distance for the whole route
        float speed_kmh;                                      // current speed
        nav_point_t schematic_points[NAV_SCHEMATIC_MAX_POINTS]; // road-shape preview, heading-up, local coords
        uint8_t schematic_point_count;
        bool ble_connected; // placeholder, always false in dummy mode
    } nav_data_t;

    // Resets internal simulator state. Call once from setup().
    void nav_sim_init(void);

    // Advances the simulation. Call every loop() iteration (cheap) so the
    // heading sweep and interpolated values stay smooth; the underlying
    // "target" values only actually change every couple of seconds.
    void nav_sim_update(uint32_t now_ms);

    // Latest simulated data. Pointer stays valid for the program's lifetime.
    const nav_data_t *nav_sim_get_data(void);

#ifdef __cplusplus
}
#endif
