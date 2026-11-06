// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <media/videobuf2-v4l2.h>

#include "hevc_priv.h"

static int hevc_dpb_test_init(struct kunit *test)
{
	struct aml_hevc_ctx *h = kunit_kzalloc(test, sizeof(*h), GFP_KERNEL);

	if (!h)
		return -ENOMEM;
	h->p[P_PIC_WIDTH] = 832;
	h->p[P_PIC_HEIGHT] = 480;
	h->p[P_CHROMA_FORMAT_IDC] = 1;
	h->p[P_TEMPORAL_ID] = 1;
	h->p[P_LOG2_DIFF_MAX_MIN_CB_SIZE] = 3;
	h->p[P_SLICE_TYPE] = H265_I_SLICE;
	test->priv = h;
	return 0;
}

static void valid_crop_selects_visible_rectangle(struct kunit *test)
{
	struct aml_hevc_ctx *h = test->priv;
	struct v4l2_rect crop;

	h->p[P_CONFORMANCE_WINDOW_FLAG] = 1;
	h->p[P_CONF_WIN_LEFT_OFFSET] = 1;
	h->p[P_CONF_WIN_RIGHT_OFFSET] = 3;
	h->p[P_CONF_WIN_TOP_OFFSET] = 2;
	h->p[P_CONF_WIN_BOTTOM_OFFSET] = 4;
	KUNIT_ASSERT_EQ(test, h265_validate_params(h->p), 0);
	crop = h265_display_crop(h->p);
	KUNIT_EXPECT_EQ(test, crop.left, 2);
	KUNIT_EXPECT_EQ(test, crop.top, 4);
	KUNIT_EXPECT_EQ(test, crop.width, 824U);
	KUNIT_EXPECT_EQ(test, crop.height, 468U);
}

static void invalid_crop_keeps_decoding_full_picture(struct kunit *test)
{
	static const u16 offsets[][4] = {
		{ 1, 24576, 0, 120 },
		{ 1, 57344, 0, 120 },
		{ 0, 0, 1, 239 },
		{ 1, 415, 0, 0 },
	};
	struct aml_hevc_ctx *h = test->priv;
	unsigned int i;

	h->p[P_CONFORMANCE_WINDOW_FLAG] = 1;
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		struct v4l2_rect crop;

		h->p[P_CONF_WIN_LEFT_OFFSET] = offsets[i][0];
		h->p[P_CONF_WIN_RIGHT_OFFSET] = offsets[i][1];
		h->p[P_CONF_WIN_TOP_OFFSET] = offsets[i][2];
		h->p[P_CONF_WIN_BOTTOM_OFFSET] = offsets[i][3];
		KUNIT_ASSERT_EQ(test, h265_validate_params(h->p), 0);
		crop = h265_display_crop(h->p);
		KUNIT_EXPECT_EQ(test, crop.left, 0);
		KUNIT_EXPECT_EQ(test, crop.top, 0);
		KUNIT_EXPECT_EQ(test, crop.width, 832U);
		KUNIT_EXPECT_EQ(test, crop.height, 480U);
	}
	/* The fallback does not admit unsupported chroma or invalid dimensions. */
	h->p[P_CHROMA_FORMAT_IDC] = 2;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EOPNOTSUPP);
	h->p[P_CHROMA_FORMAT_IDC] = 1;
	h->p[P_PIC_WIDTH] = 0;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EINVAL);
}

static void eos_retires_short_and_long_term_references(struct kunit *test)
{
	struct aml_hevc_ctx *h = test->priv;
	unsigned int i;

	h->cur = 1;
	h->curr_poc = 24;
	h->pic_w = 832;
	h->pic_h = 480;
	for (i = 0; i < 2; i++) {
		h->pic[i].valid = true;
		h->pic[i].referenced = true;
		h->pic[i].long_term = true;
		h->pic[i].poc = 22 + 2 * i;
		h->pic[i].width = h->pic_w;
		h->pic[i].height = h->pic_h;
	}
	h265_end_sequence(h);
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_EQ(test, h->pic[i].poc, (int)H265_INVALID_POC);
		KUNIT_EXPECT_FALSE(test, h->pic[i].referenced);
		KUNIT_EXPECT_FALSE(test, h->pic[i].long_term);
	}
	/* A following picture cannot retain or match an old POC 24. */
	h->cur = 2;
	h->curr_poc = 32;
	h->nal_type = 1;
	h->slice_type = H265_P_SLICE;
	h->p[P_NUM_REF_IDX_L0_ACTIVE] = 1;
	h->p[P_CUR_RPS] = 0x3ff8;
	h->p[P_CUR_RPS + 1] = RPS_END;
	h265_apply_rps(h);
	KUNIT_EXPECT_EQ(test, h265_set_ref_lists(h), -EINVAL);
	h->p[P_CUR_RPS] |= BIT(RPS_LT_BIT);
	h265_apply_rps(h);
	KUNIT_EXPECT_EQ(test, h265_set_ref_lists(h), -EINVAL);
}

static void eos_flushes_reordered_output_before_next_cra(struct kunit *test)
{
	static const int pocs[] = { 4, 2, 3, 0 };
	static const int slots[] = { 1, 2, 0 };
	struct aml_hevc_ctx *h = test->priv;
	struct vb2_v4l2_buffer *outputs;
	unsigned int i;

	outputs = kunit_kcalloc(test, ARRAY_SIZE(pocs), sizeof(*outputs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, outputs);
	h->epoch = 1;
	h->pending = 3;
	for (i = 0; i < 3; i++) {
		h->pic[i].poc = pocs[i];
		h->pic[i].epoch = h->epoch;
		h->pic[i].reorder_limit = 7;
		h->pic[i].output = &outputs[i];
	}
	KUNIT_ASSERT_EQ(test, h265_next_output(h, false), -1);
	h265_end_sequence(h);
	/* Repeated EOS must not replace the saved display POC with INVALID. */
	h265_end_sequence(h);
	KUNIT_EXPECT_EQ(test, h->pending, 3U);
	h->nal_type = H265_NAL_CRA;
	h->curr_poc = 0;
	KUNIT_ASSERT_FALSE(test, h265_update_random_access(h));
	h->pic[3].poc = 0;
	h->pic[3].epoch = h->epoch;
	h->pic[3].reorder_limit = 7;
	h->pic[3].output = &outputs[3];
	h->pending++;
	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		int slot = h265_next_output(h, false);

		KUNIT_ASSERT_EQ(test, slot, slots[i]);
		h->pic[slot].output = NULL;
		h->pic[slot].output_flush = false;
		h->pending--;
	}
	/* The new sequence still obeys its own reorder limit. */
	KUNIT_EXPECT_EQ(test, h265_next_output(h, false), -1);
	KUNIT_EXPECT_EQ(test, h265_next_output(h, true), 3);
	KUNIT_EXPECT_EQ(test, h->pending, 1U);
}

static void eos_rearms_cra_and_skips_leading_rasl(struct kunit *test)
{
	struct aml_hevc_ctx *h = test->priv;

	h->epoch = 1;
	h->curr_poc = 256;
	h->prev_tid0_poc = 256;
	h->poc_random_access = -H265_MAX_INT;
	h265_end_sequence(h);
	KUNIT_EXPECT_EQ(test, h->curr_poc, (int)H265_INVALID_POC);
	KUNIT_EXPECT_EQ(test, h->poc_random_access, H265_MAX_INT);
	h->nal_type = H265_NAL_CRA;
	h->temporal_id = 1;
	h->p[P_LOG2_MAX_POC_LSB_MINUS4] = 4;
	h->p[P_POC_LSB] = 32;
	h265_update_poc(h);
	KUNIT_ASSERT_EQ(test, h->curr_poc, 32);
	KUNIT_ASSERT_FALSE(test, h265_update_random_access(h));
	KUNIT_EXPECT_EQ(test, h->epoch, 2U);
	KUNIT_EXPECT_EQ(test, h->poc_random_access, 32);
	h->curr_poc = 28;
	h->nal_type = H265_NAL_RASL_R;
	KUNIT_EXPECT_TRUE(test, h265_update_random_access(h));
	h->nal_type = H265_NAL_RASL_N;
	KUNIT_EXPECT_TRUE(test, h265_update_random_access(h));
	h->nal_type = 6;
	KUNIT_EXPECT_FALSE(test, h265_update_random_access(h));
}

static struct kunit_case hevc_dpb_test_cases[] = {
	KUNIT_CASE(valid_crop_selects_visible_rectangle),
	KUNIT_CASE(invalid_crop_keeps_decoding_full_picture),
	KUNIT_CASE(eos_retires_short_and_long_term_references),
	KUNIT_CASE(eos_flushes_reordered_output_before_next_cra),
	KUNIT_CASE(eos_rearms_cra_and_skips_leading_rasl),
	{}
};

static struct kunit_suite hevc_dpb_test_suite = {
	.name = "amlogic-hevc-dpb",
	.init = hevc_dpb_test_init,
	.test_cases = hevc_dpb_test_cases,
};

kunit_test_suite(hevc_dpb_test_suite);
