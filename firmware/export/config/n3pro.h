/*
 * This config file is for the Cayin N3Pro
 */

/* For Rolo and boot loader */
#define MODEL_NUMBER 126
#define MODEL_NAME   "Cayin N3Pro"

#define PIVOT_ROOT "/mnt/sd_0"
#define MULTIDRIVE_DIR "/mnt/usb"

/* LCD dimensions */
/* 360x480 portrait touchscreen */
#define LCD_WIDTH  360
#define LCD_HEIGHT 480
/* sqrt(360^2 + 480^2) / 3.5 = 171 */
#define LCD_DPI 171

#define HAVE_LCD_SLEEP
#define LCD_SLEEP_TIMEOUT (2*HZ)

/* Internal Rockbox depth stays 16bpp/RGB565 (like R1).  The N3Pro panel is a
 * 32-bit Ingenic SLCD, so lcd-linuxfb converts 16bpp->XRGB8888 at blit time. */
#define LCD_DEPTH  16
#define LCD_PIXELFORMAT RGB565

#define CPU_FREQ           1008000000

/* define this if you have access to the quickscreen */
#define HAVE_QUICKSCREEN
#define HAVE_HOTKEY

#define HAVE_HEADPHONE_DETECTION
#define NO_BUTTON_LR

/* RGB indicator (TI LP5562 via the led_pattern sysfs node) */
#define HAVE_GENERAL_PURPOSE_LED

#ifndef BOOTLOADER
#define HAVE_BUTTON_DATA
#define HAVE_TOUCHSCREEN
#define HAVE_KBD_POINT_MODE
#endif

/* KeyPad configuration for plugins */
#define CONFIG_KEYPAD CAYIN_N3PRO_PAD

/* define this if the target has volume keys which can be used in the lists */
#define HAVE_VOLUME_IN_LIST

/* The volume control is a rotary encoder (sa-ring-keys -> KEY_LEFT/KEY_RIGHT).
 * It emits press+release within microseconds, so route it through Rockbox's
 * queueing scroll-wheel path instead of the plain button bitmap. */
#define HAVE_SCROLLWHEEL

/* Battery */
#define BATTERY_TYPES_COUNT  1

/* Audio codec */
#define HAVE_HIBY_LINUX_CODEC

/* We don't have hardware controls */
#define HAVE_SW_TONE_CONTROLS

/* HW codec is flexible */
#define HW_SAMPR_CAPS SAMPR_CAP_ALL_192

/* Battery */
#define CONFIG_BATTERY_MEASURE (VOLTAGE_MEASURE|PERCENTAGE_MEASURE|TIME_MEASURE)

#define BATTERY_CAPACITY_DEFAULT 4000 /* default battery capacity */
#define BATTERY_CAPACITY_MIN 4000  /* min. capacity selectable */
#define BATTERY_CAPACITY_MAX 4000 /* max. capacity selectable */
#define BATTERY_CAPACITY_INC 0   /* capacity increment */

#define MIN_BRIGHTNESS_SETTING      1
#define MAX_BRIGHTNESS_SETTING      255
#define BRIGHTNESS_STEP             5
#define DEFAULT_BRIGHTNESS_SETTING  255

/* ROLO */
#define BOOTFILE_EXT "n3pro"
#define BOOTFILE     "rockbox." BOOTFILE_EXT
#define BOOTDIR      "/.rockbox"

/* USB */
#define HAVE_USB_ADB
#define USB_VID_STR "C502"
#define USB_PID_STR "0029"
#define HAVE_HOST_USB_AUDIO

/* Generic HiBy stuff */
#include "hibylinux.h"

/* start the ADB gadget automatically in the bootloader */
#define AUTO_ENABLE_ADB
