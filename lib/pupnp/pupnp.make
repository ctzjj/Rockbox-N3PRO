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
