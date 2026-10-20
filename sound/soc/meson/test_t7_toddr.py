#!/usr/bin/env python3
"""T7 register ABI: source selection, PCM alignment and channel synchronization."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('axg-toddr.c').read_text()
functions = source[source.index('static int t7_toddr_dai_hw_params'):
                   source.index('static const struct snd_soc_dai_ops t7_toddr_ops')]
enum_start = source.index('static const struct soc_enum t7_toddr_sel_enum')
enum = source[enum_start:source.index('\n};', enum_start) + 3]
texts_start = source.index('static const char * const sm1_toddr_sel_texts')
texts = source[texts_start:source.index('\n};', texts_start) + 3]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define FIELD_PREP(mask,value) (((value) << __builtin_ctz(mask)) & (mask))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define FIFO_CTRL0 0
#define FIFO_CTRL1 4
#define CTRL0_TODDR_TYPE GENMASK(15,13)
#define CTRL0_TODDR_MSB_POS GENMASK(12,8)
#define CTRL0_TODDR_LSB_POS GENMASK(7,3)
#define SNDRV_PCM_FORMAT_S16_LE 2
struct regmap { u32 ctrl0, ctrl1; };
struct axg_fifo { struct regmap *map; u32 *chsync; };
struct snd_soc_dai { struct axg_fifo *fifo; };
struct snd_pcm_substream { int unused; };
struct snd_pcm_hw_params { int format; unsigned int channels; };
struct soc_enum { u32 reg, shift_l, shift_r, items; const char *const *texts; u32 mask; };
#define params_format(p) ((p)->format)
#define params_channels(p) ((p)->channels)
static bool fail_read, fail_write;
static unsigned int generic_calls, shutdowns;
static struct axg_fifo *snd_soc_dai_get_drvdata(struct snd_soc_dai *dai) { return dai->fifo; }
static int regmap_read(struct regmap *map, unsigned int reg, u32 *value) {
    assert(reg==FIFO_CTRL1); if (fail_read) return -EIO; *value=map->ctrl1; return 0;
}
static int regmap_update_bits(struct regmap *map, unsigned int reg, u32 mask, u32 value) {
    assert(reg==FIFO_CTRL0); if (fail_write) return -EIO;
    map->ctrl0=(map->ctrl0 & ~mask) | (value & mask); return 0;
}
static u32 readl(u32 *p) { return *p; }
static void writel(u32 value,u32 *p) { *p=value; }
static int axg_toddr_dai_hw_params(struct snd_pcm_substream *ss,struct snd_pcm_hw_params *p,struct snd_soc_dai *dai) {
    generic_calls++; return 0;
}
static void axg_toddr_dai_shutdown(struct snd_pcm_substream *ss,struct snd_soc_dai *dai) { shutdowns++; }
'''
checks = r'''
int main(void) {
    struct regmap map={.ctrl0=0xa8ffabcd, .ctrl1=0x80001234};
    u32 sync=0x123400ab;
    struct axg_fifo fifo={.map=&map,.chsync=&sync};
    struct snd_soc_dai dai={.fifo=&fifo};
    struct snd_pcm_hw_params params={.format=2,.channels=2};
    u32 mask=t7_toddr_sel_enum.mask << t7_toddr_sel_enum.shift_l;
    map.ctrl1=(map.ctrl1&~mask) | (8U << t7_toddr_sel_enum.shift_l);
    assert((map.ctrl1 & 0x7c000000)==0x20000000);
    assert((map.ctrl1 & ~0x7c000000)==0x80001234);
    u32 original=map.ctrl0;
    fail_read=true;
    assert(t7_toddr_dai_hw_params(NULL,&params,&dai)==-EIO && sync==0x123400ab);
    fail_read=false;fail_write=true;
    assert(t7_toddr_dai_hw_params(NULL,&params,&dai)==-EIO && sync==0x123400ab);
    fail_write=false;params.format=10;
    assert(t7_toddr_dai_hw_params(NULL,&params,&dai)==-EINVAL && sync==0x123400ab);
    params.format=2;
    assert(!t7_toddr_dai_hw_params(NULL,&params,&dai));
    u32 fields=CTRL0_TODDR_TYPE|CTRL0_TODDR_MSB_POS|CTRL0_TODDR_LSB_POS;
    assert((map.ctrl0&fields)==((27U<<8)|(12U<<3)));
    assert((map.ctrl0&~fields)==(original&~fields));
    assert(sync==0x92340001 && !generic_calls);
    t7_toddr_dai_shutdown(NULL,&dai);
    assert(sync==0x12340001 && shutdowns==1);
    map.ctrl1=0;params.channels=8;
    assert(!t7_toddr_dai_hw_params(NULL,&params,&dai) && generic_calls==1);
    assert((sync&0x800000ff)==0x80000007);
    puts("T7 selector isolation, S16 cropping and channel sync passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'toddr.c'
    binary = Path(directory) / 'toddr'
    code.write_text(prefix + texts + enum + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
