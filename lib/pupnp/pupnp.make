#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# libupnp (pupnp) 1.6.21 - Portable SDK for UPnP devices (BSD-3)
# Vendored from release-1.6.21 with the autotools layer stripped;
# configuration is provided by autoconfig.h / upnp/inc/upnpconfig.h
# in this directory (hand-written, see README.rockbox).

PUPNPLIB_DIR = $(ROOTDIR)/lib/pupnp
PUPNPLIB_SRC = $(call preprocess, $(PUPNPLIB_DIR)/SOURCES)
PUPNPLIB_OBJ := $(call c2obj, $(PUPNPLIB_SRC))

PUPNPLIB = $(BUILDDIR)/lib/libpupnp.a

PUPNPFLAGS = -I$(PUPNPLIB_DIR) \
             -I$(PUPNPLIB_DIR)/upnp/inc \
             -I$(PUPNPLIB_DIR)/upnp/src/inc \
             -I$(PUPNPLIB_DIR)/ixml/inc \
             -I$(PUPNPLIB_DIR)/ixml/src/inc \
             -I$(PUPNPLIB_DIR)/threadutil/inc \
             -I$(PUPNPLIB_DIR)/threadutil/src/inc \
             -O2 -DNDEBUG -DNO_DEBUG

OTHER_SRC += $(PUPNPLIB_SRC)
CORE_LIBS += $(PUPNPLIB)

$(PUPNPLIB): $(PUPNPLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

$(BUILDDIR)/lib/pupnp/%.o: $(ROOTDIR)/lib/pupnp/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<)) \
		$(CC) $(PUPNPFLAGS) -c $< -o $@

# The ixml sources include ixmlparser.h / ixmlmembuf.h by bare name; the
# build dependency machinery maps such headers into $(BUILDDIR), so provide
# them there (the real files live in ixml/src/inc).
$(BUILDDIR)/ixmlparser.h: $(PUPNPLIB_DIR)/ixml/src/inc/ixmlparser.h
	$(SILENT)cp $< $@
$(BUILDDIR)/ixmlmembuf.h: $(PUPNPLIB_DIR)/ixml/src/inc/ixmlmembuf.h
	$(SILENT)cp $< $@

# The pupnp sources include their own headers by bare name (ssdplib.h,
# httpparser.h, ...); the build dependency machinery maps those into
# $(BUILDDIR), so provide them there from wherever they live in the tree.
$(BUILDDIR)/%.h: $(PUPNPLIB_DIR)/upnp/src/inc/%.h
	$(SILENT)cp $< $@
$(BUILDDIR)/%.h: $(PUPNPLIB_DIR)/ixml/src/inc/%.h
	$(SILENT)cp $< $@
$(BUILDDIR)/%.h: $(PUPNPLIB_DIR)/threadutil/src/inc/%.h
	$(SILENT)cp $< $@
$(BUILDDIR)/%.h: $(PUPNPLIB_DIR)/%.h
	$(SILENT)cp $< $@
