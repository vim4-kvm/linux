// SPDX-License-Identifier: GPL-2.0-only
/* Amlogic T7 voice activity detector observing a 16 kHz PDM stream. */

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pm_wakeirq.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#include "t7-vad-coeff.h"

#define VAD_TOP0		0x00
#define VAD_TOP1		0x04
#define VAD_LUT_CTRL	0xb0
#define VAD_LUT_WRITE	0xb4
/* Virtual register for the separate audio-fabric feed control. */
#define VAD_FEED	0xc8

struct t7_vad {
	struct regmap *map;
	struct clk_bulk_data clocks[2];
	atomic_t events;
	bool powered;
};

static int t7_vad_stop(struct regmap *map)
{
	int ret, top_ret, feed_ret;

	ret = regmap_write(map, VAD_TOP0, 0);
	top_ret = regmap_write(map, VAD_TOP1, 0);
	feed_ret = regmap_update_bits(map, VAD_FEED, BIT(31), 0);
	return ret ?: top_ret ?: feed_ret;
}

static int t7_vad_start(struct regmap *map)
{
	const struct reg_sequence sequence[] = {
		{ VAD_TOP0, 0x7ff }, { VAD_TOP0, 0 },
		{ VAD_TOP1, 0xff }, { VAD_TOP1, 0 },
		{ VAD_TOP0, GENMASK(31, 27) | BIT(23) | BIT(22) },
		{ 0x48, (2U << 30) | BIT(24) | BIT(16) }, /* 32 ms frame. */
		{ 0x4c, 0xd65 }, { 0x50, 0xd00103ff },
		{ 0x54, 0x11050000 }, { 0x58, 0x1b }, { 0x5c, 0xc001fd },
		{ 0x60, 0x137f0000 }, { 0x64, 0x186d0000 }, { 0x68, 0xfd00f61 },
		{ 0x6c, 0x10030001 },
		{ 0xbc, 1 }, { 0xc0, 2 }, { 0xc4, 0xa0000719 },
	};
	unsigned int i;
	int ret;

	ret = t7_vad_stop(map);
	if (ret)
		return ret;
	/* Coefficient access requires the feed and detector clocks running. */
	ret = regmap_update_bits(map, VAD_FEED, BIT(31) | BIT(30) | GENMASK(16, 12),
				 BIT(31) | BIT(30) | (4 << 12));
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(map, sequence, ARRAY_SIZE(sequence));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(vad_de_coeff_16k); i++) {
		ret = regmap_write(map, 0x0c + i * 4, vad_de_coeff_16k[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(vad_ram_coeff); i++) {
		ret = regmap_write(map, VAD_LUT_WRITE, vad_ram_coeff[i]);
		if (ret)
			return ret;
		ret = regmap_write(map, VAD_LUT_CTRL, BIT(31) | i);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence enable[] = {
			{ VAD_TOP0, 0x7ff }, { VAD_TOP0, 0 },
			{ VAD_TOP1, 0xff }, { VAD_TOP1, 0 },
			{ VAD_TOP0, GENMASK(31, 27) | BIT(23) | BIT(22) },
		};

		return regmap_multi_reg_write(map, enable, ARRAY_SIZE(enable));
	}
}

static int t7_vad_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *control, int event)
{
	struct t7_vad *vad = snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int ret;

	if (event == SND_SOC_DAPM_PRE_PMD) {
		if (!vad->powered)
			return 0;
		ret = t7_vad_stop(vad->map);
		regcache_cache_only(vad->map, true);
		clk_bulk_disable_unprepare(ARRAY_SIZE(vad->clocks), vad->clocks);
		vad->powered = false;
		return ret;
	}
	ret = clk_set_rate(vad->clocks[1].clk, 25000000);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(vad->clocks), vad->clocks);
	if (ret)
		return ret;
	regcache_cache_only(vad->map, false);
	ret = t7_vad_start(vad->map);
	if (ret) {
		t7_vad_stop(vad->map);
		regcache_cache_only(vad->map, true);
		clk_bulk_disable_unprepare(ARRAY_SIZE(vad->clocks), vad->clocks);
	} else {
		vad->powered = true;
	}
	return ret;
}

static irqreturn_t t7_vad_irq(int irq, void *data)
{
	struct device *dev = data;
	struct t7_vad *vad = dev_get_drvdata(dev);

	atomic_add_unless(&vad->events, 1, INT_MAX);
	pm_wakeup_event(dev, 0);
	return IRQ_HANDLED;
}

static int t7_vad_events_get(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct t7_vad *vad = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));

	value->value.integer.value[0] = atomic_read(&vad->events);
	return 0;
}

static const struct snd_kcontrol_new t7_vad_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Voice Activity Events",
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = snd_soc_info_volsw,
		.get = t7_vad_events_get,
		.private_value = SOC_SINGLE_VALUE(SND_SOC_NOPM, 0, 0, INT_MAX, 0, 0),
	},
};

static const struct snd_kcontrol_new t7_vad_switch =
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 0);

static const struct snd_soc_dapm_widget t7_vad_widgets[] = {
	SND_SOC_DAPM_AIF_IN("PDM", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_SWITCH("Detector Enable", SND_SOC_NOPM, 0, 0, &t7_vad_switch),
	SND_SOC_DAPM_PGA_E("Detector", SND_SOC_NOPM, 0, 0, NULL, 0, t7_vad_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("Voice Activity"),
};

static const struct snd_soc_dapm_route t7_vad_routes[] = {
	{ "Detector Enable", "Switch", "PDM" },
	{ "Detector", NULL, "Detector Enable" },
	{ "Voice Activity", NULL, "Detector" },
};

static const struct snd_soc_component_driver t7_vad_component = {
	.controls = t7_vad_controls,
	.num_controls = ARRAY_SIZE(t7_vad_controls),
	.dapm_widgets = t7_vad_widgets,
	.num_dapm_widgets = ARRAY_SIZE(t7_vad_widgets),
	.dapm_routes = t7_vad_routes,
	.num_dapm_routes = ARRAY_SIZE(t7_vad_routes),
};

static int t7_vad_read(void *context, unsigned int reg, unsigned int *value)
{
	void __iomem **banks = context;

	*value = readl(reg == VAD_FEED ? banks[1] : banks[0] + reg);
	return 0;
}

static int t7_vad_write(void *context, unsigned int reg, unsigned int value)
{
	void __iomem **banks = context;

	writel(value, reg == VAD_FEED ? banks[1] : banks[0] + reg);
	return 0;
}

static const struct regmap_bus t7_vad_bus = {
	.reg_read = t7_vad_read, .reg_write = t7_vad_write, .fast_io = true,
};

static bool t7_vad_volatile(struct device *dev, unsigned int reg)
{
	return true;
}

static const struct regmap_config t7_vad_regmap = {
	.reg_bits = 32, .val_bits = 32, .reg_stride = 4, .max_register = VAD_FEED,
	.cache_type = REGCACHE_FLAT, .volatile_reg = t7_vad_volatile,
};

static int t7_vad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t7_vad *vad;
	void __iomem **banks;
	int ret, irq, i;

	vad = devm_kzalloc(dev, sizeof(*vad), GFP_KERNEL);
	banks = devm_kcalloc(dev, 2, sizeof(*banks), GFP_KERNEL);
	if (!vad || !banks)
		return -ENOMEM;
	for (i = 0; i < 2; i++) {
		banks[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(banks[i]))
			return PTR_ERR(banks[i]);
	}
	vad->map = devm_regmap_init(dev, &t7_vad_bus, banks, &t7_vad_regmap);
	if (IS_ERR(vad->map))
		return PTR_ERR(vad->map);
	regcache_cache_only(vad->map, true);
	vad->clocks[0].id = "pclk";
	vad->clocks[1].id = "clk";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(vad->clocks), vad->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");
	irq = platform_get_irq_byname(pdev, "voice");
	if (irq < 0)
		return irq;
	platform_set_drvdata(pdev, vad);
	ret = devm_request_irq(dev, irq, t7_vad_irq, 0, dev_name(dev), dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request voice IRQ\n");
	device_set_wakeup_capable(dev, true);
	ret = dev_pm_set_wake_irq(dev, irq);
	if (ret) {
		device_set_wakeup_capable(dev, false);
		return ret;
	}
	ret = devm_snd_soc_register_component(dev, &t7_vad_component, NULL, 0);
	if (ret) {
		dev_pm_clear_wake_irq(dev);
		device_set_wakeup_capable(dev, false);
	}
	return ret;
}

static void t7_vad_remove(struct platform_device *pdev)
{
	dev_pm_clear_wake_irq(&pdev->dev);
	device_set_wakeup_capable(&pdev->dev, false);
}

static const struct of_device_id t7_vad_match[] = {
	{ .compatible = "amlogic,t7-vad" }, { }
};
MODULE_DEVICE_TABLE(of, t7_vad_match);

static struct platform_driver t7_vad_driver = {
	.probe = t7_vad_probe,
	.remove = t7_vad_remove,
	.driver = { .name = "meson-t7-vad", .of_match_table = t7_vad_match },
};
module_platform_driver(t7_vad_driver);

MODULE_DESCRIPTION("Amlogic T7 voice activity detector");
MODULE_LICENSE("GPL");
