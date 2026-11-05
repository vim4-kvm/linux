// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>
#include <media/v4l2-h264.h>

#include "aml_vdec_drv.h"
#include "aml_vdec_hw.h"
#include "h264_priv.h"

struct h264_test_ctx {
	struct aml_h264_ctx h264;
	struct aml_vdec_ctx ctx;
	struct aml_vdec_dev dev;
};

static void release_device(void *data)
{
	platform_device_put(data);
}

static int h264_test_init(struct kunit *test)
{
	struct h264_test_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	int ret;

	if (!ctx)
		return -ENOMEM;
	/* A real device supplies logging context; no driver is bound or probed. */
	ctx->dev.plat_dev = platform_device_alloc("amlogic-vdec-kunit", PLATFORM_DEVID_NONE);
	if (!ctx->dev.plat_dev)
		return -ENOMEM;
	ret = kunit_add_action_or_reset(test, release_device, ctx->dev.plat_dev);
	if (ret)
		return ret;
	ctx->ctx.dev = &ctx->dev;
	ctx->ctx.codec_priv = &ctx->h264;
	ctx->h264.v4l2_ctx = &ctx->ctx;
	test->priv = &ctx->h264;
	return 0;
}

static void duplicate_weighted_references(struct kunit *test)
{
	struct aml_h264_ctx *h = test->priv;
	struct v4l2_h264_dpb_entry pictures[] = {
		{ .pic_num = 2 }, { .pic_num = 1 }, { .pic_num = 0 },
	};
	static const int order[] = { 1, 1, 2, 0 };
	unsigned int i;
	int index = 0;

	h->mslice.num_ref_idx_l0 = 4;
	h->mslice.num_ref_idx_l1 = 2;
	for (i = 0; i < ARRAY_SIZE(pictures); i++)
		h->ref_list0[i].dpb = &pictures[i];
	h->list_size[0] = 3;
	h264_copy_ref_list(h, 0);
	for (i = 0; i < ARRAY_SIZE(order); i++)
		h264_reorder_short_term(&h->mslice, 0, order[i], &index);
	KUNIT_EXPECT_EQ(test, index, 4);
	KUNIT_EXPECT_EQ(test, h->list_size[0], 4);
	for (i = 0; i < ARRAY_SIZE(order); i++) {
		KUNIT_ASSERT_NOT_NULL(test, h->ref_list0[i].dpb);
		KUNIT_EXPECT_EQ(test, h->ref_list0[i].dpb->pic_num, order[i]);
	}
	KUNIT_EXPECT_EQ(test, h264_ref_list_size(h, 1), 2);
	h->mslice.num_ref_idx_l0 = 1;
	KUNIT_EXPECT_EQ(test, h264_ref_list_size(h, 0), 1);
}

static void independent_b_reference_lists(struct kunit *test)
{
	struct aml_h264_ctx *h = test->priv;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(h->ref_list0); i++) {
		h->ref_list0[i].poc = i + 1;
		h->ref_list1[i].poc = 17 - i;
	}
	h264_copy_ref_list(h, 0);
	h264_copy_ref_list(h, 1);
	KUNIT_EXPECT_MEMEQ(test, h->ref_list0_unreordered, h->ref_list0, sizeof(h->ref_list0));
	KUNIT_EXPECT_MEMEQ(test, h->ref_list1_unreordered, h->ref_list1, sizeof(h->ref_list1));
}

static void field_reference_metadata(struct kunit *test)
{
	static const struct v4l2_ctrl_h264_sps sps = {};
	struct aml_h264_ctx *h = test->priv;
	struct v4l2_ctrl_h264_decode_params *decode;
	struct v4l2_h264_reflist_builder builder;
	struct v4l2_h264_reference refs[V4L2_H264_REF_LIST_LEN];
	unsigned int i;

	decode = kunit_kzalloc(test, sizeof(*decode), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, decode);
	h->ctrl_ref.decode = decode;
	decode->flags = V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
	decode->frame_num = 15;
	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		struct v4l2_h264_dpb_entry *entry = &decode->dpb[i];

		entry->frame_num = i;
		entry->fields = V4L2_H264_FRAME_REF;
		entry->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
			       V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
		entry->top_field_order_cnt = i * 2;
		entry->bottom_field_order_cnt = (int)i * 2 - 1;
	}
	decode->dpb[0].flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
	v4l2_h264_init_reflist_builder(&builder, decode, &sps, decode->dpb);
	KUNIT_ASSERT_EQ(test, builder.num_valid, V4L2_H264_REF_LIST_LEN);
	v4l2_h264_build_p_ref_list(&builder, refs);
	h->ref_list0[V4L2_H264_REF_LIST_LEN].poc = 123456;
	h264_config_p_reflist(h, refs, builder.num_valid);
	KUNIT_EXPECT_EQ(test, h->list_size[0], V4L2_H264_REF_LIST_LEN);
	KUNIT_EXPECT_EQ(test, h->ref_list0[V4L2_H264_REF_LIST_LEN].poc, 123456);
	for (i = 0; i < builder.num_valid; i++) {
		struct v4l2_h264_dpb_entry *entry = &decode->dpb[refs[i].index];
		int poc = refs[i].fields == V4L2_H264_TOP_FIELD_REF ?
			  entry->top_field_order_cnt : entry->bottom_field_order_cnt;

		KUNIT_EXPECT_EQ(test, h->ref_list0[i].fields, refs[i].fields);
		KUNIT_EXPECT_EQ(test, h->ref_list0[i].poc, poc);
		KUNIT_EXPECT_EQ(test, h->ref_list0[i].long_term_flag,
				refs[i].index == 0);
	}

	/* A frame reference uses the earlier POC, including a negative bottom POC. */
	decode->flags = 0;
	v4l2_h264_init_reflist_builder(&builder, decode, &sps, decode->dpb);
	KUNIT_ASSERT_EQ(test, builder.num_valid, V4L2_H264_NUM_DPB_ENTRIES);
	v4l2_h264_build_p_ref_list(&builder, refs);
	h264_config_p_reflist(h, refs, builder.num_valid);
	for (i = 0; i < builder.num_valid; i++) {
		KUNIT_EXPECT_EQ(test, h->ref_list0[i].fields, V4L2_H264_FRAME_REF);
		KUNIT_EXPECT_EQ(test, h->ref_list0[i].poc,
				decode->dpb[refs[i].index].bottom_field_order_cnt);
	}
}

static void current_picture_order_count(struct kunit *test)
{
	static const struct v4l2_ctrl_h264_sps sps = {};
	struct aml_h264_ctx *h = test->priv;
	struct v4l2_ctrl_h264_decode_params *decode;

	decode = kunit_kzalloc(test, sizeof(*decode), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, decode);
	h->ctrl_ref.sps = &sps;
	h->ctrl_ref.decode = decode;
	decode->top_field_order_cnt = 8;
	decode->bottom_field_order_cnt = -2;
	decode->flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
	h264_config_ref_lists(h->v4l2_ctx);
	KUNIT_EXPECT_EQ(test, h->curr_spec.poc, -2);

	decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
	h264_config_ref_lists(h->v4l2_ctx);
	KUNIT_EXPECT_EQ(test, h->curr_spec.poc, 8);
	decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD;
	h264_config_ref_lists(h->v4l2_ctx);
	KUNIT_EXPECT_EQ(test, h->curr_spec.poc, -2);

	decode->flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
	decode->top_field_order_cnt = -8;
	decode->bottom_field_order_cnt = 2;
	h264_config_ref_lists(h->v4l2_ctx);
	KUNIT_EXPECT_EQ(test, h->curr_spec.poc, -8);
}

static int signed_field(u8 code)
{
	return code & 1 ? (code + 1) / 2 : -(int)code / 2;
}

static void parameter_slots_and_signed_fields(struct kunit *test)
{
	struct aml_h264_ctx *h = test->priv;
	struct v4l2_ctrl_h264_sps sps = {
		.profile_idc = 100, .level_idc = 31, .chroma_format_idc = 1,
		.max_num_ref_frames = 4, .pic_width_in_mbs_minus1 = 39,
		.pic_height_in_map_units_minus1 = 22,
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};
	struct v4l2_ctrl_h264_pps pps = {};
	u16 table[258];
	unsigned int poc, i, width, parsed_width, area;
	int qp;

	for (poc = 0; poc <= 2; poc++) {
		sps.pic_order_cnt_type = poc;
		memset(table, 0xa5, sizeof(table));
		h264_build_sps_params(h, table + 1, &sps);
		KUNIT_EXPECT_EQ(test, table[0], 0xa5a5);
		KUNIT_EXPECT_EQ(test, table[257], 0xa5a5);
		KUNIT_EXPECT_EQ(test, table[4], PARAM_BASE_VAL);
		for (i = 33; i < 257; i++)
			KUNIT_EXPECT_EQ(test, table[i], 0);
	}
	memset(table, 0xa5, sizeof(table));
	h264_build_pps_params(h, table + 1, &pps);
	KUNIT_EXPECT_EQ(test, table[0], 0xa5a5);
	KUNIT_EXPECT_EQ(test, table[257], 0xa5a5);
	KUNIT_EXPECT_EQ(test, table[4], PARAM_BASE_VAL);
	for (i = 9; i < 257; i++)
		KUNIT_EXPECT_EQ(test, table[i], 0);
	for (width = 240; width <= 256; width += 16) {
		sps.pic_width_in_mbs_minus1 = width - 1;
		sps.pic_height_in_map_units_minus1 = 143;
		h264_build_sps_params(h, table + 1, &sps);
		parsed_width = h->seq_info & 255;
		if (!parsed_width)
			parsed_width = 256;
		area = (h->seq_info >> 8) & 65535;
		KUNIT_EXPECT_EQ(test, parsed_width, width);
		KUNIT_EXPECT_EQ(test, area, width * 144);
		KUNIT_EXPECT_EQ(test, area / parsed_width, 144);
	}
	for (qp = -26; qp <= 25; qp++) {
		pps.pic_init_qp_minus26 = qp;
		pps.pic_init_qs_minus26 = qp;
		pps.chroma_qp_index_offset = qp % 13;
		pps.second_chroma_qp_index_offset = -(qp % 13);
		h264_build_pps_params(h, table + 1, &pps);
		KUNIT_EXPECT_EQ(test, signed_field(table[1]), qp);
		KUNIT_EXPECT_EQ(test, signed_field(table[1] >> 8), qp);
		KUNIT_EXPECT_EQ(test, signed_field(table[8]), pps.chroma_qp_index_offset);
		KUNIT_EXPECT_EQ(test, signed_field(table[6]), pps.second_chroma_qp_index_offset);
	}
}

static void interlaced_frame_dimensions(struct kunit *test)
{
	struct aml_h264_ctx *h = test->priv;
	struct v4l2_ctrl_h264_sps sps = {
		.chroma_format_idc = 1,
		.pic_width_in_mbs_minus1 = 119,
		.pic_height_in_map_units_minus1 = 33,
	};
	u16 table[256];

	/* 1080i needs 1088 coded lines, not one field's 544 lines. */
	h264_build_sps_params(h, table, &sps);
	KUNIT_EXPECT_EQ(test, h->frame_height, 1088);
	KUNIT_EXPECT_EQ(test, h->mb_height, 68);
	KUNIT_EXPECT_EQ(test, h->mb_total, 120 * 68);
	KUNIT_EXPECT_EQ(test, (h->seq_info >> 8) & 0xffff, 120 * 68);
	sps.flags = V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	h264_build_sps_params(h, table, &sps);
	KUNIT_EXPECT_EQ(test, h->frame_height, 1088);

	/* Expand map units before rounding the canvas to four MB rows. */
	sps.pic_height_in_map_units_minus1 = 34;
	h264_build_sps_params(h, table, &sps);
	KUNIT_EXPECT_EQ(test, h->frame_height, 1120);
	KUNIT_EXPECT_EQ(test, h->mb_height, 72);
	KUNIT_EXPECT_EQ(test, (h->seq_info >> 8) & 0xffff, 120 * 70);

	/* Progressive SPS retains its one-row map units. */
	sps.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	h264_build_sps_params(h, table, &sps);
	KUNIT_EXPECT_EQ(test, h->frame_height, 560);
	KUNIT_EXPECT_EQ(test, h->mb_height, 36);
	KUNIT_EXPECT_EQ(test, (h->seq_info >> 8) & 0xffff, 120 * 35);
}

static void input_padding_boundaries(struct kunit *test)
{
	static const u32 sizes[] = { 1, 7, 8, 1000, 3064 };
	u8 *memory = kunit_kmalloc(test, 4096, GFP_KERNEL);
	unsigned int i;
	u32 size;

	KUNIT_ASSERT_NOT_NULL(test, memory);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		size = sizes[i];
		memset(memory, 0xa5, 4096);
		KUNIT_ASSERT_EQ(test, h264_pad_input(memory, size, 4096), 0);
		KUNIT_EXPECT_NULL(test, memchr_inv(memory, 0xa5, size));
		KUNIT_EXPECT_NULL(test, memchr_inv(memory + size, 0, 1024));
		KUNIT_EXPECT_NULL(test, memchr_inv(memory + size + 1024, 0xa5, 3072 - size));
	}
	memset(memory, 0xa5, 4096);
	KUNIT_EXPECT_EQ(test, h264_pad_input(memory, 3065, 4096), -EINVAL);
	KUNIT_EXPECT_EQ(test, h264_pad_input(memory, 0, 4096), -EINVAL);
	KUNIT_EXPECT_EQ(test, h264_pad_input(NULL, 1, 4096), -EINVAL);
	KUNIT_EXPECT_EQ(test, h264_pad_input(memory, 1, 1032), -EINVAL);
	KUNIT_EXPECT_NULL(test, memchr_inv(memory, 0xa5, 4096));
}

static void canvas_address_layout(struct kunit *test)
{
	unsigned int i;
	u64 layout;
	u32 low, high, addr;

	for (i = 0; i <= V4L2_H264_NUM_DPB_ENTRIES; i++) {
		addr = 0xc0000000 + i * 0x80000;
		layout = aml_vdec_canvas_layout(addr, 640, 384);
		low = lower_32_bits(layout);
		high = upper_32_bits(layout);
		KUNIT_EXPECT_EQ(test, (low & 0x1fffffffU) << 3, addr);
		KUNIT_EXPECT_EQ(test, ((low >> 29) | ((high & 0x1ff) << 3)) << 3, 640);
		KUNIT_EXPECT_EQ(test, (high >> 9) & 0x1fff, 384);
		KUNIT_EXPECT_EQ(test, high >> 22, 7 << 4);
		layout = aml_vdec_canvas_layout(addr + 640 * 384, 640, 192);
		KUNIT_EXPECT_EQ(test, (lower_32_bits(layout) & 0x1fffffffU) << 3,
				addr + 640 * 384);
		KUNIT_EXPECT_EQ(test, (upper_32_bits(layout) >> 9) & 0x1fff, 192);
	}
}

static void original_picture_structure(struct kunit *test)
{
	struct v4l2_ctrl_h264_decode_params decode = {};
	struct v4l2_ctrl_h264_sps sps = {};

	sps.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	KUNIT_EXPECT_EQ(test, h264_picture_structure(&decode, &sps), H264_PICTURE_FRAME);
	sps.flags = V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	KUNIT_EXPECT_EQ(test, h264_picture_structure(&decode, &sps), H264_PICTURE_MBAFF);
	decode.flags = V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
	KUNIT_EXPECT_EQ(test, h264_picture_structure(&decode, &sps), H264_PICTURE_TOP);
	decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD;
	KUNIT_EXPECT_EQ(test, h264_picture_structure(&decode, &sps), H264_PICTURE_BOTTOM);
	/* A frame picture need not be MBAFF when field pictures are allowed. */
	sps.flags = 0;
	decode.flags = 0;
	KUNIT_EXPECT_EQ(test, h264_picture_structure(&decode, &sps), H264_PICTURE_FRAME);
}

static void colocated_mv_layout(struct kunit *test)
{
	static const struct {
		enum h264_picture_structure current_structure, reference;
		u32 write_mb, read_mb;
	} cases[] = {
		{ H264_PICTURE_FRAME, H264_PICTURE_FRAME, 7, 7 },
		{ H264_PICTURE_TOP, H264_PICTURE_FRAME, 14, 11 },
		{ H264_PICTURE_BOTTOM, H264_PICTURE_FRAME, 14, 11 },
		{ H264_PICTURE_FRAME, H264_PICTURE_TOP, 7, 6 },
		{ H264_PICTURE_FRAME, H264_PICTURE_BOTTOM, 7, 6 },
		{ H264_PICTURE_MBAFF, H264_PICTURE_TOP, 14, 14 },
		{ H264_PICTURE_MBAFF, H264_PICTURE_FRAME, 14, 14 },
		{ H264_PICTURE_FRAME, H264_PICTURE_MBAFF, 7, 7 },
		{ H264_PICTURE_TOP, H264_PICTURE_MBAFF, 14, 14 },
		{ H264_PICTURE_TOP, H264_PICTURE_BOTTOM, 14, 14 },
	};
	u32 write_offset, read_offset;
	unsigned int i;
	int ret;

	/* Nonzero row and column distinguish field and frame address layouts. */
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		ret = h264_mv_offsets(cases[i].current_structure, cases[i].reference,
				      7, 4, 16 * 96, false, &write_offset, &read_offset);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, write_offset, cases[i].write_mb * 96);
		KUNIT_EXPECT_EQ(test, read_offset, cases[i].read_mb * 96);
	}
	ret = h264_mv_offsets(H264_PICTURE_FRAME, H264_PICTURE_FRAME,
			      15, 4, 16 * 96, true, &write_offset, &read_offset);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, write_offset, 15 * 24);
	KUNIT_EXPECT_EQ(test, read_offset, 15 * 24);
	ret = h264_mv_offsets(H264_PICTURE_FRAME, H264_PICTURE_FRAME,
			      16, 4, 16 * 96, true, &write_offset, &read_offset);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	ret = h264_mv_offsets(H264_PICTURE_TOP, H264_PICTURE_FRAME,
			      8, 4, 16 * 96, false, &write_offset, &read_offset);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	ret = h264_mv_offsets(H264_PICTURE_FRAME, H264_PICTURE_FRAME,
			      U32_MAX, 4, U32_MAX, false, &write_offset, &read_offset);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	ret = h264_mv_offsets(H264_PICTURE_FRAME, H264_PICTURE_FRAME,
			      0, 0, 16 * 96, false, &write_offset, &read_offset);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void timestamp_picture_slots(struct kunit *test)
{
	struct aml_h264_ctx *h = test->priv;
	struct v4l2_ctrl_h264_decode_params *decode;
	int first_canvas, first_mv;

	decode = kunit_kzalloc(test, sizeof(*decode), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, decode);
	h->ctrl_ref.decode = decode;
	h->colocated_buf_num = 2;
	decode->nal_ref_idc = 1;
	decode->flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
	h->curr_spec.structure = H264_PICTURE_TOP;
	h264_prune_picture_slots(h, 0, false);
	KUNIT_ASSERT_EQ(test, h264_get_picture_slots(h, 0), 0);
	first_canvas = h->curr_spec.canvas_pos;
	first_mv = h->curr_spec.col_buf_index;

	/* Zero is a valid timestamp; IDR's held second field must reuse it. */
	h->curr_spec.structure = H264_PICTURE_BOTTOM;
	h264_prune_picture_slots(h, 0, true);
	KUNIT_ASSERT_EQ(test, h264_get_picture_slots(h, 0), 0);
	KUNIT_EXPECT_EQ(test, h->curr_spec.canvas_pos, first_canvas);
	KUNIT_EXPECT_EQ(test, h->curr_spec.col_buf_index, first_mv);
	KUNIT_EXPECT_EQ(test, h->colocated_structure[first_mv], H264_PICTURE_TOP);

	decode->flags = 0;
	decode->dpb[0].flags = V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
	decode->dpb[0].reference_ts = 0;
	h264_prune_picture_slots(h, 1ULL << 40, false);
	KUNIT_ASSERT_EQ(test, h264_get_picture_slots(h, 1ULL << 40), 0);
	KUNIT_EXPECT_NE(test, h->curr_spec.canvas_pos, first_canvas);
	KUNIT_EXPECT_NE(test, h->curr_spec.col_buf_index, first_mv);
	KUNIT_EXPECT_EQ(test, h264_get_picture_slots(h, 17), -ENOSPC);

	/* An inactive DPB entry cannot keep storage alive after FLUSH/reuse. */
	decode->dpb[0].flags = 0;
	h264_prune_picture_slots(h, 18, false);
	KUNIT_EXPECT_EQ(test, h->canvas_pos_map, 0);
	KUNIT_EXPECT_EQ(test, h->colocated_buf_map, 0);
	KUNIT_ASSERT_EQ(test, h264_get_picture_slots(h, 18), 0);
	decode->flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
	decode->dpb[0].flags = V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
	decode->dpb[0].reference_ts = 18;
	h264_prune_picture_slots(h, 19, false);
	KUNIT_EXPECT_EQ(test, h->canvas_pos_map, 0);
	KUNIT_EXPECT_EQ(test, h->colocated_buf_map, 0);
}

static struct kunit_case h264_test_cases[] = {
	KUNIT_CASE(timestamp_picture_slots),
	KUNIT_CASE(original_picture_structure),
	KUNIT_CASE(colocated_mv_layout),
	KUNIT_CASE(duplicate_weighted_references),
	KUNIT_CASE(independent_b_reference_lists),
	KUNIT_CASE(field_reference_metadata),
	KUNIT_CASE(current_picture_order_count),
	KUNIT_CASE(parameter_slots_and_signed_fields),
	KUNIT_CASE(interlaced_frame_dimensions),
	KUNIT_CASE(input_padding_boundaries),
	KUNIT_CASE(canvas_address_layout),
	{}
};

static struct kunit_suite h264_test_suite = {
	.name = "amlogic-h264-parameters",
	.init = h264_test_init,
	.test_cases = h264_test_cases,
};

kunit_test_suite(h264_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
