// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "h264_headers.h"

#define HEADER_CAPACITY 8192

struct header_test_ctx {
	struct v4l2_ctrl_h264_sps sps;
	struct v4l2_ctrl_h264_pps pps;
	struct v4l2_ctrl_h264_scaling_matrix matrix;
	u8 data[HEADER_CAPACITY];
};

struct rbsp_reader {
	struct kunit *test;
	u8 *data;
	size_t size;
	size_t bit;
};

static u64 read_bits(struct rbsp_reader *r, unsigned int count)
{
	u64 value = 0;

	if (count > 64 || r->bit + count > r->size * 8) {
		KUNIT_FAIL(r->test, "truncated generated RBSP at bit %zu", r->bit);
		return 0;
	}
	while (count--) {
		value = (value << 1) | ((r->data[r->bit / 8] >> (7 - r->bit % 8)) & 1);
		r->bit++;
	}
	return value;
}

static u64 read_ue(struct rbsp_reader *r)
{
	unsigned int zeros = 0;

	while (!read_bits(r, 1)) {
		if (++zeros > 33) {
			KUNIT_FAIL(r->test, "invalid generated Exp-Golomb value");
			return 0;
		}
	}
	return (1ULL << zeros) - 1 + read_bits(r, zeros);
}

static s64 read_se(struct rbsp_reader *r)
{
	u64 code = read_ue(r);

	return code & 1 ? (s64)((code + 1) / 2) : -(s64)(code / 2);
}

static int header_test_init(struct kunit *test)
{
	struct header_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->sps = (struct v4l2_ctrl_h264_sps) {
		.profile_idc = 100, .level_idc = 30, .chroma_format_idc = 1,
		.log2_max_pic_order_cnt_lsb_minus4 = 2, .max_num_ref_frames = 4,
		.pic_width_in_mbs_minus1 = 39, .pic_height_in_map_units_minus1 = 22,
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY |
			 V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE,
	};
	ctx->pps = (struct v4l2_ctrl_h264_pps) {
		.num_ref_idx_l0_default_active_minus1 = 2, .weighted_bipred_idc = 2,
		.pic_init_qp_minus26 = -4, .chroma_qp_index_offset = -2,
		.second_chroma_qp_index_offset = -2,
		.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE |
			 V4L2_H264_PPS_FLAG_WEIGHTED_PRED |
			 V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT |
			 V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE,
	};
	memset(&ctx->matrix, 16, sizeof(ctx->matrix));
	return 0;
}

static int build_headers(struct header_test_ctx *ctx, void *data, size_t size)
{
	return aml_h264_build_headers(data, size, &ctx->sps, &ctx->pps, &ctx->matrix);
}

/* Decode the output syntax independently of the production bit writer. */
static void open_rbsp(struct kunit *test, struct rbsp_reader *r, u8 nal_type)
{
	struct header_test_ctx *ctx = test->priv;
	int len = build_headers(ctx, ctx->data, sizeof(ctx->data));
	size_t i, start = 0, end;
	unsigned int zeros = 0;

	KUNIT_ASSERT_GT(test, len, 0);
	for (i = 0; i + 4 < len; i++) {
		if (!memcmp(ctx->data + i, "\0\0\0\1", 4) &&
		    (ctx->data[i + 4] & 0x1f) == nal_type) {
			start = i + 5;
			break;
		}
	}
	KUNIT_ASSERT_NE(test, start, (size_t)0);
	end = len;
	for (i = start; i + 4 < len; i++) {
		if (!memcmp(ctx->data + i, "\0\0\0\1", 4)) {
			end = i;
			break;
		}
	}
	r->test = test;
	r->data = kunit_kmalloc(test, end - start, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r->data);
	for (i = start; i < end; i++) {
		u8 byte = ctx->data[i];

		if (zeros == 2 && byte == 3) {
			zeros = 0;
			continue;
		}
		r->data[r->size++] = byte;
		zeros = byte ? 0 : zeros + 1;
	}
}

static void check_matrix(struct kunit *test, struct rbsp_reader *r,
			 const u8 *matrix, unsigned int side)
{
	int diagonal, row, first, last, step, value = 8;

	KUNIT_ASSERT_EQ(test, read_bits(r, 1), 1);
	for (diagonal = 0; diagonal < side * 2 - 1; diagonal++) {
		first = max(0, diagonal - (int)side + 1);
		last = min((int)side - 1, diagonal);
		step = 1;
		if (!(diagonal & 1)) {
			swap(first, last);
			step = -1;
		}
		for (row = first; ; row += step) {
			value = (value + read_se(r)) & 255;
			KUNIT_EXPECT_EQ(test, value, matrix[row * side + diagonal - row]);
			if (row == last)
				break;
		}
	}
}

static void matrix_and_constraint_order(struct kunit *test)
{
	struct header_test_ctx *ctx = test->priv;
	struct rbsp_reader sps = {}, pps = {};
	unsigned int i, j;

	ctx->sps.constraint_set_flags = 0x25;
	for (i = 0; i < 6; i++)
		for (j = 0; j < 16; j++)
			ctx->matrix.scaling_list_4x4[i][j] = (i * 17 + j * 13) % 255 + 1;
	for (i = 0; i < 2; i++)
		for (j = 0; j < 64; j++)
			ctx->matrix.scaling_list_8x8[i][j] = (i * 29 + j * 19) % 255 + 1;
	open_rbsp(test, &sps, 7);
	KUNIT_ASSERT_NOT_NULL(test, sps.data);
	read_bits(&sps, 8);
	KUNIT_EXPECT_EQ(test, read_bits(&sps, 8), 0xa4);
	open_rbsp(test, &pps, 8);
	KUNIT_ASSERT_NOT_NULL(test, pps.data);
	KUNIT_EXPECT_EQ(test, read_ue(&pps), 0);
	KUNIT_EXPECT_EQ(test, read_ue(&pps), 0);
	KUNIT_EXPECT_EQ(test, read_bits(&pps, 2), 2);
	KUNIT_EXPECT_EQ(test, read_ue(&pps), 0);
	KUNIT_EXPECT_EQ(test, read_ue(&pps), 2);
	KUNIT_EXPECT_EQ(test, read_ue(&pps), 0);
	KUNIT_EXPECT_EQ(test, read_bits(&pps, 3), 6);
	KUNIT_EXPECT_EQ(test, read_se(&pps), -4);
	KUNIT_EXPECT_EQ(test, read_se(&pps), 0);
	KUNIT_EXPECT_EQ(test, read_se(&pps), -2);
	KUNIT_EXPECT_EQ(test, read_bits(&pps, 5), 0x13);
	for (i = 0; i < 6; i++)
		check_matrix(test, &pps, ctx->matrix.scaling_list_4x4[i], 4);
	for (i = 0; i < 2; i++)
		check_matrix(test, &pps, ctx->matrix.scaling_list_8x8[i], 8);
	KUNIT_EXPECT_EQ(test, read_se(&pps), -2);
	KUNIT_EXPECT_EQ(test, read_bits(&pps, 1), 1);
}

static void set_poc_extremes(struct header_test_ctx *ctx)
{
	unsigned int i;

	ctx->sps.pic_order_cnt_type = 1;
	ctx->sps.offset_for_non_ref_pic = S32_MIN;
	ctx->sps.offset_for_top_to_bottom_field = S32_MAX;
	ctx->sps.num_ref_frames_in_pic_order_cnt_cycle = 255;
	for (i = 0; i < 255; i++)
		ctx->sps.offset_for_ref_frame[i] = S32_MIN;
}

static void signed_poc_extremes(struct kunit *test)
{
	struct header_test_ctx *ctx = test->priv;
	struct rbsp_reader r = {};
	unsigned int i;

	set_poc_extremes(ctx);
	open_rbsp(test, &r, 7);
	KUNIT_ASSERT_NOT_NULL(test, r.data);
	read_bits(&r, 24);
	KUNIT_EXPECT_EQ(test, read_ue(&r), 0);
	KUNIT_EXPECT_EQ(test, read_ue(&r), 1);
	KUNIT_EXPECT_EQ(test, read_ue(&r), 0);
	KUNIT_EXPECT_EQ(test, read_ue(&r), 0);
	read_bits(&r, 2);
	KUNIT_EXPECT_EQ(test, read_ue(&r), 0);
	KUNIT_EXPECT_EQ(test, read_ue(&r), 1);
	KUNIT_EXPECT_EQ(test, read_bits(&r, 1), 0);
	KUNIT_EXPECT_EQ(test, read_se(&r), S32_MIN);
	KUNIT_EXPECT_EQ(test, read_se(&r), S32_MAX);
	KUNIT_EXPECT_EQ(test, read_ue(&r), 255);
	for (i = 0; i < 255; i++)
		KUNIT_EXPECT_EQ(test, read_se(&r), S32_MIN);
}

static void capacity_and_escaping(struct kunit *test)
{
	struct header_test_ctx *ctx = test->priv;
	u8 *guarded = kunit_kmalloc(test, HEADER_CAPACITY + 2, GFP_KERNEL);
	int len, ret, mode;
	size_t capacity;
	void *changed;

	KUNIT_ASSERT_NOT_NULL(test, guarded);
	for (mode = 0; mode < 2; mode++) {
		if (mode)
			set_poc_extremes(ctx);
		len = build_headers(ctx, ctx->data, sizeof(ctx->data));
		KUNIT_ASSERT_GT(test, len, 0);
		for (capacity = 0; capacity <= len; capacity++) {
			memset(guarded, 0xa5, HEADER_CAPACITY + 2);
			ret = build_headers(ctx, guarded + 1, capacity);
			KUNIT_ASSERT_EQ(test, ret, capacity == len ? len : -ENOSPC);
			KUNIT_ASSERT_EQ(test, guarded[0], 0xa5);
			changed = memchr_inv(guarded + capacity + 1, 0xa5,
					     HEADER_CAPACITY + 1 - capacity);
			KUNIT_ASSERT_NULL(test, changed);
		}
	}
	ctx->matrix.scaling_list_4x4[0][0] = 0;
	KUNIT_EXPECT_EQ(test, build_headers(ctx, ctx->data, sizeof(ctx->data)), -EINVAL);
}

static struct kunit_case header_test_cases[] = {
	KUNIT_CASE(matrix_and_constraint_order),
	KUNIT_CASE(signed_poc_extremes),
	KUNIT_CASE(capacity_and_escaping),
	{}
};

static struct kunit_suite header_test_suite = {
	.name = "amlogic-h264-headers",
	.init = header_test_init,
	.test_cases = header_test_cases,
};

kunit_test_suite(header_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
