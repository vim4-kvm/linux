// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Motion-JPEG decoding on the Amlogic legacy VDEC core.
 *
 * Ported from the reference's vmjpeg_multi.c.  The microcode parses the whole
 * JPEG itself - markers, tables, scan - so every OUTPUT buffer is simply one
 * complete JPEG picture and needs no controls.  It writes planar 4:2:0 into
 * three canvases (Y, U, V), so the only CAPTURE format is YUV420 (I420).
 * Only 8-bit sequential 4:2:0 pictures are currently supported.
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

#include "aml_vdec.h"
#include "aml_vdec_hw.h"
#include "mjpeg.h"

#define MJPEG_TIMEOUT_MS	500

struct aml_mjpeg_ctx {
	bool picture_ready;
	struct aml_vdec_ctx *v4l2_ctx;

	void *stage_vaddr;
	dma_addr_t stage_paddr;
	u32 stage_size;

	int canvas[3];
};

static void mjpeg_canvas_free(struct aml_vdec_hw *hw, struct aml_mjpeg_ctx *m)
{
	int i;

	if (hw->has_internal_canvas)
		return;
	for (i = 0; i < ARRAY_SIZE(m->canvas); i++) {
		if (m->canvas[i] >= 0)
			meson_canvas_free(hw->canvas, m->canvas[i]);
		m->canvas[i] = -1;
	}
}

/* Y, U, V canvases for the capture buffer; returns the spec or ~0. */
static u32 mjpeg_canvas_setup(struct aml_vdec_ctx *ctx, struct vb2_buffer *vb)
{
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	struct aml_mjpeg_ctx *m = ctx->codec_priv;
	struct v4l2_pix_format_mplane *pf = &ctx->pix_fmt[AML_FMT_DST];
	u32 w = pf->plane_fmt[0].bytesperline;
	u32 h = pf->height;
	dma_addr_t addr[3];
	u32 cw[3] = { w, w / 2, w / 2 };
	u32 ch[3] = { h, h / 2, h / 2 };
	int i;

	addr[0] = vb2_dma_contig_plane_dma_addr(vb, 0);
	addr[1] = addr[0] + w * h;
	addr[2] = addr[1] + (w / 2) * (h / 2);

	for (i = 0; i < 3; i++) {
		u8 idx;

		if (hw->has_internal_canvas) {
			if (aml_vdec_write_internal_canvas(hw, i, addr[i], cw[i], ch[i]))
				return ~0u;
			m->canvas[i] = i;
			continue;
		}
		if (meson_canvas_alloc(hw->canvas, &idx)) {
			mjpeg_canvas_free(hw, m);
			return ~0u;
		}
		m->canvas[i] = idx;
		meson_canvas_config(hw->canvas, idx, addr[i], cw[i], ch[i],
				    MESON_CANVAS_WRAP_NONE,
				    MESON_CANVAS_BLKMODE_LINEAR,
				    MESON_CANVAS_ENDIAN_SWAP64);
		regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_CANVAS_BLK32,
			     (1 << 11) | (1 << 8) | idx);
	}

	return m->canvas[0] | (m->canvas[1] << 8) | (m->canvas[2] << 16);
}

/*
 * The microcode only handles baseline (SOF0/SOF1) 4:2:0 Y/Cb/Cr: a 4:2:2,
 * 4:4:4 or progressive picture decodes "successfully" into garbage, and a
 * grey one fails.  Check the frame header and refuse the rest up front.
 */
VISIBLE_IF_KUNIT bool mjpeg_supported(const u8 *p, u32 len, u32 max_width, u32 max_height)
{
	u32 i = 2;

	if (!p || len < 4 || p[0] != 0xff || p[1] != 0xd8)
		return false;
	while (i + 4 <= len) {
		u8 marker;
		u32 seg, width, height;

		if (p[i] != 0xff)
			return false;
		marker = p[i + 1];
		if (marker == 0xff || marker == 0x01 ||
		    (marker >= 0xd0 && marker <= 0xd7)) {
			i += marker == 0xff ? 1 : 2;
			continue;
		}
		seg = (p[i + 2] << 8) | p[i + 3];
		if (seg < 2 || seg > len - i - 2)
			return false;
		if (marker == 0xc0 || marker == 0xc1) {
			if (seg != 17 || p[i + 4] != 8 ||
			    p[i + 9] != 3)
				break;
			/* Y 2x2, Cb 1x1, Cr 1x1 */
			if (p[i + 11] != 0x22 || p[i + 14] != 0x11 ||
			    p[i + 17] != 0x11)
				break;
			height = (p[i + 5] << 8) | p[i + 6];
			width = (p[i + 7] << 8) | p[i + 8];
			return width && height && width <= max_width && height <= max_height;
		}
		if ((marker >= 0xc2 && marker <= 0xcf && marker != 0xc4 &&
		     marker != 0xc8 && marker != 0xcc) || marker == 0xda)
			break;
		i += 2 + seg;
	}
	return false;
}
EXPORT_SYMBOL_IF_KUNIT(mjpeg_supported);

static int mjpeg_prepare_input(struct aml_vdec_ctx *ctx,
			       struct vb2_v4l2_buffer *src)
{
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	struct aml_mjpeg_ctx *m = ctx->codec_priv;
	struct vb2_buffer *vb = &src->vb2_buf;
	const u8 *in = vb2_plane_vaddr(vb, 0);
	u32 len = vb2_get_plane_payload(vb, 0);
	u32 need;
	bool pad;
	u8 *p;

	if (!in || len < 4 || len > vb2_plane_size(vb, 0) ||
	    check_add_overflow(len, VLD_PADDING_SIZE + 64u + 8u, &need) ||
	    need > U32_MAX - SZ_64K - PAGE_SIZE || len > U32_MAX / 8 - 6)
		return -EINVAL;
	if (!mjpeg_supported(in, len, ctx->pix_fmt[AML_FMT_DST].width,
			     ctx->pix_fmt[AML_FMT_DST].height))
		return -EINVAL;

	/*
	 * The microcode skips whatever segment follows SOI without looking at
	 * it.  Encoders put APP0/COM there, but a webcam frame or a JPEG rebuilt
	 * from VA-API buffers starts straight with DQT, which then goes missing
	 * and the picture decodes with the wrong quantiser.  Give it a COM
	 * segment to skip.
	 */
	pad = !((in[3] >= 0xe0 && in[3] <= 0xef) || in[3] == 0xfe);

	if (need > m->stage_size) {
		if (m->stage_vaddr)
			dma_free_coherent(hw->dev, m->stage_size,
					  m->stage_vaddr, m->stage_paddr);
		m->stage_size = PAGE_ALIGN(need + SZ_64K);
		m->stage_vaddr = dma_alloc_coherent(hw->dev, m->stage_size,
						    &m->stage_paddr, GFP_KERNEL);
		if (!m->stage_vaddr) {
			m->stage_size = 0;
			return -ENOMEM;
		}
	}

	p = m->stage_vaddr;
	if (pad) {
		static const u8 com[] = { 0xff, 0xd8, 0xff, 0xfe, 0x00, 0x04, 0x00, 0x00 };

		memcpy(p, com, sizeof(com));
		memcpy(p + sizeof(com), in + 2, len - 2);
		len += sizeof(com) - 2;
	} else {
		memcpy(p, in, len);
	}
	memset(p + len, 0, VLD_PADDING_SIZE);

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
		     round_down(m->stage_paddr + len + VLD_PADDING_SIZE,
				VDEC_FIFO_ALIGN));
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_BUF_CNTL, 3);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_BUF_CNTL, 2);
	regmap_write(hw->map[DOS_BUS], VLD_MEM_VIFIFO_CONTROL,
		     (0x11 << 16) | (1 << 10) | (7 << 3));

	regmap_write(hw->map[DOS_BUS], VIFF_BIT_CNT, len * 8);

	return 0;
}

/* The reference's init_scaler(): a unity 4-point triangle in the pscaler. */
static void mjpeg_init_scaler(struct aml_vdec_hw *hw)
{
	static const u32 filt_coef[] = {
		0x20402000, 0x20402000, 0x1f3f2101, 0x1f3f2101,
		0x1e3e2202, 0x1e3e2202, 0x1d3d2303, 0x1d3d2303,
		0x1c3c2404, 0x1c3c2404, 0x1b3b2505, 0x1b3b2505,
		0x1a3a2606, 0x1a3a2606, 0x19392707, 0x19392707,
		0x18382808, 0x18382808, 0x17372909, 0x17372909,
		0x16362a0a, 0x16362a0a, 0x15352b0b, 0x15352b0b,
		0x14342c0c, 0x14342c0c, 0x13332d0d, 0x13332d0d,
		0x12322e0e, 0x12322e0e, 0x11312f0f, 0x11312f0f,
		0x10303010
	};
	/* horizontal/vertical initial info (Y, C) and phase steps */
	static const u8 init_slot[] = { 37, 41, 39, 43 };
	static const u8 step_slot[] = { 36, 40, 38, 42 };
	struct regmap *map = hw->map[DOS_BUS];
	int i;

	regmap_write(map, PSCALE_CTRL, 0xc000);
	regmap_write(map, PSCALE_BMEM_ADDR, 0);
	for (i = 0; i < ARRAY_SIZE(filt_coef); i++) {
		regmap_write(map, PSCALE_BMEM_DAT, 0);
		regmap_write(map, PSCALE_BMEM_DAT, filt_coef[i]);
	}
	for (i = 0; i < ARRAY_SIZE(init_slot); i++) {
		regmap_write(map, PSCALE_BMEM_ADDR, init_slot[i] * 2);
		regmap_write(map, PSCALE_BMEM_DAT, 0x0008);
		regmap_write(map, PSCALE_BMEM_DAT, 0x60000000);
	}
	for (i = 0; i < ARRAY_SIZE(step_slot); i++) {
		regmap_write(map, PSCALE_BMEM_ADDR, step_slot[i] * 2 + 1);
		regmap_write(map, PSCALE_BMEM_DAT, 0x10000);
	}

	regmap_write(map, DOS_SW_RESET0, 1 << 10);
	regmap_write(map, DOS_SW_RESET0, 0);
	/* The T7 revision C pscaler has a separate byte-switch remap. */
	if (hw->canvas_endian_remap)
		regmap_update_bits(map, PSCALE_CTRL2, GENMASK(24, 16), GENMASK(24, 16));
	regmap_write(map, PSCALE_RST, 0x7);
	regmap_write(map, PSCALE_RST, 0x0);
}

static void mjpeg_signal_done(struct aml_vdec_ctx *ctx, int result)
{
	/* The worker stops DMA before returning either buffer to userspace. */
	ctx->decode_result = result;
	ctx->int_cond = 1;
	wake_up_interruptible(&ctx->queue);
}

static irqreturn_t mjpeg_isr(int irq, void *priv)
{
	struct aml_vdec_dev *dev = priv;

	regmap_write(dev->dec_hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_CLR_REG, 1);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t mjpeg_threaded_isr(int irq, void *priv)
{
	struct aml_vdec_dev *dev = priv;
	struct aml_vdec_hw *hw = dev->dec_hw;
	struct aml_mjpeg_ctx *m = hw->curr_ctx;
	struct aml_vdec_ctx *ctx;
	u32 status;

	if (!m)
		return IRQ_HANDLED;
	ctx = m->v4l2_ctx;
	if (ctx->int_cond)
		return IRQ_HANDLED;

	if (read_dos_reg(hw, AV_SCRATCH_D)) {
		regmap_write(hw->map[DOS_BUS], AV_SCRATCH_D, 0);
		return IRQ_HANDLED;
	}

	status = read_dos_reg(hw, MJPEG_DEC_STATUS);
	dev_dbg(hw->dev, "mjpeg irq status %x from %x w %u h %u\n",
		status, read_dos_reg(hw, MJPEG_FROM_AMRISC),
		 read_dos_reg(hw, MJPEG_PIC_WIDTH),
		 read_dos_reg(hw, MJPEG_PIC_HEIGHT));

	/* Picture size report: the buffer is already there, go on. */
	if (status == MJPEG_STATUS_SIZE) {
		u32 w = read_dos_reg(hw, MJPEG_PIC_WIDTH);
		u32 h = read_dos_reg(hw, MJPEG_PIC_HEIGHT);
		struct v4l2_pix_format_mplane *pf = &ctx->pix_fmt[AML_FMT_DST];

		if (!w || !h || w > pf->width || h > pf->height) {
			dev_err(hw->dev, "mjpeg %ux%u does not fit %ux%u\n",
				w, h, pf->plane_fmt[0].bytesperline, pf->height);
			mjpeg_signal_done(ctx, -EINVAL);
			return IRQ_HANDLED;
		}
		regmap_write(hw->map[DOS_BUS], MJPEG_DECODE_PARAM,
			     (h << 4) | 0x8000);
		regmap_write(hw->map[DOS_BUS], MJPEG_DEC_STATUS, 0);
		return IRQ_HANDLED;
	}
	if (status == MJPEG_STATUS_EMPTY || status == MJPEG_STATUS_TIMEOUT) {
		mjpeg_signal_done(ctx, -EIO);
		return IRQ_HANDLED;
	}

	/* A completed frame leaves status clear and reports buffer index zero. */
	if (status || (read_dos_reg(hw, AV_SCRATCH_5) & 0xffffff)) {
		mjpeg_signal_done(ctx, -EIO);
		return IRQ_HANDLED;
	}
	regmap_write(hw->map[DOS_BUS], MJPEG_FROM_AMRISC, 0);
	mjpeg_signal_done(ctx, 0);

	return IRQ_HANDLED;
}

static void mjpeg_restore_hw(struct aml_vdec_ctx *ctx, u32 spec)
{
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	struct regmap *map = hw->map[DOS_BUS];

	regmap_write(map, AV_SCRATCH_4, spec);
	regmap_write(map, AV_SCRATCH_5, 0 | (1 << 24));

	regmap_write(map, DOS_SW_RESET0, (1 << 7) | (1 << 6));
	regmap_write(map, DOS_SW_RESET0, 0);
	mjpeg_init_scaler(hw);

	regmap_write(map, MJPEG_TO_AMRISC, 0);
	regmap_write(map, MJPEG_FROM_AMRISC, 0);
	regmap_write(map, MJPEG_DEC_STATUS, 0);
	regmap_write(map, AV_SCRATCH_D, 0);
	regmap_write(map, MJPEG_STOP_POS, 0);

	regmap_write(map, MCPU_INTR_MSK, 0xffff);
	regmap_write(map, MJPEG_DECODE_PARAM,
		     (ctx->pix_fmt[AML_FMT_DST].height << 4) | 0x8000);

	regmap_write(map, VDEC_ASSIST_MBOX1_CLR_REG, 1);
	regmap_write(map, VDEC_ASSIST_MBOX1_MASK, 1);
	regmap_write(map, VDEC_ASSIST_AMR1_INT8, 8);
	regmap_update_bits(map, MDEC_PIC_DC_CTRL, 1 << 17, 0);
}

int aml_mjpeg_init(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_mjpeg_ctx *m;
	int i;

	if (!vdec_get_hw(ctx->dev))
		return -ENODEV;

	m = kzalloc_obj(*m, GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->v4l2_ctx = ctx;
	for (i = 0; i < ARRAY_SIZE(m->canvas); i++)
		m->canvas[i] = -1;

	ctx->codec_priv = m;
	return 0;
}

void aml_mjpeg_prepare(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);

	hw->hw_ops.irq_handler = mjpeg_isr;
	hw->hw_ops.irq_threaded_func = mjpeg_threaded_isr;
	hw->hw_ops.load_firmware_ex = NULL;
}

void aml_mjpeg_finish(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;

	mjpeg_canvas_free(vdec_get_hw(ctx->dev), ctx->codec_priv);
}

void aml_mjpeg_exit(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_mjpeg_ctx *m = ctx->codec_priv;
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
	aml_mjpeg_finish(ctx);
	if (m->stage_vaddr)
		dma_free_coherent(hw->dev, m->stage_size,
				  m->stage_vaddr, m->stage_paddr);

	kfree(m);
	ctx->codec_priv = NULL;
}

int aml_mjpeg_dec_run(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	struct vb2_v4l2_buffer *src, *dst;
	u32 spec;
	long ret;

	if (ctx->pix_fmt[AML_FMT_DST].pixelformat != V4L2_PIX_FMT_YUV420) {
		dev_err(hw->dev, "mjpeg: capture format must be YUV420\n");
		return -EINVAL;
	}

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst)
		return -EINVAL;

	ret = mjpeg_prepare_input(ctx, src);
	if (ret)
		return ret;

	spec = mjpeg_canvas_setup(ctx, &dst->vb2_buf);
	if (spec == ~0u)
		return -ENOMEM;
	mjpeg_restore_hw(ctx, spec);

	dev_dbg(hw->dev, "mjpeg run %lu bytes spec %06x\n",
		vb2_get_plane_payload(&src->vb2_buf, 0), spec);

	ctx->int_cond = 0;
	ctx->decode_result = -EIO;
	regmap_update_bits(hw->map[DOS_BUS], VLD_MEM_VIFIFO_CONTROL, 0x6, 0x6);
	aml_start_vdec_hw(hw);

	ret = wait_event_interruptible_timeout(ctx->queue, ctx->int_cond,
					       msecs_to_jiffies(MJPEG_TIMEOUT_MS));
	ctx->int_cond = 0;
	if (!ret) {
		dev_err(hw->dev,
			"mjpeg timeout: status %x from %x pc %x lvl %x rp %x wp %x bitcnt %x\n",
			read_dos_reg(hw, MJPEG_DEC_STATUS),
			read_dos_reg(hw, MJPEG_FROM_AMRISC),
			read_dos_reg(hw, MPC_E),
			read_dos_reg(hw, VLD_MEM_VIFIFO_LEVEL),
			read_dos_reg(hw, VLD_MEM_VIFIFO_RP),
			read_dos_reg(hw, VLD_MEM_VIFIFO_WP),
			read_dos_reg(hw, VIFF_BIT_CNT));
		ctx->decode_failed = true;
		return -ETIMEDOUT;
	}

	if (ret < 0)
		return ret;
	if (!ctx->decode_result)
		vb2_set_plane_payload(&dst->vb2_buf, 0,
				      ctx->pix_fmt[AML_FMT_DST].plane_fmt[0].sizeimage);
	return ctx->decode_result;
}

static int mjpeg_submit(struct aml_vdec_ctx *ctx, struct vb2_v4l2_buffer *src,
			struct vb2_v4l2_buffer *dst, struct aml_vdec_decode_result *result)
{
	struct aml_mjpeg_ctx *m = ctx->codec_priv;
	int ret = aml_mjpeg_dec_run(ctx);

	m->picture_ready = !ret;
	result->picture = !ret;
	return ret;
}

static int mjpeg_get_frame(struct aml_vdec_ctx *ctx, bool flush, struct aml_vdec_frame *frame)
{
	struct aml_mjpeg_ctx *m = ctx->codec_priv;

	if (!m->picture_ready)
		return 0;
	frame->buffer = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	frame->final = true;
	m->picture_ready = false;
	return 1;
}

const struct aml_vdec_stateful_ops aml_mjpeg_stateful_ops = {
	.submit = mjpeg_submit,
	.get_frame = mjpeg_get_frame,
};

static void jpeg_set_colors(struct aml_vdec_ctx *ctx, enum v4l2_buf_type type,
			    struct v4l2_pix_format_mplane *fmt)
{
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->xfer_func = V4L2_XFER_FUNC_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	/* The JPEG firmware converts full-range input to limited-range YUV. */
	fmt->quantization = V4L2_TYPE_IS_OUTPUT(type) ?
		V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
}

static const u32 jpeg_formats[] = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUV420 };

const struct aml_vdec_node_desc aml_vdec_jpeg_node = {
	.interface = &aml_vdec_stateful_interface,
	.stateful_ops = &aml_mjpeg_stateful_ops,
	.formats = jpeg_formats,
	.num_formats = ARRAY_SIZE(jpeg_formats),
	.default_output = V4L2_PIX_FMT_MJPEG,
	.default_capture = V4L2_PIX_FMT_YUV420,
	.set_colors = jpeg_set_colors,
};
