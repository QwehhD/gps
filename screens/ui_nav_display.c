#include "ui_nav_display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_Subtitle);

lv_obj_t *ui_nav_display = NULL;

static lv_obj_t *label_distance = NULL;
static lv_obj_t *label_unit = NULL;

// ---- Layout (px, 240x240 round panel) --------------------------------------
#define CENTER_X 120
#define CENTER_Y 120

// Road line: heading-up, rider at the arrow. Schematic units -> px.
#define ROUTE_ORIGIN_Y 132
#define ROUTE_PX_PER_UNIT 4
#define ROUTE_WIDTH 8

// Side roads: drawn as a road outline (two thin parallel edge lines, about as
// wide overall as the route) in grey-blue, fading out away from the route.
#define SIDE_EDGE_WIDTH 2
#define SIDE_HALF_GAP_PX 3.5f // each edge sits this far from the road's centre line
#define SIDE_FADE_STEPS 4

// Rider arrow (chevron) sits just above the top of the bottom panel. It is
// drawn as a raised 3D chevron: soft shadow, a darker extruded underside,
// and a lit left face next to a shaded right face.
#define ARROW_TIP_Y 118
#define ARROW_BASE_Y 148
#define ARROW_NOTCH_Y 140
#define ARROW_HALF_W 16
#define ARROW_DEPTH 3 // extrusion below the chevron, px

// Bottom panel is a dome: a big circle centered below the screen, so its top
// edge curves (peak just under the arrow, shoulders meeting the bezel around
// y=187) instead of being a straight cut.
#define DOME_CY 300
#define DOME_R 150
#define DOME_SHOULDER_Y 190 // lowest y the dark road area reaches on screen

// Maneuver icon lives in a 40x40 box at the left of the panel.
#define ICON_X 62
#define ICON_Y 161
#define ICON_BOX 40
#define ICON_STROKE 6

#define DIST_LABEL_X 109
#define DIST_LABEL_Y 155
#define UNIT_LABEL_X 111
#define UNIT_LABEL_Y 184

// Progress arc hugging the bottom edge, filling left -> right as the turn
// gets closer. It starts filling within PROGRESS_RANGE_M of the turn.
#define ARC_RADIUS 112
#define ARC_START_DEG 49
#define ARC_END_DEG 131
#define ARC_SPAN_DEG (ARC_END_DEG - ARC_START_DEG)
#define ARC_BG_WIDTH 3
#define ARC_FG_WIDTH 5
#define PROGRESS_RANGE_M 500.0f

#define COLOR_BG lv_color_hex(0x0C0C20)
#define COLOR_PANEL lv_color_hex(0x2C2C31)
#define COLOR_ARC_BG lv_color_hex(0x5A5A62)
#define COLOR_SIDE lv_color_hex(0x9AA1B5)
#define COLOR_ARROW_LIT lv_color_hex(0xFFFFFF)
#define COLOR_ARROW_SHADED lv_color_hex(0xC2C7D2)
#define COLOR_ARROW_EDGE_LIT lv_color_hex(0x8E94A3)
#define COLOR_ARROW_EDGE_SHADED lv_color_hex(0x5F6574)

// ---- Icon shapes (40x40 box, drawn pointing right/up) ----------------------
typedef struct
{
    int8_t x, y;
} icon_pt_t;

typedef struct
{
    const icon_pt_t *path;
    uint8_t path_count;
    icon_pt_t head[3]; // arrowhead triangle: tip + two base corners
} icon_shape_t;

static const icon_pt_t PATH_STRAIGHT[] = {{20, 38}, {20, 14}};
static const icon_pt_t PATH_TURN[] = {{7, 38}, {7, 25}, {9, 19}, {14, 15}, {21, 14}, {28, 14}};
static const icon_pt_t PATH_UTURN[] = {{8, 38}, {8, 15}, {11, 7}, {20, 4}, {29, 7}, {32, 15}, {32, 24}};

#define COUNT(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

static const icon_shape_t SHAPE_STRAIGHT = {PATH_STRAIGHT, COUNT(PATH_STRAIGHT), {{20, 0}, {11, 14}, {29, 14}}};
static const icon_shape_t SHAPE_TURN = {PATH_TURN, COUNT(PATH_TURN), {{40, 14}, {28, 5}, {28, 23}}};
static const icon_shape_t SHAPE_UTURN = {PATH_UTURN, COUNT(PATH_UTURN), {{32, 38}, {23, 24}, {41, 24}}};

// ---- Animation -------------------------------------------------------------
// set() only stores targets; a timer eases the shown values toward them, so
// the picture moves smoothly even if data arrives in steps (e.g. 1 Hz BLE).
#define ANIM_PERIOD_MS 20
#define EASE_RATE 10.0f     // 1/s, higher = snappier follow
#define SIDE_FADE_RATE 5.0f // 1/s, side roads fade in/out over ~200 ms

// The route is re-spaced into ROUTE_SAMPLES points ROUTE_SAMPLE_STEP units
// (5 px) apart along its length, counted from the rider: ROUTE_BEHIND_STEPS
// points behind the arrow (10 units), the rest ahead (80 units, well past
// the screen edge). Point i is then "the same spot relative to the rider" on
// any update, so the line glides instead of being redrawn from scratch. The
// spacing is dense enough that corners, drawn with the line's round caps,
// come out round and do not wobble as the samples slide along the road.
#define ROUTE_SAMPLE_STEP 1.25f
#define ROUTE_BEHIND_STEPS 8
#define ROUTE_SAMPLES (ROUTE_BEHIND_STEPS + 65)

// Before drawing, straight runs of samples are merged into single segments
// (Douglas-Peucker within this many px), so the line costs a dozen or so
// draw calls instead of ~70 while looking the same.
#define ROUTE_SIMPLIFY_PX 0.75f

// Room for every side road in the data plus the ones still fading out.
#define SIDE_SLOTS (NAV_SIDE_ROADS_MAX * 2)

// Screen areas redrawn while animating (the rest is left untouched).
#define ROUTE_AREA_Y2 DOME_SHOULDER_Y
#define ARC_AREA_X1 40
#define ARC_AREA_Y1 196
#define ARC_AREA_X2 200
#define ARC_AREA_Y2 236

// ---- View state (what the draw callback renders) ---------------------------
typedef struct
{
    float x, y;
} fpoint_t;

typedef struct
{
    fpoint_t from, to;               // shown, schematic units
    fpoint_t from_target, to_target; // latest data
    float opa;                       // 0..1, fades in while alive, out after
    bool alive;                      // present in the latest data
    lv_point_t from_px, to_px;       // what was last drawn
} side_view_t;

typedef struct
{
    fpoint_t route_target[ROUTE_SAMPLES]; // schematic units, evenly spaced
    fpoint_t route_shown[ROUTE_SAMPLES];
    bool route_valid; // false until a road with at least 2 points arrives

    lv_point_t route_px[ROUTE_SAMPLES]; // screen px
    uint16_t route_px_count;
    lv_point_t route_draw[ROUTE_SAMPLES]; // route_px simplified for drawing
    uint16_t route_draw_count;

    side_view_t sides[SIDE_SLOTS];

    nav_maneuver_t maneuver;

    float progress_target; // 0..1
    float progress_shown;
    int32_t progress_deg; // what was last drawn, 0..ARC_SPAN_DEG

    uint32_t last_tick;
} nav_view_t;

static nav_view_t view;
static lv_timer_t *anim_timer = NULL;
static char distance_text[12];
static char unit_text[3];

// Layer coordinates of the screen's top-left corner (0,0 on this device, but
// kept explicit so the drawing code stays position-independent).
static int32_t off_x, off_y;

static lv_point_t units_to_px(fpoint_t p)
{
    lv_point_t out = {CENTER_X + (int32_t)lroundf(p.x * ROUTE_PX_PER_UNIT),
                      ROUTE_ORIGIN_Y - (int32_t)lroundf(p.y * ROUTE_PX_PER_UNIT)};
    return out;
}

// ---- Small drawing helpers -------------------------------------------------
// Round ends are drawn by LVGL as extra circles, which cost more than the line
// itself, so callers only ask for the ones that are actually visible.
static void draw_line_ends(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           int32_t width, lv_color_t color, lv_opa_t opa, bool round_start, bool round_end)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = opa;
    dsc.round_start = round_start;
    dsc.round_end = round_end;
    dsc.p1.x = off_x + x1;
    dsc.p1.y = off_y + y1;
    dsc.p2.x = off_x + x2;
    dsc.p2.y = off_y + y2;
    lv_draw_line(layer, &dsc);
}

static void draw_line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      int32_t width, lv_color_t color, lv_opa_t opa)
{
    draw_line_ends(layer, x1, y1, x2, y2, width, color, opa, true, true);
}

static void draw_tri(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                     int32_t x3, int32_t y3, lv_color_t color, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = opa;
    dsc.p[0].x = off_x + x1;
    dsc.p[0].y = off_y + y1;
    dsc.p[1].x = off_x + x2;
    dsc.p[1].y = off_y + y2;
    dsc.p[2].x = off_x + x3;
    dsc.p[2].y = off_y + y3;
    lv_draw_triangle(layer, &dsc);
}

static void draw_arc(lv_layer_t *layer, int32_t start_deg, int32_t end_deg, int32_t width, lv_color_t color)
{
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.center.x = off_x + CENTER_X;
    dsc.center.y = off_y + CENTER_Y;
    dsc.radius = ARC_RADIUS;
    dsc.start_angle = start_deg;
    dsc.end_angle = end_deg;
    dsc.opa = LV_OPA_COVER;
    dsc.rounded = 1;
    lv_draw_arc(layer, &dsc);
}

// ---- Scene pieces, back to front -------------------------------------------
static void draw_side_roads(lv_layer_t *layer)
{
    for (uint8_t i = 0; i < SIDE_SLOTS; i++)
    {
        const side_view_t *side = &view.sides[i];
        if (side->opa <= 0.0f)
        {
            continue;
        }
        float ax = (float)side->from_px.x;
        float ay = (float)side->from_px.y;
        float dx = (float)side->to_px.x - ax;
        float dy = (float)side->to_px.y - ay;
        float len = hypotf(dx, dy);
        if (len < 1.0f)
        {
            continue;
        }
        // Offset from the centre line to one edge (perpendicular to the road).
        float nx = -dy / len * SIDE_HALF_GAP_PX;
        float ny = dx / len * SIDE_HALF_GAP_PX;

        // Fade by blending toward the background colour, as nested lines that
        // all start at the route: the full edge in the dimmest shade, then
        // shorter and brighter ones on top. Every piece shares the same start
        // pixel, so the thin edge stays straight instead of kinking where
        // separately rounded pieces would meet. No round ends: the start
        // hides under the route and the far end has faded out.
        for (int edge = -1; edge <= 1; edge += 2)
        {
            float ex = ax + nx * edge;
            float ey = ay + ny * edge;
            int32_t sx = (int32_t)lroundf(ex);
            int32_t sy = (int32_t)lroundf(ey);
            for (uint8_t k = 0; k < SIDE_FADE_STEPS; k++)
            {
                float reach = (float)(SIDE_FADE_STEPS - k) / SIDE_FADE_STEPS; // 1, 0.75, 0.5, 0.25
                float strength = side->opa * (1.0f - 0.9f * (reach - 1.0f / SIDE_FADE_STEPS));
                lv_color_t color = lv_color_mix(COLOR_SIDE, COLOR_BG, (uint8_t)(strength * 255.0f));
                draw_line_ends(layer, sx, sy,
                               (int32_t)lroundf(ex + dx * reach), (int32_t)lroundf(ey + dy * reach),
                               SIDE_EDGE_WIDTH, color, LV_OPA_COVER, false, false);
            }
        }
    }
}

static void draw_route(lv_layer_t *layer)
{
    // One round cap per joint is enough: each segment's round end also
    // covers the start of the next one.
    for (uint16_t i = 1; i < view.route_draw_count; i++)
    {
        draw_line_ends(layer, view.route_draw[i - 1].x, view.route_draw[i - 1].y,
                       view.route_draw[i].x, view.route_draw[i].y,
                       ROUTE_WIDTH, lv_color_white(), LV_OPA_COVER, i == 1, true);
    }
}

static void draw_panel(lv_layer_t *layer)
{
    // Drawn after the roads so anything running behind the rider is cut off
    // cleanly at the dome edge.
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = COLOR_PANEL;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;
    lv_area_t area = {off_x + CENTER_X - DOME_R, off_y + DOME_CY - DOME_R,
                      off_x + CENTER_X + DOME_R, off_y + DOME_CY + DOME_R};
    lv_draw_rect(layer, &dsc, &area);
}

static void draw_progress_arc(lv_layer_t *layer)
{
    draw_arc(layer, ARC_START_DEG, ARC_END_DEG, ARC_BG_WIDTH, COLOR_ARC_BG);
    if (view.progress_deg > 0)
    {
        draw_arc(layer, ARC_END_DEG - view.progress_deg, ARC_END_DEG, ARC_FG_WIDTH, lv_color_white());
    }
}

static void draw_rider_arrow(lv_layer_t *layer)
{
    const int32_t cx = CENTER_X;
    const int32_t lx = CENTER_X - ARROW_HALF_W;
    const int32_t rx = CENTER_X + ARROW_HALF_W;

    // Faint tail below the chevron, like a motion trail.
    draw_line(layer, cx, ARROW_NOTCH_Y, cx, ARROW_BASE_Y + 14, 6, lv_color_white(), LV_OPA_30);

    // Soft drop shadow: a few growing, offset triangles at low opacity whose
    // overlap darkens toward the middle.
    for (int32_t g = 3; g >= 1; g--)
    {
        draw_tri(layer, cx, ARROW_TIP_Y + 4 - g, lx - g, ARROW_BASE_Y + 5 + g, rx + g, ARROW_BASE_Y + 5 + g,
                 lv_color_black(), LV_OPA_20);
    }

    // Extruded underside: the two bottom edges pushed down by ARROW_DEPTH.
    draw_tri(layer, lx, ARROW_BASE_Y, cx, ARROW_NOTCH_Y, cx, ARROW_NOTCH_Y + ARROW_DEPTH,
             COLOR_ARROW_EDGE_LIT, LV_OPA_COVER);
    draw_tri(layer, lx, ARROW_BASE_Y, cx, ARROW_NOTCH_Y + ARROW_DEPTH, lx, ARROW_BASE_Y + ARROW_DEPTH,
             COLOR_ARROW_EDGE_LIT, LV_OPA_COVER);
    draw_tri(layer, cx, ARROW_NOTCH_Y, rx, ARROW_BASE_Y, rx, ARROW_BASE_Y + ARROW_DEPTH,
             COLOR_ARROW_EDGE_SHADED, LV_OPA_COVER);
    draw_tri(layer, cx, ARROW_NOTCH_Y, rx, ARROW_BASE_Y + ARROW_DEPTH, cx, ARROW_NOTCH_Y + ARROW_DEPTH,
             COLOR_ARROW_EDGE_SHADED, LV_OPA_COVER);

    // Top faces: light comes from the left.
    draw_tri(layer, cx, ARROW_TIP_Y, lx, ARROW_BASE_Y, cx, ARROW_NOTCH_Y, COLOR_ARROW_LIT, LV_OPA_COVER);
    draw_tri(layer, cx, ARROW_TIP_Y, cx, ARROW_NOTCH_Y, rx, ARROW_BASE_Y, COLOR_ARROW_SHADED, LV_OPA_COVER);
}

static void draw_maneuver_icon(lv_layer_t *layer)
{
    const icon_shape_t *shape = &SHAPE_STRAIGHT;
    bool mirror = false;

    switch (view.maneuver)
    {
    case NAV_MANEUVER_TURN_LEFT:
        shape = &SHAPE_TURN;
        mirror = true;
        break;
    case NAV_MANEUVER_TURN_RIGHT:
        shape = &SHAPE_TURN;
        break;
    case NAV_MANEUVER_U_TURN:
        shape = &SHAPE_UTURN;
        break;
    default:
        break;
    }

#define ICON_X_AT(px) (ICON_X + (mirror ? ICON_BOX - (px) : (px)))
#define ICON_Y_AT(py) (ICON_Y + (py))

    for (uint8_t i = 1; i < shape->path_count; i++)
    {
        draw_line_ends(layer,
                       ICON_X_AT(shape->path[i - 1].x), ICON_Y_AT(shape->path[i - 1].y),
                       ICON_X_AT(shape->path[i].x), ICON_Y_AT(shape->path[i].y),
                       ICON_STROKE, lv_color_white(), LV_OPA_COVER, i == 1, true);
    }
    draw_tri(layer,
             ICON_X_AT(shape->head[0].x), ICON_Y_AT(shape->head[0].y),
             ICON_X_AT(shape->head[1].x), ICON_Y_AT(shape->head[1].y),
             ICON_X_AT(shape->head[2].x), ICON_Y_AT(shape->head[2].y),
             lv_color_white(), LV_OPA_COVER);

#undef ICON_X_AT
#undef ICON_Y_AT
}

static void nav_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_obj_get_coords(lv_event_get_target_obj(e), &coords);
    off_x = coords.x1;
    off_y = coords.y1;

    draw_side_roads(layer);
    draw_route(layer);
    draw_panel(layer);
    draw_progress_arc(layer);
    draw_rider_arrow(layer);
    draw_maneuver_icon(layer);
}

// ---- Text ------------------------------------------------------------------
static lv_obj_t *make_label(const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(ui_nav_display);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

// Rounds like a real turn-by-turn display: 10 m steps from 100 m, 5 m steps
// below that, and kilometers with one decimal from ~1 km.
static void format_distance(float meters, char *num, size_t num_size, char *unit, size_t unit_size)
{
    if (meters < 0.0f)
    {
        meters = 0.0f;
    }
    if (meters >= 995.0f)
    {
        snprintf(num, num_size, "%.1f", meters / 1000.0f);
        snprintf(unit, unit_size, "km");
    }
    else
    {
        int step = meters >= 100.0f ? 10 : 5;
        snprintf(num, num_size, "%d", ((int)(meters / step + 0.5f)) * step);
        snprintf(unit, unit_size, "m");
    }
}

// ---- Route geometry --------------------------------------------------------
static void invalidate_rect(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t coords;
    lv_obj_get_coords(ui_nav_display, &coords);
    lv_area_t area = {coords.x1 + x1, coords.y1 + y1, coords.x1 + x2, coords.y1 + y2};
    lv_obj_invalidate_area(ui_nav_display, &area);
}

// Point on the polyline at road distance s. Before the start or past the end
// the first/last segment is extended in a straight line.
static fpoint_t road_point_at(const nav_point_t *p, const float *cum, uint8_t n, float s)
{
    uint8_t i = 0;
    while (i + 2 < n && s > cum[i + 1])
    {
        i++;
    }
    float len = cum[i + 1] - cum[i];
    float t = len > 0.0f ? (s - cum[i]) / len : 0.0f;
    fpoint_t out = {p[i].x + (p[i + 1].x - p[i].x) * t, p[i].y + (p[i + 1].y - p[i].y) * t};
    return out;
}

// Re-spaces the schematic polyline into ROUTE_SAMPLES evenly spaced points,
// anchored at the spot on the road closest to the rider (the origin).
// Returns false if there is no road to draw.
static bool resample_route(const nav_data_t *nav, fpoint_t *out)
{
    uint8_t n = nav->schematic_point_count;
    if (n > NAV_SCHEMATIC_MAX_POINTS)
    {
        n = NAV_SCHEMATIC_MAX_POINTS;
    }
    if (n < 2)
    {
        return false;
    }

    const nav_point_t *p = nav->schematic_points;
    float cum[NAV_SCHEMATIC_MAX_POINTS]; // road distance at each input point
    cum[0] = 0.0f;
    float rider_s = 0.0f;
    float best_d2 = INFINITY;
    for (uint8_t i = 0; i + 1 < n; i++)
    {
        float dx = p[i + 1].x - p[i].x;
        float dy = p[i + 1].y - p[i].y;
        float len2 = dx * dx + dy * dy;
        cum[i + 1] = cum[i] + sqrtf(len2);

        // Closest point to the origin on this segment.
        float t = len2 > 0.0f ? -(p[i].x * dx + p[i].y * dy) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float cx = p[i].x + dx * t;
        float cy = p[i].y + dy * t;
        float d2 = cx * cx + cy * cy;
        if (d2 < best_d2)
        {
            best_d2 = d2;
            rider_s = cum[i] + (cum[i + 1] - cum[i]) * t;
        }
    }

    for (uint8_t k = 0; k < ROUTE_SAMPLES; k++)
    {
        float s = rider_s + (k - ROUTE_BEHIND_STEPS) * ROUTE_SAMPLE_STEP;
        out[k] = road_point_at(p, cum, n, s);
    }
    return true;
}

// Distance from p to the line through a and b, in px.
static float line_distance(lv_point_t p, lv_point_t a, lv_point_t b)
{
    float dx = (float)(b.x - a.x);
    float dy = (float)(b.y - a.y);
    float len = hypotf(dx, dy);
    if (len == 0.0f)
    {
        return hypotf((float)(p.x - a.x), (float)(p.y - a.y));
    }
    return fabsf(dx * (float)(a.y - p.y) - dy * (float)(a.x - p.x)) / len;
}

// Douglas-Peucker on route_px into route_draw: keeps the endpoints and every
// sample that sits more than ROUTE_SIMPLIFY_PX off the simplified line.
static void simplify_route(void)
{
    uint16_t n = view.route_px_count;
    if (n < 3)
    {
        memcpy(view.route_draw, view.route_px, n * sizeof(view.route_px[0]));
        view.route_draw_count = n;
        return;
    }

    bool keep[ROUTE_SAMPLES] = {false};
    uint16_t stack_from[ROUTE_SAMPLES];
    uint16_t stack_to[ROUTE_SAMPLES];
    uint16_t sp = 0;
    keep[0] = keep[n - 1] = true;
    stack_from[sp] = 0;
    stack_to[sp] = n - 1;
    sp++;
    while (sp > 0)
    {
        sp--;
        uint16_t from = stack_from[sp];
        uint16_t to = stack_to[sp];
        float worst = 0.0f;
        uint16_t worst_i = 0;
        for (uint16_t i = from + 1; i < to; i++)
        {
            float d = line_distance(view.route_px[i], view.route_px[from], view.route_px[to]);
            if (d > worst)
            {
                worst = d;
                worst_i = i;
            }
        }
        if (worst > ROUTE_SIMPLIFY_PX)
        {
            keep[worst_i] = true;
            stack_from[sp] = from;
            stack_to[sp] = worst_i;
            sp++;
            stack_from[sp] = worst_i;
            stack_to[sp] = to;
            sp++;
        }
    }

    uint16_t m = 0;
    for (uint16_t i = 0; i < n; i++)
    {
        if (keep[i])
        {
            view.route_draw[m++] = view.route_px[i];
        }
    }
    view.route_draw_count = m;
}

// Schematic units -> screen px. Returns true if any drawn pixel position
// changed, so a sub-pixel glide does not trigger a redraw.
static bool rebuild_route_px(void)
{
    uint16_t n = view.route_valid ? ROUTE_SAMPLES : 0;
    bool changed = n != view.route_px_count;
    view.route_px_count = n;
    for (uint16_t i = 0; i < n; i++)
    {
        lv_point_t px = units_to_px(view.route_shown[i]);
        changed |= px.x != view.route_px[i].x || px.y != view.route_px[i].y;
        view.route_px[i] = px;
    }
    if (changed)
    {
        simplify_route();
    }
    return changed;
}

// Pairs each side road in the new data with the one shown last time (start
// nearby, pointing the same way) so it glides instead of jumping. Unmatched
// new roads fade in; shown roads that disappeared fade out.
static void update_side_targets(const nav_data_t *nav)
{
    bool claimed[SIDE_SLOTS] = {false};
    uint8_t count = nav->side_road_count;
    if (count > NAV_SIDE_ROADS_MAX)
    {
        count = NAV_SIDE_ROADS_MAX;
    }

    for (uint8_t i = 0; i < count; i++)
    {
        fpoint_t from = {nav->side_roads[i].from.x, nav->side_roads[i].from.y};
        fpoint_t to = {nav->side_roads[i].to.x, nav->side_roads[i].to.y};
        float dir_x = to.x - from.x;
        float dir_y = to.y - from.y;
        float dir_len = hypotf(dir_x, dir_y);
        // Points far from the rider move further per update while turning.
        float tolerance = 3.0f + 0.15f * hypotf(from.x, from.y);

        int best = -1;
        float best_dist = tolerance;
        for (uint8_t j = 0; j < SIDE_SLOTS; j++)
        {
            side_view_t *side = &view.sides[j];
            if (claimed[j] || (!side->alive && side->opa <= 0.0f))
            {
                continue;
            }
            float dist = hypotf(side->from_target.x - from.x, side->from_target.y - from.y);
            float sx = side->to_target.x - side->from_target.x;
            float sy = side->to_target.y - side->from_target.y;
            float cosine = (sx * dir_x + sy * dir_y) / (hypotf(sx, sy) * dir_len + 1e-6f);
            if (dist < best_dist && cosine > 0.8f)
            {
                best = j;
                best_dist = dist;
            }
        }

        if (best < 0)
        {
            for (uint8_t j = 0; j < SIDE_SLOTS; j++)
            {
                if (!claimed[j] && !view.sides[j].alive && view.sides[j].opa <= 0.0f)
                {
                    best = j;
                    view.sides[j].from = from;
                    view.sides[j].to = to;
                    view.sides[j].opa = 0.0f;
                    break;
                }
            }
            if (best < 0)
            {
                continue; // no free slot; it will show up on a later update
            }
        }

        claimed[best] = true;
        view.sides[best].from_target = from;
        view.sides[best].to_target = to;
        view.sides[best].alive = true;
    }

    for (uint8_t j = 0; j < SIDE_SLOTS; j++)
    {
        if (!claimed[j])
        {
            view.sides[j].alive = false;
        }
    }
}

// ---- Animation -------------------------------------------------------------
// Moves `shown` toward `target` by fraction k; returns true if it moved.
static bool ease_toward(float *shown, float target, float k)
{
    float diff = target - *shown;
    if (fabsf(diff) < 0.01f)
    {
        bool moved = *shown != target;
        *shown = target;
        return moved;
    }
    *shown += diff * k;
    return true;
}

static void nav_anim_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();
    float dt = lv_tick_diff(now, view.last_tick) / 1000.0f;
    view.last_tick = now;
    if (dt > 0.1f)
    {
        dt = 0.1f; // after a stall, catch up gently instead of jumping
    }
    float k = 1.0f - expf(-EASE_RATE * dt);

    bool redraw_roads = false;
    if (view.route_valid)
    {
        bool moved = false;
        for (uint8_t i = 0; i < ROUTE_SAMPLES; i++)
        {
            moved |= ease_toward(&view.route_shown[i].x, view.route_target[i].x, k);
            moved |= ease_toward(&view.route_shown[i].y, view.route_target[i].y, k);
        }
        if (moved)
        {
            redraw_roads |= rebuild_route_px();
        }
    }

    for (uint8_t j = 0; j < SIDE_SLOTS; j++)
    {
        side_view_t *side = &view.sides[j];
        if (!side->alive && side->opa <= 0.0f)
        {
            continue;
        }
        float old_opa = side->opa;
        side->opa += (side->alive ? dt : -dt) * SIDE_FADE_RATE;
        side->opa = side->opa < 0.0f ? 0.0f : (side->opa > 1.0f ? 1.0f : side->opa);
        ease_toward(&side->from.x, side->from_target.x, k);
        ease_toward(&side->from.y, side->from_target.y, k);
        ease_toward(&side->to.x, side->to_target.x, k);
        ease_toward(&side->to.y, side->to_target.y, k);

        lv_point_t from_px = units_to_px(side->from);
        lv_point_t to_px = units_to_px(side->to);
        if (side->opa != old_opa || from_px.x != side->from_px.x || from_px.y != side->from_px.y ||
            to_px.x != side->to_px.x || to_px.y != side->to_px.y)
        {
            side->from_px = from_px;
            side->to_px = to_px;
            redraw_roads = true;
        }
    }

    if (redraw_roads)
    {
        invalidate_rect(0, 0, 239, ROUTE_AREA_Y2);
    }

    ease_toward(&view.progress_shown, view.progress_target, k);
    int32_t deg = (int32_t)lroundf(view.progress_shown * ARC_SPAN_DEG);
    if (deg != view.progress_deg)
    {
        view.progress_deg = deg;
        invalidate_rect(ARC_AREA_X1, ARC_AREA_Y1, ARC_AREA_X2, ARC_AREA_Y2);
    }
}

// ---- Public API ------------------------------------------------------------
void ui_nav_display_screen_init(void)
{
    memset(&view, 0, sizeof(view));
    view.maneuver = NAV_MANEUVER_STRAIGHT;
    view.last_tick = lv_tick_get();
    distance_text[0] = '\0';
    unit_text[0] = '\0';

    ui_nav_display = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_nav_display, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_nav_display, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_nav_display, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_nav_display, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_nav_display, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(ui_nav_display, nav_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    // Labels are children, so they render on top of everything the draw
    // callback paints.
    label_distance = make_label(&lv_font_montserrat_36, lv_color_white());
    lv_label_set_text(label_distance, "");
    lv_obj_set_pos(label_distance, DIST_LABEL_X, DIST_LABEL_Y);

    label_unit = make_label(&ui_font_Subtitle, lv_color_white());
    lv_label_set_text(label_unit, "");
    lv_obj_set_pos(label_unit, UNIT_LABEL_X, UNIT_LABEL_Y);

    anim_timer = lv_timer_create(nav_anim_cb, ANIM_PERIOD_MS, NULL);
}

void ui_nav_display_set(const nav_data_t *nav)
{
    if (nav == NULL || ui_nav_display == NULL)
    {
        return;
    }

    // The animation timer glides the shown route toward this target. Only
    // the very first route (nothing shown yet) appears directly.
    if (resample_route(nav, view.route_target))
    {
        if (!view.route_valid)
        {
            memcpy(view.route_shown, view.route_target, sizeof(view.route_shown));
            view.route_valid = true;
            rebuild_route_px();
            invalidate_rect(0, 0, 239, ROUTE_AREA_Y2);
        }
    }
    else if (view.route_valid)
    {
        view.route_valid = false;
        rebuild_route_px();
        invalidate_rect(0, 0, 239, ROUTE_AREA_Y2);
    }

    update_side_targets(nav);

    if (nav->maneuver != view.maneuver)
    {
        view.maneuver = nav->maneuver;
        lv_obj_invalidate(ui_nav_display);
    }

    float progress = 1.0f - nav->distance_to_turn_m / PROGRESS_RANGE_M;
    view.progress_target = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);

    char num[12];
    char unit[3];
    format_distance(nav->distance_to_turn_m, num, sizeof(num), unit, sizeof(unit));
    if (strcmp(num, distance_text) != 0)
    {
        strcpy(distance_text, num);
        lv_label_set_text(label_distance, num);
    }
    if (strcmp(unit, unit_text) != 0)
    {
        strcpy(unit_text, unit);
        lv_label_set_text(label_unit, unit);
    }
}

void ui_nav_display_screen_destroy(void)
{
    if (anim_timer != NULL)
    {
        lv_timer_delete(anim_timer);
        anim_timer = NULL;
    }
    if (ui_nav_display != NULL)
    {
        lv_obj_del(ui_nav_display);
        ui_nav_display = NULL;
        label_distance = NULL;
        label_unit = NULL;
    }
}
