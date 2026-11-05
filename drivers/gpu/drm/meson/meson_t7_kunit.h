/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MESON_T7_KUNIT_H
#define MESON_T7_KUNIT_H

#if IS_ENABLED(CONFIG_KUNIT)
u32 meson_t7_osd_alpha(u32 ctrl, u16 alpha);
u32 meson_t7_osd_reflect(u32 format, unsigned int rotation);
#endif

#endif
