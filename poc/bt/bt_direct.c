/* BT PoC v3: DIRECT IPC client - bypass the alsa plugin entirely.
 * Talks to bluetoothd's abstract audio socket \0/org/bluez/audio,
 * replays the captured message sequence, receives the L2CAP fd via
 * SCM_RIGHTS, then SBC-encodes a sine (borrowing the plugin's exported
 * sbc_* API) and streams RTP packets directly.
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

/* bluez libsbc public ABI */
struct sbc_struct {
    unsigned long flags;
    uint8_t frequency;
    uint8_t blocks;
    uint8_t subbands;
    uint8_t mode;
    uint8_t allocation;
    uint8_t bitpool;
    uint8_t endian;
    void *priv;
    void *priv2;
    void *priv3;
};
typedef struct sbc_struct sbc_t;
static int (*sbc_init)(sbc_t*, long);
static int (*sbc_init_primitives)(sbc_t*);
static ssize_t (*sbc_encode)(sbc_t*, const void*, size_t, void*, size_t);
static size_t (*sbc_get_codesize)(sbc_t*);
static size_t (*sbc_get_frame_length)(sbc_t*);
static unsigned (*sbc_get_frame_duration)(sbc_t*);
static void (*sbc_finish)(sbc_t*);

/* SBC constants (bluez) */
#define SBC_LR           0x00
#define SBC_MODE_JOINT   0x02
#define SBC_BLK_16       0x30
#define SBC_SUBBAND_8    0x02
#define SBC_ALLOC_LOUDN  0x00
#define SBC_FREQ_44100   0x20

static int mksock(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    a.sun_path[0] = '\0';
    strcpy(a.sun_path + 1, "/org/bluez/audio");
    if (connect(s, (void*)&a, sizeof a) < 0) {
        perror("connect audio");
        return -1;
    }
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
    /* SOCK_STREAM framing: 4-byte header, then (length-4) payload */
    int n = read_full(s, buf, 4);
    if (n < 4) return n;
    int len = buf[2] | (buf[3] << 8);
    if (len < 4 || len > max) { printf("bad len %d (type %02x code %02x)\n", len, buf[0], buf[1]); return -1; }
    n = read_full(s, buf + 4, len - 4);
    return (n == len - 4) ? len : -1;
}

int main(int argc, char **argv)
{
    const char *mac = argc > 1 ? argv[1] : "84:AC:60:73:BD:6F";
    int secs = argc > 2 ? atoi(argv[2]) : 8;
    double freq = argc > 3 ? atof(argv[3]) : 440.0;

    void *h = dlopen("/usr/lib/alsa-lib/libasound_module_pcm_bluetooth.so", RTLD_NOW);
    if (!h) { printf("dlopen plugin: %s\n", dlerror()); return 1; }
    sbc_init = dlsym(h, "sbc_init");
    sbc_init_primitives = dlsym(h, "sbc_init_primitives");
    sbc_encode = dlsym(h, "sbc_encode");
    sbc_get_codesize = dlsym(h, "sbc_get_codesize");
    sbc_get_frame_length = dlsym(h, "sbc_get_frame_length");
    sbc_get_frame_duration = dlsym(h, "sbc_get_frame_duration");
    sbc_finish = dlsym(h, "sbc_finish");
    if (!sbc_init || !sbc_encode || !sbc_get_codesize) { printf("sbc syms missing\n"); return 1; }
    printf("sbc API ok\n");

    int s = mksock();
    if (s < 0) return 1;
    printf("connected to \\0/org/bluez/audio\n");

    /* --- GET_CAPABILITIES (exact replay of plugin capture) --- */
    unsigned char req[256];
    int n;
    memset(req, 0, sizeof req);
    req[0] = 0x00; req[1] = 0x00; req[2] = 0xab; req[3] = 0x00; /* request, GET_CAPS, len 171 */
    strcpy((char*)req + 22, mac);
    req[169] = 0x01; req[170] = 0x00;
    printf("MYREQ:");
    for (int i = 0; i < 171; i++) printf("%02x", req[i]);
    printf("\n");
    n = send(s, req, 171, 0);
    printf("GET_CAPS sent %d\n", n);
    unsigned char rsp[1024];
    n = read_msg(s, rsp, sizeof rsp);
    dump("caps rsp", rsp, n);
    if (rsp[0] == 0x03) { printf("GET_CAPS ERROR errno=%d\n", rsp[4]); return 1; }

    /* --- BT_OPEN (exact replay; seid=5 lock=2 tail) --- */
    memset(req, 0, sizeof req);
    req[0] = 0x00; req[1] = 0x01; req[2] = 0xaa; req[3] = 0x00;
    strcpy((char*)req + 22, mac);
    req[168] = 0x05; req[169] = 0x02;
    n = send(s, req, 170, 0);
    printf("OPEN sent %d\n", n);
    n = read_msg(s, rsp, sizeof rsp);
    dump("open rsp", rsp, n);
    if (rsp[0] == 0x03) { printf("OPEN ERROR errno=%d\n", rsp[4]); return 1; }

    /* --- BT_SET_CONFIGURATION (verbatim replay, SBC) --- */
    static const unsigned char setcfg[17] =
        {0x00,0x02,0x11,0x00, 0x05,0x00,0x01,0x0d, 0x00,0x00,0x01,0x02, 0x01,0x01,0x01,0x02, 0x26};
    n = send(s, setcfg, 17, 0);
    printf("SET_CONFIG sent %d\n", n);
    n = read_msg(s, rsp, sizeof rsp);
    dump("setcfg rsp", rsp, n);
    if (rsp[0] == 0x03) { printf("SETCFG ERROR errno=%d\n", rsp[4]); return 1; }

    /* --- BT_START_STREAM --- */
    static const unsigned char startm[4] = {0x00,0x04,0x04,0x00};
    n = send(s, startm, 4, 0);
    printf("START sent %d\n", n);
    n = read_msg(s, rsp, sizeof rsp);
    dump("start rsp", rsp, n);
    /* NEW_STREAM notification + fd */
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

    /* clear O_NONBLOCK (bluetoothd passes a nonblocking fd): blocking send
     * gives natural flow control against the earbuds' consumption rate */
    int fl = fcntl(a2dp_fd, F_GETFL);
    if (fl >= 0) fcntl(a2dp_fd, F_SETFL, fl & ~O_NONBLOCK);

    int sndbuf = 16384; /* ~320ms: small enough for timely backpressure */
    setsockopt(a2dp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);

/* REAL bluez sbc.h field values (wrong ones = white noise on the sink) */
#define SBC_FREQ_44100_F 0x02
#define SBC_BLK_16_F     0x03
#define SBC_SB_8_F       0x01
#define SBC_MODE_JOINT_F 0x03
#define SBC_ALLOC_LOUD_F 0x00

    static unsigned char sbcbuf[256];
    static sbc_t * const sbcp = (sbc_t*)sbcbuf;
    memset(sbcbuf, 0, sizeof sbcbuf);
    int r = sbc_init(sbcp, 0);
    sbcp->frequency  = SBC_FREQ_44100_F;
    sbcp->blocks     = SBC_BLK_16_F;
    sbcp->subbands   = SBC_SB_8_F;
    sbcp->mode       = SBC_MODE_JOINT_F;
    sbcp->allocation = SBC_ALLOC_LOUD_F;
    sbcp->bitpool    = 0x26;
    if (sbc_init_primitives) sbc_init_primitives(sbcp);
    printf("sbc_init=%d codesize=%zu framelen=%zu dur=%uus\n", r,
           sbc_get_codesize(sbcp), sbc_get_frame_length(sbcp), sbc_get_frame_duration(sbcp));

    size_t codesize = sbc_get_codesize(sbcp);
    size_t framelen = sbc_get_frame_length(sbcp);
    unsigned dur_us = sbc_get_frame_duration(sbcp);
#define sbc (*sbcp)

    /* sine source: integer LUT (libm sin() is soft-float slow on MIPS) */
    const unsigned rate = 44100;
    static int16_t pcm[1024 * 2 * 8]; /* enough for a few frames */
    static int16_t sinlut[1024];
    {
        int i;
        for (i = 0; i < 1024; i++)
            sinlut[i] = (int16_t)(9000.0 * sin(2.0 * M_PI * (double)i / 1024.0));
    }
    unsigned int phq = 0; /* 16.16 phase */
    unsigned int incq = (unsigned int)((double)freq * 1024.0 / (double)rate * 65536.0 + 0.5);

    /* RTP: 6 frames per packet; payload = 12B RTP + 1B A2DP media header
     * (F/S/L bits zero, low nibble = frame count) + raw SBC frames.
     * Missing that 1-byte header shifts the sink's parser -> white noise! */
    int dumpfd = -1;
    if (getenv("SBC_DUMP")) {
        dumpfd = open(getenv("SBC_DUMP"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        printf("dump -> %s fd=%d\n", getenv("SBC_DUMP"), dumpfd);
    }
    const int NFR = 6;
    unsigned char pkt[12 + 1 + framelen * NFR];
    uint16_t seqn = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0x11223344;
    long total_frames = (long)rate * secs / (codesize / 4);
    long sent = 0;
    long long min_lead = 1000000000LL;
#define LEAD_US 100000LL
    setbuf(stdout, NULL);
    printf("loop: codesize=%zu framelen=%zu total=%ld priv=%p priv2=%p priv3=%p\n",
           codesize, framelen, total_frames, sbc.priv, sbc.priv2, sbc.priv3);
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    while (sent < total_frames) {
        int k = NFR;
        if (k > total_frames - sent) k = total_frames - sent;
        unsigned char *out = pkt + 13;
        pkt[12] = (unsigned char)k; /* A2DP SBC media payload header: frame count */
        for (int f = 0; f < k; f++) {
            int16_t *p = pcm;
            for (size_t i = 0; i < codesize / 4; i++) {
                int16_t v = sinlut[(phq >> 16) & 1023];
                phq += incq;
                *p++ = v; *p++ = v;
            }
            ssize_t w = sbc_encode(&sbc, pcm, codesize, out, framelen);
            if (w < 0) { printf("sbc_encode err %zd\n", w); return 1; }
            out += framelen;
            if (dumpfd >= 0 && sent < 200) write(dumpfd, out - framelen, framelen);
        }
        pkt[0] = 0x80; pkt[1] = 0x60;
        pkt[2] = seqn >> 8; pkt[3] = seqn & 0xff;
        pkt[4] = ts >> 24; pkt[5] = ts >> 16; pkt[6] = ts >> 8; pkt[7] = ts;
        memcpy(pkt + 8, &ssrc, 4);
        ssize_t w = send(a2dp_fd, pkt, 13 + framelen * k, 0);
        if (w < 0) {
            if (errno == EAGAIN) { usleep(2000); continue; }
            perror("send a2dp"); break;
        }
        seqn++; ts += (codesize / 4) * k; sent += k;

        /* Hold the media lead at ~LEAD_US over wall clock: sleep exactly the
         * surplus so we neither underrun (sink dry -> pops) nor overrun
         * (sink buffer overflow -> drops -> pops). Blocking send still gives
         * hard backpressure. */
        gettimeofday(&t1, NULL);
        long long elapsed_us = (long long)(t1.tv_sec - t0.tv_sec) * 1000000LL
                             + (t1.tv_usec - t0.tv_usec);
        long long media_us = (long long)sent * (codesize / 4) * 1000000LL / rate;
        long long lead = media_us - elapsed_us;
        if (lead > LEAD_US) usleep((useconds_t)(lead - LEAD_US));
        if (lead < min_lead) min_lead = lead;
    }
    gettimeofday(&t1, NULL);
    long long total_us = (long long)(t1.tv_sec - t0.tv_sec) * 1000000LL
                       + (t1.tv_usec - t0.tv_usec);
    printf("done: %ld frames (%.1fs audio) in %.2fs wall, min_lead=%.0fms\n", sent,
           (double)sent * (codesize/4) / rate, (double)total_us / 1e6,
           (double)min_lead / 1000.0);
    close(a2dp_fd);
    close(s);
    return 0;
}
