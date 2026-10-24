#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise system resume ordering and error unwind using the driver function."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[3] /
          'drivers/gpu/drm/panfrost/panfrost_device.c').read_text()
start = source.index('static int panfrost_device_resume(')
function = source[start:source.index('\n}', start) + 2]
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
#define GPU_PM_VREG_OFF 0
#define GPU_PM_CLK_DIS 1
#define IS_ERR(p) ((p) == &failed_opp)
#define PTR_ERR(p) (-ENOENT)
struct dev_pm_opp { int unused; };
static struct dev_pm_opp opp, failed_opp;
struct device { void *data; };
struct clk { int enabled; };
struct panfrost_compatible { unsigned int pm_features; };
struct panfrost_device {
    struct panfrost_compatible *comp;
    struct { unsigned long fast_rate; } pfdevfreq;
    struct clk *clock, *bus_clock;
};
static bool lookup_error;
static int opp_error, runtime_error, clock_error;
static int opp_puts, voltage_calls, runtime_calls, clock_calls;
static int sequence, voltage_order, clock_order, runtime_order;
static bool supply_on;
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static struct dev_pm_opp *dev_pm_opp_find_freq_ceil(struct device *dev,
                                                 unsigned long *rate)
{
    (void)dev; (void)rate;
    return lookup_error ? &failed_opp : &opp;
}
static int dev_pm_opp_set_opp(struct device *dev, struct dev_pm_opp *value)
{
    (void)dev;
    voltage_calls++; voltage_order = ++sequence;
    if (value && opp_error) return opp_error;
    supply_on = value != NULL;
    return 0;
}
static void dev_pm_opp_put(struct dev_pm_opp *value)
{
    assert(value == &opp); opp_puts++;
}
static int clk_enable(struct clk *clock)
{
    clock_calls++; clock_order = ++sequence;
    if (clock_calls == clock_error) return -EIO;
    clock->enabled++;
    return 0;
}
static void clk_disable(struct clk *clock) { clock->enabled--; }
static int pm_runtime_force_resume(struct device *dev)
{
    (void)dev; runtime_calls++; runtime_order = ++sequence;
    return runtime_error;
}
/* DRIVER_FUNCTION */
int main(void)
{
    struct clk core = {0}, bus = {0};
    struct panfrost_compatible comp = {
        .pm_features = BIT(GPU_PM_VREG_OFF) | BIT(GPU_PM_CLK_DIS),
    };
    struct panfrost_device gpu = {
        .comp = &comp, .clock = &core, .bus_clock = &bus,
        .pfdevfreq.fast_rate = 800000000,
    };
    struct device dev = {.data = &gpu};
    lookup_error = true;
    assert(panfrost_device_resume(&dev) == -ENOENT);
    assert(!opp_puts && !voltage_calls && !clock_calls && !runtime_calls);
    lookup_error = false; opp_error = -ERANGE;
    assert(panfrost_device_resume(&dev) == -ERANGE);
    assert(opp_puts == 1 && voltage_calls == 1 && !supply_on);
    assert(!clock_calls && !runtime_calls);
    opp_error = 0; clock_error = 2;
    assert(panfrost_device_resume(&dev) == -EIO);
    assert(!core.enabled && !bus.enabled && !supply_on && !runtime_calls);
    clock_error = 0; runtime_error = -ETIMEDOUT;
    assert(panfrost_device_resume(&dev) == -ETIMEDOUT);
    assert(!core.enabled && !bus.enabled && !supply_on);
    runtime_error = 0;
    assert(panfrost_device_resume(&dev) == 0);
    assert(core.enabled == 1 && bus.enabled == 1 && supply_on);
    assert(voltage_order < clock_order && clock_order < runtime_order);
    printf("Panfrost system resume ordering and failures: PASS\n");
}
'''.replace('/* DRIVER_FUNCTION */', function)
with tempfile.TemporaryDirectory(prefix='panfrost-pm-test-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', str(path / 'test.c'), '-o', str(path / 'test')],
                   check=True)
    subprocess.run([str(path / 'test')], check=True)
