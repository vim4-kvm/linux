/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MESON_T7_DOS_STATE_H__
#define __MESON_T7_DOS_STATE_H__

#include <linux/errno.h>
#include <linux/types.h>

struct meson_t7_dos_state {
	unsigned int encoders;
	bool decoder;
	bool poisoned;
};

/* The caller serializes access to the arbitration state. */
static inline int meson_t7_dos_acquire(struct meson_t7_dos_state *state, bool exclusive)
{
	if (state->poisoned)
		return -EIO;
	if (state->decoder || (exclusive && state->encoders))
		return -EBUSY;
	if (exclusive)
		state->decoder = true;
	else
		state->encoders++;
	return 0;
}

static inline void meson_t7_dos_release(struct meson_t7_dos_state *state, bool exclusive)
{
	if (exclusive)
		state->decoder = false;
	else if (state->encoders)
		state->encoders--;
}

#endif /* __MESON_T7_DOS_STATE_H__ */
