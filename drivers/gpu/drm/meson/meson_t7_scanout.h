/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_SCANOUT_H
#define __MESON_T7_SCANOUT_H

#include <linux/dma-fence.h>
#include <linux/errno.h>
#include <drm/drm_framebuffer.h>

/* Three OSD inputs and two video inputs share one atomic latch boundary. */
#define MESON_T7_MAX_LAYERS 5

struct meson_t7_scanout {
	struct drm_framebuffer *active[MESON_T7_MAX_LAYERS];
	struct drm_framebuffer *pending[MESON_T7_MAX_LAYERS];
	struct drm_framebuffer *retiring[MESON_T7_MAX_LAYERS];
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
 * A failed transaction retains all generations, including unchanged layers.
 * Late interrupts never unpin a failed transaction; only reboot can do that.
 */
static inline void meson_t7_scanout_advance(struct meson_t7_scanout *scanout,
					    bool safe, bool defer)
{
	unsigned int i;

	if (scanout->faulted)
		return;
	if (!safe) {
		scanout->faulted = true;
		return;
	}
	for (i = 0; i < MESON_T7_MAX_LAYERS; i++) {
		if (scanout->retiring[i])
			drm_framebuffer_put(scanout->retiring[i]);
		scanout->retiring[i] = NULL;
		if (defer)
			scanout->retiring[i] = scanout->active[i];
		else if (scanout->active[i])
			drm_framebuffer_put(scanout->active[i]);
		scanout->active[i] = scanout->pending[i];
		scanout->pending[i] = NULL;
	}
}

static inline void meson_t7_scanout_retire(struct meson_t7_scanout *scanout, bool safe)
{
	meson_t7_scanout_advance(scanout, safe, false);
}

/* The active generation stays scanned while a completed long update drains. */
static inline void meson_t7_scanout_release_retiring(struct meson_t7_scanout *scanout)
{
	unsigned int i;

	if (scanout->faulted)
		return;
	for (i = 0; i < MESON_T7_MAX_LAYERS; i++) {
		if (scanout->retiring[i])
			drm_framebuffer_put(scanout->retiring[i]);
		scanout->retiring[i] = NULL;
	}
}

/* Register-lock protected. Only the commit worker touches framebuffer refs;
 * vsync may signal these driver-owned, IRQ-safe reader fences independently.
 * Each plane has its own reader even when allocations alias across planes.
 */
struct meson_t7_scanout_readers {
	struct dma_fence *active[MESON_T7_MAX_LAYERS];
	struct dma_fence *pending[MESON_T7_MAX_LAYERS];
	struct dma_fence *retiring[MESON_T7_MAX_LAYERS];
	u32 retire_vsync;
	bool waiting, faulted;
};

static inline int meson_t7_scanout_readers_acquire(struct meson_t7_scanout_readers *readers,
						   struct dma_fence * const *next)
{
	unsigned int i;

	if (readers->faulted)
		return -EIO;
	for (i = 0; i < MESON_T7_MAX_LAYERS; i++)
		if (readers->pending[i])
			return -EBUSY;
	for (i = 0; i < MESON_T7_MAX_LAYERS; i++)
		readers->pending[i] = dma_fence_get(next[i]);
	return 0;
}

static inline void meson_t7_scanout_readers_vsync(struct meson_t7_scanout_readers *readers,
						  u32 vsync)
{
	unsigned int i;

	if (readers->faulted || !readers->waiting || vsync == readers->retire_vsync)
		return;
	/* At least one complete scan after replacement, including counter wrap. */
	for (i = 0; i < MESON_T7_MAX_LAYERS; i++) {
		if (readers->retiring[i])
			dma_fence_signal(readers->retiring[i]);
		dma_fence_put(readers->retiring[i]);
		readers->retiring[i] = NULL;
	}
	readers->waiting = false;
}

static inline bool meson_t7_scanout_readers_ready(struct meson_t7_scanout_readers *readers,
						  u32 vsync)
{
	if (readers->faulted)
		return false;
	meson_t7_scanout_readers_vsync(readers, vsync);
	return !readers->waiting;
}

/* Called only after the new addresses have latched. A second latch in the
 * same scan cannot retire the previous generation: fail closed and pin it.
 */
static inline bool meson_t7_scanout_readers_advance(struct meson_t7_scanout_readers *readers,
						    u32 vsync)
{
	unsigned int i;

	if (!meson_t7_scanout_readers_ready(readers, vsync)) {
		readers->faulted = true;
		return false;
	}
	for (i = 0; i < MESON_T7_MAX_LAYERS; i++) {
		/* Two-phase reconfiguration can keep the same submitted reader. */
		if (readers->active[i] == readers->pending[i]) {
			dma_fence_put(readers->pending[i]);
		} else {
			readers->retiring[i] = readers->active[i];
			readers->active[i] = readers->pending[i];
			if (readers->retiring[i])
				readers->waiting = true;
		}
		readers->pending[i] = NULL;
	}
	readers->retire_vsync = vsync;
	return true;
}

#endif
