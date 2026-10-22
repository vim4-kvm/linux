#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual driver functions against deterministic failing backends.

No hardware, kernel module loading, duplicated stop implementation or sleeps.
The backends inject every MMIO failure, stuck status and ownership transition.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def function(path, name):
    source = path.read_text()
    # Function signatures used here fit on one line.
    start = source.rfind("\n", 0, source.index(name + "(")) + 1
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def run_c(source):
    with tempfile.TemporaryDirectory(prefix="t7-vdec-test-") as directory:
        path = Path(directory) / "test.c"
        path.write_text(source)
        output = Path(directory) / "test"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror",
                        "-Wno-unused-function", "-I", str(HERE),
                        str(path), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True, timeout=5)


COMMON = """
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#define BIT(n) (1U << (n))
typedef uint32_t u32;
"""


class LifetimeTests(unittest.TestCase):
    def test_canvas_allocation_failure_preserves_unallocated_sentinels(self):
        source = (HERE / "h264.c").read_text()
        start = source.index("struct h264_decode_buf_spec {")
        spec = source[start:source.index("};", start) + 2]
        allocate = function(HERE / "h264.c", "config_decode_canvas")
        release = function(HERE / "h264.c", "release_canvas_index")
        run_c(COMMON + """
#include "reg_defines.h"
typedef uint8_t u8;
typedef uintptr_t dma_addr_t;
#define DOS_BUS 0
#define MESON_CANVAS_WRAP_NONE 0
#define MESON_CANVAS_BLKMODE_LINEAR 0
#define MESON_CANVAS_ENDIAN_SWAP64 0
#define dev_dbg(...) ((void)0)
#define spec2canvas(spec) 0
struct aml_vdec_hw { void *canvas, *dev, *map[1]; };
static int allocations, fail_at, frees;
static int meson_canvas_alloc(void *canvas, u8 *index)
{
    if (++allocations == fail_at) return -ENOSPC;
    *index = allocations + 10; return 0;
}
static void meson_canvas_free(void *canvas, int index)
{ assert(index == 11 || index == 12); frees++; }
#define meson_canvas_config(...) ((void)0)
static int regmap_write(void *map, int reg, unsigned int value) { return 0; }
""" + spec + allocate + release + """
int main(void)
{
    struct aml_vdec_hw hw = {0};
    for (int fail = 1; fail <= 3; fail++) {
        struct h264_decode_buf_spec spec = {
            .y_canvas_index = -1, .u_canvas_index = -1, .v_canvas_index = -1
        };
        allocations = frees = 0; fail_at = fail;
        int result = config_decode_canvas(&hw, &spec, 20, 16);
        assert(result == (fail <= 2 ? -ENOSPC : 0));
        release_canvas_index(&hw, &spec);
        assert(spec.y_canvas_index == -1 && spec.u_canvas_index == -1);
        assert(frees == (fail == 1 ? 0 : fail == 2 ? 1 : 2));
        release_canvas_index(&hw, &spec);
        assert(frees == (fail == 1 ? 0 : fail == 2 ? 1 : 2));
    }
    return 0;
}
""")

    def test_platform_core_respects_explicit_domain_ownership(self):
        root = HERE.parents[4]
        probe = function(root / "drivers/base/platform.c", "platform_probe")
        run_c(COMMON + """
#define EPROBE_DEFER 517
#define PD_FLAG_ATTACH_POWER_ON 1
#define PD_FLAG_DETACH_POWER_OFF 2
#define unlikely(value) (value)
#define dev_warn(...) ((void)0)
struct platform_device { int unused; };
struct platform_driver {
    int (*probe)(struct platform_device *);
    bool driver_managed_pm, prevent_deferred_probe;
};
struct device { struct platform_driver *driver; void *of_node; };
static struct platform_device pdev;
#define to_platform_driver(driver) (driver)
#define to_platform_device(dev) (&pdev)
static int domain_calls, probe_calls, attach_error;
static int platform_probe_fail(struct platform_device *dev) { return 0; }
static int of_clk_set_defaults(void *node, bool force) { return 0; }
static int dev_pm_domain_attach(struct device *dev, int flags)
{ domain_calls++; return attach_error; }
static int driver_probe(struct platform_device *dev) { probe_calls++; return 0; }
""" + probe + """
int main(void)
{
    struct platform_driver drv = { .probe = driver_probe, .driver_managed_pm = true };
    struct device dev = { .driver = &drv };
    assert(!platform_probe(&dev) && !domain_calls && probe_calls == 1);
    drv.driver_managed_pm = false;
    assert(!platform_probe(&dev) && domain_calls == 1 && probe_calls == 2);
    attach_error = -EPROBE_DEFER;
    assert(platform_probe(&dev) == -EPROBE_DEFER && probe_calls == 2);
    return 0;
}
""")

    def test_stop_mmio_errors_and_busy_status(self):
        stop = function(HERE / "aml_vdec_hw.c", "aml_stop_vdec_hw")
        run_c(COMMON + """
#include "reg_defines.h"
#define DOS_BUS 0
struct aml_vdec_hw { void *map[1]; int dec_irq; bool has_dbus_ctrl; };
static int transactions, fail_at, busy_reg, last_write, relax_count;
static int regmap_write(void *map, int reg, unsigned int value)
{
    if (++transactions == fail_at) return -EIO;
    last_write = reg;
    return 0;
}
static int regmap_read(void *map, int reg, unsigned int *value)
{
    if (++transactions == fail_at) return -EIO;
    *value = reg == VDEC_ASSIST_DBUS_DISABLE ? 0xffff : 0;
    if (reg == busy_reg)
        *value = reg == VDEC_ASSIST_DBUS_DISABLE ? 0 : 0xffff;
    return 0;
}
#define regmap_read_poll_timeout(map, reg, value, condition, delay, limit) \
    ({ int err = regmap_read(map, reg, &(value)); \
       if (!err && !(condition)) err = -ETIMEDOUT; err; })
static void synchronize_irq(int irq) {}
static void cpu_relax(void) { relax_count++; }
""" + stop + """
int main(void)
{
    struct aml_vdec_hw hw = { .has_dbus_ctrl = true };
    busy_reg = -1;
    assert(aml_stop_vdec_hw(&hw) == 0);
    assert(last_write == VDEC_ASSIST_DBUS_DISABLE && relax_count == 200);
    int total = transactions;
    for (int n = 1; n <= total; n++) {
        transactions = 0; fail_at = n;
        assert(aml_stop_vdec_hw(&hw) == -EIO);
        assert(transactions == n); /* No MMIO after a failed transaction. */
    }
    const int busy[] = { IMEM_DMA_CTRL, LMEM_DMA_CTRL, WRRSP_LMEM,
                         VDEC_ASSIST_DBUS_DISABLE };
    fail_at = 0;
    for (unsigned int n = 0; n < sizeof(busy) / sizeof(busy[0]); n++) {
        transactions = 0; busy_reg = busy[n];
        assert(aml_stop_vdec_hw(&hw) == -ETIMEDOUT);
    }
    busy_reg = -1; hw.has_dbus_ctrl = false;
    assert(aml_stop_vdec_hw(&hw) == 0);
    assert(last_write == DOS_SW_RESET0);
    return 0;
}
""")

    def test_finish_only_after_confirmed_stop_and_no_retry(self):
        stop_job = function(HERE / "aml_vdec.c", "aml_vdec_stop_job")
        run_c(COMMON + """
struct aml_vdec_hw { bool faulted; };
struct aml_codec_ops { void (*finish)(void *); };
struct dev { struct aml_vdec_hw *dec_hw; };
struct aml_vdec_ctx { struct dev *dev; bool dos_clk_en;
                      struct aml_codec_ops *codec_ops; };
static int stop_result, stops, finishes;
static int aml_stop_vdec_hw(struct aml_vdec_hw *hw)
{ stops++; return stop_result; }
static void aml_vdec_fault(struct aml_vdec_ctx *ctx)
{ ctx->dev->dec_hw->faulted = true; }
static void finish(void *ctx) { finishes++; }
""" + stop_job + """
int main(void)
{
    struct aml_vdec_hw hw = {0};
    struct dev dev = { &hw };
    struct aml_codec_ops ops = { finish };
    struct aml_vdec_ctx ctx = { &dev, true, &ops };
    stop_result = -ETIMEDOUT;
    aml_vdec_stop_job(&ctx);
    assert(hw.faulted && !finishes && stops == 1 && ctx.dos_clk_en);
    aml_vdec_stop_job(&ctx);
    assert(stops == 1);
    hw.faulted = false; stop_result = 0;
    aml_vdec_stop_job(&ctx);
    assert(stops == 2 && finishes == 1 && !ctx.dos_clk_en);
    aml_vdec_stop_job(&ctx);
    assert(stops == 2 && finishes == 1);
    return 0;
}
""")

    def test_vb2_close_keeps_faulted_plane_references(self):
        cleanup = function(HERE / "aml_vdec.c", "vb2_ops_vdec_buf_cleanup")
        run_c(COMMON + """
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
struct dma_buf { int puts; };
struct hw { bool faulted; };
struct dev { struct hw *dec_hw; };
struct aml_vdec_ctx { struct dev *dev; };
struct queue { struct aml_vdec_ctx *ctx; };
struct vb2_buffer { unsigned int num_planes; struct queue *vb2_queue; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct m2m { struct vb2_v4l2_buffer vb; };
struct aml_vdec_buffer { struct m2m m2m; struct dma_buf *pins[2]; };
#define to_vb2_v4l2_buffer(vb) container_of(vb, struct vb2_v4l2_buffer, vb2_buf)
#define vb2_get_drv_priv(q) ((q)->ctx)
static void dma_buf_put(struct dma_buf *buf) { buf->puts++; }
""" + cleanup + """
int main(void)
{
    struct hw hw = { .faulted = true };
    struct dev dev = { &hw };
    struct aml_vdec_ctx ctx = { &dev };
    struct queue q = { &ctx };
    struct dma_buf y = {0}, uv = {0};
    struct aml_vdec_buffer buf = {
        .m2m.vb.vb2_buf = { 2, &q }, .pins = { &y, &uv },
    };
    vb2_ops_vdec_buf_cleanup(&buf.m2m.vb.vb2_buf);
    assert(!y.puts && !uv.puts);
    hw.faulted = false;
    vb2_ops_vdec_buf_cleanup(&buf.m2m.vb.vb2_buf);
    assert(y.puts == 1 && uv.puts == 1);
    return 0;
}
""")

    def test_dos_lease_and_permanent_quarantine(self):
        source = HERE.parent / "t7-dos.c"
        code = "\n".join(function(source, name) for name in
                         ("meson_t7_dos_get", "meson_t7_dos_put",
                          "meson_t7_dos_poison"))
        run_c(COMMON + """
struct device { int unused; };
static int dos_lock, encoders, module_refs;
static bool decoder, poisoned;
#define mutex_lock(lock) ((void)(lock))
#define mutex_unlock(lock) ((void)(lock))
#define dev_err(...) ((void)0)
#define THIS_MODULE 0
#define __module_get(module) (module_refs++)
""" + code + """
int main(void)
{
    assert(!meson_t7_dos_get(0, false));
    assert(!meson_t7_dos_get(0, false));
    assert(meson_t7_dos_get(0, true) == -EBUSY);
    meson_t7_dos_put(false);
    assert(meson_t7_dos_get(0, true) == -EBUSY);
    meson_t7_dos_put(false);
    assert(!meson_t7_dos_get(0, true));
    assert(meson_t7_dos_get(0, false) == -EBUSY);
    meson_t7_dos_put(true);
    meson_t7_dos_poison(); meson_t7_dos_poison();
    assert(module_refs == 1);
    assert(meson_t7_dos_get(0, false) == -EIO);
    assert(meson_t7_dos_get(0, true) == -EIO);
    return 0;
}
""")


if __name__ == "__main__":
    unittest.main()
