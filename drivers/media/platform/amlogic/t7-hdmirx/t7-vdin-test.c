// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019 Amlogic, Inc. All rights reserved. */
/* T7 VDIN0 capture; the split-resource profile permits OSD-only handoff. */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "t7-hdmirx.h"

static bool nv12;
module_param(nv12, bool, 0400);
MODULE_PARM_DESC(nv12, "Capture BT.709 limited-range NV12 instead of RGB24");

static bool direct_dma;
module_param(direct_dma, bool, 0400);
MODULE_PARM_DESC(direct_dma, "Capture into leased coherent vb2 buffers without a CPU frame copy");

/* VCBUS word indices; the accessors alone convert them to byte offsets. */
#define VDIN_SCALE_COEF_IDX	0x1200
#define VDIN_SCALE_COEF		0x1201
#define VDIN_COM_CTRL0		0x1202
#define VDIN_ACTIVE_PIXELS	0x1203
#define VDIN_LCNT_STATUS		0x1204
#define VDIN_ASFIFO_CTRL1		0x1209
#define VDIN_WIDTHS		0x120a
#define VDIN_SC_MISC_CTRL		0x120b
#define VDIN_HSC_PHASE_STEP	0x120c
#define VDIN_HSC_INI_CTRL		0x120d
#define VDIN_MATRIX_CTRL		0x1210
#define VDIN_LFIFO_CTRL		0x121a
#define VDIN_GCLK_CTRL		0x121b
#define VDIN_INTF_WIDTH		0x121c
#define VDIN_WR_CTRL2		0x121f
#define VDIN_WR_CTRL		0x1220
#define VDIN_WR_H_WIN		0x1221
#define VDIN_WR_V_WIN		0x1222
#define VDIN_VSC_PHASE_STEP	0x1223
#define VDIN_VSC_INI_CTRL		0x1224
#define VDIN_SCIN_HEIGHT		0x1225
#define VDIN_DUMMY_DATA		0x1226
#define VDIN_WR_URGENT		0x1257
#define VDIN_WR_STATUS		0x1258
#define VDIN_GCLK_CTRL2		0x1270
#define VDIN_VSHRK_CTRL		0x1271
#define VDIN_HDR_CTRL		0x1280
#define VDIN_HDR_MATRIX_IN	0x12bb
#define VDIN_HDR_MATRIX_OUT	0x12bc
#define VDIN_WR_BADDR		0x12cc
#define VDIN_WR_CHROMA		0x12cd
#define VDIN_WR_STRIDE		0x12ce
#define VDIN_WR_CHROMA_STRIDE	0x12cf
#define VDIN_DOUBLE_CTRL		0x410b
#define VDIN_TOP_MISC		0x410d
#define VPU_SW_RESET		0x2720
#define VPU_WRARB_MODE		0x27a2
#define VPU_WRARB_REQEN		0x27a3
#define VDIN_WR_REQ		BIT(8)
#define VDIN_VCP_WR		BIT(25)
#define VDIN_DONE_CLEAR		BIT(21)
#define VDIN_DMA_SLOTS		3

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

struct t7_vdin_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	struct dma_buf *lease;
	dma_addr_t dma;
	bool hardware_used;
};

struct t7_vdin_snapshot {
	size_t size;
	unsigned char data[];
};

struct t7_vdin {
	unsigned int width, height, stride, frame_size, allocation_size, frame_capacity;
	/* Source lease geometry is independent of the downscaled DMA image. */
	unsigned int input_width, input_height;
	u32 pixel_format;
	bool nv12;
	bool direct_dma;
	struct kref ref;
	struct device *dev;
	struct device *receiver;
	struct device *domains[3];
	unsigned int domain_count;
	void __iomem *regs;
	void __iomem *split_regs[4];
	struct clk_bulk_data clocks[5];
	struct mutex lock; /* Serializes DMA capture and the immutable copy. */
	struct completion done;
	spinlock_t irq_lock; /* Protects armed/frame_irqs and request gating. */
	bool armed;
	bool faulted;
	bool dma_idle;
	unsigned int frame_irqs;
	unsigned int snapshots;
	int irq;
	void *frame;
	dma_addr_t frame_dma;
	bool memory_on;
	u64 frame_timestamp;
	struct v4l2_device v4l2;
	struct video_device video;
	struct vb2_queue queue;
	struct mutex video_lock; /* Non-queue ioctls; never acquired by the worker. */
	struct mutex queue_mutex; /* vb2 allocation, streaming and close. */
	struct delayed_work source_work;
	bool source_seen, source_locked;
	unsigned int source_width, source_height;
	unsigned int source_period;
	spinlock_t queue_lock;
	struct list_head queued;
	wait_queue_head_t buffer_wait;
	struct task_struct *thread;
	bool streaming; /* Protected by lock, including debugfs exclusion. */
	bool receiver_lease;
	/* Private cached DMA ring; all ownership transitions use irq_lock. */
	struct t7_vdin_slot slots[VDIN_DMA_SLOTS];
	bool ring_mode;
	bool ring_running;
	bool ring_stopping;
	bool ring_discard;
	int ring_error;
	unsigned int active_slot;
	u32 ring_sequence;
	u32 ring_pauses;
	u64 copied_bytes;
	/* Last failed worker remains visible across close and the next STREAMON. */
	u32 stream_stops, last_stop_sequence, source_changes;
	u64 invalid_frames;
	int last_stop_error;
	const char *last_stop_reason;
	u32 hdr_gain[149], hdr_oetf[149];
	bool hdr_curve_pending;
};

static void __iomem *t7_vdin_reg(struct t7_vdin *vdin, u32 reg)
{
	if (vdin->regs)
		return vdin->regs + reg * 4;
	if (reg >= 0x1200 && reg <= 0x12ff)
		return vdin->split_regs[0] + (reg - 0x1200) * 4;
	if (reg == VPU_SW_RESET)
		return vdin->split_regs[1];
	if (reg >= VPU_WRARB_MODE && reg <= VPU_WRARB_REQEN)
		return vdin->split_regs[2] + (reg - VPU_WRARB_MODE) * 4;
	if (reg >= VDIN_DOUBLE_CTRL && reg <= VDIN_TOP_MISC)
		return vdin->split_regs[3] + (reg - VDIN_DOUBLE_CTRL) * 4;
	/* Do not turn an accidental new register into an out-of-resource access. */
	WARN_ON_ONCE(1);
	return NULL;
}

static u32 t7_vdin_read(struct t7_vdin *vdin, u32 reg)
{
	void __iomem *address = t7_vdin_reg(vdin, reg);

	return address ? readl(address) : 0;
}

static void t7_vdin_write(struct t7_vdin *vdin, u32 reg, u32 value)
{
	void __iomem *address = t7_vdin_reg(vdin, reg);

	if (address)
		writel(value, address);
}

static void t7_vdin_update(struct t7_vdin *vdin, u32 reg, u32 mask, u32 value)
{
	t7_vdin_write(vdin, reg, (t7_vdin_read(vdin, reg) & ~mask) | (value & mask));
}

#include "t7-vdin-hdr.h"

static struct t7_vdin_buffer *t7_vdin_next_buffer(struct t7_vdin *vdin);

/* Caller holds irq_lock and has proved that no request targets the old slot. */
static void t7_vdin_ring_arm(struct t7_vdin *vdin, unsigned int index)
{
	struct t7_vdin_slot *slot = &vdin->slots[index];
	dma_addr_t address = slot->dma;

	if (vdin->direct_dma) {
		if (!slot->buffer)
			slot->buffer = t7_vdin_next_buffer(vdin);
		if (!slot->buffer)
			return;
		slot->buffer->hardware_used = true;
		address = slot->buffer->dma;
	}
	vdin->slots[index].state = VDIN_SLOT_WRITING;
	vdin->active_slot = index;
	t7_vdin_write(vdin, VDIN_WR_BADDR, address >> 4);
	if (vdin->nv12)
		t7_vdin_write(vdin, VDIN_WR_CHROMA,
			      (address + vdin->stride * vdin->height) >> 4);
	vdin->dma_idle = false;
	vdin->ring_running = true;
	if (vdin->hdr_curve_pending && t7_hdmirx_stream_is_hdr(vdin->receiver)) {
		/* Requests are gated at an AXI-complete frame boundary. Ignore the
		 * first frame after a LUT change if programming crosses blanking.
		 */
		t7_vdin_hdr_reload(vdin);
		vdin->ring_discard = true;
	}
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_WR_REQ | VDIN_VCP_WR,
		       VDIN_WR_REQ | VDIN_VCP_WR);
}

static int t7_vdin_ring_free_slot(struct t7_vdin *vdin)
{
	unsigned int i;

	for (i = 0; i < VDIN_DMA_SLOTS; i++)
		if (vdin->slots[i].state == VDIN_SLOT_FREE)
			return i;
	return -1;
}

/* Gate at write completion, hand off the old address, then rearm in blanking. */
static void t7_vdin_ring_done(struct t7_vdin *vdin)
{
	struct t7_vdin_slot *slot = &vdin->slots[vdin->active_slot];
	int next;

	if (!vdin->ring_running)
		return;
	vdin->frame_irqs++;
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_WR_REQ | VDIN_VCP_WR, 0);
	t7_vdin_read(vdin, VDIN_WR_CTRL);
	vdin->ring_running = false;
	vdin->dma_idle = t7_vdin_read(vdin, VDIN_WR_STATUS) & BIT(0);
	if (!vdin->dma_idle) {
		/* A late interrupt cannot transfer a potentially writable slot. */
		vdin->ring_error = -EIO;
	} else if (!vdin->ring_stopping) {
		if (vdin->ring_discard) {
			vdin->ring_discard = false;
			slot->state = VDIN_SLOT_FREE;
		} else {
			slot->timestamp = ktime_get_ns();
			slot->sequence = vdin->ring_sequence++;
			slot->state = VDIN_SLOT_READY;
		}
		next = t7_vdin_ring_free_slot(vdin);
		if (next >= 0)
			t7_vdin_ring_arm(vdin, next);
		if (!vdin->ring_running)
			vdin->ring_pauses++;
	}
	if (!vdin->ring_running)
		complete(&vdin->done);
	wake_up(&vdin->buffer_wait);
}

static irqreturn_t t7_vdin_write_done(int irq, void *data)
{
	struct t7_vdin *vdin = data;
	unsigned long flags;

	spin_lock_irqsave(&vdin->irq_lock, flags);
	/* The write-done interrupt is latched until this clear pulse. */
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, VDIN_DONE_CLEAR);
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, 0);
	if (vdin->ring_mode) {
		t7_vdin_ring_done(vdin);
		goto unlock;
	}
	if (vdin->armed && ++vdin->frame_irqs == 2) {
		/* Discard the first partial frame, then stop before another starts. */
		t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_WR_REQ | VDIN_VCP_WR, 0);
		t7_vdin_read(vdin, VDIN_WR_CTRL);
		vdin->armed = false;
		vdin->dma_idle = true;
		vdin->frame_timestamp = ktime_get_ns();
		complete(&vdin->done);
	}
unlock:
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	return IRQ_HANDLED;
}

static int t7_vdin_stop_dma(struct t7_vdin *vdin)
{
	unsigned long flags;
	u32 idle;
	int ret;

	spin_lock_irqsave(&vdin->irq_lock, flags);
	vdin->armed = false;
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_WR_REQ | VDIN_VCP_WR, 0);
	t7_vdin_read(vdin, VDIN_WR_CTRL);
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	synchronize_irq(vdin->irq);
	/* With requests gated, T7's final-AXI-response write-done proves drain.
	 * Source loss may suppress the IRQ: software completion is required to
	 * publish a frame, but must not veto hardware-confirmed DMA shutdown.
	 */
	ret = readl_poll_timeout(t7_vdin_reg(vdin, VDIN_WR_STATUS), idle,
				 idle & BIT(0),
				 10, 100000);
	if (!ret)
		vdin->dma_idle = true;
	if (ret) {
		/* Keep DMA memory, clocks and binding alive until a hardware reboot. */
		if (!vdin->faulted)
			__module_get(THIS_MODULE);
		vdin->faulted = true;
		dev_err(vdin->dev, "DMA idle timeout (%#x); retaining buffer and module\n", idle);
	}
	return ret;
}

static void t7_vdin_configure_scale(struct t7_vdin *vdin)
{
	static const u32 bicubic[] = {
		0x00800000, 0x007f0100, 0xff7f0200, 0xfe7f0300, 0xfd7e0500,
		0xfc7e0600, 0xfb7d0800, 0xfb7c0900, 0xfa7b0b00, 0xfa7a0dff,
		0xf9790fff, 0xf97711ff, 0xf87613ff, 0xf87416fe, 0xf87218fe,
		0xf8701afe, 0xf76f1dfd, 0xf76d1ffd, 0xf76b21fd, 0xf76824fd,
		0xf76627fc, 0xf76429fc, 0xf7612cfc, 0xf75f2ffb, 0xf75d31fb,
		0xf75a34fb, 0xf75837fa, 0xf7553afa, 0xf8523cfa, 0xf8503ff9,
		0xf84d42f9, 0xf84a45f9, 0xf84848f8,
	};
	unsigned int i;

	t7_vdin_write(vdin, VDIN_SC_MISC_CTRL, 0);
	t7_vdin_write(vdin, VDIN_VSC_INI_CTRL, 0);
	if (vdin->width != vdin->input_width) {
		t7_vdin_write(vdin, VDIN_SCALE_COEF_IDX, 0x100);
		for (i = 0; i < ARRAY_SIZE(bicubic); i++)
			t7_vdin_write(vdin, VDIN_SCALE_COEF, bicubic[i]);
		t7_vdin_write(vdin, VDIN_HSC_PHASE_STEP,
			      ((u64)vdin->input_width << 24) / vdin->width);
		t7_vdin_write(vdin, VDIN_HSC_INI_CTRL, BIT(29) | (4 << 24));
		t7_vdin_write(vdin, VDIN_SC_MISC_CTRL, BIT(6) | BIT(5) | BIT(4) | 4);
	}
	if (vdin->height != vdin->input_height) {
		t7_vdin_write(vdin, VDIN_VSC_PHASE_STEP,
			      ((u64)vdin->input_height << 20) / vdin->height);
		t7_vdin_write(vdin, VDIN_SCIN_HEIGHT, vdin->input_height - 1);
		t7_vdin_write(vdin, VDIN_DUMMY_DATA, 0x008080);
		t7_vdin_write(vdin, VDIN_VSC_INI_CTRL, BIT(23) | 0x8000);
	}
}

static void t7_vdin_configure(struct t7_vdin *vdin)
{
	/* HDR2 matrix-I uses the same signed coefficients/offsets as T7 VDIN.
	 * Output is always limited BT.709, regardless of the HDMI quantization.
	 */
	static const u32 matrices[][9] = {
		/* RGB full, RGB limited. */
		{ 0, 0, 0x00bb0275, 0x003f1f99, 0x1ea601c2, 0x01c21e67,
		  0x00001fd7, 0x00400200, 0x00000200 },
		{ 0x07c007c0, 0x000007c0, 0x00da02dc, 0x004a1f87, 0x1e6d020b,
		  0x020b1e25, 0x00001fd0, 0x00400200, 0x00000200 },
		/* YCbCr601 limited, YCbCr601 full, YCbCr709 full. */
		{ 0x07c00600, 0x00000600, 0x04001f8a, 0x1f2c0000, 0x04120075,
		  0x0000004d, 0x0000041a, 0x00400200, 0x00000200 },
		{ 0x00000600, 0x00000600, 0x03701f99, 0x1f460000, 0x03930066,
		  0x00000044, 0x0000039a, 0x00400200, 0x00000200 },
		{ 0x00000600, 0x00000600, 0x03700000, 0, 0x03830000,
		  0, 0x00000383, 0x00400200, 0x00000200 },
	};
	const u32 *matrix = NULL;
	bool yuv = t7_hdmirx_stream_is_yuv(vdin->receiver);
	bool full = t7_hdmirx_stream_is_full_range(vdin->receiver);
	unsigned int i;

	if (vdin->nv12) {
		if (!yuv)
			matrix = matrices[full ? 0 : 1];
		else if (t7_hdmirx_stream_is_bt601(vdin->receiver))
			matrix = matrices[full ? 3 : 2];
		else if (full)
			matrix = matrices[4];
	}
	t7_vdin_write(vdin, VDIN_WR_CTRL, 0);
	t7_vdin_update(vdin, VDIN_TOP_MISC, BIT(26) | BIT(29), BIT(26) | BIT(29));
	udelay(1);
	t7_vdin_update(vdin, VDIN_TOP_MISC, BIT(26) | BIT(29), 0);
	t7_vdin_write(vdin, VDIN_GCLK_CTRL, 0);
	t7_vdin_write(vdin, VDIN_GCLK_CTRL2, 0);
	/* The core supplies R/G/B; the packed MIF emits components 2/1/0. */
	t7_vdin_write(vdin, VDIN_COM_CTRL0, vdin->nv12 ? 0x915 : 0x195);
	/* The fixed legacy RX leaves TOP format at RGB for 4:4:4, requiring this
	 * component rotation. Dynamic mode configures TOP's native YUV mapping,
	 * which already supplies Y/Cb/Cr, as does expanded 4:2:0.
	 */
	if (t7_hdmirx_stream_needs_yuv444_swap(vdin->receiver))
		t7_vdin_write(vdin, VDIN_COM_CTRL0, 0x255);
	t7_vdin_write(vdin, VDIN_ASFIFO_CTRL1, 0xe40000);
	t7_vdin_update(vdin, VPU_SW_RESET, BIT(26), BIT(26));
	t7_vdin_update(vdin, VPU_SW_RESET, BIT(26), 0);
	t7_vdin_write(vdin, VDIN_WIDTHS, ((vdin->input_width - 1) << 16) | (vdin->width - 1));
	t7_vdin_write(vdin, VDIN_INTF_WIDTH, vdin->input_width - 1);
	t7_vdin_configure_scale(vdin);
	t7_vdin_write(vdin, VDIN_VSHRK_CTRL, 0);
	t7_vdin_write(vdin, VDIN_MATRIX_CTRL, 0);
	t7_vdin_write(vdin, VDIN_HDR_CTRL, 0);
	t7_vdin_write(vdin, VDIN_HDR_MATRIX_IN, 0);
	t7_vdin_write(vdin, VDIN_HDR_MATRIX_OUT, 0);
	if (matrix) {
		t7_vdin_write(vdin, 0x128c, matrix[0]);
		t7_vdin_write(vdin, 0x128d, matrix[1]);
		for (i = 0; i < 5; i++)
			t7_vdin_write(vdin, 0x1282 + i, matrix[2 + i]);
		t7_vdin_write(vdin, 0x128a, matrix[7]);
		t7_vdin_write(vdin, 0x128b, matrix[8]);
		t7_vdin_write(vdin, VDIN_HDR_CTRL, BIT(16));
		t7_vdin_write(vdin, VDIN_HDR_MATRIX_IN, 1);
		t7_vdin_write(vdin, VDIN_MATRIX_CTRL, 1);
	}
	if (t7_hdmirx_stream_is_hdr(vdin->receiver))
		t7_vdin_hdr_configure(vdin);
	t7_vdin_update(vdin, VDIN_LFIFO_CTRL, GENMASK(13, 0) | BIT(17) | BIT(18),
		       BIT(17) | 0x780);
	t7_vdin_write(vdin, VDIN_WR_CTRL2, BIT(18) | (2 << 12));
	t7_vdin_write(vdin, VDIN_WR_H_WIN, BIT(30) | (vdin->width - 1));
	t7_vdin_write(vdin, VDIN_WR_V_WIN, vdin->height - 1);
	/* Packed 24-bit output; use EOL to reset the packer on every line. */
	t7_vdin_write(vdin, VDIN_WR_CTRL,
		      ((vdin->nv12 ? 2U : 3U) << 30) | BIT(29) | BIT(27) |
		      BIT(24) | BIT(23) | BIT(22) |
		      (vdin->nv12 ? BIT(18) : (3 << 16)) |
		      (vdin->nv12 ? (2 << 12) : BIT(12)) | BIT(10) | BIT(9));
	t7_vdin_write(vdin, VDIN_WR_BADDR, vdin->frame_dma >> 4);
	t7_vdin_write(vdin, VDIN_WR_STRIDE, vdin->stride >> 4);
	if (vdin->nv12) {
		t7_vdin_write(vdin, VDIN_WR_CHROMA,
			      (vdin->frame_dma + vdin->stride * vdin->height) >> 4);
		t7_vdin_write(vdin, VDIN_WR_CHROMA_STRIDE, vdin->stride >> 4);
	}
	/* MIF0 takes normal VDIN0; done waits for the final AXI write response. */
	t7_vdin_update(vdin, VDIN_DOUBLE_CTRL, GENMASK(7, 0) | BIT(20),
		       BIT(4) | BIT(20));
	t7_vdin_update(vdin, VPU_WRARB_MODE, BIT(16), 0);
	t7_vdin_update(vdin, VPU_WRARB_REQEN, BIT(0), BIT(0));
	t7_vdin_write(vdin, VDIN_WR_URGENT, BIT(9) | BIT(8));
}

static int t7_vdin_capture(struct t7_vdin *vdin)
{
	unsigned long flags;
	bool completed;
	int ret;

	if (vdin->faulted)
		return -EIO;
	ret = t7_hdmirx_stream_get(vdin->receiver, vdin->input_width, vdin->input_height);
	if (ret)
		return ret;
	if (!vdin->nv12 && t7_hdmirx_stream_is_yuv(vdin->receiver)) {
		t7_hdmirx_stream_put(vdin->receiver);
		return -EOPNOTSUPP;
	}
	memset(vdin->frame, 0xa5, vdin->frame_size);
	dma_wmb();
	t7_vdin_configure(vdin);
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, VDIN_DONE_CLEAR);
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, 0);
	reinit_completion(&vdin->done);
	spin_lock_irqsave(&vdin->irq_lock, flags);
	vdin->frame_irqs = 0;
	vdin->dma_idle = false;
	vdin->armed = true;
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_WR_REQ | VDIN_VCP_WR,
		       VDIN_WR_REQ | VDIN_VCP_WR);
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	completed = wait_for_completion_timeout(&vdin->done, msecs_to_jiffies(1000));
	ret = t7_vdin_stop_dma(vdin);
	if (ret)
		return ret;
	if (!completed) {
		dev_err(vdin->dev, "capture timeout: IRQs %u, pixels %#x, lines %#x, WR %#x\n",
			vdin->frame_irqs, t7_vdin_read(vdin, VDIN_ACTIVE_PIXELS),
			t7_vdin_read(vdin, VDIN_LCNT_STATUS), t7_vdin_read(vdin, VDIN_WR_STATUS));
		t7_hdmirx_stream_put(vdin->receiver);
		return -ETIMEDOUT;
	}
	dma_rmb();
	ret = t7_hdmirx_check_video(vdin->receiver);
	if (!ret)
		vdin->snapshots++;
	t7_hdmirx_stream_put(vdin->receiver);
	return ret;
}

static int t7_vdin_frame_open(struct inode *inode, struct file *file)
{
	struct t7_vdin *vdin = inode->i_private;
	struct t7_vdin_snapshot *snapshot;
	int ret;

	ret = debugfs_file_get(file->f_path.dentry);
	if (ret)
		return ret;
	ret = mutex_lock_interruptible(&vdin->lock);
	if (ret)
		goto put_file;
	/* S_FMT may change geometry; size and capture share the same lock. */
	snapshot = kvmalloc(struct_size(snapshot, data, vdin->frame_size), GFP_KERNEL);
	if (!snapshot) {
		mutex_unlock(&vdin->lock);
		ret = -ENOMEM;
		goto put_file;
	}
	ret = vdin->streaming ? -EBUSY : t7_vdin_capture(vdin);
	if (!ret) {
		snapshot->size = vdin->frame_size;
		memcpy(snapshot->data, vdin->frame, snapshot->size);
	}
	mutex_unlock(&vdin->lock);
	if (ret)
		goto free_snapshot;
	/* Per-fd immutable storage survives later captures or device removal. */
	file->private_data = snapshot;
	debugfs_file_put(file->f_path.dentry);
	return 0;
free_snapshot:
	kvfree(snapshot);
put_file:
	debugfs_file_put(file->f_path.dentry);
	return ret;
}

static ssize_t t7_vdin_frame_read(struct file *file, char __user *buffer,
				  size_t size, loff_t *offset)
{
	struct t7_vdin_snapshot *snapshot = file->private_data;

	return simple_read_from_buffer(buffer, size, offset, snapshot->data, snapshot->size);
}

static int t7_vdin_frame_release(struct inode *inode, struct file *file)
{
	kvfree(file->private_data);
	return 0;
}

static const struct file_operations t7_vdin_frame_fops = {
	.owner = THIS_MODULE,
	.open = t7_vdin_frame_open,
	.read = t7_vdin_frame_read,
	.release = t7_vdin_frame_release,
};

static struct t7_vdin_buffer *t7_vdin_next_buffer(struct t7_vdin *vdin)
{
	struct t7_vdin_buffer *buffer = NULL;
	unsigned long flags;

	spin_lock_irqsave(&vdin->queue_lock, flags);
	if (!list_empty(&vdin->queued)) {
		buffer = list_first_entry(&vdin->queued, struct t7_vdin_buffer, list);
		list_del(&buffer->list);
	}
	spin_unlock_irqrestore(&vdin->queue_lock, flags);
	return buffer;
}

static bool t7_vdin_has_buffer(struct t7_vdin *vdin)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&vdin->queue_lock, flags);
	ready = !list_empty(&vdin->queued);
	spin_unlock_irqrestore(&vdin->queue_lock, flags);
	return ready;
}

static void t7_vdin_return_buffers(struct t7_vdin *vdin, enum vb2_buffer_state state)
{
	struct t7_vdin_buffer *buffer;

	while ((buffer = t7_vdin_next_buffer(vdin)))
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
}

static void t7_vdin_ring_begin(struct t7_vdin *vdin)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&vdin->irq_lock, flags);
	for (i = 0; i < VDIN_DMA_SLOTS; i++)
		vdin->slots[i].state = VDIN_SLOT_FREE;
	vdin->ring_error = 0;
	vdin->ring_stopping = false;
	vdin->ring_discard = true;
	vdin->ring_sequence = 0;
	vdin->ring_pauses = 0;
	vdin->copied_bytes = 0;
	vdin->frame_irqs = 0;
	t7_vdin_configure(vdin);
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, VDIN_DONE_CLEAR);
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, 0);
	reinit_completion(&vdin->done);
	vdin->ring_mode = true;
	t7_vdin_ring_arm(vdin, 0);
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
}

static int t7_vdin_ring_stop(struct t7_vdin *vdin)
{
	unsigned long flags;
	bool running;
	int ret;

	spin_lock_irqsave(&vdin->irq_lock, flags);
	vdin->ring_stopping = true;
	running = vdin->ring_running;
	if (running)
		reinit_completion(&vdin->done);
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	/* Prefer a frame boundary to truncating an in-flight AXI transaction. */
	if (running)
		wait_for_completion_timeout(&vdin->done, msecs_to_jiffies(1000));
	ret = t7_vdin_stop_dma(vdin);
	spin_lock_irqsave(&vdin->irq_lock, flags);
	vdin->ring_mode = false;
	vdin->ring_running = false;
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	return ret;
}

static bool t7_vdin_ring_ready(struct t7_vdin *vdin)
{
	unsigned long flags;
	unsigned int i;
	bool ready = false;

	spin_lock_irqsave(&vdin->irq_lock, flags);
	for (i = 0; i < VDIN_DMA_SLOTS; i++)
		ready |= vdin->slots[i].state == VDIN_SLOT_READY;
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	return ready;
}

static struct t7_vdin_slot *t7_vdin_ring_take(struct t7_vdin *vdin)
{
	struct t7_vdin_slot *oldest = NULL;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&vdin->irq_lock, flags);
	for (i = 0; i < VDIN_DMA_SLOTS; i++) {
		struct t7_vdin_slot *slot = &vdin->slots[i];

		if (slot->state == VDIN_SLOT_READY &&
		    (!oldest || (s32)(slot->sequence - oldest->sequence) < 0))
			oldest = slot;
	}
	if (oldest)
		oldest->state = VDIN_SLOT_COPYING;
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	return oldest;
}

static void t7_vdin_ring_release(struct t7_vdin *vdin, struct t7_vdin_slot *slot)
{
	unsigned long flags;

	spin_lock_irqsave(&vdin->irq_lock, flags);
	if (vdin->direct_dma)
		slot->buffer = NULL;
	slot->state = VDIN_SLOT_FREE;
	if (!vdin->ring_running && !vdin->ring_stopping && !vdin->ring_error &&
	    !kthread_should_stop()) {
		/* An arbitrary restart may begin mid-frame; discard it once. */
		vdin->ring_discard = true;
		t7_vdin_ring_arm(vdin, slot - vdin->slots);
	}
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
}

static int t7_vdin_thread(void *data)
{
	struct t7_vdin *vdin = data;
	const char *reason = "dma-wait";
	int ret = 0;

	t7_vdin_ring_begin(vdin);
	while (!kthread_should_stop()) {
		struct t7_vdin_buffer *buffer;
		struct t7_vdin_slot *slot;
		long ready;

		reason = "dma-wait";
		ready = wait_event_interruptible_timeout(vdin->buffer_wait,
							 kthread_should_stop() ||
							 READ_ONCE(vdin->ring_error) ||
							 (vdin->direct_dma ?
							  (t7_vdin_ring_ready(vdin) ||
							   (!READ_ONCE(vdin->ring_running) && t7_vdin_has_buffer(vdin))) :
							  (t7_vdin_has_buffer(vdin) && t7_vdin_ring_ready(vdin))),
							 msecs_to_jiffies(1000));
		if (kthread_should_stop())
			break;
		if (ready < 0) {
			ret = ready;
			break;
		}
		ret = READ_ONCE(vdin->ring_error);
		if (!ready && (t7_vdin_has_buffer(vdin) ||
			       (vdin->direct_dma && READ_ONCE(vdin->ring_running))))
			ret = -ETIMEDOUT;
		if (ret)
			break;
		if (!ready)
			continue;
		slot = t7_vdin_ring_take(vdin);
		if (!slot) {
			if (vdin->direct_dma) {
				unsigned long flags;
				int next;

				spin_lock_irqsave(&vdin->irq_lock, flags);
				next = t7_vdin_ring_free_slot(vdin);
				if (next >= 0 && !vdin->ring_running && !vdin->ring_stopping &&
				    !vdin->ring_error) {
					vdin->ring_discard = true;
					t7_vdin_ring_arm(vdin, next);
				}
				spin_unlock_irqrestore(&vdin->irq_lock, flags);
			}
			continue;
		}
		buffer = vdin->direct_dma ? slot->buffer : t7_vdin_next_buffer(vdin);
		mutex_lock(&vdin->lock);
		ret = t7_hdmirx_check_capture(vdin->receiver, &reason);
		if (!vdin->direct_dma)
			dma_sync_single_for_cpu(vdin->dev, slot->dma, vdin->frame_size, DMA_FROM_DEVICE);
		if (!ret) {
			if (vdin->direct_dma) {
				/* The IRQ proved final AXI completion before publishing READY. */
				dma_rmb();
			} else {
				memcpy(vb2_plane_vaddr(&buffer->vb.vb2_buf, 0),
				       slot->data, vdin->frame_size);
				vdin->copied_bytes += vdin->frame_size;
			}
			/* Publish CPU writes to the coherent output before handing it off. */
			dma_wmb();
			ret = t7_hdmirx_check_capture(vdin->receiver, &reason);
			buffer->vb.vb2_buf.timestamp = slot->timestamp;
			buffer->vb.sequence = slot->sequence;
			buffer->vb.field = V4L2_FIELD_NONE;
		}
		if (!vdin->direct_dma)
			dma_sync_single_for_device(vdin->dev, slot->dma, vdin->frame_size, DMA_FROM_DEVICE);
		/* A torn RX measurement invalidates this frame, not the stream.
		 * Persistent instability is bounded by the receiver recovery worker.
		 */
		if (ret == -EAGAIN)
			vdin->invalid_frames++;
		if (ret && ret != -EAGAIN) {
			unsigned long flags;

			spin_lock_irqsave(&vdin->irq_lock, flags);
			vdin->ring_stopping = true;
			spin_unlock_irqrestore(&vdin->irq_lock, flags);
		}
		t7_vdin_ring_release(vdin, slot);
		mutex_unlock(&vdin->lock);
		vb2_buffer_done(&buffer->vb.vb2_buf,
				ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		if (ret && ret != -EAGAIN)
			break;
	}
	if (t7_vdin_ring_stop(vdin))
		ret = -EIO;
	mutex_lock(&vdin->lock);
	if (vdin->receiver_lease && !vdin->faulted) {
		t7_hdmirx_stream_put(vdin->receiver);
		vdin->receiver_lease = false;
	}
	mutex_unlock(&vdin->lock);
	if (vdin->direct_dma) {
		unsigned int i;

		/* STREAMOFF joins us before vb2 may destroy its buffer objects. On
		 * a stop fault, buf_cleanup retains their pre-acquired DMA leases.
		 */
		for (i = 0; i < VDIN_DMA_SLOTS; i++) {
			struct t7_vdin_buffer *buffer = vdin->slots[i].buffer;

			if (buffer) {
				vdin->slots[i].buffer = NULL;
				vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_ERROR);
			}
		}
	}
	if (ret) {
		vdin->stream_stops++;
		vdin->last_stop_error = ret;
		vdin->last_stop_reason = reason;
		vdin->last_stop_sequence = vdin->ring_sequence;
		vb2_queue_error(&vdin->queue);
		t7_vdin_return_buffers(vdin, VB2_BUF_STATE_ERROR);
		/* Keep the task alive until STREAMOFF joins it. */
		wait_event_interruptible(vdin->buffer_wait, kthread_should_stop());
	}
	return 0;
}

static int t7_vdin_queue_setup(struct vb2_queue *queue, unsigned int *buffers,
			       unsigned int *planes, unsigned int sizes[],
			       struct device *alloc_devs[])
{
	struct t7_vdin *vdin = vb2_get_drv_priv(queue);

	/* Reject retries before vb2 can allocate a new CMA-backed buffer pool. */
	if (vdin->faulted)
		return -EIO;
	if (*planes)
		return *planes == 1 && sizes[0] >= vdin->allocation_size ? 0 : -EINVAL;
	*planes = 1;
	sizes[0] = vdin->allocation_size;
	return 0;
}

static int t7_vdin_buffer_init(struct vb2_buffer *vb)
{
	struct t7_vdin *vdin = vb2_get_drv_priv(vb->vb2_queue);
	struct t7_vdin_buffer *buffer = container_of(to_vb2_v4l2_buffer(vb),
						   struct t7_vdin_buffer, vb);

	if (!vdin->direct_dma)
		return 0;
	/* Acquire before any IRQ can use the address. This independent allocator
	 * reference survives STREAMOFF, orphaned mappings and REQBUFS(0).
	 */
	buffer->lease = vb->vb2_queue->mem_ops->get_dmabuf(vb, vb->planes[0].mem_priv, O_RDWR);
	if (IS_ERR_OR_NULL(buffer->lease)) {
		buffer->lease = NULL;
		return -ENOMEM;
	}
	buffer->dma = vb2_dma_contig_plane_dma_addr(vb, 0);
	return 0;
}

static void t7_vdin_buffer_cleanup(struct vb2_buffer *vb)
{
	struct t7_vdin *vdin = vb2_get_drv_priv(vb->vb2_queue);
	struct t7_vdin_buffer *buffer = container_of(to_vb2_v4l2_buffer(vb),
						   struct t7_vdin_buffer, vb);

	/* Never free/reallocate memory a failed-stop MIF might still address.
	 * The existing fault path pins the module, clocks and private allocations;
	 * retain used direct buffers too, until hardware reboot. ERROR contents are
	 * invalid and the faulted queue cannot restart. Unused buffers may be freed.
	 */
	if (buffer->lease && !(vdin->faulted && buffer->hardware_used))
		dma_buf_put(buffer->lease);
	buffer->lease = NULL;
}

static int t7_vdin_buffer_prepare(struct vb2_buffer *vb)
{
	struct t7_vdin *vdin = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < vdin->allocation_size)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, vdin->frame_size);
	return 0;
}

static void t7_vdin_buffer_queue(struct vb2_buffer *vb)
{
	struct t7_vdin *vdin = vb2_get_drv_priv(vb->vb2_queue);
	struct t7_vdin_buffer *buffer = container_of(to_vb2_v4l2_buffer(vb),
						   struct t7_vdin_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&vdin->queue_lock, flags);
	list_add_tail(&buffer->list, &vdin->queued);
	spin_unlock_irqrestore(&vdin->queue_lock, flags);
	wake_up(&vdin->buffer_wait);
}

static int t7_vdin_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct t7_vdin *vdin = vb2_get_drv_priv(queue);
	int ret;

	mutex_lock(&vdin->lock);
	ret = vdin->faulted ? -EIO :
		t7_hdmirx_stream_get(vdin->receiver, vdin->input_width, vdin->input_height);
	if (!ret && !vdin->nv12 && t7_hdmirx_stream_is_yuv(vdin->receiver)) {
		t7_hdmirx_stream_put(vdin->receiver);
		ret = -EOPNOTSUPP;
	}
	if (!ret) {
		vdin->receiver_lease = true;
		vdin->streaming = true;
		vdin->thread = kthread_run(t7_vdin_thread, vdin, "t7-vdin-capture");
		if (IS_ERR(vdin->thread)) {
			ret = PTR_ERR(vdin->thread);
			vdin->thread = NULL;
			vdin->streaming = false;
			t7_hdmirx_stream_put(vdin->receiver);
			vdin->receiver_lease = false;
		}
	}
	mutex_unlock(&vdin->lock);
	if (ret)
		t7_vdin_return_buffers(vdin, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void t7_vdin_stop_streaming(struct vb2_queue *queue)
{
	struct t7_vdin *vdin = vb2_get_drv_priv(queue);

	/* Finish the bounded capture and CPU copy before returning any buffer. */
	kthread_stop(vdin->thread);
	vdin->thread = NULL;
	t7_vdin_return_buffers(vdin, VB2_BUF_STATE_ERROR);
	mutex_lock(&vdin->lock);
	vdin->streaming = false;
	mutex_unlock(&vdin->lock);
}

static const struct vb2_ops t7_vdin_queue_ops = {
	.queue_setup = t7_vdin_queue_setup,
	.buf_init = t7_vdin_buffer_init,
	.buf_cleanup = t7_vdin_buffer_cleanup,
	.buf_prepare = t7_vdin_buffer_prepare,
	.buf_queue = t7_vdin_buffer_queue,
	.start_streaming = t7_vdin_start_streaming,
	.stop_streaming = t7_vdin_stop_streaming,
};

static void t7_vdin_source_work(struct work_struct *work)
{
	struct t7_vdin *vdin = container_of(to_delayed_work(work), struct t7_vdin, source_work);
	struct v4l2_dv_timings timings;
	struct v4l2_fract interval;
	struct v4l2_event event = { .type = V4L2_EVENT_SOURCE_CHANGE };
	unsigned int period = 0, delta;
	bool locked;
	int ret;

	mutex_lock(&vdin->video_lock);
	ret = t7_hdmirx_query_timings(vdin->receiver, &timings, &interval);
	/* A torn period is a retry, not evidence that the source disappeared. */
	if (ret != -EAGAIN) {
		locked = !ret;
		if (locked && interval.denominator)
			period = div_u64((u64)interval.numerator * 1000000, interval.denominator);
		delta = period > vdin->source_period ? period - vdin->source_period :
			vdin->source_period - period;
		if (vdin->source_seen && (locked != vdin->source_locked ||
		    (locked && (timings.bt.width != vdin->source_width ||
				timings.bt.height != vdin->source_height ||
				delta > vdin->source_period / 2000)))) {
			event.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
			vdin->source_changes++;
			v4l2_event_queue(&vdin->video, &event);
		}
		vdin->source_seen = true;
		vdin->source_locked = locked;
		if (locked) {
			vdin->source_width = timings.bt.width;
			vdin->source_height = timings.bt.height;
			vdin->source_period = period;
		}
	}
	mutex_unlock(&vdin->video_lock);
	schedule_delayed_work(&vdin->source_work, msecs_to_jiffies(250));
}

static void t7_vdin_source_start(struct t7_vdin *vdin)
{
	schedule_delayed_work(&vdin->source_work, 0);
}

static void t7_vdin_source_stop(struct t7_vdin *vdin)
{
	/* Cancel before taking either ioctl lock: the work owns video_lock. */
	cancel_delayed_work_sync(&vdin->source_work);
}

static int t7_vdin_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "t7-vdin-test", sizeof(cap->driver));
	strscpy(cap->card, "T7 HDMI capture experiment", sizeof(cap->card));
	return 0;
}

static int t7_vdin_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *fmt)
{
	struct t7_vdin *vdin = video_drvdata(file);

	if (fmt->index)
		return -EINVAL;
	fmt->pixelformat = vdin->pixel_format;
	return 0;
}

static void t7_vdin_fill_fmt(struct t7_vdin *vdin, struct v4l2_format *fmt,
			     unsigned int width, unsigned int height)
{
	/* VDIN needs 16-byte rows; Wave5 imports NV12 with 32-byte rows.
	 * Preserve the visible width while sharing the same DMA allocation.
	 */
	unsigned int stride = ALIGN(width * (vdin->nv12 ? 1 : 3),
				    vdin->nv12 ? 32 : 16);

	fmt->fmt.pix = (struct v4l2_pix_format) {
		.width = width,
		.height = height,
		.pixelformat = vdin->pixel_format,
		.field = V4L2_FIELD_NONE,
		.bytesperline = stride,
		.sizeimage = vdin->nv12 ? stride * ALIGN(height, 16) * 3 / 2 : stride * height,
		.colorspace = vdin->nv12 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SRGB,
		.xfer_func = vdin->nv12 ? V4L2_XFER_FUNC_709 : V4L2_XFER_FUNC_SRGB,
		.ycbcr_enc = vdin->nv12 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_DEFAULT,
		.quantization = vdin->nv12 ? V4L2_QUANTIZATION_LIM_RANGE :
					   V4L2_QUANTIZATION_FULL_RANGE,
	};
}

static int t7_vdin_get_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct t7_vdin *vdin = video_drvdata(file);

	t7_vdin_fill_fmt(vdin, fmt, vdin->width, vdin->height);
	return 0;
}

static int t7_vdin_negotiate_fmt(struct t7_vdin *vdin, struct v4l2_format *fmt,
				struct v4l2_dv_timings *timings)
{
	struct v4l2_fract interval;
	unsigned int width = fmt->fmt.pix.width, height = fmt->fmt.pix.height;
	int ret;

	ret = t7_hdmirx_query_timings(vdin->receiver, timings, &interval);
	if (ret)
		return ret;
	if (timings->bt.width > 3840 || timings->bt.height > 2160)
		return -ERANGE;
	if (!vdin->nv12 || !width || !height) {
		width = timings->bt.width;
		height = timings->bt.height;
	} else {
		/* MIF strides are aligned independently; preserve the active width.
		 * NV12 chroma needs even dimensions.
		 * Keep downscale ratios within the scaler's phase-step range.
		 */
		if (width < 320)
			width = 320;
		if (height < 180)
			height = 180;
		if (width > timings->bt.width)
			width = timings->bt.width;
		if (height > timings->bt.height)
			height = timings->bt.height;
		width &= ~1U;
		height &= ~1U;
	}
	t7_vdin_fill_fmt(vdin, fmt, width, height);
	return 0;
}

static int t7_vdin_try_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct v4l2_dv_timings timings;

	return t7_vdin_negotiate_fmt(video_drvdata(file), fmt, &timings);
}

static int t7_vdin_set_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct t7_vdin *vdin = video_drvdata(file);
	struct v4l2_dv_timings timings;
	int ret;

	/* The core holds video_lock here. Disconnect takes queue_mutex first;
	 * trylock avoids reversing that order while still excluding REQBUFS.
	 */
	if (!mutex_trylock(&vdin->queue_mutex))
		return -EBUSY;
	ret = -EBUSY;
	if (vb2_is_busy(&vdin->queue) || !mutex_trylock(&vdin->lock))
		goto unlock_queue;
	ret = vdin->faulted ? -EIO : t7_vdin_negotiate_fmt(vdin, fmt, &timings);
	if (!ret) {
		unsigned int frame_size = fmt->fmt.pix.bytesperline * fmt->fmt.pix.height;

		if (vdin->nv12)
			frame_size += frame_size / 2;
		if (frame_size > vdin->frame_capacity) {
			ret = -ERANGE;
		} else {
			vdin->input_width = timings.bt.width;
			vdin->input_height = timings.bt.height;
			vdin->width = fmt->fmt.pix.width;
			vdin->height = fmt->fmt.pix.height;
			vdin->stride = fmt->fmt.pix.bytesperline;
			vdin->allocation_size = fmt->fmt.pix.sizeimage;
			vdin->frame_size = frame_size;
		}
	}
	mutex_unlock(&vdin->lock);
unlock_queue:
	mutex_unlock(&vdin->queue_mutex);
	return ret;
}

static int t7_vdin_enum_input(struct file *file, void *priv, struct v4l2_input *input)
{
	struct t7_vdin *vdin = video_drvdata(file);

	if (input->index)
		return -EINVAL;
	strscpy(input->name, "HDMI", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->capabilities = 0;
	/* Polling status must not hold the capture queue lock across the 20ms
	 * stability wait. STREAMON still performs the two-sample qualification.
	 */
	input->status = t7_hdmirx_check_video_now(vdin->receiver) ?
			V4L2_IN_ST_NO_SYNC : 0;
	return 0;
}

static int t7_vdin_get_input(struct file *file, void *priv, unsigned int *input)
{
	*input = 0;
	return 0;
}

static int t7_vdin_query_dv_timings(struct file *file, void *priv,
				    struct v4l2_dv_timings *timings)
{
	struct t7_vdin *vdin = video_drvdata(file);
	struct v4l2_fract interval;

	return t7_hdmirx_query_timings(vdin->receiver, timings, &interval);
}

static int t7_vdin_get_parm(struct file *file, void *priv, struct v4l2_streamparm *parm)
{
	struct t7_vdin *vdin = video_drvdata(file);
	struct v4l2_dv_timings timings;
	struct v4l2_fract interval;
	int ret;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	ret = t7_hdmirx_query_timings(vdin->receiver, &timings, &interval);
	if (ret)
		return ret;
	/* Capture cannot change the source clock; TIMEPERFRAME capability would
	 * incorrectly promise S_PARM support. This is a measured, read-only value.
	 */
	memset(&parm->parm, 0, sizeof(parm->parm));
	parm->parm.capture.timeperframe = interval;
	return 0;
}

static int t7_vdin_set_input(struct file *file, void *priv, unsigned int input)
{
	return input ? -EINVAL : 0;
}

static int t7_vdin_get_edid(struct file *file, void *priv, struct v4l2_edid *edid)
{
	struct t7_vdin *vdin = video_drvdata(file);

	return t7_hdmirx_get_edid(vdin->receiver, edid);
}

static int t7_vdin_set_edid(struct file *file, void *priv, struct v4l2_edid *edid)
{
	struct t7_vdin *vdin = video_drvdata(file);

	return t7_hdmirx_set_edid(vdin->receiver, edid);
}

static const struct v4l2_ioctl_ops t7_vdin_ioctl_ops = {
	.vidioc_g_edid = t7_vdin_get_edid,
	.vidioc_s_edid = t7_vdin_set_edid,
	.vidioc_querycap = t7_vdin_querycap,
	.vidioc_enum_fmt_vid_cap = t7_vdin_enum_fmt,
	.vidioc_g_fmt_vid_cap = t7_vdin_get_fmt,
	.vidioc_try_fmt_vid_cap = t7_vdin_try_fmt,
	.vidioc_s_fmt_vid_cap = t7_vdin_set_fmt,
	.vidioc_enum_input = t7_vdin_enum_input,
	.vidioc_g_input = t7_vdin_get_input,
	.vidioc_s_input = t7_vdin_set_input,
	.vidioc_query_dv_timings = t7_vdin_query_dv_timings,
	.vidioc_g_parm = t7_vdin_get_parm,
	.vidioc_subscribe_event = v4l2_src_change_event_subscribe,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations t7_vdin_video_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static void t7_vdin_release(struct kref *ref)
{
	kfree(container_of(ref, struct t7_vdin, ref));
}

static void t7_vdin_put(void *data)
{
	struct t7_vdin *vdin = data;

	kref_put(&vdin->ref, t7_vdin_release);
}

static void t7_vdin_video_release(struct video_device *video)
{
	t7_vdin_put(video_get_drvdata(video));
}

static void t7_vdin_video_unregister(void *data)
{
	struct t7_vdin *vdin = data;
	struct video_device *video = &vdin->video;

	t7_vdin_source_stop(vdin);
	/* Quiesce both ioctl classes before RX resources can be unmapped. The
	 * vb2 unregister helper only takes the queue lock, so perform its release
	 * sequence here with the separate status lock and the same device ref.
	 */
	get_device(&video->dev);
	mutex_lock(&vdin->queue_mutex);
	mutex_lock(&vdin->video_lock);
	video_unregister_device(video);
	mutex_unlock(&vdin->video_lock);
	vb2_queue_release(&vdin->queue);
	vdin->queue.owner = NULL;
	mutex_unlock(&vdin->queue_mutex);
	v4l2_device_unregister(&vdin->v4l2);
	put_device(&video->dev);
}

static int t7_vdin_video_register(struct t7_vdin *vdin)
{
	struct vb2_queue *queue = &vdin->queue;
	struct video_device *video = &vdin->video;
	int ret;

	ret = v4l2_device_register(vdin->dev, &vdin->v4l2);
	if (ret)
		return ret;
	queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	queue->io_modes = VB2_MMAP;
	queue->drv_priv = vdin;
	queue->buf_struct_size = sizeof(struct t7_vdin_buffer);
	queue->ops = &t7_vdin_queue_ops;
	queue->mem_ops = &vb2_dma_contig_memops;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
				 V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	queue->min_queued_buffers = 2;
	queue->lock = &vdin->queue_mutex;
	queue->dev = vdin->dev;
	ret = vb2_queue_init(queue);
	if (ret)
		goto unregister_v4l2;
	strscpy(video->name, "t7-vdin-test", sizeof(video->name));
	video->v4l2_dev = &vdin->v4l2;
	video->fops = &t7_vdin_video_fops;
	video->ioctl_ops = &t7_vdin_ioctl_ops;
	video->release = t7_vdin_video_release;
	video->lock = &vdin->video_lock;
	video->queue = queue;
	video->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(video, vdin);
	/* Open files can outlive platform removal; vb2 memory has no DMA owner. */
	kref_get(&vdin->ref);
	ret = video_register_device(video, VFL_TYPE_VIDEO, -1);
	if (ret) {
		t7_vdin_put(vdin);
		vb2_queue_release(queue);
		goto unregister_v4l2;
	}
	ret = devm_add_action_or_reset(vdin->dev, t7_vdin_video_unregister, vdin);
	if (!ret)
		t7_vdin_source_start(vdin);
	return ret;

unregister_v4l2:
	v4l2_device_unregister(&vdin->v4l2);
	return ret;
}

static int t7_vdin_status(struct seq_file *s, void *unused)
{
	struct t7_vdin *vdin = dev_get_drvdata(s->private);

	seq_printf(s, "snapshots: %u\n", vdin->snapshots);
	seq_printf(s, "frame_irqs: %u\n", vdin->frame_irqs);
	seq_printf(s, "faulted: %u\n", vdin->faulted);
	seq_printf(s, "dma_idle: %u\n", vdin->dma_idle);
	seq_printf(s, "direct_dma: %u\n", vdin->direct_dma);
	seq_printf(s, "stream_copy_bytes: %llu\n", READ_ONCE(vdin->copied_bytes));
	seq_printf(s, "stream_frames: %u\n", vdin->ring_sequence);
	seq_printf(s, "stream_pauses: %u\n", vdin->ring_pauses);
	seq_printf(s, "stream_error: %d\n", vdin->ring_error);
	seq_printf(s, "stream_stops: %u\n", READ_ONCE(vdin->stream_stops));
	seq_printf(s, "source_changes: %u\n", READ_ONCE(vdin->source_changes));
	seq_printf(s, "invalid_frames: %llu\n", READ_ONCE(vdin->invalid_frames));
	seq_printf(s, "last_stop_error: %d\n", READ_ONCE(vdin->last_stop_error));
	seq_printf(s, "last_stop_reason: %s\n", vdin->last_stop_reason ?: "none");
	seq_printf(s, "last_stop_sequence: %u\n", READ_ONCE(vdin->last_stop_sequence));
	seq_printf(s, "write_ctrl: %#x\n", t7_vdin_read(vdin, VDIN_WR_CTRL));
	seq_printf(s, "write_status: %#x\n", t7_vdin_read(vdin, VDIN_WR_STATUS));
	seq_printf(s, "pixels: %#x\n", t7_vdin_read(vdin, VDIN_ACTIVE_PIXELS));
	seq_printf(s, "lines: %#x\n", t7_vdin_read(vdin, VDIN_LCNT_STATUS));
	seq_printf(s, "vpu_hz: %lu\n", clk_get_rate(vdin->clocks[1].clk));
	seq_printf(s, "vapb_hz: %lu\n", clk_get_rate(vdin->clocks[2].clk));
	seq_printf(s, "clkb_hz: %lu\n", clk_get_rate(vdin->clocks[3].clk));
	seq_printf(s, "measure_hz: %lu\n", clk_get_rate(vdin->clocks[4].clk));
	return 0;
}

static void t7_vdin_power_off(void *data)
{
	struct t7_vdin *vdin = data;
	struct arm_smccc_res res;

	if (vdin->memory_on)
		arm_smccc_smc(0x82000093, 60, 0, 0, 0, 0, 0, 0, &res);
	while (vdin->domain_count) {
		struct device *domain = vdin->domains[--vdin->domain_count];

		pm_runtime_put_sync(domain);
		dev_pm_domain_detach(domain, true);
	}
}

static void t7_vdin_clocks_off(void *data)
{
	struct t7_vdin *vdin = data;

	clk_bulk_disable_unprepare(ARRAY_SIZE(vdin->clocks), vdin->clocks);
}

static void t7_vdin_free_frame(void *data)
{
	struct t7_vdin *vdin = data;
	unsigned int i;

	if (vdin->faulted)
		return;
	for (i = 0; i < VDIN_DMA_SLOTS; i++)
		if (vdin->slots[i].data)
			dma_free_noncoherent(vdin->dev, vdin->frame_capacity, vdin->slots[i].data,
					     vdin->slots[i].dma, DMA_FROM_DEVICE);
	dma_free_coherent(vdin->dev, vdin->frame_capacity, vdin->frame, vdin->frame_dma);
}

/* Both tables form one transaction. Never expose partially replaced LUTs to
 * the IRQ; malformed input leaves the previous curve completely intact.
 */
static int t7_vdin_parse_hdr_curve(char *text, u32 *tables)
{
	char *token;
	unsigned int i;
	int ret;

	text = strim(text);
	for (i = 0; i < 298; i++) {
		token = strsep(&text, ",\n");
		if (!token || !*token)
			return -EINVAL;
		ret = kstrtou32(token, 10, &tables[i]);
		if (ret || tables[i] > 4095)
			return -EINVAL;
		if (i < 149 && (!tables[i] || (i && tables[i] > tables[i - 1])))
			return -EINVAL;
		if (i > 149 && tables[i] < tables[i - 1])
			return -EINVAL;
	}
	return text || tables[149] || tables[297] != 4095 ? -EINVAL : 0;
}

static ssize_t hdr_curve_store(struct device *dev, struct device_attribute *attr,
			       const char *buffer, size_t count)
{
	struct t7_vdin *vdin = dev_get_drvdata(dev);
	unsigned long flags;
	u32 *tables;
	char *text;
	int ret;

	tables = kmalloc_array(298, sizeof(*tables), GFP_KERNEL);
	text = kmemdup_nul(buffer, count, GFP_KERNEL);
	if (!tables || !text) {
		ret = -ENOMEM;
		goto free;
	}
	ret = t7_vdin_parse_hdr_curve(text, tables);
	if (ret)
		goto free;
	mutex_lock(&vdin->lock);
	spin_lock_irqsave(&vdin->irq_lock, flags);
	memcpy(vdin->hdr_gain, tables, sizeof(vdin->hdr_gain));
	memcpy(vdin->hdr_oetf, tables + 149, sizeof(vdin->hdr_oetf));
	vdin->hdr_curve_pending = true;
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	mutex_unlock(&vdin->lock);
free:
	kfree(text);
	kfree(tables);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(hdr_curve);

static struct attribute *t7_vdin_attrs[] = {
	&dev_attr_hdr_curve.attr,
	NULL,
};
static const struct attribute_group t7_vdin_group = {
	.attrs = t7_vdin_attrs,
};

static void t7_vdin_debugfs_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static void t7_vdin_put_receiver(void *data)
{
	put_device(data);
}

static int t7_vdin_map_regs(struct platform_device *pdev, struct t7_vdin *vdin)
{
	static const struct {
		resource_size_t start, size;
	} regions[] = {
		{ 0xff004800, 0x400 }, /* VDIN0, including its HDR2 core. */
		{ 0xff009c80, 0x4 },   /* VPU_SW_RESET, only bit 26 is updated. */
		{ 0xff009e88, 0x8 },   /* VDIN write arbitration. */
		{ 0xff01042c, 0xc },   /* VDIN double-write and top controls. */
	};
	struct resource *window;
	unsigned int i;

	/* Existing capture-only DTBs retain their exclusive whole-window lease. */
	if (!of_property_present(pdev->dev.of_node, "amlogic,split-vcbus")) {
		vdin->regs = devm_platform_ioremap_resource(pdev, 0);
		return PTR_ERR_OR_ZERO(vdin->regs);
	}
	window = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!window || window->start != 0xff000000 || resource_size(window) != 0x40000)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		struct resource resource = DEFINE_RES_MEM(regions[i].start, regions[i].size);

		/* Keep resource reservation: mapping alone would silently allow conflicts. */
		vdin->split_regs[i] = devm_ioremap_resource(&pdev->dev, &resource);
		if (IS_ERR(vdin->split_regs[i]))
			return PTR_ERR(vdin->split_regs[i]);
	}
	return 0;
}

static int t7_vdin_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *receiver;
	struct device_node *node;
	struct arm_smccc_res res;
	struct dentry *debugfs;
	struct t7_vdin *vdin;
	unsigned int i;
	int ret;

	vdin = kzalloc_obj(*vdin);
	if (!vdin)
		return -ENOMEM;
	kref_init(&vdin->ref);
	vdin->direct_dma = direct_dma;
	ret = devm_add_action_or_reset(dev, t7_vdin_put, vdin);
	if (ret)
		return ret;
	vdin->dev = dev;
	memcpy(vdin->hdr_gain, t7_hdr_gain, sizeof(vdin->hdr_gain));
	memcpy(vdin->hdr_oetf, t7_hdr_srgb, sizeof(vdin->hdr_oetf));
	mutex_init(&vdin->lock);
	mutex_init(&vdin->video_lock);
	mutex_init(&vdin->queue_mutex);
	INIT_DELAYED_WORK(&vdin->source_work, t7_vdin_source_work);
	spin_lock_init(&vdin->irq_lock);
	spin_lock_init(&vdin->queue_lock);
	INIT_LIST_HEAD(&vdin->queued);
	init_waitqueue_head(&vdin->buffer_wait);
	init_completion(&vdin->done);
	platform_set_drvdata(pdev, vdin);
	node = of_parse_phandle(dev->of_node, "amlogic,receiver", 0);
	receiver = of_find_device_by_node(node);
	of_node_put(node);
	if (!receiver)
		return -EPROBE_DEFER;
	vdin->receiver = &receiver->dev;
	ret = devm_add_action_or_reset(dev, t7_vdin_put_receiver, vdin->receiver);
	if (ret)
		return ret;
	if (!device_is_bound(vdin->receiver))
		return -EPROBE_DEFER;
	if (!device_link_add(dev, vdin->receiver, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	ret = t7_hdmirx_get_max_size(vdin->receiver, &vdin->width, &vdin->height);
	if (ret)
		return ret;
	vdin->nv12 = nv12;
	vdin->pixel_format = nv12 ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_RGB24;
	vdin->input_width = vdin->width;
	vdin->input_height = vdin->height;
	vdin->stride = vdin->width * (nv12 ? 1 : 3);
	vdin->frame_size = vdin->stride * vdin->height;
	if (nv12)
		vdin->frame_size += vdin->frame_size / 2;
	vdin->frame_capacity = vdin->frame_size;
	/* Keep NV12 tightly packed, with tail room for aligned encoder plane views. */
	vdin->allocation_size = nv12 ? vdin->stride * ALIGN(vdin->height, 16) * 3 / 2 :
				       vdin->frame_size;
	ret = t7_vdin_map_regs(pdev, vdin);
	if (ret)
		return ret;
	vdin->clocks[0].id = "pclk";
	vdin->clocks[1].id = "vpu";
	vdin->clocks[2].id = "vapb";
	vdin->clocks[3].id = "clkb";
	vdin->clocks[4].id = "measure";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(vdin->clocks), vdin->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot acquire capture clocks\n");
	ret = devm_add_action_or_reset(dev, t7_vdin_power_off, vdin);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(vdin->domains); i++) {
		struct device *domain = dev_pm_domain_attach_by_id(dev, i);

		if (IS_ERR_OR_NULL(domain))
			return domain ? PTR_ERR(domain) : -ENODEV;
		ret = pm_runtime_resume_and_get(domain);
		if (ret < 0) {
			dev_pm_domain_detach(domain, true);
			return ret;
		}
		vdin->domains[vdin->domain_count++] = domain;
	}
	ret = clk_set_rate(vdin->clocks[1].clk, vdin->width > 1920 ? 666666666 : 400000000);
	if (ret)
		return ret;
	ret = clk_set_rate(vdin->clocks[2].clk, vdin->width > 1920 ? 500000000 : 250000000);
	if (ret)
		return ret;
	ret = clk_set_rate(vdin->clocks[3].clk, vdin->width > 1920 ? 666666666 : 400000000);
	if (ret)
		return ret;
	ret = clk_set_rate(vdin->clocks[4].clk, 50000000);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(vdin->clocks), vdin->clocks);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, t7_vdin_clocks_off, vdin);
	if (ret)
		return ret;
	/* T7 firmware mempd IDs follow 52 power domains: VDIN0 module index 8. */
	arm_smccc_smc(0x82000093, 60, 1, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		return dev_err_probe(dev, -EIO, "Cannot power VDIN0 line memory\n");
	vdin->memory_on = true;
	t7_vdin_write(vdin, VDIN_WR_CTRL, 0);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	vdin->frame = dma_alloc_coherent(dev, vdin->frame_size, &vdin->frame_dma, GFP_KERNEL);
	if (!vdin->frame)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, t7_vdin_free_frame, vdin);
	if (ret)
		return ret;
	/* Direct capture uses queued vb2 allocations; the private copy ring has
	 * no DMA or CPU consumer in that mode. Keep the separate snapshot frame.
	 */
	for (i = 0; !vdin->direct_dma && i < VDIN_DMA_SLOTS; i++) {
		vdin->slots[i].data = dma_alloc_noncoherent(dev, vdin->frame_size,
							    &vdin->slots[i].dma, DMA_FROM_DEVICE,
							    GFP_KERNEL);
		if (!vdin->slots[i].data)
			return -ENOMEM;
		/* Allocation zeroes the cached mapping; transfer initial ownership too. */
		dma_sync_single_for_device(dev, vdin->slots[i].dma,
					   vdin->frame_size, DMA_FROM_DEVICE);
	}
	vdin->irq = platform_get_irq(pdev, 0);
	if (vdin->irq < 0)
		return vdin->irq;
	ret = devm_request_irq(dev, vdin->irq, t7_vdin_write_done, 0, dev_name(dev), vdin);
	if (ret)
		return ret;
	ret = devm_device_add_group(dev, &t7_vdin_group);
	if (ret)
		return ret;
	debugfs = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(debugfs))
		return PTR_ERR(debugfs);
	debugfs_create_file("frame", 0400, debugfs, vdin, &t7_vdin_frame_fops);
	debugfs_create_devm_seqfile(dev, "status", debugfs, t7_vdin_status);
	ret = devm_add_action_or_reset(dev, t7_vdin_debugfs_remove, debugfs);
	if (ret)
		return ret;
	ret = t7_vdin_video_register(vdin);
	if (ret)
		return ret;
	dev_info(dev, "%ux%u %s capture ready at /dev/video%d\n",
		 vdin->width, vdin->height, nv12 ? "NV12" : "RGB24", vdin->video.num);
	return 0;
}

static const struct of_device_id t7_vdin_match[] = {
	{ .compatible = "amlogic,t7-vdin-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, t7_vdin_match);

static struct platform_driver t7_vdin_driver = {
	.probe = t7_vdin_probe,
	.driver = {
		.name = "t7-vdin-test",
		.of_match_table = t7_vdin_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(t7_vdin_driver);

MODULE_DESCRIPTION("Amlogic T7 VDIN0 capture experiment");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
