// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/videodev2.h>

#include "wave5-vui.h"

static void wave5_vui_defaults(struct kunit *test)
{
	u8 data[5];
	int hevc;

	for (hevc = 0; hevc <= 1; hevc++) {
		KUNIT_ASSERT_EQ(test, wave5_make_vui(data, hevc, 0, 0, 0, 0),
				38 + hevc);
		/* video_signal_type_present, video_format, colour_description. */
		KUNIT_EXPECT_EQ(test, data[0], 0x35);
		KUNIT_EXPECT_EQ(test, data[1], 1);
		KUNIT_EXPECT_EQ(test, data[2], 1);
		KUNIT_EXPECT_EQ(test, data[3], 1);
		/* The suffix has no timing info or bitstream restrictions. */
		KUNIT_EXPECT_EQ(test, data[4], 0);
	}
}

static void wave5_vui_colour_description(struct kunit *test)
{
	u8 data[5];
	int hevc, bits;

	for (hevc = 0; hevc <= 1; hevc++) {
		bits = wave5_make_vui(data, hevc,
				      V4L2_COLORSPACE_BT2020, V4L2_XFER_FUNC_SMPTE2084,
				      V4L2_YCBCR_ENC_BT2020, V4L2_QUANTIZATION_FULL_RANGE);
		KUNIT_ASSERT_EQ(test, bits, 38 + hevc);
		/* H.264/HEVC video_full_range_flag precedes the colour triplet. */
		KUNIT_EXPECT_EQ(test, data[0], 0x37);
		KUNIT_EXPECT_EQ(test, data[1], 9);
		KUNIT_EXPECT_EQ(test, data[2], 16);
		KUNIT_EXPECT_EQ(test, data[3], 9);
		bits = wave5_make_vui(data, hevc,
				      V4L2_COLORSPACE_REC709, V4L2_XFER_FUNC_SRGB,
				      V4L2_YCBCR_ENC_709, V4L2_QUANTIZATION_LIM_RANGE);
		KUNIT_ASSERT_EQ(test, bits, 38 + hevc);
		KUNIT_EXPECT_EQ(test, data[0], 0x35);
		KUNIT_EXPECT_EQ(test, data[1], 1);
		KUNIT_EXPECT_EQ(test, data[2], 13);
		KUNIT_EXPECT_EQ(test, data[3], 1);
	}
}

static void wave5_vui_rejects_unsupported(struct kunit *test)
{
	u8 data[5];
	const u8 unchanged[5] = { 0xa5, 0xa5, 0xa5, 0xa5, 0xa5 };
	u32 fields[4];
	unsigned int field;
	int bits;

	for (field = 0; field < ARRAY_SIZE(fields); field++) {
		memset(data, 0xa5, sizeof(data));
		memset(fields, 0, sizeof(fields));
		fields[field] = U32_MAX;
		bits = wave5_make_vui(data, true, fields[0], fields[1],
				      fields[2], fields[3]);
		KUNIT_EXPECT_EQ(test, bits, -EINVAL);
		KUNIT_EXPECT_MEMEQ(test, data, unchanged, sizeof(data));
	}
}

static struct kunit_case wave5_vui_cases[] = {
	KUNIT_CASE(wave5_vui_defaults),
	KUNIT_CASE(wave5_vui_colour_description),
	KUNIT_CASE(wave5_vui_rejects_unsupported),
	{}
};

static struct kunit_suite wave5_vui_suite = {
	.name = "wave5-vui",
	.test_cases = wave5_vui_cases,
};

kunit_test_suite(wave5_vui_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Wave5 VUI serialization tests");
