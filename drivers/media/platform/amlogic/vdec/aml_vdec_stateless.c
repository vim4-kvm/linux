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

static const struct aml_dec_type dec_type_name[] = {
	{
		.codec_type = CODEC_TYPE_HEVC,
		.name = "HEVC",
	},
	{
		.codec_type = CODEC_TYPE_MJPEG,
		.name = "MJPEG",
	},
	{
		.codec_type = CODEC_TYPE_MPEG2,
		.name = "MPEG2",
	},
	{
		.codec_type = CODEC_TYPE_H264,
		.name = "H264",
	},
};

static const char *dec_type_to_name(unsigned int type)
{
	int i;
	int size = ARRAY_SIZE(dec_type_name);

	for (i = 0; i < size; i++) {
		if (dec_type_name[i].codec_type == type)
			return dec_type_name[i].name;
	}

	return "ERR";
}

static int hevc_get_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct aml_vdec_ctx *ctx = ctrl_to_dec_ctx(ctrl->handler);

	if (ctrl->id != V4L2_CID_MIN_BUFFERS_FOR_CAPTURE)
		return -EINVAL;
	ctrl->val = READ_ONCE(ctx->hevc_min_capture);
	return 0;
}

static const struct v4l2_ctrl_ops hevc_ctrl_ops = {
	.g_volatile_ctrl = hevc_get_volatile_ctrl,
};

int aml_vdec_ctrls_setup(struct aml_vdec_ctx *ctx)
{
	int i;
	int ctrls_size = ctx->is_jpeg || ctx->is_hevc ? 0 : ARRAY_SIZE(controls);

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, ctrls_size);
	if (ctx->is_hevc) {
		struct v4l2_ctrl *ctrl;

		ctrl = v4l2_ctrl_new_std(&ctx->ctrl_handler, &hevc_ctrl_ops,
					 V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, 1, 18, 1, 1);
		if (ctrl)
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE;
	}
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

static int vidioc_vdec_querycap(struct file *file, void *priv,
				struct v4l2_capability *cap)
{
	strscpy(cap->driver, VCODEC_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "platform:" VCODEC_DRV_NAME, sizeof(cap->card));

	return 0;
}

static bool vdec_format_supported(struct aml_vdec_ctx *ctx, const struct aml_video_fmt *fmt)
{
	if (ctx->is_hevc)
		return fmt->type == AML_FMT_DEC ? fmt->codec_type == CODEC_TYPE_HEVC :
			fmt->fourcc == V4L2_PIX_FMT_NV12M;
	if (fmt->codec_type == CODEC_TYPE_HEVC)
		return false;
	if (fmt->type == AML_FMT_DEC)
		return ctx->is_jpeg == (fmt->codec_type == CODEC_TYPE_MJPEG);
	return ctx->is_jpeg == (fmt->fourcc == V4L2_PIX_FMT_YUV420);
}

static int vidioc_vdec_enum_fmt(struct aml_vdec_ctx *ctx,
				struct v4l2_fmtdesc *f, bool is_output)
{
	struct aml_vdec_dev *dev = ctx->dev;
	const struct aml_video_fmt *fmt;
	int i = 0, j = 0;

	for (; i < dev->pvdec_data->num_fmts; i++) {
		fmt = &dev->pvdec_data->dec_fmt[i];
		if (is_output && fmt->type != AML_FMT_DEC)
			continue;
		if (!is_output && fmt->type != AML_FMT_FRAME)
			continue;
		if (!vdec_format_supported(ctx, fmt))
			continue;

		if (j == f->index) {
			f->pixelformat = fmt->fourcc;
			if (ctx->is_hevc && is_output)
				f->flags |= V4L2_FMT_FLAG_DYN_RESOLUTION;
			strscpy(f->description, fmt->name,
				sizeof(f->description));
			return 0;
		}
		++j;
	}
	return -EINVAL;
}

static const struct aml_video_fmt *aml_vdec_get_video_fmt(struct aml_vdec_dev
							  *dev, u32 format)
{
	const struct aml_video_fmt *fmt;
	unsigned int k;

	for (k = 0; k < dev->pvdec_data->num_fmts; k++) {
		fmt = &dev->pvdec_data->dec_fmt[k];
		if (fmt->fourcc == format)
			return fmt;
	}

	return NULL;
}

static int aml_vdec_init_dec_inst(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_dev *dev = ctx->dev;
	struct aml_video_fmt *fmt_out = &ctx->dec_fmt[AML_FMT_SRC];
	int ret = -1;

	ctx->codec_ops = &dev->pvdec_data->codec_ops[fmt_out->codec_type];
	if (ctx->codec_ops->init) {
		ret = ctx->codec_ops->init(ctx);
		if (ret < 0)
			return ret;
	}
	ctx->curr_dec_type = fmt_out->codec_type;
	dev_dbg(&dev->plat_dev->dev, "%s set curr_dec_type %s\n",
		__func__, dec_type_to_name(ctx->curr_dec_type));

	return ret;
}

static void set_pic_info(struct aml_vdec_ctx *ctx,
			 struct v4l2_pix_format_mplane *pix_mp,
			 enum v4l2_buf_type type)
{
	if (V4L2_TYPE_IS_OUTPUT(type)) {
		ctx->pic_info.output_pix_fmt = pix_mp->pixelformat;
		ctx->pic_info.coded_width = ALIGN(pix_mp->width, 64);
		ctx->pic_info.coded_height = ALIGN(pix_mp->height, 64);
		ctx->pic_info.fb_size[0] =
		    ctx->pic_info.coded_width * ctx->pic_info.coded_height;
		ctx->pic_info.fb_size[1] = ctx->pic_info.fb_size[0] / 2;
		ctx->pic_info.plane_num = 1;
	} else {
		ctx->pic_info.plane_num = pix_mp->num_planes;
		ctx->pic_info.cap_pix_fmt = pix_mp->pixelformat;
	}
}

static int vidioc_vdec_enum_framesizes(struct file *file, void *priv,
				       struct v4l2_frmsizeenum *fsize)
{
	const struct aml_video_fmt *fmt;
	struct aml_vdec_dev *dev = video_drvdata(file);
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	if (fsize->index != 0)
		return -EINVAL;

	fmt = aml_vdec_get_video_fmt(dev, fsize->pixel_format);
	if (!fmt || !vdec_format_supported(ctx, fmt))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = fmt->stepwise;

	return 0;
}

static int vdec_try_fmt_mp(struct aml_vdec_ctx *ctx, enum v4l2_buf_type type,
			   struct v4l2_pix_format_mplane *pix_mp)
{
	const struct aml_video_fmt *dec_fmt;
	const struct aml_video_fmt *requested;
	int i, align;

	if (ctx->is_hevc)
		return aml_hevc_try_fmt(ctx, type, pix_mp);
	if (V4L2_TYPE_IS_OUTPUT(type))
		dec_fmt = &ctx->dec_fmt[AML_FMT_SRC];
	else
		dec_fmt = &ctx->dec_fmt[AML_FMT_DST];
	requested = aml_vdec_get_video_fmt(ctx->dev, pix_mp->pixelformat);
	if (requested && vdec_format_supported(ctx, requested) && requested->type ==
	    (V4L2_TYPE_IS_OUTPUT(type) ? AML_FMT_DEC : AML_FMT_FRAME))
		dec_fmt = requested;
	if (ctx->is_jpeg) {
		pix_mp->colorspace = V4L2_COLORSPACE_SRGB;
		pix_mp->xfer_func = V4L2_XFER_FUNC_SRGB;
		pix_mp->ycbcr_enc = V4L2_YCBCR_ENC_601;
		/* The JPEG firmware converts full-range input to limited-range YUV. */
		pix_mp->quantization = V4L2_TYPE_IS_OUTPUT(type) ?
			V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
	}

	pix_mp->field = V4L2_FIELD_NONE;

	if (V4L2_TYPE_IS_OUTPUT(type)) {
		pix_mp->num_planes = dec_fmt->num_planes;
		pix_mp->pixelformat = dec_fmt->fourcc;
		if (!pix_mp->plane_fmt[0].sizeimage)
			pix_mp->plane_fmt[0].sizeimage =
			    (pix_mp->height * pix_mp->width * 3) / 2;
	}

	align = ctx->dev->pvdec_data->dec_fmt->align;
	pix_mp->height = ALIGN(pix_mp->height, align);
	pix_mp->width = ALIGN(pix_mp->width, align);

	v4l2_apply_frmsize_constraints(&pix_mp->width, &pix_mp->height,
				       &dec_fmt->stepwise);
	dev_dbg(&ctx->dev->plat_dev->dev,
		"%s type %d four_cc %d pix_mp->width %d pix_mp->height %d\n",
		__func__, type, dec_fmt->fourcc, pix_mp->width, pix_mp->height);

	v4l2_fill_pixfmt_mp(pix_mp, dec_fmt->fourcc, pix_mp->width,
			    pix_mp->height);

	for (i = 0; i < pix_mp->num_planes; i++)
		memset(&pix_mp->plane_fmt[i].reserved[0], 0x0,
		       sizeof(pix_mp->plane_fmt[0].reserved));

	memset(pix_mp->reserved, 0x0, sizeof(pix_mp->reserved));
	pix_mp->flags = 0;

	dev_dbg(&ctx->dev->plat_dev->dev,
		"%s type %d fmt %d num_plane %d sizeimage0 %d sizeimage1 %d\n",
		__func__, type, pix_mp->pixelformat, pix_mp->num_planes,
		pix_mp->plane_fmt[0].sizeimage, pix_mp->plane_fmt[1].sizeimage);

	return 0;
}

static int vdec_s_fmt_output(struct aml_vdec_ctx *ctx, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct aml_video_fmt *out_fmt;
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->m2m_ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (vb2_is_busy(vq) ||
	    vb2_is_busy(v4l2_m2m_get_vq(ctx->m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)))
		return -EBUSY;

	out_fmt = aml_vdec_get_video_fmt(ctx->dev, pix_mp->pixelformat);
	if (out_fmt && out_fmt->type == AML_FMT_DEC && vdec_format_supported(ctx, out_fmt))
		ctx->dec_fmt[AML_FMT_SRC] = *out_fmt;
	else
		dev_dbg(&ctx->dev->plat_dev->dev,
			"%s fmt %d not supported, use default\n", __func__,
			pix_mp->pixelformat);

	/* Preserve visible dimensions before rounding storage to canvas alignment. */
	v4l2_apply_frmsize_constraints(&pix_mp->width, &pix_mp->height,
				       &ctx->dec_fmt[AML_FMT_SRC].stepwise);
	ctx->compose = (struct v4l2_rect) { .width = pix_mp->width, .height = pix_mp->height };
	ret = vdec_try_fmt_mp(ctx, f->type, pix_mp);
	set_pic_info(ctx, pix_mp, f->type);

	ctx->pix_fmt[AML_FMT_SRC] = *pix_mp;
	ctx->pix_fmt[AML_FMT_DST] = *pix_mp;
	vdec_try_fmt_mp(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			&ctx->pix_fmt[AML_FMT_DST]);
	ctx->dec_fmt[AML_FMT_DST] = *aml_vdec_get_video_fmt(ctx->dev,
					ctx->pix_fmt[AML_FMT_DST].pixelformat);
	set_pic_info(ctx, &ctx->pix_fmt[AML_FMT_DST], V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

	return ret;
}

static int vdec_s_fmt_capture(struct aml_vdec_ctx *ctx, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct aml_video_fmt *cap_fmt;
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(vq))
		return -EBUSY;

	/* Capture storage must cover the dimensions selected on OUTPUT. */
	pix_mp->width = ctx->pic_info.coded_width;
	pix_mp->height = ctx->pic_info.coded_height;
	ret = vdec_try_fmt_mp(ctx, f->type, pix_mp);
	cap_fmt = aml_vdec_get_video_fmt(ctx->dev, pix_mp->pixelformat);
	ctx->dec_fmt[AML_FMT_DST] = *cap_fmt;
	set_pic_info(ctx, pix_mp, f->type);
	ctx->pix_fmt[AML_FMT_DST] = *pix_mp;

	return ret;
}

static void reset_output_fmts(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_dev *dev = ctx->dev;
	const struct aml_video_fmt *out_fmt;
	struct v4l2_pix_format_mplane fmt;

	out_fmt = aml_vdec_get_video_fmt(dev, ctx->is_hevc ? V4L2_PIX_FMT_HEVC :
					ctx->is_jpeg ? V4L2_PIX_FMT_MJPEG :
					V4L2_PIX_FMT_H264_SLICE);
	if (!out_fmt)
		return;

	ctx->dec_fmt[AML_FMT_SRC] = *out_fmt;

	memset(&fmt, 0, sizeof(struct v4l2_pix_format_mplane));

	fmt.height = out_fmt->stepwise.min_height;
	fmt.width = out_fmt->stepwise.min_width;
	/* reset bytesperline to 0 for output fmts */
	fmt.plane_fmt[0].bytesperline = 0;
	fmt.colorspace = V4L2_COLORSPACE_DEFAULT;
	fmt.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	vdec_try_fmt_mp(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &fmt);

	ctx->pix_fmt[AML_FMT_SRC] = fmt;
}

static void reset_capture_fmts(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_dev *dev = ctx->dev;
	const struct aml_video_fmt *cap_fmt;
	struct v4l2_pix_format_mplane fmt;

	cap_fmt = aml_vdec_get_video_fmt(dev, ctx->is_hevc ? V4L2_PIX_FMT_NV12M :
					ctx->is_jpeg ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_NV12);
	if (!cap_fmt)
		return;

	ctx->dec_fmt[AML_FMT_DST] = *cap_fmt;

	memset(&fmt, 0, sizeof(struct v4l2_pix_format_mplane));

	fmt.height = cap_fmt->stepwise.min_height;
	fmt.width = cap_fmt->stepwise.min_width;
	fmt.colorspace = V4L2_COLORSPACE_DEFAULT;
	fmt.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	vdec_try_fmt_mp(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &fmt);

	ctx->pix_fmt[AML_FMT_DST] = fmt;
	ctx->compose = (struct v4l2_rect) { .width = fmt.width, .height = fmt.height };
}

void aml_vdec_reset_fmts(struct aml_vdec_ctx *ctx)
{
	ctx->m2m_ctx->q_lock = &ctx->v4l2_intf_lock;
	reset_output_fmts(ctx);
	reset_capture_fmts(ctx);
}

static int vdec_g_fmt(struct aml_vdec_ctx *ctx, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;

	if (ctx->is_hevc && !V4L2_TYPE_IS_OUTPUT(f->type)) {
		*pix_mp = ctx->pix_fmt[AML_FMT_DST];
		return aml_hevc_try_fmt(ctx, f->type, pix_mp);
	}
	if (V4L2_TYPE_IS_OUTPUT(f->type))
		*pix_mp = ctx->pix_fmt[AML_FMT_SRC];
	else
		*pix_mp = ctx->pix_fmt[AML_FMT_DST];

	dev_dbg(&ctx->dev->plat_dev->dev,
		"%s fmt %d num planes %d\n", __func__, pix_mp->pixelformat,
		pix_mp->num_planes);

	return 0;
}

static int vidioc_try_fmt_cap_mplane(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	return vdec_try_fmt_mp(fh_to_dec_ctx(file), f->type, &f->fmt.pix_mp);
}

static int vidioc_try_fmt_out_mplane(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	return vdec_try_fmt_mp(fh_to_dec_ctx(file), f->type, &f->fmt.pix_mp);
}

static int vidioc_vdec_s_fmt_out_mplane(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vdec_s_fmt_output(ctx, f);
}

static int vidioc_vdec_s_fmt_cap_mplane(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vdec_s_fmt_capture(ctx, f);
}

static int vidioc_vdec_g_fmt_out_mplane(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vdec_g_fmt(ctx, f);
}

static int vidioc_vdec_g_fmt_cap_mplane(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vdec_g_fmt(ctx, f);
}

static int vidioc_vdec_enum_fmt_out_mplane(struct file *file,
					   void *priv, struct v4l2_fmtdesc *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vidioc_vdec_enum_fmt(ctx, f, 1);
}

static int vidioc_vdec_enum_fmt_cap_mplane(struct file *file,
					   void *priv, struct v4l2_fmtdesc *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vidioc_vdec_enum_fmt(ctx, f, 0);
}

static int vdec_g_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	if (ctx->is_hevc)
		return aml_hevc_get_selection(ctx, s);
	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    ctx->dec_fmt[AML_FMT_SRC].codec_type != CODEC_TYPE_MJPEG)
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
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	if (ctx->is_jpeg || ctx->is_hevc)
		return v4l2_m2m_ioctl_try_decoder_cmd(file, priv, dc);
	return v4l2_m2m_ioctl_stateless_try_decoder_cmd(file, priv, dc);
}

static int vdec_decoder_cmd(struct file *file, void *priv, struct v4l2_decoder_cmd *dc)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);
	struct vb2_v4l2_buffer *dst;
	int ret;

	if (ctx->is_hevc)
		return aml_hevc_decoder_cmd(file, priv, dc);
	if (ctx->dec_fmt[AML_FMT_SRC].codec_type != CODEC_TYPE_MJPEG)
		return v4l2_m2m_ioctl_stateless_decoder_cmd(file, priv, dc);
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

	if (ctx->is_hevc && sub->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subscribe(fh, sub);
	if (ctx->is_hevc && sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 2, NULL);
	return v4l2_ctrl_subscribe_event(fh, sub);
}

const struct v4l2_ioctl_ops aml_vdec_ioctl_ops = {
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

static void aml_vdec_release_instance(struct aml_vdec_ctx *ctx)
{
	if (ctx->codec_ops && ctx->codec_ops->exit)
		ctx->codec_ops->exit(ctx);
}

static int vb2ops_vdec_queue_setup(struct vb2_queue *vq,
				   unsigned int *nbuffers,
				   unsigned int *nplanes,
				   unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pix_fmt;
	unsigned int i;

	if (ctx->is_hevc && V4L2_TYPE_IS_CAPTURE(vq->type))
		*nbuffers = max(*nbuffers, READ_ONCE(ctx->hevc_min_capture));
	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->pix_fmt[AML_FMT_SRC];
	else
		pix_fmt = &ctx->pix_fmt[AML_FMT_DST];

	if (*nplanes) {
		if (*nplanes != pix_fmt->num_planes)
			return -EINVAL;

		for (i = 0; i < *nplanes; i++) {
			if (sizes[i] < pix_fmt->plane_fmt[i].sizeimage) {
				dev_err(&ctx->dev->plat_dev->dev,
					"not supported sizeimage\n");
				return -EINVAL;
			}
			alloc_devs[i] = &ctx->dev->plat_dev->dev;
		}
	} else {
		if (vq->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			*nplanes = pix_fmt->num_planes;
		else if (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
			*nplanes = 1;

		for (i = 0; i < *nplanes; i++) {
			alloc_devs[i] = &ctx->dev->plat_dev->dev;
			sizes[i] = pix_fmt->plane_fmt[i].sizeimage;
		}
	}

	if (*nplanes) {
		dev_dbg(&ctx->dev->plat_dev->dev,
			"type: %d, plane: %d, buf cnt: %d, size: [Y: %u, C: %u]\n",
			vq->type, *nplanes, *nbuffers, sizes[0], sizes[1]);
		return 0;
	}

	return -EINVAL;
}

static int vb2ops_vdec_buf_prepare(struct vb2_buffer *vb)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *pix_fmt;
	unsigned int sizeimage = 0;
	int i;

	if (ctx->dev->dec_hw->faulted)
		return -EIO;
	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
		pix_fmt = &ctx->pix_fmt[AML_FMT_SRC];
	else
		pix_fmt = &ctx->pix_fmt[AML_FMT_DST];

	for (i = 0; i < pix_fmt->num_planes; i++) {
		sizeimage = pix_fmt->plane_fmt[i].sizeimage;
		if (vb2_plane_size(vb, i) < sizeimage)
			return -EINVAL;

		if (V4L2_TYPE_IS_CAPTURE(vb->type)) {
			vb2_set_plane_payload(vb, i,
					      pix_fmt->plane_fmt[i].sizeimage);
			dev_dbg(&ctx->dev->plat_dev->dev,
				"%s type: %d set plane: %d, sizeimage: %d\n",
				__func__, vb->vb2_queue->type, i,
				pix_fmt->plane_fmt[i].sizeimage);
		}
	}

	return 0;
}

static void vb2_ops_vdec_buf_queue(struct vb2_buffer *vb)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vb2_v4l2 = to_vb2_v4l2_buffer(vb);

	if ((ctx->is_jpeg || ctx->is_hevc) &&
	    V4L2_TYPE_IS_CAPTURE(vb->type) && v4l2_m2m_dst_buf_is_last(ctx->m2m_ctx)) {
		unsigned int i;

		for (i = 0; i < vb->num_planes; i++)
			vb2_set_plane_payload(vb, i, 0);
		if (ctx->is_hevc) {
			aml_hevc_last_buffer_done(ctx, vb2_v4l2);
		} else {
			vb2_v4l2->sequence = ctx->sequence_cap++;
			v4l2_m2m_last_buffer_done(ctx->m2m_ctx, vb2_v4l2);
		}
		return;
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vb2_v4l2);
}

static void vb2_ops_vdec_buf_finish(struct vb2_buffer *vb)
{
}

static int vb2ops_vdec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(q);

	if (ctx->dev->dec_hw->faulted)
		return -EIO;
	if (ctx->is_jpeg || ctx->is_hevc)
		v4l2_m2m_update_start_streaming_state(ctx->m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		ctx->sequence_out = 0;
		ctx->is_output_streamon = 1;
		if (aml_vdec_init_dec_inst(ctx) < 0)
			return -EINVAL;
	} else {
		ctx->sequence_cap = 0;
		ctx->is_cap_streamon = 1;
		if (ctx->is_hevc)
			aml_hevc_start_capture(ctx);
	}

	return 0;
}

static void vb2ops_vdec_stop_streaming(struct vb2_queue *q)
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *src_buf = NULL, *dst_buf = NULL;

	if (ctx->is_hevc)
		aml_hevc_cancel_output(ctx);
	if (!ctx->is_hevc || V4L2_TYPE_IS_OUTPUT(q->type))
		aml_vdec_release_instance(ctx);

	if (ctx->is_jpeg || ctx->is_hevc)
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

static int vb2ops_vdec_out_buf_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
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

int aml_vdec_queue_init(void *priv, struct vb2_queue *src_vq,
			struct vb2_queue *dst_vq)
{
	struct aml_vdec_ctx *ctx = (struct aml_vdec_ctx *)priv;
	struct aml_vdec_dev *dev = ctx->dev;
	int ret = 0;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = dev->pvdec_data->has_dbus_ctrl ? VB2_MMAP : VB2_MMAP | VB2_DMABUF;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->drv_priv = ctx;
	src_vq->ops = &aml_vdec_vb2_ops;
	src_vq->lock = &ctx->v4l2_intf_lock;
	src_vq->buf_struct_size = sizeof(struct aml_vdec_buffer);
	src_vq->supports_requests = !ctx->is_jpeg && !ctx->is_hevc;
	if (dev->pvdec_data->has_internal_canvas && !ctx->is_jpeg && !ctx->is_hevc)
		src_vq->subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	ret = vb2_queue_init(src_vq);
	if (ret) {
		v4l2_info(&dev->v4l2_dev,
			  "Failed to initialize videobuf2 queue(output)");
		return ret;
	}

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = src_vq->io_modes;
	dst_vq->drv_priv = ctx;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->ops = &aml_vdec_vb2_ops;
	dst_vq->lock = &ctx->v4l2_intf_lock;
	dst_vq->buf_struct_size = sizeof(struct aml_vdec_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	ret = vb2_queue_init(dst_vq);
	if (ret) {
		v4l2_info(&dev->v4l2_dev,
			  "Failed to initialize videobuf2 queue(capture)");
		vb2_queue_release(src_vq);
	}

	return ret;
}
