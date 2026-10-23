#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production power-cycle/reload sequence with faulted PM callbacks.

The model verifies ownership and ordering, not the secure firmware or DMA bus.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / 'wave5-vpu.c').read_text()
sequence = source[source.index('static int wave5_power_notify('):
                  source.index('static __maybe_unused int wave5_pm_suspend(')]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,x) ((v)=(x))
#define lockdep_assert_held(p) assert(*(p))
#define NOTIFY_OK 0
#define GENPD_NOTIFY_OFF 1
#define GENPD_NOTIFY_ON 2
#define SW_RESET_ON_BOOT 2
#define U32_MAX UINT32_MAX
#define W5_VPU_VINT_REASON_CLR 1
#define W5_VPU_VINT_REASON_USR 2
#define W5_VPU_VINT_CLEAR 3
#define W5_INIT_VPU 1
#define xchg(p,v) ({ unsigned old=*(p); *(p)=(v); old; })
static unsigned recovery_fail_step;
struct notifier_block { int unused; };
struct firmware { const u8 *data; size_t size; };
struct vpu_device {
    int hw_lock;
    struct vpu_device *dev;
    const struct firmware *firmware;
    bool power_off_seen, power_on_seen;
    struct notifier_block power_notifier;
};
static int stage, refs, resets, offs, ons, inits, nested;
static bool powered;
static const u8 code[] = {1,2,3,4};
static const struct firmware firmware = {code,sizeof(code)};
static int wave5_power_notify(struct notifier_block *, unsigned long, void *);
static void wave5_vpu_isolate(struct vpu_device *dev, const char *reason, unsigned cmd)
{ (void)dev; (void)reason; assert(cmd==W5_INIT_VPU); nested++; }
static int wave5_vpu_reset(struct vpu_device *dev, int mode)
{ (void)dev; assert(powered && mode==SW_RESET_ON_BOOT); resets++; return stage==2 ? -ETIMEDOUT : 0; }
static int pm_runtime_put_sync_suspend(struct vpu_device *dev)
{
    assert(resets==1 && refs==1 && powered); refs--; offs++;
    if (stage==3) return -EBUSY;
    if (stage==4) return 0; /* genpd governor declined power-off. */
    powered=false;
    wave5_power_notify(&dev->power_notifier,GENPD_NOTIFY_OFF,NULL);
    return 0;
}
static void pm_runtime_get_noresume(struct vpu_device *dev) { (void)dev; refs++; }
static int pm_runtime_resume_and_get(struct vpu_device *dev)
{
    assert(offs==1 && dev->power_off_seen && !powered && !refs); ons++;
    if (stage==5) return -EIO;
    powered=true; refs++;
    if (stage!=6) wave5_power_notify(&dev->power_notifier,GENPD_NOTIFY_ON,NULL);
    return 0;
}
static bool wave5_vpu_is_init(struct vpu_device *dev)
{ (void)dev; assert(powered && ons==1); return stage==7; }
static int wave5_vpu_init(struct vpu_device *dev, u8 *data, size_t size)
{
    assert(powered && dev->power_off_seen && dev->power_on_seen && refs==1);
    assert(data==code && size==sizeof(code)); inits++;
    return stage==8 ? -ETIMEDOUT : 0;
}
static void vpu_write_reg(struct vpu_device *dev, unsigned reg, unsigned value)
{ (void)dev; (void)reg; (void)value; assert(inits==1 && !stage); }
'''
checks = r'''
int main(void)
{
    const int expected[] = {0,-EINVAL,-ETIMEDOUT,-EBUSY,-EBUSY,-EIO,-EIO,-EIO,-ETIMEDOUT};
    for (stage=0; stage<9; stage++) {
        struct vpu_device dev={.hw_lock=1,.firmware=stage==1 ? NULL : &firmware};
        dev.dev=&dev; refs=1; powered=true;
        resets=offs=ons=inits=0;
        /* Stale notifier observations must never authorize a fresh cycle. */
        dev.power_off_seen=dev.power_on_seen=true;
        assert(wave5_vpu_recover_hardware(&dev)==expected[stage]);
        assert(refs==1);
        assert(inits==(stage==0 || stage==8));
        if (stage==1) assert(!resets && !offs && !ons);
        if (stage==2) assert(!offs && !ons);
        if (stage==3 || stage==4) assert(!ons);
    }
    for (unsigned fault=1; fault<=5; fault++) {
        struct vpu_device dev={.hw_lock=1,.firmware=&firmware};
        dev.dev=&dev; refs=1; powered=true; stage=0;
        resets=offs=ons=inits=nested=0; recovery_fail_step=fault;
        assert(wave5_vpu_recover_hardware(&dev)<0);
        assert(refs==1 && !inits && !recovery_fail_step);
        assert(nested==(fault==5));
        if (fault==1) assert(!resets);
        if (fault<=2) assert(!offs);
        if (fault==3) assert(!ons && !powered);
    }
    puts("Wave5 recovery: cold reload, 8 failure boundaries and 5 one-shot injections passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-recovery-') as directory:
    path = Path(directory)
    (path / 'recovery.c').write_text(prefix + sequence + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(path / 'recovery.c'), '-o', str(path / 'recovery')], check=True)
    subprocess.run([str(path / 'recovery')], check=True, timeout=10)
