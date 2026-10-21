#!/usr/bin/env python3
"""Exercise actual PHY functions with bounded MMIO, clock loss and PLL timeout."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-hdmirx.c').read_text()
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define T7_RX_'))
types = source[source.index('static const u32 t7_hdmirx_phy_top_regs'):
               source.index('static int t7_hdmirx_validate_edid')]
functions = source[source.index('static u32 t7_hdmirx_meter_hz'):
                   source.index('static void t7_hdmirx_core_stop')]
for signature in ('static int t7_hdmirx_validate_edid(',
                  'static int t7_hdmirx_validate_phy_edid('):
    start = source.index(signature)
    functions += source[start:source.index('\n}\n', start) + 3]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
struct mutex { int unused; };
struct delayed_work { int unused; };
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define U32_MAX UINT32_MAX
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctz(mask))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define div_u64(value, divisor) ((value) / (divisor))
struct clk_bulk_data { int unused; };
struct debugfs_blob_wrapper { void *data; unsigned long size; };
'''
mmio = r'''
static u8 top[0x4000], phy[0x50], core[0x8000];
static u8 readb(void *address) {
    assert((u8 *)address >= core && (u8 *)address < core + sizeof(core));
    return *(u8 *)address;
}
static struct t7_hdmirx receiver;
static bool lock_pll, jitter, delayed_source;
static unsigned int delays, meter_reads, pll_attempts, writes;
static u32 raw_read(void *address)
{
    u32 value;
    memcpy(&value, address, 4);
    return value;
}
static bool bounded(void *address)
{
    uintptr_t addr = (uintptr_t)address;
    return (addr >= (uintptr_t)top && addr + 4 <= (uintptr_t)top + sizeof(top)) ||
           (addr >= (uintptr_t)phy && addr + 4 <= (uintptr_t)phy + sizeof(phy));
}
static u32 readl(void *address)
{
    assert(bounded(address));
    if (address == top + T7_RX_TOP_METER_CABLE_STAT) {
        meter_reads++;
        if (delayed_source && meter_reads <= 200) return 0;
        if (jitter) return BIT(31) | (meter_reads % 2 ? 50500 : 50800);
    }
    if (address == phy + T7_RX_PHY_APLL0 && lock_pll &&
        raw_read(address) == 0x14090428)
        return raw_read(address) | T7_RX_PHY_PLL_LOCK;
    return raw_read(address);
}
static void writel(u32 value, void *address)
{
    assert(bounded(address));
    if (address == phy + T7_RX_PHY_APLL0 && value == 0x20090428)
        pll_attempts++;
    memcpy(address, &value, 4);
    writes++;
}
static void usleep_range(unsigned int low, unsigned int high)
{
    assert(low > 0 && high >= low);
    assert(++delays < 1000);
}
static void t7_hdmirx_set_hpd(struct t7_hdmirx *rx, bool high)
{
    assert(!high);
    writel(readl(rx->top + T7_RX_TOP_HPD_PWR5V) & ~BIT(rx->port),
           rx->top + T7_RX_TOP_HPD_PWR5V);
}
static void reset(void)
{
    memset(top, 0, sizeof(top));
    memset(phy, 0, sizeof(phy));
    memset(core, 0, sizeof(core));
    receiver = (struct t7_hdmirx) { .top = top, .phy = phy, .cor = core, .port = 1 };
    lock_pll = jitter = delayed_source = false;
    delays = meter_reads = pll_attempts = writes = 0;
    writel(BIT(21) | BIT(1), top + T7_RX_TOP_HPD_PWR5V);
}
'''
checks = r'''
int main(void)
{
    unsigned int i, before;
    u8 edid[256] = {0};
    memset(edid + 1, 0xff, 6);
    edid[20] = 0x80; edid[54] = 0x02; edid[55] = 0x3a;
    for (i = 0; i < 127; i++) edid[127] -= edid[i];
    assert(!t7_hdmirx_validate_phy_edid(edid, 128));
    edid[126] = 1; edid[127]--;
    assert(t7_hdmirx_validate_phy_edid(edid, 256) == -EOPNOTSUPP);
    edid[126] = 0; edid[127]++;
    edid[54] = 0x01; edid[127]++;
    assert(t7_hdmirx_validate_phy_edid(edid, 128) == -EOPNOTSUPP);
    edid[20] = 0; edid[127] += 0x80;
    assert(t7_hdmirx_validate_phy_edid(edid, 128) == -EOPNOTSUPP);
    edid[0] = 1;
    assert(t7_hdmirx_validate_phy_edid(edid, 128) == -EINVAL);
    assert(t7_hdmirx_meter_hz(50688, 24000000) == 0);
    assert(t7_hdmirx_meter_hz(BIT(31) | 50688, 24000000) == 148500000);
    assert(t7_hdmirx_meter_hz(BIT(31) | 50688, 12000000) == 74250000);
    assert(!t7_hdmirx_meter_hz(BIT(31) | 0xffffff, 24000000));
    reset();
    for (i = 0; i < ARRAY_SIZE(t7_hdmirx_phy_top_regs); i++)
        writel(0xa5000000 | i, top + t7_hdmirx_phy_top_regs[i]);
    writel(0x9001, phy + T7_RX_PHY_MISC1);
    t7_hdmirx_phy_prepare(&receiver);
    assert(receiver.phy_active);
    assert((readl(phy + T7_RX_PHY_MISC1) & 0xf001) == 0x9001);
    assert((readl(phy + T7_RX_PHY_MISC3) & (7 << 6)) == BIT(7));
    assert((readl(top + T7_RX_TOP_PORT_SEL) & 7) == 2);
    writel(BIT(31) | 50688, top + T7_RX_TOP_METER_CABLE_STAT);
    assert(!t7_hdmirx_wait_cable(&receiver, 24000000));
    assert(meter_reads == 4);
    lock_pll = true;
    assert(!t7_hdmirx_phy_pll(&receiver) && pll_attempts == 1);
    t7_hdmirx_phy_equalize(&receiver);
    assert(readl(phy + T7_RX_PHY_DCHD0) & BIT(28));
    assert(readl(phy + T7_RX_PHY_DCHD0) & BIT(27));
    assert(readl(phy + T7_RX_PHY_DCHD2) & BIT(26));
    t7_hdmirx_phy_stop(&receiver);
    assert(!receiver.phy_active);
    assert(!(readl(top + T7_RX_TOP_HPD_PWR5V) & BIT(1)));
    assert((readl(phy + T7_RX_PHY_APLL0) & (3 << 28)) == BIT(29));
    for (i = 0; i < ARRAY_SIZE(t7_hdmirx_phy_top_regs); i++)
        assert(readl(top + t7_hdmirx_phy_top_regs[i]) == (0xa5000000 | i));
    before = writes;
    t7_hdmirx_phy_stop(&receiver);
    assert(writes == before);
    /* A valid-looking but unstable input must never reach PLL programming. */
    reset(); jitter = true;
    assert(t7_hdmirx_wait_cable(&receiver, 24000000) == -ETIMEDOUT);
    assert(meter_reads == 500 && !pll_attempts);
    /* Source reconnection may take more than three seconds after HPD. */
    reset(); delayed_source = true;
    writel(BIT(31) | 50688, top + T7_RX_TOP_METER_CABLE_STAT);
    assert(!t7_hdmirx_wait_cable(&receiver, 24000000) && meter_reads == 204);
    reset(); writel(BIT(31) | 101376, top + T7_RX_TOP_METER_CABLE_STAT);
    assert(t7_hdmirx_wait_cable(&receiver, 24000000) == -ETIMEDOUT);
    assert(receiver.cable_hz == 297000000 && !pll_attempts);
    reset(); writel(50688, top + T7_RX_TOP_METER_CABLE_STAT);
    assert(t7_hdmirx_wait_cable(&receiver, 24000000) == -ETIMEDOUT);
    assert(!receiver.cable_hz && !pll_attempts);
    reset(); writel(0, top + T7_RX_TOP_HPD_PWR5V);
    assert(t7_hdmirx_wait_cable(&receiver, 24000000) == -ENOLINK && !meter_reads);
    reset(); t7_hdmirx_phy_prepare(&receiver);
    assert(t7_hdmirx_phy_pll(&receiver) == -ETIMEDOUT && pll_attempts == 3);
    t7_hdmirx_phy_stop(&receiver);
    assert(!receiver.phy_active && !(readl(top + T7_RX_TOP_HPD_PWR5V) & BIT(1)));
    reset(); receiver.uhd60 = true;
    writel(BIT(31) | 50688, top + T7_RX_TOP_METER_CABLE_STAT);
    assert(t7_hdmirx_wait_cable(&receiver, 24000000) == -ETIMEDOUT);
    assert(!pll_attempts);
    reset(); receiver.uhd60 = true; core[T7_RX_COR_TMDS_CONFIG] = 3;
    writel(BIT(31) | 50688, top + T7_RX_TOP_METER_CABLE_STAT);
    assert(!t7_hdmirx_wait_cable(&receiver, 24000000));
    lock_pll = true;
    t7_hdmirx_phy_prepare(&receiver);
    assert(!t7_hdmirx_phy_pll(&receiver));
    assert((raw_read(phy + T7_RX_PHY_APLL4) & (7U << 24)) == (1U << 24));
    puts("PHY clock/SCDC qualification, trim, bounded PLL timeout and cleanup passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'phy.c'
    executable = Path(directory) / 'phy'
    code.write_text(prefix + defines + '\n' + types + mmio + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(code),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
