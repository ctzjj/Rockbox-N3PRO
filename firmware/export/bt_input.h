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

#ifndef __BT_INPUT_H__
#define __BT_INPUT_H__

#include <stdbool.h>
#include <stddef.h>

/**
 * Bluetooth A2DP receive (sink) input.
 *
 * The target driver owns the pump that drains the decoded phone stream
 * into the mixer and runs it through the DSP chain.  While it is active
 * it owns the output path, so shared code must not start local playback
 * or the network radio then.  A target implements these hooks when it
 * defines HAVE_BT_INPUT.
 */
bool bt_input_start(void);
void bt_input_stop(void);

/* True while the receive pump is running (screen lifetime). */
bool bt_input_active(void);

/* Marks whether the receive screen is open. The pump only retries for
 * the phone while fg is set; in the background a lost or missing link
 * stops it instead of spinning idle. */
void bt_input_set_fg(bool fg);

/* True while the capture transport is up, false while waiting for the
 * peer (reconnect loop). */
bool bt_input_link_ok(void);

/* Full status for a receive view.  WAITING = never connected since the
 * pump started; DISCONNECTED = was connected, peer dropped. */
enum bt_input_state
{
    BT_INPUT_IDLE = 0,
    BT_INPUT_WAITING,
    BT_INPUT_CONNECTED,
    BT_INPUT_DISCONNECTED,
};
enum bt_input_state bt_input_get_state(void);

/* Last peer BD_ADDR seen ("" until one links up). */
void bt_input_get_peer(char *buf, size_t len);

/* Negotiated stream rate in Hz (0 while not up). */
int bt_input_get_rate(void);

/* Ring fill in ms (activity/debug indicator). */
int bt_input_get_fill_ms(void);

#endif /* __BT_INPUT_H__ */
