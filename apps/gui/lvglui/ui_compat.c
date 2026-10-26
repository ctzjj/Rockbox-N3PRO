/* ui_compat.c - no-op stand-ins for classic-UI functions that the frozen
 * plugin ABI and settings machinery still reference after the classic UI
 * sources were dropped from the LVGL build. Compiled only when
 * HAVE_LVGL_UI is defined (see apps/SOURCES). */

#include "config.h"
#include "menu.h"
#include "root_menu.h"
#include "playlist_menu.h"
#include "playlist_viewer.h"
#include "debug_menu.h"

void shell_playlist_show(void);        /* ui_library.c (keeps LVGL out here) */

/* "View current playlist" from the playlist menu: show our LVGL viewer
 * instead of the classic playlist_viewer.c screen. */
enum playlist_viewer_result playlist_viewer_ex(const char *filename,
                                               int *most_recent_selection)
{
    (void)filename;
    (void)most_recent_selection;
    shell_playlist_show();
    return PLAYLIST_VIEWER_OK;
}

/* ---- skin engine stand-ins (skin sources dropped from the LVGL build) --- */
#include "wps.h"
#include "skin_engine/skin_engine.h"
#include "statusbar-skinned.h"
#include "list.h"
#include "audio.h"
#include "cuesheet.h"
#include "misc.h"
#include "playlist.h"
#include "settings.h"

/* playback/misc code writes id3/ff-rewind info here for the (removed) WPS;
 * the LVGL player reads audio_* APIs directly */
struct wps_state wps_state;

void gui_sync_skin_init(void)
{
}

void sb_skin_init(void)
{
}

void settings_apply_skins(void)
{
}

/* ".wps file selected in the file browser" path (unreachable without the
 * classic browser) */
int ft_apply_skin_file(char *buf, char *file)
{
    (void)buf;
    (void)file;
    return 0;
}

/* statusbar/skin API used by core code (menu, misc, action, playlist):
 * every call is dead without a skin, so plain no-ops */
void sb_skin_force_next_update(void)
{
}

bool sb_set_title_text(const char *title, enum themable_icons icon,
                       enum screen_type screen)
{
    (void)title;
    (void)icon;
    (void)screen;
    return false;
}

const char *sb_get_persistent_title(enum screen_type screen)
{
    (void)screen;
    return NULL;
}

bool sb_set_persistent_title(const char *title, enum themable_icons icon,
                             enum screen_type screen)
{
    (void)title;
    (void)icon;
    (void)screen;
    return false;
}

void sb_skin_has_title(enum screen_type screen)
{
    (void)screen;
}

const char *sb_get_title(enum screen_type screen)
{
    (void)screen;
    return NULL;
}

enum themable_icons sb_get_icon(enum screen_type screen)
{
    (void)screen;
    return Icon_NOICON;
}

void sb_bypass_touchregions(bool enable)
{
    (void)enable;
}

int sb_touch_to_button(int context)
{
    (void)context;
    return 0;
}

void skin_update(enum skinnable_screens skin, enum screen_type screen,
                 unsigned int update_type)
{
    (void)skin;
    (void)screen;
    (void)update_type;
}

void skin_mark_dirty(enum screen_type screen)
{
    (void)screen;
}

bool skinlist_draw(struct screen *display, struct gui_synclist *list)
{
    (void)display;
    (void)list;
    return false;
}

bool skinlist_get_item(struct screen *display, struct gui_synclist *list,
                       int x, int y, int *item)
{
    (void)display;
    (void)list;
    (void)x;
    (void)y;
    (void)item;
    return false;
}

void skinlist_set_cfg(enum screen_type screen, struct listitem_viewport_cfg *cfg)
{
    (void)screen;
    (void)cfg;
}

int skinlist_get_line_count(enum screen_type screen, struct gui_synclist *list)
{
    (void)screen;
    (void)list;
    return 0;
}

void wps_do_action(enum wps_do_action_type action, bool updatewps)
{
    (void)action;
    (void)updatewps;
}

void wps_playlist_percent_prepare(void)
{
}

/* classic WPS state: the LVGL player has the same id3 as the audio engine */
struct wps_state *get_wps_state(void)
{
    wps_state.id3 = audio_current_track();
    return &wps_state;
}

/* verbatim from the classic WPS (apps/gui/wps.c, not built here):
 * restart the track unless we are still inside its first skip_thresh ms */
void wps_prev_track(unsigned long skip_thresh)
{
    struct wps_state *state = get_wps_state();

    if (!state->id3 || state->id3->elapsed < skip_thresh)
    {
        audio_prev();
        return;
    }
    else
    {
        if (state->id3->cuesheet)
        {
            curr_cuesheet_skip(state->id3->cuesheet, -1, state->id3->elapsed);
            return;
        }

        audio_pre_ff_rewind();
        audio_ff_rewind(0);
    }
}

static void wps_next_track(void)
{
    struct wps_state *state = get_wps_state();

    /* take care of if we're playing a cuesheet */
    if (state->id3->cuesheet)
    {
        if (curr_cuesheet_skip(state->id3->cuesheet, 1, state->id3->elapsed))
        {
            /* if the result was false, then we really want
               to skip to the next track */
            return;
        }
    }

    audio_next();
}

/* verbatim from the classic WPS: the prev/next buttons honour the
 * skip-length, prevent-skip and rewind-across-tracks settings */
void wps_play_hop(int direction)
{
    struct wps_state *state = get_wps_state();
    struct cuesheet *cue = state->id3->cuesheet;
    long step = global_settings.skip_length * 1000;
    long elapsed = state->id3->elapsed;
    long remaining = state->id3->length - elapsed;

    /* if cuesheet is active, then we want the current tracks end instead of
     * the total end */
    if (cue && (cue->curr_track_idx + 1 < cue->track_count))
    {
        int next = cue->curr_track_idx + 1;
        struct cue_track_info *t = &cue->tracks[next];
        remaining = t->offset - elapsed;
    }

    if (step < 0)
    {
        if (direction < 0)
        {
            wps_prev_track(DEFAULT_SKIP_THRESH);
            return;
        }
        else if (remaining < DEFAULT_SKIP_THRESH * 2)
        {
            wps_next_track();
            return;
        }
        else
            elapsed += (remaining - DEFAULT_SKIP_THRESH * 2);
    }
    else if (!global_settings.prevent_skip &&
           (!step ||
            (direction > 0 && step >= remaining) ||
            (direction < 0 && elapsed < DEFAULT_SKIP_THRESH)))
    {   /* Do normal track skipping */
        if (direction > 0)
            wps_next_track();
        else if (direction < 0)
        {
            if (step > 0 && global_settings.rewind_across_tracks &&
                elapsed < DEFAULT_SKIP_THRESH && playlist_check(-1))
            {
                bool audio_paused = (audio_status() & AUDIO_STATUS_PAUSE) ? true : false;
                if (!audio_paused)
                    audio_pause();
                audio_prev();
                audio_ff_rewind(-step);
                if (!audio_paused)
                    audio_resume();
                return;
            }

            wps_prev_track(DEFAULT_SKIP_THRESH);
        }
        return;
    }
    else if (direction == 1 && step >= remaining)
    {
        system_sound_play(SOUND_TRACK_NO_MORE);
        return;
    }
    else if (direction == -1 && elapsed < step)
    {
        elapsed = 0;
    }
    else
    {
        elapsed += step * direction;
    }
    audio_pre_ff_rewind();
    audio_ff_rewind(elapsed);
}
