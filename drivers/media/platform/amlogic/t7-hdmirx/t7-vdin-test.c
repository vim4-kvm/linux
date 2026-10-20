// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019 Amlogic, Inc. All rights reserved. */
/* Exclusive T7 VDIN0 raw snapshot experiment; DRM must remain disabled. */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>

#include "t7-hdmirx.h"

/* VCBUS word indices; the accessors alone convert them to byte offsets. */
#define VDIN_COM_CTRL0		0x1202
#define VDIN_ACTIVE_PIXELS	0x1203
#define VDIN_LCNT_STATUS		0x1204
#define VDIN_ASFIFO_CTRL1		0x1209
#define VDIN_WIDTHS		0x120a
#define VDIN_SC_MISC_CTRL		0x120b
#define VDIN_MATRIX_CTRL		0x1210
#define VDIN_LFIFO_CTRL		0x121a
#define VDIN_GCLK_CTRL		0x121b
#define VDIN_INTF_WIDTH		0x121c
#define VDIN_WR_CTRL2		0x121f
#define VDIN_WR_CTRL		0x1220
#define VDIN_WR_H_WIN		0x1221
#define VDIN_WR_V_WIN		0x1222
#define VDIN_WR_URGENT		0x1257
#define VDIN_WR_STATUS		0x1258
#define VDIN_GCLK_CTRL2		0x1270
#define VDIN_VSHRK_CTRL		0x1271
#define VDIN_HDR_CTRL		0x1280
#define VDIN_HDR_MATRIX_IN	0x12bb
#define VDIN_HDR_MATRIX_OUT	0x12bc
#define VDIN_WR_BADDR		0x12cc
#define VDIN_WR_STRIDE		0x12ce
#define VDIN_DOUBLE_CTRL		0x410b
#define VDIN_TOP_MISC		0x410d
#define VPU_SW_RESET		0x2720
#define VPU_WRARB_MODE		0x27a2
#define VPU_WRARB_REQEN		0x27a3
#define VDIN_WR_REQ		BIT(8)
#define VDIN_VCP_WR		BIT(25)
#define VDIN_DONE_CLEAR		BIT(21)
#define VDIN_WIDTH		1920
#define VDIN_HEIGHT		1080
#define VDIN_STRIDE		(VDIN_WIDTH * 3)
#define VDIN_FRAME_SIZE		(VDIN_STRIDE * VDIN_HEIGHT)

struct t7_vdin {
	struct device *dev;
	struct device *receiver;
	struct device *domains[3];
	unsigned int domain_count;
	void __iomem *regs;
	struct clk_bulk_data clocks[5];
	struct mutex lock; /* Serializes DMA capture and the immutable copy. */
	struct completion done;
	spinlock_t irq_lock; /* Protects armed/frame_irqs and request gating. */
	bool armed;
	bool faulted;
	unsigned int frame_irqs;
	unsigned int snapshots;
	int irq;
	void *frame;
	dma_addr_t frame_dma;
	bool memory_on;
};

static u32 t7_vdin_read(struct t7_vdin *vdin, u32 reg)
{
	return readl(vdin->regs + reg * 4);
}

static void t7_vdin_write(struct t7_vdin *vdin, u32 reg, u32 value)
{
	writel(value, vdin->regs + reg * 4);
}

static void t7_vdin_update(struct t7_vdin *vdin, u32 reg, u32 mask, u32 value)
{
	t7_vdin_write(vdin, reg, (t7_vdin_read(vdin, reg) & ~mask) | (value & mask));
}

static irqreturn_t t7_vdin_write_done(int irq, void *data)
{
	struct t7_vdin *vdin = data;
	unsigned long flags;

	spin_lock_irqsave(&vdin->irq_lock, flags);
	/* The write-done interrupt is latched until this clear pulse. */
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, VDIN_DONE_CLEAR);
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, 0);
	if (vdin->armed && ++vdin->frame_irqs == 2) {
		/* Discard the first partial frame, then stop before another starts. */
		t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_WR_REQ | VDIN_VCP_WR, 0);
		t7_vdin_read(vdin, VDIN_WR_CTRL);
		vdin->armed = false;
		complete(&vdin->done);
	}
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	return IRQ_HANDLED;
}

static int t7_vdin_stop_dma(struct t7_vdin *vdin)
{
	unsigned long flags;
	u32 idle;
	int ret;

	spin_lock_irqsave(&vdin->irq_lock, flags);
	vdin->armed = false;
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_WR_REQ | VDIN_VCP_WR, 0);
	t7_vdin_read(vdin, VDIN_WR_CTRL);
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	synchronize_irq(vdin->irq);
	/* T7's integrated MIF uses write-done, not the older VPU arbiter bits. */
	ret = readl_poll_timeout(vdin->regs + VDIN_WR_STATUS * 4, idle,
				 vdin->frame_irqs == 2 && (idle & BIT(0)),
				 10, 100000);
	if (ret) {
		/* Keep DMA memory, clocks and binding alive until a hardware reboot. */
		if (!vdin->faulted)
			__module_get(THIS_MODULE);
		vdin->faulted = true;
		dev_err(vdin->dev, "DMA idle timeout (%#x); retaining buffer and module\n", idle);
	}
	return ret;
}

static void t7_vdin_configure(struct t7_vdin *vdin)
{
	t7_vdin_write(vdin, VDIN_WR_CTRL, 0);
	t7_vdin_update(vdin, VDIN_TOP_MISC, BIT(26) | BIT(29), BIT(26) | BIT(29));
	udelay(1);
	t7_vdin_update(vdin, VDIN_TOP_MISC, BIT(26) | BIT(29), 0);
	t7_vdin_write(vdin, VDIN_GCLK_CTRL, 0);
	t7_vdin_write(vdin, VDIN_GCLK_CTRL2, 0);
	/* HDMI input; components G/B/R pass through with no colour matrix. */
	t7_vdin_write(vdin, VDIN_COM_CTRL0, 0x915);
	t7_vdin_write(vdin, VDIN_ASFIFO_CTRL1, 0xe40000);
	t7_vdin_update(vdin, VPU_SW_RESET, BIT(26), BIT(26));
	t7_vdin_update(vdin, VPU_SW_RESET, BIT(26), 0);
	t7_vdin_write(vdin, VDIN_WIDTHS, ((VDIN_WIDTH - 1) << 16) | (VDIN_WIDTH - 1));
	t7_vdin_write(vdin, VDIN_INTF_WIDTH, VDIN_WIDTH - 1);
	t7_vdin_write(vdin, VDIN_SC_MISC_CTRL, 0);
	t7_vdin_write(vdin, VDIN_VSHRK_CTRL, 0);
	t7_vdin_write(vdin, VDIN_MATRIX_CTRL, 0);
	t7_vdin_write(vdin, VDIN_HDR_CTRL, 0);
	t7_vdin_write(vdin, VDIN_HDR_MATRIX_IN, 0);
	t7_vdin_write(vdin, VDIN_HDR_MATRIX_OUT, 0);
	t7_vdin_update(vdin, VDIN_LFIFO_CTRL, GENMASK(13, 0) | BIT(17) | BIT(18),
		       BIT(17) | 0x780);
	t7_vdin_write(vdin, VDIN_WR_CTRL2, BIT(18) | (2 << 12));
	t7_vdin_write(vdin, VDIN_WR_H_WIN, BIT(30) | (VDIN_WIDTH - 1));
	t7_vdin_write(vdin, VDIN_WR_V_WIN, VDIN_HEIGHT - 1);
	/* Packed 24-bit output; use EOL to reset the packer on every line. */
	t7_vdin_write(vdin, VDIN_WR_CTRL,
		      (3U << 30) | BIT(29) | BIT(27) | BIT(24) | BIT(23) | BIT(22) | (3 << 16) |
		      BIT(12) | BIT(10) | BIT(9));
	t7_vdin_write(vdin, VDIN_WR_BADDR, vdin->frame_dma >> 4);
	t7_vdin_write(vdin, VDIN_WR_STRIDE, VDIN_STRIDE >> 4);
	/* MIF0 takes normal VDIN0; done waits for the final AXI write response. */
	t7_vdin_update(vdin, VDIN_DOUBLE_CTRL, GENMASK(7, 0) | BIT(20),
		       BIT(4) | BIT(20));
	t7_vdin_update(vdin, VPU_WRARB_MODE, BIT(16), 0);
	t7_vdin_update(vdin, VPU_WRARB_REQEN, BIT(0), BIT(0));
	t7_vdin_write(vdin, VDIN_WR_URGENT, BIT(9) | BIT(8));
}

static int t7_vdin_capture(struct t7_vdin *vdin)
{
	unsigned long flags;
	bool completed;
	int ret;

	if (vdin->faulted)
		return -EIO;
	ret = t7_hdmirx_check_1080p(vdin->receiver);
	if (ret)
		return ret;
	memset(vdin->frame, 0xa5, VDIN_FRAME_SIZE);
	dma_wmb();
	t7_vdin_configure(vdin);
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, VDIN_DONE_CLEAR);
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_DONE_CLEAR, 0);
	reinit_completion(&vdin->done);
	spin_lock_irqsave(&vdin->irq_lock, flags);
	vdin->frame_irqs = 0;
	vdin->armed = true;
	t7_vdin_update(vdin, VDIN_WR_CTRL, VDIN_WR_REQ | VDIN_VCP_WR,
		       VDIN_WR_REQ | VDIN_VCP_WR);
	spin_unlock_irqrestore(&vdin->irq_lock, flags);
	completed = wait_for_completion_timeout(&vdin->done, msecs_to_jiffies(1000));
	ret = t7_vdin_stop_dma(vdin);
	if (ret)
		return ret;
	if (!completed) {
		dev_err(vdin->dev, "capture timeout: IRQs %u, pixels %#x, lines %#x, WR %#x\n",
			vdin->frame_irqs, t7_vdin_read(vdin, VDIN_ACTIVE_PIXELS),
			t7_vdin_read(vdin, VDIN_LCNT_STATUS), t7_vdin_read(vdin, VDIN_WR_STATUS));
		return -ETIMEDOUT;
	}
	dma_rmb();
	ret = t7_hdmirx_check_1080p(vdin->receiver);
	if (!ret)
		vdin->snapshots++;
	return ret;
}

static int t7_vdin_frame_open(struct inode *inode, struct file *file)
{
	struct t7_vdin *vdin = inode->i_private;
	void *snapshot;
	int ret;

	ret = debugfs_file_get(file->f_path.dentry);
	if (ret)
		return ret;
	snapshot = kvmalloc(VDIN_FRAME_SIZE, GFP_KERNEL);
	if (!snapshot) {
		ret = -ENOMEM;
		goto put_file;
	}
	ret = mutex_lock_interruptible(&vdin->lock);
	if (ret)
		goto free_snapshot;
	ret = t7_vdin_capture(vdin);
	if (!ret)
		memcpy(snapshot, vdin->frame, VDIN_FRAME_SIZE);
	mutex_unlock(&vdin->lock);
	if (ret)
		goto free_snapshot;
	/* Per-fd immutable storage survives later captures or device removal. */
	file->private_data = snapshot;
	debugfs_file_put(file->f_path.dentry);
	return 0;
free_snapshot:
	kvfree(snapshot);
put_file:
	debugfs_file_put(file->f_path.dentry);
	return ret;
}

static ssize_t t7_vdin_frame_read(struct file *file, char __user *buffer,
				  size_t size, loff_t *offset)
{
	return simple_read_from_buffer(buffer, size, offset, file->private_data, VDIN_FRAME_SIZE);
}

static int t7_vdin_frame_release(struct inode *inode, struct file *file)
{
	kvfree(file->private_data);
	return 0;
}

static const struct file_operations t7_vdin_frame_fops = {
	.owner = THIS_MODULE,
	.open = t7_vdin_frame_open,
	.read = t7_vdin_frame_read,
	.release = t7_vdin_frame_release,
};

static int t7_vdin_status(struct seq_file *s, void *unused)
{
	struct t7_vdin *vdin = dev_get_drvdata(s->private);

	seq_printf(s, "snapshots: %u\n", vdin->snapshots);
	seq_printf(s, "frame_irqs: %u\n", vdin->frame_irqs);
	seq_printf(s, "faulted: %u\n", vdin->faulted);
	seq_printf(s, "write_ctrl: %#x\n", t7_vdin_read(vdin, VDIN_WR_CTRL));
	seq_printf(s, "write_status: %#x\n", t7_vdin_read(vdin, VDIN_WR_STATUS));
	seq_printf(s, "pixels: %#x\n", t7_vdin_read(vdin, VDIN_ACTIVE_PIXELS));
	seq_printf(s, "lines: %#x\n", t7_vdin_read(vdin, VDIN_LCNT_STATUS));
	seq_printf(s, "vpu_hz: %lu\n", clk_get_rate(vdin->clocks[1].clk));
	seq_printf(s, "vapb_hz: %lu\n", clk_get_rate(vdin->clocks[2].clk));
	seq_printf(s, "clkb_hz: %lu\n", clk_get_rate(vdin->clocks[3].clk));
	seq_printf(s, "measure_hz: %lu\n", clk_get_rate(vdin->clocks[4].clk));
	return 0;
}

static void t7_vdin_power_off(void *data)
{
	struct t7_vdin *vdin = data;
	struct arm_smccc_res res;

	if (vdin->memory_on)
		arm_smccc_smc(0x82000093, 60, 0, 0, 0, 0, 0, 0, &res);
	while (vdin->domain_count) {
		struct device *domain = vdin->domains[--vdin->domain_count];

		pm_runtime_put_sync(domain);
		dev_pm_domain_detach(domain, true);
	}
}

static void t7_vdin_clocks_off(void *data)
{
	struct t7_vdin *vdin = data;

	clk_bulk_disable_unprepare(ARRAY_SIZE(vdin->clocks), vdin->clocks);
}

static void t7_vdin_free_frame(void *data)
{
	struct t7_vdin *vdin = data;

	if (!vdin->faulted)
		dma_free_coherent(vdin->dev, VDIN_FRAME_SIZE, vdin->frame, vdin->frame_dma);
}

static void t7_vdin_debugfs_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static void t7_vdin_put_receiver(void *data)
{
	put_device(data);
}

static int t7_vdin_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *receiver;
	struct device_node *node;
	struct arm_smccc_res res;
	struct dentry *debugfs;
	struct t7_vdin *vdin;
	unsigned int i;
	int ret;

	vdin = devm_kzalloc(dev, sizeof(*vdin), GFP_KERNEL);
	if (!vdin)
		return -ENOMEM;
	vdin->dev = dev;
	mutex_init(&vdin->lock);
	spin_lock_init(&vdin->irq_lock);
	init_completion(&vdin->done);
	platform_set_drvdata(pdev, vdin);
	node = of_parse_phandle(dev->of_node, "amlogic,receiver", 0);
	receiver = of_find_device_by_node(node);
	of_node_put(node);
	if (!receiver)
		return -EPROBE_DEFER;
	vdin->receiver = &receiver->dev;
	ret = devm_add_action_or_reset(dev, t7_vdin_put_receiver, vdin->receiver);
	if (ret)
		return ret;
	if (!device_is_bound(vdin->receiver))
		return -EPROBE_DEFER;
	if (!device_link_add(dev, vdin->receiver, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	vdin->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vdin->regs))
		return PTR_ERR(vdin->regs);
	vdin->clocks[0].id = "pclk";
	vdin->clocks[1].id = "vpu";
	vdin->clocks[2].id = "vapb";
	vdin->clocks[3].id = "clkb";
	vdin->clocks[4].id = "measure";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(vdin->clocks), vdin->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot acquire capture clocks\n");
	ret = devm_add_action_or_reset(dev, t7_vdin_power_off, vdin);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(vdin->domains); i++) {
		struct device *domain = dev_pm_domain_attach_by_id(dev, i);

		if (IS_ERR_OR_NULL(domain))
			return domain ? PTR_ERR(domain) : -ENODEV;
		ret = pm_runtime_resume_and_get(domain);
		if (ret < 0) {
			dev_pm_domain_detach(domain, true);
			return ret;
		}
		vdin->domains[vdin->domain_count++] = domain;
	}
	ret = clk_set_rate(vdin->clocks[1].clk, 400000000);
	if (ret)
		return ret;
	ret = clk_set_rate(vdin->clocks[2].clk, 250000000);
	if (ret)
		return ret;
	ret = clk_set_rate(vdin->clocks[3].clk, 400000000);
	if (ret)
		return ret;
	ret = clk_set_rate(vdin->clocks[4].clk, 50000000);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(vdin->clocks), vdin->clocks);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, t7_vdin_clocks_off, vdin);
	if (ret)
		return ret;
	/* T7 firmware mempd IDs follow 52 power domains: VDIN0 module index 8. */
	arm_smccc_smc(0x82000093, 60, 1, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		return dev_err_probe(dev, -EIO, "Cannot power VDIN0 line memory\n");
	vdin->memory_on = true;
	t7_vdin_write(vdin, VDIN_WR_CTRL, 0);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	vdin->frame = dma_alloc_coherent(dev, VDIN_FRAME_SIZE, &vdin->frame_dma, GFP_KERNEL);
	if (!vdin->frame)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, t7_vdin_free_frame, vdin);
	if (ret)
		return ret;
	vdin->irq = platform_get_irq(pdev, 0);
	if (vdin->irq < 0)
		return vdin->irq;
	ret = devm_request_irq(dev, vdin->irq, t7_vdin_write_done, 0, dev_name(dev), vdin);
	if (ret)
		return ret;
	debugfs = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(debugfs))
		return PTR_ERR(debugfs);
	debugfs_create_file("frame", 0400, debugfs, vdin, &t7_vdin_frame_fops);
	debugfs_create_devm_seqfile(dev, "status", debugfs, t7_vdin_status);
	ret = devm_add_action_or_reset(dev, t7_vdin_debugfs_remove, debugfs);
	if (ret)
		return ret;
	dev_info(dev, "exclusive 1080p snapshot ready; production capture not implemented\n");
	return 0;
}

static const struct of_device_id t7_vdin_match[] = {
	{ .compatible = "amlogic,t7-vdin-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, t7_vdin_match);

static struct platform_driver t7_vdin_driver = {
	.probe = t7_vdin_probe,
	.driver = {
		.name = "t7-vdin-test",
		.of_match_table = t7_vdin_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(t7_vdin_driver);

MODULE_DESCRIPTION("Amlogic T7 VDIN0 snapshot experiment");
MODULE_LICENSE("GPL");
