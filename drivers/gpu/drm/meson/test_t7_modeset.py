#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare executable PLL/VENC code with a local vendor source checkout."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'^(?:static )?(?:inline )?[\w \t*\n]+\b' + name + r'\([^;]*?\)\s*\{',
                      source, re.M)
    assert match, name
    depth = 1
    offset = match.end()
    while depth:
        depth += (source[offset] == '{') - (source[offset] == '}')
        offset += 1
    return source[match.start():offset]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', type=Path, required=True, help='common_drivers checkout')
parser.add_argument('--output', type=Path, help='Write the generated vendor comparison table')
parser.add_argument('--dtb', type=Path, help='Check compiled native display resource ownership')
args = parser.parse_args()
here = Path(__file__).resolve().parent
vendor = args.vendor / 'drivers/media/vout/hdmitx21/hw'
source = (here / 'meson_t7_modeset.c').read_text()
registers = (here / 'meson_registers.h').read_text()
defines = '\n'.join(re.findall(r'^#define\s+(?:ENCP_\w+|VENC_INTCTRL)\s+0x[0-9a-f]+.*$',
                               registers, re.M))
types = (here / 'meson_t7_modeset.h').read_text()
clock_type = types[types.index('struct meson_t7_clock {'):types.index('\n};') + 3]
timing_source = (args.vendor / 'drivers/media/vout/hdmitx_common/hdmitx_mode.c').read_text()
modes = []
for label, vic, name in [('720p60', 4, '1280x720p60hz'),
                         ('1080p50', 31, '1920x1080p50hz'),
                         ('1080p60', 16, '1920x1080p60hz'),
                         ('1440p60', 1024, '2560x1440p60hz'),
                         ('4k30', 95, '3840x2160p30hz'),
                         ('4k60', 97, '3840x2160p60hz')]:
    row = re.search(r'\{\w+, "' + name + r'", (?:NULL|"[^"]*"),([^}]+)}', timing_source)[1]
    numbers = [int(value.strip()) for value in row.split(',') if value.strip()]
    _, _, _, pixel, ht, _, hf, hsw, _, h, vt, _, vf, vsw, _, v = numbers[:16]
    modes.append((label, vic, pixel, h, h + hf, h + hf + hsw, ht,
                  v, v + vf, v + vf + vsw, vt))
modes += [('1080p59.94', 16, 148352, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125),
          ('custom-154MHz', 1024, 154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235)]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint8_t u8;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U << (l)) & (~0U >> (31-(h))))
#define min(a,b) ((a)<(b)?(a):(b))
#define __ffs(x) __builtin_ctz(x)
#define HDMITX_INFO(...) do {} while (0)
#define HDMITX_ERROR(...) do {} while (0)
#define MIN_HTXPLL_VCO 3000000
#define MAX_HTXPLL_VCO 6000000
#define HDMITX_VESA_OFFSET 1024
#define HDMI_COLORSPACE_YUV420 3
#define WAIT_FOR_PLL_LOCKED(reg) do {} while (0)
struct drm_display_mode {
    int clock,hdisplay,hsync_start,hsync_end,htotal;
    int vdisplay,vsync_start,vsync_end,vtotal,flags;
};
struct hdmi_timing {
    int vic,h_total,h_blank,h_front,h_sync,h_back,h_active;
    int v_total,v_front,v_sync,v_back,v_active;
};
struct hdmi_format_para { struct hdmi_timing timing; int cs; };
struct chip_data {int chip_type;};
struct hdmitx_dev { struct {struct hdmi_format_para fmt_para;} tx_comm; int frl_rate,dsc_en;
    struct {struct chip_data *chip_data;} tx_hw;};
static struct hdmitx_dev device;
static struct hdmitx_dev *get_hdmitx21_device(void) {return &device;}
static const struct hdmi_timing *hdmitx_mode_vic_to_hdmi_timing(int vic)
{ return &device.tx_comm.fmt_para.timing; }
enum hdmi_vic {DUMMY_VIC};
static u32 vendor_regs[0x6000], actual_regs[0x6000], pll[7], video[8], divider, shared[256];
static bool fail_lock;
struct meson_t7_modeset {void *pll,*vid2,*vid_div,*clkctrl;};
static void meson_t7_write(struct meson_t7_modeset *hw,u32 reg,u32 value)
{assert(reg!=0x27a2 && reg!=0x27a3);actual_regs[reg]=value;}
static void hd21_write_reg(u32 reg,u32 value) {vendor_regs[reg]=value;}
static u32 hd21_read_reg(u32 reg) {return vendor_regs[reg] | (fail_lock ? 0 : 0xc0000000);}
static void hd21_set_reg_bits(u32 reg,u32 value,u32 start,u32 width)
{u32 mask=((1U<<width)-1)<<start;vendor_regs[reg]=(vendor_regs[reg]&~mask)|(value<<start);}
static void writel(u32 value,void *reg) {*(u32 *)reg=value;}
static u32 readl(void *reg) {return *(u32 *)reg | ((uintptr_t)reg>=(uintptr_t)pll &&
    (uintptr_t)reg<(uintptr_t)(pll+7) && !fail_lock ? 0xc0000000 : 0);}
static void usleep_range(int a,int b) {}
static void udelay(int a) {}
static void regmap_update_bits(void *map,u32 reg,u32 mask,u32 value)
{assert(reg==0xd8 || reg==0xdc);shared[reg/4]=(shared[reg/4]&~mask)|(value&mask);}
struct arm_smccc_res {u32 a0;};
static void arm_smccc_smc(u32 fn,u32 op,u32 wr,u32 enc,u32 a,u32 b,u32 c,u32 d,
                         struct arm_smccc_res *res)
{assert(fn==0x820000ab && op==9 && wr==1 && enc==1);}
#define DRM_MODE_FLAG_PHSYNC 1
#define DRM_MODE_FLAG_NHSYNC 2
#define DRM_MODE_FLAG_PVSYNC 4
#define DRM_MODE_FLAG_NVSYNC 8
#define DRM_MODE_FLAG_INTERLACE 16
#define DRM_MODE_FLAG_DBLSCAN 32
enum drm_mode_status {MODE_OK,MODE_NO_INTERLACE,MODE_NO_DBLESCAN,MODE_BAD,
    MODE_BAD_HVALUE,MODE_BAD_VVALUE,MODE_H_ILLEGAL,MODE_V_ILLEGAL,MODE_NOCLOCK,MODE_VSYNC};
'''
hw_t7 = (vendor / 'hw_t7.c').read_text()
venc_source = (vendor / 'enc_cfg_hw.c').read_text()
clk_source = (vendor / 'hw_clk.c').read_text()
pll_defines = '\n'.join(f'#define ANACTRL_HDMIPLL_CTRL{i} {i}' for i in range(7))
vics = set(re.findall(r'HDMI_\d+_\w+', function(hw_t7, 'set_hpll_hclk_v1')))
vic_defines = '\n'.join(f'#define {vic} {vic.split("_")[1]}' for vic in sorted(vics))
driver_names = ('meson_t7_clock_calculate', 'meson_t7_mode_valid', 'meson_t7_pll_lock',
                'meson_t7_pll_recipe', 'meson_t7_pll_set', 'meson_t7_video_clock',
                'meson_t7_venc_set', 'meson_t7_modeset_disable', 'meson_t7_modeset_enable')
vendor_names = ('set_hpll_hclk_v1', 'set_hpll_hclk_v2', 'set_hpll_hclk_v3',
                't7_auto_set_hpll')
other_registers = '\n'.join(re.findall(r'^#define\s+(?:VPU_HDMI_SETTING|VIU_OSD_BLEND_\w+|'
                                     r'VPP_OSD1_\w+|VPP_POSTBLEND_H_SIZE)\s+0x[0-9a-f]+.*$',
                                     registers, re.M))
body = preamble + defines + '\n' + other_registers + '\n' + pll_defines + '\n' + vic_defines
body += '\n' + clock_type
body += '\n' + '\n'.join(function(source, name) for name in driver_names)
body += '\n' + '\n'.join(function(hw_t7, name) for name in vendor_names)
body += '\n' + function(clk_source, 't7_calc_pixel_clk_hpll_vco_od')
body += '\n' + function(venc_source, 'config_tv_enc_calc')
video_source = (vendor / 'hdmi_tx_hw.c').read_text()
video_registers = (vendor / 'register.h').read_text()
for name, word in re.findall(r'^#define (CLKCTRL_\w+)\s+CLKCTRL_REG_ADDR\((0x[0-9a-f]+)\)',
                             video_registers, re.M):
    body += f'\n#define {name} {int(word, 16) * 4}\n'
body += '\n#define MESON_CPU_ID_S5 8\n' + function(video_source, 'enable_crt_video_hdmi')
body += '\nint main(void) { struct meson_t7_modeset hw={.pll=pll,.vid2=video,.vid_div=&divider};\n'
body += '''struct chip_data chip={.chip_type=7};device.tx_hw.chip_data=&chip;
enable_crt_video_hdmi(1,0,2);meson_t7_video_clock(&hw);
assert(shared[0xdc/4]==vendor_regs[CLKCTRL_ENC_HDMI_CLK_CTRL]);
assert((video[1] & (BIT(10)|BIT(9)|BIT(5)))==vendor_regs[CLKCTRL_VID_CLK2_CTRL2]);
puts("ENC2 pixel/FE/PNX mux matches executable vendor enable_crt_video_hdmi(1,0,2)");
'''
for name, vic, pixel, h, hs, he, ht, v, vs, ve, vt in modes:
    body += f'''
    {{struct drm_display_mode mode={{{pixel},{h},{hs},{he},{ht},{v},{vs},{ve},{vt},0}};
    struct meson_t7_clock clock; u32 vco,od1,od2;
    memset(vendor_regs,0,sizeof(vendor_regs)); memset(actual_regs,0,sizeof(actual_regs));
    memset(pll,0,sizeof(pll));
    device.tx_comm.fmt_para.timing=(struct hdmi_timing){{{vic},{ht},{ht-h},{hs-h},{he-hs},
        {ht-he},{h},{vt},{vs-v},{ve-vs},{vt-ve},{v}}};
    assert(!meson_t7_clock_calculate({pixel},&clock));
    assert(meson_t7_mode_valid(&mode)==MODE_OK);
    t7_calc_pixel_clk_hpll_vco_od({pixel},&vco,&od1,&od2);
    assert(clock.od1==od1 && clock.od2==od2);
    if (!clock.fractional) assert(clock.vco==vco);
    config_tv_enc_calc(&device,{vic}); meson_t7_venc_set(&hw,&mode);
    assert(!memcmp(vendor_regs,actual_regs,sizeof(vendor_regs)));
    memset(vendor_regs,0,sizeof(vendor_regs));
    if(clock.vco==5940000) set_hpll_hclk_v1(0xf7,clock.fractional?0x8168:0x10000);
    else t7_auto_set_hpll(clock.vco);
    assert(!meson_t7_pll_set(&hw,&clock,{pixel}>340000));
    for(int i=0;i<7;i++) {{u32 mask=i?~0U:~(GENMASK(31,30)|GENMASK(21,16));
        assert((pll[i]&mask)==(vendor_regs[i]&mask));}}
    printf("{name} %u %u %u %u %u %u\\n",clock.vco,clock.od1,clock.od2,clock.frac,
           actual_regs[ENCP_VIDEO_MAX_PXCNT],actual_regs[ENCP_VIDEO_MAX_LNCNT]);
    assert(!meson_t7_modeset_enable(&hw,&mode));
    assert(actual_regs[ENCP_VIDEO_EN]==1);
    assert(actual_regs[VPP_OSD1_IN_SIZE]==({v}U<<16|{h}));
    meson_t7_modeset_disable(&hw); assert(actual_regs[ENCP_VIDEO_EN]==0);
    fail_lock=true; assert(meson_t7_pll_set(&hw,&clock,false)==-ETIMEDOUT); fail_lock=false;
    mode.flags=DRM_MODE_FLAG_INTERLACE; assert(meson_t7_mode_valid(&mode)==MODE_NO_INTERLACE);
    mode.flags=DRM_MODE_FLAG_DBLSCAN; assert(meson_t7_mode_valid(&mode)==MODE_NO_DBLESCAN);
    mode.flags=0; mode.clock=594001; assert(meson_t7_mode_valid(&mode)==MODE_NOCLOCK);
    mode.clock={pixel}; mode.vsync_start=mode.vdisplay+1;
    assert(meson_t7_mode_valid(&mode)==MODE_V_ILLEGAL);
    }}
'''
body += '''{struct meson_t7_clock clock;
assert(meson_t7_clock_calculate(0,&clock)==-ERANGE);
assert(meson_t7_clock_calculate(594001,&clock)==-ERANGE);}
{struct drm_display_mode mode={25175,640,656,752,800,480,490,492,2000,0};
assert(meson_t7_mode_valid(&mode)==MODE_VSYNC);
mode.vtotal=1311;assert(meson_t7_mode_valid(&mode)==MODE_OK);}
return 0;}
'''
with tempfile.TemporaryDirectory(prefix='t7-modeset-') as directory:
    c_file = Path(directory) / 'test.c'
    c_file.write_text(body)
    executable = Path(directory) / 'test'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                    '-Wno-unused-variable', '-o', str(executable), str(c_file)], check=True)
    result = subprocess.check_output([str(executable)], text=True)
print(result, end='')
if args.output:
    args.output.write_text(json.dumps(dict(vendor_commit=subprocess.check_output(
        ['git', '-C', str(args.vendor), 'rev-parse', 'HEAD'], text=True).strip(),
        parameters=[dict(zip(('mode', 'vco_khz', 'od1', 'od2', 'frac', 'max_pixel', 'max_line'),
                             line.split())) for line in result.splitlines()
                    if line.split()[0] in {mode[0] for mode in modes}]), indent=2) + '\n')
print('Executable vendor PLL/VENC comparison and PLL timeout tests passed')

# Execute the bridge's PHY/config/atomic-enable functions, including SCDC failure.
bridge_source = (here / 'meson_hdmitx21.c').read_text()
bridge_defines = '\n'.join(line for line in bridge_source.splitlines()
                           if line.startswith('#define '))
bridge_program = preamble + '\n' + clock_type + '\n' + bridge_defines + r'''
#define IS_ENABLED(x) 1
#define WARN_ON(x) (x)
#define DRM_MODE_FMT "mode"
#define DRM_MODE_ARG(mode) 0
static void debug_log(void *dev,const char *format,...) {}
#define drm_dbg_kms(...) debug_log(__VA_ARGS__)
#define dev_dbg(...) do {} while (0)
#define dev_err(...) do {} while (0)
#define MODE_CLOCK_HIGH 100
#define DRM_OUTPUT_COLOR_FORMAT_YCBCR444 1
struct drm_display_info {bool is_hdmi,has_audio,only420; int max_tmds_clock,color_formats;
    struct {struct {bool supported; struct {bool supported;} scrambling;} scdc;} hdmi;};
struct drm_connector {struct drm_display_info display_info;};
struct drm_bridge {void *dev,*encoder;};
struct drm_crtc_state {struct drm_display_mode adjusted_mode;void *crtc;};
struct drm_connector_state {void *crtc;};
struct drm_atomic_commit {int unused;};
struct meson_hdmitx21 {struct drm_bridge bridge;bool enc2_modeset;void *dev;int packet_lock;};
static struct drm_connector connector;
static struct drm_connector_state conn_state;
static struct drm_crtc_state crtc_state;
static bool ready=true, scdc_fail;
static int scdc_value, scdc_writes, phy_starts, avmute, audio_on;
static u32 top[256], cor[4096], phy[6];
static struct meson_hdmitx21 *bridge_to_hdmitx(struct drm_bridge *b) {return (void *)b;}
static struct drm_connector *drm_atomic_get_new_connector_for_encoder(void *s,void *e)
{return &connector;}
static struct drm_connector_state *drm_atomic_get_new_connector_state(void *s,void *c)
{return &conn_state;}
static struct drm_crtc_state *drm_atomic_get_new_crtc_state(void *s,void *c)
{return &crtc_state;}
static bool meson_t7_display_ready(void *c) {return ready;}
static bool drm_mode_is_420_only(const struct drm_display_info *i,const void *m)
{return i->only420;}
static void top_write(void *tx,u32 reg,u32 val) {top[reg/4]=val;}
static void cor_write(void *tx,u32 reg,u32 val) {cor[reg]=val;}
static void cor_update(void *tx,u32 reg,u32 mask,u32 val)
{cor[reg]=(cor[reg]&~mask)|(val&mask);}
static void cor_pulse(void *tx,u32 reg,u32 mask) {cor[reg]&=~mask;}
static u32 phy_read(void *tx,u32 reg) {return phy[(reg-PHY_CTRL0)/4];}
static void phy_write(void *tx,u32 reg,u32 val)
{phy[(reg-PHY_CTRL0)/4]=val; if(reg==PHY_CTRL0 && val>PHY_CTRL0_BANDGAP) phy_starts++;}
static void msleep(int a) {}
static void mdelay(int a) {}
static void mutex_lock(void *a) {}
static void mutex_unlock(void *a) {}
static int meson_hdmitx21_ddc_write(void *tx,int addr,int offset,int value)
{assert(addr==0x54 && offset==0x20);scdc_value=value;scdc_writes++;return scdc_fail?-EIO:0;}
static void meson_hdmitx21_audio_disconnect(void *tx) {audio_on=0;}
static void meson_hdmitx21_set_avmute(void *tx,bool mute) {avmute=mute;}
static void meson_hdmitx21_hw_init(void *tx) {assert(avmute);}
static void meson_hdmitx21_write_avi(void *tx,void *c,const void *m) {}
static void meson_hdmitx21_write_vendor(void *tx,void *c,const void *m) {}
static void meson_hdmitx21_audio_mode(void *tx,int clock,bool on) {audio_on=on;}
'''
bridge_program += '\n' + function(source, 'meson_t7_clock_calculate')
bridge_program += '\n' + function(source, 'meson_t7_mode_valid')
for name in ('meson_hdmitx21_yuv_sink', 'meson_hdmitx21_phy_on',
             'meson_hdmitx21_phy_off', 'meson_hdmitx21_config',
             'meson_hdmitx21_bridge_mode_valid', 'meson_hdmitx21_bridge_atomic_enable'):
    bridge_program += '\n' + function(bridge_source, name)
phy_values = re.findall(r'HDMI_PHYPARA_\w+', function(hw_t7, 'set21_phy_by_mode_t7'))
bridge_program += '\n' + '\n'.join(f'#define {name} {i}' for i, name in enumerate(dict.fromkeys(phy_values)))
bridge_program += '\n#define ANACTRL_HDMIPHY_CTRL0 0\n#define ANACTRL_HDMIPHY_CTRL3 3\n'
bridge_program += '#define ANACTRL_HDMIPHY_CTRL5 5\n' + function(hw_t7, 'set21_phy_by_mode_t7')
bridge_program += r'''
int main(void) {
    struct meson_hdmitx21 tx={.enc2_modeset=true};
    crtc_state.adjusted_mode=(struct drm_display_mode){594000,3840,4016,4104,4400,
        2160,2168,2178,2250,0};
    struct drm_display_info *info=&connector.display_info;
    info->color_formats=BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);
    info->is_hdmi=info->has_audio=info->hdmi.scdc.supported=true;
    info->hdmi.scdc.scrambling.supported=true;info->max_tmds_clock=600000;
    assert(meson_hdmitx21_bridge_mode_valid(&tx.bridge,info,&crtc_state.adjusted_mode)==MODE_OK);
    info->color_formats=1;
    assert(meson_hdmitx21_bridge_mode_valid(&tx.bridge,info,&crtc_state.adjusted_mode)==MODE_BAD);
    info->color_formats=BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444);info->is_hdmi=false;
    assert(meson_hdmitx21_bridge_mode_valid(&tx.bridge,info,&crtc_state.adjusted_mode)==MODE_BAD);
    info->is_hdmi=true;
    info->hdmi.scdc.scrambling.supported=false;
    assert(meson_hdmitx21_bridge_mode_valid(&tx.bridge,info,&crtc_state.adjusted_mode)==MODE_CLOCK_HIGH);
    info->hdmi.scdc.scrambling.supported=true;info->max_tmds_clock=300000;
    assert(meson_hdmitx21_bridge_mode_valid(&tx.bridge,info,&crtc_state.adjusted_mode)==MODE_CLOCK_HIGH);
    info->max_tmds_clock=600000;info->only420=true;
    assert(meson_hdmitx21_bridge_mode_valid(&tx.bridge,info,&crtc_state.adjusted_mode)==MODE_BAD);
    info->only420=false;scdc_fail=true;
    meson_hdmitx21_bridge_atomic_enable(&tx.bridge,NULL);
    assert(scdc_value==3 && !phy_starts && avmute && !audio_on);
    scdc_fail=false;
    meson_hdmitx21_bridge_atomic_enable(&tx.bridge,NULL);
    assert(scdc_value==3 && phy_starts==1 && !avmute && audio_on);
    assert(top[TOP_TMDS_CLK_PTTN_23/4]==0x03ff03ff && cor[SCRCTL]==0x21);
    for(int i=0;i<3;i++) {
        int clocks[]={74250,241500,594000};
        int modes[]={HDMI_PHYPARA_DEF,HDMI_PHYPARA_3G,HDMI_PHYPARA_6G};
        memset(vendor_regs,0,sizeof(vendor_regs));set21_phy_by_mode_t7(modes[i]);
        meson_hdmitx21_phy_on(&tx,clocks[i]);
        assert(phy[0]==vendor_regs[0] && phy[3]==vendor_regs[3] && phy[5]==vendor_regs[5]);
    }
    crtc_state.adjusted_mode.clock=148500;
    meson_hdmitx21_bridge_atomic_enable(&tx.bridge,NULL);
    assert(scdc_value==0 && cor[SCRCTL]==0x20 && top[TOP_TMDS_CLK_PTTN_01/4]==0x001f001f);
    ready=false;int previous=scdc_writes;
    meson_hdmitx21_bridge_atomic_enable(&tx.bridge,NULL);assert(scdc_writes==previous);
    puts("Bridge: sink mode limits, SCDC failure/1:40/1:10, failed CRTC and vendor PHY drives passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='t7-bridge-') as directory:
    c_file = Path(directory) / 'test.c'
    c_file.write_text(bridge_program)
    executable = Path(directory) / 'test'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                    '-Wno-unused-variable', '-o', str(executable), str(c_file)], check=True)
    subprocess.run([str(executable)], check=True)

if args.dtb:
    def prop(path, name, kind='s'):
        return subprocess.check_output(['fdtget', '-t', kind, str(args.dtb), path, name],
                                       text=True).strip()

    assert prop('/display@ff006840', 'compatible') == 'amlogic,t7-display'
    assert prop('/soc/bus@fe000000/clock-controller@81c0', 'status') == 'disabled'
    assert prop('/vpu@ff000000', 'status') == 'disabled'
    assert prop('/chosen/framebuffer@3fdeec00', 'status') == 'disabled'
    capture_source = (here.parents[2] / 'media/platform/amlogic/t7-hdmirx/t7-vdin-test.c').read_text()
    region_text = capture_source.split('regions[] = {', 1)[1].split('};', 1)[0]
    capture = [(int(a, 16), int(b, 16)) for a, b in re.findall(
        r'\{\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+)\s*}', region_text)]
    cells = [int(cell, 16) for cell in prop('/display@ff006840', 'reg', 'x').split()]
    display = [(cells[i + 1], cells[i + 3]) for i in range(0, len(cells), 4)]
    assert len(display) == 12 and capture
    for start, size in display:
        for other, length in capture:
            assert max(start, other) >= min(start + size, other + length)
    assert prop('/video-capture@ff000000', 'amlogic,split-vcbus') == ''
    print('Native DT: exclusive PLL, split MMIO, no capture overlap, unchanged default M1')
