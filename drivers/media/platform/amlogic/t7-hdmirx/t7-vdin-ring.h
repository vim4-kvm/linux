/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T7_VDIN_RING_H
#define T7_VDIN_RING_H

#include <linux/types.h>

#define VDIN_DMA_SLOTS		3

struct t7_vdin_buffer;

enum t7_vdin_slot_state {
	VDIN_SLOT_FREE,
	VDIN_SLOT_WRITING,
	VDIN_SLOT_READY,
	VDIN_SLOT_COPYING,
};

struct t7_vdin_slot {
	void *data;
	dma_addr_t dma;
	enum t7_vdin_slot_state state;
	u64 timestamp;
	u32 sequence;
	struct t7_vdin_buffer *buffer;
};

#if IS_ENABLED(CONFIG_KUNIT)
struct t7_vdin_slot *t7_vdin_take_ready(struct t7_vdin_slot slots[VDIN_DMA_SLOTS]);
#endif

#endif
