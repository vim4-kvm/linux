/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ADLAK_BOUNDS_H
#define ADLAK_BOUNDS_H

/* Subtraction avoids signed overflow in untrusted task/fixup ranges. */
static inline int adlak_range_valid(int offset, int count, int total)
{
	return offset >= 0 && count >= 0 && total >= 0 &&
	       offset <= total && count <= total - offset;
}

static inline int adlak_fixup_valid(int location, unsigned int shift,
				    unsigned int unit, int config_size)
{
	return !(location & 3) && shift < 32 && unit &&
	       adlak_range_valid(location, 4, config_size);
}

#endif
