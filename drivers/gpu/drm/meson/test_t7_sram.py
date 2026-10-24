#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify drained OSD SRAM takeover, failure unwind and the vendor firmware ABI."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[4]
source = Path(__file__).with_name('meson_t7_handoff.c').read_text()
functions = source[source.index('static void meson_t7_handoff_domain_put('):
                   source.index('static void __iomem *meson_t7_handoff_map(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint32_t u32;
struct device { unsigned int id; bool attached, resumed; };
static struct device domains[15];
static void (*actions[15])(void *);
static void *action_data[15];
static unsigned int domain_count=10;
static unsigned int action_count, calls, fail_at, failure, ticks;
static bool timeout;
static u32 osd[3];
struct meson_t7_handoff {
    struct { struct device *dev; } drm;
    void *osd, *extra_osd[2];
    u32 vsync_count;
    int wait;
    void *video;
};
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define READ_ONCE(x) (x)
#define msecs_to_jiffies(x) (x)
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) > UINTPTR_MAX - 4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 value, void *p) { *(u32 *)p = value; }
static struct device *dev_pm_domain_attach_by_id(struct device *dev, unsigned int i)
{
    (void)dev;
    assert(i >= 3 && i < domain_count && ticks == 2);
    /* No SRAM transition is permitted while a boot OSD can still fetch. */
    assert(!(osd[0] & 1) && !(osd[1] & 1) && !(osd[2] & 1));
    calls++;
    if (i == fail_at && failure == 1) return NULL;
    if (i == fail_at && failure == 2) return (void *)(intptr_t)-EAGAIN;
    domains[i].id = i;
    domains[i].attached = true;
    return &domains[i];
}
static int pm_runtime_resume_and_get(struct device *dev)
{
    if (dev->id == fail_at && failure == 3) return -EIO;
    assert(!dev->resumed);
    dev->resumed = true;
    return 0;
}
static void dev_pm_domain_detach(struct device *dev, bool off)
{
    assert(off && dev->attached && !dev->resumed);
    dev->attached = false;
}
static void pm_runtime_put_sync(struct device *dev)
{
    assert(dev->resumed);
    dev->resumed = false;
}
static int devm_add_action_or_reset(struct device *dev, void (*action)(void *), void *data)
{
    struct device *pd = data;
    (void)dev;
    if (pd->id == fail_at && failure == 4) {
        action(data);
        return -ENOMEM;
    }
    actions[action_count] = action;
    action_data[action_count++] = data;
    return 0;
}
#define wait_event_timeout(wait, condition, ms) ({ \
    assert((ms) == 100 && !(condition)); \
    assert(!(osd[0] & 1) && !(osd[1] & 1) && !(osd[2] & 1)); \
    if (!timeout) { \
        display->vsync_count++; ticks++; assert(!(condition)); \
        display->vsync_count++; ticks++; assert(condition); \
    } \
    !timeout; \
})
'''
checks = r'''
static void unwind(void)
{
    while (action_count) {
        action_count--;
        actions[action_count](action_data[action_count]);
    }
    for (unsigned int i = 0; i < 10; i++)
        assert(!domains[i].attached && !domains[i].resumed);
}
int main(void)
{
    struct meson_t7_handoff display = {.osd=&osd[0], .extra_osd={&osd[1], &osd[2]}};
    const int errors[] = {0, -ENODEV, -EAGAIN, -EIO, -ENOMEM};
    for (domain_count=10;domain_count<=15;domain_count+=5) {
      display.video=domain_count==15 ? &display : NULL;
      for (failure = 0; failure < 5; failure++) {
        for (fail_at = 3; fail_at < domain_count; fail_at++) {
            osd[0] = osd[1] = osd[2] = 0xa5a50005;
            display.vsync_count = UINT32_MAX; /* Drain also spans counter wrap. */
            ticks = calls = 0;
            assert(meson_t7_handoff_sram_get(&display) == errors[failure]);
            assert(ticks == 2 && calls == (failure ? fail_at - 2 : domain_count-3));
            for (unsigned int i = 0; i < 3; i++) assert(osd[i] == 0xa5a50004);
            unwind();
        }
      }
    }
    timeout = true; ticks = calls = 0;
    assert(meson_t7_handoff_sram_get(&display) == -ETIMEDOUT);
    assert(!calls && !action_count); /* A missing vsync must not acquire or drop SRAM. */
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)

    def execute(name, contents):
        code, executable = path / f'{name}.c', path / name
        code.write_text(contents)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        str(code), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    execute('takeover', prefix + functions + checks)
    vendor_power = (args.vendor / 'drivers/power/sec_power_domain.c').read_text()
    domain_table = vendor_power.split('t7_pm_domains[]')[1].split('};')[0]
    indices = re.findall(r'\[(PDID_T7_\w+)\]', domain_table)
    vendor_smc = (args.vendor / 'drivers/power/sec_pwr_ctrl.c').read_text()
    vendor_function = vendor_smc[vendor_smc.index('unsigned long vpu_mempd_psci_smc('):
                                 vendor_smc.index('EXPORT_SYMBOL(vpu_mempd_psci_smc)')]
    pd_source = (root / 'drivers/pmdomain/amlogic/meson-secure-pwrc.c').read_text()
    names = re.findall(r'T7_MEM_PD\((\w+)\)',
                       pd_source.split('t7_pwrc_domains[] = {')[1].split('SEC_PD(')[0])
    vendor_modules = {
        'OSD1': 'VPU_VIU_OSD1', 'OSD2': 'VPU_VIU_OSD2',
        'VD1': 'VPU_VIU_VD1', 'VD2': 'VPU_VIU_VD2', 'OSD3': 'VPU_VIU_OSD3',
        'OSD_SCALE': 'VPU_VIU_OSD_SCALE', 'OSD2_SCALE': 'VPU_VD2_OSD2_SCALE',
        'MALI_AFBC': 'VPU_MAIL_AFBCD', 'VD1_SCALE': 'VPU_VD1_SCALE',
        'VD2_SCALE': 'VPU_VD2_SCALE', 'VD2_OFIFO': 'VPU_VD2_OFIFO',
        'OSD_BLD34': 'VPU_OSD_BLD34', 'RDMA': 'VPU_RDMA',
    }
    assert set(names) == vendor_modules.keys()
    abi = '#include <assert.h>\n#include <stdbool.h>\ntypedef unsigned int u32;\n'
    abi += f'#include "{args.vendor}/include/dt-bindings/power/t7-pd.h"\n'
    abi += f'#include "{args.vendor}/include/linux/amlogic/media/vpu/vpu.h"\n'
    abi += f'#include "{root}/include/dt-bindings/power/amlogic,t7-pwrc.h"\n'
    abi += 'static const char vendor_domains[] = {'
    abi += ','.join(f'[{index}]=1' for index in indices) + '};\n'
    abi += 'static int get_max_id(void) { return sizeof(vendor_domains); }\n'
    abi += 'static unsigned int smc_id, smc_state;\n'
    abi += 'static unsigned long pwr_ctrl_psci_smc(int id, bool on) '
    abi += '{ smc_id=id; smc_state=on; return 0; }\n' + vendor_function
    abi += 'int main(void) {\n'
    for name in names:
        abi += f'assert(!vpu_mempd_psci_smc({vendor_modules[name]}, true));\n'
        abi += f'assert(smc_id == PWRC_T7_{name}_MEM_ID && smc_state == 1);\n'
    abi += '}\n'
    execute('vendor-abi', abi)
print(f'OSD/video SRAM: 95 acquisition/unwind cases, missing vsync, wrap and {len(names)} vendor SMC IDs passed')
