// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>
#include <drm/drm_blend.h>

#include "meson_t7_fb.h"
#include "meson_t7_handoff.h"
#include "meson_t7_kunit.h"
#include "meson_t7_layers.h"
#include "meson_t7_modeset.h"
#include "meson_t7_video.h"

static void t7_fb_storage(struct kunit *test)
{
	struct drm_framebuffer *fb;
	struct drm_gem_dma_object *y, *uv;

	fb = kunit_kzalloc(test, sizeof(*fb), GFP_KERNEL);
	y = kunit_kzalloc(test, sizeof(*y), GFP_KERNEL);
	uv = kunit_kzalloc(test, sizeof(*uv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fb);
	KUNIT_ASSERT_NOT_NULL(test, y);
	KUNIT_ASSERT_NOT_NULL(test, uv);
	fb->width = 17;
	fb->height = 32;
	fb->format = drm_format_info(DRM_FORMAT_RGB888);
	fb->pitches[0] = 64;
	fb->obj[0] = &y->base;
	y->dma_addr = 0x20000000;
	y->base.size = 31 * 64 + 17 * 3;
	KUNIT_EXPECT_EQ(test, meson_t7_fb_check(fb), -ERANGE);
	y->base.size = 32 * 64;
	KUNIT_EXPECT_EQ(test, meson_t7_fb_check(fb), 0);
	y->dma_addr = BIT_ULL(32) - y->base.size;
	KUNIT_EXPECT_EQ(test, meson_t7_fb_check(fb), 0);
	y->dma_addr += 16;
	KUNIT_EXPECT_EQ(test, meson_t7_fb_check(fb), -ERANGE);
	y->dma_addr = 0x20000000;
	fb->format = drm_format_info(DRM_FORMAT_NV12);
	fb->width = 1920;
	fb->height = 1080;
	fb->pitches[0] = 1920;
	fb->pitches[1] = 1920;
	fb->obj[1] = &y->base;
	fb->offsets[1] = 1920 * 1080;
	y->base.size = 1920 * 1088 * 3 / 2;
	KUNIT_EXPECT_EQ(test, meson_t7_fb_check(fb), 0);
	/* Separate imports retain their own offsets and complete chroma rows. */
	fb->obj[1] = &uv->base;
	fb->offsets[1] = 32;
	uv->dma_addr = 0x30000000;
	uv->base.size = 32 + 1920 * 540;
	KUNIT_EXPECT_EQ(test, meson_t7_fb_check(fb), 0);
	uv->base.size--;
	KUNIT_EXPECT_EQ(test, meson_t7_fb_check(fb), -ERANGE);
	uv->base.size++;
	fb->pitches[1] = 1888;
	KUNIT_EXPECT_EQ(test, meson_t7_fb_check(fb), -EINVAL);
	fb->pitches[1] = 1920;
	fb->modifier = DRM_FORMAT_MOD_INVALID;
	KUNIT_EXPECT_EQ(test, meson_t7_fb_check(fb), -EINVAL);
}

static void t7_link_clock(struct kunit *test)
{
	struct meson_t7_link_clock link;
	unsigned int pixel, bpc, y420;
	u64 serial;
	s64 error;

	KUNIT_ASSERT_EQ(test, meson_t7_link_clock_calculate(594000, 10, true, &link), 0);
	KUNIT_EXPECT_EQ(test, link.tmds_khz, 371250);
	KUNIT_EXPECT_EQ(test, meson_t7_link_clock_calculate(25174, 8, false, &link), -ERANGE);
	KUNIT_EXPECT_EQ(test, meson_t7_link_clock_calculate(148500, 12, false, &link), -ERANGE);
	for (pixel = 25175; pixel <= 594000; pixel += 997) {
		for (bpc = 8; bpc <= 10; bpc += 2) {
			for (y420 = 0; y420 < 2; y420++) {
				if (meson_t7_link_clock_calculate(pixel, bpc, y420, &link))
					continue;
				serial = (u64)link.pll.vco * 1000;
				serial /= link.pll.od1 * link.pll.od2 * 10;
				error = serial - (u64)link.tmds_khz * 1000;
				KUNIT_ASSERT_LT(test, abs(error), 5000);
				KUNIT_ASSERT_EQ(test, link.pixel_div, y420 ? 2 : 1);
			}
		}
	}
}

static void t7_mode_admission(struct kunit *test)
{
	struct drm_display_mode mode = meson_t7_handoff_mode;
	struct meson_t7_clock clock;

	KUNIT_EXPECT_EQ(test, meson_t7_mode_valid(&mode), MODE_OK);
	KUNIT_EXPECT_EQ(test, meson_t7_handoff_mode_valid(&mode), MODE_OK);
	mode.hsync_start++;
	KUNIT_EXPECT_EQ(test, meson_t7_handoff_mode_valid(&mode), MODE_BAD);
	mode = meson_t7_handoff_mode;
	mode.flags |= DRM_MODE_FLAG_INTERLACE;
	KUNIT_EXPECT_EQ(test, meson_t7_mode_valid(&mode), MODE_NO_INTERLACE);
	mode = meson_t7_handoff_mode;
	mode.vsync_start = mode.vdisplay + 1;
	KUNIT_EXPECT_EQ(test, meson_t7_mode_valid(&mode), MODE_V_ILLEGAL);
	KUNIT_EXPECT_EQ(test, meson_t7_clock_calculate(25174, &clock), -ERANGE);
	KUNIT_EXPECT_EQ(test, meson_t7_clock_calculate(594001, &clock), -ERANGE);
	KUNIT_ASSERT_EQ(test, meson_t7_clock_calculate(148500, &clock), 0);
	KUNIT_EXPECT_EQ(test, clock.vco / clock.od1 / clock.od2 / 10, 148500);
}

static void t7_video_mif(struct kunit *test)
{
	struct meson_t7_video_frame frame = {
		.format = DRM_FORMAT_NV12, .width = 1280, .height = 720,
		.crop_width = 1280, .crop_height = 720,
		.pitch = { 1280, 1280 }, .address = { 0x10000000, 0x20000000 },
		.size = { 1280 * 720, 1280 * 360 },
	};
	struct meson_t7_video_mif mif, previous;

	KUNIT_ASSERT_EQ(test, meson_t7_video_mif_prepare(&frame, 0, &mif), 0);
	KUNIT_EXPECT_EQ(test, mif.gen & 1, 0);
	KUNIT_EXPECT_EQ(test, mif.address[0], frame.address[0] >> 4);
	KUNIT_EXPECT_EQ(test, mif.address[1], frame.address[1] >> 4);
	KUNIT_EXPECT_EQ(test, mif.gen2, 2);
	frame.format = DRM_FORMAT_NV21;
	frame.vskip = 1;
	KUNIT_ASSERT_EQ(test, meson_t7_video_mif_prepare(&frame, 1, &mif), 0);
	KUNIT_EXPECT_EQ(test, mif.gen2, 1);
	KUNIT_EXPECT_EQ(test, mif.repeat_pattern, 8);
	previous = mif;
	frame.size[1]--;
	KUNIT_EXPECT_EQ(test, meson_t7_video_mif_prepare(&frame, 1, &mif), -ERANGE);
	KUNIT_EXPECT_MEMEQ(test, &mif, &previous, sizeof(mif));
	frame.size[1]++;
	frame.x = 1;
	KUNIT_EXPECT_EQ(test, meson_t7_video_mif_prepare(&frame, 1, &mif), -EINVAL);
	frame.x = 0;
	frame.address[1] = BIT_ULL(32) - 16;
	KUNIT_EXPECT_EQ(test, meson_t7_video_mif_prepare(&frame, 1, &mif), -ERANGE);
}

static void t7_video_phase(struct kunit *test)
{
	struct meson_t7_video_phase phase, previous;

	KUNIT_ASSERT_EQ(test, meson_t7_video_phase_prepare(1920, 1080, 1920, 1080, &phase), 0);
	KUNIT_EXPECT_EQ(test, phase.hstep, BIT(24));
	KUNIT_EXPECT_EQ(test, phase.vphase, 0);
	KUNIT_ASSERT_EQ(test, meson_t7_video_phase_prepare(3840, 2160, 960, 540, &phase), 0);
	KUNIT_EXPECT_EQ(test, phase.vstep, BIT(26));
	KUNIT_EXPECT_EQ(test, phase.vreceive_repeat, 4);
	previous = phase;
	KUNIT_EXPECT_EQ(test, meson_t7_video_phase_prepare(3840, 2160, 959, 540, &phase), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &phase, &previous, sizeof(phase));
	KUNIT_EXPECT_EQ(test, meson_t7_video_phase_prepare(1920, 1080, 1920, 0, &phase), -EINVAL);
}

static void t7_video_pps(struct kunit *test)
{
	const unsigned int words = MESON_T7_VIDEO_PPS_PAIRS * 2 + 1;
	u32 *table = kunit_kmalloc_array(test, words, sizeof(*table), GFP_KERNEL);
	unsigned int index;
	int bytes;

	KUNIT_ASSERT_NOT_NULL(test, table);
	for (index = 0; index < 2; index++) {
		memset(table, 0xa5, words * sizeof(*table));
		bytes = meson_t7_video_pps_build(table, MESON_T7_VIDEO_PPS_PAIRS,
						 index, 3840, 2160, 1920, 1080, 0);
		KUNIT_ASSERT_EQ(test, bytes, MESON_T7_VIDEO_PPS_PAIRS * 8);
		KUNIT_EXPECT_EQ(test, table[words - 1], 0xa5a5a5a5);
		memset(table, 0xa5, words * sizeof(*table));
		bytes = meson_t7_video_pps_build(table, MESON_T7_VIDEO_PPS_PAIRS - 1,
						 index, 3840, 2160, 1920, 1080, 0);
		KUNIT_EXPECT_EQ(test, bytes, -ENOSPC);
		KUNIT_EXPECT_EQ(test, table[0], 0xa5a5a5a5);
	}
}

static void t7_osd_layers(struct kunit *test)
{
	struct meson_t7_osd_layer layers[3] = {}, old[3];
	u32 *table = kunit_kcalloc(test, MESON_T7_RDMA_PAIRS * 2 + 1, sizeof(u32), GFP_KERNEL);
	unsigned int i;
	int bytes;

	KUNIT_ASSERT_NOT_NULL(test, table);
	for (i = 0; i < 3; i++) {
		layers[i] = (struct meson_t7_osd_layer) {
			.enabled = true, .format = DRM_FORMAT_XRGB8888, .pitch = 2560,
			.address = 0x10000000 + i * 0x100000,
			.src_w = 640, .src_h = 480, .width = 640, .height = 480,
			.zpos = i, .alpha = 0xffff, .blend = DRM_MODE_BLEND_PIXEL_NONE,
		};
	}
	bytes = meson_t7_osd_layers_build(table, MESON_T7_RDMA_PAIRS, layers, 1920, 1080, 0, 0);
	KUNIT_ASSERT_GT(test, bytes, 0);
	KUNIT_EXPECT_LE(test, bytes, MESON_T7_RDMA_PAIRS * 8);
	memcpy(old, layers, sizeof(old));
	layers[0].address += 16;
	KUNIT_EXPECT_EQ(test, meson_t7_osd_layers_flip(table, 3, layers, old), 24);
	layers[0].pitch += 32;
	KUNIT_EXPECT_EQ(test, meson_t7_osd_layers_flip(table, 3, layers, old), 0);
	layers[0].pitch -= 32;
	table[4] = 0xdeadbeef;
	KUNIT_EXPECT_EQ(test, meson_t7_osd_layers_flip(table, 2, layers, old), -ENOSPC);
	KUNIT_EXPECT_EQ(test, table[4], 0xdeadbeef);
}

static void t7_osd_encoding(struct kunit *test)
{
	u32 ctrl = 0xa5a5a5a5;
	u32 mask = GENMASK(20, 12);

	KUNIT_EXPECT_EQ(test, meson_t7_osd_alpha(ctrl, 0xffff) & mask, BIT(20));
	KUNIT_EXPECT_EQ(test, meson_t7_osd_alpha(ctrl, 0) & mask, 0);
	KUNIT_EXPECT_EQ(test, meson_t7_osd_alpha(ctrl, 0x8000) & ~mask, ctrl & ~mask);
	KUNIT_EXPECT_EQ(test, meson_t7_osd_reflect(0, DRM_MODE_REFLECT_X), BIT(28));
	KUNIT_EXPECT_EQ(test, meson_t7_osd_reflect(BIT(28), DRM_MODE_REFLECT_Y), BIT(29));
}

static struct kunit_case meson_t7_cases[] = {
	KUNIT_CASE(t7_fb_storage),
	KUNIT_CASE(t7_link_clock),
	KUNIT_CASE(t7_mode_admission),
	KUNIT_CASE(t7_video_mif),
	KUNIT_CASE(t7_video_phase),
	KUNIT_CASE(t7_video_pps),
	KUNIT_CASE(t7_osd_layers),
	KUNIT_CASE(t7_osd_encoding),
	{}
};

static struct kunit_suite meson_t7_suite = {
	.name = "meson-t7",
	.test_cases = meson_t7_cases,
};

kunit_test_suite(meson_t7_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Meson T7 display pure logic tests");
