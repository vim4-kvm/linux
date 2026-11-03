#!/usr/bin/env python3
"""Exercise actual driver registration/handlers with the core's lock selection.

Gates hold a queue operation or an RX read until the main thread releases it;
timeouts only bound a failed test, never schedule the expected interleaving.
Requires Linux UAPI headers, a C compiler and pthreads; no board or privileges.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

directory = Path(__file__).parent
driver = (Path(sys.argv[1]) if len(sys.argv) > 1 else directory/'t7-vdin-test.c').read_text()
core = (directory.parents[2]/'v4l2-core/v4l2-ioctl.c').read_text()
format_start = 'static void t7_vdin_fill_fmt' if 'static void t7_vdin_fill_fmt' in driver else 'static int t7_vdin_get_fmt'
formats = driver[driver.index(format_start):
                 driver.index('static int t7_vdin_enum_input')]
queries = driver[driver.index('static int t7_vdin_query_dv_timings'):
                 driver.index('static int t7_vdin_set_input')]
registration = driver[driver.index('static void t7_vdin_video_unregister'):
                      driver.index('static int t7_vdin_get_control')]
registration += driver[driver.index('static int t7_vdin_video_register'):
                       driver.index('static int t7_vdin_hdr_status')]
selector = core[core.index('static struct mutex *v4l2_ioctl_get_lock'):
                core.index('/* Common ioctl debug function')]
commands = ['VIDIOC_QUERY_DV_TIMINGS', 'VIDIOC_G_PARM', 'VIDIOC_G_FMT',
            'VIDIOC_S_FMT', 'VIDIOC_STREAMOFF']
entries = []
for command in commands:
    match = re.search(r'IOCTL_INFO\(' + command + r',\s*\w+,\s*\w+,\s*(.*?)\),', core)
    if not match:
        raise RuntimeError(f'No core ioctl entry for {command}')
    entries.append(f'[_IOC_NR({command})] = {{ .flags = {match[1]} }},')

prefix = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define max(a,b) ((a)>(b)?(a):(b))
#define GFP_KERNEL 0
typedef uintptr_t dma_addr_t;
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <linux/videodev2.h>
typedef uint32_t u32;
static bool nv12 = true;
struct v4l2_ctrl_handler { int error; };
struct v4l2_ctrl { unsigned int flags; };
static const int t7_vdin_control_ops;
static const int t7_vdin_vrr_control;
#define v4l2_ctrl_new_custom(handler, config, priv) ((void)(config))
#define v4l2_ctrl_ptr_create(p) (p)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static struct v4l2_ctrl *v4l2_ctrl_new_std_compound(
    struct v4l2_ctrl_handler *h, const void *ops, unsigned int id,
    void *value, void *minimum, void *maximum) {
    static struct v4l2_ctrl control;
    return &control;
}
struct mutex { pthread_mutex_t native; };
struct t7_vdin_buffer { int unused; };
struct device { int refs; };
struct v4l2_device { int unused; };
struct vb2_queue {
    unsigned int type,io_modes,buf_struct_size,timestamp_flags,min_queued_buffers;
    void *drv_priv,*owner; const void *ops,*mem_ops;
    struct mutex *lock; struct device *dev; bool busy;
};
struct video_device {
    struct v4l2_ctrl_handler *ctrl_handler;
    char name[32]; struct v4l2_device *v4l2_dev;
    const void *fops,*ioctl_ops;
    void (*release)(struct video_device *);
    struct mutex *lock; struct vb2_queue *queue;
    unsigned int device_caps; void *data; atomic_bool registered;
    struct device dev;
};
struct t7_vdin {
    struct v4l2_ctrl_handler controls;
    struct mutex video_lock,queue_mutex,lock;
    struct video_device video; struct vb2_queue queue;
    struct device *dev,*receiver; struct v4l2_device v4l2; unsigned int ref;
    unsigned int width,height,pixel_format,stride,allocation_size,frame_capacity,frame_size;
    unsigned int input_width,input_height;
    unsigned int default_frame_capacity;
    struct v4l2_pix_format configured_format;
    void *frame; dma_addr_t frame_dma;
    bool nv12,faulted,direct_dma,native_hdr;
};
struct file { struct t7_vdin *data; };
struct v4l2_m2m_ctx { struct mutex *q_lock; };
struct v4l2_fh { struct v4l2_m2m_ctx *m2m_ctx; };
#define V4L2_IOCTLS 256
#define INFO_FL_QUEUE 1
#define INFO_FL_PRIO 2
#define INFO_FL_ALWAYS_COPY 4
#define INFO_FL_CLEAR(...) 0
#define VB2_MMAP 1
#define ALIGN(value,n) (((value)+(n)-1)&~((n)-1))
#define VFL_TYPE_VIDEO 1
#define strscpy(dst,src,size) snprintf(dst,size,"%s",src)
static const struct { unsigned int flags; } v4l2_ioctls[V4L2_IOCTLS] = {
'''
stubs = r'''
};
static struct t7_vdin capture;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool queue_entered, release_queue, query_done, format_done;
static bool pause_query, query_entered, release_query, remove_has_queue;
static unsigned int rx_reads, queue_releases;
static bool input_locked;
static unsigned int input_width=3840,input_height=2160;
static _Thread_local bool removing;
static void (*remove_action)(void *);
static void *remove_data;
static void mutex_lock(struct mutex *lock) {
    assert(!pthread_mutex_lock(&lock->native));
    if (removing && lock==&capture.queue_mutex) {
        pthread_mutex_lock(&gate); remove_has_queue=true;
        pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    }
}
static int __attribute__((unused)) mutex_trylock(struct mutex *lock) {
    return !pthread_mutex_trylock(&lock->native);
}
static void mutex_unlock(struct mutex *lock) { assert(!pthread_mutex_unlock(&lock->native)); }
static void *video_drvdata(struct file *file) { return file->data; }
static bool vb2_is_busy(struct vb2_queue *queue) { return queue->busy; }
static int yuv444_status;
static int t7_hdmirx_yuv444_format(struct device *dev, struct v4l2_pix_format *pix)
{
 if (!yuv444_status) {
  pix->colorspace=V4L2_COLORSPACE_REC709;
  pix->xfer_func=V4L2_XFER_FUNC_709;
  pix->ycbcr_enc=V4L2_YCBCR_ENC_709;
  pix->quantization=V4L2_QUANTIZATION_LIM_RANGE;
 }
 return yuv444_status;
}
static bool fail_allocation;
static unsigned allocations, frees;
static void *dma_alloc_coherent(struct device *dev, unsigned size, dma_addr_t *dma, int flags)
{
 if (fail_allocation) return NULL;
 void *memory=malloc(1); assert(memory); *dma=(uintptr_t)memory; allocations++; return memory;
}
static void dma_free_coherent(struct device *dev, unsigned size, void *memory, dma_addr_t dma)
{ assert(memory && (uintptr_t)memory==dma); free(memory); frees++; }
static int t7_hdmirx_query_timings(struct device *dev, struct v4l2_dv_timings *mode,
                                  struct v4l2_fract *interval) {
    (void)dev; (void)mode; (void)interval;
    pthread_mutex_lock(&gate); rx_reads++; query_entered=true;
    pthread_cond_broadcast(&changed);
    while (pause_query && !release_query) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
    if (!input_locked) return -ENOLINK;
    mode->bt.width=input_width; mode->bt.height=input_height;
    return 0;
}
static const int t7_vdin_queue_ops, vb2_dma_contig_memops;
static const int t7_vdin_video_fops, t7_vdin_ioctl_ops;
static void t7_vdin_source_start(struct t7_vdin *v) { (void)v; }
static void t7_vdin_source_stop(struct t7_vdin *v) { (void)v; }
static void t7_vdin_video_release(struct video_device *v) { (void)v; }
static void t7_vdin_put(struct t7_vdin *v) { assert(v->ref); v->ref--; }
static void kref_get(unsigned int *ref) { (*ref)++; }
static void get_device(struct device *dev) { assert(dev->refs); dev->refs++; }
static void put_device(struct device *dev) { assert(dev->refs); dev->refs--; }
static int v4l2_device_register(struct device *dev, struct v4l2_device *v) {
    (void)dev; (void)v; return 0;
}
static void v4l2_device_unregister(struct v4l2_device *v) { (void)v; }
static int vb2_queue_init(struct vb2_queue *q) { (void)q; return 0; }
static void vb2_queue_release(struct vb2_queue *q) {
    assert(!atomic_load(&capture.video.registered)); q->busy=false; queue_releases++;
}
static void video_set_drvdata(struct video_device *v, void *data) { v->data=data; }
static int video_register_device(struct video_device *v, int type, int index) {
    (void)type; (void)index; v->dev.refs=1; atomic_store(&v->registered,true); return 0;
}
static void video_unregister_device(struct video_device *v) {
    assert(atomic_exchange(&v->registered,false)); put_device(&v->dev);
}
/* Also permit the prior driver source for a behavioral red/green check. */
static void __attribute__((unused)) vb2_video_unregister_device(struct video_device *v) {
    get_device(&v->dev); video_unregister_device(v); mutex_lock(v->queue->lock);
    vb2_queue_release(v->queue); v->queue->owner=NULL; mutex_unlock(v->queue->lock);
    put_device(&v->dev);
}
static int devm_add_action_or_reset(struct device *dev, void (*action)(void *), void *data) {
    (void)dev; remove_action=action; remove_data=data; return 0;
}
'''
checks = r'''
/* Model the core's lock/check/dispatch shell, using its actual lock selector. */
static int dispatch(unsigned int cmd, void *arg) {
    struct file file = {.data=&capture};
    struct mutex *lock = v4l2_ioctl_get_lock(&capture.video,NULL,cmd,arg);
    if (lock) mutex_lock(lock);
    int result=-ENODEV;
    if (atomic_load(&capture.video.registered)) {
        switch (cmd) {
        case VIDIOC_QUERY_DV_TIMINGS: result=t7_vdin_query_dv_timings(&file,NULL,arg); break;
        case VIDIOC_G_PARM: result=t7_vdin_get_parm(&file,NULL,arg); break;
        case VIDIOC_G_FMT: result=t7_vdin_get_fmt(&file,NULL,arg); break;
        case VIDIOC_S_FMT: result=t7_vdin_set_fmt(&file,NULL,arg); break;
        case VIDIOC_STREAMOFF:
            pthread_mutex_lock(&gate); queue_entered=true; pthread_cond_broadcast(&changed);
            while (!release_queue) pthread_cond_wait(&changed,&gate);
            pthread_mutex_unlock(&gate); result=0; break;
        default: assert(false);
        }
    }
    if (lock) mutex_unlock(lock);
    return result;
}
struct call { unsigned int cmd; int result; };
static void *call_worker(void *data) {
    struct call *call=data;
    union { struct v4l2_dv_timings timing; struct v4l2_format format; } arg={0};
    call->result=dispatch(call->cmd,&arg);
    pthread_mutex_lock(&gate);
    if (call->cmd==VIDIOC_QUERY_DV_TIMINGS) query_done=true;
    if (call->cmd==VIDIOC_S_FMT) format_done=true;
    pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    return NULL;
}
static void *remove_worker(void *unused) {
    (void)unused; removing=true; remove_action(remove_data); return NULL;
}
static bool wait_for(bool *flag) {
    struct timespec end; clock_gettime(CLOCK_REALTIME,&end); end.tv_sec+=5;
    pthread_mutex_lock(&gate);
    while (!*flag) {
        int ret=pthread_cond_timedwait(&changed,&gate,&end);
        if (ret==ETIMEDOUT) break;
        assert(!ret);
    }
    bool ready=*flag; pthread_mutex_unlock(&gate); return ready;
}
int main(void) {
    pthread_mutex_init(&capture.video_lock.native,NULL);
    pthread_mutex_init(&capture.queue_mutex.native,NULL);
    pthread_mutex_init(&capture.lock.native,NULL);
    capture.width=3840; capture.height=2160; capture.stride=3840;
    capture.allocation_size=3840*2160*3/2; capture.nv12=true;
    capture.frame_capacity=capture.allocation_size;
    capture.default_frame_capacity=capture.frame_capacity;
    capture.frame=malloc(1); capture.frame_dma=(uintptr_t)capture.frame;
    capture.pixel_format=V4L2_PIX_FMT_NV12; capture.ref=1;
    assert(!t7_vdin_video_register(&capture));
    struct call stop={.cmd=VIDIOC_STREAMOFF},query={.cmd=VIDIOC_QUERY_DV_TIMINGS};
    struct call format={.cmd=VIDIOC_S_FMT};
    pthread_t a,b,c;
    pthread_create(&a,NULL,call_worker,&stop); assert(wait_for(&queue_entered));
    pthread_create(&b,NULL,call_worker,&query);
    pthread_create(&c,NULL,call_worker,&format);
    bool independent=wait_for(&query_done) && wait_for(&format_done);
    pthread_mutex_lock(&gate); release_queue=true;
    pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    pthread_join(a,NULL); pthread_join(b,NULL); pthread_join(c,NULL);
    assert(independent && query.result==-ENOLINK && format.result==-EBUSY);
    struct v4l2_format fmt={0}; input_locked=true;
    assert(!dispatch(VIDIOC_S_FMT,&fmt) && fmt.fmt.pix.width==3840);
    assert(capture.input_width==3840 && capture.input_height==2160);
    fmt.fmt.pix.width=1280; fmt.fmt.pix.height=720;
    struct file format_file={.data=&capture};
    assert(!t7_vdin_try_fmt(&format_file,NULL,&fmt));
    assert(fmt.fmt.pix.width==1280 && fmt.fmt.pix.height==720);
    assert(!dispatch(VIDIOC_S_FMT,&fmt) && capture.width==1280);
    assert(capture.input_width==3840 && capture.frame_size==1280*720*3/2);
    fmt.fmt.pix.width=3840; fmt.fmt.pix.height=2160;
    assert(!dispatch(VIDIOC_S_FMT,&fmt));
    capture.queue.busy=true; assert(dispatch(VIDIOC_S_FMT,&fmt)==-EBUSY);
    capture.queue.busy=false;
    input_width=1920; input_height=1080;
    assert(!dispatch(VIDIOC_G_FMT,&fmt) && fmt.fmt.pix.width==3840);
    capture.queue.busy=true;
    assert(dispatch(VIDIOC_S_FMT,&fmt)==-EBUSY && capture.width==3840);
    capture.queue.busy=false;
    assert(!dispatch(VIDIOC_S_FMT,&fmt) && capture.width==1920 && capture.height==1080);
    assert(capture.frame_size==3110400 && capture.allocation_size==3133440);
    assert(capture.frame_capacity==12441600);
    input_width=1366; input_height=768;
    fmt.fmt.pix.width=1366; fmt.fmt.pix.height=768;
    assert(!dispatch(VIDIOC_S_FMT,&fmt));
    assert(capture.width==1366 && capture.stride==1376);
    assert(capture.frame_size==1376*768*3/2);
    assert(capture.allocation_size>=capture.frame_size);
    /* A 16-byte VDIN stride is too short for Wave5's 32-byte NV12 rows. */
    input_width=720; input_height=400;
    fmt.fmt.pix.width=720; fmt.fmt.pix.height=400;
    assert(!dispatch(VIDIOC_S_FMT,&fmt));
    assert(capture.width==720 && capture.stride==736);
    assert(capture.frame_size==736*400*3/2);
    input_width=1680; input_height=1050;
    fmt.fmt.pix.width=1680; fmt.fmt.pix.height=1050;
    assert(!dispatch(VIDIOC_S_FMT,&fmt));
    assert(capture.width==1680 && capture.stride==1696);
    assert(capture.frame_size==1696*1050*3/2);
    assert(capture.allocation_size>=1696*1056*3/2);
    input_width=1920; input_height=1080;
    fmt.fmt.pix.width=1920; fmt.fmt.pix.height=1080;
    assert(!dispatch(VIDIOC_S_FMT,&fmt));
    input_width=5000; input_height=3000;
    assert(dispatch(VIDIOC_S_FMT,&fmt)==-ERANGE && capture.width==1920);
    input_width=3840; input_height=2160;
    fmt.fmt.pix.width=3840; fmt.fmt.pix.height=2160;
    assert(!dispatch(VIDIOC_S_FMT,&fmt) && capture.width==3840);
    /* Native 444 retains the entire source raster and rejects a changed source. */
    fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_YUV24;
    assert(dispatch(VIDIOC_S_FMT,&fmt)==-EINVAL);
    capture.direct_dma=true;
    fmt.fmt.pix.width=1280; fmt.fmt.pix.height=720;
    fail_allocation=true;
    assert(dispatch(VIDIOC_S_FMT,&fmt)==-ENOMEM);
    assert(capture.frame_capacity==12441600 && capture.pixel_format==V4L2_PIX_FMT_NV12);
    assert(!allocations && !frees);
    fail_allocation=false;
    assert(!dispatch(VIDIOC_S_FMT,&fmt));
    assert(capture.frame_capacity==24883200 && allocations==1 && frees==1);
    assert(capture.width==3840 && capture.height==2160 && capture.stride==11520);
    assert(capture.frame_size==24883200 && !capture.nv12 && !capture.native_hdr);
    capture.queue.busy=true;
    assert(dispatch(VIDIOC_S_FMT,&fmt)==-EBUSY);
    capture.queue.busy=false; yuv444_status=-EOPNOTSUPP;
    assert(dispatch(VIDIOC_S_FMT,&fmt)==-EINVAL);
    struct v4l2_pix_format saved=capture.configured_format;
    input_locked=false;
    assert(!dispatch(VIDIOC_G_FMT,&fmt) && !memcmp(&fmt.fmt.pix,&saved,sizeof(saved)));
    struct file reopened={.data=&capture};
    assert(!t7_vdin_get_fmt(&reopened,NULL,&fmt));
    assert(!memcmp(&fmt.fmt.pix,&saved,sizeof(saved)));
    input_locked=true;
    fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_NV12;
    fail_allocation=true;
    assert(dispatch(VIDIOC_S_FMT,&fmt)==-ENOMEM);
    assert(capture.frame_capacity==24883200 && capture.pixel_format==V4L2_PIX_FMT_YUV24);
    fail_allocation=false;
    assert(!dispatch(VIDIOC_S_FMT,&fmt) && capture.nv12);
    assert(capture.frame_capacity==12441600 && allocations==2 && frees==2);
    /* Consumers may retry RGB after an unsupported v308 probe. */
    nv12=false; fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_YUV24;
    assert(t7_vdin_try_fmt(&format_file,NULL,&fmt)==-EINVAL);
    fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_RGB24;
    assert(!t7_vdin_try_fmt(&format_file,NULL,&fmt));
    assert(fmt.fmt.pix.pixelformat==V4L2_PIX_FMT_RGB24);
    fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_YUV24;
    assert(t7_vdin_try_fmt(&format_file,NULL,&fmt)==-EINVAL);
    yuv444_status=-EAGAIN;
    assert(t7_vdin_try_fmt(&format_file,NULL,&fmt)==-EAGAIN);
    yuv444_status=-ENOLINK;
    assert(t7_vdin_try_fmt(&format_file,NULL,&fmt)==-ENOLINK);
    nv12=true;
    /* A query already using RX must finish before removal clears registration. */
    pause_query=true; query_entered=query_done=false; capture.queue.owner=&capture;
    pthread_create(&b,NULL,call_worker,&query); assert(wait_for(&query_entered));
    pthread_create(&a,NULL,remove_worker,NULL); assert(wait_for(&remove_has_queue));
    assert(atomic_load(&capture.video.registered) && !queue_releases);
    pthread_mutex_lock(&gate); release_query=true;
    pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    pthread_join(b,NULL); pthread_join(a,NULL);
    assert(!atomic_load(&capture.video.registered) && queue_releases==1 && !capture.video.dev.refs);
    assert(!capture.queue.owner);
    unsigned int reads=rx_reads;
    struct v4l2_dv_timings timing={0};
    assert(dispatch(VIDIOC_QUERY_DV_TIMINGS,&timing)==-ENODEV && rx_reads==reads);
    assert(dispatch(VIDIOC_G_FMT,&fmt)==-ENODEV);
    puts("ioctl locks: status during gated stop, S_FMT exclusion, query/removal and old fd passed");
}
'''
prefix = '#include "' + str(directory.parents[4] / 'include/uapi/linux/amlogic-vdin.h') + '"\n' + prefix
with tempfile.TemporaryDirectory(prefix='t7-ioctl-locks-') as temporary:
    path = Path(temporary)
    (path/'test.c').write_text(prefix + '\n'.join(entries) + stubs + formats + queries +
                             registration + selector + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-pthread', '-fsanitize=address,undefined',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=20)
