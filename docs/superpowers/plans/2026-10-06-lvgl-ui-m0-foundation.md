# LVGL UI M0 — Foundation (build wiring + fb/input port + smoke screen)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** LVGL v8.4 compiles inside the rockbox app build, renders through the existing framebuffer path, receives touch from the button queue, and boots to a smoke screen on the N3Pro — with `HAVE_LVGL_UI` undefined producing a byte-identical classic build.

**Architecture:** New port layer `apps/gui/lvglui/` (5 files). LVGL archive built from the vendored submodule via a `lvgl.make` following the `apps/dlna/dlna.make` pattern (`CORE_LIBS` prepend). GUI entry swapped at `apps/main.c:247` behind `HAVE_LVGL_UI`.

**Tech Stack:** rockbox app build (`build-n3pro`, mipsel-rockbox-linux-gnu toolchain), LVGL v8.4.0 submodule (already staged at `lib/lvgl`).

**Conventions:** all shell commands run in WSL Debian; repo root `/home/ctzjj/rockbox`; build dir `build-n3pro`. Toolchain prefix `/home/ctzjj/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-`.

---

### Task 1: LVGL config header

**Files:**
- Create: `lib/lvgl/lv_conf.h`

- [ ] **Step 1: Write `lib/lvgl/lv_conf.h`**

```c
#ifndef LV_CONF_H
#define LV_CONF_H
/* Minimal LVGL v8.4 config for the Rockbox N3Pro port. */

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Time base: rockbox ticks -> milliseconds. */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "kernel.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)((long long)current_tick * 1000 / HZ))

/* Hosted linux app: use system heap. */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>

#define LV_DISP_DEF_REFR_PERIOD 16
#define LV_INDEV_DEF_READ_PERIOD 20

/* Fonts: built-in latin only for M0; CJK fonts arrive in M1. */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_DEFERRED_PATH_DRAW 0

/* Widgets needed at M0 (grow per milestone). */
#define LV_USE_OBJ        1 /* implicit */
#define LV_USE_LABEL      1
#define LV_USE_BTN        1
#define LV_USE_ARC        0
#define LV_USE_IMG        0
#define LV_USE_LIST       0
#define LV_USE_SLIDER     0
#define LV_USE_SWITCH     0
#define LV_USE_TEXTAREA   0

/* Everything else default-off via the guard below. */
#define LV_USE_ANIMIMA   0
#define LV_USE_FLEX 1
#define LV_USE_SNAPSHOT 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#endif /* LV_CONF_H */
```

Note: after the first build, run `make` and let the compiler list any additionally required `LV_USE_*` flags (v8.4 warns with exact names); enable only what is requested.

- [ ] **Step 2: Verify header parses**

Run: `wsl -d Debian -- bash -lc "cd /home/ctzjj/rockbox && gcc -fsyntax-only -x c - <<<'#include \"lib/lvgl/lv_conf.h\"' && echo OK"`
Expected: `OK`

---

### Task 2: Build wiring (`lvgl.make` + `tools/root.make`)

**Files:**
- Create: `lib/lvgl/lvgl.make`
- Modify: `tools/root.make` (after line 93, next to `libtlsf.make`)

- [ ] **Step 1: Write `lib/lvgl/lvgl.make`** (pattern copied from `apps/dlna/dlna.make` + `lib/tlsf/libtlsf.make`)

```make
LVGL_DIR := $(ROOTDIR)/lib/lvgl
LVGL_SRC := $(wildcard $(LVGL_DIR)/src/core/*.c) \
            $(wildcard $(LVGL_DIR)/src/draw/*.c) \
            $(wildcard $(LVGL_DIR)/src/font/*.c) \
            $(wildcard $(LVGL_DIR)/src/hal/*.c) \
            $(wildcard $(LVGL_DIR)/src/misc/*.c) \
            $(wildcard $(LVGL_DIR)/src/widgets/*.c)
LVGL_OBJ := $(call c2obj, $(LVGL_SRC))
LVGL := $(BUILDDIR)/lib/liblvgl.a

OTHER_SRC += $(LVGL_SRC)
INCLUDES += -I$(LVGL_DIR)

LVGLFLAGS = $(CFLAGS) $(SHARED_CFLAGS) -DLV_CONF_INCLUDE_SIMPLE

$(BUILDDIR)/lib/lvgl/src/%.o: $(LVGL_DIR)/src/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) -c $< -o $@ $(LVGLFLAGS)

$(LVGL): $(LVGL_OBJ)
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

CORE_LIBS := $(LVGL) $(CORE_LIBS)
```

- [ ] **Step 2: Add the include to `tools/root.make`**

Insert directly after line 93 (`include $(ROOTDIR)/lib/tlsf/libtlsf.make`), same indentation:

```make
include $(ROOTDIR)/lib/lvgl/lvgl.make
```

- [ ] **Step 3: Compile the archive alone**

Run: `wsl -d Debian -- bash -lc "cd /home/ctzjj/rockbox/build-n3pro && make /home/ctzjj/rockbox/build-n3pro/lib/liblvgl.a 2>&1 | tail -20"`
Expected: ends with `AR liblvgl.a`; no errors. If warnings demand extra `LV_USE_*` or `LV_FONT_*` flags, add them to `lv_conf.h` and re-run until clean.

- [ ] **Step 4: Commit**

```bash
git add lib/lvgl/lv_conf.h lib/lvgl/lvgl.make tools/root.make .gitmodules lib/lvgl
git commit -m "lvgl: vendor LVGL v8.4 and wire it into the app build"
```

---

### Task 3: Port layer — display

**Files:**
- Create: `apps/gui/lvglui/lvgl_ui.h`
- Create: `apps/gui/lvglui/lvgl_port_disp.c`

- [ ] **Step 1: Write `apps/gui/lvglui/lvgl_ui.h`**

```c
#ifndef LVGL_UI_H
#define LVGL_UI_H
#ifdef HAVE_LVGL_UI

void lvgui_main(void);            /* replaces root_menu() in apps/main.c */
void lvgui_port_disp_init(void);
void lvgui_port_input_init(void);
void lvgui_input_feed(int button, intptr_t data);
void lvgui_scr_smoke_create(void);

#endif /* HAVE_LVGL_UI */
#endif /* LVGL_UI_H */
```

- [ ] **Step 2: Write `apps/gui/lvglui/lvgl_port_disp.c`**

```c
#include "lvgl.h"
#include <string.h>
#include "lcd.h"
#include "lvgl_ui.h"

/* LVGL renders RGB565 exactly like rockbox's framebuffer (LCD_DEPTH 16,
 * RGB565, same byte order on little-endian mipsel): flush = row memcpy +
 * the existing beam-chasing lcd_update_rect(). */

#define SMOKE_HOR 360
#define SMOKE_VER 480
#define DRAW_BUF_LINES 40

static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t g_buf1[SMOKE_HOR * DRAW_BUF_LINES];
static lv_disp_drv_t g_disp_drv;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    int w = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; y++)
    {
        memcpy(FBADDR(area->x1, y), color_p, w * sizeof(lv_color_t));
        color_p += w;
    }
    lcd_update_rect(area->x1, area->y1, w, area->y2 - area->y1 + 1);
    lv_disp_flush_ready(drv);
    (void)drv;
}

void lvgui_port_disp_init(void)
{
    lv_disp_draw_buf_init(&g_draw_buf, g_buf1, NULL, SMOKE_HOR * DRAW_BUF_LINES);
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.hor_res = SMOKE_HOR;
    g_disp_drv.ver_res = SMOKE_VER;
    g_disp_drv.draw_buf = &g_draw_buf;
    g_disp_drv.flush_cb = flush_cb;
    lv_disp_drv_register(&g_disp_drv);
}
```

- [ ] **Step 3: Commit**

```bash
git add apps/gui/lvglui/
git commit -m "lvgl: display port flushing into the rockbox framebuffer"
```

---

### Task 4: Port layer — input

**Files:**
- Create: `apps/gui/lvglui/lvgl_port_input.c`

- [ ] **Step 1: Write `apps/gui/lvglui/lvgl_port_input.c`**

```c
#include "lvgl.h"
#include "button.h"
#include "lvgl_ui.h"

/* One input pipeline: the GUI thread drains the rockbox button queue and
 * feeds LVGL here. Touch coordinates ride the button data word as
 * (x << 16) | y (HAVE_BUTTON_DATA). */

static lv_indev_drv_t g_indev_pointer;
static lv_indev_data_t g_last_touch;

void lvgui_input_feed(int button, intptr_t data)
{
    if (button & BUTTON_TOUCH)
    {
        g_last_touch.point.x = (data >> 16) & 0xffff;
        g_last_touch.point.y = data & 0xffff;
        g_last_touch.state = (button & BUTTON_REL)
                             ? LV_INDEV_STATE_RELEASED
                             : LV_INDEV_STATE_PRESSED;
    }
}

static void pointer_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    *data = g_last_touch;   /* keep last point when released */
    (void)drv;
}

void lvgui_port_input_init(void)
{
    lv_indev_drv_init(&g_indev_pointer);
    g_indev_pointer.type = LV_INDEV_TYPE_POINTER;
    g_indev_pointer.read_cb = pointer_read_cb;
    lv_indev_drv_register(&g_indev_pointer);
}
```

- [ ] **Step 2: Commit**

```bash
git add apps/gui/lvglui/lvgl_port_input.c
git commit -m "lvgl: pointer input fed from the rockbox button queue"
```

---

### Task 5: Smoke screen + main loop + entry hook

**Files:**
- Create: `apps/gui/lvglui/lvgl_scr_smoke.c`
- Create: `apps/gui/lvglui/lvgl_ui.c`
- Modify: `apps/main.c:247`
- Modify: `firmware/export/config/n3pro.h` (inside `#ifndef BOOTLOADER`)

- [ ] **Step 1: Write `apps/gui/lvglui/lvgl_scr_smoke.c`**

```c
#include "lvgl.h"
#include "lvgl_ui.h"
#include "version.h"

static lv_obj_t *g_counter_label;
static lv_obj_t *g_touch_label;
static int g_count;

static void btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        g_count++;
        lv_label_set_text_fmt(g_counter_label, "taps: %d", g_count);
    }
}

static void screen_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED)
    {
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_label_set_text_fmt(g_touch_label, "touch: %d,%d", p.x, p.y);
    }
}

void lvgui_scr_smoke_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x171a24), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x3a1f4e), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "LVGL on N3Pro\nrockbox %s", ROCKBOX_REPO_URL ? "" : "");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    g_counter_label = lv_label_create(scr);
    lv_label_set_text(g_counter_label, "taps: 0");
    lv_obj_set_style_text_color(g_counter_label, lv_color_hex(0x3df3a5), 0);
    lv_obj_align(g_counter_label, LV_ALIGN_TOP_MID, 0, 100);

    g_touch_label = lv_label_create(scr);
    lv_label_set_text(g_touch_label, "touch: -");
    lv_obj_set_style_text_color(g_touch_label, lv_color_hex(0xffd97d), 0);
    lv_obj_align(g_touch_label, LV_ALIGN_TOP_MID, 0, 130);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "tap me");
    lv_obj_center(btn_label);
}
```

(If `version.h` lacks `ROCKBOX_REPO_URL`, drop the second format line and use a plain `lv_label_set_text(title, "LVGL on N3Pro");`.)

- [ ] **Step 2: Write `apps/gui/lvglui/lvgl_ui.c`**

```c
#include "lvgl.h"
#include "kernel.h"
#include "button.h"
#include "lvgl_ui.h"

void lvgui_main(void)
{
    lv_init();
    lvgui_port_disp_init();
    lvgui_port_input_init();
    lvgui_scr_smoke_create();

    while (1)
    {
        int button = button_get_w_tmo(HZ / 20);
        if (button)
            lvgui_input_feed(button, button_get_data());
        lv_timer_handler();
    }
}
```

- [ ] **Step 3: Hook the entry in `apps/main.c`**

Change line 246-247 (`CHART(">root_menu"); root_menu();`) to:

```c
    CHART(">root_menu");
#ifdef HAVE_LVGL_UI
    lvgui_main();
#else
    root_menu();
#endif
```

and add near the existing includes:

```c
#ifdef HAVE_LVGL_UI
#include "gui/lvglui/lvgl_ui.h"
#endif
```

- [ ] **Step 4: Enable the define in `firmware/export/config/n3pro.h`** (inside the existing `#ifndef BOOTLOADER` block, after `HAVE_KBD_POINT_MODE`):

```c
/* Modern touch UI (LVGL) replacing the classic menu tree. */
#define HAVE_LVGL_UI 1
```

- [ ] **Step 5: Add sources to `apps/SOURCES`** (after `menu.c` line):

```
#ifdef HAVE_LVGL_UI
gui/lvglui/lvgl_ui.c
gui/lvglui/lvgl_port_disp.c
gui/lvglui/lvgl_port_input.c
gui/lvglui/lvgl_scr_smoke.c
#endif
```

- [ ] **Step 6: Full build**

Run: `wsl -d Debian -- bash -lc "cd /home/ctzjj/rockbox/build-n3pro && make 2>&1 | tail -25"`
Expected: links `rockbox.elf` with no undefined references to `lv_*`. Common fixes: missing `LV_USE_*` flags (compiler names them); `FBADDR` include (add `#include "lcd-target.h"` if `lcd.h` alone is not enough on this target).

- [ ] **Step 7: Commit**

```bash
git add apps/gui/lvglui/lvgl_scr_smoke.c apps/gui/lvglui/lvgl_ui.c apps/main.c firmware/export/config/n3pro.h apps/SOURCES
git commit -m "lvgl: smoke screen as the GUI entry behind HAVE_LVGL_UI"
```

---

### Task 6: On-device verification

- [ ] **Step 1: Confirm deploy path**

Run: `wsl -d Debian -- bash -lc "head -40 /home/ctzjj/n3pro/build_and_pack.sh"`
Identify how `rockbox.n3pro` reaches the device (expected: package into update payload or adb push to `/.rockbox/`).

- [ ] **Step 2: Deploy and boot**

Use the script from step 1 (or the established adb/ssh path) and reboot the N3Pro.
Expected: dark gradient smoke screen with "LVGL on N3Pro".

- [ ] **Step 3: Verify touch mapping**

Tap the four screen corners and the center; record the five `touch: x,y` readouts.
- If coordinates are mirrored (x → 359-x, y → 479-y) or swapped: apply the transform in `pointer_read_cb` (`p.x = 359 - p.x;` etc.) so taps land where the finger is, rebuild, redeploy, re-verify.
- The counter must increment on button taps.

- [ ] **Step 4: Verify hardware keys do not crash**

Press POWER / PREV / NEXT / PLAY / HOME and turn the volume knob; the UI must stay responsive (they are ignored in M0), audio volume must still change (rotary handled outside LVGL at this stage).

- [ ] **Step 5: Verify playback path intact**

Play a file via resume-on-boot (or web control), confirm audio works while LVGL owns the screen.

- [ ] **Step 6: Regression check — classic build**

Run: `wsl -d Debian -- bash -lc "cd /home/ctzjj/rockbox && git stash list >/dev/null; grep -n HAVE_LVGL_UI firmware/export/config/n3pro.h && sed -i 's/#define HAVE_LVGL_UI 1/#if 0 \\&\\& LVGL_UI_DISABLED\\n#define HAVE_LVGL_UI 1\\n#endif/' firmware/export/config/n3pro.h; cd build-n3pro && make clean >/dev/null && make 2>&1 | tail -3; cd .. && git checkout firmware/export/config/n3pro.h"`
(Practical shortcut: temporarily comment the define, rebuild, confirm classic UI boots in QEMU or on device, restore the define.) Expected: classic build unaffected.

- [ ] **Step 7: Commit any calibration fix**

```bash
git add apps/gui/lvglui/lvgl_port_input.c
git commit -m "lvgl: touch coordinate calibration for the N3Pro panel"
```

---

## Self-Review

- **Coverage:** M0 scope = build wiring, display port, input port, smoke screen, device verification. No Part-A inventory items touched yet (M1+). ✔
- **Placeholders:** the two conditional adjustments (touch transform in Task 6.3, optional `LV_USE_*` flags in Task 2.3/5.6) include exact commands and acceptance criteria, not open TODOs. ✔
- **Type consistency:** `lvgui_main/lvgui_port_disp_init/lvgui_port_input_init/lvgui_input_feed/lvgui_scr_smoke_create` declared once in `lvgl_ui.h`, used identically in Tasks 3-5. ✔

## Verification summary (evidence before "done")

1. `liblvgl.a` builds clean (Task 2.3).
2. `rockbox.elf` links with LVGL smoke screen (Task 5.6).
3. Device boots to smoke screen; taps map 1:1; keys harmless; audio path intact (Task 6.2-6.5).
4. Classic build with the define removed still builds/boots (Task 6.6).
