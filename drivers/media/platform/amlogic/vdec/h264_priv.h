/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */
#ifndef AML_H264_PRIV_H
#define AML_H264_PRIV_H

#include <kunit/visibility.h>
#include <linux/types.h>
#include <linux/v4l2-controls.h>

#include "h264.h"

#define H264_MAX_COL_BUF 32

/* H264_CO_MB_RD_ADDR[31:30] describes the reference's original layout. */
enum h264_picture_structure {
	H264_PICTURE_TOP,
	H264_PICTURE_BOTTOM,
	H264_PICTURE_FRAME,
	H264_PICTURE_MBAFF,
};

struct vdec_h264_stateless_ctrl_ref {
	const struct v4l2_ctrl_h264_decode_params *decode;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling;
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
};

enum SliceType {
	P_SLICE = 0,
	B_SLICE = 1,
	I_SLICE = 2,
	SP_SLICE = 3,
	SI_SLICE = 4,
	MAX_SLICE_TYPES = 5
};

#define I_Slice                               2
#define P_Slice                               5
#define B_Slice                               6
#define P_Slice_0                             0
#define B_Slice_1                             1
#define I_Slice_7                             7

/* Used by firmware */
union param {
	struct {
		unsigned short data[RPM_END - RPM_BEGIN];
	} l;
	struct {
		unsigned short dump[DPB_OFFSET];
		unsigned short dpb_base[FRAME_IN_DPB << 3];

		unsigned short dpb_max_buffer_frame;
		unsigned short actual_dpb_size;

		unsigned short colocated_buf_status;

		unsigned short num_forward_short_term_reference_pic;
		unsigned short num_short_term_reference_pic;
		unsigned short num_reference_pic;

		unsigned short current_dpb_index;
		unsigned short current_decoded_frame_num;
		unsigned short current_reference_frame_num;

		unsigned short l0_size;
		unsigned short l1_size;
		/*
		 * [6:5] : nal_ref_idc
		 * [4:0] : nal_unit_type
		 */
		unsigned short NAL_info_mmco;
		/*
		 * [1:0] : 00 - top field, 01 - bottom field,
		 *         10 - frame, 11 - mbaff frame
		 */
		unsigned short picture_structure_mmco;
		unsigned short frame_num;
		unsigned short pic_order_cnt_lsb;

		unsigned short num_ref_idx_l0_active_minus1;
		unsigned short num_ref_idx_l1_active_minus1;

		unsigned short PrevPicOrderCntLsb;
		unsigned short PreviousFrameNum;

		/* 32 bits variables */
		unsigned short delta_pic_order_cnt_bottom[2];
		unsigned short delta_pic_order_cnt_0[2];
		unsigned short delta_pic_order_cnt_1[2];

		unsigned short PrevPicOrderCntMsb[2];
		unsigned short PrevFrameNumOffset[2];

		unsigned short frame_pic_order_cnt[2];
		unsigned short top_field_pic_order_cnt[2];
		unsigned short bottom_field_pic_order_cnt[2];

		unsigned short colocated_mv_addr_start[2];
		unsigned short colocated_mv_addr_end[2];
		unsigned short colocated_mv_wr_addr[2];
	} dpb;
	struct {
		unsigned short dump[MMCO_OFFSET];

		/* array base address for offset_for_ref_frame */
		unsigned short offset_for_ref_frame_base[128];

		/*
		 * 0 - Index in DPB
		 * 1 - Picture Flag
		 *  [2] : 0 - short term reference,
		 *			1 - long term reference
		 *  [1] : bottom field
		 *  [0] : top field
		 * 2 - Picture Number (short term or long term) low 16 bits
		 * 3 - Picture Number (short term or long term) high 16 bits
		 */
		unsigned short reference_base[128];

		/* command and parameter, until command is 3 */
		unsigned short l0_reorder_cmd[REORDER_CMD_MAX];
		unsigned short l1_reorder_cmd[REORDER_CMD_MAX];

		/* command and parameter, until command is 0 */
		unsigned short mmco_cmd[44];

		unsigned short l0_base[40];
		unsigned short l1_base[40];
	} mmco;
	struct {
		/* from ucode lmem, do not change this struct */
	} p;
};

struct h264_canvas {
	u64 timestamp;
	enum h264_picture_structure structure;
};

struct h264_decode_buf_spec {
	struct v4l2_h264_dpb_entry *dpb;
	int canvas_pos;
	u32 dpb_index;
	int poc;
	int col_buf_index;
	int y_canvas_index;
	int u_canvas_index;
	int v_canvas_index;
	u8 used;
	u8 long_term_flag;
	u8 fields;
	enum h264_picture_structure structure;
	dma_addr_t y_dma_addr;
	dma_addr_t c_dma_addr;
};

#define REORDERING_COMMAND_MAX_SIZE	33
struct slice {
	int frame_num;
	/*modification */
	int slice_type;
	int num_ref_idx_l0;
	int num_ref_idx_l1;
	int first_mb_in_slice;
	int ref_pic_list_reordering_flag[2];
	int modification_of_pic_nums_idc[2][REORDERING_COMMAND_MAX_SIZE];
	int abs_diff_pic_num_minus1[2][REORDERING_COMMAND_MAX_SIZE];
	int long_term_pic_idx[2][REORDERING_COMMAND_MAX_SIZE];
	unsigned char dec_ref_pic_marking_buffer_valid;
};

struct aml_h264_ctx {
	struct aml_vdec_ctx *v4l2_ctx;
	u8 init_flag;
	u8 new_pic_flag;
	u8 mc_cpu_loaded;
	u8 param_set;
	u8 colocated_buf_num;
	u8 reg_iqidct_control_init_flag;
	u32 reg_iqidct_control;
	u32 reg_vcop_ctrl_reg;
	u32 reg_rv_ai_mb_count;
	u32 vld_dec_control;
	u32 save_avscratch_f;
	u32 seq_info;
	u32 decode_pic_count;
	union param dpb_param;
	u32 dec_status;
	struct slice mslice;
	struct h264_decode_buf_spec curr_spec;
	struct h264_decode_buf_spec ref_list0[V4L2_H264_REF_LIST_LEN + 1];
	struct h264_decode_buf_spec ref_list1[V4L2_H264_REF_LIST_LEN + 1];
	struct h264_decode_buf_spec ref_list0_unreordered[V4L2_H264_REF_LIST_LEN + 1];
	struct h264_decode_buf_spec ref_list1_unreordered[V4L2_H264_REF_LIST_LEN + 1];
	u8 list_size[2];
	u32 canvas_pos_map;
	u32 configured_canvas_map;
	struct h264_canvas ref_canvas[V4L2_H264_NUM_DPB_ENTRIES + 1];
	dma_addr_t lmem_phy_addr;
	void *lmem_addr;
	dma_addr_t mc_cpu_paddr;
	void *mc_cpu_vaddr;
	dma_addr_t cma_alloc_addr;
	void *cma_alloc_vaddr;
	dma_addr_t collated_cma_addr;
	dma_addr_t collated_cma_addr_end;
	void *collated_cma_vaddr;
	dma_addr_t workspace_offset;
	void *workspace_vaddr;
	u32 col_buf_alloc_size;
	u32 one_col_buf_size;
	u32 colocated_buf_map;
	u64 colocated_timestamp[H264_MAX_COL_BUF];
	enum h264_picture_structure colocated_structure[H264_MAX_COL_BUF];

	u32 frame_width;
	u32 frame_height;
	u32 mb_width;
	u32 mb_height;
	u32 mb_total;
	u32 max_num_ref_frames;
	void *input_vaddr;
	dma_addr_t input_dma;
	u32 input_size;
	bool staged_input;

	struct vdec_h264_stateless_ctrl_ref ctrl_ref;
};

#if IS_ENABLED(CONFIG_KUNIT)
void h264_prune_picture_slots(struct aml_h264_ctx *h264, u64 timestamp, bool held);
int h264_get_picture_slots(struct aml_h264_ctx *h264, u64 timestamp);
enum h264_picture_structure
h264_reference_structure(enum h264_picture_structure original, u8 fields);
enum h264_picture_structure
h264_picture_structure(const struct v4l2_ctrl_h264_decode_params *decode,
		       const struct v4l2_ctrl_h264_sps *sps);
int h264_mv_offsets(enum h264_picture_structure current_structure,
		    enum h264_picture_structure reference, u32 first_mb,
		    u32 mb_width, u32 buffer_size, bool compact,
		    u32 *write_offset, u32 *read_offset);
void h264_build_sps_params(struct aml_h264_ctx *h264_ctx,
			   unsigned short *sps_base,
			   const struct v4l2_ctrl_h264_sps *sps);
void h264_build_pps_params(struct aml_h264_ctx *h264_ctx,
			   unsigned short *pps_base,
			   const struct v4l2_ctrl_h264_pps *pps);
int h264_pad_input(void *input, u32 payload_size, u32 buffer_size);
int h264_ref_list_size(struct aml_h264_ctx *h264_ctx, int cur_list);
void h264_copy_ref_list(struct aml_h264_ctx *h264_ctx, int curr_list);
void h264_config_ref_lists(struct aml_vdec_ctx *ctx);
void h264_config_p_reflist(struct aml_h264_ctx *h264_ctx,
			   const struct v4l2_h264_reference *reflist,
			   u32 list_size);
void h264_reorder_short_term(struct slice *curr_slice, int cur_list,
			     int pic_num_lx, int *ref_idx_lx);
#endif

#endif
