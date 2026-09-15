/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware  |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by the Rockbox-N3PRO contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software, either version 2 of the
 * License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef __N3PRO_BT_INPUT_H__
#define __N3PRO_BT_INPUT_H__

#include <stdbool.h>

/**
 * Bluetooth A2DP receive pump (Cayin N3Pro hosted port).
 *
 * The vendor bluetoothd registers the A2DP sink role; the ALSA bluetooth
 * plugin delivers the decoded phone stream in capture mode. The pump
 * drains it into the PCM_MIXER_CHAN_BT_AUDIO mixer channel, running the
 * CODEC_IDX_AUDIO DSP chain so EQ and the Sound settings apply.
 *
 * n3pro_bt_rx_start() returns false if local playback currently owns
 * the output. While receiving, audiohw_set_volume() pins the wired
 * output wide open (the phone is the volume control); the user's own
 * volume setting is never modified, so any exit path is safe.
 */
bool n3pro_bt_rx_start(void);
void n3pro_bt_rx_stop(void);

/* True while the receive pump is running (screen lifetime). */
bool n3pro_bt_rx_get_active(void);

/* Marks whether the receive screen is open. The pump only retries for
 * the phone while fg is set; in the background a lost or missing link
 * stops it (and the mixer channel) instead of spinning idle. */
void n3pro_bt_rx_set_fg(bool fg);

/* True while the capture transport is up, false while waiting for the
 * peer (reconnect loop). */
bool n3pro_bt_rx_link_ok(void);

/* Full status for the receive view. WAITING = never connected since the
 * pump started; DISCONNECTED = was connected, peer dropped. */
enum n3pro_bt_rx_state
{
    N3PRO_BT_RX_IDLE = 0,
    N3PRO_BT_RX_WAITING,
    N3PRO_BT_RX_CONNECTED,
    N3PRO_BT_RX_DISCONNECTED,
};
enum n3pro_bt_rx_state n3pro_bt_rx_get_state(void);

/* Last peer BD_ADDR seen ("" until one links up). */
void n3pro_bt_rx_get_peer(char *buf, size_t len);

/* Negotiated stream rate in Hz (0 while not up). */
int n3pro_bt_rx_get_rate(void);

/* Ring fill in ms (activity/debug indicator). */
int n3pro_bt_rx_get_fill_ms(void);

#endif /* __N3PRO_BT_INPUT_H__ */
