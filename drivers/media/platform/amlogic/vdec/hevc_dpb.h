/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
#ifndef AML_HEVC_DPB_H
#define AML_HEVC_DPB_H

#include <linux/types.h>

/* NAL unit types, H.265 table 7-1. */
#define H265_NAL_IDR_W_RADL		19
#define H265_NAL_IDR_N_LP		20
#define H265_NAL_CRA			21
#define H265_NAL_BLA_W_LP		16
#define H265_NAL_BLA_W_RADL		17
#define H265_NAL_BLA_N_LP		18
#define H265_NAL_RASL_N			8
#define H265_NAL_RASL_R			9
#define H265_NAL_VPS			32
#define H265_NAL_SPS			33
#define H265_NAL_PPS			34
#define H265_NAL_EOS			36

/* The microcode's slice_type: B, P, I. */
#define H265_B_SLICE			0
#define H265_P_SLICE			1
#define H265_I_SLICE			2

#define H265_SAO_ABV_BUF_SIZE	0x30000
#define H265_SAO_VB_BUF_SIZE	0x30000

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
	bool output_pending;
	u32 epoch;
	unsigned int reorder_limit;
};

struct h265_tile {
	int start_cu_x, start_cu_y, width, height;
	u32 sao_vb, sao_abv;
};

struct h265_rps_ref {
	int slot;		/* -1 when absent from the snapshot */
	bool used;
};

struct h265_rps_set {
	struct h265_rps_ref ref[H265_MAX_REF];
	unsigned int count;
};

struct h265_resolved_rps {
	u16 words[H265_MAX_REF];
	struct h265_rps_set st_before, st_after, lt_curr, lt_foll;
};

struct h265_dpb {
	u32 epoch;
	unsigned int pending, reorder_limit;
	struct h265_pic pic[H265_SLOTS];
	int nslots;
	bool ten_bit;
	int prev_tid0_poc;
	int poc_random_access;
	u32 decode_idx;
	/* picture state */
	u16 p[P_RPM_COUNT];
	struct h265_resolved_rps rps;
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

int h265_pic_by_poc(struct h265_dpb *h, int poc, bool ref_only);
int h265_resolve_rps(const struct h265_dpb *h, struct h265_resolved_rps *resolved);
bool h265_rps_matches(const struct h265_resolved_rps *resolved, const u16 *words);
void h265_apply_rps(struct h265_dpb *h, const struct h265_resolved_rps *resolved);
int h265_set_ref_lists(struct h265_dpb *h, const struct h265_resolved_rps *resolved);
void h265_update_tiles(struct h265_dpb *h, int w_cu, int h_cu,
		       int sao_mem_unit);
int h265_tile_index(struct h265_dpb *h, int cu_adr, int w_lcu);
int h265_validate_params(const u16 *p);
void h265_update_poc(struct h265_dpb *h);
bool h265_update_random_access(struct h265_dpb *h);
int h265_free_slot(struct h265_dpb *h);
int h265_next_output(struct h265_dpb *h, bool flush);
void h265_queue_output(struct h265_dpb *h, unsigned int slot);
void h265_remove_output(struct h265_dpb *h, unsigned int slot);

#endif
