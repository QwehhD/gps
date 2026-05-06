#include "../ui.h"

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
}

void ui_gps_tracker_screen_destroy(void)
{
    lv_obj_del(ui_gps_tracker);
    ui_gps_tracker = NULL;
}
