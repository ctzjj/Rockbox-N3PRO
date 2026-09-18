/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/            \/     \/
 *
 * Copyright (C) 2026 by the Rockbox-N3PRO contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef __BT_AUDIO_H__
#define __BT_AUDIO_H__

#include <stdbool.h>
#include <stddef.h>

/**
 * Bluetooth audio output (A2DP source) driver contract.
 *
 * apps/bluetooth.c is the generic menu/UI framework; a target that defines
 * HAVE_BT_AUDIO provides the device half here, implemented in its own
 * driver directory.  Nothing in this header may name a target.
 */

#define BT_AUDIO_MAC_LEN   18
#define BT_AUDIO_NAME_LEN  80

struct bt_audio_dev
{
    char mac[BT_AUDIO_MAC_LEN];
    char name[BT_AUDIO_NAME_LEN];
    bool paired;
};

/* Bring the stack up; false when unavailable. */
bool bt_audio_prepare(void);

/* Radio powered (drives the statusbar glyph). */
bool bt_audio_stack_on(void);
bool bt_audio_radio_on(void);
void bt_audio_power_off(void);

/* Reset the whole stack to its boot-time state. */
void bt_audio_reset_stack(void);

/* Known peers (paired) and a fresh scan merged into out[]. */
int bt_audio_list(struct bt_audio_dev *out, int max);
int bt_audio_scan(struct bt_audio_dev *out, int count, int max);

/* MAC of the currently linked peer, if any. */
bool bt_audio_current(char *mac, size_t len);
bool bt_audio_linked(const char *mac);

/* Connect to (pairing if needed) and route the output to dev; shows its
 * own progress/failure messages. */
void bt_audio_connect(const struct bt_audio_dev *dev);
void bt_audio_unpair(const char *mac);

/* Full teardown: drop the route, stop the watchdog and power the radio
 * down (shows a message). */
void bt_audio_disconnect(void);

/* Drop the output route and the peer link but keep the radio powered
 * (used when the receive screen takes the radio over). */
void bt_audio_release(void);

/* Drop a specific peer link. */
void bt_audio_disconnect_peer(const char *mac);

/* Active output name / codec for the status view. */
const char *bt_audio_codec(void);
bool bt_audio_output_active(void);

/* Output watchdog (falls back to the wired output on link loss) and the
 * "a menu-driven route is in progress" guard around it. */
void bt_audio_watchdog_start(void);
void bt_audio_watchdog_stop(void);
void bt_audio_busy(bool on);

/* Peer the output route is pinned to ("" when local). */
const char *bt_audio_selected(void);

#endif /* __BT_AUDIO_H__ */
