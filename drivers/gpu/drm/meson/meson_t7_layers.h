/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_LAYERS_H
#define __MESON_T7_LAYERS_H

#include <linux/types.h>

#define MESON_T7_OSD_LAYERS 3
#define MESON_T7_RDMA_PAIRS 512

struct meson_t7_osd_layer {
	bool enabled;
	u32 format, pitch, address;
	u32 src_x, src_y, src_w, src_h;
	u32 x, y, width, height;
	u32 zpos, rotation, blend;
	u16 alpha;
};

/* Return descriptor bytes, or an error without starting register DMA. */
int meson_t7_osd_layers_build(u32 *table, unsigned int pairs,
			      const struct meson_t7_osd_layer *layers,
			      unsigned int width, unsigned int height,
			      u32 path_start, u32 path_misc);

/* Return address-only descriptor bytes, zero if a full transaction is needed. */
int meson_t7_osd_layers_flip(u32 *table, unsigned int pairs,
			     const struct meson_t7_osd_layer *next,
			     const struct meson_t7_osd_layer *old);

#endif
