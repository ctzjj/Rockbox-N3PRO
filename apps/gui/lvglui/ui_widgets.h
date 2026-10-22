/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Reusable LVGL components for the n3pro UI.  Every screen must build from
 * these primitives so the house style can never drift between pages.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include "lvgl.h"

/* ---- surfaces ---------------------------------------------------------- */

/* Frosted-glass card (house surface): glass_bg @ card_opa, 1px border_white
 * hairline, zero padding (absolute children are never shifted). */
lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                  lv_coord_t w, lv_coord_t h, lv_coord_t radius);

/* Filled circle (icon tile / artwork disc / speaker cone ...) */
lv_obj_t *ui_disc(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                  lv_coord_t d, lv_color_t color);

/* Pill-shaped container holding a row of round buttons (transport bar).
 * Not clickable/scrollable itself. */
lv_obj_t *ui_btn_group(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h);

/* ---- bars -------------------------------------------------------------- */
#define UI_BAR_SEEK   1   /* warm th->progress indicator, draggable slider */
#define UI_BAR_BUFFER 2   /* blue th->accent_blue indicator, plain bar     */
#define UI_BAR_DB     3   /* amber palette indicator on a faint track      */

/* Draggable seek slider (invisible knob, enlarged touch area). Attach the
 * caller's event callbacks afterwards. */
lv_obj_t *ui_slider_bar(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, int kind);

/* Read-only progress bar. */
lv_obj_t *ui_progress_bar(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h, int kind);

/* ---- buttons ----------------------------------------------------------- */

/* Round button: accent = play_btn_bg/fg (highlighted), else
 * btn_secondary/btn_secondary_opa with fg_text. sym selects the small
 * symbol font, otherwise the 28px font_mid. */
lv_obj_t *ui_round_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t d, const char *icon, bool sym, bool accent,
                       lv_event_cb_t cb, void *ud);

/* ---- volume capsule (shared by the player and the stream screens) ------ */

/* Builds the vertical volume capsule (glass, 44x216 house metrics) with a
 * slider bound to SOUND_VOLUME. *val_out receives the percentage label
 * (may be NULL). slider_cb is attached to LV_EVENT_VALUE_CHANGED. The
 * caller owns positioning/animation of the returned capsule. */
lv_obj_t *ui_volume_capsule(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                            lv_event_cb_t slider_cb, void *ud,
                            lv_obj_t **val_out);

/* Refresh the percentage label text. */
void ui_volume_label(lv_obj_t *val_label, int vol);

/* Bottom "select" sheet: slides up, tap a row to choose (returns index),
 * tap the scrim / cancel to abort (returns -1). Blocking. */
int ui_select_sheet(const char *title, const char **items, int count,
                    int sel);

#endif /* UI_WIDGETS_H */
