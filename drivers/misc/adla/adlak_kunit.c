// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/bitmap.h>
#include <linux/limits.h>
#include <linux/module.h>

#include "adlak_bounds.h"
#include "adlak_context.h"
#include "adlak_device.h"
#include "adlak_kunit.h"
#include "adlak_mm_common.h"
#include "adlak_os.h"
#include "adlak_submit.h"

static void adla_pool_release(void *data)
{
	adlak_bitmap_pool_deinit(data);
}

static void adla_bitmap_pool_test(struct kunit *test)
{
	const size_t page = ADLAK_MM_POOL_PAGE_SIZE;
	struct adlak_mem_exclude excluded = { .start = 30 * page, .size = 20 * page };
	struct adlak_mm_pool_priv *pool;
	unsigned long first, second;

	pool = kunit_kzalloc(test, sizeof(*pool), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pool);
	KUNIT_ASSERT_EQ(test, adlak_bitmap_pool_init(pool, 32 * page, &excluded), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, adla_pool_release, pool), 0);
	KUNIT_ASSERT_NOT_NULL(test, pool->lock);
	KUNIT_EXPECT_EQ(test, adlak_alloc_from_bitmap_pool(pool, 0), ADLAK_INVALID_ADDR);
	KUNIT_EXPECT_EQ(test, adlak_alloc_from_bitmap_pool(pool, SIZE_MAX), ADLAK_INVALID_ADDR);

	first = adlak_alloc_from_bitmap_pool(pool, page + 1);
	second = adlak_alloc_from_bitmap_pool(pool, 28 * page);
	KUNIT_ASSERT_EQ(test, first, 0UL);
	KUNIT_ASSERT_EQ(test, second, 2 * page);
	KUNIT_EXPECT_EQ(test, pool->used_count, (size_t)30);
	KUNIT_EXPECT_EQ(test, adlak_alloc_from_bitmap_pool(pool, page), ADLAK_INVALID_ADDR);
	KUNIT_EXPECT_EQ(test, pool->used_count, (size_t)30);

	adlak_free_to_bitmap_pool(pool, first, page + 1);
	KUNIT_EXPECT_EQ(test, pool->used_count, (size_t)28);
	KUNIT_EXPECT_EQ(test, adlak_alloc_from_bitmap_pool(pool, page), 0UL);
	KUNIT_EXPECT_EQ(test, adlak_alloc_from_bitmap_pool(pool, page), page);
	adlak_free_to_bitmap_pool(pool, 0, 2 * page);
	adlak_free_to_bitmap_pool(pool, second, 28 * page);
	KUNIT_EXPECT_EQ(test, pool->used_count, (size_t)0);
	KUNIT_EXPECT_EQ(test, bitmap_weight(pool->bitmap, pool->bits), 2);
	KUNIT_EXPECT_TRUE(test, test_bit(30, pool->bitmap));
	KUNIT_EXPECT_TRUE(test, test_bit(31, pool->bitmap));
}

static void adla_mutex_release(void *data)
{
	adlak_os_mutex_destroy(data);
}

static void adla_add_task(struct list_head *list, struct adlak_task *task,
			  struct adlak_context *context, int invoke_id)
{
	task->context = context;
	task->invoke_idx = invoke_id;
	context->invoke_cnt++;
	list_add_tail(&task->head, list);
}

static void adla_queued_cancel_test(struct kunit *test)
{
	struct adla_cancel_fixture {
		struct adlak_device dev;
		struct adlak_context contexts[2];
		struct adlak_task tasks[4];
	} *fixture;
	struct adlak_workqueue *queue;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	queue = &fixture->dev.queue;
	INIT_LIST_HEAD(&queue->pending_list);
	INIT_LIST_HEAD(&queue->finished_list);
	INIT_LIST_HEAD(&queue->scheduled_list);
	KUNIT_ASSERT_EQ(test, adlak_os_mutex_init(&queue->wq_mutex), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, adla_mutex_release,
							&queue->wq_mutex), 0);
	fixture->contexts[0].net_id = 1;
	fixture->contexts[1].net_id = 2;
	adla_add_task(&queue->pending_list, &fixture->tasks[0], &fixture->contexts[0], 1);
	adla_add_task(&queue->pending_list, &fixture->tasks[1], &fixture->contexts[1], 2);
	adla_add_task(&queue->finished_list, &fixture->tasks[2], &fixture->contexts[0], 3);
	adla_add_task(&queue->scheduled_list, &fixture->tasks[3], &fixture->contexts[0], 4);
	queue->pending_num = 2;
	queue->finished_num = 1;

	KUNIT_EXPECT_EQ(test, adlak_invoke_del_with_invokeid(&fixture->dev, 1, -1), -1);
	KUNIT_EXPECT_EQ(test, queue->pending_num, 1);
	KUNIT_EXPECT_EQ(test, queue->finished_num, 0);
	KUNIT_EXPECT_EQ(test, fixture->contexts[0].invoke_cnt, 1U);
	KUNIT_EXPECT_EQ(test, fixture->contexts[1].invoke_cnt, 1U);
	KUNIT_EXPECT_TRUE(test, fixture->tasks[3].flag & ADLAK_TASK_CANCELED);
	KUNIT_EXPECT_PTR_EQ(test, queue->scheduled_list.next, &fixture->tasks[3].head);
	KUNIT_EXPECT_TRUE(test, list_empty(&queue->finished_list));

	KUNIT_EXPECT_EQ(test, adlak_invoke_del_with_invokeid(&fixture->dev, 2, 999), 0);
	KUNIT_EXPECT_EQ(test, queue->pending_num, 1);
	KUNIT_EXPECT_EQ(test, fixture->contexts[1].invoke_cnt, 1U);
	KUNIT_EXPECT_EQ(test, adlak_invoke_del_with_invokeid(&fixture->dev, 2, 2), 0);
	KUNIT_EXPECT_EQ(test, queue->pending_num, 0);
	KUNIT_EXPECT_EQ(test, fixture->contexts[1].invoke_cnt, 0U);
	KUNIT_EXPECT_TRUE(test, list_empty(&queue->pending_list));
}

static void adla_range_bounds_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, adlak_range_valid(0, 1, 1));
	KUNIT_EXPECT_TRUE(test, adlak_range_valid(INT_MAX, 0, INT_MAX));
	KUNIT_EXPECT_FALSE(test, adlak_range_valid(INT_MAX, 1, INT_MAX));
	KUNIT_EXPECT_FALSE(test, adlak_range_valid(-1, 1, 1));
	KUNIT_EXPECT_FALSE(test, adlak_range_valid(0, -1, 1));
	KUNIT_EXPECT_FALSE(test, adlak_range_valid(0, 0, -1));
	KUNIT_EXPECT_FALSE(test, adlak_range_valid(2, 0, 1));
}

static void adla_fixup_bounds_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, adlak_fixup_valid(124, 31, 16, 128));
	KUNIT_EXPECT_FALSE(test, adlak_fixup_valid(128, 0, 16, 128));
	KUNIT_EXPECT_FALSE(test, adlak_fixup_valid(125, 0, 16, 132));
	KUNIT_EXPECT_FALSE(test, adlak_fixup_valid(-4, 0, 16, 128));
	KUNIT_EXPECT_FALSE(test, adlak_fixup_valid(124, 32, 16, 128));
	KUNIT_EXPECT_FALSE(test, adlak_fixup_valid(124, 0, 0, 128));
}

static struct kunit_case adla_cases[] = {
	KUNIT_CASE(adla_bitmap_pool_test),
	KUNIT_CASE(adla_queued_cancel_test),
	KUNIT_CASE(adla_range_bounds_test),
	KUNIT_CASE(adla_fixup_bounds_test),
	{}
};

static struct kunit_suite adla_suite = {
	.name = "amlogic-adla",
	.test_cases = adla_cases,
};

kunit_test_suite(adla_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ADLA allocation, queued cancellation and bounds tests");
