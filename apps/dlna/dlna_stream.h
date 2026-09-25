/* dlna_stream.h - standalone stream engine for the DLNA renderer.
 *
 * Modelled one-to-one on apps/netfm_stream.h (the network radio): a
 * detached worker thread owns the HTTP(S) connection and fills a
 * compressed-data ring; the shared decoder host (netfm_playback.c)
 * consumes it and plays through its own mixer channel, so the pushed
 * audio follows the exact same path as the radio - mixer channel ->
 * DSP chain -> output - without touching any netfm code.
 *
 * Differences from the radio engine, imposed by DLNA pushes:
 *  - the stream format is sniffed from the first body bytes (a push
 *    can be FLAC/OGG/WAV/M4A/ADTS/MP3, not just the radio's MP3), and
 *  - a push of a finite file (Content-Length) plays ONCE: no radio
 *    style reconnect loop; the state ends in DLNA_STREAM_FINISHED.
 */
#ifndef DLNA_STREAM_H
#define DLNA_STREAM_H

#include <stdbool.h>
#include <stddef.h>

#define DLNA_STREAM_NAME_LEN 128
#define DLNA_STREAM_URL_LEN 512

enum dlna_stream_state
{
    DLNA_STREAM_IDLE = 0,
    DLNA_STREAM_CONNECTING,
    DLNA_STREAM_BUFFERING,
    DLNA_STREAM_PLAYING,
    DLNA_STREAM_DISCONNECTED,
    /* the finite stream was played to its end (no reconnect) */
    DLNA_STREAM_FINISHED,
    DLNA_STREAM_ERROR,
};

struct dlna_stream_status
{
    enum dlna_stream_state state;
    char name[DLNA_STREAM_NAME_LEN];
    char format[16];
    int sample_rate;
    int bitrate;
    int buffer_percent;
};

bool dlna_stream_start(const char *name, const char *url);
void dlna_stream_stop(void);
/* flag-only stop, safe from the audio thread (local playback takeover) */
void dlna_stream_stop_async(void);
bool dlna_stream_get_status(struct dlna_stream_status *status);
bool dlna_stream_is_active(void);
/* false when another source owns the audio path: bluetooth receive,
 * USB DAC, or a running network-radio stream */
bool dlna_stream_can_start(void);
/* the worker sniffs the stream type; the UI brings the decoder up from
 * a Rockbox thread when it becomes ready */
bool dlna_stream_codec_pending(void);
bool dlna_stream_codec_start_now(void);

#endif /* DLNA_STREAM_H */
