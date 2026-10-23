#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise selection bypasses and close/IRQ ordering using actual driver C."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).parent
encoder = (root / 'wave5-vpu-enc.c').read_text()
hardware = (root / 'wave5-hw.c').read_text()
selection = encoder[encoder.index('static int wave5_vpu_enc_s_selection('):
                    encoder.index('static int wave5_vpu_enc_encoder_cmd(')]
release = encoder[encoder.index('static int wave5_vpu_enc_release('):
                  encoder.index('static long wave5_vpu_enc_ioctl(')]
# Compile the real validation prefix through the geometry checks. Later
# profile/GOP checks are independent and have their own test harnesses.
validation = hardware[hardware.index('int wave5_vpu_enc_check_open_param('):]
validation = validation[:validation.index('\n\tif (param->profile)')] + '\nreturn 0;\n}\n'
limits = '\n'.join(line for line in ((root / 'wave5-vpuconfig.h').read_text() +
                                    (root / 'wave5-vpuapi.h').read_text()).splitlines()
                   if line.startswith(('#define W5_MIN_ENC_PIC_', '#define W5_MAX_ENC_PIC_',
                                       '#define MAX_NUM_INSTANCE', '#define MAX_BIT_RATE')))
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef int32_t s32;
#define W_AVC_ENC 1
#define W_HEVC_ENC 2
#define PRODUCT_ID_521 521
#define V4L2_BUF_TYPE_VIDEO_OUTPUT 2
#define V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE 10
#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 9
#define V4L2_SEL_TGT_CROP 0
#define V4L2_SEL_FLAG_GE 1
#define V4L2_SEL_FLAG_LE 2
#define VPU_INST_STATE_NONE 0
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define IS_ALIGNED(x,a) (!((x) & ((a)-1)))
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
struct v4l2_rect { int left, top; unsigned width, height; };
struct v4l2_selection { unsigned type, target, flags; struct v4l2_rect r; };
struct vpu_attr { bool support_hevc10bit_enc, support_avc10bit_enc; };
struct vpu_device { int product, dev_lock; struct vpu_attr attr; bool isolated, legacy_firmware; };
struct v4l2_m2m_ctx { bool running, linked, output, capture, output_busy, capture_busy; };
static bool *v4l2_m2m_get_src_vq(struct v4l2_m2m_ctx *ctx) { return ctx?&ctx->output_busy:NULL; }
static bool *v4l2_m2m_get_dst_vq(struct v4l2_m2m_ctx *ctx) { return ctx?&ctx->capture_busy:NULL; }
static bool vb2_is_busy(bool *queue) { return queue && *queue; }
struct vpu_instance {
    bool released;
    struct vpu_device *dev;
    int std, id, state;
    unsigned frame_rate;
    struct { unsigned width, height; } src_fmt, dst_fmt;
    struct v4l2_rect conf_win;
    struct { struct v4l2_m2m_ctx *m2m_ctx; } v4l2_fh;
};
struct file { struct vpu_instance *inst; void *private_data; };
static bool wave5_vpu_isolated(struct vpu_device *dev) { return dev->isolated; }
static void wave5_vpu_isolate(struct vpu_device *dev, const char *reason, u32 command)
{ (void)reason; (void)command; dev->isolated = true; }
#define v4l2_fh_del(...) ((void)0)
#define v4l2_fh_exit(...) ((void)0)
struct enc_wave_param { unsigned internal_bit_depth; };
struct enc_open_param {
    unsigned pic_width, pic_height, frame_rate_info, bit_rate;
    struct enc_wave_param wave_param;
};
static struct vpu_instance *file_to_vpu_inst(struct file *f) { return f->inst; }
static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static int wave5_vpu_enc_close(struct vpu_instance *inst, u32 *reason)
{ (void)inst; *reason = 0; return 0; }
static void cancel_job(struct v4l2_m2m_ctx *ctx)
{
    if (ctx->running) {
        /* Inject completion precisely when cancellation starts waiting.
         * Removing the instance from dispatch first loses this IRQ. */
        assert(ctx->linked);
        ctx->running = false;
    }
}
static int v4l2_m2m_streamoff(struct file *f, struct v4l2_m2m_ctx *ctx, int type)
{
    assert(f->inst->dev->dev_lock);
    cancel_job(ctx);
    bool *streaming = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ?
                      &ctx->output : &ctx->capture;
    bool was_streaming = *streaming;
    *streaming = false;
    return was_streaming ? 0 : -EINVAL;
}
static int wave5_vpu_release_device(struct file *f,
        int (*close_func)(struct vpu_instance *, u32 *), char *name)
{
    struct v4l2_m2m_ctx *ctx = f->inst->v4l2_fh.m2m_ctx;
    u32 reason;
    (void)name;
    ctx->linked = false;
    cancel_job(ctx);
    assert(!ctx->output && !ctx->capture);
    return close_func(f->inst, &reason);
}
'''
checks = r'''
int main(void)
{
    struct vpu_device dev = {.product=PRODUCT_ID_521};
    struct vpu_instance inst = {.dev=&dev,.src_fmt={1920,1088},.dst_fmt={1920,1080},
                                .conf_win={0,0,1920,1080}};
    struct file file = {.inst=&inst};
    const unsigned dimensions[][2] = {
        {1366,768}, {1440,900}, {1680,1050}, {1920,1080},
        {1368,768}, {640,480}, {0,768}, {640,0}, {1367,768}, {640,479}
    };
    for (int codec=W_AVC_ENC; codec<=W_HEVC_ENC; codec++) {
        inst.std = codec;
        for (unsigned i=0; i<sizeof(dimensions)/sizeof(dimensions[0]); i++) {
            unsigned w=dimensions[i][0], h=dimensions[i][1];
            bool supported = w >= W5_MIN_ENC_PIC_WIDTH && h >= W5_MIN_ENC_PIC_HEIGHT &&
                             !(w % 2) && !(h % 2);
            struct v4l2_rect before = inst.conf_win;
            struct v4l2_selection s = {.type=V4L2_BUF_TYPE_VIDEO_OUTPUT,
                                       .target=V4L2_SEL_TGT_CROP,.r={0,0,w,h}};
            int ret = wave5_vpu_enc_s_selection(&file,NULL,&s);
            assert(ret == (supported ? 0:-EINVAL));
            if (!supported) assert(!memcmp(&inst.conf_win,&before,sizeof(before)));
            else assert(inst.conf_win.width==w && inst.conf_win.height==h);
            /* The firmware guard still rejects unpadded internal callers. */
            struct enc_open_param p = {.pic_width=w,.pic_height=h,.frame_rate_info=60};
            bool aligned = supported && !(w % (codec==W_HEVC_ENC ? 8:2)) &&
                           !(h % (codec==W_HEVC_ENC ? 8:2));
            assert(wave5_vpu_enc_check_open_param(&inst,&p) == (aligned ? 0:-EINVAL));
        }
    }
    struct v4l2_rect saved = inst.conf_win;
    struct v4l2_selection offset = {.type=V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .target=V4L2_SEL_TGT_CROP,.r={2,4,1366,768}};
    assert(!wave5_vpu_enc_s_selection(&file,NULL,&offset));
    assert(inst.conf_win.left==2 && inst.conf_win.top==4);
    inst.state=1;
    assert(wave5_vpu_enc_s_selection(&file,NULL,&offset)==-EBUSY);
    inst.state=0;
    struct v4l2_m2m_ctx allocated={.output_busy=true};
    inst.v4l2_fh.m2m_ctx=&allocated;
    assert(wave5_vpu_enc_s_selection(&file,NULL,&offset)==-EBUSY);
    allocated.output_busy=false; allocated.capture_busy=true;
    assert(wave5_vpu_enc_s_selection(&file,NULL,&offset)==-EBUSY);
    inst.v4l2_fh.m2m_ctx=NULL;
    offset.r.left=1920;
    assert(wave5_vpu_enc_s_selection(&file,NULL,&offset)==-ERANGE);
    offset.r.left=-2;
    assert(wave5_vpu_enc_s_selection(&file,NULL,&offset)==-EINVAL);
    inst.conf_win=saved;
    /* Close before STREAMON, during a job, and after either STREAMOFF order. */
    for (int output=0; output<2; output++)
    for (int capture=0; capture<2; capture++)
    for (int running=0; running<2; running++) {
        struct v4l2_m2m_ctx ctx = {.linked=true,.running=running,
                                   .output=output,.capture=capture};
        inst.v4l2_fh.m2m_ctx=&ctx;
        assert(!wave5_vpu_enc_release(&file));
        assert(!ctx.running && !ctx.linked && !dev.dev_lock);
    }
    /* Quarantined close must not release a queue or issue DESTROY. */
    struct v4l2_m2m_ctx pinned = {.linked=true,.running=true,.output=true,.capture=true};
    inst.v4l2_fh.m2m_ctx = &pinned;
    dev.isolated = true;
    file.private_data = &inst;
    assert(!wave5_vpu_enc_release(&file));
    assert(pinned.linked && pinned.running && pinned.output && pinned.capture);
    assert(!file.private_data && !dev.dev_lock);
    puts("Wave5 selection/firmware geometry (20 cases) and close/IRQ ordering (8 cases) passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-hang-') as directory:
    source = Path(directory) / 'hang.c'
    binary = Path(directory) / 'hang'
    source.write_text(prefix + limits + '\n' + selection + validation + release + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function',
                    '-fsanitize=address,undefined',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

interval = encoder[encoder.index('static int wave5_vpu_enc_s_parm('):
                   encoder.index('static int wave5_set_qp_limits(')]
# Keep only the requested function, independent of adjacent control helpers.
interval = interval[:interval.index('\n}\n') + 3]
interval_types = r'''
#define V4L2_CAP_TIMEPERFRAME 0x1000
#define max_t(type,a,b) ((type)(a)>(type)(b)?(type)(a):(type)(b))
struct v4l2_streamparm {
    unsigned type;
    struct { struct { unsigned capability;
        struct { unsigned numerator, denominator; } timeperframe;
    } output; } parm;
};
'''
interval_checks = r'''
int main(void)
{
    struct vpu_instance inst={.frame_rate=30};
    struct file file={.inst=&inst};
    struct v4l2_streamparm parm={.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    assert(!wave5_vpu_enc_s_parm(&file,NULL,&parm));
    assert(parm.parm.output.timeperframe.numerator==1);
    assert(parm.parm.output.timeperframe.denominator==30);
    parm.parm.output.timeperframe.numerator=1001;
    parm.parm.output.timeperframe.denominator=60000;
    assert(!wave5_vpu_enc_s_parm(&file,NULL,&parm));
    assert(inst.frame_rate==59 && parm.parm.output.timeperframe.numerator==1);
    assert(parm.parm.output.timeperframe.denominator==59);
    parm.parm.output.timeperframe.numerator=2;
    parm.parm.output.timeperframe.denominator=1;
    assert(!wave5_vpu_enc_s_parm(&file,NULL,&parm) && inst.frame_rate==1);
    inst.state=1;
    assert(wave5_vpu_enc_s_parm(&file,NULL,&parm)==-EBUSY);
    parm.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    assert(wave5_vpu_enc_s_parm(&file,NULL,&parm)==-EINVAL);
    puts("Wave5 negotiated frame intervals and busy sequence state passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-interval-') as directory:
    source = Path(directory) / 'interval.c'
    binary = Path(directory) / 'interval'
    source.write_text(prefix + interval_types + interval + interval_checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
