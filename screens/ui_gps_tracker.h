// GPS Tracker Screen
#ifndef UI_GPS_TRACKER_H
#define UI_GPS_TRACKER_H

#include "../src/nav_sim.h"

#ifdef __cplusplus
extern "C"
{
#endif

    extern void ui_gps_tracker_screen_init(void);
    extern void ui_gps_tracker_screen_destroy(void);

    extern lv_obj_t *ui_gps_tracker;
    extern lv_obj_t *ui_label_gps_status;
    extern lv_obj_t *ui_label_latitude;
    extern lv_obj_t *ui_label_longitude;
    extern lv_obj_t *ui_label_speed;
    extern lv_obj_t *ui_label_heading;
    extern lv_obj_t *ui_label_mag_x;
    extern lv_obj_t *ui_label_mag_y;
    extern lv_obj_t *ui_label_mag_z;

    extern lv_obj_t *ui_nav_heading_needle;
    extern lv_obj_t *ui_nav_schematic_line;
    extern lv_obj_t *ui_label_maneuver;
    extern lv_obj_t *ui_label_distance_to_turn;

    // Cheap, call every loop() iteration: only updates the needle rotation
    // so the heading sweep animates smoothly at UI frame rate.
    extern void ui_gps_tracker_set_heading(float bearing_deg);

    // Heavier, call every few hundred ms: refreshes speed/distance/maneuver
    // text and (only when the maneuver changed) the schematic line preview.
    extern void ui_gps_tracker_set_nav_info(const nav_data_t *nav);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
