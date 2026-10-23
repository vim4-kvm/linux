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
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	void __iomem *osd;
	void __iomem *venc;
	void __iomem *rdma;
	u32 *rdma_table;
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
	u32 requested, written, applied;
	u32 vsync_count;
	bool faulted;
	/* Keep DMA memory alive even if the display interrupt stops. */
	struct drm_framebuffer *scanout;
	struct drm_framebuffer *stalled;
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
	display->rdma_done = false;
	display->armed_vsync = display->vsync_count;
	dma_wmb();
	writel(display->rdma_dma, display->rdma + RDMA_REG(RDMA_AHB_START_ADDR_1));
	writel(0, display->rdma + RDMA_REG(T7_RDMA_START_MSB));
	writel(display->rdma_dma + (display->native_modeset ? 39 : 31),
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

/* The caller is the serialized atomic commit worker, never the IRQ handler. */
static void meson_t7_handoff_scanout(struct meson_t7_handoff *display,
				     struct drm_framebuffer *fb, dma_addr_t addr)
{
	unsigned long flags;
	u32 sequence;

	if (READ_ONCE(display->faulted))
		return;
	if (fb)
		drm_framebuffer_get(fb);
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
		struct drm_plane_state *plane = display->plane.state;

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
	if (!wait_event_timeout(display->wait,
				READ_ONCE(display->applied) == sequence,
				msecs_to_jiffies(100))) {
		WRITE_ONCE(display->faulted, true);
		display->stalled = fb;
		drm_err(&display->drm, "OSD latch timeout; DMA buffers pinned until reboot\n");
		return;
	}
	if (display->scanout)
		drm_framebuffer_put(display->scanout);
	display->scanout = fb;
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
	unsigned int rotations = DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y;
	int ret;

	ret = drm_plane_create_alpha_property(plane);
	if (ret)
		return ret;
	ret = drm_plane_create_rotation_property(plane, DRM_MODE_ROTATE_0, rotations);
	if (ret)
		return ret;
	ret = drm_plane_create_blend_mode_property(plane, BIT(DRM_MODE_BLEND_PIXEL_NONE));
	if (ret)
		return ret;
	return drm_plane_create_zpos_immutable_property(plane, 0);
}

static int meson_t7_handoff_crtc_check(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct meson_t7_handoff *display = to_handoff(crtc->dev);

	if (new->enable && (display->native_modeset ? meson_t7_mode_valid(&new->mode) :
			   meson_t7_handoff_mode_valid(&new->mode)) != MODE_OK)
		return -EINVAL;
	return drm_atomic_add_affected_planes(state, crtc);
}

static void meson_t7_handoff_abort_event(struct drm_crtc *crtc)
{
	struct drm_pending_event *event = &crtc->state->event->base;

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
	crtc->state->event = NULL;
	drm_event_cancel_free(crtc->dev, event);
}

static void meson_t7_handoff_flush(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
{
	struct meson_t7_handoff *display = to_handoff(crtc->dev);
	struct drm_plane_state *plane = display->plane.state;
	struct drm_crtc_state *old = drm_atomic_get_old_crtc_state(state, crtc);
	bool vblank_ref = false;

	/* Native enable programs the stopped pipe and completes the initial flip. */
	if (display->native_modeset && drm_atomic_crtc_needs_modeset(crtc->state))
		return;
	if (crtc->state->active) {
		/* Keep DRM sequence/timestamps running after the idle off-delay. */
		if (old->active)
			vblank_ref = !drm_crtc_vblank_get(crtc);
		if (plane->visible)
			meson_t7_handoff_scanout(display, plane->fb,
						 drm_fb_dma_get_gem_addr(plane->fb, plane, 0));
		else
			meson_t7_handoff_scanout(display, NULL,
					 display->native_modeset ? 0 : HANDOFF_BOOT_ADDR);
	}
	if (crtc->state->event && READ_ONCE(display->faulted))
		meson_t7_handoff_abort_event(crtc);
	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
		spin_unlock_irq(&crtc->dev->event_lock);
	}
	if (vblank_ref)
		drm_crtc_vblank_put(crtc);
}

static void meson_t7_handoff_enable(struct drm_crtc *crtc,
				    struct drm_atomic_commit *state)
{
	struct meson_t7_handoff *display = to_handoff(crtc->dev);
	struct drm_plane_state *plane = display->plane.state;
	int ret;

	if (display->native_modeset) {
		/* Gate fetch before changing geometry; no stale boot-frame DMA at 4K. */
		writel(readl(display->osd + OSD_REG(VIU_OSD1_CTRL_STAT)) & ~BIT(0),
		       display->osd + OSD_REG(VIU_OSD1_CTRL_STAT));
		ret = meson_t7_modeset_enable(display->modeset, &crtc->state->adjusted_mode);
		if (ret) {
			WRITE_ONCE(display->faulted, true);
			drm_err(crtc->dev, "ENC2 modeset failed: %d; restart required\n", ret);
			if (crtc->state->event)
				meson_t7_handoff_abort_event(crtc);
			return;
		}
		writel((crtc->state->adjusted_mode.hdisplay - 1) << 16,
		       display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W1));
		writel((crtc->state->adjusted_mode.vdisplay - 1) << 16,
		       display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W2));
		writel((crtc->state->adjusted_mode.hdisplay - 1) << 16,
		       display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W3));
		writel((crtc->state->adjusted_mode.vdisplay - 1) << 16,
		       display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W4));
	}
	drm_crtc_vblank_on(crtc);
	if (display->native_modeset) {
		meson_t7_handoff_scanout(display, plane->visible ? plane->fb : NULL,
			plane->visible ? drm_fb_dma_get_gem_addr(plane->fb, plane, 0) : 0);
		if (crtc->state->event) {
			if (display->faulted) {
				meson_t7_handoff_abort_event(crtc);
			} else {
				spin_lock_irq(&crtc->dev->event_lock);
				drm_crtc_send_vblank_event(crtc, crtc->state->event);
				crtc->state->event = NULL;
				spin_unlock_irq(&crtc->dev->event_lock);
			}
		}
	}
}

static void meson_t7_handoff_disable(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
	struct meson_t7_handoff *display = to_handoff(crtc->dev);

	if (display->native_modeset) {
		/* Blank at vsync and retire before stopping the encoder's interrupts. */
		meson_t7_handoff_scanout(display, NULL, 0);
		meson_t7_modeset_disable(display->modeset);
		drm_crtc_vblank_off(crtc);
		if (crtc->state->event) {
			if (display->faulted) {
				meson_t7_handoff_abort_event(crtc);
			} else if (!crtc->state->active) {
				spin_lock_irq(&crtc->dev->event_lock);
				drm_crtc_send_vblank_event(crtc, crtc->state->event);
				crtc->state->event = NULL;
				spin_unlock_irq(&crtc->dev->event_lock);
			}
		}
		return;
	}
	/* The reserved boot frame is never returned to the DMA allocator. */
	meson_t7_handoff_scanout(to_handoff(crtc->dev), NULL, HANDOFF_BOOT_ADDR);
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

static int meson_t7_handoff_probe(struct platform_device *pdev)
{
	static const u32 formats[] = { DRM_FORMAT_RGB888, DRM_FORMAT_XRGB8888,
				       DRM_FORMAT_XBGR8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	struct device *dev = &pdev->dev;
	struct meson_t7_handoff *display;
	struct drm_connector *connector;
	struct drm_bridge *bridge;
	struct drm_device *drm;
	struct clk_bulk_data *clocks;
	struct device_node *node;
	unsigned int i;
	u32 ctrl, format, htotal, vtotal;
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
	for (i = 0; i < 3; i++) {
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
	display->osd = meson_t7_handoff_map(pdev, "osd1", 0xff006840, 0x80);
	if (IS_ERR(display->osd))
		return PTR_ERR(display->osd);
	/* The matching firmware selects ENC2: vendor VENC words have +0x800. */
	display->venc = meson_t7_handoff_map(pdev, "venc", 0xff008db8,
					   display->native_modeset ? 0x608 : 0x108);
	if (IS_ERR(display->venc))
		return PTR_ERR(display->venc);
	if (display->linear_formats) {
		u32 control;

		display->rdma = meson_t7_handoff_map(pdev, "rdma", 0xff004400, 0x100);
		if (IS_ERR(display->rdma))
			return PTR_ERR(display->rdma);
		/* Never reset a running channel belonging to another firmware user. */
		for (i = 0; i < 7; i++)
			if (readl(display->rdma + RDMA_REG(RDMA_AUTO_SRC1_SEL) + i * 4))
				return dev_err_probe(dev, -EBUSY, "Firmware RDMA channel active\n");
		display->rdma_table = dmam_alloc_coherent(dev, 40, &display->rdma_dma,
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
	ctrl = readl(display->osd + OSD_REG(VIU_OSD1_CTRL_STAT));
	format = readl(display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W0));
	display->boot_format = format;
	display->boot_stride = readl(display->osd + OSD_REG(VIU_OSD1_BLK2_CFG_W4));
	display->boot_ctrl2 = readl(display->osd + OSD_REG(VIU_OSD1_CTRL_STAT2));
	htotal = readl(display->venc + VENC_REG(ENCP_VIDEO_MAX_PXCNT));
	vtotal = readl(display->venc + VENC_REG(ENCP_VIDEO_MAX_LNCNT));
	dev_info(dev, "boot OSD ctrl=%#x format=%#x ENC2 totals=%u/%u\n",
		 ctrl, format, htotal, vtotal);
	/* RGB888 little endian, linear, 1920x1080 with the matching boot frame. */
	if ((ctrl & 5) != 5 || (format & 0xffff) != 0x8700 ||
	    readl(display->osd + OSD_REG(VIU_OSD1_BLK1_CFG_W4)) != HANDOFF_BOOT_ADDR >> 4 ||
	    readl(display->osd + OSD_REG(VIU_OSD1_BLK2_CFG_W4)) != HANDOFF_PITCH >> 4 ||
	    readl(display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W1)) != (HANDOFF_WIDTH - 1) << 16 ||
	    readl(display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W2)) != (HANDOFF_HEIGHT - 1) << 16 ||
	    readl(display->venc + VENC_REG(ENCP_VIDEO_EN)) != 1 ||
	    htotal != 2199 || vtotal != 1124)
		return dev_err_probe(dev, -ENODEV, "Firmware display contract does not match\n");
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
	drm->mode_config.min_width = display->native_modeset ? 640 : HANDOFF_WIDTH;
	drm->mode_config.max_width = display->native_modeset ? 3840 : HANDOFF_WIDTH;
	drm->mode_config.min_height = display->native_modeset ? 480 : HANDOFF_HEIGHT;
	drm->mode_config.max_height = display->native_modeset ? 2160 : HANDOFF_HEIGHT;
	ret = drm_universal_plane_init(drm, &display->plane, 1, &meson_t7_handoff_plane_funcs,
				       formats, display->linear_formats ? ARRAY_SIZE(formats) : 1,
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
	ret = drm_crtc_init_with_planes(drm, &display->crtc, &display->plane, NULL,
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
	if (display->native_modeset) {
		/* AUTO has only one choice while the inherited matrix is retained. */
		ret = drm_connector_attach_color_format_property(connector,
					BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444));
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
