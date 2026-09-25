/* dlna_stream.c - standalone stream engine for the DLNA renderer.
 *
 * Kept entirely on the DLNA side, sharing no code or state with the
 * network radio:
 *
 *   gmr (UPnP/SOAP/GENA) -> dlna_output.c -> dlna_stream.c (this file)
 *                                             -> dlna_playback.c
 *                                                (decoder -> mixer
 *                                                 channel -> DSP -> out)
 *
 * Same shape as the radio engine: a detached worker pthread owns the
 * socket (plain HTTP or HTTPS via the system OpenSSL, dlopen'ed) and the
 * decoder host plays the PCM through its own mixer channel.
 *
 * Where a DLNA push differs from a radio stream:
 *  - the codec is sniffed from the first body bytes (multi-format);
 *  - a finite file (Content-Length) is buffered in RAM and served as a
 *    seekable source, so container codecs (FLAC, MP4/M4A, ALAC) decode
 *    with the stock Rockbox codecs; it plays once and ends FINISHED;
 *  - a live stream (no length) keeps the forward-only ring path.
 */
#define _GNU_SOURCE             /* strcasestr for the header scan */

#include "config.h"

#ifdef HAVE_DLNA

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "audio.h"
#include "file.h"
#include "dlna_playback.h"
#include "dsp_core.h"
#include "kernel.h"
#include "metadata.h"
#include "pcm_mixer.h"
#include "playback.h"
#include "thread.h"
#include "wifi_hal.h"
#ifdef HAVE_BT_INPUT
#include "bt_input.h"
#endif
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
#include "usb.h"
#endif
#ifdef HAVE_NETFM
#include "netfm_stream.h"       /* the network radio is a peer output owner */
#endif

#include "dlna_stream.h"

#define DLNA_RING_SIZE      (4 * 1024 * 1024)
#define DLNA_HEADER_SIZE    4096
#define DLNA_CONNECT_TIMEOUT_MS 2500
#define DLNA_RECONNECT_US   1000000
#define DLNA_PLAYING_THRESHOLD 32768

/* ---------------------------------------------------------------- */
/* TLS (system OpenSSL via dlopen; no link-time dependency)          */
/* ---------------------------------------------------------------- */

typedef struct ssl_ctx_st DLNA_SSL_CTX;
typedef struct ssl_st DLNA_SSL;
typedef struct ssl_method_st DLNA_SSL_METHOD;

#define DLNA_SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define DLNA_TLSEXT_NAMETYPE_host_name    0

struct dlna_tls_api
{
    void *handle;
    DLNA_SSL_METHOD *(*client_method)(void);
    int (*library_init)(void);
    DLNA_SSL_CTX *(*ctx_new)(const DLNA_SSL_METHOD *);
    void (*ctx_free)(DLNA_SSL_CTX *);
    void (*ctx_set_verify)(DLNA_SSL_CTX *, int, int (*)(int, void *));
    DLNA_SSL *(*ssl_new)(DLNA_SSL_CTX *);
    void (*ssl_free)(DLNA_SSL *);
    int (*set_fd)(DLNA_SSL *, int);
    long (*ctrl)(DLNA_SSL *, int, long, void *);
    int (*do_connect)(DLNA_SSL *);
    int (*read)(DLNA_SSL *, void *, int);
    int (*write)(DLNA_SSL *, const void *, int);
    int (*get_error)(const DLNA_SSL *, int);
    void (*shutdown_ssl)(DLNA_SSL *);
};

static struct dlna_tls_api tls;
static bool tls_tried;

static bool dlna_tls_load(void)
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

    tls.client_method = dlsym(tls.handle, "SSLv23_client_method");
    tls.library_init  = dlsym(tls.handle, "SSL_library_init");
    tls.ctx_new       = dlsym(tls.handle, "SSL_CTX_new");
    tls.ctx_free      = dlsym(tls.handle, "SSL_CTX_free");
    tls.ctx_set_verify = dlsym(tls.handle, "SSL_CTX_set_verify");
    tls.ssl_new       = dlsym(tls.handle, "SSL_new");
    tls.ssl_free      = dlsym(tls.handle, "SSL_free");
    tls.set_fd        = dlsym(tls.handle, "SSL_set_fd");
    tls.ctrl          = dlsym(tls.handle, "SSL_ctrl");
    tls.do_connect    = dlsym(tls.handle, "SSL_connect");
    tls.read          = dlsym(tls.handle, "SSL_read");
    tls.write         = dlsym(tls.handle, "SSL_write");
    tls.get_error     = dlsym(tls.handle, "SSL_get_error");
    tls.shutdown_ssl  = dlsym(tls.handle, "SSL_shutdown");

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

struct dlna_conn
{
    int fd;
    DLNA_SSL *ssl;
    DLNA_SSL_CTX *ctx;
};

static ssize_t dlna_conn_read(struct dlna_conn *c, void *buf, size_t len)
{
    if (c->ssl)
        return (ssize_t)tls.read(c->ssl, buf, (int)len);
    return recv(c->fd, buf, len, 0);
}

static ssize_t dlna_conn_write(struct dlna_conn *c, const void *buf,
                               size_t len)
{
    if (c->ssl)
        return (ssize_t)tls.write(c->ssl, buf, (int)len);
    return send(c->fd, buf, len, MSG_NOSIGNAL);
}

static void dlna_conn_close(struct dlna_conn *c)
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

static int dlna_url_split(const char *url, char *host, size_t host_size,
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

/* connect() with a timeout: Linux would retry a SYN for over a minute
 * on an unreachable host, keeping the worker (and its teardown) alive
 * far too long. */
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

static int dlna_conn_open(const char *url, char *host, size_t host_size,
                          char *path, size_t path_size, struct dlna_conn *c)
{
    struct addrinfo hints, *res, *ai;
    char service[8];
    int port;
    bool is_tls;

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    if (dlna_url_split(url, host, host_size, &port, path, path_size,
                       &is_tls) != 0)
        return -1;
    if (is_tls && !dlna_tls_load())
        return -2;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0)
        return -1;

    for (ai = res; ai; ai = ai->ai_next)
    {
        c->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c->fd < 0)
            continue;
        if (connect_timeout(c->fd, ai->ai_addr, ai->ai_addrlen,
                            DLNA_CONNECT_TIMEOUT_MS) == 0)
            break;
        close(c->fd);
        c->fd = -1;
    }
    freeaddrinfo(res);
    if (c->fd < 0)
        return -1;

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
            dlna_conn_close(c);
            return -2;
        }
        if (tls.ctx_set_verify)
            tls.ctx_set_verify(c->ctx, 0, NULL);   /* no CA store on device */
        c->ssl = tls.ssl_new(c->ctx);
        if (!c->ssl)
        {
            dlna_conn_close(c);
            return -2;
        }
        tls.set_fd(c->ssl, c->fd);
        if (tls.ctrl)
            tls.ctrl(c->ssl, DLNA_SSL_CTRL_SET_TLSEXT_HOSTNAME,
                     DLNA_TLSEXT_NAMETYPE_host_name, (void *)host);
        if (tls.do_connect(c->ssl) != 1)
        {
            dlna_conn_close(c);
            return -2;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* source state                                                      */
/* ---------------------------------------------------------------- */

struct dlna_source
{
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t thread;
    volatile bool running;
    volatile bool active;
    volatile bool codec_ready;     /* worker resolved the stream type */
    volatile bool codec_started;   /* UI brought the decoder up */
    volatile bool starting;        /* decoder bring-up in flight */
    volatile bool thread_alive;    /* worker has not returned yet */
    volatile bool eof;             /* finite stream fully written */
    bool finite;                   /* Content-Length was present */
    unsigned long bytes_total;  /* compressed bytes produced so far */
    unsigned long long produced;/* total bytes written to the ring (abs) */
    unsigned long long base_abs;/* abs position of the oldest retained byte */
    unsigned long long read_abs;/* abs decoder read position */
    long content_length;        /* HTTP Content-Length, or -1 */
    unsigned long bytes_mark;   /* accounting for the bitrate estimate */
    long tick_mark;
    size_t head;
    size_t tail;
    unsigned char ring[DLNA_RING_SIZE];
    /* finite push: the whole body buffered in RAM, served seekably */
    unsigned char *mem;
    size_t mem_size;
    size_t mem_pos;
    char url[DLNA_STREAM_URL_LEN];
    struct dlna_stream_status status;
    struct mp3entry id3;
    char codec[12];
};

static struct dlna_source source = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .ready = PTHREAD_COND_INITIALIZER,
};
static struct dlna_conn *volatile active_conn;

/* A pushed file's RAM buffer may still be read by the decoder thread
 * after a stop: never free it synchronously.  It is retired here and
 * released by mem_reap() once the decode thread has actually returned. */
static unsigned char *pending_mem;

static void mem_reap(void);

static size_t used_locked(void)
{
    return (size_t)(source.produced - source.base_abs);
}

static size_t free_locked(void)
{
    return DLNA_RING_SIZE - used_locked();
}

static void set_state(enum dlna_stream_state state)
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

/* Free the retired push buffer, but only once no decode thread is reading
 * it (the source callbacks share the engine's global buffer). */
static void mem_reap(void)
{
    unsigned char *p;

    if (!pending_mem || !dlna_pb_idle())
        return;

    pthread_mutex_lock(&source.lock);
    p = pending_mem;
    pending_mem = NULL;
    pthread_mutex_unlock(&source.lock);
    free(p);
}

static void worker_done(void)
{
    pthread_mutex_lock(&source.lock);
    source.thread_alive = false;
    pthread_cond_broadcast(&source.ready);
    pthread_mutex_unlock(&source.lock);
}

/* Blocking ring write; returns false when the stream was stopped.  The
 * write is atomic so the codec only ever sees whole frames.  The ring
 * retains data until it is full: space is reclaimed only from bytes the
 * decoder has already read past, so a file's header/moov stays resident
 * for the container codecs' backward seeks. */
static bool ring_write(const unsigned char *data, size_t size)
{
    if (size > DLNA_RING_SIZE)
        return false;

    pthread_mutex_lock(&source.lock);
    while (free_locked() < size && source.running)
    {
        if (source.read_abs > source.base_abs)
        {
            unsigned long long room = (unsigned long long)(size - free_locked());
            unsigned long long can = source.read_abs - source.base_abs;
            if (can > room)
                can = room;
            source.base_abs += can;
            continue;
        }
        pthread_cond_wait(&source.ready, &source.lock);
    }
    if (!source.running)
    {
        pthread_mutex_unlock(&source.lock);
        return false;
    }

    size_t w = (size_t)(source.produced % DLNA_RING_SIZE);
    size_t first = MIN(size, DLNA_RING_SIZE - w);
    memcpy(source.ring + w, data, first);
    memcpy(source.ring, data + first, size - first);
    source.produced += size;
    source.head = (size_t)(source.produced % DLNA_RING_SIZE);
    source.bytes_total += size;
    source.status.buffer_percent =
        (int)(used_locked() * 100 / DLNA_RING_SIZE);
    pthread_cond_broadcast(&source.ready);
    pthread_mutex_unlock(&source.lock);
    return true;
}

/* ---------------------------------------------------------------- */
/* format sniffing (DLNA pushes are not just MP3)                    */
/* ---------------------------------------------------------------- */

/* Choose the codec from the first body bytes; decided once per stream.
 * The names are the Rockbox codec identifiers dlna_playback loads. */
static void sniff_codec(const unsigned char *p, size_t n)
{
    const char *codec = "mpa";
    const char *label = "MP3";

    pthread_mutex_lock(&source.lock);
    if (source.codec[0] != '\0' || n < 4)
    {
        pthread_mutex_unlock(&source.lock);
        return;
    }

    if (p[0] == 'f' && p[1] == 'L' && p[2] == 'a' && p[3] == 'C')
    {
        codec = "flac"; label = "FLAC";
        /* STREAMINFO starts at offset 8; the sample rate is in its bytes
         * 10..12 (20 bits).  The codec is told the rate from id3, so grab
         * it here or 48/96 kHz files play at the wrong speed. */
        if (n >= 21)
        {
            unsigned sr = ((unsigned)p[18] << 12) | ((unsigned)p[19] << 4) |
                          ((unsigned)p[20] >> 4);
            if (sr >= 8000 && sr <= 192000)
                source.id3.frequency = sr;
        }
    }
    else if (p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S')
    {
        codec = "vorbis"; label = "OGG";
    }
    else if (p[0] == 'I' && p[1] == 'D' && p[2] == '3')
    {
        codec = "mpa"; label = "MP3";
    }
    else if (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
             n >= 12 && p[8] == 'W' && p[9] == 'A' && p[10] == 'V' &&
             p[11] == 'E')
    {
        codec = "wav"; label = "WAV";
    }
    else if (n >= 8 && p[4] == 'f' && p[5] == 't' && p[6] == 'y' &&
             p[7] == 'p')
    {
        codec = "aac"; label = "M4A";
        /* libm4a's read_chunk_stsd() refuses the track unless the id3 the
         * host passes in declares the MP4/AAC codec type, so set it here
         * or qtmovie_read() fails and aac.c returns CODEC_ERROR. */
        source.id3.codectype = AFMT_MP4_AAC;
    }
    else if (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0)
    {
        /* MPEG audio sync: layer bits 00 = ADTS AAC, else MPEG audio */
        if ((p[1] & 0x06) == 0x00)
        {
            codec = "aac_bsf"; label = "AAC";
        }
        else
        {
            codec = "mpa"; label = "MP3";
        }
    }

    snprintf(source.codec, sizeof(source.codec), "%s", codec);
    snprintf(source.status.format, sizeof(source.status.format), "%s", label);
    source.codec_ready = true;
    pthread_cond_broadcast(&source.ready);
    pthread_mutex_unlock(&source.lock);
}

/* ---------------------------------------------------------------- */
/* HTTP helpers                                                      */
/* ---------------------------------------------------------------- */

static long content_length_of(const char *headers)
{
    const char *cl = strcasestr(headers, "Content-Length:");
    if (!cl)
        return -1;
    cl += 15;
    while (*cl == ' ')
        cl++;
    return strtol(cl, NULL, 10);
}

/* ---------------------------------------------------------------- */
/* worker                                                            */
/* ---------------------------------------------------------------- */

static void *download_thread(void *unused)
{
    (void)unused;

    while (source.running)
    {
        struct dlna_conn c;
        char host[256], path[DLNA_STREAM_URL_LEN];
        char request[1024];
        char headers[DLNA_HEADER_SIZE];
        unsigned char body[8192];
        size_t hlen = 0;
        int rlen, rc;

        set_state(DLNA_STREAM_CONNECTING);
        rc = dlna_conn_open(source.url, host, sizeof(host), path,
                            sizeof(path), &c);
        if (rc != 0)
        {
            set_state(rc == -2 ? DLNA_STREAM_ERROR
                               : DLNA_STREAM_DISCONNECTED);
            usleep(DLNA_RECONNECT_US);
            continue;
        }
        active_conn = &c;
        rlen = snprintf(request, sizeof(request),
                        "GET %.511s HTTP/1.0\r\nHost: %.255s\r\n"
                        "User-Agent: Mozilla/5.0\r\nAccept: */*\r\n"
                        "Connection: close\r\n\r\n", path, host);
        if (rlen < 0 || rlen >= (int)sizeof(request) ||
            dlna_conn_write(&c, request, (size_t)rlen) <= 0)
        {
            active_conn = NULL;
            dlna_conn_close(&c);
            if (!source.running)
                break;
            set_state(DLNA_STREAM_DISCONNECTED);
            usleep(DLNA_RECONNECT_US);
            continue;
        }

        while (source.running && hlen + 1 < sizeof(headers))
        {
            ssize_t n = dlna_conn_read(&c, headers + hlen, 1);
            if (n < 0)
            {
                /* SO_RCVTIMEO expiring is not end-of-headers */
                if (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;
            hlen++;
            headers[hlen] = '\0';
            if (hlen >= 4 && strcmp(headers + hlen - 4, "\r\n\r\n") == 0)
                break;
        }
        if (!strstr(headers, " 200 ") && !strstr(headers, " 206 "))
        {
            active_conn = NULL;
            dlna_conn_close(&c);
            if (!source.running)
                break;
            set_state(DLNA_STREAM_ERROR);
            usleep(DLNA_RECONNECT_US);
            continue;
        }

        /* A push with a known length is a finite file: it plays once,
         * not reconnect (the radio loop would replay the track forever).
         * Everything is streamed through the seekable ring; a finite
         * push also gets its true length so codecs can seek (FLAC seeks
         * back to the first frame after reading the metadata).  No
         * length => a live stream that reconnects. */
        long total = content_length_of(headers);
        pthread_mutex_lock(&source.lock);
        source.finite = total > 0;
        source.content_length = total;
        pthread_mutex_unlock(&source.lock);

        set_state(DLNA_STREAM_BUFFERING);
        bool sniffed = false;

        /* Everything streams through the fixed ring, which retains data
         * until it fills up so the header/moov stays seekable. */
        long received = 0;
        while (source.running)
        {
            /* a finite push is complete once Content-Length bytes arrived */
            if (source.finite && source.content_length > 0 &&
                received >= source.content_length)
                break;

            ssize_t n = dlna_conn_read(&c, body, sizeof(body));
            if (n < 0)
            {
                /* SO_RCVTIMEO expiring is NOT end-of-stream: the server
                 * (or the phone) may just be quiet for a moment.  Treating
                 * it as EOF drained the buffer and then went silent
                 * mid-track (and made the "buffer full" case look dead). */
                if (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;              /* the server closed the connection */
            received += n;
            if (!sniffed)
            {
                sniff_codec(body, (size_t)n);
                sniffed = true;
            }
            if (!ring_write(body, (size_t)n))
                break;
            pthread_mutex_lock(&source.lock);
            if (used_locked() > DLNA_PLAYING_THRESHOLD &&
                source.status.state != DLNA_STREAM_PLAYING)
                source.status.state = DLNA_STREAM_PLAYING;
            pthread_mutex_unlock(&source.lock);
        }

        active_conn = NULL;
        dlna_conn_close(&c);
        if (!source.running)
            break;

        if (source.finite)
        {
            /* no more bytes will come: let the decoder drain to its end,
             * then report it (never reconnect) */
            pthread_mutex_lock(&source.lock);
            source.eof = true;
            pthread_cond_broadcast(&source.ready);
            pthread_mutex_unlock(&source.lock);

            for (int i = 0; i < 200 && source.running && !source.codec_started; i++)
                usleep(50000);
            while (source.running && dlna_pb_active())
                usleep(100000);

            if (source.running)
            {
                dlna_pb_stop();
                pthread_mutex_lock(&source.lock);
                source.status.state = DLNA_STREAM_FINISHED;
                pthread_cond_broadcast(&source.ready);
                pthread_mutex_unlock(&source.lock);
            }
            break;
        }

        set_state(DLNA_STREAM_DISCONNECTED);
        usleep(DLNA_RECONNECT_US);
    }
    worker_done();
    return NULL;
}

/* ---------------------------------------------------------------- */
/* codec external source                                             */
/* ---------------------------------------------------------------- */

/* Read from the compressed ring.  Runs on the codec thread, a
 * cooperative Rockbox thread: blocking on a pthread condition would
 * stall the whole scheduler, so an empty ring is waited out with a
 * plain sleep.  A drained ring at end-of-file returns short/0 so the
 * codec sees its EOF. */
static size_t read_source(void *context, void *ptr, size_t size)
{
    struct dlna_source *s = context;
    size_t done = 0;

    while (done < size)
    {
        pthread_mutex_lock(&s->lock);
        unsigned long long avail = s->produced - s->read_abs;
        if (avail)
        {
            size_t n = (size_t)MIN((unsigned long long)(size - done), avail);
            size_t r = (size_t)(s->read_abs % DLNA_RING_SIZE);
            size_t first = MIN(n, DLNA_RING_SIZE - r);
            memcpy((unsigned char *)ptr + done, s->ring + r, first);
            memcpy((unsigned char *)ptr + done + first, s->ring, n - first);
            s->read_abs += n;
            pthread_cond_broadcast(&s->ready);
            pthread_mutex_unlock(&s->lock);
            done += n;
            continue;
        }
        pthread_mutex_unlock(&s->lock);
        /* stopping, or the stream is fully drained: EOF */
        if (!s->running || s->eof)
            break;
        usleep(20000);          /* 20 ms, let the worker fill the ring */
    }
    return done;
}

/* request_buffer hands the codec a pointer straight into the source (the
 * RAM buffer, or a contiguous run of the ring) and advance_buffer moves
 * the read position by exactly what the codec consumed.  There is no
 * private staging copy, so the positions the codec tracks match the
 * source exactly - container codecs (FLAC, MP4) depend on that. */

static size_t read_filebuf_source(void *context, void *ptr, size_t size)
{
    return read_source(context, ptr, size);
}

/* Scratch used to linearise a request that straddles the ring seam.  The
 * codec keeps the returned pointer until its next request_buffer(), so a
 * single buffer is safe (it is only refilled on the next request). */
static unsigned char req_linear[65536];   /* >= codec MAX_FRAMESIZE */

static void *request_source(void *context, size_t *size, size_t request)
{
    struct dlna_source *s = context;

    if (request > sizeof(req_linear))
        request = sizeof(req_linear);

    /* Wait until a whole request is buffered (or the stream ends).  A
     * frame must never be split: at the ring seam the run used to be cut
     * short, the decoder then started mid-frame and failed (FLAC returns
     * CODEC_ERROR once the cursor crossed a 4 MB boundary).  The ring
     * holds MBs, so this only waits at the very start / on a slow net. */
    for (;;)
    {
        unsigned long long avail;
        bool end;
        pthread_mutex_lock(&s->lock);
        avail = s->produced - s->read_abs;
        end = s->eof || !s->running;
        if (avail >= request || (end && avail))
        {
            size_t r = (size_t)(s->read_abs % DLNA_RING_SIZE);
            size_t contig = DLNA_RING_SIZE - r;
            size_t n = (size_t)MIN((unsigned long long)request, avail);
            void *p;

            if (n <= contig)
                p = s->ring + r;            /* contiguous: no copy */
            else
            {
                /* straddles the seam: hand back a linear copy */
                memcpy(req_linear, s->ring + r, contig);
                memcpy(req_linear + contig, s->ring, n - contig);
                p = req_linear;
            }
            *size = n;
            pthread_mutex_unlock(&s->lock);
            return p;
        }
        pthread_mutex_unlock(&s->lock);
        if (end)
        {
            *size = 0;
            return NULL;
        }
        usleep(20000);
    }
}

static void advance_source(void *context, size_t amount)
{
    struct dlna_source *s = context;

    /* consume exactly what the codec used.  The data stays retained (the
     * ring reclaims it only when it needs the space), so a skip or a seek
     * back into already-read bytes keeps working. */
    pthread_mutex_lock(&s->lock);
    s->read_abs += amount;
    if (s->read_abs > s->produced)
        s->read_abs = s->produced;
    pthread_cond_broadcast(&s->ready);
    pthread_mutex_unlock(&s->lock);
}

static bool seek_source(void *context, size_t position)
{
    struct dlna_source *s = context;

    /* accept any position still retained in the ring.  The header/moov at
     * the start stays resident until the buffer actually fills up, and
     * positions not yet downloaded are waited for. */
    {
        bool ok = false;
        for (;;)
        {
            unsigned long long pos = (unsigned long long)position;
            bool ahead, running;

            pthread_mutex_lock(&s->lock);
            if (pos >= s->base_abs && pos <= s->produced)
            {
                s->read_abs = pos;
                pthread_cond_broadcast(&s->ready);
                pthread_mutex_unlock(&s->lock);
                ok = true;
                break;
            }
            ahead = pos > s->produced;
            running = s->running;
            pthread_mutex_unlock(&s->lock);
            if (!ahead || !running)
                break;                      /* discarded, or stopped */
            usleep(20000);                  /* wait for the download */
        }
        return ok;
    }
}

static void seek_done(void *context) { (void)context; }
static void set_source_offset(void *context, size_t offset)
{
    (void)context;
    (void)offset;
}

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

static struct dlna_pb_src dlna_src_ops = {
    .context = &source,
    .filesize = -1,
    .read_filebuf = read_filebuf_source,
    .request_buffer = request_source,
    .advance_buffer = advance_source,
    .seek_buffer = seek_source,
    .seek_complete = seek_done,
    .set_offset = set_source_offset,
    .configure = configure_source,
};

bool dlna_stream_codec_pending(void)
{
    return source.codec_ready && !source.codec_started;
}

bool dlna_stream_codec_start_now(void)
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

    /* Wait, bounded, for the local-playback teardown to free the
     * output channel (mirrors the radio engine). */
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
        source.starting = false;
        pthread_mutex_unlock(&source.lock);
        return false;
    }
    source.codec_started = true;
    pthread_mutex_unlock(&source.lock);

    /* a finite push is seekable: give the decoder the real file length
     * (the announced Content-Length) so container codecs - libm4a in
     * particular - can seek and detect EOF correctly */
    if (source.finite && source.content_length > 0)
        dlna_src_ops.filesize = (off_t)source.content_length;
    else
        dlna_src_ops.filesize = -1;

    ok = dlna_pb_start(codec, &source.id3, &dlna_src_ops);
    pthread_mutex_lock(&source.lock);
    source.starting = false;
    pthread_mutex_unlock(&source.lock);

    if (!ok)
        set_state(DLNA_STREAM_ERROR);
    return ok;
}

/* ---------------------------------------------------------------- */
/* public API                                                        */
/* ---------------------------------------------------------------- */

/* true while another input owns the audio path or the radio streams */
static bool dlna_external_input_active(void)
{
#ifdef HAVE_BT_INPUT
    if (bt_input_active())
        return true;
#endif
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
    if (usb_audio_get_active())
        return true;
#endif
    return false;
}

/* Another source that owns the audio path and that the DLNA renderer
 * must never play over: an external input, or the network radio.  The
 * dependency points this way only (netfm is never taught about DLNA), so
 * a radio started while the renderer is playing makes the renderer
 * yield - see dlna_monitor_thread(). */
static bool dlna_other_source_active(void)
{
#ifdef HAVE_NETFM
    if (netfm_stream_is_active())
        return true;
#endif
    return dlna_external_input_active();
}

bool dlna_stream_can_start(void)
{
    /* local playback (the wired/headphone player) is taken over by
     * dlna_output_do_play()/dlna_menu(), which stop it first, so it is
     * not checked here; the hard blocks are the external inputs and the
     * network radio, which the renderer must never run over. */
    return !dlna_other_source_active();
}

static void dlna_monitor_start(void);

bool dlna_stream_start(const char *name, const char *url)
{
    bool is_tls;

    if (!name || !url || !wifi_hal_is_up())
        return false;
    if (!dlna_stream_can_start())
        return false;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return false;
    is_tls = strncmp(url, "https://", 8) == 0;
    if (is_tls && !dlna_tls_load())
        return false;

    dlna_stream_stop();

    for (int i = 0; i < 100 && source.thread_alive; i++)
        sleep(HZ / 50);

    pthread_mutex_lock(&source.lock);
    if (source.thread_alive)
    {
        pthread_mutex_unlock(&source.lock);
        return false;
    }
    pthread_mutex_unlock(&source.lock);

    /* A previous decode thread (stopped but not yet returned) still reads
     * the shared source: wait, bounded, before dropping the buffer and
     * starting the new decode. */
    for (int i = 0; i < 100 && !dlna_pb_idle(); i++)
        sleep(HZ / 50);
    if (!dlna_pb_idle())
        return false;
    mem_reap();

    pthread_mutex_lock(&source.lock);
    memset(&source.status, 0, sizeof(source.status));
    snprintf(source.status.name, sizeof(source.status.name), "%.127s", name);
    snprintf(source.url, sizeof(source.url), "%.511s", url);
    source.status.state = DLNA_STREAM_CONNECTING;
    source.head = source.tail = 0;
    source.bytes_total = source.bytes_mark = 0;
    source.produced = 0;
    source.base_abs = 0;
    source.read_abs = 0;
    source.content_length = -1;
    source.tick_mark = current_tick;
    source.codec_ready = false;
    source.codec_started = false;
    source.starting = false;
    source.codec[0] = '\0';
    source.eof = false;
    source.finite = false;
    free(source.mem);
    source.mem = NULL;
    source.mem_size = 0;
    source.mem_pos = 0;
    memset(&source.id3, 0, sizeof(source.id3));
    /* fake metadata must never stay all zero: aac_bsf divides by the
     * bitrate and the codec configures the DSP from the frequency */
    source.id3.frequency = 44100;
    source.id3.bitrate = 128;
    source.id3.layer = 3;
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
        dlna_stream_stop();
        return false;
    }
    pthread_detach(source.thread);

    dlna_monitor_start();
    return true;
}

void dlna_stream_stop(void)
{
    pthread_mutex_lock(&source.lock);
    source.running = false;
    source.active = false;
    pthread_cond_broadcast(&source.ready);
    source.status.state = DLNA_STREAM_IDLE;
    pthread_mutex_unlock(&source.lock);

    if (active_conn && active_conn->fd >= 0)
        shutdown(active_conn->fd, SHUT_RDWR);

    /* Bluetooth-style teardown: stop the mixer channel at once, never
     * wait for the worker/decoder - they wind down on their own. */
    dlna_pb_stop();

    mem_reap();                     /* release an earlier retired buffer */

    pthread_mutex_lock(&source.lock);
    source.codec_ready = false;
    source.codec_started = false;
    source.starting = false;
    source.codec[0] = '\0';
    source.head = source.tail = 0;
    /* NOTE: source.id3 is left alone here - a decoder may still be
     * running on it (dp_ci.id3 points at it); the next start() rebuilds
     * it after the decode thread has gone. */
    /* the decoder may still be reading the buffered push: retire it and
     * let mem_reap() release it once the decode thread is gone */
    if (source.mem)
    {
        pending_mem = source.mem;
        source.mem = NULL;
        source.mem_size = 0;
        source.mem_pos = 0;
    }
    pthread_mutex_unlock(&source.lock);

    mem_reap();
}

/* Quick stop for the local-playback takeover (apps/playback.c): free
 * the output immediately, let the worker/decoder wind down on their
 * own - the full stop() would block the audio thread. */
void dlna_stream_stop_async(void)
{
    pthread_mutex_lock(&source.lock);
    source.running = false;
    source.active = false;
    pthread_cond_broadcast(&source.ready);
    source.status.state = DLNA_STREAM_IDLE;
    pthread_mutex_unlock(&source.lock);

    if (active_conn && active_conn->fd >= 0)
        shutdown(active_conn->fd, SHUT_RDWR);

    /* flag-only: stopping the mixer channel here would take
     * pcm_play_lock, which the audio thread may already hold */
    dlna_pb_request_stop();
}

bool dlna_stream_get_status(struct dlna_stream_status *status)
{
    static struct dlna_stream_status last;
    if (!status)
        return false;
    /* never block a Rockbox software thread on source.lock */
    if (pthread_mutex_trylock(&source.lock) != 0)
    {
        *status = last;
        return false;
    }
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

bool dlna_stream_is_active(void)
{
    /* lock-free: called from UI/audio threads */
    return source.active;
}

/* ---------------------------------------------------------------- */
/* background monitor                                                */
/* ---------------------------------------------------------------- */

/* Same shape as the radio monitor: brings the decoder up once the
 * worker has sniffed the format, and retires the stream when an
 * external input (bluetooth receive, USB DAC) takes the output over -
 * the bluetooth entry screen does not know about the DLNA renderer,
 * exactly like it does not know about the radio. */
static long dlna_mon_stack[(DEFAULT_STACK_SIZE + 0x1000) / sizeof(long)];
static volatile bool dlna_mon_running;

static void dlna_monitor_thread(void)
{
    while (dlna_stream_is_active())
    {
        if (dlna_stream_codec_pending())
            dlna_stream_codec_start_now();

        if (dlna_other_source_active())
        {
            dlna_stream_stop();
            break;
        }
        mem_reap();
        sleep(HZ / 10);         /* tight enough that a radio start makes
                                 * the renderer yield before the two can
                                 * share the DSP chain */
    }

    /* A local-playback takeover only flags the stop from the audio
     * thread (dlna_stream_stop_async): finish the teardown here, on a
     * normal thread, where stopping the mixer channel is safe. */
    if (!source.running && dlna_pb_active())
        dlna_stream_stop();

    /* the decode thread is winding down: release the buffered push once
     * it is gone (never while it may still be reading it) */
    pthread_mutex_lock(&source.lock);
    if (source.mem)
    {
        pending_mem = source.mem;
        source.mem = NULL;
        source.mem_size = 0;
        source.mem_pos = 0;
    }
    pthread_mutex_unlock(&source.lock);
    mem_reap();

    dlna_mon_running = false;
}

static void dlna_monitor_start(void)
{
    if (dlna_mon_running)
        return;
    dlna_mon_running = true;
    if (create_thread(dlna_monitor_thread, dlna_mon_stack,
                      sizeof(dlna_mon_stack), 0, "dlna mon"
                      IF_PRIO(, PRIORITY_USER_INTERFACE)
                      IF_COP(, CPU)) <= 0)
        dlna_mon_running = false;
}

#endif /* HAVE_DLNA */
