// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "../t7-dos-state.h"

static void shared_leases_block_decoder_until_last_release(struct kunit *test)
{
	struct meson_t7_dos_state state = {};

	KUNIT_ASSERT_EQ(test, meson_t7_dos_acquire(&state, false), 0);
	KUNIT_ASSERT_EQ(test, meson_t7_dos_acquire(&state, false), 0);
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, true), -EBUSY);
	meson_t7_dos_release(&state, false);
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, true), -EBUSY);
	meson_t7_dos_release(&state, false);
	KUNIT_ASSERT_EQ(test, meson_t7_dos_acquire(&state, true), 0);
	meson_t7_dos_release(&state, true);
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, false), 0);
}

static void decoder_lease_blocks_all_other_leases(struct kunit *test)
{
	struct meson_t7_dos_state state = {};

	KUNIT_ASSERT_EQ(test, meson_t7_dos_acquire(&state, true), 0);
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, false), -EBUSY);
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, true), -EBUSY);
	meson_t7_dos_release(&state, true);
	KUNIT_ASSERT_EQ(test, meson_t7_dos_acquire(&state, false), 0);
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, false), 0);
}

static void poisoned_state_denies_leases_after_release(struct kunit *test)
{
	struct meson_t7_dos_state state = {};

	KUNIT_ASSERT_EQ(test, meson_t7_dos_acquire(&state, true), 0);
	state.poisoned = true;
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, false), -EIO);
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, true), -EIO);
	meson_t7_dos_release(&state, true);
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, false), -EIO);
	KUNIT_EXPECT_EQ(test, meson_t7_dos_acquire(&state, true), -EIO);
}

static struct kunit_case t7_dos_cases[] = {
	KUNIT_CASE(shared_leases_block_decoder_until_last_release),
	KUNIT_CASE(decoder_lease_blocks_all_other_leases),
	KUNIT_CASE(poisoned_state_denies_leases_after_release),
	{}
};

static struct kunit_suite t7_dos_suite = {
	.name = "amlogic-t7-dos",
	.test_cases = t7_dos_cases,
};

kunit_test_suite(t7_dos_suite);
