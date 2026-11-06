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

static const struct aml_vdec_v4l2_ctrl controls[] = {
	{
		.codec_type = CODEC_TYPE_MPEG2,
		.cfg = { .id = V4L2_CID_STATELESS_MPEG2_SEQUENCE },
	}, {
		.codec_type = CODEC_TYPE_MPEG2,
		.cfg = { .id = V4L2_CID_STATELESS_MPEG2_PICTURE },
	}, {
		.codec_type = CODEC_TYPE_MPEG2,
		.cfg = { .id = V4L2_CID_STATELESS_MPEG2_QUANTISATION },
	},
	{
		.codec_type = CODEC_TYPE_H264,
		.cfg = {
			.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
		},
	}, {
		.codec_type = CODEC_TYPE_H264,
		.cfg = {
			.id = V4L2_CID_STATELESS_H264_SPS,
		},
	}, {
		.codec_type = CODEC_TYPE_H264,
		.cfg = {
			.id = V4L2_CID_STATELESS_H264_PPS,
		},
	}, {
		.codec_type = CODEC_TYPE_H264,
		.cfg = {
			.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
		},
	}, {
		.codec_type = CODEC_TYPE_H264,
		.cfg = {
			.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
			.min = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
			.def = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
			.max = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		},
	}, {
		.codec_type = CODEC_TYPE_H264,
		.cfg = {
			.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		},
	}, {
		.codec_type = CODEC_TYPE_H264,
		.cfg = {
			.id = V4L2_CID_STATELESS_H264_START_CODE,
			.min = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
			.def = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
			.max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		},
	}, {
		.codec_type = CODEC_TYPE_H264,
		.cfg = {
			.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			.min = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE,
			.max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
			.def = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
		},
	}
};

static int stateless_ctrls_setup(struct aml_vdec_ctx *ctx)
{
	int i;
	int ctrls_size = ARRAY_SIZE(controls);

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, ctrls_size);
	for (i = 0; i < ctrls_size; i++) {
		if (controls[i].codec_type == CODEC_TYPE_MPEG2 &&
		    !ctx->dev->pvdec_data->has_internal_canvas)
			continue;
		v4l2_ctrl_new_custom(&ctx->ctrl_handler, &controls[i].cfg, NULL);
		if (ctx->ctrl_handler.error) {
			dev_info(&ctx->dev->plat_dev->dev, "add ctrl for (%d) failed%d\n",
				 controls[i].cfg.id, ctx->ctrl_handler.error);
			v4l2_ctrl_handler_free(&ctx->ctrl_handler);
			return ctx->ctrl_handler.error;
		}
	}
	if (ctx->ctrl_handler.error) {
		int error = ctx->ctrl_handler.error;

		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		return error;
	}
	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	return v4l2_ctrl_handler_setup(&ctx->ctrl_handler);
}

static void vb2_ops_vdec_buf_queue(struct vb2_buffer *vb)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int vb2ops_vdec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(q);

	if (ctx->dev->dec_hw->faulted)
		return -EIO;
	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		ctx->sequence_out = 0;
		ctx->is_output_streamon = 1;
		if (aml_vdec_init_dec_inst(ctx) < 0)
			return -EINVAL;
	} else {
		ctx->sequence_cap = 0;
		ctx->is_cap_streamon = 1;
	}

	return 0;
}

static void vb2ops_vdec_stop_streaming(struct vb2_queue *q)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *src_buf = NULL, *dst_buf = NULL;

	aml_vdec_release_instance(ctx);

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

static void vb2ops_vdec_buf_request_complete(struct vb2_buffer *vb)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_handler);
}

static const struct vb2_ops aml_vdec_vb2_ops = {
	.queue_setup = vb2ops_vdec_queue_setup,
	.start_streaming = vb2ops_vdec_start_streaming,
	.stop_streaming = vb2ops_vdec_stop_streaming,

	.buf_cleanup = aml_vdec_buf_cleanup,
	.buf_prepare = vb2ops_vdec_buf_prepare,
	.buf_out_validate = vb2ops_vdec_out_buf_validate,
	.buf_queue = vb2_ops_vdec_buf_queue,
	.buf_finish = vb2_ops_vdec_buf_finish,
	.buf_request_complete = vb2ops_vdec_buf_request_complete,
};

const struct v4l2_ioctl_ops aml_vdec_stateless_ioctl_ops = {
	.vidioc_querycap = vidioc_vdec_querycap,
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

	.vidioc_decoder_cmd = v4l2_m2m_ioctl_stateless_decoder_cmd,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

static void stateless_finish_job(struct aml_vdec_ctx *ctx, enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);

	src->sequence = ctx->sequence_out++;
	if (state == VB2_BUF_STATE_ERROR)
		src->flags &= ~V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
	v4l2_m2m_buf_done_and_job_finish(ctx->dev->m2m_dev_dec, ctx->m2m_ctx, state);
}

static void stateless_device_run(void *priv)
{
	aml_vdec_run_pair(priv);
}

static const struct v4l2_m2m_ops stateless_m2m_ops = {
	.device_run = stateless_device_run,
};

const struct aml_vdec_interface aml_vdec_stateless_interface = {
	.ioctl_ops = &aml_vdec_stateless_ioctl_ops,
	.queue_ops = &aml_vdec_vb2_ops,
	.m2m_ops = &stateless_m2m_ops,
	.controls_init = stateless_ctrls_setup,
	.finish_job = stateless_finish_job,
	.supports_requests = true,
};

static const u32 stateless_formats[] = {
	V4L2_PIX_FMT_H264_SLICE, V4L2_PIX_FMT_MPEG2_SLICE,
	V4L2_PIX_FMT_NV21M, V4L2_PIX_FMT_NV21, V4L2_PIX_FMT_NV12M, V4L2_PIX_FMT_NV12,
};

const struct aml_vdec_node_desc aml_vdec_stateless_node = {
	.interface = &aml_vdec_stateless_interface,
	.formats = stateless_formats,
	.num_formats = ARRAY_SIZE(stateless_formats),
	.default_output = V4L2_PIX_FMT_H264_SLICE,
	.default_capture = V4L2_PIX_FMT_NV12,
};
