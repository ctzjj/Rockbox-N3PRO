/***************************************************************************
 * One-shot NTP time sync: started once the WiFi connection has an IP.
 * The thread tries an Aliyun NTP server up to three times and exits
 * in every case - on success after setting the system clock, on
 * failure without side effects.  Nothing stays resident.
 ****************************************************************************/
#ifndef NET_TIME_H
#define NET_TIME_H

void net_time_sync_start(void);

#endif
