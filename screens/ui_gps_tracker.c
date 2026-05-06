#include "../ui.h"

// Screen objects
lv_obj_t * ui_gps_tracker = NULL;
lv_obj_t * ui_label_gps_status = NULL;
lv_obj_t * ui_label_latitude = NULL;
lv_obj_t * ui_label_longitude = NULL;
lv_obj_t * ui_label_speed = NULL;
lv_obj_t * ui_label_heading = NULL;

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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Status
    ui_label_gps_status = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_gps_status, "Waiting...");
    lv_obj_set_style_text_color(ui_label_gps_status, lv_color_hex(0xFF9900), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_gps_status, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_gps_status, LV_ALIGN_TOP_MID, 0, 40);
    
    // Latitude
    ui_label_latitude = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_latitude, "Lat: 0.0000");
    lv_obj_set_style_text_color(ui_label_latitude, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_latitude, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_latitude, LV_ALIGN_TOP_MID, 0, 70);
    
    // Longitude
    ui_label_longitude = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_longitude, "Lon: 0.0000");
    lv_obj_set_style_text_color(ui_label_longitude, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_longitude, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_longitude, LV_ALIGN_TOP_MID, 0, 100);
    
    // Speed
    ui_label_speed = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_speed, "Speed: 0.0 km/h");
    lv_obj_set_style_text_color(ui_label_speed, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_speed, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_speed, LV_ALIGN_CENTER, 0, 0);
    
    // Heading
    ui_label_heading = lv_label_create(ui_gps_tracker);
    lv_label_set_text(ui_label_heading, "Heading: 0.0°");
    lv_obj_set_style_text_color(ui_label_heading, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_label_heading, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_align(ui_label_heading, LV_ALIGN_CENTER, 0, 30);
}

void ui_gps_tracker_screen_destroy(void)
{
    lv_obj_del(ui_gps_tracker);
    ui_gps_tracker = NULL;
}
