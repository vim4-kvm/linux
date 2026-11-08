// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "hevc_dpb.h"

#include "hevc_headers_kunit_data.h"

static void output_flags_and_parameter_replacement(struct kunit *test)
{
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
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, opflag_hidden,
						    sizeof(opflag_hidden), &au), 0);
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
	u8 data[64] = {
		0, 0, 1, 0x44, 9,
		0, 0, 1, 0x48, 9, 0x80,
		0, 0, 1, 2, 1, 0xd8, 0x9e,
		0, 0, 1, 2, 9, 0x80,
	};
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	memcpy(data + 10, opflag_hidden, sizeof(opflag_hidden));
	memcpy(data + 10 + sizeof(opflag_hidden), "\0\0\1\2\11\200", 6);
	/* Exclude unused space after the enhancement slice. */
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, opflag_params,
						    sizeof(opflag_params), &au), 0);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, data,
						    16 + sizeof(opflag_hidden), &au), 0);
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
	u8 data[12 + sizeof(opflag_hidden)] = {
		0, 0, 1, 0, 1, 0x80,
	};
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;
	u8 type;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	memcpy(data + 6, opflag_hidden, sizeof(opflag_hidden));
	memcpy(data + 6 + sizeof(opflag_hidden), "\0\0\1\0\1\200", 6);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, opflag_params,
						    sizeof(opflag_params), &au), 0);
	for (type = 10; type < 32; type++) {
		if (type >= 16 && type <= 21)
			continue;
		data[3] = type << 1;
		data[9 + sizeof(opflag_hidden)] = type << 1;
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
						    sizeof(ltrpsps_params), &au), 0);
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

static void inline_prediction_and_long_term_source_boundaries(struct kunit *test)
{
	/* Small complete syntax prefixes isolate 7-52's SPS/explicit boundary. */
	static const u8 mixed[] = {
		0, 0, 1, 2, 1, 0xd2, 0xa3, 0x4f, 0x22, 0x36, 0xbe, 0x77,
	};
	static const u8 single[] = { 0x00, 0x00, 0x01, 0x02, 0x01, 0xd2, 0xa3, 0x5b, 0x80 };
	static const u8 predicted[] = { 0x00, 0x00, 0x01, 0x02, 0x01, 0xd2, 0xa3, 0x5c };
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_au_info au;
	struct h265_sps_prefix *sps;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	sps = &headers->sps[0];
	sps->valid = true;
	sps->log2_max_pic_order_cnt_lsb = 8;
	sps->long_term_ref_pics_present = true;
	sps->num_long_term_ref_pics_sps = 2;
	sps->lt_poc_lsb[1] = 249;
	sps->lt_used = BIT(1);
	headers->pps[0].valid = true;
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, mixed, sizeof(mixed), &au), 0);
	KUNIT_EXPECT_EQ(test, au.picture.rps.num_long_term_sps, 1);
	KUNIT_EXPECT_EQ(test, au.picture.rps.num_long_term_pics, 2);
	KUNIT_EXPECT_EQ(test, au.picture.rps.lt[0].sps_index, 1);
	KUNIT_EXPECT_EQ(test, au.picture.rps.lt[0].poc_lsb, 249);
	KUNIT_EXPECT_TRUE(test, au.picture.rps.lt[0].used);
	KUNIT_EXPECT_EQ(test, au.picture.rps.lt[0].cycle, 3U);
	KUNIT_EXPECT_EQ(test, au.picture.rps.lt[1].cycle, 1U);
	KUNIT_EXPECT_EQ(test, au.picture.rps.lt[2].cycle, 3U);
	sps->num_long_term_ref_pics_sps = 1;
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, single, sizeof(single), &au), 0);
	KUNIT_EXPECT_EQ(test, au.picture.rps.lt[0].sps_index, 0);
	KUNIT_EXPECT_EQ(test, au.picture.rps.lt[0].cycle, 2U);
	KUNIT_EXPECT_EQ(test, h265_headers_parse_au(headers, mixed, 8, &au), -EINVAL);
	/* Inline prediction shifts [-1] by +1: zero disappears, +1 remains. */
	sps->long_term_ref_pics_present = false;
	sps->num_short_term_ref_pic_sets = 1;
	sps->st_rps[0].num_delta_pocs = 1;
	sps->st_rps[0].delta_poc[0] = -1;
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, predicted,
						    sizeof(predicted), &au), 0);
	KUNIT_EXPECT_EQ(test, au.picture.rps.st.num_delta_pocs, 1);
	KUNIT_EXPECT_EQ(test, au.picture.rps.st.num_positive_pics, 1);
	KUNIT_EXPECT_EQ(test, au.picture.rps.st.delta_poc[0], 1);
	KUNIT_EXPECT_EQ(test, au.picture.rps.st.used, 1);
}

static void real_slice_rps_msb_cycles(struct kunit *test)
{
	static const struct {
		int poc;
		const u8 *slice;
		size_t size;
		const u16 *rpm;
		int pocs[6], count;
		int expected[4], used;
	} cases[] = {
		{ 330, rpsd_330_slice, sizeof(rpsd_330_slice), rpsd_330_rpm,
		  { 326, 300, 70 }, 3, { 326, 70 }, 2 },
		{ 847, rpsd_847_slice, sizeof(rpsd_847_slice), rpsd_847_rpm,
		  { 505, 761, 154 }, 3, { 761, 505 }, 2 },
		{ 1108, rpsd_1108_slice, sizeof(rpsd_1108_slice), rpsd_1108_rpm,
		  { 1017, 761, 505, 1103, 1104, 154 }, 6,
		  { 1017, 1103, 505, 1104 }, 4 },
	};
	struct h265_headers *headers = kunit_kzalloc(test, sizeof(*headers), GFP_KERNEL);
	struct h265_dpb *h = kunit_kzalloc(test, sizeof(*h), GFP_KERNEL);
	struct h265_au_info au;
	struct h265_resolved_rps snapshot;
	unsigned int c, i;

	KUNIT_ASSERT_NOT_NULL(test, headers);
	KUNIT_ASSERT_NOT_NULL(test, h);
	KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, rpsd_params,
						    sizeof(rpsd_params), &au), 0);
	for (c = 0; c < ARRAY_SIZE(cases); c++) {
		memset(h, 0, sizeof(*h));
		h->cur = -1;
		memcpy(h->p, cases[c].rpm, sizeof(h->p));
		h->curr_poc = cases[c].poc;
		h->nal_type = 1;
		h->slice_type = H265_P_SLICE;
		h->pic_w = h->p[P_PIC_WIDTH];
		h->pic_h = h->p[P_PIC_HEIGHT];
		for (i = 0; i < cases[c].count; i++)
			h->pic[i] = (struct h265_pic) {
				.valid = true, .referenced = true, .poc = cases[c].pocs[i],
				.width = h->pic_w, .height = h->pic_h,
			};
		KUNIT_ASSERT_EQ(test, h265_headers_parse_au(headers, cases[c].slice,
							    cases[c].size, &au), 0);
		KUNIT_ASSERT_EQ(test, h265_resolve_rps(h, &au.picture.rps, &h->rps), 0);
		h265_apply_rps(h, &h->rps);
		h->cur = 17;
		KUNIT_ASSERT_EQ(test, h265_set_ref_lists(h, &h->rps), 0);
		KUNIT_EXPECT_MEMEQ(test, h->ref_poc_l0, cases[c].expected,
				   cases[c].used * sizeof(int));
	}
	/* 1108 retains 1017 and 505, even with the third LSB249 cycle in DPB. */
	KUNIT_EXPECT_FALSE(test, h->pic[1].referenced);
	h->pic[1].referenced = true;
	snapshot = h->rps;
	h->pic[0].valid = false;
	KUNIT_EXPECT_EQ(test, h265_resolve_rps(h, &au.picture.rps, &h->rps), -EINVAL);
	h->pic[0].valid = true;
	au.picture.rps.lt[0].msb_present = false;
	KUNIT_EXPECT_EQ(test, h265_resolve_rps(h, &au.picture.rps, &h->rps), -EINVAL);
	au.picture.rps.lt[0].msb_present = true;
	au.picture.rps.num_long_term_pics--;
	KUNIT_EXPECT_EQ(test, h265_resolve_rps(h, &au.picture.rps, &h->rps), -EINVAL);
	au.picture.rps.num_long_term_pics++;
	au.picture.rps.lt[0].used = false;
	KUNIT_EXPECT_EQ(test, h265_resolve_rps(h, &au.picture.rps, &h->rps), -EINVAL);
	au.picture.rps.lt[0].used = true;
	swap(au.picture.rps.lt[0], au.picture.rps.lt[1]);
	KUNIT_EXPECT_EQ(test, h265_resolve_rps(h, &au.picture.rps, &h->rps), -EINVAL);
	swap(au.picture.rps.lt[0], au.picture.rps.lt[1]);
	au.picture.rps.poc_lsb ^= 1;
	KUNIT_EXPECT_EQ(test, h265_resolve_rps(h, &au.picture.rps, &h->rps), -EINVAL);
	au.picture.rps.poc_lsb ^= 1;
	KUNIT_EXPECT_MEMEQ(test, &h->rps, &snapshot, sizeof(snapshot));
	for (i = 0; i < 6; i++)
		KUNIT_EXPECT_TRUE(test, h->pic[i].referenced);
	/* An unused following picture may already be absent after random access. */
	h->pic[5].valid = false;
	KUNIT_ASSERT_EQ(test, h265_resolve_rps(h, &au.picture.rps, &h->rps), 0);
	KUNIT_EXPECT_EQ(test, h->rps.lt_foll.ref[0].slot, -1);
}

static struct kunit_case hevc_headers_cases[] = {
	KUNIT_CASE(real_slice_rps_msb_cycles),
	KUNIT_CASE(inline_prediction_and_long_term_source_boundaries),
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
