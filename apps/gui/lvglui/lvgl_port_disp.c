/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__  _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * LVGL display port for the Cayin N3Pro: LVGL renders RGB565 exactly like
 * rockbox's own framebuffer (LCD_DEPTH 16, RGB565, same byte order on
 * little-endian mipsel), so flushing is a per-row memcpy into the rockbox
 * framebuffer plus the existing beam-chasing lcd_update_rect() - the vsync
 * chase, the 180 degree rotation and the 16->32bpp conversion all happen
 * in lcd-n3pro.c as usual.
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

#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include "lcd.h"
#include "lvgl_ui.h"

#define LVGUI_HOR 360
#define LVGUI_VER 480
#define LVGUI_DRAW_BUF_LINES 480

static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t g_buf1[LVGUI_HOR * LVGUI_DRAW_BUF_LINES];
static lv_disp_drv_t g_disp_drv;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *color_p)
{
    int w = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; y++)
    {
        memcpy(FBADDR(area->x1, y), color_p, w * sizeof(lv_color_t));
        color_p += w;
    }
    lcd_update_rect(area->x1, area->y1, w, area->y2 - area->y1 + 1);
    lv_disp_flush_ready(drv);
}

void lvgui_port_disp_init(void)
{
    lv_disp_draw_buf_init(&g_draw_buf, g_buf1, NULL,
                          LVGUI_HOR * LVGUI_DRAW_BUF_LINES);
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.hor_res = LVGUI_HOR;
    g_disp_drv.ver_res = LVGUI_VER;
    g_disp_drv.draw_buf = &g_draw_buf;
    g_disp_drv.flush_cb = flush_cb;
    /* The rockbox framebuffer is shared with the classic UI (splashes,
     * dialogs) and with rockbox's own partial redraws.  LVGL only renders
     * invalidated areas, so stale classic pixels survive in the shared fb
     * and show through as overlapping/ghosted content.  Force a full
     * render+flush every cycle so LVGL always paints the whole screen;
     * the 1 Hz status bar tick then guarantees any foreign content is
     * wiped within a second. */
    /* Partial refresh: LVGL only flushes the invalidated rectangles.
     * Full refresh made every statusbar tick blit the whole 360x480 frame
     * (691 KB of memcpy + panel update) once per second. */
    g_disp_drv.full_refresh = 0;
    lv_disp_drv_register(&g_disp_drv);
}

