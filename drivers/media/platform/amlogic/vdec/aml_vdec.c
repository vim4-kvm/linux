// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#include <linux/dma-buf.h>
#include <media/meson-t7-dos.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include "aml_vdec.h"
#include "aml_vdec_hw.h"
#include "aml_vdec_platform.h"
#include "hevc.h"

#define VCODEC_DRV_NAME "aml-vdec-drv"

void aml_vdec_fault(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_hw *hw = ctx->dev->dec_hw;

	if (xchg(&hw->faulted, true))
		return;
	if (hw->powered) {
		regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_MASK, 0);
		disable_irq(hw->dec_irq);
	}
	/* Pin driver, device, codec DMA and the exclusive DOS lease until reset. */
	__module_get(THIS_MODULE);
	if (hw->has_dbus_ctrl)
		meson_t7_dos_poison();
	get_device(hw->dev);
	if (ctx->m2m_ctx) {
		vb2_queue_error(v4l2_m2m_get_src_vq(ctx->m2m_ctx));
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->m2m_ctx));
	}
	dev_crit(hw->dev, "VDEC DMA ownership uncertain; quarantined until system reset\n");
}

static void aml_vdec_stop_job(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_hw *hw = ctx->dev->dec_hw;

	if (!ctx->dos_clk_en || hw->faulted)
		return;
	if (aml_stop_vdec_hw(hw)) {
		aml_vdec_fault(ctx);
		return;
	}
	ctx->dos_clk_en = false;
	if (ctx->codec_ops->finish)
		ctx->codec_ops->finish(ctx);
}

static void vdec_job_finish(struct aml_vdec_ctx *ctx,
			    enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *src, *dst;

	src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	src->sequence = ctx->sequence_out++;
	if (state == VB2_BUF_STATE_ERROR)
		src->flags &= ~V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
	if (ctx->is_jpeg &&
	    v4l2_m2m_is_last_draining_src_buf(ctx->m2m_ctx, src)) {
		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(ctx->m2m_ctx);
	}
	v4l2_m2m_buf_done_and_job_finish(ctx->dev->m2m_dev_dec, ctx->m2m_ctx,
					 state);
}

static void m2mops_vdec_device_run(void *m2m_priv)
{
	struct aml_vdec_ctx *ctx = m2m_priv;
	struct aml_vdec_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct media_request *src_req;
	int ret = 0;
	const char *fw_path = dev->pvdec_data->fw_path[ctx->curr_dec_type];

	if (ctx->is_hevc) {
		aml_hevc_device_run(ctx);
		return;
	}
	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	/* The core may release a held buffer on timestamp change or FLUSH. */
	if (!dst_buf->is_held)
		dst_buf->sequence = ctx->sequence_cap++;
	v4l2_m2m_buf_copy_metadata(src_buf, dst_buf);
	if (dev->dec_hw->faulted || ctx->decode_failed)
		goto err_cancel_job;
	dev_dbg(&dev->plat_dev->dev, "device run : src buf : %d dst buf %d\n",
		src_buf->vb2_buf.index, dst_buf->vb2_buf.index);
	if (WARN_ON_ONCE(!ctx->codec_ops->run))
		goto err_cancel_job;

	src_req = src_buf->vb2_buf.req_obj.req;
	if (src_req) {
		ret = v4l2_ctrl_request_setup(src_req, &ctx->ctrl_handler);
		if (ret)
			goto complete_request;
	}
	ret = aml_vdec_get_buffer_pins(ctx);
	if (ret)
		goto complete_request;
	/* m2m owns the hardware until stop has drained IRQ and DMA activity. */
	WRITE_ONCE(dev->dec_hw->curr_ctx, ctx->codec_priv);
	if (ctx->codec_ops->prepare)
		ctx->codec_ops->prepare(ctx);
	dos_enable(dev->dec_hw);
	/* incase of bus hang in stop_streaming */
	ctx->dos_clk_en = 1;
	ret = aml_vdec_reset_core(dev->dec_hw, ctx->curr_dec_type);
	if (ret) {
		/* Isolation/reset failed: do not retry MMIO or return DMA ownership. */
		aml_vdec_fault(ctx);
		goto complete_request;
	}

	ret = load_firmware(dev->dec_hw, fw_path, ctx->curr_dec_type);
	if (ret)
		goto stop_job;

	ret = ctx->codec_ops->run(ctx);

stop_job:
	/*
	 * IRQ only signals completion. Successful completion and canvas release
	 * require CPU/DMA stop and T7 DBUS isolation to acknowledge. A failed
	 * stop pins allocations before the queues report errors to userspace.
	 */
	aml_vdec_stop_job(ctx);
complete_request:
	if (src_req)
		v4l2_ctrl_request_complete(src_req, &ctx->ctrl_handler);
	if (ret < 0 || dev->dec_hw->faulted)
		goto err_cancel_job;

	aml_vdec_put_buffer_pins(ctx);
	WRITE_ONCE(dev->dec_hw->curr_ctx, NULL);
	vdec_job_finish(ctx, VB2_BUF_STATE_DONE);
	return;

err_cancel_job:
	aml_vdec_stop_job(ctx);
	aml_vdec_put_buffer_pins(ctx);
	if (!dev->dec_hw->faulted)
		WRITE_ONCE(dev->dec_hw->curr_ctx, NULL);
	vdec_job_finish(ctx, VB2_BUF_STATE_ERROR);
}

static int m2mops_vdec_job_ready(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;

	return ctx->is_hevc ? aml_hevc_job_ready(ctx) : 1;
}

const struct v4l2_m2m_ops aml_vdec_m2m_ops = {
	.device_run = m2mops_vdec_device_run,
	.job_ready = m2mops_vdec_job_ready,
};

static int aml_vdec_pin_buffer(struct vb2_buffer *vb)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct aml_vdec_buffer *buf = container_of(to_vb2_v4l2_buffer(vb),
						  struct aml_vdec_buffer, m2m.vb);
	unsigned int i;

	if (!ctx->dev->pvdec_data->has_dbus_ctrl)
		return 0;
	/* T7 accepts MMAP only: retain both allocation and original DMA mapping. */
	for (i = 0; i < vb->num_planes; i++) {
		buf->pins[i] = vb->vb2_queue->mem_ops->get_dmabuf(vb,
						vb->planes[i].mem_priv, O_RDWR);
		if (IS_ERR_OR_NULL(buf->pins[i])) {
			buf->pins[i] = NULL;
			return -ENOMEM;
		}
	}
	return 0;
}

void aml_vdec_buf_cleanup(struct vb2_buffer *vb)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct aml_vdec_buffer *buf = container_of(to_vb2_v4l2_buffer(vb),
						  struct aml_vdec_buffer, m2m.vb);
	unsigned int i;

	/* Do not put the pins on uncertain stop, even on process exit. */
	if (ctx->dev->dec_hw->faulted)
		return;
	for (i = 0; i < vb->num_planes; i++) {
		if (buf->pins[i]) {
			dma_buf_put(buf->pins[i]);
			buf->pins[i] = NULL;
		}
	}
}

void aml_vdec_put_buffer_pins(struct aml_vdec_ctx *ctx)
{
	struct vb2_queue *queues[] = { v4l2_m2m_get_src_vq(ctx->m2m_ctx),
				       v4l2_m2m_get_dst_vq(ctx->m2m_ctx) };
	struct vb2_buffer *vb;
	unsigned int q, i;

	if (!ctx->buffers_held)
		return;
	for (q = 0; q < ARRAY_SIZE(queues); q++) {
		for (i = 0; i < queues[q]->max_num_buffers; i++) {
			vb = vb2_get_buffer(queues[q], i);
			if (vb)
				aml_vdec_buf_cleanup(vb);
		}
	}
	ctx->buffers_held = false;
	mutex_unlock(&queues[1]->mmap_lock);
	mutex_unlock(&queues[0]->mmap_lock);
}

int aml_vdec_get_buffer_pins(struct aml_vdec_ctx *ctx)
{
	struct vb2_queue *queues[] = { v4l2_m2m_get_src_vq(ctx->m2m_ctx),
				       v4l2_m2m_get_dst_vq(ctx->m2m_ctx) };
	struct vb2_buffer *vb;
	unsigned int q, i;

	if (!ctx->dev->pvdec_data->has_dbus_ctrl)
		return 0;
	/*
	 * Pin before the first MMIO, not at allocation: an idle internal export
	 * makes vb2 QUERYBUF incorrectly report MAPPED before userspace mmap.
	 * Freeze both buffer arrays, including DPB references, until DMA stops.
	 * STREAMOFF/close wait for m2m cancellation before taking mmap_lock;
	 * CREATE_BUFS must not add an unpinned DMA target during this job.
	 */
	mutex_lock(&queues[0]->mmap_lock);
	mutex_lock_nested(&queues[1]->mmap_lock, SINGLE_DEPTH_NESTING);
	ctx->buffers_held = true;
	for (q = 0; q < ARRAY_SIZE(queues); q++) {
		for (i = 0; i < queues[q]->max_num_buffers; i++) {
			vb = vb2_get_buffer(queues[q], i);
			if (vb && aml_vdec_pin_buffer(vb)) {
				aml_vdec_put_buffer_pins(ctx);
				return -ENOMEM;
			}
		}
	}
	return 0;
}
