/* ui_stock.h -- the stock-UI bridge (ui_stock.c): implements the classic
 * do_menu()/option_screen()/graphical-EQ surfaces on LVGL.
 *
 * Backend-side header: ui_stock.c is where the two worlds meet (like the
 * lvgl_port_* files), so it may include Rockbox core headers.  This file
 * is NOT included by the screens (see tools/n3pro-lvgl-boundary.sh). */

#ifndef UI_STOCK_H
#define UI_STOCK_H
#ifdef HAVE_LVGL_UI

#include "settings_list.h"

/* EQ editor data: stock int_setting views over
 * global_settings.eq_band_settings (defined in ui_backend.c) */
const struct int_setting *be_eq_gain_setting(void);
void be_eq_apply(void);
void be_eq_apply_band(int band);
const char *be_eq_band_name(int band, char *buf, size_t len);
const struct int_setting *be_eq_q_setting(void);
const struct int_setting *be_eq_cutoff_setting(void);

#endif /* HAVE_LVGL_UI */
#endif /* UI_STOCK_H */
