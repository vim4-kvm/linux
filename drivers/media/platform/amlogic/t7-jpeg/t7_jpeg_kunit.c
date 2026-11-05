// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include "t7-jpeg-kunit.h"

static void t7_jpeg_quality(struct kunit *test)
{
	u16 quant[2][64], previous[2][64];
	unsigned int quality, table, i;

	memset(previous, 0xff, sizeof(previous));
	for (quality = 1; quality <= 100; quality++) {
		t7_jpeg_quantize(quant, quality);
		for (table = 0; table < 2; table++) {
			for (i = 0; i < 64; i++) {
				KUNIT_ASSERT_GE(test, quant[table][i], 1);
				KUNIT_ASSERT_LE(test, quant[table][i], 255);
				KUNIT_ASSERT_LE(test, quant[table][i], previous[table][i]);
				previous[table][i] = quant[table][i];
				if (quality == 100)
					KUNIT_EXPECT_EQ(test, quant[table][i], 1);
			}
		}
	}
}

static void t7_jpeg_header(struct kunit *test)
{
	u16 quant[2][64];
	u8 *header;
	u32 length, offset, size;
	unsigned int table;

	header = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, header);
	memset(header, 0xa5, PAGE_SIZE);
	t7_jpeg_quantize(quant, 50);
	length = make_header(header, 4096, 2160, quant);
	KUNIT_ASSERT_LT(test, length, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, length % 8, 0);
	KUNIT_EXPECT_EQ(test, header[length], 0xa5);
	KUNIT_ASSERT_EQ(test, get_unaligned_be16(header), 0xffd8);
	KUNIT_ASSERT_EQ(test, get_unaligned_be16(header + 2), 0xffdb);
	KUNIT_ASSERT_EQ(test, get_unaligned_be16(header + 4), 132);
	for (table = 0; table < 2; table++) {
		const u8 *dqt = header + 6 + table * 65;

		KUNIT_EXPECT_EQ(test, dqt[0], table);
		/* First diagonal turn distinguishes zigzag from raster storage. */
		KUNIT_EXPECT_EQ(test, dqt[1], quant[table][0]);
		KUNIT_EXPECT_EQ(test, dqt[3], quant[table][8]);
		KUNIT_EXPECT_EQ(test, dqt[4], quant[table][16]);
	}
	offset = 2 + 2 + 132;
	KUNIT_ASSERT_EQ(test, get_unaligned_be16(header + offset), 0xffc4);
	size = get_unaligned_be16(header + offset + 2);
	KUNIT_ASSERT_EQ(test, size, 418);
	offset += 2 + size;
	KUNIT_ASSERT_EQ(test, get_unaligned_be16(header + offset), 0xffc0);
	KUNIT_EXPECT_EQ(test, header[offset + 4], 8);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(header + offset + 5), 2160);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(header + offset + 7), 4096);
	KUNIT_EXPECT_EQ(test, header[offset + 9], 3);
	KUNIT_EXPECT_EQ(test, header[offset + 11], 0x22);
	KUNIT_EXPECT_EQ(test, header[offset + 14], 0x11);
	KUNIT_EXPECT_EQ(test, header[offset + 17], 0x11);
	offset += 19;
	/* Fill bytes may precede SOS, but may never enter its scan payload. */
	while (offset < length && header[offset] == 0xff)
		offset++;
	KUNIT_ASSERT_LT(test, offset, length);
	KUNIT_EXPECT_EQ(test, header[offset], 0xda);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(header + offset + 1), 12);
	KUNIT_EXPECT_EQ(test, length - offset, 13);
	KUNIT_EXPECT_EQ(test, header[length - 3], 0);
	KUNIT_EXPECT_EQ(test, header[length - 2], 63);
	KUNIT_EXPECT_EQ(test, header[length - 1], 0);
}

static struct kunit_case t7_jpeg_cases[] = {
	KUNIT_CASE(t7_jpeg_quality),
	KUNIT_CASE(t7_jpeg_header),
	{}
};

static struct kunit_suite t7_jpeg_suite = {
	.name = "t7-jpeg",
	.test_cases = t7_jpeg_cases,
};

kunit_test_suite(t7_jpeg_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T7 JPEG header serialization tests");
