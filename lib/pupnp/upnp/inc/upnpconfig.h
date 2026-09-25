/* upnpconfig.h -- hand-written substitute for the configure-generated
 * public configuration header of pupnp 1.6.21 (Rockbox N3Pro hosted
 * build: client+device, webserver, ssdp/soap/gena/tools enabled, no
 * debug, no ipv6, no OpenSSL). */

#ifndef UPNP_CONFIG_H
#define UPNP_CONFIG_H

/** The library version (string) */
#define UPNP_VERSION_STRING "1.6.21"

#define UPNP_VERSION_MAJOR 1
#define UPNP_VERSION_MINOR 6
#define UPNP_VERSION_PATCH 21

/** The library version (numeric) e.g. 10300 means version 1.3.0 */
#define UPNP_VERSION	\
  ((UPNP_VERSION_MAJOR * 100 + UPNP_VERSION_MINOR) * 100 + UPNP_VERSION_PATCH)

#define UPNP_HAVE_CLIENT 1
#define UPNP_HAVE_DEVICE 1
#define UPNP_HAVE_WEBSERVER 1
#define UPNP_HAVE_SSDP 1
#define UPNP_HAVE_OPTSSDP 0
#define UPNP_HAVE_SOAP 1
#define UPNP_HAVE_GENA 1
#define UPNP_HAVE_TOOLS 1
#define UPNP_HAVE_DEBUG 0
#define UPNP_ENABLE_IPV6 0

#endif /* UPNP_CONFIG_H */
