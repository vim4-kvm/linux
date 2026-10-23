#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare T7 CEC pad/filter programming with the vendor CECA implementation."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    start = re.search(r'(?m)^(?:static )?(?:void|int|u32)\s+' + name + r'\(', source).start()
    brace = source.index('{', start)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if not depth:
            return source[start:end + 1]
    raise AssertionError(name)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', required=True, type=Path)
parser.add_argument('--dtb', type=Path)
args = parser.parse_args()
source = Path(__file__).with_name('ao-cec.c').read_text()
vendor = (args.vendor / 'drivers/media/cec/hdmi_aocec_api.c').read_text()
# The branch uses the A1-and-later register group selected by cec_t7_data.
table = re.search(r'top_reg_tab.*?=\s*\{(.*?)\n};', vendor, re.S)[1]
offsets = dict((name, int(value, 16)) for value, name in re.findall(
    r'\{[^,]+,\s*(0x[0-9a-f]+)}\s*,?/\*(AO_CEC\w+)\*/', table))
assert [offsets[key] for key in ('AO_CEC_GEN_CNTL', 'AO_CEC_RW_REG',
        'AO_CEC_INTR_MASKN', 'AO_CEC_INTR_CLR', 'AO_CEC_INTR_STAT')] == list(range(0, 20, 4))
assert offsets['AO_CECB_GEN_CNTL'] == 0x40
init = function(vendor, 'ao_ceca_init')
start = init.rindex('if (cec_dev->plat_data->ee_to_ao) {')
init = init[init.index('{', start) + 1:init.index('} else {', start)]
defines = '\n'.join(line for line in source.splitlines()
                    if line.startswith('#define CEC_GEN_CNTL'))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h))))
#define FIELD_PREP(mask, value) (((value) << __builtin_ctz(mask)) & (mask))
static u32 regs[32], reference[32];
struct meson_ao_cec_device {
    u32 *base, *cecb_route;
    u32 cecb_route_saved;
};
#define readl_relaxed(p) (*(u32 *)(p))
#define writel_relaxed(v,p) (*(u32 *)(p) = (v))
#define writel_bits_relaxed(mask,val,p) writel_relaxed((readl_relaxed(p) & ~(mask)) | (val), p)
#define AO_CEC_GEN_CNTL 0
#define AO_CECB_GEN_CNTL 16
#define CEC_A 0
#define CEC_ERR(...) ((void)0)
static void write_ao(unsigned int word, u32 value) {reference[word] = value;}
static void cec_set_reg_bits(unsigned int word, u32 value, int start, int width)
{
    u32 mask = ((1U << width) - 1) << start;
    reference[word] = (reference[word] & ~mask) | (value << start);
}
'''
body = prefix + defines + '\n'
body += function(source, 'meson_ao_cec_t7_setup') + '\n'
body += function(source, 'meson_ao_cec_t7_restore_route') + '\n'
body += function(vendor, 'cec_ip_share_io') + '\n'
body += 'static void vendor_init(void) {unsigned long data32;\n' + init + '}\n'
body += r'''
int main(void)
{
    struct meson_ao_cec_device cec = {.base=regs, .cecb_route=regs+16};
    const u32 mask = CEC_GEN_CNTL_FILTER_DELAY | CEC_GEN_CNTL_FILTER_TICK |
                     CEC_GEN_CNTL_SYS_CLK_EN | CEC_GEN_CNTL_SHARE_IO;
    for (unsigned int seed = 0; seed < 256; seed++) {
        u32 original = seed * 0x01010101U;
        regs[0] = original;
        regs[16] = reference[16] = ~original;
        cec.cecb_route_saved = regs[16] & CEC_GEN_CNTL_SHARE_IO;
        vendor_init();
        cec_ip_share_io(false, CEC_A);
        meson_ao_cec_t7_setup(&cec);
        assert((regs[0] & mask) == (reference[0] & mask));
        assert((regs[0] & ~mask) == (original & ~mask));
        assert(regs[16] == reference[16]);
        /* Restore only routing: preserve changes to unrelated CECB fields. */
        regs[16] ^= BIT(9);
        u32 before_restore = regs[16];
        meson_ao_cec_t7_restore_route(&cec);
        assert((regs[16] & ~CEC_GEN_CNTL_SHARE_IO) ==
               (before_restore & ~CEC_GEN_CNTL_SHARE_IO));
        assert((regs[16] & CEC_GEN_CNTL_SHARE_IO) == cec.cecb_route_saved);
    }
}
'''
with tempfile.TemporaryDirectory() as work:
    path = Path(work)
    (path / 'cec.c').write_text(body)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'cec.c'), '-o', str(path / 'cec')], check=True)
    subprocess.run([str(path / 'cec')], check=True)
print('T7 CECA: 256 vendor filter/routing combinations and masked restore passed')
if args.dtb:
    def prop(node, key, kind='x'):
        return subprocess.check_output(['fdtget', '-t', kind, str(args.dtb), node, key],
                                       text=True).strip()
    node = '/cec@fe044000'
    assert prop(node, 'compatible', 's') == 'amlogic,t7-ao-cec'
    assert prop(node, 'reg') == '0 fe044000 0 14 0 fe044040 0 4'
    assert prop(node, 'interrupts') == '0 b3 1'
    assert prop(node, 'clock-names', 's') == 'core pclk'
    tx = '/soc/bus@fe000000/hdmi-tx@300000'
    assert prop(node, 'hdmi-phandle') == prop(tx, 'phandle')
    assert prop('/soc/bus@fe000000/pinctrl@4000/cec-a/mux', 'groups', 's') == 'cec_a'
    print('Compiled M3 DT: CECA IRQ, split resources, clocks, pin and TX notifier target passed')
