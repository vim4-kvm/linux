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
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/videodev2.h>
#include <linux/v4l2-dv-timings.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/soc.h>

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
#define T7_RX_COR_DDC_SWITCH	0x0009
#define T7_RX_COR_SINK_VERSION	0x0301
#define T7_RX_COR_TMDS_CONFIG	0x0320
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
#define T7_RX_COR_HSYNC_LOW	0x1890
#define T7_RX_COR_HSYNC_HIGH	0x1892
#define T7_RX_COR_HFRONT		0x1894
#define T7_RX_COR_HBACK		0x1896
#define T7_RX_COR_VSYNC_LOW	0x1898
#define T7_RX_COR_VSYNC_HIGH	0x189a
#define T7_RX_COR_VFRONT		0x189c
#define T7_RX_COR_VBACK		0x189e
#define T7_RX_COR_DPLL_CONFIG	0x1f15
#define T7_RX_COR_DPLL_HDMI2	0x1f52
#define T7_RX_COR_PHY_CLOCK1	0x20a3
#define T7_RX_COR_PHY_CLOCK2	0x20a4
#define T7_RX_COR_PHY_CLOCK3	0x20a5

#define T7_RX_TOP_ACR_CTRL	(0x00e * 4)
#define T7_RX_TOP_ACR_CTRL2	(0x043 * 4)
#define T7_RX_AUD_PLL0		0x80
#define T7_RX_AUD_PLL2		0x84
#define T7_RX_AUD_PLL3		0x88
#define T7_RX_AUD_PLL_STATUS	0x8c
#define T7_RX_COR_CTS		0x111a
#define T7_RX_COR_N		0x111d
#define T7_RX_COR_AUDIO_PACKET	0x1127
#define T7_RX_COR_AUDIO_INFO	0x1184
#define T7_RX_COR_AUDIO_AIF	0x1180
#define T7_RX_COR_AUDIO_CHSTATUS	0x1419
#define T7_RX_COR_AUDIO_MCLK	0x1401
#define T7_RX_COR_AUDIO_TDM1	0x1440
#define T7_RX_COR_AUDIO_TDM2	0x1441
#define T7_RX_COR_AVI_PACKET	0x1140
#define T7_RX_COR_UNREC_CTRL	0x115e
#define T7_RX_COR_UNREC_TYPE	0x115f
#define T7_RX_COR_UNREC_PACKET	0x11c0
#define T7_RX_COR_PACKET_IRQ_MODE	0x1120

static char *edid_firmware;
module_param(edid_firmware, charp, 0400);
MODULE_PARM_DESC(edid_firmware, "Optional validated EDID file in /lib/firmware for DDC bring-up");

static bool phy_test;
module_param(phy_test, bool, 0400);
MODULE_PARM_DESC(phy_test, "Opt-in 148.5 MHz input PLL/equalizer trial; requires EDID firmware");

static bool core_test;
module_param(core_test, bool, 0400);
MODULE_PARM_DESC(core_test, "Initialize the TMDS video core; requires phy_test=1");

static bool uhd60;
module_param(uhd60, bool, 0400);
MODULE_PARM_DESC(uhd60, "Opt-in RGB8 3840x2160p60 HDMI 2.0 trial; requires matching EDID");

static bool dynamic_modes;
module_param(dynamic_modes, bool, 0400);
MODULE_PARM_DESC(dynamic_modes, "Select PHY from the measured TMDS clock and capture progressive input timings; requires uhd60=1");

static bool hdr_probe;
module_param(hdr_probe, bool, 0400);
MODULE_PARM_DESC(hdr_probe, "Lock the 371.25 MHz HDR trial link for packet observation; HDR capture remains disabled");

static bool hdr_capture;
module_param(hdr_capture, bool, 0400);
MODULE_PARM_DESC(hdr_capture, "Experimental HDR10 420 10-bit input with VDIN HDR2 conversion; requires hdr_probe");

static bool audio_test;
module_param(audio_test, bool, 0400);
MODULE_PARM_DESC(audio_test, "Opt-in stereo LPCM ALSA capture codec for the dedicated audio DTB");

static const u32 t7_hdmirx_phy_top_regs[] = {
	T7_RX_TOP_CLK_CNTL, T7_RX_TOP_PORT_SEL, T7_RX_TOP_PHYIF_CNTL0,
	T7_RX_TOP_CHAN_SWITCH_0, T7_RX_TOP_CHAN_SWITCH_1,
	T7_RX_TOP_METER_HDMI_CNTL, T7_RX_TOP_METER_CABLE_CNTL,
	T7_RX_TOP_VID_CNTL, T7_RX_TOP_VID_CNTL2, T7_RX_TOP_OUTPUT_OVERRIDE,
};

/* Stereo receiver output and hardware ACR/unmute; never enable the test sine. */
static const u32 t7_hdmirx_audio_core_settings[][2] = {
	{ 0x1468, 0x50 }, /* Video clock difference threshold, output disabled */
	{ 0x1411, 0x0c }, /* Post divider fallback */
	{ 0x1402, 0x43 }, /* Sample frequency fallback */
	{ 0x140f, 0x00 }, /* UPLL fallback */
	{ 0x14a0, 0xff }, /* ACR averaging window */
	{ 0x1400, 0x43 }, /* Hardware N/CTS with sample frequency fallback */
	{ 0x1417, 0x00 }, /* No sample frequency override/filter */
	{ 0x1418, 0x0c }, /* MCLK loopback, CTS threshold */
	{ 0x1427, 0xf4 }, /* Enable audio output lanes and mute flag */
	{ 0x1428, 0xe4 }, /* Preserve channel order */
	{ 0x1426, 0x40 }, /* I2S framing */
	{ 0x142b, 0x00 }, /* No sample overwrite */
	{ 0x142d, 0x00 }, /* No forced output mute */
	{ 0x142c, 0x00 }, /* No upper sample overwrite */
	{ 0x1429, 0x3f }, /* Enable SPDIF and hardware mute, internal sine disabled */
	{ 0x14b1, 0x02 }, /* Keep system interrupt state */
	{ 0x14b5, 0xc3 }, /* Enable ACR and automatic mute/unmute */
	{ 0x14b7, 0x00 }, /* No extra mute event sources */
	{ 0x144e, 0x10 }, /* Duplicate stereo lane */
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

struct t7_hdmirx;
/* Raw FDET counters, before 420 horizontal expansion. */
struct t7_hdmirx_measurement {
	u32 width, height, hlow, hhigh, hfront, hback;
	u32 vlow, vhigh, vfront, vback, ticks, polarity;
};

struct t7_hdmirx_color {
	u32 space, depth, colorimetry;
	u32 full;
};

struct t7_hdmirx_audio_format {
	u32 rate, channels;
	u8 coding, allocation;
	bool pcm;
};

struct t7_hdmirx_link {
	struct mutex lock;
	struct delayed_work work;
	struct t7_hdmirx *rx;
	unsigned int users;
	u32 previous_cable;
	unsigned int stable_samples;
	unsigned int timing_retries;
	bool previous_uhd;
	bool ready;
	bool configured;
};

struct t7_hdmirx {
	struct t7_hdmirx_link *link;
	bool uhd60;
	bool dynamic_modes;
	bool hdr_probe;
	bool hdr_capture;
	bool lease_hdr;
	bool lease_yuv;
	bool lease_420;
	struct t7_hdmirx_measurement lease_mode;
	struct t7_hdmirx_color lease_color;
	u32 phy_band, pll_band;
	unsigned long long drm_received_ns;
	u8 saved_unrec_ctrl, saved_unrec_type;
	u8 saved_packet_irq_mode;
	struct mutex audio_lock;
	u8 saved_ddc_switch, saved_sink_version, saved_tmds_config;
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
	u8 core_saved_aon_reset;
	u8 core_saved_power;
	u8 core_saved_sys;
	u8 core_saved_phyck[3];
	u32 phy_top_saved[ARRAY_SIZE(t7_hdmirx_phy_top_regs)];
	u32 cable_hz;
	u8 edid[T7_RX_EDID_CAPACITY];
	struct debugfs_blob_wrapper edid_blob;
	void __iomem *frhdmirx;
	struct clk_bulk_data audio_clocks[3];
	bool audio_active;
	bool audio_invalid;
	struct delayed_work audio_work;
	struct snd_soc_component *audio_component;
	struct snd_pcm_substream *audio_substream;
	struct t7_hdmirx_audio_format audio_format, audio_pending;
	u32 audio_generation;
	unsigned int audio_stable_samples;
	unsigned int audio_read_retries;
	u32 audio_pll_saved[3];
	u32 audio_fifo_saved;
	u8 audio_core_saved[3];
	u32 audio_clk_saved;
	u32 audio_acr_saved[2];
	u8 audio_settings_saved[ARRAY_SIZE(t7_hdmirx_audio_core_settings)];
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

static int t7_hdmirx_validate_uhd_edid(const u8 *edid, size_t size)
{
	int ret = t7_hdmirx_validate_edid(edid, size);

	if (ret)
		return ret;
	/* The opt-in profile requires a 594 MHz preferred timing and CTA block. */
	if (size != 256 || !(edid[20] & BIT(7)) ||
	    (edid[54] | (edid[55] << 8)) != 59400 || edid[128] != 2)
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
	u32 port_sel;
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
	/* A runtime DDC disable restores the pre-probe port snapshot. An active
	 * core must reselect its PHY input as well as enabling the EDID path.
	 */
	port_sel = rx->saved_port_sel | BIT(4);
	if (rx->core_active)
		port_sel = (port_sel & ~GENMASK(2, 0)) | BIT(rx->port);
	writel(port_sel, rx->top + T7_RX_TOP_PORT_SEL);
	memcpy(rx->edid, edid, size);
	rx->edid_blob.data = rx->edid;
	rx->edid_blob.size = size;
	/* UHD sources must see SCDC enabled before the first HPD rising edge. */
	if (!rx->uhd60)
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

static u32 t7_hdmirx_sample_cable(struct t7_hdmirx *rx, unsigned long reference_hz)
{
	u32 status = readl(rx->top + T7_RX_TOP_METER_CABLE_STAT);

	/* Completed counts latch; arm the next measurement without resetting PCS. */
	writel(BIT(5) | BIT(6), rx->top + T7_RX_TOP_SW_RESET);
	readl(rx->top + T7_RX_TOP_SW_RESET);
	writel(0, rx->top + T7_RX_TOP_SW_RESET);
	readl(rx->top + T7_RX_TOP_SW_RESET);
	return t7_hdmirx_meter_hz(status, reference_hz);
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

static void t7_hdmirx_phy_configure(struct t7_hdmirx *rx)
{
	static const u32 dcha[][4] = {
		{ 0x17270888, 0x45ff4b58, 0x01ff28d4, 0x817 },
		{ 0x17270888, 0x45ff4b58, 0x01ff28d4, 0x817 },
		{ 0x17270fff, 0x45ff4b58, 0x01ff28c9, 0x630 },
		{ 0x13270fff, 0x45ff4b58, 0x01ff28c9, 0x630 },
		{ 0x10210fff, 0x45ff4b58, 0x01ff2a21, 0x1080 },
		{ 0x10210fff, 0x45ff4b58, 0x01ff2a21, 0x1080 },
	};
	static const u32 dchd[][3] = {
		{ 0xe3372103, 0x40000000, 0x0d4aaaaa },
		{ 0xe3372103, 0x40000000, 0x0d4aaaaa },
		{ 0xe3372103, 0x40009000, 0x0d422222 },
		{ 0xe3372903, 0x40009000, 0x0d422222 },
		{ 0xe3372903, 0x4004f000, 0x0d422222 },
		{ 0xe3372903, 0x4004f000, 0x0d422222 },
	};
	u32 band = rx->dynamic_modes ? rx->phy_band : rx->uhd60 ? 5 : 2;
	u32 trim = readl(rx->phy + T7_RX_PHY_MISC1);
	u32 misc0;
	/* T7 PHY bands follow character clock, including the SCDC 1:40 ratio. */
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
	writel(dcha[band][0], rx->phy + T7_RX_PHY_DCHA0);
	writel(dcha[band][1], rx->phy + T7_RX_PHY_DCHA1);
	writel(dcha[band][2], rx->phy + T7_RX_PHY_DCHA2);
	writel(dcha[band][3], rx->phy + T7_RX_PHY_DCHA3);
	/* Low bands use the T7 automatic 4x oversampling policy (os_rate=3). */
	if (rx->dynamic_modes && band <= 1) {
		writel(0x630, rx->phy + T7_RX_PHY_DCHA3);
		t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHA2,
			GENMASK(9, 0) | GENMASK(26, 25), 0xc9 | (2 << 25));
	}
	writel((dchd[band][0] | (rx->dynamic_modes && band <= 1 ? 2 << 6 : 0)) &
	       ~(BIT(28) | GENMASK(25, 24)), rx->phy + T7_RX_PHY_DCHD0);
	writel(dchd[band][1], rx->phy + T7_RX_PHY_DCHD1);
	writel(dchd[band][2] & ~BIT(26), rx->phy + T7_RX_PHY_DCHD2);
	writel(0x10000000, rx->phy + T7_RX_PHY_DCHD3);
	writel(0x3001, rx->phy + T7_RX_PHY_DCHD4);
	writel(0x817 | (BIT(rx->port) << 6), rx->phy + T7_RX_PHY_MISC3);
	usleep_range(5, 10);
	misc0 = (0x200773f8 & ~(BIT(10) | GENMASK(9, 7))) | BIT(rx->port);
	writel(misc0, rx->phy + T7_RX_PHY_MISC0);
	usleep_range(5, 10);
	writel(misc0 | BIT(10) | GENMASK(9, 7), rx->phy + T7_RX_PHY_MISC0);
}

static void t7_hdmirx_phy_prepare(struct t7_hdmirx *rx)
{
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
	writel(BIT(5) | BIT(6), rx->top + T7_RX_TOP_SW_RESET);
	readl(rx->top + T7_RX_TOP_SW_RESET);
	writel(0, rx->top + T7_RX_TOP_SW_RESET);
	t7_hdmirx_phy_configure(rx);
}

static int t7_hdmirx_wait_cable(struct t7_hdmirx *rx, unsigned long reference_hz)
{
	u32 previous = 0, sample_hz;
	unsigned int i, stable = 0;

	/* EDID reconnects on the source can outlast the initial three seconds. */
	for (i = 0; i < 500; i++) {
		if (!(readl(rx->top + T7_RX_TOP_HPD_PWR5V) & BIT(20 + rx->port)))
			return -ENOLINK;
		sample_hz = t7_hdmirx_sample_cable(rx, reference_hz);
		rx->cable_hz = sample_hz;
		/* Both profiles have a 148.5 MHz cable clock; SCDC distinguishes 1:40. */
		if ((!!(readb(rx->cor + T7_RX_COR_TMDS_CONFIG) & BIT(1)) == rx->uhd60) &&
		    sample_hz >= 146500000 && sample_hz <= 150500000 && previous &&
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
	/* T7 M/N, TMDS OD, secondary OD and recovered-audio divider. */
	static const u32 pll[][4] = {
		{ 160, 1, 5, 1 }, { 80, 1, 4, 0 }, { 40, 1, 3, 0 },
		{ 40, 2, 2, 0 }, { 40, 1, 1, 0 },
	};
	u32 band = rx->dynamic_modes ? rx->pll_band : rx->uhd60 ? 4 : 2;
	u32 phy_band = rx->dynamic_modes ? rx->phy_band : rx->uhd60 ? 5 : 2;
	u32 od = pll[band][2] - (rx->dynamic_modes && phy_band <= 1 ? 2 : 0);
	u32 data = 0x00090000 | pll[band][0] | (pll[band][1] << 10);
	u32 ctrl3 = (phy_band <= 2 ? 0x10058f30 : 0x10058f00) | pll[band][3];
	u32 ctrl4 = 0x080130c0 | (od << 24);
	unsigned int attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		writel(data | 0x20000000, rx->phy + T7_RX_PHY_APLL0);
		usleep_range(5, 10);
		writel(data | 0x30000000, rx->phy + T7_RX_PHY_APLL0);
		usleep_range(5, 10);
		writel(0, rx->phy + T7_RX_PHY_APLL1);
		writel(0x00001118, rx->phy + T7_RX_PHY_APLL2);
		usleep_range(5, 10);
		writel(ctrl3, rx->phy + T7_RX_PHY_APLL3);
		writel(ctrl4, rx->phy + T7_RX_PHY_APLL4);
		usleep_range(5, 10);
		writel(ctrl4 | 0x00800000, rx->phy + T7_RX_PHY_APLL4);
		usleep_range(5, 10);
		writel(data | 0x34000000, rx->phy + T7_RX_PHY_APLL0);
		usleep_range(5, 10);
		writel(data | 0x14000000, rx->phy + T7_RX_PHY_APLL0);
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
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD0, BIT(27), BIT(27));
	if (rx->dynamic_modes ? rx->phy_band >= 3 : rx->uhd60)
		t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD0, BIT(29), 0);
	t7_hdmirx_phy_update(rx, T7_RX_PHY_DCHD2, BIT(26), 0);
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
	/* Restore firmware isolation/clock state for a later PHY-only trial. */
	writeb(rx->core_saved_sys, rx->cor + T7_RX_COR_SYS_CTRL);
	writeb(rx->core_saved_phyck[0], rx->cor + T7_RX_COR_PHY_CLOCK1);
	writeb(rx->core_saved_phyck[1], rx->cor + T7_RX_COR_PHY_CLOCK2);
	writeb(rx->core_saved_phyck[2], rx->cor + T7_RX_COR_PHY_CLOCK3);
	writeb(rx->core_saved_power, rx->cor + T7_RX_COR_POWER_CTRL);
	writeb(rx->core_saved_aon_reset, rx->cor + T7_RX_COR_AON_RESET);
	readb(rx->cor + T7_RX_COR_PHY_CLOCK3);
	writeb(rx->saved_ddc_switch, rx->cor + T7_RX_COR_DDC_SWITCH);
	writeb(rx->saved_sink_version, rx->cor + T7_RX_COR_SINK_VERSION);
	writeb(rx->saved_tmds_config, rx->cor + T7_RX_COR_TMDS_CONFIG);
	if (rx->hdr_probe) {
		writeb(rx->saved_unrec_type, rx->cor + T7_RX_COR_UNREC_TYPE);
		writeb(rx->saved_unrec_ctrl, rx->cor + T7_RX_COR_UNREC_CTRL);
	}
	if (rx->hdr_capture)
		writeb(rx->saved_packet_irq_mode, rx->cor + T7_RX_COR_PACKET_IRQ_MODE);
	WRITE_ONCE(rx->drm_received_ns, 0);
	rx->core_active = false;
}

static void t7_hdmirx_reset_pcs(struct t7_hdmirx *rx)
{
	u8 reset;

	/* Re-align the PCS after the input PLL and equalizer have settled. */
	writel(BIT(13) | BIT(7), rx->top + T7_RX_TOP_SW_RESET);
	readl(rx->top + T7_RX_TOP_SW_RESET);
	usleep_range(1, 5);
	writel(0, rx->top + T7_RX_TOP_SW_RESET);
	readl(rx->top + T7_RX_TOP_SW_RESET);
	/* T7 rx_cor_reset also pulses the deep-colour FIFO after TMDS relock.
	 * PCS alignment alone can leave FDET at width+2/height zero.
	 */
	reset = readb(rx->cor + T7_RX_COR_POWER_RESET) & ~BIT(4);
	writeb(reset | BIT(4), rx->cor + T7_RX_COR_POWER_RESET);
	usleep_range(1, 5);
	writeb(reset, rx->cor + T7_RX_COR_POWER_RESET);
}

static int t7_hdmirx_core_start(struct t7_hdmirx *rx)
{
	struct arm_smccc_res res;

	/* T7's firmware owns TOP_SECURE_MODE, including non-secure core access. */
	arm_smccc_smc(0x8200008c, 0xa2, 0x1f, 0, 0, 0, 0, 0, &res);
	rx->core_smc_result = res.a0;
	if (res.a0)
		return -EIO;
	rx->saved_ddc_switch = readb(rx->cor + T7_RX_COR_DDC_SWITCH);
	rx->saved_sink_version = readb(rx->cor + T7_RX_COR_SINK_VERSION);
	rx->saved_tmds_config = readb(rx->cor + T7_RX_COR_TMDS_CONFIG);
	rx->core_saved_aon_reset = readb(rx->cor + T7_RX_COR_AON_RESET);
	rx->core_saved_power = readb(rx->cor + T7_RX_COR_POWER_CTRL);
	rx->core_saved_sys = readb(rx->cor + T7_RX_COR_SYS_CTRL);
	rx->core_saved_phyck[0] = readb(rx->cor + T7_RX_COR_PHY_CLOCK1);
	rx->core_saved_phyck[1] = readb(rx->cor + T7_RX_COR_PHY_CLOCK2);
	rx->core_saved_phyck[2] = readb(rx->cor + T7_RX_COR_PHY_CLOCK3);
	if (rx->hdr_probe) {
		rx->saved_unrec_ctrl = readb(rx->cor + T7_RX_COR_UNREC_CTRL);
		rx->saved_unrec_type = readb(rx->cor + T7_RX_COR_UNREC_TYPE);
	}
	if (rx->hdr_capture)
		rx->saved_packet_irq_mode = readb(rx->cor + T7_RX_COR_PACKET_IRQ_MODE);
	rx->core_active = true;
	/* Hold AON reset while removing TMDS isolation and selecting PHY lanes. */
	writeb(1, rx->cor + T7_RX_COR_AON_RESET);
	writeb(0x11, rx->cor + T7_RX_COR_HDMI2_MODE);
	/* Start a fresh SCDC handshake while HPD is low. */
	writeb(0x86, rx->cor + T7_RX_COR_DDC_SWITCH);
	writeb(1, rx->cor + T7_RX_COR_SINK_VERSION);
	writeb(0, rx->cor + T7_RX_COR_TMDS_CONFIG);
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
	/* The UHD CTA profile is HDMI; the base-block 1080p profile is DVI. */
	writeb(rx->uhd60 ? 6 : 0, rx->cor + T7_RX_COR_PREAMBLE);
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
	if (readb(rx->cor + T7_RX_COR_SYS_CTRL) != 3 ||
	    readb(rx->cor + T7_RX_COR_PHY_CLOCK1) != 0xff)
		return -EIO;
	if (rx->hdr_probe) {
		/* Capture static metadata in the byte register bank, without packet DMA. */
		writeb(0x87, rx->cor + T7_RX_COR_UNREC_TYPE);
		writeb(1, rx->cor + T7_RX_COR_UNREC_CTRL);
	}
	if (rx->hdr_capture) {
		/* Default is content-change IRQs. Freshness needs every DRM receipt,
		 * including identical static metadata. This does not unmask an IRQ.
		 */
		writeb(rx->saved_packet_irq_mode | BIT(4),
		       rx->cor + T7_RX_COR_PACKET_IRQ_MODE);
		writeb(BIT(4), rx->cor + 0x1134);
	}
	if (rx->uhd60)
		t7_hdmirx_set_hpd(rx, true);
	return 0;
}

static u32 t7_hdmirx_core_read16(struct t7_hdmirx *rx, u32 offset)
{
	return readb(rx->cor + offset) | (readb(rx->cor + offset + 1) << 8);
}

static bool t7_hdmirx_packet_valid(const u8 *data, u8 type, u8 version, u8 length)
{
	unsigned int i;
	u8 sum = 0;

	if (data[0] != type || data[1] != version || data[2] != length)
		return false;
	for (i = 0; i < (unsigned int)length + 4; i++)
		sum += data[i];
	return !sum;
}

/* Called by the link worker. A cached packet bank is not proof of reception.
 * Consume only the DRM new-packet W1C flag, leaving AVI/audio IRQs untouched.
 * Double reads and checksum reject torn packets; no metadata DMA is needed.
 */
static void t7_hdmirx_update_drm(struct t7_hdmirx *rx)
{
	u8 packet[30], again[30];
	unsigned int i;

	if (!rx->hdr_capture)
		return;
	if (!(readl(rx->top + T7_RX_TOP_HPD_PWR5V) & BIT(20 + rx->port)) ||
	    !(readl(rx->top + T7_RX_TOP_TMDS_ALIGN_STAT) & BIT(0))) {
		WRITE_ONCE(rx->drm_received_ns, 0);
		writeb(BIT(4), rx->cor + 0x1134);
		return;
	}
	if (!(readb(rx->cor + 0x1134) & BIT(4)))
		return;
	writeb(BIT(4), rx->cor + 0x1134);
	for (i = 0; i < sizeof(packet); i++)
		packet[i] = readb(rx->cor + 0x11c0 + i);
	for (i = 0; i < sizeof(again); i++)
		again[i] = readb(rx->cor + 0x11c0 + i);
	WRITE_ONCE(rx->drm_received_ns,
		   !memcmp(packet, again, sizeof(packet)) &&
		   t7_hdmirx_packet_valid(packet, 0x87, 1, 26) &&
		   packet[4] == 2 && packet[5] == 0 ? ktime_get_ns() : 0);
}

static bool t7_hdmirx_hdr_input(struct t7_hdmirx *rx)
{
	u8 avi[17], again[17];
	u64 received = READ_ONCE(rx->drm_received_ns);
	u8 depth = readb(rx->cor + T7_RX_COR_COLOR_DEPTH);
	unsigned int i;

	if (!rx->hdr_capture || !received || ktime_get_ns() - received > 1000000000ULL)
		return false;
	for (i = 0; i < sizeof(avi); i++)
		avi[i] = readb(rx->cor + 0x1140 + i);
	for (i = 0; i < sizeof(again); i++)
		again[i] = readb(rx->cor + 0x1140 + i);
	if (memcmp(avi, again, sizeof(avi)) ||
	    !t7_hdmirx_packet_valid(avi, 0x82, 2, 13) ||
	    (avi[5] >> 6) != 3 || ((avi[6] >> 4) & 7) != 6 || (avi[8] & 0xc0))
		return false;
	/* HDR2 receives normalized Y/Cb/Cr after TOP's unpacker for each YUV
	 * transport. Its PQ/2020 matrices and LUTs are independent of timing.
	 * RGB/2020 needs a different input matrix and remains unsupported.
	 */
	if (rx->dynamic_modes)
		return (avi[4] >> 5) >= 1 && (avi[4] >> 5) <= 3 &&
			(depth == 0 || depth == 4 || depth == 5 || depth == 6);
	return (avi[4] >> 5) == 3 && avi[7] == 97 && depth == 5;
}

/* macOS can keep the same 420/10-bit transport when HDR is disabled. Its
 * BT.709 limited AVI describes ordinary SDR and needs no PQ/tone mapping.
 */
static bool t7_hdmirx_sdr420_input(struct t7_hdmirx *rx)
{
	u8 avi[17], again[17];
	unsigned int i;

	if (!rx->hdr_capture || readb(rx->cor + T7_RX_COR_COLOR_DEPTH) != 5)
		return false;
	for (i = 0; i < sizeof(avi); i++)
		avi[i] = readb(rx->cor + 0x1140 + i);
	for (i = 0; i < sizeof(again); i++)
		again[i] = readb(rx->cor + 0x1140 + i);
	return !memcmp(avi, again, sizeof(avi)) &&
		t7_hdmirx_packet_valid(avi, 0x82, 2, 13) &&
		(avi[4] >> 5) == 3 && (avi[5] >> 6) == 2 &&
		!(avi[8] & 0xc0) && avi[7] == 97;
}

static bool t7_hdmirx_sdr444_input(struct t7_hdmirx *rx)
{
	u8 avi[17], again[17];
	unsigned int i;

	/* A zero GCP depth code means unspecified, which is also legal for 8-bit. */
	if (readb(rx->cor + T7_RX_COR_COLOR_DEPTH) > 4)
		return false;
	for (i = 0; i < sizeof(avi); i++)
		avi[i] = readb(rx->cor + T7_RX_COR_AVI_PACKET + i);
	for (i = 0; i < sizeof(again); i++)
		again[i] = readb(rx->cor + T7_RX_COR_AVI_PACKET + i);
	return !memcmp(avi, again, sizeof(avi)) &&
		t7_hdmirx_packet_valid(avi, 0x82, 2, 13) &&
		(avi[4] >> 5) == 2 && (avi[5] >> 6) == 2 && !(avi[8] & 0xc0);
}

static int t7_hdmirx_read_color(struct t7_hdmirx *rx, struct t7_hdmirx_color *color)
{
	u8 avi[17], again[17], depth = readb(rx->cor + T7_RX_COR_COLOR_DEPTH);
	u32 space = (readl(rx->top + T7_RX_TOP_VID_STAT) >> 20) & 15;
	unsigned int i;

	if (space > 3 || (depth != 0 && depth != 4 && depth != 5 && depth != 6))
		return -ERANGE;
	*color = (struct t7_hdmirx_color) {
		.space = space, .depth = depth == 5 ? 10 : depth == 6 ? 12 : 8,
		.colorimetry = 2, .full = !space,
	};
	/* RX_AUDP_STAT_DP2 bit 0 is the T7 HDMI/DVI detector. A cached AVI
	 * bank can survive a switch to a DVI BIOS source and must be ignored.
	 */
	if (!(readb(rx->cor + T7_RX_COR_AUDIO_PACKET) & BIT(0)))
		return space ? -ERANGE : 0;
	for (i = 0; i < sizeof(avi); i++)
		avi[i] = readb(rx->cor + T7_RX_COR_AVI_PACKET + i);
	for (i = 0; i < sizeof(again); i++)
		again[i] = readb(rx->cor + T7_RX_COR_AVI_PACKET + i);
	if (memcmp(avi, again, sizeof(avi)))
		return -EAGAIN;
	if (!t7_hdmirx_packet_valid(avi, 0x82, 2, 13))
		return -ERANGE;
	if ((avi[4] >> 5) != space)
		return -EAGAIN;
	/* Repeated pixels require a separate de-repeat clock setup. */
	if (avi[8] & 15)
		return -ERANGE;
	if ((avi[5] >> 6) == 3 && !t7_hdmirx_hdr_input(rx))
		return -ERANGE;
	if (!space) {
		u32 range = (avi[6] >> 2) & 3;

		if (range == 3)
			return -ERANGE;
		color->full = range == 2 || (!range && (!avi[7] || avi[7] == 1));
	} else {
		u32 range = avi[8] >> 6;

		if (range > 1)
			return -ERANGE;
		color->full = range == 1;
		color->colorimetry = avi[5] >> 6;
		if (!color->colorimetry)
			color->colorimetry = t7_hdmirx_core_read16(rx, T7_RX_COR_HEIGHT) <= 576 ? 1 : 2;
		/* Keep the qualified PQ/2020 path; reject HLG and other extended spaces. */
		if (color->colorimetry == 3 && !t7_hdmirx_hdr_input(rx))
			return -ERANGE;
	}
	return 0;
}

static void t7_hdmirx_read_measurement(struct t7_hdmirx *rx,
				     struct t7_hdmirx_measurement *m)
{
	*m = (struct t7_hdmirx_measurement) {
		.width = t7_hdmirx_core_read16(rx, T7_RX_COR_WIDTH),
		.height = t7_hdmirx_core_read16(rx, T7_RX_COR_HEIGHT),
		.hlow = t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC_LOW),
		.hhigh = t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC_HIGH),
		.hfront = t7_hdmirx_core_read16(rx, T7_RX_COR_HFRONT),
		.hback = t7_hdmirx_core_read16(rx, T7_RX_COR_HBACK),
		.vlow = t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC_LOW),
		.vhigh = t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC_HIGH),
		.vfront = t7_hdmirx_core_read16(rx, T7_RX_COR_VFRONT),
		.vback = t7_hdmirx_core_read16(rx, T7_RX_COR_VBACK),
		.ticks = t7_hdmirx_core_read16(rx, T7_RX_COR_FRAME_TICKS) |
			 (readb(rx->cor + T7_RX_COR_FRAME_TICKS + 2) << 16),
		.polarity = readb(rx->cor + T7_RX_COR_FRAME_STATUS),
	};
}

static int t7_hdmirx_measure(struct t7_hdmirx *rx, bool yuv420,
			     struct t7_hdmirx_measurement *m)
{
	struct t7_hdmirx_measurement again;
	u32 hsync, vsync, reference = clk_get_rate(rx->clocks[T7_RX_PCLK].clk);
	u32 scale = yuv420 ? 2 : 1;
	u64 pixelclock;

	t7_hdmirx_read_measurement(rx, m);
	t7_hdmirx_read_measurement(rx, &again);
	if (memcmp(m, &again, sizeof(*m)))
		return -EAGAIN;
	/* FDET polarity bits select the pulse, not the much longer idle level. */
	hsync = m->polarity & BIT(0) ? m->hhigh : m->hlow;
	vsync = m->polarity & BIT(1) ? m->vhigh : m->vlow;
	if (m->polarity & BIT(2) || !hsync || !vsync ||
	    m->width * scale < 320 || m->width * scale > 3840 ||
	    m->height < 180 || m->height > 2160 || (m->height & 1) ||
	    (m->width & 1) || !m->ticks ||
	    m->ticks < reference / 120 || m->ticks > reference / 20 ||
	    m->width + m->hfront + hsync + m->hback != m->hlow + m->hhigh ||
	    m->height + m->vfront + vsync + m->vback != m->vlow + m->vhigh)
		return -ERANGE;
	pixelclock = div_u64((u64)reference * (m->hlow + m->hhigh) * scale *
			    (m->vlow + m->vhigh), m->ticks);
	if (pixelclock < 24000000 || pixelclock > 600000000)
		return -ERANGE;
	return 0;
}

static bool t7_hdmirx_same_measurement(const struct t7_hdmirx_measurement *a,
				      const struct t7_hdmirx_measurement *b)
{
	struct t7_hdmirx_measurement left = *a, right = *b;
	u32 delta = a->ticks > b->ticks ? a->ticks - b->ticks : b->ticks - a->ticks;

	left.ticks = right.ticks = 0;
	/* Meter quantization is a few ticks; 60 versus 59.94 remains a change. */
	return !memcmp(&left, &right, sizeof(left)) && delta <= a->ticks / 2000;
}

static int t7_hdmirx_video_status(struct t7_hdmirx *rx)
{
	struct t7_hdmirx_measurement measured;
	struct t7_hdmirx_color color;
	u32 ticks, reference;
	bool hdr, yuv, yuv420;
	int ret;

	if (!rx || !rx->core_active)
		return -ENOLINK;
	if (rx->dynamic_modes &&
	    (!!(readb(rx->cor + T7_RX_COR_TMDS_CONFIG) & BIT(1)) != READ_ONCE(rx->uhd60)))
		return -ENOLINK;
	if (rx->link && !(readl(rx->phy + T7_RX_PHY_APLL0) & T7_RX_PHY_PLL_LOCK))
		return -ENOLINK;
	if (!(readl(rx->top + T7_RX_TOP_HPD_PWR5V) & BIT(20 + rx->port)) ||
	    !(readl(rx->top + T7_RX_TOP_TMDS_ALIGN_STAT) & BIT(0)))
		return -ENOLINK;
	if (readb(rx->cor + T7_RX_COR_FRAME_STATUS) & BIT(2))
		return -ERANGE;
	hdr = t7_hdmirx_hdr_input(rx);
	yuv420 = hdr || t7_hdmirx_sdr420_input(rx);
	yuv = yuv420 || t7_hdmirx_sdr444_input(rx);
	if (rx->dynamic_modes) {
		ret = t7_hdmirx_read_color(rx, &color);
		if (ret)
			return ret;
		yuv420 = color.space == 3;
		yuv = color.space != 0;
		if (rx->link && rx->link->users &&
		    memcmp(&color, &rx->lease_color, sizeof(color)))
			return -ENOLINK;
	} else if (!yuv && (readl(rx->top + T7_RX_TOP_VID_STAT) & GENMASK(23, 20) ||
	    readb(rx->cor + T7_RX_COR_COLOR_DEPTH) > 4))
		return -ERANGE;
	if (rx->link && rx->link->users &&
	    (hdr != rx->lease_hdr || yuv != rx->lease_yuv || yuv420 != rx->lease_420))
		return -ENOLINK;
	if (rx->dynamic_modes) {
		ret = t7_hdmirx_measure(rx, yuv420, &measured);
		if (!ret && rx->link && rx->link->users &&
		    !t7_hdmirx_same_measurement(&measured, &rx->lease_mode))
			return -ENOLINK;
		return ret;
	}
	if (t7_hdmirx_core_read16(rx, T7_RX_COR_WIDTH) != (yuv420 ? 1920 : rx->uhd60 ? 3840 : 1920) ||
	    t7_hdmirx_core_read16(rx, T7_RX_COR_HEIGHT) != (rx->uhd60 ? 2160 : 1080) ||
	    t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC_LOW) +
	    t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC_HIGH) != (yuv420 ? 2200 : rx->uhd60 ? 4400 : 2200) ||
	    t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC_LOW) +
	    t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC_HIGH) != (rx->uhd60 ? 2250 : 1125))
		return -ERANGE;
	ticks = t7_hdmirx_core_read16(rx, T7_RX_COR_FRAME_TICKS) |
		(readb(rx->cor + T7_RX_COR_FRAME_TICKS + 2) << 16);
	reference = clk_get_rate(rx->clocks[T7_RX_PCLK].clk);
	if (!ticks || ticks < reference / 61 || ticks > reference / 59)
		return -ERANGE;
	return 0;
}
int t7_hdmirx_check_video_now(struct device *dev)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);

	if (rx && rx->link && !READ_ONCE(rx->link->ready))
		return -ENOLINK;
	return t7_hdmirx_video_status(rx);
}
EXPORT_SYMBOL_GPL(t7_hdmirx_check_video_now);

int t7_hdmirx_check_video(struct device *dev)
{
	int ret = t7_hdmirx_check_video_now(dev);

	if (ret)
		return ret;
	usleep_range(20000, 21000);
	return t7_hdmirx_check_video_now(dev);
}
EXPORT_SYMBOL_GPL(t7_hdmirx_check_video);

int t7_hdmirx_get_max_size(struct device *dev, unsigned int *width, unsigned int *height)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);

	if (!rx || !rx->core_active)
		return -ENOLINK;
	*width = rx->dynamic_modes || rx->uhd60 ? 3840 : 1920;
	*height = rx->dynamic_modes || rx->uhd60 ? 2160 : 1080;
	return 0;
}
EXPORT_SYMBOL_GPL(t7_hdmirx_get_max_size);

int t7_hdmirx_query_timings(struct device *dev, struct v4l2_dv_timings *timings,
			  struct v4l2_fract *interval)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);
	struct v4l2_dv_timings mode;
	u32 ticks, reference, before, after;
	u32 hsync, vsync;
	bool selected_uhd;
	int ret;

	ret = t7_hdmirx_check_video_now(dev);
	if (ret)
		return ret;
	if (rx->dynamic_modes) {
		struct t7_hdmirx_measurement m;
		struct t7_hdmirx_color color;
		u32 scale;

		ret = t7_hdmirx_read_color(rx, &color);
		if (ret)
			return ret;
		scale = color.space == 3 ? 2 : 1;
		ret = t7_hdmirx_measure(rx, scale == 2, &m);
		if (ret)
			return ret;
		mode = (struct v4l2_dv_timings) { .type = V4L2_DV_BT_656_1120 };
		mode.bt.width = m.width * scale;
		mode.bt.height = m.height;
		mode.bt.polarities = (m.polarity & BIT(0) ? V4L2_DV_HSYNC_POS_POL : 0) |
			(m.polarity & BIT(1) ? V4L2_DV_VSYNC_POS_POL : 0);
		mode.bt.hsync = (m.polarity & BIT(0) ? m.hhigh : m.hlow) * scale;
		mode.bt.vsync = m.polarity & BIT(1) ? m.vhigh : m.vlow;
		mode.bt.hfrontporch = m.hfront * scale;
		mode.bt.hbackporch = m.hback * scale;
		mode.bt.vfrontporch = m.vfront;
		mode.bt.vbackporch = m.vback;
		reference = clk_get_rate(rx->clocks[T7_RX_PCLK].clk);
		mode.bt.pixelclock = div_u64((u64)reference * (m.hlow + m.hhigh) * scale *
					    (m.vlow + m.vhigh), m.ticks);
		ret = t7_hdmirx_check_video_now(dev);
		if (ret)
			return ret;
		*timings = mode;
		*interval = (struct v4l2_fract) { .numerator = m.ticks, .denominator = reference };
		return 0;
	}
	selected_uhd = READ_ONCE(rx->uhd60);
	/* Byte-addressed counters can roll over between bus reads. Retry through
	 * userspace rather than publishing a torn period as a different frame rate.
	 * No frame wait here: status polling shares the capture queue lock.
	 */
	before = t7_hdmirx_core_read16(rx, T7_RX_COR_FRAME_TICKS) |
		 (readb(rx->cor + T7_RX_COR_FRAME_TICKS + 2) << 16);
	after = t7_hdmirx_core_read16(rx, T7_RX_COR_FRAME_TICKS) |
		(readb(rx->cor + T7_RX_COR_FRAME_TICKS + 2) << 16);
	if (before != after)
		return -EAGAIN;
	ticks = after;
	reference = clk_get_rate(rx->clocks[T7_RX_PCLK].clk);
	if (!ticks || ticks < reference / 61 || ticks > reference / 59)
		return -ERANGE;
	mode = selected_uhd ? (struct v4l2_dv_timings)V4L2_DV_BT_CEA_3840X2160P60 :
			  (struct v4l2_dv_timings)V4L2_DV_BT_CEA_1920X1080P60;
	/* Both qualified CEA modes use positive sync pulses. The low-level count
	 * includes the active picture and porches, not the sync pulse width.
	 */
	hsync = t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC_HIGH);
	if (t7_hdmirx_hdr_input(rx) || t7_hdmirx_sdr420_input(rx))
		hsync *= 2;
	vsync = t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC_HIGH);
	/* Only these CEA profiles have qualified porch/polarity settings. Do not
	 * fabricate DV timings for arbitrary modes with the same active area.
	 */
	if (hsync != mode.bt.hsync || vsync != mode.bt.vsync)
		return -ERANGE;
	ret = t7_hdmirx_check_video_now(dev);
	if (ret)
		return ret;
	if (selected_uhd != READ_ONCE(rx->uhd60))
		return -EAGAIN;
	mode.bt.pixelclock = div_u64((u64)reference *
		(mode.bt.width + mode.bt.hfrontporch + mode.bt.hsync + mode.bt.hbackporch) *
		(mode.bt.height + mode.bt.vfrontporch + mode.bt.vsync + mode.bt.vbackporch), ticks);
	/* The measured clock is authoritative, not an exact 60/1 or 60000/1001. */
	mode.bt.flags &= ~(V4L2_DV_FL_CAN_REDUCE_FPS | V4L2_DV_FL_REDUCED_FPS);
	*timings = mode;
	*interval = (struct v4l2_fract) { .numerator = ticks, .denominator = reference };
	return 0;
}
EXPORT_SYMBOL_GPL(t7_hdmirx_query_timings);

static u32 t7_hdmirx_audio_word(struct t7_hdmirx *rx, unsigned int reg)
{
	return ((readb(rx->cor + reg) & 0xf) << 16) |
	       (readb(rx->cor + reg + 1) << 8) | readb(rx->cor + reg + 2);
}

static unsigned int t7_hdmirx_audio_rate(u32 tmds, u32 n, u32 cts)
{
	static const unsigned int rates[] = {
		32000, 44100, 48000, 88200, 96000, 176400, 192000,
	};
	u64 measured;
	unsigned int i;

	if (!tmds || !n || !cts)
		return 0;
	measured = div_u64((u64)tmds * n, 128 * cts);
	for (i = 0; i < ARRAY_SIZE(rates); i++)
		if (measured >= rates[i] - rates[i] / 100 &&
		    measured <= rates[i] + rates[i] / 100)
			return rates[i];
	return 0;
}

static int t7_hdmirx_validate_audio(u32 tmds, u32 n, u32 cts,
				  u8 info, u8 packet, u8 channel_status,
				  unsigned int rate)
{
	u64 measured;

	if (!tmds || !n || !cts)
		return -ENOLINK;
	if (rate != 32000 && rate != 44100 && rate != 48000 &&
	    rate != 88200 && rate != 96000 && rate != 176400 && rate != 192000)
		return -EINVAL;
	/* Reject compressed, HBR and multichannel layouts before touching clocks. */
	if ((info >> 4) > 1 || (info & 7) > 1 ||
	    (packet & (BIT(6) | GENMASK(4, 3))) || (channel_status & BIT(1)))
		return -EINVAL;
	measured = div_u64((u64)tmds * n, 128 * cts);
	if (measured < rate - rate / 100 || measured > rate + rate / 100)
		return -ERANGE;
	return 0;
}

static bool t7_hdmirx_audio_same(const struct t7_hdmirx_audio_format *a,
				const struct t7_hdmirx_audio_format *b)
{
	return a->rate == b->rate && a->channels == b->channels &&
	       a->coding == b->coding && a->allocation == b->allocation && a->pcm == b->pcm;
}

static int t7_hdmirx_read_audio(struct t7_hdmirx *rx,
			      struct t7_hdmirx_audio_format *format)
{
	static const unsigned int aif_rates[] = {
		0, 32000, 44100, 48000, 88200, 96000, 176400, 192000,
	};
	u8 aif[14], second[14], packet, channel_status, sum = 0;
	u32 n, cts, tmds;
	unsigned int i, declared;

	if (!rx->core_active || (rx->link && !READ_ONCE(rx->link->ready)))
		return -ENOLINK;
	n = t7_hdmirx_audio_word(rx, T7_RX_COR_N);
	cts = t7_hdmirx_audio_word(rx, T7_RX_COR_CTS);
	packet = readb(rx->cor + T7_RX_COR_AUDIO_PACKET);
	channel_status = readb(rx->cor + T7_RX_COR_AUDIO_CHSTATUS);
	for (i = 0; i < ARRAY_SIZE(aif); i++)
		aif[i] = readb(rx->cor + T7_RX_COR_AUDIO_AIF + i);
	for (i = 0; i < ARRAY_SIZE(second); i++)
		second[i] = readb(rx->cor + T7_RX_COR_AUDIO_AIF + i);
	if (memcmp(aif, second, sizeof(aif)) ||
	    n != t7_hdmirx_audio_word(rx, T7_RX_COR_N) ||
	    cts != t7_hdmirx_audio_word(rx, T7_RX_COR_CTS) ||
	    packet != readb(rx->cor + T7_RX_COR_AUDIO_PACKET) ||
	    channel_status != readb(rx->cor + T7_RX_COR_AUDIO_CHSTATUS))
		return -EAGAIN;
	/* T7 exposes the received AIF header/checksum and ten payload bytes. */
	if (aif[0] != 0x84 || aif[1] != 1 || aif[2] != 10)
		return -ENOLINK;
	for (i = 0; i < ARRAY_SIZE(aif); i++)
		sum += aif[i];
	if (sum)
		return -EAGAIN;
	tmds = t7_hdmirx_meter_hz(readl(rx->top + T7_RX_TOP_METER_HDMI_STAT),
				clk_get_rate(rx->clocks[T7_RX_METER].clk));
	format->rate = t7_hdmirx_audio_rate(tmds, n, cts);
	if (!format->rate)
		return -EAGAIN;
	declared = aif_rates[(aif[5] >> 2) & 7];
	if (declared && declared != format->rate)
		return -EAGAIN;
	format->channels = (aif[4] & 7) ? (aif[4] & 7) + 1 :
		(packet & GENMASK(4, 3) ? 0 : 2);
	format->coding = aif[4] >> 4;
	format->allocation = aif[7];
	/* T7 FRHDMIRX has one SPDIF lane and no multichannel PAO path. */
	format->pcm = !t7_hdmirx_validate_audio(tmds, n, cts, aif[4], packet,
						channel_status, format->rate) && !aif[7];
	return 0;
}

static void t7_hdmirx_audio_stop_locked(void *data)
{
	struct t7_hdmirx *rx = data;
	unsigned int i;
	u32 value;

	if (!rx->audio_active)
		return;
	/* TODDR owns DMA and stops it through ALSA before this codec is freed. */
	value = readl(rx->frhdmirx);
	writel(value & ~BIT(31), rx->frhdmirx);
	writel(rx->audio_fifo_saved, rx->frhdmirx);
	writeb(readb(rx->cor + T7_RX_COR_POWER_RESET) | BIT(1),
	       rx->cor + T7_RX_COR_POWER_RESET);
	writel(0, rx->phy + T7_RX_AUD_PLL0);
	writel(rx->audio_pll_saved[1], rx->phy + T7_RX_AUD_PLL2);
	writel(rx->audio_pll_saved[2], rx->phy + T7_RX_AUD_PLL3);
	writel(rx->audio_pll_saved[0], rx->phy + T7_RX_AUD_PLL0);
	writel(rx->audio_acr_saved[0], rx->top + T7_RX_TOP_ACR_CTRL);
	writel(rx->audio_acr_saved[1], rx->top + T7_RX_TOP_ACR_CTRL2);
	writeb(rx->audio_core_saved[0], rx->cor + T7_RX_COR_AUDIO_MCLK);
	writeb(rx->audio_core_saved[1], rx->cor + T7_RX_COR_AUDIO_TDM1);
	writeb(rx->audio_core_saved[2], rx->cor + T7_RX_COR_AUDIO_TDM2);
	for (i = 0; i < ARRAY_SIZE(t7_hdmirx_audio_core_settings); i++)
		writeb(rx->audio_settings_saved[i],
		       rx->cor + t7_hdmirx_audio_core_settings[i][0]);
	value = readl(rx->top + T7_RX_TOP_CLK_CNTL);
	writel((value & ~BIT(4)) | rx->audio_clk_saved,
	       rx->top + T7_RX_TOP_CLK_CNTL);
	clk_bulk_disable_unprepare(ARRAY_SIZE(rx->audio_clocks), rx->audio_clocks);
	rx->audio_active = false;
}

static void t7_hdmirx_audio_stop(void *data)
{
	struct t7_hdmirx *rx = data;

	mutex_lock(&rx->audio_lock);
	t7_hdmirx_audio_stop_locked(rx);
	mutex_unlock(&rx->audio_lock);
}

static int t7_hdmirx_audio_hw_params_locked(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params,
				    struct snd_soc_dai *dai)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dai->dev);
	struct t7_hdmirx_audio_format format = { 0 };
	u32 status, value;
	unsigned int i;
	int ret;

	if (!rx->core_active || params_channels(params) != 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;
	ret = t7_hdmirx_read_audio(rx, &format);
	if (ret)
		return ret;
	if (!format.pcm)
		return -EINVAL;
	if (format.rate != params_rate(params))
		return -ERANGE;
	/* Qualification must have observed the same format twice. */
	if (!t7_hdmirx_audio_same(&format, &rx->audio_format))
		return -ENOLINK;
	if (rx->audio_active)
		return -EBUSY;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(rx->audio_clocks), rx->audio_clocks);
	if (ret)
		return ret;
	rx->audio_fifo_saved = readl(rx->frhdmirx);
	if (rx->audio_fifo_saved & BIT(31)) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(rx->audio_clocks), rx->audio_clocks);
		return -EBUSY;
	}
	rx->audio_pll_saved[0] = readl(rx->phy + T7_RX_AUD_PLL0);
	rx->audio_pll_saved[1] = readl(rx->phy + T7_RX_AUD_PLL2);
	rx->audio_pll_saved[2] = readl(rx->phy + T7_RX_AUD_PLL3);
	rx->audio_core_saved[0] = readb(rx->cor + T7_RX_COR_AUDIO_MCLK);
	rx->audio_core_saved[1] = readb(rx->cor + T7_RX_COR_AUDIO_TDM1);
	rx->audio_core_saved[2] = readb(rx->cor + T7_RX_COR_AUDIO_TDM2);
	rx->audio_clk_saved = readl(rx->top + T7_RX_TOP_CLK_CNTL) & BIT(4);
	rx->audio_acr_saved[0] = readl(rx->top + T7_RX_TOP_ACR_CTRL);
	rx->audio_acr_saved[1] = readl(rx->top + T7_RX_TOP_ACR_CTRL2);
	for (i = 0; i < ARRAY_SIZE(t7_hdmirx_audio_core_settings); i++)
		rx->audio_settings_saved[i] =
			readb(rx->cor + t7_hdmirx_audio_core_settings[i][0]);
	rx->audio_active = true;
	for (i = 0; i < ARRAY_SIZE(t7_hdmirx_audio_core_settings); i++)
		writeb(t7_hdmirx_audio_core_settings[i][1],
		       rx->cor + t7_hdmirx_audio_core_settings[i][0]);
	/* The PLL consumes the receiver's ACR path and its 500 MHz reference. */
	writel((7 << 13) | BIT(11) | BIT(8) | BIT(6) | (3 << 4),
	       rx->top + T7_RX_TOP_ACR_CTRL);
	writel(BIT(4) | 7, rx->top + T7_RX_TOP_ACR_CTRL2);
	/* Route the receiver's recovered stereo SPDIF lane into FRHDMIRX. */
	writeb(0, rx->cor + T7_RX_COR_AUDIO_TDM1);
	writeb(0x10, rx->cor + T7_RX_COR_AUDIO_TDM2);
	writeb(0x80, rx->cor + T7_RX_COR_AUDIO_MCLK);
	value = readl(rx->top + T7_RX_TOP_CLK_CNTL);
	writel(value & ~BIT(4), rx->top + T7_RX_TOP_CLK_CNTL);
	writel(0x40001540, rx->phy + T7_RX_AUD_PLL0);
	writel(2 << 2, rx->phy + T7_RX_AUD_PLL2);
	writel(rx->dynamic_modes ? (rx->pll_band < 2 ? 3 : 4 - rx->pll_band) :
		       rx->uhd60 ? 0 : 2, rx->phy + T7_RX_AUD_PLL3);
	writel(0x60001540, rx->phy + T7_RX_AUD_PLL0);
	writel(readl(rx->top + T7_RX_TOP_ACR_CTRL) | BIT(11),
	       rx->top + T7_RX_TOP_ACR_CTRL);
	ret = readl_poll_timeout(rx->phy + T7_RX_AUD_PLL_STATUS, status,
				status & BIT(31), 10, 100000);
	if (ret) {
		t7_hdmirx_audio_stop_locked(rx);
		return ret;
	}
	value = readb(rx->cor + T7_RX_COR_POWER_RESET);
	writeb(value | BIT(1), rx->cor + T7_RX_COR_POWER_RESET);
	udelay(1);
	writeb(value & ~BIT(1), rx->cor + T7_RX_COR_POWER_RESET);
	/* T7 has one stereo lane; no FRHDMIRX IRQ or compressed PAO path. */
	value = BIT(24) | BIT(25) | BIT(26) | (6 << 8) | (0xff << 12);
	writel(value, rx->frhdmirx);
	writel(value | BIT(29), rx->frhdmirx);
	writel(value | BIT(29) | BIT(28), rx->frhdmirx);
	writel(value | BIT(29) | BIT(28) | BIT(31), rx->frhdmirx);
	rx->audio_invalid = false;
	rx->audio_substream = substream;
	return 0;
}

static int t7_hdmirx_audio_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params,
				    struct snd_soc_dai *dai)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dai->dev);
	int ret;

	mutex_lock(&rx->audio_lock);
	ret = t7_hdmirx_audio_hw_params_locked(substream, params, dai);
	mutex_unlock(&rx->audio_lock);
	return ret;
}

static int t7_hdmirx_audio_hw_free(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dai->dev);

	mutex_lock(&rx->audio_lock);
	rx->audio_substream = NULL;
	t7_hdmirx_audio_stop_locked(rx);
	mutex_unlock(&rx->audio_lock);
	return 0;
}

static int t7_hdmirx_audio_startup(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dai->dev);
	unsigned int rate;

	mutex_lock(&rx->audio_lock);
	rate = rx->audio_format.pcm ? rx->audio_format.rate : 0;
	mutex_unlock(&rx->audio_lock);
	/* Keep enumeration available without a source. hw_params still validates
	 * the live input; when qualified, ALSA plug can choose the native rate.
	 */
	if (rate)
		return snd_pcm_hw_constraint_minmax(substream->runtime,
					SNDRV_PCM_HW_PARAM_RATE, rate, rate);
	return 0;
}

static int t7_hdmirx_audio_prepare(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dai->dev);
	int ret;

	mutex_lock(&rx->audio_lock);
	/* An old PCM must close and renegotiate after loss or a format change. */
	ret = rx->audio_invalid || !rx->audio_active ? -ENOLINK : 0;
	mutex_unlock(&rx->audio_lock);
	return ret;
}

static const struct snd_soc_dai_ops t7_hdmirx_audio_ops = {
	.startup = t7_hdmirx_audio_startup,
	.prepare = t7_hdmirx_audio_prepare,
	.hw_params = t7_hdmirx_audio_hw_params,
	.hw_free = t7_hdmirx_audio_hw_free,
};

static struct snd_soc_dai_driver t7_hdmirx_audio_dai = {
	.name = "t7-hdmirx-audio",
	.capture = {
		.stream_name = "HDMI RX Capture",
		.channels_min = 2, .channels_max = 2,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
			 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
			 SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &t7_hdmirx_audio_ops,
};

static const char * const t7_hdmirx_audio_control_names[] = {
	"HDMI RX Audio Rate", "HDMI RX Audio Channels",
	"HDMI RX Audio Generation", "HDMI RX Audio State",
};

static void t7_hdmirx_audio_work(struct work_struct *work)
{
	struct t7_hdmirx *rx = container_of(to_delayed_work(work), struct t7_hdmirx, audio_work);
	struct t7_hdmirx_audio_format format = { 0 };
	struct snd_pcm_substream *stop = NULL;
	unsigned int i;
	int ret;

	mutex_lock(&rx->audio_lock);
	ret = t7_hdmirx_read_audio(rx, &format);
	/* ACR counters can update between byte reads. One incoherent snapshot
	 * is not a source change; bound retries so a persistently invalid packet
	 * still stops capture. A real video/link loss remains immediate.
	 */
	if (ret == -EAGAIN) {
		rx->audio_stable_samples = 0;
		if (++rx->audio_read_retries < 3)
			goto out;
	} else {
		rx->audio_read_retries = 0;
	}
	if (ret) {
		memset(&format, 0, sizeof(format));
		rx->audio_stable_samples = 0;
	} else if (!t7_hdmirx_audio_same(&format, &rx->audio_format)) {
		if (t7_hdmirx_audio_same(&format, &rx->audio_pending))
			rx->audio_stable_samples++;
		else
			rx->audio_stable_samples = 1;
		rx->audio_pending = format;
		/* Mute the old stream immediately; publish a new format only after
		 * consecutive samples. Never play new-rate data with old PCM params.
		 */
		if (rx->audio_stable_samples < 2)
			memset(&format, 0, sizeof(format));
	}
	if (!t7_hdmirx_audio_same(&format, &rx->audio_format)) {
		rx->audio_format = format;
		rx->audio_generation = (rx->audio_generation + 1) & INT_MAX;
		if (rx->audio_active) {
			rx->audio_invalid = true;
			writel(readl(rx->frhdmirx) & ~BIT(31), rx->frhdmirx);
			stop = rx->audio_substream;
		}
		for (i = 0; i < ARRAY_SIZE(t7_hdmirx_audio_control_names); i++) {
			snd_soc_component_notify_control(rx->audio_component,
						 t7_hdmirx_audio_control_names[i]);
		}
	}
out:
	mutex_unlock(&rx->audio_lock);
	/* ALSA takes the stream lock here. Do not invert it with hw_free's
	 * audio mutex; component removal joins this worker before freeing PCMs.
	 */
	if (stop)
		snd_pcm_stop_xrun(stop);
	schedule_delayed_work(&rx->audio_work, msecs_to_jiffies(100));
}

static int t7_hdmirx_audio_control_info(struct snd_kcontrol *control,
				      struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = control->private_value == 0 ? 192000 :
		control->private_value == 1 ? 8 : control->private_value == 2 ? INT_MAX : 2;
	return 0;
}

static int t7_hdmirx_audio_control_get(struct snd_kcontrol *control,
				     struct snd_ctl_elem_value *value)
{
	struct snd_soc_component *component = snd_kcontrol_chip(control);
	struct t7_hdmirx *rx = dev_get_drvdata(component->dev);
	unsigned int result;

	mutex_lock(&rx->audio_lock);
	switch (control->private_value) {
	case 0:
		result = rx->audio_format.rate;
		break;
	case 1:
		result = rx->audio_format.channels;
		break;
	case 2:
		result = rx->audio_generation;
		break;
	default:
		result = !rx->audio_format.rate ? 0 : rx->audio_format.pcm ? 1 : 2;
		break;
	}
	mutex_unlock(&rx->audio_lock);
	value->value.integer.value[0] = result;
	return 0;
}

#define HDMIRX_AUDIO_CONTROL(label, index) { \
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = label, .private_value = index, \
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
	.info = t7_hdmirx_audio_control_info, .get = t7_hdmirx_audio_control_get, \
}

static const struct snd_kcontrol_new t7_hdmirx_audio_controls[] = {
	HDMIRX_AUDIO_CONTROL("HDMI RX Audio Rate", 0),
	HDMIRX_AUDIO_CONTROL("HDMI RX Audio Channels", 1),
	HDMIRX_AUDIO_CONTROL("HDMI RX Audio Generation", 2),
	HDMIRX_AUDIO_CONTROL("HDMI RX Audio State", 3),
};

static int t7_hdmirx_audio_component_probe(struct snd_soc_component *component)
{
	struct t7_hdmirx *rx = dev_get_drvdata(component->dev);

	rx->audio_component = component;
	schedule_delayed_work(&rx->audio_work, 0);
	return 0;
}

static void t7_hdmirx_audio_component_remove(struct snd_soc_component *component)
{
	struct t7_hdmirx *rx = dev_get_drvdata(component->dev);

	cancel_delayed_work_sync(&rx->audio_work);
	rx->audio_component = NULL;
}

static const struct snd_soc_component_driver t7_hdmirx_audio_component = {
	.name = "t7-hdmirx-audio",
	.probe = t7_hdmirx_audio_component_probe,
	.remove = t7_hdmirx_audio_component_remove,
	.controls = t7_hdmirx_audio_controls,
	.num_controls = ARRAY_SIZE(t7_hdmirx_audio_controls),
};

static int t7_hdmirx_audio_register(struct platform_device *pdev, struct t7_hdmirx *rx)
{
	struct device *dev = &pdev->dev;
	struct resource *phy = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	int ret;

	if (!audio_test)
		return 0;
	INIT_DELAYED_WORK(&rx->audio_work, t7_hdmirx_audio_work);
	if (!core_test || !phy || resource_size(phy) < 0x90)
		return -EINVAL;
	rx->frhdmirx = devm_platform_ioremap_resource_byname(pdev, "frhdmirx");
	if (IS_ERR(rx->frhdmirx))
		return PTR_ERR(rx->frhdmirx);
	rx->audio_clocks[0].id = "audio-pll";
	rx->audio_clocks[1].id = "frhdmirx";
	rx->audio_clocks[2].id = "acr";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(rx->audio_clocks), rx->audio_clocks);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, t7_hdmirx_audio_stop, rx);
	if (ret)
		return ret;
	return devm_snd_soc_register_component(dev, &t7_hdmirx_audio_component,
					     &t7_hdmirx_audio_dai, 1);
}

int t7_hdmirx_get_edid(struct device *dev, struct v4l2_edid *edid)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);
	unsigned int blocks;
	int ret = 0;

	if (!rx || !rx->link || edid->pad)
		return -EINVAL;
	mutex_lock(&rx->link->lock);
	blocks = rx->edid_blob.size / T7_RX_EDID_BLOCK_SIZE;
	if (!edid->blocks) {
		edid->blocks = blocks;
	} else if (edid->start_block >= blocks) {
		ret = -ENODATA;
	} else {
		edid->blocks = min(edid->blocks, blocks - edid->start_block);
		memcpy(edid->edid, rx->edid + edid->start_block * T7_RX_EDID_BLOCK_SIZE,
		       edid->blocks * T7_RX_EDID_BLOCK_SIZE);
	}
	mutex_unlock(&rx->link->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(t7_hdmirx_get_edid);

int t7_hdmirx_set_edid(struct device *dev, const struct v4l2_edid *edid)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);
	u8 previous[T7_RX_EDID_CAPACITY];
	size_t size, previous_size;
	u32 saved_reset, saved_port;
	int ret = 0;

	if (!rx || !rx->link || edid->pad || edid->start_block)
		return -EINVAL;
	if (edid->blocks > T7_RX_EDID_CAPACITY / T7_RX_EDID_BLOCK_SIZE)
		return -E2BIG;
	size = edid->blocks * T7_RX_EDID_BLOCK_SIZE;
	if (size) {
		ret = t7_hdmirx_validate_edid(edid->edid, size);
		if (ret)
			return ret;
	}
	mutex_lock(&rx->link->lock);
	if (rx->edid_blob.size == size && (!size || !memcmp(rx->edid, edid->edid, size)))
		goto out;
	if (rx->link->users) {
		ret = -EBUSY;
		goto out;
	}
	if (!size) {
		t7_hdmirx_ddc_stop(rx);
		goto out;
	}
	previous_size = rx->edid_blob.size;
	memcpy(previous, rx->edid, previous_size);
	saved_reset = rx->saved_reset;
	saved_port = rx->saved_port_sel;
	ret = t7_hdmirx_ddc_start(rx, edid->edid, size);
	if (ret && previous_size)
		t7_hdmirx_ddc_start(rx, previous, previous_size);
	/* Boot defers UHD HPD until core_start has enabled SCDC. Runtime
	 * replacement already has that core, so finish the disconnect pulse.
	 */
	if (rx->ddc_active && (!rx->uhd60 || rx->core_active))
		t7_hdmirx_set_hpd(rx, true);
	/* Runtime updates must not replace the pre-probe teardown snapshot. */
	rx->saved_reset = saved_reset;
	rx->saved_port_sel = saved_port;
out:
	mutex_unlock(&rx->link->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(t7_hdmirx_set_edid);

int t7_hdmirx_stream_get(struct device *dev, unsigned int width, unsigned int height)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);
	struct t7_hdmirx_measurement measured;
	struct t7_hdmirx_color color = { 0 };
	bool yuv420;
	int ret;

	if (!rx || !rx->link)
		return -ENOLINK;
	mutex_lock(&rx->link->lock);
	ret = t7_hdmirx_check_video(dev);
	yuv420 = t7_hdmirx_hdr_input(rx) || t7_hdmirx_sdr420_input(rx);
	if (!ret && rx->dynamic_modes) {
		ret = t7_hdmirx_read_color(rx, &color);
		yuv420 = color.space == 3;
		if (!ret)
			ret = t7_hdmirx_measure(rx, yuv420, &measured);
		if (!ret && (width != measured.width * (yuv420 ? 2 : 1) ||
			     height != measured.height))
			ret = -EPIPE;
		if (!ret)
			rx->lease_mode = measured;
	} else if (!ret && (width != (rx->uhd60 ? 3840 : 1920) ||
		     height != (rx->uhd60 ? 2160 : 1080)))
		ret = -EPIPE;
	if (!ret) {
		if (rx->dynamic_modes && !rx->link->users) {
			u32 control = readl(rx->top + T7_RX_TOP_VID_CNTL);

			rx->lease_color = color;
			/* T7's 422 unpacker uses all three input-mapping selectors at 3. */
			writeb(color.space == 1 ? 0xd8 : 0x40, rx->cor + T7_RX_COR_INPUT_MAP);
			writeb(color.space == 1 ? 6 : 4, rx->cor + T7_RX_COR_INPUT_MAP + 1);
			control &= ~GENMASK(26, 24);
			control |= (color.space == 1 ? 1 : color.space == 2 ? 2 : 0) << 24;
			writel(control, rx->top + T7_RX_TOP_VID_CNTL);
		}
		rx->lease_hdr = t7_hdmirx_hdr_input(rx);
		rx->lease_420 = rx->dynamic_modes ? color.space == 3 :
			rx->lease_hdr || t7_hdmirx_sdr420_input(rx);
		rx->lease_yuv = rx->dynamic_modes ? color.space != 0 :
			rx->lease_420 || t7_hdmirx_sdr444_input(rx);
		rx->link->users++;
	}
	mutex_unlock(&rx->link->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(t7_hdmirx_stream_get);

bool t7_hdmirx_stream_is_hdr(struct device *dev)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);

	return READ_ONCE(rx->lease_hdr);
}
EXPORT_SYMBOL_GPL(t7_hdmirx_stream_is_hdr);

bool t7_hdmirx_stream_is_yuv(struct device *dev)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);

	return READ_ONCE(rx->lease_yuv);
}
EXPORT_SYMBOL_GPL(t7_hdmirx_stream_is_yuv);

bool t7_hdmirx_stream_needs_yuv444_swap(struct device *dev)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);

	return !rx->dynamic_modes && READ_ONCE(rx->lease_yuv) && !READ_ONCE(rx->lease_420);
}
EXPORT_SYMBOL_GPL(t7_hdmirx_stream_needs_yuv444_swap);

bool t7_hdmirx_stream_is_full_range(struct device *dev)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);

	return rx->dynamic_modes ? READ_ONCE(rx->lease_color.full) : !rx->lease_yuv;
}
EXPORT_SYMBOL_GPL(t7_hdmirx_stream_is_full_range);

bool t7_hdmirx_stream_is_bt601(struct device *dev)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);

	return rx->dynamic_modes && READ_ONCE(rx->lease_color.colorimetry) == 1;
}
EXPORT_SYMBOL_GPL(t7_hdmirx_stream_is_bt601);

void t7_hdmirx_stream_put(struct device *dev)
{
	struct t7_hdmirx *rx = dev_get_drvdata(dev);

	mutex_lock(&rx->link->lock);
	if (WARN_ON(!rx->link->users)) {
		mutex_unlock(&rx->link->lock);
		return;
	}
	rx->link->users--;
	mutex_unlock(&rx->link->lock);
}
EXPORT_SYMBOL_GPL(t7_hdmirx_stream_put);

static bool t7_hdmirx_sdr_cable(u32 cable)
{
	return cable >= 146500000 && cable <= 150500000;
}

static bool t7_hdmirx_cable_supported(struct t7_hdmirx *rx, u32 cable, bool hdmi2)
{
	if (rx->dynamic_modes)
		return cable >= 24000000 && (u64)cable * (hdmi2 ? 4 : 1) < 600000000;
	if (t7_hdmirx_sdr_cable(cable))
		return true;
	/* 4K60 YCbCr420 10-bit uses a 371.25 MHz character clock at 1:40.
	 * Its PHY_BW_4 coefficients and PLL divider match the UHD RGB8 trial.
	 * Admission here only permits PHY observation; video_status still rejects
	 * YCbCr/deep-colour input before any capture lease can be acquired.
	 */
	return rx->hdr_probe && hdmi2 && cable >= 90812500 && cable <= 94812500;
}

static void t7_hdmirx_select_phy(struct t7_hdmirx *rx, u32 cable, bool hdmi2)
{
	u32 clock = cable * (hdmi2 ? 4 : 1);

	/* T7 has different first PLL and PHY thresholds (35 versus 45 MHz). */
	rx->phy_band = clock < 45000000 ? 0 : clock < 77000000 ? 1 :
		clock < 155000000 ? 2 : clock < 340000000 ? 3 : clock < 525000000 ? 4 : 5;
	rx->pll_band = clock < 35000000 ? 0 : clock < 75000000 ? 1 :
		clock < 155000000 ? 2 : clock < 340000000 ? 3 : 4;
}

static void t7_hdmirx_link_work(struct work_struct *work)
{
	struct t7_hdmirx_link *link = container_of(to_delayed_work(work),
						struct t7_hdmirx_link, work);
	struct t7_hdmirx *rx = link->rx;
	u32 cable = 0;
	bool next_uhd, yuv;
	bool sampled = false;
	u32 previous_pll;
	struct t7_hdmirx_color color;
	int ret;

	mutex_lock(&link->lock);
	t7_hdmirx_update_drm(rx);
	if (rx->dynamic_modes && rx->core_active && !link->users)
		writeb(readb(rx->cor + T7_RX_COR_AUDIO_PACKET) & BIT(0) ? 6 : 0,
		       rx->cor + T7_RX_COR_PREAMBLE);
	ret = t7_hdmirx_video_status(rx);
	if (ret == -EAGAIN) {
		/* One torn snapshot is harmless. A stuck FDET can change on every
		 * read indefinitely; after a second, qualify clocks and recover PCS.
		 * The users check below still protects live or quarantined DMA.
		 */
		if (link->timing_retries < 4) {
			link->timing_retries++;
			goto out;
		}
	} else {
		link->timing_retries = 0;
	}
	if (rx->dynamic_modes && link->configured && !link->users) {
		cable = t7_hdmirx_sample_cable(rx, clk_get_rate(rx->clocks[T7_RX_METER].clk));
		sampled = true;
		if (abs((int)cable - (int)link->previous_cable) > 500000 ||
		    !!(readb(rx->cor + T7_RX_COR_TMDS_CONFIG) & BIT(1)) != link->previous_uhd)
			link->configured = false;
	}
	yuv = t7_hdmirx_hdr_input(rx) || t7_hdmirx_sdr420_input(rx);
	if (!ret && link->configured &&
	    (rx->dynamic_modes || !rx->hdr_probe ||
	     (t7_hdmirx_sdr_cable(link->previous_cable) && !yuv) ||
	     (link->previous_cable && !t7_hdmirx_sdr_cable(link->previous_cable) && yuv))) {
		if (!READ_ONCE(link->ready)) {
			usleep_range(20000, 21000);
			ret = t7_hdmirx_video_status(rx);
		}
		WRITE_ONCE(link->ready, !ret);
		goto out;
	}
	WRITE_ONCE(link->ready, false);
	/* A live or quarantined MIF owns its receiver until DMA is safe. */
	if (link->users) {
		link->configured = false;
		goto out;
	}
	/* SDR pixels after observation still need a newly qualified clock. */
	if (!ret)
		link->configured = false;
	if (!sampled)
		cable = t7_hdmirx_sample_cable(rx, clk_get_rate(rx->clocks[T7_RX_METER].clk));
	rx->cable_hz = cable;
	next_uhd = !!(readb(rx->cor + T7_RX_COR_TMDS_CONFIG) & BIT(1));
	if (!(readl(rx->top + T7_RX_TOP_HPD_PWR5V) & BIT(20 + rx->port)) ||
	    (!rx->dynamic_modes && next_uhd != rx->uhd60) ||
	    !t7_hdmirx_cable_supported(rx, cable, next_uhd)) {
		link->stable_samples = 0;
		link->previous_cable = 0;
		link->configured = false;
		goto out;
	}
	/* Keep an unsupported but aligned probe signal intact for packet reads.
	 * A supported colour with malformed FDET geometry still needs PCS recovery:
	 * alignment alone can survive a mode switch with height zero/width +2.
	 * Remember its reference clock so returning SDR is requalified, including
	 * fresh TMDS meter counts for the subsequent ALSA N/CTS validation.
	 */
	if (rx->hdr_probe && ret == -ERANGE && link->configured &&
	    (!rx->dynamic_modes || t7_hdmirx_read_color(rx, &color) == -ERANGE) &&
	    (readl(rx->phy + T7_RX_PHY_APLL0) & T7_RX_PHY_PLL_LOCK) &&
	    (readl(rx->top + T7_RX_TOP_TMDS_ALIGN_STAT) & BIT(0))) {
		link->previous_cable = cable;
		link->previous_uhd = next_uhd;
		link->stable_samples = 0;
		goto out;
	}
	if (link->previous_cable && link->previous_uhd == next_uhd &&
	    abs((int)cable - (int)link->previous_cable) <= 500000)
		link->stable_samples++;
	else
		link->stable_samples = 0;
	link->previous_cable = cable;
	link->previous_uhd = next_uhd;
	if (link->stable_samples < 3)
		goto out;
	link->stable_samples = 0;
	mutex_lock(&rx->audio_lock);
	previous_pll = rx->dynamic_modes ? rx->pll_band : rx->uhd60 ? 4 : 2;
	if (rx->dynamic_modes)
		t7_hdmirx_select_phy(rx, cable, next_uhd);
	if (rx->dynamic_modes || rx->uhd60 != next_uhd) {
		WRITE_ONCE(rx->uhd60, next_uhd);
		/* ACR CTS division tracks PHY_BW_2 versus PHY_BW_5. Preserve ALSA
		 * routing and DMA while resetting only the recovered audio PLL.
		 */
		if (rx->audio_active && previous_pll != (rx->dynamic_modes ? rx->pll_band : next_uhd ? 4 : 2)) {
			static const u32 audio_div[] = { 3, 3, 2, 1, 0 };

			writel(0x40001540, rx->phy + T7_RX_AUD_PLL0);
			writel(audio_div[rx->dynamic_modes ? rx->pll_band : next_uhd ? 4 : 2],
			       rx->phy + T7_RX_AUD_PLL3);
			writel(0x60001540, rx->phy + T7_RX_AUD_PLL0);
		}
		t7_hdmirx_phy_configure(rx);
	}
	ret = t7_hdmirx_phy_pll(rx);
	mutex_unlock(&rx->audio_lock);
	if (ret)
		goto out;
	/* Video PLL/PCS only: keep DDC/HPD and the audio PLL configuration. */
	t7_hdmirx_phy_equalize(rx);
	t7_hdmirx_reset_pcs(rx);
	link->configured = true;
	ret = t7_hdmirx_video_status(rx);
	if (!ret) {
		usleep_range(20000, 21000);
		ret = t7_hdmirx_video_status(rx);
	}
	WRITE_ONCE(link->ready, !ret);
out:
	mutex_unlock(&link->lock);
	schedule_delayed_work(&link->work, msecs_to_jiffies(250));
}

static void t7_hdmirx_link_stop(void *data)
{
	struct t7_hdmirx_link *link = data;

	cancel_delayed_work_sync(&link->work);
	WRITE_ONCE(link->ready, false);
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
	seq_printf(s, "phy_active: %u\n", rx->phy_active);
	seq_printf(s, "dynamic_modes: %u\n", rx->dynamic_modes);
	seq_printf(s, "hdr_probe: %u\n", rx->hdr_probe);
	seq_printf(s, "hdr_capture: %u\n", rx->hdr_capture);
	seq_printf(s, "hdr_input: %u\n", t7_hdmirx_hdr_input(rx));
	seq_printf(s, "drm_received_ns: %llu\n", READ_ONCE(rx->drm_received_ns));
	seq_printf(s, "selected_uhd: %u\n", READ_ONCE(rx->uhd60));
	seq_printf(s, "phy_band: %u\n", READ_ONCE(rx->phy_band));
	seq_printf(s, "pll_band: %u\n", READ_ONCE(rx->pll_band));
	seq_printf(s, "core_active: %u\n", rx->core_active);
	if (rx->link) {
		seq_printf(s, "link_ready: %u\n", READ_ONCE(rx->link->ready));
		seq_printf(s, "capture_users: %u\n", READ_ONCE(rx->link->users));
	}
	if (rx->core_active) {
		u8 avi[17], unrec[31], aif[14];
		u32 ticks = t7_hdmirx_core_read16(rx, T7_RX_COR_FRAME_TICKS) |
			    (readb(rx->cor + T7_RX_COR_FRAME_TICKS + 2) << 16);

		seq_printf(s, "core_state: %#x\n", readb(rx->cor + T7_RX_COR_STATE));
		seq_printf(s, "core_smc_result: %#lx\n", rx->core_smc_result);
		seq_printf(s, "core_sys_ctrl: %#x\n", readb(rx->cor + T7_RX_COR_SYS_CTRL));
		seq_printf(s, "core_phyck: %#x\n", readb(rx->cor + T7_RX_COR_PHY_CLOCK1));
		seq_printf(s, "core_aon_reset: %#x\n", readb(rx->cor + T7_RX_COR_AON_RESET));
		seq_printf(s, "core_fdet: %#x\n", readb(rx->cor + T7_RX_COR_FRAME_STATUS));
		seq_printf(s, "color_depth_code: %#x\n", readb(rx->cor + T7_RX_COR_COLOR_DEPTH));
		/* Raw diagnostic snapshots; no claim of packet freshness or HDR readiness. */
		for (i = 0; i < ARRAY_SIZE(avi); i++)
			avi[i] = readb(rx->cor + T7_RX_COR_AVI_PACKET + i);
		seq_printf(s, "avi_packet: %*phN\n", (int)sizeof(avi), avi);
		if (rx->hdr_probe) {
			for (i = 0; i < ARRAY_SIZE(unrec); i++)
				unrec[i] = readb(rx->cor + T7_RX_COR_UNREC_PACKET + i);
			seq_printf(s, "drm_candidate: %*phN\n", (int)sizeof(unrec), unrec);
		}
		seq_printf(s, "width: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_WIDTH));
		seq_printf(s, "height: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_HEIGHT));
		seq_printf(s, "htotal: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC_LOW) +
			   t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC_HIGH));
		seq_printf(s, "vtotal: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC_LOW) +
			   t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC_HIGH));
		seq_printf(s, "hsync_high: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_HSYNC_HIGH));
		seq_printf(s, "vsync_high: %u\n", t7_hdmirx_core_read16(rx, T7_RX_COR_VSYNC_HIGH));
		seq_printf(s, "frame_ticks: %u\n", ticks);
		seq_printf(s, "audio_n: %u\n", t7_hdmirx_audio_word(rx, T7_RX_COR_N));
		seq_printf(s, "audio_cts: %u\n", t7_hdmirx_audio_word(rx, T7_RX_COR_CTS));
		seq_printf(s, "audio_info: %#x\n", readb(rx->cor + T7_RX_COR_AUDIO_INFO));
		seq_printf(s, "audio_packet: %#x\n", readb(rx->cor + T7_RX_COR_AUDIO_PACKET));
		for (i = 0; i < ARRAY_SIZE(aif); i++)
			aif[i] = readb(rx->cor + T7_RX_COR_AUDIO_AIF + i);
		seq_printf(s, "audio_aif: %*phN\n", (int)sizeof(aif), aif);
		mutex_lock(&rx->audio_lock);
		seq_printf(s, "audio_rate: %u\n", rx->audio_format.rate);
		seq_printf(s, "audio_channels: %u\n", rx->audio_format.channels);
		seq_printf(s, "audio_pcm: %u\n", rx->audio_format.pcm);
		seq_printf(s, "audio_generation: %u\n", rx->audio_generation);
		seq_printf(s, "audio_active: %u\n", rx->audio_active);
		mutex_unlock(&rx->audio_lock);
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

	if (uhd60 && !core_test)
		return dev_err_probe(dev, -EINVAL,
				     "UHD trial requires video core initialization\n");
	if (phy_test && !edid_firmware)
		return dev_err_probe(dev, -EINVAL, "PHY trial requires an EDID firmware\n");
	if (core_test && !phy_test)
		return dev_err_probe(dev, -EINVAL, "Core trial requires PHY initialization\n");
	if (hdr_probe && !dynamic_modes)
		return dev_err_probe(dev, -EINVAL, "HDR observation requires dynamic_modes=1\n");
	rx = devm_kzalloc(dev, sizeof(*rx), GFP_KERNEL);
	if (!rx)
		return -ENOMEM;
	ret = device_property_read_u32(dev, "amlogic,input-port", &rx->port);
	if (ret || rx->port > 2)
		return dev_err_probe(dev, -EINVAL, "Require input port 0..2\n");
	rx->uhd60 = uhd60;
	if (dynamic_modes && !uhd60)
		return -EINVAL;
	rx->dynamic_modes = dynamic_modes;
	rx->hdr_probe = hdr_probe;
	rx->hdr_capture = hdr_capture && hdr_probe && dynamic_modes;
	mutex_init(&rx->audio_lock);
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
		ret = 0;
		if (phy_test)
			ret = uhd60 ? t7_hdmirx_validate_uhd_edid(firmware->data, firmware->size) :
				      t7_hdmirx_validate_phy_edid(firmware->data, firmware->size);
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
		if (core_test) {
			ret = devm_add_action_or_reset(dev, t7_hdmirx_core_stop, rx);
			if (ret)
				return ret;
			/* Core isolation must be removed before cable clock qualification. */
			ret = t7_hdmirx_core_start(rx);
			if (ret)
				return dev_err_probe(dev, ret, "Cannot initialize receiver core\n");
		}
		if (core_test) {
			rx->link = devm_kzalloc(dev, sizeof(*rx->link), GFP_KERNEL);
			if (!rx->link)
				return -ENOMEM;
			rx->link->rx = rx;
			mutex_init(&rx->link->lock);
			INIT_DELAYED_WORK(&rx->link->work, t7_hdmirx_link_work);
			ret = devm_add_action_or_reset(dev, t7_hdmirx_link_stop, rx->link);
			if (ret)
				return ret;
			schedule_delayed_work(&rx->link->work, 0);
		} else {
			ret = t7_hdmirx_wait_cable(rx, clk_get_rate(rx->clocks[T7_RX_METER].clk));
			if (!ret)
				ret = t7_hdmirx_phy_pll(rx);
			if (ret)
				return dev_err_probe(dev, ret, "PHY trial failed\n");
			t7_hdmirx_phy_equalize(rx);
		}
	}
	ret = t7_hdmirx_audio_register(pdev, rx);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register experimental HDMI audio\n");
	debugfs = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(debugfs))
		return PTR_ERR(debugfs);
	debugfs_create_devm_seqfile(dev, "status", debugfs, t7_hdmirx_status);
	debugfs_create_blob("edid", 0400, debugfs, &rx->edid_blob);
	/* Remove files before their managed callback context or clocks disappear. */
	ret = devm_add_action_or_reset(dev, t7_hdmirx_debugfs_remove, debugfs);
	if (ret)
		return ret;
	dev_info(dev, "receiver ready, input port %u; waiting for a stable source\n",
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
