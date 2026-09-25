/* glib.h - Minimal glib shim for the vendored GMediaRender sources.
 *
 * The vendored control plane (apps/dlna/gmr) is compiled WITHOUT the
 * GStreamer output module and WITHOUT main(), which were the only real
 * glib users in v0.0.8.  What is left needs exactly the types and the
 * GMainLoop trio below.  This file is found first through
 * -Iapps/dlna/glib_lite and thus shadows a system <glib.h> (there is
 * none in the Rockbox toolchain anyway).
 *
 * Not part of upstream GMediaRender; see apps/dlna/README.rockbox.
 */
#ifndef _GLIB_LITE_H
#define _GLIB_LITE_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef int gint;
typedef char gchar;
typedef int gboolean;
#define TRUE  1
#define FALSE 0
typedef void *gpointer;
typedef const void *gconstpointer;
typedef long long gint64;
typedef unsigned long long guint64;

/* Opaque: only passed around, never dereferenced (the only parser
 * user was main(), which is not compiled). */
typedef struct _GOptionContext GOptionContext;

typedef struct GError
{
    int code;
    char *message;
} GError;

static inline void g_error_free(GError *e) { (void)e; }
static inline void g_free(void *p) { free(p); }
static inline void *g_malloc(size_t n) { return malloc(n); }

/* output_loop() is never called from Rockbox (libupnp runs its own
 * threads and the Rockbox monitor thread drives the backend), but the
 * function must exist and link. */
typedef struct GMainLoop
{
    volatile int running;
} GMainLoop;

static inline GMainLoop *g_main_loop_new(void *context, gboolean is_running)
{
    (void)context; (void)is_running;
    return (GMainLoop *)calloc(1, sizeof(GMainLoop));
}

static inline void g_main_loop_run(GMainLoop *loop)
{
    if (loop == NULL)
        return;
    loop->running = TRUE;
    while (loop->running)
        sleep(1);
}

static inline void g_main_loop_quit(GMainLoop *loop)
{
    if (loop != NULL)
        loop->running = FALSE;
}

#endif /* _GLIB_LITE_H */
