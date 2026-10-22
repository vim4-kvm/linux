#!/usr/bin/env python3
"""Exercise actual core lifecycle and stable-timing validation with fake registers."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-hdmirx.c').read_text()
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define T7_RX_'))
types = source[source.index('static const u32 t7_hdmirx_phy_top_regs'):
               source.index('static int t7_hdmirx_validate_edid')]
functions = source[source.index('static void t7_hdmirx_core_stop'):
                   source.index('int t7_hdmirx_query_timings')]
vdin = Path(__file__).with_name('t7-vdin-test.c').read_text()
status_api = vdin[vdin.index('static int t7_vdin_enum_input'):
                  vdin.index('static int t7_vdin_get_input')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
struct mutex { int unused; };
struct delayed_work { int unused; };
#define READ_ONCE(value) (value)
#define WRITE_ONCE(place, value) ((place) = (value))
typedef uint64_t u64;
#define div_u64(a,b) ((a)/(b))
static u64 now_ns = 1000000000ULL;
static u64 ktime_get_ns(void) { return now_ns; }
#ifdef __clang__
/* The extracted register tables size real struct fields; core-only paths do not read them. */
#pragma clang diagnostic ignored "-Wunneeded-internal-declaration"
#endif
typedef uint8_t u8;
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define EXPORT_SYMBOL_GPL(name)
struct clk_bulk_data { void *clk; };
struct debugfs_blob_wrapper { void *data; unsigned long size; };
struct arm_smccc_res { unsigned long a0; };
struct device { void *data; };
'''
mmio = r'''
static u8 core[0x8000], top[0x4000];
static bool allowed, changed, smc_error, readback_error;
static unsigned int writes, pcs_resets, stability_waits;
static u8 readb(void *address)
{
    if (readback_error && address == core + T7_RX_COR_SYS_CTRL) return 0;
    return *(u8 *)address;
}
static void writeb(u8 value, void *address)
{
    assert(allowed);
    assert((u8 *)address >= core && (u8 *)address < core + sizeof(core));
    *(u8 *)address = value; writes++;
}
static u32 readl(void *address) { u32 value; memcpy(&value, address, 4); return value; }
static void writel(u32 value, void *address)
{
    assert((u8 *)address >= top && (u8 *)address + 4 <= top + sizeof(top));
    if (address == top + T7_RX_TOP_SW_RESET && value & BIT(13)) {
        assert(core[T7_RX_COR_SYS_CTRL] == 3 && core[T7_RX_COR_PHY_CLOCK1] == 255);
        pcs_resets++;
    }
    memcpy(address, &value, 4);
}
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static unsigned long clk_get_rate(void *clk) { (void)clk; return 166666664; }
static void arm_smccc_smc(unsigned long cmd, unsigned long reg, unsigned long value,
                          int a, int b, int c, int d, int e, struct arm_smccc_res *res)
{
    assert(cmd == 0x8200008c && reg == 0xa2 && value == 0x1f);
    allowed = !smc_error; res->a0 = smc_error;
}
static void t7_hdmirx_set_hpd(struct t7_hdmirx *rx, bool high)
{
    assert(!high || rx->uhd60);
}
static void usleep_range(unsigned int low, unsigned int high)
{
    assert(high >= low);
    if (low == 20000) stability_waits++;
    if (low == 20000 && changed) core[0x188e] = 0;
}
static void word(unsigned int address, unsigned int value)
{
    core[address] = value; core[address + 1] = value >> 8;
}
static void timing(void)
{
    u32 value = BIT(21);
    memcpy(top + T7_RX_TOP_HPD_PWR5V, &value, 4);
    value = 1; memcpy(top + T7_RX_TOP_TMDS_ALIGN_STAT, &value, 4);
    word(0x188c, 1920); word(0x188e, 1080);
    word(0x1890, 2156); word(0x1892, 44);
    word(0x1898, 1120); word(0x189a, 5);
    word(0x1888, 2777780 & 0xffff); core[0x188a] = 2777780 >> 16;
    core[0x1881] = 3;
}
'''
status_stubs = r'''
#define V4L2_INPUT_TYPE_CAMERA 2
#define V4L2_IN_ST_NO_SYNC 0x10000
struct v4l2_input { u32 index; unsigned char name[32]; u32 type, status, capabilities; };
struct file { void *data; };
struct t7_vdin { struct device *receiver; };
static void *video_drvdata(struct file *file) { return file->data; }
#define strscpy(dst, src, size) snprintf((char *)(dst), size, "%s", src)
'''
checks = r'''
int main(void)
{
    struct t7_hdmirx rx = { .top = top, .cor = core, .port = 1 };
    struct device dev = { .data = &rx };
    unsigned int before;
    assert(t7_hdmirx_check_video(&dev) == -ENOLINK);
    smc_error = true;
    assert(t7_hdmirx_core_start(&rx) == -EIO && !writes && !rx.core_active);
    smc_error = false; readback_error = true;
    assert(t7_hdmirx_core_start(&rx) == -EIO);
    t7_hdmirx_core_stop(&rx); readback_error = false;
    assert(!t7_hdmirx_core_start(&rx)); timing();
    assert(!pcs_resets);
    core[T7_RX_COR_POWER_RESET] = BIT(6);
    t7_hdmirx_reset_pcs(&rx);
    assert(pcs_resets == 1 && !readl(top + T7_RX_TOP_SW_RESET));
    assert(core[T7_RX_COR_POWER_RESET] == BIT(6));
    assert(rx.core_active && !t7_hdmirx_check_video(&dev));
    core[T7_RX_COR_COLOR_DEPTH] = 5;
    assert(t7_hdmirx_check_video(&dev) == -ERANGE);
    core[T7_RX_COR_COLOR_DEPTH] = 4;
    writel(BIT(20), top + T7_RX_TOP_VID_STAT);
    assert(t7_hdmirx_check_video(&dev) == -ERANGE);
    writel(0, top + T7_RX_TOP_VID_STAT);
    core[0x1881] |= BIT(2);
    assert(t7_hdmirx_check_video(&dev) == -ERANGE); timing();
    word(0x188c, 1280);
    assert(t7_hdmirx_check_video(&dev) == -ERANGE); timing();
    word(0x1888, 0); core[0x188a] = 0;
    assert(t7_hdmirx_check_video(&dev) == -ERANGE); timing();
    changed = true;
    assert(t7_hdmirx_check_video(&dev) == -ERANGE); timing(); changed = false;
    memset(top + T7_RX_TOP_TMDS_ALIGN_STAT, 0, 4);
    assert(t7_hdmirx_check_video(&dev) == -ENOLINK); timing();
    t7_hdmirx_core_stop(&rx); before = writes;
    assert(!rx.core_active && !core[0x20a3] && !core[0x20a4] && !core[0x20a5]);
    t7_hdmirx_core_stop(&rx); assert(writes == before);
    core[T7_RX_COR_SYS_CTRL] = 3;
    core[T7_RX_COR_PHY_CLOCK1] = 255;
    core[T7_RX_COR_PHY_CLOCK2] = 63;
    core[T7_RX_COR_PHY_CLOCK3] = 1;
    assert(!t7_hdmirx_core_start(&rx));
    t7_hdmirx_core_stop(&rx);
    assert(core[T7_RX_COR_SYS_CTRL] == 3 && core[T7_RX_COR_PHY_CLOCK1] == 255);
    assert(core[T7_RX_COR_PHY_CLOCK2] == 63 && core[T7_RX_COR_PHY_CLOCK3] == 1);
    rx.core_active = true; timing(); rx.uhd60 = true;
    assert(t7_hdmirx_check_video(&dev) == -ERANGE);
    word(0x188c, 3840); word(0x188e, 2160);
    word(0x1890, 4312); word(0x1892, 88);
    word(0x1898, 2240); word(0x189a, 10);
    assert(!t7_hdmirx_check_video(&dev));
    word(0x188e, 1080);
    assert(t7_hdmirx_check_video(&dev) == -ERANGE);
    struct t7_vdin capture = { .receiver = &dev };
    struct file file = { .data = &capture };
    struct v4l2_input input = { 0 };
    before = stability_waits;
    /* Repeated live status polls must not wait for another frame while holding
     * the V4L2 queue lock. Loss and recovery must still be reported immediately.
     */
    assert(!t7_vdin_enum_input(&file, NULL, &input) && input.status);
    word(0x188e, 2160);
    assert(!t7_vdin_enum_input(&file, NULL, &input));
    assert(stability_waits == before);
    assert(!input.status);
    writel(0, top + T7_RX_TOP_TMDS_ALIGN_STAT);
    assert(!t7_vdin_enum_input(&file, NULL, &input) && input.status);
    writel(1, top + T7_RX_TOP_TMDS_ALIGN_STAT);
    assert(!t7_vdin_enum_input(&file, NULL, &input) && !input.status);
    for (int i = 0; i < 100; i++)
        assert(!t7_vdin_enum_input(&file, NULL, &input) && !input.status);
    assert(stability_waits == before);
    input.index = 1;
    assert(t7_vdin_enum_input(&file, NULL, &input) == -EINVAL);
    /* Start-time qualification continues to reject a signal changing mid-wait. */
    changed = true;
    assert(t7_hdmirx_check_video(&dev) == -ERANGE);
    assert(stability_waits == before + 1);
    changed=false;t7_hdmirx_core_stop(&rx);
    core[T7_RX_COR_UNREC_CTRL]=0x34;core[T7_RX_COR_UNREC_TYPE]=0x55;
    rx.hdr_probe=true;
    assert(!t7_hdmirx_core_start(&rx));
    assert(core[T7_RX_COR_UNREC_CTRL]==1 && core[T7_RX_COR_UNREC_TYPE]==0x87);
    core[T7_RX_COR_COLOR_DEPTH]=5;
    assert(t7_hdmirx_check_video(&dev)==-ERANGE);
    core[T7_RX_COR_COLOR_DEPTH]=4;
    writel(BIT(20),top+T7_RX_TOP_VID_STAT);
    assert(t7_hdmirx_check_video(&dev)==-ERANGE);
    rx.hdr_capture=true;
    unsigned char avi[17]={0x82,2,13,0x54,0x72,0xe8,0x60,0x61};
    unsigned char drm[30]={0x87,1,26,0x5c,2};
    memcpy(core+0x1140,avi,sizeof(avi)); memcpy(core+0x11c0,drm,sizeof(drm));
    word(T7_RX_COR_WIDTH,1920);word(T7_RX_COR_HEIGHT,2160);
    word(T7_RX_COR_HSYNC_LOW,2156);word(T7_RX_COR_HSYNC_HIGH,44);
    word(T7_RX_COR_VSYNC_LOW,2240);word(T7_RX_COR_VSYNC_HIGH,10);
    core[T7_RX_COR_COLOR_DEPTH]=5;
    assert(!t7_hdmirx_hdr_input(&rx)); /* cached DRM is insufficient */
    core[0x1134]=BIT(4);t7_hdmirx_update_drm(&rx);
    assert(t7_hdmirx_hdr_input(&rx) && !t7_hdmirx_video_status(&rx));
    now_ns+=1000000001ULL;
    assert(!t7_hdmirx_hdr_input(&rx)); /* missing refresh expires */
    unsigned char sdravi[17]={0x82,2,13,0xf4,0x72,0xa8,0,0x61};
    memcpy(core+0x1140,sdravi,sizeof(sdravi));
    assert(t7_hdmirx_sdr420_input(&rx) && !t7_hdmirx_video_status(&rx));
    struct t7_hdmirx_link lease={.users=1};
    u32 pll=T7_RX_PHY_PLL_LOCK;rx.phy=core;memcpy(core+T7_RX_PHY_APLL0,&pll,4);
    rx.link=&lease;rx.lease_yuv=true;rx.lease_hdr=true;rx.lease_420=true;
    assert(t7_hdmirx_video_status(&rx)==-ENOLINK); /* HDR -> SDR, same geometry */
    rx.lease_hdr=false;assert(!t7_hdmirx_video_status(&rx));
    rx.lease_yuv=false;assert(t7_hdmirx_video_status(&rx)==-ENOLINK);
    unsigned char sdr444[17]={0x82,2,13,0x14,0x52,0xa8,0,0x61};
    memcpy(core+0x1140,sdr444,sizeof(sdr444));core[T7_RX_COR_COLOR_DEPTH]=0;
    word(T7_RX_COR_WIDTH,3840);word(T7_RX_COR_HSYNC_LOW,4312);word(T7_RX_COR_HSYNC_HIGH,88);
    assert(t7_hdmirx_sdr444_input(&rx));
    rx.lease_yuv=true;assert(t7_hdmirx_video_status(&rx)==-ENOLINK); /* 420 -> 444 */
    rx.lease_420=false;assert(!t7_hdmirx_video_status(&rx));
    core[T7_RX_COR_COLOR_DEPTH]=5;assert(!t7_hdmirx_sdr444_input(&rx));
    core[T7_RX_COR_COLOR_DEPTH]=4;assert(t7_hdmirx_sdr444_input(&rx));
    core[0x1140+8]=0x40;core[0x1140+3]-=0x40;
    assert(!t7_hdmirx_sdr444_input(&rx)); /* full-range YCbCr needs a different matrix */
    memcpy(core+0x1140,sdr444,sizeof(sdr444));core[0x1140+3]^=1;
    assert(!t7_hdmirx_sdr444_input(&rx)); /* corrupt AVI */
    word(T7_RX_COR_WIDTH,1920);word(T7_RX_COR_HSYNC_LOW,2156);word(T7_RX_COR_HSYNC_HIGH,44);
    core[T7_RX_COR_COLOR_DEPTH]=5;
    rx.link=NULL;memcpy(core+0x1140,avi,sizeof(avi));
    core[0x1134]=BIT(4);t7_hdmirx_update_drm(&rx);
    core[0x1140+3]^=1;assert(!t7_hdmirx_hdr_input(&rx));
    core[0x1140+3]^=1;assert(t7_hdmirx_hdr_input(&rx));
    core[0x11c0+4]=3;core[0x11c0+3]--; /* valid HLG is not PQ */
    core[0x1134]=BIT(4);t7_hdmirx_update_drm(&rx);
    assert(!t7_hdmirx_hdr_input(&rx));
    memcpy(core+0x11c0,drm,sizeof(drm));core[0x1134]=BIT(4);
    t7_hdmirx_update_drm(&rx);assert(t7_hdmirx_hdr_input(&rx));
    writel(0,top+T7_RX_TOP_TMDS_ALIGN_STAT);t7_hdmirx_update_drm(&rx);
    assert(!rx.drm_received_ns);
    t7_hdmirx_core_stop(&rx);
    assert(core[T7_RX_COR_UNREC_CTRL]==0x34 && core[T7_RX_COR_UNREC_TYPE]==0x55);
    puts("Core lifecycle, stable start qualification and nonblocking status loss/recovery passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'core.c'
    executable = Path(directory) / 'core'
    code.write_text(prefix + defines + '\n' + types + mmio + functions + status_stubs + status_api + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
