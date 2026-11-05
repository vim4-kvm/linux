// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "wave5-vpu.h"
#include "wave5-params-kunit.h"

struct wave5_params_ctx {
	struct vpu_device dev;
	struct vpu_instance inst;
	struct enc_open_param open;
};

static int wave5_params_init(struct kunit *test)
{
	struct wave5_params_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->inst.dev = &ctx->dev;
	test->priv = ctx;
	return 0;
}

static void wave5_geometry(struct kunit *test)
{
	const unsigned int modes[][2] = {
		{ 640, 480 }, { 720, 400 }, { 800, 600 }, { 1024, 768 },
		{ 1280, 720 }, { 1280, 1024 }, { 1366, 768 }, { 1440, 900 },
		{ 1600, 900 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
		{ 2560, 1440 }, { 3840, 2160 },
	};
	struct wave5_params_ctx *ctx = test->priv;
	struct enc_wave_param *param = &ctx->open.wave_param;
	unsigned int i, rotation, width, height, coded_w, coded_h;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		width = modes[i][0];
		height = modes[i][1];
		memset(param, 0, sizeof(*param));
		wave5_set_enc_crop_info(W_AVC_ENC, param, 0, width, height);
		KUNIT_EXPECT_EQ(test, ALIGN(width, 16) - param->conf_win_right, width);
		KUNIT_EXPECT_EQ(test, ALIGN(height, 16) - param->conf_win_bot, height);
		KUNIT_EXPECT_EQ(test, param->conf_win_right % 2, 0);
		KUNIT_EXPECT_EQ(test, param->conf_win_bot % 2, 0);
		memset(param, 0, sizeof(*param));
		wave5_set_enc_crop_info(W_AVC_ENC, param, 3, width, height);
		coded_w = ALIGN(height, 16) - param->conf_win_left - param->conf_win_right;
		coded_h = ALIGN(width, 16) - param->conf_win_top - param->conf_win_bot;
		KUNIT_EXPECT_EQ(test, coded_w, height);
		KUNIT_EXPECT_EQ(test, coded_h, width);
		ctx->inst.std = W_HEVC_ENC;
		ctx->inst.conf_win = (struct v4l2_rect) {
			.left = 2, .top = 4, .width = width, .height = height,
		};
		for (rotation = 0; rotation < 4; rotation++) {
			wave5_set_enc_picture_size(&ctx->open, &ctx->inst);
			wave5_set_enc_crop_info(W_HEVC_ENC, param,
						rotation ? 1 | (rotation << 1) : 0,
						ctx->open.pic_width, ctx->open.pic_height);
			coded_w = rotation ? ALIGN(ctx->open.pic_width, 32) : ctx->open.pic_width;
			coded_h = rotation ? ALIGN(ctx->open.pic_height, 32) : ctx->open.pic_height;
			if (rotation & 1)
				swap(coded_w, coded_h);
			coded_w -= param->conf_win_left + param->conf_win_right;
			KUNIT_EXPECT_EQ(test, coded_w,
					rotation & 1 ? height : width);
			KUNIT_EXPECT_EQ(test, coded_h - param->conf_win_top - param->conf_win_bot,
					rotation & 1 ? width : height);
		}
	}
}

static void wave5_refresh_budget(struct kunit *test)
{
	struct wave5_params_ctx *ctx = test->priv;
	struct enc_wave_param *param = &ctx->open.wave_param;
	u32 height, period, rows, refresh;
	int hevc;

	for (hevc = 0; hevc <= 1; hevc++) {
		for (height = 128; height <= 2160; height += 2) {
			rows = DIV_ROUND_UP(height, hevc ? 64 : 16);
			for (period = 1; period <= 60; period++) {
				wave5_set_intra_refresh(param, height, period, hevc);
				refresh = hevc ? param->intra_refresh_arg :
						param->intra_mb_refresh_arg;
				KUNIT_ASSERT_GE(test, refresh, 1);
				KUNIT_ASSERT_LE(test, refresh, rows);
				KUNIT_ASSERT_GE(test, refresh * period, rows);
				KUNIT_ASSERT_LT(test, (refresh - 1) * period, rows);
			}
		}
		wave5_set_intra_refresh(param, 1080, 0, hevc);
		KUNIT_EXPECT_EQ(test, (u32)param->intra_mb_refresh_mode, 0);
		KUNIT_EXPECT_EQ(test, param->intra_mb_refresh_arg, 0);
		KUNIT_EXPECT_EQ(test, (u32)param->intra_refresh_mode, 0);
		KUNIT_EXPECT_EQ(test, param->intra_refresh_arg, 0);
	}
}

static void wave5_vbv_units(struct kunit *test)
{
	struct wave5_params_ctx *ctx = test->priv;
	struct vpu_instance *inst = &ctx->inst;

	inst->std = W_AVC_ENC;
	inst->h264_cpb_size = 1000;
	inst->bit_rate = 8192000;
	KUNIT_ASSERT_EQ(test, wave5_set_vbv_size(&ctx->open, inst), 0);
	KUNIT_EXPECT_EQ(test, ctx->open.vbv_buffer_size, 1000);
	inst->h264_cpb_size = 1;
	KUNIT_EXPECT_EQ(test, wave5_set_vbv_size(&ctx->open, inst), -ERANGE);
	KUNIT_EXPECT_EQ(test, ctx->open.vbv_buffer_size, 1000);
	inst->h264_cpb_size = U32_MAX;
	KUNIT_EXPECT_EQ(test, wave5_set_vbv_size(&ctx->open, inst), -ERANGE);
	inst->bit_rate = 0;
	KUNIT_EXPECT_EQ(test, wave5_set_vbv_size(&ctx->open, inst), -EINVAL);
	inst->std = W_HEVC_ENC;
	inst->rc_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
	KUNIT_ASSERT_EQ(test, wave5_set_vbv_size(&ctx->open, inst), 0);
	KUNIT_EXPECT_EQ(test, ctx->open.vbv_buffer_size, 3000);
}

static void wave5_gop_references(struct kunit *test)
{
	struct wave5_params_ctx *ctx = test->priv;
	struct enc_wave_param *input = &ctx->inst.hevc_param;
	struct enc_wave_param *output = &ctx->open.wave_param;
	unsigned int layers, i, size, seen;
	u32 temporal[MAX_GOP_NUM + 1];

	ctx->inst.std = W_HEVC_ENC;
	input->hier_enabled = true;
	input->hier_qp_enable = true;
	output->max_qp_p = 51;
	output->max_qp_b = 51;
	for (i = 0; i < 4; i++)
		input->hier_qp[i] = 24 + i;
	for (layers = 1; layers <= 4; layers++) {
		size = BIT(layers - 1);
		input->hier_layers = layers;
		ctx->inst.b_frames = size - 1;
		KUNIT_ASSERT_EQ(test, wave5_set_enc_gop(&ctx->open, &ctx->inst), 0);
		seen = BIT(0);
		memset(temporal, 0, sizeof(temporal));
		for (i = 0; i < size; i++) {
			const struct custom_gop_pic_param *pic = &ctx->open.custom_gop[i];

			KUNIT_ASSERT_GE(test, pic->poc_offset, 1);
			KUNIT_ASSERT_LE(test, pic->poc_offset, size);
			KUNIT_ASSERT_EQ(test, seen & BIT(pic->poc_offset), 0);
			KUNIT_ASSERT_NE(test, seen & BIT(pic->ref_poc_l0), 0);
			KUNIT_EXPECT_LE(test, temporal[pic->ref_poc_l0], pic->temporal_id);
			if (pic->pic_type == PIC_TYPE_B) {
				KUNIT_ASSERT_NE(test, seen & BIT(pic->ref_poc_l1), 0);
				KUNIT_EXPECT_LT(test, pic->ref_poc_l0, pic->poc_offset);
				KUNIT_EXPECT_GT(test, pic->ref_poc_l1, pic->poc_offset);
			}
			KUNIT_EXPECT_EQ(test, pic->pic_qp, 24 + pic->temporal_id);
			seen |= BIT(pic->poc_offset);
			temporal[pic->poc_offset] = pic->temporal_id;
		}
		KUNIT_EXPECT_EQ(test, seen, BIT(size + 1) - 1);
	}
	ctx->inst.ltr_count = 1;
	KUNIT_EXPECT_EQ(test, wave5_set_enc_gop(&ctx->open, &ctx->inst), -EINVAL);
	ctx->inst.ltr_count = 0;
	input->hier_layers = 0;
	KUNIT_EXPECT_EQ(test, wave5_set_enc_gop(&ctx->open, &ctx->inst), -EINVAL);
	input->hier_layers = 5;
	KUNIT_EXPECT_EQ(test, wave5_set_enc_gop(&ctx->open, &ctx->inst), -EINVAL);
}

static void wave5_gop_other_topologies(struct kunit *test)
{
	struct wave5_params_ctx *ctx = test->priv;
	struct enc_wave_param *input;
	unsigned int codec, size, i, layers;
	u32 seen;

	ctx->open.wave_param.max_qp_p = 51;
	ctx->open.wave_param.max_qp_b = 51;
	for (codec = 0; codec < 2; codec++) {
		ctx->inst.std = codec ? W_HEVC_ENC : W_AVC_ENC;
		input = codec ? &ctx->inst.hevc_param : &ctx->inst.enc_param;
		memset(input, 0, sizeof(*input));
		for (size = 3; size <= MAX_GOP_NUM; size++) {
			ctx->inst.b_frames = size - 1;
			KUNIT_ASSERT_EQ(test, wave5_set_enc_gop(&ctx->open, &ctx->inst), 0);
			KUNIT_ASSERT_EQ(test, ctx->open.custom_gop_size, size);
			seen = BIT(0);
			for (i = 0; i < size; i++) {
				const struct custom_gop_pic_param *pic = &ctx->open.custom_gop[i];

				KUNIT_ASSERT_GE(test, pic->poc_offset, 1);
				KUNIT_ASSERT_LE(test, pic->poc_offset, size);
				KUNIT_ASSERT_EQ(test, seen & BIT(pic->poc_offset), 0);
				KUNIT_ASSERT_NE(test, seen & BIT(pic->ref_poc_l0), 0);
				if (pic->pic_type == PIC_TYPE_B)
					KUNIT_ASSERT_NE(test, seen & BIT(pic->ref_poc_l1), 0);
				seen |= BIT(pic->poc_offset);
			}
			KUNIT_EXPECT_EQ(test, seen, BIT(size + 1) - 1);
		}
		input->hier_enabled = true;
		input->hier_p = true;
		ctx->inst.b_frames = 0;
		for (layers = 1; layers <= 4; layers++) {
			input->hier_layers = layers;
			size = BIT(layers - 1);
			KUNIT_ASSERT_EQ(test, wave5_set_enc_gop(&ctx->open, &ctx->inst), 0);
			KUNIT_ASSERT_EQ(test, ctx->open.custom_gop_size, size);
			for (i = 0; i < size; i++) {
				const struct custom_gop_pic_param *pic = &ctx->open.custom_gop[i];

				KUNIT_EXPECT_EQ(test, pic->pic_type, PIC_TYPE_P);
				KUNIT_EXPECT_EQ(test, pic->poc_offset, i + 1);
				KUNIT_EXPECT_LT(test, pic->ref_poc_l0, pic->poc_offset);
				if (pic->ref_poc_l0) {
					const struct custom_gop_pic_param *ref;

					ref = &ctx->open.custom_gop[pic->ref_poc_l0 - 1];
					KUNIT_EXPECT_LE(test, ref->temporal_id, pic->temporal_id);
				}
			}
		}
	}
}

static void wave5_legacy_format(struct kunit *test)
{
	struct wave5_params_ctx *ctx = test->priv;

	ctx->dev.legacy_firmware = true;
	ctx->inst.std = W_AVC_ENC;
	KUNIT_EXPECT_EQ(test, wave5_enc_sps_profile(&ctx->inst, H264_PROFILE_HP), 3);
	KUNIT_EXPECT_EQ(test, wave5_enc_sps_profile(&ctx->inst, H264_PROFILE_HIGH10), 4);
	ctx->inst.std = W_HEVC_ENC;
	KUNIT_EXPECT_EQ(test, wave5_enc_sps_profile(&ctx->inst, HEVC_PROFILE_MAIN),
			HEVC_PROFILE_MAIN);
	ctx->inst.std = W_AVC_ENC;
	ctx->dev.legacy_firmware = false;
	KUNIT_EXPECT_EQ(test, wave5_enc_sps_profile(&ctx->inst, H264_PROFILE_HP),
			H264_PROFILE_HP);
	KUNIT_EXPECT_EQ(test, wave5_enc_source_endian(FORMAT_420_P10_16BIT_LSB, true), 14);
	KUNIT_EXPECT_EQ(test, wave5_enc_source_endian(FORMAT_420_P10_16BIT_LSB, false), 15);
	KUNIT_EXPECT_EQ(test, wave5_enc_source_endian(FORMAT_420, true), 15);
}

static struct kunit_case wave5_params_cases[] = {
	KUNIT_CASE(wave5_geometry),
	KUNIT_CASE(wave5_refresh_budget),
	KUNIT_CASE(wave5_vbv_units),
	KUNIT_CASE(wave5_gop_references),
	KUNIT_CASE(wave5_gop_other_topologies),
	KUNIT_CASE(wave5_legacy_format),
	{}
};

static struct kunit_suite wave5_params_suite = {
	.name = "wave5-params",
	.init = wave5_params_init,
	.test_cases = wave5_params_cases,
};

kunit_test_suite(wave5_params_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Wave5 encoder parameter tests");
