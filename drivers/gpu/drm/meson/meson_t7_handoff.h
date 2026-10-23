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

#endif
