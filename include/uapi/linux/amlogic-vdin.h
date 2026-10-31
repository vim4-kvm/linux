/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_AMLOGIC_VDIN_H
#define _UAPI_AMLOGIC_VDIN_H

#include <linux/videodev2.h>

/* Private T7 full-packed 4:2:2, not P010 or Y210. One memory plane;
 * each five bytes contain Cb, Y0, Cr, Y1 in ascending little-endian
 * ten-bit slots. Rows start at bytesperline, aligned to 32 bytes.
 * no_swap_en=1; no reversal of 16-byte blocks. Padding is not image data.
 * Consumers must convert the layout before passing it to a standard codec.
 */
#define V4L2_PIX_FMT_AMLY v4l2_fourcc('A', 'M', 'L', 'Y')

#endif
