/***************************************************************************
 * Optional radio status icons for the classic statusbar.
 *
 * Targets that define HAVE_STATUSBAR_RF_ICONS implement these two
 * queries; the shared statusbar draws a small bluetooth / wireless
 * glyph whenever the corresponding radio is powered.
 ****************************************************************************/
#ifndef STATUSBAR_RF_H
#define STATUSBAR_RF_H

#include "config.h"
#include <stdbool.h>

#ifdef HAVE_STATUSBAR_RF_ICONS
bool statusbar_rf_bt_on(void);
bool statusbar_rf_wifi_on(void);
#endif

#endif
