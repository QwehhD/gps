#include <lvgl.h>
#include <stdio.h>
#include "../screens/ui_gps_tracker.h"
#include "../screens/ui_nav_display.h"

// 0 = turn-by-turn navigation display (default), 1 = the old text/debug
// screen with lat/lon, compass X/Y/Z, heading needle and schematic preview.
// Only the selected screen is created; the update functions below skip the
// other one because its widget pointers stay NULL.
#define UI_START_DEBUG_SCREEN 0

extern "C"
{

    void ui_init(void)
    {
#if UI_START_DEBUG_SCREEN
        ui_gps_tracker_screen_init();
        lv_scr_load(ui_gps_tracker);
#else
        ui_nav_display_screen_init();
        lv_scr_load(ui_nav_display);
#endif
    }

    void ui_destroy(void)
    {
        if (ui_gps_tracker)
        {
            ui_gps_tracker_screen_destroy();
        }
        ui_nav_display_screen_destroy();
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

    // Cheap: call every loop() iteration. The navigation display eases toward
    // whatever it was given last, so a steadily updated target keeps the road
    // and progress arc moving smoothly instead of in 200 ms steps.
    void ui_update_nav_display(const nav_data_t *nav)
    {
        ui_nav_display_set(nav);
    }

    // Cheap: call every loop() iteration with whether the nav data is
    // current; the navigation display only acts when it changes.
    void ui_update_nav_signal(bool has_signal)
    {
        ui_nav_display_set_signal(has_signal);
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
