#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual resume/expiry/suspend/fini functions against lifecycle stubs."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[3] /
          'drivers/gpu/drm/panfrost/panfrost_devfreq.c').read_text()
functions = []
for name in ('panfrost_devfreq_end_boost', 'panfrost_devfreq_resume',
             'panfrost_devfreq_suspend', 'panfrost_devfreq_fini'):
    start = source.index('static void ' + name) if name.endswith('end_boost') else source.index('void ' + name)
    functions.append(source[start:source.index('\n}', start) + 2])
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool pending; };
struct request { int floor; bool removed; };
struct panfrost_devfreq {
 void *devfreq, *cooling;
 struct request resume_boost;
 struct delayed_work resume_boost_work;
 unsigned long fast_rate;
};
struct panfrost_device { struct panfrost_devfreq pfdevfreq; };
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define system_wq NULL
#define msecs_to_jiffies(x) (x)
static struct panfrost_devfreq *active;
static void panfrost_devfreq_reset(struct panfrost_devfreq *p) { (void)p; }
static void devfreq_resume_device(void *p) { assert(p); }
static void devfreq_suspend_device(void *p) {
 assert(p && !active->resume_boost.floor && !active->resume_boost_work.pending);
}
static void dev_pm_qos_update_request(struct request *r, int v) {
 assert(!r->removed); r->floor=v;
}
static void dev_pm_qos_remove_request(struct request *r) {
 assert(!active->resume_boost_work.pending); r->removed=true;
}
static void cancel_delayed_work_sync(struct delayed_work *w) { w->pending=false; }
static void mod_delayed_work(void *q, struct delayed_work *w, int delay) {
 (void)q; assert(delay > 0); w->pending=true;
}
static void devfreq_cooling_unregister(void *p) { (void)p; }
/* FUNCTIONS */
int main(void) {
 struct panfrost_device gpu = {0};
 panfrost_devfreq_resume(&gpu); panfrost_devfreq_suspend(&gpu);
 panfrost_devfreq_fini(&gpu);
 active=&gpu.pfdevfreq; active->devfreq=active; active->fast_rate=800000000;
 panfrost_devfreq_resume(&gpu);
 assert(active->resume_boost.floor==800000 && active->resume_boost_work.pending);
 active->resume_boost_work.pending=false;
 panfrost_devfreq_end_boost(&active->resume_boost_work.work);
 assert(active->resume_boost.floor==0);
 panfrost_devfreq_resume(&gpu); panfrost_devfreq_suspend(&gpu);
 assert(!active->resume_boost_work.pending);
 panfrost_devfreq_resume(&gpu); panfrost_devfreq_fini(&gpu);
 assert(active->resume_boost.removed && !active->resume_boost_work.pending);
}
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path / 'fixture.c').write_text(fixture.replace('/* FUNCTIONS */', '\n'.join(functions)))
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(path / 'fixture.c'),
                    '-o', str(path / 'fixture')], check=True)
    subprocess.run([str(path / 'fixture')], check=True)
print('Panfrost resume boost lifecycle: PASS')
