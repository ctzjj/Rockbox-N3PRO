/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/    \/
 *
 * Copyright (C) 2026 ctzjj
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
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "sysfs.h"
#include "settings.h"
#include "power.h"
#include "kernel.h"
#include "playback.h"
#include "audio.h"

/* The N3Pro's RGB indicator is a TI LP5562 driven by a custom kernel pattern
 * engine.  The individual /sys/class/leds/{R,G,B,W}/brightness files have no
 * effect while the engine is running; instead a preset pattern is selected by
 * writing its number to the led_pattern sysfs node:
 *
 *   0 = off            1 = yellow-green    2 = green
 *   3 = blue           4 = purple          5 = white
 *   6 = red breathing  7 = red solid
 */
static const char * const sysfs_led_pattern =
    "/sys/bus/i2c/devices/1-0030/led_pattern";

enum n3pro_led_pattern {
    N3PRO_LED_INVALID = -1,
    N3PRO_LED_OFF,
    N3PRO_LED_YELLOW_GREEN,
    N3PRO_LED_GREEN,
    N3PRO_LED_BLUE,
    N3PRO_LED_PURPLE,
    N3PRO_LED_WHITE,
    N3PRO_LED_RED_BREATHING,
    N3PRO_LED_RED_SOLID,
};

#define DEFAULT_PATTERN N3PRO_LED_YELLOW_GREEN

static int last_pattern = N3PRO_LED_INVALID;

static void set_led(int pattern)
{
    if (last_pattern != pattern) {
        last_pattern = pattern;
        sysfs_set_int(sysfs_led_pattern, pattern);
    }
}

#ifdef HAVE_GENERAL_PURPOSE_LED
void led_hw_brightness(int brightness)
{
    /* No software brightness control; the pattern engine owns the channels. */
    (void)brightness;
}

void led_hw_charged(void)
{
    set_led(N3PRO_LED_RED_SOLID);
}

void led_hw_charging(void)
{
    set_led(N3PRO_LED_RED_BREATHING);
}

void led_hw_off(void)
{
    /* Don't extinguish the indicator while charging is indicated. */
    if (!charging_state()) {
        set_led(N3PRO_LED_OFF);
    }
}

void led_hw_on(void)
{
    if (charging_state()) {
        led_hw_charging();
    } else if (global_settings.use_led_indicators) {
        set_led(DEFAULT_PATTERN);
    } else {
        led_hw_off();
    }
}

void led_hw_set_mode(bool mode)
{
    if (mode) {
        led_hw_on();
    } else {
        led_hw_off();
    }
}
#endif

#ifndef BOOTLOADER
/* The generic Rockbox LED core never calls the charging hooks, so poll the
 * power supply ourselves from the button tick. */
void led_n3pro_tick(void)
{
    static long last = 0;
    char status[16] = {0};

    if (TIME_BEFORE(current_tick, last + HZ))
        return;
    last = current_tick;

    sysfs_get_string("/sys/class/power_supply/battery/status", status,
                     sizeof(status));

    if (strncmp(status, "Charging", 8) == 0) {
        set_led(N3PRO_LED_RED_BREATHING);
    } else if (strncmp(status, "Full", 4) == 0) {
        set_led(N3PRO_LED_RED_SOLID);
    } else {
        /* Like the stock firmware: lit only while audio is playing (and only
         * if the user enabled the LED indicators), with the colour chosen by
         * the track's sample rate. */
        unsigned int st = audio_status();
        if ((st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE)
            && global_settings.use_led_indicators) {
            struct mp3entry *id3 = audio_current_track();
            unsigned int hz = id3 ? id3->frequency : 0;

            if (hz == 0)             set_led(DEFAULT_PATTERN);
            else if (hz <= 48000)    set_led(N3PRO_LED_YELLOW_GREEN); /* 44.1/48k */
            else if (hz <= 96000)    set_led(N3PRO_LED_GREEN);        /* 88.2/96k */
            else if (hz <= 192000)   set_led(N3PRO_LED_BLUE);         /* 176.4/192k */
            else if (hz <= 384000)   set_led(N3PRO_LED_PURPLE);       /* 352.8/384k */
            else                     set_led(N3PRO_LED_WHITE);        /* DSD */
        } else {
            set_led(N3PRO_LED_OFF);
        }
    }
}
#endif
