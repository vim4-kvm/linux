// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/* Recover the T7 encoder, retaining DMA ownership if recovery fails. */
#include <linux/dma-buf.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/ktime.h>
#include "wave5-vpu.h"

#define WAVE5_JOB_TIMEOUT_MS 5000
#define WAVE5_ABORT_TIMEOUT_MS 1000

/* One shot: acknowledge a real command, then simulate losing its completion. */
static unsigned int timeout_command;
module_param(timeout_command, uint, 0600);
MODULE_PARM_DESC(timeout_command, "One-shot T7 command timeout injection (command opcode)");

bool wave5_vpu_isolated(struct vpu_device *dev)
{
	/* Observe retired contexts before allowing access after recovery. */
	return smp_load_acquire(&dev->isolated);
}

bool wave5_vpu_hw_blocked(struct vpu_device *dev)
{
	/* Only the worker holding dev_lock, irq_lock and hw_lock may recover MMIO. */
	return wave5_vpu_isolated(dev) && READ_ONCE(dev->recovery_owner) != current;
}

bool wave5_vpu_inject_timeout(struct vpu_device *dev, u32 command)
{
	if (!dev->isolation_ready || !dev->legacy_firmware || !command)
		return false;
	return cmpxchg(&timeout_command, command, 0) == command;
}

void wave5_vpu_isolate(struct vpu_device *dev, const char *reason, u32 command)
{
	/* Decoder and other integrations have different buffer lifetimes. */
	if (!dev->isolation_ready || !dev->legacy_firmware)
		return;
	if (cmpxchg(&dev->isolated, 0, 1)) {
		if (READ_ONCE(dev->recovery_owner))
			WRITE_ONCE(dev->recovery_failed, true);
		return;
	}
	WRITE_ONCE(dev->isolation_reason, reason);
	WRITE_ONCE(dev->recovery_failed, false);
	WRITE_ONCE(dev->isolation_command, command);
	WRITE_ONCE(dev->isolation_time_ns, ktime_get_boottime_ns());
	/* Keep callbacks and DMA storage until recovery succeeds or the board reboots. */
	__module_get(THIS_MODULE);
	get_device(dev->dev);
	if (dev->irq >= 0)
		disable_irq_nosync(dev->irq);
	/* Publish metadata before the sysfs recovering reader's acquire load. */
	smp_store_release(&dev->isolated, 3);
	schedule_work(&dev->isolation_work);
}

/* All scheduler completion paths are serialized by irq_lock. */
void wave5_vpu_job_finish(struct vpu_instance *inst)
{
	lockdep_assert_held(&inst->dev->irq_lock);
	if (v4l2_m2m_get_curr_priv(inst->v4l2_m2m_dev) != inst)
		return;
	cancel_delayed_work(&inst->dev->job_timeout_work);
	v4l2_m2m_job_finish(inst->v4l2_m2m_dev, inst->v4l2_fh.m2m_ctx);
	wake_up_all(&inst->job_wait);
}

void wave5_vpu_job_start(struct vpu_instance *inst)
{
	lockdep_assert_held(&inst->dev->irq_lock);
	if (!inst->dev->legacy_firmware)
		return;
	inst->dev->job_deadline = jiffies + msecs_to_jiffies(WAVE5_JOB_TIMEOUT_MS);
	mod_delayed_work(system_wq, &inst->dev->job_timeout_work,
			 msecs_to_jiffies(WAVE5_JOB_TIMEOUT_MS));
}

static void wave5_release_planes(struct vpu_device *dev);

static int wave5_recover(struct vpu_device *dev)
{
	struct vpu_instance *inst, *tmp;
	int ret;

	/* Scheduler waits have been completed before acquiring the ioctl lock. */
	mutex_lock(&dev->dev_lock);
	mutex_lock(&dev->irq_lock);
	mutex_lock(&dev->hw_lock);
	WRITE_ONCE(dev->recovery_owner, current);
	ret = READ_ONCE(dev->shutting_down) ? -ESHUTDOWN : wave5_vpu_recover_hardware(dev);
	WRITE_ONCE(dev->recovery_owner, NULL);
	mutex_unlock(&dev->hw_lock);
	if (!ret && READ_ONCE(dev->recovery_failed))
		ret = -EIO;
	if (ret) {
		dev_err(dev->dev, "power-cycle recovery failed: %d\n", ret);
		/* Publish the final state to the sysfs isolated reader. */
		smp_store_release(&dev->isolated, 2);
		goto unlock;
	}
	/* Old contexts never submit again, even after the device becomes usable. */
	list_for_each_entry(inst, &dev->instances, list) {
		inst->state = VPU_INST_STATE_NONE;
		kfifo_reset(&inst->irq_status);
	}
	/* Publish retired instances before sysfs reports the device usable. */
	smp_store_release(&dev->isolated, 0);
	mutex_unlock(&dev->irq_lock);
	wave5_release_planes(dev);
	list_for_each_entry_safe(inst, tmp, &dev->instances, list) {
		wave5_vpu_enc_free_buffers(inst);
		if (inst->released)
			wave5_vpu_release_retained(inst);
	}
	if (dev->irq >= 0)
		enable_irq(dev->irq);
	else
		hrtimer_start(&dev->hrtimer, ns_to_ktime(dev->vpu_poll_interval * NSEC_PER_MSEC),
			      HRTIMER_MODE_REL_PINNED);
	mutex_unlock(&dev->dev_lock);
	return 0;
unlock:
	mutex_unlock(&dev->irq_lock);
	mutex_unlock(&dev->dev_lock);
	return ret;
}

static void wave5_isolation_work(struct work_struct *work)
{
	struct vpu_device *dev = container_of(work, struct vpu_device, isolation_work);
	struct vpu_instance *inst;
	char event[] = "WAVE5_ISOLATED=1";
	char recovered[] = "WAVE5_RECOVERED=1";
	char *envp[2];

	envp[0] = event;
	envp[1] = NULL;

	/* Drain a handler that passed the guard before the timeout was claimed. */
	if (dev->irq >= 0) {
		synchronize_irq(dev->irq);
	} else {
		hrtimer_cancel(&dev->hrtimer);
		kthread_flush_work(&dev->work);
	}
	/* Complete scheduler waits before taking dev_lock in wave5_recover(). */
	mutex_lock(&dev->irq_lock);
	list_for_each_entry(inst, &dev->instances, list) {
		struct v4l2_m2m_ctx *ctx = inst->v4l2_fh.m2m_ctx;

		WRITE_ONCE(inst->recovery_error, true);
		vb2_queue_error(&ctx->out_q_ctx.q);
		vb2_queue_error(&ctx->cap_q_ctx.q);
		complete_all(&inst->irq_done);
		wave5_vpu_job_finish(inst);
	}
	mutex_unlock(&dev->irq_lock);
	if (!wave5_recover(dev)) {
		envp[0] = recovered;
		dev_info(dev->dev, "power cycle and firmware reload complete; reopen old instances\n");
		kobject_uevent_env(&dev->dev->kobj, KOBJ_CHANGE, envp);
		put_device(dev->dev);
		module_put(THIS_MODULE);
		return;
	}
	dev_err(dev->dev, "isolated: %s, command %#x; DMA storage retained until reboot\n",
		dev->isolation_reason, dev->isolation_command);
	kobject_uevent_env(&dev->dev->kobj, KOBJ_CHANGE, envp);
}

static void wave5_job_timeout_work(struct work_struct *work)
{
	struct vpu_device *dev = container_of(to_delayed_work(work),
					    struct vpu_device, job_timeout_work);
	struct vpu_instance *inst;

	mutex_lock(&dev->irq_lock);
	inst = v4l2_m2m_get_curr_priv(dev->v4l2_m2m_enc_dev);
	if (inst && !wave5_vpu_isolated(dev)) {
		/* A previous timer may have started just before a new job was armed. */
		if (time_before(jiffies, dev->job_deadline))
			mod_delayed_work(system_wq, &dev->job_timeout_work,
					 dev->job_deadline - jiffies);
		else
			wave5_vpu_isolate(dev, "frame completion timeout", 0);
	}
	mutex_unlock(&dev->irq_lock);
}

void wave5_vpu_job_abort(void *priv)
{
	struct vpu_instance *inst = priv;
	struct vpu_device *dev = inst->dev;

	if (wait_event_timeout(inst->job_wait,
			       v4l2_m2m_get_curr_priv(inst->v4l2_m2m_dev) != inst,
			       msecs_to_jiffies(WAVE5_ABORT_TIMEOUT_MS)))
		return;
	wave5_vpu_isolate(dev, "job abort timeout", 0);
	mutex_lock(&dev->irq_lock);
	/* Also covers cancel racing with device_run before it takes irq_lock. */
	wave5_vpu_job_finish(inst);
	mutex_unlock(&dev->irq_lock);
}

void wave5_vpu_isolation_init(struct vpu_device *dev)
{
	INIT_LIST_HEAD(&dev->retained_planes);
	INIT_WORK(&dev->isolation_work, wave5_isolation_work);
	INIT_DELAYED_WORK(&dev->job_timeout_work, wave5_job_timeout_work);
}

static ssize_t isolated_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct vpu_device *dev = dev_get_drvdata(device);

	/* Pair with publication so readers can subsequently fetch reason/time. */
	return sysfs_emit(buf, "%u\n", smp_load_acquire(&dev->isolated) == 2);
}
static DEVICE_ATTR_RO(isolated);

static ssize_t recovering_show(struct device *device, struct device_attribute *attr, char *buf)
{
	struct vpu_device *dev = dev_get_drvdata(device);

	/* Pair with the metadata publication before recovery is queued. */
	return sysfs_emit(buf, "%u\n", smp_load_acquire(&dev->isolated) == 3);
}
static DEVICE_ATTR_RO(recovering);

static ssize_t isolation_reason_show(struct device *device, struct device_attribute *attr,
				     char *buf)
{
	struct vpu_device *dev = dev_get_drvdata(device);
	const char *reason = READ_ONCE(dev->isolation_reason);

	return sysfs_emit(buf, "%s\n", reason ?: "none");
}
static DEVICE_ATTR_RO(isolation_reason);

static ssize_t isolation_time_ns_show(struct device *device, struct device_attribute *attr,
				      char *buf)
{
	struct vpu_device *dev = dev_get_drvdata(device);

	return sysfs_emit(buf, "%llu\n", READ_ONCE(dev->isolation_time_ns));
}
static DEVICE_ATTR_RO(isolation_time_ns);

static struct attribute *wave5_isolation_attrs[] = {
	&dev_attr_isolated.attr,
	&dev_attr_recovering.attr,
	&dev_attr_isolation_reason.attr,
	&dev_attr_isolation_time_ns.attr,
	NULL,
};

const struct attribute_group wave5_isolation_group = {
	.attrs = wave5_isolation_attrs,
};

/*
 * vb2 STREAMOFF can unmap imported planes even when stop_streaming returns
 * without touching them. Hold an independent dma-buf reference and retain the
 * allocator's original reference/mapping when DMA termination is unproven.
 * MMAP's VMA/export references can then disappear without freeing our storage.
 */
struct wave5_plane {
	struct vpu_device *dev;
	void *memory;
	struct dma_buf *dbuf;
	struct list_head retained;
	bool release_pending;
	bool unmap_pending;
};

static void wave5_retain_plane(struct wave5_plane *plane)
{
	lockdep_assert_held(&plane->dev->dev_lock);
	if (list_empty(&plane->retained))
		list_add_tail(&plane->retained, &plane->dev->retained_planes);
}

static void *wave5_plane_alloc(struct vb2_buffer *vb, struct device *dev, unsigned long size)
{
	struct vpu_instance *inst = vb2_get_drv_priv(vb->vb2_queue);
	struct wave5_plane *plane = kzalloc(sizeof(*plane), GFP_KERNEL);

	if (!plane)
		return ERR_PTR(-ENOMEM);
	plane->dev = inst->dev;
	INIT_LIST_HEAD(&plane->retained);
	plane->memory = vb2_dma_contig_memops.alloc(vb, dev, size);
	if (IS_ERR(plane->memory)) {
		void *error = plane->memory;

		kfree(plane);
		return error;
	}
	return plane;
}

static void wave5_plane_put(void *priv)
{
	struct wave5_plane *plane = priv;

	if (wave5_vpu_isolated(plane->dev)) {
		plane->release_pending = true;
		wave5_retain_plane(plane);
		return;
	}
	vb2_dma_contig_memops.put(plane->memory);
	kfree(plane);
}

static void *wave5_plane_attach(struct vb2_buffer *vb, struct device *dev,
				struct dma_buf *dbuf, unsigned long size)
{
	struct vpu_instance *inst = vb2_get_drv_priv(vb->vb2_queue);
	struct wave5_plane *plane = kzalloc(sizeof(*plane), GFP_KERNEL);

	if (!plane)
		return ERR_PTR(-ENOMEM);
	plane->dev = inst->dev;
	INIT_LIST_HEAD(&plane->retained);
	plane->memory = vb2_dma_contig_memops.attach_dmabuf(vb, dev, dbuf, size);
	if (IS_ERR(plane->memory)) {
		void *error = plane->memory;

		kfree(plane);
		return error;
	}
	get_dma_buf(dbuf);
	plane->dbuf = dbuf;
	return plane;
}

static void wave5_plane_detach(void *priv)
{
	struct wave5_plane *plane = priv;

	if (wave5_vpu_isolated(plane->dev)) {
		plane->release_pending = true;
		wave5_retain_plane(plane);
		return;
	}
	vb2_dma_contig_memops.detach_dmabuf(plane->memory);
	dma_buf_put(plane->dbuf);
	kfree(plane);
}

static int wave5_plane_map(void *priv)
{
	struct wave5_plane *plane = priv;

	if (wave5_vpu_isolated(plane->dev))
		return -EIO;
	return vb2_dma_contig_memops.map_dmabuf(plane->memory);
}

static void wave5_plane_unmap(void *priv)
{
	struct wave5_plane *plane = priv;

	if (wave5_vpu_isolated(plane->dev)) {
		plane->unmap_pending = true;
		wave5_retain_plane(plane);
		return;
	}
	vb2_dma_contig_memops.unmap_dmabuf(plane->memory);
}

static void wave5_release_planes(struct vpu_device *dev)
{
	struct wave5_plane *plane, *tmp;

	lockdep_assert_held(&dev->dev_lock);
	list_for_each_entry_safe(plane, tmp, &dev->retained_planes, retained) {
		list_del_init(&plane->retained);
		if (plane->unmap_pending) {
			plane->unmap_pending = false;
			wave5_plane_unmap(plane);
		}
		if (plane->release_pending) {
			if (plane->dbuf)
				wave5_plane_detach(plane);
			else
				wave5_plane_put(plane);
		}
	}
}

static void wave5_plane_prepare(void *priv)
{
	struct wave5_plane *plane = priv;

	if (!wave5_vpu_isolated(plane->dev))
		vb2_dma_contig_memops.prepare(plane->memory);
}

static void wave5_plane_finish(void *priv)
{
	struct wave5_plane *plane = priv;

	if (!wave5_vpu_isolated(plane->dev))
		vb2_dma_contig_memops.finish(plane->memory);
}

static void *wave5_plane_cookie(struct vb2_buffer *vb, void *priv)
{
	struct wave5_plane *plane = priv;

	return vb2_dma_contig_memops.cookie(vb, plane->memory);
}

static void *wave5_plane_vaddr(struct vb2_buffer *vb, void *priv)
{
	struct wave5_plane *plane = priv;

	return vb2_dma_contig_memops.vaddr(vb, plane->memory);
}

static unsigned int wave5_plane_users(void *priv)
{
	struct wave5_plane *plane = priv;

	return vb2_dma_contig_memops.num_users(plane->memory);
}

static int wave5_plane_mmap(void *priv, struct vm_area_struct *vma)
{
	struct wave5_plane *plane = priv;

	return vb2_dma_contig_memops.mmap(plane->memory, vma);
}

static struct dma_buf *wave5_plane_export(struct vb2_buffer *vb, void *priv,
					  unsigned long flags)
{
	struct wave5_plane *plane = priv;

	return vb2_dma_contig_memops.get_dmabuf(vb, plane->memory, flags);
}

const struct vb2_mem_ops wave5_quarantine_memops = {
	.alloc = wave5_plane_alloc,
	.put = wave5_plane_put,
	.attach_dmabuf = wave5_plane_attach,
	.detach_dmabuf = wave5_plane_detach,
	.map_dmabuf = wave5_plane_map,
	.unmap_dmabuf = wave5_plane_unmap,
	.prepare = wave5_plane_prepare,
	.finish = wave5_plane_finish,
	.cookie = wave5_plane_cookie,
	.vaddr = wave5_plane_vaddr,
	.num_users = wave5_plane_users,
	.mmap = wave5_plane_mmap,
	.get_dmabuf = wave5_plane_export,
};
