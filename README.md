# Rockbox for the Cayin N3Pro

A **hosted** Rockbox port for the Cayin N3Pro digital audio player
(Ingenic X1000 "MIPS XBurst", Linux 3.10, HiByOS).

Rockbox runs as a normal Linux application on top of the stock HiByOS.  The
boot loader, kernel and the stock player binary are left untouched, so the
installation is fully reversible and cannot brick the device.

## Status

| Feature | State |
|---|---|
| Boot (Rockbox launcher menu + fallback to stock player) | working |
| LCD (360x480, 32bpp XRGB8888 SLCD, 180° rotated) | working |
| Buttons (power / prev / play / next) | working |
| Touchscreen (GT9xx) + HOME key | working |
| Volume wheel (rotary encoder -> scroll wheel) | working |
| Audio (AK4493 via ALSA) | working |
| Headphone detection | working |
| SD card hotplug (`/mnt/sd_0`) | working |
| ADB over USB (for debugging) | working |
| DAC filter roll-off + tube mode settings | working |
| Backlight | working |

## Hardware notes

* SoC: Ingenic X1000, Linux 3.10.14, HiByOS.
* Display: 360x480 smart LCD (SLCD); the kernel framebuffer is **32bpp
  XRGB8888** with a 1440-byte stride, and the panel is mounted rotated 180°.
  Rockbox keeps its internal 16bpp RGB565 framebuffer and converts/rotates on
  update (`firmware/target/hosted/lcd-linuxfb.c`).
* Audio: dual AK4493 DACs on ALSA card 0 (`n3pro-ak4493-i2s`).  Tube mode is a
  two-step operation: power the tube via sysfs `timbre_select`, then select the
  wiring (triode / ultra-linear) through the `Timbre Tube Mode` ALSA control.
  The tube is powered only while playing and switched off 10 s after pause.
* Volume: a rotary encoder (`sa-ring-keys`) that emits `KEY_LEFT`/`KEY_RIGHT`;
  it is wired to Rockbox's scroll-wheel mechanism (`HAVE_SCROLLWHEEL`).

## Files

New target files live under `firmware/target/hosted/cayin/n3pro/` and
`firmware/export/config/n3pro.h`; the keymap is `apps/keymaps/keymap-n3pro.c`.
Registration is in `tools/configure`, `tools/builds.pm`, `firmware/SOURCES`,
`apps/SOURCES` and `apps/bitmaps/native/SOURCES`.

* `n3pro_patcher.sh` - unpack / inject / pack a Rockbox `.upt` image.

## Building

Use the shared MIPS hosted toolchain (`mipsel-rockbox-linux-gnu`, built by
`tools/rockboxdev.sh`), then:

    mkdir build && cd build
    ../tools/configure --target=n3pro --ram=16 --rbdir=/.rockbox --type=N
    make -j
    make fullzip          # SD-card payload (.rockbox with fonts + theme)

For the boot loader:

    mkdir build-bl && cd build-bl
    ../tools/configure --target=n3pro --ram=16 --rbdir=/.rockbox --type=B
    make -j

## Packaging

`n3pro_patcher.sh` builds the installation image from a stock update file:

    ./n3pro_patcher.sh n3pro.upt bootloader.n3pro

It also supports `--unpack`, `--inject-app` and `--pack` individually.

## Installing

1. Unpack `rockbox-*.zip` to the root of a FAT32 microSD card, so that a
   `.rockbox` directory appears there.
2. Put the patched `.upt` file on the card.
3. Power off, then hold the **play** button and press **power** to enter the
   update mode.  The updater flashes the image automatically.
4. On boot the Rockbox boot loader menu appears; pick `ROCKBOX` or
   `CAYIN PLAYER` (the stock player).

To revert, simply flash the stock `.upt`.

## License

GPLv2 or later, same as Rockbox.  See `docs/COPYING` in the Rockbox tree.
