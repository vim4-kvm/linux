/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
#ifndef AML_HEVC_PRIV_H
#define AML_HEVC_PRIV_H

#include <kunit/visibility.h>
#include <linux/types.h>
#include <linux/videodev2.h>

#include "hevc.h"

#define H265_SLOTS		18
#define H265_MAX_REF		16
#define H265_MAX_TILE_COLS	10
#define H265_MAX_TILE_ROWS	20
#define H265_MIN_DIM		16
#define H265_MAX_DIM		4096
#define H265_MAX_HEIGHT		2304

#define H265_INVALID_POC	0x80000000
#define H265_MAX_INT		0x7fffffff

/* CUR_RPS entries, SUPPORT_LONG_TERM_RPS layout. */
#define RPS_END			0x8000
#define RPS_LT_BIT		14
#define RPS_USED_BIT		13

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

struct h265_pic {
	bool valid;		/* holds a decoded or decoding picture */
	bool referenced;
	bool long_term;
	int poc;
	u32 decode_idx;
	u32 width, height;
	int slice_type;
	struct vb2_v4l2_buffer *output;
	u32 epoch;

	void *body_v, *hdr_v, *mv_v;
	dma_addr_t body_p, hdr_p, mv_p;
};

struct h265_tile {
	int start_cu_x, start_cu_y, width, height;
	dma_addr_t sao_vb, sao_abv;
};

struct aml_hevc_ctx {
	struct aml_vdec_ctx *v4l2_ctx;
	struct aml_vdec_hw *hw;
	bool configured;
	bool format_known;
	u32 epoch, sequence;
	unsigned int pending, reorder_limit;
	struct v4l2_pix_format_mplane format;
	struct v4l2_rect crop;
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
	size_t body_size, hdr_size, mv_size;
	u32 alloc_w, alloc_h;
	/* picture slots in use: the stream's DPB size plus two, at most H265_SLOTS */
	int nslots, alloc_nslots;
	bool ten_bit, alloc_ten_bit;
	dma_addr_t dw_target_y, dw_target_c;

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

#if IS_ENABLED(CONFIG_KUNIT)
size_t h265_mv_buf_size(u32 w, u32 h);
int h265_validate_params(const u16 *p);
void h265_update_tiles(struct aml_hevc_ctx *h, int w_cu, int h_cu,
		       int sao_mem_unit);
int h265_set_ref_lists(struct aml_hevc_ctx *h);
void h265_update_poc(struct aml_hevc_ctx *h);
void h265_capture_format(struct v4l2_pix_format_mplane *fmt, u32 width, u32 height);
#endif

#endif
