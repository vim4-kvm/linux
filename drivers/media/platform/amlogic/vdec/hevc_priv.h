/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
#ifndef AML_HEVC_PRIV_H
#define AML_HEVC_PRIV_H

#include <linux/types.h>
#include <linux/videodev2.h>

#include "hevc.h"

struct h265_buffer {
	void *body_v, *hdr_v, *mv_v;
	dma_addr_t body_p, hdr_p, mv_p;
};

struct aml_hevc_ctx {
	struct aml_vdec_ctx *v4l2_ctx;
	struct aml_vdec_hw *hw;
	struct aml_vdec_stateful_ctx *stateful;
	bool configured;
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
	struct h265_dpb dpb;
	struct h265_buffer buffers[H265_SLOTS];
	struct vb2_v4l2_buffer *capture[H265_SLOTS];
	size_t body_size, hdr_size, mv_size;
	u32 alloc_w, alloc_h;
	/* picture slots in use: the stream's DPB size plus two, at most H265_SLOTS */
	int alloc_nslots;
	bool alloc_ten_bit;
	dma_addr_t dw_target_y, dw_target_c;

	/* stream state kept across access units */
	u32 start_decoding_flag;
	u32 rps_set_id;
	bool have_vps, have_sps, have_pps, have_start;

};

int h265_hw_alloc_workspace(struct aml_hevc_ctx *h);
int h265_hw_alloc_pictures(struct aml_hevc_ctx *h);
void h265_hw_free(struct aml_hevc_ctx *h);
int h265_hw_run(struct aml_hevc_ctx *h, const void *data, size_t size);

size_t h265_mv_buf_size(u32 w, u32 h);
void h265_capture_format(struct v4l2_pix_format_mplane *fmt, u32 width, u32 height);

#endif
