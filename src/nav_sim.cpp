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
    constexpr float TURN_SPEED_UNITS_S = 14.0f;     // how fast the rider rolls through a junction
    constexpr float HEADING_LOOKAHEAD = 3.0f;       // units behind/ahead used to smooth the heading

    // Each leg is a fixed road shape the rider "drives" along. The preview
    // fills the whole round screen (~4 px per unit), so every road runs past
    // the screen edge — the last point is deliberately far out so the line
    // never visibly ends mid-screen.
    const nav_point_t PRESET_STRAIGHT[] = {
        {0, 0}, {0, 10}, {0, 20}, {0, 34},
    };
    const nav_point_t PRESET_LEFT[] = {
        {0, 0}, {0, 9}, {-1, 14}, {-5, 18}, {-12, 19}, {-34, 19},
    };
    const nav_point_t PRESET_RIGHT[] = {
        {0, 0}, {0, 9}, {1, 14}, {5, 18}, {12, 19}, {34, 19},
    };
    const nav_point_t PRESET_UTURN[] = {
        {0, 0}, {0, 12}, {2, 18}, {8, 21}, {14, 18}, {16, 12}, {16, 0}, {16, -12},
    };

    // Posted limits cycled per leg, so the sign changes while testing.
    const uint16_t SPEED_LIMITS_KMH[] = {40, 50, 60, 70};
    uint8_t g_limit_index = 0;

    struct PresetInfo
    {
        const nav_point_t *points;
        uint8_t count;
        float turn_s; // road distance from the leg start to the junction
        float exit_s; // road distance where the rider has finished turning
    };

    // While the distance counts down from 500 m to 0 the rider moves from the
    // start of the road to turn_s, so the junction reaches the arrow right as
    // the distance hits 0. Then the rider rolls on to exit_s; since the view
    // is heading-up, the road swings around the arrow through the turn.
    const PresetInfo PRESETS[NAV_MANEUVER_COUNT] = {
        {PRESET_STRAIGHT, sizeof(PRESET_STRAIGHT) / sizeof(PRESET_STRAIGHT[0]), 10.0f, 10.0f},
        {PRESET_LEFT, sizeof(PRESET_LEFT) / sizeof(PRESET_LEFT[0]), 12.0f, 31.0f},
        {PRESET_RIGHT, sizeof(PRESET_RIGHT) / sizeof(PRESET_RIGHT[0]), 12.0f, 31.0f},
        {PRESET_UTURN, sizeof(PRESET_UTURN) / sizeof(PRESET_UTURN[0]), 10.0f, 44.0f},
    };

    nav_data_t g_nav;
    uint32_t g_last_update_ms = 0;
    uint32_t g_next_speed_target_ms = 0;
    float g_speed_target = 0.0f;
    bool g_turning = false; // rider is rolling through the junction
    float g_turn_elapsed_s = 0.0f;

    float random_range(float lo, float hi)
    {
        return lo + (hi - lo) * (static_cast<float>(rand()) / static_cast<float>(RAND_MAX));
    }

    // Point on the preset at road distance s; the first/last segment is
    // extended in a straight line before the start and past the end.
    nav_point_t preset_point_at(const PresetInfo &preset, float s)
    {
        const nav_point_t *p = preset.points;
        uint8_t i = 0;
        float seg_start = 0.0f;
        float len = hypotf(p[1].x - p[0].x, p[1].y - p[0].y);
        while (i + 2 < preset.count && s > seg_start + len)
        {
            seg_start += len;
            i++;
            len = hypotf(p[i + 1].x - p[i].x, p[i + 1].y - p[i].y);
        }
        float t = len > 0.0f ? (s - seg_start) / len : 0.0f;
        nav_point_t out = {p[i].x + (p[i + 1].x - p[i].x) * t, p[i].y + (p[i + 1].y - p[i].y) * t};
        return out;
    }

    // Puts the rider at road distance s and writes the whole road heading-up
    // around them: rider at the origin, direction of travel = +y. The heading
    // follows the road through a short look-behind/look-ahead chord, so it
    // turns smoothly instead of snapping at each preset corner.
    void place_rider(nav_maneuver_t maneuver, float s)
    {
        const PresetInfo &preset = PRESETS[maneuver];
        nav_point_t pos = preset_point_at(preset, s);
        nav_point_t back = preset_point_at(preset, s - HEADING_LOOKAHEAD);
        nav_point_t ahead = preset_point_at(preset, s + HEADING_LOOKAHEAD);
        float hx = ahead.x - back.x;
        float hy = ahead.y - back.y;
        float hl = hypotf(hx, hy);
        if (hl > 0.0f)
        {
            hx /= hl;
            hy /= hl;
        }
        else
        {
            hx = 0.0f;
            hy = 1.0f;
        }

        g_nav.schematic_point_count = preset.count;
        for (uint8_t i = 0; i < preset.count; i++)
        {
            float dx = preset.points[i].x - pos.x;
            float dy = preset.points[i].y - pos.y;
            g_nav.schematic_points[i].x = dx * hy - dy * hx;
            g_nav.schematic_points[i].y = dx * hx + dy * hy;
        }
    }

    // Rider position along the road while approaching the junction.
    float approach_s(nav_maneuver_t maneuver)
    {
        float leg_progress = 1.0f - g_nav.distance_to_turn_m / DISTANCE_START_M;
        return PRESETS[maneuver].turn_s * fminf(fmaxf(leg_progress, 0.0f), 1.0f);
    }

    void advance_to_next_leg()
    {
        g_nav.maneuver = static_cast<nav_maneuver_t>((g_nav.maneuver + 1) % NAV_MANEUVER_COUNT);
        g_nav.distance_to_turn_m = DISTANCE_START_M;
        g_nav.total_distance_m = random_range(300.0f, 1500.0f);
        g_limit_index = (g_limit_index + 1) % (sizeof(SPEED_LIMITS_KMH) / sizeof(SPEED_LIMITS_KMH[0]));
        g_nav.speed_limit_kmh = SPEED_LIMITS_KMH[g_limit_index];
        place_rider(g_nav.maneuver, approach_s(g_nav.maneuver));
    }

} // namespace

void nav_sim_init(void)
{
    g_nav.bearing_deg = 0.0f;
    g_nav.speed_kmh = 0.0f;
    g_nav.distance_to_turn_m = DISTANCE_START_M;
    g_nav.maneuver = NAV_MANEUVER_STRAIGHT;
    g_nav.total_distance_m = random_range(300.0f, 1500.0f);
    g_limit_index = 0;
    g_nav.speed_limit_kmh = SPEED_LIMITS_KMH[g_limit_index];
    g_nav.ble_connected = false;
    g_turning = false;
    g_turn_elapsed_s = 0.0f;
    place_rider(g_nav.maneuver, approach_s(g_nav.maneuver));

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

    if (g_turning)
    {
        // At the junction: distance stays 0 while the rider rolls from turn_s
        // to exit_s (eased in and out), then the next leg starts from there.
        const PresetInfo &preset = PRESETS[g_nav.maneuver];
        float duration_s = (preset.exit_s - preset.turn_s) / TURN_SPEED_UNITS_S;
        g_turn_elapsed_s += dt_s;
        float t = g_turn_elapsed_s / duration_s;
        if (t >= 1.0f)
        {
            g_turning = false;
            advance_to_next_leg();
        }
        else
        {
            float eased = t * t * (3.0f - 2.0f * t); // smoothstep
            place_rider(g_nav.maneuver, preset.turn_s + (preset.exit_s - preset.turn_s) * eased);
        }
        return;
    }

    // Distance-to-turn counts down as if the simulated speed were being
    // covered. At zero the rider goes through the junction (if this maneuver
    // turns at all), then the next maneuver/preset starts.
    float speed_ms = fmaxf(g_nav.speed_kmh / 3.6f, MIN_DISTANCE_SPEED_MS);
    g_nav.distance_to_turn_m -= speed_ms * dt_s;
    if (g_nav.distance_to_turn_m <= 0.0f)
    {
        g_nav.distance_to_turn_m = 0.0f;
        if (PRESETS[g_nav.maneuver].exit_s > PRESETS[g_nav.maneuver].turn_s)
        {
            g_turning = true;
            g_turn_elapsed_s = 0.0f;
            place_rider(g_nav.maneuver, PRESETS[g_nav.maneuver].turn_s);
        }
        else
        {
            advance_to_next_leg();
        }
    }
    else
    {
        place_rider(g_nav.maneuver, approach_s(g_nav.maneuver));
    }
}

const nav_data_t *nav_sim_get_data(void)
{
    return &g_nav;
}
