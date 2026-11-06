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

static ssize_t power_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	s32 state = -1, nic3 = -1;
	int ret;

	/* Read-only SMC status, including while DOS is powered off. */
	ret = aml_vdec_t7_power_state(vdec->dec_hw, &state, &nic3);
	if (ret)
		return ret;
	return sysfs_emit(buf, "VDEC[4]=%d NIC3[39]=%d (0=on 1=off)\n", state, nic3);
}
static DEVICE_ATTR_RO(power_state);

static ssize_t faulted_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(vdec->dec_hw->faulted));
}
static DEVICE_ATTR_RO(faulted);

static struct attribute *aml_vdec_attrs[] = {
	&dev_attr_faulted.attr,
	&dev_attr_power_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(aml_vdec);

static int fops_vcodec_open(struct file *file)
{
	struct aml_vdec_node *node = video_drvdata(file);
	struct aml_vdec_dev *dec_dev = node->dev;
	struct aml_vdec_ctx *ctx = NULL;
	int ret = 0;

	ctx = kzalloc(node->desc->interface->context_size, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_lock(&dec_dev->dev_mutex);
	if (dec_dev->dec_hw->faulted) {
		ret = -EIO;
		goto err_open;
	}
	if (dec_dev->pvdec_data->has_dbus_ctrl) {
		/* Metadata-only opens wake VDEC1, never the HEVC core. */
		ret = pm_runtime_resume_and_get(&dec_dev->plat_dev->dev);
		if (ret < 0)
			goto err_open;
	}
	ctx->dev = dec_dev;
	ctx->node = node->desc;
	ctx->node->interface->context_init(ctx);
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh, file);
	mutex_init(&ctx->v4l2_intf_lock);
	init_waitqueue_head(&ctx->job->queue);
	ctx->job->int_cond = 0;

	ctx->m2m_ctx = v4l2_m2m_ctx_init(dec_dev->m2m_dev_dec, ctx,
					 &aml_vdec_queue_init);
	if (IS_ERR(ctx->m2m_ctx)) {
		ret = PTR_ERR((__force void *)ctx->m2m_ctx);
		v4l2_err(&dec_dev->v4l2_dev, "Failed to v4l2_m2m_ctx_init() (%d)", ret);
		goto err_m2m_ctx_init;
	}

	ctx->fh.m2m_ctx = ctx->m2m_ctx;
	if (ctx->node->buffered_capture) {
		v4l2_m2m_set_dst_buffered(ctx->m2m_ctx, true);
		ctx->m2m_ctx->ignore_cap_streaming = true;
	}
	ret = ctx->node->interface->controls_init(ctx);
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
	struct aml_vdec_dev *dec_dev = fh_to_dec_ctx(file)->dev;
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
	if (!dec_dev->dec_hw->faulted)
		kfree(ctx);
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
	.ioctl_ops = &aml_vdec_stateless_ioctl_ops,
	.release = video_device_release,
	.vfl_dir = VFL_DIR_M2M,
	.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING,
};

static const struct media_device_ops aml_m2m_media_ops = {
	.req_validate = vb2_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static int aml_vdec_register_interface(struct aml_vdec_dev *dev,
				       struct video_device *vfd,
				       struct media_intf_devnode **result)
{
	struct media_intf_devnode *intf;
	struct media_pad *processor, *sink;
	u32 flags = MEDIA_LNK_FL_IMMUTABLE | MEDIA_LNK_FL_ENABLED;

	/* Both video interfaces use the same DMA engines and decoder scheduler. */
	processor = media_entity_remote_pad_unique(&dev->nodes[0].vfd->entity, MEDIA_PAD_FL_SINK);
	if (IS_ERR(processor))
		return PTR_ERR(processor);
	sink = media_entity_remote_pad_unique(processor->entity, MEDIA_PAD_FL_SINK);
	if (IS_ERR(sink))
		return PTR_ERR(sink);
	intf = media_devnode_create(&dev->mdev, MEDIA_INTF_T_V4L_VIDEO, 0,
				    VIDEO_MAJOR, vfd->minor);
	if (!intf)
		return -ENOMEM;
	if (!media_create_intf_link(&dev->nodes[0].vfd->entity, &intf->intf, flags) ||
	    !media_create_intf_link(sink->entity, &intf->intf, flags)) {
		media_devnode_remove(intf);
		return -ENOMEM;
	}
	*result = intf;
	return 0;
}

static int aml_vdec_alloc_node(struct aml_vdec_dev *dev, unsigned int index)
{
	const struct aml_vdec_node_info *info = &dev->pvdec_data->nodes[index];
	struct aml_vdec_node *node = &dev->nodes[index];
	struct video_device *vfd;

	vfd = video_device_alloc();
	if (!vfd)
		return -ENOMEM;
	*vfd = dec_dev;
	strscpy(vfd->name, info->name, sizeof(vfd->name));
	vfd->v4l2_dev = &dev->v4l2_dev;
	vfd->lock = &dev->dev_mutex;
	vfd->ioctl_ops = info->desc->interface->ioctl_ops;
	node->dev = dev;
	node->desc = info->desc;
	node->vfd = vfd;
	video_set_drvdata(vfd, node);
	return 0;
}

static int aml_vdec_register_node(struct aml_vdec_dev *dev, unsigned int index)
{
	struct aml_vdec_node *node = &dev->nodes[index];
	int ret;

	if (!node->vfd) {
		ret = aml_vdec_alloc_node(dev, index);
		if (ret)
			return ret;
	}
	ret = video_register_device(node->vfd, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;
	if (index)
		ret = aml_vdec_register_interface(dev, node->vfd, &node->intf);
	return ret;
}

static void aml_vdec_unregister_nodes(struct aml_vdec_dev *dev, unsigned int first)
{
	struct aml_vdec_node *node;
	unsigned int i = dev->pvdec_data->num_nodes;

	while (i > first) {
		node = &dev->nodes[--i];
		if (node->intf) {
			media_devnode_remove(node->intf);
			node->intf = NULL;
		}
		if (!node->vfd)
			continue;
		if (video_is_registered(node->vfd))
			video_unregister_device(node->vfd);
		else
			video_device_release(node->vfd);
		node->vfd = NULL;
	}
}

static int aml_vdec_drv_probe(struct platform_device *pdev)
{
	const struct aml_dev_platform_data *pdata;
	struct aml_vdec_dev *dev;
	struct video_device *vfd_dec;
	struct aml_vdec_hw *hw;
	unsigned int i;
	int ret = 0;

	pdata = of_device_get_match_data(&pdev->dev);
	if (!pdata)
		return -ENODEV;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->plat_dev = pdev;
	mutex_init(&dev->dev_mutex);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	dev->pvdec_data = pdata;
	dev->nodes = devm_kcalloc(&pdev->dev, pdata->num_nodes,
				  sizeof(*dev->nodes), GFP_KERNEL);
	if (!dev->nodes) {
		ret = -ENOMEM;
		goto err_device_alloc;
	}
	ret = aml_vdec_alloc_node(dev, 0);
	if (ret)
		goto err_device_alloc;
	vfd_dec = dev->nodes[0].vfd;
	platform_set_drvdata(pdev, dev);

	hw = devm_kzalloc(&pdev->dev, sizeof(*hw), GFP_KERNEL);
	if (!hw) {
		ret = -ENOMEM;
		goto err_dec_mem_init;
	}
	dev->dec_hw = hw;

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
	ret = aml_vdec_register_node(dev, 0);
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
	for (i = 1; i < pdata->num_nodes; i++) {
		if (!aml_vdec_core_present(hw, pdata->nodes[i].core))
			continue;
		ret = aml_vdec_register_node(dev, i);
		if (ret) {
			aml_vdec_unregister_nodes(dev, 1);
			media_device_unregister(&dev->mdev);
			goto err_media_dev_register;
		}
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
	aml_vdec_unregister_nodes(dev, 0);
err_device_alloc:
	v4l2_device_unregister(&dev->v4l2_dev);
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

	aml_vdec_unregister_nodes(dev, 1);
	if (media_devnode_is_registered(dev->mdev.devnode)) {
		media_device_unregister(&dev->mdev);
		media_device_cleanup(&dev->mdev);
	}

	if (dev->m2m_dev_dec)
		v4l2_m2m_release(dev->m2m_dev_dec);
	aml_vdec_unregister_nodes(dev, 0);
	if (dev->dec_hw) {
		dev->pvdec_data->destroy_hw_resource(dev);
		dev->dec_hw = NULL;
	}
	v4l2_device_unregister(&dev->v4l2_dev);
}

static int aml_vdec_runtime_resume(struct device *dev)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	int ret;

	if (vdec->pvdec_data->has_dbus_ctrl) {
		ret = meson_t7_dos_get(dev, true);
		if (ret)
			return ret;
	}
	ret = vdec_enable(vdec->dec_hw);
	if (ret && vdec->pvdec_data->has_dbus_ctrl)
		meson_t7_dos_put(true);
	return ret;
}

static int aml_vdec_runtime_suspend(struct device *dev)
{
	struct aml_vdec_dev *vdec = dev_get_drvdata(dev);
	int ret;

	ret = aml_hevc_release_hw(vdec->dec_hw);
	if (ret)
		return ret;
	ret = vdec_disable(vdec->dec_hw);
	if (!ret && vdec->pvdec_data->has_dbus_ctrl)
		meson_t7_dos_put(true);
	return ret;
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
