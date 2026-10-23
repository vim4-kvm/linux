/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_SCANOUT_H
#define __MESON_T7_SCANOUT_H

#include <linux/errno.h>
#include <drm/drm_framebuffer.h>

/* Three OSD inputs and two video inputs share one atomic latch boundary. */
#define MESON_T7_MAX_LAYERS 5

struct meson_t7_scanout {
	struct drm_framebuffer *active[MESON_T7_MAX_LAYERS];
	struct drm_framebuffer *pending[MESON_T7_MAX_LAYERS];
	bool faulted;
};

/* Called only by the serialized CRTC commit worker, before publishing DMA. */
static inline int meson_t7_scanout_acquire(struct meson_t7_scanout *scanout,
					   struct drm_framebuffer * const *next)
{
	unsigned int i;

	if (scanout->faulted)
		return -EIO;
	for (i = 0; i < MESON_T7_MAX_LAYERS; i++) {
		scanout->pending[i] = next[i];
		if (next[i])
			drm_framebuffer_get(next[i]);
	}
	return 0;
}

/*
 * Every enabled fetch engine must be idle or have crossed its safe boundary.
 * A failed transaction retains BOTH generations, including unchanged layers.
 * Late interrupts never unpin a failed transaction; only reboot can do that.
 */
static inline void meson_t7_scanout_retire(struct meson_t7_scanout *scanout, bool safe)
{
	unsigned int i;

	if (scanout->faulted)
		return;
	if (!safe) {
		scanout->faulted = true;
		return;
	}
	for (i = 0; i < MESON_T7_MAX_LAYERS; i++) {
		if (scanout->active[i])
			drm_framebuffer_put(scanout->active[i]);
		scanout->active[i] = scanout->pending[i];
		scanout->pending[i] = NULL;
	}
}

#endif
