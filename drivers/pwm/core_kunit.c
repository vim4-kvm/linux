// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/completion.h>
#include <linux/debug_locks.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/pwm.h>
#include <linux/sched.h>
#include <linux/sched/task.h>

struct pwm_test_clock {
	struct clk_hw hw;
	struct pwm_device *pwm;
	unsigned int calls;
	struct completion entered;
	struct completion release;
};

struct pwm_test_chip {
	struct clk *input;
	struct completion *entered;
};

struct pwm_test_contention {
	struct pwm_test_clock clock;
	struct clk_init_data init;
	struct device *dev;
	struct pwm_device *pwm;
	struct task_struct *clock_task;
	struct task_struct *pwm_task;
	struct completion pwm_entered;
	int clock_ret;
	int pwm_ret;
};

static int pwm_test_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			  const struct pwm_state *state)
{
	return 0;
}

static int pwm_test_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			      struct pwm_state *state)
{
	struct pwm_test_chip *data = pwmchip_get_drvdata(chip);

	/* The regulator path takes the controller mutex before the CCF lock. */
	if (data->entered)
		complete(data->entered);
	state->period = NSEC_PER_SEC / clk_get_rate(data->input);
	state->duty_cycle = state->period / 2;
	return 0;
}

static const struct pwm_ops pwm_test_ops = {
	.apply = pwm_test_apply,
	.get_state = pwm_test_get_state,
};

static int pwm_test_get_duty_cycle(struct clk_hw *hw, struct clk_duty *duty)
{
	struct pwm_test_clock *clock = container_of(hw, struct pwm_test_clock, hw);
	struct pwm_state state = {};
	int ret;

	/* A clock provider enters PWM with the CCF prepare lock already held. */
	clock->calls++;
	if (!clock->pwm) {
		complete(&clock->entered);
		wait_for_completion(&clock->release);
		duty->num = 1;
		duty->den = 2;
		return 0;
	}
	ret = pwm_get_state_hw(clock->pwm, &state);
	if (ret)
		return ret;
	duty->num = state.duty_cycle;
	duty->den = state.period;
	return 0;
}

static const struct clk_ops pwm_test_clk_ops = {
	.get_duty_cycle = pwm_test_get_duty_cycle,
};

static void pwm_test_put_chip(void *data)
{
	pwmchip_put(data);
}

static void pwm_test_remove_chip(void *data)
{
	pwmchip_remove(data);
}

static void pwm_test_unregister_input(void *data)
{
	clk_unregister_fixed_rate(data);
}

static void pwm_test_unregister_clock(void *data)
{
	clk_hw_unregister(data);
}

static int pwm_test_hold_clock(void *data)
{
	struct pwm_test_contention *ctx = data;

	ctx->clock_ret = clk_hw_register(ctx->dev, &ctx->clock.hw);
	return 0;
}

static int pwm_test_read_controller(void *data)
{
	struct pwm_test_contention *ctx = data;
	struct pwm_state state = {};

	ctx->pwm_ret = pwm_get_state_hw(ctx->pwm, &state);
	return 0;
}

static void pwm_test_stop_contention(void *data)
{
	struct pwm_test_contention *ctx = data;

	complete_all(&ctx->clock.release);
	if (!IS_ERR_OR_NULL(ctx->clock_task)) {
		kthread_stop(ctx->clock_task);
		put_task_struct(ctx->clock_task);
		ctx->clock_task = NULL;
		if (!ctx->clock_ret)
			clk_hw_unregister(&ctx->clock.hw);
	}
	if (!IS_ERR_OR_NULL(ctx->pwm_task)) {
		kthread_stop(ctx->pwm_task);
		put_task_struct(ctx->pwm_task);
		ctx->pwm_task = NULL;
	}
}

static void pwm_separate_controller_clock_order_test(struct kunit *test)
{
	struct clk_init_data init = {
		.name = "pwm-kunit-output",
		.ops = &pwm_test_clk_ops,
	};
	struct pwm_chip *chips[2];
	struct pwm_test_clock *clock;
	struct pwm_test_contention *ctx;
	struct device *dev;
	struct clk *input;
	int i, ret;

	KUNIT_ASSERT_TRUE(test, debug_locks);
	dev = kunit_device_register(test, "pwm-clock-order");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	input = clk_register_fixed_rate(dev, "pwm-kunit-input", NULL, 0, 24000000);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, input);
	ret = kunit_add_action_or_reset(test, pwm_test_unregister_input, input);
	KUNIT_ASSERT_EQ(test, ret, 0);

	for (i = 0; i < ARRAY_SIZE(chips); i++) {
		struct pwm_test_chip *chip_input;

		chips[i] = pwmchip_alloc(dev, 1, sizeof(*chip_input));
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, chips[i]);
		ret = kunit_add_action_or_reset(test, pwm_test_put_chip, chips[i]);
		KUNIT_ASSERT_EQ(test, ret, 0);
		chips[i]->ops = &pwm_test_ops;
		chip_input = pwmchip_get_drvdata(chips[i]);
		chip_input->input = input;
		ret = pwmchip_add(chips[i]);
		KUNIT_ASSERT_EQ(test, ret, 0);
		ret = kunit_add_action_or_reset(test, pwm_test_remove_chip, chips[i]);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->dev = dev;
	ctx->pwm = &chips[0]->pwms[0];
	ctx->clock_ret = -EINPROGRESS;
	ctx->init.name = "pwm-kunit-contention";
	ctx->init.ops = &pwm_test_clk_ops;
	ctx->clock.hw.init = &ctx->init;
	init_completion(&ctx->clock.entered);
	init_completion(&ctx->clock.release);
	init_completion(&ctx->pwm_entered);
	((struct pwm_test_chip *)pwmchip_get_drvdata(chips[0]))->entered =
		&ctx->pwm_entered;
	ret = kunit_add_action_or_reset(test, pwm_test_stop_contention, ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ctx->clock_task = kthread_create(pwm_test_hold_clock, ctx, "pwm-test-clk");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->clock_task);
	get_task_struct(ctx->clock_task);
	wake_up_process(ctx->clock_task);
	wait_for_completion(&ctx->clock.entered);
	ctx->pwm_task = kthread_create(pwm_test_read_controller, ctx, "pwm-test-read");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->pwm_task);
	get_task_struct(ctx->pwm_task);
	wake_up_process(ctx->pwm_task);
	wait_for_completion(&ctx->pwm_entered);
	/* CCF tries its mutex first; force the blocking path to record this edge. */
	while (READ_ONCE(ctx->pwm_task->__state) != TASK_UNINTERRUPTIBLE)
		cond_resched();
	pwm_test_stop_contention(ctx);
	KUNIT_ASSERT_EQ(test, ctx->clock_ret, 0);
	KUNIT_ASSERT_EQ(test, ctx->pwm_ret, 0);
	clock = kunit_kzalloc(test, sizeof(*clock), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clock);
	clock->pwm = &chips[1]->pwms[0];
	clock->hw.init = &init;
	/* Registration queries duty through the actual CCF provider callback. */
	ret = clk_hw_register(dev, &clock->hw);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, pwm_test_unregister_clock, &clock->hw);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, debug_locks);
	KUNIT_EXPECT_GT(test, clock->calls, 0U);
}

static struct kunit_case pwm_core_cases[] = {
	KUNIT_CASE(pwm_separate_controller_clock_order_test),
	{}
};

static struct kunit_suite pwm_core_suite = {
	.name = "pwm-clock-locking",
	.test_cases = pwm_core_cases,
};

kunit_test_suite(pwm_core_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PWM controller clock lock dependency tests");
