/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_FB_H
#define __MESON_T7_FB_H

#include <linux/errno.h>
#include <linux/kernel.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>

/*
 * Linear MIFs fetch complete aligned rows, including the final row's padding.
 * DRM's generic framebuffer size check only requires the final visible pixels.
 * Validate each imported object separately, using its negotiated plane offset;
 * allocation tailroom is not a substitute for a chroma-plane offset.
 */
static inline int meson_t7_fb_check(struct drm_framebuffer *fb)
{
	unsigned int i;

	if (!fb || !fb->width || !fb->height || fb->width > 3840 || fb->height > 2160 ||
	    fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return -EINVAL;
	for (i = 0; i < fb->format->num_planes; i++) {
		struct drm_gem_dma_object *object = drm_fb_dma_get_gem_obj(fb, i);
		u32 width = drm_format_info_plane_width(fb->format, fb->width, i);
		u32 height = drm_format_info_plane_height(fb->format, fb->height, i);
		u64 size = (u64)fb->pitches[i] * height;
		u64 offset = fb->offsets[i], address;

		if (!object || !fb->format->cpp[i] || !IS_ALIGNED(fb->pitches[i], 32) ||
		    fb->pitches[i] > 0xfff * 16 ||
		    fb->pitches[i] < width * fb->format->cpp[i])
			return -EINVAL;
		if (offset > object->base.size || size > object->base.size - offset ||
		    object->dma_addr > U32_MAX || offset > U32_MAX - object->dma_addr)
			return -ERANGE;
		address = object->dma_addr + offset;
		if (!IS_ALIGNED(address, 16) || size > (u64)U32_MAX + 1 - address)
			return -ERANGE;
	}
	return 0;
}

#endif
