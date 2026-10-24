// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Amlogic T7 HDMI 2.1 transmitter ("hdmitx21") bridge
 *
 * The controller is not a DesignWare core: it has a TOP block (32-bit
 * registers) and a COR block (8-bit registers). The VPU side (VENC, video
 * clocks, HDMI PLL) is set by the meson DRM driver. This bridge sets the
 * controller, the DDC master, the HDMI PHY and the AVI InfoFrame.
 *
 * Register sequences follow the Amlogic vendor driver (hdmitx21).
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_plane.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/display/drm_hdmi_helper.h>
#include <drm/display/drm_hdmi_state_helper.h>
#include <sound/hdmi-codec.h>
#include <linux/hdmi.h>
#include <linux/media-bus-format.h>

#include "meson_drv.h"
#include "meson_t7_handoff.h"
#include "meson_t7_modeset.h"

/* TOP registers (32-bit, byte offsets) */
#define TOP_SW_RESET		0x000
#define  TOP_SW_RESET_GLITCH	BIT(6)
#define  TOP_SW_RESET_I2C	BIT(9)
#define TOP_CLK_CNTL		0x004
#define  TOP_CLK_CNTL_AUD_MCLK_SPDIF	BIT(13)
#define TOP_HPD_FILTER		0x008
#define TOP_INTR_MASKN		0x00c
#define TOP_INTR_STAT		0x010
#define TOP_INTR_STAT_CLR	0x014
#define  TOP_INTR_HPD_RISE	BIT(1)
#define  TOP_INTR_HPD_FALL	BIT(2)
#define TOP_STAT0		0x038
#define  TOP_STAT0_HPD		BIT(0)
#define TOP_BIST_CNTL		0x018
#define TOP_TMDS_CLK_PTTN_01	0x028
#define TOP_TMDS_CLK_PTTN_23	0x02c
#define TOP_TMDS_CLK_PTTN_CNTL	0x030
#define TOP_INFILTER		0x074
#define TOP_SEC_SCRATCH		0x07c
#define TOP_HS_INTR_CNTL	0x084
#define TOP_HV_ACTIVE		0x08c
#define TOP_I2C_BUSY_CNT_MAX	0x0a0
#define TOP_SEC_VIDEO_OVR	0x0cc
#define TOP_SEC_VIDEO_OVR_0	0x0d0
#define TOP_SEC_VIDEO_OVR_1	0x0d4

/* COR registers (8-bit, byte offsets) */
#define INTR3_MASK		0x001
#define DDC_ADDR_REG		0x003
#define DDC_SEGM		0x004
#define DDC_OFFSET		0x005
#define DDC_DIN_CNT1		0x006
#define DDC_DIN_CNT2		0x007
#define DDC_STATUS		0x008
#define  DDC_STATUS_BUSY	BIT(4)
#define  DDC_STATUS_NACK	BIT(5)
#define  DDC_STATUS_BUS_LOW	BIT(6)
#define DDC_CMD			0x009
#define  DDC_CMD_SEQ_READ	0x02
#define  DDC_CMD_SEQ_WRITE	0x06
#define  DDC_CMD_ENH_READ	0x04
#define  DDC_CMD_CLR_FIFO	0x09
#define  DDC_CMD_ABORT		0x0f
#define  DDC_CMD_CLK_RESET	0x0a
#define DDC_DATA		0x00a
#define DDC_DOUT_CNT		0x00b
#define AON_CYP_CTL		0x018
#define SOC_FUNC_SEL		0x10b
#define CLKPWD			0x10d
#define PWD_SRST		0x110
#define PCLK2TMDS_MISC0		0x12a
#define PCLK2TMDS_MISC1		0x12b
#define CLK_DIV_CNTRL		0x12f
#define HDCP_CTRL		0x20f
#define CLKRATIO		0x235
#define P2T_CTRL		0x236
#define HBLANK_REKEY_CONTROL	0x23c
#define TMDS_CTRL		0x267
#define FRL_CTRL		0x29d
#define TEST_TXCTRL		0x2ca
#define H21TXSB_CTRL		0x319
#define TPI_SC			0x61a
#define  TPI_SC_HDMI_MODE	BIT(0)
#define  TPI_SC_AVMUTE		BIT(3)
#define  TPI_SC_AVMUTE_UPD	BIT(7)
#define PKT_FILTER_0		0x245
#define TPI_INFO_FSEL		0x6bf
#define  TPI_INFO_FSEL_AVI	0
#define  TPI_INFO_FSEL_AUDIO	2
#define  TPI_INFO_FSEL_VENDOR	5
#define  TPI_INFO_FSEL_DRM	6
#define TPI_INFO_B0		0x6c0
#define TPI_INFO_EN		0x6df
#define GCP_CNTL		0x6e1
#define VTEM_CTRL		0x6ed
#define LM_DDC			0x29f
#define TPI_DDC_MASTER_EN	0x6f8
#define  TPI_DDC_HW_EN		BIT(7)
#define HDCP2X_TX_SRST		0x8b5
#define HDCP2X_CTL_0		0x8bd
#define SCRCTL			0x900
#define ACR_CTRL		0xa01
#define  ACR_CTRL_EN		BIT(1)
#define FREQ_SVAL		0xa02
#define N_SVAL1			0xa03
#define N_SVAL2			0xa04
#define N_SVAL3			0xa05
#define ACR_CTS_CLK_DIV		0xa0e
#define AUD_EN			0xa13
#define AUD_MODE		0xa14
#define SPDIF_SSMPL2		0xa19
#define SPDIF_ERTH		0xa1b
#define I2S_IN_MAP		0xa1c
#define I2S_IN_CTRL		0xa1d
#define I2S_CHST0		0xa1e
#define ASRC			0xa25
#define I2S_IN_SIZE		0xa26
#define AIP_RST			0xa2c
#define AIP_HDMI2MHL		0xa2d
#define AUDP_TXCTRL		0xa2f
#define  AUDP_TXCTRL_MUTE	BIT(7)
#define TPI_AUD_CONFIG		0xa62
#define  TPI_AUD_CONFIG_MUTE	BIT(4)
#define VP_INPUT_MASK		0xb18
#define VP_INPUT_SYNC_ADJUST	0xb1c
#define VP_OUTPUT_MAPPING	0xb44
#define VP_OUTPUT_MAPPING_HI	0xb45
#define VP_OUTPUT_MASK		0xb46
#define CSC0_CTRL		0xd20
#define CSC0_CTRL2		0xd21
#define CSC1_CTRL		0xda0
#define CSC1_CTRL2		0xda1

/* PHY registers, ANACTRL_HDMIPHY_CTRL0 at offset 0 */
#define PHY_CTRL0		0x200
#define PHY_CTRL1		0x204
#define PHY_CTRL3		0x20c
#define PHY_CTRL5		0x214

/* PHY bandgap only: needed for DDC while the TMDS output is off */
#define PHY_CTRL0_BANDGAP	0x0b4242

#define DDC_SEGMENT_ADDR	0x30
#define DDC_EDID_ADDR		0x50
#define DDC_SCDC_ADDR		0x54
#define SCDC_TMDS_CONFIG	0x20

struct meson_hdmitx21 {
	struct device *dev;
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;
	resource_size_t boot_top, boot_cor;
	struct regmap *hhi;	/* analog control block with the PHY, from meson DRM */
	struct clk *pclk;
	struct i2c_adapter ddc;
	struct mutex ddc_lock;	/* DDC master access */

	/* Audio: kept to set up again after a mode change */
	struct mutex audio_lock;	/* audio state */
	struct mutex packet_lock; /* Shared TPI InfoFrame selector and payload window. */
	bool audio_enabled;
	bool audio_muted;
	bool sink_hdmi;
	struct hdmi_codec_params audio_params;
	unsigned long long tmds_hz;
	struct gpio_desc *hpd;
	struct delayed_work hpd_work;
	enum drm_connector_status hpd_sample, hpd_reported;
	bool hpd_polling;
	bool boot_monitor;
	bool enc2_modeset;
	bool atomic_color;
	void __iomem *phy;
};

static inline struct meson_hdmitx21 *bridge_to_hdmitx(struct drm_bridge *bridge)
{
	return container_of(bridge, struct meson_hdmitx21, bridge);
}

/* T7 TOP/COR are secure: reg_ops.c uses width-specific firmware calls. */
static u32 meson_hdmitx21_smc_read(resource_size_t address, unsigned int width)
{
	struct arm_smccc_res result;

	arm_smccc_smc(0x82000018, address, width, 0, 0, 0, 0, 0, &result);
	return result.a0;
}

static void meson_hdmitx21_smc_write(resource_size_t address, u32 value,
				    unsigned int width)
{
	struct arm_smccc_res result;

	arm_smccc_smc(0x82000019, address, value, width, 0, 0, 0, 0, &result);
}

static u32 top_read(struct meson_hdmitx21 *tx, u32 reg)
{
	return meson_hdmitx21_smc_read(tx->boot_top + reg, 32);
}

static void top_write(struct meson_hdmitx21 *tx, u32 reg, u32 val)
{
	meson_hdmitx21_smc_write(tx->boot_top + reg, val, 32);
}

static u8 cor_read(struct meson_hdmitx21 *tx, u32 reg)
{
	return meson_hdmitx21_smc_read(tx->boot_cor + reg, 8);
}

static void cor_write(struct meson_hdmitx21 *tx, u32 reg, u8 val)
{
	meson_hdmitx21_smc_write(tx->boot_cor + reg, val, 8);
}

static u32 phy_read(struct meson_hdmitx21 *tx, u32 reg)
{
	u32 val = 0;

	if (tx->phy)
		return readl(tx->phy + reg - PHY_CTRL0);
	regmap_read(tx->hhi, reg, &val);

	return val;
}

static void phy_write(struct meson_hdmitx21 *tx, u32 reg, u32 val)
{
	if (tx->phy) {
		writel(val, tx->phy + reg - PHY_CTRL0);
		return;
	}
	regmap_write(tx->hhi, reg, val);
}

static void cor_update(struct meson_hdmitx21 *tx, u32 reg, u8 mask, u8 val)
{
	cor_write(tx, reg, (cor_read(tx, reg) & ~mask) | (val & mask));
}

static void top_pulse(struct meson_hdmitx21 *tx, u32 bit)
{
	top_write(tx, TOP_SW_RESET, top_read(tx, TOP_SW_RESET) | bit);
	usleep_range(1000, 2000);
	top_write(tx, TOP_SW_RESET, top_read(tx, TOP_SW_RESET) & ~bit);
	usleep_range(1000, 2000);
}

static void cor_pulse(struct meson_hdmitx21 *tx, u32 reg, u8 bit)
{
	cor_update(tx, reg, bit, bit);
	cor_update(tx, reg, bit, 0);
}

static void meson_hdmitx21_set_avmute(struct meson_hdmitx21 *tx, bool mute)
{
	cor_update(tx, TPI_SC, TPI_SC_AVMUTE_UPD, 0);
	cor_update(tx, TPI_SC, TPI_SC_AVMUTE, 0);
	if (mute)
		cor_update(tx, TPI_SC, TPI_SC_AVMUTE, TPI_SC_AVMUTE);
	else
		cor_update(tx, TPI_SC, TPI_SC_AVMUTE_UPD, TPI_SC_AVMUTE_UPD);
}

/* DDC master */

/*
 * The DDC master drops a command written right after the previous one.
 * The vendor driver does not see this: each of its register accesses is
 * a slow secure monitor call.
 */
static void meson_hdmitx21_ddc_cmd(struct meson_hdmitx21 *tx, u8 cmd)
{
	cor_write(tx, DDC_CMD, cmd);
	usleep_range(100, 200);
}

static void meson_hdmitx21_ddc_reset(struct meson_hdmitx21 *tx)
{
	cor_update(tx, TPI_DDC_MASTER_EN, TPI_DDC_HW_EN, TPI_DDC_HW_EN);
	meson_hdmitx21_ddc_cmd(tx, DDC_CMD_ABORT);
	meson_hdmitx21_ddc_cmd(tx, DDC_CMD_CLK_RESET);
	cor_update(tx, TPI_DDC_MASTER_EN, TPI_DDC_HW_EN, 0);
}

static int meson_hdmitx21_ddc_wait_idle(struct meson_hdmitx21 *tx)
{
	int i;

	for (i = 0; i < 10; i++) {
		if (!(cor_read(tx, DDC_STATUS) & DDC_STATUS_BUSY))
			return 0;
		if (i == 4)
			meson_hdmitx21_ddc_reset(tx);
		usleep_range(2000, 2500);
	}

	return -EBUSY;
}

static int meson_hdmitx21_ddc_read(struct meson_hdmitx21 *tx, u8 address, u8 segment,
				   u8 offset, u8 *buf, unsigned int len)
{
	unsigned int timeout = len * 12 / 10 + 10;
	int ret = 0;
	u8 n, st;

	ret = meson_hdmitx21_ddc_wait_idle(tx);
	if (ret)
		return ret;

	cor_update(tx, TPI_DDC_MASTER_EN, TPI_DDC_HW_EN, TPI_DDC_HW_EN);
	cor_write(tx, DDC_ADDR_REG, address << 1);
	cor_write(tx, DDC_SEGM, segment);
	cor_write(tx, DDC_OFFSET, offset);
	cor_write(tx, DDC_DIN_CNT2, len >> 8);
	cor_write(tx, DDC_DIN_CNT1, len & 0xff);
	meson_hdmitx21_ddc_cmd(tx, DDC_CMD_CLR_FIFO);
	meson_hdmitx21_ddc_cmd(tx, segment ? DDC_CMD_ENH_READ : DDC_CMD_SEQ_READ);
	usleep_range(2000, 3000);

	while (len && timeout) {
		n = cor_read(tx, DDC_DOUT_CNT) & 0x1f;
		if (n) {
			if (n > len || n > 16) {
				ret = -EIO;
				break;
			}
			len -= n;
			while (n--)
				*buf++ = cor_read(tx, DDC_DATA);
			continue;
		}

		usleep_range(1000, 1500);
		timeout--;
		st = cor_read(tx, DDC_STATUS);
		if (st & (DDC_STATUS_BUS_LOW | DDC_STATUS_NACK)) {
			cor_write(tx, DDC_STATUS, 0);
			ret = -ENXIO;
			break;
		}
	}

	if (!ret && len)
		ret = -ETIMEDOUT;
	if (ret) {
		meson_hdmitx21_ddc_cmd(tx, DDC_CMD_ABORT);
		meson_hdmitx21_ddc_cmd(tx, DDC_CMD_CLK_RESET);
	}
	cor_update(tx, TPI_DDC_MASTER_EN, TPI_DDC_HW_EN, 0);

	return ret;
}

/* One-byte DDC write, used for the SCDC of the sink */
static int meson_hdmitx21_ddc_write(struct meson_hdmitx21 *tx, u8 addr,
				    u8 offset, u8 val)
{
	u8 st;
	int ret;

	mutex_lock(&tx->ddc_lock);
	ret = meson_hdmitx21_ddc_wait_idle(tx);
	if (ret)
		goto out;

	cor_update(tx, HDCP2X_CTL_0, BIT(0), 0);
	cor_write(tx, LM_DDC, 0x80);
	cor_update(tx, TPI_DDC_MASTER_EN, TPI_DDC_HW_EN, TPI_DDC_HW_EN);
	cor_write(tx, DDC_ADDR_REG, addr << 1);
	cor_write(tx, DDC_OFFSET, offset);
	meson_hdmitx21_ddc_cmd(tx, DDC_CMD_CLR_FIFO);
	cor_write(tx, DDC_DATA, val);
	cor_write(tx, DDC_DIN_CNT1, 1);
	cor_write(tx, DDC_DIN_CNT2, 0);
	meson_hdmitx21_ddc_cmd(tx, DDC_CMD_SEQ_WRITE);

	ret = read_poll_timeout(cor_read, st, !(st & DDC_STATUS_BUSY),
				1000, 20000, false, tx, DDC_STATUS);
	if (!ret && (st & (DDC_STATUS_NACK | DDC_STATUS_BUS_LOW))) {
		cor_write(tx, DDC_STATUS, 0);
		ret = -ENXIO;
	}
	cor_update(tx, TPI_DDC_MASTER_EN, TPI_DDC_HW_EN, 0);
out:
	mutex_unlock(&tx->ddc_lock);

	return ret;
}

static u8 meson_hdmitx21_read_packet(struct meson_hdmitx21 *tx, u8 bank, u8 *packet)
{
	u8 saved, enabled;
	unsigned int i;

	/* Readers must not change the bank used by audio or an atomic commit. */
	mutex_lock(&tx->packet_lock);
	saved = cor_read(tx, TPI_INFO_FSEL);
	cor_write(tx, TPI_INFO_FSEL, bank);
	enabled = cor_read(tx, TPI_INFO_EN);
	for (i = 0; i < TPI_INFO_EN - TPI_INFO_B0; i++)
		packet[i] = cor_read(tx, TPI_INFO_B0 + i);
	cor_write(tx, TPI_INFO_FSEL, saved);
	mutex_unlock(&tx->packet_lock);
	return enabled;
}

static ssize_t link_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct meson_hdmitx21 *tx = dev_get_drvdata(dev);
	static const u8 banks[] = {
		TPI_INFO_FSEL_AVI, TPI_INFO_FSEL_AUDIO, TPI_INFO_FSEL_DRM,
	};
	u8 packet[TPI_INFO_EN - TPI_INFO_B0], scdc_config = 0, scdc_status = 0;
	int config_ret = -ENOLINK, status_ret = -ENOLINK;
	unsigned int i;
	ssize_t count;

	count = sysfs_emit(buf, "top=%08x active=%08x phy0=%08x phy1=%08x phy3=%08x\n",
			   top_read(tx, TOP_STAT0), top_read(tx, TOP_HV_ACTIVE),
			   phy_read(tx, PHY_CTRL0), phy_read(tx, PHY_CTRL1),
			   phy_read(tx, PHY_CTRL3));
	count += sysfs_emit_at(buf, count,
			       "tpi=%02x p2t=%02x scrctl=%02x clkratio=%02x gcp=%02x csc=%02x/%02x\n",
			       cor_read(tx, TPI_SC), cor_read(tx, P2T_CTRL), cor_read(tx, SCRCTL),
			       cor_read(tx, CLKRATIO), cor_read(tx, GCP_CNTL),
			       cor_read(tx, CSC0_CTRL), cor_read(tx, CSC1_CTRL));
	for (i = 0; i < ARRAY_SIZE(banks); i++) {
		u8 enabled = meson_hdmitx21_read_packet(tx, banks[i], packet);

		count += sysfs_emit_at(buf, count, "packet%u enable=%02x data=%*ph\n",
				       banks[i], enabled, (int)sizeof(packet), packet);
	}
	/* Read sink status through the same bounded, serialized DDC master.
	 * No SCDC configuration is written by this diagnostic interface.
	 */
	mutex_lock(&tx->ddc_lock);
	if (gpiod_get_value_cansleep(tx->hpd) > 0) {
		config_ret = meson_hdmitx21_ddc_read(tx, DDC_SCDC_ADDR, 0,
						     SCDC_TMDS_CONFIG, &scdc_config, 1);
		status_ret = meson_hdmitx21_ddc_read(tx, DDC_SCDC_ADDR, 0,
						     0x40, &scdc_status, 1);
	}
	mutex_unlock(&tx->ddc_lock);
	return count + sysfs_emit_at(buf, count, "scdc_config=%02x ret=%d status=%02x ret=%d\n",
				    scdc_config, config_ret, scdc_status, status_ret);
}
static DEVICE_ATTR_RO(link_state);

static struct attribute *meson_hdmitx21_link_attrs[] = {
	&dev_attr_link_state.attr,
	NULL,
};

static const struct attribute_group meson_hdmitx21_link_group = {
	.attrs = meson_hdmitx21_link_attrs,
};

/*
 * The DDC master only does EDID style reads: optional segment write to
 * 0x30, offset write to 0x50, then a read from 0x50.
 */
static int meson_hdmitx21_i2c_xfer(struct i2c_adapter *adap,
				   struct i2c_msg *msgs, int num)
{
	struct meson_hdmitx21 *tx = i2c_get_adapdata(adap);
	u8 segment = 0, offset = 0;
	int i, ret = 0;

	/* Validate the complete EDID transaction before issuing a DDC command. */
	if (num != 2 && num != 3)
		return -EOPNOTSUPP;
	for (i = 0; i < num; i++)
		if (!msgs[i].len || !msgs[i].buf ||
		    msgs[i].flags & ~(I2C_M_RD | I2C_M_DMA_SAFE))
			return -EOPNOTSUPP;
	if (num == 3 && (msgs[0].addr != DDC_SEGMENT_ADDR ||
			msgs[0].flags & I2C_M_RD || msgs[0].len != 1))
		return -EOPNOTSUPP;
	if (msgs[num - 2].addr != DDC_EDID_ADDR ||
	    msgs[num - 2].flags & I2C_M_RD || msgs[num - 2].len != 1 ||
	    msgs[num - 1].addr != DDC_EDID_ADDR || !(msgs[num - 1].flags & I2C_M_RD) ||
	    msgs[num - 1].len > 256 - msgs[num - 2].buf[0])
		return -EOPNOTSUPP;

	mutex_lock(&tx->ddc_lock);
	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];

		if (msg->addr == DDC_SEGMENT_ADDR && !(msg->flags & I2C_M_RD) &&
		    msg->len == 1) {
			segment = msg->buf[0];
		} else if (msg->addr == DDC_EDID_ADDR && !(msg->flags & I2C_M_RD) &&
			   msg->len == 1) {
			offset = msg->buf[0];
		} else if (msg->addr == DDC_EDID_ADDR && (msg->flags & I2C_M_RD)) {
			ret = meson_hdmitx21_ddc_read(tx, DDC_EDID_ADDR, segment, offset,
						      msg->buf, msg->len);
			if (ret)
				break;
			offset += msg->len;
		} else {
			ret = -EOPNOTSUPP;
			break;
		}
	}
	mutex_unlock(&tx->ddc_lock);

	return ret ? ret : num;
}

static u32 meson_hdmitx21_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm meson_hdmitx21_i2c_algo = {
	.master_xfer = meson_hdmitx21_i2c_xfer,
	.functionality = meson_hdmitx21_i2c_func,
};

static int meson_hdmitx21_add_ddc(struct meson_hdmitx21 *tx)
{
	mutex_init(&tx->ddc_lock);
	tx->ddc.owner = THIS_MODULE;
	tx->ddc.algo = &meson_hdmitx21_i2c_algo;
	tx->ddc.dev.parent = tx->dev;
	strscpy(tx->ddc.name, "meson-hdmitx21-ddc", sizeof(tx->ddc.name));
	i2c_set_adapdata(&tx->ddc, tx);
	return devm_i2c_add_adapter(tx->dev, &tx->ddc);
}

/* Controller */

static void meson_hdmitx21_phy_bandgap(struct meson_hdmitx21 *tx)
{
	if (!phy_read(tx, PHY_CTRL0))
		phy_write(tx, PHY_CTRL0, PHY_CTRL0_BANDGAP);
}

static void meson_hdmitx21_hw_init(struct meson_hdmitx21 *tx)
{
	top_write(tx, TOP_SW_RESET, 0);
	cor_write(tx, INTR3_MASK, 0xff);
	top_write(tx, TOP_SEC_SCRATCH, 1);

	top_write(tx, TOP_INTR_STAT_CLR, 0x1ff);
	/* audio mclk select, i2s, tmds and pixel clocks on */
	top_write(tx, TOP_CLK_CNTL, 0x0000200f);
	/* HPD uses the GPIO input; no TOP interrupt consumer is installed. */
	top_write(tx, TOP_INTR_MASKN, 0);
	/* DDC internal clock divider */
	top_write(tx, TOP_INFILTER, 0x01000000);
	cor_update(tx, AON_CYP_CTL, 0x3, 0x2);
	cor_update(tx, PCLK2TMDS_MISC0, 0x3, 0);
	cor_update(tx, HBLANK_REKEY_CONTROL, BIT(6), BIT(6));

	top_pulse(tx, TOP_SW_RESET_I2C);
}

static void meson_hdmitx21_phy_on(struct meson_hdmitx21 *tx, int tmds_khz)
{
	u32 val;
	int i;

	phy_write(tx, PHY_CTRL0, 0);
	val = phy_read(tx, PHY_CTRL1);
	val = (val & 0x0000ffff) | (0x0390 << 16);
	phy_write(tx, PHY_CTRL1, val);
	phy_write(tx, PHY_CTRL1, val | BIT(17));
	phy_write(tx, PHY_CTRL1, val & ~BIT(17));
	val = phy_read(tx, PHY_CTRL1) & ~0xf;
	phy_write(tx, PHY_CTRL1, val);
	msleep(20);
	for (i = 0; i < 3; i++) {
		phy_write(tx, PHY_CTRL1, val | 0xf);
		mdelay(2);
		phy_write(tx, PHY_CTRL1, val | 0xe);
		mdelay(2);
	}
	/* PHY values by TMDS clock: up to 150 MHz, 300 MHz, 600 MHz */
	if (tmds_khz > 300000) {
		phy_write(tx, PHY_CTRL5, 0x0000080b);
		phy_write(tx, PHY_CTRL0, 0x37eb65c4);
	} else {
		phy_write(tx, PHY_CTRL5, 0x00000003);
		phy_write(tx, PHY_CTRL0, tmds_khz > 150000 ? 0x33eb42a2 : 0x33eb4252);
	}
	phy_write(tx, PHY_CTRL3, 0x2ab0ff3b);
}

static void meson_hdmitx21_phy_off(struct meson_hdmitx21 *tx)
{
	phy_write(tx, PHY_CTRL0, PHY_CTRL0_BANDGAP);
	phy_write(tx, PHY_CTRL3, 0x3);
	phy_write(tx, PHY_CTRL5, 0x800);
}

static void meson_hdmitx21_write_infoframe(struct meson_hdmitx21 *tx, u8 sel,
					   const u8 *buf, ssize_t len)
{
	int i;

	mutex_lock(&tx->packet_lock);
	cor_write(tx, TPI_INFO_FSEL, sel);
	if (!buf) {
		cor_write(tx, TPI_INFO_EN, 0);
		mutex_unlock(&tx->packet_lock);
		return;
	}
	for (i = 0; i < 31; i++)
		cor_write(tx, TPI_INFO_B0 + i, i < len ? buf[i] : 0);
	cor_write(tx, TPI_INFO_EN, 0xe0);
	mutex_unlock(&tx->packet_lock);
}

static void meson_hdmitx21_write_avi(struct meson_hdmitx21 *tx,
				     struct drm_connector *connector,
				     const struct drm_display_mode *mode)
{
	struct hdmi_avi_infoframe frame;
	u8 buf[HDMI_INFOFRAME_SIZE(AVI)];
	ssize_t len;

	if (drm_hdmi_avi_infoframe_from_display_mode(&frame, connector, mode))
		return;
	/* The inherited OSD matrix converts RGB to BT.709 limited, even at 720p.
	 * Keep this description in step with the CSC bypass in config(). Do not
	 * advertise Broadcast RGB: no RGB transport/range selection is implemented.
	 */
	frame.colorspace = HDMI_COLORSPACE_YUV444;
	frame.colorimetry = HDMI_COLORIMETRY_ITU_709;
	frame.quantization_range = HDMI_QUANTIZATION_RANGE_DEFAULT;
	frame.ycc_quantization_range = HDMI_YCC_QUANTIZATION_RANGE_LIMITED;

	len = hdmi_avi_infoframe_pack(&frame, buf, sizeof(buf));
	if (len < 0)
		return;

	meson_hdmitx21_write_infoframe(tx, TPI_INFO_FSEL_AVI, buf, len);
}

static void meson_hdmitx21_config(struct meson_hdmitx21 *tx,
				  const struct drm_display_mode *mode,
				  bool hdmi_mode, bool scrambling)
{
	/* 8 bit, no deep colour */
	cor_update(tx, GCP_CNTL, BIT(0), 0);
	top_write(tx, TOP_I2C_BUSY_CNT_MAX, 30);
	cor_pulse(tx, PWD_SRST, BIT(0));
	cor_pulse(tx, HDCP_CTRL, BIT(2));
	cor_pulse(tx, AON_CYP_CTL, BIT(3));
	cor_pulse(tx, HDCP2X_TX_SRST, BIT(5));
	cor_update(tx, PCLK2TMDS_MISC1, BIT(4), 0);

	top_write(tx, TOP_SW_RESET, 0);
	/* TMDS clock pattern: 1/10, or 1/40 above 340 MHz */
	if (scrambling) {
		top_write(tx, TOP_TMDS_CLK_PTTN_01, 0x00000000);
		top_write(tx, TOP_TMDS_CLK_PTTN_23, 0x03ff03ff);
	} else {
		top_write(tx, TOP_TMDS_CLK_PTTN_01, 0x001f001f);
		top_write(tx, TOP_TMDS_CLK_PTTN_23, 0x001f001f);
	}
	top_write(tx, TOP_TMDS_CLK_PTTN_CNTL, 1);
	/* normal TMDS data to the PHY */
	top_write(tx, TOP_BIST_CNTL, 0x00001000);
	if (scrambling)
		top_write(tx, TOP_TMDS_CLK_PTTN_CNTL, 2);
	cor_write(tx, SCRCTL, 0x20 | scrambling);
	top_write(tx, TOP_HPD_FILTER, 0x80007000);
	cor_write(tx, P2T_CTRL, 0x00);
	cor_update(tx, FRL_CTRL, 0xf, 0);
	cor_write(tx, H21TXSB_CTRL, 0);
	cor_write(tx, CLK_DIV_CNTRL, 1);
	cor_write(tx, CLKPWD, 0xf4);
	cor_write(tx, SOC_FUNC_SEL, 0x01);
	cor_write(tx, TEST_TXCTRL, 0x02);
	cor_write(tx, CLKRATIO, 0x8a);

	/* Vendor OSD1 RGB -> BT.709 limited is retained in VPP. Forward YUV444
	 * without a second conversion; AVI and the bridge bus format match it.
	 */
	cor_write(tx, CSC0_CTRL, 0x00);
	cor_write(tx, CSC0_CTRL2, 0x08);
	cor_write(tx, CSC1_CTRL, 0x00);
	cor_write(tx, CSC1_CTRL2, 0x08);

	cor_write(tx, VP_INPUT_MASK, 0x2a);
	cor_write(tx, VP_OUTPUT_MAPPING, 0x40);
	cor_write(tx, VP_OUTPUT_MAPPING_HI, 0x04);
	cor_write(tx, VP_OUTPUT_MASK, 0x00);

	cor_write(tx, VTEM_CTRL, 0x04);
	top_write(tx, TOP_HS_INTR_CNTL, 0x10);
	top_write(tx, TOP_SEC_VIDEO_OVR, 0x3bccd2ac);
	top_write(tx, TOP_SEC_VIDEO_OVR_0, 0);
	top_write(tx, TOP_SEC_VIDEO_OVR_1, 0);
	top_write(tx, TOP_HV_ACTIVE, (mode->vdisplay << 16) | mode->hdisplay);

	/* FIFO resets */
	cor_update(tx, PWD_SRST, 0x6, 0x6);
	cor_update(tx, PWD_SRST, 0x6, 0);

	cor_update(tx, TPI_SC, TPI_SC_HDMI_MODE, hdmi_mode ? TPI_SC_HDMI_MODE : 0);
}

static int meson_hdmitx21_write_avi_infoframe(struct drm_bridge *bridge,
					      const u8 *buffer, size_t len)
{
	meson_hdmitx21_write_infoframe(bridge_to_hdmitx(bridge), TPI_INFO_FSEL_AVI, buffer, len);
	return 0;
}

static int meson_hdmitx21_clear_avi_infoframe(struct drm_bridge *bridge)
{
	return meson_hdmitx21_write_avi_infoframe(bridge, NULL, 0);
}

static int meson_hdmitx21_write_hdmi_infoframe(struct drm_bridge *bridge,
					       const u8 *buffer, size_t len)
{
	meson_hdmitx21_write_infoframe(bridge_to_hdmitx(bridge), TPI_INFO_FSEL_VENDOR, buffer, len);
	return 0;
}

static int meson_hdmitx21_clear_hdmi_infoframe(struct drm_bridge *bridge)
{
	return meson_hdmitx21_write_hdmi_infoframe(bridge, NULL, 0);
}

static int meson_hdmitx21_write_hdr_infoframe(struct drm_bridge *bridge,
					      const u8 *buffer, size_t len)
{
	meson_hdmitx21_write_infoframe(bridge_to_hdmitx(bridge), TPI_INFO_FSEL_DRM, buffer, len);
	return 0;
}

static int meson_hdmitx21_clear_hdr_infoframe(struct drm_bridge *bridge)
{
	return meson_hdmitx21_write_hdr_infoframe(bridge, NULL, 0);
}

static int meson_hdmitx21_write_audio_infoframe(struct drm_bridge *bridge,
						const u8 *buffer, size_t len)
{
	meson_hdmitx21_write_infoframe(bridge_to_hdmitx(bridge), TPI_INFO_FSEL_AUDIO, buffer, len);
	return 0;
}

static int meson_hdmitx21_clear_audio_infoframe(struct drm_bridge *bridge)
{
	return meson_hdmitx21_write_audio_infoframe(bridge, NULL, 0);
}

static void meson_hdmitx21_config_color(struct meson_hdmitx21 *tx,
					const struct drm_connector_state *conn)
{
	bool deep = conn->hdmi.output_bpc == 10;

	cor_update(tx, GCP_CNTL, BIT(0), deep ? BIT(0) : 0);
	cor_write(tx, P2T_CTRL, deep ? BIT(7) | 1 : 0);
	cor_write(tx, VP_INPUT_MASK, deep ? 0x15 : 0x2a);
	/* VPP POST2 already produces the negotiated domain and range. */
	cor_write(tx, CSC0_CTRL, 0);
	cor_write(tx, CSC1_CTRL, 0);
}

static void meson_hdmitx21_write_vendor(struct meson_hdmitx21 *tx,
					struct drm_connector *connector,
					const struct drm_display_mode *mode)
{
	struct hdmi_vendor_infoframe frame;
	u8 buf[10];
	ssize_t len;

	if (drm_hdmi_vendor_infoframe_from_display_mode(&frame, connector, mode))
		return;
	len = hdmi_vendor_infoframe_pack(&frame, buf, sizeof(buf));
	if (len > 0)
		meson_hdmitx21_write_infoframe(tx, TPI_INFO_FSEL_VENDOR, buf, len);
}

/* Audio: two channel LPCM from I2S (TDM through tohdmitx) */

static void meson_hdmitx21_audio_mute(struct meson_hdmitx21 *tx, bool mute)
{
	cor_write(tx, AUD_EN, mute ? 0x00 : 0x03);
	cor_update(tx, AUDP_TXCTRL, AUDP_TXCTRL_MUTE, mute ? AUDP_TXCTRL_MUTE : 0);
	cor_update(tx, TPI_AUD_CONFIG, TPI_AUD_CONFIG_MUTE,
		   mute ? TPI_AUD_CONFIG_MUTE : 0);
}

static void meson_hdmitx21_audio_config(struct meson_hdmitx21 *tx)
{
	struct hdmi_codec_params *p = &tx->audio_params;
	u8 buf[HDMI_INFOFRAME_SIZE(AUDIO)];
	unsigned int n, cts;
	ssize_t len;
	int i;

	if (!tx->tmds_hz || !tx->sink_hdmi)
		return;

	cor_update(tx, AIP_RST, BIT(0), BIT(0));
	cor_update(tx, SPDIF_SSMPL2, BIT(5), 0);
	/* audio master clock from I2S, not from S/PDIF */
	top_write(tx, TOP_CLK_CNTL, top_read(tx, TOP_CLK_CNTL) &
		  ~TOP_CLK_CNTL_AUD_MCLK_SPDIF);
	cor_write(tx, AIP_HDMI2MHL, 0x00);
	cor_write(tx, PKT_FILTER_0, 0x02);
	cor_write(tx, ASRC, 0x00);
	cor_write(tx, VP_INPUT_SYNC_ADJUST, 0x01);
	/* layout 0 (two channels), muted */
	cor_write(tx, AUDP_TXCTRL, AUDP_TXCTRL_MUTE);

	/* ACR: N from the table, CTS measured by the hardware */
	drm_hdmi_acr_get_n_cts(tx->tmds_hz,
			       p->sample_rate, &n, &cts);
	cor_update(tx, ACR_CTS_CLK_DIV, BIT(4), 0);
	cor_write(tx, N_SVAL1, n & 0xff);
	cor_write(tx, N_SVAL2, (n >> 8) & 0xff);
	cor_write(tx, N_SVAL3, (n >> 16) & 0xff);
	cor_write(tx, ACR_CTRL, ACR_CTRL_EN);

	cor_write(tx, FREQ_SVAL, 0x00);
	/* 24 bit samples */
	cor_write(tx, TPI_AUD_CONFIG, 0xc0 | TPI_AUD_CONFIG_MUTE);
	cor_write(tx, I2S_IN_MAP, 0xe4);
	cor_write(tx, I2S_IN_CTRL, 0x20);
	cor_write(tx, I2S_IN_SIZE, 0x0b);
	for (i = 0; i < 5; i++)
		cor_write(tx, I2S_CHST0 + i, p->iec.status[i]);
	cor_write(tx, SPDIF_ERTH, 0x00);
	/* I2S, SD0 only */
	cor_write(tx, AUD_MODE, 0x10);
	cor_write(tx, AUD_EN, 0x03);

	len = hdmi_audio_infoframe_pack(&p->cea, buf, sizeof(buf));
	if (len > 0)
		meson_hdmitx21_write_infoframe(tx, TPI_INFO_FSEL_AUDIO, buf, len);

	cor_update(tx, AIP_RST, BIT(0), 0);
	meson_hdmitx21_audio_mute(tx, tx->audio_muted);

	dev_dbg(tx->dev, "audio %d Hz, %d bit, N %u, TMDS %llu Hz\n",
		p->sample_rate, p->sample_width, n, tx->tmds_hz);
}

static void meson_hdmitx21_audio_off(struct meson_hdmitx21 *tx)
{
	meson_hdmitx21_audio_mute(tx, true);
	cor_update(tx, ACR_CTRL, ACR_CTRL_EN, 0);
	mutex_lock(&tx->packet_lock);
	cor_write(tx, TPI_INFO_FSEL, TPI_INFO_FSEL_AUDIO);
	cor_write(tx, TPI_INFO_EN, 0);
	mutex_unlock(&tx->packet_lock);
}

static void meson_hdmitx21_audio_disconnect(struct meson_hdmitx21 *tx)
{
	if (!(tx->bridge.ops & DRM_BRIDGE_OP_HDMI_AUDIO))
		return;
	mutex_lock(&tx->audio_lock);
	tx->tmds_hz = 0;
	tx->sink_hdmi = false;
	meson_hdmitx21_audio_off(tx);
	mutex_unlock(&tx->audio_lock);
}

static void meson_hdmitx21_audio_mode(struct meson_hdmitx21 *tx, unsigned long long tmds_hz,
				      bool hdmi)
{
	if (!(tx->bridge.ops & DRM_BRIDGE_OP_HDMI_AUDIO))
		return;
	mutex_lock(&tx->audio_lock);
	tx->tmds_hz = tmds_hz;
	tx->sink_hdmi = hdmi;
	if (tx->audio_enabled && hdmi)
		meson_hdmitx21_audio_config(tx);
	else
		meson_hdmitx21_audio_off(tx);
	mutex_unlock(&tx->audio_lock);
}

static int meson_hdmitx21_audio_prepare(struct drm_bridge *bridge,
					struct drm_connector *connector,
					struct hdmi_codec_daifmt *fmt,
					struct hdmi_codec_params *hparms)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);

	if (fmt->fmt != HDMI_I2S || hparms->channels != 2)
		return -EINVAL;
	if (hparms->sample_width != 16 && hparms->sample_width != 20 &&
	    hparms->sample_width != 24)
		return -EINVAL;
	switch (hparms->sample_rate) {
	case 32000:
	case 44100:
	case 48000:
	case 88200:
	case 96000:
	case 176400:
	case 192000:
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&tx->audio_lock);
	if (!tx->tmds_hz || !tx->sink_hdmi) {
		mutex_unlock(&tx->audio_lock);
		return -ENODEV;
	}
	tx->audio_params = *hparms;
	tx->audio_enabled = true;
	meson_hdmitx21_audio_config(tx);
	mutex_unlock(&tx->audio_lock);

	return 0;
}

static void meson_hdmitx21_audio_shutdown(struct drm_bridge *bridge,
					  struct drm_connector *connector)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);

	mutex_lock(&tx->audio_lock);
	tx->audio_enabled = false;
	meson_hdmitx21_audio_off(tx);
	mutex_unlock(&tx->audio_lock);
}

static int meson_hdmitx21_audio_mute_stream(struct drm_bridge *bridge,
					    struct drm_connector *connector,
					    bool enable, int direction)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);

	mutex_lock(&tx->audio_lock);
	tx->audio_muted = enable;
	if (tx->audio_enabled && tx->tmds_hz)
		meson_hdmitx21_audio_mute(tx, enable);
	mutex_unlock(&tx->audio_lock);

	return 0;
}

/* Bridge */

static bool meson_hdmitx21_yuv_sink(const struct drm_display_info *info)
{
	return info->is_hdmi && (info->color_formats & BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444));
}

static int meson_hdmitx21_bridge_attach(struct drm_bridge *bridge,
					struct drm_encoder *encoder,
					enum drm_bridge_attach_flags flags)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);
	struct meson_drm *priv = bridge->dev->dev_private;

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR))
		return -EINVAL;
	if (tx->enc2_modeset)
		return 0;

	/* The HDMI PHY registers are in the analog control block of the VPU */
	tx->hhi = priv->hhi;
	meson_hdmitx21_phy_bandgap(tx);

	if (!tx->next_bridge)
		return 0;

	return drm_bridge_attach(encoder, tx->next_bridge, bridge, flags);
}

static enum drm_mode_status
meson_hdmitx21_bridge_mode_valid(struct drm_bridge *bridge,
				 const struct drm_display_info *info,
				 const struct drm_display_mode *mode)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);
	enum drm_mode_status status = MODE_OK;
	const char *reason;

	if (tx->enc2_modeset && !tx->atomic_color && !meson_hdmitx21_yuv_sink(info)) {
		drm_dbg_kms(bridge->dev,
			    "Reject sink without HDMI YUV444: inherited VPP is BT.709 limited\n");
		return MODE_BAD;
	}

	if (IS_ENABLED(CONFIG_DRM_MESON_T7_HANDOFF) && tx->enc2_modeset)
		status = meson_t7_mode_valid(mode);
	if (status != MODE_OK) {
		if (status == MODE_NOCLOCK)
			reason = "pixel clock outside 25.175..594MHz or VCO outside 3..6GHz";
		else if (status == MODE_VSYNC)
			reason = "refresh below 23Hz exceeds the buffer-retirement deadline";
		else if (status == MODE_NO_INTERLACE || status == MODE_NO_DBLESCAN)
			reason = "only progressive single-scan timing is implemented";
		else
			reason = "geometry/porches exceed ENC2 linear path limits";
		drm_dbg_kms(bridge->dev, "ENC2 rejects " DRM_MODE_FMT ": %s (status=%d)\n",
			    DRM_MODE_ARG(mode), reason, status);
		return status;
	}
	/* The HDMI state helper checks EDID format/depth and actual TMDS rate.
	 * A 594MHz pixel clock can be transported as 297MHz YUV420.
	 */
	if (tx->atomic_color) {
		unsigned int rate = mode->clock;

		if (drm_mode_is_420(info, mode))
			rate /= 2;
		if (rate > 340000 && (!info->hdmi.scdc.supported ||
				      !info->hdmi.scdc.scrambling.supported))
			return MODE_CLOCK_HIGH;
		return MODE_OK;
	}
	/* Up to 600 MHz; above 340 MHz the sink must support scrambling */
	if (mode->clock > 600000)
		return MODE_CLOCK_HIGH;
	if (mode->clock > 340000 &&
	    (!info->hdmi.scdc.supported || !info->hdmi.scdc.scrambling.supported)) {
		drm_dbg_kms(bridge->dev, "Reject >340MHz TMDS without sink SCDC/scrambling\n");
		return MODE_CLOCK_HIGH;
	}
	if (info->max_tmds_clock && mode->clock > info->max_tmds_clock) {
		drm_dbg_kms(bridge->dev, "Reject TMDS above sink limit %d kHz\n",
			    info->max_tmds_clock);
		return MODE_CLOCK_HIGH;
	}
	if (drm_mode_is_420_only(info, mode)) {
		drm_dbg_kms(bridge->dev, "Reject 4:2:0-only timing on the RGB888 path\n");
		return MODE_BAD;
	}

	return MODE_OK;
}

static enum drm_connector_status
meson_hdmitx21_bridge_detect(struct drm_bridge *bridge,
			     struct drm_connector *connector)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);

	int level = gpiod_get_value_cansleep(tx->hpd);

	if (level < 0)
		return connector_status_unknown;
	return level ? connector_status_connected : connector_status_disconnected;
}

static const struct drm_edid *
meson_hdmitx21_bridge_edid_read(struct drm_bridge *bridge,
				struct drm_connector *connector)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);
	const struct drm_edid *drm_edid;

	if (!tx->boot_monitor)
		meson_hdmitx21_phy_bandgap(tx);
	drm_edid = drm_edid_read_ddc(connector, &tx->ddc);
	if (!drm_edid && !tx->boot_monitor) {
		/* retry once after a DDC glitch filter reset */
		top_pulse(tx, TOP_SW_RESET_GLITCH);
		drm_edid = drm_edid_read_ddc(connector, &tx->ddc);
	}
	if (!drm_edid)
		dev_dbg(tx->dev, "failed to read EDID\n");

	return drm_edid;
}

static u32 *
meson_hdmitx21_bridge_get_input_bus_fmts(struct drm_bridge *bridge,
					 struct drm_bridge_state *bridge_state,
					 struct drm_crtc_state *crtc_state,
					 struct drm_connector_state *conn_state,
					 u32 output_fmt,
					 unsigned int *num_input_fmts)
{
	u32 *fmts;

	*num_input_fmts = 0;
	fmts = kmalloc_obj(*fmts);
	if (!fmts)
		return NULL;

	/* Framebuffers are RGB; the retained OSD matrix precedes this bus. */
	fmts[0] = bridge_to_hdmitx(bridge)->atomic_color && conn_state->hdmi.output_bpc == 10 ?
		MEDIA_BUS_FMT_YUV10_1X30 : MEDIA_BUS_FMT_YUV8_1X24;
	*num_input_fmts = 1;

	return fmts;
}

static enum drm_mode_status
meson_hdmitx21_tmds_valid(const struct drm_bridge *bridge,
			  const struct drm_display_mode *mode, unsigned long long rate)
{
	return rate > 600000000 ? MODE_CLOCK_HIGH : MODE_OK;
}

static int meson_hdmitx21_bridge_atomic_check(struct drm_bridge *bridge,
					      struct drm_bridge_state *bridge_state,
					      struct drm_crtc_state *crtc_state,
					      struct drm_connector_state *conn)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);
	const struct drm_display_info *info = &conn->connector->display_info;
	struct drm_connector_state *old;
	struct drm_plane_state *plane_state;
	struct drm_plane *plane;
	struct meson_t7_link_clock link;
	struct hdmi_avi_infoframe *avi = &conn->hdmi.infoframes.avi.data.avi;
	bool bt2020 = conn->colorspace == DRM_MODE_COLORIMETRY_BT2020_YCC;
	bool rgb = conn->hdmi.output_format == DRM_OUTPUT_COLOR_FORMAT_RGB444;
	int i, ret;
	bool yuv420 = conn->hdmi.output_format == DRM_OUTPUT_COLOR_FORMAT_YCBCR420;

	if (!IS_ENABLED(CONFIG_DRM_MESON_T7_HANDOFF) || !tx->atomic_color)
		return 0;
	if (conn->colorspace != DRM_MODE_COLORIMETRY_DEFAULT &&
	    conn->colorspace != DRM_MODE_COLORIMETRY_BT709_YCC && !bt2020)
		return -EINVAL;
	if (conn->hdmi.output_format == DRM_OUTPUT_COLOR_FORMAT_YCBCR420 &&
	    ((crtc_state->adjusted_mode.hdisplay | crtc_state->adjusted_mode.htotal) & 1))
		return -EINVAL;
	ret = meson_t7_link_clock_calculate(crtc_state->adjusted_mode.clock,
					    conn->hdmi.output_bpc, yuv420, &link);
	if (ret)
		return ret;
	if (conn->hdmi.tmds_char_rate > 340000000 &&
	    (!info->hdmi.scdc.supported || !info->hdmi.scdc.scrambling.supported))
		return -EINVAL;
	if (rgb && bt2020)
		return -EINVAL;
	/* Linear NV12/NV21 video currently has only a BT.709 input contract.
	 * Reject mixed gamuts rather than tagging those pixels as BT.2020.
	 */
	ret = drm_atomic_add_affected_planes(crtc_state->state, crtc_state->crtc);
	if (ret)
		return ret;
	for_each_new_plane_in_state(crtc_state->state, plane, plane_state, i) {
		if (bt2020 && plane_state->crtc == crtc_state->crtc && plane_state->fb &&
		    plane_state->fb->format->is_yuv)
			return -EINVAL;
	}
	if (conn->hdr_output_metadata) {
		const struct hdr_output_metadata *hdr = conn->hdr_output_metadata->data;
		unsigned int eotf;

		if (conn->hdr_output_metadata->length != sizeof(*hdr) || !hdr)
			return -EINVAL;
		eotf = hdr->hdmi_metadata_type1.eotf;
		if (!info->is_hdmi || !bt2020 || conn->hdmi.output_bpc != 10 ||
		    hdr->metadata_type || hdr->hdmi_metadata_type1.metadata_type ||
		    eotf > HDMI_EOTF_BT_2100_HLG ||
		    !(info->hdr_sink_metadata.hdmi_type1.eotf & BIT(eotf)))
			return -EINVAL;
	}
	/* T7 uses per-OSD HDR2 in the vendor path, not the optional 0x3910
	 * matrix used by other SoCs. Its inherited routing and LUT ownership
	 * are not yet acquired here. Never tag unconverted pixels as BT.2020.
	 */
	if (bt2020) {
		drm_dbg_kms(bridge->dev, "BT.2020 needs exclusive T7 OSD HDR2 ownership\n");
		return -EOPNOTSUPP;
	}
	avi->quantization_range = rgb ? (conn->hdmi.is_limited_range ?
		HDMI_QUANTIZATION_RANGE_LIMITED : HDMI_QUANTIZATION_RANGE_FULL) :
		HDMI_QUANTIZATION_RANGE_DEFAULT;
	avi->ycc_quantization_range = HDMI_YCC_QUANTIZATION_RANGE_LIMITED;
	if (!bt2020)
		avi->colorimetry = HDMI_COLORIMETRY_ITU_709;
	old = drm_atomic_get_old_connector_state(crtc_state->state, conn->connector);
	if (old->colorspace != conn->colorspace ||
	    !drm_connector_atomic_hdr_metadata_equal(old, conn))
		crtc_state->mode_changed = true;
	return 0;
}

static void meson_hdmitx21_bridge_atomic_enable(struct drm_bridge *bridge,
						struct drm_atomic_commit *state)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);
	const struct drm_crtc_state *crtc_state;
	struct drm_connector *connector;
	const struct drm_display_mode *mode;
	struct drm_connector_state *conn;
	unsigned long long tmds_rate;
	bool hdmi_mode, scrambling;

	connector = drm_atomic_get_new_connector_for_encoder(state, bridge->encoder);
	if (WARN_ON(!connector))
		return;
	conn = drm_atomic_get_new_connector_state(state, connector);
	crtc_state = drm_atomic_get_new_crtc_state(state, conn->crtc);
	if (WARN_ON(!crtc_state))
		return;

	mode = &crtc_state->adjusted_mode;
	hdmi_mode = connector->display_info.is_hdmi;
	if (IS_ENABLED(CONFIG_DRM_MESON_T7_HANDOFF) && tx->enc2_modeset &&
	    !meson_t7_display_ready(crtc_state->crtc))
		return;

	tmds_rate = tx->atomic_color ? conn->hdmi.tmds_char_rate : mode->clock * 1000ULL;
	scrambling = tmds_rate > 340000000;
	if (connector->display_info.hdmi.scdc.supported &&
	    meson_hdmitx21_ddc_write(tx, DDC_SCDC_ADDR, SCDC_TMDS_CONFIG,
				     scrambling ? 0x03 : 0x00)) {
		dev_err(tx->dev, "SCDC write failed; TX remains muted\n");
		meson_hdmitx21_audio_disconnect(tx);
		meson_hdmitx21_set_avmute(tx, true);
		meson_hdmitx21_phy_off(tx);
		return;
	}

	meson_hdmitx21_set_avmute(tx, true);
	if (tx->enc2_modeset)
		meson_hdmitx21_hw_init(tx);
	meson_hdmitx21_config(tx, mode, hdmi_mode, scrambling);
	if (tx->atomic_color) {
		meson_hdmitx21_config_color(tx, conn);
		if (drm_atomic_helper_connector_hdmi_update_infoframes(connector, state)) {
			dev_err(tx->dev, "InfoFrame update failed; TX remains muted\n");
			return;
		}
	} else if (hdmi_mode) {
		meson_hdmitx21_write_avi(tx, connector, mode);
		meson_hdmitx21_write_vendor(tx, connector, mode);
	}
	meson_hdmitx21_phy_on(tx, DIV_ROUND_CLOSEST_ULL(tmds_rate, 1000));
	cor_update(tx, TMDS_CTRL, BIT(3), BIT(3));
	meson_hdmitx21_set_avmute(tx, false);

	meson_hdmitx21_audio_mode(tx, tmds_rate,
				  hdmi_mode && connector->display_info.has_audio);

	dev_dbg(tx->dev, "enabled %dx%d@%d %s\n", mode->hdisplay,
		mode->vdisplay, drm_mode_vrefresh(mode), hdmi_mode ? "HDMI" : "DVI");
}

static void meson_hdmitx21_bridge_atomic_disable(struct drm_bridge *bridge,
						 struct drm_atomic_commit *state)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);

	meson_hdmitx21_audio_disconnect(tx);
	meson_hdmitx21_set_avmute(tx, true);
	meson_hdmitx21_phy_off(tx);
	if (tx->atomic_color)
		meson_hdmitx21_clear_hdr_infoframe(bridge);
	mutex_lock(&tx->packet_lock);
	cor_write(tx, TPI_INFO_FSEL, 0);
	cor_write(tx, TPI_INFO_EN, 0);
	cor_write(tx, TPI_INFO_FSEL, TPI_INFO_FSEL_VENDOR);
	cor_write(tx, TPI_INFO_EN, 0);
	mutex_unlock(&tx->packet_lock);
}

static void meson_hdmitx21_cec_hpd_notify(struct drm_bridge *bridge,
					  struct drm_connector *connector,
					  enum drm_connector_status status)
{
	if (status == connector_status_disconnected)
		drm_connector_cec_phys_addr_invalidate(connector);
}

/* GPIOW15 is the T7 HPD level. TOP_STAT0 bit 0 is not a cable indication.
 * Polling keeps the firmware pin/IRQ routing out of the handoff experiment.
 */
static void meson_hdmitx21_hpd_work(struct work_struct *work)
{
	struct meson_hdmitx21 *tx = container_of(to_delayed_work(work),
						struct meson_hdmitx21, hpd_work);
	enum drm_connector_status status;

	status = meson_hdmitx21_bridge_detect(&tx->bridge, NULL);
	if (status != connector_status_unknown && status == tx->hpd_sample &&
	    status != tx->hpd_reported) {
		tx->hpd_reported = status;
		if (status == connector_status_disconnected)
			meson_hdmitx21_audio_disconnect(tx);
		drm_bridge_hpd_notify(&tx->bridge, status);
	}
	tx->hpd_sample = status;
	if (READ_ONCE(tx->hpd_polling))
		schedule_delayed_work(&tx->hpd_work, msecs_to_jiffies(100));
}

static void meson_hdmitx21_monitor_hpd_enable(struct drm_bridge *bridge)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);

	tx->hpd_sample = connector_status_unknown;
	tx->hpd_reported = connector_status_unknown;
	WRITE_ONCE(tx->hpd_polling, true);
	schedule_delayed_work(&tx->hpd_work, 0);
}

static void meson_hdmitx21_monitor_hpd_disable(struct drm_bridge *bridge)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);

	WRITE_ONCE(tx->hpd_polling, false);
	cancel_delayed_work_sync(&tx->hpd_work);
}

static void meson_hdmitx21_monitor_cleanup(void *data)
{
	struct meson_hdmitx21 *tx = data;

	meson_hdmitx21_monitor_hpd_disable(&tx->bridge);
}

static const struct drm_bridge_funcs meson_hdmitx21_bridge_funcs = {
	.attach = meson_hdmitx21_bridge_attach,
	.mode_valid = meson_hdmitx21_bridge_mode_valid,
	.detect = meson_hdmitx21_bridge_detect,
	.edid_read = meson_hdmitx21_bridge_edid_read,
	.atomic_get_input_bus_fmts = meson_hdmitx21_bridge_get_input_bus_fmts,
	.atomic_check = meson_hdmitx21_bridge_atomic_check,
	.hdmi_tmds_char_rate_valid = meson_hdmitx21_tmds_valid,
	.hdmi_write_avi_infoframe = meson_hdmitx21_write_avi_infoframe,
	.hdmi_clear_avi_infoframe = meson_hdmitx21_clear_avi_infoframe,
	.hdmi_write_hdmi_infoframe = meson_hdmitx21_write_hdmi_infoframe,
	.hdmi_clear_hdmi_infoframe = meson_hdmitx21_clear_hdmi_infoframe,
	.hdmi_write_hdr_drm_infoframe = meson_hdmitx21_write_hdr_infoframe,
	.hdmi_clear_hdr_drm_infoframe = meson_hdmitx21_clear_hdr_infoframe,
	.hdmi_write_audio_infoframe = meson_hdmitx21_write_audio_infoframe,
	.hdmi_clear_audio_infoframe = meson_hdmitx21_clear_audio_infoframe,
	.atomic_enable = meson_hdmitx21_bridge_atomic_enable,
	.atomic_disable = meson_hdmitx21_bridge_atomic_disable,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_create_state = drm_atomic_helper_bridge_create_state,
	.hpd_enable = meson_hdmitx21_monitor_hpd_enable,
	.hpd_disable = meson_hdmitx21_monitor_hpd_disable,
	.hpd_notify = meson_hdmitx21_cec_hpd_notify,
	.hdmi_audio_prepare = meson_hdmitx21_audio_prepare,
	.hdmi_audio_shutdown = meson_hdmitx21_audio_shutdown,
	.hdmi_audio_mute_stream = meson_hdmitx21_audio_mute_stream,
};

/*
 * The handoff bridge owns no modesetting state. In particular it must never
 * enter the normal attach/enable callbacks, which change PHY and TX registers.
 * Use the vendor TOP/COR SMC read ABI, including its byte-width argument.
 */
static int meson_hdmitx21_boot_attach(struct drm_bridge *bridge,
				      struct drm_encoder *encoder,
				      enum drm_bridge_attach_flags flags)
{
	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR))
		return -EINVAL;
	return 0;
}

static enum drm_mode_status
meson_hdmitx21_boot_mode_valid(struct drm_bridge *bridge,
			       const struct drm_display_info *info,
			       const struct drm_display_mode *mode)
{
	enum drm_mode_status status = meson_t7_handoff_mode_valid(mode);

	if (status != MODE_OK)
		drm_dbg_kms(bridge->dev, "Reject " DRM_MODE_FMT
			    ": firmware ENC2/VPP/PLL timing is fixed (status=%d)\n",
			    DRM_MODE_ARG(mode), status);
	return status;
}

static enum drm_connector_status
meson_hdmitx21_boot_detect(struct drm_bridge *bridge, struct drm_connector *connector)
{
	/* Fixed firmware sink. Real T7 HPD is GPIOW15, not TOP_STAT0 bit 0. */
	return connector_status_connected;
}

static int meson_hdmitx21_boot_get_modes(struct drm_bridge *bridge,
					 struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(bridge->dev, &meson_t7_handoff_mode);
	if (!mode)
		return -ENOMEM;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_bridge_funcs meson_hdmitx21_boot_funcs = {
	.attach = meson_hdmitx21_boot_attach,
	.detect = meson_hdmitx21_boot_detect,
	.get_modes = meson_hdmitx21_boot_get_modes,
	.mode_valid = meson_hdmitx21_boot_mode_valid,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_create_state = drm_atomic_helper_bridge_create_state,
};

static void meson_hdmitx21_monitor_atomic_enable(struct drm_bridge *bridge,
						struct drm_atomic_commit *state)
{
	struct meson_hdmitx21 *tx = bridge_to_hdmitx(bridge);
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;

	if (!(bridge->ops & DRM_BRIDGE_OP_HDMI_AUDIO))
		return;
	connector = drm_atomic_get_new_connector_for_encoder(state, bridge->encoder);
	if (WARN_ON(!connector))
		return;
	conn_state = drm_atomic_get_new_connector_state(state, connector);
	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (WARN_ON(!crtc_state))
		return;
	/* CRTC validated the firmware timing. Never reconfigure video in this path. */
	meson_hdmitx21_audio_mode(tx, crtc_state->adjusted_mode.clock * 1000ULL,
				  connector->display_info.is_hdmi &&
				  connector->display_info.has_audio &&
				  meson_hdmitx21_bridge_detect(bridge, connector) ==
				  connector_status_connected);
}

static void meson_hdmitx21_monitor_atomic_disable(struct drm_bridge *bridge,
						 struct drm_atomic_commit *state)
{
	meson_hdmitx21_audio_disconnect(bridge_to_hdmitx(bridge));
}

/* Observe DDC/HPD without changing the firmware's video, PHY or PLL.
 * EDID can enumerate 4K, but mode_valid accepts only the boot timing.
 */
static const struct drm_bridge_funcs meson_hdmitx21_monitor_funcs = {
	.attach = meson_hdmitx21_boot_attach,
	.detect = meson_hdmitx21_bridge_detect,
	.edid_read = meson_hdmitx21_bridge_edid_read,
	.mode_valid = meson_hdmitx21_boot_mode_valid,
	.hpd_enable = meson_hdmitx21_monitor_hpd_enable,
	.hpd_disable = meson_hdmitx21_monitor_hpd_disable,
	.atomic_enable = meson_hdmitx21_monitor_atomic_enable,
	.atomic_disable = meson_hdmitx21_monitor_atomic_disable,
	.hdmi_audio_prepare = meson_hdmitx21_audio_prepare,
	.hdmi_audio_shutdown = meson_hdmitx21_audio_shutdown,
	.hdmi_audio_mute_stream = meson_hdmitx21_audio_mute_stream,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_create_state = drm_atomic_helper_bridge_create_state,
};

static int meson_hdmitx21_boot_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct meson_hdmitx21 *tx;
	struct resource *top, *cor;
	bool monitor = of_property_present(dev->of_node, "amlogic,boot-monitor");
	bool enc2 = of_property_present(dev->of_node, "amlogic,enc2-modeset");
	u32 status, active, tpi;
	int ret;

	if (enc2 && !IS_ENABLED(CONFIG_DRM_MESON_T7_HANDOFF))
		return -EOPNOTSUPP;
	top = platform_get_resource_byname(pdev, IORESOURCE_MEM, "top");
	cor = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cor");
	if (!top || !cor || top->start != 0xfe300000 || cor->start != 0xfe380000 ||
	    resource_size(top) != 0x10000 || resource_size(cor) != 0x10000)
		return -EINVAL;
	if (!devm_request_mem_region(dev, top->start, resource_size(top), dev_name(dev)) ||
	    !devm_request_mem_region(dev, cor->start, resource_size(cor), dev_name(dev)))
		return -EBUSY;
	tx = devm_drm_bridge_alloc(dev, struct meson_hdmitx21, bridge,
				   enc2 ? &meson_hdmitx21_bridge_funcs :
				   monitor ? &meson_hdmitx21_monitor_funcs :
				   &meson_hdmitx21_boot_funcs);
	if (IS_ERR(tx))
		return PTR_ERR(tx);
	tx->dev = dev;
	tx->enc2_modeset = enc2;
	platform_set_drvdata(pdev, tx);
	tx->atomic_color = of_property_read_bool(dev->of_node, "amlogic,atomic-color");
	if (tx->atomic_color && !enc2)
		return -EINVAL;
	monitor |= enc2;
	if (enc2) {
		struct resource *phy = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");

		if (!phy || phy->start != 0xfe008200 || resource_size(phy) != 0x18)
			return -EINVAL;
		tx->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
		if (IS_ERR(tx->phy))
			return PTR_ERR(tx->phy);
	}
	mutex_init(&tx->audio_lock);
	mutex_init(&tx->packet_lock);
	tx->audio_muted = true;
	tx->boot_top = top->start;
	tx->boot_cor = cor->start;
	tx->pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(tx->pclk))
		return PTR_ERR(tx->pclk);
	status = top_read(tx, TOP_STAT0);
	active = top_read(tx, TOP_HV_ACTIVE);
	tpi = cor_read(tx, TPI_SC);
	dev_info(dev, "boot TX TOP_STAT0=%#x active=%#x TPI_SC=%#x (%s)\n",
		 status, active, tpi, enc2 ? "ENC2 atomic modeset" : "firmware timing");
	if (active != (1080 << 16 | 1920))
		return dev_err_probe(dev, -ENODEV, "TX boot active area is not 1080p\n");
	tx->bridge.of_node = dev->of_node;
	tx->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_MODES;
	tx->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	if (monitor) {
		tx->boot_monitor = !enc2;
		tx->hpd = devm_gpiod_get(dev, "hpd", GPIOD_IN);
		if (IS_ERR(tx->hpd))
			return dev_err_probe(dev, PTR_ERR(tx->hpd), "No HPD GPIO\n");
		INIT_DELAYED_WORK(&tx->hpd_work, meson_hdmitx21_hpd_work);
		ret = meson_hdmitx21_add_ddc(tx);
		if (ret)
			return ret;
		tx->bridge.ddc = &tx->ddc;
		tx->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID | DRM_BRIDGE_OP_HPD;
		if (enc2 && IS_ENABLED(CONFIG_CEC_NOTIFIER)) {
			tx->bridge.ops |= DRM_BRIDGE_OP_HDMI_CEC_NOTIFIER;
			tx->bridge.hdmi_cec_dev = dev;
		}
		if (tx->atomic_color) {
			tx->bridge.ops |= DRM_BRIDGE_OP_HDMI | DRM_BRIDGE_OP_HDMI_HDR_DRM_INFOFRAME;
			tx->bridge.supported_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444) |
				BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444) |
				BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420);
			tx->bridge.max_bpc = 10;
			tx->bridge.ycbcr_420_allowed = true;
			tx->bridge.vendor = "Amlogic";
			tx->bridge.product = "T7 HDMI";
		}
		if (of_property_present(dev->of_node, "#sound-dai-cells")) {
			tx->bridge.ops |= DRM_BRIDGE_OP_HDMI_AUDIO;
			tx->bridge.hdmi_audio_dev = dev;
			tx->bridge.hdmi_audio_max_i2s_playback_channels = 2;
			tx->bridge.hdmi_audio_i2s_formats = SNDRV_PCM_FMTBIT_S16_LE |
							 SNDRV_PCM_FMTBIT_S24_LE;
			tx->bridge.hdmi_audio_dai_port = -1;
		}
	}
	ret = devm_drm_bridge_add(dev, &tx->bridge);
	if (ret || !monitor)
		return ret;
	if (enc2) {
		ret = devm_device_add_group(dev, &meson_hdmitx21_link_group);
		if (ret)
			return ret;
	}
	/* Stop work before the managed bridge is removed. */
	return devm_add_action_or_reset(dev, meson_hdmitx21_monitor_cleanup, tx);
}

static int meson_hdmitx21_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct meson_hdmitx21 *tx;
	struct reset_control *rst;
	struct resource *top, *cor;
	int ret;

	if (of_property_present(dev->of_node, "amlogic,boot-handoff") ||
	    of_property_present(dev->of_node, "amlogic,enc2-modeset"))
		return meson_hdmitx21_boot_probe(pdev);

	tx = devm_drm_bridge_alloc(dev, struct meson_hdmitx21, bridge,
				   &meson_hdmitx21_bridge_funcs);
	if (IS_ERR(tx))
		return PTR_ERR(tx);
	tx->dev = dev;
	platform_set_drvdata(pdev, tx);
	mutex_init(&tx->audio_lock);
	mutex_init(&tx->packet_lock);
	tx->audio_muted = true;
	tx->hpd = devm_gpiod_get(dev, "hpd", GPIOD_IN);
	if (IS_ERR(tx->hpd))
		return dev_err_probe(dev, PTR_ERR(tx->hpd), "No HPD GPIO\n");
	INIT_DELAYED_WORK(&tx->hpd_work, meson_hdmitx21_hpd_work);

	top = platform_get_resource_byname(pdev, IORESOURCE_MEM, "top");
	cor = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cor");
	if (!top || !cor || top->start != 0xfe300000 || cor->start != 0xfe380000 ||
	    resource_size(top) != 0x10000 || resource_size(cor) != 0x10000)
		return -EINVAL;
	if (!devm_request_mem_region(dev, top->start, resource_size(top), dev_name(dev)) ||
	    !devm_request_mem_region(dev, cor->start, resource_size(cor), dev_name(dev)))
		return -EBUSY;
	tx->boot_top = top->start;
	tx->boot_cor = cor->start;

	tx->pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(tx->pclk))
		return dev_err_probe(dev, PTR_ERR(tx->pclk), "failed to get pclk\n");

	rst = devm_reset_control_array_get_optional_exclusive(dev);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "failed to get resets\n");
	reset_control_deassert(rst);

	meson_hdmitx21_hw_init(tx);

	tx->next_bridge = devm_drm_of_get_bridge(dev, dev->of_node, 1, 0);
	if (IS_ERR(tx->next_bridge)) {
		if (PTR_ERR(tx->next_bridge) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		tx->next_bridge = NULL;
	}

	ret = meson_hdmitx21_add_ddc(tx);
	if (ret)
		return ret;

	tx->bridge.of_node = dev->of_node;
	tx->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID |
			 DRM_BRIDGE_OP_HPD;
	tx->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	tx->bridge.ddc = &tx->ddc;
	tx->bridge.interlace_allowed = false;

	if (of_property_present(dev->of_node, "#sound-dai-cells")) {
		tx->bridge.ops |= DRM_BRIDGE_OP_HDMI_AUDIO;
		tx->bridge.hdmi_audio_dev = dev;
		tx->bridge.hdmi_audio_max_i2s_playback_channels = 2;
		tx->bridge.hdmi_audio_dai_port = -1;
	}

	ret = devm_drm_bridge_add(dev, &tx->bridge);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, meson_hdmitx21_monitor_cleanup, tx);
}

static const struct of_device_id meson_hdmitx21_of_match[] = {
	{ .compatible = "amlogic,t7-hdmi-tx" },
	{ }
};
MODULE_DEVICE_TABLE(of, meson_hdmitx21_of_match);

static struct platform_driver meson_hdmitx21_driver = {
	.probe = meson_hdmitx21_probe,
	.driver = {
		.name = "meson-hdmitx21",
		.of_match_table = meson_hdmitx21_of_match,
	},
};
module_platform_driver(meson_hdmitx21_driver);

MODULE_DESCRIPTION("Amlogic T7 HDMI 2.1 transmitter bridge");
MODULE_LICENSE("GPL");
