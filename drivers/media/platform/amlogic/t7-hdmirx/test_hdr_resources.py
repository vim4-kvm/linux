#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual PQ writer and pool limits without hardware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
text = (root / 't7-vdin-test.c').read_text()
start = text.index('static int t7_vdin_queue_setup(')
queue = text[start:text.index('\n}', start) + 2]
fixture = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
typedef uint32_t u32;
#define BIT(n) (1u << (n))
#define VDIN_DMA_SLOTS 4
#define VDIN_HDR_MATRIX_IN 0x12bb
#define VDIN_HDR_MATRIX_OUT 0x12bc
#define VDIN_HDR_CTRL 0x1280
#define VDIN_MATRIX_CTRL 0x1210
struct t7_vdin {u32 hdr_gain[149], hdr_oetf[149]; bool hdr_curve_pending, faulted; unsigned allocation_size;};
struct device {int unused;};
struct vb2_queue {struct t7_vdin *priv; unsigned allocated;};
static void *vb2_get_drv_priv(struct vb2_queue *q) {return q->priv;}
static unsigned vb2_get_num_buffers(struct vb2_queue *q) {return q->allocated;}
static u32 regs[0x1300];
static void t7_vdin_write(struct t7_vdin *v, u32 reg, u32 value) {(void)v; regs[reg]=value;}
#include "t7-vdin-hdr.h"
'''
fixture += queue
fixture += r'''
int main(void) {
 struct t7_vdin v={.allocation_size=12443648}; struct vb2_queue q={&v,0};
 unsigned n=64,p=0,s[1]={0};
 assert(!t7_vdin_queue_setup(&q,&n,&p,s,0) && n==16 && p==1 && s[0]==v.allocation_size);
 n=14;
 assert(!t7_vdin_queue_setup(&q,&n,&p,s,0) && n==14);
 q.allocated=15; n=64; p=1;
 assert(!t7_vdin_queue_setup(&q,&n,&p,s,0) && n==1);
 q.allocated=16; n=1;
 assert(t7_vdin_queue_setup(&q,&n,&p,s,0)==-ENOBUFS);
 q.allocated=0; s[0]=1;
 assert(t7_vdin_queue_setup(&q,&n,&p,s,0)==-EINVAL);
 v.faulted=true; n=64;
 assert(t7_vdin_queue_setup(&q,&n,&p,s,0)==-EIO && n==64);
 t7_vdin_hdr_configure(&v);
 assert(regs[0x128d]==(u32)-512);
 /* T7 precise coefficients from ncl_2020_709, not the old rounded /256 table. */
 const int vendor[9]={3401,-1204,-149,-255,2320,-17,-37,-206,2291};
 assert(regs[0x12b0]==(BIT(4)|11));
 for(unsigned i=0;i<4;i++)
  assert(regs[0x12b1+i]==((u32)(vendor[2*i+1]&65535)<<16 | (vendor[2*i]&65535)));
 assert(regs[0x12b5]==2291);
}
'''
with tempfile.TemporaryDirectory() as temp:
    source, exe = Path(temp) / 'test.c', Path(temp) / 'test'
    source.write_text(fixture)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all', '-I', str(root),
                    str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Actual PQ precise gamut and REQBUFS/CREATE_BUFS/fault budget: PASS')
