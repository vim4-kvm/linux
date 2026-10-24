#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real ASRC sequence, coefficient ports and failure boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile

directory = Path(__file__).parent
source = (directory / 't7-asrc.c').read_text()
defines = '\n'.join(re.findall(r'^#define.*$', source, re.M))
start = source.index('static int t7_asrc_coefficients')
functions = source[start:source.index('static int t7_asrc_event', start)]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define FIELD_PREP(mask,v) (((v) << __builtin_ctz(mask)) & (mask))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define U32_MAX UINT32_MAX
#define div_u64(a,b) ((a) / (b))
struct regmap { u32 regs[80]; unsigned int writes, fail, sinc, aa; bool stuck; };
struct reg_sequence { unsigned int reg, def; };
static int regmap_write(struct regmap *m, unsigned int r, u32 v) {
    assert(r < sizeof(m->regs) && !(r % 4));
    if (++m->writes == m->fail) return -EIO;
    m->regs[r / 4] = v;
    if (r == 0x104) ++m->sinc;
    if (r == 0xc4) ++m->aa;
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
#define regmap_read_poll_timeout(m,r,v,condition,delay,timeout) \
    ((v) = (m)->stuck ? 0 : 1U << 11, (condition) ? 0 : -ETIMEDOUT)
'''
checks = r'''
int main(void) {
    struct regmap map = {0};
    assert(!t7_asrc_start(&map, 8, 44100, 48000, 2, false));
    unsigned int count = map.writes;
    assert(map.sinc == 4128 && !map.aa);
    assert(map.regs[ASRC_PHASE_STEP / 4] == ((uint64_t)44100 << 28) / 48000);
    assert((map.regs[ASRC_CTRL1 / 4] & ASRC_INPUT_ENABLE));
    assert(!(map.regs[ASRC_CTRL1 / 4] & ASRC_BYPASS));
    assert(!t7_asrc_stop(&map));
    assert(!(map.regs[ASRC_CTRL1 / 4] & ASRC_INPUT_ENABLE));
    assert(!(map.regs[ASRC_CHSYNC / 4] & BIT(31)));
    for (unsigned int fail = 1; fail <= count; fail++) {
        map = (struct regmap){.fail = fail};
        assert(t7_asrc_start(&map, 8, 44100, 48000, 2, false) == -EIO);
        assert(map.writes == fail);
        assert(!(map.regs[ASRC_CTRL1 / 4] & ASRC_INPUT_ENABLE));
    }
    map = (struct regmap){.stuck = true};
    assert(t7_asrc_start(&map, 8, 48000, 48000, 2, false) == -ETIMEDOUT);
    assert(!map.sinc && !map.aa);
    map = (struct regmap){0};
    assert(t7_asrc_start(&map, 8, 48000, 16000, 2, true) == -EINVAL);
    assert(t7_asrc_start(&map, 6, 48000, 48000, 2, true) == -EINVAL);
    assert(t7_asrc_start(&map, 6, 48000, 16000, 0, true) == -EINVAL);
    assert(t7_asrc_start(&map, 13, 48000, 48000, 2, false) == -EINVAL);
    assert(!map.writes);
    assert(!t7_asrc_start(&map, 6, 48000, 16000, 2, true));
    assert(map.sinc == 4128 && map.aa == 32);
    assert(map.regs[ASRC_ADJ_CTRL1 / 4] == (384U << 16));
    assert(!t7_asrc_start(&map, 8, 192000, 8000, 8, false));
    assert(map.regs[ASRC_CTRL1 / 4] & BIT(10));
    assert(map.regs[ASRC_CTRL2 / 4] & BIT(16));
    assert(map.regs[ASRC_PHASE_STEP / 4] == 24U * (1U << 27));
    assert(((map.regs[ASRC_CTRL2 / 4] >> 24) & 63) == 8);
    puts("ASRC phase, AA, loopback restriction, timeout and every write failure passed");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    code = Path(temporary) / 'asrc.c'
    binary = Path(temporary) / 'asrc'
    code.write_text(prefix + (directory / 't7-asrc-coeff.h').read_text() +
                    '\n' + defines + '\n' + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
