/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\__/  \/   \/   \/
 *
 * Native LVGL menu engine -- a rendering drop-in for the classic
 * apps/menu.c do_menu().  Item order, visibility rules, callbacks and
 * handlers are the stock ones; only the drawing is ours.  It also
 * replaces the classic option_screen() so MT_SETTING items stay native.
 *
 * This is a BLOCKING engine: it pumps the UI loop until the user acts,
 * so it must only be reached from an lvgui_run_job() context (menu.c
 * diverts do_menu() here under HAVE_LVGL_UI + lvgui_in_job()).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.  See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/

#include "config.h"

#ifdef HAVE_LVGL_UI

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "lvgl.h"
#include "lvgl_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_backend.h"
#include "menus/eq_menu.h"

#include "system.h"
#include "settings.h"
#include "settings_list.h"
#include "menu.h"
#include "option_select.h"
#include "lang.h"
#include "gui/splash.h"
#include "sound.h"
#include "string-extra.h"
#include "root_menu.h"
#include "dir.h"
#include "file.h"
#include "tree.h"
#include "filetypes.h"
#include "touchscreen.h"
#include "netfm_stream.h"
#include "cuesheet.h"
#include "dlna/dlna_stream.h"
#include "dlna/dlna_output.h"
#include <math.h>

#define MAX_MENUS 8
#define MAX_MENU_ITEMS 64

/* ---- shared event state (UI is single threaded) ------------------------ */
static int  g_menu_row;      /* clicked row (0-based), -1 = none          */
static lv_obj_t *g_menu_scr;   /* current menu screen (rebuilt in place)    */
static bool g_menu_cancel;   /* the scrim (outside the card) was tapped   */
/* a settings change rebuilt the whole UI (theme/font size): unwind the menu
 * stack without touching the screens that shell_nav_home() removed */
static bool g_menu_abort;

static void menu_row_cb(lv_event_t *e)
{
    g_menu_row = (int)(intptr_t)lv_event_get_user_data(e);
}

static void menu_scrim_cb(lv_event_t *e)
{
    (void)e;
    g_menu_cancel = true;
}

/* DELETE on a menu screen: the edge-swipe gesture popped it */
static void menu_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_menu_row < 0)
        g_menu_cancel = true;
}

/* ---- callbacks / labels (mirror apps/menu.c) --------------------------- */

static int empty_menu_cb(int action, const struct menu_item_ex *it,
                         struct gui_synclist *l)
{
    (void)it; (void)l;
    return action;
}

static menu_callback_type menu_get_cb(const struct menu_item_ex *m)
{
    menu_callback_type cb;
    if (m->flags & (MENU_HAS_DESC | MENU_DYNAMIC_DESC))
        cb = m->callback_and_desc->menu_callback;
    else
        cb = m->menu_callback;
    return cb ? cb : empty_menu_cb;
}

static bool menu_item_visible(const struct menu_item_ex *container, int i)
{
    menu_callback_type cb;
    const struct menu_item_ex *it;
    if ((container->flags & MENU_TYPE_MASK) == MT_RETURN_ID)
    {
        cb = menu_get_cb(container);
        it = (const struct menu_item_ex *)(intptr_t)i;
    }
    else
    {
        it = container->submenus[i];
        cb = menu_get_cb(it);
    }
    return cb(ACTION_REQUEST_MENUITEM, it, NULL) != ACTION_EXIT_MENUITEM;
}

static const char *menu_item_label(const struct menu_item_ex *container, int i,
                                   char *buf, size_t bufsz)
{
    int type = container->flags & MENU_TYPE_MASK;
    const struct menu_item_ex *item;

    if (type == MT_RETURN_ID)
    {
        if (container->flags & MENU_DYNAMIC_DESC)
            return container->menu_get_name_and_icon->list_get_name(i,
                container->menu_get_name_and_icon->list_get_name_data,
                buf, bufsz);
        return P2STR((unsigned char *)container->strings[i]);
    }

    item = container->submenus[i];
    if ((item->flags & MENU_DYNAMIC_DESC) && (type != MT_SETTING_W_TEXT))
        return item->menu_get_name_and_icon->list_get_name(i,
            item->menu_get_name_and_icon->list_get_name_data, buf, bufsz);

    int itype = item->flags & MENU_TYPE_MASK;
    if (itype == MT_SETTING || itype == MT_SETTING_W_TEXT)
    {
        const struct settings_list *v = find_setting(item->variable);
        return v ? (const char *)str(v->lang_id) : "Not Done yet!";
    }
    return P2STR(item->callback_and_desc->desc);
}

static const char *menu_title(const struct menu_item_ex *menu, char *buf,
                              size_t bufsz)
{
    if (menu->flags & MENU_HAS_DESC)
        return P2STR(menu->callback_and_desc->desc);
    if (menu->flags & MENU_DYNAMIC_DESC)
        return menu->menu_get_name_and_icon->list_get_name(-1,
            menu->menu_get_name_and_icon->list_get_name_data, buf, bufsz);
    return "";
}

/* ---- build the glass sheet for one menu level --------------------------- */
/* Returns the full-screen scrim (owns the card); fills vis[] with the
 * ORIGINAL menu indices of the visible rows and *nvis with their count. */
/* stock gui/icon.h id (or one of our private negative pseudo-ids) mapped
 * to an LVGL glyph; NULL when nothing fits. Shared with the simplelist
 * hook (list.c) via lvgl_ui.h. */
/* Home-style tiles show the first character of the row label.  Copies the
 * first UTF-8 code point (1..4 bytes) of s into out. */
void lvgui_first_char(const char *s, char *out, size_t sz)
{
    out[0] = '\0';
    if (!s || !*s || sz < 2)
        return;
    unsigned char c = (unsigned char)s[0];
    int n = 1;
    if (c >= 0xf0)
        n = 4;
    else if (c >= 0xe0)
        n = 3;
    else if (c >= 0xc0)
        n = 2;
    for (int k = 1; k < n; k++)       /* truncated sequence (end of string) */
        if (!s[k])
        {
            n = k;
            break;
        }
    if ((size_t)n >= sz)
        n = (int)sz - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

/* Maps a stock icon id (or a private negative pseudo id) to a glyph and a
 * home-palette colour index (icon_grad[] in ui_shell.c: 0 amber, 1 pink,
 * 2 teal, 3 blue, 4 green, 5 aqua) so list icons match the home rows. */
const char *lvgui_menu_icon(int icon_id, int *grad)
{
    if (grad)
        *grad = 4;                       /* default: settings green */
#define GI(g) do { if (grad) *grad = (g); } while (0)
    switch (icon_id)
    {
        case Icon_Audio:            GI(0); return LV_SYMBOL_AUDIO;
        case Icon_Folder:           GI(2); return LV_SYMBOL_DRIVE;
        case Icon_Playlist:         GI(1); return LV_SYMBOL_LIST;
        case Icon_Submenu:          GI(3); return LV_SYMBOL_LIST;
        case Icon_Submenu_Entered:  GI(1); return LV_SYMBOL_LIST;
        case Icon_Wps:              GI(5); return LV_SYMBOL_IMAGE;
        case Icon_Firmware:         GI(0); return LV_SYMBOL_REFRESH;
        case Icon_Font:             GI(2); return LV_SYMBOL_FILE;
        case Icon_Language:         GI(2); return LV_SYMBOL_FILE;
        case Icon_Config:           GI(4); return LV_SYMBOL_FILE;
        case Icon_Plugin:           GI(3); return LV_SYMBOL_DRIVE;
        case Icon_Bookmark:         GI(1); return LV_SYMBOL_LIST;
        case Icon_Preset:           GI(0); return LV_SYMBOL_AUDIO;
        case Icon_Queued:           GI(1); return LV_SYMBOL_LIST;
        case Icon_Moving:           GI(1); return LV_SYMBOL_NEXT;
        case Icon_Keyboard:         GI(4); return LV_SYMBOL_SETTINGS;
        case Icon_Menu_setting:     GI(4); return LV_SYMBOL_SETTINGS;
        case Icon_General_settings_menu: GI(4); return LV_SYMBOL_SETTINGS;
        case Icon_Menu_functioncall: GI(5); return LV_SYMBOL_PLAY;
        case Icon_System_menu:      GI(3); return LV_SYMBOL_SETTINGS;
        case Icon_Playback_menu:    GI(4); return LV_SYMBOL_PLAY;
        case Icon_Display_menu:     GI(5); return LV_SYMBOL_IMAGE;
        case Icon_Remote_Display_menu: GI(5); return LV_SYMBOL_IMAGE;
        case Icon_Radio_screen:     GI(0); return LV_SYMBOL_AUDIO;
        case Icon_Voice:            GI(1); return LV_SYMBOL_VOLUME_MAX;
        case Icon_Recording:        GI(2); return LV_SYMBOL_VIDEO;
        case Icon_file_view_menu:   GI(2); return LV_SYMBOL_DRIVE;
        case Icon_EQ:               GI(0); return LV_SYMBOL_AUDIO;
        case Icon_Rockbox:          GI(4); return LV_SYMBOL_HOME;
        default:                    break;
    }
#undef GI
#define GI(g) do { if (grad) *grad = (g); } while (0)
    switch (-icon_id)                 /* private pseudo ids */
    {
        case 1:  GI(4); return LV_SYMBOL_POWER;      /* on / off            */
        case 2:  GI(2); return LV_SYMBOL_WIFI;       /* wifi / web          */
        case 3:  GI(4); return LV_SYMBOL_BLUETOOTH;  /* bluetooth           */
        case 4:  GI(2); return LV_SYMBOL_REFRESH;    /* scan                */
        case 5:  GI(5); return LV_SYMBOL_EYE_OPEN;   /* status / info       */
        case 6:  GI(1); return LV_SYMBOL_TRASH;      /* reset / forget      */
        case 7:  GI(3); return LV_SYMBOL_KEYBOARD;   /* ssh terminal        */
        case 8:  GI(0); return LV_SYMBOL_AUDIO;      /* radio / stream      */
        case 9:  GI(1); return LV_SYMBOL_CLOSE;      /* cancel / disconnect */
        case 10: GI(4); return LV_SYMBOL_PLAY;       /* connect             */
        case 11: GI(5); return LV_SYMBOL_UPLOAD;     /* dlna push           */
        case 12: GI(1); return LV_SYMBOL_LIST;       /* bookmark row        */
        default: return NULL;
    }
#undef GI
}

static lv_obj_t *menu_build(const struct menu_item_ex *menu, const char *title,
                            int *vis, int *nvis, int selected)
{
    g_menu_row = -1;
    g_menu_cancel = false;

    int count = MENU_GET_COUNT(menu->flags);
    if (count > MAX_MENU_ITEMS)
        count = MAX_MENU_ITEMS;
    *nvis = 0;
    for (int i = 0; i < count; i++)
        if (menu_item_visible(menu, i))
            vis[(*nvis)++] = i;

    lv_obj_t *list;
    lv_obj_t *scr;
    if (g_menu_scr && lv_scr_act() == g_menu_scr)
        shell_menu_screen_rebuild(g_menu_scr, title, &list);
    else
    {
        scr = shell_menu_screen_push(title, &list, menu_close_cb);
        g_menu_scr = scr;
    }

    lv_obj_t *selrow = NULL;
    int n = *nvis;
    for (int r = 0; r < n; r++)
    {
        int i = vis[r];
        char lbuf[128];
        const char *lab = menu_item_label(menu, i, lbuf, sizeof lbuf);
        bool sel = (i == selected);

        /* home-style tile: first character of the label, tinted by the
         * stock icon class */
        char icb[8];
        int grad = 3;
        bool chev = false;
        icb[0] = '\0';
        if ((menu->flags & MENU_TYPE_MASK) != MT_RETURN_ID)
        {
            const struct menu_item_ex *child = menu->submenus[i];
            if (child->flags & MENU_HAS_DESC)
                lvgui_menu_icon(child->callback_and_desc->icon_id, &grad);
            int ctype = child->flags & MENU_TYPE_MASK;
            chev = (ctype == MT_MENU || ctype == MT_RETURN_ID);
            lvgui_first_char(lab, icb, sizeof icb);
        }

        lv_obj_t *row = shell_menu_row_new(list,
                                           icb[0] ? icb : NULL, false,
                                           lab ? lab : "", chev, false, r,
                                           menu_row_cb,
                                           icb[0] ? grad : -1);
        if (sel)
            selrow = row;
    }
    if (selrow)
        lv_obj_scroll_to_view(selrow, LV_ANIM_OFF);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    return scr;
}

/* ---- native option screen (option_screen() takeover) --------------------
 * Reached from the stock engine: option_screen() (apps/gui/option_select.c)
 * diverts here while we run as a job, so MT_SETTING items keep their full
 * stock behaviour (value<->selection mapping, apply callbacks, config save)
 * and only the list UI is replaced. */
/* live-preview state for the option screen (stock option_screen semantics:
 * every highlight change applies the value; cancel reverts; OK keeps) */
static struct opt_prev
{
    const struct settings_list *setting;
    int vtype;
    void (*function)(int);
    int cur;
} g_optp;

static void opt_preview_cb(int sel, void *ud)
{
    struct opt_prev *o = ud;
    int newval = lvgui_opt_selection_to_val(o->setting, sel);
    bool changed = (newval != o->cur);
    if (o->vtype == F_T_BOOL)
        *(bool *)o->setting->setting = (newval != 0);
    else
        *(int *)o->setting->setting = newval;
    if (o->function && (!(o->setting->flags & F_CB_ONLY_IF_CHANGED) || changed))
        o->function(newval);
    o->cur = newval;
}

bool lvgui_option_screen(const struct settings_list *setting,
                         struct viewport parent[NB_SCREENS], bool use_temp_var,
                         const unsigned char *option_title)
{
    (void)parent;
    (void)use_temp_var;      /* F_TEMPVAR screens only exist in classic UI */
    if (!setting)
        return false;

    int vtype = setting->flags & F_T_MASK;
    if (vtype != F_T_INT && vtype != F_T_UINT && vtype != F_T_BOOL)
        return false;                         /* int/bools only, like stock */

    const char *title = (setting->lang_id == -1)
                        ? setting_get_cfgvals(setting)
                        : P2STR(option_title);

    int oldvalue = (vtype == F_T_BOOL)
                   ? (*(bool *)setting->setting ? 1 : 0)
                   : *(int *)setting->setting;
    int nb_items = 0, selected = 0;
    void (*function)(int) = NULL;
    lvgui_opt_val_to_selection(setting, oldvalue, &nb_items,
                               &selected, &function);
    if (nb_items <= 0)
        return false;
    if (nb_items > 200)
        nb_items = 200;      /* keep the list sane on tiny devices */

    const char **labels = malloc(nb_items * sizeof(char *));
    if (!labels)
        return false;

    char tmp[128];
    for (int i = 0; i < nb_items; i++)
    {
        /* the formatter may return a language string instead of filling
         * tmp (e.g. playback frequency 0 -> "auto"), so keep its pointer */
        const char *txt = option_get_valuestring(setting, tmp, sizeof tmp,
                               lvgui_opt_selection_to_val(setting, i));
        labels[i] = strdup(txt ? txt : "");
    }

    g_optp.setting = setting;
    g_optp.vtype = vtype;
    g_optp.function = function;
    g_optp.cur = oldvalue;
    int sel = shell_list_pick_live(title, labels, nb_items, selected,
                                   opt_preview_cb, &g_optp);

    for (int i = 0; i < nb_items; i++)
        free((void *)labels[i]);
    free(labels);

    if (sel < 0 || sel >= nb_items)
    {
        /* cancelled: stock option_screen restores the starting value */
        if (g_optp.cur != oldvalue)
        {
            if (vtype == F_T_BOOL)
                *(bool *)setting->setting = (oldvalue != 0);
            else
                *(int *)setting->setting = oldvalue;
            if (function)
                function(oldvalue);
        }
        return false;
    }

    if (g_optp.cur != oldvalue)
        settings_save();                   /* preview already applied it */
    return false;
}

/* ---- native file picker for the theme/lang/cfg folders -------------------
 * Replaces the classic browser inside browse_folder(); selection runs the
 * stock tree "enter" logic via lvgui_ft_enter_path(). Listing, filtering
 * and sorting come from ft_load() (stock file-view settings honored). */
int lvgui_ft_enter_path(const char *path);

static bool pick_ext_match(int dirfilter, int attr)
{
    switch (dirfilter)
    {
        case SHOW_WPS:  return attr & FILE_ATTR_WPS;
        case SHOW_FONT: return attr & FILE_ATTR_FONT;
        case SHOW_SBS:  return attr & FILE_ATTR_SBS;
        case SHOW_CFG:  return attr & FILE_ATTR_CFG;
        case SHOW_LNG:  return attr & FILE_ATTR_LNG;
#if CONFIG_TUNER
        case SHOW_FMS:  return attr & FILE_ATTR_FMS;
#endif
        case SHOW_M3U:  return attr & FILE_ATTR_M3U;
        default:        return true;
    }
}

int lvgui_browse_pick(const struct browse_context *browse)
{
    static char cur[MAX_PATH];          /* jobs run one at a time */
    strlcpy(cur, browse->root ? browse->root : "/", sizeof cur);

    for (;;)
    {
        char **names = NULL, **paths = NULL, **icons = NULL;
        int n = be_dir_scan_filter(cur, browse->dirfilter,
                                   &names, &paths, &icons);
        if (n < 0)
            return GO_TO_PREVIOUS;

        const char *labels[256];
        static int kept[256];
        int k = 0, sel0 = -1;
        labels[0] = "..";
        for (int i = 0; i < n && k < 254; i++)
        {
            bool isdir = icons[i][0] == 'd';
            if (!isdir && !pick_ext_match(browse->dirfilter,
                                          filetype_get_attr(paths[i])))
                continue;
            labels[k + 1] = names[i];
            kept[k] = i;
            if (sel0 < 0 && browse->selected &&
                !strcasecmp(names[i], browse->selected))
                sel0 = k + 1;
            k++;
        }

        const char *title = browse->title ? browse->title : cur;
        int sel = shell_list_pick(title, labels, k + 1, sel0);

        char picked[MAX_PATH];
        bool picked_isdir = false;
        if (sel > 0 && sel <= k)
        {
            strlcpy(picked, paths[kept[sel - 1]], sizeof picked);
            picked_isdir = icons[kept[sel - 1]][0] == 'd';
        }
        for (int i = 0; i < n; i++)
        {
            free(names[i]);
            free(paths[i]);
            free(icons[i]);
        }
        free(names);
        free(paths);
        free(icons);

        if (sel < 0 || sel > k)
            return GO_TO_PREVIOUS;
        if (sel == 0)                        /* "..": up, exit at root */
        {
            if (!strcasecmp(cur, browse->root))
                return GO_TO_PREVIOUS;
            char *slash = strrchr(cur, '/');
            if (slash == cur)
                cur[1] = '\0';
            else if (slash)
                *slash = '\0';
            continue;
        }
        if (picked_isdir)
        {
            strlcpy(cur, picked, sizeof cur);
            continue;
        }

        lvgui_ft_enter_path(picked);         /* stock apply + splashes */
        return GO_TO_PREVIOUS;
    }
}

/* ---- the engine -------------------------------------------------------- */
int lvgui_do_menu(const struct menu_item_ex *start_menu, int *start_selected)
{
    const struct menu_item_ex *menu = start_menu;
    int selected = start_selected ? *start_selected : 0;
    int ret = 0;
    bool done = false;

    struct { const struct menu_item_ex *menu; int selected; } stk[MAX_MENUS];
    int top = 0;
    int vis[MAX_MENU_ITEMS];

    if (!start_menu)
        return GO_TO_PREVIOUS;

    while (!done)
    {
        if (g_menu_abort)
        {
            g_menu_abort = false;
            g_menu_scr = NULL;
            ret = GO_TO_PREVIOUS;
            break;
        }
        char tbuf[80];
        const char *title = menu_title(menu, tbuf, sizeof tbuf);
        int nvis = 0;
        menu_build(menu, title, vis, &nvis, selected);

        while (g_menu_row < 0 && !g_menu_cancel)
            lvgui_pump();

        int row = g_menu_row;
        bool cancel = g_menu_cancel;
        int orig = (row >= 0 && row < nvis) ? vis[row] : -1;

        if (cancel)
        {
            /* the edge-swipe gesture already popped our screen */
            g_menu_scr = NULL;
        }
        /* else: KEEP our screen on the stack — the item action pushes its
         * successor on top of us, so backing out reveals this menu again
         * instead of flashing the parent (home) for one frame. */

        if (cancel || orig < 0)
        {
            /* ACTION_STD_CANCEL */
            menu_callback_type cbm = menu_get_cb(menu);
            cbm(ACTION_EXIT_MENUITEM, menu, NULL);
            if (menu->flags & MENU_EXITAFTERTHISMENU)
                done = true;
            else if (top > 0)
            {
                top--;
                menu = stk[top].menu;
                selected = stk[top].selected;
            }
            else
            {
                ret = GO_TO_PREVIOUS;
                done = true;
            }
            continue;
        }

        if ((menu->flags & MENU_TYPE_MASK) == MT_RETURN_ID)
        {
            ret = orig;                  /* string list: index is the result */
            done = true;
            continue;
        }

        const struct menu_item_ex *item = menu->submenus[orig];
        int itype = item->flags & MENU_TYPE_MASK;

        menu_callback_type cb = menu_get_cb(item);
        if (cb(ACTION_ENTER_MENUITEM, item, NULL) == ACTION_EXIT_MENUITEM)
            continue;                    /* item aborted entry */

        switch (itype)
        {
        case MT_MENU:
            if (top < MAX_MENUS)
            {
                stk[top].menu = menu;
                stk[top].selected = orig;
                top++;
                menu = item;
                selected = 0;
            }
            break;

        case MT_RETURN_ID:
            if (top < MAX_MENUS)
            {
                stk[top].menu = menu;
                stk[top].selected = orig;
                top++;
                menu = item;
                selected = 0;
            }
            break;

        case MT_SETTING:
        case MT_SETTING_W_TEXT:
            /* stock engine renders the setting screen; its option_screen()
             * hook keeps the UI native while we run as a job */
            do_setting_from_menu(item, NULL);
            cb(ACTION_EXIT_MENUITEM, item, NULL);
            break;

        case MT_RETURN_VALUE:
            ret = item->value;
            done = true;
            break;

        case MT_FUNCTION_CALL_W_PARAM:
        case MT_FUNCTION_CALL:
        {
            int rv;
            if (itype == MT_FUNCTION_CALL_W_PARAM)
                rv = item->function_param->function_w_param(
                                                    item->function_param->param);
            else
                rv = item->function->function();
            cb(ACTION_EXIT_MENUITEM, item, NULL);

            if ((item->flags & MENU_FUNC_CHECK_RETVAL) && rv != 0)
            {
                ret = rv;
                done = true;
            }
            else if ((menu->flags & MENU_EXITAFTERTHISMENU) &&
                     !(item->flags & MENU_EXITAFTERTHISMENU))
                done = true;
            break;
        }

        default:
            break;
        }
    }

    if (g_menu_scr)
    {
        if (lv_scr_act() == g_menu_scr)
            shell_nav_pop_now();
        g_menu_scr = NULL;
    }

    if (start_selected)
        *start_selected = selected;
    return ret;
}

/* ---- native colour picker (apps/gui/color_picker.c set_color) ---------- */
bool lvgui_set_color(char *title, unsigned *color, unsigned banned)
{
    static const uint32_t palette[] = {
        0x000000, 0x404040, 0x808080, 0xc0c0c0, 0xffffff,
        0x800000, 0xff0000, 0xff8080, 0xffc0c0,
        0x804000, 0xff8000, 0xffbf00, 0xffe080,
        0x808000, 0xffff00, 0xffff80, 0xf0f0a0,
        0x008000, 0x00ff00, 0x80ff80, 0x008040, 0x00c85a,
        0x008080, 0x00ffff, 0x80ffff,
        0x000080, 0x0000ff, 0x8080ff, 0x4080ff, 0x80a0ff,
        0x400080, 0x8000ff, 0xc0a0ff,
        0x800080, 0xff00ff, 0xff80ff, 0x800040, 0xff4080,
        0x663300, 0xa0522d, 0xd2a679,
    };
    const int ncol = (int)sizeof(palette) / sizeof(palette[0]);
    const int perrow = 7;
    int c_pick;

    c_pick = -1;
    g_menu_row = -1;
    g_menu_cancel = false;

    lv_obj_t *scr = lv_layer_top();
    lv_obj_t *scrim = lv_obj_create(scr);
    lv_obj_set_size(scrim, 360, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scrim, th->scrim, 0);
    lv_obj_set_style_bg_opa(scrim, th->scrim_opa, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_add_event_cb(scrim, menu_scrim_cb, LV_EVENT_CLICKED, NULL);
    /* reuse the menu-cancel flag: a tap outside the card cancels */
    g_menu_cancel = false;

    lv_obj_t *card = lv_obj_create(scrim);
    lv_obj_set_size(card, 348, 366);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, th->menu_bg, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, th->radius_big, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title ? title : "");
    lv_obj_set_style_text_color(t, th->fg_text, 0);
    lv_obj_set_style_text_font(t, th->font_cjk, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 2);

    int rows = (ncol + perrow - 1) / perrow;
    for (int i = 0; i < ncol; i++)
    {
        if (banned != (unsigned)-1 && palette[i] == banned)
            continue;
        lv_obj_t *sw = lv_obj_create(card);
        lv_obj_set_size(sw, 40, 40);
        lv_obj_set_pos(sw, 4 + (i % perrow) * 47, 34 + (i / perrow) * 47);
        lv_obj_set_style_bg_color(sw, lv_color_hex(palette[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sw, 10, 0);
        lv_obj_set_style_border_width(sw, 2, 0);
        lv_obj_set_style_border_color(sw,
            (palette[i] == *color) ? th->accent : th->border_white, 0);
        lv_obj_set_style_border_opa(sw,
            (palette[i] == *color) ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sw, menu_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    while (!g_menu_cancel && g_menu_row < 0)
        lvgui_pump();

    lv_obj_del(scrim);
    lvgui_pump();

    c_pick = g_menu_row;
    if (c_pick < 0)
        return false;               /* cancelled: colour unchanged */
    *color = (unsigned)palette[c_pick];
    return false;                   /* true would mean USB connected */
}

/* ---- native touchscreen calibration (apps/screens.c calibrate) --------- */
static int  g_cal_hit;
static int  g_cal_x, g_cal_y;
static bool g_cal_cancel;

static void cal_point_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *ind = lv_indev_get_act();
    lv_point_t p;
    if (!ind)
        return;
    lv_indev_get_point(ind, &p);
    g_cal_x = p.x;
    g_cal_y = p.y;
    g_cal_hit = 1;
}

static void cal_cancel_cb(lv_event_t *e)
{
    (void)e;
    g_cal_cancel = true;
}

/* Fills *cal with 3 raw-vs-target sample pairs, same targets as the
 * classic get_sample() loop.  Returns 0 ok, -1 cancelled. */
int lvgui_calibrate_sample(struct touchscreen_calibration *cal,
                           const short points[3][2])
{
    lv_obj_t *scr = lv_layer_top();
    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, 360, 480);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_add_event_cb(ov, cal_point_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *hint = lv_label_create(ov);
    lv_label_set_text(hint, "请依次点按十字标记");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x909090), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *cancel = lv_label_create(ov);
    lv_label_set_text(cancel, str(LANG_CANCEL));
    lv_obj_set_style_text_color(cancel, lv_color_hex(0x909090), 0);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel, cal_cancel_cb, LV_EVENT_CLICKED, NULL);

    int ret = 0;
    for (int i = 0; i < 3 && ret == 0; i++)
    {
        lv_obj_t *cross = lv_label_create(ov);
        lv_label_set_text(cross, "+");
        lv_obj_set_style_text_color(cross, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(cross, th->font_big, 0);
        lv_obj_set_pos(cross, points[i][0] - 14, points[i][1] - 17);
        lv_obj_update_layout(cross);

        g_cal_hit = 0;
        g_cal_cancel = false;
        while (!g_cal_hit && !g_cal_cancel)
            lvgui_pump();

        lv_obj_del(cross);
        if (g_cal_cancel)
        {
            ret = -1;
            break;
        }
        cal->x[i][0] = g_cal_x;
        cal->y[i][0] = g_cal_y;
        cal->x[i][1] = points[i][0];
        cal->y[i][1] = points[i][1];
    }

    lv_obj_del(ov);
    lvgui_pump();
    return ret;
}

/* ---- native graphical EQ (eq_menu_graphical stand-in) ------------------- */
/* Layout follows the HiBy FIR EQ screen: response curve pinned to the top,
 * ten vertical band sliders in a horizontally scrolling row, and the
 * selected band's cutoff/Q steppers in a fixed bottom row. Colours come
 * from our own light theme. */
static bool g_eq_done;
static int g_eq_band;
static lv_obj_t *g_eq_line;
static lv_point_t g_eq_pts[81];
static lv_obj_t *g_eq_sliders[EQ_NUM_BANDS];
static lv_obj_t *g_eq_vals[EQ_NUM_BANDS];
static lv_obj_t *g_eq_freq[EQ_NUM_BANDS];
static lv_obj_t *g_eq_fc_val, *g_eq_q_val, *g_eq_type_val;
static lv_obj_t *g_eq_pg_l, *g_eq_pg_r;
static lv_obj_t *g_eq_row;

/* horizontal pager for the band columns (drag also works) */
static void eq_sel_refresh(void);

/* prev/next band: select it and centre that column in the viewport */
static void eq_scroll_cb(lv_event_t *e)
{
    long d = (long)(intptr_t)lv_event_get_user_data(e);
    int nb = g_eq_band + (int)d;
    if (nb < 0)
        nb = 0;
    if (nb > EQ_NUM_BANDS - 1)
        nb = EQ_NUM_BANDS - 1;
    if (nb == g_eq_band)
        return;
    g_eq_band = nb;
    eq_sel_refresh();
    if (g_eq_row)
    {
        /* minimal scroll: only move when the selected column is outside
         * the viewport; never centre it unnecessarily */
        lv_coord_t vw = lv_obj_get_width(g_eq_row);
        if (vw <= 0 || vw > 360)
            vw = 360;
        lv_coord_t colx = 4 + nb * 68;      /* column left in content */
        lv_coord_t sx = lv_obj_get_scroll_x(g_eq_row);
        if (colx < sx)
            sx = colx;
        else if (colx + 68 > sx + vw)
            sx = colx + 68 - vw;
        if (sx < 0)
            sx = 0;
        lv_obj_scroll_to_x(g_eq_row, sx, LV_ANIM_ON);
    }
}

static void eq_scrim_cb(lv_event_t *e)
{
    (void)e;
    g_eq_done = true;
}

static const char *eq_fmt_freq(int hz, char *buf, size_t sz)
{
    if (hz >= 1000)
        snprintf(buf, sz, "%.1fk", hz / 1000.0);
    else
        snprintf(buf, sz, "%d", hz);
    return buf;
}

static const char *eq_type_name(int t);

static const char *eq_type_name_short(int t)
{
    static char buf[12];
    const char *full = eq_type_name(t);
    int n = 0, i = 0;

    while (full[i] && n < 2)                 /* first two UTF-8 chars */
    {
        int len = (full[i] & 0x80) == 0 ? 1 :
                  (full[i] & 0xE0) == 0xC0 ? 2 :
                  (full[i] & 0xF0) == 0xE0 ? 3 : 4;
        for (int k = 0; k < len; k++)
            buf[i + k] = full[i + k];
        i += len;
        n++;
    }
    buf[i] = '\0';
    return buf;
}

static const char *eq_type_name(int t)
{
    /* stock wording follows the language file:
     * 低架滤波器 / 带通滤波器 / 高架滤波器 */
    return t == EQ_FILTER_LOW_SHELF  ? str(LANG_EQUALIZER_BAND_LOW_SHELF) :
           t == EQ_FILTER_HIGH_SHELF ? str(LANG_EQUALIZER_BAND_HIGH_SHELF) :
                                       str(LANG_EQUALIZER_BAND_PEAK);
}

static double eq_band_db(const struct eq_band_setting *b, double f)
{
    double sr = 44100.0;
    double w0 = 2.0 * M_PI * b->cutoff / sr;
    double A = pow(10.0, b->gain / 400.0);
    double q = b->q / 10.0;
    double alpha, a0, a1, a2, b0, b1, b2, sq;

    if (b->type == EQ_FILTER_LOW_SHELF)
    {
        alpha = sin(w0) * 0.7071;
        sq = sqrt(A);
        b0 = A * ((A + 1) - (A - 1) * cos(w0) + 2 * sq * alpha);
        b1 = 2 * A * ((A - 1) - (A + 1) * cos(w0));
        b2 = A * ((A + 1) - (A - 1) * cos(w0) - 2 * sq * alpha);
        a0 = (A + 1) + (A - 1) * cos(w0) + 2 * sq * alpha;
        a1 = -2 * ((A - 1) + (A + 1) * cos(w0));
        a2 = (A + 1) + (A - 1) * cos(w0) - 2 * sq * alpha;
    }
    else if (b->type == EQ_FILTER_HIGH_SHELF)   /* high shelf */
    {
        alpha = sin(w0) * 0.7071;
        sq = sqrt(A);
        b0 = A * ((A + 1) + (A - 1) * cos(w0) + 2 * sq * alpha);
        b1 = -2 * A * ((A - 1) + (A + 1) * cos(w0));
        b2 = A * ((A + 1) + (A - 1) * cos(w0) - 2 * sq * alpha);
        a0 = (A + 1) - (A - 1) * cos(w0) + 2 * sq * alpha;
        a1 = 2 * ((A - 1) - (A + 1) * cos(w0));
        a2 = (A + 1) - (A - 1) * cos(w0) - 2 * sq * alpha;
    }
    else                                /* peaking */
    {
        alpha = sin(w0) / (2 * q);
        b0 = 1 + alpha * A;
        b1 = -2 * cos(w0);
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * cos(w0);
        a2 = 1 - alpha / A;
    }
    {
        double w = 2.0 * M_PI * f / sr;
        double c1 = cos(w), c2 = cos(2 * w), s1 = sin(w), s2 = sin(2 * w);
        double nb0 = b0 / a0, nb1 = b1 / a0, nb2 = b2 / a0;
        double na1 = a1 / a0, na2 = a2 / a0;
        /* |H(e^jw)| = |N| / |D| for
         * H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2) */
        double nre = nb0 + nb1 * c1 + nb2 * c2;
        double nim = -(nb1 * s1 + nb2 * s2);
        double dre = 1.0 + na1 * c1 + na2 * c2;
        double dim = -(na1 * s1 + na2 * s2);
        double nmag = sqrt(nre * nre + nim * nim);
        double dmag = sqrt(dre * dre + dim * dim);
        return 20.0 * log10((nmag / (dmag > 1e-12 ? dmag : 1e-12)) + 1e-12);
    }
}

static void eq_curve_redraw(void)
{
    for (int p = 0; p < 81; p++)
    {
        double f = 20.0 * pow(22040.0 / 20.0, p / 80.0);
        double db = 0;
        for (int i = 0; i < EQ_NUM_BANDS; i++)
            db += eq_band_db(&global_settings.eq_band_settings[i], f);
        if (db > 12)
            db = 12;
        if (db < -12)
            db = -12;
        g_eq_pts[p].x = 36 + p * 4;
        g_eq_pts[p].y = 144 - (int)(db * 8);
    }
    lv_line_set_points(g_eq_line, g_eq_pts, 81);
    lv_obj_invalidate(g_eq_line);
    lv_refr_now(NULL);          /* partial flushes don't always reach the panel */
}

static void eq_write_apply(void)
{
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        global_settings.eq_band_settings[i].gain =
            lv_slider_get_value(g_eq_sliders[i]);
        be_eq_apply_band(i);               /* stock: per-band coef push */
    }
    settings_save();                       /* one save on 完成 */
}

static void eq_sel_refresh(void)
{
    const struct int_setting *qs = be_eq_q_setting();
    const struct int_setting *cs = be_eq_cutoff_setting();
    char b[24];
    int i = g_eq_band;

    if (g_eq_fc_val)
    {
        eq_fmt_freq(global_settings.eq_band_settings[i].cutoff,
                    b, sizeof b);
        lv_label_set_text(g_eq_fc_val, b);
    }
    if (g_eq_q_val && qs->formatter)
    {
        qs->formatter(b, sizeof b, global_settings.eq_band_settings[i].q,
                      "Q");
        lv_label_set_text(g_eq_q_val, b);
    }
    if (g_eq_type_val)
        lv_label_set_text(g_eq_type_val,
                          eq_type_name_short(global_settings.eq_band_settings[i].type));
    (void)cs;
    for (int j = 0; j < EQ_NUM_BANDS; j++)
    {
        bool sel = (j == i);
        lv_obj_set_style_bg_color(g_eq_sliders[j],
                                  sel ? th->accent : th->accent_blue,
                                  LV_PART_INDICATOR);
        if (g_eq_freq[j])
        {
            lv_obj_set_style_text_color(g_eq_freq[j],
                                        sel ? th->accent : th->fg_text, 0);
            lv_obj_set_style_bg_opa(g_eq_freq[j],
                                    sel ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        }
        if (g_eq_vals[j])
            lv_obj_set_style_text_color(g_eq_vals[j], th->fg_text, 0);
    }
}

static void eq_sel_refresh(void);

static void eq_gain_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    bool press = (lv_event_get_code(e) == LV_EVENT_PRESSED);

    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        if (g_eq_sliders[i] != sl)
            continue;
        if (g_eq_band != i)
        {
            g_eq_band = i;
            eq_sel_refresh();
        }
        if (press)
            break;
        const struct int_setting *is = be_eq_gain_setting();
        static char bufs[EQ_NUM_BANDS][32];
        int v = lv_slider_get_value(sl);
        /* live-write so the curve follows the drag in real time;
         * stock graphical screen pushes the band's coefficients
         * on every change */
        global_settings.eq_band_settings[i].gain = v;
        be_eq_apply_band(i);
        if (is->formatter)
            is->formatter(bufs[i], sizeof bufs[i], v, "dB");
        else
            snprintf(bufs[i], sizeof bufs[i], "%d", v);
        lv_label_set_text(g_eq_vals[i], bufs[i]);
        eq_curve_redraw();
        break;
    }
}

static void eq_col_cb(lv_event_t *e)
{
    g_eq_band = (int)(long)lv_event_get_user_data(e);
    eq_sel_refresh();
}

static void eq_fc_step_cb(lv_event_t *e)
{
    static const int fc_min = EQ_CUTOFF_MIN, fc_max = EQ_CUTOFF_MAX;
    int dir = (intptr_t)lv_event_get_user_data(e) ? 100 : -100;
    struct eq_band_setting *b = &global_settings.eq_band_settings[g_eq_band];
    char buf[24];

    b->cutoff += dir;
    if (b->cutoff < fc_min)
        b->cutoff = fc_min;
    if (b->cutoff > fc_max)
        b->cutoff = fc_max;
    eq_fmt_freq(b->cutoff, buf, sizeof buf);
    lv_label_set_text(g_eq_fc_val, buf);
    eq_curve_redraw();
    be_eq_apply_band(g_eq_band);           /* stock: band coef push only */
}

static void eq_q_step_cb(lv_event_t *e)
{
    int dir = (intptr_t)lv_event_get_user_data(e) ? EQ_Q_STEP : -EQ_Q_STEP;
    struct eq_band_setting *b = &global_settings.eq_band_settings[g_eq_band];
    const struct int_setting *qs = be_eq_q_setting();
    char buf[24];

    b->q += dir;
    if (b->q < EQ_Q_MIN)
        b->q = EQ_Q_MIN;
    if (b->q > EQ_Q_MAX)
        b->q = EQ_Q_MAX;
    if (qs->formatter)
    {
        qs->formatter(buf, sizeof buf, b->q, "Q");
        lv_label_set_text(g_eq_q_val, buf);
    }
    eq_curve_redraw();
    be_eq_apply_band(g_eq_band);           /* stock: band coef push only */
}

/* Non-blocking bottom sheet drawn inside the EQ screen: the editor's own
 * pump loop handles its events (a nested pump would dead-lock). */
static lv_obj_t *g_eq_sheet;

static void eq_sheet_close(void)
{
    if (g_eq_sheet)
    {
        lv_obj_del(g_eq_sheet);
        g_eq_sheet = NULL;
        lv_obj_invalidate(lv_layer_top());
        lv_refr_now(NULL);
    }
}

static void eq_sheet_scrim_cb(lv_event_t *e)
{
    (void)e;
    eq_sheet_close();
}

static void eq_sheet_row_cb(lv_event_t *e)
{
    int sel = (int)(intptr_t)lv_event_get_user_data(e);
    struct eq_band_setting *b = &global_settings.eq_band_settings[g_eq_band];

    b->type = sel == 0 ? EQ_FILTER_LOW_SHELF :
              sel == 2 ? EQ_FILTER_HIGH_SHELF : EQ_FILTER_PEAK;
    lv_label_set_text(g_eq_type_val, eq_type_name_short(b->type));
    eq_sheet_close();
    eq_curve_redraw();
    be_eq_apply_band(g_eq_band);           /* stock: band coef push only */
}

static void eq_type_open_cb(lv_event_t *e)
{
    static const char *names[3];
    lv_obj_t *panel = lv_obj_get_parent(g_eq_type_val);
    struct eq_band_setting *b = &global_settings.eq_band_settings[g_eq_band];
    int cur = b->type == EQ_FILTER_LOW_SHELF ? 0 :
              b->type == EQ_FILTER_HIGH_SHELF ? 2 : 1;

    (void)e;
    if (g_eq_sheet)
        return;
    names[0] = str(LANG_EQUALIZER_BAND_LOW_SHELF);
    names[1] = str(LANG_EQUALIZER_BAND_PEAK);
    names[2] = str(LANG_EQUALIZER_BAND_HIGH_SHELF);

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
    lv_obj_add_event_cb(scrim, eq_sheet_scrim_cb, LV_EVENT_CLICKED, NULL);
    g_eq_sheet = scrim;

    const int SH = 54 + 3 * 52 + 56;
    lv_obj_t *sh = lv_obj_create(scrim);
    lv_obj_set_size(sh, 360, SH);
    lv_obj_set_pos(sh, 0, 480 - SH);
    lv_obj_set_style_radius(sh, th->radius_big, 0);
    lv_obj_set_style_bg_color(sh, th->glass_bg, 0);
    lv_obj_set_style_bg_grad_color(sh, lv_color_darken(th->glass_bg, 16), 0);
    lv_obj_set_style_bg_grad_dir(sh, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sh, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sh, th->border_white, 0);
    lv_obj_set_style_border_opa(sh, th->card_border_opa, 0);
    lv_obj_set_style_border_width(sh, 1, 0);
    lv_obj_set_style_pad_all(sh, 0, 0);
    lv_obj_set_style_shadow_width(sh, 0, 0);
    lv_obj_clear_flag(sh, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sh, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(sh);
    lv_label_set_text(t, "类型");
    lv_obj_set_style_text_font(t, th->font_title, 0);
    lv_obj_set_style_text_color(t, th->fg_text, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

    for (int i = 0; i < 3; i++)
    {
        bool sel = (i == cur);
        lv_obj_t *row = lv_btn_create(sh);
        lv_obj_set_size(row, 336, 44);
        lv_obj_set_pos(row, 12, 52 + i * 52);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_color(row, sel ? th->accent_blue : th->glass_bg, 0);
        lv_obj_set_style_bg_opa(row, sel ? LV_OPA_COVER : th->menu_row_opa, 0);
        lv_obj_set_style_border_color(row, th->border_white, 0);
        lv_obj_set_style_border_opa(row, th->card_border_opa, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_left(row, 16, 0);
        lv_obj_add_event_cb(row, eq_sheet_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_font(l, th->font_cjk, 0);
        lv_obj_set_style_text_color(l, sel ? th->fg_inverse : th->fg_text, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        if (sel)
        {
            lv_obj_t *ck = lv_label_create(row);
            lv_label_set_text(ck, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(ck, th->font_symbol, 0);
            lv_obj_set_style_text_color(ck, th->fg_inverse, 0);
            lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -14, 0);
        }
    }

    lv_obj_t *cx = lv_btn_create(sh);
    lv_obj_set_size(cx, 336, 44);
    lv_obj_set_pos(cx, 12, SH - 52);
    lv_obj_set_style_radius(cx, 12, 0);
    lv_obj_set_style_bg_color(cx, th->btn_secondary, 0);
    lv_obj_set_style_bg_opa(cx, th->btn_secondary_opa, 0);
    lv_obj_set_style_border_width(cx, 0, 0);
    lv_obj_set_style_shadow_width(cx, 0, 0);
    lv_obj_add_event_cb(cx, eq_sheet_scrim_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cx);
    lv_label_set_text(cl, str(LANG_CANCEL));
    lv_obj_set_style_text_font(cl, th->font_cjk, 0);
    lv_obj_set_style_text_color(cl, th->fg_text, 0);
    lv_obj_center(cl);

    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);
}

static lv_obj_t *eq_btn_new(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *txt, bool primary, bool sym,
                            lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, th->accent_blue, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, sym ? h / 2 : 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, sym ? th->font_symbol : th->font_cjk, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

/* ---- Bluetooth menu as a native do_menu tree -------------------------- */
static int bt_open_output(void) { return bt_lvgl_output(); }
static int bt_open_input(void)  { return bt_lvgl_input(); }
static int bt_do_reset(void)    { return bt_lvgl_reset(); }

MENUITEM_FUNCTION(bt_out_item, 0, ID2P(LANG_BT_AUDIO_OUT),
                  bt_open_output, NULL, Icon_Submenu);
MENUITEM_FUNCTION(bt_in_item, 0, ID2P(LANG_BT_AUDIO_IN),
                  bt_open_input, NULL, Icon_Submenu);
MENUITEM_FUNCTION(bt_rst_item, 0, ID2P(LANG_BT_RESET),
                  bt_do_reset, NULL, Icon_Submenu);

MAKE_MENU(bt_lvgl_menu, ID2P(LANG_BLUETOOTH), NULL, Icon_Submenu,
          &bt_out_item, &bt_in_item, &bt_rst_item);

int lvgui_bt_menu(void)
{
    return do_menu(&bt_lvgl_menu, NULL, NULL, false);
}

bool lvgui_eq_graphical(void)
{
    const struct int_setting *is = be_eq_gain_setting();
    static char vb[EQ_NUM_BANDS][32];
    static char fb[EQ_NUM_BANDS][16];

    g_eq_done = false;
    g_eq_band = 0;

    lv_obj_t *scr = lv_layer_top();
    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_size(panel, 360, 480);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, th->glass_bg, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    /* no click-to-close: only the finish button exits */

    /* header */
    lv_obj_t *t = lv_label_create(panel);
    lv_label_set_text(t, str(LANG_EQUALIZER_GRAPHICAL));
    lv_obj_set_style_text_font(t, th->font_cjk, 0);
    lv_obj_set_style_text_color(t, th->fg_text, 0);
    lv_obj_set_pos(t, 14, 12);
    eq_btn_new(panel, 282, 8, 64, 32, "完成", true, false,
               eq_scrim_cb, NULL);

    /* curve grid: horizontal dB lines + labels, vertical decade lines */
    static const int gyy[5] = {48, 96, 144, 192, 240};
    static const char *gly[5] = {"+12", "+6", "0", "-6", "-12"};
    for (int g = 0; g < 5; g++)
    {
        lv_obj_t *ln = lv_obj_create(panel);
        lv_obj_set_size(ln, 318, 1);
        lv_obj_set_pos(ln, 38, gyy[g]);
        lv_obj_set_style_bg_color(ln, th->fg_text, 0);
        lv_obj_set_style_bg_opa(ln, g == 2 ? LV_OPA_30 : LV_OPA_10, 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *lb = lv_label_create(panel);
        lv_label_set_text(lb, gly[g]);
        lv_obj_set_style_text_font(lb, th->font_symbol, 0);
        lv_obj_set_style_text_color(lb, th->fg_text, 0);
        lv_obj_set_style_text_opa(lb, LV_OPA_50, 0);
        lv_obj_set_pos(lb, 0, gyy[g] - 7);
    }
    static const int gxx[3] = {109, 214, 318};
    static const char *glx[3] = {"100", "1k", "10k"};
    for (int g = 0; g < 3; g++)
    {
        lv_obj_t *ln = lv_obj_create(panel);
        lv_obj_set_size(ln, 1, 192);
        lv_obj_set_pos(ln, gxx[g], 48);
        lv_obj_set_style_bg_color(ln, th->fg_text, 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_10, 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *lb = lv_label_create(panel);
        lv_label_set_text(lb, glx[g]);
        lv_obj_set_style_text_font(lb, th->font_symbol, 0);
        lv_obj_set_style_text_color(lb, th->fg_text, 0);
        lv_obj_set_style_text_opa(lb, LV_OPA_50, 0);
        lv_obj_set_pos(lb, gxx[g] - 8, 244);
    }

    /* response curve */
    g_eq_line = lv_line_create(panel);
    lv_obj_set_style_line_color(g_eq_line, th->accent, 0);
    lv_obj_set_style_line_width(g_eq_line, 3, 0);
    lv_obj_set_style_line_rounded(g_eq_line, true, 0);
    lv_obj_set_pos(g_eq_line, 0, 0);
    eq_curve_redraw();

    /* band slider row: horizontally scrollable, ten columns */
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, 360, 148);
    lv_obj_set_pos(row, 0, 262);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_size(row, 360, 148);              /* viewport */
    /* NOTE: do NOT call lv_obj_set_content_width() here -- in LVGL 8 it
     * resizes the object itself (content + pad), which made the row 704px
     * wide and killed the scroll range.  The absolutely positioned band
     * columns already extend the scrollable area. */
    g_eq_row = row;
    /* band prev/next: two separate wide bar buttons */
    static const int pgx[2] = { 156, 258 };
    for (int k = 0; k < 2; k++)
    {
        lv_obj_t *b = lv_obj_create(panel);
        lv_obj_set_size(b, 90, 30);
        lv_obj_set_pos(b, pgx[k], 440);
        lv_obj_set_style_radius(b, 15, 0);
        lv_obj_set_style_bg_color(b, th->btn_secondary, 0);
        lv_obj_set_style_bg_opa(b, th->btn_secondary_opa, 0);
        lv_obj_set_style_border_color(b, th->border_white, 0);
        lv_obj_set_style_border_opa(b, th->card_border_opa, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, eq_scroll_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(k ? 1 : -1));
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, k ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(l, th->font_symbol, 0);
        lv_obj_set_style_text_color(l, th->fg_text, 0);
        lv_obj_center(l);
        if (k)
            g_eq_pg_r = b;
        else
            g_eq_pg_l = b;
    }

    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        int x0 = 4 + i * 68;

        eq_fmt_freq(global_settings.eq_band_settings[i].cutoff,
                    fb[i], sizeof fb[i]);
        g_eq_freq[i] = lv_label_create(row);
        lv_label_set_text(g_eq_freq[i], fb[i]);
        lv_obj_set_style_text_font(g_eq_freq[i], th->font_symbol, 0);
        lv_obj_set_style_text_color(g_eq_freq[i], th->fg_text, 0);
        lv_obj_set_width(g_eq_freq[i], 68);
        lv_obj_set_style_text_align(g_eq_freq[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_eq_freq[i], x0, 2);
        lv_obj_set_style_bg_color(g_eq_freq[i], th->accent, 0);
        lv_obj_set_style_bg_opa(g_eq_freq[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(g_eq_freq[i], 8, 0);
        lv_obj_add_flag(g_eq_freq[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(g_eq_freq[i], eq_col_cb, LV_EVENT_CLICKED,
                            (void *)(long)i);

        lv_obj_t *sl = lv_slider_create(row);
        lv_obj_set_size(sl, 24, 92);             /* h > w -> vertical */
        lv_slider_set_range(sl, is->min, is->max);
        lv_slider_set_value(sl, global_settings.eq_band_settings[i].gain,
                            LV_ANIM_OFF);
        lv_obj_set_pos(sl, x0 + 18, 20);
        lv_obj_set_style_bg_color(sl, th->fg_text, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sl, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, th->accent_blue, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_border_color(sl, th->fg_text, LV_PART_KNOB);
        lv_obj_set_style_border_width(sl, 2, LV_PART_KNOB);
        lv_obj_add_event_cb(sl, eq_gain_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(sl, eq_gain_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(sl, eq_gain_cb, LV_EVENT_RELEASED, NULL);
        g_eq_sliders[i] = sl;

        g_eq_vals[i] = lv_label_create(row);
        lv_obj_set_style_text_font(g_eq_vals[i], th->font_symbol, 0);
        lv_obj_set_style_text_color(g_eq_vals[i], th->fg_text, 0);
        lv_obj_set_width(g_eq_vals[i], 68);
        lv_label_set_long_mode(g_eq_vals[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(g_eq_vals[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_eq_vals[i], x0, 112);
        if (is->formatter)
            is->formatter(vb[i], sizeof vb[i],
                          global_settings.eq_band_settings[i].gain, "dB");
        else
            snprintf(vb[i], sizeof vb[i], "%d",
                     global_settings.eq_band_settings[i].gain);
        lv_label_set_text(g_eq_vals[i], vb[i]);
        lv_obj_add_flag(g_eq_vals[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(g_eq_vals[i], eq_col_cb, LV_EVENT_CLICKED,
                            (void *)(long)i);
    }

    /* selected-band steppers: frequency then Q */
    lv_obj_t *c1 = lv_label_create(panel);
    lv_label_set_text(c1, "频率");
    lv_obj_set_style_text_font(c1, th->font_cjk, 0);
    lv_obj_set_style_text_color(c1, th->fg_text, 0);
    lv_obj_set_pos(c1, 14, 406);

    g_eq_fc_val = lv_label_create(panel);
    lv_obj_set_style_text_font(g_eq_fc_val, th->font_symbol, 0);
    lv_obj_set_style_text_color(g_eq_fc_val, th->fg_text, 0);
    lv_obj_set_width(g_eq_fc_val, 64);
    lv_obj_set_style_text_align(g_eq_fc_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g_eq_fc_val, 98, 412);

    lv_obj_t *c2 = lv_label_create(panel);
    lv_label_set_text(c2, "Q");
    lv_obj_set_style_text_font(c2, th->font_symbol, 0);
    lv_obj_set_style_text_color(c2, th->fg_text, 0);
    lv_obj_set_pos(c2, 204, 406);

    g_eq_q_val = lv_label_create(panel);
    lv_obj_set_style_text_font(g_eq_q_val, th->font_symbol, 0);
    lv_obj_set_style_text_color(g_eq_q_val, th->fg_text, 0);
    lv_obj_set_width(g_eq_q_val, 48);
    lv_obj_set_style_text_align(g_eq_q_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g_eq_q_val, 268, 412);

    eq_btn_new(panel, 58, 400, 34, 34, LV_SYMBOL_MINUS, false, true,
               eq_fc_step_cb, NULL);
    eq_btn_new(panel, 166, 400, 34, 34, LV_SYMBOL_PLUS, false, true,
               eq_fc_step_cb, (void *)1);
    eq_btn_new(panel, 224, 400, 34, 34, LV_SYMBOL_MINUS, false, true,
               eq_q_step_cb, NULL);
    eq_btn_new(panel, 320, 400, 34, 34, LV_SYMBOL_PLUS, false, true,
               eq_q_step_cb, (void *)1);

    /* selected-band filter type row: 低架 / 峰值 / 高架 */
    lv_obj_t *c3 = lv_label_create(panel);
    lv_label_set_text(c3, "类型");
    lv_obj_set_style_text_font(c3, th->font_cjk, 0);
    lv_obj_set_style_text_color(c3, th->fg_text, 0);
    lv_obj_set_pos(c3, 14, 444);

    g_eq_type_val = lv_label_create(panel);
    lv_obj_set_style_text_font(g_eq_type_val, th->font_cjk, 0);
    lv_obj_set_style_text_color(g_eq_type_val, th->fg_text, 0);
    lv_obj_set_width(g_eq_type_val, 64);
    lv_obj_set_style_text_align(g_eq_type_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g_eq_type_val, 98, 444);

    lv_obj_set_width(g_eq_type_val, 84);
    lv_obj_set_pos(g_eq_type_val, 58, 442);
    lv_obj_set_style_bg_color(g_eq_type_val, th->accent, 0);
    lv_obj_set_style_bg_opa(g_eq_type_val, LV_OPA_30, 0);
    lv_obj_set_style_radius(g_eq_type_val, 10, 0);
    lv_obj_set_style_pad_ver(g_eq_type_val, 3, 0);
    lv_obj_add_flag(g_eq_type_val, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_eq_type_val, eq_type_open_cb, LV_EVENT_CLICKED, NULL);

    /* pager is created early: raise it above the type row */
    lv_obj_move_foreground(g_eq_pg_l);
    lv_obj_move_foreground(g_eq_pg_r);

    eq_sel_refresh();
    lv_obj_invalidate(panel);
    lv_refr_now(NULL);

    while (!g_eq_done)
        lvgui_pump();

    eq_write_apply();
    lv_obj_del(panel);
    lvgui_pump();
    return false;
}

/* ---- native date/time editor (screens.c set_time_screen stand-in) ------- */
static bool g_stm_done, g_stm_ok;
static struct tm g_stm;
static lv_obj_t *g_stm_labels[6];

static void stm_scrim_cb(lv_event_t *e)
{
    (void)e;
    g_stm_done = true;
    g_stm_ok = false;
}

static int stm_get(int i)
{
    switch (i)
    {
        case 0: return g_stm.tm_hour;
        case 1: return g_stm.tm_min;
        case 2: return g_stm.tm_sec;
        case 3: return g_stm.tm_year;
        case 4: return g_stm.tm_mon;
        default: return g_stm.tm_mday;
    }
}

static void stm_set(int i, int v)
{
    switch (i)
    {
        case 0: g_stm.tm_hour = v; break;
        case 1: g_stm.tm_min = v; break;
        case 2: g_stm.tm_sec = v; break;
        case 3: g_stm.tm_year = v; break;
        case 4: g_stm.tm_mon = v; break;
        default: g_stm.tm_mday = v; break;
    }
}

static int stm_days(void)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int y = g_stm.tm_year + 1900, m = g_stm.tm_mon;
    if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return d[m];
}

static void stm_label_refresh(int i)
{
    char b[16];
    if (!g_stm_labels[i])
        return;
    if (i == 3)
        snprintf(b, sizeof b, "%04d", g_stm.tm_year + 1900);
    else if (i == 4)
        snprintf(b, sizeof b, "%02d", g_stm.tm_mon + 1);
    else
        snprintf(b, sizeof b, "%02d", stm_get(i));
    lv_label_set_text(g_stm_labels[i], b);
}

static void stm_step_cb(lv_event_t *e)
{
    long code = (long)(intptr_t)lv_event_get_user_data(e);
    int i = (int)(code >> 1);
    int dir = (code & 1) ? 1 : -1;
    int mn = (i == 5) ? 1 : (i == 3 ? 1 : 0);
    int mx = (i == 0) ? 23 : (i == 1 || i == 2) ? 59 :
             (i == 3) ? 200 : (i == 4) ? 11 : stm_days();

    int v = stm_get(i) + dir;
    if (v < mn)
        v = mn;
    if (v > mx)
        v = mx;
    stm_set(i, v);
    if (g_stm.tm_mday > stm_days())
        g_stm.tm_mday = stm_days();
    for (int k = 0; k < 6; k++)
        stm_label_refresh(k);
}

static void stm_btn_cb(lv_event_t *e)
{
    g_stm_ok = (intptr_t)lv_event_get_user_data(e) != 0;
    g_stm_done = true;
}

bool lvgui_set_time_screen(const char *title, struct tm *tm, bool set_date)
{
    static const int caps[6] = {
        'H', 'M', 'S', 'Y', 0, 'D'
    };
    (void)caps;

    g_stm = *tm;
    g_stm_done = false;
    g_stm_ok = false;

    lv_obj_t *scr = lv_layer_top();
    lv_obj_t *scrim = lv_obj_create(scr);
    lv_obj_set_size(scrim, 360, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scrim, th->scrim, 0);
    lv_obj_set_style_bg_opa(scrim, th->scrim_opa, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_add_event_cb(scrim, stm_scrim_cb, LV_EVENT_CLICKED, NULL);

    const int CW = 340, CH = set_date ? 258 : 178;
    int cx = (360 - CW) / 2, cy = (480 - CH) / 2;
    shell_soft_shadow(scrim, cx, cy, CW, CH, th->radius, 6);
    lv_obj_t *card = shell_glass_new(scrim);
    lv_obj_set_size(card, CW, CH);
    lv_obj_set_pos(card, cx, cy);
    lv_obj_set_style_bg_color(card, th->glass_bg, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, th->border_white, 0);
    lv_obj_set_style_border_opa(card, th->card_border_opa, 0);
    lv_obj_set_style_radius(card, th->radius, 0);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title ? title : "");
    lv_obj_set_style_text_color(t, th->fg_text, 0);
    lv_obj_set_style_text_font(t, th->font_cjk, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);

    /* two rows of [-] value [+] steppers: time then (optionally) date */
    for (int row = 0; row < 2; row++)
    {
        if (row == 1 && !set_date)
            break;
        int y = 52 + row * 62;
        for (int c = 0; c < 3; c++)
        {
            int i = row * 3 + c;
            int gx = 16 + c * 104;

            lv_obj_t *b = lv_btn_create(card);
            lv_obj_set_size(b, 30, 30);
            lv_obj_set_pos(b, gx, y);
            lv_obj_set_style_bg_color(b, th->btn_secondary, 0);
            lv_obj_set_style_bg_opa(b, th->btn_secondary_opa, 0);
            lv_obj_set_style_radius(b, 10, 0);
            lv_obj_add_event_cb(b, stm_step_cb, LV_EVENT_CLICKED,
                                (void *)(long)(i * 2));

            lv_obj_t *bl = lv_label_create(b);
            lv_label_set_text(bl, LV_SYMBOL_MINUS);
            lv_obj_set_style_text_color(bl, th->fg_text, 0);
            lv_obj_set_style_text_font(bl, th->font_symbol, 0);
            lv_obj_center(bl);

            lv_obj_t *v = lv_label_create(card);
            lv_obj_set_style_text_font(v, th->font_symbol, 0);
            lv_obj_set_style_text_color(v, th->fg_text, 0);
            lv_obj_set_width(v, 40);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(v, gx + 32, y + 7);
            g_stm_labels[i] = v;
            stm_label_refresh(i);

            b = lv_btn_create(card);
            lv_obj_set_size(b, 30, 30);
            lv_obj_set_pos(b, gx + 74, y);
            lv_obj_set_style_bg_color(b, th->btn_secondary, 0);
            lv_obj_set_style_bg_opa(b, th->btn_secondary_opa, 0);
            lv_obj_set_style_radius(b, 10, 0);
            lv_obj_add_event_cb(b, stm_step_cb, LV_EVENT_CLICKED,
                                (void *)(long)(i * 2 + 1));

            bl = lv_label_create(b);
            lv_label_set_text(bl, LV_SYMBOL_PLUS);
            lv_obj_set_style_text_color(bl, th->fg_text, 0);
            lv_obj_set_style_text_font(bl, th->font_symbol, 0);
            lv_obj_center(bl);
        }
    }

    /* OK / cancel buttons */
    lv_obj_t *ok = lv_btn_create(card);
    lv_obj_set_size(ok, 144, 38);
    lv_obj_set_pos(ok, 16, CH - 52);
    lv_obj_set_style_bg_color(ok, th->accent_blue, 0);
    lv_obj_set_style_radius(ok, 12, 0);
    lv_obj_add_event_cb(ok, stm_btn_cb, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, str(LANG_SET_BOOL_YES));
    lv_obj_set_style_text_color(okl, lv_color_white(), 0);
    lv_obj_set_style_text_font(okl, th->font_cjk, 0);
    lv_obj_center(okl);

    lv_obj_t *no = lv_btn_create(card);
    lv_obj_set_size(no, 144, 38);
    lv_obj_set_pos(no, CW - 160, CH - 52);
    lv_obj_set_style_bg_color(no, th->btn_secondary, 0);
    lv_obj_set_style_bg_opa(no, th->btn_secondary_opa, 0);
    lv_obj_set_style_radius(no, 12, 0);
    lv_obj_add_event_cb(no, stm_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nol = lv_label_create(no);
    lv_label_set_text(nol, str(LANG_CANCEL));
    lv_obj_set_style_text_color(nol, th->fg_text, 0);
    lv_obj_set_style_text_font(nol, th->font_cjk, 0);
    lv_obj_center(nol);

    while (!g_stm_done)
        lvgui_pump();

    if (g_stm_ok)
        *tm = g_stm;
    else
        tm->tm_year = -1;                      /* stock cancel contract */

    lv_obj_del(scrim);
    lvgui_pump();
    return false;
}

/* ---- native UI font-size picker (theme_menu 字体, persisted setting) ----- */
/* ---- native main-menu (root menu) editor -------------------------------- */
static const char *g_rm_keys[24];
static unsigned char g_rm_en[24];
static lv_obj_t *g_rm_marks[24];
static int g_rm_n;
static bool g_rm_done;

static void rm_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= g_rm_n)
        return;
    g_rm_en[i] = !g_rm_en[i];
    lv_label_set_text(g_rm_marks[i], g_rm_en[i] ? LV_SYMBOL_OK : "");
}

static void rm_apply_cb(lv_event_t *e)
{
    (void)e;
    const char *sel[24];
    int k = 0;
    for (int i = 0; i < g_rm_n; i++)
        if (g_rm_en[i])
            sel[k++] = g_rm_keys[i];
    if (k == 0)
        return;
    be_root_apply(sel, k);
    settings_save();
    shell_home_rebuild();               /* apply immediately: no reboot */
    shell_toast(300, "已应用", LV_SYMBOL_OK);
    g_rm_done = true;
    shell_nav_pop();
}

static void rm_close_cb(lv_event_t *e)
{
    (void)e;
    g_rm_done = true;
}

int lvgui_root_menu_edit(void)
{
    const char *labels[24];
    g_rm_done = false;
    g_rm_n = be_root_items(g_rm_keys, labels, g_rm_en, 24);
    if (g_rm_n <= 0)
        return 0;

    lv_obj_t *list;
    lv_obj_t *scr = shell_menu_screen_push(str(LANG_MAIN_MENU), &list,
                                           rm_close_cb);
    for (int i = 0; i < g_rm_n; i++)
    {
        lv_obj_t *row = shell_menu_row_new(list, NULL, false, labels[i],
                                           false, false, i, rm_row_cb, -1);
        lv_obj_t *m = lv_label_create(row);
        lv_label_set_text(m, g_rm_en[i] ? LV_SYMBOL_OK : "");
        lv_obj_set_style_text_font(m, th->font_symbol, 0);
        lv_obj_set_style_text_color(m,
            g_rm_en[i] ? th->accent_blue : th->fg_dim, 0);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, -14, 0);
        g_rm_marks[i] = m;
    }
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 340, 48);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(btn, th->accent_blue, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, rm_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "应用");
    lv_obj_set_style_text_color(bl, th->fg_inverse, 0);
    lv_obj_center(bl);

    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    while (!g_rm_done)
        lvgui_pump();
    return 0;
}

int lvgui_font_pick(void)
{
    static char bufs[3][24];
    static const char *lbl[3];
    int cur = global_settings.ui_font_size;

    if (cur < 0 || cur > 2)
        cur = 1;
    snprintf(bufs[0], sizeof bufs[0], "小");
    snprintf(bufs[1], sizeof bufs[1], "默认");
    snprintf(bufs[2], sizeof bufs[2], "大");
    for (int i = 0; i < 3; i++)
        lbl[i] = bufs[i];

    int sel = shell_list_pick("界面字体大小", lbl, 3, cur);
    if (sel >= 0 && sel != cur)
    {
        global_settings.ui_font_size = sel;
        settings_save();
        /* fonts come from the theme: apply at once and start over on Home */
        ui_theme_init();
        shell_theme_refresh();
        shell_home_rebuild();
        shell_nav_home();
        g_menu_abort = true;
    }
    return GO_TO_PREVIOUS;
}

/* ---- native UI theme picker (theme_menu 主题, persisted setting) --------- */
int lvgui_theme_pick(void)
{
    static char bufs[2][24];
    static const char *lbl[2];
    int cur = global_settings.ui_dark_mode ? 1 : 0;

    snprintf(bufs[0], sizeof bufs[0], "浅色");
    snprintf(bufs[1], sizeof bufs[1], "深色");
    lbl[0] = bufs[0];
    lbl[1] = bufs[1];

    int sel = shell_list_pick("界面主题", lbl, 2, cur);
    if (sel >= 0 && sel != cur)
    {
        global_settings.ui_dark_mode = (sel == 1);
        settings_save();
        /* live switch: refill the palette and rebuild the UI (no reboot) */
        ui_theme_init();
        shell_theme_refresh();
        shell_home_rebuild();
        shell_nav_home();
        g_menu_abort = true;
    }
    return GO_TO_PREVIOUS;
}

/* ---- network-source player screens (netfm / bt-rx / dlna) ---------------
 * Redesigned playback UI for the three stream sources: hero card (icon,
 * source name, live state pill), info card, optional progress/buffer bar
 * and a stop button. Back gesture leaves the source running (stock
 * semantics); the stop button requests teardown by the stock caller. */
struct netsrc_data {
    char main[96];              /* station / track / peer               */
    char state[48];             /* live state text                      */
    bool active;                /* playing / connected -> accent colour */
    char lab[4][28];
    char val[4][40];
    int pct;                    /* 0..100, -1 = no bar                  */
    char bar_r[28];             /* text right of the bar                */
    const unsigned char *art;   /* cover bytes (RAM), NULL = none       */
    size_t art_len;
    unsigned art_gen;
    long dur_ms;
};

struct netsrc_view {
    const char *title;
    const char *icon;           /* LV_SYMBOL_* */
    const char *stop;           /* stop button caption */
    const char *bar_lab;        /* NULL = no bar, "" = bar without label */
    int period_ms;
    void (*fetch)(struct netsrc_data *d);
    int (*step)(int dir);       /* prev/next station, NULL = none */
    int icon_kind;              /* 0 = LV_SYMBOL icon, 1 = drawn radio */
};

static struct {
    const struct netsrc_view *v;
    struct netsrc_data d;
    lv_obj_t *scr;
    lv_obj_t *main_l, *state_l, *info_l, *bar, *bar_r_l, *bar_l_l;
    long dur_ms;
    lv_obj_t *tile, *art_img, *art_box;
    lv_img_dsc_t *art_dsc_p;
    lvui_art_t *art_dsc;
    unsigned art_gen;
    char infobuf[128];
    lv_timer_t *tm;
    bool done;
    int stop_req;
} g_ns;

static void ns_apply(void);

/* volume side capsule (same interaction as the local player) */
static lv_obj_t *g_ns_vol_ovl;
static lv_obj_t *g_ns_vol_val;

static void ns_vol_update(void)
{
    int v = global_status.volume;
    int lo = sound_min(SOUND_VOLUME), hi = sound_max(SOUND_VOLUME);
    int pct = hi > lo ? (v - lo) * 100 / (hi - lo) : 0;
    ui_volume_label(g_ns_vol_val, pct);
}

static void ns_vol_slider_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    sound_set(SOUND_VOLUME, v);
    global_status.volume = v;
    ns_vol_update();
}

static void ns_vol_scrim_cb(lv_event_t *e)
{
    (void)e;
    if (g_ns_vol_ovl)
        lv_obj_del(g_ns_vol_ovl);
    g_ns_vol_ovl = NULL;
    g_ns_vol_val = NULL;
}

static void ns_vol_toggle(void)
{
    if (g_ns_vol_ovl)
    {
        ns_vol_scrim_cb(NULL);
        return;
    }
    if (!g_ns.scr)
        return;

    lv_obj_t *ovl = lv_obj_create(g_ns.scr);
    lv_obj_set_size(ovl, 360, 480);
    lv_obj_set_pos(ovl, 0, 0);
    lv_obj_set_style_bg_opa(ovl, th->scrim_opa, 0);
    lv_obj_set_style_bg_color(ovl, th->scrim, 0);
    lv_obj_set_style_radius(ovl, 0, 0);
    lv_obj_set_style_border_width(ovl, 0, 0);
    shell_style_shadow_off(ovl);
    lv_obj_add_event_cb(ovl, ns_vol_scrim_cb, LV_EVENT_CLICKED, NULL);
    g_ns_vol_ovl = ovl;

    lv_obj_t *cap = ui_volume_capsule(ovl, 360 - 44 - 30, 132,
                                      ns_vol_slider_cb, NULL,
                                      &g_ns_vol_val);
    (void)cap;
}

static void ns_vol_btn_cb(lv_event_t *e)
{
    (void)e;
    ns_vol_toggle();
}

static void ns_step_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_ns.v && g_ns.v->step && g_ns.v->step(dir) >= 0)
        ns_apply();
}

static void ns_bar_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *ind;
    lv_point_t p;
    lv_area_t a;
    int w, pct, ms;

    if (!g_ns.bar || !g_ns.v || g_ns.v->icon_kind != 2 || g_ns.dur_ms <= 0)
        return;
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED)
        return;
    ind = lv_indev_get_act();
    if (!ind)
        return;
    lv_indev_get_point(ind, &p);
    lv_obj_get_coords(g_ns.bar, &a);
    w = lv_area_get_width(&a);
    if (w <= 0)
        return;
    pct = (p.x - a.x1) * 100 / w;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(g_ns.bar, pct, LV_ANIM_OFF);
    ms = (int)(g_ns.dur_ms * pct / 100);
    if (code == LV_EVENT_RELEASED)
        be_dlna_seek(ms);
}

static void ns_apply(void)
{
    const struct netsrc_view *v = g_ns.v;
    struct netsrc_data *d = &g_ns.d;
    if (!v)
        return;
    v->fetch(d);
    {
        /* Follow the shared decoded cover (whichever consumer decoded it);
         * the compressed bytes may already be gone by the time we look.
         * The lock keeps the download thread from freeing them mid-decode. */
        int pw = 0, ph = 0;
        unsigned pg = 0;
        const void *px;

        dlna_output_art_lock();
        px = be_dlna_art_pixels(&pw, &ph, &pg);

        if (g_ns.tile && pg != g_ns.art_gen)
        {
            lv_img_dsc_t *dsc;
            bool ok = false;

            g_ns.art_gen = pg;
            if (g_ns.art_img)
            {
                lv_obj_del(g_ns.art_img);
                g_ns.art_img = NULL;
            }
            if (g_ns.art_box)
            {
                lv_obj_del(g_ns.art_box);
                g_ns.art_box = NULL;
            }
            if (g_ns.art_dsc_p)
            {
                free(g_ns.art_dsc_p);
                g_ns.art_dsc_p = NULL;
            }

            /* only pixels decoded from THIS generation count */
            if (pg == d->art_gen && px && pw > 0)
                ok = true;
            else if (d->art && d->art_len &&
                     be_dlna_art_decode(d->art, d->art_len, d->art_gen))
            {
                px = be_dlna_art_pixels(&pw, &ph, &pg);
                g_ns.art_gen = pg;
                ok = (px != NULL && pw > 0);
            }

            dsc = calloc(1, sizeof *dsc);
            if (ok && dsc)
            {
                dsc->header.always_zero = 0;
                dsc->header.w = (lv_coord_t)pw;
                dsc->header.h = (lv_coord_t)ph;
                dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
                dsc->data_size = (uint32_t)pw * (uint32_t)ph * 2;
                dsc->data = (const uint8_t *)px;
                g_ns.art_dsc_p = dsc;

                g_ns.art_box = lv_obj_create(g_ns.scr);
                lv_obj_set_size(g_ns.art_box, 240, 240);
                lv_obj_set_pos(g_ns.art_box, 60, 62);
                lv_obj_set_style_radius(g_ns.art_box, 18, 0);
                lv_obj_set_style_bg_opa(g_ns.art_box, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(g_ns.art_box, 0, 0);
                lv_obj_set_style_pad_all(g_ns.art_box, 0, 0);
                lv_obj_set_style_clip_corner(g_ns.art_box, true, 0);
                shell_style_shadow_off(g_ns.art_box);
                lv_obj_clear_flag(g_ns.art_box, LV_OBJ_FLAG_SCROLLABLE);

                g_ns.art_img = lv_img_create(g_ns.art_box);
                lv_img_set_src(g_ns.art_img, dsc);
                lv_obj_center(g_ns.art_img);
                lv_obj_clear_flag(g_ns.art_img, LV_OBJ_FLAG_CLICKABLE);
            }
            else
            {
                ok = false;
                free(dsc);
            }

            if (ok)
            {
                lv_obj_add_flag(g_ns.tile, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(g_ns.main_l, 15, 308);
                lv_obj_add_flag(g_ns.state_l, LV_OBJ_FLAG_HIDDEN);
                if (g_ns.info_l)
                    lv_obj_add_flag(g_ns.info_l, LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_clear_flag(g_ns.tile, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(g_ns.main_l, 15, 218);
                lv_obj_clear_flag(g_ns.state_l, LV_OBJ_FLAG_HIDDEN);
                if (g_ns.info_l)
                    lv_obj_clear_flag(g_ns.info_l, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    dlna_output_art_unlock();
    lv_label_set_text(g_ns.main_l, d->main[0] ? d->main : "-");
    lv_obj_set_style_text_color(g_ns.state_l,
        d->active ? th->accent : th->fg_dim, 0);
    lv_label_set_text(g_ns.state_l, d->state);
    {
        int n = 0;
        g_ns.infobuf[0] = '\0';
        for (int i = 0; i < 4; i++)
        {
            if (!d->val[i][0])
                continue;
            if (n)
                strlcat(g_ns.infobuf, " · ", sizeof g_ns.infobuf);
            strlcat(g_ns.infobuf, d->val[i], sizeof g_ns.infobuf);
            n++;
        }
    }
    if (g_ns.info_l)
        lv_label_set_text(g_ns.info_l, g_ns.infobuf);
    if (g_ns.bar)
        lv_bar_set_value(g_ns.bar, d->pct < 0 ? 0 : d->pct, LV_ANIM_OFF);
    g_ns.dur_ms = d->dur_ms;
    if (g_ns.bar_l_l && g_ns.v && g_ns.v->icon_kind == 2 && g_ns.dur_ms > 0)
    {
        unsigned int ms = (unsigned int)(g_ns.dur_ms *
                                         (d->pct < 0 ? 0 : d->pct) / 100);
        char eb[16];
        snprintf(eb, sizeof eb, "%u:%02u", ms / 60000, (ms / 1000) % 60);
        lv_label_set_text(g_ns.bar_l_l, eb);
    }
    if (g_ns.bar_r_l)
        lv_label_set_text(g_ns.bar_r_l, d->pct < 0 ? "" : d->bar_r);
}

static void ns_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* the netfm screen (the only source with a step callback) must close
     * when the stream is stopped elsewhere, e.g. from the control centre */
    if (!g_ns.done && g_ns.v && g_ns.v->step && !be_media_is_radio())
    {
        g_ns.done = true;
        shell_nav_pop();
        return;
    }
    ns_apply();
}

static void ns_stop_cb(lv_event_t *e)
{
    (void)e;
    if (g_ns.v && g_ns.v->icon_kind == 2)
        be_dlna_stop();
    g_ns.stop_req = 1;
    g_ns.done = true;
    shell_nav_pop();            /* deferred: event-cb safe */
}

static void ns_close_cb(lv_event_t *e)
{
    (void)e;
    if (!g_ns.done)
    {
        g_ns.stop_req = 0;
        g_ns.done = true;
    }
}

/* small round transport button, same style as the local player */
static lv_obj_t *ns_gbtn(lv_obj_t *parent, lv_coord_t x, const char *sym,
                         bool accent, lv_event_cb_t cb, void *ud)
{
    return ui_round_btn(parent, x, 8, 48, sym, false, accent, cb, ud);
}

static int lvgui_netsrc_screen(const struct netsrc_view *v)
{
    g_ns.done = false;
    g_ns.stop_req = 0;
    g_ns.v = v;
    memset(&g_ns.d, 0, sizeof g_ns.d);
    g_ns.d.pct = -1;
    g_ns.main_l = g_ns.state_l = g_ns.info_l = NULL;
    g_ns.bar = g_ns.bar_r_l = g_ns.bar_l_l = NULL;
    g_ns.dur_ms = 0;
    g_ns.tile = g_ns.art_img = g_ns.art_box = NULL;
    g_ns.art_dsc_p = NULL;
    g_ns.art_dsc = NULL;
    g_ns.art_gen = 0;

    v->fetch(&g_ns.d);          /* first fill: decides the row count */

    lv_obj_t *scr = shell_screen_new();
    shell_statusbar_build(scr);
    shell_header(scr, v->title);
    lv_obj_add_event_cb(scr, ns_close_cb, LV_EVENT_DELETE, NULL);
    g_ns.scr = scr;

    /* integral player layout: one continuous screen, no cards */
    shell_soft_shadow(scr, 124, 92, 112, 112, 56, 8);
    lv_obj_t *tile = lv_obj_create(scr);
    lv_obj_set_size(tile, 112, 112);
    lv_obj_set_pos(tile, 124, 92);
    lv_obj_set_style_radius(tile, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, th->accent_blue, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);   /* default theme pad shifts
                                               absolutely-positioned art by
                                               ~17px right+down without it */
    shell_style_shadow_off(tile);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    g_ns.tile = tile;
    if (v->icon_kind == 1)
    {
        /* skeuomorphic portable radio, centred on the tile (pad 0):
         * antenna w/ ball tip, shaded cabinet, silver speaker with cone,
         * grille dots and a tuning dial with a pointer. */
        static const lv_point_t ant[] = { {40, 33}, {58, 9} };
        lv_obj_t *antl = lv_line_create(tile);
        lv_line_set_points(antl, ant, 2);
        lv_obj_set_style_line_color(antl, lv_color_hex(0xdde6f2), 0);
        lv_obj_set_style_line_width(antl, 3, 0);
        lv_obj_set_style_line_rounded(antl, true, 0);

        lv_obj_t *tip = lv_obj_create(tile);
        lv_obj_set_size(tip, 6, 6);
        lv_obj_set_pos(tip, 55, 6);
        lv_obj_set_style_radius(tip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(tip, lv_color_hex(0xdde6f2), 0);
        lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tip, 0, 0);
        shell_style_shadow_off(tip);
        lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE |
                               LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *body = lv_obj_create(tile);
        lv_obj_set_size(body, 78, 52);
        lv_obj_set_pos(body, 17, 31);
        lv_obj_set_style_radius(body, 12, 0);
        lv_obj_set_style_bg_color(body, lv_color_hex(0x49566c), 0);
        lv_obj_set_style_bg_grad_color(body, lv_color_hex(0x1f2735), 0);
        lv_obj_set_style_bg_grad_dir(body, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(body, lv_color_hex(0x9fb0ca), 0);
        lv_obj_set_style_border_width(body, 1, 0);
        lv_obj_set_style_pad_all(body, 0, 0);
        shell_style_shadow_off(body);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_CLICKABLE);

        /* speaker: silver ring + dark cone + centre dot */
        lv_obj_t *ring = lv_obj_create(body);
        lv_obj_set_size(ring, 30, 30);
        lv_obj_set_pos(ring, 13, 11);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ring, lv_color_hex(0xd7dfea), 0);
        lv_obj_set_style_bg_grad_color(ring, lv_color_hex(0x7e8ea6), 0);
        lv_obj_set_style_bg_grad_dir(ring, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(0xf2f6fb), 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_pad_all(ring, 0, 0);
        shell_style_shadow_off(ring);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *cone = lv_obj_create(ring);
        lv_obj_set_size(cone, 20, 20);
        lv_obj_align(cone, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(cone, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cone, lv_color_hex(0x232c3c), 0);
        lv_obj_set_style_bg_opa(cone, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cone, 0, 0);
        shell_style_shadow_off(cone);
        lv_obj_clear_flag(cone, LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *cd = lv_obj_create(cone);
        lv_obj_set_size(cd, 6, 6);
        lv_obj_align(cd, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(cd, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cd, lv_color_hex(0x8494ab), 0);
        lv_obj_set_style_bg_opa(cd, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cd, 0, 0);
        shell_style_shadow_off(cd);
        lv_obj_clear_flag(cd, LV_OBJ_FLAG_SCROLLABLE |
                               LV_OBJ_FLAG_CLICKABLE);

        /* grille dots between speaker and dial */
        for (int g = 0; g < 3; g++)
        {
            lv_obj_t *gd = lv_obj_create(body);
            lv_obj_set_size(gd, 3, 3);
            lv_obj_set_pos(gd, 47, 18 + g * 8);
            lv_obj_set_style_radius(gd, 1, 0);
            lv_obj_set_style_bg_color(gd, lv_color_hex(0xaebdd2), 0);
            lv_obj_set_style_bg_opa(gd, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(gd, 0, 0);
            shell_style_shadow_off(gd);
            lv_obj_clear_flag(gd, LV_OBJ_FLAG_SCROLLABLE |
                                   LV_OBJ_FLAG_CLICKABLE);
        }

        /* tuning dial with pointer */
        lv_obj_t *dial = lv_obj_create(body);
        lv_obj_set_size(dial, 14, 14);
        lv_obj_set_pos(dial, 55, 19);
        lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dial, lv_color_hex(0x2a3446), 0);
        lv_obj_set_style_bg_opa(dial, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dial, lv_color_hex(0xe8eef7), 0);
        lv_obj_set_style_border_width(dial, 2, 0);
        lv_obj_set_style_pad_all(dial, 0, 0);
        shell_style_shadow_off(dial);
        lv_obj_clear_flag(dial, LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_CLICKABLE);

        static const lv_point_t ptr[] = { {62, 33}, {66, 37} };
        lv_obj_t *ptl = lv_line_create(body);
        lv_line_set_points(ptl, ptr, 2);
        lv_obj_set_style_line_color(ptl, lv_color_hex(0xdde6f2), 0);
        lv_obj_set_style_line_width(ptl, 2, 0);
        lv_obj_set_style_line_rounded(ptl, true, 0);
    }
    else if (v->icon_kind == 2)
    {
        /* official-style DLNA mark: pre-rendered anti-aliased bitmap */
        LV_IMG_DECLARE(ui_icon_dlna);
        lv_obj_t *im = lv_img_create(tile);
        lv_img_set_src(im, &ui_icon_dlna);
        lv_obj_set_style_img_recolor(im, th->fg_ink, 0);
        lv_obj_set_style_img_recolor_opa(im, LV_OPA_COVER, 0);
        lv_obj_center(im);
        lv_img_set_zoom(im, 298);      /* 48 px -> ~56 px on the disc */
    }
    else
    {
        lv_obj_t *il = lv_label_create(tile);
        lv_label_set_text(il, v->icon ? v->icon : "");
        lv_obj_set_style_text_font(il, th->font_big, 0);
        lv_obj_set_style_text_color(il, th->fg_ink, 0);
        lv_obj_center(il);
    }

    g_ns.main_l = lv_label_create(scr);
    lv_obj_set_style_text_font(g_ns.main_l, th->font_cjk, 0);
    lv_obj_set_style_text_color(g_ns.main_l, th->fg_text, 0);
    lv_obj_set_style_text_align(g_ns.main_l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_ns.main_l, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(g_ns.main_l, 330, 44);
    lv_obj_set_pos(g_ns.main_l, 15, 218);

    g_ns.state_l = lv_label_create(scr);
    lv_obj_set_style_text_font(g_ns.state_l, th->font_cjk, 0);
    lv_obj_set_style_text_align(g_ns.state_l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(g_ns.state_l, 330, 20);
    lv_obj_set_pos(g_ns.state_l, 15, 270);

    g_ns.info_l = lv_label_create(scr);
    lv_obj_set_style_text_font(g_ns.info_l, th->font_cjk, 0);
    lv_obj_set_style_text_color(g_ns.info_l, th->fg_dim, 0);
    lv_obj_set_style_text_align(g_ns.info_l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_ns.info_l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_ns.info_l, 330, 20);
    lv_obj_set_pos(g_ns.info_l, 15, 296);

    if (v->bar_lab)
    {
        g_ns.bar = ui_progress_bar(scr, 30, 344, 300, 8, UI_BAR_BUFFER);
        lv_bar_set_range(g_ns.bar, 0, 100);
        lv_obj_add_flag(g_ns.bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(g_ns.bar, ns_bar_cb, LV_EVENT_ALL, NULL);

        if (*v->bar_lab)
        {
            lv_obj_t *bl = lv_label_create(scr);
            lv_label_set_text(bl, v->bar_lab);
            lv_obj_set_style_text_font(bl, th->font_cjk, 0);
            lv_obj_set_style_text_color(bl, th->fg_dim, 0);
            lv_obj_set_pos(bl, 30, 358);
        }

        g_ns.bar_l_l = lv_label_create(scr);
        lv_obj_set_style_text_font(g_ns.bar_l_l, th->font_cjk, 0);
        lv_obj_set_style_text_color(g_ns.bar_l_l, th->fg_dim, 0);
        lv_obj_set_pos(g_ns.bar_l_l, 16, 358);
        lv_label_set_text(g_ns.bar_l_l, "0:00");

        g_ns.bar_r_l = lv_label_create(scr);
        lv_obj_set_style_text_font(g_ns.bar_r_l, th->font_cjk, 0);
        lv_obj_set_style_text_color(g_ns.bar_r_l, th->fg_dim, 0);
        lv_obj_align(g_ns.bar_r_l, LV_ALIGN_TOP_RIGHT, -16, 358);
    }

    /* transport row: one full-width pill button group, evenly spread */
    {
        lv_obj_t *grp = ui_btn_group(scr, 10, 396, 340, 64);

        int n = v->step ? 4 : 2;
        int gap = (340 - n * 48) / (n + 1);
        int x = gap;
        if (v->step)
        {
            ns_gbtn(grp, x, LV_SYMBOL_PREV, false, ns_step_cb,
                    (void *)(intptr_t)-1);
            x += 48 + gap;
        }
        ns_gbtn(grp, x, LV_SYMBOL_STOP, false, ns_stop_cb, NULL);
        x += 48 + gap;
        if (v->step)
        {
            ns_gbtn(grp, x, LV_SYMBOL_NEXT, false, ns_step_cb,
                    (void *)(intptr_t)1);
            x += 48 + gap;
        }
        ns_gbtn(grp, x, LV_SYMBOL_VOLUME_MAX, false, ns_vol_btn_cb, NULL);
    }

    shell_nav_push(scr);

    ns_apply();
    g_ns.tm = lv_timer_create(ns_timer_cb,
                              v->period_ms >= 200 ? v->period_ms : 200,
                              NULL);
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);

    while (!g_ns.done)
        lvgui_pump();
    lv_timer_del(g_ns.tm);
    g_ns.v = NULL;
    if (g_ns.art_dsc)
    {
        lvui_art_free(g_ns.art_dsc);
        g_ns.art_dsc = NULL;
    }
    if (g_ns.art_dsc_p)
    {
        free(g_ns.art_dsc_p);
        g_ns.art_dsc_p = NULL;
    }
    g_ns.art_img = g_ns.tile = g_ns.art_box = NULL;
    g_ns.art_gen = 0;
    lvgui_pump();               /* let the deferred nav pop run */
    return g_ns.stop_req;
}

/* ---- netfm (network radio) ---------------------------------------------- */
static void ns_fetch_netfm(struct netsrc_data *d)
{
    struct netfm_stream_status st;
    be_netfm_get_status(&st);

    switch (st.state)
    {
        case NETFM_STREAM_CONNECTING:   strlcpy(d->state, str(LANG_NETFM_ST_CONNECTING), sizeof d->state); break;
        case NETFM_STREAM_BUFFERING:    strlcpy(d->state, str(LANG_NETFM_ST_BUFFERING), sizeof d->state); break;
        case NETFM_STREAM_PLAYING:      strlcpy(d->state, str(LANG_NETFM_ST_PLAYING), sizeof d->state); break;
        case NETFM_STREAM_DISCONNECTED: strlcpy(d->state, str(LANG_NETFM_ST_DISCONNECTED), sizeof d->state); break;
        case NETFM_STREAM_ERROR:        strlcpy(d->state, str(LANG_NETFM_ST_ERROR), sizeof d->state); break;
        default:                        strlcpy(d->state, str(LANG_NETFM_ST_STOPPED), sizeof d->state); break;
    }
    d->active = (st.state == NETFM_STREAM_PLAYING);
    snprintf(d->main, sizeof d->main, "%.95s", st.name);
    snprintf(d->lab[0], sizeof d->lab[0], "%s", str(LANG_NETFM_FORMAT));
    snprintf(d->val[0], sizeof d->val[0], "%.11s", st.format);
    snprintf(d->lab[1], sizeof d->lab[1], "%s", str(LANG_NETFM_BITRATE));
    snprintf(d->val[1], sizeof d->val[1], "%d kbps", st.bitrate);
    snprintf(d->lab[2], sizeof d->lab[2], "%s", str(LANG_BT_RX_RATE));
    snprintf(d->val[2], sizeof d->val[2], "%d Hz", st.sample_rate);
    d->lab[3][0] = '\0';
    d->pct = st.buffer_percent;
    snprintf(d->bar_r, sizeof d->bar_r, "%d%%", st.buffer_percent);
}

int lvgui_netfm_play(struct netfm_stream_status *init)
{
    (void)init;
    struct netsrc_view v = { str(LANG_NETFM), NULL,
                             str(LANG_NETFM_STOP), str(LANG_NETFM_BUFFER),
                             1000, ns_fetch_netfm, be_netfm_step, 1 };
    return lvgui_netsrc_screen(&v);
}

/* ---- bluetooth receive ---------------------------------------------------*/
static void ns_fetch_bt(struct netsrc_data *d)
{
    char peer[64];

    be_bt_rx_peer(peer, sizeof peer);
    strlcpy(d->state, be_bt_rx_state_name(), sizeof d->state);
    d->active = be_bt_rx_connected();
    snprintf(d->main, sizeof d->main, "%.95s", peer[0] ? peer : d->state);
    snprintf(d->lab[0], sizeof d->lab[0], "%s", str(LANG_BT_CODEC));
    snprintf(d->val[0], sizeof d->val[0], "%.31s", be_bt_codec());
    snprintf(d->lab[1], sizeof d->lab[1], "%s", str(LANG_BT_RX_RATE));
    {
        int rate = be_bt_rx_rate();
        snprintf(d->val[1], sizeof d->val[1],
                 rate > 0 ? "%d Hz" : "-", rate);
    }
    d->lab[2][0] = '\0';
    d->lab[3][0] = '\0';
    d->pct = -1;
}

int lvgui_bt_rx_view(void)
{
    struct netsrc_view v = { str(LANG_BT_RX), LV_SYMBOL_BLUETOOTH,
                             str(LANG_BT_DISCONNECT), NULL,
                             1000, ns_fetch_bt };
    return lvgui_netsrc_screen(&v);
}

/* ---- DLNA renderer --------------------------------------------------------*/
static void ns_fetch_dlna(struct netsrc_data *d)
{
    struct dlna_stream_status st;
    be_dlna_get_status(&st);
    d->art = dlna_output_art_data(&d->art_len, &d->art_gen);

    switch (st.state)
    {
        case DLNA_STREAM_CONNECTING:   strlcpy(d->state, str(LANG_NETFM_ST_CONNECTING), sizeof d->state); break;
        case DLNA_STREAM_BUFFERING:    strlcpy(d->state, str(LANG_NETFM_ST_BUFFERING), sizeof d->state); break;
        case DLNA_STREAM_PLAYING:      strlcpy(d->state, str(LANG_NETFM_ST_PLAYING), sizeof d->state); break;
        case DLNA_STREAM_DISCONNECTED: strlcpy(d->state, str(LANG_NETFM_ST_DISCONNECTED), sizeof d->state); break;
        case DLNA_STREAM_ERROR:        strlcpy(d->state, str(LANG_NETFM_ST_ERROR), sizeof d->state); break;
        default:                       strlcpy(d->state, str(LANG_DLNA_IDLE), sizeof d->state); break;
    }
    d->active = (st.state == DLNA_STREAM_PLAYING);
    snprintf(d->main, sizeof d->main, "%.95s", st.name);
    snprintf(d->lab[0], sizeof d->lab[0], "%s", str(LANG_DLNA_FORMAT));
    snprintf(d->val[0], sizeof d->val[0], "%.11s", st.format);
    snprintf(d->lab[1], sizeof d->lab[1], "%s", str(LANG_DLNA_BITRATE));
    snprintf(d->val[1], sizeof d->val[1], "%d kbps", st.bitrate);
    snprintf(d->lab[2], sizeof d->lab[2], "%s", str(LANG_BT_RX_RATE));
    snprintf(d->val[2], sizeof d->val[2], "%d Hz", st.sample_rate);
    d->lab[3][0] = '\0';
    {
        long pos_ms = 0, dur_ms = 0;
        be_dlna_time(&pos_ms, &dur_ms);
        st.position_ms = pos_ms;
        st.duration_ms = dur_ms;
        d->dur_ms = dur_ms;
    }
    if (st.duration_ms > 0)
    {
        unsigned int ms = (unsigned int)st.position_ms;
        unsigned int dur = (unsigned int)st.duration_ms;
        d->pct = (int)(ms * 100 / dur);
        snprintf(d->bar_r, sizeof d->bar_r, "%u:%02u",
                 dur / 60000, (dur / 1000) % 60);
    }
    else
    {
        d->pct = 0;
        unsigned int ms = (unsigned int)st.position_ms;
        snprintf(d->bar_r, sizeof d->bar_r, "%u:%02u",
                 ms / 60000, (ms / 1000) % 60);
    }
}

int lvgui_dlna_view(void)
{
    struct netsrc_view v = { str(LANG_DLNA), LV_SYMBOL_WIFI,
                             str(LANG_NETFM_STOP), "",
                             500, ns_fetch_dlna, NULL, 2 };
    return lvgui_netsrc_screen(&v);
}

/* ---- native text page (hw/debug/legal/document screens) ----------------- */
static lv_obj_t *g_txt_label;
static bool g_txt_done;
static char g_txt_buf[4096];
static void (*g_txt_fill)(char *buf, size_t sz, void *ud);
static void *g_txt_ud;

static void txt_close_cb(lv_event_t *e)
{
    (void)e;
    g_txt_done = true;
}

static void txt_refresh(void)
{
    if (!g_txt_label || !g_txt_fill)
        return;
    g_txt_buf[0] = 0;
    g_txt_fill(g_txt_buf, sizeof g_txt_buf, g_txt_ud);
    lv_label_set_text(g_txt_label, g_txt_buf);
}

static void txt_timer_cb(lv_timer_t *t)
{
    (void)t;
    txt_refresh();
}

void lvgui_text_page(const char *title, int period_ms,
                     void (*fill)(char *buf, size_t sz, void *ud), void *ud)
{
    g_txt_done = false;
    g_txt_fill = fill;
    g_txt_ud = ud;
    g_txt_buf[0] = 0;

    lv_obj_t *scr = shell_screen_new();
    shell_statusbar_build(scr);
    shell_header(scr, title);
    lv_obj_add_event_cb(scr, txt_close_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 360, 480 - 74);
    lv_obj_set_pos(cont, 0, 74);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_style_bg_color(cont, th->glass_bg, 0);
    lv_obj_set_style_bg_opa(cont, th->card_opa, 0);
    lv_obj_set_style_radius(cont, th->radius, 0);
    lv_obj_set_style_border_color(cont, th->border_white, 0);
    lv_obj_set_style_border_opa(cont, th->card_border_opa, 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    shell_style_shadow_off(cont);

    g_txt_label = lv_label_create(cont);
    lv_obj_set_width(g_txt_label, 328);
    lv_label_set_long_mode(g_txt_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(g_txt_label, th->fg_text, 0);
    lv_obj_set_style_text_font(g_txt_label, th->font_cjk, 0);

    txt_refresh();
    shell_nav_push(scr);
    lv_timer_t *tm = NULL;
    if (period_ms > 0)
        tm = lv_timer_create(txt_timer_cb, period_ms, NULL);
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);

    while (!g_txt_done)
        lvgui_pump();
    if (tm)
        lv_timer_del(tm);
    g_txt_label = NULL;
    g_txt_fill = NULL;
    lvgui_pump();                       /* run the deferred nav pop */
}

/* ---- credits / legal: the stock text files in a native page ------------- */
static void page_static_fill(char *buf, size_t sz, void *ud)
{
    strlcpy(buf, (const char *)ud, sz);
}

static int page_from_files(const char *title, const char *const *paths, int n)
{
    for (int i = 0; i < n; i++)
    {
        int len = be_read_text(paths[i], g_txt_buf, sizeof g_txt_buf - 1);
        if (len > 0)
        {
            g_txt_buf[len] = 0;
            lvgui_text_page(title, 0, page_static_fill, g_txt_buf);
            return 0;
        }
    }
    lvgui_text_page(title, 0, page_static_fill, "（未找到相关文本文件）");
    return 0;
}

int lvgui_credits_page(void)
{
    static const char *const files[] =
    {
        ROCKBOX_DIR "/docs/CREDITS",
        ROCKBOX_DIR "/CREDITS",
    };
    return page_from_files("鸣谢", files, 2);
}

int lvgui_legal_page(void)
{
    static const char *const files[] =
    {
        ROCKBOX_DIR "/docs/COPYING.txt",
        ROCKBOX_DIR "/docs/LICENSES.txt",
        ROCKBOX_DIR "/COPYING",
    };
    return page_from_files("法律声明", files, 3);
}

/* ---- cuesheet track picker (replaces the classic synclist) -------------- */
int lvgui_cuesheet_pick(struct cuesheet *cue, const char *title)
{
    static const char *labels[256];
    static char bufs[256][112];
    int n = cue->track_count;
    if (n > 256)
        n = 256;
    for (int i = 0; i < n; i++)
    {
        unsigned long off = cue->tracks[i].offset;
        snprintf(bufs[i], sizeof bufs[i], "%d. %s   %lu:%02lu", i + 1,
                 cue->tracks[i].title, off / 60000, (off / 1000) % 60);
        labels[i] = bufs[i];
    }
    if (n <= 0)
        return -1;
    return shell_list_pick(title, labels, n, 0);
}

#endif /* HAVE_LVGL_UI */
