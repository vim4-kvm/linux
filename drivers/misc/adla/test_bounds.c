// SPDX-License-Identifier: GPL-2.0
/* Host test: cc -Wall -Wextra -Werror test_bounds.c -o /tmp/adla-bounds */
#include <assert.h>
#include <limits.h>

#include "drv/common/adlak_bounds.h"

int main(void)
{
	int offset, count, total;

	for (total = 0; total < 32; total++)
		for (offset = -1; offset < 34; offset++)
			for (count = -1; count < 34; count++)
				assert(adlak_range_valid(offset, count, total) ==
				       (offset >= 0 && count >= 0 &&
					(long long)offset + count <= total));
	assert(!adlak_range_valid(INT_MAX, 1, INT_MAX));
	assert(adlak_range_valid(INT_MAX, 0, INT_MAX));
	assert(!adlak_range_valid(INT_MIN, INT_MAX, INT_MAX));
	assert(adlak_fixup_valid(0, 31, 1, 4));
	assert(!adlak_fixup_valid(0, 32, 1, 4));
	assert(!adlak_fixup_valid(0, 0, 0, 4));
	assert(!adlak_fixup_valid(1, 0, 1, 8));
	assert(!adlak_fixup_valid(-4, 0, 1, 8));
	assert(!adlak_fixup_valid(4, 0, 1, 4));
	return 0;
}
