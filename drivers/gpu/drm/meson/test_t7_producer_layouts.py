#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Feed actual VDIN/Wave5 producer DMA layouts into T7 framebuffer bounds checks."""
from pathlib import Path
import re
import subprocess
import tempfile


directory = Path(__file__).resolve().parent
root = directory.parents[3]
vdin = (root / 'drivers/media/platform/amlogic/t7-hdmirx/t7-vdin-test.c').read_text()
wave5 = (root / 'drivers/media/platform/chips-media/wave5/wave5-vpu-dec.c').read_text()
check = (directory / 'meson_t7_fb.h').read_text()
check = re.sub(r'^#include[^\n]*\n', '', check, flags=re.M)
ring = vdin[vdin.index('static void t7_vdin_ring_arm('):
            vdin.index('static int t7_vdin_ring_free_slot(')]
linear = wave5[wave5.index('\tfor (i = 0; i < linear_num; i++) {'):]
linear = linear[:linear.index('\n\tret = wave5_vpu_dec_register_frame_buffer_ex(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef u64 dma_addr_t;
#define U32_MAX UINT32_MAX
#define IS_ALIGNED(v,a) (!((v)&((a)-1)))
#define ALIGN(v,a) (((v)+(a)-1)&~((a)-1))
#define DRM_FORMAT_MOD_LINEAR 0
struct drm_format_info {unsigned int num_planes, cpp[2], hsub, vsub;};
struct drm_gem_dma_object {u64 dma_addr; struct {u64 size;} base;};
struct drm_framebuffer {
    u32 width,height,pitches[2],offsets[2]; u64 modifier;
    const struct drm_format_info *format; struct drm_gem_dma_object *objects[2];
};
static struct drm_gem_dma_object *drm_fb_dma_get_gem_obj(struct drm_framebuffer *f, int p)
{return f->objects[p];}
static int drm_format_info_plane_width(const struct drm_format_info *f, unsigned int w, int p)
{return p ? (w+f->hsub-1)/f->hsub : w;}
static int drm_format_info_plane_height(const struct drm_format_info *f, unsigned int h, int p)
{return p ? (h+f->vsub-1)/f->vsub : h;}
enum {VDIN_WR_BADDR,VDIN_WR_CHROMA,VDIN_WR_CTRL,VDIN_SLOT_WRITING};
#define VDIN_WR_REQ 1
#define VDIN_VCP_WR 2
struct t7_vdin_buffer {dma_addr_t dma; bool hardware_used;};
struct t7_vdin_slot {dma_addr_t dma; struct t7_vdin_buffer *buffer; int state;};
struct t7_vdin {
    struct t7_vdin_slot slots[1]; unsigned int stride,height,active_slot;
    bool direct_dma,nv12,dma_idle,ring_running,hdr_curve_pending,ring_discard;
    void *receiver;
};
static struct t7_vdin_buffer capture;
static u32 written[3];
static struct t7_vdin_buffer *t7_vdin_next_buffer(struct t7_vdin *v) {return &capture;}
static bool t7_hdmirx_stream_is_hdr(void *receiver) {return false;}
static void t7_vdin_hdr_reload(struct t7_vdin *v) {assert(false);}
static void t7_vdin_write(struct t7_vdin *v, u32 reg, u32 value) {written[reg]=value;}
static void t7_vdin_update(struct t7_vdin *v, u32 reg, u32 mask, u32 value)
{written[reg]=(written[reg]&~mask)|(value&mask);}
struct vb2_buffer {dma_addr_t address[2]; u32 size[2];};
static struct vb2_buffer decoded;
struct vb2_queue {int unused;};
struct v4l2_m2m_ctx {struct vb2_queue queue;};
static struct vb2_queue *v4l2_m2m_get_dst_vq(struct v4l2_m2m_ctx *c) {return &c->queue;}
static struct vb2_buffer *vb2_get_buffer(struct vb2_queue *q, int i) {return &decoded;}
static u32 vb2_plane_size(struct vb2_buffer *b, int p) {return b->size[p];}
static dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *b, int p)
{return b->address[p];}
struct frame_buffer {
    dma_addr_t buf_y,buf_cb,buf_cr; u32 size,width,stride,map_type; bool update_fb_info;
};
struct vpu_instance {
    struct {struct v4l2_m2m_ctx *m2m_ctx;} v4l2_fh;
    struct {u32 width,height,num_planes;} dst_fmt,src_fmt;
    struct frame_buffer frame_buf[1]; int output_format;
};
#define FORMAT_422 1
#define LINEAR_FRAME_MAP 0
'''
checks = r'''
static void validate(u32 width, u32 height, u32 stride, dma_addr_t y, dma_addr_t uv,
                     u32 y_size, u32 uv_size, bool separate)
{
    const struct drm_format_info nv12={2,{1,2},2,2};
    struct drm_gem_dma_object a={.dma_addr=y,.base.size=y_size};
    struct drm_gem_dma_object b={.dma_addr=uv,.base.size=uv_size};
    struct drm_framebuffer fb={.width=width,.height=height,.pitches={stride,stride},
        .format=&nv12,.objects={&a,separate ? &b : &a},
        .offsets={0,separate ? 0 : uv-y}};
    assert(!meson_t7_fb_check(&fb));
    /* Producer coordinates, not allocation height, determine the scanned chroma. */
    assert(fb.objects[1]->dma_addr+fb.offsets[1]==uv);
    if (separate) b.base.size=(u64)stride*((height+1)/2)-1;
    else a.base.size=fb.offsets[1]+(u64)stride*((height+1)/2)-1;
    assert(meson_t7_fb_check(&fb)==-ERANGE);
}
int main(void)
{
    const u32 geometry[][2]={{720,400},{1280,720},{1920,1080},{3840,2160}};
    struct v4l2_m2m_ctx context={0};
    for (unsigned int n=0;n<sizeof(geometry)/sizeof(geometry[0]);n++) {
        u32 width=geometry[n][0],height=geometry[n][1],stride=ALIGN(width,32);
        for (unsigned int direct=0;direct<2;direct++) {
            struct t7_vdin v={.stride=stride,.height=height,.nv12=true,.direct_dma=direct};
            v.slots[0].dma=0x20000000; capture.dma=0x30000000;
            t7_vdin_ring_arm(&v,0);
            assert(v.ring_running && !v.dma_idle);
            assert(written[VDIN_WR_BADDR]==(direct ? capture.dma : v.slots[0].dma)>>4);
            if (direct) assert(capture.hardware_used);
            validate(width,height,stride,(u64)written[VDIN_WR_BADDR]<<4,
                     (u64)written[VDIN_WR_CHROMA]<<4,
                     stride*ALIGN(height,16)*3/2,0,false);
        }
        for (unsigned int planes=1;planes<=2;planes++) {
            struct vpu_instance producer={.v4l2_fh.m2m_ctx=&context,
                .dst_fmt={stride,height,planes},.src_fmt.width=width};
            struct vpu_instance *inst=&producer;
            unsigned int i,linear_num=1,non_linear_num=0;
            decoded=(struct vb2_buffer){.address={0x20000000,0x30000000},
                .size={stride*height*(planes==1 ? 3 : 2)/2,stride*height/2}};
            /* Actual wave5_prepare_fb() linear loop is inserted here. */
'''
checks += linear + r'''
            struct frame_buffer *f=&producer.frame_buf[0];
            assert(f->map_type==LINEAR_FRAME_MAP && f->update_fb_info);
            assert(f->width==width && f->stride==stride);
            validate(width,height,f->stride,f->buf_y,f->buf_cb,
                     decoded.size[0],decoded.size[1],planes==2);
        }
    }
    puts("Producer layout integration: 8 VDIN ring/DMA and 8 Wave5 NV12/NV12M cases passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'producers.c').write_text(prefix + check + ring + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(path / 'producers.c'),
                    '-o', str(path / 'producers')], check=True)
    subprocess.run([str(path / 'producers')], check=True)
