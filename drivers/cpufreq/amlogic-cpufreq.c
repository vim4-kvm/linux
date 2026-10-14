// SPDX-License-Identifier: GPL-2.0
/*
 * Amlogic binned CPU frequency scaling
 *
 * Newer Amlogic SoCs are binned at manufacture, and better silicon runs
 * the same CPU frequencies at lower voltages. The secure monitor reports
 * the bin, which selects the matching opp-microvolt-bin<N> voltages before
 * the generic cpufreq-dt driver takes over.
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 */

#include <linux/arch_topology.h>
#include <linux/cpu.h>
#include <linux/firmware/meson/meson_sm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

/* T7 reports one bin per cluster: the firmware takes the cluster index */
#define AMLOGIC_CPUFREQ_BIN_PER_CLUSTER	BIT(0)

#define AMLOGIC_CPUFREQ_MAX_CLUSTERS	2

struct amlogic_cpufreq_priv {
	struct platform_device *cpufreq_dt_pdev;
	int opp_tokens[];
};

/* One OPP table per cluster: the first one to probe sets up all CPUs */
static bool amlogic_cpufreq_done;

static int amlogic_cpufreq_get_bin(struct device *dev, unsigned int cluster)
{
	struct meson_sm_firmware *fw;
	s32 bin;
	int ret;

	struct device_node *sm_np __free(device_node) =
		of_parse_phandle(dev->of_node, "amlogic,secure-monitor", 0);
	if (!sm_np)
		return dev_err_probe(dev, -ENODEV,
				     "Failed to get the secure monitor\n");

	fw = meson_sm_get(sm_np);
	if (!fw)
		return -EPROBE_DEFER;

	ret = meson_sm_call(fw, SM_GET_DVFS_TABLE_INDEX, &bin, cluster, 0, 0,
			    0, 0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read the CPU bin\n");

	if (bin < 0)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid CPU bin %d\n", bin);

	return bin;
}

static void amlogic_cpufreq_clear_config(struct amlogic_cpufreq_priv *priv)
{
	unsigned int cpu;

	for_each_present_cpu(cpu)
		dev_pm_opp_clear_config(priv->opp_tokens[cpu]);
}

static int amlogic_cpufreq_probe(struct platform_device *pdev)
{
	char name[] = "binXXXXXXXXXXX"; /* Integers can take 11 chars max */
	int bins[AMLOGIC_CPUFREQ_MAX_CLUSTERS];
	struct dev_pm_opp_config config = {};
	struct amlogic_cpufreq_priv *priv;
	unsigned long flags;
	struct device *cpu_dev;
	unsigned int cpu, cluster;
	int i, nbins, ret;

	if (amlogic_cpufreq_done)
		return 0;

	flags = (unsigned long)of_device_get_match_data(&pdev->dev);
	nbins = flags & AMLOGIC_CPUFREQ_BIN_PER_CLUSTER ?
		AMLOGIC_CPUFREQ_MAX_CLUSTERS : 1;
	for (i = 0; i < nbins; i++) {
		bins[i] = amlogic_cpufreq_get_bin(&pdev->dev, i);
		if (bins[i] < 0)
			return bins[i];
	}

	priv = devm_kzalloc(&pdev->dev,
			    struct_size(priv, opp_tokens, num_possible_cpus()),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	config.prop_name = name;

	for_each_present_cpu(cpu) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev) {
			ret = -ENODEV;
			goto err_clear_config;
		}

		cluster = nbins > 1 ? topology_cluster_id(cpu) : 0;
		if (cluster >= nbins) {
			ret = -EINVAL;
			goto err_clear_config;
		}
		snprintf(name, sizeof(name), "bin%d", bins[cluster]);

		ret = dev_pm_opp_set_config(cpu_dev, &config);
		if (ret < 0)
			goto err_clear_config;

		priv->opp_tokens[cpu] = ret;
	}

	priv->cpufreq_dt_pdev = platform_device_register_simple("cpufreq-dt",
								-1, NULL, 0);
	if (IS_ERR(priv->cpufreq_dt_pdev)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(priv->cpufreq_dt_pdev),
				    "Failed to register cpufreq-dt\n");
		goto err_clear_config;
	}

	platform_set_drvdata(pdev, priv);
	amlogic_cpufreq_done = true;

	for (i = 0; i < nbins; i++)
		dev_info(&pdev->dev, "Using CPU bin %d voltages for cluster %d\n",
			 bins[i], i);

	return 0;

err_clear_config:
	amlogic_cpufreq_clear_config(priv);
	return ret;
}

static void amlogic_cpufreq_remove(struct platform_device *pdev)
{
	struct amlogic_cpufreq_priv *priv = platform_get_drvdata(pdev);

	if (!priv)
		return;

	amlogic_cpufreq_done = false;
	platform_device_unregister(priv->cpufreq_dt_pdev);
	amlogic_cpufreq_clear_config(priv);
}

static const struct of_device_id amlogic_cpufreq_match_list[] = {
	{ .compatible = "amlogic,s4-cpu-operating-points" },
	{
		.compatible = "amlogic,t7-cpu-operating-points",
		.data = (void *)AMLOGIC_CPUFREQ_BIN_PER_CLUSTER,
	},
	{}
};
MODULE_DEVICE_TABLE(of, amlogic_cpufreq_match_list);

static struct platform_driver amlogic_cpufreq_driver = {
	.probe = amlogic_cpufreq_probe,
	.remove = amlogic_cpufreq_remove,
	.driver = {
		.name = "amlogic-cpufreq",
		.of_match_table = amlogic_cpufreq_match_list,
	},
};
module_platform_driver(amlogic_cpufreq_driver);

MODULE_AUTHOR("Martin Blumenstingl <martin.blumenstingl@googlemail.com>");
MODULE_DESCRIPTION("Amlogic binned CPU frequency scaling driver");
MODULE_LICENSE("GPL");
