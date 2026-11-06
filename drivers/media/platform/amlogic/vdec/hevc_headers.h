/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
#ifndef AML_HEVC_HEADERS_H
#define AML_HEVC_HEADERS_H

#include <linux/types.h>

struct h265_output_limits {
	u32 max_dec_pic_buffering;
	u32 max_num_reorder_pics;
	u32 max_latency_increase_plus1;
};

struct h265_sps_prefix {
	bool valid;
	struct h265_output_limits limits;
};

struct h265_pps_prefix {
	bool valid;
	u8 sps_id;
	u8 extra_slice_header_bits;
	bool output_flag_present;
};

struct h265_headers {
	struct h265_sps_prefix sps[16];
	struct h265_pps_prefix pps[64];
};

struct h265_picture_header {
	u8 nal_type, temporal_id, pps_id;
	bool no_output_of_prior_pics;
	bool pic_output;
	struct h265_output_limits limits;
};

struct h265_au_info {
	bool has_picture;
	bool cvs_boundary_before;
	bool cvs_boundary_after;
	struct h265_picture_header picture;
};

int h265_headers_parse_au(struct h265_headers *headers, const u8 *data, size_t size,
			  struct h265_au_info *au);

#endif
