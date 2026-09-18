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

/* Streamed-audio playback for the network radio: the compressed data
 * comes from the stream engine (apps/netfm_stream.c) through the source
 * callbacks below; a dedicated thread runs the Rockbox decoder on it and
 * feeds the PCM into a ring that a mixer channel plays through the
 * normal output path (the same shape as the Bluetooth receive pump in
 * n3pro-bt-input.c).
 *
 * Nothing of the local playback state (playlist, buffering, resume, id3
 * slots) is touched: a stream is a source of its own.
 */

#ifndef NETFM_PLAYBACK_H
#define NETFM_PLAYBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mp3entry;

/* compressed input for the decoder, provided by the stream engine */
struct netfm_playback_src
{
    void *context;
    size_t (*read_filebuf)(void *ctx, void *ptr, size_t size);
    void *(*request_buffer)(void *ctx, size_t *size, size_t request);
    void (*advance_buffer)(void *ctx, size_t amount);
    bool (*seek_buffer)(void *ctx, size_t position);
    void (*seek_complete)(void *ctx);
    void (*set_offset)(void *ctx, size_t offset);
    void (*configure)(void *ctx, int setting, long value);
};

/* Start decoding `codec` (e.g. "aac_bsf"/"mpa") with `id3` as its
 * metadata.  Fails when local playback owns the shared DSP chain. */
bool netfm_playback_start(const char *codec, struct mp3entry *id3,
                          const struct netfm_playback_src *src);
void netfm_playback_stop(void);
/* flag-only stop: safe from the audio thread (no locks, no mixer calls) */
void netfm_playback_request_stop(void);
bool netfm_playback_active(void);

#endif /* NETFM_PLAYBACK_H */
