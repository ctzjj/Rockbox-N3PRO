/***************************************************************************
 * WiFi HAL implementation for the Cayin N3Pro hosted port.
 *
 * The vendor scripts bring the interface up and down (/sbin/wifi_on.sh
 * and /sbin/wifi_off.sh start wpa_supplicant and udhcpc); networks are
 * then managed through /sbin/wpa_cli, which talks to the supplicant's
 * control socket.
 ****************************************************************************/
#include "config.h"

#ifdef CAYIN_N3PRO

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "kernel.h"
#include "wifi_hal.h"
#include "statusbar_rf.h"

#define WIFI_WPA_CLI   "/sbin/wpa_cli"
#define WIFI_ON_SH     "/sbin/wifi_on.sh"
#define WIFI_OFF_SH    "/sbin/wifi_off.sh"
#define WIFI_CMD_MAX   300
#define WIFI_BUF_MAX   4096
#define WIFI_ID_NONE   (-1)
#define WIFI_CONNECT_TIMEOUT_S 15

/* ------------------------------------------------------------------ */
/* wpa_cli plumbing                                                    */
/* ------------------------------------------------------------------ */

/* Run wpa_cli and capture the raw (possibly multi-line) reply.
 * Returns false when the control socket is unreachable. */
static bool wifi_wpa(const char *args, char *out, size_t outsz)
{
    char cmd[WIFI_CMD_MAX];
    snprintf(cmd, sizeof(cmd), "%s %s 2>/dev/null", WIFI_WPA_CLI, args);

    FILE *f = popen(cmd, "r");
    if (!f)
        return false;

    size_t got = 0;
    if (out && outsz > 1)
    {
        out[0] = '\0';
        while (got + 1 < outsz && fgets(out + got, outsz - got, f))
            got += strlen(out + got);
    }
    pclose(f);

    return got != 0;
}

/* First reply line, skipping the interface banner, trimmed. */
static bool wifi_wpa_line(const char *args, char *out, size_t outsz)
{
    char buf[256];
    if (!wifi_wpa(args, buf, sizeof(buf)))
        return false;

    char *line = buf;
    if (strncmp(line, "Selected interface", 18) == 0)
    {
        line = strchr(line, '\n');
        if (!line)
            return false;
        line++;
    }
    snprintf(out, outsz, "%.*s", (int)outsz - 1, line);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return out[0] != '\0';
}

static bool wifi_wpa_ok(const char *args)
{
    char reply[64];
    return wifi_wpa_line(args, reply, sizeof(reply)) &&
           strncmp(reply, "OK", 2) == 0;
}

/* Extract "key=value" from a multi-line reply. */
static bool wifi_get_field(const char *reply, const char *key,
                           char *out, size_t outsz)
{
    char match[32];
    snprintf(match, sizeof(match), "%s=", key);

    const char *line = reply;
    while (line && *line)
    {
        if (strncmp(line, match, strlen(match)) == 0)
        {
            const char *val = line + strlen(match);
            size_t n = 0;
            while (val[n] && val[n] != '\n' && n + 1 < outsz)
            {
                out[n] = val[n];
                n++;
            }
            out[n] = '\0';
            return true;
        }
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return false;
}

static bool wifi_status_reply(char *buf, size_t bufsz)
{
    return wifi_wpa("status", buf, bufsz) &&
           strstr(buf, "wpa_state=") != NULL;
}

/* ------------------------------------------------------------------ */
/* saved-network table                                                 */
/* ------------------------------------------------------------------ */

static int wifi_find_network(const char *ssid)
{
    static char buf[WIFI_BUF_MAX];
    if (!wifi_wpa("list_networks", buf, sizeof(buf)))
        return WIFI_ID_NONE;

    int id = WIFI_ID_NONE;
    char *line = strtok(buf, "\n");
    while (line)
    {
        if (strncmp(line, "network id", 10) != 0 &&
            strncmp(line, "Selected interface", 18) != 0)
        {
            char *tab = strchr(line, '\t');
            if (tab)
            {
                /* ssid field only: cut at the bssid column */
                char *end = strchr(tab + 1, '\t');
                if (end)
                    *end = '\0';
                *tab = '\0';
                if (strcmp(tab + 1, ssid) == 0)
                {
                    id = atoi(line);
                    break;
                }
            }
        }
        line = strtok(NULL, "\n");
    }
    return id;
}

/* ------------------------------------------------------------------ */
/* HAL                                                                 */
/* ------------------------------------------------------------------ */

bool wifi_hal_is_up(void)
{
    static char buf[WIFI_BUF_MAX];
    return wifi_status_reply(buf, sizeof(buf));
}

bool wifi_hal_power_on(void)
{
    if (system(WIFI_ON_SH " >/dev/null 2>&1") != 0)
        return false;
    sleep(HZ / 2);
    return wifi_hal_is_up();
}

void wifi_hal_power_off(void)
{
    system(WIFI_OFF_SH " >/dev/null 2>&1");
    sleep(HZ / 2);
}

int wifi_hal_scan(struct wifi_hal_net *nets, int max)
{
    static char buf[WIFI_BUF_MAX];
    if (!nets || max <= 0)
        return 0;

    wifi_wpa_ok("scan");
    sleep(HZ * 2);

    if (!wifi_wpa("scan_results", buf, sizeof(buf)))
        return 0;

    /* bssid \t freq \t signal \t flags \t ssid */
    int count = 0;
    char *line = strtok(buf, "\n");
    while (line && count < max)
    {
        if (strncmp(line, "bssid", 5) != 0 &&
            strncmp(line, "Selected interface", 18) != 0)
        {
            char *f1 = strchr(line, '\t');
            char *f2 = f1 ? strchr(f1 + 1, '\t') : NULL;
            char *f3 = f2 ? strchr(f2 + 1, '\t') : NULL;
            char *f4 = f3 ? strchr(f3 + 1, '\t') : NULL;
            if (f4)
            {
                char ssid[WIFI_HAL_SSID_LEN];
                snprintf(ssid, sizeof(ssid), "%s", f4 + 1);
                size_t len = strlen(ssid);
                while (len > 0 && (ssid[len - 1] == '\r' ||
                                   ssid[len - 1] == ' '))
                    ssid[--len] = '\0';

                if (ssid[0] == '\0')
                {
                    line = strtok(NULL, "\n");
                    continue;
                }

                int sig = atoi(f2 + 1);
                bool secure = strstr(f3 + 1, "WPA") != NULL ||
                              strstr(f3 + 1, "RSN") != NULL;

                /* dedup: keep the strongest entry per ssid */
                int i;
                for (i = 0; i < count; i++)
                    if (strcmp(nets[i].ssid, ssid) == 0)
                        break;
                if (i < count)
                {
                    if (sig > nets[i].rssi)
                        nets[i].rssi = sig;
                    line = strtok(NULL, "\n");
                    continue;
                }

                snprintf(nets[count].ssid, WIFI_HAL_SSID_LEN, "%s", ssid);
                nets[count].rssi = sig;
                nets[count].secure = secure;
                count++;
            }
        }
        line = strtok(NULL, "\n");
    }

    /* strongest first */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (nets[j].rssi > nets[i].rssi)
            {
                struct wifi_hal_net tmp = nets[i];
                nets[i] = nets[j];
                nets[j] = tmp;
            }
    return count;
}

int wifi_hal_saved(struct wifi_hal_saved *nets, int max)
{
    static char buf[WIFI_BUF_MAX];
    if (!nets || max <= 0)
        return 0;

    if (!wifi_wpa("list_networks", buf, sizeof(buf)))
        return 0;

    int count = 0;
    char *line = strtok(buf, "\n");
    while (line && count < max)
    {
        if (strncmp(line, "network id", 10) != 0 &&
            strncmp(line, "Selected interface", 18) != 0)
        {
            /* id \t ssid \t bssid \t flags */
            char *tab = strchr(line, '\t');
            char *bssid = tab ? strchr(tab + 1, '\t') : NULL;
            char *flags = bssid ? strchr(bssid + 1, '\t') : NULL;
            if (tab && flags)
            {
                *tab = '\0';
                *flags = '\0';
                nets[count].current = strstr(flags + 1, "CURRENT") != NULL;
                snprintf(nets[count].ssid, WIFI_HAL_SSID_LEN, "%s", tab + 1);
                count++;
            }
        }
        line = strtok(NULL, "\n");
    }
    return count;
}

bool wifi_hal_connect(const char *ssid, const char *psk)
{
    char args[WIFI_CMD_MAX];

    int id = wifi_find_network(ssid);
    if (id != WIFI_ID_NONE)
    {
        snprintf(args, sizeof(args), "select_network %d", id);
        if (!wifi_wpa_ok(args))
            return false;
    }
    else
    {
        char reply[64];
        if (!wifi_wpa_line("add_network", reply, sizeof(reply)))
            return false;
        id = atoi(reply);
        if (id < 0)
            return false;

        snprintf(args, sizeof(args), "set_network %d ssid '\"%s\"'", id,
                 ssid);
        if (!wifi_wpa_ok(args))
            return false;

        if (psk && psk[0])
        {
            snprintf(args, sizeof(args), "set_network %d psk '\"%s\"'", id,
                     psk);
            if (!wifi_wpa_ok(args))
                return false;
        }
        else
        {
            snprintf(args, sizeof(args), "set_network %d key_mgmt NONE", id);
            wifi_wpa_ok(args);
        }

        snprintf(args, sizeof(args), "enable_network %d", id);
        wifi_wpa_ok(args);
    }
    wifi_wpa_ok("save_config");

    static char buf[WIFI_BUF_MAX];
    for (int i = 0; i < WIFI_CONNECT_TIMEOUT_S * 2; i++)
    {
        sleep(HZ / 2);
        if (wifi_status_reply(buf, sizeof(buf)) &&
            strstr(buf, "wpa_state=COMPLETED"))
        {
            /* the udhcpc from wifi_on.sh exits after its first lease;
             * renew one for the newly associated network */
            system("/sbin/udhcpc -i wlan0 -q -n -t 4 -T 2 >/dev/null 2>&1");
            return true;
        }
    }
    return false;
}

bool wifi_hal_forget(const char *ssid)
{
    char args[WIFI_CMD_MAX];

    int id = wifi_find_network(ssid);
    if (id == WIFI_ID_NONE)
        return false;

    snprintf(args, sizeof(args), "remove_network %d", id);
    if (!wifi_wpa_ok(args))
        return false;
    return wifi_wpa_ok("save_config");
}

/* Radio power: the interface operstate reflects what the vendor power
 * scripts do (rfkill stays unblocked on this hardware, so it is not
 * a usable signal here). */
static bool wifi_radio_on(void)
{
    char val[32];
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (!f)
        return false;
    val[0] = '\0';
    if (!fgets(val, sizeof(val), f))
        val[0] = 'd';
    fclose(f);
    return strncmp(val, "down", 4) != 0;
}

void wifi_hal_reset(void)
{
    /* The boot state is: interface down, no supplicant, no dhcp --
     * exactly what the vendor power-off script produces.  rfkill is
     * left untouched so the next power-on works normally. */
    system(WIFI_OFF_SH " >/dev/null 2>&1");
    sleep(HZ / 2);
}

/* statusbar glyph query (statusbar_rf.h) */
bool statusbar_rf_wifi_on(void)
{
    return wifi_radio_on();
}

bool wifi_hal_get_ip(char *out, size_t outsz)
{
    static char buf[WIFI_BUF_MAX];
    if (!out || outsz == 0)
        return false;
    out[0] = '\0';
    if (!wifi_status_reply(buf, sizeof(buf)))
        return false;
    return wifi_get_field(buf, "ip_address", out, outsz);
}

bool wifi_hal_get_status(struct wifi_hal_status *st)
{
    static char buf[WIFI_BUF_MAX];
    if (!st)
        return false;

    memset(st, 0, sizeof(*st));
    st->radio = wifi_radio_on();
    if (!wifi_status_reply(buf, sizeof(buf)))
    {
        st->state = WIFI_HAL_DOWN;
        return true;
    }

    char state[32] = "";
    wifi_get_field(buf, "wpa_state", state, sizeof(state));
    wifi_get_field(buf, "ssid", st->ssid, sizeof(st->ssid));
    wifi_get_field(buf, "ip_address", st->ip, sizeof(st->ip));

    st->state = WIFI_HAL_IDLE;
    if (strcmp(state, "COMPLETED") == 0)
        st->state = WIFI_HAL_CONNECTED;
    else if (strcmp(state, "SCANNING") == 0 ||
             strcmp(state, "ASSOCIATING") == 0 ||
             strcmp(state, "ASSOCIATED") == 0 ||
             strcmp(state, "4WAY_HANDSHAKE") == 0 ||
             strcmp(state, "GROUP_HANDSHAKE") == 0)
        st->state = WIFI_HAL_CONNECTING;

    if (st->state == WIFI_HAL_CONNECTED)
    {
        static char poll[256];
        char rssi[16];
        if (wifi_wpa("signal_poll", poll, sizeof(poll)) &&
            wifi_get_field(poll, "RSSI", rssi, sizeof(rssi)))
            st->rssi = atoi(rssi);
    }
    return true;
}

#endif /* CAYIN_N3PRO */
