#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real JPEG validation, firmware events and DMA close ownership."""
from pathlib import Path
import unittest

from test_t7_lifetime import COMMON, function, run_c

HERE = Path(__file__).resolve().parent


class MJPEGTests(unittest.TestCase):
    def test_jpeg_interface_maps_shared_engines_and_cleans_partial_links(self):
        code = function(HERE / 'aml_vdec_drv.c', 'aml_vdec_register_jpeg_interface')
        run_c(COMMON + """
#define MEDIA_LNK_FL_IMMUTABLE 1
#define MEDIA_LNK_FL_ENABLED 2
#define MEDIA_PAD_FL_SINK 1
#define MEDIA_INTF_T_V4L_VIDEO 1
#define VIDEO_MAJOR 81
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
struct media_entity { int id; };
struct media_pad { struct media_entity *entity; };
struct media_intf_devnode { int intf; };
struct video_device { struct media_entity entity; int minor; };
struct aml_vdec_dev {
    struct video_device *vfd, *jpeg_vfd;
    struct media_intf_devnode *jpeg_intf;
    int mdev;
};
static struct media_entity processor = { 2 }, sink = { 3 };
static struct media_pad proc_pad = { &processor }, sink_pad = { &sink };
static struct media_intf_devnode intf;
static unsigned int links, calls, fail_at, removed;
static struct media_pad *media_entity_remote_pad_unique(struct media_entity *e, unsigned int type)
{ assert(type == MEDIA_PAD_FL_SINK); return e->id == 1 ? &proc_pad : &sink_pad; }
static struct media_intf_devnode *media_devnode_create(int *d, int type, int flags, int major, int minor)
{ assert(major == 81 && minor == 7); return &intf; }
static void *media_create_intf_link(struct media_entity *e, int *i, u32 flags)
{
    calls++;
    assert(i == &intf.intf && flags == 3 && e->id == (calls == 1 ? 1 : 3));
    if (calls == fail_at) return NULL;
    links++; return &intf;
}
static void media_devnode_remove(struct media_intf_devnode *i)
{ assert(i == &intf); links = 0; removed++; }
""" + code + """
int main(void)
{
    struct video_device video = { .entity.id = 1 }, jpeg = { .minor = 7 };
    struct aml_vdec_dev dev = { .vfd = &video, .jpeg_vfd = &jpeg };
    for (fail_at = 1; fail_at <= 2; fail_at++) {
        calls = 0;
        assert(aml_vdec_register_jpeg_interface(&dev) == -ENOMEM);
        assert(!links && !dev.jpeg_intf && removed == fail_at);
    }
    calls = 0; fail_at = 0;
    assert(!aml_vdec_register_jpeg_interface(&dev));
    assert(links == 2 && dev.jpeg_intf == &intf);
    return 0;
}
""")

    def test_drain_preserves_last_frame_and_empties_only_sentinels(self):
        code = '\n'.join(function(HERE / 'aml_vdec.c', name) for name in
                         ('vdec_mark_last', 'vdec_try_decoder_cmd', 'vdec_decoder_cmd'))
        run_c(COMMON + """
#include <linux/videodev2.h>
#define CODEC_TYPE_MJPEG 2
#define AML_FMT_SRC 0
struct vb2_buffer { unsigned int payload; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; unsigned int flags; };
struct queue {
    struct vb2_v4l2_buffer *src, *dst, *last;
    bool draining, stopped;
};
struct aml_vdec_ctx {
    unsigned int curr_dec_type;
    struct { unsigned int codec_type; } dec_fmt[1];
    struct queue *m2m_ctx;
};
struct file { struct aml_vdec_ctx *ctx; };
static struct aml_vdec_ctx *fh_to_dec_ctx(struct file *file) { return file->ctx; }
static struct vb2_v4l2_buffer *v4l2_m2m_next_src_buf(struct queue *q) { return q->src; }
static struct vb2_v4l2_buffer *v4l2_m2m_next_dst_buf(struct queue *q) { return q->dst; }
static bool v4l2_m2m_is_last_draining_src_buf(struct queue *q, struct vb2_v4l2_buffer *b)
{ return q->draining && q->last == b; }
static void v4l2_m2m_mark_stopped(struct queue *q) { q->stopped = true; q->draining = false; }
static void vb2_set_plane_payload(struct vb2_buffer *b, int plane, unsigned int size)
{ b->payload = size; }
static int v4l2_m2m_ioctl_try_decoder_cmd(struct file *f, void *p, struct v4l2_decoder_cmd *c)
{ return c->cmd == V4L2_DEC_CMD_STOP || c->cmd == V4L2_DEC_CMD_START ? 0 : -EINVAL; }
static int v4l2_m2m_ioctl_stateless_try_decoder_cmd(struct file *f, void *p,
                                                 struct v4l2_decoder_cmd *c)
{ return c->cmd == V4L2_DEC_CMD_FLUSH ? 0 : -EINVAL; }
static int v4l2_m2m_ioctl_stateless_decoder_cmd(struct file *f, void *p,
                                             struct v4l2_decoder_cmd *c)
{ return v4l2_m2m_ioctl_stateless_try_decoder_cmd(f, p, c); }
static int v4l2_m2m_ioctl_decoder_cmd(struct file *f, void *p, struct v4l2_decoder_cmd *c)
{ return v4l2_m2m_ioctl_try_decoder_cmd(f, p, c); }
""" + code + """
int main(void)
{
    struct vb2_v4l2_buffer src = {}, dst = { .vb2_buf.payload = 1000 }, next = {};
    struct queue q = { .src = &src, .dst = &dst, .last = &next, .draining = true };
    struct aml_vdec_ctx ctx = { .curr_dec_type = CODEC_TYPE_MJPEG,
        .dec_fmt[0].codec_type = CODEC_TYPE_MJPEG, .m2m_ctx = &q };
    struct file file = { &ctx };
    struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_STOP };
    assert(!vdec_decoder_cmd(&file, NULL, &cmd));
    assert(dst.vb2_buf.payload == 1000);
    vdec_mark_last(&ctx);
    assert(!dst.flags && !q.stopped);
    q.last = &src;
    vdec_mark_last(&ctx);
    assert(dst.flags & V4L2_BUF_FLAG_LAST);
    assert(q.stopped && dst.vb2_buf.payload == 1000);
    q.src = NULL;
    assert(!vdec_decoder_cmd(&file, NULL, &cmd));
    assert(!dst.vb2_buf.payload);
    cmd.cmd = V4L2_DEC_CMD_FLUSH;
    assert(vdec_try_decoder_cmd(&file, NULL, &cmd) == -EINVAL);
    ctx.dec_fmt[0].codec_type = 0;
    assert(!vdec_decoder_cmd(&file, NULL, &cmd));
    cmd.cmd = V4L2_DEC_CMD_STOP;
    assert(vdec_decoder_cmd(&file, NULL, &cmd) == -EINVAL);
    return 0;
}
""")

    def test_header_truncation_sampling_and_capture_bounds(self):
        code = function(HERE / 'mjpeg.c', 'mjpeg_supported')
        run_c(COMMON + """
#include <string.h>
typedef uint8_t u8;
""" + code + """
int main(void)
{
    u8 jpeg[] = { 0xff, 0xd8, 0xff, 0xfe, 0, 4, 0, 0,
                  0xff, 0xc0, 0, 17, 8, 1, 0x70, 2, 0x80, 3,
                  1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 };
    assert(mjpeg_supported(jpeg, sizeof(jpeg), 640, 384));
    for (unsigned int n = 0; n < sizeof(jpeg); n++)
        assert(!mjpeg_supported(jpeg, n, 640, 384));
    assert(!mjpeg_supported(NULL, sizeof(jpeg), 640, 384));
    assert(!mjpeg_supported(jpeg, sizeof(jpeg), 639, 384));
    assert(!mjpeg_supported(jpeg, sizeof(jpeg), 640, 367));
    jpeg[9] = 0xc2;
    assert(!mjpeg_supported(jpeg, sizeof(jpeg), 640, 384));
    jpeg[9] = 0xc1;
    assert(mjpeg_supported(jpeg, sizeof(jpeg), 640, 384));
    jpeg[19] = 0x21;
    assert(!mjpeg_supported(jpeg, sizeof(jpeg), 640, 384));
    jpeg[19] = 0x22; jpeg[12] = 12;
    assert(!mjpeg_supported(jpeg, sizeof(jpeg), 640, 384));
    jpeg[12] = 8; jpeg[5] = 1;
    assert(!mjpeg_supported(jpeg, sizeof(jpeg), 640, 384));
    jpeg[5] = 0xff;
    assert(!mjpeg_supported(jpeg, sizeof(jpeg), 640, 384));
    return 0;
}
""")

    def test_irq_errors_do_not_complete_buffers_or_free_dma(self):
        code = '\n'.join(function(HERE / 'mjpeg.c', name) for name in
                         ('mjpeg_signal_done', 'mjpeg_threaded_isr'))
        run_c(COMMON + """
#include <linux/videodev2.h>
#include "reg_defines.h"
#include "mjpeg.h"
#define DOS_BUS 0
#define AML_FMT_DST 1
#define IRQ_HANDLED 1
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
typedef int irqreturn_t;
struct aml_vdec_hw { void *map[1]; void *curr_ctx; };
struct aml_vdec_dev { struct aml_vdec_hw *dec_hw; };
struct aml_vdec_ctx {
    int decode_result, int_cond, queue;
    struct v4l2_pix_format_mplane pix_fmt[2];
};
struct aml_mjpeg_ctx { struct aml_vdec_ctx *v4l2_ctx; };
static unsigned int status, width = 640, height = 368, index, wakeups, acks;
static u32 read_dos_reg(struct aml_vdec_hw *hw, u32 reg)
{
    switch (reg) {
    case MJPEG_DEC_STATUS: return status;
    case MJPEG_PIC_WIDTH: return width;
    case MJPEG_PIC_HEIGHT: return height;
    case AV_SCRATCH_5: return index;
    default: return 0;
    }
}
static void regmap_write(void *map, u32 reg, u32 value)
{ if (reg == MJPEG_DEC_STATUS && !value) acks++; }
static void wake_up_interruptible(int *queue) { wakeups++; }
""" + code + """
int main(void)
{
    struct aml_vdec_ctx ctx = { .pix_fmt[1] = { .width = 640, .height = 384 } };
    struct aml_mjpeg_ctx m = { &ctx };
    struct aml_vdec_hw hw = { .curr_ctx = &m };
    struct aml_vdec_dev dev = { &hw };
    status = MJPEG_STATUS_SIZE;
    mjpeg_threaded_isr(0, &dev);
    assert(acks == 1 && !wakeups && !ctx.int_cond);
    width = 641;
    mjpeg_threaded_isr(0, &dev);
    assert(ctx.decode_result == -EINVAL && wakeups == 1 && acks == 1);
    ctx.int_cond = 0; status = MJPEG_STATUS_EMPTY;
    mjpeg_threaded_isr(0, &dev);
    assert(ctx.decode_result == -EIO && wakeups == 2);
    mjpeg_threaded_isr(0, &dev);
    assert(wakeups == 2);
    ctx.int_cond = 0; status = MJPEG_STATUS_TIMEOUT;
    mjpeg_threaded_isr(0, &dev);
    assert(ctx.decode_result == -EIO && wakeups == 3);
    ctx.int_cond = 0; status = 0x1234;
    mjpeg_threaded_isr(0, &dev);
    assert(ctx.decode_result == -EIO);
    ctx.int_cond = 0; status = 0; index = 1;
    mjpeg_threaded_isr(0, &dev);
    assert(ctx.decode_result == -EIO);
    ctx.int_cond = 0; index = 0;
    mjpeg_threaded_isr(0, &dev);
    assert(!ctx.decode_result && ctx.int_cond && wakeups == 6);
    return 0;
}
""")

    def test_failed_stop_quarantines_jpeg_staging_buffer(self):
        code = function(HERE / 'mjpeg.c', 'aml_mjpeg_exit')
        run_c(COMMON + """
struct aml_vdec_hw { bool faulted; void *dev; };
struct aml_mjpeg_ctx { void *stage_vaddr; unsigned long stage_size, stage_paddr; };
struct aml_vdec_ctx {
    struct aml_vdec_hw *dev;
    struct aml_mjpeg_ctx *codec_priv;
    bool dos_clk_en;
};
static int stop_error, stops, dma_frees, heap_frees, finishes;
static struct aml_vdec_hw *vdec_get_hw(struct aml_vdec_hw *h) { return h; }
static int aml_stop_vdec_hw(struct aml_vdec_hw *hw) { stops++; return stop_error; }
static void aml_vdec_fault(struct aml_vdec_ctx *ctx) { ctx->dev->faulted = true; }
static void aml_mjpeg_finish(void *p) { finishes++; }
static void dma_free_coherent(void *d, unsigned long s, void *p, unsigned long a) { dma_frees++; }
static void kfree(void *p) { heap_frees++; }
""" + code + """
int main(void)
{
    struct aml_vdec_hw hw = {};
    struct aml_mjpeg_ctx m = { .stage_vaddr = &m };
    struct aml_vdec_ctx ctx = { &hw, &m, true };
    stop_error = -ETIMEDOUT;
    aml_mjpeg_exit(&ctx);
    assert(hw.faulted && ctx.codec_priv == &m && stops == 1);
    assert(!dma_frees && !heap_frees && !finishes);
    aml_mjpeg_exit(&ctx);
    assert(stops == 1 && !dma_frees);
    hw.faulted = false; stop_error = 0;
    aml_mjpeg_exit(&ctx);
    assert(!ctx.codec_priv && !ctx.dos_clk_en && finishes == 1);
    assert(dma_frees == 1 && heap_frees == 1);
    return 0;
}
""")


if __name__ == '__main__':
    unittest.main()
