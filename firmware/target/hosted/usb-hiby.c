/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/    \/
 *
 * Copyright (C) 2018 by Marcin Bukat
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
#include <sys/mount.h>
#include <string.h>
#include "config.h"
#include "disk.h"
#include "usb.h"
#include "sysfs.h"
#include "power.h"
#ifdef HAVE_HOST_USB_AUDIO
#include "pcm.h"
#include "hiby/usb-dac-hiby.h"
#endif
#include "logf.h"
int disk_mount_all(void)
{
    const char * const dev[] = {"/dev/mmcblk0p1", "/dev/mmcblk0"};
    const char * const fs[] = {"vfat", "exfat"};

    sysfs_set_string("/sys/class/android_usb/android0/f_mass_storage/lun/file", "");

    for (int i=0; i<2; i++)
    {
        for (int j=0; j<2; j++)
        {
            int rval = mount(dev[i], PIVOT_ROOT, fs[j], 0, NULL);
            if (rval == 0 || errno == -EBUSY)
            {
                logf("mount good! %d/%d %d %d", i, j, rval, errno);
#ifdef HAVE_MULTIDRIVE
                startup_rbhome();
#endif
                return 1;
            }
         }
    }

    logf("mount failed! %d", errno);
    return 0;
}

int disk_unmount_all(void)
{
#ifdef HAVE_MULTIDRIVE
    cleanup_rbhome();
#endif

    if (umount(PIVOT_ROOT) == 0)
    {
        sysfs_set_string("/sys/class/android_usb/android0/f_mass_storage/lun/file", "/dev/mmcblk0");
        logf("umount_all good");
        return 1;
    }

    logf("umount_all failed! %d", errno);
#ifdef HAVE_MULTIDRIVE
    startup_rbhome();
#endif

    return 0;
}

#if defined(CAYIN_N3PRO)
/* ------------------------------------------------------------------------
 * Cayin N3Pro
 *
 * The vendor android_usb gadget is configured through sysfs; changing the
 * function list requires the gadget to be disabled first. The structure
 * mirrors firmware/target/hosted/hiby/usb-hiby-gadget.c (R1):
 *
 *  - usb_detect() never touches the gadget or the DAC; it only reports
 *    insertion. A charger never enumerates the gadget, so "CONFIGURED"
 *    tells a real host apart from a dumb charger.
 *  - The base mode is applied by configure_usb_mode(); the DAC gadget is
 *    reconciled separately from usb_set_audio()/hiby_set_usb_mode(), the
 *    only two places R1 drives it from.
 *  - The DAC is never started before the PCM mixer is up (pcm_is_initialized)
 *    and if it fails to come up the base mode is restored.
 * ------------------------------------------------------------------------ */
static const char * const sysfs_functions = "/sys/class/android_usb/android0/functions";
static const char * const sysfs_enable    = "/sys/class/android_usb/android0/enable";
static const char * const sysfs_lun       = "/sys/class/android_usb/android0/f_mass_storage/lun/file";

static int _usb_mode  = -1;
static int _usb_audio = 0;
static bool _usb_init = false;

/* Reconfiguring the gadget invalidates the endpoints adbd holds open; the
 * daemon does not notice and turns into a zombie (interface enumerates but
 * refuses connections). The vendor adbserver.sh loop respawns it within a
 * second, so kick it here after every function-list change. */
static void restart_adbd(void)
{
    system("killall adbd 2>/dev/null");
}

/* The "USB Audio" setting: 0 never, 1 always, 2 while charge-only,
 * 3 while mass-storage (same values as R1). */
static bool dac_wanted(void)
{
    switch (_usb_audio)
    {
    case 1: return true;
    case 2: return _usb_mode == USB_MODE_CHARGE;
    case 3: return _usb_mode == USB_MODE_MASS_STORAGE;
    default: return false;
    }
}

/* Apply the base USB mode without the DAC (R1 configure_usb_mode). */
static void configure_usb_mode(int mode)
{
    sysfs_set_int(sysfs_enable, 0);

    /* Plain modes are plain composites: clear the IAD device class set by
     * the DAC gadget, or Windows mis-parses the descriptors (it binds the
     * whole device as a single ADB interface and loses everything else). */
    sysfs_set_int("/sys/class/android_usb/android0/bDeviceClass", 0);
    sysfs_set_int("/sys/class/android_usb/android0/bDeviceSubClass", 0);
    sysfs_set_int("/sys/class/android_usb/android0/bDeviceProtocol", 0);

    switch (mode)
    {
    case USB_MODE_CHARGE:
        /* charge only: export nothing and stay disabled */
        sysfs_set_string(sysfs_functions, "");
        restart_adbd();
        return;

    case USB_MODE_ADB:
        sysfs_set_string(sysfs_functions, "adb");
        sysfs_set_string("/sys/class/android_usb/android0/idVendor", "18D1");
        sysfs_set_string("/sys/class/android_usb/android0/idProduct", "D002");
        break;

    case USB_MODE_MASS_STORAGE:
    default:
        sysfs_set_string(sysfs_lun, "/dev/mmcblk0");
        sysfs_set_string(sysfs_functions, "mass_storage");
        sysfs_set_string("/sys/class/android_usb/android0/idVendor", "C502");
        sysfs_set_string("/sys/class/android_usb/android0/idProduct", "0029");
        break;
    }

    sysfs_set_int(sysfs_enable, 1);
    restart_adbd();
}

/* Bring the DAC gadget up standalone (R1 enable_usb_audio). Composite with
 * the base function, uac_sa first: its audio interfaces are hard-coded 0-1. */
static bool enable_usb_audio(void)
{
    /* Never before the PCM mixer exists (boot), like R1. */
    if (!pcm_is_initialized())
        return false;

    sysfs_set_int(sysfs_enable, 0);
    usb_dac_stop();

    switch (_usb_mode)
    {
    case USB_MODE_ADB:
        /* No IAD here: with bDeviceClass 0xEF Windows fails to parse the
         * uac_sa+adb composite. Also use the Cayin VID/PID: the Google/
         * vendor ADB INFs match 18D1:D002 at device level and preempt the
         * composite driver, which hides every other function. Windows'
         * in-box WinUSB matches the ADB interface (FF/42/01) on its own. */
        sysfs_set_string(sysfs_functions, "uac_sa,adb");
        sysfs_set_string("/sys/class/android_usb/android0/idVendor", "C502");
        sysfs_set_string("/sys/class/android_usb/android0/idProduct", "0029");
        break;

    case USB_MODE_MASS_STORAGE:
        /* composite: the card is exported AND the host gets a sound card */
        sysfs_set_string(sysfs_lun, "/dev/mmcblk0");
        sysfs_set_string(sysfs_functions, "uac_sa,mass_storage");
        sysfs_set_string("/sys/class/android_usb/android0/idVendor", "C502");
        sysfs_set_string("/sys/class/android_usb/android0/idProduct", "0029");
        /* Interface Association Descriptor, like the stock gadget / R1 */
        sysfs_set_int("/sys/class/android_usb/android0/bDeviceClass", 0xEF);
        sysfs_set_int("/sys/class/android_usb/android0/bDeviceSubClass", 2);
        sysfs_set_int("/sys/class/android_usb/android0/bDeviceProtocol", 1);
        break;

    default:
        sysfs_set_string(sysfs_functions, "uac_sa");
        sysfs_set_string("/sys/class/android_usb/android0/idVendor", "C502");
        sysfs_set_string("/sys/class/android_usb/android0/idProduct", "0029");
        break;
    }

    sysfs_set_int(sysfs_enable, 1);
    restart_adbd();

    /* Do NOT usb_dac_start() here: the vendor driver's open() blocks until a
     * host is streaming ("wait uac sa device"), which would hang whichever
     * thread applies the setting. The pump is started from usb_detect()
     * once the gadget is actually CONFIGURED. */
    return true;
}

/* Tear the DAC gadget down and restore the base mode (R1 disable_usb_audio). */
static void disable_usb_audio(void)
{
    usb_dac_stop();
    configure_usb_mode(_usb_mode < 0 ? USBMODE_DEFAULT : _usb_mode);
}

/* Reconcile the DAC with the setting + base mode (R1 apply_usb_audio). */
static void apply_usb_audio(void)
{
    bool want = dac_wanted();
    bool active = usb_audio_get_active();

    if (want && !active)
    {
        /* enable_usb_audio() returns false if applied before audio_init()
         * (PCM mixer not up yet -- e.g. at boot) or if uac_sa never comes
         * up. Either way restore the base mode so we never leave a dead
         * gadget behind. */
        if (!enable_usb_audio())
            configure_usb_mode(_usb_mode < 0 ? USBMODE_DEFAULT : _usb_mode);
    }
    else if (!want && active)
    {
        disable_usb_audio();
    }
}

void hiby_set_usb_mode(int mode)
{
    if (!_usb_init)
        usb_init_device();

    /* Drop any active DAC before reconfiguring the base gadget. */
    if (usb_audio_get_active())
        disable_usb_audio();

    _usb_mode = mode;
    configure_usb_mode(mode);
    apply_usb_audio();
}

/* Target implementation of the "USB Audio" setting. */
void usb_set_audio(int value)
{
    _usb_audio = value;
    if (_usb_mode >= 0)
        apply_usb_audio();
}

/* Re-apply the USB Audio setting once the PCM mixer is up. The setting is
 * applied at boot before audio_init(), so the first attempt bails out on
 * pcm_is_initialized() and the base mode stays; call this periodically
 * (from the N3Pro housekeeping tick) until it sticks. */
void cayin_usb_retry(void)
{
#ifdef HAVE_HOST_USB_AUDIO
    /* Fire exactly once after the PCM mixer comes up: a failing attempt
     * reconfigures the gadget, so retrying forever would flap it. */
    static bool done = false;

    if (done || _usb_audio <= 0 || _usb_mode < 0 || !pcm_is_initialized())
        return;
    if (!usb_audio_get_active() && dac_wanted())
        apply_usb_audio();
    done = true;
#endif
}

int usb_detect(void)
{
    char state[32] = {0};
    bool configured;

    /* A dumb charger supplies VBUS but never enumerates the gadget, so only
     * a CONFIGURED gadget counts as "connected to a host". */
    sysfs_get_string("/sys/class/android_usb/android0/state", state, sizeof(state));
    configured = strstr(state, "CONFIGURED") != NULL;

#ifdef HAVE_HOST_USB_AUDIO
    /* Start/stop the host-PCM pump here (USB thread context): the vendor
     * uac_sa open() blocks until a host is streaming, so it may only run
     * once the gadget is actually configured. */
    if (configured)
    {
        if (dac_wanted() && !usb_audio_get_active())
            usb_dac_start();
    }
    else if (usb_audio_get_active())
    {
        usb_dac_stop();
    }
#endif

    return configured ? USB_INSERTED : USB_EXTRACTED;
}

void usb_enable(bool on)
{
    /* Deliberately a no-op: the gadget must stay enabled across unplug so
     * the still-asserted soft-connect re-enumerates on replug without any
     * help (this is how the stock firmware behaves). Tearing it down here
     * left the device dead to the host on re-insertion, because a
     * composite including uac_sa does not come back from enable=0/1.
     * The gadget lifecycle is owned by configure_usb_mode()/the DAC
     * helpers, which reconfigure it only on mode/setting changes. */
    (void)on;
}

void usb_init_device(void)
{
    /* The vendor sys_server daemon also drives this gadget
     * (usb_mgr_set_functions) and would tear our function list back down;
     * Rockbox is the only USB manager now. */
    system("killall sys_server 2>/dev/null");

    sysfs_set_string("/sys/class/android_usb/android0/iManufacturer", "Rockbox.org");
    sysfs_set_string("/sys/class/android_usb/android0/iProduct", "Rockbox media player");
    sysfs_set_string("/sys/class/android_usb/android0/iSerial", "0123456789ABCDEF");
    sysfs_set_string("/sys/class/android_usb/android0/f_mass_storage/inquiry_string", "Rockbox 0100");
    _usb_init = true;
}

#else  /* !CAYIN_N3PRO */

/* TODO: implement usb detection properly */
int usb_detect(void)
{
    return power_input_status() == POWER_INPUT_USB_CHARGER ? USB_INSERTED : USB_EXTRACTED;
}

void usb_enable(bool on)
{
    logf("usb enable %d %d\n", on, adb_mode);

    /* Ignore usb enable/disable when ADB is enabled so we can fireup adb shell
     * without entering ums mode
     */
    if (!adb_mode)
    {
        sysfs_set_int("/sys/class/android_usb/android0/enable", on ? 1 : 0);
    }
}

void usb_init_device(void)
{
    char functions[32] = {0};

    /* Check if ADB was activated in bootloader */
    sysfs_get_string("/sys/class/android_usb/android0/functions", functions, sizeof(functions));
    adb_mode = (strstr(functions, "adb") == NULL) ? false : true;

    usb_enable(false);

    if (adb_mode)
    {
        sysfs_set_string("/sys/class/android_usb/android0/functions", "mass_storage,adb");
        sysfs_set_string("/sys/class/android_usb/android0/idVendor", "18D1");
        sysfs_set_string("/sys/class/android_usb/android0/idProduct", "D002");
    }
    else
    {
        sysfs_set_string("/sys/class/android_usb/android0/functions", "mass_storage");
        sysfs_set_string("/sys/class/android_usb/android0/idVendor", "C502");
        sysfs_set_string("/sys/class/android_usb/android0/idProduct", "0029");
    }

    sysfs_set_string("/sys/class/android_usb/android0/iManufacturer", "Rockbox.org");
    sysfs_set_string("/sys/class/android_usb/android0/iProduct", "Rockbox media player");
    sysfs_set_string("/sys/class/android_usb/android0/iSerial", "0123456789ABCDEF");
    sysfs_set_string("/sys/class/android_usb/android0/f_mass_storage/inquiry_string", "Rockbox 0100");
}

#endif /* CAYIN_N3PRO */
