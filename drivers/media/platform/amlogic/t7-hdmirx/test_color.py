#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual format reporting and protected input classification."""
from pathlib import Path
import math
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent


def function(file, signature):
    text = (root / file).read_text()
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]


pq = (root / 't7-vdin-hdr.h').read_text()
values = list(map(int, re.search(r't7_hdr_srgb\[149\] = \{([^}]+)', pq)[1].replace(',', ' ').split()))
nodes = [0] + [2 ** (i + 3) for i in range(1, 8)]
nodes += [2 ** j + i * 2 ** (j - 2) for j in range(11, 20) for i in range(4)]
nodes += [2 ** j + i * 2 ** (j - 3) for j in range(20, 31) for i in range(8)]
nodes += [2 ** 31 + i * 2 ** 27 for i in range(16)] + [2 ** 32 - 1]
expected = []
for node in nodes:
    linear = node / 2 ** 32
    encoded = 12.92 * linear if linear <= .0031308 else 1.055 * linear ** (1 / 2.4) - .055
    expected.append(math.floor(4095 * encoded + .5))
assert values == expected, 'Keep the existing product sRGB-shaped OETF samples'

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <linux/videodev2.h>
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
struct link {int lock, users; bool ready;};
struct t7_hdmirx {struct link *link; bool lease_hdr, live_hdr;};
struct device {struct t7_hdmirx *rx;};
typedef __u32 u32;
static bool nv12=true;
struct t7_vdin {bool nv12; struct device *receiver; bool direct_dma;};
static unsigned reads;
static void mutex_lock(int *lock) {assert(!*lock); *lock=1;}
static void mutex_unlock(int *lock) {assert(*lock); *lock=0;}
static struct t7_hdmirx *dev_get_drvdata(struct device *dev) {return dev->rx;}
static bool t7_hdmirx_hdr_input(struct t7_hdmirx *rx)
{assert(rx->link->lock && rx->link->ready); reads++; return rx->live_hdr;}
'''
fixture += function('t7-hdmirx.c', 'bool t7_hdmirx_capture_is_hdr(')
fixture += '\n#include "' + str(root.parents[4] / 'include/uapi/linux/amlogic-vdin.h') + '"\n'
fixture += '\n' + function('t7-vdin-test.c', 'static void t7_vdin_fill_fmt(')
fixture += r'''
int main(void) {
 struct link link={0}; struct t7_hdmirx rx={.link=&link};
 struct device dev={&rx}; struct t7_vdin v={.nv12=true,.receiver=&dev};
 struct v4l2_format fmt={0};
 assert(!t7_hdmirx_capture_is_hdr(&dev) && reads==0);
 link.ready=true; rx.live_hdr=true;
 t7_vdin_fill_fmt(&v,&fmt,3840,2160);
 assert(fmt.fmt.pix.xfer_func==V4L2_XFER_FUNC_709);
 assert(fmt.fmt.pix.colorspace==V4L2_COLORSPACE_REC709);
 assert(fmt.fmt.pix.ycbcr_enc==V4L2_YCBCR_ENC_709);
 assert(fmt.fmt.pix.quantization==V4L2_QUANTIZATION_LIM_RANGE);
 assert(fmt.fmt.pix.bytesperline==3840 && fmt.fmt.pix.sizeimage==12441600);
 link.users=1; link.ready=false; rx.lease_hdr=true; rx.live_hdr=false;
 reads=0; t7_vdin_fill_fmt(&v,&fmt,3840,2160);
 assert(fmt.fmt.pix.xfer_func==V4L2_XFER_FUNC_709 && reads==0);
 rx.lease_hdr=false; t7_vdin_fill_fmt(&v,&fmt,3840,2160);
 assert(fmt.fmt.pix.xfer_func==V4L2_XFER_FUNC_709 && reads==0);
 v.nv12=false; nv12=false; t7_vdin_fill_fmt(&v,&fmt,1920,1080);
 assert(fmt.fmt.pix.xfer_func==V4L2_XFER_FUNC_SRGB);
 assert(fmt.fmt.pix.quantization==V4L2_QUANTIZATION_FULL_RANGE);
 rx.link=NULL; assert(!t7_hdmirx_capture_is_hdr(&dev));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    source.write_text(fixture)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Product OETF, legacy 709 video contract and RGB sRGB/full: PASS')
