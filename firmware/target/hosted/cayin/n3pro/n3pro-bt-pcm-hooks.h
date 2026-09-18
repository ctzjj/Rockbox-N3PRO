/***************************************************************************
 * N3Pro dynamic PCM routing hooks for the shared hosted ALSA backend.
 *
 * Included directly by firmware/target/hosted/pcm-alsa.c for CAYIN_N3PRO,
 * following the same pattern as the HiBy R1 port's pcm-alsa-hiby-hooks.h:
 *
 *   - a single poll thread pumps the playback PCM (the stock HiByOS
 *     bluetooth plugin has no SIGIO support, and running both a SIGIO
 *     handler and the poll thread races the kernel PCM wait queues);
 *   - pcm_alsa_switch_playback_device() re-opens the PCM on another
 *     device from the menu thread while Rockbox keeps running;
 *   - the bluetooth PCM is opened against a private config tree parsed
 *     from /etc/asound.conf, which the vendor player rewrites at runtime
 *     (alsa-lib caches the global config, so the file alone is not enough).
 ****************************************************************************/
#ifndef __N3PRO_BT_PCM_HOOKS_H__
#define __N3PRO_BT_PCM_HOOKS_H__

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "n3pro-bt-pcm.h"

/* Defined later in pcm-alsa.c. */
static void open_hwdev(const char *device, snd_pcm_stream_t mode);
static void pcm_pump_locked(snd_pcm_t *h);
static int set_hwparams(snd_pcm_t *handle, unsigned long sampr);
static int set_swparams(snd_pcm_t *handle);

/* Name of the "type bluetooth" PCM entry the vendor player writes into
 * /etc/asound.conf lives in n3pro-bt-pcm.h (N3PRO_BT_DEVICE). */

/* The plugin talks to bluetoothd over D-Bus, so the pump only has to be
 * fast enough to keep its buffer topped up. */
#define N3PRO_POLL_INTERVAL_US 4000

static pthread_t n3pro_poll_thread;
static volatile bool n3pro_poll_run = false;
static bool n3pro_poll_started = false;
static pthread_mutex_t n3pro_poll_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool n3pro_mtx_init = false;
/* Config tree backing the currently open bluetooth handle, and a separate
 * one for the (handle-less) readiness probe so probing never frees the
 * tree a live handle still references. */
static snd_config_t *n3pro_bt_cfg = NULL;
static snd_config_t *n3pro_probe_cfg = NULL;

static void n3pro_pcm_mutex_init_once(void)
{
    pthread_mutexattr_t attr;

    if (n3pro_mtx_init)
        return;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&pcm_mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    n3pro_mtx_init = true;
}

static bool n3pro_pcm_is_bt_device(const char *device)
{
    return device && !strcmp(device, N3PRO_BT_DEVICE);
}

/* Open the bluetooth PCM through a private config tree parsed from
 * /etc/asound.conf.  The tree is kept alive in *slot for as long as the
 * handle references it. */
static int n3pro_bt_open_ex(snd_pcm_t **pcm, snd_pcm_stream_t mode,
                            snd_config_t **slot)
{
    snd_config_t *top = NULL;
    snd_input_t *in = NULL;
    int err;

    if (*slot)
    {
        snd_config_delete(*slot);
        *slot = NULL;
    }

    err = snd_config_top(&top);
    if (err >= 0)
        err = snd_input_stdio_open(&in, "/etc/asound.conf", "r");
    if (err >= 0)
    {
        err = snd_config_load(top, in);
        snd_input_close(in);
    }
    if (err >= 0)
        err = snd_pcm_open_lconf(pcm, N3PRO_BT_DEVICE, mode,
                                 SND_PCM_NONBLOCK, top);

    if (err >= 0)
        *slot = top;
    else if (top)
        snd_config_delete(top);

    return err;
}

static int n3pro_bt_open(snd_pcm_t **pcm, snd_pcm_stream_t mode)
{
    return n3pro_bt_open_ex(pcm, mode, &n3pro_bt_cfg);
}

/* Non-destructive readiness probe: the plugin refuses the open with -240
 * until the A2DP transport is really up, so the caller retries this until
 * it succeeds and only then switches the engine. */
int pcm_alsa_bt_probe(void)
{
    snd_pcm_t *pcm = NULL;
    int err = n3pro_bt_open_ex(&pcm, SND_PCM_STREAM_PLAYBACK, &n3pro_probe_cfg);

    if (err >= 0)
        snd_pcm_close(pcm);

    return err;
}

/* Whether the engine is currently routed to the bluetooth output. */
bool pcm_alsa_is_bluetooth_active(void)
{
    return current_alsa_device &&
           !strcmp(current_alsa_device, N3PRO_BT_DEVICE);
}

/* Raised by the pump when the bluetooth PCM dies under us (earpieces
 * switched off, transport torn down); the menu watchdog acts on it. */
static volatile bool n3pro_bt_link_lost = false;

static void n3pro_bt_mark_link_lost(const char *why)
{
    (void)why;
    n3pro_bt_link_lost = true;
}

bool pcm_alsa_bt_link_lost(void)
{
    return n3pro_bt_link_lost;
}

void pcm_alsa_bt_link_lost_clear(void)
{
    n3pro_bt_link_lost = false;
}

/* Re-use the current handle when the same device is requested and it is
 * still alive (matches the R1 port's hiby_pcm_keep_hwdev()). */
static bool n3pro_pcm_keep_hwdev(const char *device, snd_pcm_stream_t mode)
{
    bool keep;
    (void)mode;

    keep = handle && current_alsa_device &&
           !strcmp(current_alsa_device, device) &&
           snd_pcm_state(handle) != SND_PCM_STATE_DISCONNECTED;

    return keep;
}

/* Re-apply the current sample rate on a freshly opened handle (called with
 * pcm_mtx held, from the switch). */
static int n3pro_pcm_reapply_nolock(unsigned int rate)
{
    int err;

    if (!handle || rate == 0)
        return -1;

    snd_pcm_drop(handle);
    last_sample_rate = rate;
    err = set_hwparams(handle, rate);
    if (err < 0)
        return err;
    set_swparams(handle);
    return 0;
}

static void *n3pro_poll_thread_fn(void *arg)
{
    (void)arg;

    while (n3pro_poll_run)
    {
        snd_pcm_t *h = handle;

        if (h && pthread_mutex_trylock(&pcm_mtx) == 0)
        {
            pcm_pump_locked(h);
            pthread_mutex_unlock(&pcm_mtx);
        }

        usleep(N3PRO_POLL_INTERVAL_US);
    }

    return NULL;
}

/* Stop (and join) the pump before the handle is closed: the poll thread
 * reads the global handle without the lock, so it must not be running
 * while snd_pcm_close() frees it.  Re-started by n3pro_pcm_after_open(). */
static void n3pro_pcm_stop_poll(void)
{
    pthread_mutex_lock(&n3pro_poll_mtx);

    if (n3pro_poll_started)
    {
        n3pro_poll_run = false;
        pthread_join(n3pro_poll_thread, NULL);
        n3pro_poll_started = false;
    }

    pthread_mutex_unlock(&n3pro_poll_mtx);
}

/* Start the pump once, for the whole runtime. */
static void n3pro_pcm_after_open(void)
{
    n3pro_pcm_mutex_init_once();
    pthread_mutex_lock(&n3pro_poll_mtx);

    if (!n3pro_poll_started)
    {
        n3pro_poll_run = true;
        if (pthread_create(&n3pro_poll_thread, NULL, n3pro_poll_thread_fn, NULL) == 0)
            n3pro_poll_started = true;
        else
            n3pro_poll_run = false;
    }

    pthread_mutex_unlock(&n3pro_poll_mtx);
}

int pcm_alsa_switch_playback_device(const char *device)
{
    unsigned int rate;
    bool ok;
    int err;

    if (!device || !*device)
        return -1;

    n3pro_pcm_mutex_init_once();
    pthread_mutex_lock(&pcm_mtx);

    /* open_hwdev() resets last_sample_rate; keep the rate so the new
     * handle can be reconfigured in place. */
    rate = last_sample_rate ? last_sample_rate : real_sample_rate;

    open_hwdev(device, SND_PCM_STREAM_PLAYBACK);
    err = n3pro_pcm_reapply_nolock(rate);

    ok = err == 0 && handle != NULL && current_alsa_device &&
         !strcmp(current_alsa_device, device);

    pthread_mutex_unlock(&pcm_mtx);
    return ok ? 0 : -1;
}

/* Positive "plain local file playback" check (see n3pro-bt-pcm.h):
 * false whenever any external mode owns the audio path.  New external
 * modes MUST be wired in here. */
bool n3pro_local_playback(void)
{
    extern bool bt_input_active(void);

    if (pcm_alsa_is_bluetooth_active())
        return false;
    if (bt_input_active())
        return false;
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
    {
        extern bool usb_audio_get_active(void);
        if (usb_audio_get_active())
            return false;
    }
#endif
    return true;
}

#endif /* __N3PRO_BT_PCM_HOOKS_H__ */
