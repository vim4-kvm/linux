// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/resource.h>
#include <kunit/test.h>
#include <linux/debug_locks.h>

#include "adlak_os.h"

static void adlak_mutex_cleanup(void *handle)
{
	adlak_os_mutex_destroy(handle);
}

static void adlak_mutex_roles_test(struct kunit *test)
{
	adlak_os_mutex_t *device, *smmu;
	int ret;

	device = kunit_kzalloc(test, sizeof(*device), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, device);
	smmu = kunit_kzalloc(test, sizeof(*smmu), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, smmu);
	KUNIT_ASSERT_EQ(test, adlak_os_mutex_init(device), 0);
	ret = kunit_add_action_or_reset(test, adlak_mutex_cleanup, device);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, adlak_os_mutex_init(smmu), 0);
	ret = kunit_add_action_or_reset(test, adlak_mutex_cleanup, smmu);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Device initialization holds its mutex while initializing the SMMU. */
	KUNIT_ASSERT_EQ(test, READ_ONCE(debug_locks), 1);
	KUNIT_ASSERT_EQ(test, adlak_os_mutex_lock(device), 0);
	KUNIT_EXPECT_EQ(test, adlak_os_mutex_lock(smmu), 0);
	KUNIT_EXPECT_EQ(test, adlak_os_mutex_unlock(smmu), 0);
	KUNIT_EXPECT_EQ(test, adlak_os_mutex_unlock(device), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(debug_locks), 1);
}

static struct kunit_case adlak_os_cases[] = {
	KUNIT_CASE(adlak_mutex_roles_test),
	{}
};

static struct kunit_suite adlak_os_suite = {
	.name = "adlak-os-mutex",
	.test_cases = adlak_os_cases,
};

kunit_test_suite(adlak_os_suite);
