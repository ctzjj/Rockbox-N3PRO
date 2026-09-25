/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  __|  _ \| | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \__  \/  __/\__  \  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by the Rockbox-N3PRO contributors
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

/* DLNA renderer streamed-audio playback host.
 *
 * Sibling of the network-radio host (apps/netfm_playback.c) but kept
 * entirely separate so the two features share no code or state: it runs
 * its own decoder thread, its own PCM ring and its own mixer channel
 * (PCM_MIXER_CHAN_DLNA).  Compressed data comes from the DLNA stream
 * engine (apps/dlna/dlna_stream.c) through the source callbacks.
 *
 * The important difference from the radio host: this one is seekable.
 * It keeps codec_api.curpos and filesize up to date and honours
 * seek_buffer(), so container codecs (FLAC, MP4/M4A, ALAC, ...) decode
 * correctly when the engine serves a finite, fully buffered push.
 */

#include "config.h"

#ifdef HAVE_DLNA

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

#include "codecs.h"
#include "dsp_core.h"
#include "kernel.h"
#include "metadata.h"
#include "panic.h"
#include "pcm_mixer.h"
#include "sound.h"
#include "thread.h"

#include "dlna_playback.h"

#define DP_PCM_FRAMES     32768  /* S16 stereo, ~370 ms at 44.1 kHz */
#define DP_CHUNK_FRAMES   512
#define DP_WORKSPACE      (192 * 1024)

/* PCM ring: the decoder thread produces, the mixer callback consumes. */
static int16_t dp_ring[DP_PCM_FRAMES * 2];
static volatile unsigned int dp_head;
static volatile unsigned int dp_tail;

static struct dsp_config *dp_dsp;
static int dp_rate;
static int dp_depth;
static int dp_stereo_mode;
static unsigned int dp_amp = MIX_AMP_UNITY;  /* DLNA stream gain, 16.16 */

static pthread_t dp_thread;
static volatile bool dp_running;    /* the decoder thread must keep going */
static volatile bool dp_stop_req;   /* ask the decoder to return */
static volatile bool dp_done;       /* the decoder thread has returned */
static volatile bool dp_thread_exited = true;   /* no decode thread alive */
static const struct dlna_pb_src *dp_src;
static char dp_codec[12];
static struct codec_api dp_ci;

/* Workspace handed to the decoder (codec_get_buffer). */
static unsigned char dp_workspace[DP_WORKSPACE];

/* ---------------------------------------------------------------- */
/* mixer side                                                        */
/* ---------------------------------------------------------------- */

static void dp_get_more(const void **start, size_t *size)
{
    static int16_t out[2][DP_CHUNK_FRAMES * 2];
    static unsigned int which;
    int16_t *buf;

    which ^= 1;
    buf = out[which];

    unsigned int tail = dp_tail;
    unsigned int avail = dp_head - tail;

    avail = MIN(avail, DP_CHUNK_FRAMES);
    for (unsigned int i = 0; i < avail; i++)
    {
        unsigned int idx = (tail + i) & (DP_PCM_FRAMES - 1);
        buf[2 * i]     = dp_ring[2 * idx];
        buf[2 * i + 1] = dp_ring[2 * idx + 1];
    }
    memset(&buf[2 * avail], 0, (DP_CHUNK_FRAMES - avail) * 2 * sizeof(int16_t));
    dp_tail = tail + avail;

    *start = buf;
    *size = DP_CHUNK_FRAMES * 2 * sizeof(int16_t);
}

static const struct mixer_play_cbs dp_cbs =
{
    .get_more = dp_get_more,
};

/* Follow a sample-rate change at the stream's start.  Runs on the
 * decoder thread. */
static void dp_apply_rate(int rate)
{
    if (rate < 8000 || rate > 192000)
        return;
    if (rate == dp_rate)
        return;
    dp_rate = rate;

    dp_tail = dp_head;              /* drop audio from the old rate */

    /* Hand the DSP the source rate; a fixed-rate output (bluetooth)
     * clamps mixer_set_frequency() to what it can actually run, so read
     * the real output rate back and let the DSP resample to it. */
    mixer_set_frequency(rate);
    int out = mixer_get_frequency();
    if (out < 8000 || out > 192000)
        out = rate;

    if (dp_dsp)
    {
        dsp_configure(dp_dsp, DSP_SET_FREQUENCY, rate);
        dsp_configure(dp_dsp, DSP_SET_OUT_FREQUENCY, out);
    }
    /* mixer_set_frequency() stops the PCM driver; re-kick our channel. */
    mixer_channel_play_data(PCM_MIXER_CHAN_DLNA, &dp_cbs, NULL, 0);
}

/* ---------------------------------------------------------------- */
/* codec_api                                                         */
/* ---------------------------------------------------------------- */

static void *dp_codec_get_buffer(size_t *size)
{
    *size = sizeof(dp_workspace);
    return dp_workspace;
}

static size_t dp_read_filebuf(void *ptr, size_t size)
{
    size_t n = dp_src->read_filebuf(dp_src->context, ptr, size);
    dp_ci.curpos += n;              /* keep the position in sync, as the
                                     * real codec_thread host does */
    return n;
}

static void *dp_request_buffer(size_t *size, size_t request)
{
    return dp_src->request_buffer(dp_src->context, size, request);
}

static void dp_advance_buffer(size_t amount)
{
    dp_src->advance_buffer(dp_src->context, amount);
    dp_ci.curpos += amount;
}

static bool dp_seek_buffer(size_t position)
{
    if (!dp_src->seek_buffer(dp_src->context, position))
        return false;
    dp_ci.curpos = position;
    return true;
}

static void dp_seek_complete(void)
{
    dp_src->seek_complete(dp_src->context);
}

static void dp_set_offset(size_t offset)
{
    dp_src->set_offset(dp_src->context, offset);
}

static void dp_set_elapsed(unsigned long elapsed)
{
    (void)elapsed;                  /* a stream has no local timeline */
}

static void dp_configure(int setting, intptr_t value)
{
    switch (setting)
    {
        case DSP_SET_FREQUENCY:
            dp_apply_rate((int)value);
            break;
        case DSP_SET_SAMPLE_DEPTH:
            dp_depth = (int)value;
            break;
        case DSP_SET_STEREO_MODE:
            dp_stereo_mode = (int)value;
            break;
        default:
            break;
    }

    if (dp_dsp)
        dsp_configure(dp_dsp, setting, value);
    dp_src->configure(dp_src->context, setting, (long)value);
}

static long dp_get_command(intptr_t *param)
{
    (void)param;
    return dp_stop_req ? CODEC_ACTION_HALT : CODEC_ACTION_NULL;
}

/* The decoder runs on a raw pthread, not a Rockbox scheduler thread, so
 * the codec must not use the cooperative sleep()/yield() (they operate
 * on a scheduler entry the pthread does not own, and sleep() in
 * particular never gets woken again).  Use the host calls instead, as
 * the Bluetooth input pump does. */
static unsigned int dp_codec_sleep(unsigned int ticks)
{
    usleep((useconds_t)ticks * (1000000 / HZ));
    return 0;
}

static void dp_codec_yield(void)
{
    sched_yield();
}

static bool dp_loop_track(void)
{
    return false;
}

static void dp_strip_filesize(off_t size)
{
    (void)size;
}

/* Decoded PCM arrives here (planar or interleaved, as the codec
 * configured): run it through the shared DSP chain and push the result
 * into the ring.  Runs on the decoder thread. */
static void dp_pcm_insert(const void *channel1, const void *channel2, int count)
{
    int done = 0;
    /* input samples are 32-bit past 16-bit depth (all our codecs) */
    const size_t sample_size = dp_depth > 16 ? sizeof(int32_t) : sizeof(int16_t);

    while (done < count && !dp_stop_req)
    {
        int frames = MIN(count - done, DP_CHUNK_FRAMES);
        const unsigned char *p1 = (const unsigned char *)channel1 +
                                  (size_t)done * sample_size;
        const unsigned char *p2 = (const unsigned char *)channel2 +
                                  (size_t)done * sample_size;

        while (!dp_stop_req &&
               (dp_head - dp_tail) > DP_PCM_FRAMES - DP_CHUNK_FRAMES)
        {
            usleep(5000);           /* ring full: let the mixer drain it */
        }
        if (dp_stop_req)
            break;

        /* The shared DSP chain belongs to the codec thread while local
         * playback runs; a stream never plays then (local playback stops
         * it), so just wait for the channel to become free. */
        if (!dp_dsp ||
            mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_STOPPED)
        {
            usleep(5000);
            continue;
        }

        static int16_t dbuf[DP_CHUNK_FRAMES * 4];
        struct dsp_buffer src, dst;

        src.remcount = frames;
        src.pin[0] = p1;
        src.pin[1] = p2;
        src.proc_mask = 0;
        dst.remcount = 0;
        dst.bufcount = MAX(frames, 1024);
        dst.p16out = dbuf;

        /* thread_yield must be false: we are a raw pthread, not a Rockbox
         * scheduler thread, and the internal cooperative yield() would
         * corrupt/destroy the scheduler (same as the Bluetooth pump). */
        dsp_process(dp_dsp, &src, &dst, false);

        int out = dst.remcount;
        for (int i = 0; i < out; i++)
        {
            unsigned int idx = (dp_head + i) & (DP_PCM_FRAMES - 1);
            dp_ring[2 * idx]     = dbuf[2 * i];
            dp_ring[2 * idx + 1] = dbuf[2 * i + 1];
        }
        dp_head += out;
        done += frames;
    }
}

/* ---------------------------------------------------------------- */
/* decoder thread                                                    */
/* ---------------------------------------------------------------- */

static void *dp_decode_thread(void *arg)
{
    (void)arg;

    /* The decoder must stay ahead of the output; without this the UI and
     * the LCD updates starve it and the ring underruns (audible clicks),
     * exactly like the Bluetooth input pump. */
    setpriority(PRIO_PROCESS, 0, -10);

    if (codec_load_file(dp_codec, &dp_ci) >= 0)
        codec_run_proc();
    codec_close();

    dp_done = true;                 /* the source reached its end */
    dp_thread_exited = true;        /* the source is now safe to release */
    return NULL;
}

static void dp_codec_api_init(struct mp3entry *id3)
{
    memset(&dp_ci, 0, sizeof(dp_ci));

    dp_ci.dsp = dp_dsp;
    dp_ci.id3 = id3;
    dp_ci.filesize = dp_src->filesize;  /* real length when finite, else -1 */
    dp_ci.curpos = 0;
    dp_ci.audio_hid = -1;
    dp_ci.codec_get_buffer = dp_codec_get_buffer;
    dp_ci.pcmbuf_insert = dp_pcm_insert;
    dp_ci.set_elapsed = dp_set_elapsed;
    dp_ci.read_filebuf = dp_read_filebuf;
    dp_ci.request_buffer = dp_request_buffer;
    dp_ci.advance_buffer = dp_advance_buffer;
    dp_ci.seek_buffer = dp_seek_buffer;
    dp_ci.seek_complete = dp_seek_complete;
    dp_ci.set_offset = dp_set_offset;
    dp_ci.configure = dp_configure;
    dp_ci.get_command = dp_get_command;
    dp_ci.loop_track = dp_loop_track;
    dp_ci.strip_filesize = dp_strip_filesize;

    dp_ci.sleep = dp_codec_sleep;
    dp_ci.yield = dp_codec_yield;
    dp_ci.commit_dcache = commit_dcache;
    dp_ci.commit_discard_dcache = commit_discard_dcache;
    dp_ci.strcpy = strcpy;
    dp_ci.strlen = strlen;
    dp_ci.strcmp = strcmp;
    dp_ci.strcat = strcat;
    dp_ci.memset = memset;
    dp_ci.memcpy = memcpy;
    dp_ci.memmove = memmove;
    dp_ci.memcmp = memcmp;
    dp_ci.memchr = memchr;
    dp_ci.qsort = qsort;
    dp_ci.panicf = panicf;
}

/* ---------------------------------------------------------------- */
/* public API                                                        */
/* ---------------------------------------------------------------- */

bool dlna_pb_start(const char *codec, struct mp3entry *id3,
                   const struct dlna_pb_src *src)
{
    if (dp_running || !codec || !id3 || !src)
        return false;

    /* A previous decoder (stopped but not yet returned) is still reading
     * the shared source; a second one must never overlap it.  Wait,
     * bounded, for it to leave. */
    for (int i = 0; i < 200 && !dp_thread_exited; i++)
        usleep(5000);
    if (!dp_thread_exited)
        return false;

    /* local playback owns the shared DSP chain: it must have yielded
     * before the stream starts */
    if (mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_STOPPED)
        return false;

    dp_src = src;
    snprintf(dp_codec, sizeof(dp_codec), "%s", codec);

    if (!dp_dsp)
    {
        dp_dsp = dsp_get_config(CODEC_IDX_AUDIO);
        dsp_configure(dp_dsp, DSP_RESET, 0);
        dsp_configure(dp_dsp, DSP_SET_STEREO_MODE, STEREO_NONINTERLEAVED);
        dsp_configure(dp_dsp, DSP_SET_SAMPLE_DEPTH, 16);
    }

    dp_head = dp_tail = 0;
    dp_rate = 0;
    dp_depth = 16;
    dp_stereo_mode = STEREO_NONINTERLEAVED;
    dp_stop_req = false;
    dp_done = false;
    dp_codec_api_init(id3);

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_DLNA, dp_amp);
    mixer_channel_play_data(PCM_MIXER_CHAN_DLNA, &dp_cbs, NULL, 0);

    dp_thread_exited = false;
    if (pthread_create(&dp_thread, NULL, dp_decode_thread, NULL) != 0)
    {
        dp_thread_exited = true;
        mixer_channel_stop(PCM_MIXER_CHAN_DLNA);
        return false;
    }

    dp_running = true;
    return true;
}

void dlna_pb_request_stop(void)
{
    dp_stop_req = true;             /* the codec returns; the decoder thread
                                     * then exits and the ring drains */
}

void dlna_pb_stop(void)
{
    if (!dp_running)
        return;

    dp_stop_req = true;             /* the codec returns CODEC_ACTION_HALT */

    /* Stop the mixer channel now and do not wait for the decoder thread -
     * it winds down on its own (never joined). */
    mixer_channel_stop(PCM_MIXER_CHAN_DLNA);

    dp_running = false;
    dp_done = true;
    pthread_detach(dp_thread);
}

bool dlna_pb_active(void)
{
    /* false once the source ended on its own (dp_done), not just when
     * stopped explicitly */
    return dp_running && !dp_done;
}

bool dlna_pb_idle(void)
{
    return dp_thread_exited;
}

/* The AVTransport volume control acts on the DLNA channel's own gain, so
 * the master volume (the wheel / WPS) stays untouched and remains the
 * ceiling: at 1.0 the stream plays at the current master volume, at 0.0
 * it is muted.  gmr sends a linear amplitude (10^(dB/20)), which is
 * exactly the gain we want. */
void dlna_pb_set_amplitude(float value)
{
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;
    dp_amp = (unsigned int)(value * (float)MIX_AMP_UNITY + 0.5f);
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_DLNA, dp_amp);
}

#endif /* HAVE_DLNA */
