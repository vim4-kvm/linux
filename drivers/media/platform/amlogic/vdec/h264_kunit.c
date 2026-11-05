// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

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

static struct kunit_case h264_test_cases[] = {
	KUNIT_CASE(duplicate_weighted_references),
	KUNIT_CASE(independent_b_reference_lists),
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
