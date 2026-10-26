/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   ____  _______  ___
 *   Source      |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \ \/\  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|   /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/    \/    \/            \/
 *
 * UI shell: light frosted-glass theme helpers, screen stack with back
 * navigation, status bar (clock + battery), reusable row component,
 * and the Home screen. Shared building blocks (screens, glass panes,
 * soft shadows, headers) are exported for the feature screens (M2+).
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

#include "config.h"
#include "lvgl.h"
#include "lcd.h"
#include "lvgl_ui.h"
#include "root_menu.h"
#include "ui_theme.h"
#include "dlna/dlna_output.h"
#include "ui_backend.h"
#include "kernel.h"
#include "powermgmt.h"
#include "timefuncs.h"
#include "settings.h"
#include "lang.h"
#include "statusbar.h"
#include "sound.h"
#include "button.h"
#include "audio.h"
#include "metadata.h"
#include "string-extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

/* ---- palette -> theme tokens (all looks come from ui_theme.c) -------- */
#define COL_BG_TOP    th->bd_grad_top
#define COL_BG_BOT    th->bd_grad_bot
#define COL_GLASS_BRD th->border_white
#define COL_TEXT      th->fg_text
#define COL_TEXT_DIM  th->fg_dim
#define COL_ACCENT    th->accent
#define COL_ACCENT2   th->accent2
#define COL_LINK      th->fg_link
#define COL_INK       th->fg_ink
#define COL_SHADOW    th->sh_dark

static const lv_color_t icon_grad[] =
{
    LV_COLOR_MAKE(0xff, 0xd9, 0x7d), LV_COLOR_MAKE(0xff, 0x6a, 0x88),
    LV_COLOR_MAKE(0x21, 0xe0, 0xc2), LV_COLOR_MAKE(0x2f, 0x8b, 0xff),
    LV_COLOR_MAKE(0x43, 0xe9, 0x7b), LV_COLOR_MAKE(0x38, 0xf2, 0xd5),
};

/* ---- backdrop: procedural gradient + glow blobs, blurred once --------- */
/* Real backdrop blur is impossible per-pane in SW RGB565, so the whole
 * wallpaper is pre-blurred once at boot (thin gaussian). Panels draw
 * translucent white over it, which reads as frosted glass everywhere. */
#define BK_W 360
#define BK_H 480
#define BLUR_R 14         /* box blur radius; 2x H+V passes = heavy fog */

static lv_img_dsc_t backdrop_dsc;
static uint16_t *backdrop_buf;

/* ---- backdrop presets (Theme settings -> Backdrop) -------------------- */
/* index 0 follows the theme; the rest are fixed palettes drawn by the same
 * gradient + glow blob generator as the theme wallpaper. */
typedef struct
{
    uint32_t top, bot;
    struct { int x, y, r, str; uint32_t col; } blob[TH_BLOBS];
} bd_preset_t;

static const bd_preset_t bd_presets[] =
{
    { 0, 0, {{{0}}} },                                        /* follow theme */
    { 0x2E7FA8, 0x3E5FB8, { { 120,  90, 170, 75, 0x4FE8C0 },
                            { 285, 300, 180, 60, 0x9F86FF },
                            { 300, 440, 150, 50, 0x66E0FF } } }, /* aurora */
    { 0xE08A5C, 0xC85A86, { { 250, 110, 165, 75, 0xFFC48A },
                            {  90, 300, 175, 65, 0xFF8FB4 },
                            { 300, 440, 150, 55, 0xB08CFF } } }, /* sunset */
    { 0x4E8A54, 0x2F6E5A, { { 105, 120, 165, 65, 0xA8E87A },
                            { 285, 330, 175, 60, 0x4FC8B0 },
                            { 180, 450, 140, 45, 0xE8D28A } } }, /* forest */
    { 0x3A5A8C, 0x4A3C8C, { { 110, 140, 170, 55, 0x6A9CFF },
                            { 300, 300, 180, 50, 0x8A6CFF },
                            { 200, 450, 150, 45, 0x5AD0E8 } } }, /* midnight */
    { 0xB08A5C, 0x8A6A4A, { { 120, 130, 165, 60, 0xE8C48A },
                            { 300, 320, 175, 55, 0xC08A5C },
                            { 200, 450, 145, 45, 0xF2E0B8 } } }, /* dune */
    { 0x8A8A92, 0x6E6E78, { { 120, 140, 165, 50, 0xB8B8C0 },
                            { 300, 320, 175, 45, 0xD0D0D8 },
                            { 200, 450, 140, 35, 0xE8E8EE } } }, /* graphite */
    { 0x0B0B0D, 0x000000, { { 0, 0, 0, 0, 0 } } },             /* black */
};
#define BD_PRESET_COUNT ((int)(sizeof(bd_presets) / sizeof(bd_presets[0])))

static void box_blur_h(uint8_t *src, uint8_t *dst, int w, int h, int r)
{
    int div = 2 * r + 1;
    for (int y = 0; y < h; y++)
    {
        uint8_t *s = src + y * w * 3;
        uint8_t *d = dst + y * w * 3;
        int sr = s[0] * (r + 1), sg = s[1] * (r + 1), sb = s[2] * (r + 1);
        for (int i = 1; i <= r; i++)
        {
            sr += s[i * 3]; sg += s[i * 3 + 1]; sb += s[i * 3 + 2];
        }
        for (int x = 0; x < w; x++)
        {
            d[x * 3] = sr / div; d[x * 3 + 1] = sg / div; d[x * 3 + 2] = sb / div;
            int xi = x - r, xo = x + r + 1;
            if (xi < 0) xi = 0;
            if (xo >= w) xo = w - 1;
            sr += s[xo * 3] - s[xi * 3];
            sg += s[xo * 3 + 1] - s[xi * 3 + 1];
            sb += s[xo * 3 + 2] - s[xi * 3 + 2];
        }
    }
}

static void box_blur_v(uint8_t *src, uint8_t *dst, int w, int h, int r)
{
    int div = 2 * r + 1;
    int stride = w * 3;
    for (int x = 0; x < w; x++)
    {
        uint8_t *s = src + x * 3;
        uint8_t *d = dst + x * 3;
        int sr = s[0] * (r + 1), sg = s[1] * (r + 1), sb = s[2] * (r + 1);
        for (int i = 1; i <= r; i++)
        {
            sr += s[i * stride]; sg += s[i * stride + 1]; sb += s[i * stride + 2];
        }
        for (int y = 0; y < h; y++)
        {
            d[y * stride] = sr / div;
            d[y * stride + 1] = sg / div;
            d[y * stride + 2] = sb / div;
            int yi = y - r, yo = y + r + 1;
            if (yi < 0) yi = 0;
            if (yo >= h) yo = h - 1;
            sr += s[yo * stride] - s[yi * stride];
            sg += s[yo * stride + 1] - s[yi * stride + 1];
            sb += s[yo * stride + 2] - s[yi * stride + 2];
        }
    }
}

static void backdrop_init(void)
{
    uint8_t *work = malloc((size_t)BK_W * BK_H * 3);   /* RGB888 workspace */
    uint8_t *tmp = malloc((size_t)BK_W * BK_H * 3);
    if (!backdrop_buf)                                 /* keep it across theme
                                                        * switches */
        backdrop_buf = malloc((size_t)BK_W * BK_H * 2); /* RGB565 result */
    if (!work || !tmp || !backdrop_buf)
    {
        free(work); free(tmp);
        return;   /* screens fall back to plain gradient */
    }

    /* base vertical gradient: the theme's, or the chosen preset's 24-bit
     * palette.  Theme colours come packed at 5/6/5 -- expand to 8 bits. */
    int preset = global_settings.ui_backdrop;
    const bd_preset_t *bp = (preset > 0 && preset < BD_PRESET_COUNT)
                            ? &bd_presets[preset] : NULL;
    uint8_t tr, tg, tb, br, bg_, bb;
    if (bp)
    {
        tr = (bp->top >> 16) & 0xff; tg = (bp->top >> 8) & 0xff; tb = bp->top & 0xff;
        br = (bp->bot >> 16) & 0xff; bg_ = (bp->bot >> 8) & 0xff; bb = bp->bot & 0xff;
        if (global_settings.ui_dark_mode)
        {
            /* dark theme: same presets, night-time palette */
            tr /= 4; tg /= 4; tb /= 4;
            br /= 4; bg_ /= 4; bb /= 4;
        }
    }
    else
    {
        tr = LV_COLOR_GET_R(th->bd_grad_top); tg = LV_COLOR_GET_G(th->bd_grad_top);
        tb = LV_COLOR_GET_B(th->bd_grad_top);
        br = LV_COLOR_GET_R(th->bd_grad_bot); bg_ = LV_COLOR_GET_G(th->bd_grad_bot);
        bb = LV_COLOR_GET_B(th->bd_grad_bot);
        tr = (tr << 3) | (tr >> 2);  tg = (tg << 2) | (tg >> 4);  tb = (tb << 3) | (tb >> 2);
        br = (br << 3) | (br >> 2);  bg_ = (bg_ << 2) | (bg_ >> 4);  bb = (bb << 3) | (bb >> 2);
    }
    for (int y = 0; y < BK_H; y++)
    {
        int t = y * 256 / (BK_H - 1);
        uint8_t r = (uint8_t)(tr + (br - tr) * t / 256);
        uint8_t g = (uint8_t)(tg + (bg_ - tg) * t / 256);
        uint8_t b = (uint8_t)(tb + (bb - tb) * t / 256);
        uint8_t *row = work + (size_t)y * BK_W * 3;
        for (int x = 0; x < BK_W; x++)
        {
            row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
        }
    }

    /* glow blobs: lerp toward the blob colour (never clips to white) */
    for (unsigned i = 0; i < TH_BLOBS; i++)
    {
        int rad, cx, cy, strv;
        uint8_t cr, cg, cb;
        if (bp)
        {
            rad = bp->blob[i].r; cx = bp->blob[i].x; cy = bp->blob[i].y;
            strv = bp->blob[i].str;
            cr = (bp->blob[i].col >> 16) & 0xff;
            cg = (bp->blob[i].col >> 8) & 0xff;
            cb = bp->blob[i].col & 0xff;
            if (global_settings.ui_dark_mode)
            {
                cr /= 3; cg /= 3; cb /= 3; strv /= 2;
            }
        }
        else
        {
            rad = th->bd_blob[i].r; cx = th->bd_blob[i].x; cy = th->bd_blob[i].y;
            strv = th->bd_blob[i].str;
            cr = LV_COLOR_GET_R(th->bd_blob[i].col);
            cg = LV_COLOR_GET_G(th->bd_blob[i].col);
            cb = LV_COLOR_GET_B(th->bd_blob[i].col);
            cr = (cr << 3) | (cr >> 2);
            cg = (cg << 2) | (cg >> 4);
            cb = (cb << 3) | (cb >> 2);
        }
        if (rad <= 0)
            continue;
        int r2 = rad * rad;
        float str = strv / 100.0f;
        int x0 = cx - rad, x1 = cx + rad;
        int y0 = cy - rad, y1 = cy + rad;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= BK_W) x1 = BK_W - 1;
        if (y1 >= BK_H) y1 = BK_H - 1;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
            {
                int dx = x - cx, dy = y - cy;
                int d2 = dx * dx + dy * dy;
                if (d2 >= r2) continue;
                float f = 1.0f - (float)sqrt((double)d2) / rad;
                f = f * f * str;
                uint8_t *p = work + ((size_t)y * BK_W + x) * 3;
                p[0] = (uint8_t)(p[0] + (cr - p[0]) * f);
                p[1] = (uint8_t)(p[1] + (cg - p[1]) * f);
                p[2] = (uint8_t)(p[2] + (cb - p[2]) * f);
            }
    }

    /* thin gaussian (two box passes each direction) */
    box_blur_h(work, tmp, BK_W, BK_H, BLUR_R);
    box_blur_v(tmp, work, BK_W, BK_H, BLUR_R);
    box_blur_h(work, tmp, BK_W, BK_H, BLUR_R);
    box_blur_v(tmp, work, BK_W, BK_H, BLUR_R);

    /* pack to RGB565 with noise dither: a heavily blurred 565 gradient bands
     * into visible contour rings ("moiré"); random grain hides them without
     * the cross-hatch look an ordered (Bayer) matrix leaves on flat areas */
    uint32_t seed = 0x1234567u;
    for (int i = 0; i < BK_W * BK_H; i++)
    {
        seed = seed * 1664525u + 1013904223u;
        int d = (int)((seed >> 16) & 7) - 4;                     /* -4..3 */
        int r = work[i * 3] + d, g = work[i * 3 + 1] + d, b = work[i * 3 + 2] + d;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        backdrop_buf[i] = lv_color_make(r, g, b).full;
    }
    free(work); free(tmp);

    backdrop_dsc.header.always_zero = 0;
    backdrop_dsc.header.w = BK_W;
    backdrop_dsc.header.h = BK_H;
    backdrop_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    backdrop_dsc.data_size = (uint32_t)BK_W * BK_H * 2;
    backdrop_dsc.data = (const uint8_t *)backdrop_buf;
}

/* ---- screen stack --------------------------------------------------- */
#define NAV_DEPTH 12
static lv_obj_t *nav_stack[NAV_DEPTH];
static int nav_depth;

lv_obj_t *shell_screen_new(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG_TOP, 0);
    if (backdrop_buf)
        lv_obj_set_style_bg_img_src(scr, &backdrop_dsc, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* default UI text font for everything on the screen (multilingual,
     * follows the 界面字体大小 setting); symbol labels override locally */
    lv_obj_set_style_text_font(scr, th->font_cjk, 0);
    return scr;
}

/* paint the pre-blurred wallpaper on any object (full-screen overlays) */
void shell_backdrop_apply(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, COL_BG_TOP, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (backdrop_buf)
        lv_obj_set_style_bg_img_src(o, &backdrop_dsc, 0);
}

void shell_nav_push(lv_obj_t *scr)
{
    if (nav_depth >= NAV_DEPTH)
    {
        lv_obj_del(scr);
        return;
    }
    shell_toast_clear();          /* a new screen hides a sticky splash */
    nav_stack[nav_depth++] = scr;
    lv_scr_load(scr);
}

/* Delete-and-load must not run inside a widget event callback (the
 * tapped object lives on the screen being deleted -> use-after-free).
 * Defer to the next idle cycle. Also: never delete the ACTIVE screen
 * before switching away, or act_scr dangles -> SIGSEGV on next draw. */
static void nav_pop_do(void *param)
{
    (void)param;
    if (nav_depth <= 1)
        return;                    /* never pop Home */
    lv_obj_t *scr = nav_stack[--nav_depth];
    lv_scr_load(nav_stack[nav_depth - 1]);   /* switch away FIRST */
    lv_obj_del(scr);                          /* then delete */
    shell_toast_clear();                      /* parent screen is shown now */
}

void shell_nav_pop(void)
{
    lv_async_call(nav_pop_do, NULL);
}

/* Pop the top screen right away (no deferral). Only safe from a timer /
 * lv_async context, never from an event cb on the screen being removed. */
void shell_nav_pop_now(void)
{
    nav_pop_do(NULL);
}

/* Return to Home, tearing down every screen above it. Deferred like
 * nav_pop_do: callers typically sit in an event cb on the screen that
 * would be deleted underneath them. */
static void nav_home_do(void *param)
{
    (void)param;
    if (nav_depth <= 1)
        return;
    lv_scr_load(nav_stack[0]);            /* switch away FIRST */
    while (nav_depth > 1)
        lv_obj_del(nav_stack[--nav_depth]);
    shell_toast_clear();
}

void shell_nav_home(void)
{
    lv_async_call(nav_home_do, NULL);
}

static void obj_del_do(void *param)
{
    lv_obj_t *o = param;
    if (o)
        lv_obj_del(o);
}

/* Safe deletion from inside an event callback (e.g. removing the
 * container the clicked button lives in). */
void shell_obj_del_async(lv_obj_t *o)
{
    if (o)
        lv_async_call(obj_del_do, o);
}

bool shell_nav_full(void)
{
    return nav_depth >= NAV_DEPTH;
}

/* ---- status bar ------------------------------------------------------ */
/* Each screen owns its labels; keep pointers in a slot table so the
 * 1s timer refreshes whichever screens are alive (home + stacked). */
#define SB_SLOTS (4 * NAV_DEPTH)
static lv_obj_t *sb_slots[SB_SLOTS];

static void sb_slot_release(lv_event_t *e)
{
    lv_obj_t *l = lv_event_get_target(e);
    for (int i = 0; i < SB_SLOTS; i++)
        if (sb_slots[i] == l)
            sb_slots[i] = NULL;
}

static lv_obj_t *sb_slot_track(lv_obj_t *label)
{
    for (int i = 0; i < SB_SLOTS; i++)
    {
        if (sb_slots[i] == NULL)
        {
            sb_slots[i] = label;
            lv_obj_add_event_cb(label, sb_slot_release, LV_EVENT_DELETE, NULL);
            return label;
        }
    }
    return NULL;
}

static void statusbar_tick(lv_timer_t *t)
{
    (void)t;
    /* panel off: nothing to update (the wake repaint refreshes it) */
    if (!lcd_active())
        return;
    struct tm *tm = get_time();
    int batt = battery_level();
    /* classic status-bar volume readout: raw volume, /10 when it has a
     * decimal place (see gui_statusbar_icon_volume in statusbar.c) */
    int vol = global_status.volume;
    int nd = sound_numdecimals(SOUND_VOLUME);
    if (nd)
        vol /= 10 * nd;
    for (int i = 0; i < SB_SLOTS; i += 4)
    {
        lv_obj_t *tl = sb_slots[i], *bl = sb_slots[i + 1],
                 *vl = sb_slots[i + 2], *wl = sb_slots[i + 3];
        if (tl && tm)
        {
            if (global_settings.timeformat == 1)
            {
                int h12 = tm->tm_hour % 12;
                if (!h12)
                    h12 = 12;
                lv_label_set_text_fmt(tl, "%d:%02d %s", h12, tm->tm_min,
                                      tm->tm_hour < 12 ? "AM" : "PM");
            }
            else
                lv_label_set_text_fmt(tl, "%02d:%02d", tm->tm_hour, tm->tm_min);
        }
        if (bl)
        {
            if (global_settings.battery_display == 0) /* graphic */
            {
                const char *s =
                    batt >= 95 ? LV_SYMBOL_BATTERY_FULL :
                    batt >= 75 ? LV_SYMBOL_BATTERY_3   :
                    batt >= 50 ? LV_SYMBOL_BATTERY_2   :
                    batt >= 25 ? LV_SYMBOL_BATTERY_1   :
                                 LV_SYMBOL_BATTERY_EMPTY;
                lv_label_set_text(bl, s);
            }
            else
                lv_label_set_text_fmt(bl, "%d%%", batt);
        }
        if (vl)
        {
            int vmin = sound_min(SOUND_VOLUME), vmax = sound_max(SOUND_VOLUME);
            int pct = (vmax > vmin)
                      ? (global_status.volume - vmin) * 100 / (vmax - vmin)
                      : 0;
            lv_label_set_text_fmt(vl, "%s %d%%", LV_SYMBOL_VOLUME_MAX, pct);
        }
        if (wl)
        {
            char net[16];
            int n = 0;
            if (be_wifi_up())
            {
                strcpy(net, LV_SYMBOL_WIFI);
                n = strlen(net);
            }
            if (be_bt_active())
            {
                if (n)
                    net[n++] = ' ';
                strcpy(net + n, LV_SYMBOL_BLUETOOTH);
            }
            else if (n)
                net[n] = '\0';
            else
                net[0] = '\0';
            lv_label_set_text(wl, net);
        }
    }
}

static void statusbar_build(lv_obj_t *parent)
{
    if (global_settings.statusbar == STATUSBAR_OFF)
        return;
    bool bot = (global_settings.statusbar == STATUSBAR_BOTTOM);
    lv_align_t al_l = bot ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_TOP_LEFT;
    lv_align_t al_r = bot ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_TOP_RIGHT;
    int dy = bot ? -7 : 7;

    lv_obj_t *tl = lv_label_create(parent);
    lv_obj_set_style_text_font(tl, th->font_symbol, 0);
    lv_obj_set_style_text_color(tl, COL_TEXT_DIM, 0);
    lv_obj_align(tl, al_l, 10, dy);
    lv_label_set_text(tl, "");
    sb_slot_track(tl);

    lv_obj_t *bl = lv_label_create(parent);
    lv_obj_set_style_text_font(bl, th->font_symbol, 0);
    lv_obj_set_style_text_color(bl, COL_TEXT_DIM, 0);
    lv_obj_align(bl, al_r, -10, dy);
    lv_label_set_text(bl, "");
    sb_slot_track(bl);

    lv_obj_t *vl = lv_label_create(parent);
    lv_obj_set_style_text_font(vl, th->font_symbol, 0);
    lv_obj_set_style_text_color(vl, COL_TEXT_DIM, 0);
    lv_obj_align(vl, al_r, -54, dy);
    lv_label_set_text(vl, "");
    sb_slot_track(vl);

    /* network icons: wifi + bluetooth when either is up */
    lv_obj_t *wl = lv_label_create(parent);
    lv_obj_set_style_text_font(wl, th->font_symbol, 0);
    lv_obj_set_style_text_color(wl, COL_TEXT_DIM, 0);
    lv_obj_align(wl, al_r, -116, dy);
    lv_label_set_text(wl, "");
    sb_slot_track(wl);
}

void shell_scrollbar_setup(lv_obj_t *list)
{
    lv_obj_set_scrollbar_mode(list,
        global_settings.scrollbar == SCROLLBAR_OFF ? LV_SCROLLBAR_MODE_OFF
                                                   : LV_SCROLLBAR_MODE_AUTO);
}

void shell_statusbar_build(lv_obj_t *parent)
{
    statusbar_build(parent);
    /* fill the labels immediately: they are created empty so the LVGL
     * default "Text" never shows, but waiting for the 1 Hz tick would
     * make the status bar appear blank for a moment on every new screen */
    statusbar_tick(NULL);
}

/* ---- glass container -------------------------------------------------- */
/* The SW renderer draws LVGL's native box shadow as a hard-edged band,
 * which reads as a square block under the rounded pane. Build a soft
 * shadow instead: a stack of concentric rounded rects with falling
 * opacity, created before the pane at the same spot. */
void shell_soft_shadow(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, lv_coord_t radius,
                       lv_coord_t dy)
{
    (void)dy;   /* per-layer dy is used instead; kept for call-site clarity */
    for (unsigned i = 0; i < 3; i++)
    {
        lv_coord_t s = th->sh_spread[i];
        lv_obj_t *sh = lv_obj_create(parent);
        lv_obj_set_size(sh, w + 2 * s, h + 2 * s);
        /* keep the dark under the pane (dy from the pane top, not from
         * the layer top) so the halo can shine along the top and sides */
        lv_obj_set_pos(sh, x - s, y + 3 - i);
        lv_obj_set_style_radius(sh, radius + s, 0);
        lv_obj_set_style_bg_opa(sh, th->sh_dark_opa[i], 0);
        lv_obj_set_style_bg_color(sh, th->sh_dark, 0);
        lv_obj_set_style_border_width(sh, 0, 0);
        lv_obj_set_style_pad_all(sh, 0, 0);
        lv_obj_clear_flag(sh, LV_OBJ_FLAG_SCROLLABLE |
                                    LV_OBJ_FLAG_CLICKABLE);
    }

    /* light halo hugging the pane: the frosted-glass edge glow */
    for (unsigned i = 0; i < 3; i++)
    {
        lv_coord_t s = th->sh_halo_spread[i];
        lv_obj_t *gl = lv_obj_create(parent);
        lv_obj_set_size(gl, w + 2 * s, h + 2 * s);
        lv_obj_set_pos(gl, x - s, y - s);
        lv_obj_set_style_radius(gl, radius + s, 0);
        lv_obj_set_style_bg_opa(gl, th->sh_halo_opa[i], 0);
        lv_obj_set_style_bg_color(gl, th->border_white, 0);
        lv_obj_set_style_border_width(gl, 0, 0);
        lv_obj_set_style_pad_all(gl, 0, 0);
        lv_obj_clear_flag(gl, LV_OBJ_FLAG_SCROLLABLE |
                                   LV_OBJ_FLAG_CLICKABLE);
    }
}

lv_obj_t *shell_style_shadow_off(lv_obj_t *o)
{
    lv_obj_set_style_shadow_width(o, 0, 0);
    return o;
}

/* ---- generic modal dialog ---------------------------------------------- */
/* Dark scrim over the whole active screen (blocks input to what is below)
 * with a centered glass card on top: title, wrapped body and a row of
 * buttons laid out automatically. The NULL-terminated button array gives
 * per-button style, callback and user data, so the same component serves
 * simple confirms (确定/取消) and future flows like file deletion. */
lv_obj_t *shell_modal_open(const char *title, const char *body,
                           const shell_modal_btn_t *buttons)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_t *scrim = lv_obj_create(scr);
    lv_obj_set_size(scrim, 360, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(scrim, th->scrim_input_opa, 0);
    lv_obj_set_style_bg_color(scrim, th->scrim_input, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);

    shell_soft_shadow(scrim, 10, 140, 340, 190, th->radius, 7);
    lv_obj_t *card = shell_glass_new(scrim);
    lv_obj_set_size(card, 340, 190);
    lv_obj_set_pos(card, 10, 140);
    lv_obj_set_style_bg_opa(card, th->kbd_card_opa, 0);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, shell_col_text(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *b = lv_label_create(card);
    lv_label_set_text(b, body);
    lv_obj_set_style_text_color(b, shell_col_dim(), 0);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, 300);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 44);

    int n = 0;
    while (buttons && buttons[n].text)
        n++;
    if (n > 0)
    {
        lv_coord_t gap = 8, margin = 20;
        lv_coord_t w = (340 - 2 * margin - gap * (n - 1)) / n;
        for (int i = 0; i < n; i++)
        {
            lv_coord_t x = margin + i * (w + gap);
            lv_obj_t *btn = shell_glass_new(card);
            lv_obj_set_size(btn, w, 44);
            lv_obj_set_pos(btn, x, 136);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            if (buttons[i].primary)
            {
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(btn, shell_icon_grad(0), 0);
            }
            else
            {
                lv_obj_set_style_bg_opa(btn, th->btn_secondary_opa, 0);
                lv_obj_set_style_border_opa(btn, th->card_border_opa, 0);
            }
            lv_obj_add_event_cb(btn, buttons[i].cb, LV_EVENT_CLICKED,
                                buttons[i].user_data);

            lv_obj_t *l = lv_label_create(btn);
            lv_label_set_text(l, buttons[i].text);
            lv_obj_set_style_text_color(l, buttons[i].primary
                                              ? shell_col_ink()
                                              : shell_col_text(), 0);
            lv_obj_center(l);
        }
    }

    return card;
}

void shell_modal_close(lv_obj_t *card)
{
    if (!card)
        return;
    lv_obj_t *scrim = lv_obj_get_parent(card);
    shell_obj_del_async(scrim);   /* may be called from a button inside */
}

void shell_modal_close_ev(lv_event_t *e)
{
    shell_modal_close(lv_obj_get_parent(lv_event_get_target(e)));
}

/* ---- native text input (stand-in for the classic kbd_input) ------------- */
static bool g_kbd_done;
static bool g_kbd_ok;

static void kbd_ok_cb(lv_event_t *e)
{
    (void)e;
    g_kbd_ok = true;
    g_kbd_done = true;
}

static void kbd_cancel_cb(lv_event_t *e)
{
    (void)e;
    g_kbd_ok = false;
    g_kbd_done = true;
}

static lv_obj_t *kbd_mkbtn(lv_obj_t *parent, const char *txt, int x,
                           lv_event_cb_t cb, bool primary)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 120, 40);
    lv_obj_set_pos(b, x, 138);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b,
        primary ? th->accent_blue : th->btn_secondary, 0);
    lv_obj_set_style_bg_opa(b, primary ? 230 : th->btn_secondary_opa, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, th->border_white, 0);
    lv_obj_set_style_border_opa(b, th->card_border_opa, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l,
        primary ? th->border_white : th->fg_text, 0);
    lv_obj_center(l);
    return b;
}


static lv_obj_t *g_kbd_scr, *g_kbd_kb, *g_kbd_cand, *g_kbd_ta;
static bool g_kbd_cn, g_kbd_symbols;
static int  g_kbd_shift;
static char g_pybuf[32];

/* Reference layout (12u grid): letters 0.9u, function keys 1.5u, bottom row
 * 2 / 2 / 3 / 2.5 / 2.5u. Widths below are in 1/20u so they stay integral;
 * rows 2 and 3 are centred with transparent spacers like the reference. */
static const char *kmap_lower[] = {
    "#+=","q","w","e","r","t","y","u","i","o","p"," ","\n",
    " "," ","a","s","d","f","g","h","j","k","l"," "," ","\n",
    " ","'","-","z","x","c","v","b","n","m",".",",",":"," ","\n",
    "123"," ","空格","中","OK",""
};
static const char *kmap_upper[] = {
    "#+=","Q","W","E","R","T","Y","U","I","O","P"," ","\n",
    " "," ","A","S","D","F","G","H","J","K","L"," "," ","\n",
    " ","'","-","Z","X","C","V","B","N","M",".",",",":"," ","\n",
    "123"," ","空格","中","OK",""
};
static const char *kmap_sym[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "@","#","￥","%","&","*","-","+","(",")","\n",
    "~","!","?","/","\\","|","[","]","{","}","\n",
    "ABC","空格"," ","OK",""
};

/* Width units must stay within LV_BTNMATRIX_WIDTH_MASK (0..15).
 * letter rows: 0.9u letters = 6, 1.5u function keys = 10, 12u = 80
 * bottom row:  2/2/3/2.5/2.5u = 4/4/6/5/5 (row is normalised on its own) */
static const uint8_t kw_abc[44] = {
    10,6,6,6,6,6,6,6,6,6,6,10,
    3,10,6,6,6,6,6,6,6,6,6,10,3,
    2,10,6,6,6,6,6,6,6,6,6,6,6,2,
    4,4,6,5,5
};
static const uint8_t kw_sym[34] = {
    6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,
    6,6,5,7
};

#define KBD_LANG_ID 42          /* 中 key id on the letter pages */

static lv_obj_t *g_kbd_icons[4];

static void kbd_icon_del(void)
{
    for (int i = 0; i < 4; i++)
    {
        if (g_kbd_icons[i])
        {
            lv_obj_del(g_kbd_icons[i]);
            g_kbd_icons[i] = NULL;
        }
    }
}

static void kbd_icon_add(int slot, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h, const char *sym)
{
    lv_obj_t *o = lv_obj_create(g_kbd_kb);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    shell_style_shadow_off(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(o);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, th->font_symbol, 0);
    lv_obj_set_style_text_color(l, th->fg_text, 0);
    lv_obj_center(l);
    g_kbd_icons[slot] = o;
}

static void kbd_set_page(bool symbols, bool upper)
{
    const char **map = symbols ? kmap_sym
                               : (upper ? kmap_upper : kmap_lower);
    int n = symbols ? 34 : 44;
    const uint8_t *w = symbols ? kw_sym : kw_abc;

    lv_btnmatrix_set_map(g_kbd_kb, map);
    for (int i = 0; i < n; i++)
        lv_btnmatrix_set_btn_width(g_kbd_kb, (uint16_t)i, w[i]);

    if (!symbols)
    {
        /* transparent spacers that centre rows 2/3 */
        lv_btnmatrix_set_btn_ctrl(g_kbd_kb, 12, LV_BTNMATRIX_CTRL_HIDDEN);
        lv_btnmatrix_set_btn_ctrl(g_kbd_kb, 24, LV_BTNMATRIX_CTRL_HIDDEN);
        lv_btnmatrix_set_btn_ctrl(g_kbd_kb, 25, LV_BTNMATRIX_CTRL_HIDDEN);
        lv_btnmatrix_set_btn_ctrl(g_kbd_kb, 38, LV_BTNMATRIX_CTRL_HIDDEN);
        if (g_kbd_cn)
            lv_btnmatrix_set_btn_ctrl(g_kbd_kb, KBD_LANG_ID,
                                      LV_BTNMATRIX_CTRL_CHECKED);
    }

    /* icon overlays (blank matrix labels) */
    kbd_icon_del();
    if (symbols)
    {
        kbd_icon_add(0, 189, 174, 61, 44, LV_SYMBOL_BACKSPACE);
    }
    else
    {
        kbd_icon_add(0, 305, 2, 56, 44, LV_SYMBOL_BACKSPACE);
        kbd_icon_add(1, 2, 55, 56, 44, LV_SYMBOL_UP);
        kbd_icon_add(2, 305, 55, 56, 44, LV_SYMBOL_NEW_LINE);
        kbd_icon_add(3, 63, 174, 58, 44, LV_SYMBOL_KEYBOARD);
    }
}

static void cand_hide(void)
{
    if (g_kbd_cand)
    {
        /* may run from a candidate tap: never delete the pressed row's
         * parent synchronously */
        lv_obj_del_async(g_kbd_cand);
        g_kbd_cand = NULL;
    }
}

static void cand_pick_cb(lv_event_t *e)
{
    const char *ch = (const char *)lv_event_get_user_data(e);
    if (g_kbd_ta && ch && ch[0])
    {
        int n = (int)strlen(g_pybuf);
        while (n-- > 0)
            lv_textarea_del_char(g_kbd_ta);
        lv_textarea_add_text(g_kbd_ta, ch);
    }
    g_pybuf[0] = '\0';
    cand_hide();
}

extern lv_pinyin_dict_t lv_ime_pinyin_def_dict[];

/* one syllable -> candidate strip (up to 8 chars, first one highlighted) */
static void cand_update(void)
{
    cand_hide();
    if (!g_pybuf[0] || !g_kbd_ta)
        return;

    const char *val = NULL;
    for (int i = 0; lv_ime_pinyin_def_dict[i].py; i++)
    {
        if (!strcmp(lv_ime_pinyin_def_dict[i].py, g_pybuf))
        {
            val = lv_ime_pinyin_def_dict[i].py_mb;
            break;
        }
    }
    if (!val)
        return;

    static char chbuf[8][8];
    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    g_kbd_cand = bar;
    lv_obj_set_size(bar, 340, 40);
    lv_obj_set_pos(bar, 10, 212);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, th->glass_bg, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, th->border_white, 0);
    lv_obj_set_style_border_opa(bar, th->card_border_opa, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_radius(bar, 10, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    const char *p = val;
    int n = 0;
    while (*p && n < 8)
    {
        unsigned char c = (unsigned char)*p;
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        memcpy(chbuf[n], p, len);
        chbuf[n][len] = '\0';

        lv_obj_t *b = lv_btn_create(bar);
        lv_obj_set_size(b, 38, 32);
        lv_obj_set_style_bg_color(b,
            n == 0 ? th->accent_blue : th->btn_secondary, 0);
        lv_obj_set_style_bg_opa(b,
            n == 0 ? LV_OPA_COVER : th->btn_secondary_opa, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_add_event_cb(b, cand_pick_cb, LV_EVENT_CLICKED, chbuf[n]);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, chbuf[n]);
        lv_obj_set_style_text_font(l, th->font_cn, 0);
        lv_obj_set_style_text_color(l,
            n == 0 ? th->fg_inverse : th->fg_text, 0);
        lv_obj_center(l);

        p += len;
        n++;
    }
    lv_obj_invalidate(bar);
    lv_refr_now(NULL);
}

static void kbd_backspace(void)
{
    lv_textarea_del_char(g_kbd_ta);
    if (g_kbd_cn && g_pybuf[0])
    {
        g_pybuf[strlen(g_pybuf) - 1] = '\0';
        cand_update();
    }
}

static void kbd_shift_key(void)
{
    g_kbd_shift ^= 1;
    kbd_set_page(false, g_kbd_shift != 0);
}

static void kbd_ok_key(void)
{
    g_kbd_ok = true;
    g_kbd_done = true;
}

static void kbd_key_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(bm);
    const char *t = lv_btnmatrix_get_btn_text(bm, id);

    if (!t)
        return;

    /* icon keys carry a space label -> dispatch by button id */
    if (!t[0] || !strcmp(t, " "))
    {
        if (!g_kbd_symbols)
        {
            if (id == 11) { kbd_backspace(); return; }
            if (id == 13) { kbd_shift_key(); return; }
            if (id == 23) { kbd_ok_key(); return; }
            return;                 /* 41 = keyboard icon (no-op) */
        }
        if (id == 32) { kbd_backspace(); return; }
        return;
    }

    if (!strcmp(t, "OK"))
    {
        kbd_ok_key();
        return;
    }
    if (!strcmp(t, "123") || !strcmp(t, "#+="))
    {
        g_kbd_symbols = true;
        kbd_set_page(true, false);
        return;
    }
    if (!strcmp(t, "ABC"))
    {
        g_kbd_symbols = false;
        kbd_set_page(false, g_kbd_shift != 0);
        return;
    }
    if (!strcmp(t, "\u4e2d"))
    {
        g_kbd_cn = !g_kbd_cn;
        g_pybuf[0] = '\0';
        cand_hide();
        if (g_kbd_cn)
            lv_btnmatrix_set_btn_ctrl(g_kbd_kb, KBD_LANG_ID,
                                      LV_BTNMATRIX_CTRL_CHECKED);
        else
            lv_btnmatrix_clear_btn_ctrl(g_kbd_kb, KBD_LANG_ID,
                                        LV_BTNMATRIX_CTRL_CHECKED);
        return;
    }
    if (!strcmp(t, "\u7a7a\u683c"))
    {
        lv_textarea_add_text(g_kbd_ta, " ");
        if (g_kbd_cn)
        {
            g_pybuf[0] = '\0';
            cand_hide();
        }
        return;
    }
    if (!strcmp(t, "'"))
    {
        if (g_kbd_cn)
        {
            lv_textarea_add_text(g_kbd_ta, "'");
            int n = (int)strlen(g_pybuf);
            if (n < (int)sizeof g_pybuf - 1)
            {
                g_pybuf[n] = '\'';
                g_pybuf[n + 1] = '\0';
            }
            cand_hide();
        }
        return;
    }

    /* letters: pinyin in Chinese mode, plain text otherwise */
    if ((unsigned char)t[0] >= 0x80)
    {
        lv_textarea_add_text(g_kbd_ta, t);
        return;
    }
    char c = t[0];
    if (g_kbd_cn)
    {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        {
            char sb[2] = { c, '\0' };
            lv_textarea_add_text(g_kbd_ta, sb);
        }
        int n = (int)strlen(g_pybuf);
        if (n < (int)sizeof g_pybuf - 1)
        {
            g_pybuf[n] = c;
            g_pybuf[n + 1] = '\0';
        }
        cand_update();
        return;
    }

    if (g_kbd_shift && c >= 'a' && c <= 'z')
        c += 'A' - 'a';
    {
        char sb[2] = { c, '\0' };
        lv_textarea_add_text(g_kbd_ta, sb);
    }
}

static void kbd_make_kb(void)
{
    /* opaque-ish glass panel behind the keys (the home list must not
     * show through the key gaps) */
    lv_obj_t *bg = lv_obj_create(g_kbd_scr);
    lv_obj_set_size(bg, 360, 224);
    lv_obj_align(bg, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bg, th->glass_bg, 0);
    lv_obj_set_style_bg_opa(bg, 235, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    shell_style_shadow_off(bg);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_kbd_kb = lv_btnmatrix_create(g_kbd_scr);
    lv_obj_set_size(g_kbd_kb, 360, 224);
    lv_obj_align(g_kbd_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(g_kbd_kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_kbd_kb, 0, 0);
    lv_obj_set_style_radius(g_kbd_kb, 0, 0);
    lv_obj_set_style_pad_all(g_kbd_kb, 4, 0);
    lv_obj_set_style_pad_row(g_kbd_kb, 6, 0);
    lv_obj_set_style_pad_column(g_kbd_kb, 5, 0);

    lv_obj_set_style_bg_color(g_kbd_kb, th->btn_secondary, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(g_kbd_kb, th->btn_secondary_opa, LV_PART_ITEMS);
    lv_obj_set_style_text_color(g_kbd_kb, th->fg_text, LV_PART_ITEMS);
    lv_obj_set_style_text_font(g_kbd_kb, th->font_cjk, LV_PART_ITEMS);
    lv_obj_set_style_border_color(g_kbd_kb, th->border_white, LV_PART_ITEMS);
    lv_obj_set_style_border_opa(g_kbd_kb, th->card_border_opa, LV_PART_ITEMS);
    lv_obj_set_style_border_width(g_kbd_kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(g_kbd_kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(g_kbd_kb, th->accent_blue,
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(g_kbd_kb, th->fg_inverse,
                                 LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(g_kbd_kb, th->accent_blue,
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(g_kbd_kb, th->fg_inverse,
                                 LV_PART_ITEMS | LV_STATE_CHECKED);

    g_kbd_symbols = false;
    g_kbd_shift = 0;
    kbd_set_page(false, false);
    lv_obj_add_event_cb(g_kbd_kb, kbd_key_cb, LV_EVENT_VALUE_CHANGED, NULL);

}

/* Drop-in replacement for the classic kbd_input() (apps/recorder/keyboard.c)
 * used by the stock rename / create-directory / playlist-name handlers: it
 * shows our LVGL keyboard and BLOCKS, pumping the UI loop, until the user
 * confirms or cancels. Returns 0 when confirmed (text copied back), -1 on
 * cancel -- the same convention as the original. Must run from a
 * lvgui_run_job() context, never inside an LVGL event callback. */
/* The keyboard overlay is built once and only shown/hidden afterwards: a
 * modal that deletes its own widgets while a touch is still in flight is
 * exactly the use-after-free LVGL cannot guard against (lv_obj_tree.c only
 * resets the indev when the *deleted* object is the pressed one, not when
 * it is a child). */
static lv_obj_t *g_kbd_scrim;

static void kbd_build(void)
{
    /* parent the overlay to the persistent top layer, not the screen: the
     * stock handler may reopen folders (popping screens) underneath us */
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scrim, 360, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(scrim, th->scrim_input_opa, 0);
    lv_obj_set_style_bg_color(scrim, th->scrim_input, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_add_event_cb(scrim, kbd_cancel_cb, LV_EVENT_CLICKED, NULL);

    shell_soft_shadow(scrim, 20, 16, 320, 180, th->radius, 6);
    lv_obj_t *card = shell_glass_new(scrim);
    lv_obj_set_size(card, 320, 180);
    lv_obj_set_pos(card, 20, 16);
    lv_obj_set_style_bg_opa(card, th->kbd_card_opa, 0);

    lv_obj_t *ta = lv_textarea_create(card);
    lv_obj_set_size(ta, 288, 42);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 20);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_style_text_color(ta, shell_col_text(), 0);
    lv_obj_set_style_text_font(ta, th->font_cn, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(ta, th->btn_secondary, 0);
    lv_obj_set_style_border_color(ta, th->border_white, 0);
    lv_obj_set_style_border_opa(ta, th->card_border_opa, 0);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);

    kbd_mkbtn(card, str(LANG_CANCEL), 8, kbd_cancel_cb, false);
    kbd_mkbtn(card, str(LANG_SET_BOOL_YES), 192, kbd_ok_cb, true);

    g_kbd_scr = scrim;
    g_kbd_ta = ta;
    g_kbd_cn = false;
    g_pybuf[0] = '\0';
    kbd_make_kb();
    g_kbd_scrim = scrim;
}

int shell_kbd_input(char *text, int buflen)
{
    if (buflen <= 0)
        return -1;
    if (!g_kbd_scrim)
        kbd_build();

    g_kbd_done = false;
    g_kbd_ok = false;
    g_kbd_cn = false;
    g_kbd_symbols = false;
    g_kbd_shift = 0;
    g_pybuf[0] = '\0';
    kbd_set_page(false, false);
    lv_textarea_set_max_length(g_kbd_ta, buflen - 1);
    lv_textarea_set_text(g_kbd_ta, text ? text : "");
    lv_obj_add_state(g_kbd_ta, LV_STATE_FOCUSED);
    lv_obj_clear_flag(g_kbd_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_kbd_scrim);

    while (!g_kbd_done)
        lvgui_pump();

    int ret = -1;
    if (g_kbd_ok)
    {
        strlcpy(text, lv_textarea_get_text(g_kbd_ta), buflen);
        ret = 0;
    }
    cand_hide();
    lv_obj_add_flag(g_kbd_scrim, LV_OBJ_FLAG_HIDDEN);
    return ret;
}

/* ---- blocking yes/no dialog (gui_syncyesno_run stand-in) ---------------- */
static bool g_yesno_done;
static int  g_yesno_res;               /* 0 = yes, 1 = no */

static void yesno_btn_cb(lv_event_t *e)
{
    g_yesno_res = (int)(intptr_t)lv_event_get_user_data(e);
    g_yesno_done = true;
}

static lv_obj_t *yesno_mkbtn(lv_obj_t *parent, const char *txt, int x,
                             lv_color_t bg, uint8_t opa, lv_color_t fg,
                             intptr_t res)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 120, 40);
    lv_obj_set_pos(b, x, 112);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, opa, 0);
    lv_obj_set_style_border_color(b, th->border_white, 0);
    lv_obj_set_style_border_opa(b, th->card_border_opa, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, yesno_btn_cb, LV_EVENT_CLICKED, (void *)res);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    return b;
}

/* Shows the message and BLOCKS (pumping the UI loop) until answered:
 * 0 = yes, 1 = no (same numbering as enum yesno_res). Only call from a
 * lvgui_run_job() context. */
static lv_obj_t *g_yesno_scrim;
static lv_obj_t *g_yesno_msg;

static void yesno_build(void)
{
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scrim, 360, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scrim, th->scrim, 0);
    lv_obj_set_style_bg_opa(scrim, th->scrim_opa, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_add_event_cb(scrim, yesno_btn_cb, LV_EVENT_CLICKED, (void *)1);

    const int CW = 300, CH = 168;
    int cx = (360 - CW) / 2, cy = (480 - CH) / 2;
    shell_soft_shadow(scrim, cx, cy, CW, CH, th->radius, 6);
    lv_obj_t *card = shell_glass_new(scrim);
    lv_obj_set_size(card, CW, CH);
    lv_obj_set_pos(card, cx, cy);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, th->radius, 0);

    g_yesno_msg = lv_label_create(card);
    lv_obj_set_style_text_color(g_yesno_msg, th->fg_text, 0);
    lv_label_set_long_mode(g_yesno_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_yesno_msg, CW - 40);
    lv_obj_align(g_yesno_msg, LV_ALIGN_TOP_MID, 0, 18);

    yesno_mkbtn(card, str(LANG_SET_BOOL_NO), 18, th->btn_secondary, th->btn_secondary_opa,
                th->fg_text, 1);
    yesno_mkbtn(card, str(LANG_SET_BOOL_YES), CW - 138, th->accent_blue, LV_OPA_COVER,
                th->fg_inverse, 0);

    g_yesno_scrim = scrim;
}

int shell_yesno_run(const char *message)
{
    if (!message)
        message = "";
    if (!g_yesno_scrim)
        yesno_build();

    g_yesno_done = false;
    g_yesno_res = 1;                   /* tapping outside means no */
    lv_label_set_text(g_yesno_msg, message);
    lv_obj_clear_flag(g_yesno_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_yesno_scrim);

    while (!g_yesno_done)
        lvgui_pump();

    int ret = g_yesno_res;
    lv_obj_add_flag(g_yesno_scrim, LV_OBJ_FLAG_HIDDEN);
    return ret;
}

/* ---- blocking list picker (simplelist stand-in) ------------------------- */
static bool g_pick_done;
static int  g_pick_res;                /* selected index, -1 = cancel */
static bool g_pick_confirmed;          /* live picker: tapped-highlight = OK */
static int  g_pick_count;
static lv_obj_t **g_pick_rows;         /* live picker row handles */
static void (*g_pick_on_move)(int sel, void *ud);
static void *g_pick_move_ud;

/* Picker screens stay on the nav stack while their action runs: the
 * action may push a successor on top, so going back returns to the list
 * instead of exposing the parent for one frame (visible flash). */
static lv_obj_t *g_pick_stack[8];
static int g_pick_n;
static char g_pick_title[80];   /* title of the list currently on screen */

static void picker_forget(lv_obj_t *scr)
{
    for (int i = 0; i < g_pick_n; i++)
    {
        if (g_pick_stack[i] == scr)
        {
            memmove(&g_pick_stack[i], &g_pick_stack[i + 1],
                    (g_pick_n - i - 1) * sizeof(g_pick_stack[0]));
            g_pick_n--;
            return;
        }
    }
}

static void picker_add(lv_obj_t *scr)
{
    if (g_pick_n < (int)(sizeof(g_pick_stack) / sizeof(g_pick_stack[0])))
        g_pick_stack[g_pick_n++] = scr;
}

/* called when a job finishes: drop picker screens left on top */
void shell_pick_cleanup(void)
{
    while (nav_depth > 1 && g_pick_n > 0 &&
           g_pick_stack[g_pick_n - 1] == lv_scr_act())
    {
        g_pick_n--;
        shell_nav_pop_now();
    }
}

static void pick_row_cb(lv_event_t *e)
{
    g_pick_res = (int)(intptr_t)lv_event_get_user_data(e);
    g_pick_done = true;
}

/* ---- menu list screens (nav stack; global edge-swipe back pops them) ---- */

static void pick_close_cb(lv_event_t *e)
{
    picker_forget(lv_event_get_current_target(e));
    if (!g_pick_done)
    {
        g_pick_res = -1;
        g_pick_done = true;
    }
}

/* live preview rows: one tap applies the value and leaves, like the stock
 * option screen (the old "tap to preview, tap again to confirm" needed two
 * taps for every setting) */
static void pick_live_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    g_pick_res = i;
    if (g_pick_on_move)
        g_pick_on_move(i, g_pick_move_ud);
    g_pick_confirmed = true;
    g_pick_done = true;
    shell_nav_pop();
}

/* Push a full-screen menu list (house style: wallpaper, centred title,
 * hairline, scrollable glass rows). rows are added by the caller with
 * shell_menu_row_new(); on_close fires on LV_EVENT_DELETE (the global
 * edge-swipe gesture pops the nav stack). */
/* build (or rebuild) the contents of a menu screen */
static void menu_screen_build(lv_obj_t *scr, const char *title,
                              lv_obj_t **list_out, lv_event_cb_t on_close)
{
    shell_statusbar_build(scr);
    shell_header(scr, title ? title : "");

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, 360, 480 - 74);
    lv_obj_set_pos(list, 0, 74);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_style_pad_left(list, 10, 0);
    lv_obj_set_style_pad_right(list, 10, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_right(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, th->fg_text, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_30, LV_PART_SCROLLBAR);
    shell_style_shadow_off(list);
    if (list_out)
        *list_out = list;
    if (on_close)
        lv_obj_add_event_cb(scr, on_close, LV_EVENT_DELETE, NULL);
}

/* Rebuild the CONTENTS of an existing menu screen (keeps the screen and
 * its DELETE callbacks; no navigation happens). Used when a menu level is
 * re-rendered after returning from a submenu or an item action. */
void shell_menu_screen_rebuild(lv_obj_t *scr, const char *title,
                               lv_obj_t **list_out)
{
    lv_obj_clean(scr);
    menu_screen_build(scr, title, list_out, NULL);
}

lv_obj_t *shell_menu_screen_push(const char *title, lv_obj_t **list_out,
                                 lv_event_cb_t on_close)
{
    lv_obj_t *scr = shell_screen_new();
    menu_screen_build(scr, title, list_out, on_close);
    shell_nav_push(scr);
    return scr;
}

/* House-style menu row (matches the home entry rows): 44px frosted-glass
 * row, optional icon tile (tile colour = home palette index 0..5, or -1 for
 * the plain accent), optional '>' chevron. sel highlights with the
 * accent colour (selection lists / option previews). */
lv_obj_t *shell_menu_row_new(lv_obj_t *list, const char *icon, bool sym,
                             const char *label, bool chev, bool sel,
                             int idx, lv_event_cb_t cb, int grad)
{
    lv_obj_t *row = shell_glass_new(list);
    lv_obj_set_size(row, 336, 46);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (sel)
    {
        /* sapphire crystal: translucent blue so the frost still shows */
        lv_obj_set_style_bg_color(row, lv_color_mix(lv_color_white(),
                                  th->accent_blue, 40), 0);
        lv_obj_set_style_bg_grad_color(row,
            lv_color_darken(th->accent_blue, 30), 0);
        lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(row, global_settings.ui_dark_mode ? 150 : 128, 0);
        lv_obj_set_style_border_color(row,
            lv_color_lighten(th->accent_blue, 100), 0);
        lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_border_width(row, 1, 0);
    }
    if (cb)
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    if (icon && *icon)
    {
        lv_obj_t *tile = lv_obj_create(row);
        lv_obj_set_size(tile, 28, 28);
        lv_obj_align(tile, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_set_style_radius(tile, 9, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(tile,
            grad >= 0 ? shell_icon_grad(grad) : th->accent_blue, 0);
        shell_style_shadow_off(tile);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *il = lv_label_create(tile);
        lv_label_set_text(il, icon);
        /* symbol glyphs need the symbol font, text icons (first character)
         * need the multilingual UI font */
        lv_obj_set_style_text_font(il,
            sym ? th->font_symbol : th->font_cjk, 0);
        lv_obj_set_style_text_color(il, COL_INK, 0);
        lv_obj_center(il);
    }

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label ? label : "");
    lv_obj_set_style_text_color(l, sel ? th->fg_inverse : COL_TEXT, 0);
    lv_obj_set_style_text_font(l, th->font_cjk, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, (icon && *icon) ? 52 : 18, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, (icon && *icon) ? (336 - 52 - 26)
                                        : (336 - 18 - 14));

    if (chev)
    {
        lv_obj_t *c = lv_label_create(row);
        lv_label_set_text(c, ">");
        lv_obj_set_style_text_color(c, COL_TEXT_DIM, 0);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    return row;
}

/* one-shot per-row icons for the NEXT shell_list_pick*_ call (cleared when
 * that pick finishes); entries are LV_SYMBOL strings, NULL rows skip tile */
static const char **g_pick_icns;
/* one-shot per-row tile colours (home palette indexes) for the same call */
static const int *g_pick_grads;

void shell_pick_icons(const char **icons, int count)
{
    (void)count;
    g_pick_icns = icons;
}

void shell_pick_grads(const int *grads, int count)
{
    (void)count;
    g_pick_grads = grads;
}

/* Shows a titled list and BLOCKS (pumping the UI loop) until the user taps
 * a row or edge-swipes back. Returns the selected index, -1 when cancelled.
 * Only call from a lvgui_run_job() context. */
int shell_list_pick(const char *title, const char **items, int count,
                    int preselect)
{
    if (count <= 0 || !items)
        return -1;

    /* Same list re-shown by its owner loop (same title, same screen):
     * rebuild the CONTENTS in place — no navigation happens, so nothing
     * flashes. A DIFFERENT list (e.g. scan results over the wifi menu)
     * must PUSH: going back then reveals the menu underneath, and the
     * push itself ends any sticky status toast ("扫描中…"). */
    lv_obj_t *list;
    lv_obj_t *scr;
    bool same = (g_pick_n > 0 && g_pick_stack[g_pick_n - 1] == lv_scr_act() &&
                 g_pick_title[0] && title &&
                 !strcmp(g_pick_title, title));
    if (same)
    {
        scr = g_pick_stack[--g_pick_n];
        lv_obj_clean(scr);
        lv_obj_remove_event_cb(scr, pick_close_cb);
        menu_screen_build(scr, title, &list, pick_close_cb);
        shell_toast_clear();  /* in-place rebuild does no navigation */
    }
    else
    {
        scr = shell_menu_screen_push(title, &list, pick_close_cb);
    }
    snprintf(g_pick_title, sizeof g_pick_title, "%s", title ? title : "");

    g_pick_done = false;
    g_pick_res = -1;
    picker_add(scr);

    lv_obj_t *selrow = NULL;
    for (int i = 0; i < count; i++)
    {
        lv_obj_t *row = shell_menu_row_new(list,
            (g_pick_icns && g_pick_icns[i]) ? g_pick_icns[i] : NULL, false,
            items[i] ? items[i] : "", false, i == preselect, i,
            pick_row_cb,
            (g_pick_grads && g_pick_grads[i] >= 0) ? g_pick_grads[i] : -1);
        if (i == preselect)
            selrow = row;
    }
    if (selrow)
        lv_obj_scroll_to_view(selrow, LV_ANIM_OFF);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    while (!g_pick_done)
        lvgui_pump();
    lvgui_pump();               /* let the deferred nav pop run */
    g_pick_icns = NULL;
    g_pick_grads = NULL;

    return g_pick_res;
}

/* ---- live-refresh status list (netfm / bt-rx / wifi / dlna simplelists) --
 * Periodically calls on_tick (the stock simplelist ACTION_NONE callback,
 * which refreshes the underlying status data) and re-renders every row via
 * fill. Rows act like shell_list_pick rows. Returns picked index, -1 on
 * cancel. Only call from a lvgui_run_job() context. */
static lv_obj_t *g_rfr_rows[128];
static bool g_rfr_done;
static int g_rfr_res;
static void (*g_rfr_tick)(void *ud);
static void (*g_rfr_fill)(int idx, char *buf, size_t sz, void *ud);
static void *g_rfr_ud;

static void rfr_row_cb(lv_event_t *e)
{
    g_rfr_res = (int)(intptr_t)lv_event_get_user_data(e);
    g_rfr_done = true;
    shell_nav_pop();            /* deferred: event-cb safe */
}

static void rfr_close_cb(lv_event_t *e)
{
    (void)e;
    if (!g_rfr_done)
    {
        g_rfr_res = -1;
        g_rfr_done = true;
    }
}

static void rfr_timer_cb(lv_timer_t *t)
{
    (void)t;
    char buf[112];
    if (g_rfr_done)
        return;
    if (g_rfr_tick)
        g_rfr_tick(g_rfr_ud);
    if (!g_rfr_fill)
        return;
    for (int i = 0; i < 128 && g_rfr_rows[i]; i++)
    {
        buf[0] = '\0';
        g_rfr_fill(i, buf, sizeof buf, g_rfr_ud);
        lv_obj_t *lbl = lv_obj_get_child(g_rfr_rows[i], 0);
        if (lbl)
            lv_label_set_text(lbl, buf);
    }
}

int shell_list_pick_refresh(const char *title, int count, int preselect,
                            int period_ms,
                            void (*on_tick)(void *ud),
                            void (*fill)(int idx, char *buf, size_t sz,
                                         void *ud),
                            void *ud)
{
    if (count <= 0 || count > 128)
        return -1;
    g_rfr_done = false;
    g_rfr_res = -1;
    g_rfr_tick = on_tick;
    g_rfr_fill = fill;
    g_rfr_ud = ud;
    memset(g_rfr_rows, 0, sizeof g_rfr_rows);

    char buf[112];
    lv_obj_t *list;
    shell_menu_screen_push(title, &list, rfr_close_cb);
    for (int i = 0; i < count; i++)
    {
        buf[0] = '\0';
        if (fill)
            fill(i, buf, sizeof buf, ud);
        g_rfr_rows[i] = shell_menu_row_new(list,
            (g_pick_icns && g_pick_icns[i]) ? g_pick_icns[i] : NULL, false,
            buf, false, i == preselect, i, rfr_row_cb,
            (g_pick_grads && g_pick_grads[i] >= 0) ? g_pick_grads[i] : -1);
    }
    if (preselect >= 0 && preselect < count)
        lv_obj_scroll_to_view(g_rfr_rows[preselect], LV_ANIM_OFF);
    lv_timer_t *tm = lv_timer_create(rfr_timer_cb,
                                     period_ms >= 200 ? period_ms : 200,
                                     NULL);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    while (!g_rfr_done)
        lvgui_pump();
    lv_timer_del(tm);
    memset(g_rfr_rows, 0, sizeof g_rfr_rows);
    lvgui_pump();               /* let the deferred nav pop run */
    g_pick_icns = NULL;
    g_pick_grads = NULL;
    return g_rfr_res;
}

/* Live-preview variant for option_screen: on_move fires on every highlight
 * change; tapping the highlighted row confirms; edge-swipe cancels (-1). */
int shell_list_pick_live(const char *title, const char **items, int count,
                         int preselect,
                         void (*on_move)(int sel, void *ud), void *ud)
{
    if (count <= 0 || !items)
        return -1;
    g_pick_done = false;
    g_pick_res = preselect;
    g_pick_confirmed = false;
    g_pick_count = count;
    g_pick_on_move = on_move;
    g_pick_move_ud = ud;

    lv_obj_t *list;
    shell_menu_screen_push(title, &list, pick_close_cb);

    g_pick_rows = malloc(count * sizeof(lv_obj_t *));
    for (int i = 0; i < count; i++)
        g_pick_rows[i] = shell_menu_row_new(list,
            (g_pick_icns && g_pick_icns[i]) ? g_pick_icns[i] : NULL, false,
            items[i] ? items[i] : "", false, i == preselect, i,
            pick_live_row_cb,
            (g_pick_grads && g_pick_grads[i] >= 0) ? g_pick_grads[i] : -1);
    if (preselect >= 0 && preselect < count)
        lv_obj_scroll_to_view(g_pick_rows[preselect], LV_ANIM_OFF);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    while (!g_pick_done)
        lvgui_pump();
    lvgui_pump();

    int ret = g_pick_confirmed ? g_pick_res : -1;
    free(g_pick_rows);
    g_pick_rows = NULL;
    g_pick_icns = NULL;
    g_pick_grads = NULL;
    return ret;
}

/* ---- splash / toast (splashf stand-in) ---------------------------------- */
static lv_obj_t *g_splash;
static bool g_toast_sticky;

/* Classic splash(0,...) means "stay until the next screen is drawn" (the
 * classic renderer just overwrote the framebuffer). Our toast lives on
 * lv_layer_top and would linger forever, so screen transitions clear it. */
void shell_toast_clear(void)
{
    if (g_splash)
    {
        lv_obj_del(g_splash);
        g_splash = NULL;
    }
    g_toast_sticky = false;
}

bool shell_toast_is_sticky(void)
{
    return g_toast_sticky;
}

/* Shows a small glass toast. ticks>0: blocks (pumping the UI loop) for that
 * long, then removes it (classic splash slept the same amount). ticks==0:
 * shows it, or removes the current one when msg is empty/NULL. */
/* Android-style toast: translucent black pill, white text, optional icon
 * (pass an LVGL symbol glyph, or NULL). Blocks for `ticks` when > 0. */
void shell_toast(int ticks, const char *msg, const char *icon)
{
    shell_toast_clear();
    if (!msg || !msg[0])
        return;

    lv_obj_t *scr = lv_layer_top();
    g_splash = lv_obj_create(scr);
    lv_obj_clear_flag(g_splash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_splash, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g_splash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_splash, LV_OPA_80, 0);
    lv_obj_set_style_radius(g_splash, th->radius_big, 0);
    lv_obj_set_style_border_width(g_splash, 0, 0);
    lv_obj_set_style_shadow_width(g_splash, 0, 0);
    lv_obj_set_style_pad_hor(g_splash, 20, 0);
    lv_obj_set_style_pad_ver(g_splash, 12, 0);
    lv_obj_set_style_pad_column(g_splash, 10, 0);
    lv_obj_set_flex_flow(g_splash, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_splash, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (icon && icon[0])
    {
        lv_obj_t *il = lv_label_create(g_splash);
        lv_label_set_text(il, icon);
        lv_obj_set_style_text_font(il, th->font_symbol, 0);
        lv_obj_set_style_text_color(il, lv_color_white(), 0);
    }

    lv_obj_t *l = lv_label_create(g_splash);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(l, 240, 0);
    lv_obj_align(g_splash, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);

    if (ticks > 0)
    {
        uint32_t ms = (uint32_t)ticks * 1000 / HZ;
        uint32_t t0 = lv_tick_get();
        while (lv_tick_elaps(t0) < ms && g_splash)
            lvgui_pump();
        shell_toast_clear();
    }
    else
    {
        g_toast_sticky = true;
    }
}

void shell_splash(int ticks, const char *msg)
{
    shell_toast(ticks, msg, NULL);
}

/* ---- artwork glue: backend decoder -> LVGL image descriptor -------------- */

lvui_art_t *lvui_art_wrap(int w, int h, void *pixels)
{
    lvui_art_t *a = malloc(sizeof *a);
    if (!a)
    {
        free(pixels);
        return NULL;
    }
    a->dsc.header.always_zero = 0;
    a->dsc.header.w = w;
    a->dsc.header.h = h;
    a->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    a->dsc.data_size = (uint32_t)w * h * 2;
    a->dsc.data = pixels;
    return a;
}

lvui_art_t *lib_track_art_sized(const char *path, int w, int h)
{
    be_img_t *im = be_track_art(path, w, h);
    if (!im)
        return NULL;
    lvui_art_t *a = lvui_art_wrap(im->w, im->h, im->pixels);
    free(im);
    return a;
}

lvui_art_t *lib_track_art(const char *path)
{
    return lib_track_art_sized(path, BE_ART_SIDE, BE_ART_SIDE);
}

void lvui_art_free(lvui_art_t *art)
{
    if (art)
    {
        free((void *)art->dsc.data);
        free(art);
    }
}

lv_obj_t *shell_glass_new(lv_obj_t *parent)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(g, th->card_opa, 0);
    /* glassmorphism recipe: almost clear fill (white @ ~12%), the frost
     * comes from the pre-blurred wallpaper showing through, not from a
     * tinted pane.  1px light rim + soft drop shadow give it the edge. */
    lv_obj_set_style_bg_color(g, lv_color_white(), 0);
    lv_obj_set_style_border_width(g, 1, 0);
    lv_obj_set_style_border_color(g, th->border_white, 0);
    lv_obj_set_style_border_opa(g, th->card_border_opa, 0);
    lv_obj_set_style_radius(g, th->radius, 0);
    /* lift the pane off the wallpaper: soft shadow (0 8 32 rgba(0,0,0,.25)) */
    lv_obj_set_style_shadow_width(g, 24, 0);
    lv_obj_set_style_shadow_spread(g, 0, 0);
    lv_obj_set_style_shadow_ofs_x(g, 0, 0);
    lv_obj_set_style_shadow_ofs_y(g, 4, 0);
    lv_obj_set_style_shadow_color(g, th->sh_dark, 0);
    lv_obj_set_style_shadow_opa(g, th->glass_shadow_opa, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_style_text_font(g, th->font_cjk, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(g, true, 0);
    return g;
}

/* ---- entry row --------------------------------------------------------- */
typedef struct
{
    const char *icon;     /* single CJK glyph shown in colored square */
    const char *title;    /* literal fallback (used when lang == 0) */
    int lang;             /* LANG_ token; 0 = use title */
    int grad;             /* index into icon_grad */
    void (*on_open)(lv_event_t *);
} row_desc_t;

static void row_build(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                      lv_coord_t w, const row_desc_t *d)
{
    lv_obj_t *row = shell_glass_new(parent);
    lv_obj_set_size(row, w, 46);
    lv_obj_set_pos(row, x, y);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, th->card_opa, 0);
    lv_obj_set_style_border_opa(row, th->card_border_opa, 0);
    lv_obj_set_style_radius(row, th->radius, 0);
    lv_obj_add_event_cb(row, d->on_open, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_obj_create(row);
    lv_obj_set_size(icon, 28, 28);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_radius(icon, 9, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(icon, icon_grad[d->grad], 0);
    shell_style_shadow_off(icon);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE |
                              LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *il = lv_label_create(icon);
    lv_label_set_text(il, d->icon);
    lv_obj_set_style_text_font(il, th->font_cjk, 0);
    lv_obj_set_style_text_color(il, COL_INK, 0);
    lv_obj_center(il);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, d->lang ? str(d->lang) : d->title);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_font(title, th->font_cjk, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 52, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, w - 52 - 26);

    lv_obj_t *chev = lv_label_create(row);
    lv_label_set_text(chev, ">");
    lv_obj_set_style_text_color(chev, COL_TEXT_DIM, 0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);
}

/* ---- placeholder sub-screen (until later milestones) ------------------ */
static void open_placeholder(const char *title)
{
    lv_obj_t *scr = shell_screen_new();

    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *note = lv_label_create(scr);
    lv_label_set_text(note, str(LANG_N3_NOT_IMPLEMENTED));
    lv_obj_set_style_text_color(note, COL_TEXT_DIM, 0);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 100);

    shell_nav_push(scr);
}

static void open_library(lv_event_t *e)   { (void)e; library_open(); }
static void open_playlists(lv_event_t *e) { (void)e; shell_playlists_show(); }
static void open_files(lv_event_t *e)     { (void)e; shell_files_show(); }
/* menu rows run the stock menu objects through the native do_menu hook */
static void menu_job(void *p) { void (*fn)(void) = (void (*)(void))p; fn(); }

static void open_plugins(lv_event_t *e)   { (void)e; lvgui_run_job(menu_job, (void *)be_plugin_menu); }
static void open_bluetooth(lv_event_t *e) { (void)e; lvgui_run_job(menu_job, (void *)be_bluetooth_menu); }
static void open_wifi(lv_event_t *e)      { (void)e; lvgui_run_job(menu_job, (void *)be_wifi_menu); }
static void open_system(lv_event_t *e)    { (void)e; lvgui_run_job(menu_job, (void *)be_system_menu); }
static void open_shortcuts(lv_event_t *e) { (void)e; lvgui_run_job(menu_job, (void *)be_shortcut_menu); }
static void open_bookmarks(lv_event_t *e) { (void)e; lvgui_run_job(menu_job, (void *)be_bookmark_load); }
static void open_settings(lv_event_t *e)  { (void)e; lvgui_run_job(menu_job, (void *)be_settings_menu); }

/* Settings -> Startup/Shutdown -> Start Screen, applied once at boot.
 * Same arithmetic as root_menu_setup_screens(): the setting stores
 * GO_TO_* + 2, 0 means "previous screen" (global_status.last_screen). */
void shell_start_screen_apply(void)
{
    switch (be_start_screen())
    {
    case GO_TO_FILEBROWSER:
        shell_files_show();
        break;
    case GO_TO_DBBROWSER:
        library_open();
        break;
    case GO_TO_WPS:
        shell_player_show();
        break;
    case GO_TO_RECENTBMARKS:
        open_bookmarks(NULL);
        break;
    case GO_TO_PLUGIN:
        open_plugins(NULL);
        break;
    case GO_TO_PLAYLISTS_SCREEN:
        shell_playlists_show();
        break;
    default:                  /* GO_TO_ROOT / GO_TO_MAINMENU: Home */
        break;
    }
}
int lvgui_dlna_view(void);

static void wps_job(void *p)
{
    (void)p;
    if (be_audio_status())
        shell_player_show();
    else
        be_resume_playback();
}

/* Runs as a UI job like the stock menu screens: lvgui_dlna_view() pushes a
 * screen and pumps, which must not happen inside the click event itself. */
static void dlna_view_job(void *p)
{
    (void)p;
    lvgui_dlna_view();
}

static void open_wps(lv_event_t *e)
{
    (void)e;
    if (be_dlna_active())
        lvgui_run_job(dlna_view_job, NULL);
    else
        lvgui_run_job(wps_job, NULL);
}

/* ---- home screen ------------------------------------------------------- */
/* mini player dock: cover chip, title, elapsed/total, play-pause toggle */
static lv_obj_t *dock_l1, *dock_l2, *dock_btn;
static lv_obj_t *dock_art;          /* cover thumbnail in the dock chip */
static lv_obj_t *dock_glyph;        /* fallback music note */
static lv_obj_t *dock_chip;         /* 32x32 cover chip container */
static lvui_art_t *dock_art_dsc;
static char dock_path[MAX_PATH];
static lv_timer_t *dock_timer;

static char dock_art_path[MAX_PATH];
static lv_img_dsc_t *dock_dsc_p;

static void dock_art_drop(void)
{
    if (dock_art_dsc)
    {
        lvui_art_free(dock_art_dsc);
        dock_art_dsc = NULL;
    }
    if (dock_art)
    {
        lv_obj_del(dock_art);
        dock_art = NULL;
    }
    dock_art_path[0] = '\0';
    if (dock_dsc_p)
    {
        free(dock_dsc_p);
        dock_dsc_p = NULL;
    }
    if (dock_glyph)
        lv_obj_clear_flag(dock_glyph, LV_OBJ_FLAG_HIDDEN);
}

static void dock_set_local_art(const char *trk)
{
    if (!trk)
        trk = "";
    if (strcmp(dock_art_path, trk) == 0)
        return;
    dock_art_drop();
    strlcpy(dock_art_path, trk, sizeof dock_art_path);
    if (!trk[0])
        return;
    dock_art_dsc = lib_track_art_sized(trk, 32, 32);
    if (dock_art_dsc)
    {
        dock_art = lv_img_create(dock_chip ? dock_chip
                                        : lv_obj_get_parent(dock_btn));
        lv_img_set_src(dock_art, &dock_art_dsc->dsc);
        lv_obj_center(dock_art);
        lv_obj_clear_flag(dock_art, LV_OBJ_FLAG_CLICKABLE);
        if (dock_glyph)
            lv_obj_add_flag(dock_glyph, LV_OBJ_FLAG_HIDDEN);
    }
}


/* DLNA cover: decoded from the RAM copy handed over by the DLNA output. */
static unsigned dock_dlna_gen;

static void dock_set_dlna_art_impl(void)
{
    static uint16_t pix[32 * 32];
    const unsigned char *data;
    const void *src;
    lv_img_dsc_t *dsc;
    size_t len = 0;
    unsigned gen = 0, pg = 0;
    int pw = 0, ph = 0, x, y;

    data = dlna_output_art_data(&len, &gen);
    if (gen == dock_dlna_gen)
        return;
    dock_dlna_gen = gen;
    dock_art_drop();
    if (!data || !len)
        return;

    src = be_dlna_art_pixels(&pw, &ph, &pg);
    if (pg != gen || !src || pw <= 0)
    {
        if (!be_dlna_art_decode(data, len, gen))
            return;
        src = be_dlna_art_pixels(&pw, &ph, &pg);
        if (!src || pw <= 0 || ph <= 0)
            return;
    }

    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++)
            pix[y * 32 + x] =
                ((const uint16_t *)src)[(size_t)(y * ph / 32) * (size_t)pw +
                                        (size_t)(x * pw / 32)];

    dsc = calloc(1, sizeof *dsc);
    if (!dsc)
        return;
    dsc->header.always_zero = 0;
    dsc->header.w = 32;
    dsc->header.h = 32;
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->data_size = 32 * 32 * 2;
    dsc->data = (const uint8_t *)pix;
    dock_dsc_p = dsc;

    dock_art = lv_img_create(dock_chip ? dock_chip
                                    : lv_obj_get_parent(dock_btn));
    lv_img_set_src(dock_art, dsc);
    lv_obj_center(dock_art);
    lv_obj_clear_flag(dock_art, LV_OBJ_FLAG_CLICKABLE);
    if (dock_glyph)
        lv_obj_add_flag(dock_glyph, LV_OBJ_FLAG_HIDDEN);
}

static void dock_set_dlna_art(void)
{
    dlna_output_art_lock();
    dock_set_dlna_art_impl();
    dlna_output_art_unlock();
}

/* ---- lock screen (clock only, up-swipe to unlock) ---------------------- */
int lvgl_shell_locked;
static lv_timer_t *lock_tm;
static lv_obj_t *lock_time_l;

static void lock_tick_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char b[12];

    if (!lt || !lock_time_l)
        return;
    snprintf(b, sizeof b, "%02d:%02d", lt->tm_hour, lt->tm_min);
    lv_label_set_text(lock_time_l, b);
}

/* called from the input layer once an upward swipe is recognised */
void shell_lock_hide(void)
{
    if (!lvgl_shell_locked)
        return;
    lvgl_shell_locked = 0;
    if (lock_tm)
    {
        lv_timer_del(lock_tm);
        lock_tm = NULL;
    }
    lock_time_l = NULL;
    shell_nav_pop();
}

void shell_lock_show(void)
{
    lv_obj_t *scr = shell_screen_new();
    lv_obj_t *h;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x070b14), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lock_time_l = lv_label_create(scr);
    lv_obj_set_style_text_font(lock_time_l, th->font_big, 0);
    lv_obj_set_style_text_color(lock_time_l, lv_color_white(), 0);
    lv_obj_align(lock_time_l, LV_ALIGN_CENTER, 0, -20);

    h = lv_label_create(scr);
    lv_label_set_text(h, str(LANG_N3_SWIPE_UNLOCK));
    lv_obj_set_style_text_font(h, th->font_cjk, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x8899aa), 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -40);

    lvgl_shell_locked = 1;
    lock_tick_cb(NULL);
    lock_tm = lv_timer_create(lock_tick_cb, 1000, NULL);
    shell_nav_push(scr);
}

static void dock_tick(lv_timer_t *t)
{
    (void)t;
    if (!dock_l1 || !lcd_active())
        return;

    char tb[128], sb[64];
    const char *title = NULL;
    const char *sub = NULL;
    bool local = false;

    const char *trk = be_audio_track_path();
    int st = be_audio_status();

    if (be_netfm_active())
    {
        be_netfm_name(tb, sizeof tb);
        title = tb[0] ? tb : str(LANG_N3_NET_RADIO);
        sub = str(LANG_N3_NET_RADIO);
    }
    else if (be_dlna_active())
    {
        long pos, dur;
        be_dlna_name(tb, sizeof tb);
        title = tb[0] ? tb : "DLNA";
        be_dlna_time(&pos, &dur);
        pos /= 1000;
        dur /= 1000;
        snprintf(sb, sizeof sb, "DLNA %ld:%02ld / %ld:%02ld",
                 pos / 60, pos % 60, dur / 60, dur % 60);
        sub = sb;
    }
    else if (be_bt_rx_active())
    {
        be_bt_rx_peer(tb, sizeof tb);
        title = tb[0] ? tb : str(LANG_BT_RX);
        sub = str(LANG_BT_RX);
    }
    else if (trk && trk[0] && (st & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)))
    {
        char namebuf[MAX_PATH];
        const char *base = strrchr(trk, '/');
        const char *ttl;
        base = base ? base + 1 : trk;
        strlcpy(namebuf, base, sizeof namebuf);
        char *dot = strrchr(namebuf, '.');
        if (dot && dot != namebuf)
            *dot = '\0';
        ttl = be_audio_track_title();
        title = shell_cs(tb, sizeof tb, (ttl && ttl[0]) ? ttl : namebuf);
        long el = be_audio_track_elapsed() / 1000;
        long len = be_audio_track_length() / 1000;
        snprintf(sb, sizeof sb, "%ld:%02ld / %ld:%02ld",
                 el / 60, el % 60, len / 60, len % 60);
        sub = sb;
        local = true;
    }

    if (!title)
    {
        if (dock_path[0])
        {
            dock_path[0] = '\0';
            lv_label_set_text(dock_l1, str(LANG_N3_NOTHING_PLAYING));
            lv_label_set_text(dock_l2, str(LANG_N3_TAP_TO_OPEN));
            lv_label_set_text(dock_btn, LV_SYMBOL_PLAY);
        }
        return;
    }

    if (strcmp(dock_path, title) != 0)
    {
        strlcpy(dock_path, title, sizeof dock_path);
        lv_label_set_text(dock_l1, title);
    }
    lv_label_set_text(dock_l2, sub);
    lv_label_set_text(dock_btn,
                      (st & AUDIO_STATUS_PAUSE) ? LV_SYMBOL_PLAY
                                                : LV_SYMBOL_PAUSE);
    if (local)
        dock_set_local_art(trk);
    else if (be_dlna_active())
        dock_set_dlna_art();
    else
    {
        dock_dlna_gen = 0;
        dock_art_drop();
    }
}

static void dock_playpause_cb(lv_event_t *e)
{
    (void)e;
    be_audio_pause_toggle();
}

static void home_build(lv_obj_t *scr)
{
    statusbar_build(scr);

    lv_obj_t *heel = lv_label_create(scr);
    lv_label_set_text(heel, str(LANG_N3_DOCK_SLOGAN));
    lv_obj_set_style_text_font(heel, th->font_title, 0);
    lv_obj_set_style_text_color(heel, COL_TEXT, 0);
    lv_obj_align(heel, LV_ALIGN_TOP_LEFT, 12, 26);

    /* entry rows — follow the root-menu configuration (设置→主菜单 plugin
     * order/hides); falls back to the full default set.  The bottom dock is
     * the single playback entry point; no duplicate continue card here. */
    static const struct
    {
        const char *name;
        const char *icon;
        int grad;
        int lang, alt_lang;    /* alt used when nothing is playing */
        void (*on_open)(lv_event_t *);
    } home_map[] =
    {
        { "bookmarks",   "签", 1, LANG_BOOKMARK_MENU_RECENT_BOOKMARKS, 0, open_bookmarks },
        { "files",       "夹", 2, LANG_DIR_BROWSER,     0, open_files },
        { "database",    "乐", 0, LANG_TAGCACHE,         0, open_library },
        { "wps",         "播", 4, LANG_NOW_PLAYING, LANG_RESUME_PLAYBACK, open_wps },
        { "settings",    "设", 4, LANG_SETTINGS,         0, open_settings },
        { "playlists",   "单", 1, LANG_PLAYLISTS,        0, open_playlists },
        { "bluetooth",   "蓝", 4, LANG_BLUETOOTH,        0, open_bluetooth },
        { "wifi",        "网", 2, LANG_WIFI,             0, open_wifi },
        { "plugins",     "插", 3, LANG_PLUGINS,          0, open_plugins },
        { "system_menu", "系", 3, LANG_SYSTEM,           0, open_system },
        { "shortcuts",   "快", 5, LANG_SHORTCUTS,        0, open_shortcuts },
    };
    extern int lvgui_root_names(const char **names, int max);

    const char *names[16];
    int n = lvgui_root_names(names, 16);

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_set_size(col, 360, 326);
    lv_obj_set_pos(col, 0, 62);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(col, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(col, th->border_white, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(col, 60, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(col, 2, LV_PART_SCROLLBAR);
    shell_scrollbar_setup(col);

    for (int i = 0, built = 0; i < n || (n <= 0 && i < (int)(sizeof(home_map) / sizeof(home_map[0]))); i++)
    {
        const char *key = (n > 0) ? names[i] : home_map[i].name;
        row_desc_t d;

        if (!key)
            continue;
        unsigned j;
        for (j = 0; j < sizeof(home_map) / sizeof(home_map[0]); j++)
        {
            if (!strcmp(key, home_map[j].name))
                break;
        }
        if (j >= sizeof(home_map) / sizeof(home_map[0]))
            continue;                       /* recording/radio etc.: N/A */
        d.icon    = home_map[j].icon;
        d.title   = home_map[j].name;
        d.lang    = home_map[j].alt_lang && !be_audio_status()
                        ? home_map[j].alt_lang : home_map[j].lang;
        d.grad    = home_map[j].grad;
        d.on_open = home_map[j].on_open;
        row_build(col, 12, 56 * (lv_coord_t)built++, 336, &d);
    }

    /* mini player dock: cover | title + time | play-pause button */
    shell_soft_shadow(scr, 10, 414, 340, 56, th->radius, 7);
    lv_obj_t *mini = shell_glass_new(scr);
    lv_obj_set_size(mini, 340, 56);
    lv_obj_set_pos(mini, 10, 414);
    lv_obj_add_flag(mini, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mini, open_wps, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mcov = lv_obj_create(mini);
    lv_obj_set_size(mcov, 32, 32);
    lv_obj_align(mcov, LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_set_style_radius(mcov, 8, 0);
    lv_obj_set_style_bg_opa(mcov, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(mcov, COL_ACCENT2, 0);
    lv_obj_set_style_border_width(mcov, 0, 0);
    shell_style_shadow_off(mcov);
    lv_obj_clear_flag(mcov, LV_OBJ_FLAG_SCROLLABLE |
                               LV_OBJ_FLAG_CLICKABLE);
    dock_chip = mcov;
    lv_obj_t *mglyph = lv_label_create(mcov);
    dock_glyph = mglyph;
    lv_label_set_text(mglyph, "♪");
    lv_obj_set_style_text_color(mglyph, th->fg_inverse, 0);
    lv_obj_center(mglyph);

    lv_obj_t *mt = lv_label_create(mini);
    lv_label_set_text(mt, str(LANG_N3_NOTHING_PLAYING));
    lv_obj_set_style_text_color(mt, COL_TEXT, 0);
    lv_obj_align(mt, LV_ALIGN_LEFT_MID, 48, -12);
    lv_label_set_long_mode(mt, LV_LABEL_LONG_DOT);
    lv_obj_set_width(mt, 210);
    dock_l1 = mt;

    lv_obj_t *ms = lv_label_create(mini);
    lv_label_set_text(ms, str(LANG_N3_TAP_TO_OPEN));
    lv_obj_set_style_text_color(ms, COL_TEXT_DIM, 0);
    lv_obj_align(ms, LV_ALIGN_LEFT_MID, 48, 12);
    lv_label_set_long_mode(ms, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ms, 210);
    dock_l2 = ms;

    lv_obj_t *mb = lv_obj_create(mini);
    lv_obj_set_size(mb, 36, 36);
    lv_obj_align(mb, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_radius(mb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(mb, th->chip_opa, 0);
    lv_obj_set_style_bg_color(mb, th->btn_secondary, 0);
    lv_obj_set_style_border_width(mb, 1, 0);
    lv_obj_set_style_border_color(mb, th->border_white, 0);
    lv_obj_set_style_border_opa(mb, th->card_border_opa, 0);
    shell_style_shadow_off(mb);
    lv_obj_clear_flag(mb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mb, dock_playpause_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mg = lv_label_create(mb);
    lv_label_set_text(mg, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(mg, th->font_symbol, 0);
    lv_obj_set_style_text_color(mg, COL_TEXT, 0);
    lv_obj_center(mg);
    dock_btn = mg;

    if (!dock_timer)
        dock_timer = lv_timer_create(dock_tick, 1000, NULL);
}

/* ---- shared blocks for feature screens (M2+) --------------------------- */
void shell_header(lv_obj_t *scr, const char *title)
{
    /* no back button: the global edge-swipe gesture handles going back */
    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_set_style_text_font(t, th->font_title, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 12, 26);
}

lv_color_t shell_col_text(void) { return COL_TEXT; }
lv_color_t shell_col_dim(void)  { return COL_TEXT_DIM; }
lv_color_t shell_col_link(void) { return COL_LINK; }
lv_color_t shell_col_ink(void)  { return COL_INK; }

/* Rebuild the home screen in place (root menu configuration changed).
 * Must run from an async/lv_timer context, not from inside an event. */
static void home_rebuild_do(void *p)
{
    (void)p;
    if (!nav_stack[0])
        return;
    if (dock_art)
    {
        lv_obj_del(dock_art);
        dock_art = NULL;
    }
    if (dock_art_dsc)
    {
        lvui_art_free(dock_art_dsc);
        dock_art_dsc = NULL;
    }
    dock_chip = dock_glyph = dock_l1 = dock_l2 = dock_btn = NULL;
    dock_path[0] = '\0';

    lv_obj_t *nh = shell_screen_new();
    home_build(nh);
    lv_obj_t *old = nav_stack[0];
    nav_stack[0] = nh;
    if (nav_depth <= 1)
        lv_scr_load(nh);
    lv_obj_del(old);
    statusbar_tick(NULL);
}

void shell_home_rebuild(void)
{
    lv_async_call(home_rebuild_do, NULL);
}

/* repaint the procedural wallpaper after ui_theme_init() changed the
 * palette (live theme switch); screens keep pointing at the same image */
void shell_theme_refresh(void)
{
    backdrop_init();
    lv_obj_invalidate(lv_scr_act());
}

lv_color_t shell_icon_grad(int i)
{
    return icon_grad[(unsigned)i % (sizeof(icon_grad) / sizeof(icon_grad[0]))];
}

/* ---- entry ------------------------------------------------------------- */
void lvgui_shell_start(void)
{
    ui_theme_init();
    backdrop_init();

    lv_obj_t *home = shell_screen_new();
    home_build(home);
    nav_stack[nav_depth++] = home;
    lv_scr_load(home);

    lv_timer_create(statusbar_tick, 1000, NULL);
    statusbar_tick(NULL);
}
