// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Motion-JPEG decoding on the Amlogic legacy VDEC core.
 *
 * Ported from the reference's vmjpeg_multi.c.  The microcode parses the whole
 * JPEG itself - markers, tables, scan - so every OUTPUT buffer is simply one
 * complete JPEG picture and needs no controls.  It writes planar 4:2:0 into
 * three canvases (Y, U, V), so the only CAPTURE format is YUV420 (I420); a
 * 4:2:2 or 4:4:4 source comes out downsampled to 4:2:0 by the microcode.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "aml_vdec_drv.h"
#include "aml_vdec_hw.h"
#include "mjpeg.h"

#define MJPEG_TIMEOUT_MS	500

struct aml_mjpeg_ctx {
	struct aml_vdec_ctx *v4l2_ctx;

	void *stage_vaddr;
	dma_addr_t stage_paddr;
	u32 stage_size;

	int canvas[3];
	u32 decoded;
};

static void mjpeg_canvas_free(struct aml_vdec_hw *hw, struct aml_mjpeg_ctx *m)
{
	int i;

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
static bool mjpeg_supported(struct device *dev, const u8 *p, u32 len)
{
	u32 i = 2;

	if (len < 4 || p[0] != 0xff || p[1] != 0xd8)
		return false;
	while (i + 4 <= len) {
		u8 marker;
		u32 seg;

		if (p[i] != 0xff) {
			i++;
			continue;
		}
		marker = p[i + 1];
		if (marker == 0xff || marker == 0x01 ||
		    (marker >= 0xd0 && marker <= 0xd7)) {
			i += marker == 0xff ? 1 : 2;
			continue;
		}
		seg = (p[i + 2] << 8) | p[i + 3];
		if (marker == 0xc0 || marker == 0xc1) {
			if (i + 2 + seg > len || seg < 17 || p[i + 4] != 8 ||
			    p[i + 9] != 3)
				break;
			/* Y 2x2, Cb 1x1, Cr 1x1 */
			if (p[i + 11] != 0x22 || p[i + 14] != 0x11 ||
			    p[i + 17] != 0x11)
				break;
			return true;
		}
		if ((marker >= 0xc2 && marker <= 0xcf && marker != 0xc4 &&
		     marker != 0xc8 && marker != 0xcc) || marker == 0xda)
			break;
		i += 2 + seg;
	}
	dev_dbg(dev, "mjpeg: not a baseline 4:2:0 JPEG\n");
	return false;
}

static int mjpeg_prepare_input(struct aml_vdec_ctx *ctx,
			       struct vb2_v4l2_buffer *src)
{
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	struct aml_mjpeg_ctx *m = ctx->codec_priv;
	struct vb2_buffer *vb = &src->vb2_buf;
	const u8 *in = vb2_plane_vaddr(vb, 0);
	u32 len = vb2_get_plane_payload(vb, 0);
	u32 need = len + VLD_PADDING_SIZE + 64 + 8;
	bool pad;
	u8 *p;

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
	regmap_write(map, PSCALE_RST, 0x7);
	regmap_write(map, PSCALE_RST, 0x0);
}

static void mjpeg_job_done(struct aml_vdec_ctx *ctx, enum vb2_buffer_state st)
{
	struct aml_vdec_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);

	/*
	 * A JPEG request carries no controls, so the buffer is its only
	 * object: once that is done the request is complete, and completing
	 * the control side afterwards (as device_run does for the other
	 * legacy codecs) binds to a finished request and WARNs.  Do it first.
	 */
	if (src && src->vb2_buf.req_obj.req)
		v4l2_ctrl_request_complete(src->vb2_buf.req_obj.req,
					   &ctx->ctrl_handler);
	mjpeg_canvas_free(dev->dec_hw, ctx->codec_priv);
	if (dst)
		vb2_set_plane_payload(&dst->vb2_buf, 0,
				      ctx->pix_fmt[AML_FMT_DST].plane_fmt[0].sizeimage);
	ctx->int_cond = 1;
	ctx->job_finished = true;
	v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev_dec, ctx->m2m_ctx, st);
	wake_up(&ctx->queue);
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
	if (ctx->job_finished)
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

		if (w > pf->plane_fmt[0].bytesperline || h > pf->height) {
			dev_err(hw->dev, "mjpeg %ux%u does not fit %ux%u\n",
				w, h, pf->plane_fmt[0].bytesperline, pf->height);
			mjpeg_job_done(ctx, VB2_BUF_STATE_ERROR);
			return IRQ_HANDLED;
		}
		regmap_write(hw->map[DOS_BUS], MJPEG_DECODE_PARAM,
			     (h << 4) | 0x8000);
		regmap_write(hw->map[DOS_BUS], MJPEG_DEC_STATUS, 0);
		return IRQ_HANDLED;
	}
	if (status == MJPEG_STATUS_TIMEOUT) {
		mjpeg_job_done(ctx, VB2_BUF_STATE_ERROR);
		return IRQ_HANDLED;
	}

	/*
	 * Anything else is the end of the picture - the reference does not
	 * look at what FROM_AMRISC holds either (it is often 0).
	 */
	m->decoded++;
	regmap_write(hw->map[DOS_BUS], MJPEG_FROM_AMRISC, 0);
	mjpeg_job_done(ctx, VB2_BUF_STATE_DONE);

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

	m = kzalloc_obj(*m);
	if (!m)
		return -ENOMEM;
	m->v4l2_ctx = ctx;
	for (i = 0; i < ARRAY_SIZE(m->canvas); i++)
		m->canvas[i] = -1;

	ctx->codec_priv = m;
	return 0;
}

void aml_mjpeg_exit(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_mjpeg_ctx *m = ctx->codec_priv;
	struct aml_vdec_hw *hw;

	if (!m)
		return;
	hw = vdec_get_hw(ctx->dev);

	if (ctx->dos_clk_en)
		aml_stop_vdec_hw(hw);
	mjpeg_canvas_free(hw, m);
	if (m->stage_vaddr)
		dma_free_coherent(hw->dev, m->stage_size,
				  m->stage_vaddr, m->stage_paddr);

	if (hw->curr_ctx == m)
		hw->curr_ctx = NULL;
	kfree(m);
	ctx->codec_priv = NULL;
}

int aml_mjpeg_dec_run(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_mjpeg_ctx *m = ctx->codec_priv;
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

	if (!mjpeg_supported(hw->dev, vb2_plane_vaddr(&src->vb2_buf, 0),
			     vb2_get_plane_payload(&src->vb2_buf, 0)))
		return -EINVAL;

	hw->curr_ctx = m;
	hw->hw_ops.irq_handler = mjpeg_isr;
	hw->hw_ops.irq_threaded_func = mjpeg_threaded_isr;

	if (mjpeg_prepare_input(ctx, src) < 0)
		return -ENOMEM;

	spec = mjpeg_canvas_setup(ctx, &dst->vb2_buf);
	if (spec == ~0u)
		return -ENOMEM;
	mjpeg_restore_hw(ctx, spec);

	dev_dbg(hw->dev, "mjpeg run %lu bytes spec %06x\n",
		vb2_get_plane_payload(&src->vb2_buf, 0), spec);

	ctx->int_cond = 0;
	regmap_update_bits(hw->map[DOS_BUS], VLD_MEM_VIFIFO_CONTROL, 0x6, 0x6);
	aml_start_vdec_hw(hw);

	ret = wait_event_timeout(ctx->queue, ctx->int_cond,
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
		aml_stop_vdec_hw(hw);
		mjpeg_canvas_free(hw, m);
		return -ETIMEDOUT;
	}

	aml_stop_vdec_hw(hw);
	return 0;
}
