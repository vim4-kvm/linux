// SPDX-License-Identifier: GPL-2.0-only
/* Fixed T7 OSD1 handoff. The normal meson initialization is deliberately absent. */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <dt-bindings/power/amlogic,t7-pwrc.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_blend.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "meson_registers.h"
#include "meson_t7_handoff.h"
#include "meson_t7_modeset.h"
#include "meson_t7_scanout.h"
#include "meson_t7_layers.h"
#include "meson_t7_fb.h"
#include "meson_t7_video_kms.h"

#define HANDOFF_WIDTH	1920
#define HANDOFF_HEIGHT	1080
#define HANDOFF_PITCH	5760
#define HANDOFF_BOOT_ADDR	0x3fdeec00
#define HANDOFF_FORMAT_MASK	0x0f3c
#define OSD_REG(reg)	(((reg) - VIU_OSD1_CTRL_STAT) * 4)
#define VENC_REG(reg)	(((reg) - VENC_INTCTRL) * 4)
#define RDMA_REG(reg)	(((reg) - RDMA_AHB_START_ADDR_MAN) * 4)
#define T7_RDMA_START_MSB	0x1132
#define T7_RDMA_END_MSB	0x1133

struct meson_t7_handoff {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_plane extra_planes[2];
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	void __iomem *osd;
	void __iomem *extra_osd[2];
	void __iomem *venc;
	void __iomem *rdma;
	void __iomem *paths;
	u32 *rdma_table;
	u32 rdma_bytes;
	dma_addr_t rdma_dma;
	u32 armed_vsync;
	bool rdma_done;
	spinlock_t lock; /* Address publication and IRQ acknowledgment. */
	wait_queue_head_t wait;
	u32 pending_addr;
	bool pending_enable;
	u32 pending_format, pending_stride, pending_ctrl2;
	u32 pending_ctrl;
	u32 boot_format, boot_stride, boot_ctrl2;
	bool linear_formats;
	struct meson_t7_modeset *modeset;
	bool native_modeset;
	bool managed_sram;
	struct clk *vpu_clock;
	struct meson_t7_video_kms *video;
	bool layers_valid;
	struct meson_t7_osd_layer layers[MESON_T7_OSD_LAYERS];
	u32 requested, written, applied;
	u32 vsync_count;
	bool faulted;
	/* Keep DMA memory alive even if the display interrupt stops. */
	struct meson_t7_scanout buffers;
};

static struct meson_t7_handoff *to_handoff(struct drm_device *drm)
{
	return container_of(drm, struct meson_t7_handoff, drm);
}

bool meson_t7_display_ready(struct drm_crtc *crtc)
{
	return !READ_ONCE(to_handoff(crtc->dev)->faulted);
}
EXPORT_SYMBOL_GPL(meson_t7_display_ready);

unsigned long meson_t7_display_clock_rate(struct drm_crtc *crtc)
{
	return clk_get_rate(to_handoff(crtc->dev)->vpu_clock);
}
EXPORT_SYMBOL_GPL(meson_t7_display_clock_rate);

static u32 meson_t7_osd_alpha(u32 ctrl, u16 alpha)
{
	u32 value = alpha >> 8;

	/* The OSD alpha multiplier represents unity as 0x100, not 0xff. */
	if (value == 0xff)
		value = 0x100;
	return (ctrl & ~GENMASK(20, 12)) | value << 12;
}

static u32 meson_t7_osd_reflect(u32 format, unsigned int rotation)
{
	format &= ~(BIT(28) | BIT(29));
	if (rotation & DRM_MODE_REFLECT_X)
		format |= BIT(28);
	if (rotation & DRM_MODE_REFLECT_Y)
		format |= BIT(29);
	return format;
}

/* Both interrupt orders are possible: do not complete until both have arrived. */
static void meson_t7_handoff_rdma_complete(struct meson_t7_handoff *display)
{
	if (display->rdma_done && display->vsync_count != display->armed_vsync)
		WRITE_ONCE(display->applied, display->written);
}

static irqreturn_t meson_t7_handoff_rdma_irq(int irq, void *data)
{
	struct meson_t7_handoff *display = data;
	unsigned long flags;
	u32 status;

	spin_lock_irqsave(&display->lock, flags);
	status = readl(display->rdma + RDMA_REG(RDMA_STATUS));
	if (!(status & RDMA_IRQ_STAT_CHAN1)) {
		spin_unlock_irqrestore(&display->lock, flags);
		return IRQ_NONE;
	}
	/* Stop replay before the worker can reuse the coherent descriptor table. */
	writel(0, display->rdma + RDMA_REG(RDMA_AUTO_SRC1_SEL));
	writel((readl(display->rdma + RDMA_REG(RDMA_CTRL)) & 0x00ffffff) |
	       RDMA_IRQ_CLEAR_CHAN1, display->rdma + RDMA_REG(RDMA_CTRL));
	readl(display->rdma + RDMA_REG(RDMA_CTRL));
	display->rdma_done = true;
	display->written = display->requested;
	meson_t7_handoff_rdma_complete(display);
	spin_unlock_irqrestore(&display->lock, flags);
	wake_up(&display->wait);
	return IRQ_HANDLED;
}

static void meson_t7_handoff_rdma_arm(struct meson_t7_handoff *display)
{
	u32 *table = display->rdma_table;

	/* Vendor T7 channel 1: word-address/value pairs, VIU1 vsync trigger. */
	if (!display->managed_sram) {
		table[0] = VIU_OSD1_BLK0_CFG_W0;
		table[1] = display->pending_format;
		table[2] = VIU_OSD1_BLK2_CFG_W4;
		table[3] = display->pending_stride;
		table[4] = VIU_OSD1_CTRL_STAT2;
		table[5] = display->pending_ctrl2;
		table[6] = VIU_OSD1_BLK1_CFG_W4;
		table[7] = display->pending_addr >> 4;
		if (display->native_modeset) {
			table[8] = VIU_OSD1_CTRL_STAT;
			table[9] = (display->pending_ctrl & ~BIT(0)) |
				   (display->pending_enable ? BIT(0) : 0);
		}
		display->rdma_bytes = display->native_modeset ? 40 : 32;
	}
	display->rdma_done = false;
	display->armed_vsync = display->vsync_count;
	dma_wmb();
	writel(display->rdma_dma, display->rdma + RDMA_REG(RDMA_AHB_START_ADDR_1));
	writel(0, display->rdma + RDMA_REG(T7_RDMA_START_MSB));
	writel(display->rdma_dma + display->rdma_bytes - 1,
	       display->rdma + RDMA_REG(RDMA_AHB_END_ADDR_1));
	writel(0, display->rdma + RDMA_REG(T7_RDMA_END_MSB));
	writel(1, display->rdma + RDMA_REG(RDMA_AUTO_SRC1_SEL));
}

static irqreturn_t meson_t7_handoff_irq(int irq, void *data)
{
	struct meson_t7_handoff *display = data;
	unsigned long flags;

	/* T7 VIU1 uses an edge IRQ; the vendor handler has no VENC acknowledgment. */
	drm_crtc_handle_vblank(&display->crtc);
	spin_lock_irqsave(&display->lock, flags);
	WRITE_ONCE(display->vsync_count, display->vsync_count + 1);
	if (display->rdma) {
		meson_t7_handoff_rdma_complete(display);
		goto vblank;
	}
	WRITE_ONCE(display->applied, display->written);
	if (display->written != display->requested) {
		if (display->linear_formats) {
			writel(display->pending_format,
			       display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W0));
			writel(display->pending_stride,
			       display->osd + OSD_REG(VIU_OSD1_BLK2_CFG_W4));
			writel(display->pending_ctrl2,
			       display->osd + OSD_REG(VIU_OSD1_CTRL_STAT2));
		}
		writel(display->pending_addr >> 4,
		       display->osd + OSD_REG(VIU_OSD1_BLK1_CFG_W4));
		readl(display->osd + OSD_REG(VIU_OSD1_BLK1_CFG_W4));
		display->written = display->requested;
	}
vblank:
	spin_unlock_irqrestore(&display->lock, flags);
	wake_up(&display->wait);
	return IRQ_HANDLED;
}

static void meson_t7_handoff_finish(struct meson_t7_handoff *display, u32 sequence, bool slow)
{
	u32 start;

	if (!wait_event_timeout(display->wait,
				READ_ONCE(display->applied) == sequence,
				msecs_to_jiffies(100))) {
		WRITE_ONCE(display->faulted, true);
		meson_t7_scanout_retire(&display->buffers, false);
		drm_err(&display->drm, "OSD latch timeout; DMA buffers pinned until reboot\n");
		return;
	}
	meson_t7_scanout_advance(&display->buffers, true, slow);
	if (!slow)
		return;
	/* Do not signal a reusable old buffer until a long RDMA's fetches drain. */
	start = READ_ONCE(display->vsync_count);
	if (!wait_event_timeout(display->wait,
				READ_ONCE(display->vsync_count) != start,
				msecs_to_jiffies(100))) {
		WRITE_ONCE(display->faulted, true);
		meson_t7_scanout_retire(&display->buffers, false);
		drm_err(&display->drm, "OSD drain timeout; DMA buffers pinned until reboot\n");
		return;
	}
	meson_t7_scanout_release_retiring(&display->buffers);
}

/* The caller is the serialized atomic commit worker, never the IRQ handler. */
static void meson_t7_handoff_scanout(struct meson_t7_handoff *display,
				     struct drm_framebuffer *fb, dma_addr_t addr,
				     const struct drm_plane_state *plane)
{
	struct drm_framebuffer *next[MESON_T7_MAX_LAYERS] = { fb };
	unsigned long flags;
	u32 sequence;

	if (READ_ONCE(display->faulted))
		return;
	if (meson_t7_scanout_acquire(&display->buffers, next))
		return;
	spin_lock_irqsave(&display->lock, flags);
	if (display->linear_formats) {
		u32 format = display->boot_format & HANDOFF_FORMAT_MASK;

		/* Vendor osd_fb.c: RGB888 = block 7; XRGB/XBGR = block 5, map 1/2. */
		if (fb && fb->format->format == DRM_FORMAT_XRGB8888)
			format = 0x0504;
		else if (fb && fb->format->format == DRM_FORMAT_XBGR8888)
			format = 0x0508;
		display->pending_format = (display->boot_format & ~HANDOFF_FORMAT_MASK) | format;
		display->pending_stride = display->boot_stride;
		if (fb)
			display->pending_stride = (display->boot_stride & ~0xfff) |
						  (fb->pitches[0] >> 4);
		/* X formats ignore memory alpha; restore the exact boot state on detach. */
		display->pending_ctrl2 = fb ? display->boot_ctrl2 | BIT(14) : display->boot_ctrl2;
	}
	if (display->native_modeset) {
		display->pending_ctrl = readl(display->osd + OSD_REG(VIU_OSD1_CTRL_STAT));
		if (fb && plane) {
			display->pending_ctrl =
				meson_t7_osd_alpha(display->pending_ctrl, plane->alpha);
			display->pending_format =
				meson_t7_osd_reflect(display->pending_format, plane->rotation);
		}
	}
	display->pending_addr = addr;
	display->pending_enable = !!fb;
	sequence = ++display->requested;
	if (display->rdma)
		meson_t7_handoff_rdma_arm(display);
	spin_unlock_irqrestore(&display->lock, flags);
	/*
	 * The vendor OSD path writes the linear address at vsync. Wait for the
	 * following vsync as well before releasing the previous DMA allocation.
	 * M2 instead arms hardware RDMA ahead of vsync, and requires its completion
	 * interrupt AND a vsync before retiring the previous allocation.
	 */
	meson_t7_handoff_finish(display, sequence, false);
}

static void meson_t7_handoff_layer_state(struct drm_plane_state *state,
					 struct meson_t7_osd_layer *layer)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_dma_object *object = drm_fb_dma_get_gem_obj(fb, 0);

	*layer = (struct meson_t7_osd_layer) {
		.enabled = true,
		.format = fb->format->format,
		.pitch = fb->pitches[0],
		.address = object->dma_addr + fb->offsets[0],
		.src_x = state->src.x1 >> 16,
		.src_y = state->src.y1 >> 16,
		.src_w = drm_rect_width(&state->src) >> 16,
		.src_h = drm_rect_height(&state->src) >> 16,
		.x = state->dst.x1,
		.y = state->dst.y1,
		.width = drm_rect_width(&state->dst),
		.height = drm_rect_height(&state->dst),
		.zpos = state->normalized_zpos,
		.alpha = state->alpha,
		.rotation = state->rotation,
		.blend = state->pixel_blend_mode,
	};
}

static void meson_t7_handoff_scanout_layers(struct meson_t7_handoff *display,
					    struct drm_atomic_commit *commit, bool blank)
{
	struct meson_t7_osd_layer layers[MESON_T7_OSD_LAYERS] = {};
	struct drm_framebuffer *next[MESON_T7_MAX_LAYERS] = {};
	struct drm_crtc_state *crtc = drm_atomic_get_new_crtc_state(commit, &display->crtc);
	struct drm_display_mode *mode = &crtc->adjusted_mode;
	u32 *video_table;
	unsigned int i;
	unsigned long flags;
	u32 sequence;
	int bytes;
	int video_bytes;
	bool slow, video_update, video_reconfigure = false;

	if (READ_ONCE(display->faulted))
		return;
	for (i = 0; i < MESON_T7_OSD_LAYERS; i++) {
		struct drm_plane *plane = i ? &display->extra_planes[i - 1] : &display->plane;
		struct drm_plane_state *state = drm_atomic_get_new_plane_state(commit, plane);

		if (!blank && state && state->visible && state->fb) {
			meson_t7_handoff_layer_state(state, &layers[i]);
			next[i] = state->fb;
		}
	}
	if (meson_t7_video_collect(display->video, commit, blank, next + MESON_T7_OSD_LAYERS))
		return;
	video_update = meson_t7_video_in_use(display->video, commit);
	if (meson_t7_scanout_acquire(&display->buffers, next))
		return;
	bytes = display->layers_valid ?
		meson_t7_osd_layers_flip(display->rdma_table, MESON_T7_RDMA_PAIRS,
					 layers, display->layers) : 0;
	slow = !bytes;
	if (slow)
		bytes = meson_t7_osd_layers_build(display->rdma_table, MESON_T7_RDMA_PAIRS,
						  layers, mode->hdisplay, mode->vdisplay,
						  readl(display->paths), readl(display->paths + 4));
	if (bytes >= 0 && video_update) {
		video_table = display->rdma_table + bytes / sizeof(u32);
		video_bytes = meson_t7_video_flip(display->video, commit, blank,
						  video_table,
						  MESON_T7_VIDEO_KMS_PAIRS);
		video_reconfigure = !video_bytes;
		if (video_reconfigure)
			video_bytes = meson_t7_video_build(display->video, commit, blank, false,
							   layers,
							   video_table,
							   MESON_T7_VIDEO_KMS_PAIRS);
		/* Video address writes also retain the previous frame for one full scan. */
		slow = true;
		bytes = video_bytes < 0 ? video_bytes : bytes + video_bytes;
	}
	if (bytes < 0) {
		WRITE_ONCE(display->faulted, true);
		meson_t7_scanout_retire(&display->buffers, false);
		drm_err(&display->drm, "OSD transaction build failed: %d\n", bytes);
		return;
	}
	spin_lock_irqsave(&display->lock, flags);
	display->rdma_bytes = bytes;
	sequence = ++display->requested;
	meson_t7_handoff_rdma_arm(display);
	spin_unlock_irqrestore(&display->lock, flags);
	meson_t7_handoff_finish(display, sequence, slow);
	/*
	 * Vendor ON_REQ -> ON_PENDING: configure with fetch disconnected, drain the
	 * previous generation, then enable on a later vsync. A failure in either
	 * transaction pins every old/new plane until reboot, including video.
	 */
	if (!READ_ONCE(display->faulted) && video_reconfigure && !blank) {
		if (meson_t7_scanout_acquire(&display->buffers, next))
			return;
		bytes = meson_t7_video_build(display->video, commit, false, true, layers,
					     display->rdma_table, MESON_T7_VIDEO_KMS_PAIRS);
		if (bytes < 0) {
			WRITE_ONCE(display->faulted, true);
			meson_t7_scanout_retire(&display->buffers, false);
			return;
		}
		spin_lock_irqsave(&display->lock, flags);
		display->rdma_bytes = bytes;
		sequence = ++display->requested;
		meson_t7_handoff_rdma_arm(display);
		spin_unlock_irqrestore(&display->lock, flags);
		meson_t7_handoff_finish(display, sequence, true);
	}
	if (!READ_ONCE(display->faulted)) {
		for (i = 0; i < MESON_T7_OSD_LAYERS; i++)
			display->layers[i] = layers[i];
		display->layers_valid = true;
	}
}

static void meson_t7_handoff_drain_layers(struct meson_t7_handoff *display)
{
	u32 start = READ_ONCE(display->vsync_count);

	/* Blank has latched; let every old fetch finish before stopping VENC. */
	if (!wait_event_timeout(display->wait,
				READ_ONCE(display->vsync_count) != start,
				msecs_to_jiffies(100))) {
		WRITE_ONCE(display->faulted, true);
		meson_t7_scanout_retire(&display->buffers, false);
		return;
	}
	meson_t7_scanout_retire(&display->buffers, true);
}

static int meson_t7_handoff_layer_check(struct drm_plane_state *new,
					struct drm_crtc_state *crtc_state)
{
	struct drm_framebuffer *fb = new->fb;
	int ret;

	ret = drm_atomic_helper_check_plane_state(new, crtc_state, 1 << 14, 4 << 16,
						  true, false);
	if (ret || !new->visible)
		return ret;
	switch (fb->format->format) {
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
		break;
	default:
		return -EINVAL;
	}
	if ((new->src.x1 | new->src.y1 | new->src.x2 | new->src.y2) & 0xffff)
		return -EINVAL;
	return meson_t7_fb_check(fb);
}

static int meson_t7_handoff_plane_check(struct drm_plane *plane,
					struct drm_atomic_commit *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state, plane);
	struct meson_t7_handoff *display = to_handoff(plane->dev);
	struct drm_crtc_state *crtc_state = NULL;
	dma_addr_t addr;
	unsigned int width = HANDOFF_WIDTH, height = HANDOFF_HEIGHT;
	int ret;

	if (READ_ONCE(display->faulted))
		return -EIO;
	if (new->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new->crtc);
	if (display->managed_sram)
		return meson_t7_handoff_layer_check(new, crtc_state);
	/* RMFB detaches a copied plane state whose visible flag can still be set. */
	ret = drm_atomic_helper_check_plane_state(new, crtc_state,
						  DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret)
		return ret;
	if (!new->visible)
		return 0;
	if (display->native_modeset) {
		width = crtc_state->adjusted_mode.hdisplay;
		height = crtc_state->adjusted_mode.vdisplay;
	}
	if (new->src_x || new->src_y || new->crtc_x || new->crtc_y ||
	    (new->fb->format->format != DRM_FORMAT_RGB888 &&
	     (!display->linear_formats ||
	      (new->fb->format->format != DRM_FORMAT_XRGB8888 &&
	       new->fb->format->format != DRM_FORMAT_XBGR8888))) ||
	    new->src_w != width << 16 || new->src_h != height << 16 ||
	    (unsigned int)new->crtc_w != width || (unsigned int)new->crtc_h != height ||
	    new->fb->width != width || new->fb->height != height ||
	    new->fb->pitches[0] < width * new->fb->format->cpp[0] ||
	    new->fb->pitches[0] > 0xfff * 16 || !IS_ALIGNED(new->fb->pitches[0], 32) ||
	    (!display->linear_formats &&
	     (new->fb->pitches[0] != HANDOFF_PITCH || new->fb->offsets[0])) ||
	    new->fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return -EINVAL;
	addr = drm_fb_dma_get_gem_addr(new->fb, new, 0);
	if (!IS_ALIGNED(addr, 16) ||
	    addr > U32_MAX - new->fb->pitches[0] * height + 1)
		return -ERANGE;
	return 0;
}

static void meson_t7_handoff_plane_update(struct drm_plane *plane,
					  struct drm_atomic_commit *state)
{
	/* The CRTC flush publishes the address after all plane state is ready. */
}

static const struct drm_plane_helper_funcs meson_t7_handoff_plane_helpers = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = meson_t7_handoff_plane_check,
	.atomic_update = meson_t7_handoff_plane_update,
	.atomic_disable = meson_t7_handoff_plane_update,
};

static const struct drm_plane_funcs meson_t7_handoff_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static int meson_t7_plane_properties(struct drm_plane *plane)
{
	struct meson_t7_handoff *display = to_handoff(plane->dev);
	unsigned int rotations = DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y;
	int ret;

	ret = drm_plane_create_alpha_property(plane);
	if (ret)
		return ret;
	ret = drm_plane_create_rotation_property(plane, DRM_MODE_ROTATE_0, rotations);
	if (ret)
		return ret;
	ret = drm_plane_create_blend_mode_property(plane, BIT(DRM_MODE_BLEND_PIXEL_NONE) |
			(to_handoff(plane->dev)->managed_sram ? BIT(DRM_MODE_BLEND_COVERAGE) : 0));
	if (ret)
		return ret;
	if (to_handoff(plane->dev)->managed_sram)
		return drm_plane_create_zpos_property(plane, plane->type == DRM_PLANE_TYPE_CURSOR ?
						     (display->video ? 4 : 2) :
						     (plane->type == DRM_PLANE_TYPE_OVERLAY) +
						     (display->video ? 2 : 0),
						     0, display->video ? 4 : 2);
	return drm_plane_create_zpos_immutable_property(plane, 0);
}

static int meson_t7_handoff_crtc_check(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct meson_t7_handoff *display = to_handoff(crtc->dev);
	int ret;

	if (new->enable && (display->native_modeset ? meson_t7_mode_valid(&new->mode) :
			   meson_t7_handoff_mode_valid(&new->mode)) != MODE_OK)
		return -EINVAL;
	ret = drm_atomic_add_affected_planes(state, crtc);
	if (ret)
		return ret;
	return meson_t7_video_zpos_check(display->video, state);
}

static void meson_t7_handoff_abort_event(struct drm_crtc *crtc, struct drm_crtc_state *new)
{
	struct drm_pending_event *event = &new->event->base;

	/* Unblock atomic dependencies, without claiming a successful DMA latch. */
	if (event->fence) {
		dma_fence_set_error(event->fence, -EIO);
		dma_fence_signal(event->fence);
	}
	if (event->completion) {
		complete_all(event->completion);
		event->completion_release(event->completion);
		event->completion = NULL;
	}
	new->event = NULL;
	drm_event_cancel_free(crtc->dev, event);
}

static void meson_t7_handoff_flush(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
{
	struct meson_t7_handoff *display = to_handoff(crtc->dev);
	struct drm_plane_state *plane = drm_atomic_get_new_plane_state(state, &display->plane);
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_crtc_state *old = drm_atomic_get_old_crtc_state(state, crtc);
	bool vblank_ref = false;

	/* Native enable programs the stopped pipe and completes the initial flip. */
	if (display->native_modeset && drm_atomic_crtc_needs_modeset(new))
		return;
	if (new->active) {
		/* Keep DRM sequence/timestamps running after the idle off-delay. */
		if (old->active)
			vblank_ref = !drm_crtc_vblank_get(crtc);
		if (display->managed_sram)
			meson_t7_handoff_scanout_layers(display, state, false);
		else if (plane->visible)
			meson_t7_handoff_scanout(display, plane->fb,
					 drm_fb_dma_get_gem_addr(plane->fb, plane, 0), plane);
		else
			meson_t7_handoff_scanout(display, NULL,
					 display->native_modeset ? 0 : HANDOFF_BOOT_ADDR, NULL);
	}
	if (new->event && READ_ONCE(display->faulted))
		meson_t7_handoff_abort_event(crtc, new);
	if (new->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, new->event);
		new->event = NULL;
		spin_unlock_irq(&crtc->dev->event_lock);
	}
	if (vblank_ref)
		drm_crtc_vblank_put(crtc);
}

static void meson_t7_handoff_enable(struct drm_crtc *crtc,
				    struct drm_atomic_commit *state)
{
	struct meson_t7_handoff *display = to_handoff(crtc->dev);
	struct drm_plane_state *plane = drm_atomic_get_new_plane_state(state, &display->plane);
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_connector *connector;
	struct drm_connector_state *conn = NULL;
	int ret;

	if (display->native_modeset) {
		/* Gate fetch before changing geometry; no stale boot-frame DMA at 4K. */
		display->layers_valid = false;
		writel(readl(display->osd + OSD_REG(VIU_OSD1_CTRL_STAT)) & ~BIT(0),
		       display->osd + OSD_REG(VIU_OSD1_CTRL_STAT));
		connector = drm_atomic_get_new_connector_for_encoder(state, &display->encoder);
		if (connector && connector->hdmi.funcs)
			conn = drm_atomic_get_new_connector_state(state, connector);
		ret = meson_t7_modeset_enable(display->modeset, &new->adjusted_mode, conn);
		if (ret) {
			WRITE_ONCE(display->faulted, true);
			drm_err(crtc->dev, "ENC2 modeset failed: %d; restart required\n", ret);
			if (new->event)
				meson_t7_handoff_abort_event(crtc, new);
			return;
		}
		writel((new->adjusted_mode.hdisplay - 1) << 16,
		       display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W1));
		writel((new->adjusted_mode.vdisplay - 1) << 16,
		       display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W2));
		writel((new->adjusted_mode.hdisplay - 1) << 16,
		       display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W3));
		writel((new->adjusted_mode.vdisplay - 1) << 16,
		       display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W4));
	}
	drm_crtc_vblank_on(crtc);
	if (display->native_modeset) {
		if (display->managed_sram)
			meson_t7_handoff_scanout_layers(display, state, false);
		else
			meson_t7_handoff_scanout(display, plane->visible ? plane->fb : NULL,
						 plane->visible ?
						 drm_fb_dma_get_gem_addr(plane->fb, plane, 0) : 0,
						 plane);
		if (new->event) {
			if (display->faulted) {
				meson_t7_handoff_abort_event(crtc, new);
			} else {
				spin_lock_irq(&crtc->dev->event_lock);
				drm_crtc_send_vblank_event(crtc, new->event);
				new->event = NULL;
				spin_unlock_irq(&crtc->dev->event_lock);
			}
		}
	}
}

static void meson_t7_handoff_disable(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
	struct meson_t7_handoff *display = to_handoff(crtc->dev);
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);

	if (display->native_modeset) {
		/* Blank at vsync and retire before stopping the encoder's interrupts. */
		if (display->managed_sram)
			meson_t7_handoff_scanout_layers(display, state, true);
		else
			meson_t7_handoff_scanout(display, NULL, 0, NULL);
		if (display->managed_sram && !READ_ONCE(display->faulted))
			meson_t7_handoff_drain_layers(display);
		meson_t7_modeset_disable(display->modeset);
		drm_crtc_vblank_off(crtc);
		if (new->event) {
			if (display->faulted) {
				meson_t7_handoff_abort_event(crtc, new);
			} else if (!new->active) {
				spin_lock_irq(&crtc->dev->event_lock);
				drm_crtc_send_vblank_event(crtc, new->event);
				new->event = NULL;
				spin_unlock_irq(&crtc->dev->event_lock);
			}
		}
		return;
	}
	/* The reserved boot frame is never returned to the DMA allocator. */
	meson_t7_handoff_scanout(to_handoff(crtc->dev), NULL, HANDOFF_BOOT_ADDR, NULL);
	drm_crtc_vblank_off(crtc);
	meson_t7_handoff_flush(crtc, state);
}

static int meson_t7_handoff_enable_vblank(struct drm_crtc *crtc)
{
	return 0;
}

static void meson_t7_handoff_disable_vblank(struct drm_crtc *crtc)
{
	/* Keep the IRQ running to retire buffers during CRTC disable. */
}

static const struct drm_crtc_funcs meson_t7_handoff_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = drm_crtc_cleanup,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = meson_t7_handoff_enable_vblank,
	.disable_vblank = meson_t7_handoff_disable_vblank,
};

static const struct drm_crtc_helper_funcs meson_t7_handoff_crtc_helpers = {
	.atomic_check = meson_t7_handoff_crtc_check,
	.atomic_flush = meson_t7_handoff_flush,
	.atomic_enable = meson_t7_handoff_enable,
	.atomic_disable = meson_t7_handoff_disable,
};

static int meson_t7_handoff_dumb_create(struct drm_file *file, struct drm_device *drm,
					struct drm_mode_create_dumb *args)
{
	struct meson_t7_handoff *display = to_handoff(drm);

	if (((!display->native_modeset &&
	      (args->width != HANDOFF_WIDTH || args->height != HANDOFF_HEIGHT))) ||
	    !args->width || args->width > 3840 || !args->height || args->height > 2160 ||
	    (args->bpp != 24 && (!display->linear_formats || args->bpp != 32)))
		return -EINVAL;
	args->pitch = ALIGN(args->width * (args->bpp / 8), 32);
	args->size = PAGE_ALIGN(args->pitch * args->height);
	return drm_gem_dma_dumb_create_internal(file, drm, args);
}

DEFINE_DRM_GEM_DMA_FOPS(meson_t7_handoff_fops);

static const struct drm_driver meson_t7_handoff_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(meson_t7_handoff_dumb_create),
	.fops = &meson_t7_handoff_fops,
	.name = "meson-t7-handoff",
	.desc = "T7 fixed firmware display handoff",
	.major = 1,
};

static const struct drm_mode_config_funcs meson_t7_handoff_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void meson_t7_handoff_commit_tail(struct drm_atomic_commit *state)
{
	struct drm_device *drm = state->dev;

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_planes(drm, state, 0);
	drm_atomic_helper_commit_modeset_enables(drm, state);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	/* Flush already waited for safe latch; a second vblank halves throughput. */
	drm_atomic_helper_wait_for_flip_done(drm, state);
	drm_atomic_helper_cleanup_planes(drm, state);
}

static const struct drm_mode_config_helper_funcs meson_t7_handoff_mode_helpers = {
	.atomic_commit_tail = meson_t7_handoff_commit_tail,
};

static void meson_t7_handoff_domain_put(void *data)
{
	struct device *domain = data;

	pm_runtime_put_sync(domain);
	dev_pm_domain_detach(domain, true);
}

static int meson_t7_handoff_domains_get(struct device *dev, unsigned int first,
					unsigned int end)
{
	unsigned int i;
	int ret;

	for (i = first; i < end; i++) {
		struct device *domain = dev_pm_domain_attach_by_id(dev, i);

		if (IS_ERR_OR_NULL(domain))
			return domain ? PTR_ERR(domain) : -ENODEV;
		ret = pm_runtime_resume_and_get(domain);
		if (ret < 0) {
			dev_pm_domain_detach(domain, true);
			return ret;
		}
		ret = devm_add_action_or_reset(dev, meson_t7_handoff_domain_put, domain);
		if (ret)
			return ret;
	}
	return 0;
}

static int meson_t7_handoff_sram_get(struct meson_t7_handoff *display)
{
	u32 start;
	unsigned int i;

	/*
	 * Only the M3 profile owns these domains. Stop boot fetches and drain them
	 * while IRQs and the boot SRAM are still alive. A later probe failure can
	 * then unwind domains without removing memory from under an active fetch.
	 */
	writel(readl(display->osd) & ~BIT(0), display->osd);
	for (i = 0; i < ARRAY_SIZE(display->extra_osd); i++)
		writel(readl(display->extra_osd[i]) & ~BIT(0), display->extra_osd[i]);
	readl(display->osd);
	start = READ_ONCE(display->vsync_count);
	if (!wait_event_timeout(display->wait,
				(u32)(READ_ONCE(display->vsync_count) - start) >= 2,
				msecs_to_jiffies(100)))
		return -ETIMEDOUT;
	return meson_t7_handoff_domains_get(display->drm.dev, 3, display->video ? 15 : 10);
}

static void __iomem *meson_t7_handoff_map(struct platform_device *pdev,
					  const char *name, resource_size_t start,
					  resource_size_t size)
{
	struct resource *resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!resource || resource->start != start || resource_size(resource) != size)
		return ERR_PTR(-EINVAL);
	return devm_ioremap_resource(&pdev->dev, resource);
}

static int meson_t7_handoff_check_dt(struct device *dev)
{
	struct of_phandle_args pll;
	struct device_node *node;
	struct reserved_mem *memory;
	int ret;

	if (of_property_read_bool(dev->of_node, "amlogic,managed-osd-sram")) {
		static const u32 ids[] = {
			PWRC_T7_VPU_HDMI_ID, PWRC_T7_VI_CLK1_ID, PWRC_T7_VI_CLK2_ID,
			PWRC_T7_OSD1_MEM_ID, PWRC_T7_OSD2_MEM_ID, PWRC_T7_OSD3_MEM_ID,
			PWRC_T7_OSD_SCALE_MEM_ID, PWRC_T7_OSD2_SCALE_MEM_ID,
			PWRC_T7_OSD_BLD34_MEM_ID, PWRC_T7_RDMA_MEM_ID,
			PWRC_T7_VD1_MEM_ID, PWRC_T7_VD2_MEM_ID, PWRC_T7_VD1_SCALE_MEM_ID,
			PWRC_T7_VD2_SCALE_MEM_ID, PWRC_T7_VD2_OFIFO_MEM_ID,
		};
		unsigned int i, count = of_property_read_bool(dev->of_node,
							     "amlogic,video-planes") ? 15 : 10;

		if (!of_device_is_compatible(dev->of_node, "amlogic,t7-display") ||
		    of_count_phandle_with_args(dev->of_node, "power-domains",
					       "#power-domain-cells") != count)
			return -EINVAL;
		for (i = 0; i < count; i++) {
			ret = of_parse_phandle_with_args(dev->of_node, "power-domains",
							 "#power-domain-cells", i, &pll);
			if (ret)
				return ret;
			ret = pll.args_count == 1 && pll.args[0] == ids[i] &&
				of_device_is_compatible(pll.np, "amlogic,t7-pwrc") ? 0 : -EINVAL;
			of_node_put(pll.np);
			if (ret)
				return ret;
		}
	}
	if (!of_device_is_compatible(dev->of_node, "amlogic,t7-display")) {
		ret = of_parse_phandle_with_args(dev->of_node, "clocks", "#clock-cells", 4, &pll);
		if (ret)
			return ret;
		ret = of_device_is_compatible(pll.np, "amlogic,t7-hdmi-pll-ro") ? 0 : -EINVAL;
		of_node_put(pll.np);
		if (ret)
			return dev_err_probe(dev, ret, "HDMI PLL provider must be read-only\n");
	} else {
		for_each_of_allnodes(node) {
			if (of_device_is_available(node) &&
			    (of_device_is_compatible(node, "amlogic,t7-hdmi-pll-ro") ||
			     of_device_is_compatible(node, "amlogic,t7-hdmi-pll"))) {
				of_node_put(node);
				return dev_err_probe(dev, -EBUSY,
						     "Native display must own HDMI PLL\n");
			}
		}
	}
	for_each_compatible_node(node, NULL, "simple-framebuffer") {
		if (of_device_is_available(node)) {
			of_node_put(node);
			return dev_err_probe(dev, -EBUSY,
					     "Disable simple-framebuffer before handoff\n");
		}
	}
	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	memory = node ? of_reserved_mem_lookup(node) : NULL;
	if (!memory || memory->base != 0x3f800000 || memory->size != 0xc00000 ||
	    !of_property_present(node, "no-map")) {
		of_node_put(node);
		return dev_err_probe(dev, -EINVAL, "Missing reserved firmware frames\n");
	}
	of_node_put(node);
	return 0;
}

static int meson_t7_handoff_boot_check(struct meson_t7_handoff *display)
{
	u32 ctrl = readl(display->osd + OSD_REG(VIU_OSD1_CTRL_STAT));
	u32 format = readl(display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W0));
	u32 unpack = readl(display->osd + OSD_REG(VIU_OSD1_MALI_UNPACK_CTRL));
	u32 htotal = readl(display->venc + VENC_REG(ENCP_VIDEO_MAX_PXCNT));
	u32 vtotal = readl(display->venc + VENC_REG(ENCP_VIDEO_MAX_LNCNT));

	/* Linear OSD ownership does not include the Mali decoder or its DMA. */
	if (format & BIT(30) || unpack & BIT(31))
		return -EBUSY;
	/* RGB888 little endian, linear, 1920x1080 with the matching boot frame. */
	if ((ctrl & 5) != 5 || (format & 0xffff) != 0x8700 ||
	    readl(display->osd + OSD_REG(VIU_OSD1_BLK1_CFG_W4)) != HANDOFF_BOOT_ADDR >> 4 ||
	    readl(display->osd + OSD_REG(VIU_OSD1_BLK2_CFG_W4)) != HANDOFF_PITCH >> 4 ||
	    readl(display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W1)) != (HANDOFF_WIDTH - 1) << 16 ||
	    readl(display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W2)) != (HANDOFF_HEIGHT - 1) << 16 ||
	    readl(display->venc + VENC_REG(ENCP_VIDEO_EN)) != 1 ||
	    htotal != 2199 || vtotal != 1124)
		return -ENODEV;
	display->boot_format = format;
	display->boot_stride = readl(display->osd + OSD_REG(VIU_OSD1_BLK2_CFG_W4));
	display->boot_ctrl2 = readl(display->osd + OSD_REG(VIU_OSD1_CTRL_STAT2));
	drm_info(&display->drm, "boot OSD ctrl=%#x format=%#x ENC2 totals=%u/%u\n",
		 ctrl, format, htotal, vtotal);
	return 0;
}

static int meson_t7_handoff_probe(struct platform_device *pdev)
{
	static const u32 formats[] = { DRM_FORMAT_RGB888, DRM_FORMAT_XRGB8888,
		DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	struct device *dev = &pdev->dev;
	struct meson_t7_handoff *display;
	struct drm_connector *connector;
	struct drm_bridge *bridge;
	struct drm_device *drm;
	struct clk_bulk_data *clocks;
	struct device_node *node;
	unsigned int i;
	size_t rdma_size;
	int ret, irq;

	ret = meson_t7_handoff_check_dt(dev);
	if (ret)
		return ret;

	display = devm_drm_dev_alloc(dev, &meson_t7_handoff_driver,
				     struct meson_t7_handoff, drm);
	if (IS_ERR(display))
		return PTR_ERR(display);
	drm = &display->drm;
	display->native_modeset = of_device_is_compatible(dev->of_node, "amlogic,t7-display");
	display->managed_sram = of_property_read_bool(dev->of_node, "amlogic,managed-osd-sram");
	display->linear_formats = display->native_modeset || of_device_is_compatible(dev->of_node,
							"amlogic,t7-display-linear");
	platform_set_drvdata(pdev, display);
	spin_lock_init(&display->lock);
	init_waitqueue_head(&display->wait);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = devm_clk_bulk_get_all_enabled(dev, &clocks);
	if (ret < 0)
		return ret;
	if (display->native_modeset && (ret != 5 || clk_get_rate(clocks[4].clk) != 24000000))
		return dev_err_probe(dev, -EINVAL, "Native PLL requires a 24MHz reference\n");
	if (display->native_modeset)
		display->vpu_clock = clocks[1].clk;
	ret = meson_t7_handoff_domains_get(dev, 0, 3);
	if (ret)
		return ret;
	display->osd = meson_t7_handoff_map(pdev, "osd1", 0xff006840, 0x80);
	if (IS_ERR(display->osd))
		return PTR_ERR(display->osd);
	if (display->managed_sram) {
		static const struct {
			const char *name;
			resource_size_t base, size;
		} regions[] = {
			{ "scaler2", 0xff016900, 0x38 },
			{ "scaler3", 0xff016a00, 0x38 },
			{ "osd2-address", 0xff006990, 0xc },
			{ "osd2-unpack", 0xff006af4, 4 },
			{ "dimm1", 0xff006b7c, 4 },
			{ "dimm2", 0xff006b3c, 4 },
			{ "dimm3", 0xff00f680, 4 },
		};

		display->paths = meson_t7_handoff_map(pdev, "osd-paths", 0xff006834, 8);
		if (IS_ERR(display->paths))
			return PTR_ERR(display->paths);
		for (i = 0; i < ARRAY_SIZE(regions); i++) {
			void __iomem *bank = meson_t7_handoff_map(pdev, regions[i].name,
							 regions[i].base, regions[i].size);

			if (IS_ERR(bank))
				return PTR_ERR(bank);
		}
		display->extra_osd[0] = meson_t7_handoff_map(pdev, "osd2", 0xff0068c0, 0x80);
		display->extra_osd[1] = meson_t7_handoff_map(pdev, "osd3", 0xff00f600, 0x80);
		for (i = 0; i < ARRAY_SIZE(display->extra_osd); i++)
			if (IS_ERR(display->extra_osd[i]))
				return PTR_ERR(display->extra_osd[i]);
		/* No ownership or decoder-idle contract exists for other boot layers. */
		for (i = 0; i < ARRAY_SIZE(display->extra_osd); i++)
			if (readl(display->extra_osd[i]) & BIT(0))
				return dev_err_probe(dev, -EBUSY, "Firmware extra OSD active\n");
	}
	/* The matching firmware selects ENC2: vendor VENC words have +0x800. */
	display->venc = meson_t7_handoff_map(pdev, "venc", 0xff008db8,
					   display->native_modeset ? 0x608 : 0x108);
	if (IS_ERR(display->venc))
		return PTR_ERR(display->venc);
	/* Reject an incompatible fetch path before changing the hardware RDMA channel. */
	ret = meson_t7_handoff_boot_check(display);
	if (ret)
		return dev_err_probe(dev, ret, "Firmware display contract does not match\n");
	display->video = meson_t7_video_map(pdev);
	if (IS_ERR(display->video))
		return dev_err_probe(dev, PTR_ERR(display->video),
				     "Video firmware contract mismatch\n");
	if (display->video && !display->managed_sram)
		return -EINVAL;
	if (display->linear_formats) {
		u32 control;

		display->rdma = meson_t7_handoff_map(pdev, "rdma", 0xff004400, 0x100);
		if (IS_ERR(display->rdma))
			return PTR_ERR(display->rdma);
		/* Never reset a running channel belonging to another firmware user. */
		for (i = 0; i < 7; i++)
			if (readl(display->rdma + RDMA_REG(RDMA_AUTO_SRC1_SEL) + i * 4))
				return dev_err_probe(dev, -EBUSY, "Firmware RDMA channel active\n");
		rdma_size = display->managed_sram ? MESON_T7_RDMA_PAIRS * 8 : 40;
		if (display->video)
			rdma_size += MESON_T7_VIDEO_KMS_PAIRS * 8;
		display->rdma_table = dmam_alloc_coherent(dev, rdma_size, &display->rdma_dma,
							  GFP_KERNEL);
		if (!display->rdma_table)
			return -ENOMEM;
		control = readl(display->rdma + RDMA_REG(RDMA_CTRL)) & 0x00ffffff;
		control &= ~(RDMA_CTRL_AHB_WR_BURST | RDMA_CTRL_AHB_RD_BURST |
			     RDMA_CTRL_SW_RESET);
		writel(control | RDMA_DEFAULT_CONFIG | (3 << 4) | RDMA_IRQ_CLEAR_CHAN1,
		       display->rdma + RDMA_REG(RDMA_CTRL));
		control = readl(display->rdma + RDMA_REG(RDMA_ACCESS_AUTO));
		control &= ~RDMA_ACCESS_ADDR_INC_CHAN1;
		writel(control | RDMA_ACCESS_RW_FLAG_CHAN1,
		       display->rdma + RDMA_REG(RDMA_ACCESS_AUTO));
		irq = platform_get_irq(pdev, 1);
		if (irq < 0)
			return irq;
		ret = devm_request_irq(dev, irq, meson_t7_handoff_rdma_irq, 0,
				       "meson-t7-osd-rdma", display);
		if (ret)
			return ret;
	}
	if (display->native_modeset) {
		display->modeset = meson_t7_modeset_init(pdev);
		if (IS_ERR(display->modeset))
			return dev_err_probe(dev, PTR_ERR(display->modeset),
					     "No ENC2 timing resources\n");
	}
	node = of_graph_get_remote_node(dev->of_node, 0, 0);
	if (!node || !of_device_is_compatible(node, "amlogic,t7-hdmi-tx") ||
	    (display->native_modeset ? !of_property_present(node, "amlogic,enc2-modeset") :
	     !of_property_present(node, "amlogic,boot-handoff"))) {
		of_node_put(node);
		return dev_err_probe(dev, -EINVAL, "TX must use the boot-handoff bridge\n");
	}
	of_node_put(node);
	bridge = devm_drm_of_get_bridge(dev, dev->of_node, 0, 0);
	if (IS_ERR(bridge))
		return dev_err_probe(dev, PTR_ERR(bridge), "No boot TX bridge\n");
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &meson_t7_handoff_mode_funcs;
	drm->mode_config.helper_private = &meson_t7_handoff_mode_helpers;
	drm->mode_config.normalize_zpos = display->managed_sram;
	drm->mode_config.min_width = display->native_modeset ? 640 : HANDOFF_WIDTH;
	drm->mode_config.max_width = display->native_modeset ? 3840 : HANDOFF_WIDTH;
	drm->mode_config.min_height = display->native_modeset ? 480 : HANDOFF_HEIGHT;
	drm->mode_config.max_height = display->native_modeset ? 2160 : HANDOFF_HEIGHT;
	ret = drm_universal_plane_init(drm, &display->plane, 1, &meson_t7_handoff_plane_funcs,
				       formats, display->managed_sram ? ARRAY_SIZE(formats) :
				       display->linear_formats ? 3 : 1,
				       modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&display->plane, &meson_t7_handoff_plane_helpers);
	if (display->native_modeset) {
		ret = meson_t7_plane_properties(&display->plane);
		if (ret)
			return ret;
	}
	if (display->managed_sram) {
		for (i = 0; i < ARRAY_SIZE(display->extra_planes); i++) {
			struct drm_plane *plane = &display->extra_planes[i];

			ret = drm_universal_plane_init(drm, plane, 1, &meson_t7_handoff_plane_funcs,
						       formats, ARRAY_SIZE(formats), modifiers,
						       i ? DRM_PLANE_TYPE_CURSOR :
						       DRM_PLANE_TYPE_OVERLAY, "OSD%u", i + 2);
			if (ret)
				return ret;
			drm_plane_helper_add(plane, &meson_t7_handoff_plane_helpers);
			ret = meson_t7_plane_properties(plane);
			if (ret)
				return ret;
		}
	}
	ret = meson_t7_video_planes_init(display->video, drm);
	if (ret)
		return ret;
	ret = drm_crtc_init_with_planes(drm, &display->crtc, &display->plane,
					display->managed_sram ? &display->extra_planes[1] : NULL,
					&meson_t7_handoff_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&display->crtc, &meson_t7_handoff_crtc_helpers);
	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	ret = drm_simple_encoder_init(drm, &display->encoder, DRM_MODE_ENCODER_TMDS);
	if (ret)
		return ret;
	display->encoder.possible_crtcs = 1;
	ret = drm_bridge_attach(&display->encoder, bridge, NULL, DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		return ret;
	connector = drm_bridge_connector_init(drm, &display->encoder);
	if (IS_ERR(connector))
		return PTR_ERR(connector);
	if (display->native_modeset && !connector->hdmi.funcs) {
		/* AUTO has only one choice while the inherited matrix is retained. */
		ret = drm_connector_attach_color_format_property(connector,
					BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444));
		if (ret)
			return ret;
	}
	if (connector->hdmi.funcs) {
		u32 colorspaces = BIT(DRM_MODE_COLORIMETRY_BT709_YCC) |
				  BIT(DRM_MODE_COLORIMETRY_BT2020_YCC);

		ret = drm_connector_attach_broadcast_rgb_property(connector);
		if (ret)
			return ret;
		ret = drm_mode_create_hdmi_colorspace_property(connector, colorspaces);
		if (ret)
			return ret;
		ret = drm_connector_attach_colorspace_property(connector);
		if (ret)
			return ret;
	}
	ret = drm_connector_attach_encoder(connector, &display->encoder);
	if (ret)
		return ret;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, meson_t7_handoff_irq, 0, dev_name(dev), display);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	/* Do not enable legacy ENC0 interrupts or rewrite the firmware VIU mux. */
	if (!wait_event_timeout(display->wait, READ_ONCE(display->vsync_count) >= 2,
				msecs_to_jiffies(100)))
		return dev_err_probe(dev, -ETIMEDOUT, "No firmware VIU1 vsync IRQ\n");
	if (display->managed_sram) {
		ret = meson_t7_handoff_sram_get(display);
		if (ret)
			return dev_err_probe(dev, ret, "Cannot safely acquire OSD SRAM\n");
	}
	/* This also enables the bridge connector's HPD callbacks. */
	drmm_kms_helper_poll_init(drm);
	return drm_dev_register(drm, 0);
}

static void meson_t7_handoff_shutdown(struct platform_device *pdev)
{
	struct meson_t7_handoff *display = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&display->drm);
}

static int meson_t7_handoff_suspend(struct device *dev)
{
	/* Firmware state cannot be reconstructed after a system power cycle. */
	return -EBUSY;
}

static const struct dev_pm_ops meson_t7_handoff_pm = {
	.suspend = meson_t7_handoff_suspend,
	.freeze = meson_t7_handoff_suspend,
	.poweroff = meson_t7_handoff_suspend,
};

static const struct of_device_id meson_t7_handoff_match[] = {
	{ .compatible = "amlogic,t7-display-handoff" },
	{ .compatible = "amlogic,t7-display-linear" },
	{ .compatible = "amlogic,t7-display" },
	{ }
};
MODULE_DEVICE_TABLE(of, meson_t7_handoff_match);

static struct platform_driver meson_t7_handoff_platform_driver = {
	.probe = meson_t7_handoff_probe,
	.shutdown = meson_t7_handoff_shutdown,
	.driver = {
		.name = "meson-t7-handoff",
		.of_match_table = meson_t7_handoff_match,
		.pm = &meson_t7_handoff_pm,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(meson_t7_handoff_platform_driver);

MODULE_DESCRIPTION("T7 OSD1 fixed firmware handoff experiment");
MODULE_LICENSE("GPL");
