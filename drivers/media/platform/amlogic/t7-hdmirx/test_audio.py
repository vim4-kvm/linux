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
worker = source[source.index('static const char * const t7_hdmirx_audio_control_names'):
                source.index('static int t7_hdmirx_audio_control_info')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <limits.h>
struct mutex { bool held; };
struct delayed_work { int unused; };
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define READ_ONCE(value) (value)
#define div_u64(value, divisor) ((value) / (divisor))
#define SNDRV_PCM_FORMAT_S16_LE 2
struct clk_bulk_data { const char *id; void *clk; };
struct debugfs_blob_wrapper { void *data; unsigned long size; };
struct device { void *data; };
struct snd_soc_card { void *snd_card; };
struct snd_soc_component { struct snd_soc_card *card; };
struct snd_soc_dai { struct device *dev; };
struct snd_pcm_substream { void *runtime; };
#define SNDRV_PCM_HW_PARAM_RATE 0
static unsigned int constraint_rate;
static int snd_pcm_hw_constraint_minmax(void *runtime, int param, unsigned int min, unsigned int max) {
    assert(min == max); constraint_rate = min; return 0;
}
struct snd_pcm_hw_params { unsigned int rate, channels; int format; };
#define params_rate(p) ((p)->rate)
#define params_channels(p) ((p)->channels)
#define params_format(p) ((p)->format)
'''
stubs = r'''
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; }
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
static void aif(unsigned int rate_code, unsigned int channels) {
    u8 *p = core + T7_RX_COR_AUDIO_AIF;
    memset(p, 0, 14); p[0]=0x84; p[1]=1; p[2]=10;
    p[4]=0x10 | (channels-1); p[5]=rate_code << 2;
    for (unsigned int i=0;i<14;i++) if(i!=3) p[3]-=p[i];
}
struct work_struct { int unused; };
struct snd_ctl_elem_id { int iface; char name[44]; };
#define SNDRV_CTL_ELEM_IFACE_MIXER 2
#define SNDRV_CTL_EVENT_MASK_VALUE 1
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
#define to_delayed_work(work) ((struct delayed_work *)(work))
#define msecs_to_jiffies(ms) (ms)
#define strscpy(dst,src,size) snprintf(dst,size,"%s",src)
static unsigned int notifications, xruns;
static struct t7_hdmirx *watched;
static int snd_soc_component_notify_control(struct snd_soc_component *component, const char *name) {
    assert(component && !strncmp(name,"HDMI RX Audio ",14)); notifications++; return 0;
}
static void snd_pcm_stop_xrun(struct snd_pcm_substream *stream) {
    assert(stream && watched && !watched->audio_lock.held); xruns++;
}
static void schedule_delayed_work(struct delayed_work *work, unsigned int delay) {
    assert(delay==100);
}
'''
checks = r'''
int main(void) {
    struct t7_hdmirx rx = { .top=top, .phy=phy, .cor=core, .frhdmirx=fifo,
                            .core_active=true, .uhd60=true };
    struct device device = { .data=&rx };
    struct snd_soc_dai dai = { .dev=&device };
    struct snd_pcm_substream stream = {0};
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
    aif(3,2);
    assert(!t7_hdmirx_read_audio(&rx,&rx.audio_format));
    assert(rx.audio_format.rate==48000 && rx.audio_format.pcm);
    assert(!t7_hdmirx_audio_startup(&stream,&dai) && constraint_rate==48000);
    assert(t7_hdmirx_audio_prepare(&stream,&dai)==-ENOLINK);
    const unsigned int rates[]={32000,44100,48000,88200,96000,176400,192000};
    const unsigned int ns[]={4096,6272,6144,12544,12288,25088,24576};
    for (unsigned int i=0;i<ARRAY_SIZE(rates);i++) {
        struct t7_hdmirx_audio_format format={0};
        acr(T7_RX_COR_N,ns[i]); acr(T7_RX_COR_CTS,i==1||i==3||i==5?660000:594000);
        aif(i+1,2);
        assert(!t7_hdmirx_read_audio(&rx,&format));
        assert(format.rate==rates[i] && format.pcm);
    }
    acr(T7_RX_COR_N,6144); acr(T7_RX_COR_CTS,594000); aif(3,8);
    struct t7_hdmirx_audio_format unsupported={0};
    assert(!t7_hdmirx_read_audio(&rx,&unsupported));
    assert(unsupported.channels==8 && !unsupported.pcm);
    aif(2,2); /* internally valid packet with a contradictory ACR rate */
    assert(t7_hdmirx_read_audio(&rx,&unsupported)==-EAGAIN);
    aif(3,2); core[T7_RX_COR_AUDIO_AIF+3]++;
    assert(t7_hdmirx_read_audio(&rx,&unsupported)==-EAGAIN);
    aif(3,2);
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
    assert(!t7_hdmirx_audio_hw_params(&stream,&params,&dai));
    assert(!t7_hdmirx_audio_prepare(&stream,&dai));
    rx.audio_invalid=true;
    assert(t7_hdmirx_audio_prepare(&stream,&dai)==-ENOLINK);
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
    struct snd_soc_card card={0};
    struct snd_soc_component component={.card=&card};
    rx.audio_component=&component; watched=&rx;
    struct t7_hdmirx_link link={.ready=true}; rx.link=&link;
    memset(&rx.audio_format,0,sizeof(rx.audio_format));
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(!rx.audio_format.rate && !notifications);
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(rx.audio_format.rate==48000 && notifications==4 && rx.audio_generation==1);
    assert(!t7_hdmirx_audio_hw_params(&stream,&params,&dai));
    before=rx.audio_generation;
    core[T7_RX_COR_AUDIO_AIF+3]++;
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(rx.audio_generation==before && !rx.audio_invalid && !xruns);
    core[T7_RX_COR_AUDIO_AIF+3]--;
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(!rx.audio_read_retries);
    acr(T7_RX_COR_N,6272); acr(T7_RX_COR_CTS,660000); aif(2,2);
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(!rx.audio_format.rate && xruns==1 && !(readl(fifo)&BIT(31)));
    assert(t7_hdmirx_audio_prepare(&stream,&dai)==-ENOLINK);
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(rx.audio_format.rate==44100 && rx.audio_generation==3 && notifications==12);
    assert(!t7_hdmirx_audio_hw_free(&stream,&dai));
    params.rate=44100;
    assert(!t7_hdmirx_audio_hw_params(&stream,&params,&dai));
    link.ready=false;
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(!rx.audio_format.rate && rx.audio_invalid);
    assert(!t7_hdmirx_audio_hw_free(&stream,&dai));
    link.ready=true;
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(!rx.audio_format.rate);
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(rx.audio_format.rate==44100 && rx.audio_generation==5);
    before=notifications;
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(before==notifications); /* no repeated events on a stable source */
    assert(!t7_hdmirx_audio_hw_params(&stream,&params,&dai));
    before=xruns;
    core[T7_RX_COR_AUDIO_AIF+3]++;
    for(unsigned int i=0;i<2;i++) {
        t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
        assert(rx.audio_format.rate==44100 && before==xruns);
    }
    t7_hdmirx_audio_work((struct work_struct *)&rx.audio_work);
    assert(!rx.audio_format.rate && xruns==before+1);
    assert(!t7_hdmirx_audio_hw_free(&stream,&dai));
    puts("seven ACR/AIF rates, malformed/unsupported packets, native constraints, stale PCM, PLL unwind passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'audio.c'
    binary = Path(directory) / 'audio'
    code.write_text(prefix + defines + '\n' + types + stubs + functions + worker + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
