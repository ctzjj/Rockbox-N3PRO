/***************************************************************************
 * Bluetooth audio menu root item (Cayin N3Pro hosted port).
 ****************************************************************************/
#include "config.h"

#ifdef CAYIN_N3PRO

#include "menu.h"
#include "lang.h"
#include "settings.h"
#include "n3pro_bluetooth.h"

MENUITEM_FUNCTION(n3pro_bluetooth_root_item, 0, ID2P(LANG_BLUETOOTH),
                  n3pro_bluetooth_menu, NULL, Icon_Submenu);

#endif /* CAYIN_N3PRO */
