/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__  _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * LVGL input port for the Cayin N3Pro: a single input pipeline in which
 * the GUI thread drains the rockbox button queue (button_get_w_tmo) and
 * feeds LVGL here. Touch coordinates ride the button data word packed as
 * (x << 16) | y (HAVE_BUTTON_DATA). Physical keys are mapped per-screen
 * from M1 onwards.
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

#include "lvgl.h"
#include "button.h"
#include "lvgl_ui.h"
#include "settings.h"
#include "misc.h"

static lv_indev_drv_t g_indev_pointer;
static lv_indev_data_t g_last_touch;

/* global edge-swipe-back: start within 48 px of a side edge and drag
 * 45 px towards the centre (horizontal dominant) to pop the stack */
#define EDGE_PX   48
#define SWIPE_PX  45

static lv_coord_t edge_start_x;
static lv_coord_t edge_start_y;
static bool edge_armed;
static bool top_armed;              /* top-edge pull-down (control centre) */
static bool close_armed;            /* this gesture began with the panel open */
static lv_indev_state_t edge_prev = LV_INDEV_STATE_RELEASED;

extern int lvgl_shell_locked;
extern void shell_lock_hide(void);

void lvgui_input_feed(int button, intptr_t data)
{
    extern volatile bool lvgui_plugin_active;
    /* Pointer-mode touch events from button-devinput.c: the queue carries
     * BUTTON_TOUCHSCREEN (press/repeat) and BUTTON_REL|BUTTON_TOUCHSCREEN,
     * with coordinates packed in the data word as (x << 16) | y. */
    if (button & BUTTON_TOUCHSCREEN)
    {
        g_last_touch.point.x = (lv_coord_t)((data >> 16) & 0xffff);
        g_last_touch.point.y = (lv_coord_t)(data & 0xffff);
        g_last_touch.state = (button & BUTTON_REL)
                             ? LV_INDEV_STATE_RELEASED
                             : LV_INDEV_STATE_PRESSED;

        /* the lock screen owns every touch: no edge-back, no pull-down,
         * and only an upward swipe (>= 80 px) unlocks */
        if (lvgl_shell_locked)
        {
            if (g_last_touch.state == LV_INDEV_STATE_PRESSED &&
                edge_prev == LV_INDEV_STATE_RELEASED)
                edge_start_y = g_last_touch.point.y;
            else if (g_last_touch.state == LV_INDEV_STATE_RELEASED &&
                     edge_prev == LV_INDEV_STATE_PRESSED &&
                     edge_start_y - g_last_touch.point.y >= 80)
                shell_lock_hide();
            edge_prev = g_last_touch.state;
            return;
        }

        /* arm only on the press *edge* (move repeats must not re-arm) */
        /* a plugin owns the screen: feed it the classic LEFT/RIGHT keys
         * (PictureFlow and friends only map buttons, not touch) */
        if (lvgui_plugin_active)
        {
            static lv_coord_t px, py;

            if (g_last_touch.state == LV_INDEV_STATE_PRESSED &&
                edge_prev == LV_INDEV_STATE_RELEASED)
            {
                px = g_last_touch.point.x;
                py = g_last_touch.point.y;
            }
            else if (edge_prev == LV_INDEV_STATE_PRESSED &&
                     g_last_touch.state == LV_INDEV_STATE_RELEASED)
            {
                lv_coord_t dx = g_last_touch.point.x - px;
                lv_coord_t dy = g_last_touch.point.y - py;
                lv_coord_t adx = dx < 0 ? -dx : dx;
                lv_coord_t ady = dy < 0 ? -dy : dy;
                if (adx > 40 && adx > ady)
                {
                    long btn = dx < 0 ? BUTTON_RIGHT : BUTTON_LEFT;
                    button_queue_post(btn, 0);
                    button_queue_post(btn | BUTTON_REL, 0);
                }
            }
            edge_prev = g_last_touch.state;
            return;
        }

        if (g_last_touch.state == LV_INDEV_STATE_PRESSED &&
            edge_prev == LV_INDEV_STATE_RELEASED)
        {
            be_keyclick();
            edge_armed = g_last_touch.point.x < EDGE_PX ||
                         g_last_touch.point.x > 360 - EDGE_PX;
            top_armed = g_last_touch.point.y < 40;
            /* only a gesture that BEGAN with the panel open may close it */
            close_armed = lvgui_control_is_open();
            edge_start_x = g_last_touch.point.x;
            edge_start_y = g_last_touch.point.y;
            /* open immediately on a touch in the top strip: the release then
             * lands on the panel (never on the list below, never closing) */
            if (top_armed && !close_armed)
            {
                lvgui_control_open();
                top_armed = false;
            }
        }
        else if (g_last_touch.state == LV_INDEV_STATE_PRESSED &&
                 edge_prev == LV_INDEV_STATE_PRESSED &&
                 top_armed && !lvgui_control_is_open())
        {
            int dy = g_last_touch.point.y - edge_start_y;
            if (dy > SWIPE_PX)
            {
                lvgui_control_open();
                top_armed = false;
                edge_armed = false;
            }
        }
        else if (g_last_touch.state == LV_INDEV_STATE_RELEASED)
        {
            int dx = g_last_touch.point.x - edge_start_x;
            int dy = g_last_touch.point.y - edge_start_y;
            bool swiped = dx > SWIPE_PX || dx < -SWIPE_PX ||
                          dy > SWIPE_PX || dy < -SWIPE_PX;

            if (lvgui_control_is_open())
            {
                if (close_armed && swiped)
                    lvgui_control_close();
            }
            else if (top_armed && dy > SWIPE_PX)
            {
                lvgui_control_open();
            }
            else if (((edge_start_x < EDGE_PX && dx > SWIPE_PX) ||
                      (edge_start_x > 360 - EDGE_PX && dx < -SWIPE_PX)) &&
                     (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy))
            {
                /* inside a menu job the swipe means "one menu level up":
                 * popping the screen instead would flash Home while the
                 * loop pushes the parent level again */
                bool lvgui_menu_cancel(void);
                if (!lvgui_menu_cancel())
                    shell_nav_pop();   /* deferred internally */
            }
            edge_armed = false;
            top_armed = false;
            close_armed = false;
        }
        edge_prev = g_last_touch.state;
    }
}

static void pointer_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    *data = g_last_touch;   /* keep the last point while released */
    (void)drv;
}

void lvgui_port_input_init(void)
{
    lv_indev_drv_init(&g_indev_pointer);
    g_indev_pointer.type = LV_INDEV_TYPE_POINTER;
    g_indev_pointer.read_cb = pointer_read_cb;
    lv_indev_drv_register(&g_indev_pointer);
}
