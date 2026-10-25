// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * HEVC (H.265) decoding on the Amlogic S4 HEVC core.
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
 * Pictures are written compressed through the MMU (the reference picture the
 * hardware reads back) and, by the double-write unit, as plain NV12 - straight
 * into the CAPTURE buffer when its geometry allows.
 *
 * This first version is driven with raw Annex-B access units, one per OUTPUT
 * buffer, and hands pictures back in decode order.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "aml_vdec_drv.h"
#include "aml_vdec_hw.h"
#include "hevc.h"

/* Workspace, the reference's amvh265_workbuff_spec[1] (4096x2048). */
#define H265_IPP_SIZE		0x4000
#define H265_SAO_ABV_BUF_SIZE	0x30000
#define H265_SAO_VB_BUF_SIZE	0x30000
#define H265_SHORT_TERM_RPS_SIZE 0x0800
#define H265_VPS_SIZE		0x0800
#define H265_SPS_SIZE		0x0800
#define H265_PPS_SIZE		0x2000
#define H265_SAO_UP_SIZE	0x2800
#define H265_SWAP_BUF_SIZE	0x0800
#define H265_SWAP_BUF2_SIZE	0x0800
#define H265_SCALELUT_SIZE	0x8000
#define H265_DBLK_PARA_SIZE	0x19100
#define H265_DBLK_DATA_SIZE	0x88800
#define H265_DBLK_DATA2_SIZE	0x48800
#define H265_MMU_VBH_SIZE	0x5000
#define H265_MPRED_ABOVE_SIZE	0x4000

#define H265_RPM_SIZE		(0x80 * 2)
#define H265_LMEM_SIZE		(0x500 * 2)
#define H265_MMU_MAP_SIZE	(0x1200 * 4)
#define H265_AUX_PREFIX_SIZE	(12 * 1024)
#define H265_AUX_SUFFIX_SIZE	(12 * 1024)

#define H265_PADDING_SIZE	(16 * 1024)
#define H265_TIMEOUT_MS		500

/* 16 references plus the picture being decoded plus slack. */
#define H265_SLOTS		18
#define H265_MAX_REF		16
#define H265_MAX_TILE_COLS	10
#define H265_MAX_TILE_ROWS	20
#define H265_MIN_DIM		16
#define H265_MAX_DIM		4096

#define H265_INVALID_POC	0x80000000
#define H265_MAX_INT		0x7fffffff

/* CUR_RPS entries, SUPPORT_LONG_TERM_RPS layout. */
#define RPS_END			0x8000
#define RPS_LT_BIT		14
#define RPS_USED_BIT		13

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

/* The RPM parameter block, the reference's union param_u, as u16 indices. */
enum {
	P_CUR_RPS = 0,			/* 16 entries */
	P_NUM_REF_IDX_L0_ACTIVE = 16,
	P_NUM_REF_IDX_L1_ACTIVE,
	P_SLICE_TYPE,
	P_SLICE_TEMPORAL_MVP_ENABLE_FLAG,
	P_DEPENDENT_SLICE_SEGMENT_FLAG,
	P_SLICE_SEGMENT_ADDRESS,
	P_NUM_TITLE_ROWS_MINUS1,
	P_PIC_WIDTH,
	P_PIC_HEIGHT,
	P_LOG2_MIN_CB_SIZE_MINUS3,
	P_LOG2_DIFF_MAX_MIN_CB_SIZE,
	P_LOG2_MAX_POC_LSB_MINUS4,
	P_POC_LSB,
	P_COLLOCATED_FROM_L0_FLAG,
	P_COLLOCATED_REF_IDX,
	P_LOG2_PARALLEL_MERGE_LEVEL,
	P_FIVE_MINUS_MAX_NUM_MERGE_CAND,
	P_SPS_NUM_REORDER_PICS_0,
	P_MODIFICATION_FLAG,
	P_TILES_ENABLED_FLAG,
	P_NUM_TILE_COLUMNS_MINUS1,
	P_NUM_TILE_ROWS_MINUS1,
	P_TILE_WIDTH,			/* 12 entries */
	P_TILE_HEIGHT = P_TILE_WIDTH + 12,	/* 8 entries */
	P_MISC_FLAG0 = P_TILE_HEIGHT + 8,
	P_PPS_BETA_OFFSET_DIV2,
	P_PPS_TC_OFFSET_DIV2,
	P_SLICE_BETA_OFFSET_DIV2,
	P_SLICE_TC_OFFSET_DIV2,
	P_PPS_CB_QP_OFFSET,
	P_PPS_CR_QP_OFFSET,
	P_FIRST_SLICE_SEGMENT_IN_PIC_FLAG,
	P_TEMPORAL_ID,
	P_NAL_UNIT_TYPE,
	P_VUI_NUM_UNITS_IN_TICK_HI,
	P_VUI_NUM_UNITS_IN_TICK_LO,
	P_VUI_TIME_SCALE_HI,
	P_VUI_TIME_SCALE_LO,
	P_BIT_DEPTH,
	P_PROFILE_ETC,
	P_SEI_FRAME_FIELD_INFO,
	P_VIDEO_SIGNAL_TYPE,
	P_MODIFICATION_LIST,		/* 0x20 entries */
	P_CONFORMANCE_WINDOW_FLAG = P_MODIFICATION_LIST + 0x20,
	P_CONF_WIN_LEFT_OFFSET,
	P_CONF_WIN_RIGHT_OFFSET,
	P_CONF_WIN_TOP_OFFSET,
	P_CONF_WIN_BOTTOM_OFFSET,
	P_CHROMA_FORMAT_IDC,
	P_COLOR_DESCRIPTION,
	P_ASPECT_RATIO_IDC,
	P_SAR_WIDTH,
	P_SAR_HEIGHT,
	P_SPS_MAX_DEC_PIC_BUFFERING_MINUS1_0,
	P_RPM_COUNT = 0x80
};

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

struct h265_pic {
	bool valid;		/* holds a decoded or decoding picture */
	bool referenced;
	bool long_term;
	int poc;
	u32 decode_idx;
	u32 width, height;
	int slice_type;

	void *body_v, *hdr_v, *mv_v, *dw_v;
	dma_addr_t body_p, hdr_p, mv_p, dw_p;
};

struct h265_tile {
	int start_cu_x, start_cu_y, width, height;
	dma_addr_t sao_vb, sao_abv;
};

struct aml_hevc_ctx {
	struct aml_vdec_ctx *v4l2_ctx;
	struct aml_vdec_hw *hw;
	bool ucode_loaded;
	bool started;
	int rounds;
	bool have_picture;
	bool finished;

	/* workspace */
	void *ws_v;
	dma_addr_t ws_p;
	size_t ws_size;
	dma_addr_t ipp, sao_abv, sao_vb, short_term_rps, vps, sps, pps, sao_up;
	dma_addr_t swap_buf, swap_buf2, scalelut, dblk_para, dblk_data;
	dma_addr_t dblk_data2, mmu_vbh, mpred_above;
	void *side_v;
	dma_addr_t side_p;
	size_t side_size;
	dma_addr_t rpm, lmem, mmu_map, aux;
	void *stream_v;
	dma_addr_t stream_p;
	size_t stream_size;
	size_t payload;

	/* picture buffers */
	struct h265_pic pic[H265_SLOTS];
	size_t body_size, hdr_size, mv_size, dw_size;
	u32 alloc_w, alloc_h;
	/* picture slots in use: the stream's DPB size plus two, at most H265_SLOTS */
	int nslots, alloc_nslots;
	bool ten_bit, alloc_ten_bit;
	dma_addr_t dw_target_y, dw_target_c;
	size_t dw_target_size;

	/* stream state kept across access units */
	u32 start_decoding_flag;
	u32 rps_set_id;
	bool have_vps, have_sps, have_pps, have_start;
	int prev_tid0_poc;
	int poc_random_access;
	u32 decode_idx;

	/* picture state */
	u16 p[P_RPM_COUNT];
	int cur;		/* slot being decoded, -1 when none */
	int col;
	int curr_poc, iprev_poc, col_poc;
	bool new_pic;
	int nal_type, temporal_id;
	u32 pic_w, pic_h;
	int lcu_size, lcu_size_log2, lcu_x_num, lcu_y_num, lcu_total;
	int slice_addr, slice_segment_addr;
	int slice_type;
	bool tmvp, is_next_slice_segment, ldc, col_from_l0;
	int plevel, max_merge_cand;
	int ref_num_l0, ref_num_l1;
	int ref_poc_l0[H265_MAX_REF], ref_poc_l1[H265_MAX_REF];
	u32 misc_flag0;

	bool tile_enabled;
	int num_tile_col, num_tile_row;
	struct h265_tile tile[H265_MAX_TILE_ROWS][H265_MAX_TILE_COLS];
	int tile_x, tile_y;
	bool new_tile;
	int tile_start_lcu_x, tile_start_lcu_y, tile_width_lcu, tile_height_lcu;
};

static void wr(struct aml_vdec_hw *hw, u32 reg, u32 val)
{
	regmap_write(hw->map[DOS_BUS], reg, val);
}

/* ---------------------------------------------------------------- */
/* buffers                                                          */
/* ---------------------------------------------------------------- */

static void h265_free_pics(struct aml_hevc_ctx *h)
{
	struct device *dev = h->hw->dev;
	int i;

	for (i = 0; i < H265_SLOTS; i++) {
		struct h265_pic *pic = &h->pic[i];

		if (pic->body_v)
			dma_free_coherent(dev, h->body_size, pic->body_v, pic->body_p);
		if (pic->hdr_v)
			dma_free_coherent(dev, h->hdr_size, pic->hdr_v, pic->hdr_p);
		if (pic->mv_v)
			dma_free_coherent(dev, h->mv_size, pic->mv_v, pic->mv_p);
		if (pic->dw_v)
			dma_free_noncoherent(dev, h->dw_size, pic->dw_v, pic->dw_p,
					     DMA_FROM_DEVICE);
		memset(pic, 0, sizeof(*pic));
	}
	h->body_size = 0;
	h->hdr_size = 0;
	h->mv_size = 0;
	h->dw_size = 0;
	h->alloc_w = 0;
	h->alloc_h = 0;
	h->alloc_nslots = 0;
}

static void h265_free(struct aml_hevc_ctx *h)
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

static int h265_alloc_workspace(struct aml_hevc_ctx *h)
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
		h265_free(h);
		return -ENOMEM;
	}
	memset(h->side_v, 0, h->side_size);
	p = h->side_p;
	h->rpm = h265_carve(&p, H265_RPM_SIZE);
	h->lmem = h265_carve(&p, H265_LMEM_SIZE);
	h->mmu_map = h265_carve(&p, H265_MMU_MAP_SIZE);
	h->aux = h265_carve(&p, H265_AUX_PREFIX_SIZE + H265_AUX_SUFFIX_SIZE);

	dev_info(dev, "hevc: workspace %zu KiB at %pad, side %zu KiB\n",
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

static size_t h265_mv_buf_size(u32 w, u32 h, int lcu_size)
{
	int unit = lcu_size == 64 ? 0x200 : lcu_size == 32 ? 0x80 : 0x20;
	u32 xn = DIV_ROUND_UP(w, lcu_size), yn = DIV_ROUND_UP(h, lcu_size);

	return ALIGN(xn * yn * unit, SZ_64K);
}

static int h265_alloc_pics(struct aml_hevc_ctx *h)
{
	struct device *dev = h->hw->dev;
	u32 w = h->pic_w, ht = h->pic_h;
	size_t body = ALIGN(h265_comp_body_size(w, ht, h->ten_bit), PAGE_SIZE);
	/* Size the MV buffers for the smallest LCU, so they fit any. */
	size_t mv = h265_mv_buf_size(w, ht, 16);
	size_t hdr = h265_hdr_alloc_size(w, ht);
	size_t dw = h->dw_target_y ? 0 : ALIGN(w * ht * 3 / 2, SZ_64K);
	int i;

	if (h->alloc_w == w && h->alloc_h == ht && h->dw_size == dw &&
	    h->alloc_ten_bit == h->ten_bit && h->alloc_nslots == h->nslots)
		return 0;

	h265_free_pics(h);
	for (i = 0; i < h->nslots; i++) {
		struct h265_pic *pic = &h->pic[i];

		pic->body_v = dma_alloc_coherent(dev, body, &pic->body_p, GFP_KERNEL);
		pic->hdr_v = dma_alloc_coherent(dev, hdr, &pic->hdr_p, GFP_KERNEL);
		pic->mv_v = dma_alloc_coherent(dev, mv, &pic->mv_p, GFP_KERNEL);
		if (dw)
			pic->dw_v = dma_alloc_noncoherent(dev, dw, &pic->dw_p,
							  DMA_FROM_DEVICE, GFP_KERNEL);
		if (!pic->body_v || !pic->hdr_v || !pic->mv_v || (dw && !pic->dw_v)) {
			h->body_size = body;
			h->hdr_size = hdr;
			h->mv_size = mv;
			h->dw_size = dw;
			h265_free_pics(h);
			return -ENOMEM;
		}
		memset(pic->mv_v, 0, mv);
		pic->poc = H265_INVALID_POC;
	}
	h->body_size = body;
	h->hdr_size = hdr;
	h->mv_size = mv;
	h->dw_size = dw;
	h->alloc_w = w;
	h->alloc_h = ht;
	h->alloc_ten_bit = h->ten_bit;
	h->alloc_nslots = h->nslots;

	/* init_pic_list_hw(): the compressed-header address of every picture. */
	wr(h->hw, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, BIT(1) | BIT(2));
	for (i = 0; i < H265_SLOTS; i++)
		wr(h->hw, HEVCD_MPP_ANC2AXI_TBL_DATA,
		   i < h->nslots ? h->pic[i].hdr_p >> 5 : 0);
	wr(h->hw, HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, BIT(0));
	wr(h->hw, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, BIT(0));
	for (i = 0; i < 32; i++)
		wr(h->hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR, 0);

	dev_info(dev, "hevc: %d pictures %ux%u, body %zu KiB, mv %zu KiB, dw %zu KiB each\n",
		 h->nslots, w, ht, body / 1024, mv / 1024, dw / 1024);
	return 0;
}

/* ---------------------------------------------------------------- */
/* hardware setup                                                   */
/* ---------------------------------------------------------------- */

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
	wr(hw, HEVC_DBLK_CFG3, 0x4010);	/* <= 4096x2304 */
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
	wr(hw, DOS_SW_RESET3, 0);
	regmap_update_bits(hw->map[DOS_BUS], HEVC_CABAC_CONTROL, 1, 0);
	regmap_update_bits(hw->map[DOS_BUS], HEVC_PARSER_CORE_CONTROL, 1, 0);

	wr(hw, HEVC_SHIFT_STARTCODE, 0x00000100);
	wr(hw, HEVC_SHIFT_EMULATECODE, 0x00000300);

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

	/* performance_profile: multi slice in ucode (HEVC_8K_LFTOFFSET_FIX) */
	wr(hw, H265_NAL_SEARCH_CTL, read_dos_reg(hw, H265_NAL_SEARCH_CTL) | BIT(21));
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

	wr(hw, HEVC_WAIT_FLAG, 1);
	wr(hw, HEVC_ASSIST_MBOX0_CLR_REG, 1);
	wr(hw, HEVC_ASSIST_MBOX0_MASK, 1);
	wr(hw, HEVC_PSCALE_CTRL, 0);
	wr(hw, H265_DEBUG_REG1, 0);
	/* check vps/sps/pps/i-slice in ucode */
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
	size_t want = ALIGN(size + H265_PADDING_SIZE, SZ_64K);

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

	wr(hw, HEVC_STREAM_START_ADDR, h->stream_p);
	wr(hw, HEVC_STREAM_END_ADDR, h->stream_p + h->stream_size - 1);
	wr(hw, HEVC_STREAM_RD_PTR, h->stream_p);
	wr(hw, HEVC_STREAM_WR_PTR, h->stream_p + size + H265_PADDING_SIZE);
	wr(hw, HEVC_SHIFT_BYTE_COUNT, 0);
	wr(hw, H265_DECODE_SIZE, size);
	wr(hw, HEVC_STREAM_CONTROL,
	   (read_dos_reg(hw, HEVC_STREAM_CONTROL) & ~0xfff0) | (7 << 4) | BIT(0));
	wr(hw, HEVC_STREAM_FIFO_CTL,
	   read_dos_reg(hw, HEVC_STREAM_FIFO_CTL) | BIT(29));
	return 0;
}

/* ---------------------------------------------------------------- */
/* picture management                                               */
/* ---------------------------------------------------------------- */

static int h265_pic_by_poc(struct aml_hevc_ctx *h, int poc, bool ref_only)
{
	int i, best = -1;

	if (poc == (int)H265_INVALID_POC)
		return -1;
	for (i = 0; i < H265_SLOTS; i++) {
		struct h265_pic *pic = &h->pic[i];

		if (!pic->valid || pic->poc != poc || i == h->cur)
			continue;
		if (ref_only && (!pic->referenced || pic->width != h->pic_w ||
				 pic->height != h->pic_h))
			continue;
		if (best < 0 || pic->decode_idx > h->pic[best].decode_idx)
			best = i;
	}
	return best;
}

/* Decode a CUR_RPS entry to a POC; returns false at the end marker. */
static bool h265_rps_poc(struct aml_hevc_ctx *h, int i, int *poc, bool *used, bool *lt)
{
	u16 e = h->p[P_CUR_RPS + i];
	int delt;

	if (e == RPS_END)
		return false;
	delt = e & ((1 << (RPS_USED_BIT - 1)) - 1);
	if (e & (1 << (RPS_USED_BIT - 1)))
		*poc = h->curr_poc - ((1 << (RPS_USED_BIT - 1)) - delt);
	else
		*poc = h->curr_poc + delt;
	*used = (e >> RPS_USED_BIT) & 1;
	*lt = (e >> RPS_LT_BIT) & 1;
	return true;
}

/* Debug: log each picture's POC, RPS entries and the slots' POCs. */

/* apply_ref_pic_set(): everything the current RPS does not name stops being
 * a reference.
 */
static void h265_apply_rps(struct aml_hevc_ctx *h)
{
	int i, j;

	for (i = 0; i < H265_SLOTS; i++) {
		struct h265_pic *pic = &h->pic[i];
		bool found = false;

		pic->long_term = false;
		if (!pic->valid || !pic->referenced || pic->poc == h->curr_poc)
			continue;
		for (j = 0; j < 16; j++) {
			int poc;
			bool used, lt;

			if (!h265_rps_poc(h, j, &poc, &used, &lt))
				break;
			if (poc == pic->poc) {
				pic->long_term = lt;
				found = true;
				break;
			}
		}
		if (!found)
			pic->referenced = false;
	}
}

/*
 * Entry @c of a reference list built from the short-term pictures in @first
 * and @second order, followed by the long-term ones.
 */
static int h265_ref_entry(int c, const int *first, int n_first,
			  const int *second, int n_second, const int *lt)
{
	if (c >= n_first + n_second)
		return lt[c - n_first - n_second];
	if (c >= n_first)
		return second[c - n_first];
	return first[c];
}

/* set_ref_pic_list() */
static void h265_set_ref_lists(struct aml_hevc_ctx *h)
{
	int st0[16], st1[16], lt[16];
	int n_neg = 0, n_pos = 0, n_lt = 0, total, i, r;
	int l0 = min_t(int, h->p[P_NUM_REF_IDX_L0_ACTIVE], H265_MAX_REF);
	int l1 = min_t(int, h->p[P_NUM_REF_IDX_L1_ACTIVE], H265_MAX_REF);
	u16 mod = h->p[P_MODIFICATION_FLAG];

	memset(h->ref_poc_l0, 0, sizeof(h->ref_poc_l0));
	memset(h->ref_poc_l1, 0, sizeof(h->ref_poc_l1));

	for (i = 0; i < 16; i++) {
		u16 e = h->p[P_CUR_RPS + i];
		int poc;
		bool used, is_lt;

		if (!h265_rps_poc(h, i, &poc, &used, &is_lt))
			break;
		if (!used)
			continue;
		if (is_lt)
			lt[n_lt++] = poc;
		else if (e & (1 << (RPS_USED_BIT - 1)))
			st0[n_neg++] = poc;
		else
			st1[n_pos++] = poc;
	}
	total = n_neg + n_pos + n_lt;

	if (total > 0) {
		for (r = 0; r < l0; r++) {
			int c = (mod & 1) ? h->p[P_MODIFICATION_LIST + r] : r % total;

			if (c < total)
				h->ref_poc_l0[r] = h265_ref_entry(c, st0, n_neg,
								  st1, n_pos, lt);
		}
		if (h->p[P_SLICE_TYPE] == H265_B_SLICE) {
			for (r = 0; r < l1; r++) {
				int c;

				if (mod & 2)
					c = h->p[P_MODIFICATION_LIST + ((mod & 1) ? l0 : 0) + r];
				else
					c = r % total;
				if (c < total)
					h->ref_poc_l1[r] = h265_ref_entry(c, st1, n_pos,
									  st0, n_neg, lt);
			}
		}
	}
	{
		struct v4l2_ctrl *ctrl = v4l2_ctrl_find(&h->v4l2_ctx->ctrl_handler,
							V4L2_CID_AML_HEVC_REFS);

		if (ctrl && ctrl->p_cur.p_s32[0] == 1) {
			for (r = 0; r < l0; r++)
				h->ref_poc_l0[r] = ctrl->p_cur.p_s32[1 + r];
			if (h->p[P_SLICE_TYPE] == H265_B_SLICE)
				for (r = 0; r < l1; r++)
					h->ref_poc_l1[r] = ctrl->p_cur.p_s32[17 + r];
		}
	}
	h->ref_num_l0 = l0;
	h->ref_num_l1 = l1;
}

static void h265_update_tiles(struct aml_hevc_ctx *h, int w_cu, int h_cu,
			      int sao_mem_unit)
{
	int sao_vb_size = (sao_mem_unit + (2 << 4)) * h_cu;
	int sao_abv_size = sao_mem_unit * w_cu;
	int i, j, x, y;
	bool uniform;

	h->tile_enabled = h->p[P_TILES_ENABLED_FLAG] & 1;
	if (!h->tile_enabled) {
		h->num_tile_col = 1;
		h->num_tile_row = 1;
		h->tile[0][0] = (struct h265_tile){ 0, 0, w_cu, h_cu,
						    h->sao_vb, h->sao_abv };
		return;
	}
	h->num_tile_col = h->p[P_NUM_TILE_COLUMNS_MINUS1] + 1;
	h->num_tile_row = h->p[P_NUM_TILE_ROWS_MINUS1] + 1;
	if (h->num_tile_col > H265_MAX_TILE_COLS || h->num_tile_col <= 0)
		h->num_tile_col = 1;
	if (h->num_tile_row > H265_MAX_TILE_ROWS || h->num_tile_row <= 0)
		h->num_tile_row = 1;
	uniform = h->p[P_TILES_ENABLED_FLAG] & 2;

	y = 0;
	for (i = 0; i < h->num_tile_row; i++) {
		x = 0;
		for (j = 0; j < h->num_tile_col; j++) {
			struct h265_tile *t = &h->tile[i][j];

			if (j == h->num_tile_col - 1)
				t->width = w_cu - x;
			else
				t->width = uniform ? w_cu / h->num_tile_col :
						     h->p[P_TILE_WIDTH + j];
			if (i == h->num_tile_row - 1)
				t->height = h_cu - y;
			else
				t->height = uniform ? h_cu / h->num_tile_row :
						      h->p[P_TILE_HEIGHT + i];
			t->start_cu_x = x;
			t->start_cu_y = y;
			t->sao_vb = h->sao_vb + j * sao_vb_size;
			t->sao_abv = h->sao_abv + i * sao_abv_size;
			x += t->width;
		}
		y += h->tile[i][0].height;
	}
}

static int h265_tile_index(struct aml_hevc_ctx *h, int cu_adr, int w_lcu)
{
	int cu_x, cu_y, tx = 0, ty = 0, i;

	if (!w_lcu)
		return -1;
	cu_x = cu_adr % w_lcu;
	cu_y = cu_adr / w_lcu;
	for (i = 0; i < h->num_tile_col; i++) {
		if (cu_x >= h->tile[0][i].start_cu_x)
			tx = i;
		else
			break;
	}
	for (i = 0; i < h->num_tile_row; i++) {
		if (cu_y >= h->tile[i][0].start_cu_y)
			ty = i;
		else
			break;
	}
	return tx | (ty << 8);
}

/* ---------------------------------------------------------------- */
/* per-slice hardware configuration                                 */
/* ---------------------------------------------------------------- */

static void h265_fill_mmu_map(struct aml_hevc_ctx *h)
{
	struct h265_pic *pic = &h->pic[h->cur];
	u32 *map = h->side_v + (h->mmu_map - h->side_p);
	u32 pages = h->body_size / PAGE_SIZE, first = pic->body_p >> PAGE_SHIFT, i;

	for (i = 0; i < pages && i < H265_MMU_MAP_SIZE / 4; i++)
		map[i] = first + i;
}

/* init_decode_head_hw() */
static void h265_config_comp(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	u32 body = h265_comp_body_size(h->pic_w, h->pic_h, h->ten_bit);
	u32 v;

	wr(hw, HEVCD_MPP_DECOMP_CTL1, BIT(4));	/* paged memory */
	wr(hw, HEVCD_MPP_DECOMP_CTL2, 0);
	wr(hw, HEVC_CM_BODY_LENGTH, body);
	wr(hw, HEVC_CM_HEADER_OFFSET, body);
	wr(hw, HEVC_CM_HEADER_LENGTH, h265_comp_header_size(h->pic_w, h->pic_h));
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

	if (h->slice_type == H265_I_SLICE)
		return;
	wr(hw, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, BIT(0));
	for (i = 0; i < h->ref_num_l0; i++) {
		int s = h265_pic_by_poc(h, h->ref_poc_l0[i], true);

		if (s < 0)
			s = h->cur;
		wr(hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR, (s << 16) | (s << 8) | s);
	}
	if (h->slice_type != H265_B_SLICE)
		return;
	wr(hw, HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, (16 << 8) | BIT(0));
	for (i = 0; i < h->ref_num_l1; i++) {
		int s = h265_pic_by_poc(h, h->ref_poc_l1[i], true);

		if (s < 0)
			s = h->cur;
		wr(hw, HEVCD_MPP_ANC_CANVAS_DATA_ADDR, (s << 16) | (s << 8) | s);
	}
}

/* config_mcrcc_axi_hw() */
static void h265_config_mcrcc(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	u32 a, b;

	wr(hw, HEVCD_MCRCC_CTL1, 0x2);		/* reset mcrcc */
	if (h->slice_type == H265_I_SLICE) {
		wr(hw, HEVCD_MCRCC_CTL1, 0x0);
		return;
	}
	if (h->slice_type == H265_B_SLICE) {
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
	struct h265_pic *cur = &h->pic[h->cur], *col = &h->pic[h->col];
	int unit = h->lcu_size_log2 == 6 ? 0x200 : h->lcu_size_log2 == 5 ? 0x80 : 0x20;
	dma_addr_t rd_start = col->mv_p;
	dma_addr_t rd_ptr = rd_start + h->slice_addr * unit;
	dma_addr_t wr_ptr = cur->mv_p + h->slice_addr * unit;
	bool above_en = true, mv_wr_en = true;
	bool mv_rd_en = h->slice_type != H265_I_SLICE;
	u32 v, lt = 0, en0 = 0, en1 = 0;
	int i;

	wr(hw, HEVC_MPRED_MV_WR_START_ADDR, cur->mv_p);
	wr(hw, HEVC_MPRED_MV_RD_START_ADDR, rd_start);
	v = (h->lcu_x_num - h->tile_width_lcu) * unit;
	wr(hw, HEVC_MPRED_MV_WR_ROW_JUMP, v);
	wr(hw, HEVC_MPRED_MV_RD_ROW_JUMP, v);

	v = (h->slice_type & 3) |
	    (h->new_pic << 2) |
	    (h->new_tile << 3) |
	    (h->is_next_slice_segment << 4) |
	    (h->tmvp << 5) |
	    (h->ldc << 6) |
	    (h->col_from_l0 << 7) |
	    (0 << 8) |			/* above_ptr_ctrl */
	    (above_en << 9) |
	    (mv_wr_en << 10) |
	    (mv_rd_en << 11) |
	    (0 << 12) |			/* col_isIntra */
	    (1 << 13) |			/* buffer_linear */
	    (0 << 14) | (0 << 15) |	/* LongTerm_Curr, LongTerm_Col: no effect seen */
	    ((h->lcu_size_log2 & 0xf) << 16) |
	    (3 << 20) |			/* cu_size_log2 */
	    ((h->plevel & 7) << 24);
	wr(hw, HEVC_MPRED_CTRL0, v);

	wr(hw, HEVC_MPRED_CTRL1, h->max_merge_cand | (2 << 4) | (3 << 8) |
				 (5 << 12) | (36 << 16));
	wr(hw, HEVC_MPRED_PIC_SIZE, h->pic_w | (h->pic_h << 16));
	wr(hw, HEVC_MPRED_PIC_SIZE_LCU, (h->lcu_x_num - 1) | ((h->lcu_y_num - 1) << 16));
	wr(hw, HEVC_MPRED_TILE_START, h->tile_start_lcu_x | (h->tile_start_lcu_y << 16));
	wr(hw, HEVC_MPRED_TILE_SIZE_LCU, h->tile_width_lcu | (h->tile_height_lcu << 16));
	wr(hw, HEVC_MPRED_REF_NUM, h->ref_num_l0 | (h->ref_num_l1 << 8));

	for (i = 0; i < h->ref_num_l0; i++) {
		int s = h265_pic_by_poc(h, h->ref_poc_l0[i], true);

		if (s >= 0 && h->pic[s].long_term)
			lt |= BIT(i);
		en0 |= BIT(i);
	}
	for (i = 0; i < h->ref_num_l1; i++) {
		int s = h265_pic_by_poc(h, h->ref_poc_l1[i], true);

		if (s >= 0 && h->pic[s].long_term)
			lt |= BIT(i + 16);
		en1 |= BIT(i);
	}
	wr(hw, HEVC_MPRED_LT_REF, lt);
	wr(hw, HEVC_MPRED_REF_EN_L0, en0);
	wr(hw, HEVC_MPRED_REF_EN_L1, en1);
	wr(hw, HEVC_MPRED_CUR_POC, h->curr_poc);
	wr(hw, HEVC_MPRED_COL_POC, h->col_poc);

	/* L0/L1 pairs, in this order. */
	for (i = 0; i < 16; i++) {
		wr(hw, HEVC_MPRED_L0_REF00_POC + REG_ALIGN(i), h->ref_poc_l0[i]);
		wr(hw, HEVC_MPRED_L1_REF00_POC + REG_ALIGN(i), h->ref_poc_l1[i]);
	}

	if (h->new_pic) {
		wr(hw, HEVC_MPRED_ABV_START_ADDR, h->mpred_above);
		wr(hw, HEVC_MPRED_MV_WPTR, wr_ptr);
		wr(hw, HEVC_MPRED_MV_RPTR, rd_start);
	} else if (!h->is_next_slice_segment) {
		wr(hw, HEVC_MPRED_MV_RPTR, rd_ptr);
	}
	wr(hw, HEVC_MPRED_MV_RD_END_ADDR, rd_start + h->mv_size);
}

/* config_sao_hw() for compressed reference plus 1:1 NV12 double write */
static void h265_config_sao(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	struct h265_pic *cur = &h->pic[h->cur];
	u32 misc = h->misc_flag0;
	u32 uv_size = h->lcu_total * h->lcu_size * h->lcu_size / 2;
	u32 uv_h = (uv_size + 0xffff) >> 16;
	dma_addr_t dw_y, dw_c;
	u32 v, v2;
	bool slice_dbk_disabled;

	if (h->dw_target_y) {
		dw_y = h->dw_target_y;
		dw_c = h->dw_target_c;
	} else {
		dw_y = cur->dw_p;
		dw_c = dw_y + h->pic_w * h->pic_h;
	}

	v = read_dos_reg(hw, HEVC_SAO_CTRL0);
	v &= ~0xf;
	v |= h->lcu_size_log2;
	wr(hw, HEVC_SAO_CTRL0, v);
	wr(hw, HEVC_SAO_PIC_SIZE, h->pic_w | (h->pic_h << 16));
	wr(hw, HEVC_SAO_PIC_SIZE_LCU, (h->lcu_x_num - 1) | ((h->lcu_y_num - 1) << 16));

	v = read_dos_reg(hw, HEVC_SAO_CTRL5);
	v &= ~(0xff << 16);		/* double write 1:1 */
	if (h->ten_bit)
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

	if (h->new_pic) {
		wr(hw, HEVC_DBLK_CFG2, h->pic_w | (h->pic_h << 16));
		v = 0;
		if ((misc >> PCM_ENABLE_FLAG_BIT) & 1)
			v = ((misc >> PCM_LOOP_FILTER_DISABLED_FLAG_BIT) & 1) << 3;
		v |= ((h->p[P_PPS_CB_QP_OFFSET] & 0x1f) << 4) |
		     ((h->p[P_PPS_CR_QP_OFFSET] & 0x1f) << 9);
		v |= h->lcu_size == 64 ? 0 : h->lcu_size == 32 ? 1 : 2;
		v |= h->pic_w <= 64 ? BIT(20) : 0;
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
	wr(hw, HEVC_SAO_CTRL1, v);

	/* 1st and 2nd write enabled, H.265 format */
	v = 0x57 << 8;
	if (h->pic_w >= 1280)
		v |= BIT(4);		/* dblk pipeline mode */
	v |= BIT(8) | BIT(9);
	wr(hw, HEVC_DBLK_CFGB, v);

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
	if (h->tile_enabled) {
		v |= ((misc >> LOOP_FILTER_ACROSS_TILES_ENABLED_FLAG_BIT) & 1) << 0;
		v2 |= ((misc >> LOOP_FILTER_ACROSS_TILES_ENABLED_FLAG_BIT) & 1) << 8;
	}
	slice_dbk_disabled = (misc >> SLICE_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 1;
	if ((misc & BIT(DEBLOCKING_FILTER_OVERRIDE_ENABLED_FLAG_BIT)) &&
	    (misc & BIT(DEBLOCKING_FILTER_OVERRIDE_FLAG_BIT))) {
		v |= slice_dbk_disabled << 2;
		if (!slice_dbk_disabled) {
			v |= (h->p[P_SLICE_BETA_OFFSET_DIV2] & 0xf) << 3;
			v |= (h->p[P_SLICE_TC_OFFSET_DIV2] & 0xf) << 7;
		}
	} else {
		v |= ((misc >> PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 1) << 2;
		if (!((misc >> PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 1)) {
			v |= (h->p[P_PPS_BETA_OFFSET_DIV2] & 0xf) << 3;
			v |= (h->p[P_PPS_TC_OFFSET_DIV2] & 0xf) << 7;
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

	for (i = 0; i < P_RPM_COUNT; i += 4)
		for (j = 0; j < 4; j++)
			h->p[i + j] = raw[i + 3 - j];
}

/*
 * hevc_slice_segment_header_process().  Returns 0 to decode the slice, 1 to
 * skip it, negative on error.
 */
static int h265_slice_header(struct aml_hevc_ctx *h)
{
	struct aml_vdec_hw *hw = h->hw;
	u16 *p = h->p;
	int i, ret, col_ref;

	h->temporal_id = p[P_TEMPORAL_ID];
	h->nal_type = p[P_NAL_UNIT_TYPE];
	h->misc_flag0 = p[P_MISC_FLAG0];

	if (!p[P_FIRST_SLICE_SEGMENT_IN_PIC_FLAG]) {
		h->slice_segment_addr = p[P_SLICE_SEGMENT_ADDRESS];
		if (!p[P_DEPENDENT_SLICE_SEGMENT_FLAG])
			h->slice_addr = h->slice_segment_addr;
	} else {
		h->slice_segment_addr = 0;
		h->slice_addr = 0;
	}
	h->iprev_poc = h->curr_poc;
	h->slice_type = p[P_SLICE_TYPE];
	h->tmvp = p[P_SLICE_TEMPORAL_MVP_ENABLE_FLAG] & 1;
	h->is_next_slice_segment = !!p[P_DEPENDENT_SLICE_SEGMENT_FLAG];

	if (p[P_PIC_WIDTH] < H265_MIN_DIM || p[P_PIC_HEIGHT] < H265_MIN_DIM ||
	    p[P_PIC_WIDTH] > H265_MAX_DIM || p[P_PIC_HEIGHT] > H265_MAX_DIM) {
		dev_err(hw->dev, "hevc: implausible picture size %ux%u\n",
			p[P_PIC_WIDTH], p[P_PIC_HEIGHT]);
		return -EINVAL;
	}
	/*
	 * bit_depth: luma - 8 in bits 3:0, chroma - 8 in bits 7:4.  10 bit is
	 * decoded in full - the compressed reference keeps all ten bits - and
	 * the double-write copy the client gets is 8 bit, which is all this
	 * output path can carry.
	 */
	if ((p[P_BIT_DEPTH] & 0xff) != 0 && (p[P_BIT_DEPTH] & 0xff) != 0x22) {
		dev_err_once(hw->dev, "hevc: bit depth %04x not supported (8 or 10 only)\n",
			     p[P_BIT_DEPTH]);
		return -EINVAL;
	}
	h->ten_bit = (p[P_BIT_DEPTH] & 0xff) == 0x22;
	h->pic_w = p[P_PIC_WIDTH];
	h->pic_h = p[P_PIC_HEIGHT];

	h->lcu_size = 1 << (p[P_LOG2_MIN_CB_SIZE_MINUS3] + 3 +
			    p[P_LOG2_DIFF_MAX_MIN_CB_SIZE]);
	if (h->lcu_size < 16 || h->lcu_size > 64)
		return -EINVAL;
	h->lcu_size_log2 = ilog2(h->lcu_size);
	h->lcu_x_num = DIV_ROUND_UP(h->pic_w, h->lcu_size);
	h->lcu_y_num = DIV_ROUND_UP(h->pic_h, h->lcu_size);
	h->lcu_total = h->lcu_x_num * h->lcu_y_num;

	/* POC, 8.3.1 */
	if (h->nal_type == H265_NAL_IDR_W_RADL || h->nal_type == H265_NAL_IDR_N_LP) {
		h->curr_poc = 0;
		if (h->temporal_id - 1 == 0)
			h->prev_tid0_poc = h->curr_poc;
	} else {
		int max_lsb = 1 << (p[P_LOG2_MAX_POC_LSB_MINUS4] + 4);
		int prev_lsb = h->prev_tid0_poc % max_lsb;
		int prev_msb = h->prev_tid0_poc - prev_lsb;
		int lsb = p[P_POC_LSB];
		int msb;

		if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2)
			msb = prev_msb + max_lsb;
		else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2)
			msb = prev_msb - max_lsb;
		else
			msb = prev_msb;
		if (h->nal_type == H265_NAL_BLA_W_LP ||
		    h->nal_type == H265_NAL_BLA_W_RADL ||
		    h->nal_type == H265_NAL_BLA_N_LP)
			msb = 0;
		h->curr_poc = msb + lsb;
		if (h->temporal_id - 1 == 0)
			h->prev_tid0_poc = h->curr_poc;
	}

	/* skip RASL pictures after the CRA/BLA the stream starts on */
	if (h->poc_random_access == H265_MAX_INT) {
		if (h->nal_type == H265_NAL_CRA || h->nal_type == H265_NAL_BLA_W_LP ||
		    h->nal_type == H265_NAL_BLA_W_RADL || h->nal_type == H265_NAL_BLA_N_LP)
			h->poc_random_access = h->curr_poc;
		else
			h->poc_random_access = -H265_MAX_INT;
	} else if (h->nal_type == H265_NAL_BLA_W_LP ||
		   h->nal_type == H265_NAL_BLA_W_RADL ||
		   h->nal_type == H265_NAL_BLA_N_LP) {
		h->poc_random_access = h->curr_poc;
	} else if (h->curr_poc < h->poc_random_access &&
		   (h->nal_type == H265_NAL_RASL_N || h->nal_type == H265_NAL_RASL_R)) {
		return 1;
	}

	wr(hw, HEVC_WAIT_FLAG, read_dos_reg(hw, HEVC_WAIT_FLAG) | 0x2);

	if (p[P_SLICE_SEGMENT_ADDRESS] == 0) {
		int sao_mem_unit = (h->lcu_size == 16 ? 9 : h->lcu_size == 32 ? 14 : 24) << 4;
		int w_cu = h->lcu_x_num, h_cu = h->lcu_y_num;
		int slot = -1;

		h->new_pic = true;
		h265_apply_rps(h);

		/*
		 * A slot per picture the DPB can hold, plus the current one and
		 * one spare.  Allocating the worst case (16 + 2) regardless costs
		 * 18 x 13 MB at 4K, which with the capture pool does not fit in
		 * CMA; a 4K stream's DPB is 6 or so.
		 */
		h->nslots = clamp_t(int, p[P_SPS_MAX_DEC_PIC_BUFFERING_MINUS1_0] + 3,
				    4, H265_SLOTS);
		ret = h265_alloc_pics(h);
		if (ret)
			return ret;
		for (i = 0; i < h->nslots; i++) {
			if (!h->pic[i].valid || !h->pic[i].referenced) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			dev_err(hw->dev, "hevc: no free picture buffer\n");
			return -ENOBUFS;
		}
		h->cur = slot;
		h->pic[slot].valid = true;
		h->pic[slot].referenced = true;
		h->pic[slot].long_term = false;
		h->pic[slot].poc = h->curr_poc;
		h->pic[slot].width = h->pic_w;
		h->pic[slot].height = h->pic_h;
		h->pic[slot].decode_idx = h->decode_idx++;
		h->pic[slot].slice_type = h->slice_type;

		h265_config_comp(h);
		h265_fill_mmu_map(h);
		wr(hw, HEVC_ASSIST_MMU_MAP_ADDR, h->mmu_map);
		wr(hw, HEVC_SAO_MMU_DMA_CTRL, h->mmu_map);

		h265_update_tiles(h, w_cu, h_cu, sao_mem_unit);
		wr(hw, H265_SAO_MEM_UNIT, sao_mem_unit);
		wr(hw, H265_SAO_ABV, h->sao_abv);
		wr(hw, H265_SAO_VB_SIZE, (sao_mem_unit + (2 << 4)) * h_cu);
		wr(hw, H265_SAO_VB, h->sao_vb);
	} else {
		if (h->cur < 0)
			return 1;	/* a picture's first slice went missing */
		h->new_pic = false;
	}

	if (h->iprev_poc != h->curr_poc) {
		h->new_tile = true;
		h->tile_x = 0;
		h->tile_y = 0;
	} else if (h->tile_enabled) {
		int t = h265_tile_index(h, p[P_SLICE_SEGMENT_ADDRESS], h->lcu_x_num);

		if (t != -1 && t != (h->tile_x | (h->tile_y << 8))) {
			h->new_tile = true;
			h->tile_x = t & 0xff;
			h->tile_y = (t >> 8) & 0xff;
		} else {
			h->new_tile = false;
		}
	} else {
		h->new_tile = false;
	}
	if (h->tile_x >= H265_MAX_TILE_COLS || h->tile_y >= H265_MAX_TILE_ROWS)
		h->new_tile = false;
	if (h->new_tile) {
		struct h265_tile *t = &h->tile[h->tile_y][h->tile_x];

		h->tile_start_lcu_x = t->start_cu_x;
		h->tile_start_lcu_y = t->start_cu_y;
		h->tile_width_lcu = t->width;
		h->tile_height_lcu = t->height;
	}

	h265_set_ref_lists(h);
	col_ref = p[P_COLLOCATED_REF_IDX];
	h->ldc = false;
	if (h->slice_type != H265_I_SLICE) {
		h->ldc = true;
		for (i = 0; i < h->ref_num_l0 && h->ldc; i++)
			if (h->ref_poc_l0[i] > h->curr_poc)
				h->ldc = false;
		if (h->slice_type == H265_B_SLICE)
			for (i = 0; i < h->ref_num_l1 && h->ldc; i++)
				if (h->ref_poc_l1[i] > h->curr_poc)
					h->ldc = false;
	}
	h->col_from_l0 = p[P_COLLOCATED_FROM_L0_FLAG] & 1;
	h->plevel = p[P_LOG2_PARALLEL_MERGE_LEVEL];
	h->max_merge_cand = 5 - p[P_FIVE_MINUS_MAX_NUM_MERGE_CAND];

	if (h->slice_type == H265_B_SLICE && !h->col_from_l0)
		h->col_poc = col_ref < h->ref_num_l1 ? h->ref_poc_l1[col_ref] : H265_INVALID_POC;
	else
		h->col_poc = col_ref < h->ref_num_l0 ? h->ref_poc_l0[col_ref] : H265_INVALID_POC;

	h->col = h->cur;
	if (h->slice_type != H265_I_SLICE && h->col_poc != (int)H265_INVALID_POC) {
		int c = h265_pic_by_poc(h, h->col_poc, true);

		if (c >= 0)
			h->col = c;
		else
			dev_dbg(hw->dev, "hevc: collocated poc %d missing\n", h->col_poc);
	}

	if (!h->tile_width_lcu || !h->tile_height_lcu)
		return -EINVAL;

	h265_config_mc_buffer(h);
	h265_config_mcrcc(h);
	h265_config_mpred(h);
	h265_config_sao(h);

	dev_dbg(hw->dev,
		"hevc: slice poc %d type %d nal %d addr %u new %d slot %d refs %d/%d col %d\n",
		h->curr_poc, h->slice_type, h->nal_type, p[P_SLICE_SEGMENT_ADDRESS],
		h->new_pic, h->cur, h->ref_num_l0, h->ref_num_l1, h->col_poc);
	return 0;
}

/* ---------------------------------------------------------------- */
/* V4L2 glue                                                        */
/* ---------------------------------------------------------------- */

static void h265_set_dw_target(struct aml_hevc_ctx *h)
{
	struct aml_vdec_ctx *ctx = h->v4l2_ctx;
	struct v4l2_pix_format_mplane *pf = &ctx->pix_fmt[AML_FMT_DST];
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	struct vb2_buffer *vb;

	h->dw_target_y = 0;
	h->dw_target_c = 0;
	h->dw_target_size = 0;
	if (!dst || pf->plane_fmt[0].bytesperline != pf->width)
		return;
	vb = &dst->vb2_buf;
	h->dw_target_y = vb2_dma_contig_plane_dma_addr(vb, 0);
	if (!h->dw_target_y)
		return;
	if (vb->num_planes >= 2) {
		h->dw_target_c = vb2_dma_contig_plane_dma_addr(vb, 1);
		h->dw_target_size = vb2_plane_size(vb, 0) + vb2_plane_size(vb, 1);
		if (!h->dw_target_c)
			h->dw_target_y = 0;
	} else {
		h->dw_target_c = h->dw_target_y + (dma_addr_t)pf->width * pf->height;
		h->dw_target_size = vb2_plane_size(vb, 0);
	}
}

static void h265_copy_out(struct aml_hevc_ctx *h)
{
	struct aml_vdec_ctx *ctx = h->v4l2_ctx;
	struct v4l2_pix_format_mplane *pf = &ctx->pix_fmt[AML_FMT_DST];
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	struct h265_pic *pic;
	struct vb2_buffer *vb;
	u32 stride, dst_h, row, w, ht;
	u8 *y, *c;
	const u8 *src;

	if (!dst || h->cur < 0)
		return;
	vb = &dst->vb2_buf;
	pic = &h->pic[h->cur];
	stride = pf->plane_fmt[0].bytesperline ? : pic->width;
	dst_h = pf->height ? : pic->height;

	if (!h->dw_target_y) {
		src = pic->dw_v;
		if (!src)
			return;
		dma_sync_single_for_cpu(h->hw->dev, pic->dw_p, h->dw_size, DMA_FROM_DEVICE);
		w = pic->width;
		ht = pic->height;
		y = vb2_plane_vaddr(vb, 0);
		if (!y)
			return;
		c = vb->num_planes >= 2 ? vb2_plane_vaddr(vb, 1) : y + (size_t)stride * dst_h;
		if (!c)
			return;
		for (row = 0; row < ht; row++)
			memcpy(y + (size_t)row * stride, src + (size_t)row * w, w);
		for (row = 0; row < ht / 2; row++)
			memcpy(c + (size_t)row * stride, src + (size_t)(ht + row) * w, w);
		aml_vdec_dst_cpu_written(h->hw->dev, vb);
	}
	if (vb->num_planes >= 2) {
		vb2_set_plane_payload(vb, 0, stride * dst_h);
		vb2_set_plane_payload(vb, 1, stride * dst_h / 2);
	} else {
		vb2_set_plane_payload(vb, 0, stride * dst_h * 3 / 2);
	}
	h->have_picture = true;
}

static void h265_finish(struct aml_hevc_ctx *h, enum vb2_buffer_state state)
{
	struct aml_vdec_ctx *ctx = h->v4l2_ctx;
	struct vb2_v4l2_buffer *buf;
	unsigned int i;

	if (h->finished)
		return;
	h->finished = true;

	if (!h->have_picture) {
		buf = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
		if (buf)
			for (i = 0; i < buf->vb2_buf.num_planes; i++)
				vb2_set_plane_payload(&buf->vb2_buf, i, 0);
	}
	buf = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	if (buf && buf->vb2_buf.req_obj.req)
		v4l2_ctrl_request_complete(buf->vb2_buf.req_obj.req, &ctx->ctrl_handler);

	ctx->int_cond = 1;
	ctx->job_finished = true;
	v4l2_m2m_buf_done_and_job_finish(ctx->dev->m2m_dev_dec, ctx->m2m_ctx, state);
	wake_up(&ctx->queue);
}

static void h265_command(struct aml_vdec_hw *hw, u32 cmd)
{
	wr(hw, HEVC_ASSIST_MBOX0_CLR_REG, 1);
	wr(hw, HEVC_DEC_STATUS_REG, cmd);
	wr(hw, HEVC_MCPU_INTR_REQ, AMRISC_MAIN_REQ);
}

static void h265_picture_done(struct aml_hevc_ctx *h)
{
	u32 info = read_dos_reg(h->hw, H265_DECODE_INFO);

	h->start_decoding_flag |= info & 0xff;
	h->rps_set_id = (info >> 8) & 0xff;
	if (h->cur >= 0)
		h265_copy_out(h);
	h->cur = -1;
}

static irqreturn_t h265_irq_thread(int irq, void *priv)
{
	struct aml_hevc_ctx *h = priv;
	struct aml_vdec_hw *hw = h->hw;
	u32 st = hw->hevc_irq_status & 0xff;

	if (h->finished)
		return IRQ_HANDLED;
	if (++h->rounds > 4096) {
		dev_err(hw->dev, "hevc: runaway decode, giving up\n");
		h265_finish(h, VB2_BUF_STATE_ERROR);
		return IRQ_HANDLED;
	}

	switch (st) {
	case H265_NAL_SEARCH_DONE: {
		int nal = read_dos_reg(hw, H265_CUR_NAL_UNIT_TYPE);
		u32 parse = H265_DISCARD_NAL;

		if (nal == H265_NAL_EOS) {
			h->curr_poc = H265_INVALID_POC;
			h->poc_random_access = H265_MAX_INT;
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
		if (ret < 0) {
			dev_err(hw->dev, "hevc: slice setup failed (%d)\n", ret);
			h265_finish(h, VB2_BUF_STATE_ERROR);
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
	case H265_DECODE_BUFEMPTY:
	case H265_DECODE_BUFEMPTY2:
	case H265_SEARCH_BUFEMPTY:
	case H265_NAL_DECODE_DONE:
	case H265_FIND_NEXT_PIC_NAL:
		h265_picture_done(h);
		h265_finish(h, VB2_BUF_STATE_DONE);
		return IRQ_HANDLED;
	case H265_DECPIC_DATA_ERROR:
	case H265_DECODE_OVER_SIZE:
	case H265_DECODE_TIMEOUT:
		dev_err(hw->dev, "hevc: decoder reported %02x\n", st);
		h->cur = -1;
		h265_finish(h, VB2_BUF_STATE_ERROR);
		return IRQ_HANDLED;
	default:
		dev_dbg(hw->dev, "hevc: unhandled status %02x\n", st);
		h265_command(hw, H265_ACTION_DONE);
		return IRQ_HANDLED;
	}
}

int aml_hevc_init(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_vdec_hw *hw = vdec_get_hw(ctx->dev);
	struct aml_hevc_ctx *h;

	if (!hw)
		return -ENODEV;
	h = kzalloc_obj(*h);
	if (!h)
		return -ENOMEM;
	h->v4l2_ctx = ctx;
	h->hw = hw;
	h->cur = -1;
	h->curr_poc = H265_INVALID_POC;
	h->poc_random_access = H265_MAX_INT;

	/* One client of the HEVC core at a time - see aml_vp9_init(). */
	if (cmpxchg(&hw->hevc_priv, NULL, h)) {
		dev_info(&ctx->dev->plat_dev->dev,
			 "hevc: HEVC core already in use, refusing second instance\n");
		kfree(h);
		return -EBUSY;
	}
	ctx->codec_priv = h;
	hw->hevc_irq_thread_fn = h265_irq_thread;
	return 0;
}

void aml_hevc_exit(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_hevc_ctx *h = ctx->codec_priv;

	if (!h)
		return;
	h->hw->hevc_irq_thread_fn = NULL;
	h->hw->hevc_priv = NULL;
	if (h->started)
		regmap_write(h->hw->map[DOS_BUS], HEVC_MPSR, 0);
	h265_free(h);
	kfree(h);
	ctx->codec_priv = NULL;
}

int aml_hevc_dec_run(void *priv)
{
	struct aml_vdec_ctx *ctx = priv;
	struct aml_hevc_ctx *h = ctx->codec_priv;
	struct aml_vdec_hw *hw = h->hw;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	struct vb2_buffer *vb;
	int ret;

	if (!src)
		return -EINVAL;
	vb = &src->vb2_buf;

	ret = hvdec_enable(hw);
	if (ret)
		return ret;
	dos_enable_hevc(hw);

	if (!h->ucode_loaded) {
		hvdec_switch_codec(hw, CODEC_TYPE_HEVC);
		ret = hevc_load_fw(hw, CODEC_TYPE_HEVC, H265_FW_FORMAT_MMU,
				   H265_FW_CORE_HEVC, "hevc");
		if (ret)
			return ret;
		h->ucode_loaded = true;
	}

	ret = h265_alloc_workspace(h);
	if (ret)
		return ret;

	if (h->started) {
		wr(hw, HEVC_MPSR, 0);
		wr(hw, HEVC_CPSR, 0);
	}
	aml_vp9_reset_core(hw);
	h265_prot_init(h);
	h265_set_dw_target(h);
	if (h->dw_target_y && (size_t)h->alloc_w * h->alloc_h * 3 / 2 > h->dw_target_size &&
	    h->alloc_w) {
		h->dw_target_y = 0;	/* stream outgrew the capture buffer */
	}

	ret = h265_feed(h, vb2_plane_vaddr(vb, 0), vb2_get_plane_payload(vb, 0));
	if (ret)
		return ret;

	h->rounds = 0;
	h->have_picture = false;
	h->finished = false;
	h->cur = -1;
	ctx->int_cond = 0;

	wr(hw, DOS_SW_RESET3, BIT(12) | BIT(11));
	wr(hw, DOS_SW_RESET3, 0);
	wr(hw, HEVC_ASSIST_MBOX0_CLR_REG, 1);
	wr(hw, HEVC_DEC_STATUS_REG, 0);
	wr(hw, HEVC_CPSR, 0);
	wr(hw, HEVC_MPSR, 1);
	h->started = true;

	/* Uninterruptible on purpose - see aml_vp9_dec_run(). */
	ret = wait_event_timeout(ctx->queue, ctx->int_cond,
				 msecs_to_jiffies(H265_TIMEOUT_MS));
	ctx->int_cond = 0;
	if (!ret) {
		dev_err(hw->dev, "hevc: decode timeout, status %02x pc %08x shift %08x\n",
			hw->hevc_irq_status, read_dos_reg(hw, HEVC_MPC_E),
			read_dos_reg(hw, HEVC_SHIFT_BYTE_COUNT));
		wr(hw, HEVC_MPSR, 0);
		h->cur = -1;
		hw->hevc_dirty = true;
		return -ETIMEDOUT;
	}
	return 0;
}
