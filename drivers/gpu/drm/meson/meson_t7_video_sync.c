// SPDX-License-Identifier: GPL-2.0-only
/* Reservation readers protect scanout beyond the new frame's latch event. */
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane.h>

#include "meson_t7_video_sync.h"

struct meson_t7_video_plane_state {
	struct drm_plane_state base;
	struct dma_fence *reader;
	bool submitted;
};

struct meson_t7_video_fence {
	struct dma_fence base;
	spinlock_t lock; /* Fence signal state and callback list. */
};

static struct meson_t7_video_plane_state *video_state(struct drm_plane_state *state)
{
	return container_of(state, struct meson_t7_video_plane_state, base);
}

static const char *video_fence_driver_name(struct dma_fence *fence)
{
	return "meson-t7";
}

static const char *video_fence_timeline_name(struct dma_fence *fence)
{
	return "video scanout";
}

static const struct dma_fence_ops video_fence_ops = {
	.get_driver_name = video_fence_driver_name,
	.get_timeline_name = video_fence_timeline_name,
	.release = dma_fence_free,
};

static struct dma_fence *video_fence_create(void)
{
	struct meson_t7_video_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return NULL;
	spin_lock_init(&fence->lock);
	/* A cancelled prepare must never replace an older active reader in the
	 * same reservation. Give each prepared reader an independent context.
	 */
	dma_fence_init(&fence->base, &video_fence_ops, &fence->lock,
		       dma_fence_context_alloc(1), 1);
	return &fence->base;
}

void meson_t7_video_cleanup_fb(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct meson_t7_video_plane_state *video = video_state(state);

	if (!video->reader)
		return;
	/* A submitted reader belongs to the hardware owner, including on faults. */
	if (!video->submitted)
		dma_fence_signal(video->reader);
	dma_fence_put(video->reader);
	video->reader = NULL;
	video->submitted = false;
}

void meson_t7_video_plane_destroy_state(struct drm_plane *plane, struct drm_plane_state *state)
{
	meson_t7_video_cleanup_fb(plane, state);
	__drm_atomic_helper_plane_destroy_state(state);
	kfree(video_state(state));
}

void meson_t7_video_plane_reset(struct drm_plane *plane)
{
	struct meson_t7_video_plane_state *state;

	if (plane->state)
		meson_t7_video_plane_destroy_state(plane, plane->state);
	plane->state = NULL;
	state = kzalloc_obj(*state);
	if (state)
		__drm_atomic_helper_plane_reset(plane, &state->base);
}

struct drm_plane_state *meson_t7_video_plane_duplicate_state(struct drm_plane *plane)
{
	struct meson_t7_video_plane_state *state;

	if (!plane->state)
		return NULL;
	state = kzalloc_obj(*state);
	if (!state)
		return NULL;
	__drm_atomic_helper_plane_duplicate_state(plane, &state->base);
	return &state->base;
}

int meson_t7_video_prepare_fb(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct meson_t7_video_plane_state *video = video_state(state);
	struct dma_fence *reader;
	unsigned int i, j;
	int ret;

	if (video->reader)
		return -EINVAL;
	/* Preserve normal implicit producer dependencies and IN_FENCE_FD. */
	ret = drm_gem_plane_helper_prepare_fb(plane, state);
	if (ret || !state->fb || !state->visible || !state->crtc)
		return ret;
	reader = video_fence_create();
	if (!reader)
		return -ENOMEM;
	video->reader = reader;
	for (i = 0; i < state->fb->format->num_planes; i++) {
		struct drm_gem_object *object = drm_gem_fb_get_obj(state->fb, i);

		if (!object || !object->resv) {
			ret = -EINVAL;
			goto fail;
		}
		/* NV12 can use one object twice, or distinct objects sharing a resv. */
		for (j = 0; j < i; j++)
			if (drm_gem_fb_get_obj(state->fb, j)->resv == object->resv)
				break;
		if (j < i)
			continue;
		ret = dma_resv_lock(object->resv, NULL);
		if (ret)
			goto fail;
		ret = dma_resv_reserve_fences(object->resv, 1);
		if (!ret)
			dma_resv_add_fence(object->resv, reader, DMA_RESV_USAGE_READ);
		dma_resv_unlock(object->resv);
		if (ret)
			goto fail;
	}
	return 0;

fail:
	meson_t7_video_cleanup_fb(plane, state);
	return ret;
}

struct dma_fence *meson_t7_video_claim_reader(struct drm_plane_state *state)
{
	struct meson_t7_video_plane_state *video;

	if (!state || !state->fb || !state->visible || !state->crtc)
		return NULL;
	video = video_state(state);
	if (video->reader)
		video->submitted = true;
	return video->reader;
}
