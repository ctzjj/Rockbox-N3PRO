#include "config.h"
#ifdef HAVE_NETFM
#include <stdio.h>
#include <string.h>
#include "action.h"
#include "audio.h"
#include "file.h"
#include "gui/list.h"
#include "kernel.h"
#include "lang.h"
#include "misc.h"
#include "netfm.h"
#include "netfm_stream.h"
#include "splash.h"
#include "wifi_hal.h"

#define NETFM_PATH ROCKBOX_DIR "/stream/netfm/netfm.txt"
#define NETFM_STATUS_ROWS 7

static struct netfm_station stations[NETFM_MAX_STATIONS];
static int station_count;

static void trim(char *s)
{
    char *p = s;
    size_t n;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
}

int netfm_parse_presets(struct netfm_station *out, int max)
{
    int fd = open(NETFM_PATH, O_RDONLY), count = 0;
    char line[NETFM_NAME_LEN + NETFM_URL_LEN + 4];
    if (fd < 0) return 0;
    while (count < max)
    {
        int n = 0;
        char c;
        while (n + 1 < (int)sizeof(line) && read(fd, &c, 1) == 1)
        {
            line[n++] = c;
            if (c == '\n') break;
        }
        if (!n) break;
        line[n] = '\0';
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *comma = strchr(line, ',');
        if (!comma) continue;
        *comma++ = '\0';
        trim(line);
        trim(comma);
        if (!line[0] || !comma[0]) continue;
        snprintf(out[count].name, sizeof(out[count].name), "%.127s", line);
        snprintf(out[count].url, sizeof(out[count].url), "%.511s", comma);
        count++;
    }
    close(fd);
    return count;
}

static const char *station_name_cb(int selected, void *data,
                                   char *buffer, size_t size)
{
    (void)data;
    if (selected < 0 || selected >= station_count) return "";
    snprintf(buffer, size, "%.127s", stations[selected].name);
    return buffer;
}

static const char *state_name(enum netfm_stream_state state)
{
    switch (state)
    {
        case NETFM_STREAM_CONNECTING: return str(LANG_NETFM_ST_CONNECTING);
        case NETFM_STREAM_BUFFERING: return str(LANG_NETFM_ST_BUFFERING);
        case NETFM_STREAM_PLAYING: return str(LANG_NETFM_ST_PLAYING);
        case NETFM_STREAM_DISCONNECTED: return str(LANG_NETFM_ST_DISCONNECTED);
        case NETFM_STREAM_ERROR: return str(LANG_NETFM_ST_ERROR);
        default: return str(LANG_NETFM_ST_STOPPED);
    }
}

static const char *status_name_cb(int selected, void *data,
                                  char *buffer, size_t size)
{
    struct netfm_stream_status *status = data;
    if (!status)
        return "";
    switch (selected)
    {
        case 0:
            snprintf(buffer, size, "%s: %s", str(LANG_BT_STATUS),
                     state_name(status->state));
            break;
        case 1:
            snprintf(buffer, size, "%s: %.80s", str(LANG_NETFM_STATION),
                     status->name);
            break;
        case 2:
            snprintf(buffer, size, "%s: %.12s", str(LANG_NETFM_FORMAT),
                     status->format);
            break;
        case 3:
            snprintf(buffer, size, "%s: %d kbps", str(LANG_NETFM_BITRATE),
                     status->bitrate);
            break;
        case 4:
            snprintf(buffer, size, "%s: %d Hz", str(LANG_BT_RX_RATE),
                     status->sample_rate);
            break;
        case 5:
            snprintf(buffer, size, "%s: %d%%", str(LANG_NETFM_BUFFER),
                     status->buffer_percent);
            break;
        default:
            snprintf(buffer, size, "%s", str(LANG_NETFM_STOP));
            break;
    }
    return buffer;
}

static int status_action_cb(int action, struct gui_synclist *lists)
{
    static char previous[NETFM_STATUS_ROWS][96];
    struct netfm_stream_status *status = lists->data;
    char current[96];
    bool changed = false;

    if (netfm_stream_codec_pending())
        netfm_stream_codec_start_now();

    /* The wheel adjusts the shared system volume: the stream plays at
     * unity channel gain through the normal output path, so the user's
     * own volume applies, exactly as it does to the USB DAC input.
     * Consuming the action keeps the cursor of this status page still. */
    if (action == ACTION_STD_NEXT)
    {
        adjust_volume(1);
        return ACTION_NONE;
    }
    if (action == ACTION_STD_PREV)
    {
        adjust_volume(-1);
        return ACTION_NONE;
    }

    if (action == ACTION_STD_OK)
    {
        /* only the last row (stop) acts; other rows do nothing */
        if (gui_synclist_get_sel_pos(lists) == NETFM_STATUS_ROWS - 1)
        {
            netfm_stream_stop();
            return ACTION_STD_CANCEL;
        }
        return ACTION_NONE;
    }
    if (action != ACTION_NONE || !status)
        return action;

    /* the worker keeps the snapshot current; refresh it here so the
     * redraw below sees fresh values */
    netfm_stream_get_status(status);

    for (int i = 0; i < NETFM_STATUS_ROWS; i++)
    {
        status_name_cb(i, status, current, sizeof(current));
        if (strcmp(previous[i], current) != 0)
            changed = true;
    }
    if (!changed)
        return ACTION_NONE;
    for (int i = 0; i < NETFM_STATUS_ROWS; i++)
        status_name_cb(i, status, previous[i], sizeof(previous[i]));
    return ACTION_REDRAW;
}

static void netfm_play_screen(const struct netfm_station *station)
{
    struct netfm_stream_status status;
    struct simplelist_info info;
    bool same;

    /* The radio keeps playing when this screen is left, so entering it
     * again attaches to the running stream instead of restarting it.
     * Selecting another station replaces the current one. */
    netfm_stream_get_status(&status);
    same = netfm_stream_is_active() && strcmp(status.name, station->name) == 0;

    if (!same)
    {
        if (!netfm_stream_can_start())
        {
            splash(HZ * 2, str(LANG_NETFM_CONFLICT));
            return;
        }
        if (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        {
            audio_stop();
            /* the stop is asynchronous - let it finish so its teardown
             * cannot stop the output channel the stream is about to use */
            for (int i = 0; i < 50 &&
                 (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)); i++)
                sleep(HZ / 50);
        }
        if (!netfm_stream_start(station->name, station->url))
        {
            splash(HZ * 2, str(LANG_NETFM_START_FAIL));
            return;
        }
        netfm_stream_get_status(&status);
    }

    simplelist_info_init(&info, str(LANG_NETFM), NETFM_STATUS_ROWS, &status);
    info.get_name = status_name_cb;
    info.action_callback = status_action_cb;
    info.timeout = HZ * 2;
    info.selection = NETFM_STATUS_ROWS - 1;
    info.title_icon = Icon_Submenu;
    simplelist_show_list(&info);

    /* deliberately no stop here: Back leaves the radio playing and the
     * stop row (last) tears the stream down explicitly */
}

int netfm_menu(void)
{
    if (!wifi_hal_is_up())
    {
        splash(HZ * 2, str(LANG_NETFM_WIFI_REQUIRED));
        return 0;
    }
    station_count = netfm_parse_presets(stations, NETFM_MAX_STATIONS);
    if (!station_count)
    {
        splash(HZ * 2, str(LANG_NETFM_EMPTY));
        return 0;
    }
    while (true)
    {
        struct simplelist_info info;
        simplelist_info_init(&info, str(LANG_NETFM), station_count, NULL);
        info.get_name = station_name_cb;
        info.selection = -1;
        info.title_icon = Icon_Submenu;
        simplelist_show_list(&info);
        if (info.selection < 0)
            break;
        netfm_play_screen(&stations[info.selection]);
    }
    return 0;
}
#endif
