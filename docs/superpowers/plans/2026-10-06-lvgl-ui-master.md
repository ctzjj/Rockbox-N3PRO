# LVGL UI for Cayin N3Pro — Master Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement each milestone plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the classic Rockbox UI on the N3Pro with a modern touch-first LVGL UI (glassmorphism, album-art accent, approved 24-screen design), covering 100% of the menus and settings enumerated below.

**Architecture:** LVGL v8.4 (vendored in `lib/lvgl`) renders into Rockbox's native RGB565 framebuffer via a port layer (`apps/gui/lvglui/`) and flushes through the existing beam-chasing `lcd_update_rect()` path. The LVGL shell replaces `root_menu()` as the GUI entry; plugins, pitch screen and debug screens keep running in the classic renderer (LVGL pauses while they draw). All engine work (playlist/tagcache/audio/settings/DSP/wifi/bt/netfm/dlna/web HALs) is reused untouched — only the presentation layer is replaced. Strings come from the Rockbox language core (`str(LANG_*)`) so chinese-simp stays consistent.

**Tech Stack:** C (rockbox app build, mipsel toolchain), LVGL v8.4.0 (submodule), generated CJK fonts (lv_font_conv, committed), rockbox kernel threads + button queue.

**Branch:** `lvgl-ui` (from `n3pro-newbase` @ b7a4753ae0, repo `/home/ctzjj/rockbox`).

---

## Part A — Verified inventory (source of truth; DO NOT shrink)

Compiled-truth corrections vs. earlier assumptions: `CONFIG_RTC = RTC_HOSTED` (**Time & Date exists**, hibylinux.h:83), `HAVE_PITCHCONTROL` **on** (config.h:1425 → pitch screen + timestretch), WiFi menu has **9 items** (SSH + DLNA on), WPS context slot 3 default = **PITCHSCREEN** (settings_list.c:810-823).

### A1. Root menu (apps/root_menu.c:550-575, verified against build-n3pro objects)
1. Recent Bookmarks *(hidden at runtime when `usemrb == 0`)*
2. Files
3. Database (tagcache)
4. Now Playing / Resume Playback *(label dynamic on audio_status())*
5. Settings
6. Playlists
7. Bluetooth (3 items: Output / Input / Reset — apps/bluetooth.c:564-591)
8. WiFi (9 items — below)
9. Plugins (Games / Applications / Demos)
10. System (Rockbox info / Credits / Running Time / Legal / Debug — main_menu.c:491-493)
11. Shortcuts
Compiled out: Recording, FM Radio. Root order user-configurable (`root menu order` cfg + main_menu_config.rock plugin).

### A2. Settings ▸ Sound (sound_menu.c:279-322 + eq_menu.c)
Volume · Maximum Volume Limit · Bass · Treble · Balance · Channel Configuration · Stereo Width · DAC filter roll-off · **Tube Mode** (N3Pro-specific) · Crossfeed submenu (master/direct/cross/HF atten/HF cutoff) · **Space'80 Reverb** submenu (decay/freq comp/gain/dry-wet) · **SWD Effect** submenu (on/preset/3D width/center/focus/definition/bass/bass EQ/output mode/in gain/out gain/limiter) · **Equalizer** submenu (enable/graphical EQ/precut/simple 5-band/advanced per-band cutoff·Q·gain·type/save preset/browse presets/reset) · Dithering · **Haas Surround** submenu (enable/balance/f(x1)/f(x2)/side only/dry-wet) · **PBE** submenu (enable/precut) · Auditory Fatigue Reduction · **Timestretch** · **Compressor** submenu (threshold/makeup/ratio/knee/attack/release).
Off: bass/treble cutoff, HW-EQ, 3D depth, HP/LO select, power mode, speaker.

### A3. Settings ▸ Playback (playback_menu.c:184-218)
Shuffle · Repeat · Play Selected First · FF/RW min step · FF/RW accel · Fade on Stop/Pause · Single Mode · Party Mode · **Crossfade** (enable/in delay/in duration/out delay/out duration/mode) · **Replaygain** (type/prevent clipping/pre-amp) · Track Skip Beep · Auto-Change Directory · Constrain Auto-Change · Cuesheet Support · **Pause on Headphone Unplug** (mode + autoresume) · Skip Length · Prevent Track Skipping · Rewind Across Tracks · Rewind Before Resume · Rewind on Pause · Frequency (→192k) · Album Art · Logging.
Off: anti-skip buffer, S/PDIF.

### A4. Settings ▸ General (settings_menu.c:841-855)
- **What's Playing Screen**: WPS Hotkey + Set Context Items 1-4 + Reset (settings_menu.c:773-828)
- **Playlists**: Sort · Viewer Settings (icons/indices/track display) · Recursive Insert · Current Playlist (warn erase/keep track when replacing/show shuffled adding/show queue options) (playlist_menu.c:120-163)
- **File View**: sort case/dirs/files/numbers · Show Files · Filename Extensions · Follow Playlist · Show Path · Remember last folder · Start at / · File Browser Hotkey (settings_menu.c:186-223)
- **Database**: Auto Update · Initialize Now · Update Now · Gather Runtime Data · Export/Import Modifications · Select scan directories (tagcache_ram off) (settings_menu.c:153-180)
- **Display**: **LCD Settings** (backlight · backlight plugged · backlight on lock · caption backlight · fade in/out · first press enables only · **backlight exemptions** · brightness) · **Scrolling** (speed/start delay/step/bidi limit/screen scrolls out of view/scroll step/paged/list wraparound/list order/accel start/accel speed) · **Peak Meter** (release/peak hold/clip hold/scale/range min/range max) · Default Codepage · **Touchscreen Settings** (mode · flick shortcuts · line padding · calibrate · reset calibration) (display_menu.c)
- **System** (settings_menu.c:435-494): Limits · Volume Adjustment Mode · Number of Volume Steps · Shortcuts-instead-of-Quickscreen · Car Adapter Mode (charging on) · Keyclick (+repeats) · Selective Softlock · **USB** (USB Audio, USB Mode/ADB) · Use LED indicators
- **Startup/Shutdown**: Show Shutdown Message · Start Screen · Idle Poweroff · Sleep Timer toggle · Default Sleep Duration · Start Sleep Timer on Boot · Restart on Keypress (settings_menu.c:542-578)
- **Bookmarking**: on stop · update on stop · load last · maintain recent list (usemrb — also gates root item A1.1)
- **Automatic Resume**: enable · resume on automatic track change
- **Language** (browse .lng)
- **Voice**: menus · directories · dir .talk clips · filenames · file .talk clips · file type · battery level · voice volume

### A5. Settings ▸ Theme (theme_menu.c:388-418) — governs classic renderer, keep reachable
Browse Themes · Font · While Playing Screen · Base Skin · Show Icons · Clear Backdrop · **Status-/Scrollbar** (scroll bar · width · status bar · volume display · battery display) · Line Selector Type · Line Separator · **Colours** (line selector primary/secondary/text · separator · background · foreground · reset).

### A6. Settings ▸ Time & Date (time_menu.c, RTC_HOSTED)
Set Time/Date · Time Format 12/24h. (Alarm off.)

### A7. Settings ▸ Main Menu + Manage Settings
Main Menu (main_menu_config.rock) · Browse .cfg · Reset Settings · Save .cfg · Save Sound Settings · Save Theme Settings.

### A8. WPS + context/quick surfaces (onplay.c, gui/wps.c, quickscreen.c)
- WPS onplay menu: Current Playlist (view/save/**search in playlist**/shuffle/**playing time**) · Add to Playlist (existing/new) · Sound Settings · Playback Settings · Rating · Bookmarks (create/list) · View Cue Sheet · **5 configurable context slots** (defaults: view playlist / track info / reveal in files / **pitch screen** / album art)
- Hotkey actions (any slot / tree hotkey): off, view playlist, properties, pictureflow, track info, pitch screen, open with, delete, insert, insert shuffled, bookmark, bookmark list, plugin, album art, reveal in files
- Quickscreen: 4 configurable setting slots (default: shuffle/repeat), `shortcuts_replaces_qs` swap
- Pitch screen (HAVE_PITCHCONTROL), Playing Time screen, Playlist Catalog viewer

### A9. Tree/Database context menu (onplay.c tree_onplay_menu)
View · Playing Next (insert first/last/shuffled + queue variants + replace) · Add to Playlist · Rename · Cut · Copy · Paste · Delete · Delete Dir · Open With · Create Directory · Properties · Track Info · Reveal · PictureFlow · Set Backdrop · Add to Favourites · Set As (start dir/database dir/catalog dir) · Sort Playlists (in m3u view) · file Menu

### A10. WiFi menu (apps/wifi_menu.c — 9 items) + aux screens
WiFi On/Off (stops SSH/Web/NetFM/DLNA on off) · Scan (SSID list + password kbd + auto-join + NTP) · Saved Networks (connect/forget) · Status (5 live rows: radio/conn/SSID/IP/signal) · Reset · SSH Start/Stop · NetFM (station list from `stream/netfm/netfm.txt`; play screen 7 rows: state/name/format/bitrate/rate/buffer/Stop; Back keeps playing) · Web Control (4 rows: service/access code/URL/start-stop) · DLNA (renderer browse/play).
Bluetooth menu: Output (status/devices+scan+connect/disconnect) · Input rx screen (5 rows: state/peer/codec/rate/disconnect) · Reset.

### A11. System root menu
Rockbox info (versions/free space/battery) · Credits · Running Time · Legal Notices · Debug (classic screen, keep).

### A12. Plugins
Games / Applications / Demos browsers (full keypad-style set via SOURCES.app_build CAYIN_N3PRO exception). Radio/recorder plugins compiled out. Plugins run in **classic renderer**; LVGL pauses.

### A13. Statusbar fields (statusbar.c) — LVGL status bar must reproduce
Battery (+charging anim) · USB plug · charger plug · volume · play state · repeat mode · shuffle · keylock · disk LED · clock (RTC) · **RF icons: WiFi + BT** (12px glyphs, HAVE_STATUSBAR_RF_ICONS).

### A14. Globals touching the UI
LCD sleep (`HAVE_LCD_SLEEP`, timeout 2s after backlight off) · touch ignored while screen off (button-n3pro.c:86-96) · softlock/keylock · sleep timer · USB plug behavior (gadget reconfig, no classic USB screen file) · startup splash/resume.

---

## Part B — Screen set for the LVGL UI

From the approved mockups (`all-screens.html`) **extended** so nothing in Part A is unreachable:

| # | LVGL screen | Covers |
|---|---|---|
| 1 | Home | A1 root (2-6, 9-11) + continue-playing card + mini player |
| 2-5 | Library root / Albums grid / Album detail / All tracks | Database browsing + ratings + search |
| 6 | Files | A4 File View browsing + file rows |
| 7 | Queue/Playlist viewer | Current playlist + catalog + queue ops (A8/A9 playlist parts) |
| 8-12 | WiFi / Scan+KBD / NetFM / Web / DLNA | A10 |
| 13-20 | Settings root / Sound / EQ editor / Playback / Display / System(USB/LED/softlock/keyclick/limits) / General (all A4 submenus as grouped tabs) / Theme+Time+Manage+Voice/Language | A2-A7 |
| 21 | WPS (approved design) | playback UI + pitch entry + playing time |
| 22 | Quickscreen overlay | A8 |
| 23 | Lock screen | keylock + LCD sleep wake |
| 24 | Context sheet (track/file) + text keyboard | A8/A9 actions + rename/password |
| 25 | Bluetooth screens (root/output devices/rx status) | A10 BT |
| 26 | Plugins browser | A12 (launch classic) |
| 27 | System/info screen (+ link to classic Debug) | A11 |
| 28 | Shortcuts screen | A1.11 |
| 29 | Sleep timer sheet | A4 Startup/Shutdown quick access |
| — | Classic embeds (LVGL pauses): plugins, pitch screen, debug, playing time, pictureflow | risk containment |

**Rule: every item in Part A must be reachable from exactly ONE primary entry (no duplicates, none lost).** Each milestone plan includes a coverage checklist against Part A.

## Part C — Milestones (each = one detailed sub-plan, written just-in-time)

- **M0 Foundation** — LVGL builds in rockbox, fb port, input port, smoke screen, device verification. *Detailed plan: `2026-10-06-lvgl-ui-m0-foundation.md` (this folder).*
- **M1 Theme & shell** — glass theme engine, accent-from-album-art, CJK fonts (generated, committed), screen stack + nav, status bar, list/row/button/sheet/keyboard components, dark/light.
- **M2 Home + Library** — screens 1-5 bridging tagtree/tagcache; search.
- **M3 Files + Playlists** — screens 6-7 + context sheet (A9) + catalog.
- **M4 WPS + Quickscreen** — screen 21/22 + volume popup + ↻ cycle + rating + pitch entry; seek; metadata.
- **M5 Sound settings** — A2 incl. graphical EQ editor screen.
- **M6 Remaining settings** — A3-A7 full coverage + coverage checklist.
- **M7 Network & BT** — screens 8-12, BT screens; runtime-state wiring.
- **M8 System surfaces** — lock, shortcuts, sleep sheet, plugins browser, info, USB/statusbar events.
- **M9 Polish & perf** — animations within frame budget (blit ≈9ms), boot splash, resume, edge cases (USB plug, battery critical, screen-off touch ignore).
- **M10 Release** — tag v3.0.0 line, workflow, README.

## Part D — Cross-cutting decisions (locked)

1. LVGL renders 360x480 RGB565 into rockbox fb; flush = per-row `memcpy` to `FBADDR` + `lcd_update_rect` (reuses vsync chase; no new fb access).
2. LVGL runs in the GUI thread (the `root_menu()` caller); `lv_timer_handler()` cadence HZ/20; `LV_TICK_CUSTOM` maps `current_tick`→ms.
3. Input: one pipeline — rockbox button queue (`button_get_w_tmo` + `button_get_data`, touch coords `x<<16|y`); LVGL indevs: POINTER (touch+HOME-cap key), ENCODER (rotary = volume/list scroll), buttons map to nav actions per-screen.
4. Strings via `str(LANG_*)` (language core); branded custom labels in a local zh/en table.
5. Fonts: Montserrat (built-in) for Latin/digits + generated CJK subsets (12/16/20/28px, GB2312-level1 ~3500 chars + symbol set ★▶⏸⏭⏮🔊…) via lv_font_conv, committed under `apps/gui/lvglui/fonts/`.
6. Plugins/pitch/debug stay classic (LVGL `lv_disp` inactive while a classic screen runs; resume redraws).
7. All new code guarded `#ifdef HAVE_LVGL_UI`; a firmware build without the define is bit-identical to today.
8. Settings engine untouched — the LVGL settings screens read/write `global_settings` through the same `set_setting()`/callbacks as classic menus.

## Part E — Risks
- Touch coordinate orientation (GT9XX raw vs LCD 180° rotation) — calibrate on device in M0/M1.
- Frame budget: full-screen invalidation ≈9ms blit; keep partial draws (LVGL dirty areas) and avoid shadows on scrolling lists until measured (M9).
- LVGL memory: system malloc (hosted app, plenty); cap draw buf 360×40 px.
- CJK font size (~700KB total) — fine in .rockbox.
- Keymap regressions for hardware keys while LVGL owns the screen — per-screen keymaps in M1.
