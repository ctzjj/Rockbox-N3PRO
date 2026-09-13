/* BT PoC AAC: DIRECT IPC client, A2DP AAC (LATM) source.
 * Same approach as bt_direct.c (SBC): talk to bluetoothd's abstract audio
 * socket, replay GET_CAPS / OPEN(seid=2) / SET_CONFIG(16B) / START, receive
 * the L2CAP fd via SCM_RIGHTS, then encode with the FDK-AAC API exported by
 * the HiBy ALSA plugin and stream RTP packets (12B RTP + LATM, no A2DP media
 * payload header, 90 kHz RTP clock).
 */
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* --- FDK-AAC ABI (aacenc_lib.h, stable since 0.1.x) --- */
typedef int INT;
typedef unsigned int UINT;
typedef unsigned char UCHAR;

typedef struct {
    INT numBufs;
    void **bufs;
    INT *bufferIdentifiers;
    INT *bufSizes;
    INT *bufElSizes;
} AACENC_BufDesc;

typedef struct {
    INT numInSamples;
    INT numAncBytes;
} AACENC_InArgs;

typedef struct {
    INT numOutBytes;
    INT numInSamples;
    INT numAncBytes;
    INT bitResState;
} AACENC_OutArgs;

typedef struct {
    UINT maxOutBufBytes;
    UINT maxAncBytes;
    UINT inBufFillLevel;
    UINT inputChannels;
    UINT frameLength;
    UINT nDelay;
    UINT nDelayCore;
    UCHAR confBuf[64];
    UINT confSize;
} AACENC_InfoStruct;

#define AACENC_AOT              0x0100
#define AACENC_BITRATE          0x0101
#define AACENC_SAMPLERATE       0x0103
#define AACENC_CHANNELMODE      0x0106
#define AACENC_AFTERBURNER      0x0200
#define AACENC_TRANSMUX         0x0300
#define AACENC_HEADER_PERIOD    0x0301
#define AACENC_AUDIOMUXVER      0x0304

#define AOT_AAC_LC              2
#define MODE_2                  2
#define TT_MP4_LATM_MCP1        6

#define IN_AUDIO_DATA           0
#define OUT_BITSTREAM_DATA      3

static int (*aacEncOpen)(void **, unsigned, unsigned);
static int (*aacEncoder_SetParam)(void *, int, unsigned);
static int (*aacEncEncode)(void *, AACENC_BufDesc *, AACENC_BufDesc *,
                           AACENC_InArgs *, AACENC_OutArgs *);
static int (*aacEncInfo)(void *, AACENC_InfoStruct *);
static int (*aacEncClose)(void **);

static int mksock(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    a.sun_path[0] = '\0';
    strcpy(a.sun_path + 1, "/org/bluez/audio");
    if (connect(s, (void*)&a, sizeof a) < 0) { perror("connect audio"); return -1; }
    return s;
}

static void dump(const char *tag, const unsigned char *b, int n)
{
    printf("%s [%d]:", tag, n);
    for (int i = 0; i < n && i < 32; i++) printf(" %02x", b[i]);
    printf("\n");
}

static int read_full(int s, unsigned char *b, int want)
{
    int got = 0;
    while (got < want) {
        int n = recv(s, b + got, want - got, 0);
        if (n <= 0) return got;
        got += n;
    }
    return got;
}

static int read_msg(int s, unsigned char *buf, int max)
{
    int n = read_full(s, buf, 4);
    if (n < 4) return n;
    int len = buf[2] | (buf[3] << 8);
    if (len < 4 || len > max) { printf("bad len %d\n", len); return -1; }
    n = read_full(s, buf + 4, len - 4);
    return (n == len - 4) ? len : -1;
}

int main(int argc, char **argv)
{
    const char *mac = argc > 1 ? argv[1] : "84:AC:60:73:BD:6F";
    int secs = argc > 2 ? atoi(argv[2]) : 8;
    double freq = argc > 3 ? atof(argv[3]) : 440.0;
    unsigned bitrate = argc > 4 ? (unsigned)atoi(argv[4]) : 256000;

    void *h = dlopen("/usr/lib/alsa-lib/libasound_module_pcm_bluetooth.so", RTLD_NOW);
    if (!h) { printf("dlopen plugin: %s\n", dlerror()); return 1; }
    aacEncOpen = dlsym(h, "aacEncOpen");
    aacEncoder_SetParam = dlsym(h, "aacEncoder_SetParam");
    aacEncEncode = dlsym(h, "aacEncEncode");
    aacEncInfo = dlsym(h, "aacEncInfo");
    aacEncClose = dlsym(h, "aacEncClose");
    if (!aacEncOpen || !aacEncoder_SetParam || !aacEncEncode || !aacEncInfo) {
        printf("fdk syms missing: open=%p set=%p enc=%p info=%p\n",
               (void*)aacEncOpen, (void*)aacEncoder_SetParam,
               (void*)aacEncEncode, (void*)aacEncInfo);
        return 1;
    }
    printf("fdk-aac API ok\n");

    int s = mksock();
    if (s < 0) return 1;
    printf("connected to \\0/org/bluez/audio\n");

    unsigned char req[256], rsp[1024];
    int n;

    /* --- GET_CAPABILITIES --- */
    memset(req, 0, sizeof req);
    req[0] = 0x00; req[1] = 0x00; req[2] = 0xab; req[3] = 0x00;
    strcpy((char*)req + 22, mac);
    req[169] = 0x01; req[170] = 0x00;
    n = send(s, req, 171, 0);
    printf("GET_CAPS sent %d\n", n);
    n = read_msg(s, rsp, sizeof rsp);
    dump("caps rsp", rsp, n);
    if (n < 4 || rsp[0] == 0x03) { printf("GET_CAPS fail\n"); return 1; }

    /* --- BT_OPEN: AAC Source SEP seid = 2 --- */
    memset(req, 0, sizeof req);
    req[0] = 0x00; req[1] = 0x01; req[2] = 0xaa; req[3] = 0x00;
    strcpy((char*)req + 22, mac);
    req[168] = 0x02; req[169] = 0x02;
    n = send(s, req, 170, 0);
    printf("OPEN(seid=2) sent %d\n", n);
    n = read_msg(s, rsp, sizeof rsp);
    dump("open rsp", rsp, n);
    if (n < 4 || rsp[0] == 0x03) { printf("OPEN fail\n"); return 1; }

    /* --- BT_SET_CONFIGURATION: AAC MPEG-4 LC (16B) --- */
    static const unsigned char setcfg[16] =
        {0x00,0x02,0x10,0x00, 0x02,0x00,0x05,0x0c, 0x00,0x00,0x80,0x01, 0x04,0x82,0x0c,0x00};
    n = send(s, setcfg, 16, 0);
    printf("SET_CONFIG sent %d\n", n);
    n = read_msg(s, rsp, sizeof rsp);
    dump("setcfg rsp", rsp, n);
    if (n < 4 || rsp[0] == 0x03) { printf("SET_CONFIG fail\n"); return 1; }

    /* --- BT_START_STREAM --- */
    static const unsigned char startm[4] = {0x00,0x04,0x04,0x00};
    n = send(s, startm, 4, 0);
    printf("START sent %d\n", n);
    n = read_msg(s, rsp, sizeof rsp);
    dump("start rsp", rsp, n);
    n = read_msg(s, rsp, sizeof rsp);
    dump("newstream?", rsp, n);

    unsigned char cmbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov; unsigned char b1;
    struct msghdr m;
    iov.iov_base = &b1; iov.iov_len = 1;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = cmbuf; m.msg_controllen = sizeof cmbuf;
    n = recvmsg(s, &m, 0);
    int a2dp_fd = -1;
    struct cmsghdr *c;
    for (c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            a2dp_fd = *(int*)CMSG_DATA(c);
    printf("recvmsg=%d a2dp_fd=%d\n", n, a2dp_fd);
    if (a2dp_fd < 0) { printf("NO STREAM FD\n"); return 1; }

    int fl = fcntl(a2dp_fd, F_GETFL);
    if (fl >= 0) fcntl(a2dp_fd, F_SETFL, fl & ~O_NONBLOCK);
    int sndbuf = 32768;
    setsockopt(a2dp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);

    /* --- encoder init --- */
    const unsigned rate = 44100;
    void *enc = NULL;
    int err = aacEncOpen(&enc, 0x0F, 2);
    printf("aacEncOpen=%d enc=%p\n", err, enc);
    if (err) return 1;
#define SP(p, v) do { err = aacEncoder_SetParam(enc, (p), (v)); \
        printf("  set 0x%04x=%u -> %d\n", (p), (unsigned)(v), err); } while (0)
    SP(AACENC_AOT, AOT_AAC_LC);
    SP(AACENC_BITRATE, bitrate);
    SP(AACENC_SAMPLERATE, rate);
    SP(AACENC_CHANNELMODE, MODE_2);
    SP(AACENC_AFTERBURNER, 1);
    SP(AACENC_TRANSMUX, TT_MP4_LATM_MCP1);
    SP(AACENC_HEADER_PERIOD, 1);
    err = aacEncEncode(enc, NULL, NULL, NULL, NULL);
    printf("aacEncEncode(init)=%d\n", err);
    AACENC_InfoStruct info;
    memset(&info, 0, sizeof info);
    err = aacEncInfo(enc, &info);
    printf("aacEncInfo=%d channels=%u frameLength=%u maxOut=%u confSize=%u\n",
           err, info.inputChannels, info.frameLength, info.maxOutBufBytes, info.confSize);
    if (info.frameLength == 0 || info.inputChannels == 0) { printf("bad info\n"); return 1; }

    const unsigned frame_samples = info.frameLength;         /* per channel */
    const unsigned in_samples = info.frameLength * info.inputChannels;

    /* sine source */
    static int16_t pcm[2048 * 2];
    static int16_t sinlut[1024];
    for (int i = 0; i < 1024; i++)
        sinlut[i] = (int16_t)(9000.0 * sin(2.0 * M_PI * (double)i / 1024.0));
    unsigned int phq = 0;
    unsigned int incq = (unsigned int)(freq * 1024.0 / (double)rate * 65536.0 + 0.5);

    /* encoder output may reach info.maxOutBufBytes (>= 1536 for 256k stereo),
     * but an A2DP packet may only carry mtu-12 bytes -> encode into a full
     * size buffer, then fragment when sending */
    const size_t outbuf_cap = info.maxOutBufBytes + 64;
    int in_bufferIdentifiers[] = { IN_AUDIO_DATA };
    int out_bufferIdentifiers[] = { OUT_BITSTREAM_DATA };
    int in_bufSizes[] = { (int)(in_samples * 2) };
    int out_bufSizes[] = { (int)outbuf_cap };
    int in_bufElSizes[] = { 2 };
    int out_bufElSizes[] = { 1 };

    const size_t mtu = 672;
    const size_t payload_len_max = mtu - 12;
    static unsigned char pkt[12 + 4096];

    uint16_t seqn = 0;
    uint32_t ssrc = 0x11223344;
    unsigned long long ts_num = 0;   /* exact 90kHz accumulator */
    long total_frames = (long)rate * secs;   /* audio frames (per channel) */
    long sent = 0;
    long long min_lead = 1000000000LL;
#define LEAD_US 100000LL
    setbuf(stdout, NULL);

    int dumpfd = -1;
    if (getenv("AAC_DUMP"))
        dumpfd = open(getenv("AAC_DUMP"), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    printf("encoding: frame=%u samples, bitrate=%u, total=%ld frames\n",
           frame_samples, bitrate, total_frames);

    while (sent < total_frames) {
        for (unsigned i = 0; i < frame_samples; i++) {
            int16_t v = sinlut[(phq >> 16) & 1023];
            phq += incq;
            pcm[2 * i] = v; pcm[2 * i + 1] = v;
        }

        unsigned char *payload = pkt + 12;
        void *inbuf = pcm;
        void *outbuf = payload;

        AACENC_BufDesc in_buf = {
            .numBufs = 1, .bufs = (void**)&inbuf,
            .bufferIdentifiers = in_bufferIdentifiers,
            .bufSizes = in_bufSizes, .bufElSizes = in_bufElSizes,
        };
        AACENC_BufDesc out_buf = {
            .numBufs = 1, .bufs = (void**)&outbuf,
            .bufferIdentifiers = out_bufferIdentifiers,
            .bufSizes = out_bufSizes, .bufElSizes = out_bufElSizes,
        };
        AACENC_InArgs in_args = { .numInSamples = (INT)in_samples, .numAncBytes = 0 };
        AACENC_OutArgs out_args = { 0 };

        err = aacEncEncode(enc, &in_buf, &out_buf, &in_args, &out_args);
        if (err) { printf("aacEncEncode err=%d\n", err); break; }

        /* the encoder buffers input (codec delay): early calls consume samples
         * but emit nothing yet, so always advance the clock and only send when
         * there are output bytes */
        unsigned cons = out_args.numInSamples ? (unsigned)out_args.numInSamples : in_samples;
        unsigned cframes = cons / (info.inputChannels ? info.inputChannels : 2);
        ts_num += (unsigned long long)cframes * 90000ULL;
        sent += cframes;

        if (out_args.numOutBytes > 0) {
            if (dumpfd >= 0 && sent < 20)
                write(dumpfd, payload, out_args.numOutBytes);

            uint32_t ts = (uint32_t)(ts_num / 90000);

            /* RTP header + LATM payload; fragment per RFC 3016 (no extra header) */
            size_t plen = out_args.numOutBytes;
            do {
                size_t chunk = plen > payload_len_max ? payload_len_max : plen;
                int mark = plen <= payload_len_max;

                pkt[0] = 0x80;
                pkt[1] = mark ? 0xE0 : 0x60;
                pkt[2] = seqn >> 8; pkt[3] = seqn & 0xff;
                pkt[4] = ts >> 24; pkt[5] = ts >> 16; pkt[6] = ts >> 8; pkt[7] = ts;
                memcpy(pkt + 8, &ssrc, 4);

                ssize_t w = send(a2dp_fd, pkt, 12 + chunk, 0);
                if (w < 0) {
                    if (errno == EAGAIN) { usleep(2000); continue; }
                    perror("send a2dp"); printf("sent=%ld\n", sent); goto done;
                }
                seqn++;
                plen -= chunk;
                if (plen) memmove(payload, payload + chunk, plen);
            } while (plen);
        }

        gettimeofday(&t1, NULL);
        long long elapsed_us = (long long)(t1.tv_sec - t0.tv_sec) * 1000000LL
                             + (t1.tv_usec - t0.tv_usec);
        long long media_us = (long long)sent * 1000000LL / rate;
        long long lead = media_us - elapsed_us;
        if (lead > LEAD_US) usleep((useconds_t)(lead - LEAD_US));
        if (lead < min_lead) min_lead = lead;
    }
done:
    gettimeofday(&t1, NULL);
    long long total_us = (long long)(t1.tv_sec - t0.tv_sec) * 1000000LL
                       + (t1.tv_usec - t0.tv_usec);
    printf("done: %ld frames (%.1fs audio) in %.2fs wall, min_lead=%.0fms\n", sent,
           (double)sent / rate, (double)total_us / 1e6, (double)min_lead / 1000.0);
    if (dumpfd >= 0) close(dumpfd);
    aacEncClose(&enc);
    close(a2dp_fd);
    close(s);
    return 0;
}
