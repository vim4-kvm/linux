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
    tx_properties = subprocess.check_output(
        ['fdtget', '-p', str(args.dtb), '/soc/bus@fe000000/hdmi-tx@300000'], text=True).split()
    assert not any(name.startswith('pinctrl-') or name in
                   ('resets', 'reset-names', '#sound-dai-cells') for name in tx_properties)
    print('Compiled M1 DT: disjoint capture/display claims, exclusive KMS and read-only PLL')

source = Path(__file__).with_name('meson_t7_handoff.c').read_text()
# The handoff must not configure legacy VENC interrupts or the global VIU mux.
assert 'display->osd + OSD_REG(VIU_OSD1_BLK1_CFG_W4)' in source
register_source = Path(__file__).with_name('meson_registers.h').read_text()
registers = '\n'.join(line for line in register_source.splitlines()
                      if re.match(r'#define\s', line) and not line.endswith('\\'))
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define '))
types = source[source.index('struct meson_t7_handoff {'):source.index('static struct meson_t7_handoff *')]
retirement = Path(__file__).with_name('meson_t7_scanout.h').read_text()
types = retirement[retirement.index('#define MESON_T7_MAX_LAYERS'):
                   retirement.rindex('#endif')] + types
layers_header = Path(__file__).with_name('meson_t7_layers.h').read_text()
types = layers_header[layers_header.index('#define MESON_T7_OSD_LAYERS'):
                      layers_header.rindex('#endif')] + types
layers_source = Path(__file__).with_name('meson_t7_layers.c').read_text()
fb_source = Path(__file__).with_name('meson_t7_fb.h').read_text()
functions = fb_source[fb_source.index('static inline int meson_t7_fb_check'):
                      fb_source.rindex('#endif')]
functions += source[source.index('static u32 meson_t7_osd_alpha'):
                   source.index('/* Both interrupt orders')]
functions += source[source.index('static void meson_t7_handoff_rdma_complete'):
                   source.index('static int meson_t7_handoff_plane_check')]
functions += source[source.index('static int meson_t7_handoff_plane_check'):
                    source.index('static void meson_t7_handoff_plane_update')]
functions += source[source.index('static void meson_t7_handoff_abort_event'):
                    source.index('static int meson_t7_handoff_enable_vblank(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
typedef int spinlock_t;
typedef int wait_queue_head_t;
typedef int irqreturn_t;
struct drm_device { int event_lock; };
struct dma_fence { int error; bool signaled; };
struct completion { unsigned int completed, released; };
struct drm_pending_event {
    struct dma_fence *fence;
    struct completion *completion;
    void (*completion_release)(struct completion *);
};
struct drm_pending_vblank_event { struct drm_pending_event base; };
struct drm_plane_state;
struct drm_display_mode {int hdisplay, vdisplay, clock, htotal, vtotal;};
struct drm_rect {int x1,y1,x2,y2;};
static int drm_rect_width(const struct drm_rect *r) {return r->x2-r->x1;}
static int drm_rect_height(const struct drm_rect *r) {return r->y2-r->y1;}
struct drm_gem_dma_object {dma_addr_t dma_addr; struct {u64 size;} base;};
struct drm_crtc_state {
    bool active, modeset;
    struct drm_display_mode adjusted_mode;
    struct drm_pending_vblank_event *event;
};
static bool drm_atomic_crtc_needs_modeset(struct drm_crtc_state *state)
{return state->modeset;}
struct drm_plane { struct drm_device *dev; struct drm_plane_state *state; };
struct drm_crtc { struct drm_device *dev; struct drm_crtc_state *state; };
struct drm_encoder { int unused; };
struct drm_format_info { unsigned int format; unsigned int cpp[2]; unsigned int num_planes; };
static int drm_format_info_plane_width(const struct drm_format_info *f, int w, int p)
{return p ? w/2 : w;}
static int drm_format_info_plane_height(const struct drm_format_info *f, int h, int p)
{return p ? h/2 : h;}
struct drm_framebuffer {
    unsigned int references;
    const struct drm_format_info *format;
    unsigned int width, height, pitches[2], offsets[2];
    uint64_t modifier;
    struct drm_gem_dma_object *object;
    struct drm_gem_dma_object *objects[2];
};
struct drm_plane_state {
    bool visible;
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb;
    unsigned int src_x, src_y, src_w, src_h;
    unsigned int rotation;
    u16 alpha;
    int crtc_x, crtc_y, crtc_w, crtc_h;
    struct drm_rect src,dst;
    unsigned int normalized_zpos, pixel_blend_mode;
};
struct drm_atomic_commit {
    struct drm_plane_state *plane;
    struct drm_plane *planes[5];
    struct drm_plane_state *states[5];
    struct drm_crtc_state *old_crtc, *new_crtc;
};
#define DRM_FORMAT_RGB888 1
#define DRM_FORMAT_XRGB8888 2
#define DRM_FORMAT_XBGR8888 3
#define DRM_FORMAT_ARGB8888 4
#define DRM_FORMAT_ABGR8888 5
#define DRM_FORMAT_NV12 6
#define DRM_FORMAT_NV21 7
#define DRM_MODE_BLEND_PIXEL_NONE 0
#define DRM_MODE_BLEND_COVERAGE 1
#define DRM_MODE_REFLECT_X (1 << 4)
#define DRM_MODE_REFLECT_Y (1 << 5)
#define DRM_FORMAT_MOD_LINEAR 0
#define DRM_PLANE_NO_SCALING 1
#define U32_MAX UINT32_MAX
#define IS_ALIGNED(value, alignment) (!((value) & ((alignment) - 1)))
#define __iomem
#define BIT(n) (1U << (n))
#define IRQ_HANDLED 1
#define IRQ_NONE 0
#define GENMASK(h, l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31 - (h))))
#define dma_wmb() ((void)0)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define div_u64(a,b) ((u64)(a)/(b))
#define swap(a,b) do {__typeof__(a) t=a;a=b;b=t;} while(0)
#define fallthrough __attribute__((fallthrough))
#define spin_lock_irqsave(lock, flags) ((void)(lock), (void)((flags) = 0))
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define spin_lock_irq(lock) ((void)(lock))
#define spin_unlock_irq(lock) ((void)(lock))
#define READ_ONCE(value) (value)
#define WRITE_ONCE(target, value) ((target) = (value))
#define msecs_to_jiffies(value) (value)
#define drm_err(...) ((void)0)
static bool inject_timeout;
static bool fail_drain;
static bool native_writes;
static bool rdma_first;
static unsigned int timeout_part;
static u32 osd[32], venc[66];
static u32 rdma[64], table[1024], pipeline[0x6000], paths[2];
static unsigned int writes, vblanks;
static bool fail_modeset;
static unsigned int timing_stops;
struct meson_t7_modeset;
static int meson_t7_modeset_enable(struct meson_t7_modeset *hw, const void *mode)
{
    assert(!(osd[0] & 1)); /* Fetch must be blank before changing timing. */
    return fail_modeset ? -ETIMEDOUT : 0;
}
static void meson_t7_modeset_disable(struct meson_t7_modeset *hw)
{
    assert(!(osd[0] & 1)); /* Blank RDMA completed before stopping vsync. */
    timing_stops++;
}
static void drm_crtc_vblank_on(struct drm_crtc *crtc) {}
static void drm_crtc_vblank_off(struct drm_crtc *crtc) {}
static struct meson_t7_handoff *to_handoff(struct drm_device *drm);
static struct drm_plane_state *drm_atomic_get_new_plane_state(
    struct drm_atomic_commit *state, struct drm_plane *plane)
{
    for (unsigned int i=0;i<5;i++) if (state->planes[i]==plane) return state->states[i];
    return state->plane;
}
static struct drm_crtc_state *drm_atomic_get_new_crtc_state(
    struct drm_atomic_commit *state, struct drm_crtc *crtc) { return state->new_crtc; }
static struct drm_crtc_state *drm_atomic_get_old_crtc_state(
    struct drm_atomic_commit *state, struct drm_crtc *crtc) { return state->old_crtc; }
static int drm_atomic_helper_check_plane_state(struct drm_plane_state *plane,
    struct drm_crtc_state *crtc, int min_scale, int max_scale,
    bool can_position, bool can_update_disabled)
{
    /* DRM's helper clears copied visibility when the new framebuffer is NULL. */
    plane->visible = plane->fb != NULL;
    plane->src=(struct drm_rect){plane->src_x,plane->src_y,
        plane->src_x+plane->src_w,plane->src_y+plane->src_h};
    plane->dst=(struct drm_rect){plane->crtc_x,plane->crtc_y,
        plane->crtc_x+plane->crtc_w,plane->crtc_y+plane->crtc_h};
    return 0;
}
static struct drm_gem_dma_object *drm_fb_dma_get_gem_obj(struct drm_framebuffer *fb,
                                                      unsigned int index)
{
    static struct drm_gem_dma_object default_object={.dma_addr=0x20000000,
                                                    .base.size=64*1024*1024};
    assert(fb);
    if (fb->objects[index]) return fb->objects[index];
    return fb->object ? fb->object : &default_object;
}
static dma_addr_t drm_fb_dma_get_gem_addr(struct drm_framebuffer *fb,
    struct drm_plane_state *plane, unsigned int index)
{
    assert(fb);
    return 0x20000000;
}
static int drm_crtc_vblank_get(struct drm_crtc *crtc) { return 0; }
static void drm_crtc_vblank_put(struct drm_crtc *crtc) { }
static unsigned int sent, cancelled;
static void *last_sent;
static struct drm_framebuffer *retired_before_event;
static void drm_crtc_send_vblank_event(struct drm_crtc *crtc, void *event)
{
    if (retired_before_event) assert(retired_before_event->references==1);
    sent++; last_sent=event;
}
static void dma_fence_set_error(struct dma_fence *fence, int error) { fence->error=error; }
static void dma_fence_signal(struct dma_fence *fence) { fence->signaled=true; }
static void complete_all(struct completion *completion) { completion->completed++; }
static void release_completion(struct completion *completion) { completion->released++; }
static void drm_event_cancel_free(struct drm_device *drm, struct drm_pending_event *event)
{ cancelled++; assert(!event->completion); }
static u32 readl(void *address) { return *(u32 *)address; }
static void writel(u32 value, void *address)
{
    if ((uintptr_t)address >= (uintptr_t)rdma &&
        (uintptr_t)address < (uintptr_t)(rdma + 64)) {
        *(u32 *)address = value;
        if (address == &rdma[0x14]) rdma[0x15] &= ~(value & 0xff000000);
        return;
    }
    /* This function must never write a VDIN, VPP or arbitration register. */
    assert(address == &osd[4] || address == &osd[5] ||
           address == &osd[11] || address == &osd[29] ||
           (native_writes && (address == &osd[0] || address == &osd[3] ||
            ((uintptr_t)address >= (uintptr_t)&osd[12] &&
             (uintptr_t)address <= (uintptr_t)&osd[14]))));
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
struct meson_t7_video_kms {
    struct drm_framebuffer *frames[2];
    bool fail_enable;
    unsigned int configured, enabled;
};
#define MESON_T7_VIDEO_KMS_PAIRS 320
static int meson_t7_video_collect(struct meson_t7_video_kms *video,
    struct drm_atomic_commit *commit, bool blank, struct drm_framebuffer **frames)
{
    if (video) for (unsigned int i=0;i<2;i++) frames[i]=blank?NULL:video->frames[i];
    return 0;
}
static bool meson_t7_video_in_use(struct meson_t7_video_kms *video,
    struct drm_atomic_commit *commit)
{ return video != NULL; }
static int meson_t7_video_flip(struct meson_t7_video_kms *video,
    struct drm_atomic_commit *commit, bool blank, u32 *table, unsigned int pairs)
{ return 0; }
static int meson_t7_video_build(struct meson_t7_video_kms *video,
    struct drm_atomic_commit *commit, bool blank, bool enable, const void *osd,
    u32 *table, unsigned int pairs)
{
    assert(video && pairs>=MESON_T7_VIDEO_KMS_PAIRS);
    if (enable) {
        assert(video->configured>video->enabled);
        video->enabled++;
        for (unsigned int i=0;i<2;i++)
            if (video->frames[i]) assert(video->frames[i]->references>=2);
        if (video->fail_enable) inject_timeout=true;
    } else video->configured++;
    table[0]=0x4800; table[1]=enable && video->frames[0] && !blank;
    table[2]=0x4880; table[3]=enable && video->frames[1] && !blank;
    return 16;
}
#define wait_event_timeout(wait, condition, timeout) ({ \
    bool missed=inject_timeout || (fail_drain && display->managed_sram && !rdma[0x23]); \
    assert((timeout) == 100); \
    assert(!(condition)); \
    if (display->managed_sram && !rdma[0x23]) { \
        if (!missed) meson_t7_handoff_irq(0, display); \
    } else if (display->rdma) { \
        assert(!meson_t7_handoff_rdma_irq(0, display)); \
        if (!inject_timeout || timeout_part == 2) { \
            for (unsigned int i = 0; i < display->rdma_bytes / 4; i += 2) { \
                if (display->managed_sram) pipeline[table[i]]=table[i+1]; \
                else writel(table[i + 1], osd + table[i] - VIU_OSD1_CTRL_STAT); \
            } \
            rdma[0x15] = RDMA_IRQ_STAT_CHAN1; \
            if (inject_timeout) meson_t7_handoff_rdma_irq(0, display); \
            else { \
                if (rdma_first) meson_t7_handoff_rdma_irq(0, display); \
                else meson_t7_handoff_irq(0, display); \
                assert(!(condition)); \
                if (rdma_first) meson_t7_handoff_irq(0, display); \
                else meson_t7_handoff_rdma_irq(0, display); \
            } \
        } \
        if (inject_timeout && timeout_part == 1) meson_t7_handoff_irq(0, display); \
    } else { \
        meson_t7_handoff_irq(0, display); \
        assert(!(condition)); \
        if (!inject_timeout) meson_t7_handoff_irq(0, display); \
    } \
    assert((bool)(condition) == !missed); \
    !missed; \
})
'''
checks = r'''
int main(void)
{
    struct meson_t7_handoff display = { .osd = osd, .venc = venc };
    struct drm_framebuffer first = { .references = 1 };
    struct drm_framebuffer second = { .references = 1 }, third = { .references = 1 };
    struct drm_crtc_state running = { .active = true };
    struct drm_plane_state removed = { .visible = true };
    struct drm_atomic_commit rmfb = { .plane = &removed,
        .old_crtc = &running, .new_crtc = &running };

    assert(meson_t7_osd_alpha(0xa5a00fff, 0) == (0xa5a00fff & ~GENMASK(20,12)));
    assert((meson_t7_osd_alpha(0, 65535) >> 12) == 256);
    assert((meson_t7_osd_alpha(0, 32768) >> 12) == 128);
    assert(meson_t7_osd_reflect(0xffffffff, 0) == (0xffffffff & ~GENMASK(29,28)));
    assert(meson_t7_osd_reflect(0, DRM_MODE_REFLECT_X) == BIT(28));
    assert(meson_t7_osd_reflect(0, DRM_MODE_REFLECT_Y) == BIT(29));
    assert(meson_t7_osd_reflect(0, DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)
           == (BIT(28) | BIT(29)));

    display.plane.dev = display.crtc.dev = &display.drm;
    display.plane.state = &removed;
    display.crtc.state = &running;

    meson_t7_handoff_irq(0, &display);
    assert(!writes);
    meson_t7_handoff_scanout(&display, &first, 0x20000000, NULL);
    assert(writes == 1 && first.references == 2 && osd[4] == 0x02000000);
    meson_t7_handoff_scanout(&display, &second, 0x21000000, NULL);
    assert(writes == 2 && first.references == 1 && second.references == 2);
    /* RMFB/file close detaches fb and crtc from a copied, visible plane state. */
    assert(!meson_t7_handoff_plane_check(&display.plane, &rmfb));
    assert(!removed.visible);
    meson_t7_handoff_flush(&display.crtc, &rmfb);
    assert(writes == 3 && second.references == 1 && !display.buffers.active[0]);
    assert(osd[4] == HANDOFF_BOOT_ADDR >> 4);
    /* Counter wrap must not retire the buffer before two real interrupts. */
    display.requested = display.written = display.applied = UINT32_MAX;
    meson_t7_handoff_scanout(&display, &first, 0x20000000, NULL);
    assert(!display.applied && first.references == 2);
    inject_timeout = true;
    meson_t7_handoff_scanout(&display, &second, 0x21000000, NULL);
    assert(display.faulted && display.buffers.active[0] == &first &&
           display.buffers.pending[0] == &second);
    assert(first.references == 2 && second.references == 2);
    unsigned int stopped_at = writes;
    meson_t7_handoff_scanout(&display, &third, 0x22000000, NULL);
    assert(writes == stopped_at && third.references == 1);
    /* A delayed IRQ cannot make the failed path drop either DMA reference. */
    meson_t7_handoff_irq(0, &display);
    assert(first.references == 2 && second.references == 2);
    puts("OSD two-vsync retirement, active-CRTC RMFB, wrap and timeout pinning passed");

    /* M2 imports can carry a padded 32-bit pitch, but never a non-linear modifier. */
    const struct drm_format_info xrgb = {DRM_FORMAT_XRGB8888, {4}, 1};
    const struct drm_format_info xbgr = {DRM_FORMAT_XBGR8888, {4}, 1};
    struct drm_framebuffer gpu = {.references=1, .format=&xrgb,
        .width=1920, .height=1080, .pitches={8192}};
    struct drm_plane_state visible = {.fb=&gpu, .crtc=&display.crtc,
        .src_w=1920 << 16, .src_h=1080 << 16, .crtc_w=1920, .crtc_h=1080};
    rmfb.plane = &visible;
    display.faulted = false;
    display.buffers.faulted = false;
    assert(meson_t7_handoff_plane_check(&display.plane, &rmfb) == -EINVAL);
    display.linear_formats = true;
    assert(!meson_t7_handoff_plane_check(&display.plane, &rmfb));
    gpu.modifier = 1;
    assert(meson_t7_handoff_plane_check(&display.plane, &rmfb) == -EINVAL);
    gpu.modifier = 0; gpu.pitches[0] = 7681;
    assert(meson_t7_handoff_plane_check(&display.plane, &rmfb) == -EINVAL);
    gpu.pitches[0] = 7648;
    assert(meson_t7_handoff_plane_check(&display.plane, &rmfb) == -EINVAL);
    gpu.pitches[0] = 8192;
    display.buffers.active[0] = NULL; display.buffers.pending[0] = NULL;
    display.boot_format = 0x8700; display.boot_stride = 360; display.boot_ctrl2 = 0x12;
    inject_timeout = false;
    unsigned int before = writes;
    meson_t7_handoff_scanout(&display, &gpu, 0x22000000, NULL);
    assert(writes == before + 4 && osd[11] == 0x8504 && osd[5] == 512);
    assert(osd[29] == (0x12 | (1 << 14)) && gpu.references == 2);
    gpu.format = &xbgr;
    meson_t7_handoff_scanout(&display, &gpu, 0x23000000, NULL);
    assert(osd[11] == 0x8508 && gpu.references == 2);
    meson_t7_handoff_scanout(&display, NULL, HANDOFF_BOOT_ADDR, NULL);
    assert(osd[11] == 0x8700 && osd[5] == 360 && osd[29] == 0x12);
    assert(gpu.references == 1);
    puts("Linear OSD: RGB32 layout, padding/modifier rejection, vsync programming and boot restore passed");

    /* Same buffer can be queued again; each reference still retires exactly once. */
    display.rdma = rdma; display.rdma_table = table; display.rdma_dma = 0x100000;
    unsigned int start_vblank = vblanks;
    for (unsigned int i = 0; i < 60; i++) {
        rdma_first = i & 1;
        if (i == 30) display.requested = display.written = display.applied = UINT32_MAX;
        struct drm_framebuffer *next = i & 1 ? &gpu : &first;
        if (next == &first) first.format = &xrgb;
        meson_t7_handoff_scanout(&display, next, 0x22000000, NULL);
        assert(!display.faulted && display.buffers.active[0] == next);
        assert(!rdma[0x23] && !rdma[0x15]);
    }
    assert(vblanks - start_vblank == 60);
    meson_t7_handoff_scanout(&display, NULL, HANDOFF_BOOT_ADDR, NULL);
    assert(first.references == 2); /* The earlier M1 fault intentionally pins one. */
    assert(gpu.references == 1);
    for (timeout_part = 1; timeout_part <= 2; timeout_part++) {
        struct meson_t7_handoff failed = {.osd=osd, .rdma=rdma, .rdma_table=table};
        struct drm_framebuffer old = {.references=2}, next = {.references=1};
        failed.buffers.active[0] = &old;
        inject_timeout = true;
        meson_t7_handoff_scanout(&failed, &next, 0x22000000, NULL);
        assert(failed.faulted && old.references == 2 && next.references == 2);
        /* A late missing interrupt cannot release a failed transaction's buffers. */
        rdma[0x15] = RDMA_IRQ_STAT_CHAN1;
        meson_t7_handoff_rdma_irq(0, &failed);
        meson_t7_handoff_irq(0, &failed);
        assert(old.references == 2 && next.references == 2);
    }
    puts("RDMA: 60 flips/60 vsyncs, IRQ orders/wrap, detach, missing DMA/vsync pinning passed");

    struct dma_fence fence={0};
    struct completion completion={0};
    struct drm_pending_vblank_event event={.base={.fence=&fence,
        .completion=&completion, .completion_release=release_completion}};
    display.faulted=true; running.event=&event;
    unsigned int previously_sent=sent;
    meson_t7_handoff_flush(&display.crtc, &rmfb);
    assert(!running.event && cancelled == 1 && sent == previously_sent);
    assert(fence.error == -EIO && fence.signaled);
    assert(completion.completed == 1 && completion.released == 1);
    /* Legacy event without an explicit out-fence follows the same cancellation path. */
    event.base.fence=NULL; event.base.completion=NULL; running.event=&event;
    meson_t7_handoff_flush(&display.crtc, &rmfb);
    assert(!running.event && cancelled == 2 && sent == previously_sent);
    puts("Latch failure: no successful page-flip event, error fence and released commit dependency passed");

    native_writes=true; inject_timeout=false;
    display.native_modeset=true; display.faulted=false; display.buffers.active[0]=NULL;
    running.adjusted_mode.hdisplay=3840; running.adjusted_mode.vdisplay=2160;
    running.modeset=true; running.active=true;
    struct drm_framebuffer uhd={.references=1,.format=&xrgb,.width=3840,.height=2160,
                               .pitches={15360}};
    display.plane.state->fb=&uhd; display.plane.state->visible=true;
    display.plane.state->alpha=32768;
    display.plane.state->rotation=DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y;
    rmfb.plane=display.plane.state;
    previously_sent=sent; running.event=&event;
    meson_t7_handoff_flush(&display.crtc,&rmfb);
    assert(sent==previously_sent && running.event==&event);
    meson_t7_handoff_enable(&display.crtc,&rmfb);
    assert(sent==previously_sent+1 && !running.event && uhd.references==2 && (osd[0]&1));
    assert(((osd[0] >> 12) & 0x1ff) == 128);
    assert((osd[11] & GENMASK(29,28)) == GENMASK(29,28));
    assert(osd[VIU_OSD1_BLK0_CFG_W1 - VIU_OSD1_CTRL_STAT]==3839U<<16);
    assert(osd[VIU_OSD1_BLK0_CFG_W2 - VIU_OSD1_CTRL_STAT]==2159U<<16);
    running.active=false;
    meson_t7_handoff_disable(&display.crtc,&rmfb);
    assert(timing_stops==1 && uhd.references==1 && !display.buffers.active[0] && !(osd[0]&1));
    running.active=true; running.event=&event; fail_modeset=true;
    previously_sent=sent;
    meson_t7_handoff_enable(&display.crtc,&rmfb);
    assert(display.faulted && !running.event && sent==previously_sent && uhd.references==1);
    puts("Native atomic modeset: deferred initial flip, 4K geometry, blank/drain/stop and PLL failure passed");

    struct meson_t7_handoff multi={.osd=osd,.rdma=rdma,.rdma_table=table,.paths=paths,
        .native_modeset=true,.managed_sram=true,.linear_formats=true};
    struct drm_crtc_state output={.active=true,.adjusted_mode={1920,1080}};
    struct drm_atomic_commit multi_commit={.old_crtc=&output,.new_crtc=&output};
    multi.crtc.dev=&multi.drm; multi.crtc.state=&output;
    struct drm_framebuffer desktop_a={.references=1,.format=&xrgb,
        .width=1920,.height=1080,.pitches={7680}}, desktop_b=desktop_a;
    struct drm_framebuffer overlay=desktop_a, cursor=desktop_a;
    struct drm_plane_state states[3];
    for (unsigned int i=0;i<3;i++) {
        states[i]=(struct drm_plane_state){.fb=i==0 ? &desktop_a :
            i==1 ? &overlay : &cursor,.crtc=&multi.crtc,
            .src_w=1920<<16,.src_h=1080<<16,.crtc_w=1920,.crtc_h=1080,
            .alpha=65535,.normalized_zpos=i};
        struct drm_plane *p=i ? &multi.extra_planes[i-1] : &multi.plane;
        p->dev=&multi.drm; p->state=&states[i]; rmfb.plane=&states[i];
        multi_commit.planes[i]=p; multi_commit.states[i]=&states[i];
        rmfb.new_crtc=&output;
        assert(!meson_t7_handoff_plane_check(p,&rmfb));
    }
    /* An imported object shorter than its final padded row never reaches DMA. */
    struct drm_gem_dma_object short_import={.dma_addr=0x21000000,
        .base.size=7680*1080-1};
    cursor.object=&short_import;
    unsigned int writes_before_check=writes;
    assert(meson_t7_handoff_plane_check(&multi.extra_planes[1],&rmfb)==-ERANGE);
    assert(writes==writes_before_check && cursor.references==1);
    short_import.base.size++;
    assert(!meson_t7_handoff_plane_check(&multi.extra_planes[1],&rmfb));
    cursor.object=NULL;
    fail_modeset=false; inject_timeout=false;
    meson_t7_handoff_scanout_layers(&multi,&multi_commit,false);
    unsigned int frame_start=vblanks;
    for (unsigned int frame=0;frame<60;frame++) {
        states[0].fb=frame&1 ? &desktop_a : &desktop_b;
        retired_before_event=frame&1 ? &desktop_b : &desktop_a;
        output.event=&event; previously_sent=sent;
        meson_t7_handoff_flush(&multi.crtc,&multi_commit);
        assert(sent==previously_sent+1 && !output.event);
        assert(multi.rdma_bytes==24);
        assert(!multi.faulted && multi.buffers.active[0]==states[0].fb);
        assert(multi.buffers.active[1]==&overlay && multi.buffers.active[2]==&cursor);
        assert(overlay.references==2 && cursor.references==2);
    }
    assert(vblanks-frame_start==60);
    /* Geometry/blend changes wait an extra boundary before exposing reuse. */
    states[0].fb=&desktop_b; states[1].alpha=32768;
    retired_before_event=&desktop_a; output.event=&event;
    frame_start=vblanks;
    meson_t7_handoff_flush(&multi.crtc,&multi_commit);
    assert(vblanks-frame_start==2 && !output.event && multi.rdma_bytes>40);
    retired_before_event=NULL;
    /* RMFB's stale visible bit must not pass a NULL FB into GEM or the builder. */
    states[2].fb=NULL; states[2].crtc=NULL; states[2].visible=true;
    rmfb.plane=&states[2];
    assert(!meson_t7_handoff_plane_check(&multi.extra_planes[1],&rmfb));
    assert(!states[2].visible);
    meson_t7_handoff_scanout_layers(&multi,&multi_commit,false);
    assert(cursor.references==1 && !multi.buffers.active[2]);
    meson_t7_handoff_scanout_layers(&multi,&multi_commit,true);
    assert(cursor.references==1 && overlay.references==1);
    assert(!(pipeline[0x1a10]&1) && !(pipeline[0x1a30]&1) && !(pipeline[0x3d80]&1));
    meson_t7_handoff_drain_layers(&multi);
    assert(desktop_a.references==1 && desktop_b.references==1 && overlay.references==1);
    for (unsigned int i=0;i<3;i++)
        assert(!multi.buffers.active[i] && !multi.buffers.retiring[i]);

    /* A later nonblocking commit may have swapped every global state already. */
    struct drm_framebuffer future_fb=desktop_a;
    struct drm_plane_state future_plane=states[0]; future_plane.fb=&future_fb;
    struct drm_pending_vblank_event future_event={0};
    struct drm_crtc_state future_crtc={.active=false,.event=&future_event};
    multi.plane.state=&future_plane;
    multi.extra_planes[0].state=multi.extra_planes[1].state=&future_plane;
    multi.crtc.state=&future_crtc;
    output.event=&event; states[0].fb=&desktop_a;
    previously_sent=sent;
    meson_t7_handoff_flush(&multi.crtc,&multi_commit);
    assert(sent==previously_sent+1 && last_sent==&event && !output.event);
    assert(future_crtc.event==&future_event && future_fb.references==1);
    assert(multi.buffers.active[0]==&desktop_a && multi.buffers.active[1]==&overlay);
    assert(!multi.buffers.active[2]);
    puts("Queued atomic states: scanout and event belong to the submitted commit, not global state");

    states[0].fb=&desktop_a;
    meson_t7_handoff_scanout_layers(&multi,&multi_commit,false);
    states[0].fb=&desktop_b;
    meson_t7_handoff_scanout_layers(&multi,&multi_commit,false);
    /* Missing the post-latch boundary must not send an event or release old DMA. */
    states[0].fb=&desktop_a;
    fail_drain=true; output.active=false; output.event=&event; previously_sent=sent;
    struct dma_fence drain_fence={0}; event.base.fence=&drain_fence;
    meson_t7_handoff_disable(&multi.crtc,&multi_commit);
    assert(multi.faulted && multi.buffers.faulted);
    assert(sent==previously_sent && !output.event && drain_fence.error==-EIO);
    assert(desktop_a.references==1 && desktop_b.references==2 && overlay.references==2);
    rdma[0x15]=RDMA_IRQ_STAT_CHAN1;
    meson_t7_handoff_rdma_irq(0,&multi); meson_t7_handoff_irq(0,&multi);
    meson_t7_handoff_scanout_layers(&multi,&multi_commit,false);
    assert(desktop_a.references==1 && desktop_b.references==2 && overlay.references==2);
    puts("Three OSD engines: 60 safe events/60 vsyncs, long-update event delay, RMFB and drain fault pinning passed");

    struct meson_t7_handoff five={.osd=osd,.rdma=rdma,.rdma_table=table,.paths=paths,
        .native_modeset=true,.managed_sram=true,.linear_formats=true};
    five.crtc.dev=&five.drm;
    struct drm_framebuffer va={.references=1}, vb={.references=1};
    struct drm_framebuffer vc={.references=1}, vd={.references=1};
    struct meson_t7_video_kms video={.frames={&va,&vb}};
    five.video=&video;
    for (unsigned int i=0;i<3;i++) {
        struct drm_plane *p=i?&five.extra_planes[i-1]:&five.plane;
        p->dev=&five.drm;
        states[i].fb=NULL; states[i].visible=false;
        multi_commit.planes[i]=p;
    }
    fail_drain=false; inject_timeout=false; output.active=true; output.event=&event;
    frame_start=vblanks; previously_sent=sent;
    meson_t7_handoff_flush(&five.crtc,&multi_commit);
    assert(vblanks-frame_start==4 && sent==previously_sent+1);
    assert(video.configured==1 && video.enabled==1);
    assert(va.references==2 && vb.references==2 && (pipeline[0x4800]&1));
    video.frames[0]=&vc; video.frames[1]=&vd;
    output.event=&event;
    meson_t7_handoff_flush(&five.crtc,&multi_commit);
    assert(va.references==1 && vb.references==1 && vc.references==2 && vd.references==2);
    video.frames[0]=video.frames[1]=NULL;
    meson_t7_handoff_scanout_layers(&five,&multi_commit,true);
    assert(vc.references==1 && vd.references==1 && !(pipeline[0x4800]&1));
    video.frames[0]=&va; video.frames[1]=&vb; video.fail_enable=true;
    output.event=&event; previously_sent=sent;
    meson_t7_handoff_flush(&five.crtc,&multi_commit);
    assert(five.faulted && five.buffers.faulted && !output.event && sent==previously_sent);
    assert(va.references==3 && vb.references==3); /* Both enabling generations pinned. */
    rdma[0x15]=RDMA_IRQ_STAT_CHAN1;
    meson_t7_handoff_rdma_irq(0,&five); meson_t7_handoff_irq(0,&five);
    assert(va.references==3 && vb.references==3);
    puts("Video retirement: two-phase latch/drain, two video references, RMFB and enable failure pinning passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'handoff.c'
    executable = Path(directory) / 'handoff'
    code.write_text(prefix + registers + '\n' + defines + '\n' +
                    types + '\nstatic struct meson_t7_handoff *to_handoff(struct drm_device *drm) '
                    '{ return (struct meson_t7_handoff *)drm; }\n' +
                    layers_source[layers_source.index('/* Vendor meson_osd_scaler.c:'):] +
                    functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-I', str(Path(__file__).parent),
                    str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
