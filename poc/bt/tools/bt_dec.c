/* Decode a raw SBC frame dump to WAV via the vendored decoder. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "sbc.h"

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s in.sbc out.raw\n", argv[0]); return 1; }
    int in = open(argv[1], O_RDONLY);
    if (in < 0) { perror("open-in"); return 1; }
    int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { perror("open-out"); return 1; }

    sbc_t sbc;
    sbc_init(&sbc, 0);
    static unsigned char buf[65536];
    static unsigned char pcm[65536];
    size_t have = 0;
    long frames = 0;
    for (;;) {
        if (have < 4096) {
            ssize_t r = read(in, buf + have, sizeof buf - have);
            if (r <= 0) break;
            have += r;
        }
        size_t written = 0;
        ssize_t consumed = sbc_decode(&sbc, buf, have, pcm, sizeof pcm, &written);
        if (consumed <= 0) { printf("decode err %zd\n", consumed); break; }
        if (write(out, pcm, written) < 0) { perror("write"); break; }
        memmove(buf, buf + consumed, have - consumed);
        have -= consumed;
        frames++;
    }
    printf("decoded %ld frames\n", frames);
    close(in); close(out);
    return 0;
}
