/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2016 Amaury Pouly
 * Copyright (C) 2019 Roman Stolyarov
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

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include "lcd.h"
#include "lcd-target.h"
#include "backlight-target.h"
#include "sysfs.h"
#include "panic.h"

//#define FB_DOUBLEBUF

static int fd = -1;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
fb_data *framebuffer = NULL; /* global variable, see lcd-target.h */

#ifdef FB_DOUBLEBUF
static int fb_plane;
int doublebuf;
#else
#define fb_plane 0
#endif

static void redraw(void)
{
#ifdef FB_DOUBLEBUF
    if (doublebuf) {
        vinfo.yoffset = fb_plane * LCD_HEIGHT;
        fb_plane = !fb_plane;
        vinfo.activate = FB_ACTIVATE_VBL;
    }
#endif

    ioctl(fd, FBIOPAN_DISPLAY, &vinfo);
}

void lcd_init_device(void)
{
    const char * const fb_dev = "/dev/fb0";
    fd = open(fb_dev, O_RDWR | O_CLOEXEC);
    if(fd < 0)
    {
        panicf("Cannot open framebuffer: %s\n", fb_dev);
    }

    /* get fixed information */
    if(ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0)
    {
        panicf("Cannot read framebuffer fixed information");
    }

    if (finfo.smem_len < FRAMEBUFFER_SIZE)
        panicf("FRAMEBUFFER_SIZE too large for hardware? (%u vs %u)", FRAMEBUFFER_SIZE, finfo.smem_len);
#ifdef FB_DOUBLEBUF
    if (finfo.smem_len >= 2*FRAMEBUFFER_SIZE)
        doublebuf = 1;
    fb_plane = 0;
#endif

    /* get variable information */
    if(ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
    {
        panicf("Cannot read framebuffer variable information");
    }

#if defined(CAYIN_N3PRO)
    /* N3Pro uses a fixed 32bpp Ingenic SLCD and the mode cannot be changed.
     * Leave it as-is; the blit converts our 16bpp buffer to the panel format. */
#elif defined(FB_DOUBLEBUF)
    if (doublebuf) {
        vinfo.yres_virtual = vinfo.yres * 2;
        if(ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo) < 0) {
            panicf("Cannot write framebuffer variable information");
        }
    }

#if 0
    /* check resolution and framebuffer size */
    if(vinfo.xres != LCD_WIDTH || vinfo.yres != LCD_HEIGHT || vinfo.bits_per_pixel != LCD_DEPTH)
    {
        panicf("Unexpected framebuffer resolution: %dx%dx%d\n", vinfo.xres,
            vinfo.yres, vinfo.bits_per_pixel);
    }
#endif
#elif defined(FB_STRIDE_MISMATCH)
    /* For framebuffers wider than the panel and cannot be resized */
    if (vinfo.bits_per_pixel != LCD_DEPTH)
        panicf("Unexpected framebuffer depth: %d", vinfo.bits_per_pixel);
#else
    /* Make sure we match our desired bitdepth */
    if (vinfo.bits_per_pixel != LCD_DEPTH || vinfo.xres != LCD_WIDTH || vinfo.yres != LCD_HEIGHT) {
        vinfo.bits_per_pixel = LCD_DEPTH;
        vinfo.xres = LCD_WIDTH;
        vinfo.yres = LCD_HEIGHT;
        if (ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo)) {
            panicf("Cannot set framebuffer to %dx%dx%d",
               vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
        }
    }
#endif

    /* map framebuffer */
    framebuffer = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if((void *)framebuffer == MAP_FAILED)
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
    munmap(framebuffer, FRAMEBUFFER_SIZE);
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

extern void lcd_copy_buffer_rect(fb_data *dst, const fb_data *src,
                                 int width, int height);

#ifdef FB_STRIDE_MISMATCH
/* lcd_copy_buffer_rect() for framebuffers wider than the panel */
static void fb_copy_rect(fb_data *dst, const fb_data *src,
                         int width, int height)
{
    while (height--)
    {
        memcpy(dst, src, width * sizeof(fb_data));
        dst += FB_STRIDE_MISMATCH;
        src += LCD_WIDTH;
    }
}
#endif

#if defined(CAYIN_N3PRO)
/* The N3Pro panel is a 32bpp XRGB8888 Ingenic SLCD whose scan order is
 * reversed (180-degree).  Convert Rockbox's 16bpp RGB565 framebuffer into the
 * panel format and write it rotated so the image appears upright. */
static void fb_blit_n3pro(const fb_data *src_base, int x, int y,
                          int width, int height)
{
    unsigned int *dst = (unsigned int *)framebuffer;

    for (int j = 0; j < height; j++)
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
#endif

void lcd_update(void)
{
    if (fd < 0) return;

#ifdef HAVE_LCD_ENABLE
    if (lcd_active())
#endif
    {
#if defined(CAYIN_N3PRO)
        fb_blit_n3pro(FBADDR(0, 0), 0, 0, LCD_WIDTH, LCD_HEIGHT);
#else
        /* Copy the Rockbox framebuffer to the second framebuffer */
#ifdef FB_STRIDE_MISMATCH
        /* Strides differ - not one contiguous run */
        fb_copy_rect(LCD_FRAMEBUF_ADDR(0, 0), FBADDR(0, 0),
                     LCD_WIDTH, LCD_HEIGHT);
#else
        fb_data *dst = LCD_FRAMEBUF_ADDR(0, 0) + (fb_plane * FRAMEBUFFER_SIZE);
        lcd_copy_buffer_rect(dst, FBADDR(0,0),
                             LCD_WIDTH*LCD_HEIGHT, 1);
#endif
#endif
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
#if defined(CAYIN_N3PRO)
        fb_blit_n3pro(FBADDR(0, 0), x, y, width, height);
#else
#ifdef FB_STRIDE_MISMATCH
        /* Strides differ - do line-by-line */
        fb_copy_rect(LCD_FRAMEBUF_ADDR(x, y), FBADDR(x, y), width, height);
#else
        fb_data *dst = LCD_FRAMEBUF_ADDR(x, y) + (fb_plane * FRAMEBUFFER_SIZE);
        fb_data *src = FBADDR(x,y);

        /* Copy part of the Rockbox framebuffer to the second framebuffer */
        if (width < LCD_WIDTH)
        {
            /* Not full width - do line-by-line */
            lcd_copy_buffer_rect(dst, src, width, height);
        }
        else
        {
            /* Full width - copy as one line */
            lcd_copy_buffer_rect(dst, src, LCD_WIDTH*height, 1);
        }
#endif
#endif
        redraw();
    }
}
