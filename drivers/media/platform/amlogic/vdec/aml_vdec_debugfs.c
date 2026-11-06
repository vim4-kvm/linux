// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "aml_vdec.h"
#include "aml_vdec_hw.h"
#include "hevc_regs.h"

struct aml_vdec_debugfs {
	struct aml_vdec_dev *dev;
	struct dentry *directory;
	bool hevc_test_passed;
	u32 hevc_parser_version;
	bool hevc_core_test_passed;
};

static int hevc_power_test(struct aml_vdec_debugfs *diagnostic)
{
	struct aml_vdec_hw *hw = diagnostic->dev->dec_hw;
	u32 value;
	int ret, off_ret;

	if (!hw->hevc.present)
		return -EOPNOTSUPP;
	if (hw->faulted || hw->diag.probe_failed)
		return -EIO;
	if (!hw->diag.mdec_test_passed || hw->vdec1.powered || hw->diag.probe_step)
		return -EBUSY;
	diagnostic->hevc_test_passed = false;
	ret = vdec_enable(hw);
	if (ret)
		return ret;
	ret = aml_hevc_power_on(hw);
	if (ret)
		goto power_off;
	/* No microcode, CPU start or DMA: only the audited reset/status registers. */
	dev_info(hw->dev, "HEVC test: MPSR read begin\n");
	ret = regmap_read(hw->map[DOS_BUS], HEVC_MPSR, &value);
	if (ret)
		goto power_off;
	if (value) {
		ret = -EBUSY;
		goto power_off;
	}
	dev_info(hw->dev, "HEVC test: parser version read begin\n");
	ret = regmap_read(hw->map[DOS_BUS], HEVC_PARSER_VERSION, &value);
	if (!ret) {
		diagnostic->hevc_parser_version = value;
		dev_info(hw->dev, "HEVC test: parser version=%#x\n", value);
	}

power_off:
	off_ret = aml_hevc_power_off(hw);
	if (!hw->faulted)
		off_ret = vdec_disable(hw) ?: off_ret;
	ret = ret ?: off_ret;
	if (ret)
		hw->diag.probe_failed = true;
	else
		diagnostic->hevc_test_passed = true;
	return ret;
}

static int hevc_firmware_test(struct aml_vdec_debugfs *diagnostic)
{
	struct aml_vdec_hw *hw = diagnostic->dev->dec_hw;
	int ret, off_ret;

	if (!hw->hevc.present)
		return -EOPNOTSUPP;
	if (hw->faulted || hw->diag.probe_failed)
		return -EIO;
	if (!diagnostic->hevc_test_passed || hw->vdec1.powered || hw->diag.probe_step)
		return -EBUSY;
	diagnostic->hevc_core_test_passed = false;
	ret = vdec_enable(hw);
	if (ret)
		return ret;
	ret = aml_hevc_prepare_hw(hw);
	/* Even a rejected load can have touched IMEM DMA; always drain it. */
	off_ret = aml_hevc_release_hw(hw);
	if (!hw->faulted)
		off_ret = vdec_disable(hw) ?: off_ret;
	ret = ret ?: off_ret;
	if (ret)
		hw->diag.probe_failed = true;
	else
		diagnostic->hevc_core_test_passed = true;
	return ret;
}

static ssize_t run_hevc_test(struct file *file, const char __user *input, size_t count,
			     int (*run_test)(struct aml_vdec_debugfs *diagnostic))
{
	struct aml_vdec_debugfs *diagnostic = file->private_data;
	struct aml_vdec_dev *dev = diagnostic->dev;
	bool run;
	int ret;

	ret = kstrtobool_from_user(input, count, &run);
	if (ret || !run)
		return ret ?: -EINVAL;
	mutex_lock(&dev->dev_mutex);
	if (!aml_vdec_power_allowed())
		ret = -EPERM;
	else if (dev->open_count || !pm_runtime_suspended(&dev->plat_dev->dev))
		ret = -EBUSY;
	else
		ret = run_test(diagnostic);
	mutex_unlock(&dev->dev_mutex);
	return ret ?: count;
}

static ssize_t hevc_test_read(struct file *file, char __user *output,
			      size_t count, loff_t *position)
{
	struct aml_vdec_debugfs *diagnostic = file->private_data;
	char text[96];
	int length;

	length = scnprintf(text, sizeof(text), "passed=%u parser_version=%#x\n",
			   READ_ONCE(diagnostic->hevc_test_passed),
			   READ_ONCE(diagnostic->hevc_parser_version));
	return simple_read_from_buffer(output, count, position, text, length);
}

static ssize_t hevc_test_write(struct file *file, const char __user *input,
			       size_t count, loff_t *position)
{
	return run_hevc_test(file, input, count, hevc_power_test);
}

static ssize_t hevc_core_test_read(struct file *file, char __user *output,
				   size_t count, loff_t *position)
{
	struct aml_vdec_debugfs *diagnostic = file->private_data;
	struct aml_vdec_hw *hw = diagnostic->dev->dec_hw;
	char text[96];
	int length;

	length = scnprintf(text, sizeof(text), "passed=%u axi_front=%#x axi_back=%#x\n",
			   READ_ONCE(diagnostic->hevc_core_test_passed),
			   READ_ONCE(hw->hevc.axi_front), READ_ONCE(hw->hevc.axi_back));
	return simple_read_from_buffer(output, count, position, text, length);
}

static ssize_t hevc_core_test_write(struct file *file, const char __user *input,
				    size_t count, loff_t *position)
{
	return run_hevc_test(file, input, count, hevc_firmware_test);
}

static const struct file_operations hevc_test_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hevc_test_read,
	.write = hevc_test_write,
	.llseek = default_llseek,
};

static const struct file_operations hevc_core_test_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hevc_core_test_read,
	.write = hevc_core_test_write,
	.llseek = default_llseek,
};

static struct dentry *aml_vdec_debugfs_root;

void aml_vdec_debugfs_root_init(void)
{
	aml_vdec_debugfs_root = debugfs_create_dir("aml-vdec", NULL);
}

void aml_vdec_debugfs_root_remove(void)
{
	debugfs_remove_recursive(aml_vdec_debugfs_root);
	aml_vdec_debugfs_root = NULL;
}

void aml_vdec_debugfs_init(struct aml_vdec_dev *dev)
{
	struct aml_vdec_debugfs *diagnostic;

	if (!IS_ENABLED(CONFIG_DEBUG_FS) || !dev->dec_hw->hevc.present ||
	    IS_ERR_OR_NULL(aml_vdec_debugfs_root))
		return;
	diagnostic = devm_kzalloc(&dev->plat_dev->dev, sizeof(*diagnostic), GFP_KERNEL);
	if (!diagnostic)
		return;
	diagnostic->dev = dev;
	diagnostic->directory = debugfs_create_dir(dev_name(&dev->plat_dev->dev),
						   aml_vdec_debugfs_root);
	if (IS_ERR(diagnostic->directory))
		return;
	dev->debugfs = diagnostic;
	debugfs_create_file("hevc_test", 0644, diagnostic->directory,
			    diagnostic, &hevc_test_fops);
	debugfs_create_file("hevc_core_test", 0644, diagnostic->directory,
			    diagnostic, &hevc_core_test_fops);
}

void aml_vdec_debugfs_remove(struct aml_vdec_dev *dev)
{
	if (!dev->debugfs)
		return;
	debugfs_remove_recursive(dev->debugfs->directory);
	dev->debugfs = NULL;
}
