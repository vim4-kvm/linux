#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inject DMA/firmware failures into the actual encoder registration and close."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).parent
hardware = (root / 'wave5-hw.c').read_text()
register = hardware[hardware.index('static u32 calculate_luma_stride'):
                    hardware.index('static u32 wave5_vpu_enc_validate_sec_axi')]
api = (root / 'wave5-vpuapi.c').read_text()
close = api[api.index('int wave5_vpu_enc_close('):
            api.index('int wave5_vpu_enc_register_frame_buffer(')]
# Register values and buffer sizing do not affect allocation ownership.
registers = sorted(set(re.findall(r'\bW5_[A-Z_0-9]+', register)))
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
#define ALIGN(x, n) (((x) + (n) - 1) & ~((n) - 1))
#define DIV_ROUND_UP(x, n) (((x) + (n) - 1) / (n))
#define BUFFER_MARGIN 4096
#define VLC_BUF_NUM 2
#define WAVE521_COMMAND_QUEUE_DEPTH 2
#define MAX_FIRMWARE_CALL_RETRY 3
#define WAVE5_SYSERR_VPU_STILL_RUNNING 1
#define W_AVC_ENC 1
#define W_HEVC_ENC 2
#define MIRDIR_HOR_VER 3
#define WAVE5_ENC_AVC_BUF_SIZE(w, h) ((w) * (h))
#define WAVE5_ENC_HEVC_BUF_SIZE(w, h) ((w) * (h))
#define WAVE5_FBC_LUMA_TABLE_SIZE(w, h) ((w) * (h))
#define WAVE5_FBC_CHROMA_TABLE_SIZE(w, h) ((w) * (h))
#define WAVE5_SUBSAMPLED_ONE_SIZE_AVC(w, h) ((w) * (h))
#define WAVE5_SUBSAMPLED_ONE_SIZE(w, h) ((w) * (h))
#define dev_dbg(...) ((void)0)
#define dev_warn(...) ((void)0)
enum tiled_map_type { COMPRESSED_FRAME_MAP };
struct vpu_buf { size_t size; void *vaddr; uintptr_t daddr; };
struct enc_open_param {
    u32 pic_width, pic_height;
    struct { u32 internal_bit_depth; } wave_param;
};
struct enc_info {
    struct enc_open_param open_param;
    u32 stride, rotation_angle, mirror_direction, vlc_buf_size, param_buf_size;
    struct vpu_buf vb_work, vb_mv, vb_fbc_y_tbl, vb_fbc_c_tbl, vb_sub_sam_buf, vb_task;
};
struct codec_info { struct enc_info enc_info; };
struct device { void *data; };
struct vpu_device { struct device *dev; int hw_lock; bool legacy_firmware; };
struct vpu_instance { struct vpu_device *dev; struct codec_info *codec_info; int std; };
struct frame_buffer { uintptr_t buf_y, buf_cb; };
static unsigned attempts, fail_at, live, commands;
static bool firmware_owns, fail_command, fail_queue, fail_close;
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static int mutex_lock_interruptible(int *lock) { (void)lock; return 0; }
static void mutex_unlock(int *lock) { (void)lock; }
static void pm_runtime_resume_and_get(struct device *dev) { (void)dev; }
static void pm_runtime_put_sync(struct device *dev) { (void)dev; }
static void pm_runtime_put_autosuspend(struct device *dev) { (void)dev; }
static void wave5_vpu_isolate(struct vpu_device *dev, const char *reason, u32 command)
{ (void)dev; (void)reason; (void)command; }
static void vpu_write_reg(struct vpu_device *dev, u32 reg, uintptr_t value)
{ (void)dev; (void)reg; (void)value; }
static int wave5_vdi_allocate_dma_memory(struct vpu_device *dev, struct vpu_buf *vb)
{
    (void)dev;
    assert(!vb->vaddr);
    if (++attempts == fail_at) return -ENOMEM;
    vb->vaddr = malloc(vb->size);
    assert(vb->vaddr);
    vb->daddr = (uintptr_t)vb->vaddr;
    live++;
    return 0;
}
static int wave5_vdi_free_dma_memory(struct vpu_device *dev, struct vpu_buf *vb)
{
    (void)dev;
    assert(!firmware_owns);
    if (vb->vaddr) { assert(live); live--; free(vb->vaddr); }
    memset(vb, 0, sizeof(*vb));
    return 0;
}
static int send_firmware_command(struct vpu_instance *inst, int cmd, bool wait,
                                 void *a, void *b)
{
    (void)inst; (void)cmd; (void)wait; (void)a; (void)b;
    commands++;
    firmware_owns = true;
    return fail_command ? -ETIMEDOUT : 0;
}
static int wave5_vpu_firmware_command_queue_error_check(struct vpu_device *dev, void *reason)
{ (void)dev; (void)reason; return fail_queue ? -EIO : 0; }
static int wave5_vpu_enc_finish_seq(struct vpu_instance *inst, u32 *fail_res)
{
    (void)inst; (void)fail_res;
    if (fail_close) return -ETIMEDOUT;
    firmware_owns = false;
    return 0;
}
'''
checks = r'''
int main(void)
{
    struct device dev;
    struct vpu_device vpu = { .dev = &dev };
    struct frame_buffer frames[10] = {0};
    dev.data = &vpu;
    for (int codec = W_AVC_ENC; codec <= W_HEVC_ENC; codec++)
    for (int existing_task = 0; existing_task < 2; existing_task++)
    for (unsigned failure = 0; failure <= 7; failure++) {
        struct codec_info info = { .enc_info = {
            .open_param = { .pic_width = 3840, .pic_height = 2160,
                            .wave_param = { .internal_bit_depth = 8 } },
            .stride = 3840, .vlc_buf_size = 256, .param_buf_size = 128,
        } };
        struct vpu_instance inst = { .dev = &vpu, .codec_info = &info, .std = codec };
        u32 reason;
        attempts = 0; fail_at = 0; commands = 0;
        firmware_owns = false; fail_command = false; fail_queue = false;
        if (existing_task) {
            info.enc_info.vb_task.size = 256;
            assert(!wave5_vdi_allocate_dma_memory(&vpu, &info.enc_info.vb_task));
        }
        attempts = 0;
        fail_at = failure <= 5 ? failure : 0;
        fail_command = failure == 6;
        fail_queue = failure == 7;
        int ret = wave5_vpu_enc_register_framebuffer(&dev, &inst, frames,
                                                    COMPRESSED_FRAME_MAP, 10);
        if (failure && !(failure == 5 && existing_task)) assert(ret < 0);
        else assert(!ret);
        if (commands) {
            assert(live == 5);
            // A failed destroy must retain every DMA allocation for hardware.
            fail_close = true;
            assert(wave5_vpu_enc_close(&inst, &reason) == -ETIMEDOUT);
            assert(live == 5);
        } else {
            assert(live == (unsigned)existing_task);
        }
        fail_close = false;
        assert(!wave5_vpu_enc_close(&inst, &reason));
        assert(!live);
    }
    puts("Wave5 allocation rollback, firmware ownership and close passed (32 cases)");
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'cleanup.c'
    test.write_text(harness + '\nenum { ' + ', '.join(registers) + ' };\n'
                    + register + close + checks)
    executable = Path(directory) / 'cleanup'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-O1', '-g', '-fsanitize=address,undefined', str(test),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
