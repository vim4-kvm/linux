#!/usr/bin/env python3
"""Run the actual RX timing query and VDIN ioctls against register transitions.

Requires Linux UAPI headers, but no board, kernel modules or privileges.
"""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-hdmirx.c').read_text()
vdin = Path(__file__).with_name('t7-vdin-test.c').read_text()
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define T7_RX_'))
types = source[source.index('static const u32 t7_hdmirx_phy_top_regs'):
               source.index('static int t7_hdmirx_validate_edid')]
functions = source[source.index('static u32 t7_hdmirx_core_read16'):
                   source.index('static u32 t7_hdmirx_audio_word')]
ioctls = vdin[vdin.index('static int t7_vdin_query_dv_timings'):
             vdin.index('static int t7_vdin_set_input')]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/videodev2.h>
#include <linux/v4l2-dv-timings.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define READ_ONCE(value) (value)
#define EXPORT_SYMBOL_GPL(name)
struct mutex { int unused; };
struct delayed_work { int unused; };
struct clk_bulk_data { void *clk; };
struct debugfs_blob_wrapper { void *data; unsigned long size; };
struct device { void *data; };
'''
stubs = r'''
static u8 core[0x8000], top[0x4000], phy[0x100];
static unsigned int period_reads, change_read, changed_period;
static bool lose_signal;
static void word(unsigned int reg, unsigned int value) {
    core[reg] = value; core[reg+1] = value >> 8;
}
static void period(unsigned int value) {
    word(T7_RX_COR_FRAME_TICKS, value);
    core[T7_RX_COR_FRAME_TICKS+2] = value >> 16;
}
static u8 readb(void *address) {
    if (address == core + T7_RX_COR_FRAME_TICKS && ++period_reads == change_read) {
        period(changed_period);
        if (lose_signal) memset(top + T7_RX_TOP_TMDS_ALIGN_STAT, 0, 4);
    }
    return *(u8 *)address;
}
static u32 readl(void *address) { u32 value; memcpy(&value,address,4); return value; }
static unsigned long clk_get_rate(void *clk) { (void)clk; return 166666664; }
static u64 div_u64(u64 a, u32 b) { assert(b); return a/b; }
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static void usleep_range(unsigned int low, unsigned int high) {
    (void)low; (void)high; assert(!"timing ioctls must not sleep for a frame");
}
struct t7_vdin { struct device *receiver; };
struct file { void *data; };
static void *video_drvdata(struct file *file) { return file->data; }
'''
checks = r'''
static void setup(bool uhd, unsigned int ticks) {
    memset(core,0,sizeof(core)); memset(top,0,sizeof(top));
    u32 value = BIT(21); memcpy(top + T7_RX_TOP_HPD_PWR5V,&value,4);
    value = 1; memcpy(top + T7_RX_TOP_TMDS_ALIGN_STAT,&value,4);
    value = T7_RX_PHY_PLL_LOCK; memcpy(phy + T7_RX_PHY_APLL0,&value,4);
    word(T7_RX_COR_WIDTH, uhd ? 3840 : 1920);
    word(T7_RX_COR_HEIGHT, uhd ? 2160 : 1080);
    /* Byte layout observed on hardware: positive pulse is the HIGH count. */
    word(T7_RX_COR_HSYNC_LOW, uhd ? 4312 : 2156);
    word(T7_RX_COR_HSYNC_HIGH, uhd ? 88 : 44);
    word(T7_RX_COR_VSYNC_LOW, uhd ? 2240 : 1120);
    word(T7_RX_COR_VSYNC_HIGH, uhd ? 10 : 5);
    period(ticks); period_reads = change_read = 0; lose_signal = false;
}
int main(void) {
    struct t7_hdmirx_link link = {.ready = true};
    struct t7_hdmirx rx = {.link=&link,.core_active=true,.port=1,
                           .top=top,.phy=phy,.cor=core};
    struct device dev = {.data=&rx};
    struct t7_vdin capture = {.receiver=&dev};
    struct file file = {.data=&capture};
    struct v4l2_dv_timings timing;
    struct v4l2_fract interval;
    struct v4l2_streamparm parm = {.type=V4L2_BUF_TYPE_VIDEO_CAPTURE};
    for (int uhd=0; uhd<2; uhd++) {
        rx.uhd60 = uhd;
        for (int fractional=0; fractional<2; fractional++) {
            unsigned int ticks = fractional ? 2780556 : 2777778;
            setup(uhd,ticks);
            assert(!t7_vdin_query_dv_timings(&file,NULL,&timing));
            assert(!t7_vdin_get_parm(&file,NULL,&parm));
            assert(timing.bt.width==(uhd?3840:1920) && timing.bt.height==(uhd?2160:1080));
            unsigned int ht=timing.bt.width+timing.bt.hfrontporch+timing.bt.hsync+timing.bt.hbackporch;
            unsigned int vt=timing.bt.height+timing.bt.vfrontporch+timing.bt.vsync+timing.bt.vbackporch;
            double fps=(double)timing.bt.pixelclock/ht/vt;
            assert(fractional ? (fps>59.939 && fps<59.941) : (fps>59.999 && fps<60.001));
            assert(parm.parm.capture.timeperframe.numerator==ticks);
            assert(parm.parm.capture.timeperframe.denominator==166666664);
            assert(!parm.parm.capture.capability);
        }
    }
    setup(true,2777778);
    struct v4l2_dv_timings untouched; memset(&untouched,0xa5,sizeof(untouched));
    timing=untouched;
    /* A changed middle/low byte with an unchanged high byte is still torn. */
    change_read=3; changed_period=2780556;
    assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-EAGAIN);
    assert(!memcmp(&timing,&untouched,sizeof(timing)));
    setup(true,2777778); change_read=2; changed_period=2777778; lose_signal=true;
    assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-ENOLINK);
    assert(!memcmp(&timing,&untouched,sizeof(timing)));
    setup(true,5555556); /* 30 fps is not a qualified capture profile. */
    assert(t7_vdin_get_parm(&file,NULL,&parm)==-ERANGE);
    setup(true,0); assert(t7_vdin_query_dv_timings(&file,NULL,&timing)==-ERANGE);
    setup(true,2777778); core[T7_RX_COR_FRAME_STATUS]=BIT(2);
    assert(t7_vdin_query_dv_timings(&file,NULL,&timing)==-ERANGE);
    setup(true,2777778); core[T7_RX_COR_COLOR_DEPTH]=5;
    assert(t7_vdin_query_dv_timings(&file,NULL,&timing)==-ERANGE);
    setup(true,2777778); word(T7_RX_COR_HSYNC_HIGH,80); word(T7_RX_COR_HSYNC_LOW,4320);
    assert(t7_vdin_query_dv_timings(&file,NULL,&timing)==-ERANGE);
    setup(true,2777778); word(T7_RX_COR_HSYNC_HIGH,4312); word(T7_RX_COR_HSYNC_LOW,88);
    assert(t7_vdin_query_dv_timings(&file,NULL,&timing)==-ERANGE);
    setup(true,2777778); link.ready=false;
    assert(t7_vdin_get_parm(&file,NULL,&parm)==-ENOLINK);
    parm.type=V4L2_BUF_TYPE_VIDEO_OUTPUT;
    assert(t7_vdin_get_parm(&file,NULL,&parm)==-EINVAL);
    puts("Measured timing ioctls: UHD/FHD, fractional rate, torn counter, loss, unsupported modes passed");
}
'''
with tempfile.TemporaryDirectory(prefix='t7-timings-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(prefix + defines + '\n' + types + stubs + functions + ioctls + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function',
                    '-fsanitize=undefined,address', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
