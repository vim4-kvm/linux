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

	if (!hw || !hw->has_hevc)
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
	int ret;

	if (h->hw->faulted || h->hw->hevc_irq_enabled)
		return -EIO;
	if (!h->dpb.pic_w || !h->dpb.pic_h)
		return -EINVAL;
	/* The header pass has stopped and synchronized all firmware accesses. */
	ret = h265_hw_alloc_pictures(h);
	if (!ret)
		h->configured = true;
	return ret;
}

int aml_hevc_dec_run(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_hevc_ctx *h = ctx->codec_priv;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	int ret;

	if (!src || h->hw->faulted)
		return -EINVAL;
	ret = h265_hw_alloc_workspace(h);
	if (ret)
		return ret;
	ret = h265_set_dw_target(h);
	if (ret)
		return ret;
	return h265_hw_run(h, vb2_plane_vaddr(&src->vb2_buf, 0),
			   vb2_get_plane_payload(&src->vb2_buf, 0));
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

int aml_hevc_get_selection(struct aml_vdec_ctx *ctx, struct v4l2_selection *s)
{
	struct aml_hevc_ctx *h;
	int ret = 0;

	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	h = ctx->codec_priv;
	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		s->r = h && h->stateful->format_known ? h->stateful->crop : ctx->compose;
		break;
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		s->r = (struct v4l2_rect) {
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

static void h265_output(struct aml_hevc_ctx *h, int slot,
			enum vb2_buffer_state state, bool last)
{
	struct vb2_v4l2_buffer *vb = h->capture[slot];

	h->capture[slot] = NULL;
	h265_remove_output(&h->dpb, slot);
	vb->sequence = h->stateful->sequence++;
	if (last)
		vb->flags |= V4L2_BUF_FLAG_LAST;
	v4l2_m2m_buf_done(vb, state);
}

static void h265_flush_pending(struct aml_hevc_ctx *h,
			       enum vb2_buffer_state state, bool last)
{
	int slot;

	while ((slot = h265_next_output(&h->dpb, true)) >= 0)
		h265_output(h, slot, state, last && h->dpb.pending == 1);
}

void aml_hevc_eos(struct aml_vdec_ctx *ctx)
{
	const struct v4l2_event event = { .type = V4L2_EVENT_EOS };

	v4l2_event_queue_fh(&ctx->fh, &event);
}

void aml_hevc_last_buffer_done(struct aml_vdec_ctx *ctx,
			       struct vb2_v4l2_buffer *vb)
{
	struct aml_hevc_ctx *h;

	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	h = ctx->codec_priv;
	if (h)
		vb->sequence = h->stateful->sequence++;
	aml_hevc_eos(ctx);
	v4l2_m2m_last_buffer_done(ctx->m2m_ctx, vb);
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
}

static void h265_end_capture(struct aml_vdec_ctx *ctx, bool eos)
{
	struct aml_hevc_ctx *h = ctx->codec_priv;
	struct vb2_v4l2_buffer *vb = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);
	unsigned int i;

	if (!vb) {
		if (eos)
			ctx->m2m_ctx->next_buf_last = true;
		return;
	}
	for (i = 0; i < vb->vb2_buf.num_planes; i++)
		vb2_set_plane_payload(&vb->vb2_buf, i, 0);
	vb->sequence = h->stateful->sequence++;
	if (eos) {
		aml_hevc_eos(ctx);
		v4l2_m2m_last_buffer_done(ctx->m2m_ctx, vb);
	} else {
		/* A source change terminates CAPTURE, not the OUTPUT drain. */
		vb->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_DONE);
	}
}

void aml_hevc_cancel_output(struct aml_vdec_ctx *ctx)
{
	struct aml_hevc_ctx *h;

	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	h = ctx->codec_priv;
	if (h)
		h265_flush_pending(h, VB2_BUF_STATE_ERROR, false);
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
}

void aml_hevc_start_capture(struct aml_vdec_ctx *ctx)
{
	struct aml_hevc_ctx *h;

	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	h = ctx->codec_priv;
	if (h)
		h->stateful->sequence = 0;
	WRITE_ONCE(aml_vdec_stateful(ctx)->wait_capture, false);
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
}

static void h265_source_change(struct aml_hevc_ctx *h)
{
	struct aml_vdec_ctx *ctx = h->v4l2_ctx;
	struct v4l2_event event = { .type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION };
	struct v4l2_pix_format_mplane *old = &ctx->pix_fmt[AML_FMT_DST];
	u16 *p = h->dpb.p;
	bool capture_matches;
	bool end_capture = h->stateful->format_known && ctx->is_cap_streamon;

	h->stateful->format = ctx->pix_fmt[AML_FMT_DST];
	h265_capture_format(&h->stateful->format, h->dpb.pic_w, h->dpb.pic_h);
	capture_matches = !h->stateful->format_known && ctx->is_cap_streamon &&
		old->width == h->stateful->format.width &&
		old->height == h->stateful->format.height &&
		old->num_planes == 2 &&
		old->plane_fmt[0].sizeimage >= h->stateful->format.plane_fmt[0].sizeimage &&
		old->plane_fmt[1].sizeimage >= h->stateful->format.plane_fmt[1].sizeimage;
	h->stateful->format.quantization = p[P_VIDEO_SIGNAL_TYPE] & BIT(9) ?
		V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
	h->stateful->crop = (struct v4l2_rect) { .width = h->dpb.pic_w, .height = h->dpb.pic_h };
	if (p[P_CONFORMANCE_WINDOW_FLAG]) {
		h->stateful->crop.left = 2 * p[P_CONF_WIN_LEFT_OFFSET];
		h->stateful->crop.top = 2 * p[P_CONF_WIN_TOP_OFFSET];
		h->stateful->crop.width -= h->stateful->crop.left + 2 * p[P_CONF_WIN_RIGHT_OFFSET];
		h->stateful->crop.height -= h->stateful->crop.top + 2 * p[P_CONF_WIN_BOTTOM_OFFSET];
	}
	h->dpb.reorder_limit = p[P_SPS_NUM_REORDER_PICS_0];
	WRITE_ONCE(aml_vdec_stateful(ctx)->min_capture, h->dpb.reorder_limit + 2);
	h->stateful->format_known = true;
	h->configured = false;
	ctx->pix_fmt[AML_FMT_DST] = h->stateful->format;
	ctx->compose = h->stateful->crop;
	ctx->pic_info.coded_width = h->stateful->format.width;
	ctx->pic_info.coded_height = h->stateful->format.height;
	WRITE_ONCE(aml_vdec_stateful(ctx)->wait_capture, !capture_matches);
	/* Publish the reason before LAST can wake a CAPTURE waiter. */
	v4l2_event_queue_fh(&ctx->fh, &event);
	if (end_capture) {
		if (h->dpb.pending)
			h265_flush_pending(h, VB2_BUF_STATE_DONE, true);
		else
			h265_end_capture(ctx, false);
	}
}

int aml_hevc_job_ready(struct aml_vdec_ctx *ctx)
{
	struct aml_hevc_ctx *h = ctx->codec_priv;

	if (!h || READ_ONCE(aml_vdec_stateful(ctx)->wait_capture))
		return 0;
	if (h->stateful->format_known && (!ctx->is_cap_streamon ||
					  !v4l2_m2m_num_dst_bufs_ready(ctx->m2m_ctx)))
		return 0;
	return 1;
}

void aml_hevc_device_run(struct aml_vdec_ctx *ctx)
{
	struct aml_hevc_ctx *h = ctx->codec_priv;
	struct vb2_v4l2_buffer *src, *dst;
	bool drain;
	int ret, slot;

	/* Queue allocation locks precede the stateful lock, as in vb2 queue setup. */
	ret = aml_vdec_get_buffer_pins(ctx);
	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	if (ret || ctx->decode_failed || ctx->dev->dec_hw->faulted) {
		ret = ret ?: -EIO;
		goto error;
	}
	if (h->stateful->format_known && !h->configured) {
		ret = aml_hevc_configure(ctx);
		if (ret)
			goto error;
	}
	ret = aml_hevc_dec_run(ctx);
	if (ret < 0)
		goto error;
	if (ret == 1) {
		h265_source_change(h);
		goto finish;
	}
	src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	drain = v4l2_m2m_is_last_draining_src_buf(ctx->m2m_ctx, src);
	if (h->have_picture) {
		dst = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);
		v4l2_m2m_buf_copy_metadata(src, dst);
		dst->field = V4L2_FIELD_NONE;
		dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
				V4L2_BUF_FLAG_BFRAME);
		dst->flags |= h->dpb.slice_type == H265_I_SLICE ? V4L2_BUF_FLAG_KEYFRAME :
			h->dpb.slice_type == H265_P_SLICE ?
			V4L2_BUF_FLAG_PFRAME : V4L2_BUF_FLAG_BFRAME;
		h->capture[h->dpb.cur] = dst;
		h265_queue_output(&h->dpb, h->dpb.cur);
	}
	v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
	src->sequence = ctx->sequence_out++;
	if (drain) {
		if (h->dpb.pending) {
			aml_hevc_eos(ctx);
			h265_flush_pending(h, VB2_BUF_STATE_DONE, true);
			v4l2_m2m_mark_stopped(ctx->m2m_ctx);
		} else {
			h265_end_capture(ctx, true);
		}
	} else {
		while ((slot = h265_next_output(&h->dpb, false)) >= 0)
			h265_output(h, slot, VB2_BUF_STATE_DONE, false);
	}
	/* Publish drain events before either queue can report its final buffer. */
	v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
	goto finish;
error:
	ctx->decode_failed = true;
	if (h)
		h265_flush_pending(h, VB2_BUF_STATE_ERROR, false);
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

int aml_hevc_decoder_cmd(struct file *file, void *priv, struct v4l2_decoder_cmd *cmd)
{
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);
	struct aml_hevc_ctx *h;
	int ret;

	ret = v4l2_m2m_ioctl_try_decoder_cmd(file, priv, cmd);
	if (ret)
		return ret;
	mutex_lock(&aml_vdec_stateful(ctx)->lock);
	h = ctx->codec_priv;
	if (cmd->cmd == V4L2_DEC_CMD_STOP && !ctx->m2m_ctx->is_draining &&
	    !ctx->m2m_ctx->has_stopped && !v4l2_m2m_next_src_buf(ctx->m2m_ctx)) {
		if (h)
			h265_flush_pending(h, VB2_BUF_STATE_DONE, false);
		/* The generic helper supplies LAST after all reordered pictures. */
		if (v4l2_m2m_next_dst_buf(ctx->m2m_ctx)) {
			struct vb2_buffer *vb = &v4l2_m2m_next_dst_buf(ctx->m2m_ctx)->vb2_buf;
			unsigned int i;

			for (i = 0; i < vb->num_planes; i++)
				vb2_set_plane_payload(vb, i, 0);
			aml_hevc_eos(ctx);
		}
	}
	ret = v4l2_m2m_ioctl_decoder_cmd(file, priv, cmd);
	mutex_unlock(&aml_vdec_stateful(ctx)->lock);
	return ret;
}

static const u32 hevc_formats[] = { V4L2_PIX_FMT_HEVC, V4L2_PIX_FMT_NV12M };

const struct aml_vdec_node_desc aml_vdec_hevc_node = {
	.interface = &aml_vdec_stateful_interface,
	.formats = hevc_formats,
	.num_formats = ARRAY_SIZE(hevc_formats),
	.default_output = V4L2_PIX_FMT_HEVC,
	.default_capture = V4L2_PIX_FMT_NV12M,
	.output_flags = V4L2_FMT_FLAG_DYN_RESOLUTION,
	.events = BIT(V4L2_EVENT_SOURCE_CHANGE) | BIT(V4L2_EVENT_EOS),
	.buffered_capture = true,
	.try_format = aml_hevc_try_fmt,
	.get_selection = aml_hevc_get_selection,
	.decoder_cmd = aml_hevc_decoder_cmd,
	.start_capture = aml_hevc_start_capture,
	.cancel_output = aml_hevc_cancel_output,
	.last_buffer = aml_hevc_last_buffer_done,
	.run = aml_hevc_device_run,
	.job_ready = aml_hevc_job_ready,
};
