/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2010 Thomas Martitz
 * Copyright (c) 2020 Solomon Peachy
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

/*
 * Based, but heavily modified, on the example given at
 * http://www.alsa-project.org/alsa-doc/alsa-lib/_2test_2pcm_8c-example.html
 *
 * This driver uses the so-called unsafe async callback method.
 *
 * To make the async callback safer, an alternative stack is installed, since
 * it's run from a signal hanlder (which otherwise uses the user stack).
 *
 * TODO: Rewrite this to properly use multithreading and/or direct mmap()
 */

#include "autoconf.h"

#include <stdlib.h>
#include <stdbool.h>
#include <alsa/asoundlib.h>

//#define LOGF_ENABLE

#include "system.h"
#include "debug.h"
#include "kernel.h"
#include "panic.h"

#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "pcm_sink.h"
#include "audiohw.h"
#include "pcm-alsa.h"
#include "fixedpoint.h"

#include "logf.h"

#include <pthread.h>
#include <signal.h>

/* plughw:0,0 works with both, however "default" is recommended.
 * default doesnt seem to work with async callback but doesn't break
 * with multple applications running */
#define DEFAULT_PLAYBACK_DEVICE "plughw:0,0"
#define DEFAULT_CAPTURE_DEVICE "default"

#if MIX_FRAME_SAMPLES < 512
#error "MIX_FRAME_SAMPLES needs to be at least 512!"
#elif MIX_FRAME_SAMPLES < 1024
#warning "MIX_FRAME_SAMPLES <1024 may cause dropouts!"
#endif

/* PCM_DC_OFFSET_VALUE is a workaround for eros q hardware quirk */
#if !defined(PCM_DC_OFFSET_VALUE)
# define PCM_DC_OFFSET_VALUE 0
#endif

static const snd_pcm_access_t access_ = SND_PCM_ACCESS_RW_INTERLEAVED; /* access mode */
#if defined(HAVE_ALSA_32BIT)
static const snd_pcm_format_t format = SND_PCM_FORMAT_S32_LE;    /* sample format */
typedef int32_t sample_t;
#else
static const snd_pcm_format_t format = SND_PCM_FORMAT_S16;    /* sample format */
typedef int16_t sample_t;
#endif
static const int channels = 2;                                /* count of channels */
static unsigned int real_sample_rate;
static unsigned int last_sample_rate;

static snd_pcm_t *handle = NULL;
static snd_pcm_sframes_t buffer_size;
static snd_pcm_sframes_t period_size;
static sample_t *frames = NULL;

static const void  *pcm_data = 0;
static size_t       pcm_size = 0;

static unsigned int xruns = 0;

static snd_async_handler_t *ahandler = NULL;
static pthread_mutex_t pcm_mtx;
static long signal_stack[SIGSTKSZ/sizeof(long)];

static const char *playback_dev = DEFAULT_PLAYBACK_DEVICE;

#ifdef HAVE_RECORDING
static void *pcm_data_rec = DEFAULT_CAPTURE_DEVICE;
static const char *capture_dev = NULL;
static snd_pcm_stream_t current_alsa_mode;  /* SND_PCM_STREAM_PLAYBACK / _CAPTURE */
#endif

static const char *current_alsa_device;

#if defined(CAYIN_N3PRO)
/* Dynamic output routing + the poll pump for the stock bluetooth PCM
 * (see cayin/n3pro/n3pro-bt-pcm-hooks.h). */
#include "cayin/n3pro/n3pro-bt-pcm-hooks.h"
#endif

void pcm_alsa_set_playback_device(const char *device)
{
    playback_dev = device;
}

#if defined(CAYIN_N3PRO)
/* The stock bluetooth PCM is configured when it is opened and the vendor
 * plugin rejects later reconfiguration, so report its real rate: generic
 * rate changes are then clamped to it and the DSP resamples the stream. */
unsigned int pcm_sink_fixed_rate(void)
{
    if (real_sample_rate && pcm_alsa_is_bluetooth_active())
        return real_sample_rate;
    return 0;
}
#endif

#ifdef HAVE_RECORDING
void pcm_alsa_set_capture_device(const char *device)
{
    capture_dev = device;
}
#endif

static int set_hwparams(snd_pcm_t *handle, unsigned long sampr)
{
    int err;
    unsigned int srate;
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_malloc(&params);

    /* Size playback buffers based on sample rate.

       Buffer size must be at least 4x period size!

       Note these are in FRAMES, and are sized to be about 8.5ms
       for the buffer and 2.1ms for the period
     */
#ifdef CAYIN_N3PRO
    /* The X1000 smart-LCD update path can stall the kernel (and thus
     * any thread) for tens of milliseconds when the screen wakes or
     * redraws. The stock ~10ms hardware buffer underruns audibly on
     * every such event, so keep ~200ms of audio in the hardware and
     * let the poll thread catch up once the stall is over. */
    {
        unsigned long want = sampr / 5;          /* frames in 200ms */
        buffer_size = (want + MIX_FRAME_SAMPLES - 1) & ~(unsigned long)(MIX_FRAME_SAMPLES - 1);
        if (buffer_size < MIX_FRAME_SAMPLES * 4)
            buffer_size = MIX_FRAME_SAMPLES * 4;
        period_size = buffer_size / 8;
    }
#else
    if (sampr > SAMPR_96) {
        buffer_size = MIX_FRAME_SAMPLES * 4 * 4;
        period_size = MIX_FRAME_SAMPLES * 4;
    } else if (sampr > SAMPR_48) {
        buffer_size = MIX_FRAME_SAMPLES * 2 * 4;
        period_size = MIX_FRAME_SAMPLES * 2;
    } else {
        buffer_size = MIX_FRAME_SAMPLES * 4;
        period_size = MIX_FRAME_SAMPLES;
    }
#endif

    /* choose all parameters */
    err = snd_pcm_hw_params_any(handle, params);
    if (err < 0)
    {
#if defined(CAYIN_N3PRO)
        /* The bluetooth endpoint keeps the A2DP transport in flux (the
         * earpiece may have just re-connected); a rejected configuration
         * must not panic - fail softly so the router can fall back. */
        logf("Broken configuration for playback: no configurations available: %s", snd_strerror(err));
#else
        panicf("Broken configuration for playback: no configurations available: %s", snd_strerror(err));
#endif
        goto error;
    }
    /* set the interleaved read/write format */
    err = snd_pcm_hw_params_set_access(handle, params, access_);
    if (err < 0)
    {
#if defined(CAYIN_N3PRO)
        logf("Access type not available for playback: %s", snd_strerror(err));
#else
        panicf("Access type not available for playback: %s", snd_strerror(err));
#endif
        goto error;
    }
    /* set the sample format */
    err = snd_pcm_hw_params_set_format(handle, params, format);
    if (err < 0)
    {
        logf("Sample format not available for playback: %s", snd_strerror(err));
        goto error;
    }
    /* set the count of channels */
    err = snd_pcm_hw_params_set_channels(handle, params, channels);
    if (err < 0)
    {
        logf("Channels count (%i) not available for playbacks: %s", channels, snd_strerror(err));
        goto error;
    }
    /* set the stream rate */
    srate = sampr;
    err = snd_pcm_hw_params_set_rate_near(handle, params, &srate, 0);
    if (err < 0)
    {
        logf("Rate %luHz not available for playback: %s", sampr, snd_strerror(err));
        goto error;
    }
    real_sample_rate = srate;
    if (real_sample_rate != sampr)
    {
        logf("Rate doesn't match (requested %luHz, get %dHz)", sampr, real_sample_rate);
        err = -EINVAL;
        goto error;
    }

    /* set the buffer size */
    err = snd_pcm_hw_params_set_buffer_size_near(handle, params, &buffer_size);
    if (err < 0)
    {
        logf("Unable to set buffer size %ld for playback: %s", buffer_size, snd_strerror(err));
        goto error;
    }

    /* set the period size */
    err = snd_pcm_hw_params_set_period_size_near (handle, params, &period_size, NULL);
    if (err < 0)
    {
        logf("Unable to set period size %ld for playback: %s", period_size, snd_strerror(err));
        goto error;
    }

    if (frames) free(frames);
    frames = calloc(1, period_size * channels * sizeof(sample_t));

    /* write the parameters to device */
    err = snd_pcm_hw_params(handle, params);
    if (err < 0)
    {
        logf("Unable to set hw params for playback: %s", snd_strerror(err));
        goto error;
    }

    err = 0; /* success */
error:
    snd_pcm_hw_params_free(params);
    return err;
}

/* Set sw params: playback start threshold and low buffer watermark */
static int set_swparams(snd_pcm_t *handle)
{
    int err;

    snd_pcm_sw_params_t *swparams;
    snd_pcm_sw_params_malloc(&swparams);

    /* get the current swparams */
    err = snd_pcm_sw_params_current(handle, swparams);
    if (err < 0)
    {
        logf("Unable to determine current swparams for playback: %s", snd_strerror(err));
        goto error;
    }
    /* start the transfer when the buffer is half full */
    err = snd_pcm_sw_params_set_start_threshold(handle, swparams, buffer_size / 2);
    if (err < 0)
    {
        logf("Unable to set start threshold mode for playback: %s", snd_strerror(err));
        goto error;
    }
    /* allow the transfer when at least period_size samples can be processed */
    err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_size);
    if (err < 0)
    {
        logf("Unable to set avail min for playback: %s", snd_strerror(err));
        goto error;
    }
    /* write the parameters to the playback device */
    err = snd_pcm_sw_params(handle, swparams);
    if (err < 0)
    {
        logf("Unable to set sw params for playback: %s", snd_strerror(err));
        goto error;
    }

    err = 0; /* success */
error:
    snd_pcm_sw_params_free(swparams);
    return err;
}

#if defined(HAVE_ALSA_32BIT)
/* Multiplicative factors applied to each sample */
static int32_t dig_vol_mult_l = 0;
static int32_t dig_vol_mult_r = 0;

void pcm_set_mixer_volume(int vol_db_l, int vol_db_r)
{
    dig_vol_mult_l = fp_factor(fp_div(vol_db_l, 10, 16), 16);
    dig_vol_mult_r = fp_factor(fp_div(vol_db_r, 10, 16), 16);
}
#endif

/* copy pcm samples to a spare buffer, suitable for snd_pcm_writei() */
static bool copy_frames(bool first)
{
    ssize_t nframes, frames_left = period_size;
    bool new_buffer = false;

    while (frames_left > 0)
    {
        if (!pcm_size)
        {
            new_buffer = true;
#ifdef HAVE_RECORDING
            switch (current_alsa_mode)
            {
            case SND_PCM_STREAM_PLAYBACK:
#endif
                if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &pcm_data, &pcm_size))
                {
                    return false;
                }
#ifdef HAVE_RECORDING
                break;
            case SND_PCM_STREAM_CAPTURE:
                if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &pcm_data, &pcm_size))
                {
                    return false;
                }
                break;
            default:
                break;
            }
#endif
        }

        /* Note:  This assumes stereo 16-bit */
        if (pcm_size % 4)
            panicf("Wrong pcm_size");
        /* the compiler will optimize this test away */
        nframes = MIN((ssize_t)pcm_size/4, frames_left);

#ifdef HAVE_RECORDING
        switch (current_alsa_mode)
        {
        case SND_PCM_STREAM_PLAYBACK:
#endif
#if defined(HAVE_ALSA_32BIT)
            if (format == SND_PCM_FORMAT_S32_LE)
            {
                /* We have to convert 16-bit to 32-bit, the need to multiply the
                 * sample by some value so the sound is not too low */
                const int16_t *pcm_ptr = pcm_data;
                sample_t *sample_ptr = &frames[2*(period_size-frames_left)];
                for (int i = 0; i < nframes; i++)
                {
                    *sample_ptr++ = (*pcm_ptr++ * dig_vol_mult_l) + PCM_DC_OFFSET_VALUE;
                    *sample_ptr++ = (*pcm_ptr++ * dig_vol_mult_r) + PCM_DC_OFFSET_VALUE;
                }
            }
            else
#endif
            {
                /* Rockbox and PCM have same format: memcopy */
                memcpy(&frames[2*(period_size-frames_left)], pcm_data, nframes * 4);
	    }
#ifdef HAVE_RECORDING
            break;
        case SND_PCM_STREAM_CAPTURE:
            memcpy(pcm_data_rec, &frames[2*(period_size-frames_left)], nframes * 4);
            break;
        default:
            break;
        }
#endif
        pcm_data += nframes*4;
        pcm_size -= nframes*4;
        frames_left -= nframes;

        if (new_buffer && !first)
        {
            new_buffer = false;
#ifdef HAVE_RECORDING
            switch (current_alsa_mode)
            {
            case SND_PCM_STREAM_PLAYBACK:
#endif
                pcm_play_dma_status_callback(PCM_DMAST_STARTED);
#ifdef HAVE_RECORDING
                break;
            case SND_PCM_STREAM_CAPTURE:
                pcm_rec_dma_status_callback(PCM_DMAST_STARTED);
                break;
            default:
                break;
            }
#endif
        }
    }

    return true;
}

#if defined(CAYIN_N3PRO)
/* Data pump, called with pcm_mtx held.  A single poll thread drives both
 * outputs: the stock bluetooth plugin has no SIGIO support, and running an
 * async handler alongside the poll thread made the kernel PCM wake a
 * thread twice (wait_queue_remove() on an already-removed wait queue). */
static void pcm_pump_locked(snd_pcm_t *h)
{
    int err;
    snd_pcm_state_t state = snd_pcm_state(h);
    bool wrote = false;

    if (state == SND_PCM_STATE_XRUN)
    {
        xruns++;
        logf("initial underrun!");
        err = snd_pcm_recover(h, -EPIPE, 0);
        if (err < 0) {
            logf("XRUN Recovery error: %s", snd_strerror(err));
            return;
        }
    }
    else if (state == SND_PCM_STATE_DRAINING)
    {
        logf("draining...");
        return;
    }
    else if (state == SND_PCM_STATE_SETUP)
    {
        return;
    }
    else if (state == SND_PCM_STATE_DISCONNECTED)
    {
#if defined(CAYIN_N3PRO)
        if (n3pro_pcm_is_bt_device(current_alsa_device))
            n3pro_bt_mark_link_lost("pcm disconnected");
#endif
        return;
    }

#ifdef HAVE_RECORDING
    if (current_alsa_mode == SND_PCM_STREAM_PLAYBACK)
    {
#endif
        while (snd_pcm_avail_update(h) >= period_size)
        {
            if (copy_frames(false))
            {
            retry:
                err = snd_pcm_writei(h, frames, period_size);
                if (err == -EPIPE)
                {
                    logf("mid underrun!");
                    xruns++;
                    err = snd_pcm_recover(h, -EPIPE, 0);
                    if (err < 0) {
                       logf("XRUN Recovery error: %s", snd_strerror(err));
                       return;
                    }
                    goto retry;
                }
                else if (err != period_size)
                {
                    logf("Write error: written %i expected %li", err, period_size);
                    break;
                }
                wrote = true;
            }
            else
            {
                logf("%s: No Data (%d).", __func__, state);
                break;
            }
        }
#ifdef HAVE_RECORDING
    }
    else if (current_alsa_mode == SND_PCM_STREAM_CAPTURE)
    {
        while (snd_pcm_avail_update(h) >= period_size)
        {
            int rerr = snd_pcm_readi(h, frames, period_size);
            if (rerr == -EPIPE)
            {
                logf("rec mid underrun!");
                xruns++;
                rerr = snd_pcm_recover(h, -EPIPE, 0);
                if (rerr < 0) {
                   logf("XRUN Recovery error: %s", snd_strerror(rerr));
                   return;
                }
                continue;  /* buffer contents trashed, no sense in trying to copy */
            }
            else if (rerr != period_size)
            {
                logf("Read error: read %i expected %li", rerr, period_size);
                break;
            }

            /* start the fake DMA transfer */
            if (!copy_frames(false))
                break;
        }
    }
#endif

    /* Only start a stream this round actually fed: starting an empty
     * PREPARED PCM underruns at once, and the poll thread would then
     * recover/start it forever (an ioctl storm that starves the CPU). */
    if (wrote && snd_pcm_state(h) == SND_PCM_STATE_PREPARED)
    {
        err = snd_pcm_start(h);
        if (err < 0)
            logf("cb start error: %s", snd_strerror(err));
    }
}
#else /* !CAYIN_N3PRO */
static void async_callback(snd_async_handler_t *ahandler)
{
    int err;

    if (!ahandler) return;

    snd_pcm_t *handle = snd_async_handler_get_pcm(ahandler);

    if (!handle) return;

    if (pthread_mutex_trylock(&pcm_mtx) != 0)
        return;

    snd_pcm_state_t state = snd_pcm_state(handle);

    if (state == SND_PCM_STATE_XRUN)
    {
        xruns++;
        logf("initial underrun!");
        err = snd_pcm_recover(handle, -EPIPE, 0);
        if (err < 0) {
            logf("XRUN Recovery error: %s", snd_strerror(err));
            goto abort;
        }
    }
    else if (state == SND_PCM_STATE_DRAINING)
    {
        logf("draining...");
        goto abort;
    }
    else if (state == SND_PCM_STATE_SETUP)
    {
        goto abort;
    }

#ifdef HAVE_RECORDING
    if (current_alsa_mode == SND_PCM_STREAM_PLAYBACK)
    {
#endif
        while (snd_pcm_avail_update(handle) >= period_size)
        {
            if (copy_frames(false))
            {
            retry:
                err = snd_pcm_writei(handle, frames, period_size);
                if (err == -EPIPE)
                {
                    logf("mid underrun!");
                    xruns++;
                    err = snd_pcm_recover(handle, -EPIPE, 0);
                    if (err < 0) {
                       logf("XRUN Recovery error: %s", snd_strerror(err));
                       goto abort;
                    }
                    goto retry;
                }
                else if (err != period_size)
                {
                    logf("Write error: written %i expected %li", err, period_size);
                    break;
                }
            }
            else
            {
                logf("%s: No Data (%d).", __func__, state);
                break;
            }
        }
#ifdef HAVE_RECORDING
    }
    else if (current_alsa_mode == SND_PCM_STREAM_CAPTURE)
    {
        while (snd_pcm_avail_update(handle) >= period_size)
        {
            int err = snd_pcm_readi(handle, frames, period_size);
            if (err == -EPIPE)
            {
                logf("rec mid underrun!");
                xruns++;
                err = snd_pcm_recover(handle, -EPIPE, 0);
                if (err < 0) {
                   logf("XRUN Recovery error: %s", snd_strerror(err));
                   goto abort;
                }
		continue;  /* buffer contents trashed, no sense in trying to copy */
            }
                else if (err != period_size)
                {
                    logf("Write error: written %i expected %li", err, period_size);
#if defined(CAYIN_N3PRO)
                    if (n3pro_pcm_is_bt_device(current_alsa_device))
                        n3pro_bt_mark_link_lost("write error");
#endif
                    break;
                }

            /* start the fake DMA transfer */
            if (!copy_frames(false))
            {
                /* do not spam logf */
                /* logf("%s: No Data.", __func__); */
                break;
            }
        }
    }
#endif

    if (snd_pcm_state(handle) == SND_PCM_STATE_PREPARED)
    {
        err = snd_pcm_start(handle);
        if (err < 0) {
            logf("cb start error: %s", snd_strerror(err));
            /* Depending on the error we might be SOL */
        }
    }

abort:
    pthread_mutex_unlock(&pcm_mtx);
}
#endif /* CAYIN_N3PRO */

static void close_hwdev(void)
{
    logf("closedev (%p)", handle);

#if defined(CAYIN_N3PRO)
    /* The poll thread pumps the handle without the lock, so make sure it
     * is not running before the handle is freed. */
    n3pro_pcm_stop_poll();
#endif

    if (handle) {
        snd_pcm_drain(handle);
#ifdef AUDIOHW_MUTE_ON_STOP
        audiohw_mute(true);
#endif
        if (ahandler) {
            snd_async_del_handler(ahandler);
            ahandler = NULL;
        }
        snd_pcm_close(handle);

        handle = NULL;
    }
    current_alsa_device = NULL;

#ifdef HAVE_RECORDING
    pcm_data_rec = NULL;
#endif
}

static void alsadev_cleanup(void)
{
    free(frames);
    frames = NULL;
    close_hwdev();
}

static void open_hwdev(const char *device, snd_pcm_stream_t mode)
{
    int err;

    logf("opendev %s (%p)", device, handle);

#if defined(CAYIN_N3PRO)
    if (n3pro_pcm_keep_hwdev(device, mode))
        return;
#else
    if (handle && device == current_alsa_device
#ifdef HAVE_RECORDING
        && current_alsa_mode == mode
#endif
        )
    {
        return;
    }
#endif

    /* Close old handle */
    close_hwdev();

#if defined(CAYIN_N3PRO)
    if (n3pro_pcm_is_bt_device(device))
    {
        /* Stock bluetooth plugin: opened against a freshly parsed config
         * tree so the peer the vendor player wrote into /etc/asound.conf
         * is picked up; a failure is non-fatal (the caller retries). */
        if ((err = n3pro_bt_open(&handle, mode)) < 0)
        {
            logf("%s(): Cannot open bluetooth PCM: %s", __func__, snd_strerror(err));
            handle = NULL;
            return;
        }
    }
    else
#endif
    if ((err = snd_pcm_open(&handle, device, mode, 0)) < 0)
    {
        panicf("%s(): Cannot open device %s: %s", __func__, device, snd_strerror(err));
    }
    last_sample_rate = 0;

#if defined(CAYIN_N3PRO)
    /* The poll thread is the only pump for this target (the stock bluetooth
     * plugin has no SIGIO support, and driving the wired PCM from both a
     * SIGIO handler and the poll thread races the kernel wait queues). */
    n3pro_pcm_after_open();
#else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&pcm_mtx, &attr);

    /* assign alternative stack for the signal handlers */
    stack_t ss = {
        .ss_sp = signal_stack,
        .ss_size = sizeof(signal_stack),
        .ss_flags = 0
    };
    struct sigaction sa;

    err = sigaltstack(&ss, NULL);
    if (err < 0)
    {
        panicf("Unable to install alternative signal stack: %s", strerror(err));
    }

    err = snd_async_add_pcm_handler(&ahandler, handle, async_callback, NULL);
    if (err < 0)
    {
        panicf("Unable to register async handler: %s", snd_strerror(err));
    }

    /* only modify the stack the handler runs on */
    sigaction(SIGIO, NULL, &sa);
    sa.sa_flags |= SA_ONSTACK;
    err = sigaction(SIGIO, &sa, NULL);
    if (err < 0)
    {
        panicf("Unable to install alternative signal stack: %s", strerror(err));
    }
#endif

#ifdef HAVE_RECORDING
    current_alsa_mode = mode;
#else
    (void)mode;
#endif
    current_alsa_device = device;

    atexit(alsadev_cleanup);
}

static void sink_dma_init(void)
{
    logf("PCM DMA Init");

    audiohw_preinit();

    open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);

    return;
}

static void sink_lock(void)
{
    pthread_mutex_lock(&pcm_mtx);
}

static void sink_unlock(void)
{
    pthread_mutex_unlock(&pcm_mtx);
}

static void sink_set_freq_nolock(uint16_t freq)
{
    unsigned int sampr = hw_freq_sampr[freq];

    logf("PCM DMA Settings %d %lu", last_sample_rate, sampr);

    if (last_sample_rate != sampr)
    {
        last_sample_rate = sampr;

#ifdef AUDIOHW_MUTE_ON_SRATE_CHANGE
        audiohw_mute(true);
#endif
        snd_pcm_drop(handle);

        set_hwparams(handle, sampr); // FIXME: check return code?
        set_swparams(handle); // FIXME: check return code?

#if defined(HAVE_NWZ_LINUX_CODEC)
        /* Sony NWZ linux driver uses a nonstandard mecanism to set the sampling rate */
        audiohw_set_frequency(sampr);
#endif
        /* (Will be unmuted by pcm resuming) */
    }
}

static void sink_set_freq(uint16_t freq)
{
    sink_lock();
    sink_set_freq_nolock(freq);
    sink_unlock();
}

static void sink_dma_stop(void)
{
    logf("PCM DMA stop (%d)", snd_pcm_state(handle));

    int err = snd_pcm_drain(handle);
    if (err < 0)
        if (err < 0)
            logf("Drain failed: %s", snd_strerror(err));
#ifdef AUDIOHW_MUTE_ON_STOP
    audiohw_mute(true);
#endif
}

static void sink_dma_start(const void *addr, size_t size)
{
    logf("PCM DMA start (%p %d)", addr, size);

    pcm_data = addr;
    pcm_size = size;

#if !defined(AUDIOHW_MUTE_ON_STOP) && defined(AUDIOHW_MUTE_ON_SRATE_CHANGE)
    audiohw_mute(false);
#endif

#if defined(CAYIN_N3PRO)
    /* The dedicated poll thread (n3pro_pcm_after_open) pumps the PCM and
     * runs the ALSA state machine for this target.  Waiting for
     * SND_PCM_STATE_RUNNING on the caller would busy-spin a raw pthread
     * forever when the stock bluetooth plugin stays in PREPARED/DRAINING
     * (e.g. after a failed BT_OPEN), freezing the whole engine.  A drained
     * PCM is left in SETUP, which the pump skips, so re-prepare it here. */
    if (handle && snd_pcm_state(handle) == SND_PCM_STATE_SETUP)
        snd_pcm_prepare(handle);
    return;
#endif

    while (1)
    {
        snd_pcm_state_t state = snd_pcm_state(handle);
        logf("PCM State %d", state);

        switch (state)
        {
            case SND_PCM_STATE_RUNNING:
#if defined(AUDIOHW_MUTE_ON_STOP)
                audiohw_mute(false);
#endif
                return;
            case SND_PCM_STATE_XRUN:
            {
                logf("Trying to recover from underrun");
                int err = snd_pcm_recover(handle, -EPIPE, 0);
                if (err < 0)
                    logf("Recovery failed: %s", snd_strerror(err));
                continue;
            }
            case SND_PCM_STATE_SETUP:
            {
                int err = snd_pcm_prepare(handle);
                if (err < 0)
                    logf("Prepare error: %s", snd_strerror(err));
            }
                /* fall through */
            case SND_PCM_STATE_PREPARED:
            {
                int err;
#if 0
                /* fill buffer with silence to initiate playback without noisy click */
                snd_pcm_sframes_t sample_size = buffer_size;
                sample_t *samples = calloc(1, sample_size * channels * sizeof(sample_t));

                snd_pcm_format_set_silence(format, samples, sample_size);
                err = snd_pcm_writei(handle, samples, sample_size);
                free(samples);

                if (err != (ssize_t)sample_size)
                {
                    logf("Initial write error: written %i expected %li", err, sample_size);
                    return;
                }
#else
                /* Fill buffer with proper sample data */
                while (snd_pcm_avail_update(handle) >= period_size)
                {
                    if (copy_frames(true))
                    {
                        err = snd_pcm_writei(handle, frames, period_size);
                        if (err < 0 && err != period_size && err != -EAGAIN)
                        {
                            logf("Write error: written %i expected %li", err, period_size);
                            break;
                        }
                    }
                }
#endif
                err = snd_pcm_start(handle);
                if (err < 0) {
                    logf("start error: %s", snd_strerror(err));
                    /* We will recover on the next iteration */
                }

                break;
            }
            case SND_PCM_STATE_DRAINING:
                /* run until drained */
                continue;
            default:
                logf("Unhandled state: %s", snd_pcm_state_name(state));
                return;
        }
    }
}

static void sink_dma_postinit(void)
{
    audiohw_postinit();

#ifdef AUDIOHW_NEEDS_INITIAL_UNMUTE
    audiohw_mute(false);
#endif
}

unsigned int pcm_alsa_get_rate(void)
{
    return real_sample_rate;
}

unsigned int pcm_alsa_get_xruns(void)
{
    return xruns;
}

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
        .volume_type  = PCM_NATIVE_VOLUME_TYPE,
    },
    .ops = {
        .init     = sink_dma_init,
        .postinit = sink_dma_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_dma_start,
        .stop     = sink_dma_stop,
    },
};

#ifdef HAVE_RECORDING
void pcm_rec_lock(void)
{
    sink_lock();
}

void pcm_rec_unlock(void)
{
    sink_unlock();
}

void pcm_rec_dma_init(void)
{
    logf("PCM REC DMA Init");

    open_hwdev(capture_dev, SND_PCM_STREAM_CAPTURE);
}

void pcm_rec_dma_close(void)
{
    logf("Rec DMA Close");
    // close_hwdev();
    open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);
}

void pcm_rec_dma_start(void *start, size_t size)
{
    logf("PCM REC DMA start (%p %d)", start, size);
    pcm_data_rec = start;
    pcm_size = size;

    if (!handle) return;

    while (1)
    {
        snd_pcm_state_t state = snd_pcm_state(handle);

        switch (state)
        {
            case SND_PCM_STATE_RUNNING:
                return;
            case SND_PCM_STATE_XRUN:
            {
                logf("Trying to recover from error");
                int err = snd_pcm_recover(handle, -EPIPE, 0);
                if (err < 0)
                    panicf("Recovery failed: %s", snd_strerror(err));
                continue;
            }
            case SND_PCM_STATE_SETUP:
            {
                int err = snd_pcm_prepare(handle);
                if (err < 0)
                    panicf("Prepare error: %s", snd_strerror(err));
            }
                /* fall through */
            case SND_PCM_STATE_PREPARED:
            {
                int err = snd_pcm_start(handle);
                if (err < 0)
                    panicf("Start error: %s", snd_strerror(err));
                return;
            }
            case SND_PCM_STATE_DRAINING:
                /* run until drained */
                continue;
            default:
                logf("Unhandled state: %s", snd_pcm_state_name(state));
                return;
        }
    }
}

void pcm_rec_dma_stop(void)
{
    logf("Rec DMA Stop");
    close_hwdev();
}

const void * pcm_rec_dma_get_peak_buffer(void)
{
    uintptr_t addr = (uintptr_t)pcm_data_rec;
    return (void*)((addr + 3) & ~3);
}

#ifdef SIMULATOR
void audiohw_set_recvol(int left, int right, int type)
{
    (void)left;
    (void)right;
    (void)type;
}
#endif

#endif /* HAVE_RECORDING */
