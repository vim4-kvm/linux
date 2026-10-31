#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run vb2's actual export callback twice against one allocator."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
text = (root / 'common/videobuf2/videobuf2-dma-contig.c').read_text()
start = text.index('static struct dma_buf *vb2_dc_get_dmabuf(')
function = text[start:text.index('\n}', start) + 2]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
struct vb2_buffer {int unused;};
struct vb2_dc_buf {void *sgt_base; unsigned size, refcount;};
struct dma_buf {void *priv; unsigned inode;};
struct export_info {void *ops, *priv; unsigned size; unsigned long flags;};
static int vb2_dc_dmabuf_ops;
#define DEFINE_DMA_BUF_EXPORT_INFO(name) struct export_info name={0}
#define WARN_ON(x) (x)
#define IS_ERR(x) false
#define refcount_inc(p) (++*(p))
static void *vb2_dc_get_base_sgt(struct vb2_dc_buf *b) {return b;}
static struct dma_buf *dma_buf_export(struct export_info *i) {
 static unsigned serial;
 struct dma_buf *d=malloc(sizeof(*d)); assert(d);
 *d=(struct dma_buf){i->priv, ++serial}; return d;
}
'''
checks = r'''
int main(void) {
 struct vb2_dc_buf owner={.size=12443648,.refcount=1};
 struct dma_buf *lease=vb2_dc_get_dmabuf(0,&owner,0);
 struct dma_buf *exported=vb2_dc_get_dmabuf(0,&owner,0);
 assert(lease!=exported && lease->inode!=exported->inode);
 assert(lease->priv==exported->priv && lease->priv==&owner);
 assert(owner.refcount==3);
 free(lease); free(exported);
}
'''
with tempfile.TemporaryDirectory() as directory:
    source, binary = Path(directory)/'exports.c', Path(directory)/'exports'
    source.write_text(fixture + function + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Actual vb2 dual export: distinct files, shared allocator identity: PASS')
