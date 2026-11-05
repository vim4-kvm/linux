/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T7_JPEG_KUNIT_H
#define T7_JPEG_KUNIT_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_KUNIT)
u32 make_header(u8 *header, u32 width, u32 height, u16 quant[2][64]);
void t7_jpeg_quantize(u16 quant[2][64], u32 quality);
#endif

#endif
