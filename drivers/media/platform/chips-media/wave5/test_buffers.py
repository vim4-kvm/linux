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
