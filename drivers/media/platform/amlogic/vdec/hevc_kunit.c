// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include "hevc_priv.h"

static void reset_params(struct h265_dpb *h)
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
	struct h265_dpb *h = kunit_kzalloc(test, sizeof(*h), GFP_KERNEL);

	if (!h)
		return -ENOMEM;
	reset_params(h);
	test->priv = h;
	return 0;
}

static void poc_wrap_and_random_access(struct kunit *test)
{
	static const unsigned int excluded[] = { 0, 2, 4, 6, 7, 8, 9 };
	struct h265_dpb *h = test->priv;
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
	struct h265_dpb *h = test->priv;
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
	struct h265_dpb *h = test->priv;

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

static void first_slice_ignores_absent_segment_address(struct kunit *test)
{
	struct h265_dpb *h = test->priv;

	h->p[P_PIC_WIDTH] = 832;
	h->p[P_PIC_HEIGHT] = 480;
	h->p[P_FIRST_SLICE_SEGMENT_IN_PIC_FLAG] = 1;
	/* The address syntax element is absent on the first slice. */
	h->p[P_SLICE_SEGMENT_ADDRESS] = U16_MAX;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), 0);
	h->p[P_FIRST_SLICE_SEGMENT_IN_PIC_FLAG] = 0;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EINVAL);
	h->p[P_SLICE_SEGMENT_ADDRESS] = 26;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), 0);
	h->p[P_SLICE_SEGMENT_ADDRESS] = 13 * 8;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EINVAL);
}

static void missing_and_reordered_references(struct kunit *test)
{
	static const int pocs[] = { 3, 5, 1 };
	struct h265_dpb *h = test->priv;
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

static void long_term_references_survive_poc_lsb_wrap(struct kunit *test)
{
	static const int pocs[] = { 250, 210, 70, 230 };
	static const u16 rps[] = { 0x3ff6, 0x3fce, 0x6042, 0x60e2, RPS_END };
	struct h265_dpb *h = test->priv;
	unsigned int i;

	h->curr_poc = 260;
	h->cur = 17;
	h->pic_w = 416;
	h->pic_h = 240;
	h->slice_type = H265_P_SLICE;
	h->nal_type = 1;
	h->p[P_SLICE_TYPE] = H265_P_SLICE;
	h->p[P_NUM_REF_IDX_L0_ACTIVE] = ARRAY_SIZE(pocs);
	memcpy(&h->p[P_CUR_RPS], rps, sizeof(rps));
	for (i = 0; i < ARRAY_SIZE(pocs); i++) {
		h->pic[i].valid = true;
		h->pic[i].referenced = true;
		h->pic[i].width = 416;
		h->pic[i].height = 240;
		h->pic[i].poc = pocs[i];
	}
	h265_apply_rps(h);
	KUNIT_ASSERT_EQ(test, h265_set_ref_lists(h), 0);
	KUNIT_EXPECT_MEMEQ(test, h->ref_poc_l0, pocs, sizeof(pocs));
	KUNIT_EXPECT_TRUE(test, h->pic[2].referenced);
	KUNIT_EXPECT_TRUE(test, h->pic[2].long_term);
	KUNIT_EXPECT_TRUE(test, h->pic[3].referenced);
	KUNIT_EXPECT_TRUE(test, h->pic[3].long_term);
	/* Two retained POC cycles with the same LSB cannot be guessed apart. */
	h->pic[4] = h->pic[2];
	h->pic[4].poc = 326;
	KUNIT_EXPECT_EQ(test, h265_set_ref_lists(h), -EINVAL);
	/* A short-term delta must still identify a complete POC exactly. */
	h->p[P_CUR_RPS] = 0x2004;
	h->p[P_CUR_RPS + 1] = RPS_END;
	h->p[P_NUM_REF_IDX_L0_ACTIVE] = 1;
	h->pic[4].poc = 8;
	KUNIT_EXPECT_EQ(test, h265_set_ref_lists(h), -EINVAL);
}

static void previous_decoded_picture_can_be_long_term(struct kunit *test)
{
	struct h265_dpb *h = test->priv;
	int slot;

	h->curr_poc = 326;
	h->cur = 0;
	h->nslots = 4;
	h->pic_w = 416;
	h->pic_h = 240;
	h->slice_type = H265_P_SLICE;
	h->nal_type = 1;
	h->p[P_SLICE_TYPE] = H265_P_SLICE;
	h->p[P_NUM_REF_IDX_L0_ACTIVE] = 1;
	/* LT delta zero names POC 70 in the previous eight-bit POC cycle. */
	h->p[P_CUR_RPS] = 0x6000;
	h->p[P_CUR_RPS + 1] = RPS_END;
	h->pic[0].valid = true;
	h->pic[0].referenced = true;
	h->pic[0].width = 416;
	h->pic[0].height = 240;
	h->pic[0].poc = 70;

	/* RPS is applied while cur still identifies the preceding decoded picture. */
	h265_apply_rps(h);
	KUNIT_ASSERT_TRUE(test, h->pic[0].referenced);
	KUNIT_EXPECT_TRUE(test, h->pic[0].long_term);
	slot = h265_free_slot(h);
	KUNIT_ASSERT_EQ(test, slot, 1);
	h->cur = slot;
	h->pic[slot] = h->pic[0];
	h->pic[slot].poc = h->curr_poc;
	/* The new picture shares the LT LSB, but must not become its own reference. */
	KUNIT_ASSERT_EQ(test, h265_set_ref_lists(h), 0);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[0], 70);
}

static void mv_storage_covers_edge_ctus(struct kunit *test)
{
	static const u32 sizes[][2] = {
		{ 256, 256 }, { 720, 720 }, { 832, 608 }, { 1920, 1080 }, { 4096, 2304 },
	};
	unsigned int i, log2_ctu;

	/* Both edge geometries overrun the old 64 KiB allocation with 64x64 CTUs. */
	KUNIT_EXPECT_EQ(test, h265_mv_buf_size(720, 720), (size_t)SZ_128K);
	KUNIT_EXPECT_EQ(test, h265_mv_buf_size(832, 608), (size_t)SZ_128K);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		size_t capacity = h265_mv_buf_size(sizes[i][0], sizes[i][1]);

		for (log2_ctu = 4; log2_ctu <= 6; log2_ctu++) {
			u32 columns = DIV_ROUND_UP(sizes[i][0], BIT(log2_ctu));
			u32 rows = DIV_ROUND_UP(sizes[i][1], BIT(log2_ctu));
			u32 unit = 1U << (2 * log2_ctu - 3);

			KUNIT_EXPECT_GE(test, capacity, (size_t)columns * rows * unit);
		}
	}
}

static void rps_and_output_own_picture_slots(struct kunit *test)
{
	struct h265_dpb *h = test->priv;
	unsigned int i;

	h->nslots = 4;
	h->curr_poc = 4;
	h->nal_type = 1;
	/* POC 0 is retained for a later picture but unused by this slice. */
	h->p[P_CUR_RPS] = 0x1ffc;
	h->p[P_CUR_RPS + 1] = 0x3ffe;
	h->p[P_CUR_RPS + 2] = RPS_END;
	for (i = 0; i < 3; i++) {
		h->pic[i].valid = true;
		h->pic[i].referenced = true;
	}
	h->pic[0].poc = 0;
	h->pic[1].poc = 2;
	h->pic[2].poc = 3;
	h->pic[2].output_pending = true;
	h265_apply_rps(h);
	KUNIT_EXPECT_TRUE(test, h->pic[0].referenced);
	KUNIT_EXPECT_TRUE(test, h->pic[1].referenced);
	KUNIT_EXPECT_FALSE(test, h->pic[2].referenced);
	/* An unreferenced reordered output must survive until it is delivered. */
	KUNIT_EXPECT_EQ(test, h265_free_slot(h), 3);
	h->pic[3].valid = true;
	h->pic[3].referenced = true;
	KUNIT_EXPECT_EQ(test, h265_free_slot(h), -ENOBUFS);
	h->pic[2].output_pending = false;
	KUNIT_EXPECT_EQ(test, h265_free_slot(h), 2);
}

static void irap_releases_previous_references(struct kunit *test)
{
	static const unsigned int nals[] = {
		H265_NAL_BLA_W_LP, H265_NAL_BLA_W_RADL, H265_NAL_BLA_N_LP,
		H265_NAL_IDR_W_RADL, H265_NAL_IDR_N_LP,
	};
	struct h265_dpb *h = test->priv;
	unsigned int i;

	h->curr_poc = 0;
	h->pic[0].valid = true;
	h->pic[0].poc = -1;
	h->p[P_CUR_RPS] = 0x3fff;
	h->p[P_CUR_RPS + 1] = RPS_END;
	for (i = 0; i < ARRAY_SIZE(nals); i++) {
		h->nal_type = nals[i];
		h->pic[0].referenced = true;
		h265_apply_rps(h);
		KUNIT_EXPECT_FALSE(test, h->pic[0].referenced);
	}
}

static void output_orders_b_pictures_before_new_epoch(struct kunit *test)
{
	static const int pocs[] = { 0, 4, 2, 1, 0 };
	static const int slots[] = { 0, 3, 2, 1, 4 };
	struct h265_dpb *h = test->priv;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pocs); i++) {
		h->pic[i].poc = pocs[i];
		h->pic[i].output_pending = true;
	}
	/* A new IDR's POC 0 must follow the previous GOP's reordered pictures. */
	h->pic[4].epoch = 1;
	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		int slot = h265_next_output(h, true);

		KUNIT_ASSERT_EQ(test, slot, slots[i]);
		h->pic[slot].output_pending = false;
	}
	KUNIT_EXPECT_EQ(test, h265_next_output(h, true), -1);
}

static void output_respects_each_pictures_reorder_limit(struct kunit *test)
{
	static const int pocs[] = { 4, 6, 8, 10, 2 };
	struct h265_dpb *h = test->priv;
	unsigned int i;

	/* The initial sequence had no reordering; a later SPS requires four. */
	h->reorder_limit = 0;
	for (i = 0; i < ARRAY_SIZE(pocs); i++) {
		h->pic[i].poc = pocs[i];
		h->pic[i].reorder_limit = 4;
		if (i < 4)
			h->pic[i].output_pending = true;
	}
	h->pending = 4;
	KUNIT_EXPECT_EQ(test, h265_next_output(h, false), -1);
	/* A later decoded picture has a lower POC and must precede POC 4. */
	h->pic[4].output_pending = true;
	h->pending++;
	KUNIT_EXPECT_EQ(test, h265_next_output(h, false), 4);
	h->pic[4].output_pending = false;
	h->pending--;
	KUNIT_EXPECT_EQ(test, h265_next_output(h, false), -1);
	KUNIT_EXPECT_EQ(test, h265_next_output(h, true), 0);
	/* An IRAP epoch boundary flushes the older sequence's remaining output. */
	h->epoch++;
	KUNIT_EXPECT_EQ(test, h265_next_output(h, false), 0);
}

static void continuous_cra_keeps_leading_picture_order(struct kunit *test)
{
	static const int pocs[] = { 26, 27, 28, 29, 30, 31, 32, 25 };
	struct h265_dpb *h = test->priv;
	unsigned int i;

	h->nal_type = H265_NAL_IDR_N_LP;
	h->poc_random_access = H265_MAX_INT;
	KUNIT_ASSERT_FALSE(test, h265_update_random_access(h));
	KUNIT_ASSERT_EQ(test, h->epoch, 1U);
	h->nal_type = H265_NAL_CRA;
	h->curr_poc = 32;
	KUNIT_ASSERT_FALSE(test, h265_update_random_access(h));
	KUNIT_EXPECT_EQ(test, h->epoch, 1U);
	for (i = 0; i < ARRAY_SIZE(pocs); i++) {
		h->pic[i].poc = pocs[i];
		h->pic[i].epoch = h->epoch;
		h->pic[i].reorder_limit = 7;
		if (i < 7)
			h->pic[i].output_pending = true;
	}
	h->pending = 7;
	KUNIT_EXPECT_EQ(test, h265_next_output(h, false), -1);
	h->nal_type = H265_NAL_RASL_R;
	h->curr_poc = 25;
	KUNIT_ASSERT_FALSE(test, h265_update_random_access(h));
	h->pic[7].output_pending = true;
	h->pending++;
	KUNIT_EXPECT_EQ(test, h265_next_output(h, false), 7);
}

static void restarted_cra_skips_unavailable_rasl(struct kunit *test)
{
	struct h265_dpb *h = test->priv;

	/* A source change rebuilt the picture pool before replaying this CRA. */
	h->poc_random_access = H265_MAX_INT;
	h->nal_type = H265_NAL_CRA;
	h->curr_poc = 32;
	KUNIT_ASSERT_FALSE(test, h265_update_random_access(h));
	KUNIT_EXPECT_EQ(test, h->poc_random_access, 32);
	KUNIT_EXPECT_EQ(test, h->epoch, 1U);
	h->nal_type = H265_NAL_RASL_R;
	h->curr_poc = 28;
	KUNIT_EXPECT_TRUE(test, h265_update_random_access(h));
	h->nal_type = H265_NAL_RASL_N;
	KUNIT_EXPECT_TRUE(test, h265_update_random_access(h));
	/* RADL pictures do not require the discarded pre-CRA references. */
	h->nal_type = 6;
	KUNIT_EXPECT_FALSE(test, h265_update_random_access(h));
	h->nal_type = H265_NAL_IDR_N_LP;
	h->curr_poc = 0;
	KUNIT_EXPECT_FALSE(test, h265_update_random_access(h));
	KUNIT_EXPECT_EQ(test, h->epoch, 2U);
	KUNIT_EXPECT_EQ(test, h->poc_random_access, -H265_MAX_INT);
	h->nal_type = H265_NAL_CRA;
	h->curr_poc = 32;
	KUNIT_EXPECT_FALSE(test, h265_update_random_access(h));
	h->nal_type = H265_NAL_RASL_R;
	h->curr_poc = 25;
	KUNIT_EXPECT_FALSE(test, h265_update_random_access(h));
}

static struct kunit_case hevc_test_cases[] = {
	KUNIT_CASE(poc_wrap_and_random_access),
	KUNIT_CASE(invalid_firmware_parameters),
	KUNIT_CASE(first_slice_ignores_absent_segment_address),
	KUNIT_CASE(tile_boundaries),
	KUNIT_CASE(missing_and_reordered_references),
	KUNIT_CASE(long_term_references_survive_poc_lsb_wrap),
	KUNIT_CASE(previous_decoded_picture_can_be_long_term),
	KUNIT_CASE(capture_sao_capacity),
	KUNIT_CASE(mv_storage_covers_edge_ctus),
	KUNIT_CASE(rps_and_output_own_picture_slots),
	KUNIT_CASE(irap_releases_previous_references),
	KUNIT_CASE(output_orders_b_pictures_before_new_epoch),
	KUNIT_CASE(output_respects_each_pictures_reorder_limit),
	KUNIT_CASE(continuous_cra_keeps_leading_picture_order),
	KUNIT_CASE(restarted_cra_skips_unavailable_rasl),
	{}
};

static struct kunit_suite hevc_test_suite = {
	.name = "amlogic-hevc-parameters",
	.init = hevc_test_init,
	.test_cases = hevc_test_cases,
};

kunit_test_suite(hevc_test_suite);
