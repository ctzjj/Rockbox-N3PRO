/***************************************************************************
 * Generic Web remote control framework.
 *
 * Serves the browser page from .rockbox/web/control/ (no built-in
 * fallback: the service refuses to start without the files) and
 * exposes a small JSON API plus a WebSocket for live status.  The
 * page offers title / cover / progress, prev / next / play-pause,
 * seek and volume; access is guarded by a short code shown on the
 * device.
 *
 * Core playback APIs are only touched from the cooperative Rockbox
 * worker thread; the network threads exchange data through a
 * snapshot and a command queue.
 ****************************************************************************/
#include "config.h"

#ifdef HAVE_WEB_CONTROL

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <fcntl.h>

#include "kernel.h"
#include "thread.h"
#include "action.h"
#include "splash.h"
#include "lang.h"
#include "settings.h"
#include "sound.h"
#include "audio.h"
#include "playlist.h"
#include "gui/list.h"
#include "albumart.h"
#include "file.h"
#include "dir.h"
#include "dirent.h"
#include "rbpaths.h"
#include "strcasestr.h"
#include "filetypes.h"
#include "powermgmt.h"
#include "web_control.h"

#if defined(HAVE_WIFI_MENU)
#include "wifi_hal.h"

/* TEMP diagnostics -- remove before final sync */
#define WEB_LOG_PATH "/tmp/web.log"
static void web_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void web_log(const char *fmt, ...)
{
    FILE *f = fopen(WEB_LOG_PATH, "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
}
#endif

#ifdef CAYIN_N3PRO
#include "n3pro-bt-pcm.h"
#endif

#define WEB_PORT_MAIN    80
#define WEB_PORT_ALT     8080
#define WEB_CMD_Q        8
#define WEB_WS_MAX       4
#define WEB_FILES_MAX    400
#define WEB_NAME_MAX     120
/* Rockbox-vfs paths: "/" is the storage root, the page lives in
 * <storage>/.rockbox/web/control/ (same layout as langs/themes). */
#define WEB_PAGE_DIR     ROCKBOX_DIR "/web/control"
#define WEB_PAGE_INDEX   WEB_PAGE_DIR "/index.html"

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

enum web_cmd
{
    WEB_CMD_NONE = 0,
    WEB_CMD_PLAYPAUSE,
    WEB_CMD_NEXT,
    WEB_CMD_PREV,
    WEB_CMD_STOP,
    WEB_CMD_SEEK,
    WEB_CMD_VOL,
    WEB_CMD_JUMP,      /* param = playlist slot (0 = current track) */
    WEB_CMD_FILEPLAY,  /* path operand in pend_path */
    WEB_CMD_REPEAT,    /* cycle repeat mode Off -> All -> One */
};

struct web_state
{
    volatile bool running;
    int port;
    char code[7];
    char token[17];

    pthread_mutex_t mutex;
    pthread_t accept_pt;
    int listen_fd;

    /* snapshot, guarded by mutex */
    struct
    {
        int state;                 /* 0 stopped, 1 playing, 2 paused */
        int vol;                   /* 0..100 */
        int bat;                   /* battery percent */
        int rep;                   /* repeat mode (REPEAT_*) */
        long serial;               /* bumped on track change */
        bool aa;                   /* cover available */
        bool aa_embed;
        char aa_path[260];         /* track path (embed) or art file */
        long aa_pos, aa_size;
        char title[130], artist[96], album[96];
        unsigned long pos, len;    /* ms */
    } snap;
    char json[1400];

    /* websocket clients */
    int ws_fd[WEB_WS_MAX];

    /* command queue */
    struct { int cmd; long param; } cmds[WEB_CMD_Q];
    int cmd_rd, cmd_wr;
    char pend_path[MAX_PATH];        /* WEB_CMD_FILEPLAY operand */
};

static struct web_state W =
{
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .listen_fd = -1,
};

/* One directory entry as the browser sees it (file management). */
struct web_entry
{
    char name[WEB_NAME_MAX];
    long mtime;
    int attr;                   /* ATTR_DIRECTORY or filetype bits */
};

static int web_collect_dir(const char *path, struct web_entry *ents,
                           int max, bool audio_only);
static void web_json_reply(int fd, int code, const char *text,
                           const char *msg);

static char *web_str(int id)
{
    return (char *)str(id);
}

/* ------------------------------------------------------------------ */
/* sha1 + base64 (for the WebSocket handshake)                         */
/* ------------------------------------------------------------------ */

static void web_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE,
                      0x10325476, 0xC3D2E1F0 };
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 9 + 63) / 64) * 64;   /* padded size */
    uint8_t *buf = malloc(total);

    if (!buf)
    {
        memset(out, 0, 20);
        return;
    }
    memcpy(buf, data, len);
    buf[len] = 0x80;
    memset(buf + len + 1, 0, total - len - 1);
    for (int i = 0; i < 8; i++)                  /* big-endian bit count */
        buf[total - 1 - i] = (uint8_t)(bits >> (8 * i));

    for (size_t off = 0; off < total; off += 64)
    {
        uint32_t w[80];

        for (int i = 0; i < 16; i++)             /* big-endian words */
            w[i] = ((uint32_t)buf[off + i * 4] << 24) |
                   ((uint32_t)buf[off + i * 4 + 1] << 16) |
                   ((uint32_t)buf[off + i * 4 + 2] << 8) |
                   (uint32_t)buf[off + i * 4 + 3];
        for (int i = 16; i < 80; i++)
        {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(buf);

    for (int i = 0; i < 5; i++)
    {
        out[i * 4 + 0] = h[i] >> 24;
        out[i * 4 + 1] = h[i] >> 16;
        out[i * 4 + 2] = h[i] >> 8;
        out[i * 4 + 3] = h[i];
    }
}

static void web_b64(const uint8_t *in, size_t len, char *out)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;

    for (i = 0; i + 2 < len; i += 3)
    {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = tab[v & 63];
    }
    if (len - i == 1)
    {
        out[o++] = tab[in[i] >> 2];
        out[o++] = tab[(in[i] & 3) << 4];
        out[o++] = '=';
        out[o++] = '=';
    }
    else if (len - i == 2)
    {
        out[o++] = tab[in[i] >> 2];
        out[o++] = tab[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = tab[(in[i + 1] & 15) << 2];
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* snapshot / commands (worker side)                                   */
/* ------------------------------------------------------------------ */

static void web_json_escape(char *out, size_t outsz, const char *in)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < outsz; p++)
    {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"' || ch == '\\')
        {
            out[o++] = '\\';
            out[o++] = ch;
        }
        else if (ch < 0x20)
        {
            o += snprintf(out + o, outsz - o, "\\u%04x", ch);
        }
        else
            out[o++] = ch;
    }
    out[o] = '\0';
}

/* Resolve the artwork for the current track into the snapshot:
 * embedded art first, then a folder image lookup. Called from the
 * worker thread only. */
static void web_snap_albumart(struct mp3entry *id3)
{
    bool embed = id3->has_embedded_albumart && id3->albumart.size > 0;
    char art[260];
    bool found = false;
    if (!embed)
    {
        struct dim dim = { 0, 0 };
        found = find_albumart(id3, art, sizeof(art), &dim);
    }
    if (embed)
    {
        W.snap.aa = true;
        W.snap.aa_embed = true;
        snprintf(W.snap.aa_path, sizeof(W.snap.aa_path), "%s", id3->path);
        W.snap.aa_pos = (long)id3->albumart.pos;
        W.snap.aa_size = id3->albumart.size;
    }
    else if (found)
    {
        W.snap.aa = true;
        W.snap.aa_embed = false;
        snprintf(W.snap.aa_path, sizeof(W.snap.aa_path), "%s", art);
        W.snap.aa_pos = 0;
        W.snap.aa_size = 0;
    }
    else
        W.snap.aa = false;
}

/* Called from the worker thread only: refresh the snapshot and
 * rebuild the JSON. */
static void web_refresh_snapshot(void)
{
    struct mp3entry *id3 = audio_current_track();
    int status = audio_status();
    int vmin = sound_min(SOUND_VOLUME);
    int vmax = sound_max(SOUND_VOLUME);
    char t[200], a[130], al[130];

    int state = 0;
    /* paused playback reports PLAY *and* PAUSE - test PAUSE first so the
     * state actually reflects it */
    if (status & AUDIO_STATUS_PAUSE)
        state = 2;
    else if (status & AUDIO_STATUS_PLAY)
        state = 1;

    pthread_mutex_lock(&W.mutex);
    W.snap.state = state;
    W.snap.vol = vmax > vmin
        ? (global_status.volume - vmin) * 100 / (vmax - vmin) : 0;
    W.snap.bat = battery_level();
    W.snap.rep = global_settings.repeat_mode;

    if (id3)
    {
        const char *title = id3->title;
        char base[130];
        if (!title || !title[0])
        {
            const char *slash = strrchr(id3->path, '/');
            snprintf(base, sizeof(base), "%s", slash ? slash + 1 : id3->path);
            title = base;
        }
        snprintf(W.snap.title, sizeof(W.snap.title), "%s", title ? title : "-");
        snprintf(W.snap.artist, sizeof(W.snap.artist), "%s",
                 id3->artist ? id3->artist : "");
        snprintf(W.snap.album, sizeof(W.snap.album), "%s",
                 id3->album ? id3->album : "");
        W.snap.pos = id3->elapsed;
        W.snap.len = id3->length;

        static char last_track[260] = "";
        static int aa_retries = 0;
        if (strcmp(last_track, id3->path) != 0)
        {
            snprintf(last_track, sizeof(last_track), "%s", id3->path);
            W.snap.serial++;

            /* the artwork lookup touches the filesystem - run it
             * only when the track changes, not on every refresh */
            web_snap_albumart(id3);

            /* right after a skip the metadata may not be parsed yet
             * (embedded art missing from the half-filled id3); retry
             * twice so late artwork is still found */
            aa_retries = W.snap.aa ? 0 : 2;
        }
        else if (!W.snap.aa && aa_retries > 0)
        {
            aa_retries--;
            web_snap_albumart(id3);
            if (W.snap.aa)
                W.snap.serial++;   /* page must refetch the cover */
        }
    }
    else
    {
        W.snap.title[0] = '\0';
        W.snap.artist[0] = '\0';
        W.snap.album[0] = '\0';
        W.snap.pos = 0;
        W.snap.len = 0;
        W.snap.aa = false;
    }

    web_json_escape(t, sizeof(t), W.snap.title);
    web_json_escape(a, sizeof(a), W.snap.artist);
    web_json_escape(al, sizeof(al), W.snap.album);
    snprintf(W.json, sizeof(W.json),
             "{\"state\":%d,\"title\":\"%s\",\"artist\":\"%s\","
             "\"album\":\"%s\",\"pos\":%lu,\"len\":%lu,\"vol\":%d,"
             "\"bat\":%d,\"rep\":%d,\"serial\":%ld,\"aa\":%s}",
             W.snap.state, t, a, al, W.snap.pos, W.snap.len,
             W.snap.vol, W.snap.bat, W.snap.rep, W.snap.serial,
             W.snap.aa ? "true" : "false");
    pthread_mutex_unlock(&W.mutex);
}

static void web_exec_cmd(int cmd, long param)
{
    web_log("exec %d %ld", cmd, param);
    switch (cmd)
    {
        case WEB_CMD_PLAYPAUSE:
            /* paused reports PLAY *and* PAUSE - test PAUSE first so the
             * toggle actually alternates instead of pausing forever */
            if (audio_status() & AUDIO_STATUS_PAUSE)
                audio_resume();
            else if (audio_status() & AUDIO_STATUS_PLAY)
                audio_pause();
            break;
        case WEB_CMD_NEXT:
            audio_next();
            break;
        case WEB_CMD_PREV:
            audio_prev();
            break;
        case WEB_CMD_STOP:
            audio_stop();
            break;
        case WEB_CMD_SEEK:
            audio_ff_rewind(param);
            break;
        case WEB_CMD_VOL:
        {
            int vmin = sound_min(SOUND_VOLUME);
            int vmax = sound_max(SOUND_VOLUME);
            if (param < 0) param = 0;
            if (param > 100) param = 100;
            sound_set_volume(vmin + (int)param * (vmax - vmin) / 100);
            break;
        }
        case WEB_CMD_JUMP:
            /* param = display slot.  Resolve it to a real index and
             * start there directly: audio_skip() navigates by repeat
             * rules, and under Repeat One it refuses to leave the
             * current track at all. */
            if (param >= 0)
            {
                struct playlist_track_info info;
                int n = playlist_amount();
                for (int i = 0; i < n; i++)
                {
                    if (playlist_get_track_info(NULL, i, &info) == 0 &&
                        info.display_index == param + 1)
                    {
                        playlist_start(info.index, 0, 0);
                        break;
                    }
                }
            }
            break;
        case WEB_CMD_FILEPLAY:
        {
            /* Play the chosen file the way the browser does: the whole
             * directory becomes the new playlist (sorted with the
             * browser settings), starting at the selected file. */
            char local[MAX_PATH];
            pthread_mutex_lock(&W.mutex);
            snprintf(local, sizeof(local), "%s", W.pend_path);
            pthread_mutex_unlock(&W.mutex);

            char *sep = strrchr(local, '/');
            if (!sep)
                break;
            char selname[WEB_NAME_MAX];
            snprintf(selname, sizeof(selname), "%s", sep + 1);
            *sep = '\0';

            static struct web_entry ents[WEB_FILES_MAX];
            int n = web_collect_dir(local, ents, WEB_FILES_MAX, true);
            if (n <= 0)
                break;

            audio_stop();
            if (playlist_create(local, NULL) == -1)
                break;

            int sel = 0;
            for (int i = 0; i < n; i++)
            {
                char full[MAX_PATH];
                snprintf(full, sizeof(full), "%s/%s", local, ents[i].name);
                playlist_insert_track(NULL, full, PLAYLIST_INSERT_LAST,
                                      false, false);
                if (strcasecmp(ents[i].name, selname) == 0)
                    sel = i;
            }
            if (global_settings.playlist_shuffle)
                sel = playlist_shuffle(current_tick, sel);
            playlist_start(sel, 0, 0);
            break;
        }
        case WEB_CMD_REPEAT:
        {
            /* cycle like the playback settings offer: Off -> All -> One;
             * shuffle/ab states (if ever set elsewhere) fold back to Off */
            int r = global_settings.repeat_mode;
            if (r < REPEAT_OFF || r > REPEAT_ONE)
                r = REPEAT_OFF;
            else
                r = (r == REPEAT_ONE) ? REPEAT_OFF : r + 1;
            global_settings.repeat_mode = r;
            settings_save();
            break;
        }
    }
}

static void web_push_cmd(int cmd, long param)
{
    pthread_mutex_lock(&W.mutex);
    int next = (W.cmd_wr + 1) % WEB_CMD_Q;
    if (next != W.cmd_rd)    /* drop when full */
    {
        W.cmds[W.cmd_wr].cmd = cmd;
        W.cmds[W.cmd_wr].param = param;
        W.cmd_wr = next;
    }
    pthread_mutex_unlock(&W.mutex);
}

static void web_drain_cmds(void)
{
    while (1)
    {
        int cmd = WEB_CMD_NONE;
        long param = 0;

        pthread_mutex_lock(&W.mutex);
        if (W.cmd_rd != W.cmd_wr)
        {
            cmd = W.cmds[W.cmd_rd].cmd;
            param = W.cmds[W.cmd_rd].param;
            W.cmd_rd = (W.cmd_rd + 1) % WEB_CMD_Q;
        }
        pthread_mutex_unlock(&W.mutex);

        if (cmd == WEB_CMD_NONE)
            break;
        web_exec_cmd(cmd, param);
    }
}

/* ------------------------------------------------------------------ */
/* worker thread (cooperative)                                         */
/* ------------------------------------------------------------------ */

static volatile bool web_worker_run;
static volatile bool web_worker_started;

static void web_ws_broadcast(const char *payload, size_t len)
{
    uint8_t hdr[10];
    size_t hlen;

    if (len < 126)
    {
        hdr[0] = 0x81;
        hdr[1] = (uint8_t)len;
        hlen = 2;
    }
    else
    {
        hdr[0] = 0x81;
        hdr[1] = 126;
        hdr[2] = len >> 8;
        hdr[3] = len & 0xff;
        hlen = 4;
    }

    /* Snapshot the clients, then send WITHOUT holding the mutex: a
     * stalled client (screen off, backgrounded tab) must never block
     * the worker - commands and status refreshs would queue behind
     * it.  The sockets are non-blocking; a client that cannot take
     * the frame is dropped and reconnects by itself. */
    int fds[WEB_WS_MAX];
    pthread_mutex_lock(&W.mutex);
    for (int i = 0; i < WEB_WS_MAX; i++)
        fds[i] = W.ws_fd[i];
    pthread_mutex_unlock(&W.mutex);

    for (int i = 0; i < WEB_WS_MAX; i++)
    {
        int fd = fds[i];
        if (fd < 0)
            continue;
        if (send(fd, hdr, hlen, MSG_NOSIGNAL) < 0 ||
            send(fd, payload, len, MSG_NOSIGNAL) < 0)
        {
            close(fd);
            pthread_mutex_lock(&W.mutex);
            if (W.ws_fd[i] == fd)
                W.ws_fd[i] = -1;
            pthread_mutex_unlock(&W.mutex);
        }
    }
}

static bool web_local_playback(void)
{
    /* Positive check: the web remote drives the local file player only.
     * Anything that is not plainly local file playback (USB DAC input,
     * bluetooth receive, bluetooth output route -- and any future
     * external mode registered in the target helper) refuses to run. */
#ifdef CAYIN_N3PRO
    return n3pro_local_playback();
#else
    return true;
#endif
}

static void web_worker_thread(void)
{
    char last[1400] = "";
    int loops = 0;

    web_log("worker up");

    while (web_worker_run)
    {
        /* An external mode (USB DAC plugged, bluetooth route started)
         * may take over while the server runs: stop it then. */
        if (!web_local_playback())
        {
            web_log("worker exit: not local");
            web_control_stop();
            break;
        }

        web_drain_cmds();
        web_refresh_snapshot();

        pthread_mutex_lock(&W.mutex);
        size_t len = strlen(W.json);
        char snap[1400];
        memcpy(snap, W.json, len + 1);
        pthread_mutex_unlock(&W.mutex);

        if (strcmp(snap, last) != 0)
        {
            strcpy(last, snap);
            web_ws_broadcast(snap, len);
        }
        if ((++loops & 15) == 0)
            web_log("hb loops=%d len=%d", loops, (int)len);
        sleep(HZ / 4);
    }
    web_log("worker end run=%d", web_worker_run);
    web_worker_started = false;
}

static long web_worker_stack[(DEFAULT_STACK_SIZE + 0x2000) / sizeof(long)];

/* ------------------------------------------------------------------ */
/* sockets                                                             */
/* ------------------------------------------------------------------ */

static void web_ws_client(int fd, const char *headers);
static void web_ws_register(int fd);
static void web_ws_unregister(int fd);

static bool web_send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0)
    {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool web_send_str(int fd, const char *s)
{
    return web_send_all(fd, s, strlen(s));
}

/* Read one line (incl. CRLF) with a hard cap. */
static int web_read_line(int fd, char *buf, size_t bufsz)
{
    size_t got = 0;
    while (got + 1 < bufsz)
    {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0)
            return -1;
        if (c == '\n')
            break;
        buf[got++] = c;
    }
    buf[got] = '\0';
    if (got && buf[got - 1] == '\r')
        buf[got - 1] = '\0';
    return (int)got;
}

static const char *web_ctype(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm"))
        return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".css"))
        return "text/css; charset=utf-8";
    if (!strcasecmp(dot, ".js"))
        return "text/javascript; charset=utf-8";
    if (!strcasecmp(dot, ".png"))
        return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"))
        return "image/jpeg";
    if (!strcasecmp(dot, ".bmp"))
        return "image/bmp";
    if (!strcasecmp(dot, ".svg"))
        return "image/svg+xml";
    if (!strcasecmp(dot, ".ico"))
        return "image/x-icon";
    if (!strcasecmp(dot, ".json"))
        return "application/json";
    if (!strcasecmp(dot, ".txt"))
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static bool web_http_head(int fd, int code, const char *status,
                          const char *ctype, long len)
{
    char head[320];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Cache-Control: no-store\r\n"
             "Connection: close\r\n\r\n",
             code, status, ctype, len);
    return web_send_str(fd, head);
}

/* Serve a file with a rockbox-root path (already validated). */
static void web_serve_file(int fd, const char *rpath)
{
    int f = open(rpath, O_RDONLY);
    if (f < 0)
    {
        const char *msg = "not found";
        web_http_head(fd, 404, "Not Found", "text/plain", (long)strlen(msg));
        web_send_str(fd, msg);
        return;
    }

    long size = lseek(f, 0, SEEK_END);
    lseek(f, 0, SEEK_SET);
    if (size < 0 || size > 8 * 1024 * 1024)
        size = 0;

    if (!web_http_head(fd, 200, "OK", web_ctype(rpath), size))
    {
        close(f);
        return;
    }

    char buf[16 * 1024];
    ssize_t n;
    long left = size;
    while (left > 0 && (n = read(f, buf, sizeof(buf))) > 0)
    {
        if (!web_send_all(fd, buf, n))
            break;
        left -= n;
    }
    close(f);
}

static bool web_page_present(void)
{
    int f = open(WEB_PAGE_INDEX, O_RDONLY);
    if (f < 0)
        return false;
    close(f);
    return true;
}

static void web_serve_cover(int fd)
{
    pthread_mutex_lock(&W.mutex);
    bool aa = W.snap.aa, embed = W.snap.aa_embed;
    char path[260];
    long pos = W.snap.aa_pos, size = W.snap.aa_size;
    snprintf(path, sizeof(path), "%s", W.snap.aa_path);
    pthread_mutex_unlock(&W.mutex);

    if (!aa)
        goto missing;

    int f = open(path, O_RDONLY);
    if (f < 0)
        goto missing;

    uint8_t magic[4] = { 0 };
    if (embed)
    {
        if (lseek(f, pos, SEEK_SET) < 0)
        {
            close(f);
            goto missing;
        }
        if (read(f, magic, 3) != 3)
        {
            close(f);
            goto missing;
        }
        lseek(f, pos, SEEK_SET);
    }
    else
    {
        if (read(f, magic, 3) != 3)
        {
            close(f);
            goto missing;
        }
        lseek(f, 0, SEEK_SET);
        long len = lseek(f, 0, SEEK_END);
        lseek(f, 0, SEEK_SET);
        if (len > 0)
            size = len;
    }

    if (!embed && size <= 0)
        size = 8 * 1024 * 1024;

    const char *ct = "image/jpeg";
    if (magic[0] == 0x89 && magic[1] == 'P')
        ct = "image/png";
    else if (magic[0] == 'B' && magic[1] == 'M')
        ct = "image/bmp";

    if (!web_http_head(fd, 200, "OK", ct, size))
    {
        close(f);
        return;
    }

    char buf[16 * 1024];
    ssize_t n;
    long left = size;
    while (left > 0 && (n = read(f, buf, sizeof(buf))) > 0)
    {
        if (!web_send_all(fd, buf, n))
            break;
        left -= n;
    }
    close(f);
    return;

missing:
{
    const char *msg = "no art";
    web_http_head(fd, 404, "Not Found", "text/plain", (long)strlen(msg));
    web_send_str(fd, msg);
}
}

static bool web_authed(const char *headers)
{
    char expect[64];
    snprintf(expect, sizeof(expect), "Bearer %s", W.token);

    const char *h = headers;
    while (h && *h)
    {
        if (strncasecmp(h, "Authorization:", 14) == 0)
        {
            const char *v = h + 14;
            while (*v == ' ')
                v++;
            return strncmp(v, expect, strlen(expect)) == 0;
        }
        h = strchr(h, '\n');
        if (h)
            h++;
    }
    return false;
}

static bool web_query_token(const char *path)
{
    char match[64];
    snprintf(match, sizeof(match), "token=%s", W.token);
    return strstr(path, match) != NULL;
}

static long web_parse_param(const char *body, const char *key)
{
    char match[24];
    snprintf(match, sizeof(match), "%s=", key);
    const char *v = strstr(body, match);
    if (!v)
        return -1;
    v += strlen(match);
    return atol(v);
}

/* Current playlist, enumerated in playback order from the first
 * position -- a stable list that does not rotate with the playing
 * track; "cur" marks the playing track's slot and the web page
 * jumps by converting the slot to an offset (audio_skip).  Runs on
 * the connection thread. */
#define WEB_PL_MAX 500

static void web_serve_playlist(int fd)
{
    int n = playlist_amount();
    web_log("pl amount=%d", n);
    if (n < 0)
        n = 0;
    if (n > WEB_PL_MAX)
        n = WEB_PL_MAX;

    /* Enumerate the playlist CONTENT the way the on-device playlist
     * viewer does: by index, ordered by display position.  playlist_
     * peek() cannot be used for listing - it follows repeat-mode
     * navigation, and under Repeat One every step resolves to the
     * current track, so the whole list would show the same song. */
    char *names = NULL;
    if (n > 0)
    {
        names = calloc((size_t)n, 128);
        if (!names)
        {
            web_json_reply(fd, 500, "Server Error", "{\"error\":\"oom\"}");
            return;
        }
        for (int i = 0; i < n; i++)
        {
            struct playlist_track_info info;
            if (playlist_get_track_info(NULL, i, &info) < 0)
                continue;
            int slot = info.display_index - 1;
            if (slot < 0 || slot >= n)
                continue;
            const char *base = strrchr(info.filename, '/');
            base = base ? base + 1 : info.filename;
            snprintf(names + (size_t)slot * 128, 128, "%s", base);
        }
    }

    int cur = playlist_get_display_index() - 1;
    if (cur < 0)
        cur = 0;
    if (cur > n - 1)
        cur = n - 1;
    web_log("pl cur=%d", cur);

    size_t cap = 16 * 1024;
    char *buf = malloc(cap);
    if (!buf)
    {
        free(names);
        web_json_reply(fd, 500, "Server Error", "{\"error\":\"oom\"}");
        return;
    }

    size_t o = (size_t)snprintf(buf, cap, "{\"cur\":%d,\"tracks\":[", cur);

    for (int k = 0; k < n; k++)
    {
        char disp[128];
        char esc[192];
        snprintf(disp, sizeof(disp), "%s", names + (size_t)k * 128);
        char *dot = strrchr(disp, '.');
        if (dot && dot != disp)
            *dot = '\0';
        web_json_escape(esc, sizeof(esc), disp);

        size_t need = strlen(esc) + 8;
        if (o + need + 4 > cap)
        {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb)
                break;
            buf = nb;
        }
        o += (size_t)snprintf(buf + o, cap - o, "%s\"%s\"",
                              k ? "," : "", esc);
    }

    o += (size_t)snprintf(buf + o, cap - o, "]}");
    web_http_head(fd, 200, "OK", "application/json", (long)o);
    web_send_all(fd, buf, o);
    free(buf);
    free(names);
}

/* ------------------------------------------------------------------ */
/* file management                                                     */
/* ------------------------------------------------------------------ */

static bool web_query_value(const char *path, const char *key,
                            char *out, size_t outsz)
{
    const char *v = strstr(path, key);
    if (!v)
        return false;
    v += strlen(key);
    size_t n = 0;
    while (v[n] && v[n] != '&' && n + 1 < outsz)
    {
        out[n] = v[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

/* One directory entry as the browser sees it -- declared at the top
 * of the file; comparators and the collector follow. */
static int web_cmp_criteria(const struct web_entry *e1,
                            const struct web_entry *e2, int criteria)
{
    switch (criteria)
    {
        case SORT_TYPE:
        case SORT_TYPE_REVERSED:
        {
            int t1 = e1->attr & FILE_ATTR_MASK;
            int t2 = e2->attr & FILE_ATTR_MASK;
            if (!t1)
                t1 = INT_MAX;   /* unknown type sorts last */
            if (!t2)
                t2 = INT_MAX;
            if (t1 != t2)
                return (t1 - t2) * (criteria == SORT_TYPE_REVERSED ? -1 : 1);
            return strcasecmp(e1->name, e2->name);
        }
        case SORT_DATE:
        case SORT_DATE_REVERSED:
            if (e1->mtime != e2->mtime)
                return (int)((e1->mtime - e2->mtime) *
                             (criteria == SORT_DATE_REVERSED ? -1 : 1));
            /* else fall through to alphabetical */
        case SORT_ALPHA:
        case SORT_ALPHA_REVERSED:
        default:
        {
            int r = strcasecmp(e1->name, e2->name);
            return r * (criteria == SORT_ALPHA_REVERSED ? -1 : 1);
        }
    }
}

/* qsort comparator: directories first, each group with its own
 * setting from global_settings (read live -- it is what the browser
 * itself consults). */
static int web_cmp_entry(const void *a, const void *b)
{
    const struct web_entry *e1 = a;
    const struct web_entry *e2 = b;
    bool d1 = e1->attr & ATTR_DIRECTORY;
    bool d2 = e2->attr & ATTR_DIRECTORY;

    if (d1 && d2)
        return web_cmp_criteria(e1, e2, global_settings.sort_dir);
    if (!d1 && !d2)
        return web_cmp_criteria(e1, e2, global_settings.sort_file);
    return (int)d2 - (int)d1;   /* directory goes first */
}

/* Collect one directory's entries, sorted the way the Rockbox file
 * browser sorts them.  audio_only keeps just audio files (used to
 * build directory playlists).  Returns count, or -1 when the
 * directory cannot be opened. */
static int web_collect_dir(const char *path, struct web_entry *ents,
                           int max, bool audio_only)
{
    DIR *dp = opendir(path);
    if (!dp)
        return -1;

    int n = 0;
    struct DIRENT *ent;
    while ((ent = readdir(dp)) != NULL)
    {
        const char *nm = ent->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, ".."))
            continue;
        if (n >= max)
            break;

        struct dirinfo di = dir_get_info(dp, ent);
        bool isdir = di.attribute & ATTR_DIRECTORY;
        int attr = isdir ? ATTR_DIRECTORY : filetype_get_attr(nm);
        /* audio-only listings must contain neither directories nor
         * non-audio files (the on-device browser plays directories'
         * audio files only) */
        if (audio_only &&
            (isdir || (attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO))
            continue;

        snprintf(ents[n].name, WEB_NAME_MAX, "%s", nm);
        ents[n].mtime = (long)di.mtime;
        ents[n].attr = attr;
        n++;
    }
    closedir(dp);

    qsort(ents, (size_t)n, sizeof(struct web_entry), web_cmp_entry);
    return n;
}

static void web_json_reply(int fd, int code, const char *text,
                           const char *msg)
{
    web_http_head(fd, code, text, "application/json", (long)strlen(msg));
    web_send_str(fd, msg);
}

/* Directory listing in Rockbox VFS terms -- the same opendir /
 * readdir / dir_get_info layer the on-device file browser uses.
 * Without a path= query the browser default
 * (global_settings.start_directory) is listed.  Connection thread. */
static void web_serve_files(int fd, const char *qpath)
{
    char dir[MAX_PATH];
    char fixed[MAX_PATH];
    if (!web_query_value(qpath, "path=", dir, sizeof(dir)) || !dir[0])
        snprintf(dir, sizeof(dir), "%s", global_settings.start_directory);
    if (dir[0] == '/')
        snprintf(fixed, sizeof(fixed), "%s", dir);
    else
        snprintf(fixed, sizeof(fixed), "/%s", dir);

    struct web_entry *ents = malloc((size_t)WEB_FILES_MAX * sizeof(*ents));
    if (!ents)
    {
        web_json_reply(fd, 500, "Server Error", "{\"error\":\"oom\"}");
        return;
    }
    int n = web_collect_dir(fixed, ents, WEB_FILES_MAX, false);
    if (n < 0)
    {
        free(ents);
        web_json_reply(fd, 404, "Not Found", "{\"error\":\"nodir\"}");
        return;
    }

    size_t cap = 16 * 1024;
    char *buf = malloc(cap);
    if (!buf)
    {
        free(ents);
        web_json_reply(fd, 500, "Server Error", "{\"error\":\"oom\"}");
        return;
    }

    char esc[192];
    web_json_escape(esc, sizeof(esc), fixed);
    size_t o = (size_t)snprintf(buf, cap, "{\"path\":\"%s\",\"dirs\":[",
                                esc);

    for (int pass = 0; pass < 2; pass++)
    {
        bool want_dir = pass == 0;
        bool first = true;
        for (int i = 0; i < n; i++)
        {
            if (((ents[i].attr & ATTR_DIRECTORY) != 0) != want_dir)
                continue;
            web_json_escape(esc, sizeof(esc), ents[i].name);
            size_t need = strlen(esc) + 8;
            if (o + need + 4 > cap)
            {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb)
                {
                    free(buf);
                    free(ents);
                    web_json_reply(fd, 500, "Server Error",
                                   "{\"error\":\"oom\"}");
                    return;
                }
                buf = nb;
            }
            o += (size_t)snprintf(buf + o, cap - o, "%s\"%s\"",
                                  first ? "" : ",", esc);
            first = false;
        }
        if (pass == 0)
            o += (size_t)snprintf(buf + o, cap - o, "],\"files\":[");
    }
    o += (size_t)snprintf(buf + o, cap - o, "]}");

    web_http_head(fd, 200, "OK", "application/json", (long)o);
    web_send_all(fd, buf, o);
    free(buf);
    free(ents);
}

/* POST /api/fileop?op=play|del&path=...  Play goes through the command
 * queue (audio APIs need rockbox thread context); delete is a plain
 * syscall and runs inline. */
static void web_serve_fileop(int fd, const char *path)
{
    char op[8] = "";
    char pval[MAX_PATH];
    bool hasp = web_query_value(path, "path=", pval, sizeof(pval));
    web_query_value(path, "op=", op, sizeof(op));

    if (strcmp(op, "play") == 0 && hasp && pval[0] == '/')
    {
        pthread_mutex_lock(&W.mutex);
        snprintf(W.pend_path, sizeof(W.pend_path), "%s", pval);
        pthread_mutex_unlock(&W.mutex);
        web_push_cmd(WEB_CMD_FILEPLAY, 0);
        web_log("fileplay %s", pval);
        web_json_reply(fd, 200, "OK", "{\"ok\":true}");
    }
    else if (strcmp(op, "del") == 0 && hasp && pval[0] == '/')
    {
        int rc = remove(pval);
        web_log("filedel %s rc=%d", pval, rc);
        if (rc == 0)
            web_json_reply(fd, 200, "OK", "{\"ok\":true}");
        else
            web_json_reply(fd, 500, "Server Error",
                           "{\"error\":\"delete failed\"}");
    }
    else
        web_json_reply(fd, 400, "Bad Request", "{\"error\":\"bad op\"}");
}

/* POST /api/upload?path=<dir>&name=<file> with a raw body -- streams
 * straight from the socket because bodies can be huge.  Connection
 * thread. */
static void web_serve_upload(int fd, const char *headers, const char *path,
                             long content_len)
{
    if (!web_authed(headers) && !web_query_token(path))
    {
        web_json_reply(fd, 401, "Unauthorized",
                       "{\"error\":\"unauthorized\"}");
        return;
    }

    char dir[MAX_PATH], name[256], full[MAX_PATH + 300];
    if (!web_query_value(path, "path=", dir, sizeof(dir)) ||
        !web_query_value(path, "name=", name, sizeof(name)) ||
        name[0] == '\0' || strchr(name, '/') || strstr(name, ".."))
    {
        web_json_reply(fd, 400, "Bad Request", "{\"error\":\"bad path\"}");
        return;
    }

    if (dir[0] && dir[strlen(dir) - 1] == '/')
        snprintf(full, sizeof(full), "%s%s", dir, name);
    else
        snprintf(full, sizeof(full), "%s/%s", dir, name);

    if (content_len < 0)
        content_len = 0;

    int out = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
    {
        web_log("upload open fail %s", full);
        web_json_reply(fd, 500, "Server Error",
                       "{\"error\":\"cannot create\"}");
        return;
    }

    long total = 0;
    char buf[16384];
    bool ok = true;
    while (total < content_len)
    {
        size_t want = content_len - total > (long)sizeof(buf)
                          ? sizeof(buf)
                          : (size_t)(content_len - total);
        ssize_t r = recv(fd, buf, want, 0);
        if (r <= 0)
        {
            ok = false;
            break;
        }
        ssize_t w = write(out, buf, r);
        if (w != r)
        {
            ok = false;
            break;
        }
        total += r;
    }
    close(out);

    if (!ok)
    {
        remove(full);               /* discard the partial upload */
        web_log("upload fail %s (%ld bytes)", full, total);
        web_json_reply(fd, 500, "Server Error",
                       "{\"error\":\"transfer failed\"}");
        return;
    }

    web_log("upload ok %s (%ld bytes)", full, total);
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"ok\":true,\"bytes\":%ld}", total);
    web_json_reply(fd, 200, "OK", msg);
}

static void web_handle_http(int fd, char *method, char *path,
                            char *headers, char *body)
{
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0)
    {
        web_serve_file(fd, WEB_PAGE_INDEX);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/login") == 0)
    {
        char needle[16];
        snprintf(needle, sizeof(needle), "code=%s", W.code);
        if (strstr(body, needle) == NULL)
        {
            const char *msg = "{\"error\":\"bad code\"}";
            web_http_head(fd, 401, "Unauthorized", "application/json",
                          (long)strlen(msg));
            web_send_str(fd, msg);
            return;
        }
        char reply[80];
        web_log("login ok");
        snprintf(reply, sizeof(reply), "{\"token\":\"%s\"}", W.token);
        web_http_head(fd, 200, "OK", "application/json", (long)strlen(reply));
        web_send_str(fd, reply);
        return;
    }

    if (!web_authed(headers) && !web_query_token(path))
    {
        const char *msg = "{\"error\":\"unauthorized\"}";
        web_http_head(fd, 401, "Unauthorized", "application/json",
                      (long)strlen(msg));
        web_send_str(fd, msg);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/status", 11) == 0)
    {
        pthread_mutex_lock(&W.mutex);
        char json[1400];
        strcpy(json, W.json);
        pthread_mutex_unlock(&W.mutex);
        web_http_head(fd, 200, "OK", "application/json", (long)strlen(json));
        web_send_str(fd, json);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/playlist", 13) == 0)
    {
        web_serve_playlist(fd);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/files", 10) == 0)
    {
        web_serve_files(fd, path);
        return;
    }

    if (strcmp(method, "POST") == 0 && strncmp(path, "/api/fileop", 11) == 0)
    {
        web_serve_fileop(fd, path);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/cmd") == 0)
    {
        int cmd = WEB_CMD_NONE;
        long param = 0;

        if (strstr(body, "cmd=playpause"))
            cmd = WEB_CMD_PLAYPAUSE;
        else if (strstr(body, "cmd=next"))
            cmd = WEB_CMD_NEXT;
        else if (strstr(body, "cmd=prev"))
            cmd = WEB_CMD_PREV;
        else if (strstr(body, "cmd=stop"))
            cmd = WEB_CMD_STOP;
        else if (strstr(body, "cmd=seek"))
        {
            cmd = WEB_CMD_SEEK;
            param = web_parse_param(body, "param");
        }
        else if (strstr(body, "cmd=volume"))
        {
            cmd = WEB_CMD_VOL;
            param = web_parse_param(body, "param");
        }
        else if (strstr(body, "cmd=jump"))
        {
            cmd = WEB_CMD_JUMP;
            param = web_parse_param(body, "param");
        }
        else if (strstr(body, "cmd=repeat"))
            cmd = WEB_CMD_REPEAT;

        if (cmd != WEB_CMD_NONE)
            web_push_cmd(cmd, param);

        web_log("cmd req cmd=%d param=%ld body=[%.60s]", cmd, param,
                body ? body : "");
        const char *msg = "{\"ok\":true}";
        web_http_head(fd, 200, "OK", "application/json", (long)strlen(msg));
        web_send_str(fd, msg);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/cover", 10) == 0)
    {
        web_serve_cover(fd);
        return;
    }

    /* WebSocket upgrade */
    if (strcmp(method, "GET") == 0 && strncmp(path, "/ws", 3) == 0 &&
        strcasestr(headers, "Upgrade: websocket"))
    {
        web_ws_client(fd, headers);
        return;
    }

    /* any other GET: static file below WEB_PAGE_DIR (no "..") */
    if (strcmp(method, "GET") == 0 && path[0] == '/' &&
        strncmp(path, WEB_PAGE_DIR "/", sizeof(WEB_PAGE_DIR)) == 0 &&
        !strstr(path, ".."))
    {
        char file[256];
        snprintf(file, sizeof(file), "%s", path);
        char *q = strchr(file, '?');
        if (q)
            *q = '\0';
        web_serve_file(fd, file);
        return;
    }

    const char *msg = "not found";
    web_http_head(fd, 404, "Not Found", "text/plain", (long)strlen(msg));
    web_send_str(fd, msg);
}

/* ------------------------------------------------------------------ */
/* websocket client                                                    */
/* ------------------------------------------------------------------ */

static void web_ws_register(int fd)
{
    /* pushes must never block the worker: broadcasts send to these
     * sockets outside the mutex, so they have to be non-blocking */
    fcntl(fd, F_SETFL, O_NONBLOCK);

    pthread_mutex_lock(&W.mutex);
    for (int i = 0; i < WEB_WS_MAX; i++)
    {
        if (W.ws_fd[i] < 0)
        {
            W.ws_fd[i] = fd;
            pthread_mutex_unlock(&W.mutex);
            return;
        }
    }
    pthread_mutex_unlock(&W.mutex);
}

static void web_ws_unregister(int fd)
{
    pthread_mutex_lock(&W.mutex);
    for (int i = 0; i < WEB_WS_MAX; i++)
    {
        if (W.ws_fd[i] == fd)
            W.ws_fd[i] = -1;
    }
    pthread_mutex_unlock(&W.mutex);
}

void web_ws_client(int fd, const char *headers)
{
    const char *key = strcasestr(headers, "Sec-WebSocket-Key:");
    if (!key)
    {
        close(fd);
        return;
    }
    key += strlen("Sec-WebSocket-Key:");
    while (*key == ' ')
        key++;
    char k[64] = "";
    snprintf(k, sizeof(k), "%s", key);
    char *eol = strchr(k, '\r');
    if (eol) *eol = '\0';
    eol = strchr(k, '\n');
    if (eol) *eol = '\0';

    char cat[128];
    snprintf(cat, sizeof(cat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", k);
    uint8_t hash[20];
    char b64[32];
    web_sha1((const uint8_t *)cat, strlen(cat), hash);
    web_b64(hash, 20, b64);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n", b64);
    if (!web_send_str(fd, resp))
    {
        close(fd);
        return;
    }

    web_ws_register(fd);
    web_log("ws client fd=%d", fd);

    /* Keep the connection open; ignore client frames, drop on error
     * or when the server stops. */
    while (W.running)
    {
        uint8_t hdr[2];
        ssize_t n = recv(fd, hdr, 2, 0);
        if (n == 0)
            break;                      /* peer closed */
        if (n < 0)
        {
            /* SO_RCVTIMEO fired: idle but healthy client (browsers
             * stay silent until they ping or close) - keep waiting */
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;                      /* hard error */
        }
        if (n == 1)
        {
            /* partial header: fetch the remaining byte */
            if (recv(fd, hdr + 1, 1, 0) <= 0)
                break;
        }
        uint64_t len = hdr[1] & 0x7f;
        bool masked = (hdr[1] & 0x80) != 0;
        if (len == 126)
        {
            uint8_t ext[2];
            if (recv(fd, ext, 2, 0) != 2)
                break;
            len = (ext[0] << 8) | ext[1];
        }
        else if (len == 127)
            break;                       /* oversize, ignore */
        uint8_t mask[4] = { 0 };
        if (masked && recv(fd, mask, 4, 0) != 4)
            break;
        while (len > 0)
        {
            uint8_t sink[64];
            size_t chunk = len > sizeof(sink) ? sizeof(sink) : len;
            if (recv(fd, sink, chunk, 0) <= 0)
            {
                len = 0;
                break;
            }
            len -= chunk;
        }
        if (hdr[0] == 0x88)              /* close */
            break;
        if (hdr[0] == 0x89)              /* ping -> pong */
        {
            uint8_t pong[2] = { 0x8A, 0 };
            send(fd, pong, 2, MSG_NOSIGNAL);
        }
    }

    web_ws_unregister(fd);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* connection threads                                                  */
/* ------------------------------------------------------------------ */

static void *web_conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char method[8] = "", path[256] = "", headers[2048];
    char body[512] = "";

    if (web_read_line(fd, path, sizeof(path)) < 0)
        goto out;
    {
        char *sp = strchr(path, ' ');
        if (!sp)
            goto out;
        *sp = '\0';
        snprintf(method, sizeof(method), "%s", path);
        char *p2 = sp + 1;
        char *sp2 = strchr(p2, ' ');
        if (sp2)
            *sp2 = '\0';
        char full[256];
        snprintf(full, sizeof(full), "%s", p2);
        /* url-decode path; the query string is KEPT because the
         * token auth (?token=) reads it (WS and cover requests
         * cannot send an Authorization header) */
        size_t o = 0;
        for (size_t i = 0; full[i] && o + 1 < sizeof(path); i++)
        {
            if (full[i] == '%' && isxdigit((unsigned char)full[i + 1]) &&
                isxdigit((unsigned char)full[i + 2]))
            {
                char hex[3] = { full[i + 1], full[i + 2], 0 };
                path[o++] = (char)strtol(hex, NULL, 16);
                i += 2;
            }
            else
                path[o++] = full[i];
        }
        path[o] = '\0';
    }

    /* headers */
    size_t hlen = 0;
    headers[0] = '\0';
    while (hlen + 2 < sizeof(headers))
    {
        char line[512];
        if (web_read_line(fd, line, sizeof(line)) < 0)
            goto out;
        if (line[0] == '\0')
            break;
        int l = snprintf(headers + hlen, sizeof(headers) - hlen,
                         "%s\n", line);
        if (l > 0)
            hlen += l;
    }

    /* uploads stream straight from the socket (bodies may be huge) */
    if (strncmp(path, "/api/upload", 11) == 0)
    {
        char *ucl = strcasestr(headers, "Content-Length:");
        web_serve_upload(fd, headers, path, ucl ? atol(ucl + 15) : 0);
        goto out;
    }

    /* body (POST) */
    char *cl = strcasestr(headers, "Content-Length:");
    if (cl)
    {
        long n = atol(cl + 15);
        if (n > 0 && n < (long)sizeof(body))
        {
            size_t got = 0;
            while (got < (size_t)n)
            {
                ssize_t r = recv(fd, body + got, n - got, 0);
                if (r <= 0)
                    goto out;
                got += r;
            }
            body[n] = '\0';
        }
    }

    web_handle_http(fd, method, path, headers, body);

out:
    if (W.running && strncmp(path, "/ws", 3) != 0)
        close(fd);                       /* ws closed itself */
    return NULL;
}

static void *web_accept_thread(void *arg)
{
    (void)arg;

    while (W.running)
    {
        int fd = accept(W.listen_fd, NULL, NULL);
        if (fd < 0)
        {
            if (!W.running)
                break;
            sleep(HZ / 10);
            continue;
        }

        pthread_t pt;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 48 * 1024);
        if (pthread_create(&pt, &attr, web_conn_thread,
                           (void *)(intptr_t)fd) != 0)
            close(fd);
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* start / stop                                                        */
/* ------------------------------------------------------------------ */

static void web_gen_code(char *buf, size_t len)
{
    static const char set[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (size_t i = 0; i + 1 < len; i++)
        buf[i] = set[rand() % (sizeof(set) - 1)];
    buf[len - 1] = '\0';
}

bool web_control_running(void)
{
    return W.running;
}

static bool web_start(void)
{
    if (W.running)
        return true;

    /* Local file playback only (see web_local_playback). */
    if (!web_local_playback())
        return false;

    /* Refuse to run without the page files (shipped under
     * .rockbox/web/control/); there is deliberately no built-in
     * fallback page. */
    if (!web_page_present())
        return false;

    srand((unsigned)(current_tick & 0x7fffffff) ^ (unsigned)time(NULL));
    web_gen_code(W.code, sizeof(W.code));
    for (int i = 0; i < 8; i++)
        snprintf(W.token + i * 2, 3, "%02X", rand() & 0xff);
    W.token[16] = '\0';

    for (int i = 0; i < WEB_WS_MAX; i++)
        W.ws_fd[i] = -1;
    W.cmd_rd = W.cmd_wr = 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    W.port = WEB_PORT_MAIN;
    addr.sin_port = htons(W.port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        W.port = WEB_PORT_ALT;
        addr.sin_port = htons(W.port);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            close(fd);
            return false;
        }
    }

    if (listen(fd, 16) < 0)
    {
        close(fd);
        return false;
    }
    W.listen_fd = fd;

    remove(WEB_LOG_PATH);
    web_log("start port=%d code=%s", W.port, W.code);

    if (pthread_create(&W.accept_pt, NULL, web_accept_thread, NULL) != 0)
    {
        close(fd);
        W.listen_fd = -1;
        return false;
    }

    web_worker_run = true;
    if (!web_worker_started &&
        create_thread(web_worker_thread, web_worker_stack,
                      sizeof(web_worker_stack), 0, "web worker"
                      IF_PRIO(, PRIORITY_USER_INTERFACE)
                      IF_COP(, CPU)) > 0)
        web_worker_started = true;
    else if (!web_worker_started)
        web_worker_run = false;

    web_log("worker create=%d", web_worker_started);
    W.running = true;
    return true;
}

void web_control_stop(void)
{
    if (!W.running)
        return;

    W.running = false;
    shutdown(W.listen_fd, SHUT_RDWR);
    close(W.listen_fd);
    W.listen_fd = -1;
    pthread_join(W.accept_pt, NULL);

    pthread_mutex_lock(&W.mutex);
    for (int i = 0; i < WEB_WS_MAX; i++)
    {
        if (W.ws_fd[i] >= 0)
        {
            shutdown(W.ws_fd[i], SHUT_RDWR);
            W.ws_fd[i] = -1;
        }
    }
    pthread_mutex_unlock(&W.mutex);

    web_worker_run = false;
}

/* ------------------------------------------------------------------ */
/* device screen                                                       */
/* ------------------------------------------------------------------ */

#define WEB_ROWS 4

static char web_ip[48];

static void web_row(int row, char *buffer, size_t buffer_len)
{
    const char *str;

    if (row < 0 || row >= WEB_ROWS)
    {
        buffer[0] = '\0';
        return;
    }

    switch (row)
    {
        case 0:
            str = W.running ? web_str(LANG_WEB_RUNNING)
                            : web_str(LANG_WEB_STOPPED);
            snprintf(buffer, buffer_len, "%s: %s",
                     web_str(LANG_WEB_SERVICE), str);
            break;
        case 1:
            snprintf(buffer, buffer_len, "%s: %s",
                     web_str(LANG_WEB_CODE), W.code);
            break;
        case 2:
            snprintf(buffer, buffer_len, "%s: http://%s:%d/",
                     web_str(LANG_WEB_ADDRESS), web_ip[0] ? web_ip : "-",
                     W.port);
            break;
        default:
            str = W.running ? web_str(LANG_WEB_STOP)
                            : web_str(LANG_WEB_START);
            snprintf(buffer, buffer_len, "%s", str);
            break;
    }
}

static const char *web_name_cb(int selected, void *data,
                               char *buffer, size_t buffer_len)
{
    (void)data;
    web_row(selected, buffer, buffer_len);
    return buffer;
}

static int web_action_cb(int action, struct gui_synclist *lists)
{
    static int last_running = -1;

    (void)lists;

    if (action == ACTION_STD_OK)
    {
        int sel = gui_synclist_get_sel_pos(lists);
        if (sel == WEB_ROWS - 1)
        {
            if (W.running)
                web_control_stop();
            else
            {
#if defined(HAVE_WIFI_MENU)
                char ip[48];
                if (wifi_hal_get_ip(ip, sizeof(ip)))
                    snprintf(web_ip, sizeof(web_ip), "%s", ip);
#endif
                splash(0, web_str(LANG_WEB_STARTING));
                if (!web_start())
                    splash(HZ * 2, web_str(LANG_WEB_FAILED));
            }
            return ACTION_REDRAW;
        }
        return ACTION_NONE;
    }

    if (action == ACTION_NONE)
    {
        if (last_running != (W.running ? 1 : 0))
        {
            last_running = W.running ? 1 : 0;
            return ACTION_REDRAW;
        }
    }
    return action;
}

void web_control_screen(void)
{
    web_ip[0] = '\0';

    /* The web remote drives the local file player only: refuse in any
     * external mode (USB DAC, bluetooth in/out, future modes). */
    if (!web_local_playback())
    {
        splash(HZ * 2, web_str(LANG_WEB_NOT_LOCAL));
        return;
    }

    /* No built-in fallback page: refuse to start without the files. */
    if (!web_page_present())
    {
        splash(HZ * 2, web_str(LANG_WEB_NO_FILES));
        return;
    }

#if defined(HAVE_WIFI_MENU)
    if (!wifi_hal_is_up())
    {
        splash(HZ * 2, web_str(LANG_WIFI_OFF));
        return;
    }
    {
        char ip[48];
        if (wifi_hal_get_ip(ip, sizeof(ip)))
            snprintf(web_ip, sizeof(web_ip), "%s", ip);
    }
#endif

    if (!W.running)
    {
        splash(0, web_str(LANG_WEB_STARTING));
        if (!web_start())
        {
            splash(HZ * 2, web_str(LANG_WEB_FAILED));
            return;
        }
    }

    struct simplelist_info info;
    simplelist_info_init(&info, web_str(LANG_WEB_CONTROL), WEB_ROWS, NULL);
    info.get_name = web_name_cb;
    info.action_callback = web_action_cb;
    info.timeout = HZ * 2;
    info.selection = WEB_ROWS - 1;
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);
    /* Back: server keeps running in the background */
}

#endif /* HAVE_WEB_CONTROL */
