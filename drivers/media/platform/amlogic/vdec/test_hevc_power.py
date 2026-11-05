#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise HEVC domain/clock rollback before any firmware or DMA is allowed."""
from pathlib import Path
import unittest

from test_t7_lifetime import COMMON, function, run_c

HERE = Path(__file__).resolve().parent


class HEVCPowerTests(unittest.TestCase):
    def test_power_preflight_and_every_backend_failure_preserve_ownership(self):
        code = '\n'.join(function(HERE / 'aml_vdec_hw.c', name) for name in (
            't7_hevc_power_off', 't7_hevc_power_on', 'aml_vdec_t7_hevc_test'))
        run_c(COMMON + r'''
#include <string.h>
#define U32_MAX UINT32_MAX
#define DOS_BUS 0
#define HEVCF_MUX 2
#define VDEC_HEVC 1
#define PWRC_T7_DOS_HEVC_ID 3
#define DOS_GCLK_EN3 0xfc00
#define HEVC_MPSR 0xcc04
#define HEVC_PARSER_VERSION 0xc400
#define dev_info(...) ((void)0)
typedef int32_t s32;
struct clk { int id; };
struct gate { struct clk *clk; };
struct pd { void *dev; };
struct pm { struct pd pd_data[2]; };
struct aml_vdec_hw {
    void *dev, *map[1], *clkctrl;
    struct gate gates[3];
    struct clk *hevc_p0;
    struct pm *pm;
    bool has_hevc, powered, faulted, hevc_powered, probe_failed;
    bool hevc_test_passed, mdec_test_passed;
    u32 hevc_parser_version, probe_step;
};
static unsigned int calls, fail_at, last_calls, internal_reads;
static unsigned int clock_refs, domain_refs, bus_refs, frequency;
static u32 local_gates, bad_clock, busy_cpu;
static struct clk parent = { 0 }, mux = { 1 };
static struct pm pm;
static struct aml_vdec_hw hw;
static int backend(void) { return ++calls == fail_at ? -EIO : 0; }
static int t7_domain_state(unsigned int id, s32 *state)
{
    assert(id == PWRC_T7_DOS_HEVC_ID);
    if (backend()) return -EIO;
    *state = !domain_refs;
    return 0;
}
static void t7_power_fault(struct aml_vdec_hw *h) { h->faulted = true; }
static int clk_set_parent(struct clk *c, struct clk *p)
{
    assert(bus_refs == 1 && local_gates == U32_MAX && c == &mux && p == &parent);
    return backend();
}
static int clk_set_rate(struct clk *p, unsigned long rate)
{
    assert(p == &parent);
    if (backend()) return -EIO;
    frequency = rate; return 0;
}
static int clk_prepare_enable(struct clk *c)
{
    assert(c == &mux && frequency == 800000000);
    if (backend()) return -EIO;
    clock_refs++; return 0;
}
static void clk_disable_unprepare(struct clk *c)
{ assert(c == &mux && clock_refs == 1); clock_refs--; }
static int pm_runtime_resume_and_get(void *dev)
{
    assert(clock_refs == 1 && bus_refs == 1);
    if (backend()) return -EIO;
    domain_refs++; return 0;
}
static int pm_runtime_put_sync_suspend(void *dev)
{
    assert(!clock_refs && domain_refs == 1);
    if (backend()) return -EIO;
    domain_refs--; return 0;
}
static int regmap_write(void *map, u32 reg, u32 value)
{
    assert(reg == DOS_GCLK_EN3 && bus_refs == 1);
    if (backend()) return -EIO;
    local_gates = value; return 0;
}
static int regmap_read(void *map, u32 reg, u32 *value)
{
    if (backend()) return -EIO;
    assert(domain_refs == 1 && clock_refs == 1 && bus_refs == 1);
    if (reg == 0x144) *value = BIT(8) ^ bad_clock;
    else if (reg == 0x14c) *value = 0;
    else if (reg == DOS_GCLK_EN3) *value = local_gates;
    else {
        assert(!bad_clock);
        internal_reads++;
        if (reg == HEVC_MPSR) *value = busy_cpu;
        else { assert(reg == HEVC_PARSER_VERSION); *value = 0x10001; }
    }
    return 0;
}
static int vdec_enable(struct aml_vdec_hw *h)
{
    if (backend()) return -EIO;
    assert(!bus_refs);
    h->powered = true; bus_refs++; return 0;
}
static int vdec_disable(struct aml_vdec_hw *h)
{
    assert(!h->hevc_powered && !clock_refs && !domain_refs);
    if (backend()) { h->faulted = true; return -EIO; }
    h->powered = false; bus_refs--; return 0;
}
''' + code + r'''
static void reset(unsigned int failure)
{
    memset(&hw, 0, sizeof(hw));
    hw.has_hevc = hw.mdec_test_passed = true;
    hw.gates[HEVCF_MUX].clk = &mux; hw.hevc_p0 = &parent; hw.pm = &pm;
    calls = internal_reads = clock_refs = domain_refs = bus_refs = 0;
    local_gates = frequency = bad_clock = busy_cpu = 0;
    fail_at = failure;
}
int main(void)
{
    reset(0);
    assert(!aml_vdec_t7_hevc_test(&hw));
    assert(hw.hevc_test_passed && hw.hevc_parser_version == 0x10001);
    assert(internal_reads == 2 && !clock_refs && !domain_refs && !bus_refs);
    last_calls = calls;
    assert(!aml_vdec_t7_hevc_test(&hw));
    assert(!clock_refs && !domain_refs && !bus_refs);
    for (unsigned int i = 1; i <= last_calls; i++) {
        reset(i);
        assert(aml_vdec_t7_hevc_test(&hw) < 0);
        assert(!hw.hevc_test_passed && !clock_refs);
        if (hw.faulted) assert(bus_refs == 1);
        else assert(!bus_refs && !domain_refs);
    }
    reset(0); bad_clock = BIT(8);
    assert(aml_vdec_t7_hevc_test(&hw) == -EHOSTDOWN);
    assert(!internal_reads && !clock_refs && !domain_refs && !bus_refs);
    reset(0); busy_cpu = 1;
    assert(aml_vdec_t7_hevc_test(&hw) == -EBUSY);
    assert(internal_reads == 1 && !clock_refs && !domain_refs && !bus_refs);
    reset(0); hw.has_hevc = false;
    assert(aml_vdec_t7_hevc_test(&hw) == -EOPNOTSUPP && !calls);
    reset(0); hw.mdec_test_passed = false;
    assert(aml_vdec_t7_hevc_test(&hw) == -EBUSY && !calls);
    reset(0); hw.faulted = true;
    assert(aml_vdec_t7_hevc_test(&hw) == -EIO && !calls);
    return 0;
}
''')
