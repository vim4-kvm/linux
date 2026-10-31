#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run actual AMLY reporting, byte budget and HDMI metadata getters off-board."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent


def function(name, signature):
    text = (root / name).read_text()
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <linux/videodev2.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
static bool nv12=true;
struct t7_vdin {bool direct_dma,nv12,faulted; unsigned frame_capacity, allocation_size;};
struct file {struct t7_vdin *driver;};
static void *video_drvdata(struct file *f) {return f->driver;}
#define strscpy(to,from) snprintf((char *)(to),sizeof(to),"%s",(from))
struct vb2_queue {struct t7_vdin *driver; unsigned count;};
struct device {struct t7_hdmirx *driver;};
struct link {int lock;};
struct t7_hdmirx {struct link *link; u8 drm_packet[30]; uint64_t drm_received_ns;};
static void *vb2_get_drv_priv(struct vb2_queue *q) {return q->driver;}
static unsigned vb2_get_num_buffers(struct vb2_queue *q) {return q->count;}
static struct t7_hdmirx *dev_get_drvdata(struct device *d) {return d->driver;}
static uint64_t ktime_get_ns(void) {return 2000000001;}
static void mutex_lock(int *m) {assert(!*m);*m=1;}
static void mutex_unlock(int *m) {assert(*m);*m=0;}
'''
prefix += '\n#include "' + str(root.parents[4] / 'include/uapi/linux/amlogic-vdin.h') + '"\n'
code = prefix + '\n'.join([
    function('t7-vdin-test.c', 'static int t7_vdin_enum_fmt('),
    function('t7-vdin-test.c', 'static void t7_vdin_fill_fmt('),
    function('t7-vdin-test.c', 'static int t7_vdin_queue_setup('),
    function('t7-hdmirx.c', 'static u16 t7_drm_word('),
    function('t7-hdmirx.c', 'int t7_hdmirx_get_hdr10('),
]) + r'''
int main(void) {
 struct t7_vdin v={.direct_dma=true,.nv12=true};
 struct file file={&v}; struct v4l2_fmtdesc desc={.index=1};
 assert(!t7_vdin_enum_fmt(&file,0,&desc));
 assert(desc.pixelformat==V4L2_PIX_FMT_AMLY && desc.description[0]);
 v.direct_dma=false;
 assert(t7_vdin_enum_fmt(&file,0,&desc)==-EINVAL);
 v.direct_dma=true;
 struct v4l2_format f={.fmt.pix.pixelformat=V4L2_PIX_FMT_AMLY};
 t7_vdin_fill_fmt(&v,&f,3840,2160);
 assert(f.fmt.pix.bytesperline==9600 && f.fmt.pix.sizeimage==20736000);
 assert(f.fmt.pix.xfer_func==V4L2_XFER_FUNC_SMPTE2084);
 assert(f.fmt.pix.colorspace==V4L2_COLORSPACE_BT2020);
 v.frame_capacity=f.fmt.pix.sizeimage;
 v.allocation_size=f.fmt.pix.sizeimage;
 struct vb2_queue q={.driver=&v};
 unsigned buffers=32,planes=0,sizes[1];
 assert(!t7_vdin_queue_setup(&q,&buffers,&planes,sizes,0));
 assert(buffers==8 && planes==1);
 assert((uint64_t)buffers*sizes[0]+v.frame_capacity<=192U*1024*1024);
 q.count=buffers; buffers=1; planes=0;
 assert(t7_vdin_queue_setup(&q,&buffers,&planes,sizes,0)==-ENOBUFS);
 /* A nonaligned active width retains its pixels and pads rows separately. */
 f.fmt.pix.pixelformat=V4L2_PIX_FMT_AMLY;
 t7_vdin_fill_fmt(&v,&f,1366,768);
 assert(f.fmt.pix.width==1366 && f.fmt.pix.bytesperline==3424);
 f.fmt.pix.pixelformat=V4L2_PIX_FMT_NV12;
 t7_vdin_fill_fmt(&v,&f,3840,2160);
 assert(f.fmt.pix.xfer_func==V4L2_XFER_FUNC_709);
 assert(f.fmt.pix.bytesperline==3840);
 struct link link={0}; struct t7_hdmirx rx={.link=&link};
 struct device dev={&rx};
 struct v4l2_ctrl_hdr10_cll_info cll;
 struct v4l2_ctrl_hdr10_mastering_display master;
 assert(t7_hdmirx_get_hdr10(&dev,&cll,&master)==-ENODATA);
 rx.drm_received_ns=2000000000;rx.drm_packet[4]=2;
 /* Legitimate zero fields must remain zero, including mastering primaries. */
 assert(!t7_hdmirx_get_hdr10(&dev,&cll,&master));
 assert(!master.white_point_x && !cll.max_content_light_level);
 rx.drm_packet[22]=0xe8;rx.drm_packet[23]=3; /* HDMI maximum 1000 cd/m2 */
 rx.drm_packet[24]=50; /* HDMI minimum already 0.0001 cd/m2 */
 rx.drm_packet[26]=0xe8;rx.drm_packet[27]=3;
 assert(!t7_hdmirx_get_hdr10(&dev,&cll,&master));
 assert(master.max_display_mastering_luminance==10000000);
 assert(master.min_display_mastering_luminance==50);
 assert(cll.max_content_light_level==1000 && !cll.max_pic_average_light_level);
 rx.drm_packet[4]=3;
 assert(t7_hdmirx_get_hdr10(&dev,&cll,&master)==-ENODATA);
 rx.drm_packet[4]=2;rx.drm_received_ns=1000000001;
 assert(t7_hdmirx_get_hdr10(&dev,&cll,&master)==0);
 rx.drm_received_ns=1;
 assert(t7_hdmirx_get_hdr10(&dev,&cll,&master)==-ENODATA);
 rx.drm_received_ns=0;
 assert(t7_hdmirx_get_hdr10(&dev,&cll,&master)==-ENODATA);
}
'''
with tempfile.TemporaryDirectory() as temp:
    source, binary = Path(temp) / 'native.c', Path(temp) / 'native'
    source.write_text(code)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-fsanitize=undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('AMLY layout, shared byte budget and HDMI HDR10 metadata units/absence: PASS')
