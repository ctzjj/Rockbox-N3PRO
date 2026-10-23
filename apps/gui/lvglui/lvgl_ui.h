#ifndef LVGL_UI_H
#define LVGL_UI_H
#ifdef HAVE_LVGL_UI

#include <stdbool.h>

void lvgui_main(void);            /* replaces root_menu() in apps/main.c */
void lvgui_port_disp_init(void);
void lvgui_port_input_init(void);
void lvgui_input_feed(int button, intptr_t data);
void lvgui_shell_start(void);

/* shell building blocks (ui_shell.c) -- include lvgl.h first */
lv_obj_t *shell_screen_new(void);
void shell_nav_push(lv_obj_t *scr);
void shell_nav_pop(void);
void shell_nav_pop_now(void);
void shell_nav_home(void);
/* rebuild the home screen after a root-menu configuration change */
void shell_home_rebuild(void);
/* repaint the wallpaper after a live theme change (ui_theme_init()) */
void shell_theme_refresh(void);
/* a .lng was loaded: rebuild the screens that were built with old strings */
void lvgui_lang_changed(void);
bool shell_nav_full(void);
void shell_statusbar_build(lv_obj_t *parent);
void shell_header(lv_obj_t *scr, const char *title);
void shell_obj_del_async(lv_obj_t *o);

/* generic modal dialog: dark scrim + glass card over the active screen */
typedef struct
{
    const char *text;        /* button label; array ends with a NULL text */
    bool primary;            /* accent-styled button */
    lv_event_cb_t cb;        /* may be NULL */
    void *user_data;         /* for the callback */
} shell_modal_btn_t;

lv_obj_t *shell_modal_open(const char *title, const char *body,
                           const shell_modal_btn_t *buttons);
void shell_modal_close(lv_obj_t *card);
void shell_modal_close_ev(lv_event_t *e);
/* blocking LVGL keyboard, kbd_input() stand-in (ui_shell.c): returns 0 on
 * confirm (text copied back), -1 on cancel -- only from lvgui_run_job() */
int shell_kbd_input(char *text, int buflen);
/* queue a classic/blocking handler to run at the top of the UI loop */
void lvgui_run_job(void (*fn)(void *), void *param);
/* one iteration of the UI loop (buttons + lv_timer_handler); used while a
 * blocking stock handler shows LVGL UI */
void lvgui_pump(void);
/* pump only LVGL timers (no input) while blocking in backend code */
void lvgui_idle_pump(void);
/* true while a deferred classic handler runs at the top of the loop */
bool lvgui_in_job(void);
/* blocking yes/no dialog, gui_syncyesno_run() stand-in (ui_shell.c):
 * 0 = yes, 1 = no -- only from an lvgui_run_job() context */
int shell_yesno_run(const char *message);
/* blocking playlist picker, display_playlists() stand-in (ui_library.c):
 * 0 = picked (path in out), -1 = cancel -- only from lvgui_run_job() */
int shell_plcat_pick(char *out, size_t outsz);
/* menu list screen (nav-stack based, edge-swipe back pops it; ui_shell.c):
 * pushes a full-screen page in the house style, returns it; rows via
 * shell_menu_row_new(); on_close fires on LV_EVENT_DELETE */
lv_obj_t *shell_menu_screen_push(const char *title, lv_obj_t **list_out,
                                 lv_event_cb_t on_close);
/* house-style menu row (ui_shell.c): optional icon tile (tile colour =
 * home palette index 0..5, -1 = plain accent) / '>' chevron */
lv_obj_t *shell_menu_row_new(lv_obj_t *list, const char *icon, bool sym,
                             const char *label, bool chev, bool sel,
                             int idx, lv_event_cb_t cb, int grad);
/* one-shot per-row tile colours for the next shell_list_pick*() */
void shell_pick_grads(const int *grads, int count);
/* blocking titled list picker, simplelist stand-in (ui_shell.c):
 * selected index, or -1 when cancelled -- only from lvgui_run_job() */
int shell_list_pick(const char *title, const char **items, int count,
                    int preselect);
/* rebuild a menu screen's contents in place (no navigation) */
void shell_menu_screen_rebuild(lv_obj_t *scr, const char *title, lv_obj_t **list_out);
/* drop list-picker screens left on top when a job ends */
void shell_pick_cleanup(void);

/* pull-down control centre */
void lvgui_control_open(void);
void lvgui_control_close(void);
bool lvgui_control_is_open(void);
/* Bluetooth menu (native do_menu tree) */
int lvgui_bt_menu(void);
/* stock resume-playback entry (home play row) */
int lvgui_resume_playback(void);
/* live-preview variant for option_screen: on_move fires on every highlight
 * change; tapping the highlighted row confirms, back cancels (-1) */
int shell_list_pick_live(const char *title, const char **items, int count,
                         int preselect,
                         void (*on_move)(int sel, void *ud), void *ud);
/* live-refresh status list (netfm/bt-rx/wifi/dlna status simplelists):
 * on_tick refreshes the data (stock ACTION_NONE callback), fill re-renders
 * the row labels every period; returns picked index, -1 when cancelled */
int shell_list_pick_refresh(const char *title, int count, int preselect,
                            int period_ms,
                            void (*on_tick)(void *ud),
                            void (*fill)(int idx, char *buf, size_t sz,
                                         void *ud), void *ud);
/* one-shot per-row icons (LV_SYMBOL strings) for the NEXT pick call */
void shell_pick_icons(const char **icons, int count);
/* stock gui/icon.h id (or private negative pseudo-id) -> LVGL glyph */
const char *lvgui_menu_icon(int icon_id, int *grad);
/* first UTF-8 code point of s (home-style tile text) */
void lvgui_first_char(const char *s, char *out, size_t sz);
/* apply the scrollbar setting to a scrolling container (ui_shell.c) */
void shell_scrollbar_setup(lv_obj_t *list);
/* splash/toast stand-in (ui_shell.c): ticks>0 blocks for that long, then
 * removes; ticks==0 shows/hides (empty msg) without blocking. Job context. */
void shell_splash(int ticks, const char *msg);
/* Android-style toast: translucent black pill, white text, optional icon
 * (LVGL symbol glyph or NULL); blocks for ticks when > 0 */
void shell_toast(int ticks, const char *msg, const char *icon);
/* clear any live toast (screen transitions do this automatically, matching
 * classic splash(0,...) being overwritten by the next screen draw) */
void shell_toast_clear(void);
lv_obj_t *shell_glass_new(lv_obj_t *parent);
void shell_soft_shadow(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, lv_coord_t radius,
                       lv_coord_t dy);
lv_obj_t *shell_style_shadow_off(lv_obj_t *o);
lv_color_t shell_col_text(void);
lv_color_t shell_col_dim(void);
lv_color_t shell_col_link(void);
lv_color_t shell_col_ink(void);
lv_color_t shell_icon_grad(int i);

/* M2 library screens (ui_library.c) */
void library_open(void);

/* file browser (ui_library.c) */
void shell_files_show(void);

/* playlist catalog (ui_library.c) */
void shell_playlists_show(void);

/* charset: rewrite GBK tag text into UTF-8 for display (ui_charset.c);
 * returns dst (always a NUL-terminated converted copy) */
const char *shell_cs(char *dst, size_t dstsz, const char *src);

/* decoded album art shared between screens */
typedef struct
{
    lv_img_dsc_t dsc;
} lvui_art_t;

lvui_art_t *lib_track_art(const char *path);   /* malloc'd; caller frees */
lvui_art_t *lib_track_art_sized(const char *path, int w, int h);
void lvui_art_free(lvui_art_t *art);

/* M3 now-playing screen (ui_player.c) */
void shell_player_show(void);
void shell_playlist_show(void);

#endif /* HAVE_LVGL_UI */
#endif /* LVGL_UI_H */
