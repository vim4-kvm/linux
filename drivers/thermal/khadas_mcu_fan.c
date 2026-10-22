// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Khadas MCU Controlled FAN driver
 *
 * Copyright (C) 2020 BayLibre SAS
 * Author(s): Neil Armstrong <narmstrong@baylibre.com>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/khadas-mcu.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <linux/regulator/consumer.h>
#include <linux/limits.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>

static bool quiet_auto = true;
module_param(quiet_auto, bool, 0444);
MODULE_PARM_DESC(quiet_auto, "Use the VIM4 quiet automatic fan curve instead of thermal cooling states");

static const char * const khadas_vim4_fan_sensors[] = {
	"a73-thermal", "a53-thermal", "gpu-thermal",
	"nna-thermal", "vpu-thermal", "hevc-thermal",
};

struct khadas_mcu_fan_ctx {
	struct khadas_mcu *mcu;
	unsigned int fan_reg;
	unsigned int level;
	unsigned int max_level;
	struct thermal_cooling_device *cdev;
	struct regulator *power;
	struct device *dev;
	struct delayed_work quiet_work;
	bool quiet;
	bool sensor_fault;
	bool quiet_started;
	bool quiet_sampled;
	int temperature;
	struct mutex curve_lock;
	unsigned int trigger_low;
	unsigned int trigger_mid;
	unsigned int trigger_high;
};

static int khadas_vim4_fan_lerp(int temp, int low, int high,
			       int low_speed, int high_speed)
{
	return low_speed + (temp - low) * (high_speed - low_speed) /
		(high - low);
}

static int khadas_vim4_fan_target_curve(int temp, bool sensor_fault,
				      int low, int mid, int high)
{
	int target;
	int start = low - 15000;
	int daily = low - 5000;

	if (temp >= 100000 || temp >= high)
		target = 100;
	else if (temp < start)
		target = 0;
	else if (temp < daily)
		target = khadas_vim4_fan_lerp(temp, start, daily, 5, 25);
	else if (temp < low)
		target = khadas_vim4_fan_lerp(temp, daily, low, 25, 35);
	else if (temp < mid)
		target = khadas_vim4_fan_lerp(temp, low, mid, 35, 70);
	else
		target = khadas_vim4_fan_lerp(temp, mid, high, 70, 100);

	/* Unknown sensors must not override another sensor's valid hot reading. */
	return sensor_fault ? max(target, 15) : target;
}

static int khadas_vim4_fan_target(int temp, bool sensor_fault)
{
	return khadas_vim4_fan_target_curve(temp, sensor_fault, 60000, 80000, 100000);
}

static unsigned int khadas_vim4_fan_next(unsigned int current_speed,
				       unsigned int target)
{
	/* Stop/start and emergency cooling bypass the normal 5% ramp. */
	if (!target || target == 100 || !current_speed)
		return target;
	if (target > current_speed + 5)
		return current_speed + 5;
	if (current_speed > target + 5)
		return current_speed - 5;
	return target;
}

static int khadas_mcu_fan_set_level(struct khadas_mcu_fan_ctx *ctx,
				    unsigned int level)
{
	int ret;

	ret = regmap_write(ctx->mcu->regmap, ctx->fan_reg, level);
	if (ret)
		return ret;

	WRITE_ONCE(ctx->level, level);

	return 0;
}

static void khadas_vim4_fan_work(struct work_struct *work)
{
	struct khadas_mcu_fan_ctx *ctx = container_of(to_delayed_work(work),
					struct khadas_mcu_fan_ctx, quiet_work);
	int hottest = INT_MIN;
	bool fault = false;
	bool read_fault = false;
	bool was_fault = ctx->sensor_fault;
	unsigned int target;
	int i, ret;

	/* T7's built-in sensors cover CPU and media workloads independently. */
	for (i = 0; i < ARRAY_SIZE(khadas_vim4_fan_sensors); i++) {
		struct thermal_zone_device *zone;
		int temperature;

		zone = thermal_zone_get_zone_by_name(khadas_vim4_fan_sensors[i]);
		if (IS_ERR(zone)) {
			fault = true;
			read_fault |= PTR_ERR(zone) != -ENODEV;
			continue;
		}
		if (thermal_zone_get_temp(zone, &temperature)) {
			fault = true;
			read_fault = true;
			continue;
		}
		hottest = max(hottest, temperature);
	}
	WRITE_ONCE(ctx->temperature, hottest);
	WRITE_ONCE(ctx->sensor_fault, fault);
	mutex_lock(&ctx->curve_lock);
	target = khadas_vim4_fan_target_curve(hottest, fault,
		ctx->trigger_low * 1000, ctx->trigger_mid * 1000, ctx->trigger_high * 1000);
	/* Fault handling and its first valid sample bypass the normal ramp. */
	if (ctx->quiet_started && !fault && !was_fault)
		target = khadas_vim4_fan_next(ctx->level, target);
	ret = khadas_mcu_fan_set_level(ctx, target);
	if (ret)
		dev_err_ratelimited(ctx->dev, "Failed to set fan speed: %d\n", ret);
	else if (!fault)
		ctx->quiet_started = true;
	mutex_unlock(&ctx->curve_lock);
	if (fault) {
		/* Only an unregistered zone on the first sample is expected. */
		if (!ctx->quiet_sampled && !read_fault)
			dev_info(ctx->dev,
				 "Waiting for fan thermal zones; requesting %u%% output\n", target);
		else
			dev_warn_ratelimited(ctx->dev,
					     "Fan temperature unavailable; requesting %u%% output\n",
					     target);
	}
	ctx->quiet_sampled = true;

	/* Thermal zones may register after the first boot-time sample. */
	schedule_delayed_work(&ctx->quiet_work,
			      msecs_to_jiffies(ret || fault ? 1000 : 30000));
}

static void khadas_vim4_fan_stop(void *data)
{
	struct khadas_mcu_fan_ctx *ctx = data;

	cancel_delayed_work_sync(&ctx->quiet_work);
	/* Leave cooling available if this driver is detached while running. */
	khadas_mcu_fan_set_level(ctx, ctx->max_level);
}

static ssize_t speed_percent_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(ctx->level));
}
static DEVICE_ATTR_RO(speed_percent);

static ssize_t sensor_fault_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(ctx->sensor_fault));
}
static DEVICE_ATTR_RO(sensor_fault);

static ssize_t temperature_millic_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);

	if (READ_ONCE(ctx->sensor_fault))
		return -ENODATA;
	return sysfs_emit(buf, "%d\n", READ_ONCE(ctx->temperature));
}
static DEVICE_ATTR_RO(temperature_millic);

static ssize_t trigger_temperatures_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);
	ssize_t size;

	mutex_lock(&ctx->curve_lock);
	size = sysfs_emit(buf, "%u %u %u\n", ctx->trigger_low,
			  ctx->trigger_mid, ctx->trigger_high);
	mutex_unlock(&ctx->curve_lock);
	return size;
}

static ssize_t trigger_temperatures_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned int low, mid, high;
	char extra;

	if (sscanf(buf, "%u %u %u %c", &low, &mid, &high, &extra) != 3 ||
	    low < 35 || low > 80 || mid < low + 5 || mid > 95 ||
	    high < mid + 5 || high > 100)
		return -EINVAL;
	mutex_lock(&ctx->curve_lock);
	ctx->trigger_low = low;
	ctx->trigger_mid = mid;
	ctx->trigger_high = high;
	ctx->quiet_started = false;
	mutex_unlock(&ctx->curve_lock);
	mod_delayed_work(system_wq, &ctx->quiet_work, 0);
	return count;
}
static DEVICE_ATTR_RW(trigger_temperatures);

static struct attribute *khadas_vim4_fan_attrs[] = {
	&dev_attr_speed_percent.attr,
	&dev_attr_sensor_fault.attr,
	&dev_attr_temperature_millic.attr,
	&dev_attr_trigger_temperatures.attr,
	NULL,
};
static const struct attribute_group khadas_vim4_fan_group = {
	.attrs = khadas_vim4_fan_attrs,
};

static int khadas_mcu_fan_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct khadas_mcu_fan_ctx *ctx = cdev->devdata;

	*state = ctx->max_level;

	return 0;
}

static int khadas_mcu_fan_get_cur_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct khadas_mcu_fan_ctx *ctx = cdev->devdata;

	*state = ctx->level;

	return 0;
}

static int
khadas_mcu_fan_set_cur_state(struct thermal_cooling_device *cdev,
			     unsigned long state)
{
	struct khadas_mcu_fan_ctx *ctx = cdev->devdata;

	if (state > ctx->max_level)
		return -EINVAL;

	if (state == ctx->level)
		return 0;

	return khadas_mcu_fan_set_level(ctx, state);
}

static const struct thermal_cooling_device_ops khadas_mcu_fan_cooling_ops = {
	.get_max_state = khadas_mcu_fan_get_max_state,
	.get_cur_state = khadas_mcu_fan_get_cur_state,
	.set_cur_state = khadas_mcu_fan_set_cur_state,
};

static void khadas_mcu_fan_disable_power(void *data)
{
	regulator_disable(data);
}

static int khadas_mcu_fan_probe(struct platform_device *pdev)
{
	struct khadas_mcu *mcu = dev_get_drvdata(pdev->dev.parent);
	struct thermal_cooling_device *cdev;
	struct device *dev = &pdev->dev;
	struct khadas_mcu_fan_ctx *ctx;
	int ret;
	const struct khadas_mcu_fan_pdata *pdata = dev_get_platdata(&pdev->dev);

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->mcu = mcu;
	ctx->fan_reg   = pdata->fan_reg;
	ctx->max_level = pdata->max_level;
	ctx->dev = dev;
	mutex_init(&ctx->curve_lock);
	ctx->trigger_low = 60;
	ctx->trigger_mid = 80;
	ctx->trigger_high = 100;
	ctx->quiet = quiet_auto && pdata->fan_reg == KHADAS_MCU_VIM4_FAN_CTRL_REG &&
		pdata->max_level == 100;

	ctx->power = devm_regulator_get_optional(dev->parent, "fan");
	if (IS_ERR(ctx->power)) {
		if (PTR_ERR(ctx->power) == -ENODEV)
			ctx->power = NULL;
		else
			return PTR_ERR(ctx->power);
	}

	if (ctx->power) {
		ret = regulator_enable(ctx->power);
		if (ret) {
			dev_err(dev, "Failed to enable fan power supply: %d\n", ret);
			return ret;
		}

		ret = devm_add_action_or_reset(dev, khadas_mcu_fan_disable_power,
					       ctx->power);
		if (ret)
			return ret;
	}

	platform_set_drvdata(pdev, ctx);

	if (ctx->quiet) {
		/* The quiet worker is the sole owner; do not bind step_wise too. */
		INIT_DELAYED_WORK(&ctx->quiet_work, khadas_vim4_fan_work);
		ctx->sensor_fault = true;
		ret = khadas_mcu_fan_set_level(ctx,
					    khadas_vim4_fan_target(INT_MIN, true));
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(dev, khadas_vim4_fan_stop, ctx);
		if (ret)
			return ret;
		ret = devm_device_add_group(dev, &khadas_vim4_fan_group);
		if (ret)
			return ret;
		schedule_delayed_work(&ctx->quiet_work, 0);
		return 0;
	}

	cdev = devm_thermal_of_child_cooling_device_register(dev->parent,
							     dev->parent->of_node,
							     "khadas-mcu-fan", ctx,
							     &khadas_mcu_fan_cooling_ops);
	if (IS_ERR(cdev)) {
		ret = PTR_ERR(cdev);
		dev_err(dev, "Failed to register khadas-mcu-fan as cooling device: %d\n",
			ret);
		return ret;
	}
	ctx->cdev = cdev;

	return 0;
}

static void khadas_mcu_fan_shutdown(struct platform_device *pdev)
{
	struct khadas_mcu_fan_ctx *ctx = platform_get_drvdata(pdev);

	if (ctx->quiet)
		cancel_delayed_work_sync(&ctx->quiet_work);
	khadas_mcu_fan_set_level(ctx, 0);
}

#ifdef CONFIG_PM_SLEEP
static int khadas_mcu_fan_suspend(struct device *dev)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned int level_save;
	int ret;

	if (ctx->quiet)
		cancel_delayed_work_sync(&ctx->quiet_work);
	level_save = ctx->level;
	ret = khadas_mcu_fan_set_level(ctx, 0);
	if (ret)
		goto restart_work;

	ctx->level = level_save;

	if (ctx->power) {
		ret = regulator_disable(ctx->power);
		if (ret) {
			khadas_mcu_fan_set_level(ctx, level_save);
			goto restart_work;
		}
	}

	return 0;

restart_work:
	if (ctx->quiet)
		schedule_delayed_work(&ctx->quiet_work, 0);
	return ret;
}

static int khadas_mcu_fan_resume(struct device *dev)
{
	struct khadas_mcu_fan_ctx *ctx = dev_get_drvdata(dev);
	int ret;

	if (ctx->power) {
		ret = regulator_enable(ctx->power);
		if (ret)
			return ret;
	}

	if (ctx->quiet) {
		schedule_delayed_work(&ctx->quiet_work, 0);
		return 0;
	}
	return khadas_mcu_fan_set_level(ctx, ctx->level);
}
#endif

static SIMPLE_DEV_PM_OPS(khadas_mcu_fan_pm, khadas_mcu_fan_suspend,
			 khadas_mcu_fan_resume);

static const struct platform_device_id khadas_mcu_fan_id_table[] = {
	{ .name = "khadas-mcu-fan-ctrl", },
	{},
};
MODULE_DEVICE_TABLE(platform, khadas_mcu_fan_id_table);

static struct platform_driver khadas_mcu_fan_driver = {
	.probe		= khadas_mcu_fan_probe,
	.shutdown	= khadas_mcu_fan_shutdown,
	.driver	= {
		.name		= "khadas-mcu-fan-ctrl",
		.pm		= &khadas_mcu_fan_pm,
	},
	.id_table	= khadas_mcu_fan_id_table,
};

module_platform_driver(khadas_mcu_fan_driver);

MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_DESCRIPTION("Khadas MCU FAN driver");
MODULE_LICENSE("GPL");
