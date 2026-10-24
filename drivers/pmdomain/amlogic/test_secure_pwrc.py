#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fault the actual secure-domain transition callbacks and polling macro."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / 'meson-secure-pwrc.c').read_text()
functions = source[source.index('static int meson_secure_pwrc_wait('):
                   source.index('static bool meson_secure_mempd_initially_off(')]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef int32_t s32;
typedef int64_t ktime_t;
typedef uint64_t u64;
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define SM_A1_PWRC_SET 1
#define SM_A1_PWRC_GET 2
#define PWRC_OFF 0
#define PWRC_ON 1
#define pr_err(...) ((void)0)
#define barrier() ((void)0)
#define cpu_relax() ((void)0)
#define might_sleep_if(x) ((void)(x))
static ktime_t now;
static ktime_t ktime_get(void) { return ++now; }
static ktime_t ktime_add_us(ktime_t t, u64 us) { return t+us; }
static int ktime_compare(ktime_t a, ktime_t b) { return (a>b)-(a<b); }
static void usleep_range(unsigned a, unsigned b) { (void)b; now+=a; }
struct generic_pm_domain { int unused; };
struct meson_secure_pwrc { void *fw; };
struct meson_secure_pwrc_domain {
    struct generic_pm_domain base;
    struct meson_secure_pwrc *pwrc;
    unsigned index;
    bool verify_state;
};
static unsigned sets, gets, target, mode;
static int meson_sm_call(void *fw, unsigned cmd, s32 *value, unsigned index,
                        unsigned state, unsigned a, unsigned b, unsigned c)
{
    (void)fw; (void)a; (void)b; (void)c; assert(index==5);
    if (cmd==SM_A1_PWRC_SET) {
        sets++; target=!state;
        if (value) *value=mode==6 ? -EACCES : mode==7 ? 1 : 0;
        return mode==1 ? -EINVAL : 0;
    }
    assert(cmd==SM_A1_PWRC_GET); gets++;
    if (mode==2) return -EINVAL;
    if (mode==3) *value=-1;
    else if (mode==4) *value=!target;
    else if (mode==5) *value=2; /* malformed secure response */
    else *value=gets<3 ? !target : target;
    return 0;
}
'''
checks = r'''
int main(void)
{
    struct meson_secure_pwrc controller={};
    struct meson_secure_pwrc_domain domain={.index=5,.pwrc=&controller,.verify_state=true};
    const int expected[]={0,-EINVAL,-EINVAL,-EIO,-ETIMEDOUT,-ETIMEDOUT};
    for (unsigned off=0; off<2; off++) {
        for (mode=0; mode<6; mode++) {
            sets=gets=0; now=0;
            int ret=off ? meson_secure_pwrc_off(&domain.base) : meson_secure_pwrc_on(&domain.base);
            assert(ret==expected[mode] && sets==1 && now<10100);
            if (mode==1) assert(!gets);
            if (mode==0) assert(gets==3);
        }
    }
    domain.verify_state=false; mode=4; sets=gets=0;
    assert(!meson_secure_pwrc_off(&domain.base) && sets==1 && !gets);
    /* SRAM SET must check the firmware payload and must never issue GET. */
    for (unsigned off=0; off<2; off++) {
        for (mode=0; mode<8; mode++) {
            sets=gets=0;
            int ret=off ? meson_secure_mempd_off(&domain.base) :
                          meson_secure_mempd_on(&domain.base);
            assert(ret==(mode==1 ? -EINVAL : mode==6 ? -EACCES : mode==7 ? -EIO : 0));
            assert(sets==1 && !gets && target==off);
        }
    }
    puts("Secure PWRC: 12 transition/failure cases and unmodified-domain behavior passed");
}
'''
with tempfile.TemporaryDirectory(prefix='secure-pwrc-') as directory:
    path = Path(directory)
    (path / 'linux').mkdir()
    for name in ('kernel', 'types', 'ktime', 'delay', 'io'):
        (path / 'linux' / f'{name}.h').write_text('/* supplied by harness */\n')
    poll = root.parents[2] / 'include/linux/iopoll.h'
    (path / 'pwrc.c').write_text(prefix + f'\n#include "{poll}"\n' + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(path),
                    str(path / 'pwrc.c'), '-o', str(path / 'pwrc')], check=True)
    subprocess.run([str(path / 'pwrc')], check=True, timeout=10)
