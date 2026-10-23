// SPDX-License-Identifier: GPL-2.0-only
/* Fixed T7 OSD1 handoff. The normal meson initialization is deliberately absent. */
#include <linux/clk.h>
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
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
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

#define HANDOFF_WIDTH	1920
#define HANDOFF_HEIGHT	1080
#define HANDOFF_PITCH	5760
#define HANDOFF_BOOT_ADDR	0x3fdeec00
#define OSD_REG(reg)	(((reg) - VIU_OSD1_CTRL_STAT) * 4)
#define VENC_REG(reg)	(((reg) - VENC_INTCTRL) * 4)

struct meson_t7_handoff {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	void __iomem *osd;
	void __iomem *venc;
	spinlock_t lock; /* Address publication and IRQ acknowledgment. */
	wait_queue_head_t wait;
	u32 pending_addr;
	u32 requested, written, applied;
	u32 boot_interrupts;
	bool faulted;
	/* Keep DMA memory alive even if the display interrupt stops. */
	struct drm_framebuffer *scanout;
	struct drm_framebuffer *stalled;
};

static struct meson_t7_handoff *to_handoff(struct drm_device *drm)
{
	return container_of(drm, struct meson_t7_handoff, drm);
}

static irqreturn_t meson_t7_handoff_irq(int irq, void *data)
{
	struct meson_t7_handoff *display = data;
	unsigned long flags;

	/* VENC_INTFLAG is read-to-clear, as in the normal meson handler. */
	readl(display->venc + VENC_REG(VENC_INTFLAG));
	spin_lock_irqsave(&display->lock, flags);
	WRITE_ONCE(display->applied, display->written);
	if (display->written != display->requested) {
		writel(display->pending_addr >> 4,
		       display->osd + OSD_REG(VIU_OSD1_BLK1_CFG_W4));
		readl(display->osd + OSD_REG(VIU_OSD1_BLK1_CFG_W4));
		display->written = display->requested;
	}
	spin_unlock_irqrestore(&display->lock, flags);
	drm_crtc_handle_vblank(&display->crtc);
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
	display->pending_addr = addr;
	sequence = ++display->requested;
	spin_unlock_irqrestore(&display->lock, flags);
	/*
	 * The vendor OSD path writes the linear address at vsync. Wait for the
	 * following vsync as well before releasing the previous DMA allocation.
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
	struct drm_crtc_state *crtc_state;
	dma_addr_t addr;
	int ret;

	if (READ_ONCE(display->faulted))
		return -EIO;
	if (!new->crtc)
		return 0;
	crtc_state = drm_atomic_get_new_crtc_state(state, new->crtc);
	ret = drm_atomic_helper_check_plane_state(new, crtc_state,
						  DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret)
		return ret;
	if (!new->visible)
		return 0;
	if (new->src_x || new->src_y || new->crtc_x || new->crtc_y ||
	    new->fb->format->format != DRM_FORMAT_RGB888 ||
	    new->src_w != HANDOFF_WIDTH << 16 || new->src_h != HANDOFF_HEIGHT << 16 ||
	    new->crtc_w != HANDOFF_WIDTH || new->crtc_h != HANDOFF_HEIGHT ||
	    new->fb->width != HANDOFF_WIDTH || new->fb->height != HANDOFF_HEIGHT ||
	    new->fb->pitches[0] != HANDOFF_PITCH || new->fb->offsets[0] ||
	    new->fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return -EINVAL;
	addr = drm_fb_dma_get_gem_addr(new->fb, new, 0);
	if (!IS_ALIGNED(addr, 16) || addr > U32_MAX - HANDOFF_PITCH * HANDOFF_HEIGHT + 1)
		return -ERANGE;
	return 0;
}

static void meson_t7_handoff_plane_update(struct drm_plane *plane,
					  struct drm_atomic_commit *state)
{
	/* The CRTC flush publishes the address after all plane state is ready. */
}

static const struct drm_plane_helper_funcs meson_t7_handoff_plane_helpers = {
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

static int meson_t7_handoff_crtc_check(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);

	if (new->enable && !drm_mode_equal(&new->mode, &meson_t7_handoff_mode))
		return -EINVAL;
	return drm_atomic_add_affected_planes(state, crtc);
}

static void meson_t7_handoff_flush(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
{
	struct meson_t7_handoff *display = to_handoff(crtc->dev);
	struct drm_plane_state *plane = display->plane.state;
	struct drm_crtc_state *old = drm_atomic_get_old_crtc_state(state, crtc);
	bool vblank_ref = false;

	if (crtc->state->active) {
		/* Keep DRM sequence/timestamps running after the idle off-delay. */
		if (old->active)
			vblank_ref = !drm_crtc_vblank_get(crtc);
		if (plane->visible)
			meson_t7_handoff_scanout(display, plane->fb,
						 drm_fb_dma_get_gem_addr(plane->fb, plane, 0));
		else
			meson_t7_handoff_scanout(display, NULL, HANDOFF_BOOT_ADDR);
	}
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
	drm_crtc_vblank_on(crtc);
}

static void meson_t7_handoff_disable(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
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
	if (args->width != HANDOFF_WIDTH || args->height != HANDOFF_HEIGHT || args->bpp != 24)
		return -EINVAL;
	args->pitch = HANDOFF_PITCH;
	args->size = PAGE_ALIGN(HANDOFF_PITCH * HANDOFF_HEIGHT);
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

static void meson_t7_handoff_domain_put(void *data)
{
	struct device *domain = data;

	pm_runtime_put_sync(domain);
	dev_pm_domain_detach(domain, true);
}

static void meson_t7_handoff_restore_irq(void *data)
{
	struct meson_t7_handoff *display = data;

	writel(display->boot_interrupts, display->venc + VENC_REG(VENC_INTCTRL));
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

	ret = of_parse_phandle_with_args(dev->of_node, "clocks", "#clock-cells", 4, &pll);
	if (ret)
		return ret;
	ret = of_device_is_compatible(pll.np, "amlogic,t7-hdmi-pll-ro") ? 0 : -EINVAL;
	of_node_put(pll.np);
	if (ret)
		return dev_err_probe(dev, ret, "HDMI PLL provider must be read-only\n");
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
	static const u32 formats[] = { DRM_FORMAT_RGB888 };
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
	platform_set_drvdata(pdev, display);
	spin_lock_init(&display->lock);
	init_waitqueue_head(&display->wait);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = devm_clk_bulk_get_all_enabled(dev, &clocks);
	if (ret < 0)
		return ret;
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
	display->venc = meson_t7_handoff_map(pdev, "venc", 0xff006db8, 0x108);
	if (IS_ERR(display->venc))
		return PTR_ERR(display->venc);
	ctrl = readl(display->osd + OSD_REG(VIU_OSD1_CTRL_STAT));
	format = readl(display->osd + OSD_REG(VIU_OSD1_BLK0_CFG_W0));
	htotal = readl(display->venc + VENC_REG(ENCP_VIDEO_MAX_PXCNT));
	vtotal = readl(display->venc + VENC_REG(ENCP_VIDEO_MAX_LNCNT));
	dev_info(dev, "boot OSD ctrl=%#x format=%#x VENC totals=%u/%u\n",
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
	    !of_property_present(node, "amlogic,boot-handoff")) {
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
	drm->mode_config.min_width = HANDOFF_WIDTH;
	drm->mode_config.max_width = HANDOFF_WIDTH;
	drm->mode_config.min_height = HANDOFF_HEIGHT;
	drm->mode_config.max_height = HANDOFF_HEIGHT;
	ret = drm_universal_plane_init(drm, &display->plane, 1, &meson_t7_handoff_plane_funcs,
				       formats, ARRAY_SIZE(formats), modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&display->plane, &meson_t7_handoff_plane_helpers);
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
	/* Preserve all other VENC interrupt users; VDIN has a separate IRQ. */
	display->boot_interrupts = readl(display->venc + VENC_REG(VENC_INTCTRL));
	ret = devm_add_action_or_reset(dev, meson_t7_handoff_restore_irq, display);
	if (ret)
		return ret;
	writel(display->boot_interrupts | VENC_INTCTRL_ENCP_LNRST_INT_EN,
	       display->venc + VENC_REG(VENC_INTCTRL));
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
