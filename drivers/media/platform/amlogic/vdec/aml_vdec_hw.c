// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#include <linux/arm-smccc.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <media/meson-t7-dos.h>
#include <dt-bindings/power/amlogic,t7-pwrc.h>

#include "aml_vdec_drv.h"
#include "aml_vdec_hw.h"
#include "aml_vdec_platform.h"

#define MHz     (1000000)
#define MC_SIZE (4096 * 16)

static struct pm_pd_s vdec_domain_data[] = {
	[VDEC_1] = {.name = "vdec", },
	[VDEC_HEVC] = {.name = "hevc", },
};

u32 read_dos_reg(struct aml_vdec_hw *hw, u32 addr)
{
	u32 ret_val;

	regmap_read(hw->map[DOS_BUS], addr, &ret_val);

	return ret_val;
}

static void dos_reg_write_bits(struct aml_vdec_hw *hw, u32 reg, u32 val, int start, int len)
{
	u32 mask = (((1L << (len)) - 1) << (start));

	regmap_update_bits(hw->map[DOS_BUS], reg, mask, val);
}

void dos_enable(struct aml_vdec_hw *hw)
{
	dos_reg_write_bits(hw, DOS_GCLK_EN0, 0x3ff, 0, 10);

	regmap_write(hw->map[DOS_BUS], GCLK_EN, 0x3ff);

	regmap_update_bits(hw->map[DOS_BUS], MDEC_PIC_DC_CTRL, (1 << 31), 0);
}

int aml_vdec_reset_core(struct aml_vdec_hw *hw)
{
	unsigned int mask = BIT(21);
	unsigned int value;
	unsigned int i;
	int ret;

	if (hw->has_dbus_ctrl) {
		ret = regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_DBUS_DISABLE, 0xffff);
		if (ret)
			return ret;
		ret = regmap_read_poll_timeout(hw->map[DOS_BUS], VDEC_ASSIST_DBUS_DISABLE,
					       value, value == 0xffff, 1, 100000);
		if (ret)
			return ret;
		/* Retain the vendor DBUS settling loop after the acknowledgment. */
		for (i = 0; i < 200; i++)
			cpu_relax();
	} else {
		regmap_update_bits(hw->map[DMC_BUS], 0x0, mask, 0);
		usleep_range(60, 70);
	}
	ret = regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0,
			   (1 << 3) | (1 << 4) | (1 << 5) | (1 << 7) |
			   (1 << 8) | (1 << 9));
	if (ret)
		return ret;
	ret = regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(hw->map[DOS_BUS], VDEC_ASSIST_MMC_CTRL1, 1 << 3, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(hw->map[DOS_BUS], MDEC_PIC_DC_MUX_CTRL, 1 << 31, 0);
	if (ret)
		return ret;
	ret = regmap_write(hw->map[DOS_BUS], MDEC_EXTIF_CFG1, 0);
	if (ret)
		return ret;

	if (hw->has_dbus_ctrl)
		return regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_DBUS_DISABLE, 0);

	return regmap_update_bits(hw->map[DMC_BUS], 0x0, mask, mask);
}

void aml_start_vdec_hw(struct aml_vdec_hw *hw)
{
	u32 reg_read_val;

	regmap_read(hw->map[DOS_BUS], DOS_SW_RESET0, &reg_read_val);
	regmap_read(hw->map[DOS_BUS], DOS_SW_RESET0, &reg_read_val);
	regmap_read(hw->map[DOS_BUS], DOS_SW_RESET0, &reg_read_val);

	regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, (1 << 12) | (1 << 11));
	regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, 0);

	regmap_read(hw->map[DOS_BUS], DOS_SW_RESET0, &reg_read_val);
	regmap_read(hw->map[DOS_BUS], DOS_SW_RESET0, &reg_read_val);
	regmap_read(hw->map[DOS_BUS], DOS_SW_RESET0, &reg_read_val);

	regmap_write(hw->map[DOS_BUS], MPSR, 0x0001);
}

int aml_stop_vdec_hw(struct aml_vdec_hw *hw)
{
	u32 reg_val = 0;
	unsigned int i;
	int ret;

	ret = regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_MASK, 0);
	if (ret)
		return ret;
	synchronize_irq(hw->dec_irq);
	ret = regmap_write(hw->map[DOS_BUS], MPSR, 0);
	if (ret)
		return ret;
	ret = regmap_write(hw->map[DOS_BUS], CPSR, 0);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(hw->map[DOS_BUS], IMEM_DMA_CTRL,
				       reg_val, !(reg_val & BIT(15)), 10, 100000);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(hw->map[DOS_BUS], LMEM_DMA_CTRL,
				       reg_val, !(reg_val & BIT(15)), 10, 100000);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(hw->map[DOS_BUS], WRRSP_LMEM,
				       reg_val, !(reg_val & 0xfff), 10, 12500);
	if (ret)
		return ret;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(hw->map[DOS_BUS], DOS_SW_RESET0, &reg_val);
		if (ret)
			return ret;
	}

	ret = regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, BIT(12) | BIT(11));
	if (ret)
		return ret;
	ret = regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, 0);
	if (ret)
		return ret;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(hw->map[DOS_BUS], DOS_SW_RESET0, &reg_val);
		if (ret)
			return ret;
	}

	/*
	 * T7 has no DMC request bit. Keep DBUS isolated after stopping the
	 * CPUs; only the next reset may re-enable it. Failure to acknowledge
	 * isolation is not permission to return vb2 or codec DMA memory.
	 */
	if (hw->has_dbus_ctrl) {
		ret = regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_DBUS_DISABLE, 0xffff);
		if (ret)
			return ret;
		ret = regmap_read_poll_timeout(hw->map[DOS_BUS], VDEC_ASSIST_DBUS_DISABLE,
					       reg_val, reg_val == 0xffff, 1, 100000);
		if (ret)
			return ret;
		for (i = 0; i < 200; i++)
			cpu_relax();
	}
	return 0;
}

int load_firmware(struct aml_vdec_hw *hw, const char *path)
{
	const struct firmware *fw;
	struct device *dev = hw->dev;
	static u8 *mc_addr;
	static dma_addr_t mc_addr_map;
	int fw_head_len;
	ulong timeout;
	int ret;

	if (hw->has_dbus_ctrl) {
		struct arm_smccc_res result;

		/* Vendor tee_load_video_fw(H264_MULTI=15, VDEC_LEGENCY=0). */
		arm_smccc_smc(0xb200000f, 15, 0, 0, 0, 0, 0, 0, &result);
		if ((s32)result.a0) {
			dev_err(dev, "secure H264 load failed: %#lx\n", result.a0);
			return -EIO;
		}
		ret = regmap_read(hw->map[DOS_BUS], AV_SCRATCH_G, &hw->secure_swap);
		if (ret)
			return ret;
		dev_dbg(dev, "secure H264 loaded, scratch G=%#x\n", hw->secure_swap);
		return 0;
	}

	ret = request_firmware(&fw, path, dev);
	if (ret < 0) {
		dev_info(dev, "request_firmware %s failed ret %d\n", path, ret);
		return -EINVAL;
	}

	if (fw->size > MC_SIZE) {
		dev_info(dev, "fw %s oversize\n", path);
		ret = -EINVAL;
		goto release_firmware;
	}

	fw_head_len = 512;
	mc_addr = dma_alloc_coherent(dev, MC_SIZE, &mc_addr_map, GFP_KERNEL);
	if (!mc_addr) {
		dev_info(dev, "no mem for fw %s\n", path);
		ret = -ENOMEM;
		goto release_firmware;
	}
	memset(mc_addr, 0, MC_SIZE);
	memcpy(mc_addr, ((u8 *)fw->data + fw_head_len),
	       (fw->size - fw_head_len));

	regmap_write(hw->map[DOS_BUS], MPSR, 0);
	regmap_write(hw->map[DOS_BUS], CPSR, 0);

	timeout = read_dos_reg(hw, MPSR);
	timeout = read_dos_reg(hw, MPSR);

	timeout = jiffies + HZ;

	regmap_write(hw->map[DOS_BUS], IMEM_DMA_ADR, mc_addr_map);
	regmap_write(hw->map[DOS_BUS], IMEM_DMA_COUNT, 0x1000);
	regmap_write(hw->map[DOS_BUS], IMEM_DMA_CTRL, (0x8000 | (7 << 16)));

	while (read_dos_reg(hw, IMEM_DMA_CTRL) & 0x8000) {
		if (time_before(jiffies, timeout)) {
			schedule();
		} else {
			dev_info(dev, "vdec load mc error\n");
			ret = -EBUSY;
			break;
		}
	}

	/* Only h264 needs this step */
	if (hw->hw_ops.load_firmware_ex) {
		ret = hw->hw_ops.load_firmware_ex(hw->curr_ctx,
						  mc_addr,
						  (fw->size - fw_head_len));
		if (ret < 0) {
			ret = -EINVAL;
			goto free_dma_mem;
		}
	}

free_dma_mem:
	dma_free_coherent(dev, MC_SIZE, mc_addr, mc_addr_map);
release_firmware:
	release_firmware(fw);
	return ret;
}

static void t7_clock_provider_put(void *data)
{
	put_device(data);
}

static int t7_clock_provider_init(struct aml_vdec_hw *hw)
{
	struct platform_device *provider;
	struct device_node *node;
	int ret;

	node = of_parse_phandle(hw->dev->of_node, "clocks", 0);
	if (!node)
		return -EINVAL;
	if (!of_device_is_compatible(node, "amlogic,t7-peripherals-clkc")) {
		of_node_put(node);
		return -EINVAL;
	}
	provider = of_find_device_by_node(node);
	of_node_put(node);
	if (!provider)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(hw->dev, t7_clock_provider_put, &provider->dev);
	if (ret)
		return ret;
	/* Reuse the provider mapping; never claim or write its shared registers. */
	hw->clkctrl = dev_get_regmap(&provider->dev, NULL);
	return hw->clkctrl ? 0 : -EPROBE_DEFER;
}

static int vdec_clock_gate_init(struct aml_vdec_hw *hw)
{
	int ret;

	hw->gates[VDEC].id = "vdec";
	hw->gates[VDEC_MUX].id = "clk_vdec_mux";
	hw->gates[HEVCF_MUX].id = "clk_hevcf_mux";

	ret = devm_clk_bulk_get(hw->dev, hw->has_dbus_ctrl ? 2 : DOS_CLK_MAX, hw->gates);
	if (ret || !hw->has_dbus_ctrl)
		return ret;
	hw->apb_gates[0].id = "sys-apb";
	hw->apb_gates[1].id = "a73-apb";
	ret = devm_clk_bulk_get(hw->dev, ARRAY_SIZE(hw->apb_gates), hw->apb_gates);
	if (ret)
		return ret;
	return t7_clock_provider_init(hw);
}

static struct clk_bulk_data *vdec_get_clk_by_name(struct aml_vdec_hw *hw,
						  const char *name)
{
	int i;

	for (i = 0; i < DOS_CLK_MAX; i++) {
		if (!strcmp(name, hw->gates[i].id)) {
			if (hw->gates[i].clk)
				return &hw->gates[i];
		}
	}
	return NULL;
}

static int pm_vdec_power_domain_init(struct aml_vdec_hw *hw)
{
	int i, err;
	const struct power_manager_s *pm = hw->pm;
	struct pm_pd_s *pd = pm->pd_data;

	mutex_init(&hw->pm_mutex);

	for (i = 0; i < (hw->has_dbus_ctrl ? 1 : VDEC_MAX); i++) {
		pd[i].dev = dev_pm_domain_attach_by_name(hw->dev, pd[i].name);
		if (IS_ERR_OR_NULL(pd[i].dev)) {
			err = pd[i].dev ? PTR_ERR(pd[i].dev) : -ENODEV;
			dev_dbg(hw->dev, "Get %s failed, pm-domain: %d\n",
				pd[i].name, err);
			if (hw->has_dbus_ctrl)
				return err;
			continue;
		}

		pd[i].link = device_link_add(hw->dev, pd[i].dev,
					     (hw->has_dbus_ctrl ? 0 : DL_FLAG_PM_RUNTIME) |
					     DL_FLAG_STATELESS);
		if (IS_ERR_OR_NULL(pd[i].link)) {
			dev_err(hw->dev, "Adding %s device link failed!\n", pd[i].name);
			return -ENODEV;
		}

		dev_dbg(hw->dev, "power domain: name: %s, dev: %p, link: %p\n",
			pd[i].name, pd[i].dev, pd[i].link);
	}
	if (hw->has_dbus_ctrl) {
		/* No automatic power-on: platform driver owns the transition. */
		hw->nic3_dev = dev_pm_domain_attach_by_name(hw->dev, "nic3");
		if (IS_ERR_OR_NULL(hw->nic3_dev))
			return hw->nic3_dev ? PTR_ERR(hw->nic3_dev) : -ENODEV;
	}

	return 0;
}

static void pm_vdec_power_domain_release(struct aml_vdec_hw *hw)
{
	int i;
	const struct power_manager_s *pm = hw->pm;
	struct pm_pd_s *pd = pm->pd_data;

	if (!IS_ERR_OR_NULL(hw->nic3_dev))
		dev_pm_domain_detach(hw->nic3_dev, true);

	for (i = 0; i < (hw->has_dbus_ctrl ? 1 : VDEC_MAX); i++) {
		if (!IS_ERR_OR_NULL(pd[i].link))
			device_link_del(pd[i].link);

		if (!IS_ERR_OR_NULL(pd[i].dev))
			dev_pm_domain_detach(pd[i].dev, true);
	}
}

static void dos_local_config(struct aml_vdec_hw *hw, bool is_on, int id)
{
	/* T7 delegates memory power, isolation and local reset to secure genpd. */
	if (hw->has_dbus_ctrl)
		return;

	if (is_on) {
		usleep_range(20, 100);

		switch (id) {
		case VDEC_1:
			regmap_write(hw->map[DOS_BUS], DOS_MEM_PD_VDEC, 0);
			regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, 0xfffffffc);
			usleep_range(20, 100);
			regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, 0);
			usleep_range(20, 100);
			regmap_write(hw->map[DOS_BUS], DOS_MEM_PD_VDEC, 0);
			break;
		case VDEC_HEVC:
			regmap_write(hw->map[DOS_BUS], DOS_MEM_PD_HEVC, 0);
			regmap_write(hw->map[DOS_BUS], DOS_SW_RESET3, 0xffffffff);
			usleep_range(20, 100);
			regmap_write(hw->map[DOS_BUS], DOS_SW_RESET3, 0);
			usleep_range(20, 100);
			regmap_write(hw->map[DOS_BUS], DOS_MEM_PD_HEVC, 0);
			break;
		default:
			dev_info(hw->dev, "%s on, not found id %d\n", __func__, id);
			break;
		}
	} else {
		switch (id) {
		case VDEC_1:
			regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, 0xfffffffc);
			usleep_range(20, 100);
			regmap_write(hw->map[DOS_BUS], DOS_SW_RESET0, 0);
			usleep_range(20, 100);
			regmap_write(hw->map[DOS_BUS], DOS_MEM_PD_VDEC, 0xffffffffUL);
			break;
		case VDEC_HEVC:
			regmap_write(hw->map[DOS_BUS], DOS_SW_RESET3, 0xffffffff);
			usleep_range(20, 100);
			regmap_write(hw->map[DOS_BUS], DOS_SW_RESET3, 0);
			usleep_range(20, 100);
			regmap_write(hw->map[DOS_BUS], DOS_MEM_PD_HEVC, 0xffffffffUL);
			break;
		default:
			dev_info(hw->dev, "%s off, not found id %d\n", __func__, id);
			break;
		}
	}

	dev_dbg(hw->dev, "%s end, id %d, is_on %d\n", __func__, id, is_on);
}

static int pm_vdec_power_domain_power_on(struct aml_vdec_hw *hw, int id)
{
	const struct power_manager_s *pm = hw->pm;
	struct device *dev = pm->pd_data[id].dev;
	struct clk_bulk_data *gate_node = NULL;
	int ret;

	if (IS_ERR_OR_NULL(dev))
		return -ENODEV;

	if (id == VDEC_1)
		gate_node = vdec_get_clk_by_name(hw, "clk_vdec_mux");
	else if (id == VDEC_HEVC)
		gate_node = vdec_get_clk_by_name(hw, "clk_hevcf_mux");

	if (gate_node) {
		ret = clk_prepare_enable(gate_node->clk);
		if (ret)
			return ret;
		if (id == VDEC_1) {
			ret = clk_set_rate(gate_node->clk, 499999992);
			if (ret) {
				clk_disable_unprepare(gate_node->clk);
				return ret;
			}
			dev_dbg(hw->dev, "after set, vdec mux clock is %lu Hz\n",
				clk_get_rate(gate_node->clk));
		}
		dev_dbg(hw->dev, "the %-15s clock on\n", gate_node->id);
	} else {
		return -ENODEV;
	}

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		clk_disable_unprepare(gate_node->clk);
		return ret;
	}

	dos_local_config(hw, 1, id);
	return 0;
}

static int pm_vdec_power_domain_power_off(struct aml_vdec_hw *hw, int id)
{
	const struct power_manager_s *pm = hw->pm;
	struct device *dev = pm->pd_data[id].dev;
	struct clk_bulk_data *gate_node = NULL;

	dos_local_config(hw, 0, id);

	if (!IS_ERR_OR_NULL(dev) && !hw->has_dbus_ctrl) {
		pm_runtime_put_sync(dev);
		dev_dbg(dev, "dev: %p link %p the %-15s power off\n",
			dev, pm->pd_data[id].link, pm->pd_data[id].name);
	}

	if (id == VDEC_1)
		gate_node = vdec_get_clk_by_name(hw, "clk_vdec_mux");
	else if (id == VDEC_HEVC)
		gate_node = vdec_get_clk_by_name(hw, "clk_hevcf_mux");

	if (gate_node) {
		clk_disable_unprepare(gate_node->clk);
		dev_dbg(hw->dev, "the %-15s clock off\n", gate_node->id);
	} else {
		dev_info(hw->dev, "clk %d, unreachable\n", id);
	}

	/* T7's secure power switch follows functional clock shutdown. */
	if (!IS_ERR_OR_NULL(dev) && hw->has_dbus_ctrl)
		return pm_runtime_put_sync(dev);

	return 0;
}

static bool pm_vdec_power_domain_power_state(struct aml_vdec_hw *hw, int id)
{
	if (hw->pm->pd_data[id].dev)
		return pm_runtime_active(hw->pm->pd_data[id].dev);
	else
		return false;
}

static int vdec_poweron(struct aml_vdec_hw *hw, enum vdec_type_e core)
{
	int ret = 0;

	if (core >= VDEC_MAX)
		return -EINVAL;

	mutex_lock(&hw->pm_mutex);
	if (IS_ERR_OR_NULL(hw->pm->pd_data[core].dev)) {
		ret = -ENODEV;
		goto out;
	}

	hw->pm->pd_data[core].ref_count++;
	if (hw->pm->pd_data[core].ref_count > 1)
		goto out;

	if (!hw->has_dbus_ctrl && hw->pm->power_state(hw, core))
		goto out;

	ret = hw->pm->power_on(hw, core);
	if (ret)
		hw->pm->pd_data[core].ref_count--;

out:
	mutex_unlock(&hw->pm_mutex);
	return ret;
}

static int vdec_poweroff(struct aml_vdec_hw *hw, enum vdec_type_e core)
{
	int ret = 0;

	if (core >= VDEC_MAX)
		return -EINVAL;

	mutex_lock(&hw->pm_mutex);
	if (hw->pm->pd_data[core].ref_count == 0)
		goto out;

	hw->pm->pd_data[core].ref_count--;
	if (hw->pm->pd_data[core].ref_count > 0)
		goto out;

	ret = hw->pm->power_off(hw, core);

out:
	mutex_unlock(&hw->pm_mutex);
	return ret;
}

/* Vendor sec_pwr_ctrl.c: read-only status, 0 = on, 1 = off. */
static int t7_domain_state(unsigned int domain, s32 *state)
{
	struct arm_smccc_res result;

	arm_smccc_smc(0x82000095, domain, 0, 0, 0, 0, 0, 0, &result);
	*state = (s32)result.a0;
	return (*state == 0 || *state == 1) ? 0 : -EIO;
}

int aml_vdec_t7_power_state(struct aml_vdec_hw *hw, s32 *vdec, s32 *nic3)
{
	int ret;

	if (!hw->has_dbus_ctrl)
		return -EOPNOTSUPP;
	ret = t7_domain_state(PWRC_T7_DOS_VDEC_ID, vdec);
	if (ret)
		return ret;
	return t7_domain_state(PWRC_T7_NIC3_ID, nic3);
}

static void t7_power_fault(struct aml_vdec_hw *hw)
{
	/* No DOS access: domain or clock state is no longer trustworthy. */
	if (xchg(&hw->faulted, true))
		return;
	__module_get(THIS_MODULE);
	get_device(hw->dev);
	meson_t7_dos_poison();
	dev_crit(hw->dev, "VDEC power state uncertain; retain lease until system reset\n");
}

static int t7_clocks_on(struct aml_vdec_hw *hw)
{
	int ret;

	/* Vendor keeps these CPU APB gates on via CLK_IGNORE_UNUSED. */
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(hw->apb_gates), hw->apb_gates);
	if (ret)
		return ret;
	hw->apb_held = true;
	ret = clk_bulk_prepare_enable(2, hw->gates);
	if (ret) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(hw->apb_gates), hw->apb_gates);
		hw->apb_held = false;
		return ret;
	}
	hw->clocks_held = true;
	return 0;
}

static void t7_clocks_off(struct aml_vdec_hw *hw)
{
	if (hw->clocks_held) {
		clk_bulk_disable_unprepare(2, hw->gates);
		hw->clocks_held = false;
	}
	if (hw->apb_held) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(hw->apb_gates), hw->apb_gates);
		hw->apb_held = false;
	}
}

static int t7_power_off(struct aml_vdec_hw *hw)
{
	s32 state;
	int ret = 0, parent_ret = 0;

	/* Vendor order: clocks off, then secure domain off; no local PD reset. */
	if (hw->clocks_held) {
		clk_bulk_disable_unprepare(2, hw->gates);
		hw->clocks_held = false;
	}
	if (hw->powered)
		ret = pm_runtime_put_sync_suspend(hw->pm->pd_data[VDEC_1].dev);
	hw->powered = false;
	if (hw->nic3_held) {
		parent_ret = pm_runtime_put_sync_suspend(hw->nic3_dev);
		hw->nic3_held = false;
	}
	/* Vendor leaves the CPU bridges on even across secure power-off. */
	t7_clocks_off(hw);
	if (ret < 0 || parent_ret < 0 ||
	    t7_domain_state(PWRC_T7_DOS_VDEC_ID, &state) || state != 1) {
		t7_power_fault(hw);
		return -EIO;
	}
	return 0;
}

static int t7_clock_preflight(struct aml_vdec_hw *hw)
{
	u32 sys, apb, p0, p1, selected;
	int ret;

	/*
	 * Live T7 vendor H.264: SYS_DOS bit 1, VDEC p0=0x100, p1 low half=0.
	 * CCF's cached rate alone cannot prove the gate survived secure power-on.
	 * Read CLKCTRL (not DOS) after the transition, before any internal access.
	 * Both documented mux paths are valid; preserve all shared HCODEC bits.
	 */
	ret = regmap_read(hw->clkctrl, 0x44, &sys);
	if (ret)
		return ret;
	ret = regmap_read(hw->clkctrl, 0x4c, &apb);
	if (ret)
		return ret;
	ret = regmap_read(hw->clkctrl, 0x140, &p0);
	if (ret)
		return ret;
	ret = regmap_read(hw->clkctrl, 0x148, &p1);
	if (ret)
		return ret;
	selected = p1 & BIT(15) ? p1 : p0;
	dev_info(hw->dev, "CLKCTRL: SYS=%#x APB=%#x VDEC0=%#x VDEC1=%#x path=%u\n",
		 sys, apb, p0, p1, !!(p1 & BIT(15)));
	/* 800 MHz recipe: fclk_div2p5, divide by one, gate enabled. */
	if (!(sys & BIT(1)) || (apb & (BIT(19) | BIT(20))) != (BIT(19) | BIT(20)) ||
	    (selected & 0xf7f) != BIT(8)) {
		dev_err(hw->dev, "refusing DOS MMIO: physical clock configuration mismatch\n");
		return -EHOSTDOWN;
	}
	return 0;
}

static int t7_power_on(struct aml_vdec_hw *hw)
{
	s32 vdec = -1, nic3 = -1;
	int ret;

	if (hw->faulted)
		return -EIO;
	/* vdec_power_ctrl.c enables the functional clock before secure PM. */
	/* T7 vdec_max_clk_get(), used unconditionally by vendor clkg12.c. */
	ret = clk_set_rate(hw->gates[VDEC_MUX].clk, 800000000);
	if (ret)
		return ret;
	ret = t7_clocks_on(hw);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(hw->pm->pd_data[VDEC_1].dev);
	if (ret < 0) {
		t7_clocks_off(hw);
		return ret;
	}
	hw->powered = true;
	/*
	 * genpd's successful SET call does not verify firmware a0 or GET status.
	 * Do not touch even DOS_GCLK_EN0 until both VDEC and its NIC3 parent
	 * report on. This query itself uses SMC, never the gated DOS aperture.
	 */
	ret = aml_vdec_t7_power_state(hw, &vdec, &nic3);
	dev_info(hw->dev, "pre-DOS power: VDEC[4]=%d NIC3[39]=%d rate=%lu\n",
		 vdec, nic3, clk_get_rate(hw->gates[VDEC_MUX].clk));
	if (ret || vdec || nic3) {
		dev_err(hw->dev, "refusing DOS MMIO: secure power check failed\n");
		t7_power_off(hw);
		return ret ?: -EHOSTDOWN;
	}
	ret = t7_clock_preflight(hw);
	if (ret)
		t7_power_off(hw);
	return ret;
}

int aml_vdec_t7_power_test(struct aml_vdec_hw *hw)
{
	int ret, off_ret;

	if (!hw->has_dbus_ctrl)
		return -EOPNOTSUPP;
	if (hw->probe_failed)
		return -EIO;
	hw->power_test_passed = false;
	hw->mdec_test_passed = false;
	ret = t7_power_on(hw);
	if (ret)
		return ret;
	/*
	 * No documented VDEC silicon ID register was found. Sample the known
	 * reset register read by vendor amvdec_start(), without writing reset,
	 * enabling internal gates/IRQ, loading firmware or starting any DMA.
	 */
	ret = regmap_read(hw->map[DOS_BUS], DOS_SW_RESET0, &hw->power_test_reset);
	if (!ret)
		dev_info(hw->dev, "power test DOS_SW_RESET0 offset=%#x value=%#x\n",
			 DOS_SW_RESET0, hw->power_test_reset);
	off_ret = t7_power_off(hw);
	if (ret || off_ret)
		return ret ?: off_ret;
	hw->power_test_passed = true;
	dev_info(hw->dev, "power test complete: VDEC off; no firmware or DMA started\n");
	return 0;
}

static int t7_probe_begin(struct aml_vdec_hw *hw)
{
	s32 vdec = -1, nic3 = -1;
	int ret;

	/*
	 * Vendor vdec_set_clk() programs DOS_GCLK_EN0 BEFORE secure VDEC on.
	 * Hold NIC3, DOS clocks and CPU APB gates for that DOS-top access. Its first read is
	 * an explicit probe; never access internal MDEC registers in this phase.
	 */
	ret = t7_clocks_on(hw);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(hw->nic3_dev);
	if (ret < 0)
		return ret;
	hw->nic3_held = true;
	ret = aml_vdec_t7_power_state(hw, &vdec, &nic3);
	dev_info(hw->dev, "pre-EN0: VDEC[4]=%d NIC3[39]=%d rate=%lu\n",
		 vdec, nic3, clk_get_rate(hw->gates[VDEC_MUX].clk));
	if (ret || vdec != 1 || nic3)
		return ret ?: -EHOSTDOWN;
	return 0;
}

static int t7_probe_domain_on(struct aml_vdec_hw *hw)
{
	struct arm_smccc_res result;
	s32 vdec = -1, nic3 = -1;
	int ret;

	/* Vendor vdec_set_clk(): DOS EN0, frequency selection, then domain on. */
	ret = clk_set_rate(hw->gates[VDEC_MUX].clk, 800000000);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(hw->pm->pd_data[VDEC_1].dev);
	if (ret < 0)
		return ret;
	hw->powered = true;
	ret = aml_vdec_t7_power_state(hw, &vdec, &nic3);
	dev_info(hw->dev, "post-EN0 power: VDEC[4]=%d NIC3[39]=%d\n", vdec, nic3);
	if (ret || vdec || nic3)
		return ret ?: -EHOSTDOWN;
	/* DMC normal-memory policy, not an APB permission or register proxy. */
	arm_smccc_smc(0xb200000e, 13, 0, 0, 0, 0, 0, 0, &result);
	dev_info(hw->dev, "CONFIG_DEVICE_SECURE(13,0) a0=%#lx\n", result.a0);
	if (result.a0)
		return -EACCES;
	return t7_clock_preflight(hw);
}

/* Each register step performs exactly one MMIO operation, never an RMW. */
static int t7_register_access(struct aml_vdec_hw *hw, unsigned int step)
{
	u32 reg, value = 0;
	bool write = false;
	int ret;

	switch (step) {
	case 2:
	case 3:
	case 4:
	case 6:
		reg = DOS_GCLK_EN0;
		write = step == 3;
		value = (hw->probe_en0 & ~0x3ff) | 0x3ff;
		break;
	case 7:
		reg = DOS_SW_RESET0;
		break;
	case 8:
		reg = DOS_MEM_PD_VDEC;
		break;
	case 9:
	case 10:
	case 11:
		reg = GCLK_EN;
		write = step == 10;
		value = 0x3ff;
		break;
	case 12:
		reg = MDEC_PIC_DC_CTRL;
		break;
	default:
		return -EINVAL;
	}
	dev_info(hw->dev, "DOS step %u: %s offset=%#x begin\n",
		 step, write ? "write" : "read", reg);
	if (write)
		ret = regmap_write(hw->map[DOS_BUS], reg, value);
	else
		ret = regmap_read(hw->map[DOS_BUS], reg, &value);
	if (ret)
		return ret;
	/* Order the transaction before its completion marker and next step. */
	mb();
	dev_info(hw->dev, "DOS step %u: %s offset=%#x value=%#x complete\n",
		 step, write ? "write" : "read", reg, value);
	if (!write)
		hw->probe_value = value;
	if (step == 2)
		hw->probe_en0 = value;
	if ((step == 4 || step == 6 || step == 11) && (value & 0x3ff) != 0x3ff)
		return -EIO;
	if ((step == 7 || step == 8) && value)
		return -EHOSTDOWN;
	if (step == 12)
		hw->mdec_test_value = value;
	return 0;
}

int aml_vdec_t7_register_step(struct aml_vdec_hw *hw, unsigned int step)
{
	int ret, off_ret;

	if (!hw->has_dbus_ctrl)
		return -EOPNOTSUPP;
	if (hw->faulted || hw->probe_failed)
		return -EIO;
	if (!hw->power_test_passed)
		return -EPERM;
	/* Zero aborts a live session without another DOS transaction. */
	if (step && (step > 13 || step != hw->probe_step + 1))
		return -EINVAL;
	if (step == 1) {
		hw->mdec_test_passed = false;
		__module_get(THIS_MODULE);
		hw->probe_pinned = true;
		ret = t7_probe_begin(hw);
	} else if (step == 5) {
		ret = t7_probe_domain_on(hw);
	} else if (!step || step == 13) {
		ret = 0;
	} else {
		ret = t7_register_access(hw, step);
	}
	if (ret || !step || step == 13) {
		off_ret = t7_power_off(hw);
		hw->probe_failed = ret || off_ret;
		hw->mdec_test_passed = step == 13 && !hw->probe_failed;
		hw->probe_step = 0;
		if (hw->probe_pinned) {
			hw->probe_pinned = false;
			module_put(THIS_MODULE);
		}
		return ret ?: off_ret;
	}
	hw->probe_step = step;
	return 0;
}

int aml_vdec_t7_mdec_test(struct aml_vdec_hw *hw)
{
	unsigned int step;
	int ret;

	if (!hw->has_dbus_ctrl)
		return -EOPNOTSUPP;
	for (step = 1; step <= 13; step++) {
		ret = aml_vdec_t7_register_step(hw, step);
		if (ret)
			return ret;
	}
	dev_info(hw->dev, "MDEC test complete: VDEC off; no firmware or DMA started\n");
	return 0;
}

int vdec_enable(struct aml_vdec_hw *hw)
{
	int ret;

	if (hw->has_dbus_ctrl) {
		unsigned int step;

		ret = t7_probe_begin(hw);
		if (ret)
			goto power_off;
		for (step = 2; step <= 11; step++) {
			ret = step == 5 ? t7_probe_domain_on(hw) : t7_register_access(hw, step);
			if (ret)
				goto power_off;
		}
		ret = regmap_update_bits(hw->map[DOS_BUS], MDEC_PIC_DC_CTRL, BIT(31), 0);
		if (ret)
			goto power_off;
		regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_MASK, 0);
		regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_CLR_REG, 1);
		enable_irq(hw->dec_irq);
		return 0;
	}
	return vdec_poweron(hw, VDEC_1);

power_off:
	t7_power_off(hw);
	return ret;
}

int vdec_disable(struct aml_vdec_hw *hw)
{
	if (hw->has_dbus_ctrl) {
		if (hw->faulted)
			return -EIO;
		if (!hw->powered)
			return 0;
		regmap_write(hw->map[DOS_BUS], VDEC_ASSIST_MBOX1_MASK, 0);
		disable_irq(hw->dec_irq);
		return t7_power_off(hw);
	}
	return vdec_poweroff(hw, VDEC_1);
}

static const struct power_manager_s pm[] = {
	[AML_PM_PD] = {
		.pd_data    = vdec_domain_data,
		.init        = pm_vdec_power_domain_init,
		.release    = pm_vdec_power_domain_release,
		.power_on    = pm_vdec_power_domain_power_on,
		.power_off    = pm_vdec_power_domain_power_off,
		.power_state = pm_vdec_power_domain_power_state,
	},
};

static irqreturn_t vdec_irq_handler(int irq, void *priv)
{
	struct aml_vdec_dev *dev = (struct aml_vdec_dev *)priv;
	struct aml_vdec_hw *hw = dev->dec_hw;
	irqreturn_t ret = IRQ_HANDLED;

	if (hw->hw_ops.irq_handler)
		ret = hw->hw_ops.irq_handler(irq, priv);

	return ret;
}

static irqreturn_t vdec_threaded_isr_handler(int irq, void *priv)
{
	struct aml_vdec_dev *dev = (struct aml_vdec_dev *)priv;
	struct aml_vdec_hw *hw = dev->dec_hw;
	irqreturn_t ret = IRQ_HANDLED;

	if (hw->hw_ops.irq_threaded_func)
		ret = hw->hw_ops.irq_threaded_func(irq, priv);

	return ret;
}

struct aml_vdec_hw *vdec_get_hw(void *priv)
{
	struct aml_vdec_dev *dev = (struct aml_vdec_dev *)priv;

	return dev->dec_hw;
}

static const struct regmap_config dos_regmap_conf = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x10000,
};

static const struct regmap_config dmc_regmap_conf = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x20,
};

int dev_request_hw_resources(void *priv)
{
	struct aml_vdec_dev *dev = (struct aml_vdec_dev *)priv;
	struct aml_vdec_hw *hw;
	struct platform_device *pdev;
	void __iomem *reg_base[MAX_REG_BUS];
	struct resource res;
	int i;
	int ret = -1;

	if (!dev || !dev->dec_hw)
		return -1;

	pdev = dev->plat_dev;
	hw = dev->dec_hw;
	hw->dev = &pdev->dev;
	hw->has_dbus_ctrl = dev->pvdec_data->has_dbus_ctrl;

	hw->dec_irq = platform_get_irq(pdev, hw->has_dbus_ctrl ? 0 : VDEC_IRQ_1);
	if (hw->dec_irq < 0) {
		dev_err(&pdev->dev, "get irq failed\n");
		return hw->dec_irq;
	}
	ret = devm_request_threaded_irq(&pdev->dev, hw->dec_irq, vdec_irq_handler,
					vdec_threaded_isr_handler,
					IRQF_ONESHOT | (hw->has_dbus_ctrl ? IRQF_NO_AUTOEN : 0),
					"vdec-1", dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to install irq %d (%d)",
			hw->dec_irq, ret);
		return -1;
	}

	for (i = 0; i < (hw->has_dbus_ctrl ? 1 : MAX_REG_BUS); i++) {
		if (of_address_to_resource(pdev->dev.of_node, i, &res)) {
			dev_err(&pdev->dev, "of_address_to_resource %d failed\n", i);
			return -EINVAL;
		}
		reg_base[i] = devm_ioremap_resource(&pdev->dev, &res);

		if (IS_ERR(reg_base[i]))
			return PTR_ERR(reg_base[i]);

		if (i == DOS_BUS) {
			hw->map[i] = devm_regmap_init_mmio(&pdev->dev, reg_base[i],
							   &dos_regmap_conf);
		} else if (i == DMC_BUS) {
			hw->map[i] = devm_regmap_init_mmio(&pdev->dev, reg_base[i],
							   &dmc_regmap_conf);
		}

		if (IS_ERR(hw->map[i]))
			return PTR_ERR(hw->map[i]);

		dev_dbg(&pdev->dev, "%s, res start %llx, end %llx, iomap: %p\n",
			__func__, (unsigned long long)res.start,
			(unsigned long long)res.end, reg_base[i]);
	}
	hw->canvas = meson_canvas_get(&pdev->dev);
	if (IS_ERR(hw->canvas))
		return PTR_ERR(hw->canvas);

	hw->pm = &pm[dev->pvdec_data->power_type];
	if (hw->pm->init) {
		ret = hw->pm->init(hw);
		if (ret < 0) {
			dev_err(&pdev->dev, "power mgr init failed!\n");
			hw->pm->release(hw);
			return ret;
		}
	}

	ret = vdec_clock_gate_init(hw);
	if (ret) {
		dev_err(&pdev->dev, "clk bulk init failed!\n");
		hw->pm->release(hw);
		return ret;
	}

	dev_dbg(&pdev->dev, "##Amlogic hw resource init OK##\n");

	return 0;
}

void dev_destroy_hw_resources(void *priv)
{
	struct aml_vdec_dev *dev = (struct aml_vdec_dev *)priv;
	struct aml_vdec_hw *hw;

	if (!dev || !dev->dec_hw)
		return;

	hw = dev->dec_hw;

	if (hw->pm->release)
		hw->pm->release(hw);

	dev_dbg(hw->dev, "##Amlogic hw resource release OK##\n");
}
