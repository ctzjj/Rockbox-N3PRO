/* ui_backend.c -- rockbox adapter implementation.
 * Everything here talks to rockbox (tagcache, playlist, file tree,
 * metadata, artwork); nothing here talks to LVGL. */

#include "config.h"
#include "misc.h"
#include "tagcache.h"
#include "playlist.h"
#include "playlist_catalog.h"
#include "playlist_menu.h"
#include "menu.h"
#include "menus/exported_menus.h"
#include "onplay.h"
#include "backlight.h"
#include "audiohw.h"
#include "bt_audio.h"
#include "bookmark.h"
#include "bluetooth.h"
#include "wifi_menu.h"
#include "shortcuts.h"
#include "settings_list.h"
#include "root_menu.h"
#include "settings.h"
#include "menus/eq_menu.h"
#include "thread.h"
#include "kernel.h"
#include "metadata.h"
#include "dir.h"
#include "file.h"
#include "tree.h"
#include "tagtree.h"
#include "filetree.h"
#include "netfm_stream.h"
#include "netfm_playback.h"
#include "netfm.h"
#include "wifi_hal.h"
#include "filetypes.h"
#include "albumart.h"
#include "jpeg_load.h"
#include "bmp.h"
#include "string-extra.h"
#include "audio.h"
#include "wps.h"
#include "sound.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "usb.h"
#include "bt_input.h"
#include "system.h"
#include "powermgmt.h"
#include "power.h"
#include "dlna/dlna_output.h"
#include "dlna/dlna_stream.h"
#include "lang.h"
#include "gui/splash.h"
#include "ui_backend.h"

/* forward: local playback refusal with a toast (defined near
 * be_playlist_start; used by the earlier start paths too) */
static bool be_playback_allowed(void);

/* charset conversion (ui_charset.c) -- declared locally so this layer
 * keeps zero LVGL/UI includes */
const char *shell_cs(char *dst, size_t dstsz, const char *src);
/* toast shown while a blocking shutdown runs (ui_shell.c) */
void shell_toast(int ticks, const char *msg, const char *icon);
/* classic WPS prev helper carried over in ui_compat.c */
void wps_prev_track(unsigned long skip_thresh);

#define LIB_MAX_ITEMS  2000      /* query result guard */
void be_list_free(be_list_t *t)
{
    for (int i = 0; i < t->count; i++)
    {
        free(t->title[i]);
        free(t->sub[i]);
        free(t->path[i]);
    }
    free(t->title);
    free(t->sub);
    free(t->path);
    memset(t, 0, sizeof *t);
}

/* ---- artwork: embedded JPEG first, then folder images ------------------- */

#include "../../../lib/lvgl/src/extra/libs/png/lodepng.h"

/* decode any PNG blob into a w x h RGB565 buffer (nearest-neighbour scale) */
static be_img_t *be_png_blob(const unsigned char *data, size_t len, int w, int h)
{
    unsigned pw = 0, ph = 0;
    unsigned char *rgba = NULL;
    if (lodepng_decode32(&rgba, &pw, &ph, data, len) != 0 || !rgba ||
        !pw || !ph)
    {
        free(rgba);
        return NULL;
    }
    void *out = malloc((size_t)w * h * 2);
    if (!out)
    {
        free(rgba);
        return NULL;
    }
    for (int y = 0; y < h; y++)
    {
        unsigned sy = (unsigned)y * ph / (unsigned)h;
        for (int x = 0; x < w; x++)
        {
            unsigned sx = (unsigned)x * pw / (unsigned)w;
            const unsigned char *p = rgba + 4 * ((size_t)sy * pw + sx);
            ((uint16_t *)out)[(size_t)y * w + x] =
                (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) |
                           (p[2] >> 3));
        }
    }
    free(rgba);
    be_img_t *a = malloc(sizeof *a);
    if (!a)
    {
        free(out);
        return NULL;
    }
    a->pixels = out;
    a->w = w;
    a->h = h;
    return a;
}

static be_img_t *be_png_embed(const char *path, long pos, long size,
                              int w, int h)
{
    if (size <= 0)
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    unsigned char *data = malloc((size_t)size);
    if (!data)
    {
        close(fd);
        return NULL;
    }
    lseek(fd, pos, SEEK_SET);
    ssize_t rd = read(fd, data, (size_t)size);
    close(fd);
    be_img_t *im = (rd == size) ? be_png_blob(data, (size_t)size, w, h) : NULL;
    free(data);
    return im;
}

static be_img_t *be_png_file(const char *path, int w, int h)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0)
    {
        close(fd);
        return NULL;
    }
    unsigned char *data = malloc((size_t)size);
    if (!data)
    {
        close(fd);
        return NULL;
    }
    ssize_t rd = read(fd, data, (size_t)size);
    close(fd);
    be_img_t *im = (rd == size) ? be_png_blob(data, (size_t)size, w, h) : NULL;
    free(data);
    return im;
}


static be_img_t *be_art_from_bm(struct bitmap *bm, void *buf)
{
    if (bm->width <= 0 || bm->height <= 0 || !bm->data)
    {
        free(buf);
        return NULL;
    }
    be_img_t *a = malloc(sizeof *a);
    if (!a)
    {
        free(buf);
        return NULL;
    }
    a->pixels = buf;
    a->w = bm->width;
    a->h = bm->height;
    return a;
}

/* Decode a JPEG/PNG held in memory (DLNA cover art).  No file involved,
 * so nothing is written to the SD card or the tmp ramdisk. */
be_img_t *be_art_blob(const unsigned char *data, size_t len, int w, int h)
{
    if (!data || len < 8 || w <= 0 || h <= 0)
        return NULL;
    if (data[0] == 0x89 && data[1] == 'P')
        return be_png_blob((unsigned char *)data, len, w, h);
    if (data[0] == 0xFF && data[1] == 0xD8)
    {
        size_t maxsize = (size_t)w * h * 2 + JPEG_DECODE_OVERHEAD;
        void *buf = malloc(maxsize);
        struct bitmap bm;
        if (!buf)
            return NULL;
        memset(&bm, 0, sizeof bm);
        bm.data = buf;
        bm.width = w;
        bm.height = h;
        if (decode_jpeg_mem((unsigned char *)data, len, &bm, maxsize,
                            FORMAT_NATIVE | FORMAT_RESIZE, NULL) > 0)
            return be_art_from_bm(&bm, buf);
        free(buf);
    }
    return NULL;
}

/* Decode a (possibly embedded) JPEG into a w x h bitmap.  Ask the decoder
 * for the exact buffer size first: JPEG_DECODE_OVERHEAD alone is not enough
 * for the resize line buffers once the source is much bigger than the
 * target, and the decode then quietly returns 0 (a missing cover). */
static be_img_t *be_art_jpeg(const char *path, int offset, unsigned long len,
                             int w, int h, int fmt)
{
    struct bitmap probe;
    memset(&probe, 0, sizeof probe);
    probe.width = w;
    probe.height = h;
    /* the decoder stores its state at bm->data, so the probe needs a real
     * buffer; JPEG_DECODE_OVERHEAD is by definition big enough */
    probe.data = malloc(JPEG_DECODE_OVERHEAD);
    if (!probe.data)
        return NULL;
    int need = clip_jpeg_file(path, offset, len, &probe, JPEG_DECODE_OVERHEAD,
                              fmt | FORMAT_RETURN_SIZE, NULL);
    free(probe.data);
    if (need <= 0)
        return NULL;
    void *buf = malloc(need + 128);   /* alignment slack for the second run */
    if (!buf)
        return NULL;
    struct bitmap bm;
    memset(&bm, 0, sizeof bm);
    bm.data = buf;
    bm.width = w;
    bm.height = h;
    int rc = clip_jpeg_file(path, offset, len, &bm, need + 128, fmt, NULL);
    if (rc > 0)
        return be_art_from_bm(&bm, buf);
    free(buf);
    return NULL;
}

be_img_t *be_track_art(const char *path, int w, int h)
{
    static struct mp3entry id3;          /* big; reuse, UI thread only */
    memset(&id3, 0, sizeof id3);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    bool ok = get_metadata(&id3, fd, path);
    close(fd);
    if (!ok)
        return NULL;

    size_t maxsize = (size_t)w * h * 2 + JPEG_DECODE_OVERHEAD;
    /* stretch to fill the tile (user preference: 平铺铺满) */
    const int art_fmt = FORMAT_NATIVE | FORMAT_RESIZE;
    struct bitmap bm;

    /* embedded PNG (FLAC/APE/MP4 covers are often PNG) */
    if (id3.has_embedded_albumart && id3.albumart.size > 0 &&
        (id3.albumart.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_PNG)
    {
        be_img_t *im = be_png_embed(path, id3.albumart.pos,
                                    (long)id3.albumart.size, w, h);

        if (im)
            return im;
    }

    /* embedded JPEG */

    if (id3.has_embedded_albumart && id3.albumart.size > 0 &&
        (id3.albumart.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_JPG)
    {
        be_img_t *im = be_art_jpeg(path, id3.albumart.pos,
                                   (unsigned long)id3.albumart.size,
                                   w, h, art_fmt);
        if (im)
            return im;
    }

    /* stock artwork lookup: same folder and its parent directory */
    char artpath[MAX_PATH];
    artpath[0] = 0;
    struct dim adim = { w, h };
    bool found = find_albumart(&id3, artpath, sizeof artpath, &adim);
    if (found)
    {
        const char *ext = strrchr(artpath, '.');
        if (ext && strcasecmp(ext, ".jpg") == 0)
        {
            be_img_t *im = be_art_jpeg(artpath, 0, 0, w, h, art_fmt);
            if (im)
                return im;
        }
        else if (ext && strcasecmp(ext, ".png") == 0)
        {
            be_img_t *im = be_png_file(artpath, w, h);
            if (im)
                return im;
        }
        else if (ext && strcasecmp(ext, ".bmp") == 0)
        {
            void *buf = malloc(maxsize);
            if (buf)
            {
                memset(&bm, 0, sizeof bm);
                bm.data = buf;
                bm.width = w;
                bm.height = h;
                if (read_bmp_file(artpath, &bm, maxsize,
                                  art_fmt, NULL) > 0)
                    return be_art_from_bm(&bm, buf);
                free(buf);
            }
        }
    }
    return NULL;
}

void be_img_free(be_img_t *a)
{
    if (a)
    {
        free(a->pixels);
        free(a);
    }
}

/* ---- playback ------------------------------------------------------------ */
/* Building the playlist reads metadata for every track.  Run it in the
 * caller's (GUI) thread like the classic UI does: the job yields every few
 * tracks, and a background thread used to trip the kernel's "abandon ship"
 * check in thread_exit() when it was the last thing running. */

typedef struct
{
    char **paths;
    int count;
    int start;
} playjob_t;

static void play_build_run(playjob_t *j)
{
    struct playlist_insert_context ctx;
    playlist_create(NULL, NULL);
    if (playlist_insert_context_create(NULL, &ctx, PLAYLIST_INSERT_LAST,
                                       false, false) == 0)
    {
        for (int i = 0; i < j->count; i++)
        {
            if (playlist_insert_context_add(&ctx, j->paths[i]) < 0)
                break;
            if ((i & 15) == 0)
                yield();
        }
        playlist_insert_context_release(&ctx);
    }
    if (be_playback_allowed())
        playlist_start(j->start, 0, 0);
}

void be_play(const be_list_t *t, int start)
{
    if (t->count <= 0 || start < 0 || start >= t->count)
        return;

    /* Big lists take a while (tag reads): tell the user before we block. */
    if (t->count > 64)
        shell_toast(0, str(LANG_N3_LOADING_PLAYLIST), NULL);

    playjob_t job;
    job.paths = t->path;                /* borrowed: valid for this call */
    job.count = t->count;
    job.start = start;
    play_build_run(&job);
}

/* Play a playlist file exactly like the file browser does (dir + name,
 * through the original ft_play_playlist: bookmarks, warnings, shuffle). */
bool be_play_playlist_file(const char *full)
{
    char path[MAX_PATH];
    strlcpy(path, full, sizeof path);
    char *slash = strrchr(path, '/');
    if (!slash)
        return false;
    char dir[MAX_PATH];
    size_t dl = (size_t)(slash - path);
    if (dl == 0)
        strlcpy(dir, "/", sizeof dir);
    else
    {
        memcpy(dir, path, dl);
        dir[dl] = '\0';
    }
    return ft_play_playlist(path, dir, slash + 1);
}

/* ---- file system ---------------------------------------------------------- */

bool be_is_audio(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    static const char *ext[] = {
        "mp3", "mp2", "mp1", "flac", "m4a", "m4b", "mp4", "aac", "ogg",
        "oga", "opus", "spx", "wav", "wv", "ape", "mpc", "aif", "aiff",
        "wma", "alac", "sid", "mod", "nsf", "ay", "vgm", NULL
    };
    for (int i = 0; ext[i]; i++)
        if (!strcasecmp(dot + 1, ext[i]))
            return true;
    return false;
}

bool be_is_playlist_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8"));
}

static const char *be_dir_icon(int attr)
{
    if (attr & ATTR_DIRECTORY)
        return "d";
    if ((attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
        return "a";
    return "f";
}

/* Same rule as ext_stripit() in tree.c: when the extension is hidden the
 * stock browser strips it for display only (the path keeps it). */
static bool be_ext_strip(const struct entry *e)
{
    if (e->attr & ATTR_DIRECTORY)
        return false;
    switch (global_settings.show_filename_ext)
    {
        case 0:
            return true;                 /* off */
        case 1:
            return false;                /* on */
        case 2:
            return filetype_supported(e->attr);
        default:
            return global_settings.dirfilter != SHOW_ALL;
    }
}

static int be_dir_scan_impl(const char *path, char ***pnames,
                            char ***ppaths, char ***picons)
{
    struct tree_context *c = tree_get_context();
    c->is_browsing = false;
    c->browse = NULL;
    c->out_of_tree = 0;
    strlcpy(c->currdir, path, sizeof c->currdir);
    c->dirlevel = (strcmp(path, "/") == 0) ? 0 : 1;   /* SHOW_PLUGINS only */
    if (ft_load(c, NULL) < 0)          /* 0 on success, count in filesindir */
        return -1;
    int n = c->filesindir;

    char **names = calloc(n ? n : 1, sizeof(char *));
    char **paths = calloc(n ? n : 1, sizeof(char *));
    char **icons = calloc(n ? n : 1, sizeof(char *));
    if (!names || !paths || !icons)
    {
        free(names); free(paths); free(icons);
        return -1;
    }

    tree_lock_cache(c);                    /* pin so the cache cannot move */
    struct entry *ents = tree_get_entries(c);
    for (int i = 0; i < n; i++)
    {
        char full[MAX_PATH];
        char nm[MAX_PATH];
        if (strcmp(path, "/") == 0)
            snprintf(full, sizeof full, "/%s", ents[i].name);
        else
            snprintf(full, sizeof full, "%s/%s", path, ents[i].name);
        strlcpy(nm, ents[i].name, sizeof nm);
        if (be_ext_strip(&ents[i]))
        {
            char *dot = strrchr(nm, '.');
            if (dot && dot != nm)
                *dot = '\0';
        }
        names[i] = strdup(nm);
        paths[i] = strdup(full);
        icons[i] = strdup(be_dir_icon(ents[i].attr));
    }
    tree_unlock_cache(c);

    *pnames = names;
    *ppaths = paths;
    *picons = icons;
    return n;
}

int be_dir_scan(const char *path, char ***pnames, char ***ppaths,
                char ***picons)
{
    struct tree_context *c = tree_get_context();
    int *prev = c->dirfilter;
    int prev_sort = c->sort_dir;
    c->dirfilter = &global_settings.dirfilter;   /* leave DB mode */
    /* ft_load() sorts directories with c->sort_dir (not global_settings),
     * which only the stock browser used to refresh - so apply it here or a
     * changed "File View" sort never shows up in the file browser. */
    c->sort_dir = global_settings.sort_dir;
    int n = be_dir_scan_impl(path, pnames, ppaths, picons);
    c->sort_dir = prev_sort;
    c->dirfilter = prev;
    return n;
}

int be_dir_scan_filter(const char *path, int dirfilter, char ***pnames,
                       char ***ppaths, char ***picons)
{
    struct tree_context *c = tree_get_context();
    int *prev = c->dirfilter;
    int prev_sort = c->sort_dir;
    int local = dirfilter;
    c->dirfilter = &local;
    c->sort_dir = global_settings.sort_dir;
    int n = be_dir_scan_impl(path, pnames, ppaths, picons);
    c->sort_dir = prev_sort;
    c->dirfilter = prev;
    return n;
}

/* ---- database browsing through the stock tagtree ------------------------ */
/* Exactly the state machine the original browser drives (tree.c): the
 * tree context carries the tagtree table/extra/level, entries come from
 * tagtree_get_entry_name/attr, entering and backing out go through
 * tagtree_enter/exit and playback is started by tagtree itself. */
static int be_db_dirfilter = SHOW_ID3DB;

static struct tree_context *be_db_tc(void)
{
    struct tree_context *c = tree_get_context();
    c->is_browsing = false;
    c->browse = NULL;
    c->out_of_tree = 0;
    c->dirfilter = &be_db_dirfilter;
    return c;
}

void be_db_root(void)
{
    struct tree_context *c = be_db_tc();
    c->currtable = 0;
    c->currextra = 0;
    c->dirlevel = 0;
    c->selected_item = 0;
    tagtree_load(c);
}

int be_db_scan(char ***pnames, char ***picons)
{
    struct tree_context *c = be_db_tc();
    if (!tagcache_is_usable())
        return -1;
    if (tagtree_load(c) < 0)
        return -1;
    int n = c->filesindir;
    char **names = calloc(n ? n : 1, sizeof(char *));
    char **icons = calloc(n ? n : 1, sizeof(char *));
    if (!names || !icons)
    {
        free(names);
        free(icons);
        return -1;
    }
    char buf[MAX_PATH];
    for (int i = 0; i < n; i++)
    {
        buf[0] = 0;
        tagtree_get_entry_name(c, i, buf, sizeof buf);
        names[i] = strdup(buf);
        c->selected_item = i;
        icons[i] = strdup(tagtree_get_attr(c) == FILE_ATTR_AUDIO ? "a" : "d");
    }
    *pnames = names;
    *picons = icons;
    return n;
}

const char *be_db_title(void)
{
    return tagtree_get_title(be_db_tc());
}

int be_db_enter(int idx)
{
    struct tree_context *c = be_db_tc();
    c->selected_item = idx;
    return tagtree_enter(c, true);
}

void be_db_back(void)
{
    struct tree_context *c = be_db_tc();
    if (c->dirlevel > 0)
        tagtree_exit(c, true);
    /* the entry cache still holds the level we just left; reload the
     * parent's entries so indices from the parent list map correctly */
    tagtree_load(c);
}

int be_read_text(const char *path, char *buf, int sz)
{
    if (sz <= 1)
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, buf, sz - 1);
    close(fd);
    return n;
}

int be_delete(const char *path)
{
    if (dir_exists(path))
    {
        DIR *d = opendir(path);
        if (!d)
            return -1;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (e->d_name[0] == '.')
                continue;
            char full[MAX_PATH];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            be_delete(full);
        }
        closedir(d);
        return rmdir(path);
    }
    return remove(path);
}

bool be_file_info(const char *path, unsigned long *psize, time_t *pmt)
{
    char dir[MAX_PATH];
    strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return false;
    const char *name = slash + 1;
    if (!name[0])
        return false;
    if (slash == dir)
        dir[1] = '\0';
    else
        *slash = '\0';
    DIR *dp = opendir(dir);
    if (!dp)
        return false;
    struct dirent *e;
    bool ok = false;
    while ((e = readdir(dp)) != NULL)
    {
        if (strcmp(e->d_name, name))
            continue;
        struct dirinfo di = dir_get_info(dp, e);
        if (psize)
            *psize = (unsigned long)di.size;
        if (pmt)
            *pmt = di.mtime;
        ok = true;
        break;
    }
    closedir(dp);
    return ok;
}

/* ---- playlist catalog ------------------------------------------------------ */

int be_playlists_scan(char ***pnames, char ***ppaths)
{
    char dir[MAX_PATH];
    catalog_get_directory(dir, sizeof dir);
    if (!dir[0])
        strlcpy(dir, "/Playlists", sizeof dir);

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
    *pnames = tt;
    *ppaths = pp;
    return n;
}

void be_str_arrays_free(char **a, char **b, int n)
{
    for (int i = 0; i < n; i++)
    {
        free(a[i]);
        free(b[i]);
    }
    free(a);
    free(b);
}

/* ---- now-playing state --------------------------------------------------- */

/* "dir/name.ext" -> "name" (malloc'd buffers; mirrors the UI helper) */
static void be_file_stem(const char *path, char *stem, size_t sz)
{
    const char *slash = strrchr(path, '/');
    const char *name = (slash && slash[1]) ? slash + 1 : path;
    strlcpy(stem, name, sz);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem)
        *dot = '\0';
}

void be_player_info(struct be_player_info *out)
{
    memset(out, 0, sizeof *out);
    struct mp3entry *id3 = audio_current_track();
    int st = audio_status();
    if (!id3 || !id3->path[0] ||
        !(st & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)))
        return;

    out->active = true;
    out->paused = (st & AUDIO_STATUS_PAUSE) != 0;
    out->elapsed = id3->elapsed;
    out->length = id3->length;
    out->filesize = id3->filesize;
    out->freq = id3->frequency;
    out->bitrate = id3->bitrate;
    strlcpy(out->path, id3->path, sizeof out->path);
    const char *dot = strrchr(id3->path, '.');
    snprintf(out->ext, sizeof out->ext, "%s", dot ? dot + 1 : "");

    char stem[MAX_PATH];
    be_file_stem(id3->path, stem, sizeof stem);
    shell_cs(out->title, sizeof out->title,
             id3->title && id3->title[0] ? id3->title : stem);
    if (id3->artist && id3->artist[0])
        shell_cs(out->artist, sizeof out->artist, id3->artist);
    if (id3->album && id3->album[0])
        shell_cs(out->album, sizeof out->album, id3->album);
}

void be_player_toggle_pause(void)
{
    /* same call the classic WPS made for its play/pause button: handles
     * the pause-rewind setting and saves the resume info */
    extern void wps_do_action(enum wps_do_action_type action, bool updatewps);
    wps_do_action(WPS_PLAYPAUSE, true);
}

void be_player_next(void)
{
    /* classic WPS next button: honours skip length / prevent skip */
    extern void wps_play_hop(int direction);
    wps_play_hop(1);
}

void be_player_prev(void)
{
    /* classic WPS prev button: same call, same settings */
    extern void wps_play_hop(int direction);
    wps_play_hop(-1);
}

void be_player_seek(long pos_ms)
{
    audio_pre_ff_rewind();
    audio_ff_rewind(pos_ms);
}

bool be_track_lrc_title_path(char *out, size_t outsz)
{
    struct mp3entry *id3 = audio_current_track();
    if (!id3 || !id3->path[0] || !id3->title || !id3->title[0])
        return false;
    char dir[MAX_PATH];
    strlcpy(dir, id3->path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return false;
    *slash = '\0';
    snprintf(out, outsz, "%s/%s.lrc", dir, id3->title);
    return true;
}

/* ---- current-playlist editing (playlist_viewer.c equivalents) ----------- */

int be_playlist_count(void)
{
    return playlist_amount();
}

int be_playlist_resume(void)
{
    int idx = -1;
    playlist_get_resume_info(&idx);
    return idx;
}

bool be_playlist_track_path(int index, char *out, size_t outsz)
{
    struct playlist_track_info info;
    if (!out || !outsz)
        return false;
    if (playlist_get_track_info(playlist_get_current(), index, &info) < 0)
        return false;
    strlcpy(out, info.filename, outsz);
    return true;
}

bool be_playlist_name(char *out, size_t outsz)
{
    struct playlist_info *pl = playlist_get_current();
    if (!out || !outsz || !pl || !pl->filename[0])
        return false;
    strlcpy(out, pl->filename, outsz);
    return true;
}

/* Mirrors playlist_viewer.c's delete_track(): drop the track, then keep the
 * engine sane (stop when empty, otherwise advance to the next track). */
void be_playlist_remove(int index)
{
    struct playlist_info *pl = playlist_get_current();
    if (!pl || index < 0 || index >= playlist_amount())
        return;
    bool was_playing = (index == be_playlist_resume()) &&
        (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE));
    playlist_delete(pl, index);
    if (was_playing)
    {
        if (playlist_amount_ex(pl) <= 0)
            audio_stop();
        else
            audio_play(0, 0);
    }
}

void be_playlist_move(int from, int to)
{
    struct playlist_info *pl = playlist_get_current();
    int n = playlist_amount();
    if (!pl || from < 0 || to < 0 || from >= n || to >= n || from == to)
        return;
    playlist_move(pl, from, to);
}

/* Commit a move like playlist_viewer.c's move-mode: move, then mark the
 * playlist modified so it is offered for saving. */
void be_playlist_move_rows(int from, int to)
{
    struct playlist_info *pl = playlist_get_current();
    int n = playlist_amount();
    if (!pl || from < 0 || to < 0 || from >= n || to >= n || from == to)
        return;
    playlist_move(pl, from, to);
    playlist_set_modified(pl, true);
}

void be_playlist_shuffle(void)
{
    struct playlist_info *pl = playlist_get_current();
    if (!pl)
        return;
    /* the exact pair used by playlist_viewer.c */
    playlist_sort(pl, !pl);
    playlist_randomise(pl, (unsigned)current_tick, !pl);
}

bool be_playlist_save(const char *name)
{
    struct playlist_info *pl = playlist_get_current();
    if (!pl || !name || !name[0])
        return false;
    char buf[MAX_PATH];
    strlcpy(buf, name, sizeof buf);
    return playlist_save(pl, buf) >= 0;
}

void be_playlist_insert_next(const char *path)
{
    if (path && path[0])
        playlist_insert_track(NULL, path, PLAYLIST_INSERT, false, true);
}

/* ---- bridges to classic engines ------------------------------------------
 * The classic screens inside these calls divert to the native UI through
 * the HAVE_LVGL_UI hooks (do_menu, option_screen, yesno) because backend
 * calls always run inside lvgui_run_job(). */
void be_onplay_insert_menu(const char *path)
{
    onplay_show_playlist_menu(path, FILE_ATTR_AUDIO, NULL);
}

void be_onplay_addto_menu(const char *path)
{
    onplay_show_playlist_cat_menu(path, FILE_ATTR_AUDIO, NULL);
}

void be_playlist_save_prompt(void)
{
    save_playlist_screen(playlist_get_current());
}

/* ---- audio state / playback (UI must not touch core audio directly) --- */
int be_audio_status(void)
{
    return audio_status();
}

const char *be_audio_track_path(void)
{
    struct mp3entry *id3 = audio_current_track();
    return (id3 && id3->path[0]) ? id3->path : NULL;
}

const char *be_audio_track_title(void)
{
    struct mp3entry *id3 = audio_current_track();
    return (id3 && id3->title && id3->title[0]) ? id3->title : NULL;
}

long be_audio_track_elapsed(void)
{
    struct mp3entry *id3 = audio_current_track();
    return id3 ? id3->elapsed : 0;
}

long be_audio_track_length(void)
{
    struct mp3entry *id3 = audio_current_track();
    return id3 ? id3->length : 0;
}

void be_audio_pause_toggle(void)
{
    if (audio_status() & AUDIO_STATUS_PAUSE)
        audio_resume();
    else
        audio_pause();
}

void be_play_now(const char *path)
{
    playlist_insert_track(NULL, path, PLAYLIST_INSERT, false, true);
    int n = playlist_amount();
    if (n > 0 && be_playback_allowed())
        playlist_start(n - 1, 0, 0);
}

/* Local playback may only start when no hard source owns the output
 * (mirrors playback.c audio_start_playback, but with a user-visible
 * splash/toast instead of a silent refusal). */
static bool be_playback_allowed(void)
{
    if (usb_audio_get_active())
    {
        splash(HZ * 2, str(LANG_USB_DAC_ACTIVE));
        return false;
    }
#ifdef HAVE_BT_INPUT
    if (bt_input_active())
    {
        splash(HZ * 2, str(LANG_BT_RX_ACTIVE));
        return false;
    }
#endif
#ifdef HAVE_DLNA
    if (dlna_stream_is_active())
    {
        splash(HZ * 2, str(LANG_NETFM_CONFLICT));
        return false;
    }
#endif
    return true;
}

void be_playlist_start(int idx)
{
    if (!be_playback_allowed())
        return;
    playlist_start(idx, 0, 0);
}

void be_playlist_shuffle_apply(bool on)
{
    if (on)
        playlist_shuffle((int)time(NULL), -1);
    else
        playlist_sort(NULL, true);
}

void be_volume_set(int v)
{
    sound_set(SOUND_VOLUME, v);
}

void be_settings_save(void)
{
    settings_save();
}

/* ---- tagcache / database build state ---- */
bool be_db_usable(void)
{
    return tagcache_is_usable();
}

struct tagcache_stat *be_db_stat(void)
{
    return tagcache_get_stat();
}

void be_db_rebuild(void)
{
    tagcache_rebuild();
}

int be_db_max_commit_step(void)
{
    return tagcache_get_max_commit_step();
}

void be_playlist_viewer_settings(void)
{
    do_menu(&viewer_settings_menu, NULL, NULL, false);
}

/* root-menu rows: exact stock counterparts (root_menu.c items[][]) */
extern const struct menu_item_ex main_menu_;   /* menus/main_menu.c  */
extern const struct menu_item_ex info_menu;    /* menus/main_menu.c  */
extern const struct menu_item_ex plugin_menu;  /* menus/plugin_menu.c */

/* Stock menus ask get_current_activity() to decide what to show (e.g. the
 * File View entry is hidden outside ACTIVITY_SETTINGS).  lvgui bypasses
 * root_menu, so every settings-family menu has to declare it itself. */
static void do_settings_menu(const struct menu_item_ex *m)
{
    push_current_activity(ACTIVITY_SETTINGS);
    do_menu(m, NULL, NULL, false);
    pop_current_activity();
    settings_save();     /* lvgui exits hard, so nothing else persists this */
}

void be_settings_menu(void)
{
    do_settings_menu(&main_menu_);              /* GO_TO_MAINMENU */
}

/* rockbox's own keyclick entry: settings/repeat/hardware handling included */
void be_keyclick(void)
{
    keyclick_click(true, BUTTON_TOUCHSCREEN);
}

/* Settings -> Startup/Shutdown -> Start Screen, resolved exactly like
 * root_menu_setup_screens() (the setting stores GO_TO_* + 2) */
int be_start_screen(void)
{
    int screen = global_settings.start_in_screen - 2;

    if (global_settings.start_in_screen == 0)
        screen = global_status.last_screen;
    if (screen > GO_TO_SHORTCUTMENU ||
        (screen == GO_TO_WPS && global_status.resume_index == -1))
        screen = GO_TO_ROOT;
    return screen;
}

void be_playlist_viewer_opts(int *indices, int *track_display)
{
    if (indices)
        *indices = global_settings.playlist_viewer_indices;
    if (track_display)
        *track_display = global_settings.playlist_viewer_track_display;
}

/* playlist viewer line, following format_line() from the classic
 * playlist_viewer: [current], indices prefix, file name / path / tags */
void be_playlist_row_label(int idx, bool current, char *out, size_t outsz)
{
    char path[MAX_PATH];
    char shown[MAX_PATH];

    out[0] = '\0';
    if (!be_playlist_track_path(idx, path, sizeof path))
        return;

    int indices, track_display;
    be_playlist_viewer_opts(&indices, &track_display);

    if (track_display == 1)
    {
        strlcpy(shown, path, sizeof shown);
    }
    else
    {
        const char *base = strrchr(path, '/');
        strlcpy(shown, base ? base + 1 : path, sizeof shown);
        char *dot = strrchr(shown, '.');
        if (dot)
            *dot = '\0';
    }

    struct mp3entry id3;
    if (track_display > 1 && get_metadata(&id3, -1, path) &&
        id3.title && *id3.title)
    {
        if (track_display == 2 && id3.album && *id3.album)
            snprintf(shown, sizeof shown, "%s - %s", id3.title, id3.album);
        else
            strlcpy(shown, id3.title, sizeof shown);
    }

    if (indices)
        snprintf(out, outsz, "%s%d. %s%s", current ? "[" : "", idx + 1,
                 shown, current ? "]" : "");
    else
        snprintf(out, outsz, "%s%s%s", current ? "[" : "", shown,
                 current ? "]" : "");
}

void be_power_off(void)
{
    /* sys_poweroff() only broadcasts SYS_POWEROFF, which is consumed by the
     * classic UI event loop that we never run under LVGL: shut down here. */
    if (global_settings.show_shutdown_message)
        shell_toast(0, str(LANG_SHUTTINGDOWN), NULL);
    power_off();
}

void be_reboot(void)
{
    shell_toast(0, str(LANG_N3_REBOOTING), NULL);
    system_reboot();
}

void be_system_menu(void)
{
    do_menu(&info_menu, NULL, NULL, false);    /* GO_TO_SYSTEM_SCREEN */
}

void be_plugin_menu(void)
{
    do_menu(&plugin_menu, NULL, NULL, false);  /* GO_TO_BROWSEPLUGINS */
}

void lvgui_idle_pump(void);

/* LVGL is single-threaded: only the thread that entered the menu job may
 * pump it.  The stock bluetooth code runs its blocking steps from other
 * threads too (e.g. the auto-connect watchdog), and pumping LVGL from
 * those would corrupt the UI / freeze the player. */
static unsigned int be_gui_thread;

static void be_bt_idle(void)
{
    if (be_gui_thread && thread_self() != be_gui_thread)
        return;
    lvgui_idle_pump();
}

/* ---- control centre bridges ---------------------------------------- */
bool be_wifi_on(void)
{
    return wifi_hal_is_up();
}

void be_wifi_toggle(void)
{
    if (wifi_hal_is_up())
        wifi_hal_power_off();
    else
        wifi_hal_power_on();
}

bool be_tube_on(void)
{
    return global_settings.tube_mode != 0;
}

void be_tube_toggle(void)
{
    be_tube_set(global_settings.tube_mode == 1 ? 0 : 1);
}

/* 1 = triode, 2 = ultra-linear; selecting the active mode turns it off */
void be_tube_set(int mode)
{
    if (global_settings.tube_mode == mode)
        mode = 0;
    global_settings.tube_mode = mode;
    /* exactly the path the settings menu uses for this CHOICE_SETTING
     * (sound_set_tube_mode -> audiohw_set_tube_mode, guarded by
     * audio_is_initialized) */
    sound_set(SOUND_TUBE_MODE, mode);
    settings_save();
}

int be_tube_mode(void)
{
    return global_settings.tube_mode;
}

int be_volume_pct_get(void)
{
    int lo = sound_min(SOUND_VOLUME), hi = sound_max(SOUND_VOLUME);
    if (hi <= lo)
        return 0;
    return (global_status.volume - lo) * 100 / (hi - lo);
}

void be_volume_set_pct(int pct)
{
    int lo = sound_min(SOUND_VOLUME), hi = sound_max(SOUND_VOLUME);
    int v;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    v = lo + (hi - lo) * pct / 100;
    sound_set(SOUND_VOLUME, v);
    global_status.volume = v;
}

int be_brightness_get(void)
{
    return global_settings.brightness * 100 / MAX_BRIGHTNESS_SETTING;
}

void be_brightness_set(int pct)
{
    int val;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    val = pct * MAX_BRIGHTNESS_SETTING / 100;
    /* the panel write costs ~12-30 ms; only touch it when the value moves
     * by more than one 5% step so dragging stays smooth */
    if (val / 13 == global_settings.brightness / 13 && pct != 0 && pct != 100)
        return;
    global_settings.brightness = val;
    backlight_set_brightness(val);
}

bool be_media_active(void)
{
    return be_dlna_active() || netfm_stream_is_active() ||
           netfm_playback_active() || (audio_status() & AUDIO_STATUS_PLAY);
}

bool be_media_is_radio(void)
{
    return netfm_stream_is_active() || netfm_playback_active();
}

bool be_media_playing(void)
{
    int st = audio_status();
    return (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
}

void be_media_toggle(void)
{
    int st;
    if (netfm_stream_is_active() || netfm_playback_active())
    {
        netfm_stream_stop();
        return;
    }
    st = audio_status();
    if (st & AUDIO_STATUS_PAUSE)
        audio_resume();
    else if (st & AUDIO_STATUS_PLAY)
        audio_pause();
}

/* Track skips are executed on a worker thread: audio_skip() takes the
 * id3 mutex and can block for a second or two while the codec parses the
 * next track, which must not stall the UI thread. */
static struct event_queue step_q;
static bool step_q_ready;
static char step_stack[2048];

static void step_thread(void)
{
    struct queue_event ev;
    while (1)
    {
        queue_wait(&step_q, &ev);
        if (ev.id == 1)
            audio_next();
        else if (ev.id == 2)
            audio_prev();
    }
}

static void step_init(void)
{
    if (step_q_ready)
        return;
    queue_init(&step_q, 4);
    create_thread(step_thread, step_stack, sizeof step_stack, 0,
                  "lvgui step" IF_PRIO(, PRIORITY_PLAYBACK));
    step_q_ready = true;
}

void be_media_step(int dir)
{
    if (netfm_stream_is_active() || netfm_playback_active())
    {
        be_netfm_step(dir);
        return;
    }
    step_init();
    queue_post(&step_q, dir > 0 ? 1 : 2, 0);
}

void be_sound_menu(void)
{
    do_settings_menu(&sound_settings);
}

void be_display_menu(void)
{
    do_settings_menu(&display_menu);
}

void be_netfm_name(char *out, size_t sz)
{
    struct netfm_stream_status st;
    netfm_stream_get_status(&st);
    snprintf(out, sz, "%s", st.name);
}

void be_bt_disconnect(void)
{
    bt_audio_disconnect();
}

void be_resume_playback(void)
{
    lvgui_resume_playback();
}

void be_bt_menu_open(void)
{
    be_bluetooth_menu();
}

void be_bluetooth_menu(void)
{
    be_gui_thread = thread_self();
    bt_audio_idle_hook = be_bt_idle;
    push_current_activity(ACTIVITY_SETTINGS);
    lvgui_bt_menu();    /* native do_menu tree (calls the stock sub-screens) */
    pop_current_activity();
    settings_save();
}

void be_wifi_menu(void)
{
    push_current_activity(ACTIVITY_SETTINGS);
    wifi_menu();        /* ditto */
    pop_current_activity();
    settings_save();
}

/* native main-menu (root menu) editor */
int be_root_items(const char **keys, const char **labels,
                  unsigned char *enabled, int max)
{
    return lvgui_root_items(keys, labels, enabled, max);
}

void be_root_apply(const char **keys, int n)
{
    lvgui_root_apply(keys, n);
}

void be_bookmark_load(void)
{
    bookmark_load_menu();                      /* GO_TO_RECENTBMARKS */
}

void be_shortcut_menu(void)
{
    push_current_activity(ACTIVITY_SETTINGS);
    do_shortcut_menu(NULL);                    /* GO_TO_SHORTCUTMENU */
    pop_current_activity();
    settings_save();
}

/* equalizer graphical editor: stock data + stock apply (eq_menu.c) */
const struct int_setting *be_eq_gain_setting(void)
{
    return eq_get_gain_setting();
}

void be_eq_apply(void)
{
    eq_apply_settings();
}

void be_eq_apply_band(int band)
{
    eq_apply_band(band);
}

const char *be_eq_band_name(int band, char *buf, size_t len)
{
    return eq_gain_band_name(band, buf, len);
}

const struct int_setting *be_eq_q_setting(void)
{
    return eq_get_q_setting();
}

const struct int_setting *be_eq_cutoff_setting(void)
{
    return eq_get_cutoff_setting();
}

/* ---- network-source status screens (netfm / bt-rx / dlna) -------------- */
#include "bt_input.h"
#include "bt_audio.h"
#include "bt_audio.h"
#include "dlna/dlna_stream.h"
#include "lang.h"

void be_netfm_get_status(struct netfm_stream_status *st)
{
    netfm_stream_get_status(st);
}

void be_netfm_stop(void)
{
    netfm_stream_stop();
}

const char *be_bt_rx_state_name(void)
{
    switch (bt_input_get_state())
    {
        case BT_INPUT_CONNECTED:    return str(LANG_BT_CONNECTED);
        case BT_INPUT_DISCONNECTED: return str(LANG_BT_DISCONNECTED);
        default:                    return str(LANG_BT_RX_WAITING);
    }
}

bool be_bt_rx_connected(void)
{
    return bt_input_get_state() == BT_INPUT_CONNECTED;
}

void be_bt_rx_peer(char *buf, size_t sz)
{
    buf[0] = '\0';
    bt_input_get_peer(buf, sz);
}

const char *be_bt_codec(void)
{
    return bt_audio_codec();
}

int be_bt_rx_rate(void)
{
    return bt_input_get_rate();
}

void be_dlna_get_status(struct dlna_stream_status *st)
{
    dlna_stream_get_status(st);
}

bool be_netfm_active(void)
{
    return netfm_stream_is_active() || netfm_playback_active();
}

bool be_dlna_active(void)
{
    struct dlna_stream_status st;
    dlna_stream_get_status(&st);
    /* Only a running session owns the dock: after STOP the renderer stays
     * "disconnected/alive" for a while, which used to keep the old track on
     * the home screen even though nothing was playing. */
    return st.state == DLNA_STREAM_CONNECTING ||
           st.state == DLNA_STREAM_BUFFERING ||
           st.state == DLNA_STREAM_PLAYING;
}

void be_dlna_time(long *pos, long *dur)
{
    struct dlna_stream_status st;
    dlna_stream_get_status(&st);
    *pos = (long)st.position_ms;
    *dur = (long)st.duration_ms;
    /* the stream only learns the length from the container; the controller
     * announced it in the DIDL <res duration="..."> */
    if (*dur <= 0)
        *dur = (long)dlna_output_get_duration_ms();
}

/* One decoded DLNA cover, shared by the player screen and the dock chip.
 * The compressed download is released right after this decode, so the only
 * long-lived cost is 240x240 RGB565 (~113 KB). */
#define DLNA_ART_PX 240
static uint16_t dlna_pix[DLNA_ART_PX * DLNA_ART_PX];
static unsigned dlna_pix_gen;
static int dlna_pix_w, dlna_pix_h;

bool be_dlna_art_decode(const unsigned char *data, size_t len, unsigned gen)
{
    be_img_t *im;

    if (!data || !len)
        return false;
    im = be_art_blob(data, len, DLNA_ART_PX, DLNA_ART_PX);
    dlna_output_art_release(gen);
    if (!im)
        return false;
    if (im->w <= 0 || im->h <= 0 || im->w > DLNA_ART_PX ||
        im->h > DLNA_ART_PX)
    {
        be_img_free(im);
        return false;
    }
    memcpy(dlna_pix, im->pixels, (size_t)im->w * im->h * 2);
    dlna_pix_w = im->w;
    dlna_pix_h = im->h;
    be_img_free(im);
    dlna_pix_gen = gen;
    return true;
}

const void *be_dlna_art_pixels(int *w, int *h, unsigned *gen)
{
    *w = dlna_pix_w;
    *h = dlna_pix_h;
    *gen = dlna_pix_gen;
    return dlna_pix;
}

void be_dlna_stop(void)
{
    dlna_output_request_stop();
}

void be_dlna_seek(int ms)
{
    if (ms >= 0)
        dlna_stream_seek(ms);
}

void be_dlna_name(char *out, size_t sz)
{
    struct dlna_stream_status st;
    dlna_stream_get_status(&st);
    snprintf(out, sz, "%s", st.name);
}

bool be_bt_rx_active(void)
{
    return bt_input_active();
}

const char *be_netfm_last_name(void)
{
    return netfm_last_station_name();
}

int be_netfm_step(int dir)
{
    return netfm_step_station(dir);
}

bool be_wifi_up(void)
{
    return wifi_hal_is_up();
}

bool be_bt_active(void)
{
    return bt_audio_output_active() || bt_input_active();
}
