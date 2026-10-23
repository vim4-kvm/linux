// SPDX-License-Identifier: GPL-2.0-only
/* Amlogic T7 stereo audio effects engine. */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/uaccess.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "t7-aed-coeff.h"

#define AED_EQ_EN		0x08
#define AED_VOLUME		0x10
#define AED_DRC_CTRL		0x1c
#define AED_ED_CTRL		0x148
#define AED_DC_EN		0x14c
#define AED_MDRC_CTRL		0xcc
#define AED_TOP_CTRL0		0x20c
#define AED_TOP_CTRL1		0x210
#define AED_TOP_CTRL2		0x214
#define AED_EQ_WORDS		200
#define AED_Q23_ONE		0x800000

struct t7_aed {
	struct regmap *map;
	struct clk_bulk_data clocks[2];
	struct mutex lock; /* Protect coefficients and stream configuration. */
	u32 eq[AED_EQ_WORDS];
	u32 drc[18];
	u32 mdrc[30];
	unsigned int width;
	bool powered;
	bool changing_source;
};

/* Q3.23, encoded in 26 bits. The denominator is 1 + a1 z^-1 + a2 z^-2. */
static bool t7_aed_biquad_valid(const u32 *coeff)
{
	s32 a1 = sign_extend32(coeff[3], 25);
	s32 a2 = sign_extend32(coeff[4], 25);
	unsigned int i;

	for (i = 0; i < 5; i++)
		if (coeff[i] & ~GENMASK(25, 0))
			return false;
	return a2 > -AED_Q23_ONE && a2 < AED_Q23_ONE &&
		AED_Q23_ONE + a1 + a2 > 0 && AED_Q23_ONE - a1 + a2 > 0;
}

static int t7_aed_load_eq(struct regmap *map, const u32 *eq)
{
	unsigned int bank, i;
	int ret;

	for (bank = 0; bank < 2; bank++) {
		unsigned int control = bank ? 0x244 : 0;
		unsigned int data = bank ? 0x248 : 4;

		for (i = 0; i < 225; i++) {
			u32 value = i < 5 ? DC_CUT_COEFF[i] :
				i < 205 ? eq[i - 5] : CROSSOVER_COEFF[i - 205];

			ret = regmap_write(map, data, value);
			if (ret)
				return ret;
			ret = regmap_write(map, control, (i << 2) | 3);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int t7_aed_stop(struct regmap *map)
{
	int ret, gate_ret;

	ret = regmap_update_bits(map, AED_TOP_CTRL0, BIT(31) | GENMASK(2, 0), 0);
	gate_ret = regmap_update_bits(map, AED_TOP_CTRL2, BIT(3), 0);
	return ret ?: gate_ret;
}

static bool t7_aed_drc_valid(const u32 *drc, unsigned int words)
{
	unsigned int i, j;
	static const unsigned int pairs[] = { 0, 2, 6, 8, 12 };

	if (words == 18) {
		for (i = 0; i < ARRAY_SIZE(pairs); i++) {
			j = pairs[i];
			if (drc[j] > AED_Q23_ONE || drc[j + 1] > AED_Q23_ONE ||
			    drc[j] + drc[j + 1] != AED_Q23_ONE)
				return false;
		}
		return drc[5] <= 0x40000 && drc[11] <= 0x40000 && drc[14] <= 255 &&
			drc[15] == drc[10] && drc[16] == 0x40000;
	}
	if (words != 30)
		return false;
	for (i = 0; i < 30; i += 10) {
		for (j = 0; j < 6; j += 2)
			if (drc[i + j] > AED_Q23_ONE || drc[i + j + 1] > AED_Q23_ONE ||
			    drc[i + j] + drc[i + j + 1] != AED_Q23_ONE)
				return false;
		if (drc[i + 7] > 0x40000 || drc[i + (i ? 9 : 8)] > 0x40000)
			return false;
	}
	return true;
}

static int t7_aed_start(struct regmap *map, unsigned int width, const u32 *eq,
			const u32 *drc, const u32 *mdrc)
{
	static const unsigned int drc_regs[] = {
		0x68, 0x6c, 0x98, 0x9c, 0x28, 0x3c,
		0x70, 0x74, 0xa0, 0xa4, 0x2c, 0x44,
		0x20, 0x24, 0xc8, 0x54, 0x40, 0x64,
	};
	unsigned int i;
	int ret;
	const struct reg_sequence sequence[] = {
		{ 0x0c, 20 }, /* Twenty biquads per channel. */
		{ 0x14, 2 }, /* Volume slew. */
		{ 0x18, 0 }, /* Mute. */
		{ 0x188, 0 }, /* Noise detector bypass. */
		{ 0x18c, 0x40000 }, { 0x190, 0 },
		{ 0x194, 0 }, { 0x198, 0x40000 }, { 0x19c, 0x7fffff },
		{ AED_TOP_CTRL1, (1 << 20) | BIT(5) | BIT(4) | 1 },
		{ AED_TOP_CTRL2, BIT(3) | 1 | (1 << 12) }, /* TDMOUT B, stereo. */
		{ AED_ED_CTRL, 1 }, { AED_ED_CTRL, 1 },
	};

	if (width != 16 && width != 24 && width != 32)
		return -EINVAL;
	if (!t7_aed_drc_valid(drc, 18) || !t7_aed_drc_valid(mdrc, 30))
		return -EINVAL;
	for (i = 0; i < AED_EQ_WORDS; i += 5)
		if (!t7_aed_biquad_valid(eq + i))
			return -EINVAL;
	ret = t7_aed_stop(map);
	if (ret)
		return ret;
	ret = t7_aed_load_eq(map, eq);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(drc_regs); i++) {
		ret = regmap_write(map, drc_regs[i], drc[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < 30; i++) {
		ret = regmap_write(map, 0xd0 + i * 4, mdrc[i]);
		if (ret)
			return ret;
	}
	ret = regmap_multi_reg_write(map, sequence, ARRAY_SIZE(sequence));
	if (ret)
		return ret;
	ret = regmap_update_bits(map, AED_TOP_CTRL0, GENMASK(13, 6),
				 ((width == 16 ? 2 : 4) << 11) | ((width - 1) << 6));
	if (ret)
		return ret;
	ret = regmap_update_bits(map, AED_TOP_CTRL0, BIT(29), BIT(29));
	if (ret)
		return ret;
	ret = regmap_update_bits(map, AED_TOP_CTRL0, BIT(29), 0);
	if (ret)
		return ret;
	return regmap_update_bits(map, AED_TOP_CTRL0, BIT(31) | GENMASK(2, 0),
				  BIT(31) | GENMASK(2, 0));
}

static int t7_aed_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *control, int event)
{
	struct t7_aed *aed = snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int ret;

	guard(mutex)(&aed->lock);
	if (event == SND_SOC_DAPM_PRE_PMD) {
		if (!aed->powered)
			return 0;
		ret = t7_aed_stop(aed->map);
		regcache_cache_only(aed->map, true);
		clk_bulk_disable_unprepare(ARRAY_SIZE(aed->clocks), aed->clocks);
		aed->powered = false;
		return ret;
	}
	if (!aed->width || aed->changing_source)
		return -EBUSY;
	ret = clk_set_rate(aed->clocks[1].clk, 200000000);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(aed->clocks), aed->clocks);
	if (ret)
		return ret;
	regcache_cache_only(aed->map, false);
	ret = t7_aed_start(aed->map, aed->width, aed->eq, aed->drc, aed->mdrc);
	if (!ret) {
		regcache_mark_dirty(aed->map);
		ret = regcache_sync(aed->map);
	}
	if (!ret) {
		aed->powered = true;
		return 0;
	}
	t7_aed_stop(aed->map);
	regcache_cache_only(aed->map, true);
	clk_bulk_disable_unprepare(ARRAY_SIZE(aed->clocks), aed->clocks);
	return ret;
}

static int t7_aed_coeff_get(struct snd_kcontrol *control, unsigned int __user *bytes,
			    unsigned int size)
{
	struct t7_aed *aed = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	struct soc_bytes_ext *params = (void *)control->private_value;
	__be32 eq[AED_EQ_WORDS];
	u32 *coeff = params->max == 800 ? aed->eq : params->max == 72 ? aed->drc : aed->mdrc;
	unsigned int i;

	if (size < params->max)
		return -ENOSPC;
	guard(mutex)(&aed->lock);
	for (i = 0; i < params->max / 4; i++)
		eq[i] = cpu_to_be32(coeff[i]);
	return copy_to_user(bytes, eq, params->max) ? -EFAULT : 0;
}

static int t7_aed_coeff_put(struct snd_kcontrol *control, const unsigned int __user *bytes,
			    unsigned int size)
{
	struct t7_aed *aed = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	struct soc_bytes_ext *params = (void *)control->private_value;
	u32 eq[AED_EQ_WORDS];
	u32 *coeff = params->max == 800 ? aed->eq : params->max == 72 ? aed->drc : aed->mdrc;
	unsigned int i;

	if (size != params->max)
		return -EINVAL;
	if (copy_from_user(eq, bytes, size))
		return -EFAULT;
	for (i = 0; i < size / 4; i++)
		eq[i] = be32_to_cpu((__force __be32)eq[i]);
	if (size == 800) {
		for (i = 0; i < AED_EQ_WORDS; i += 5)
			if (!t7_aed_biquad_valid(eq + i))
				return -EINVAL;
	} else {
		if (!t7_aed_drc_valid(eq, size / 4))
			return -EINVAL;
	}
	guard(mutex)(&aed->lock);
	if (aed->powered)
		return -EBUSY;
	if (!memcmp(eq, coeff, size))
		return 0;
	memcpy(coeff, eq, size);
	return 1;
}

static const DECLARE_TLV_DB_SCALE(t7_aed_lr_tlv, -12750, 50, 1);
static const struct snd_kcontrol_new t7_aed_controls[] = {
	SOC_SINGLE("EQ Switch", AED_EQ_EN, 0, 1, 0),
	SOC_SINGLE("DRC Switch", AED_DRC_CTRL, 0, 1, 0),
	SOC_SINGLE("Multiband DRC Switch", AED_MDRC_CTRL, 8, 1, 0),
	SOC_SINGLE("DC Filter Switch", AED_DC_EN, 0, 1, 0),
	SOC_SINGLE("Master Playback Volume", AED_VOLUME, 16, 1023, 1),
	SOC_DOUBLE_TLV("Playback Volume", AED_VOLUME, 8, 0, 255, 1, t7_aed_lr_tlv),
	SND_SOC_BYTES_TLV("EQ Coefficients", 800, t7_aed_coeff_get, t7_aed_coeff_put),
	SND_SOC_BYTES_TLV("DRC Coefficients", 72, t7_aed_coeff_get, t7_aed_coeff_put),
	SND_SOC_BYTES_TLV("Multiband DRC Coefficients", 120, t7_aed_coeff_get, t7_aed_coeff_put),
};

static const char * const t7_aed_sources[] = { "IN 0", "IN 1", "IN 2", "IN 3" };
static SOC_ENUM_SINGLE_DECL(t7_aed_source_enum, AED_TOP_CTRL0, 3, t7_aed_sources);

static int t7_aed_source_put(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct snd_soc_component *component = snd_soc_dapm_kcontrol_to_component(control);
	struct t7_aed *aed = snd_soc_component_get_drvdata(component);
	int ret;

	mutex_lock(&aed->lock);
	if (aed->powered) {
		mutex_unlock(&aed->lock);
		return -EBUSY;
	}
	aed->changing_source = true;
	mutex_unlock(&aed->lock);
	ret = snd_soc_dapm_put_enum_double(control, value);
	mutex_lock(&aed->lock);
	aed->changing_source = false;
	mutex_unlock(&aed->lock);
	return ret;
}

static const struct snd_kcontrol_new t7_aed_mux =
	SOC_DAPM_ENUM_EXT("Input Source", t7_aed_source_enum,
			  snd_soc_dapm_get_enum_double, t7_aed_source_put);

static const struct snd_soc_dapm_widget t7_aed_widgets[] = {
	SND_SOC_DAPM_AIF_IN("IN 0", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 1", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 2", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 3", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MUX("SRC SEL", SND_SOC_NOPM, 0, 0, &t7_aed_mux),
	SND_SOC_DAPM_PGA_E("ENGINE", SND_SOC_NOPM, 0, 0, NULL, 0, t7_aed_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_AIF_OUT("TDMOUT B", "Playback", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route t7_aed_routes[] = {
	{ "SRC SEL", "IN 0", "IN 0" }, { "SRC SEL", "IN 1", "IN 1" },
	{ "SRC SEL", "IN 2", "IN 2" }, { "SRC SEL", "IN 3", "IN 3" },
	{ "ENGINE", NULL, "SRC SEL" }, { "TDMOUT B", NULL, "ENGINE" },
};

static const struct snd_soc_component_driver t7_aed_component = {
	.controls = t7_aed_controls,
	.num_controls = ARRAY_SIZE(t7_aed_controls),
	.dapm_widgets = t7_aed_widgets,
	.num_dapm_widgets = ARRAY_SIZE(t7_aed_widgets),
	.dapm_routes = t7_aed_routes,
	.num_dapm_routes = ARRAY_SIZE(t7_aed_routes),
};

static int t7_aed_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct t7_aed *aed = snd_soc_dai_get_drvdata(dai);

	guard(mutex)(&aed->lock);
	if (aed->powered)
		return -EBUSY;
	aed->width = params_width(params);
	return 0;
}

static const struct snd_soc_dai_ops t7_aed_ops = { .hw_params = t7_aed_hw_params };
static struct snd_soc_dai_driver t7_aed_dai = {
	.name = "AED",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &t7_aed_ops,
};

static bool t7_aed_volatile(struct device *dev, unsigned int reg)
{
	return reg != AED_EQ_EN && reg != AED_DRC_CTRL && reg != AED_VOLUME &&
		reg != AED_TOP_CTRL0 && reg != AED_DC_EN && reg != AED_MDRC_CTRL;
}

static const struct reg_default t7_aed_defaults[] = {
	{ AED_EQ_EN, 0 }, { AED_DRC_CTRL, 2 << 3 }, { AED_VOLUME, 0xffff },
	{ AED_TOP_CTRL0, 0 },
	{ AED_DC_EN, 0 }, { AED_MDRC_CTRL, 0x3f },
};

static const struct regmap_config t7_aed_regmap = {
	.reg_bits = 32, .val_bits = 32, .reg_stride = 4, .max_register = 0x374,
	.cache_type = REGCACHE_FLAT,
	.volatile_reg = t7_aed_volatile,
	.reg_defaults = t7_aed_defaults,
	.num_reg_defaults = ARRAY_SIZE(t7_aed_defaults),
};

static int t7_aed_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t7_aed *aed;
	void __iomem *regs;
	unsigned int i;
	int ret;

	aed = devm_kzalloc(dev, sizeof(*aed), GFP_KERNEL);
	if (!aed)
		return -ENOMEM;
	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	aed->map = devm_regmap_init_mmio(dev, regs, &t7_aed_regmap);
	if (IS_ERR(aed->map))
		return PTR_ERR(aed->map);
	aed->clocks[0].id = "pclk";
	aed->clocks[1].id = "clk";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(aed->clocks), aed->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");
	ret = devm_mutex_init(dev, &aed->lock);
	if (ret)
		return ret;
	for (i = 0; i < AED_EQ_WORDS; i += 5)
		aed->eq[i] = AED_Q23_ONE;
	memcpy(aed->drc, fullband_drc_coeff, sizeof(aed->drc));
	aed->drc[15] = aed->drc[10]; /* Output threshold follows the second knee. */
	memcpy(aed->mdrc, multiband_drc_coeff, sizeof(aed->mdrc));
	regcache_cache_only(aed->map, true);
	platform_set_drvdata(pdev, aed);
	return devm_snd_soc_register_component(dev, &t7_aed_component, &t7_aed_dai, 1);
}

static const struct of_device_id t7_aed_match[] = {
	{ .compatible = "amlogic,t7-aed" }, { }
};
MODULE_DEVICE_TABLE(of, t7_aed_match);

static struct platform_driver t7_aed_driver = {
	.probe = t7_aed_probe,
	.driver = { .name = "meson-t7-aed", .of_match_table = t7_aed_match },
};
module_platform_driver(t7_aed_driver);

MODULE_DESCRIPTION("Amlogic T7 stereo audio effects engine");
MODULE_LICENSE("GPL");
