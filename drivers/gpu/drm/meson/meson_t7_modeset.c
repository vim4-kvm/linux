// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019 Amlogic, Inc. All rights reserved. */
/* ENC2 timing and clocks. No VDIN, canvas or global VPU initialization. */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "meson_registers.h"
#include "meson_t7_modeset.h"

struct meson_t7_window {
	u32 first, last;
	void __iomem *base;
};

struct meson_t7_modeset {
	struct regmap *clkctrl;
	void __iomem *pll, *vid2, *vid_div;
	struct meson_t7_window vcbus[9];
};

int meson_t7_clock_calculate(unsigned int pixel_khz, struct meson_t7_clock *clock)
{
	u32 vco, div = 1, remainder;

	if (pixel_khz < 25175 || pixel_khz > 594000)
		return -ERANGE;
	vco = pixel_khz * 10;
	while (vco < 3000000 && div < 16) {
		vco *= 2;
		div *= 2;
	}
	if (vco < 3000000 || vco >= 6000000)
		return -ERANGE;
	clock->od1 = min(div, 4U);
	clock->od2 = div / clock->od1;
	clock->fractional = abs((int)vco - 5934066) <= div * 5;
	/* The 5.94GHz recipe includes the vendor's 1000/1001 trim. */
	clock->vco = clock->fractional ? 5940000 : vco;
	clock->m = clock->vco / 24000;
	remainder = clock->vco % 24000;
	clock->frac = (remainder / 16 * (1 << 17) / 1500) +
		     (remainder % 16 * (1 << 13) / 1500);
	if (clock->fractional)
		clock->frac = 0x8168;
	return 0;
}

enum drm_mode_status meson_t7_mode_valid(const struct drm_display_mode *mode)
{
	struct meson_t7_clock clock;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;
	if (mode->flags & ~(DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC |
			    DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC))
		return MODE_BAD;
	if (mode->hdisplay < 640 || mode->hdisplay > 3840 || mode->htotal > 8192)
		return MODE_BAD_HVALUE;
	if (mode->vdisplay < 480 || mode->vdisplay > 2160 || mode->vtotal > 4096)
		return MODE_BAD_VVALUE;
	if (mode->hdisplay >= mode->hsync_start || mode->hsync_start >= mode->hsync_end ||
	    mode->hsync_end >= mode->htotal || mode->hsync_start - mode->hdisplay < 5)
		return MODE_H_ILLEGAL;
	/* The VENC FIFO requires at least two front-porch lines. */
	if (mode->vdisplay >= mode->vsync_start || mode->vsync_start >= mode->vsync_end ||
	    mode->vsync_end >= mode->vtotal || mode->vsync_start - mode->vdisplay < 2)
		return MODE_V_ILLEGAL;
	if (meson_t7_clock_calculate(mode->clock, &clock))
		return MODE_NOCLOCK;
	/* Leave room for two frame boundaries inside the 100ms latch deadline. */
	if (mode->clock * 1000 < 23 * mode->htotal * mode->vtotal)
		return MODE_VSYNC;
	return MODE_OK;
}

static void meson_t7_write(struct meson_t7_modeset *hw, u32 reg, u32 value)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hw->vcbus); i++) {
		struct meson_t7_window *window = &hw->vcbus[i];

		if (reg >= window->first && reg <= window->last) {
			writel(value, window->base + (reg - window->first) * 4);
			return;
		}
	}
	WARN_ON_ONCE(1);
}

static bool meson_t7_pll_lock(struct meson_t7_modeset *hw)
{
	unsigned int retry;
	u32 value;

	for (retry = 0; retry < 10; retry++) {
		usleep_range(50, 60);
		value = readl(hw->pll);
		if ((value & GENMASK(31, 30)) == GENMASK(31, 30))
			return true;
		writel(value | BIT(29), hw->pll);
		writel(value & ~BIT(29), hw->pll);
	}
	return false;
}

static bool meson_t7_pll_recipe(struct meson_t7_modeset *hw, u32 m, u32 frac,
				u32 ctrl3, u32 ctrl4, u32 ctrl6, bool automatic)
{
	u32 ctrl0 = (automatic ? 0x3b000400 : 0x3b3a0400) | m;

	writel(ctrl0, hw->pll);
	writel(frac, hw->pll + 4);
	writel(0, hw->pll + 8);
	writel(ctrl3, hw->pll + 12);
	writel(ctrl4, hw->pll + 16);
	writel(0x3927200a, hw->pll + 20);
	writel(ctrl6, hw->pll + 24);
	writel(ctrl0 & ~BIT(29), hw->pll);
	return meson_t7_pll_lock(hw);
}

static int meson_t7_pll_set(struct meson_t7_modeset *hw,
			  const struct meson_t7_clock *clock, bool high_rate)
{
	u32 ctrl3, ctrl4, value;
	bool locked;

	if (clock->vco == 5940000) {
		ctrl3 = high_rate || clock->fractional ? 0x6a685c00 : 0x6a68dc00;
		ctrl4 = high_rate ? 0x11551293 :
			clock->fractional ? 0x44331290 : 0x65771290;
		locked = meson_t7_pll_recipe(hw, 0xf7, clock->frac, ctrl3, ctrl4,
					     0x56540000, false);
		if (!locked && clock->fractional)
			return -ETIMEDOUT;
		if (!locked)
			locked = meson_t7_pll_recipe(hw, 0x7b, 0x18000, 0xea68dc00,
						     0x65771290, 0x56540000, false);
		if (!locked)
			locked = meson_t7_pll_recipe(hw, 0xf7, 0x10000, 0xea68dc00,
						     0x65771290, 0x55540000, false);
	} else {
		locked = meson_t7_pll_recipe(hw, clock->m, clock->frac, 0x6a685c00,
					     0x33771290, 0x55540000, true);
	}
	if (!locked)
		return -ETIMEDOUT;
	value = readl(hw->pll) & ~GENMASK(21, 16);
	value |= __ffs(clock->od1) << 16 | __ffs(clock->od2) << 18 | BIT(20);
	writel(value, hw->pll);
	return 0;
}

static void meson_t7_video_clock(struct meson_t7_modeset *hw)
{
	u32 value;

	/* Shared clock-controller regmap serializes masked updates with CCF. */
	regmap_update_bits(hw->clkctrl, 0xd8, GENMASK(27, 16), 0);
	regmap_update_bits(hw->clkctrl, 0xdc, GENMASK(22, 0),
			   (2 << 21) | BIT(20) | (2 << 13) | BIT(12) | (2 << 5) | BIT(4));
	value = readl(hw->vid_div) & ~(GENMASK(19, 0));
	writel(value | BIT(25), hw->vid_div);
	/* VID_PLL_DIV_5: load shift preset, then enable output. */
	writel(value | BIT(25) | (2 << 16) | BIT(15) | 0x739c, hw->vid_div);
	writel(value | BIT(25) | (2 << 16) | BIT(19) | 0x739c, hw->vid_div);
	writel(readl(hw->vid2) & ~BIT(19), hw->vid2);
	udelay(2);
	writel(readl(hw->vid2 + 8) & ~GENMASK(7, 0), hw->vid2 + 8);
	writel(readl(hw->vid2 + 12) & ~GENMASK(15, 12), hw->vid2 + 12);
	udelay(5);
	value = readl(hw->vid2) & ~GENMASK(18, 16);
	writel(value | BIT(19) | 3, hw->vid2);
	writel(readl(hw->vid2 + 4) | BIT(10) | BIT(9) | BIT(5) | BIT(3), hw->vid2 + 4);
}

static void meson_t7_venc_set(struct meson_t7_modeset *hw,
			     const struct drm_display_mode *mode)
{
	u32 h_end = mode->htotal - (mode->hsync_start - mode->hdisplay - 5);
	u32 v_end = mode->vtotal - (mode->vsync_start - mode->vdisplay - 1);
	u32 h_begin = h_end - mode->hdisplay, v_begin = v_end - mode->vdisplay;
	u32 hsync = mode->hsync_end - mode->hsync_start;
	u32 vsync = mode->vsync_end - mode->vsync_start;

	meson_t7_write(hw, ENCP_DVI_HSO_BEGIN, 5);
	meson_t7_write(hw, ENCP_DVI_HSO_END, 5 + hsync);
	meson_t7_write(hw, ENCP_DVI_VSO_BLINE_EVN, 1);
	meson_t7_write(hw, ENCP_DVI_VSO_ELINE_EVN, 1 + vsync);
	meson_t7_write(hw, ENCP_DVI_VSO_BEGIN_EVN, 5);
	meson_t7_write(hw, ENCP_DVI_VSO_END_EVN, 5);
	meson_t7_write(hw, ENCP_DE_H_BEGIN, h_begin);
	meson_t7_write(hw, ENCP_DE_H_END, h_end);
	meson_t7_write(hw, ENCP_DE_V_BEGIN_EVEN, v_begin);
	meson_t7_write(hw, ENCP_DE_V_END_EVEN, v_end);
	meson_t7_write(hw, ENCP_VIDEO_MODE, 0x4040);
	meson_t7_write(hw, ENCP_VIDEO_MODE_ADV, 0x18);
	meson_t7_write(hw, ENCP_VIDEO_HAVON_BEGIN, h_begin - 2);
	meson_t7_write(hw, ENCP_VIDEO_HAVON_END, h_end - 3);
	meson_t7_write(hw, ENCP_VIDEO_VAVON_BLINE, v_begin);
	meson_t7_write(hw, ENCP_VIDEO_VAVON_ELINE, v_end - 1);
	meson_t7_write(hw, ENCP_VIDEO_HSO_BEGIN, 3);
	meson_t7_write(hw, ENCP_VIDEO_HSO_END, 3 + hsync);
	meson_t7_write(hw, ENCP_VIDEO_VSO_BEGIN, 0);
	meson_t7_write(hw, ENCP_VIDEO_VSO_END, 0);
	meson_t7_write(hw, ENCP_VIDEO_VSO_BLINE, 1);
	meson_t7_write(hw, ENCP_VIDEO_VSO_ELINE, 1 + vsync);
	meson_t7_write(hw, ENCP_VIDEO_MAX_PXCNT, mode->htotal - 1);
	meson_t7_write(hw, ENCP_VIDEO_MAX_LNCNT, mode->vtotal - 1);
}

void meson_t7_modeset_disable(struct meson_t7_modeset *hw)
{
	meson_t7_write(hw, ENCP_VIDEO_EN, 0);
}

static void meson_t7_osd_matrix(struct meson_t7_modeset *hw)
{
	/* T7 osd_hw.c: RGB709_to_YUV709l_coeff, after the OSD blend. */
	static const int coeff[9] = { 186, 627, 63, -103, -345, 448, 448, -407, -41 };
	unsigned int i;

	meson_t7_write(hw, 0x391b, 0);
	meson_t7_write(hw, 0x391c, 0);
	for (i = 0; i < 4; i++)
		meson_t7_write(hw, 0x3910 + i,
			       ((coeff[2 * i] & 0x1fff) << 16) | (coeff[2 * i + 1] & 0x1fff));
	meson_t7_write(hw, 0x3914, coeff[8] & 0x1fff);
	meson_t7_write(hw, 0x3918, 0);
	meson_t7_write(hw, 0x3919, (64 << 16) | 512);
	meson_t7_write(hw, 0x391a, 512);
	meson_t7_write(hw, 0x391d, 1);
}

int meson_t7_modeset_enable(struct meson_t7_modeset *hw,
			   const struct drm_display_mode *mode)
{
	struct meson_t7_clock clock;
	struct arm_smccc_res res;
	u32 size = mode->vdisplay << 16 | mode->hdisplay;
	int ret;

	ret = meson_t7_clock_calculate(mode->clock, &clock);
	if (ret)
		return ret;
	meson_t7_modeset_disable(hw);
	ret = meson_t7_pll_set(hw, &clock, mode->clock > 340000);
	if (ret)
		return ret;
	meson_t7_video_clock(hw);
	/* T7 VIM4 routes HDMI through ENC2, not the generic driver's ENC0. */
	arm_smccc_smc(0x820000ab, 9, 1, 1, 0, 0, 0, 0, &res);
	meson_t7_venc_set(hw, mode);
	meson_t7_write(hw, 0x1cef, 1);
	meson_t7_write(hw, VPU_HDMI_SETTING, BIT(1) |
			((mode->flags & DRM_MODE_FLAG_PHSYNC) ? BIT(2) : 0) |
			((mode->flags & DRM_MODE_FLAG_PVSYNC) ? BIT(3) : 0) | (3 << 16));
	/* VENC YCbCr -> formatter CrYCb; TX expects limited YUV444, 8-bit. */
	meson_t7_write(hw, 0x2743, (2 << 22) | BIT(10) | (2 << 2));
	meson_t7_osd_matrix(hw);
	/* Single unscaled OSD1, all size encodings match the vendor VPP path. */
	meson_t7_write(hw, 0x5a08, 0);
	meson_t7_write(hw, VIU_OSD_BLEND_DIN0_SCOPE_H, (mode->hdisplay - 1) << 16);
	meson_t7_write(hw, VIU_OSD_BLEND_DIN0_SCOPE_V, (mode->vdisplay - 1) << 16);
	meson_t7_write(hw, VIU_OSD_BLEND_BLEND0_SIZE, size);
	meson_t7_write(hw, VIU_OSD_BLEND_BLEND1_SIZE, size);
	meson_t7_write(hw, VPP_OSD1_IN_SIZE, size);
	meson_t7_write(hw, VPP_OSD1_BLD_H_SCOPE, mode->hdisplay - 1);
	meson_t7_write(hw, VPP_OSD1_BLD_V_SCOPE, mode->vdisplay - 1);
	meson_t7_write(hw, VPP_POSTBLEND_H_SIZE, size);
	meson_t7_write(hw, 0x1da5, mode->hdisplay << 16 | mode->vdisplay);
	meson_t7_write(hw, ENCP_VIDEO_EN, 1);
	return 0;
}

struct meson_t7_modeset *meson_t7_modeset_init(struct platform_device *pdev)
{
	static const struct {
		const char *name;
		u32 first, last;
	} windows[] = {
		{ "venc", VENC_INTCTRL, 0x1cef },
		{ "vpp", VPP_OSD1_IN_SIZE, VPP_OSD1_BLD_V_SCOPE },
		{ "blend", VIU_OSD_BLEND_CTRL, VIU_OSD_BLEND_CTRL1 },
		{ "scaler", 0x5a08, 0x5a08 },
		{ "post-size", VPP_POSTBLEND_H_SIZE, VPP_POSTBLEND_H_SIZE },
		{ "out-size", 0x1da5, 0x1da5 },
		{ "hdmi-setting", VPU_HDMI_SETTING, VPU_HDMI_SETTING },
		{ "hdmi-format", 0x2743, 0x2743 },
		{ "osd-matrix", 0x3910, 0x391d },
	};
	struct meson_t7_modeset *hw;
	struct platform_device *clock_device;
	struct device_node *node;
	unsigned int i;
	static const struct {
		const char *name;
		u32 start, size;
	} clock_regions[] = {
		{ "pll", 0xfe0081c0, 0x20 },
		{ "vid2", 0xfe0001e0, 0x20 },
		{ "vid-div", 0xfe000204, 4 },
	};

	for (i = 0; i < ARRAY_SIZE(clock_regions); i++) {
		struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
								   clock_regions[i].name);

		if (!res || res->start != clock_regions[i].start ||
		    resource_size(res) != clock_regions[i].size)
			return ERR_PTR(-EINVAL);
	}
	hw = devm_kzalloc(&pdev->dev, sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return ERR_PTR(-ENOMEM);
	node = of_parse_phandle(pdev->dev.of_node, "amlogic,clock-controller", 0);
	if (!node || !of_device_is_compatible(node, "amlogic,t7-peripherals-clkc")) {
		of_node_put(node);
		return ERR_PTR(-EINVAL);
	}
	clock_device = of_find_device_by_node(node);
	of_node_put(node);
	if (!clock_device)
		return ERR_PTR(-EPROBE_DEFER);
	hw->clkctrl = dev_get_regmap(&clock_device->dev, NULL);
	if (hw->clkctrl && !device_link_add(&pdev->dev, &clock_device->dev,
					    DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&clock_device->dev);
		return ERR_PTR(-ENOMEM);
	}
	put_device(&clock_device->dev);
	if (!hw->clkctrl)
		return ERR_PTR(-EPROBE_DEFER);
	hw->pll = devm_platform_ioremap_resource_byname(pdev, "pll");
	if (IS_ERR(hw->pll))
		return ERR_CAST(hw->pll);
	hw->vid2 = devm_platform_ioremap_resource_byname(pdev, "vid2");
	if (IS_ERR(hw->vid2))
		return ERR_CAST(hw->vid2);
	hw->vid_div = devm_platform_ioremap_resource_byname(pdev, "vid-div");
	if (IS_ERR(hw->vid_div))
		return ERR_CAST(hw->vid_div);
	for (i = 0; i < ARRAY_SIZE(windows); i++) {
		struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
								   windows[i].name);
		u32 offset = i ? 0 : 0x800;

		if (!res || res->start != 0xff000000 + (windows[i].first + offset) * 4 ||
		    resource_size(res) != (windows[i].last - windows[i].first + 1) * 4)
			return ERR_PTR(-EINVAL);
		hw->vcbus[i].first = windows[i].first;
		hw->vcbus[i].last = windows[i].last;
		/* VENC is already claimed by the display's IRQ/readback path. */
		if (!i) {
			hw->vcbus[i].base = devm_ioremap(&pdev->dev, res->start,
							 resource_size(res));
			if (!hw->vcbus[i].base)
				return ERR_PTR(-ENOMEM);
		} else {
			hw->vcbus[i].base = devm_platform_ioremap_resource_byname(pdev,
									      windows[i].name);
			if (IS_ERR(hw->vcbus[i].base))
				return ERR_CAST(hw->vcbus[i].base);
		}
	}
	return hw;
}
