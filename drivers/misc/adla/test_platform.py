#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute the actual platform power functions with fault-injected host APIs."""
from pathlib import Path
import subprocess
import tempfile

SOURCE = Path(__file__).parent
platform = (SOURCE / 'drv/port/platform/linux/adlak_platform_config.c').read_text()
functions = platform[platform.index('int adlak_platform_set_clock('):]
supply = (SOURCE / 'drv/port/platform/linux/adlak_regulator.c').read_text()
supply = supply[supply.index('static void adlak_disable_supply('):]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#define WRITE_ONCE(x, v) ((x) = (v))
struct clk { int enabled; struct clk *parent; };
struct device { int usage, enabled; };
struct adlak_device {
 struct device *dev;
 struct clk *clk, *clk_core, *clk_parent0;
 bool is_clk_core_enabled, is_suspend;
 int clk_core_freq_real, clk_axi_freq_real, power_error;
 int clk_core_freq_set, clk_axi_freq_set;
 void *sem_pm_wakeup;
};
static int fail_at, calls, semaphore, active_dma;
static int fault(void) { return ++calls == fail_at ? -EIO : 0; }
static struct clk *clk_get_parent(struct clk *c) { return c->parent; }
static int clk_set_parent(struct clk *c, struct clk *p) {
 int r=fault(); if (!r) c->parent=p; return r;
}
static int clk_set_rate(struct clk *c, int r) { (void)c; (void)r; return fault(); }
static int clk_prepare_enable(struct clk *c) {
 int r=fault(); if (!r) c->enabled++; return r;
}
static void clk_disable_unprepare(struct clk *c) { assert(c->enabled>0); c->enabled--; }
static int clk_get_rate(struct clk *c) { (void)c; return 800000000; }
static void adlak_dpm_clk_update(void *d, int c, int a) { (void)d; (void)c; (void)a; }
static int pm_runtime_resume_and_get(struct device *d) {
 int r=fault(); if (!r) d->usage++; return r;
}
static void pm_runtime_put_sync(struct device *d) { assert(d->usage>0); d->usage--; }
static void pm_runtime_put_autosuspend(struct device *d) { pm_runtime_put_sync(d); }
static void pm_runtime_mark_last_busy(struct device *d) { (void)d; }
static void pm_runtime_set_autosuspend_delay(struct device *d,int t) { (void)d; (void)t; }
static void pm_runtime_use_autosuspend(struct device *d) { (void)d; }
static void pm_runtime_dont_use_autosuspend(struct device *d) { (void)d; }
static void pm_runtime_enable(struct device *d) { d->enabled++; }
static void pm_runtime_disable(struct device *d) { assert(d->enabled>0); d->enabled--; }
static void pm_runtime_barrier(struct device *d) { (void)d; }
static int adlak_os_sema_init(void **s,int a,int b) { (void)s; (void)a; (void)b; semaphore++; return 0; }
static void adlak_os_sema_destroy(void **s) { (void)s; assert(semaphore>0); semaphore--; }
static void adlak_hw_dev_suspend(void *d) { (void)d; assert(!active_dma); }
struct regulator { int enabled; };
static struct regulator regulator;
static void (*release_supply)(void *);
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define PTR_ERR(p) ((long)(p))
static struct regulator *devm_regulator_get(struct device *d,const char *n) {
 (void)d; (void)n; return fault() ? (void *)(intptr_t)-EIO:&regulator;
}
static int regulator_set_voltage(struct regulator *r,int low,int high) {
 (void)r; assert(low==890000 && high==890000); return fault();
}
static int regulator_enable(struct regulator *r) {
 int err=fault(); if(!err) r->enabled++; return err;
}
static int regulator_disable(struct regulator *r) {
 assert(r->enabled>0); r->enabled--; return 0;
}
static int devm_add_action_or_reset(struct device *d,void (*fn)(void *),void *r) {
 (void)d; int err=fault(); if(err) fn(r); else release_supply=fn; return err;
}
'''
test = r'''
int main(void) {
 for (int stage=1; stage<=5; stage++) {
  struct device dev={0};
  struct clk parent={0}, previous={0}, mux={.parent=&previous}, core={0};
  struct adlak_device adla={.dev=&dev, .clk=&mux, .clk_core=&core,
   .clk_parent0=&parent, .clk_core_freq_set=800000000};
  calls=0; fail_at=stage;
  int result=adlak_platform_pm_init(&adla);
  if (stage<=4) {
   assert(result==-EIO && adla.is_suspend && !core.enabled);
   assert(!dev.usage && !dev.enabled && !semaphore);
   if (stage>=2) assert(mux.parent==&previous);
  } else {
   assert(!result && dev.usage==1 && core.enabled==1);
   adlak_platform_suspend(&adla);
   assert(!dev.usage && !core.enabled);
   calls=0; fail_at=1;
   assert(adlak_platform_resume(&adla)==-EIO);
   assert(!dev.usage && adla.is_suspend && !core.enabled);
   calls=0; fail_at=0;
   assert(!adlak_platform_resume(&adla));
   assert(dev.usage==1 && core.enabled==1);
   adlak_platform_pm_deinit(&adla);
   assert(!dev.usage && !dev.enabled && !core.enabled && !semaphore);
  }
 }
 for(int stage=1;stage<=5;stage++) {
  struct device dev={0}; struct adlak_device adla={.dev=&dev};
  calls=0; fail_at=stage; release_supply=0;
  int result=adlak_voltage_init(&adla);
  if(stage<=4) assert(result==-EIO && !regulator.enabled);
  else { assert(!result && regulator.enabled==1 && release_supply);
         release_supply(&regulator); assert(!regulator.enabled); }
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    (root / 'test.c').write_text(stubs + functions + supply + test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(root / 'test.c'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)

# The command emitter and invoke validation must select the same format.
submit = (SOURCE / 'drv/common/adlak_submit.c').read_text()
begin = submit.index('\tfor (int i = pinvoke_desc->start_idx;')
end = submit.index('\tif (++pmodel_attr->invoke_count', begin)
block = submit[begin:end]
bounds = (SOURCE / 'drv/common/adlak_bounds.h').read_text()
fixup_test = r'''
#include <assert.h>
#include <errno.h>
#define dev_err_ratelimited(...) ((void)0)
struct adlak_submit_task { int addr_fixup_offset, addr_fixup_count, config_size;
 struct { int common_size; } config_v2; };
struct adlak_submit_addr_fixup { int loc; unsigned int shift, unit; };
struct model { struct adlak_submit_task *submit_tasks;

 int addr_fixups_num;
 struct adlak_submit_addr_fixup *submit_addr_fixups; struct { int support; } cmd_buf_attr; };
struct invoke { int start_idx, end_idx, addr_fixups_num; };
static int validate(struct model *pmodel_attr, struct invoke *pinvoke_desc) {
 int ret=-EINVAL;
''' + block + r'''
 return 0;
err_copy_from_user:
 return ret;
}
int main(void) {
 struct adlak_submit_task task={.addr_fixup_count=1,.config_v2.common_size=128};
 struct adlak_submit_addr_fixup fixup={.loc=124,.unit=16};
 struct model model={.submit_tasks=&task,.submit_addr_fixups=&fixup,
  .addr_fixups_num=1,.cmd_buf_attr.support=1};
 struct invoke invoke={.addr_fixups_num=1};
 assert(!validate(&model,&invoke));
 /* A zero-count INVOKE reuses the registered model's previous fixups. */
 invoke.addr_fixups_num=0;
 assert(!validate(&model,&invoke));
 task.addr_fixup_offset=1;
 assert(validate(&model,&invoke)==-EINVAL);
 task.addr_fixup_offset=0; model.addr_fixups_num=0;
 assert(validate(&model,&invoke)==-EINVAL);
 model.addr_fixups_num=1;
 fixup.loc=128; assert(validate(&model,&invoke)==-EINVAL);
 fixup.loc=124; fixup.unit=0; assert(validate(&model,&invoke)==-EINVAL);
 fixup.unit=16; model.cmd_buf_attr.support=0; task.config_size=128;
 assert(!validate(&model,&invoke));
 task.addr_fixup_count=0; task.addr_fixup_offset=999; invoke.addr_fixups_num=0;
 assert(!validate(&model,&invoke));
 return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    (root / 'fixups.c').write_text(bounds + fixup_test)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(root / 'fixups.c'),
                    '-o', str(root / 'fixups')], check=True)
    subprocess.run([str(root / 'fixups')], check=True)

# Firmware IRQ errors must preserve probe deferral before any hardware access.
source = Path(__file__).parent / 'drv/port/platform/linux/adlak_platform_config.c'
text = source.read_text()
start = text.index('\tret = platform_get_irq_byname(')
end = text.index('    AML_LOG_DEBUG("get IRQ number:', start)
block = text[start:end]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
struct resource { int irqline; };
struct device { void *pdev; struct resource hw_res; };
static int result;
static int platform_get_irq_byname(void *dev, const char *name)
{
    (void)dev;
    assert(!strcmp(name, "adla"));
    return result;
}
static int acquire(struct device *padlak)
{
    int ret;
''' + block + r'''
    return 0;
}
int main(void)
{
    struct device dev = {0};
    result = -517;
    assert(acquire(&dev) == -517 && dev.hw_res.irqline == 0);
    result = -ENXIO;
    assert(acquire(&dev) == -ENXIO && dev.hw_res.irqline == 0);
    result = 123;
    assert(acquire(&dev) == 0 && dev.hw_res.irqline == 123);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'test.c').write_text(harness)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(root / 'test.c'),
                    '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
