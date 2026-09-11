/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 *
 * Copyright (c) 2018 Marcin Bukat
 * Copyright (c) 2025 Solomon Peachy
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

//#define LOGF_ENABLE

#include "config.h"
#include "audio.h"
#include "audiohw.h"
#include "button.h"
#include "system.h"
#include "kernel.h"
#include "panic.h"
#include "sysfs.h"
#include "alsa-controls.h"
#include "pcm-alsa.h"

#include "logf.h"

#if defined(CAYIN_N3PRO)
#include "cayin-n3pro.h"    /* shared electron-tube power management */
#endif

int hiby_has_valid_output(void);

static int hw_init = 0;

static long int vol_l_hw = 255;
static long int vol_r_hw = 255;
static long int last_ps = -1;

static int muted = -1;

void audiohw_mute(int mute)
{
    if (hw_init < 0 || muted == mute)
        return;

    muted = mute;

#if defined(CAYIN_N3PRO)
    alsa_controls_set_bool("AK4493 Soft Mute", !!mute);
#else
    alsa_controls_set_bool("Mute Output", !!mute);
#endif
}

int hiby_has_valid_output(void) {
    long int ps = 0; // Muted, if nothing is plugged in!

    int status = 0;

    if (!hw_init) return ps;

    const char * const sysfs_hs_switch = "/sys/class/switch/headset/state";
    const char * const sysfs_bal_switch = "/sys/class/switch/balance/state";

    if (sysfs_get_int(sysfs_hs_switch, &status) && status)
        ps = 2; // headset

    if (sysfs_get_int(sysfs_bal_switch, &status) && status)
        ps = 3; // balanced output

    return ps;
}

int hiby_get_outputs(void){
    long int ps = hiby_has_valid_output();

    hiby_set_output(ps);

    return ps;
}

void hiby_set_output(int ps)
{
    if (!hw_init || muted) return;

    // Default to headset if nothing was ever inserted; otherwise, R3 Pro II crashes on playback
    if (ps == 0)
    {
        ps = last_ps > 0 ? last_ps : 2;
    }

    if (last_ps != ps)
    {
        logf("set out %d/%d", ps, last_ps);
        /* Output port switch */
        last_ps = ps;
        alsa_controls_set_ints("Output Port Switch", 1, &last_ps);
        audiohw_set_volume(vol_l_hw, vol_r_hw);
    }
}

void audiohw_preinit(void)
{
    logf("hw preinit");
    alsa_controls_init("default");
    hw_init = 1;

    audiohw_mute(false);  /* No need ? */
#if !defined(CAYIN_N3PRO)
    alsa_controls_set_bool("DOP_EN", 0); //isDSD
#endif
}

void audiohw_postinit(void)
{
    logf("hw postinit");
#if defined(CAYIN_N3PRO)
    /* make sure the AK4493 output port is routed (headset/line/balanced) */
    hiby_get_outputs();
#endif
}

void audiohw_close(void)
{
    logf("hw close");
    hw_init = 0;
    alsa_controls_close();
}

void audiohw_set_frequency(int fsel)
{
    (void)fsel;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    logf("hw vol %d %d", vol_l, vol_r);

    long l,r;

    vol_l_hw = vol_l;
    vol_r_hw = vol_r;

    l = -vol_l/5;
    r = -vol_r/5;

    if (!hw_init)
        return;

    alsa_controls_set_ints("Left Playback Volume", 1, &l);
    alsa_controls_set_ints("Right Playback Volume", 1, &r);
}

void audiohw_set_filter_roll_off(int value)
{
    logf("rolloff %d", value);
#if defined(CAYIN_N3PRO)
    /* N3Pro order:
     *   0 = Short delay sharp (default)
     *   1 = Short delay slow
     *   2 = Sharp
     *   3 = Slow
     *   4 = Super slow */
#else
    /* 0 = Sharp;
     *       1 = Slow;
     *       2 = Short Sharp
     *       3 = Short Slow
     *       4 = Super Slow */
#endif
    long int value_hw = value;
    alsa_controls_set_ints("Digital Filter", 1, &value_hw);
#if defined(CAYIN_N3PRO)
    alsa_controls_set_ints("AK4493 Digital Filter", 1, &value_hw);
#endif
}

#if defined(AUDIOHW_HAVE_TUBE_MODE)
/* 0 = transistor (tube off), 1 = triode, 2 = ultra-linear.
 *
 * The tube must be powered on through the platform sysfs first: the
 * triode/ultra-linear wiring (tube mode a0/a1 GPIOs) is only switched by
 * the "Timbre Tube Mode" ALSA control while the tube supply is enabled,
 * otherwise that write is gated off. */
void audiohw_set_tube_mode(int mode)
{
    logf("tube %d", mode);
#if defined(CAYIN_N3PRO)
    /* Power and triode/ultra-linear wiring are managed by the N3Pro driver
     * in step with the playback state (see cayin-n3pro.c); just record the
     * desired mode here. */
    cayin_tube_set_mode(mode);
#else
    long int value_hw = mode;
    alsa_controls_set_ints("Timbre Tube Mode", 1, &value_hw);
#endif
}
#endif

