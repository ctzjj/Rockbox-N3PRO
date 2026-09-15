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
#include "n3pro-bt-input.h" /* n3pro_bt_rx_get_active() */
#include <alsa/asoundlib.h>
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
    /* Raise the headphone analog gain stage to "medium" exactly like the
     * stock player does at boot, so maximum loudness lines up. */
    sysfs_set_string(CAYIN_N3PRO_SYSFS_BASE "/output_gain", "output_gain_m");

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

    /* The softvol control is stereo ("Front Left/Right"); every channel has
     * to be written or the untouched ones keep their old (possibly muted)
     * value and the output comes out unbalanced. */
    {
        unsigned int ch;
        unsigned int cnt = snd_ctl_elem_info_get_count(info);

        if (cnt < 1)
            cnt = 1;
        if (cnt > 8)
            cnt = 8;

        for (ch = 0; ch < cnt; ch++)
            snd_ctl_elem_value_set_integer(val, ch, step);
    }

    {
        int rc = snd_ctl_elem_write(btvol_ctl, val);
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

#if defined(CAYIN_N3PRO)
    /* Stock-player volume model, from /usr/resource/ot_devices.json:
     * the hardware register equals the 0..100 volume step (255 at step 0,
     * 1..100 at steps 1..100 -- this driver's control is loudest at
     * higher values) and a per-step software gain in centibels supplies
     * the fine curve, applied to the 32-bit sample stream by pcm-alsa. */
    static const int sw_gain[101] =
    {
        -1500, -1200,  -600,  -510,  -470,  -420,  -395,  -375,  -361,  -358,
         -346,  -335,  -325,  -316,  -308,  -300,  -292,  -285,  -278,  -271,
         -264,  -258,  -252,  -246,  -240,  -235,  -230,  -225,  -220,  -215,
         -210,  -207,  -204,  -201,  -198,  -195,  -192,  -189,  -186,  -183,
         -180,  -177,  -174,  -171,  -168,  -165,  -162,  -159,  -156,  -153,
         -150,  -147,  -144,  -141,  -138,  -135,  -132,  -129,  -126,  -123,
         -120,  -117,  -114,  -111,  -108,  -105,  -102,   -99,   -96,   -93,
          -90,   -87,   -84,   -81,   -78,   -75,   -72,   -69,   -66,   -63,
          -60,   -57,   -54,   -51,   -48,   -45,   -42,   -39,   -36,   -33,
          -30,   -27,   -24,   -21,   -18,   -15,   -12,    -9,    -6,    -3,
            0
    };

    if (!hw_init)
        return;

    /* Bluetooth receive: the wired output stays wide open (HW step
     * 100, unity software gain) -- the phone is the volume control.
     * Pinning it HERE instead of saving/restoring the global volume
     * means an unclean end (crash, poweroff while waiting for the
     * phone) can never leave a persisted maximum in the settings. */
    if (n3pro_bt_rx_get_active())
    {
        l = r = 100;
        alsa_controls_set_ints("Left Playback Volume", 1, &l);
        alsa_controls_set_ints("Right Playback Volume", 1, &r);
        pcm_set_mixer_volume(0, 0);
        return;
    }

    int step_l = (vol_l + 1020) / 10;
    int step_r = (vol_r + 1020) / 10;

    if (step_l < 0)   step_l = 0;
    if (step_l > 100) step_l = 100;
    if (step_r < 0)   step_r = 0;
    if (step_r > 100) step_r = 100;

    l = step_l ? step_l : 255;
    r = step_r ? step_r : 255;
#else
    l = -vol_l/5;
    r = -vol_r/5;

    if (!hw_init)
        return;
#endif

    alsa_controls_set_ints("Left Playback Volume", 1, &l);
    alsa_controls_set_ints("Right Playback Volume", 1, &r);

#if defined(CAYIN_N3PRO)
    if (pcm_alsa_is_bluetooth_active())
    {
        /* The bluetooth route bypasses the AK4493, so the hardware
         * register is inert there; the userspace softvol wrapped around
         * the bluetooth PCM is the volume control.  Hold the 32-bit
         * sample stream at unity so the two attenuations do not stack. */
        pcm_set_mixer_volume(0, 0);
        n3pro_set_bt_volume((vol_l + vol_r) / 2);
    }
    else
        pcm_set_mixer_volume(sw_gain[step_l], sw_gain[step_r]);
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

