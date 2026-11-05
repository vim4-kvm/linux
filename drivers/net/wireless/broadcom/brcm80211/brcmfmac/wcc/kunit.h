/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BRCMF_WCC_KUNIT_H
#define BRCMF_WCC_KUNIT_H

#include <linux/types.h>

struct brcmf_pub;

#if IS_ENABLED(CONFIG_KUNIT)
void brcmf_wcc_feat_update(struct brcmf_pub *drvr, const char *caps,
			   int err, bool discovery_bss);
#endif

#endif
