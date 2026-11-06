// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "hevc_dpb.h"

int h265_pic_by_poc(struct h265_dpb *h, int poc, bool ref_only)
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

static bool h265_reference_candidate(const struct h265_dpb *h, int slot)
{
	const struct h265_pic *pic = &h->pic[slot];

	/* cur may still name the preceding picture before slot allocation. */
	return pic->valid && pic->referenced && pic->poc != h->curr_poc &&
		pic->width == h->pic_w && pic->height == h->pic_h;
}

static int h265_rps_poc(int curr_poc, u16 entry)
{
	int delta = entry & (BIT(RPS_USED_BIT - 1) - 1);

	if (entry & BIT(RPS_USED_BIT - 1))
		delta -= BIT(RPS_USED_BIT - 1);
	return curr_poc + delta;
}

static void h265_rps_add(struct h265_rps_set *set, int slot, bool used)
{
	set->ref[set->count++] = (struct h265_rps_ref) { .slot = slot, .used = used };
}

bool h265_rps_matches(const struct h265_resolved_rps *resolved, const u16 *words)
{
	int i;

	for (i = 0; i < H265_MAX_REF; i++) {
		if (resolved->words[i] != words[i])
			return false;
		if (words[i] == RPS_END)
			break;
	}
	return true;
}

int h265_resolve_rps(const struct h265_dpb *h, struct h265_resolved_rps *resolved)
{
	bool reset = h->nal_type == H265_NAL_IDR_W_RADL ||
		     h->nal_type == H265_NAL_IDR_N_LP ||
		     (h->nal_type >= H265_NAL_BLA_W_LP && h->nal_type <= H265_NAL_BLA_N_LP);
	struct h265_resolved_rps result = {};
	int lt_poc[H265_MAX_REF], lt_slot[H265_MAX_REF], n_lt = 0;
	bool lt_used[H265_MAX_REF], assigned[H265_MAX_REF] = {};
	u32 st_slots = 0, mask = BIT(h->p[P_LOG2_MAX_POC_LSB_MINUS4] + 4) - 1;
	int i, j, k;

	memcpy(result.words, &h->p[P_CUR_RPS], sizeof(result.words));
	if (reset)
		goto done;

	/* Establish all short-term ownership before considering any LT LSB. */
	for (i = 0; i < H265_MAX_REF && result.words[i] != RPS_END; i++) {
		u16 entry = result.words[i];
		int poc = h265_rps_poc(h->curr_poc, entry), slot = -1;
		bool used = entry & BIT(RPS_USED_BIT);

		if (entry & BIT(RPS_LT_BIT)) {
			lt_poc[n_lt] = poc;
			lt_used[n_lt++] = used;
			continue;
		}
		for (j = 0; j < H265_SLOTS; j++)
			if (h265_reference_candidate(h, j) && h->pic[j].poc == poc &&
			    (slot < 0 || h->pic[j].decode_idx > h->pic[slot].decode_idx))
				slot = j;
		if (slot >= 0)
			st_slots |= BIT(slot);
		h265_rps_add(entry & BIT(RPS_USED_BIT - 1) ?
			     &result.st_before : &result.st_after, slot, used);
	}

	for (i = 0; i < n_lt; i++) {
		int entries[H265_MAX_REF], candidates[H265_SLOTS];
		int count = 0, n_candidates = 0;

		if (assigned[i])
			continue;
		for (j = i; j < n_lt; j++)
			if (((u32)lt_poc[j] & mask) == ((u32)lt_poc[i] & mask))
				entries[count++] = j;
		for (j = 0; j < H265_SLOTS; j++) {
			if ((st_slots & BIT(j)) || !h265_reference_candidate(h, j) ||
			    ((u32)h->pic[j].poc & mask) != ((u32)lt_poc[i] & mask))
				continue;
			for (k = 0; k < n_candidates; k++)
				if (h->pic[candidates[k]].poc == h->pic[j].poc)
					break;
			if (k == n_candidates)
				candidates[n_candidates++] = j;
			else if (h->pic[j].decode_idx > h->pic[candidates[k]].decode_idx)
				candidates[k] = j;
		}
		/* Equal-size LSB groups follow descending POC in RPM order. */
		if (n_candidates && n_candidates != count)
			return -EINVAL;
		for (j = 0; j < n_candidates; j++)
			for (k = j + 1; k < n_candidates; k++)
				if (h->pic[candidates[k]].poc > h->pic[candidates[j]].poc)
					swap(candidates[j], candidates[k]);
		for (j = 0; j < count; j++) {
			lt_slot[entries[j]] = n_candidates ? candidates[j] : -1;
			assigned[entries[j]] = true;
		}
	}
	for (i = 0; i < n_lt; i++)
		h265_rps_add(lt_used[i] ? &result.lt_curr : &result.lt_foll,
			     lt_slot[i], lt_used[i]);
done:
	*resolved = result;
	return 0;
}

void h265_apply_rps(struct h265_dpb *h, const struct h265_resolved_rps *resolved)
{
	const struct h265_rps_set *sets[] = {
		&resolved->st_before, &resolved->st_after, &resolved->lt_curr, &resolved->lt_foll,
	};
	u32 keep = 0, long_term = 0;
	int i, j;

	for (i = 0; i < ARRAY_SIZE(sets); i++)
		for (j = 0; j < sets[i]->count; j++) {
			int slot = sets[i]->ref[j].slot;

			if (slot < 0)
				continue;
			keep |= BIT(slot);
			if (i >= 2)
				long_term |= BIT(slot);
		}
	for (i = 0; i < H265_SLOTS; i++) {
		h->pic[i].long_term = long_term & BIT(i);
		if (!(keep & BIT(i)))
			h->pic[i].referenced = false;
	}
}

static int h265_used_pocs(const struct h265_dpb *h, const struct h265_rps_set *set,
			  int *pocs)
{
	int i, count = 0;

	for (i = 0; i < set->count; i++) {
		const struct h265_rps_ref *ref = &set->ref[i];

		if (!ref->used)
			continue;
		pocs[count++] = ref->slot < 0 ? H265_INVALID_POC : h->pic[ref->slot].poc;
	}
	return count;
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
int h265_set_ref_lists(struct h265_dpb *h, const struct h265_resolved_rps *resolved)
{
	int st0[16], st1[16], lt[16];
	int n_neg, n_pos, n_lt, total, r;
	int l0 = h->slice_type == H265_I_SLICE ? 0 : h->p[P_NUM_REF_IDX_L0_ACTIVE];
	int l1 = h->slice_type == H265_B_SLICE ? h->p[P_NUM_REF_IDX_L1_ACTIVE] : 0;
	u16 mod = h->p[P_MODIFICATION_FLAG];

	memset(h->ref_poc_l0, 0, sizeof(h->ref_poc_l0));
	memset(h->ref_poc_l1, 0, sizeof(h->ref_poc_l1));

	n_neg = h265_used_pocs(h, &resolved->st_before, st0);
	n_pos = h265_used_pocs(h, &resolved->st_after, st1);
	n_lt = h265_used_pocs(h, &resolved->lt_curr, lt);
	total = n_neg + n_pos + n_lt;
	if (!total && (l0 || l1))
		return -EINVAL;

	if (total > 0) {
		for (r = 0; r < l0; r++) {
			int c = (mod & 1) ? h->p[P_MODIFICATION_LIST + r] : r % total;

			if (c >= total)
				return -EINVAL;
			h->ref_poc_l0[r] = h265_ref_entry(c, st0, n_neg, st1, n_pos, lt);
			if (h265_pic_by_poc(h, h->ref_poc_l0[r], true) < 0)
				return -EINVAL;
		}
		if (h->p[P_SLICE_TYPE] == H265_B_SLICE) {
			for (r = 0; r < l1; r++) {
				int c;

				if (mod & 2)
					c = h->p[P_MODIFICATION_LIST + ((mod & 1) ? l0 : 0) + r];
				else
					c = r % total;
				if (c >= total)
					return -EINVAL;
				h->ref_poc_l1[r] = h265_ref_entry(c, st1, n_pos, st0, n_neg, lt);
				if (h265_pic_by_poc(h, h->ref_poc_l1[r], true) < 0)
					return -EINVAL;
			}
		}
	}
	h->ref_num_l0 = l0;
	h->ref_num_l1 = l1;
	return 0;
}

void h265_update_tiles(struct h265_dpb *h, int w_cu, int h_cu,
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
						    0, 0 };
		return;
	}
	h->num_tile_col = h->p[P_NUM_TILE_COLUMNS_MINUS1] + 1;
	h->num_tile_row = h->p[P_NUM_TILE_ROWS_MINUS1] + 1;
	uniform = h->p[P_TILES_ENABLED_FLAG] & 2;

	y = 0;
	for (i = 0; i < h->num_tile_row; i++) {
		x = 0;
		for (j = 0; j < h->num_tile_col; j++) {
			struct h265_tile *t = &h->tile[i][j];

			if (j == h->num_tile_col - 1)
				t->width = w_cu - x;
			else
				t->width = uniform ? (j + 1) * w_cu / h->num_tile_col -
						     j * w_cu / h->num_tile_col :
						     h->p[P_TILE_WIDTH + j];
			if (i == h->num_tile_row - 1)
				t->height = h_cu - y;
			else
				t->height = uniform ? (i + 1) * h_cu / h->num_tile_row -
						      i * h_cu / h->num_tile_row :
						      h->p[P_TILE_HEIGHT + i];
			t->start_cu_x = x;
			t->start_cu_y = y;
			t->sao_vb = j * sao_vb_size;
			t->sao_abv = i * sao_abv_size;
			x += t->width;
		}
		y += h->tile[i][0].height;
	}
}

int h265_tile_index(struct h265_dpb *h, int cu_adr, int w_lcu)
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

struct h265_crop h265_display_crop(const u16 *p)
{
	struct h265_crop crop = { .width = p[P_PIC_WIDTH], .height = p[P_PIC_HEIGHT] };
	u32 width, height;

	if (!p[P_CONFORMANCE_WINDOW_FLAG])
		return crop;
	/* The NV12 path only supports 4:2:0, with two samples per crop unit. */
	width = 2U * (p[P_CONF_WIN_LEFT_OFFSET] + p[P_CONF_WIN_RIGHT_OFFSET]);
	height = 2U * (p[P_CONF_WIN_TOP_OFFSET] + p[P_CONF_WIN_BOTTOM_OFFSET]);
	/* Firmware can leave unusable offsets even when the SPS has no crop. */
	if (width >= crop.width || height >= crop.height)
		return crop;
	crop.left = 2U * p[P_CONF_WIN_LEFT_OFFSET];
	crop.top = 2U * p[P_CONF_WIN_TOP_OFFSET];
	crop.width -= width;
	crop.height -= height;
	return crop;
}

int h265_validate_params(const u16 *p)
{
	unsigned int log2_ctu, width_ctu, height_ctu, cols, rows, i, sum, sao_unit;

	if (p[P_PIC_WIDTH] < H265_MIN_DIM || p[P_PIC_HEIGHT] < H265_MIN_DIM ||
	    p[P_PIC_WIDTH] > H265_MAX_DIM || p[P_PIC_HEIGHT] > H265_MAX_HEIGHT)
		return -EINVAL;
	/* An NV12 CAPTURE queue must not silently discard a Main10 stream's low bits. */
	if (p[P_CHROMA_FORMAT_IDC] != 1 || p[P_BIT_DEPTH])
		return -EOPNOTSUPP;
	if (p[P_LOG2_MIN_CB_SIZE_MINUS3] > 3 || p[P_LOG2_DIFF_MAX_MIN_CB_SIZE] > 3)
		return -EINVAL;
	log2_ctu = p[P_LOG2_MIN_CB_SIZE_MINUS3] + 3 + p[P_LOG2_DIFF_MAX_MIN_CB_SIZE];
	if (log2_ctu < 4 || log2_ctu > 6 || p[P_LOG2_MAX_POC_LSB_MINUS4] > 12)
		return -EINVAL;
	if (p[P_POC_LSB] >= BIT(p[P_LOG2_MAX_POC_LSB_MINUS4] + 4) ||
	    !p[P_TEMPORAL_ID] || p[P_TEMPORAL_ID] > 7 || p[P_NAL_UNIT_TYPE] > 31 ||
	    p[P_SLICE_TYPE] > H265_I_SLICE || p[P_NUM_REF_IDX_L0_ACTIVE] > H265_MAX_REF ||
	    p[P_NUM_REF_IDX_L1_ACTIVE] > H265_MAX_REF ||
	    p[P_SPS_MAX_DEC_PIC_BUFFERING_MINUS1_0] >= H265_MAX_REF ||
	    p[P_SPS_NUM_REORDER_PICS_0] > p[P_SPS_MAX_DEC_PIC_BUFFERING_MINUS1_0] ||
	    p[P_FIVE_MINUS_MAX_NUM_MERGE_CAND] > 4)
		return -EINVAL;
	width_ctu = DIV_ROUND_UP(p[P_PIC_WIDTH], 1U << log2_ctu);
	height_ctu = DIV_ROUND_UP(p[P_PIC_HEIGHT], 1U << log2_ctu);
	if (!p[P_FIRST_SLICE_SEGMENT_IN_PIC_FLAG] &&
	    p[P_SLICE_SEGMENT_ADDRESS] >= width_ctu * height_ctu)
		return -EINVAL;
	if (!(p[P_TILES_ENABLED_FLAG] & 1))
		return 0;
	cols = p[P_NUM_TILE_COLUMNS_MINUS1] + 1;
	rows = p[P_NUM_TILE_ROWS_MINUS1] + 1;
	if (cols > H265_MAX_TILE_COLS || rows > H265_MAX_TILE_ROWS ||
	    cols > width_ctu || rows > height_ctu)
		return -EINVAL;
	if (!(p[P_TILES_ENABLED_FLAG] & 2)) {
		/* RPM has eight explicit row heights; the final row uses the remainder. */
		if (rows > 9)
			return -EOPNOTSUPP;
		for (i = 0, sum = 0; i + 1 < cols; i++) {
			if (!p[P_TILE_WIDTH + i])
				return -EINVAL;
			sum += p[P_TILE_WIDTH + i];
		}
		if (sum >= width_ctu)
			return -EINVAL;
		for (i = 0, sum = 0; i + 1 < rows; i++) {
			if (!p[P_TILE_HEIGHT + i])
				return -EINVAL;
			sum += p[P_TILE_HEIGHT + i];
		}
		if (sum >= height_ctu)
			return -EINVAL;
	}
	sao_unit = (log2_ctu == 4 ? 9 : log2_ctu == 5 ? 14 : 24) << 4;
	if ((sao_unit + 32) * height_ctu * cols > H265_SAO_VB_BUF_SIZE ||
	    sao_unit * width_ctu * rows > H265_SAO_ABV_BUF_SIZE)
		return -EOPNOTSUPP;
	return 0;
}

void h265_update_poc(struct h265_dpb *h)
{
	u16 *p = h->p;

	/* POC, 8.3.1 */
	if (h->nal_type == H265_NAL_IDR_W_RADL || h->nal_type == H265_NAL_IDR_N_LP) {
		h->curr_poc = 0;
	} else {
		int max_lsb = 1 << (p[P_LOG2_MAX_POC_LSB_MINUS4] + 4);
		int prev_lsb = h->prev_tid0_poc & (max_lsb - 1);
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
		    h->nal_type == H265_NAL_BLA_N_LP ||
		    (h->nal_type == H265_NAL_CRA && h->poc_random_access == H265_MAX_INT))
			msb = 0;
		h->curr_poc = msb + lsb;
	}
	/* prevTid0Pic excludes sub-layer non-reference, RADL and RASL pictures. */
	if (h->temporal_id == 1 && h->nal_type != 0 && h->nal_type != 2 &&
	    h->nal_type != 4 && (h->nal_type < 6 || h->nal_type > H265_NAL_RASL_R))
		h->prev_tid0_poc = h->curr_poc;
}

/* Track coded-sequence boundaries and RASL pictures needing older references. */
bool h265_update_random_access(struct h265_dpb *h)
{
	bool idr = h->nal_type == H265_NAL_IDR_W_RADL ||
		   h->nal_type == H265_NAL_IDR_N_LP;
	bool bla = h->nal_type >= H265_NAL_BLA_W_LP && h->nal_type <= H265_NAL_BLA_N_LP;
	bool first = h->poc_random_access == H265_MAX_INT;

	if (!h->slice_segment_addr && (idr || bla || (first && h->nal_type == H265_NAL_CRA)))
		h->epoch++;
	if (first || idr) {
		if (bla || h->nal_type == H265_NAL_CRA)
			h->poc_random_access = h->curr_poc;
		else
			h->poc_random_access = -H265_MAX_INT;
	} else if (bla) {
		h->poc_random_access = h->curr_poc;
	} else if (h->curr_poc < h->poc_random_access &&
		   (h->nal_type == H265_NAL_RASL_N || h->nal_type == H265_NAL_RASL_R)) {
		return true;
	}
	return false;
}

int h265_free_slot(struct h265_dpb *h)
{
	int i;

	for (i = 0; i < h->nslots; i++)
		if (!h->pic[i].output_pending && (!h->pic[i].valid || !h->pic[i].referenced))
			return i;
	return -ENOBUFS;
}

int h265_next_output(struct h265_dpb *h, bool flush)
{
	int i, next = -1;

	for (i = 0; i < H265_SLOTS; i++) {
		if (!h->pic[i].output_pending)
			continue;
		if (next < 0 || h->pic[i].epoch < h->pic[next].epoch ||
		    (h->pic[i].epoch == h->pic[next].epoch && h->pic[i].poc < h->pic[next].poc))
			next = i;
	}
	if (next >= 0 && !flush && h->pic[next].epoch == h->epoch &&
	    h->pending <= h->pic[next].reorder_limit)
		return -1;
	return next;
}

void h265_queue_output(struct h265_dpb *h, unsigned int slot)
{
	h->pic[slot].output_pending = true;
	h->pending++;
}

void h265_remove_output(struct h265_dpb *h, unsigned int slot)
{
	h->pic[slot].output_pending = false;
	h->pending--;
}
