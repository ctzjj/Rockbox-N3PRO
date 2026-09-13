/* Fake sys_client responder: reply to everything, log traffic.
 * argv[1] = reply string (default "OK") */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
    const char *reply = argc > 1 ? argv[1] : "OK";
    int rlen = strlen(reply);

    unlink("/var/run/sys_client");
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "/var/run/sys_client");
    if (bind(s, (void*)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(s, 4) < 0) { perror("listen"); return 1; }

    int log = open("/tmp/syscli.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dprintf(log, "responder up, reply=\"%s\"\n", reply);

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        for (;;) {
            char buf[512];
            ssize_t n = read(c, buf, sizeof buf - 1);
            if (n <= 0) break;
            buf[n] = 0;
            dprintf(log, "RECV(%zd): %s\n", n, buf);
            ssize_t w = write(c, reply, rlen);
            dprintf(log, "SENT(%zd): %s\n", w, reply);
            fsync(log);
        }
        close(c);
        dprintf(log, "-- client disconnected --\n");
    }
    return 0;
}
