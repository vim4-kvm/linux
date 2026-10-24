#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute PPS transactions using vendor register tables, regions and filters."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', required=True, type=Path)
args = parser.parse_args()
directory = Path(__file__).resolve().parent
sink = args.vendor / 'drivers/media/video_sink'
registers = (args.vendor / 'include/linux/amlogic/media/registers/regs/vpp_regs.h').read_text()
definitions = '\n'.join(line for line in registers.splitlines()
                        if re.match(r'#define \w+\s+(?:0x[0-9a-fA-F]+|\d+)\s*$', line))
structure = re.search(r'struct hw_pps_reg_s \{.*?\n\};',
                      (sink / 'video_reg.h').read_text(), re.S)[0]
table = re.search(r'struct hw_pps_reg_s pps_reg_t7_array.*?\n};',
                  (sink / 'video_reg.c').read_text(), re.S)[0]
table = re.sub(r'\[MAX_VD_LAYER_T7 \+ 1\]', '[]', table)
coefs = re.search(r'const u32 vpp_filter_coefs_bicubic\[\].*?\n};',
                  (sink / 'vpp.c').read_text(), re.S)[0]
hw = (sink / 'video_hw.c').read_text()
scaler = hw[hw.index('static void vd1_scaler_setting('):
            hw.index('static void vdx_scaler_setting(')]
calls = []
for field in ('vd_hsc_region12_startp', 'vd_hsc_region34_startp', 'vd_hsc_region4_endp',
              'vd_vsc_region12_startp', 'vd_vsc_region34_startp', 'vd_vsc_region4_endp'):
    calls.append(re.search(r'cur_dev->rdma_func\[vpp_index\].rdma_wr\s*\('
                           r'\s*vd_pps_reg->' + field + r',.*?;', scaler, re.S)[0])
video_h = (args.vendor / 'include/linux/amlogic/media/video_sink/video.h').read_text()
definitions += '\n' + '\n'.join(line for line in video_h.splitlines()
                                if re.match(r'#define VPP_REGION\w+\s+\w+\s*$', line))
source = '\n'.join(re.sub(r'^#include[^\n]*\n', '', (directory / name).read_text(), flags=re.M)
                    for name in ('meson_t7_video.h', 'meson_t7_video.c',
                                 'meson_t7_video_pps.c'))
body = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h))))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define div_u64(a,b) ((u64)(a)/(b))
#define U32_MAX UINT32_MAX
#define DRM_FORMAT_NV12 1
#define DRM_FORMAT_NV21 2
'''
body += definitions + '\n' + structure + table + coefs + source
body += r'''
static u32 regs[0x6000];
static void wr(u32 r,u32 v) {assert(r<ARRAY_SIZE(regs)); regs[r]=v;}
static struct {struct {void (*rdma_wr)(u32,u32);} rdma_func[1];}
    device={.rdma_func={{wr}}}, *cur_dev=&device;
static void vendor_regions(unsigned int index,u32 ow,u32 oh)
{
    u32 r1=0,r2=ow-1,r3=ow-1,vpp_index=0;
    struct hw_pps_reg_s *vd_pps_reg=&pps_reg_t7_array[index];
'''
body += '\n'.join(calls[:3]) + '\nr1=oh-1;\n' + '\n'.join(calls[3:]) + '\n}\n'
body += r'''
static void compare(u32 index,u32 w,u32 h,u32 ow,u32 oh)
{
    u32 table[MESON_T7_VIDEO_PPS_PAIRS*2+2];
    struct hw_pps_reg_s *r=&pps_reg_t7_array[index];
    struct meson_t7_video_phase p;
    assert(!meson_t7_video_phase_prepare(w,h,ow,oh,&p));
    memset(table,0xa5,sizeof(table));
    int bytes=meson_t7_video_pps_build(table,MESON_T7_VIDEO_PPS_PAIRS,index,w,h,ow,oh,
                                      0xa5a5ffff);
    assert(bytes==MESON_T7_VIDEO_PPS_PAIRS*8);
    assert(table[MESON_T7_VIDEO_PPS_PAIRS*2]==0xa5a5a5a5);
    u32 expected[0x6000];
    memset(regs,0,sizeof(regs)); vendor_regions(index,ow,oh);
    memcpy(expected,regs,sizeof(regs));
    memset(regs,0,sizeof(regs));
    u32 coef_index=0,phase_count[2]={0};
    for (int n=0;n<bytes/4;n+=2) {
        u32 reg=table[n],value=table[n+1];
        if (reg==r->vd_scale_coef_idx) coef_index=value>>8;
        if (reg==r->vd_scale_coef) {
            assert(coef_index<2);
            assert(phase_count[coef_index]<33);
            assert(value==vpp_filter_coefs_bicubic[2+phase_count[coef_index]++]);
        }
        wr(reg,value);
    }
    assert(phase_count[0]==33 && phase_count[1]==33);
    const u32 regions[]={r->vd_hsc_region12_startp,r->vd_hsc_region34_startp,
        r->vd_hsc_region4_endp,r->vd_vsc_region12_startp,r->vd_vsc_region34_startp,
        r->vd_vsc_region4_endp};
    for (u32 n=0;n<ARRAY_SIZE(regions);n++)
        assert(expected[regions[n]]==regs[regions[n]]);
    assert(regs[r->vd_vsc_start_phase_step]==p.vstep);
    assert(regs[r->vd_hsc_start_phase_step]==p.hstep);
    assert(regs[r->vd_vsc_init_phase]==p.vphase);
    assert(regs[r->vd_vsc_phase_ctrl]==p.vreceive_repeat);
    assert(regs[index?VD2_SC_GCLK_CTRL_T7:VPP_SC_GCLK_CTRL_T7]==0xa5a50000);
    assert(!(regs[r->vd_sc_misc] & (BIT(19)|BIT(20)|BIT(21)|BIT(23))));
    u32 snapshot[ARRAY_SIZE(table)]; memcpy(snapshot,table,sizeof(table));
    assert(meson_t7_video_pps_build(table,MESON_T7_VIDEO_PPS_PAIRS-1,index,w,h,ow,oh,0)
           ==-ENOSPC);
    assert(!memcmp(snapshot,table,sizeof(table)));
    assert(meson_t7_video_pps_build(table,MESON_T7_VIDEO_PPS_PAIRS,index,w,h,0,oh,0)
           ==-EINVAL);
    assert(!memcmp(snapshot,table,sizeof(table)));
}
int main(void)
{
    for (u32 i=0;i<2;i++) {
        compare(i,3840,2160,3840,2160);
        compare(i,3840,2160,1920,1080);
        compare(i,1920,1080,1280,720);
        compare(i,1280,720,1920,1080);
        compare(i,3840,2160,960,540);
        compare(i,960,540,3840,2160);
    }
}
'''
with tempfile.TemporaryDirectory() as work:
    root = Path(work)
    (root / 'pps.c').write_text(body)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(root / 'pps.c'), '-o', str(root / 'pps')],
                   check=True)
    subprocess.run([str(root / 'pps')], check=True)
print('T7 PPS transactions: both vendor banks, regions, 33-phase coefficients and bounds passed')
