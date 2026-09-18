/***************************************************************************
 * WiFi menu root item.
 ****************************************************************************/
#include "config.h"

#ifdef HAVE_WIFI_MENU

#include "menu.h"
#include "lang.h"
#include "settings.h"
#include "wifi_menu.h"

MENUITEM_FUNCTION(wifi_root_item, 0, ID2P(LANG_WIFI),
                  wifi_menu, NULL, Icon_Submenu);

#endif /* HAVE_WIFI_MENU */
