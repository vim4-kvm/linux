// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * MPEG-1/2 decoding on the Amlogic legacy VDEC core, stateless.
 *
 * Ported from the reference's frame-based vmpeg12_multi.c.  That microcode
 * parses the elementary stream itself - sequence header, extensions, picture
 * header and the slices - so the parsed V4L2 controls are turned back into
 * the headers they came from and staged in front of the slice data.  The
 * microcode decodes into whatever canvas REC_CANVAS_ADDR names and predicts
 * from MREG_REF0 (older) and MREG_REF1 (newer) anchor pictures; reordering
 * and the reference bookkeeping are left to userspace, like every other
 * stateless codec.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "aml_vdec_drv.h"
#include "aml_vdec_hw.h"
#include "mpeg12.h"

#define MPEG12_TIMEOUT_MS	500
#define MPEG12_HDR_MAX		1024
#define MPEG12_TAIL		4

struct mpeg12_canvas {
	int y;
	int uv;
};

struct aml_mpeg12_ctx {
	struct aml_vdec_ctx *v4l2_ctx;

	void *wks_vaddr;
	dma_addr_t wks_paddr;
	void *cc_vaddr;
	dma_addr_t cc_paddr;

	void *stage_vaddr;
	dma_addr_t stage_paddr;
	u32 stage_size;

	/* rec, ref0, ref1 */
	struct mpeg12_canvas canvas[3];

	u32 pic_info;
	u32 status;
	u32 decoded;

	/*
	 * Field pictures come as two requests into one held capture buffer.
	 * The first field is only stashed (headers + slices); the microcode
	 * decodes both fields from one chunk when the second one arrives,
	 * like the vendor's frame-based input, and interrupts once.
	 */
	u8 *fld_buf;
	u32 fld_len;
	u32 fld_cap;
	u64 fld_ts;
	bool fld_pending;
	struct v4l2_ctrl_mpeg2_picture fld_pic;
};

struct bitw {
	u8 *buf;
	u32 bits;
};

static void put_bits(struct bitw *bw, u32 n, u32 v)
{
	while (n--) {
		u32 byte = bw->bits >> 3;
		u32 bit = 7 - (bw->bits & 7);

		if (!(bw->bits & 7))
			bw->buf[byte] = 0;
		if ((v >> n) & 1)
			bw->buf[byte] |= 1 << bit;
		bw->bits++;
	}
}

static void put_align(struct bitw *bw)
{
	while (bw->bits & 7)
		put_bits(bw, 1, 0);
}

static void put_start(struct bitw *bw, u8 code)
{
	put_align(bw);
	put_bits(bw, 24, 1);
	put_bits(bw, 8, code);
}

static void *mpeg12_ctrl(struct aml_vdec_ctx *ctx, u32 id)
{
	struct v4l2_ctrl *ctrl = v4l2_ctrl_find(&ctx->ctrl_handler, id);

	return ctrl ? ctrl->p_cur.p : NULL;
}

/*
 * Rebuild sequence header + sequence extension + picture header + picture
 * coding extension. Matrix controls already use bitstream zigzag order.
 */
static u32 mpeg12_write_headers(u8 *buf,
				const struct v4l2_ctrl_mpeg2_sequence *seq,
				const struct v4l2_ctrl_mpeg2_picture *pic,
				const struct v4l2_ctrl_mpeg2_quantisation *quant,
				bool with_seq)
{
	struct bitw bw = { .buf = buf };
	u32 vbv = seq->vbv_buffer_size;
	int i;

	if (!with_seq)
		goto picture;

	put_start(&bw, 0xb3);
	put_bits(&bw, 12, seq->horizontal_size & 0xfff);
	put_bits(&bw, 12, seq->vertical_size & 0xfff);
	put_bits(&bw, 4, 1);		/* aspect ratio: square */
	put_bits(&bw, 4, 3);		/* 25 fps, informational */
	put_bits(&bw, 18, 0x3ffff);	/* bit_rate: variable */
	put_bits(&bw, 1, 1);
	put_bits(&bw, 10, vbv & 0x3ff);
	put_bits(&bw, 1, 0);		/* constrained_parameters_flag */
	put_bits(&bw, 1, 1);
	for (i = 0; i < 64; i++)
		put_bits(&bw, 8, quant->intra_quantiser_matrix[i]);
	put_bits(&bw, 1, 1);
	for (i = 0; i < 64; i++)
		put_bits(&bw, 8, quant->non_intra_quantiser_matrix[i]);

	put_start(&bw, 0xb5);
	put_bits(&bw, 4, 1);		/* sequence extension */
	put_bits(&bw, 8, seq->profile_and_level_indication ?
		 seq->profile_and_level_indication & 0xff : 0x48);
	put_bits(&bw, 1, !!(seq->flags & V4L2_MPEG2_SEQ_FLAG_PROGRESSIVE));
	put_bits(&bw, 2, seq->chroma_format ? seq->chroma_format : 1);
	put_bits(&bw, 2, (seq->horizontal_size >> 12) & 3);
	put_bits(&bw, 2, (seq->vertical_size >> 12) & 3);
	put_bits(&bw, 12, 0);		/* bit_rate_extension */
	put_bits(&bw, 1, 1);
	put_bits(&bw, 8, (vbv >> 10) & 0xff);
	put_bits(&bw, 1, 0);		/* low_delay */
	put_bits(&bw, 2, 0);
	put_bits(&bw, 5, 0);

picture:
	put_start(&bw, 0x00);
	put_bits(&bw, 10, 0);		/* temporal_reference */
	put_bits(&bw, 3, pic->picture_coding_type);
	put_bits(&bw, 16, 0xffff);	/* vbv_delay */
	if (pic->picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_P ||
	    pic->picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_B) {
		put_bits(&bw, 1, 0);
		put_bits(&bw, 3, 7);
	}
	if (pic->picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_B) {
		put_bits(&bw, 1, 0);
		put_bits(&bw, 3, 7);
	}
	put_bits(&bw, 1, 0);		/* extra_bit_picture */

	put_start(&bw, 0xb5);
	put_bits(&bw, 4, 8);		/* picture coding extension */
	put_bits(&bw, 4, pic->f_code[0][0]);
	put_bits(&bw, 4, pic->f_code[0][1]);
	put_bits(&bw, 4, pic->f_code[1][0]);
	put_bits(&bw, 4, pic->f_code[1][1]);
	put_bits(&bw, 2, pic->intra_dc_precision);
	put_bits(&bw, 2, pic->picture_structure);
	put_bits(&bw, 1, !!(pic->flags & V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST));
	put_bits(&bw, 1, !!(pic->flags & V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT));
	put_bits(&bw, 1, !!(pic->flags & V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV));
	put_bits(&bw, 1, !!(pic->flags & V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE));
	put_bits(&bw, 1, !!(pic->flags & V4L2_MPEG2_PIC_FLAG_INTRA_VLC));
	put_bits(&bw, 1, !!(pic->flags & V4L2_MPEG2_PIC_FLAG_ALT_SCAN));
	put_bits(&bw, 1, !!(pic->flags & V4L2_MPEG2_PIC_FLAG_REPEAT_FIRST));
	put_bits(&bw, 1, !!(pic->flags & V4L2_MPEG2_PIC_FLAG_PROGRESSIVE));
	put_bits(&bw, 1, !!(pic->flags & V4L2_MPEG2_PIC_FLAG_PROGRESSIVE));
	put_bits(&bw, 1, 0);		/* composite_display_flag */
	put_align(&bw);
	/* Preserve picture-level matrix changes, including the second field. */
	put_start(&bw, 0xb5);
	put_bits(&bw, 4, 3);
	put_bits(&bw, 1, 1);
	for (i = 0; i < 64; i++)
		put_bits(&bw, 8, quant->intra_quantiser_matrix[i]);
	put_bits(&bw, 1, 1);
	for (i = 0; i < 64; i++)
		put_bits(&bw, 8, quant->non_intra_quantiser_matrix[i]);
	put_bits(&bw, 1, 1);
	for (i = 0; i < 64; i++)
		put_bits(&bw, 8, quant->chroma_intra_quantiser_matrix[i]);
	put_bits(&bw, 1, 1);
	for (i = 0; i < 64; i++)
		put_bits(&bw, 8, quant->chroma_non_intra_quantiser_matrix[i]);
	put_align(&bw);

	return bw.bits >> 3;
}

/* Stash the first field of a pair: headers and slices, for the next job. */
static int mpeg12_stash_field(struct aml_vdec_ctx *ctx,
			      const struct v4l2_ctrl_mpeg2_sequence *seq,
			      const struct v4l2_ctrl_mpeg2_picture *pic,
			      const struct v4l2_ctrl_mpeg2_quantisation *quant,
			      struct vb2_v4l2_buffer *src)
{
	struct aml_mpeg12_ctx *m = ctx->codec_priv;
	u32 payload = vb2_get_plane_payload(&src->vb2_buf, 0);
	u32 need;
	u32 hdr;

	if (check_add_overflow(payload, MPEG12_HDR_MAX, &need))
		return -EOVERFLOW;
	if (need > m->fld_cap) {
		kvfree(m->fld_buf);
		m->fld_cap = 0;
		m->fld_buf = kvmalloc(need, GFP_KERNEL);
		if (!m->fld_buf)
			return -ENOMEM;
		m->fld_cap = need;
	}
	hdr = mpeg12_write_headers(m->fld_buf, seq, pic, quant, true);
	memcpy(m->fld_buf + hdr, vb2_plane_vaddr(&src->vb2_buf, 0), payload);
	m->fld_len = hdr + payload;
	m->fld_pic = *pic;
	m->fld_ts = src->vb2_buf.timestamp;
	m->fld_pending = true;
	return 0;
}

static int mpeg12_prepare_input(struct aml_vdec_ctx *ctx,
				const struct v4l2_ctrl_mpeg2_sequence *seq,
				const struct v4l2_ctrl_mpeg2_picture *pic,
				const struct v4l2_ctrl_mpeg2_quantisation *quant,
				bool pair)
{
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	struct aml_mpeg12_ctx *m = ctx->codec_priv;
	struct vb2_v4l2_buffer *src;
	struct vb2_buffer *vb;
	u32 payload, hdr, need, len;
	u8 *p;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	if (!src)
		return -EINVAL;
	vb = &src->vb2_buf;
	payload = vb2_get_plane_payload(vb, 0);

	if (check_add_overflow(payload, MPEG12_HDR_MAX + MPEG12_TAIL +
			       VLD_PADDING_SIZE + VDEC_FIFO_ALIGN, &need) ||
	    (pair && check_add_overflow(need, m->fld_len, &need)) ||
	    need > U32_MAX - VDEC_FIFO_ALIGN)
		return -EOVERFLOW;
	need = round_up(need, VDEC_FIFO_ALIGN);
	if (need > m->stage_size) {
		if (m->stage_vaddr)
			dma_free_coherent(hw->dev, m->stage_size,
					  m->stage_vaddr, m->stage_paddr);
		m->stage_size = need;
		m->stage_vaddr = dma_alloc_coherent(hw->dev, m->stage_size,
						    &m->stage_paddr, GFP_KERNEL);
		if (!m->stage_vaddr) {
			m->stage_size = 0;
			return -ENOMEM;
		}
	}

	p = m->stage_vaddr;
	len = 0;
	if (pair) {
		memcpy(p, m->fld_buf, m->fld_len);
		len = m->fld_len;
	}
	hdr = mpeg12_write_headers(p + len, seq, pic, quant, !pair);
	memcpy(p + len + hdr, vb2_plane_vaddr(vb, 0), payload);
	len += hdr + payload;
	/* The microcode only ends the last slice on the next start code. */
	p[len] = 0;
	p[len + 1] = 0;
	p[len + 2] = 1;
	p[len + 3] = 0;
	memset(p + len + MPEG12_TAIL, 0, VLD_PADDING_SIZE);

	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_CONTROL, 0);
	regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0,
		     (1 << 5) | (1 << 4) | (1 << 3));
	regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, 0);
	regmap_write(hw->map[DOS_BUS], POWER_CTL_VLD, 1 << 4);

	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_START_PTR, m->stage_paddr);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_END_PTR,
		     m->stage_paddr + m->stage_size - VDEC_FIFO_ALIGN);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_CURR_PTR, m->stage_paddr);

	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_CONTROL, 1);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_CONTROL, 0);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_BUF_CNTL, 2);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_RP, m->stage_paddr);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_WP,
		     round_down(m->stage_paddr + len + MPEG12_TAIL +
				VLD_PADDING_SIZE, VDEC_FIFO_ALIGN));
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_BUF_CNTL, 3);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_BUF_CNTL, 2);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_CONTROL,
		     (0x11 << 16) | (1 << 10) | (7 << 3));

	regmap_write(hw->map[DOS_BUS], VIFF_BIT_CNT, len * 8);

	return 0;
}

static void mpeg12_canvas_free(struct aml_vdec_hw *hw, struct aml_mpeg12_ctx *m)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(m->canvas); i++) {
		if (!hw->has_internal_canvas && m->canvas[i].y >= 0)
			meson_canvas_free(hw->canvas, m->canvas[i].y);
		if (!hw->has_internal_canvas && m->canvas[i].uv >= 0)
			meson_canvas_free(hw->canvas, m->canvas[i].uv);
		m->canvas[i].y = -1;
		m->canvas[i].uv = -1;
	}
}

/* Returns the canvas spec (y | uv << 8 | uv << 16) for the buffer, or ~0. */
static u32 mpeg12_canvas_setup(struct aml_vdec_ctx *ctx,
			       struct mpeg12_canvas *c, struct vb2_buffer *vb)
{
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	u32 w = ctx->pic_info.coded_width;
	u32 h = ctx->pic_info.coded_height;
	dma_addr_t y, uv;
	u8 yi, ci;

	y = vb2_dma_contig_plane_dma_addr(vb, 0);
	if (ctx->pic_info.plane_num > 1)
		uv = vb2_dma_contig_plane_dma_addr(vb, 1);
	else
		uv = y + ctx->pic_info.fb_size[0];
	if (hw->has_internal_canvas) {
		struct aml_mpeg12_ctx *m = ctx->codec_priv;

		yi = (c - m->canvas) * 2;
		ci = yi + 1;
		if (aml_vdec_write_internal_canvas(hw, yi, y, w, h) ||
		    aml_vdec_write_internal_canvas(hw, ci, uv, w, h / 2))
			return ~0u;
		goto configure;
	}

	/* a slot reused within one job must not leak its old canvases */
	if (c->y >= 0)
		meson_canvas_free(hw->canvas, c->y);
	if (c->uv >= 0)
		meson_canvas_free(hw->canvas, c->uv);
	c->y = -1;
	c->uv = -1;

	if (meson_canvas_alloc(hw->canvas, &yi))
		return ~0u;
	if (meson_canvas_alloc(hw->canvas, &ci)) {
		meson_canvas_free(hw->canvas, yi);
		return ~0u;
	}
	c->y = yi;
	c->uv = ci;

	meson_canvas_config(hw->canvas, yi, y, w, h, MESON_CANVAS_WRAP_NONE,
			    MESON_CANVAS_BLKMODE_LINEAR, MESON_CANVAS_ENDIAN_SWAP64);
	meson_canvas_config(hw->canvas, ci, uv, w, h / 2, MESON_CANVAS_WRAP_NONE,
			    MESON_CANVAS_BLKMODE_LINEAR, MESON_CANVAS_ENDIAN_SWAP64);
configure:
	dev_dbg(hw->dev, "mpeg12 canvas %u Y=%pad UV=%pad %ux%u planes=%u\n",
		yi, &y, &uv, w, h, ctx->pic_info.plane_num);
	regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_CANVAS_BLK32,
		     (1 << 11) | (1 << 8) | yi);
	regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_CANVAS_BLK32,
		     (1 << 11) | (1 << 8) | ci);

	return yi | (ci << 8) | (ci << 16);
}

static u32 mpeg12_ref_spec(struct aml_vdec_ctx *ctx, struct mpeg12_canvas *c,
			   u64 ts)
{
	struct vb2_queue *vq = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	struct vb2_buffer *vb = vb2_find_buffer(vq, ts);

	if (!vb)
		return 0xffffffff;
	return mpeg12_canvas_setup(ctx, c, vb);
}

static void mpeg12_job_done(struct aml_vdec_ctx *ctx, int result)
{
	ctx->decode_result = result;
	ctx->int_cond = 1;
	wake_up_interruptible(&ctx->queue);
}

static int mpeg12_reference_canvases(struct aml_vdec_ctx *ctx,
				     const struct v4l2_ctrl_mpeg2_picture *pic,
				     bool pair, u32 *ref0, u32 *ref1)
{
	struct aml_mpeg12_ctx *m = ctx->codec_priv;
	const struct v4l2_ctrl_mpeg2_picture *first = pair ? &m->fld_pic : pic;

	/*
	 * The microcode decodes both fields in one run. An I/P pair predicts
	 * its second field from its first field, without an external anchor.
	 * Selecting the second field here rejects the initial I/P pair before
	 * any picture exists in the DPB.
	 */
	*ref0 = ~0u;
	*ref1 = ~0u;
	if (first->picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_P) {
		*ref1 = mpeg12_ref_spec(ctx, &m->canvas[2], first->forward_ref_ts);
		*ref0 = *ref1;
		if (*ref1 == ~0u)
			return -EINVAL;
	} else if (first->picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_B) {
		*ref0 = mpeg12_ref_spec(ctx, &m->canvas[1], first->forward_ref_ts);
		*ref1 = mpeg12_ref_spec(ctx, &m->canvas[2], first->backward_ref_ts);
		if (*ref0 == ~0u || *ref1 == ~0u)
			return -EINVAL;
	}
	return 0;
}

static irqreturn_t mpeg12_isr(int irq, void *priv)
{
	struct aml_vdec_dev *dev = priv;

	regmap_write(dev->dec_hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_CLR_REG, 1);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t mpeg12_threaded_isr(int irq, void *priv)
{
	struct aml_vdec_dev *dev = priv;
	struct aml_vdec_hw *hw = dev->dec_hw;
	struct aml_mpeg12_ctx *m = hw->curr_ctx;
	struct aml_vdec_ctx *ctx;
	u32 reg;

	if (!m)
		return IRQ_HANDLED;
	ctx = m->v4l2_ctx;
	if (ctx->int_cond)
		return IRQ_HANDLED;

	if (read_dos_reg(hw, MREG_DEBUG)) {
		regmap_write(hw->map[DOS_BUS], MREG_DEBUG, 0);
		return IRQ_HANDLED;
	}

	/* Sequence info report: the parameters are known already, go on. */
	if (read_dos_reg(hw, MREG_SEQ_REPORT) == 1) {
		regmap_write(hw->map[DOS_BUS], MREG_SEQ_REPORT, 0);
		return IRQ_HANDLED;
	}

	/* User data (closed captions) - nobody consumes it here. */
	if (read_dos_reg(hw, MREG_USERDATA) & (1 << 16)) {
		regmap_write(hw->map[DOS_BUS], MREG_USERDATA, 0);
		return IRQ_HANDLED;
	}

	reg = read_dos_reg(hw, MREG_BUFFEROUT);
	m->status = reg;
	m->pic_info = read_dos_reg(hw, MREG_PIC_INFO);

	dev_dbg(hw->dev,
		"mpeg12 irq out %u info %08x rec %06x w %u h %u err %u\n",
		 reg, m->pic_info, read_dos_reg(hw, REC_CANVAS_ADDR),
		 read_dos_reg(hw, MREG_PIC_WIDTH),
		 read_dos_reg(hw, MREG_PIC_HEIGHT),
		 read_dos_reg(hw, MREG_ERROR_COUNT));

	switch (reg) {
	case MPEG12_PIC_DONE:
	case MPEG12_SEQ_END:
		m->decoded++;
		mpeg12_job_done(ctx, (m->pic_info & MPEG12_PICINFO_ERROR) ?
				-EIO : 0);
		break;
	case MPEG12_DATA_EMPTY:
	case MPEG12_DATA_REQUEST:
		mpeg12_job_done(ctx, -EIO);
		break;
	default:
		/* Not ours; leave the microcode waiting for the timeout. */
		return IRQ_HANDLED;
	}
	regmap_write(hw->map[DOS_BUS], MREG_BUFFEROUT, 0);

	return IRQ_HANDLED;
}

static void mpeg12_restore_hw(struct aml_vdec_ctx *ctx)
{
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	struct aml_mpeg12_ctx *m = ctx->codec_priv;
	u32 w = ctx->pic_info.coded_width;
	u32 h = ctx->pic_info.coded_height;

	regmap_write(hw->map[DOS_BUS], MREG_CO_MV_START, m->wks_paddr);
	regmap_write(hw->map[DOS_BUS], MREG_CC_ADDR, m->cc_paddr);

	/*
	 * No context is carried between pictures: the headers are resent with
	 * every one, so the microcode is always started as if fresh.
	 */
	regmap_write(hw->map[DOS_BUS], MPEG1_2_REG, 0);
	regmap_write(hw->map[DOS_BUS], PSCALE_CTRL, 0);
	regmap_write(hw->map[DOS_BUS], PIC_HEAD_INFO, 0x380);
	regmap_write(hw->map[DOS_BUS], M4_CONTROL_REG, 0);
	regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_CLR_REG, 1);
	regmap_write(hw->map[DOS_BUS], MREG_BUFFEROUT, 0);
	regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_MASK, 1);
	regmap_write(hw->map[DOS_BUS], MREG_CMD, (w << 16) | h);
	regmap_write(hw->map[DOS_BUS], MREG_PIC_WIDTH, 0);
	regmap_write(hw->map[DOS_BUS], MREG_PIC_HEIGHT, 0);
	regmap_write(hw->map[DOS_BUS], MREG_SEQ_INFO, 0);
	regmap_write(hw->map[DOS_BUS], F_CODE_REG, 0);
	regmap_write(hw->map[DOS_BUS], SLICE_VER_POS_PIC_TYPE, 0);
	regmap_write(hw->map[DOS_BUS], MB_INFO, 0);
	/* IQ/IDCT state survives the common reset; do not inherit H.264 mode. */
	regmap_write(hw->map[DOS_BUS], VCOP_CTRL_REG, 0);
	regmap_write(hw->map[DOS_BUS], MREG_SIGNAL_TYPE, 0);
	regmap_write(hw->map[DOS_BUS], MREG_ERROR_COUNT, 0);
	/* bit1: report width/height, bit0: CC buffer from the host */
	regmap_write(hw->map[DOS_BUS], MREG_FATAL_ERROR, 3);
	regmap_write(hw->map[DOS_BUS], MREG_WAIT_BUFFER, 0);
	regmap_write(hw->map[DOS_BUS], MREG_SEQ_REPORT, 0);
	regmap_write(hw->map[DOS_BUS], MREG_DEBUG, 0);
	regmap_write(hw->map[DOS_BUS], MREG_DECODE_STOP_POS, 0);

	regmap_update_bits(hw->map[DOS_BUS], MDEC_PIC_DC_CTRL, 1 << 17, 1 << 17);
	if (ctx->dec_fmt[AML_FMT_DST].fourcc == V4L2_PIX_FMT_NV21 ||
	    ctx->dec_fmt[AML_FMT_DST].fourcc == V4L2_PIX_FMT_NV21M)
		regmap_update_bits(hw->map[DOS_BUS], MDEC_PIC_DC_CTRL,
				   1 << 16, 1 << 16);
	else
		regmap_update_bits(hw->map[DOS_BUS], MDEC_PIC_DC_CTRL, 1 << 16, 0);

	regmap_write(hw->map[DOS_BUS], MREG_USERDATA, 0);
	/* frame based input, no saved context */
	regmap_write(hw->map[DOS_BUS], MREG_INPUT, 1 << 7);
}

int aml_mpeg12_init(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	struct aml_mpeg12_ctx *m;
	int i;

	if (!hw)
		return -ENODEV;

	m = kzalloc_obj(*m, GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->v4l2_ctx = ctx;
	for (i = 0; i < ARRAY_SIZE(m->canvas); i++) {
		m->canvas[i].y = -1;
		m->canvas[i].uv = -1;
	}

	m->wks_vaddr = dma_alloc_coherent(hw->dev, MPEG12_WORKSPACE_SIZE,
					  &m->wks_paddr, GFP_KERNEL);
	m->cc_vaddr = dma_alloc_coherent(hw->dev, MPEG12_CCBUF_SIZE,
					 &m->cc_paddr, GFP_KERNEL);
	if (!m->wks_vaddr || !m->cc_vaddr) {
		if (m->wks_vaddr)
			dma_free_coherent(hw->dev, MPEG12_WORKSPACE_SIZE,
					  m->wks_vaddr, m->wks_paddr);
		if (m->cc_vaddr)
			dma_free_coherent(hw->dev, MPEG12_CCBUF_SIZE,
					  m->cc_vaddr, m->cc_paddr);
		kfree(m);
		return -ENOMEM;
	}

	ctx->codec_priv = m;
	return 0;
}

void aml_mpeg12_prepare(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);

	hw->hw_ops.irq_handler = mpeg12_isr;
	hw->hw_ops.irq_threaded_func = mpeg12_threaded_isr;
	hw->hw_ops.load_firmware_ex = NULL;
}

void aml_mpeg12_finish(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;

	mpeg12_canvas_free(vdec_get_hw(ctx->dev), ctx->codec_priv);
}

void aml_mpeg12_exit(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_mpeg12_ctx *m = ctx->codec_priv;
	struct aml_vdec_hw *hw;

	if (!m)
		return;
	hw = vdec_get_hw(ctx->dev);
	if (hw->faulted)
		return;

	if (ctx->dos_clk_en && aml_stop_vdec_hw(hw)) {
		aml_vdec_fault(ctx);
		return;
	}
	ctx->dos_clk_en = false;
	aml_mpeg12_finish(ctx);

	if (m->stage_vaddr)
		dma_free_coherent(hw->dev, m->stage_size,
				  m->stage_vaddr, m->stage_paddr);
	dma_free_coherent(hw->dev, MPEG12_WORKSPACE_SIZE,
			  m->wks_vaddr, m->wks_paddr);
	dma_free_coherent(hw->dev, MPEG12_CCBUF_SIZE,
			  m->cc_vaddr, m->cc_paddr);
	kvfree(m->fld_buf);

	kfree(m);
	ctx->codec_priv = NULL;
}

int aml_mpeg12_dec_run(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_mpeg12_ctx *m = ctx->codec_priv;
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	const struct v4l2_ctrl_mpeg2_sequence *seq;
	const struct v4l2_ctrl_mpeg2_picture *pic;
	const struct v4l2_ctrl_mpeg2_quantisation *quant;
	struct vb2_v4l2_buffer *src, *dst;
	u32 rec, ref0 = 0xffffffff, ref1 = 0xffffffff;
	bool field, pair;
	long ret;

	seq = mpeg12_ctrl(ctx, V4L2_CID_STATELESS_MPEG2_SEQUENCE);
	pic = mpeg12_ctrl(ctx, V4L2_CID_STATELESS_MPEG2_PICTURE);
	quant = mpeg12_ctrl(ctx, V4L2_CID_STATELESS_MPEG2_QUANTISATION);
	if (!seq || !pic || !quant)
		return -EINVAL;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst)
		return -EINVAL;
	if (!vb2_get_plane_payload(&src->vb2_buf, 0) ||
	    vb2_get_plane_payload(&src->vb2_buf, 0) > vb2_plane_size(&src->vb2_buf, 0) ||
	    !vb2_plane_vaddr(&src->vb2_buf, 0))
		return -EINVAL;
	if (seq->chroma_format != 1 || !seq->horizontal_size || !seq->vertical_size ||
	    seq->horizontal_size > AML_VDEC_1080P_MAX_W ||
	    seq->vertical_size > AML_VDEC_1080P_MAX_H ||
	    seq->horizontal_size > ctx->pic_info.coded_width ||
	    seq->vertical_size > ctx->pic_info.coded_height)
		return -EINVAL;

	field = pic->picture_structure != V4L2_MPEG2_PIC_FRAME;
	pair = field && m->fld_pending &&
	       m->fld_ts == src->vb2_buf.timestamp &&
	       m->fld_pic.picture_structure != pic->picture_structure;
	if (!pair)
		m->fld_pending = false;

	if (field && !pair &&
	    (src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF)) {
		ret = mpeg12_stash_field(ctx, seq, pic, quant, src);
		if (ret)
			return ret;
		dev_dbg(hw->dev, "mpeg12 stash field %u type %u\n",
			pic->picture_structure,
			 pic->picture_coding_type);
		/* the capture buffer stays held for the second field */
		return 0;
	}
	m->fld_pending = false;
	if (field && !pair)
		return -EINVAL;

	ret = mpeg12_prepare_input(ctx, seq, pic, quant, pair);
	if (ret)
		return ret;

	mpeg12_restore_hw(ctx);

	rec = mpeg12_canvas_setup(ctx, &m->canvas[0], &dst->vb2_buf);
	if (rec == ~0u)
		return -ENOMEM;

	ret = mpeg12_reference_canvases(ctx, pic, pair, &ref0, &ref1);
	if (ret)
		return ret;
	regmap_write(hw->map[DOS_BUS], MREG_REF0, ref0);
	regmap_write(hw->map[DOS_BUS], MREG_REF1, ref1);
	regmap_write(hw->map[DOS_BUS], REC_CANVAS_ADDR, rec);
	regmap_write(hw->map[DOS_BUS], ANC2_CANVAS_ADDR, rec);

	dev_dbg(hw->dev,
		"mpeg12 run type %u struct %u%s %ux%u rec %06x ref0 %08x ref1 %08x\n",
		 pic->picture_coding_type, pic->picture_structure,
		 pair ? " pair" : "",
		 seq->horizontal_size, seq->vertical_size,
		 rec, ref0, ref1);
	dev_dbg(hw->dev, "mpeg12 state iq=%x vcop=%x dc=%x\n",
		read_dos_reg(hw, IQIDCT_CONTROL), read_dos_reg(hw, VCOP_CTRL_REG),
		read_dos_reg(hw, MDEC_PIC_DC_CTRL));

	ctx->int_cond = 0;
	ctx->decode_result = -EIO;
	regmap_update_bits(hw->map[DOS_BUS], VLD_MEM_VIFIFO_CONTROL, 0x6, 0x6);
	aml_start_vdec_hw(hw);

	ret = wait_event_interruptible_timeout(ctx->queue, ctx->int_cond,
					       msecs_to_jiffies(MPEG12_TIMEOUT_MS));
	ctx->int_cond = 0;
	if (!ret) {
		dev_err(hw->dev,
			"mpeg12 timeout: out %x info %x pc %x lvl %x rp %x wp %x bitcnt %x err %x\n",
			read_dos_reg(hw, MREG_BUFFEROUT),
			read_dos_reg(hw, MREG_PIC_INFO),
			read_dos_reg(hw, MPC_E),
			read_dos_reg(hw, VLD_MEM_VIFIFO_LEVEL),
			read_dos_reg(hw, VLD_MEM_VIFIFO_RP),
			read_dos_reg(hw, VLD_MEM_VIFIFO_WP),
			read_dos_reg(hw, VIFF_BIT_CNT),
			read_dos_reg(hw, MREG_ERROR_COUNT));
		ctx->decode_failed = true;
		return -ETIMEDOUT;
	}
	if (ret < 0)
		return ret;
	return ctx->decode_result;
}
