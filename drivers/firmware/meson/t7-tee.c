// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

static DEFINE_MUTEX(boot_lock);
static bool completed;

/* T7 BL31 HDCPTX_IOOPR commands, shared with the HDMI transmitter. */
#define T7_HDCPTX_IOOPR		0x820000ab
#define T7_HDCP14_KEY_READY	1
#define T7_HDCP22_KEY_READY	4

static int hdcp_key_ready_get(void *data, u64 *value)
{
	struct arm_smccc_res result;
	u32 ready;

	/* Query only: loading keys and starting authentication belong to HDCP. */
	arm_smccc_smc(T7_HDCPTX_IOOPR, (unsigned long)data,
		      0, 0, 0, 0, 0, 0, &result);
	ready = (u32)result.a0;
	if (ready == (u32)SMCCC_RET_NOT_SUPPORTED)
		return -EOPNOTSUPP;
	if (ready > 1)
		return -EIO;
	*value = ready;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(hdcp_key_ready_fops, hdcp_key_ready_get, NULL, "%llu\n");

static void tee_debugfs_remove(void *directory)
{
	debugfs_remove_recursive(directory);
}

static int tee_debugfs_init(struct device *dev)
{
	struct dentry *directory;

	directory = debugfs_create_dir("meson_t7_tee", NULL);
	if (IS_ERR(directory))
		return 0;
	debugfs_create_file("hdcp14_key_ready", 0400, directory,
			    (void *)T7_HDCP14_KEY_READY, &hdcp_key_ready_fops);
	debugfs_create_file("hdcp22_key_ready", 0400, directory,
			    (void *)T7_HDCP22_KEY_READY, &hdcp_key_ready_fops);
	return devm_add_action_or_reset(dev, tee_debugfs_remove, directory);
}

static u32 tee_call(u32 function)
{
	struct arm_smccc_res result;

	arm_smccc_smc(ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32,
				       ARM_SMCCC_OWNER_TRUSTED_OS, function),
		      0, 0, 0, 0, 0, 0, 0, &result);
	return result.a0;
}

static ssize_t sys_boot_complete_show(const struct class *class,
				     const struct class_attribute *attr, char *buf)
{
	u32 result = tee_call(0xe061);

	if (result > 1)
		return -EIO;
	return sysfs_emit(buf, "%u\n", result);
}

static ssize_t sys_boot_complete_store(const struct class *class,
				      const struct class_attribute *attr,
				      const char *buf, size_t count)
{
	bool value;
	int ret = 0;

	if (kstrtobool(buf, &value) || !value)
		return -EINVAL;
	mutex_lock(&boot_lock);
	if (!completed) {
		if (tee_call(0xe060))
			ret = -EIO;
		else
			completed = true;
	}
	mutex_unlock(&boot_lock);
	return ret ? ret : count;
}
static CLASS_ATTR_RW(sys_boot_complete);

static struct class tee_class = {
	.name = "tee_info",
};

static int tee_probe(struct platform_device *pdev)
{
	int ret = class_register(&tee_class);

	if (ret)
		return ret;
	ret = class_create_file(&tee_class, &class_attr_sys_boot_complete);
	if (!ret)
		ret = tee_debugfs_init(&pdev->dev);
	if (ret)
		class_unregister(&tee_class);
	return ret;
}

static void tee_remove(struct platform_device *pdev)
{
	class_remove_file(&tee_class, &class_attr_sys_boot_complete);
	class_unregister(&tee_class);
}

static const struct of_device_id tee_match[] = {
	{ .compatible = "amlogic,t7-tee-control" },
	{}
};
MODULE_DEVICE_TABLE(of, tee_match);

static struct platform_driver tee_driver = {
	.probe = tee_probe,
	.remove = tee_remove,
	.driver = {
		.name = "t7-tee-control",
		.of_match_table = tee_match,
	},
};
module_platform_driver(tee_driver);
MODULE_DESCRIPTION("Amlogic T7 secure boot completion interface");
MODULE_LICENSE("GPL");
