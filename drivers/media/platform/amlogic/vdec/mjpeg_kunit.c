// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "mjpeg.h"

static const u8 jpeg_header[] = {
	0xff, 0xd8, 0xff, 0xfe, 0, 4, 0, 0,
	0xff, 0xc0, 0, 17, 8, 1, 0x70, 2, 0x80, 3,
	1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1,
};

static void truncated_header(struct kunit *test)
{
	unsigned int size;

	for (size = 0; size < sizeof(jpeg_header); size++)
		KUNIT_EXPECT_FALSE(test, mjpeg_supported(jpeg_header, size, 640, 384));
	KUNIT_EXPECT_FALSE(test, mjpeg_supported(NULL, sizeof(jpeg_header), 640, 384));
	KUNIT_EXPECT_TRUE(test, mjpeg_supported(jpeg_header, sizeof(jpeg_header), 640, 384));
}

static void sampling_and_capacity(struct kunit *test)
{
	u8 header[sizeof(jpeg_header)];

	memcpy(header, jpeg_header, sizeof(header));
	KUNIT_EXPECT_FALSE(test, mjpeg_supported(header, sizeof(header), 639, 384));
	KUNIT_EXPECT_FALSE(test, mjpeg_supported(header, sizeof(header), 640, 367));
	header[9] = 0xc2;
	KUNIT_EXPECT_FALSE(test, mjpeg_supported(header, sizeof(header), 640, 384));
	header[9] = 0xc1;
	KUNIT_EXPECT_TRUE(test, mjpeg_supported(header, sizeof(header), 640, 384));
	header[19] = 0x21;
	KUNIT_EXPECT_FALSE(test, mjpeg_supported(header, sizeof(header), 640, 384));
	header[19] = 0x22;
	header[12] = 12;
	KUNIT_EXPECT_FALSE(test, mjpeg_supported(header, sizeof(header), 640, 384));
	header[12] = 8;
	header[5] = 1;
	KUNIT_EXPECT_FALSE(test, mjpeg_supported(header, sizeof(header), 640, 384));
	header[5] = 0xff;
	KUNIT_EXPECT_FALSE(test, mjpeg_supported(header, sizeof(header), 640, 384));
}

static struct kunit_case mjpeg_test_cases[] = {
	KUNIT_CASE(truncated_header),
	KUNIT_CASE(sampling_and_capacity),
	{}
};

static struct kunit_suite mjpeg_test_suite = {
	.name = "amlogic-mjpeg-headers",
	.test_cases = mjpeg_test_cases,
};

kunit_test_suite(mjpeg_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
