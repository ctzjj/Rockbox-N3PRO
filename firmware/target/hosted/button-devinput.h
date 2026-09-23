/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by Roman Artiukhin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 ****************************************************************************/


#ifndef __BUTTON_DEVINPUT_H__
#define __BUTTON_DEVINPUT_H__

#include <stdbool.h>

/* Highest /dev/input/event<N> index polled.  Must cover every node that
 * may be wired in at runtime (e.g. a Bluetooth peer's AVRCP device that
 * only exists while the link is up). */
#define NR_POLL_DESC 10

void button_close_device(void);
void button_add_input_device(int i);
void button_remove_input_device(int i);
bool button_input_device_is_open(int i);

#endif
