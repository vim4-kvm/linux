// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/* HEVC stateful codec adapter. */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-dma-contig.h>

#include "aml_vdec.h"
#include "aml_vdec_hw.h"
#include "hevc_priv.h"

static int h265_set_dw_target(struct aml_hevc_ctx *h)
{
	struct aml_vdec_ctx *ctx = h->v4l2_ctx;
	struct v4l2_pix_format_mplane *pf = &ctx->pix_fmt[AML_FMT_DST];
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	size_t chroma = ALIGN(ALIGN(h->dpb.pic_w, 64) * ALIGN(h->dpb.pic_h, 64) / 2, SZ_64K);

	h->dw_target_y = 0;
	h->dw_target_c = 0;
	if (!h->configured)
		return 0;
	if (!dst || pf->pixelformat != V4L2_PIX_FMT_NV12M ||
	    pf->plane_fmt[0].bytesperline != ALIGN(h->dpb.pic_w, 64) ||
	    dst->vb2_buf.num_planes != 2 ||
	    vb2_plane_size(&dst->vb2_buf, 0) < chroma * 2 ||
	    vb2_plane_size(&dst->vb2_buf, 1) < chroma)
		return -EINVAL;
	h->dw_target_y = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	h->dw_target_c = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 1);
	return 0;
}

int aml_hevc_init(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_vdec_hw *hw = ctx->dev->dec_hw;
	struct aml_hevc_ctx *h;

	if (!hw || !hw->hevc.present)
		return -ENODEV;
	h = kzalloc_obj(*h);
	if (!h)
		return -ENOMEM;
	h->v4l2_ctx = ctx;
	h->hw = hw;
	h->stateful = aml_vdec_stateful(ctx);
	h->stateful->format_known = false;
	h->stateful->sequence = 0;
	memset(&h->stateful->format, 0, sizeof(h->stateful->format));
	memset(&h->stateful->crop, 0, sizeof(h->stateful->crop));
	h->dpb.cur = -1;
	h->dpb.curr_poc = H265_INVALID_POC;
	h->dpb.poc_random_access = H265_MAX_INT;
	WRITE_ONCE(aml_vdec_stateful(ctx)->wait_capture, false);

	ctx->codec_priv = h;
	return 0;
}

void aml_hevc_exit(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_hevc_ctx *h = ctx->codec_priv;

	if (!h)
		return;
	if (h->hw->faulted)
		return;
	h265_hw_free(h);
	kfree(h);
	ctx->codec_priv = NULL;
}

int aml_hevc_configure(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_hevc_ctx *h = ctx->codec_priv;
	bool replace;
	int ret, i;

	if (h->hw->faulted || h->hw->hevc.irq_enabled)
		return -EIO;
	if (!h->dpb.pic_w || !h->dpb.pic_h)
		return -EINVAL;
	if (h->dpb.pending)
		return -EBUSY;
	replace = h->alloc_w != h->dpb.pic_w || h->alloc_h != h->dpb.pic_h ||
		h->alloc_nslots != h->dpb.nslots;
	/* The header pass has stopped and synchronized all firmware accesses. */
	ret = h265_hw_alloc_pictures(h);
	if (!ret) {
		if (replace)
			for (i = 0; i < H265_SLOTS; i++) {
				memset(&h->dpb.pic[i], 0, sizeof(h->dpb.pic[i]));
				h->dpb.pic[i].poc = H265_INVALID_POC;
			}
		h->configured = true;
	}
	return ret;
}

void h265_capture_format(struct v4l2_pix_format_mplane *fmt, u32 width, u32 height)
{
	u32 chroma;

	width = ALIGN(clamp_t(u32, width, 64, H265_MAX_DIM), 64);
	height = ALIGN(clamp_t(u32, height, 64, H265_MAX_HEIGHT), 64);
	v4l2_fill_pixfmt_mp(fmt, V4L2_PIX_FMT_NV12M, width, height);
	/* SAO lengths are in 64 KiB chroma units; Y has twice that capacity. */
	chroma = ALIGN(width * height / 2, SZ_64K);
	fmt->plane_fmt[0].sizeimage = chroma * 2;
	fmt->plane_fmt[1].sizeimage = chroma;
	fmt->field = V4L2_FIELD_NONE;
}

int aml_hevc_try_fmt(struct aml_vdec_ctx *ctx, enum v4l2_buf_type type,
		     struct v4l2_pix_format_mplane *fmt)
{
	struct aml_hevc_ctx *h;

	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	h = ctx->codec_priv;
	if (V4L2_TYPE_IS_OUTPUT(type)) {
		fmt->width = clamp_t(u32, fmt->width, 64, H265_MAX_DIM);
		fmt->height = clamp_t(u32, fmt->height, 64, H265_MAX_HEIGHT);
		fmt->pixelformat = V4L2_PIX_FMT_HEVC;
		fmt->field = V4L2_FIELD_NONE;
		fmt->num_planes = 1;
		fmt->plane_fmt[0].bytesperline = 0;
		fmt->plane_fmt[0].sizeimage = clamp_t(u32, fmt->plane_fmt[0].sizeimage,
						      SZ_1M, SZ_16M);
	} else if (h && h->stateful->format_known) {
		*fmt = h->stateful->format;
	} else {
		h265_capture_format(fmt, fmt->width, fmt->height);
	}
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
	return 0;
}

static int hevc_prepare(struct aml_vdec_ctx *ctx)
{
	struct aml_hevc_ctx *h = ctx->codec_priv;
	int ret;

	if (h->stateful->format_known && !h->configured) {
		ret = aml_hevc_configure(ctx);
		if (ret)
			return ret;
	}
	if (!v4l2_m2m_next_src_buf(ctx->m2m_ctx) || h->hw->faulted)
		return -EINVAL;
	ret = h265_hw_alloc_workspace(h);
	if (ret)
		return ret;
	return h265_set_dw_target(h);
}

static int hevc_submit(struct aml_vdec_ctx *ctx, struct vb2_v4l2_buffer *src,
		       struct vb2_v4l2_buffer *dst, struct aml_vdec_decode_result *result)
{
	struct aml_hevc_ctx *h = ctx->codec_priv;
	const void *data = vb2_plane_vaddr(&src->vb2_buf, 0);
	size_t size = vb2_get_plane_payload(&src->vb2_buf, 0);
	int ret, stop_ret;

	if (!h->have_au) {
		ret = h265_headers_parse_au(&h->headers, data, size, &h->au);
		if (ret) {
			if (h->au.unsupported == H265_UNSUPPORTED_SPS_LT)
				dev_dbg(h->hw->dev, "hevc: unsupported: SPS long-term list\n");
			else if (h->au.unsupported == H265_UNSUPPORTED_WPP_ONE_COLUMN)
				dev_dbg(h->hw->dev, "hevc: unsupported: WPP with one CTB column\n");
			stop_ret = aml_vdec_job_stop(ctx);
			if (stop_ret)
				return stop_ret;
			result->recoverable_error = true;
			return 0;
		}
		h->have_au = true;
		h->picture_started = false;
		if (h->au.cvs_boundary_before)
			h265_dpb_end_sequence(&h->dpb);
	}
	ret = h265_hw_run(h, data, size);
	stop_ret = aml_vdec_job_stop(ctx);
	if (stop_ret)
		return stop_ret;
	if (ret < 0 && h->dpb.cur >= 0) {
		h->dpb.pic[h->dpb.cur].valid = false;
		h->dpb.pic[h->dpb.cur].referenced = false;
	}

	if (ret == -EBADMSG) {
		h->have_au = false;
		result->recoverable_error = true;
		return 0;
	}
	if (ret < 0)
		return ret;
	if (ret != 1) {
		if (h->have_picture)
			h265_dpb_end_picture(&h->dpb, h->dpb.cur, h->au.picture.pic_output,
					     &h->au.picture.limits);
		if (h->au.cvs_boundary_after)
			h265_dpb_end_sequence(&h->dpb);
		h->have_au = false;
	}
	result->format_changed = ret == 1;
	result->picture = h->have_picture;
	result->slot = h->dpb.cur;
	result->flags = h->dpb.slice_type == H265_I_SLICE ? V4L2_BUF_FLAG_KEYFRAME :
		h->dpb.slice_type == H265_P_SLICE ?
			V4L2_BUF_FLAG_PFRAME : V4L2_BUF_FLAG_BFRAME;
	return 0;
}

static void hevc_queue_capture(struct aml_vdec_ctx *ctx, unsigned int slot,
			       struct vb2_v4l2_buffer *dst)
{
	struct aml_hevc_ctx *h = ctx->codec_priv;

	h->capture[slot] = dst;
}

static int hevc_get_frame(struct aml_vdec_ctx *ctx, bool flush, struct aml_vdec_frame *frame)
{
	struct aml_hevc_ctx *h = ctx->codec_priv;
	int slot;

	if (flush)
		h265_dpb_flush(&h->dpb);
	slot = h265_dpb_pop_discard(&h->dpb);
	frame->disposition = AML_VDEC_FRAME_DISCARD;
	if (slot < 0) {
		slot = h265_dpb_pop_output(&h->dpb);
		frame->disposition = AML_VDEC_FRAME_OUTPUT;
	}

	if (slot < 0)
		return 0;
	frame->buffer = h->capture[slot];
	h->capture[slot] = NULL;
	frame->final = !h->dpb.pending;
	return 1;
}

static void hevc_get_format(struct aml_vdec_ctx *ctx, struct aml_vdec_stream_info *info)
{
	struct aml_hevc_ctx *h = ctx->codec_priv;
	struct h265_crop crop = h265_display_crop(h->dpb.p);
	u16 *p = h->dpb.p;

	info->format = ctx->pix_fmt[AML_FMT_DST];
	h265_capture_format(&info->format, h->dpb.pic_w, h->dpb.pic_h);
	info->format.quantization = p[P_VIDEO_SIGNAL_TYPE] & BIT(9) ?
		V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
	info->crop = (struct v4l2_rect) {
		.left = crop.left, .top = crop.top, .width = crop.width, .height = crop.height,
	};
	info->min_capture = h->au.picture.limits.max_num_reorder_pics + 2;
	h->configured = false;
}

const struct aml_vdec_stateful_ops aml_hevc_stateful_ops = {
	.prepare = hevc_prepare,
	.submit = hevc_submit,
	.queue_capture = hevc_queue_capture,
	.get_frame = hevc_get_frame,
	.get_format = hevc_get_format,
};

static const u32 hevc_formats[] = { V4L2_PIX_FMT_HEVC, V4L2_PIX_FMT_NV12M };

const struct aml_vdec_node_desc aml_vdec_hevc_node = {
	.interface = &aml_vdec_stateful_interface,
	.stateful_ops = &aml_hevc_stateful_ops,
	.job_ops = &aml_hevc_job_ops,
	.formats = hevc_formats,
	.num_formats = ARRAY_SIZE(hevc_formats),
	.default_output = V4L2_PIX_FMT_HEVC,
	.default_capture = V4L2_PIX_FMT_NV12M,
	.output_flags = V4L2_FMT_FLAG_DYN_RESOLUTION,
	.events = BIT(V4L2_EVENT_SOURCE_CHANGE) | BIT(V4L2_EVENT_EOS),
	.buffered_capture = true,
	.try_format = aml_hevc_try_fmt,
};
