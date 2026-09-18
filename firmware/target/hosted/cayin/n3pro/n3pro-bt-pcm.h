/***************************************************************************
 * N3Pro dynamic PCM output routing (hosted ALSA backend)
 *
 * The stock HiByOS ALSA plugin (libasound_module_pcm_bluetooth.so) plays
 * A2DP through an ordinary "type bluetooth" PCM whose peer address is
 * written to /etc/asound.conf at runtime.  This is the N3Pro counterpart
 * of the HiBy R1 port's pcm-alsa-hiby helper: the playback PCM can be
 * re-opened against a different device while Rockbox keeps running.
 ****************************************************************************/
#ifndef __N3PRO_BT_PCM_H__
#define __N3PRO_BT_PCM_H__

#include <stdbool.h>

/* Name of the playback PCM entry written into /etc/asound.conf at runtime.
 * It is a "softvol" wrapper around the stock "type bluetooth" PCM so the
 * volume buttons can attenuate the digital stream (the AK4493 hardware
 * volume does not affect the bluetooth path).  The wrapped entry itself is
 * still named pcm.bluetooth. */
#define N3PRO_BT_DEVICE "btvol"

/* Re-open the playback PCM on the given ALSA device.  Runs on the caller's
 * thread (menu thread only).  Returns 0 on success, <0 on failure. */
int pcm_alsa_switch_playback_device(const char *device);

/* Returns 0 when the stock bluetooth PCM can be opened right now (the
 * A2DP transport is up); <0 while it is still coming up.  Does not change
 * the active output. */
int pcm_alsa_bt_probe(void);

/* True while the engine is routed to the bluetooth output. */
bool pcm_alsa_is_bluetooth_active(void);

/* Set by the data pump when the bluetooth link drops while the engine is
 * routed there (the stock plugin reports a dead PCM).  The menu watchdog
 * picks this up and falls back to the wired output so playback continues. */
bool pcm_alsa_bt_link_lost(void);
void pcm_alsa_bt_link_lost_clear(void);

/* Positive "plain local file playback" check.  True ONLY when the normal
 * file player owns the whole audio path; false whenever any external mode
 * is active (bluetooth receive, bluetooth output route, USB DAC input).
 * This is the single choke point for such queries: when a new external
 * playback mode is added to the port, it MUST be wired in here so that
 * dependants (e.g. the web remote, which drives the file player only)
 * refuse to run. */
bool n3pro_local_playback(void);

#endif /* __N3PRO_BT_PCM_H__ */
