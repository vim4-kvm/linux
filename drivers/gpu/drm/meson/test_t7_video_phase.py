#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare progressive PPS phase math with the vendor frame-to-video function."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', required=True, type=Path)
args = parser.parse_args()
directory = Path(__file__).resolve().parent
vendor = args.vendor / 'drivers/media/video_sink'
vpp = (vendor / 'vpp.c').read_text()
hw = (vendor / 'video_hw.c').read_text()
header = (args.vendor / 'include/linux/amlogic/media/video_sink/vpp.h').read_text()
enum = re.search(r'enum f2v_vphase_type_e \{.*?\n\}', header, re.S)[0] + ';'
function = re.search(r'static void f2v_get_vertical_phase\(.*?^\}', vpp, re.S | re.M)[0]
positions = re.findall(r'static const u8 f2v_420_\w+\[.*?;', vpp)
skip = re.search(r'static const u8 skip_tab\[.*?;', hw)[0]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#define BIT(n) (1U << (n))
#define div_u64(a,b) ((u64)(a)/(b))
#define ZOOM_BITS 18
#define PHASE_BITS 8
#define U32_MAX UINT32_MAX
#define DRM_FORMAT_NV12 1
#define DRM_FORMAT_NV21 2
struct f2v_vphase_s {int repeat_skip; u8 phase;};
'''
source = '\n'.join(re.sub(r'^#include[^\n]*\n', '', (directory / name).read_text(), flags=re.M)
                    for name in ('meson_t7_video.h', 'meson_t7_video.c'))
body = prefix + enum + '\n' + '\n'.join(positions) + skip + function + source
body += r'''
static void compare(u32 w, u32 h, u32 ow, u32 oh)
{
    struct meson_t7_video_phase p;
    struct f2v_vphase_s expected[F2V_TYPE_MAX];
    assert(!meson_t7_video_phase_prepare(w,h,ow,oh,&p));
    u32 ratio=(h<<18)/oh;
    f2v_get_vertical_phase(ratio,0,expected,0);
    assert(p.hstep==((w<<18)/ow)<<6);
    assert(p.vstep==ratio<<6);
    assert(p.vphase==((u32)expected[F2V_P2P].phase<<8));
    assert(expected[F2V_P2P].repeat_skip>=0);
    assert(p.vreceive_repeat==skip_tab[expected[F2V_P2P].repeat_skip]);
    struct meson_t7_video_phase old=p;
    assert(meson_t7_video_phase_prepare(w,h,ow,0,&p)==-EINVAL);
    assert(!memcmp(&p,&old,sizeof(p)));
}
int main(void)
{
    /* Exercise both sides of unity and skip transitions, including exact 4:1. */
    for (u32 out=64;out<=2160;out+=2) {
        compare(out,out,out,out);
        if (out>64) compare(out,out-2,out,out);
        if (out<2160) compare(out,out+2,out,out);
        if (out<=540) compare(out*4,out*4,out,out);
        if (out<=1080) compare(out*2,out*2,out,out);
        if (out>=256 && !(out&3)) compare(out/4,out/4,out,out);
    }
    compare(3840,2160,1920,1080);
    compare(1920,1080,1280,720);
    compare(1280,720,1920,1080);
    struct meson_t7_video_phase p={0};
    assert(meson_t7_video_phase_prepare(3840,2160,959,540,&p)==-EINVAL);
    assert(meson_t7_video_phase_prepare(1920,1080,3841,2160,&p)==-EINVAL);
    assert(meson_t7_video_phase_prepare(0,1080,1920,1080,&p)==-EINVAL);
    assert(meson_t7_video_phase_prepare(1920,1080,1920,1080,NULL)==-EINVAL);
}
'''
with tempfile.TemporaryDirectory() as work:
    root = Path(work)
    (root / 'phase.c').write_text(body)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(root / 'phase.c'), '-o', str(root / 'phase')],
                   check=True)
    subprocess.run([str(root / 'phase')], check=True)
print('T7 PPS: progressive scale boundaries and vendor vertical phase comparison passed')
