// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "t7-hdmirx-kunit.h"
#include "t7-hlg-state.h"
#include "t7-vdin-ring.h"

static void t7_edid_blocks(struct kunit *test)
{
	const u8 header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };
	u8 *edid, sum;
	unsigned int blocks, block, i;

	edid = kunit_kzalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, edid);
	for (blocks = 1; blocks <= 4; blocks++) {
		memset(edid, 0, 512);
		memcpy(edid, header, sizeof(header));
		edid[126] = blocks - 1;
		for (block = 0; block < blocks; block++) {
			sum = 0;
			for (i = 0; i < 127; i++)
				sum += edid[block * 128 + i];
			edid[block * 128 + 127] = -sum;
		}
		KUNIT_ASSERT_EQ(test, t7_hdmirx_validate_edid(edid, blocks * 128), 0);
		for (block = 0; block < blocks; block++) {
			edid[block * 128 + 30] ^= 1;
			KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_edid(edid, blocks * 128), -EINVAL);
			edid[block * 128 + 30] ^= 1;
		}
	}
	/* A self-consistent checksum must not hide a wrong extension count. */
	edid[126]--;
	edid[127]++;
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_edid(edid, 512), -EINVAL);
	edid[126]++;
	edid[127]--;
	edid[0]++;
	edid[127]--;
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_edid(edid, 512), -EINVAL);
}

static void t7_edid_lengths(struct kunit *test)
{
	u8 edid[128] = {};

	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_edid(edid, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_edid(edid, 127), -EINVAL);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_edid(edid, 129), -EINVAL);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_edid(edid, 513), -E2BIG);
}

static void t7_audio_acr(struct kunit *test)
{
	const unsigned int rates[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
	const u32 n[] = { 4096, 6272, 6144, 12544, 12288, 25088, 24576 };
	unsigned int i;
	u32 cts;

	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		cts = rates[i] % 44100 ? 594000 : 660000;
		KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, n[i], cts,
							       1, 0, 0, rates[i]), 0);
	}
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, 6144, 594000,
						       1, 0, 0, 44100), -ERANGE);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, 0, 594000,
						       1, 0, 0, 48000), -ENOLINK);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, 6144, 0,
						       1, 0, 0, 48000), -ENOLINK);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(0, 6144, 594000,
						       1, 0, 0, 48000), -ENOLINK);
}

static void t7_audio_admission(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, 6144, 594000,
						       0x21, 0, 0, 48000), -EINVAL);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, 6144, 594000,
						       7, 0, 0, 48000), -EINVAL);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, 6144, 594000,
						       1, BIT(6), 0, 48000), -EINVAL);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, 6144, 594000,
						       1, BIT(3), 0, 48000), -EINVAL);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, 6144, 594000,
						       1, 0, BIT(1), 48000), -EINVAL);
	KUNIT_EXPECT_EQ(test, t7_hdmirx_validate_audio(594000000, 6144, 594000,
						       1, 0, 0, 64000), -EINVAL);
}

static void t7_lut_crc(struct kunit *test)
{
	const u32 sequential[] = { 0x04030201, 0x08070605 };
	const u32 shifted[] = { 4, 8, 1020, 16380 };

	/* Independent zlib CRC32 of LE words, after the port's sample shift. */
	KUNIT_EXPECT_EQ(test, t7_vdin_lut_crc(sequential, 2, 0), 0x3fca88c5U);
	KUNIT_EXPECT_EQ(test, t7_vdin_lut_crc(shifted, 4, 2), 0x5dfa0371U);
	KUNIT_EXPECT_EQ(test, t7_vdin_lut_crc(NULL, 0, 0), 0U);
}

struct curve_ctx {
	char valid[4096];
	char text[4096];
	u32 values[298];
};

static struct curve_ctx *t7_curve_alloc(struct kunit *test)
{
	struct curve_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	unsigned int i, len = 0;

	if (!ctx)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(ctx->values); i++)
		len += scnprintf(ctx->valid + len, sizeof(ctx->valid) - len, "%u%c",
				 i < 149 ? 1024 : i == 297 ? 4095 : 0,
				 i == 148 || i == 297 ? '\n' : ',');
	return ctx;
}

static void t7_curve_transaction(struct kunit *test)
{
	struct curve_ctx *ctx = t7_curve_alloc(test);
	unsigned int len, n;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	strscpy(ctx->text, ctx->valid, sizeof(ctx->text));
	KUNIT_ASSERT_EQ(test, t7_vdin_parse_hdr_curve(ctx->text, ctx->values), 0);
	KUNIT_EXPECT_EQ(test, ctx->values[0], 1024U);
	KUNIT_EXPECT_EQ(test, ctx->values[149], 0U);
	KUNIT_EXPECT_EQ(test, ctx->values[297], 4095U);
	len = strlen(ctx->valid);
	for (n = 0; n < len - 1; n++) {
		memcpy(ctx->text, ctx->valid, n);
		ctx->text[n] = 0;
		KUNIT_ASSERT_EQ(test, t7_vdin_parse_hdr_curve(ctx->text, ctx->values), -EINVAL);
	}
	strscpy(ctx->text, ctx->valid, sizeof(ctx->text));
	strlcat(ctx->text, "1", sizeof(ctx->text));
	KUNIT_EXPECT_EQ(test, t7_vdin_parse_hdr_curve(ctx->text, ctx->values), -EINVAL);
}

static void t7_curve_constraints(struct kunit *test)
{
	struct curve_ctx *ctx = t7_curve_alloc(test);
	const char invalid[] = { 'x', '-', '9', '0' };
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	for (i = 0; i < ARRAY_SIZE(invalid); i++) {
		strscpy(ctx->text, ctx->valid, sizeof(ctx->text));
		ctx->text[0] = invalid[i];
		KUNIT_EXPECT_EQ(test, t7_vdin_parse_hdr_curve(ctx->text, ctx->values), -EINVAL);
	}
	strscpy(ctx->text, ctx->valid, sizeof(ctx->text));
	ctx->text[strlen(ctx->text) - 2] = '4';
	KUNIT_EXPECT_EQ(test, t7_vdin_parse_hdr_curve(ctx->text, ctx->values), -EINVAL);
	strscpy(ctx->text, ctx->valid, sizeof(ctx->text));
	/* The second gain must not exceed the first, even within 12 bits. */
	ctx->text[5] = '2';
	KUNIT_EXPECT_EQ(test, t7_vdin_parse_hdr_curve(ctx->text, ctx->values), -EINVAL);
	strscpy(ctx->text, ctx->valid, sizeof(ctx->text));
	memcpy(ctx->text, "0000", 4);
	KUNIT_EXPECT_EQ(test, t7_vdin_parse_hdr_curve(ctx->text, ctx->values), -EINVAL);
	strscpy(ctx->text, ctx->valid, sizeof(ctx->text));
	ctx->text[149 * 5] = '1';
	KUNIT_EXPECT_EQ(test, t7_vdin_parse_hdr_curve(ctx->text, ctx->values), -EINVAL);
	strscpy(ctx->text, ctx->valid, sizeof(ctx->text));
	ctx->text[149 * 5 + 2] = '1';
	KUNIT_EXPECT_EQ(test, t7_vdin_parse_hdr_curve(ctx->text, ctx->values), -EINVAL);
}

static void t7_ring_oldest_ready(struct kunit *test)
{
	struct t7_vdin_slot slots[VDIN_DMA_SLOTS] = {
		{ .state = VDIN_SLOT_READY, .sequence = 0 },
		{ .state = VDIN_SLOT_READY, .sequence = U32_MAX },
		{ .state = VDIN_SLOT_READY, .sequence = U32_MAX - 1 },
	};

	KUNIT_EXPECT_PTR_EQ(test, t7_vdin_take_ready(slots), &slots[2]);
	KUNIT_EXPECT_EQ(test, slots[2].state, VDIN_SLOT_COPYING);
	KUNIT_EXPECT_PTR_EQ(test, t7_vdin_take_ready(slots), &slots[1]);
	KUNIT_EXPECT_PTR_EQ(test, t7_vdin_take_ready(slots), &slots[0]);
	KUNIT_EXPECT_PTR_EQ(test, t7_vdin_take_ready(slots), NULL);
	slots[0].state = VDIN_SLOT_FREE;
	slots[1].state = VDIN_SLOT_WRITING;
	KUNIT_EXPECT_PTR_EQ(test, t7_vdin_take_ready(slots), NULL);
}

static void t7_hdr_encoding_lease(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, t7_hdr_eotf_supported(2));
	KUNIT_EXPECT_TRUE(test, t7_hdr_eotf_supported(3));
	KUNIT_EXPECT_FALSE(test, t7_hdr_eotf_supported(0));
	KUNIT_EXPECT_FALSE(test, t7_hdr_eotf_supported(1));
	KUNIT_EXPECT_FALSE(test, t7_hdr_eotf_supported(255));
	KUNIT_EXPECT_FALSE(test, t7_hdr_encoding_changed(false, 0, false, 3));
	KUNIT_EXPECT_FALSE(test, t7_hdr_encoding_changed(true, 2, true, 2));
	KUNIT_EXPECT_TRUE(test, t7_hdr_encoding_changed(true, 3, true, 2));
	KUNIT_EXPECT_TRUE(test, t7_hdr_encoding_changed(true, 2, false, 2));
	KUNIT_EXPECT_TRUE(test, t7_hdr_encoding_changed(false, 2, true, 2));
}

static struct kunit_case t7_hdmirx_cases[] = {
	KUNIT_CASE(t7_edid_blocks),
	KUNIT_CASE(t7_edid_lengths),
	KUNIT_CASE(t7_audio_acr),
	KUNIT_CASE(t7_audio_admission),
	KUNIT_CASE(t7_lut_crc),
	KUNIT_CASE(t7_curve_transaction),
	KUNIT_CASE(t7_curve_constraints),
	KUNIT_CASE(t7_ring_oldest_ready),
	KUNIT_CASE(t7_hdr_encoding_lease),
	{}
};

static struct kunit_suite t7_hdmirx_suite = {
	.name = "t7-hdmirx",
	.test_cases = t7_hdmirx_cases,
};

kunit_test_suite(t7_hdmirx_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T7 HDMI receiver and VDIN pure logic tests");
