/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__  _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * LVGL shell entry for the Cayin N3Pro. Replaces root_menu() as the GUI
 * thread loop: drains the rockbox button queue once per cycle (HZ/20),
 * feeds the input port, then runs the LVGL timer handler. The display
 * flush rides on lcd_update_rect() inside the port layer.
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

#include <stdio.h>
#include "lvgl.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "backlight.h"
#include "powermgmt.h"
#include "events.h"
#include "misc.h"
#include "lvgl_ui.h"

/* The panel loses its content while asleep; LVGL thinks nothing changed.
 * On wake, mark the whole screen dirty so it gets re-flushed. */
static volatile bool g_wake_dirty;
static void lvgui_lcd_activation_hook(unsigned short id, void *event_data)
{
    (void)id;
    (void)event_data;
    /* Runs on the core/backlight thread: only set a flag here. The UI
     * loop performs the actual full redraw (screen AND top layer, where
     * all pages/menus live) once the panel is active again. */
    g_wake_dirty = true;
}

/* ---- one iteration of the UI loop + deferred stock actions -------------- */

static void (*g_job_fn)(void *);
static void *g_job_param;
static bool g_in_job;

/* True while a deferred (classic) handler runs at the top of the loop --
 * blocking stock UI (yesno dialogs) is only swapped to LVGL in that
 * context, never from a rockbox thread. */
bool lvgui_in_job(void)
{
    return g_in_job;
}

/* Queue a classic (potentially blocking) rockbox handler; it runs at the
 * top of lvgui_main, never inside an LVGL event callback. */
void lvgui_run_job(void (*fn)(void *), void *param)
{
    g_job_fn = fn;
    g_job_param = param;
}

/* One iteration of the main loop: buttons in, LVGL out. Also used by
 * shell_kbd_input while it blocks a stock handler. */
void lvgui_idle_pump(void)
{
    lv_timer_handler();
}

static void lvgui_home_key(void)
{
    extern int lvgl_shell_locked;
    /* the lock screen owns the device: only the swipe unlocks */
    if (lvgl_shell_locked)
        return;
    if (lvgui_control_is_open())
        lvgui_control_close();
    shell_nav_home();
}

void lvgui_pump(void)
{
    if (g_wake_dirty)
    {
        g_wake_dirty = false;
        lv_obj_invalidate(lv_scr_act());
        lv_obj_invalidate(lv_layer_top());
        lv_refr_now(NULL);
    }
    /* Idle-friendly polling: 20 Hz right after any input, 10 Hz once the
     * user has been idle for 3 s (halves the wakeups while nothing moves). */
    static long last_input;
    long now_ms = lv_tick_get();
    int tmo = (last_input && now_ms - last_input < 3000) ? HZ / 20 : HZ / 10;
    int button = button_get_w_tmo(tmo);
    if (button)
        last_input = now_ms;
    if (button)
    {
        /* Any user activity wakes the display and postpones idle
         * poweroff; the LCD_EVENT_ACTIVATION hook reflushes the
         * framebuffer content lost while the panel slept. */
        backlight_on();
        reset_poweroff_timer();
        /* The N3Pro's rotary encoder is the volume knob; button-n3pro.c
         * maps it to the scroll buttons (FWD = down, BACK = up). Drive
         * the rockbox volume core so every screen (and the status bar)
         * follows. */
        if (button & BUTTON_SCROLL_FWD)
            adjust_volume(-1);
        else if (button & BUTTON_SCROLL_BACK)
            adjust_volume(1);
        /* the bottom-front touch key under the status LED is the HOME key */
        if (button & BUTTON_HOME)
        {
            if (!(button & (BUTTON_REL | BUTTON_REPEAT)))
                lvgui_home_key();
            button &= ~BUTTON_HOME;
        }
        lvgui_input_feed(button, button_get_data());
    }
    lv_timer_handler();
}

void lvgui_main(void)
{
    lv_init();
    lvgui_port_disp_init();
    lvgui_port_input_init();
    lvgui_shell_start();
    add_event(LCD_EVENT_ACTIVATION, lvgui_lcd_activation_hook);
    {
        extern void lvgui_dbg_init(void);
        lvgui_dbg_init();
    }

    while (1)
    {
        lvgui_pump();
        /* Stock rockbox handlers are deferred to this top level (outside
         * lv_timer_handler): the ones that pop classic UI block their
         * caller, and shell_kbd_input pumps LVGL while it waits -- nesting
         * lv_timer_handler would be unsafe. */
        if (g_job_fn)
        {
            void (*fn)(void *) = g_job_fn;
            void *param = g_job_param;
            g_job_fn = NULL;
            g_in_job = true;
            fn(param);
            g_in_job = false;
            /* drop list-picker screens left on the nav stack by the job */
            shell_pick_cleanup();
            /* classic splash(0,...) only lived for the blocking call's
             * duration; make sure a sticky toast cannot outlive its job */
            shell_toast_clear();
        }
    }
}
