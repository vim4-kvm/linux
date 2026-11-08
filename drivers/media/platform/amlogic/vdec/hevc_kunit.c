// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include "hevc_priv.h"

struct hevc_rps_test {
	struct h265_dpb dpb;
	struct h265_slice_rps slice;
};

static struct h265_slice_rps *test_slice(struct h265_dpb *h)
{
	return &container_of(h, struct hevc_rps_test, dpb)->slice;
}

/* Existing list tests specify ST references as RPM; LT syntax is explicit. */
static int resolve_rps(struct h265_dpb *h, struct h265_resolved_rps *resolved)
{
	struct h265_slice_rps *slice = test_slice(h);
	unsigned int i, n = 0;

	slice->poc_bits = h->p[P_LOG2_MAX_POC_LSB_MINUS4] + 4;
	slice->poc_lsb = (u32)h->curr_poc & (BIT(slice->poc_bits) - 1);
	h->p[P_POC_LSB] = slice->poc_lsb;
	memset(&slice->st, 0, sizeof(slice->st));
	for (i = 0; i < H265_MAX_REF && h->p[i] != RPS_END; i++) {
		u16 word = h->p[i];

		if (word & BIT(RPS_LT_BIT))
			continue;
		slice->st.delta_poc[n] = sign_extend32(word & 0x1fff, 12);
		if (word & BIT(RPS_USED_BIT))
			slice->st.used |= BIT(n);
		n++;
	}
	slice->st.num_delta_pocs = n;
	return h265_resolve_rps(h, slice, resolved);
}

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
	struct hevc_rps_test *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	struct h265_dpb *h;

	if (!state)
		return -ENOMEM;
	h = &state->dpb;
	reset_params(h);
	test->priv = h;
	return 0;
}

static int resolve_ref_lists(struct h265_dpb *h)
{
	int ret = resolve_rps(h, &h->rps);

	return ret ?: h265_set_ref_lists(h, &h->rps);
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
	h->p[P_PIC_WIDTH] = 0;
	h->p[P_PIC_HEIGHT] = 0;
	h->p[P_CHROMA_FORMAT_IDC] = 0;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EBADMSG);
	for (i = 0; i < ARRAY_SIZE(shifts); i++) {
		reset_params(h);
		h->p[shifts[i]] = U16_MAX;
		KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EINVAL);
	}
	reset_params(h);
	h->p[P_SLICE_SEGMENT_ADDRESS] = 110;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EINVAL);
}

static void valid_crop_selects_visible_rectangle(struct kunit *test)
{
	struct h265_dpb *h = test->priv;
	struct h265_crop crop;

	h->p[P_PIC_WIDTH] = 832;
	h->p[P_PIC_HEIGHT] = 480;
	h->p[P_CONFORMANCE_WINDOW_FLAG] = 1;
	h->p[P_CONF_WIN_LEFT_OFFSET] = 1;
	h->p[P_CONF_WIN_RIGHT_OFFSET] = 3;
	h->p[P_CONF_WIN_TOP_OFFSET] = 2;
	h->p[P_CONF_WIN_BOTTOM_OFFSET] = 4;
	KUNIT_ASSERT_EQ(test, h265_validate_params(h->p), 0);
	crop = h265_display_crop(h->p);
	KUNIT_EXPECT_EQ(test, crop.left, 2U);
	KUNIT_EXPECT_EQ(test, crop.top, 4U);
	KUNIT_EXPECT_EQ(test, crop.width, 824U);
	KUNIT_EXPECT_EQ(test, crop.height, 468U);
}

static void invalid_crop_keeps_decoding_full_picture(struct kunit *test)
{
	static const u16 offsets[][4] = {
		{ 1, 24576, 0, 120 }, { 1, 57344, 0, 120 },
		{ 0, 0, 1, 239 }, { 1, 415, 0, 0 },
	};
	struct h265_dpb *h = test->priv;
	unsigned int i;

	h->p[P_PIC_WIDTH] = 832;
	h->p[P_PIC_HEIGHT] = 480;
	h->p[P_CONFORMANCE_WINDOW_FLAG] = 1;
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		struct h265_crop crop;

		memcpy(&h->p[P_CONF_WIN_LEFT_OFFSET], offsets[i], sizeof(offsets[i]));
		KUNIT_ASSERT_EQ(test, h265_validate_params(h->p), 0);
		crop = h265_display_crop(h->p);
		KUNIT_EXPECT_EQ(test, crop.left, 0U);
		KUNIT_EXPECT_EQ(test, crop.top, 0U);
		KUNIT_EXPECT_EQ(test, crop.width, 832U);
		KUNIT_EXPECT_EQ(test, crop.height, 480U);
	}
	h->p[P_CHROMA_FORMAT_IDC] = 2;
	KUNIT_EXPECT_EQ(test, h265_validate_params(h->p), -EOPNOTSUPP);
	h->p[P_CHROMA_FORMAT_IDC] = 1;
	h->p[P_PIC_WIDTH] = 0;
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
	struct h265_slice_rps *slice = test_slice(h);
	unsigned int i;

	slice->num_long_term_pics = 1;
	slice->lt[0] = (struct h265_lt_ref) { .poc_lsb = 1, .used = true };
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
	KUNIT_ASSERT_EQ(test, resolve_ref_lists(h), 0);
	KUNIT_EXPECT_MEMEQ(test, h->ref_poc_l0, pocs, sizeof(pocs));
	KUNIT_EXPECT_EQ(test, h->ref_poc_l1[0], 5);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l1[1], 3);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l1[2], 1);
	h->pic[2].valid = false;
	KUNIT_EXPECT_EQ(test, resolve_ref_lists(h), -EINVAL);
	h->pic[2].valid = true;
	h->p[P_MODIFICATION_FLAG] = 1;
	h->p[P_MODIFICATION_LIST] = 2;
	h->p[P_MODIFICATION_LIST + 1] = 0;
	h->p[P_MODIFICATION_LIST + 2] = 1;
	KUNIT_ASSERT_EQ(test, resolve_ref_lists(h), 0);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[0], 1);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[1], 3);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[2], 5);
	h->p[P_MODIFICATION_LIST] = 3;
	KUNIT_EXPECT_EQ(test, resolve_ref_lists(h), -EINVAL);
	slice->num_long_term_pics = 0;
	h->p[P_CUR_RPS] = RPS_END;
	KUNIT_EXPECT_EQ(test, resolve_ref_lists(h), -EINVAL);
	h->slice_type = H265_I_SLICE;
	h->p[P_SLICE_TYPE] = H265_I_SLICE;
	KUNIT_EXPECT_EQ(test, resolve_ref_lists(h), 0);
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
	struct h265_slice_rps *slice = test_slice(h);
	unsigned int i;

	slice->num_long_term_pics = 2;
	slice->lt[0] = (struct h265_lt_ref) { .poc_lsb = 70, .used = true };
	slice->lt[1] = (struct h265_lt_ref) { .poc_lsb = 230, .used = true };
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
	KUNIT_ASSERT_EQ(test, resolve_rps(h, &h->rps), 0);
	h265_apply_rps(h, &h->rps);
	KUNIT_ASSERT_EQ(test, resolve_ref_lists(h), 0);
	KUNIT_EXPECT_MEMEQ(test, h->ref_poc_l0, pocs, sizeof(pocs));
	KUNIT_EXPECT_TRUE(test, h->pic[2].referenced);
	KUNIT_EXPECT_TRUE(test, h->pic[2].long_term);
	KUNIT_EXPECT_TRUE(test, h->pic[3].referenced);
	KUNIT_EXPECT_TRUE(test, h->pic[3].long_term);
	/* Two retained POC cycles with the same LSB cannot be guessed apart. */
	h->pic[4] = h->pic[2];
	h->pic[4].poc = 326;
	KUNIT_EXPECT_EQ(test, resolve_ref_lists(h), -EINVAL);
	/* A short-term delta must still identify a complete POC exactly. */
	h->p[P_CUR_RPS] = 0x2004;
	slice->num_long_term_pics = 0;
	h->p[P_CUR_RPS + 1] = RPS_END;
	h->p[P_NUM_REF_IDX_L0_ACTIVE] = 1;
	h->pic[4].poc = 8;
	KUNIT_EXPECT_EQ(test, resolve_ref_lists(h), -EINVAL);
}

static void previous_decoded_picture_can_be_long_term(struct kunit *test)
{
	struct h265_dpb *h = test->priv;
	struct h265_slice_rps *slice = test_slice(h);
	int slot;

	slice->num_long_term_pics = 1;
	slice->lt[0] = (struct h265_lt_ref) { .poc_lsb = 70, .used = true };
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
	KUNIT_ASSERT_EQ(test, resolve_rps(h, &h->rps), 0);
	h265_apply_rps(h, &h->rps);
	KUNIT_ASSERT_TRUE(test, h->pic[0].referenced);
	KUNIT_EXPECT_TRUE(test, h->pic[0].long_term);
	slot = h265_free_slot(h);
	KUNIT_ASSERT_EQ(test, slot, 1);
	h->cur = slot;
	h->pic[slot] = h->pic[0];
	h->pic[slot].poc = h->curr_poc;
	/* The new picture shares the LT LSB, but must not become its own reference. */
	KUNIT_ASSERT_EQ(test, resolve_ref_lists(h), 0);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[0], 70);
}

static void rps_snapshot_resolves_st_before_lt(struct kunit *test)
{
	static const u16 words[] = { 0x3ffc, 0x1fe2, 0xfffc, RPS_END };
	static const int pocs[] = { 326, 300, 70 };
	struct h265_dpb *h = test->priv;
	struct h265_slice_rps *slice = test_slice(h);
	unsigned int i;

	slice->num_long_term_pics = 1;
	slice->lt[0] = (struct h265_lt_ref) {
		.poc_lsb = 70, .used = true, .msb_present = true, .cycle = 1,
	};
	h->curr_poc = 330;
	h->cur = 0;
	h->nal_type = 1;
	h->slice_type = H265_P_SLICE;
	h->p[P_SLICE_TYPE] = H265_P_SLICE;
	h->p[P_NUM_REF_IDX_L0_ACTIVE] = 2;
	memcpy(h->p, words, sizeof(words));
	for (i = 0; i < ARRAY_SIZE(pocs); i++) {
		h->pic[i].valid = true;
		h->pic[i].referenced = true;
		h->pic[i].poc = pocs[i];
	}
	KUNIT_ASSERT_EQ(test, resolve_rps(h, &h->rps), 0);
	h265_apply_rps(h, &h->rps);
	KUNIT_EXPECT_TRUE(test, h->pic[1].referenced);
	KUNIT_EXPECT_TRUE(test, h->pic[2].long_term);
	/* Allocating the current slot cannot change an already resolved RPS. */
	h->cur = 3;
	h->pic[3] = h->pic[0];
	h->pic[3].poc = h->curr_poc;
	KUNIT_ASSERT_EQ(test, h265_set_ref_lists(h, &h->rps), 0);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[0], 326);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[1], 70);
	/* A later slice may change its list, but must retain the picture RPS. */
	h->p[P_MODIFICATION_FLAG] = 1;
	h->p[P_MODIFICATION_LIST] = 1;
	h->p[P_MODIFICATION_LIST + 1] = 0;
	KUNIT_EXPECT_TRUE(test, h265_rps_matches(&h->rps, h->p));
	KUNIT_ASSERT_EQ(test, h265_set_ref_lists(h, &h->rps), 0);
	KUNIT_EXPECT_EQ(test, h->ref_poc_l0[0], 70);
	h->p[P_CUR_RPS + 4] = 0x1234;
	KUNIT_EXPECT_TRUE(test, h265_rps_matches(&h->rps, h->p));
	h->p[P_CUR_RPS + 2] ^= BIT(RPS_USED_BIT);
	KUNIT_EXPECT_FALSE(test, h265_rps_matches(&h->rps, h->p));
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
	KUNIT_ASSERT_EQ(test, resolve_rps(h, &h->rps), 0);
	h265_apply_rps(h, &h->rps);
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
		KUNIT_ASSERT_EQ(test, resolve_rps(h, &h->rps), 0);
		h265_apply_rps(h, &h->rps);
		KUNIT_EXPECT_FALSE(test, h->pic[0].referenced);
	}
}

static const struct h265_output_limits reorder_four = {
	.max_dec_pic_buffering = 6, .max_num_reorder_pics = 4,
};

static void decoded_for_output(struct h265_dpb *h, unsigned int slot, int poc,
			       const struct h265_output_limits *limits)
{
	h->pic[slot].valid = true;
	h->pic[slot].referenced = true;
	h->pic[slot].poc = poc;
	h265_dpb_end_picture(h, slot, true, limits);
}

static void output_fifo_preserves_bumping_before_a_new_irap(struct kunit *test)
{
	struct h265_dpb *h = test->priv;
	struct h265_picture_info info = { .irap = true, .no_rasl_output = true };

	decoded_for_output(h, 0, 4, &reorder_four);
	decoded_for_output(h, 1, 2, &reorder_four);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), -ENOENT);
	h265_dpb_begin_picture(h, &info, &reorder_four);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 1);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 0);
	decoded_for_output(h, 2, 0, &reorder_four);
	h265_dpb_flush(h);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 2);
	KUNIT_EXPECT_EQ(test, h->pending, 0U);
}

static void output_respects_the_active_sps_reorder_limit(struct kunit *test)
{
	static const int pocs[] = { 4, 6, 8, 10, 2 };
	struct h265_dpb *h = test->priv;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pocs); i++)
		decoded_for_output(h, i, pocs[i], &reorder_four);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 4);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), -ENOENT);
	h265_dpb_flush(h);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 0);
	h265_dpb_flush(h);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 1);
}

static void continuous_cra_keeps_leading_picture_order(struct kunit *test)
{
	static const int pocs[] = { 26, 27, 28, 29, 30, 31, 32, 25 };
	const struct h265_output_limits limits = {
		.max_dec_pic_buffering = 10, .max_num_reorder_pics = 7,
	};
	struct h265_dpb *h = test->priv;
	unsigned int i;

	h->nal_type = H265_NAL_IDR_N_LP;
	h->poc_random_access = H265_MAX_INT;
	KUNIT_ASSERT_FALSE(test, h265_update_random_access(h));
	h->nal_type = H265_NAL_CRA;
	h->curr_poc = 32;
	KUNIT_ASSERT_FALSE(test, h265_update_random_access(h));
	for (i = 0; i < ARRAY_SIZE(pocs); i++)
		decoded_for_output(h, i, pocs[i], &limits);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 7);
}

static void cvs_boundary_discards_waiting_pictures_without_matching_old_pocs(struct kunit *test)
{
	struct h265_picture_info info = { .irap = true, .is_cra = true,
		.no_rasl_output = true };
	struct h265_dpb *h = test->priv;

	decoded_for_output(h, 0, 42, &reorder_four);
	decoded_for_output(h, 1, 44, &reorder_four);
	h265_dpb_end_sequence(h);
	KUNIT_EXPECT_TRUE(test, h->new_cvs);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), -ENOENT);
	KUNIT_EXPECT_TRUE(test, h->pic[0].referenced);
	h265_dpb_begin_picture(h, &info, &reorder_four);
	KUNIT_EXPECT_FALSE(test, h->pic[0].referenced);
	KUNIT_EXPECT_EQ(test, h265_pic_by_poc(h, 42, true), -1);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_discard(h), 0);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_discard(h), 1);
	h265_dpb_flush(h);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), -ENOENT);
	KUNIT_EXPECT_EQ(test, h->pending, 0U);
}

static void hidden_pictures_keep_references_and_fullness_bumps_before_decode(struct kunit *test)
{
	struct h265_dpb *h = test->priv;
	const struct h265_output_limits limits = {
		.max_dec_pic_buffering = 2, .max_num_reorder_pics = 4,
	};
	struct h265_picture_info info = {};

	decoded_for_output(h, 0, 2, &reorder_four);
	decoded_for_output(h, 1, 4, &reorder_four);
	h->pic[0].referenced = false;
	h265_dpb_begin_picture(h, &info, &limits);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 0);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), -ENOENT);
	h->pic[2].valid = true;
	h->pic[2].referenced = true;
	h->pic[2].poc = 6;
	h265_dpb_end_picture(h, 2, false, &reorder_four);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_discard(h), 2);
	KUNIT_EXPECT_TRUE(test, h->pic[2].referenced);
	KUNIT_EXPECT_TRUE(test, h->pic[1].needed_for_output);
}

static void latency_counts_only_later_output_pictures(struct kunit *test)
{
	struct h265_dpb *h = test->priv;
	const struct h265_output_limits limits = {
		.max_dec_pic_buffering = 6, .max_num_reorder_pics = 1,
		.max_latency_increase_plus1 = 1,
	};

	decoded_for_output(h, 0, 4, &reorder_four);
	decoded_for_output(h, 1, 2, &limits);
	KUNIT_EXPECT_EQ(test, h->pic[0].latency_count, 1U);
	KUNIT_EXPECT_EQ(test, h->pic[1].latency_count, 0U);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 1);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), 0);
	KUNIT_EXPECT_EQ(test, h265_dpb_pop_output(h), -ENOENT);
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
	KUNIT_EXPECT_EQ(test, h->poc_random_access, -H265_MAX_INT);
	h->nal_type = H265_NAL_CRA;
	h->curr_poc = 32;
	KUNIT_EXPECT_FALSE(test, h265_update_random_access(h));
	h->nal_type = H265_NAL_RASL_R;
	h->curr_poc = 25;
	KUNIT_EXPECT_FALSE(test, h265_update_random_access(h));
}

#include "hevc_kunit_traces.h"

static void check_trace_outputs(struct kunit *test, struct h265_dpb *h,
				const u16 *expected, unsigned int count, unsigned int *seen)
{
	int slot;

	while (h265_dpb_pop_discard(h) >= 0)
		;
	while ((slot = h265_dpb_pop_output(h)) >= 0) {
		KUNIT_ASSERT_LT(test, *seen, count);
		KUNIT_EXPECT_EQ(test, h->pic[slot].decode_idx, (u32)expected[(*seen)++]);
	}
}

static void replay_output_trace(struct kunit *test, const struct h265_output_trace *trace,
				unsigned int count, const u16 *expected, unsigned int outputs)
{
	struct h265_dpb *h = test->priv;
	bool no_rasl = false;
	unsigned int i, j, k, seen = 0;
	int slot;

	reset_params(h);
	h->nslots = H265_SLOTS;
	for (i = 0; i < count; i++) {
		const struct h265_output_trace *p = &trace[i];
		struct h265_picture_info info = {
			.irap = p->nal >= 16 && p->nal <= 23, .is_cra = p->nal == 21,
			.no_rasl_output = p->flags & 4, .no_output_of_prior_pics = p->flags & 2,
			.first_picture = !h->decode_idx,
		};
		struct h265_output_limits limits = {
			.max_dec_pic_buffering = p->buffering, .max_num_reorder_pics = p->reorder,
		};

		if (info.irap)
			no_rasl = info.no_rasl_output;
		if (no_rasl && (p->nal == 8 || p->nal == 9))
			continue;
		for (j = 0; j < H265_SLOTS; j++) {
			if (!h->pic[j].referenced)
				continue;
			for (k = 0; k < p->nrefs; k++)
				if (h->pic[j].poc == p->refs[k])
					break;
			h->pic[j].referenced = k < p->nrefs;
		}
		h265_dpb_begin_picture(h, &info, &limits);
		check_trace_outputs(test, h, expected, outputs, &seen);
		slot = h265_free_slot(h);
		KUNIT_ASSERT_GE(test, slot, 0);
		h->pic[slot] = (struct h265_pic) { .valid = true, .referenced = true,
			.poc = p->poc, .decode_idx = h->decode_idx++ };
		h265_dpb_end_picture(h, slot, p->flags & 1, &limits);
		check_trace_outputs(test, h, expected, outputs, &seen);
	}
	h265_dpb_flush(h);
	check_trace_outputs(test, h, expected, outputs, &seen);
	KUNIT_EXPECT_EQ(test, seen, outputs);
	KUNIT_EXPECT_EQ(test, h->pending, 0U);
}

static void conformance_output_prefix_traces(struct kunit *test)
{
	replay_output_trace(test, output_trace_0, ARRAY_SIZE(output_trace_0),
			    output_expected_0, ARRAY_SIZE(output_expected_0));
	replay_output_trace(test, output_trace_1, ARRAY_SIZE(output_trace_1),
			    output_expected_1, ARRAY_SIZE(output_expected_1));
	replay_output_trace(test, output_trace_2, ARRAY_SIZE(output_trace_2),
			    output_expected_2, ARRAY_SIZE(output_expected_2));
	replay_output_trace(test, output_trace_3, ARRAY_SIZE(output_trace_3),
			    output_expected_3, ARRAY_SIZE(output_expected_3));
	replay_output_trace(test, output_trace_4, ARRAY_SIZE(output_trace_4),
			    output_expected_4, ARRAY_SIZE(output_expected_4));
	replay_output_trace(test, output_trace_5, ARRAY_SIZE(output_trace_5),
			    output_expected_5, ARRAY_SIZE(output_expected_5));
}

static struct kunit_case hevc_test_cases[] = {
	KUNIT_CASE(poc_wrap_and_random_access),
	KUNIT_CASE(invalid_firmware_parameters),
	KUNIT_CASE(valid_crop_selects_visible_rectangle),
	KUNIT_CASE(invalid_crop_keeps_decoding_full_picture),
	KUNIT_CASE(first_slice_ignores_absent_segment_address),
	KUNIT_CASE(tile_boundaries),
	KUNIT_CASE(missing_and_reordered_references),
	KUNIT_CASE(long_term_references_survive_poc_lsb_wrap),
	KUNIT_CASE(previous_decoded_picture_can_be_long_term),
	KUNIT_CASE(rps_snapshot_resolves_st_before_lt),
	KUNIT_CASE(capture_sao_capacity),
	KUNIT_CASE(mv_storage_covers_edge_ctus),
	KUNIT_CASE(rps_and_output_own_picture_slots),
	KUNIT_CASE(irap_releases_previous_references),
	KUNIT_CASE(output_fifo_preserves_bumping_before_a_new_irap),
	KUNIT_CASE(output_respects_the_active_sps_reorder_limit),
	KUNIT_CASE(continuous_cra_keeps_leading_picture_order),
	KUNIT_CASE(restarted_cra_skips_unavailable_rasl),
	KUNIT_CASE(cvs_boundary_discards_waiting_pictures_without_matching_old_pocs),
	KUNIT_CASE(hidden_pictures_keep_references_and_fullness_bumps_before_decode),
	KUNIT_CASE(latency_counts_only_later_output_pictures),
	KUNIT_CASE(conformance_output_prefix_traces),
	{}
};

static struct kunit_suite hevc_test_suite = {
	.name = "amlogic-hevc-parameters",
	.init = hevc_test_init,
	.test_cases = hevc_test_cases,
};

kunit_test_suite(hevc_test_suite);
