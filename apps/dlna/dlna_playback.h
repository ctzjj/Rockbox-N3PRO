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

/* DLNA streamed-audio playback host.
 *
 * This is the DLNA engine's own decoder host (a sibling of the radio's
 * netfm_playback.c, which it does not touch): the DLNA stream engine
 * (apps/dlna/dlna_stream.c) hands the compressed data through the source
 * callbacks below, a dedicated thread runs the Rockbox decoder on it and
 * feeds the PCM into a ring that a mixer channel plays through the normal
 * output path.
 *
 * Unlike the radio host, which only ever sees a forward-only live stream,
 * this one is seekable: it maintains codec_api.curpos and filesize and
 * honours seek_buffer(), so container codecs (FLAC, MP4/M4A, ALAC, ...)
 * work when the engine serves a finite, fully buffered push.
 */

#ifndef DLNA_PLAYBACK_H
#define DLNA_PLAYBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct mp3entry;

/* compressed input for the decoder, provided by the DLNA stream engine */
struct dlna_pb_src
{
    void *context;
    /* total length of the source, or -1 when it is a forward-only stream */
    off_t filesize;
    size_t (*read_filebuf)(void *ctx, void *ptr, size_t size);
    void *(*request_buffer)(void *ctx, size_t *size, size_t request);
    void (*advance_buffer)(void *ctx, size_t amount);
    bool (*seek_buffer)(void *ctx, size_t position);
    void (*seek_complete)(void *ctx);
    void (*set_offset)(void *ctx, size_t offset);
    void (*configure)(void *ctx, int setting, long value);
};

/* Start decoding `codec` with `id3` as its metadata.  Fails when local
 * playback owns the shared DSP chain. */
bool dlna_pb_start(const char *codec, struct mp3entry *id3,
                   const struct dlna_pb_src *src);
void dlna_pb_stop(void);
/* flag-only stop: safe from the audio thread (no locks, no mixer calls) */
void dlna_pb_request_stop(void);
bool dlna_pb_active(void);
/* true once no decode thread is touching the source any more (the
 * previous one has fully returned); the engine must not free or replace
 * the buffered source, nor start a new decode, until this is true */
bool dlna_pb_idle(void);
/* Per-stream gain (0.0..1.0) on the DLNA mixer channel: the AVTransport
 * volume control acts here, NOT on the global master volume.  1.0 means
 * "as loud as the current master volume"; the master volume stays the
 * ceiling.  Safe to call from a Rockbox thread. */
void dlna_pb_set_amplitude(float value);

#endif /* DLNA_PLAYBACK_H */
