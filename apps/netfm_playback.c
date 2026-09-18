/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/            \/     \/
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

/* Streamed-audio playback.  The stream engine provides the compressed
 * data; this module owns a decoder thread and a PCM ring and plays it
 * through a mixer channel of its own (patterned after the Bluetooth
 * receive pump).  Local playback state is never touched. */

#include "config.h"

#ifdef HAVE_NETFM

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "codecs.h"
#include "dsp_core.h"
#include "kernel.h"
#include "metadata.h"
#include "panic.h"
#include "pcm_mixer.h"
#include "sound.h"
#include "thread.h"

#include "netfm_playback.h"

#define NF_PCM_FRAMES     32768  /* S16 stereo, ~370 ms at 44.1 kHz */
#define NF_CHUNK_FRAMES   512
#define NF_WORKSPACE      (192 * 1024)
#define NF_STOP_TRIES     100

/* PCM ring: the decoder thread produces, the mixer callback consumes. */
static int16_t nf_ring[NF_PCM_FRAMES * 2];
static volatile unsigned int nf_head;
static volatile unsigned int nf_tail;

static struct dsp_config *nf_dsp;
static int nf_rate;
static int nf_depth;
static int nf_stereo_mode;

static pthread_t nf_thread;
static volatile bool nf_running;    /* the decoder thread must keep going */
static volatile bool nf_stop_req;   /* ask the decoder to return */
static const struct netfm_playback_src *nf_src;
static char nf_codec[12];
static struct codec_api nf_ci;

/* Workspace handed to the decoder (codec_get_buffer). */
static unsigned char nf_workspace[NF_WORKSPACE];

/* ---------------------------------------------------------------- */
/* mixer side                                                        */
/* ---------------------------------------------------------------- */

static void nf_get_more(const void **start, size_t *size)
{
    static int16_t out[2][NF_CHUNK_FRAMES * 2];
    static unsigned int which;
    int16_t *buf;

    which ^= 1;
    buf = out[which];

    unsigned int tail = nf_tail;
    unsigned int avail = nf_head - tail;

    avail = MIN(avail, NF_CHUNK_FRAMES);
    for (unsigned int i = 0; i < avail; i++)
    {
        unsigned int idx = (tail + i) & (NF_PCM_FRAMES - 1);
        buf[2 * i]     = nf_ring[2 * idx];
        buf[2 * i + 1] = nf_ring[2 * idx + 1];
    }
    memset(&buf[2 * avail], 0, (NF_CHUNK_FRAMES - avail) * 2 * sizeof(int16_t));
    nf_tail = tail + avail;

    *start = buf;
    *size = NF_CHUNK_FRAMES * 2 * sizeof(int16_t);
}

static const struct mixer_play_cbs nf_cbs =
{
    .get_more = nf_get_more,
};

/* Follow a sample-rate change at the stream's start.  Runs on the
 * decoder thread. */
static void nf_apply_rate(int rate)
{
    if (rate < 8000 || rate > 192000)
        return;
    if (rate == nf_rate)
        return;
    nf_rate = rate;

    nf_tail = nf_head;              /* drop audio from the old rate */

    /* Hand the DSP the source rate; a fixed-rate output (bluetooth)
     * clamps mixer_set_frequency() to what it can actually run, so read
     * the real output rate back and let the DSP resample to it. */
    mixer_set_frequency(rate);
    int out = mixer_get_frequency();
    if (out < 8000 || out > 192000)
        out = rate;

    if (nf_dsp)
    {
        dsp_configure(nf_dsp, DSP_SET_FREQUENCY, rate);
        dsp_configure(nf_dsp, DSP_SET_OUT_FREQUENCY, out);
    }
    /* mixer_set_frequency() stops the PCM driver; re-kick our channel. */
    mixer_channel_play_data(PCM_MIXER_CHAN_NETFM, &nf_cbs, NULL, 0);
}

/* ---------------------------------------------------------------- */
/* codec_api                                                         */
/* ---------------------------------------------------------------- */

static void *nf_codec_get_buffer(size_t *size)
{
    *size = sizeof(nf_workspace);
    return nf_workspace;
}

static size_t nf_read_filebuf(void *ptr, size_t size)
{
    return nf_src->read_filebuf(nf_src->context, ptr, size);
}

static void *nf_request_buffer(size_t *size, size_t request)
{
    return nf_src->request_buffer(nf_src->context, size, request);
}

static void nf_advance_buffer(size_t amount)
{
    nf_src->advance_buffer(nf_src->context, amount);
}

static bool nf_seek_buffer(size_t position)
{
    return nf_src->seek_buffer(nf_src->context, position);
}

static void nf_seek_complete(void)
{
    nf_src->seek_complete(nf_src->context);
}

static void nf_set_offset(size_t offset)
{
    nf_src->set_offset(nf_src->context, offset);
}

static void nf_set_elapsed(unsigned long elapsed)
{
    (void)elapsed;                  /* a live stream has no timeline */
}

static void nf_configure(int setting, intptr_t value)
{
    switch (setting)
    {
        case DSP_SET_FREQUENCY:
            nf_apply_rate((int)value);
            break;
        case DSP_SET_SAMPLE_DEPTH:
            nf_depth = (int)value;
            break;
        case DSP_SET_STEREO_MODE:
            nf_stereo_mode = (int)value;
            break;
        default:
            break;
    }

    if (nf_dsp)
        dsp_configure(nf_dsp, setting, value);
    nf_src->configure(nf_src->context, setting, (long)value);
}

static int nf_get_command(intptr_t *param)
{
    (void)param;
    return nf_stop_req ? CODEC_ACTION_HALT : CODEC_ACTION_NULL;
}

/* The decoder runs on a raw pthread, not a Rockbox scheduler thread, so
 * the codec must not use the cooperative sleep()/yield() (they operate
 * on a scheduler entry the pthread does not own, and sleep() in
 * particular never gets woken again).  Use the host calls instead, as
 * the Bluetooth input pump does. */
static void nf_codec_sleep(int ticks)
{
    usleep((useconds_t)ticks * (1000000 / HZ));
}

static void nf_codec_yield(void)
{
    sched_yield();
}

static bool nf_loop_track(void)
{
    return false;
}

static void nf_strip_filesize(off_t size)
{
    (void)size;
}

/* Decoded PCM arrives here (planar or interleaved, as the codec
 * configured): run it through the shared DSP chain and push the result
 * into the ring.  Runs on the decoder thread. */
static void nf_pcm_insert(const void *channel1, const void *channel2, int count)
{
    int done = 0;
    /* input samples are 32-bit past 16-bit depth (all our codecs) */
    const size_t sample_size = nf_depth > 16 ? sizeof(int32_t) : sizeof(int16_t);

    while (done < count && !nf_stop_req)
    {
        int frames = MIN(count - done, NF_CHUNK_FRAMES);
        const unsigned char *p1 = (const unsigned char *)channel1 +
                                  (size_t)done * sample_size;
        const unsigned char *p2 = (const unsigned char *)channel2 +
                                  (size_t)done * sample_size;

        while (!nf_stop_req &&
               (nf_head - nf_tail) > NF_PCM_FRAMES - NF_CHUNK_FRAMES)
        {
            usleep(5000);           /* ring full: let the mixer drain it */
        }
        if (nf_stop_req)
            break;

        /* The shared DSP chain belongs to the codec thread while local
         * playback runs; a stream never plays then (local playback stops
         * it), so just wait for the channel to become free. */
        if (!nf_dsp ||
            mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_STOPPED)
        {
            usleep(5000);
            continue;
        }

        static int16_t dbuf[NF_CHUNK_FRAMES * 4];
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
        dsp_process(nf_dsp, &src, &dst, false);

        int out = dst.remcount;
        for (int i = 0; i < out; i++)
        {
            unsigned int idx = (nf_head + i) & (NF_PCM_FRAMES - 1);
            nf_ring[2 * idx]     = dbuf[2 * i];
            nf_ring[2 * idx + 1] = dbuf[2 * i + 1];
        }
        nf_head += out;
        done += frames;
    }
}

/* ---------------------------------------------------------------- */
/* decoder thread                                                    */
/* ---------------------------------------------------------------- */

static void *nf_decode_thread(void *arg)
{
    (void)arg;

    /* The decoder must stay ahead of the output; without this the UI and
     * the LCD updates starve it and the ring underruns (audible clicks),
     * exactly like the Bluetooth input pump. */
    setpriority(PRIO_PROCESS, 0, -10);

    int status = codec_load_file(nf_codec, &nf_ci);

    if (status >= 0)
        status = codec_run_proc();
    codec_close();

    return NULL;
}

static void nf_codec_api_init(struct mp3entry *id3)
{
    memset(&nf_ci, 0, sizeof(nf_ci));

    nf_ci.dsp = nf_dsp;
    nf_ci.id3 = id3;
    nf_ci.filesize = -1;
    nf_ci.audio_hid = -1;
    nf_ci.codec_get_buffer = nf_codec_get_buffer;
    nf_ci.pcmbuf_insert = nf_pcm_insert;
    nf_ci.set_elapsed = nf_set_elapsed;
    nf_ci.read_filebuf = nf_read_filebuf;
    nf_ci.request_buffer = nf_request_buffer;
    nf_ci.advance_buffer = nf_advance_buffer;
    nf_ci.seek_buffer = nf_seek_buffer;
    nf_ci.seek_complete = nf_seek_complete;
    nf_ci.set_offset = nf_set_offset;
    nf_ci.configure = nf_configure;
    nf_ci.get_command = nf_get_command;
    nf_ci.loop_track = nf_loop_track;
    nf_ci.strip_filesize = nf_strip_filesize;

    nf_ci.sleep = nf_codec_sleep;
    nf_ci.yield = nf_codec_yield;
    nf_ci.commit_dcache = commit_dcache;
    nf_ci.commit_discard_dcache = commit_discard_dcache;
    nf_ci.strcpy = strcpy;
    nf_ci.strlen = strlen;
    nf_ci.strcmp = strcmp;
    nf_ci.strcat = strcat;
    nf_ci.memset = memset;
    nf_ci.memcpy = memcpy;
    nf_ci.memmove = memmove;
    nf_ci.memcmp = memcmp;
    nf_ci.memchr = memchr;
    nf_ci.qsort = qsort;
    nf_ci.panicf = panicf;
}

/* ---------------------------------------------------------------- */
/* public API                                                        */
/* ---------------------------------------------------------------- */

bool netfm_playback_start(const char *codec, struct mp3entry *id3,
                          const struct netfm_playback_src *src)
{
    if (nf_running || !codec || !id3 || !src)
        return false;

    /* local playback owns the shared DSP chain: it must have yielded
     * (it calls netfm_stream_stop() before starting) */
    if (mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_STOPPED)
        return false;

    nf_src = src;
    snprintf(nf_codec, sizeof(nf_codec), "%s", codec);

    if (!nf_dsp)
    {
        nf_dsp = dsp_get_config(CODEC_IDX_AUDIO);
        dsp_configure(nf_dsp, DSP_RESET, 0);
        dsp_configure(nf_dsp, DSP_SET_STEREO_MODE, STEREO_NONINTERLEAVED);
        dsp_configure(nf_dsp, DSP_SET_SAMPLE_DEPTH, 16);
    }

    nf_head = nf_tail = 0;
    nf_rate = 0;
    nf_depth = 16;
    nf_stereo_mode = STEREO_NONINTERLEAVED;
    nf_stop_req = false;
    nf_codec_api_init(id3);

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_NETFM, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_NETFM, &nf_cbs, NULL, 0);

    if (pthread_create(&nf_thread, NULL, nf_decode_thread, NULL) != 0)
    {
        mixer_channel_stop(PCM_MIXER_CHAN_NETFM);
        return false;
    }

    nf_running = true;
    return true;
}

void netfm_playback_request_stop(void)
{
    nf_stop_req = true;             /* the codec returns; the decoder thread
                                     * then exits and the ring drains */
}

void netfm_playback_stop(void)
{
    if (!nf_running)
        return;

    nf_stop_req = true;             /* the codec returns CODEC_ACTION_HALT */

    /* Bluetooth-style: stop the mixer channel now and do not wait for the
     * decoder thread - it winds down on its own (never joined). */
    mixer_channel_stop(PCM_MIXER_CHAN_NETFM);

    nf_running = false;
    pthread_detach(nf_thread);
}

bool netfm_playback_active(void)
{
    return nf_running;
}

#endif /* HAVE_NETFM */
