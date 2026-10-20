/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

/* Benchmark candidate only; not linked into the capture driver.
 * Non-overlapping RAM buffers, positive size divisible by 128.
 * No loads/stores outside the requested span; only caller-saved v0-v7 are used.
 */
SYM_FUNC_START(t7_vdin_copy_blocks)
1:
	ldp	q0, q1, [x1]
	ldp	q2, q3, [x1, #32]
	ldp	q4, q5, [x1, #64]
	ldp	q6, q7, [x1, #96]
	stnp	q0, q1, [x0]
	stnp	q2, q3, [x0, #32]
	stnp	q4, q5, [x0, #64]
	stnp	q6, q7, [x0, #96]
	add	x1, x1, #128
	add	x0, x0, #128
	subs	x2, x2, #128
	b.ne	1b
	ret
SYM_FUNC_END(t7_vdin_copy_blocks)
