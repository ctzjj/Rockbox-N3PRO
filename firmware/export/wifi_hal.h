/***************************************************************************
 * Generic WiFi HAL contract for hosted ports.
 *
 * The generic WiFi menu (apps/wifi_menu.c) talks only to this
 * interface; the port provides the implementation next to its other
 * target drivers.
 ****************************************************************************/
#ifndef WIFI_HAL_H
#define WIFI_HAL_H

#include <stdbool.h>

#define WIFI_HAL_SSID_LEN 33
#define WIFI_HAL_PSK_LEN  64
#define WIFI_HAL_IP_LEN   48

struct wifi_hal_net
{
    char ssid[WIFI_HAL_SSID_LEN];
    int  rssi;   /* dBm; higher = stronger */
    bool secure; /* WPA/RSN advertised */
};

struct wifi_hal_saved
{
    char ssid[WIFI_HAL_SSID_LEN];
    bool current; /* currently associated */
};

enum wifi_hal_state
{
    WIFI_HAL_DOWN = 0,    /* interface/supplicant not running */
    WIFI_HAL_CONNECTED,
    WIFI_HAL_CONNECTING,  /* associating or scanning */
    WIFI_HAL_IDLE,        /* up but not associated */
};

struct wifi_hal_status
{
    enum wifi_hal_state state;
    char ssid[WIFI_HAL_SSID_LEN];
    char ip[WIFI_HAL_IP_LEN];
    int  rssi; /* dBm, 0 = unknown */
    bool radio; /* radio hardware powered */
};

/* Bring the interface and supplicant up / down. */
bool wifi_hal_power_on(void);
void wifi_hal_power_off(void);

/* Reset to the power-on state: interface down, no supplicant, no
 * dhcp client; saved networks are kept. */
void wifi_hal_reset(void);

/* Current IPv4 address of the station interface, dotted quad.
 * Returns false when not connected. */
bool wifi_hal_get_ip(char *out, size_t outsz);

/* Cheap check: supplicant control interface reachable. */
bool wifi_hal_is_up(void);

/* Trigger a scan and return a deduplicated, strongest-first list.
 * Returns the entry count (0 = none or not up). */
int wifi_hal_scan(struct wifi_hal_net *nets, int max);

/* Saved-network table; returns the entry count. */
int wifi_hal_saved(struct wifi_hal_saved *nets, int max);

/* Associate with an ssid.  Reuses a saved entry when present, else
 * adds one (open network when psk is empty).  Blocks until the link
 * settles or times out. */
bool wifi_hal_connect(const char *ssid, const char *psk);

/* Remove a saved network. */
bool wifi_hal_forget(const char *ssid);

/* Snapshot of the current link state. */
bool wifi_hal_get_status(struct wifi_hal_status *st);

#endif /* WIFI_HAL_H */
