// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "hevc_headers.h"

/* Read an RBSP prefix directly from the feed's bytes, without a second mapping. */
struct h265_bits {
	const u8 *data;
	size_t size, pos;
	u8 byte, left, zeros;
	bool error;
};

static u32 h265_u(struct h265_bits *b, unsigned int bits)
{
	u32 value = 0;

	while (bits--) {
		if (!b->left) {
			if (b->zeros == 2 && b->pos < b->size && b->data[b->pos] == 3) {
				b->pos++;
				b->zeros = 0;
			}
			if (b->pos == b->size) {
				b->error = true;
				return 0;
			}
			b->byte = b->data[b->pos++];
			b->zeros = b->byte ? 0 : min(b->zeros + 1, 2);
			b->left = 8;
		}
		value = (value << 1) | ((b->byte >> --b->left) & 1);
	}
	return value;
}

static u32 h265_ue(struct h265_bits *b)
{
	unsigned int zeros = 0;

	while (!h265_u(b, 1) && !b->error) {
		if (++zeros == 32) {
			b->error = true;
			return 0;
		}
	}
	return (BIT(zeros) - 1) + h265_u(b, zeros);
}

static void h265_skip(struct h265_bits *b, unsigned int bits)
{
	while (bits && !b->error) {
		unsigned int n = min(bits, 32U);

		h265_u(b, n);
		bits -= n;
	}
}

static int h265_st_rps(struct h265_bits *b, const struct h265_sps_prefix *sps,
		       unsigned int index, struct h265_st_rps *rps)
{
	u32 i, count;

	memset(rps, 0, sizeof(*rps));
	if (index && h265_u(b, 1)) {
		const struct h265_st_rps *ref;
		u32 back = 1;
		s32 delta;
		bool sign;

		if (index == sps->num_short_term_ref_pic_sets)
			back += h265_ue(b);
		if (!back || back > index)
			return -EINVAL;
		ref = &sps->st_rps[index - back];
		sign = h265_u(b, 1);
		count = h265_ue(b);
		if (count > 32767)
			return -EINVAL;
		delta = sign ? -(s32)(count + 1) : count + 1;
		for (i = 0; i <= ref->num_delta_pocs; i++) {
			bool used = h265_u(b, 1);
			bool retain = used || h265_u(b, 1);
			s32 poc = delta + (i < ref->num_delta_pocs ? ref->delta_poc[i] : 0);
			u32 j, n = rps->num_delta_pocs;

			if (!retain || !poc)
				continue;
			if (n == H265_HEADER_REFS)
				return -EINVAL;
			/* S0 is decreasing negative POC; S1 is increasing positive POC. */
			for (j = 0; j < n; j++) {
				s32 old = rps->delta_poc[j];

				if ((poc < 0 && (old > 0 || poc > old)) ||
				    (poc > 0 && old > 0 && poc < old))
					break;
			}
			memmove(&rps->delta_poc[j + 1], &rps->delta_poc[j],
				(n - j) * sizeof(poc));
			rps->delta_poc[j] = poc;
			rps->used = (rps->used & (BIT(j) - 1)) |
				((rps->used & ~(BIT(j) - 1)) << 1) | (used ? BIT(j) : 0);
			rps->num_delta_pocs++;
			if (poc < 0)
				rps->num_negative_pics++;
			else
				rps->num_positive_pics++;
		}
	} else {
		u32 negative = h265_ue(b), positive = h265_ue(b);
		s32 delta = 0;

		if (negative > H265_HEADER_REFS || positive > H265_HEADER_REFS - negative)
			return -EINVAL;
		rps->num_negative_pics = negative;
		rps->num_positive_pics = positive;
		rps->num_delta_pocs = negative + positive;
		for (i = 0; i < negative + positive; i++) {
			u32 step = h265_ue(b);

			if (step > 32767)
				return -EINVAL;
			if (i == negative)
				delta = 0;
			delta += i < negative ? -(s32)(step + 1) : step + 1;
			rps->delta_poc[i] = delta;
			if (h265_u(b, 1))
				rps->used |= BIT(i);
		}
	}
	return b->error ? -EINVAL : 0;
}

static void h265_scaling_list(struct h265_bits *b)
{
	u32 size, matrix, i;

	for (size = 0; size < 4 && !b->error; size++)
		for (matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
			if (!h265_u(b, 1)) {
				h265_ue(b);
				continue;
			}
			if (size > 1)
				h265_ue(b); /* scaling_list_dc_coef_minus8, se(v) */
			for (i = 0; i < min(64U, 1U << (4 + 2 * size)); i++)
				h265_ue(b); /* scaling_list_delta_coef, se(v) */
		}
}

static int h265_sps_rps(struct h265_sps_prefix *sps, struct h265_bits *b)
{
	u32 min_cb = h265_ue(b), diff_cb = h265_ue(b), count, i;

	if (min_cb > 3 || diff_cb > 3 || min_cb + diff_cb > 3)
		return -EINVAL;
	sps->log2_ctb_size = min_cb + diff_cb + 3;
	for (i = 0; i < 4; i++)
		h265_ue(b); /* transform sizes and hierarchy depths */
	if (h265_u(b, 1) && h265_u(b, 1))
		h265_scaling_list(b);
	h265_skip(b, 2); /* amp_enabled_flag, sample_adaptive_offset_enabled_flag */
	if (h265_u(b, 1)) {
		h265_skip(b, 8); /* pcm_sample_bit_depth_luma/chroma_minus1 */
		h265_ue(b);
		h265_ue(b);
		h265_u(b, 1);
	}
	count = h265_ue(b);
	if (count > H265_ST_RPS_SETS)
		return -EINVAL;
	sps->num_short_term_ref_pic_sets = count;
	for (i = 0; i < count; i++)
		if (h265_st_rps(b, sps, i, &sps->st_rps[i]))
			return -EINVAL;
	sps->long_term_ref_pics_present = h265_u(b, 1);
	if (sps->long_term_ref_pics_present) {
		count = h265_ue(b);
		if (count > ARRAY_SIZE(sps->lt_poc_lsb))
			return -EINVAL;
		sps->num_long_term_ref_pics_sps = count;
		for (i = 0; i < count; i++) {
			sps->lt_poc_lsb[i] = h265_u(b, sps->log2_max_pic_order_cnt_lsb);
			if (h265_u(b, 1))
				sps->lt_used |= BIT(i);
		}
	}
	return b->error ? -EINVAL : 0;
}

static int h265_sps_prefix(struct h265_sps_prefix *sps, struct h265_bits *b, u32 *id)
{
	bool profile[7], level[7];
	u32 chroma, sublayers, i, first, poc_bits;

	sps->vps_id = h265_u(b, 4);
	sublayers = h265_u(b, 3);
	h265_u(b, 1);
	if (sublayers > 6)
		return -EINVAL;
	h265_skip(b, 96);
	for (i = 0; i < sublayers; i++) {
		profile[i] = h265_u(b, 1);
		level[i] = h265_u(b, 1);
	}
	if (sublayers)
		h265_skip(b, 2 * (8 - sublayers));
	for (i = 0; i < sublayers; i++) {
		if (profile[i])
			h265_skip(b, 88);
		if (level[i])
			h265_skip(b, 8);
	}
	*id = h265_ue(b);
	chroma = h265_ue(b);
	if (*id >= 16 || chroma > 3)
		return -EINVAL;
	if (chroma == 3)
		sps->separate_colour_plane = h265_u(b, 1);
	sps->width = h265_ue(b);
	sps->height = h265_ue(b);
	if (h265_u(b, 1))
		for (i = 0; i < 4; i++)
			h265_ue(b);
	h265_ue(b);
	h265_ue(b);
	poc_bits = h265_ue(b);
	if (poc_bits > 12)
		return -EINVAL;
	sps->log2_max_pic_order_cnt_lsb = poc_bits + 4;
	first = h265_u(b, 1) ? 0 : sublayers;
	for (i = first; i <= sublayers; i++) {
		u32 buffering = h265_ue(b);

		sps->limits.max_dec_pic_buffering = buffering + 1;
		sps->limits.max_num_reorder_pics = h265_ue(b);
		sps->limits.max_latency_increase_plus1 = h265_ue(b);
		if (buffering > 15 || sps->limits.max_num_reorder_pics > buffering)
			return -EINVAL;
	}
	return h265_sps_rps(sps, b);
}

static int h265_sps(struct h265_headers *headers, struct h265_bits *b)
{
	struct h265_sps_prefix *sps;
	u32 id;
	int ret;

	sps = kzalloc_obj(*sps);
	if (!sps)
		return -ENOMEM;
	ret = h265_sps_prefix(sps, b, &id);
	if (!ret) {
		sps->valid = true;
		headers->sps[id] = *sps;
	}
	kfree(sps);
	return ret;
}

static int h265_pps(struct h265_headers *headers, struct h265_bits *b)
{
	struct h265_pps_prefix pps = { .valid = true };
	u32 id = h265_ue(b), sps = h265_ue(b);

	if (id >= ARRAY_SIZE(headers->pps) || sps >= ARRAY_SIZE(headers->sps))
		return -EINVAL;
	pps.sps_id = sps;
	h265_u(b, 1); /* dependent_slice_segments_enabled_flag */
	pps.output_flag_present = h265_u(b, 1);
	pps.extra_slice_header_bits = h265_u(b, 3);
	h265_skip(b, 2); /* sign_data_hiding_enabled_flag, cabac_init_present_flag */
	h265_ue(b); /* num_ref_idx_l0_default_active_minus1 */
	h265_ue(b); /* num_ref_idx_l1_default_active_minus1 */
	h265_ue(b); /* init_qp_minus26, se(v) */
	h265_skip(b, 2); /* constrained_intra_pred_flag, transform_skip_enabled_flag */
	if (h265_u(b, 1))
		h265_ue(b); /* diff_cu_qp_delta_depth */
	h265_ue(b); /* pps_cb_qp_offset, se(v) */
	h265_ue(b); /* pps_cr_qp_offset, se(v) */
	h265_skip(b, 5); /* chroma offsets, weighted pred/bipred, transquant, tiles */
	pps.entropy_coding_sync_enabled = h265_u(b, 1);
	if (b->error)
		return -EINVAL;
	headers->pps[id] = pps;
	return 0;
}

static int h265_slice_rps(struct h265_bits *b, const struct h265_sps_prefix *sps,
			  struct h265_slice_rps *rps)
{
	u32 i, count, n_sps = 0, cycle = 0;

	rps->poc_bits = sps->log2_max_pic_order_cnt_lsb;
	rps->poc_lsb = h265_u(b, rps->poc_bits);
	rps->st_from_sps = h265_u(b, 1);
	count = sps->num_short_term_ref_pic_sets;
	if (rps->st_from_sps) {
		u32 index = count > 1 ? h265_u(b, fls(count - 1)) : 0;

		if (!count || index >= count)
			return -EINVAL;
		rps->st_index = index;
		rps->st = sps->st_rps[index];
	} else if (h265_st_rps(b, sps, count, &rps->st)) {
		return -EINVAL;
	}
	if (!sps->long_term_ref_pics_present)
		return b->error ? -EINVAL : 0;
	if (sps->num_long_term_ref_pics_sps)
		n_sps = h265_ue(b);
	count = h265_ue(b);
	if (n_sps > sps->num_long_term_ref_pics_sps || n_sps > H265_HEADER_REFS ||
	    count > H265_HEADER_REFS - n_sps)
		return -EINVAL;
	rps->num_long_term_sps = n_sps;
	rps->num_long_term_pics = count;
	for (i = 0; i < n_sps + count; i++) {
		struct h265_lt_ref *lt = &rps->lt[i];

		lt->from_sps = i < n_sps;
		if (lt->from_sps) {
			u32 n = sps->num_long_term_ref_pics_sps;
			u32 index = n > 1 ? h265_u(b, fls(n - 1)) : 0;

			if (index >= n)
				return -EINVAL;
			lt->sps_index = index;
			lt->poc_lsb = sps->lt_poc_lsb[index];
			lt->used = sps->lt_used & BIT(index);
		} else {
			lt->poc_lsb = h265_u(b, rps->poc_bits);
			lt->used = h265_u(b, 1);
		}
		lt->msb_present = h265_u(b, 1);
		if (lt->msb_present)
			lt->delta_cycle = h265_ue(b);
		/* H.265 7-52 starts a new accumulation at each source group. */
		if (!i || i == n_sps)
			cycle = 0;
		if (lt->delta_cycle > U32_MAX - cycle)
			return -EINVAL;
		cycle += lt->delta_cycle;
		lt->cycle = cycle;
	}
	return b->error ? -EINVAL : 0;
}

static int h265_slice(const struct h265_headers *headers, struct h265_bits *b,
		      u8 type, u8 temporal_id, struct h265_au_info *au)
{
	struct h265_picture_header pic = { .nal_type = type, .temporal_id = temporal_id,
		.pic_output = true };
	const struct h265_pps_prefix *pps;
	const struct h265_sps_prefix *sps;
	u32 id;

	if (!h265_u(b, 1))
		return b->error ? -EINVAL : 0;
	if (au->has_picture)
		return -EINVAL;
	if (type >= 16 && type <= 23)
		pic.no_output_of_prior_pics = h265_u(b, 1);
	id = h265_ue(b);
	if (id >= ARRAY_SIZE(headers->pps) || !headers->pps[id].valid)
		return -EINVAL;
	pps = &headers->pps[id];
	if (!headers->sps[pps->sps_id].valid)
		return -EINVAL;
	sps = &headers->sps[pps->sps_id];
	pic.pps_id = id;
	pic.limits = headers->sps[pps->sps_id].limits;
	h265_skip(b, pps->extra_slice_header_bits);
	if (h265_ue(b) > 2)
		return -EINVAL;
	if (pps->output_flag_present)
		pic.pic_output = h265_u(b, 1);
	if (sps->separate_colour_plane)
		h265_u(b, 2);
	if (type != 19 && type != 20 && h265_slice_rps(b, sps, &pic.rps))
		return -EINVAL;
	if (b->error)
		return -EINVAL;
	au->picture = pic;
	au->has_picture = true;
	return 0;
}

static size_t h265_start_code(const u8 *data, size_t size, size_t from)
{
	size_t i;

	for (i = from; i + 2 < size; i++)
		if (!data[i] && !data[i + 1] && data[i + 2] == 1)
			return i;
	return size;
}

int h265_headers_parse_au(struct h265_headers *headers, const u8 *data, size_t size,
			  struct h265_au_info *au)
{
	size_t start, next;

	memset(au, 0, sizeof(*au));
	if (!data || !size)
		return -EINVAL;
	start = h265_start_code(data, size, 0);
	if (start == size)
		return -EINVAL;
	while (start < size) {
		size_t pos = start + 3;
		struct h265_bits bits;
		u8 type;
		int ret = 0;

		next = h265_start_code(data, size, pos);
		if (next - pos < 2 || data[pos] & 0x80 ||
		    !(data[pos + 1] & 7))
			return -EINVAL;
		if (data[pos] & 1 || data[pos + 1] & 0xf8) {
			start = next;
			continue;
		}
		type = (data[pos] >> 1) & 63;
		bits = (struct h265_bits) { .data = data + pos + 2, .size = next - pos - 2 };
		if (type == 33) {
			ret = h265_sps(headers, &bits);
		} else if (type == 34) {
			ret = h265_pps(headers, &bits);
		} else if (type <= 9 || (type >= 16 && type <= 21)) {
			ret = h265_slice(headers, &bits, type, data[pos + 1] & 7, au);
		} else if (type == 36 || type == 37) {
			if (au->has_picture)
				au->cvs_boundary_after = true;
			else
				au->cvs_boundary_before = true;
		}
		if (ret)
			return ret;
		start = next;
	}
	return 0;
}
