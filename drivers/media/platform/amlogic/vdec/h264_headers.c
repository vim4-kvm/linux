// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/* Serialize stateless parameter sets for the firmware's native NAL parser. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "h264_headers.h"

struct h264_header_writer {
	u8 *data;
	size_t capacity;
	size_t length;
	u8 byte;
	u8 bits;
	u8 zeros;
	int error;
};

static void h264_header_byte(struct h264_header_writer *writer, u8 byte)
{
	if (writer->length == writer->capacity) {
		writer->error = -ENOSPC;
		return;
	}
	writer->data[writer->length++] = byte;
}

static void h264_header_bits(struct h264_header_writer *writer, u64 value, unsigned int count)
{
	while (count-- && !writer->error) {
		writer->byte = (writer->byte << 1) | ((value >> count) & 1);
		if (++writer->bits != 8)
			continue;
		if (writer->zeros == 2 && writer->byte <= 3) {
			h264_header_byte(writer, 3);
			writer->zeros = 0;
		}
		if (!writer->error)
			h264_header_byte(writer, writer->byte);
		writer->zeros = writer->byte ? 0 : writer->zeros + 1;
		writer->bits = 0;
		writer->byte = 0;
	}
}

static void h264_header_ue(struct h264_header_writer *writer, u64 value)
{
	unsigned int bits = fls64(value + 1);

	h264_header_bits(writer, 0, bits - 1);
	h264_header_bits(writer, value + 1, bits);
}

static void h264_header_se(struct h264_header_writer *writer, s32 value)
{
	/* Widen before negating: valid signed controls may contain INT_MIN. */
	h264_header_ue(writer, value > 0 ? 2 * (s64)value - 1 : -2 * (s64)value);
}

static void h264_header_start(struct h264_header_writer *writer, u8 type)
{
	static const u8 prefix[] = { 0, 0, 0, 1 };

	if (writer->capacity - writer->length < sizeof(prefix) + 1) {
		writer->error = -ENOSPC;
		return;
	}
	memcpy(writer->data + writer->length, prefix, sizeof(prefix));
	writer->length += sizeof(prefix);
	h264_header_byte(writer, 0x60 | type);
	writer->zeros = 0;
}

static void h264_header_finish(struct h264_header_writer *writer)
{
	h264_header_bits(writer, 1, 1);
	if (writer->bits)
		h264_header_bits(writer, 0, 8 - writer->bits);
}

static void h264_header_sps(struct h264_header_writer *w,
			    const struct v4l2_ctrl_h264_sps *sps)
{
	unsigned int i;

	h264_header_start(w, 7);
	h264_header_bits(w, sps->profile_idc, 8);
	for (i = 0; i < 6; i++)
		h264_header_bits(w, sps->constraint_set_flags >> i, 1);
	h264_header_bits(w, 0, 2);
	h264_header_bits(w, sps->level_idc, 8);
	h264_header_ue(w, sps->seq_parameter_set_id);
	if (sps->profile_idc >= 100) {
		h264_header_ue(w, sps->chroma_format_idc);
		h264_header_ue(w, sps->bit_depth_luma_minus8);
		h264_header_ue(w, sps->bit_depth_chroma_minus8);
		h264_header_bits(w, !!(sps->flags &
				       V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS), 1);
		/* Resolved matrices are emitted in the PPS, overriding the SPS. */
		h264_header_bits(w, 0, 1);
	}
	h264_header_ue(w, sps->log2_max_frame_num_minus4);
	h264_header_ue(w, sps->pic_order_cnt_type);
	if (!sps->pic_order_cnt_type) {
		h264_header_ue(w, sps->log2_max_pic_order_cnt_lsb_minus4);
	} else if (sps->pic_order_cnt_type == 1) {
		h264_header_bits(w, !!(sps->flags &
				       V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO), 1);
		h264_header_se(w, sps->offset_for_non_ref_pic);
		h264_header_se(w, sps->offset_for_top_to_bottom_field);
		h264_header_ue(w, sps->num_ref_frames_in_pic_order_cnt_cycle);
		for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
			h264_header_se(w, sps->offset_for_ref_frame[i]);
	}
	h264_header_ue(w, sps->max_num_ref_frames);
	h264_header_bits(w, !!(sps->flags & V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED), 1);
	h264_header_ue(w, sps->pic_width_in_mbs_minus1);
	h264_header_ue(w, sps->pic_height_in_map_units_minus1);
	h264_header_bits(w, !!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY), 1);
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY))
		h264_header_bits(w, !!(sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD), 1);
	h264_header_bits(w, !!(sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE), 1);
	/* Visible crop and colour metadata belong to the V4L2 client. */
	h264_header_bits(w, 0, 2);
	h264_header_finish(w);
}

static void h264_header_matrix(struct h264_header_writer *w, const u8 *matrix, unsigned int size)
{
	static const u8 scan4[] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
	static const u8 scan8[] = {
		0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
		12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
		35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
		58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
	};
	const u8 *scan = size == 16 ? scan4 : scan8;
	int last = 8;
	unsigned int i;

	for (i = 0; i < size; i++) {
		int next = matrix[scan[i]];

		if (!next) {
			w->error = -EINVAL;
			return;
		}
		h264_header_se(w, ((next - last + 128) & 255) - 128);
		last = next;
	}
}

static void h264_header_pps(struct h264_header_writer *w,
			    const struct v4l2_ctrl_h264_pps *pps,
			    const struct v4l2_ctrl_h264_scaling_matrix *matrix)
{
	unsigned int i;
	bool transform8 = pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;

	h264_header_start(w, 8);
	h264_header_ue(w, pps->pic_parameter_set_id);
	h264_header_ue(w, pps->seq_parameter_set_id);
	h264_header_bits(w, !!(pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE), 1);
	h264_header_bits(w, !!(pps->flags &
			       V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT), 1);
	h264_header_ue(w, 0);
	h264_header_ue(w, pps->num_ref_idx_l0_default_active_minus1);
	h264_header_ue(w, pps->num_ref_idx_l1_default_active_minus1);
	h264_header_bits(w, !!(pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED), 1);
	h264_header_bits(w, pps->weighted_bipred_idc, 2);
	h264_header_se(w, pps->pic_init_qp_minus26);
	h264_header_se(w, pps->pic_init_qs_minus26);
	h264_header_se(w, pps->chroma_qp_index_offset);
	h264_header_bits(w, !!(pps->flags &
			       V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT), 1);
	h264_header_bits(w, !!(pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED), 1);
	h264_header_bits(w, !!(pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT), 1);
	h264_header_bits(w, transform8, 1);
	h264_header_bits(w, 1, 1);
	for (i = 0; i < 6; i++) {
		h264_header_bits(w, 1, 1);
		h264_header_matrix(w, matrix->scaling_list_4x4[i], 16);
	}
	if (transform8) {
		for (i = 0; i < 2; i++) {
			h264_header_bits(w, 1, 1);
			h264_header_matrix(w, matrix->scaling_list_8x8[i], 64);
		}
	}
	h264_header_se(w, pps->second_chroma_qp_index_offset);
	h264_header_finish(w);
}

int aml_h264_build_headers(void *buffer, size_t capacity,
			   const struct v4l2_ctrl_h264_sps *sps,
			   const struct v4l2_ctrl_h264_pps *pps,
			   const struct v4l2_ctrl_h264_scaling_matrix *matrix)
{
	struct h264_header_writer writer = { .data = buffer, .capacity = capacity };

	if (sps->profile_idc < 100 || sps->chroma_format_idc != 1 ||
	    sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8)
		return -EINVAL;
	h264_header_sps(&writer, sps);
	if (!writer.error)
		h264_header_pps(&writer, pps, matrix);
	return writer.error ? writer.error : writer.length;
}
