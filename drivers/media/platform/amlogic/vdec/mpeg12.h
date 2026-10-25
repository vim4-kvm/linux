/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * MPEG-1/2 decoding on the Amlogic legacy VDEC core.
 */

#ifndef _AML_MPEG12_H_
#define _AML_MPEG12_H_

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
#define PIC_HEAD_INFO		REG_ALIGN(0x0c03)

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
void aml_mpeg12_exit(void *priv);
int aml_mpeg12_dec_run(void *priv);

#endif
