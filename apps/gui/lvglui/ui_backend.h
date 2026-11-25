/* ui_backend.h -- rockbox data/playback adapter for the LVGL UI.
 *
 * Pure data in and out: no LVGL types live here. The UI layer renders
 * whatever these calls return and owns the returned memory until it frees
 * it. All rockbox knowledge (tagcache, playlist, file tree, metadata,
 * artwork decoding) stays behind this header. */

#ifndef UI_BACKEND_H
#define UI_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define BE_PATH_MAX 260            /* rockbox MAX_PATH */

/* Generic display list: parallel strings, count entries. */
typedef struct { char **title; char **sub; char **path; int count; } be_list_t;

/* Decoded image: native RGB565 pixel buffer, malloc'd. */
typedef struct { void *pixels; int w, h; } be_img_t;

#define BE_ART_SIDE 141            /* standard square cover size */

void be_list_free(be_list_t *t);

be_img_t *be_track_art(const char *path, int w, int h);
be_img_t *be_art_blob(const unsigned char *data, size_t len,int w, int h);
void be_img_free(be_img_t *a);

/* Build a playlist from the list paths and start playing at index start.
 * Small lists build synchronously, big ones on a background thread so the
 * caller's UI stays responsive. Never touches the UI itself. */
void be_play(const be_list_t *t, int start);

/* Play a playlist file through the stock ft_play_playlist (bookmarks,
 * erase warning and shuffle handling included). */
bool be_play_playlist_file(const char *full);

/* Scan a folder through the stock file-tree loader (ft_load: rockbox's
 * own sorting, filtering and memory handling). Returns the entry count
 * (>= 0) and three malloc'd parallel arrays owned by the caller, or -1
 * on error. */
int  be_dir_scan(const char *path, char ***names, char ***paths,
                 char ***icons);

/* Same, but with an explicit dirfilter (stock SHOW_* value) so callers can
 * browse with the same filtering/sorting the classic browser used. */
int  be_dir_scan_filter(const char *path, int dirfilter, char ***names,
                        char ***paths, char ***icons);

/* Database browsing driven by rockbox's own tagtree (apps/tagtree.c):
 * levels, filters, sorting and the play action all come from stock code.
 * be_db_scan() returns malloc'd name/icon arrays for the current level. */
void be_db_root(void);
int  be_db_scan(char ***names, char ***icons);
const char *be_db_title(void);
int  be_db_enter(int idx);          /* 0 = descended, non-0 = playback */
void be_db_back(void);

/* Read up to sz-1 bytes of a text file; returns bytes read or -1. */
int be_read_text(const char *path, char *buf, int sz);

bool be_is_audio(const char *name);
bool be_is_playlist_name(const char *name);

/* Recursive delete (files and folders); 0 on success. */
int  be_delete(const char *path);

/* Size and modification time of a file or directory. */
bool be_file_info(const char *path, unsigned long *size, time_t *mtime);

/* Scan the playlist catalog; returns the count and two malloc'd parallel
 * arrays (display name without extension, full path). */
int  be_playlists_scan(char ***names, char ***paths);
void be_str_arrays_free(char **a, char **b, int n);

/* ---- now-playing state ---------------------------------------------------
 * Snapshot of the audio engine's current track. String fields are already
 * charset-converted to UTF-8 (shell_cs); 'title' falls back to the file
 * stem when the tag has none, 'artist'/'album' stay empty when untagged. */
struct be_player_info
{
    bool active;                /* a playable track is loaded */
    bool paused;
    long elapsed;               /* ms into the track */
    long length;                /* ms */
    long filesize;              /* bytes */
    long freq;                  /* sample rate, Hz */
    int  bitrate;               /* kbps */
    char ext[16];               /* "mp3", "flac", ... */
    char title[96];
    char artist[96];
    char album[96];
    char path[BE_PATH_MAX];     /* raw rockbox path */
};

void be_player_info(struct be_player_info *out);
void be_player_toggle_pause(void);
void be_player_next(void);
void be_player_prev(void);
void be_player_seek(long pos_ms);

/* Path of "<dir>/<raw tag title>.lrc" for the lyric-title fallback;
 * false when there is no current track or no tag title. */
bool be_track_lrc_title_path(char *out, size_t outsz);

/* ---- current-playlist editing (playlist_viewer.c equivalents) ----------- */
/* audio state / playback */
int  be_audio_status(void);
/* status bits, mirroring audio.h (for the be_audio_status() result) */
#define BE_AUDIO_STATUS_PLAY  0x0001
#define BE_AUDIO_STATUS_PAUSE 0x0002
const char *be_audio_track_path(void);
const char *be_audio_track_title(void);
long be_audio_track_elapsed(void);
long be_audio_track_length(void);
void be_audio_pause_toggle(void);
void be_play_now(const char *path);
void be_playlist_start(int idx);
void be_playlist_shuffle_apply(bool on);
void be_volume_set(int v);
void be_settings_save(void);
/* repeat mode, mirroring global_settings.repeat_mode (settings.h enum) */
#define BE_REPEAT_OFF  0
#define BE_REPEAT_ALL  1
#define BE_REPEAT_ONE  2
int  be_player_repeat_mode(void);
bool be_player_shuffle(void);
void be_player_cycle_repeat(void);   /* cycle it like the stock setting callback */
bool be_player_toggle_shuffle(void); /* flip playlist_shuffle, return the new state */
int  be_volume_get(void);            /* global_status.volume */
void be_volume_range(int *lo, int *hi);

/* ---- shell state snapshots (status bar, theme, scrollbar) --------------- */
struct be_statusbar
{
    bool have_time;
    int  hour, min;        /* 0..23, 0..59 */
    bool timeformat_12h;   /* global_settings.timeformat == 1 */
    int  batt;             /* battery_level() */
    bool batt_graphic;     /* global_settings.battery_display == 0 */
    int  vol_pct;          /* volume as 0..100 */
    /* wifi/bt are NOT part of the snapshot: be_wifi_up() runs wpa_cli, so
     * the status bar asks for them only while the icon label exists */
};
void be_statusbar_snapshot(struct be_statusbar *out);
void be_statusbar_mode(bool *off, bool *bottom);
void be_scrollbar_prefs(bool *off, bool *left);
bool be_lcd_active(void);
int  be_ui_backdrop_preset(void);
bool be_ui_dark_mode(void);
int  be_ui_font_size(void);
bool be_show_shutdown_message(void);
uint32_t be_ticks_to_ms(int ticks);
/* DLNA cover RAM copy (the decoded pixels come from be_dlna_art_pixels) */
const unsigned char *be_dlna_art_data(size_t *len, unsigned *gen);
void be_dlna_art_lock(void);
void be_dlna_art_unlock(void);

/* ---- stock onplay context menu (opaque handles) -------------------------
 * The LVGL context menu (ui_library.c) walks rockbox's own
 * tree_onplay_menu() tree; these accessors keep every menu_item_ex
 * detail on this side of the boundary.  Handles are opaque to the UI
 * and stay valid until the next be_onplay_prepare(). */
typedef const void *be_menu_h;         /* a whole menu tree */
typedef const void *be_menu_item_h;    /* one entry of it */
enum                    /* normalized item type (be_menu_item_type) */
{
    BE_ITEM_OTHER = 0,
    BE_ITEM_MENU,
    BE_ITEM_FUNCTION,
    BE_ITEM_FUNCTION_PARAM,
};
enum                    /* onplay-style result, normalized */
{
    BE_ONPLAY_NONE = 0,
    BE_ONPLAY_START_PLAY,
    BE_ONPLAY_PLAYLIST,
    BE_ONPLAY_RELOAD_DIR,
};
int  be_menu_count(be_menu_h menu);
int  be_menu_item_type(be_menu_h menu, int i);
bool be_menu_item_visible(be_menu_h menu, int i);
const char *be_menu_item_label(be_menu_h menu, int i, char *buf, size_t bufsz);
const char *be_menu_title(be_menu_h menu);      /* NULL when there is none */
be_menu_item_h be_menu_item(be_menu_h menu, int i);
int  be_menu_item_kind(be_menu_item_h item);    /* 0 none / 1 props / 2 delete */
int  be_menu_item_run(be_menu_item_h item);     /* stock handler (job ctx);
                                                 * BE_ONPLAY_*, -1 not runnable */
int  be_onplay_result(void);                    /* BE_ONPLAY_* */
void be_onplay_prepare(const char *path, int attr);
be_menu_h be_onplay_tree_menu(void);
int  be_filetype_attr(const char *path);
int  be_attr_directory(void);
int  be_dirfilter_user(void);                   /* global_settings.dirfilter */
int  be_dirfilter_show_m3u(void);               /* the stock catalog's filter */
void be_tree_dirfilter_set(int filter);         /* temp override while open */
void be_tree_dirfilter_restore(void);           /* undo the override */
void be_tree_dirfilter_reset_to_user(void);     /* point back at the setting */
/* the playlist viewer's own context menu (stock item list via do_menu) */
int  be_playlist_viewer_menu(void);             /* picked index, -1 cancel */
/* scan the catalog directory (stock catalog_get_directory + readdir):
 * returns the count and hands over two malloc'd string arrays (titles
 * without extension, full paths); the caller frees the ARRAYS with free(),
 * the strings keep their own owners (see shell_plcat_pick) */
int  be_playlist_catalog_scan(char ***titles, char ***paths, int max);

/* tagcache build state, field by field (no struct tagcache_stat here) */
bool be_db_commit_delayed(void);
bool be_db_readyvalid(void);
int  be_db_commit_step(void);
const char *be_db_curentry(void);
int  be_db_scan_count(void);                    /* files scanned so far */
bool be_db_ask_needed(void);   /* readyvalid && !ready && nothing processed */

/* tag snapshot for the properties screen (strings valid until the next
 * be_track_tags() call) */
bool be_track_tags(const char *path);
const char *be_tag_title(void);
const char *be_tag_artist(void);
const char *be_tag_album(void);
const char *be_tag_genre(void);
int  be_tag_year(void);
long be_tag_length(void);
int  be_tag_bitrate(void);
long be_tag_frequency(void);

/* browser settings */
bool be_browser_keep_directory(void);
bool be_browser_full_paths(void);      /* show_path_in_browser == SHOW_PATH_FULL */
bool be_browser_resume_current(void);  /* browse_current */
bool be_dir_exists(const char *path);
/* tagcache / database build state */
bool be_db_usable(void);
void be_db_rebuild(void);
int  be_db_max_commit_step(void);

int  be_playlist_count(void);
int  be_playlist_resume(void);                 /* playing index, -1 if none */
bool be_playlist_track_path(int index, char *out, size_t outsz);
bool be_playlist_name(char *out, size_t outsz);/* current playlist's filename */
void be_playlist_remove(int index);            /* delete + keep playback sane */
void be_playlist_move(int from, int to);
void be_playlist_move_rows(int from, int to);  /* move + mark modified */
void be_playlist_shuffle(void);                /* original sort+randomise pair */
bool be_playlist_save(const char *name);
void be_playlist_insert_next(const char *path);/* play the track right after */

/* bridges to classic engines that render through the native-UI hooks
 * (do_menu / option_screen / yesno take over while running as a job) */
void be_onplay_insert_menu(const char *path);  /* onplay "Playing Next..."  */
void be_onplay_addto_menu(const char *path);   /* onplay "Add to Playlist"  */
void be_playlist_save_prompt(void);            /* save_playlist_screen()    */
void be_playlist_viewer_settings(void);        /* do_menu(viewer_settings)  */

/* root-menu rows, stock counterparts (render through the native do_menu) */
void be_settings_menu(void);                   /* 设置 → main_menu_          */
void be_system_menu(void);                     /* 系统 → info_menu           */
void be_plugin_menu(void);                     /* 插件 → plugin_menu         */
void be_bluetooth_menu(void);                  /* 蓝牙 → bluetooth_root_item */
void be_wifi_menu(void);                       /* 网络 → wifi_root_item      */
void be_bookmark_load(void);

/* power: the control centre's 关机 / 重启 rows (confirm first, in the UI) */
void be_keyclick(void);
/* start screen target, normalized (root_menu GO_TO_* translated here so
 * screens never see the enum) */
enum
{
    BE_START_HOME = 0,     /* root/home, incl. GO_TO_ROOT / GO_TO_MAINMENU */
    BE_START_FILES,
    BE_START_DATABASE,
    BE_START_WPS,
    BE_START_BOOKMARKS,
    BE_START_PLUGINS,
    BE_START_PLAYLISTS,
};
int be_start_screen(void);   /* returns a BE_START_* value */
void be_playlist_viewer_opts(int *indices, int *track_display);
void be_playlist_row_label(int idx, bool current, char *out, size_t outsz);
void be_power_off(void);
void be_reboot(void);

/* control centre */
bool be_wifi_on(void);
void be_wifi_toggle(void);
bool be_tube_on(void);
void be_tube_toggle(void);
void be_tube_set(int mode);
int be_tube_mode(void);
int  be_volume_pct_get(void);
void be_volume_set_pct(int pct);
int  be_brightness_get(void);
void be_brightness_set(int pct);
bool be_media_active(void);
bool be_media_is_radio(void);
bool be_media_playing(void);
void be_media_toggle(void);
void be_media_step(int dir);
void be_bt_menu_open(void);
void be_resume_playback(void);
void be_bt_disconnect(void);
void be_netfm_name(char *out, size_t sz);
void be_sound_menu(void);
void be_display_menu(void);
int be_root_items(const char **keys, const char **labels,
                  unsigned char *enabled, int max);
void be_root_apply(const char **keys, int n);
                   /* 最近书签 → load menu       */
void be_shortcut_menu(void);                   /* 快捷方式 → do_shortcut_menu */

/* the equalizer graphical editor's stock data accessors live in
 * ui_stock.h (ui_stock.c is the stock-UI bridge) */

/* network-source status screens (netfm / bt-rx / dlna); struct types are
 * declared in netfm_stream.h / dlna/dlna_stream.h */
struct netfm_stream_status;
struct dlna_stream_status;
void be_netfm_get_status(struct netfm_stream_status *st);
int  be_netfm_step(int dir);
const char *be_netfm_last_name(void);
bool be_netfm_active(void);
bool be_dlna_active(void);
void be_dlna_name(char *out, size_t sz);
void be_dlna_time(long *pos, long *dur);
void be_dlna_seek(int ms);
void be_dlna_stop(void);
bool be_dlna_art_decode(const unsigned char *data, size_t len, unsigned gen);
const void *be_dlna_art_pixels(int *w, int *h, unsigned *gen);
bool be_bt_rx_active(void);                  /* prev/next station         */
bool be_wifi_up(void);                        /* wifi powered & up         */
bool be_bt_active(void);                      /* bt output or rx active    */
void be_netfm_stop(void);
const char *be_bt_rx_state_name(void);
bool be_bt_rx_connected(void);
void be_bt_rx_peer(char *buf, size_t sz);
const char *be_bt_codec(void);
int be_bt_rx_rate(void);
void be_dlna_get_status(struct dlna_stream_status *st);

#endif
