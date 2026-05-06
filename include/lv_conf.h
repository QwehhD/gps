#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_HOR_RES_MAX 240
#define LV_VER_RES_MAX 240

// Memory
#define LV_MEM_SIZE (64 * 1024)
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC malloc
#define LV_MEM_CUSTOM_FREE free

// Features
#define LV_USE_OBJ_DRAW_PART 1
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_ANIM 1
#define LV_USE_ANIMIMG 1
#define LV_USE_GROUP 1

// Fonts
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// Indev
#define LV_USE_INDEV_NONE 0
#define LV_INDEV_DEF_READ_PERIOD 30

// Tick
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE <Arduino.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

// Disable logging
#define LV_USE_LOG 0

#endif
