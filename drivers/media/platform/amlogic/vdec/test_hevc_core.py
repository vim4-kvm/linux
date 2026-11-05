#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the real core probe with stuck DMA, partial AXI ack and backend faults."""
from pathlib import Path
import unittest

from test_t7_lifetime import COMMON, function, run_c

HERE = Path(__file__).resolve().parent


class HEVCCoreTests(unittest.TestCase):
    def test_stop_reset_and_failed_load_preserve_dma_ownership(self):
        code = '\n'.join(function(HERE / 'aml_vdec_hw.c', name) for name in (
            't7_hevc_stop', 't7_hevc_reset', 'aml_vdec_t7_hevc_core_test'))
        run_c(COMMON + r'''
#include <string.h>
#include "reg_defines.h"
#define DOS_BUS 0
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define cpu_relax() ((void)0)
struct regmap { int unused; };
struct arm_smccc_res { unsigned long a0; };
struct aml_vdec_hw {
    struct regmap *map[1];
    bool has_hevc, faulted, probe_failed, hevc_test_passed, powered;
    bool hevc_core_test_passed;
    u32 probe_step, hevc_axi_front, hevc_axi_back;
};
static struct aml_vdec_hw hw;
static u32 regs[0x10000], stuck_reg, front_ack, back_ack;
static unsigned int calls, fail_at, bus_refs, hevc_refs, reset_count, loads;
static unsigned int stop_after_load, accesses_after_fault;
static int backend(void)
{
    if (hw.faulted) accesses_after_fault++;
    return ++calls == fail_at ? -EIO : 0;
}
static int regmap_write(struct regmap *map, u32 reg, u32 value)
{
    assert(bus_refs && hevc_refs && !hw.faulted);
    if (backend()) return -EIO;
    if (reg == HEVC_MPSR || reg == HEVC_CPSR || reg == HEVC_ASSIST_MBOX0_MASK)
        assert(value == 0);
    if (reg == HEVC_ASSIST_MBOX0_MASK && loads) stop_after_load++;
    if (reg == DOS_SW_RESET3 && value) {
        assert((regs[HEVC_ASSIST_AXI_CTRL] & (BIT(6) | BIT(14))) == (BIT(6) | BIT(14)));
        assert(regs[HEVC_SAO_MMU_RESET_CTRL] & 1);
        reset_count++;
    }
    if (reg == HEVC_ASSIST_AXI_CTRL && !(value & (BIT(6) | BIT(14)))) {
        assert(reset_count && !regs[DOS_SW_RESET3]);
        assert(!(regs[HEVC_SAO_MMU_RESET_CTRL] & 1));
    }
    regs[reg] = value;
    return 0;
}
static int regmap_read(struct regmap *map, u32 reg, u32 *value)
{
    assert(bus_refs && hevc_refs && !hw.faulted);
    if (backend()) return -EIO;
    if (reg == HEVC_ASSIST_AXI_STATUS) {
        assert(regs[HEVC_ASSIST_AXI_CTRL] & BIT(6));
        *value = front_ack;
    } else if (reg == HEVC_ASSIST_AXI_STATUS2_LO) {
        assert(regs[HEVC_ASSIST_AXI_CTRL] & BIT(14));
        *value = back_ack;
    } else if (reg == stuck_reg) *value = UINT32_MAX;
    else *value = regs[reg];
    return 0;
}
static int regmap_update_bits(struct regmap *map, u32 reg, u32 mask, u32 value)
{
    u32 old;
    int ret = regmap_read(map, reg, &old);
    return ret ?: regmap_write(map, reg, (old & ~mask) | (value & mask));
}
#define regmap_read_poll_timeout(map, reg, value, condition, delay, limit) \
    (regmap_read(map, reg, &value) ?: ((condition) ? 0 : -ETIMEDOUT))
static int vdec_enable(struct aml_vdec_hw *h)
{
    assert(!bus_refs);
    if (backend()) return -EIO;
    bus_refs++; h->powered = true; return 0;
}
static int t7_hevc_power_on(struct aml_vdec_hw *h)
{
    assert(bus_refs && !hevc_refs);
    if (backend()) return -EIO;
    hevc_refs++; return 0;
}
static void t7_power_fault(struct aml_vdec_hw *h) { h->faulted = true; }
static int t7_hevc_power_off(struct aml_vdec_hw *h)
{
    assert(!h->faulted);
    if (backend()) { h->faulted = true; return -EIO; }
    if (hevc_refs) hevc_refs--;
    return 0;
}
static int vdec_disable(struct aml_vdec_hw *h)
{
    assert(!h->faulted && !hevc_refs);
    if (backend()) { h->faulted = true; return -EIO; }
    bus_refs--; h->powered = false; return 0;
}
static void arm_smccc_smc(u32 call, u32 arg, u32 core, u32 x3, u32 x4,
                          u32 x5, u32 x6, u32 x7, struct arm_smccc_res *r)
{
    assert(bus_refs && hevc_refs && !hw.faulted);
    assert(!core && !x3 && !x4 && !x5 && !x6 && !x7);
    if (call == 0xb200000e) {
        assert(arg == 4 && !reset_count && !loads);
    } else {
        assert(call == 0xb200000f && arg == 17);
        assert(reset_count == 1 && !regs[DOS_SW_RESET3]);
        assert(!(regs[HEVC_ASSIST_AXI_CTRL] & (BIT(6) | BIT(14))));
        loads++;
    }
    r->a0 = backend() ? 0xffff0000 : 0;
}
''' + code + r'''
static void reset(unsigned int failure)
{
    memset(&hw, 0, sizeof(hw));
    memset(regs, 0, sizeof(regs));
    hw.has_hevc = hw.hevc_test_passed = true;
    front_ack = back_ack = BIT(15) | BIT(11);
    fail_at = failure;
    calls = stuck_reg = bus_refs = hevc_refs = reset_count = loads = 0;
    stop_after_load = accesses_after_fault = 0;
}
int main(void)
{
    reset(0);
    assert(!aml_vdec_t7_hevc_core_test(&hw));
    unsigned int successful_calls = calls;
    assert(hw.hevc_core_test_passed && loads == 1 && stop_after_load == 1);
    assert(!bus_refs && !hevc_refs);
    for (unsigned int i = 1; i <= successful_calls; i++) {
        reset(i);
        assert(aml_vdec_t7_hevc_core_test(&hw) < 0);
        assert(!hw.hevc_core_test_passed && !accesses_after_fault);
        if (hw.faulted) assert(bus_refs == 1);
        else {
            assert(!bus_refs && !hevc_refs);
            if (loads) assert(stop_after_load == 1);
        }
    }
    const u32 dma_regs[] = { HEVC_IMEM_DMA_CTRL, HEVC_LMEM_DMA_CTRL, HEVC_WRRSP_LMEM };
    for (unsigned int i = 0; i < 3; i++) {
        reset(0); stuck_reg = dma_regs[i];
        assert(aml_vdec_t7_hevc_core_test(&hw) == -ETIMEDOUT);
        assert(hw.faulted && hevc_refs && bus_refs && !reset_count && !loads);
    }
    const u32 incomplete[] = { 0, BIT(15), BIT(11) };
    for (unsigned int i = 0; i < 3; i++) {
        reset(0); front_ack = incomplete[i];
        assert(aml_vdec_t7_hevc_core_test(&hw) == -ETIMEDOUT);
        assert(hw.faulted && hevc_refs && bus_refs && !reset_count && !loads);
        reset(0); back_ack = incomplete[i];
        assert(aml_vdec_t7_hevc_core_test(&hw) == -ETIMEDOUT);
        assert(hw.faulted && hevc_refs && bus_refs && !reset_count && !loads);
    }
    reset(0); hw.hevc_test_passed = false;
    assert(aml_vdec_t7_hevc_core_test(&hw) == -EBUSY && !calls);
    reset(0); hw.probe_failed = true;
    assert(aml_vdec_t7_hevc_core_test(&hw) == -EIO && !calls);
    reset(0); hw.powered = true;
    assert(aml_vdec_t7_hevc_core_test(&hw) == -EBUSY && !calls);
    return 0;
}
''')
