// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019 Amlogic, Inc. All rights reserved. */
/* Exclusive T7 VDIN0 capture experiment; DRM must remain disabled. */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>

#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

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

struct t7_vdin_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct t7_vdin {
	struct kref ref;
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
	u64 frame_timestamp;
	struct v4l2_device v4l2;
	struct video_device video;
	struct vb2_queue queue;
	struct mutex video_lock; /* V4L2 ioctls; never acquired by the worker. */
	spinlock_t queue_lock;
	struct list_head queued;
	wait_queue_head_t buffer_wait;
	struct task_struct *thread;
	bool streaming; /* Protected by lock, including debugfs exclusion. */
	u32 sequence;
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
		vdin->frame_timestamp = ktime_get_ns();
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
	ret = vdin->streaming ? -EBUSY : t7_vdin_capture(vdin);
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

static struct t7_vdin_buffer *t7_vdin_next_buffer(struct t7_vdin *vdin)
{
	struct t7_vdin_buffer *buffer = NULL;
	unsigned long flags;

	spin_lock_irqsave(&vdin->queue_lock, flags);
	if (!list_empty(&vdin->queued)) {
		buffer = list_first_entry(&vdin->queued, struct t7_vdin_buffer, list);
		list_del(&buffer->list);
	}
	spin_unlock_irqrestore(&vdin->queue_lock, flags);
	return buffer;
}

static bool t7_vdin_has_buffer(struct t7_vdin *vdin)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&vdin->queue_lock, flags);
	ready = !list_empty(&vdin->queued);
	spin_unlock_irqrestore(&vdin->queue_lock, flags);
	return ready;
}

static void t7_vdin_return_buffers(struct t7_vdin *vdin, enum vb2_buffer_state state)
{
	struct t7_vdin_buffer *buffer;

	while ((buffer = t7_vdin_next_buffer(vdin)))
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
}

static int t7_vdin_thread(void *data)
{
	struct t7_vdin *vdin = data;

	while (!kthread_should_stop()) {
		struct t7_vdin_buffer *buffer;
		int ret;

		wait_event_interruptible(vdin->buffer_wait,
					 kthread_should_stop() || t7_vdin_has_buffer(vdin));
		if (kthread_should_stop())
			break;
		buffer = t7_vdin_next_buffer(vdin);
		if (!buffer)
			continue;
		mutex_lock(&vdin->lock);
		ret = t7_vdin_capture(vdin);
		if (!ret) {
			/* Hardware never addresses vb2 memory, even on a stop timeout. */
			memcpy(vb2_plane_vaddr(&buffer->vb.vb2_buf, 0),
			       vdin->frame, VDIN_FRAME_SIZE);
			buffer->vb.vb2_buf.timestamp = vdin->frame_timestamp;
			buffer->vb.sequence = vdin->sequence++;
			buffer->vb.field = V4L2_FIELD_NONE;
		}
		mutex_unlock(&vdin->lock);
		vb2_buffer_done(&buffer->vb.vb2_buf,
				ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		if (ret) {
			vb2_queue_error(&vdin->queue);
			t7_vdin_return_buffers(vdin, VB2_BUF_STATE_ERROR);
			/* Keep the task alive until STREAMOFF joins it. */
			wait_event_interruptible(vdin->buffer_wait, kthread_should_stop());
			break;
		}
	}
	return 0;
}

static int t7_vdin_queue_setup(struct vb2_queue *queue, unsigned int *buffers,
			       unsigned int *planes, unsigned int sizes[],
			       struct device *alloc_devs[])
{
	if (*planes)
		return *planes == 1 && sizes[0] >= VDIN_FRAME_SIZE ? 0 : -EINVAL;
	*planes = 1;
	sizes[0] = VDIN_FRAME_SIZE;
	return 0;
}

static int t7_vdin_buffer_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < VDIN_FRAME_SIZE)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, VDIN_FRAME_SIZE);
	return 0;
}

static void t7_vdin_buffer_queue(struct vb2_buffer *vb)
{
	struct t7_vdin *vdin = vb2_get_drv_priv(vb->vb2_queue);
	struct t7_vdin_buffer *buffer = container_of(to_vb2_v4l2_buffer(vb),
						   struct t7_vdin_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&vdin->queue_lock, flags);
	list_add_tail(&buffer->list, &vdin->queued);
	spin_unlock_irqrestore(&vdin->queue_lock, flags);
	wake_up(&vdin->buffer_wait);
}

static int t7_vdin_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct t7_vdin *vdin = vb2_get_drv_priv(queue);
	int ret;

	mutex_lock(&vdin->lock);
	ret = vdin->faulted ? -EIO : t7_hdmirx_check_1080p(vdin->receiver);
	if (!ret) {
		vdin->streaming = true;
		vdin->sequence = 0;
		vdin->thread = kthread_run(t7_vdin_thread, vdin, "t7-vdin-capture");
		if (IS_ERR(vdin->thread)) {
			ret = PTR_ERR(vdin->thread);
			vdin->thread = NULL;
			vdin->streaming = false;
		}
	}
	mutex_unlock(&vdin->lock);
	if (ret)
		t7_vdin_return_buffers(vdin, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void t7_vdin_stop_streaming(struct vb2_queue *queue)
{
	struct t7_vdin *vdin = vb2_get_drv_priv(queue);

	/* Finish the bounded capture and CPU copy before returning any buffer. */
	kthread_stop(vdin->thread);
	vdin->thread = NULL;
	t7_vdin_return_buffers(vdin, VB2_BUF_STATE_ERROR);
	mutex_lock(&vdin->lock);
	vdin->streaming = false;
	mutex_unlock(&vdin->lock);
}

static const struct vb2_ops t7_vdin_queue_ops = {
	.queue_setup = t7_vdin_queue_setup,
	.buf_prepare = t7_vdin_buffer_prepare,
	.buf_queue = t7_vdin_buffer_queue,
	.start_streaming = t7_vdin_start_streaming,
	.stop_streaming = t7_vdin_stop_streaming,
};

static int t7_vdin_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "t7-vdin-test", sizeof(cap->driver));
	strscpy(cap->card, "T7 HDMI capture experiment", sizeof(cap->card));
	return 0;
}

static int t7_vdin_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *fmt)
{
	if (fmt->index)
		return -EINVAL;
	fmt->pixelformat = V4L2_PIX_FMT_RGB24;
	return 0;
}

static int t7_vdin_get_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	fmt->fmt.pix = (struct v4l2_pix_format) {
		.width = VDIN_WIDTH,
		.height = VDIN_HEIGHT,
		.pixelformat = V4L2_PIX_FMT_RGB24,
		.field = V4L2_FIELD_NONE,
		.bytesperline = VDIN_STRIDE,
		.sizeimage = VDIN_FRAME_SIZE,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.xfer_func = V4L2_XFER_FUNC_SRGB,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
	};
	return 0;
}

static int t7_vdin_set_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct t7_vdin *vdin = video_drvdata(file);

	if (vb2_is_busy(&vdin->queue))
		return -EBUSY;
	return t7_vdin_get_fmt(file, priv, fmt);
}

static int t7_vdin_enum_input(struct file *file, void *priv, struct v4l2_input *input)
{
	struct t7_vdin *vdin = video_drvdata(file);

	if (input->index)
		return -EINVAL;
	strscpy(input->name, "HDMI", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->capabilities = 0;
	if (t7_hdmirx_check_1080p(vdin->receiver))
		input->status = V4L2_IN_ST_NO_SYNC;
	return 0;
}

static int t7_vdin_get_input(struct file *file, void *priv, unsigned int *input)
{
	*input = 0;
	return 0;
}

static int t7_vdin_set_input(struct file *file, void *priv, unsigned int input)
{
	return input ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops t7_vdin_ioctl_ops = {
	.vidioc_querycap = t7_vdin_querycap,
	.vidioc_enum_fmt_vid_cap = t7_vdin_enum_fmt,
	.vidioc_g_fmt_vid_cap = t7_vdin_get_fmt,
	.vidioc_try_fmt_vid_cap = t7_vdin_get_fmt,
	.vidioc_s_fmt_vid_cap = t7_vdin_set_fmt,
	.vidioc_enum_input = t7_vdin_enum_input,
	.vidioc_g_input = t7_vdin_get_input,
	.vidioc_s_input = t7_vdin_set_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations t7_vdin_video_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static void t7_vdin_release(struct kref *ref)
{
	kfree(container_of(ref, struct t7_vdin, ref));
}

static void t7_vdin_put(void *data)
{
	struct t7_vdin *vdin = data;

	kref_put(&vdin->ref, t7_vdin_release);
}

static void t7_vdin_video_release(struct video_device *video)
{
	t7_vdin_put(video_get_drvdata(video));
}

static void t7_vdin_video_unregister(void *data)
{
	struct t7_vdin *vdin = data;

	vb2_video_unregister_device(&vdin->video);
	v4l2_device_unregister(&vdin->v4l2);
}

static int t7_vdin_video_register(struct t7_vdin *vdin)
{
	struct vb2_queue *queue = &vdin->queue;
	struct video_device *video = &vdin->video;
	int ret;

	ret = v4l2_device_register(vdin->dev, &vdin->v4l2);
	if (ret)
		return ret;
	queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	queue->io_modes = VB2_MMAP;
	queue->drv_priv = vdin;
	queue->buf_struct_size = sizeof(struct t7_vdin_buffer);
	queue->ops = &t7_vdin_queue_ops;
	queue->mem_ops = &vb2_vmalloc_memops;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
				 V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	queue->min_queued_buffers = 2;
	queue->lock = &vdin->video_lock;
	queue->dev = vdin->dev;
	ret = vb2_queue_init(queue);
	if (ret)
		goto unregister_v4l2;
	strscpy(video->name, "t7-vdin-test", sizeof(video->name));
	video->v4l2_dev = &vdin->v4l2;
	video->fops = &t7_vdin_video_fops;
	video->ioctl_ops = &t7_vdin_ioctl_ops;
	video->release = t7_vdin_video_release;
	video->lock = &vdin->video_lock;
	video->queue = queue;
	video->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(video, vdin);
	/* Open files can outlive platform removal; vb2 memory has no DMA owner. */
	kref_get(&vdin->ref);
	ret = video_register_device(video, VFL_TYPE_VIDEO, -1);
	if (ret) {
		t7_vdin_put(vdin);
		vb2_queue_release(queue);
		goto unregister_v4l2;
	}
	return devm_add_action_or_reset(vdin->dev, t7_vdin_video_unregister, vdin);

unregister_v4l2:
	v4l2_device_unregister(&vdin->v4l2);
	return ret;
}

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

	vdin = kzalloc_obj(*vdin);
	if (!vdin)
		return -ENOMEM;
	kref_init(&vdin->ref);
	ret = devm_add_action_or_reset(dev, t7_vdin_put, vdin);
	if (ret)
		return ret;
	vdin->dev = dev;
	mutex_init(&vdin->lock);
	mutex_init(&vdin->video_lock);
	spin_lock_init(&vdin->irq_lock);
	spin_lock_init(&vdin->queue_lock);
	INIT_LIST_HEAD(&vdin->queued);
	init_waitqueue_head(&vdin->buffer_wait);
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
	ret = t7_vdin_video_register(vdin);
	if (ret)
		return ret;
	dev_info(dev, "exclusive 1080p RGB24 capture ready at /dev/video%d\n", vdin->video.num);
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

MODULE_DESCRIPTION("Amlogic T7 VDIN0 capture experiment");
MODULE_LICENSE("GPL");
