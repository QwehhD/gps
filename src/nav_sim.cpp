// Implementation of the navigation dummy-data simulator declared in
// nav_sim.h. Pure data generation only — no LVGL/TFT_eSPI/Wire includes so
// this file can be dropped in on hardware that has no sensors wired up yet.
#include "nav_sim.h"

#include <math.h>
#include <stdlib.h>

namespace
{

    constexpr uint32_t HEADING_PERIOD_MS = 13000;   // one full 0-360 sweep (10-15s range)
    constexpr uint32_t SPEED_TARGET_INTERVAL_MS = 2500;
    constexpr float SPEED_SMOOTH_RATE = 1.2f;       // higher = faster settle toward target
    constexpr float DISTANCE_START_M = 500.0f;
    constexpr float MIN_DISTANCE_SPEED_MS = 5.0f;   // keeps the countdown moving even at 0 km/h

    const nav_point_t PRESET_STRAIGHT[] = {
        {0, 0}, {0, 10}, {0, 20}, {0, 30},
    };
    const nav_point_t PRESET_LEFT[] = {
        {0, 0}, {0, 10}, {0, 18}, {-8, 24}, {-18, 25},
    };
    const nav_point_t PRESET_RIGHT[] = {
        {0, 0}, {0, 10}, {0, 18}, {8, 24}, {18, 25},
    };
    const nav_point_t PRESET_UTURN[] = {
        {0, 0}, {0, 12}, {6, 16}, {10, 10}, {8, 2}, {2, -4},
    };

    struct PresetInfo
    {
        const nav_point_t *points;
        uint8_t count;
    };

    const PresetInfo PRESETS[NAV_MANEUVER_COUNT] = {
        {PRESET_STRAIGHT, sizeof(PRESET_STRAIGHT) / sizeof(PRESET_STRAIGHT[0])},
        {PRESET_LEFT, sizeof(PRESET_LEFT) / sizeof(PRESET_LEFT[0])},
        {PRESET_RIGHT, sizeof(PRESET_RIGHT) / sizeof(PRESET_RIGHT[0])},
        {PRESET_UTURN, sizeof(PRESET_UTURN) / sizeof(PRESET_UTURN[0])},
    };

    nav_data_t g_nav;
    uint32_t g_last_update_ms = 0;
    uint32_t g_next_speed_target_ms = 0;
    float g_speed_target = 0.0f;

    float random_range(float lo, float hi)
    {
        return lo + (hi - lo) * (static_cast<float>(rand()) / static_cast<float>(RAND_MAX));
    }

    void apply_preset(nav_maneuver_t maneuver)
    {
        const PresetInfo &preset = PRESETS[maneuver];
        g_nav.schematic_point_count = preset.count;
        for (uint8_t i = 0; i < preset.count; i++)
        {
            g_nav.schematic_points[i] = preset.points[i];
        }
    }

    void advance_to_next_leg()
    {
        g_nav.maneuver = static_cast<nav_maneuver_t>((g_nav.maneuver + 1) % NAV_MANEUVER_COUNT);
        g_nav.distance_to_turn_m = DISTANCE_START_M;
        g_nav.total_distance_m = random_range(300.0f, 1500.0f);
        apply_preset(g_nav.maneuver);
    }

} // namespace

void nav_sim_init(void)
{
    g_nav.bearing_deg = 0.0f;
    g_nav.speed_kmh = 0.0f;
    g_nav.distance_to_turn_m = DISTANCE_START_M;
    g_nav.maneuver = NAV_MANEUVER_STRAIGHT;
    g_nav.total_distance_m = random_range(300.0f, 1500.0f);
    g_nav.ble_connected = false;
    apply_preset(g_nav.maneuver);

    g_last_update_ms = 0;
    g_next_speed_target_ms = 0;
    g_speed_target = g_nav.speed_kmh;
}

void nav_sim_update(uint32_t now_ms)
{
    if (g_last_update_ms == 0)
    {
        g_last_update_ms = now_ms;
    }
    float dt_s = (now_ms - g_last_update_ms) / 1000.0f;
    g_last_update_ms = now_ms;
    if (dt_s < 0.0f || dt_s > 1.0f)
    {
        dt_s = 0.0f; // guard against millis() wraparound / the very first call
    }

    // Heading is a pure function of time, so the sweep stays smooth and
    // repeatable no matter how often/irregularly this is called.
    g_nav.bearing_deg = fmodf((float)(now_ms % HEADING_PERIOD_MS) / HEADING_PERIOD_MS * 360.0f, 360.0f);

    // Speed: pick a new random target every couple of seconds, then ease
    // toward it every frame so the UI never sees a hard jump.
    if (now_ms >= g_next_speed_target_ms)
    {
        g_speed_target = random_range(0.0f, 120.0f);
        g_next_speed_target_ms = now_ms + SPEED_TARGET_INTERVAL_MS;
    }
    g_nav.speed_kmh += (g_speed_target - g_nav.speed_kmh) * fminf(1.0f, dt_s * SPEED_SMOOTH_RATE);

    // Distance-to-turn counts down as if the simulated speed were being
    // covered; wrap to the next maneuver/preset once it reaches zero.
    float speed_ms = fmaxf(g_nav.speed_kmh / 3.6f, MIN_DISTANCE_SPEED_MS);
    g_nav.distance_to_turn_m -= speed_ms * dt_s;
    if (g_nav.distance_to_turn_m <= 0.0f)
    {
        advance_to_next_leg();
    }
}

const nav_data_t *nav_sim_get_data(void)
{
    return &g_nav;
}
