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

#define VCODEC_DRV_NAME "aml-vdec-drv"

static int stateful_get_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct aml_vdec_ctx *ctx = ctrl_to_dec_ctx(ctrl->handler);

	if (ctrl->id != V4L2_CID_MIN_BUFFERS_FOR_CAPTURE)
		return -EINVAL;
	ctrl->val = READ_ONCE(ctx->hevc_min_capture);
	return 0;
}

static int vdec_g_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	if (ctx->node->get_selection)
		return ctx->node->get_selection(ctx, s);
	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		s->r = ctx->compose;
		return 0;
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		s->r = (struct v4l2_rect) {
			.width = ctx->pix_fmt[AML_FMT_DST].width,
			.height = ctx->pix_fmt[AML_FMT_DST].height,
		};
		return 0;
	default:
		return -EINVAL;
	}
}

static int vdec_try_decoder_cmd(struct file *file, void *priv, struct v4l2_decoder_cmd *dc)
{
	return v4l2_m2m_ioctl_try_decoder_cmd(file, priv, dc);
}

static int vdec_decoder_cmd(struct file *file, void *priv, struct v4l2_decoder_cmd *dc)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);
	struct vb2_v4l2_buffer *dst;
	int ret;

	if (ctx->node->decoder_cmd)
		return ctx->node->decoder_cmd(file, priv, dc);
	ret = vdec_try_decoder_cmd(file, priv, dc);
	if (ret)
		return ret;
	/* An empty drain returns a sentinel, not the unused buffer's contents. */
	if (dc->cmd == V4L2_DEC_CMD_STOP && !v4l2_m2m_next_src_buf(ctx->m2m_ctx)) {
		dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
		if (dst)
			vb2_set_plane_payload(&dst->vb2_buf, 0, 0);
	}
	return v4l2_m2m_ioctl_decoder_cmd(file, priv, dc);
}

static int vdec_subscribe_event(struct v4l2_fh *fh,
				const struct v4l2_event_subscription *sub)
{
	struct aml_vdec_ctx *ctx = container_of(fh, struct aml_vdec_ctx, fh);

	if ((ctx->node->events & BIT(V4L2_EVENT_SOURCE_CHANGE)) &&
	    sub->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subscribe(fh, sub);
	if ((ctx->node->events & BIT(V4L2_EVENT_EOS)) &&
	    sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 2, NULL);
	return v4l2_ctrl_subscribe_event(fh, sub);
}

static void vb2_ops_vdec_buf_queue(struct vb2_buffer *vb)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vb2_v4l2 = to_vb2_v4l2_buffer(vb);

	if (V4L2_TYPE_IS_CAPTURE(vb->type) && v4l2_m2m_dst_buf_is_last(ctx->m2m_ctx)) {
		unsigned int i;

		for (i = 0; i < vb->num_planes; i++)
			vb2_set_plane_payload(vb, i, 0);
		if (ctx->node->last_buffer) {
			ctx->node->last_buffer(ctx, vb2_v4l2);
		} else {
			vb2_v4l2->sequence = ctx->sequence_cap++;
			v4l2_m2m_last_buffer_done(ctx->m2m_ctx, vb2_v4l2);
		}
		return;
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vb2_v4l2);
}

static int vb2ops_vdec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(q);

	if (ctx->dev->dec_hw->faulted)
		return -EIO;
	v4l2_m2m_update_start_streaming_state(ctx->m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		ctx->sequence_out = 0;
		ctx->is_output_streamon = 1;
		if (aml_vdec_init_dec_inst(ctx) < 0)
			return -EINVAL;
	} else {
		ctx->sequence_cap = 0;
		ctx->is_cap_streamon = 1;
		if (ctx->node->start_capture)
			ctx->node->start_capture(ctx);
	}

	return 0;
}

static void vb2ops_vdec_stop_streaming(struct vb2_queue *q)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *src_buf = NULL, *dst_buf = NULL;

	if (ctx->node->cancel_output)
		ctx->node->cancel_output(ctx);
	if (!ctx->node->buffered_capture || V4L2_TYPE_IS_OUTPUT(q->type))
		aml_vdec_release_instance(ctx);

	v4l2_m2m_update_stop_streaming_state(ctx->m2m_ctx, q);

	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		while ((src_buf = v4l2_m2m_src_buf_remove(ctx->m2m_ctx)))
			v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
		ctx->is_output_streamon = 0;
	} else {
		while ((dst_buf = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx)))
			v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
		ctx->is_cap_streamon = 0;
	}
}

const struct v4l2_ioctl_ops aml_vdec_stateful_ioctl_ops = {
	.vidioc_querycap = vidioc_vdec_querycap,
	.vidioc_g_selection = vdec_g_selection,
	.vidioc_enum_framesizes = vidioc_vdec_enum_framesizes,

	.vidioc_enum_fmt_vid_cap = vidioc_vdec_enum_fmt_cap_mplane,
	.vidioc_try_fmt_vid_cap_mplane = vidioc_try_fmt_cap_mplane,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_vdec_s_fmt_cap_mplane,
	.vidioc_g_fmt_vid_cap_mplane = vidioc_vdec_g_fmt_cap_mplane,

	.vidioc_enum_fmt_vid_out = vidioc_vdec_enum_fmt_out_mplane,
	.vidioc_try_fmt_vid_out_mplane = vidioc_try_fmt_out_mplane,
	.vidioc_s_fmt_vid_out_mplane = vidioc_vdec_s_fmt_out_mplane,
	.vidioc_g_fmt_vid_out_mplane = vidioc_vdec_g_fmt_out_mplane,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,

	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_decoder_cmd = vdec_decoder_cmd,
	.vidioc_try_decoder_cmd = vdec_try_decoder_cmd,

	.vidioc_subscribe_event = vdec_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

static const struct vb2_ops aml_vdec_vb2_ops = {
	.queue_setup = vb2ops_vdec_queue_setup,
	.start_streaming = vb2ops_vdec_start_streaming,
	.stop_streaming = vb2ops_vdec_stop_streaming,

	.buf_cleanup = aml_vdec_buf_cleanup,
	.buf_prepare = vb2ops_vdec_buf_prepare,
	.buf_out_validate = vb2ops_vdec_out_buf_validate,
	.buf_queue = vb2_ops_vdec_buf_queue,
	.buf_finish = vb2_ops_vdec_buf_finish,
};

static void stateful_finish_job(struct aml_vdec_ctx *ctx,
				enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *src, *dst;

	src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	src->sequence = ctx->sequence_out++;
	if (state == VB2_BUF_STATE_ERROR)
		src->flags &= ~V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
	if (v4l2_m2m_is_last_draining_src_buf(ctx->m2m_ctx, src)) {
		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(ctx->m2m_ctx);
	}
	v4l2_m2m_buf_done_and_job_finish(ctx->dev->m2m_dev_dec, ctx->m2m_ctx,
					 state);
}

static const struct v4l2_ctrl_ops min_capture_ctrl_ops = {
	.g_volatile_ctrl = stateful_get_volatile_ctrl,
};

static int stateful_ctrls_setup(struct aml_vdec_ctx *ctx)
{
	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 0);
	if (ctx->node->buffered_capture) {
		struct v4l2_ctrl *ctrl;

		ctrl = v4l2_ctrl_new_std(&ctx->ctrl_handler, &min_capture_ctrl_ops,
					 V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, 1, 18, 1, 1);
		if (ctrl)
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE;
	}
	if (ctx->ctrl_handler.error) {
		int ret = ctx->ctrl_handler.error;

		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		return ret;
	}
	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	return v4l2_ctrl_handler_setup(&ctx->ctrl_handler);
}

static void stateful_device_run(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;

	ctx->node->run(ctx);
}

static int stateful_job_ready(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;

	return ctx->node->job_ready ? ctx->node->job_ready(ctx) : 1;
}

static const struct v4l2_m2m_ops stateful_m2m_ops = {
	.device_run = stateful_device_run,
	.job_ready = stateful_job_ready,
};

const struct aml_vdec_interface aml_vdec_stateful_interface = {
	.ioctl_ops = &aml_vdec_stateful_ioctl_ops,
	.queue_ops = &aml_vdec_vb2_ops,
	.m2m_ops = &stateful_m2m_ops,
	.controls_init = stateful_ctrls_setup,
	.finish_job = stateful_finish_job,
};
