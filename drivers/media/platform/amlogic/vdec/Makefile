# SPDX-License-Identifier: GPL-2.0-only
aml-vdec-drv-objs := aml_vdec.o aml_vdec_stateless.o aml_vdec_stateful.o aml_vdec_drv.o aml_vdec_hw.o aml_vdec_platform.o \
		     h264.o h264_headers.o mpeg12.o mjpeg.o hevc.o hevc_dpb.o hevc_hw.o

ifneq ($(CONFIG_VIDEO_AMLOGIC_VDEC_KUNIT_TEST),)
aml-vdec-drv-objs += hevc_kunit.o aml_vdec_job_kunit.o
endif

obj-$(CONFIG_VIDEO_AMLOGIC_VDEC) += aml-vdec-drv.o
obj-$(CONFIG_VIDEO_AMLOGIC_VDEC_KUNIT_TEST) += h264_headers_kunit.o
obj-$(CONFIG_VIDEO_AMLOGIC_VDEC_KUNIT_TEST) += mjpeg_kunit.o
obj-$(CONFIG_VIDEO_AMLOGIC_VDEC_KUNIT_TEST) += mpeg12_kunit.o
obj-$(CONFIG_VIDEO_AMLOGIC_VDEC_KUNIT_TEST) += h264_kunit.o
