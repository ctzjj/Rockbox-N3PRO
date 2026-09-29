/***************************************************************************
 * N3Pro USB DAC output routing (hosted ALSA backend).
 *
 * When a USB sound card appears on the OTG port the playback PCM is
 * re-opened on it automatically: kernel uevents drive the switch, the
 * executor thread performs it (see n3pro-usb-out.c), and the shared
 * pcm-alsa.c backend learns about the device through the hooks in
 * n3pro-usb-pcm-hooks.h -- the exact pattern the bluetooth output
 * route established (n3pro-bt-pcm.h / n3pro-bt-pcm-hooks.h).
 *
 * Routing table (output devices only; no input modes take part):
 *   INT  --usb add-->            USB
 *   USB  --usb remove-->         INT
 *   INT/USB --bt earphones-->    BT   (bluetooth preempts, existing logic)
 *   BT   --bt gone-->            wired() = USB when present, else INT
 *   USB  --pcm DISCONNECTED-->   INT   (pump fallback, uevent lost)
 *   boot with DAC plugged:       USB
 ****************************************************************************/
#ifndef __N3PRO_USB_PCM_H__
#define __N3PRO_USB_PCM_H__

#include <stdbool.h>
#include <stddef.h>

/* Name of the generated playback PCM entry: a plug->softvol wrapper
 * around the discovered card, written to N3PRO_USB_CONF (our own file;
 * the vendor /etc/asound.conf belongs to the bluetooth route). */
#define N3PRO_USB_DEVICE   "usbvol"
#define N3PRO_USB_CONF     "/etc/asound-usb.conf"

/* Internal wired output when no USB card is around (same literal the
 * bluetooth route uses as its local device). */
#define N3PRO_INT_DEVICE   "plughw:0,0"

/* Find the first "USB-Audio" card in /proc/asound/cards and copy its
 * ALSA card id (the bracketed name, e.g. "YSHIFIF801").  Card numbers
 * are unstable across replugs, so every device string is built from the
 * id.  Returns false when no USB card is present. */
bool n3pro_usb_card_discover(char *id, size_t idsz);

/* (Re)write N3PRO_USB_CONF so pcm.!usbvol wraps plughw:CARD=<id>.
 * Skips the write when the content already matches; returns false when
 * the file cannot be written (the route then stays away from USB). */
bool n3pro_usb_write_conf(const char *card_id);

/* Returns 0 when the generated usbvol PCM opens right now. */
int pcm_alsa_usb_probe(void);

/* True while the engine is routed to the USB output. */
bool pcm_alsa_is_usb_active(void);

/* Set by the data pump when the USB PCM dies under us (card pulled);
 * the executor picks this up and falls back to the internal card. */
bool pcm_alsa_usb_link_lost(void);
void pcm_alsa_usb_link_lost_clear(void);

/* Wired-output resolution: N3PRO_USB_DEVICE when a USB card is present
 * and its configuration is writable, else N3PRO_INT_DEVICE.  Used by
 * the bluetooth route when it falls back to the wired output. */
const char *n3pro_usb_wired_device(void);

/* Volume-domain management.  The USB route keeps an independent level
 * (global_settings.usb_volume) exactly like the bluetooth route; this
 * puts the hardware-volume level back before bluetooth takes over. */
void n3pro_usb_vol_leave(void);

/* Enter the USB volume domain (and adopt the active card when the route
 * was switched by the bluetooth fallback rather than the engine). */
void n3pro_usb_vol_enter(void);

/* Start the uevent listener and the routing executor (once).  Called
 * from the pcm backend after the first successful open. */
void n3pro_usb_out_start(void);

#endif /* __N3PRO_USB_PCM_H__ */
