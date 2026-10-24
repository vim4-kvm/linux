/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#ifndef _AML_VDEC_HW_H_
#define _AML_VDEC_HW_H_

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/soc/amlogic/meson-canvas.h>
#include "reg_defines.h"

#define VDEC_FIFO_ALIGN     8
#define VLD_PADDING_SIZE    1024

/**
 * struct aml_vdec_hw_ops - codec mode specific operations for hw
 * @load_firmware_ex: Load firmware for current dec specific.
 * @irq_handler: mandatory call when the ISR triggers
 * @irq_threaded_func: mandatory call for the threaded ISR
 * @canvas_alloc: Alloc canvas for curr frame
 * @canvas_free: Release canvas.
 * @config_canvas: Config for curr frame, such as w/h, Y/UV start addr etc.
 */
struct aml_vdec_hw_ops {
	int (*load_firmware_ex)(void *priv, const u8 *data, u32 len);
	irqreturn_t (*irq_handler)(int irq, void *priv);
	irqreturn_t (*irq_threaded_func)(int irq, void *priv);
	int (*canvas_alloc)(u8 *canvas_index);
	void (*canvas_free)(u8 canvas_index);
	void (*config_canvas)(u8 canvas_index,
			      ulong addr, u32 width, u32 height,
			      u32 wrap, u32 blkmode, u32 endian);
};

/**
 * enum vdec_type_e - Type of decoder hardware.
 */
enum vdec_type_e {
	VDEC_1 = 0,
	VDEC_HEVC,
	VDEC_MAX,
};

/**
 * enum vdec_irq_num - Definition of the irq.
 */
enum vdec_irq_num {
	VDEC_IRQ_0 = 0,
	VDEC_IRQ_1,
	VDEC_IRQ_2,
	VDEC_IRQ_MAX,
};

/**
 * enum vdec_type_e - Type of decoder clock.
 */
enum clk_type_e {
	VDEC = 0,
	VDEC_MUX,
	HEVCF_MUX,
	DOS_CLK_MAX,
};

/**
 * enum aml_power_type_e - Type of decoder power.
 */
enum aml_power_type_e {
	AML_PM_PD = 0,
};

/**
 * enum mm_bus_e - Type of decoder hardware bus.
 */
enum mm_bus_e {
	DOS_BUS = 0,
	DMC_BUS,
	MAX_REG_BUS
};

/**
 * struct pm_pd_s - power domain definition
 * @name: Power domain name.
 * @dev: Pointer to device structure.
 * @mutex: Pointer to device_link structure.
 * @ref_count: Curr power domain instance ref count.
 */
struct pm_pd_s {
	u8 *name;
	struct device *dev;
	struct device_link *link;
	int ref_count;
};

/**
 * struct aml_vdec_hw - decoder hardware resources definition
 * @pdev: Pointer to struct platform_device.
 * @dev: Pointer to struct device.
 * @regs: Reg base for dos/dmc hardware.
 * @pm_mutex: Mutex for pm->pd_data.
 * @pm: Pointer to struct power_manager_s.
 * @hw_ops: Hardware resource operation functions. See struct aml_vdec_hw_ops.
 * @gates: Clk instance used by curr decoder context.
 * @dec_irq: Irq registered.
 * @curr_ctx: Pointer to curr decoder context.
 * @has_dbus_ctrl: Isolate VDEC through DOS instead of DMC; no local PD writes.
 * @faulted: DMA ownership could not be returned; reset the system to recover.
 * @powered: Domain and functional clocks are held.
 * @power_test_passed: Explicit read-only DOS access and power-off succeeded.
 * @power_test_reset: Cached DOS_SW_RESET0 sample; not a silicon identifier.
 * @mdec_test_passed: Explicit MDEC read and subsequent power-off succeeded.
 * @mdec_test_value: Cached MDEC_PIC_DC_CTRL sample.
 * @secure_swap: Scratch G supplied by the secure microcode loader.
 * @nic3_dev: Parent domain held explicitly for DOS-top access before VDEC on.
 * @nic3_held: An explicit parent runtime PM reference is owned.
 * @clocks_held: DOS bus and VDEC functional clocks are owned.
 * @probe_step: Last completed single-register diagnostic step, zero if idle.
 * @probe_value: Cached value of the last diagnostic read.
 * @probe_en0: DOS-top gate sample, including the shared HCODEC bits.
 * @probe_failed: A failed diagnostic prohibits another attempt in this probe.
 * @probe_pinned: Module reference held across diagnostic sysfs writes.
 * @clkctrl: Clock provider's regmap, used only to verify physical gate state.
 * @apb_gates: CPU APB bridge gates retained during experimental T7 power-on.
 * @apb_held: APB references remain held until secure power-off completes.
 * @vdec_p0: Functional clock path selected by the live T7 vendor decoder.
 */
struct aml_vdec_hw {
	struct platform_device *pdev;
	struct device *dev;
	struct regmap *map[MAX_REG_BUS];
	struct mutex pm_mutex;
	struct meson_canvas *canvas;
	const struct power_manager_s *pm;
	struct aml_vdec_hw_ops hw_ops;
	struct clk_bulk_data gates[DOS_CLK_MAX];
	int dec_irq;
	void *curr_ctx;
	bool has_dbus_ctrl;
	bool faulted;
	bool powered;
	bool power_test_passed;
	u32 power_test_reset;
	bool mdec_test_passed;
	u32 mdec_test_value;
	u32 secure_swap;
	struct device *nic3_dev;
	bool nic3_held;
	bool clocks_held;
	u32 probe_step;
	u32 probe_value;
	u32 probe_en0;
	bool probe_failed;
	bool probe_pinned;
	struct regmap *clkctrl;
	struct clk_bulk_data apb_gates[2];
	bool apb_held;
	struct clk *vdec_p0;
};

/**
 * struct power_manager_s - Power manager & opertion function
 * @pd_data: Pointer to struct pm_pd_s
 * @init: Power manager init.
 * @release: Power manager release.
 * @power_on: Power on for decoder hw.
 * @power_off: Power off for decoder hw.
 * @power_state: Query the power state.
 */
struct power_manager_s {
	struct pm_pd_s *pd_data;
	int (*init)(struct aml_vdec_hw *hw);
	void (*release)(struct aml_vdec_hw *hw);
	int (*power_on)(struct aml_vdec_hw *hw, int id);
	int (*power_off)(struct aml_vdec_hw *hw, int id);
	bool (*power_state)(struct aml_vdec_hw *hw, int id);
};

int dev_request_hw_resources(void *priv);
void dev_destroy_hw_resources(void *priv);
struct aml_vdec_hw *vdec_get_hw(void *priv);
u32 read_dos_reg(struct aml_vdec_hw *hw, u32 reg_addr);
int vdec_enable(struct aml_vdec_hw *hw);
int vdec_disable(struct aml_vdec_hw *hw);
int aml_vdec_t7_power_state(struct aml_vdec_hw *hw, s32 *vdec, s32 *nic3);
int aml_vdec_t7_power_test(struct aml_vdec_hw *hw);
int aml_vdec_t7_mdec_test(struct aml_vdec_hw *hw);
int aml_vdec_t7_register_step(struct aml_vdec_hw *hw, unsigned int step);
void dos_enable(struct aml_vdec_hw *hw);
void aml_start_vdec_hw(struct aml_vdec_hw *hw);
int aml_stop_vdec_hw(struct aml_vdec_hw *hw);
int load_firmware(struct aml_vdec_hw *hw, const char *path);
int aml_vdec_reset_core(struct aml_vdec_hw *hw);

#endif
