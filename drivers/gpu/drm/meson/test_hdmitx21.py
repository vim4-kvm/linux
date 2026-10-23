#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the TX transport and DDC state machine with deterministic firmware replies."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('meson_hdmitx21.c').read_text()


def functions(start, end):
    return source[source.index(start):source.index(end)]


transport = functions('static u32 meson_hdmitx21_smc_read', 'static u32 phy_read')
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef uint64_t resource_size_t;
struct meson_hdmitx21 { resource_size_t boot_top, boot_cor; };
struct arm_smccc_res { unsigned long a0; };
static unsigned long call[8];
static void arm_smccc_smc(unsigned long a, unsigned long b, unsigned long c,
                         unsigned long d, unsigned long e, unsigned long f,
                         unsigned long g, unsigned long h, struct arm_smccc_res *res)
{
    unsigned long args[] = {a,b,c,d,e,f,g,h};
    for (int i = 0; i < 8; i++) call[i] = args[i];
    res->a0 = 0x87654321;
}
'''
program += transport
program += r'''
int main(void)
{
    struct meson_hdmitx21 tx = {0xfe300000, 0xfe380000};
    assert(top_read(&tx, 0x8c) == 0x87654321);
    assert(call[0] == 0x82000018 && call[1] == 0xfe30008c && call[2] == 32);
    assert(cor_read(&tx, 0x61a) == 0x21);
    assert(call[1] == 0xfe38061a && call[2] == 8 && call[3] == 0);
    top_write(&tx, 0x14, 0x12345678);
    assert(call[0] == 0x82000019 && call[1] == 0xfe300014);
    assert(call[2] == 0x12345678 && call[3] == 32);
    cor_write(&tx, 0x9, 0x302);
    assert(call[1] == 0xfe380009 && call[2] == 2 && call[3] == 8);
    for (int i = 4; i < 8; i++) assert(call[i] == 0);
    puts("TX firmware transport: TOP32/COR8 read and write arguments passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'transport.c'
    c.write_text(program)
    binary = Path(tmp) / 'transport'
    subprocess.run(['cc', '-Wno-overflow', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
