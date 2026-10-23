#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the TX bridge lookup with deferred probes and devres failures."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('meson_hdmitx21.c').read_text()
release = source[source.index('static void meson_hdmitx21_put_bridge('):
                 source.index('static int meson_hdmitx21_probe(')]
lookup = source[source.index('\ttx->next_bridge = of_drm_get_bridge_by_endpoint('):
                source.index('\ttx->ddc.owner = THIS_MODULE;')]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#define EPROBE_DEFER 517
#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define PTR_ERR(ptr) ((int)(intptr_t)(ptr))
#define IS_ERR(ptr) ((uintptr_t)(ptr) >= (uintptr_t)-4095)
struct device { void *of_node; };
struct drm_bridge { int refs; };
struct meson_hdmitx21 { struct drm_bridge *next_bridge; };
static struct drm_bridge bridge;
static int lookup_error, action_error;
static void (*action)(void *);
static void *action_data;
static struct drm_bridge *of_drm_get_bridge_by_endpoint(void *node, int port, int endpoint)
{
    assert(node && port == 1 && endpoint == 0);
    if (lookup_error)
        return ERR_PTR(lookup_error);
    bridge.refs++;
    return &bridge;
}
static void drm_bridge_put(struct drm_bridge *value)
{
    assert(value == &bridge && bridge.refs == 1);
    bridge.refs--;
}
static int devm_add_action_or_reset(struct device *dev, void (*release)(void *), void *data)
{
    assert(dev && !action);
    if (action_error) {
        release(data);
        return action_error;
    }
    action = release;
    action_data = data;
    return 0;
}
'''
harness += release
harness += '''
static int probe(struct device *dev, struct meson_hdmitx21 *tx)
{
    int ret;
''' + lookup + '''
    return 0;
}
'''
harness += r'''
int main(void)
{
    struct device dev = { .of_node = &dev };
    struct meson_hdmitx21 tx;
    lookup_error = -EPROBE_DEFER;
    assert(probe(&dev, &tx) == -EPROBE_DEFER && !bridge.refs && !action);
    lookup_error = -EINVAL;
    assert(probe(&dev, &tx) == -EINVAL && !bridge.refs && !action);
    lookup_error = -ENODEV;
    assert(!probe(&dev, &tx) && !tx.next_bridge && !bridge.refs && !action);
    lookup_error = 0;
    action_error = -ENOMEM;
    assert(probe(&dev, &tx) == -ENOMEM && !bridge.refs && !action);
    action_error = 0;
    assert(!probe(&dev, &tx) && tx.next_bridge == &bridge && bridge.refs == 1 && action);
    action(action_data);
    assert(!bridge.refs);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    (root / 'bridge.c').write_text(harness)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(root / 'bridge.c'), '-o', str(root / 'bridge')], check=True)
    subprocess.run([str(root / 'bridge')], check=True)
