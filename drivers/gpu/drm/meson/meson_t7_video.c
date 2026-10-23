// SPDX-License-Identifier: GPL-2.0-only
/* T7 video input parameters. No MMIO or fetch enable is performed here. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <drm/drm_fourcc.h>

#include "meson_t7_video.h"

int meson_t7_video_phase_prepare(u32 width, u32 height, u32 output_width,
				 u32 output_height, struct meson_t7_video_phase *phase)
{
	struct meson_t7_video_phase next = {};
	u32 ratio, offset;

	if (!phase || !width || !height || !output_width || !output_height ||
	    width > 3840 || output_width > 3840 || height > 2160 || output_height > 2160 ||
	    width > output_width * 4 || output_width > width * 4 ||
	    height > output_height * 4 || output_height > height * 4)
		return -EINVAL;
	/* The vendor truncates the 18-bit ratio before expanding to 24 bits. */
	next.hstep = div_u64((u64)width << 18, output_width) << 6;
	ratio = div_u64((u64)height << 18, output_height);
	next.vstep = ratio << 6;
	/* f2v_get_vertical_phase(): progressive downscaling shifts output by 1/4. */
	offset = ratio > BIT(18) ? ratio >> 10 : 0;
	next.vphase = ((offset & 0x3ff) >> 2) << 8;
	/* skip_tab[0/1]: receive four samples, repeat one/zero initial samples. */
	next.vreceive_repeat = offset >= 0x400 ? 0x04 : 0x24;
	*phase = next;
	return 0;
}

int meson_t7_video_mif_prepare(const struct meson_t7_video_frame *frame,
			       unsigned int index, struct meson_t7_video_mif *mif)
{
	struct meson_t7_video_mif next = {};
	u32 right, bottom;
	unsigned int i;

	if (!frame || !mif || index > 1)
		return -EINVAL;
	if (frame->format != DRM_FORMAT_NV12 && frame->format != DRM_FORMAT_NV21)
		return -EINVAL;
	if (!frame->width || !frame->height || frame->width > 3840 || frame->height > 2160 ||
	    !frame->crop_width || !frame->crop_height ||
	    ((frame->width | frame->height | frame->x | frame->y |
	      frame->crop_width | frame->crop_height) & 1) ||
	    frame->x > frame->width || frame->crop_width > frame->width - frame->x ||
	    frame->y > frame->height || frame->crop_height > frame->height - frame->y)
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		u64 bytes = (u64)frame->pitch[i] * (frame->height >> i);

		if (frame->pitch[i] < frame->width || frame->pitch[i] > 0xfff * 16 ||
		    (frame->pitch[i] & 31) || (frame->address[i] & 15))
			return -EINVAL;
		if (bytes > frame->size[i] || frame->address[i] > U32_MAX ||
		    bytes - 1 > U32_MAX - frame->address[i])
			return -ERANGE;
		next.address[i] = frame->address[i] >> 4;
	}
	right = frame->x + frame->crop_width - 1;
	bottom = frame->y + frame->crop_height - 1;
	next.base = 0x4800 + index * 0x80;
	next.stride0 = (frame->pitch[0] >> 4) | (frame->pitch[1] >> 4) << 16;
	/* set_vd_mif_linear() writes only 16 bits, despite its bit16 expression. */
	next.stride1 = frame->pitch[1] >> 4;
	next.stride1_mask = GENMASK(15, 0);
	next.luma_x = frame->x | right << 16;
	next.luma_y = frame->y | bottom << 16;
	next.chroma_x = (frame->x >> 1) | (right >> 1) << 16;
	next.chroma_y = (frame->y >> 1) | (bottom >> 1) << 16;
	next.format_width = frame->crop_width << 16 | frame->crop_width >> 1;
	/* Progressive 4:2:0: H 2:1, V phase 8/16, initial phase 12, repeat line 0. */
	next.format_ctrl = BIT(29) | BIT(21) | BIT(20) | BIT(16) | 12 << 8 | 8 << 1 | BIT(0);
	/* Urgent Y/C, eight hold lines, repeat chroma, linear separate planes. */
	next.gen = 3 << 27 | 8 << 19 | BIT(6) | BIT(4) | BIT(1);
	/* vd1_set_dcu() resets on field; vdx_set_dcu() deliberately does not. */
	if (!index)
		next.gen |= BIT(29);
	next.gen2 = frame->format == DRM_FORMAT_NV12 ? 2 : 1;
	next.gen2_mask = GENMASK(5, 0);
	/* 8-bit, no block tiling or canvas; burst follows the luma pitch. */
	next.gen3 = (frame->pitch[0] & 63) ? 1 << 1 : 2 << 1;
	next.gen3_mask = GENMASK(2, 0) | GENMASK(9, 8) | GENMASK(15, 12) | GENMASK(26, 18);
	*mif = next;
	return 0;
}
