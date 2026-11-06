/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#ifndef _AML_VDEC_H_
#define _AML_VDEC_H_

#include "aml_vdec_drv.h"

struct aml_vdec_frame {
	struct vb2_v4l2_buffer *buffer;
	/* No codec-owned output buffer remains after this frame is taken. */
	bool final;
};

struct aml_vdec_decode_result {
	bool format_changed;
	bool picture;
	unsigned int slot;
	u32 flags;
};

struct aml_vdec_stream_info {
	struct v4l2_pix_format_mplane format;
	struct v4l2_rect crop;
	unsigned int min_capture;
};

struct aml_vdec_stateful_ops {
	int (*prepare)(struct aml_vdec_ctx *ctx);
	int (*submit)(struct aml_vdec_ctx *ctx, struct vb2_v4l2_buffer *src,
		      struct vb2_v4l2_buffer *dst, struct aml_vdec_decode_result *result);
	void (*queue_capture)(struct aml_vdec_ctx *ctx, unsigned int slot,
			      struct vb2_v4l2_buffer *dst);
	int (*get_frame)(struct aml_vdec_ctx *ctx, bool flush, struct aml_vdec_frame *frame);
	void (*get_format)(struct aml_vdec_ctx *ctx, struct aml_vdec_stream_info *info);
};

extern const struct aml_vdec_stateful_ops aml_hevc_stateful_ops;
extern const struct aml_vdec_stateful_ops aml_mjpeg_stateful_ops;

struct aml_vdec_stateful_ctx {
	struct aml_vdec_ctx ctx;
	/* Serializes stateful metadata and retained CAPTURE buffers. */
	struct mutex lock;
	struct aml_vdec_frame frame;
	bool wait_capture;
	u32 min_capture;
	bool format_known;
	u32 sequence;
	struct v4l2_pix_format_mplane format;
	struct v4l2_rect crop;
};

static inline struct aml_vdec_stateful_ctx *aml_vdec_stateful(struct aml_vdec_ctx *ctx)
{
	return container_of(ctx, struct aml_vdec_stateful_ctx, ctx);
}

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
int aml_hevc_try_fmt(struct aml_vdec_ctx *ctx, enum v4l2_buf_type type,
		     struct v4l2_pix_format_mplane *fmt);
#endif
