/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MEDIA_MESON_T7_DOS_H
#define _MEDIA_MESON_T7_DOS_H

#include <linux/device.h>

/*
 * JPEG and Wave may coexist. An experimental VDEC owns all three engines
 * exclusively, including their probe/firmware/reset and removal phases.
 * A quarantined decoder must keep its lease until a system reset.
 */
int meson_t7_dos_get(struct device *dev, bool exclusive);
int devm_meson_t7_dos_get_shared(struct device *dev);
void meson_t7_dos_put(bool exclusive);
void meson_t7_dos_poison(void);

#endif
