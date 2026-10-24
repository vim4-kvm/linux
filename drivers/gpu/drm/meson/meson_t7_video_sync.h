/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_VIDEO_SYNC_H
#define __MESON_T7_VIDEO_SYNC_H

struct dma_fence;
struct drm_plane;
struct drm_plane_state;

void meson_t7_video_plane_reset(struct drm_plane *plane);
struct drm_plane_state *meson_t7_video_plane_duplicate_state(struct drm_plane *plane);
void meson_t7_video_plane_destroy_state(struct drm_plane *plane, struct drm_plane_state *state);
int meson_t7_video_prepare_fb(struct drm_plane *plane, struct drm_plane_state *state);
void meson_t7_video_cleanup_fb(struct drm_plane *plane, struct drm_plane_state *state);

/* Borrowed until state cleanup. Scanout must acquire its own reference before
 * completing the commit, and must already own the corresponding framebuffer.
 */
struct dma_fence *meson_t7_video_claim_reader(struct drm_plane_state *state);

#endif
