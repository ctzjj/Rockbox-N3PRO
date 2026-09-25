/* dlna_output.c - GMediaRender output-module backend for Rockbox.
 *
 * Layering (all DLNA files, no netfm code is touched):
 *
 *     gmr (UPnP/SOAP/GENA)  ->  dlna_output.c  ->  dlna_stream.c
 *                                                     -> dlna_playback.c
 *                                                        (decoder -> mixer
 *                                                         -> DSP -> out)
 *
 * AVTransport semantics vs. the engine capabilities (v1):
 *   SetAVTransportURI -> remember the URI (started on Play)
 *   Play              -> dlna_stream_start() (stops local playback
 *                        first, mirroring the radio entry screen)
 *   Pause/Stop        -> dlna_stream_stop() (Play restarts the URI)
 *   Seek              -> unsupported (streams have no seek yet)
 *   GetPositionInfo   -> no counters; transport degrades to 0:00:00
 *   SetNextAVTransportURI -> auto-advance when the current stream
 *                        reaches DLNA_STREAM_FINISHED
 * RenderingControl volume maps linearly onto the Rockbox master
 * volume (sound_min(SOUND_VOLUME)..sound_max(SOUND_VOLUME) <-> 0..100).
 */

/* _GNU_SOURCE comes from the gmr sources; keep consistent */
#define _GNU_SOURCE

#include "config.h"

#ifdef HAVE_DLNA

#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "audio.h"
#include "file.h"
#include "kernel.h"
#include "sound.h"
#include "settings.h"        /* global_status (struct system_status) */
#include "thread.h"          /* create_thread (Rockbox monitor thread) */

#include "dlna_stream.h"
#include "dlna_playback.h"

#include "output.h"
#include "output_module.h"
#include "upnp_transport.h"
#include "dlna_output.h"

#define URI_LEN DLNA_STREAM_URL_LEN
#define NAME_LEN DLNA_STREAM_NAME_LEN

static char cur_uri[URI_LEN];
static char next_uri[URI_LEN];
static output_transition_cb_t transition_cb;
static volatile bool monitoring;        /* renderer session alive */
static volatile bool pending_play;      /* set by the libupnp worker thread */
static volatile bool pending_stop;
static volatile float pending_volume;   /* 0..1 linear amplitude */
static volatile bool pending_volume_valid;
static float dlna_volume = 1.0f;        /* the AVTransport gain, reported
                                         * back to the controller */
static volatile bool monitor_exited = true;
static bool was_active;
static bool stop_reported;

/* The monitor IS a Rockbox thread: the UPnP callbacks run on a raw
 * libupnp pthread, and starting a stream means audio_stop(), the mixer
 * and create_thread() - all cooperative Rockbox calls that must not run
 * on a pthread (that is what froze the UI).  The pthread only posts a
 * request; the monitor performs it. */
static long dlna_out_mon_stack[(DEFAULT_STACK_SIZE + 0x1000) / sizeof(long)];

static void dlna_output_monitor(void);
static void dlna_output_do_play(void);

/* Derive a display name from the URI: last path component without
 * query string, percent-decoded (a DLNA push URL carries the
 * percent-encoded filename; without decoding the row shows %E4%B8...). */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void uri_to_name(const char *uri, char *name, size_t name_size)
{
    const char *slash = strrchr(uri, '/');
    const char *base = slash ? slash + 1 : uri;
    size_t len = strcspn(base, "?#");

    if (len == 0)
        len = strlen(base);

    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < name_size; i++)
    {
        if (base[i] == '%' && i + 2 < len)
        {
            int hi = hexval(base[i + 1]);
            int lo = hexval(base[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                name[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        name[o++] = base[i];
    }
    name[o] = '\0';
}

/* Controllers send the DIDL title HTML/XML-escaped, often as numeric
 * entities (NetEase: &#32418;&#33394;... == UTF-8 Chinese).  Decode the
 * common named entities and &#NNN;/&#xHH; into UTF-8 in place. */
static void html_entity_decode(char *s)
{
    static const struct { const char *name; char c; } named[] = {
        { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' },
        { "quot;", '"' }, { "apos;", '\'' }, { NULL, 0 }
    };
    char *r = s;
    char *w = s;

    while (*r != '\0')
    {
        if (r[0] == '&' && r[1] == '#')
        {
            char *end = NULL;
            long v = (r[2] == 'x' || r[2] == 'X')
                     ? strtol(r + 3, &end, 16) : strtol(r + 2, &end, 10);
            if (end != NULL && *end == ';' && v > 0 && v < 0x110000)
            {
                if (v < 0x80)
                    *w++ = (char)v;
                else if (v < 0x800)
                {
                    *w++ = (char)(0xC0 | (v >> 6));
                    *w++ = (char)(0x80 | (v & 0x3F));
                }
                else if (v < 0x10000)
                {
                    *w++ = (char)(0xE0 | (v >> 12));
                    *w++ = (char)(0x80 | ((v >> 6) & 0x3F));
                    *w++ = (char)(0x80 | (v & 0x3F));
                }
                else
                {
                    *w++ = (char)(0xF0 | (v >> 18));
                    *w++ = (char)(0x80 | ((v >> 12) & 0x3F));
                    *w++ = (char)(0x80 | ((v >> 6) & 0x3F));
                    *w++ = (char)(0x80 | (v & 0x3F));
                }
                r = end + 1;
                continue;
            }
        }
        else if (r[0] == '&')
        {
            int i;
            for (i = 0; named[i].name != NULL; i++)
            {
                size_t l = strlen(named[i].name);
                if (strncmp(r + 1, named[i].name, l) == 0)
                {
                    *w++ = named[i].c;
                    r += 1 + l;
                    break;
                }
            }
            if (named[i].name != NULL)
                continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static int dlna_output_init(void)
{
    cur_uri[0] = '\0';
    next_uri[0] = '\0';
    transition_cb = NULL;
    was_active = false;
    stop_reported = false;
    pending_play = false;
    pending_stop = false;
    pending_volume_valid = false;
    dlna_volume = 1.0f;

    /* output_init() runs on the UI thread (dlna_renderer_start), so this
     * is a safe place to create a Rockbox monitor thread. */
    if (!monitoring)
    {
        monitoring = true;
        monitor_exited = false;
        if (create_thread(dlna_output_monitor, dlna_out_mon_stack,
                          sizeof(dlna_out_mon_stack), 0, "dlna out"
                          IF_PRIO(, PRIORITY_USER_INTERFACE)
                          IF_COP(, CPU)) <= 0)
        {
            monitoring = false;
            monitor_exited = true;
        }
    }
    return 0;
}

static void dlna_output_set_uri(const char *uri,
                                output_update_meta_cb_t meta_cb)
{
    (void)meta_cb;      /* stream metadata is not parsed in v1 */
    if (!uri)
        uri = "";
    strncpy(cur_uri, uri, URI_LEN - 1);
    cur_uri[URI_LEN - 1] = '\0';
}

static void dlna_output_set_next_uri(const char *uri)
{
    if (!uri)
        uri = "";
    strncpy(next_uri, uri, URI_LEN - 1);
    next_uri[URI_LEN - 1] = '\0';
}

static int dlna_output_play(output_transition_cb_t done_callback)
{
    transition_cb = done_callback;

    if (cur_uri[0] == '\0')
        return -1;

    /* Runs on a raw libupnp pthread: only post the request.  The monitor
     * (a Rockbox thread) performs the can_start check, audio_stop() and
     * dlna_stream_start(), which must not run on a pthread. */
    pending_play = true;
    return 0;
}

/* The actual start, on the Rockbox monitor thread: the stream owns the
 * output, exactly like the radio and the bluetooth receive screens, so
 * stop any local playback first and let its teardown finish. */
static void dlna_output_do_play(void)
{
    char name[NAME_LEN];

    if (cur_uri[0] == '\0' || !dlna_stream_can_start())
        return;

    if (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
    {
        audio_stop();
        for (int i = 0; i < 50 &&
             (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)); i++)
            sleep(HZ / 50);
    }

    {
        const char *title = upnp_transport_last_title();
        if (title != NULL && title[0] != '\0')
        {
            snprintf(name, sizeof(name), "%.*s", NAME_LEN - 1, title);
            html_entity_decode(name);
        }
        else
            uri_to_name(cur_uri, name, sizeof(name));
    }
    stop_reported = false;
    dlna_stream_start(name, cur_uri);
}

static int dlna_output_stop(void)
{
    pending_stop = true;        /* performed by the monitor thread */
    return 0;
}

static int dlna_output_pause(void)
{
    /* the engine has no pause; stop now, Play restarts the URI */
    pending_stop = true;
    return 0;
}

static int dlna_output_seek(gint64 position_nanos)
{
    (void)position_nanos;
    return -1;
}

static int dlna_output_get_position(gint64 *track_duration,
                                    gint64 *track_position)
{
    (void)track_duration;
    (void)track_position;
    return -1;
}

static int dlna_output_get_volume(float *value)
{
    /* That is the DLNA stream's own gain, not the master volume: the
     * controller sees its own setting, and the player keeps the ceiling. */
    if (!value)
        return -1;
    *value = dlna_volume;
    return 0;
}

static int dlna_output_set_volume(float value)
{
    /* Runs on the libupnp worker thread: only post it.  The monitor (a
     * Rockbox thread) applies it to the DLNA channel's gain - the master
     * volume is never touched. */
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;
    pending_volume = value;
    pending_volume_valid = true;
    return 0;
}

static int dlna_output_get_mute(int *value)
{
    if (!value)
        return -1;
    *value = 0;
    return 0;
}

static int dlna_output_set_mute(int value)
{
    (void)value;        /* no master mute in Rockbox; accept and ignore */
    return 0;
}

/* Watch the stream: a finished track with a queued SetNextAVTransportURI
 * advances automatically, feeding the transport its transition
 * callback.  A finished track without one just ends - the engine does
 * not reconnect, so a pushed file never loops. */
/* Rockbox monitor thread: performs the play/stop requests posted by the
 * libupnp pthread, and auto-advances a finished track when a
 * SetNextAVTransportURI is queued.  A finished track without one just
 * ends - the engine does not reconnect, so a pushed file never loops. */
static void dlna_output_monitor(void)
{
    while (monitoring)
    {
        struct dlna_stream_status status;
        bool active;

        if (pending_stop)
        {
            pending_stop = false;
            dlna_stream_stop();
        }
        if (pending_volume_valid)
        {
            pending_volume_valid = false;
            dlna_volume = pending_volume;
            dlna_pb_set_amplitude(dlna_volume);
        }
        if (pending_play)
        {
            pending_play = false;
            dlna_output_do_play();

            /* If the push could not start (the output is owned by a hard
             * input), put the transport back to STOPPED instead of leaving
             * it "PLAYING" with no audio. */
            if (!dlna_stream_is_active())
            {
                if (transition_cb)
                    transition_cb(PLAY_STOPPED);
                stop_reported = true;
            }
        }

        /* Bring the decoder up as soon as the worker has probed the
         * stream - exactly like the radio monitor, so this does not
         * depend on the DLNA screen being refreshed. */
        if (dlna_stream_codec_pending())
            dlna_stream_codec_start_now();

        active = dlna_stream_is_active();
        if (active)
            dlna_stream_get_status(&status);
        else
            status.state = DLNA_STREAM_IDLE;

        bool ended = (status.state == DLNA_STREAM_FINISHED) ||
                     (was_active && !active);
        if (ended && next_uri[0] != '\0')
        {
            strncpy(cur_uri, next_uri, URI_LEN - 1);
            cur_uri[URI_LEN - 1] = '\0';
            next_uri[0] = '\0';
            stop_reported = false;

            dlna_output_do_play();
            if (dlna_stream_is_active() && transition_cb)
                transition_cb(PLAY_STARTED_NEXT_STREAM);
            active = dlna_stream_is_active();
        }
        else if (ended && !stop_reported)
        {
            /* The stream finished, or another output owner stopped it.
             * Tell the transport it is STOPPED: otherwise transport_state_
             * stays TRANSPORT_PLAYING and the controller's next Play is a
             * silent no-op ("nothing to change") - the renderer then looks
             * stuck on "waiting for push". */
            if (transition_cb)
                transition_cb(PLAY_STOPPED);
            stop_reported = true;
        }
        was_active = active;

        sleep(HZ / 4);
    }
    monitor_exited = true;
}

/* Full teardown: stop the stream, end the monitor thread (bounded wait)
 * so no worker survives the DLNA screen. */
void dlna_output_shutdown(void)
{
    if (monitoring)
    {
        monitoring = false;
        for (int i = 0; i < 200 && !monitor_exited; i++)
            sleep(HZ / 100);
    }
    dlna_stream_stop();
    cur_uri[0] = '\0';
    next_uri[0] = '\0';
    transition_cb = NULL;
    pending_play = false;
    pending_stop = false;
    was_active = false;
}

struct output_module dlna_stream_output =
{
    .shortname    = "dlna",
    .description  = "Rockbox DLNA stream engine (through the DSP chain)",
    .add_options  = NULL,
    .init         = dlna_output_init,
    .set_uri      = dlna_output_set_uri,
    .set_next_uri = dlna_output_set_next_uri,
    .play         = dlna_output_play,
    .stop         = dlna_output_stop,
    .pause        = dlna_output_pause,
    .loop         = NULL,
    .seek         = dlna_output_seek,
    .get_position = dlna_output_get_position,
    .get_volume   = dlna_output_get_volume,
    .set_volume   = dlna_output_set_volume,
    .get_mute     = dlna_output_get_mute,
    .set_mute     = dlna_output_set_mute,
};

#endif /* HAVE_DLNA */
