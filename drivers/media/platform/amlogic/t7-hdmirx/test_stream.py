#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run actual IRQ/ring/vb2 C code with deterministic hardware and CPU-copy gates."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-vdin-test.c').read_text()
source = source.replace('#include "t7-vdin-hdr.h"', Path(__file__).with_name('t7-vdin-hdr.h').read_text())
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define '))
types = source[source.index('enum t7_vdin_slot_state {'):source.index('static u32 t7_vdin_read')]
hardware = source[source.index('static u32 t7_vdin_read'):source.index('static int t7_vdin_capture')]
stream = source[source.index('static struct t7_vdin_buffer *t7_vdin_next_buffer(struct t7_vdin *vdin)\n{'):
                source.index('static const struct vb2_ops t7_vdin_queue_ops')]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
typedef pthread_mutex_t spinlock_t;
typedef int irqreturn_t;
#define VDIN_FRAME_SIZE (1920U*1080U*3U)
#define module_param(...)
#define MODULE_PARM_DESC(...)
#define __iomem
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((~0U>>(31-(h))) & (~0U<<(l)))
#define IRQ_HANDLED 1
#define THIS_MODULE NULL
#define READ_ONCE(x) __atomic_load_n(&(x),__ATOMIC_ACQUIRE)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define IS_ERR(p) ((p)==(void *)-ENOMEM)
#define PTR_ERR(p) (-ENOMEM)
#define spin_lock_irqsave(l,f) ((void)((f)=0),pthread_mutex_lock(l))
#define spin_unlock_irqrestore(l,f) ((void)(f),pthread_mutex_unlock(l))
struct mutex { pthread_mutex_t native; };
struct delayed_work { int unused; };
#define mutex_lock(l) pthread_mutex_lock(&(l)->native)
#define mutex_unlock(l) pthread_mutex_unlock(&(l)->native)
#define dev_err(...) ((void)0)
#define msecs_to_jiffies(x) (x)
#define DMA_FROM_DEVICE 1
#define dma_wmb() ((void)0)
#define dma_rmb() ((void)0)
#define O_RDWR 2
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define V4L2_FIELD_NONE 1
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; unsigned long generation; } wait_queue_head_t;
static bool expire;
#define fake_wait(w,condition,timed) ({ \
    long result=0; \
    for (;;) { \
        pthread_mutex_lock(&(w).lock); unsigned long generation=(w).generation; \
        pthread_mutex_unlock(&(w).lock); \
        if (condition) { result=1; break; } \
        if ((timed) && READ_ONCE(expire)) break; \
        pthread_mutex_lock(&(w).lock); \
        while (generation==(w).generation) pthread_cond_wait(&(w).cond,&(w).lock); \
        pthread_mutex_unlock(&(w).lock); \
    } result; \
})
#define wait_event_interruptible(w,condition) ((void)fake_wait(w,condition,false))
#define wait_event_interruptible_timeout(w,condition,limit) fake_wait(w,condition,true)
static void wake_up(wait_queue_head_t *w) {
    pthread_mutex_lock(&w->lock); w->generation++; pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
}
struct list_head { struct list_head *next,*prev; };
static bool list_empty(struct list_head *h) { return h->next==h; }
static void list_add_tail(struct list_head *n,struct list_head *h) {
    n->prev=h->prev; n->next=h; h->prev->next=n; h->prev=n;
}
static void list_del(struct list_head *n) { n->prev->next=n->next; n->next->prev=n->prev; }
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
struct device { int unused; };
struct clk_bulk_data { int unused; };
struct kref { int unused; };
struct v4l2_device { int unused; };
struct video_device { int unused; };
struct allocation { unsigned char *data; unsigned int refs; };
struct dma_buf { struct allocation *allocation; };
struct vb2_buffer;
struct vb2_mem_ops { struct dma_buf *(*get_dmabuf)(struct vb2_buffer *,void *,unsigned long); };
struct vb2_queue { void *priv; bool error; const struct vb2_mem_ops *mem_ops; };
struct vb2_buffer { struct vb2_queue *vb2_queue; unsigned long timestamp;
    unsigned char *data; size_t payload,size; int state,done;
    struct { void *mem_priv; } planes[1]; dma_addr_t dma; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; unsigned int sequence,field; };
#define to_vb2_v4l2_buffer(p) container_of(p,struct vb2_v4l2_buffer,vb2_buf)
enum vb2_buffer_state { VB2_BUF_STATE_QUEUED=1,VB2_BUF_STATE_ERROR,VB2_BUF_STATE_DONE };
struct completion { wait_queue_head_t wait; bool done; };
struct task_struct { pthread_t thread; int (*fn)(void *); void *arg; bool stop; };
'''
stubs = r'''
static uint8_t registers[0x40000];
static struct t7_vdin receiver;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned int started,entered,permits,completed,pins,stop_waiting;
static bool stopping,fail_timing,cpu_owned[VDIN_DMA_SLOTS];
static unsigned int live_leases;
static bool fail_lease;
static void allocation_put(struct allocation *memory) {
    assert(memory && memory->refs);
    if (!--memory->refs) { free(memory->data); free(memory); }
}
static struct dma_buf *get_dmabuf(struct vb2_buffer *vb,void *mem,unsigned long flags) {
    if (fail_lease) return NULL;
    struct dma_buf *lease=calloc(1,sizeof(*lease)); assert(lease);
    lease->allocation=mem; lease->allocation->refs++;
    live_leases++; return lease;
}
static void dma_buf_put(struct dma_buf *lease) {
    assert(lease && live_leases); live_leases--; allocation_put(lease->allocation); free(lease);
}
static dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *vb,int plane) { return vb->dma; }
static const struct vb2_mem_ops mem_ops={.get_dmabuf=get_dmabuf};
static _Thread_local struct task_struct *current;
static u32 readl(void *address) { u32 v; memcpy(&v,address,4); return v; }
static void writel(u32 value,void *address) {
    if (address==registers+VDIN_WR_CTRL*4 && value&VDIN_WR_REQ &&
        !(readl(address)&VDIN_WR_REQ)) {
        int index=receiver.active_slot;
        assert(receiver.slots[index].state==VDIN_SLOT_WRITING && !cpu_owned[index]);
        dma_addr_t address=receiver.direct_dma ? receiver.slots[index].buffer->dma : receiver.slots[index].dma;
        if (receiver.direct_dma) {
            struct t7_vdin_buffer *b=receiver.slots[index].buffer;
            assert(b->lease && b->hardware_used && !b->vb.vb2_buf.done);
        }
        assert(readl(registers+VDIN_WR_BADDR*4)==address>>4);
        u32 busy=0; memcpy(registers+VDIN_WR_STATUS*4,&busy,4);
        pthread_mutex_lock(&gate); started++; pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&gate);
    }
    memcpy(address,&value,4);
}
static void udelay(unsigned int delay) { assert(delay==1); }
static u64 ktime_get_ns(void) { static u64 ticks; return ++ticks; }
static void __module_get(void *module) { pins++; }
static void synchronize_irq(int irq) { }
#define readl_poll_timeout(address,value,condition,interval,limit) \
    ((value=readl(address)),(condition)?0:-ETIMEDOUT)
static void complete(struct completion *c) {
    pthread_mutex_lock(&c->wait.lock); c->done=true;
    pthread_cond_broadcast(&c->wait.cond); pthread_mutex_unlock(&c->wait.lock);
}
static void reinit_completion(struct completion *c) {
    pthread_mutex_lock(&c->wait.lock); c->done=false; pthread_mutex_unlock(&c->wait.lock);
}
static unsigned long wait_for_completion_timeout(struct completion *c,unsigned long limit) {
    pthread_mutex_lock(&gate); stop_waiting++; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    pthread_mutex_lock(&c->wait.lock);
    while (!c->done && !READ_ONCE(expire)) pthread_cond_wait(&c->wait.cond,&c->wait.lock);
    bool done=c->done; c->done=false; pthread_mutex_unlock(&c->wait.lock); return done;
}
static bool kthread_should_stop(void) { return READ_ONCE(current->stop); }
static void *thread_main(void *p) { current=p; current->fn(current->arg); return NULL; }
static struct task_struct *kthread_run(int (*fn)(void *),void *arg,const char *name) {
    struct task_struct *t=calloc(1,sizeof(*t)); assert(t); t->fn=fn; t->arg=arg;
    assert(!pthread_create(&t->thread,NULL,thread_main,t)); return t;
}
static void kthread_stop(struct task_struct *t) {
    __atomic_store_n(&t->stop,true,__ATOMIC_RELEASE);
    wake_up(&((struct t7_vdin *)t->arg)->buffer_wait);
    pthread_mutex_lock(&gate); stopping=true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    assert(!pthread_join(t->thread,NULL)); free(t);
}
static void *vb2_get_drv_priv(struct vb2_queue *q) { return q->priv; }
static void *vb2_plane_vaddr(struct vb2_buffer *b,int p) { return b->data; }
static size_t vb2_plane_size(struct vb2_buffer *b,int p) { return b->size; }
static void vb2_set_plane_payload(struct vb2_buffer *b,int p,size_t n) { b->payload=n; }
static void vb2_buffer_done(struct vb2_buffer *b,enum vb2_buffer_state state) {
    pthread_mutex_lock(&gate); assert(!b->done);
    b->state=state; b->done=1; completed++; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
}
static void vb2_queue_error(struct vb2_queue *q) {
    pthread_mutex_lock(&gate); q->error=true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
}
static int t7_hdmirx_check_video(void *dev) { return fail_timing?-ENOLINK:0; }
static unsigned int receiver_users;
static bool t7_hdmirx_stream_is_hdr(void *dev) { (void)dev; return false; }
static int t7_hdmirx_stream_get(void *dev, unsigned int width, unsigned int height) {
    int ret=t7_hdmirx_check_video(dev);
    if (!ret) receiver_users++;
    return ret;
}
static void t7_hdmirx_stream_put(void *dev) {
    (void)dev;
    assert(receiver_users && !receiver.ring_running && !receiver.armed);
    receiver_users--;
}
static int t7_hdmirx_check_video_now(void *dev) {
    bool copying=false;
    for (int i=0;i<VDIN_DMA_SLOTS;i++) copying|=cpu_owned[i];
    if (!copying && !receiver.direct_dma) {
        pthread_mutex_lock(&gate); unsigned int n=++entered;
        pthread_cond_broadcast(&changed);
        while (permits<n) pthread_cond_wait(&changed,&gate);
        pthread_mutex_unlock(&gate);
    }
    return fail_timing?-ENOLINK:0;
}
static void dma_sync_single_for_cpu(struct device *dev,dma_addr_t dma,size_t size,int dir) {
    int i=(dma-0x10000000)/0x1000000;
    assert(size==VDIN_FRAME_SIZE && dir==DMA_FROM_DEVICE);
    assert(receiver.slots[i].state==VDIN_SLOT_COPYING && !cpu_owned[i]); cpu_owned[i]=true;
}
static void dma_sync_single_for_device(struct device *dev,dma_addr_t dma,size_t size,int dir) {
    int i=(dma-0x10000000)/0x1000000;
    assert(size==VDIN_FRAME_SIZE && dir==DMA_FROM_DEVICE);
    assert(receiver.slots[i].state==VDIN_SLOT_COPYING && cpu_owned[i]); cpu_owned[i]=false;
}
'''
checks = r'''
static void permit(unsigned int n) {
    pthread_mutex_lock(&gate); permits=n; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
}
static void await(unsigned int *counter,unsigned int target) {
    pthread_mutex_lock(&gate);
    while (*counter<target) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
}
static void step(unsigned char pixel,bool busy) {
    pthread_mutex_lock(&receiver.irq_lock);
    assert(receiver.ring_running && readl(registers+VDIN_WR_CTRL*4)&VDIN_WR_REQ);
    int index=receiver.active_slot;
    assert(!cpu_owned[index] && receiver.slots[index].state==VDIN_SLOT_WRITING);
    void *output=receiver.direct_dma ? receiver.slots[index].buffer->vb.vb2_buf.data : receiver.slots[index].data;
    memset(output,pixel,VDIN_FRAME_SIZE);
    u32 done=busy?0:1; memcpy(registers+VDIN_WR_STATUS*4,&done,4);
    pthread_mutex_unlock(&receiver.irq_lock);
    t7_vdin_write_done(1,&receiver);
}
static void *stop_stream(void *unused) { t7_vdin_stop_streaming(&receiver.queue); return NULL; }
static void init_wait(wait_queue_head_t *w) {
    pthread_mutex_init(&w->lock,NULL); pthread_cond_init(&w->cond,NULL);
}
static void destroy_wait(wait_queue_head_t *w) {
    pthread_mutex_destroy(&w->lock); pthread_cond_destroy(&w->cond);
}
static void setup(struct t7_vdin_buffer b[4]) {
    receiver_users=0;
    memset(&receiver,0,sizeof(receiver)); memset(b,0,4*sizeof(*b));
    memset(registers,0,sizeof(registers)); memset(cpu_owned,0,sizeof(cpu_owned));
    pthread_mutex_init(&receiver.lock.native,NULL); pthread_mutex_init(&receiver.queue_lock,NULL);
    pthread_mutex_init(&receiver.irq_lock,NULL);
    init_wait(&receiver.buffer_wait); init_wait(&receiver.done.wait);
    receiver.queued.next=receiver.queued.prev=&receiver.queued;
    receiver.queue.priv=&receiver; receiver.queue.mem_ops=&mem_ops; receiver.regs=registers;
    receiver.width=1920; receiver.height=1080; receiver.stride=5760;
    receiver.frame_size=VDIN_FRAME_SIZE;
    receiver.allocation_size=VDIN_FRAME_SIZE;
    started=entered=permits=completed=pins=stop_waiting=0;
    stopping=fail_timing=expire=false;
    for (int i=0;i<VDIN_DMA_SLOTS;i++) {
        receiver.slots[i].data=malloc(VDIN_FRAME_SIZE); assert(receiver.slots[i].data);
        receiver.slots[i].dma=0x10000000+i*0x1000000;
    }
    for (int i=0;i<4;i++) {
        b[i].vb.vb2_buf.vb2_queue=&receiver.queue; b[i].vb.vb2_buf.size=VDIN_FRAME_SIZE;
        struct allocation *memory=calloc(1,sizeof(*memory)); assert(memory);
        memory->refs=1; memory->data=calloc(1,VDIN_FRAME_SIZE); assert(memory->data);
        b[i].vb.vb2_buf.planes[0].mem_priv=memory; b[i].vb.vb2_buf.data=memory->data;
        assert(!t7_vdin_buffer_prepare(&b[i].vb.vb2_buf));
        t7_vdin_buffer_queue(&b[i].vb.vb2_buf);
    }
}
static void teardown(struct t7_vdin_buffer b[4]) {
    assert(receiver.receiver_lease ? receiver.faulted && receiver_users==1 : receiver_users==0);
    assert(list_empty(&receiver.queued) && !receiver.streaming && !receiver.thread);
    for (int i=0;i<VDIN_DMA_SLOTS;i++) { assert(!cpu_owned[i]); free(receiver.slots[i].data); }
    for (int i=0;i<4;i++) {
        t7_vdin_buffer_cleanup(&b[i].vb.vb2_buf);
        allocation_put(b[i].vb.vb2_buf.planes[0].mem_priv);
    }
    pthread_mutex_destroy(&receiver.lock.native); pthread_mutex_destroy(&receiver.queue_lock);
    pthread_mutex_destroy(&receiver.irq_lock);
    destroy_wait(&receiver.buffer_wait); destroy_wait(&receiver.done.wait);
}
static void setup_direct(struct t7_vdin_buffer b[4]) {
    setup(b); receiver.direct_dma=true;
    for (int i=0;i<VDIN_DMA_SLOTS;i++) {
        free(receiver.slots[i].data); receiver.slots[i].data=NULL;
        receiver.slots[i].dma=0;
    }
    for (int i=0;i<4;i++) {
        b[i].vb.vb2_buf.dma=0x20000000+i*0x1000000;
        assert(!t7_vdin_buffer_init(&b[i].vb.vb2_buf));
    }
    assert(live_leases==4);
}
static void check_direct(void) {
    struct t7_vdin_buffer b[4]; pthread_t stop;
    /* Held/exported buffers are never selected again until explicitly queued. */
    setup_direct(b); assert(!t7_vdin_start_streaming(&receiver.queue,4)); await(&started,1);
    step(1,false); step(2,false); await(&completed,1);
    for (unsigned int i=1;i<4;i++) { step(i+2,false); await(&completed,i+1); }
    assert(!receiver.ring_running && receiver.dma_idle && !pins);
    for (int i=0;i<4;i++) assert(b[i].vb.vb2_buf.data[VDIN_FRAME_SIZE-1]==i+2);
    b[0].vb.vb2_buf.done=0; t7_vdin_buffer_queue(&b[0].vb.vb2_buf); await(&started,6);
    step(6,false); step(7,false); await(&completed,5);
    assert(b[0].vb.sequence==4 && b[0].vb.vb2_buf.data[VDIN_FRAME_SIZE-1]==7);
    for (int i=1;i<4;i++) assert(b[i].vb.vb2_buf.data[VDIN_FRAME_SIZE-1]==i+2);
    t7_vdin_stop_streaming(&receiver.queue); assert(!receiver.copied_bytes);
    teardown(b); assert(!live_leases);
    /* STREAMOFF cannot return a direct buffer before the final write response. */
    setup_direct(b); assert(!t7_vdin_start_streaming(&receiver.queue,4)); await(&started,1);
    assert(!pthread_create(&stop,NULL,stop_stream,NULL)); await(&stop_waiting,1);
    assert(!completed); step(1,false); pthread_join(stop,NULL);
    assert(completed==4 && !pins); teardown(b); assert(!live_leases);
    /* Source loss may suppress the IRQ even after the final AXI response. */
    setup_direct(b); assert(!t7_vdin_start_streaming(&receiver.queue,4)); await(&started,1);
    u32 drained = BIT(0);
    memcpy(registers+VDIN_WR_STATUS*4,&drained,4);
    assert(!receiver.dma_idle);
    __atomic_store_n(&expire,true,__ATOMIC_RELEASE); wake_up(&receiver.buffer_wait);
    await(&completed,4); t7_vdin_stop_streaming(&receiver.queue);
    assert(!receiver.faulted && !pins && !receiver.receiver_lease);
    for (int i=0;i<4;i++) assert(b[i].vb.vb2_buf.state==VB2_BUF_STATE_ERROR);
    teardown(b); assert(!live_leases);
    /* Busy IRQ / missing IRQ: retain used allocator memory despite vb2 cleanup. */
    for (int missing=0;missing<2;missing++) {
        setup_direct(b); assert(!t7_vdin_start_streaming(&receiver.queue,4)); await(&started,1);
        if (missing) { __atomic_store_n(&expire,true,__ATOMIC_RELEASE); wake_up(&receiver.buffer_wait); }
        else step(1,true);
        await(&completed,4); t7_vdin_stop_streaming(&receiver.queue);
        assert(receiver.faulted && pins==1 && receiver.queue.error);
        unsigned int buffers=4, planes=0, sizes[1]={0};
        for (int retry=0;retry<16;retry++)
            assert(t7_vdin_queue_setup(&receiver.queue,&buffers,&planes,sizes,NULL)==-EIO);
        struct dma_buf *quarantined=b[0].lease;
        for (int i=0;i<4;i++) assert(b[i].vb.vb2_buf.state==VB2_BUF_STATE_ERROR);
        teardown(b); assert(live_leases==1 && quarantined->allocation->refs==1);
        memset(b,0xdd,sizeof(b)); /* vb2 metadata is gone; hardware allocation is not */
        memset(quarantined->allocation->data,0xa5,VDIN_FRAME_SIZE);
        assert(quarantined->allocation->data[VDIN_FRAME_SIZE-1]==0xa5);
        dma_buf_put(quarantined); /* harness teardown only: real driver retains until reboot */
    }
    /* A failed pre-acquired lease never supplies an address to hardware. */
    setup(b); receiver.direct_dma=true; fail_lease=true;
    assert(t7_vdin_buffer_init(&b[0].vb.vb2_buf)==-ENOMEM && !b[0].lease && !started);
    fail_lease=false; t7_vdin_return_buffers(&receiver,VB2_BUF_STATE_QUEUED); teardown(b);
    assert(!live_leases);
}
int main(void) {
    struct t7_vdin_buffer b[4]; pthread_t stop;
    /* Import tail capacity must not enlarge the visible bytesused payload. */
    receiver.queue.priv=&receiver;
    receiver.frame_size=1920*1080*3/2;
    receiver.allocation_size=1920*1088*3/2;
    struct vb2_buffer padded={.vb2_queue=&receiver.queue,.size=receiver.frame_size};
    assert(t7_vdin_buffer_prepare(&padded)==-EINVAL);
    padded.size=receiver.allocation_size;
    assert(!t7_vdin_buffer_prepare(&padded) && padded.payload==receiver.frame_size);
    unsigned int planes=0,count=4,sizes[1]={0};
    assert(!t7_vdin_queue_setup(&receiver.queue,&count,&planes,sizes,NULL));
    assert(planes==1 && sizes[0]==receiver.allocation_size);
    /* Hardware fills the ring while CPU copy is blocked, without reusing COPYING. */
    setup(b); assert(!t7_vdin_start_streaming(&receiver.queue,4)); await(&started,1);
    step(1,false); step(2,false); await(&entered,1);
    step(3,false); step(4,false);
    assert(!receiver.ring_running && receiver.dma_idle && receiver.ring_pauses==1);
    assert(receiver.slots[0].state==VDIN_SLOT_COPYING);
    permit(3); await(&completed,3);
    assert(receiver.ring_running); step(5,false); step(6,false);
    permit(4); await(&completed,4);
    const unsigned char expected[4]={2,3,4,6};
    for (int i=0;i<4;i++) {
        assert(b[i].vb.sequence==(unsigned)i && b[i].vb.vb2_buf.data[VDIN_FRAME_SIZE-1]==expected[i]);
    }
    step(7,false); step(8,false); step(9,false);
    assert(!receiver.ring_running && receiver.ring_pauses==2);
    assert(b[0].vb.vb2_buf.data[VDIN_FRAME_SIZE-1]==2);
    t7_vdin_stop_streaming(&receiver.queue); assert(!pins); teardown(b);
    /* STREAMOFF waits for CPU copy and then a hardware completion boundary. */
    setup(b); assert(!t7_vdin_start_streaming(&receiver.queue,4)); await(&started,1);
    step(1,false); step(2,false); await(&entered,1);
    assert(!pthread_create(&stop,NULL,stop_stream,NULL));
    pthread_mutex_lock(&gate); while (!stopping) pthread_cond_wait(&changed,&gate);
    assert(!completed); pthread_mutex_unlock(&gate);
    permit(1); await(&stop_waiting,1); step(3,false); pthread_join(stop,NULL);
    assert(completed==4 && b[0].vb.vb2_buf.state==VB2_BUF_STATE_DONE && !pins);
    for (int i=1;i<4;i++) assert(b[i].vb.vb2_buf.state==VB2_BUF_STATE_ERROR);
    teardown(b);
    /* A late/busy write interrupt must not expose a DMA-owned buffer to the CPU. */
    setup(b); assert(!t7_vdin_start_streaming(&receiver.queue,4)); await(&started,1);
    step(1,true); await(&completed,4); t7_vdin_stop_streaming(&receiver.queue);
    assert(receiver.queue.error && receiver.faulted && pins==1 && !entered);
    for (int i=0;i<4;i++) assert(b[i].vb.vb2_buf.state==VB2_BUF_STATE_ERROR);
    teardown(b);
    /* A missing IRQ follows the bounded-stop fault path without a CPU handoff. */
    setup(b); assert(!t7_vdin_start_streaming(&receiver.queue,4)); await(&started,1);
    __atomic_store_n(&expire,true,__ATOMIC_RELEASE); wake_up(&receiver.buffer_wait);
    await(&completed,4); t7_vdin_stop_streaming(&receiver.queue);
    assert(receiver.queue.error && receiver.faulted && pins==1 && !entered);
    teardown(b);
    /* Oldest-ready selection is independent of slot index and handles u32 wrap. */
    setup(b);
    for (int i=0;i<VDIN_DMA_SLOTS;i++) {
        receiver.slots[i].state=VDIN_SLOT_READY;
        receiver.slots[i].sequence=0U-i;
    }
    assert(t7_vdin_ring_take(&receiver)==&receiver.slots[2]);
    assert(t7_vdin_ring_take(&receiver)==&receiver.slots[1]);
    assert(t7_vdin_ring_take(&receiver)==&receiver.slots[0]);
    t7_vdin_return_buffers(&receiver,VB2_BUF_STATE_QUEUED); teardown(b);
    setup(b); fail_timing=true;
    assert(t7_vdin_start_streaming(&receiver.queue,4)==-ENOLINK && !started);
    for (int i=0;i<4;i++) assert(b[i].vb.vb2_buf.state==VB2_BUF_STATE_QUEUED);
    teardown(b);
    setup(b); receiver.faulted=true;
    assert(t7_vdin_start_streaming(&receiver.queue,4)==-EIO && !started);
    teardown(b);
    check_direct();
    puts("DMA ring/direct leases: ownership, starvation, in-flight stop, busy IRQ and quarantine passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'stream.c'
    binary = Path(directory) / 'stream'
    code.write_text(prefix + defines + '\n' + types + stubs + hardware + stream + checks)
    subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-O1', '-g', '-fsanitize=address,undefined',
                    '-Wno-unused-parameter', '-Wno-unused-function', str(code),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
