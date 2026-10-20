// SPDX-License-Identifier: GPL-2.0-only
/* Amlogic T7 HDMI receiver resource and cable detection bring-up. */
/* Copyright (c) 2019 Amlogic, Inc. All rights reserved. */
#include <linux/bitfield.h>
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/seq_file.h>

#include "t7-hdmirx.h"

/* TOP register indices use 32-bit words; EDID RAM uses byte offsets. */
#define T7_RX_TOP_SW_RESET	(0x000 * 4)
#define T7_RX_TOP_CLK_CNTL	(0x001 * 4)
#define T7_RX_TOP_HPD_PWR5V	(0x002 * 4)
#define T7_RX_TOP_PORT_SEL	(0x003 * 4)
#define T7_RX_TOP_EDID_GEN_CNTL	(0x004 * 4)
#define T7_RX_TOP_EDID_GEN_STAT	(0x008 * 4)
#define T7_RX_TOP_INTR_MASKN	(0x009 * 4)
#define T7_RX_TOP_VID_CNTL		(0x00c * 4)
#define T7_RX_TOP_VID_CNTL2	(0x013 * 4)
#define T7_RX_TOP_VID_STAT		(0x00d * 4)
#define T7_RX_TOP_OUTPUT_OVERRIDE	(0x090 * 4)
#define T7_RX_TOP_METER_HDMI_CNTL	(0x011 * 4)
#define T7_RX_TOP_METER_HDMI_STAT	(0x012 * 4)
#define T7_RX_TOP_CHAN_SWITCH_0	(0x028 * 4)
#define T7_RX_TOP_TMDS_ALIGN_STAT	(0x02b * 4)
#define T7_RX_TOP_INFILTER_I2C0	(0x02d * 4)
#define T7_RX_TOP_METER_CABLE_CNTL (0x03a * 4)
#define T7_RX_TOP_METER_CABLE_STAT (0x03b * 4)
#define T7_RX_TOP_CHAN_SWITCH_1	(0x03c * 4)
#define T7_RX_TOP_PHYIF_CNTL0	(0x080 * 4)
#define T7_RX_TOP_MISC_STAT0	(0x084 * 4)
#define T7_RX_METER_VALID		BIT(31)
#define T7_RX_METER_COUNT		GENMASK(23, 0)
#define T7_RX_METER_REF_CYCLES	8192
#define T7_RX_PHY_APLL0		0x00
#define T7_RX_PHY_APLL1		0x04
#define T7_RX_PHY_APLL2		0x08
#define T7_RX_PHY_APLL3		0x0c
#define T7_RX_PHY_APLL4		0x10
#define T7_RX_PHY_MISC0		0x14
#define T7_RX_PHY_MISC1		0x18
#define T7_RX_PHY_MISC2		0x1c
#define T7_RX_PHY_MISC3		0x20
#define T7_RX_PHY_DCHA0		0x24
#define T7_RX_PHY_DCHA1		0x28
#define T7_RX_PHY_DCHA2		0x2c
#define T7_RX_PHY_DCHA3		0x30
#define T7_RX_PHY_DCHD0		0x34
#define T7_RX_PHY_DCHD1		0x38
#define T7_RX_PHY_DCHD2		0x3c
#define T7_RX_PHY_DCHD3		0x40
#define T7_RX_PHY_DCHD4		0x44
#define T7_RX_PHY_DCHD_STAT	0x4c
#define T7_RX_PHY_PLL_LOCK		BIT(31)
#define T7_RX_TOP_5V_MASK		GENMASK(22, 20)
#define T7_RX_TOP_HPD_MASK	GENMASK(2, 0)
#define T7_RX_TOP_HPD_OVERRIDE	BIT(4)
#define T7_RX_TOP_HPD_REVERSE	BIT(5)
#define T7_RX_TOP_EDID_RESET	BIT(1)
#define T7_RX_EDID_BASE		0x1000
#define T7_RX_EDID_CAPACITY	512
#define T7_RX_EDID_BLOCK_SIZE	128

/* Receiver core registers are byte-addressed and require byte accesses. */
#define T7_RX_COR_AON_RESET	0x0005
#define T7_RX_COR_HDMI2_MODE	0x0040
#define T7_RX_COR_INT_CTRL		0x0079
#define T7_RX_COR_HPD_CTRL		0x00f5
#define T7_RX_COR_POWER_CTRL	0x1001
#define T7_RX_COR_POWER_RESET	0x1005
#define T7_RX_COR_STATE		0x1006
#define T7_RX_COR_SYS_CTRL		0x1007
#define T7_RX_COR_CHANNEL_MAP	0x100e
#define T7_RX_COR_DATA_INVERT	0x100f
#define T7_RX_COR_TMDS_CLOCK	0x1013
#define T7_RX_COR_HDMI_MODE	0x1022
#define T7_RX_COR_PREAMBLE		0x1023
#define T7_RX_COR_COLOR_DEPTH	0x1027
#define T7_RX_COR_FRL_CTRL		0x1028
#define T7_RX_COR_TEST_CLOCK	0x103b
#define T7_RX_COR_PWD_INT_CTRL	0x105f
#define T7_RX_COR_VIDEO_DIV	0x10c1
#define T7_RX_COR_CLAMP		0x10c4
#define T7_RX_COR_AV_MUTE		0x1122
#define T7_RX_COR_INPUT_FORMAT	0x1814
#define T7_RX_COR_INPUT_MAP	0x1816
#define T7_RX_COR_OUTPUT_SYNC	0x1842
#define T7_RX_COR_FRAME_STATUS	0x1881
#define T7_RX_COR_FRAME_TICKS	0x1888
#define T7_RX_COR_WIDTH		0x188c
#define T7_RX_COR_HEIGHT		0x188e
#define T7_RX_COR_HSYNC		0x1890
#define T7_RX_COR_HFRONT_BACK	0x1892
#define T7_RX_COR_VSYNC		0x1898
#define T7_RX_COR_VFRONT_BACK	0x189a
#define T7_RX_COR_DPLL_CONFIG	0x1f15
#define T7_RX_COR_DPLL_HDMI2	0x1f52
#define T7_RX_COR_PHY_CLOCK1	0x20a3
#define T7_RX_COR_PHY_CLOCK2	0x20a4
#define T7_RX_COR_PHY_CLOCK3	0x20a5

static char *edid_firmware;
module_param(edid_firmware, charp, 0400);
MODULE_PARM_DESC(edid_firmware, "Optional validated EDID file in /lib/firmware for DDC bring-up");

static bool phy_test;
module_param(phy_test, bool, 0400);
MODULE_PARM_DESC(phy_test, "Opt-in 148.5 MHz input PLL/equalizer trial; requires EDID firmware");

static bool core_test;
module_param(core_test, bool, 0400);
MODULE_PARM_DESC(core_test, "Initialize the TMDS video core; requires phy_test=1");

static const u32 t7_hdmirx_phy_top_regs[] = {
	T7_RX_TOP_CLK_CNTL, T7_RX_TOP_PORT_SEL, T7_RX_TOP_PHYIF_CNTL0,
	T7_RX_TOP_CHAN_SWITCH_0, T7_RX_TOP_CHAN_SWITCH_1,
	T7_RX_TOP_METER_HDMI_CNTL, T7_RX_TOP_METER_CABLE_CNTL,
	T7_RX_TOP_VID_CNTL, T7_RX_TOP_VID_CNTL2, T7_RX_TOP_OUTPUT_OVERRIDE,
};

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
	void __iomem *phy;
	void __iomem *cor;
	struct clk_bulk_data clocks[T7_RX_NUM_CLOCKS];
	u32 port;
	u32 saved_reset;
	u32 saved_port_sel;
	bool ddc_active;
	bool phy_active;
	bool core_active;
	unsigned long core_smc_result;
	u32 phy_top_saved[ARRAY_SIZE(t7_hdmirx_phy_top_regs)];
	u32 cable_hz;
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

static int t7_hdmirx_validate_phy_edid(const u8 *edid, size_t size)
{
	int ret = t7_hdmirx_validate_edid(edid, size);

	if (ret)
		return ret;
	/* No CTA/SCDC negotiation: 594 MHz at 1:40 also presents a 148.5 MHz clock. */
	if (size != T7_RX_EDID_BLOCK_SIZE || !(edid[20] & BIT(7)) ||
	    (edid[54] | (edid[55] << 8)) != 14850)
		return -EOPNOTSUPP;
	return 0;
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

static u32 t7_hdmirx_meter_hz(u32 status, unsigned long reference_hz)
{
	u64 frequency;

	if (!(status & T7_RX_METER_VALID))
		return 0;
	frequency = div_u64((u64)FIELD_GET(T7_RX_METER_COUNT, status) * reference_hz,
			    T7_RX_METER_REF_CYCLES);
	return frequency <= U32_MAX ? frequency : 0;
}

static void t7_hdmirx_phy_update(struct t7_hdmirx *rx, u32 reg, u32 mask, u32 value)
{
	u32 data = readl(rx->phy + reg);

	writel((data & ~mask) | (value & mask), rx->phy + reg);
}

static void t7_hdmirx_phy_stop(void *data)
{
	struct t7_hdmirx *rx = data;
	unsigned int i;

	if (!rx->phy_active)
		return;
	t7_hdmirx_set_hpd(rx, false);
	/* Power down the PLL, termination and data channels before gating clocks. */
	t7_hdmirx_phy_update(rx, T7_RX_PHY_APLL0, GENMASK(29, 28), BIT(29));
	writel(0x00800800, rx->phy + T7_RX_PHY_MISC0);
	/* Keep the boot-provided termination trim for the next probe. */
	t7_hdmirx_phy_update(rx, T7_RX_PHY_MISC2, BIT(29) | BIT(30), BIT(29) | BIT(30));
	writel(0, rx->phy + T7_RX_PHY_MISC3);
	writel(0x10000000, rx->phy + T7_RX_PHY_DCHA0);
	for (i = T7_RX_PHY_DCHA1; i <= T7_RX_PHY_DCHD4; i += 4)
		writel(0, rx->phy + i);
	readl(rx->phy + T7_RX_PHY_DCHD4);
	for (i = 0; i < ARRAY_SIZE(t7_hdmirx_phy_top_regs); i++)
		writel(rx->phy_top_saved[i], rx->top + t7_hdmirx_phy_top_regs[i]);
	rx->phy_active = false;
}

static void t7_hdmirx_phy_prepare(struct t7_hdmirx *rx)
{
	u32 trim = readl(rx->phy + T7_RX_PHY_MISC1);
	u32 misc0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(t7_hdmirx_phy_top_regs); i++)
		rx->phy_top_saved[i] = readl(rx->top + t7_hdmirx_phy_top_regs[i]);
	rx->phy_active = true;
	writel(0, rx->top + T7_RX_TOP_CLK_CNTL);
	writel((rx->saved_port_sel & ~GENMASK(2, 0)) | BIT(4) | BIT(rx->port),
	       rx->top + T7_RX_TOP_PORT_SEL);
	writel(BIT(1), rx->top + T7_RX_TOP_PHYIF_CNTL0);
	writel(0x24000001, rx->top + T7_RX_TOP_CHAN_SWITCH_0);
	writel(0x03000007, rx->top + T7_RX_TOP_CHAN_SWITCH_1);
	writel(BIT(28) | T7_RX_METER_REF_CYCLES, rx->top + T7_RX_TOP_METER_CABLE_CNTL);
	writel(BIT(28) | T7_RX_METER_REF_CYCLES, rx->top + T7_RX_TOP_METER_HDMI_CNTL);
	/* Discard meter state left by the previous receiver instance. */
	writel(BIT(5) | BIT(6), rx->top + T7_RX_TOP_SW_RESET);
	readl(rx->top + T7_RX_TOP_SW_RESET);
	writel(0, rx->top + T7_RX_TOP_SW_RESET);
	/* Fixed PHY_BW_2 analog settings; other input rates must not program APLL. */
	writel(0x200773f8, rx->phy + T7_RX_PHY_MISC0);
	usleep_range(5, 10);
	if (trim & BIT(0))
		trim = (0x000c01c0 & ~(GENMASK(15, 12) | BIT(0))) |
		       (trim & (GENMASK(15, 12) | BIT(0)));
	else
		trim = 0x000c01c0;
	writel(trim, rx->phy + T7_RX_PHY_MISC1);
	writel(0, rx->phy + T7_RX_PHY_MISC2);
	writel(0x817, rx->phy + T7_RX_PHY_MISC3);
	writel(0x17270fff, rx->phy + T7_RX_PHY_DCHA0);
	writel(0x45ff4b58, rx->phy + T7_RX_PHY_DCHA1);
	writel(0x01ff28c9, rx->phy + T7_RX_PHY_DCHA2);
	writel(0x630, rx->phy + T7_RX_PHY_DCHA3);
	writel(0xe3372103 & ~(BIT(28) | GENMASK(25, 24)), rx->phy + T7_RX_PHY_DCHD0);
	writel(0x40009000, rx->phy + T7_RX_PHY_DCHD1);
	writel(0x0d422222 & ~BIT(26), rx->phy + T7_RX_PHY_DCHD2);
	writel(0x10000000, rx->phy + T7_RX_PHY_DCHD3);
	writel(0x3001, rx->phy + T7_RX_PHY_DCHD4);
	writel(0x817 | (BIT(rx->port) << 6), rx->phy + T7_RX_PHY_MISC3);
	usleep_range(5, 10);
	misc0 = (0x200773f8 & ~(BIT(10) | GENMASK(9, 7))) | BIT(rx->port);
	writel(misc0, rx->phy + T7_RX_PHY_MISC0);
	usleep_range(5, 10);
	writel(misc0 | BIT(10) | GENMASK(9, 7), rx->phy + T7_RX_PHY_MISC0);
}

static int t7_hdmirx_wait_cable(struct t7_hdmirx *rx, unsigned long reference_hz)
{
	u32 previous = 0, sample_hz, status;
	unsigned int i, stable = 0;

	for (i = 0; i < 150; i++) {
		if (!(readl(rx->top + T7_RX_TOP_HPD_PWR5V) & BIT(20 + rx->port)))
			return -ENOLINK;
		status = readl(rx->top + T7_RX_TOP_METER_CABLE_STAT);
		sample_hz = t7_hdmirx_meter_hz(status, reference_hz);
		rx->cable_hz = sample_hz;
		/* This trial only supports 1080p60 RGB 8-bit's 148.5 MHz TMDS clock. */
		if (sample_hz >= 146500000 && sample_hz <= 150500000 && previous &&
		    abs((int)sample_hz - (int)previous) <= 500000)
			stable++;
		else
			stable = 0;
		if (stable == 3)
			return 0;
		previous = sample_hz;
		usleep_range(20000, 21000);
	}
	return -ETIMEDOUT;
}

static int t7_hdmirx_phy_pll(struct t7_hdmirx *rx)
{
	unsigned int attempt;

	/* PHY_BW_2 / PLL_BW_2: M=40, N=1, TMDS output divide by 8. */
	for (attempt = 0; attempt < 3; attempt++) {
		writel(0x20090428, rx->phy + T7_RX_PHY_APLL0);
		usleep_range(5, 10);
		writel(0x30090428, rx->phy + T7_RX_PHY_APLL0);
		usleep_range(5, 10);
		writel(0, rx->phy + T7_RX_PHY_APLL1);
		writel(0x00001118, rx->phy + T7_RX_PHY_APLL2);
		usleep_range(5, 10);
		writel(0x10058f30, rx->phy + T7_RX_PHY_APLL3);
		writel(0x0b0130c0, rx->phy + T7_RX_PHY_APLL4);
		usleep_range(5, 10);
		writel(0x0b8130c0, rx->phy + T7_RX_PHY_APLL4);
		usleep_range(5, 10);
		writel(0x34090428, rx->phy + T7_RX_PHY_APLL0);
		usleep_range(5, 10);
		writel(0x14090428, rx->phy + T7_RX_PHY_APLL0);
		usleep_range(5, 10);
		writel(0x0000301c, rx->phy + T7_RX_PHY_APLL2);
		usleep_range(100, 110);
		if (readl(rx->phy + T7_RX_PHY_APLL0) & T7_RX_PHY_PLL_LOCK)
			return 0;
	}
	return -ETIMEDOUT;
}

static void t7_hdmirx_phy_equalize(struct t7_hdmirx *rx)
{
	/* 10-bit CDR mode, then release the CDR/EQ reset and free-running gate. */
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD0, BIT(31) | GENMASK(2, 0), BIT(31) | 3);
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD0,
			     BIT(30) | BIT(28) | GENMASK(25, 24), 0);
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD2, BIT(31) | BIT(26), 0);
	usleep_range(100, 110);
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD0, GENMASK(25, 24), GENMASK(25, 24));
	usleep_range(100, 110);
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD0, BIT(30), BIT(30));
	usleep_range(10000, 10100);
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD2, BIT(27), BIT(27));
	usleep_range(10, 20);
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD2, BIT(26), BIT(26));
	usleep_range(100, 110);
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD0, BIT(28), BIT(28));
}

static void t7_hdmirx_core_stop(void *data)
{
	struct t7_hdmirx *rx = data;

	if (!rx->core_active)
		return;
	t7_hdmirx_set_hpd(rx, false);
	writeb(0x1a, rx->cor + 0x1005);
	writeb(1, rx->cor + T7_RX_COR_AON_RESET);
	writeb(1, rx->cor + T7_RX_COR_POWER_CTRL);
	writeb(0, rx->cor + T7_RX_COR_SYS_CTRL);
	writeb(0, rx->cor + T7_RX_COR_PHY_CLOCK1);
	writeb(0, rx->cor + T7_RX_COR_PHY_CLOCK2);
	writeb(0, rx->cor + T7_RX_COR_PHY_CLOCK3);
	readb(rx->cor + T7_RX_COR_PHY_CLOCK3);
	rx->core_active = false;
}

static int t7_hdmirx_core_start(struct t7_hdmirx *rx)
{
	struct arm_smccc_res res;

	/* T7's firmware owns TOP_SECURE_MODE, including non-secure core access. */
	arm_smccc_smc(0x8200008c, 0xa2, 0x1f, 0, 0, 0, 0, 0, &res);
	rx->core_smc_result = res.a0;
	if (res.a0)
		return -EIO;
	rx->core_active = true;
	/* Hold AON reset while removing TMDS isolation and selecting PHY lanes. */
	writeb(1, rx->cor + T7_RX_COR_AON_RESET);
	writeb(0x11, rx->cor + T7_RX_COR_HDMI2_MODE);
	writeb(0, rx->cor + T7_RX_COR_INT_CTRL);
	writeb(0, rx->cor + T7_RX_COR_POWER_CTRL);
	writeb(3, rx->cor + T7_RX_COR_SYS_CTRL);
	writeb(0x12, rx->cor + T7_RX_COR_CHANNEL_MAP);
	writeb(0x52, rx->cor + T7_RX_COR_DATA_INVERT);
	writeb(0x20, rx->cor + T7_RX_COR_TMDS_CLOCK);
	writeb(0x80, rx->cor + T7_RX_COR_TEST_CLOCK);
	writeb(1, rx->cor + T7_RX_COR_VIDEO_DIV);
	/* Native single-rate RGB input and video component mapping. */
	writeb(0, rx->cor + T7_RX_COR_INPUT_FORMAT);
	writeb(0, rx->cor + T7_RX_COR_INPUT_FORMAT + 1);
	writeb(0x40, rx->cor + T7_RX_COR_INPUT_MAP);
	writeb(4, rx->cor + T7_RX_COR_INPUT_MAP + 1);
	writeb(0, rx->cor + T7_RX_COR_OUTPUT_SYNC);
	writeb(4, rx->cor + T7_RX_COR_HDMI_MODE);
	writeb(1, rx->cor + T7_RX_COR_HPD_CTRL);
	writeb(0, rx->cor + T7_RX_COR_CLAMP);
	/* DVI sources have no HDMI guard-band preamble. */
	writeb(0, rx->cor + T7_RX_COR_PREAMBLE);
	writeb(0xff, rx->cor + T7_RX_COR_PHY_CLOCK1);
	writeb(0x3f, rx->cor + T7_RX_COR_PHY_CLOCK2);
	writeb(1, rx->cor + T7_RX_COR_PHY_CLOCK3);
	writeb(0, rx->cor + T7_RX_COR_AON_RESET);
	writeb(0, rx->cor + T7_RX_COR_PWD_INT_CTRL);
	/* TMDS DPLL, no FRL, and video/deep-colour reset pulse. */
	writeb(0x10, rx->cor + T7_RX_COR_DPLL_CONFIG);
	writeb(0, rx->cor + T7_RX_COR_FRL_CTRL);
	writeb(0, rx->cor + T7_RX_COR_DPLL_HDMI2);
	writeb(0x20, rx->cor + T7_RX_COR_AV_MUTE);
	writeb(0x1a, rx->cor + T7_RX_COR_POWER_RESET);
	usleep_range(100, 110);
	writeb(1, rx->cor + T7_RX_COR_POWER_RESET);
	writel(BIT(21) | BIT(20) | BIT(7), rx->top + T7_RX_TOP_VID_CNTL);
	writel(0x0a, rx->top + T7_RX_TOP_VID_CNTL2);
	writel(0, rx->top + T7_RX_TOP_OUTPUT_OVERRIDE);
	/* Re-align the PCS after lane and video setup; lock alone is insufficient. */
	writel(BIT(13) | BIT(7), rx->top + T7_RX_TOP_SW_RESET);
	readl(rx->top + T7_RX_TOP_SW_RESET);
	usleep_range(1, 5);
	writel(0, rx->top + T7_RX_TOP_SW_RESET);
	if (readb(rx->cor + T7_RX_COR_SYS_CTRL) != 3 ||
	    readb(rx->cor + T7_RX_COR_PHY_CLOCK1) != 0xff)
		return -EIO;
	return 0;
}

static u32 t7_hdmirx_core_read16(struct t7_hdmirx *rx, u32 offset)
{
	return readb(rx->cor + offset) | (readb(rx->cor + offset + 1) << 8);
}

int t7_hdmirx_check_1080p(struct device *dev)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);
	u32 ticks, reference;
	unsigned int i;

	if (!rx || !rx->core_active)
		return -ENOLINK;
	for (i = 0; i < 2; i++) {
		if (!(readl(rx->top + T7_RX_TOP_HPD_PWR5V) & BIT(20 + rx->port)) ||
		    !(readl(rx->top + T7_RX_TOP_TMDS_ALIGN_STAT) & BIT(0)))
			return -ENOLINK;
		if (readb(rx->cor + T7_RX_COR_FRAME_STATUS) & BIT(2))
			return -ERANGE;
		if (readl(rx->top + T7_RX_TOP_VID_STAT) & GENMASK(23, 20) ||
		    readb(rx->cor + T7_RX_COR_COLOR_DEPTH) > 4)
			return -ERANGE;
		if (t7_hdmirx_core_read16(rx, T7_RX_COR_WIDTH) != 1920 ||
		    t7_hdmirx_core_read16(rx, T7_RX_COR_HEIGHT) != 1080 ||
		    t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC) +
		    t7_hdmirx_core_read16(rx, T7_RX_COR_HFRONT_BACK) != 2200 ||
		    t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC) +
		    t7_hdmirx_core_read16(rx, T7_RX_COR_VFRONT_BACK) != 1125)
			return -ERANGE;
		ticks = t7_hdmirx_core_read16(rx, T7_RX_COR_FRAME_TICKS) |
			(readb(rx->cor + T7_RX_COR_FRAME_TICKS + 2) << 16);
		reference = clk_get_rate(rx->clocks[T7_RX_PCLK].clk);
		if (!ticks || ticks < reference / 61 || ticks > reference / 59)
			return -ERANGE;
		if (!i)
			usleep_range(20000, 21000);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(t7_hdmirx_check_1080p);

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
	seq_printf(s, "phy_active: %u\n", rx->phy_active);
	seq_printf(s, "core_active: %u\n", rx->core_active);
	if (rx->core_active) {
		u32 ticks = t7_hdmirx_core_read16(rx, T7_RX_COR_FRAME_TICKS) |
			    (readb(rx->cor + T7_RX_COR_FRAME_TICKS + 2) << 16);

		seq_printf(s, "core_state: %#x\n", readb(rx->cor + T7_RX_COR_STATE));
		seq_printf(s, "core_smc_result: %#lx\n", rx->core_smc_result);
		seq_printf(s, "core_sys_ctrl: %#x\n", readb(rx->cor + T7_RX_COR_SYS_CTRL));
		seq_printf(s, "core_phyck: %#x\n", readb(rx->cor + T7_RX_COR_PHY_CLOCK1));
		seq_printf(s, "core_aon_reset: %#x\n", readb(rx->cor + T7_RX_COR_AON_RESET));
		seq_printf(s, "core_fdet: %#x\n", readb(rx->cor + T7_RX_COR_FRAME_STATUS));
		seq_printf(s, "color_depth_code: %#x\n", readb(rx->cor + T7_RX_COR_COLOR_DEPTH));
		seq_printf(s, "width: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_WIDTH));
		seq_printf(s, "height: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_HEIGHT));
		seq_printf(s, "htotal: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC) +
			   t7_hdmirx_core_read16(rx, T7_RX_COR_HFRONT_BACK));
		seq_printf(s, "vtotal: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC) +
			   t7_hdmirx_core_read16(rx, T7_RX_COR_VFRONT_BACK));
		seq_printf(s, "frame_ticks: %u\n", ticks);
	}
	if (rx->phy_active) {
		u32 cable = readl(rx->top + T7_RX_TOP_METER_CABLE_STAT);
		u32 tmds = readl(rx->top + T7_RX_TOP_METER_HDMI_STAT);
		unsigned long reference = clk_get_rate(rx->clocks[T7_RX_METER].clk);

		seq_printf(s, "cable_meter: %#010x\n", cable);
		seq_printf(s, "tmds_meter: %#010x\n", tmds);
		seq_printf(s, "cable_hz: %u\n", t7_hdmirx_meter_hz(cable, reference));
		seq_printf(s, "tmds_hz: %u\n", t7_hdmirx_meter_hz(tmds, reference));
		seq_printf(s, "phy_pll_locked: %u\n",
			   !!(readl(rx->phy + T7_RX_PHY_APLL0) & T7_RX_PHY_PLL_LOCK));
		seq_printf(s, "phy_sqo: %u\n", readl(rx->top + T7_RX_TOP_MISC_STAT0) & 1);
		seq_printf(s, "tmds_aligned: %u\n",
			   readl(rx->top + T7_RX_TOP_TMDS_ALIGN_STAT) & 1);
	}
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

	if (phy_test && !edid_firmware)
		return dev_err_probe(dev, -EINVAL, "PHY trial requires an EDID firmware\n");
	if (core_test && !phy_test)
		return dev_err_probe(dev, -EINVAL, "Core trial requires PHY initialization\n");
	rx = devm_kzalloc(dev, sizeof(*rx), GFP_KERNEL);
	if (!rx)
		return -ENOMEM;
	ret = device_property_read_u32(dev, "amlogic,input-port", &rx->port);
	if (ret || rx->port > 2)
		return dev_err_probe(dev, -EINVAL, "Require input port 0..2\n");
	rx->top = devm_platform_ioremap_resource_byname(pdev, "top");
	if (IS_ERR(rx->top))
		return PTR_ERR(rx->top);
	rx->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(rx->phy))
		return PTR_ERR(rx->phy);
	rx->cor = devm_platform_ioremap_resource_byname(pdev, "cor");
	if (IS_ERR(rx->cor))
		return PTR_ERR(rx->cor);
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
		ret = phy_test ? t7_hdmirx_validate_phy_edid(firmware->data, firmware->size) : 0;
		if (!ret)
			ret = t7_hdmirx_ddc_start(rx, firmware->data, firmware->size);
		release_firmware(firmware);
		if (ret)
			return dev_err_probe(dev, ret, "Cannot publish EDID\n");
	}
	if (phy_test) {
		ret = devm_add_action_or_reset(dev, t7_hdmirx_phy_stop, rx);
		if (ret)
			return ret;
		t7_hdmirx_phy_prepare(rx);
		ret = t7_hdmirx_wait_cable(rx, clk_get_rate(rx->clocks[T7_RX_METER].clk));
		if (!ret)
			ret = t7_hdmirx_phy_pll(rx);
		if (ret)
			return dev_err_probe(dev, ret, "PHY trial failed, cable %u Hz, APLL %#x\n",
					     rx->cable_hz, readl(rx->phy + T7_RX_PHY_APLL0));
		t7_hdmirx_phy_equalize(rx);
	}
	if (core_test) {
		ret = devm_add_action_or_reset(dev, t7_hdmirx_core_stop, rx);
		if (ret)
			return ret;
		ret = t7_hdmirx_core_start(rx);
		if (ret)
			return dev_err_probe(dev, ret, "Cannot initialize receiver core\n");
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

static int __init t7_hdmirx_init(void)
{
	/* A faulted VDIN consumer must retain its supplier until reboot. */
	t7_hdmirx_driver.driver.suppress_bind_attrs = core_test;
	return platform_driver_register(&t7_hdmirx_driver);
}
module_init(t7_hdmirx_init);

static void __exit t7_hdmirx_exit(void)
{
	platform_driver_unregister(&t7_hdmirx_driver);
}
module_exit(t7_hdmirx_exit);

MODULE_DESCRIPTION("Amlogic T7 HDMI receiver bring-up");
MODULE_LICENSE("GPL");
