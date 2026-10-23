/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_HANDOFF_H
#define __MESON_T7_HANDOFF_H

#include <drm/drm_modes.h>

/* Firmware contract, not an EDID-derived mode or a programmable clock. */
static const struct drm_display_mode meson_t7_handoff_mode = {
	DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
		 148500, 1920, 2008, 2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC)
};

static inline enum drm_mode_status
meson_t7_handoff_mode_valid(const struct drm_display_mode *mode)
{
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;
	if (mode->hdisplay != meson_t7_handoff_mode.hdisplay)
		return MODE_ONE_WIDTH;
	if (mode->vdisplay != meson_t7_handoff_mode.vdisplay)
		return MODE_ONE_HEIGHT;
	if (mode->clock != meson_t7_handoff_mode.clock)
		return MODE_NOCLOCK;
	if (!drm_mode_equal(mode, &meson_t7_handoff_mode))
		return MODE_BAD;
	return MODE_OK;
}

#endif
