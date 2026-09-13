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
#include "settings.h"
#include "sound.h"
#include "n3pro-bt-pcm.h"   /* pcm_alsa_is_bluetooth_active() */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdarg.h>
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

#if defined(CAYIN_N3PRO)
    /* The N3Pro has three separate output jacks, each with its own switch
     * device.  The returned values (and the value written to the AK4493
     * "Output Port Switch" mixer control by hiby_set_output()) match the
     * stock player's routing table:
     *   0 = spdif, 1 = lineout, 2 = headset, 3 = balance, 4 = i2s.
     * The balanced jack takes precedence, then headset, then line out. */
    const char * const sysfs_hs_switch  = "/sys/class/switch/headset/state";
    const char * const sysfs_lo_switch  = "/sys/class/switch/lineout/state";
    const char * const sysfs_bal_switch = "/sys/class/switch/balance/state";

    if (sysfs_get_int(sysfs_hs_switch, &status) && status)
        ps = 2; // headset

    if (ps == 0 && sysfs_get_int(sysfs_lo_switch, &status) && status)
        ps = 1; // line out

    if (sysfs_get_int(sysfs_bal_switch, &status) && status)
        ps = 3; // balanced output
#else
    const char * const sysfs_hs_switch = "/sys/class/switch/headset/state";
    const char * const sysfs_bal_switch = "/sys/class/switch/balance/state";

    if (sysfs_get_int(sysfs_hs_switch, &status) && status)
        ps = 2; // headset

    if (sysfs_get_int(sysfs_bal_switch, &status) && status)
        ps = 3; // balanced output
#endif

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

#if defined(CAYIN_N3PRO)
static void n3pro_btvol_log(const char *fmt, ...)
{
    FILE *f = fopen("/mnt/sd_0/.rockbox/btvol.log", "a");
    va_list ap;

    if (!f)
        return;

    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* The bluetooth route bypasses the AK4493 DAC, so the hardware volume has
 * no effect on it.  Drive the userspace softvol control wrapped around the
 * vendor PCM instead, mapping the volume/volume_limit span onto whatever
 * raw range the softvol entry was created with (min_dB/max_dB in the
 * generated asound.conf, 5 steps per dB). */
static void n3pro_set_bt_volume(int vol_cb)
{
    int min_vol = sound_min(SOUND_VOLUME);
    int max_vol = sound_max(SOUND_VOLUME);
    int limit_vol = global_settings.volume_limit;
    int span;
    int pct;
    long max_step;
    long step;
    static snd_ctl_t *btvol_ctl = NULL;
    snd_ctl_elem_value_t *val;
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;

    if (limit_vol < max_vol)
        max_vol = limit_vol;
    if (max_vol < min_vol)
        max_vol = min_vol;

    if (vol_cb < min_vol)
        vol_cb = min_vol;
    if (vol_cb > max_vol)
        vol_cb = max_vol;

    span = max_vol - min_vol;
    if (span <= 0)
        pct = 100;
    else
        pct = ((vol_cb - min_vol) * 100 + span / 2) / span;

    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;

    /* The softvol control only exists while the wrapped PCM is open, which
     * is well after alsa_controls_init() cached the control list, so it has
     * to be written through a direct ctl handle instead. */
    if (!btvol_ctl && snd_ctl_open(&btvol_ctl, "default", 0) < 0)
    {
        btvol_ctl = NULL;
        return;
    }

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, "Bluetooth Vol");

    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_info_set_id(info, id);
    if (snd_ctl_elem_info(btvol_ctl, info) < 0)
    {
        snd_ctl_close(btvol_ctl);
        btvol_ctl = NULL;
        return;
    }

    max_step = snd_ctl_elem_info_get_max(info);
    if (max_step <= 0)
        max_step = 255;

    step = (pct * max_step + 50) / 100;

    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_value_set_id(val, id);
    snd_ctl_elem_value_set_integer(val, 0, step);

    {
        int rc = snd_ctl_elem_write(btvol_ctl, val);
        n3pro_btvol_log("btvol: vol_cb=%d pct=%d max=%ld step=%ld rc=%d",
                        vol_cb, pct, max_step, step, rc);
        if (rc < 0)
        {
            /* The control disappears when the bluetooth PCM is closed;
             * drop the handle so the next volume change reopens it. */
            snd_ctl_close(btvol_ctl);
            btvol_ctl = NULL;
        }
    }
}
#endif

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

#if defined(CAYIN_N3PRO)
    {
        int bt_active = pcm_alsa_is_bluetooth_active();
        n3pro_btvol_log("ahw: vol=%d bt=%d", (vol_l + vol_r) / 2, bt_active);
        if (bt_active)
            n3pro_set_bt_volume((vol_l + vol_r) / 2);
    }
#endif
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

