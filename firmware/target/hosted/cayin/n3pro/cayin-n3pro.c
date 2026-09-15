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
 * Recovered from the stock player and kernel:
 *   /sys/devices/platform/cayin-n3pro.0/timbre_select  "transistor" | "tube"
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
#include "n3pro-bt-pcm.h"
#include "n3pro-bt-input.h"

#define TIMBRE_PATH      CAYIN_N3PRO_SYSFS_BASE "/timbre_select"

static const char * const timbre_names[] =
{
    "transistor", "tube"
};

void cayin_set_timbre(int mode)
{
    if (mode < CAYIN_TIMBRE_TRANSISTOR || mode > CAYIN_TIMBRE_TUBE)
        return;
    sysfs_set_string(TIMBRE_PATH, timbre_names[mode]);
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

/* Off without the grace delay (conditions that can never play through
 * the tube: mode off, bluetooth output). */
static void tube_off_now(void)
{
    if (tube_powered)
        tube_power(false);
    tube_off_tick = 0;
}

/* Off after TUBE_OFF_DELAY so a brief pause (or jack wiggle) does not
 * bounce the tube on and off. */
static void tube_off_delayed(void)
{
    if (!tube_powered)
    {
        tube_off_tick = 0;
        return;
    }
    if (tube_off_tick == 0)
        tube_off_tick = current_tick + TUBE_OFF_DELAY;
    else if (TIME_AFTER(current_tick, tube_off_tick))
    {
        tube_power(false);
        tube_off_tick = 0;
    }
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
    bool playing;

    /* 1. Headphone jack first: the JAN6418 buffer sits in the 3.5 mm
     *    single-ended path ONLY, so without that jack the tube may
     *    never be powered, no matter where audio runs. */
    if (out_ps != CAYIN_OUTPUT_HEADSET)
    {
        tube_off_delayed();
        return;
    }

    /* 2. Bluetooth OUTPUT bypasses the tube entirely: switch it off
     *    straight away while audio is routed there. */
    if (pcm_alsa_is_bluetooth_active())
    {
        tube_off_now();
        return;
    }

    /* 3. Who is playing on the wired path?
     *    Bluetooth INPUT streams to the jack through the mixer while
     *    the audio core stays idle -- count the receive link as
     *    playing.  Otherwise it is ordinary local playback. */
    if (n3pro_bt_rx_get_active())
        playing = n3pro_bt_rx_link_ok();
    else
    {
        int st = audio_status();

        playing = (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
    }

    if (tube_desired == 0)
    {
        tube_off_now();
        return;
    }

    if (playing)
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
    else
        tube_off_delayed();
}
