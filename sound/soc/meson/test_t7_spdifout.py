#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the SPDIF formatter against old and T7 register layouts."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).with_name('axg-spdifout.c').read_text()
defines = '\n'.join(re.findall(r'^#define.*$', source, re.M))
start = source.index('static int axg_spdifout_sample_fmt')
function = source[start:source.index('\nstatic int ', start + 1)]
start = source.index('struct axg_spdifout_match_data {')
data_type = source[start:source.index('\n};', start) + 3]
start = source.index('static const struct axg_spdifout_match_data axg_spdifout_data')
data = source[start:source.index('static const struct of_device_id', start)]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
struct regmap { unsigned int regs[32]; };
struct snd_pcm_hw_params { unsigned int channels, width, physical_width; };
#define params_channels(p) ((p)->channels)
#define params_width(p) ((p)->width)
#define params_physical_width(p) ((p)->physical_width)
#define dev_err(...) ((void)0)
static int regmap_update_bits(struct regmap *map, unsigned int reg,
                              unsigned int mask, unsigned int val) {
    assert(reg < sizeof(map->regs) && !(reg % 4));
    map->regs[reg / 4] = (map->regs[reg / 4] & ~mask) | (val & mask);
    return 0;
}
'''
types = r'''
struct axg_spdifout {
    const struct axg_spdifout_match_data *data;
    struct regmap *map;
};
struct snd_soc_dai { struct axg_spdifout *priv; };
#define snd_soc_dai_get_drvdata(dai) ((dai)->priv)
'''
checks = r'''
int main(void) {
    const struct axg_spdifout_match_data *layouts[] = {
        &axg_spdifout_data, &t7_spdifout_data,
    };
    for (unsigned int i = 0; i < 2; i++) {
        struct regmap map = {0};
        struct axg_spdifout priv = {.data = layouts[i], .map = &map};
        struct snd_soc_dai dai = {.priv = &priv};
        struct snd_pcm_hw_params p = {.channels = 2, .width = 16, .physical_width = 16};
        map.regs[3] = 0xe5a5ffff;
        map.regs[4] = 0x0300000f;
        assert(!axg_spdifout_sample_fmt(&p, &dai));
        unsigned int expected = i ? 0xe5a50003 : 0xe5a5f03f;
        assert(map.regs[3] == expected);
        assert(map.regs[4] == 0x03000f2f);
        p.channels = 1; p.width = 24; p.physical_width = 32;
        assert(!axg_spdifout_sample_fmt(&p, &dai));
        expected = i ? 0xe5a50001 : 0xe5a5f01f;
        assert(map.regs[3] == expected && map.regs[4] == 0x0300174f);
        p.channels = 3;
        assert(axg_spdifout_sample_fmt(&p, &dai) == -EINVAL);
        assert(map.regs[3] == expected);
    }
    puts("SPDIF lane clearing, mono/stereo and PCM packing passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'spdifout.c'
    binary = Path(directory) / 'spdifout'
    code.write_text(prefix + defines + '\n' + data_type + types + data + function + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
