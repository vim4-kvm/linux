#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute OSD alpha/reflection against the vendor MIF register functions."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    start = re.search(r'(?m)^(?:static )?(?:void|u32)\s+' + name + r'\(', source).start()
    brace = source.index('{', start)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if not depth:
            return source[start:end + 1]
    raise AssertionError(name)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', required=True, type=Path)
args = parser.parse_args()
source = Path(__file__).with_name('meson_t7_handoff.c').read_text()
vendor = (args.vendor / 'drivers/drm/vpu-hw/meson_vpu_osd_mif.c').read_text()
conversion = re.search(r'global_alpha = mvos->global_alpha >> 8;\s*'
                       r'if \(global_alpha == 0xff\)\s*global_alpha = 0x100;', vendor)[0]
body = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h))))
#define DRM_MODE_REFLECT_X BIT(4)
#define DRM_MODE_REFLECT_Y BIT(5)
struct meson_vpu_block {int unused;};
struct osd_mif_reg_s {u32 viu_osd_ctrl_stat, viu_osd_blk0_cfg_w0;};
struct rdma_reg_ops {void (*rdma_write_reg_bits)(u32, u32, int, int);};
static u32 registers[2];
static void update(u32 index, u32 value, int start, int width)
{
    u32 mask = ((1U << width) - 1) << start;
    registers[index] = (registers[index] & ~mask) | (value << start);
}
'''
for name in ('meson_t7_osd_alpha', 'meson_t7_osd_reflect'):
    body += function(source, name) + '\n'
for name in ('osd_global_alpha_set', 'osd_reverse_x_enable', 'osd_reverse_y_enable'):
    body += function(vendor, name) + '\n'
body += 'static u16 convert(u16 alpha) {\n'
body += 'struct {u16 global_alpha;} storage = {alpha}, *mvos = &storage; u16 global_alpha;\n'
body += conversion + '\nreturn global_alpha; }\n'
body += r'''
int main(void)
{
    struct meson_vpu_block block = {0};
    struct rdma_reg_ops ops = {.rdma_write_reg_bits=update};
    struct osd_mif_reg_s reg = {.viu_osd_ctrl_stat=0, .viu_osd_blk0_cfg_w0=1};
    for (unsigned int alpha = 0; alpha <= UINT16_MAX; alpha++) {
        u32 original = alpha * 0x10101U;
        registers[0] = original;
        osd_global_alpha_set(&block, &ops, &reg, convert(alpha));
        assert(meson_t7_osd_alpha(original, alpha) == registers[0]);
    }
    for (unsigned int seed = 0; seed < 256; seed++) {
        for (unsigned int xy = 0; xy < 4; xy++) {
            u32 original = seed * 0x01010101U;
            u32 rotation = (xy & 1 ? DRM_MODE_REFLECT_X : 0) |
                           (xy & 2 ? DRM_MODE_REFLECT_Y : 0);
            registers[1] = original;
            osd_reverse_x_enable(&block, &ops, &reg, xy & 1);
            osd_reverse_y_enable(&block, &ops, &reg, xy & 2);
            assert(meson_t7_osd_reflect(original, rotation) == registers[1]);
        }
    }
}
'''
with tempfile.TemporaryDirectory() as work:
    path = Path(work)
    (path / 'osd.c').write_text(body)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(path / 'osd.c'),
                    '-o', str(path / 'osd')], check=True)
    subprocess.run([str(path / 'osd')], check=True)
print('Vendor OSD comparison: all 65536 alpha values and 1024 reflection combinations passed')
