#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute OSD transactions and compare scaler programming with vendor C."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', type=Path, required=True)
args = parser.parse_args()
directory = Path(__file__).resolve().parent
vendor_dir = args.vendor / 'drivers/drm/vpu-hw'
vendor = (vendor_dir / 'meson_osd_scaler.c').read_text()
source = (directory / 'meson_t7_layers.c').read_text()
header = (directory / 'meson_t7_layers.h').read_text()
registers = (directory / 'meson_registers.h').read_text()


def without_includes(text):
    return re.sub(r'^#include[^\n]*\n', '', text, flags=re.M)


def initializer(text, name):
    start = text.index(name)
    return text[start:text.index('};', start) + 2]


# Catch using the old G12 OSD3 layout or assuming all OSD banks have equal offsets.
vendor_mif = (vendor_dir / 'meson_vpu_osd_mif.h').read_text()
definitions = dict(re.findall(r'^#define\s+(\w+)\s+(0x[0-9a-f]+)\s*$', vendor_mif, re.M))
rows = source.split('} regs[] = {')[1].split('};')[0]
addresses = [int(value, 16) for value in re.findall(r'0x[0-9a-f]+', rows)]
suffixes = ('CTRL_STAT', 'CTRL_STAT2', 'BLK0_CFG_W0', 'BLK0_CFG_W1', 'BLK0_CFG_W2',
            'BLK0_CFG_W3', 'BLK0_CFG_W4', 'BLK1_CFG_W4', 'BLK2_CFG_W4',
            'FIFO_CTRL_STAT', 'MALI_UNPACK_CTRL')
assert addresses == [int(definitions[f'VIU_OSD{index}_{suffix}'], 16)
                     for index in range(1, 4) for suffix in suffixes]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h))))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define swap(a,b) do { __typeof__(a) t = a; a = b; b = t; } while (0)
#define fallthrough __attribute__((fallthrough))
#define div_u64(a,b) ((u64)(a)/(b))
#define do_div(a,b) ((a) /= (b))
#define DRM_MODE_REFLECT_X BIT(4)
#define DRM_MODE_REFLECT_Y BIT(5)
#define DRM_MODE_BLEND_PIXEL_NONE 0
#define DRM_MODE_BLEND_COVERAGE 1
#define DRM_FORMAT_RGB888 1
#define DRM_FORMAT_XRGB8888 2
#define DRM_FORMAT_XBGR8888 3
#define DRM_FORMAT_ARGB8888 4
#define DRM_FORMAT_ABGR8888 5
#define DRM_SCALING_FILTER_BICUBIC_SHARP 1
#define DRM_SCALING_FILTER_REPEATE 10
#define OSD_V2 2
#define OSD_V7 7
static int osdscaler_h_filter_mode = -1, osdscaler_v_filter_mode = -1;
struct meson_vpu_scaler {u32 linebuffer, bank_length;};
struct meson_vpu_pipeline {u8 osd_version;};
struct meson_vpu_block {
    struct meson_vpu_pipeline *pipeline;
    struct meson_vpu_scaler *scaler;
    bool init_done;
};
#define to_scaler_block(b) ((b)->scaler)
struct meson_vpu_scaler_state {
    u32 input_width, input_height, output_width, output_height;
    u32 scan_mode_out, scaler_filter_mode;
};
struct rdma_reg_ops {
    void (*rdma_write_reg)(u32, u32);
    void (*rdma_write_reg_bits)(u32, u32, unsigned int, unsigned int);
};
static u32 hw[0x6000], coefficients[2][33];
static unsigned int phase, axis;
static void write_reg(u32 reg, u32 value)
{
    assert(reg < ARRAY_SIZE(hw));
    hw[reg] = value;
    if ((reg & 0xff3f) == 0x5a0c) {axis = !!(value & BIT(8)); phase = 0;}
    if ((reg & 0xff3f) == 0x5a0d) {
        assert(phase < 33); coefficients[axis][phase++] = value;
    }
}
static void write_bits(u32 reg, u32 value, unsigned int shift, unsigned int bits)
{
    u32 mask = ((1U << bits) - 1) << shift;
    write_reg(reg, (hw[reg] & ~mask) | ((value << shift) & mask));
}
'''
checks = r'''
int main(void)
{
    u32 table[MESON_T7_RDMA_PAIRS * 2 + 2];
    struct meson_t7_osd_layer layers[3] = {0};
    struct meson_vpu_pipeline pipeline = {.osd_version=5};
    struct meson_vpu_scaler scaler = {.linebuffer=1920, .bank_length=4};
    struct meson_vpu_block block = {.pipeline=&pipeline, .scaler=&scaler};
    struct rdma_reg_ops ops = {.rdma_write_reg=write_reg, .rdma_write_reg_bits=write_bits};
    const unsigned int sizes[][4] = {
        {1920,1080,1920,1080}, {1920,1080,3840,2160}, {3840,2160,1920,1080},
        {3840,2160,960,540}, {1280,720,640,360}, {720,480,180,120},
        {1919,1079,2048,1152}, {1921,1081,1920,1080}, {256,256,512,512},
    };
    for (unsigned int osd=0; osd<3; osd++) {
        for (unsigned int sample=0; sample<ARRAY_SIZE(sizes); sample++) {
            memset(layers, 0, sizeof(layers));
            layers[osd]=(struct meson_t7_osd_layer){.enabled=true,
                .format=DRM_FORMAT_ARGB8888, .pitch=15360, .address=0x20000000,
                .src_w=sizes[sample][0], .src_h=sizes[sample][1],
                .width=sizes[sample][2], .height=sizes[sample][3],
                .alpha=65535, .blend=DRM_MODE_BLEND_COVERAGE};
            memset(hw, 0, sizeof(hw)); memset(coefficients, 0, sizeof(coefficients));
            int bytes=meson_t7_osd_layers_build(table, MESON_T7_RDMA_PAIRS, layers,
                                               3840,2160,0xa5a5a5a5,0x5a5a5a5a);
            assert(bytes>0 && bytes<=MESON_T7_RDMA_PAIRS*8);
            for (int i=0; i<bytes/4; i+=2) write_reg(table[i],table[i+1]);
            u32 actual[14], actual_coefficients[2][33];
            memcpy(actual, hw+0x5a00+osd*0x40, sizeof(actual));
            memcpy(actual_coefficients,coefficients,sizeof(coefficients));
            struct meson_vpu_scaler_state state={.input_width=sizes[sample][0],
                .input_height=sizes[sample][1],.output_width=sizes[sample][2],
                .output_height=sizes[sample][3]};
            memset(hw, 0, sizeof(hw)); memset(coefficients, 0, sizeof(coefficients));
            block.init_done=false;
            osd_scaler_config(&osd_scaler_t7_reg[osd], &state, &block, &ops);
            assert(!memcmp(actual,hw+0x5a00+osd*0x40,sizeof(actual)));
            assert(!memcmp(actual_coefficients,coefficients,sizeof(coefficients)));
        }
    }
    /* All three engines and z-order changes must be in a single bounded table. */
    for (unsigned int i=0; i<3; i++)
        layers[i]=(struct meson_t7_osd_layer){.enabled=true,
            .format=DRM_FORMAT_XRGB8888,.pitch=7680,.address=0x21000000+i*0x1000000,
            .src_w=1920,.src_h=1080,.width=1920,.height=1080,.alpha=65535,.zpos=2-i};
    int bytes=meson_t7_osd_layers_build(table,MESON_T7_RDMA_PAIRS,layers,
                                       1920,1080,0xffffffff,0x12345678);
    memset(hw,0,sizeof(hw));
    for (int i=0;i<bytes/4;i+=2) write_reg(table[i],table[i+1]);
    assert((hw[VIU_OSD_BLEND_CTRL]&0xffff)==0x4123);
    assert((hw[0x1a10]&1) && (hw[0x1a30]&1) && (hw[0x3d80]&1));
    assert(hw[0x1a0d]==(0xffffffff & ~GENMASK(25,24)));
    assert((hw[OSD_PATH_MISC_CTRL]&0xffff)==0x5678);
    /* Truncated descriptors never overflow their allocation or arm hardware. */
    table[6]=0xdeadbeef;
    assert(meson_t7_osd_layers_build(table,3,layers,1920,1080,0,0)==-ENOSPC);
    assert(table[6]==0xdeadbeef);
    layers[0].width=0;
    assert(meson_t7_osd_layers_build(table,MESON_T7_RDMA_PAIRS,layers,
                                    1920,1080,0,0)==-EINVAL);
    memset(layers,0,sizeof(layers));
    bytes=meson_t7_osd_layers_build(table,MESON_T7_RDMA_PAIRS,layers,1920,1080,0,0);
    for (int i=0;i<bytes/4;i+=2) write_reg(table[i],table[i+1]);
    assert(!(hw[0x1a10]&1) && !(hw[0x1a30]&1) && !(hw[0x3d80]&1));
}
'''
vendor_code = vendor[vendor.index('static unsigned int __osd_filter_coefs_bicubic_sharp'):
                     vendor.index('static void scaler_size_check(')]
register_table = initializer(vendor, 'static struct osd_scaler_reg_s osd_scaler_t7_reg')
body = (prefix + without_includes(registers) + without_includes(header) +
        without_includes(source) + (vendor_dir / 'meson_osd_scaler.h').read_text() +
        register_table + vendor_code + checks)
with tempfile.TemporaryDirectory() as work:
    path = Path(work)
    (path / 'layers.c').write_text(body)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(path / 'layers.c'),
                    '-o', str(path / 'layers')], check=True)
    subprocess.run([str(path / 'layers')], check=True)
print('OSD transactions: 27 vendor scaler/coefficients cases, MIF banks, zpos, blank and bounds passed')
