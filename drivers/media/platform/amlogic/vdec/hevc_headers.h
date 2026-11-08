/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
#ifndef AML_HEVC_HEADERS_H
#define AML_HEVC_HEADERS_H

#include <linux/types.h>

struct h265_output_limits {
	u32 max_dec_pic_buffering;
	u32 max_num_reorder_pics;
	u32 max_latency_increase_plus1;
};

#define H265_HEADER_REFS 16
#define H265_ST_RPS_SETS 64

struct h265_st_rps {
	u8 num_negative_pics, num_positive_pics, num_delta_pocs;
	u16 used;
	s32 delta_poc[H265_HEADER_REFS];
};

struct h265_sps_prefix {
	bool valid;
	u8 vps_id;
	u8 log2_max_pic_order_cnt_lsb;
	u8 log2_ctb_size;
	u32 width, height;
	bool separate_colour_plane;
	u8 num_short_term_ref_pic_sets;
	struct h265_st_rps st_rps[H265_ST_RPS_SETS];
	bool long_term_ref_pics_present;
	u8 num_long_term_ref_pics_sps;
	u16 lt_poc_lsb[32];
	u32 lt_used;
	struct h265_output_limits limits;
};

struct h265_pps_prefix {
	bool valid;
	u8 sps_id;
	u8 extra_slice_header_bits;
	bool output_flag_present;
	bool entropy_coding_sync_enabled;
};

struct h265_headers {
	struct h265_sps_prefix sps[16];
	struct h265_pps_prefix pps[64];
};

struct h265_lt_ref {
	u16 poc_lsb;
	u8 sps_index;
	bool from_sps, used, msb_present;
	u32 delta_cycle, cycle;
};

struct h265_slice_rps {
	u16 poc_lsb;
	u8 poc_bits;
	bool st_from_sps;
	u8 st_index;
	struct h265_st_rps st;
	u8 num_long_term_sps, num_long_term_pics;
	struct h265_lt_ref lt[H265_HEADER_REFS];
};

struct h265_picture_header {
	u8 nal_type, temporal_id, pps_id;
	bool no_output_of_prior_pics;
	bool pic_output;
	struct h265_output_limits limits;
	struct h265_slice_rps rps;
};

struct h265_au_info {
	bool has_picture;
	bool reorder_parameter_sets;
	bool cvs_boundary_before;
	bool cvs_boundary_after;
	struct h265_picture_header picture;
};

int h265_headers_parse_au(struct h265_headers *headers, const u8 *data, size_t size,
			  struct h265_au_info *au);

/* Copy an AU into a size-byte destination, optionally sorting its prefix PS. */
int h265_headers_copy_au(u8 *dst, const u8 *data, size_t size, bool reorder);

#endif
