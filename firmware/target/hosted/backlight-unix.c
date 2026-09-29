/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2019 by Roman Stolyarov
 * Copyright (C) 2025 by Melissa Autumn
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include "config.h"
#include "backlight-target.h"
#include "sysfs.h"
#include "panic.h"
#include "lcd.h"

#if defined(BACKLIGHT_RG_NANO)
static const char * const sysfs_bl_brightness =
    "/sys/class/backlight/backlight/brightness";

static const char * const sysfs_bl_power =
    "/sys/class/backlight/backlight/bl_power";
#elif defined(BACKLIGHT_HIBY)
static const char * const sysfs_bl_brightness =
    "/sys/class/backlight/backlight_pwm0/brightness";

static const char * const sysfs_bl_power =
    /* Framebuffer powers off both touch (if available) and screen */
    "/sys/class/graphics/fb0/blank";

#else
static const char * const sysfs_bl_brightness =
    "/sys/class/backlight/pwm-backlight.0/brightness";

static const char * const sysfs_bl_power =
    "/sys/class/backlight/pwm-backlight.0/bl_power";
#endif

bool backlight_hw_init(void)
{
    backlight_hw_on();
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
#ifdef HAVE_BUTTON_LIGHT
    buttonlight_hw_on();
#ifdef HAVE_BUTTONLIGHT_BRIGHTNESS
    buttonlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
#endif
#endif
    return true;
}

static int last_bl = -1;

/* Ref: https://www.kernel.org/doc/html/latest/gpu/backlight.html#c.backlight_properties */
#define BACKLIGHT_POWER_ON 0
#define BACKLIGHT_POWER_REDUCED 1
#define BACKLIGHT_POWER_OFF 4

#if defined(CAYIN_N3PRO) && !defined(BOOTLOADER)
/*
 * Non-blocking backlight fade for the Cayin N3Pro.
 *
 * Writing the vendor brightness sysfs reprograms the panel over the LCD
 * MCU bus and stalls the entire kernel for 12-30 ms per write (worse while
 * the vendor bluetooth PCM is active).  The stock software fade performs
 * ~50 such writes from the cooperative backlight thread, freezing the
 * system for the whole fade; every audio chain with less than ~1 s of
 * slack (DLNA decoded ring 370 ms, bluetooth input ring 200 ms) breaks up
 * audibly.  Local playback survives on its multi-second pcmbuf and the
 * netfm path survives on the 200 ms ALSA hardware buffer.
 *
 * This target therefore duty-cycles the fast bl_power gate (~0.1 ms per
 * write with the fd held open) at 200 Hz from a dedicated raw thread
 * (CONFIG_BACKLIGHT_FADING_TARGET, so the stock cooperative-thread
 * stepping is compiled out).  The vendor driver keeps its parked
 * brightness: leaving the gate open at the end of a fade-up restores
 * exactly the configured level, so no slow-path write ever happens during
 * a fade.  Bare pthread: usleep() only, no Rockbox kernel calls (see the
 * port's AGENTS.md "Golden rules").  Everything in this block is
 * CAYIN_N3PRO-only; other hosted targets compile the stock code below.
 */
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <time.h>

#define BLFADE_FREQ_HZ      200     /* PWM rate, visually smooth */
#define BLFADE_UP_MS        400     /* keep < LCD_SLEEP_TIMEOUT (2 s) */
#define BLFADE_DOWN_MS      700
/* skip PWM phases shorter than this: usleep granularity vs. visual effect */
#define BLFADE_MIN_PHASE_US 400
/* a gate write slower than this means the kernel path is contended (it
 * happens while the vendor bluetooth PCM runs: ~4.5 ms per write instead
 * of 0.04-0.1 ms).  PWM cannot survive that, so the fade bails out and
 * switches directly - a clean step beats visible flicker. */
#define BLFADE_SLOW_WRITE_S 0.001

static int  blfade_fd = -1;         /* bl_power fd, held open */
static bool blfade_fade_in, blfade_fade_out;  /* user setting toggles */
static bool blfade_started;
static pthread_t blfade_tid;
static pthread_mutex_t blfade_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  blfade_cond = PTHREAD_COND_INITIALIZER;
static int  blfade_seq;             /* bump to (re)start a fade */
static int  blfade_dir;             /* +1 up, -1 down, 0 park off */
static double blfade_duty;          /* 0..1, owned by the worker */

static double blfade_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* absolute-time sleep: keeps the PWM edges punctual even when the single
 * core is loaded by the audio chains (usleep drift shows up as flicker) */
static void blfade_sleep_until(double t)
{
    struct timespec ts;
    ts.tv_sec = (time_t)t;
    ts.tv_nsec = (long)((t - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) { ts.tv_sec -= 1; ts.tv_nsec += 1000000000L; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

static void blfade_gate(const char *level)
{
    if (blfade_fd < 0)
        blfade_fd = open(sysfs_bl_power, O_WRONLY);
    if (blfade_fd >= 0)
    {
        if (write(blfade_fd, level, strlen(level)) < 0)
        {
            /* nothing sensible to do; the next phase retries */
        }
    }
}

/* gate write with latency measurement, for the PWM bailout check */
static double blfade_gate_timed(const char *level)
{
    double t0 = blfade_now();
    blfade_gate(level);
    return blfade_now() - t0;
}

static void blfade_settle(int dir)
{
    if (dir > 0)
    {
        blfade_gate("0");           /* gate open: driver restores duty */
        blfade_duty = 1.0;
    }
    else
    {
        blfade_gate("1");           /* gate closed, stock off level */
        blfade_duty = 0.0;
    }
}

static void *blfade_worker(void *arg)
{
    int seen = 0;

    (void)arg;
    prctl(PR_SET_NAME, "bl_fade", 0, 0, 0);

    /* real-time priority so the 200 Hz edges stay punctual while the audio
     * chains load the single core; the thread sleeps ~95 % of the time, so
     * this cannot starve anything.  Falls back silently when not root. */
    {
        struct sched_param sp;
        sp.sched_priority = 2;
        sched_setscheduler(0, SCHED_FIFO, &sp);
    }

    pthread_mutex_lock(&blfade_mtx);
    while (true)
    {
        int dir;

        while (blfade_seq == seen)
            pthread_cond_wait(&blfade_cond, &blfade_mtx);
        seen = blfade_seq;
        dir = blfade_dir;
        pthread_mutex_unlock(&blfade_mtx);

        if (dir == 0)
        {
            /* park with the gate closed, no fade */
            blfade_gate("1");
            blfade_duty = 0.0;
        }
        else
        {
            const double from = blfade_duty;
            const double to = (dir > 0) ? 1.0 : 0.0;
            bool done = (from == to);

            if (!done)
            {
                const double dur = ((dir > 0) ? BLFADE_UP_MS
                                              : BLFADE_DOWN_MS) * 0.001;
                const double period = 1.0 / BLFADE_FREQ_HZ;
                double t0 = blfade_now();
                double cycle = t0;
                bool bail = false;

                while (!done && !bail)
                {
                    double t = (cycle - t0) / dur;
                    if (t > 1.0)
                        t = 1.0;
                    blfade_duty = from + (to - from) * t;

                    {
                        double on_s = blfade_duty * period;
                        double off_s = (1.0 - blfade_duty) * period;

                        /* each cycle is anchored absolutely so that a
                         * preempted edge cannot shift the next ones */
                        if (on_s * 1e6 >= BLFADE_MIN_PHASE_US)
                        {
                            if (blfade_gate_timed("0") > BLFADE_SLOW_WRITE_S)
                                bail = true;
                            blfade_sleep_until(cycle + on_s);
                        }
                        if (!bail && off_s * 1e6 >= BLFADE_MIN_PHASE_US)
                        {
                            if (blfade_gate_timed("4") > BLFADE_SLOW_WRITE_S)
                                bail = true;
                            blfade_sleep_until(cycle + on_s + off_s);
                        }
                        cycle += period;
                    }

                    t = (cycle - t0) / dur;
                    done = (t >= 1.0);
                    if (!done)
                    {
                        pthread_mutex_lock(&blfade_mtx);
                        done = (blfade_seq != seen);   /* superseded */
                        pthread_mutex_unlock(&blfade_mtx);
                    }
                }
            }

            pthread_mutex_lock(&blfade_mtx);
            if (blfade_seq == seen)    /* not superseded: settle */
            {
                pthread_mutex_unlock(&blfade_mtx);
                blfade_settle(dir);
            }
            else
                pthread_mutex_unlock(&blfade_mtx);
        }

        pthread_mutex_lock(&blfade_mtx);
    }
    return NULL;
}

static void blfade_arm(int dir)
{
    pthread_mutex_lock(&blfade_mtx);
    blfade_dir = dir;
    blfade_seq++;
    pthread_cond_signal(&blfade_cond);
    pthread_mutex_unlock(&blfade_mtx);

    if (!blfade_started)
    {
        blfade_started = true;
        if (pthread_create(&blfade_tid, NULL, blfade_worker, NULL) == 0)
            pthread_detach(blfade_tid);
    }
}

/* BACKLIGHT_FADING_TARGET: the settings toggles call these directly */
void backlight_set_fade_in(bool value)
{
    blfade_fade_in = value;
}

void backlight_set_fade_out(bool value)
{
    blfade_fade_out = value;
}

void backlight_hw_on(void)
{
#ifdef HAVE_LCD_ENABLE
    lcd_enable(true);
#endif
    if (blfade_fade_in)
        blfade_arm(1);
    else
    {
        last_bl = BACKLIGHT_POWER_ON;
        sysfs_set_int(sysfs_bl_power, last_bl);
    }
}

void backlight_hw_off(void)
{
    last_bl = BACKLIGHT_POWER_REDUCED;
    if (blfade_fade_out)
    {
        /* the panel stays enabled while the LED fades; the regular lcd
         * sleep countdown (LCD_SLEEP_TIMEOUT) blanks it afterwards */
        blfade_arm(-1);
        return;
    }
    sysfs_set_int(sysfs_bl_power, last_bl);
#ifdef HAVE_LCD_ENABLE
    lcd_enable(false);
#endif
}

#else /* !CAYIN_N3PRO || BOOTLOADER: stock hosted behaviour */

void backlight_hw_on(void)
{
    if (last_bl != BACKLIGHT_POWER_ON) {
#ifdef HAVE_LCD_ENABLE
        lcd_enable(true);
#endif
        last_bl = BACKLIGHT_POWER_ON;
        sysfs_set_int(sysfs_bl_power, last_bl);
    }
}

void backlight_hw_off(void)
{
    if (last_bl != BACKLIGHT_POWER_REDUCED) {
        last_bl = BACKLIGHT_POWER_REDUCED;
        sysfs_set_int(sysfs_bl_power, last_bl);
#ifdef HAVE_LCD_ENABLE
        lcd_enable(false);
#endif
    }
}

#endif /* CAYIN_N3PRO fade engine */

void backlight_hw_brightness(int brightness)
{
    /* cap range, just in case */
    if (brightness > MAX_BRIGHTNESS_SETTING)
        brightness = MAX_BRIGHTNESS_SETTING;
    if (brightness < MIN_BRIGHTNESS_SETTING)
        brightness = MIN_BRIGHTNESS_SETTING;

    sysfs_set_int(sysfs_bl_brightness, brightness);
}

#ifdef HAVE_LCD_SLEEP
void lcd_awake(void)
{
    /* Nothing to do */
}

void lcd_sleep(void)
{
    if (last_bl != BACKLIGHT_POWER_OFF) {
        last_bl = BACKLIGHT_POWER_OFF;
        sysfs_set_int(sysfs_bl_power, last_bl);
#ifdef HAVE_LCD_ENABLE
        lcd_enable(false);
#endif
    }
}
#endif
