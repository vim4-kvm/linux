#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check real combiner code at channel boundaries and every write failure."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).with_name('t7-loopback.c').read_text()
defines = '\n'.join(re.findall(r'^#define.*$', source, re.M))
start = source.index('static int t7_loopback_start')
functions = source[start:source.index('static int t7_loopback_event', start)]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define FIELD_GET(mask,v) (((v) & (mask)) >> __builtin_ctz(mask))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct regmap { u32 regs[15]; unsigned int writes, fail; };
struct reg_sequence { unsigned int reg, def; };
static int regmap_write(struct regmap *m, unsigned int r, u32 v) {
    assert(r < sizeof(m->regs) && !(r % 4));
    if (++m->writes == m->fail) return -EIO;
    m->regs[r / 4] = v; return 0;
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
    u32 pdm = 4U << 20 | 3U << 16;
    u32 asrc = 14U << 20 | 1U << 16;
    assert(!t7_loopback_start(&map, pdm, asrc));
    assert(map.regs[LB_CTRL0 / 4] == (BIT(31) | 31U << 8));
    assert(map.regs[LB_CTRL2 / 4] == (pdm | 15));
    assert(map.regs[LB_CTRL3 / 4] == (asrc | 3));
    assert((map.regs[LB_ORIGINAL_SYNC / 4] & 255) == 3);
    assert((map.regs[LB_INSERT_SYNC / 4] & 255) == 1);
    unsigned int count = map.writes;
    for (unsigned int fail = 1; fail <= count; fail++) {
        map = (struct regmap){.fail = fail};
        assert(t7_loopback_start(&map, pdm, asrc) == -EIO);
        assert(map.writes == fail && !(map.regs[LB_CTRL0 / 4] & BIT(31)));
    }
    for (unsigned int data = 1; data <= 16; data++) {
        for (unsigned int ref = 1; ref <= 16; ref++) {
            map = (struct regmap){0};
            assert(!t7_loopback_start(&map, 4U << 20 | (data - 1) << 16,
                                     6U << 20 | (ref - 1) << 16));
            assert((map.regs[LB_CTRL2 / 4] & 65535) == ((1U << data) - 1));
            assert((map.regs[LB_CTRL3 / 4] & 65535) == ((1U << ref) - 1));
            assert(!t7_loopback_stop(&map));
            assert(!(map.regs[LB_CTRL0 / 4] & BIT(31)));
            assert(!(map.regs[LB_ORIGINAL_SYNC / 4] & BIT(31)));
            assert(!(map.regs[LB_INSERT_SYNC / 4] & BIT(31)));
        }
    }
    map = (struct regmap){0};
    assert(t7_loopback_start(&map, 7U << 20, asrc) == -EINVAL);
    assert(t7_loopback_start(&map, pdm, 7U << 20) == -EINVAL);
    assert(!map.writes);
    puts("Loopback PDM/reference packing, channel boundaries and fault cleanup passed");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    code = Path(temporary) / 'loopback.c'
    binary = Path(temporary) / 'loopback'
    code.write_text(prefix + defines + '\n' + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
