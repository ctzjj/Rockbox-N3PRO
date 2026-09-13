/* Vendored sbc encoder test: dump N SBC frames of 440Hz sine.
 * Links against the local bluez sbc sources (no dlopen). */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "sbc.h"

int main(int argc, char **argv)
{
    if (argc < 7) { printf("usage: %s mode blocks subbands bitpool nframes out\n", argv[0]); return 1; }
    int mode = atoi(argv[1]), blocks = atoi(argv[2]), subs = atoi(argv[3]);
    int bp = atoi(argv[4]), nf = atoi(argv[5]);
    const char *outpath = argv[6];

    sbc_t sbc;
    if (sbc_init(&sbc, 0) < 0) { printf("init fail\n"); return 1; }
    sbc.frequency  = SBC_FREQ_44100;
    sbc.blocks     = (uint8_t)blocks;
    sbc.subbands   = (uint8_t)subs;
    sbc.mode       = (uint8_t)mode;
    sbc.allocation = SBC_AM_LOUDNESS;
    sbc.bitpool    = (uint8_t)bp;
    sbc.endian     = SBC_LE;

    size_t cs = sbc_get_codesize(&sbc);
    size_t fl = sbc_get_frame_length(&sbc);
    printf("mode=%d blk=%d sub=%d bp=%d: codesize=%zu framelen=%zu\n", mode, blocks, subs, bp, cs, fl);

    int fd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    char ppath[256];
    snprintf(ppath, sizeof ppath, "%s.pcm", outpath);
    int pfd = open(ppath, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    static int16_t pcm[8192];
    static unsigned char enc[2048];
    double ph = 0, inc = 2.0*M_PI*440.0/44100;
    for (int f = 0; f < nf; f++) {
        for (size_t i = 0; i < cs/2; i += 2) {
            int16_t v = (int16_t)(9000.0*sin(ph));
            ph += inc; if (ph > 2*M_PI) ph -= 2*M_PI;
            pcm[i] = v; pcm[i+1] = v;
        }
        ssize_t written = 0;
        if (pfd >= 0) write(pfd, pcm, cs);
        ssize_t r = sbc_encode(&sbc, pcm, cs, enc, fl, &written);
        if (r != (ssize_t)cs || written != (ssize_t)fl) { printf("enc=%zd written=%zd\n", r, written); break; }
        if (write(fd, enc, fl) < 0) { perror("write"); break; }
    }
    close(fd);
    if (pfd >= 0) close(pfd);
    printf("dumped %s\n", outpath);
    return 0;
}
