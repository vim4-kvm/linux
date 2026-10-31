/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
#ifndef WAVE5_HDR10_H
#define WAVE5_HDR10_H

/* Annex B prefix SEI for the standard V4L2 HDR10 compound controls.
 * NULL means absent; an all-zero CLL is still present. Mastering luminance
 * already uses 0.0001 cd/m2 in both the control and HEVC syntax.
 * This serializer is not yet connected to the firmware's SEI DMA command.
 */
static unsigned int wave5_hdr10_sei(u8 data[80],
				    const struct v4l2_ctrl_hdr10_mastering_display *mastering,
				    const struct v4l2_ctrl_hdr10_cll_info *cll)
{
	u8 rbsp[33];
	unsigned int size = 0, out = 0, zeros = 0, i;
	u32 value;

	if (!mastering && !cll)
		return 0;
	if (mastering) {
		rbsp[size++] = 137;
		rbsp[size++] = 24;
		for (i = 0; i < 8; i++) {
			if (i < 6)
				value = i & 1 ? mastering->display_primaries_y[i / 2] :
					mastering->display_primaries_x[i / 2];
			else
				value = i & 1 ? mastering->white_point_y : mastering->white_point_x;
			rbsp[size++] = value >> 8;
			rbsp[size++] = value;
		}
		for (i = 0; i < 2; i++) {
			value = i ? mastering->min_display_mastering_luminance :
				mastering->max_display_mastering_luminance;
			rbsp[size++] = value >> 24;
			rbsp[size++] = value >> 16;
			rbsp[size++] = value >> 8;
			rbsp[size++] = value;
		}
	}
	if (cll) {
		rbsp[size++] = 144;
		rbsp[size++] = 4;
		rbsp[size++] = cll->max_content_light_level >> 8;
		rbsp[size++] = cll->max_content_light_level;
		rbsp[size++] = cll->max_pic_average_light_level >> 8;
		rbsp[size++] = cll->max_pic_average_light_level;
	}
	rbsp[size++] = 0x80;
	data[out++] = 0;
	data[out++] = 0;
	data[out++] = 0;
	data[out++] = 1;
	data[out++] = 39 << 1;
	data[out++] = 1;
	for (i = 0; i < size; i++) {
		if (zeros == 2 && rbsp[i] <= 3) {
			data[out++] = 3;
			zeros = 0;
		}
		data[out++] = rbsp[i];
		zeros = rbsp[i] ? 0 : zeros + 1;
	}
	return out;
}
#endif
