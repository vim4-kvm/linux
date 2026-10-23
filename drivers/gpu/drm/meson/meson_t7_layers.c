// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019 Amlogic, Inc. All rights reserved. */
/* T7 OSD MIF, scalers and mixer. All changes share one hardware RDMA latch. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>

#include "meson_registers.h"
#include "meson_t7_layers.h"

/* Vendor meson_osd_scaler.c: 33 phases, signed 8-bit four-tap coefficients. */
static const u32 t7_bicubic[] = {
	0x00800000, 0x007f0100, 0xff7f0200, 0xfe7f0300, 0xfd7e0500, 0xfc7e0600,
	0xfb7d0800, 0xfb7c0900, 0xfa7b0b00, 0xfa7a0dff, 0xf9790fff, 0xf97711ff,
	0xf87613ff, 0xf87416fe, 0xf87218fe, 0xf8701afe, 0xf76f1dfd, 0xf76d1ffd,
	0xf76b21fd, 0xf76824fd, 0xf76627fc, 0xf76429fc, 0xf7612cfc, 0xf75f2ffb,
	0xf75d31fb, 0xf75a34fb, 0xf75837fa, 0xf7553afa, 0xf8523cfa, 0xf8503ff9,
	0xf84d42f9, 0xf84a45f9, 0xf84848f8,
};

static const u32 t7_bilinear2[] = {
	0x80000000, 0x7e020000, 0x7c040000, 0x7a060000, 0x78080000, 0x760a0000,
	0x740c0000, 0x720e0000, 0x70100000, 0x6e120000, 0x6c140000, 0x6a160000,
	0x68180000, 0x661a0000, 0x641c0000, 0x621e0000, 0x60200000, 0x5e220000,
	0x5c240000, 0x5a260000, 0x58280000, 0x562a0000, 0x542c0000, 0x522e0000,
	0x50300000, 0x4e320000, 0x4c340000, 0x4a360000, 0x48380000, 0x463a0000,
	0x443c0000, 0x423e0000, 0x40400000,
};

static const u32 t7_triangle4[] = {
	0x20402000, 0x20402000, 0x1f3f2101, 0x1f3f2101, 0x1e3e2202, 0x1e3e2202,
	0x1d3d2303, 0x1d3d2303, 0x1c3c2404, 0x1c3c2404, 0x1b3b2505, 0x1b3b2505,
	0x1a3a2606, 0x1a3a2606, 0x19392707, 0x19392707, 0x18382808, 0x18382808,
	0x17372909, 0x17372909, 0x16362a0a, 0x16362a0a, 0x15352b0b, 0x15352b0b,
	0x14342c0c, 0x14342c0c, 0x13332d0d, 0x13332d0d, 0x12322e0e, 0x12322e0e,
	0x11312f0f, 0x11312f0f, 0x10303010,
};

struct meson_t7_rdma_builder {
	u32 *table;
	unsigned int used, capacity;
};

static void meson_t7_layer_write(struct meson_t7_rdma_builder *rdma, u32 reg, u32 value)
{
	if (rdma->used < rdma->capacity) {
		rdma->table[rdma->used * 2] = reg;
		rdma->table[rdma->used * 2 + 1] = value;
	}
	rdma->used++;
}

static void meson_t7_layer_scale(struct meson_t7_rdma_builder *rdma,
				 const struct meson_t7_osd_layer *layer, u32 base)
{
	bool double_line = layer->src_w > 1920;
	bool scaled = layer->src_w != layer->width || layer->src_h != layer->height;
	unsigned int bank = double_line ? 2 : 4;
	const u32 *horizontal = t7_bicubic, *vertical = t7_bicubic;
	unsigned int i;
	u32 vstep, hstep;

	/* Match the vendor's progressive P2P phase and integer truncation order. */
	vstep = div_u64((u64)layer->src_h << 20, layer->height) << 4;
	hstep = div_u64((u64)layer->src_w << 18, layer->width) << 6;
	if (double_line) {
		vertical = t7_bilinear2;
	} else if (layer->width <= 720) {
		if (layer->src_w != layer->width)
			horizontal = t7_triangle4;
		if (layer->src_h != layer->height)
			vertical = t7_triangle4;
	}
	meson_t7_layer_write(rdma, base, vstep);
	meson_t7_layer_write(rdma, base + 1, 0);
	meson_t7_layer_write(rdma, base + 2, BIT(25) | (scaled ? BIT(24) : 0) |
			     (double_line ? BIT(21) : BIT(8)) | bank << 3 | bank);
	meson_t7_layer_write(rdma, base + 3, hstep);
	meson_t7_layer_write(rdma, base + 4, 0);
	meson_t7_layer_write(rdma, base + 5, (scaled ? BIT(22) : 0) | BIT(8) | 4 << 3 | 4);
	meson_t7_layer_write(rdma, base + 7, 0x80808080);
	meson_t7_layer_write(rdma, base + 9, (layer->src_w - 1) << 16 | (layer->src_h - 1));
	meson_t7_layer_write(rdma, base + 10, layer->width - 1);
	meson_t7_layer_write(rdma, base + 11, layer->height - 1);
	meson_t7_layer_write(rdma, base + 12, BIT(8));
	for (i = 0; i < ARRAY_SIZE(t7_bicubic); i++)
		meson_t7_layer_write(rdma, base + 13, horizontal[i]);
	meson_t7_layer_write(rdma, base + 12, 0);
	for (i = 0; i < ARRAY_SIZE(t7_bicubic); i++)
		meson_t7_layer_write(rdma, base + 13, vertical[i]);
	meson_t7_layer_write(rdma, base + 8, scaled ? BIT(3) | BIT(2) : 0);
}

static int meson_t7_layer_mif(struct meson_t7_rdma_builder *rdma,
			      const struct meson_t7_osd_layer *layer, unsigned int index)
{
	static const struct {
		u32 ctrl, ctrl2, w0, w1, w2, w3, w4, address, stride, fifo, unpack, dimm;
	} regs[] = {
		{ 0x1a10, 0x1a2d, 0x1a1b, 0x1a1c, 0x1a1d, 0x1a1e, 0x1a13,
		  0x1a14, 0x1a15, 0x1a2b, 0x1a2f, 0x1adf },
		{ 0x1a30, 0x1a4d, 0x1a3b, 0x1a3c, 0x1a3d, 0x1a3e, 0x1a64,
		  0x1a65, 0x1a66, 0x1a4b, 0x1abd, 0x1acf },
		{ 0x3d80, 0x3d81, 0x3d88, 0x3d8c, 0x3d90, 0x3d94, 0x3d98,
		  0x3d99, 0x3d9a, 0x3d9c, 0x3d9f, 0x3da0 },
	};
	u32 format, alpha = layer->alpha >> 8;
	bool replace = layer->blend == DRM_MODE_BLEND_PIXEL_NONE;

	/* Vendor initialization disables the constant-color substitute on every OSD. */
	meson_t7_layer_write(rdma, regs[index].dimm, 0);
	if (!layer->enabled) {
		meson_t7_layer_write(rdma, regs[index].ctrl, BIT(2));
		return 0;
	}
	switch (layer->format) {
	case DRM_FORMAT_RGB888:
		format = 0x8700;
		replace = true;
		break;
	case DRM_FORMAT_XRGB8888:
		replace = true;
		fallthrough;
	case DRM_FORMAT_ARGB8888:
		format = 0x8504;
		break;
	case DRM_FORMAT_XBGR8888:
		replace = true;
		fallthrough;
	case DRM_FORMAT_ABGR8888:
		format = 0x8508;
		break;
	default:
		return -EINVAL;
	}
	if (layer->rotation & DRM_MODE_REFLECT_X)
		format |= BIT(28);
	if (layer->rotation & DRM_MODE_REFLECT_Y)
		format |= BIT(29);
	if (alpha == 0xff)
		alpha = 0x100;
	meson_t7_layer_write(rdma, regs[index].w0, format);
	meson_t7_layer_write(rdma, regs[index].w1,
			     (layer->src_x + layer->src_w - 1) << 16 | layer->src_x);
	meson_t7_layer_write(rdma, regs[index].w2,
			     (layer->src_y + layer->src_h - 1) << 16 | layer->src_y);
	meson_t7_layer_write(rdma, regs[index].w3, (layer->src_w - 1) << 16);
	meson_t7_layer_write(rdma, regs[index].w4, (layer->src_h - 1) << 16);
	meson_t7_layer_write(rdma, regs[index].address, layer->address >> 4);
	meson_t7_layer_write(rdma, regs[index].stride, layer->pitch >> 4);
	meson_t7_layer_write(rdma, regs[index].ctrl2, 0xff << 6 | (replace ? BIT(14) : 0));
	meson_t7_layer_write(rdma, regs[index].unpack, 0);
	/* Vendor FIFO hold lines, burst length, depth and limits; no global arbiter writes. */
	meson_t7_layer_write(rdma, regs[index].fifo,
			     BIT(31) | 2 << 24 | 2 << 22 | 0x20 << 12 | BIT(10) | 8 << 5 | BIT(0));
	meson_t7_layer_scale(rdma, layer, 0x5a00 + index * 0x40);
	meson_t7_layer_write(rdma, regs[index].ctrl, alpha << 12 | BIT(2) | BIT(0));
	return 0;
}

int meson_t7_osd_layers_build(u32 *table, unsigned int pairs,
			      const struct meson_t7_osd_layer *layers,
			      unsigned int width, unsigned int height,
			      u32 path_start, u32 path_misc)
{
	struct meson_t7_rdma_builder rdma = { .table = table, .capacity = pairs };
	unsigned int i, j, count = 0, order[MESON_T7_OSD_LAYERS];
	u32 blend = 0x4444 | 3 << 27 | 4U << 29 | 5 << 20;
	int ret;

	for (i = 0; i < MESON_T7_OSD_LAYERS; i++) {
		const struct meson_t7_osd_layer *layer = &layers[i];

		if (layer->enabled) {
			if (!layer->src_w || !layer->src_h || !layer->width || !layer->height)
				return -EINVAL;
			order[count++] = i;
		}
		ret = meson_t7_layer_mif(&rdma, layer, i);
		if (ret)
			return ret;
		path_misc &= ~(0xfU << (i * 4 + 16));
		path_misc |= (i + 1) << (i * 4 + 16);
	}
	/* Ascending zpos is the vendor DIN ordering; equal zpos uses normalized plane ID. */
	for (i = 0; i < count; i++)
		for (j = i + 1; j < count; j++)
			if (layers[order[j]].zpos < layers[order[i]].zpos)
				swap(order[i], order[j]);
	for (i = 0; i < count; i++) {
		const struct meson_t7_osd_layer *layer = &layers[order[i]];

		blend &= ~(0xfU << (i * 4));
		blend |= (order[i] + 1) << (i * 4) | BIT(20 + i);
		meson_t7_layer_write(&rdma, VIU_OSD_BLEND_DIN0_SCOPE_H + i * 2,
				     (layer->x + layer->width - 1) << 16 | layer->x);
		meson_t7_layer_write(&rdma, VIU_OSD_BLEND_DIN0_SCOPE_V + i * 2,
				     (layer->y + layer->height - 1) << 16 | layer->y);
	}
	/* Vendor T7 keeps DIN0/2 clocks enabled and clips unused dummy inputs away. */
	for (i = count; i < 4; i++) {
		meson_t7_layer_write(&rdma, VIU_OSD_BLEND_DIN0_SCOPE_H + i * 2, 0xffffffff);
		meson_t7_layer_write(&rdma, VIU_OSD_BLEND_DIN0_SCOPE_V + i * 2, 0x043a0439);
	}
	meson_t7_layer_write(&rdma, VIU_OSD_BLEND_DUMMY_DATA0, 0);
	meson_t7_layer_write(&rdma, VIU_OSD_BLEND_DUMMY_ALPHA, 0);
	meson_t7_layer_write(&rdma, VIU_OSD_BLEND_BLEND0_SIZE, height << 16 | width);
	meson_t7_layer_write(&rdma, VIU_OSD_BLEND_BLEND1_SIZE, height << 16 | width);
	meson_t7_layer_write(&rdma, VIU_OSD_BLEND_CTRL1, 0x31);
	meson_t7_layer_write(&rdma, VIU_OSD_BLEND_CTRL, blend);
	/* OSD3 routes to VPP0; preserve video, VIU2 and unrelated path fields. */
	meson_t7_layer_write(&rdma, 0x1a0d, path_start & ~GENMASK(25, 24));
	meson_t7_layer_write(&rdma, OSD_PATH_MISC_CTRL, path_misc);
	if (rdma.used > rdma.capacity)
		return -ENOSPC;
	return rdma.used * 8;
}
