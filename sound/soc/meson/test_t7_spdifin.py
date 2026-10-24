#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run SPDIF input clock, timer and failure-unwind code without hardware."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).with_name('axg-spdifin.c').read_text()
defines = '\n'.join(re.findall(r'^#define.*$', source[:source.index('struct axg_spdifin_cfg')], re.M))
types = source[source.index('struct axg_spdifin_cfg'):source.index('/*\n * TODO:')]
start = source.index('static void axg_spdifin_write_mode_param')
functions = source[start:source.index('static int axg_spdifin_dai_remove', start)]
start = source.index('static const unsigned int axg_spdifin_mode_rates')
data = source[start:source.index('static const struct of_device_id', start)]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define FIELD_PREP(mask,value) (((value) << __builtin_ctz(mask)) & (mask))
#define dev_err(...) ((void)0)
#define do_div(n,d) ({ unsigned int rem = (n) % (d); (n) /= (d); rem; })
struct regmap { unsigned int regs[16]; bool fail_filter; };
struct clk { unsigned long rate; unsigned int enabled; bool fail_rate, fail_enable; };
static int clk_set_rate(struct clk *c, unsigned long rate) {
    if (c->fail_rate) return -EINVAL; c->rate = rate; return 0;
}
static unsigned long clk_get_rate(struct clk *c) { return c->rate; }
static int clk_prepare_enable(struct clk *c) {
    if (c->fail_enable) return -EIO; c->enabled++; return 0;
}
static void clk_disable_unprepare(struct clk *c) { assert(c->enabled); c->enabled--; }
static int regmap_update_bits(struct regmap *m, unsigned int r,
                              unsigned int mask, unsigned int value) {
    if (m->fail_filter && r == 0x18) return -EIO;
    assert(r < sizeof(m->regs) && !(r % 4));
    m->regs[r / 4] = (m->regs[r / 4] & ~mask) | (value & mask); return 0;
}
static unsigned int regmap_get_reg_stride(struct regmap *m) { return 4; }
'''
dai = r'''
struct snd_soc_dai { struct axg_spdifin *priv; };
#define snd_soc_dai_get_drvdata(d) ((d)->priv)
'''
checks = r'''
int main(void) {
    const struct axg_spdifin_cfg *layouts[] = {&axg_cfg, &t7_cfg};
    for (unsigned int i = 0; i < 2; i++) {
        struct clk pclk = {0}, refclk = {0};
        struct regmap map = {0};
        struct axg_spdifin priv = {.conf = layouts[i], .map = &map,
                                  .refclk = &refclk, .pclk = &pclk};
        struct snd_soc_dai dai = {.priv = &priv};
        pclk.fail_enable = true;
        assert(axg_spdifin_dai_probe(&dai) == -EIO && !pclk.enabled);
        pclk.fail_enable = false; refclk.fail_rate = true;
        assert(axg_spdifin_dai_probe(&dai) == -EINVAL && !pclk.enabled);
        refclk.fail_rate = false; refclk.fail_enable = true;
        assert(axg_spdifin_dai_probe(&dai) == -EIO && !pclk.enabled);
        refclk.fail_enable = false;
        if (i) {
            map.fail_filter = true;
            assert(axg_spdifin_dai_probe(&dai) == -EIO);
            assert(!pclk.enabled && !refclk.enabled);
            map.fail_filter = false;
        }
        map.regs[6] = 0x004000aa;
        assert(!axg_spdifin_dai_probe(&dai));
        assert(pclk.enabled == 1 && refclk.enabled == 1);
        assert(map.regs[6] == (i ? 0x004010aa : 0x004000aa));
        assert((map.regs[1] & 0xfffff) == refclk.rate / 1000);
        unsigned int previous = 256;
        for (unsigned int mode = 0; mode < 7; mode++) {
            unsigned int timer = axg_spdifin_mode_timer(&priv, mode, refclk.rate);
            assert(timer && timer < previous);
            unsigned int reg = 4 + mode / 4;
            unsigned int shift = 8 * (3 - mode % 4);
            assert(((map.regs[reg] >> shift) & 0xff) == timer);
            previous = timer;
        }
    }
    puts("SPDIF seven-rate timers, filter isolation and clock unwind passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'spdifin.c'
    binary = Path(directory) / 'spdifin'
    code.write_text(prefix + defines + '\n' + types + dai + data + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-misleading-indentation',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
