#ifndef NETFM_H
#define NETFM_H

#include <stddef.h>

#define NETFM_NAME_LEN 128
#define NETFM_URL_LEN 512

struct netfm_station
{
    char name[NETFM_NAME_LEN];
    char url[NETFM_URL_LEN];
};

int netfm_menu(void);

#ifdef HAVE_LVGL_UI
/* prev/next station while the LVGL play screen is open (netfm.c) */
int netfm_step_station(int dir);
const char *netfm_last_station_name(void);
#endif

#endif
