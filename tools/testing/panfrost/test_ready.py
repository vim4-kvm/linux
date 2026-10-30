#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual two-phase readiness helper without MMIO access."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[3] /
          'drivers/gpu/drm/panfrost/panfrost_gpu.c').read_text()
start = source.index('static int panfrost_gpu_wait_ready(')
function = source[start:source.index('\n}', start) + 2]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t u32;
struct panfrost_device { void *iomem; };
static u32 values[2];
static unsigned calls[2], budgets[2];
#define POLL(addr, val, cond, delay, timeout, phase) ({ \
 (void)(addr); calls[phase]++; budgets[phase]=timeout; \
 assert((delay)==((phase)==0?1:10)); \
 val=values[phase]; (cond)?0:-ETIMEDOUT; })
#define readl_relaxed_poll_timeout_atomic(a,v,c,d,t) POLL(a,v,c,d,t,0)
#define readl_relaxed_poll_timeout(a,v,c,d,t) POLL(a,v,c,d,t,1)
/* FUNCTION */
int main(void) {
 struct panfrost_device gpu={0};
 values[0]=15;
 assert(panfrost_gpu_wait_ready(&gpu, 0, 15, 20000)==0);
 assert(calls[0]==1 && !calls[1] && budgets[0]==10);
 values[0]=0; values[1]=15;
 assert(panfrost_gpu_wait_ready(&gpu, 0, 15, 20000)==0);
 assert(calls[1]==1 && budgets[1]==19990);
 values[1]=31; /* Additional ready bits do not satisfy exact-mask readiness. */
 assert(panfrost_gpu_wait_ready(&gpu, 0, 15, 1000)==-ETIMEDOUT);
 assert(budgets[1]==990);
 values[1]=0;
 assert(panfrost_gpu_wait_ready(&gpu, 0, 15, 1000)==-ETIMEDOUT);
}
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path / 'fixture.c').write_text(fixture.replace('/* FUNCTION */', function))
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(path / 'fixture.c'),
                    '-o', str(path / 'fixture')], check=True)
    subprocess.run([str(path / 'fixture')], check=True)
print('Panfrost bounded readiness poll: PASS')
