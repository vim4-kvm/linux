/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
#ifndef AML_H264_HEADERS_H
#define AML_H264_HEADERS_H

#include <linux/types.h>
#include <linux/v4l2-controls.h>

int aml_h264_build_headers(void *buffer, size_t capacity,
			   const struct v4l2_ctrl_h264_sps *sps,
			   const struct v4l2_ctrl_h264_pps *pps,
			   const struct v4l2_ctrl_h264_scaling_matrix *matrix);

#endif
