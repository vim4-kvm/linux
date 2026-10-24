#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute real DAPM events, including power-down after failed power-up."""
from pathlib import Path
import re
import subprocess
import tempfile

prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef int atomic_t;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define guard(x) (void)
#define SND_SOC_DAPM_PRE_PMD 1
#define SND_SOC_DAPM_PRE_PMU 2
struct mutex { int unused; };
struct clk { unsigned int refs, exclusive; };
struct clk_bulk_data { const char *id; struct clk *clk; };
struct reset_control { int unused; };
struct regmap { bool cache_only, running; };
struct snd_kcontrol { int unused; };
struct snd_soc_component { void *data; };
struct snd_soc_dapm_widget { struct snd_soc_component *dapm; };
#define snd_soc_dapm_to_component(d) (d)
#define snd_soc_component_get_drvdata(c) ((c)->data)
static unsigned int calls, fail;
static int step(void) { return ++calls == fail ? -EIO : 0; }
static int clk_set_rate_exclusive(struct clk *c, unsigned long rate) {
    int ret = step(); if (!ret) c->exclusive++; return ret;
}
static void clk_rate_exclusive_put(struct clk *c) { assert(c->exclusive); c->exclusive--; }
static int clk_set_rate(struct clk *c, unsigned long rate) { return step(); }
static unsigned long clk_get_rate(struct clk *c) { return 12288000; }
static int clk_bulk_prepare_enable(unsigned int n, struct clk_bulk_data *c) {
    int ret = step(); if (ret) return ret;
    for (unsigned int i = 0; i < n; i++) c[i].clk->refs++;
    return 0;
}
static void clk_bulk_disable_unprepare(unsigned int n, struct clk_bulk_data *c) {
    for (unsigned int i = 0; i < n; i++) { assert(c[i].clk->refs); c[i].clk->refs--; }
}
static int clk_prepare_enable(struct clk *c) { int ret = step(); if (!ret) c->refs++; return ret; }
static void clk_disable_unprepare(struct clk *c) { assert(c->refs); c->refs--; }
static void regcache_cache_only(struct regmap *m, bool only) { m->cache_only = only; }
static void regcache_mark_dirty(struct regmap *m) { }
static int regcache_sync(struct regmap *m) { return step(); }
static int regmap_read(struct regmap *m, unsigned int reg, unsigned int *value) {
    *value = 8; return step();
}
static int reset_control_reset(struct reset_control *r) { return step(); }
'''

directory = Path(__file__).parent
for name, variable, setup in (
        ('asrc', 'asrc', 'state.rate = 48000; state.channels = 2;'),
        ('loopback', 'lb', 'state.configured = true; state.pclk = &clocks[0];'),
        ('aed', 'aed', 'state.width = 24;'),
        ('vad', 'vad', '')):
    source = (directory / f't7-{name}.c').read_text()
    structure = re.search(r'struct t7_' + name + r' \{.*?\n\};', source, re.S).group()
    defines = '\n'.join(re.findall(r'^#define.*$', source, re.M))
    start = source.index(f'static int t7_{name}_event')
    event = source[start:source.index('\nstatic ', start + 1)]
    stubs = f'''
static int t7_{name}_start(struct regmap *map, ...) {{
    int ret = step(); if (!ret) map->running = true; return ret;
}}
static int t7_{name}_stop(struct regmap *map) {{ map->running = false; return 0; }}
'''
    if name != 'loopback':
        setup += ' for (unsigned int i = 0; i < ARRAY_SIZE(state.clocks); i++) state.clocks[i].clk = &clocks[i];'
    checks = '''
int main(void) {
    unsigned int count = 0;
    for (unsigned int failure = 0; failure <= count; failure++) {
        struct regmap map = {.cache_only = true};
        struct clk clocks[3] = {0};
        struct t7_NAME state = {.map = &map};
        SETUP
        struct snd_soc_component component = {.data = &state};
        struct snd_soc_dapm_widget widget = {.dapm = &component};
        calls = 0; fail = failure;
        int ret = t7_NAME_event(&widget, NULL, SND_SOC_DAPM_PRE_PMU);
        if (!failure) count = calls;
        assert(ret == (failure ? -EIO : 0));
        assert(state.powered == !failure);
        assert(!t7_NAME_event(&widget, NULL, SND_SOC_DAPM_PRE_PMD));
        assert(!t7_NAME_event(&widget, NULL, SND_SOC_DAPM_PRE_PMD));
        assert(!state.powered && !map.running && map.cache_only);
        for (unsigned int i = 0; i < 3; i++) assert(!clocks[i].refs && !clocks[i].exclusive);
        fail = 0;
        assert(!t7_NAME_event(&widget, NULL, SND_SOC_DAPM_PRE_PMU));
        assert(state.powered && map.running && !map.cache_only);
        assert(!t7_NAME_event(&widget, NULL, SND_SOC_DAPM_PRE_PMD));
        for (unsigned int i = 0; i < 3; i++) assert(!clocks[i].refs && !clocks[i].exclusive);
    }
    puts("NAME: each power-up failure, repeated power-down and successful retry passed");
}
'''.replace('NAME', name).replace('SETUP', setup)
    with tempfile.TemporaryDirectory() as temporary:
        code = Path(temporary) / 'power.c'
        binary = Path(temporary) / 'power'
        code.write_text(prefix + defines + '\n' + structure + stubs + event + checks)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
