#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run actual quarantine/scheduler/allocator C against deterministic owners.

Models scheduler races and buffer lifetimes, not physical DMA termination.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / 'wave5-isolation.c').read_text()
logic = source[source.index('bool wave5_vpu_isolated('):source.index('static ssize_t isolated_show(')]
planes = source[source.index('struct wave5_plane {'):]
core = (root.parents[4] / 'include/media/videobuf2-core.h').read_text()
memops = core[core.index('struct vb2_mem_ops {'):core.index('\n};', core.index('struct vb2_mem_ops {')) + 3]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v, x) ((v) = (x))
#define smp_store_release(p, value) (*(p) = (value))
#define cmpxchg(p, old, value) ({ unsigned _was = *(p); if (_was == (old)) *(p) = (value); _was; })
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define WAVE5_JOB_TIMEOUT_MS 5000
#define WAVE5_ABORT_TIMEOUT_MS 1000
#define msecs_to_jiffies(x) (x)
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define THIS_MODULE NULL
#define KOBJ_CHANGE 1
#define GFP_KERNEL 0
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x) > (uintptr_t)-4096)
#define dev_err(...) ((void)0)
struct device { unsigned refs; int kobj; };
struct work_struct { void (*fn)(struct work_struct *); bool pending; };
struct delayed_work { struct work_struct work; unsigned long delay; };
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
#define INIT_WORK(w, f) ((w)->fn = (f))
#define INIT_DELAYED_WORK(w, f) INIT_WORK(&(w)->work, f)
static void *system_wq;
static unsigned long jiffies;
static unsigned timeout_command, pins, disables, events, finishes, wakes, waits;
struct vpu_instance;
struct vb2_queue { bool error; void *priv; };
struct vb2_buffer { struct vb2_queue *vb2_queue; };
struct v4l2_m2m_ctx { struct { struct vb2_queue q; } out_q_ctx, cap_q_ctx; };
struct m2m_dev { struct vpu_instance *current; };
struct list_head { struct vpu_instance *first; };
struct vpu_device {
    unsigned isolated;
    bool isolation_ready, legacy_firmware;
    const char *isolation_reason;
    u32 isolation_command;
    u64 isolation_time_ns;
    struct work_struct isolation_work;
    struct delayed_work job_timeout_work;
    unsigned long job_deadline;
    int irq_lock, irq;
    struct device *dev;
    struct list_head instances;
    struct m2m_dev *v4l2_m2m_enc_dev;
};
struct vpu_instance {
    struct vpu_device *dev;
    struct m2m_dev *v4l2_m2m_dev;
    struct { struct v4l2_m2m_ctx *m2m_ctx; } v4l2_fh;
    struct vpu_instance *next;
    unsigned irq_done, job_wait;
};
#define list_for_each_entry(p, head, member) for ((p) = (head)->first; (p); (p) = (p)->next)
static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
#define lockdep_assert_held(lock) assert(*(lock))
static u64 ktime_get_boottime_ns(void) { return 123456789; }
static void __module_get(void *module) { (void)module; pins++; }
static void get_device(struct device *dev) { dev->refs++; }
static void disable_irq_nosync(int irq) { assert(irq >= 0); disables++; }
static void schedule_work(struct work_struct *work) { work->pending = true; }
static void cancel_delayed_work(struct delayed_work *work) { work->work.pending = false; }
static void mod_delayed_work(void *queue, struct delayed_work *work, unsigned long delay)
{ (void)queue; work->work.pending = true; work->delay = delay; }
static void *v4l2_m2m_get_curr_priv(struct m2m_dev *dev) { return dev->current; }
static void v4l2_m2m_job_finish(struct m2m_dev *dev, struct v4l2_m2m_ctx *ctx)
{ assert(dev->current && dev->current->v4l2_fh.m2m_ctx == ctx); dev->current = NULL; finishes++; }
static void wake_up_all(unsigned *wait) { (void)wait; wakes++; }
static void complete_all(unsigned *completion) { *completion = 1; }
static void vb2_queue_error(struct vb2_queue *q) { q->error = true; }
static void kobject_uevent_env(int *kobj, int action, char **env)
{ (void)kobj; assert(action == KOBJ_CHANGE && env[0]); events++; }
/* Inject completion exactly at abort's wait boundary, without sleeping. */
static struct m2m_dev *complete_during_wait;
static int wait_boundary(bool condition)
{
    waits++;
    if (complete_during_wait) { complete_during_wait->current = NULL; return 1; }
    return condition;
}
#define wait_event_timeout(w, condition, timeout) ((void)(w), (void)(timeout), wait_boundary(condition))
static void *kzalloc(size_t n, int flags) { (void)flags; return calloc(1, n); }
#define kfree free
struct dma_buf { unsigned refs; };
static void get_dma_buf(struct dma_buf *buf) { buf->refs++; }
static void dma_buf_put(struct dma_buf *buf) { assert(buf->refs); buf->refs--; }
struct vm_area_struct { int unused; };
'''
allocator = r'''
struct allocation { unsigned refs, maps; struct dma_buf *dbuf; uintptr_t addr; };
static unsigned frees, unmaps, prepares, syncs, detaches;
static void *vb2_get_drv_priv(struct vb2_queue *q) { return q->priv; }
static void *alloc_plane(struct vb2_buffer *vb, struct device *dev, unsigned long size)
{ (void)vb; (void)dev; (void)size; struct allocation *a = calloc(1, sizeof(*a)); a->refs=1; return a; }
static void put_plane(void *priv)
{ struct allocation *a=priv; assert(a->refs); if (!--a->refs) { frees++; free(a); } }
static void *attach_plane(struct vb2_buffer *vb, struct device *dev, struct dma_buf *buf, unsigned long size)
{ struct allocation *a=alloc_plane(vb, dev, size); a->dbuf=buf; return a; }
static void detach_plane(void *priv) { detaches++; put_plane(priv); }
static int map_plane(void *priv) { ((struct allocation *)priv)->maps++; return 0; }
static void unmap_plane(void *priv) { unmaps++; assert(((struct allocation *)priv)->maps--); }
static void prepare_plane(void *priv) { (void)priv; prepares++; }
static void finish_plane(void *priv) { (void)priv; syncs++; }
static void *cookie_plane(struct vb2_buffer *vb, void *priv)
{ (void)vb; return &((struct allocation *)priv)->addr; }
static void *vaddr_plane(struct vb2_buffer *vb, void *priv) { (void)vb; return priv; }
static unsigned users_plane(void *priv) { return ((struct allocation *)priv)->refs; }
static int mmap_plane(void *priv, struct vm_area_struct *vma)
{ (void)vma; ((struct allocation *)priv)->refs++; return 0; }
static struct dma_buf *export_plane(struct vb2_buffer *vb, void *priv, unsigned long flags)
{ (void)vb; (void)flags; ((struct allocation *)priv)->refs++; return NULL; }
static const struct vb2_mem_ops vb2_dma_contig_memops = {
    .alloc=alloc_plane, .put=put_plane, .attach_dmabuf=attach_plane, .detach_dmabuf=detach_plane,
    .map_dmabuf=map_plane, .unmap_dmabuf=unmap_plane, .prepare=prepare_plane, .finish=finish_plane,
    .cookie=cookie_plane, .vaddr=vaddr_plane, .num_users=users_plane,
    .mmap=mmap_plane, .get_dmabuf=export_plane,
};
'''
checks = r'''
int main(void)
{
    struct device device = {};
    struct m2m_dev scheduler = {};
    struct vpu_device dev = {.isolation_ready=true, .legacy_firmware=true, .dev=&device,
                             .v4l2_m2m_enc_dev=&scheduler, .irq=1};
    struct v4l2_m2m_ctx contexts[2] = {};
    struct vpu_instance a = {.dev=&dev, .v4l2_m2m_dev=&scheduler, .v4l2_fh={&contexts[0]}};
    struct vpu_instance b = {.dev=&dev, .v4l2_m2m_dev=&scheduler, .v4l2_fh={&contexts[1]}};
    a.next=&b; dev.instances.first=&a;
    wave5_vpu_isolation_init(&dev);
    timeout_command=8;
    assert(!wave5_vpu_inject_timeout(&dev, 9));
    assert(wave5_vpu_inject_timeout(&dev, 8));
    assert(!wave5_vpu_inject_timeout(&dev, 8));
    scheduler.current=&a;
    complete_during_wait=&scheduler;
    wave5_vpu_job_abort(&a);
    assert(!dev.isolated && !finishes);
    complete_during_wait=NULL;
    scheduler.current=&a;
    mutex_lock(&dev.irq_lock);
    jiffies=100; wave5_vpu_job_start(&a);
    mutex_unlock(&dev.irq_lock);
    jiffies=200;
    dev.job_timeout_work.work.fn(&dev.job_timeout_work.work);
    assert(!dev.isolated && dev.job_timeout_work.delay==4900);
    /* An abort before device_run starts must still release the scheduler. */
    wave5_vpu_job_abort(&a);
    assert(dev.isolated && finishes==1 && wakes==1 && pins==1 && disables==1);
    wave5_vpu_isolate(&dev, "second error must not overwrite first", 123);
    assert(pins==1 && !dev.isolation_command);
    dev.isolation_work.fn(&dev.isolation_work);
    assert(events==1 && finishes==1 && contexts[0].out_q_ctx.q.error &&
           contexts[0].cap_q_ctx.q.error && contexts[1].out_q_ctx.q.error &&
           contexts[1].cap_q_ctx.q.error && a.irq_done && b.irq_done);
    /* A late callback cannot finish another/nonexistent scheduler job. */
    mutex_lock(&dev.irq_lock); wave5_vpu_job_finish(&a); mutex_unlock(&dev.irq_lock);
    assert(finishes==1);
    dev.isolated=0; scheduler.current=&a;
    jiffies=10000; dev.job_deadline=9999;
    dev.job_timeout_work.work.fn(&dev.job_timeout_work.work);
    assert(dev.isolated);
    dev.isolation_work.fn(&dev.isolation_work);
    assert(finishes==2);

    /* MMAP/export references may disappear after quarantine, but ours stays. */
    dev.isolated=0;
    struct vb2_queue q={.priv=&a}; struct vb2_buffer vb={.vb2_queue=&q};
    struct wave5_plane *m=wave5_plane_alloc(&vb, &device, 4096);
    assert(!IS_ERR(m));
    struct allocation *original=m->memory;
    wave5_plane_mmap(m, NULL); wave5_plane_export(&vb, m, 0);
    assert(wave5_plane_users(m)==3 && wave5_plane_cookie(&vb,m)==&original->addr);
    dev.isolated=1;
    wave5_plane_put(m); put_plane(original); put_plane(original);
    assert(!frees && original->refs==1);
    dev.isolated=0; wave5_plane_put(m); assert(frees==1);

    struct dma_buf buf={.refs=1};
    m=wave5_plane_attach(&vb, &device, &buf, 4096);
    original=m->memory;
    assert(buf.refs==2 && !wave5_plane_map(m));
    wave5_plane_prepare(m);
    dev.isolated=1;
    wave5_plane_finish(m); wave5_plane_unmap(m); wave5_plane_detach(m);
    dma_buf_put(&buf); /* vb2 drops its own import ref; the independent ref stays. */
    assert(buf.refs==1 && original->maps==1 && !unmaps && !detaches && !syncs);
    assert(wave5_plane_map(m)==-EIO && prepares==1);
    dev.isolated=0; wave5_plane_unmap(m); wave5_plane_detach(m);
    assert(!buf.refs && frees==2 && unmaps==1 && detaches==1);
    puts("Wave5 isolation: scheduler cancel races, all-instance errors, late IRQ, MMAP/export/import retention passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-isolation-') as directory:
    code = Path(directory) / 'isolation.c'
    binary = Path(directory) / 'isolation'
    code.write_text(prefix + memops + allocator + logic + planes + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
