// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>
#include <linux/module.h>

#include "khadas_mcu_fan_kunit.h"

static void khadas_fan_curve(struct kunit *test)
{
	int temp, speed, previous = 0;

	for (temp = 0; temp <= 130000; temp += 1000) {
		speed = khadas_vim4_fan_target(temp, false);
		KUNIT_ASSERT_GE(test, speed, previous);
		KUNIT_ASSERT_LE(test, speed, 100);
		KUNIT_ASSERT_EQ(test, khadas_vim4_fan_target(temp, true), max(speed, 15));
		if (temp < 45000)
			KUNIT_EXPECT_EQ(test, speed, 0);
		if (temp >= 100000)
			KUNIT_EXPECT_EQ(test, speed, 100);
		previous = speed;
	}
	KUNIT_EXPECT_EQ(test, khadas_vim4_fan_target(INT_MIN, true), 15);
	KUNIT_EXPECT_EQ(test, khadas_vim4_fan_target(45000, false), 5);
	KUNIT_EXPECT_EQ(test, khadas_vim4_fan_target(50000, false), 15);
}

static void khadas_fan_custom_curve(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, khadas_vim4_fan_target_curve(49000, false, 65000, 85000, 100000), 0);
	KUNIT_EXPECT_EQ(test, khadas_vim4_fan_target_curve(65000, false, 65000, 85000, 100000), 35);
	KUNIT_EXPECT_EQ(test, khadas_vim4_fan_target_curve(100000, false, 65000, 85000, 100000),
			100);
	KUNIT_EXPECT_EQ(test, khadas_vim4_fan_target_curve(49000, true, 65000, 85000, 100000), 15);
}

static void khadas_fan_ramp(struct kunit *test)
{
	unsigned int current_speed, target, next;

	for (current_speed = 0; current_speed <= 100; current_speed++) {
		for (target = 0; target <= 100; target++) {
			next = khadas_vim4_fan_next(current_speed, target);
			if (!current_speed || !target || target == 100) {
				KUNIT_ASSERT_EQ(test, next, target);
				continue;
			}
			KUNIT_ASSERT_GE(test, next, min(current_speed, target));
			KUNIT_ASSERT_LE(test, next, max(current_speed, target));
			KUNIT_ASSERT_LE(test, abs((int)next - (int)current_speed), 5);
		}
	}
}

static struct kunit_case khadas_fan_cases[] = {
	KUNIT_CASE(khadas_fan_curve),
	KUNIT_CASE(khadas_fan_custom_curve),
	KUNIT_CASE(khadas_fan_ramp),
	{}
};

static struct kunit_suite khadas_fan_suite = {
	.name = "khadas-mcu-fan",
	.test_cases = khadas_fan_cases,
};

kunit_test_suite(khadas_fan_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Khadas MCU fan curve tests");
