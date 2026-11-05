// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include "hevc_priv.h"

static void reset_params(struct aml_hevc_ctx *h)
{
	memset(h, 0, sizeof(*h));
	h->p[P_PIC_WIDTH] = 704;
	h->p[P_PIC_HEIGHT] = 640;
	h->p[P_CHROMA_FORMAT_IDC] = 1;
	h->p[P_TEMPORAL_ID] = 1;
	h->p[P_LOG2_DIFF_MAX_MIN_CB_SIZE] = 3;
	h->p[P_LOG2_MAX_POC_LSB_MINUS4] = 4;
	h->p[P_SLICE_TYPE] = H265_I_SLICE;
}

static int hevc_test_init(struct kunit *test)
{
	struct aml_hevc_ctx *h = kunit_kzalloc(test, sizeof(*h), GFP_KERNEL);

	if (!h)
		return -ENOMEM;
	reset_params(h);
	test->priv = h;
	return 0;
}

static void poc_wrap_and_random_access(struct kunit *test)
{
	static const unsigned int excluded[] = { 0, 2, 4, 6, 7, 8, 9 };
	struct aml_hevc_ctx *h = test->priv;
	unsigned int i;

	h->temporal_id = 1;
	h->nal_type = 1;
	h->prev_tid0_poc = 1;
	h->p[P_POC_LSB] = 255;
	h265_update_poc(h);
	KUNIT_EXPECT_EQ(test, h->curr_poc, -1);
	KUNIT_EXPECT_EQ(test, h->prev_tid0_poc, -1);
	h->p[P_POC_LSB] = 1;
	h265_update_poc(h);
	KUNIT_EXPECT_EQ(test, h->curr_poc, 1);
	KUNIT_EXPECT_EQ(test, h->prev_tid0_poc, 1);
	for (i = 0; i < ARRAY_SIZE(excluded); i++) {
		h->nal_type = excluded[i];
		h->p[P_POC_LSB] = 3;
		h265_update_poc(h);
		KUNIT_EXPECT_EQ(test, h->curr_poc, 3);
		KUNIT_EXPECT_EQ(test, h->prev_tid0_poc, 1);
	}
	h->nal_type = H265_NAL_CRA;
	h->poc_random_access = H265_MAX_INT;
	h->p[P_POC_LSB] = 240;
	h265_update_poc(h);
	KUNIT_EXPECT_EQ(test, h->curr_poc, 240);
	KUNIT_EXPECT_EQ(test, h->prev_tid0_poc, 240);
	h->nal_type = H265_NAL_IDR_N_LP;
	h265_update_poc(h);
	KUNIT_EXPECT_EQ(test, h->curr_poc, 0);
	KUNIT_EXPECT_EQ(test, h->prev_tid0_poc, 0);
}

static void invalid_firmware_parameters(struct kunit *test)
{
	static const unsigned int shifts[] = {
		P_LOG2_MIN_CB_SIZE_MINUS3, P_LOG2_DIFF_MAX_MIN_CB_SIZE,
		P_LOG2_MAX_POC_LSB_MINUS4,
	};
	struct aml_hevc_ctx *h = test->priv;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), 0);
	h->p[P_BIT_DEPTH] = 0x22;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EOPNOTSUPP);
	reset_params(h);
	h->p[P_CHROMA_FORMAT_IDC] = 2;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EOPNOTSUPP);
	for (i = 0; i < ARRAY_SIZE(shifts); i++) {
		reset_params(h);
		h->p[shifts[i]] = U16_MAX;
		KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EINVAL);
	}
	reset_params(h);
	h->p[P_SLICE_SEGMENT_ADDRESS] = 110;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EINVAL);
}

static void tile_boundaries(struct kunit *test)
{
	struct aml_hevc_ctx *h = test->priv;

	h->p[P_TILES_ENABLED_FLAG] = 3;
	h->p[P_NUM_TILE_COLUMNS_MINUS1] = 2;
	h->p[P_NUM_TILE_ROWS_MINUS1] = 2;
	KUNIT_ASSERT_EQ(test, h265_validate_params(h->p), 0);
	h265_update_tiles(h, 11, 10, 384);
	KUNIT_EXPECT_EQ(test, h->tile[0][0].width, 3);
	KUNIT_EXPECT_EQ(test, h->tile[0][1].width, 4);
	KUNIT_EXPECT_EQ(test, h->tile[0][2].width, 4);
	KUNIT_EXPECT_EQ(test, h->tile[0][1].start_cu_x, 3);
	KUNIT_EXPECT_EQ(test, h->tile[0][2].start_cu_x, 7);
	KUNIT_EXPECT_EQ(test, h->tile[0][0].height, 3);
	KUNIT_EXPECT_EQ(test, h->tile[1][0].height, 3);
	KUNIT_EXPECT_EQ(test, h->tile[2][0].height, 4);
	h->p[P_TILES_ENABLED_FLAG] = 1;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EINVAL);
	h->p[P_TILE_WIDTH] = 2;
	h->p[P_TILE_WIDTH + 1] = 2;
	h->p[P_TILE_HEIGHT] = 2;
	h->p[P_TILE_HEIGHT + 1] = 2;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), 0);
	h->p[P_TILE_WIDTH + 1] = 10;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EINVAL);
	reset_params(h);
	h->p[P_TILES_ENABLED_FLAG] = 1;
	h->p[P_NUM_TILE_ROWS_MINUS1] = 9;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EOPNOTSUPP);
	reset_params(h);
	h->p[P_PIC_WIDTH] = 4096;
	h->p[P_PIC_HEIGHT] = 2304;
	h->p[P_LOG2_DIFF_MAX_MIN_CB_SIZE] = 1;
	h->p[P_TILES_ENABLED_FLAG] = 3;
	h->p[P_NUM_TILE_COLUMNS_MINUS1] = 9;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EOPNOTSUPP);
}

static void missing_and_reordered_references(struct kunit *test)
{
	static const int pocs[] = { 3, 5, 1 };
	struct aml_hevc_ctx *h = test->priv;
	unsigned int i;

	h->curr_poc = 4;
	h->cur = 17;
	h->pic_w = 704;
	h->pic_h = 640;
	h->slice_type = H265_B_SLICE;
	h->p[P_SLICE_TYPE] = H265_B_SLICE;
	h->p[P_NUM_REF_IDX_L0_ACTIVE] = 3;
	h->p[P_NUM_REF_IDX_L1_ACTIVE] = 3;
	/* Firmware RPS: used short-term -1, +1, then long-term -3. */
	h->p[P_CUR_RPS] = 0x3fff;
	h->p[P_CUR_RPS + 1] = 0x2001;
	h->p[P_CUR_RPS + 2] = 0x7ffd;
	h->p[P_CUR_RPS + 3] = RPS_END;
	for (i = 0; i < ARRAY_SIZE(pocs); i++) {
		h->pic[i].valid = true;
		h->pic[i].referenced = true;
		h->pic[i].width = 704;
		h->pic[i].height = 640;
		h->pic[i].poc = pocs[i];
	}
	KUNIT_ASSERT_EQ(test, h265_set_ref_lists(h), 0);
	KUNIT_EXPECT_MEMEQ(test, h->ref_poc_l0, pocs, sizeof(pocs));
	KUNIT_EXPECT_EQ(test, h->ref_poc_l1[0], 5);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l1[1], 3);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l1[2], 1);
	h->pic[2].valid = false;
	KUNIT_EXPECT_EQ(test, h265_set_ref_lists(h), -EINVAL);
	h->pic[2].valid = true;
	h->p[P_MODIFICATION_FLAG] = 1;
	h->p[P_MODIFICATION_LIST] = 2;
	h->p[P_MODIFICATION_LIST + 1] = 0;
	h->p[P_MODIFICATION_LIST + 2] = 1;
	KUNIT_ASSERT_EQ(test, h265_set_ref_lists(h), 0);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[0], 1);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[1], 3);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[2], 5);
	h->p[P_MODIFICATION_LIST] = 3;
	KUNIT_EXPECT_EQ(test, h265_set_ref_lists(h), -EINVAL);
	h->p[P_CUR_RPS] = RPS_END;
	KUNIT_EXPECT_EQ(test, h265_set_ref_lists(h), -EINVAL);
	h->slice_type = H265_I_SLICE;
	h->p[P_SLICE_TYPE] = H265_I_SLICE;
	KUNIT_EXPECT_EQ(test, h265_set_ref_lists(h), 0);
	KUNIT_EXPECT_EQ(test, h->ref_num_l0, 0);
	KUNIT_EXPECT_EQ(test, h->ref_num_l1, 0);
}

static void capture_sao_capacity(struct kunit *test)
{
	static const u32 sizes[][2] = {
		{ 64, 64 }, { 256, 256 }, { 640, 368 }, { 1920, 1080 }, { 4096, 2304 },
	};
	struct v4l2_pix_format_mplane f = {};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		h265_capture_format(&f, sizes[i][0], sizes[i][1]);
		KUNIT_EXPECT_EQ(test, f.num_planes, 2);
		KUNIT_EXPECT_GE(test, f.width, sizes[i][0]);
		KUNIT_EXPECT_GE(test, f.height, sizes[i][1]);
		KUNIT_EXPECT_GE(test, f.plane_fmt[0].sizeimage, f.width * f.height);
		KUNIT_EXPECT_GE(test, f.plane_fmt[1].sizeimage, f.width * f.height / 2);
		KUNIT_EXPECT_EQ(test, f.plane_fmt[1].sizeimage % SZ_64K, 0);
		KUNIT_EXPECT_EQ(test, f.plane_fmt[0].sizeimage, 2 * f.plane_fmt[1].sizeimage);
	}
	h265_capture_format(&f, 256, 256);
	KUNIT_EXPECT_EQ(test, f.plane_fmt[0].sizeimage, 131072);
	KUNIT_EXPECT_EQ(test, f.plane_fmt[1].sizeimage, 65536);
}

static struct kunit_case hevc_test_cases[] = {
	KUNIT_CASE(poc_wrap_and_random_access),
	KUNIT_CASE(invalid_firmware_parameters),
	KUNIT_CASE(tile_boundaries),
	KUNIT_CASE(missing_and_reordered_references),
	KUNIT_CASE(capture_sao_capacity),
	{}
};

static struct kunit_suite hevc_test_suite = {
	.name = "amlogic-hevc-parameters",
	.init = hevc_test_init,
	.test_cases = hevc_test_cases,
};

kunit_test_suite(hevc_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
