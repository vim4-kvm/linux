// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

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

static struct kunit_case hevc_dpb_test_cases[] = {
	KUNIT_CASE(valid_crop_selects_visible_rectangle),
	KUNIT_CASE(invalid_crop_keeps_decoding_full_picture),
	{}
};

static struct kunit_suite hevc_dpb_test_suite = {
	.name = "amlogic-hevc-dpb",
	.init = hevc_dpb_test_init,
	.test_cases = hevc_dpb_test_cases,
};

kunit_test_suite(hevc_dpb_test_suite);
