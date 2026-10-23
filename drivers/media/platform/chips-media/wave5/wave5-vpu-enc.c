// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave5 series multi-standard codec IP - encoder interface
 *
 * Copyright (C) 2021-2023 CHIPS&MEDIA INC
 */

#include <linux/pm_runtime.h>
#include "wave5-helper.h"

#define VPU_ENC_DEV_NAME "C&M Wave5 VPU encoder"
#define VPU_ENC_DRV_NAME "wave5-enc"

static const struct v4l2_frmsize_stepwise enc_frmsize[FMT_TYPES] = {
	[VPU_FMT_TYPE_CODEC] = {
		.min_width = W5_MIN_ENC_PIC_WIDTH,
		.max_width = W5_MAX_ENC_PIC_WIDTH,
		.step_width = W5_ENC_CODEC_STEP_WIDTH,
		.min_height = W5_MIN_ENC_PIC_HEIGHT,
		.max_height = W5_MAX_ENC_PIC_HEIGHT,
		.step_height = W5_ENC_CODEC_STEP_HEIGHT,
	},
	[VPU_FMT_TYPE_RAW] = {
		.min_width = W5_MIN_ENC_PIC_WIDTH,
		.max_width = W5_MAX_ENC_PIC_WIDTH,
		.step_width = W5_ENC_RAW_STEP_WIDTH,
		.min_height = W5_MIN_ENC_PIC_HEIGHT,
		.max_height = W5_MAX_ENC_PIC_HEIGHT,
		.step_height = W5_ENC_RAW_STEP_HEIGHT,
	},
};

/* HEVC SRC_SIZE is block aligned. Smaller visible sizes use the conformance
 * window, independently of the negotiated raw frame's storage alignment.
 */
static const struct v4l2_frmsize_stepwise hevc_frmsize = {
	.min_width = W5_MIN_ENC_PIC_WIDTH,
	.max_width = W5_MAX_ENC_PIC_WIDTH,
	.step_width = 8,
	.min_height = W5_MIN_ENC_PIC_HEIGHT,
	.max_height = W5_MAX_ENC_PIC_HEIGHT,
	.step_height = 8,
};

static const struct vpu_format enc_fmt_list[FMT_TYPES][MAX_FMTS] = {
	[VPU_FMT_TYPE_CODEC] = {
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_HEVC,
			.v4l2_frmsize = &hevc_frmsize,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_H264,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_CODEC],
		},
	},
	[VPU_FMT_TYPE_RAW] = {
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV420,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV12,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV21,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV420M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV12M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV21M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV422P,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV16,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV61,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV422M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV16M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV61M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUYV,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YVYU,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_UYVY,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_VYUY,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
	}
};

static int switch_state(struct vpu_instance *inst, enum vpu_instance_state state)
{
	switch (state) {
	case VPU_INST_STATE_NONE:
		goto invalid_state_switch;
	case VPU_INST_STATE_OPEN:
		if (inst->state != VPU_INST_STATE_NONE)
			goto invalid_state_switch;
		break;
	case VPU_INST_STATE_INIT_SEQ:
		if (inst->state != VPU_INST_STATE_OPEN && inst->state != VPU_INST_STATE_STOP)
			goto invalid_state_switch;
		break;
	case VPU_INST_STATE_PIC_RUN:
		if (inst->state != VPU_INST_STATE_INIT_SEQ)
			goto invalid_state_switch;
		break;
	case VPU_INST_STATE_STOP:
		break;
	}

	dev_dbg(inst->dev->dev, "Switch state from %s to %s.\n",
		state_to_str(inst->state), state_to_str(state));
	inst->state = state;
	return 0;

invalid_state_switch:
	WARN(1, "Invalid state switch from %s to %s.\n",
	     state_to_str(inst->state), state_to_str(state));
	return -EINVAL;
}

static struct vb2_v4l2_buffer *wave5_find_src(struct v4l2_m2m_ctx *ctx, bool submitted)
{
	struct v4l2_m2m_buffer *buf;
	struct vb2_v4l2_buffer *next = NULL;
	unsigned long flags;

	spin_lock_irqsave(&ctx->out_q_ctx.rdy_spinlock, flags);
	v4l2_m2m_for_each_src_buf(ctx, buf) {
		if (READ_ONCE(wave5_to_vpu_src_buf(&buf->vb)->consumed) == submitted) {
			next = &buf->vb;
			break;
		}
	}
	spin_unlock_irqrestore(&ctx->out_q_ctx.rdy_spinlock, flags);
	return next;
}

static dma_addr_t wave5_vpu_enc_source_addr(struct vb2_buffer *vb, unsigned int plane)
{
	return vb2_dma_contig_plane_dma_addr(vb, plane) + vb->planes[plane].data_offset;
}

static int start_encode(struct vpu_instance *inst, u32 *fail_res)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	int ret;
	struct vb2_v4l2_buffer *src_buf;
	struct vb2_v4l2_buffer *dst_buf;
	struct frame_buffer frame_buf;
	struct enc_param pic_param;
	const struct v4l2_format_info *info;
	u32 stride = inst->src_fmt.plane_fmt[0].bytesperline;
	u32 luma_size = 0;
	u32 chroma_size = 0;

	memset(&pic_param, 0, sizeof(struct enc_param));
	memset(&frame_buf, 0, sizeof(struct frame_buffer));

	info = v4l2_format_info(inst->src_fmt.pixelformat);
	if (!info)
		return -EINVAL;

	if (info->mem_planes == 1) {
		luma_size = stride * inst->src_fmt.height;
		chroma_size = luma_size / (info->hdiv * info->vdiv);
	} else {
		luma_size = inst->src_fmt.plane_fmt[0].sizeimage;
		chroma_size = inst->src_fmt.plane_fmt[1].sizeimage;
	}

	dst_buf = v4l2_m2m_next_dst_buf(m2m_ctx);
	if (!dst_buf) {
		dev_dbg(inst->dev->dev, "%s: No destination buffer found\n", __func__);
		return -EAGAIN;
	}

	pic_param.pic_stream_buffer_addr =
		vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	pic_param.pic_stream_buffer_size =
		vb2_plane_size(&dst_buf->vb2_buf, 0);

	src_buf = wave5_find_src(m2m_ctx, false);
	if (!src_buf) {
		dev_dbg(inst->dev->dev, "%s: No source buffer found\n", __func__);
		if (m2m_ctx->is_draining)
			pic_param.src_end_flag = 1;
		else
			return -EAGAIN;
	} else {
		if (inst->src_fmt.num_planes == 1) {
			frame_buf.buf_y =
				wave5_vpu_enc_source_addr(&src_buf->vb2_buf, 0);
			frame_buf.buf_cb = frame_buf.buf_y + luma_size;
			frame_buf.buf_cr = frame_buf.buf_cb + chroma_size;
		} else if (inst->src_fmt.num_planes == 2) {
			frame_buf.buf_y =
				wave5_vpu_enc_source_addr(&src_buf->vb2_buf, 0);
			frame_buf.buf_cb =
				wave5_vpu_enc_source_addr(&src_buf->vb2_buf, 1);
			frame_buf.buf_cr = frame_buf.buf_cb + chroma_size;
		} else if (inst->src_fmt.num_planes == 3) {
			frame_buf.buf_y =
				wave5_vpu_enc_source_addr(&src_buf->vb2_buf, 0);
			frame_buf.buf_cb =
				wave5_vpu_enc_source_addr(&src_buf->vb2_buf, 1);
			frame_buf.buf_cr =
				wave5_vpu_enc_source_addr(&src_buf->vb2_buf, 2);
		}
		frame_buf.stride = stride;
		pic_param.src_idx = src_buf->vb2_buf.index;
	}

	pic_param.source_frame = &frame_buf;
	pic_param.code_option.implicit_header_encode = 1;
	pic_param.code_option.encode_aud = inst->encode_aud;
	if (!inst->codec_info->enc_info.open_param.rc_enable) {
		const struct enc_wave_param *param =
			&inst->codec_info->enc_info.open_param.wave_param;

		pic_param.force_qp = true;
		pic_param.qp_i = param->intra_qp;
		pic_param.qp_p = param->fixed_qp_p;
		pic_param.qp_b = param->fixed_qp_b;
	}
	if (src_buf)
		pic_param.force_keyframe = atomic_xchg(&inst->force_keyframe, 0);
	ret = wave5_vpu_enc_start_one_frame(inst, &pic_param, fail_res);
	if (ret) {
		if (wave5_vpu_isolated(inst->dev))
			return ret;
		/* A command which was not accepted must not consume the request. */
		if (pic_param.force_keyframe)
			atomic_set(&inst->force_keyframe, 1);
		if (*fail_res == WAVE5_SYSERR_QUEUEING_FAIL)
			return -EINVAL;

		dev_dbg(inst->dev->dev, "%s: wave5_vpu_enc_start_one_frame fail: %d\n",
			__func__, ret);
		if (src_buf)
			src_buf = v4l2_m2m_src_buf_remove_by_idx(m2m_ctx, src_buf->vb2_buf.index);
		if (!src_buf) {
			dev_dbg(inst->dev->dev,
				"%s: Removing src buf failed, the queue is empty\n",
				__func__);
			return -EINVAL;
		}
		dst_buf = v4l2_m2m_dst_buf_remove(m2m_ctx);
		if (!dst_buf) {
			dev_dbg(inst->dev->dev,
				"%s: Removing dst buf failed, the queue is empty\n",
				__func__);
			return -EINVAL;
		}
		switch_state(inst, VPU_INST_STATE_STOP);
		dst_buf->vb2_buf.timestamp = src_buf->vb2_buf.timestamp;
		v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
		return ret;
	} else {
		if (src_buf)
			WRITE_ONCE(wave5_to_vpu_src_buf(src_buf)->consumed, true);
		dev_dbg(inst->dev->dev, "%s: wave5_vpu_enc_start_one_frame success\n",
			__func__);
	}

	return 0;
}

static void wave5_vpu_enc_finish_encode(struct vpu_instance *inst)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	int ret;
	struct enc_output_info enc_output_info;
	struct vb2_v4l2_buffer *src_buf = NULL;
	struct vb2_v4l2_buffer *dst_buf = NULL;

	if (wave5_vpu_isolated(inst->dev) || inst->recovery_error)
		return;
	/* STREAMOFF drains reports itself after M2M cancellation has finished. */
	if (inst->dev->legacy_firmware &&
	    v4l2_m2m_get_curr_priv(inst->v4l2_m2m_dev) != inst)
		return;
	ret = wave5_vpu_enc_get_output_info(inst, &enc_output_info);
	if (ret) {
		dev_dbg(inst->dev->dev,
			"%s: vpu_enc_get_output_info fail: %d  reason: %u | info: %u\n",
			__func__, ret, enc_output_info.error_reason, enc_output_info.warn_info);
		/* A failed result query cannot prove which buffers firmware owns. */
		wave5_vpu_isolate(inst->dev, "frame result query failed", 0);
		return;
	}

	dev_dbg(inst->dev->dev,
		"%s: pic_type %i recon_idx %i src_idx %i pic_byte %u pts %llu\n",
		__func__,  enc_output_info.pic_type, enc_output_info.recon_frame_index,
		enc_output_info.enc_src_idx, enc_output_info.enc_pic_byte, enc_output_info.pts);

	/* Lookahead accepted input, but did not write a coded picture yet. Keep
	 * both queues owned by the driver and let another instance run meanwhile.
	 */
	if (enc_output_info.recon_frame_index == RECON_IDX_FLAG_ENC_DELAY) {
		wave5_vpu_job_finish(inst);
		return;
	}
	if (enc_output_info.enc_src_idx >= 0) {
		src_buf = v4l2_m2m_src_buf_remove_by_idx(m2m_ctx, enc_output_info.enc_src_idx);
		if (!src_buf) {
			dev_warn(inst->dev->dev, "%s: no source buffer found\n", __func__);
		} else {
			inst->timestamp = src_buf->vb2_buf.timestamp;
			v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_DONE);
		}
	}

	dst_buf = v4l2_m2m_dst_buf_remove(m2m_ctx);
	if (enc_output_info.recon_frame_index == RECON_IDX_FLAG_ENC_END) {
		static const struct v4l2_event vpu_event_eos = {
			.type = V4L2_EVENT_EOS
		};

		if (!WARN_ON(!dst_buf)) {
			vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
			dst_buf->field = V4L2_FIELD_NONE;
			v4l2_m2m_last_buffer_done(m2m_ctx, dst_buf);
		}

		v4l2_event_queue_fh(&inst->v4l2_fh, &vpu_event_eos);

		wave5_vpu_job_finish(inst);
	} else {
		if (!dst_buf) {
			dev_warn(inst->dev->dev, "No bitstream buffer.");
			wave5_vpu_job_finish(inst);
			return;
		}

		vb2_set_plane_payload(&dst_buf->vb2_buf, 0, enc_output_info.bitstream_size);

		dst_buf->vb2_buf.timestamp = inst->timestamp;
		dst_buf->field = V4L2_FIELD_NONE;
		dst_buf->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
				    V4L2_BUF_FLAG_BFRAME);
		if (enc_output_info.pic_type == PIC_TYPE_I ||
		    enc_output_info.pic_type == PIC_TYPE_IDR) {
			/* AVC IDR uses NAL type 5; HEVC IDR uses 19 or 20. */
			if ((inst->std == W_AVC_ENC && enc_output_info.enc_vcl_nut == 5) ||
			    (inst->std == W_HEVC_ENC &&
			     (enc_output_info.enc_vcl_nut == 19 ||
			      enc_output_info.enc_vcl_nut == 20)))
				dst_buf->flags |= V4L2_BUF_FLAG_KEYFRAME;
			else
				dst_buf->flags |= V4L2_BUF_FLAG_PFRAME;
		} else if (enc_output_info.pic_type == PIC_TYPE_P) {
			dst_buf->flags |= V4L2_BUF_FLAG_PFRAME;
		} else if (enc_output_info.pic_type == PIC_TYPE_B) {
			dst_buf->flags |= V4L2_BUF_FLAG_BFRAME;
		}

		v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);

		dev_dbg(inst->dev->dev, "%s: frame_cycle %8u\n",
			__func__, enc_output_info.frame_cycle);

		wave5_vpu_job_finish(inst);
	}
}

static int wave5_vpu_enc_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, VPU_ENC_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, VPU_ENC_DRV_NAME, sizeof(cap->card));

	return 0;
}

static int wave5_vpu_enc_enum_framesizes(struct file *f, void *fh, struct v4l2_frmsizeenum *fsize)
{
	const struct vpu_format *vpu_fmt;

	if (fsize->index)
		return -EINVAL;

	vpu_fmt = wave5_find_vpu_fmt(fsize->pixel_format, enc_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt) {
		vpu_fmt = wave5_find_vpu_fmt(fsize->pixel_format, enc_fmt_list[VPU_FMT_TYPE_RAW]);
		if (!vpu_fmt)
			return -EINVAL;
	}

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = fsize->pixel_format == V4L2_PIX_FMT_HEVC ?
		hevc_frmsize : *vpu_fmt->v4l2_frmsize;

	return 0;
}

static int wave5_vpu_enc_enum_fmt_cap(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct vpu_format *vpu_fmt;

	dev_dbg(inst->dev->dev, "%s: index: %u\n", __func__, f->index);

	vpu_fmt = wave5_find_vpu_fmt_by_idx(f->index, enc_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt)
		return -EINVAL;

	f->pixelformat = vpu_fmt->v4l2_pix_fmt;
	f->flags = 0;

	return 0;
}

static int wave5_vpu_enc_try_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct v4l2_frmsize_stepwise *frmsize;
	const struct vpu_format *vpu_fmt;
	int width, height;

	dev_dbg(inst->dev->dev, "%s: fourcc: %u width: %u height: %u num_planes: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.field);

	vpu_fmt = wave5_find_vpu_fmt(f->fmt.pix_mp.pixelformat, enc_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt) {
		width = inst->dst_fmt.width;
		height = inst->dst_fmt.height;
		f->fmt.pix_mp.pixelformat = inst->dst_fmt.pixelformat;
		frmsize = &enc_frmsize[VPU_FMT_TYPE_CODEC];
	} else {
		/* Coded dimensions are read-only on the CAPTURE queue. */
		width = inst->conf_win.left + inst->conf_win.width;
		height = inst->conf_win.top + inst->conf_win.height;
		f->fmt.pix_mp.pixelformat = vpu_fmt->v4l2_pix_fmt;
		frmsize = vpu_fmt->v4l2_frmsize;
	}

	wave5_update_pix_fmt(&f->fmt.pix_mp, VPU_FMT_TYPE_CODEC,
			     width, height, frmsize);
	f->fmt.pix_mp.colorspace = inst->colorspace;
	f->fmt.pix_mp.ycbcr_enc = inst->ycbcr_enc;
	f->fmt.pix_mp.quantization = inst->quantization;
	f->fmt.pix_mp.xfer_func = inst->xfer_func;

	return 0;
}

static int wave5_vpu_enc_s_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	int i, ret;

	if (inst->state != VPU_INST_STATE_NONE ||
	    vb2_is_busy(v4l2_m2m_get_src_vq(inst->v4l2_fh.m2m_ctx)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(inst->v4l2_fh.m2m_ctx)))
		return -EBUSY;

	dev_dbg(inst->dev->dev, "%s: fourcc: %u width: %u height: %u num_planes: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.field);

	ret = wave5_vpu_enc_try_fmt_cap(file, fh, f);
	if (ret)
		return ret;

	inst->std = wave5_to_vpu_std(f->fmt.pix_mp.pixelformat, inst->type);
	if (inst->std == STD_UNKNOWN) {
		dev_warn(inst->dev->dev, "unsupported pixelformat: %.4s\n",
			 (char *)&f->fmt.pix_mp.pixelformat);
		return -EINVAL;
	}

	inst->dst_fmt.width = f->fmt.pix_mp.width;
	inst->dst_fmt.height = f->fmt.pix_mp.height;
	inst->dst_fmt.pixelformat = f->fmt.pix_mp.pixelformat;
	inst->dst_fmt.field = f->fmt.pix_mp.field;
	inst->dst_fmt.flags = f->fmt.pix_mp.flags;
	inst->dst_fmt.num_planes = f->fmt.pix_mp.num_planes;
	for (i = 0; i < inst->dst_fmt.num_planes; i++) {
		inst->dst_fmt.plane_fmt[i].bytesperline = f->fmt.pix_mp.plane_fmt[i].bytesperline;
		inst->dst_fmt.plane_fmt[i].sizeimage = f->fmt.pix_mp.plane_fmt[i].sizeimage;
	}

	return 0;
}

static int wave5_vpu_enc_g_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	int i;

	f->fmt.pix_mp.width = inst->dst_fmt.width;
	f->fmt.pix_mp.height = inst->dst_fmt.height;
	f->fmt.pix_mp.pixelformat = inst->dst_fmt.pixelformat;
	f->fmt.pix_mp.field = inst->dst_fmt.field;
	f->fmt.pix_mp.flags = inst->dst_fmt.flags;
	f->fmt.pix_mp.num_planes = inst->dst_fmt.num_planes;
	for (i = 0; i < f->fmt.pix_mp.num_planes; i++) {
		f->fmt.pix_mp.plane_fmt[i].bytesperline = inst->dst_fmt.plane_fmt[i].bytesperline;
		f->fmt.pix_mp.plane_fmt[i].sizeimage = inst->dst_fmt.plane_fmt[i].sizeimage;
	}

	f->fmt.pix_mp.colorspace = inst->colorspace;
	f->fmt.pix_mp.ycbcr_enc = inst->ycbcr_enc;
	f->fmt.pix_mp.quantization = inst->quantization;
	f->fmt.pix_mp.xfer_func = inst->xfer_func;

	return 0;
}

static int wave5_vpu_enc_enum_fmt_out(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct vpu_format *vpu_fmt;

	dev_dbg(inst->dev->dev, "%s: index: %u\n", __func__, f->index);

	vpu_fmt = wave5_find_vpu_fmt_by_idx(f->index, enc_fmt_list[VPU_FMT_TYPE_RAW]);
	if (!vpu_fmt)
		return -EINVAL;

	f->pixelformat = vpu_fmt->v4l2_pix_fmt;
	f->flags = 0;

	return 0;
}

static int wave5_vpu_enc_try_fmt_out(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct v4l2_frmsize_stepwise *frmsize;
	const struct vpu_format *vpu_fmt;
	int width, height;

	dev_dbg(inst->dev->dev, "%s: fourcc: %u width: %u height: %u num_planes: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.field);

	vpu_fmt = wave5_find_vpu_fmt(f->fmt.pix_mp.pixelformat, enc_fmt_list[VPU_FMT_TYPE_RAW]);
	if (!vpu_fmt) {
		width = inst->src_fmt.width;
		height = inst->src_fmt.height;
		f->fmt.pix_mp.pixelformat = inst->src_fmt.pixelformat;
		frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW];
	} else {
		width = f->fmt.pix_mp.width;
		height = f->fmt.pix_mp.height;
		f->fmt.pix_mp.pixelformat = vpu_fmt->v4l2_pix_fmt;
		frmsize = vpu_fmt->v4l2_frmsize;
	}

	wave5_update_pix_fmt(&f->fmt.pix_mp, VPU_FMT_TYPE_RAW,
			     width, height, frmsize);
	return 0;
}

static int wave5_vpu_enc_s_fmt_out(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct vpu_format *vpu_fmt;
	const struct v4l2_format_info *info;
	int i, ret;

	if (inst->state != VPU_INST_STATE_NONE ||
	    vb2_is_busy(v4l2_m2m_get_src_vq(inst->v4l2_fh.m2m_ctx)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(inst->v4l2_fh.m2m_ctx)))
		return -EBUSY;

	dev_dbg(inst->dev->dev, "%s: fourcc: %u width: %u height: %u num_planes: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.field);

	ret = wave5_vpu_enc_try_fmt_out(file, fh, f);
	if (ret)
		return ret;

	inst->src_fmt.width = f->fmt.pix_mp.width;
	inst->src_fmt.height = f->fmt.pix_mp.height;
	inst->src_fmt.pixelformat = f->fmt.pix_mp.pixelformat;
	inst->src_fmt.field = f->fmt.pix_mp.field;
	inst->src_fmt.flags = f->fmt.pix_mp.flags;
	inst->src_fmt.num_planes = f->fmt.pix_mp.num_planes;
	for (i = 0; i < inst->src_fmt.num_planes; i++) {
		inst->src_fmt.plane_fmt[i].bytesperline = f->fmt.pix_mp.plane_fmt[i].bytesperline;
		inst->src_fmt.plane_fmt[i].sizeimage = f->fmt.pix_mp.plane_fmt[i].sizeimage;
	}

	info = v4l2_format_info(inst->src_fmt.pixelformat);
	if (!info)
		return -EINVAL;

	inst->cbcr_interleave = info->comp_planes == 2;

	switch (inst->src_fmt.pixelformat) {
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV21M:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_NV61M:
		inst->nv21 = true;
		break;
	default:
		inst->nv21 = false;
	}

	inst->colorspace = f->fmt.pix_mp.colorspace;
	inst->ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
	inst->quantization = f->fmt.pix_mp.quantization;
	inst->xfer_func = f->fmt.pix_mp.xfer_func;

	vpu_fmt = wave5_find_vpu_fmt(inst->dst_fmt.pixelformat, enc_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt)
		return -EINVAL;

	wave5_update_pix_fmt(&inst->dst_fmt, VPU_FMT_TYPE_CODEC,
			     f->fmt.pix_mp.width, f->fmt.pix_mp.height,
			     vpu_fmt->v4l2_frmsize);
	inst->conf_win.width = inst->dst_fmt.width;
	inst->conf_win.height = inst->dst_fmt.height;
	inst->conf_win.left = 0;
	inst->conf_win.top = 0;

	return 0;
}

static int wave5_vpu_enc_g_selection(struct file *file, void *fh, struct v4l2_selection *s)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);

	dev_dbg(inst->dev->dev, "%s: type: %u | target: %u\n", __func__, s->type, s->target);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;
	switch (s->target) {
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = inst->src_fmt.width;
		s->r.height = inst->src_fmt.height;
		break;
	case V4L2_SEL_TGT_CROP:
		s->r = inst->conf_win;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int wave5_vpu_enc_s_selection(struct file *file, void *fh, struct v4l2_selection *s)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	struct v4l2_rect rect = s->r;
	u32 width, height;

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	if (s->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;
	if (s->flags & ~(V4L2_SEL_FLAG_GE | V4L2_SEL_FLAG_LE))
		return -EINVAL;
	if (inst->state != VPU_INST_STATE_NONE ||
	    vb2_is_busy(v4l2_m2m_get_src_vq(inst->v4l2_fh.m2m_ctx)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(inst->v4l2_fh.m2m_ctx)))
		return -EBUSY;

	dev_dbg(inst->dev->dev, "%s: V4L2_SEL_TGT_CROP width: %u | height: %u\n",
		__func__, s->r.width, s->r.height);

	/* The 4:2:0 conformance window uses pairs of luma samples. Do not
	 * silently change a requested visible rectangle or its chroma phase.
	 */
	if (rect.left < 0 || rect.top < 0 ||
	    rect.width < W5_MIN_ENC_PIC_WIDTH ||
	    rect.height < W5_MIN_ENC_PIC_HEIGHT ||
	    !IS_ALIGNED(rect.left | rect.top | rect.width | rect.height, 2))
		return -EINVAL;
	if ((u32)rect.left > inst->src_fmt.width || (u32)rect.top > inst->src_fmt.height ||
	    rect.width > inst->src_fmt.width - rect.left ||
	    rect.height > inst->src_fmt.height - rect.top)
		return -ERANGE;

	width = rect.left + rect.width;
	height = rect.top + rect.height;
	if (inst->std == W_HEVC_ENC) {
		width = ALIGN(width, 8);
		height = ALIGN(height, 8);
	}
	if (width > inst->src_fmt.width || height > inst->src_fmt.height)
		return -ERANGE;

	inst->conf_win = rect;
	inst->dst_fmt.width = width;
	inst->dst_fmt.height = height;

	return 0;
}

static int wave5_vpu_enc_encoder_cmd(struct file *file, void *fh, struct v4l2_encoder_cmd *ec)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, fh, ec);
	if (ret)
		return ret;

	if (!wave5_vpu_both_queues_are_streaming(inst))
		return 0;

	switch (ec->cmd) {
	case V4L2_ENC_CMD_STOP:
		if (m2m_ctx->is_draining)
			return -EBUSY;

		if (m2m_ctx->has_stopped)
			return 0;

		m2m_ctx->last_src_buf = v4l2_m2m_last_src_buf(m2m_ctx);
		m2m_ctx->is_draining = true;

		v4l2_m2m_try_schedule(m2m_ctx);
		break;
	case V4L2_ENC_CMD_START:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int wave5_vpu_enc_g_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);

	dev_dbg(inst->dev->dev, "%s: type: %u\n", __func__, a->type);

	if (a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT && a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;
	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.output.timeperframe.numerator = 1;
	a->parm.output.timeperframe.denominator = inst->frame_rate;

	dev_dbg(inst->dev->dev, "%s: numerator: %u | denominator: %u\n",
		__func__, a->parm.output.timeperframe.numerator,
		a->parm.output.timeperframe.denominator);

	return 0;
}

static int wave5_vpu_enc_s_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);

	dev_dbg(inst->dev->dev, "%s: type: %u\n", __func__, a->type);

	if (a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT && a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	if (inst->state != VPU_INST_STATE_NONE)
		return -EBUSY;

	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	if (a->parm.output.timeperframe.denominator && a->parm.output.timeperframe.numerator) {
		inst->frame_rate = max_t(u32, 1, a->parm.output.timeperframe.denominator /
					a->parm.output.timeperframe.numerator);
	}
	/* This command interface currently negotiates whole frames per second. */
	a->parm.output.timeperframe.numerator = 1;
	a->parm.output.timeperframe.denominator = inst->frame_rate;

	dev_dbg(inst->dev->dev, "%s: numerator: %u | denominator: %u\n",
		__func__, a->parm.output.timeperframe.numerator,
		a->parm.output.timeperframe.denominator);

	return 0;
}

static const struct v4l2_ioctl_ops wave5_vpu_enc_ioctl_ops = {
	.vidioc_querycap = wave5_vpu_enc_querycap,
	.vidioc_enum_framesizes = wave5_vpu_enc_enum_framesizes,

	.vidioc_enum_fmt_vid_cap	= wave5_vpu_enc_enum_fmt_cap,
	.vidioc_s_fmt_vid_cap_mplane = wave5_vpu_enc_s_fmt_cap,
	.vidioc_g_fmt_vid_cap_mplane = wave5_vpu_enc_g_fmt_cap,
	.vidioc_try_fmt_vid_cap_mplane = wave5_vpu_enc_try_fmt_cap,

	.vidioc_enum_fmt_vid_out	= wave5_vpu_enc_enum_fmt_out,
	.vidioc_s_fmt_vid_out_mplane = wave5_vpu_enc_s_fmt_out,
	.vidioc_g_fmt_vid_out_mplane = wave5_vpu_g_fmt_out,
	.vidioc_try_fmt_vid_out_mplane = wave5_vpu_enc_try_fmt_out,

	.vidioc_g_selection = wave5_vpu_enc_g_selection,
	.vidioc_s_selection = wave5_vpu_enc_s_selection,

	.vidioc_g_parm = wave5_vpu_enc_g_parm,
	.vidioc_s_parm = wave5_vpu_enc_s_parm,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = wave5_vpu_enc_encoder_cmd,

	.vidioc_subscribe_event = wave5_vpu_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int wave5_set_qp_limits(struct enc_wave_param *param, struct v4l2_ctrl *ctrl)
{
	struct v4l2_ctrl **limits = ctrl->cluster;
	u32 minimum[3], maximum[3];
	unsigned int i;

	/* The codec-wide bounds also constrain each picture-type interval. */
	for (i = 0; i < 3; i++) {
		minimum[i] = max(limits[0]->val, limits[2 + 2 * i]->val);
		maximum[i] = min(limits[1]->val, limits[3 + 2 * i]->val);
		if (minimum[i] > maximum[i])
			return -EINVAL;
	}
	param->min_qp_i = minimum[0];
	param->max_qp_i = maximum[0];
	param->min_qp_p = minimum[1];
	param->max_qp_p = maximum[1];
	param->min_qp_b = minimum[2];
	param->max_qp_b = maximum[2];
	return 0;
}

static int wave5_vpu_enc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vpu_instance *inst = wave5_ctrl_to_vpu_inst(ctrl);
	struct enc_wave_param *param = &inst->enc_param;

	if ((ctrl->id >= V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP &&
	     ctrl->id <= V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L6_BR) ||
	    (ctrl->id >= V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_MIN_QP &&
	     ctrl->id <= V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_MAX_QP))
		param = &inst->hevc_param;

	/* Sequence controls cannot be applied to an already created instance. */
	if (inst->state != VPU_INST_STATE_NONE &&
	    ctrl->id != V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME &&
	    ctrl->id != V4L2_CID_MIN_BUFFERS_FOR_OUTPUT)
		return -EBUSY;

	dev_dbg(inst->dev->dev, "%s: name: %s | value: %d\n", __func__, ctrl->name, ctrl->val);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_AU_DELIMITER:
		inst->encode_aud = ctrl->val;
		break;
	case V4L2_CID_HFLIP:
		inst->mirror_direction = (inst->mirror_direction & ~2) | (ctrl->val << 1);
		break;
	case V4L2_CID_VFLIP:
		inst->mirror_direction = (inst->mirror_direction & ~1) | ctrl->val;
		break;
	case V4L2_CID_ROTATE:
		inst->rot_angle = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_VBV_SIZE:
		inst->vbv_buf_size = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE:
		inst->h264_cpb_size = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEADER_MODE:
		if (ctrl->val != V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME)
			return -EINVAL;
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_BITRATE_MODE_VBR:
		case V4L2_MPEG_VIDEO_BITRATE_MODE_CBR:
		case V4L2_MPEG_VIDEO_BITRATE_MODE_CQ:
			inst->rc_mode = ctrl->val;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		inst->bit_rate = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY:
		inst->quality = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_BACKGROUND_DETECTION:
		param->bg_detection = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		param->avc_idr_period = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
		inst->b_frames = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD:
		inst->intra_refresh_period = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE:
		if (ctrl->val != V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE_CYCLIC)
			return -EINVAL;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		atomic_set(&inst->force_keyframe, 1);
		break;
	case V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE:
		param->independ_slice_mode = ctrl->val;
		param->avc_slice_mode = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_MB:
		param->independ_slice_mode_arg = ctrl->val;
		param->avc_slice_arg = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE:
		inst->rc_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE:
		param->mb_level_rc_enable = ctrl->val;
		param->cu_level_rc_enable = ctrl->val;
		param->hvs_qp_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
		param->en_still_picture = 0;
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN:
			param->profile = HEVC_PROFILE_MAIN;
			param->internal_bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE:
			param->profile = HEVC_PROFILE_STILLPICTURE;
			param->en_still_picture = 1;
			param->internal_bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10:
			param->profile = HEVC_PROFILE_MAIN10;
			param->internal_bit_depth = 10;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LEVEL:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_1:
			param->level = 10 * 3;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_2:
			param->level = 20 * 3;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1:
			param->level = 21 * 3;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_3:
			param->level = 30 * 3;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1:
			param->level = 31 * 3;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_4:
			param->level = 40 * 3;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1:
			param->level = 41 * 3;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_5:
			param->level = 50 * 3;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1:
			param->level = 51 * 3;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2:
			param->level = 52 * 3;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP:
	case V4L2_CID_MPEG_VIDEO_H264_MIN_QP:
		return wave5_set_qp_limits(param, ctrl);
	case V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP:
		param->intra_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP:
	case V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP:
		param->fixed_qp_p = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP:
	case V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP:
		param->fixed_qp_b = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_TIER:
		param->tier = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_INTRA_PU_SPLIT:
		param->intra_nx_n_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED:
			param->disable_deblk = 1;
			param->sao_enable = 0;
			param->lf_cross_slice_boundary_enable = 0;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_ENABLED:
			param->disable_deblk = 0;
			param->sao_enable = 1;
			param->lf_cross_slice_boundary_enable = 1;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY:
			param->disable_deblk = 0;
			param->sao_enable = 1;
			param->lf_cross_slice_boundary_enable = 0;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2:
		param->beta_offset_div2 = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2:
		param->tc_offset_div2 = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_TYPE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_HEVC_REFRESH_NONE:
			param->decoding_refresh_type = 0;
			break;
		case V4L2_MPEG_VIDEO_HEVC_REFRESH_CRA:
			param->decoding_refresh_type = 1;
			break;
		case V4L2_MPEG_VIDEO_HEVC_REFRESH_IDR:
			param->decoding_refresh_type = 2;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_PERIOD:
		param->intra_period = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LOSSLESS_CU:
		param->lossless_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_CONST_INTRA_PRED:
		param->const_intra_pred_flag = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_WAVEFRONT:
		param->wpp_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_STRONG_SMOOTHING:
		param->strong_intra_smooth_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_MAX_NUM_MERGE_MV_MINUS1:
		param->max_num_merge = ctrl->val + 1;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_TMV_PREDICTION:
		param->tmvp_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		param->constraint_set1_flag =
			ctrl->val == V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
		case V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE:
			param->profile = H264_PROFILE_BP;
			param->internal_bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
			param->profile = H264_PROFILE_MP;
			param->internal_bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED:
			param->profile = H264_PROFILE_EXTENDED;
			param->internal_bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
			param->profile = H264_PROFILE_HP;
			param->internal_bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10:
			param->profile = H264_PROFILE_HIGH10;
			param->internal_bit_depth = 10;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422:
			param->profile = H264_PROFILE_HIGH422;
			param->internal_bit_depth = 10;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE:
			param->profile = H264_PROFILE_HIGH444;
			param->internal_bit_depth = 10;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:
			param->level = 10;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1B:
			param->level = 9;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:
			param->level = 11;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:
			param->level = 12;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:
			param->level = 13;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:
			param->level = 20;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:
			param->level = 21;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:
			param->level = 22;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
			param->level = 30;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
			param->level = 31;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
			param->level = 32;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
			param->level = 40;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:
			param->level = 41;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
			param->level = 42;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_5_0:
			param->level = 50;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_5_1:
			param->level = 51;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP:
		param->intra_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_DISABLED:
			param->disable_deblk = 1;
			param->lf_cross_slice_boundary_enable = 1;
			break;
		case V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_ENABLED:
			param->disable_deblk = 0;
			param->lf_cross_slice_boundary_enable = 1;
			break;
		case V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY:
			param->disable_deblk = 0;
			param->lf_cross_slice_boundary_enable = 0;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA:
		param->beta_offset_div2 = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA:
		param->tc_offset_div2 = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM:
		param->transform8x8_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_CONSTRAINED_INTRA_PREDICTION:
		param->const_intra_pred_flag = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_CHROMA_QP_INDEX_OFFSET:
		param->chroma_cb_qp_offset = ctrl->val;
		param->chroma_cr_qp_offset = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_I_PERIOD:
		param->intra_period = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE:
		param->entropy_coding_mode = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR:
		param->forced_idr_header_enable = ctrl->val;
		break;
	case V4L2_CID_MIN_BUFFERS_FOR_OUTPUT:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops wave5_vpu_enc_ctrl_ops = {
	.s_ctrl = wave5_vpu_enc_s_ctrl,
};

static int wave5_vpu_enc_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
				     unsigned int *num_planes, unsigned int sizes[],
				     struct device *alloc_devs[])
{
	struct vpu_instance *inst = vb2_get_drv_priv(q);
	struct v4l2_pix_format_mplane inst_format =
		(q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ? inst->src_fmt : inst->dst_fmt;
	unsigned int i;

	dev_dbg(inst->dev->dev, "%s: num_buffers: %u | num_planes: %u | type: %u\n", __func__,
		*num_buffers, *num_planes, q->type);

	if (*num_planes) {
		if (inst_format.num_planes != *num_planes)
			return -EINVAL;

		for (i = 0; i < *num_planes; i++) {
			if (sizes[i] < inst_format.plane_fmt[i].sizeimage)
				return -EINVAL;
		}
	} else {
		*num_planes = inst_format.num_planes;
		for (i = 0; i < *num_planes; i++) {
			sizes[i] = inst_format.plane_fmt[i].sizeimage;
			dev_dbg(inst->dev->dev, "%s: size[%u]: %u\n", __func__, i, sizes[i]);
		}
	}

	dev_dbg(inst->dev->dev, "%s: size: %u\n", __func__, sizes[0]);

	return 0;
}

static int wave5_vpu_enc_buf_prepare(struct vb2_buffer *vb)
{
	struct vpu_instance *inst = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int i;

	if (vb->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return 0;

	/* Imported planes may share an allocation with different data offsets. */
	for (i = 0; i < inst->src_fmt.num_planes; i++) {
		u32 offset = vb->planes[i].data_offset;
		u32 required = inst->src_fmt.plane_fmt[i].sizeimage;

		if (offset > vb2_plane_size(vb, i) ||
		    required > vb2_plane_size(vb, i) - offset ||
		    vb2_get_plane_payload(vb, i) < offset ||
		    required > vb2_get_plane_payload(vb, i) - offset)
			return -EINVAL;
	}

	return 0;
}

static void wave5_vpu_enc_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vpu_instance *inst = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;

	dev_dbg(inst->dev->dev, "%s: type: %4u index: %4u size: ([0]=%4lu, [1]=%4lu, [2]=%4lu)\n",
		__func__, vb->type, vb->index, vb2_plane_size(&vbuf->vb2_buf, 0),
		vb2_plane_size(&vbuf->vb2_buf, 1), vb2_plane_size(&vbuf->vb2_buf, 2));

	if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		WRITE_ONCE(wave5_to_vpu_src_buf(vbuf)->consumed, false);
		vbuf->sequence = inst->queued_src_buf_num++;
	} else {
		vbuf->sequence = inst->queued_dst_buf_num++;
	}

	v4l2_m2m_buf_queue(m2m_ctx, vbuf);
}

static void wave5_set_intra_refresh(struct enc_wave_param *param, u32 height,
				  u32 period, bool hevc)
{
	u32 rows = DIV_ROUND_UP(height, hevc ? 64 : 16);
	u32 refresh_rows = period ? DIV_ROUND_UP(rows, period) : 0;

	/* Firmware takes rows per frame. Round up to finish within the requested
	 * period, including partial bottom rows, without enabling the other codec.
	 */
	param->intra_mb_refresh_mode = !hevc && period ? REFRESH_MB_MODE_CTU_ROWS : 0;
	param->intra_mb_refresh_arg = !hevc ? refresh_rows : 0;
	param->intra_refresh_mode = hevc && period ? REFRESH_MODE_CTU_ROWS : 0;
	param->intra_refresh_arg = hevc ? refresh_rows : 0;
}

static void wave5_set_enc_picture_size(struct enc_open_param *open_param,
				       struct vpu_instance *inst)
{
	/* SRC_SIZE must remain block aligned even when the visible rectangle is
	 * not. Padding comes from the negotiated OUTPUT storage, never beyond it.
	 */
	open_param->pic_width = inst->conf_win.left + inst->conf_win.width;
	open_param->pic_height = inst->conf_win.top + inst->conf_win.height;
	if (inst->std == W_HEVC_ENC) {
		open_param->pic_width = ALIGN(open_param->pic_width, 8);
		open_param->pic_height = ALIGN(open_param->pic_height, 8);
	}
	open_param->wave_param.conf_win_left = inst->conf_win.left;
	open_param->wave_param.conf_win_top = inst->conf_win.top;
	open_param->wave_param.conf_win_right = open_param->pic_width -
		inst->conf_win.left - inst->conf_win.width;
	open_param->wave_param.conf_win_bot = open_param->pic_height -
		inst->conf_win.top - inst->conf_win.height;
}

static int wave5_set_vbv_size(struct enc_open_param *open_param,
			      struct vpu_instance *inst)
{
	u32 size = inst->std == W_AVC_ENC ? inst->h264_cpb_size : inst->vbv_buf_size;
	u64 duration;

	/* The userspace unit is KiB; firmware expects milliseconds at target bps. */
	if (!size) {
		open_param->vbv_buffer_size =
			inst->rc_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR ? 3000 : 1000;
		return 0;
	}
	if (!inst->bit_rate)
		return -EINVAL;
	duration = DIV_ROUND_UP_ULL((u64)size * 8192000, inst->bit_rate);
	if (duration < MIN_VBV_BUFFER_SIZE || duration > MAX_VBV_BUFFER_SIZE)
		return -ERANGE;
	open_param->vbv_buffer_size = duration;
	return 0;
}

static void wave5_set_constant_quality(struct enc_wave_param *param,
				       struct vpu_instance *inst)
{
	u32 qp;

	if (inst->rc_mode != V4L2_MPEG_VIDEO_BITRATE_MODE_CQ)
		return;
	/* Normalize the standard quality scale to inverse QP, with RC disabled. */
	qp = DIV_ROUND_CLOSEST((100 - inst->quality) * 51, 99);
	param->intra_qp = qp;
	param->fixed_qp_p = qp;
	param->fixed_qp_b = qp;
}

static int wave5_set_enc_openparam(struct enc_open_param *open_param,
				   struct vpu_instance *inst)
{
	struct enc_wave_param input = inst->std == W_HEVC_ENC ?
		inst->hevc_param : inst->enc_param;
	const struct enc_wave_param *common = &inst->enc_param;
	const struct v4l2_format_info *info;
	int ret;

	info = v4l2_format_info(inst->src_fmt.pixelformat);
	if (!info)
		return -EINVAL;

	if (info->hdiv == 2 && info->vdiv == 1)
		open_param->src_format = FORMAT_422;
	else
		open_param->src_format = FORMAT_420;

	switch (info->format) {
	case V4L2_PIX_FMT_YUYV:
		open_param->packed_format = PACKED_YUYV;
		break;
	case V4L2_PIX_FMT_YVYU:
		open_param->packed_format = PACKED_YVYU;
		break;
	case V4L2_PIX_FMT_UYVY:
		open_param->packed_format = PACKED_UYVY;
		break;
	case V4L2_PIX_FMT_VYUY:
		open_param->packed_format = PACKED_VYUY;
		break;
	default:
		break;
	}
	/* The fixed IBPBP preset has one reordered B picture between P pictures. */
	open_param->wave_param.gop_preset_idx = inst->b_frames ?
		PRESET_IDX_IBPBP : PRESET_IDX_IPP_SINGLE;
	if (inst->b_frames &&
	    ((inst->std == W_AVC_ENC && input.profile == H264_PROFILE_BP) ||
	     (inst->std == W_HEVC_ENC && input.en_still_picture)))
		return -EINVAL;
	open_param->wave_param.hvs_qp_scale = 2;
	open_param->wave_param.hvs_max_delta_qp = 10;
	open_param->wave_param.skip_intra_trans = 1;
	open_param->wave_param.intra_nx_n_enable = input.intra_nx_n_enable;
	open_param->wave_param.nr_intra_weight_y = 7;
	open_param->wave_param.nr_intra_weight_cb = 7;
	open_param->wave_param.nr_intra_weight_cr = 7;
	open_param->wave_param.nr_inter_weight_y = 4;
	open_param->wave_param.nr_inter_weight_cb = 4;
	open_param->wave_param.nr_inter_weight_cr = 4;
	open_param->wave_param.rdo_skip = 1;
	open_param->wave_param.lambda_scaling_enable = 1;

	open_param->line_buf_int_en = true;
	wave5_set_enc_picture_size(open_param, inst);
	open_param->frame_rate_info = inst->frame_rate;
	open_param->rc_enable = inst->rc_enable &&
		inst->rc_mode != V4L2_MPEG_VIDEO_BITRATE_MODE_CQ;
	if (open_param->rc_enable) {
		open_param->wave_param.initial_rc_qp = -1;
		open_param->wave_param.rc_weight_param = 16;
		open_param->wave_param.rc_weight_buf = 128;
	}
	open_param->wave_param.mb_level_rc_enable = common->mb_level_rc_enable;
	open_param->wave_param.cu_level_rc_enable = common->cu_level_rc_enable;
	open_param->wave_param.hvs_qp_enable = common->hvs_qp_enable;
	open_param->bit_rate = inst->bit_rate;
	if (open_param->rc_enable) {
		ret = wave5_set_vbv_size(open_param, inst);
		if (ret)
			return ret;
	}
	open_param->wave_param.profile = input.profile;
	open_param->wave_param.en_still_picture = input.en_still_picture;
	open_param->wave_param.level = input.level;
	open_param->wave_param.tier = input.tier;
	open_param->wave_param.internal_bit_depth = input.internal_bit_depth;
	open_param->wave_param.intra_qp = input.intra_qp;
	open_param->wave_param.fixed_qp_p = input.fixed_qp_p;
	open_param->wave_param.fixed_qp_b = input.fixed_qp_b;
	wave5_set_constant_quality(&open_param->wave_param, inst);
	open_param->wave_param.min_qp_i = input.min_qp_i;
	open_param->wave_param.max_qp_i = input.max_qp_i;
	open_param->wave_param.min_qp_p = input.min_qp_p;
	open_param->wave_param.max_qp_p = input.max_qp_p;
	open_param->wave_param.min_qp_b = input.min_qp_b;
	open_param->wave_param.max_qp_b = input.max_qp_b;
	open_param->wave_param.disable_deblk = input.disable_deblk;
	open_param->wave_param.sao_enable = input.sao_enable;
	open_param->wave_param.lf_cross_slice_boundary_enable =
		input.lf_cross_slice_boundary_enable;
	open_param->wave_param.tc_offset_div2 = input.tc_offset_div2;
	open_param->wave_param.beta_offset_div2 = input.beta_offset_div2;
	open_param->wave_param.decoding_refresh_type = input.decoding_refresh_type;
	open_param->wave_param.intra_period = input.intra_period;
	open_param->wave_param.bg_detection = common->bg_detection;
	if (inst->std == W_HEVC_ENC) {
		if (input.intra_period == 0) {
			open_param->wave_param.decoding_refresh_type = DEC_REFRESH_TYPE_IDR;
			open_param->wave_param.intra_period = common->avc_idr_period;
		}
	} else {
		open_param->wave_param.constraint_set1_flag = input.constraint_set1_flag;
		open_param->wave_param.avc_idr_period = input.avc_idr_period;
	}
	open_param->wave_param.entropy_coding_mode = input.entropy_coding_mode;
	open_param->wave_param.lossless_enable = input.lossless_enable;
	open_param->wave_param.const_intra_pred_flag = input.const_intra_pred_flag;
	open_param->wave_param.wpp_enable = input.wpp_enable;
	open_param->wave_param.strong_intra_smooth_enable = input.strong_intra_smooth_enable;
	open_param->wave_param.max_num_merge = input.max_num_merge;
	open_param->wave_param.tmvp_enable = input.tmvp_enable;
	open_param->wave_param.transform8x8_enable = input.transform8x8_enable;
	open_param->wave_param.chroma_cb_qp_offset = input.chroma_cb_qp_offset;
	open_param->wave_param.chroma_cr_qp_offset = input.chroma_cr_qp_offset;
	open_param->wave_param.independ_slice_mode = common->independ_slice_mode;
	/* V4L2 counts 16x16 macroblocks; HEVC firmware counts 64x64 CTUs. */
	open_param->wave_param.independ_slice_mode_arg =
		DIV_ROUND_UP(common->independ_slice_mode_arg, 16);
	open_param->wave_param.avc_slice_mode = common->avc_slice_mode;
	open_param->wave_param.avc_slice_arg = common->avc_slice_arg;
	wave5_set_intra_refresh(&open_param->wave_param, inst->conf_win.height,
			       inst->intra_refresh_period, inst->std == W_HEVC_ENC);
	open_param->wave_param.forced_idr_header_enable = common->forced_idr_header_enable;

	return 0;
}

static int initialize_sequence(struct vpu_instance *inst)
{
	struct enc_initial_info initial_info;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = wave5_vpu_enc_issue_seq_init(inst);
	if (ret) {
		dev_err(inst->dev->dev, "%s: wave5_vpu_enc_issue_seq_init, fail: %d\n",
			__func__, ret);
		return ret;
	}

	if (wave5_vpu_wait_interrupt(inst, VPU_ENC_TIMEOUT) < 0) {
		dev_err(inst->dev->dev, "%s: wave5_vpu_wait_interrupt failed\n", __func__);
		return -EINVAL;
	}

	ret = wave5_vpu_enc_complete_seq_init(inst, &initial_info);
	if (ret)
		return ret;

	dev_dbg(inst->dev->dev, "%s: min_frame_buffer: %u | min_source_buffer: %u\n",
		__func__, initial_info.min_frame_buffer_count,
		initial_info.min_src_frame_count);
	inst->min_src_buf_count = initial_info.min_src_frame_count +
				  WAVE521_COMMAND_QUEUE_DEPTH;

	ctrl = v4l2_ctrl_find(&inst->v4l2_ctrl_hdl,
			      V4L2_CID_MIN_BUFFERS_FOR_OUTPUT);
	if (ctrl)
		v4l2_ctrl_s_ctrl(ctrl, inst->min_src_buf_count);

	inst->fbc_buf_count = initial_info.min_frame_buffer_count;

	return 0;
}

static int prepare_fb(struct vpu_instance *inst)
{
	u32 fb_stride = ALIGN(inst->dst_fmt.width, 32);
	u32 fb_height = ALIGN(inst->dst_fmt.height, 32);
	int i, ret = 0;

	/* Instance cleanup releases these only after enc_close stops firmware. */
	for (i = 0; i < inst->fbc_buf_count; i++) {
		u32 luma_size = fb_stride * fb_height;
		u32 chroma_size = ALIGN(fb_stride / 2, 16) * fb_height;

		inst->frame_vbuf[i].size = luma_size + chroma_size;
		ret = wave5_vdi_allocate_dma_memory(inst->dev, &inst->frame_vbuf[i]);
		if (ret < 0) {
			dev_err(inst->dev->dev, "%s: failed to allocate FBC buffer %zu\n",
				__func__, inst->frame_vbuf[i].size);
			return ret;
		}

		inst->frame_buf[i].buf_y = inst->frame_vbuf[i].daddr;
		inst->frame_buf[i].buf_cb = (dma_addr_t)-1;
		inst->frame_buf[i].buf_cr = (dma_addr_t)-1;
		inst->frame_buf[i].update_fb_info = true;
		inst->frame_buf[i].size = inst->frame_vbuf[i].size;
	}

	ret = wave5_vpu_enc_register_frame_buffer(inst, inst->fbc_buf_count, fb_stride,
						  fb_height, COMPRESSED_FRAME_MAP);
	if (ret) {
		dev_err(inst->dev->dev,
			"%s: wave5_vpu_enc_register_frame_buffer, fail: %d\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

static int wave5_vpu_enc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vpu_instance *inst = vb2_get_drv_priv(q);
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	int ret = 0;

	if (wave5_vpu_isolated(inst->dev) || inst->recovery_error) {
		wave5_return_bufs(q, VB2_BUF_STATE_QUEUED);
		return -EIO;
	}
	pm_runtime_resume_and_get(inst->dev->dev);
	v4l2_m2m_update_start_streaming_state(m2m_ctx, q);

	if (inst->state == VPU_INST_STATE_NONE &&
	    (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE || m2m_ctx->out_q_ctx.q.streaming)) {
		struct enc_open_param open_param;

		memset(&open_param, 0, sizeof(struct enc_open_param));

		ret = wave5_set_enc_openparam(&open_param, inst);
		if (ret) {
			dev_dbg(inst->dev->dev, "%s: wave5_set_enc_openparam, fail: %d\n",
				__func__, ret);
			goto return_buffers;
		}

		ret = wave5_vpu_enc_open(inst, &open_param);
		if (ret) {
			dev_dbg(inst->dev->dev, "%s: wave5_vpu_enc_open, fail: %d\n",
				__func__, ret);
			goto return_buffers;
		}

		if (inst->mirror_direction) {
			wave5_vpu_enc_give_command(inst, ENABLE_MIRRORING, NULL);
			wave5_vpu_enc_give_command(inst, SET_MIRROR_DIRECTION,
						   &inst->mirror_direction);
		}
		if (inst->rot_angle) {
			wave5_vpu_enc_give_command(inst, ENABLE_ROTATION, NULL);
			wave5_vpu_enc_give_command(inst, SET_ROTATION_ANGLE, &inst->rot_angle);
		}

		ret = switch_state(inst, VPU_INST_STATE_OPEN);
		if (ret)
			goto return_buffers;
	}
	if (inst->state == VPU_INST_STATE_OPEN &&
	    (m2m_ctx->cap_q_ctx.q.streaming || q->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)) {
		ret = initialize_sequence(inst);
		if (ret) {
			dev_warn(inst->dev->dev, "Sequence not found: %d\n", ret);
			goto return_buffers;
		}
		ret = switch_state(inst, VPU_INST_STATE_INIT_SEQ);
		if (ret)
			goto return_buffers;
		/*
		 * The sequence must be analyzed first to calculate the proper
		 * size of the auxiliary buffers.
		 */
		ret = prepare_fb(inst);
		if (ret) {
			dev_warn(inst->dev->dev, "Framebuffer preparation, fail: %d\n", ret);
			goto return_buffers;
		}

		ret = switch_state(inst, VPU_INST_STATE_PIC_RUN);
	}
	if (ret)
		goto return_buffers;

	pm_runtime_put_autosuspend(inst->dev->dev);
	return 0;
return_buffers:
	wave5_return_bufs(q, VB2_BUF_STATE_QUEUED);
	pm_runtime_put_autosuspend(inst->dev->dev);
	return ret;
}

static int wave5_stop_retained_sources(struct vpu_instance *inst)
{
	struct v4l2_m2m_ctx *ctx = inst->v4l2_fh.m2m_ctx;
	struct v4l2_m2m_buffer *buf;
	unsigned long flags;
	unsigned int i;
	u32 fail_res;
	int ret;

	if (wave5_vpu_isolated(inst->dev) || inst->recovery_error ||
	    !wave5_find_src(inst->v4l2_fh.m2m_ctx, true))
		return 0;
	/* An idle command queue does not release lookahead DMA references.
	 * Destroy the firmware context before either queue returns its buffers.
	 */
	ret = wave5_vpu_enc_close(inst, &fail_res);
	if (ret) {
		wave5_vpu_isolate(inst->dev, "retained source destroy failed", 0);
		return ret;
	}
	for (i = 0; i < inst->fbc_buf_count; i++)
		wave5_vpu_dec_reset_framebuffer(inst, i);
	inst->fbc_buf_count = 0;
	memset(&inst->codec_info->enc_info, 0, sizeof(inst->codec_info->enc_info));
	inst->codec_info->enc_info.destroyed = true;
	/* A later STREAMON may rebuild the context on this same file. Sources
	 * retained on a still-streaming OUTPUT queue can then be submitted anew.
	 */
	spin_lock_irqsave(&ctx->out_q_ctx.rdy_spinlock, flags);
	v4l2_m2m_for_each_src_buf(ctx, buf)
		WRITE_ONCE(wave5_to_vpu_src_buf(&buf->vb)->consumed, false);
	spin_unlock_irqrestore(&ctx->out_q_ctx.rdy_spinlock, flags);
	inst->state = VPU_INST_STATE_NONE;
	return ret;
}

static void streamoff_output(struct vpu_instance *inst, struct vb2_queue *q)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct vb2_v4l2_buffer *buf;

	while ((buf = v4l2_m2m_src_buf_remove(m2m_ctx))) {
		dev_dbg(inst->dev->dev, "%s: buf type %4u | index %4u\n",
			__func__, buf->vb2_buf.type, buf->vb2_buf.index);
		v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	}
}

static void streamoff_capture(struct vpu_instance *inst, struct vb2_queue *q)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct vb2_v4l2_buffer *buf;

	while ((buf = v4l2_m2m_dst_buf_remove(m2m_ctx))) {
		dev_dbg(inst->dev->dev, "%s: buf type %4u | index %4u\n",
			__func__, buf->vb2_buf.type, buf->vb2_buf.index);
		vb2_set_plane_payload(&buf->vb2_buf, 0, 0);
		v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	}

	v4l2_m2m_clear_state(m2m_ctx);
}

static void wave5_vpu_enc_stop_streaming(struct vb2_queue *q)
{
	struct vpu_instance *inst = vb2_get_drv_priv(q);
	unsigned int timeout = inst->dev->legacy_firmware ? 5000 : VPU_ENC_TIMEOUT;
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout);

	/*
	 * Note that we don't need m2m_ctx->next_buf_last for this driver, so we
	 * don't call v4l2_m2m_update_stop_streaming_state().
	 */

	dev_dbg(inst->dev->dev, "%s: type: %u\n", __func__, q->type);
	pm_runtime_resume_and_get(inst->dev->dev);

	if (!inst->recovery_error && wave5_vpu_both_queues_are_streaming(inst))
		switch_state(inst, VPU_INST_STATE_STOP);

	while (!wave5_vpu_isolated(inst->dev) && !inst->recovery_error) {
		struct queue_status_info q_status;
		struct enc_output_info enc_output_info;

		wave5_vpu_enc_give_command(inst, ENC_GET_QUEUE_STATUS, &q_status);

		if (q_status.report_queue_count == 0)
			break;
		if (time_after_eq(jiffies, deadline)) {
			wave5_vpu_isolate(inst->dev, "streamoff drain timeout", 0);
			break;
		}

		if (wave5_vpu_wait_interrupt(inst, jiffies_to_msecs(deadline - jiffies)) < 0)
			break;

		if (wave5_vpu_enc_get_output_info(inst, &enc_output_info))
			dev_dbg(inst->dev->dev, "Getting encoding results from fw, fail\n");
	}

	wave5_stop_retained_sources(inst);
	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		streamoff_output(inst, q);
	else
		streamoff_capture(inst, q);

	pm_runtime_put_autosuspend(inst->dev->dev);
}

static const struct vb2_ops wave5_vpu_enc_vb2_ops = {
	.queue_setup = wave5_vpu_enc_queue_setup,
	.buf_queue = wave5_vpu_enc_buf_queue,
	.buf_prepare = wave5_vpu_enc_buf_prepare,
	.start_streaming = wave5_vpu_enc_start_streaming,
	.stop_streaming = wave5_vpu_enc_stop_streaming,
};

static void wave5_set_default_format(struct vpu_instance *inst,
				     struct v4l2_pix_format_mplane *src_fmt,
				     struct v4l2_pix_format_mplane *dst_fmt)
{
	src_fmt->pixelformat = enc_fmt_list[VPU_FMT_TYPE_RAW][0].v4l2_pix_fmt;
	wave5_update_pix_fmt(src_fmt, VPU_FMT_TYPE_RAW,
			     W5_DEF_ENC_PIC_WIDTH, W5_DEF_ENC_PIC_HEIGHT,
			     &enc_frmsize[VPU_FMT_TYPE_RAW]);

	dst_fmt->pixelformat = enc_fmt_list[VPU_FMT_TYPE_CODEC][0].v4l2_pix_fmt;
	wave5_update_pix_fmt(dst_fmt, VPU_FMT_TYPE_CODEC,
			     W5_DEF_ENC_PIC_WIDTH, W5_DEF_ENC_PIC_HEIGHT,
			     &enc_frmsize[VPU_FMT_TYPE_CODEC]);
	inst->std = wave5_to_vpu_std(dst_fmt->pixelformat, inst->type);
}

static int wave5_vpu_enc_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct vpu_instance *inst = priv;

	src_vq->max_num_buffers = ARRAY_SIZE(inst->codec_info->enc_info.pts_map);
	return wave5_vpu_queue_init(priv, src_vq, dst_vq, &wave5_vpu_enc_vb2_ops);
}

static const struct vpu_instance_ops wave5_vpu_enc_inst_ops = {
	.finish_process = wave5_vpu_enc_finish_encode,
};

static void wave5_vpu_enc_device_run(void *priv)
{
	struct vpu_instance *inst = priv;
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	u32 fail_res = 0;
	int ret = 0;

	mutex_lock(&inst->dev->irq_lock);
	if (wave5_vpu_isolated(inst->dev) || inst->recovery_error) {
		wave5_vpu_job_finish(inst);
		mutex_unlock(&inst->dev->irq_lock);
		return;
	}
	ret = pm_runtime_resume_and_get(inst->dev->dev);
	if (ret < 0) {
		wave5_vpu_isolate(inst->dev, "job runtime resume failed", 0);
		wave5_vpu_job_finish(inst);
		mutex_unlock(&inst->dev->irq_lock);
		return;
	}
	wave5_vpu_job_start(inst);
	switch (inst->state) {
	case VPU_INST_STATE_PIC_RUN:
		ret = start_encode(inst, &fail_res);
		if (ret) {
			if (ret == -EINVAL)
				dev_err(inst->dev->dev,
					"Frame encoding on m2m context (%p), fail: %d (res: %d)\n",
					m2m_ctx, ret, fail_res);
			else if (ret == -EAGAIN)
				dev_dbg(inst->dev->dev, "Missing buffers for encode, try again\n");
			break;
		}
		dev_dbg(inst->dev->dev, "%s: leave with active job", __func__);
		pm_runtime_put_autosuspend(inst->dev->dev);
		mutex_unlock(&inst->dev->irq_lock);
		return;
	default:
		WARN(1, "Execution of a job in state %s is invalid.\n",
		     state_to_str(inst->state));
		break;
	}
	dev_dbg(inst->dev->dev, "%s: leave and finish job", __func__);
	pm_runtime_put_autosuspend(inst->dev->dev);
	wave5_vpu_job_finish(inst);
	mutex_unlock(&inst->dev->irq_lock);
}

static int wave5_vpu_enc_job_ready(void *priv)
{
	struct vpu_instance *inst = priv;
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;

	if (wave5_vpu_isolated(inst->dev) || READ_ONCE(inst->recovery_error))
		return false;
	switch (inst->state) {
	case VPU_INST_STATE_NONE:
		dev_dbg(inst->dev->dev, "Encoder must be open to start queueing M2M jobs!\n");
		return false;
	case VPU_INST_STATE_PIC_RUN:
		if (m2m_ctx->is_draining || wave5_find_src(m2m_ctx, false)) {
			dev_dbg(inst->dev->dev, "Encoder ready for a job, state: %s\n",
				state_to_str(inst->state));
			return true;
		}
		fallthrough;
	default:
		dev_dbg(inst->dev->dev,
			"Encoder not ready for a job, state: %s, %s draining, %d src bufs ready\n",
			state_to_str(inst->state), m2m_ctx->is_draining ? "is" : "is not",
			v4l2_m2m_num_src_bufs_ready(m2m_ctx));
		break;
	}
	return false;
}

static const struct v4l2_m2m_ops wave5_vpu_enc_m2m_ops = {
	.device_run = wave5_vpu_enc_device_run,
	.job_ready = wave5_vpu_enc_job_ready,
};

static const struct v4l2_m2m_ops wave5_vpu_enc_t7_m2m_ops = {
	.device_run = wave5_vpu_enc_device_run,
	.job_ready = wave5_vpu_enc_job_ready,
	.job_abort = wave5_vpu_job_abort,
};

static void wave5_create_qp_limits(struct vpu_instance *inst, bool hevc)
{
	struct v4l2_ctrl_handler *hdl = &inst->v4l2_ctrl_hdl;
	struct v4l2_ctrl **limits = hevc ? inst->hevc_qp_limits : inst->avc_qp_limits;
	u32 first = hevc ? V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_MIN_QP :
		V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MIN_QP;
	unsigned int i;

	limits[0] = v4l2_ctrl_find(hdl, hevc ? V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP :
				 V4L2_CID_MPEG_VIDEO_H264_MIN_QP);
	limits[1] = v4l2_ctrl_find(hdl, hevc ? V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP :
				 V4L2_CID_MPEG_VIDEO_H264_MAX_QP);
	for (i = 0; i < 6; i++)
		limits[i + 2] = v4l2_ctrl_new_std(hdl, &wave5_vpu_enc_ctrl_ops,
						  first + i, 0, 51, 1, i % 2 ? 51 : 0);
	if (!hdl->error)
		v4l2_ctrl_cluster(8, limits);
}

static int wave5_vpu_open_enc(struct file *filp)
{
	struct video_device *vdev = video_devdata(filp);
	struct vpu_device *dev = video_drvdata(filp);
	struct vpu_instance *inst = NULL;
	struct v4l2_ctrl_handler *v4l2_ctrl_hdl;
	int ret = 0;

	if (wave5_vpu_isolated(dev))
		return -EIO;
	inst = kzalloc_obj(*inst);
	if (!inst)
		return -ENOMEM;
	v4l2_ctrl_hdl = &inst->v4l2_ctrl_hdl;

	inst->dev = dev;
	inst->type = VPU_INST_TYPE_ENC;
	inst->ops = &wave5_vpu_enc_inst_ops;

	inst->codec_info = kzalloc_obj(*inst->codec_info);
	if (!inst->codec_info) {
		kfree(inst);
		return -ENOMEM;
	}

	v4l2_fh_init(&inst->v4l2_fh, vdev);
	v4l2_fh_add(&inst->v4l2_fh, filp);

	INIT_LIST_HEAD(&inst->list);
	atomic_set(&inst->force_keyframe, 0);

	inst->v4l2_m2m_dev = inst->dev->v4l2_m2m_enc_dev;
	inst->v4l2_fh.m2m_ctx =
		v4l2_m2m_ctx_init(inst->v4l2_m2m_dev, inst, wave5_vpu_enc_queue_init);
	if (IS_ERR(inst->v4l2_fh.m2m_ctx)) {
		ret = PTR_ERR(inst->v4l2_fh.m2m_ctx);
		goto cleanup_inst;
	}
	v4l2_m2m_set_src_buffered(inst->v4l2_fh.m2m_ctx, true);

	v4l2_ctrl_handler_init(v4l2_ctrl_hdl, 80);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10, 0,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2, 0,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_1);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_TIER,
			       V4L2_MPEG_VIDEO_HEVC_TIER_HIGH, 0,
			       V4L2_MPEG_VIDEO_HEVC_TIER_MAIN);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_INTRA_PU_SPLIT, 0, 1, 1, 1);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
			  0, 51, 1, 8);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
			  0, 51, 1, 51);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP,
			  0, 51, 1, 30);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,
			  0, 51, 1, 30);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP,
			  0, 51, 1, 30);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE,
			       V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY, 0,
			       V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_ENABLED);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2,
			  -6, 6, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2,
			  -6, 6, 1, 0);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_TYPE,
			       V4L2_MPEG_VIDEO_HEVC_REFRESH_IDR, 0,
			       V4L2_MPEG_VIDEO_HEVC_REFRESH_IDR);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_PERIOD,
			  0, 2047, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_LOSSLESS_CU,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_CONST_INTRA_PRED,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_WAVEFRONT,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_STRONG_SMOOTHING,
			  0, 1, 1, 1);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_MAX_NUM_MERGE_MV_MINUS1,
			  0, 1, 1, 1);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_TMV_PREDICTION,
			  0, 1, 1, 1);

	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE,
			       dev->legacy_firmware ?
			       BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED) |
			       BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422) |
			       BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE) : 0,
			       V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_LEVEL,
			       V4L2_MPEG_VIDEO_H264_LEVEL_5_1, 0,
			       V4L2_MPEG_VIDEO_H264_LEVEL_1_0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
			  0, 51, 1, 8);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
			  0, 51, 1, 51);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP,
			  0, 51, 1, 30);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,
			  0, 51, 1, 30);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP,
			  0, 51, 1, 30);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE,
			       V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY, 0,
			       V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_ENABLED);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA,
			  -6, 6, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA,
			  -6, 6, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_CONSTRAINED_INTRA_PREDICTION,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_CHROMA_QP_INDEX_OFFSET,
			  -12, 12, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
			  0, 2047, 1, 0);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
			       V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC, 0,
			       V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_AU_DELIMITER,
			  0, 1, 1, 1);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_BACKGROUND_DETECTION,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_HFLIP,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_VFLIP,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_ROTATE,
			  0, 270, 90, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_VBV_SIZE,
			  0, 262144, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE,
			  0, 262144, 1, 0);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEADER_MODE,
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
			       BIT(V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE),
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CQ, 0,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_BITRATE,
			  0, 700000000, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY, 1, 100, 1, 50);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_GOP_SIZE,
			  0, 2047, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD, 0, 4096, 1, 0);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE,
			       V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE_CYCLIC,
			       BIT(V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE_RANDOM),
			       V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE_CYCLIC);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
			  0, 0, 0, 0);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE,
			       V4L2_MPEG_VIDEO_MULTI_SLICE_MODE_MAX_MB, 0,
			       V4L2_MPEG_VIDEO_MULTI_SLICE_MODE_SINGLE);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_MB,
			  0, 0xFFFF, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 1, 32, 1, 1);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave5_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR,
			  0, 1, 1, 0);

	wave5_create_qp_limits(inst, false);
	wave5_create_qp_limits(inst, true);
	if (v4l2_ctrl_hdl->error) {
		ret = -ENODEV;
		goto cleanup_inst;
	}

	inst->v4l2_fh.ctrl_handler = v4l2_ctrl_hdl;
	v4l2_ctrl_handler_setup(v4l2_ctrl_hdl);

	wave5_set_default_format(inst, &inst->src_fmt, &inst->dst_fmt);
	inst->conf_win.width = inst->dst_fmt.width;
	inst->conf_win.height = inst->dst_fmt.height;
	inst->colorspace = V4L2_COLORSPACE_REC709;
	inst->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	inst->quantization = V4L2_QUANTIZATION_DEFAULT;
	inst->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	inst->frame_rate = 30;

	init_completion(&inst->irq_done);
	init_waitqueue_head(&inst->job_wait);
	ret = wave5_kfifo_alloc(inst);
	if (ret) {
		dev_err(inst->dev->dev, "failed to allocate fifo\n");
		goto cleanup_inst;
	}

	inst->id = ida_alloc(&inst->dev->inst_ida, GFP_KERNEL);
	if (inst->id < 0) {
		dev_warn(inst->dev->dev, "Allocating instance ID, fail: %d\n", inst->id);
		ret = inst->id;
		goto cleanup_inst;
	}

	wave5_vdi_allocate_sram(inst->dev);

	ret = mutex_lock_interruptible(&dev->dev_lock);
	if (ret)
		goto cleanup_inst;

	mutex_lock(&dev->irq_lock);
	if (wave5_vpu_isolated(dev)) {
		mutex_unlock(&dev->irq_lock);
		mutex_unlock(&dev->dev_lock);
		ret = -EIO;
		goto cleanup_inst;
	}
	spin_lock_irq(&dev->irq_spinlock);
	list_add_tail(&inst->list, &dev->instances);
	spin_unlock_irq(&dev->irq_spinlock);
	mutex_unlock(&dev->irq_lock);

	mutex_unlock(&dev->dev_lock);

	return 0;

cleanup_inst:
	wave5_cleanup_instance(inst, filp);
	return ret;
}

static int wave5_vpu_enc_release(struct file *filp)
{
	struct vpu_instance *inst = file_to_vpu_inst(filp);
	struct vpu_device *dev = inst->dev;
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	int ret;

	/*
	 * Cancel while the instance can still receive its completion IRQ. The
	 * common release helper removes it from IRQ dispatch before freeing the
	 * context. STREAMOFF cancels even when the queue was not streaming;
	 * keeping both queues stopped also prevents another job being queued.
	 */
	mutex_lock(&dev->dev_lock);
	if (wave5_vpu_isolated(dev))
		goto retain;
	v4l2_m2m_streamoff(filp, m2m_ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	v4l2_m2m_streamoff(filp, m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (wave5_vpu_isolated(dev))
		goto retain;
	/* Confirm DESTROY before releasing any queue backing on the T7. */
	if (dev->legacy_firmware && inst->state != VPU_INST_STATE_NONE) {
		u32 fail_res;

		ret = wave5_vpu_enc_close(inst, &fail_res);
		if (ret) {
			wave5_vpu_isolate(dev, "instance destroy failed", 0);
			goto retain;
		}
		inst->state = VPU_INST_STATE_NONE;
	}
	ret = wave5_vpu_release_device(filp, wave5_vpu_enc_close, "encoder");
	mutex_unlock(&dev->dev_lock);

	return ret;
retain:
	/* Keep the queues, imported DMA refs, instance and auxiliary allocations. */
	inst->released = true;
	v4l2_fh_del(&inst->v4l2_fh, filp);
	v4l2_fh_exit(&inst->v4l2_fh);
	filp->private_data = NULL;
	mutex_unlock(&dev->dev_lock);
	return 0;
}

static long wave5_vpu_enc_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct vpu_instance *inst = file_to_vpu_inst(filp);

	if (wave5_vpu_isolated(inst->dev) || READ_ONCE(inst->recovery_error))
		return -EIO;
	return video_ioctl2(filp, cmd, arg);
}

static __poll_t wave5_vpu_enc_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct vpu_instance *inst = file_to_vpu_inst(filp);

	if (wave5_vpu_isolated(inst->dev) || READ_ONCE(inst->recovery_error))
		return EPOLLERR;
	return v4l2_m2m_fop_poll(filp, wait);
}

static const struct v4l2_file_operations wave5_vpu_enc_fops = {
	.owner = THIS_MODULE,
	.open = wave5_vpu_open_enc,
	.release = wave5_vpu_enc_release,
	.unlocked_ioctl = wave5_vpu_enc_ioctl,
	.poll = wave5_vpu_enc_poll,
	.mmap = v4l2_m2m_fop_mmap,
};

int wave5_vpu_enc_register_device(struct vpu_device *dev)
{
	struct video_device *vdev_enc;
	int ret;

	vdev_enc = devm_kzalloc(dev->v4l2_dev.dev, sizeof(*vdev_enc), GFP_KERNEL);
	if (!vdev_enc)
		return -ENOMEM;

	dev->v4l2_m2m_enc_dev = v4l2_m2m_init(dev->legacy_firmware ?
		&wave5_vpu_enc_t7_m2m_ops : &wave5_vpu_enc_m2m_ops);
	if (IS_ERR(dev->v4l2_m2m_enc_dev)) {
		ret = PTR_ERR(dev->v4l2_m2m_enc_dev);
		dev_err(dev->dev, "v4l2_m2m_init, fail: %d\n", ret);
		return -EINVAL;
	}

	dev->video_dev_enc = vdev_enc;

	strscpy(vdev_enc->name, VPU_ENC_DEV_NAME, sizeof(vdev_enc->name));
	vdev_enc->fops = &wave5_vpu_enc_fops;
	vdev_enc->ioctl_ops = &wave5_vpu_enc_ioctl_ops;
	vdev_enc->release = video_device_release_empty;
	vdev_enc->v4l2_dev = &dev->v4l2_dev;
	vdev_enc->vfl_dir = VFL_DIR_M2M;
	vdev_enc->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	vdev_enc->lock = &dev->dev_lock;

	ret = video_register_device(vdev_enc, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;

	video_set_drvdata(vdev_enc, dev);

	return 0;
}

void wave5_vpu_enc_unregister_device(struct vpu_device *dev)
{
	video_unregister_device(dev->video_dev_enc);
	if (dev->v4l2_m2m_enc_dev)
		v4l2_m2m_release(dev->v4l2_m2m_enc_dev);
}
