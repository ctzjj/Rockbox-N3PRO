/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Cayin N3Pro hardware-specific controls (reverse engineered from the
 * stock hiby_player + the cayin-n3pro kernel driver).
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
#ifndef _CAYIN_N3PRO_H_
#define _CAYIN_N3PRO_H_

#include <stdbool.h>

#define CAYIN_N3PRO_SYSFS_BASE "/sys/devices/platform/cayin-n3pro.0"

/* Electron-tube (2x JAN6418) vs solid-state output stage (timbre_select) */
enum cayin_timbre
{
    CAYIN_TIMBRE_TRANSISTOR = 0,   /* "transistor" (tubes off) */
    CAYIN_TIMBRE_TUBE,             /* "tube" (triode / ultra-linear) */
};

/* Timbre (electron tube) stage selector */
void cayin_set_timbre(int mode);

/* Output ports, as reported by hiby_get_outputs().  The values match the
 * stock player's routing table for the AK4493 "Output Port Switch" mixer
 * control: 0 = spdif, 1 = lineout, 2 = headset, 3 = balance, 4 = i2s. */
#define CAYIN_OUTPUT_NONE     0
#define CAYIN_OUTPUT_LINEOUT  1
#define CAYIN_OUTPUT_HEADSET  2
#define CAYIN_OUTPUT_BALANCED 3

/* Electron-tube (2x JAN6418) power management, driven by the playback state:
 * the tubes are powered only while audio is playing on the 3.5 mm
 * single-ended headphone output and are switched off again 10s after
 * playback stops/pauses or another output (line out / 4.4 balanced) is
 * selected (saves power and tube life).  Line out and the balanced output
 * are always driven by the transistor stage, exactly like the stock
 * firmware.  The triode / ultra-linear wiring is only programmable while
 * the tubes are powered.
 *   mode: 0 = transistor (off), 1 = triode, 2 = ultra-linear            */
void cayin_tube_set_mode(int mode);
void cayin_tube_tick(int out_ps);

/* RGB indicator (LP5562 pattern engine), polled from the button tick. */
void led_n3pro_tick(void);

/* Re-apply the USB Audio setting once PCM is up (usb-n3pro.c). */
void cayin_usb_retry(void);

#endif /* _CAYIN_N3PRO_H_ */
