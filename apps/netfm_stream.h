#ifndef NETFM_STREAM_H
#define NETFM_STREAM_H

#include <stdbool.h>
#include <stddef.h>

#define NETFM_STREAM_NAME_LEN 128
#define NETFM_STREAM_URL_LEN 512

enum netfm_stream_state
{
    NETFM_STREAM_IDLE = 0,
    NETFM_STREAM_CONNECTING,
    NETFM_STREAM_BUFFERING,
    NETFM_STREAM_PLAYING,
    NETFM_STREAM_DISCONNECTED,
    NETFM_STREAM_ERROR,
};

struct netfm_stream_status
{
    enum netfm_stream_state state;
    char name[NETFM_STREAM_NAME_LEN];
    char format[16];
    int sample_rate;
    int bitrate;
    int buffer_percent;
};

bool netfm_stream_start(const char *name, const char *url);
void netfm_stream_stop(void);
/* release the output immediately (local playback takeover) */
void netfm_stream_stop_async(void);
bool netfm_stream_get_status(struct netfm_stream_status *status);
bool netfm_stream_is_active(void);
/* false when an external input (bluetooth receive, USB DAC) owns the
 * audio path; network radio and those must not run together */
bool netfm_stream_can_start(void);
/* the worker resolves the stream type; the UI brings the decoder up
 * from a Rockbox thread when it becomes ready */
bool netfm_stream_codec_pending(void);
bool netfm_stream_codec_start_now(void);

/* TEMP diagnostics: emit a timestamped mark into the netfm log */

#endif
