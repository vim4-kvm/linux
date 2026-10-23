// SPDX-License-Identifier: GPL-2.0-only
/* Amlogic T7 asynchronous sample rate converter. */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "t7-asrc-coeff.h"

#define ASRC_CTRL0		0x00
#define ASRC_CTRL1		0x04
#define ASRC_CTRL2		0x08
#define ASRC_PHASE_INIT		0x0c
#define ASRC_PHASE_STEP		0x10
#define ASRC_ADJ_CTRL0		0x18
#define ASRC_ADJ_CTRL1		0x1c
#define ASRC_ADJ_SHIFT		0x20
#define ASRC_ADJ_LENGTH		0x24
#define ASRC_ADJ_FORCE		0x28
#define ASRC_WATCHDOG		0x30
#define ASRC_STATUS		0x40
#define ASRC_AA_ADDR		0xc0
#define ASRC_AA_DATA		0xc4
#define ASRC_SINC_ADDR		0x100
#define ASRC_SINC_DATA		0x104
/* Virtual address for the separate audio-bus synchronizer register. */
#define ASRC_CHSYNC		0x108
#define ASRC_INPUT_ENABLE	BIT(11)
#define ASRC_BYPASS		BIT(24)

struct t7_asrc {
	struct regmap *map;
	struct clk_bulk_data clocks[3];
	struct mutex lock; /* Protect stream parameters and power transitions. */
	unsigned int rate;
	unsigned int channels;
	unsigned int input_rate;
	bool loopback;
	bool powered;
	bool changing_source;
};

static int t7_asrc_coefficients(struct regmap *map, unsigned int address,
				unsigned int data, const s32 *coefficients,
				unsigned int count, unsigned int offset)
{
	unsigned int i;
	int ret;

	ret = regmap_write(map, address, offset);
	if (ret)
		return ret;
	for (i = 0; i < count; i++) {
		ret = regmap_write(map, data, coefficients[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int t7_asrc_start(struct regmap *map, unsigned int source,
			 unsigned int input_rate, unsigned int output_rate,
			 unsigned int channels, bool loopback)
{
	unsigned int val, msb, lsb, ratio;
	u64 step;
	bool aa;
	int ret;

	if (!channels || channels > 8 || !output_rate || !input_rate ||
	    input_rate > 192000 || output_rate > 192000 ||
	    input_rate < 8000 || output_rate < 8000 || source > 11 ||
	    (loopback && (source != 6 || output_rate != 16000)))
		return -EINVAL;

	/* The phase accumulator has four integer bits. */
	aa = input_rate > output_rate * 8;
	step = div_u64((u64)input_rate << (aa ? 27 : 28), output_rate);
	if (step > U32_MAX)
		return -ERANGE;
	msb = source == 3 || source == 8 || source == 11 ? 27 : 31;
	lsb = loopback ? 0 : msb - 23;
	ratio = (loopback ? 12288000 : output_rate * 256) / output_rate / channels;
	val = source | ASRC_BYPASS | BIT(12) | (0x2a << 18) |
		(aa ? BIT(10) : 0) |
		FIELD_PREP(GENMASK(17, 13), msb) |
		FIELD_PREP(GENMASK(31, 27), lsb);
	ret = regmap_write(map, ASRC_CTRL0, 7);
	if (ret)
		return ret;
	ret = regmap_write(map, ASRC_CTRL0, 0);
	if (ret)
		return ret;
	ret = regmap_write(map, ASRC_CTRL1, val);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(map, ASRC_STATUS, val,
				       ((val >> 11) & 7) == 1, 1, 20);
	if (ret)
		return ret;
	ret = t7_asrc_coefficients(map, ASRC_SINC_ADDR, ASRC_SINC_DATA,
				   loopback ? sinc_coef : sinc8_coef,
				   SINC8_FILTER_COEF_SIZE, 0);
	if (ret)
		return ret;
	if (loopback || aa) {
		ret = t7_asrc_coefficients(map, ASRC_AA_ADDR, ASRC_AA_DATA,
					   aa_coef_a_half, AA_FILTER_COEF_SIZE, 129 * 32);
		if (ret)
			return ret;
	}

	{
		const struct reg_sequence sequence[] = {
			{ ASRC_CTRL2, 0x3f3f | channels << 24 | (aa ? BIT(16) : 0) },
			{ ASRC_PHASE_INIT, 0 },
			{ ASRC_PHASE_STEP, step },
			{ ASRC_CTRL0, BIT(2) },
			{ ASRC_ADJ_CTRL1, ratio << 16 },
			{ ASRC_ADJ_SHIFT, 0x1f020604 },
			{ ASRC_ADJ_LENGTH, 0x100 },
			{ ASRC_ADJ_FORCE, 0 },
			{ ASRC_ADJ_CTRL0, 1 },
			{ ASRC_CTRL0, 0 },
			{ ASRC_WATCHDOG, 0x1c593 },
			{ ASRC_CHSYNC, BIT(31) | (channels - 1) },
		};

		ret = regmap_multi_reg_write(map, sequence, ARRAY_SIZE(sequence));
		if (ret)
			return ret;
	}
	return regmap_update_bits(map, ASRC_CTRL1,
				  ASRC_BYPASS | ASRC_INPUT_ENABLE | BIT(26),
				  ASRC_INPUT_ENABLE | BIT(26));
}

static int t7_asrc_stop(struct regmap *map)
{
	int ret, sync_ret;

	ret = regmap_update_bits(map, ASRC_CTRL1,
				 ASRC_BYPASS | ASRC_INPUT_ENABLE, ASRC_BYPASS);
	sync_ret = regmap_update_bits(map, ASRC_CHSYNC, BIT(31), 0);
	return ret ?: sync_ret;
}

static int t7_asrc_event(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *control, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct t7_asrc *asrc = snd_soc_component_get_drvdata(component);
	unsigned int source;
	int ret;

	guard(mutex)(&asrc->lock);
	if (event == SND_SOC_DAPM_PRE_PMD) {
		if (!asrc->powered)
			return 0;
		ret = t7_asrc_stop(asrc->map);
		regcache_cache_only(asrc->map, true);
		clk_bulk_disable_unprepare(ARRAY_SIZE(asrc->clocks), asrc->clocks);
		clk_rate_exclusive_put(asrc->clocks[1].clk);
		asrc->powered = false;
		return ret;
	}
	if (!asrc->rate)
		return -EINVAL;
	if (asrc->changing_source)
		return -EBUSY;
	ret = clk_set_rate_exclusive(asrc->clocks[1].clk,
				     asrc->loopback ? 12288000 : asrc->rate * 256);
	if (ret)
		return ret;
	ret = clk_set_rate(asrc->clocks[2].clk, clk_get_rate(asrc->clocks[1].clk));
	if (ret)
		goto release_rate;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(asrc->clocks), asrc->clocks);
	if (ret)
		goto release_rate;
	regcache_cache_only(asrc->map, false);
	ret = regmap_read(asrc->map, ASRC_CTRL1, &source);
	if (!ret)
		ret = t7_asrc_start(asrc->map, asrc->loopback ? 6 : source & 31,
				    asrc->input_rate, asrc->rate, asrc->channels,
				    asrc->loopback);
	if (!ret) {
		asrc->powered = true;
		return 0;
	}
	t7_asrc_stop(asrc->map);
	regcache_cache_only(asrc->map, true);
	clk_bulk_disable_unprepare(ARRAY_SIZE(asrc->clocks), asrc->clocks);
release_rate:
	clk_rate_exclusive_put(asrc->clocks[1].clk);
	return ret;
}

static int t7_asrc_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct t7_asrc *asrc = snd_soc_dai_get_drvdata(dai);

	guard(mutex)(&asrc->lock);
	if (asrc->powered)
		return -EBUSY;
	if (asrc->loopback && params_rate(params) != 16000)
		return -EINVAL;
	asrc->rate = params_rate(params);
	asrc->channels = params_channels(params);
	return 0;
}

static const unsigned int t7_asrc_rates[] = {
	8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000,
};

static const char * const t7_asrc_rate_texts[] = {
	"8000", "11025", "16000", "22050", "32000", "44100", "48000", "88200",
	"96000", "176400", "192000",
};

static SOC_ENUM_SINGLE_EXT_DECL(t7_asrc_rate_enum, t7_asrc_rate_texts);

static int t7_asrc_rate_get(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct t7_asrc *asrc = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	unsigned int i;

	guard(mutex)(&asrc->lock);
	for (i = 0; i < ARRAY_SIZE(t7_asrc_rates); i++)
		if (asrc->input_rate == t7_asrc_rates[i])
			value->value.enumerated.item[0] = i;
	return 0;
}

static int t7_asrc_rate_put(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct t7_asrc *asrc = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	unsigned int i = value->value.enumerated.item[0];

	if (i >= ARRAY_SIZE(t7_asrc_rates))
		return -EINVAL;
	guard(mutex)(&asrc->lock);
	if (asrc->powered)
		return -EBUSY;
	if (asrc->input_rate == t7_asrc_rates[i])
		return 0;
	asrc->input_rate = t7_asrc_rates[i];
	return 1;
}

static const struct snd_kcontrol_new t7_asrc_controls[] = {
	SOC_ENUM_EXT("Input Sample Rate", t7_asrc_rate_enum, t7_asrc_rate_get, t7_asrc_rate_put),
};

static const char * const t7_asrc_sources[] = {
	"IN 0", "IN 1", "IN 2", "IN 3", "IN 4", "IN 5", "IN 6", "IN 7",
	"IN 8", "IN 9", "IN 10", "IN 11",
};

static SOC_ENUM_SINGLE_DECL(t7_asrc_source_enum, ASRC_CTRL1, 0, t7_asrc_sources);

static int t7_asrc_source_put(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct snd_soc_component *component = snd_soc_dapm_kcontrol_to_component(control);
	struct t7_asrc *asrc = snd_soc_component_get_drvdata(component);
	int ret;

	/* Do not hold our lock across DAPM, which can call the power event. */
	mutex_lock(&asrc->lock);
	if (asrc->powered) {
		mutex_unlock(&asrc->lock);
		return -EBUSY;
	}
	asrc->changing_source = true;
	mutex_unlock(&asrc->lock);
	ret = snd_soc_dapm_put_enum_double(control, value);
	mutex_lock(&asrc->lock);
	asrc->changing_source = false;
	mutex_unlock(&asrc->lock);
	return ret;
}

static const struct snd_kcontrol_new t7_asrc_mux =
	SOC_DAPM_ENUM_EXT("Input Source", t7_asrc_source_enum,
			  snd_soc_dapm_get_enum_double, t7_asrc_source_put);

static const struct snd_soc_dapm_widget t7_asrc_widgets[] = {
	SND_SOC_DAPM_AIF_IN("IN 0", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 1", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 2", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 3", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 4", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 5", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 6", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 7", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 8", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 9", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 10", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("IN 11", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MUX("SRC SEL", SND_SOC_NOPM, 0, 0, &t7_asrc_mux),
	SND_SOC_DAPM_PGA_E("ASRC", SND_SOC_NOPM, 0, 0, NULL, 0, t7_asrc_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_AIF_OUT("OUT", "Capture", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route t7_asrc_routes[] = {
	{ "SRC SEL", "IN 0", "IN 0" },
	{ "SRC SEL", "IN 1", "IN 1" },
	{ "SRC SEL", "IN 2", "IN 2" },
	{ "SRC SEL", "IN 3", "IN 3" },
	{ "SRC SEL", "IN 4", "IN 4" },
	{ "SRC SEL", "IN 5", "IN 5" },
	{ "SRC SEL", "IN 6", "IN 6" },
	{ "SRC SEL", "IN 7", "IN 7" },
	{ "SRC SEL", "IN 8", "IN 8" },
	{ "SRC SEL", "IN 9", "IN 9" },
	{ "SRC SEL", "IN 10", "IN 10" },
	{ "SRC SEL", "IN 11", "IN 11" },
	{ "ASRC", NULL, "SRC SEL" },
	{ "OUT", NULL, "ASRC" },
};

static const struct snd_soc_component_driver t7_asrc_component = {
	.controls = t7_asrc_controls,
	.num_controls = ARRAY_SIZE(t7_asrc_controls),
	.dapm_widgets = t7_asrc_widgets,
	.num_dapm_widgets = ARRAY_SIZE(t7_asrc_widgets),
	.dapm_routes = t7_asrc_routes,
	.num_dapm_routes = ARRAY_SIZE(t7_asrc_routes),
};

static const struct snd_soc_dapm_widget t7_asrc_b_widgets[] = {
	SND_SOC_DAPM_AIF_IN("IN 6", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_PGA_E("ASRC", SND_SOC_NOPM, 0, 0, NULL, 0, t7_asrc_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_AIF_OUT("OUT", "Capture", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route t7_asrc_b_routes[] = {
	{ "ASRC", NULL, "IN 6" },
	{ "OUT", NULL, "ASRC" },
};

static const struct snd_soc_component_driver t7_asrc_b_component = {
	.controls = t7_asrc_controls,
	.num_controls = ARRAY_SIZE(t7_asrc_controls),
	.dapm_widgets = t7_asrc_b_widgets,
	.num_dapm_widgets = ARRAY_SIZE(t7_asrc_b_widgets),
	.dapm_routes = t7_asrc_b_routes,
	.num_dapm_routes = ARRAY_SIZE(t7_asrc_b_routes),
};

static const struct snd_soc_dai_ops t7_asrc_dai_ops = {
	.hw_params = t7_asrc_hw_params,
};

static struct snd_soc_dai_driver t7_asrc_dai = {
	.name = "ASRC",
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_8000_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &t7_asrc_dai_ops,
};

static bool t7_asrc_volatile(struct device *dev, unsigned int reg)
{
	return reg != ASRC_CTRL1;
}

static const struct reg_default t7_asrc_defaults[] = { { ASRC_CTRL1, ASRC_BYPASS } };

static const struct regmap_range t7_asrc_read_ranges[] = {
	regmap_reg_range(ASRC_CTRL0, ASRC_WATCHDOG),
	regmap_reg_range(ASRC_STATUS, 0x5c),
	regmap_reg_range(ASRC_AA_ADDR, ASRC_AA_DATA),
	regmap_reg_range(ASRC_SINC_ADDR, ASRC_SINC_DATA),
	regmap_reg_range(ASRC_CHSYNC, ASRC_CHSYNC),
};

static const struct regmap_access_table t7_asrc_read_table = {
	.yes_ranges = t7_asrc_read_ranges,
	.n_yes_ranges = ARRAY_SIZE(t7_asrc_read_ranges),
};

static const struct regmap_range t7_asrc_write_ranges[] = {
	regmap_reg_range(ASRC_CTRL0, ASRC_WATCHDOG),
	regmap_reg_range(ASRC_AA_ADDR, ASRC_AA_DATA),
	regmap_reg_range(ASRC_SINC_ADDR, ASRC_SINC_DATA),
	regmap_reg_range(ASRC_CHSYNC, ASRC_CHSYNC),
};

static const struct regmap_access_table t7_asrc_write_table = {
	.yes_ranges = t7_asrc_write_ranges,
	.n_yes_ranges = ARRAY_SIZE(t7_asrc_write_ranges),
};

static const struct regmap_config t7_asrc_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = ASRC_CHSYNC,
	.cache_type = REGCACHE_FLAT,
	.volatile_reg = t7_asrc_volatile,
	.reg_defaults = t7_asrc_defaults,
	.num_reg_defaults = ARRAY_SIZE(t7_asrc_defaults),
	.rd_table = &t7_asrc_read_table,
	.wr_table = &t7_asrc_write_table,
};

static int t7_asrc_read(void *context, unsigned int reg, unsigned int *val)
{
	void __iomem **banks = context;

	*val = readl(reg < 0x108 ? banks[0] + reg : banks[1]);
	return 0;
}

static int t7_asrc_write(void *context, unsigned int reg, unsigned int val)
{
	void __iomem **banks = context;

	writel(val, reg < 0x108 ? banks[0] + reg : banks[1]);
	return 0;
}

static const struct regmap_bus t7_asrc_bus = {
	.reg_read = t7_asrc_read,
	.reg_write = t7_asrc_write,
	.fast_io = true,
};

static int t7_asrc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem **banks;
	struct t7_asrc *asrc;
	struct snd_soc_dai_driver *dai;
	int ret, i;

	asrc = devm_kzalloc(dev, sizeof(*asrc), GFP_KERNEL);
	dai = devm_kmemdup(dev, &t7_asrc_dai, sizeof(*dai), GFP_KERNEL);
	banks = devm_kcalloc(dev, 2, sizeof(*banks), GFP_KERNEL);
	if (!asrc || !banks || !dai)
		return -ENOMEM;
	for (i = 0; i < 2; i++) {
		banks[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(banks[i]))
			return PTR_ERR(banks[i]);
	}
	asrc->map = devm_regmap_init(dev, &t7_asrc_bus, banks, &t7_asrc_regmap_config);
	if (IS_ERR(asrc->map))
		return PTR_ERR(asrc->map);
	asrc->clocks[0].id = "pclk";
	asrc->clocks[1].id = "mclk";
	asrc->clocks[2].id = "clk";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(asrc->clocks), asrc->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");
	ret = devm_mutex_init(dev, &asrc->lock);
	if (ret)
		return ret;
	asrc->loopback = of_device_is_compatible(dev->of_node, "amlogic,t7-asrc-b");
	if (asrc->loopback)
		dai->capture.rates = SNDRV_PCM_RATE_16000;
	asrc->input_rate = 48000;
	regcache_cache_only(asrc->map, true);
	platform_set_drvdata(pdev, asrc);
	return devm_snd_soc_register_component(dev,
		asrc->loopback ? &t7_asrc_b_component : &t7_asrc_component, dai, 1);
}

static const struct of_device_id t7_asrc_match[] = {
	{ .compatible = "amlogic,t7-asrc-a" },
	{ .compatible = "amlogic,t7-asrc-b" },
	{ }
};
MODULE_DEVICE_TABLE(of, t7_asrc_match);

static struct platform_driver t7_asrc_driver = {
	.probe = t7_asrc_probe,
	.driver = {
		.name = "meson-t7-asrc",
		.of_match_table = t7_asrc_match,
	},
};
module_platform_driver(t7_asrc_driver);

MODULE_DESCRIPTION("Amlogic T7 asynchronous sample rate converter");
MODULE_LICENSE("GPL");
