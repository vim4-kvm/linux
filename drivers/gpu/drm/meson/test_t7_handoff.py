#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the handoff IRQ and buffer retirement with controlled vsync delivery."""
from pathlib import Path
import argparse
import subprocess
import tempfile
import re


def initializer(source, name):
    start = source.index(name + ' = {') + len(name + ' = ')
    depth = 0
    for offset in range(start, len(source)):
        depth += (source[offset] == '{') - (source[offset] == '}')
        if not depth:
            return source[start:offset + 1]
    raise AssertionError('Unclosed initializer: ' + name)


# Absence of write callbacks is a handoff contract, including clock registration.
clock_dir = Path(__file__).resolve().parents[4] / 'drivers/clk/meson'
clock_source = (clock_dir / 't7-pll.c').read_text()
for name in ('t7_hdmi_pll_dco_ro', 't7_hdmi_pll_od_ro', 't7_hdmi_pll_ro'):
    clock = initializer(clock_source, name)
    assert 'CLK_SET_RATE_PARENT' not in clock, name
    ops_name = re.search(r'\.ops\s*=\s*&([a-z_]+)', clock)[1]
    ops_file = 'clk-pll.c' if ops_name == 'meson_clk_pll_ro_ops' else 'clk-regmap.c'
    ops = initializer((clock_dir / ops_file).read_text(), ops_name)
    callbacks = dict(re.findall(r'\.([a-z_]+)\s*=\s*([a-z_]+)', ops))
    assert callbacks.pop('init') == 'clk_regmap_init', callbacks
    assert set(callbacks) <= {'recalc_rate', 'determine_rate', 'is_enabled'}, callbacks
print('HDMI PLL handoff has no registration, enable, disable or rate write callbacks')

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dtb', type=Path, help='Also check the compiled VIM4 M1 resource profile')
args = parser.parse_args()
if args.dtb:
    def property_value(node, name, kind='s'):
        return subprocess.check_output(['fdtget', '-t', kind, str(args.dtb), node, name],
                                       text=True).strip()

    vdin = (clock_dir.parents[2] / 'drivers/media/platform/amlogic/t7-hdmirx/t7-vdin-test.c').read_text()
    regions = initializer(vdin, 'regions[]')
    capture = [(int(start, 16), int(size, 16))
               for start, size in re.findall(r'\{\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+)\s*}', regions)]
    assert capture
    cells = [int(cell, 16) for cell in property_value('/display@ff006840', 'reg', 'x').split()]
    assert len(cells) % 4 == 0
    display = [((cells[i] << 32) | cells[i + 1], (cells[i + 2] << 32) | cells[i + 3])
               for i in range(0, len(cells), 4)]
    for start, size in display:
        for capture_start, capture_size in capture:
            assert max(start, capture_start) >= min(start + size, capture_start + capture_size)
    assert property_value('/chosen/framebuffer@3fdeec00', 'status') == 'disabled'
    assert property_value('/vpu@ff000000', 'status') == 'disabled'
    assert property_value('/video-capture@ff000000', 'amlogic,split-vcbus') == ''
    assert property_value('/soc/bus@fe000000/clock-controller@81c0', 'compatible').endswith('-ro')
    print('Compiled M1 DT: disjoint capture/display claims, exclusive KMS and read-only PLL')

source = Path(__file__).with_name('meson_t7_handoff.c').read_text()
# The handoff must not configure legacy VENC interrupts or the global VIU mux.
assert len(re.findall(r'\bwritel\(', source)) == 1
assert 'display->osd + OSD_REG(VIU_OSD1_BLK1_CFG_W4)' in source
register_source = Path(__file__).with_name('meson_registers.h').read_text()
registers = '\n'.join(line for line in register_source.splitlines()
                      if line.startswith('#define ') and not line.endswith('\\'))
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define '))
types = source[source.index('struct meson_t7_handoff {'):source.index('static struct meson_t7_handoff *')]
functions = source[source.index('static irqreturn_t meson_t7_handoff_irq'):
                   source.index('static int meson_t7_handoff_plane_check')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t dma_addr_t;
typedef int spinlock_t;
typedef int wait_queue_head_t;
typedef int irqreturn_t;
struct drm_device { int unused; };
struct drm_plane { int unused; };
struct drm_crtc { int unused; };
struct drm_encoder { int unused; };
struct drm_framebuffer { unsigned int references; };
#define __iomem
#define BIT(n) (1U << (n))
#define IRQ_HANDLED 1
#define spin_lock_irqsave(lock, flags) ((void)(lock), (void)((flags) = 0))
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define READ_ONCE(value) (value)
#define WRITE_ONCE(target, value) ((target) = (value))
#define msecs_to_jiffies(value) (value)
#define drm_err(...) ((void)0)
static bool inject_timeout;
static u32 osd[32], venc[66];
static unsigned int writes, vblanks;
static u32 readl(void *address) { return *(u32 *)address; }
static void writel(u32 value, void *address)
{
    /* This function must never write a VDIN, VPP or arbitration register. */
    assert(address == &osd[4]);
    *(u32 *)address = value;
    writes++;
}
static void drm_crtc_handle_vblank(struct drm_crtc *crtc) { (void)crtc; vblanks++; }
static void wake_up(wait_queue_head_t *wait) { (void)wait; }
static void drm_framebuffer_get(struct drm_framebuffer *fb) { fb->references++; }
static void drm_framebuffer_put(struct drm_framebuffer *fb)
{
    assert(fb->references > 1);
    fb->references--;
}
#define wait_event_timeout(wait, condition, timeout) ({ \
    assert((timeout) == 100); \
    assert(!(condition)); \
    meson_t7_handoff_irq(0, display); \
    assert(!(condition)); \
    if (!inject_timeout) meson_t7_handoff_irq(0, display); \
    assert((bool)(condition) == !inject_timeout); \
    !inject_timeout; \
})
'''
checks = r'''
int main(void)
{
    struct meson_t7_handoff display = { .osd = osd, .venc = venc };
    struct drm_framebuffer first = { 1 }, second = { 1 }, third = { 1 };

    meson_t7_handoff_irq(0, &display);
    assert(!writes);
    meson_t7_handoff_scanout(&display, &first, 0x20000000);
    assert(writes == 1 && first.references == 2 && osd[4] == 0x02000000);
    meson_t7_handoff_scanout(&display, &second, 0x21000000);
    assert(writes == 2 && first.references == 1 && second.references == 2);
    meson_t7_handoff_scanout(&display, NULL, HANDOFF_BOOT_ADDR);
    assert(writes == 3 && second.references == 1 && !display.scanout);
    assert(osd[4] == HANDOFF_BOOT_ADDR >> 4);
    /* Counter wrap must not retire the buffer before two real interrupts. */
    display.requested = display.written = display.applied = UINT32_MAX;
    meson_t7_handoff_scanout(&display, &first, 0x20000000);
    assert(!display.applied && first.references == 2);
    inject_timeout = true;
    meson_t7_handoff_scanout(&display, &second, 0x21000000);
    assert(display.faulted && display.scanout == &first && display.stalled == &second);
    assert(first.references == 2 && second.references == 2);
    unsigned int stopped_at = writes;
    meson_t7_handoff_scanout(&display, &third, 0x22000000);
    assert(writes == stopped_at && third.references == 1);
    /* A delayed IRQ cannot make the failed path drop either DMA reference. */
    meson_t7_handoff_irq(0, &display);
    assert(first.references == 2 && second.references == 2);
    puts("OSD address publication, two-vsync retirement, wrap and timeout pinning passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'handoff.c'
    executable = Path(directory) / 'handoff'
    code.write_text(prefix + registers + '\n' + defines + '\n' +
                    types + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-I', str(Path(__file__).parent),
                    str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
