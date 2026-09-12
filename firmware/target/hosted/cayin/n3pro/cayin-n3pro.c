/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Cayin N3Pro hardware-specific controls.
 *
 * Everything below was recovered from the stock player and kernel:
 *   /sys/devices/platform/cayin-n3pro.0/timbre_select  "transistor" | "tube"
 *   /sys/devices/platform/cayin-n3pro.0/power_output   "standard" | "high_resistant"
 *   /sys/devices/platform/cayin-n3pro.0/output_gain    "output_gain_l|m|h"
 *   /sys/devices/platform/cayin-n3pro.0/line_out_gain  "lo_gain_l|m|h"
 *   /sys/devices/platform/cayin-n3pro.0/dsd_gain
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
#include "sysfs.h"
#include "kernel.h"
#include "system.h"
#include "audio.h"
#include "alsa-controls.h"
#include "cayin-n3pro.h"

#define TIMBRE_PATH      CAYIN_N3PRO_SYSFS_BASE "/timbre_select"
#define POWER_PATH       CAYIN_N3PRO_SYSFS_BASE "/power_output"
#define OUT_GAIN_PATH    CAYIN_N3PRO_SYSFS_BASE "/output_gain"
#define LO_GAIN_PATH     CAYIN_N3PRO_SYSFS_BASE "/line_out_gain"
#define DSD_GAIN_PATH    CAYIN_N3PRO_SYSFS_BASE "/dsd_gain"

static const char * const timbre_names[] =
{
    "transistor", "tube"
};

static const char * const power_names[] =
{
    "standard", "high_resistant"
};

static const char * const out_gain_names[] =
{
    "output_gain_l", "output_gain_m", "output_gain_h"
};

static const char * const lo_gain_names[] =
{
    "lo_gain_l", "lo_gain_m", "lo_gain_h"
};

void cayin_set_timbre(int mode)
{
    if (mode < CAYIN_TIMBRE_TRANSISTOR || mode > CAYIN_TIMBRE_TUBE)
        return;
    sysfs_set_string(TIMBRE_PATH, timbre_names[mode]);
}

int cayin_get_timbre(void)
{
    char buf[16];
    if (!sysfs_get_string(TIMBRE_PATH, buf, sizeof(buf)))
        return CAYIN_TIMBRE_TRANSISTOR;
    return (buf[0] == 't' && buf[1] == 'u') ? CAYIN_TIMBRE_TUBE
                                            : CAYIN_TIMBRE_TRANSISTOR;
}

void cayin_set_power_output(int mode)
{
    if (mode < CAYIN_POWER_STANDARD || mode > CAYIN_POWER_HIGH_RESISTANT)
        return;
    sysfs_set_string(POWER_PATH, power_names[mode]);
}

void cayin_set_output_gain(int gain)
{
    if (gain < CAYIN_GAIN_LOW || gain > CAYIN_GAIN_HIGH)
        return;
    sysfs_set_string(OUT_GAIN_PATH, out_gain_names[gain]);
}

void cayin_set_line_out_gain(int gain)
{
    if (gain < CAYIN_GAIN_LOW || gain > CAYIN_GAIN_HIGH)
        return;
    sysfs_set_string(LO_GAIN_PATH, lo_gain_names[gain]);
}

void cayin_set_dsd_gain(int gain)
{
    char buf[8];
    /* Best-effort: values not yet confirmed on hardware. */
    if (gain < 0)
        gain = 0;
    if (gain > 6)
        gain = 6;
    buf[0] = '0' + gain;
    buf[1] = '\0';
    sysfs_set_string(DSD_GAIN_PATH, buf);
}

/* ---- electron-tube power management ---------------------------------- */
/* The tube is powered only while audio is actually playing on the 3.5 mm
 * single-ended headphone output and is switched off again TUBE_OFF_DELAY
 * after playback stops/pauses or another jack is selected.  Kept entirely
 * in the N3Pro target so no shared code is involved.  The triode/
 * ultra-linear wiring is only accepted by the hardware while the tube
 * supply is on, so we always power up first and apply the wiring right
 * after.  Line out and the 4.4 mm balanced output are hard-wired to the
 * transistor stage (stock firmware does the same).                        */

#define TUBE_OFF_DELAY   (10*HZ)

static int  tube_desired  = 0;      /* 0=off 1=triode 2=ultra-linear */
static int  tube_wiring   = -1;     /* last value written to the mixer */
static bool tube_powered  = false;
static long tube_off_tick = 0;

static void tube_power(bool on)
{
    if (on == tube_powered)
        return;
    cayin_set_timbre(on ? CAYIN_TIMBRE_TUBE : CAYIN_TIMBRE_TRANSISTOR);
    tube_powered = on;
    tube_wiring = -1;               /* re-apply wiring after (re)power */
}

void cayin_tube_set_mode(int mode)
{
    if (mode < 0)
        mode = 0;
    if (mode > 2)
        mode = 2;
    tube_desired = mode;
    tube_wiring = -1;
    if (mode == 0)
        tube_power(false);
}

void cayin_tube_tick(int out_ps)
{
    int st = audio_status();
    bool playing = (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);

    if (tube_desired == 0)
    {
        if (tube_powered)
            tube_power(false);
        tube_off_tick = 0;
        return;
    }

    /* JAN6418 tube buffer sits in the single-ended headphone path only. */
    if (playing && out_ps == CAYIN_OUTPUT_HEADSET)
    {
        /* Resume cancels a pending power-off so a quick pause/resume does
         * not bounce the tube on and off. */
        tube_off_tick = 0;
        if (!tube_powered)
            tube_power(true);
        if (tube_wiring != tube_desired)
        {
            long v = tube_desired;
            alsa_controls_set_ints("Timbre Tube Mode", 1, &v);
            tube_wiring = tube_desired;
        }
    }
    else if (tube_powered)
    {
        if (tube_off_tick == 0)
            tube_off_tick = current_tick + TUBE_OFF_DELAY;
        else if (TIME_AFTER(current_tick, tube_off_tick))
        {
            tube_power(false);
            tube_off_tick = 0;
        }
    }
}
