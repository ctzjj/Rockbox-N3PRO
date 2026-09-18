/***************************************************************************
 * Generic Web remote control framework.
 *
 * Runs a small HTTP + WebSocket server while active; the browser page
 * (served from RAM) shows title / cover / progress and offers play,
 * pause, prev, next, seek and volume.  Access is guarded by a short
 * numeric code shown on the device screen.
 *
 * Core playback APIs are only touched from the cooperative Rockbox
 * worker thread; network threads exchange data through a snapshot
 * and a command queue.
 ****************************************************************************/
#ifndef _WEB_CONTROL_H_
#define _WEB_CONTROL_H_

#ifdef HAVE_WEB_CONTROL

/* Modal-ish screen: starts the server (if not running), shows the
 * access code and address; Back leaves the server running. */
void web_control_screen(void);

/* Stop the server and the worker (safe to call when not running). */
void web_control_stop(void);

bool web_control_running(void);

#endif /* HAVE_WEB_CONTROL */
#endif /* _WEB_CONTROL_H_ */
