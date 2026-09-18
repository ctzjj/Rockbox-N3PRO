/***************************************************************************
 * Network radio stream engine (generic).
 *
 * A detached worker thread owns the socket side (HTTP or HTTPS via the
 * system OpenSSL, HLS playlist/segment fetching, MPEG-TS demux) and fills
 * a compressed-data ring.  A decoder thread of its own (netfm_playback.c)
 * consumes that ring and plays the PCM through its own mixer channel, so
 * the local playback state is never involved.
 *
 * The UI never blocks: it only reads the small status snapshot.
 *
 * Preset file, one station per line:  name,url
 * Supported: HTTP MP3/ICY streams and HLS v3 (MPEG-TS, AAC-LC or MPEG
 * audio).  HTTPS uses the firmware's own OpenSSL through dlopen so a
 * build without a TLS library still runs (HTTPS then reports an error).
 ****************************************************************************/
#include "config.h"

#ifdef HAVE_NETFM

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdarg.h>

#include "audio.h"
#include "netfm_playback.h"
#include "dsp_core.h"
#include "kernel.h"
#include "metadata.h"
#include "netfm_stream.h"
#include "pcm_mixer.h"
#include "playback.h"
#include "thread.h"
#include "wifi_hal.h"
#if defined(CAYIN_N3PRO)
#include "n3pro-bt-input.h"
#endif
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
#include "usb.h"
#endif

/* ---- temporary diagnostics (NETFM_DBG) ---- */
#define NETFM_STREAM_DBG 0
#define NETFM_STREAM_DBG_VERBOSE 0
static void ns_log(const char *fmt, ...)
{
#if NETFM_STREAM_DBG
    char buf[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        /* bypass app_open(): write to the host /tmp tmpfs, not the SD card */
        int fd = (int)syscall(SYS_openat, AT_FDCWD, "/tmp/nfstream.log",
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0)
        {
            syscall(SYS_write, fd, buf, (size_t)n);
            syscall(SYS_close, fd);
        }
    }
#else
    (void)fmt;
#endif
}
#define NSLOG(...) ns_log(__VA_ARGS__)


#define NETFM_RING_SIZE      (192 * 1024)
#define NETFM_HEADER_SIZE    4096
/* One HLS segment; live audio segments are a few hundred kB.  Rockbox
 * forbids dynamic allocation, so this is a static buffer. */
#define NETFM_SEGMENT_MAX    (512 * 1024)
#define NETFM_PLAYLIST_MAX   8192
#define NETFM_PES_MAX        (64 * 1024)
#define NETFM_CONNECT_TIMEOUT_MS 2500
#define NETFM_PROBE_US       50000      /* 50 ms */
#define NETFM_PROBE_TRIES    100        /* 5 s */
#define NETFM_STOP_TRIES     25         /* 500 ms of cooperative waiting */
#define NETFM_RECONNECT_US   1000000

enum netfm_input_mode
{
    NETFM_INPUT_MP3 = 0,
    NETFM_INPUT_HLS,
};

/* ---------------------------------------------------------------- */
/* TLS (system OpenSSL via dlopen; no link-time dependency)          */
/* ---------------------------------------------------------------- */

typedef struct ssl_ctx_st NETFM_SSL_CTX;
typedef struct ssl_st NETFM_SSL;
typedef struct ssl_method_st NETFM_SSL_METHOD;

#define NETFM_SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define NETFM_TLSEXT_NAMETYPE_host_name    0

struct netfm_tls_api
{
    void *handle;
    NETFM_SSL_METHOD *(*client_method)(void);
    int (*library_init)(void);
    NETFM_SSL_CTX *(*ctx_new)(const NETFM_SSL_METHOD *);
    void (*ctx_free)(NETFM_SSL_CTX *);
    void (*ctx_set_verify)(NETFM_SSL_CTX *, int, int (*)(int, void *));
    NETFM_SSL *(*ssl_new)(NETFM_SSL_CTX *);
    void (*ssl_free)(NETFM_SSL *);
    int (*set_fd)(NETFM_SSL *, int);
    long (*ctrl)(NETFM_SSL *, int, long, void *);
    int (*do_connect)(NETFM_SSL *);
    int (*read)(NETFM_SSL *, void *, int);
    int (*write)(NETFM_SSL *, const void *, int);
    int (*get_error)(const NETFM_SSL *, int);
    void (*shutdown_ssl)(NETFM_SSL *);
};

static struct netfm_tls_api tls;
static bool tls_tried;

static void *tls_sym(void *h, const char *name)
{
    return dlsym(h, name);
}

static bool netfm_tls_load(void)
{
    const char *names[] = { "libssl.so.1.0.0", "libssl.so.1.1",
                            "libssl.so.3", "libssl.so", NULL };
    if (tls_tried)
        return tls.handle != NULL;
    tls_tried = true;

    for (int i = 0; names[i] && !tls.handle; i++)
        tls.handle = dlopen(names[i], RTLD_NOW);
    if (!tls.handle)
        return false;
    dlopen("libcrypto.so.1.0.0", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libcrypto.so.1.1", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libcrypto.so.3", RTLD_NOW | RTLD_GLOBAL);

    tls.client_method = tls_sym(tls.handle, "SSLv23_client_method");
    tls.library_init  = tls_sym(tls.handle, "SSL_library_init");
    tls.ctx_new       = tls_sym(tls.handle, "SSL_CTX_new");
    tls.ctx_free      = tls_sym(tls.handle, "SSL_CTX_free");
    tls.ctx_set_verify = tls_sym(tls.handle, "SSL_CTX_set_verify");
    tls.ssl_new       = tls_sym(tls.handle, "SSL_new");
    tls.ssl_free      = tls_sym(tls.handle, "SSL_free");
    tls.set_fd        = tls_sym(tls.handle, "SSL_set_fd");
    tls.ctrl          = tls_sym(tls.handle, "SSL_ctrl");
    tls.do_connect    = tls_sym(tls.handle, "SSL_connect");
    tls.read          = tls_sym(tls.handle, "SSL_read");
    tls.write         = tls_sym(tls.handle, "SSL_write");
    tls.get_error     = tls_sym(tls.handle, "SSL_get_error");
    tls.shutdown_ssl  = tls_sym(tls.handle, "SSL_shutdown");

    if (!tls.client_method || !tls.ctx_new || !tls.ssl_new || !tls.set_fd ||
        !tls.do_connect || !tls.read || !tls.write || !tls.get_error)
    {
        dlclose(tls.handle);
        memset(&tls, 0, sizeof(tls));
        return false;
    }
    if (tls.library_init)
        tls.library_init();
    return true;
}

/* ---------------------------------------------------------------- */
/* connection (plain or TLS)                                         */
/* ---------------------------------------------------------------- */

struct netfm_conn
{
    int fd;
    NETFM_SSL *ssl;
    NETFM_SSL_CTX *ctx;
};

static ssize_t netfm_conn_read(struct netfm_conn *c, void *buf, size_t len)
{
    if (c->ssl)
        return (ssize_t)tls.read(c->ssl, buf, (int)len);
    return recv(c->fd, buf, len, 0);
}

static ssize_t netfm_conn_write(struct netfm_conn *c, const void *buf,
                                size_t len)
{
    if (c->ssl)
        return (ssize_t)tls.write(c->ssl, buf, (int)len);
    return send(c->fd, buf, len, MSG_NOSIGNAL);
}

static void netfm_conn_close(struct netfm_conn *c)
{
    if (c->ssl)
    {
        if (tls.shutdown_ssl)
            tls.shutdown_ssl(c->ssl);
        tls.ssl_free(c->ssl);
    }
    if (c->ctx && tls.ctx_free)
        tls.ctx_free(c->ctx);
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->ssl = NULL;
    c->ctx = NULL;
}

/* Split an url into host/port/path and report whether TLS is needed.
 * Returns 0 on success, -1 for an unsupported or malformed url. */
static int netfm_url_split(const char *url, char *host, size_t host_size,
                           int *port, char *path, size_t path_size,
                           bool *is_tls)
{
    const char *p, *slash;
    if (strncmp(url, "http://", 7) == 0)
    {
        *is_tls = false;
        *port = 80;
        p = url + 7;
    }
    else if (strncmp(url, "https://", 8) == 0)
    {
        *is_tls = true;
        *port = 443;
        p = url + 8;
    }
    else
        return -1;

    slash = strchr(p, '/');
    if (!slash)
        slash = p + strlen(p);
    if ((size_t)(slash - p) >= host_size || (size_t)(slash - p) == 0)
        return -1;
    memcpy(host, p, (size_t)(slash - p));
    host[slash - p] = '\0';
    snprintf(path, path_size, "%s", *slash ? slash : "/");

    char *colon = strrchr(host, ':');
    if (colon)
    {
        *colon++ = '\0';
        *port = atoi(colon);
    }
    if (*port < 1 || *port > 65535)
        return -1;
    return 0;
}

/* connect() with a timeout: Linux retries SYN for well over a minute on
 * an unreachable host, which would keep the worker (and the whole stream
 * teardown) alive far too long. */
static int connect_timeout(int fd, const struct sockaddr *addr,
                           socklen_t addrlen, int ms)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, addr, addrlen);
    if (rc != 0 && errno == EINPROGRESS)
    {
        fd_set wfds;
        struct timeval tv;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc > 0)
        {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0)
                err = -1;
            rc = err == 0 ? 0 : -1;
        }
        else
            rc = -1;
    }
    else if (rc != 0)
        rc = -1;

    fcntl(fd, F_SETFL, flags);
    return rc;
}

static int netfm_conn_open(const char *url, char *host, size_t host_size,
                           char *path, size_t path_size,
                           struct netfm_conn *c)
{
    struct addrinfo hints, *res, *ai;
    char service[8];
    int port;
    bool is_tls;

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    if (netfm_url_split(url, host, host_size, &port, path, path_size,
                        &is_tls) != 0)
    {
        return -1;
    }
    if (is_tls && !netfm_tls_load())
        return -2;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0)
    {
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next)
    {
        c->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c->fd < 0)
            continue;
        if (connect_timeout(c->fd, ai->ai_addr, ai->ai_addrlen,
                            NETFM_CONNECT_TIMEOUT_MS) == 0)
            break;
        close(c->fd);
        c->fd = -1;
    }
    freeaddrinfo(res);
    if (c->fd < 0)
    {
        return -1;
    }

    {
        struct timeval tv = { 3, 0 };
        setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (is_tls)
    {
        c->ctx = tls.ctx_new(tls.client_method());
        if (!c->ctx)
        {
            netfm_conn_close(c);
            return -2;
        }
        if (tls.ctx_set_verify)
            tls.ctx_set_verify(c->ctx, 0, NULL);   /* no CA store on device */
        c->ssl = tls.ssl_new(c->ctx);
        if (!c->ssl)
        {
            netfm_conn_close(c);
            return -2;
        }
        tls.set_fd(c->ssl, c->fd);
        if (tls.ctrl)
            tls.ctrl(c->ssl, NETFM_SSL_CTRL_SET_TLSEXT_HOSTNAME,
                     NETFM_TLSEXT_NAMETYPE_host_name, (void *)host);
        if (tls.do_connect(c->ssl) != 1)
        {
            netfm_conn_close(c);
            return -2;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* source state                                                      */
/* ---------------------------------------------------------------- */

struct netfm_source
{
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t thread;
    volatile bool running;
    volatile bool active;
    bool probe_done;
    volatile bool codec_ready;     /* worker resolved the stream type */
    volatile bool codec_started;   /* UI brought the decoder up */
    volatile bool starting;        /* decoder bring-up in flight */
    volatile bool thread_alive;    /* worker has not returned yet */
    bool in_pes;          /* MPEG-TS PES reassembly state */
    unsigned long bytes_total;  /* compressed bytes produced so far */
    unsigned long bytes_mark;   /* accounting for the bitrate estimate */
    long tick_mark;
    size_t head;
    size_t tail;
    unsigned char ring[NETFM_RING_SIZE];
    char url[NETFM_STREAM_URL_LEN];
    struct netfm_stream_status status;
    struct mp3entry id3;
    enum netfm_input_mode mode;
    char codec[12];

    /* HLS */
    char playlist_url[NETFM_STREAM_URL_LEN];
    unsigned int next_sequence;
    bool sequence_init;
    int pmt_pid;
    int audio_pid;
    int ts_stream_type;
    unsigned char pes[NETFM_PES_MAX];
    size_t pes_len;
};

static struct netfm_source source = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .ready = PTHREAD_COND_INITIALIZER,
};
static struct netfm_conn *volatile active_conn;


static size_t used_locked(void)
{
    return source.head >= source.tail ? source.head - source.tail
        : NETFM_RING_SIZE - source.tail + source.head;
}

static size_t free_locked(void)
{
    return NETFM_RING_SIZE - used_locked() - 1;
}


static void set_state(enum netfm_stream_state state)
{
    pthread_mutex_lock(&source.lock);
    source.status.state = state;
    pthread_mutex_unlock(&source.lock);
}

static void set_format(const char *fmt)
{
    pthread_mutex_lock(&source.lock);
    snprintf(source.status.format, sizeof(source.status.format), "%s", fmt);
    pthread_mutex_unlock(&source.lock);
}

/* The worker is about to return: let stop()/start() know it is safe to
 * reuse the ring and the codec source. */
static void worker_done(void)
{
    pthread_mutex_lock(&source.lock);
    source.thread_alive = false;
    pthread_cond_broadcast(&source.ready);
    pthread_mutex_unlock(&source.lock);
}

/* Blocking ring write; returns false when the stream was stopped. */
static bool ring_write(const unsigned char *data, size_t size)
{
    /* The codec extracts whole frames from the ring, so the write must be
     * atomic: a partially written frame would leave libfaad parsing
     * garbage after a stop (it faults with a division by zero). */
    if (size > NETFM_RING_SIZE - 1)
        return false;               /* cannot happen with our buffers */

    pthread_mutex_lock(&source.lock);
    while (free_locked() < size && source.running)
        pthread_cond_wait(&source.ready, &source.lock);
    if (!source.running)
    {
        pthread_mutex_unlock(&source.lock);
        return false;               /* keep the ring frame-aligned */
    }

    size_t first = MIN(size, NETFM_RING_SIZE - source.head);
    memcpy(source.ring + source.head, data, first);
    memcpy(source.ring, data + first, size - first);
    source.head = (source.head + size) % NETFM_RING_SIZE;
    source.bytes_total += size;
    source.status.buffer_percent =
        (int)(used_locked() * 100 / (NETFM_RING_SIZE - 1));
    pthread_cond_broadcast(&source.ready);
    pthread_mutex_unlock(&source.lock);
    return true;
}

/* ---------------------------------------------------------------- */
/* HTTP helper (playlists and HLS segments)                          */
/* ---------------------------------------------------------------- */

/* Static scratch for one fetched resource and for the parsed playlist
 * text (the playlist must stay valid while its segments are fetched).
 * No dynamic allocation: Rockbox code must use static memory. */
static unsigned char http_buf[NETFM_SEGMENT_MAX];
static char pl_text[NETFM_PLAYLIST_MAX];

/* Fetch a whole http(s) resource into http_buf; returns its length, or 0
 * on failure.  The resource is truncated at the buffer size. */
static size_t fetch_http(const char *url)
{
    struct netfm_conn c;
    char host[256], path[NETFM_STREAM_URL_LEN];
    char request[1024];
    char headers[NETFM_HEADER_SIZE];
    size_t hlen = 0, size = 0;
    int rlen;

    if (netfm_conn_open(url, host, sizeof(host), path, sizeof(path), &c) != 0)
        return 0;

    /* let netfm_stream_stop() abort a fetch that is in flight */
    active_conn = &c;

    rlen = snprintf(request, sizeof(request),
                    "GET %.511s HTTP/1.0\r\nHost: %.255s\r\n"
                    "User-Agent: Mozilla/5.0\r\nAccept: */*\r\n"
                    "Connection: close\r\n\r\n", path, host);
    if (rlen < 0 || rlen >= (int)sizeof(request) ||
        netfm_conn_write(&c, request, (size_t)rlen) <= 0)
        goto out;

    while (source.running && hlen + 1 < sizeof(headers))
    {
        ssize_t n = netfm_conn_read(&c, headers + hlen, 1);
        if (n <= 0)
            break;
        hlen++;
        headers[hlen] = '\0';
        if (hlen >= 4 && strcmp(headers + hlen - 4, "\r\n\r\n") == 0)
            break;
    }
    if (!strstr(headers, " 200 ") && !strstr(headers, " 206 "))
        goto out;

    while (source.running && size < NETFM_SEGMENT_MAX)
    {
        ssize_t n = netfm_conn_read(&c, http_buf + size,
                                    NETFM_SEGMENT_MAX - size);
        if (n <= 0)
            break;
        size += (size_t)n;
    }

out:
    if (size == 0)
    active_conn = NULL;
    netfm_conn_close(&c);
    return size;
}

/* Resolve a possibly relative reference against the playlist url. */
static void resolve_url(const char *base, const char *ref, char *out,
                        size_t out_size)
{
    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0)
    {
        snprintf(out, out_size, "%s", ref);
        return;
    }
    const char *scheme_end = strstr(base, "://");
    const char *host_start = scheme_end ? scheme_end + 3 : base;
    const char *host_end = strchr(host_start, '/');
    if (!host_end)
        host_end = host_start + strlen(host_start);

    if (ref[0] == '/')
    {
        size_t prefix = (size_t)(host_end - base);
        if (prefix >= out_size)
            prefix = out_size - 1;
        memcpy(out, base, prefix);
        snprintf(out + prefix, out_size - prefix, "%s", ref);
        return;
    }

    const char *slash = strrchr(base, '/');
    if (slash && slash > host_start)
    {
        size_t prefix = (size_t)(slash - base + 1);
        if (prefix >= out_size)
            prefix = out_size - 1;
        memcpy(out, base, prefix);
        snprintf(out + prefix, out_size - prefix, "%s", ref);
        return;
    }
    size_t prefix = (size_t)(host_end - base);
    if (prefix >= out_size)
        prefix = out_size - 1;
    memcpy(out, base, prefix);
    snprintf(out + prefix, out_size - prefix, "/%s", ref);
}

static unsigned int parse_sequence(const char *line)
{
    const char *p = line + strlen("#EXT-X-MEDIA-SEQUENCE:");
    while (*p == ' ')
        p++;
    return (unsigned int)strtoul(p, NULL, 10);
}

/* ---------------------------------------------------------------- */
/* MPEG-TS demux                                                     */
/* ---------------------------------------------------------------- */

static void ts_handle_pat(const unsigned char *section, size_t len)
{
    if (len < 8 || section[0] != 0x00)
        return;
    int section_len = ((section[1] & 0x0f) << 8) | section[2];
    for (int i = 8; i + 4 <= section_len && i + 4 <= (int)len; i += 4)
    {
        int program = (section[i] << 8) | section[i + 1];
        int pid = ((section[i + 2] & 0x1f) << 8) | section[i + 3];
        if (program != 0)
        {
            source.pmt_pid = pid;
            break;
        }
    }
}

static void ts_handle_pmt(const unsigned char *section, size_t len)
{
    if (len < 12 || section[0] != 0x02)
        return;
    int section_len = ((section[1] & 0x0f) << 8) | section[2];
    int program_info = ((section[10] & 0x0f) << 8) | section[11];
    int i = 12 + program_info;
    while (i + 5 <= section_len && i + 5 <= (int)len)
    {
        int stream_type = section[i];
        int pid = ((section[i + 1] & 0x1f) << 8) | section[i + 2];
        int es_info = ((section[i + 3] & 0x0f) << 8) | section[i + 4];
        if (stream_type == 0x0f || stream_type == 0x03 ||
            stream_type == 0x04 || stream_type == 0x11)
        {
            source.audio_pid = pid;
            source.ts_stream_type = stream_type;
            /* raw ADTS AAC has no container: aac.codec wants MP4, the
             * bitstream-filter codec decodes bare ADTS frames */
            snprintf(source.codec, sizeof(source.codec), "%s",
                     (stream_type == 0x0f || stream_type == 0x11)
                         ? "aac_bsf" : "mpa");
            break;
        }
        i += 5 + es_info;
    }
}

/* AAC ADTS sampling frequency index -> Hz (0..12 valid) */
static const int aac_samplerates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

/* Set while the probe parses the first segment: the stream type/rate are
 * learned, but nothing is written to the ring yet (the ring is small and
 * the decoder is not consuming during the probe). */
static bool ts_probe_only;

/* Feed one complete PES payload to the ring. */
static void ts_emit_pes(const unsigned char *pes, size_t len)
{
    if (len == 0)
        return;
    size_t i = 0;
    if (len > 9 && pes[0] == 0 && pes[1] == 0 && pes[2] == 1)
        i = MIN((size_t)(9 + pes[8]), len);

    if (source.ts_stream_type == 0x0f || source.ts_stream_type == 0x11)
    {
        /* AAC in ADTS framing: emit whole frames */
        while (i + 7 <= len)
        {
            if (pes[i] != 0xff || (pes[i + 1] & 0xf6) != 0xf0)
            {
                i++;
                continue;
            }
            int frame = ((pes[i + 3] & 3) << 11) | (pes[i + 4] << 3) |
                        (pes[i + 5] >> 5);
            if (frame < 7 || i + (size_t)frame > len)
                break;

            /* the ADTS header carries the real sample rate; the codec
             * reads id3->frequency when it configures the DSP, so learn
             * it here (the probe runs before the codec starts) */
            int sfi = (pes[i + 2] >> 2) & 0xf;
            if (sfi < 13 && aac_samplerates[sfi] &&
                source.id3.frequency != aac_samplerates[sfi])
            {
                NSLOG("stream: adts sfi=%d rate=%d probe_only=%d\n",
                      sfi, aac_samplerates[sfi], (int)ts_probe_only);
                source.id3.frequency = aac_samplerates[sfi];
                pthread_mutex_lock(&source.lock);
                source.status.sample_rate = aac_samplerates[sfi];
                pthread_mutex_unlock(&source.lock);
            }

            if (ts_probe_only)
            {
                i += (size_t)frame;
                continue;
            }
            ring_write(pes + i, (size_t)frame);
            i += (size_t)frame;
        }
    }
    else
    {
        /* MPEG audio in PES: hand the payload over; the mpa codec
         * resynchronises on its own */
        if (i < len && !ts_probe_only)
            ring_write(pes + i, len - i);
    }
}

static void ts_feed(const unsigned char *ts, size_t len)
{
    for (size_t off = 0; off + 188 <= len; off += 188)
    {
        const unsigned char *p = ts + off;
        if (p[0] != 0x47)
            continue;
        int pid = ((p[1] & 0x1f) << 8) | p[2];
        int afc = (p[3] >> 4) & 3;
        int pos = 4;
        bool start = (p[1] & 0x40) != 0;
        if (afc == 0 || afc == 2)
            continue;
        if (afc == 3)
        {
            if (5 + p[4] > 188)
                continue;
            pos += 1 + p[4];
        }
        if (pos >= 188)
            continue;
        const unsigned char *payload = p + pos;
        size_t plen = 188 - pos;

        if (pid == 0)
        {
            /* PAT: pointer field then the section */
            size_t ptr = payload[0];
            if (ptr + 4 <= plen)
                ts_handle_pat(payload + 1 + ptr, plen - 1 - ptr);
            continue;
        }
        if (source.pmt_pid >= 0 && pid == source.pmt_pid)
        {
            size_t ptr = payload[0];
            if (ptr + 4 <= plen)
                ts_handle_pmt(payload + 1 + ptr, plen - 1 - ptr);
            continue;
        }
        if (source.audio_pid < 0 || pid != source.audio_pid)
            continue;

        if (start)
        {
            if (source.in_pes && source.pes_len)
                ts_emit_pes(source.pes, source.pes_len);
            source.pes_len = 0;
            source.in_pes = true;
        }
        if (!source.in_pes)
            continue;
        size_t n = MIN(plen, NETFM_PES_MAX - source.pes_len);
        if (n == 0)
        {
            ts_emit_pes(source.pes, source.pes_len);
            source.pes_len = 0;
            n = MIN(plen, NETFM_PES_MAX);
        }
        memcpy(source.pes + source.pes_len, payload, n);
        source.pes_len += n;
    }
}

/* ---------------------------------------------------------------- */
/* HLS                                                               */
/* ---------------------------------------------------------------- */

static bool hls_fetch_playlist(void)
{
    NSLOG("worker: fetch pl t=%ld\n", (long)current_tick);
    size_t len = fetch_http(source.playlist_url);
    NSLOG("worker: pl len=%d t=%ld\n", (int)len, (long)current_tick);
    if (len == 0)
    {
        return false;
    }
    if (len >= sizeof(pl_text))
        len = sizeof(pl_text) - 1;
    memcpy(pl_text, http_buf, len);
    pl_text[len] = '\0';

    char segment[NETFM_STREAM_URL_LEN];
    char *save = pl_text;
    char *line;
    unsigned int seq = 0;
    bool got_sequence = false;
    bool fetched_any = false;

    while ((line = strsep(&save, "\n")) != NULL)
    {
        while (*line == '\r' || *line == ' ' || *line == '\t')
            line++;
        if (*line == '\0')
            continue;
        if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0)
        {
            seq = parse_sequence(line);
            got_sequence = true;
            continue;
        }
        if (*line == '#')
            continue;

        if (!got_sequence)
            seq = source.sequence_init ? source.next_sequence : 0;
        if (!source.sequence_init || seq >= source.next_sequence)
        {
            resolve_url(source.playlist_url, line, segment, sizeof(segment));
            NSLOG("worker: fetch seg seq=%u t=%ld\n", seq, (long)current_tick);
            size_t tslen = fetch_http(segment);
            NSLOG("worker: seg len=%d t=%ld\n", (int)tslen, (long)current_tick);
            if (tslen)
            {
                if (source.pes_len)
                    ts_emit_pes(source.pes, source.pes_len);
                source.pes_len = 0;
                ts_feed(http_buf, tslen);
                NSLOG("worker: fed t=%ld\n", (long)current_tick);
                fetched_any = true;
            }
            source.next_sequence = seq + 1;
            source.sequence_init = true;
        }
        if (got_sequence)
            seq++;
    }
    return fetched_any;
}

/* Probe the first HLS segment so the codec can be chosen before the
 * codec thread is started. */
static bool hls_probe(void)
{
    size_t len = fetch_http(source.playlist_url);
    if (len == 0)
        return false;
    if (len >= sizeof(pl_text))
        len = sizeof(pl_text) - 1;
    memcpy(pl_text, http_buf, len);
    pl_text[len] = '\0';

    char segment[NETFM_STREAM_URL_LEN];
    segment[0] = '\0';
    char *save = pl_text;
    char *line;
    unsigned int seq = 0;
    bool have_seq = false;
    while ((line = strsep(&save, "\n")) != NULL)
    {
        while (*line == '\r' || *line == ' ' || *line == '\t')
            line++;
        if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0)
        {
            seq = parse_sequence(line);
            have_seq = true;
            continue;
        }
        if (*line && *line != '#')
        {
            resolve_url(source.playlist_url, line, segment, sizeof(segment));
            /* the first segment is fetched again for playback once the
             * decoder is up, so the ring is filled from a clean start */
            source.next_sequence = have_seq ? seq : 0;
            source.sequence_init = true;
            break;
        }
    }
    if (!segment[0])
    {
        return false;
    }

    size_t tslen = fetch_http(segment);
    if (!tslen)
        return false;
    snprintf(source.codec, sizeof(source.codec), "aac_bsf");   /* default */
    source.pmt_pid = -1;
    source.audio_pid = -1;
    ts_probe_only = true;
    ts_feed(http_buf, tslen);
    ts_probe_only = false;
    /* the segment is fetched again for playback; drop the probe's
     * partial PES so playback starts on a clean boundary */
    source.pes_len = 0;
    source.in_pes = false;
    return source.audio_pid >= 0;
}

/* ---------------------------------------------------------------- */
/* worker                                                            */
/* ---------------------------------------------------------------- */

static void *download_thread(void *unused)
{
    (void)unused;

    if (source.mode == NETFM_INPUT_HLS)
    {
        set_state(NETFM_STREAM_BUFFERING);
        {
            bool probed = false;
            /* a probe can fail on a transient network hiccup (cold DNS,
             * segment not yet published); retry before giving up */
            for (int attempt = 0; attempt < 3 && source.running; attempt++)
            {
                if (hls_probe())
                {
                    probed = true;
                    break;
                }
                usleep(1000000);
            }
            if (!probed)
            {
                source.probe_done = true;
                set_state(NETFM_STREAM_ERROR);
                worker_done();
                return NULL;
            }
        }
        set_format(source.codec[0] == 'a' ? "AAC" : "MP3");
        source.probe_done = true;
        pthread_mutex_lock(&source.lock);
        source.codec_ready = true;
        pthread_mutex_unlock(&source.lock);
        while (source.running)
        {
            bool r = hls_fetch_playlist();
            if (r)
            {
                pthread_mutex_lock(&source.lock);
                source.status.state = NETFM_STREAM_PLAYING;
                pthread_mutex_unlock(&source.lock);
                NSLOG("stream: hls segment ok seq=%u\n", source.next_sequence);
                usleep(500 * 1000);
            }
            else
            {
                NSLOG("stream: hls segment FAIL seq=%u\n", source.next_sequence);
                set_state(NETFM_STREAM_DISCONNECTED);
                usleep(NETFM_RECONNECT_US);
            }
        }
        NSLOG("stream: hls worker exit running=%d\n", (int)source.running);
        worker_done();
        return NULL;
    }

    /* direct HTTP(S) MP3 / ICY stream, with reconnect */
    snprintf(source.codec, sizeof(source.codec), "mpa");
    source.probe_done = true;
    pthread_mutex_lock(&source.lock);
    source.codec_ready = true;
    pthread_mutex_unlock(&source.lock);
    while (source.running)
    {
        struct netfm_conn c;
        char host[256], path[NETFM_STREAM_URL_LEN];
        char request[1024];
        char headers[NETFM_HEADER_SIZE];
        unsigned char body[8192];
        size_t hlen = 0;
        int rlen, rc;

        set_state(NETFM_STREAM_CONNECTING);
        rc = netfm_conn_open(source.url, host, sizeof(host), path,
                             sizeof(path), &c);
        if (rc != 0)
        {
            set_state(rc == -2 ? NETFM_STREAM_ERROR : NETFM_STREAM_DISCONNECTED);
            usleep(NETFM_RECONNECT_US);
            continue;
        }
        active_conn = &c;
        rlen = snprintf(request, sizeof(request),
                        "GET %.511s HTTP/1.0\r\nHost: %.255s\r\n"
                        "User-Agent: Mozilla/5.0\r\nAccept: */*\r\n"
                        "Connection: close\r\n\r\n", path, host);
        if (rlen < 0 || rlen >= (int)sizeof(request) ||
            netfm_conn_write(&c, request, (size_t)rlen) <= 0)
        {
            active_conn = NULL;
            netfm_conn_close(&c);
            if (!source.running)
                break;
            set_state(NETFM_STREAM_DISCONNECTED);
            usleep(NETFM_RECONNECT_US);
            continue;
        }

        while (source.running && hlen + 1 < sizeof(headers))
        {
            ssize_t n = netfm_conn_read(&c, headers + hlen, 1);
            if (n <= 0)
                break;
            hlen++;
            headers[hlen] = '\0';
            if (hlen >= 4 && strcmp(headers + hlen - 4, "\r\n\r\n") == 0)
                break;
        }
        if (!strstr(headers, " 200 ") && !strstr(headers, " 206 "))
        {
            active_conn = NULL;
            netfm_conn_close(&c);
            if (!source.running)
                break;
            set_state(NETFM_STREAM_ERROR);
            usleep(NETFM_RECONNECT_US);
            continue;
        }

        set_format("MP3");
        set_state(NETFM_STREAM_BUFFERING);
        while (source.running)
        {
            ssize_t n = netfm_conn_read(&c, body, sizeof(body));
            if (n <= 0)
                break;
            if (!ring_write(body, (size_t)n))
                break;
            pthread_mutex_lock(&source.lock);
            if (used_locked() > 32768 &&
                source.status.state != NETFM_STREAM_PLAYING)
                source.status.state = NETFM_STREAM_PLAYING;
            pthread_mutex_unlock(&source.lock);
        }
        active_conn = NULL;
        netfm_conn_close(&c);
        if (!source.running)
            break;
        set_state(NETFM_STREAM_DISCONNECTED);
        usleep(NETFM_RECONNECT_US);
    }
    worker_done();
    return NULL;
}

/* ---------------------------------------------------------------- */
/* codec external source                                             */
/* ---------------------------------------------------------------- */

/* Read from the compressed ring.  This runs on the codec thread, which is
 * a cooperative Rockbox thread: blocking it on a pthread condition would
 * stall the whole Rockbox scheduler (and the UI with it), so an empty ring
 * is waited for with the tick-based sleep instead. */
static size_t read_source(void *context, void *ptr, size_t size)
{
    struct netfm_source *s = context;
    size_t done = 0;
    static bool waiting;
    while (done < size && s->running)
    {
        pthread_mutex_lock(&s->lock);
        size_t avail = used_locked();
        if (avail)
        {
            if (waiting)
            {
                if (NETFM_STREAM_DBG_VERBOSE)
                    NSLOG("stream: read resume t=%ld\n", (long)current_tick);
                waiting = false;
            }
            size_t n = MIN(size - done, avail);
            size_t first = MIN(n, NETFM_RING_SIZE - s->tail);
            memcpy((unsigned char *)ptr + done, s->ring + s->tail, first);
            memcpy((unsigned char *)ptr + done + first, s->ring, n - first);
            s->tail = (s->tail + n) % NETFM_RING_SIZE;
            pthread_cond_broadcast(&s->ready);
            pthread_mutex_unlock(&s->lock);
            done += n;
            continue;
        }
        pthread_mutex_unlock(&s->lock);
        if (!s->running)
        {
            NSLOG("stream: read_source stop (done=%u want=%u)\n",
                  (unsigned)done, (unsigned)size);
            break;
        }
        if (!waiting)
        {
            if (NETFM_STREAM_DBG_VERBOSE)
                NSLOG("stream: read starve t=%ld done=%u/%u\n",
                      (long)current_tick, (unsigned)done, (unsigned)size);
            waiting = true;
        }
        /* raw decoder pthread: the cooperative sleep() never wakes it */
        usleep(20000);              /* 20 ms, let the worker fill the ring */
    }
    return done;
}

/* Pending chunk handed to the codec through request_buffer: the codec
 * may consume only part of it, so the remainder is kept until
 * advance_buffer says how much it actually used. */
static unsigned char req_buf[8192];
static size_t req_len;
static size_t req_pos;

static size_t read_filebuf_source(void *context, void *ptr, size_t size)
{
    size_t done = 0;
    if (!source.running)
    {
        req_pos = req_len = 0;      /* stopping: report a clean EOF */
        return 0;
    }
    if (req_pos < req_len)
    {
        size_t n = MIN(size, req_len - req_pos);
        memcpy(ptr, req_buf + req_pos, n);
        req_pos += n;
        done += n;
    }
    if (done < size && req_pos >= req_len)
    {
        req_pos = req_len = 0;
        done += read_source(context, (unsigned char *)ptr + done, size - done);
    }
    return done;
}

static void *request_source(void *context, size_t *size, size_t request)
{
    size_t want = MIN(request, sizeof(req_buf));

    /* While the stream is being stopped do not hand the codec a short
     * buffer: one ending inside an ADTS frame makes libfaad parse
     * garbage and fault.  A clean EOF makes the codec return instead. */
    if (!source.running)
    {
        req_pos = req_len = 0;
        *size = 0;
        return NULL;
    }

    /* keep the unconsumed tail and top the buffer up to the requested
     * size: the codec decodes whole frames out of what it is given, so a
     * short tail would leave it with an incomplete frame */
    if (req_pos > 0)
    {
        if (req_pos < req_len)
            memmove(req_buf, req_buf + req_pos, req_len - req_pos);
        req_len -= req_pos;
        req_pos = 0;
    }
    while (req_len < want)
    {
        size_t got = read_source(context, req_buf + req_len, want - req_len);
        if (got == 0)
            break;                  /* stream ended or stopped */
        req_len += got;
    }

    *size = req_len;
    return req_len ? req_buf : NULL;
}

static void advance_source(void *context, size_t amount)
{
    (void)context;
    req_pos += amount;
    if (req_pos > req_len)
        req_pos = req_len;
}

static bool seek_source(void *context, size_t position)
{
    (void)context;
    return position == 0;
}

static void seek_done(void *context) { (void)context; }
static void set_source_offset(void *context, size_t offset)
{
    (void)context;
    (void)offset;
}

/* The decoder reports the stream parameters through its configure
 * callback; keep the sample rate visible in the status snapshot. */
static void configure_source(void *context, int setting, long value)
{
    (void)context;
    if ((setting == DSP_SET_FREQUENCY || setting == DSP_SET_OUT_FREQUENCY) &&
        value > 0)
    {
        pthread_mutex_lock(&source.lock);
        source.status.sample_rate = (int)value;
        pthread_mutex_unlock(&source.lock);
    }
}

/* the decoder's compressed input comes from our ring */
static const struct netfm_playback_src netfm_src_ops = {
    .context = &source,
    .read_filebuf = read_filebuf_source,
    .request_buffer = request_source,
    .advance_buffer = advance_source,
    .seek_buffer = seek_source,
    .seek_complete = seek_done,
    .set_offset = set_source_offset,
    .configure = configure_source,
};

/* Ready to bring the decoder up once the worker has probed the stream. */
bool netfm_stream_codec_pending(void)
{
    /* lock-free: called from the monitor/UI threads */
    return source.codec_ready && !source.codec_started;
}

bool netfm_stream_codec_start_now(void)
{
    char codec[12];
    bool ok;

    pthread_mutex_lock(&source.lock);
    if (!source.active || source.starting || !source.codec_ready ||
        source.codec_started)
    {
        pthread_mutex_unlock(&source.lock);
        return false;
    }
    source.starting = true;
    snprintf(codec, sizeof(codec), "%s", source.codec);
    pthread_mutex_unlock(&source.lock);

    /* Stopping local playback is asynchronous and its teardown stops the
     * output channel; starting the stream before that finished lets the
     * teardown kill our channel, leaving the decoder with nowhere to
     * write (compressed buffer fills up, no sound).  Wait, bounded, for
     * the channel to be free and retry on the next status refresh. */
    for (int i = 0; i < 50; i++)
    {
        if (!(audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)) &&
            mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_STOPPED)
            break;
        sleep(HZ / 50);
    }
    if (mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_STOPPED)
    {
        pthread_mutex_lock(&source.lock);
        source.starting = false;
        pthread_mutex_unlock(&source.lock);
        return false;
    }

    pthread_mutex_lock(&source.lock);
    if (source.codec_started || !source.active)
    {
        /* a stop overtook the bring-up; do not start into a dead stream */
        source.starting = false;
        pthread_mutex_unlock(&source.lock);
        return false;
    }
    source.codec_started = true;
    pthread_mutex_unlock(&source.lock);

    /* hand the compressed ring to our own decoder thread; it plays
     * through its own mixer channel (see netfm_playback.c) */
    NSLOG("stream: codec_start_now codec=%s id3freq=%d\n",
          codec, (int)source.id3.frequency);
    ok = netfm_playback_start(codec, &source.id3, &netfm_src_ops);
    NSLOG("stream: codec_start_now -> %d\n", ok);

    pthread_mutex_lock(&source.lock);
    source.starting = false;
    pthread_mutex_unlock(&source.lock);

    if (!ok)
        set_state(NETFM_STREAM_ERROR);
    return ok;
}

/* ---------------------------------------------------------------- */
/* public API                                                        */
/* ---------------------------------------------------------------- */

static bool netfm_external_input_active(void)
{
#if defined(CAYIN_N3PRO)
    if (n3pro_bt_rx_get_active())
        return true;
#endif
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
    if (usb_audio_get_active())
        return true;
#endif
    return false;
}

bool netfm_stream_can_start(void)
{
    return !netfm_external_input_active();
}

static void netfm_monitor_start(void);

bool netfm_stream_start(const char *name, const char *url)
{
    bool is_tls;

    if (!name || !url || !wifi_hal_is_up())
        return false;
    if (netfm_external_input_active())
        return false;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return false;
    is_tls = strncmp(url, "https://", 8) == 0;
    if (is_tls && !netfm_tls_load())
        return false;                      /* TLS unavailable */

    NSLOG("stream: start name=%s url=%s\n", name, url);
    netfm_stream_stop();

    /* A previous worker that is still winding down (e.g. a station whose
     * host never answered) must not block a new selection for long: give
     * it a bounded, cooperative moment (a DNS lookup or connect can take
     * a couple of seconds), then fail only if it is really stuck. */
    for (int i = 0; i < 100 && source.thread_alive; i++)
        sleep(HZ / 50);

    pthread_mutex_lock(&source.lock);
    if (source.thread_alive)
    {
        pthread_mutex_unlock(&source.lock);
        return false;
    }
    /* A decoder that survived the stop above still reads this shared id3
     * (and divides by its bitrate): never rewrite it underneath a live
     * decoder. */
    if (netfm_playback_active())
    {
        pthread_mutex_unlock(&source.lock);
        return false;
    }
    memset(&source.status, 0, sizeof(source.status));
    snprintf(source.status.name, sizeof(source.status.name), "%.127s", name);
    snprintf(source.url, sizeof(source.url), "%.511s", url);
    snprintf(source.playlist_url, sizeof(source.playlist_url), "%.511s", url);
    source.mode = strstr(url, ".m3u8") ? NETFM_INPUT_HLS : NETFM_INPUT_MP3;
    source.status.state = NETFM_STREAM_CONNECTING;
    source.head = source.tail = 0;
    source.pes_len = 0;
    source.in_pes = false;
    req_len = req_pos = 0;
    ts_probe_only = false;
    source.bytes_total = source.bytes_mark = 0;
    source.tick_mark = current_tick;
    source.pmt_pid = -1;
    source.audio_pid = -1;
    source.ts_stream_type = -1;
    source.sequence_init = false;
    source.next_sequence = 0;
    source.probe_done = false;
    source.codec_ready = false;
    source.codec_started = false;
    source.starting = false;
    source.codec[0] = '\0';
    memset(&source.id3, 0, sizeof(source.id3));
    /* the stream's fake metadata must never stay all zero: aac_bsf
     * divides by id3->bitrate for its elapsed calculation and the codec
     * configures the DSP from id3->frequency */
    source.id3.frequency = 44100;
    source.id3.bitrate = 128;
    source.id3.layer = (source.mode == NETFM_INPUT_HLS) ? 0 : 3;
    /* the stream's own metadata is handed to the readers (WPS, status,
     * web) through netfm_stream_current_id3() while it plays; the local
     * playlist's entries are never touched by the stream */
    source.id3.title = source.status.name;
    source.id3.artist = source.status.name;
    snprintf(source.id3.path, sizeof(source.id3.path), "%.*s",
             (int)sizeof(source.id3.path) - 1, source.url);
    source.running = true;
    source.active = true;
    source.thread_alive = true;
    pthread_mutex_unlock(&source.lock);

    if (pthread_create(&source.thread, NULL, download_thread, NULL) != 0)
    {
        netfm_stream_stop();
        return false;
    }
    pthread_detach(source.thread);

    /* The radio keeps playing when its screen is left, so the decoder
     * bring-up and the takeover checks cannot live in the UI any more. */
    netfm_monitor_start();

    /* The worker probes the stream (HLS needs the PMT to pick aac/mpa)
     * and starts the decoder from its own context, so the UI never
     * blocks on DNS/TLS/network latency. */
    return true;
}

void netfm_stream_stop(void)
{
    NSLOG("stream: stop enter tid=%lu\n", (unsigned long)pthread_self());
    pthread_mutex_lock(&source.lock);
    source.running = false;
    source.active = false;
    pthread_cond_broadcast(&source.ready);
    source.status.state = NETFM_STREAM_IDLE;
    pthread_mutex_unlock(&source.lock);
    NSLOG("stream: stop 1 flags\n");

    if (active_conn && active_conn->fd >= 0)
        shutdown(active_conn->fd, SHUT_RDWR);
    NSLOG("stream: stop 2 shutdown done\n");

    /* Bluetooth-style teardown: stop the mixer channel at once and never
     * wait for the worker/decoder - they wind down on their own.  Waiting
     * (here, on the monitor, or on the audio thread) is what wedged the
     * cooperative scheduler. */
    netfm_playback_stop();
    NSLOG("stream: stop 3 playback_stop done\n");

    /* Drop the metadata handed to the decoder and the ring state: a
     * stale stream id3 must not leak into a later local playback. */
    pthread_mutex_lock(&source.lock);
    source.codec_ready = false;
    source.codec_started = false;
    source.starting = false;
    source.codec[0] = '\0';
    /* never leave it all zero - aac_bsf divides by the bitrate and the
     * decoder configures the DSP from the frequency */
    memset(&source.id3, 0, sizeof(source.id3));
    source.id3.frequency = 44100;
    source.id3.bitrate = 128;
    source.head = source.tail = 0;
    req_len = req_pos = 0;
    pthread_mutex_unlock(&source.lock);
    NSLOG("stream: stop done tid=%lu\n", (unsigned long)pthread_self());
}

/* Quick stop used by the local-playback takeover (apps/playback.c): free
 * the output immediately and let the worker/decoder wind down on their
 * own - the full stop() would block the audio thread for seconds. */
void netfm_stream_stop_async(void)
{
    NSLOG("stream: stop_async enter tid=%lu\n", (unsigned long)pthread_self());
    NSLOG("stream: sa A prelock\n");
    pthread_mutex_lock(&source.lock);
    NSLOG("stream: sa B locked\n");
    source.running = false;
    source.active = false;
    pthread_cond_broadcast(&source.ready);
    source.status.state = NETFM_STREAM_IDLE;
    pthread_mutex_unlock(&source.lock);
    NSLOG("stream: sa C unlocked\n");

    if (active_conn && active_conn->fd >= 0)
        shutdown(active_conn->fd, SHUT_RDWR);
    NSLOG("stream: sa D shutdown\n");

    /* Only flag the decoder: stopping the mixer channel here would take
     * pcm_play_lock, which the audio thread may already hold (this runs
     * on the audio thread during the local-playback takeover) and would
     * deadlock.  The monitor thread finishes the teardown. */
    netfm_playback_request_stop();
    NSLOG("stream: stop_async done tid=%lu\n", (unsigned long)pthread_self());
}

bool netfm_stream_get_status(struct netfm_stream_status *status)
{
    static struct netfm_stream_status last;
    if (!status)
        return false;
    /* never block a Rockbox software thread on source.lock: a raw worker/
     * decoder pthread may hold it, and blocking here freezes the whole
     * cooperative scheduler (UI + monitor). */
    if (pthread_mutex_trylock(&source.lock) != 0)
    {
        *status = last;
        return false;
    }
    /* estimate the stream bitrate from the compressed bytes produced */
    long now = current_tick;
    if (source.bytes_total != source.bytes_mark && now - source.tick_mark >= HZ)
    {
        unsigned long db = source.bytes_total - source.bytes_mark;
        long dt = now - source.tick_mark;
        if (dt > 0)
            source.status.bitrate =
                (int)((db * 8 * HZ) / ((unsigned long)dt * 1000));
        source.bytes_mark = source.bytes_total;
        source.tick_mark = now;
    }
    *status = last = source.status;
    pthread_mutex_unlock(&source.lock);
    return true;
}

bool netfm_stream_is_active(void)
{
    /* lock-free: this is called from the UI/audio threads and must never
     * block on source.lock, which a raw worker/decoder pthread may hold */
    return source.active;
}

/* ---------------------------------------------------------------- */
/* background monitor                                                */
/* ---------------------------------------------------------------- */

/* The radio keeps playing after its screen was left, so the decoder
 * bring-up and the checks for another source taking the output over
 * cannot live in the UI any more. */
static long netfm_mon_stack[(DEFAULT_STACK_SIZE + 0x1000) / sizeof(long)];
static volatile bool netfm_mon_running;

static void netfm_monitor_thread(void)
{
    NSLOG("stream: monitor start tid=%lu\n", (unsigned long)pthread_self());
    int mon_loops = 0;
    while (netfm_stream_is_active())
    {
        NSLOG("stream: mon loop=%d active=%d running=%d nf=%d t=%ld\n",
              ++mon_loops, (int)source.active, (int)source.running,
              (int)netfm_playback_active(), (long)current_tick);
        if (netfm_stream_codec_pending())
            netfm_stream_codec_start_now();

        /* an external input (bluetooth receive, USB DAC) taking the
         * output over must stop the radio at once */
        if (netfm_external_input_active())
        {
            netfm_stream_stop();
            break;
        }
        sleep(HZ / 4);
    }

    /* A local-playback takeover only flags the stop from the audio thread
     * (see netfm_stream_stop_async): finish the teardown here, on a
     * normal thread, where stopping the mixer channel is safe. */
    NSLOG("stream: monitor loop exit running=%d active=%d playback=%d\n",
          (int)source.running, (int)source.active, netfm_playback_active());
    if (!source.running && netfm_playback_active())
        netfm_stream_stop();

    netfm_mon_running = false;
    NSLOG("stream: monitor exit done\n");
}

static void netfm_monitor_start(void)
{
    if (netfm_mon_running)
        return;
    netfm_mon_running = true;
    if (create_thread(netfm_monitor_thread, netfm_mon_stack,
                      sizeof(netfm_mon_stack), 0, "netfm mon"
                      IF_PRIO(, PRIORITY_USER_INTERFACE)
                      IF_COP(, CPU)) <= 0)
        netfm_mon_running = false;
}

#endif /* HAVE_NETFM */
