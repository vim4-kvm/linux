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
functions = source[source.index('static u32 t7_hdmirx_meter_hz'):
                   source.index('static u32 t7_hdmirx_sample_cable')] + functions
ioctls = vdin[vdin.index('static int t7_vdin_query_dv_timings'):
             vdin.index('static int t7_vdin_set_input')]
prefix = r'''
#include <assert.h>
#define dev_info_ratelimited(...) ((void)0)
#define pr_info_ratelimited(...) ((void)0)
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
#define WRITE_ONCE(place, value) ((place) = (value))
static u64 ktime_get_ns(void) { return 1000000000ULL; }
#define EXPORT_SYMBOL_GPL(name)
#define FIELD_GET(mask,value) (((value)&(mask)) >> __builtin_ctz(mask))
#define U32_MAX UINT32_MAX
struct mutex { int unused; };
struct delayed_work { int unused; };
struct clk_bulk_data { void *clk; };
struct debugfs_blob_wrapper { void *data; unsigned long size; };
struct device { void *data; };
'''
stubs = r'''
static u8 core[0x8000], top[0x4000], phy[0x100];
static unsigned int period_reads, change_read, changed_period;
static unsigned int measurement_reads, corrupt_measurement;
static bool lose_signal;
static void word(unsigned int reg, unsigned int value) {
    core[reg] = value; core[reg+1] = value >> 8;
}
static void period(unsigned int value) {
    word(T7_RX_COR_FRAME_TICKS, value);
    core[T7_RX_COR_FRAME_TICKS+2] = value >> 16;
}
static u8 readb(void *address) {
    if (address == core + T7_RX_COR_WIDTH && ++measurement_reads == corrupt_measurement)
        period(changed_period);
    if (address == core + T7_RX_COR_FRAME_TICKS && ++period_reads == change_read) {
        period(changed_period);
        if (lose_signal) memset(top + T7_RX_TOP_TMDS_ALIGN_STAT, 0, 4);
    }
    return *(u8 *)address;
}
static u32 readl(void *address) { u32 value; memcpy(&value,address,4); return value; }
static void writeb(u8 value, void *address) { *(u8 *)address = value; }
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
static const struct v4l2_dv_timings modes[] = {
    V4L2_DV_BT_CEA_640X480P59_94,
    { .type=V4L2_DV_BT_656_1120, .bt={.width=720,.height=400,
        .polarities=V4L2_DV_VSYNC_POS_POL,.pixelclock=28322000,
        .hfrontporch=18,.hsync=108,.hbackporch=54,
        .vfrontporch=12,.vsync=2,.vbackporch=35} },
    V4L2_DV_BT_DMT_800X600P60, V4L2_DV_BT_DMT_1024X768P60,
    V4L2_DV_BT_CEA_1280X720P60, V4L2_DV_BT_CEA_1280X720P50,
    V4L2_DV_BT_DMT_1280X1024P60, V4L2_DV_BT_DMT_1366X768P60,
    V4L2_DV_BT_DMT_1440X900P60_RB, V4L2_DV_BT_DMT_1600X900P60_RB,
    V4L2_DV_BT_DMT_1680X1050P60_RB, V4L2_DV_BT_CEA_1920X1080P24,
    V4L2_DV_BT_CEA_1920X1080P25, V4L2_DV_BT_CEA_1920X1080P30,
    V4L2_DV_BT_CEA_1920X1080P50, V4L2_DV_BT_DMT_1920X1200P60_RB,
    { .type=V4L2_DV_BT_656_1120, .bt={.width=2560,.height=1440,
        .polarities=V4L2_DV_HSYNC_POS_POL,.pixelclock=241500000,
        .hfrontporch=48,.hsync=32,.hbackporch=80,
        .vfrontporch=3,.vsync=5,.vbackporch=33} },
    V4L2_DV_BT_CEA_3840X2160P30, V4L2_DV_BT_CEA_3840X2160P60,
};

static void measured_mode(const struct v4l2_dv_timings *t) {
    const struct v4l2_bt_timings *b=&t->bt;
    unsigned int ht=b->width+b->hfrontporch+b->hsync+b->hbackporch;
    unsigned int vt=b->height+b->vfrontporch+b->vsync+b->vbackporch;
    memset(core,0,sizeof(core));
    core[T7_RX_COR_FRAME_STATUS]=(b->polarities&V4L2_DV_HSYNC_POS_POL?BIT(0):0) |
        (b->polarities&V4L2_DV_VSYNC_POS_POL?BIT(1):0);
    word(T7_RX_COR_WIDTH,b->width); word(T7_RX_COR_HEIGHT,b->height);
    word(T7_RX_COR_HSYNC_LOW,(b->polarities&V4L2_DV_HSYNC_POS_POL)?ht-b->hsync:b->hsync);
    word(T7_RX_COR_HSYNC_HIGH,(b->polarities&V4L2_DV_HSYNC_POS_POL)?b->hsync:ht-b->hsync);
    word(T7_RX_COR_HFRONT,b->hfrontporch); word(T7_RX_COR_HBACK,b->hbackporch);
    word(T7_RX_COR_VSYNC_LOW,(b->polarities&V4L2_DV_VSYNC_POS_POL)?vt-b->vsync:b->vsync);
    word(T7_RX_COR_VSYNC_HIGH,(b->polarities&V4L2_DV_VSYNC_POS_POL)?b->vsync:vt-b->vsync);
    word(T7_RX_COR_VFRONT,b->vfrontporch); word(T7_RX_COR_VBACK,b->vbackporch);
    period((u64)166666664*ht*vt/b->pixelclock);
}

static void setup(bool uhd, unsigned int ticks) {
    memset(core,0,sizeof(core)); memset(top,0,sizeof(top));
    u32 value = BIT(21); memcpy(top + T7_RX_TOP_HPD_PWR5V,&value,4);
    value = 1; memcpy(top + T7_RX_TOP_TMDS_ALIGN_STAT,&value,4);
    memcpy(top + T7_RX_TOP_MISC_STAT0,&value,4);
    value = T7_RX_METER_VALID|4096; memcpy(top + T7_RX_TOP_METER_CABLE_STAT,&value,4);
    value = T7_RX_METER_VALID|(uhd?16384:4096); memcpy(top + T7_RX_TOP_METER_HDMI_STAT,&value,4);
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
        /* An APLL status dip must not revoke a valid vendor-qualified link. */
        u32 value = 0;
        memcpy(phy+T7_RX_PHY_APLL0,&value,4);
        assert(!t7_hdmirx_query_timings(&dev,&timing,&interval));
        memcpy(top+T7_RX_TOP_MISC_STAT0,&value,4);
        assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-ENOLINK);
        value=1;memcpy(top+T7_RX_TOP_MISC_STAT0,&value,4);
        value=4096;memcpy(top+T7_RX_TOP_METER_CABLE_STAT,&value,4);
        assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-ENOLINK);
        value=T7_RX_METER_VALID|4096;memcpy(top+T7_RX_TOP_METER_CABLE_STAT,&value,4);
        for (int sign=-1;sign<=1;sign+=2) {
            value=T7_RX_METER_VALID|((uhd?16384:4096)+sign*98);
            memcpy(top+T7_RX_TOP_METER_HDMI_STAT,&value,4);
            assert(!t7_hdmirx_query_timings(&dev,&timing,&interval));
            value=T7_RX_METER_VALID|((uhd?16384:4096)+sign*99);
            memcpy(top+T7_RX_TOP_METER_HDMI_STAT,&value,4);
            assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-ENOLINK);
        }
        setup(uhd,2777778);
        value=0;memcpy(top+T7_RX_TOP_TMDS_ALIGN_STAT,&value,4);
        assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-ENOLINK);
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
    setup(false,2777778); rx.dynamic_modes=true; rx.uhd60=false; link.ready=true;
    for (unsigned int i=0;i<ARRAY_SIZE(modes);i++) {
        measured_mode(&modes[i]);
        assert(!t7_hdmirx_query_timings(&dev,&timing,&interval));
        const struct v4l2_bt_timings *a=&modes[i].bt,*b=&timing.bt;
        assert(a->width==b->width && a->height==b->height);
        assert(a->hsync==b->hsync && a->vsync==b->vsync && a->polarities==b->polarities);
        assert(a->hfrontporch==b->hfrontporch && a->hbackporch==b->hbackporch);
        assert(a->vfrontporch==b->vfrontporch && a->vbackporch==b->vbackporch);
        assert(b->pixelclock>=a->pixelclock-1000 && b->pixelclock<=a->pixelclock+1000);
        t7_hdmirx_read_measurement(&rx,&rx.lease_mode);
        assert(!t7_hdmirx_read_color(&rx,&rx.lease_color)); link.users=1;
        assert(!t7_hdmirx_video_status(&rx));
        period(rx.lease_mode.ticks+1); assert(!t7_hdmirx_video_status(&rx));
        period((u64)rx.lease_mode.ticks*1001/1000);
        assert(t7_hdmirx_video_status(&rx)==-ENOLINK);
        link.users=0;
        printf("Measured mode %ux%u pixelclock=%llu passed\n",b->width,b->height,
               (unsigned long long)b->pixelclock);
    }
    /* A coherent but invalid FDET during a lease drops a frame. It must
     * never become a timing result, hide physical loss, or poison the next
     * valid frame. Valid clock changes remain a lease violation.
     */
    measured_mode(&modes[0]); link.users=1;
    t7_hdmirx_read_measurement(&rx,&rx.lease_mode);
    assert(!t7_hdmirx_read_color(&rx,&rx.lease_color));
    period(0xffffff);
    assert(t7_hdmirx_check_video_now(&dev)==-EAGAIN);
    timing=untouched;
    assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-EAGAIN);
    assert(!memcmp(&timing,&untouched,sizeof(timing)));
    measured_mode(&modes[0]); word(T7_RX_COR_HEIGHT,0); period(0xffffff);
    assert(t7_hdmirx_check_video_now(&dev)==-EAGAIN);
    link.ready=false;
    assert(t7_hdmirx_check_video_now(&dev)==-ENOLINK);
    link.ready=true; measured_mode(&modes[0]);
    assert(!t7_hdmirx_check_video_now(&dev));
    /* The ioctl takes a fresh measurement between its status checks.
     * Saturation at that middle read must not become a false source loss.
     */
    for (unsigned int read=3;read<=5;read+=2) {
        measured_mode(&modes[0]); measurement_reads=0; corrupt_measurement=read;
        changed_period=0xffffff; timing=untouched;
        interval=(struct v4l2_fract){123,456};
        assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-EAGAIN);
        assert(!memcmp(&timing,&untouched,sizeof(timing)));
        assert(interval.numerator==123 && interval.denominator==456);
    }
    corrupt_measurement=0; measured_mode(&modes[0]);
    assert(!t7_hdmirx_query_timings(&dev,&timing,&interval));
    measured_mode(&modes[1]);
    assert(t7_hdmirx_check_video_now(&dev)==-ENOLINK);
    link.users=0; measured_mode(&modes[0]); period(0xffffff);
    assert(t7_hdmirx_check_video_now(&dev)==-ERANGE);
    measured_mode(&modes[0]); word(T7_RX_COR_HBACK,1);
    assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-ERANGE);
    measured_mode(&modes[0]); core[T7_RX_COR_FRAME_STATUS]|=BIT(2);
    assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-ERANGE);
    measured_mode(&modes[0]); period_reads=0; change_read=2; changed_period=1234567;
    assert(t7_hdmirx_query_timings(&dev,&timing,&interval)==-EAGAIN);
    change_read=0;
    for (unsigned int space=0;space<4;space++)
        for (unsigned int depth=4;depth<=6;depth++)
            for (unsigned int full=0;full<2;full++)
                for (unsigned int c=1;c<=2;c++) {
        measured_mode(&modes[13]);
        u32 top_color=space<<20; memcpy(top+T7_RX_TOP_VID_STAT,&top_color,4);
        core[T7_RX_COR_COLOR_DEPTH]=depth;
        core[T7_RX_COR_AUDIO_PACKET]=1;
        u8 *avi=core+T7_RX_COR_AVI_PACKET;
        avi[0]=0x82;avi[1]=2;avi[2]=13;avi[4]=space<<5;avi[5]=c<<6;
        avi[6]=space?0:(full?2:1)<<2;avi[8]=space?full<<6:0;
        for (unsigned int j=0;j<17;j++) if(j!=3)avi[3]-=avi[j];
        if (space==3) {
            for (unsigned int r=T7_RX_COR_WIDTH;r<=T7_RX_COR_HBACK;r+=2)
                if (r!=T7_RX_COR_HEIGHT)word(r,t7_hdmirx_core_read16(&rx,r)/2);
        }
        struct t7_hdmirx_color color;
        assert(!t7_hdmirx_read_color(&rx,&color));
        assert(color.space==space && color.depth==(depth==4?8:depth==5?10:12) && color.full==full);
        assert(!t7_hdmirx_query_timings(&dev,&timing,&interval));
        assert(timing.bt.width==1920 && timing.bt.height==1080);
        assert(timing.bt.pixelclock>74249000 && timing.bt.pixelclock<74251000);
        rx.lease_color=color;rx.lease_yuv=space!=0;rx.lease_420=space==3;
        t7_hdmirx_read_measurement(&rx,&rx.lease_mode);link.users=1;
        assert(!t7_hdmirx_video_status(&rx));
        core[T7_RX_COR_COLOR_DEPTH]=depth==4?5:4;
        assert(t7_hdmirx_video_status(&rx)==-ENOLINK);link.users=0;
    }
    u8 *avi=core+T7_RX_COR_AVI_PACKET;
    avi[5]=3<<6;avi[3]=0;
    for (unsigned int j=0;j<17;j++)if(j!=3)avi[3]-=avi[j];
    struct t7_hdmirx_color color;
    assert(t7_hdmirx_read_color(&rx,&color)==-ERANGE); /* Unmapped HLG/2020. */
    avi[5]^=1; assert(t7_hdmirx_read_color(&rx,&color)==-ERANGE);
    /* PQ/2020 HDR2 follows normalized YUV, including 422's fixed transport
     * container with an unspecified GCP depth. Fresh metadata is mandatory.
     */
    rx.hdr_capture=true;rx.drm_received_ns=1000000000ULL;
    for (unsigned int space=1;space<4;space++) {
        memset(avi,0,17);avi[0]=0x82;avi[1]=2;avi[2]=13;
        avi[4]=space<<5;avi[5]=3<<6;avi[6]=6<<4;
        for (unsigned int j=0;j<17;j++)if(j!=3)avi[3]-=avi[j];
        u32 top_color=space<<20;memcpy(top+T7_RX_TOP_VID_STAT,&top_color,4);
        core[T7_RX_COR_COLOR_DEPTH]=space==1?0:6;
        assert(t7_hdmirx_hdr_input(&rx));
        assert(!t7_hdmirx_read_color(&rx,&color));
        rx.drm_received_ns=0;
        assert(!t7_hdmirx_hdr_input(&rx));
        assert(t7_hdmirx_read_color(&rx,&color)==-ERANGE);
        rx.drm_received_ns=1000000000ULL;
    }
    puts("RGB/444/422/420, 8/10/12-bit, range/601/709 and unsupported HDR guards passed");
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
