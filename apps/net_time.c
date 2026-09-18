/***************************************************************************
 * One-shot NTP time sync: started once the WiFi connection has an IP.
 * The thread tries an Aliyun NTP server up to three times and exits
 * in every case - on success after setting the system clock (the
 * process runs as root), on failure without side effects.  Nothing
 * stays resident.
 ****************************************************************************/
#include "config.h"

#ifdef HAVE_WIFI_MENU

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "net_time.h"

#define NTP_HOST     "ntp.aliyun.com"
#define NTP_FALLBACK "203.107.6.88"    /* Aliyun public NTP anycast */
#define NTP_PORT     123
#define NTP_TRIES    3
#define NTP_TIMEOUT  3                 /* seconds per reply */

static volatile bool net_time_running = false;

/* One query attempt: resolve, ask, parse the transmit timestamp and
 * set the system clock.  Returns true on success. */
static bool net_time_once(void)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NTP_PORT);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(NTP_HOST, NULL, &hints, &res) == 0 && res)
    {
        memcpy(&addr, res->ai_addr, sizeof(addr));
        addr.sin_port = htons(NTP_PORT);
        freeaddrinfo(res);
    }
    else if (inet_aton(NTP_FALLBACK, &addr.sin_addr) == 0)
        return false;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;

    struct timeval tv = { NTP_TIMEOUT, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char pkt[48] = { 0 };
    pkt[0] = 0x1B;                    /* LI=0, VN=3, client mode */

    bool ok = false;
    if (sendto(fd, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&addr, sizeof(addr)) > 0)
    {
        unsigned char rep[48];
        if (recv(fd, rep, sizeof(rep), 0) == (ssize_t)sizeof(rep))
        {
            /* transmit timestamp, 32-bit seconds since 1900 */
            unsigned long secs = ((unsigned long)rep[40] << 24) |
                                 ((unsigned long)rep[41] << 16) |
                                 ((unsigned long)rep[42] << 8) |
                                 (unsigned long)rep[43];
            time_t epoch = (time_t)(secs - 2208988800UL);
            struct tm tm_now;
            if (epoch > (time_t)1600000000 &&      /* sanity: > 2020 */
                gmtime_r(&epoch, &tm_now) && tm_now.tm_year + 1900 > 2020)
            {
                struct timespec ts = { epoch, 0 };
                ok = clock_settime(CLOCK_REALTIME, &ts) == 0;
            }
        }
    }

    close(fd);
    return ok;
}

static void *net_time_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < NTP_TRIES; i++)
    {
        if (net_time_once())
            break;                    /* clock set - done */
        sleep(1);
    }
    net_time_running = false;
    return NULL;
}

void net_time_sync_start(void)
{
    if (net_time_running)
        return;
    net_time_running = true;

    pthread_t pt;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 32 * 1024);
    if (pthread_create(&pt, &attr, net_time_thread, NULL) != 0)
        net_time_running = false;
    pthread_attr_destroy(&attr);
}

#endif /* HAVE_WIFI_MENU */
