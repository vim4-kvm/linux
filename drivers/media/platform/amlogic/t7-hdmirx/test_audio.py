#!/usr/bin/env python3
"""Actual audio callbacks: reject unsupported sources and unwind clock/PLL failures."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-hdmirx.c').read_text()
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define T7_RX_'))
types = source[source.index('static const u32 t7_hdmirx_phy_top_regs'):
               source.index('static int t7_hdmirx_validate_edid')]
functions = source[source.index('static u32 t7_hdmirx_audio_word'):
                   source.index('static const struct snd_soc_dai_ops t7_hdmirx_audio_ops')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
struct mutex { int unused; };
struct delayed_work { int unused; };
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define div_u64(value, divisor) ((value) / (divisor))
#define SNDRV_PCM_FORMAT_S16_LE 2
struct clk_bulk_data { const char *id; void *clk; };
struct debugfs_blob_wrapper { void *data; unsigned long size; };
struct device { void *data; };
struct snd_soc_dai { struct device *dev; };
struct snd_pcm_substream { int unused; };
struct snd_pcm_hw_params { unsigned int rate, channels; int format; };
#define params_rate(p) ((p)->rate)
#define params_channels(p) ((p)->channels)
#define params_format(p) ((p)->format)
'''
stubs = r'''
static u8 core[0x8000], top[0x4000], phy[0x90], fifo[0x30];
static bool clocks, fail_clocks, pll_locked;
static unsigned int writes, enables, disables;
static u8 readb(void *p) { return *(u8 *)p; }
static void writeb(u8 v, void *p) { *(u8 *)p = v; writes++; }
static u32 readl(void *p) { u32 v; memcpy(&v, p, 4); return v; }
static void writel(u32 v, void *p) {
    assert(clocks);
    assert(((u8 *)p >= top && (u8 *)p + 4 <= top + sizeof(top)) ||
           ((u8 *)p >= phy && (u8 *)p + 4 <= phy + sizeof(phy)) ||
           ((u8 *)p >= fifo && (u8 *)p + 4 <= fifo + sizeof(fifo)));
    memcpy(p, &v, 4); writes++;
}
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static unsigned long clk_get_rate(void *clk) { return 24000000; }
static u32 t7_hdmirx_meter_hz(u32 status, unsigned long reference) { return 594000000; }
static int clk_bulk_prepare_enable(unsigned int count, struct clk_bulk_data *data) {
    assert(count == 3 && !clocks); enables++;
    if (fail_clocks) return -EIO;
    clocks = true; return 0;
}
static void clk_bulk_disable_unprepare(unsigned int count, struct clk_bulk_data *data) {
    assert(clocks && count == 3); clocks = false; disables++;
}
#define readl_poll_timeout(address, value, condition, delay, timeout) \
    ({ (value) = pll_locked ? BIT(31) : 0; (condition) ? 0 : -ETIMEDOUT; })
static void udelay(unsigned int us) { assert(us == 1); }
static void acr(unsigned int offset, unsigned int value) {
    core[offset] = value >> 16; core[offset+1] = value >> 8; core[offset+2] = value;
}
'''
checks = r'''
int main(void) {
    struct t7_hdmirx rx = { .top=top, .phy=phy, .cor=core, .frhdmirx=fifo,
                            .core_active=true, .uhd60=true };
    struct device device = { .data=&rx };
    struct snd_soc_dai dai = { .dev=&device };
    struct snd_pcm_hw_params params = { .rate=48000, .channels=2, .format=2 };
    unsigned int before;
    u32 saved[] = {0x1234, 0x5678, 0x9876};
    memcpy(phy+T7_RX_AUD_PLL0, saved, sizeof(saved));
    core[T7_RX_COR_AUDIO_MCLK]=0x55;
    core[T7_RX_COR_AUDIO_TDM1]=0x66;
    core[T7_RX_COR_AUDIO_TDM2]=0x77;
    for (unsigned int i=0;i<ARRAY_SIZE(t7_hdmirx_audio_core_settings);i++)
        core[t7_hdmirx_audio_core_settings[i][0]]=0x80+i;
    core[T7_RX_COR_AUDIO_INFO]=1;
    assert(t7_hdmirx_audio_hw_params(NULL,&params,&dai)==-ENOLINK);
    assert(!writes && !enables);
    acr(T7_RX_COR_N,6144); acr(T7_RX_COR_CTS,594000);
    assert(t7_hdmirx_audio_word(&rx,T7_RX_COR_CTS)==594000);
    for (unsigned int rate=32000; rate<=48000; rate+=1000) {
        if (rate==32000 || rate==48000) continue;
        assert(t7_hdmirx_validate_audio(594000000,6144,594000,1,0,0,rate));
    }
    assert(!t7_hdmirx_validate_audio(594000000,4096,594000,1,0,0,32000));
    assert(!t7_hdmirx_validate_audio(594000000,6272,660000,1,0,0,44100));
    assert(t7_hdmirx_validate_audio(594000000,6144,594000,0x21,0,0,48000)==-EINVAL);
    core[T7_RX_COR_AUDIO_PACKET]=BIT(6);
    assert(t7_hdmirx_audio_hw_params(NULL,&params,&dai)==-EINVAL);
    core[T7_RX_COR_AUDIO_PACKET]=0;
    core[T7_RX_COR_AUDIO_CHSTATUS]=BIT(1);
    assert(t7_hdmirx_audio_hw_params(NULL,&params,&dai)==-EINVAL);
    core[T7_RX_COR_AUDIO_CHSTATUS]=0;
    params.rate=44100;
    assert(t7_hdmirx_audio_hw_params(NULL,&params,&dai)==-ERANGE);
    params.rate=48000;
    assert(!writes && !enables);
    fail_clocks=true;
    assert(t7_hdmirx_audio_hw_params(NULL,&params,&dai)==-EIO);
    assert(!writes && !clocks && !rx.audio_active);
    fail_clocks=false;
    u32 busy=BIT(31); memcpy(fifo,&busy,4);
    assert(t7_hdmirx_audio_hw_params(NULL,&params,&dai)==-EBUSY);
    assert(!writes && readl(fifo)==busy && !clocks);
    memset(fifo,0,sizeof(fifo));
    assert(t7_hdmirx_audio_hw_params(NULL,&params,&dai)==-ETIMEDOUT);
    assert(!clocks && !rx.audio_active && !memcmp(phy+T7_RX_AUD_PLL0,saved,sizeof(saved)));
    assert(core[T7_RX_COR_AUDIO_MCLK]==0x55 && core[T7_RX_COR_AUDIO_TDM1]==0x66 && core[T7_RX_COR_AUDIO_TDM2]==0x77);
    for (unsigned int i=0;i<ARRAY_SIZE(t7_hdmirx_audio_core_settings);i++)
        assert(core[t7_hdmirx_audio_core_settings[i][0]]==0x80+i);
    before=writes; t7_hdmirx_audio_stop(&rx); assert(before==writes);
    pll_locked=true;
    assert(!t7_hdmirx_audio_hw_params(NULL,&params,&dai));
    assert(clocks && rx.audio_active && (readl(fifo)&BIT(31)));
    assert((core[0x1429] & (BIT(6)|BIT(0)))==BIT(0));
    assert(t7_hdmirx_audio_hw_params(NULL,&params,&dai)==-EBUSY);
    assert(!t7_hdmirx_audio_hw_free(NULL,&dai));
    assert(!clocks && !rx.audio_active && !readl(fifo));
    assert(!memcmp(phy+T7_RX_AUD_PLL0,saved,sizeof(saved)));
    for (unsigned int i=0;i<ARRAY_SIZE(t7_hdmirx_audio_core_settings);i++)
        assert(core[t7_hdmirx_audio_core_settings[i][0]]==0x80+i);
    before=writes; t7_hdmirx_audio_stop(&rx); assert(before==writes);
    assert(disables+1==enables); /* the failed bulk enable owns no clock reference */
    puts("audio source validation, busy FIFO, PLL failure and repeated cleanup passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'audio.c'
    binary = Path(directory) / 'audio'
    code.write_text(prefix + defines + '\n' + types + stubs + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
