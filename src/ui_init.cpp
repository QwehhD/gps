#include <lvgl.h>
#include <stdio.h>
#include "../screens/ui_gps_tracker.h"

extern "C"
{

    void ui_init(void)
    {
        // Initialize GPS tracker screen
        ui_gps_tracker_screen_init();
        lv_scr_load(ui_gps_tracker);
    }

    void ui_destroy(void)
    {
        ui_gps_tracker_screen_destroy();
    }

    // Cheap: call every loop() iteration for a smooth heading-needle sweep.
    void ui_update_nav_heading(float bearing_deg)
    {
        ui_gps_tracker_set_heading(bearing_deg);
    }

    // Heavier: call every few hundred ms to refresh speed/distance/maneuver
    // text and (only on maneuver change) the schematic road preview.
    void ui_update_nav_info(const nav_data_t *nav)
    {
        ui_gps_tracker_set_nav_info(nav);
    }

    void ui_update_gps(double lat, double lon, float speed, float heading, int16_t x, int16_t y, int16_t z, bool connected)
    {
        char buf[128];

        if (ui_label_gps_status)
        {
            lv_label_set_text(ui_label_gps_status, connected ? "✓ Connected" : "⚠ Waiting...");
            lv_obj_set_style_text_color(ui_label_gps_status,
                                        connected ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF9900), LV_PART_MAIN);
        }

        if (ui_label_latitude)
        {
            snprintf(buf, sizeof(buf), "Lat: %.4f", lat);
            lv_label_set_text(ui_label_latitude, buf);
        }

        if (ui_label_longitude)
        {
            snprintf(buf, sizeof(buf), "Lon: %.4f", lon);
            lv_label_set_text(ui_label_longitude, buf);
        }

        if (ui_label_speed)
        {
            snprintf(buf, sizeof(buf), "Spd: %.1f km/h", speed);
            lv_label_set_text(ui_label_speed, buf);
        }

        if (ui_label_heading)
        {
            snprintf(buf, sizeof(buf), "H: %.1f°", heading);
            lv_label_set_text(ui_label_heading, buf);
        }

        if (ui_label_mag_x)
        {
            snprintf(buf, sizeof(buf), "X: %d", x);
            lv_label_set_text(ui_label_mag_x, buf);
        }

        if (ui_label_mag_y)
        {
            snprintf(buf, sizeof(buf), "Y: %d", y);
            lv_label_set_text(ui_label_mag_y, buf);
        }

        if (ui_label_mag_z)
        {
            snprintf(buf, sizeof(buf), "Z: %d", z);
            lv_label_set_text(ui_label_mag_z, buf);
        }
    }
}
