/* dlna.h - DLNA renderer (UPnP MediaRenderer) lifecycle, entry from the
 * WiFi menu. */
#ifndef DLNA_H
#define DLNA_H

#include <stdbool.h>

/* Bring up the renderer: output module, UPnP device (SSDP + SOAP on
 * port 49494), AVTransport + RenderingControl + ConnectionManager.
 * Returns true on success. Safe to call when already running. */
bool dlna_renderer_start(void);

/* Tear the renderer down (sends SSDP byebye), stop the stream and join
 * every worker thread.  Safe to call when not running. */
void dlna_renderer_stop(void);

bool dlna_renderer_active(void);

/* the renderer's main menu (status rows + the exit row) */
int dlna_menu(void);

const char *dlna_renderer_friendly_name(void);

#endif
