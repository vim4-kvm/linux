#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real multi-layer DMA reference owner without hardware."""
from pathlib import Path
import subprocess
import tempfile


source = Path(__file__).with_name('meson_t7_scanout.h').read_text()
implementation = source[source.index('#define MESON_T7_MAX_LAYERS'):source.rindex('#endif')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <stdint.h>
typedef uint32_t u32;
struct dma_fence {unsigned int refs;bool signaled;};
static struct dma_fence *dma_fence_get(struct dma_fence *f)
{if(f) {assert(f->refs);f->refs++;}return f;}
static void dma_fence_put(struct dma_fence *f)
{if(f) {assert(f->refs>1);f->refs--;}}
static void dma_fence_signal(struct dma_fence *f) {assert(f);f->signaled=true;}
struct drm_framebuffer { unsigned int refs; };
static void drm_framebuffer_get(struct drm_framebuffer *fb) { fb->refs++; }
static void drm_framebuffer_put(struct drm_framebuffer *fb)
{
    assert(fb->refs > 1); /* The simulated exporter retains its own reference. */
    fb->refs--;
}
'''
checks = r'''
int main(void)
{
    struct meson_t7_scanout owner = {0};
    struct drm_framebuffer desktop = {1}, cursor = {1}, video = {1}, decoded = {1};
    struct drm_framebuffer *next[MESON_T7_MAX_LAYERS] = {
        &desktop, &cursor, &desktop, &video, &decoded
    };

    assert(!meson_t7_scanout_acquire(&owner, next));
    assert(desktop.refs == 3 && cursor.refs == 2 && video.refs == 2 && decoded.refs == 2);
    meson_t7_scanout_retire(&owner, true);
    /* Unchanged/shared FBs are acquired before ANY old layer is retired. */
    for (unsigned int frame = 0; frame < 120; frame++) {
        next[1] = frame & 1 ? &cursor : NULL;
        assert(!meson_t7_scanout_acquire(&owner, next));
        assert(desktop.refs == 5 && video.refs == 3 && decoded.refs == 3);
        meson_t7_scanout_retire(&owner, true);
        assert(desktop.refs == 3 && video.refs == 2 && decoded.refs == 2);
        assert(cursor.refs == (next[1] ? 2U : 1U));
    }
    /* RMFB and CRTC disable blank every layer in one transaction. */
    for (unsigned int i = 0; i < MESON_T7_MAX_LAYERS; i++) next[i] = NULL;
    assert(!meson_t7_scanout_acquire(&owner, next));
    assert(desktop.refs == 3 && video.refs == 2);
    meson_t7_scanout_retire(&owner, true);
    assert(desktop.refs == 1 && cursor.refs == 1 && video.refs == 1 && decoded.refs == 1);

    next[0] = &desktop; next[1] = &cursor; next[3] = &video;
    assert(!meson_t7_scanout_acquire(&owner, next));
    meson_t7_scanout_retire(&owner, true);
    next[0] = &decoded; next[1] = NULL; next[3] = &video; next[4] = &desktop;
    assert(!meson_t7_scanout_acquire(&owner, next));
    meson_t7_scanout_retire(&owner, false);
    assert(owner.faulted);
    assert(desktop.refs == 3 && cursor.refs == 2 && video.refs == 3 && decoded.refs == 2);
    /* Neither a delayed completion nor a later blank can release pinned DMA. */
    meson_t7_scanout_retire(&owner, true);
    assert(meson_t7_scanout_acquire(&owner, next) == -EIO);
    assert(desktop.refs == 3 && cursor.refs == 2 && video.refs == 3 && decoded.refs == 2);
    assert(owner.active[1] == &cursor && owner.pending[1] == NULL);

    /* Multi-layer tables can cross active fetch: retain one extra generation. */
    struct meson_t7_scanout pipelined = {0};
    struct drm_framebuffer a = {1}, b = {1}, c = {1};
    for (unsigned int i = 0; i < MESON_T7_MAX_LAYERS; i++) next[i] = &a;
    assert(!meson_t7_scanout_acquire(&pipelined, next));
    meson_t7_scanout_advance(&pipelined, true, true);
    for (unsigned int i = 0; i < MESON_T7_MAX_LAYERS; i++) next[i] = &b;
    assert(!meson_t7_scanout_acquire(&pipelined, next));
    meson_t7_scanout_advance(&pipelined, true, true);
    assert(a.refs == 6 && b.refs == 6);
    for (unsigned int i = 0; i < MESON_T7_MAX_LAYERS; i++) next[i] = &c;
    assert(!meson_t7_scanout_acquire(&pipelined, next));
    meson_t7_scanout_advance(&pipelined, true, true);
    assert(a.refs == 1 && b.refs == 6 && c.refs == 6);
    for (unsigned int i = 0; i < MESON_T7_MAX_LAYERS; i++) next[i] = &a;
    assert(!meson_t7_scanout_acquire(&pipelined, next));
    meson_t7_scanout_advance(&pipelined, false, true);
    meson_t7_scanout_retire(&pipelined, true);
    assert(a.refs == 6 && b.refs == 6 && c.refs == 6);
    assert(meson_t7_scanout_acquire(&pipelined, next) == -EIO);

    struct meson_t7_scanout disabled = {0};
    assert(!meson_t7_scanout_acquire(&disabled, next));
    meson_t7_scanout_advance(&disabled, true, true);
    for (unsigned int i = 0; i < MESON_T7_MAX_LAYERS; i++) next[i] = NULL;
    assert(!meson_t7_scanout_acquire(&disabled, next));
    meson_t7_scanout_advance(&disabled, true, true);
    assert(a.refs == 11); /* Blank latched, but the previous fetch may still run. */
    meson_t7_scanout_retire(&disabled, true); /* One further vsync drains it. */
    assert(a.refs == 6);

    /* Every new latch can advance while the preceding reader drains. */
    struct meson_t7_scanout_readers readers={0};
    struct dma_fence frames[2][120]={0};
    struct dma_fence *incoming[MESON_T7_MAX_LAYERS]={0};
    u32 vsync=UINT32_MAX-1;
    for(unsigned int frame=0;frame<120;frame++,vsync++) {
        for(unsigned int plane=0;plane<2;plane++) {
            frames[plane][frame].refs=1;
            incoming[3+plane]=&frames[plane][frame];
        }
        meson_t7_scanout_readers_vsync(&readers,vsync);
        assert(!meson_t7_scanout_readers_acquire(&readers,incoming));
        assert(meson_t7_scanout_readers_advance(&readers,vsync));
        meson_t7_scanout_readers_vsync(&readers,vsync);
        for(unsigned int plane=0;plane<2;plane++) {
            assert(!frames[plane][frame].signaled && frames[plane][frame].refs==2);
            if(frame) {
                assert(!frames[plane][frame-1].signaled);
                assert(frames[plane][frame-1].refs==2);
            }
            if(frame>1) {
                assert(frames[plane][frame-2].signaled);
                assert(frames[plane][frame-2].refs==1);
            }
        }
    }
    /* An idle producer needs no further atomic commit to retire frame118. */
    meson_t7_scanout_readers_vsync(&readers,vsync);
    assert(frames[0][118].signaled && !frames[0][119].signaled);
    for(unsigned int i=0;i<MESON_T7_MAX_LAYERS;i++) incoming[i]=NULL;
    assert(!meson_t7_scanout_readers_acquire(&readers,incoming));
    assert(meson_t7_scanout_readers_advance(&readers,vsync));
    meson_t7_scanout_readers_vsync(&readers,vsync);
    assert(!frames[0][119].signaled);
    meson_t7_scanout_readers_vsync(&readers,vsync+1);
    assert(frames[0][119].signaled && frames[1][119].signaled);

    struct meson_t7_scanout_readers fault={0};
    struct dma_fence first={.refs=1},second={.refs=1},third={.refs=1};
    incoming[3]=&first;
    assert(!meson_t7_scanout_readers_acquire(&fault,incoming));
    incoming[3]=&second;
    assert(meson_t7_scanout_readers_acquire(&fault,incoming)==-EBUSY);
    assert(first.refs==2 && second.refs==1);
    assert(meson_t7_scanout_readers_advance(&fault,10));
    /* A two-phase transaction keeps the same reader alive without signaling. */
    incoming[3]=&first;
    assert(!meson_t7_scanout_readers_acquire(&fault,incoming));
    assert(meson_t7_scanout_readers_advance(&fault,10));
    assert(!first.signaled && first.refs==2);
    incoming[3]=&second;
    assert(!meson_t7_scanout_readers_acquire(&fault,incoming));
    assert(meson_t7_scanout_readers_advance(&fault,11));
    incoming[3]=&third;
    assert(!meson_t7_scanout_readers_acquire(&fault,incoming));
    assert(!meson_t7_scanout_readers_advance(&fault,11));
    assert(fault.faulted && !first.signaled && !second.signaled && !third.signaled);
    meson_t7_scanout_readers_vsync(&fault,1000);
    assert(!first.signaled && !second.signaled && !third.signaled);
    assert(first.refs==2 && second.refs==2 && third.refs==2);
    assert(meson_t7_scanout_readers_acquire(&fault,incoming)==-EIO);
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'scanout.c'
    executable = Path(directory) / 'scanout'
    code.write_text(prefix + implementation + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('Five-layer retirement: aliases, pipelined readers, wrap, idle completion and fault pinning passed')
