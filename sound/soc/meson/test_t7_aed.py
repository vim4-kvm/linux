#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the real AED SRAM, biquad validation and enable/failure sequence."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).with_name('t7-aed.c').read_text()
defines = '\n'.join(re.findall(r'^#define.*$', source, re.M))
start = source.index('static bool t7_aed_biquad_valid')
functions = source[start:source.index('static int t7_aed_event', start)]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef int32_t s32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static s32 sign_extend32(u32 v, unsigned int b) { return (s32)(v << (31-b)) >> (31-b); }
struct regmap { u32 regs[256]; unsigned int writes, fail, ram_a, ram_b; };
struct reg_sequence { unsigned int reg, def; };
static int regmap_write(struct regmap *m, unsigned int r, u32 v) {
    assert(r < sizeof(m->regs) && !(r % 4));
    if (++m->writes == m->fail) return -EIO;
    m->regs[r / 4] = v;
    if (r == 0) { assert(v == (m->ram_a << 2 | 3)); ++m->ram_a; }
    if (r == 0x244) { assert(v == (m->ram_b << 2 | 3)); ++m->ram_b; }
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
    u32 eq[AED_EQ_WORDS] = {0};
    u32 drc[18];
    for (unsigned int i = 0; i < 18; i++) drc[i] = fullband_drc_coeff[i];
    drc[15] = drc[10];
    for (unsigned int i = 0; i < AED_EQ_WORDS; i += 5) eq[i] = AED_Q23_ONE;
    struct regmap map = {0};
    map.regs[AED_TOP_CTRL0 / 4] = 3 << 3;
    assert(!t7_aed_start(&map, 24, eq, drc, multiband_drc_coeff));
    unsigned int count = map.writes;
    assert(map.ram_a == 225 && map.ram_b == 225);
    assert(map.regs[0x54 / 4] == drc[10]);
    assert(map.regs[0x40 / 4] == 0x40000);
    assert(map.regs[0xd0 / 4] == multiband_drc_coeff[0]);
    assert((map.regs[AED_TOP_CTRL0 / 4] & BIT(31)));
    assert((map.regs[AED_TOP_CTRL0 / 4] & (3 << 3)) == (3 << 3));
    assert((map.regs[AED_TOP_CTRL2 / 4] & 15) == 9);
    assert(map.regs[AED_TOP_CTRL1 / 4] == (1 << 20 | 3 << 4 | 1));
    for (unsigned int fail = 1; fail <= count; fail++) {
        map = (struct regmap){.fail = fail};
        assert(t7_aed_start(&map, 24, eq, drc, multiband_drc_coeff) == -EIO);
        assert(!t7_aed_stop(&map));
        assert(!(map.regs[AED_TOP_CTRL0 / 4] & (BIT(31) | 7)));
        assert(!(map.regs[AED_TOP_CTRL2 / 4] & BIT(3)));
    }
    map = (struct regmap){0};
    assert(t7_aed_start(&map, 8, eq, drc, multiband_drc_coeff) == -EINVAL && !map.writes);
    eq[4] = AED_Q23_ONE;
    assert(t7_aed_start(&map, 24, eq, drc, multiband_drc_coeff) == -EINVAL && !map.writes);
    eq[4] = 0; eq[3] = AED_Q23_ONE;
    assert(!t7_aed_biquad_valid(eq));
    eq[3] = 0; eq[0] |= BIT(26);
    assert(!t7_aed_biquad_valid(eq));
    assert(t7_aed_drc_valid(drc, 18));
    drc[0]++;
    assert(!t7_aed_drc_valid(drc, 18));
    puts("AED dual SRAM, stable biquads, FIFO source isolation and every failure passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'aed.c'
    binary = Path(directory) / 'aed'
    code.write_text(prefix + Path(__file__).with_name('t7-aed-coeff.h').read_text() +
                    '\n' + defines + '\n' + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
