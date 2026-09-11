# AGENTS.md — Cayin N3Pro (Rockbox hosted port)

This repository is **not** a Rockbox fork. It contains only the files a Rockbox
checkout needs in order to build for the **Cayin N3Pro** DAP (HiByOS / Ingenic
X1000, Linux 3.10.14), plus a `n3pro_port.patch` that applies them to a pristine
tree, and `n3pro_patcher.sh` which builds the flashable `.upt` image.

Base commit the patch was generated against: **`10ec9bd530`** (Rockbox master,
2026-09).  See "Applying to a newer master" below.

---

## Layout

```
firmware/export/config/n3pro.h                     target config (the only model header)
firmware/target/hosted/cayin/n3pro/                 the N3Pro target driver
    button-n3pro.c / button-target.h                keys, touch, scroll wheel, HOME
    powermgmt-n3pro.c                               battery curves (see "Battery")
    led-n3pro.c                                     RGB LED (LP5562 pattern engine)
    debug-n3pro.c lcd-target.h system-target.h adc-target.h
    cayin-n3pro.c/.h                                tube / filter / gain housekeeping
    n3pro_patcher.sh                                unpack + inject + pack a .upt
apps/keymaps/keymap-n3pro.c                         keymap (scroll wheel + HOME)
wps/cabbiev2.360x480x16.wps, wps/cabbiev2/*-360x480x16.bmp
tools/…                                             configure/builds.pm entries (patch only)
n3pro_port.patch                                    ALL of the port as a unified diff
```

Everything outside `firmware/target/hosted/cayin/n3pro/`, `n3pro.h`,
`keymap-n3pro.c` and the WPS assets is a **change to shared Rockbox code** and
must obey the rules below.

---

## Golden rules

1. **Never modify shared code without a compile guard.**
   Every change to a file used by other targets must be wrapped in
   `#if defined(CAYIN_N3PRO) … #else … #endif` (or
   `#if (AUDIOHW_CAPS & TUBE_MODE_CAP)` for the tube setting), with the `#else`
   branch byte-identical to upstream.  Shared files touched so far:
   `firmware/usb.c`, `firmware/target/hosted/usb-hiby.c`,
   `firmware/target/hosted/lcd-linuxfb.c`,
   `firmware/target/hosted/hiby/hibylinux_codec.c` / `.h`,
   `firmware/target/hosted/hiby/usb-dac-hiby.c`, `apps/recorder/keyboard.c`,
   `apps/settings{,_list}.c`, `apps/settings.h`, `apps/menus/{sound,settings}_menu.c`,
   `firmware/{sound.c,export/sound.h,export/audiohw.h,export/audiohw_settings.h,export/config.h}`,
   `bootloader/hibyos_linux.c`, `firmware/export/usb.h`.
   `usb-hiby.c` is also compiled for AGPtEK Rocker / Aigo Eros Q / Surfans F28 /
   Xduoo X20 / X3II — and `usb-dac-hiby.c` for R1/R3ProII/AP80Max.  Breaking
   their `#else` path is the easiest way to regress other players.
2. **Reuse upstream, never reinvent.**  Filters use `LANG_FILTER_*`; USB uses the
   stock `usb_audio` setting (`LANG_USB_DAC`) and USB-modes; the wheel uses the
   `HAVE_SCROLLWHEEL`/`BUTTON_SCROLL_FWD|BACK` mechanism (same as AP80Max); the
   LED uses `HAVE_GENERAL_PURPOSE_LED` + `led_hw_*()`.  New identifiers get the
   `N3PRO_*` / `CAYIN_*` / `LANG_TUBE_*` prefix.
3. **Never touch the kernel or the bootloader image.**  The port is hosted: only
   `/usr/bin/hiby_player.sh` (rewritten) and `/usr/bin/bootloader.n3pro` (added)
   change in the rootfs; the stock `hiby_player` binary stays and remains
   selectable from the boot menu.  Flash failures can therefore always be undone
   by flashing the stock `update.upt`.
4. **Keep the model ids consistent** (bump them if upstream claims one):
   `MODEL_NUMBER 126` (`n3pro.h`), configure menu entry `303|n3pro`
   (`target_id 126`), `CAYIN_N3PRO_PAD 81` (`firmware/export/config.h`),
   `TUBE_MODE_CAP (1 << 13)` in `audiohw.h`.
5. **Language ids are positional.**  Inserting/removing entries in
   `apps/lang/english.lang` shifts every later id.  The `.lng` files shipped in
   `rockbox-full.zip` must therefore always match the app; a card with stale
   `.lng` shows wrong menu labels (the classic symptom: the USB / LED entries
   "disappear" because they are mislabelled).  When in doubt, replace the whole
   `.rockbox` from the matching zip.

---

## Building

The port needs the **MIPS hosted Rockbox toolchain** (`mipsel-rockbox-linux-gnu`,
gcc 9.x + glibc 2.27 sysroot + alsa-lib), built with `tools/rockboxdev.sh`.

```sh
export PATH=$HOME/rockbox-toolchain/bin:$PATH

mkdir build-n3pro build-n3pro-bl
( cd build-n3pro    && ../tools/configure --target=n3pro --type=N && make -j$(nproc) )
( cd build-n3pro-bl && ../tools/configure --target=n3pro --type=B && make -j$(nproc) )

# SD-card payload (app + fonts + langs + theme)
( cd build-n3pro && make fullzip )        # -> rockbox-full.zip
```

- app output: `rockbox.n3pro`, bootloader output: `bootloader.n3pro`.
- `make zip` (without `full`) omits the fonts — do not ship that one.

## Packaging the firmware image

```sh
firmware/target/hosted/cayin/n3pro/n3pro_patcher.sh stock.upt build-n3pro-bl/bootloader.n3pro
# -–unpack / --inject-app / --pack modes are also available
```

Requires `7z`, `genisoimage`, `mtd-utils` (mkfs.ubifs) and
`ubireader_extract_files`.  The patcher preserves the stock kernel md5, rebuilds
the UBIFS rootfs with the N3Pro geometry (`-m 2048 -e 126976 -c 480`) and emits
`<name>_rb.upt` with a corrected `update.txt`.

## Installing (end user)

1. Card formatted **exFAT** (or FAT32 that the updater accepts); extract
   `rockbox-n3pro.zip` to the card root → `.rockbox/`.
2. **Rename the image to `update.upt`** and copy it to the card root — the
   updater only looks for that exact name.
3. Power off, hold **PLAY** while pressing **POWER** → updater v1.1 flashes and
   reboots.  Boot menu: `ROCKBOX` / `TOOLS` / `CAYIN PLAYER` (stock).

## Deploying during development

```sh
adb shell "mount -t vfat /dev/mmcblk0p1 /mnt/sd_0"      # the card is NOT
adb push rockbox.n3pro /mnt/sd_0/.rockbox/rockbox.n3pro # mounted while the
adb shell "sync; reboot"                                # UMS gadget is up
```

- Mounting first matters: without it the push lands in the rootfs shadow
  directory and nothing changes.
- On Windows `adb` may need the libusb backend (`set ADB_LIBUSB=1`), and
  sometimes `adb kill-server` + replug.  The USB-DAC/ADB composite binds the
  ADB interface to generic WinUSB, which the classic backend does not always
  pick up.
- If ADB is unavailable, the bootloader **TOOLS → ADB start** gives a root
  shell, or edit the card from a card reader (`E:\.rockbox\`).

---

## Hardware notes (recovered from the stock firmware + kernel)

- **SoC / OS**: Ingenic X1000, Linux 3.10.14, HiByOS.
- **Display**: 360×480 smart LCD.  `/dev/fb0` is **32bpp XRGB8888** with a
  1440-byte stride and the panel is mounted **rotated 180°**.  Rockbox keeps its
  internal 16bpp RGB565 framebuffer and converts/rotates on update
  (`fb_blit_n3pro()` in `lcd-linuxfb.c`).  The framebuffer may read as all zeros
  once the backlight is off (the SLCD latches the image), so idle screenshots are
  unreliable.
- **Audio**: dual AK4493 on ALSA card 0 (`n3pro-ak4493-i2s`).  Digital filter:
  ALSA `AK4493 Digital Filter` (5 modes) + `Digital Filter`.  Tube mode is a
  **two-step** operation: power the tube via sysfs `timbre_select` first, then
  select triode/ultra-linear with the gated ALSA control `Timbre Tube Mode`.
  `cayin-n3pro.c` powers the tube only while playing and switches it off 10 s
  after pause (via the button tick).
- **Volume**: rotary encoder `sa-ring-keys` emitting `KEY_LEFT`/`KEY_RIGHT`; each
  detent is a press+release ~20 µs apart, so it is wired to the scroll wheel
  (`HAVE_SCROLLWHEEL`), not to plain buttons.
- **Touch**: GT9XX (type B multitouch); the bottom-front HOME key reports
  `KEY_MENU` and maps to `BUTTON_HOME`.
- **LED**: TI LP5562 behind a custom kernel pattern engine.  The per-channel
  `/sys/class/leds/{R,G,B,W}/brightness` files do nothing; write a preset to
  `/sys/bus/i2c/devices/1-0030/led_pattern`:
  `0 off, 1 yellow-green, 2 green, 3 blue, 4 purple, 5 white, 6 red breathing,
  7 red solid`.  Charge = 6 → 7; playback colour by sample rate.
- **USB**: the vendor `android_usb` sysfs gadget (`/sys/class/android_usb/android0`),
  *not* configfs (configfs only exposes `ecm.0` on this kernel).  Host-PCM goes
  through the vendor char device `/dev/uac_sa` (`functions=uac_sa`, or the
  composites `uac_sa,mass_storage` / `uac_sa,adb`; `uac_sa` must be first, its
  audio interfaces are hard-coded 0/1).  `enable=0 → write functions → enable=1`
  is mandatory.  Two vendor daemons interfere and are dealt with:
  `sys_server` drives the same gadget (`killall` on init) and `adbd` must be
  restarted after every function-list change (the vendor `adbserver.sh` loop
  respawns it).  A dumb charger never enumerates, so `state` containing
  `CONFIGURED` distinguishes a host from a charger.
- **Battery**: the gauge percentage comes from the kernel fuel gauge
  (`/sys/class/power_supply/battery/capacity`, `PERCENTAGE_MEASURE`) — it needs
  **no calibration**; `powermgmt-n3pro.c` only supplies voltage curves for the
  voltage readout / runtime estimate.
- **No cpufreq**: the SoC runs at a fixed frequency, so the device idles warm;
  `/proc/stat`'s idle counter is not maintained and will look like 100 % system.

---

## Applying to a newer master

```sh
git clone git://git.rockbox.org/rockbox && cd rockbox
git checkout 10ec9bd530          # patch base (safest)
git apply /path/to/n3pro_port.patch
# on a much newer tree: git apply --3way … , or --reject and fix the .rej
```

The most likely conflict points are `apps/lang/english.lang` (four entries are
inserted mid-file), `tools/configure`, `tools/builds.pm`,
`firmware/export/config.h`, the `SOURCES` files, `apps/settings_list.c`,
`apps/menus/sound_menu.c` and the 45 plugin keymaps (one `#elif` each).
Bump `MODEL_NUMBER` / menu id / `CAYIN_N3PRO_PAD` if upstream has taken them.

## Known gaps

- USB Audio in the mass-storage/ADB composites is verified on Windows only.
- Battery voltage curves are inherited from R1; the gauge itself is exact.
- `BUTTON_LEFT`/`BUTTON_RIGHT` intentionally mirror `BUTTON_PREV`/`BUTTON_NEXT`
  (affects simulator/plugins only).
