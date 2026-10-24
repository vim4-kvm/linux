/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_VIDEO_KMS_H
#define __MESON_T7_VIDEO_KMS_H

#include <linux/types.h>

struct drm_atomic_commit;
struct drm_device;
struct drm_framebuffer;
struct platform_device;
struct meson_t7_osd_layer;
struct meson_t7_video_kms;

/* Both complete PPS/MIF images and the common blend transaction. */
#define MESON_T7_VIDEO_KMS_PAIRS 320

struct meson_t7_video_kms *meson_t7_video_map(struct platform_device *pdev);
int meson_t7_video_planes_init(struct meson_t7_video_kms *video, struct drm_device *drm);
int meson_t7_video_collect(struct meson_t7_video_kms *video, struct drm_atomic_commit *commit,
			   bool blank, struct drm_framebuffer **frames);
bool meson_t7_video_in_use(struct meson_t7_video_kms *video, struct drm_atomic_commit *commit);
int meson_t7_video_flip(struct meson_t7_video_kms *video, struct drm_atomic_commit *commit,
			bool blank, u32 *table, unsigned int pairs);
int meson_t7_video_build(struct meson_t7_video_kms *video, struct drm_atomic_commit *commit,
			 bool blank, bool enable, const struct meson_t7_osd_layer *osd,
			 u32 *table, unsigned int pairs);
int meson_t7_video_zpos_check(struct meson_t7_video_kms *video,
			      struct drm_atomic_commit *commit);

#endif
