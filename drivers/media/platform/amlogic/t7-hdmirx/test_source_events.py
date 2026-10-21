#!/usr/bin/env python3
"""Run the actual source monitor across mode/loss/recovery transitions."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-vdin-test.c').read_text()
functions = source[source.index('static void t7_vdin_source_work'):
                   source.index('static int t7_vdin_querycap')]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <linux/videodev2.h>
struct mutex { bool held; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct device { int unused; };
struct video_device { int unused; };
struct t7_vdin {
    struct delayed_work source_work; struct mutex video_lock;
    struct device *receiver; struct video_device video;
    bool source_seen,source_locked; unsigned int source_width,source_height;
};
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
#define msecs_to_jiffies(n) (n)
static unsigned int width=3840,height=2160,events,scheduled,canceled;
static int input_error=-ENOLINK;
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; }
static int t7_hdmirx_query_timings(struct device *dev, struct v4l2_dv_timings *mode,
                                  struct v4l2_fract *interval) {
    (void)dev; (void)interval;
    if (input_error) return input_error;
    mode->bt.width=width; mode->bt.height=height; return 0;
}
static void v4l2_event_queue(struct video_device *video, const struct v4l2_event *event) {
    (void)video;
    assert(event->type==V4L2_EVENT_SOURCE_CHANGE);
    assert(event->u.src_change.changes==V4L2_EVENT_SRC_CH_RESOLUTION); events++;
}
static void schedule_delayed_work(struct delayed_work *w, unsigned int delay) {
    (void)w; assert(delay==0 || delay==250); scheduled++;
}
static void cancel_delayed_work_sync(struct delayed_work *w) { (void)w; canceled++; }
'''
checks = r'''
int main(void) {
    struct t7_vdin receiver={0};
    t7_vdin_source_start(&receiver); assert(scheduled==1);
    t7_vdin_source_work(&receiver.source_work.work); assert(!events && receiver.source_seen);
    input_error=0; t7_vdin_source_work(&receiver.source_work.work);
    assert(events==1 && receiver.source_locked && receiver.source_width==3840);
    for (int i=0;i<100;i++) t7_vdin_source_work(&receiver.source_work.work);
    assert(events==1);
    width=1920; height=1080; t7_vdin_source_work(&receiver.source_work.work);
    assert(events==2 && receiver.source_width==1920 && receiver.source_height==1080);
    input_error=-EAGAIN; t7_vdin_source_work(&receiver.source_work.work);
    assert(events==2 && receiver.source_locked);
    input_error=-ERANGE; t7_vdin_source_work(&receiver.source_work.work);
    assert(events==3 && !receiver.source_locked);
    input_error=-ENOLINK; t7_vdin_source_work(&receiver.source_work.work); assert(events==3);
    input_error=0; width=3840; height=2160;
    t7_vdin_source_work(&receiver.source_work.work); assert(events==4 && receiver.source_locked);
    assert(!receiver.video_lock.held);
    t7_vdin_source_stop(&receiver); assert(canceled==1);
    puts("Source monitor: real mode/loss/recovery events, stable input and torn-counter retry passed");
}
'''
with tempfile.TemporaryDirectory(prefix='t7-source-events-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(fixture + functions + checks)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
