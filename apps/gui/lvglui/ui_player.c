/* ui_player.c — M3 now-playing screen (WPS equivalent, LVGL edition)
 *
 * Follows the approved design mock (brainstorm 587, screen 21/21b):
 * immersive status bar (battery only), square art card with a "setting
 * sun" placeholder, centered title block, thin seek bar and round
 * transport (repeat / prev / big play-pause / next / volume) — all
 * functional — plus a bottom-sheet settings drawer and a side volume
 * popup.  The art card is a pager: swipe left/right on the card to
 * cycle art -> lyrics -> track info; the transport row stays put. */
#include "config.h"
#ifdef HAVE_LVGL_UI

#include <math.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "lvgl_ui.h"
#include "ui_backend.h"
#include "audio.h"
#include "metadata.h"
#include "settings.h"
#include "lang.h"
#include "sound.h"
#include "powermgmt.h"
#include "playlist.h"
#include "timefuncs.h"
#include "string-extra.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* geometry (360x480) — square art card */
#define PB_X     35
#define PB_Y     34
#define PB_W     290
#define PB_H     290
#define PGR_H    340          /* pager region: cover + title, above seek bar */

/* a swipe also fires CLICKED on a button under the finger; ignore clicks
 * that arrive this soon after a gesture was seen */
#define GEST_MS  400

#define LR_MAX_LINES 512
#define LR_TXT       96
#define LR_ROWS      8             /* lyric lines shown inside the 220 card */

#define INFO_ROWS    9
#define INFO_ROW_H   26

typedef struct
{
    lv_obj_t *scr;
    lv_obj_t *banner;          /* square art card (pager host) */
    lv_obj_t *artpg;           /* page 0: cover card + title */
    lv_obj_t *cardc;           /* the square cover card inside page 0 */
    lv_obj_t *ph;              /* placeholder group: sun circles */
    lv_obj_t *tt;              /* album name over the card */
    lv_obj_t *img;             /* real art */
    lv_obj_t *lrc;             /* page 1: lyrics */
    lv_obj_t *infoc;           /* page 2: track info (scrollable) */
    int page;                  /* 0=art 1=lyrics 2=info */

    lv_obj_t *title;
    lv_obj_t *sub;
    lv_obj_t *bar;
    lv_obj_t *t_el;
    lv_obj_t *t_tot;
    lv_obj_t *btn_play;        /* label on the big button */
    lv_obj_t *rep_ic;          /* repeat loop-arrows symbol */
    lv_obj_t *rep_lines;       /* three-bar icon for sequential (off) */
    lv_obj_t *rep_badge;       /* "1" badge, shown in repeat-one */
    bool seeking;              /* user is dragging the seek bar */
    long seek_target;          /* pending seek position (ms), 0 = none */
    uint32_t gest_ts;          /* last gesture tick (button-click guard) */
    lv_point_t press_pt;       /* last transport-button press point */
    lv_obj_t *batt;
    lv_obj_t *clock;           /* top-left clock in the status row */

    lv_obj_t *vol_ovl;         /* volume side capsule overlay (lazy) */
    lv_obj_t *vol_slider;
    lv_obj_t *vol_val;

    lv_obj_t *drawer;          /* bottom quick-settings drawer */
    lv_obj_t *dr_scrim;        /* tap-outside-to-close scrim behind drawer */
    lv_obj_t *dr_rep;          /* repeat chip label */
    lv_obj_t *dr_shf;          /* shuffle chip label */

    lv_timer_t *timer;

    /* lyrics page */
    struct lr_line *lines;
    int lr_n, lr_cur, lr_last_cur;
    lv_obj_t *lr_lbl[LR_ROWS];
    lv_obj_t *lr_none;
    lv_timer_t *lr_timer;
    char lr_path[MAX_PATH];

    /* info page */
    lv_obj_t *inf_v[INFO_ROWS];

    lvui_art_t *art;
    char path[MAX_PATH];
} player_t;

typedef struct lr_line
{
    long ms;
    char txt[LR_TXT];
} lr_line;

static void player_tick_refresh(player_t *p);
static void fmt_time(char *buf, size_t sz, long ms);
static void cover_set_page(player_t *p, int page);
static lv_obj_t *g_scrim;
static void info_build(player_t *p);
static void drawer_open(player_t *p);
static void drawer_close(player_t *p);

/* gesture guard helper: true if a click should be ignored because a
 * gesture just happened (the swipe also lands on the button) */
/* record where a transport button was pressed */
static void btn_press_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    lv_indev_t *ind = lv_indev_get_act();
    if (ind)
        lv_indev_get_point(ind, &p->press_pt);
}

/* ignore a button release that was really a drag (e.g. pulling up the
 * drawer from on top of the transport row) */
static bool btn_dragged(player_t *p)
{
    lv_indev_t *ind = lv_indev_get_act();
    if (!ind)
        return false;
    lv_point_t c;
    lv_indev_get_point(ind, &c);
    int dx = c.x - p->press_pt.x, dy = c.y - p->press_pt.y;
    return (dx * dx + dy * dy) > (14 * 14);
}

static bool gest_blocked(player_t *p)
{
    return p && ((uint32_t)(lv_tick_get() - p->gest_ts) < GEST_MS
                 || btn_dragged(p));
}

/* ---- controls ------------------------------------------------------------ */
static void btn_prev_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    if (gest_blocked(p)) return;
    be_player_prev();
}

static void btn_play_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    if (gest_blocked(p)) return;
    be_player_toggle_pause();
}

static void btn_next_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    if (gest_blocked(p)) return;
    be_player_next();
}

/* swipe up: settings drawer, swipe down: close it */
static void scr_gesture_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    p->gest_ts = lv_tick_get();
    if (p->seeking)
        return;
    if (dir == LV_DIR_TOP && !p->drawer)
        drawer_open(p);
    else if (dir == LV_DIR_BOTTOM && p->drawer)
        drawer_close(p);
}

/* left/right on the art card cycles art -> lyrics -> info; the card
 * stops gesture bubbling so it receives these (up/down open the drawer) */
static void cover_gesture_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    p->gest_ts = lv_tick_get();
    if (dir == LV_DIR_LEFT)
        cover_set_page(p, (p->page + 1) % 3);
    else if (dir == LV_DIR_RIGHT)
        cover_set_page(p, (p->page + 2) % 3);
    else if (dir == LV_DIR_TOP && !p->drawer)
        drawer_open(p);
    else if (dir == LV_DIR_BOTTOM && p->drawer)
        drawer_close(p);
}

/* repeat: single key cycles Off -> All -> One (live-read by playlist);
 * icon: 顺序 = three bars, 全部循环 = two curved arrows (loop),
 * 单曲循环 = loop arrows + "1" badge */
static void player_set_repeat(player_t *p)
{
    int r = global_settings.repeat_mode;
    if (r == REPEAT_OFF)
    {
        lv_obj_clear_flag(p->rep_lines, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->rep_ic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->rep_badge, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(p->rep_lines, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(p->rep_ic, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(p->rep_badge, r == REPEAT_ONE ? "1" : "A");
        lv_obj_clear_flag(p->rep_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

LV_IMG_DECLARE(ui_icon_repeat);

static void btn_repeat_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    if (gest_blocked(p)) return;
    global_settings.repeat_mode = (global_settings.repeat_mode + 1) % 3;
    player_set_repeat(p);
}

/* ---- seek bar (draggable) ------------------------------------------------ */
static void seek_show_drag(player_t *p)
{
    struct be_player_info pi;
    be_player_info(&pi);
    if (!pi.active || pi.length <= 0)
        return;
    long pos = (long)(pi.length * lv_slider_get_value(p->bar) / 1000);
    char b[16];
    fmt_time(b, sizeof b, pos);
    lv_label_set_text(p->t_el, b);
}

static void seek_pressed_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    p->seeking = true;
    seek_show_drag(p);
}

static void seek_val_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    if (p->seeking)
        seek_show_drag(p);
}

static void seek_released_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    p->seeking = false;
    struct be_player_info pi;
    be_player_info(&pi);
    if (pi.active && pi.length > 0)
    {
        long pos = (long)(pi.length * lv_slider_get_value(p->bar) / 1000);
        p->seek_target = pos;
        be_player_seek(pos);
    }
}

/* ---- volume side capsule (HyperOS-style vertical slider) ---------------- */
static void vol_update_label(player_t *p)
{
    int v = global_status.volume;
    int lo = sound_min(SOUND_VOLUME), hi = sound_max(SOUND_VOLUME);
    int pct = hi > lo ? (v - lo) * 100 / (hi - lo) : 0;
    ui_volume_label(p->vol_val, pct);
}

static void vol_slider_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    int v = lv_slider_get_value(p->vol_slider);
    be_volume_set(v);
    global_status.volume = v;
    vol_update_label(p);
}

static void vol_scrim_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    lv_obj_del(p->vol_ovl);
    p->vol_ovl = NULL;
    p->vol_slider = NULL;
    p->vol_val = NULL;
}

#define VOL_W   44
#define VOL_H   216
#define VOL_X   (360 - VOL_W - 30)
#define VOL_Y   132

static void vol_toggle(player_t *p)
{
    if (p->vol_ovl)
    {
        lv_obj_del(p->vol_ovl);
        p->vol_ovl = NULL;
        p->vol_slider = NULL;
        p->vol_val = NULL;
        return;
    }

    /* scrim: tap outside closes */
    lv_obj_t *ovl = lv_obj_create(p->scr);
    lv_obj_set_size(ovl, 360, 480);
    lv_obj_set_pos(ovl, 0, 0);
    lv_obj_set_style_bg_opa(ovl, th->scrim_opa, 0);
    lv_obj_set_style_bg_color(ovl, th->scrim, 0);
    lv_obj_set_style_radius(ovl, 0, 0);
    lv_obj_set_style_border_width(ovl, 0, 0);
    shell_style_shadow_off(ovl);
    lv_obj_add_event_cb(ovl, vol_scrim_cb, LV_EVENT_CLICKED, p);
    p->vol_ovl = ovl;

    /* white translucent capsule sliding in from the right edge */
    lv_obj_t *cap = ui_volume_capsule(ovl, 360, VOL_Y, vol_slider_cb, p,
                                      &p->vol_val);
    p->vol_slider = lv_obj_get_child(cap, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cap);
    lv_anim_set_values(&a, 360, VOL_X);
    lv_anim_set_time(&a, 250);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void btn_vol_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    if (gest_blocked(p)) return;
    vol_toggle(p);
}

/* ---- bottom drawer: playback / sound quick settings -------------------- */
static void dr_close_done(lv_anim_t *a)
{
    player_t *p = a->user_data;
    if (p->drawer)
    {
        lv_obj_del(p->drawer);
        p->drawer = NULL;
    }
    if (p->dr_scrim)
    {
        lv_obj_del(p->dr_scrim);
        p->dr_scrim = NULL;
    }
}

static void drawer_close(player_t *p)
{
    if (!p->drawer)
        return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p->drawer);
    lv_anim_set_values(&a, lv_obj_get_y(p->drawer), 486);
    lv_anim_set_time(&a, 220);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, dr_close_done);
    a.user_data = p;
    lv_anim_start(&a);
}

static void dr_close_cb(lv_event_t *e)
{
    drawer_close(lv_event_get_user_data(e));
}

/* chip button; returns its (centered) label for live text updates */
static lv_obj_t *dr_chip(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         const char *txt, lv_event_cb_t cb, player_t *p)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, 148, 38);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, th->radius, 0);
    lv_obj_set_style_bg_opa(b, th->chip_opa, 0);
    lv_obj_set_style_bg_color(b, th->btn_secondary, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, th->border_white, 0);
    lv_obj_set_style_border_opa(b, th->chip_border_opa, 0);
    shell_style_shadow_off(b);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, th->fg_text, 0);
    lv_obj_center(l);
    lv_label_set_text(l, txt);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, p);
    return l;
}

static void dr_rep_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    global_settings.repeat_mode = (global_settings.repeat_mode + 1) % 3;
    player_set_repeat(p);
    static char names[3][48];
    static const int modes[3] = { LANG_OFF, LANG_ALL, LANG_REPEAT_ONE };
    for (int i = 0; i < 3; i++)
        snprintf(names[i], sizeof names[i], "%s · %s", str(LANG_REPEAT),
                 str(modes[i]));
    lv_label_set_text(p->dr_rep, names[global_settings.repeat_mode]);
}

static void dr_shf_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    global_settings.playlist_shuffle = !global_settings.playlist_shuffle;
    be_playlist_shuffle_apply(global_settings.playlist_shuffle);
    char b[48];
    snprintf(b, sizeof b, "%s · %s", str(LANG_SHUFFLE),
             global_settings.playlist_shuffle ? str(LANG_SET_BOOL_YES)
                                              : str(LANG_SET_BOOL_NO));
    lv_label_set_text(p->dr_shf, b);
}

static void dr_vol_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    drawer_close(p);
    vol_toggle(p);
}

static void dr_pl_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    drawer_close(p);
    shell_playlist_show();
}

static void drawer_open(player_t *p)
{
    if (p->drawer)
        return;

    /* full-screen scrim behind the sheet: a tap outside the drawer
     * dismisses it (created first so the drawer stacks on top) */
    lv_obj_t *sc = lv_obj_create(p->scr);
    lv_obj_set_size(sc, 360, 480);
    lv_obj_set_pos(sc, 0, 0);
    lv_obj_set_style_bg_opa(sc, th->scrim_opa, 0);
    lv_obj_set_style_bg_color(sc, th->scrim, 0);
    lv_obj_set_style_radius(sc, 0, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    shell_style_shadow_off(sc);
    lv_obj_clear_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sc, dr_close_cb, LV_EVENT_CLICKED, p);
    p->dr_scrim = sc;

    lv_obj_t *d = lv_obj_create(p->scr);
    lv_obj_set_size(d, 360, 224);           /* full width, bottom clipped */
    lv_obj_set_pos(d, 0, 486);              /* starts below screen */
    lv_obj_set_style_bg_opa(d, th->menu_card_opa, 0);     /* dark frosted glass */
    lv_obj_set_style_bg_color(d, th->menu_bg, 0);
    lv_obj_set_style_radius(d, th->radius_big, 0);      /* rounded top corners only */
    lv_obj_set_style_border_width(d, 1, 0);
    lv_obj_set_style_border_color(d, th->border_white, 0);
    lv_obj_set_style_border_opa(d, th->menu_border_opa, 0);
    lv_obj_set_style_clip_corner(d, true, 0);
    shell_style_shadow_off(d);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    p->drawer = d;

    lv_obj_t *hd = lv_obj_create(d);
    lv_obj_set_size(hd, 36, 4);
    lv_obj_set_pos(hd, 162, 8);
    lv_obj_set_style_bg_opa(hd, th->menu_row_opa, 0);
    lv_obj_set_style_bg_color(hd, th->fg_dim, 0);
    lv_obj_set_style_radius(hd, 2, 0);
    lv_obj_set_style_border_width(hd, 0, 0);
    shell_style_shadow_off(hd);
    lv_obj_clear_flag(hd, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(d);
    lv_label_set_text(t, str(LANG_N3_QUICK_SETTINGS));
    lv_obj_set_style_text_color(t, th->fg_text, 0);
    lv_obj_set_pos(t, 16, 22);

    /* playback settings */
    char rb[3][48], sb[48];
    static const int modes[3] = { LANG_OFF, LANG_ALL, LANG_REPEAT_ONE };
    for (int i = 0; i < 3; i++)
        snprintf(rb[i], sizeof rb[i], "%s · %s", str(LANG_REPEAT),
                 str(modes[i]));
    snprintf(sb, sizeof sb, "%s · %s", str(LANG_SHUFFLE),
             global_settings.playlist_shuffle ? str(LANG_SET_BOOL_YES)
                                              : str(LANG_SET_BOOL_NO));
    p->dr_rep = dr_chip(d, 16, 60, rb[global_settings.repeat_mode],
                        dr_rep_cb, p);
    p->dr_shf = dr_chip(d, 196, 60, sb, dr_shf_cb, p);

    /* sound settings */
    dr_chip(d, 16, 112, str(LANG_VOLUME), dr_vol_cb, p);
    dr_chip(d, 196, 112, str(LANG_PLAYLIST), dr_pl_cb, p);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_values(&a, 486, 300);
    lv_anim_set_time(&a, 250);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ---- card pager: art / lyrics / info ------------------------------------ */
static void cover_set_page(player_t *p, int page)
{
    p->page = page;
    if (page == 0)
    {
        lv_obj_clear_flag(p->artpg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->lrc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->infoc, LV_OBJ_FLAG_HIDDEN);
    }
    else if (page == 1)
    {
        lv_obj_add_flag(p->artpg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(p->lrc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->infoc, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(p->artpg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->lrc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(p->infoc, LV_OBJ_FLAG_HIDDEN);
        info_build(p);
    }
}

/* ---- lifecycle ----------------------------------------------------------- */
static void player_delete_cb(lv_event_t *e)
{
    player_t *p = lv_event_get_user_data(e);
    if (p->timer)
        lv_timer_del(p->timer);
    if (p->lr_timer)
        lv_timer_del(p->lr_timer);
    free(p->lines);
    lvui_art_free(p->art);
    free(p);
}

static void fmt_time(char *buf, size_t sz, long ms)
{
    long s = ms / 1000;
    snprintf(buf, sz, "%ld:%02ld", s / 60, s % 60);
}

static const char *batt_sym(int lvl)
{
    if (lvl > 87) return LV_SYMBOL_BATTERY_FULL;
    if (lvl > 62) return LV_SYMBOL_BATTERY_3;
    if (lvl > 37) return LV_SYMBOL_BATTERY_2;
    if (lvl > 12) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void player_tick(lv_timer_t *t)
{
    player_tick_refresh(t->user_data);
}

static void player_set_art(player_t *p)
{
    lvui_art_free(p->art);
    p->art = NULL;
    if (p->img)
    {
        lv_obj_del(p->img);     /* recreating is simplest */
        p->img = NULL;
    }
    if (!p->path[0])
        return;
    /* decode the cover at the card's size and let LVGL scale it to the box */
    p->art = lib_track_art_sized(p->path, 240, 240);
    if (!p->art)
    {
        lv_obj_clear_flag(p->ph, LV_OBJ_FLAG_HIDDEN);
        return;                 /* placeholder: sun + album name */
    }
    lv_obj_add_flag(p->ph, LV_OBJ_FLAG_HIDDEN);
    p->img = lv_img_create(p->cardc);
    lv_img_set_src(p->img, &p->art->dsc);
    lv_img_set_zoom(p->img, (uint16_t)(PB_W * 256 / 240));
    lv_obj_align(p->img, LV_ALIGN_CENTER, 0, 0);
    if (g_scrim)
        lv_obj_move_foreground(g_scrim);
    lv_obj_clear_flag(p->img, LV_OBJ_FLAG_CLICKABLE);
}

static void player_tick_refresh(player_t *p)
{
    struct be_player_info pi;
    be_player_info(&pi);

    if (!pi.active)
    {
        lv_label_set_text(p->title, str(LANG_N3_NOTHING_PLAYING));
        lv_label_set_text(p->sub, "");
        lv_label_set_text(p->tt, "");
        lv_obj_clear_flag(p->ph, LV_OBJ_FLAG_HIDDEN);
        lv_slider_set_value(p->bar, 0, LV_ANIM_OFF);
        p->seeking = false;
        p->seek_target = 0;
        lv_label_set_text(p->t_el, "0:00");
        lv_label_set_text(p->t_tot, "0:00");
        lv_label_set_text(p->btn_play, LV_SYMBOL_PLAY);
        if (p->img)
        {
            lv_obj_del(p->img);
            p->img = NULL;
        }
        lvui_art_free(p->art);
        p->art = NULL;
        p->path[0] = '\0';
        return;
    }

    /* track change: refresh text + cover + info page */
    if (strcmp(p->path, pi.path) != 0)
    {
        strlcpy(p->path, pi.path, sizeof p->path);

        lv_label_set_text(p->title, pi.title);
        char sub[MAX_PATH];
        sub[0] = '\0';
        if (pi.artist[0])
            strlcpy(sub, pi.artist, sizeof sub);
        if (pi.album[0])
        {
            if (sub[0])
                snprintf(sub + strlen(sub), sizeof sub - strlen(sub),
                         " · %s", pi.album);
            else
                strlcpy(sub, pi.album, sizeof sub);
        }
        lv_label_set_text(p->sub, sub);
        lv_label_set_text(p->tt, pi.album);
        player_set_art(p);
        if (p->page == 2)
            info_build(p);
    }

    long len = pi.length;
    long el = pi.elapsed;
    if (!p->seeking)
    {
        if (p->seek_target)
        {
            if (labs(el - p->seek_target) < 1500)
                p->seek_target = 0;      /* seek landed */
            else
                el = p->seek_target;     /* hold the bar at target */
        }
        if (len > 0)
            lv_slider_set_value(p->bar, (int32_t)(el * 1000 / len),
                                LV_ANIM_OFF);
        char b[16];
        fmt_time(b, sizeof b, el);
        lv_label_set_text(p->t_el, b);
    }
    char bt[16];
    fmt_time(bt, sizeof bt, len);
    lv_label_set_text(p->t_tot, bt);
    lv_label_set_text(p->btn_play,
                      pi.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);

    /* volume capsule follows the physical volume keys */
    if (p->vol_slider &&
        lv_slider_get_value(p->vol_slider) != global_status.volume)
    {
        lv_slider_set_value(p->vol_slider, global_status.volume, LV_ANIM_OFF);
        vol_update_label(p);
    }
}

/* ---- lyrics page (LRC, reworked from the lrcplayer plugin idea) --------- */
static void lr_add(player_t *p, long ms, const char *txt)
{
    if (p->lr_n >= LR_MAX_LINES)
        return;
    int i = p->lr_n;
    while (i > 0 && p->lines[i - 1].ms > ms)
    {
        p->lines[i] = p->lines[i - 1];
        i--;
    }
    p->lines[i].ms = ms;
    shell_cs(p->lines[i].txt, LR_TXT, txt);   /* lrc may be GBK */
    p->lr_n++;
}

static void lr_parse(player_t *p, const char *fname)
{
    FILE *f = fopen(fname, "r");
    if (!f)
        return;
    char buf[256];
    while (p->lr_n < LR_MAX_LINES && fgets(buf, sizeof buf, f))
    {
        char *q = buf;
        if ((unsigned char)q[0] == 0xEF && (unsigned char)q[1] == 0xBB)
            q += 3;                          /* UTF-8 BOM */
        long ms = -1;
        char txt[LR_TXT];
        txt[0] = '\0';
        int parsed = 0;
        while (*q == '[')
        {
            char *end = strchr(q, ']');
            if (!end)
                break;
            int mm = 0, ss = 0, xx = 0;
            if (sscanf(q, "[%d:%d.%d", &mm, &ss, &xx) >= 2)
            {
                ms = (long)mm * 60000 + ss * 1000 + xx % 1000;
                q = end + 1;
                parsed = 1;
            }
            else
                break;                       /* [ti:] [ar:] meta tag */
        }
        if (!parsed || ms < 0)
            continue;
        char *nl = strpbrk(q, "\r\n");
        if (nl)
            *nl = '\0';
        if (*q)
        {
            strlcpy(txt, q, LR_TXT);
            lr_add(p, ms, txt);
        }
    }
    fclose(f);
}

static void lr_find_and_parse(player_t *p, const char *track)
{
    free(p->lines);
    p->lines = NULL;
    p->lr_n = 0;
    p->lr_cur = -1;

    /* split the audio path into directory + filename-without-extension,
     * so the sidecar lyric file is "<same name>.lrc" */
    char dir[MAX_PATH], stem[MAX_PATH];
    strlcpy(dir, track, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = '\0';                            /* dir = "/mnt/.../Song" */
    strlcpy(stem, slash + 1, sizeof stem);    /* "BEYOND - ????.flac" */
    char *dot = strrchr(stem, '.');
    if (dot)
        *dot = '\0';                          /* "BEYOND - ????" */

    p->lines = malloc(sizeof(lr_line) * LR_MAX_LINES);
    if (!p->lines)
        return;

    char fname[MAX_PATH], rb[MAX_PATH];
    snprintf(rb, sizeof rb, "%s/%s.lrc", dir, stem);
    snprintf(fname, sizeof fname, "%s%s", PIVOT_ROOT, rb);   /* rockbox path -> host */
    lr_parse(p, fname);

    /* fall back to a file named after the title */
    if (!p->lr_n && be_track_lrc_title_path(rb, sizeof rb))
    {
        snprintf(fname, sizeof fname, "%s%s", PIVOT_ROOT, rb);
        lr_parse(p, fname);
    }
}

static void lr_paint(player_t *p)
{
    if (p->lr_none)
        lv_obj_add_flag(p->lr_none, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < LR_ROWS; i++)
    {
        int idx = p->lr_cur - LR_ROWS / 2 + i;
        if (p->lr_n && idx >= 0 && idx < p->lr_n)
            lv_label_set_text(p->lr_lbl[i], p->lines[idx].txt);
        else
            lv_label_set_text(p->lr_lbl[i], "");
        int mid = i == LR_ROWS / 2;
        lv_obj_set_style_text_opa(p->lr_lbl[i], mid ? 255 : 130, 0);
        lv_obj_set_style_text_color(p->lr_lbl[i],
                                    mid ? th->lrc_active
                                        : th->lrc_dim, 0);
    }
    if (!p->lr_n && p->lr_none)
        lv_obj_clear_flag(p->lr_none, LV_OBJ_FLAG_HIDDEN);
}

static void lr_tick(lv_timer_t *t)
{
    player_t *p = t->user_data;
    struct be_player_info pi;
    be_player_info(&pi);
    if (!pi.active)
        return;
    if (strcmp(p->lr_path, pi.path) != 0)
    {
        strlcpy(p->lr_path, pi.path, sizeof p->lr_path);
        lr_find_and_parse(p, pi.path);
        p->lr_cur = -1;
        lr_paint(p);
    }
    long el = pi.elapsed;
    while (p->lr_cur + 1 < p->lr_n && p->lines[p->lr_cur + 1].ms <= el)
        p->lr_cur++;
    /* allow jumping back after a seek */
    if (p->lr_cur >= 0 && p->lr_cur + 1 < p->lr_n && p->lines[p->lr_cur].ms > el)
    {
        p->lr_cur = -1;
        while (p->lr_cur + 1 < p->lr_n && p->lines[p->lr_cur + 1].ms <= el)
            p->lr_cur++;
    }
    if (p->lr_cur != p->lr_last_cur)
    {
        lr_paint(p);
        p->lr_last_cur = p->lr_cur;
    }
}

/* ---- info page (labels are rebuilt in place on track change) ------------ */
static void info_build(player_t *p)
{
    struct be_player_info pi;
    be_player_info(&pi);
    static const char *keys[INFO_ROWS] = {
        "标题", "艺术家", "专辑", "格式", "采样率",
        "比特率", "时长", "文件大小", "文件路径"
    };
    /* i18n: rows with a stock LANG token show translated */
    static int key_lang[INFO_ROWS] = {
        LANG_ID3_TITLE, LANG_ID3_ARTIST, LANG_ID3_ALBUM, LANG_FORMAT,
        LANG_ID3_FREQUENCY, LANG_ID3_BITRATE, LANG_ID3_LENGTH, LANG_FILESIZE,
        LANG_ID3_PATH
    };
    const char *kstr[INFO_ROWS];
    for (int i = 0; i < INFO_ROWS; i++)
        kstr[i] = key_lang[i] ? str(key_lang[i]) : keys[i];
    const char *vals[INFO_ROWS] = {0};

    char ext[16] = "", freq[32] = "", br[32] = "", dur[32] = "", fsz[32] = "";
    if (pi.active)
    {
        snprintf(ext, sizeof ext, "%s", pi.ext[0] ? pi.ext : "—");
        snprintf(freq, sizeof freq, "%ld Hz", pi.freq);
        snprintf(br, sizeof br, "%d kbps", pi.bitrate);
        long s = pi.length / 1000;
        snprintf(dur, sizeof dur, "%ld:%02ld", s / 60, s % 60);
        snprintf(fsz, sizeof fsz, "%.1f MB", (double)pi.filesize / 1048576.0);
        vals[0] = pi.title[0] ? pi.title : "—";
        vals[1] = pi.artist[0] ? pi.artist : "—";
        vals[2] = pi.album[0] ? pi.album : "—";
        vals[3] = ext;
        vals[4] = freq;
        vals[5] = br;
        vals[6] = dur;
        vals[7] = fsz;
        vals[8] = pi.path;
    }
    else
    {
        for (int i = 0; i < INFO_ROWS; i++)
            vals[i] = "—";
    }

    for (int i = 0; i < INFO_ROWS; i++)
    {
        char line[512];
        snprintf(line, sizeof line, "%s: %s", kstr[i], vals[i]);
        lv_label_set_text(p->inf_v[i], line);
    }
}

/* ---- construction -------------------------------------------------------- */
/* small round transport button with a montserrat-28 symbol */
static lv_obj_t *tbtn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                      const char *sym, lv_event_cb_t cb, player_t *p)
{
    lv_obj_t *b = ui_round_btn(parent, x, y, 48, sym, false, false, cb,
                               p);
    lv_obj_add_event_cb(b, btn_press_cb, LV_EVENT_PRESSED, p);
    return b;
}

void shell_player_show(void)
{
    if (shell_nav_full())
        return;
    player_t *p = calloc(1, sizeof *p);
    if (!p)
        return;

    lv_obj_t *scr = shell_screen_new();

    shell_statusbar_build(scr);   /* shared status row: time/vol/bat/net */

    /* ---- region host: cover+title / lyrics / info (swipe pager) ---------- */
    lv_obj_t *bn = lv_obj_create(scr);
    lv_obj_set_size(bn, 360, PGR_H);
    lv_obj_set_pos(bn, 0, PB_Y);
    lv_obj_set_style_bg_opa(bn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bn, 0, 0);
    lv_obj_set_style_pad_all(bn, 0, 0);
    shell_style_shadow_off(bn);
    lv_obj_clear_flag(bn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(bn, cover_gesture_cb, LV_EVENT_GESTURE, p);
    p->banner = bn;

    /* page 0: square cover card + title block */
    lv_obj_t *ap = lv_obj_create(bn);
    lv_obj_set_size(ap, 360, PGR_H);
    lv_obj_set_pos(ap, 0, 0);
    lv_obj_set_style_bg_opa(ap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ap, 0, 0);
    lv_obj_set_style_pad_all(ap, 0, 0);
    shell_style_shadow_off(ap);
    lv_obj_clear_flag(ap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    p->artpg = ap;

    /* borderless artwork: no drop shadow */

    lv_obj_t *cc = lv_obj_create(ap);
    lv_obj_set_size(cc, PB_W, PB_H);
    lv_obj_set_pos(cc, PB_X, 0);
    lv_obj_set_style_radius(cc, 30, 0);
    lv_obj_set_style_bg_opa(cc, LV_OPA_TRANSP, 0);   /* borderless artwork */
    lv_obj_set_style_border_width(cc, 0, 0);
    lv_obj_set_style_pad_all(cc, 0, 0);
    lv_obj_set_style_clip_corner(cc, true, 0);
    shell_style_shadow_off(cc);
    lv_obj_clear_flag(cc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    p->cardc = cc;

    /* placeholder cover: soft gradient + music note (no card frame) */
    lv_obj_t *ph = lv_obj_create(cc);
    lv_obj_set_size(ph, PB_W, PB_H);
    lv_obj_set_pos(ph, 0, 0);
    lv_obj_set_style_radius(ph, 30, 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ph, th->accent_blue, 0);
    lv_obj_set_style_bg_grad_color(ph, th->accent, 0);
    lv_obj_set_style_bg_grad_dir(ph, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(ph, 0, 0);
    lv_obj_set_style_pad_all(ph, 0, 0);
    lv_obj_set_style_clip_corner(ph, true, 0);
    shell_style_shadow_off(ph);
    lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *note = lv_label_create(ph);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, th->font_big, 0);
    lv_obj_set_style_text_color(note, lv_color_white(), 0);
    lv_obj_set_style_text_opa(note, 150, 0);
    lv_obj_center(note);
    p->ph = ph;

    /* bottom scrim so the title stays readable over artwork */
    g_scrim = lv_obj_create(cc);
    lv_obj_set_size(g_scrim, PB_W, 96);
    lv_obj_set_pos(g_scrim, 0, PB_H - 96);
    lv_obj_set_style_radius(g_scrim, 0, 0);
    lv_obj_set_style_bg_color(g_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_scrim, LV_OPA_30, 0);
    lv_obj_set_style_bg_grad_color(g_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(g_scrim, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(g_scrim, 0, 0);
    lv_obj_set_style_pad_all(g_scrim, 0, 0);
    lv_obj_clear_flag(g_scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    shell_style_shadow_off(g_scrim);
    lv_obj_add_flag(g_scrim, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *tt = lv_label_create(cc);
    lv_obj_set_size(tt, PB_W - 24, 26);
    lv_obj_set_pos(tt, 12, PB_H - 38);
    lv_obj_set_style_text_align(tt, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(tt, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(tt, th->art_tt_text, 0);
    lv_label_set_text(tt, "");
    lv_obj_add_flag(tt, LV_OBJ_FLAG_HIDDEN);   /* art is borderless now */
    p->tt = tt;

    /* page 1: lyrics (fills the whole region) */
    lv_obj_t *lrc = lv_obj_create(bn);
    lv_obj_set_size(lrc, 360, 310);
    lv_obj_set_pos(lrc, 0, 0);
    lv_obj_set_style_bg_opa(lrc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(lrc, th->menu_bg, 0);
    lv_obj_set_style_radius(lrc, 26, 0);
    lv_obj_set_style_border_width(lrc, 0, 0);
    lv_obj_set_style_pad_all(lrc, 0, 0);
    lv_obj_set_style_clip_corner(lrc, true, 0);
    shell_style_shadow_off(lrc);
    lv_obj_clear_flag(lrc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < LR_ROWS; i++)
    {
        lv_obj_t *lb = lv_label_create(lrc);
        lv_obj_set_size(lb, 340, 24);
        lv_obj_set_pos(lb, 10, 20 + i * 32);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(lb, th->fg_text, 0);
        lv_obj_set_style_text_opa(lb, 130, 0);
        lv_label_set_text(lb, "");
        p->lr_lbl[i] = lb;
    }
    lv_obj_t *lnone = lv_label_create(lrc);
    lv_label_set_text(lnone, str(LANG_N3_NO_LYRICS));
    lv_obj_set_style_text_color(lnone, th->fg_dim, 0);
    lv_obj_set_style_text_opa(lnone, 150, 0);
    lv_obj_align(lnone, LV_ALIGN_CENTER, 0, 0);
    p->lr_none = lnone;
    p->lrc = lrc;

    /* page 2: track info (fills the whole region, scrollable) */
    lv_obj_t *inf = lv_obj_create(bn);
    lv_obj_set_size(inf, 360, 310);
    lv_obj_set_pos(inf, 0, 0);
    lv_obj_set_style_bg_opa(inf, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(inf, th->menu_bg, 0);
    lv_obj_set_style_radius(inf, 26, 0);
    lv_obj_set_style_border_width(inf, 0, 0);
    lv_obj_set_style_pad_all(inf, 0, 0);
    lv_obj_set_style_pad_left(inf, 14, 0);
    lv_obj_set_style_pad_right(inf, 14, 0);
    lv_obj_set_style_pad_top(inf, 14, 0);
    lv_obj_set_style_clip_corner(inf, true, 0);
    shell_style_shadow_off(inf);
    lv_obj_set_scroll_dir(inf, LV_DIR_VER);
    lv_obj_set_flex_flow(inf, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(inf, 10, 0);
    for (int i = 0; i < INFO_ROWS; i++)
    {
        lv_obj_t *v = lv_label_create(inf);
        lv_obj_set_width(v, 332);
        lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(v, th->fg_text, 0);
        lv_obj_set_style_text_opa(v, 210, 0);
        lv_label_set_text(v, "");
        p->inf_v[i] = v;
    }
    p->infoc = inf;
    cover_set_page(p, 0);

    /* ---- title block (belongs to page 0, hides with the cover) ----------- */
    lv_obj_t *t = lv_label_create(ap);
    lv_label_set_text(t, str(LANG_N3_NOTHING_PLAYING));
    lv_obj_set_style_text_color(t, th->fg_text, 0);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_size(t, 324, 22);
    lv_obj_set_pos(t, 12, 292);
    p->title = t;

    lv_obj_t *s = lv_label_create(ap);
    lv_label_set_text(s, "");
    lv_obj_set_style_text_color(s, th->fg_text, 0);
    lv_obj_set_style_text_opa(s, 205, 0);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s, 324, 20);
    lv_obj_set_pos(s, 12, 336);
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    p->sub = s;

    /* ---- seek (draggable) ------------------------------------------------- */
    lv_obj_t *bar = ui_slider_bar(scr, 30, 354, 300, 8, UI_BAR_SEEK);
    lv_obj_add_event_cb(bar, seek_pressed_cb, LV_EVENT_PRESSED, p);
    lv_obj_add_event_cb(bar, seek_val_cb, LV_EVENT_VALUE_CHANGED, p);
    lv_obj_add_event_cb(bar, seek_released_cb, LV_EVENT_RELEASED, p);
    p->bar = bar;

    lv_obj_t *te = lv_label_create(scr);
    lv_label_set_text(te, "0:00");
    lv_obj_set_style_text_color(te, th->fg_text, 0);
    lv_obj_set_style_text_opa(te, 215, 0);
    lv_obj_set_pos(te, 30, 366);
    p->t_el = te;

    lv_obj_t *tt2 = lv_label_create(scr);
    lv_label_set_text(tt2, "0:00");
    lv_obj_set_style_text_color(tt2, th->fg_text, 0);
    lv_obj_set_style_text_opa(tt2, 215, 0);
    lv_obj_align(tt2, LV_ALIGN_TOP_RIGHT, -30, 366);
    p->t_tot = tt2;

    /* ---- transport: one full-width pill button group (like netsrc) ------- */
    lv_obj_t *grp = ui_btn_group(scr, 10, 396, 340, 64);

    /* repeat: three bars = 顺序, loop arrows = 全部, arrows+"1" = 单曲 */
    lv_obj_t *br = tbtn(grp, 16, 8, "", btn_repeat_cb, p);
    lv_obj_t *bars = lv_obj_create(br);
    lv_obj_set_size(bars, 22, 15);
    lv_obj_center(bars);
    lv_obj_set_style_bg_opa(bars, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bars, 0, 0);
    lv_obj_set_style_pad_all(bars, 0, 0);
    shell_style_shadow_off(bars);
    lv_obj_clear_flag(bars, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *ln = lv_obj_create(bars);
        lv_obj_set_size(ln, 22, 3);
        lv_obj_set_pos(ln, 0, i * 6);
        lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ln, th->fg_text, 0);
        lv_obj_set_style_radius(ln, 2, 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        shell_style_shadow_off(ln);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
    p->rep_lines = bars;

    /* circular double-arrow ("cached" style): two 115-degree arcs, each
     * ending in a solid arrow head, with a clear gap between the two
     * halves; the middle is free for the A / 1 marker */
    lv_obj_t *ic = lv_obj_create(br);
    lv_obj_set_size(ic, 40, 40);
    lv_obj_center(ic);
    lv_obj_set_style_bg_opa(ic, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_pad_all(ic, 0, 0);
    shell_style_shadow_off(ic);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    p->rep_ic = ic;

    /* anti-aliased circular double-arrow icon (pre-rendered bitmap) */
    lv_obj_t *repimg = lv_img_create(ic);
    lv_img_set_src(repimg, &ui_icon_repeat);
    lv_obj_center(repimg);
    lv_obj_set_style_img_recolor(repimg, th->fg_text, 0);
    lv_obj_set_style_img_recolor_opa(repimg, LV_OPA_COVER, 0);
    lv_obj_clear_flag(repimg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* mode marker inside the ring: "A" = all, "1" = one */
    lv_obj_t *rl = lv_label_create(ic);
    lv_label_set_text(rl, "A");
    lv_obj_set_style_text_font(rl, th->font_cjk, 0);
    lv_obj_set_style_text_color(rl, th->fg_text, 0);
    lv_obj_set_style_bg_opa(rl, LV_OPA_TRANSP, 0);
    lv_obj_align(rl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(rl);
    p->rep_badge = rl;
    player_set_repeat(p);

    tbtn(grp, 80, 8, LV_SYMBOL_PREV, btn_prev_cb, p);

    lv_obj_t *big = tbtn(grp, 144, 8, LV_SYMBOL_PLAY, btn_play_cb, p);
    p->btn_play = lv_obj_get_child(big, 0);

    tbtn(grp, 208, 8, LV_SYMBOL_NEXT, btn_next_cb, p);
    tbtn(grp, 272, 8, LV_SYMBOL_VOLUME_MAX, btn_vol_cb, p);

    lv_obj_add_event_cb(scr, player_delete_cb, LV_EVENT_DELETE, p);
    lv_obj_add_event_cb(scr, scr_gesture_cb, LV_EVENT_GESTURE, p);

    p->scr = scr;
    p->path[0] = '\0';
    p->lr_path[0] = '\0';
    shell_nav_push(scr);
    player_tick_refresh(p);          /* paint immediately */
    p->timer = lv_timer_create(player_tick, 500, p);
    p->lr_timer = lv_timer_create(lr_tick, 400, p);
}

#endif /* HAVE_LVGL_UI */
