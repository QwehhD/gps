// Implementation of the navigation dummy-data simulator declared in
// nav_sim.h. Pure data generation only — no LVGL/TFT_eSPI/Wire includes so
// this file can be dropped in on hardware that has no sensors wired up yet.
//
// The simulator keeps an endless random route in "world" coordinates
// (straight stretches joined by junctions), generated a little ahead of the
// rider and forgotten a little behind. The rider drives along it; every
// update the part of the route around the rider is written into nav_data_t
// heading-up, so upcoming turns are already visible before they are reached
// and passing a junction never swaps the picture.
#include "nav_sim.h"

#include <math.h>
#include <stdlib.h>

namespace
{

    constexpr uint32_t HEADING_PERIOD_MS = 13000;   // one full 0-360 sweep (10-15s range)
    constexpr uint32_t SPEED_TARGET_INTERVAL_MS = 2500;
    constexpr float SPEED_SMOOTH_RATE = 1.2f;       // higher = faster settle toward target
    constexpr float MIN_DISTANCE_SPEED_MS = 5.0f;   // keeps the rider moving even at 0 km/h
    constexpr float kPi = 3.14159265f;
    constexpr float kDegToRad = kPi / 180.0f;

    // One schematic unit (~4 px on screen) stands for this much real road.
    constexpr float M_PER_UNIT = 15.0f;

    constexpr float HEADING_LOOKAHEAD = 3.0f; // units behind/ahead used to smooth the heading
    constexpr float VIEW_BEHIND = 12.0f;      // route sent to the UI behind the rider, units
    constexpr float VIEW_AHEAD = 80.0f;       // ...and ahead of the rider (past the screen edge)
    constexpr float GENERATE_AHEAD = 110.0f;  // keep at least this much route generated ahead
    constexpr float FORGET_BEHIND = 30.0f;    // drop route this far behind the rider
    constexpr float CORNER_CUT = 2.0f;        // junction corners are chamfered by this much
    constexpr float UTURN_RADIUS = 2.5f;

    struct RoadPoint
    {
        float x, y; // world coordinates, units
        float s;    // road distance from the start of the route
    };

    struct SideRoad
    {
        float x[NAV_SIDE_ROAD_MAX_POINTS]; // world polyline, point 0 on the route
        float y[NAV_SIDE_ROAD_MAX_POINTS];
        float s; // road distance of the spot where it meets the route
    };

    struct Junction
    {
        float s; // road distance of the junction
        nav_maneuver_t maneuver;
    };

    constexpr int MAX_ROAD_POINTS = 64;
    constexpr int MAX_SIDE_ROADS = 96;
    constexpr int MAX_JUNCTIONS = 16;

    RoadPoint g_road[MAX_ROAD_POINTS];
    int g_road_count = 0;
    SideRoad g_side[MAX_SIDE_ROADS];
    int g_side_count = 0;
    Junction g_junction[MAX_JUNCTIONS];
    int g_junction_count = 0;

    float g_end_heading = 0.0f; // world direction (radians, 0 = +x) at the end of the route
    float g_rider_s = 0.0f;

    nav_data_t g_nav;
    uint32_t g_last_update_ms = 0;
    uint32_t g_next_speed_target_ms = 0;
    float g_speed_target = 0.0f;

    float random_range(float lo, float hi)
    {
        return lo + (hi - lo) * (static_cast<float>(rand()) / static_cast<float>(RAND_MAX));
    }

    bool chance(float p)
    {
        return random_range(0.0f, 1.0f) < p;
    }

    void add_road_point(float x, float y)
    {
        if (g_road_count >= MAX_ROAD_POINTS)
        {
            return;
        }
        float s = 0.0f;
        if (g_road_count > 0)
        {
            const RoadPoint &prev = g_road[g_road_count - 1];
            s = prev.s + hypotf(x - prev.x, y - prev.y);
        }
        g_road[g_road_count++] = {x, y, s};
    }

    // Side roads are short polylines. About half of them bend gently (up to
    // ~50 degrees spread over their length) so the view isn't all straight
    // stubs.
    void add_side_road(float x, float y, float angle, float length, float s)
    {
        if (g_side_count >= MAX_SIDE_ROADS)
        {
            return;
        }
        constexpr int segments = NAV_SIDE_ROAD_MAX_POINTS - 1;
        float bend = chance(0.5f) ? random_range(-50.0f, 50.0f) * kDegToRad : 0.0f;

        SideRoad &road = g_side[g_side_count++];
        road.s = s;
        road.x[0] = x;
        road.y[0] = y;
        for (int k = 0; k < segments; k++)
        {
            float heading = angle + bend * (k + 0.5f) / segments;
            road.x[k + 1] = road.x[k] + cosf(heading) * length / segments;
            road.y[k + 1] = road.y[k] + sinf(heading) * length / segments;
        }
    }

    // Branches at a junction: any of straight-on / left / right that the
    // route itself does not take, each with some probability.
    void add_junction_branches(float x, float y, float in_angle, float out_angle, float s, float p)
    {
        const float candidates[3] = {in_angle, in_angle + kPi / 2.0f, in_angle - kPi / 2.0f};
        for (float c : candidates)
        {
            float diff = fabsf(remainderf(c - out_angle, 2.0f * kPi));
            if (diff < 35.0f * kDegToRad || !chance(p))
            {
                continue;
            }
            add_side_road(x, y, c + random_range(-12.0f, 12.0f) * kDegToRad, random_range(8.0f, 14.0f), s);
        }
    }

    // Appends one straight stretch plus the junction at its end.
    void generate_leg()
    {
        const RoadPoint &end = g_road[g_road_count - 1];
        float a = g_end_heading;
        float straight = random_range(12.0f, 26.0f);
        float jx = end.x + cosf(a) * straight;
        float jy = end.y + sinf(a) * straight;

        // A minor cross street somewhere along the stretch.
        if (chance(0.45f))
        {
            float t = random_range(0.35f, 0.65f) * straight;
            float mx = end.x + cosf(a) * t;
            float my = end.y + sinf(a) * t;
            float cross = a + kPi / 2.0f + random_range(-10.0f, 10.0f) * kDegToRad;
            if (chance(0.8f))
            {
                add_side_road(mx, my, cross, random_range(6.0f, 11.0f), end.s + t);
            }
            if (chance(0.8f))
            {
                add_side_road(mx, my, cross + kPi, random_range(6.0f, 11.0f), end.s + t);
            }
        }

        float r = random_range(0.0f, 1.0f);
        nav_maneuver_t maneuver;
        float turn;
        if (r < 0.38f)
        {
            maneuver = NAV_MANEUVER_TURN_LEFT;
            turn = random_range(55.0f, 100.0f) * kDegToRad;
        }
        else if (r < 0.76f)
        {
            maneuver = NAV_MANEUVER_TURN_RIGHT;
            turn = -random_range(55.0f, 100.0f) * kDegToRad;
        }
        else if (r < 0.92f)
        {
            maneuver = NAV_MANEUVER_STRAIGHT;
            turn = 0.0f;
        }
        else
        {
            maneuver = NAV_MANEUVER_U_TURN;
            turn = -kPi; // loops around to the right
        }
        float b = a + turn;
        float junction_s;

        if (maneuver == NAV_MANEUVER_STRAIGHT)
        {
            add_road_point(jx, jy);
            junction_s = g_road[g_road_count - 1].s;
            add_junction_branches(jx, jy, a, b, junction_s, 0.85f);
        }
        else if (maneuver == NAV_MANEUVER_U_TURN)
        {
            float rx = sinf(a), ry = -cosf(a); // rider's right
            add_road_point(jx, jy);
            junction_s = g_road[g_road_count - 1].s + UTURN_RADIUS;
            add_road_point(jx + (cosf(a) + rx) * UTURN_RADIUS, jy + (sinf(a) + ry) * UTURN_RADIUS);
            add_road_point(jx + rx * 2.0f * UTURN_RADIUS, jy + ry * 2.0f * UTURN_RADIUS);
            add_junction_branches(jx, jy, a, b, junction_s, 0.5f);
        }
        else
        {
            add_road_point(jx - cosf(a) * CORNER_CUT, jy - sinf(a) * CORNER_CUT);
            junction_s = g_road[g_road_count - 1].s + CORNER_CUT;
            add_road_point(jx + cosf(b) * CORNER_CUT, jy + sinf(b) * CORNER_CUT);
            add_junction_branches(jx, jy, a, b, junction_s, 0.6f);
        }

        if (g_junction_count < MAX_JUNCTIONS)
        {
            g_junction[g_junction_count++] = {junction_s, maneuver};
        }
        g_end_heading = b;
    }

    // Keeps the route generated ahead of the rider and drops what is far
    // behind, so the fixed-size arrays never run out.
    void maintain_route()
    {
        while (g_road[g_road_count - 1].s < g_rider_s + GENERATE_AHEAD &&
               g_road_count + 3 <= MAX_ROAD_POINTS && g_junction_count < MAX_JUNCTIONS)
        {
            generate_leg();
        }

        int drop = 0;
        while (drop + 2 < g_road_count && g_road[drop + 1].s < g_rider_s - FORGET_BEHIND)
        {
            drop++;
        }
        for (int i = drop; i < g_road_count; i++)
        {
            g_road[i - drop] = g_road[i];
        }
        g_road_count -= drop;

        int keep = 0;
        for (int i = 0; i < g_side_count; i++)
        {
            if (g_side[i].s >= g_rider_s - FORGET_BEHIND)
            {
                g_side[keep++] = g_side[i];
            }
        }
        g_side_count = keep;

        keep = 0;
        for (int i = 0; i < g_junction_count; i++)
        {
            if (g_junction[i].s > g_rider_s)
            {
                g_junction[keep++] = g_junction[i];
            }
        }
        g_junction_count = keep;
    }

    // World point at road distance s (clamped to the generated route).
    void road_point_at(float s, float *x, float *y)
    {
        int i = 0;
        while (i + 2 < g_road_count && s > g_road[i + 1].s)
        {
            i++;
        }
        const RoadPoint &a = g_road[i];
        const RoadPoint &b = g_road[i + 1];
        float len = b.s - a.s;
        float t = len > 0.0f ? (s - a.s) / len : 0.0f;
        t = fminf(fmaxf(t, 0.0f), 1.0f);
        *x = a.x + (b.x - a.x) * t;
        *y = a.y + (b.y - a.y) * t;
    }

    // Writes the route window and nearby side roads into g_nav, heading-up
    // around the rider: rider at the origin, direction of travel = +y. The
    // heading follows the road through a short look-behind/look-ahead chord,
    // so the view turns smoothly through junctions.
    void publish_view()
    {
        float px, py, bx, by, ax, ay;
        road_point_at(g_rider_s, &px, &py);
        road_point_at(g_rider_s - HEADING_LOOKAHEAD, &bx, &by);
        road_point_at(g_rider_s + HEADING_LOOKAHEAD, &ax, &ay);
        float hx = ax - bx;
        float hy = ay - by;
        float hl = hypotf(hx, hy);
        hx = hl > 0.0f ? hx / hl : 0.0f;
        hy = hl > 0.0f ? hy / hl : 1.0f;

        auto to_local = [&](float wx, float wy) {
            float dx = wx - px;
            float dy = wy - py;
            nav_point_t p = {dx * hy - dy * hx, dx * hx + dy * hy};
            return p;
        };

        // Route: exact cut at the window start, the corners inside, exact cut
        // at the window end. If the corners don't all fit, the line simply
        // stops at the last one that did rather than cutting across.
        float s_from = g_rider_s - VIEW_BEHIND;
        float s_to = g_rider_s + VIEW_AHEAD;
        uint8_t n = 0;
        bool truncated = false;
        float wx, wy;
        road_point_at(s_from, &wx, &wy);
        g_nav.schematic_points[n++] = to_local(wx, wy);
        for (int i = 0; i < g_road_count && !truncated; i++)
        {
            if (g_road[i].s > s_from && g_road[i].s < s_to)
            {
                if (n < NAV_SCHEMATIC_MAX_POINTS - 1)
                {
                    g_nav.schematic_points[n++] = to_local(g_road[i].x, g_road[i].y);
                }
                else
                {
                    truncated = true;
                }
            }
        }
        if (!truncated)
        {
            road_point_at(s_to, &wx, &wy);
            g_nav.schematic_points[n++] = to_local(wx, wy);
        }
        g_nav.schematic_point_count = n;

        uint8_t m = 0;
        for (int i = 0; i < g_side_count && m < NAV_SIDE_ROADS_MAX; i++)
        {
            if (g_side[i].s >= s_from && g_side[i].s <= s_to)
            {
                for (uint8_t k = 0; k < NAV_SIDE_ROAD_MAX_POINTS; k++)
                {
                    g_nav.side_roads[m].points[k] = to_local(g_side[i].x[k], g_side[i].y[k]);
                }
                g_nav.side_roads[m].point_count = NAV_SIDE_ROAD_MAX_POINTS;
                m++;
            }
        }
        g_nav.side_road_count = m;

        if (g_junction_count > 0)
        {
            g_nav.maneuver = g_junction[0].maneuver;
            g_nav.distance_to_turn_m = (g_junction[0].s - g_rider_s) * M_PER_UNIT;
            g_nav.total_distance_m = (g_junction[g_junction_count - 1].s - g_rider_s) * M_PER_UNIT;
        }
    }

} // namespace

void nav_sim_init(void)
{
    g_nav.bearing_deg = 0.0f;
    g_nav.speed_kmh = 0.0f;
    g_nav.ble_connected = false;

    // Start on a straight road heading "north" (+y), with some road behind.
    g_road_count = 0;
    g_side_count = 0;
    g_junction_count = 0;
    add_road_point(0.0f, -VIEW_BEHIND - 8.0f);
    add_road_point(0.0f, 0.0f);
    g_end_heading = kPi / 2.0f;
    g_rider_s = g_road[1].s;
    maintain_route();
    publish_view();

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

    // Drive along the route at the simulated speed.
    float speed_ms = fmaxf(g_nav.speed_kmh / 3.6f, MIN_DISTANCE_SPEED_MS);
    g_rider_s += speed_ms * dt_s / M_PER_UNIT;
    maintain_route();
    publish_view();
}

const nav_data_t *nav_sim_get_data(void)
{
    return &g_nav;
}
