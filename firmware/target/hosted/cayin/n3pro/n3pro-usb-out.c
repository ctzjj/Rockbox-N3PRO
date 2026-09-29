/***************************************************************************
 * N3Pro USB DAC output routing engine.
 *
 * Two threads cooperate (see n3pro-usb-pcm.h for the routing table):
 *
 *   - a raw pthread blocks on the kernel uevent multicast socket and
 *     only bumps a sequence counter when a sound-device add/remove was
 *     seen -- it never touches Rockbox APIs;
 *   - a Rockbox thread sleeps in HZ/4 beats, picks the counter up and
 *     performs the actual route change (pause -> re-open -> resume),
 *     mirroring the bluetooth route's watchdog context so the pcm
 *     switch stays on a non-audio thread, as pcm-alsa requires.
 *
 * The route change itself goes through pcm_alsa_switch_playback_device()
 * exactly like the bluetooth route; the volume domain swap mirrors
 * bt_vol_swap() in n3pro-bluetooth.c.
 ****************************************************************************/
#include "config.h"

#ifdef CAYIN_N3PRO

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#include "kernel.h"
#include "thread.h"
#include "audio.h"
#include "button.h"
#include "splash.h"
#include "settings.h"
#include "sound.h"
#include "pcm-alsa.h"
#include "logf.h"
#include "n3pro-usb-pcm.h"

/* ------------------------------------------------------------------ */
/* card discovery + alsa configuration                                 */
/* ------------------------------------------------------------------ */

bool n3pro_usb_card_discover(char *id, size_t idsz)
{
    FILE *f = fopen("/proc/asound/cards", "r");
    char line[256];

    if (!f || !id || idsz == 0)
    {
        if (f)
            fclose(f);
        return false;
    }

    while (fgets(line, sizeof(line), f))
    {
        /* "  3 [YSHIFIF801     ]: USB-Audio - YSHIFI-F801" -- the
         * bracketed field is fixed width, so the trailing padding has
         * to be trimmed or the card name never resolves. */
        char *b = strchr(line, '[');
        char *e = b ? strchr(b, ']') : NULL;
        if (b && e && strstr(line, "]: USB-Audio"))
        {
            size_t n, s;

            snprintf(id, idsz, "%.*s", (int)(e - b - 1), b + 1);

            /* the bracketed field is fixed width: trim the padding */
            n = strlen(id);
            while (n > 0 && id[n - 1] == ' ')
                id[--n] = '\0';
            s = 0;
            while (id[s] == ' ')
                s++;
            if (s > 0)
                memmove(id, id + s, strlen(id + s) + 1);

            fclose(f);
            return id[0] != '\0';
        }
    }
    fclose(f);
    return false;
}

bool n3pro_usb_write_conf(const char *card_id)
{
    char buf[512];
    /* The whole chain is built from plugin TYPES: the handle is opened
     * against a private config tree that does not carry the standard
     * pcm.* definitions, so named PCMs like "plughw:CARD=x" would not
     * resolve.  The hw plugin takes the card id by name.  Volume is NOT
     * wrapped in a softvol here: the shared 32-bit digital gain in
     * pcm-alsa carries the USB level (see hibylinux_codec.c). */
    int len = snprintf(buf, sizeof(buf),
        "pcm.%s {\n"
        "    type plug\n"
        "    slave.pcm {\n"
        "        type hw\n"
        "        card %s\n"
        "    }\n"
        "}\n", N3PRO_USB_DEVICE, card_id);

    if (len <= 0 || len >= (int)sizeof(buf))
        return false;

    FILE *f = fopen(N3PRO_USB_CONF, "r");
    if (f)
    {
        char old[512];
        size_t got = fread(old, 1, sizeof(old) - 1, f);
        fclose(f);
        old[got] = '\0';
        if (strcmp(old, buf) == 0)
            return true;               /* unchanged */
    }

    f = fopen(N3PRO_USB_CONF, "w");
    if (!f)
        return false;
    fputs(buf, f);
    fclose(f);
    return true;
}

const char *n3pro_usb_wired_device(void)
{
    char id[32];
    if (n3pro_usb_card_discover(id, sizeof(id)) &&
        n3pro_usb_write_conf(id))
        return N3PRO_USB_DEVICE;
    return N3PRO_INT_DEVICE;
}

/* ------------------------------------------------------------------ */
/* volume domain (mirrors bt_vol_swap in n3pro-bluetooth.c)            */
/* ------------------------------------------------------------------ */

/* ALSA card id the active USB route was opened for ("" = not routed).
 * keep_hwdev() would keep the old handle alive on a same-name request,
 * so a card swap must bounce through the internal device once. */
static char usb_routed_id[32] = "";

static bool usb_vol_on = false;

static void usb_vol_swap(void)
{
    int t;

    if (!global_settings.usb_volume_set)
    {
        global_settings.usb_volume = global_status.volume;
        global_settings.usb_volume_set = true;
    }

    t = global_status.volume;
    sound_set_volume(global_settings.usb_volume);
    global_settings.usb_volume = t;
    settings_save();
}

static void usb_vol_enter_usb(void)
{
    if (usb_vol_on)
        return;
    usb_vol_swap();
    usb_vol_on = true;
}

static void usb_vol_enter_local(void)
{
    if (!usb_vol_on)
        return;
    usb_vol_swap();
    usb_vol_on = false;
}

void n3pro_usb_vol_leave(void)
{
    usb_vol_enter_local();
}

/* Public entry for the bluetooth route when it falls back onto the USB
 * output: enter the domain AND adopt the active card into the engine's
 * retarget bookkeeping (the switch happened outside the engine). */
void n3pro_usb_vol_enter(void)
{
    usb_vol_enter_usb();

    if (pcm_alsa_is_usb_active() && usb_routed_id[0] == '\0')
    {
        char id[32];
        if (n3pro_usb_card_discover(id, sizeof(id)))
            snprintf(usb_routed_id, sizeof(usb_routed_id), "%s", id);
    }
}

/* ------------------------------------------------------------------ */
/* route changes                                                       */
/* ------------------------------------------------------------------ */

static bool usb_pause_if_playing(void)
{
    int status = audio_status();
    if ((status & AUDIO_STATUS_PLAY) && !(status & AUDIO_STATUS_PAUSE))
    {
        audio_pause();
        sleep(HZ / 4);
        return true;
    }
    return false;
}

static void usb_route_to_internal(bool show_message)
{
    bool was_playing = usb_pause_if_playing();

    pcm_alsa_switch_playback_device(N3PRO_INT_DEVICE);
    usb_routed_id[0] = '\0';
    usb_vol_enter_local();

    if (was_playing)
    {
        /* Internal output needs a load: the jack state is real again now
         * that the USB override is off, so only resume when something is
         * plugged -- otherwise stay paused. */
        if (headphones_inserted())
            audio_resume();
    }

    if (show_message)
        splash(HZ, "Output: Local");
}

/* Take the discovered card up.  When the USB route is already active on
 * ANOTHER card, bounce through the internal device first so the usbvol
 * PCM is really re-opened against the rewritten configuration. */
static void usb_route_to_card(const char *id)
{
    bool was_playing;
    bool retarget = pcm_alsa_is_usb_active() &&
                    strcmp(usb_routed_id, id) != 0;

    if (!n3pro_usb_write_conf(id))
        return;

    was_playing = usb_pause_if_playing();

    if (retarget)
    {
        pcm_alsa_switch_playback_device(N3PRO_INT_DEVICE);
        usb_vol_enter_local();
    }

    if (pcm_alsa_switch_playback_device(N3PRO_USB_DEVICE) == 0)
    {
        snprintf(usb_routed_id, sizeof(usb_routed_id), "%s", id);
        usb_vol_enter_usb();
        if (was_playing)
            audio_resume();
        logf("usb out: routed to card %s", id);
        splash(HZ, "Output: USB DAC");
    }
    else
    {
        /* Could not open the card after all: put the internal device
         * back and stay honest about the route. */
        logf("usb out: open failed for card %s", id);
        pcm_alsa_switch_playback_device(N3PRO_INT_DEVICE);
        usb_routed_id[0] = '\0';
        usb_vol_enter_local();
        if (was_playing)
            audio_resume();
        splash(HZ, "USB DAC open failed");
    }
}

/* ------------------------------------------------------------------ */
/* uevent listener (raw pthread: only bumps the sequence counter)      */
/* ------------------------------------------------------------------ */

static volatile unsigned usb_evt_seq = 0;

static void *usb_uevent_thread(void *arg)
{
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
    char buf[4096];

    (void)arg;

    int s = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (s < 0)
        return NULL;
    if (bind(s, (void *)&sa, sizeof(sa)) < 0)
    {
        close(s);
        return NULL;
    }

    while (true)
    {
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        /* first token: ACTION@DEVPATH */
        if (strstr(buf, "/sound/card"))
            usb_evt_seq++;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* executor (rockbox thread)                                           */
/* ------------------------------------------------------------------ */

static void usb_route_executor(void)
{
    unsigned handled_seq = usb_evt_seq;
    long start_tick = current_tick;

    /* Let the freshly opened playback engine settle before the first
     * route decision. */
    while (TIME_BEFORE(current_tick, start_tick + 2 * HZ))
        sleep(HZ / 4);

    /* Initial decision: a DAC that was already plugged when this thread
     * started had its uevents emitted long before the socket existed. */
    if (!pcm_alsa_is_bluetooth_active())
    {
        char id[32];
        if (n3pro_usb_card_discover(id, sizeof(id)) &&
            !pcm_alsa_is_usb_active())
            usb_route_to_card(id);
    }

    while (true)
    {
        sleep(HZ / 4);

        unsigned seq = usb_evt_seq;

        if (seq != handled_seq)
        {
            handled_seq = seq;

            /* Bluetooth owns the output: drop the event; the route
             * back from bluetooth resolves wired() on its own. */
            if (pcm_alsa_is_bluetooth_active())
                continue;

            char id[32];
            bool present = n3pro_usb_card_discover(id, sizeof(id));

            if (present)
            {
                if (!pcm_alsa_is_usb_active() ||
                    strcmp(usb_routed_id, id) != 0)
                    usb_route_to_card(id);
            }
            else if (pcm_alsa_is_usb_active())
                usb_route_to_internal(true);
            else
                pcm_alsa_usb_link_lost_clear();
        }
        else if (pcm_alsa_is_usb_active() && pcm_alsa_usb_link_lost())
        {
            /* the pump saw the PCM die: uevent backup path */
            pcm_alsa_usb_link_lost_clear();
            usb_route_to_internal(true);
        }
    }
}

static long usb_executor_stack[(DEFAULT_STACK_SIZE + 0x1000) / sizeof(long)];
static bool usb_out_started = false;

void n3pro_usb_out_start(void)
{
    pthread_t t;
    pthread_attr_t attr;

    if (usb_out_started)
        return;
    usb_out_started = true;

    if (create_thread(usb_route_executor, usb_executor_stack,
                      sizeof(usb_executor_stack), 0, "usb route"
                      IF_PRIO(, PRIORITY_SYSTEM)
                      IF_COP(, CPU)) <= 0)
        usb_out_started = false;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, usb_uevent_thread, NULL) != 0)
        usb_out_started = false;      /* executor still runs, uevent-less */
    pthread_attr_destroy(&attr);
}

#endif /* CAYIN_N3PRO */
