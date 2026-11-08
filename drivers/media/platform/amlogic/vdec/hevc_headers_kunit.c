// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "hevc_headers.h"

#include "hevc_headers_kunit_data.h"

static void output_flags_and_parameter_replacement(struct kunit *test)
{
	static const u8 hidden[] = { 0, 0, 1, 2, 1, 0xd8, 0x9e };
	static const u8 idr[] = { 0, 0, 1, 0x26, 1, 0xaf, 0x8c };
	/* Replace PPS0 with output_flag_present_flag=0, then the same I prefix. */
	static const u8 replace[] = { 0, 0, 1, 0x44, 1, 0xc1, 0x90, 0x95, 0x81, 0x12,
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
	static const u8 boundary[] = {
		0, 0, 1, 0x48, 1, 0x80, /* EOS before first VCL */
		0, 0, 1, 0x28, 1, 0xae,
		0, 0, 1, 0x50, 1, 0x80, /* suffix SEI */
		0, 0, 0, 1, 0x4a, 1, 0x80, /* EOB after the picture */
	};
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, bumping_params,
						    sizeof(bumping_params), &au), 0);
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

static void sps_reference_tables_and_wavefront_geometry(struct kunit *test)
{
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;
	const struct h265_sps_prefix *sps;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, ltrpsps_params,
						    sizeof(ltrpsps_params), &au), -EOPNOTSUPP);
	sps = &headers->sps[0];
	KUNIT_ASSERT_TRUE(test, sps->valid);
	KUNIT_EXPECT_EQ(test, sps->width, 416U);
	KUNIT_EXPECT_EQ(test, sps->log2_ctb_size, 6);
	KUNIT_EXPECT_EQ(test, sps->num_short_term_ref_pic_sets, 12);
	KUNIT_EXPECT_EQ(test, sps->st_rps[0].num_delta_pocs, 4);
	KUNIT_EXPECT_EQ(test, sps->st_rps[0].delta_poc[0], -8);
	KUNIT_EXPECT_EQ(test, sps->st_rps[0].delta_poc[3], -16);
	KUNIT_EXPECT_EQ(test, sps->st_rps[11].num_delta_pocs, 0);
	KUNIT_EXPECT_EQ(test, sps->num_long_term_ref_pics_sps, 8);
	KUNIT_EXPECT_EQ(test, sps->lt_used, 0xc6U);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, wpp_params,
						    sizeof(wpp_params), &au), 0);
	KUNIT_EXPECT_EQ(test, headers->sps[0].width, 64U);
	KUNIT_EXPECT_EQ(test, headers->sps[0].log2_ctb_size, 6);
	KUNIT_EXPECT_TRUE(test, headers->pps[0].entropy_coding_sync_enabled);
	/* A truncated replacement must retain the previous complete SPS. */
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, ltrpsps_params, 28, &au),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, headers->sps[0].width, 64U);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, rpsd_params,
						    sizeof(rpsd_params), &au), 0);
	KUNIT_EXPECT_EQ(test, headers->sps[0].log2_max_pic_order_cnt_lsb, 8);
}

static void nonempty_sps_long_term_list_rejects_the_au(struct kunit *test)
{
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	u8 trailing_sps[sizeof(hang_lt0_idr) + sizeof(hang_lt1_sps)];
	struct h265_au_info au;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	/* Parameter-set-only AUs must be rejected before firmware parses the SPS. */
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, hang_lt1_sps,
						    sizeof(hang_lt1_sps), &au), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, au.unsupported, H265_UNSUPPORTED_SPS_LT);
	KUNIT_EXPECT_FALSE(test, au.has_picture);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_lt0_sps,
						    sizeof(hang_lt0_sps), &au), 0);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_lt0_pps,
						    sizeof(hang_lt0_pps), &au), 0);
	/* An unsupported SPS later in an AU still prevents feeding the whole AU. */
	memcpy(trailing_sps, hang_lt0_idr, sizeof(hang_lt0_idr));
	memcpy(trailing_sps + sizeof(hang_lt0_idr), hang_lt1_sps, sizeof(hang_lt1_sps));
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, trailing_sps,
						    sizeof(trailing_sps), &au), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, au.unsupported, H265_UNSUPPORTED_SPS_LT);
}

static void cached_long_term_sps_blocks_picture_until_replaced(struct kunit *test)
{
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_lt1_sps,
						    sizeof(hang_lt1_sps), &au), -EOPNOTSUPP);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_lt0_pps,
						    sizeof(hang_lt0_pps), &au), 0);
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, hang_lt0_idr,
						    sizeof(hang_lt0_idr), &au), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, au.unsupported, H265_UNSUPPORTED_SPS_LT);
	KUNIT_EXPECT_FALSE(test, au.has_picture);
	/* LT present with an empty table is supported and clears the old limit. */
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_lt0_sps,
						    sizeof(hang_lt0_sps), &au), 0);
	KUNIT_EXPECT_TRUE(test, headers->sps[0].long_term_ref_pics_present);
	KUNIT_EXPECT_EQ(test, headers->sps[0].num_long_term_ref_pics_sps, 0);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_lt0_idr,
						    sizeof(hang_lt0_idr), &au), 0);
	KUNIT_EXPECT_TRUE(test, au.has_picture);
	KUNIT_EXPECT_EQ(test, au.unsupported, H265_UNSUPPORTED_NONE);
}

static void wavefront_requires_more_than_one_column(struct kunit *test)
{
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	/* WPP parameter sets are safe; the restriction applies on activation. */
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, wpp_params,
						    sizeof(wpp_params), &au), 0);
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, hang_wpp1_idr,
						    sizeof(hang_wpp1_idr), &au), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, au.unsupported, H265_UNSUPPORTED_WPP_ONE_COLUMN);
	KUNIT_EXPECT_FALSE(test, au.has_picture);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_wpp0_sps,
						    sizeof(hang_wpp0_sps), &au), 0);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_wpp0_pps,
						    sizeof(hang_wpp0_pps), &au), 0);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_wpp0_idr,
						    sizeof(hang_wpp0_idr), &au), 0);
	KUNIT_EXPECT_TRUE(test, au.has_picture);
	KUNIT_EXPECT_EQ(test, headers->sps[0].width, 64U);
	KUNIT_EXPECT_FALSE(test, headers->pps[0].entropy_coding_sync_enabled);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_wpp2_sps,
						    sizeof(hang_wpp2_sps), &au), 0);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_wpp2_pps,
						    sizeof(hang_wpp2_pps), &au), 0);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, hang_wpp2_idr,
						    sizeof(hang_wpp2_idr), &au), 0);
	KUNIT_EXPECT_TRUE(test, au.has_picture);
	KUNIT_EXPECT_EQ(test, headers->sps[0].width, 128U);
	KUNIT_EXPECT_TRUE(test, headers->pps[0].entropy_coding_sync_enabled);
	KUNIT_EXPECT_EQ(test, au.unsupported, H265_UNSUPPORTED_NONE);
}

static struct kunit_case hevc_headers_cases[] = {
	KUNIT_CASE(nonempty_sps_long_term_list_rejects_the_au),
	KUNIT_CASE(cached_long_term_sps_blocks_picture_until_replaced),
	KUNIT_CASE(wavefront_requires_more_than_one_column),
	KUNIT_CASE(sps_reference_tables_and_wavefront_geometry),
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
