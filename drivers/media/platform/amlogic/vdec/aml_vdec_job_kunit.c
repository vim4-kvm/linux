// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "aml_vdec.h"
#include "aml_vdec_hw.h"
#include "aml_vdec_platform.h"

struct job_fixture {
	struct aml_vdec_ctx ctx;
	struct aml_vdec_job job;
	struct aml_vdec_dev dev;
	struct aml_vdec_hw hw;
	struct aml_dev_platform_data platform;
	struct v4l2_m2m_ctx m2m;
	struct aml_vdec_node_desc node;
	struct aml_vdec_interface interface;
	/* Models the interface lock held while buffered output is processed. */
	struct mutex lock;
	int prepare_error, acquire_error, stop_error;
	unsigned int begins, stops, releases;
	bool owner;
};

static struct job_fixture *job_fixture(struct aml_vdec_ctx *ctx)
{
	return container_of(ctx, struct job_fixture, ctx);
}

static void job_lock(struct aml_vdec_ctx *ctx)
{
	mutex_lock(&job_fixture(ctx)->lock);
}

static void job_unlock(struct aml_vdec_ctx *ctx)
{
	mutex_unlock(&job_fixture(ctx)->lock);
}

static int job_prepare(struct aml_vdec_ctx *ctx)
{
	return job_fixture(ctx)->prepare_error;
}

static int job_begin(struct aml_vdec_ctx *ctx)
{
	struct job_fixture *f = job_fixture(ctx);

	f->begins++;
	f->owner = true;
	return f->acquire_error;
}

static int job_stop(struct aml_vdec_ctx *ctx)
{
	struct job_fixture *f = job_fixture(ctx);

	if (f->hw.faulted)
		return -EIO;
	f->stops++;
	if (f->stop_error)
		f->hw.faulted = true;
	return f->stop_error;
}

static void job_release(struct aml_vdec_ctx *ctx)
{
	struct job_fixture *f = job_fixture(ctx);

	f->releases++;
	f->owner = false;
}

static const struct aml_vdec_job_ops test_job_ops = {
	.acquire = job_begin,
	.stop = job_stop,
	.release = job_release,
};

static int job_test_init(struct kunit *test)
{
	struct job_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return -ENOMEM;
	mutex_init(&f->lock);
	f->ctx.job = &f->job;
	f->ctx.dev = &f->dev;
	f->ctx.node = &f->node;
	f->ctx.m2m_ctx = &f->m2m;
	f->dev.dec_hw = &f->hw;
	f->dev.pvdec_data = &f->platform;
	f->node.interface = &f->interface;
	f->node.job_ops = &test_job_ops;
	f->interface.lock_job = job_lock;
	f->interface.unlock_job = job_unlock;
	f->interface.prepare_job = job_prepare;
	test->priv = f;
	return 0;
}

static void stopped_job_retains_owner_until_output_is_processed(struct kunit *test)
{
	struct job_fixture *f = test->priv;

	KUNIT_ASSERT_EQ(test, aml_vdec_job_acquire(&f->ctx), 0);
	KUNIT_EXPECT_TRUE(test, f->owner);
	KUNIT_EXPECT_TRUE(test, mutex_is_locked(&f->lock));
	KUNIT_EXPECT_EQ(test, aml_vdec_job_stop(&f->ctx), 0);
	/* Stopping permits output processing, but does not release its lifetime. */
	KUNIT_EXPECT_TRUE(test, f->owner);
	KUNIT_EXPECT_TRUE(test, mutex_is_locked(&f->lock));
	aml_vdec_job_release(&f->ctx);
	KUNIT_EXPECT_FALSE(test, f->owner);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&f->lock));
	KUNIT_EXPECT_EQ(test, f->stops, 1U);
}

static void preparation_failure_does_not_touch_hardware(struct kunit *test)
{
	struct job_fixture *f = test->priv;

	f->prepare_error = -ENOMEM;
	KUNIT_EXPECT_EQ(test, aml_vdec_job_acquire(&f->ctx), -ENOMEM);
	aml_vdec_job_release(&f->ctx);
	KUNIT_EXPECT_EQ(test, f->begins, 0U);
	KUNIT_EXPECT_EQ(test, f->stops, 0U);
	KUNIT_EXPECT_FALSE(test, f->job.acquired);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&f->lock));
}

static void partial_hardware_acquire_is_stopped_before_release(struct kunit *test)
{
	struct job_fixture *f = test->priv;

	f->acquire_error = -EIO;
	KUNIT_EXPECT_EQ(test, aml_vdec_job_acquire(&f->ctx), -EIO);
	aml_vdec_job_release(&f->ctx);
	KUNIT_EXPECT_EQ(test, f->begins, 1U);
	KUNIT_EXPECT_EQ(test, f->stops, 1U);
	KUNIT_EXPECT_FALSE(test, f->owner);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&f->lock));
}

static void failed_stop_keeps_the_owner_quarantined(struct kunit *test)
{
	struct job_fixture *f = test->priv;

	KUNIT_ASSERT_EQ(test, aml_vdec_job_acquire(&f->ctx), 0);
	f->stop_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, aml_vdec_job_stop(&f->ctx), -ETIMEDOUT);
	aml_vdec_job_release(&f->ctx);
	KUNIT_EXPECT_TRUE(test, f->owner);
	KUNIT_EXPECT_TRUE(test, f->job.acquired);
	KUNIT_EXPECT_EQ(test, f->stops, 1U);
	KUNIT_EXPECT_EQ(test, f->releases, 0U);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&f->lock));
}

static struct kunit_case job_test_cases[] = {
	KUNIT_CASE(stopped_job_retains_owner_until_output_is_processed),
	KUNIT_CASE(preparation_failure_does_not_touch_hardware),
	KUNIT_CASE(partial_hardware_acquire_is_stopped_before_release),
	KUNIT_CASE(failed_stop_keeps_the_owner_quarantined),
	{}
};

static struct kunit_suite job_test_suite = {
	.name = "amlogic-vdec-job",
	.init = job_test_init,
	.test_cases = job_test_cases,
};

kunit_test_suite(job_test_suite);
