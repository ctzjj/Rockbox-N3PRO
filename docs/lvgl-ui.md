# LVGL UI — architecture and the backend boundary

The `n3pro-lvgl` line replaces Rockbox's classic renderer with a touch-first
LVGL 8.4 UI. Engine code (playback, playlist, tagcache, settings, DSP, WiFi,
Bluetooth, netfm, DLNA) is reused **unmodified**; only presentation changes.
The golden rule of this line: **screens never talk to Rockbox core — they talk
to `ui_backend.c`.**

Rockbox-tree branch that produced the overlay: `lvgl-ui` (from
`n3pro-newbase`), working tree `/home/ctzjj/rockbox`.

---

## 1. Layering

```
apps/gui/lvglui/
  ui_shell.c  ui_library.c  ui_player.c  ui_menu.c  ui_control.c   ← screens
        │           │            │           │           │
        │     ui_theme.c (th->)  ui_widgets.c (ui_*)  ui_icons.c   ← house style
        │           │            │           │           │
        └───────────┴────────────┴───────────┴───────────┘
                              │  only these three entry points:
                lvgl_ui.h (shell/job API) · ui_backend.h (be_*) · ui_theme.h
                              │
                        ui_backend.c  (NO LVGL types, NO lvgl.h)
                              │
   Rockbox core: audio/playlist · tagcache/tagtree · tree/ft_load · settings
                 settings_list · eq · bt_audio · wifi_hal · netfm · dlna · power
```

Ports (the only code allowed to touch both worlds):

| file | role |
|---|---|
| `lvgl_port_disp.c` | LVGL flush → per-row memcpy into Rockbox's RGB565 framebuffer (`FBADDR`) + `lcd_update_rect()`; the n3pro LCD driver keeps doing the 180° rotation and the beam-chasing tear-free blit. Partial refresh (`full_refresh = 0`), one full-screen draw buffer. |
| `lvgl_port_input.c` | Rockbox button queue → LVGL pointer indev. Touch rides `BUTTON_TOUCHSCREEN` with `(x << 16) \| y` in the button data word; also owns the global gestures (top strip 40 px = control centre, side edge 24 px + 56 px drag = back, lock screen = swipe up ≥ 80 px). |
| `lvgl_ui.c` | The GUI thread loop: replaces `root_menu()` (apps/main.c). `lvgui_pump()` = wake-redraw + `button_get_w_tmo()` + volume knob + `lvgui_input_feed()` + `lv_timer_handler()`; stock blocking handlers run as *jobs* between pumps. |
| `ui_compat.c` | No-op stand-ins for classic-UI symbols the frozen plugin ABI/settings still reference (skin engine, WPS, playlist_viewer). |

`HAVE_LVGL_UI` lives in `firmware/export/config/n3pro.h`; `apps/SOURCES`
compiles the `gui/lvglui/` block and drops the classic UI files
(`wps.c`, `playlist_viewer.c`, `gui/skin_engine/*`, `statusbar-skinned.c`).
LVGL itself is the `lib/lvgl` submodule (v8.4.0), built by `lib/lvgl.make`.

---

## 2. The backend contract (do not break it)

`ui_backend.h` is the **only** interface between screens and Rockbox. It is a
pure data API: no LVGL types, no `lvgl.h`, no UI callbacks.

### Rules

1. **No Rockbox headers in screens.** `ui_*.c` may include only `lvgl.h`,
   `lvgl_ui.h`, `ui_theme.h`, `ui_widgets.h` (plus libc). Anything from
   `audio.h`, `settings.h`, `playlist.h`, `tagtree.h`, `bt_audio.h`,
   `wifi_hal.h`, `netfm*.h`, `dlna/*` must be reached through a `be_*` call.
2. **`ui_backend.c` stays LVGL-free.** It must not include `lvgl.h`. If a
   backend routine has to notify the UI (e.g. a toast during a blocking reset),
   declare the tiny UI hook locally at the top of the file
   (`void shell_toast(int, const char *, const char *);`,
   `bool lvgui_in_job(void);`) — never pull in the UI headers.
3. **Reuse stock logic.** Scanning uses `ft_load`, database browsing uses
   `tagtree`, playlist editing uses the stock API, settings use
   `global_settings` + `settings_list`, EQ uses the stock gain editor, and
   "open with", save-playlist, context menus call the stock entry points (see
   §4). Never re-implement sorting, filtering, shuffle or tag parsing.
4. **Ownership is explicit.**
   - `be_list_t` → `be_list_free()`
   - `be_img_t` / `lvui_art_t` / `char **` arrays → the matching
     `be_img_free()` / `be_str_arrays_free()` / `free()`
   - Paths are copied into caller buffers (`BE_PATH_MAX`); strings returned
     from fixed structs are valid until the next `be_*` call.
   - New code should avoid `malloc`; the list/artwork buffers above are the
     documented, deliberate exception because the UI owns them.
5. **Text.** Use `str(LANG_*)` for anything the stock language has. New strings
   must go into `apps/lang/english.lang` (**ids are positional** — inserting
   shifts every later id, so regenerate all `.lng`). GBK tag data goes through
   `shell_cs()` before it reaches a label.
6. **Threading.** LVGL lives on the GUI thread only. Audio/network/BT threads
   must never call LVGL — they update state, the UI polls it (`be_*` +
   `lv_timer`) or is refreshed by an event. Never call a screen function from a
   Rockbox callback.
7. **Power.** `be_power_off()` calls `power_off()` and `be_reboot()` calls
   `system_reboot()` (hosted, `firmware/target/hosted/system-hosted.c`).
   Do **not** use `sys_poweroff()`/`sys_reboot()`: they only broadcast
   `SYS_POWEROFF`/`SYS_REBOOT`, which is consumed by the classic event loop
   that the LVGL build never runs. Both shutdown paths ignore SIGTERM so the
   process survives init's shutdown signal (otherwise the launcher's
   "app exited → reboot" fallback would turn power-off into a reboot) and the
   panel keeps the "正在关机…/正在重启…" toast until the power goes.

### Blocking operations: jobs

Stock handlers block (they own the action loop while their screen runs). The
LVGL loop therefore refuses to run them inside an `lv_timer`/event callback:

```c
static void my_job(void *param)            /* runs at the top of lvgui_main */
{
    be_do_something_blocking();            /* may pump LVGL via lvgui_idle_pump */
}

/* from an LVGL event callback: */
lvgui_run_job(my_job, NULL);
```

`lvgui_in_job()` is true only while such a handler runs. The stock UI
functions check it and switch to their LVGL equivalent; that is what keeps all
logic (menu trees, option editors, confirmations) intact. Consequently:

- blocking LVGL dialogs (`shell_yesno_run`, `shell_kbd_input`,
  `shell_list_pick*`, `shell_splash`, `shell_toast` with `ticks > 0`) are
  **job-only**;
- `lvgui_pump()` (input + timers) and `lvgui_idle_pump()` (timers only) exist
  for code that has to wait while the classic call is in flight;
- after a job that drew with the classic renderer (plugins, USB screen, debug
  screens) invalidate `lv_scr_act()` and `lv_layer_top()` so LVGL repaints over
  the foreign pixels — the framebuffer is shared.

---

## 3. The shell

- **Navigation**: `shell_screen_new()` creates a full-screen page (wallpaper
  backdrop, `th->font_cjk` default font); `shell_nav_push()` shows it and
  pushes it on a depth-12 stack. Popping is deferred with `lv_async_call()`
  because the tapped widget lives on the screen being deleted — use
  `shell_nav_pop()` from callbacks, `shell_nav_pop_now()` only from timers.
  Home is stack slot 0 and is never popped; `shell_nav_home()` tears the pages
  above it down.
- **Chrome**: `shell_statusbar_build()`, `shell_header()`, `shell_menu_screen_push()`,
  `shell_menu_row_new()`, `shell_glass_new()`, `shell_soft_shadow()`,
  `shell_scrollbar_setup()`.
- **Dialogs**: `shell_modal_open()` (scrim + glass card, arbitrary buttons),
  `shell_yesno_run()`, `shell_kbd_input()`, `shell_list_pick*()`,
  `ui_select_sheet()`.
- **Overlays**: `ui_control.c` is the pull-down control centre (volume/now
  playing cards, source toggles, and the second row 锁屏/设置/关机/重启);
  `ui_shell.c` also owns the lock screen (`lvgl_shell_locked`,
  `shell_lock_show()/hide()`), which takes over all gestures until the upward
  swipe.
- **Style**: every colour/opacity/radius/font/spacing is in `ui_theme_t` and
  reached as `th->…`; reusable components are in `ui_widgets.h`. Screens must
  not hard-code styles, so a theme swap is one `ui_theme_fill()`.

---

## 4. Stock entry points remapped for LVGL

Only these files contain `#ifdef HAVE_LVGL_UI` inside shared Rockbox code.
Each is a two-line hook: *if `lvgui_in_job()` → LVGL implementation, else the
byte-identical upstream path.*

| stock entry point | LVGL implementation |
|---|---|
| `do_menu()` (apps/menu.c) | `lvgui_do_menu()` (ui_menu.c) — whole menu tree, settings editors, EQ |
| `simplelist_run*` (apps/gui/list.c) | `shell_list_pick*()` / `shell_list_pick_refresh()` |
| `gui_syncyesno_run*` (apps/gui/yesno.c) | `shell_yesno_run()` |
| `splash()` (apps/gui/splash.c) | `shell_splash()` |
| `option_screen()` (apps/gui/option_select.c) | `lvgui_option_screen()` (live preview) |
| `set_color()` (apps/gui/color_picker.c) | `lvgui_set_color()` |
| `kbd_input()` (apps/recorder/keyboard.c) | `shell_kbd_input()` (+ pinyin IME) |
| `rockbox_browse()` (apps/tree.c) | `lvgui_browse_pick()` |
| "Open with" picker (apps/filetypes.c) | `shell_list_pick()` |
| playlist catalog (apps/playlist_catalog.c) | `shell_plcat_pick()` |
| bookmark load / cuesheet pick | `lvgui_do_menu()` / `lvgui_cuesheet_pick()` |
| Bluetooth menus + BT-RX screen (apps/bluetooth.c) | `lvgui_do_menu()` / `lvgui_bt_rx_view()` |
| WiFi "resetting…" text screen (apps/wifi_menu.c) | `shell_toast()` |
| Debug pages (apps/debug_menu.c) | `lvgui_text_page()` |
| root menu / main loop (apps/root_menu.c, main.c) | `lvgui_main()` |

Everything else in those files is untouched; the `#else` branches must stay
byte-identical to upstream (same rule as the hardware port — see AGENTS.md).

---

## 5. Adding a feature (checklist)

1. **Data first.** Add `be_xxx()` to `ui_backend.h` + implement it in
   `ui_backend.c` using stock APIs. No LVGL, no UI.
2. **UI second.** Build the screen from `shell_*` / `ui_*` / `th->`. Poll the
   backend from an `lv_timer` (100 ms–1 s) or refresh on events; never from
   another thread.
3. **Actions.** Do the work synchronously in the event callback only if it is
   instantaneous; otherwise `lvgui_run_job()` it. Confirm destructive actions
   with `shell_yesno_run()` *inside the job*.
4. **Entry point.** Add the row to the home screen (`ui_shell.c`), the control
   centre (`ui_control.c`) or the stock menu tree, and register the icon in
   `ui_icons.c` / `lvgui_menu_icon()`.
5. **Strings.** `str(LANG_*)`; new language entries only by appending at the
   end of `english.lang` sections and regenerating the `.lng` files.
6. **Build, deploy, verify** (§6), then run the review checklist (§8).

Sketch:

```c
/* ui_backend.h */
bool be_foo_get(int *out);

/* ui_backend.c */
bool be_foo_get(int *out)
{
    if (!audio_status()) return false;
    *out = global_settings.foo;          /* stock data, stock units */
    return true;
}

/* ui_player.c */
static void foo_refresh(lv_timer_t *t)
{
    int v;
    if (be_foo_get(&v))
        lv_label_set_text_fmt(g_foo, "%d", v);
}

static void foo_job(void *p) { (void)p; if (shell_yesno_run("确定？") == 0) be_foo_apply(); }
static void foo_cb(lv_event_t *e) { (void)e; lvgui_run_job(foo_job, NULL); }
```

---

## 6. Build, deploy, verify

```sh
# one-time: the patch adds lib/lvgl as a gitlink only
git submodule update --init lib/lvgl        # LVGL v8.4.0

export PATH=$HOME/rockbox-toolchain/bin:$PATH
mkdir -p build-n3pro && cd build-n3pro
../tools/configure --target=n3pro --type=N && make -j$(nproc)
```

Deploy on the device (the SD card is not mounted at boot):

```sh
adb shell "mount -t vfat /dev/mmcblk0p1 /mnt/sd_0"
adb push rockbox.n3pro /mnt/sd_0/.rockbox/rockbox.n3pro
adb shell "killall rockbox.n3pro"          # the launcher restarts it
```

Screenshots for UI work: the framebuffer reads as black while the backlight is
off (the panel latches the image), so wake it first.

Package with `firmware/target/hosted/cayin/n3pro/n3pro_patcher.sh` as for the
other lines.

---

## 7. Constraints that bite

- **360×480, RGB565, 180°-rotated panel.** LVGL never sees the rotation; the
  LCD driver does it. Coordinates in events are screen coordinates
  (0,0 = top-left as the user sees it).
- **Shared framebuffer, partial refresh.** LVGL only flushes invalidated areas;
  anything that painted behind its back (classic splash/plugin/USB screen) must
  be followed by a full invalidation. The wake path does this automatically
  (`LCD_EVENT_ACTIVATION` → `g_wake_dirty`).
- **No vsync ioctl.** `lcd_update_rect()` does the beam-chasing write in the
  LCD driver; do not write `FBADDR` directly from UI code.
- **No heap discipline.** Prefer statics; heap buffers returned by `be_*` are
  caller-owned and must be freed deterministically.
- **Rotary encoder = volume** at the shell level (`adjust_volume()` in
  `lvgui_pump`), independent of the focused screen.
- **glibc 2.16 runtime**: no `powf`/newer libm symbols (see AGENTS.md).
- **Language ids are positional**; stale `.lng` files mislabel every later id.
- **Plugins keep the classic renderer**; they run inside a job, so LVGL is not
  pumping while they draw.

---

## 8. Review checklist

- [ ] No `#include` of Rockbox core headers in `ui_*.c`; everything through `be_*`.
- [ ] No `lvgl.h`/UI headers in `ui_backend.c`.
- [ ] Blocking stock calls wrapped in `lvgui_run_job()`; blocking dialogs only in jobs.
- [ ] No LVGL call from a non-GUI thread.
- [ ] Style from `th->`, components from `ui_widgets.h`; no hard-coded colours/metrics.
- [ ] Strings via `str(LANG_*)`; charset conversion for tag text.
- [ ] Buffers freed; no new `malloc` in hot paths.
- [ ] Destructive actions confirmed; shutdown through `be_power_off()/be_reboot()`.
- [ ] Builds with the toolchain, runs on the device, and a screenshot proves it.
