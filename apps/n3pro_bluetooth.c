/***************************************************************************
 * Bluetooth audio (A2DP source) for the Cayin N3Pro hosted port.
 *
 * Follows the HiBy R1 port's design: everything runs synchronously on the
 * menu thread -- there is no background thread and no link watcher.  The
 * stock HiByOS bluetooth stack is driven through the vendor /var/run/
 * sys_server socket (BT:LIST / BT:SCAN / BT:PAIR / BT:CONNECT / ...), and
 * the A2DP audio is played through the stock ALSA plugin by writing its
 * peer address into /etc/asound.conf and re-opening the playback PCM.
 ****************************************************************************/
#include "config.h"

#ifdef CAYIN_N3PRO

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/stat.h>

#include "kernel.h"
#include "audio.h"
#include "menu.h"
#include "splash.h"
#include "lang.h"
#include "settings.h"
#include "sound.h"
#include "gui/list.h"
#include "pcm-alsa.h"
#include "n3pro-bt-pcm.h"

/* str() yields const unsigned char *; keep the string call sites simple. */
static const char *bt_str(int id)
{
    return (const char *)str(id);
}

#define BT_MAX_DEVICES 32
#define BT_NAME_LEN 80
#define BT_LOCAL_PLAYBACK_DEVICE "plughw:0,0"
#define BT_ASOUND_CONF "/etc/asound.conf"
#define BT_AUDIO_CONF "/etc/bluetooth/audio.conf"
#define BT_SYS_SOCKET "/var/run/sys_server"
#define BT_LIST_FILE "/data/bt_list.txt"
#define BT_SCAN_FILE "/data/bt_scan.txt"
#define BT_SYS_REPLY_MAX 128
#define BT_DEVICE_PICK_CANCEL (-1)
#define BT_DEVICE_PICK_SCAN (-2)

struct bt_device
{
    char mac[18];
    char name[BT_NAME_LEN];
    bool paired;
};

struct bt_device_menu_data
{
    struct bt_device *devices;
    int count;
    bool include_scan_item;
    char connected_mac[18];
};

static char bt_selected_mac[18];
static bool bt_prefer = false;   /* the user wants the bluetooth output */

/* Output watchdog state (see bt_watchdog()). */
static bool bt_want = false;   /* the user wants bluetooth output */
static bool bt_busy = false;   /* a menu-driven route is in progress */
static bool bt_watchdog_started = false;
static volatile bool bt_watchdog_run = false;

static void bt_watchdog_start(void);
static void bt_watchdog_stop(void);

static bool bt_prepare_stack(void);
static void bt_connect_device(const struct bt_device *device);

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

static int bt_add_device_unique_ex(struct bt_device *devices, int count, int max_devices,
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
    const struct bt_device *da = a;
    const struct bt_device *db = b;

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
                                          struct bt_device *devices,
                                          int max_devices,
                                          bool *ready)
{
    FILE *fp;
    char line[512];
    char mac[18] = "";
    char name[BT_NAME_LEN] = "";
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

static int bt_load_devices_from_bt_list_file(struct bt_device *devices, int max_devices,
                                             bool *ready)
{
    return bt_load_devices_from_json_file(BT_LIST_FILE, devices, max_devices, ready);
}

static int bt_merge_devices_from_bt_scan_file(struct bt_device *devices, int count,
                                              int max_devices, bool *ready)
{
    static struct bt_device scanned[BT_MAX_DEVICES];
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

static int bt_load_devices_via_sys_list(struct bt_device *devices, int max_devices)
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
static int bt_scan_and_merge_devices(struct bt_device *devices, int count, int max_devices)
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

static int bt_choose_device(const char *title, struct bt_device *devices, int count,
                            bool include_scan_item)
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
    bt_current_connection(data.connected_mac, sizeof(data.connected_mac));

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

static void bt_set_selected_mac(const char *mac)
{
    if (mac && mac[0])
        snprintf(bt_selected_mac, sizeof(bt_selected_mac), "%s", mac);
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

/* Write the peer the stock bluetooth ALSA plugin must stream to.  The
 * vendor player rewrites this file at runtime, so we do the same. */
static void bt_write_asound(const char *mac)
{
    FILE *f = fopen(BT_ASOUND_CONF, "w");

    if (!f)
        return;

    fprintf(f, "pcm.bluetooth {\n");
    fprintf(f, "    type bluetooth\n");
    fprintf(f, "    device \"%s\"\n", mac);
    fprintf(f, "    profile \"a2dp\"\n");
    fprintf(f, "}\n");

    /* The AK4493 hardware volume does not touch the bluetooth path, so
     * wrap the plugin in a userspace softvol whose "Bluetooth Vol" mixer
     * control is driven by audiohw_set_volume() while this route is up. */
    fprintf(f, "pcm.%s {\n", N3PRO_BT_DEVICE);
    fprintf(f, "    type softvol\n");
    fprintf(f, "    slave.pcm {\n");
    fprintf(f, "        type bluetooth\n");
    fprintf(f, "        device \"%s\"\n", mac);
    fprintf(f, "        profile \"a2dp\"\n");
    fprintf(f, "    }\n");
    fprintf(f, "    control {\n");
    fprintf(f, "        name \"Bluetooth Vol\"\n");
    fprintf(f, "        card 0\n");
    fprintf(f, "    }\n");
    /* The audible window of a digital attenuation is about 50 dB; use it
     * all so the volume steps do not bunch up in the top few dB. */
    fprintf(f, "    min_dB -50.0\n");
    fprintf(f, "    max_dB 0.0\n");
    fprintf(f, "}\n");

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
        bt_set_codec(bt_codec_pref[i]);

        /* The codec is fixed when the A2DP transport is set up, so an
         * attempt after the first needs a fresh connection; the first one
         * runs on the link the caller has just established (repeatedly
         * toggling the link wedges the stock bluetooth stack). */
        if (i > 0)
        {
            snprintf(cmd, sizeof(cmd),
                     "/usr/bin/bt-connect -d %s >/dev/null 2>&1", mac);
            system(cmd);
            sleep(HZ / 2);
        }

        snprintf(cmd, sizeof(cmd),
                 "/usr/bin/bt-connect -c %s >/dev/null 2>&1", mac);
        system(cmd);

        for (t = 0; t < 6; t++)
        {
            if (pcm_alsa_bt_probe() == 0)
                return bt_codec_pref[i];
            sleep(HZ / 4);
        }
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

static void bt_route_to_local(bool show_message)
{
    pcm_alsa_switch_playback_device(BT_LOCAL_PLAYBACK_DEVICE);
    bt_kick_audio_if_playing();
    bt_vol_enter_local();
    if (show_message)
        splash(HZ, "Output: Local");
}

static void bt_route_log(const char *msg)
{
    FILE *f = fopen("/mnt/sd_0/.rockbox/bt_route.log", "a");

    if (!f)
        return;

    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
}

static bool bt_route_to_bluetooth(const char *mac)
{
    char line[96];
    int rc;

    if (!mac || !mac[0])
        return false;

    bt_write_asound(mac);
    snprintf(line, sizeof(line), "route: mac=%s asound written", mac);
    bt_route_log(line);

    /* Pick the highest codec the earphone supports; on success the A2DP
     * transport is already up, so no extra wait is needed. */
    if (!bt_pick_codec(mac))
    {
        bt_route_log("route: codec pick failed");
        bt_route_to_local(false);
        return false;
    }

    bt_route_log("route: codec ok");

    rc = pcm_alsa_switch_playback_device(N3PRO_BT_DEVICE);
    snprintf(line, sizeof(line), "route: switch rc=%d active=%d",
             rc, pcm_alsa_is_bluetooth_active());
    bt_route_log(line);

    if (rc == 0)
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
    if (!mac || !mac[0])
        return false;

    bt_write_asound(mac);

    if (pcm_alsa_switch_playback_device(N3PRO_BT_DEVICE) == 0)
    {
        bt_set_selected_mac(mac);
        bt_vol_enter_bt();
        bt_kick_audio_if_playing();
        bt_route_log("watchdog: peer back, routed to bluetooth");
        return true;
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
            return true;

        reply[0] = '\0';
        bt_sys_command("BT:ON", reply, sizeof(reply));

        sleep(HZ / 5);
    }

    return false;
}

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

/* Per-device action menu: pick Connect or Delete (unpair). */
static void bt_device_menu(struct bt_device *devices, int *count, int idx)
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
        bt_connect_device(&devices[idx]);
    }
    else if (info.selection == 1)
    {
        bt_delete_device(devices[idx].mac);
        memmove(&devices[idx], &devices[idx + 1],
                (size_t)(*count - idx - 1) * sizeof(devices[0]));
        (*count)--;
    }
}

static void bt_show_devices(void)
{
    static struct bt_device devices[BT_MAX_DEVICES];
    int count;
    int idx;

    if (!bt_prepare_stack())
    {
        splash(HZ * 2, bt_str(LANG_BT_UNAVAILABLE));
        return;
    }

    splash(0, bt_str(LANG_BT_LOADING));
    count = bt_load_devices_via_sys_list(devices, BT_MAX_DEVICES);
    if (count <= 0)
    {
        splash(0, bt_str(LANG_BT_SCANNING));
        count = bt_scan_and_merge_devices(devices, count, BT_MAX_DEVICES);
    }

    while (1)
    {
        idx = bt_choose_device("Devices", devices, count, true);
        if (idx == BT_DEVICE_PICK_SCAN)
        {
            splash(0, bt_str(LANG_BT_SCANNING));
            count = bt_scan_and_merge_devices(devices, count, BT_MAX_DEVICES);
            if (count <= 0)
                splash(HZ, bt_str(LANG_BT_NO_DEVICES));
            continue;
        }

        if (idx < 0 || idx >= count)
            return;

        bt_device_menu(devices, &count, idx);
    }
}

static void bt_connect_device(const struct bt_device *device)
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
        snprintf(cmd, sizeof(cmd), "BT:CONNECT:%s", mac);
        ctl_rc = bt_sys_command(cmd, reply, sizeof(reply));
        if (ctl_rc == 0 && bt_sys_reply_ok(reply, "BT:CONNECT"))
        {
            sleep(HZ / 2);
            routed = bt_route_to_bluetooth(mac);
        }
    }

    if (routed)
    {
        bt_want = true;
        splash(HZ, bt_str(LANG_BT_CONNECTED));
    }
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
    long last_route = 0;

    while (bt_watchdog_run)
    {
        sleep(HZ / 2);

        if (bt_busy)
            continue;

        if (!TIME_AFTER(current_tick, last_check + 2 * HZ))
            continue;
        last_check = current_tick;

        if (pcm_alsa_is_bluetooth_active())
        {
            if ((pcm_alsa_bt_link_lost() ||
                 (bt_selected_mac[0] && !bt_peer_linked(bt_selected_mac))) &&
                TIME_AFTER(current_tick, last_fallback + 5 * HZ))
            {
                last_fallback = current_tick;
                pcm_alsa_bt_link_lost_clear();
                bt_route_log("watchdog: peer gone, back to local");
                bt_route_to_local(false);
            }
        }
        else if (bt_prefer && bt_selected_mac[0] &&
                 bt_peer_linked(bt_selected_mac) &&
                 TIME_AFTER(current_tick, last_route + 5 * HZ))
        {
            /* The earpieces came back and the vendor stack reconnected
             * them: pick the bluetooth output up again by itself. */
            last_route = current_tick;
            bt_route_auto(bt_selected_mac);
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

static void bt_show_status(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, bt_str(LANG_BT_STATUS), 0, NULL);
    simplelist_reset_lines();

    if (bt_selected_mac[0])
    {
        simplelist_addline("%s: %s", bt_str(LANG_BT_MAC), bt_selected_mac);
        /* Real link state from the peer's AVRCP input device; opening the
         * PCM just to test readiness would block on the bluetooth stack. */
        simplelist_addline("%s: %s", bt_str(LANG_BT_LINK),
                           bt_peer_linked(bt_selected_mac)
                               ? bt_str(LANG_ON) : bt_str(LANG_OFF));
        simplelist_addline("%s: %s", bt_str(LANG_BT_CODEC), bt_get_codec());
    }
    else
    {
        simplelist_addline("%s", bt_str(LANG_BT_DEVICE_LOCAL));
    }

    simplelist_addline("%s: %s", bt_str(LANG_BT_OUTPUT),
                       pcm_alsa_is_bluetooth_active()
                           ? bt_str(LANG_BLUETOOTH) : bt_str(LANG_BT_LOCAL));
    simplelist_addline("%s: %s", bt_str(LANG_BT_RADIO),
                       bt_radio_on() ? bt_str(LANG_ON) : bt_str(LANG_OFF));

    info.count = simplelist_get_line_count();
    simplelist_show_list(&info);
}

int n3pro_bluetooth_menu(void)
{
    int action = -1;

    while (true)
    {
        struct simplelist_info info;

        simplelist_info_init(&info, bt_str(LANG_BLUETOOTH), 3, NULL);
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
                bt_busy = true;
                bt_show_devices();
                bt_busy = false;
                break;
            case 2:
                bt_busy = true;
                bt_disconnect();
                bt_busy = false;
                break;
            default:
                break;
        }
    }
    return 0;
}

#endif /* CAYIN_N3PRO */
