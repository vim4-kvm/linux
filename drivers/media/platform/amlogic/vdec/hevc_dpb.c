// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
#include "hevc_priv.h"

struct v4l2_rect h265_display_crop(const u16 *p)
{
	struct v4l2_rect crop = {
		.width = p[P_PIC_WIDTH],
		.height = p[P_PIC_HEIGHT],
	};
	u32 width, height;

	if (!p[P_CONFORMANCE_WINDOW_FLAG])
		return crop;
	/* Only 4:2:0 reaches the NV12 CAPTURE path. */
	width = 2U * (p[P_CONF_WIN_LEFT_OFFSET] + p[P_CONF_WIN_RIGHT_OFFSET]);
	height = 2U * (p[P_CONF_WIN_TOP_OFFSET] + p[P_CONF_WIN_BOTTOM_OFFSET]);
	/* Firmware can leave unusable offsets even when the SPS has no crop. */
	if (width >= crop.width || height >= crop.height)
		return crop;
	crop.left = 2U * p[P_CONF_WIN_LEFT_OFFSET];
	crop.top = 2U * p[P_CONF_WIN_TOP_OFFSET];
	crop.width -= width;
	crop.height -= height;
	return crop;
}

void h265_end_sequence(struct aml_hevc_ctx *h)
{
	unsigned int i;

	for (i = 0; i < H265_SLOTS; i++) {
		struct h265_pic *pic = &h->pic[i];

		/* Keep display order while immediately retiring reference identities. */
		if (pic->output && !pic->output_flush) {
			pic->output_poc = pic->poc;
			pic->output_flush = true;
		}
		pic->poc = H265_INVALID_POC;
		pic->referenced = false;
		pic->long_term = false;
	}
	h->curr_poc = H265_INVALID_POC;
	h->poc_random_access = H265_MAX_INT;
}
