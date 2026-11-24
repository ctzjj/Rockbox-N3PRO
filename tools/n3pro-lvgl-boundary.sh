#!/bin/sh
# tools/n3pro-lvgl-boundary.sh -- check the LVGL UI backend boundary
# (docs/lvgl-ui.md rule 1 / AGENTS.md golden rule 7).
#
# The screen files must not include Rockbox core headers: everything they
# need from the engine goes through ui_backend.h (be_*) and lvgl_ui.h
# (shell_/lvgui_/ui_*).  Allowed for screens:
#
#   lvgl.h, lvgl_ui.h, ui_theme.h, ui_widgets.h, ui_backend.h
#   config.h (compile-time switches), lang.h (str(LANG_*)),
#   string-extra.h (header-only libc extensions)
#   anything in angle brackets (libc / system headers)
#
# Backend-side files, which MAY include core headers, are everything else
# under apps/gui/lvglui/: lvgl_ui.c, ui_backend.c, ui_stock.c (the
# stock-UI bridge), ui_compat.c, ui_charset.c, png_decode.c,
# ime_pinyin.c, lvgl_port_disp.c, lvgl_port_input.c.
#
# Usage: sh tools/n3pro-lvgl-boundary.sh   (from the repository root)
# Exits non-zero, printing file:line for every violation.

LC_ALL=C
L=apps/gui/lvglui
SCREENS="$L/ui_shell.c $L/ui_library.c $L/ui_player.c $L/ui_control.c \
$L/ui_theme.c $L/ui_widgets.c $L/ui_icons.c"

fail=0
for f in $SCREENS; do
    if [ ! -f "$f" ]; then
        echo "boundary: missing screen file $f"
        fail=1
        continue
    fi
    violations=$(grep -n '#include "' "$f" |
        grep -vE '"(lvgl|lvgl_ui|ui_theme|ui_widgets|ui_backend|config|lang|string-extra)\.h"')
    if [ -n "$violations" ]; then
        echo "boundary violations in $f:"
        echo "$violations"
        fail=1
    fi
done

if [ "$fail" = "0" ]; then
    echo "lvgl-ui boundary OK: screens include only lvgl/ui/config/lang/libc headers"
fi
exit "$fail"
