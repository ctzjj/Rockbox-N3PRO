/* DLNA screen - UPnP MediaRenderer front-end (GMediaRender + pupnp).
 *
 * UI modelled after apps/netfm.c: entering the screen brings the
 * renderer up; Back leaves it running in the background so the phone
 * can keep pushing; the last row ("Stop") tears everything down with
 * no residual threads.  The wheel adjusts the shared volume, exactly
 * like the netfm status screen. */
#include "config.h"
#ifdef HAVE_DLNA

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>

#include "action.h"
#include "audio.h"
#include "gui/list.h"
#include "kernel.h"
#include "lang.h"
#include "misc.h"
#include "splash.h"
#include "wifi_hal.h"
#ifdef HAVE_NETFM
#include "netfm_stream.h"
#endif
#ifdef HAVE_BT_INPUT
#include "bt_input.h"
#endif
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
#include "usb.h"
#endif

#include "dlna.h"
#include "dlna_stream.h"
#include "dlna_output.h"
#include "output.h"
#include "logging.h"
#include "upnp_device.h"
#include "upnp_renderer.h"
#include "upnp_transport.h"
#include "upnp_control.h"

#define DLNA_ROWS 6
#define DLNA_FRIENDLY_NAME "Rockbox N3Pro"
#define DLNA_UUID "Rockbox-N3Pro-1"
#define DLNA_PORT 49494

static struct upnp_device *dlna_device;
static bool dlna_running;

bool dlna_renderer_active(void)
{
    return dlna_running;
}

const char *dlna_renderer_friendly_name(void)
{
    return DLNA_FRIENDLY_NAME;
}

/* HiByOS ships with the loopback interface unconfigured (no 127.0.0.1).
 * libupnp's miniserver binds a UDP socket to 127.0.0.1 and UpnpInit()
 * fails with UPNP_E_SOCKET_BIND (-203), which makes GMediaRender retry
 * for 60s in the calling thread - freezing the UI. Bring lo up first;
 * harmless if it is already configured. */
static void ensure_loopback_up(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);

    struct sockaddr_in *sa = (struct sockaddr_in *)&ifr.ifr_addr;
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ioctl(s, SIOCSIFADDR, &ifr);

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0)
    {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

bool dlna_renderer_start(void)
{
    struct upnp_device_descriptor *desc;
    struct upnp_device *dev;

    if (dlna_running)
        return true;

    ensure_loopback_up();
    Log_init(NULL);
    output_init("dlna");

    desc = upnp_renderer_descriptor(DLNA_FRIENDLY_NAME, DLNA_UUID);
    if (!desc)
        return false;

    dev = upnp_device_init(desc, NULL, DLNA_PORT);
    if (!dev)
        return false;

    upnp_transport_init(dev);
    upnp_control_init(dev);

    dlna_device = dev;
    dlna_running = true;
    return true;
}

void dlna_renderer_stop(void)
{
    if (!dlna_running)
        return;

    dlna_running = false;

    /* Detach the event collectors from the device before it is destroyed:
     * they are process-global and survive a restart (gmr only inits once),
     * so a stale device pointer would be used on the next event. */
    upnp_transport_deinit();
    upnp_control_deinit();

    /* Unregisters the device and calls UpnpFinish(), which joins the
     * libupnp worker threads (miniserver, SSDP, timer). */
    upnp_device_shutdown(dlna_device);
    dlna_device = NULL;

    /* Stops the stream and joins the monitor thread. */
    dlna_output_shutdown();
}

/* ------------------------------------------------------------------ */
/* status screen                                                       */
/* ------------------------------------------------------------------ */

static const char *dlna_state_name(enum dlna_stream_state state)
{
    switch (state)
    {
        case DLNA_STREAM_CONNECTING: return str(LANG_NETFM_ST_CONNECTING);
        case DLNA_STREAM_BUFFERING: return str(LANG_NETFM_ST_BUFFERING);
        case DLNA_STREAM_PLAYING: return str(LANG_NETFM_ST_PLAYING);
        case DLNA_STREAM_DISCONNECTED: return str(LANG_NETFM_ST_DISCONNECTED);
        case DLNA_STREAM_ERROR: return str(LANG_NETFM_ST_ERROR);
        /* no stream yet, or a pushed file played to its end without a
         * queued next one: the renderer is up, waiting for the phone */
        default: return str(LANG_DLNA_IDLE);
    }
}

static const char *dlna_row_cb(int selected, void *data,
                               char *buffer, size_t size)
{
    struct dlna_stream_status *status = data;
    if (!status)
        return "";
    switch (selected)
    {
        case 0:
            snprintf(buffer, size, "%s: %s", str(LANG_DLNA_STATUS),
                     dlna_state_name(status->state));
            break;
        case 1:
            snprintf(buffer, size, "%s: %.80s", str(LANG_DLNA_TRACK),
                     status->name);
            break;
        case 2:
            snprintf(buffer, size, "%s: %.12s", str(LANG_DLNA_FORMAT),
                     status->format);
            break;
        case 3:
            snprintf(buffer, size, "%s: %d kbps", str(LANG_DLNA_BITRATE),
                     status->bitrate);
            break;
        case 4:
            snprintf(buffer, size, "%s: %d%%", str(LANG_DLNA_BUFFER),
                     status->buffer_percent);
            break;
        default:
            snprintf(buffer, size, "%s", str(LANG_DLNA_EXIT));
            break;
    }
    return buffer;
}

static int dlna_action_cb(int action, struct gui_synclist *lists)
{
    static char previous[DLNA_ROWS][96];
    struct dlna_stream_status *status = lists->data;
    char current[96];
    bool changed = false;

    if (dlna_stream_codec_pending())
        dlna_stream_codec_start_now();

    /* the wheel adjusts the shared system volume, like netfm */
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
        /* only the last row (stop) acts; the stop itself happens after
         * the list exits, so the teardown cannot fight the redraw */
        if (gui_synclist_get_sel_pos(lists) == DLNA_ROWS - 1)
            return ACTION_STD_CANCEL;
        return ACTION_NONE;
    }
    if (action != ACTION_NONE || !status)
        return action;

    dlna_stream_get_status(status);

    for (int i = 0; i < DLNA_ROWS; i++)
    {
        dlna_row_cb(i, status, current, sizeof(current));
        if (strcmp(previous[i], current) != 0)
            changed = true;
    }
    if (!changed)
        return ACTION_NONE;
    for (int i = 0; i < DLNA_ROWS; i++)
        dlna_row_cb(i, status, previous[i], sizeof(previous[i]));
    return ACTION_REDRAW;
}

int dlna_menu(void)
{
    struct dlna_stream_status status;
    struct simplelist_info info;

    if (!wifi_hal_is_up())
    {
        splash(HZ * 2, str(LANG_NETFM_WIFI_REQUIRED));
        return 0;
    }

    /* Entering this screen takes the output: refuse the hard inputs, stop
     * the radio and stop local playback.  This runs on EVERY entry, also
     * when the renderer itself is already running in the background - the
     * previous renderer-running guard used to skip the takeover and left
     * the radio playing under the DLNA stream. */
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
    /* the USB DAC is a hard input and cannot be preempted */
    if (usb_audio_get_active())
    {
        splash(HZ * 2, str(LANG_USB_DAC_ACTIVE));
        return 0;
    }
#endif
#ifdef HAVE_BT_INPUT
    /* The bluetooth receive path is a hard input: refuse, never
     * force-stop it (that would touch its PCM channel). */
    if (bt_input_active())
    {
        splash(HZ * 2, str(LANG_NETFM_CONFLICT));
        return 0;
    }
#endif
#ifdef HAVE_NETFM
    if (netfm_stream_is_active())
        netfm_stream_stop();
#endif
    if (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
    {
        audio_stop();
        /* the stop is asynchronous - let it finish so its teardown
         * cannot stop the output channel the stream is about to use */
        for (int i = 0; i < 50 &&
             (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)); i++)
            sleep(HZ / 50);
    }

    if (!dlna_renderer_active())
    {
        if (!dlna_renderer_start())
        {
            splash(HZ * 2, str(LANG_NETFM_START_FAIL));
            return 0;
        }
    }

    dlna_stream_get_status(&status);
    simplelist_info_init(&info, str(LANG_DLNA), DLNA_ROWS, &status);
    info.get_name = dlna_row_cb;
    info.action_callback = dlna_action_cb;
    info.timeout = HZ * 2;
    info.selection = DLNA_ROWS - 1;
    info.title_icon = Icon_Submenu;
    simplelist_show_list(&info);

    if (info.selection == DLNA_ROWS - 1)
    {
        /* the exit row: renderer, stream and all threads stop here */
        dlna_renderer_stop();
    }
    /* Back: the renderer (and any playing stream) keeps running in the
     * background, like the netfm radio and the bluetooth input screen */
    return 0;
}

#endif /* HAVE_DLNA */
