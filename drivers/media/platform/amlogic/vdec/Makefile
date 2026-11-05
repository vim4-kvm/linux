# SPDX-License-Identifier: GPL-2.0-only
aml-vdec-drv-objs := aml_vdec.o aml_vdec_drv.o aml_vdec_hw.o aml_vdec_platform.o \
		     h264.o h264_headers.o mpeg12.o

obj-$(CONFIG_VIDEO_AMLOGIC_VDEC) += aml-vdec-drv.o
obj-$(CONFIG_VIDEO_AMLOGIC_VDEC_KUNIT_TEST) += h264_headers_kunit.o
obj-$(CONFIG_VIDEO_AMLOGIC_VDEC_KUNIT_TEST) += h264_kunit.o
obj-$(CONFIG_VIDEO_AMLOGIC_VDEC_KUNIT_TEST) += mpeg12_kunit.o
