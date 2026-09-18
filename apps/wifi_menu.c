/***************************************************************************
 * Generic WiFi menu (framework only).
 *
 * All device interaction goes through the wifi_hal interface; ports
 * provide the implementation in their target directory.  The
 * remembered network (ssid + password) is kept in the Rockbox
 * settings, so it survives reboots and can be edited by hand in
 * .rockbox/config.cfg when the on-device keyboard is inconvenient.
 ****************************************************************************/
#include "config.h"

#ifdef HAVE_WIFI_MENU

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"
#include "thread.h"
#include "action.h"
#include "splash.h"
#include "lang.h"
#include "settings.h"
#include "screen_access.h"
#include "viewport.h"
#include "gui/list.h"
#include "keyboard.h"
#include "yesno.h"
#include "wifi_hal.h"
#include "wifi_menu.h"
#ifdef HAVE_WEB_CONTROL
#include "web_control.h"
#endif

#define WIFI_MAX_NETS  32
#define WIFI_PICK_CANCEL (-1)

static char *wifi_str(int id)
{
    return (char *)str(id);
}

/* ------------------------------------------------------------------ */
/* shared list plumbing                                                */
/* ------------------------------------------------------------------ */

struct wifi_id_list
{
    const int *ids;
    int count;
};

static const char *wifi_id_name_cb(int selected, void *data,
                             char *buffer, size_t buffer_len)
{
    struct wifi_id_list *list = data;
    if (!list || selected < 0 || selected >= list->count)
        return (char *)"";
    snprintf(buffer, buffer_len, "%s", wifi_str(list->ids[selected]));
    return buffer;
}

/* ------------------------------------------------------------------ */
/* remembered-network bookkeeping (generic settings)                   */
/* ------------------------------------------------------------------ */

static void wifi_remember(const char *ssid, const char *psk)
{
    snprintf(global_settings.wifi_ssid, sizeof(global_settings.wifi_ssid),
             "%s", ssid);
    snprintf(global_settings.wifi_psk, sizeof(global_settings.wifi_psk),
             "%s", psk ? psk : "");
    settings_save();
}

static void wifi_forget_remembered(const char *ssid)
{
    if (strcmp(ssid, global_settings.wifi_ssid) == 0)
    {
        global_settings.wifi_ssid[0] = '\0';
        global_settings.wifi_psk[0] = '\0';
        settings_save();
    }
}

/* ------------------------------------------------------------------ */
/* flows                                                               */
/* ------------------------------------------------------------------ */

static bool wifi_ensure_up(void)
{
    if (wifi_hal_is_up())
        return true;
    splash(0, wifi_str(LANG_WIFI_STARTING));
    if (!wifi_hal_power_on())
    {
        splash(HZ * 2, wifi_str(LANG_WIFI_FAILED));
        return false;
    }
    return true;
}

/* Power on and attach the remembered network, if any. */
static bool wifi_flow_power_on(void)
{
    splash(0, wifi_str(LANG_WIFI_STARTING));
    if (!wifi_hal_power_on())
    {
        splash(HZ * 2, wifi_str(LANG_WIFI_FAILED));
        return false;
    }

    if (global_settings.wifi_ssid[0])
    {
        splash(0, wifi_str(LANG_WIFI_CONNECTING));
        if (wifi_hal_connect(global_settings.wifi_ssid,
                             global_settings.wifi_psk))
            splash(HZ * 2, wifi_str(LANG_WIFI_CONNECTED));
        else
            splash(HZ * 2, wifi_str(LANG_WIFI_NOT_CONNECTED));
    }
    else
        splash(HZ * 2, wifi_str(LANG_WIFI_ON));
    return true;
}

static const char *wifi_scan_name_cb(int selected, void *data,
                               char *buffer, size_t buffer_len)
{
    struct wifi_hal_net *nets = data;
    if (!nets || selected < 0)
        return (char *)"";
    snprintf(buffer, buffer_len, "%s", nets[selected].ssid);
    return buffer;
}

static const char *wifi_saved_name_cb(int selected, void *data,
                                char *buffer, size_t buffer_len)
{
    struct wifi_hal_saved *nets = data;
    if (!nets || selected < 0)
        return (char *)"";
    snprintf(buffer, buffer_len, "%s%s", nets[selected].ssid,
             nets[selected].current ? " *" : "");
    return buffer;
}

/* Scan, pick a network, ask for a password and connect. */
static void wifi_flow_scan(void)
{
    static struct wifi_hal_net nets[WIFI_MAX_NETS];
    int count;

    if (!wifi_ensure_up())
        return;

    splash(0, wifi_str(LANG_WIFI_SCANNING));
    count = wifi_hal_scan(nets, WIFI_MAX_NETS);
    if (count <= 0)
    {
        splash(HZ * 2, wifi_str(LANG_WIFI_NO_NETS));
        return;
    }

    struct simplelist_info info;
    simplelist_info_init(&info, wifi_str(LANG_WIFI_SCAN), count, nets);
    info.get_name = wifi_scan_name_cb;
    simplelist_show_list(&info);
    int pick = info.selection;
    if (pick < 0 || pick >= count)
        return;

    char psk[WIFI_HAL_PSK_LEN];
    psk[0] = '\0';
    if (nets[pick].secure)
    {
        splash(HZ, wifi_str(LANG_WIFI_PASSWORD));
        if (kbd_input(psk, sizeof(psk), NULL) < 0)
            return;
    }

    splash(0, wifi_str(LANG_WIFI_CONNECTING));
    if (wifi_hal_connect(nets[pick].ssid, psk))
    {
        wifi_remember(nets[pick].ssid, psk);
        splash(HZ * 2, wifi_str(LANG_WIFI_CONNECTED));
    }
    else
        splash(HZ * 2, wifi_str(LANG_WIFI_FAILED));
}

/* Pick a saved network, then connect to it or delete it. */
static void wifi_flow_saved(void)
{
    static struct wifi_hal_saved nets[WIFI_MAX_NETS];
    int count;

    if (!wifi_ensure_up())
        return;

    count = wifi_hal_saved(nets, WIFI_MAX_NETS);
    if (count <= 0)
    {
        splash(HZ * 2, wifi_str(LANG_WIFI_NO_NETS));
        return;
    }

    struct simplelist_info info;
    simplelist_info_init(&info, wifi_str(LANG_WIFI_SAVED), count, nets);
    info.get_name = wifi_saved_name_cb;
    simplelist_show_list(&info);
    int pick = info.selection;
    if (pick < 0 || pick >= count)
        return;

    static const int act_ids[3] = { LANG_WIFI_CONNECT, LANG_WIFI_FORGET,
                                    LANG_CANCEL };
    struct wifi_id_list act = { act_ids, 3 };
    struct simplelist_info act_info;
    simplelist_info_init(&act_info, nets[pick].ssid, 3, &act);
    act_info.get_name = wifi_id_name_cb;
    simplelist_show_list(&act_info);
    if (act_info.selection < 0)
        return;

    if (act_info.selection == 0)
    {
        /* keep the remembered password when it belongs to this ssid */
        const char *psk = strcmp(nets[pick].ssid, global_settings.wifi_ssid) == 0
                              ? global_settings.wifi_psk : "";
        splash(0, wifi_str(LANG_WIFI_CONNECTING));
        if (wifi_hal_connect(nets[pick].ssid, psk))
        {
            wifi_remember(nets[pick].ssid, psk);
            splash(HZ * 2, wifi_str(LANG_WIFI_CONNECTED));
        }
        else
            splash(HZ * 2, wifi_str(LANG_WIFI_FAILED));
    }
    else if (act_info.selection == 1)
    {
        if (wifi_hal_forget(nets[pick].ssid))
            wifi_forget_remembered(nets[pick].ssid);
    }
}

/* ------------------------------------------------------------------ */
/* status screen                                                       */
/* ------------------------------------------------------------------ */

#define WIFI_STATUS_ROWS 5

static struct wifi_hal_status wifi_status_snap;
static volatile int  wifi_status_seq;
static volatile bool wifi_status_run;
static volatile bool wifi_status_started;

/* Cooperative monitor thread: polls the status into a snapshot so the
 * UI loop never blocks on the HAL. Started on entering the status
 * screen, stops itself when the screen is left. */
static void wifi_status_thread(void)
{
    while (wifi_status_run)
    {
        struct wifi_hal_status st;
        if (!wifi_hal_get_status(&st))
            memset(&st, 0, sizeof(st));
        wifi_status_snap = st;
        wifi_status_seq++;
        sleep(HZ * 2);
    }
    wifi_status_started = false;
}

static long wifi_status_stack[(DEFAULT_STACK_SIZE + 0x1000) / sizeof(long)];

/* Format one status row from the current snapshot. */
static void wifi_status_row(int row, char *buffer, size_t buffer_len)
{
    struct wifi_hal_status st = wifi_status_snap;

    if (row < 0 || row >= WIFI_STATUS_ROWS)
    {
        buffer[0] = '\0';
        return;
    }

    switch (row)
    {
        case 0:
            snprintf(buffer, buffer_len, "%s: %s", wifi_str(LANG_WIFI_RADIO),
                     st.radio ? wifi_str(LANG_WIFI_RADIO_ON)
                              : wifi_str(LANG_WIFI_RADIO_OFF));
            break;
        case 1:
        {
            int state_txt;
            switch (st.state)
            {
                case WIFI_HAL_CONNECTED:
                    state_txt = LANG_WIFI_CONNECTED; break;
                case WIFI_HAL_CONNECTING:
                    state_txt = LANG_WIFI_SCANNING; break;
                case WIFI_HAL_IDLE:
                    state_txt = LANG_WIFI_NOT_CONNECTED; break;
                default:
                    state_txt = LANG_WIFI_OFF; break;
            }
            snprintf(buffer, buffer_len, "%s: %s", wifi_str(LANG_WIFI_STATUS),
                     wifi_str(state_txt));
            break;
        }
        case 2:
            snprintf(buffer, buffer_len, "%s: %s", wifi_str(LANG_WIFI_SSID),
                     st.ssid[0] ? st.ssid : "-");
            break;
        case 3:
            snprintf(buffer, buffer_len, "%s: %s", wifi_str(LANG_WIFI_IP),
                     st.ip[0] ? st.ip : "-");
            break;
        default:
            /* quantised to 5 dBm steps so normal jitter does not
             * trigger a redraw on every poll */
            if (st.rssi)
                snprintf(buffer, buffer_len, "%s: %d dBm",
                         wifi_str(LANG_WIFI_SIGNAL), (st.rssi / 5) * 5);
            else
                snprintf(buffer, buffer_len, "%s: -",
                         wifi_str(LANG_WIFI_SIGNAL));
            break;
    }
}

static const char *wifi_status_name_cb(int selected, void *data,
                                       char *buffer, size_t buffer_len)
{
    (void)data;
    wifi_status_row(selected, buffer, buffer_len);
    return buffer;
}

static int wifi_status_action_cb(int action, struct gui_synclist *lists)
{
    static char last[WIFI_STATUS_ROWS][64];

    (void)lists;

    if (action == ACTION_STD_OK)
        return ACTION_NONE;           /* status rows do nothing */

    if (action == ACTION_NONE)
    {
        /* Input timeout: redraw only when something changed. */
        char row[64];
        bool changed = false;

        for (int i = 0; i < WIFI_STATUS_ROWS; i++)
        {
            wifi_status_row(i, row, sizeof(row));
            if (strcmp(row, last[i]) != 0)
                changed = true;
        }
        if (changed)
        {
            for (int i = 0; i < WIFI_STATUS_ROWS; i++)
                wifi_status_row(i, last[i], sizeof(last[i]));
            return ACTION_REDRAW;
        }
    }
    return action;
}

static void wifi_status_screen(void)
{
    wifi_status_run = true;
    if (!wifi_status_started &&
        create_thread(wifi_status_thread, wifi_status_stack,
                      sizeof(wifi_status_stack), 0, "wifi status"
                      IF_PRIO(, PRIORITY_USER_INTERFACE)
                      IF_COP(, CPU)) > 0)
        wifi_status_started = true;
    else if (!wifi_status_started)
        wifi_status_run = false;

    struct simplelist_info info;
    simplelist_info_init(&info, wifi_str(LANG_WIFI_STATUS), WIFI_STATUS_ROWS,
                         NULL);
    info.get_name = wifi_status_name_cb;
    info.action_callback = wifi_status_action_cb;
    info.timeout = HZ * 2;
    info.selection = 0;
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);

    wifi_status_run = false;
}

/* ------------------------------------------------------------------ */
/* reset                                                               */
/* ------------------------------------------------------------------ */

static void wifi_flow_reset(void)
{
    if (!yesno_pop(wifi_str(LANG_WIFI_RESET_CONFIRM)))
        return;

    struct viewport vp;
    struct screen *sc = &screens[SCREEN_MAIN];

    viewport_set_defaults(&vp, SCREEN_MAIN);
    sc->set_viewport(&vp);
    sc->clear_display();
    sc->puts(0, 0, wifi_str(LANG_WIFI_RESETTING));
    sc->update_viewport();
    sc->set_viewport(NULL);

    wifi_hal_reset();
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

static const char *wifi_menu_name_cb(int selected, void *data,
                               char *buffer, size_t buffer_len)
{
    bool on = (bool)(intptr_t)data;
    int ids[6] = { on ? LANG_WIFI_OFF : LANG_WIFI_ON,
                   LANG_WIFI_SCAN, LANG_WIFI_SAVED, LANG_WIFI_STATUS,
                   LANG_WIFI_RESET };
    int count = 5;
#ifdef HAVE_WEB_CONTROL
    ids[5] = LANG_WEB_CONTROL;
    count = 6;
#endif
    if (selected < 0 || selected >= count)
        return (char *)"";
    snprintf(buffer, buffer_len, "%s", wifi_str(ids[selected]));
    return buffer;
}

int wifi_menu(void)
{
    while (true)
    {
        bool on = wifi_hal_is_up();
        int count = 5;
#ifdef HAVE_WEB_CONTROL
        count = 6;
#endif

        struct simplelist_info info;
        simplelist_info_init(&info, wifi_str(LANG_WIFI), count,
                             (void *)(intptr_t)on);
        info.get_name = wifi_menu_name_cb;
        info.selection = -1;
        info.title_icon = Icon_Submenu;

        simplelist_show_list(&info);
        if (info.selection < 0)
            break;

        switch (info.selection)
        {
            case 0:
                if (on)
                {
                    splash(0, wifi_str(LANG_WIFI_STOPPING));
#ifdef HAVE_WEB_CONTROL
                    web_control_stop();
#endif
                    wifi_hal_power_off();
                }
                else
                    wifi_flow_power_on();
                break;
            case 1:
                wifi_flow_scan();
                break;
            case 2:
                wifi_flow_saved();
                break;
            case 3:
                wifi_status_screen();
                break;
            case 4:
#ifdef HAVE_WEB_CONTROL
                web_control_stop();
#endif
                wifi_flow_reset();
                break;
#ifdef HAVE_WEB_CONTROL
            case 5:
                web_control_screen();
                break;
#endif
        }
    }
    return 0;
}

#endif /* HAVE_WIFI_MENU */
