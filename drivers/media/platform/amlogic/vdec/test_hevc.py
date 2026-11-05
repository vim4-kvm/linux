#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check hardware-parameter boundaries before shifts, tile indexing or DMA setup."""
from pathlib import Path
import re
import unittest

from test_t7_lifetime import COMMON, function, run_c

HERE = Path(__file__).resolve().parent


class HEVCParameterTests(unittest.TestCase):
    def test_reordered_frames_keep_their_storage_until_output_and_drain(self):
        source = (HERE / 'hevc.c').read_text()
        picture = re.search(r'struct h265_pic \{.*?\n\};', source, re.S).group()
        functions = '\n'.join(function(HERE / 'hevc.c', name) for name in (
            'h265_free_slot', 'h265_next_output', 'h265_output', 'h265_flush_pending'))
        run_c(COMMON + r'''
#define H265_SLOTS 18
#define V4L2_BUF_FLAG_LAST 1
typedef uint64_t dma_addr_t;
enum vb2_buffer_state { VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR };
struct vb2_v4l2_buffer { u32 sequence, flags, timestamp; };
''' + picture + r'''
struct aml_hevc_ctx {
    struct h265_pic pic[H265_SLOTS];
    int nslots;
    u32 sequence;
    unsigned int pending;
};
static u32 timestamps[18], flags[18];
static unsigned int count;
static enum vb2_buffer_state expected;
static void v4l2_m2m_buf_done(struct vb2_v4l2_buffer *vb, enum vb2_buffer_state state)
{
    assert(state == expected && vb->sequence == count);
    timestamps[count] = vb->timestamp; flags[count++] = vb->flags;
}
''' + functions + r'''
int main(void)
{
    struct aml_hevc_ctx h = { .nslots = 5, .pending = 5 };
    const int pocs[] = { 0, 4, 2, 1, 0 };
    struct vb2_v4l2_buffer frames[5] = { 0 };
    for (unsigned int i = 0; i < 5; i++) {
        frames[i].timestamp = 100 + i;
        h.pic[i].valid = true;
        h.pic[i].poc = pocs[i];
        h.pic[i].epoch = i == 4 ? 2 : 1;
        h.pic[i].output = &frames[i];
    }
    assert(h265_free_slot(&h) == -ENOBUFS); /* Non-reference output is still held. */
    h.pic[0].referenced = true;
    assert(h265_next_output(&h) == 0);
    h265_output(&h, 0, VB2_BUF_STATE_DONE, false);
    assert(h265_free_slot(&h) == -ENOBUFS); /* Displayed reference is still held. */
    h.pic[0].referenced = false;
    assert(h265_free_slot(&h) == 0);
    h.nslots = 2; /* A new smaller SPS must not hide old pending pictures. */
    h265_flush_pending(&h, VB2_BUF_STATE_DONE, true);
    assert(count == 5 && !h.pending && h265_next_output(&h) == -1);
    assert(timestamps[0] == 100 && timestamps[1] == 103 && timestamps[2] == 102);
    assert(timestamps[3] == 101 && timestamps[4] == 104);
    for (unsigned int i = 0; i < 5; i++) assert(flags[i] == (i == 4));
    h265_flush_pending(&h, VB2_BUF_STATE_DONE, true);
    assert(count == 5);
    frames[0].flags = 0; h.pic[0].output = &frames[0]; h.pending = 1;
    expected = VB2_BUF_STATE_ERROR;
    h265_flush_pending(&h, VB2_BUF_STATE_ERROR, false);
    assert(count == 6 && !flags[5]);
    return 0;
}
''')

    def test_capture_planes_cover_sao_burst_lengths(self):
        run_c(COMMON + r'''
#include <linux/videodev2.h>
#define H265_MAX_DIM 4096
#define H265_MAX_HEIGHT 2304
#define SZ_64K 65536U
#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))
#define clamp_t(t, v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
static void v4l2_fill_pixfmt_mp(struct v4l2_pix_format_mplane *f, u32 fourcc, u32 w, u32 h)
{
    f->pixelformat = fourcc; f->width = w; f->height = h; f->num_planes = 2;
    f->plane_fmt[0].bytesperline = f->plane_fmt[1].bytesperline = w;
}
''' + function(HERE / 'hevc.c', 'h265_capture_format') + r'''
int main(void)
{
    struct v4l2_pix_format_mplane f = { 0 };
    const u32 sizes[][2] = { { 64, 64 }, { 256, 256 }, { 640, 368 },
                             { 1920, 1080 }, { 4096, 2304 } };
    for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        h265_capture_format(&f, sizes[i][0], sizes[i][1]);
        assert(f.num_planes == 2 && f.width >= sizes[i][0] && f.height >= sizes[i][1]);
        assert(f.plane_fmt[0].sizeimage >= f.width * f.height);
        assert(f.plane_fmt[1].sizeimage >= f.width * f.height / 2);
        assert(!(f.plane_fmt[1].sizeimage % SZ_64K));
        assert(f.plane_fmt[0].sizeimage == 2 * f.plane_fmt[1].sizeimage);
    }
    h265_capture_format(&f, 256, 256);
    assert(f.plane_fmt[0].sizeimage == 131072 && f.plane_fmt[1].sizeimage == 65536);
    return 0;
}
''')

    def test_job_synchronizes_stop_before_returning_dma_ownership(self):
        run_c(COMMON + r'''
#include "hevc.h"
#define H265_TIMEOUT_MS 500
#define READ_ONCE(v) (v)
#define msecs_to_jiffies(v) (v)
#define dev_err(...) ((void)0)
struct vb2_buffer { int unused; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct aml_vdec_hw { bool faulted; u32 hevc_irq_status; };
struct aml_vdec_ctx { void *codec_priv, *m2m_ctx; int int_cond, decode_result, queue; };
struct picture { bool valid, referenced; };
struct aml_hevc_ctx {
    struct aml_vdec_hw *hw;
    bool configured, have_picture, finished;
    int cur, rounds;
    struct picture pic[2];
};
static struct vb2_v4l2_buffer input;
static struct aml_vdec_ctx context;
static unsigned int stage, stop_count;
static int prepare_error, feed_error, stop_error, irq_error, irq_result;
static bool timeout, stopped;
static struct vb2_v4l2_buffer *v4l2_m2m_next_src_buf(void *ctx) { return &input; }
static void *vb2_plane_vaddr(struct vb2_buffer *vb, int i) { return &input; }
static size_t vb2_get_plane_payload(struct vb2_buffer *vb, int i) { return 64; }
static int h265_alloc_workspace(struct aml_hevc_ctx *h) { assert(!stage); return 0; }
static int h265_set_dw_target(struct aml_hevc_ctx *h) { assert(!stage); return 0; }
static int aml_hevc_prepare_hw(struct aml_vdec_hw *hw)
{ assert(!stage); stage = 1; return prepare_error; }
static void h265_irq_thread(void) {}
static int aml_hevc_enable_irq(struct aml_vdec_hw *hw, void (*cb)(void), void *h)
{ assert(stage == 1); stage = 2; return irq_error; }
static void h265_prot_init(struct aml_hevc_ctx *h) { assert(stage == 2); }
static void h265_config_pic_list(struct aml_hevc_ctx *h) { assert(stage == 2); }
static int h265_feed(struct aml_hevc_ctx *h, void *data, size_t size)
{ assert(stage == 2); return feed_error; }
static void wr(struct aml_vdec_hw *hw, u32 reg, u32 value)
{ assert(stage == 2 && !stopped); }
static u32 read_dos_reg(struct aml_vdec_hw *hw, u32 reg) { return 0; }
static unsigned long wait_for_irq(void)
{
    assert(stage == 2);
    context.decode_result = irq_result;
    return !timeout;
}
#define wait_event_timeout(q, c, t) wait_for_irq()
static int aml_hevc_stop_hw(struct aml_vdec_hw *hw)
{
    assert(stage > 0); stop_count++; stopped = true;
    if (stop_error) hw->faulted = true;
    return stop_error;
}
''' + function(HERE / 'hevc.c', 'aml_hevc_dec_run') + r'''
static void reset(struct aml_vdec_hw *hw)
{
    stage = stop_count = 0; stopped = timeout = false;
    prepare_error = feed_error = stop_error = irq_error = irq_result = 0;
    hw->faulted = false;
}
int main(void)
{
    struct aml_vdec_hw hw = { 0 };
    struct aml_hevc_ctx h = { .hw = &hw, .configured = true };
    context.codec_priv = &h;
    for (int result = -EIO; result <= 1; result++) {
        reset(&hw); irq_result = result;
        assert(aml_hevc_dec_run(&context) == result);
        assert(stopped && stop_count == 1);
    }
    reset(&hw); timeout = true;
    assert(aml_hevc_dec_run(&context) == -ETIMEDOUT && stopped);
    reset(&hw); prepare_error = -EACCES;
    assert(aml_hevc_dec_run(&context) == -EACCES && stopped);
    reset(&hw); irq_error = -EBUSY;
    assert(aml_hevc_dec_run(&context) == -EBUSY && stopped);
    reset(&hw); feed_error = -ENOMEM;
    assert(aml_hevc_dec_run(&context) == -ENOMEM && stopped);
    reset(&hw); stop_error = -ETIMEDOUT;
    assert(aml_hevc_dec_run(&context) == -ETIMEDOUT && hw.faulted);
    return 0;
}
''')

    def test_geometry_tiles_and_reference_boundaries(self):
        source = (HERE / 'hevc.c').read_text()
        rpm = re.search(r'enum \{\n\s*P_CUR_RPS.*?\n\};', source, re.S).group()
        tile = re.search(r'struct h265_tile \{.*?\n\};', source, re.S).group()
        picture = re.search(r'struct h265_pic \{.*?\n\};', source, re.S).group()
        defines = '\n'.join(re.findall(r'^#define H265_[^\n]+', source, re.M))
        run_c(COMMON + r'''
#include <string.h>
typedef uint16_t u16;
typedef uint64_t dma_addr_t;
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#include "hevc.h"
''' + defines + '\n' + rpm + '\n' + tile + '\n' + picture + r'''
#define RPS_END 0x8000
#define RPS_LT_BIT 14
#define RPS_USED_BIT 13
struct aml_hevc_ctx {
    u16 p[P_RPM_COUNT];
    bool tile_enabled;
    int num_tile_col, num_tile_row;
    dma_addr_t sao_vb, sao_abv;
    struct h265_tile tile[H265_MAX_TILE_ROWS][H265_MAX_TILE_COLS];
    struct h265_pic pic[H265_SLOTS];
    int curr_poc, slice_type, cur, ref_num_l0, ref_num_l1;
    int prev_tid0_poc, nal_type, temporal_id, poc_random_access;
    u32 pic_w, pic_h;
    int ref_poc_l0[H265_MAX_REF], ref_poc_l1[H265_MAX_REF];
};
''' + function(HERE / 'hevc.c', 'h265_validate_params') + '\n' +
              function(HERE / 'hevc.c', 'h265_update_tiles') + '\n' +
              '\n'.join(function(HERE / 'hevc.c', name) for name in (
                  'h265_pic_by_poc', 'h265_rps_poc', 'h265_ref_entry', 'h265_set_ref_lists',
                  'h265_update_poc')) + r'''
static struct aml_hevc_ctx h;
static void reset(void)
{
    memset(&h, 0, sizeof(h));
    h.p[P_PIC_WIDTH] = 704; h.p[P_PIC_HEIGHT] = 640;
    h.p[P_CHROMA_FORMAT_IDC] = h.p[P_TEMPORAL_ID] = 1;
    h.p[P_LOG2_DIFF_MAX_MIN_CB_SIZE] = 3;
    h.p[P_LOG2_MAX_POC_LSB_MINUS4] = 4;
    h.p[P_SLICE_TYPE] = H265_I_SLICE;
}
int main(void)
{
    reset(); h.temporal_id = 1; h.nal_type = 1; h.prev_tid0_poc = 1;
    h.p[P_POC_LSB] = 255;
    h265_update_poc(&h);
    assert(h.curr_poc == -1 && h.prev_tid0_poc == -1);
    h.p[P_POC_LSB] = 1;
    h265_update_poc(&h);
    assert(h.curr_poc == 1 && h.prev_tid0_poc == 1);
    const unsigned int excluded[] = { 0, 2, 4, 6, 7, 8, 9 };
    for (unsigned int i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
        h.nal_type = excluded[i]; h.p[P_POC_LSB] = 3;
        h265_update_poc(&h);
        assert(h.curr_poc == 3 && h.prev_tid0_poc == 1);
    }
    h.nal_type = H265_NAL_CRA; h.poc_random_access = H265_MAX_INT;
    h.p[P_POC_LSB] = 240;
    h265_update_poc(&h);
    assert(h.curr_poc == 240 && h.prev_tid0_poc == 240);
    h.nal_type = H265_NAL_IDR_N_LP;
    h265_update_poc(&h);
    assert(!h.curr_poc && !h.prev_tid0_poc);
    reset(); assert(!h265_validate_params(h.p));
    h.p[P_BIT_DEPTH] = 0x22;
    assert(h265_validate_params(h.p) == -EOPNOTSUPP);
    reset(); h.p[P_CHROMA_FORMAT_IDC] = 2;
    assert(h265_validate_params(h.p) == -EOPNOTSUPP);
    const unsigned int shifts[] = { P_LOG2_MIN_CB_SIZE_MINUS3,
                                   P_LOG2_DIFF_MAX_MIN_CB_SIZE, P_LOG2_MAX_POC_LSB_MINUS4 };
    for (unsigned int i = 0; i < 3; i++) {
        reset(); h.p[shifts[i]] = UINT16_MAX;
        assert(h265_validate_params(h.p) == -EINVAL);
    }
    reset(); h.p[P_SLICE_SEGMENT_ADDRESS] = 110;
    assert(h265_validate_params(h.p) == -EINVAL);
    reset(); h.p[P_TILES_ENABLED_FLAG] = 3;
    h.p[P_NUM_TILE_COLUMNS_MINUS1] = h.p[P_NUM_TILE_ROWS_MINUS1] = 2;
    assert(!h265_validate_params(h.p));
    h265_update_tiles(&h, 11, 10, 384);
    assert(h.tile[0][0].width == 3 && h.tile[0][1].width == 4 && h.tile[0][2].width == 4);
    assert(h.tile[0][1].start_cu_x == 3 && h.tile[0][2].start_cu_x == 7);
    assert(h.tile[0][0].height == 3 && h.tile[1][0].height == 3 && h.tile[2][0].height == 4);
    h.p[P_TILES_ENABLED_FLAG] = 1;
    assert(h265_validate_params(h.p) == -EINVAL);
    h.p[P_TILE_WIDTH] = h.p[P_TILE_WIDTH + 1] = 2;
    h.p[P_TILE_HEIGHT] = h.p[P_TILE_HEIGHT + 1] = 2;
    assert(!h265_validate_params(h.p));
    h.p[P_TILE_WIDTH + 1] = 10;
    assert(h265_validate_params(h.p) == -EINVAL);
    reset(); h.p[P_TILES_ENABLED_FLAG] = 1; h.p[P_NUM_TILE_ROWS_MINUS1] = 9;
    assert(h265_validate_params(h.p) == -EOPNOTSUPP);
    reset(); h.p[P_PIC_WIDTH] = 4096; h.p[P_PIC_HEIGHT] = 2304;
    h.p[P_LOG2_DIFF_MAX_MIN_CB_SIZE] = 1;
    h.p[P_TILES_ENABLED_FLAG] = 3; h.p[P_NUM_TILE_COLUMNS_MINUS1] = 9;
    assert(h265_validate_params(h.p) == -EOPNOTSUPP);
    reset(); h.curr_poc = 4; h.cur = 17; h.pic_w = 704; h.pic_h = 640;
    h.slice_type = h.p[P_SLICE_TYPE] = H265_B_SLICE;
    h.p[P_NUM_REF_IDX_L0_ACTIVE] = h.p[P_NUM_REF_IDX_L1_ACTIVE] = 3;
    h.p[P_CUR_RPS] = 0x3fff;     /* used short-term -1 */
    h.p[P_CUR_RPS + 1] = 0x2001; /* used short-term +1 */
    h.p[P_CUR_RPS + 2] = 0x7ffd; /* used long-term -3 */
    h.p[P_CUR_RPS + 3] = RPS_END;
    const int pocs[] = { 3, 5, 1 };
    for (unsigned int i = 0; i < 3; i++) {
        h.pic[i].valid = h.pic[i].referenced = true;
        h.pic[i].width = 704; h.pic[i].height = 640; h.pic[i].poc = pocs[i];
    }
    assert(!h265_set_ref_lists(&h));
    assert(h.ref_poc_l0[0] == 3 && h.ref_poc_l0[1] == 5 && h.ref_poc_l0[2] == 1);
    assert(h.ref_poc_l1[0] == 5 && h.ref_poc_l1[1] == 3 && h.ref_poc_l1[2] == 1);
    h.pic[2].valid = false;
    assert(h265_set_ref_lists(&h) == -EINVAL);
    h.pic[2].valid = true;
    h.p[P_MODIFICATION_FLAG] = 1;
    h.p[P_MODIFICATION_LIST] = 2;
    h.p[P_MODIFICATION_LIST + 1] = 0;
    h.p[P_MODIFICATION_LIST + 2] = 1;
    assert(!h265_set_ref_lists(&h));
    assert(h.ref_poc_l0[0] == 1 && h.ref_poc_l0[1] == 3 && h.ref_poc_l0[2] == 5);
    h.p[P_MODIFICATION_LIST] = 3;
    assert(h265_set_ref_lists(&h) == -EINVAL);
    h.p[P_CUR_RPS] = RPS_END;
    assert(h265_set_ref_lists(&h) == -EINVAL);
    h.slice_type = h.p[P_SLICE_TYPE] = H265_I_SLICE;
    assert(!h265_set_ref_lists(&h) && !h.ref_num_l0 && !h.ref_num_l1);
    return 0;
}
''', flags=('-fsanitize=undefined', '-fno-sanitize-recover=all'))
