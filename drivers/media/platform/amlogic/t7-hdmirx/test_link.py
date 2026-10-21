#!/usr/bin/env python3
"""Actual receiver recovery/lease code with deterministic source and PLL gates."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-hdmirx.c').read_text()
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define T7_RX_'))
types = source[source.index('static const u32 t7_hdmirx_phy_top_regs'):
               source.index('static int t7_hdmirx_validate_edid')]
start = source.index('static u32 t7_hdmirx_sample_cable(')
functions = source[start:source.index('\n}', start) + 2] + '\n'
functions += source[source.index('int t7_hdmirx_stream_get('):
                   source.index('static int t7_hdmirx_status(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunneeded-internal-declaration"
#endif
typedef uint8_t u8;
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define READ_ONCE(value) (value)
#define WRITE_ONCE(place, value) ((place) = (value))
#define WARN_ON(condition) (condition)
#define EXPORT_SYMBOL_GPL(name)
#define msecs_to_jiffies(value) (value)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
struct mutex { bool held; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct clk_bulk_data { void *clk; };
struct debugfs_blob_wrapper { void *data; unsigned long size; };
struct device { void *data; };
'''
stubs = r'''
static bool signal_valid, source_present, hdmi2 = true, pll_fail, late_alignment, lose_during_wait;
static unsigned int pll_calls, pcs_calls, queued, canceled, last_delay, meter_resets;
static unsigned int profile_changes;
static u32 meter_reset;
static u32 cable_hz;
static struct t7_hdmirx receiver;
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static void mutex_lock(struct mutex *lock) { assert(!lock->held); lock->held = true; }
static void mutex_unlock(struct mutex *lock) { assert(lock->held); lock->held = false; }
static int t7_hdmirx_video_status(struct t7_hdmirx *rx)
{
    return signal_valid && (!rx->dynamic_modes || rx->uhd60==hdmi2) ? 0 : -ENOLINK;
}
static int t7_hdmirx_check_video(struct device *dev)
{
    struct t7_hdmirx *rx = dev->data;
    return rx->link->ready ? t7_hdmirx_video_status(rx) : -ENOLINK;
}
static u32 readl(void *address)
{
    if (address == receiver.top + T7_RX_TOP_SW_RESET) return meter_reset;
    if (address == receiver.top + T7_RX_TOP_HPD_PWR5V)
        return source_present ? BIT(20 + receiver.port) : 0;
    assert(address == receiver.top + T7_RX_TOP_METER_CABLE_STAT);
    return cable_hz;
}
static void writel(u32 value, void *address)
{
    if (address == receiver.phy + T7_RX_AUD_PLL0 || address == receiver.phy + T7_RX_AUD_PLL3) {
        assert(!receiver.link->users && receiver.audio_lock.held);
        memcpy(address,&value,4); return;
    }
    assert(address == receiver.top + T7_RX_TOP_SW_RESET && !receiver.link->users);
    assert(!value || value == (BIT(5) | BIT(6)));
    meter_reset = value;
    if (value) meter_resets++;
}
static u8 readb(void *address)
{
    assert(address == receiver.cor + T7_RX_COR_TMDS_CONFIG);
    return hdmi2 ? BIT(1) : 0;
}
static unsigned long clk_get_rate(void *clk) { (void)clk; return 24000000; }
static u32 t7_hdmirx_meter_hz(u32 value, unsigned long rate) { (void)rate; return value; }
static int t7_hdmirx_phy_pll(struct t7_hdmirx *rx)
{
    assert(rx->link->lock.held && !rx->link->users);
    pll_calls++; return pll_fail ? -ETIMEDOUT : 0;
}
static void t7_hdmirx_phy_configure(struct t7_hdmirx *rx) {
    assert(rx->link->lock.held && !rx->link->users); profile_changes++;
}
static void t7_hdmirx_phy_equalize(struct t7_hdmirx *rx)
{
    assert(!rx->link->users); signal_valid = !late_alignment;
}
static void t7_hdmirx_reset_pcs(struct t7_hdmirx *rx) { (void)rx; pcs_calls++; }
static void usleep_range(unsigned int low, unsigned int high)
{
    assert(low == 20000 && high == 21000);
    if (lose_during_wait) signal_valid = false;
}
static void schedule_delayed_work(struct delayed_work *work, unsigned int delay)
{
    (void)work; queued++; last_delay = delay;
}
static void cancel_delayed_work_sync(struct delayed_work *work) { (void)work; canceled++; }
'''
checks = r'''
static void tick(struct t7_hdmirx_link *link, int count)
{
    while (count--) {
        t7_hdmirx_link_work(&link->work.work);
        assert(!link->lock.held && last_delay == 250);
    }
}
int main(void)
{
    unsigned char top[0x4000], core[0x8000], phy[0x100]={0};
    struct t7_hdmirx_link link = {.rx = &receiver};
    struct device dev = {.data = &receiver};
    receiver.top = top; receiver.cor = core; receiver.link = &link; receiver.uhd60 = true;
    tick(&link, 100);
    assert(!link.ready && !pll_calls && queued == 100);
    assert(t7_hdmirx_stream_get(&dev,3840,2160) == -ENOLINK && !link.users);
    source_present = true; cable_hz = 148500000;
    tick(&link, 3); assert(!pll_calls);
    tick(&link, 1); assert(pll_calls == 1 && pcs_calls == 1 && link.ready);
    tick(&link, 100); assert(pll_calls == 1);
    assert(!t7_hdmirx_stream_get(&dev,3840,2160) && !t7_hdmirx_stream_get(&dev,3840,2160) && link.users == 2);
    unsigned int resets_before = meter_resets;
    signal_valid = false; tick(&link, 10);
    assert(!link.ready && pll_calls == 1 && meter_resets == resets_before);
    t7_hdmirx_stream_put(&dev); tick(&link, 10); assert(pll_calls == 1);
    t7_hdmirx_stream_put(&dev); tick(&link, 4); assert(link.ready && pll_calls == 2);
    signal_valid = false; source_present = false; tick(&link, 10);
    assert(!link.ready && !link.configured && !link.stable_samples && pll_calls == 2);
    source_present = true; hdmi2 = false; tick(&link, 10); assert(pll_calls == 2);
    hdmi2 = true; pll_fail = true; tick(&link, 4); assert(!link.ready && pll_calls == 3);
    pll_fail = false; late_alignment = true; tick(&link, 3);
    assert(pll_calls == 4 && !link.ready && link.configured);
    signal_valid = true; tick(&link, 1); assert(link.ready && pll_calls == 4);
    link.ready = false; lose_during_wait = true; tick(&link, 1);
    assert(!link.ready && !signal_valid);
    lose_during_wait=false; late_alignment=false; receiver.dynamic_modes=true;
    receiver.phy=phy; receiver.audio_active=true;
    hdmi2=false; tick(&link,4);
    assert(link.ready && !receiver.uhd60 && profile_changes==1);
    u32 audio_div; memcpy(&audio_div,phy+T7_RX_AUD_PLL3,4); assert(audio_div==2);
    assert(t7_hdmirx_stream_get(&dev,3840,2160)==-EPIPE && !link.users);
    assert(!t7_hdmirx_stream_get(&dev,1920,1080));
    hdmi2=true; tick(&link,20);
    assert(!link.ready && !receiver.uhd60 && profile_changes==1);
    t7_hdmirx_stream_put(&dev); tick(&link,4);
    assert(link.ready && receiver.uhd60 && profile_changes==2);
    memcpy(&audio_div,phy+T7_RX_AUD_PLL3,4); assert(audio_div==0);
    assert(t7_hdmirx_stream_get(&dev,1920,1080)==-EPIPE && !link.users);
    t7_hdmirx_link_stop(&link);
    assert(canceled == 1 && !link.ready);
    puts("Receiver recovery: absent/delayed source, DMA leases, PLL failure and late alignment passed");
}
'''
with tempfile.TemporaryDirectory(prefix='t7-link-test-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(prefix + defines + '\n' + types + stubs + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(path/'test.c'),
                    '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
