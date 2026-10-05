// Navigation Display Screen
//
// Round 240x240 turn-by-turn view: schematic road line with the rider arrow,
// a bottom panel with the upcoming maneuver icon + distance, a speed-limit
// sign, and a progress arc along the bottom edge. Reads only from nav_data_t
// (src/nav_sim.h), so it works the same for dummy and real data.
#ifndef UI_NAV_DISPLAY_H
#define UI_NAV_DISPLAY_H

#include <lvgl.h>
#include "../src/nav_sim.h"

#ifdef __cplusplus
extern "C"
{
#endif

    extern lv_obj_t *ui_nav_display;

    extern void ui_nav_display_screen_init(void);
    extern void ui_nav_display_screen_destroy(void);

    // Pushes new nav data into the view. Cheap when nothing visible changed
    // (it only invalidates the screen if the road, icon, distance text,
    // progress arc or speed limit actually differ), so calling it every
    // couple hundred ms is fine.
    extern void ui_nav_display_set(const nav_data_t *nav);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
