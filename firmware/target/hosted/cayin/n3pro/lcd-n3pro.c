/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 ctzjj
 * Based on lcd-linuxfb.c (C) 2016 Amaury Pouly
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Self-contained framebuffer driver for the Cayin N3Pro.
 *
 * The panel is a 360x480 panel scanned straight out of the framebuffer by
 * the Ingenic LCDC at 60 Hz.  The vendor kernel fixes /dev/fb0 at 32bpp
 * XRGB8888 (FBIOPUT_VSCREENINFO rejects other depths) and mounts the panel
 * rotated 180 degrees, so every update converts Rockbox's 16bpp RGB565
 * framebuffer and writes it rotated.
 *
 * There is no blanking interval and the fbdev driver supports neither page
 * flipping (yres_virtual is clamped to yres) nor FBIO_WAITFORVSYNC, so
 * writing the visible buffer tears whenever the scan beam crosses the rows
 * being written.  The panel TE signal is wired to a GPIO which the kernel
 * counts as the "slcd_vsync" interrupt (~60 Hz), though: we lock onto that
 * counter and write in scan order, staying ahead of the beam. */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include "lcd.h"
#include "lcd-target.h"
#include "sysfs.h"
#include "panic.h"

static int fd = -1;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
fb_data *framebuffer = NULL; /* global variable, see lcd-target.h */

static void redraw(void)
{
    ioctl(fd, FBIOPAN_DISPLAY, &vinfo);
}

/* ---- scan-beam synchronisation --------------------------------------- */

static long long n3pro_frame_ns;
static long long n3pro_next_vsync_ns;

static long long n3pro_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Current value of the slcd_vsync interrupt counter, -1 if unavailable. */
static long long n3pro_vsync_count(void)
{
    char buf[2048];
    int fd = open("/proc/interrupts", O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    const char *p = strstr(buf, "slcd_vsync");
    if (!p)
        return -1;
    while (p > buf && p[-1] != '\n')
        p--;                          /* start of the interrupt line */
    p = strchr(p, ':');
    if (!p)
        return -1;
    return strtoll(p + 1, NULL, 10);
}

/* Block until the next panel frame start (best effort; if the sync source
 * disappears we simply return and the blit runs unsynchronised).  The frame
 * period is predicted from the panel timing and re-locked at every edge, so
 * the tight-poll window stays around a millisecond. */
static void n3pro_wait_vsync(void)
{
    long long c0, t0, now, d;

    now = n3pro_now_ns();
    d = n3pro_next_vsync_ns - now;
    if (d > 400000)
    {
        if (d > 20000000LL)
            d = 20000000LL;           /* stale prediction: cap the sleep */
        usleep((d - 400000) / 1000);
    }

    c0 = n3pro_vsync_count();
    if (c0 < 0)
        return;
    t0 = n3pro_now_ns();
    while (n3pro_vsync_count() == c0)
    {
        now = n3pro_now_ns();
        if (now - t0 > 30000000LL)
        {
            n3pro_next_vsync_ns = now + n3pro_frame_ns;
            return;                   /* lost sync: write unsynchronised */
        }
    }
    n3pro_next_vsync_ns = n3pro_now_ns() + n3pro_frame_ns;
}

/* ---- blit ------------------------------------------------------------- */

/* Convert Rockbox's 16bpp RGB565 framebuffer into the panel's 32bpp
 * XRGB8888 format and write it rotated 180 degrees so the image appears
 * upright.  Rows are written in panel scan order (fb row 0 first) so that,
 * combined with n3pro_wait_vsync(), the blit (~9 ms/frame) stays ahead of
 * the scan beam (~35 us/row). */
static void fb_blit_n3pro(const fb_data *src_base, int x, int y,
                          int width, int height)
{
    unsigned int *dst = (unsigned int *)framebuffer;

    for (int j = height - 1; j >= 0; j--)
    {
        const fb_data *srow = src_base + (long)(y + j) * LCD_WIDTH + x;
        /* rotated destination row, walked backwards */
        unsigned int *drow = dst + (long)(LCD_HEIGHT - 1 - y - j) * LCD_WIDTH
                               + (LCD_WIDTH - 1 - x);

        for (int i = 0; i < width; i++)
        {
            unsigned short c = srow[i];
            unsigned r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
            *drow-- = ((r << 3 | r >> 2) << 16)
                    | ((g << 2 | g >> 4) << 8)
                    |  (b << 3 | b >> 2);
        }
    }
}

/* ---- driver API ------------------------------------------------------- */

void lcd_init_device(void)
{
    const char * const fb_dev = "/dev/fb0";
    fd = open(fb_dev, O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        panicf("Cannot open framebuffer: %s\n", fb_dev);
    }

    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0)
    {
        panicf("Cannot read framebuffer fixed information");
    }

    if (finfo.smem_len < LCD_WIDTH * LCD_HEIGHT * 4)
        panicf("Framebuffer too small (%u bytes)", finfo.smem_len);

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
    {
        panicf("Cannot read framebuffer variable information");
    }

    /* Frame period from the panel timing (sync margins are zero). */
    n3pro_frame_ns = (long long)vinfo.pixclock
                     * (vinfo.xres + vinfo.left_margin + vinfo.right_margin
                        + vinfo.hsync_len)
                     * (vinfo.yres + vinfo.upper_margin + vinfo.lower_margin
                        + vinfo.vsync_len) / 1000;

    framebuffer = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    if ((void *)framebuffer == MAP_FAILED)
    {
        panicf("Cannot map framebuffer");
    }

    memset(framebuffer, 0, finfo.smem_len);

#ifdef HAVE_LCD_ENABLE
    lcd_set_active(true);
#endif
}

#ifdef HAVE_LCD_SHUTDOWN
void lcd_shutdown(void)
{
    munmap(framebuffer, finfo.smem_len);
    framebuffer = NULL;
    close(fd);
    fd = -1;
}
#endif

#ifdef HAVE_LCD_ENABLE
void lcd_enable(bool on)
{
    if (fd < 0) return;

    if (lcd_active() == on)
        return;

    lcd_set_active(on);

    if (on) {
        send_event(LCD_EVENT_ACTIVATION, NULL);
        ioctl(fd, FB_BLANK_UNBLANK);
    } else {
        memset(framebuffer, 0, finfo.smem_len);
        redraw();
        ioctl(fd, FB_BLANK_POWERDOWN);
    }
}
#endif

void lcd_update(void)
{
    if (fd < 0) return;

#ifdef HAVE_LCD_ENABLE
    if (lcd_active())
#endif
    {
        n3pro_wait_vsync();           /* chase the scan beam (full screen) */
        fb_blit_n3pro(FBADDR(0, 0), 0, 0, LCD_WIDTH, LCD_HEIGHT);
        redraw();
    }
}

void lcd_update_rect(int x, int y, int width, int height)
{
    if (fd < 0) return;

#ifdef HAVE_LCD_ENABLE
    if (lcd_active())
#endif
    {
        if (width * height >= LCD_WIDTH * LCD_HEIGHT / 8)
            n3pro_wait_vsync();       /* chase the beam on big updates */
        fb_blit_n3pro(FBADDR(0, 0), x, y, width, height);
        redraw();
    }
}
