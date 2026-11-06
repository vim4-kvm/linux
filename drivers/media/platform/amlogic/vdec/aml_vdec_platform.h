/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#ifndef AML_VDEC_PLATFORM_H_
#define AML_VDEC_PLATFORM_H_

#include <linux/videodev2.h>
#include "aml_vdec_drv.h"

#define MAX_DEC_FORMAT 4

/**
 * struct aml_codec_ops - codec mode specific operations
 * @init: Used for decoder initialization.
 * @exit: If needed, can be used to undo the .init phase.
 * @run: Execute a request decoding job in sleepable context.
 * @prepare: Select codec callbacks before loading firmware for a job.
 * @finish: Release per-job canvas ownership after hardware stop.
 * Caller should ensure that a pair of buffers is ready and the
 * hardware is powered on and clk is enabled. Returns zero if OK,
 * a negative value in error cases.
 */
struct aml_codec_ops {
	const struct aml_vdec_reset_profile *reset;
	u32 secure_format;
	int (*post_load)(struct aml_vdec_hw *hw);
	int (*init)(void *priv);
	void (*exit)(void *priv);
	void (*prepare)(void *priv);
	int (*run)(void *priv);
	void (*finish)(void *priv);
};

struct aml_vdec_node_info {
	const struct aml_vdec_node_desc *desc;
	const char *name;
	enum vdec_type_e core;
};

/**
 * struct aml_dev_platform_data - compatible data for each chip.
 * @nodes: Video node descriptors in registration order.
 * @num_nodes: Number of platform video nodes.
 * @dec_fmt: Support dec format.
 * @codec_ops: Codec operation function.
 * @req_hw_resource: Operation function to request the hardware resource.
 * @destroy_hw_resource: Operation function to release the hardware resource.
 * @power_type: Type of power that the current chip need. See aml_power_type_e.
 * @fw_path: Path of the firmware.bin.
 * @has_dbus_ctrl: Use DOS DBUS isolation instead of a DMC request bit.
 * @num_fmts: Number of formats in dec_fmt.
 * @has_internal_canvas: Use the internal DOS canvas lookup table.
 */
struct aml_dev_platform_data {
	const struct aml_vdec_node_info *nodes;
	unsigned int num_nodes;
	const struct aml_codec_ops *codec_ops;
	const struct aml_video_fmt *dec_fmt;
	int num_fmts;
	int (*req_hw_resource)(void *priv);
	void (*destroy_hw_resource)(void *priv);
	int power_type;
	const char *fw_path[MAX_DEC_FORMAT];
	bool has_dbus_ctrl;
	bool has_internal_canvas;
};

extern const struct aml_dev_platform_data aml_vdec_s4_pdata;
extern const struct aml_dev_platform_data aml_vdec_t7_pdata;

#endif
