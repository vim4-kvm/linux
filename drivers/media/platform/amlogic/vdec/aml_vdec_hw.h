/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#ifndef _AML_VDEC_HW_H_
#define _AML_VDEC_HW_H_

#include <kunit/visibility.h>

#include "aml_vdec_drv.h"

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

#if IS_ENABLED(CONFIG_KUNIT)
u64 aml_vdec_canvas_layout(dma_addr_t addr, u32 stride, u32 height);
#endif

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
 * enum vdec_irq_num - Definition of the irq.
 * @VDEC_IRQ_0: First mailbox interrupt.
 * @VDEC_IRQ_1: Second mailbox interrupt.
 * @VDEC_IRQ_2: Third mailbox interrupt.
 * @VDEC_IRQ_MAX: Number of interrupt slots.
 */
enum vdec_irq_num {
	VDEC_IRQ_0 = 0,
	VDEC_IRQ_1,
	VDEC_IRQ_2,
	VDEC_IRQ_MAX,
};

/**
 * enum clk_type_e - Type of decoder clock.
 * @VDEC: Decoder bus clock.
 * @VDEC_MUX: Legacy decoder functional clock.
 * @HEVCF_MUX: HEVC front-end functional clock.
 * @DOS_CLK_MAX: Number of shared clock slots.
 */
enum clk_type_e {
	VDEC = 0,
	VDEC_MUX,
	HEVCF_MUX,
	DOS_CLK_MAX,
};

/**
 * enum aml_power_type_e - Type of decoder power.
 * @AML_PM_PD: Generic power-domain management.
 */
enum aml_power_type_e {
	AML_PM_PD = 0,
};

/**
 * enum mm_bus_e - Type of decoder hardware bus.
 * @DOS_BUS: Decoder register bank.
 * @DMC_BUS: Memory request control register bank.
 * @MAX_REG_BUS: Number of register banks.
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
 * @link: Link retaining the power-domain supplier.
 * @ref_count: Curr power domain instance ref count.
 */
struct pm_pd_s {
	u8 *name;
	struct device *dev;
	struct device_link *link;
	int ref_count;
};

/* Shared DOS resources and the two independently stopped hardware cores. */

struct aml_vdec1_core {
	struct aml_vdec_job *job;
	/* DOS bus and VDEC functional clocks, enabled as the original pair. */
	struct clk_bulk_data *gates;
	struct clk *parent;
	struct aml_vdec_hw_ops ops;
	int irq;
	void *priv;
	bool powered;
	bool clocks_held;
	u32 secure_swap;
	u32 irq_count;
	u32 threaded_irq_count;
};

struct aml_hevc_core {
	struct aml_vdec_job *job;
	struct clk_bulk_data *front;
	struct clk *front_parent;
	struct clk *back;
	struct clk *back_parent;
	bool present;
	bool powered;
	int irq;
	bool irq_enabled;
	bool fw_loaded;
	bool fw_swap;
	u32 fw_swap_addr;
	u32 irq_status;
	u32 axi_front;
	u32 axi_back;
	irqreturn_t (*irq_thread)(int irq, void *priv);
	void *priv;
};

struct aml_vdec_diagnostics {
	bool power_test_passed;
	u32 power_test_reset;
	bool mdec_test_passed;
	u32 mdec_test_value;
	u32 probe_step;
	u32 probe_value;
	u32 probe_en0;
	bool probe_failed;
	bool probe_pinned;
	bool hevc_test_passed;
	u32 hevc_parser_version;
	bool hevc_core_test_passed;
};

struct aml_vdec_hw {
	struct aml_vdec1_core vdec1;
	struct aml_hevc_core hevc;
	struct aml_vdec_diagnostics diag;
	/* Contiguous registration storage; cores retain their own clock views. */
	struct clk_bulk_data clock_data[DOS_CLK_MAX];
	struct platform_device *pdev;
	struct device *dev;
	struct regmap *map[MAX_REG_BUS];
	/* Serializes power-domain references shared by both cores. */
	struct mutex pm_mutex;
	struct meson_canvas *canvas;
	const struct power_manager_s *pm;
	bool has_dbus_ctrl;
	bool has_internal_canvas;
	bool canvas_endian_remap;
	bool faulted;
	struct device *nic3_dev;
	bool nic3_held;
	struct regmap *clkctrl;
	struct clk_bulk_data apb_gates[2];
	bool apb_held;
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

struct aml_codec_ops;
struct aml_vdec_reset_profile {
	u32 mask;
	bool preserve_power_ctl;
};

extern const struct aml_vdec_job_ops aml_vdec1_job_ops;
extern const struct aml_vdec_job_ops aml_hevc_job_ops;

bool aml_vdec_core_present(struct aml_vdec_hw *hw, enum vdec_type_e core);
int dev_request_hw_resources(void *priv);
void dev_destroy_hw_resources(void *priv);
struct aml_vdec_hw *vdec_get_hw(void *priv);
u32 read_dos_reg(struct aml_vdec_hw *hw, u32 reg_addr);
int vdec_enable(struct aml_vdec_hw *hw);
int vdec_disable(struct aml_vdec_hw *hw);
int aml_vdec_t7_power_state(struct aml_vdec_hw *hw, s32 *vdec, s32 *nic3);
int aml_vdec_t7_power_test(struct aml_vdec_hw *hw);
int aml_vdec_t7_mdec_test(struct aml_vdec_hw *hw);
int aml_vdec_t7_hevc_test(struct aml_vdec_hw *hw);
int aml_vdec_t7_hevc_core_test(struct aml_vdec_hw *hw);
int aml_hevc_prepare_hw(struct aml_vdec_hw *hw);
int aml_hevc_enable_irq(struct aml_vdec_hw *hw,
			irqreturn_t (*thread)(int irq, void *priv), void *priv);
int aml_hevc_stop_hw(struct aml_vdec_hw *hw);
int aml_hevc_release_hw(struct aml_vdec_hw *hw);
int aml_vdec_t7_register_step(struct aml_vdec_hw *hw, unsigned int step);
void dos_enable(struct aml_vdec_hw *hw);
void aml_start_vdec_hw(struct aml_vdec_hw *hw);
int aml_stop_vdec_hw(struct aml_vdec_hw *hw);
int load_firmware(struct aml_vdec_hw *hw, const char *path,
		  const struct aml_codec_ops *codec);
int aml_vdec_write_internal_canvas(struct aml_vdec_hw *hw, u8 index,
				   dma_addr_t addr, u32 stride, u32 height);
int aml_vdec_reset_core(struct aml_vdec_hw *hw,
			const struct aml_vdec_reset_profile *profile);

#endif
