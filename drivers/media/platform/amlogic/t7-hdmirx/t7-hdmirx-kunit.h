/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T7_HDMIRX_KUNIT_H
#define T7_HDMIRX_KUNIT_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_KUNIT)
int t7_hdmirx_validate_edid(const u8 *edid, size_t size);
int t7_hdmirx_validate_audio(u32 tmds, u32 n, u32 cts, u8 info, u8 packet,
			     u8 channel_status, unsigned int rate);
u32 t7_vdin_lut_crc(const u32 *lut, unsigned int count, unsigned int shift);
int t7_vdin_parse_hdr_curve(char *text, u32 *tables);
#endif

#endif
