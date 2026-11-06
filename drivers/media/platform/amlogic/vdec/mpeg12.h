/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * MPEG-1/2 decoding on the Amlogic legacy VDEC core.
 */

#ifndef _AML_MPEG12_H_
#define _AML_MPEG12_H_

#include <media/v4l2-ctrls.h>

#include <kunit/visibility.h>
#include <linux/v4l2-controls.h>

#if IS_ENABLED(CONFIG_KUNIT)
u32 mpeg12_write_headers(u8 *buf, const struct v4l2_ctrl_mpeg2_sequence *seq,
			 const struct v4l2_ctrl_mpeg2_picture *pic,
			 const struct v4l2_ctrl_mpeg2_quantisation *quant, bool with_seq);
unsigned int mpeg12_reference_timestamps(const struct v4l2_ctrl_mpeg2_picture *pic,
					 const struct v4l2_ctrl_mpeg2_picture *first,
					 bool pair, u64 refs[2]);
int mpeg12_reference_registers(const u64 refs[2], const u32 specs[2],
			       unsigned int count, u32 *ref0, u32 *ref1);

#endif

/* Scratch register meanings for the mpeg12_multi microcode (vmpeg12_multi.c). */
#define MREG_CC_ADDR		AV_SCRATCH_0
#define MREG_REF0		AV_SCRATCH_2
#define MREG_REF1		AV_SCRATCH_3
#define MREG_SEQ_INFO		AV_SCRATCH_4
#define MREG_PIC_INFO		AV_SCRATCH_5
#define MREG_PIC_WIDTH		AV_SCRATCH_6
#define MREG_PIC_HEIGHT		AV_SCRATCH_7
#define MREG_INPUT		AV_SCRATCH_8
#define MREG_BUFFEROUT		AV_SCRATCH_9
#define MREG_CMD		AV_SCRATCH_A
#define MREG_CO_MV_START	AV_SCRATCH_B
#define MREG_ERROR_COUNT	AV_SCRATCH_C
#define MREG_FRAME_OFFSET	AV_SCRATCH_D
#define MREG_WAIT_BUFFER	AV_SCRATCH_E
#define MREG_FATAL_ERROR	AV_SCRATCH_F
#define MREG_SEQ_REPORT		AV_SCRATCH_G
#define MREG_SIGNAL_TYPE	AV_SCRATCH_H
#define MREG_USERDATA		AV_SCRATCH_J
#define MREG_DECODE_STOP_POS	AV_SCRATCH_K
#define MREG_DEBUG		AV_SCRATCH_M

/* VLD block registers the microcode keeps its picture state in. */
#define MPEG1_2_REG		REG_ALIGN(0x0c01)
#define F_CODE_REG		REG_ALIGN(0x0c02)
#define PIC_HEAD_INFO		REG_ALIGN(0x0c03)
#define SLICE_VER_POS_PIC_TYPE	REG_ALIGN(0x0c04)

/* MREG_BUFFEROUT values. */
#define MPEG12_PIC_DONE		1
#define MPEG12_DATA_EMPTY	2
#define MPEG12_SEQ_END		3
#define MPEG12_DATA_REQUEST	4

#define MPEG12_PICINFO_ERROR	0x80000000
#define MPEG12_PICINFO_TYPE	0x00030000

#define MPEG12_WORKSPACE_SIZE	(4 * SZ_64K)
#define MPEG12_CCBUF_SIZE	(5 * 1024)

int aml_mpeg12_init(void *priv);
void aml_mpeg12_prepare(void *priv);
void aml_mpeg12_finish(void *priv);
void aml_mpeg12_exit(void *priv);
int aml_mpeg12_dec_run(void *priv);

extern const struct aml_vdec_reset_profile aml_mpeg12_reset_profile;

#define AML_MPEG12_NUM_CONTROLS 3
extern const struct v4l2_ctrl_config aml_mpeg12_controls[AML_MPEG12_NUM_CONTROLS];

#endif
