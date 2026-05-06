// GPS Tracker Screen
#ifndef UI_GPS_TRACKER_H
#define UI_GPS_TRACKER_H

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

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
