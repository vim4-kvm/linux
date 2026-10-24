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
#include <errno.h>
#include <stddef.h>
typedef uint32_t u32;
typedef uint8_t u8;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h))))
#define FIELD_PREP(mask, value) (((value) << __builtin_ctz(mask)) & (mask))
static u32 regs[32], reference[32];
struct meson_ao_cec_device {
    u32 *base, *cecb_route;
    u32 cecb_route_saved;
    void *notify, *adap, *core;
    struct platform_device *pdev;
    int irq;
};
struct device {int unused;};
struct platform_device {struct meson_ao_cec_device *driver_data; struct device dev;};
struct cec_adapter {void *priv;};
#define CEC_LOG_ADDR_INVALID 255
#define CEC_LOGICAL_ADDR0 0x16
#define CEC_LOGICAL_ADDR1 0x17
#define LOGICAL_ADDR_DISABLE 0
#define LOGICAL_ADDR_MASK 0xf
#define LOGICAL_ADDR_VALID 0x10
static u8 command[256];
static int write_error, write_count;
static int meson_ao_cec_clear(struct meson_ao_cec_device *cec) {(void)cec; return 0;}
static void udelay(unsigned int delay) {(void)delay;}
static void meson_ao_cec_write(struct meson_ao_cec_device *cec,
                              unsigned long address, u8 value, int *ret)
{
    (void)cec;
    if (*ret) return;
    write_count++;
    if (write_error) *ret=write_error;
    else command[address]=value;
}
static unsigned int aocec_rd_reg(unsigned int address) {return reference[address];}
static void aocec_wr_reg(unsigned int address, unsigned int value) {reference[address]=value;}
#define CONFIG_AMLOGIC_ZAPPER_CUT 1
#define CEC_CHIP_A1 0
static struct {int chip_id;} platform={0};
static struct {void *unused; __typeof__(platform) *plat_data;} device={0,&platform}, *cec_dev=&device;
static bool clock_live=true, notifier_live=true, adapter_live=true, irq_live=true;
static int probe_failure;
static struct meson_ao_cec_device *platform_get_drvdata(struct platform_device *pdev)
{return pdev->driver_data;}
static void platform_set_drvdata(struct platform_device *pdev, void *data)
{pdev->driver_data=data;}
static void cec_notifier_cec_adap_unregister(void *notify, void *adap)
{(void)notify; (void)adap; assert(adapter_live); notifier_live=false;}
static void cec_unregister_adapter(void *adap)
{(void)adap; assert(clock_live && !notifier_live && !irq_live); adapter_live=false;}
static void cec_delete_adapter(void *adap)
{(void)adap; assert(!irq_live && !clock_live); adapter_live=false;}
static void clk_disable_unprepare(void *clk)
{(void)clk; assert(!irq_live); clock_live=false;}
static void devm_free_irq(struct device *dev, int irq, void *data)
{(void)dev; (void)irq; (void)data; assert(adapter_live && irq_live); irq_live=false;}
static void meson_ao_cec_irq_setup(struct meson_ao_cec_device *cec, bool enable)
{(void)cec; assert(clock_live); regs[2]=enable;}
static void meson_ao_cec_irq(void) {}
static void meson_ao_cec_irq_thread(void) {}
static int devm_request_threaded_irq(struct device *dev, int irq, void (*top)(void),
                                     void (*thread)(void), int flags,
                                     void *name, void *data)
{
    (void)dev; (void)irq; (void)top; (void)thread; (void)flags; (void)name; (void)data;
    assert(clock_live && (regs[0] & 1) && !regs[2]);
    if (probe_failure == 1) return -EIO;
    irq_live=true;
    return 0;
}
static void *cec_notifier_cec_adap_register(struct device *hdmi, void *port, void *adap)
{
    (void)hdmi; (void)port; (void)adap;
    assert(irq_live);
    if (probe_failure == 2) return NULL;
    notifier_live=true;
    return &notifier_live;
}
static int cec_register_adapter(void *adap, struct device *dev)
{
    (void)adap; (void)dev;
    assert(irq_live && notifier_live && clock_live);
    if (probe_failure == 3) return -EIO;
    /* Deterministically open/configure as soon as the device is published. */
    regs[0]=0;
    regs[2]=3;
    return 0;
}
static int meson_ao_cec_adap_enable(void *adap, bool enable)
{(void)adap; assert(!enable && clock_live); regs[0]=1; regs[2]=0; return 0;}
#define dev_err(...) ((void)0)
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
body += function(source, 'meson_ao_cec_remove') + '\n'
body += function(source, 'meson_ao_cec_set_log_addr') + '\n'
probe = function(source, 'meson_ao_cec_probe')
body += 'static int probe_registration(struct platform_device *pdev) {\n'
body += 'struct meson_ao_cec_device *ao_cec=pdev->driver_data;\n'
body += 'struct device *hdmi_dev=NULL; int irq=ao_cec->irq, ret;\n'
body += probe[probe.index('\tao_cec->pdev = pdev;'):] + '\n'
body += function(vendor, 'cec_ip_share_io') + '\n'
body += function(vendor, 'ceca_addr_add') + '\n'
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
    struct platform_device pdev = {.driver_data=&cec};
    struct cec_adapter adap = {.priv=&cec};
    for (unsigned int addr = 0; addr < 16; addr++) {
        reference[CEC_LOGICAL_ADDR0] = reference[CEC_LOGICAL_ADDR1] = 0;
        ceca_addr_add(addr);
        command[CEC_LOGICAL_ADDR0] = command[CEC_LOGICAL_ADDR1] = 0xff;
        assert(meson_ao_cec_set_log_addr(&adap, addr) == 0);
        assert(command[CEC_LOGICAL_ADDR0] == reference[CEC_LOGICAL_ADDR0]);
        assert(command[CEC_LOGICAL_ADDR1] == reference[CEC_LOGICAL_ADDR1]);
    }
    assert(meson_ao_cec_set_log_addr(&adap, CEC_LOG_ADDR_INVALID) == 0);
    assert(!command[CEC_LOGICAL_ADDR0] && !command[CEC_LOGICAL_ADDR1]);
    assert(meson_ao_cec_set_log_addr(&adap, 16) == -EINVAL);
    write_error=-EIO; write_count=0;
    assert(meson_ao_cec_set_log_addr(&adap, 8) == -EIO && write_count == 1);
    write_error=0; cec.cecb_route=NULL;
    assert(meson_ao_cec_set_log_addr(&adap, 4) == 0);
    assert(command[CEC_LOGICAL_ADDR0] == (LOGICAL_ADDR_VALID | 4));
    meson_ao_cec_remove(&pdev);
    assert(!clock_live && !notifier_live && !adapter_live);
    for (probe_failure=0; probe_failure<4; probe_failure++) {
        clock_live=adapter_live=true;
        notifier_live=irq_live=false;
        regs[0]=0; regs[2]=3;
        int result=probe_registration(&pdev);
        if (!probe_failure) {
            assert(!result && regs[0] == 0 && regs[2] == 3);
            meson_ao_cec_remove(&pdev);
        } else {
            assert(result < 0 && !clock_live && !adapter_live && !irq_live);
            assert(!notifier_live && regs[0] == 1 && regs[2] == 0);
        }
    }
}
'''
with tempfile.TemporaryDirectory() as work:
    path = Path(work)
    (path / 'cec.c').write_text(body)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-label',
                    str(path / 'cec.c'), '-o', str(path / 'cec')], check=True)
    subprocess.run([str(path / 'cec')], check=True)
print('T7 CECA: vendor filters/routing/addresses, I/O failures, publication race and teardown passed')
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
