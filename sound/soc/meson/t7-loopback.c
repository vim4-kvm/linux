// SPDX-License-Identifier: GPL-2.0-only
/* Amlogic T7 microphone/reference hardware loopback combiner. */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define LB_CTRL0		0x00
#define LB_CTRL1		0x04
#define LB_CTRL2		0x08
#define LB_CTRL3		0x0c
#define LB_INSERT_SYNC		0x34
#define LB_ORIGINAL_SYNC	0x38
#define LB_SOURCE		GENMASK(24, 20)
#define LB_CHANNELS		GENMASK(19, 16)

struct t7_loopback {
	struct regmap *map;
	struct clk *pclk;
	struct reset_control *reset;
	struct mutex lock; /* Serialize channel controls, hw_params and DAPM. */
	bool configured;
	bool powered;
	bool changing_source;
};

static int t7_loopback_start(struct regmap *map, unsigned int data,
			     unsigned int reference)
{
	unsigned int source = FIELD_GET(LB_SOURCE, data);
	unsigned int ref_source = FIELD_GET(LB_SOURCE, reference);
	unsigned int channels = FIELD_GET(LB_CHANNELS, data) + 1;
	unsigned int ref_channels = FIELD_GET(LB_CHANNELS, reference) + 1;
	unsigned int format;

	if ((source > 4 && source != 18) || (ref_source != 6 && ref_source != 14))
		return -EINVAL;
	format = source == 3 ? (3 << 13) | (27 << 8) | (4 << 3) : 31 << 8;
	{
		const struct reg_sequence sequence[] = {
			{ LB_CTRL0, format },
			{ LB_CTRL1, 31 << 8 },
			{ LB_CTRL2, (data & (LB_SOURCE | LB_CHANNELS)) | GENMASK(channels - 1, 0) },
			{ LB_CTRL3, (reference & (LB_SOURCE | LB_CHANNELS)) |
				    GENMASK(ref_channels - 1, 0) },
			{ LB_ORIGINAL_SYNC, BIT(31) | BIT(29) | (channels - 1) },
			{ LB_INSERT_SYNC, BIT(31) | BIT(29) | (ref_channels - 1) },
			{ LB_CTRL0, format | BIT(31) },
		};

		return regmap_multi_reg_write(map, sequence, ARRAY_SIZE(sequence));
	}
}

static int t7_loopback_stop(struct regmap *map)
{
	int ret, original_ret, insert_ret;

	ret = regmap_update_bits(map, LB_CTRL0, BIT(31), 0);
	original_ret = regmap_update_bits(map, LB_ORIGINAL_SYNC, BIT(31), 0);
	insert_ret = regmap_update_bits(map, LB_INSERT_SYNC, BIT(31), 0);
	return ret ?: original_ret ?: insert_ret;
}

static int t7_loopback_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *control, int event)
{
	struct t7_loopback *lb = snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	unsigned int data, reference;
	int ret;

	guard(mutex)(&lb->lock);
	if (event == SND_SOC_DAPM_PRE_PMD) {
		ret = t7_loopback_stop(lb->map);
		regcache_cache_only(lb->map, true);
		clk_disable_unprepare(lb->pclk);
		lb->powered = false;
		return ret;
	}
	if (!lb->configured)
		return -EINVAL;
	if (lb->changing_source)
		return -EBUSY;
	ret = regmap_read(lb->map, LB_CTRL2, &data);
	if (ret)
		return ret;
	ret = regmap_read(lb->map, LB_CTRL3, &reference);
	if (ret)
		return ret;
	ret = clk_prepare_enable(lb->pclk);
	if (ret)
		return ret;
	ret = reset_control_reset(lb->reset);
	if (ret)
		goto disable_clock;
	regcache_cache_only(lb->map, false);
	ret = t7_loopback_start(lb->map, data, reference);
	if (!ret) {
		lb->powered = true;
		return 0;
	}
	t7_loopback_stop(lb->map);
	regcache_cache_only(lb->map, true);
disable_clock:
	clk_disable_unprepare(lb->pclk);
	return ret;
}

static int t7_loopback_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct t7_loopback *lb = snd_soc_dai_get_drvdata(dai);
	unsigned int data, reference;
	int ret;

	guard(mutex)(&lb->lock);
	if (lb->powered)
		return -EBUSY;
	ret = regmap_read(lb->map, LB_CTRL2, &data);
	if (ret)
		return ret;
	ret = regmap_read(lb->map, LB_CTRL3, &reference);
	if (ret)
		return ret;
	if (params_channels(params) != FIELD_GET(LB_CHANNELS, data) +
	    FIELD_GET(LB_CHANNELS, reference) + 2)
		return -EINVAL;
	lb->configured = true;
	return 0;
}

static int t7_loopback_count_info(struct snd_kcontrol *control, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 1;
	info->value.integer.max = 16;
	return 0;
}

static int t7_loopback_count_get(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct t7_loopback *lb = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	unsigned int config;
	int ret;

	guard(mutex)(&lb->lock);
	ret = regmap_read(lb->map, control->private_value, &config);
	if (ret)
		return ret;
	value->value.integer.value[0] = FIELD_GET(LB_CHANNELS, config) + 1;
	return 0;
}

static int t7_loopback_count_put(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct t7_loopback *lb = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	unsigned int count = value->value.integer.value[0], config;
	int ret;

	if (!count || count > 16)
		return -EINVAL;
	guard(mutex)(&lb->lock);
	if (lb->powered)
		return -EBUSY;
	ret = regmap_read(lb->map, control->private_value, &config);
	if (ret || FIELD_GET(LB_CHANNELS, config) == count - 1)
		return ret;
	ret = regmap_update_bits(lb->map, control->private_value,
				 LB_CHANNELS, FIELD_PREP(LB_CHANNELS, count - 1));
	if (ret)
		return ret;
	lb->configured = false;
	return 1;
}

static const struct snd_kcontrol_new t7_loopback_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Data Capture Channels",
		.info = t7_loopback_count_info,
		.get = t7_loopback_count_get,
		.put = t7_loopback_count_put,
		.private_value = LB_CTRL2,
	}, {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Reference Capture Channels",
		.info = t7_loopback_count_info,
		.get = t7_loopback_count_get,
		.put = t7_loopback_count_put,
		.private_value = LB_CTRL3,
	},
};

static const char * const t7_loopback_data_texts[] = {
	"TDMIN A", "TDMIN B", "TDMIN C", "SPDIFIN", "PDM", "TDMIN D",
};

static const unsigned int t7_loopback_data_values[] = { 0, 1, 2, 3, 4, 18 };
static SOC_VALUE_ENUM_SINGLE_DECL(t7_loopback_data_enum, LB_CTRL2, 20, 31,
				  t7_loopback_data_texts, t7_loopback_data_values);
static const char * const t7_loopback_ref_texts[] = { "TDMIN LB", "ASRC B" };
static const unsigned int t7_loopback_ref_values[] = { 6, 14 };
static SOC_VALUE_ENUM_SINGLE_DECL(t7_loopback_ref_enum, LB_CTRL3, 20, 31,
				  t7_loopback_ref_texts, t7_loopback_ref_values);

static int t7_loopback_source_put(struct snd_kcontrol *control,
				  struct snd_ctl_elem_value *value)
{
	struct snd_soc_component *component = snd_soc_dapm_kcontrol_to_component(control);
	struct t7_loopback *lb = snd_soc_component_get_drvdata(component);
	int ret;

	mutex_lock(&lb->lock);
	if (lb->powered) {
		mutex_unlock(&lb->lock);
		return -EBUSY;
	}
	lb->changing_source = true;
	mutex_unlock(&lb->lock);
	ret = snd_soc_dapm_put_enum_double(control, value);
	mutex_lock(&lb->lock);
	lb->changing_source = false;
	mutex_unlock(&lb->lock);
	return ret;
}

static const struct snd_kcontrol_new t7_loopback_data_mux =
	SOC_DAPM_ENUM_EXT("Data Source", t7_loopback_data_enum,
			  snd_soc_dapm_get_enum_double, t7_loopback_source_put);
static const struct snd_kcontrol_new t7_loopback_ref_mux =
	SOC_DAPM_ENUM_EXT("Reference Source", t7_loopback_ref_enum,
			  snd_soc_dapm_get_enum_double, t7_loopback_source_put);

static const struct snd_soc_dapm_widget t7_loopback_widgets[] = {
	SND_SOC_DAPM_AIF_IN("TDMIN A", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("TDMIN B", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("TDMIN C", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("TDMIN D", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("SPDIFIN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("PDM", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("TDMIN LB", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("ASRC B", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MUX("DATA SEL", SND_SOC_NOPM, 0, 0, &t7_loopback_data_mux),
	SND_SOC_DAPM_MUX("REF SEL", SND_SOC_NOPM, 0, 0, &t7_loopback_ref_mux),
	SND_SOC_DAPM_PGA_E("COMBINER", SND_SOC_NOPM, 0, 0, NULL, 0,
			   t7_loopback_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_AIF_OUT("OUT", "Capture", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route t7_loopback_routes[] = {
	{ "DATA SEL", "TDMIN A", "TDMIN A" },
	{ "DATA SEL", "TDMIN B", "TDMIN B" },
	{ "DATA SEL", "TDMIN C", "TDMIN C" },
	{ "DATA SEL", "TDMIN D", "TDMIN D" },
	{ "DATA SEL", "SPDIFIN", "SPDIFIN" },
	{ "DATA SEL", "PDM", "PDM" },
	{ "REF SEL", "TDMIN LB", "TDMIN LB" },
	{ "REF SEL", "ASRC B", "ASRC B" },
	{ "COMBINER", NULL, "DATA SEL" },
	{ "COMBINER", NULL, "REF SEL" },
	{ "OUT", NULL, "COMBINER" },
};

static const struct snd_soc_component_driver t7_loopback_component = {
	.controls = t7_loopback_controls,
	.num_controls = ARRAY_SIZE(t7_loopback_controls),
	.dapm_widgets = t7_loopback_widgets,
	.num_dapm_widgets = ARRAY_SIZE(t7_loopback_widgets),
	.dapm_routes = t7_loopback_routes,
	.num_dapm_routes = ARRAY_SIZE(t7_loopback_routes),
};

static const struct snd_soc_dai_ops t7_loopback_dai_ops = {
	.hw_params = t7_loopback_hw_params,
};

static struct snd_soc_dai_driver t7_loopback_dai = {
	.name = "Loopback",
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 32,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &t7_loopback_dai_ops,
};

static bool t7_loopback_volatile(struct device *dev, unsigned int reg)
{
	return reg != LB_CTRL2 && reg != LB_CTRL3;
}

static const struct reg_default t7_loopback_defaults[] = {
	{ LB_CTRL2, (4 << 20) | (3 << 16) | 15 },
	{ LB_CTRL3, (14 << 20) | (1 << 16) | 3 },
};

static const struct regmap_config t7_loopback_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = LB_ORIGINAL_SYNC,
	.cache_type = REGCACHE_FLAT,
	.volatile_reg = t7_loopback_volatile,
	.reg_defaults = t7_loopback_defaults,
	.num_reg_defaults = ARRAY_SIZE(t7_loopback_defaults),
};

static int t7_loopback_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t7_loopback *lb;
	void __iomem *regs;
	int ret;

	lb = devm_kzalloc(dev, sizeof(*lb), GFP_KERNEL);
	if (!lb)
		return -ENOMEM;
	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	lb->map = devm_regmap_init_mmio(dev, regs, &t7_loopback_regmap_config);
	if (IS_ERR(lb->map))
		return PTR_ERR(lb->map);
	lb->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(lb->pclk))
		return dev_err_probe(dev, PTR_ERR(lb->pclk), "failed to get clock\n");
	lb->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(lb->reset))
		return dev_err_probe(dev, PTR_ERR(lb->reset), "failed to get reset\n");
	ret = devm_mutex_init(dev, &lb->lock);
	if (ret)
		return ret;
	regcache_cache_only(lb->map, true);
	platform_set_drvdata(pdev, lb);
	return devm_snd_soc_register_component(dev, &t7_loopback_component, &t7_loopback_dai, 1);
}

static const struct of_device_id t7_loopback_match[] = {
	{ .compatible = "amlogic,t7-loopback" },
	{ }
};
MODULE_DEVICE_TABLE(of, t7_loopback_match);

static struct platform_driver t7_loopback_driver = {
	.probe = t7_loopback_probe,
	.driver = {
		.name = "meson-t7-loopback",
		.of_match_table = t7_loopback_match,
	},
};
module_platform_driver(t7_loopback_driver);

MODULE_DESCRIPTION("Amlogic T7 microphone and playback reference combiner");
MODULE_LICENSE("GPL");
