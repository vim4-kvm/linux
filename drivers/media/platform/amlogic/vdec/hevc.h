/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * HEVC (H.265) decoding on the Amlogic HEVC core.
 */

#ifndef _AML_HEVC_H_
#define _AML_HEVC_H_

#include "hevc_regs.h"
#include "hevc_dpb.h"

/*
 * Scratch register meanings for the HEVC microcode.  Several of these names
 * are already taken by av1.h for a different scratch register - each codec's
 * microcode gives the same register its own meaning - so the HEVC ones carry
 * an H265_ prefix.  From the reference's vh265.c.
 */
#define HEVC_ASSIST_SCRATCH_6		REG_ALIGN(0x30c6)
#define HEVC_ASSIST_SCRATCH_C		REG_ALIGN(0x30cc)
#define HEVC_ASSIST_SCRATCH_D		REG_ALIGN(0x30cd)

#define H265_VPS_BUFFER			HEVC_ASSIST_SCRATCH_3
#define H265_SPS_BUFFER			HEVC_ASSIST_SCRATCH_4
#define H265_SAO_UP			HEVC_ASSIST_SCRATCH_6
#define H265_SAO_MEM_UNIT		HEVC_ASSIST_SCRATCH_9
#define H265_SAO_ABV			HEVC_ASSIST_SCRATCH_A
#define H265_SAO_VB_SIZE		HEVC_ASSIST_SCRATCH_B
#define H265_SAO_VB			HEVC_ASSIST_SCRATCH_C
#define H265_SCALELUT			HEVC_ASSIST_SCRATCH_D
#define H265_LMEM_DUMP_ADR		HEVC_ASSIST_SCRATCH_F
#define H265_DEBUG_REG1			HEVC_ASSIST_SCRATCH_G
#define H265_DECODE_MODE2		HEVC_ASSIST_SCRATCH_H
#define H265_NAL_SEARCH_CTL		HEVC_ASSIST_SCRATCH_I
#define H265_CUR_NAL_UNIT_TYPE		HEVC_ASSIST_SCRATCH_J	/* read */
#define H265_DECODE_MODE		HEVC_ASSIST_SCRATCH_J	/* write */
#define H265_DECODE_STOP_POS		HEVC_ASSIST_SCRATCH_K
#define H265_AUX_ADR			HEVC_ASSIST_SCRATCH_L
#define H265_AUX_DATA_SIZE		HEVC_ASSIST_SCRATCH_M
#define H265_DECODE_SIZE		HEVC_ASSIST_SCRATCH_N
/* Read back after a chunk: [15:8] rps_set_id, [7:0] start_decoding_flag. */
#define H265_DECODE_INFO		HEVC_ASSIST_SCRATCH_1

/* Not in av1.h/vp9.h yet; addresses from the staging meson hevc_regs.h. */
#define HEVC_PARSER_LCU_START		REG_ALIGN(0x3124)
#define HEVC_MCPU_INTR_REQ		REG_ALIGN(0x3305)
#define HEVC_MPRED_CTRL1		REG_ALIGN(0x3202)
#define HEVC_MPRED_PIC_SIZE		REG_ALIGN(0x3205)
#define HEVC_MPRED_PIC_SIZE_LCU		REG_ALIGN(0x3206)
#define HEVC_MPRED_TILE_START		REG_ALIGN(0x3207)
#define HEVC_MPRED_TILE_SIZE_LCU	REG_ALIGN(0x3208)
#define HEVC_MPRED_REF_NUM		REG_ALIGN(0x3209)
#define HEVC_MPRED_LT_REF		REG_ALIGN(0x320a)
#define HEVC_MPRED_REF_EN_L0		REG_ALIGN(0x320c)
#define HEVC_MPRED_REF_EN_L1		REG_ALIGN(0x320d)
#define HEVC_MPRED_L1_REF00_POC		REG_ALIGN(0x3230)
#define HEVC_MPRED_CUR_POC		REG_ALIGN(0x3260)
#define HEVC_MPRED_COL_POC		REG_ALIGN(0x3261)
#define HEVC_SAO_PIC_SIZE		REG_ALIGN(0x3605)
#define HEVC_SAO_PIC_SIZE_LCU		REG_ALIGN(0x3606)
#define HEVC_DBLK_CFG1			REG_ALIGN(0x3501)
#define HEVC_DBLK_CFG2			REG_ALIGN(0x3502)
#define HEVC_DBLK_STS1			REG_ALIGN(0x350c)

#define AMRISC_MAIN_REQ			0x04

/* Status the microcode posts, and the commands the host answers with. */
#define H265_NAL_UNIT_VPS		0x01
#define H265_NAL_UNIT_SPS		0x02
#define H265_NAL_UNIT_PPS		0x03
#define H265_NAL_UNIT_CODED_SLICE	0x04
#define H265_CODED_SLICE_SEGMENT_DAT	0x05
#define H265_SLICE_SEGMENT_DONE		0x08
#define H265_NAL_SEARCH_DONE		0x09
#define H265_DECPIC_DATA_DONE		0x0a
#define H265_DECPIC_DATA_ERROR		0x0b
#define H265_SEI_DAT			0x0c
#define H265_SEI_DAT_DONE			0x0d
#define H265_NAL_DECODE_DONE		0x0e
#define H265_OVER_DECODE		0x0f
#define H265_DECODE_BUFEMPTY		0x20
#define H265_DECODE_TIMEOUT		0x21
#define H265_SEARCH_BUFEMPTY		0x22
#define H265_DECODE_OVER_SIZE		0x23
#define H265_DECODE_BUFEMPTY2		0x24
#define H265_DECODE_PARAMS_ERR		0x25
#define H265_FIND_NEXT_PIC_NAL		0x50
#define H265_DISCARD_NAL		0xf0
#define H265_ACTION_DONE		0xff

#define H265_DECODE_MODE_MULTI_FRAMEBASE	0x1

int aml_hevc_init(void *priv);
void aml_hevc_exit(void *priv);
int aml_hevc_configure(void *priv);

#endif
