#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute progressive T7 video MIF parameters against vendor C fragments.

No MMIO, fetch enable, scaler, blend or buffer retirement is exercised here.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:void|u32)\s+' + name + r'\(', source)
    start = match.start()
    brace = source.index('{', start)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if not depth:
            return source[start:end + 1]
    raise AssertionError(name)


def without_includes(source):
    return re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', required=True, type=Path)
args = parser.parse_args()
directory = Path(__file__).resolve().parent
sink = args.vendor / 'drivers/media/video_sink'
hw = (sink / 'video_hw.c').read_text()
reg_header = (sink / 'video_reg.h').read_text()
registers = (args.vendor / 'include/linux/amlogic/media/registers/regs/viu_regs.h').read_text()
definitions = '\n'.join(line for line in registers.splitlines()
                        if re.match(r'#define (SC2_VD[12]_IF0_|VD[12]_IF0_)', line))
for path in ('video_sink/video.h', 'vfm/vframe.h'):
    text = (args.vendor / 'include/linux/amlogic/media' / path).read_text()
    definitions += '\n' + '\n'.join(line for line in text.splitlines()
                                    if re.match(r'#define (VDIF_|VD1_FMT_|HFORMATTER_|VFORMATTER_|'
                                                r'VIDTYPE_|BITDEPTH_|FULL_PACK_)', line)
                                    and not line.endswith('\\'))
linear_struct = re.search(r'struct hw_vd_linear_reg_s \{.*?\n\};', reg_header, re.S)[0]
mif_struct = re.search(r'struct hw_vd_reg_s \{.*?\n\};', reg_header, re.S)[0]
position = (sink / 'video_priv.h').read_text()
position = re.search(r'struct mif_pos_s \{.*?\n\};', position, re.S)[0]
common = (sink / 'video_common.c').read_text()
body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define U32_MAX UINT32_MAX
#define BIT(n) (1U << (n))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define DRM_FORMAT_NV12 1
#define DRM_FORMAT_NV21 2
#define pr_err(...) assert(0)
#define S5_DISPLAY_MODULE 5
#define C3_DISPLAY_MODULE 3
#define T7_DISPLAY_MODULE 7
#define MESON_CPU_MAJOR_ID_G12A 12
#define DEBUG_FLAG_GOFIELD_MANUL BIT(5)
#define VFRAME_FLAG_VIDEO_LINEAR 0x400
#define H_MIRROR 1
#define V_MIRROR 2
'''
body += definitions + '\n' + linear_struct + '\n' + mif_struct + '\n' + position + '\n'
body += without_includes((directory / 'meson_t7_video.h').read_text())
body += without_includes((directory / 'meson_t7_video.c').read_text())
body += r'''
static u32 regs[0x5000];
static void wr(u32 reg, u32 value) { assert(reg < 0x5000); regs[reg] = value; }
static void bits(u32 reg, u32 value, unsigned int start, unsigned int width)
{
    u32 mask = (UINT32_MAX >> (32-width)) << start;
    wr(reg, (regs[reg] & ~mask) | ((value << start) & mask));
}
struct canvas_config_s {u64 phy_addr; u32 width;};
struct vframe_s {u32 type, flag;};
struct video_layer_s {struct hw_vd_linear_reg_s vd_mif_linear_reg; u8 vpp_index;};
struct vpp_frame_par_s {u32 hscale_skip_count;};
static struct {
    u32 display_module;
    struct {void (*rdma_wr)(u32,u32); void (*rdma_wr_bits)(u32,u32,unsigned int,unsigned int);} rdma_func[1];
} device = {.display_module=T7_DISPLAY_MODULE, .rdma_func={{wr,bits}}}, *cur_dev=&device;
static u32 vpp_hold_line[1] = {8};
static bool is_amdv_on(void) {return false;}
static bool is_dovi_tv_on(void) {return false;}
static bool is_meson_txlx_cpu(void) {return false;}
static bool cpu_after_eq(int cpu) {return cpu == MESON_CPU_MAJOR_ID_G12A;}
static bool is_crop_left_odd(struct vpp_frame_par_s *p) {(void)p; return false;}
static void set_vd_mif_linear_s5(struct video_layer_s *l, struct canvas_config_s *c,
                               u32 n, struct vframe_s *v, u32 s) {assert(0);}
'''
body += function(common, 'viu_line_stride') + '\n'
body += function(hw, 'set_vd_mif_linear') + '\n'
scope = function(hw, 'vd_mif_setting')
scope = scope[scope.index('/* vd horizontal setting */'):scope.index('if (setting->skip_afbc')]
body += 'static void vendor_scope(struct mif_pos_s *setting) {\n'
body += 'struct hw_vd_reg_s *vd_mif_reg=setting->p_vd_mif_reg;\n'
body += 'u32 ls, le, h_skip=1; u8 vpp_index=0;\n' + scope + '\n}\n'
for index, name in enumerate(('vd1_set_dcu', 'vdx_set_dcu')):
    dcu = function(hw, name)
    # Run the original control and formatter blocks, with progressive NV12/NV21
    # inputs and no DI, Dolby Vision, interlace, skip, mirror or MVC state.
    start = (dcu.rindex('if (cur_dev->display_module != C3_DISPLAY_MODULE)\n\t\tr =')
             if not index else dcu.rindex('r = (3 << VDIF_URGENT_BIT)'))
    control = dcu[start:dcu.index('/* FIXME: don\'t use glayer_info[', start)]
    formatter = dcu[dcu.rindex('/* chroma formatter */'):dcu.index('/* LOOP/SKIP pattern */')]
    body += f'static void vendor_dcu{index}(u32 type) {{\n'
    body += r'''
    u32 r, hphase=0, hformatter, vformatter, vphase, vini_phase, vrepeat;
    u8 vpp_index=0;
    bool is_mvc=false;
    struct vpp_frame_par_s params={0}, *frame_par=&params;
    struct vframe_s frame={.type=type, .flag=VFRAME_FLAG_VIDEO_LINEAR}, *vf=&frame;
    struct {u32 global_debug;} layer_data={0}, *layer=&layer_data;
    struct hw_vd_reg_s mif={.vd_if0_gen_reg=1, .vd_if0_gen_reg2=2,
                           .viu_vd_fmt_ctrl=3}, *vd_mif_reg=&mif, *vd2_mif_reg=&mif;
'''
    body += control + formatter + '\n}\n'
body += r'''
static void compare(unsigned int index, u32 width, u32 height, u32 pitch, bool nv21)
{
    struct meson_t7_video_frame f={.format=nv21?DRM_FORMAT_NV21:DRM_FORMAT_NV12,
        .width=width, .height=height, .x=2, .y=4, .crop_width=width-4, .crop_height=height-8,
        .pitch={pitch,pitch+32}, .address={0x100000,0x2000000},
        .size={(u64)pitch*height,(u64)(pitch+32)*height/2}};
    struct meson_t7_video_mif m;
    assert(!meson_t7_video_mif_prepare(&f,index,&m));
    assert(!(m.gen & 1));
    (index ? vendor_dcu1 : vendor_dcu0)(nv21?VIDTYPE_VIU_NV21:VIDTYPE_VIU_NV12);
    if (m.gen != (regs[1] & ~1U))
        fprintf(stderr,"index=%u gen=%x vendor=%x\n",index,m.gen,regs[1]);
    assert(m.gen == (regs[1] & ~1U));
    assert(m.gen2 == regs[2]);
    assert(m.format_ctrl == regs[3]);
    u32 base=index?SC2_VD2_IF0_GEN_REG:SC2_VD1_IF0_GEN_REG;
    assert(m.base==base);
    struct video_layer_s l={.vd_mif_linear_reg={base+0x20,base+0x21,base+0x22,
        base+0x23,base+0x24,base+0x25,base+0x26,base+0x27,base+0x28,base+0x29}};
    struct canvas_config_s cfg[2]={{f.address[0],f.pitch[0]},{f.address[1],f.pitch[1]}};
    regs[base+0x24]=0xa5a50000;
    set_vd_mif_linear(&l,cfg,2,NULL,0);
    for (unsigned int p=0;p<3;p++) assert(m.address[p]==regs[base+0x20+p]);
    assert(m.stride0==regs[base+0x23]);
    assert(m.stride1==(regs[base+0x24]&0xffff));
    assert((regs[base+0x24]&0xffff0000)==0xa5a50000);
    struct hw_vd_reg_s scope_regs={.vd_if0_luma_x0=base+3,.vd_if0_luma_y0=base+4,
        .vd_if0_chroma_x0=base+5,.vd_if0_chroma_y0=base+6,.viu_vd_fmt_w=base+0x1e};
    struct mif_pos_s pos={.p_vd_mif_reg=&scope_regs,
        .l_hs_luma=f.x,.l_he_luma=f.x+f.crop_width-1,
        .l_hs_chrm=f.x/2,.l_he_chrm=(f.x+f.crop_width-1)/2,
        .l_vs_luma=f.y,.l_ve_luma=f.y+f.crop_height-1,
        .l_vs_chrm=f.y/2,.l_ve_chrm=(f.y+f.crop_height-1)/2,
        .start_x_lines=f.x,.end_x_lines=f.x+f.crop_width-1};
    vendor_scope(&pos);
    assert(m.luma_x==regs[base+3]); assert(m.luma_y==regs[base+4]);
    assert(m.chroma_x==regs[base+5]); assert(m.chroma_y==regs[base+6]);
    assert(m.format_width==regs[base+0x1e]);
    /* Rejection is transactional: never leave a partly usable MIF image. */
    struct meson_t7_video_mif original=m;
    f.size[1]--;
    assert(meson_t7_video_mif_prepare(&f,index,&m)==-ERANGE);
    assert(!memcmp(&original,&m,sizeof(m)));
    f.size[1]++;
    f.address[1]=UINT32_MAX-15;
    assert(meson_t7_video_mif_prepare(&f,index,&m)==-ERANGE);
    f.address[1]=0x2000000;
    f.x=1;
    assert(meson_t7_video_mif_prepare(&f,index,&m)==-EINVAL);
    f.x=width;
    assert(meson_t7_video_mif_prepare(&f,index,&m)==-EINVAL);
    f.x=2; f.format=999;
    assert(meson_t7_video_mif_prepare(&f,index,&m)==-EINVAL);
    f.format=DRM_FORMAT_NV12; f.pitch[1]=width-1;
    assert(meson_t7_video_mif_prepare(&f,index,&m)==-EINVAL);
}
int main(void)
{
    const u32 geometry[][2]={{720,400},{1280,720},{1920,1080},{3840,2160}};
    struct meson_t7_video_mif m;
    assert(meson_t7_video_mif_prepare(NULL,0,&m)==-EINVAL);
    for (unsigned int i=0;i<4;i++)
        for (unsigned int index=0;index<2;index++)
            for (unsigned int uv=0;uv<2;uv++)
                for (unsigned int extra=0;extra<2;extra++)
                    compare(index,geometry[i][0],geometry[i][1],ALIGN(geometry[i][0],64)+extra*32,uv);
}
'''
with tempfile.TemporaryDirectory() as work:
    root = Path(work)
    (root / 'video.c').write_text(body)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-variable',
                    '-fsanitize=undefined', str(root / 'video.c'), '-o', str(root / 'video')],
                   check=True)
    subprocess.run([str(root / 'video')], check=True)
print('T7 MIF: 64 vendor address/control/formatter cases and rejection paths passed')
