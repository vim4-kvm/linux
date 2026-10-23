#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual encoder import validation and DMA offsets without hardware."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('wave5-vpu-enc.c').read_text()
address = source[source.index('static dma_addr_t wave5_vpu_enc_source_addr'):
                 source.index('static int start_encode')]
prepare = source[source.index('static int wave5_vpu_enc_buf_prepare'):
                 source.index('static void wave5_vpu_enc_buf_queue')]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t dma_addr_t;
#define V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE 10
struct vpu_instance {
    struct { unsigned num_planes; struct { u32 sizeimage; } plane_fmt[3]; } src_fmt;
};
struct vb2_buffer {
    unsigned type;
    struct vpu_instance *vb2_queue;
    struct { u32 data_offset; uint64_t size, payload, dma; } planes[3];
};
static void *vb2_get_drv_priv(void *queue) { return queue; }
static uint64_t vb2_plane_size(struct vb2_buffer *vb, unsigned p) { return vb->planes[p].size; }
static uint64_t vb2_get_plane_payload(struct vb2_buffer *vb, unsigned p) { return vb->planes[p].payload; }
static dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *vb, unsigned p) { return vb->planes[p].dma; }
'''
checks = r'''
int main(void)
{
    struct vpu_instance inst = { .src_fmt = { .num_planes = 2,
        .plane_fmt = { { 8294400 }, { 4147200 } } } };
    struct vb2_buffer vb = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .vb2_queue = &inst,
        .planes = { { 0, 12441600, 8294400, 0x40000000 },
                    { 8294400, 12441600, 12441600, 0x40000000 } } };
    assert(!wave5_vpu_enc_buf_prepare(&vb));
    assert(wave5_vpu_enc_source_addr(&vb, 0) == 0x40000000);
    assert(wave5_vpu_enc_source_addr(&vb, 1) == 0x40000000 + 8294400);
    vb.planes[1].size--;
    assert(wave5_vpu_enc_buf_prepare(&vb) == -EINVAL);
    vb.planes[1].size++;
    vb.planes[1].payload--;
    assert(wave5_vpu_enc_buf_prepare(&vb) == -EINVAL);
    vb.planes[1].payload = 1;
    assert(wave5_vpu_enc_buf_prepare(&vb) == -EINVAL);
    vb.planes[1].data_offset = UINT32_MAX;
    assert(wave5_vpu_enc_buf_prepare(&vb) == -EINVAL);
    /* 1080 visible rows need 1088-row capacity, but UV stays after active Y. */
    inst.src_fmt.plane_fmt[0].sizeimage = 1920 * 1088;
    inst.src_fmt.plane_fmt[1].sizeimage = 1920 * 1088 / 2;
    vb.planes[0].size = vb.planes[1].size = 1920 * 1080 * 3 / 2;
    vb.planes[0].payload = 1920 * 1088;
    vb.planes[1].data_offset = 1920 * 1080;
    vb.planes[1].payload = 1920 * 1080 + 1920 * 1088 / 2;
    assert(wave5_vpu_enc_buf_prepare(&vb) == -EINVAL);
    vb.planes[0].size = vb.planes[1].size = 1920 * 1088 * 3 / 2;
    assert(!wave5_vpu_enc_buf_prepare(&vb));
    assert(wave5_vpu_enc_source_addr(&vb, 1) == 0x40000000 + 1920 * 1080);
    vb.type = 9;
    assert(!wave5_vpu_enc_buf_prepare(&vb));
    puts("Wave5 shared DMA plane offsets and bounds passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'buffers.c'
    test.write_text(harness + address + prepare + checks)
    executable = Path(directory) / 'buffers'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-O2', str(test),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

# Model the V4L2 queues, while executing the real submission, result and
# scheduling functions. A held lookahead buffer must never be submitted twice.
def function(name):
    start = source.index(name)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


scheduler = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t dma_addr_t;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define spin_lock_irqsave(lock,flags) ((flags)=0)
#define spin_unlock_irqrestore(lock,flags) ((void)(flags))
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define WARN_ON(x) (x)
#define W_AVC_ENC 1
#define W_HEVC_ENC 2
#define VPU_INST_STATE_NONE 0
#define VPU_INST_STATE_PIC_RUN 3
#define VPU_INST_STATE_STOP 4
#define VB2_BUF_STATE_DONE 1
#define VB2_BUF_STATE_ERROR 2
#define V4L2_EVENT_EOS 2
#define V4L2_FIELD_NONE 0
#define V4L2_BUF_FLAG_KEYFRAME 8
#define V4L2_BUF_FLAG_PFRAME 16
#define V4L2_BUF_FLAG_BFRAME 32
#define PIC_TYPE_I 0
#define PIC_TYPE_P 1
#define PIC_TYPE_B 2
#define PIC_TYPE_IDR 5
#define RECON_IDX_FLAG_ENC_DELAY -2
#define RECON_IDX_FLAG_ENC_END -1
#define WAVE5_SYSERR_QUEUEING_FAIL 1
#define VPU_ENC_TIMEOUT 100
#define V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE 10
#define msecs_to_jiffies(x) (x)
#define jiffies_to_msecs(x) (x)
#define time_after_eq(a,b) ((a)>=(b))
#define ENC_GET_QUEUE_STATUS 1
#define fallthrough __attribute__((fallthrough))
struct vb2_buffer {
    unsigned index, type; uint64_t timestamp;
    struct { u32 data_offset; uint64_t size, dma; } planes[3];
};
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; unsigned field, flags, payload, done; };
struct v4l2_m2m_buffer { struct vb2_v4l2_buffer vb; };
struct vpu_src_buffer { struct v4l2_m2m_buffer m2m_buf; bool consumed; };
#define wave5_to_vpu_src_buf(buf) ((struct vpu_src_buffer *)(buf))
struct v4l2_m2m_ctx {
    struct v4l2_m2m_buffer *src[8]; unsigned count;
    struct vb2_v4l2_buffer *dst;
    bool is_draining; unsigned jobs, eos;
};
#define v4l2_m2m_for_each_src_buf(ctx,b) \
    for(unsigned cursor=0;cursor<(ctx)->count && ((b)=(ctx)->src[cursor]);cursor++)
struct frame_buffer { uint64_t buf_y, buf_cb, buf_cr; unsigned stride; };
struct enc_wave_param { unsigned intra_qp, fixed_qp_p, fixed_qp_b; };
struct enc_param {
    uint64_t pic_stream_buffer_addr, pic_stream_buffer_size;
    unsigned src_idx;
    struct frame_buffer *source_frame;
    struct { unsigned implicit_header_encode, encode_aud; } code_option;
    bool src_end_flag, force_keyframe, force_qp;
    unsigned qp_i, qp_p, qp_b;
};
struct enc_output_info {
    int pic_type, recon_frame_index, enc_src_idx;
    unsigned enc_pic_byte, bitstream_size, enc_vcl_nut, frame_cycle;
    uint64_t pts;
};
struct v4l2_event { unsigned type; };
struct v4l2_format_info { unsigned mem_planes, hdiv, vdiv; };
struct vpu_device { bool isolated, legacy_firmware; struct vpu_device *dev; };
struct codec_info {
    struct { bool destroyed; struct { bool rc_enable; struct enc_wave_param wave_param; } open_param; } enc_info;
};
struct vpu_instance {
    struct { struct v4l2_m2m_ctx *m2m_ctx; } v4l2_fh;
    struct { unsigned pixelformat, height, num_planes;
        struct { unsigned bytesperline, sizeimage; } plane_fmt[3]; } src_fmt;
    struct vpu_device *dev; struct codec_info *codec_info;
    void *v4l2_m2m_dev;
    unsigned encode_aud, force_keyframe, state, std, recovery_error, fbc_buf_count;
    uint64_t timestamp;
};
struct vb2_queue { struct vpu_instance *inst; unsigned type; };
struct queue_status_info { unsigned report_queue_count; };
static unsigned jiffies;
static struct enc_output_info report;
static unsigned submitted[8], submissions, fail_command, destroy_calls, fail_destroy;
static unsigned freed_frames;
static bool wave5_vpu_isolated(struct vpu_device *dev) { return dev->isolated; }
static void wave5_vpu_isolate(struct vpu_device *dev,const char *reason,u32 cmd) { dev->isolated=true; }
static const struct v4l2_format_info *v4l2_format_info(unsigned format)
{ static const struct v4l2_format_info info={1,2,2}; return &info; }
static uint64_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *vb,unsigned p) { return vb->planes[p].dma; }
static uint64_t vb2_plane_size(struct vb2_buffer *vb,unsigned p) { return vb->planes[p].size; }
static struct vb2_v4l2_buffer *v4l2_m2m_next_dst_buf(struct v4l2_m2m_ctx *ctx) { return ctx->dst; }
static struct vb2_v4l2_buffer *v4l2_m2m_dst_buf_remove(struct v4l2_m2m_ctx *ctx)
{ struct vb2_v4l2_buffer *buf=ctx->dst; ctx->dst=NULL; return buf; }
static struct vb2_v4l2_buffer *v4l2_m2m_src_buf_remove_by_idx(struct v4l2_m2m_ctx *ctx,unsigned idx)
{
    for(unsigned i=0;i<ctx->count;i++) if(ctx->src[i]->vb.vb2_buf.index==idx) {
        struct vb2_v4l2_buffer *buf=&ctx->src[i]->vb;
        memmove(&ctx->src[i],&ctx->src[i+1],(ctx->count-i-1)*sizeof(ctx->src[0]));
        ctx->count--; return buf;
    }
    return NULL;
}
static struct vb2_v4l2_buffer *v4l2_m2m_src_buf_remove(struct v4l2_m2m_ctx *ctx)
{ return ctx->count?v4l2_m2m_src_buf_remove_by_idx(ctx,ctx->src[0]->vb.vb2_buf.index):NULL; }
static void v4l2_m2m_buf_done(struct vb2_v4l2_buffer *buf,unsigned state) { assert(!buf->done); buf->done=state; }
static void vb2_set_plane_payload(struct vb2_buffer *buf,unsigned plane,unsigned payload)
{ ((struct vb2_v4l2_buffer *)buf)->payload=payload; }
static void v4l2_m2m_last_buffer_done(struct v4l2_m2m_ctx *ctx,struct vb2_v4l2_buffer *buf)
{ v4l2_m2m_buf_done(buf,VB2_BUF_STATE_DONE); ctx->is_draining=false; }
static void v4l2_event_queue_fh(void *fh,const struct v4l2_event *event) { (void)fh; }
static void wave5_vpu_job_finish(struct vpu_instance *inst) { inst->v4l2_fh.m2m_ctx->jobs++; }
static unsigned v4l2_m2m_num_src_bufs_ready(struct v4l2_m2m_ctx *ctx) { return ctx->count; }
static void *v4l2_m2m_get_curr_priv(void *ctx) { return NULL; }
static unsigned atomic_xchg(unsigned *value,unsigned next) { unsigned old=*value; *value=next; return old; }
static void atomic_set(unsigned *value,unsigned next) { *value=next; }
static int switch_state(struct vpu_instance *inst,unsigned state) { inst->state=state; return 0; }
static const char *state_to_str(unsigned state) { return "ready"; }
static int wave5_vpu_enc_start_one_frame(struct vpu_instance *inst,struct enc_param *param,u32 *fail)
{
    assert(param->force_qp && param->qp_i==23 && param->qp_p==26 && param->qp_b==29);
    if(fail_command) { *fail=0; return -EIO; }
    submitted[submissions++]=param->src_end_flag?99:param->src_idx;
    return 0;
}
static int wave5_vpu_enc_get_output_info(struct vpu_instance *inst,struct enc_output_info *out)
{ *out=report; return 0; }
static int wave5_vpu_enc_close(struct vpu_instance *inst,u32 *fail)
{ destroy_calls++; return fail_destroy?-EIO:0; }
static int wave5_vpu_dec_reset_framebuffer(struct vpu_instance *inst,unsigned index)
{ freed_frames++; return 0; }
static struct vpu_instance *vb2_get_drv_priv(struct vb2_queue *q) { return q->inst; }
static void pm_runtime_resume_and_get(struct vpu_device *dev) { (void)dev; }
static void pm_runtime_put_autosuspend(struct vpu_device *dev) { (void)dev; }
static bool wave5_vpu_both_queues_are_streaming(struct vpu_instance *inst) { return true; }
static void v4l2_m2m_clear_state(struct v4l2_m2m_ctx *ctx) { ctx->is_draining=false; }
static int wave5_vpu_wait_interrupt(struct vpu_instance *inst,unsigned timeout) { return 0; }
static void wave5_vpu_enc_give_command(struct vpu_instance *inst,unsigned cmd,struct queue_status_info *status)
{ status->report_queue_count=0; }
'''
behavior = r'''
int main(void)
{
    struct vpu_device dev={0};
    struct codec_info codec={.enc_info.open_param.wave_param={23,26,29}};
    struct vpu_src_buffer src[3]={0};
    struct vb2_v4l2_buffer dst[4]={0};
    struct v4l2_m2m_ctx ctx={.count=3,.dst=&dst[0]};
    struct vpu_instance inst={.v4l2_fh.m2m_ctx=&ctx,.dev=&dev,.codec_info=&codec,
        .src_fmt={.height=1088,.num_planes=1,.plane_fmt={{1920,0}}},
        .std=W_HEVC_ENC,.state=VPU_INST_STATE_PIC_RUN};
    u32 fail=0;
    for(unsigned i=0;i<3;i++) {
        src[i].m2m_buf.vb.vb2_buf.index=i;
        src[i].m2m_buf.vb.vb2_buf.timestamp=100+i;
        ctx.src[i]=&src[i].m2m_buf;
    }
    for(unsigned i=0;i<3;i++) {
        assert(wave5_vpu_enc_job_ready(&inst));
        assert(!start_encode(&inst,&fail));
        assert(submitted[i]==i && src[i].consumed);
        report=(struct enc_output_info){.recon_frame_index=-2,.enc_src_idx=-1};
        wave5_vpu_enc_finish_encode(&inst);
        assert(ctx.dst==&dst[0] && !dst[0].done && ctx.count==3);
    }
    assert(ctx.jobs==3 && !wave5_vpu_enc_job_ready(&inst));
    /* Firmware output order I,P,B differs from input order I,B,P. */
    const unsigned order[]={0,2,1};
    for(unsigned i=0;i<3;i++) {
        ctx.dst=&dst[i];
        report=(struct enc_output_info){.enc_src_idx=order[i],.recon_frame_index=0,
            .bitstream_size=1234,.pic_type=i==2?PIC_TYPE_B:PIC_TYPE_P};
        wave5_vpu_enc_finish_encode(&inst);
        assert(src[order[i]].m2m_buf.vb.done==VB2_BUF_STATE_DONE);
        assert(dst[i].vb2_buf.timestamp==100+order[i] && dst[i].payload==1234);
    }
    assert(ctx.count==0 && (dst[2].flags&V4L2_BUF_FLAG_BFRAME));
    ctx.dst=&dst[3]; ctx.is_draining=true;
    assert(wave5_vpu_enc_job_ready(&inst) && !start_encode(&inst,&fail));
    assert(submitted[3]==99);
    report=(struct enc_output_info){.enc_src_idx=-1,.recon_frame_index=-1};
    wave5_vpu_enc_finish_encode(&inst);
    assert(dst[3].done && !ctx.is_draining && !dst[3].payload);
    struct v4l2_m2m_ctx other={.count=1,.src={&src[0].m2m_buf}};
    src[0].consumed=false; inst.v4l2_fh.m2m_ctx=&other;
    assert(wave5_vpu_enc_job_ready(&inst));
    inst.recovery_error=1; assert(!wave5_vpu_enc_job_ready(&inst));
    inst.recovery_error=0; inst.force_keyframe=1;
    src[0].consumed=true; src[1].consumed=false;
    src[0].m2m_buf.vb.done=src[1].m2m_buf.vb.done=dst[0].done=0;
    other=(struct v4l2_m2m_ctx){.count=2,.src={&src[0].m2m_buf,&src[1].m2m_buf},.dst=&dst[0]};
    fail_command=1;
    assert(start_encode(&inst,&fail)==-EIO && inst.force_keyframe);
    assert(other.count==1 && other.src[0]==&src[0].m2m_buf);
    assert(!src[0].m2m_buf.vb.done && src[1].m2m_buf.vb.done==VB2_BUF_STATE_ERROR);
    assert(!src[1].consumed && dst[0].done==VB2_BUF_STATE_ERROR);
    inst.fbc_buf_count=3;
    assert(!wave5_stop_retained_sources(&inst));
    assert(destroy_calls==1 && !inst.recovery_error && !dev.isolated);
    assert(inst.state==VPU_INST_STATE_NONE && !inst.fbc_buf_count && freed_frames==3);
    assert(codec.enc_info.destroyed && !src[0].consumed);
    assert(!wave5_stop_retained_sources(&inst) && destroy_calls==1);
    inst.state=VPU_INST_STATE_PIC_RUN; src[0].consumed=true; fail_destroy=1;
    assert(wave5_stop_retained_sources(&inst)==-EIO && dev.isolated);
    assert(src[0].consumed && !src[0].m2m_buf.vb.done && other.count==1);
    assert(!wave5_stop_retained_sources(&inst) && destroy_calls==2);
    for(unsigned failure=0;failure<2;failure++) {
        inst.recovery_error=0; dev.isolated=false; inst.state=VPU_INST_STATE_PIC_RUN;
        fail_destroy=failure; destroy_calls=0;
        src[0].consumed=true; src[0].m2m_buf.vb.done=dst[0].done=0;
        other=(struct v4l2_m2m_ctx){.count=1,.src={&src[0].m2m_buf},.dst=&dst[0]};
        struct vb2_queue queue={.inst=&inst,.type=9};
        wave5_vpu_enc_stop_streaming(&queue);
        assert(destroy_calls==1 && dst[0].done==VB2_BUF_STATE_ERROR);
        assert(dev.isolated==failure && !inst.recovery_error);
        assert(inst.state==(failure?VPU_INST_STATE_STOP:VPU_INST_STATE_NONE));
        /* Queueing ERROR after failed destroy is safe only because isolation
         * memory operations retain the DMA mapping (tested separately). */
        queue.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        wave5_vpu_enc_stop_streaming(&queue);
        assert(destroy_calls==1 && !other.count);
    }
    puts("Wave5 lookahead ownership, reordered timestamps, fair scheduling and drain passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-lookahead-') as directory:
    code = Path(directory) / 'lookahead.c'
    binary = Path(directory) / 'lookahead'
    functions = ''.join(function(name) for name in (
        'static struct vb2_v4l2_buffer *wave5_find_src(',
        'static dma_addr_t wave5_vpu_enc_source_addr(', 'static int start_encode(',
        'static void wave5_vpu_enc_finish_encode(', 'static int wave5_vpu_enc_job_ready(',
        'static int wave5_stop_retained_sources(', 'static void streamoff_output(',
        'static void streamoff_capture(', 'static void wave5_vpu_enc_stop_streaming('))
    code.write_text(scheduler + functions + behavior)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
