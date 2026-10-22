/* Build wrapper: LVGL's pinyin IME is not pulled in by lv_extra.c in this
 * LVGL snapshot, so compile it here (enabled via LV_USE_IME_PINYIN). */
#include "config.h"

#ifdef HAVE_LVGL_UI

#include "lvgl.h"

#include "../../../lib/lvgl/src/extra/others/ime/lv_ime_pinyin.c"

#endif /* HAVE_LVGL_UI */
