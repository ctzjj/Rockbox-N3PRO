/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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
#include <linux/input.h>

#include "sysfs.h"
#include "button.h"
#include "button-target.h"
#include "touchscreen.h"
#include "hibylinux_codec.h"
#ifndef BOOTLOADER
#include "cayin-n3pro.h"
#endif
#ifdef HAVE_BACKLIGHT
#include "backlight.h"
#endif /* HAVE_BACKLIGHT */

/*
 * Cayin N3Pro (Ingenic X1000 + HiByOS Linux).
 *
 *   gpio-keys (evdev): power / prev / next / ok(play)
 *   rotary potentiometer: volume up/down
 *   GT9XX-TS          : touchscreen (type B multitouch)
 *   bottom-front HOME : capacitive/touch key
 *
 * Keycodes are best-effort from the stock kernel gpio-keys map and must be
 * verified/adjusted on real hardware.
 */

int button_map(int keycode)
{
    switch(keycode)
    {
        case KEY_POWER:
            return BUTTON_POWER;

        case KEY_PREVIOUSSONG:
            return BUTTON_PREV;

        case KEY_NEXTSONG:
            return BUTTON_NEXT;

        case KEY_PLAYPAUSE:
            return BUTTON_PLAY;

        /* Rotary volume encoder: ticks arrive as KEY_LEFT/KEY_RIGHT and each
         * tick is a press+release within microseconds.  Map to the scroll
         * wheel so button-devinput queues them (plain buttons would be lost). */
        case KEY_LEFT:
            return BUTTON_SCROLL_BACK;   /* volume up */
        case KEY_RIGHT:
            return BUTTON_SCROLL_FWD;    /* volume down */
        case KEY_VOLUMEUP:
            return BUTTON_SCROLL_BACK;
        case KEY_VOLUMEDOWN:
            return BUTTON_SCROLL_FWD;

        /* Bottom-front touch HOME key (GT9XX reports KEY_MENU) */
        case KEY_MENU:
        case KEY_HOMEPAGE:
        case KEY_HOME:
            return BUTTON_HOME;

        case BTN_TOUCH:
        {
#ifdef HAVE_BACKLIGHT
            if (is_backlight_on(true)) {
                return BUTTON_TOUCH;
            }
            /* Screen off: ignore touch so it cannot spuriously wake. */
            return 0;
#else
            return BUTTON_TOUCH;
#endif
        }

        default:
            return 0;
    }
}

bool headphones_inserted(void)
{
#ifdef BOOTLOADER
    return false;
#else
    /* Called on every button poll; use it as the periodic N3Pro housekeeping
     * tick (electron-tube power follows the playback state). */
    cayin_tube_tick();
    /* hiby_get_outputs() also programs the AK4493 "Output Port Switch";
     * without this the DAC output port is left unrouted -> no sound. */
    int ps = hiby_get_outputs();
    return (ps == 2);
#endif
}
