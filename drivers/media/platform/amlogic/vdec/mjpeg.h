/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Motion-JPEG decoding on the Amlogic legacy VDEC core.
 */

#ifndef _AML_MJPEG_H_
#define _AML_MJPEG_H_

#include <kunit/visibility.h>
#include <linux/types.h>

/* Scratch register meanings for the mjpeg_multi microcode (vmjpeg_multi.c). */
#define MJPEG_DECODE_PARAM	AV_SCRATCH_2	/* height << 4 | 0x8000 */
#define MJPEG_TO_AMRISC		AV_SCRATCH_8
#define MJPEG_FROM_AMRISC	AV_SCRATCH_9
#define MJPEG_FRAME_OFFSET	AV_SCRATCH_A
#define MJPEG_PIC_WIDTH		AV_SCRATCH_B
#define MJPEG_PIC_HEIGHT	AV_SCRATCH_C
#define MJPEG_DEC_STATUS	AV_SCRATCH_F
#define MJPEG_STOP_POS		AV_SCRATCH_K

#define MJPEG_STATUS_SIZE	1	/* picture size known, waits for an ack */
#define MJPEG_STATUS_EMPTY	2
#define MJPEG_STATUS_TIMEOUT	0x21

/* Not in reg_defines.h; addresses from the staging meson vdec headers. */
#define PSCALE_RST		REG_ALIGN(0x0910)
#define PSCALE_BMEM_ADDR	REG_ALIGN(0x091f)
#define PSCALE_BMEM_DAT		REG_ALIGN(0x0920)
#define MCPU_INTR_MSK		REG_ALIGN(0x0304)
#define VDEC_ASSIST_AMR1_INT8	REG_ALIGN(0x002d)

int aml_mjpeg_init(void *priv);
void aml_mjpeg_prepare(void *priv);
void aml_mjpeg_finish(void *priv);
void aml_mjpeg_exit(void *priv);

#if IS_ENABLED(CONFIG_KUNIT)
bool mjpeg_supported(const u8 *p, u32 len, u32 max_width, u32 max_height);
#endif

#endif
