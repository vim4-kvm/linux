// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "axg-card-params.h"
#include "meson-card.h"
#include "t7-audio-kunit.h"

static void t7_biquad_stability(struct kunit *test)
{
	u32 coeff[5] = { BIT(23), 0, 0, 0, 0 };

	KUNIT_EXPECT_TRUE(test, t7_aed_biquad_valid(coeff));
	coeff[4] = BIT(23);
	KUNIT_EXPECT_FALSE(test, t7_aed_biquad_valid(coeff));
	coeff[4] = (u32)-BIT(23) & GENMASK(25, 0);
	KUNIT_EXPECT_FALSE(test, t7_aed_biquad_valid(coeff));
	coeff[4] = 0;
	coeff[3] = BIT(23) - 1;
	KUNIT_EXPECT_TRUE(test, t7_aed_biquad_valid(coeff));
	coeff[3]++;
	KUNIT_EXPECT_FALSE(test, t7_aed_biquad_valid(coeff));
	coeff[3] = 0;
	coeff[0] |= BIT(26);
	KUNIT_EXPECT_FALSE(test, t7_aed_biquad_valid(coeff));
}

static void t7_drc_constraints(struct kunit *test)
{
	u32 full[18] = { [0] = BIT(23), [2] = BIT(23), [6] = BIT(23),
			 [8] = BIT(23), [12] = BIT(23), [16] = 0x40000 };
	u32 multi[30] = {};
	unsigned int band, pair;

	KUNIT_EXPECT_TRUE(test, t7_aed_drc_valid(full, ARRAY_SIZE(full)));
	full[0]++;
	KUNIT_EXPECT_FALSE(test, t7_aed_drc_valid(full, ARRAY_SIZE(full)));
	full[0]--;
	full[15] = 1;
	KUNIT_EXPECT_FALSE(test, t7_aed_drc_valid(full, ARRAY_SIZE(full)));
	for (band = 0; band < 3; band++)
		for (pair = 0; pair < 6; pair += 2)
			multi[band * 10 + pair] = BIT(23);
	KUNIT_EXPECT_TRUE(test, t7_aed_drc_valid(multi, ARRAY_SIZE(multi)));
	multi[17] = 0x40001;
	KUNIT_EXPECT_FALSE(test, t7_aed_drc_valid(multi, ARRAY_SIZE(multi)));
	KUNIT_EXPECT_FALSE(test, t7_aed_drc_valid(multi, 29));
}

struct card_ctx {
	struct meson_card private;
	struct snd_soc_dai_link links[4];
	struct axg_dai_link_tdm_data pdm, reference;
	void *link_data[4];
	struct snd_soc_pcm_runtime rtd;
	struct snd_pcm_hw_params params;
};

static void t7_card_conversion(struct kunit *test)
{
	struct card_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct snd_soc_card *card;
	struct snd_interval *rate, *channels;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	card = &ctx->private.card;
	card->num_links = ARRAY_SIZE(ctx->links);
	card->dai_link = ctx->links;
	ctx->links[0].dynamic = true;
	ctx->links[3].dynamic = true;
	ctx->link_data[1] = &ctx->pdm;
	ctx->link_data[2] = &ctx->reference;
	ctx->private.link_data = ctx->link_data;
	snd_soc_card_set_drvdata(card, &ctx->private);
	ctx->rtd.card = card;
	ctx->rtd.id = 1;
	rate = hw_param_interval(&ctx->params, SNDRV_PCM_HW_PARAM_RATE);
	channels = hw_param_interval(&ctx->params, SNDRV_PCM_HW_PARAM_CHANNELS);
	rate->min = 16000;
	rate->max = 16000;
	channels->min = 6;
	channels->max = 6;
	axg_card_update_frontends(card);
	KUNIT_EXPECT_TRUE(test, ctx->links[0].dpcm_merged_rate);
	KUNIT_EXPECT_TRUE(test, ctx->links[3].dpcm_merged_chan);
	KUNIT_ASSERT_EQ(test, axg_card_be_fixup(&ctx->rtd, &ctx->params), 0);
	KUNIT_EXPECT_EQ(test, channels->min, 6);
	ctx->pdm.convert_channels = 4;
	ctx->reference.convert_channels = 2;
	ctx->reference.convert_rate = 48000;
	axg_card_update_frontends(card);
	KUNIT_EXPECT_FALSE(test, ctx->links[0].dpcm_merged_rate);
	KUNIT_EXPECT_FALSE(test, ctx->links[3].dpcm_merged_chan);
	KUNIT_ASSERT_EQ(test, axg_card_be_fixup(&ctx->rtd, &ctx->params), 0);
	KUNIT_EXPECT_EQ(test, rate->min, 16000);
	KUNIT_EXPECT_EQ(test, channels->max, 4);
	ctx->rtd.id = 2;
	KUNIT_ASSERT_EQ(test, axg_card_be_fixup(&ctx->rtd, &ctx->params), 0);
	KUNIT_EXPECT_EQ(test, rate->min, 48000);
	KUNIT_EXPECT_EQ(test, rate->max, 48000);
	KUNIT_EXPECT_EQ(test, channels->min, 2);
	KUNIT_EXPECT_EQ(test, channels->max, 2);
}

static struct kunit_case t7_audio_cases[] = {
	KUNIT_CASE(t7_biquad_stability),
	KUNIT_CASE(t7_drc_constraints),
	KUNIT_CASE(t7_card_conversion),
	{}
};

static struct kunit_suite t7_audio_suite = {
	.name = "meson-t7-audio",
	.test_cases = t7_audio_cases,
};

kunit_test_suite(t7_audio_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Meson T7 audio parameter validation tests");
