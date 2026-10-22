/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Reusable LVGL components for the n3pro UI -- see ui_widgets.h.
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
#include "config.h"

#ifdef HAVE_LVGL_UI

#include <stdio.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "lvgl_ui.h"          /* shell_style_shadow_off */
#include "sound.h"
#include "lang.h"            /* sound_min/sound_max */
#include "settings.h"         /* global_status */

lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                  lv_coord_t w, lv_coord_t h, lv_coord_t radius)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, th->glass_bg, 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_darken(th->glass_bg, 16), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, th->card_opa, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, th->border_white, 0);
    lv_obj_set_style_border_opa(c, th->card_border_opa, 0);
    lv_obj_set_style_radius(c, radius, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_text_font(c, th->font_cjk, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    shell_style_shadow_off(c);
    return c;
}

lv_obj_t *ui_disc(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                  lv_coord_t d, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, d / 2, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    shell_style_shadow_off(o);
    return o;
}

lv_obj_t *ui_btn_group(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *g = shell_glass_new(parent);
    lv_obj_set_size(g, w, h);
    lv_obj_set_pos(g, x, y);
    lv_obj_set_style_radius(g, h / 2, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

static void bar_style(lv_obj_t *bar, lv_coord_t h, int kind)
{
    lv_obj_set_style_bg_opa(bar,
        kind == UI_BAR_DB ? LV_OPA_20 : th->bar_track_opa, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, th->bar_track, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar,
        kind == UI_BAR_SEEK ? th->progress :
        kind == UI_BAR_DB   ? shell_icon_grad(0) : th->accent_blue,
        LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
}

lv_obj_t *ui_slider_bar(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, int kind)
{
    lv_obj_t *bar = lv_slider_create(parent);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_pos(bar, x, y);
    lv_slider_set_range(bar, 0, 1000);
    lv_slider_set_value(bar, 0, LV_ANIM_OFF);
    bar_style(bar, h, kind);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_KNOB);
    lv_obj_set_ext_click_area(bar, 18);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return bar;
}

lv_obj_t *ui_progress_bar(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h, int kind)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_pos(bar, x, y);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    bar_style(bar, h, kind);
    return bar;
}

lv_obj_t *ui_round_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t d, const char *icon, bool sym, bool accent,
                       lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, d / 2, 0);
    lv_obj_set_style_bg_color(b,
        accent ? th->play_btn_bg : th->btn_secondary, 0);
    lv_obj_set_style_bg_opa(b,
        accent ? LV_OPA_COVER : th->btn_secondary_opa, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, icon ? icon : "");
    lv_obj_set_style_text_font(l, sym ? th->font_symbol : th->font_mid, 0);
    lv_obj_set_style_text_color(l,
        accent ? th->play_btn_fg : th->fg_text, 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_volume_capsule(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                            lv_event_cb_t slider_cb, void *ud,
                            lv_obj_t **val_out)
{
    const lv_coord_t w = 44, h = 216;
    lv_obj_t *cap = lv_obj_create(parent);
    lv_obj_set_size(cap, w, h);
    lv_obj_set_pos(cap, x, y);
    lv_obj_set_style_bg_opa(cap, th->card_opa, 0);
    lv_obj_set_style_bg_color(cap, th->glass_bg, 0);
    lv_obj_set_style_radius(cap, th->radius_big, 0);
    lv_obj_set_style_border_width(cap, 1, 0);
    lv_obj_set_style_border_color(cap, th->border_white, 0);
    lv_obj_set_style_border_opa(cap, th->card_border_opa, 0);
    lv_obj_set_style_pad_all(cap, 0, 0);
    shell_style_shadow_off(cap);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sl = lv_slider_create(cap);
    lv_obj_set_size(sl, 26, h - 60);           /* h > w -> vertical */
    lv_obj_set_pos(sl, (w - 26) / 2, 14);
    lv_obj_clear_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_slider_set_range(sl, sound_min(SOUND_VOLUME), sound_max(SOUND_VOLUME));
    lv_slider_set_value(sl, global_status.volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(sl, th->bar_track_opa, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, th->bar_track, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 13, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, th->accent_blue, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 13, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sl, th->btn_secondary, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 2, LV_PART_KNOB);
    if (slider_cb)
        lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, ud);

    lv_obj_t *vv = lv_label_create(cap);
    lv_obj_set_style_text_font(vv, th->font_symbol, 0);
    lv_obj_set_style_text_color(vv, th->fg_text, 0);
    lv_obj_align(vv, LV_ALIGN_BOTTOM_MID, 0, -10);
    {
        int lo = sound_min(SOUND_VOLUME), hi = sound_max(SOUND_VOLUME);
        int v = global_status.volume;
        ui_volume_label(vv, hi > lo ? (v - lo) * 100 / (hi - lo) : 0);
    }
    if (val_out)
        *val_out = vv;
    return cap;
}

void ui_volume_label(lv_obj_t *val_label, int vol)
{
    char b[16];
    if (!val_label)
        return;
    snprintf(b, sizeof b, "%d%%", vol);
    lv_label_set_text(val_label, b);
}


/* ---- bottom "select" sheet (iOS style, slides up from the bottom) ------ */
static bool g_sheet_done;
static int g_sheet_res;
static uint32_t g_sheet_t0;

static void sheet_row_cb(lv_event_t *e)
{
    g_sheet_res = (int)(intptr_t)lv_event_get_user_data(e);
    g_sheet_done = true;
}

static void sheet_cancel_cb(lv_event_t *e)
{
    (void)e;
    /* the tap that opened the sheet can deliver its release to the fresh
     * scrim: ignore clicks during the first 300 ms */
    if (lv_tick_elaps(g_sheet_t0) < 300)
        return;
    g_sheet_res = -1;
    g_sheet_done = true;
}

int ui_select_sheet(const char *title, const char **items, int count, int sel)
{
    if (count <= 0 || !items)
        return -1;
    g_sheet_done = false;
    g_sheet_res = -1;
    g_sheet_t0 = lv_tick_get();

    lv_obj_t *scr = lv_layer_top();
    lv_obj_t *scrim = lv_obj_create(scr);
    lv_obj_set_size(scrim, 360, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scrim, th->scrim, 0);
    lv_obj_set_style_bg_opa(scrim, th->scrim_opa, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_add_event_cb(scrim, sheet_cancel_cb, LV_EVENT_CLICKED, NULL);

    int h = 54 + count * 52 + 56;
    lv_obj_t *sheet = lv_obj_create(scrim);
    lv_obj_set_size(sheet, 360, h);
    lv_obj_set_pos(sheet, 0, 480);
    lv_obj_set_style_radius(sheet, th->radius_big, 0);
    lv_obj_set_style_bg_color(sheet, th->glass_bg, 0);
    lv_obj_set_style_bg_grad_color(sheet, lv_color_darken(th->glass_bg, 16), 0);
    lv_obj_set_style_bg_grad_dir(sheet, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sheet, th->border_white, 0);
    lv_obj_set_style_border_opa(sheet, th->card_border_opa, 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_pad_all(sheet, 0, 0);
    lv_obj_set_style_shadow_width(sheet, 0, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(sheet);
    lv_label_set_text(t, title ? title : "");
    lv_obj_set_style_text_font(t, th->font_title, 0);
    lv_obj_set_style_text_color(t, th->fg_text, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

    for (int i = 0; i < count; i++)
    {
        bool cur = (i == sel);
        lv_obj_t *row = lv_btn_create(sheet);
        lv_obj_set_size(row, 336, 44);
        lv_obj_set_pos(row, 12, 52 + i * 52);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_color(row, cur ? th->accent_blue : th->glass_bg, 0);
        lv_obj_set_style_bg_opa(row, cur ? LV_OPA_COVER : th->menu_row_opa, 0);
        lv_obj_set_style_border_color(row, th->border_white, 0);
        lv_obj_set_style_border_opa(row, th->card_border_opa, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_left(row, 16, 0);
        lv_obj_add_event_cb(row, sheet_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, items[i] ? items[i] : "");
        lv_obj_set_style_text_font(l, th->font_cjk, 0);
        lv_obj_set_style_text_color(l, cur ? th->fg_inverse : th->fg_text, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        if (cur)
        {
            lv_obj_t *ck = lv_label_create(row);
            lv_label_set_text(ck, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(ck, th->font_symbol, 0);
            lv_obj_set_style_text_color(ck, th->fg_inverse, 0);
            lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -14, 0);
        }
    }

    lv_obj_t *cx = lv_btn_create(sheet);
    lv_obj_set_size(cx, 336, 44);
    lv_obj_set_pos(cx, 12, h - 52);
    lv_obj_set_style_radius(cx, 12, 0);
    lv_obj_set_style_bg_color(cx, th->btn_secondary, 0);
    lv_obj_set_style_bg_opa(cx, th->btn_secondary_opa, 0);
    lv_obj_set_style_border_width(cx, 0, 0);
    lv_obj_set_style_shadow_width(cx, 0, 0);
    lv_obj_add_event_cb(cx, sheet_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cx);
    lv_label_set_text(cl, str(LANG_CANCEL));
    lv_obj_set_style_text_font(cl, th->font_cjk, 0);
    lv_obj_set_style_text_color(cl, th->fg_text, 0);
    lv_obj_center(cl);

    lv_obj_set_pos(sheet, 0, 480 - h);   /* final position (no anim: the
                                          * anim timer is unreliable here) */
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);

    while (!g_sheet_done)
        lvgui_pump();

    lv_obj_del(scrim);
    lvgui_pump();
    return g_sheet_res;
}

#endif /* HAVE_LVGL_UI */

