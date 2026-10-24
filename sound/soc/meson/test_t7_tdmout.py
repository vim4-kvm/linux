#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile real formatter code; check window isolation and reset recovery."""
from pathlib import Path
import re
import subprocess
import tempfile

directory = Path(__file__).parent
source = (directory / 'axg-tdmout.c').read_text()
defines = '\n'.join(re.findall(r'^#define.*$', source, re.M))
start = source.index('static void axg_tdmout_enable')
output = source[start:source.index('static const struct snd_kcontrol_new', start)]
source = (directory / 'axg-tdm-formatter.c').read_text()
start = source.index('static void __iomem *axg_tdm_register')
windows = source[start:source.index('static const struct regmap_bus', start)]
start = source.index('int axg_tdm_formatter_set_channel_masks')
masks = source[start:source.index('EXPORT_SYMBOL_GPL', start)]
start = source.index('static int axg_tdm_formatter_enable')
enable = source[start:source.index('static void axg_tdm_formatter_disable', start)]
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
#define SND_SOC_DAIFMT_FORMAT_MASK 15
#define SND_SOC_DAIFMT_I2S 1
#define SND_SOC_DAIFMT_LEFT_J 2
#define SND_SOC_DAIFMT_DSP_A 4
#define SND_SOC_DAIFMT_DSP_B 5
#define pr_err(...) ((void)0)
#define WARN_ON(x) (x)
#define __iomem
struct regmap { u32 regs[32]; bool dirty; int fail; };
struct axg_tdm_iface { unsigned int fmt, slots, slot_width; };
struct axg_tdm_stream {
    struct axg_tdm_iface *iface;
    unsigned int channels, width, physical_width;
    u32 *mask;
};
struct axg_tdm_formatter_hw { unsigned int skew_offset; bool extended; };
struct ops {
    int (*prepare)(struct regmap *, const struct axg_tdm_formatter_hw *,
                   struct axg_tdm_stream *);
    void (*enable)(struct regmap *);
};
struct driver { struct ops *ops; struct axg_tdm_formatter_hw *quirks; };
struct axg_tdm_formatter {
    struct axg_tdm_stream *stream;
    struct driver *drv;
    struct regmap *map;
    int *reset, *sclk, *lrclk;
    bool enabled;
};
struct axg_tdm_registers { void *core, *ext; };
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 v, void *p) { *(u32 *)p = v; }
static int regmap_update_bits(struct regmap *m, unsigned int r, u32 mask, u32 v) {
    assert(r < sizeof(m->regs) && !(r % 4));
    m->regs[r / 4] = (m->regs[r / 4] & ~mask) | (v & mask); return 0;
}
static int regmap_write(struct regmap *m, unsigned int r, u32 v) {
    assert(r < sizeof(m->regs) && !(r % 4)); m->regs[r / 4] = v; return 0;
}
static unsigned int regmap_get_reg_stride(struct regmap *m) { return 4; }
static bool axg_tdm_lrclk_invert(unsigned int f) { return f == SND_SOC_DAIFMT_I2S; }
static bool axg_tdm_sclk_invert(unsigned int f) { return false; }
static int reset_control_reset(int *reset) { ++*reset; return 0; }
static void regcache_mark_dirty(struct regmap *m) { m->dirty = true; }
static int regcache_sync(struct regmap *m) {
    assert(m->dirty); return m->fail;
}
static int clk_set_phase(int *clock, int phase) { return 0; }
static int clk_prepare_enable(int *clock) { ++*clock; return 0; }
static void clk_disable_unprepare(int *clock) { --*clock; }
'''
checks = r'''
int main(void) {
	struct regmap mute_map = {0};
	t7_tdmout_enable(&mute_map);
	assert(mute_map.regs[0] & BIT(31));
	t7_tdmout_disable(&mute_map);
	assert(!(mute_map.regs[0] & BIT(31)));
	for (unsigned int i = 0; i < 4; i++)
		assert(mute_map.regs[(TDMOUT_MUTE0 / 4) + i] == ~0U);
	t7_tdmout_enable(&mute_map);
	for (unsigned int i = 0; i < 4; i++)
		assert(!mute_map.regs[(TDMOUT_MUTE0 / 4) + i]);
    u32 core[16] = {0}, ext[14] = {0}, value;
    struct axg_tdm_registers banks = {core, ext};
    assert(!axg_tdm_reg_write(&banks, 0x3c, 0x1234));
    assert(!axg_tdm_reg_write(&banks, 0x40, 0x5678));
    assert(!axg_tdm_reg_write(&banks, 0x74, 0xabcd));
    assert(core[15] == 0x1234 && ext[0] == 0x5678 && ext[13] == 0xabcd);
    assert(!axg_tdm_reg_read(&banks, 0x74, &value) && value == 0xabcd);
    struct regmap map = {0};
    struct axg_tdm_iface iface = {SND_SOC_DAIFMT_I2S, 2, 32};
    u32 mask[4] = {3, 3, 3, 3};
    struct axg_tdm_stream stream = {&iface, 8, 24, 32, mask};
    struct axg_tdm_formatter_hw hw = {2, true};
    struct ops ops = {axg_tdmout_prepare, axg_tdmout_enable};
    struct driver driver = {&ops, &hw};
    int reset = 0, sclk = 0, lrclk = 0;
    struct axg_tdm_formatter fmt = {&stream, &driver, &map, &reset, &sclk, &lrclk};
    map.fail = -EIO;
    assert(axg_tdm_formatter_enable(&fmt) == -EIO);
    assert(reset == 1 && sclk == 0 && lrclk == 0 && !fmt.enabled);
    assert(!map.regs[0]);
    map.fail = 0;
    map.regs[TDMOUT_CTRL1 / 4] = 4U << 24 | BIT(7);
    assert(!axg_tdm_formatter_enable(&fmt));
    assert(fmt.enabled && reset == 2 && sclk == 1 && lrclk == 1);
    assert((map.regs[TDMOUT_CTRL1 / 4] & (7U << 24 | BIT(7))) == (4U << 24 | BIT(7)));
    assert(map.regs[TDMOUT_MASK3 / 4] == 3);
    assert(!axg_tdm_formatter_enable(&fmt) && reset == 2);
    axg_tdmout_disable(&map);
    assert(!(map.regs[0] & BIT(31)));
    for (unsigned int channels = 1; channels <= 8; channels++) {
        stream.channels = channels;
        assert(!axg_tdmout_prepare(&map, &hw, &stream));
        unsigned int total = 0;
        for (unsigned int lane = 0; lane < 4; lane++)
            total += __builtin_popcount(map.regs[TDMOUT_MASK0 / 4 + lane]);
        assert(total == channels);
    }
    iface.fmt = 0;
    u32 before = map.regs[0];
    assert(axg_tdmout_prepare(&map, &hw, &stream) == -EINVAL);
    assert(map.regs[0] == before);
    puts("T7 output windows, four-lane masks and reset/sync failure passed");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    code = Path(temporary) / 'tdmout.c'
    binary = Path(temporary) / 'tdmout'
    code.write_text(prefix + defines + '\n' + windows + masks + output + enable + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
