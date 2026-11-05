/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/* Copyright (c) 2018 BayLibre, SAS. */
#ifndef AXG_CARD_PARAMS_H
#define AXG_CARD_PARAMS_H

#include <linux/types.h>

struct axg_dai_link_tdm_mask {
	u32 tx;
	u32 rx;
};

struct axg_dai_link_tdm_data {
	unsigned int convert_rate;
	unsigned int convert_channels;
	unsigned int mclk_fs;
	unsigned int slots;
	unsigned int slot_width;
	u32 *tx_mask;
	u32 *rx_mask;
	struct axg_dai_link_tdm_mask *codec_masks;
};

#if IS_ENABLED(CONFIG_KUNIT)
struct snd_soc_pcm_runtime;
struct snd_pcm_hw_params;
struct snd_soc_card;

int axg_card_be_fixup(struct snd_soc_pcm_runtime *rtd, struct snd_pcm_hw_params *params);
void axg_card_update_frontends(struct snd_soc_card *card);
#endif

#endif
