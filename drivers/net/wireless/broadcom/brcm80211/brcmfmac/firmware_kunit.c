// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "firmware.h"

static void brcmf_nvram_comment_at_eof_test(struct kunit *test)
{
	static const char input[] = "boardrev=0x1234\n# trailing comment";
	struct device *dev;
	u32 length;
	void *nvram;
	u8 *data;

	dev = kunit_device_register(test, "brcmf-nvram-eof");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	/* Firmware buffers need not include a NUL or a final newline. */
	data = kunit_kmalloc(test, sizeof(input) - 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, input, sizeof(input) - 1);
	nvram = brcmf_fw_nvram_strip(data, sizeof(input) - 1, &length,
				     0, 0, dev);
	KUNIT_ASSERT_NOT_NULL(test, nvram);
	KUNIT_EXPECT_STREQ(test, nvram, "boardrev=0x1234");
	kfree(nvram);
}

static void brcmf_nvram_comment_nul_test(struct kunit *test)
{
	static const char input[] = "# comment\0boardrev=0x1234\n";
	struct device *dev;
	u32 length;
	void *nvram;

	dev = kunit_device_register(test, "brcmf-nvram-nul");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	/* A NUL ends the comment even if a later entry contains a newline. */
	nvram = brcmf_fw_nvram_strip(input, sizeof(input) - 1, &length,
				     0, 0, dev);
	KUNIT_ASSERT_NOT_NULL(test, nvram);
	KUNIT_EXPECT_STREQ(test, nvram, "boardrev=0x1234");
	kfree(nvram);
}

static void brcmf_nvram_long_comment_test(struct kunit *test)
{
	static const char entry[] = "\nboardrev=0x1234\n#";
	const size_t comment_len = 65536;
	struct device *dev;
	u32 length;
	void *nvram;
	u8 *data;

	dev = kunit_device_register(test, "brcmf-nvram-long");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	/* The output limit must not truncate the input comment search. */
	data = kunit_kmalloc(test, comment_len + sizeof(entry) - 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memset(data, '#', comment_len);
	memcpy(data + comment_len, entry, sizeof(entry) - 1);
	nvram = brcmf_fw_nvram_strip(data, comment_len + sizeof(entry) - 1,
				     &length, 0, 0, dev);
	KUNIT_ASSERT_NOT_NULL(test, nvram);
	KUNIT_EXPECT_STREQ(test, nvram, "boardrev=0x1234");
	kfree(nvram);
}

static struct kunit_case brcmf_nvram_cases[] = {
	KUNIT_CASE(brcmf_nvram_comment_at_eof_test),
	KUNIT_CASE(brcmf_nvram_comment_nul_test),
	KUNIT_CASE(brcmf_nvram_long_comment_test),
	{}
};

static struct kunit_suite brcmf_nvram_suite = {
	.name = "brcmfmac-nvram",
	.test_cases = brcmf_nvram_cases,
};

kunit_test_suite(brcmf_nvram_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Broadcom FullMAC NVRAM parser tests");
