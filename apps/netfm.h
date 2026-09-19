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

#endif
