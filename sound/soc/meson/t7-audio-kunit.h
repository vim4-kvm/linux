/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T7_AUDIO_KUNIT_H
#define T7_AUDIO_KUNIT_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_KUNIT)
bool t7_aed_biquad_valid(const u32 *coeff);
bool t7_aed_drc_valid(const u32 *drc, unsigned int words);
#endif

#endif
