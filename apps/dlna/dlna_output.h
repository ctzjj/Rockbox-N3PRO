/* dlna_output.h - GMediaRender output-module backend for Rockbox.
 *
 * This is the glue between the unmodified upstream GMediaRender core
 * (apps/dlna/gmr/, output_module.h interface) and the DLNA stream
 * engine (apps/dlna/dlna_stream.c): it maps AVTransport/Rendering
 * control calls onto the engine and the Rockbox master volume. */
#ifndef DLNA_OUTPUT_H
#define DLNA_OUTPUT_H

struct output_module;

/* the module registered under the shortname "dlna" (gmr/output.c) */
extern struct output_module dlna_stream_output;

/* full teardown: stop the stream, end and join the monitor thread so
 * no worker survives the DLNA screen */
void dlna_output_shutdown(void);

#endif /* DLNA_OUTPUT_H */
