// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019 Amlogic, Inc. All rights reserved. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#include "meson_t7_video.h"

static const u32 t7_video_bicubic[] = {
	0x00800000, 0x007f0100, 0xff7f0200, 0xfe7f0300, 0xfd7e0500, 0xfc7e0600,
	0xfb7d0800, 0xfb7c0900, 0xfa7b0b00, 0xfa7a0dff, 0xf9790fff, 0xf97711ff,
	0xf87613ff, 0xf87416fe, 0xf87218fe, 0xf8701afe, 0xf76f1dfd, 0xf76d1ffd,
	0xf76b21fd, 0xf76824fd, 0xf76627fc, 0xf76429fc, 0xf7612cfc, 0xf75f2ffb,
	0xf75d31fb, 0xf75a34fb, 0xf75837fa, 0xf7553afa, 0xf8523cfa, 0xf8503ff9,
	0xf84d42f9, 0xf84a45f9, 0xf84848f8,
};

/* VD1 has a hole at 0x1d0f; VD2's vertical and horizontal banks are contiguous. */
static const struct {
	u32 coef, vertical, horizontal, misc, phase1, gate, pre;
} t7_pps[] = {
	{ 0x1d03, 0x1d05, 0x1d10, 0x1d19, 0x1d34, 0x1d36, 0x1d38 },
	{ 0x3933, 0x3935, 0x393f, 0x3948, 0x394a, 0x394c, 0x394e },
};

static void t7_video_pps_write(u32 **table, u32 reg, u32 value)
{
	*(*table)++ = reg;
	*(*table)++ = value;
}

int meson_t7_video_pps_build(u32 *table, unsigned int pairs, unsigned int index,
			     u32 width, u32 height, u32 output_width, u32 output_height,
			     u32 clock_gate)
{
	struct meson_t7_video_phase phase;
	u32 *next = table;
	u32 vertical, horizontal;
	unsigned int axis, i;
	int ret;

	if (!table || index >= ARRAY_SIZE(t7_pps))
		return -EINVAL;
	ret = meson_t7_video_phase_prepare(width, height, output_width, output_height, &phase);
	if (ret)
		return ret;
	if (pairs < MESON_T7_VIDEO_PPS_PAIRS)
		return -ENOSPC;
	vertical = t7_pps[index].vertical;
	horizontal = t7_pps[index].horizontal;
	/* Program the complete progressive profile before reconnecting its output. */
	t7_video_pps_write(&next, t7_pps[index].misc, 0);
	t7_video_pps_write(&next, t7_pps[index].gate, clock_gate & ~GENMASK(15, 0));
	/* Four-tap 8-bit coefficients; neither pre-H nor pre-V is selected in SC_MISC. */
	t7_video_pps_write(&next, t7_pps[index].pre, 0x77 << 12);
	t7_video_pps_write(&next, horizontal + 8, 4 << 16 | 1 << 20);
	t7_video_pps_write(&next, t7_pps[index].phase1, 4 << 16 | 1 << 20);
	t7_video_pps_write(&next, vertical + 8, phase.vreceive_repeat);
	t7_video_pps_write(&next, vertical + 9, phase.vphase);
	t7_video_pps_write(&next, vertical, 0);
	t7_video_pps_write(&next, vertical + 1, (output_height - 1) << 16 |
			   (output_height - 1));
	t7_video_pps_write(&next, vertical + 2, output_height - 1);
	t7_video_pps_write(&next, vertical + 3, phase.vstep);
	for (i = 4; i < 8; i++)
		t7_video_pps_write(&next, vertical + i, 0);
	t7_video_pps_write(&next, horizontal, 0);
	t7_video_pps_write(&next, horizontal + 1, (output_width - 1) << 16 |
			   (output_width - 1));
	t7_video_pps_write(&next, horizontal + 2, output_width - 1);
	t7_video_pps_write(&next, horizontal + 3, phase.hstep);
	for (i = 4; i < 8; i++)
		t7_video_pps_write(&next, horizontal + i, 0);
	for (axis = 0; axis < 2; axis++) {
		t7_video_pps_write(&next, t7_pps[index].coef, axis ? BIT(8) : 0);
		for (i = 0; i < ARRAY_SIZE(t7_video_bicubic); i++)
			t7_video_pps_write(&next, t7_pps[index].coef + 1, t7_video_bicubic[i]);
	}
	/* Last-line fix, H/V scaler, top, V1 output, and two four-sample banks. */
	t7_video_pps_write(&next, t7_pps[index].misc,
			   BIT(24) | BIT(18) | BIT(17) | BIT(16) | BIT(15) | 4 << 8 | 4);
	return (next - table) * sizeof(*table);
}
