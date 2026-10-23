#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute playback FIFO configuration while preserving all active routes."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).with_name('axg-frddr.c').read_text()
defines = '\n'.join(re.findall(r'^#define.*$', source, re.M))
start = source.index('static int axg_frddr_dai_hw_params')
common = source[start:source.index('static int axg_frddr_dai_startup', start)]
start = source.index('static int t7_frddr_dai_hw_params')
function = source[start:source.index('static const struct snd_soc_dai_ops', start)]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define FIELD_PREP(mask,value) (((value) << __builtin_ctz(mask)) & (mask))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define AXG_FIFO_BURST 8
#define FIFO_CTRL0 0
#define FIFO_CTRL1 4
#define FIFO_CTRL2 40
#define CTRL1_FRDDR_DEPTH GENMASK(31,24)
struct regmap { unsigned int regs[16]; int fail_reg; };
struct axg_fifo { struct regmap *map; unsigned int depth; };
struct snd_soc_dai { struct axg_fifo *fifo; };
struct snd_pcm_substream { int unused; };
struct snd_pcm_hw_params { unsigned int channels, period; };
#define snd_soc_dai_get_drvdata(d) ((d)->fifo)
#define params_channels(p) ((p)->channels)
#define params_period_bytes(p) ((p)->period)
static int regmap_update_bits(struct regmap *m, unsigned int r,
                              unsigned int mask, unsigned int val) {
    assert(r < sizeof(m->regs) && !(r % 4));
    if (m->fail_reg == (int)r) return -EIO;
    m->regs[r / 4] = (m->regs[r / 4] & ~mask) | (val & mask); return 0;
}
'''
checks = r'''
int main(void) {
    struct regmap map = {.fail_reg = -1};
    struct axg_fifo fifo = {.map = &map, .depth = 512};
    struct snd_soc_dai dai = {.fifo = &fifo};
    struct snd_pcm_hw_params p = {.channels = 2, .period = 1024};
    for (unsigned int channels = 1; channels <= 128; channels *= 2) {
        p.channels = channels;
        map.regs[0] = 0xb0fffffe;
        map.regs[1] = 0x00712345;
        map.regs[10] = 0xff151418;
        assert(!t7_frddr_dai_hw_params(NULL, &p, &dai));
        assert(map.regs[0] == 0xb0ffffff);
        assert(map.regs[1] == 0x3f712345);
        assert(map.regs[10] == ((channels - 1) << 24 | 0x151418));
    }
    p.period = 8;
    assert(!t7_frddr_dai_hw_params(NULL, &p, &dai));
    assert(map.regs[1] == 0x00712345);
    map.fail_reg = FIFO_CTRL1;
    map.regs[10] = 0xabcdef;
    assert(t7_frddr_dai_hw_params(NULL, &p, &dai) == -EIO);
    assert(map.regs[10] == 0xabcdef);
    map.fail_reg = FIFO_CTRL2;
    map.regs[0] = 0;
    assert(t7_frddr_dai_hw_params(NULL, &p, &dai) == -EIO);
    assert(!map.regs[0]);
    map.fail_reg = FIFO_CTRL0;
    assert(t7_frddr_dai_hw_params(NULL, &p, &dai) == -EIO);
    puts("FRDDR channel count, route preservation, depth and write failures passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'frddr.c'
    binary = Path(directory) / 'frddr'
    code.write_text(prefix + defines + '\n' + common + function + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
