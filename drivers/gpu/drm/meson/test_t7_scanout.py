#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real multi-layer DMA reference owner without hardware."""
from pathlib import Path
import subprocess
import tempfile


source = Path(__file__).with_name('meson_t7_scanout.h').read_text()
implementation = source[source.index('#define MESON_T7_MAX_LAYERS'):source.rindex('#endif')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
struct drm_framebuffer { unsigned int refs; };
static void drm_framebuffer_get(struct drm_framebuffer *fb) { fb->refs++; }
static void drm_framebuffer_put(struct drm_framebuffer *fb)
{
    assert(fb->refs > 1); /* The simulated exporter retains its own reference. */
    fb->refs--;
}
'''
checks = r'''
int main(void)
{
    struct meson_t7_scanout owner = {0};
    struct drm_framebuffer desktop = {1}, cursor = {1}, video = {1}, decoded = {1};
    struct drm_framebuffer *next[MESON_T7_MAX_LAYERS] = {
        &desktop, &cursor, &desktop, &video, &decoded
    };

    assert(!meson_t7_scanout_acquire(&owner, next));
    assert(desktop.refs == 3 && cursor.refs == 2 && video.refs == 2 && decoded.refs == 2);
    meson_t7_scanout_retire(&owner, true);
    /* Unchanged/shared FBs are acquired before ANY old layer is retired. */
    for (unsigned int frame = 0; frame < 120; frame++) {
        next[1] = frame & 1 ? &cursor : NULL;
        assert(!meson_t7_scanout_acquire(&owner, next));
        assert(desktop.refs == 5 && video.refs == 3 && decoded.refs == 3);
        meson_t7_scanout_retire(&owner, true);
        assert(desktop.refs == 3 && video.refs == 2 && decoded.refs == 2);
        assert(cursor.refs == (next[1] ? 2U : 1U));
    }
    /* RMFB and CRTC disable blank every layer in one transaction. */
    for (unsigned int i = 0; i < MESON_T7_MAX_LAYERS; i++) next[i] = NULL;
    assert(!meson_t7_scanout_acquire(&owner, next));
    assert(desktop.refs == 3 && video.refs == 2);
    meson_t7_scanout_retire(&owner, true);
    assert(desktop.refs == 1 && cursor.refs == 1 && video.refs == 1 && decoded.refs == 1);

    next[0] = &desktop; next[1] = &cursor; next[3] = &video;
    assert(!meson_t7_scanout_acquire(&owner, next));
    meson_t7_scanout_retire(&owner, true);
    next[0] = &decoded; next[1] = NULL; next[3] = &video; next[4] = &desktop;
    assert(!meson_t7_scanout_acquire(&owner, next));
    meson_t7_scanout_retire(&owner, false);
    assert(owner.faulted);
    assert(desktop.refs == 3 && cursor.refs == 2 && video.refs == 3 && decoded.refs == 2);
    /* Neither a delayed completion nor a later blank can release pinned DMA. */
    meson_t7_scanout_retire(&owner, true);
    assert(meson_t7_scanout_acquire(&owner, next) == -EIO);
    assert(desktop.refs == 3 && cursor.refs == 2 && video.refs == 3 && decoded.refs == 2);
    assert(owner.active[1] == &cursor && owner.pending[1] == NULL);
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'scanout.c'
    executable = Path(directory) / 'scanout'
    code.write_text(prefix + implementation + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('Five-layer retirement: shared buffers, 120 updates, RMFB/disable and timeout pinning passed')
