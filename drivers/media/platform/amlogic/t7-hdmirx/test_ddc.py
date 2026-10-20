#!/usr/bin/env python3
"""Compile the actual EDID/DDC functions with bounded fake MMIO and failures."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-hdmirx.c').read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define T7_RX_'))
types = source[source.index('static const u32 t7_hdmirx_phy_top_regs'):source.index('static int t7_hdmirx_validate_edid')]
functions = '\n'.join(function(name) for name in [
    'static int t7_hdmirx_validate_edid(', 'static void t7_hdmirx_set_hpd(',
    'static void t7_hdmirx_ddc_stop(', 'static int t7_hdmirx_ddc_start('])
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
struct clk_bulk_data { int unused; };
struct debugfs_blob_wrapper { void *data; unsigned long size; };
'''
mmio = r'''
static u8 memory[0x4000];
static struct t7_hdmirx receiver;
static unsigned int written, verified, hpd_high, register_writes, delay_calls;
static bool corrupt;
static u32 readl(void *address)
{
    u32 value;
    assert((u8 *)address >= memory && (u8 *)address + 4 <= memory + sizeof(memory));
    memcpy(&value, address, sizeof(value));
    return value;
}
static void writel(u32 value, void *address)
{
    if ((u8 *)address == memory + T7_RX_TOP_HPD_PWR5V && (value & BIT(receiver.port))) {
        /* Source must never see HPD before the whole bank has been verified. */
        assert(written == T7_RX_EDID_CAPACITY && verified == T7_RX_EDID_CAPACITY);
        assert(delay_calls == 1 && receiver.edid_blob.size != 0);
        hpd_high++;
    }
    assert((u8 *)address >= memory && (u8 *)address + 4 <= memory + sizeof(memory));
    memcpy(address, &value, sizeof(value));
    register_writes++;
}
static void writeb(u8 value, void *address)
{
    unsigned int base = T7_RX_EDID_BASE + receiver.port * T7_RX_EDID_CAPACITY;
    assert((u8 *)address >= memory + base &&
           (u8 *)address < memory + base + T7_RX_EDID_CAPACITY);
    assert(!(readl(memory + T7_RX_TOP_HPD_PWR5V) & BIT(receiver.port)));
    *(u8 *)address = value;
    written++;
}
static u8 readb(void *address)
{
    verified++;
    return *(u8 *)address ^ (corrupt && verified == 100 ? 1 : 0);
}
static void msleep(unsigned int delay) { assert(delay >= 100); delay_calls++; }
static void usleep_range(unsigned int low, unsigned int high) { assert(low >= 1000 && high >= low); }
'''
checks = r'''
static void checksum(u8 *data)
{
    unsigned int i;
    u8 sum = 0;
    data[127] = 0;
    for (i = 0; i < 127; i++) sum += data[i];
    data[127] = -sum;
}
static void make_edid(u8 *data, unsigned int blocks)
{
    unsigned int i;
    memset(data, 0, T7_RX_EDID_CAPACITY);
    memset(data + 1, 0xff, 6);
    data[18] = 1; data[19] = 4; data[126] = blocks - 1;
    for (i = 1; i < blocks; i++) data[i * 128] = 2;
    for (i = 0; i < blocks; i++) checksum(data + i * 128);
}
static void reset(void)
{
    u32 saved_reset = 0xffff, status = (2U << 20) | BIT(0) | BIT(2);
    memset(memory, 0xa5, sizeof(memory));
    receiver = (struct t7_hdmirx) { .top = memory, .port = 1 };
    memcpy(memory + T7_RX_TOP_SW_RESET, &saved_reset, 4);
    memcpy(memory + T7_RX_TOP_HPD_PWR5V, &status, 4);
    written = verified = hpd_high = register_writes = delay_calls = 0;
    corrupt = false;
}
int main(void)
{
    u8 data[T7_RX_EDID_CAPACITY], before[sizeof(memory)];
    unsigned int blocks, i, writes;
    for (blocks = 1; blocks <= 4; blocks++) {
        make_edid(data, blocks);
        assert(!t7_hdmirx_validate_edid(data, blocks * 128));
        data[blocks * 128 - 1]++;
        assert(t7_hdmirx_validate_edid(data, blocks * 128) == -EINVAL);
    }
    make_edid(data, 1);
    assert(t7_hdmirx_validate_edid(data, 0) == -EINVAL);
    assert(t7_hdmirx_validate_edid(data, 127) == -EINVAL);
    assert(t7_hdmirx_validate_edid(data, 129) == -EINVAL);
    assert(t7_hdmirx_validate_edid(data, 513) == -E2BIG);
    assert(t7_hdmirx_validate_edid(data, 10000) == -E2BIG);
    data[1] = 0; checksum(data);
    assert(t7_hdmirx_validate_edid(data, 128) == -EINVAL);
    make_edid(data, 1);
    data[126] = 2; checksum(data);
    reset(); memcpy(before, memory, sizeof(memory));
    assert(t7_hdmirx_ddc_start(&receiver, data, 128) == -EINVAL);
    assert(!memcmp(before, memory, sizeof(memory)) && !receiver.ddc_active);
    for (blocks = 1; blocks <= 4; blocks++) {
        reset(); make_edid(data, blocks);
        assert(!t7_hdmirx_ddc_start(&receiver, data, blocks * 128));
        assert(receiver.ddc_active && hpd_high == 1);
        assert((readl(memory + T7_RX_TOP_HPD_PWR5V) & 7) == 7);
        assert(!memcmp(receiver.edid, data, blocks * 128));
        for (i = 0; i < 512; i++) {
            assert(memory[T7_RX_EDID_BASE + i] == 0xa5);
            assert(memory[T7_RX_EDID_BASE + 1024 + i] == 0xa5);
        }
        t7_hdmirx_ddc_stop(&receiver);
        assert(!receiver.ddc_active && !receiver.edid_blob.size);
        assert((readl(memory + T7_RX_TOP_HPD_PWR5V) & 7) == 5);
        assert(readl(memory + T7_RX_TOP_SW_RESET) == 0xffff);
        assert(readl(memory + T7_RX_TOP_PORT_SEL) == 0xa5a5a5a5);
        writes = register_writes;
        t7_hdmirx_ddc_stop(&receiver);
        assert(register_writes == writes);
    }
    reset(); make_edid(data, 2); corrupt = true;
    assert(t7_hdmirx_ddc_start(&receiver, data, 256) == -EIO);
    assert(!receiver.ddc_active && !receiver.edid_blob.size && !hpd_high);
    assert(readl(memory + T7_RX_TOP_SW_RESET) == 0xffff);
    assert(readl(memory + T7_RX_TOP_PORT_SEL) == 0xa5a5a5a5);
    puts("EDID validation, HPD ordering, bank isolation and readback failure passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'ddc.c'
    executable = Path(directory) / 'ddc'
    code.write_text(prefix + defines + '\n' + types + mmio + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
