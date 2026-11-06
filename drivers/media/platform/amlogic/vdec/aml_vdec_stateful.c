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

static void stateful_output(struct aml_vdec_ctx *ctx, const struct aml_vdec_frame *frame,
			    enum vb2_buffer_state state, bool last)
{
	struct vb2_v4l2_buffer *vb = frame->buffer;

	vb->sequence = aml_vdec_stateful(ctx)->sequence++;
	if (last)
		vb->flags |= V4L2_BUF_FLAG_LAST;
	v4l2_m2m_buf_done(vb, state);
}

static bool stateful_flush(struct aml_vdec_ctx *ctx, enum vb2_buffer_state state, bool last)
{
	struct aml_vdec_frame frame;
	bool emitted = false;

	while (ctx->node->stateful_ops->get_frame(ctx, true, &frame) > 0) {
		stateful_output(ctx, &frame, state, last && frame.final);
		emitted = true;
	}
	return emitted;
}

static void stateful_eos(struct aml_vdec_ctx *ctx)
{
	const struct v4l2_event event = { .type = V4L2_EVENT_EOS };

	v4l2_event_queue_fh(&ctx->fh, &event);
}

static void stateful_last_buffer_done(struct aml_vdec_ctx *ctx,
				      struct vb2_v4l2_buffer *vb)
{
	struct aml_vdec_stateful_ctx *s;

	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	s = aml_vdec_stateful(ctx);
	if (ctx->codec_priv)
		vb->sequence = s->sequence++;
	stateful_eos(ctx);
	v4l2_m2m_last_buffer_done(ctx->m2m_ctx, vb);
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
}

static void stateful_end_capture(struct aml_vdec_ctx *ctx, bool eos)
{
	struct aml_vdec_stateful_ctx *s = aml_vdec_stateful(ctx);
	struct vb2_v4l2_buffer *vb = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);
	unsigned int i;

	if (!vb) {
		if (eos)
			ctx->m2m_ctx->next_buf_last = true;
		return;
	}
	for (i = 0; i < vb->vb2_buf.num_planes; i++)
		vb2_set_plane_payload(&vb->vb2_buf, i, 0);
	vb->sequence = s->sequence++;
	if (eos) {
		stateful_eos(ctx);
		v4l2_m2m_last_buffer_done(ctx->m2m_ctx, vb);
	} else {
		/* A source change terminates CAPTURE, not the OUTPUT drain. */
		vb->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_DONE);
	}
}

static void stateful_cancel_output(struct aml_vdec_ctx *ctx)
{
	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	if (ctx->codec_priv)
		stateful_flush(ctx, VB2_BUF_STATE_ERROR, false);
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
}

static void stateful_start_capture(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_stateful_ctx *s;

	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	s = aml_vdec_stateful(ctx);
	if (ctx->codec_priv)
		s->sequence = 0;
	WRITE_ONCE(aml_vdec_stateful(ctx)->wait_capture, false);
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
}

static int stateful_get_selection(struct aml_vdec_ctx *ctx, struct v4l2_selection *selection)
{
	struct aml_vdec_stateful_ctx *s;
	int ret = 0;

	if (selection->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	s = aml_vdec_stateful(ctx);
	switch (selection->target) {
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		selection->r = ctx->codec_priv && s->format_known ? s->crop : ctx->compose;
		break;
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		selection->r = (struct v4l2_rect) {
			.width = ctx->pix_fmt[AML_FMT_DST].width,
			.height = ctx->pix_fmt[AML_FMT_DST].height,
		};
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
	return ret;
}

static void stateful_source_change(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_stateful_ctx *s = aml_vdec_stateful(ctx);
	struct aml_vdec_stream_info info;
	struct v4l2_event event = { .type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION };
	struct v4l2_pix_format_mplane *old = &ctx->pix_fmt[AML_FMT_DST];
	bool capture_matches;
	bool end_capture = s->format_known && ctx->is_cap_streamon;

	ctx->node->stateful_ops->get_format(ctx, &info);
	s->format = info.format;
	capture_matches = !s->format_known && ctx->is_cap_streamon &&
		old->width == s->format.width && old->height == s->format.height &&
		old->num_planes == 2 &&
		old->plane_fmt[0].sizeimage >= s->format.plane_fmt[0].sizeimage &&
		old->plane_fmt[1].sizeimage >= s->format.plane_fmt[1].sizeimage;
	s->crop = info.crop;
	WRITE_ONCE(s->min_capture, info.min_capture);
	s->format_known = true;
	ctx->pix_fmt[AML_FMT_DST] = s->format;
	ctx->compose = s->crop;
	ctx->pic_info.coded_width = s->format.width;
	ctx->pic_info.coded_height = s->format.height;
	WRITE_ONCE(s->wait_capture, !capture_matches);
	/* Publish the reason before LAST can wake a CAPTURE waiter. */
	v4l2_event_queue_fh(&ctx->fh, &event);
	if (end_capture && !stateful_flush(ctx, VB2_BUF_STATE_DONE, true))
		stateful_end_capture(ctx, false);
}

static void stateful_buffered_device_run(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_decode_result result;
	struct aml_vdec_frame frame;
	struct vb2_v4l2_buffer *src, *dst;
	bool drain;
	int ret;

	/* Queue allocation locks precede the stateful lock, as in vb2 queue setup. */
	ret = aml_vdec_get_buffer_pins(ctx);
	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	if (ret || ctx->decode_failed || ctx->dev->dec_hw->faulted) {
		ret = ret ?: -EIO;
		goto error;
	}
	ret = ctx->node->stateful_ops->prepare(ctx);
	if (ret)
		goto error;
	src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	ret = ctx->node->stateful_ops->submit(ctx, src, dst, &result);
	if (ret < 0)
		goto error;
	if (result.format_changed) {
		stateful_source_change(ctx);
		goto finish;
	}
	src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	drain = v4l2_m2m_is_last_draining_src_buf(ctx->m2m_ctx, src);
	if (result.picture) {
		dst = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);
		v4l2_m2m_buf_copy_metadata(src, dst);
		dst->field = V4L2_FIELD_NONE;
		dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
				V4L2_BUF_FLAG_BFRAME);
		dst->flags |= result.flags;
		ctx->node->stateful_ops->queue_capture(ctx, result.slot, dst);
	}
	v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
	src->sequence = ctx->sequence_out++;
	if (drain) {
		if (ctx->node->stateful_ops->get_frame(ctx, true, &frame) > 0) {
			stateful_eos(ctx);
			stateful_output(ctx, &frame, VB2_BUF_STATE_DONE, frame.final);
			stateful_flush(ctx, VB2_BUF_STATE_DONE, true);
			v4l2_m2m_mark_stopped(ctx->m2m_ctx);
		} else {
			stateful_end_capture(ctx, true);
		}
	} else {
		while (ctx->node->stateful_ops->get_frame(ctx, false, &frame) > 0)
			stateful_output(ctx, &frame, VB2_BUF_STATE_DONE, false);
	}
	/* Publish drain events before either queue can report its final buffer. */
	v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
	goto finish;
error:
	ctx->decode_failed = true;
	if (ctx->codec_priv)
		stateful_flush(ctx, VB2_BUF_STATE_ERROR, false);
	src = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
	if (src) {
		src->sequence = ctx->sequence_out++;
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
	}
	vb2_queue_error(v4l2_m2m_get_src_vq(ctx->m2m_ctx));
	vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->m2m_ctx));
finish:
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
	aml_vdec_put_buffer_pins(ctx);
	v4l2_m2m_job_finish(ctx->dev->m2m_dev_dec, ctx->m2m_ctx);
}

static int stateful_buffered_decoder_cmd(struct file *file, void *priv,
					 struct v4l2_decoder_cmd *cmd)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);
	int ret;

	ret = v4l2_m2m_ioctl_try_decoder_cmd(file, priv, cmd);
	if (ret)
		return ret;
	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	if (cmd->cmd == V4L2_DEC_CMD_STOP && !ctx->m2m_ctx->is_draining &&
	    !ctx->m2m_ctx->has_stopped && !v4l2_m2m_next_src_buf(ctx->m2m_ctx)) {
		if (ctx->codec_priv)
			stateful_flush(ctx, VB2_BUF_STATE_DONE, false);
		/* The generic helper supplies LAST after all reordered pictures. */
		if (v4l2_m2m_next_dst_buf(ctx->m2m_ctx)) {
			struct vb2_buffer *vb = &v4l2_m2m_next_dst_buf(ctx->m2m_ctx)->vb2_buf;
			unsigned int i;

			for (i = 0; i < vb->num_planes; i++)
				vb2_set_plane_payload(vb, i, 0);
			stateful_eos(ctx);
		}
	}
	ret = v4l2_m2m_ioctl_decoder_cmd(file, priv, cmd);
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
	return ret;
}

static int stateful_get_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct aml_vdec_ctx *ctx = ctrl_to_dec_ctx(ctrl->handler);

	if (ctrl->id != V4L2_CID_MIN_BUFFERS_FOR_CAPTURE)
		return -EINVAL;
	ctrl->val = READ_ONCE(aml_vdec_stateful(ctx)->min_capture);
	return 0;
}

static int vdec_g_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	if (ctx->node->buffered_capture)
		return stateful_get_selection(ctx, s);
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

	if (ctx->node->buffered_capture)
		return stateful_buffered_decoder_cmd(file, priv, dc);
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
		if (ctx->node->buffered_capture) {
			stateful_last_buffer_done(ctx, vb2_v4l2);
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
		if (ctx->node->buffered_capture)
			stateful_start_capture(ctx);
	}

	return 0;
}

static void vb2ops_vdec_stop_streaming(struct vb2_queue *q)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *src_buf = NULL, *dst_buf = NULL;

	if (ctx->node->buffered_capture)
		stateful_cancel_output(ctx);
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
	dst = aml_vdec_stateful(ctx)->frame.buffer;
	if (!dst)
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

	aml_vdec_stateful(ctx)->frame.buffer = NULL;
	if (ctx->node->buffered_capture)
		stateful_buffered_device_run(ctx);
	else
		aml_vdec_run_pair(ctx);
}

static int stateful_buffered_job_ready(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_stateful_ctx *s = aml_vdec_stateful(ctx);

	if (!ctx->codec_priv || READ_ONCE(aml_vdec_stateful(ctx)->wait_capture))
		return 0;
	if (s->format_known && (!ctx->is_cap_streamon ||
				!v4l2_m2m_num_dst_bufs_ready(ctx->m2m_ctx)))
		return 0;
	return 1;
}

static int stateful_job_ready(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;

	return ctx->node->buffered_capture ? stateful_buffered_job_ready(ctx) : 1;
}

static const struct v4l2_m2m_ops stateful_m2m_ops = {
	.device_run = stateful_device_run,
	.job_ready = stateful_job_ready,
};

static void stateful_context_init(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_stateful_ctx *stateful = aml_vdec_stateful(ctx);

	mutex_init(&stateful->lock);
	stateful->min_capture = 1;
}

static int stateful_run_pair_job(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_decode_result result;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	int ret;

	ret = ctx->node->stateful_ops->submit(ctx, src, dst, &result);
	if (ret < 0)
		return ret;
	if (result.picture)
		ctx->node->stateful_ops->get_frame(ctx, false, &aml_vdec_stateful(ctx)->frame);
	return 0;
}

const struct aml_vdec_interface aml_vdec_stateful_interface = {
	.context_size = sizeof(struct aml_vdec_stateful_ctx),
	.context_init = stateful_context_init,
	.ioctl_ops = &aml_vdec_stateful_ioctl_ops,
	.queue_ops = &aml_vdec_vb2_ops,
	.m2m_ops = &stateful_m2m_ops,
	.controls_init = stateful_ctrls_setup,
	.finish_job = stateful_finish_job,
	.run_job = stateful_run_pair_job,
};

#if IS_ENABLED(CONFIG_VIDEO_AMLOGIC_VDEC_KUNIT_TEST)
#include "aml_vdec_stateful_kunit.c"
#endif
