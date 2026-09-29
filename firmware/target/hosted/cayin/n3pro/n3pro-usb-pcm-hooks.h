/***************************************************************************
 * N3Pro USB DAC routing hooks for the shared hosted ALSA backend.
 *
 * Included directly by firmware/target/hosted/pcm-alsa.c for
 * CAYIN_N3PRO, right after n3pro-bt-pcm-hooks.h (same pattern):
 *
 *   - open_hwdev() recognises the generated usbvol PCM and opens it
 *     against a private config tree parsed from N3PRO_USB_CONF, so the
 *     file can be rewritten at runtime (alsa-lib caches the global
 *     config, exactly like the bluetooth route);
 *   - the pump marks the link lost when the USB PCM dies under us;
 *   - after the first successful open the routing engine in
 *     n3pro-usb-out.c is started (executor + uevent listener).
 ****************************************************************************/
#ifndef __N3PRO_USB_PCM_HOOKS_H__
#define __N3PRO_USB_PCM_HOOKS_H__

#include "n3pro-usb-pcm.h"

static bool n3pro_pcm_is_usb_device(const char *device)
{
    return device && !strcmp(device, N3PRO_USB_DEVICE);
}

/* Open the usbvol PCM through a private config tree parsed from our own
 * alsa configuration file (kept beside the bt tree, never freed while a
 * handle references it). */
static snd_config_t *n3pro_usb_cfg = NULL;

static int n3pro_usb_open(snd_pcm_t **pcm, snd_pcm_stream_t mode)
{
    snd_config_t *top = NULL;
    snd_input_t *in = NULL;
    int err;

    if (n3pro_usb_cfg)
    {
        snd_config_delete(n3pro_usb_cfg);
        n3pro_usb_cfg = NULL;
    }

    err = snd_config_top(&top);
    if (err >= 0)
        err = snd_input_stdio_open(&in, N3PRO_USB_CONF, "r");
    if (err >= 0)
    {
        err = snd_config_load(top, in);
        snd_input_close(in);
    }
    if (err >= 0)
        err = snd_pcm_open_lconf(pcm, N3PRO_USB_DEVICE, mode, 0, top);

    if (err >= 0)
        n3pro_usb_cfg = top;
    else if (top)
        snd_config_delete(top);

    return err;
}

/* Whether the engine is currently routed to the USB output. */
bool pcm_alsa_is_usb_active(void)
{
    return current_alsa_device &&
           !strcmp(current_alsa_device, N3PRO_USB_DEVICE);
}

/* Readiness probe: open + close the generated PCM. */
int pcm_alsa_usb_probe(void)
{
    snd_pcm_t *pcm = NULL;
    int err = n3pro_usb_open(&pcm, SND_PCM_STREAM_PLAYBACK);

    if (err >= 0)
        snd_pcm_close(pcm);

    return err;
}

/* Raised by the pump when the USB PCM dies under us (card pulled while
 * the uevent was missed); the executor acts on it. */
static volatile bool n3pro_usb_link_lost_flag = false;

static void n3pro_usb_mark_link_lost(void)
{
    n3pro_usb_link_lost_flag = true;
}

bool pcm_alsa_usb_link_lost(void)
{
    return n3pro_usb_link_lost_flag;
}

void pcm_alsa_usb_link_lost_clear(void)
{
    n3pro_usb_link_lost_flag = false;
}

#endif /* __N3PRO_USB_PCM_HOOKS_H__ */
