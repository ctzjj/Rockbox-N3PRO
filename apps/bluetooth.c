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

/* Bluetooth audio - generic menu/UI framework.
 *
 * The target driver half (firmware/export/bt_audio.h, HAVE_BT_AUDIO) runs
 * the stack, lists/connects peers and switches the output PCM; the receive
 * (A2DP sink) side is the generic bt_input_* contract.  This file names no
 * target and must stay usable by any player that defines HAVE_BT_AUDIO. */

#include "config.h"

#ifdef HAVE_BT_AUDIO

#include <stdio.h>
#include <string.h>

#include "kernel.h"
#include "action.h"
#include "audio.h"
#include "menu.h"
#include "splash.h"
#include "lang.h"
#include "sound.h"
#include "screen_access.h"
#include "viewport.h"
#include "gui/list.h"
#include "yesno.h"
#include "bt_audio.h"
#include "bt_input.h"
#include "bluetooth.h"

/* str() yields const unsigned char *; keep the string call sites simple. */
static char *bt_str(int id)
{
    return (char *)str(id);
}

#define BT_MAX_DEVICES 32
#define BT_DEVICE_PICK_CANCEL (-1)
#define BT_DEVICE_PICK_SCAN (-2)

struct bt_device_menu_data
{
    struct bt_audio_dev *devices;
    int count;
    bool include_scan_item;
    char connected_mac[BT_AUDIO_MAC_LEN];
};

static int bt_simplelist_ok_cancel(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    return action;
}

static const char *bt_action_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    static const unsigned short ids[] =
    {
        LANG_BT_STATUS, LANG_BT_DEVICES, LANG_BT_DISCONNECT
    };
    (void)data;

    if (selected_item < 0 || selected_item >= 3)
    {
        buffer[0] = '\0';
        return buffer;
    }
    snprintf(buffer, buffer_len, "%s", bt_str(ids[selected_item]));
    return buffer;
}

static const char *bt_device_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    struct bt_device_menu_data *ctx = data;

    if (ctx->include_scan_item)
    {
        if (selected_item == 0)
        {
            snprintf(buffer, buffer_len, "%s", bt_str(LANG_BT_SCAN));
            return buffer;
        }
        selected_item--;
    }

    if (selected_item < 0 || selected_item >= ctx->count)
    {
        buffer[0] = '\0';
        return buffer;
    }

    if (ctx->connected_mac[0] &&
        !strcasecmp(ctx->devices[selected_item].mac, ctx->connected_mac))
    {
        snprintf(buffer, buffer_len, "%s  %s",
                 ctx->devices[selected_item].name,
                 bt_str(LANG_BT_CONNECTED_MARK));
        return buffer;
    }

    snprintf(buffer, buffer_len, "%s", ctx->devices[selected_item].name);
    return buffer;
}

static int bt_choose_device(const char *title, struct bt_audio_dev *devices,
                            int count, bool include_scan_item)
{
    struct bt_device_menu_data data;
    struct simplelist_info info;
    int total_count = count + (include_scan_item ? 1 : 0);

    if (total_count <= 0)
    {
        splash(HZ, bt_str(LANG_BT_NO_DEVICES));
        return BT_DEVICE_PICK_CANCEL;
    }

    data.devices = devices;
    data.count = count;
    data.include_scan_item = include_scan_item;
    bt_audio_current(data.connected_mac, sizeof(data.connected_mac));

    simplelist_info_init(&info, (char *)title, total_count, &data);
    info.get_name = bt_device_name_cb;
    info.action_callback = bt_simplelist_ok_cancel;
    info.selection = -1;
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);
    if (info.selection < 0 || info.selection >= total_count)
        return BT_DEVICE_PICK_CANCEL;

    if (include_scan_item && info.selection == 0)
        return BT_DEVICE_PICK_SCAN;

    return include_scan_item ? info.selection - 1 : info.selection;
}

/* Per-device action menu: pick Connect or Delete (unpair). */
static const char *bt_device_action_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    static const unsigned short ids[] = { LANG_BT_CONNECT, LANG_BT_DELETE };
    (void)data;

    if (selected_item < 0 || selected_item >= 2)
    {
        buffer[0] = '\0';
        return buffer;
    }

    snprintf(buffer, buffer_len, "%s", bt_str(ids[selected_item]));
    return buffer;
}

static void bt_device_menu(struct bt_audio_dev *devices, int *count, int idx)
{
    struct simplelist_info info;

    simplelist_info_init(&info, (char *)devices[idx].name, 2, NULL);
    info.get_name = bt_device_action_name_cb;
    info.action_callback = bt_simplelist_ok_cancel;
    info.selection = -1;
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);

    if (info.selection == 0)
    {
        bt_audio_connect(&devices[idx]);
    }
    else if (info.selection == 1)
    {
        bt_audio_unpair(devices[idx].mac);
        memmove(&devices[idx], &devices[idx + 1],
                (size_t)(*count - idx - 1) * sizeof(devices[0]));
        (*count)--;
    }
}

static void bt_show_devices(void)
{
    static struct bt_audio_dev devices[BT_MAX_DEVICES];
    int count;
    int idx;

    if (!bt_audio_prepare())
    {
        splash(HZ * 2, bt_str(LANG_BT_UNAVAILABLE));
        return;
    }

    splash(0, bt_str(LANG_BT_LOADING));
    count = bt_audio_list(devices, BT_MAX_DEVICES);
    if (count <= 0)
    {
        splash(0, bt_str(LANG_BT_SCANNING));
        count = bt_audio_scan(devices, count, BT_MAX_DEVICES);
    }

    while (1)
    {
        idx = bt_choose_device("Devices", devices, count, true);
        if (idx == BT_DEVICE_PICK_SCAN)
        {
            splash(0, bt_str(LANG_BT_SCANNING));
            count = bt_audio_scan(devices, count, BT_MAX_DEVICES);
            if (count <= 0)
                splash(HZ, bt_str(LANG_BT_NO_DEVICES));
            continue;
        }

        if (idx < 0 || idx >= count)
            return;

        bt_device_menu(devices, &count, idx);
    }
}

static void bt_show_status(void)
{
    struct simplelist_info info;
    const char *sel = bt_audio_selected();

    simplelist_info_init(&info, bt_str(LANG_BT_STATUS), 0, NULL);
    simplelist_reset_lines();

    if (sel[0])
    {
        simplelist_addline("%s: %s", bt_str(LANG_BT_MAC), sel);
        /* Real link state from the peer's AVRCP input device; opening the
         * PCM just to test readiness would block on the bluetooth stack. */
        simplelist_addline("%s: %s", bt_str(LANG_BT_LINK),
                           bt_audio_linked(sel)
                               ? bt_str(LANG_ON) : bt_str(LANG_OFF));
        simplelist_addline("%s: %s", bt_str(LANG_BT_CODEC), bt_audio_codec());
    }
    else
    {
        simplelist_addline("%s", bt_str(LANG_BT_DEVICE_LOCAL));
    }

    simplelist_addline("%s: %s", bt_str(LANG_BT_OUTPUT),
                       bt_audio_output_active()
                           ? bt_str(LANG_BLUETOOTH) : bt_str(LANG_BT_LOCAL));
    simplelist_addline("%s: %s", bt_str(LANG_BT_RADIO),
                       bt_audio_radio_on() ? bt_str(LANG_ON) : bt_str(LANG_OFF));

    info.count = simplelist_get_line_count();
    simplelist_show_list(&info);
}

/* Bluetooth output submenu: status, device list, disconnect. */
static int bt_output_menu(void)
{
    int action = -1;

    /* Output and receive are mutually exclusive: while the receive
     * path owns the radio and the wired output, refuse to route. */
    if (bt_input_active())
    {
        splash(HZ * 2, bt_str(LANG_BT_RX_ACTIVE));
        return 0;
    }

    while (true)
    {
        struct simplelist_info info;

        simplelist_info_init(&info, bt_str(LANG_BT_AUDIO_OUT), 3, NULL);
        info.get_name = bt_action_name_cb;
        info.action_callback = bt_simplelist_ok_cancel;
        info.selection = -1;
        info.title_icon = Icon_Submenu;

        simplelist_show_list(&info);
        action = info.selection;
        if (action < 0)
            break;

        switch (action)
        {
            case 0:
                bt_show_status();
                break;
            case 1:
                bt_audio_busy(true);
                bt_show_devices();
                bt_audio_busy(false);
                break;
            case 2:
                bt_audio_busy(true);
                bt_audio_disconnect();
                bt_audio_busy(false);
                break;
            default:
                break;
        }
    }
    return 0;
}

/* Bluetooth receive screen: live status lines plus a selectable
 * "Disconnect" entry at the bottom. Choosing it drops the phone link,
 * stops the pump and powers the radio down. Back leaves the receiver
 * running in the background (like USB DAC mode) so the menus stay
 * usable; local playback is refused while it runs (apps/playback.c).
 * The list refreshes itself when the link state changes (checked on
 * the 2 s input timeout). */
#define BT_RX_ROWS 5

static const char *bt_rx_name_cb(int selected_item, void *data,
                                 char *buffer, size_t buffer_len)
{
    char peer[BT_AUDIO_MAC_LEN];
    const char *str;

    (void)data;

    if (selected_item < 0 || selected_item >= BT_RX_ROWS)
    {
        buffer[0] = '\0';
        return buffer;
    }

    switch (selected_item)
    {
        case 0:
            switch (bt_input_get_state())
            {
                case BT_INPUT_CONNECTED:
                    str = bt_str(LANG_BT_CONNECTED);
                    break;
                case BT_INPUT_DISCONNECTED:
                    str = bt_str(LANG_BT_DISCONNECTED);
                    break;
                default:
                    str = bt_str(LANG_BT_RX_WAITING);
                    break;
            }
            snprintf(buffer, buffer_len, "%s: %s",
                     bt_str(LANG_BT_STATUS), str);
            break;
        case 1:
            bt_input_get_peer(peer, sizeof(peer));
            snprintf(buffer, buffer_len, "%s: %s", bt_str(LANG_BT_MAC),
                     peer[0] ? peer : "-");
            break;
        case 2:
            snprintf(buffer, buffer_len, "%s: %s", bt_str(LANG_BT_CODEC),
                     bt_audio_codec());
            break;
        case 3:
        {
            int rate = bt_input_get_rate();

            if (rate > 0)
                snprintf(buffer, buffer_len, "%s: %d Hz",
                         bt_str(LANG_BT_RX_RATE), rate);
            else
                snprintf(buffer, buffer_len, "%s: -", bt_str(LANG_BT_RX_RATE));
            break;
        }
        default:
            snprintf(buffer, buffer_len, "%s", bt_str(LANG_BT_DISCONNECT));
            break;
    }
    return buffer;
}

static int bt_rx_action_cb(int action, struct gui_synclist *lists)
{
    static int last_state = -1;
    static int last_rate = -1;
    static char last_peer[BT_AUDIO_MAC_LEN];

    if (action == ACTION_STD_OK)
    {
        if (gui_synclist_get_sel_pos(lists) == BT_RX_ROWS - 1)
            return ACTION_STD_CANCEL;    /* "Disconnect": exit list */
        return ACTION_NONE;               /* status rows do nothing */
    }

    if (action == ACTION_NONE)
    {
        /* Input timeout: redraw only when something changed. */
        char peer[BT_AUDIO_MAC_LEN];
        int state = bt_input_get_state();
        int rate = bt_input_get_rate();

        bt_input_get_peer(peer, sizeof(peer));
        if (state != last_state || rate != last_rate ||
            strcmp(peer, last_peer) != 0)
        {
            last_state = state;
            last_rate = rate;
            snprintf(last_peer, sizeof(last_peer), "%s", peer);
            return ACTION_REDRAW;
        }
    }
    return action;
}

static void bt_rx_screen(void)
{
    struct simplelist_info info;

    /* Incoming pairings/connections need the stack agent; bring it up
     * before waiting for the phone. */
    bt_audio_busy(true);
    bt_audio_prepare();
    bt_audio_busy(false);

    /* Receive and output are mutually exclusive: if an earphone route
     * is up, drop it (without powering the radio down -- receiving
     * needs it) and stop the output watchdog. */
    if (!bt_input_active() && bt_audio_selected()[0])
        bt_audio_release();

    /* The receive path owns the output, like USB DAC mode: stop any
     * running playback first. */
    if (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        audio_stop();

    /* The pump may keep waiting for the phone only while this screen
     * is open; in the background it winds itself down on link loss.
     * Set the flag BEFORE starting the pump -- its very first open
     * usually fails (no transport yet) and must retry, not wind down. */
    bt_input_set_fg(true);

    if (!bt_input_active() && !bt_input_start())
    {
        bt_input_set_fg(false);
        splash(HZ * 2, bt_str(LANG_BT_UNAVAILABLE));
        return;
    }

    simplelist_info_init(&info, bt_str(LANG_BT_RX), BT_RX_ROWS, NULL);
    info.get_name = bt_rx_name_cb;
    info.action_callback = bt_rx_action_cb;
    info.timeout = HZ * 2;
    info.selection = BT_RX_ROWS - 1;
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);

    bt_input_set_fg(false);

    if (info.selection == BT_RX_ROWS - 1)
    {
        /* "Disconnect": full stop -- drop the phone link, stop the
         * pump and power the radio down before returning. */
        char peer[BT_AUDIO_MAC_LEN];

        bt_input_get_peer(peer, sizeof(peer));
        if (peer[0])
            bt_audio_disconnect_peer(peer);
        bt_input_stop();
        bt_audio_power_off();
        splash(HZ, bt_str(LANG_BT_RX_STOPPED));
    }
    /* Back (selection < 0): receiving keeps running in the background;
     * local playback and the earphone route stay locked out until it
     * is stopped from this screen. */
}

static void bt_reset_screen(void)
{
    struct viewport vp;
    struct screen *sc = &screens[SCREEN_MAIN];

    if (!yesno_pop(bt_str(LANG_BT_RESET_CONFIRM)))
        return;

    /* The re-init blocks for several seconds (firmware download,
     * bluetoothd start); keep a message on screen while it runs.
     * The parent menu redraws on return. */
    viewport_set_defaults(&vp, SCREEN_MAIN);
    sc->set_viewport(&vp);
    sc->clear_display();
    sc->puts(0, 0, bt_str(LANG_BT_RESETTING));
    sc->update_viewport();
    sc->set_viewport(NULL);

    bt_audio_reset_stack();
}

static const char *bt_top_name_cb(int selected_item, void *data,
                                  char *buffer, size_t buffer_len)
{
    static const unsigned short ids[] =
    {
        LANG_BT_AUDIO_OUT, LANG_BT_AUDIO_IN, LANG_BT_RESET
    };
    (void)data;

    if (selected_item < 0 || selected_item >= 3)
    {
        buffer[0] = '\0';
        return buffer;
    }
    snprintf(buffer, buffer_len, "%s", bt_str(ids[selected_item]));
    return buffer;
}

int bluetooth_menu(void)
{
    while (true)
    {
        struct simplelist_info info;

        simplelist_info_init(&info, bt_str(LANG_BLUETOOTH), 3, NULL);
        info.get_name = bt_top_name_cb;
        info.selection = -1;
        info.title_icon = Icon_Submenu;

        simplelist_show_list(&info);
        if (info.selection < 0)
            break;

        switch (info.selection)
        {
            case 0:
                bt_output_menu();
                break;
            case 1:
                bt_rx_screen();
                break;
            case 2:
                bt_reset_screen();
                break;
            default:
                break;
        }
    }
    return 0;
}

#endif /* HAVE_BT_AUDIO */
