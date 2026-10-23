/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_MODESET_H
#define __MESON_T7_MODESET_H

#include <drm/drm_modes.h>

struct platform_device;
struct meson_t7_modeset;
struct drm_crtc;

struct meson_t7_clock {
	u32 vco, m, frac;
	u8 od1, od2;
	bool fractional;
};

int meson_t7_clock_calculate(unsigned int pixel_khz, struct meson_t7_clock *clock);
enum drm_mode_status meson_t7_mode_valid(const struct drm_display_mode *mode);
struct meson_t7_modeset *meson_t7_modeset_init(struct platform_device *pdev);
int meson_t7_modeset_enable(struct meson_t7_modeset *hw,
			   const struct drm_display_mode *mode);
void meson_t7_modeset_disable(struct meson_t7_modeset *hw);
bool meson_t7_display_ready(struct drm_crtc *crtc);

#endif
