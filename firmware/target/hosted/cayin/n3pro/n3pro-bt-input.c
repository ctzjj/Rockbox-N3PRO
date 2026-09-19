/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware  |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by the Rockbox-N3PRO contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software, either version 2 of the
 * License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Bluetooth A2DP receive pump for the Cayin N3Pro (hosted).
 *
 * The vendor bluetoothd exposes the A2DP sink stream through the ALSA
 * bluetooth plugin in capture mode: opening the "btin" device delivers
 * the decoded S16 stereo stream sent by the connected phone. A detached
 * pump thread (patterned after usb-dac-hiby.c) drains it into a small
 * single-producer/single-consumer ring; the mixer's get_more callback
 * plays it through the normal ALSA output path with the shared
 * CODEC_IDX_AUDIO DSP chain applied, so the EQ and Sound settings apply
 * to received audio exactly as they do to USB DAC input.
 *
 * The capture is opened through a private config tree parsed from our
 * own /tmp/btin.conf (patterned after the output path's lconf open):
 * /etc/asound.conf belongs to the vendor stack and is never touched.
 * The peer MAC comes from the ACL connection's sysfs node.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "kernel.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "n3pro-bt-pcm.h"
#include "system.h"
#include "thread.h"
#include "dsp_core.h"
#include "settings.h"
#include "sound.h"

#include <alsa/asoundlib.h>

/*#define LOGF_ENABLE*/
#include "logf.h"

#include "bt_input.h"

#define BTIN_PCM_NAME     "btin"
#define BTIN_CONF         "/tmp/btin.conf"
#define BTIN_RATE         44100
#define BTIN_LATENCY_US   200000   /* 200 ms capture buffering */
#define BTIN_SYSFS_BT     "/sys/class/bluetooth"

/* Ring of S16 stereo frames. A power of two so head/tail wrap cleanly.
 * Large enough to ride out the bluetooth plugin's bursty delivery at
 * 96 kHz (341 ms) without underrunning. */
#define BTIN_RING_FRAMES   32768
#define BTIN_CHUNK_FRAMES  512   /* mixer buffer granularity */

static int16_t btin_ring[BTIN_RING_FRAMES * 2];
static volatile unsigned int btin_head;  /* frames produced; pump only */
static volatile unsigned int btin_tail;  /* frames consumed; mixer only */
static pthread_t btin_thread;
static volatile bool btin_running;
static volatile bool btin_link_ok;       /* capture transport is up */
/* The pump may sit inside the plugin's open()/readi, so it is detached
 * and never joined: bumping the generation invalidates a stale thread. */
static volatile unsigned int btin_gen;
/* DSP chain the received PCM is run through; shared with normal
 * playback, prepared by the pump thread before streaming. */
static struct dsp_config *btin_dsp;
static int btin_rate;
/* Link-history for the WAITING vs DISCONNECTED distinction. */
static volatile bool btin_ever_connected;
/* True while the receive screen is open: only then may the pump keep
 * retrying for the phone. In the background a lost (or never
 * established) link winds the pump down instead of spinning. */
static volatile bool btin_fg;
static char btin_peer_mac[18];
/* Config tree backing the currently open capture handle (kept alive
 * for as long as the handle references it). */
static snd_config_t *btin_cfg;

/* Mixer callback (PCM feed context). Returns the next chunk from the
 * ring, zero-padded with silence on underrun so the stream never
 * stalls. Double buffered so the returned pointer stays valid until
 * the following call. */
static void btin_get_more(const void **start, size_t *size)
{
    static int16_t out[2][BTIN_CHUNK_FRAMES * 2];
    static int which;
    int16_t *buf;

    which ^= 1;
    buf = out[which];

    unsigned int tail = btin_tail;
    unsigned int avail = btin_head - tail;         /* wrap-safe */
    avail = MIN(avail, BTIN_CHUNK_FRAMES);

    for (unsigned int i = 0; i < avail; i++)
    {
        unsigned int idx = (tail + i) & (BTIN_RING_FRAMES - 1);
        buf[2 * i]     = btin_ring[2 * idx];
        buf[2 * i + 1] = btin_ring[2 * idx + 1];
    }
    memset(&buf[2 * avail], 0,
           (BTIN_CHUNK_FRAMES - avail) * 2 * sizeof(int16_t));
    btin_tail = tail + avail;

    *start = buf;
    *size = BTIN_CHUNK_FRAMES * 2 * sizeof(int16_t);
}

static const struct mixer_play_cbs btin_cbs =
{
    .get_more = btin_get_more,
};

/* Follow a sample-rate change: retime the mixer + ALSA output and the
 * DSP chain, and drop audio still buffered from the old rate.
 * Called from the pump thread only. */
static void btin_apply_rate(int rate)
{
    if (rate < 8000 || rate > 192000)
        rate = BTIN_RATE;
    if (rate == btin_rate)
        return;
    btin_rate = rate;

    btin_tail = btin_head;          /* ring: drop old-rate audio */

    mixer_set_frequency(rate);
    if (btin_dsp)
    {
        dsp_configure(btin_dsp, DSP_SET_FREQUENCY, rate);
        dsp_configure(btin_dsp, DSP_SET_OUT_FREQUENCY, rate);
    }

    /* mixer_set_frequency() stops the PCM driver and nothing restarts
     * it: re-kick our channel so the stream resumes at the new rate. */
    mixer_channel_play_data(PCM_MIXER_CHAN_BT_AUDIO, &btin_cbs, NULL, 0);
}

/* Find the currently ACL-connected peer ("hci0:NN" sysfs nodes) and
 * remember it for the status view. */
static bool btin_find_peer(char *mac, size_t len)
{
    DIR *d = opendir(BTIN_SYSFS_BT);

    if (d)
    {
        struct dirent *de;
        while ((de = readdir(d)))
        {
            if (strncmp(de->d_name, "hci0:", 5) != 0)
                continue;

            char path[64];
            snprintf(path, sizeof(path), BTIN_SYSFS_BT "/%.*s/address",
                     (int)sizeof(path) - (int)sizeof(BTIN_SYSFS_BT) - 9,
                     de->d_name);
            FILE *f = fopen(path, "r");
            if (!f)
                continue;
            if (fgets(mac, len, f))
            {
                char *nl = strchr(mac, '\n');
                if (nl)
                    *nl = '\0';
                fclose(f);
                /* D-Bus device lookups want canonical upper case. */
                for (char *p = mac; *p; p++)
                    *p = toupper((unsigned char)*p);
                snprintf(btin_peer_mac, sizeof(btin_peer_mac), "%.*s",
                         (int)sizeof(btin_peer_mac) - 1, mac);
                closedir(d);
                return true;
            }
            fclose(f);
        }
        closedir(d);
    }
    return false;
}

/* Open the capture through a private config tree parsed from our own
 * /tmp/btin.conf (never /etc/asound.conf). The tree is kept in *cfg for
 * as long as the handle references it. */
static int btin_open_capture(snd_pcm_t **pcm, snd_config_t **cfg)
{
    char mac[32];
    snd_config_t *top = NULL;
    snd_input_t *in = NULL;
    int err;

    if (!btin_find_peer(mac, sizeof(mac)))
        return -ENOENT;

    FILE *f = fopen(BTIN_CONF, "w");
    if (!f)
        return -EIO;
    fprintf(f, "pcm.%s {\n"
               "    type bluetooth\n"
               "    device \"%s\"\n"
               "    profile \"a2dp\"\n"
               "}\n", BTIN_PCM_NAME, mac);
    fclose(f);

    err = snd_config_top(&top);
    if (err >= 0)
        err = snd_input_stdio_open(&in, BTIN_CONF, "r");
    if (err >= 0)
    {
        err = snd_config_load(top, in);
        snd_input_close(in);
    }
    if (err >= 0)
        err = snd_pcm_open_lconf(pcm, BTIN_PCM_NAME, SND_PCM_STREAM_CAPTURE,
                                 SND_PCM_NONBLOCK, top);

    if (err >= 0)
        *cfg = top;
    else if (top)
        snd_config_delete(top);

    return err;
}

static void btin_close_capture(snd_pcm_t **pcm)
{
    snd_pcm_close(*pcm);
    *pcm = NULL;
    if (btin_cfg)
    {
        snd_config_delete(btin_cfg);
        btin_cfg = NULL;
    }
}

/* Configure the capture the way arecord does (M0 PoC): granular
 * hw params with a rate near 44.1 kHz, tolerating whatever the plugin
 * negotiated with the phone. snd_pcm_set_params() cannot be used here:
 * it demands the exact requested rate and the bluetooth plugin reports
 * the codec rate (96000 for LDAC), which fails with EINVAL. */
static int btin_setup_stream(snd_pcm_t *pcm, unsigned int *rate_out)
{
    snd_pcm_hw_params_t *hw;
    unsigned int rate = BTIN_RATE;
    unsigned int buf_us = BTIN_LATENCY_US;
    unsigned int per_us = BTIN_LATENCY_US / 4;
    int err;

    snd_pcm_hw_params_alloca(&hw);
    err = snd_pcm_hw_params_any(pcm, hw);
    if (err >= 0)
        err = snd_pcm_hw_params_set_access(pcm, hw,
                                           SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err >= 0)
        err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (err >= 0)
        err = snd_pcm_hw_params_set_channels(pcm, hw, 2);
    if (err >= 0)
    {
        err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
    }
    if (err >= 0)
    {
        /* Granular requests the plugin may refuse without harm. */
        snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buf_us, NULL);
        snd_pcm_hw_params_set_period_time_near(pcm, hw, &per_us, NULL);
        err = snd_pcm_hw_params(pcm, hw);
    }
    if (err < 0)
        return err;

    /* What the link actually runs at; the ring and the mixer follow. */
    if (snd_pcm_hw_params_current(pcm, hw) >= 0)
        snd_pcm_hw_params_get_rate(hw, rate_out, NULL);
    else
        *rate_out = rate;
    return 0;
}

/* Drain the A2DP capture stream into the ring.
 *
 * This is a raw pthread, not a Rockbox thread, so it must use usleep()
 * rather than the cooperative sleep() (which only the scheduler's own
 * threads may call). */
static void *btin_pump_thread(void *arg)
{
    const unsigned int gen = btin_gen;
    snd_pcm_t *pcm = NULL;
    bool dsp_init = false;
    (void)arg;

    /* The decode work happens inside the plugin's read path on this
     * thread; keep it ahead of the UI threads so screen redraws cannot
     * starve the received stream (we run as root, so this sticks). */
    setpriority(PRIO_PROCESS, 0, -10);

    while (btin_running && gen == btin_gen)
    {
        if (!pcm)
        {
            /* Transport not up yet: (re)open the capture for the
             * current peer. Opened non-blocking so a wedged vendor
             * stack can never pin this thread. */
            snd_config_t *cfg = NULL;
            int err = btin_open_capture(&pcm, &cfg);

            if (err < 0)
            {
                pcm = NULL;
                btin_link_ok = false;
                if (!btin_fg)
                {
                    /* Screen closed and no link: nobody is waiting on
                     * a reconnect, so wind down instead of spinning. */
                    bt_input_stop();
                    break;
                }
                usleep(500000);
                continue;
            }
            btin_cfg = cfg;

            unsigned int rate = 0;

            err = btin_setup_stream(pcm, &rate);

            if (err >= 0)
                err = snd_pcm_prepare(pcm);
            if (err < 0)
            {
                btin_close_capture(&pcm);
                btin_link_ok = false;
                if (!btin_fg)
                {
                    bt_input_stop();
                    break;
                }
                usleep(500000);
                continue;
            }

            /* Prepare the shared audio DSP chain on this thread, the
             * same way usb-dac-hiby.c does for USB DAC input. */
            if (!dsp_init)
            {
                btin_dsp = dsp_get_config(CODEC_IDX_AUDIO);
                dsp_configure(btin_dsp, DSP_RESET, 0);
                dsp_configure(btin_dsp, DSP_SET_STEREO_MODE, STEREO_INTERLEAVED);
                dsp_configure(btin_dsp, DSP_SET_SAMPLE_DEPTH, 16);
                dsp_init = true;
            }

            btin_head = btin_tail;   /* ring: drop stale audio */
            btin_apply_rate(rate > 0 ? (int)rate : BTIN_RATE);
            btin_ever_connected = true;
            btin_link_ok = true;

            /* Blocking reads from here on: the thread wakes exactly
             * when the plugin has data, instead of polling. The open
             * itself stays non-blocking (fails fast while the peer
             * has no transport up). */
            snd_pcm_nonblock(pcm, 0);
        }

        int16_t rbuf[BTIN_CHUNK_FRAMES * 2];
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, rbuf, BTIN_CHUNK_FRAMES);

        if (n < 0)
        {
            if (n == -EAGAIN)
            {
                /* No data right now (phone paused): the transport is
                 * still up, just quiet. */
                usleep(5000);
                continue;
            }
            /* Hard error: the transport dropped (peer left). Close and
             * let the retry loop wait for the peer to return. */
            btin_link_ok = false;
            btin_close_capture(&pcm);
            if (!btin_fg)
            {
                /* Running in the background: stop instead of
                 * retrying; re-entering the screen restarts us. */
                bt_input_stop();
                break;
            }
            usleep(500000);
            continue;
        }

        int frames = (int)n;
        unsigned int head = btin_head;

        /* Keep the capture alive when the ring is full: drop the
         * oldest chunk so backpressure never stalls the plugin. */
        if (head - btin_tail > BTIN_RING_FRAMES - BTIN_CHUNK_FRAMES)
            btin_tail += BTIN_CHUNK_FRAMES;

        /* Run the received PCM through the Rockbox DSP chain so the EQ
         * and the other Sound settings apply. Bypassed while local
         * playback runs: the codec thread owns the shared
         * CODEC_IDX_AUDIO instance then. */
        struct dsp_config *dsp = btin_dsp;
        if (dsp && mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_STOPPED)
        {
            static int16_t dbuf[BTIN_CHUNK_FRAMES * 2];
            struct dsp_buffer src, dst;

            src.remcount = frames;
            src.pin[0] = rbuf;
            src.pin[1] = rbuf;
            src.proc_mask = 0;
            dst.remcount = 0;
            dst.bufcount = BTIN_CHUNK_FRAMES;
            dst.p16out = dbuf;
            dsp_process(dsp, &src, &dst, false);

            frames = dst.remcount;
            for (int i = 0; i < frames; i++)
            {
                unsigned int idx = (head + i) & (BTIN_RING_FRAMES - 1);
                btin_ring[2 * idx]     = dbuf[2 * i];
                btin_ring[2 * idx + 1] = dbuf[2 * i + 1];
            }
        }
        else
        {
            for (int i = 0; i < frames; i++)
            {
                unsigned int idx = (head + i) & (BTIN_RING_FRAMES - 1);
                btin_ring[2 * idx]     = rbuf[2 * i];
                btin_ring[2 * idx + 1] = rbuf[2 * i + 1];
            }
        }
        btin_head = head + frames;
    }

    if (pcm)
        btin_close_capture(&pcm);
    return NULL;
}

bool bt_input_start(void)
{
    if (btin_running)
        return true;

    /* While local playback runs the output device and its rate belong
     * to it (and the core refuses to start it while we run, see
     * apps/playback.c). */
    if (mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_STOPPED)
        return false;

    /* The earphone output route owns the bluetooth PCM; refuse to start
     * receiving while it is up (the two are mutually exclusive). */
    if (pcm_alsa_is_bluetooth_active())
        return false;

    mixer_set_frequency(BTIN_RATE);

    btin_head = btin_tail = 0;
    btin_rate = 0;
    btin_dsp = NULL;
    btin_ever_connected = false;
    btin_peer_mac[0] = '\0';
    btin_link_ok = false;
    btin_gen++;
    btin_running = true;
    if (pthread_create(&btin_thread, NULL, btin_pump_thread, NULL) != 0)
    {
        logf("btin pump thread failed");
        btin_running = false;
        return false;
    }
    /* Never joined: a pump blocked inside the plugin must not block us. */
    pthread_detach(btin_thread);

    /* Force the wired output wide open for as long as we run: the pin
     * lives in audiohw_set_volume (not in the global setting), so any
     * unclean end still leaves the user's own volume intact. */
    sound_set_volume(global_status.volume);

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BT_AUDIO, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_BT_AUDIO, &btin_cbs, NULL, 0);
    return true;
}

void bt_input_stop(void)
{
    if (!btin_running)
        return;

    /* The pump is detached (it may be inside the plugin): mark it stale
     * and let it wind down on its own; joining here could deadlock. */
    btin_running = false;
    btin_gen++;
    btin_dsp = NULL;
    mixer_channel_stop(PCM_MIXER_CHAN_BT_AUDIO);

    /* Re-apply the user's own volume now that the pinned branch in
     * audiohw_set_volume no longer intercepts. */
    sound_set_volume(global_status.volume);
}

bool bt_input_active(void)
{
    return btin_running;
}

void bt_input_set_fg(bool fg)
{
    btin_fg = fg;
}

bool bt_input_link_ok(void)
{
    return btin_link_ok;
}

enum bt_input_state bt_input_get_state(void)
{
    if (!btin_running)
        return BT_INPUT_IDLE;
    if (btin_link_ok)
        return BT_INPUT_CONNECTED;
    return btin_ever_connected ? BT_INPUT_DISCONNECTED
                               : BT_INPUT_WAITING;
}

void bt_input_get_peer(char *buf, size_t len)
{
    if (!buf || !len)
        return;
    snprintf(buf, len, "%s", btin_peer_mac);
}

int bt_input_get_rate(void)
{
    return btin_rate;
}

int bt_input_get_fill_ms(void)
{
    int rate = btin_rate;

    if (rate <= 0)
        return 0;

    unsigned int fill = btin_head - btin_tail;
    if (fill > BTIN_RING_FRAMES)
        fill = BTIN_RING_FRAMES;

    return (int)((unsigned long long)fill * 1000 / (unsigned int)rate);
}
