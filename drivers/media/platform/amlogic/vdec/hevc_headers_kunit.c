// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "hevc_headers.h"

/* SPS/PPS and slice prefixes from OPFLAG_B_Qualcomm_1. */
static const u8 opflag_params[] = {
	0, 0, 0, 1, 0x42, 1, 1, 1, 0x60, 0, 0, 3, 0, 0, 3, 0,
	0, 3, 0, 0, 3, 0, 0x5d, 0xa0, 6, 0x82, 1, 0xe1, 0xfe, 0x5f,
	0x92, 0x46, 0xd9, 0xed, 0x90,
	0, 0, 1, 0x44, 1, 0xd1, 0x90, 0x95, 0x81, 0x12,
};

static void output_flags_and_parameter_replacement(struct kunit *test)
{
	static const u8 hidden[] = { 0, 0, 1, 2, 1, 0xd8, 0x9e };
	static const u8 idr[] = { 0, 0, 1, 0x26, 1, 0xaf, 0x8c };
	/* Replace PPS0 with output_flag_present_flag=0, then the same I prefix. */
	static const u8 replace[] = { 0, 0, 1, 0x44, 1, 0xc0,
		0, 0, 1, 0x26, 1, 0xae };
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, opflag_params,
						    sizeof(opflag_params), &au), 0);
	KUNIT_EXPECT_FALSE(test, au.has_picture);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hidden, sizeof(hidden), &au), 0);
	KUNIT_EXPECT_TRUE(test, au.has_picture);
	KUNIT_EXPECT_FALSE(test, au.picture.pic_output);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, idr, sizeof(idr), &au), 0);
	KUNIT_EXPECT_TRUE(test, au.picture.pic_output);
	KUNIT_EXPECT_FALSE(test, au.picture.no_output_of_prior_pics);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, replace, sizeof(replace), &au), 0);
	KUNIT_EXPECT_TRUE(test, au.picture.pic_output);
}

static void highest_sublayer_limits_and_cvs_markers(struct kunit *test)
{
	/* BUMPING_A SPS: four sublayers, reorder 0/1/2/4, buffering 6. */
	static const u8 sps[] = {
		0, 0, 1, 0x42, 1, 6, 1, 0x60, 0, 0, 3, 0, 0x80, 0, 0, 3, 0, 0,
		3, 0, 0x5a, 0, 0, 0xa0, 0x0d, 8, 0x0f, 0x16, 0x59, 0xb3,
		0x29, 0x9c, 0xc5, 0xc9, 0x69, 0x17, 0x20, 0x0f, 0xe1, 0, 0x82,
	};
	static const u8 boundary[] = {
		0, 0, 1, 0x48, 1, 0x80, /* EOS before first VCL */
		0, 0, 1, 0x28, 1, 0xae,
		0, 0, 1, 0x50, 1, 0x80, /* suffix SEI */
		0, 0, 0, 1, 0x4a, 1, 0x80, /* EOB after the picture */
	};
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, sps, sizeof(sps), &au), 0);
	KUNIT_EXPECT_EQ(test, headers->sps[0].limits.max_dec_pic_buffering, 6U);
	KUNIT_EXPECT_EQ(test, headers->sps[0].limits.max_num_reorder_pics, 4U);
	KUNIT_EXPECT_EQ(test, headers->sps[0].limits.max_latency_increase_plus1, 0U);
	headers->pps[0].valid = true;
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, boundary, sizeof(boundary), &au), 0);
	KUNIT_EXPECT_TRUE(test, au.cvs_boundary_before);
	KUNIT_EXPECT_TRUE(test, au.cvs_boundary_after);
	KUNIT_EXPECT_TRUE(test, au.picture.pic_output);
}

static void malformed_headers_do_not_install_partial_parameters(struct kunit *test)
{
	static const u8 truncated[] = { 0, 0, 1, 0x44, 1 };
	static const u8 unknown[] = { 0, 0, 1, 0x26, 1, 0xaf };
	static const u8 continuation[] = { 0, 0, 1, 2, 1, 0x40 };
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, truncated, sizeof(truncated), &au),
			-EINVAL);
	KUNIT_EXPECT_FALSE(test, headers->pps[0].valid);
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, unknown, sizeof(unknown), &au),
			-EINVAL);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, continuation,
						    sizeof(continuation), &au), 0);
	KUNIT_EXPECT_FALSE(test, au.has_picture);
}

static void nonzero_layer_nals_leave_main_picture_intact(struct kunit *test)
{
	/* Truncated enhancement PPS, enhancement EOS and slice around main VCL. */
	u8 data[] = {
		0, 0, 1, 0x44, 9,
		0, 0, 1, 0x48, 9, 0x80,
		0, 0, 1, 2, 1, 0xd8, 0x9e,
		0, 0, 1, 2, 9, 0x80,
	};
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, opflag_params,
						    sizeof(opflag_params), &au), 0);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, data, sizeof(data), &au), 0);
	KUNIT_EXPECT_TRUE(test, au.has_picture);
	KUNIT_EXPECT_EQ(test, au.picture.nal_type, (u8)1);
	KUNIT_EXPECT_FALSE(test, au.picture.pic_output);
	KUNIT_EXPECT_FALSE(test, au.cvs_boundary_before);
	KUNIT_EXPECT_FALSE(test, au.cvs_boundary_after);
	/* Header constraints still apply to ignored enhancement NALs. */
	data[3] |= 0x80;
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, data, sizeof(data), &au), -EINVAL);
	data[3] &= ~0x80;
	data[4] = 8;
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, data, sizeof(data), &au), -EINVAL);
}

static void reserved_vcl_nals_leave_main_picture_intact(struct kunit *test)
{
	u8 data[] = {
		0, 0, 1, 0, 1, 0x80,
		0, 0, 1, 2, 1, 0xd8, 0x9e,
		0, 0, 1, 0, 1, 0x80,
	};
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;
	u8 type;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, opflag_params,
						    sizeof(opflag_params), &au), 0);
	for (type = 10; type < 32; type++) {
		if (type >= 16 && type <= 21)
			continue;
		data[3] = type << 1;
		data[16] = type << 1;
		KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, data, sizeof(data), &au), 0);
		KUNIT_EXPECT_TRUE(test, au.has_picture);
		KUNIT_EXPECT_EQ(test, au.picture.nal_type, (u8)1);
		KUNIT_EXPECT_FALSE(test, au.picture.pic_output);
	}
}

static struct kunit_case hevc_headers_cases[] = {
	KUNIT_CASE(output_flags_and_parameter_replacement),
	KUNIT_CASE(highest_sublayer_limits_and_cvs_markers),
	KUNIT_CASE(malformed_headers_do_not_install_partial_parameters),
	KUNIT_CASE(nonzero_layer_nals_leave_main_picture_intact),
	KUNIT_CASE(reserved_vcl_nals_leave_main_picture_intact),
	{}
};

static struct kunit_suite hevc_headers_suite = {
	.name = "amlogic-hevc-headers",
	.test_cases = hevc_headers_cases,
};

kunit_test_suite(hevc_headers_suite);
