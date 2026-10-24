// SPDX-License-Identifier: GPL-2.0-only
/* Experimental T7 progressive linear video planes. No canvas or AFBC DMA. */
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_modeset_helper_vtables.h>

#include "meson_t7_fb.h"
#include "meson_t7_modeset.h"
#include "meson_t7_layers.h"
#include "meson_t7_video.h"
#include "meson_t7_video_kms.h"
#include "meson_t7_video_sync.h"

static const struct {
	const char *name;
	u32 start, words;
} video_banks[] = {
	{ "vd1-mif", 0x4800, 0x41 }, { "vd2-mif", 0x4880, 0x41 },
	{ "vd1-pps", 0x1d01, 0x20 }, { "vd1-pps-control", 0x1d34, 7 },
	{ "vd2-pps", 0x3933, 0x1e }, { "video-misc", 0x1d26, 1 },
	{ "video-alpha", 0x1d76, 1 }, { "video-sources", 0x1dfb, 4 },
	{ "vd3-source", 0x1def, 1 }, { "video-hdr-size", 0x1a57, 2 },
	{ "vd1-hdr", 0x3800, 1 }, { "vd2-hdr", 0x3850, 1 },
	{ "video-path", 0x1a7a, 1 }, { "video-start", 0x1a8a, 1 },
	{ "video-size", 0x1da6, 1 }, { "video-matrix", 0x1d5f, 1 },
	{ "video-dsc", 0x1a83, 2 },
	{ "video-sr", 0x1d91, 2 }, { "vd3-fetch", 0x4900, 1 },
	{ "vd3-afbc", 0x4940, 1 },
};

struct meson_t7_video_kms {
	struct drm_plane planes[2];
	void __iomem *banks[ARRAY_SIZE(video_banks)];
};

static u32 video_read(struct meson_t7_video_kms *video, u32 reg)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(video_banks); i++)
		if (reg >= video_banks[i].start &&
		    reg - video_banks[i].start < video_banks[i].words)
			return readl(video->banks[i] + (reg - video_banks[i].start) * 4);
	/* Every register used below must be covered by a narrow resource. */
	WARN_ON_ONCE(1);
	return 0;
}

static int video_boot_check(struct meson_t7_video_kms *video)
{
	/* Foreign video/decoder DMA must never be reset to obtain ownership. */
	if ((video_read(video, 0x4800) | video_read(video, 0x4880) |
	     video_read(video, 0x4900)) & BIT(0))
		return -EBUSY;
	if ((video_read(video, 0x4840) | video_read(video, 0x48c0) |
	     video_read(video, 0x4940)) & BIT(8))
		return -EBUSY;
	/* SR is not owned. Refuse firmware routing through an active SR core. */
	if (video_read(video, 0x1d91) || video_read(video, 0x1d92) ||
	    (video_read(video, 0x1a7a) & 0xff) != 0x21 ||
	    (video_read(video, 0x1a8a) & 0xff))
		return -EBUSY;
	return 0;
}

struct meson_t7_video_kms *meson_t7_video_map(struct platform_device *pdev)
{
	struct meson_t7_video_kms *video;
	unsigned int i;
	int ret;

	if (!device_property_read_bool(&pdev->dev, "amlogic,video-planes"))
		return NULL;
	video = devm_kzalloc(&pdev->dev, sizeof(*video), GFP_KERNEL);
	if (!video)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < ARRAY_SIZE(video_banks); i++) {
		struct resource *r = platform_get_resource_byname(pdev, IORESOURCE_MEM,
								 video_banks[i].name);

		if (!r || r->start != 0xff000000 + video_banks[i].start * 4 ||
		    resource_size(r) != video_banks[i].words * 4)
			return ERR_PTR(-EINVAL);
		video->banks[i] = devm_ioremap_resource(&pdev->dev, r);
		if (IS_ERR(video->banks[i]))
			return ERR_CAST(video->banks[i]);
	}
	ret = video_boot_check(video);
	if (ret)
		return ERR_PTR(ret);
	return video;
}

static int video_frame(struct drm_plane_state *state, struct meson_t7_video_frame *frame)
{
	struct drm_framebuffer *fb = state->fb;
	unsigned int i;
	int ret;

	ret = meson_t7_fb_check(fb);
	if (ret)
		return ret;
	if (fb->format->num_planes != 2 ||
	    (fb->format->format != DRM_FORMAT_NV12 && fb->format->format != DRM_FORMAT_NV21))
		return -EINVAL;
	*frame = (struct meson_t7_video_frame) {
		.format = fb->format->format, .width = fb->width, .height = fb->height,
		.x = state->src.x1 >> 16, .y = state->src.y1 >> 16,
		.crop_width = drm_rect_width(&state->src) >> 16,
		.crop_height = drm_rect_height(&state->src) >> 16,
	};
	for (i = 0; i < 2; i++) {
		struct drm_gem_dma_object *object = drm_fb_dma_get_gem_obj(fb, i);

		if (!object || fb->offsets[i] > object->base.size)
			return -EINVAL;
		frame->address[i] = object->dma_addr + fb->offsets[i];
		frame->size[i] = object->base.size - fb->offsets[i];
		frame->pitch[i] = fb->pitches[i];
	}
	return 0;
}

static u32 video_vskip(const struct meson_t7_video_frame *frame,
		       const struct drm_plane_state *state,
		       const struct drm_crtc_state *crtc)
{
	const struct drm_display_mode *mode = &crtc->adjusted_mode;

	/* T7 vpp_process_speed_check(): uncompressed 4K downscale above 30Hz. */
	return frame->crop_height > 2048 && drm_rect_height(&state->dst) < 2048 &&
		(u64)mode->clock * 1000 > 30ULL * mode->htotal * mode->vtotal;
}

static int video_plane_check(struct drm_plane *plane, struct drm_atomic_commit *commit)
{
	struct drm_plane_state *state = drm_atomic_get_new_plane_state(commit, plane);
	struct drm_crtc_state *crtc = state->crtc ?
		drm_atomic_get_new_crtc_state(commit, state->crtc) : NULL;
	struct meson_t7_video_phase phase;
	struct meson_t7_video_frame frame;
	struct meson_t7_video_mif mif;
	const struct drm_display_mode *mode;
	u64 required;
	u32 height, input_height;
	int ret;

	if (state->crtc && !meson_t7_display_ready(state->crtc))
		return -EIO;
	if (state->crtc && !crtc)
		return -EINVAL;
	ret = drm_atomic_helper_check_plane_state(state, crtc, 1 << 14, 4 << 16, true, false);
	if (ret || !state->visible)
		return ret;
	if ((state->src.x1 | state->src.y1 | state->src.x2 | state->src.y2) & 0xffff)
		return -EINVAL;
	ret = video_frame(state, &frame);
	if (ret)
		return ret;
	frame.vskip = video_vskip(&frame, state, crtc);
	input_height = frame.crop_height >> frame.vskip;
	ret = meson_t7_video_mif_prepare(&frame, 0, &mif);
	if (ret)
		return ret;
	ret = meson_t7_video_phase_prepare(frame.crop_width, input_height,
					   drm_rect_width(&state->dst),
					   drm_rect_height(&state->dst), &phase);
	if (ret)
		return ret;
	mode = &crtc->adjusted_mode;
	height = drm_rect_height(&state->dst);
	if (!mode->htotal || !mode->vtotal || !mode->clock)
		return -EINVAL;
	/* VPP_SPEED_FACTOR=0x110; cancel vtotal and reduce 0x110/256 to 17/16. */
	required = div_u64(17ULL * frame.crop_width * max(input_height, height) *
			   mode->clock * 1000, 16 * height * mode->htotal);
	if (required > meson_t7_display_clock_rate(state->crtc))
		return -EINVAL;
	return 0;
}

static void video_plane_update(struct drm_plane *plane, struct drm_atomic_commit *commit)
{
	/* All five planes are published by the serialized CRTC commit worker. */
}

static const struct drm_plane_helper_funcs video_plane_helpers = {
	.prepare_fb = meson_t7_video_prepare_fb,
	.cleanup_fb = meson_t7_video_cleanup_fb,
	.atomic_check = video_plane_check,
	.atomic_update = video_plane_update,
	.atomic_disable = video_plane_update,
};

static const struct drm_plane_funcs video_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = meson_t7_video_plane_reset,
	.atomic_duplicate_state = meson_t7_video_plane_duplicate_state,
	.atomic_destroy_state = meson_t7_video_plane_destroy_state,
};

int meson_t7_video_planes_init(struct meson_t7_video_kms *video, struct drm_device *drm)
{
	static const u32 formats[] = { DRM_FORMAT_NV12, DRM_FORMAT_NV21 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	unsigned int i;
	int ret;

	if (!video)
		return 0;
	for (i = 0; i < ARRAY_SIZE(video->planes); i++) {
		struct drm_plane *plane = &video->planes[i];

		ret = drm_universal_plane_init(drm, plane, 1, &video_plane_funcs,
					       formats, ARRAY_SIZE(formats), modifiers,
					       DRM_PLANE_TYPE_OVERLAY, "VD%u", i + 1);
		if (ret)
			return ret;
		drm_plane_helper_add(plane, &video_plane_helpers);
		ret = drm_plane_create_zpos_property(plane, i, 0, 4);
		if (ret)
			return ret;
		ret = drm_plane_create_color_properties(plane, BIT(DRM_COLOR_YCBCR_BT709),
							BIT(DRM_COLOR_YCBCR_LIMITED_RANGE),
							DRM_COLOR_YCBCR_BT709,
							DRM_COLOR_YCBCR_LIMITED_RANGE);
		if (ret)
			return ret;
	}
	return 0;
}

bool meson_t7_video_in_use(struct meson_t7_video_kms *video, struct drm_atomic_commit *commit)
{
	unsigned int i;

	if (!video)
		return false;
	for (i = 0; i < ARRAY_SIZE(video->planes); i++) {
		struct drm_plane *plane = &video->planes[i];
		struct drm_plane_state *old = drm_atomic_get_old_plane_state(commit, plane);
		struct drm_plane_state *next = drm_atomic_get_new_plane_state(commit, plane);

		if ((old && old->fb && old->crtc) || (next && next->fb && next->crtc))
			return true;
	}
	return false;
}

int meson_t7_video_collect(struct meson_t7_video_kms *video, struct drm_atomic_commit *commit,
			   bool blank, struct drm_framebuffer **frames)
{
	unsigned int i;

	if (!video)
		return 0;
	for (i = 0; i < ARRAY_SIZE(video->planes); i++) {
		struct drm_plane *plane = &video->planes[i];
		struct drm_plane_state *state = drm_atomic_get_new_plane_state(commit, plane);

		/* Affected-plane expansion omits disabled, unattached planes. */
		frames[i] = state && !blank && state->visible ? state->fb : NULL;
	}
	return 0;
}

int meson_t7_video_collect_readers(struct meson_t7_video_kms *video,
				   struct drm_atomic_commit *commit, bool blank,
				   struct dma_fence **readers)
{
	unsigned int i;

	if (!video || blank)
		return 0;
	for (i = 0; i < ARRAY_SIZE(video->planes); i++) {
		struct drm_plane_state *state;

		state = drm_atomic_get_new_plane_state(commit, &video->planes[i]);
		if (!state || !state->visible || !state->fb)
			continue;
		readers[i] = meson_t7_video_claim_reader(state);
		if (!readers[i])
			return -EINVAL;
	}
	return 0;
}

/* The OSD group is indivisible; T7's fourth postblend port allows one video above it. */
static int video_ports(struct meson_t7_video_kms *video, struct drm_atomic_commit *commit,
		       const struct meson_t7_osd_layer *osd, u32 *ports)
{
	u32 low = U32_MAX, high = 0, zpos[2] = {};
	bool enabled[2] = {};
	unsigned int i, above = 0;

	for (i = 0; i < MESON_T7_OSD_LAYERS; i++)
		if (osd[i].enabled) {
			low = min(low, osd[i].zpos);
			high = max(high, osd[i].zpos);
		}
	for (i = 0; i < 2; i++) {
		struct drm_plane *plane = &video->planes[i];
		struct drm_plane_state *state = drm_atomic_get_new_plane_state(commit, plane);

		enabled[i] = state && state->fb && state->visible;
		if (!enabled[i])
			continue;
		zpos[i] = state->normalized_zpos;
		if (low != U32_MAX && zpos[i] > low && zpos[i] < high)
			return -EINVAL;
		if (low != U32_MAX && zpos[i] > high) {
			ports[i] = 3;
			above++;
		} else {
			ports[i] = 0;
		}
	}
	if (above > 1)
		return -EINVAL;
	if (enabled[0] && enabled[1] && !above)
		ports[zpos[0] > zpos[1] ? 0 : 1] = 1;
	return 0;
}

int meson_t7_video_zpos_check(struct meson_t7_video_kms *video,
			      struct drm_atomic_commit *commit)
{
	struct meson_t7_osd_layer osd[MESON_T7_OSD_LAYERS] = {};
	struct drm_plane *plane;
	u32 ports[2] = {};
	unsigned int i = 0;

	if (!video)
		return 0;
	drm_for_each_plane(plane, video->planes[0].dev) {
		struct drm_plane_state *state = drm_atomic_get_new_plane_state(commit, plane);

		if (plane == &video->planes[0] || plane == &video->planes[1])
			continue;
		if (i >= ARRAY_SIZE(osd))
			return -EINVAL;
		osd[i].enabled = state && state->fb && state->visible;
		osd[i++].zpos = state ? state->normalized_zpos : 0;
	}
	return video_ports(video, commit, osd, ports);
}

static void video_write(u32 **table, u32 reg, u32 value)
{
	*(*table)++ = reg;
	*(*table)++ = value;
}

/* Zero requests a disconnected, two-phase reconfiguration, never a partial flip. */
int meson_t7_video_flip(struct meson_t7_video_kms *video, struct drm_atomic_commit *commit,
			bool blank, u32 *table, unsigned int pairs)
{
	struct meson_t7_video_mif mif[2];
	bool enabled[2] = {};
	u32 *next = table;
	unsigned int i, j;
	int ret;

	if (!video || blank)
		return 0;
	if (!table || pairs < 6)
		return -ENOSPC;
	for (i = 0; i < 2; i++) {
		struct drm_plane *plane = &video->planes[i];
		struct drm_plane_state *old = drm_atomic_get_old_plane_state(commit, plane);
		struct drm_plane_state *state = drm_atomic_get_new_plane_state(commit, plane);
		struct meson_t7_video_frame frame;
		struct drm_crtc_state *crtc;

		if (!old || !state)
			return 0;
		enabled[i] = state->visible && state->fb;
		if (enabled[i] != (old->visible && old->fb))
			return 0;
		if (!enabled[i])
			continue;
		/* Modeset blanking disconnected the MIF, even if the FB geometry is
		 * unchanged. Rebuild its PPS/skip profile and reconnect in two phases.
		 */
		crtc = drm_atomic_get_new_crtc_state(commit, state->crtc);
		if (!crtc || drm_atomic_crtc_needs_modeset(crtc))
			return 0;
		if (state->crtc != old->crtc || state->normalized_zpos != old->normalized_zpos ||
		    !drm_rect_equals(&state->src, &old->src) ||
		    !drm_rect_equals(&state->dst, &old->dst) ||
		    state->fb->width != old->fb->width || state->fb->height != old->fb->height ||
		    state->fb->format->format != old->fb->format->format ||
		    state->fb->pitches[0] != old->fb->pitches[0] ||
		    state->fb->pitches[1] != old->fb->pitches[1])
			return 0;
		ret = video_frame(state, &frame);
		if (ret)
			return ret;
		ret = meson_t7_video_mif_prepare(&frame, i, &mif[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < 2; i++)
		if (enabled[i])
			for (j = 0; j < 3; j++)
				video_write(&next, mif[i].base + 0x20 + j, mif[i].address[j]);
	return (next - table) * sizeof(*table);
}

int meson_t7_video_build(struct meson_t7_video_kms *video, struct drm_atomic_commit *commit,
			 bool blank, bool enable, const struct meson_t7_osd_layer *osd,
			 u32 *table, unsigned int pairs)
{
	struct meson_t7_video_mif mif[2] = {};
	struct meson_t7_video_frame frames[2];
	struct drm_plane_state *states[2];
	u32 ports[2] = {}, source[4] = { BIT(16), 0, 0, BIT(20) };
	u32 *next = table;
	unsigned int i, j;
	int ret;

	if (!video || !table)
		return -EINVAL;
	if (pairs < MESON_T7_VIDEO_KMS_PAIRS)
		return -ENOSPC;
	ret = video_ports(video, commit, osd, ports);
	if (ret)
		return ret;
	for (i = 0; i < 2; i++) {
		struct drm_crtc_state *crtc;

		states[i] = drm_atomic_get_new_plane_state(commit, &video->planes[i]);
		if (blank || !states[i] || !states[i]->visible || !states[i]->fb) {
			states[i] = NULL;
			continue;
		}
		ret = video_frame(states[i], &frames[i]);
		if (ret)
			return ret;
		crtc = drm_atomic_get_new_crtc_state(commit, states[i]->crtc);
		if (!crtc)
			return -EINVAL;
		frames[i].vskip = video_vskip(&frames[i], states[i], crtc);
		ret = meson_t7_video_mif_prepare(&frames[i], i, &mif[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < 2; i++) {
		u32 base = 0x4800 + i * 0x80;
		struct meson_t7_video_mif *m = &mif[i];
		u32 input_height;

		video_write(&next, base, enable && states[i] ? m->gen | BIT(0) : 0);
		if (!states[i])
			continue;
		input_height = frames[i].crop_height >> frames[i].vskip;
		if (enable) {
			source[ports[i]] |= (i + 1) << 8;
			if (!i)
				source[0] |= 0x11;
			else
				source[1] |= BIT(20);
			continue;
		}
		ret = meson_t7_video_pps_build(next, pairs - (next - table) / 2, i,
					       frames[i].crop_width,
					       input_height,
					       drm_rect_width(&states[i]->dst),
					       drm_rect_height(&states[i]->dst),
					       video_read(video, i ? 0x394c : 0x1d36));
		if (ret < 0)
			return ret;
		next += ret / sizeof(*next);
		for (j = 0; j < 3; j++)
			video_write(&next, base + 0x20 + j, m->address[j]);
		video_write(&next, base + 0x23, m->stride0);
		video_write(&next, base + 0x24,
			    (video_read(video, base + 0x24) & ~m->stride1_mask) | m->stride1);
		video_write(&next, base + 3, m->luma_x);
		video_write(&next, base + 4, m->luma_y);
		video_write(&next, base + 5, m->chroma_x);
		video_write(&next, base + 6, m->chroma_y);
		for (j = 0xb; j <= 0x11; j++)
			video_write(&next, base + j, j >= 0xc && j <= 0xf ? m->repeat_pattern : 0);
		video_write(&next, base + 0x19,
			    (video_read(video, base + 0x19) & ~m->gen2_mask) | m->gen2);
		video_write(&next, base + 0x1c,
			    (video_read(video, base + 0x1c) & ~m->gen3_mask) | m->gen3);
		video_write(&next, base + 0x1d, m->format_ctrl);
		video_write(&next, base + 0x1e, m->format_width);
		video_write(&next, base + 0x38, video_read(video, base + 0x38) & ~GENMASK(14, 13));
		video_write(&next, 0x1a57 + i, input_height << 16 | frames[i].crop_width);
		/* SDR YUV already matches the blend space; bypass only this input's HDR core. */
		video_write(&next, i ? 0x3850 : 0x3800,
			    video_read(video, i ? 0x3850 : 0x3800) & ~(BIT(13) | GENMASK(4, 0)));
		/* T7 ECO: bypass Dolby for this video input, using the 12-bit internal path. */
		video_write(&next, 0x1a83 + i,
			    (video_read(video, 0x1a83 + i) & ~BIT(5)) | BIT(4));
		video_write(&next, 0x1d1c + i * 2,
			    states[i]->dst.x1 << 16 | (states[i]->dst.x2 - 1));
		video_write(&next, 0x1d1d + i * 2,
			    states[i]->dst.y1 << 16 | (states[i]->dst.y2 - 1));
		if (!i) {
			video_write(&next, 0x1d01, frames[i].crop_width);
			video_write(&next, 0x1d02, input_height);
			video_write(&next, 0x1da6,
				    frames[i].crop_width << 16 | input_height);
			video_write(&next, 0x1d1a, frames[i].crop_width - 1);
			video_write(&next, 0x1d1b, 2303);
			video_write(&next, 0x1d20, frames[i].crop_width);
		}
	}
	/* The OSD source stays on its fixed port. Video owns the remaining three ports. */
	video_write(&next, 0x1dfb, source[0]);
	video_write(&next, 0x1dfc, source[1]);
	video_write(&next, 0x1def, source[2]);
	video_write(&next, 0x1dfe, source[3]);
	video_write(&next, 0x1d76, (video_read(video, 0x1d76) & ~GENMASK(8, 0)) | 0x100);
	video_write(&next, 0x1d26, (video_read(video, 0x1d26) & ~GENMASK(26, 18)) |
		    0x100 << 18 | BIT(7) | BIT(6));
	video_write(&next, 0x1d5f, video_read(video, 0x1d5f) & ~(BIT(5) | BIT(4)));
	return (next - table) * sizeof(*table);
}
