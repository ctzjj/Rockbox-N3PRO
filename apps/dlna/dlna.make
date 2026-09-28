#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# apps/dlna - DLNA renderer (UPnP MediaRenderer) on the netfm pipeline.
# Control plane: vendored GMediaRender v0.0.8 (LGPL-2.1, gmr/).
# Audio backend: dlna_stream.c -> netfm_stream_*.

DLNALIB_DIR = $(ROOTDIR)/apps/dlna
DLNALIB_SRC := $(call preprocess, $(DLNALIB_DIR)/SOURCES)
DLNALIB_OBJ := $(call c2obj, $(DLNALIB_SRC))

DLNALIB = $(BUILDDIR)/lib/libdlna.a

# libdlna references pupnp and the netfm objects, so it must be linked
# before libpupnp.a; CORE_LIBS is prepended accordingly below.
DLNAFLAGS = -I$(DLNALIB_DIR) \
            -I$(DLNALIB_DIR)/gmr \
            -I$(DLNALIB_DIR)/glib_lite \
            -I$(ROOTDIR)/lib/pupnp/gmr-include \
            -I$(ROOTDIR)/lib/pupnp/upnp/inc \
            -I$(ROOTDIR)/lib/pupnp/ixml/inc \
            -I$(ROOTDIR)/lib/pupnp/threadutil/inc \
            -DHAVE_DLNA_OUTPUT \
            -DPACKAGE_NAME='"GMediaRender"' \
            -DPACKAGE_STRING='"GMediaRender 0.0.8"' \
            -DPKG_DATADIR='"/.rockbox/dlna"'

# Third-party sources (GMediaRender) only: keep Rockbox's own
# warnings visible.
DLNAWARN = -Wno-missing-field-initializers -Wno-unused-parameter -Wno-undef \
           -Wno-strict-prototypes

OTHER_SRC += $(DLNALIB_SRC)
CORE_LIBS := $(DLNALIB) $(CORE_LIBS)

# The include paths and -DHAVE_DLNA_OUTPUT must reach these sources no matter
# which object rule make ends up picking for them (the dedicated pattern rule
# below is not always preferred over the generic $(BUILDDIR)/%.o one).  Use a
# target-specific variable so the GMediaRender PACKAGE_* defines stay out of
# the global CFLAGS (they clash with libspeex's config-speex.h).
CFLAGS += $(DLNAFLAGS)

$(DLNALIB): $(DLNALIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

$(BUILDDIR)/apps/dlna/%.o: $(ROOTDIR)/apps/dlna/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<)) \
		$(CC) $(CFLAGS) $(DLNAWARN) -c $< -o $@
