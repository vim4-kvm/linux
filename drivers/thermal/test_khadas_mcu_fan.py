#!/usr/bin/env python3
"""Exercise the driver's actual quiet policy, fault handling and work lifecycle."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path(__file__).with_name('khadas_mcu_fan.c').read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define WRITE_ONCE(a, b) ((a) = (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define IS_ERR(p) ((p) == NULL)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
#define msecs_to_jiffies(ms) (ms)
#define dev_err_ratelimited(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
struct mutex { int unused; };
static void mutex_lock(struct mutex *lock) { (void)lock; }
static void mutex_unlock(struct mutex *lock) { (void)lock; }
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct device { void *data; };
struct device_attribute { int unused; };
static void *system_wq;
struct regulator { int unused; };
struct khadas_mcu { void *regmap; };
struct thermal_zone_device { int index; };
static int temperatures[6], missing = -1, unreadable = -1;
static int write_error, regulator_error, requested, reads, canceled, queued;
static int sequence, cancel_order, write_order, delay;
static struct thermal_zone_device zones[6];
/* SENSOR_NAMES */
static struct thermal_zone_device *thermal_zone_get_zone_by_name(const char *name)
{
    for (int i = 0; i < 6; i++)
        if (name == khadas_vim4_fan_sensors[i]) {
            zones[i].index = i;
            return i == missing ? NULL : &zones[i];
        }
    assert(false); return NULL;
}
static int thermal_zone_get_temp(struct thermal_zone_device *zone, int *temp)
{
    reads++; *temp = temperatures[zone->index];
    return unreadable == -2 || zone->index == unreadable ? -1 : 0;
}
static int regmap_write(void *map, unsigned int reg, unsigned int value)
{
    (void)map; (void)reg; requested = value; write_order = ++sequence;
    return write_error;
}
static void schedule_delayed_work(struct delayed_work *work, int milliseconds)
{
    (void)work; queued++; delay = milliseconds;
}
static void mod_delayed_work(void *queue, struct delayed_work *work, int milliseconds)
{
    (void)queue; schedule_delayed_work(work, milliseconds);
}
static void cancel_delayed_work_sync(struct delayed_work *work)
{
    (void)work; canceled++; cancel_order = ++sequence;
}
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static int regulator_enable(struct regulator *power) { (void)power; return regulator_error; }
static int regulator_disable(struct regulator *power) { (void)power; return regulator_error; }
'''
start = source.index('static const char * const khadas_vim4_fan_sensors[]')
prefix = prefix.replace('/* SENSOR_NAMES */',
                        source[start:source.index('\n};', start) + 3])
start = source.index('struct khadas_mcu_fan_ctx {')
context = source[start:source.index('\n};', start) + 3]
functions = '\n'.join(function(signature) for signature in (
    'static int khadas_vim4_fan_lerp(', 'static int khadas_vim4_fan_target_curve(', 'static int khadas_vim4_fan_target(',
    'static unsigned int khadas_vim4_fan_next(', 'static int khadas_mcu_fan_set_level(',
    'static void khadas_vim4_fan_work(', 'static void khadas_vim4_fan_stop(',
    'static int khadas_mcu_fan_suspend(', 'static int khadas_mcu_fan_resume(',
    'static ssize_t trigger_temperatures_store(',
))
checks = r'''
int main(void)
{
    int previous = 0;
    for (int temp = -20000; temp <= 130000; temp += 1000) {
        int speed = khadas_vim4_fan_target(temp, false);
        assert(speed >= previous && speed <= 100);
        assert(khadas_vim4_fan_target(temp, true) == max(speed, 15));
        if (temp < 45000) assert(speed == 0);
        if (temp >= 100000) assert(speed == 100);
        previous = speed;
    }
    assert(khadas_vim4_fan_target(45000, false) == 5);
    assert(khadas_vim4_fan_target(50000, false) == 15);
    assert(khadas_vim4_fan_target(55000, false) == 25);
    assert(khadas_vim4_fan_target(60000, false) == 35);
    assert(khadas_vim4_fan_target(80000, false) == 70);
    for (int current = 0; current <= 100; current++)
        for (int target = 0; target <= 100; target++) {
            int next = khadas_vim4_fan_next(current, target);
            if (!target || target == 100 || !current) assert(next == target);
            else {
                assert(next - current <= 5 && current - next <= 5);
                assert((next >= current && next <= target) ||
                       (next <= current && next >= target));
            }
        }
    struct khadas_mcu mcu = {0};
    struct khadas_mcu_fan_ctx ctx = {.mcu = &mcu, .quiet = true,
                                    .max_level = 100, .level = 100,
                                    .trigger_low = 60, .trigger_mid = 80, .trigger_high = 100};
    struct device dev = {.data = &ctx};
    int old_queued = queued;
    const char *invalid[] = {"34 60 90", "81 90 100", "60 64 100",
                            "60 80 84", "60 80 101", "60 80 100 junk",
                            "-1 80 100", "60 80", "4294967295 80 100"};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(trigger_temperatures_store(&dev, NULL, invalid[i], strlen(invalid[i])) == -EINVAL);
        assert(ctx.trigger_low == 60 && ctx.trigger_mid == 80 && ctx.trigger_high == 100);
        assert(queued == old_queued);
    }
    ctx.quiet_started = true;
    assert(trigger_temperatures_store(&dev, NULL, "55 75 95\n", 9) == 9);
    assert(ctx.trigger_low == 55 && ctx.trigger_mid == 75 && ctx.trigger_high == 95);
    assert(!ctx.quiet_started && queued == old_queued + 1 && delay == 0);
    ctx.trigger_low = 60; ctx.trigger_mid = 80; ctx.trigger_high = 100;
    queued = old_queued;
    struct regulator power = {0};
    for (int i = 0; i < 6; i++) temperatures[i] = 50000;
    missing = 5;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 15 && ctx.sensor_fault && !ctx.quiet_started);
    assert(delay == 1000);
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 15 && !ctx.quiet_started && delay == 1000);
    missing = -1; reads = 0;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 15 && !ctx.sensor_fault && ctx.quiet_started);
    assert(reads == 6 && delay == 30000);
    temperatures[5] = 90000;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 20 && ctx.temperature == 90000);
    unreadable = 5;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 15 && ctx.sensor_fault);
    assert(delay == 1000);
    /* A missing early sensor must not hide another zone's valid hot sample. */
    unreadable = -1; missing = 0;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 85 && ctx.sensor_fault && delay == 1000);
    temperatures[5] = 100000;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 100 && ctx.sensor_fault);
    /* All temperatures unknown: use the requested fallback and keep retrying. */
    missing = -1; unreadable = -2;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 15 && ctx.sensor_fault && delay == 1000);
    /* Recovery adopts a valid hot target immediately, without a 30s ramp. */
    unreadable = -1; temperatures[5] = 90000;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 85 && !ctx.sensor_fault && delay == 30000);
    unreadable = 5;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 15 && ctx.sensor_fault && delay == 1000);
    unreadable = -1; temperatures[5] = 50000;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 15 && !ctx.sensor_fault);
    assert(delay == 30000);
    temperatures[5] = 90000;
    write_error = -1;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 15 && requested == 20 && delay == 1000);
    write_error = 0; temperatures[2] = 100000;
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 100);
    /* A custom quiet curve delays ordinary cooling, never emergency cooling. */
    assert(khadas_vim4_fan_target_curve(49000, false, 65000, 85000, 100000) == 0);
    assert(khadas_vim4_fan_target_curve(65000, false, 65000, 85000, 100000) == 35);
    assert(khadas_vim4_fan_target_curve(100000, false, 65000, 85000, 100000) == 100);
    assert(khadas_vim4_fan_target_curve(49000, true, 65000, 85000, 100000) == 15);
    ctx.level = 20;
    khadas_vim4_fan_stop(&ctx);
    assert(ctx.level == 100 && cancel_order < write_order);
    int before = canceled;
    write_error = -1;
    assert(khadas_mcu_fan_suspend(&dev) == -1);
    assert(canceled == before + 1 && delay == 0);
    write_error = 0; ctx.power = &power; regulator_error = -1;
    assert(khadas_mcu_fan_suspend(&dev) == -1 && delay == 0);
    regulator_error = 0;
    assert(khadas_mcu_fan_suspend(&dev) == 0);
    int pending = queued;
    assert(khadas_mcu_fan_resume(&dev) == 0 && queued == pending + 1 && delay == 0);
    khadas_vim4_fan_work(&ctx.quiet_work.work);
    assert(ctx.level == 100);
    ctx.quiet = false; ctx.power = NULL; ctx.level = 42;
    assert(khadas_mcu_fan_suspend(&dev) == 0);
    assert(khadas_mcu_fan_resume(&dev) == 0 && requested == 42);
    puts("Khadas quiet fan policy, faults and lifecycle: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='khadas-fan-test-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + context + functions + checks)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
