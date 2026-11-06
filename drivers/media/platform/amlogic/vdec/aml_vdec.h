/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#ifndef _AML_VDEC_H_
#define _AML_VDEC_H_

#include "aml_vdec_drv.h"

extern const struct v4l2_m2m_ops aml_vdec_m2m_ops;
extern const struct v4l2_ioctl_ops aml_vdec_stateless_ioctl_ops;
extern const struct v4l2_ioctl_ops aml_vdec_stateful_ioctl_ops;

void aml_vdec_run_pair(struct aml_vdec_ctx *ctx);
int vidioc_vdec_querycap(struct file *file, void *priv,
			 struct v4l2_capability *cap);
int vidioc_vdec_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *fsize);
int vidioc_try_fmt_cap_mplane(struct file *file, void *priv,
			      struct v4l2_format *f);
int vidioc_try_fmt_out_mplane(struct file *file, void *priv,
			      struct v4l2_format *f);
int vidioc_vdec_s_fmt_out_mplane(struct file *file, void *priv,
				 struct v4l2_format *f);
int vidioc_vdec_s_fmt_cap_mplane(struct file *file, void *priv,
				 struct v4l2_format *f);
int vidioc_vdec_g_fmt_out_mplane(struct file *file, void *priv,
				 struct v4l2_format *f);
int vidioc_vdec_g_fmt_cap_mplane(struct file *file, void *priv,
				 struct v4l2_format *f);
int vidioc_vdec_enum_fmt_out_mplane(struct file *file,
				    void *priv, struct v4l2_fmtdesc *f);
int vidioc_vdec_enum_fmt_cap_mplane(struct file *file,
				    void *priv, struct v4l2_fmtdesc *f);
int aml_vdec_init_dec_inst(struct aml_vdec_ctx *ctx);
void aml_vdec_release_instance(struct aml_vdec_ctx *ctx);
int vb2ops_vdec_queue_setup(struct vb2_queue *vq,
			    unsigned int *nbuffers,
			    unsigned int *nplanes,
			    unsigned int sizes[],
			    struct device *alloc_devs[]);
int vb2ops_vdec_buf_prepare(struct vb2_buffer *vb);
int vb2ops_vdec_out_buf_validate(struct vb2_buffer *vb);
void vb2_ops_vdec_buf_finish(struct vb2_buffer *vb);
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
