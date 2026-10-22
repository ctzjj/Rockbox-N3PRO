#ifndef LV_CONF_H
#define LV_CONF_H
/* Minimal LVGL v8.4 config for the Rockbox N3Pro port. */

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_USE_PNG 1   /* lodepng: PNG album art */

/* Time base: rockbox ticks -> milliseconds. */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "kernel.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((long long)current_tick * 1000 / HZ)

/* Hosted linux app: use the system heap. */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>

#define LV_DISP_DEF_REFR_PERIOD 33
#define LV_INDEV_DEF_READ_PERIOD 20

/* Fonts: built-in latin + generated CJK (GB2312 full, see tools/gen_gb2312.js). */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_FMT_TXT_LARGE 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_INDEV_DEF_GESTURE_LIMIT 30

#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(notosans_sc_16);
#define LV_FONT_DEFAULT &notosans_sc_16

/* Widgets needed at M0 (grow per milestone). */
#define LV_USE_LABEL      1
#define LV_USE_BTN        1
#define LV_USE_FLEX 1

#define LV_USE_KEYBOARD   1
#define LV_USE_IME_PINYIN 1
#define LV_IME_PINYIN_USE_DEFAULT_DICT 1

#define LV_USE_SNAPSHOT 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#endif /* LV_CONF_H */
