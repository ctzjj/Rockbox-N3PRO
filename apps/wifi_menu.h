/***************************************************************************
 * Generic WiFi menu (framework only).
 *
 * All device interaction goes through the wifi_hal interface; ports
 * provide the implementation in their target directory.  The
 * remembered network (ssid + password) is kept in the Rockbox
 * settings, so it survives reboots and can be edited by hand in
 * .rockbox/config.cfg when the on-device keyboard is inconvenient.
 ****************************************************************************/
#ifndef WIFI_MENU_H
#define WIFI_MENU_H

int wifi_menu(void);

#endif /* WIFI_MENU_H */
