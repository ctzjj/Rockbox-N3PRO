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
#include "settings_list.h"

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
/* tagcache / database build state */
bool be_db_usable(void);
struct tagcache_stat *be_db_stat(void);
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
int be_start_screen(void);
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

/* equalizer graphical editor: stock gain editor data + stock apply */
const struct int_setting *be_eq_gain_setting(void);
void be_eq_apply(void);
void be_eq_apply_band(int band);
const char *be_eq_band_name(int band, char *buf, size_t len);

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
const struct int_setting *be_eq_q_setting(void);
const struct int_setting *be_eq_cutoff_setting(void);

#endif
