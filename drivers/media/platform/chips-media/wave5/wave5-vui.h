/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
#ifndef WAVE5_VUI_H
#define WAVE5_VUI_H

/* Firmware takes a bit count without trailing bits or start codes. */
static int wave5_make_vui(u8 data[5], bool hevc, u32 colorspace, u32 transfer,
			  u32 matrix, u32 quantization)
{
	u8 primaries, xfer, coefficients;

	if (colorspace == V4L2_COLORSPACE_DEFAULT)
		colorspace = V4L2_COLORSPACE_REC709;
	switch (colorspace) {
	case V4L2_COLORSPACE_DEFAULT:
	case V4L2_COLORSPACE_REC709:
	case V4L2_COLORSPACE_SRGB:
		primaries = 1;
		break;
	case V4L2_COLORSPACE_SMPTE170M:
		primaries = 6;
		break;
	case V4L2_COLORSPACE_BT2020:
		primaries = 9;
		break;
	default:
		return -EINVAL;
	}
	if (transfer == V4L2_XFER_FUNC_DEFAULT)
		transfer = V4L2_MAP_XFER_FUNC_DEFAULT(colorspace);
	switch (transfer) {
	case V4L2_XFER_FUNC_709:
		xfer = 1;
		break;
	case V4L2_XFER_FUNC_SRGB:
		xfer = 13;
		break;
	case V4L2_XFER_FUNC_SMPTE2084:
		xfer = 16;
		break;
	default:
		return -EINVAL;
	}
	if (matrix == V4L2_YCBCR_ENC_DEFAULT)
		matrix = V4L2_MAP_YCBCR_ENC_DEFAULT(colorspace);
	switch (matrix) {
	case V4L2_YCBCR_ENC_709:
		coefficients = 1;
		break;
	case V4L2_YCBCR_ENC_601:
		coefficients = 6;
		break;
	case V4L2_YCBCR_ENC_BT2020:
		coefficients = 9;
		break;
	default:
		return -EINVAL;
	}
	if (quantization == V4L2_QUANTIZATION_DEFAULT)
		quantization = V4L2_MAP_QUANTIZATION_DEFAULT(false, colorspace, matrix);
	if (quantization != V4L2_QUANTIZATION_FULL_RANGE &&
	    quantization != V4L2_QUANTIZATION_LIM_RANGE)
		return -EINVAL;
	data[0] = 0x35 | (quantization == V4L2_QUANTIZATION_FULL_RANGE ? 2 : 0);
	data[1] = primaries;
	data[2] = xfer;
	data[3] = coefficients;
	data[4] = 0;
	return hevc ? 39 : 38;
}
#endif
