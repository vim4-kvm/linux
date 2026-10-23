#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise T7 input slots and actual formatter channel distribution."""
from pathlib import Path
import re
import subprocess
import tempfile

directory = Path(__file__).parent
source = (directory / 'axg-tdmin.c').read_text()
defines = '\n'.join(re.findall(r'^#define.*$', source, re.M))
start = source.index('static int axg_tdmin_prepare')
prepare = source[start:source.index('static const struct snd_soc_dapm_widget', start)]
source = (directory / 'axg-tdm-formatter.c').read_text()
start = source.index('int axg_tdm_formatter_set_channel_masks')
masks = source[start:source.index('EXPORT_SYMBOL_GPL', start)]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define AXG_TDM_NUM_LANES 4
#define SND_SOC_DAIFMT_FORMAT_MASK 0xf
#define SND_SOC_DAIFMT_I2S 1
#define SND_SOC_DAIFMT_LEFT_J 2
#define SND_SOC_DAIFMT_RIGHT_J 3
#define SND_SOC_DAIFMT_DSP_A 4
#define SND_SOC_DAIFMT_DSP_B 5
#define pr_err(...) ((void)0)
#define WARN_ON(x) (x)
struct regmap { u32 regs[16]; };
struct axg_tdm_iface { unsigned int fmt, slots, slot_width; };
struct axg_tdm_stream { struct axg_tdm_iface *iface; unsigned int channels; u32 *mask; };
struct axg_tdm_formatter_hw { unsigned int skew_offset, slot_num_mask; };
static int regmap_update_bits(struct regmap *m, unsigned int r, u32 mask, u32 val) {
    assert(r < sizeof(m->regs) && !(r % 4));
    m->regs[r / 4] = (m->regs[r / 4] & ~mask) | (val & mask); return 0;
}
static int regmap_write(struct regmap *m, unsigned int r, u32 val) {
    assert(r < sizeof(m->regs) && !(r % 4)); m->regs[r / 4] = val; return 0;
}
static unsigned int regmap_get_reg_stride(struct regmap *m) { return 4; }
static bool axg_tdm_lrclk_invert(unsigned int fmt) { return fmt == SND_SOC_DAIFMT_I2S; }
'''
checks = r'''
int main(void) {
    struct regmap map = {0};
    struct axg_tdm_iface iface = {.fmt = SND_SOC_DAIFMT_I2S, .slots = 2, .slot_width = 32};
    u32 mask[4] = {3, 3, 3, 3};
    struct axg_tdm_stream stream = {.iface = &iface, .channels = 6, .mask = mask};
    struct axg_tdm_formatter_hw hw = {.skew_offset = 3, .slot_num_mask = GENMASK(12, 8)};
    for (unsigned int slots = 2; slots <= 32; slots *= 2) {
        iface.slots = slots;
        map.regs[0] = 0xb1bfffff;
        u32 fields = (7U << 16) | BIT(25) | BIT(30) | BIT(5) | 31U | (31U << 8);
        u32 original = map.regs[0] & ~fields;
        assert(!axg_tdmin_prepare(&map, &hw, &stream));
        assert((map.regs[0] & ~fields) == original);
        assert(((map.regs[0] >> 8) & 31) == slots - 1);
        assert(((map.regs[0] >> 16) & 7) == 4);
        assert(map.regs[1] == 0x76543210);
        assert(map.regs[2] == 3 && map.regs[3] == 3);
        assert(map.regs[4] == 3 && map.regs[5] == 0);
    }
    hw.slot_num_mask = GENMASK(11, 8);
    iface.slots = 16;
    map.regs[0] = BIT(12);
    assert(!axg_tdmin_prepare(&map, &hw, &stream));
    assert((map.regs[0] & 0x1f00) == 0x1f00);
    iface.slots = 32;
    u32 saved = map.regs[0];
    assert(axg_tdmin_prepare(&map, &hw, &stream) == -EINVAL);
    assert(map.regs[0] == saved);
    hw.slot_num_mask = 0;
    map.regs[0] = 0x00001a00;
    iface.fmt = SND_SOC_DAIFMT_DSP_A;
    iface.slot_width = 16;
    assert(!axg_tdmin_prepare(&map, &hw, &stream));
    assert((map.regs[0] & 0x1f00) == 0x1a00);
    assert((map.regs[0] & 31) == 15 && !(map.regs[0] & BIT(30)));
    iface.fmt = 0;
    u32 before = map.regs[0];
    assert(axg_tdmin_prepare(&map, &hw, &stream) == -EINVAL);
    assert(map.regs[0] == before);
    stream.channels = 9;
    assert(axg_tdm_formatter_set_channel_masks(&map, &stream, 8) == -EINVAL);
    puts("TDM slots, mux isolation, legacy layout and channel distribution passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'tdmin.c'
    binary = Path(directory) / 'tdmin'
    code.write_text(prefix + defines + '\n' + masks + prepare + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
