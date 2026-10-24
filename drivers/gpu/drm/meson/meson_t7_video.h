/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_VIDEO_H
#define __MESON_T7_VIDEO_H

#include <linux/types.h>

/* Progressive, even chroma crop; addresses include each DRM plane's offset. */
struct meson_t7_video_frame {
	u32 format, width, height;
	u32 x, y, crop_width, crop_height;
	u32 vskip;
	u32 pitch[2];
	u64 address[2], size[2];
};

struct meson_t7_video_mif {
	u32 base;
	u32 address[3], stride0, stride1;
	u32 luma_x, luma_y, chroma_x, chroma_y;
	u32 format_width, format_ctrl;
	u32 repeat_pattern;
	u32 gen, gen2, gen3;
	u32 gen2_mask, gen3_mask, stride1_mask;
};

struct meson_t7_video_phase {
	u32 hstep, vstep;
	u32 vphase, vreceive_repeat;
};

/* Progressive four-tap PPS, with prescaling and super-resolution disabled. */
#define MESON_T7_VIDEO_PPS_PAIRS 92
int meson_t7_video_pps_build(u32 *table, unsigned int pairs, unsigned int index,
			     u32 width, u32 height, u32 output_width, u32 output_height,
			     u32 clock_gate);

/* Progressive PPS math only; no prescaler, clipping, or super-resolution. */
int meson_t7_video_phase_prepare(u32 width, u32 height, u32 output_width,
				 u32 output_height, struct meson_t7_video_phase *phase);

/*
 * Calculate the linear NV12/NV21 MIF image without enabling or touching DMA.
 * gen has fetch disabled. The caller must own and drain the complete video
 * pipeline before applying it; these parameters alone are not a video plane.
 * Apply gen2, gen3 and stride1 through their masks, preserving unrelated bits.
 * The stereo STRIDE_1_F1 block-mode mirror is outside this progressive profile.
 */
int meson_t7_video_mif_prepare(const struct meson_t7_video_frame *frame,
			       unsigned int index, struct meson_t7_video_mif *mif);

#endif
