#include "ui_nav_display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_H1);
LV_FONT_DECLARE(ui_font_Title);
LV_FONT_DECLARE(ui_font_Subtitle);

lv_obj_t *ui_nav_display = NULL;

static lv_obj_t *label_distance = NULL;
static lv_obj_t *label_unit = NULL;
static lv_obj_t *label_limit = NULL;

// ---- Layout (px, 240x240 round panel) --------------------------------------
#define CENTER_X 120
#define CENTER_Y 120

// Road line: heading-up, rider at the arrow. Schematic units -> px.
#define ROUTE_ORIGIN_Y 132
#define ROUTE_PX_PER_UNIT 4
#define ROUTE_WIDTH 8

// Rider arrow (chevron) sits just above the bottom panel.
#define ARROW_TIP_Y 118
#define ARROW_BASE_Y 148
#define ARROW_NOTCH_Y 140
#define ARROW_HALF_W 16

#define PANEL_Y 153

// Maneuver icon lives in a 40x40 box at the left of the panel.
#define ICON_X 62
#define ICON_Y 161
#define ICON_BOX 40
#define ICON_STROKE 6

#define DIST_LABEL_X 109
#define DIST_LABEL_Y 152
#define UNIT_LABEL_X 111
#define UNIT_LABEL_Y 184

// Speed-limit sign overlaps the top-right edge of the panel.
#define SIGN_CX 207
#define SIGN_CY 154
#define SIGN_R 21
#define SIGN_RING 5

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
#define COLOR_RED lv_color_hex(0xE03A2E)

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

// ---- View state (what the draw callback renders) ---------------------------
typedef struct
{
    lv_point_t route[NAV_SCHEMATIC_MAX_POINTS]; // already in screen px
    uint8_t route_count;
    nav_maneuver_t maneuver;
    uint16_t speed_limit;
    int32_t progress_deg; // 0..ARC_SPAN_DEG
} nav_view_t;

static nav_view_t view;
static char distance_text[12];
static char unit_text[3];

// Layer coordinates of the screen's top-left corner (0,0 on this device, but
// kept explicit so the drawing code stays position-independent).
static int32_t off_x, off_y;

// ---- Small drawing helpers -------------------------------------------------
static void draw_line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      int32_t width, lv_color_t color, lv_opa_t opa)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = opa;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = off_x + x1;
    dsc.p1.y = off_y + y1;
    dsc.p2.x = off_x + x2;
    dsc.p2.y = off_y + y2;
    lv_draw_line(layer, &dsc);
}

static void draw_tri(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                     int32_t x3, int32_t y3, lv_color_t color)
{
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
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

static void draw_box(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    lv_area_t area = {off_x + x1, off_y + y1, off_x + x2, off_y + y2};
    lv_draw_rect(layer, &dsc, &area);
}

// ---- Scene pieces, back to front -------------------------------------------
static void draw_route(lv_layer_t *layer)
{
    for (uint8_t i = 1; i < view.route_count; i++)
    {
        draw_line(layer, view.route[i - 1].x, view.route[i - 1].y,
                  view.route[i].x, view.route[i].y,
                  ROUTE_WIDTH, lv_color_white(), LV_OPA_COVER);
    }
}

static void draw_panel(lv_layer_t *layer)
{
    // Drawn after the road so a road that runs behind the rider (U-turn) is
    // cut off cleanly at the panel edge.
    draw_box(layer, 0, PANEL_Y, 239, 239, COLOR_PANEL);
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
    // Faint tail below the chevron, like a motion trail.
    draw_line(layer, CENTER_X, ARROW_NOTCH_Y, CENTER_X, ARROW_BASE_Y + 14, 6, lv_color_white(), LV_OPA_30);

    draw_tri(layer, CENTER_X, ARROW_TIP_Y,
             CENTER_X - ARROW_HALF_W, ARROW_BASE_Y, CENTER_X, ARROW_NOTCH_Y, lv_color_white());
    draw_tri(layer, CENTER_X, ARROW_TIP_Y,
             CENTER_X, ARROW_NOTCH_Y, CENTER_X + ARROW_HALF_W, ARROW_BASE_Y, lv_color_white());
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
        draw_line(layer,
                  ICON_X_AT(shape->path[i - 1].x), ICON_Y_AT(shape->path[i - 1].y),
                  ICON_X_AT(shape->path[i].x), ICON_Y_AT(shape->path[i].y),
                  ICON_STROKE, lv_color_white(), LV_OPA_COVER);
    }
    draw_tri(layer,
             ICON_X_AT(shape->head[0].x), ICON_Y_AT(shape->head[0].y),
             ICON_X_AT(shape->head[1].x), ICON_Y_AT(shape->head[1].y),
             ICON_X_AT(shape->head[2].x), ICON_Y_AT(shape->head[2].y),
             lv_color_white());

#undef ICON_X_AT
#undef ICON_Y_AT
}

static void draw_speed_sign(lv_layer_t *layer)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_white();
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.border_color = COLOR_RED;
    dsc.border_width = SIGN_RING;
    dsc.border_opa = LV_OPA_COVER;
    lv_area_t area = {off_x + SIGN_CX - SIGN_R, off_y + SIGN_CY - SIGN_R,
                      off_x + SIGN_CX + SIGN_R, off_y + SIGN_CY + SIGN_R};
    lv_draw_rect(layer, &dsc, &area);
}

static void nav_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_obj_get_coords(lv_event_get_target_obj(e), &coords);
    off_x = coords.x1;
    off_y = coords.y1;

    draw_route(layer);
    draw_panel(layer);
    draw_progress_arc(layer);
    draw_rider_arrow(layer);
    draw_maneuver_icon(layer);
    if (view.speed_limit > 0)
    {
        draw_speed_sign(layer);
    }
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

// ---- Public API ------------------------------------------------------------
void ui_nav_display_screen_init(void)
{
    memset(&view, 0, sizeof(view));
    view.maneuver = NAV_MANEUVER_STRAIGHT;
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
    label_distance = make_label(&ui_font_H1, lv_color_white());
    lv_label_set_text(label_distance, "");
    lv_obj_set_pos(label_distance, DIST_LABEL_X, DIST_LABEL_Y);

    label_unit = make_label(&ui_font_Title, lv_color_white());
    lv_label_set_text(label_unit, "");
    lv_obj_set_pos(label_unit, UNIT_LABEL_X, UNIT_LABEL_Y);

    label_limit = make_label(&ui_font_Subtitle, lv_color_hex(0x202020));
    lv_label_set_text(label_limit, "");
    lv_obj_set_width(label_limit, SIGN_R * 2);
    lv_obj_set_style_text_align(label_limit, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(label_limit, SIGN_CX - SIGN_R, SIGN_CY - 13);
    lv_obj_add_flag(label_limit, LV_OBJ_FLAG_HIDDEN);
}

void ui_nav_display_set(const nav_data_t *nav)
{
    if (nav == NULL || ui_nav_display == NULL)
    {
        return;
    }

    bool dirty = false;

    // Road line: schematic units (x lateral, y forward) -> screen px.
    lv_point_t route[NAV_SCHEMATIC_MAX_POINTS];
    uint8_t count = nav->schematic_point_count;
    if (count > NAV_SCHEMATIC_MAX_POINTS)
    {
        count = NAV_SCHEMATIC_MAX_POINTS;
    }
    for (uint8_t i = 0; i < count; i++)
    {
        route[i].x = CENTER_X + (int32_t)lroundf(nav->schematic_points[i].x * ROUTE_PX_PER_UNIT);
        route[i].y = ROUTE_ORIGIN_Y - (int32_t)lroundf(nav->schematic_points[i].y * ROUTE_PX_PER_UNIT);
    }
    if (count != view.route_count || memcmp(route, view.route, count * sizeof(route[0])) != 0)
    {
        memcpy(view.route, route, count * sizeof(route[0]));
        view.route_count = count;
        dirty = true;
    }

    if (nav->maneuver != view.maneuver)
    {
        view.maneuver = nav->maneuver;
        dirty = true;
    }

    float progress = 1.0f - nav->distance_to_turn_m / PROGRESS_RANGE_M;
    progress = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    int32_t progress_deg = (int32_t)lroundf(progress * ARC_SPAN_DEG);
    if (progress_deg != view.progress_deg)
    {
        view.progress_deg = progress_deg;
        dirty = true;
    }

    if (nav->speed_limit_kmh != view.speed_limit)
    {
        view.speed_limit = nav->speed_limit_kmh;
        if (view.speed_limit > 0)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", (unsigned)view.speed_limit);
            lv_label_set_text(label_limit, buf);
            lv_obj_remove_flag(label_limit, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(label_limit, LV_OBJ_FLAG_HIDDEN);
        }
        dirty = true;
    }

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

    if (dirty)
    {
        lv_obj_invalidate(ui_nav_display);
    }
}

void ui_nav_display_screen_destroy(void)
{
    if (ui_nav_display != NULL)
    {
        lv_obj_del(ui_nav_display);
        ui_nav_display = NULL;
        label_distance = NULL;
        label_unit = NULL;
        label_limit = NULL;
    }
}
