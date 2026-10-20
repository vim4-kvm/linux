#!/usr/bin/env python3
"""Exercise the actual vb2 worker with deterministic pthread capture/stop gates."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-vdin-test.c').read_text()
functions = source[source.index('static struct t7_vdin_buffer *t7_vdin_next_buffer'):
                   source.index('static const struct vb2_ops t7_vdin_queue_ops')]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VDIN_FRAME_SIZE 64
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define IS_ERR(p) ((p) == (void *)-ENOMEM)
#define PTR_ERR(p) (-ENOMEM)
#define spin_lock_irqsave(l,f) ((void)((f)=0), pthread_mutex_lock(l))
#define spin_unlock_irqrestore(l,f) ((void)(f), pthread_mutex_unlock(l))
#define mutex_lock(l) pthread_mutex_lock(l)
#define mutex_unlock(l) pthread_mutex_unlock(l)
#define V4L2_FIELD_NONE 1
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; } wait_queue_head_t;
#define wait_event_interruptible(w, condition) do { \
    pthread_mutex_lock(&(w).lock); \
    while (!(condition)) pthread_cond_wait(&(w).cond, &(w).lock); \
    pthread_mutex_unlock(&(w).lock); \
} while (0)
static void wake_up(wait_queue_head_t *w) {
    pthread_mutex_lock(&w->lock);
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
}
struct list_head { struct list_head *next, *prev; };
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h) {
    n->prev=h->prev; n->next=h; h->prev->next=n; h->prev=n;
}
static void list_del(struct list_head *n) { n->prev->next=n->next; n->next->prev=n->prev; }
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
struct device;
struct vb2_queue { void *priv; bool error; };
struct vb2_buffer { struct vb2_queue *vb2_queue; unsigned long timestamp;
                    unsigned char data[VDIN_FRAME_SIZE]; size_t payload, size; int state, done; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; unsigned int sequence, field; };
struct t7_vdin_buffer { struct vb2_v4l2_buffer vb; struct list_head list; };
#define to_vb2_v4l2_buffer(p) container_of(p,struct vb2_v4l2_buffer,vb2_buf)
enum vb2_buffer_state { VB2_BUF_STATE_QUEUED=1, VB2_BUF_STATE_ERROR, VB2_BUF_STATE_DONE };
struct task_struct { pthread_t thread; int (*fn)(void *); void *arg; bool stop; };
struct t7_vdin {
    pthread_mutex_t queue_lock, lock;
    struct list_head queued;
    wait_queue_head_t buffer_wait;
    struct vb2_queue queue;
    struct task_struct *thread;
    bool faulted, streaming;
    void *receiver, *frame;
    unsigned int sequence;
    unsigned long frame_timestamp;
};
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned int entered, permits, completed;
static bool fail_capture, fail_timing, stopping;
static _Thread_local struct task_struct *current;
static bool kthread_should_stop(void) {
    return __atomic_load_n(&current->stop, __ATOMIC_ACQUIRE);
}
static void *thread_main(void *p) {
    current=p; current->fn(current->arg); return NULL;
}
static struct task_struct *kthread_run(int (*fn)(void *), void *arg, const char *name) {
    struct task_struct *t=calloc(1,sizeof(*t));
    assert(t); t->fn=fn; t->arg=arg;
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
static void *vb2_plane_vaddr(struct vb2_buffer *b, int p) { return b->data; }
static size_t vb2_plane_size(struct vb2_buffer *b, int p) { return b->size; }
static void vb2_set_plane_payload(struct vb2_buffer *b, int p, size_t n) { b->payload=n; }
static void vb2_buffer_done(struct vb2_buffer *b, enum vb2_buffer_state state) {
    pthread_mutex_lock(&gate);
    assert(!b->done); b->state=state; b->done=1; completed++;
    pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
}
static void vb2_queue_error(struct vb2_queue *q) {
    pthread_mutex_lock(&gate); q->error=true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
}
static int t7_hdmirx_check_1080p(void *receiver) { return fail_timing ? -ENOLINK : 0; }
static int t7_vdin_capture(struct t7_vdin *v) {
    pthread_mutex_lock(&gate); unsigned int n=++entered;
    pthread_cond_broadcast(&changed);
    while (permits<n) pthread_cond_wait(&changed,&gate);
    bool fail=fail_capture;
    pthread_mutex_unlock(&gate);
    if (fail) { v->faulted=true; return -ETIMEDOUT; }
    memset(v->frame,(int)n,VDIN_FRAME_SIZE); v->frame_timestamp=n; return 0;
}
'''
checks = r'''
static void permit(unsigned int n) {
    pthread_mutex_lock(&gate); permits=n; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
}
static void await(unsigned int *counter, unsigned int target) {
    pthread_mutex_lock(&gate);
    while (*counter<target) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
}
static void *stop_stream(void *data) {
    t7_vdin_stop_streaming(&((struct t7_vdin *)data)->queue); return NULL;
}
static void setup(struct t7_vdin *v, struct t7_vdin_buffer b[4], unsigned char frame[64]) {
    memset(v,0,sizeof(*v)); memset(b,0,4*sizeof(*b));
    pthread_mutex_init(&v->lock,NULL); pthread_mutex_init(&v->queue_lock,NULL);
    pthread_mutex_init(&v->buffer_wait.lock,NULL); pthread_cond_init(&v->buffer_wait.cond,NULL);
    v->queued.next=v->queued.prev=&v->queued; v->queue.priv=v; v->frame=frame;
    entered=permits=completed=0; fail_capture=fail_timing=stopping=false;
    for (int i=0;i<4;i++) {
        b[i].vb.vb2_buf.vb2_queue=&v->queue;
        b[i].vb.vb2_buf.size=64;
        assert(!t7_vdin_buffer_prepare(&b[i].vb.vb2_buf));
        t7_vdin_buffer_queue(&b[i].vb.vb2_buf);
    }
}
static void teardown(struct t7_vdin *v) {
    assert(list_empty(&v->queued) && !v->streaming && !v->thread);
    pthread_mutex_destroy(&v->lock); pthread_mutex_destroy(&v->queue_lock);
    pthread_mutex_destroy(&v->buffer_wait.lock); pthread_cond_destroy(&v->buffer_wait.cond);
}
int main(void) {
    struct t7_vdin v; struct t7_vdin_buffer b[4]; unsigned char frame[64]; pthread_t stop;
    /* STREAMOFF cannot return the in-flight CPU destination before copy finishes. */
    setup(&v,b,frame); assert(!t7_vdin_start_streaming(&v.queue,4)); await(&entered,1);
    assert(!pthread_create(&stop,NULL,stop_stream,&v));
    pthread_mutex_lock(&gate); while (!stopping) pthread_cond_wait(&changed,&gate);
    assert(!completed); pthread_mutex_unlock(&gate);
    permit(1); pthread_join(stop,NULL);
    assert(completed==4 && b[0].vb.vb2_buf.state==VB2_BUF_STATE_DONE);
    for (int i=1;i<4;i++) assert(b[i].vb.vb2_buf.state==VB2_BUF_STATE_ERROR);
    assert(b[0].vb.vb2_buf.data[63]==1); teardown(&v);
    /* Queue starvation performs no capture and held destinations remain immutable. */
    setup(&v,b,frame); permit(4); assert(!t7_vdin_start_streaming(&v.queue,4));
    await(&completed,4); assert(entered==4 && !t7_vdin_has_buffer(&v));
    for (int i=0;i<4;i++) {
        assert(b[i].vb.sequence==(unsigned)i && b[i].vb.vb2_buf.data[63]==i+1);
    }
    b[0].vb.vb2_buf.done=0; t7_vdin_buffer_queue(&b[0].vb.vb2_buf);
    await(&entered,5); permit(5); await(&completed,5);
    assert(b[0].vb.sequence==4 && b[0].vb.vb2_buf.data[63]==5);
    assert(b[1].vb.vb2_buf.data[63]==2);
    t7_vdin_stop_streaming(&v.queue); teardown(&v);
    /* Capture errors drain buffers and wake DQBUF; faulted restart cannot arm DMA. */
    setup(&v,b,frame); fail_capture=true; permit(1);
    assert(!t7_vdin_start_streaming(&v.queue,4)); await(&completed,4);
    pthread_mutex_lock(&gate); while (!v.queue.error) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate); t7_vdin_stop_streaming(&v.queue);
    for (int i=0;i<4;i++) assert(b[i].vb.vb2_buf.state==VB2_BUF_STATE_ERROR);
    assert(v.faulted); teardown(&v);
    setup(&v,b,frame); fail_timing=true;
    assert(t7_vdin_start_streaming(&v.queue,4)==-ENOLINK && !entered);
    for (int i=0;i<4;i++) assert(b[i].vb.vb2_buf.state==VB2_BUF_STATE_QUEUED);
    teardown(&v);
    setup(&v,b,frame); v.faulted=true;
    assert(t7_vdin_start_streaming(&v.queue,4)==-EIO && !entered);
    for (int i=0;i<4;i++) assert(b[i].vb.vb2_buf.state==VB2_BUF_STATE_QUEUED);
    teardown(&v);
    puts("V4L2 in-flight stop, starvation/resume, immutable held buffers and queue error passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'stream.c'
    binary = Path(directory) / 'stream'
    code.write_text(prefix + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function', str(code),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
