// SPDX-License-Identifier: GPL-2.0-only
/* Amlogic T7 HDMI receiver resource and cable detection bring-up. */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/seq_file.h>

/* TOP register indices use 32-bit words; EDID RAM uses byte offsets. */
#define T7_RX_TOP_SW_RESET	(0x000 * 4)
#define T7_RX_TOP_CLK_CNTL	(0x001 * 4)
#define T7_RX_TOP_HPD_PWR5V	(0x002 * 4)
#define T7_RX_TOP_PORT_SEL	(0x003 * 4)
#define T7_RX_TOP_5V_MASK		GENMASK(22, 20)
#define T7_RX_TOP_HPD_MASK	GENMASK(2, 0)

enum t7_hdmirx_clock {
	T7_RX_PCLK,
	T7_RX_CFG,
	T7_RX_METER,
	T7_RX_2M,
	T7_RX_5M,
	T7_RX_NUM_CLOCKS,
};

struct t7_hdmirx {
	void __iomem *top;
	struct clk_bulk_data clocks[T7_RX_NUM_CLOCKS];
	u32 port;
};

static int t7_hdmirx_status(struct seq_file *s, void *unused)
{
	struct device *dev = s->private;
	struct t7_hdmirx *rx = dev_get_drvdata(dev);
	u32 status = readl(rx->top + T7_RX_TOP_HPD_PWR5V);
	unsigned int i;

	seq_printf(s, "port: %u\n", rx->port);
	seq_printf(s, "source_5v: %u\n",
		   !!(FIELD_GET(T7_RX_TOP_5V_MASK, status) & BIT(rx->port)));
	/* This is the register request, not proof of the physical HPD level. */
	seq_printf(s, "hpd_request: %u\n",
		   !!(FIELD_GET(T7_RX_TOP_HPD_MASK, status) & BIT(rx->port)));
	seq_printf(s, "top_hpd_pwr5v: %#010x\n", status);
	seq_printf(s, "top_sw_reset: %#010x\n",
		   readl(rx->top + T7_RX_TOP_SW_RESET));
	seq_printf(s, "top_clk_cntl: %#010x\n",
		   readl(rx->top + T7_RX_TOP_CLK_CNTL));
	seq_printf(s, "top_port_sel: %#010x\n",
		   readl(rx->top + T7_RX_TOP_PORT_SEL));
	for (i = 0; i < ARRAY_SIZE(rx->clocks); i++)
		seq_printf(s, "%s_hz: %lu\n", rx->clocks[i].id,
			   clk_get_rate(rx->clocks[i].clk));
	return 0;
}

static void t7_hdmirx_power_put(void *data)
{
	pm_runtime_put_sync(data);
}

static void t7_hdmirx_clocks_disable(void *data)
{
	struct t7_hdmirx *rx = data;

	clk_bulk_disable_unprepare(ARRAY_SIZE(rx->clocks), rx->clocks);
}

static void t7_hdmirx_debugfs_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static int t7_hdmirx_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "pclk", "cfg", "meter", "2m", "5m" };
	static const unsigned long rates[] = { 0, 50000000, 24000000, 2000000, 5000000 };
	struct device *dev = &pdev->dev;
	struct dentry *debugfs;
	struct t7_hdmirx *rx;
	unsigned int i;
	int ret;

	rx = devm_kzalloc(dev, sizeof(*rx), GFP_KERNEL);
	if (!rx)
		return -ENOMEM;
	ret = device_property_read_u32(dev, "amlogic,input-port", &rx->port);
	if (ret || rx->port > 2)
		return dev_err_probe(dev, -EINVAL, "Require input port 0..2\n");
	rx->top = devm_platform_ioremap_resource_byname(pdev, "top");
	if (IS_ERR(rx->top))
		return PTR_ERR(rx->top);
	for (i = 0; i < ARRAY_SIZE(rx->clocks); i++)
		rx->clocks[i].id = names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(rx->clocks), rx->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot acquire receiver clocks\n");
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Cannot power receiver\n");
	ret = devm_add_action_or_reset(dev, t7_hdmirx_power_put, dev);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(rx->clocks); i++) {
		if (!rates[i])
			continue;
		ret = clk_set_rate(rx->clocks[i].clk, rates[i]);
		if (ret)
			return dev_err_probe(dev, ret, "Cannot set %s clock\n", names[i]);
	}
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(rx->clocks), rx->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot enable receiver clocks\n");
	ret = devm_add_action_or_reset(dev, t7_hdmirx_clocks_disable, rx);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, rx);
	/* Read-only bring-up: keep HPD, EDID, IRQ and DMA state untouched. */
	debugfs = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(debugfs))
		return PTR_ERR(debugfs);
	debugfs_create_devm_seqfile(dev, "status", debugfs, t7_hdmirx_status);
	/* Remove files before their managed callback context or clocks disappear. */
	ret = devm_add_action_or_reset(dev, t7_hdmirx_debugfs_remove, debugfs);
	if (ret)
		return ret;
	dev_info(dev, "receiver resources ready, input port %u; capture not implemented\n",
		 rx->port);
	return 0;
}

static const struct of_device_id t7_hdmirx_match[] = {
	{ .compatible = "amlogic,t7-hdmirx" },
	{ }
};
MODULE_DEVICE_TABLE(of, t7_hdmirx_match);

static struct platform_driver t7_hdmirx_driver = {
	.probe = t7_hdmirx_probe,
	.driver = {
		.name = "t7-hdmirx",
		.of_match_table = t7_hdmirx_match,
	},
};
module_platform_driver(t7_hdmirx_driver);

MODULE_DESCRIPTION("Amlogic T7 HDMI receiver bring-up");
MODULE_LICENSE("GPL");
