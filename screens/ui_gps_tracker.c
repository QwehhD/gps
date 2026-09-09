#include "../ui.h"
#include <stdio.h>

// Screen objects
lv_obj_t *ui_gps_tracker = NULL;
lv_obj_t *ui_label_gps_status = NULL;
lv_obj_t *ui_label_latitude = NULL;
lv_obj_t *ui_label_longitude = NULL;
lv_obj_t *ui_label_speed = NULL;
lv_obj_t *ui_label_heading = NULL;
lv_obj_t *ui_label_mag_x = NULL;
lv_obj_t *ui_label_mag_y = NULL;
lv_obj_t *ui_label_mag_z = NULL;

lv_obj_t *ui_nav_heading_needle = NULL;
lv_obj_t *ui_nav_schematic_line = NULL;
lv_obj_t *ui_label_maneuver = NULL;
lv_obj_t *ui_label_distance_to_turn = NULL;

// Fixed size of the heading-needle and schematic-preview boxes, in px.
#define NAV_BOX_W 70
#define NAV_HEADING_BOX_H 70
#define NAV_SCHEMATIC_BOX_H 48

// Needle points (drawn vertical, pointing "up" at 0 deg) and its rotation
// pivot, both in the needle object's own local coordinate space.
static const lv_point_precise_t NAV_NEEDLE_POINTS[2] = {
    {NAV_BOX_W / 2, 8},
    {NAV_BOX_W / 2, NAV_HEADING_BOX_H - 8},
};

// Points buffer for the schematic line; lv_line only stores a pointer, so
// this must stay alive for as long as the line object exists.
static lv_point_precise_t nav_schematic_points_buf[NAV_SCHEMATIC_MAX_POINTS];
static nav_maneuver_t nav_last_maneuver = (nav_maneuver_t)-1;

static const char *nav_maneuver_label(nav_maneuver_t maneuver)
{
    switch (maneuver)
    {
    case NAV_MANEUVER_STRAIGHT:
        return "STRAIGHT";
    case NAV_MANEUVER_TURN_LEFT:
        return "LEFT";
    case NAV_MANEUVER_TURN_RIGHT:
        return "RIGHT";
    case NAV_MANEUVER_U_TURN:
        return "U-TURN";
    default:
        return "?";
    }
}

// Maps the simulator's local road-shape points (x: lateral, y: forward
// distance) into the schematic box's screen-space points, heading-up: the
// rider's current position sits near the bottom, "forward" points up.
static void nav_update_schematic_points(const nav_data_t *nav)
{
    uint8_t count = nav->schematic_point_count;
    if (count > NAV_SCHEMATIC_MAX_POINTS)
    {
        count = NAV_SCHEMATIC_MAX_POINTS;
    }
    for (uint8_t i = 0; i < count; i++)
    {
        nav_schematic_points_buf[i].x = (NAV_BOX_W / 2) + nav->schematic_points[i].x;
        nav_schematic_points_buf[i].y = 40 - nav->schematic_points[i].y;
    }
    lv_line_set_points(ui_nav_schematic_line, nav_schematic_points_buf, count);
}

void ui_gps_tracker_screen_init(void)
{
    ui_gps_tracker = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_gps_tracker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_gps_tracker, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);

    // Title
    lv_obj_t *title = lv_label_create(ui_gps_tracker);
    lv_label_set_text(title, "GPS TRACKER");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &ui_font_Title, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Status
    ui_label_gps_status = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_gps_status, "Waiting...");
    lv_obj_set_style_text_color(ui_label_gps_status, lv_color_hex(0xFF9900), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_gps_status, &ui_font_Number_big, LV_PART_MAIN);
    lv_obj_align(ui_label_gps_status, LV_ALIGN_TOP_MID, 0, 35);

    // === GPS Section ===
    lv_obj_t *gps_label = lv_label_create(ui_gps_tracker);
    lv_label_set_text(gps_label, "[GPS]");
    lv_obj_set_style_text_color(gps_label, lv_color_hex(0xFFFF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(gps_label, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(gps_label, LV_ALIGN_TOP_LEFT, 5, 55);

    // Latitude
    ui_label_latitude = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_latitude, "Lat: 0.0000");
    lv_obj_set_style_text_color(ui_label_latitude, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_latitude, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_latitude, LV_ALIGN_TOP_LEFT, 5, 70);

    // Longitude
    ui_label_longitude = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_longitude, "Lon: 0.0000");
    lv_obj_set_style_text_color(ui_label_longitude, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_longitude, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_longitude, LV_ALIGN_TOP_LEFT, 5, 85);

    // Speed
    ui_label_speed = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_speed, "Spd: 0.0 km/h");
    lv_obj_set_style_text_color(ui_label_speed, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_speed, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_speed, LV_ALIGN_TOP_LEFT, 5, 100);

    // === COMPASS Section ===
    lv_obj_t *compass_label = lv_label_create(ui_gps_tracker);
    lv_label_set_text(compass_label, "[COMPASS]");
    lv_obj_set_style_text_color(compass_label, lv_color_hex(0xFFFF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(compass_label, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(compass_label, LV_ALIGN_TOP_LEFT, 5, 120);

    // Heading
    ui_label_heading = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_heading, "H: 0.0°");
    lv_obj_set_style_text_color(ui_label_heading, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_heading, &ui_font_Number_big, LV_PART_MAIN);
    lv_obj_align(ui_label_heading, LV_ALIGN_TOP_LEFT, 5, 135);

    // Mag X
    ui_label_mag_x = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_mag_x, "X: 0");
    lv_obj_set_style_text_color(ui_label_mag_x, lv_color_hex(0xFF6B6B), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_mag_x, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_mag_x, LV_ALIGN_TOP_LEFT, 5, 155);

    // Mag Y
    ui_label_mag_y = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_mag_y, "Y: 0");
    lv_obj_set_style_text_color(ui_label_mag_y, lv_color_hex(0x6BFF6B), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_mag_y, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_mag_y, LV_ALIGN_TOP_LEFT, 5, 170);

    // Mag Z
    ui_label_mag_z = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_mag_z, "Z: 0");
    lv_obj_set_style_text_color(ui_label_mag_z, lv_color_hex(0x6B6BFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_mag_z, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_mag_z, LV_ALIGN_TOP_LEFT, 5, 185);

    // === NAV PREVIEW (right column): dummy-data visualization ===
    lv_obj_t *heading_caption = lv_label_create(ui_gps_tracker);
    lv_label_set_text(heading_caption, "HEADING");
    lv_obj_set_style_text_color(heading_caption, lv_color_hex(0xFFFF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(heading_caption, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(heading_caption, LV_ALIGN_TOP_RIGHT, -15, 48);

    lv_obj_t *heading_box = lv_obj_create(ui_gps_tracker);
    lv_obj_remove_flag(heading_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(heading_box, NAV_BOX_W, NAV_HEADING_BOX_H);
    lv_obj_align(heading_box, LV_ALIGN_TOP_RIGHT, -15, 64);
    lv_obj_set_style_bg_color(heading_box, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_color(heading_box, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_border_width(heading_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(heading_box, 6, LV_PART_MAIN);

    ui_nav_heading_needle = lv_line_create(heading_box);
    lv_obj_set_size(ui_nav_heading_needle, NAV_BOX_W, NAV_HEADING_BOX_H);
    lv_line_set_points(ui_nav_heading_needle, NAV_NEEDLE_POINTS, 2);
    lv_obj_set_style_line_color(ui_nav_heading_needle, lv_color_hex(0x00FFFF), LV_PART_MAIN);
    lv_obj_set_style_line_width(ui_nav_heading_needle, 3, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(ui_nav_heading_needle, true, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(ui_nav_heading_needle, NAV_BOX_W / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(ui_nav_heading_needle, NAV_HEADING_BOX_H / 2, LV_PART_MAIN);

    ui_label_maneuver = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_maneuver, "STRAIGHT");
    lv_obj_set_style_text_color(ui_label_maneuver, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_maneuver, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_maneuver, LV_ALIGN_TOP_RIGHT, -15, 138);

    ui_label_distance_to_turn = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_distance_to_turn, "Turn: 0 m");
    lv_obj_set_style_text_color(ui_label_distance_to_turn, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_distance_to_turn, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_distance_to_turn, LV_ALIGN_TOP_RIGHT, -15, 154);

    lv_obj_t *road_caption = lv_label_create(ui_gps_tracker);
    lv_label_set_text(road_caption, "ROAD AHEAD");
    lv_obj_set_style_text_color(road_caption, lv_color_hex(0xFFFF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(road_caption, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(road_caption, LV_ALIGN_TOP_RIGHT, -15, 172);

    lv_obj_t *schematic_box = lv_obj_create(ui_gps_tracker);
    lv_obj_remove_flag(schematic_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(schematic_box, NAV_BOX_W, NAV_SCHEMATIC_BOX_H);
    lv_obj_align(schematic_box, LV_ALIGN_TOP_RIGHT, -15, 188);
    lv_obj_set_style_bg_color(schematic_box, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_color(schematic_box, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_border_width(schematic_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(schematic_box, 6, LV_PART_MAIN);

    ui_nav_schematic_line = lv_line_create(schematic_box);
    lv_obj_set_size(ui_nav_schematic_line, NAV_BOX_W, NAV_SCHEMATIC_BOX_H);
    lv_obj_set_style_line_color(ui_nav_schematic_line, lv_color_hex(0xFF9900), LV_PART_MAIN);
    lv_obj_set_style_line_width(ui_nav_schematic_line, 3, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(ui_nav_schematic_line, true, LV_PART_MAIN);

    nav_last_maneuver = (nav_maneuver_t)-1; // force the first update to populate the line
}

void ui_gps_tracker_set_heading(float bearing_deg)
{
    if (ui_nav_heading_needle == NULL)
    {
        return;
    }
    // LVGL rotation style is in 0.1 degree units, clockwise.
    lv_obj_set_style_transform_rotation(ui_nav_heading_needle, (int32_t)(bearing_deg * 10.0f), LV_PART_MAIN);
}

void ui_gps_tracker_set_nav_info(const nav_data_t *nav)
{
    if (nav == NULL || ui_label_maneuver == NULL)
    {
        return;
    }

    char buf[32];

    lv_label_set_text(ui_label_maneuver, nav_maneuver_label(nav->maneuver));

    snprintf(buf, sizeof(buf), "Turn: %.0f m", nav->distance_to_turn_m > 0 ? nav->distance_to_turn_m : 0);
    lv_label_set_text(ui_label_distance_to_turn, buf);

    if (nav->maneuver != nav_last_maneuver)
    {
        nav_last_maneuver = nav->maneuver;
        nav_update_schematic_points(nav);
    }
}

void ui_gps_tracker_screen_destroy(void)
{
    lv_obj_del(ui_gps_tracker);
    ui_gps_tracker = NULL;
}
