// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
#include <linux/errno.h>
#include <linux/kernel.h>
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

static int h265_sps(struct h265_headers *headers, struct h265_bits *b)
{
	struct h265_sps_prefix sps = { .valid = true };
	bool profile[7], level[7];
	u32 id, chroma, sublayers, i, first;

	h265_u(b, 4);
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
	id = h265_ue(b);
	chroma = h265_ue(b);
	if (id >= ARRAY_SIZE(headers->sps) || chroma > 3)
		return -EINVAL;
	if (chroma == 3)
		h265_u(b, 1);
	h265_ue(b);
	h265_ue(b);
	if (h265_u(b, 1))
		for (i = 0; i < 4; i++)
			h265_ue(b);
	h265_ue(b);
	h265_ue(b);
	h265_ue(b);
	first = h265_u(b, 1) ? 0 : sublayers;
	for (i = first; i <= sublayers; i++) {
		u32 buffering = h265_ue(b);

		sps.limits.max_dec_pic_buffering = buffering + 1;
		sps.limits.max_num_reorder_pics = h265_ue(b);
		sps.limits.max_latency_increase_plus1 = h265_ue(b);
		if (buffering > 15 || sps.limits.max_num_reorder_pics > buffering)
			return -EINVAL;
	}
	if (b->error)
		return -EINVAL;
	headers->sps[id] = sps;
	return 0;
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
	if (b->error)
		return -EINVAL;
	headers->pps[id] = pps;
	return 0;
}

static int h265_slice(const struct h265_headers *headers, struct h265_bits *b,
		      u8 type, u8 temporal_id, struct h265_au_info *au)
{
	struct h265_picture_header pic = { .nal_type = type, .temporal_id = temporal_id,
		.pic_output = true };
	const struct h265_pps_prefix *pps;
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
	pic.pps_id = id;
	pic.limits = headers->sps[pps->sps_id].limits;
	h265_skip(b, pps->extra_slice_header_bits);
	if (h265_ue(b) > 2)
		return -EINVAL;
	if (pps->output_flag_present)
		pic.pic_output = h265_u(b, 1);
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
