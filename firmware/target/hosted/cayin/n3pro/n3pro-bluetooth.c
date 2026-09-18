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

/* Cayin N3Pro bluetooth audio driver: the target half of the generic
 * framework in apps/bluetooth.c (firmware/export/bt_audio.h).
 *
 * A2DP source: everything runs synchronously on the menu thread -- there
 * is no background thread and no link watcher.  The stock HiByOS bluetooth
 * stack is driven through the vendor /var/run/sys_server socket (BT:LIST /
 * BT:SCAN / BT:PAIR / BT:CONNECT / ...), and the A2DP audio is played
 * through the stock ALSA plugin by writing its peer address into
 * /etc/asound.conf and re-opening the playback PCM. */

#include "config.h"

#ifdef CAYIN_N3PRO

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/stat.h>

#include "kernel.h"
#include "audio.h"
#include "splash.h"
#include "lang.h"
#include "settings.h"
#include "sound.h"
#include "pcm-alsa.h"
#include "n3pro-bt-pcm.h"
#include "bt_audio.h"
#include "bt_input.h"
#include "statusbar_rf.h"

/* str() yields const unsigned char *; keep the string call sites simple. */
static char *bt_str(int id)
{
    return (char *)str(id);
}

#define BT_MAX_DEVICES 32
#define BT_LOCAL_PLAYBACK_DEVICE "plughw:0,0"
#define BT_ASOUND_CONF "/etc/asound.conf"
/* The asound override is rewritten on every output route, so keep the real
 * file on tmpfs and point /etc/asound.conf at it (once, lazily) instead of
 * wearing the rootfs flash on every connect. */
#define BT_ASOUND_TMPFS "/tmp/asound.conf"
#define BT_AUDIO_CONF "/etc/bluetooth/audio.conf"
#define BT_SYS_SOCKET "/var/run/sys_server"
#define BT_LIST_FILE "/tmp/bt_list.txt"
#define BT_SCAN_FILE "/tmp/bt_scan.txt"
#define BT_SYS_REPLY_MAX 128

static char bt_selected_mac[BT_AUDIO_MAC_LEN];
static bool bt_prefer = false;   /* the user wants the bluetooth output */

/* Output watchdog state (see bt_watchdog()). */
static bool bt_busy = false;   /* a menu-driven route is in progress */
static bool bt_peer_linked(const char *mac);
static bool bt_watchdog_started = false;
static volatile bool bt_watchdog_run = false;
/* radio powered - tracked at the power transitions so the statusbar
 * icon (statusbar_rf.h) does not have to poke the stack to know */
static volatile bool bt_stack_on = false;

static void bt_watchdog_start(void);
static void bt_watchdog_stop(void);

static bool bt_prepare_stack(void);
static void bt_connect_device(const struct bt_audio_dev *device);

static void bt_trim(char *s)
{
    size_t len;

    if (!s)
        return;

    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       isspace((unsigned char)s[len - 1])))
        s[--len] = '\0';
}

static bool bt_has_mac_pattern(const char *p, char sep)
{
    int i;

    for (i = 0; i < 17; i++)
    {
        if ((i % 3) == 2)
        {
            if (p[i] != sep)
                return false;
        }
        else if (!isxdigit((unsigned char)p[i]))
        {
            return false;
        }
    }

    return true;
}

static bool bt_extract_mac_from_line(const char *line, char *mac_out, size_t mac_out_len)
{
    size_t i, len;

    if (!line || mac_out_len < 18)
        return false;

    len = strlen(line);
    if (len < 17)
        return false;

    for (i = 0; i + 17 <= len; i++)
    {
        char sep = line[i + 2];
        if (sep != ':' && sep != '_')
            continue;
        if (!bt_has_mac_pattern(&line[i], sep))
            continue;

        snprintf(mac_out, mac_out_len, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
            toupper((unsigned char)line[i + 0]), toupper((unsigned char)line[i + 1]),
            toupper((unsigned char)line[i + 3]), toupper((unsigned char)line[i + 4]),
            toupper((unsigned char)line[i + 6]), toupper((unsigned char)line[i + 7]),
            toupper((unsigned char)line[i + 9]), toupper((unsigned char)line[i + 10]),
            toupper((unsigned char)line[i + 12]), toupper((unsigned char)line[i + 13]),
            toupper((unsigned char)line[i + 15]), toupper((unsigned char)line[i + 16]));
        return true;
    }

    return false;
}

static int bt_add_device_unique_ex(struct bt_audio_dev *devices, int count, int max_devices,
    const char *mac, const char *name, bool paired)
{
    int i;

    if (!mac || !mac[0] || count >= max_devices)
        return count;

    for (i = 0; i < count; i++)
    {
        if (!strcasecmp(devices[i].mac, mac))
        {
            if (paired)
                devices[i].paired = true;
            if (name && name[0] &&
                (!devices[i].name[0] || !strcasecmp(devices[i].name, devices[i].mac)))
            {
                snprintf(devices[i].name, sizeof(devices[i].name), "%s", name);
            }
            return count;
        }
    }

    snprintf(devices[count].mac, sizeof(devices[count].mac), "%s", mac);
    if (name && name[0])
        snprintf(devices[count].name, sizeof(devices[count].name), "%s", name);
    else
        snprintf(devices[count].name, sizeof(devices[count].name), "%s", mac);
    devices[count].paired = paired;

    return count + 1;
}

static int bt_device_sort_cmp(const void *a, const void *b)
{
    const struct bt_audio_dev *da = a;
    const struct bt_audio_dev *db = b;

    if (da->paired != db->paired)
        return da->paired ? -1 : 1;

    return strcasecmp(da->name, db->name);
}

static int bt_sys_command(const char *command, char *reply, size_t reply_size)
{
    struct sockaddr_un addr;
    struct timeval tv_send = { .tv_sec = 1, .tv_usec = 0 };
    struct timeval tv_recv = { .tv_sec = 2, .tv_usec = 0 };
    int fd = -1;
    ssize_t n;

    if (!command || !command[0])
        return -1;

    if (reply && reply_size > 0)
        reply[0] = '\0';

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", BT_SYS_SOCKET);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    if (send(fd, command, strlen(command), 0) < 0)
    {
        close(fd);
        return -1;
    }

    if (reply && reply_size > 1)
    {
        n = recv(fd, reply, reply_size - 1, 0);
        if (n < 0)
        {
            close(fd);
            return -1;
        }

        reply[n] = '\0';
        bt_trim(reply);
    }

    close(fd);
    return 0;
}

static bool bt_sys_reply_ok(const char *reply, const char *command_prefix)
{
    char ok_reply[48];
    char wait_reply[48];

    if (!reply || !reply[0])
        return false;

    if (!strcasecmp(reply, "OK") || !strcasecmp(reply, "WAITINIT"))
        return true;

    if (strstr(reply, "FAIL"))
        return false;

    if (!command_prefix || !command_prefix[0])
        return false;

    snprintf(ok_reply, sizeof(ok_reply), "%s:OK", command_prefix);
    snprintf(wait_reply, sizeof(wait_reply), "%s:WAITINIT", command_prefix);

    if (strstr(reply, ok_reply))
        return true;
    if (strstr(reply, wait_reply))
        return true;

    return false;
}

static bool bt_json_get_string_value(const char *line, const char *key,
                                     char *out, size_t out_len)
{
    const char *start;
    const char *colon;
    const char *q1;
    const char *q2;
    size_t len;

    if (!line || !key || !out || out_len == 0)
        return false;

    start = strstr(line, key);
    if (!start)
        return false;

    colon = strchr(start, ':');
    if (!colon)
        return false;

    q1 = strchr(colon, '"');
    if (!q1)
        return false;
    q1++;

    q2 = strchr(q1, '"');
    if (!q2)
        return false;

    len = (size_t)(q2 - q1);
    if (len >= out_len)
        len = out_len - 1;
    memcpy(out, q1, len);
    out[len] = '\0';
    return true;
}

static bool bt_json_get_int_value(const char *line, const char *key, int *value)
{
    const char *start;
    const char *colon;
    const char *p;

    if (!line || !key || !value)
        return false;

    start = strstr(line, key);
    if (!start)
        return false;

    colon = strchr(start, ':');
    if (!colon)
        return false;

    p = colon + 1;
    while (*p == ' ' || *p == '\t')
        p++;

    if (!isdigit((unsigned char)*p) && *p != '-')
        return false;

    *value = atoi(p);
    return true;
}

static bool bt_get_file_stamp(const char *path, long *mtime, long *size)
{
    struct stat st;

    if (!path || !mtime || !size)
        return false;

    if (stat(path, &st) < 0)
        return false;

    *mtime = (long)st.st_mtime;
    *size = (long)st.st_size;
    return true;
}

static int bt_load_devices_from_json_file(const char *path,
                                          struct bt_audio_dev *devices,
                                          int max_devices,
                                          bool *ready)
{
    FILE *fp;
    char line[512];
    char mac[BT_AUDIO_MAC_LEN] = "";
    char name[BT_AUDIO_NAME_LEN] = "";
    int paired = 0;
    int count = 0;
    bool has_device_key = false;

    if (ready)
        *ready = false;

    fp = fopen(path, "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "\"DEVICE\""))
            has_device_key = true;

        if (strstr(line, "\"MAC\""))
            bt_extract_mac_from_line(line, mac, sizeof(mac));

        if (bt_json_get_string_value(line, "\"Name\"", name, sizeof(name)))
            bt_trim(name);

        bt_json_get_int_value(line, "\"Paired\"", &paired);

        if (mac[0] && strchr(line, '}'))
        {
            count = bt_add_device_unique_ex(devices, count, max_devices,
                                            mac, name, paired != 0);
            mac[0] = '\0';
            name[0] = '\0';
            paired = 0;
        }
    }

    fclose(fp);

    if (ready)
        *ready = has_device_key;

    return count;
}

static int bt_load_devices_from_bt_list_file(struct bt_audio_dev *devices, int max_devices,
                                             bool *ready)
{
    return bt_load_devices_from_json_file(BT_LIST_FILE, devices, max_devices, ready);
}

static int bt_merge_devices_from_bt_scan_file(struct bt_audio_dev *devices, int count,
                                              int max_devices, bool *ready)
{
    static struct bt_audio_dev scanned[BT_MAX_DEVICES];
    int scanned_count;
    int i;

    scanned_count = bt_load_devices_from_json_file(BT_SCAN_FILE, scanned,
                                                   BT_MAX_DEVICES, ready);
    for (i = 0; i < scanned_count; i++)
    {
        count = bt_add_device_unique_ex(devices, count, max_devices,
                                        scanned[i].mac, scanned[i].name,
                                        scanned[i].paired);
    }

    return count;
}

static int bt_load_devices_via_sys_list(struct bt_audio_dev *devices, int max_devices)
{
    char reply[BT_SYS_REPLY_MAX];
    long old_mtime = 0;
    long old_size = 0;
    bool had_old_stamp = false;
    bool ready = false;
    bool changed = false;
    int count = 0;
    int waited = 0;

    had_old_stamp = bt_get_file_stamp(BT_LIST_FILE, &old_mtime, &old_size);
    if (bt_sys_command("BT:LIST", reply, sizeof(reply)) < 0)
        return 0;

    while (waited < HZ * 3)
    {
        long mtime = 0;
        long size = 0;

        count = bt_load_devices_from_bt_list_file(devices, max_devices, &ready);

        if (bt_get_file_stamp(BT_LIST_FILE, &mtime, &size))
        {
            if (!had_old_stamp || mtime != old_mtime || size != old_size)
                changed = true;
        }

        if (ready && (changed || waited >= HZ))
            break;

        sleep(HZ / 5);
        waited += HZ / 5;
    }

    if (count > 1)
        qsort(devices, count, sizeof(devices[0]), bt_device_sort_cmp);
    return count;
}

/* sys_server's BT:SCAN only echoes the already-cached devices on this
 * firmware (the inquiry never reports the neighbourhood), so drive the
 * vendor discovery helper directly: it runs a full ~10 s inquiry and
 * writes the JSON list itself. */
static int bt_scan_and_merge_devices(struct bt_audio_dev *devices, int count, int max_devices)
{
    bool ready = false;

    system("/usr/bin/bt-adapter -d -p " BT_SCAN_FILE " >/dev/null 2>&1");

    count = bt_merge_devices_from_bt_scan_file(devices, count, max_devices, &ready);

    if (count > 1)
        qsort(devices, count, sizeof(devices[0]), bt_device_sort_cmp);
    return count;
}

/* MAC of the currently linked device (there is at most one on this
 * player), used to flag it in the device list. */
static bool bt_current_connection(char *mac_out, size_t mac_out_len)
{
    FILE *fp;
    char line[256];
    bool found = false;

    if (!mac_out || mac_out_len < 18)
        return false;
    mac_out[0] = '\0';

    fp = popen("/usr/bin/hcitool con 2>/dev/null", "r");
    if (fp)
    {
        while (fgets(line, sizeof(line), fp))
        {
            if (bt_extract_mac_from_line(line, mac_out, mac_out_len))
            {
                found = true;
                break;
            }
        }
        pclose(fp);
    }

    return found;
}

static void bt_set_selected_mac(const char *mac)
{
    if (mac && mac[0])
    {
        /* Guard against snprintf()-ing the buffer onto itself (the link
         * watcher passes bt_selected_mac), which is undefined and was
         * wiping the address. */
        if (mac != bt_selected_mac)
            snprintf(bt_selected_mac, sizeof(bt_selected_mac), "%s", mac);
    }
    else
        bt_selected_mac[0] = '\0';
}

static void bt_kick_audio_if_playing(void)
{
    int status = audio_status();
    if ((status & AUDIO_STATUS_PLAY) && !(status & AUDIO_STATUS_PAUSE))
    {
        audio_pause();
        sleep(HZ / 4);
        audio_resume();
    }
}

/* Where the runtime asound override really lives.  Prefer the tmpfs file
 * with /etc/asound.conf symlinked to it; create the link lazily on first
 * use and fall back to the plain rootfs file when that is impossible
 * (read-only fs, no symlink support).  A dangling link after a reboot is
 * fine: alsa-lib simply sees "no user config" and we recreate the file. */
static const char *bt_asound_target(void)
{
    struct stat st;

    if (lstat(BT_ASOUND_CONF, &st) == 0)
    {
        if (S_ISLNK(st.st_mode))
            return BT_ASOUND_TMPFS;     /* already linked */
    }
    else if (errno != ENOENT)
        return BT_ASOUND_CONF;          /* cannot inspect: write plainly */

    /* A real file (or nothing): replace it with a link to the tmpfs. */
    if (unlink(BT_ASOUND_CONF) != 0 && errno != ENOENT)
        return BT_ASOUND_CONF;          /* read-only rootfs: write plainly */

    if (symlink(BT_ASOUND_TMPFS, BT_ASOUND_CONF) != 0)
        return BT_ASOUND_CONF;          /* cannot link: write plainly */

    return BT_ASOUND_TMPFS;
}

/* Write the peer the stock bluetooth ALSA plugin must stream to.  The
 * vendor player rewrites this file at runtime, so we do the same - but
 * only when the content actually changes, and on tmpfs once the symlink
 * is in place. */
static void bt_write_asound(const char *mac)
{
    char buf[1024];
    const char *path;
    int len = snprintf(buf, sizeof(buf),
        "pcm.bluetooth {\n"
        "    type bluetooth\n"
        "    device \"%s\"\n"
        "    profile \"a2dp\"\n"
        "}\n"
        /* The AK4493 hardware volume does not touch the bluetooth path, so
         * wrap the plugin in a userspace softvol whose "Bluetooth Vol" mixer
         * control is driven by audiohw_set_volume() while this route is up.
         * The softvol chain sits behind a plug, because the 32-bit sample
         * stream (HAVE_ALSA_32BIT) has to be converted down to the S16 the
         * stock bluetooth plugin accepts; plug is pass-through when the
         * formats already match. */
        "pcm.%s {\n"
        "    type plug\n"
        "    slave.pcm {\n"
        "        type softvol\n"
        "        slave.pcm {\n"
        "            type bluetooth\n"
        "            device \"%s\"\n"
        "            profile \"a2dp\"\n"
        "        }\n"
        "        control {\n"
        "            name \"Bluetooth Vol\"\n"
        "            card 0\n"
        "        }\n"
        /* The audible window of a digital attenuation is about 50 dB; use it
         * all so the volume steps do not bunch up in the top few dB. */
        "        min_dB -50.0\n"
        "        max_dB 0.0\n"
        "    }\n"
        "}\n", mac, N3PRO_BT_DEVICE, mac);

    if (len <= 0 || len >= (int)sizeof(buf))
        return;

    path = bt_asound_target();

    {
        char old[1024];
        size_t rn = 0;
        FILE *rf = fopen(path, "r");
        if (rf)
        {
            rn = fread(old, 1, sizeof(old) - 1, rf);
            fclose(rf);
            old[rn] = '\0';
        }
        if (rn == (size_t)len && memcmp(old, buf, rn) == 0)
            return;                 /* already correct - do not rewrite */
    }

    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fwrite(buf, 1, (size_t)len, f);
    fclose(f);
}

/* High bitrate first (same idea as the R1 port): keep the best codec the
 * earphone actually accepts.  The stock ALSA plugin reads DefaultCodec from
 * /etc/bluetooth/audio.conf when it opens the PCM, so a codec change means
 * rewriting that file and renegotiating the A2DP transport. */
static const char *const bt_codec_pref[] =
{
    "LDAC_HQ", "APTX", "AAC", "SBC", NULL
};

static void bt_set_codec(const char *codec)
{
    char cmd[64];

    snprintf(cmd, sizeof(cmd), "/usr/bin/bt_codec %s >/dev/null 2>&1", codec);
    system(cmd);
}

/* Walk the preference list: set the codec, renegotiate the link and probe
 * the plugin.  The first codec whose transport comes up wins. */
static const char *bt_pick_codec(const char *mac)
{
    char cmd[96];
    int i, t;

    for (i = 0; bt_codec_pref[i] != NULL; i++)
    {
        /* Give the first attempt (on the link the caller just made) time
         * to come up before stepping down the codec list. */
        int probes = (i == 0) ? 12 : 6;

        bt_set_codec(bt_codec_pref[i]);

        /* The codec is fixed when the A2DP transport is set up, so a new
         * codec needs a fresh connection.  Only ever renegotiate while
         * the link is really up: toggling a link that is not up wedges
         * the stock bluetooth stack (reboot required). */
        if (i > 0)
        {
            if (!bt_peer_linked(mac))
                break;

            snprintf(cmd, sizeof(cmd),
                     "/usr/bin/bt-connect -d %s >/dev/null 2>&1", mac);
            system(cmd);
            sleep(HZ / 2);
            snprintf(cmd, sizeof(cmd),
                     "/usr/bin/bt-connect -c %s >/dev/null 2>&1", mac);
            system(cmd);
        }

        for (t = 0; t < probes; t++)
        {
            if (pcm_alsa_bt_probe() == 0)
                return bt_codec_pref[i];
            sleep(HZ / 4);
        }

        /* Nothing came up and the link is gone: cycling would only wedge
         * the stack further. */
        if (!bt_peer_linked(mac))
            break;
    }

    return NULL;
}

/* Rockbox has a single volume, so the bluetooth and wired outputs each keep
 * a level (global_settings.bt_volume / global_status.volume) and swap them
 * when the output changes.  The bluetooth level is derived from the wired
 * one the first time; nothing is ever forced on the earpiece. */
static bool bt_vol_on_bt = false;

static void bt_vol_swap(void)
{
    int t;

    if (!global_settings.bt_volume_set)
    {
        global_settings.bt_volume = global_status.volume;
        global_settings.bt_volume_set = true;
    }

    t = global_status.volume;
    sound_set_volume(global_settings.bt_volume);
    global_settings.bt_volume = t;
    settings_save();
}

static void bt_vol_enter_bt(void)
{
    if (bt_vol_on_bt)
        return;

    bt_vol_swap();
    bt_vol_on_bt = true;
}

static void bt_vol_enter_local(void)
{
    if (!bt_vol_on_bt)
        return;

    bt_vol_swap();
    bt_vol_on_bt = false;
}

/* An unclean link drop (earpieces powered off in the middle of a
 * connection) leaves a stale ACL behind in the controller: the host side
 * loses the handle ("ACL packet for unknown connection handle") while the
 * earpieces still believe they are connected, so every later connect
 * fails with "Stream setup failed" until the earpieces are re-paired or
 * the player reboots.  An HCI reset clears the controller state and makes
 * both ends drop the ghost link. */
static void bt_controller_reset(void)
{
    system("/usr/sbin/hciconfig hci0 reset >/dev/null 2>&1");
    sleep(HZ / 2);
}

static void bt_route_to_local(bool show_message)
{
    int status = audio_status();
    bool was_playing = (status & AUDIO_STATUS_PLAY)
                       && !(status & AUDIO_STATUS_PAUSE);

    /* Leave the bluetooth route while paused: mid-stream the wired
     * device would briefly carry the bluetooth-route unity gain at
     * full scale to the headphone jack.  The wired volume is put
     * back before playback resumes. */
    if (was_playing)
    {
        audio_pause();
        sleep(HZ / 4);
    }

    pcm_alsa_switch_playback_device(BT_LOCAL_PLAYBACK_DEVICE);
    bt_vol_enter_local();

    if (was_playing)
        audio_resume();

    if (show_message)
        splash(HZ, "Output: Local");
}

static bool bt_route_to_bluetooth(const char *mac)
{
    if (!mac || !mac[0])
        return false;

    bt_write_asound(mac);

    /* Pick the highest codec the earphone supports; on success the A2DP
     * transport is already up, so no extra wait is needed. */
    if (!bt_pick_codec(mac))
    {
        bt_route_to_local(false);
        return false;
    }

    if (pcm_alsa_switch_playback_device(N3PRO_BT_DEVICE) == 0)
    {
        /* Switch to the independent bluetooth volume (the softvol follows
         * it); the earpiece volume is left alone. */
        bt_prefer = true;
        bt_watchdog_start();
        bt_vol_enter_bt();
        bt_kick_audio_if_playing();
        return true;
    }

    bt_route_to_local(false);
    return false;
}

/* Route back to bluetooth after the earpieces reconnected on their own.
 * The vendor stack has already brought the link (and its transport) up,
 * so only the PCM route is switched -- no codec walk, which would cycle
 * the link and wedge the stock bluetooth stack. */
static bool bt_route_auto(const char *mac)
{
    int i;

    if (!mac || !mac[0])
        return false;

    bt_write_asound(mac);

    /* The A2DP transport may not be fully established even though the
     * ACL link is present.  Probe a few times before giving up. */
    for (i = 0; i < 10; i++)
    {
        if (pcm_alsa_bt_probe() == 0 &&
            pcm_alsa_switch_playback_device(N3PRO_BT_DEVICE) == 0)
        {
            bt_set_selected_mac(mac);
            bt_vol_enter_bt();
            bt_kick_audio_if_playing();
            return true;
        }
        sleep(HZ / 2);
    }

    bt_route_to_local(false);
    return false;
}

static const char *bt_get_codec(void)
{
    static char codec[16];
    FILE *f = fopen(BT_AUDIO_CONF, "r");
    char line[256];

    codec[0] = '\0';
    if (!f)
        return "Unknown";

    while (fgets(line, sizeof(line), f))
    {
        char *p = strstr(line, "DefaultCodec");
        if (p)
        {
            char *eq = strchr(p, '=');
            if (eq)
            {
                char *v = eq + 1;
                size_t n = 0;
                while (*v == ' ' || *v == '\t')
                    v++;
                while (v[n] && v[n] != '\n' && v[n] != '\r' && n + 1 < sizeof(codec))
                {
                    codec[n] = v[n];
                    n++;
                }
                codec[n] = '\0';
            }
            break;
        }
    }

    fclose(f);
    return codec[0] ? codec : "Unknown";
}

/* The stock HiByOS control daemon is started by /etc/init.d/S50sys_server,
 * but it does not survive the hand-off into Rockbox on this player, so it
 * is (re)started on demand before the first BT command. */
static void bt_ensure_sys_server(void)
{
    char reply[BT_SYS_REPLY_MAX];

    if (bt_sys_command("BT:STATUS", reply, sizeof(reply)) == 0)
        return;

    system("(setsid /usr/bin/sys_server >/dev/null 2>&1 &)");
    sleep(HZ / 2);
}

static bool bt_prepare_stack(void)
{
    char reply[BT_SYS_REPLY_MAX];
    int i;

    bt_ensure_sys_server();

    system("bt_enable >/dev/null 2>&1");

    for (i = 0; i < 12; i++)
    {
        reply[0] = '\0';
        if (bt_sys_command("BT:LIST", reply, sizeof(reply)) == 0 &&
            bt_sys_reply_ok(reply, "BT:LIST"))
        {
            bt_stack_on = true;
            return true;
        }

        reply[0] = '\0';
        bt_sys_command("BT:ON", reply, sizeof(reply));

        sleep(HZ / 5);
    }

    return false;
}

static void bt_delete_device(const char *mac)
{
    char cmd[96];
    char reply[BT_SYS_REPLY_MAX];

    if (!mac || !mac[0])
        return;

    snprintf(cmd, sizeof(cmd), "BT:UNPAIR:%s", mac);
    if (bt_sys_command(cmd, reply, sizeof(reply)) != 0 ||
        !bt_sys_reply_ok(reply, "BT:UNPAIR"))
    {
        snprintf(cmd, sizeof(cmd),
                 "/usr/bin/bt-device -r %s >/dev/null 2>&1", mac);
        system(cmd);
    }

    splash(HZ, bt_str(LANG_BT_DELETED));
}

static void bt_connect_device(const struct bt_audio_dev *device)
{
    const char *mac;
    char cmd[96];
    char reply[BT_SYS_REPLY_MAX];
    int ctl_rc;
    bool routed;
    bool connect_reply_ok;
    bool pair_reply_ok = true;

    if (!device || !device->mac[0])
        return;

    mac = device->mac;
    splash(0, bt_str(LANG_BT_CONNECTING));

    if (!bt_prepare_stack())
    {
        splash(HZ * 2, bt_str(LANG_BT_UNAVAILABLE));
        return;
    }

    if (!device->paired)
    {
        snprintf(cmd, sizeof(cmd), "BT:PAIR:%s", mac);
        ctl_rc = bt_sys_command(cmd, reply, sizeof(reply));
        pair_reply_ok = (ctl_rc == 0) && bt_sys_reply_ok(reply, "BT:PAIR");
        if (pair_reply_ok)
            sleep(HZ / 2);
    }
    else if (!bt_peer_linked(mac))
    {
        /* A ghost ACL left over from an unclean link drop is the usual
         * reason a connect to an already-paired device fails ("Stream
         * setup failed"): clear the controller before dialling.  Never do
         * this after a fresh pairing -- the reset would interrupt the
         * pairing handshake that just completed. */
        bt_controller_reset();
    }

    snprintf(cmd, sizeof(cmd), "BT:CONNECT:%s", mac);
    ctl_rc = bt_sys_command(cmd, reply, sizeof(reply));
    connect_reply_ok = (ctl_rc == 0) && bt_sys_reply_ok(reply, "BT:CONNECT");

    if (!pair_reply_ok && !device->paired)
    {
        splash(HZ * 2, bt_str(LANG_BT_PAIR_FAILED));
        return;
    }

    if (!connect_reply_ok)
    {
        splash(HZ * 2, bt_str(LANG_BT_CONNECT_FAILED));
        return;
    }

    bt_set_selected_mac(mac);

    routed = bt_route_to_bluetooth(mac);
    if (!routed)
    {
        if (device->paired)
            bt_controller_reset();
        snprintf(cmd, sizeof(cmd), "BT:CONNECT:%s", mac);
        ctl_rc = bt_sys_command(cmd, reply, sizeof(reply));
        if (ctl_rc == 0 && bt_sys_reply_ok(reply, "BT:CONNECT"))
        {
            sleep(HZ / 2);
            routed = bt_route_to_bluetooth(mac);
        }
    }

    if (routed)
        splash(HZ, bt_str(LANG_BT_CONNECTED));
    else
        splash(HZ * 2, bt_str(LANG_BT_NO_ROUTE));
}

static bool bt_radio_on(void)
{
    FILE *fp;
    char line[256];
    bool on = false;

    fp = popen("/usr/sbin/hciconfig hci0 2>/dev/null", "r");
    if (fp)
    {
        while (fgets(line, sizeof(line), fp))
        {
            if (strstr(line, "UP RUNNING"))
            {
                on = true;
                break;
            }
        }
        pclose(fp);
    }

    return on;
}

/* Power the controller down as well: leaving it up after a disconnect
 * keeps the radio (and the earbuds' link) alive and drains the battery.
 * sys_server's BT:OFF answers OK but does not actually drop the adapter
 * on this firmware, so the vendor helper is driven directly. */
static void bt_power_off(void)
{
    char reply[BT_SYS_REPLY_MAX];
    int i;

    bt_sys_command("BT:OFF", reply, sizeof(reply));
    system("/usr/bin/bt_disable >/dev/null 2>&1");

    for (i = 0; i < 5 && bt_radio_on(); i++)
        sleep(HZ / 10);

    if (bt_radio_on())
        system("/usr/sbin/hciconfig hci0 down >/dev/null 2>&1");

    bt_stack_on = false;
}

/* Reset the whole bluetooth stack to the boot-time state: stop any
 * active receive session or earphone route, tear the vendor stack
 * fully down (dbus, patchram, bluetoothd, chip power -- /usr/bin/
 * bt_done) and bring it back exactly like /etc/init.d/S40bt_init
 * does at boot (/usr/bin/bt_init).  The radio ends up Powered Off;
 * the receive screen and the output menu power it on themselves
 * when they need it. */
static void bt_reset_stack(void)
{
    char cmd[36], reply[BT_SYS_REPLY_MAX];

    /* Drop anything that holds the stack or the output busy. */
    if (bt_input_active())
        bt_input_stop();
    if (bt_selected_mac[0])
    {
        snprintf(cmd, sizeof(cmd), "BT:DISCONNECT:%s", bt_selected_mac);
        bt_sys_command(cmd, reply, sizeof(reply));
        bt_prefer = false;
        bt_watchdog_stop();
        bt_route_to_local(false);
        bt_set_selected_mac(NULL);
    }

    /* The extra kills cover what bt_done forgets (the agents) and
     * sys_server, whose stale dbus connection would talk to the old
     * bluetoothd; bt_ensure_sys_server() re-spawns it on demand. */
    system("/usr/bin/bt_done >/dev/null 2>&1");
    system("killall bt-agent bt-monitor sys_server >/dev/null 2>&1");
    bt_stack_on = false;
    sleep(HZ / 2);
    system("/usr/bin/bt_init >/dev/null 2>&1");
}

static void bt_disconnect(void)
{
    char cmd[96];
    char reply[BT_SYS_REPLY_MAX];

    if (bt_selected_mac[0])
    {
        snprintf(cmd, sizeof(cmd), "BT:DISCONNECT:%s", bt_selected_mac);
        bt_sys_command(cmd, reply, sizeof(reply));
    }

    bt_prefer = false;
    bt_watchdog_stop();
    bt_route_to_local(false);
    bt_set_selected_mac(NULL);
    bt_power_off();
    splash(HZ, bt_str(LANG_BT_DISCONNECTED));
}

/* The vendor stack exposes an AVRCP input device named after the peer's
 * MAC while the earpieces are linked, and removes it again when they go
 * away: a fork-free way to notice a disconnect. */
static bool bt_peer_linked(const char *mac)
{
    char path[64];
    char name[80];
    int i;

    if (!mac || !mac[0])
        return false;

    for (i = 0; i < 16; i++)
    {
        FILE *f;

        snprintf(path, sizeof(path), "/sys/class/input/input%d/name", i);
        f = fopen(path, "r");
        if (!f)
            continue;

        name[0] = '\0';
        if (fgets(name, sizeof(name), f))
        {
            char *nl = strchr(name, '\n');

            if (nl)
                *nl = '\0';
        }
        fclose(f);

        if (name[0] && !strcasecmp(name, mac))
            return true;
    }

    return false;
}

/* True while any peer (MAC-named) input device exists; used as a fallback
 * when the selected address is not known. */
static bool bt_any_peer_input(void)
{
    char path[64];
    char name[80];
    int i;

    for (i = 0; i < 16; i++)
    {
        FILE *f;

        snprintf(path, sizeof(path), "/sys/class/input/input%d/name", i);
        f = fopen(path, "r");
        if (!f)
            continue;

        name[0] = '\0';
        if (fgets(name, sizeof(name), f))
        {
            char *nl = strchr(name, '\n');

            if (nl)
                *nl = '\0';
        }
        fclose(f);

        /* Peer devices are named after their MAC address. */
        if (strlen(name) == 17 && name[2] == ':' && name[5] == ':')
            return true;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Output watchdog                                                     */
/*                                                                     */
/* When the earpieces are switched off the stock plugin PCM goes quiet */
/* and never errors, which leaves the engine hanging on a dead bluetooth*/
/* sink until the user disconnects by hand.  This cooperative Rockbox   */
/* thread notices the peer is gone and falls back to the wired output  */
/* so playback carries on.  Rockbox threads share the UI's host thread, */
/* so re-opening the PCM here is safe.  The link is checked through the */
/* peer's input device (no forking, no PCM calls).                     */
static void bt_watchdog(void)
{
    long last_check = 0;
    long last_fallback = 0;

    while (bt_watchdog_run)
    {
        sleep(HZ / 2);

        if (bt_busy)
            continue;

        /* Never fight the receive path: while it is active the radio
         * belongs to it and the output stays on the wired jack. */
        if (bt_input_active())
            continue;

        if (!TIME_AFTER(current_tick, last_check + 2 * HZ))
            continue;
        last_check = current_tick;

        if (pcm_alsa_is_bluetooth_active())
        {
            bool lost = pcm_alsa_bt_link_lost() ||
                        (bt_selected_mac[0] ? !bt_peer_linked(bt_selected_mac)
                                            : !bt_any_peer_input());

            if (lost && TIME_AFTER(current_tick, last_fallback + 2 * HZ))
            {
                last_fallback = current_tick;
                pcm_alsa_bt_link_lost_clear();
                bt_route_to_local(false);
                /* Do NOT reset the controller here -- the vendor stack
                 * needs the adapter alive to auto-reconnect the earpiece. */
            }
        }
        else if (bt_prefer && bt_selected_mac[0] &&
                 bt_peer_linked(bt_selected_mac))
        {
            /* The earpieces came back and the vendor stack reconnected
             * them: probe the A2DP transport and pick the bluetooth
             * output up again.  bt_route_auto probes internally so no
             * extra cooldown is needed here. */
            char mac[BT_AUDIO_MAC_LEN];

            snprintf(mac, sizeof(mac), "%s", bt_selected_mac);
            bt_route_auto(mac);
        }
    }

    /* Exiting: the starter may create a fresh instance. */
    bt_watchdog_started = false;
}

static long bt_watchdog_stack[(DEFAULT_STACK_SIZE + 0x2000) / sizeof(long)];

/* Started when bluetooth output is in use and stopped again when the user
 * disconnects, so no checking happens while the radio is off. */
static void bt_watchdog_start(void)
{
    bt_watchdog_run = true;

    if (bt_watchdog_started)
        return;

    if (create_thread(bt_watchdog, bt_watchdog_stack,
                      sizeof(bt_watchdog_stack), 0, "bt_watch"
                      IF_PRIO(, PRIORITY_USER_INTERFACE)
                      IF_COP(, CPU)) > 0)
        bt_watchdog_started = true;
    else
        bt_watchdog_run = false;
}

static void bt_watchdog_stop(void)
{
    bt_watchdog_run = false;   /* the thread exits on its next wake-up */
}

/* statusbar glyph query (statusbar_rf.h) */
bool statusbar_rf_bt_on(void)
{
    return bt_stack_on;
}

/* ------------------------------------------------------------------ */
/* generic framework contract (firmware/export/bt_audio.h)             */
/* ------------------------------------------------------------------ */

bool bt_audio_prepare(void)
{
    return bt_prepare_stack();
}

bool bt_audio_stack_on(void)
{
    return bt_stack_on;
}

bool bt_audio_radio_on(void)
{
    return bt_radio_on();
}

void bt_audio_power_off(void)
{
    bt_power_off();
}

void bt_audio_reset_stack(void)
{
    bt_reset_stack();
}

int bt_audio_list(struct bt_audio_dev *out, int max)
{
    return bt_load_devices_via_sys_list(out, max);
}

int bt_audio_scan(struct bt_audio_dev *out, int count, int max)
{
    return bt_scan_and_merge_devices(out, count, max);
}

bool bt_audio_current(char *mac, size_t len)
{
    return bt_current_connection(mac, len);
}

bool bt_audio_linked(const char *mac)
{
    return bt_peer_linked(mac);
}

void bt_audio_connect(const struct bt_audio_dev *dev)
{
    bt_connect_device(dev);
}

void bt_audio_unpair(const char *mac)
{
    bt_delete_device(mac);
}

void bt_audio_disconnect(void)
{
    bt_disconnect();
}

/* Drop the output route and the peer link but keep the radio powered. */
void bt_audio_release(void)
{
    char cmd[96], reply[BT_SYS_REPLY_MAX];

    if (!bt_selected_mac[0])
        return;

    snprintf(cmd, sizeof(cmd), "BT:DISCONNECT:%s", bt_selected_mac);
    bt_sys_command(cmd, reply, sizeof(reply));
    bt_prefer = false;
    bt_watchdog_stop();
    bt_route_to_local(false);
    bt_set_selected_mac(NULL);
}

void bt_audio_disconnect_peer(const char *mac)
{
    char cmd[96];
    char reply[BT_SYS_REPLY_MAX];

    if (!mac || !mac[0])
        return;

    snprintf(cmd, sizeof(cmd), "BT:DISCONNECT:%s", mac);
    bt_sys_command(cmd, reply, sizeof(reply));
}

const char *bt_audio_codec(void)
{
    return bt_get_codec();
}

bool bt_audio_output_active(void)
{
    return pcm_alsa_is_bluetooth_active();
}

void bt_audio_watchdog_start(void)
{
    bt_watchdog_start();
}

void bt_audio_watchdog_stop(void)
{
    bt_watchdog_stop();
}

void bt_audio_busy(bool on)
{
    bt_busy = on;
}

const char *bt_audio_selected(void)
{
    return bt_selected_mac;
}

#endif /* CAYIN_N3PRO */
