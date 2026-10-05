// Navigation Display Screen
//
// Round 240x240 turn-by-turn view: route line with the side roads around it
// and the rider arrow, a domed bottom panel with the upcoming maneuver icon
// + distance, and a progress arc along the bottom edge. Reads only from
// nav_data_t (src/nav_sim.h), so it works the same for dummy and real data.
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

    // Pushes new nav data into the view. It only stores targets (the screen's
    // own timer animates toward them and redraws what actually moved), so
    // calling it every loop() iteration is fine.
    extern void ui_nav_display_set(const nav_data_t *nav);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
