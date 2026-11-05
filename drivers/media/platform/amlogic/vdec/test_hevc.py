#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check hardware-parameter boundaries before shifts, tile indexing or DMA setup."""
from pathlib import Path
import re
import unittest

from test_t7_lifetime import COMMON, function, run_c

HERE = Path(__file__).resolve().parent


class HEVCParameterTests(unittest.TestCase):
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
    u32 pic_w, pic_h;
    int ref_poc_l0[H265_MAX_REF], ref_poc_l1[H265_MAX_REF];
};
''' + function(HERE / 'hevc.c', 'h265_validate_params') + '\n' +
              function(HERE / 'hevc.c', 'h265_update_tiles') + '\n' +
              '\n'.join(function(HERE / 'hevc.c', name) for name in (
                  'h265_pic_by_poc', 'h265_rps_poc', 'h265_ref_entry', 'h265_set_ref_lists')) + r'''
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
    reset(); h.p[P_PIC_WIDTH] = h.p[P_PIC_HEIGHT] = 4096;
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
