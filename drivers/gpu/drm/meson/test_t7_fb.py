#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise complete-row bounds for linear RGB and producer NV12 layouts."""
from pathlib import Path
import re
import subprocess
import tempfile


source = Path(__file__).with_name('meson_t7_fb.h').read_text()
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
handoff = Path(__file__).with_name('meson_t7_handoff.c').read_text()
source += handoff[handoff.index('static int meson_t7_handoff_dumb_create('):
                  handoff.index('DEFINE_DRM_GEM_DMA_FOPS(')]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define U32_MAX UINT32_MAX
#define IS_ALIGNED(v,a) (!((v)&((a)-1)))
#define DRM_FORMAT_MOD_LINEAR 0
#define HANDOFF_WIDTH 1920
#define HANDOFF_HEIGHT 1080
#define ALIGN(v,a) (((v)+(a)-1)&~((a)-1))
#define PAGE_ALIGN(v) ALIGN(v,4096ULL)
struct drm_file {};
struct meson_t7_handoff {bool native_modeset, linear_formats;};
struct drm_device {struct meson_t7_handoff *display;};
struct drm_mode_create_dumb {u32 width,height,bpp,pitch;u64 size;};
static struct meson_t7_handoff *to_handoff(struct drm_device *drm)
{return drm->display;}
static int drm_gem_dma_dumb_create_internal(struct drm_file *file,
    struct drm_device *drm, struct drm_mode_create_dumb *args)
{(void)file;(void)drm;assert(args->size>=args->pitch*(u64)args->height);return 0;}
struct drm_format_info {unsigned int num_planes, cpp[2], hsub, vsub;};
struct drm_gem_dma_object {u64 dma_addr; struct {u64 size;} base;};
struct drm_framebuffer {
    u32 width, height, pitches[2], offsets[2];
    u64 modifier;
    const struct drm_format_info *format;
    struct drm_gem_dma_object *objects[2];
};
static struct drm_gem_dma_object *drm_fb_dma_get_gem_obj(struct drm_framebuffer *f, int p)
{return f->objects[p];}
static int drm_format_info_plane_width(const struct drm_format_info *f, unsigned int w, int p)
{return p ? (w+f->hsub-1)/f->hsub : w;}
static int drm_format_info_plane_height(const struct drm_format_info *f, unsigned int h, int p)
{return p ? (h+f->vsub-1)/f->vsub : h;}
'''
checks = r'''
int main(void)
{
    struct meson_t7_handoff display={.native_modeset=true,.linear_formats=true};
    struct drm_device drm={.display=&display};
    /* Mesa render-only storage for a visible 3840x2160 scanout includes tailroom. */
    struct drm_mode_create_dumb dumb={.width=3840,.height=2178,.bpp=32};
    assert(!meson_t7_handoff_dumb_create(NULL,&drm,&dumb));
    assert(dumb.pitch==15360 && dumb.size==PAGE_ALIGN(15360ULL*2178));
    dumb.height=4096;
    assert(!meson_t7_handoff_dumb_create(NULL,&drm,&dumb));
    dumb.height++;
    assert(meson_t7_handoff_dumb_create(NULL,&drm,&dumb)==-EINVAL);
    dumb.height=0;
    assert(meson_t7_handoff_dumb_create(NULL,&drm,&dumb)==-EINVAL);
    display.native_modeset=false;
    dumb=(struct drm_mode_create_dumb){.width=1920,.height=1080,.bpp=24};
    assert(!meson_t7_handoff_dumb_create(NULL,&drm,&dumb));
    dumb.height=2178;
    assert(meson_t7_handoff_dumb_create(NULL,&drm,&dumb)==-EINVAL);
    display.linear_formats=false;dumb.height=1080;dumb.bpp=32;
    assert(meson_t7_handoff_dumb_create(NULL,&drm,&dumb)==-EINVAL);
    const struct drm_format_info rgb={1,{3},1,1}, nv12={2,{1,2},2,2};
    struct drm_gem_dma_object a={.dma_addr=0x20000000}, b={.dma_addr=0x30000000};
    /* Core ADDFB permits a short final row. The MIF requires its padding too. */
    struct drm_framebuffer fb={.width=17,.height=32,.pitches={64},
        .format=&rgb,.objects={&a}};
    a.base.size=31*64+17*3;
    assert(meson_t7_fb_check(&fb)==-ERANGE);
    a.base.size=32*64;
    assert(!meson_t7_fb_check(&fb));
    fb.offsets[0]=16;
    assert(meson_t7_fb_check(&fb)==-ERANGE);
    a.base.size+=16;
    assert(!meson_t7_fb_check(&fb));
    fb.offsets[0]=1;
    assert(meson_t7_fb_check(&fb)==-ERANGE);
    fb.offsets[0]=0;
    a.dma_addr=UINT64_MAX-15;
    assert(meson_t7_fb_check(&fb)==-ERANGE);
    a.dma_addr=(1ULL<<32)-32*64;
    assert(!meson_t7_fb_check(&fb));
    a.dma_addr+=16;
    assert(meson_t7_fb_check(&fb)==-ERANGE);
    a.dma_addr=0x20000000;
    /* VDIN visible-height chroma plus tailroom for the aligned encoder view. */
    fb=(struct drm_framebuffer){.width=720,.height=400,.format=&nv12,
        .pitches={736,736},.offsets={0,736*400},.objects={&a,&a}};
    a.base.size=736*400*3/2;
    assert(!meson_t7_fb_check(&fb));
    /* A non-16-aligned visible height must not move UV into allocation tailroom. */
    fb.height=1080; fb.width=1920; fb.pitches[0]=fb.pitches[1]=1920;
    fb.offsets[1]=1920*1080; a.base.size=1920*1088*3/2;
    assert(!meson_t7_fb_check(&fb));
    assert(fb.offsets[1]==1920*1080);
    /* Wave5 NV12M and meson-vdec use independently imported Y/UV objects. */
    fb.objects[1]=&b; fb.offsets[1]=32;
    a.base.size=1920*1080; b.base.size=32+1920*540;
    assert(!meson_t7_fb_check(&fb));
    b.base.size--;
    assert(meson_t7_fb_check(&fb)==-ERANGE);
    b.base.size++; fb.offsets[1]=UINT32_MAX;
    assert(meson_t7_fb_check(&fb)==-ERANGE);
    fb.offsets[1]=32; b.dma_addr=(1ULL<<32)-1920*540;
    assert(meson_t7_fb_check(&fb)==-ERANGE);
    b.dma_addr=0x30000000;
    fb.objects[1]=NULL;
    assert(meson_t7_fb_check(&fb)==-EINVAL);
    fb.objects[1]=&b; fb.pitches[1]=1918;
    assert(meson_t7_fb_check(&fb)==-EINVAL);
    fb.pitches[1]=1888;
    assert(meson_t7_fb_check(&fb)==-EINVAL);
    fb.pitches[1]=65536;
    assert(meson_t7_fb_check(&fb)==-EINVAL);
    fb.pitches[1]=1920; fb.modifier=1;
    assert(meson_t7_fb_check(&fb)==-EINVAL);
    assert(meson_t7_fb_check(NULL)==-EINVAL);
    puts("Linear MIF: final-row padding, independent NV12 imports, offsets and DMA32 bounds passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'fb.c').write_text(prefix + source + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'fb.c'), '-o', str(path / 'fb')], check=True)
    subprocess.run([str(path / 'fb')], check=True)
