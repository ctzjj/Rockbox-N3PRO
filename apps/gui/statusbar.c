/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) Robert E. Hak (2002), Linus Nielsen Feltzing (2002)
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
#include "config.h"
#include "font.h"
#include "kernel.h"
#include "string.h" /* for memcmp oO*/
#include "string-extra.h" /* for itoa */
#include "sound.h"
#include "settings.h"
#include "viewport.h"
#include "metadata.h"
#include "icons.h"
#include "powermgmt.h"
#include "usb.h"
#include "led.h"
#include "screen_access.h"

#include "status.h" /* needed for battery_state global var */
#include "action.h" /* for keys_locked */
#include "statusbar.h"
#include "statusbar_rf.h"
#ifdef HAVE_RECORDING
#include "audio.h"
#include "recording.h"
#include "pcm_record.h"
#endif
#include "appevents.h"
#include "timefuncs.h"

/* FIXME: should be removed from icon.h to avoid redefinition,
   but still needed for compatibility with old system */
#define ICONS_SPACING                           2
#define STATUSBAR_BATTERY_X_POS                 0*ICONS_SPACING
#define STATUSBAR_BATTERY_WIDTH                 (2+(2*SYSFONT_WIDTH))
#define STATUSBAR_PLUG_X_POS                    STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                ICONS_SPACING
#define STATUSBAR_BATTERY_HEIGHT                SB_ICON_HEIGHT - 1
#define STATUSBAR_PLUG_WIDTH                    7
#define STATUSBAR_VOLUME_X_POS                  STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                2*ICONS_SPACING
#define STATUSBAR_VOLUME_WIDTH                  (2+(2*SYSFONT_WIDTH))
#define STATUSBAR_ENCODER_X_POS                 STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                2*ICONS_SPACING - 1
#define STATUSBAR_ENCODER_WIDTH                 18
#define STATUSBAR_PLAY_STATE_X_POS              STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                3*ICONS_SPACING
#define STATUSBAR_PLAY_STATE_WIDTH              7
#define STATUSBAR_PLAY_MODE_X_POS               STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                4*ICONS_SPACING
#define STATUSBAR_PLAY_MODE_WIDTH               7
#define STATUSBAR_RECFREQ_X_POS                 STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                3*ICONS_SPACING
#define STATUSBAR_RECFREQ_WIDTH                 12
#define STATUSBAR_RECCHANNELS_X_POS             STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                STATUSBAR_RECFREQ_WIDTH + \
                                                4*ICONS_SPACING
#define STATUSBAR_RECCHANNELS_WIDTH             5
#define STATUSBAR_SHUFFLE_X_POS                 STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                STATUSBAR_PLAY_MODE_WIDTH + \
                                                5*ICONS_SPACING
#define STATUSBAR_SHUFFLE_WIDTH                 7
#define STATUSBAR_LOCKM_X_POS                   STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                STATUSBAR_PLAY_MODE_WIDTH + \
                                                STATUSBAR_SHUFFLE_WIDTH + \
                                                6*ICONS_SPACING
#define STATUSBAR_LOCKM_WIDTH                   5
#define STATUSBAR_LOCKR_X_POS                   STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                STATUSBAR_PLAY_MODE_WIDTH + \
                                                STATUSBAR_SHUFFLE_WIDTH + \
                                                STATUSBAR_LOCKM_WIDTH + \
                                                7*ICONS_SPACING
#define STATUSBAR_LOCKR_WIDTH                   5

#if (CONFIG_LED == LED_VIRTUAL) || defined(HAVE_REMOTE_LCD)
#define STATUSBAR_DISK_WIDTH                    12
#define STATUSBAR_DISK_X_POS(statusbar_width)   statusbar_width - \
                                                STATUSBAR_DISK_WIDTH
#else
#define STATUSBAR_DISK_WIDTH                    0
#endif
#define STATUSBAR_TIME_X_END(statusbar_width)   statusbar_width - 1 - \
                                                STATUSBAR_DISK_WIDTH
struct gui_syncstatusbar statusbars;

/* Prototypes */
static void gui_statusbar_icon_battery(struct screen * display, int percent,
                                       int batt_charge_step);
static bool gui_statusbar_icon_volume(struct gui_statusbar * bar, int volume);
static void gui_statusbar_icon_play_state(struct screen * display, int state);
static void gui_statusbar_icon_play_mode(struct screen * display, int mode);
static void gui_statusbar_icon_shuffle(struct screen * display);
static void gui_statusbar_icon_lock(struct screen * display);
#ifdef HAS_REMOTE_BUTTON_HOLD
static void gui_statusbar_icon_lock_remote(struct screen * display);
#endif
#if (CONFIG_LED == LED_VIRTUAL) || defined(HAVE_REMOTE_LCD)
static void gui_statusbar_led(struct screen * display);
#endif
#ifdef HAVE_RECORDING
static void gui_statusbar_icon_recording_info(struct screen * display);
#endif
#if CONFIG_RTC
static void gui_statusbar_time(struct screen * display, struct tm *time);
#endif

#ifdef HAVE_STATUSBAR_RF_ICONS
static void gui_statusbar_icon_rf(struct screen *display,
                                  const struct status_info *info);
#endif

/* End prototypes */


/*
 * Initializes a status bar
 *  - bar : the bar to initialize
 */
static void gui_statusbar_init(struct screen * display, struct gui_statusbar * bar)
{
    bar->display = display;
    bar->redraw_volume = true;
    bar->volume_icon_switch_tick = bar->battery_icon_switch_tick = current_tick;
    memset((void*)&(bar->lastinfo), 0, sizeof(struct status_info));
#if CONFIG_RTC
    bar->last_tm_min = 0;
#endif
}

static struct screen * sb_fill_bar_info(struct gui_statusbar * bar)
{
    struct screen *display = bar->display;

    if (!display)
        return display;

    bar->info.battlevel = battery_level();
#ifdef HAVE_USB_POWER
    bar->info.usb_inserted = usb_inserted();
#endif
#if CONFIG_CHARGING
    bar->info.inserted = (charger_input_state == CHARGER);
    if (bar->info.inserted)
    {
        bar->info.battery_state = true;

#if CONFIG_CHARGING >= CHARGING_MONITOR

        /* zero battery run time if charging */
        if (charge_state > DISCHARGING)
            zero_runtime();

        /* animate battery if charging */
        if ((charge_state == DISCHARGING) || (charge_state == TRICKLE))
        {
            bar->info.batt_charge_step = -1;
        }
        else
        {
#else /* CONFIG_CHARGING < CHARGING_MONITOR */
            zero_runtime();
        {
#endif /* CONFIG_CHARGING < CHARGING_MONITOR */
            /* animate in (max.) 4 steps, starting near the current charge level */
            if (TIME_AFTER(current_tick, bar->battery_icon_switch_tick))
            {
                if (++bar->info.batt_charge_step > 3)
                    bar->info.batt_charge_step = bar->info.battlevel / 34;
                bar->battery_icon_switch_tick = current_tick + HZ;
            }
        }
    }
    else
#endif /* CONFIG_CHARGING */
    {
        bar->info.batt_charge_step = -1;
        if (battery_level_safe())
            bar->info.battery_state = true;
        else
            /* blink battery if level is low */
            if (TIME_AFTER(current_tick, bar->battery_icon_switch_tick) &&
               (bar->info.battlevel > -1))
            {
                bar->info.battery_state = !bar->info.battery_state;
                bar->battery_icon_switch_tick = current_tick + HZ;
            }
    }
    bar->info.volume = global_status.volume;
    bar->info.shuffle = global_settings.playlist_shuffle;
#ifdef HAS_BUTTON_HOLD
    bar->info.keylock = button_hold();
#else
    bar->info.keylock = is_keys_locked();
#endif /* HAS_BUTTON_HOLD */
#ifdef HAS_REMOTE_BUTTON_HOLD
    bar->info.keylockremote = remote_button_hold();
#endif
    bar->info.repeat = global_settings.repeat_mode;
    bar->info.playmode = current_playmode();
#if CONFIG_RTC
    bar->time = get_time();
#endif /* CONFIG_RTC */
#if (CONFIG_LED == LED_VIRTUAL) || defined(HAVE_REMOTE_LCD)
    if(!display->has_disk_led)
        bar->info.led = led_read(HZ/2); /* delay should match polling interval */
#endif
#ifdef HAVE_STATUSBAR_RF_ICONS
    bar->info.rf_bt = statusbar_rf_bt_on();
    bar->info.rf_wifi = statusbar_rf_wifi_on();
#endif

    return display;
}

void gui_statusbar_draw(struct gui_statusbar * bar, bool force_redraw, struct viewport *vp)
{
    struct viewport *last_vp = NULL;
    struct screen * display = sb_fill_bar_info(bar);
    if (!display)
        return;

    /* only redraw if forced to, or info has changed */
    if (force_redraw || bar->redraw_volume ||
#if CONFIG_RTC
        (bar->time->tm_min != bar->last_tm_min) ||
#endif
        memcmp(&(bar->info), &(bar->lastinfo), sizeof(struct status_info)))
    {
        last_vp = display->set_viewport(vp);
        display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
        display->fill_viewport();
        display->set_drawmode(DRMODE_SOLID);
        display->setfont(FONT_SYSFIXED);

        if (bar->info.battery_state)
            gui_statusbar_icon_battery(display, bar->info.battlevel,
                                       bar->info.batt_charge_step);
#ifdef HAVE_USB_POWER
        if (bar->info.usb_inserted)
            display->mono_bitmap(bitmap_icons_7x8[Icon_USBPlug],
                                 STATUSBAR_PLUG_X_POS,
                                 STATUSBAR_Y_POS, STATUSBAR_PLUG_WIDTH,
                                 SB_ICON_HEIGHT);
#endif /* HAVE_USB_POWER */
#if CONFIG_CHARGING
#ifdef HAVE_USB_POWER
        else
#endif
        /* draw power plug if charging */
        if (bar->info.inserted)
            display->mono_bitmap(bitmap_icons_7x8[Icon_Plug],
                                    STATUSBAR_PLUG_X_POS,
                                    STATUSBAR_Y_POS, STATUSBAR_PLUG_WIDTH,
                                    SB_ICON_HEIGHT);
#endif /* CONFIG_CHARGING */
#ifdef HAVE_RECORDING
        /* turn off volume display in recording screen */
        bool recscreen_on = in_recording_screen();
        if (!recscreen_on)
#endif
            bar->redraw_volume = gui_statusbar_icon_volume(bar, bar->info.volume);
        gui_statusbar_icon_play_state(display, current_playmode() + Icon_Play);

#ifdef HAVE_RECORDING
        /* If in recording screen, replace repeat mode, volume
           and shuffle icons with recording info */
        if (recscreen_on)
            gui_statusbar_icon_recording_info(display);
        else
#endif
        {
            gui_statusbar_icon_play_mode(display, bar->info.repeat);

            if (bar->info.shuffle)
                gui_statusbar_icon_shuffle(display);
        }
        if (bar->info.keylock)
            gui_statusbar_icon_lock(display);
#ifdef HAS_REMOTE_BUTTON_HOLD
        if (bar->info.keylockremote)
            gui_statusbar_icon_lock_remote(display);
#endif
#if CONFIG_RTC
        gui_statusbar_time(display, bar->time);
        bar->last_tm_min = bar->time->tm_min;
#endif /* CONFIG_RTC */
#if (CONFIG_LED == LED_VIRTUAL) || defined(HAVE_REMOTE_LCD)
        if(!display->has_disk_led && bar->info.led)
        {
            gui_statusbar_led(display);
        }
#endif
#ifdef HAVE_STATUSBAR_RF_ICONS
        gui_statusbar_icon_rf(display, &bar->info);
#endif
        display->setfont(FONT_UI);
        display->set_viewport(last_vp);
        bar->lastinfo = bar->info;
    }
}

/* from icon.c */
/*
 * Print battery icon to status bar
 */
static void gui_statusbar_icon_battery(struct screen * display, int percent,
                                       int batt_charge_step)
{
    int fill, endfill;
    char buffer[5];
    unsigned int width, height;
#if LCD_DEPTH > 1
    unsigned int prevfg = 0;
#endif

#if CONFIG_CHARGING
    if (batt_charge_step >= 0)
    {
        fill = percent * (STATUSBAR_BATTERY_WIDTH-3) / 100;
        endfill = 34 * batt_charge_step * (STATUSBAR_BATTERY_WIDTH-3) / 100;
    }
    else
#else
    (void)batt_charge_step;
#endif
    {
        fill = endfill = (percent * (STATUSBAR_BATTERY_WIDTH-3) + 50) / 100;
    }

#if CONFIG_CHARGING == CHARGING_MONITOR && !defined(SIMULATOR)
    /* Certain charge controlled targets */
    /* show graphical animation when charging instead of numbers */
    if ((global_settings.battery_display) &&
        (charge_state != CHARGING) &&
        (percent > -1) &&
        (percent <= 100)) {
#else /* all others */
    if (global_settings.battery_display && (percent > -1) && (percent <= 100)) {
#endif
        /* Numeric display */
        snprintf(buffer, sizeof(buffer), "%3d", percent);
        font_getstringsize(buffer, &width, &height, FONT_SYSFIXED);
        if (height <= STATUSBAR_HEIGHT) {
             display->putsxy(STATUSBAR_BATTERY_X_POS
                             + STATUSBAR_BATTERY_WIDTH / 2
                             - width/2, STATUSBAR_Y_POS, buffer);
        }

    }
    else {
        /* draw battery */
        display->drawrect(STATUSBAR_BATTERY_X_POS, STATUSBAR_Y_POS,
                          STATUSBAR_BATTERY_WIDTH - 1, STATUSBAR_BATTERY_HEIGHT);
        display->vline(STATUSBAR_BATTERY_X_POS + STATUSBAR_BATTERY_WIDTH - 1,
                       STATUSBAR_Y_POS + 2, STATUSBAR_Y_POS + 4);

        display->fillrect(STATUSBAR_BATTERY_X_POS + 1, STATUSBAR_Y_POS + 1,
                          fill, STATUSBAR_BATTERY_HEIGHT - 2);
#if LCD_DEPTH > 1
        if (display->depth > 1)
        {
            prevfg = display->get_foreground();
            display->set_foreground(LCD_DARKGRAY);
        }
#endif
        display->fillrect(STATUSBAR_BATTERY_X_POS + 1 + fill, STATUSBAR_Y_POS + 1,
                          endfill - fill, STATUSBAR_BATTERY_HEIGHT - 2);
#if LCD_DEPTH > 1
        if (display->depth > 1)
            display->set_foreground(prevfg);
#endif
    }

    if (percent == -1 || percent > 100) {
        display->putsxy(STATUSBAR_BATTERY_X_POS + STATUSBAR_BATTERY_WIDTH / 2
                          - 4, STATUSBAR_Y_POS, "?");
    }
}

/*
 * Print volume gauge to status bar
 */
static bool gui_statusbar_icon_volume(struct gui_statusbar * bar, int volume)
{
    int i;
    int vol;
    char buffer[4];
    unsigned int width, height;
    bool needs_redraw = false;
    int type = global_settings.volume_type;
    struct screen * display=bar->display;
    const int minvol = sound_min(SOUND_VOLUME);

    if (volume < minvol)
        volume = minvol;

    if (volume == minvol) {
        display->mono_bitmap(bitmap_icons_7x8[Icon_Mute],
                    STATUSBAR_VOLUME_X_POS + STATUSBAR_VOLUME_WIDTH / 2 - 4,
                    STATUSBAR_Y_POS, 7, SB_ICON_HEIGHT);
    }
    else {
        const int maxvol = sound_max(SOUND_VOLUME);

        if (volume > maxvol)
            volume = maxvol;
        /* We want to redraw the icon later on */
        if (bar->last_volume != volume && bar->last_volume >= minvol) {
            bar->volume_icon_switch_tick = current_tick + HZ;
        }

        /* If the timeout hasn't yet been reached, we show it numerically
           and tell the caller that we want to be called again */
        if (TIME_BEFORE(current_tick,bar->volume_icon_switch_tick)) {
            type = 1;
            needs_redraw = true;
        }

        /* display volume level numerical? */
        if (type)
        {
            const int num_decimals = sound_numdecimals(SOUND_VOLUME);
            if (num_decimals)
                volume /= 10 * num_decimals;

            snprintf(buffer, sizeof(buffer), "%2d", volume);
            font_getstringsize(buffer, &width, &height, FONT_SYSFIXED);
            if (height <= STATUSBAR_HEIGHT) {
                display->putsxy(STATUSBAR_VOLUME_X_POS
                                  + STATUSBAR_VOLUME_WIDTH / 2
                                  - width/2, STATUSBAR_Y_POS, buffer);
            }
        } else {
            /* display volume bar */
            vol = (volume - minvol) * 14 / (maxvol - minvol);
            for(i=0; i < vol; i++) {
                display->vline(STATUSBAR_VOLUME_X_POS + i,
                               STATUSBAR_Y_POS + 6 - i / 2,
                               STATUSBAR_Y_POS + 6);
            }
        }
    }
    bar->last_volume = volume;

    return needs_redraw;
}

/*
 * Print play state to status bar
 */
static void gui_statusbar_icon_play_state(struct screen * display, int state)
{
    display->mono_bitmap(bitmap_icons_7x8[state], STATUSBAR_PLAY_STATE_X_POS,
                    STATUSBAR_Y_POS, STATUSBAR_PLAY_STATE_WIDTH,
                    SB_ICON_HEIGHT);
}

/*
 * Print play mode to status bar
 */
static void gui_statusbar_icon_play_mode(struct screen * display, int mode)
{
    switch (mode) {
#ifdef AB_REPEAT_ENABLE
        case REPEAT_AB:
            display->mono_bitmap(bitmap_icons_7x8[Icon_RepeatAB],
                                 STATUSBAR_PLAY_MODE_X_POS,
                                 STATUSBAR_Y_POS, STATUSBAR_PLAY_MODE_WIDTH,
                                 SB_ICON_HEIGHT);
            break;
#endif /* AB_REPEAT_ENABLE */

        case REPEAT_ONE:
            display->mono_bitmap(bitmap_icons_7x8[Icon_RepeatOne],
                                 STATUSBAR_PLAY_MODE_X_POS,
                                 STATUSBAR_Y_POS, STATUSBAR_PLAY_MODE_WIDTH,
                                 SB_ICON_HEIGHT);
            break;

        case REPEAT_ALL:
        case REPEAT_SHUFFLE:
            display->mono_bitmap(bitmap_icons_7x8[Icon_Repeat],
                                 STATUSBAR_PLAY_MODE_X_POS,
                                 STATUSBAR_Y_POS, STATUSBAR_PLAY_MODE_WIDTH,
                                 SB_ICON_HEIGHT);
            break;
    }
}
/*
 * Print shuffle mode to status bar
 */
static void gui_statusbar_icon_shuffle(struct screen * display)
{
    display->mono_bitmap(bitmap_icons_7x8[Icon_Shuffle],
                    STATUSBAR_SHUFFLE_X_POS, STATUSBAR_Y_POS,
                    STATUSBAR_SHUFFLE_WIDTH, SB_ICON_HEIGHT);
}

/*
 * Print lock when keys are locked
 */
static void gui_statusbar_icon_lock(struct screen * display)
{
    display->mono_bitmap(bitmap_icons_5x8[Icon_Lock_Main],
                         STATUSBAR_LOCKM_X_POS, STATUSBAR_Y_POS,
                         STATUSBAR_LOCKM_WIDTH, SB_ICON_HEIGHT);
}

#ifdef HAS_REMOTE_BUTTON_HOLD
/*
 * Print remote lock when remote hold is enabled
 */
static void gui_statusbar_icon_lock_remote(struct screen * display)
{
    display->mono_bitmap(bitmap_icons_5x8[Icon_Lock_Remote],
                         STATUSBAR_LOCKR_X_POS, STATUSBAR_Y_POS,
                         STATUSBAR_LOCKR_WIDTH, SB_ICON_HEIGHT);
}
#endif

#if (CONFIG_LED == LED_VIRTUAL) || defined(HAVE_REMOTE_LCD)
/*
 * no real LED: disk activity in status bar
 */
static void gui_statusbar_led(struct screen * display)
{
    display->mono_bitmap(bitmap_icon_disk,
                         STATUSBAR_DISK_X_POS(display->getwidth()),
                         STATUSBAR_Y_POS, STATUSBAR_DISK_WIDTH,
                         SB_ICON_HEIGHT);
}
#endif

#ifdef HAVE_STATUSBAR_RF_ICONS
/* Statusbar RF icons, plotted pixel-by-pixel with fillrect: the
 * column/bit packing of the bitmap helpers is ambiguous across
 * ports, and fillrect makes every pixel deterministic.
 * Coordinates are relative to the icon's top-left corner; both
 * glyphs span the full bar height (12 rows).  wifi = three
 * concentric arcs above a dot; bt = the Hagall/Bjarkan bind
 * rune (stem with interleaved arms). */
#define RF_ICON_ROWS 12
#define RF_WIFI_W    16
#define RF_BT_W      10

static const unsigned char rf_wifi_px[][2] = {
    {6,3},{7,3},{8,3},{9,3},{4,4},{5,4},{6,4},{9,4},{10,4},{11,4},
    {2,5},{3,5},{12,5},{13,5},{2,6},{5,6},{6,6},{7,6},{8,6},{9,6},
    {10,6},{13,6},{1,7},{4,7},{11,7},{14,7},{3,8},{6,8},{7,8},{8,8},
    {9,8},{12,8},{5,9},{6,9},{9,9},{10,9},{7,10},{8,10},{7,11},{8,11},
};
static const unsigned char rf_bt_px[][2] = {
    {4,0},{5,0},{1,1},{4,1},{5,1},{6,1},{2,2},{4,2},{5,2},{7,2},
    {2,3},{4,3},{5,3},{8,3},{3,4},{4,4},{5,4},{6,4},{7,4},{4,5},
    {5,5},{3,6},{4,6},{5,6},{6,6},{7,6},{2,7},{4,7},{5,7},{8,7},
    {2,8},{4,8},{5,8},{7,8},{1,9},{4,9},{5,9},{6,9},{4,10},{5,10},
};

static void gui_statusbar_rf_plot(struct screen *display,
                                  const unsigned char px[][2],
                                  int count, int x, int y)
{
    for (int i = 0; i < count; i++)
        display->fillrect(x + px[i][0], y + px[i][1], 1, 1);
}

static void gui_statusbar_icon_rf(struct screen *display,
                                  const struct status_info *info)
{
    /* the clock hugs the right edge, rendered in the sysfont -
     * measure it the same way the time renderer does instead of
     * guessing its width, and stack the icons to its left */
    unsigned int cw, ch;
    font_getstringsize("00:00", &cw, &ch, FONT_SYSFIXED);
    int x = STATUSBAR_TIME_X_END(display->getwidth()) - (int)cw - 3;
    int y = (STATUSBAR_HEIGHT - RF_ICON_ROWS) / 2;
    if (y < 0)
        y = 0;

    if (info->rf_wifi)
    {
        x -= RF_WIFI_W;
        gui_statusbar_rf_plot(display, rf_wifi_px,
                              (int)(sizeof(rf_wifi_px) / 2), x, y);
    }
    if (info->rf_bt)
    {
        x -= 2 + RF_BT_W;
        gui_statusbar_rf_plot(display, rf_bt_px,
                              (int)(sizeof(rf_bt_px) / 2), x, y);
    }
}
#endif

#if CONFIG_RTC
/*
 * Print time to status bar
 */
static void gui_statusbar_time(struct screen * display, struct tm *time)
{
    unsigned char buffer[6];
    const unsigned char *p = buffer;
    unsigned int width, height;
    int hour, minute;
    if ( valid_time(time) ) {
        hour = time->tm_hour;
        minute = time->tm_min;
        if ( global_settings.timeformat ) { /* 12 hour clock */
            hour %= 12;
            if ( hour == 0 ) {
                hour += 12;
            }
        }
        snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
    }
    else {
        p = "--:--";
    }

    font_getstringsize(p, &width, &height, FONT_SYSFIXED);
    if (height <= STATUSBAR_HEIGHT) {
        display->putsxy(STATUSBAR_TIME_X_END(display->getwidth()) - width,
                        STATUSBAR_Y_POS, p);
    }

}
#endif

#ifdef HAVE_RECORDING
/**
 * Write a number to the display using bitmaps and return new position
 */
static int write_bitmap_number(struct screen * display, int value,
                               int x, int y)
{
    char buf[12], *ptr;
    itoa_buf(buf, sizeof(buf), value);

    for (ptr = buf; *ptr != '\0'; ptr++, x += BM_GLYPH_WIDTH)
        display->mono_bitmap(bitmap_glyphs_4x8[*ptr - '0'], x, y,
                             BM_GLYPH_WIDTH, SB_ICON_HEIGHT);
    return x;
}

/**
 * Write format info bitmaps - right justified
 */
static void gui_statusbar_write_format_info(struct screen * display)
{
    /* Can't fit info for sw codec targets in statusbar using FONT_SYSFIXED
       so must use icons */
    int rec_format = global_settings.rec_format;
    unsigned bitrk = 0; /* compiler warns about unitialized use !! */
    int xpos       = STATUSBAR_ENCODER_X_POS;
    int width      = STATUSBAR_ENCODER_WIDTH;
    const unsigned char *bm = bitmap_formats_18x8[rec_format];

    if (rec_format == REC_FORMAT_MPA_L3)
    {
        /* Special handling for mp3 */
        bitrk = global_settings.mp3_enc_config.bitrate;
        bitrk = mp3_enc_bitr[bitrk];

        width = BM_MPA_L3_M_WIDTH;

        /* Slide 'M' to right if fewer than three digits used */
        if (bitrk > 999)
            bitrk = 999; /* neurotic safety check if corrupted */
        else
        {
            if (bitrk < 100)
                xpos += BM_GLYPH_WIDTH;
            if (bitrk < 10)
                xpos += BM_GLYPH_WIDTH;
        }
    }

    /* Show bitmap - clipping right edge if needed */
    display->mono_bitmap_part(bm, 0, 0, STATUSBAR_ENCODER_WIDTH,
        xpos, STATUSBAR_Y_POS, width, SB_ICON_HEIGHT);

    if (rec_format == REC_FORMAT_MPA_L3)
    {
        xpos += BM_MPA_L3_M_WIDTH; /* to right of 'M' */
        write_bitmap_number(display, bitrk, xpos, STATUSBAR_Y_POS);
    }
}

/**
 * Write sample rate using bitmaps - left justified
 */
static void gui_statusbar_write_samplerate_info(struct screen * display)
{
    unsigned long samprk;
    int xpos;

#ifdef SIMULATOR
    samprk = 44100;
#else
#ifdef HAVE_SPDIF_REC
    if (global_settings.rec_source == AUDIO_SRC_SPDIF)
        /* Use rate in use, not current measured rate if it changed */
        samprk = pcm_rec_sample_rate();
    else
#endif
        samprk = rec_freq_sampr[global_settings.rec_frequency];
#endif /* SIMULATOR */

    samprk /= 1000;
    if (samprk > 99)
        samprk = 99;  /* Limit to 3 glyphs */

    xpos = write_bitmap_number(display, (unsigned)samprk,
                               STATUSBAR_RECFREQ_X_POS, STATUSBAR_Y_POS);

    /* write the 'k' */
    display->mono_bitmap(bitmap_glyphs_4x8[Glyph_4x8_k], xpos,
                         STATUSBAR_Y_POS, BM_GLYPH_WIDTH,
                         SB_ICON_HEIGHT);
}

static void gui_statusbar_icon_recording_info(struct screen * display)
{
    /* Display Codec info in statusbar */
    gui_statusbar_write_format_info(display);

    /* Display Samplerate info in statusbar */
    gui_statusbar_write_samplerate_info(display);

    /* Display Channel status in status bar */
    if(global_settings.rec_channels)
    {
        display->mono_bitmap(bitmap_icons_5x8[Icon_Mono],
                             STATUSBAR_RECCHANNELS_X_POS , STATUSBAR_Y_POS,
                             STATUSBAR_RECCHANNELS_WIDTH, SB_ICON_HEIGHT);
    }
    else
    {
        display->mono_bitmap(bitmap_icons_5x8[Icon_Stereo],
                             STATUSBAR_RECCHANNELS_X_POS, STATUSBAR_Y_POS,
                             STATUSBAR_RECCHANNELS_WIDTH, SB_ICON_HEIGHT);
    }
}
#endif /* HAVE_RECORDING */

void gui_syncstatusbar_init(struct gui_syncstatusbar * bars)
{
    FOR_NB_SCREENS(i) {
        gui_statusbar_init(&(screens[i]), &(bars->statusbars[i]));
    }
}


#ifdef HAVE_REMOTE_LCD
enum statusbar_values statusbar_position(int screen)
{
    if (screen == SCREEN_REMOTE)
        return global_settings.remote_statusbar;
    return global_settings.statusbar;
}
#endif
