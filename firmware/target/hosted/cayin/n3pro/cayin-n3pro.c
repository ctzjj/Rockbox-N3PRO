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

    /* The bluetooth output bypasses the JAN6418 buffer entirely (it sits in
     * the 3.5 mm single-ended path), so switch the tube off straight away
     * while audio is routed there. */
    if (pcm_alsa_is_bluetooth_active())
    {
        if (tube_powered)
            tube_power(false);
        tube_off_tick = 0;
        return;
    }

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
