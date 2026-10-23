#include "config.h"
#ifdef HAVE_LVGL_UI

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "lvgl_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_backend.h"
#include "settings.h"
#include "lang.h"

/* iOS26-style control centre: full-screen frosted panel, [connectivity 2x2]
 * top-left, [now playing] top-right, volume/brightness pills below. */
static lv_obj_t *g_ctl;
static bool g_open;
static lv_obj_t *g_med_art;      /* cover parent (accent tile) */
static lv_obj_t *g_med_img;      /* cover image */
static lv_obj_t *g_med_title;    /* scrolling title */
static lv_obj_t *g_med_play_lbl; /* play/pause glyph */
static lv_obj_t *g_wifi_btn, *g_bt_btn, *g_tr_btn, *g_ul_btn;
static lvui_art_t *g_med_dsc;
static lv_obj_t *g_med_b1, *g_med_b2, *g_med_b3, *g_med_bx;
static char g_med_path[MAX_PATH];
static lv_timer_t *g_med_timer;

bool lvgui_control_is_open(void) { return g_open; }

/* live state of the four toggles, painted in place (no rebuild) */
static void ctl_toggles_paint(void)
{
    lv_obj_t *btn[4] = { g_wifi_btn, g_bt_btn, g_tr_btn, g_ul_btn };
    bool on[4] = {
        be_wifi_up(),
        be_bt_active(),
        be_tube_mode() == 1,
        be_tube_mode() == 2,
    };

    for (int i = 0; i < 4; i++)
    {
        if (!btn[i])
            continue;
        lv_obj_set_style_bg_color(btn[i], on[i] ? th->accent_blue
                                                : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn[i], on[i] ? 235 : 120, 0);
        lv_obj_t *l = lv_obj_get_child(btn[i], 0);
        if (l)
            lv_obj_set_style_text_color(l, on[i] ? lv_color_white()
                                                 : th->fg_text, 0);
    }
}

static void ctl_media_refresh(void)
{
    const char *trk = be_audio_track_path();
    char key[160];
    if (!trk)
        trk = "";
    /* radio has no track path: key the refresh on the station name instead */
    if (be_dlna_active())
        be_dlna_name(key, sizeof key);
    else if (be_media_is_radio())
        be_netfm_name(key, sizeof key);
    else
        strlcpy(key, trk, sizeof key);
    if (strcmp(g_med_path, key) == 0)
        return;
    strlcpy(g_med_path, key, sizeof g_med_path);

    if (g_med_title)
    {
        char cbuf[160];
        if (be_dlna_active())
        {
            char nb[128];
            be_dlna_name(nb, sizeof nb);
            lv_label_set_text(g_med_title,
                              shell_cs(cbuf, sizeof cbuf,
                                       nb[0] ? nb : "DLNA"));
        }
        else if (be_media_is_radio())
        {
            char nb[128];
            be_netfm_name(nb, sizeof nb);
            lv_label_set_text(g_med_title, nb[0] ? nb : str(LANG_N3_NET_RADIO));
        }
        else
        {
            const char *ttl = be_audio_track_title();
            lv_label_set_text(g_med_title,
                              shell_cs(cbuf, sizeof cbuf,
                                       (ttl && ttl[0]) ? ttl : str(LANG_NOW_PLAYING)));
        }
    }
    if (g_med_dsc)
    {
        lvui_art_free(g_med_dsc);
        g_med_dsc = NULL;
    }
    if (g_med_img)
    {
        lv_obj_del(g_med_img);
        g_med_img = NULL;
    }
    if (trk[0] && g_med_art)
    {
        g_med_dsc = lib_track_art_sized(trk, 116, 116);
        if (g_med_dsc)
        {
            g_med_img = lv_img_create(g_med_art);
            lv_img_set_src(g_med_img, &g_med_dsc->dsc);
            lv_obj_center(g_med_img);
            lv_obj_clear_flag(g_med_img, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

static void ctl_close(void);
int lvgui_dlna_view(void);

static void ctl_dlna_job(void *p)
{
    (void)p;
    lvgui_dlna_view();
}

void shell_lock_show(void);

static void ctl_lock_job(void *p)
{
    (void)p;
    shell_lock_show();
}

static void ctl_lock_cb(lv_event_t *e)
{
    (void)e;
    ctl_close();
    lvgui_run_job(ctl_lock_job, NULL);
}

static void ctl_menu_job(void *p)
{
    void (*fn)(void) = (void (*)(void))p;

    fn();
}

static void ctl_settings_cb(lv_event_t *e)
{
    (void)e;
    ctl_close();
    lvgui_run_job(ctl_menu_job, (void *)be_settings_menu);
}

static void ctl_power_job(void *p)
{
    (void)p;
    if (shell_yesno_run(str(LANG_N3_CONFIRM_POWER_OFF)) == 0)
        be_power_off();
}

static void ctl_power_cb(lv_event_t *e)
{
    (void)e;
    ctl_close();
    lvgui_run_job(ctl_power_job, NULL);
}

static void ctl_reboot_job(void *p)
{
    (void)p;
    if (shell_yesno_run(str(LANG_REBOOT_NOW)) == 0)
        be_reboot();
}

static void ctl_reboot_cb(lv_event_t *e)
{
    (void)e;
    ctl_close();
    lvgui_run_job(ctl_reboot_job, NULL);
}

static void ctl_open_dlna_cb(lv_event_t *e)
{
    (void)e;
    if (!be_dlna_active())
        return;
    ctl_close();
    lvgui_run_job(ctl_dlna_job, NULL);
}

static void ctl_media_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_ctl)
        return;
    if (!be_media_active())
    {
        /* DLNA stopped / local playback ended: leave nothing stale behind */
        if (g_med_title)
            lv_label_set_text(g_med_title, str(LANG_N3_NOTHING_PLAYING));
        g_med_path[0] = '\0';
        if (g_med_img)
        {
            lv_obj_del(g_med_img);
            g_med_img = NULL;
        }
        if (g_med_dsc)
        {
            lvui_art_free(g_med_dsc);
            g_med_dsc = NULL;
        }
    }
    else
        ctl_media_refresh();
    ctl_toggles_paint();
    {
        bool dlna = be_dlna_active();
        if (g_med_bx)
        {
            if (dlna)
                lv_obj_clear_flag(g_med_bx, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(g_med_bx, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_med_b1)
        {
            if (dlna)
            {
                lv_obj_add_flag(g_med_b1, LV_OBJ_FLAG_HIDDEN);
                if (g_med_b2)
                    lv_obj_add_flag(g_med_b2, LV_OBJ_FLAG_HIDDEN);
                if (g_med_b3)
                    lv_obj_add_flag(g_med_b3, LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_clear_flag(g_med_b1, LV_OBJ_FLAG_HIDDEN);
                if (g_med_b2)
                    lv_obj_clear_flag(g_med_b2, LV_OBJ_FLAG_HIDDEN);
                if (g_med_b3)
                    lv_obj_clear_flag(g_med_b3, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (g_med_play_lbl)
        lv_label_set_text(g_med_play_lbl,
                          be_media_is_radio() ? LV_SYMBOL_STOP
                          : be_media_playing() ? LV_SYMBOL_PAUSE
                                               : LV_SYMBOL_PLAY);
}

static void ctl_build(void);

static void ctl_close(void)
{
    if (!g_ctl) return;
    if (g_med_timer)
    {
        lv_timer_del(g_med_timer);
        g_med_timer = NULL;
    }
    if (g_med_dsc)
    {
        lvui_art_free(g_med_dsc);
        g_med_dsc = NULL;
    }
    g_med_art = g_med_img = g_med_title = g_med_play_lbl = NULL;
    g_med_b1 = g_med_b2 = g_med_b3 = g_med_bx = NULL;
    g_wifi_btn = g_bt_btn = g_tr_btn = g_ul_btn = NULL;
    g_med_path[0] = '\0';
    lv_obj_del(g_ctl);
    g_ctl = NULL;
    g_open = false;
}

static void ctl_rebuild(void)
{
    bool was = g_open;
    ctl_close();
    if (was) ctl_build();
}

static void ctl_bg_cb(lv_event_t *e) { (void)e; }

static void ctl_vol_cb(lv_event_t *e)
{
    be_volume_set_pct(lv_slider_get_value(lv_event_get_target(e)));
}

static void ctl_bri_cb(lv_event_t *e)
{
    be_brightness_set(lv_slider_get_value(lv_event_get_target(e)));
}

static void ctl_open_async(void *p)
{
    (void)p;
    lvgui_control_open();
}

static void ctl_bt_power_job(void *p)
{
    (void)p;
    if (be_bt_active())
        be_bt_disconnect();
    lv_async_call(ctl_open_async, NULL);
}

static void ctl_wifi_job(void *p)
{
    (void)p;
    be_wifi_toggle();
    lv_async_call(ctl_open_async, NULL);   /* show the new state */
}
static void ctl_wifi_cb(lv_event_t *e) { (void)e; lvgui_run_job(ctl_wifi_job, NULL); }

static void ctl_bt_job(void *p) { (void)p; be_bt_menu_open(); }

static void ctl_bt_cb(lv_event_t *e)
{
    (void)e;
    ctl_close();
    lvgui_run_job(ctl_bt_job, NULL);
}

static void ctl_rebuild_async(void *p)
{
    (void)p;
    ctl_rebuild();
}

static void ctl_step_cb(lv_event_t *e)
{
    /* must NOT go through lvgui_run_job: while a stock screen (e.g. the
     * netfm menu) is inside its own job, queued jobs never run and the
     * skip silently does nothing. Track skips are on a worker thread and
     * station switching is quick, so call it directly. */
    be_media_step((int)(intptr_t)lv_event_get_user_data(e));
    if (be_media_is_radio() && g_med_title)
    {
        const char *nm = be_netfm_last_name();
        if (nm && nm[0])
        {
            lv_label_set_text(g_med_title, nm);
            strlcpy(g_med_path, nm, sizeof g_med_path);
        }
    }
    lv_async_call(ctl_rebuild_async, NULL);
}

static void ctl_play_cb(lv_event_t *e)
{
    (void)e;
    be_media_toggle();
    lv_async_call(ctl_rebuild_async, NULL);
}

static void ctl_long_job(void *p)
{
    void (*fn)(void) = (void (*)(void))p;
    fn();
}

static uint32_t g_btn_t[3];

static void ctl_btn_pressed(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    g_btn_t[i] = lv_tick_get();
}

static void ctl_btn_released(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bool longp = lv_tick_elaps(g_btn_t[i]) >= 500;

    if (longp)
    {
        ctl_close();
        switch (i)
        {
            case 0: lvgui_run_job(ctl_long_job, (void *)be_wifi_menu); break;
            case 1: lvgui_run_job(ctl_long_job, (void *)be_bt_menu_open); break;
            default: lvgui_run_job(ctl_long_job, (void *)be_sound_menu); break;
        }
        return;
    }

    switch (i)
    {
        case 0:
            /* the vendor power path emits splashes; run it with the drawer
             * closed, then reopen to show the new state */
            ctl_close();
            lvgui_run_job(ctl_wifi_job, NULL);
            break;
        case 1:
            if (be_bt_active())
            {
                ctl_close();
                lvgui_run_job(ctl_bt_power_job, NULL);
            }
            else
            {
                ctl_close();
                lvgui_run_job(ctl_long_job, (void *)be_bt_menu_open);
            }
            break;
        case 2:
            be_tube_set(1);
            ctl_toggles_paint();
            break;
        default:
            be_tube_set(2);
            ctl_toggles_paint();
            break;
    }
}

static void ctl_open_player_cb(lv_event_t *e)
{
    (void)e;
    ctl_close();
}

static lv_obj_t *ctl_mod(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c;
    c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, 32, 0);
    /* same thin-glass material as the shell panes */
    lv_obj_set_style_bg_color(c, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(c, th->card_opa, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_opa(c, th->card_border_opa, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_shadow_width(c, 24, 0);
    lv_obj_set_style_shadow_spread(c, 0, 0);
    lv_obj_set_style_shadow_ofs_x(c, 0, 0);
    lv_obj_set_style_shadow_ofs_y(c, 4, 0);
    lv_obj_set_style_shadow_color(c, th->sh_dark, 0);
    lv_obj_set_style_shadow_opa(c, th->glass_shadow_opa, 0);

    return c;
}

static lv_obj_t *ctl_round(lv_obj_t *parent, int x, int y,
                           const char *icon, bool on, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 48, 48);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, on ? th->accent_blue : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, on ? 220 : th->card_opa, 0);
    lv_obj_set_style_border_color(b, th->border_white, 0);
    lv_obj_set_style_border_opa(b, 150, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, icon);
    lv_obj_set_style_text_font(l, th->font_symbol, 0);
    lv_obj_set_style_text_color(l, on ? lv_color_white() : th->fg_text, 0);
    lv_obj_center(l);
    return b;
}

/* small hand-drawn sun (no sun glyph exists in the bundled fonts) */
/* hand-drawn light bulb (tube stage indicator) */
static void ctl_bulb(lv_obj_t *parent, bool on)
{
    lv_color_t col = on ? lv_color_white() : th->fg_text;
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 34, 34);
    lv_obj_set_pos(box, 7, 7);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    struct { int x, y, w, h, r; } parts[] = {
        { 10, 5, 14, 14, 7 },    /* glass  */
        { 14, 17, 6, 6, 1 },     /* neck   */
        { 11, 23, 12, 2, 1 },    /* base   */
        { 11, 26, 12, 2, 1 },
        { 12, 29, 10, 2, 1 },
    };
    for (unsigned k = 0; k < sizeof(parts) / sizeof(parts[0]); k++)
    {
        lv_obj_t *o = lv_obj_create(box);
        lv_obj_set_size(o, parts[k].w, parts[k].h);
        lv_obj_set_pos(o, parts[k].x, parts[k].y);
        lv_obj_set_style_radius(o, parts[k].r, 0);
        lv_obj_set_style_bg_color(o, col, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
}

static lv_obj_t *ctl_round_bulb(lv_obj_t *parent, int x, int y, bool on,
                                lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 48, 48);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, on ? th->accent_blue : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, on ? 220 : th->card_opa, 0);
    lv_obj_set_style_border_color(b, th->border_white, 0);
    lv_obj_set_style_border_opa(b, 150, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    ctl_bulb(b, on);
    return b;
}

static void ctl_sun(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 34, 34);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    struct { int x, y, w, h; } parts[] = {
        { 10, 10, 14, 14 },   /* disc  */
        { 15, 2, 4, 6 },      /* up    */
        { 15, 26, 4, 6 },     /* down  */
        { 2, 15, 6, 4 },      /* left  */
        { 26, 15, 6, 4 },     /* right */
        { 5, 5, 4, 4 },       /* corners */
        { 25, 5, 4, 4 },
        { 5, 25, 4, 4 },
        { 25, 25, 4, 4 },
    };
    for (unsigned k = 0; k < sizeof(parts) / sizeof(parts[0]); k++)
    {
        lv_obj_t *o = lv_obj_create(box);
        lv_obj_set_size(o, parts[k].w, parts[k].h);
        lv_obj_set_pos(o, parts[k].x, parts[k].y);
        lv_obj_set_style_radius(o, 7, 0);
        lv_obj_set_style_bg_color(o, th->fg_text, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
}

static void ctl_slider(lv_obj_t *parent, int x, int val, const char *icon,
                       lv_event_cb_t cb)
{
    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_size(sl, 44, 150);            /* h > w -> vertical */
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_pos(sl, x, 18);
    lv_obj_set_style_radius(sl, 24, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 24, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, th->fg_text, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, 70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(sl, lv_color_hex(0xdfe9ff),
                                   LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(sl, LV_GRAD_DIR_VER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);

    if (icon)
    {
        lv_obj_t *i = lv_label_create(parent);
        lv_label_set_text(i, icon);
        /* same visual size as the drawn sun on the brightness pill */
        lv_obj_set_style_text_font(i, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(i, th->fg_text, 0);
        lv_obj_set_pos(i, x - 2, 173);
    }
    else
        ctl_sun(parent, x - 3, 172);
}

static void ctl_build(void)
{
    lv_obj_t *top = lv_layer_top();
    g_ctl = lv_obj_create(top);
    lv_obj_set_size(g_ctl, 360, 480);
    lv_obj_set_pos(g_ctl, 0, 0);
    lv_obj_clear_flag(g_ctl, LV_OBJ_FLAG_SCROLLABLE);
    /* scrim: dims the page so text is not legible, colour survives */
    lv_obj_set_style_bg_color(g_ctl, lv_color_hex(0x0b0f16), 0);
    lv_obj_set_style_bg_opa(g_ctl, 165, 0);
    lv_obj_set_style_border_width(g_ctl, 0, 0);
    lv_obj_set_style_pad_all(g_ctl, 0, 0);
    lv_obj_add_event_cb(g_ctl, ctl_bg_cb, LV_EVENT_CLICKED, NULL);

    /* frosted layer: translucent glass over the blurred-looking page */
    lv_obj_t *frost = lv_obj_create(g_ctl);
    lv_obj_set_size(frost, 360, 480);
    lv_obj_set_pos(frost, 0, 0);
    /* the panel is a fresh glass page: paint the pre-blurred wallpaper so
     * the page underneath never shows through the controls */
    shell_backdrop_apply(frost);
    lv_obj_set_style_border_width(frost, 0, 0);
    lv_obj_set_style_pad_all(frost, 0, 0);
    lv_obj_clear_flag(frost, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* left: one card holding both sliders */
    lv_obj_t *slc = ctl_mod(frost, 12, 16, 166, 216);
    ctl_slider(slc, 26, be_volume_pct_get(), LV_SYMBOL_VOLUME_MAX, ctl_vol_cb);
    ctl_slider(slc, 94, be_brightness_get(), NULL, ctl_bri_cb);

    /* right: playback card */
    lv_obj_t *med = ctl_mod(frost, 186, 16, 162, 216);
    if (be_media_active())
    {
        lv_obj_t *art = lv_obj_create(med);
        lv_obj_set_size(art, 116, 116);
        lv_obj_set_pos(art, 23, 10);
        g_med_art = art;
        lv_obj_set_style_radius(art, 24, 0);
        lv_obj_set_style_bg_color(art, th->accent, 0);
        lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(art, 0, 0);
        lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *ag = lv_label_create(art);
        lv_label_set_text(ag, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(ag, th->font_symbol, 0);
        lv_obj_set_style_text_color(ag, th->fg_ink, 0);
        lv_obj_center(ag);

        lv_obj_t *l = lv_label_create(med);
        lv_label_set_text(l, str(be_media_is_radio() ? LANG_N3_NET_RADIO : LANG_NOW_PLAYING));
        lv_obj_set_style_text_color(l, th->fg_text, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(l, 116);
        lv_obj_set_pos(l, 10, 132);
        g_med_title = l;
        g_med_path[0] = '\0';
        ctl_media_refresh();
        g_med_timer = lv_timer_create(ctl_media_timer_cb, 500, NULL);

        {
            lv_obj_t *b;
            b = ui_round_btn(med, 8, 164, 38, LV_SYMBOL_PREV, false, false,
                             ctl_step_cb, (void *)-1);
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            g_med_b1 = b;
            b = ui_round_btn(med, 55, 164, 38,
                             be_media_is_radio() ? LV_SYMBOL_STOP
                             : be_media_playing() ? LV_SYMBOL_PAUSE
                                                  : LV_SYMBOL_PLAY,
                             false, true, ctl_play_cb, NULL);
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            g_med_b2 = b;
            g_med_play_lbl = lv_obj_get_child(b, 0);
            b = ui_round_btn(med, 102, 164, 38, LV_SYMBOL_NEXT, false, false,
                             ctl_step_cb, (void *)1);
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            g_med_b3 = b;

            /* DLNA replaces the transport row with a single exit button */
            /* DLNA: no transport buttons; this slot shows "DLNA" and the
             * whole card taps through to the DLNA player screen. */
            lv_obj_add_flag(med, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(med, ctl_open_dlna_cb, LV_EVENT_CLICKED, NULL);

            b = lv_obj_create(med);
            lv_obj_set_size(b, 146, 38);
            lv_obj_set_pos(b, 8, 164);
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_radius(b, 19, 0);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(b, ctl_open_dlna_cb, LV_EVENT_CLICKED, NULL);
            {
                lv_obj_t *bl = lv_label_create(b);
                lv_label_set_text(bl, "DLNA");
                lv_obj_set_style_text_font(bl, th->font_cjk, 0);
                lv_obj_set_style_text_color(bl, th->fg_text, 0);
                lv_obj_center(bl);
            }
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
            g_med_bx = b;
        }
    }
    else
    {
        lv_obj_t *g = lv_label_create(med);
        lv_label_set_text(g, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(g, th->font_big, 0);
        lv_obj_set_style_text_color(g, th->fg_dim, 0);
        lv_obj_align(g, LV_ALIGN_CENTER, 0, -12);
        lv_obj_t *l = lv_label_create(med);
        lv_label_set_text(l, str(LANG_N3_NOTHING_PLAYING));
        lv_obj_set_style_text_color(l, th->fg_text, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 30);
        lv_obj_add_flag(med, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(med, ctl_open_player_cb, LV_EVENT_CLICKED, NULL);
    }

    /* bottom: one long button strip (room for more buttons later) */
    lv_obj_t *strip = ctl_mod(frost, 12, 248, 336, 94);
    {
        lv_obj_t *b;
        b = ctl_round(strip, 36, 10, LV_SYMBOL_WIFI, be_wifi_on(), NULL);
        g_wifi_btn = b;
        lv_obj_add_event_cb(b, ctl_btn_pressed, LV_EVENT_PRESSED, (void *)0);
        lv_obj_add_event_cb(b, ctl_btn_released, LV_EVENT_RELEASED, (void *)0);
        b = ctl_round(strip, 108, 10, LV_SYMBOL_BLUETOOTH, be_bt_active(), NULL);
        g_bt_btn = b;
        lv_obj_add_event_cb(b, ctl_btn_pressed, LV_EVENT_PRESSED, (void *)1);
        lv_obj_add_event_cb(b, ctl_btn_released, LV_EVENT_RELEASED, (void *)1);
        b = ctl_round(strip, 180, 10, "TR", be_tube_mode() == 1, NULL);
        g_tr_btn = b;
        lv_obj_add_event_cb(b, ctl_btn_pressed, LV_EVENT_PRESSED, (void *)2);
        lv_obj_add_event_cb(b, ctl_btn_released, LV_EVENT_RELEASED, (void *)2);
        b = ctl_round(strip, 252, 10, "UL", be_tube_mode() == 2, NULL);
        g_ul_btn = b;
        lv_obj_add_event_cb(b, ctl_btn_pressed, LV_EVENT_PRESSED, (void *)3);
        lv_obj_add_event_cb(b, ctl_btn_released, LV_EVENT_RELEASED, (void *)3);

        static const char *caps[4];
        static const int bx[4] = { 36, 108, 180, 252 };
        caps[0] = "WiFi";
        caps[1] = str(LANG_BLUETOOTH);
        caps[2] = str(LANG_N3_TUBE);
        caps[3] = str(LANG_N3_SUPERLINEAR);
        for (int i = 0; i < 4; i++)
        {
            lv_obj_t *l = lv_label_create(strip);
            lv_label_set_text(l, caps[i]);
            lv_obj_set_style_text_font(l, th->font_small, 0);
            lv_obj_set_style_text_color(l, th->fg_text, 0);
            lv_obj_set_width(l, 72);
            lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, bx[i] - 12, 64);
        }
        ctl_toggles_paint();   /* reflect states that were already on */
    }

    /* second row: lock / settings / power off / restart */
    lv_obj_t *row2 = ctl_mod(frost, 12, 350, 336, 94);
    {
        static const char *caps2[4];
        static const int bx2[4] = { 36, 108, 180, 252 };

        caps2[0] = str(LANG_N3_LOCK);
        caps2[1] = str(LANG_SETTINGS);
        caps2[2] = str(LANG_N3_POWER_OFF);
        caps2[3] = str(LANG_N3_REBOOT);

        ctl_round(row2, 36, 10, LV_SYMBOL_EYE_CLOSE, false, ctl_lock_cb);
        ctl_round(row2, 108, 10, LV_SYMBOL_SETTINGS, false, ctl_settings_cb);
        ctl_round(row2, 180, 10, LV_SYMBOL_POWER, false, ctl_power_cb);
        ctl_round(row2, 252, 10, LV_SYMBOL_REFRESH, false, ctl_reboot_cb);

        for (int i = 0; i < 4; i++)
        {
            lv_obj_t *l = lv_label_create(row2);
            lv_label_set_text(l, caps2[i]);
            lv_obj_set_style_text_font(l, th->font_small, 0);
            lv_obj_set_style_text_color(l, th->fg_text, 0);
            lv_obj_set_width(l, 72);
            lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, bx2[i] - 12, 64);
        }
    }

    g_open = true;
    lv_obj_invalidate(g_ctl);
    lv_refr_now(NULL);
}

void lvgui_control_open(void)
{
    if (g_open) return;
    ctl_build();
}

void lvgui_control_close(void)
{
    ctl_close();
}

#endif /* HAVE_LVGL_UI */
