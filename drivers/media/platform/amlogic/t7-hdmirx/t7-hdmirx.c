// SPDX-License-Identifier: GPL-2.0-only
/* Amlogic T7 HDMI receiver resource and cable detection bring-up. */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
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
#define T7_RX_TOP_EDID_GEN_CNTL	(0x004 * 4)
#define T7_RX_TOP_EDID_GEN_STAT	(0x008 * 4)
#define T7_RX_TOP_INTR_MASKN	(0x009 * 4)
#define T7_RX_TOP_INFILTER_I2C0	(0x02d * 4)
#define T7_RX_TOP_5V_MASK		GENMASK(22, 20)
#define T7_RX_TOP_HPD_MASK	GENMASK(2, 0)
#define T7_RX_TOP_HPD_OVERRIDE	BIT(4)
#define T7_RX_TOP_HPD_REVERSE	BIT(5)
#define T7_RX_TOP_EDID_RESET	BIT(1)
#define T7_RX_EDID_BASE		0x1000
#define T7_RX_EDID_CAPACITY	512
#define T7_RX_EDID_BLOCK_SIZE	128

static char *edid_firmware;
module_param(edid_firmware, charp, 0400);
MODULE_PARM_DESC(edid_firmware, "Optional validated EDID file in /lib/firmware for DDC bring-up");

enum t7_hdmirx_clock {
	T7_RX_PCLK,
	T7_RX_SYS,
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
	u32 saved_reset;
	u32 saved_port_sel;
	bool ddc_active;
	u8 edid[T7_RX_EDID_CAPACITY];
	struct debugfs_blob_wrapper edid_blob;
};

static int t7_hdmirx_validate_edid(const u8 *edid, size_t size)
{
	static const u8 header[] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	unsigned int block, i;
	u8 sum;

	if (size > T7_RX_EDID_CAPACITY)
		return -E2BIG;
	if (size < T7_RX_EDID_BLOCK_SIZE || size % T7_RX_EDID_BLOCK_SIZE)
		return -EINVAL;
	if (memcmp(edid, header, sizeof(header)) ||
	    edid[126] != size / T7_RX_EDID_BLOCK_SIZE - 1)
		return -EINVAL;
	for (block = 0; block < size; block += T7_RX_EDID_BLOCK_SIZE) {
		sum = 0;
		for (i = 0; i < T7_RX_EDID_BLOCK_SIZE; i++)
			sum += edid[block + i];
		if (sum)
			return -EINVAL;
	}
	return 0;
}

static void t7_hdmirx_set_hpd(struct t7_hdmirx *rx, bool high)
{
	u32 value = readl(rx->top + T7_RX_TOP_HPD_PWR5V);

	/* T7's pin control uses the same override/reverse polarity as the RX TOP. */
	value |= T7_RX_TOP_HPD_OVERRIDE | T7_RX_TOP_HPD_REVERSE;
	value &= ~BIT(rx->port);
	if (high)
		value |= BIT(rx->port);
	writel(value, rx->top + T7_RX_TOP_HPD_PWR5V);
	readl(rx->top + T7_RX_TOP_HPD_PWR5V);
}

static void t7_hdmirx_ddc_stop(void *data)
{
	struct t7_hdmirx *rx = data;

	if (!rx->ddc_active)
		return;
	t7_hdmirx_set_hpd(rx, false);
	writel(rx->saved_port_sel, rx->top + T7_RX_TOP_PORT_SEL);
	writel(rx->saved_reset, rx->top + T7_RX_TOP_SW_RESET);
	readl(rx->top + T7_RX_TOP_SW_RESET);
	rx->edid_blob.size = 0;
	rx->ddc_active = false;
}

static int t7_hdmirx_ddc_start(struct t7_hdmirx *rx, const u8 *edid, size_t size)
{
	unsigned int base = T7_RX_EDID_BASE + rx->port * T7_RX_EDID_CAPACITY;
	unsigned int i;
	u8 expected;
	int ret;

	ret = t7_hdmirx_validate_edid(edid, size);
	if (ret)
		return ret;
	rx->saved_reset = readl(rx->top + T7_RX_TOP_SW_RESET);
	rx->saved_port_sel = readl(rx->top + T7_RX_TOP_PORT_SEL);
	rx->ddc_active = true;
	t7_hdmirx_set_hpd(rx, false);
	/* Give the source a real disconnect interval before replacing its EDID. */
	msleep(120);
	writel(0, rx->top + T7_RX_TOP_INTR_MASKN);
	writel(rx->saved_reset | T7_RX_TOP_EDID_RESET, rx->top + T7_RX_TOP_SW_RESET);
	writel(GENMASK(16, 13) | 9, rx->top + T7_RX_TOP_EDID_GEN_CNTL);
	writel(BIT(29) | BIT(16) | BIT(13) | BIT(0),
	       rx->top + T7_RX_TOP_INFILTER_I2C0 + rx->port * 4);
	/* EDID RAM is byte-addressed, unlike the word-indexed TOP registers. */
	for (i = 0; i < T7_RX_EDID_CAPACITY; i++)
		writeb(i < size ? edid[i] : 0, rx->top + base + i);
	for (i = 0; i < T7_RX_EDID_CAPACITY; i++) {
		expected = i < size ? edid[i] : 0;
		if (readb(rx->top + base + i) != expected) {
			t7_hdmirx_ddc_stop(rx);
			return -EIO;
		}
	}
	/* T7 DDC requires the TOP reset bank released, as in the hardware init. */
	writel(0, rx->top + T7_RX_TOP_SW_RESET);
	readl(rx->top + T7_RX_TOP_SW_RESET);
	usleep_range(1000, 2000);
	/* Enable the TOP port path used by T7's receiver initialization. */
	writel(rx->saved_port_sel | BIT(4), rx->top + T7_RX_TOP_PORT_SEL);
	memcpy(rx->edid, edid, size);
	rx->edid_blob.data = rx->edid;
	rx->edid_blob.size = size;
	t7_hdmirx_set_hpd(rx, true);
	return 0;
}

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
	seq_printf(s, "edid_bytes: %lu\n", rx->edid_blob.size);
	seq_printf(s, "top_edid_gen_stat: %#010x\n",
		   readl(rx->top + T7_RX_TOP_EDID_GEN_STAT));
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
	static const char * const names[] = { "pclk", "sys", "cfg", "meter", "2m", "5m" };
	static const unsigned long rates[] = { 0, 0, 50000000, 24000000, 2000000, 5000000 };
	struct device *dev = &pdev->dev;
	struct dentry *debugfs;
	struct t7_hdmirx *rx;
	const struct firmware *firmware;
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
	ret = devm_add_action_or_reset(dev, t7_hdmirx_ddc_stop, rx);
	if (ret)
		return ret;
	if (edid_firmware) {
		ret = request_firmware(&firmware, edid_firmware, dev);
		if (ret)
			return dev_err_probe(dev, ret, "Cannot load EDID firmware\n");
		ret = t7_hdmirx_ddc_start(rx, firmware->data, firmware->size);
		release_firmware(firmware);
		if (ret)
			return dev_err_probe(dev, ret, "Cannot publish EDID\n");
	}
	debugfs = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(debugfs))
		return PTR_ERR(debugfs);
	debugfs_create_devm_seqfile(dev, "status", debugfs, t7_hdmirx_status);
	debugfs_create_blob("edid", 0400, debugfs, &rx->edid_blob);
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
