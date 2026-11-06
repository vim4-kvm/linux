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

void aml_vdec_fault(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_hw *hw = ctx->dev->dec_hw;

	if (xchg(&hw->faulted, true))
		return;
	if (hw->vdec1.powered) {
		regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_MASK, 0);
		disable_irq(hw->vdec1.irq);
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

void aml_vdec_run_pair(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct media_request *src_req;
	int ret = 0;
	const char *fw_path = dev->pvdec_data->fw_path[ctx->curr_dec_type];

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
	WRITE_ONCE(dev->dec_hw->vdec1.priv, ctx->codec_priv);
	if (ctx->codec_ops->prepare)
		ctx->codec_ops->prepare(ctx);
	dos_enable(dev->dec_hw);
	/* incase of bus hang in stop_streaming */
	ctx->dos_clk_en = 1;
	ret = aml_vdec_reset_core(dev->dec_hw, ctx->codec_ops->reset);
	if (ret) {
		/* Isolation/reset failed: do not retry MMIO or return DMA ownership. */
		aml_vdec_fault(ctx);
		goto complete_request;
	}

	ret = load_firmware(dev->dec_hw, fw_path, ctx->codec_ops);
	if (ret)
		goto stop_job;

	ret = ctx->node->interface->run_job(ctx);

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
	WRITE_ONCE(dev->dec_hw->vdec1.priv, NULL);
	ctx->node->interface->finish_job(ctx, VB2_BUF_STATE_DONE);
	return;

err_cancel_job:
	aml_vdec_stop_job(ctx);
	aml_vdec_put_buffer_pins(ctx);
	if (!dev->dec_hw->faulted)
		WRITE_ONCE(dev->dec_hw->vdec1.priv, NULL);
	ctx->node->interface->finish_job(ctx, VB2_BUF_STATE_ERROR);
}

static void m2mops_vdec_device_run(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;

	ctx->node->interface->m2m_ops->device_run(ctx);
}

static int m2mops_vdec_job_ready(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	const struct v4l2_m2m_ops *ops = ctx->node->interface->m2m_ops;

	return ops->job_ready ? ops->job_ready(ctx) : 1;
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

int vidioc_vdec_querycap(struct file *file, void *priv,
			 struct v4l2_capability *cap)
{
	strscpy(cap->driver, VCODEC_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "platform:" VCODEC_DRV_NAME, sizeof(cap->card));

	return 0;
}

static bool vdec_format_supported(struct aml_vdec_ctx *ctx, const struct aml_video_fmt *fmt)
{
	unsigned int i;

	for (i = 0; i < ctx->node->num_formats; i++)
		if (ctx->node->formats[i] == fmt->fourcc)
			return true;
	return false;
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
			if (ctx->node->output_flags && is_output)
				f->flags |= ctx->node->output_flags;
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

int aml_vdec_init_dec_inst(struct aml_vdec_ctx *ctx)
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

int vidioc_vdec_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *fsize)
{
	const struct aml_video_fmt *fmt;
	struct aml_vdec_dev *dev = fh_to_dec_ctx(file)->dev;
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

	if (ctx->node->try_format)
		return ctx->node->try_format(ctx, type, pix_mp);
	if (V4L2_TYPE_IS_OUTPUT(type))
		dec_fmt = &ctx->dec_fmt[AML_FMT_SRC];
	else
		dec_fmt = &ctx->dec_fmt[AML_FMT_DST];
	requested = aml_vdec_get_video_fmt(ctx->dev, pix_mp->pixelformat);
	if (requested && vdec_format_supported(ctx, requested) && requested->type ==
	    (V4L2_TYPE_IS_OUTPUT(type) ? AML_FMT_DEC : AML_FMT_FRAME))
		dec_fmt = requested;
	if (ctx->node->set_colors)
		ctx->node->set_colors(ctx, type, pix_mp);

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

	out_fmt = aml_vdec_get_video_fmt(dev, ctx->node->default_output);
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

	cap_fmt = aml_vdec_get_video_fmt(dev, ctx->node->default_capture);
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

	if (ctx->node->try_format && !V4L2_TYPE_IS_OUTPUT(f->type)) {
		*pix_mp = ctx->pix_fmt[AML_FMT_DST];
		return ctx->node->try_format(ctx, f->type, pix_mp);
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

int vidioc_try_fmt_cap_mplane(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	return vdec_try_fmt_mp(fh_to_dec_ctx(file), f->type, &f->fmt.pix_mp);
}

int vidioc_try_fmt_out_mplane(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	return vdec_try_fmt_mp(fh_to_dec_ctx(file), f->type, &f->fmt.pix_mp);
}

int vidioc_vdec_s_fmt_out_mplane(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vdec_s_fmt_output(ctx, f);
}

int vidioc_vdec_s_fmt_cap_mplane(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vdec_s_fmt_capture(ctx, f);
}

int vidioc_vdec_g_fmt_out_mplane(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vdec_g_fmt(ctx, f);
}

int vidioc_vdec_g_fmt_cap_mplane(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vdec_g_fmt(ctx, f);
}

int vidioc_vdec_enum_fmt_out_mplane(struct file *file,
				    void *priv, struct v4l2_fmtdesc *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vidioc_vdec_enum_fmt(ctx, f, 1);
}

int vidioc_vdec_enum_fmt_cap_mplane(struct file *file,
				    void *priv, struct v4l2_fmtdesc *f)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	return vidioc_vdec_enum_fmt(ctx, f, 0);
}

void aml_vdec_release_instance(struct aml_vdec_ctx *ctx)
{
	if (ctx->codec_ops && ctx->codec_ops->exit)
		ctx->codec_ops->exit(ctx);
}

int vb2ops_vdec_queue_setup(struct vb2_queue *vq,
			    unsigned int *nbuffers,
			    unsigned int *nplanes,
			    unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct aml_vdec_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pix_fmt;
	unsigned int i;

	if (ctx->node->buffered_capture && V4L2_TYPE_IS_CAPTURE(vq->type))
		*nbuffers = max(*nbuffers, READ_ONCE(aml_vdec_stateful(ctx)->min_capture));
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

int vb2ops_vdec_buf_prepare(struct vb2_buffer *vb)
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

int vb2ops_vdec_out_buf_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

void vb2_ops_vdec_buf_finish(struct vb2_buffer *vb)
{
}

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
	src_vq->ops = ctx->node->interface->queue_ops;
	src_vq->lock = &ctx->v4l2_intf_lock;
	src_vq->buf_struct_size = sizeof(struct aml_vdec_buffer);
	src_vq->supports_requests = ctx->node->interface->supports_requests;
	if (dev->pvdec_data->has_internal_canvas && ctx->node->interface->supports_requests)
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
	dst_vq->ops = ctx->node->interface->queue_ops;
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
