#!/usr/bin/env python3
"""T7 register ABI: source selection, PCM alignment and channel synchronization."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).with_name('axg-toddr.c').read_text()
header = Path(__file__).with_name('axg-fifo.h').read_text()
formats_define = '#define AXG_FIFO_FORMATS' + header.split(
    '#define AXG_FIFO_FORMATS', 1)[1].split('\n\n', 1)[0] + '\n'
dai = source.split('static struct snd_soc_dai_driver t7_toddr_dai_drv', 1)[1]
formats = re.search(r'\.formats\s*=\s*([^,]+),', dai).group(1)
functions = source[source.index('static int t7_toddr_pcm_format'):
                   source.index('static const struct snd_soc_dai_ops t7_toddr_ops')]
enum_start = source.index('static const struct soc_enum t7_toddr_sel_enum')
enum = source[enum_start:source.index('\n};', enum_start) + 3]
texts_start = source.index('static const char * const t7_toddr_sel_texts')
texts = source[texts_start:enum_start]
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
#define SNDRV_PCM_FORMAT_S8 0
#define SNDRV_PCM_FORMAT_S20_LE 25
#define SNDRV_PCM_FORMAT_S24_LE 6
#define SNDRV_PCM_FORMAT_S32_LE 10
#define SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE 18
#define SNDRV_PCM_FMTBIT_S8 (1ULL << SNDRV_PCM_FORMAT_S8)
#define SNDRV_PCM_FMTBIT_S16_LE (1ULL << SNDRV_PCM_FORMAT_S16_LE)
#define SNDRV_PCM_FMTBIT_S20_LE (1ULL << SNDRV_PCM_FORMAT_S20_LE)
#define SNDRV_PCM_FMTBIT_S24_LE (1ULL << SNDRV_PCM_FORMAT_S24_LE)
#define SNDRV_PCM_FMTBIT_S32_LE (1ULL << SNDRV_PCM_FORMAT_S32_LE)
#define SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE (1ULL << SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE)
struct regmap { u32 ctrl0, ctrl1; };
struct axg_fifo { struct regmap *map; u32 *chsync; };
struct snd_soc_dai { struct axg_fifo *fifo; };
struct snd_pcm_substream { int unused; };
struct snd_pcm_hw_params { int format; unsigned int channels, width; };
struct soc_enum {
    u32 reg, shift_l, shift_r, items;
    const char *const *texts;
    u32 mask;
    const unsigned int *values;
};
#define params_format(p) ((p)->format)
#define params_channels(p) ((p)->channels)
#define params_width(p) ((p)->width)
static bool fail_read, fail_write;
static unsigned int shutdowns;
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
static void axg_toddr_dai_shutdown(struct snd_pcm_substream *ss,struct snd_soc_dai *dai) { shutdowns++; }
'''
checks = r'''
int main(void) {
    struct regmap map={.ctrl0=0xa8ffabcd, .ctrl1=0x80001234};
    u32 sync=0x123400ab;
    struct axg_fifo fifo={.map=&map,.chsync=&sync};
    struct snd_soc_dai dai={.fifo=&fifo};
    struct snd_pcm_hw_params params={.format=2,.channels=2,.width=16};
    u32 mask=t7_toddr_sel_enum.mask << t7_toddr_sel_enum.shift_l;
    map.ctrl1=(map.ctrl1&~mask) | (8U << t7_toddr_sel_enum.shift_l);
    assert((map.ctrl1 & 0x7c000000)==0x20000000);
    assert((map.ctrl1 & ~0x7c000000)==0x80001234);
    for (unsigned int i = 0; i < t7_toddr_sel_enum.items; i++) {
        unsigned int value = t7_toddr_sel_enum.values[i];
        map.ctrl1 = (map.ctrl1 & ~mask) | value << t7_toddr_sel_enum.shift_l;
        assert(((map.ctrl1 >> 26) & 31) == value);
        assert((map.ctrl1 & ~mask) == 0x80001234);
    }
    assert(t7_toddr_sel_enum.values[t7_toddr_sel_enum.items - 1] == 18);
    map.ctrl1 = (map.ctrl1 & ~mask) | 8U << t7_toddr_sel_enum.shift_l;
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
    assert(sync==0x92340001);
    t7_toddr_dai_shutdown(NULL,&dai);
    assert(sync==0x12340001 && shutdowns==1);
    map.ctrl1=0;params.channels=8;
    assert(!t7_toddr_dai_hw_params(NULL,&params,&dai));
    assert((sync&0x800000ff)==0x80000007);
    assert((map.ctrl0 & fields) == ((31U<<8)|(16U<<3)));
    params.format=SNDRV_PCM_FORMAT_S24_LE;params.width=24;
    assert(!t7_toddr_dai_hw_params(NULL,&params,&dai));
    assert((map.ctrl0 & fields) == ((4U<<13)|(31U<<8)|(8U<<3)));
    params.format=SNDRV_PCM_FORMAT_S32_LE;params.width=32;
    assert(!t7_toddr_dai_hw_params(NULL,&params,&dai));
    assert((map.ctrl0 & fields) == (31U<<8));
    map.ctrl1=3U<<26;
    assert(t7_toddr_dai_hw_params(NULL,&params,&dai)==-EINVAL);
    params.format=SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE;
    assert(!t7_toddr_dai_hw_params(NULL,&params,&dai));
    assert((map.ctrl0 & fields) == ((3U<<13)|(27U<<8)|(4U<<3)));
    for (int format = 0; format < 64; format++) {
        params.format = format;
        params.width = format == SNDRV_PCM_FORMAT_S8 ? 8 :
                       format == SNDRV_PCM_FORMAT_S16_LE ? 16 :
                       format == SNDRV_PCM_FORMAT_S24_LE ? 24 : 32;
        map.ctrl1 = format == SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE ? 3U << 26 : 0;
        original = map.ctrl0;
        u32 original_sync = sync;
        int ret = t7_toddr_dai_hw_params(NULL, &params, &dai);
        if (advertised_formats & (1ULL << format)) {
            assert(!ret);
        } else {
            assert(ret == -EINVAL);
            assert(map.ctrl0 == original && sync == original_sync);
        }
    }
    puts("T7 selector isolation, S16 cropping and channel sync passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'toddr.c'
    binary = Path(directory) / 'toddr'
    code.write_text(prefix + formats_define +
                    f'static const uint64_t advertised_formats = {formats};\n' +
                    texts + enum + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
