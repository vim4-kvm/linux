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
    match = re.search(r'^(?:static )?(?:inline )?[\w *]+\b' + name + r'\([^;]*?\)\s*\{',
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
args = parser.parse_args()
here = Path(__file__).resolve().parent
vendor = args.vendor / 'drivers/media/vout/hdmitx21/hw'
source = (here / 'meson_t7_modeset.c').read_text()
registers = (here / 'meson_registers.h').read_text()
defines = '\n'.join(re.findall(r'^#define\s+(?:ENCP_\w+|VENC_INTCTRL)\s+0x[0-9a-f]+.*$',
                               registers, re.M))
types = (here / 'meson_t7_modeset.h').read_text()
clock_type = types[types.index('struct meson_t7_clock {'):types.index('\n};') + 3]
modes = [
    ('720p60', 4, 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750),
    ('1080p50', 31, 148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125),
    ('1080p60', 16, 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125),
    ('1440p60', 1024, 241500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481),
    ('4k30', 95, 297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250),
    ('4k60', 97, 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250),
    ('1080p59.94', 16, 148352, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125),
    ('custom-154MHz', 1024, 154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235),
]
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
struct hdmitx_dev { struct {struct hdmi_format_para fmt_para;} tx_comm; int frl_rate,dsc_en; };
static struct hdmitx_dev device;
static struct hdmitx_dev *get_hdmitx21_device(void) {return &device;}
static const struct hdmi_timing *hdmitx_mode_vic_to_hdmi_timing(int vic)
{ return &device.tx_comm.fmt_para.timing; }
enum hdmi_vic {DUMMY_VIC};
static u32 vendor_regs[0x2000], actual_regs[0x2000], pll[7];
static bool fail_lock;
struct meson_t7_modeset {void *pll;};
static void meson_t7_write(struct meson_t7_modeset *hw,u32 reg,u32 value)
{actual_regs[reg]=value;}
static void hd21_write_reg(u32 reg,u32 value) {vendor_regs[reg]=value;}
static u32 hd21_read_reg(u32 reg) {return vendor_regs[reg] | (fail_lock ? 0 : 0xc0000000);}
static void hd21_set_reg_bits(u32 reg,u32 value,u32 start,u32 width)
{u32 mask=((1U<<width)-1)<<start;vendor_regs[reg]=(vendor_regs[reg]&~mask)|(value<<start);}
static void writel(u32 value,void *reg) {*(u32 *)reg=value;}
static u32 readl(void *reg) {return *(u32 *)reg | (fail_lock ? 0 : 0xc0000000);}
static void usleep_range(int a,int b) {}
'''
hw_t7 = (vendor / 'hw_t7.c').read_text()
venc_source = (vendor / 'enc_cfg_hw.c').read_text()
clk_source = (vendor / 'hw_clk.c').read_text()
pll_defines = '\n'.join(f'#define ANACTRL_HDMIPLL_CTRL{i} {i}' for i in range(7))
vics = set(re.findall(r'HDMI_\d+_\w+', function(hw_t7, 'set_hpll_hclk_v1')))
vic_defines = '\n'.join(f'#define {vic} {vic.split("_")[1]}' for vic in sorted(vics))
driver_names = ('meson_t7_clock_calculate', 'meson_t7_pll_lock', 'meson_t7_pll_recipe',
                'meson_t7_pll_set', 'meson_t7_venc_set')
vendor_names = ('set_hpll_hclk_v1', 'set_hpll_hclk_v2', 'set_hpll_hclk_v3',
                't7_auto_set_hpll')
body = preamble + defines + '\n' + pll_defines + '\n' + vic_defines + '\n' + clock_type
body += '\n' + '\n'.join(function(source, name) for name in driver_names)
body += '\n' + '\n'.join(function(hw_t7, name) for name in vendor_names)
body += '\n' + function(clk_source, 't7_calc_pixel_clk_hpll_vco_od')
body += '\n' + function(venc_source, 'config_tv_enc_calc')
body += '\nint main(void) { struct meson_t7_modeset hw={.pll=pll};\n'
for name, vic, pixel, h, hs, he, ht, v, vs, ve, vt in modes:
    body += f'''
    {{struct drm_display_mode mode={{{pixel},{h},{hs},{he},{ht},{v},{vs},{ve},{vt},0}};
    struct meson_t7_clock clock; u32 vco,od1,od2;
    memset(vendor_regs,0,sizeof(vendor_regs)); memset(actual_regs,0,sizeof(actual_regs));
    memset(pll,0,sizeof(pll));
    device.tx_comm.fmt_para.timing=(struct hdmi_timing){{{vic},{ht},{ht-h},{hs-h},{he-hs},
        {ht-he},{h},{vt},{vs-v},{ve-vs},{vt-ve},{v}}};
    assert(!meson_t7_clock_calculate({pixel},&clock));
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
    fail_lock=true; assert(meson_t7_pll_set(&hw,&clock,false)==-ETIMEDOUT); fail_lock=false;
    }}
'''
body += '''{struct meson_t7_clock clock;
assert(meson_t7_clock_calculate(0,&clock)==-ERANGE);
assert(meson_t7_clock_calculate(594001,&clock)==-ERANGE);}
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
        parameters=[line.split() for line in result.splitlines()]), indent=2) + '\n')
print('Executable vendor PLL/VENC comparison and PLL timeout tests passed')
