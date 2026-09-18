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

/* Bluetooth audio menu root item (generic; needs HAVE_BT_AUDIO). */
#include "config.h"

#ifdef HAVE_BT_AUDIO

#include "menu.h"
#include "lang.h"
#include "settings.h"
#include "bluetooth.h"

MENUITEM_FUNCTION(bluetooth_root_item, 0, ID2P(LANG_BLUETOOTH),
                  bluetooth_menu, NULL, Icon_Submenu);

#endif /* HAVE_BT_AUDIO */
