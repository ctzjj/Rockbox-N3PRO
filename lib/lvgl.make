#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__  _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/

# Build rules for the vendored LVGL submodule (lib/lvgl). This file lives
# in the parent repo because the whole lib/lvgl directory belongs to the
# submodule checkout; lv_conf.h is parent-tracked in apps/gui/lvglui/.

LVGL_DIR := $(ROOTDIR)/lib/lvgl
LVGL_SRC := $(wildcard $(LVGL_DIR)/src/core/*.c) \
            $(wildcard $(LVGL_DIR)/src/draw/*.c) \
            $(wildcard $(LVGL_DIR)/src/draw/sw/*.c) \
            $(wildcard $(LVGL_DIR)/src/font/*.c) \
            $(wildcard $(LVGL_DIR)/src/hal/*.c) \
            $(wildcard $(LVGL_DIR)/src/misc/*.c) \
            $(wildcard $(LVGL_DIR)/src/widgets/*.c) \
            $(wildcard $(LVGL_DIR)/src/extra/*.c) \
            $(wildcard $(LVGL_DIR)/src/extra/layouts/*/*.c) \
            $(wildcard $(LVGL_DIR)/src/extra/themes/*/*.c) \
            $(wildcard $(LVGL_DIR)/src/extra/widgets/*/*.c)
LVGL_OBJ := $(call c2obj, $(LVGL_SRC))
LVGL := $(BUILDDIR)/lib/liblvgl.a

OTHER_SRC += $(LVGL_SRC)
INCLUDES += -I$(LVGL_DIR) -I$(ROOTDIR)/apps/gui/lvglui

LVGLFLAGS = $(CFLAGS) $(SHARED_CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE

$(BUILDDIR)/lib/lvgl/src/%.o: $(LVGL_DIR)/src/%.c $(ROOTDIR)/apps/gui/lvglui/lv_conf.h
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) -c $< -o $@ $(LVGLFLAGS)

$(LVGL): $(LVGL_OBJ)
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

CORE_LIBS := $(LVGL) $(CORE_LIBS)
