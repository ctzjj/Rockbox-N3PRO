/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__  _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \ \/ /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|   /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/          \/     \/    \/            \/
 *
 * M2 library screens: database browsing through the stock tagtree,
 * all tracks, genres, years) and start playback from any list.
 *
 * Lists are windowed the way the classic gui_synclist does it: a fixed
 * viewport holds only the handful of visible row widgets; dragging moves
 * a virtual offset and re-binds the rows. Object coordinates stay small
 * no matter how long the list is, and memory stays flat.
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
#include "lvgl_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_backend.h"
#include "tagcache.h"
#include "playlist.h"
#include "thread.h"
#include "kernel.h"
#include "metadata.h"
#include "dir.h"
#include "file.h"
#include "tree.h"
#include "filetree.h"
#include "filetypes.h"
#include "onplay.h"
#include "playlist_catalog.h"
#include "menu.h"
#include "action.h"
#include "settings.h"
#include "lang.h"
#include "albumart.h"
#include "jpeg_load.h"
#include "bmp.h"
#include "string-extra.h"
#include <fcntl.h>
#include <unistd.h>

/* tagcache.c scan-phase file counter (scan counts files, not dirs) */
extern int tc_scan_files;
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LIB_MAX_ITEMS  2000      /* query result guard */



/* ---- queried data, freed together with the screen ---------------------- */

typedef struct
{
    be_list_t tk;
    char **fsym;              /* browser rows: per-row icon code "d"/"a"/"f" */
} lib_data;

/* ---- tagcache queries --------------------------------------------------- */
/* Unique values of one tag ("uniq" list). ftag/fvalue optionally filters
 * by another string tag (pass ftag < 0 for none). For numeric result tags
 * the seek holds the numeric value (usable as clause data). */

/* Track list (title + artist + filename), sorted by title, optionally
 * filtered by a string tag (album/artist/genre) and a numeric clause
 * (year). Pass tag < 0 to skip a filter. */






/* the file-browser list currently on screen: lets the delete action drop
 * just that one row instead of re-reading (and re-sorting) the folder */
static lib_data *g_files_d;
static void *g_files_v;                 /* vlist_t*, fully declared later */

static void lib_data_delete_cb(lv_event_t *e)
{
    lib_data *d = lv_event_get_user_data(e);
    if (!d)
        return;
    if (d == g_files_d)
    {
        g_files_d = NULL;
        g_files_v = NULL;
    }
    for (int i = 0; i < d->tk.count; i++)
    {
        free(d->tk.title[i]);
        free(d->tk.sub[i]);
        free(d->tk.path[i]);
    }
    free(d->tk.title);
    free(d->tk.sub);
    free(d->tk.path);
    /* fsym only exists on file-browser screens; every other tk filler
     * leaves it NULL */
    if (d->fsym)
    {
        for (int i = 0; i < d->tk.count; i++)
            free(d->fsym[i]);
        free(d->fsym);
    }
    free(d);
}

static lib_data *lib_data_new(lv_obj_t *scr)
{
    lib_data *d = calloc(1, sizeof *d);
    if (d)
        lv_obj_add_event_cb(scr, lib_data_delete_cb, LV_EVENT_DELETE, d);
    return d;
}

/* ---- generic screen bits ------------------------------------------------- */
static lv_obj_t *lib_screen(const char *title)
{
    lv_obj_t *scr = shell_screen_new();
    shell_statusbar_build(scr);
    shell_header(scr, title);
    return scr;
}

static lv_obj_t *lib_empty(lv_obj_t *scr, const char *msg)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_color(l, shell_col_dim(), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 130);
    return l;
}

/* ---- shared art accessors (used by the player screen) -------------------- */



/* ---- forward decls -------------------------------------------------------- */
/* ---- item activation (was the materialized row callback) ----------------- */
enum { A_PLAY, A_PJUMP, A_FILE, A_PLPLAY, A_DB };

static void lib_db_push_level(void);

/* "Browse current directory" / "Keep last directory" from File View:
 * the remembered folder survives only while keep_directory is enabled. */
static char files_last_dir[MAX_PATH] = "/";

static void files_open_dir(const char *path);
static void files_play(lib_data *d, int idx);
static void files_hold(lib_data *d, int idx);
static void plfile_hold(lib_data *d, int idx);
static void files_dir_menu(void);
static void files_empty_hold_cb(lv_event_t *e);

/* selection the context menu was opened on (used by props/delete/playlist) */
static char g_ctx_path[MAX_PATH];
static bool g_ctx_isdir;
static bool g_ctx_pllist;    /* ctx menu opened from the playlists list */

/* stock playlist catalog browses with dirfilter = SHOW_M3U (playlist_catalog.c
 * display_playlists); mirror rockbox_browse's temporary dirfilter swap while
 * the onplay menu is open so item visibility matches the original exactly */
static int g_ctx_dirfilter_val;
static int *g_ctx_prev_dirfilter;
static bool g_ctx_dirfilter_set;

/* The stock onplay menu decides visibility from *tree_context->dirfilter
 * (onplay.c clipboard_callback / dir_callback).  Set it to the scenario we
 * are showing (file browser = user's File settings filter, playlist catalog
 * = SHOW_M3U) for the lifetime of the context menu, then restore. */
static void ctx_dirfilter_begin(int filter)
{
    struct tree_context *tc = tree_get_context();
    if (g_ctx_dirfilter_set)
        return;
    g_ctx_prev_dirfilter = tc->dirfilter;
    g_ctx_dirfilter_val = filter;
    tc->dirfilter = &g_ctx_dirfilter_val;
    g_ctx_dirfilter_set = true;
}

static void ctx_dirfilter_restore_cb(lv_event_t *e)
{
    (void)e;
    if (g_ctx_dirfilter_set)
    {
        tree_get_context()->dirfilter = g_ctx_prev_dirfilter;
        g_ctx_dirfilter_set = false;
    }
}
static void ctx_do_props(void *p);

/* playlist move-mode: track being relocated (-1 = idle). While set, a tap
 * on another row commits the move (playlist_viewer.c move-mode). */
static int g_pl_moving_from = -1;
static void pl_reload_async(void *p);

/* Play a playlist file exactly like the file browser does (dir + name,
 * through the original ft_play_playlist: bookmarks, warnings, shuffle). */

/* database row activation may open tagtree's runtime search (kbd_input),
 * which blocks and pumps LVGL -- it must not run inside lv_timer_handler */
static void db_enter_job(void *p)
{
    int idx = (int)(intptr_t)p;
    if (be_db_enter(idx) == 0)
        lib_db_push_level();
    else
        shell_player_show();
}

static void pl_file_job(void *p)
{
    char *path = p;
    if (be_play_playlist_file(path))
        shell_player_show();
    free(path);
}

static void lib_activate(lib_data *d, int idx, int action)
{
    if (!d)
        return;
    switch (action)
    {
    case A_DB:        /* database row: descend a level or start playback */
        lvgui_run_job(db_enter_job, (void *)(intptr_t)idx);
        break;
    case A_PLAY:      /* track row -> play list from this index */
        be_play(&d->tk, idx);
        shell_player_show();
        break;
    case A_PJUMP:     /* current-playlist row -> jump in the live playlist */
        if (g_pl_moving_from >= 0)
        {
            /* move-mode: relocate the track to the tapped row (same row
             * cancels, like playlist_viewer.c's cancel) */
            int from = g_pl_moving_from;
            g_pl_moving_from = -1;
            if (idx != from)
                be_playlist_move_rows(from, idx);
            lv_async_call(pl_reload_async, NULL);
            break;
        }
        be_playlist_start(idx);
        shell_player_show();
        break;
    case A_FILE:      /* file browser row -> enter a folder or play a file */
        if (dir_exists(d->tk.path[idx]))
            files_open_dir(d->tk.path[idx]);
        else if (be_is_playlist_name(d->tk.path[idx]))
            lvgui_run_job(pl_file_job, strdup(d->tk.path[idx]));
        else
             files_play(d, idx);
         break;
    case A_PLPLAY:    /* playlists screen: play this playlist */
        lvgui_run_job(pl_file_job, strdup(d->tk.path[idx]));
        break;
    }
}

/* ---- virtual list core: viewport + offset, like gui_synclist ------------- */
typedef struct vlist_s vlist_t;
struct vlist_s
{
    lv_obj_t *cont;            /* viewport: receives every touch */
    lv_obj_t *sbar;            /* scrollbar indicator */
    int total_h, view_h;       /* virtual content / viewport height */
    int off;                   /* virtual offset, 0 .. total_h - view_h */
    int press_x, press_y;      /* press start (screen coords) */
    int last_y;
    int moved;                 /* accumulated |dy| since press */
    int vel;                   /* drag velocity, 1/4 px per tick */
    lv_timer_t *inertia;
    lv_timer_t *hold_timer;    /* fires when a press is held without moving */
    bool held;
    void *ud;
    void (*render)(vlist_t *v);
    void (*click)(vlist_t *v, int y_local, int x_local);
    void (*hold)(vlist_t *v, int y_local, int x_local);
    void *ctx;                 /* screen-side context, freed with the list */
};

static void vlist_scroll(vlist_t *v, int off)
{
    if (off < 0)
        off = 0;
    int max = v->total_h - v->view_h;
    if (max < 0)
        max = 0;
    if (off > max)
        off = max;
    if (off == v->off)
        return;
    v->off = off;
    v->render(v);
    if (max > 0 && v->sbar)
    {
        int h = lv_obj_get_height(v->sbar);
        int y = (v->view_h - h) * off / max;
        lv_obj_set_pos(v->sbar, 355, y);
    }
}

static void vlist_inertia_cb(lv_timer_t *t)
{
    vlist_t *v = t->user_data;
    int before = v->off;
    vlist_scroll(v, v->off + v->vel / 4);
    v->vel = v->vel * 92 / 100;
    if ((v->vel < 4 && v->vel > -4) || v->off == before)
    {
        lv_timer_del(t);
        v->inertia = NULL;
    }
}

static void vlist_hold_cb(lv_timer_t *t)
{
    vlist_t *v = t->user_data;
    v->hold_timer = NULL;
    v->held = true;                /* dispatched on RELEASED */
}

static void vlist_press_cb(lv_event_t *e)
{
    vlist_t *v = lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    v->press_x = p.x;
    v->press_y = v->last_y = p.y;
    v->moved = 0;
    v->vel = 0;
    v->held = false;
    if (v->hold_timer)
    {
        lv_timer_del(v->hold_timer);
        v->hold_timer = NULL;
    }
    if (v->inertia)
    {
        lv_timer_del(v->inertia);
        v->inertia = NULL;
    }
    if (v->hold)
    {
        v->hold_timer = lv_timer_create(vlist_hold_cb, 450, v);
        lv_timer_set_repeat_count(v->hold_timer, 1);
    }
}

static void vlist_drag_cb(lv_event_t *e)
{
    vlist_t *v = lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    int dy = v->last_y - p.y;
    v->last_y = p.y;
    v->moved += dy < 0 ? -dy : dy;
    if (v->moved > 10 && v->hold_timer)
    {
        lv_timer_del(v->hold_timer);
        v->hold_timer = NULL;
    }
    v->vel = (v->vel * 3 + dy * 4) / 4;
    vlist_scroll(v, v->off + dy);
}

static void vlist_release_cb(lv_event_t *e)
{
    vlist_t *v = lv_event_get_user_data(e);
    if (v->hold_timer)
    {
        lv_timer_del(v->hold_timer);
        v->hold_timer = NULL;
    }
    if (v->moved < 10)
    {
        lv_area_t a;
        lv_obj_get_coords(v->cont, &a);
        if (v->held && v->hold)
            v->hold(v, v->press_y - a.y1, v->press_x - a.x1);
        else if (v->click)
            v->click(v, v->press_y - a.y1, v->press_x - a.x1);
        return;
    }
    if (v->vel >= 10 || v->vel <= -10)
        v->inertia = lv_timer_create(vlist_inertia_cb, 16, v);
}

static void vlist_delete_cb(lv_event_t *e)
{
    vlist_t *v = lv_event_get_user_data(e);
    if (v->inertia)
        lv_timer_del(v->inertia);
    if (v->hold_timer)
        lv_timer_del(v->hold_timer);
    free(v->ctx);
    free(v);
}

static vlist_t *vlist_new(lv_obj_t *scr, int y, int h, int total_h, void *ud,
                          void (*render)(vlist_t *), 
                          void (*click)(vlist_t *, int, int), void *ctx)
{
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 360, h);
    lv_obj_set_pos(cont, 0, y);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);

    vlist_t *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->cont = cont;
    v->view_h = h;
    v->total_h = total_h;
    v->ud = ud;
    v->render = render;
    v->click = click;
    v->ctx = ctx;
    lv_obj_add_event_cb(cont, vlist_press_cb, LV_EVENT_PRESSED, v);
    lv_obj_add_event_cb(cont, vlist_drag_cb, LV_EVENT_PRESSING, v);
    lv_obj_add_event_cb(cont, vlist_release_cb, LV_EVENT_RELEASED, v);
    lv_obj_add_event_cb(cont, vlist_delete_cb, LV_EVENT_DELETE, v);

    if (total_h > h && global_settings.scrollbar != SCROLLBAR_OFF)
    {
        v->sbar = lv_obj_create(cont);
        int sbh = h * h / total_h;
        if (sbh < 24)
            sbh = 24;
        lv_obj_set_size(v->sbar, 3, sbh);
        lv_obj_set_pos(v->sbar,
            global_settings.scrollbar == SCROLLBAR_LEFT ? 2 : 355, 0);
        lv_obj_clear_flag(v->sbar, LV_OBJ_FLAG_SCROLLABLE |
                                    LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(v->sbar, 2, 0);
        lv_obj_set_style_bg_opa(v->sbar, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(v->sbar, th->border_white, 0);
        lv_obj_set_style_border_width(v->sbar, 0, 0);
        shell_style_shadow_off(v->sbar);
    }
    return v;
}

/* ---- windowed row widgets ------------------------------------------------- */
typedef struct
{
    lv_obj_t *wrap;    /* movable: shadow + glass row */
    lv_obj_t *row, *ic, *icl, *t, *s;
    int idx;           /* bound item index, -1 = none */
} vrow_t;

/* 'S' strlist (icon glyph, no sub), 'T' tracks (no icon), 'D' numbered */
typedef struct
{
    lib_data *d;
    int count, row_h, action;
    char kind;
    vrow_t rows[10];
    int n;
} vrows_t;

static void vrow_make(vrow_t *r, lv_obj_t *parent, int kind)
{
    memset(r, 0, sizeof *r);
    r->idx = -1;
    int has_icon = kind != 'T';

    r->wrap = lv_obj_create(parent);
    lv_obj_set_size(r->wrap, 360, 44);
    lv_obj_add_flag(r->wrap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(r->wrap, LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(r->wrap, 0, 0);
    lv_obj_set_style_bg_opa(r->wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(r->wrap, 0, 0);
    lv_obj_set_style_radius(r->wrap, 0, 0);

    r->row = shell_glass_new(r->wrap);
    lv_obj_set_size(r->row, 340, 44);
    lv_obj_set_pos(r->row, 10, 0);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(r->row, th->menu_row_opa, 0);
    lv_obj_set_style_border_opa(r->row, th->card_border_opa, 0);

    int ix = has_icon ? 44 : 12;
    if (has_icon)
    {
        r->ic = lv_obj_create(r->row);
        lv_obj_set_size(r->ic, 24, 24);
        lv_obj_align(r->ic, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_style_radius(r->ic, 7, 0);
        lv_obj_set_style_pad_all(r->ic, 0, 0);
        lv_obj_set_style_border_width(r->ic, 0, 0);
        lv_obj_set_style_bg_opa(r->ic,
                                kind == 'F' || kind == 'P' ? LV_OPA_TRANSP
                                                           : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r->ic, shell_icon_grad(0), 0);
        shell_style_shadow_off(r->ic);
        lv_obj_clear_flag(r->ic, LV_OBJ_FLAG_SCROLLABLE |
                                  LV_OBJ_FLAG_CLICKABLE);

        r->icl = lv_label_create(r->ic);
        if (kind == 'F' || kind == 'P')
            lv_obj_set_style_text_font(r->icl, th->font_symbol, 0);
        else
            lv_obj_set_style_text_color(r->icl, shell_col_ink(), 0);
        lv_obj_center(r->icl);
    }

    r->t = lv_label_create(r->row);
    lv_obj_set_style_text_color(r->t, shell_col_text(), 0);
    lv_label_set_long_mode(r->t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(r->t, 340 - ix - 14);

    if (kind != 'S')
    {
        r->s = lv_label_create(r->row);
        lv_obj_set_style_text_color(r->s, shell_col_dim(), 0);
        lv_label_set_long_mode(r->s, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r->s, 340 - ix - 14);
    }
}

static const char *first_glyph(const char *s)
{
    static char buf[5];
    if (!s || !s[0])
        return "?";
    int len = 1;
    unsigned char c = (unsigned char)s[0];
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC0) len = 2;
    if (len > 4) len = 4;
    memcpy(buf, s, len);
    buf[len] = 0;
    return buf;
}

static void vrows_render(vlist_t *v)
{
    vrows_t *vr = v->ud;
    int first = v->off / vr->row_h;
    for (int k = 0; k < vr->n; k++)
    {
        vrow_t *r = &vr->rows[k];
        int idx = first + k;
        int y = idx * vr->row_h - v->off;
        if (idx >= vr->count)
        {
            if (r->idx != -1)
            {
                lv_obj_add_flag(r->wrap, LV_OBJ_FLAG_HIDDEN);
                r->idx = -1;
            }
            continue;
        }
        lv_obj_set_pos(r->wrap, 0, y);
        lv_obj_clear_flag(r->wrap, LV_OBJ_FLAG_HIDDEN);
        if (r->idx == idx)
            continue;
        r->idx = idx;

        const char *title, *sub = NULL;
        char num[8];
        char ctitle[128], csub[128];
        {
            title = shell_cs(ctitle, sizeof ctitle, vr->d->tk.title[idx]);
            if (vr->d->tk.sub[idx])
                sub = shell_cs(csub, sizeof csub, vr->d->tk.sub[idx]);
            if (vr->kind == 'D')
            {
                snprintf(num, sizeof num, "%d", idx + 1);
                lv_label_set_text(r->icl, num);
                lv_obj_set_style_bg_color(r->ic, shell_icon_grad(idx), 0);
            }
            else if (vr->kind == 'F')
            {
                const char *code = vr->d->fsym[idx];
                const char *sym = LV_SYMBOL_FILE;
                lv_color_t col = shell_col_dim();
                if (code && code[0] == 'd')
                {
                    sym = LV_SYMBOL_DIRECTORY;
                    col = th->accent;
                }
                else if (code && code[0] == 'a')
                {
                    sym = LV_SYMBOL_AUDIO;
                    col = th->accent_blue;
                }
                lv_label_set_text(r->icl, sym);
                lv_obj_set_style_text_color(r->icl, col, 0);
            }
            else if (vr->kind == 'P')
            {
                /* playlist files: the classic browser's playlist icon role */
                lv_label_set_text(r->icl, LV_SYMBOL_LIST);
                lv_obj_set_style_text_color(r->icl, th->accent, 0);
            }
        }
        lv_label_set_text(r->t, title);
        if (r->s)
        {
            if (sub && sub[0])
            {
                lv_label_set_text(r->s, sub);
                lv_obj_clear_flag(r->s, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(r->t, LV_ALIGN_LEFT_MID,
                             r->ic ? 44 : 12, -9);
                lv_obj_align(r->s, LV_ALIGN_LEFT_MID,
                             r->ic ? 44 : 12, 9);
            }
            else
            {
                lv_obj_add_flag(r->s, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(r->t, LV_ALIGN_LEFT_MID, r->ic ? 44 : 12, 0);
            }
        }
        else
            lv_obj_align(r->t, LV_ALIGN_LEFT_MID, r->ic ? 44 : 12, 0);
    }
}

static void vrows_click(vlist_t *v, int y_local, int x_local)
{
    (void)x_local;
    vrows_t *vr = v->ud;
    int idx = (y_local + v->off) / vr->row_h;
    if (idx < 0 || idx >= vr->count)
        return;
    lib_activate(vr->d, idx, vr->action);
}

static void vrows_hold(vlist_t *v, int y_local, int x_local)
{
    (void)x_local;
    vrows_t *vr = v->ud;
    int idx = (y_local + v->off) / vr->row_h;
    if (idx < 0 || idx >= vr->count)
        return;
    if (vr->kind == 'F')
        files_hold(vr->d, idx);
    else if (vr->kind == 'P')
        plfile_hold(vr->d, idx);
}

static vlist_t *vrows_new(lv_obj_t *scr, int y, int h, lib_data *d, int count,
                          int row_h, char kind, int action)
{
    vrows_t *vr = calloc(1, sizeof *vr);
    if (!vr)
        return NULL;
    vr->d = d;
    vr->count = count;
    vr->row_h = row_h;
    vr->kind = kind;
    vr->action = action;
    vr->n = h / row_h + 2;
    if (vr->n > 10)
        vr->n = 10;
    if (vr->n > count)
        vr->n = count;
    vlist_t *v = vlist_new(scr, y, h, count * row_h, vr, vrows_render,
                           vrows_click, vr);
    if (!v)
    {
        free(vr);
        return NULL;
    }
    for (int k = 0; k < vr->n; k++)
        vrow_make(&vr->rows[k], v->cont, kind);
    v->hold = vrows_hold;
    v->render(v);
    return v;
}

static void pl_stem(const char *path, char *out, size_t outsz)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strlcpy(out, base, outsz);
    char *dot = strrchr(out, '.');
    if (dot && dot != out)
        *dot = '\0';
}

/* ---- current-playlist context menu (playlist_viewer.c aligned) ----------- */
static void pl_reload_async(void *p)
{
    (void)p;
    shell_nav_pop_now();
    shell_playlist_show();
}

/* Runs as a job: exactly the playlist_viewer.c context menu -- the same
 * item list, order and handlers -- with do_menu() diverted to our native
 * LVGL engine (see apps/menu.c).  Selection 3 enters the viewer's move
 * mode; the commit/cancel happen on the playlist list itself. */
static void pl_menu_job(void *param)
{
    int idx = (int)(long)param;
    char path[MAX_PATH];
    if (!be_playlist_track_path(idx, path, sizeof path))
        return;

    MENUITEM_STRINGLIST(menu_items, ID2P(LANG_PLAYLIST), NULL,
                        ID2P(LANG_PLAYING_NEXT), ID2P(LANG_ADD_TO_PL),
                        ID2P(LANG_REMOVE), ID2P(LANG_MOVE),
                        ID2P(LANG_MENU_SHOW_ID3_INFO), ID2P(LANG_SHUFFLE),
                        ID2P(LANG_SAVE), ID2P(LANG_PLAYLISTVIEWER_SETTINGS));

    int sel = do_menu(&menu_items, NULL, NULL, false);
    if (sel < 0)
        return;                              /* cancelled / USB attached */

    /* Abort any current move (playlist_viewer.c does the same) */
    g_pl_moving_from = -1;

    switch (sel)
    {
    case 0:   /* Playing Next... */
        be_onplay_insert_menu(path);
        break;                               /* queue changed: reload below */
    case 1:   /* Add to Playlist... */
        be_onplay_addto_menu(path);
        break;
    case 2:   /* remove */
        be_playlist_remove(idx);
        break;
    case 3:   /* move: enter move mode, commit on the list */
        g_pl_moving_from = idx;
        break;
    case 4:   /* track info */
        g_ctx_isdir = false;
        strlcpy(g_ctx_path, path, sizeof g_ctx_path);
        ctx_do_props(NULL);
        return;                              /* props on top */
    case 5:   /* shuffle */
        if (shell_yesno_run(str(LANG_SHUFFLE)) == 0)
            be_playlist_shuffle();
        break;
    case 6:   /* save */
        be_playlist_save_prompt();
        break;
    case 7:   /* playlist viewer settings (native via do_menu hook) */
        be_playlist_viewer_settings();
        break;
    }
    lv_async_call(pl_reload_async, NULL);
}

static void pl_rows_hold(vlist_t *v, int y_local, int x_local)
{
    (void)x_local;
    int idx = (y_local + v->off) / 48;
    if (idx < 0 || idx >= be_playlist_count())
        return;
    lvgui_run_job(pl_menu_job, (void *)(long)idx);
}

void shell_playlist_show(void)
{
    if (shell_nav_full())
        return;
    lv_obj_t *scr = lib_screen(str(LANG_PLAYLISTS));
    shell_nav_push(scr);

    lib_data *d = lib_data_new(scr);
    if (!d)
        return;

    int total = be_playlist_count();
    if (total > LIB_MAX_ITEMS)
        total = LIB_MAX_ITEMS;
    int cur = be_playlist_resume();          /* 0-based index of playing track */

    int n = 0;
    if (total > 0)
    {
        char **ti = malloc(total * sizeof(char *));
        char **su = malloc(total * sizeof(char *));
        char **pa = malloc(total * sizeof(char *));
        if (ti && su && pa)
        {
            char tpath[MAX_PATH];
            for (int i = 0; i < total; i++)
            {
                if (!be_playlist_track_path(i, tpath, sizeof tpath))
                    break;
                char line[MAX_PATH + 16];
                be_playlist_row_label(i, i == cur, line, sizeof line);
                ti[n] = strdup(line);
                su[n] = strdup("");
                pa[n] = strdup(tpath);
                n++;
            }
        }
        else
        {
            free(ti); free(su); free(pa);
        }
        d->tk.title = ti;
        d->tk.sub = su;
        d->tk.path = pa;
        d->tk.count = n;
    }

    if (n <= 0)
    {
        lib_empty(scr, str(LANG_N3_EMPTY_PLAYLIST));
        return;
    }
    vlist_t *vl = vrows_new(scr, 74, 406, d, n, 48, 'T', A_PJUMP);
    /* long-press: the classic playlist viewer's context menu */
    if (vl)
        vl->hold = pl_rows_hold;
    /* move-mode banner: tap a destination row to commit, the same row to
     * cancel (playlist_viewer.c move-mode) */
    if (g_pl_moving_from >= 0)
    {
        lv_obj_t *h = lv_label_create(scr);
        lv_label_set_text(h, str(LANG_N3_MOVE_HINT));
        lv_obj_set_style_text_color(h, shell_col_link(), 0);
        lv_obj_align(h, LV_ALIGN_TOP_LEFT, 12, 52);
    }
    /* classic playlist viewer opens on the playing track */
    if (vl && cur > 0)
        vlist_scroll(vl, cur * 48 - (406 - 48) / 2);
}

/* ---- screens --------------------------------------------------------------- */
/* ---- database browsing (stock tagtree) ----------------------------------
 * Levels, filters, sorting and the play action all come from rockbox's own
 * database browser (apps/tagtree.c, driven exactly like tree.c drives it).
 * This layer only renders the entries the stock code produced. */
static void lib_db_build(lv_obj_t *scr)
{
    char **names, **icons;
    int n = be_db_scan(&names, &icons);
    if (n < 0)
    {
        lib_empty(scr, str(LANG_TAGCACHE_BUSY));
        return;
    }
    lib_data *d = lib_data_new(scr);
    if (!d)
        return;
    d->tk.count = n;
    d->tk.title = names;
    d->tk.sub = calloc(n ? n : 1, sizeof(char *));
    d->tk.path = calloc(n ? n : 1, sizeof(char *));
    d->fsym = icons;
    if (n <= 0)
    {
        lib_empty(scr, str(LANG_N3_NO_ENTRIES));
        return;
    }
    vrows_new(scr, 74, 406, d, n, 48, 'F', A_DB);
}

void lib_db_build_root(lv_obj_t *scr)
{
    be_db_root();
    lib_db_build(scr);
}

static void lib_db_delete_cb(lv_event_t *e)
{
    (void)e;
    be_db_back();
    /* tagtree points tree_context->dirfilter at its own SHOW_ID3DB; restore
     * the file-browser filter so later file context menus behave normally */
    tree_get_context()->dirfilter = &global_settings.dirfilter;
}

static void lib_db_push_level(void)
{
    if (shell_nav_full())
        return;
    const char *title = be_db_title();
    lv_obj_t *scr = lib_screen((title && title[0]) ? title
                                                   : str(LANG_TAGCACHE));
    lv_obj_add_event_cb(scr, lib_db_delete_cb, LV_EVENT_DELETE, NULL);
    shell_nav_push(scr);
    lib_db_build(scr);
}

/* ---- first-run database build ------------------------------------------- */
/* Mirrors the classic GO_TO_DBBROWSER branch in root_menu.c: entering the
 * database without a usable index asks the user once (gui_syncyesno in
 * the original), starts the build via be_db_rebuild() and polls
 * be_db_stat() until be_db_usable(), showing the same
 * numbers the original splash did (files scanned / commit step). */
static lv_obj_t *lib_root_scr;
static lv_obj_t *lib_prog_label;
static lv_obj_t *lib_prog_bar;
static lv_timer_t *lib_prog_timer;
static bool lib_asked;   /* asked once this session (reinit_attempted) */

static void lib_root_delete_cb(lv_event_t *e)
{
    (void)e;
    lib_root_scr = NULL;
    lib_prog_label = NULL;
    lib_prog_bar = NULL;
    if (lib_prog_timer)
    {
        lv_timer_del(lib_prog_timer);
        lib_prog_timer = NULL;
    }
}

static bool lib_db_ready(void)
{
    return be_db_usable();
}

/* 500ms tick -- same polling cadence as the classic GO_TO_DBBROWSER
 * loop (action_userabort(HZ/2)). Progress display mirrors it:
 *   commit_step > 0        -> "Initializing DB [n/m]"  (real %)
 *   otherwise              -> "Building database: N files scanned"
 * No invented progress: while scanning there is no total, so the bar
 * stays hidden; it only appears for the real commit step count. */
static void lib_prog_tick(lv_timer_t *t)
{
    struct tagcache_stat *st = be_db_stat();
    if (!st)
        return;

    if (be_db_usable())
    {
        lv_timer_del(t);
        lib_prog_timer = NULL;
        lv_obj_t *scr = lib_root_scr;
        if (!scr)
            return;
        if (lib_prog_label)
        {
            lv_obj_del(lib_prog_label);   /* timer context: sync del ok */
            lib_prog_label = NULL;
        }
        if (lib_prog_bar)
        {
            lv_obj_del(lib_prog_bar);
            lib_prog_bar = NULL;
        }
        lib_db_build_root(scr);
        return;
    }

    if (st->commit_delayed)
    {
        /* classic behavior: delayed commit needs a reboot */
        if (lib_prog_bar)
            lv_obj_add_flag(lib_prog_bar, LV_OBJ_FLAG_HIDDEN);
        if (lib_prog_label)
            lv_label_set_text(lib_prog_label,
                              str(LANG_N3_DB_UPDATE_DELAYED));
        lv_timer_del(t);
        lib_prog_timer = NULL;
        return;
    }

    if (!st->readyvalid)
    {
        if (lib_prog_label)
            lv_label_set_text(lib_prog_label, str(LANG_N3_DB_PREPARING));
        if (lib_prog_bar)
            lv_obj_add_flag(lib_prog_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (st->commit_step > 0)
    {
        int max = be_db_max_commit_step();
        if (max < 1)
            max = 1;
        if (lib_prog_label)
            lv_label_set_text_fmt(lib_prog_label, str(LANG_N3_DB_COMMITTING),
                                  st->commit_step, max);
        if (lib_prog_bar)
        {
            lv_obj_clear_flag(lib_prog_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_range(lib_prog_bar, 0, max);
            lv_bar_set_value(lib_prog_bar, st->commit_step, LV_ANIM_OFF);
        }
    }
    else
    {
        if (lib_prog_label)
            lv_label_set_text_fmt(lib_prog_label,
                                  str(LANG_N3_DB_SCANNING),
                                  tc_scan_files,
                                  st->curentry ? st->curentry : "");
        if (lib_prog_bar)
            lv_obj_add_flag(lib_prog_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lib_start_progress(lv_obj_t *scr)
{
    lib_prog_label = lv_label_create(scr);
    lv_label_set_text(lib_prog_label, str(LANG_N3_DB_PREPARING));
    lv_obj_set_style_text_color(lib_prog_label, shell_col_text(), 0);
    lv_obj_set_style_text_align(lib_prog_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lib_prog_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lib_prog_label, 320);
    lv_obj_align(lib_prog_label, LV_ALIGN_TOP_MID, 0, 130);

    lib_prog_bar = ui_progress_bar(scr, 30, 244, 300, 12, UI_BAR_DB);
    lv_bar_set_range(lib_prog_bar, 0, 100);
    lv_obj_add_flag(lib_prog_bar, LV_OBJ_FLAG_HIDDEN);  /* commit stage only */

    lib_prog_timer = lv_timer_create(lib_prog_tick, 500, NULL);
    lib_prog_tick(lib_prog_timer);
}

/* First-run ask: modal over the CURRENT screen (no navigation), mirroring
 * the classic "Initialize database now?" prompt. 暂不 just closes it;
 * 开始建立 closes it, pushes the library screen and shows the same
 * progress view re-entering during a build would show. */
static void lib_push_root(void)
{
    lv_obj_t *scr = lib_screen(str(LANG_TAGCACHE));
    lv_obj_add_event_cb(scr, lib_root_delete_cb, LV_EVENT_DELETE, NULL);
    lib_root_scr = scr;
    shell_nav_push(scr);
}

static void lib_confirm_start_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    shell_modal_close_ev(e);
    be_db_rebuild();          /* classic GO_TO_DBBROWSER path: Q_REBUILD */
    lib_asked = true;
    lib_push_root();
    lib_start_progress(lib_root_scr);
}

static void lib_confirm_cancel_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
        shell_modal_close_ev(e);
}

static bool lib_need_ask(void)
{
    if (lib_asked || be_db_usable())
        return false;
    struct tagcache_stat *st = be_db_stat();
    return st && st->readyvalid && !st->ready &&
           st->processed_entries == 0 && st->commit_step == 0;
}

static void lib_build_confirm(void)
{
    const shell_modal_btn_t btns[] =
    {
        { str(LANG_N3_NOT_NOW), false, lib_confirm_cancel_cb, NULL },
        { str(LANG_N3_START),   true,  lib_confirm_start_cb,  NULL },
        { NULL,                 false, NULL,                  NULL },
    };
    shell_modal_open(str(LANG_N3_BUILD_DB_Q), str(LANG_N3_BUILD_DB_DESC),
                     btns);
}

/* ---- file browser -------------------------------------------------------- */
/* Listing, filtering and sorting are done by rockbox's own ft_load()
 * (apps/filetree.c) so the file-view settings (sort_case, sort_dir,
 * sort_file, dirfilter, max_files_in_dir) are honored and the entry cache
 * stays compact and buflib-managed. We only render the resulting cache. */


static char g_files_dir[MAX_PATH];      /* current browser folder */

static void files_open_dir(const char *path)
{
    if (shell_nav_full())
        return;
    char **names, **paths, **icons;
    int n = be_dir_scan(path, &names, &paths, &icons);
    if (n < 0)
        return;
    strlcpy(g_files_dir, path, sizeof g_files_dir);
    strlcpy(files_last_dir,
            global_settings.keep_directory ? path : "/",
            sizeof files_last_dir);

    const char *base = strrchr(path, '/');
    const char *ttl = (base && base[1]) ? base + 1 : str(LANG_DIR_BROWSER);
    if (global_settings.show_path_in_browser == SHOW_PATH_FULL)
        ttl = (path[0] == '/' && path[1]) ? path + 1 : path;
    lv_obj_t *scr = lib_screen(ttl);
    shell_nav_push(scr);
    lib_data *d = lib_data_new(scr);

    d->tk.count = n;
    d->tk.title = names;
    d->tk.path = paths;
    d->tk.sub = calloc(n ? n : 1, sizeof(char *));
    d->fsym = icons;

    g_files_d = d;
    g_files_v = NULL;
    if (n == 0)
    {
        /* let a long press anywhere in an empty folder bring up the
         * directory menu (so e.g. paste is reachable) */
        lv_obj_t *em = lib_empty(scr, str(LANG_N3_EMPTY_FOLDER));
        lv_obj_set_size(em, 340, 380);
        lv_obj_align(em, LV_ALIGN_TOP_MID, 0, 78);
        lv_obj_set_style_text_align(em, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(em, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(em, files_empty_hold_cb, LV_EVENT_LONG_PRESSED, NULL);
    }
    else
        g_files_v = vrows_new(scr, 74, 406, d, n, 48, 'F', A_FILE);
}

/* Play every audio file in this folder, starting at the tapped one. Reuses
 * the already-loaded browser listing (no second directory read). */
static void files_play(lib_data *d, int idx)
{
    be_list_t t;
    memset(&t, 0, sizeof t);
    int cap = 32;
    t.title = malloc(cap * sizeof(char *));
    t.sub = malloc(cap * sizeof(char *));
    t.path = malloc(cap * sizeof(char *));
    int start = -1;

    for (int i = 0; i < d->tk.count && t.count < LIB_MAX_ITEMS; i++)
    {
        if (!d->fsym[i] || d->fsym[i][0] != 'a')
            continue;                  /* only audio files take part */
        if (t.count == cap)
        {
            cap *= 2;
            t.title = realloc(t.title, cap * sizeof(char *));
            t.sub = realloc(t.sub, cap * sizeof(char *));
            t.path = realloc(t.path, cap * sizeof(char *));
        }
        t.title[t.count] = strdup(d->tk.title[i]);
        t.sub[t.count] = NULL;
        t.path[t.count] = strdup(d->tk.path[i]);
        if (i == idx)
            start = t.count;
        t.count++;
    }

    if (t.count > 0 && start >= 0)
    {
        be_play(&t, start);
        shell_player_show();
    }
    be_list_free(&t);
}

static void files_reload_async(void *p)
{
    /* drop the stale folder screen, then re-read it into a fresh one.
     * Both steps live in a single callback on purpose: two separate
     * lv_async_call()s run LIFO (newest first), which would reopen the
     * folder *before* the pop and leave the old screen on top. */
    shell_nav_pop_now();
    files_open_dir((const char *)p);
    free(p);
}

/* the playlists-list analogue of the tree browser's reload */
static void pllist_reload_async(void *p)
{
    (void)p;
    shell_nav_pop_now();
    shell_playlists_show();
}

/* ---- LVGL context menu built from rockbox's own tree_onplay_menu --------
 * The items, order, visibility rules and handlers are the stock ones; only
 * the drawing is ours (a glass sheet) so the app keeps a single look. */

static lv_obj_t *g_ctx_root;
static lv_obj_t *g_ctx_top;    /* most recent ctx screen (nav top) */

static void ctx_do_delete(void *p);

typedef struct
{
    const struct menu_item_ex *menu;
    int idx;
    lv_obj_t *overlay;
} ctx_item_t;

static bool ctx_visible(const struct menu_item_ex *menu, int i)
{
    const struct menu_item_ex *item = menu->submenus[i];
    menu_callback_type cb;
    if (item->flags & (MENU_HAS_DESC | MENU_DYNAMIC_DESC))
        cb = item->callback_and_desc->menu_callback;
    else
        cb = item->menu_callback;
    if (!cb)
        return true;
    return cb(ACTION_REQUEST_MENUITEM, item, NULL) != ACTION_EXIT_MENUITEM;
}

static const char *ctx_label(const struct menu_item_ex *menu, int i,
                             char *buf, size_t bufsz)
{
    const struct menu_item_ex *item = menu->submenus[i];
    if (item->flags & MENU_DYNAMIC_DESC)
        return item->menu_get_name_and_icon->list_get_name(i,
                   item->menu_get_name_and_icon->list_get_name_data,
                   buf, bufsz);
    return P2STR(item->callback_and_desc->desc);
}

static void ctx_scrim_cb(lv_event_t *e);
static void ctx_row_cb(lv_event_t *e);
static void ctx_row_free_cb(lv_event_t *e);

static void ctx_open(const struct menu_item_ex *menu, lv_obj_t *parent)
{
    int count = MENU_GET_COUNT(menu->flags);

    const char *title = (menu->flags & MENU_HAS_DESC)
        ? P2STR(menu->callback_and_desc->desc) : str(LANG_N3_OPTION);
    lv_obj_t *list;
    lv_obj_t *scr = shell_menu_screen_push(title, &list, ctx_scrim_cb);
    if (!parent)
        g_ctx_root = scr;
    g_ctx_top = scr;

    for (int i = 0; i < count; i++)
    {
        const struct menu_item_ex *item = menu->submenus[i];
        int type = item->flags & MENU_TYPE_MASK;
        if (type != MT_MENU && type != MT_FUNCTION_CALL &&
            type != MT_FUNCTION_CALL_W_PARAM)
            continue;
        if (!ctx_visible(menu, i))
            continue;
        char lbuf[128];
        const char *lab = ctx_label(menu, i, lbuf, sizeof lbuf);
        if (!lab || !lab[0])
            continue;

        char icb[8];
        lvgui_first_char(lab, icb, sizeof icb);
        lv_obj_t *row = shell_menu_row_new(list, icb[0] ? icb : NULL, false,
                                           lab, type == MT_MENU, false, 0,
                                           NULL, 3);

        ctx_item_t *ci = malloc(sizeof *ci);
        ci->menu = menu;
        ci->idx = i;
        ci->overlay = scr;
        lv_obj_add_event_cb(row, ctx_row_cb, LV_EVENT_CLICKED, ci);
        lv_obj_add_event_cb(row, ctx_row_free_cb, LV_EVENT_DELETE, ci);
    }
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

static void ctx_scrim_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_current_target(e);
    if (scr == g_ctx_top)
        g_ctx_top = NULL;
    if (scr == g_ctx_root)
        g_ctx_root = NULL;
}

static void ctx_row_free_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

/* run the stock onplay handler for one item (job context) */
static void ctx_stock_job(void *p)
{
    const struct menu_item_ex *item = p;
    int type = item->flags & MENU_TYPE_MASK;
    int res = ONPLAY_OK;

    if (type == MT_FUNCTION_CALL_W_PARAM)
        res = item->function_param->function_w_param(
                                          item->function_param->param);
    else if (type == MT_FUNCTION_CALL)
        res = item->function->function();
    else
        return;

    if (!(item->flags & MENU_FUNC_CHECK_RETVAL) || res == ONPLAY_OK)
        res = onplay_result_code();
    lv_obj_invalidate(lv_scr_act());

    if (res == ONPLAY_START_PLAY)
        shell_player_show();
    else if (res == ONPLAY_PLAYLIST)
        shell_playlist_show();
    else if (res == ONPLAY_RELOAD_DIR && g_ctx_pllist)
        lv_async_call(pllist_reload_async, NULL);
    else if (res == ONPLAY_RELOAD_DIR && g_files_dir[0])
    {
        char dir[MAX_PATH];
        strlcpy(dir, g_files_dir, sizeof dir);
        lv_async_call(files_reload_async, strdup(dir));
    }
}

/* close the ctx screen the action came from (unless a job already
 * pushed another screen, e.g. the player) */
static void ctx_after_action(void *p)
{
    (void)p;
    if (g_ctx_top && lv_scr_act() == g_ctx_top)
        shell_nav_pop_now();
    int res = onplay_result_code();
    if (res == ONPLAY_START_PLAY)
        shell_player_show();
    else if (res == ONPLAY_PLAYLIST)
        shell_playlist_show();
    else if (res == ONPLAY_RELOAD_DIR && g_ctx_pllist)
        lv_async_call(pllist_reload_async, NULL);
    else if (res == ONPLAY_RELOAD_DIR && g_files_dir[0])
    {
        char dir[MAX_PATH];
        strlcpy(dir, g_files_dir, sizeof dir);
        lv_async_call(files_reload_async, strdup(dir));
    }
    else
        lv_obj_invalidate(lv_scr_act());
}

static void ctx_row_cb(lv_event_t *e)
{
    ctx_item_t *ci = lv_event_get_user_data(e);
    const struct menu_item_ex *item = ci->menu->submenus[ci->idx];
    int type = item->flags & MENU_TYPE_MASK;

    /* properties / delete: our own native LVGL implementation (the stock
     * handlers would fall back to the classic GUI) */
    int special = onplay_item_kind(item);
    if (special == 1)
    {
        lv_async_call(ctx_do_props, NULL);
        return;
    }
    if (special == 2)
    {
        lv_async_call(ctx_do_delete, NULL);
        return;
    }

    if (type == MT_MENU)
    {
        ctx_open(item, ci->overlay);
        return;
    }
    /* every other item keeps its ORIGINAL handler; deferred to the top of
     * the UI loop (see ctx_stock_job) */
    if (type == MT_FUNCTION_CALL || type == MT_FUNCTION_CALL_W_PARAM)
    {
        lv_async_call(ctx_after_action, NULL);      /* close the menu */
        lvgui_run_job(ctx_stock_job, (void *)item);
    }
}

/* ---- native properties / delete (replace the classic-GUI handlers) ------- */

static void files_reload_async(void *p);




static void props_row(lv_obj_t *list, const char *label, const char *value)
{
    if (!value || !value[0])
        return;
    lv_obj_t *row = shell_glass_new(list);
    lv_obj_set_size(row, 340, 40);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, th->menu_row_opa, 0);
    lv_obj_set_style_border_opa(row, th->chip_border_opa, 0);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, shell_col_dim(), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *v = lv_label_create(row);
    char cv[256];
    lv_label_set_text(v, shell_cs(cv, sizeof cv, value));
    lv_obj_set_style_text_color(v, shell_col_text(), 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v, 210);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -12, 0);
}

static void props_show(const char *path, bool isdir)
{
    if (shell_nav_full())
        return;
    lv_obj_t *scr = lib_screen(str(LANG_PROPERTIES));
    shell_nav_push(scr);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 360, 396);
    lv_obj_set_pos(list, 0, 74);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 10, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    const char *base = strrchr(path, '/');
    base = (base && base[1]) ? base + 1 : path;

    unsigned long size = 0;
    time_t mt = 0;
    be_file_info(path, &size, &mt);

    char b[64];
    props_row(list, str(LANG_PROPERTIES_PATH), path);
    props_row(list, str(LANG_PROPERTIES_FILENAME), base);
    if (size >= 1048576)
        snprintf(b, sizeof b, "%.1f MB", (double)size / 1048576.0);
    else
        snprintf(b, sizeof b, "%lu KB", (size + 1023) / 1024);
    props_row(list, str(LANG_PROPERTIES_SIZE), b);

    struct tm *tm = localtime(&mt);
    if (tm)
    {
        snprintf(b, sizeof b, "%04d/%02d/%02d", tm->tm_year + 1900,
                 tm->tm_mon + 1, tm->tm_mday);
        props_row(list, str(LANG_PROPERTIES_DATE), b);
        snprintf(b, sizeof b, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min,
                 tm->tm_sec);
        props_row(list, str(LANG_PROPERTIES_TIME), b);
    }

    if (!isdir && be_is_audio(base))
    {
        static struct mp3entry id3;
        memset(&id3, 0, sizeof id3);
        int fd = open(path, O_RDONLY);
        if (fd >= 0)
        {
            bool ok = get_metadata(&id3, fd, path);
            close(fd);
            if (ok)
            {
                props_row(list, str(LANG_ID3_TITLE), id3.title);
                props_row(list, str(LANG_ID3_ARTIST), id3.artist);
                props_row(list, str(LANG_ID3_ALBUM), id3.album);
                props_row(list, str(LANG_ID3_GENRE), id3.genre_string);
                if (id3.year)
                {
                    snprintf(b, sizeof b, "%d", id3.year);
                    props_row(list, str(LANG_ID3_YEAR), b);
                }
                snprintf(b, sizeof b, "%ld:%02ld", (long)(id3.length / 60000),
                         (long)((id3.length / 1000) % 60));
                props_row(list, str(LANG_ID3_LENGTH), b);
                snprintf(b, sizeof b, "%u kbps", id3.bitrate);
                props_row(list, str(LANG_ID3_BITRATE), b);
                snprintf(b, sizeof b, "%lu Hz", id3.frequency);
                props_row(list, str(LANG_ID3_FREQUENCY), b);
            }
        }
    }
}

static void ctx_close(void)
{
    if (g_ctx_root)
    {
        shell_nav_pop_now();
        g_ctx_root = NULL;
    }
}

static void ctx_do_props(void *p)
{
    (void)p;
    ctx_close();
    props_show(g_ctx_path, g_ctx_isdir);
}


/* ---- blocking playlist picker (display_playlists stand-in) ---------------
 * Lists the catalog playlists in our UI and blocks until one is picked;
 * the stock "add to existing playlist" flow calls this instead of the
 * classic select-only file browser. Returns 0 with the chosen path copied
 * into out, -1 on cancel. Must run from a lvgui_run_job() context. */
static bool g_plpick_done;
static bool g_plpick_ok;
static char *g_plpick_out;
static size_t g_plpick_outsz;

static void plpick_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (!g_plpick_done)
    {
        g_plpick_ok = false;
        g_plpick_done = true;
    }
}

static void plpick_row_cb(lv_event_t *e)
{
    const char *path = lv_event_get_user_data(e);
    if (g_plpick_out && path)
        strlcpy(g_plpick_out, path, g_plpick_outsz);
    g_plpick_ok = true;                 /* buffer freed on row DELETE */
    g_plpick_done = true;
    shell_nav_pop();                    /* deferred: event-cb safe */
}

static void plpick_row_free_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

int shell_plcat_pick(char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return -1;
    g_plpick_done = false;
    g_plpick_ok = false;
    g_plpick_out = out;
    g_plpick_outsz = outsz;

    char dir[MAX_PATH];
    catalog_get_directory(dir, sizeof dir);
    if (!dir[0])
        strlcpy(dir, "/Playlists", sizeof dir);

    /* collect the catalog playlists in one pass (no rewinddir in rockbox) */
    int cap = 16, n = 0;
    char **tt = malloc(cap * sizeof(char *));
    char **pp = malloc(cap * sizeof(char *));
    DIR *dp = opendir(dir);
    if (dp)
    {
        struct dirent *en;
        while ((en = readdir(dp)) != NULL && n < LIB_MAX_ITEMS)
        {
            if (!be_is_playlist_name(en->d_name))
                continue;
            if (n == cap)
            {
                cap *= 2;
                tt = realloc(tt, cap * sizeof(char *));
                pp = realloc(pp, cap * sizeof(char *));
            }
            char full[MAX_PATH];
            snprintf(full, sizeof full, "%s/%s", dir, en->d_name);
            pp[n] = strdup(full);
            char st[MAX_PATH];
            strlcpy(st, en->d_name, sizeof st);
            char *dot = strrchr(st, '.');
            if (dot)
                *dot = '\0';
            tt[n] = strdup(st);
            n++;
        }
        closedir(dp);
    }

    lv_obj_t *list;
    shell_menu_screen_push(str(LANG_PLAYLISTS), &list, plpick_cancel_cb);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    if (n > 0)
    {
        for (int i = 0; i < n; i++)
        {
            char icb[8];
            lvgui_first_char(tt[i], icb, sizeof icb);
            lv_obj_t *row = shell_menu_row_new(list, icb[0] ? icb : NULL,
                                               false, tt[i], false, false,
                                               0, NULL, 3);
            lv_obj_add_event_cb(row, plpick_row_cb, LV_EVENT_CLICKED, pp[i]);
            lv_obj_add_event_cb(row, plpick_row_free_cb, LV_EVENT_DELETE,
                                pp[i]);
            free(tt[i]);
        }
    }
    else
    {
        lv_obj_t *l = lv_label_create(list);
        lv_label_set_text(l, str(LANG_CATALOG_NO_PLAYLISTS));
        lv_obj_set_style_text_color(l, th->fg_text, 0);
        lv_obj_set_style_pad_left(l, 16, 0);
    }
    free(tt);
    free(pp);

    while (!g_plpick_done)
        lvgui_pump();

    lvgui_pump();                       /* let the deferred nav pop run */

    return g_plpick_ok ? 0 : -1;
}

/* Drop one entry from the on-screen file list, keeping every other row
 * exactly where it was (a full re-read would re-sort the folder). */
static void files_drop_row(int idx)
{
    lib_data *d = g_files_d;
    if (!d || idx < 0 || idx >= d->tk.count)
        return;
    free(d->tk.title[idx]);
    free(d->tk.sub[idx]);
    free(d->tk.path[idx]);
    if (d->fsym)
        free(d->fsym[idx]);
    for (int i = idx; i < d->tk.count - 1; i++)
    {
        d->tk.title[i] = d->tk.title[i + 1];
        d->tk.sub[i] = d->tk.sub[i + 1];
        d->tk.path[i] = d->tk.path[i + 1];
        if (d->fsym)
            d->fsym[i] = d->fsym[i + 1];
    }
    d->tk.count--;

    vlist_t *v = g_files_v;
    if (!v)
        return;
    vrows_t *vr = v->ud;
    if (vr->count > 0)
        vr->count--;
    v->total_h -= vr->row_h;
    if (v->total_h < 0)
        v->total_h = 0;
    int max = v->total_h - v->view_h;
    if (max < 0)
        max = 0;
    if (v->off > max)
        v->off = max;
    if (v->off < 0)
        v->off = 0;
    for (int k = 0; k < vr->n; k++)
    {
        /* force every row to re-bind, and hide them all first so a row
         * that scrolls out of range cannot linger with stale content */
        vr->rows[k].idx = -1;
        lv_obj_add_flag(vr->rows[k].wrap, LV_OBJ_FLAG_HIDDEN);
    }
    v->render(v);
}

/* delete: our confirm, our recursive remove, then drop just that row */
static void del_do_async(void *p)
{
    /* dismiss the confirm modal FIRST, right here: the playlists reload
     * below tears down the whole screen (scrim included), and a separate
     * deferred modal delete would then run on freed memory (lv_async_call
     * is LIFO - see shell notes). */
    if (p)
        lv_obj_del((lv_obj_t *)p);
    if (be_delete(g_ctx_path) != 0)
        return;                         /* nothing was removed */
    int idx = -1;
    if (g_files_d)
        for (int i = 0; i < g_files_d->tk.count; i++)
            if (g_files_d->tk.path[i] &&
                strcmp(g_files_d->tk.path[i], g_ctx_path) == 0)
            {
                idx = i;
                break;
            }
    if (idx >= 0)
        files_drop_row(idx);
    else if (g_ctx_pllist)
        lv_async_call(pllist_reload_async, NULL);
    else if (g_files_dir[0])
    {
        char dir[MAX_PATH];
        strlcpy(dir, g_files_dir, sizeof dir);
        lv_async_call(files_reload_async, strdup(dir));
    }
}

static void del_go_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *scrim = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_async_call(del_do_async, scrim);
}

static void our_delete(const char *path, bool isdir)
{
    strlcpy(g_ctx_path, path, sizeof g_ctx_path);
    char body[MAX_PATH + 16];
    snprintf(body, sizeof body, "%s\n\n%s",
             isdir ? str(LANG_DELETE_DIR) : str(LANG_DELETE), path);
    const shell_modal_btn_t btns[] = {
        { str(LANG_CANCEL), false, shell_modal_close_ev, NULL },
        { str(LANG_DELETE), true, del_go_cb, NULL },
        { NULL, false, NULL, NULL }
    };
    shell_modal_open(str(LANG_DELETE), body, btns);
}

static void ctx_do_delete(void *p)
{
    (void)p;
    ctx_close();
    our_delete(g_ctx_path, g_ctx_isdir);
}

/* Long-press a file-browser row: open our own context menu whose items and
 * handlers are exactly the stock tree_onplay_menu ones. */
static void files_hold(lib_data *d, int idx)
{
    const char *path = d->tk.path[idx];
    if (!path)
        return;

    int attr = filetype_get_attr(path);
    if (d->fsym && d->fsym[idx] && d->fsym[idx][0] == 'd')
        attr |= ATTR_DIRECTORY;

    g_ctx_pllist = false;
    strlcpy(g_ctx_path, path, sizeof g_ctx_path);
    g_ctx_isdir = (attr & ATTR_DIRECTORY) != 0;

    ctx_dirfilter_begin(global_settings.dirfilter);
    onplay_prepare(path, attr);
    ctx_open(onplay_tree_menu(), NULL);
    if (g_ctx_root)
        lv_obj_add_event_cb(g_ctx_root, ctx_dirfilter_restore_cb,
                            LV_EVENT_DELETE, NULL);
}

/* Long-press a row of the playlists list: same stock onplay menu as a
 * playlist file in the tree browser, under the stock catalog's SHOW_M3U
 * dirfilter context. */
static void plfile_hold(lib_data *d, int idx)
{
    const char *path = d->tk.path[idx];
    if (!path)
        return;

    ctx_dirfilter_begin(SHOW_M3U);

    g_ctx_pllist = true;
    g_ctx_isdir = false;
    strlcpy(g_ctx_path, path, sizeof g_ctx_path);

    onplay_prepare(path, filetype_get_attr(path));
    ctx_open(onplay_tree_menu(), NULL);
    if (g_ctx_root)
        lv_obj_add_event_cb(g_ctx_root, ctx_dirfilter_restore_cb,
                            LV_EVENT_DELETE, NULL);
}

/* Context menu for the current folder itself (used in an empty folder,
 * where there is no row to long press). */
static void files_dir_menu(void)
{
    ctx_dirfilter_begin(global_settings.dirfilter);
    onplay_prepare(g_files_dir, ATTR_DIRECTORY);
    g_ctx_isdir = true;
    g_ctx_pllist = false;
    strlcpy(g_ctx_path, g_files_dir, sizeof g_ctx_path);
    ctx_open(onplay_tree_menu(), NULL);
    if (g_ctx_root)
        lv_obj_add_event_cb(g_ctx_root, ctx_dirfilter_restore_cb,
                            LV_EVENT_DELETE, NULL);
}

static void files_empty_hold_cb(lv_event_t *e)
{
    (void)e;
    files_dir_menu();
}

/* Home "Playlists" row: list the playlist-catalog contents (.m3u/.m3u8)
 * and play the tapped one. */
void shell_playlists_show(void)
{
    if (shell_nav_full())
        return;
    char **tt, **pp;
    int n = be_playlists_scan(&tt, &pp);

    lv_obj_t *scr = lib_screen(str(LANG_PLAYLISTS));
    shell_nav_push(scr);
    lib_data *d = lib_data_new(scr);
    d->tk.count = n;
    d->tk.title = tt;
    d->tk.sub = calloc(n ? n : 1, sizeof(char *));
    d->tk.path = pp;
    if (n == 0)
    {
        be_str_arrays_free(tt, pp, n);
        d->tk.title = NULL;
        d->tk.path = NULL;
        lib_empty(scr, str(LANG_CATALOG_NO_PLAYLISTS));
    }
    else
        vrows_new(scr, 74, 406, d, n, 48, 'P', A_PLPLAY);
}

void shell_files_show(void)
{
    files_open_dir(global_settings.browse_current ? files_last_dir : "/");
}

void library_open(void)
{
    if (lib_root_scr)
    {
        /* already open (timer alive): just show it again */
        if (!shell_nav_full())
            shell_nav_push(lib_root_scr);
        return;
    }

    /* first-run ask pops a modal over the CURRENT screen, no navigation */
    if (lib_need_ask())
    {
        lib_build_confirm();
        return;
    }

    lib_push_root();

    if (lib_db_ready())
    {
        lib_db_build_root(lib_root_scr);
        return;
    }

    struct tagcache_stat *st = be_db_stat();
    if (st && st->commit_delayed)
    {
        /* classic: commit was delayed, reboot needed */
        lv_obj_t *l = lv_label_create(lib_root_scr);
        lv_label_set_text(l, str(LANG_N3_DB_UPDATE_DELAYED));
        lv_obj_set_style_text_color(l, shell_col_text(), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, 320);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 150);
        return;
    }

    /* status unknown or a build/commit already under way: progress view */
    lib_start_progress(lib_root_scr);
}
