// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * HEVC (H.265) decoding on the Amlogic HEVC core.
 *
 * Same core as VP9 and AV1, running the HEVC microcode.  The division of work
 * follows the reference driver (vh265.c):
 *
 *   - The microcode finds NAL units and asks what to do with each one.  It
 *     parses VPS, SPS, PPS and slice headers itself and keeps the parameter
 *     sets in workspace buffers, so a stream fed one access unit at a time
 *     only has to carry them once.
 *   - After a slice header it hands the host a parameter block (RPM): the
 *     picture geometry, POC lsb, the current reference picture set, the
 *     reference list modification and so on.
 *   - The host owns the picture buffers: it computes the POC, marks
 *     references from the RPS, builds the two reference lists, names the
 *     reference pictures and the collocated picture to the hardware and
 *     points the output at a buffer.  Then it says "decode the slice data".
 *
 * Pictures are written compressed through the MMU for reference and directly
 * into an NV12M CAPTURE buffer by the double-write unit. Firmware interrupts
 * only wake the job worker; buffers remain owned until IRQ and DMA stop.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include "aml_vdec.h"
#include "aml_vdec_hw.h"
#include "hevc_priv.h"

/* T7 workspace geometry, including its 8K deblock line offsets. */
#define H265_IPP_SIZE		0x8000
#define H265_SHORT_TERM_RPS_SIZE 0x0800
#define H265_VPS_SIZE		0x0800
#define H265_SPS_SIZE		0x0800
#define H265_PPS_SIZE		0x2000
#define H265_SAO_UP_SIZE	0x2800
#define H265_SWAP_BUF_SIZE	0x0800
#define H265_SWAP_BUF2_SIZE	0x0800
#define H265_SCALELUT_SIZE	0x8000
#define H265_DBLK_PARA_SIZE	0x40100
#define H265_DBLK_DATA_SIZE	0x140800
#define H265_DBLK_DATA2_SIZE	0x100800
#define H265_MMU_VBH_SIZE	0xa000
#define H265_MPRED_ABOVE_SIZE	0x8000

#define H265_RPM_SIZE		(0x80 * 2)
#define H265_LMEM_SIZE		(0x500 * 2)
#define H265_MMU_MAP_SIZE	(0x1200 * 4)
#define H265_AUX_PREFIX_SIZE	(12 * 1024)
#define H265_AUX_SUFFIX_SIZE	(12 * 1024)

#define H265_PADDING_SIZE	(16 * 1024)
#define H265_TIMEOUT_MS		500

/* 16 references plus the picture being decoded plus slack. */
/* misc_flag0 bits */
#define PCM_LOOP_FILTER_DISABLED_FLAG_BIT		0
#define PCM_ENABLE_FLAG_BIT				1
#define LOOP_FILTER_ACROSS_TILES_ENABLED_FLAG_BIT	2
#define PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT	3
#define DEBLOCKING_FILTER_OVERRIDE_ENABLED_FLAG_BIT	4
#define PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT		5
#define DEBLOCKING_FILTER_OVERRIDE_FLAG_BIT		6
#define SLICE_DEBLOCKING_FILTER_DISABLED_FLAG_BIT	7
#define SLICE_SAO_LUMA_FLAG_BIT				8
#define SLICE_SAO_CHROMA_FLAG_BIT			9
#define SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT 10

#define PARSER_CMD_SKIP_CFG_0	0x0000090b
#define PARSER_CMD_SKIP_CFG_1	0x1b14140f
#define PARSER_CMD_SKIP_CFG_2	0x001b1910
static const u16 h265_parser_cmd[] = {
	0x0401, 0x8401, 0x0800, 0x0402, 0x9002, 0x1423,
	0x8CC3, 0x1423, 0x8804, 0x9825, 0x0800, 0x04FE,
	0x8406, 0x8411, 0x1800, 0x8408, 0x8409, 0x8C2A,
	0x9C2B, 0x1C00, 0x840F, 0x8407, 0x8000, 0x8408,
	0x2000, 0xA800, 0x8410, 0x04DE, 0x840C, 0x840D,
	0xAC00, 0xA000, 0x08C0, 0x08E0, 0xA40E, 0xFC00,
	0x7C00
};

static void h265_config_comp(struct aml_hevc_ctx *h);

static void wr(struct aml_vdec_hw *hw, u32 reg, u32 val)
{
	regmap_write(hw->map[DOS_BUS], reg, val);
}

static void h265_free_pics(struct aml_hevc_ctx *h)
{
	struct device *dev = h->hw->dev;
	int i;

	for (i = 0; i < H265_SLOTS; i++) {
		struct h265_buffer *pic = &h->buffers[i];

		if (pic->body_v)
			dma_free_coherent(dev, h->body_size, pic->body_v, pic->body_p);
		if (pic->hdr_v)
			dma_free_coherent(dev, h->hdr_size, pic->hdr_v, pic->hdr_p);
		if (pic->mv_v)
			dma_free_coherent(dev, h->mv_size, pic->mv_v, pic->mv_p);
		memset(pic, 0, sizeof(*pic));
	}
	h->body_size = 0;
	h->hdr_size = 0;
	h->mv_size = 0;
	h->alloc_w = 0;
	h->alloc_h = 0;
	h->alloc_nslots = 0;
}

void h265_hw_free(struct aml_hevc_ctx *h)
{
	struct device *dev = h->hw->dev;

	h265_free_pics(h);
	if (h->stream_v)
		dma_free_coherent(dev, h->stream_size, h->stream_v, h->stream_p);
	if (h->side_v)
		dma_free_coherent(dev, h->side_size, h->side_v, h->side_p);
	if (h->ws_v)
		dma_free_coherent(dev, h->ws_size, h->ws_v, h->ws_p);
	h->stream_v = NULL;
	h->side_v = NULL;
	h->ws_v = NULL;
}

/* Hand out @size bytes at *@p and move it on to the next 4K boundary */
static dma_addr_t h265_carve(dma_addr_t *p, size_t size)
{
	dma_addr_t addr = *p;

	*p = ALIGN(addr + size, SZ_4K);

	return addr;
}

int h265_hw_alloc_workspace(struct aml_hevc_ctx *h)
{
	struct device *dev = h->hw->dev;
	dma_addr_t p;

	if (h->ws_v)
		return 0;

	h->ws_size = ALIGN(H265_IPP_SIZE + H265_SAO_ABV_BUF_SIZE + H265_SAO_VB_BUF_SIZE +
			   H265_SHORT_TERM_RPS_SIZE + H265_VPS_SIZE +
			   H265_SPS_SIZE + H265_PPS_SIZE + H265_SAO_UP_SIZE +
			   H265_SWAP_BUF_SIZE + H265_SWAP_BUF2_SIZE +
			   H265_SCALELUT_SIZE + H265_DBLK_PARA_SIZE +
			   H265_DBLK_DATA_SIZE + H265_DBLK_DATA2_SIZE +
			   H265_MMU_VBH_SIZE + H265_MPRED_ABOVE_SIZE +
			   16 * SZ_4K, SZ_64K);
	h->ws_v = dma_alloc_coherent(dev, h->ws_size, &h->ws_p, GFP_KERNEL);
	if (!h->ws_v)
		return -ENOMEM;
	memset(h->ws_v, 0, h->ws_size);

	/* The reference's init_buff_spec() order, each piece page aligned. */
	p = h->ws_p;
	h->ipp = h265_carve(&p, H265_IPP_SIZE);
	h->sao_abv = h265_carve(&p, H265_SAO_ABV_BUF_SIZE);
	h->sao_vb = h265_carve(&p, H265_SAO_VB_BUF_SIZE);
	h->short_term_rps = h265_carve(&p, H265_SHORT_TERM_RPS_SIZE);
	h->vps = h265_carve(&p, H265_VPS_SIZE);
	h->sps = h265_carve(&p, H265_SPS_SIZE);
	h->pps = h265_carve(&p, H265_PPS_SIZE);
	h->sao_up = h265_carve(&p, H265_SAO_UP_SIZE);
	h->swap_buf = h265_carve(&p, H265_SWAP_BUF_SIZE);
	h->swap_buf2 = h265_carve(&p, H265_SWAP_BUF2_SIZE);
	h->scalelut = h265_carve(&p, H265_SCALELUT_SIZE);
	h->dblk_para = h265_carve(&p, H265_DBLK_PARA_SIZE);
	h->dblk_data = h265_carve(&p, H265_DBLK_DATA_SIZE);
	h->dblk_data2 = h265_carve(&p, H265_DBLK_DATA2_SIZE);
	h->mmu_vbh = h265_carve(&p, H265_MMU_VBH_SIZE);
	h->mpred_above = h265_carve(&p, H265_MPRED_ABOVE_SIZE);

	h->side_size = ALIGN(H265_RPM_SIZE + H265_LMEM_SIZE + H265_MMU_MAP_SIZE +
			     H265_AUX_PREFIX_SIZE + H265_AUX_SUFFIX_SIZE +
			     4 * SZ_4K, SZ_64K);
	h->side_v = dma_alloc_coherent(dev, h->side_size, &h->side_p, GFP_KERNEL);
	if (!h->side_v) {
		h265_hw_free(h);
		return -ENOMEM;
	}
	memset(h->side_v, 0, h->side_size);
	p = h->side_p;
	h->rpm = h265_carve(&p, H265_RPM_SIZE);
	h->lmem = h265_carve(&p, H265_LMEM_SIZE);
	h->mmu_map = h265_carve(&p, H265_MMU_MAP_SIZE);
	h->aux = h265_carve(&p, H265_AUX_PREFIX_SIZE + H265_AUX_SUFFIX_SIZE);

	dev_dbg(dev, "hevc: workspace %zu KiB at %pad, side %zu KiB\n",
		h->ws_size / 1024, &h->ws_p, h->side_size / 1024);
	return 0;
}

/*
 * Compressed body per 64x32 block: 3200 bytes for 8 bit with memory saving
 * (as VP9), 4096 for 10 bit, which cannot use it.
 */
static u32 h265_comp_body_size(u32 w, u32 h, bool ten_bit)
{
	return (ten_bit ? 4096 : 3200) * ((w + 63) >> 6) * ((h + 31) >> 5);
}

static u32 h265_comp_header_size(u32 w, u32 h)
{
	return 32 * ((w + 127) >> 7) * ((h + 63) >> 6);
}

static size_t h265_hdr_alloc_size(u32 w, u32 h)
{
	if (w * h > 4096 * 2304)
		return 0x120000;
	if (w * h > 1920 * 1088)
		return 0x48000;
	return 0x10000;
}

size_t h265_mv_buf_size(u32 w, u32 h)
{
	/* A partial 64x64 CTU still occupies a complete 0x200-byte MV unit. */
	u32 xn = DIV_ROUND_UP(w, 64), yn = DIV_ROUND_UP(h, 64);

	return ALIGN(xn * yn * 0x200, SZ_64K);
}

int h265_hw_alloc_pictures(struct aml_hevc_ctx *h)
{
	struct device *dev = h->hw->dev;
	u32 w = h->dpb.pic_w, ht = h->dpb.pic_h;
	size_t body = ALIGN(h265_comp_body_size(w, ht, h->dpb.ten_bit), PAGE_SIZE);
	size_t mv = h265_mv_buf_size(w, ht);
	size_t hdr = h265_hdr_alloc_size(w, ht);
	int i;

	if (body / PAGE_SIZE > H265_MMU_MAP_SIZE / sizeof(u32))
		return -EOPNOTSUPP;
	if (h->alloc_w == w && h->alloc_h == ht &&
	    h->alloc_ten_bit == h->dpb.ten_bit && h->alloc_nslots == h->dpb.nslots)
		return 0;

	h265_free_pics(h);
	for (i = 0; i < h->dpb.nslots; i++) {
		struct h265_buffer *pic = &h->buffers[i];

		pic->body_v = dma_alloc_coherent(dev, body, &pic->body_p, GFP_KERNEL);
		pic->hdr_v = dma_alloc_coherent(dev, hdr, &pic->hdr_p, GFP_KERNEL);
		pic->mv_v = dma_alloc_coherent(dev, mv, &pic->mv_p, GFP_KERNEL);
		if (!pic->body_v || !pic->hdr_v || !pic->mv_v) {
			h->body_size = body;
			h->hdr_size = hdr;
			h->mv_size = mv;
			h265_free_pics(h);
			return -ENOMEM;
		}
		memset(pic->mv_v, 0, mv);
	}
	h->body_size = body;
	h->hdr_size = hdr;
	h->mv_size = mv;
	h->alloc_w = w;
	h->alloc_h = ht;
	h->alloc_ten_bit = h->dpb.ten_bit;
	h->alloc_nslots = h->dpb.nslots;

	dev_dbg(dev, "hevc: %d pictures %ux%u, body %zu, mv %zu\n",
		h->dpb.nslots, w, ht, body, mv);
	return 0;
}

static void h265_config_pic_list(struct aml_hevc_ctx *h)
{
	int i;

	/* Reset loses the table even when the picture allocations are reused. */
	wr(h->hw, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, BIT(1) | BIT(2));
	for (i = 0; i < H265_SLOTS; i++)
		wr(h->hw, HEVCD_MPP_ANC2AXI_TBL_DATA,
		   i < h->dpb.nslots ? h->buffers[i].hdr_p >> 5 : 0);
	wr(h->hw, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, BIT(0));
	wr(h->hw, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, BIT(0));
	for (i = 0; i < 32; i++)
		wr(h->hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR, 0);
	h265_config_comp(h);
}

/* hevc_config_work_space_hw() */
static void h265_config_workspace(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;

	wr(hw, HEVCD_IPP_LINEBUFF_BASE, h->ipp);
	wr(hw, HEVC_RPM_BUFFER, h->rpm);
	wr(hw, HEVC_SHORT_TERM_RPS, h->short_term_rps);
	wr(hw, H265_VPS_BUFFER, h->vps);
	wr(hw, H265_SPS_BUFFER, h->sps);
	wr(hw, HEVC_PPS_BUFFER, h->pps);
	wr(hw, H265_SAO_UP, h->sao_up);
	wr(hw, HEVC_ASSIST_MMU_MAP_ADDR, h->mmu_map);
	wr(hw, H265_SCALELUT, h->scalelut);
	wr(hw, HEVC_DBLK_CFG3, 0x8020);	/* T7 workspace line offsets */
	wr(hw, HEVC_DBLK_CFG4, h->dblk_para);
	wr(hw, HEVC_DBLK_CFG5, h->dblk_data);
	wr(hw, HEVC_DBLK_CFGE, h->dblk_data2);
	wr(hw, H265_LMEM_DUMP_ADR, h->lmem);
}

/* hevc_init_decoder_hw(), less the self tests. */
static void h265_init_decoder(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	u32 v;
	int i;

	/* reset iqit to start its memory init again */
	wr(hw, DOS_SW_RESET3, BIT(14));
	regmap_update_bits(hw->map[DOS_BUS], HEVC_CABAC_CONTROL, 1, 0);
	regmap_update_bits(hw->map[DOS_BUS], HEVC_PARSER_CORE_CONTROL, 1, 0);

	v = read_dos_reg(hw, HEVC_PARSER_INT_CONTROL);
	v &= 0x03ffffff;
	v |= (3 << 29) | (2 << 26) |
	     (1 << 24) |	/* stream_buffer_empty_int_amrisc_enable */
	     (1 << 22) |	/* stream_fifo_empty_int_amrisc_enable */
	     (1 << 7) |		/* dec_done_int_cpu_enable */
	     (1 << 4) |		/* startcode_found_int_cpu_enable */
	     (1 << 0);		/* parser_int_enable */
	wr(hw, HEVC_PARSER_INT_CONTROL, v);

	v = read_dos_reg(hw, HEVC_SHIFT_STATUS);
	v |= BIT(1) | BIT(0);	/* emulation_check_on, startcode_check_on */
	wr(hw, HEVC_SHIFT_STATUS, v);

	wr(hw, HEVC_SHIFT_CONTROL,
	   (3 << 6) |		/* sft_valid_wr_position */
	   (2 << 4) |		/* emulate_code_length_sub_1 */
	   (2 << 1) |		/* start_code_length_sub_1, 3 bytes */
	   (1 << 0));		/* stream_shift_enable */
	wr(hw, HEVC_CABAC_CONTROL, BIT(0));
	wr(hw, HEVC_PARSER_CORE_CONTROL, BIT(0));
	wr(hw, HEVC_DEC_STATUS_REG, 0);

	wr(hw, HEVC_IQIT_SCALELUT_WR_ADDR, 0);
	for (i = 0; i < 1024; i++)
		wr(hw, HEVC_IQIT_SCALELUT_DATA, 0);

	wr(hw, H265_DECODE_SIZE, 0);

	wr(hw, HEVC_PARSER_CMD_WRITE, BIT(16));
	for (i = 0; i < ARRAY_SIZE(h265_parser_cmd); i++)
		wr(hw, HEVC_PARSER_CMD_WRITE, h265_parser_cmd[i]);
	wr(hw, HEVC_PARSER_CMD_SKIP_0, PARSER_CMD_SKIP_CFG_0);
	wr(hw, HEVC_PARSER_CMD_SKIP_1, PARSER_CMD_SKIP_CFG_1);
	wr(hw, HEVC_PARSER_CMD_SKIP_2, PARSER_CMD_SKIP_CFG_2);

	wr(hw, HEVC_PARSER_IF_CONTROL,
	   (1 << 5) |		/* parser_sao_if_en */
	   (1 << 2) |		/* parser_mpred_if_en */
	   (1 << 0));		/* parser_scaler_if_en */

	wr(hw, HEVCD_IPP_TOP_CNTL, BIT(0));	/* reset ipp and mpp */
	wr(hw, HEVCD_IPP_TOP_CNTL, BIT(1));	/* enable ipp */
}

static void h265_config_decode_mode(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	u32 mode = H265_DECODE_MODE_MULTI_FRAMEBASE;

	/* The supported 4K range uses host setup for each slice. */
	mode |= (h->start_decoding_flag & 0xff) << 16;
	mode |= 0x80 << 24;	/* MBX0 interrupt */
	wr(hw, H265_DECODE_MODE, mode);
	wr(hw, H265_DECODE_MODE2, h->rps_set_id);
}

/* vh265_prot_init(), run before every access unit as the reference does. */
static void h265_prot_init(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;

	h265_config_workspace(h);
	h265_init_decoder(h);

	wr(hw, HEVC_ASSIST_MBOX0_CLR_REG, 1);
	wr(hw, HEVC_ASSIST_MBOX0_MASK, 1);
	wr(hw, HEVC_PSCALE_CTRL, 0);
	wr(hw, H265_DEBUG_REG1, 0);
	wr(hw, H265_NAL_SEARCH_CTL, 0x8);
	wr(hw, H265_DECODE_STOP_POS, 0);
	h265_config_decode_mode(h);

	wr(hw, H265_AUX_ADR, h->aux);
	wr(hw, H265_AUX_DATA_SIZE, ((H265_AUX_PREFIX_SIZE >> 4) << 16) |
				   (H265_AUX_SUFFIX_SIZE >> 4));
}

static int h265_feed(struct aml_hevc_ctx *h, const void *data, size_t size)
{
	struct aml_vdec_hw *hw = h->hw;
	/* Keep the padded write pointer distinct from the empty-ring pointer. */
	size_t want = ALIGN(size + H265_PADDING_SIZE + 8, SZ_64K);

	if (!data || !size)
		return -EINVAL;
	if (h->stream_v && h->stream_size < want) {
		dma_free_coherent(hw->dev, h->stream_size, h->stream_v, h->stream_p);
		h->stream_v = NULL;
	}
	if (!h->stream_v) {
		h->stream_v = dma_alloc_coherent(hw->dev, want, &h->stream_p,
						 GFP_KERNEL);
		if (!h->stream_v)
			return -ENOMEM;
		h->stream_size = want;
	}

	memcpy(h->stream_v, data, size);
	memset(h->stream_v + size, 0, h->stream_size - size);
	h->payload = size;
	dev_dbg(hw->dev, "hevc: input bytes=%zu prefix=%*ph start=%pad skip=0\n",
		size, (int)min_t(size_t, size, 16), h->stream_v, &h->stream_p);

	wr(hw, HEVC_STREAM_START_ADDR, h->stream_p);
	wr(hw, HEVC_STREAM_END_ADDR, h->stream_p + h->stream_size);
	wr(hw, HEVC_STREAM_RD_PTR, h->stream_p);
	wr(hw, HEVC_STREAM_WR_PTR, h->stream_p + round_down(size + H265_PADDING_SIZE, 8));
	wr(hw, HEVC_STREAM_CONTROL,
	   read_dos_reg(hw, HEVC_STREAM_CONTROL) | (7 << 4));
	return 0;
}

static void h265_enable_input(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;

	wr(hw, HEVC_STREAM_CONTROL,
	   read_dos_reg(hw, HEVC_STREAM_CONTROL) | (7 << 4));
	wr(hw, HEVC_STREAM_FIFO_CTL,
	   read_dos_reg(hw, HEVC_STREAM_FIFO_CTL) | BIT(29));
	wr(hw, HEVC_STREAM_CONTROL, read_dos_reg(hw, HEVC_STREAM_CONTROL) | BIT(0));
}

static void h265_fill_mmu_map(struct aml_hevc_ctx *h)
{
	struct h265_buffer *pic = &h->buffers[h->dpb.cur];
	u32 *map = h->side_v + (h->mmu_map - h->side_p);
	u32 pages = h->body_size / PAGE_SIZE, first = pic->body_p >> PAGE_SHIFT, i;

	for (i = 0; i < pages && i < H265_MMU_MAP_SIZE / 4; i++)
		map[i] = first + i;
}

/* init_decode_head_hw() */
static void h265_config_comp(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	u32 body = h265_comp_body_size(h->dpb.pic_w, h->dpb.pic_h, h->dpb.ten_bit);
	u32 v;

	wr(hw, HEVCD_MPP_DECOMP_CTL1, BIT(4));	/* paged memory */
	wr(hw, HEVCD_MPP_DECOMP_CTL2, 0);
	wr(hw, HEVC_CM_BODY_LENGTH, body);
	wr(hw, HEVC_CM_HEADER_OFFSET, body);
	wr(hw, HEVC_CM_HEADER_LENGTH, h265_comp_header_size(h->dpb.pic_w, h->dpb.pic_h));
	wr(hw, HEVC_SAO_MMU_VH0_ADDR, h->mmu_vbh);
	wr(hw, HEVC_SAO_MMU_VH1_ADDR, h->mmu_vbh + H265_MMU_VBH_SIZE / 2);
	v = read_dos_reg(hw, HEVC_SAO_CTRL9) | BIT(0);
	wr(hw, HEVC_SAO_CTRL9, v);
	v = read_dos_reg(hw, HEVC_SAO_CTRL5) | BIT(10);	/* use CM_HEADER_START_ADDR */
	wr(hw, HEVC_SAO_CTRL5, v);
}

static void h265_config_mc_buffer(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	int i;

	if (h->dpb.slice_type == H265_I_SLICE)
		return;
	wr(hw, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, BIT(0));
	for (i = 0; i < h->dpb.ref_num_l0; i++) {
		int s = h265_pic_by_poc(&h->dpb, h->dpb.ref_poc_l0[i], true);

		if (s < 0)
			s = h->dpb.cur;
		wr(hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR, (s << 16) | (s << 8) | s);
	}
	if (h->dpb.slice_type != H265_B_SLICE)
		return;
	wr(hw, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, (16 << 8) | BIT(0));
	for (i = 0; i < h->dpb.ref_num_l1; i++) {
		int s = h265_pic_by_poc(&h->dpb, h->dpb.ref_poc_l1[i], true);

		if (s < 0)
			s = h->dpb.cur;
		wr(hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR, (s << 16) | (s << 8) | s);
	}
}

/* config_mcrcc_axi_hw() */
static void h265_config_mcrcc(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	u32 a, b;

	wr(hw, HEVCD_MCRCC_CTL1, 0x2);		/* reset mcrcc */
	if (h->dpb.slice_type == H265_I_SLICE) {
		wr(hw, HEVCD_MCRCC_CTL1, 0x0);
		return;
	}
	if (h->dpb.slice_type == H265_B_SLICE) {
		wr(hw, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, 0);
		a = read_dos_reg(hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
		a |= a << 16;
		wr(hw, HEVCD_MCRCC_CTL2, a);
		wr(hw, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, (16 << 8) | BIT(1));
		b = read_dos_reg(hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
		b |= b << 16;
		if (a == b) {
			b = read_dos_reg(hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
			b |= b << 16;
		}
		wr(hw, HEVCD_MCRCC_CTL3, b);
	} else {
		wr(hw, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, BIT(1));
		a = read_dos_reg(hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
		a |= a << 16;
		wr(hw, HEVCD_MCRCC_CTL2, a);
		a = read_dos_reg(hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
		a |= a << 16;
		wr(hw, HEVCD_MCRCC_CTL3, a);
	}
	wr(hw, HEVCD_MCRCC_CTL1, 0xff0);	/* progressive mode */
}

/* config_mpred_hw() */
static void h265_config_mpred(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	struct h265_buffer *cur = &h->buffers[h->dpb.cur], *col = &h->buffers[h->dpb.col];
	int unit = h->dpb.lcu_size_log2 == 6 ? 0x200 : h->dpb.lcu_size_log2 == 5 ? 0x80 : 0x20;
	dma_addr_t rd_start = col->mv_p;
	dma_addr_t rd_ptr = rd_start + h->dpb.slice_addr * unit;
	dma_addr_t wr_ptr = cur->mv_p + h->dpb.slice_addr * unit;
	bool above_en = true, mv_wr_en = true;
	bool mv_rd_en = h->dpb.slice_type != H265_I_SLICE;
	u32 v, lt = 0, en0 = 0, en1 = 0;
	int i;

	wr(hw, HEVC_MPRED_MV_WR_START_ADDR, cur->mv_p);
	wr(hw, HEVC_MPRED_MV_RD_START_ADDR, rd_start);
	v = (h->dpb.lcu_x_num - h->dpb.tile_width_lcu) * unit;
	wr(hw, HEVC_MPRED_MV_WR_ROW_JUMP, v);
	wr(hw, HEVC_MPRED_MV_RD_ROW_JUMP, v);

	v = (h->dpb.slice_type & 3) |
	    (h->dpb.new_pic << 2) |
	    (h->dpb.new_tile << 3) |
	    (h->dpb.is_next_slice_segment << 4) |
	    (h->dpb.tmvp << 5) |
	    (h->dpb.ldc << 6) |
	    (h->dpb.col_from_l0 << 7) |
	    (0 << 8) |			/* above_ptr_ctrl */
	    (above_en << 9) |
	    (mv_wr_en << 10) |
	    (mv_rd_en << 11) |
	    (0 << 12) |			/* col_isIntra */
	    (1 << 13) |			/* buffer_linear */
	    (0 << 14) | (0 << 15) |	/* LongTerm_Curr, LongTerm_Col: no effect seen */
	    ((h->dpb.lcu_size_log2 & 0xf) << 16) |
	    (3 << 20) |			/* cu_size_log2 */
	    ((h->dpb.plevel & 7) << 24);
	wr(hw, HEVC_MPRED_CTRL0, v);

	wr(hw, HEVC_MPRED_CTRL1, h->dpb.max_merge_cand | (2 << 4) | (3 << 8) |
				 (5 << 12) | (36 << 16));
	wr(hw, HEVC_MPRED_PIC_SIZE, h->dpb.pic_w | (h->dpb.pic_h << 16));
	wr(hw, HEVC_MPRED_PIC_SIZE_LCU, (h->dpb.lcu_x_num - 1) | ((h->dpb.lcu_y_num - 1) << 16));
	wr(hw, HEVC_MPRED_TILE_START, h->dpb.tile_start_lcu_x | (h->dpb.tile_start_lcu_y << 16));
	wr(hw, HEVC_MPRED_TILE_SIZE_LCU, h->dpb.tile_width_lcu | (h->dpb.tile_height_lcu << 16));
	wr(hw, HEVC_MPRED_REF_NUM, h->dpb.ref_num_l0 | (h->dpb.ref_num_l1 << 8));

	for (i = 0; i < h->dpb.ref_num_l0; i++) {
		int s = h265_pic_by_poc(&h->dpb, h->dpb.ref_poc_l0[i], true);

		if (s >= 0 && h->dpb.pic[s].long_term)
			lt |= BIT(i);
		en0 |= BIT(i);
	}
	for (i = 0; i < h->dpb.ref_num_l1; i++) {
		int s = h265_pic_by_poc(&h->dpb, h->dpb.ref_poc_l1[i], true);

		if (s >= 0 && h->dpb.pic[s].long_term)
			lt |= BIT(i + 16);
		en1 |= BIT(i);
	}
	wr(hw, HEVC_MPRED_LT_REF, lt);
	wr(hw, HEVC_MPRED_REF_EN_L0, en0);
	wr(hw, HEVC_MPRED_REF_EN_L1, en1);
	wr(hw, HEVC_MPRED_CUR_POC, h->dpb.curr_poc);
	wr(hw, HEVC_MPRED_COL_POC, h->dpb.col_poc);

	/* L0/L1 pairs, in this order. */
	for (i = 0; i < 16; i++) {
		wr(hw, HEVC_MPRED_L0_REF00_POC + REG_ALIGN(i), h->dpb.ref_poc_l0[i]);
		wr(hw, HEVC_MPRED_L1_REF00_POC + REG_ALIGN(i), h->dpb.ref_poc_l1[i]);
	}

	if (h->dpb.new_pic) {
		wr(hw, HEVC_MPRED_ABV_START_ADDR, h->mpred_above);
		wr(hw, HEVC_MPRED_MV_WPTR, wr_ptr);
		wr(hw, HEVC_MPRED_MV_RPTR, rd_start);
	} else if (!h->dpb.is_next_slice_segment) {
		wr(hw, HEVC_MPRED_MV_RPTR, rd_ptr);
	}
	wr(hw, HEVC_MPRED_MV_RD_END_ADDR, rd_start + h->mv_size);
}

/* config_sao_hw() for compressed reference plus 1:1 NV12 double write */
static void h265_config_sao(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	struct h265_buffer *cur = &h->buffers[h->dpb.cur];
	u32 misc = h->dpb.misc_flag0;
	u32 uv_size = h->dpb.lcu_total * h->dpb.lcu_size * h->dpb.lcu_size / 2;
	u32 uv_h = (uv_size + 0xffff) >> 16;
	dma_addr_t dw_y = h->dw_target_y, dw_c = h->dw_target_c;
	u32 v, v2;
	bool slice_dbk_disabled;

	v = read_dos_reg(hw, HEVC_SAO_CTRL0);
	v &= ~0xf;
	v |= h->dpb.lcu_size_log2;
	wr(hw, HEVC_SAO_CTRL0, v);
	wr(hw, HEVC_SAO_PIC_SIZE, h->dpb.pic_w | (h->dpb.pic_h << 16));
	wr(hw, HEVC_SAO_PIC_SIZE_LCU, (h->dpb.lcu_x_num - 1) | ((h->dpb.lcu_y_num - 1) << 16));
	if (h->dpb.new_pic)
		wr(hw, HEVC_SAO_Y_START_ADDR, U32_MAX);
	/* T7 has a separate double-write downscale control. */
	wr(hw, HEVC_SAO_CTRL26, 0);

	v = read_dos_reg(hw, HEVC_SAO_CTRL5);
	v &= ~(0xff << 16);		/* double write 1:1 */
	if (h->dpb.ten_bit)
		v &= ~BIT(9);
	else
		v |= BIT(9);		/* 8 bit, memory saving */
	wr(hw, HEVC_SAO_CTRL5, v);

	wr(hw, HEVC_SAO_Y_START_ADDR, dw_y);
	wr(hw, HEVC_CM_BODY_START_ADDR, cur->body_p);
	wr(hw, HEVC_CM_HEADER_START_ADDR, cur->hdr_p);
	wr(hw, HEVC_SAO_Y_LENGTH, (uv_h << 16) << 1);
	wr(hw, HEVC_SAO_C_START_ADDR, dw_c);
	wr(hw, HEVC_SAO_C_LENGTH, uv_h << 16);
	wr(hw, HEVC_SAO_Y_WPTR, dw_y);
	wr(hw, HEVC_SAO_C_WPTR, dw_c);

	if (h->dpb.new_pic) {
		v = (0xff << 8);
		if (h->dpb.pic_w >= 1280)
			v |= BIT(4);
		wr(hw, HEVC_DBLK_CFGB, v);
		wr(hw, HEVC_DBLK_CFG2, h->dpb.pic_w | (h->dpb.pic_h << 16));
		v = 0;
		if ((misc >> PCM_ENABLE_FLAG_BIT) & 1)
			v = ((misc >> PCM_LOOP_FILTER_DISABLED_FLAG_BIT) & 1) << 3;
		v |= ((h->dpb.p[P_PPS_CB_QP_OFFSET] & 0x1f) << 4) |
		     ((h->dpb.p[P_PPS_CR_QP_OFFSET] & 0x1f) << 9);
		v |= h->dpb.lcu_size == 64 ? 0 : h->dpb.lcu_size == 32 ? 1 : 2;
		v |= h->dpb.pic_w <= 64 ? BIT(20) : 0;
		wr(hw, HEVC_DBLK_CFG1, v);
		wr(hw, REG_ALIGN(0x3510), BIT(28));	/* sts1 chooses dblk_main */
	}

	v = read_dos_reg(hw, HEVC_SAO_CTRL1);
	v &= ~0x3000;			/* linear */
	v &= ~0xff0;
	v |= 0xff0;			/* little endian */
	v &= ~0x3;			/* both outputs on */
	v |= BIT(8);			/* NV12 */
	v &= ~(3 << 14);
	v |= 2 << 14;			/* line align 64 */
	/* The V4L path reapplies these DBLK controls for every slice. */
	v2 = 0x57 << 8;
	if (h->dpb.pic_w >= 1280)
		v2 |= BIT(4);
	wr(hw, HEVC_DBLK_CFGB, v2);
	wr(hw, HEVC_SAO_CTRL1, v);

	v = read_dos_reg(hw, HEVCD_IPP_AXIIF_CONFIG);
	v &= ~0x30;			/* linear */
	v &= ~0xf;
	v |= 0xf;			/* little endian */
	v &= ~BIT(12);			/* NV12 */
	v &= ~(3 << 8);
	v |= 2 << 8;			/* line align 64 */
	wr(hw, HEVCD_IPP_AXIIF_CONFIG, v);

	v = 0;
	v2 = read_dos_reg(hw, HEVC_SAO_CTRL0) & ~0x300;
	if (h->dpb.tile_enabled) {
		v |= ((misc >> LOOP_FILTER_ACROSS_TILES_ENABLED_FLAG_BIT) & 1) << 0;
		v2 |= ((misc >> LOOP_FILTER_ACROSS_TILES_ENABLED_FLAG_BIT) & 1) << 8;
	}
	slice_dbk_disabled = (misc >> SLICE_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 1;
	if ((misc & BIT(DEBLOCKING_FILTER_OVERRIDE_ENABLED_FLAG_BIT)) &&
	    (misc & BIT(DEBLOCKING_FILTER_OVERRIDE_FLAG_BIT))) {
		v |= slice_dbk_disabled << 2;
		if (!slice_dbk_disabled) {
			v |= (h->dpb.p[P_SLICE_BETA_OFFSET_DIV2] & 0xf) << 3;
			v |= (h->dpb.p[P_SLICE_TC_OFFSET_DIV2] & 0xf) << 7;
		}
	} else {
		v |= ((misc >> PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 1) << 2;
		if (!((misc >> PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 1)) {
			v |= (h->dpb.p[P_PPS_BETA_OFFSET_DIV2] & 0xf) << 3;
			v |= (h->dpb.p[P_PPS_TC_OFFSET_DIV2] & 0xf) << 7;
		}
	}
	if ((misc & BIT(PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT)) &&
	    ((misc & BIT(SLICE_SAO_LUMA_FLAG_BIT)) ||
	     (misc & BIT(SLICE_SAO_CHROMA_FLAG_BIT)) || !slice_dbk_disabled)) {
		v |= ((misc >> SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT) & 1) << 1;
		v2 |= ((misc >> SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT) & 1) << 9;
	} else {
		v |= ((misc >> PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT) & 1) << 1;
		v2 |= ((misc >> PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT) & 1) << 9;
	}
	wr(hw, HEVC_DBLK_CFG9, v);
	wr(hw, HEVC_SAO_CTRL0, v2);
}

static void h265_read_rpm(struct aml_hevc_ctx *h)
{
	const u16 *raw = h->side_v + (h->rpm - h->side_p);
	int i, j;

	dma_rmb();
	for (i = 0; i < P_RPM_COUNT; i += 4)
		for (j = 0; j < 4; j++)
			h->dpb.p[i + j] = raw[i + 3 - j];
	for (i = 0; i < P_RPM_COUNT; i += 32)
		dev_dbg(h->hw->dev, "hevc: rpm[%02x]=%*ph\n", i, 64, &h->dpb.p[i]);
}

/* Returns 0 to decode, 1 to skip, 2 to configure after stop, or a negative error. */
static int h265_begin_picture(struct aml_hevc_ctx *h)
{
	struct h265_picture_info info;
	int ret;

	if (h->picture_started)
		return h->skip_picture ? 1 : 0;
	if (!h->au.has_picture)
		return -EINVAL;
	if (h->dpb.new_cvs) {
		h->dpb.poc_random_access = H265_MAX_INT;
		h->dpb.prev_tid0_poc = 0;
		h->dpb.new_cvs = false;
	}
	info = (struct h265_picture_info) {
		.irap = h->dpb.nal_type >= 16 && h->dpb.nal_type <= 23,
		.is_cra = h->dpb.nal_type == H265_NAL_CRA,
		.first_picture = !h->dpb.decode_idx,
		.no_output_of_prior_pics = h->au.picture.no_output_of_prior_pics,
	};
	info.no_rasl_output = info.irap && (!info.is_cra ||
		h->dpb.poc_random_access == H265_MAX_INT);
	h265_update_poc(&h->dpb);
	h->skip_picture = h265_update_random_access(&h->dpb);
	if (!h->skip_picture) {
		ret = h265_resolve_rps(&h->dpb, &h->dpb.rps);
		if (ret)
			return ret;
		h265_apply_rps(&h->dpb, &h->dpb.rps);
		h265_dpb_begin_picture(&h->dpb, &info, &h->au.picture.limits);
	}
	h->picture_started = true;
	return h->skip_picture ? 1 : 0;
}

static int h265_slice_header(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	u16 *p = h->dpb.p;
	int i, ret, col_ref;

	ret = h265_validate_params(p);
	if (ret) {
		dev_err(hw->dev,
			"hevc: invalid RPM (%d): %ux%u ctu=%u+%u poc=%u/%u type=%u nal=%u tid=%u depth=%#x chroma=%u refs=%u/%u dpb=%u reorder=%u merge=%u tiles=%u/%u\n",
			ret, p[P_PIC_WIDTH], p[P_PIC_HEIGHT], p[P_LOG2_MIN_CB_SIZE_MINUS3],
			p[P_LOG2_DIFF_MAX_MIN_CB_SIZE], p[P_POC_LSB],
			p[P_LOG2_MAX_POC_LSB_MINUS4], p[P_SLICE_TYPE], p[P_NAL_UNIT_TYPE],
			p[P_TEMPORAL_ID], p[P_BIT_DEPTH], p[P_CHROMA_FORMAT_IDC],
			p[P_NUM_REF_IDX_L0_ACTIVE], p[P_NUM_REF_IDX_L1_ACTIVE],
			p[P_SPS_MAX_DEC_PIC_BUFFERING_MINUS1_0], p[P_SPS_NUM_REORDER_PICS_0],
			p[P_FIVE_MINUS_MAX_NUM_MERGE_CAND], p[P_NUM_TILE_COLUMNS_MINUS1],
			p[P_NUM_TILE_ROWS_MINUS1]);
		dev_err(hw->dev, "hevc: RPM slice first=%u addr=%u tiles=%#x crop=%u:%u/%u/%u/%u\n",
			p[P_FIRST_SLICE_SEGMENT_IN_PIC_FLAG], p[P_SLICE_SEGMENT_ADDRESS],
			p[P_TILES_ENABLED_FLAG], p[P_CONFORMANCE_WINDOW_FLAG],
			p[P_CONF_WIN_LEFT_OFFSET], p[P_CONF_WIN_RIGHT_OFFSET],
			p[P_CONF_WIN_TOP_OFFSET], p[P_CONF_WIN_BOTTOM_OFFSET]);
		return ret;
	}
	h->dpb.temporal_id = p[P_TEMPORAL_ID];
	h->dpb.nal_type = p[P_NAL_UNIT_TYPE];
	h->dpb.misc_flag0 = p[P_MISC_FLAG0];

	if (!p[P_FIRST_SLICE_SEGMENT_IN_PIC_FLAG]) {
		h->dpb.slice_segment_addr = p[P_SLICE_SEGMENT_ADDRESS];
		if (!p[P_DEPENDENT_SLICE_SEGMENT_FLAG])
			h->dpb.slice_addr = h->dpb.slice_segment_addr;
	} else {
		h->dpb.slice_segment_addr = 0;
		h->dpb.slice_addr = 0;
	}
	h->dpb.iprev_poc = h->dpb.curr_poc;
	h->dpb.slice_type = p[P_SLICE_TYPE];
	h->dpb.tmvp = p[P_SLICE_TEMPORAL_MVP_ENABLE_FLAG] & 1;
	h->dpb.is_next_slice_segment = !!p[P_DEPENDENT_SLICE_SEGMENT_FLAG];

	if (p[P_PIC_WIDTH] < H265_MIN_DIM || p[P_PIC_HEIGHT] < H265_MIN_DIM ||
	    p[P_PIC_WIDTH] > H265_MAX_DIM || p[P_PIC_HEIGHT] > H265_MAX_DIM) {
		dev_err(hw->dev, "hevc: implausible picture size %ux%u\n",
			p[P_PIC_WIDTH], p[P_PIC_HEIGHT]);
		return -EINVAL;
	}
	/* Main10 needs an output path that preserves all ten bits. */
	h->dpb.ten_bit = false;
	h->dpb.pic_w = p[P_PIC_WIDTH];
	h->dpb.pic_h = p[P_PIC_HEIGHT];

	h->dpb.lcu_size = 1 << (p[P_LOG2_MIN_CB_SIZE_MINUS3] + 3 +
			    p[P_LOG2_DIFF_MAX_MIN_CB_SIZE]);
	if (h->dpb.lcu_size < 16 || h->dpb.lcu_size > 64)
		return -EINVAL;
	h->dpb.lcu_size_log2 = ilog2(h->dpb.lcu_size);
	h->dpb.lcu_x_num = DIV_ROUND_UP(h->dpb.pic_w, h->dpb.lcu_size);
	h->dpb.lcu_y_num = DIV_ROUND_UP(h->dpb.pic_h, h->dpb.lcu_size);
	h->dpb.lcu_total = h->dpb.lcu_x_num * h->dpb.lcu_y_num;
	h->dpb.nslots = clamp_t(int, h->au.picture.limits.max_dec_pic_buffering + 2,
				4, H265_SLOTS);
	if (!h->dpb.slice_segment_addr) {
		ret = h265_begin_picture(h);
		if (ret)
			return ret;
		dev_dbg(hw->dev, "hevc: SPS/RPM output limits buffering=%u/%u reorder=%u/%u\n",
			h->au.picture.limits.max_dec_pic_buffering,
			p[P_SPS_MAX_DEC_PIC_BUFFERING_MINUS1_0] + 1,
			h->au.picture.limits.max_num_reorder_pics, p[P_SPS_NUM_REORDER_PICS_0]);
	}
	/* Leave the firmware paused before it can DMA into picture storage. */
	if (!h->configured || h->alloc_w != h->dpb.pic_w || h->alloc_h != h->dpb.pic_h ||
	    h->alloc_nslots != h->dpb.nslots)
		return 2;

	wr(hw, HEVC_WAIT_FLAG, read_dos_reg(hw, HEVC_WAIT_FLAG) | 0x2);

	if (h->dpb.slice_segment_addr == 0) {
		int sao_mem_unit = (h->dpb.lcu_size == 16 ? 9 :
				    h->dpb.lcu_size == 32 ? 14 : 24) << 4;
		int w_cu = h->dpb.lcu_x_num, h_cu = h->dpb.lcu_y_num;
		int slot;

		h->dpb.new_pic = true;

		slot = h265_free_slot(&h->dpb);
		if (slot < 0) {
			dev_err(hw->dev, "hevc: no free picture buffer\n");
			return -ENOBUFS;
		}
		h->dpb.cur = slot;
		h->dpb.pic[slot].valid = true;
		h->dpb.pic[slot].referenced = true;
		h->dpb.pic[slot].long_term = false;
		h->dpb.pic[slot].poc = h->dpb.curr_poc;
		h->dpb.pic[slot].width = h->dpb.pic_w;
		h->dpb.pic[slot].height = h->dpb.pic_h;
		h->dpb.pic[slot].decode_idx = h->dpb.decode_idx++;
		h->dpb.pic[slot].slice_type = h->dpb.slice_type;

		h265_fill_mmu_map(h);

		h265_update_tiles(&h->dpb, w_cu, h_cu, sao_mem_unit);
		wr(hw, H265_SAO_MEM_UNIT, sao_mem_unit);
		wr(hw, H265_SAO_ABV, h->sao_abv);
		wr(hw, H265_SAO_VB_SIZE, (sao_mem_unit + (2 << 4)) * h_cu);
		wr(hw, H265_SAO_VB, h->sao_vb);
	} else {
		if (h->dpb.cur < 0)
			return 1;	/* a picture's first slice went missing */
		if (!h265_rps_matches(&h->dpb.rps, &p[P_CUR_RPS]))
			return -EINVAL;
		h->dpb.new_pic = false;
	}

	if (h->dpb.new_pic || h->dpb.iprev_poc != h->dpb.curr_poc) {
		h->dpb.new_tile = true;
		h->dpb.tile_x = 0;
		h->dpb.tile_y = 0;
	} else if (h->dpb.tile_enabled) {
		int t = h265_tile_index(&h->dpb, h->dpb.slice_segment_addr, h->dpb.lcu_x_num);

		if (t != -1 && t != (h->dpb.tile_x | (h->dpb.tile_y << 8))) {
			h->dpb.new_tile = true;
			h->dpb.tile_x = t & 0xff;
			h->dpb.tile_y = (t >> 8) & 0xff;
		} else {
			h->dpb.new_tile = false;
		}
	} else {
		h->dpb.new_tile = false;
	}
	if (h->dpb.tile_x >= H265_MAX_TILE_COLS || h->dpb.tile_y >= H265_MAX_TILE_ROWS)
		h->dpb.new_tile = false;
	if (h->dpb.new_tile) {
		struct h265_tile *t = &h->dpb.tile[h->dpb.tile_y][h->dpb.tile_x];

		h->dpb.tile_start_lcu_x = t->start_cu_x;
		h->dpb.tile_start_lcu_y = t->start_cu_y;
		h->dpb.tile_width_lcu = t->width;
		h->dpb.tile_height_lcu = t->height;
	}

	ret = h265_set_ref_lists(&h->dpb, &h->dpb.rps);
	if (ret) {
		dev_err(hw->dev, "hevc: invalid reference lists: poc=%d type=%d refs=%u/%u mod=%#x RPS=%*ph\n",
			h->dpb.curr_poc, h->dpb.slice_type, p[P_NUM_REF_IDX_L0_ACTIVE],
			p[P_NUM_REF_IDX_L1_ACTIVE], p[P_MODIFICATION_FLAG], 32, &p[P_CUR_RPS]);
		for (i = 0; i < h->dpb.nslots; i++) {
			struct h265_pic *pic = &h->dpb.pic[i];

			if (pic->valid)
				dev_dbg(hw->dev, "hevc: DPB slot=%d poc=%d ref=%d lt=%d size=%ux%u\n",
					i, pic->poc, pic->referenced, pic->long_term,
					pic->width, pic->height);
		}
		return ret;
	}
	col_ref = p[P_COLLOCATED_REF_IDX];
	h->dpb.ldc = false;
	if (h->dpb.slice_type != H265_I_SLICE) {
		h->dpb.ldc = true;
		for (i = 0; i < h->dpb.ref_num_l0 && h->dpb.ldc; i++)
			if (h->dpb.ref_poc_l0[i] > h->dpb.curr_poc)
				h->dpb.ldc = false;
		if (h->dpb.slice_type == H265_B_SLICE)
			for (i = 0; i < h->dpb.ref_num_l1 && h->dpb.ldc; i++)
				if (h->dpb.ref_poc_l1[i] > h->dpb.curr_poc)
					h->dpb.ldc = false;
	}
	h->dpb.col_from_l0 = p[P_COLLOCATED_FROM_L0_FLAG] & 1;
	h->dpb.plevel = p[P_LOG2_PARALLEL_MERGE_LEVEL];
	h->dpb.max_merge_cand = 5 - p[P_FIVE_MINUS_MAX_NUM_MERGE_CAND];

	if (h->dpb.slice_type == H265_B_SLICE && !h->dpb.col_from_l0)
		h->dpb.col_poc = col_ref < h->dpb.ref_num_l1 ?
			h->dpb.ref_poc_l1[col_ref] : H265_INVALID_POC;
	else
		h->dpb.col_poc = col_ref < h->dpb.ref_num_l0 ?
			h->dpb.ref_poc_l0[col_ref] : H265_INVALID_POC;

	h->dpb.col = h->dpb.cur;
	if (h->dpb.slice_type != H265_I_SLICE && h->dpb.col_poc != (int)H265_INVALID_POC) {
		int c = h265_pic_by_poc(&h->dpb, h->dpb.col_poc, true);

		if (c >= 0)
			h->dpb.col = c;
		else
			dev_dbg(hw->dev, "hevc: collocated poc %d missing\n", h->dpb.col_poc);
	}

	if (!h->dpb.tile_width_lcu || !h->dpb.tile_height_lcu)
		return -EINVAL;

	h265_config_mc_buffer(h);
	h265_config_mcrcc(h);
	h265_config_mpred(h);
	h265_config_sao(h);

	dev_dbg(hw->dev,
		"hevc: slice poc %d type %d nal %d addr %u new %d slot %d refs %d/%d col %d\n",
		h->dpb.curr_poc, h->dpb.slice_type, h->dpb.nal_type, p[P_SLICE_SEGMENT_ADDRESS],
		h->dpb.new_pic, h->dpb.cur, h->dpb.ref_num_l0, h->dpb.ref_num_l1, h->dpb.col_poc);
	return 0;
}

static void h265_finish(struct aml_hevc_ctx *h, int result)
{
	struct aml_vdec_ctx *ctx = h->v4l2_ctx;

	if (h->finished)
		return;
	h->finished = true;

	ctx->job->decode_result = result;
	ctx->job->int_cond = 1;
	wake_up(&ctx->job->queue);
}

static void h265_dump_start(struct aml_vdec_hw *hw, const char *phase)
{
	dev_dbg(hw->dev,
		"hevc: %s reset=%08x mpsr=%08x cpsr=%08x pc=%08x status=%08x irq=%08x mask=%08x\n",
		phase, read_dos_reg(hw, DOS_SW_RESET3), read_dos_reg(hw, HEVC_MPSR),
		read_dos_reg(hw, HEVC_CPSR), read_dos_reg(hw, HEVC_MPC_E),
		read_dos_reg(hw, HEVC_DEC_STATUS_REG), hw->hevc.irq_status,
		read_dos_reg(hw, HEVC_ASSIST_MBOX0_MASK));
	dev_dbg(hw->dev,
		"hevc: %s stream=%08x..%08x rd=%08x wr=%08x control=%08x fifo=%08x\n",
		phase, read_dos_reg(hw, HEVC_STREAM_START_ADDR),
		read_dos_reg(hw, HEVC_STREAM_END_ADDR), read_dos_reg(hw, HEVC_STREAM_RD_PTR),
		read_dos_reg(hw, HEVC_STREAM_WR_PTR), read_dos_reg(hw, HEVC_STREAM_CONTROL),
		read_dos_reg(hw, HEVC_STREAM_FIFO_CTL));
	dev_dbg(hw->dev,
		"hevc: %s wait=%08x size=%08x shift=%08x shift_status=%08x parser_irq=%08x mode_or_nal=%08x search=%08x\n",
		phase, read_dos_reg(hw, HEVC_WAIT_FLAG), read_dos_reg(hw, H265_DECODE_SIZE),
		read_dos_reg(hw, HEVC_SHIFT_BYTE_COUNT), read_dos_reg(hw, HEVC_SHIFT_STATUS),
		read_dos_reg(hw, HEVC_PARSER_INT_CONTROL), read_dos_reg(hw, H265_DECODE_MODE),
		read_dos_reg(hw, H265_NAL_SEARCH_CTL));
	dev_dbg(hw->dev,
		"hevc: %s mailbox_irq=%08x fiq_sel=%08x mcpu_group=%08x mcpu_mask=%08x mcpu_req=%08x\n",
		phase, read_dos_reg(hw, HEVC_ASSIST_MBOX0_IRQ_REG),
		read_dos_reg(hw, HEVC_ASSIST_MBOX0_FIQ_SEL),
		read_dos_reg(hw, HEVC_MCPU_INTR_GRP), read_dos_reg(hw, HEVC_MCPU_INTR_MSK),
		read_dos_reg(hw, HEVC_MCPU_INTR_REQ));
	dev_dbg(hw->dev,
		"hevc: %s parser_status=%08x parser_core=%08x debug_g=%08x debug_h=%08x rpm_or_info=%08x lmem_cmd=%08x lmem_dma=%08x level=%08x swap=%08x imem_dma=%08x\n",
		phase, read_dos_reg(hw, HEVC_PARSER_INT_STATUS),
		read_dos_reg(hw, HEVC_PARSER_CORE_CONTROL), read_dos_reg(hw, H265_DEBUG_REG1),
		read_dos_reg(hw, H265_DECODE_MODE2), read_dos_reg(hw, HEVC_RPM_BUFFER),
		read_dos_reg(hw, H265_LMEM_DUMP_ADR), read_dos_reg(hw, HEVC_LMEM_DMA_CTRL),
		read_dos_reg(hw, HEVC_STREAM_LEVEL),
		read_dos_reg(hw, HEVC_STREAM_SWAP_BUFFER2), read_dos_reg(hw, HEVC_IMEM_DMA_CTRL));
}

static void h265_command(struct aml_vdec_hw *hw, u32 cmd)
{
	wr(hw, HEVC_ASSIST_MBOX0_CLR_REG, 1);
	wr(hw, HEVC_DEC_STATUS_REG, cmd);
	wr(hw, HEVC_MCPU_INTR_REQ, AMRISC_MAIN_REQ);
	dev_dbg(hw->dev, "hevc: command=%02x status=%08x request=%08x\n",
		cmd, read_dos_reg(hw, HEVC_DEC_STATUS_REG),
		read_dos_reg(hw, HEVC_MCPU_INTR_REQ));
}

static void h265_picture_done(struct aml_hevc_ctx *h)
{
	u32 info = read_dos_reg(h->hw, H265_DECODE_INFO);

	h->start_decoding_flag |= info & 0xff;
	h->rps_set_id = (info >> 8) & 0xff;
	h->have_picture = h->dpb.cur >= 0;
}

static irqreturn_t h265_irq_thread(int irq, void *priv)
{
	struct aml_hevc_ctx *h = priv;
	struct aml_vdec_hw *hw = h->hw;
	u32 status = hw->hevc.irq_status;
	u32 st = status & 0xff;

	dev_dbg(hw->dev, "hevc: irq status=%08x pc=%08x shift=%08x\n",
		status, read_dos_reg(hw, HEVC_MPC_E), read_dos_reg(hw, HEVC_SHIFT_BYTE_COUNT));
	if (h->finished)
		return IRQ_HANDLED;
	if (status == U32_MAX) {
		h265_finish(h, -EIO);
		return IRQ_HANDLED;
	}
	if (++h->rounds > 4096) {
		dev_err(hw->dev, "hevc: runaway decode, giving up\n");
		h265_finish(h, -EIO);
		return IRQ_HANDLED;
	}

	switch (st) {
	case H265_SEI_DAT:
		/* This handshake is polled by firmware, without an MCPU request. */
		wr(hw, HEVC_DEC_STATUS_REG, H265_SEI_DAT_DONE);
		return IRQ_HANDLED;
	case H265_NAL_SEARCH_DONE: {
		int nal = read_dos_reg(hw, H265_CUR_NAL_UNIT_TYPE);
		u32 parse = H265_DISCARD_NAL;

		if (nal == H265_NAL_EOS) {
			/* The host's AU metadata applies this boundary in byte order. */
		} else if (nal == H265_NAL_VPS) {
			parse = H265_NAL_UNIT_VPS;
			h->have_vps = true;
		} else if (h->have_vps && nal == H265_NAL_SPS) {
			parse = H265_NAL_UNIT_SPS;
			h->have_sps = true;
		} else if (h->have_vps && nal == H265_NAL_PPS) {
			parse = H265_NAL_UNIT_PPS;
			h->have_pps = true;
		} else if (h->have_vps && h->have_sps && h->have_pps && nal <= H265_NAL_CRA) {
			if (nal >= H265_NAL_BLA_W_LP) {
				parse = H265_NAL_UNIT_CODED_SLICE;
				h->have_start = true;
			} else if (h->have_start) {
				parse = H265_NAL_UNIT_CODED_SLICE;
			}
		}
		if (h->have_vps && h->have_sps && h->have_pps && h->have_start) {
			/* from here on let the microcode find NALs itself */
			wr(hw, H265_NAL_SEARCH_CTL, 0x2);
			h265_config_decode_mode(h);
		}
		dev_dbg(hw->dev, "hevc: nal %d -> %02x\n", nal, parse);
		h265_command(hw, parse);
		return IRQ_HANDLED;
	}
	case H265_SLICE_SEGMENT_DONE: {
		int ret;
		u32 info = read_dos_reg(hw, H265_DECODE_INFO);

		h->start_decoding_flag |= info & 0xff;
		h->rps_set_id = (info >> 8) & 0xff;
		h265_read_rpm(h);
		ret = h265_slice_header(h);
		if (ret == 2) {
			h->finished = true;
			h->v4l2_ctx->job->decode_result = 1;
			h->v4l2_ctx->job->int_cond = 1;
			wake_up(&h->v4l2_ctx->job->queue);
			return IRQ_HANDLED;
		}
		if (ret < 0) {
			dev_err(hw->dev, "hevc: slice setup failed (%d)\n", ret);
			h265_finish(h, -EIO);
			return IRQ_HANDLED;
		}
		if (ret > 0) {
			wr(hw, HEVC_WAIT_FLAG, read_dos_reg(hw, HEVC_WAIT_FLAG) & ~0x2);
			h265_command(hw, H265_ACTION_DONE);
			return IRQ_HANDLED;
		}
		h265_command(hw, H265_CODED_SLICE_SEGMENT_DAT);
		return IRQ_HANDLED;
	}
	case H265_DECPIC_DATA_DONE:
		h265_picture_done(h);
		h265_finish(h, h->have_picture ? 0 : -EIO);
		return IRQ_HANDLED;
	case H265_DECODE_BUFEMPTY:
	case H265_DECODE_BUFEMPTY2:
		/* Input exhaustion in a picture does not prove reconstruction completed. */
		h265_finish(h, -EIO);
		return IRQ_HANDLED;
	case H265_SEARCH_BUFEMPTY:
	case H265_NAL_DECODE_DONE:
	case H265_FIND_NEXT_PIC_NAL:
		h265_finish(h, h->dpb.cur < 0 ? 0 : -EIO);
		return IRQ_HANDLED;
	case H265_DECPIC_DATA_ERROR:
	case H265_DECODE_PARAMS_ERR:
	case H265_DECODE_OVER_SIZE:
	case H265_DECODE_TIMEOUT:
		dev_err(hw->dev, "hevc: decoder reported %02x\n", st);
		h->dpb.cur = -1;
		h265_finish(h, -EIO);
		return IRQ_HANDLED;
	default:
		dev_err(hw->dev, "hevc: unhandled status %02x\n", st);
		h265_finish(h, -EIO);
		return IRQ_HANDLED;
	}
}

/* Returns one for a parsed format, zero for a consumed AU, or a negative error. */
int h265_hw_run(struct aml_hevc_ctx *h, const void *data, size_t size)
{
	struct aml_vdec_ctx *ctx = h->v4l2_ctx;
	struct aml_vdec_hw *hw = h->hw;
	unsigned long elapsed;
	int ret;

	h->rounds = 0;
	h->have_picture = false;
	h->finished = false;
	h->dpb.cur = -1;
	ctx->job->int_cond = 0;
	ctx->job->decode_result = -EIO;
	ret = h265_feed(h, data, size);
	if (ret)
		return ret;
	/* Fresh frame-based input has no invalid prefix bytes. */
	wr(hw, HEVC_WAIT_FLAG, 0);
	h265_prot_init(h);
	h265_enable_input(h);
	/* Publish the initial command before resetting and releasing the CPUs. */
	wr(hw, HEVC_DEC_STATUS_REG, H265_ACTION_DONE);
	wr(hw, HEVC_SHIFT_BYTE_COUNT, 0);
	wr(hw, H265_DECODE_SIZE, h->payload);
	if (h->configured)
		h265_config_pic_list(h);
	/*
	 * amhevc_start() uses three readbacks before and after CPU reset
	 * to flush writes and provide a settling delay.
	 */
	read_dos_reg(hw, DOS_SW_RESET3);
	read_dos_reg(hw, DOS_SW_RESET3);
	read_dos_reg(hw, DOS_SW_RESET3);
	wr(hw, DOS_SW_RESET3, BIT(12) | BIT(11));
	wr(hw, DOS_SW_RESET3, 0);
	read_dos_reg(hw, DOS_SW_RESET3);
	read_dos_reg(hw, DOS_SW_RESET3);
	read_dos_reg(hw, DOS_SW_RESET3);
	wr(hw, HEVC_MPSR, 1);
	h265_dump_start(hw, "after-start");

	/* m2m cancellation waits for this bounded job and the subsequent DMA stop. */
	elapsed = wait_event_timeout(ctx->job->queue, READ_ONCE(ctx->job->int_cond),
				     msecs_to_jiffies(H265_TIMEOUT_MS));
	ret = elapsed ? ctx->job->decode_result : -ETIMEDOUT;
	if (!elapsed) {
		h265_dump_start(hw, "timeout");
		dev_err(hw->dev, "hevc: timeout, irq %02x status %02x pc %08x shift %08x\n",
			hw->hevc.irq_status, read_dos_reg(hw, HEVC_DEC_STATUS_REG),
			read_dos_reg(hw, HEVC_MPC_E),
			read_dos_reg(hw, HEVC_SHIFT_BYTE_COUNT));
	}
	return ret;
}

static int hevc_job_acquire(struct aml_vdec_ctx *ctx)
{
	struct aml_hevc_ctx *h = ctx->codec_priv;
	int ret, stop_ret;

	h->hw->hevc.job = ctx->job;
	ret = aml_hevc_prepare_hw(h->hw);
	if (!ret)
		ret = aml_hevc_enable_irq(h->hw, h265_irq_thread, h);
	if (!ret)
		return 0;
	stop_ret = aml_vdec_job_stop(ctx);
	if (stop_ret)
		return stop_ret;
	if (h->dpb.cur >= 0) {
		h->dpb.pic[h->dpb.cur].valid = false;
		h->dpb.pic[h->dpb.cur].referenced = false;
	}
	return ret;
}

static int hevc_job_stop(struct aml_vdec_ctx *ctx)
{
	return aml_hevc_stop_hw(ctx->dev->dec_hw);
}

static void hevc_job_release(struct aml_vdec_ctx *ctx)
{
	ctx->dev->dec_hw->hevc.job = NULL;
}

const struct aml_vdec_job_ops aml_hevc_job_ops = {
	.acquire = hevc_job_acquire,
	.stop = hevc_job_stop,
	.release = hevc_job_release,
};
