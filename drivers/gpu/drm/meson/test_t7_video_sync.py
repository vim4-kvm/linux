#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute scanout reader preparation, cancellation and state lifetime failures."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).with_name('meson_t7_video_sync.c').read_text()
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#define GFP_KERNEL 0
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
typedef int spinlock_t;
static void spin_lock_init(spinlock_t *lock) {*lock=0;}
static unsigned int allocations, releases, signals, producer_calls;
static bool fail_allocation;
static bool producer_ready;
static int producer_error;
static void *allocate(size_t bytes) {
    if (fail_allocation) return NULL;
    void *p=calloc(1,bytes);assert(p);allocations++;return p;
}
#define kzalloc_obj(object) allocate(sizeof(object))
static void kfree(void *p) {if(p) {releases++;free(p);}}
struct dma_fence;
struct dma_fence_ops {
    const char *(*get_driver_name)(struct dma_fence *);
    const char *(*get_timeline_name)(struct dma_fence *);
    void (*release)(struct dma_fence *);
};
struct dma_fence {
    unsigned int refs;
    uint64_t context;
    bool signaled;
    const struct dma_fence_ops *ops;
};
static uint64_t next_context;
static uint64_t dma_fence_context_alloc(unsigned int count) {
    assert(count==1);return ++next_context;
}
static void dma_fence_init(struct dma_fence *f,const struct dma_fence_ops *ops,
                           spinlock_t *lock,uint64_t context,unsigned int seq) {
    assert(lock && seq==1);f->refs=1;f->ops=ops;f->context=context;
}
static struct dma_fence *dma_fence_get(struct dma_fence *f) {
    if(f) {assert(f->refs);f->refs++;}return f;
}
static void dma_fence_free(struct dma_fence *f) {kfree(f);}
static void dma_fence_put(struct dma_fence *f) {
    if(f) {assert(f->refs);if(!--f->refs) f->ops->release(f);}
}
static int dma_fence_signal(struct dma_fence *f) {
    assert(f && f->refs);if(f->signaled) return -EINVAL;
    f->signaled=true;signals++;return 0;
}
struct dma_resv {
    struct dma_fence *readers[32];
    unsigned int count,locks;
    bool locked,fail;
};
#define DMA_RESV_USAGE_READ 2
static int dma_resv_lock(struct dma_resv *r,void *context) {
    assert(!context && !r->locked);r->locked=true;r->locks++;return 0;
}
static void dma_resv_unlock(struct dma_resv *r) {assert(r->locked);r->locked=false;}
static int dma_resv_reserve_fences(struct dma_resv *r,unsigned int n) {
    assert(r->locked && n==1);return r->fail ? -ENOMEM : 0;
}
static void dma_resv_add_fence(struct dma_resv *r,struct dma_fence *f,int usage) {
    assert(r->locked && usage==DMA_RESV_USAGE_READ && producer_ready);
    /* Kernel reservations may replace a previous fence in the same context. */
    for(unsigned int i=0;i<r->count;i++) if(r->readers[i]->context==f->context) {
        dma_fence_put(r->readers[i]);r->readers[i]=dma_fence_get(f);return;
    }
    assert(r->count<32);r->readers[r->count++]=dma_fence_get(f);
}
struct drm_gem_object {struct dma_resv *resv;};
struct drm_format_info {unsigned int num_planes;};
struct drm_framebuffer {struct drm_format_info *format;struct drm_gem_object *objects[2];};
struct drm_plane_state {struct drm_framebuffer *fb;bool visible;void *crtc,*fence;};
struct drm_plane {struct drm_plane_state *state;};
static struct drm_gem_object *drm_gem_fb_get_obj(struct drm_framebuffer *fb,unsigned int i) {
    assert(i<2);return fb->objects[i];
}
static int drm_gem_plane_helper_prepare_fb(struct drm_plane *plane,struct drm_plane_state *s) {
    producer_calls++;producer_ready=true;return producer_error;
}
static void __drm_atomic_helper_plane_reset(struct drm_plane *p,struct drm_plane_state *s) {p->state=s;}
static void __drm_atomic_helper_plane_duplicate_state(struct drm_plane *p,struct drm_plane_state *s) {*s=*p->state;}
static void __drm_atomic_helper_plane_destroy_state(struct drm_plane_state *s) {}
'''
checks = r'''
static int prepare(struct drm_plane *plane,struct drm_plane_state *state) {
    producer_ready=false;return meson_t7_video_prepare_fb(plane,state);
}
int main(void)
{
    struct dma_resv first={0},second={0};
    struct drm_gem_object y={&first},alias={&first},uv={&second};
    struct drm_format_info format={2};
    struct drm_framebuffer fb={&format,{&y,&alias}};
    struct drm_framebuffer split={&format,{&y,&uv}};
    struct drm_framebuffer missing={&format,{&y,NULL}};
    struct drm_plane plane={0};
    meson_t7_video_plane_reset(&plane);assert(plane.state);
    struct meson_t7_video_plane_state *a=video_state(plane.state);
    a->base.fb=&fb;a->base.visible=true;a->base.crtc=&plane;a->base.fence=&plane;
    assert(!prepare(&plane,&a->base));
    assert(first.count==1 && first.locks==1 && !first.locked);
    struct dma_fence *active=dma_fence_get(meson_t7_video_claim_reader(&a->base));
    assert(active && !active->signaled && a->base.fence==&plane);
    meson_t7_video_cleanup_fb(&plane,&a->base);
    assert(!active->signaled && !a->reader); /* Hardware owns its own reference. */
    struct drm_plane_state *b=meson_t7_video_plane_duplicate_state(&plane);
    assert(b && !video_state(b)->reader && !video_state(b)->submitted);
    assert(!prepare(&plane,b));
    assert(first.count==2 && first.readers[1]->context!=active->context);
    meson_t7_video_cleanup_fb(&plane,b);
    assert(first.readers[1]->signaled && !active->signaled);
    meson_t7_video_plane_destroy_state(&plane,b);
    struct drm_plane_state *c=meson_t7_video_plane_duplicate_state(&plane);
    c->fb=&split;second.fail=true;
    assert(prepare(&plane,c)==-ENOMEM);
    assert(!video_state(c)->reader && !first.locked && !second.locked);
    assert(first.count==3 && first.readers[2]->signaled && !active->signaled);
    unsigned int count=first.count,calls=producer_calls;
    c->visible=false;second.fail=false;
    assert(!prepare(&plane,c) && first.count==count && producer_calls==calls+1);
    c->visible=true;c->crtc=NULL;
    assert(!prepare(&plane,c) && first.count==count);
    c->crtc=&plane;c->fb=NULL;
    assert(!prepare(&plane,c) && first.count==count);
    c->fb=&fb;fail_allocation=true;
    assert(prepare(&plane,c)==-ENOMEM && first.count==count);
    fail_allocation=false;producer_error=-EIO;
    assert(prepare(&plane,c)==-EIO && first.count==count);
    producer_error=0;c->fb=&missing;
    assert(prepare(&plane,c)==-EINVAL);
    assert(!video_state(c)->reader && first.count==count+1);
    assert(first.readers[count]->signaled && !active->signaled);
    c->fb=&fb;
    assert(!prepare(&plane,c));
    struct dma_fence *next=dma_fence_get(meson_t7_video_claim_reader(c));
    assert(next && meson_t7_video_claim_reader(c)==next);
    meson_t7_video_cleanup_fb(&plane,c);
    assert(!next->signaled && !active->signaled);
    dma_fence_signal(active);dma_fence_put(active);
    assert(!next->signaled); /* Independent generations and reservations. */
    dma_fence_signal(next);dma_fence_put(next);
    meson_t7_video_plane_destroy_state(&plane,c);
    fail_allocation=true;meson_t7_video_plane_reset(&plane);
    assert(!plane.state);fail_allocation=false;
    assert(!meson_t7_video_plane_duplicate_state(&plane));
    for(unsigned int i=0;i<first.count;i++) dma_fence_put(first.readers[i]);
    assert(!second.count && allocations==releases);
    puts("Video readers: producer ordering, aliases, cancellation, split failure and state lifetime passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'sync.c').write_text(prefix + source + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(root / 'sync.c'), '-o', str(root / 'sync')], check=True)
    subprocess.run([str(root / 'sync')], check=True)
