// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <media/meson-t7-dos.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

#include "aml_vdec.h"
#include "aml_vdec_hw.h"
#include "aml_vdec_platform.h"

#define AML_VDEC_DRV_NAME "aml-vdec-drv"

static bool enable_t7;
module_param(enable_t7, bool, 0400);
MODULE_PARM_DESC(enable_t7, "Enable unverified T7 decode experiment; requires local RESET access");

static bool allow_t7_power;
module_param(allow_t7_power, bool, 0600);
MODULE_PARM_DESC(allow_t7_power, "Allow supervised T7 tests; video open requires power and MDEC tests");

static bool t7_snapshot_only = true;
module_param(t7_snapshot_only, bool, 0400);
MODULE_PARM_DESC(t7_snapshot_only, "Restrict T7 to outer DOS steps 1-8 and abort; forbid internal DOS and decode");

static ssize_t power_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	s32 state = -1, nic3 = -1;
	int ret;

	/* Read-only SMC status, including while video opens are forbidden. */
	ret = aml_vdec_t7_power_state(vdec->dec_hw, &state, &nic3);
	if (ret)
		return ret;
	return sysfs_emit(buf, "VDEC[4]=%d NIC3[39]=%d (0=on 1=off)\n", state, nic3);
}
static DEVICE_ATTR_RO(power_state);

static ssize_t power_test_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	struct aml_vdec_hw *hw = vdec->dec_hw;

	/* Cached result only: reading this file never powers up the decoder. */
	return sysfs_emit(buf, "passed=%u DOS_SW_RESET0=%#x\n",
			 READ_ONCE(hw->power_test_passed), READ_ONCE(hw->power_test_reset));
}

static ssize_t power_test_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	bool run;
	int ret;

	ret = kstrtobool(buf, &run);
	if (ret || !run)
		return -EINVAL;
	mutex_lock(&vdec->dev_mutex);
	if (!READ_ONCE(allow_t7_power))
		ret = -EPERM;
	else if (vdec->open_count || vdec->dec_hw->probe_step || !pm_runtime_suspended(dev))
		ret = -EBUSY;
	else
		ret = aml_vdec_t7_power_test(vdec->dec_hw);
	mutex_unlock(&vdec->dev_mutex);
	return ret ?: count;
}
static DEVICE_ATTR_RW(power_test);

static ssize_t mdec_test_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	struct aml_vdec_hw *hw = vdec->dec_hw;

	return sysfs_emit(buf, "passed=%u MDEC_PIC_DC_CTRL=%#x\n",
			 READ_ONCE(hw->mdec_test_passed), READ_ONCE(hw->mdec_test_value));
}

static ssize_t mdec_test_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	bool run;
	int ret;

	ret = kstrtobool(buf, &run);
	if (ret || !run)
		return -EINVAL;
	mutex_lock(&vdec->dev_mutex);
	if (t7_snapshot_only || !READ_ONCE(allow_t7_power))
		ret = -EPERM;
	else if (vdec->open_count || vdec->dec_hw->probe_step || !pm_runtime_suspended(dev))
		ret = -EBUSY;
	else
		ret = aml_vdec_t7_mdec_test(vdec->dec_hw);
	mutex_unlock(&vdec->dev_mutex);
	return ret ?: count;
}
static DEVICE_ATTR_RW(mdec_test);

static ssize_t register_step_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	struct aml_vdec_hw *hw = vdec->dec_hw;
	ssize_t count;

	/* Cached status only: reading sysfs cannot touch the DOS aperture. */
	mutex_lock(&vdec->dev_mutex);
	count = sysfs_emit(buf, "step=%u value=%#x failed=%u\n",
			   hw->probe_step, hw->probe_value, hw->probe_failed);
	mutex_unlock(&vdec->dev_mutex);
	return count;
}

static ssize_t register_step_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	unsigned int step;
	int ret;

	ret = kstrtouint(buf, 0, &step);
	if (ret)
		return ret;
	mutex_lock(&vdec->dev_mutex);
	if ((t7_snapshot_only && step > 8) || (step && !READ_ONCE(allow_t7_power)))
		ret = -EPERM;
	else if (vdec->open_count || !pm_runtime_suspended(dev))
		ret = -EBUSY;
	else
		ret = aml_vdec_t7_register_step(vdec->dec_hw, step);
	mutex_unlock(&vdec->dev_mutex);
	return ret ?: count;
}
static DEVICE_ATTR_RW(register_step);

static ssize_t faulted_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(vdec->dec_hw->faulted));
}
static DEVICE_ATTR_RO(faulted);

static struct attribute *aml_vdec_attrs[] = {
	&dev_attr_faulted.attr,
	&dev_attr_power_state.attr,
	&dev_attr_power_test.attr,
	&dev_attr_mdec_test.attr,
	&dev_attr_register_step.attr,
	NULL,
};
ATTRIBUTE_GROUPS(aml_vdec);

static int fops_vcodec_open(struct file *file)
{
	struct aml_vdec_dev *dec_dev = video_drvdata(file);
	struct aml_vdec_ctx *ctx = NULL;
	int ret = 0;

	/* udev v4l_id opens a newly registered node. Do not let it power DOS. */
	if (dec_dev->pvdec_data->has_dbus_ctrl &&
	    (t7_snapshot_only || !READ_ONCE(allow_t7_power) ||
	     !READ_ONCE(dec_dev->dec_hw->power_test_passed) ||
	     !READ_ONCE(dec_dev->dec_hw->mdec_test_passed)))
		return -EPERM;

	ctx = kzalloc_obj(*ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_lock(&dec_dev->dev_mutex);
	if (dec_dev->dec_hw->faulted) {
		ret = -EIO;
		goto err_open;
	}
	if (dec_dev->pvdec_data->has_dbus_ctrl) {
		ret = pm_runtime_resume_and_get(&dec_dev->plat_dev->dev);
		if (ret < 0)
			goto err_open;
	}
	ctx->dev = dec_dev;
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh, file);
	mutex_init(&ctx->v4l2_intf_lock);
	init_waitqueue_head(&ctx->queue);
	ctx->int_cond = 0;

	ctx->m2m_ctx = v4l2_m2m_ctx_init(dec_dev->m2m_dev_dec, ctx,
					 &aml_vdec_queue_init);
	if (IS_ERR(ctx->m2m_ctx)) {
		ret = PTR_ERR((__force void *)ctx->m2m_ctx);
		v4l2_err(&dec_dev->v4l2_dev, "Failed to v4l2_m2m_ctx_init() (%d)", ret);
		goto err_m2m_ctx_init;
	}

	ctx->fh.m2m_ctx = ctx->m2m_ctx;
	ret = aml_vdec_ctrls_setup(ctx);
	if (ret) {
		v4l2_err(&dec_dev->v4l2_dev, "Failed to init all ctrls (%d)", ret);
		goto err_ctrls_setup;
	}

	aml_vdec_reset_fmts(ctx);
	dec_dev->open_count++;
	mutex_unlock(&dec_dev->dev_mutex);

	return ret;

err_ctrls_setup:
	v4l2_m2m_ctx_release(ctx->m2m_ctx);
err_m2m_ctx_init:
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	if (dec_dev->pvdec_data->has_dbus_ctrl)
		pm_runtime_put_sync_suspend(&dec_dev->plat_dev->dev);
err_open:
	kfree(ctx);
	mutex_unlock(&dec_dev->dev_mutex);

	return ret;
}

static int fops_vcodec_release(struct file *file)
{
	struct aml_vdec_dev *dec_dev = video_drvdata(file);
	struct aml_vdec_ctx *ctx = fh_to_dec_ctx(file);

	mutex_lock(&dec_dev->dev_mutex);
	v4l2_m2m_ctx_release(ctx->m2m_ctx);
	ctx->m2m_ctx = NULL;
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	dec_dev->open_count--;
	if (!dec_dev->dec_hw->faulted) {
		if (dec_dev->pvdec_data->has_dbus_ctrl &&
		    pm_runtime_put_sync_suspend(&dec_dev->plat_dev->dev) < 0)
			aml_vdec_fault(ctx);
	}
	if (!dec_dev->dec_hw->faulted) {
		kfree(ctx);
	}
	mutex_unlock(&dec_dev->dev_mutex);

	return 0;
}

static const struct v4l2_file_operations aml_vdec_fops = {
	.owner        = THIS_MODULE,
	.open        = fops_vcodec_open,
	.release    = fops_vcodec_release,
	.poll        = v4l2_m2m_fop_poll,
	.unlocked_ioctl    = video_ioctl2,
	.mmap        = v4l2_m2m_fop_mmap,
};

static const struct video_device dec_dev = {
	.name = "aml_dev_drv",
	.fops = &aml_vdec_fops,
	.ioctl_ops = &aml_vdec_ioctl_ops,
	.release = video_device_release,
	.vfl_dir = VFL_DIR_M2M,
	.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING,
};

static const struct media_device_ops aml_m2m_media_ops = {
	.req_validate = vb2_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static int aml_vdec_drv_probe(struct platform_device *pdev)
{
	const struct aml_dev_platform_data *pdata;
	struct aml_vdec_dev *dev;
	struct video_device *vfd_dec;
	struct aml_vdec_hw *hw;
	int ret = 0;

	pdata = of_device_get_match_data(&pdev->dev);
	if (!pdata)
		return -ENODEV;
	if (pdata->has_dbus_ctrl && !enable_t7)
		return -ENODEV;

	if (pdata->has_dbus_ctrl) {
		ret = meson_t7_dos_get(&pdev->dev, true);
		if (ret)
			return ret;
	}
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_lease;
	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto err_lease;
	}

	dev->plat_dev = pdev;
	mutex_init(&dev->dev_mutex);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		goto err_lease;

	vfd_dec = video_device_alloc();
	if (!vfd_dec) {
		v4l2_err(&dev->v4l2_dev, "Failed to allocate video device\n");
		ret = -ENOMEM;
		goto err_device_alloc;
	}
	*vfd_dec = dec_dev;
	vfd_dec->v4l2_dev = &dev->v4l2_dev;
	vfd_dec->lock = &dev->dev_mutex;
	video_set_drvdata(vfd_dec, dev);
	dev->vfd = vfd_dec;
	platform_set_drvdata(pdev, dev);

	hw = devm_kzalloc(&pdev->dev, sizeof(*hw), GFP_KERNEL);
	if (!hw) {
		ret = -ENOMEM;
		goto err_dec_mem_init;
	}
	dev->dec_hw = hw;

	dev->pvdec_data = pdata;
	ret = dev->pvdec_data->req_hw_resource(dev);
	if (ret < 0)
		goto err_dec_mem_init;

	dev->m2m_dev_dec = v4l2_m2m_init(&aml_vdec_m2m_ops);
	if (IS_ERR(dev->m2m_dev_dec)) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem dec device\n");
		ret = PTR_ERR((__force void *)dev->m2m_dev_dec);
		goto err_hw_init;
	}

	/* Runtime PM must be ready before registration exposes an openable node. */
	if (pdata->has_dbus_ctrl) {
		pm_runtime_set_suspended(&pdev->dev);
		pm_runtime_enable(&pdev->dev);
	}
	ret = video_register_device(vfd_dec, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device");
		goto err_vid_dev_register;
	}

	dev->mdev.dev = &pdev->dev;
	strscpy(dev->mdev.model, AML_VDEC_DRV_NAME, sizeof(dev->mdev.model));
	media_device_init(&dev->mdev);
	dev->mdev.ops = &aml_m2m_media_ops;
	dev->v4l2_dev.mdev = &dev->mdev;

	ret = v4l2_m2m_register_media_controller(dev->m2m_dev_dec, vfd_dec,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem media controller\n");
		goto error_m2m_mc_register;
	}

	ret = media_device_register(&dev->mdev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register media device");
		goto err_media_dev_register;
	}
	if (pdata->has_dbus_ctrl) {
		return 0;
	}
	ret = vdec_enable(dev->dec_hw);
	if (ret) {
		media_device_unregister(&dev->mdev);
		goto err_media_dev_register;
	}
	return 0;

err_media_dev_register:
	v4l2_m2m_unregister_media_controller(dev->m2m_dev_dec);
error_m2m_mc_register:
	media_device_cleanup(&dev->mdev);
err_vid_dev_register:
	if (pdata->has_dbus_ctrl)
		pm_runtime_disable(&pdev->dev);
	v4l2_m2m_release(dev->m2m_dev_dec);
err_hw_init:
	dev->pvdec_data->destroy_hw_resource(dev);
	dev->dec_hw = NULL;
err_dec_mem_init:
	if (video_is_registered(vfd_dec))
		video_unregister_device(vfd_dec);
	else
		video_device_release(vfd_dec);
err_device_alloc:
	v4l2_device_unregister(&dev->v4l2_dev);
err_lease:
	if (pdata->has_dbus_ctrl)
		meson_t7_dos_put(true);
	return ret;
}

static void aml_vdec_drv_remove(struct platform_device *pdev)
{
	struct aml_vdec_dev *dev = platform_get_drvdata(pdev);

	if (dev->pvdec_data->has_dbus_ctrl) {
		pm_runtime_disable(&pdev->dev);
		if (dev->dec_hw->faulted)
			return;
	} else {
		vdec_disable(dev->dec_hw);
	}

	if (media_devnode_is_registered(dev->mdev.devnode)) {
		media_device_unregister(&dev->mdev);
		media_device_cleanup(&dev->mdev);
	}

	if (dev->m2m_dev_dec)
		v4l2_m2m_release(dev->m2m_dev_dec);
	if (dev->vfd)
		video_unregister_device(dev->vfd);
	if (dev->dec_hw) {
		dev->pvdec_data->destroy_hw_resource(dev);
		dev->dec_hw = NULL;
	}
	v4l2_device_unregister(&dev->v4l2_dev);
	if (dev->pvdec_data->has_dbus_ctrl)
		meson_t7_dos_put(true);
}

static int aml_vdec_runtime_resume(struct device *dev)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);

	if (vdec->pvdec_data->has_dbus_ctrl &&
	    (t7_snapshot_only || !READ_ONCE(allow_t7_power) ||
	     !READ_ONCE(vdec->dec_hw->power_test_passed) ||
	     !READ_ONCE(vdec->dec_hw->mdec_test_passed)))
		return -EPERM;
	return vdec_enable(vdec->dec_hw);
}

static int aml_vdec_runtime_suspend(struct device *dev)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);

	return vdec_disable(vdec->dec_hw);
}

static const struct dev_pm_ops aml_vdec_pm_ops = {
	RUNTIME_PM_OPS(aml_vdec_runtime_suspend, aml_vdec_runtime_resume, NULL)
};

static const struct of_device_id aml_vdec_match[] = {
	{.compatible = "amlogic,s4-vcodec-dec", .data = &aml_vdec_s4_pdata},
	{.compatible = "amlogic,t7-vcodec-dec", .data = &aml_vdec_t7_pdata},
	{},
};
MODULE_DEVICE_TABLE(of, aml_vdec_match);

static struct platform_driver aml_vcodec_dec_driver = {
	.probe    = aml_vdec_drv_probe,
	.remove    = aml_vdec_drv_remove,
	.driver_managed_pm = true,
	.driver    = {
		.name    = AML_VDEC_DRV_NAME,
		.of_match_table = aml_vdec_match,
		.pm = &aml_vdec_pm_ops,
		.suppress_bind_attrs = true,
		.dev_groups = aml_vdec_groups,
	},
};

module_platform_driver(aml_vcodec_dec_driver);

MODULE_DESCRIPTION("Amlogic V4L2 decoder driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
