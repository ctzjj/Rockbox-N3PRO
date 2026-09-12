/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by Michael McAllister
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

/* Host-PCM pump for the HiBy USB DAC gadget mode.
 *
 * The vendor kernel's UAC gadget function "uac_sa" exposes the char device
 * /dev/uac_sa, which delivers the host's PCM: the isochronous OUT frames are
 * converted to left-justified S16-in-S32 stereo and queued in a kernel ring,
 * drained with read(). read() is non-blocking and returns -1 until at least
 * the requested count (which must be <= the kernel ring/2) is buffered.
 *
 * A pump thread drains it into a small single-producer/single-consumer
 * ring, the mixer's get_more callback hands that PCM to the CS43131 through
 * the normal ALSA output path, the same way firmware/usbstack/usb_audio.c
 * drives its host-PCM playback.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "config.h"
#include "kernel.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "system.h"
#include "thread.h"
#include "usb.h"
#include "usb-dac-hiby.h"
#if defined(CAYIN_N3PRO)
#include <stdlib.h>
#include <time.h>
#include "dsp_core.h"
#endif

/*#define LOGF_ENABLE*/
#include "logf.h"

#define UAC_SA_DEV        "/dev/uac_sa"
/* ioctl: fills int[3] = { fmt, rate_hz, bits } */
#define UAC_SA_GET_STATUS 1
/* uac_sa delivers S32 stereo: 8 bytes per frame */
#define UAC_SA_FRAME_SIZE 8

/* Ring of S16 stereo frames. A power of two so head/tail wrap cleanly. */
#define DAC_RING_FRAMES   8192  /* ~186 ms at 44.1 kHz */
#define DAC_CHUNK_FRAMES  512   /* mixer buffer granularity */
/* one read()'s worth. Must stay <= the kernel ring/2 */
#define DAC_READ_BYTES    (DAC_CHUNK_FRAMES * UAC_SA_FRAME_SIZE)

static int16_t dac_ring[DAC_RING_FRAMES * 2];
static volatile unsigned int dac_head;  /* frames produced; pump only */
static volatile unsigned int dac_tail;  /* frames consumed; mixer only */
static pthread_t dac_thread;
static volatile bool dac_running;
#if defined(CAYIN_N3PRO)
/* The N3Pro pump may be blocked inside the vendor driver's open()/read()
 * (which wait for the host and support no poll()), so it is detached and
 * never joined: bumping the generation invalidates a stale thread. */
static volatile unsigned int dac_gen;
#endif
static int dac_fd = -1;
#if defined(CAYIN_N3PRO)
/* DSP chain the host PCM is run through (see dac_pump_thread), shared
 * with normal playback; set up in the pump thread before streaming. */
static struct dsp_config *dac_dsp;
/* Host sample rate currently applied to the mixer + DSP. */
static int dac_rate;
#endif

/* Mixer callback (PCM feed context). Returns the next chunk from the ring,
 * zero-padded with silence on underrun so the stream never stalls. Double
 * buffered so the returned pointer stays valid until the following call. */
static void dac_get_more(const void **start, size_t *size)
{
    static int16_t out[2][DAC_CHUNK_FRAMES * 2];
    static int which;
    int16_t *buf;

    which ^= 1;
    buf = out[which];

    unsigned int tail = dac_tail;
    unsigned int avail = dac_head - tail;         /* wrap-safe */
    avail = MIN(avail, DAC_CHUNK_FRAMES);

    for (unsigned int i = 0; i < avail; i++)
    {
        unsigned int idx = (tail + i) & (DAC_RING_FRAMES - 1);
        buf[2 * i]     = dac_ring[2 * idx];
        buf[2 * i + 1] = dac_ring[2 * idx + 1];
    }
    memset(&buf[2 * avail], 0,
           (DAC_CHUNK_FRAMES - avail) * 2 * sizeof(int16_t));
    dac_tail = tail + avail;

    *start = buf;
    *size = DAC_CHUNK_FRAMES * 2 * sizeof(int16_t);
}

#if defined(CAYIN_N3PRO)
/* Follow a host sample-rate change: retime the mixer + ALSA output and
 * the DSP chain, and drop audio still buffered from the old rate.
 * Called from the pump thread only. */
static void dac_apply_rate(int rate)
{
    if (rate < 8000 || rate > 768000)
        rate = USB_DAC_SAMPLE_RATE;
    if (rate == dac_rate)
        return;
    dac_rate = rate;

    /* Rockbox's output rate table tops out at 192 kHz: requesting
     * 352.8/384 kHz falls back to the 44.1 kHz default and everything
     * crawls. Halve the output rate instead (clean 2:1 ratio) and let
     * the DSP resampler downconvert the host stream. */
    int out_rate = rate;
    while (out_rate > 192000)
        out_rate /= 2;

    dac_tail = dac_head;          /* ring: drop old-rate audio */

    mixer_set_frequency(out_rate);
    if (dac_dsp)
    {
        dsp_configure(dac_dsp, DSP_SET_FREQUENCY, rate);
        dsp_configure(dac_dsp, DSP_SET_OUT_FREQUENCY, out_rate);
    }

    /* mixer_set_frequency() stops the PCM driver and nothing restarts
     * it: in the normal flow the next track's channel play_data does.
     * Re-kick our channel so the stream resumes at the new rate. */
    {
        static const struct mixer_play_cbs cbs = { .get_more = dac_get_more };
        mixer_channel_play_data(PCM_MIXER_CHAN_USBAUDIO, &cbs, NULL, 0);
    }
}

/* Infer the host sample rate from the data arrival rate. The uac_sa
 * status ioctl is unreliable on this driver family (it reports a
 * stale/default rate), so measure throughput instead. */
static int dac_infer_rate(uint64_t frames, long ns)
{
    static const int rates[] = { 44100, 48000, 88200, 96000,
                                 176400, 192000, 352800, 384000 };
    int hz = (int)(frames * 1000000000ull / (uint64_t)ns);

    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        if (hz > rates[i] * 97 / 100 && hz < rates[i] * 103 / 100)
            return rates[i];
    return 0;
}

/* Watch the kernel log for the uac_sa driver's rate announcements
 * ("... set replay rate:96000"). This is exact and event-driven; the
 * ioctl lies on this driver family and throughput guessing gets
 * imprecise at high rates (4096 frames is ~11 ms at 384 kHz). */
static int dac_kmsg_fd = -1;

/* Drain pending records; return the last "replay rate" seen, or 0. */
static int dac_kmsg_rate(int max_records)
{
    if (dac_kmsg_fd < 0)
        return 0;

    int rate = 0;
    char buf[512];

    while (max_records-- > 0)
    {
        ssize_t n = read(dac_kmsg_fd, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;                  /* EAGAIN: drained */
        buf[n] = '\0';

        char *p = strstr(buf, "replay rate");
        if (!p)
            continue;
        p += strlen("replay rate");
        while (*p != '\0' && (*p < '0' || *p > '9'))
            p++;                     /* skip ": " of both log formats */
        if (*p >= '0' && *p <= '9')
            rate = atoi(p);
    }
    return rate;
}
#endif

/* Drain /dev/uac_sa into the ring. The driver's only file
 * operations are open/read/ioctl (there is no poll()), so retry with a
 * short sleep when the host has not queued a full chunk yet.
 *
 * This is a raw pthread, not a Rockbox thread, so it must use usleep()
 * rather than the cooperative sleep() (which only the scheduler's own
 * threads may call). */
static void *dac_pump_thread(void *arg)
{
    int32_t rbuf[DAC_READ_BYTES / sizeof(int32_t)];
    (void)arg;

#if defined(CAYIN_N3PRO)
    /* All device I/O happens on this thread: the vendor driver's open()
     * blocks until the host actually streams, so the caller must never
     * touch the device. */
    const unsigned int gen = dac_gen;
    int fd = open(UAC_SA_DEV, O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        while (dac_running && gen == dac_gen)
        {
            usleep(100000);
            fd = open(UAC_SA_DEV, O_RDWR | O_NONBLOCK);
            if (fd >= 0)
                break;
        }
        if (fd < 0)
        {
            dac_running = false;
            return NULL;
        }
    }
    dac_fd = fd;

    /* Prepare the shared audio DSP chain on this thread (never on the
     * USB thread that spawned us), exactly as usbstack/usb_audio.c does
     * when its UAC connection comes up. DSP_SET_OUT_FREQUENCY must match
     * the mixer rate or the resampler engages (default output 44.1 kHz
     * vs our 48 kHz input) and mangles the host stream. */
    dac_dsp = dsp_get_config(CODEC_IDX_AUDIO);
    dsp_configure(dac_dsp, DSP_RESET, 0);
    dsp_configure(dac_dsp, DSP_SET_STEREO_MODE, STEREO_INTERLEAVED);
    dsp_configure(dac_dsp, DSP_SET_SAMPLE_DEPTH, 16);
    dac_apply_rate(USB_DAC_SAMPLE_RATE);

    /* Exact rate source: the driver logs every host-side rate change.
     * Drain the backlog to catch the rate the host negotiated while we
     * were opening the device. */
    dac_kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    {
        int r = dac_kmsg_rate(512);
        if (r > 0)
            dac_apply_rate(r);
    }

    /* Host-rate inference state: fallback for when /dev/kmsg is not
     * readable; measures the data arrival rate over rolling windows and
     * retimes the chain when two consecutive windows agree. */
    uint64_t win_frames = 0;
    int candidate = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (dac_running && gen == dac_gen)
    {
        /* Follow the driver's rate announcements as they happen. */
        {
            int r = dac_kmsg_rate(8);
            if (r > 0 && r != dac_rate)
                dac_apply_rate(r);
        }

        unsigned int head = dac_head;

        /* Leave room for a full read; else wait for the mixer to drain */
        if (head - dac_tail > DAC_RING_FRAMES - DAC_CHUNK_FRAMES)
        {
            usleep(1000);
            continue;
        }

        ssize_t n = read(fd, rbuf, DAC_READ_BYTES);
        if (n <= 0)
        {
            if (errno == EBADF)
                break;
            usleep(1000);
            continue;
        }

        int frames = n / UAC_SA_FRAME_SIZE;

        /* Run the host PCM through the Rockbox DSP chain, the same way
         * usbstack/usb_audio.c processes its UAC frames, so the EQ and
         * the other Sound settings apply to USB DAC input as well.
         * Bypassed while local playback runs: the codec thread owns the
         * shared CODEC_IDX_AUDIO instance then. */
        struct dsp_config *dsp = dac_dsp;
        if (dsp && mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_STOPPED)
        {
            static int16_t sbuf[DAC_CHUNK_FRAMES * 2];
            static int16_t dbuf[DAC_CHUNK_FRAMES * 2];

            for (int i = 0; i < frames; i++)
            {
                sbuf[2 * i]     = (int16_t)(rbuf[2 * i]     >> 16);
                sbuf[2 * i + 1] = (int16_t)(rbuf[2 * i + 1] >> 16);
            }

            struct dsp_buffer src, dst;
            src.remcount = frames;
            src.pin[0] = sbuf;
            src.pin[1] = sbuf;
            src.proc_mask = 0;
            dst.remcount = 0;
            dst.bufcount = DAC_CHUNK_FRAMES;
            dst.p16out = dbuf;
            dsp_process(dsp, &src, &dst, false);

            frames = dst.remcount;
            for (int i = 0; i < frames; i++)
            {
                unsigned int idx = (head + i) & (DAC_RING_FRAMES - 1);
                dac_ring[2 * idx]     = dbuf[2 * i];
                dac_ring[2 * idx + 1] = dbuf[2 * i + 1];
            }
        }
        else
        {
            for (int i = 0; i < frames; i++)
            {
                unsigned int idx = (head + i) & (DAC_RING_FRAMES - 1);
                dac_ring[2 * idx]     = (int16_t)(rbuf[2 * i]     >> 16);
                dac_ring[2 * idx + 1] = (int16_t)(rbuf[2 * i + 1] >> 16);
            }
        }
        dac_head = head + frames;

        /* Rate inference fallback (only without /dev/kmsg): tally
         * arriving frames per time window. */
        if (dac_kmsg_fd < 0)
        {
            win_frames += frames;
            if (win_frames >= 8192)
            {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long ns = (t1.tv_sec - t0.tv_sec) * 1000000000l
                        + (t1.tv_nsec - t0.tv_nsec);
                if (ns > 0)
                {
                    int rate = dac_infer_rate(win_frames, ns);
                    if (rate == candidate && rate > 0 && rate != dac_rate)
                        dac_apply_rate(rate);
                    candidate = rate;
                }
                win_frames = 0;
                t0 = t1;
            }
        }
    }

    if (dac_kmsg_fd >= 0)
    {
        close(dac_kmsg_fd);
        dac_kmsg_fd = -1;
    }

    close(fd);
    if (dac_fd == fd)
        dac_fd = -1;
    return NULL;

#else /* !CAYIN_N3PRO: upstream R1 behaviour */

    while (dac_running)
    {
        unsigned int head = dac_head;

        /* Leave room for a full read; else wait for the mixer to drain */
        if (head - dac_tail > DAC_RING_FRAMES - DAC_CHUNK_FRAMES)
        {
            usleep(1000);
            continue;
        }

        ssize_t n = read(dac_fd, rbuf, DAC_READ_BYTES);
        if (n <= 0)
        {
            usleep(1000);       /* no host data queued yet */
            continue;
        }

        int frames = n / UAC_SA_FRAME_SIZE;
        for (int i = 0; i < frames; i++)
        {
            unsigned int idx = (head + i) & (DAC_RING_FRAMES - 1);
            dac_ring[2 * idx]     = (int16_t)(rbuf[2 * i]     >> 16);
            dac_ring[2 * idx + 1] = (int16_t)(rbuf[2 * i + 1] >> 16);
        }
        dac_head = head + frames;
    }

    return NULL;
#endif
}

bool usb_dac_start(void)
{
    static const struct mixer_play_cbs cbs = { .get_more = dac_get_more };

#if defined(CAYIN_N3PRO)
    /* usb_detect() runs on more than one thread; without this guard each
     * of them would open the device and spawn its own pump thread. */
    if (dac_running)
        return true;

    /* While local playback runs the output device and its rate belong to
     * it (and the core refuses to start it while the DAC runs, see
     * apps/playback.c). Don't start the host-PCM pump until it stops:
     * usb_detect() retries every tick, so the DAC comes up by itself
     * once playback is over. */
    if (mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_STOPPED)
        return false;

    /* The vendor driver's open()/read() block until the host streams, so
     * ALL device I/O is done by the pump thread; this function only sets
     * up the mixer and spawns it and can therefore never wedge the USB
     * thread it is called from. */
    mixer_set_frequency(USB_DAC_SAMPLE_RATE);

    dac_head = dac_tail = 0;
    dac_rate = 0;   /* force the pump to (re)apply the host rate */
    dac_gen++;
    dac_running = true;
    if (pthread_create(&dac_thread, NULL, dac_pump_thread, NULL) != 0)
    {
        logf("uac_sa pump thread failed");
        dac_running = false;
        return false;
    }
    /* Never joined: a pump blocked inside the driver must not block us. */
    pthread_detach(dac_thread);
#else
    /* The node is created asynchronously when the UDC binds; during boot
     * the hotplug helper can lag behind, so wait briefly for it. This runs
     * on a Rockbox thread, so sleep() yields to the cooperative scheduler
     * instead of stalling every other thread the way usleep() would. */
    for (int tries = 50; tries > 0; tries--)
    {
        dac_fd = open(UAC_SA_DEV, O_RDWR);
        if (dac_fd >= 0)
            break;
        sleep(HZ / 50);     /* 20 ms */
    }
    if (dac_fd < 0)
    {
        logf("uac_sa open failed (%d)", errno);
        return false;
    }

    int rate = USB_DAC_SAMPLE_RATE;
#ifdef LOGF_ENABLE
    // Both on Windows and Linux it always returns 44100 instead of requested rate (which indicates some generic issue).
    // But since we hardcoded the only supported rate with USB_DAC_SAMPLE_RATE in gadget config (see usb-hiby-gadget.c),
    // it should be safe to ignore the mismatch and just log it for debugging purposes.
    int st[3] = {0, 0, 0};
    int status_ok = ioctl(dac_fd, UAC_SA_GET_STATUS, st);
    int reported_rate = st[1];
    if (reported_rate && reported_rate != rate)
    {
        logf("uac_sa rate mismatch: ioctl %d, reported %d, expected %d",
             status_ok, reported_rate, rate);
    }
#endif
    mixer_set_frequency(rate);

    dac_head = dac_tail = 0;
    dac_running = true;
    if (pthread_create(&dac_thread, NULL, dac_pump_thread, NULL) != 0)
    {
        logf("uac_sa pump thread failed");
        dac_running = false;
        close(dac_fd);
        dac_fd = -1;
        return false;
    }
#endif /* CAYIN_N3PRO */

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_USBAUDIO, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_USBAUDIO, &cbs, NULL, 0);
    return true;
}

void usb_dac_stop(void)
{
    if (!dac_running)
        return;

#if defined(CAYIN_N3PRO)
    /* The pump is detached (it may be blocked inside the vendor driver's
     * open()/read() with no poll() to wake it): mark it stale and let it
     * wind down on its own; joining here could deadlock forever. */
    dac_running = false;
    dac_gen++;
    dac_dsp = NULL;
    mixer_channel_stop(PCM_MIXER_CHAN_USBAUDIO);
#else
    dac_running = false;
    pthread_join(dac_thread, NULL);
    mixer_channel_stop(PCM_MIXER_CHAN_USBAUDIO);

    if (dac_fd >= 0)
    {
        close(dac_fd);
        dac_fd = -1;
    }
#endif
}

bool usb_audio_get_active(void)
{
    return dac_running;
}
