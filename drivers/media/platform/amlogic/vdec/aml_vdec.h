/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#ifndef _AML_VDEC_H_
#define _AML_VDEC_H_

#include "aml_vdec_drv.h"

/**
 * struct aml_vdec_v4l2_ctrl - helper type to declare supported ctrls
 * @codec_type: codec id this control belong to (CODEC_TYPE_H264, etc.)
 * @cfg: control configuration
 */
struct aml_vdec_v4l2_ctrl {
	unsigned int codec_type;
	struct v4l2_ctrl_config cfg;
};

struct aml_dec_type {
	unsigned int codec_type;
	const char *name;
};

extern const struct v4l2_m2m_ops aml_vdec_m2m_ops;
extern const struct v4l2_ioctl_ops aml_vdec_ioctl_ops;

int aml_vdec_ctrls_setup(struct aml_vdec_ctx *ctx);
int aml_vdec_queue_init(void *priv, struct vb2_queue *src_vq,
			struct vb2_queue *dst_vq);
void aml_vdec_reset_fmts(struct aml_vdec_ctx *ctx);
void aml_hevc_device_run(struct aml_vdec_ctx *ctx);
int aml_hevc_job_ready(struct aml_vdec_ctx *ctx);
void aml_hevc_cancel_output(struct aml_vdec_ctx *ctx);
void aml_hevc_start_capture(struct aml_vdec_ctx *ctx);
void aml_hevc_eos(struct aml_vdec_ctx *ctx);
void aml_hevc_last_buffer_done(struct aml_vdec_ctx *ctx,
			       struct vb2_v4l2_buffer *vb);
int aml_hevc_try_fmt(struct aml_vdec_ctx *ctx, enum v4l2_buf_type type,
		     struct v4l2_pix_format_mplane *fmt);
int aml_hevc_get_selection(struct aml_vdec_ctx *ctx, struct v4l2_selection *selection);
int aml_hevc_decoder_cmd(struct file *file, void *priv, struct v4l2_decoder_cmd *cmd);
#endif
