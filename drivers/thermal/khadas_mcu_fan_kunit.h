/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KHADAS_MCU_FAN_KUNIT_H
#define KHADAS_MCU_FAN_KUNIT_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_KUNIT)
int khadas_vim4_fan_target_curve(int temp, bool sensor_fault, int low, int mid, int high);
int khadas_vim4_fan_target(int temp, bool sensor_fault);
unsigned int khadas_vim4_fan_next(unsigned int current_speed, unsigned int target);
#endif

#endif
