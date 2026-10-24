#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute detector setup and verify cleanup at every register failure."""
from pathlib import Path
import re
import subprocess
import tempfile

directory = Path(__file__).parent
source = (directory / 't7-vad.c').read_text()
defines = '\n'.join(re.findall(r'^#define.*$', source, re.M))
start = source.index('static int t7_vad_stop')
functions = source[start:source.index('static int t7_vad_event', start)]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct regmap { u32 regs[64]; unsigned int writes, fail, lut; };
struct reg_sequence { unsigned int reg, def; };
static int regmap_write(struct regmap *m, unsigned int r, u32 v) {
    assert(r < sizeof(m->regs) && !(r % 4));
    if (++m->writes == m->fail) return -EIO;
    m->regs[r / 4] = v;
    if (r == 0xb0) { assert(v == (BIT(31) | m->lut)); ++m->lut; }
    return 0;
}
static int regmap_update_bits(struct regmap *m, unsigned int r, u32 mask, u32 v) {
    return regmap_write(m, r, (m->regs[r / 4] & ~mask) | (v & mask));
}
static int regmap_multi_reg_write(struct regmap *m, const struct reg_sequence *s, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        int ret = regmap_write(m, s[i].reg, s[i].def);
        if (ret) return ret;
    }
    return 0;
}
'''
checks = r'''
int main(void) {
    struct regmap map = {0};
    map.regs[VAD_FEED / 4] = BIT(5);
    assert(!t7_vad_start(&map));
    unsigned int count = map.writes;
    assert(map.lut == ARRAY_SIZE(vad_ram_coeff));
    assert(map.regs[3] == vad_de_coeff_16k[0]);
    assert(map.regs[VAD_FEED / 4] == (BIT(31) | BIT(30) | (4 << 12) | BIT(5)));
    assert(map.regs[VAD_TOP0 / 4] == (GENMASK(31,27) | BIT(23) | BIT(22)));
    for (unsigned int fail = 1; fail <= count; fail++) {
        map = (struct regmap){.fail = fail};
        assert(t7_vad_start(&map) == -EIO);
        assert(!t7_vad_stop(&map));
        assert(!map.regs[VAD_TOP0 / 4] && !map.regs[VAD_TOP1 / 4]);
        assert(!(map.regs[VAD_FEED / 4] & BIT(31)));
    }
    puts("VAD FIR/LUT, PDM feed, reset sequencing and every failure cleanup passed");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    code = Path(temporary) / 'vad.c'
    binary = Path(temporary) / 'vad'
    code.write_text(prefix + (directory / 't7-vad-coeff.h').read_text() +
                    '\n' + defines + '\n' + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
